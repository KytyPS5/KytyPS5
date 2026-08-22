#include "gpu_tiler_shaders/gpu_tiler_depth_spv.h"
#include "graphics/shader/shaderDiskCache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <vector>

namespace {

using Libs::Graphics::ShaderDiskCacheEntry;
using Libs::Graphics::ShaderDiskCacheLoad;
using Libs::Graphics::ShaderDiskCacheSave;
using Libs::Graphics::ShaderLaneMaskMode;
using Libs::Graphics::ShaderType;

namespace IR = Libs::Graphics::ShaderRecompiler::IR;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderDiskCacheTests: failed: %s\n", text);
		std::abort();
	}
}

std::filesystem::path TempPath(const char* name) {
	auto path = std::filesystem::temp_directory_path() / name;
	std::error_code ec;
	std::filesystem::remove(path, ec);
	return path;
}

// Real SPIR-V, compiled by glslang during the build - not a magic number with
// noise after it. The cache never parses SPIR-V, but a test that feeds it
// something no compiler ever produced proves less than it looks like it does.
std::vector<uint32_t> RealSpirv() {
	return {std::begin(GPU_TILER_DEPTH_SPV), std::end(GPU_TILER_DEPTH_SPV)};
}

// A plan with every serialized container non-empty, so a field that is silently
// dropped shows up as a mismatch rather than as two empty vectors comparing
// equal.
std::shared_ptr<IR::Program> MakeProgram() {
	auto program = std::make_shared<IR::Program>();

	program->stage                      = ShaderType::Pixel;
	program->lane_mask_mode             = ShaderLaneMaskMode::PerInvocation;
	program->shader_hash                = 0x0123456789abcdefULL;
	program->wave_size                  = 32;
	program->user_data_base             = 4;
	program->user_data_count            = 16;
	program->srt_plan_complete          = true;
	program->resource_tracking_complete = true;
	program->shader_info_complete       = true;
	program->binding_layout_complete    = true;

	IR::BufferResource buffer {};
	buffer.source             = 3;
	buffer.first_use_pc       = 0x40;
	buffer.max_byte_extent    = 4096;
	buffer.packed_stride      = 16;
	buffer.descriptor_swizzle = 0x0fac;
	buffer.image_alias        = 7;
	buffer.read               = true;
	buffer.written            = true;
	buffer.formatted          = true;
	program->info.buffers.push_back(buffer);

	IR::AddressResource address {};
	address.source           = 9;
	address.first_use_pc     = 0x80;
	address.kind             = IR::ResourceKind::Flat;
	address.min_offset       = -32;
	address.specialized_base = 0xdeadbeefcafeULL;
	address.unbased          = false;
	address.written          = true;
	program->info.addresses.push_back(address);

	IR::ImageResource image {};
	image.source                    = 11;
	image.first_use_pc              = 0xc0;
	image.mip_count                 = 5;
	image.storage_swizzle           = IR::StorageImageIdentitySwizzle;
	image.read                      = true;
	image.cube                      = true;
	image.indirect_root             = 2;
	image.indirect_mapping_offset   = 8;
	image.indirect_mapping_capacity = 4;
	image.indirect_resources        = {1, 2, 3};
	program->info.images.push_back(image);

	program->info.samplers.push_back({13, 0x100});
	program->info.sampled_pairs.push_back({0, 0, 0x140});

	IR::StageInput input {};
	input.kind            = IR::StageInputKind::FragCoord;
	input.location        = 2;
	input.component_count = 4;
	input.debug_name      = "frag_coord";
	program->info.inputs.push_back(input);

	IR::StageOutput output {};
	output.kind       = IR::StageOutputKind::Mrt;
	output.index      = 1;
	output.location   = 1;
	output.debug_name = "mrt1";
	program->info.outputs.push_back(output);

	program->info.vertex_offset_sgpr = 6;
	program->info.has_bitwise_xor    = true;

	program->bindings.descriptor_set       = 1;
	program->bindings.push_constant_offset  = 16;
	program->bindings.push_constant_size    = 32;
	program->bindings.buffer_offset_dword   = 2;
	program->bindings.buffer_offset_count   = 3;
	program->bindings.user_data_registers   = {0, 1, 2, 3};
	program->bindings.descriptors.push_back(
	    {IR::DescriptorBindingKind::Buffers, 0, std::vector<uint32_t> {0}});
	program->bindings.descriptors.push_back(
	    {IR::DescriptorBindingKind::Samplers, 1, std::vector<uint32_t> {13}});

	IR::SpirvRequirements requirements {};
	requirements.requires_exact_subgroup = true;
	requirements.subgroup_ballot         = true;
	requirements.pixel_valid_mask        = true;
	program->spirv_requirements          = requirements;

	return program;
}

