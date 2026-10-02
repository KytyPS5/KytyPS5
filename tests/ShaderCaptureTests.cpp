// Round-trips shader captures through the writer the emulator uses (--shader-capture-dir) and the
// loader behind --shader-replay. With an output directory argument it also leaves a replayable
// capture behind, which the `shader_capture_replay` ctest compiles with the real recompiler.
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderCapture.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace Libs::Graphics;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "ShaderCaptureTests:%d: %s\n", __LINE__, #condition);             \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

void SetCaptureDir(const std::filesystem::path& dir) {
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	options.shader_capture_dir = dir;
	Config::Load(options);
}

ShaderComputeInputInfo MakeCompute() {
	ShaderComputeInputInfo info;
	info.threads_num[0]           = 64;
	info.threads_num[1]           = 2;
	info.threads_num[2]           = 1;
	info.dispatch_threads_num[1]  = 5;
	info.lds_size_dwords          = 256;
	info.scratch_size_dwords      = 12;
	info.host_subgroup_size       = 32;
	info.wave_size                = 64;
	info.float_mode               = 0xc3;
	info.group_id[0]              = true;
	info.group_id[2]              = true;
	info.dispatch_thread_dimensions = true;
	info.thread_ids_num           = 2;
	info.workgroup_register       = 4;
	info.tg_size_en               = true;
	return info;
}

// The shader from tests/data/shader_capture/store_ok.s: v_mov_b32, buffer_store_dword through the
// V# in the four user SGPRs, s_endpgm.
constexpr std::array<uint32_t, 4> STORE_OK_CODE = {0x7e0202f2, 0xe0701000, 0x80000100, 0xbf810000};
constexpr std::array<uint32_t, 4> STORE_OK_USER = {0x10000000, 0x0, 0x100, 0x30000000};

