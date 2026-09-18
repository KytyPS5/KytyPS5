#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <mutex>
#include <span>
#include <stop_token>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>
#include <xxhash.h>

// windows.h, pulled in by the Vulkan headers, redirects this to its own ANSI entry point.
#ifdef DeleteFile
#undef DeleteFile
#endif

namespace Libs::Graphics {

namespace {

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

// Fingerprints the running binary: a rebuilt emitter emits different SPIR-V without the git
// revision changing. 0 means the binary could not be read.
uint64_t EmulatorBinaryHash() {
	std::filesystem::path exe;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	char* program = nullptr;
	if (_get_pgmptr(&program) == 0 && program != nullptr) {
		exe = std::filesystem::path(program);
	}
#else
	std::error_code error;
	exe = std::filesystem::read_symlink("/proc/self/exe", error);
	if (error) {
		exe.clear();
	}
#endif
	if (exe.empty()) {
		return 0;
	}
	Common::File file(exe, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return 0;
	}
	auto* state = XXH3_createState();
	if (state == nullptr) {
		return 0;
	}
	XXH3_64bits_reset(state);
	std::vector<uint8_t> buffer(1024u * 1024u);
	uint64_t             remaining = file.Size();
	bool                 ok        = remaining != 0;
	while (ok && remaining != 0) {
		const auto chunk = static_cast<uint32_t>(std::min<uint64_t>(remaining, buffer.size()));
		uint32_t   read  = 0;
		file.Read(buffer.data(), chunk, &read);
		ok = read == chunk;
		XXH3_64bits_update(state, buffer.data(), read);
		remaining -= read;
	}
	const auto hash = XXH3_64bits_digest(state);
	XXH3_freeState(state);
	if (!ok) {
		return 0;
	}
	return hash == 0 ? 1 : hash;
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties,
                                 uint64_t                            build_hash) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("KytyPC2:{}:{:016x}:{:08x}:{:08x}:{:08x}:{}\n", KYTY_GIT_REVISION,
	                   build_hash, properties.vendorID, properties.deviceID,
	                   properties.driverVersion, uuid);
}

// Each build keys its own file; keep one older file so alternating builds still hit. These blobs
// run to hundreds of megabytes, so the rest go.
void PruneDriverCaches(const std::filesystem::path& folder, const std::string& title_id,
                       const std::string& keep) {
	constexpr size_t KeepMax = 2;
	if (!Common::File::IsDirectoryExisting(folder)) {
		return;
	}
	const auto                                                      prefix = title_id + "-";
	const auto                                                      legacy = title_id + ".bin";
	std::vector<std::pair<Common::DateTime, std::filesystem::path>> others;
	for (const auto& entry: Common::File::GetDirEntries(folder)) {
		if (!entry.is_file || entry.name == keep || !entry.name.ends_with(".bin")) {
			continue;
		}
		if (entry.name == legacy) {
			// Unfingerprinted name written before this keying: no build can load it now.
			Common::File::DeleteFile(folder / entry.name);
			continue;
		}
		if (!entry.name.starts_with(prefix)) {
			continue;
		}
		auto path = folder / entry.name;
		others.emplace_back(Common::File::GetLastWriteTimeUTC(path), std::move(path));
	}
	if (others.size() < KeepMax) {
		return;
	}
	std::ranges::sort(others, [](const auto& a, const auto& b) { return a.first > b.first; });
	for (size_t i = KeepMax - 1; i < others.size(); i++) {
		Common::File::DeleteFile(others[i].second);
	}
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

bool ReadShaderGuestMemory(void*, uint64_t address, uint32_t* value) {
	// Coherent rather than merely clean: a GPU-driven pass sizes its own output from a
	// count an earlier dispatch wrote, and refusing that word drops the whole shader.
	if (value == nullptr) {
		return false;
	}
	return Libs::LibKernel::Memory::TryReadGpuCoherentBacking(address, value, sizeof(*value));
}

// The reader the descriptor evaluator uses for every raw read. It prefers the coherent
// path, which drains and downloads a range the GPU still owns, and otherwise falls back
// to the plain dereference the evaluator did before there was a reader at all - so a
// range the coherent path cannot serve behaves exactly as it used to, and no shader
// that resolved yesterday stops resolving today.
bool ReadShaderGuestMemoryPermissive(void*, uint64_t address, uint32_t* value) {
	if (value == nullptr) {
		return false;
	}
	if (Libs::LibKernel::Memory::TryReadGpuCoherentBacking(address, value,
	                                                       sizeof(*value))) {
		return true;
	}
	std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(*value));
	return true;
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

const char* ShaderStageName(ShaderType stage) {
	const char* name = nullptr;
	switch (stage) {
		case ShaderType::Vertex: name = "vs"; break;
		case ShaderType::Mesh: name = "ms"; break;
		case ShaderType::Local: name = "ls"; break;
		case ShaderType::TessellationControl: name = "hs"; break;
		case ShaderType::TessellationEvaluation: name = "ds"; break;
		case ShaderType::Pixel: name = "ps"; break;
		case ShaderType::Compute: name = "cs"; break;
		default: EXIT("invalid pipeline shader stage\n");
	}
	return name;
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code, const std::string& decoded_dump) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto base = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(base.parent_path());
	for (const auto& [suffix, data, size]: {
	         std::tuple {".bin", static_cast<const void*>(code.data()), code.size_bytes()},
	         std::tuple {".rdna2", static_cast<const void*>(decoded_dump.data()),
	                     decoded_dump.size()},
	     }) {
		if (size == 0) {
			continue;
		}
		auto path = base;
		path += suffix;
		Common::File file(path);
		if (file.IsInvalid()) {
			const auto path_text = Common::PathToString(path);
			LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		} else {
			file.Write(data, size);
		}
	}
}

bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	if (tools.Validate(spirv)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

// A pipeline creation still inside the driver after this long is pathological: a healthy one
// returns at once, while the SILENT HILL 2 loading-screen stall runs past a minute.
constexpr std::chrono::nanoseconds PipelineStallThreshold = std::chrono::seconds {5};
constexpr std::chrono::nanoseconds PipelineStallRepeat    = std::chrono::seconds {15};

struct PipelineCreationSlot {
	std::atomic<bool> claimed {false};
	// Non-zero only while the plain fields below are valid: published last, cleared first.
	std::atomic<int64_t> start_ns {0};
	std::atomic<int64_t> reported_ns {0};
	bool                 compute  = false;
	uint64_t             hash[2]  = {};
	uint32_t             words[2] = {};
};

// Creations are serialized by PipelineCache::m_mutex; the table only has to cover other callers.
constexpr size_t      PipelineCreationSlotCount = 32;
PipelineCreationSlot  g_pipeline_slots[PipelineCreationSlotCount];
std::atomic<uint64_t> g_pipelines_completed {0};
std::atomic<uint32_t> g_pipelines_in_flight {0};
std::atomic<bool>     g_pipeline_summary_printed {false};

int64_t PipelineNowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

// printf, not LOGF: the log stream is Silent in an ordinary run and this has to be visible there.
void PrintPipelineStallLine(bool compute, const uint64_t hash[2], const uint32_t words[2],
                            const char* phase, int64_t elapsed_ns) {
	const double seconds = static_cast<double>(elapsed_ns) / 1e9;
	if (compute) {
		std::printf("PipelineStall: compute pipeline %s after %.1fs cs=0x%016" PRIx64
		            " spirv_words=%" PRIu32 "\n",
		            phase, seconds, hash[0], words[0]);
	} else {
		std::printf("PipelineStall: graphics pipeline %s after %.1fs vs=0x%016" PRIx64
		            " spirv_words=%" PRIu32 " ps=0x%016" PRIx64 " spirv_words=%" PRIu32 "\n",
		            phase, seconds, hash[0], words[0], hash[1], words[1]);
	}
	std::fflush(stdout);
}

// Printed once, ahead of the first stall line: a stuck compile and an idle emulator look the same
// from outside, and this line is what tells them apart.
void PrintPipelineStallSummary() {
	if (g_pipeline_summary_printed.exchange(true)) {
		return;
	}
	std::printf("PipelineStall: %" PRIu64 " pipeline creations completed, %" PRIu32 " in flight\n",
	            g_pipelines_completed.load(std::memory_order_relaxed),
	            g_pipelines_in_flight.load(std::memory_order_relaxed));
	std::fflush(stdout);
}

void SweepPipelineCreations() {
	const auto now = PipelineNowNs();
	for (auto& slot: g_pipeline_slots) {
		const auto start = slot.start_ns.load(std::memory_order_acquire);
		if (start == 0) {
			continue;
		}
		const auto elapsed = now - start;
		if (elapsed < PipelineStallThreshold.count()) {
			continue;
		}
		const auto reported = slot.reported_ns.load(std::memory_order_relaxed);
		if (reported != 0 && elapsed - reported < PipelineStallRepeat.count()) {
			continue;
		}
		const bool     compute = slot.compute;
		const uint64_t hash[2] {slot.hash[0], slot.hash[1]};
		const uint32_t words[2] {slot.words[0], slot.words[1]};
		// The slot may have been freed and reclaimed while it was read; a new start says so.
		if (slot.start_ns.load(std::memory_order_acquire) != start) {
			continue;
		}
		slot.reported_ns.store(elapsed, std::memory_order_relaxed);
		PrintPipelineStallSummary();
		PrintPipelineStallLine(compute, hash, words, "still running", elapsed);
	}
}

class PipelineStallWatchdog {
public:
	PipelineStallWatchdog(): m_thread([](std::stop_token token) { Run(token); }) {}
	KYTY_CLASS_NO_COPY(PipelineStallWatchdog);

private:
	static void Run(std::stop_token token) {
		std::mutex                  mutex;
		std::condition_variable_any wake;
		std::unique_lock            lock(mutex);
		while (!wake.wait_for(lock, token, std::chrono::seconds {1},
		                      [&token] { return token.stop_requested(); })) {
			SweepPipelineCreations();
		}
	}

	std::jthread m_thread;
};

void EnsurePipelineStallWatchdog() {
	static PipelineStallWatchdog watchdog;
	(void)watchdog;
}

// Costs a slot claim and two clock reads; a healthy creation prints nothing.
class PipelineCreationTimer {
public:
	PipelineCreationTimer(bool compute, uint64_t hash0, uint32_t words0, uint64_t hash1,
	                      uint32_t words1)
	    : m_compute(compute), m_hash {hash0, hash1}, m_words {words0, words1},
	      m_start(PipelineNowNs()) {
		g_pipelines_in_flight.fetch_add(1, std::memory_order_relaxed);
		for (auto& slot: g_pipeline_slots) {
			if (slot.claimed.load(std::memory_order_relaxed) ||
			    slot.claimed.exchange(true, std::memory_order_acquire)) {
				continue;
			}
			slot.compute  = compute;
			slot.hash[0]  = hash0;
			slot.hash[1]  = hash1;
			slot.words[0] = words0;
			slot.words[1] = words1;
			slot.reported_ns.store(0, std::memory_order_relaxed);
			slot.start_ns.store(m_start, std::memory_order_release);
			m_slot = &slot;
			break;
		}
	}

	~PipelineCreationTimer() {
		const auto elapsed = PipelineNowNs() - m_start;
		if (m_slot != nullptr) {
			m_slot->start_ns.store(0, std::memory_order_release);
			m_slot->claimed.store(false, std::memory_order_release);
		}
		g_pipelines_in_flight.fetch_sub(1, std::memory_order_relaxed);
		g_pipelines_completed.fetch_add(1, std::memory_order_relaxed);
		if (elapsed >= PipelineStallThreshold.count()) {
			PrintPipelineStallSummary();
			PrintPipelineStallLine(m_compute, m_hash, m_words, "finished", elapsed);
		}
	}

	KYTY_CLASS_NO_COPY(PipelineCreationTimer);

private:
	PipelineCreationSlot* m_slot = nullptr;
	bool                  m_compute;
	uint64_t              m_hash[2];
	uint32_t              m_words[2];
	int64_t               m_start;
};

} // namespace

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
		// Set instead of `handle` when the backend refused the program.
		std::string                                  reason;
	};

	struct SourceEntry {
		explicit SourceEntry(ShaderRecompiler::IR::ResourcePlan plan)
		    : resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		ShaderRecompiler::IR::ResourcePlan resource_plan;
		std::vector<Permutation>           permutations;
	};

	static const char* StageShortName(ShaderType stage) {
		switch (stage) {
			case ShaderType::Vertex: return "vs";
			case ShaderType::Mesh: return "ms";
			case ShaderType::Pixel: return "ps";
			case ShaderType::Compute: return "cs";
			default: return "unknown";
		}
	}

	// A shader whose descriptors cannot be derived is dropped, not fatal: the draw is lost, the
	// session is not. Report each distinct hash once so a per-frame skip does not flood the log.
	// Permanent: the recompiler rejected the program, so no later dispatch can do better.
	void ReportSkipped(ShaderType stage, uint64_t hash, uint32_t pc, std::string_view reason) {
		skipped_shaders.insert(hash);
		if (!reported_shaders.insert(hash).second) {
			return;
		}
		if (reason.empty()) {
			reason = "descriptor materialization failed";
		}
		PipelineCacheLog("shader resources unavailable, skipping draws: hash=0x{:016x} "
		                 "stage={} pc=0x{:08x} {}",
		                 hash, StageShortName(stage), pc, reason);
	}

	// Transient: materialization re-executes the descriptor chain against guest memory on every
	// dispatch, so a failure describes this dispatch, not the shader. The draw is dropped and the
	// next dispatch tries again - the plan is already cached, so the retry is cheap. Recording it
	// as permanent would let one unlucky dispatch disable the shader for the whole run.
	void ReportUnmaterialized(ShaderType stage, uint64_t hash) {
		if (!reported_shaders.insert(hash).second) {
			return;
		}
		PipelineCacheLog("shader resources unavailable, skipping this dispatch: "
		                 "hash=0x{:016x} stage={} descriptor materialization failed",
		                 hash, StageShortName(stage));
	}

	// A hardware ray-tracing intersect lowered to a constant miss: the shader runs, but its
	// traced results are fabricated. Report each distinct hash once so the log says so.
	void ReportStubbed(ShaderType stage, uint64_t hash) {
		if (!stubbed_shaders.insert(hash).second) {
			return;
		}
		PipelineCacheLog("hardware ray tracing stubbed to a permanent miss: hash=0x{:016x} "
		                 "stage={}",
		                 hash, StageShortName(stage));
	}

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing up to 429 words first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 13 + ShaderVertexInputInfo::RES_MAX * 13;

	Permutation CompilePermutation(const ShaderParams&                          params,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword) {
		const char* stage_name = ShaderStageName(options.stage);
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		// Dump before the rejection returns: a shader the backend refused is exactly the one worth
		// looking at, and its decoded ISA is already in hand here.
		DumpShaderOriginal(stage_name, options.shader_hash, params.code, result.decoded_dump);
		if (!result.status.ok) {
			// The backend refused the program. Hand back a permutation with no module; the caller
			// reports it once and drops the shader's draws.
			Permutation rejected;
			rejected.specialization = std::move(specialization);
			rejected.reason         = std::move(result.status.reason);
			return rejected;
		}
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);

		const auto module = CompileSPV(result.spirv, device);
		EXIT_IF(module == nullptr);
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id          = ++next_shader_id,
		                       .module      = module,
		                       .hash        = options.shader_hash,
		                       .spirv_words = static_cast<uint32_t>(result.spirv.size())},
		};
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		// A shader already rejected once is rejected for good: skip it before paying for another
		// translation, which would otherwise repeat on every dispatch.
		if (skipped_shaders.contains(params.hash)) {
			return {};
		}

		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = static_cast<uint32_t>(params.user_data.size());
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		auto                                         entry = programs.find(lookup_key);
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		const ShaderRecompiler::IR::SrtRuntime       runtime {
		    .user_data                  = params.user_data,
		    .shader_base                = params.Base(),
		    .read_memory                = ReadShaderGuestMemoryPermissive,
		    .read_specialization_memory = ReadShaderGuestMemory,
		};
		if (entry != programs.end()) {
			// Call unconditionally: EXIT_IF drops its argument under KYTY_FINAL.
			if (!ShaderRecompiler::IR::MaterializeResources(entry->second.resource_plan, runtime,
			                                               resources, specialization)) {
				ReportUnmaterialized(stage, params.hash);
				return {};
			}
			if (const auto permutation = std::ranges::find_if(
			        entry->second.permutations, [&](const Permutation& candidate) {
				        const auto& layout = candidate.program.bindings;
				        return layout.push_data_start_dword ==
				                   ShaderRecompiler::IR::PushData::StartFor(
				                       push_data_cursor, layout.ShaderDataDwords()) &&
				               candidate.specialization == specialization;
			        });
			    permutation != entry->second.permutations.end()) {
				input_info.stage = {.program   = &permutation->program,
				                    .resources = std::move(resources)};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		const char* label = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; break;
			case ShaderType::Local: label = "ShaderRecompiler LS"; break;
			case ShaderType::TessellationControl: label = "ShaderRecompiler HS"; break;
			case ShaderType::TessellationEvaluation: label = "ShaderRecompiler DS"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = params.user_data;
		options.back_code      = params.back_code;
		options.float16     = float16;
		options.float_controls2 = float_controls2;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}
		auto translated = ShaderRecompiler::TranslateProgram(params.code, options);
		// A rejected recompile leaves no usable program: a CFG-build rejection returns an empty
		// one, and resource tracking rewrites the program in place so a rejected pass leaves it
		// half-written - extracting a plan from that indexes past the end of a resource list.
		// Checked before the cache branch because the other arm falls through to
		// CompilePermutation, which would emit SPIR-V from the same unusable program.
		if (!translated.status.ok) {
			// A shader refused this early never reaches CompilePermutation, so dump its ISA here
			// instead: a descriptor chain the host cannot re-execute is read off the ISA.
			DumpShaderOriginal(ShaderStageName(stage), params.hash, params.code,
			                   translated.decoded_dump);
			ReportSkipped(stage, params.hash, translated.status.pc, translated.status.reason);
			return {};
		}
		if (translated.program.uses_bvh_intersect_stub) {
			ReportStubbed(stage, params.hash);
		}
		if (entry == programs.end()) {
			auto resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			// A rejected plan is still cached: MaterializeResources fails on it again, so later
			// draws take the cheap cached path instead of re-translating the shader every time.
			const bool materialized = ShaderRecompiler::IR::MaterializeResources(
			    resource_plan, runtime, resources, specialization);
			entry = programs.try_emplace(lookup_key, std::move(resource_plan)).first;
			if (!materialized) {
				ReportUnmaterialized(stage, params.hash);
				return {};
			}
		}
		entry->second.permutations.push_back(CompilePermutation(
		    params, options, std::move(translated), std::move(specialization), push_data_cursor));
		if (!entry->second.permutations.back().handle) {
			auto rejected = std::move(entry->second.permutations.back());
			entry->second.permutations.pop_back();
			ReportSkipped(stage, params.hash, 0, rejected.reason);
			return {};
		}
		const auto& permutation = entry->second.permutations.back();
		input_info.stage = {.program = &permutation.program, .resources = std::move(resources)};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)],
		            counts[static_cast<size_t>(ShaderType::Local)],
		            counts[static_cast<size_t>(ShaderType::TessellationControl)],
		            counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
		return permutation.handle;
	}

	ProgramCache(vk::Device device, bool float16, bool float_controls2)
	    : device(device), float16(float16), float_controls2(float_controls2) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	std::unordered_set<uint64_t>                                skipped_shaders;
	// Log throttle only: a hash here has been reported once, whether the cause was permanent or
	// transient. Kept apart from skipped_shaders so a transient failure does not disable a shader.
	std::unordered_set<uint64_t>                                reported_shaders;
	std::unordered_set<uint64_t>                                stubbed_shaders;
	ProgramKey                                                  lookup_key;
	vk::Device                                                  device;
	bool                                                        float16 = false;
	bool                                                        float_controls2 = false;
	uint64_t                                                    next_shader_id = 0;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics),
      m_program_cache(std::make_unique<ProgramCache>(graphics.device,
                                                     graphics.shader_float16_enabled,
                                                     graphics.shader_float_controls2_enabled)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	EnsurePipelineStallWatchdog();
	InitializeDriverCache();
}