ShaderDiskCacheEntry MakeEntry() {
	ShaderDiskCacheEntry entry {};
	entry.stage             = ShaderType::Pixel;
	entry.lane_mask_mode    = ShaderLaneMaskMode::PerInvocation;
	entry.shader_hash       = 0x0123456789abcdefULL;
	entry.program_id.hash0  = 0xaabbccddU;
	entry.program_id.crc32  = 0x11223344U;
	entry.program_id.ids    = {5, 6, 7, 8};
	entry.optimization_type = Config::ShaderOptimizationType::Performance;
	entry.program           = MakeProgram();
	entry.spirv             = RealSpirv();
	return entry;
}

void ExpectSameEntry(const ShaderDiskCacheEntry& a, const ShaderDiskCacheEntry& b) {
	Check(a.stage == b.stage, "stage survives");
	Check(a.lane_mask_mode == b.lane_mask_mode, "lane mask mode survives");
	Check(a.shader_hash == b.shader_hash, "shader hash survives");
	Check(a.program_id == b.program_id, "program id survives");
	Check(a.optimization_type == b.optimization_type, "optimization type survives");
	Check(a.spirv == b.spirv, "SPIR-V survives byte for byte");

	Check(a.program != nullptr && b.program != nullptr, "both plans exist");
	Check(a.program->stage == b.program->stage, "plan stage survives");
	Check(a.program->lane_mask_mode == b.program->lane_mask_mode, "plan lane mask survives");
	Check(a.program->shader_hash == b.program->shader_hash, "plan hash survives");
	Check(a.program->wave_size == b.program->wave_size, "wave size survives");
	Check(a.program->user_data_base == b.program->user_data_base, "user data base survives");
	Check(a.program->user_data_count == b.program->user_data_count, "user data count survives");
	Check(a.program->srt_plan_complete == b.program->srt_plan_complete, "srt flag survives");
	Check(a.program->resource_tracking_complete == b.program->resource_tracking_complete,
	      "resource tracking flag survives");
	Check(a.program->shader_info_complete == b.program->shader_info_complete,
	      "shader info flag survives");
	Check(a.program->binding_layout_complete == b.program->binding_layout_complete,
	      "binding layout flag survives");

	// ShaderInfo and BindingLayout both have a defaulted operator==, so this
	// compares every field including ones added after this test was written.
	Check(a.program->info == b.program->info, "the whole ShaderInfo survives");
	Check(a.program->bindings == b.program->bindings, "the whole BindingLayout survives");
	Check(a.program->spirv_requirements.has_value() == b.program->spirv_requirements.has_value(),
	      "spirv requirements presence survives");
	if (a.program->spirv_requirements.has_value()) {
		const auto& x = *a.program->spirv_requirements;
		const auto& y = *b.program->spirv_requirements;
		Check(x.requires_exact_subgroup == y.requires_exact_subgroup &&
		          x.subgroup_ballot == y.subgroup_ballot &&
		          x.subgroup_shuffle == y.subgroup_shuffle &&
		          x.subgroup_local_invocation_id == y.subgroup_local_invocation_id &&
		          x.compute_derivatives == y.compute_derivatives &&
		          x.image_gather_extended == y.image_gather_extended &&
		          x.function_lds == y.function_lds && x.pixel_valid_mask == y.pixel_valid_mask,
		      "every spirv requirement survives");
	}
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	Check(static_cast<bool>(in), "cache file is readable");
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	Check(static_cast<bool>(out), "cache file is writable");
	out.write(reinterpret_cast<const char*>(bytes.data()),
	          static_cast<std::streamsize>(bytes.size()));
}