void TestComputeRoundTrip(const std::filesystem::path& root) {
	SetCaptureDir(root);
	const std::array<uint32_t, 3> state   = {4, 0xc340, 64};
	const auto                    compute = MakeCompute();

	std::filesystem::path dir;
	{
		const ShaderCaptureSource source {.stage          = ShaderType::Compute,
		                                  .hash           = 0x6a53456e7ef5d1b0ull,
		                                  .wave_size      = 64,
		                                  .user_data_base = 0,
		                                  .shader_base    = 0x7f0000001000ull,
		                                  .code           = STORE_OK_CODE,
		                                  .user_data      = STORE_OK_USER,
		                                  .static_state   = state};
		ShaderCapture capture(source, compute);
		dir = capture.Directory();
		CHECK(!dir.empty());
		CHECK(dir.filename().string().starts_with("cs_6a53456e7ef5d1b0_"));

		const std::array<uint32_t, 3> table = {7, 8, 9};
		ShaderCapture::RecordRead(0x2000, table, true);
		const std::array<uint32_t, 2> failed = {0, 0};
		ShaderCapture::RecordRead(0x3000, failed, false);
		// Reads are flushed as they happen, so a capture still open (or from a process that
		// died) already has them on disk.
		ShaderCaptureData partial;
		std::string       error;
		CHECK(LoadShaderCapture(dir, partial, error));
		CHECK(partial.reads.size() == 2);
	}
	// Reads after the capture ends belong to nobody.
	const std::array<uint32_t, 1> late = {1};
	ShaderCapture::RecordRead(0x4000, late, true);

	ShaderCaptureData data;
	std::string       error;
	CHECK(LoadShaderCapture(dir, data, error));
	CHECK(data.stage == ShaderType::Compute);
	CHECK(data.hash == 0x6a53456e7ef5d1b0ull);
	CHECK(data.shader_base == 0x7f0000001000ull);
	CHECK(data.wave_size == 64 && data.user_data_base == 0);
	CHECK(std::equal(STORE_OK_CODE.begin(), STORE_OK_CODE.end(), data.code.begin(), data.code.end()));
	CHECK(std::equal(STORE_OK_USER.begin(), STORE_OK_USER.end(), data.user_data.begin(),
	                 data.user_data.end()));
	CHECK(std::equal(state.begin(), state.end(), data.static_state.begin(), data.static_state.end()));
	CHECK(!data.raw_input);
	CHECK(data.compute.workgroup_register == 4 && data.compute.thread_ids_num == 2);
	CHECK(data.compute.float_mode == 0xc3 && data.compute.wave_size == 64);
	CHECK(data.compute.host_subgroup_size == 32 && data.compute.lds_size_dwords == 256);
	CHECK(data.compute.scratch_size_dwords == 12 && data.compute.tg_size_en);
	CHECK(data.compute.dispatch_thread_dimensions && data.compute.dispatch_threads_num[1] == 5);
	CHECK(data.compute.threads_num[0] == 64 && data.compute.threads_num[1] == 2);
	CHECK(data.compute.group_id[0] && !data.compute.group_id[1] && data.compute.group_id[2]);
	CHECK(data.reads.size() == 2);
	CHECK(data.reads[0].address == 0x2000 && data.reads[0].ok);
	CHECK((data.reads[0].values == std::vector<uint32_t> {7, 8, 9}));
	CHECK(data.reads[1].address == 0x3000 && !data.reads[1].ok && data.reads[1].values.size() == 2);
	CHECK(data.key == ShaderCaptureKey(ShaderType::Compute, data.hash, 4, 4, state));

	// A killed process can leave half a record; everything before it must still load.
	{
		std::ofstream out(dir / "reads.bin", std::ios::app | std::ios::binary);
		const char    junk[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
		out.write(junk, sizeof(junk));
	}
	ShaderCaptureData truncated;
	CHECK(LoadShaderCapture(dir, truncated, error));
	CHECK(truncated.reads.size() == 2);

	// Missing or corrupt input is an error, never a crash.
	ShaderCaptureData broken;
	CHECK(!LoadShaderCapture(root / "does_not_exist", broken, error));
	CHECK(!error.empty());
	{
		std::ofstream out(dir / "manifest.json", std::ios::binary | std::ios::trunc);
		out << "{ not json";
	}
	error.clear();
	CHECK(!LoadShaderCapture(dir, broken, error));
	CHECK(!error.empty());
}

// Writes a capture of the store_ok shader with ordinary compute state. The shader_capture_replay
// ctest hands it to --shader-replay, so a writer/loader mismatch fails there as well.
void WriteReplayFixture(const std::filesystem::path& root, const std::filesystem::path& keep) {
	SetCaptureDir(root);
	ShaderComputeInputInfo compute;
	compute.threads_num[0]     = 64;
	compute.threads_num[1]     = 1;
	compute.threads_num[2]     = 1;
	compute.group_id[0]        = true;
	compute.thread_ids_num     = 1;
	compute.workgroup_register = 4;
	// Replay never reads the static state; it only identifies the program variant.
	const std::array<uint32_t, 3> state = {4, 0xc040, 64};
	const ShaderCaptureSource source {.stage        = ShaderType::Compute,
	                                  .hash         = 0x1234567890abcdefull,
	                                  .wave_size    = 64,
	                                  .code         = STORE_OK_CODE,
	                                  .user_data    = STORE_OK_USER,
	                                  .static_state = state};
	std::filesystem::path dir;
	{
		ShaderCapture capture(source, compute);
		dir = capture.Directory();
	}
	CHECK(!dir.empty());
	std::filesystem::remove_all(keep);
	std::filesystem::create_directories(keep.parent_path());
	std::filesystem::copy(dir, keep, std::filesystem::copy_options::recursive);
}

void TestRawInputRoundTrip(const std::filesystem::path& root) {
	SetCaptureDir(root);
	const std::array<uint32_t, 2> code = {0xbf810000, 0xbf810000};

	ShaderPixelInputInfo pixel;
	pixel.input_num                = 3;
	pixel.interpolator_settings[1] = 7;
	pixel.ps_pos_x                 = true;
	pixel.ps_early_z               = true;
	pixel.wave_size                = 32;
	pixel.stage.program   = reinterpret_cast<const ShaderRecompiler::IR::CompiledShaderInfo*>(0x1234);
	pixel.stage.resources = reinterpret_cast<const ShaderRecompiler::IR::ResourceSnapshot*>(0x5678);
	std::filesystem::path pixel_dir;
	{
		const ShaderCaptureSource source {.stage = ShaderType::Pixel,
		                                  .hash  = 0x11,
		                                  .wave_size = 32,
		                                  .code  = code};
		ShaderCapture capture(source, pixel);
		pixel_dir = capture.Directory();
	}
	ShaderCaptureData data;
	std::string       error;
	CHECK(LoadShaderCapture(pixel_dir, data, error));
	CHECK(data.raw_input && data.stage == ShaderType::Pixel);
	CHECK(data.raw_input_size == sizeof(ShaderPixelInputInfo));
	ShaderPixelInputInfo loaded;
	std::memcpy(&loaded, data.raw_input_bytes.data(), sizeof(loaded));
	CHECK(loaded.input_num == 3 && loaded.interpolator_settings[1] == 7);
	CHECK(loaded.ps_pos_x && loaded.ps_early_z && loaded.wave_size == 32);
	// Pointers into the dying process must not leak into the capture.
	CHECK(loaded.stage.program == nullptr && loaded.stage.resources == nullptr);

	ShaderVertexInputInfo vertex;
	vertex.resources_num        = 2;
	vertex.resources[1].fields[2] = 0x40;
	vertex.scratch_size_dwords  = 9;
	vertex.logical_stage        = ShaderType::Vertex;
	std::filesystem::path vertex_dir;
	{
		const ShaderCaptureSource source {.stage = ShaderType::Vertex,
		                                  .hash  = 0x22,
		                                  .user_data_base = 8,
		                                  .code  = code,
		                                  .back_code = code};
		ShaderCapture capture(source, vertex);
		vertex_dir = capture.Directory();
	}
	ShaderCaptureData vertex_data;
	CHECK(LoadShaderCapture(vertex_dir, vertex_data, error));
	CHECK(vertex_data.raw_input_size == sizeof(ShaderVertexInputInfo));
	CHECK(vertex_data.user_data_base == 8);
	CHECK(vertex_data.back_code.size() == 2);
	ShaderVertexInputInfo vertex_loaded;
	std::memcpy(&vertex_loaded, vertex_data.raw_input_bytes.data(), sizeof(vertex_loaded));
	CHECK(vertex_loaded.resources_num == 2 && vertex_loaded.resources[1].fields[2] == 0x40);
	CHECK(vertex_loaded.scratch_size_dwords == 9);
}

void TestDisabled(const std::filesystem::path& root) {
	SetCaptureDir({});
	const std::array<uint32_t, 1> code = {0xbf810000};
	const ShaderCaptureSource source {.stage = ShaderType::Compute, .hash = 0x33, .code = code};
	ShaderComputeInputInfo compute;
	ShaderCapture          capture(source, compute);
	CHECK(capture.Directory().empty());
	const std::array<uint32_t, 1> read = {1};
	ShaderCapture::RecordRead(0x10, read, true); // must be a harmless no-op
	CHECK(!std::filesystem::exists(root / "cs_0000000000000033_00000000"));
}

} // namespace

int main(int argc, char* argv[]) {
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();

	const auto root = std::filesystem::temp_directory_path() /
	                  ("kyty_shader_capture_tests_" +
	                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
	const std::filesystem::path keep = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path();

	TestComputeRoundTrip(root / "compute");
	if (!keep.empty()) {
		WriteReplayFixture(root / "fixture", keep);
	}
	TestRawInputRoundTrip(root / "raw");
	TestDisabled(root / "disabled");

	std::filesystem::remove_all(root);
	std::puts("ShaderCapture tests passed");
	return 0;
}