PipelineCache::~PipelineCache() {
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
		return;
	}
	m_build_hash = EmulatorBinaryHash();
	if (m_build_hash == 0) {
		// Without a fingerprint the only key left is the git revision, which identifies a clean
		// build and nothing else.
		const std::string_view git_hash     = KYTY_GIT_HASH;
		const std::string_view git_revision = KYTY_GIT_REVISION;
		if (git_hash == "unknown" || git_revision == "unknown" || git_hash.ends_with("-dirty")) {
			PipelineCacheLog("Vulkan pipeline cache: disabled (build cannot be identified)");
			return;
		}
	}

	const std::filesystem::path folder("_PipelineCache");
	const auto                  name = fmt::format("{}-{:016x}.bin", title_id, m_build_hash);
	PruneDriverCaches(folder, title_id, name);
	m_driver_cache_path     = folder / name;
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature =
		    DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties(), m_build_hash);
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::Save() {
	Common::LockGuard lock(m_mutex);
	if (!WriteDriverCacheLocked()) {
		return;
	}
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

// Written mid-run and kept alive: a session that never reaches a clean exit still leaves its
// compiles on disk.
void PipelineCache::MaybeSaveDriverCacheLocked() {
	if (m_driver_cache == nullptr) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - m_last_save < DriverCacheSavePeriod) {
		return;
	}
	m_last_save = now;
	(void)WriteDriverCacheLocked();
}