void TestColdRun() {
	const auto path = TempPath("kyty_shader_cache_cold.bin");
	Check(ShaderDiskCacheLoad(path).empty(), "a missing cache file is not an error");
}

void TestRoundTrip() {
	const auto path = TempPath("kyty_shader_cache_roundtrip.bin");

	const auto original = MakeEntry();
	Check(ShaderDiskCacheSave(path, {original}), "the cache is written");
	Check(std::filesystem::file_size(path) > 0, "the cache file is not empty");

	const auto loaded = ShaderDiskCacheLoad(path);
	Check(loaded.size() == 1, "exactly one entry comes back");
	ExpectSameEntry(original, loaded[0]);

	std::filesystem::remove(path);
}

void TestManyEntries() {
	const auto path = TempPath("kyty_shader_cache_many.bin");

	std::vector<ShaderDiskCacheEntry> entries;
	for (uint32_t i = 0; i < 32; i++) {
		auto entry        = MakeEntry();
		entry.shader_hash = 0x1000ULL + i;
		entries.push_back(std::move(entry));
	}
	Check(ShaderDiskCacheSave(path, entries), "many entries are written");

	const auto loaded = ShaderDiskCacheLoad(path);
	Check(loaded.size() == entries.size(), "every entry comes back");
	for (size_t i = 0; i < loaded.size(); i++) {
		Check(loaded[i].shader_hash == 0x1000ULL + i, "entries keep their order and identity");
	}

	std::filesystem::remove(path);
}

// A run killed while writing, or a file cut short by a full disk. The bytes that
// did land must still be usable, and nothing may read past the end.
void TestTruncationIsSurvivable() {
	const auto path = TempPath("kyty_shader_cache_truncated.bin");

	std::vector<ShaderDiskCacheEntry> entries;
	for (uint32_t i = 0; i < 8; i++) {
		auto entry        = MakeEntry();
		entry.shader_hash = 0x2000ULL + i;
		entries.push_back(std::move(entry));
	}
	Check(ShaderDiskCacheSave(path, entries), "the cache is written");

	const auto full = ReadFile(path);

	// Every truncation point, not just a convenient one.
	for (size_t cut = 0; cut < full.size(); cut += 37) {
		WriteFile(path, std::vector<uint8_t>(full.begin(), full.begin() + cut));
		const auto loaded = ShaderDiskCacheLoad(path);
		Check(loaded.size() <= entries.size(), "a truncated cache never invents entries");
		for (const auto& entry: loaded) {
			Check(entry.program != nullptr, "every surviving entry has a plan");
			Check(!entry.spirv.empty(), "every surviving entry has SPIR-V");
		}
	}

	std::filesystem::remove(path);
}

void TestCorruptEntryIsDropped() {
	const auto path = TempPath("kyty_shader_cache_corrupt.bin");

	std::vector<ShaderDiskCacheEntry> entries;
	for (uint32_t i = 0; i < 4; i++) {
		auto entry        = MakeEntry();
		entry.shader_hash = 0x3000ULL + i;
		entries.push_back(std::move(entry));
	}
	Check(ShaderDiskCacheSave(path, entries), "the cache is written");

	auto bytes = ReadFile(path);
	// Flip a byte deep inside the payload area, past the file header.
	bytes[bytes.size() / 2] ^= 0xFFU;
	WriteFile(path, bytes);

	const auto loaded = ShaderDiskCacheLoad(path);
	Check(loaded.size() < entries.size(), "the damaged entry is dropped");
	for (const auto& entry: loaded) {
		Check(entry.program != nullptr && !entry.spirv.empty(), "survivors are intact");
	}

	std::filesystem::remove(path);
}

// The cache must not be read by a build whose recompiler has a different shape.
// This is the guard that stops a stale plan turning into a wrong frame.
void TestForeignBuildIsRejected() {
	const auto path = TempPath("kyty_shader_cache_foreign.bin");

	Check(ShaderDiskCacheSave(path, {MakeEntry()}), "the cache is written");
	auto bytes = ReadFile(path);

	// The build id sits right after the 8-byte magic and the 4-byte format.
	bytes[12] ^= 0x01U;
	WriteFile(path, bytes);

	Check(ShaderDiskCacheLoad(path).empty(), "a cache from another build is not read");

	std::filesystem::remove(path);
}

void TestNonCacheFileIsRejected() {
	const auto path = TempPath("kyty_shader_cache_garbage.bin");

	std::mt19937                            rng(1234);
	std::uniform_int_distribution<uint32_t> byte(0, 255);

	// Random files, including ones long enough to look like a header.
	for (size_t size: {0U, 1U, 8U, 16U, 64U, 4096U}) {
		std::vector<uint8_t> bytes(size);
		for (auto& b: bytes) {
			b = static_cast<uint8_t>(byte(rng));
		}
		WriteFile(path, bytes);
		Check(ShaderDiskCacheLoad(path).empty(), "a file that is not a cache yields nothing");
	}

	std::filesystem::remove(path);
}

// A length field is the classic place to hide a huge allocation. Rewrite the
// entry sizes to absurd values and confirm the loader refuses rather than tries.
void TestAbsurdLengthsAreRefused() {
	const auto path = TempPath("kyty_shader_cache_lengths.bin");

	Check(ShaderDiskCacheSave(path, {MakeEntry()}), "the cache is written");
	auto bytes = ReadFile(path);

	// Entry size is the first 8 bytes after the 20-byte file header.
	constexpr size_t HEADER = 8 + 4 + 8 + 4;
	Check(bytes.size() > HEADER + 8, "the file is long enough to patch");
	for (uint32_t i = 0; i < 8; i++) {
		bytes[HEADER + i] = 0xFFU;
	}
	WriteFile(path, bytes);

	Check(ShaderDiskCacheLoad(path).empty(), "an impossible entry length is refused");

	std::filesystem::remove(path);
}

void TestEmptyEntriesAreNotWritten() {
	const auto path = TempPath("kyty_shader_cache_empty.bin");

	ShaderDiskCacheEntry no_program = MakeEntry();
	no_program.program.reset();

	ShaderDiskCacheEntry no_spirv = MakeEntry();
	no_spirv.spirv.clear();

	Check(ShaderDiskCacheSave(path, {no_program, no_spirv}), "the cache is written");
	Check(ShaderDiskCacheLoad(path).empty(), "entries with nothing to cache are skipped");

	std::filesystem::remove(path);
}

void TestDisabledByEmptyPath() {
	Check(ShaderDiskCacheLoad({}).empty(), "an empty path loads nothing");
	Check(!ShaderDiskCacheSave({}, {MakeEntry()}), "an empty path saves nothing");
}

} // namespace

int main() {
	TestColdRun();
	TestRoundTrip();
	TestManyEntries();
	TestTruncationIsSurvivable();
	TestCorruptEntryIsDropped();
	TestForeignBuildIsRejected();
	TestNonCacheFileIsRejected();
	TestAbsurdLengthsAreRefused();
	TestEmptyEntriesAreNotWritten();
	TestDisabledByEmptyPath();
	std::printf("ShaderDiskCacheTests: all passed\n");
	return 0;
}