bool PipelineCache::WriteDriverCacheLocked() {
	if (m_driver_cache == nullptr) {
		return false;
	}

	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		return false;
	}
	payload.resize(size);
	auto prefix = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties(), m_build_hash);
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return false;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return false;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	return true;
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		pixel_params = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
	}
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info[tess_active ? 2u : 0u].clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor =
	    mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms  result;
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
		if (!result.pixel) {
			return {};
		}
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] = m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor);
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	Common::LockGuard lock(m_mutex);
	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		key.vertex_shader_ids[i] = programs.vertex[i].id;
	}
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = 0;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		static_params.color_srcblend[slot]       = bc.color_srcblend;
		static_params.color_comb_fcn[slot]       = bc.color_comb_fcn;
		static_params.color_destblend[slot]      = bc.color_destblend;
		static_params.alpha_srcblend[slot]       = bc.alpha_srcblend;
		static_params.alpha_comb_fcn[slot]       = bc.alpha_comb_fcn;
		static_params.alpha_destblend[slot]      = bc.alpha_destblend;
		static_params.separate_alpha_blend[slot] = bc.separate_alpha_blend;
		static_params.blend_enable[slot]         = bc.enable && !rt.info.blend_bypass;
	}
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	const bool rect_list =
	    command.GetUserConfig().GetPrimType() == Prospero::PrimitiveType::kRectList;
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		uint32_t attributes_num          = 0;
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			EXIT_IF(buffer.attr_num < 0 || buffer.attr_num > ShaderVertexInputBuffer::ATTR_MAX);
			attributes_num += static_cast<uint32_t>(buffer.attr_num);
			EXIT_IF(attributes_num > static_cast<uint32_t>(vs_input_info.resources_num));
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
			for (int attribute = 0; attribute < buffer.attr_num; attribute++) {
				const auto index = buffer.attr_indices[attribute];
				EXIT_IF(index < 0 || index >= vs_input_info.resources_num);
				key.vertex_input.attributes[index] = {
				    .offset  = buffer.attr_offsets[attribute],
				    .binding = static_cast<uint8_t>(binding),
				};
			}
		}
		EXIT_IF(attributes_num != static_cast<uint32_t>(vs_input_info.resources_num));
	}

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	{
		PipelineCreationTimer timer(false, vertex_program.hash, vertex_program.spirv_words,
		                            ps_active ? pixel_program.hash : 0,
		                            ps_active ? pixel_program.spirv_words : 0);
		CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vertex_info,
		                       ps_input_info, programs, static_params, m_driver_cache);
	}
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);
	MaybeSaveDriverCacheLocked();

	return *iter->second;
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	Common::LockGuard lock(m_mutex);

	if (auto iter = m_compute_pipelines.find(compute_program.id);
	    iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<Pipeline>();
	{
		PipelineCreationTimer timer(true, compute_program.hash, compute_program.spirv_words, 0, 0);
		CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module,
		                       m_driver_cache);
	}

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(compute_program.id, std::move(cached));
	EXIT_IF(!inserted);
	MaybeSaveDriverCacheLocked();

	return *iter->second;
}
} // namespace Libs::Graphics
