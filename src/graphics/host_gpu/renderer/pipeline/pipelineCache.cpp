#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/pipeline/shaderReadCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/pipeline/unfoldableSet.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterializationMemo.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
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
#include <optional>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
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

uint8_t RemapSourceAlphaFactor(uint8_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Alpha);
		case Prospero::BlendFactor::kOneMinusSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
		default: return factor;
	}
}

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
	// The Windows entry point is wmain, so the CRT never initializes the narrow program path and
	// _get_pgmptr does not report that as an error: it fails validation, and the invalid-parameter
	// handler is noreturn, so asking for the narrow copy kills the process.
	wchar_t* program = nullptr;
	if (_get_wpgmptr(&program) == 0 && program != nullptr) {
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

// Vulkan defines cache compatibility on the device and pipelineCacheUUID, not on our build.
uint64_t DriverCacheKey(const vk::PhysicalDeviceProperties& properties) {
	struct Identity {
		uint32_t vendor_id      = 0;
		uint32_t device_id      = 0;
		uint32_t driver_version = 0;
		uint8_t  uuid[VK_UUID_SIZE] {};
	} identity {};
	static_assert(sizeof(Identity) == 3 * sizeof(uint32_t) + VK_UUID_SIZE);
	identity.vendor_id      = properties.vendorID;
	identity.device_id      = properties.deviceID;
	identity.driver_version = properties.driverVersion;
	std::memcpy(identity.uuid, properties.pipelineCacheUUID.data(), VK_UUID_SIZE);
	const auto hash = XXH3_64bits(&identity, sizeof(identity));
	return hash == 0 ? 1 : hash;
}

constexpr std::string_view DriverCacheMagic = "KytyPC3:";

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("{}{:08x}:{:08x}:{:08x}:{}\n", DriverCacheMagic, properties.vendorID,
	                   properties.deviceID, properties.driverVersion, uuid);
}

bool DriverCacheFileIsCurrentFormat(const std::filesystem::path& path) {
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return false;
	}
	std::string magic(DriverCacheMagic.size(), '\0');
	uint32_t    read = 0;
	file.Read(magic.data(), static_cast<uint32_t>(magic.size()), &read);
	file.Close();
	return read == magic.size() && magic == DriverCacheMagic;
}

// Keep the previous driver's file in case of a rollback; an older format can never load again.
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
		if (!DriverCacheFileIsCurrentFormat(path)) {
			Common::File::DeleteFile(path);
			continue;
		}
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

// KYTY_CFG_CACHE_LOG=1 reports every write of the structurized-CFG file.
bool CfgCacheLogEnabled() {
	static const bool enabled = [] {
		const char* text = std::getenv("KYTY_CFG_CACHE_LOG");
		return text != nullptr && std::strcmp(text, "0") != 0;
	}();
	return enabled;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

// A descriptor may declare an arena far larger than the pages behind it, and enumerating the
// declared size probes memory that cannot answer. Report what is actually mapped instead.
uint64_t ReadableShaderExtent(void*, uint64_t address, uint64_t size) {
	return Libs::LibKernel::Memory::ClampRangeSize(address, size);
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	// Scalar and unformatted buffer dependencies use the same backing as native raw loads.
	// Image synchronization belongs to formatted buffer bindings, not these reads.
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(), values.size_bytes());
}

// The reader the descriptor evaluator uses for every raw read. It prefers the coherent
// path, which drains and downloads a range the GPU still owns, and otherwise falls back
// to the plain dereference the evaluator did before there was a reader at all - so a
// range the coherent path cannot serve behaves exactly as it used to, and no shader
// that resolved yesterday stops resolving today.
bool ReadShaderGuestMemoryPermissive(void*, uint64_t address, std::span<uint32_t> values) {
	if (values.empty()) {
		return false;
	}
	if (Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(),
	                                                  values.size_bytes())) {
		return true;
	}
	std::memcpy(values.data(), reinterpret_cast<const void*>(address), values.size_bytes());
	return true;
}

bool                                  ReadShaderLine(uint64_t address, void* data, uint64_t size);
bool                                  EnvSwitch(const char* name);
Libs::Graphics::ShaderGuestReadCache& ShaderReadCache();

bool ShaderReadGpuOwned(void*, uint64_t address, uint64_t size) {
	static const bool no_line_cache = EnvSwitch("KYTY_NO_OWNERSHIP_LINE_CACHE");
	if (!no_line_cache && ShaderReadCache().CleanCovered(address, size, ReadShaderLine)) {
		return false;
	}
	return Libs::LibKernel::Memory::HasGpuOwnedBytes(address, size);
}

bool ReadShaderLine(uint64_t address, void* data, uint64_t size) {
	return Libs::Graphics::GuestRange {address, size}.Valid() &&
	       Libs::LibKernel::Memory::TryReadGpuCleanBacking(address, data, size);
}

bool ShaderReadMayDrain(uint64_t address, std::span<uint32_t> values) {
	return Libs::Graphics::GuestRange {address, values.size_bytes()}.Valid() &&
	       Libs::Graphics::GuestGpu::IsGpuThread();
}

template <bool Permissive>
bool ReadShaderGuestMemoryCached(void* userdata, uint64_t address, std::span<uint32_t> values) {
	return static_cast<Libs::Graphics::ShaderGuestReadCache*>(userdata)->Read(
	    address, values, ReadShaderLine,
	    [](uint64_t word_address, std::span<uint32_t> words, bool& drained) {
		    if (words.empty()) {
			    return false;
		    }
		    if (Libs::LibKernel::Memory::TryReadGpuCleanBacking(word_address, words.data(),
		                                                        words.size_bytes())) {
			    return true;
		    }
		    drained = ShaderReadMayDrain(word_address, words);
		    return Permissive ? ReadShaderGuestMemoryPermissive(nullptr, word_address, words)
		                      : ReadShaderGuestMemory(nullptr, word_address, words);
	    });
}

bool ReadShaderGuestMemoryClean(void* userdata, uint64_t address, std::span<uint32_t> values) {
	const auto read = [](uint64_t word_address, std::span<uint32_t> words, bool&) {
		return !words.empty() && Libs::LibKernel::Memory::TryReadGpuCleanBacking(
		                             word_address, words.data(), words.size_bytes());
	};
	if (userdata == nullptr) {
		bool drained = false;
		return read(address, values, drained);
	}
	return static_cast<Libs::Graphics::ShaderGuestReadCache*>(userdata)->Read(address, values,
	                                                                          ReadShaderLine, read);
}

bool HashShaderGuestBlock(void*, uint64_t address, uint64_t size, uint64_t* hash) {
	const std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)> state(XXH3_createState(),
	                                                                     XXH3_freeState);
	if (size == 0 || state == nullptr || XXH3_64bits_reset(state.get()) != XXH_OK) {
		return false;
	}
	const auto visit = [](void* context, const uint8_t* data, uint64_t bytes) {
		XXH3_64bits_update(static_cast<XXH3_state_t*>(context), data, static_cast<size_t>(bytes));
	};
	if (!Libs::LibKernel::Memory::TryVisitGpuCleanBacking(address, size, visit, state.get())) {
		return false;
	}
	*hash = XXH3_64bits_digest(state.get());
	return true;
}

thread_local bool t_stage_reads_shared = false;

Libs::Graphics::ShaderGuestReadCache& ShaderReadCache() {
	static thread_local Libs::Graphics::ShaderGuestReadCache cache;
	return cache;
}

// Asked only after a read has already refused, so it re-probes and may well answer that the range
// reads back now - that answer is itself the finding, not a contradiction.
const char* DescribeShaderReadRefusal(void*, uint64_t address) {
	return Libs::LibKernel::Memory::DescribeGpuBackingRefusal(address, sizeof(uint32_t));
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
                        std::span<const uint32_t> code) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto             path =
	    Config::GetShaderLogFolder() / "original" /
	    fmt::format("{:04d}_new_shader_{}_{:016x}.bin", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(code.data(), code.size_bytes());
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

// vkCreate*Pipelines cannot be cancelled or given a deadline, so a module the driver cannot build
// in bounded time takes the whole session with it: RESIDENT EVIL REQUIEM's clustered-lighting
// pixel shader emits 384046 words over 562 blocks, and the build held two cores and passed 12 GB
// of working set without returning, freezing the guest at 0 fps until the process died. Refusing
// the module is the only place that can be stopped, and it costs only that shader's draws.
//
// Size alone does not say which module that is - a 322769-word compute module of two blocks builds
// here without trouble. The dispatcher fallback is what the driver cannot scale to: one loop whose
// switch carries an arm per block, with a spill variable for every value that crosses one. So the
// bound applies only to those, and it sits far above every dispatcher module measured to build
// here, the largest of which is 3955 words.
constexpr size_t MaxDispatcherSpirvWords = 131072;

// A VkPipelineCache cannot evict, so past this size the blob on disk stops growing.
constexpr uint64_t MaxDriverCacheBytes = uint64_t {1} << 30u;

// A pipeline creation still inside the driver after this long is pathological: a healthy one
// returns at once, while the SILENT HILL 2 loading-screen stall runs past a minute.
constexpr std::chrono::nanoseconds PipelineStallThreshold = std::chrono::seconds {5};
constexpr std::chrono::nanoseconds PipelineStallRepeat    = std::chrono::seconds {15};
// A finished creation is still named from here, long enough to show as a hitch.
constexpr std::chrono::nanoseconds PipelineSlowThreshold = std::chrono::milliseconds {250};

struct PipelineCreationSlot {
	std::atomic<bool> claimed {false};
	// Non-zero only while the plain fields below are valid: published last, cleared first.
	std::atomic<int64_t> start_ns {0};
	std::atomic<int64_t> reported_ns {0};
	bool                 compute  = false;
	uint64_t             hash[2]  = {};
	uint32_t             words[2] = {};
};

// Creations are serialized by the owning renderer; the table only has to cover other callers.
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
		std::printf("PipelineStall: compute pipeline %s after %.2fs cs=0x%016" PRIx64
		            " spirv_words=%" PRIu32 "\n",
		            phase, seconds, hash[0], words[0]);
	} else {
		std::printf("PipelineStall: graphics pipeline %s after %.2fs vs=0x%016" PRIx64
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
		if (elapsed >= PipelineSlowThreshold.count()) {
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

bool EnvSwitch(const char* name) {
	const char* text = std::getenv(name);
	return text != nullptr && std::strcmp(text, "0") != 0;
}

} // namespace

std::size_t
PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, key.rendering.color_count);
	for (uint32_t i = 0; i < key.rendering.color_count; i++) {
		PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.color_formats[i]));
	}
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
	for (const auto id: key.vertex_shader_ids) {
		PipelineKeyHash::Mix(hash, id);
	}
	PipelineKeyHash::Mix(hash, key.ps_shader_id);
	PipelineKeyHash::Mix(hash, key.vertex_input.binding_count);
	for (uint32_t i = 0; i < key.vertex_input.binding_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].stride);
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].instance);
	}
	PipelineKeyHash::Mix(hash, key.vertex_input.attribute_count);
	for (uint32_t i = 0; i < key.vertex_input.attribute_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].offset);
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].binding);
	}
	PipelineKeyHash::Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
	return hash;
}

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType stage           = ShaderType::Unknown;
		uint64_t   hash            = 0;
		uint32_t   user_data_count = 0;
		uint32_t   code_size       = 0;
		// Bumped when a draw proves another of this shader's descriptors unfoldable. Carrying it
		// in the key rather than erasing the old entries is deliberate: a SourceEntry owns
		// vk::ShaderModules that command buffers already recorded may still reference, and
		// nothing here knows when the last of those retires. A new key simply misses, translates
		// once more with the larger proof set, and leaves the old permutations to the destructor.
		uint32_t              unfoldable_generation = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		std::vector<uint32_t>                        function_code;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
		// Set instead of `handle` when the backend refused the program.
		std::string reason;
	};

	struct SourceEntry {
		SourceEntry(ShaderRecompiler::IR::ResourcePlan              plan,
		            std::unique_ptr<ShaderRecompiler::ShaderSource> call_source)
		    : call_source(std::move(call_source)), resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		[[nodiscard]] std::span<const uint32_t> FunctionCode() const {
			return call_source && call_source->linked
			           ? std::span<const uint32_t>(call_source->linked->code)
			                 .subspan(call_source->code.size())
			           : std::span<const uint32_t> {};
		}

		bool RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime) {
			const auto revision = call_source->revision;
			ShaderRecompiler::RefreshShaderSource(*call_source, runtime);
			resource_plan.source_reads = call_source->reads;
			return revision != call_source->revision;
		}

		// Materialization is the caller's: it can fail, and a failure is reported and relearned
		// there. A program with SWAPPC calls is also keyed on the callee code it was linked with.
		Permutation* FindPermutation(uint32_t push_data_cursor, bool exact_start) {
			for (auto& candidate: permutations) {
				const auto& layout   = candidate.program.bindings;
				const bool  start_ok = exact_start
				                           ? layout.push_data_start_dword ==
				                                 ShaderRecompiler::IR::PushData::StartFor(
				                                     push_data_cursor, layout.ShaderDataDwords())
				                           : ShaderRecompiler::IR::PushData::StartServes(
				                                 layout.push_data_start_dword, push_data_cursor);
				if (!start_ok || !ShaderRecompiler::IR::SpecializationServes(
				                     candidate.specialization, specialization)) {
					continue;
				}
				if (call_source && !std::ranges::equal(candidate.function_code, FunctionCode())) {
					continue;
				}
				return &candidate;
			}
			return nullptr;
		}

		// Retained only for a program with SWAPPC calls, whose linked callees follow guest memory.
		std::unique_ptr<ShaderRecompiler::ShaderSource> call_source;
		ShaderRecompiler::IR::ResourcePlan              resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot          resources;
		ShaderRecompiler::IR::ResourceSpecialization    specialization;
		std::vector<Permutation>                        permutations;
		ShaderRecompiler::IR::MaterializeMemoWays       memo;
		ShaderRecompiler::IR::MaterializeMemoBackoff    memo_backoff;
		// Whether the walk after a miss records for the memo; false while the stage backs off.
		bool memo_recording = false;
	};

	static bool MaterializeMemoEnabled() {
		static const bool enabled = !EnvSwitch("KYTY_NO_MATERIALIZE_MEMO");
		return enabled;
	}

	static bool MaterializeMemoHit(SourceEntry&                            entry,
	                               const ShaderRecompiler::IR::SrtRuntime& runtime) {
		entry.memo_recording = MaterializeMemoEnabled();
		if (!entry.memo_recording) {
			return false;
		}
		const bool backoff = ShaderRecompiler::IR::MaterializeMemoBackoff::Enabled();
		if (backoff && entry.memo_backoff.Skip()) {
			entry.memo_recording = false;
			entry.memo.ForgetCurrent();
			return false;
		}
		if (entry.memo.Replay(entry.resource_plan, runtime, entry.resources,
		                      entry.specialization)) {
			entry.memo_backoff.OnHit();
			return true;
		}
		if (backoff) {
			entry.memo_backoff.OnMiss();
		}
		return false;
	}

	static const char* StageShortName(ShaderType stage) {
		switch (stage) {
			case ShaderType::Vertex: return "vs";
			case ShaderType::Mesh: return "ms";
			case ShaderType::Pixel: return "ps";
			case ShaderType::Compute: return "cs";
			case ShaderType::Local: return "ls";
			case ShaderType::TessellationControl: return "hs";
			case ShaderType::TessellationEvaluation: return "ds";
			default: return "unknown";
		}
	}

	// A shader whose descriptors cannot be derived is dropped, not fatal: the draw is lost, the
	// session is not. Report each distinct hash once so a per-frame skip does not flood the log.
	// Permanent for the key that was refused: the recompiler rejected that translation, so no later
	// dispatch of it can do better. Keyed on the whole ProgramKey and not on the hash, because a
	// refusal is a property of the translation and the translation is a function of more than the
	// code - the user-data count and the stage's static state reach resource tracking, and the
	// specialization reaches the backend. A hash-keyed skip let one refused shape disable every
	// other shape of the same shader, including shapes already drawing.
	void ReportSkipped(const ProgramKey& key, ShaderType stage, uint64_t hash, uint32_t pc,
	                   std::string_view reason) {
		skipped_shaders.insert(key);
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

	// Not the shader's fault and not permanent for it: one permutation was refused, the channel
	// that asked for it is frozen, and the generation before it still draws. Deliberately does
	// not touch `skipped_shaders` at all - the refused generation is about to be rolled back, so
	// recording it would only leave an entry nothing can ever ask for again. One line per shader;
	// the draw that hits the refusal is lost and the next one renders.
	void ReportRebuildRefused(ShaderType stage, uint64_t hash, std::string_view reason) {
		if (!reported_shaders.insert(hash).second) {
			return;
		}
		PipelineCacheLog("proven-unfoldable rebuild refused, keeping the previous classification: "
		                 "hash=0x{:016x} stage={} {}",
		                 hash, StageShortName(stage), reason);
	}

	// Transient: materialization re-executes the descriptor chain against guest memory on every
	// dispatch, so a failure describes this dispatch, not the shader. The draw is dropped and the
	// next dispatch tries again - the plan is already cached, so the retry is cheap. Recording it
	// as permanent would let one unlucky dispatch disable the shader for the whole run.
	void ReportUnmaterialized(ShaderType stage, uint64_t hash) {
		if (!reported_shaders.insert(hash).second) {
			return;
		}
		const auto reason = ShaderRecompiler::IR::LastMaterializeFailure();
		PipelineCacheLog("shader resources unavailable, skipping this dispatch: "
		                 "hash=0x{:016x} stage={} descriptor materialization failed: {}",
		                 hash, StageShortName(stage),
		                 reason.empty() ? std::string_view {"no recorded reason"} : reason);
	}

	// The materialization refused a descriptor it cannot resolve on the host, and the shader is
	// translated again with that descriptor decoded in the shader. The draw that hit the refusal is
	// served by the re-translation in the same call, so this is not a skip; ReportUnmaterialized
	// still fires if the re-translation draws nothing. Its own dedupe set, so a later real skip of
	// the same hash is not swallowed by this line.
	void ReportRelearned(ShaderType stage, uint64_t hash, std::string_view reason, size_t pcs) {
		if (!relearned_shaders.insert(hash).second) {
			return;
		}
		PipelineCacheLog("shader re-translated with {} descriptor source(s) decoded in the shader: "
		                 "hash=0x{:016x} stage={} host materialization refused: {}",
		                 pcs, hash, StageShortName(stage),
		                 reason.empty() ? std::string_view {"no recorded reason"} : reason);
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

	void ReportCallStubbed(ShaderType stage, uint64_t hash) {
		if (!call_stubbed_shaders.insert(hash).second) {
			return;
		}
		PipelineCacheLog("any-hit calls stubbed to accept every hit: hash=0x{:016x} stage={}", hash,
		                 StageShortName(stage));
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
			PipelineKeyHash::Mix(hash, key.unfoldable_generation);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing the full state first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 32 + ShaderVertexInputInfo::RES_MAX * 6;

	Permutation CompilePermutation(const char*                                  stage_name,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t                  push_data_start_dword,
	                               std::span<const uint32_t> function_code) {
		// Read before the program is moved into the compile: the two numbers that say why a module
		// came out the size it did.
		const bool dispatcher  = translated.program.dispatcher_fallback;
		const auto block_count = translated.program.blocks.size();
		auto       result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                                     specialization, push_data_start_dword);
		if (!result.status.ok) {
			// The backend refused the program. Hand back a permutation with no module; the caller
			// reports it once and drops the shader's draws.
			Permutation rejected;
			rejected.specialization = std::move(specialization);
			rejected.reason         = std::move(result.status.reason);
			return rejected;
		}
		if (dispatcher && result.spirv.size() > MaxDispatcherSpirvWords) {
			Permutation rejected;
			rejected.specialization = std::move(specialization);
			rejected.reason         = fmt::format(
			    "unstructured control flow emitted {} SPIR-V words over {} blocks, past the {} a "
			    "host pipeline build is trusted with",
			    result.spirv.size(), block_count, MaxDispatcherSpirvWords);
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
		    .function_code  = {function_code.begin(), function_code.end()},
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id          = ++next_shader_id,
		                       .module      = module,
		                       .hash        = options.shader_hash,
		                       .spirv_words = static_cast<uint32_t>(result.spirv.size())},
		};
	}

	template <typename InputInfo>
	ShaderProgram Relearned(const ShaderParams& params, InputInfo& input_info,
	                        uint32_t& push_data_cursor) {
		static thread_local uint32_t depth    = 0;
		static const bool            disabled = EnvSwitch("KYTY_NO_UNFOLDABLE_RETRY");
		if (disabled || depth >= 8) {
			return {};
		}
		++depth;
		auto program = Get(params, input_info, push_data_cursor);
		--depth;
		return program;
	}

	// A materialization failed. When it named sources the channel had not proved yet, the shader
	// is re-translated with them decoded in the shader and this draw is served by that; otherwise
	// the draw is dropped and the next one tries again.
	template <typename InputInfo>
	ShaderProgram Unmaterialized(const ShaderParams& params, InputInfo& input_info,
	                             uint32_t& push_data_cursor, ShaderType stage, uint64_t code_size,
	                             uint64_t code_key, std::span<const uint32_t> reported) {
		const std::string reason(ShaderRecompiler::IR::LastMaterializeFailure());
		if (!unfoldable.Learn(code_size, code_key, reported)) {
			ReportUnmaterialized(stage, params.hash);
			return {};
		}
		ReportRelearned(stage, params.hash, reason, reported.size());
		auto program = Relearned(params, input_info, push_data_cursor);
		if (!program) {
			ReportUnmaterialized(stage, params.hash);
		}
		return program;
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

		KYTY_PROFILER_BLOCK("ProgramCache::Get");
		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		const auto code_size = UnfoldableCodeSize(params.code, params.back_code);
		std::optional<uint64_t> code_key;
		const auto              key = [&] {
			if (!code_key.has_value()) {
				KYTY_PROFILER_BLOCK("ProgramCache::ShaderCodeKey");
				code_key = ShaderCodeKey(params.code, params.back_code);
			}
			return *code_key;
		};
		const auto& proven               = unfoldable.Find(code_size, key);
		lookup_key.stage                 = stage;
		lookup_key.hash                  = params.hash;
		lookup_key.user_data_count       = params.user_data_count;
		lookup_key.code_size             = static_cast<uint32_t>(params.code.size());
		lookup_key.unfoldable_generation = proven.generation;
		BuildStageStaticKey(input_info, lookup_key.static_state);
		// A shape already rejected once is rejected for good: skip it before paying for another
		// translation, which would otherwise repeat on every dispatch. Built after the key because
		// the key is what was refused; building it costs a stage's static state and nothing more.
		if (skipped_shaders.contains(lookup_key)) {
			return {};
		}
		auto entry = programs.find(lookup_key);
		// Filled by MaterializeResources with the resources it had to bind null. Learning from it
		// moves the generation above, so the next draw of this shader misses and re-translates
		// with the proof in hand.
		std::vector<uint32_t> reported_unfoldable;
		auto&                 read_cache = ShaderReadCache();
		if (!t_stage_reads_shared) {
			read_cache.Reset();
		}
		static const bool no_read_cache = EnvSwitch("KYTY_NO_SHADER_READ_CACHE");
		static const bool no_heap_hash  = EnvSwitch("KYTY_NO_HEAP_HASH_IN_PLACE");
		static const bool                no_gpu_owned  = EnvSwitch("KYTY_NO_GPU_OWNED_NATIVE");
		ShaderRecompiler::IR::SrtRuntime runtime {
		    .user_data   = user_data,
		    .shader_base = params.Base(),
		    .read_memory =
		        no_read_cache ? ReadShaderGuestMemoryPermissive : ReadShaderGuestMemoryCached<true>,
		    .userdata = no_read_cache ? nullptr : &read_cache,
		    .read_specialization_memory =
		        no_read_cache ? ReadShaderGuestMemory : ReadShaderGuestMemoryCached<false>,
		    .readable_extent           = ReadableShaderExtent,
		    .describe_read_refusal     = DescribeShaderReadRefusal,
		    .hash_specialization_block = no_heap_hash ? nullptr : HashShaderGuestBlock,
		    .read_condition_memory     = ReadShaderGuestMemoryClean,
		    .gpu_owned                 = no_gpu_owned ? nullptr : ShaderReadGpuOwned,
		};
		if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			runtime.workgroup_counts = input_info.workgroup_counts;
		}
		bool source_changed = false;
		if (entry != programs.end() && entry->second.call_source) {
			source_changed = entry->second.RefreshCallSource(runtime);
			// The memo replays a walk of the plan it recorded, and a SWAPPC program's plan follows
			// the callees its source links, so such a program always walks.
			entry->second.memo_recording = false;
		}
		// A changed callee re-translates below before anything is materialized against its plan.
		if (entry != programs.end() && !source_changed &&
		    (entry->second.call_source || !MaterializeMemoHit(entry->second, runtime))) {
			auto&      memo     = entry->second.memo;
			const auto recorded = entry->second.memo_recording ? memo.Record(runtime) : runtime;
			// Call unconditionally: EXIT_IF drops its argument under KYTY_FINAL.
			if (!ShaderRecompiler::IR::MaterializeResources(
			        entry->second.resource_plan, recorded, entry->second.resources,
			        entry->second.specialization, &reported_unfoldable)) {
				return Unmaterialized(params, input_info, push_data_cursor, stage, code_size, key(),
				                      reported_unfoldable);
			}
			if (entry->second.memo_recording) {
				memo.Commit(entry->second.resource_plan, entry->second.resources,
				            entry->second.specialization);
			}
		}
		if (entry != programs.end() && !source_changed) {
			KYTY_PROFILER_BLOCK("ProgramCache permutation search");
			auto* permutation = entry->second.FindPermutation(push_data_cursor, true);
			if (permutation == nullptr && stage != ShaderType::Pixel) {
				permutation = entry->second.FindPermutation(push_data_cursor, false);
			}
			if (permutation != nullptr) {
				input_info.stage = {.program   = &permutation->program,
				                    .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
			// The shader is cached and a permutation of it is about to be rebuilt. Both halves of
			// the search can reject a candidate, and they are different bugs with different fixes,
			// so count which one did it and - when it was the specialization - what actually
			// differed. This is measurement: a run's own answer to how much of its recompilation
			// is the resource shape changing and how much is not.
			ReportPermutationMiss(entry->second.permutations, entry->second.specialization,
			                      push_data_cursor);
		}

		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		const char* label      = nullptr;
		const char* stage_name = nullptr;
		switch (stage) {
			case ShaderType::Vertex:
				label      = "ShaderRecompiler VS";
				stage_name = "vs";
				break;
			case ShaderType::Mesh:
				label      = "ShaderRecompiler MS";
				stage_name = "ms";
				break;
			case ShaderType::Local:
				label      = "ShaderRecompiler LS";
				stage_name = "ls";
				break;
			case ShaderType::TessellationControl:
				label      = "ShaderRecompiler HS";
				stage_name = "hs";
				break;
			case ShaderType::TessellationEvaluation:
				label      = "ShaderRecompiler DS";
				stage_name = "ds";
				break;
			case ShaderType::Pixel:
				label      = "ShaderRecompiler PS";
				stage_name = "ps";
				break;
			case ShaderType::Compute:
				label      = "ShaderRecompiler CS";
				stage_name = "cs";
				break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		ShaderRecompiler::CompileOptions options;
		options.stage          = stage;
		options.shader_hash    = params.hash;
		options.user_data      = user_data;
		options.back_code      = params.back_code;
		options.dump_ir        = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump     = options.dump_ir;
		options.dump_label     = label;
		options.input_info     = stage_input;
		options.unfoldable_pcs = proven.pcs;
		options.host_subgroup_size = host_subgroup_size;
		options.robust_buffer_access2 = robust_buffer_access2;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size      = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
				if (stage == ShaderType::Mesh) {
					// A lowered NGG vertex stage never receives the merged wave info, so the host
					// subgroup size has to travel with the stage instead.
					options.host_subgroup_size = input_info.mesh.host_subgroup_size;
				}
			}
		} else {
			options.wave_size = input_info.wave_size;
			// Compute carries its own host subgroup size; the pixel stage has none.
			if constexpr (requires { input_info.host_subgroup_size; }) {
				options.host_subgroup_size = input_info.host_subgroup_size;
			}
		}
		DumpShaderOriginal(stage_name, options.shader_hash, params.code);
		// Tracking asks the guest memory of this dispatch whether a recognised descriptor table
		// would be refused, so such a table is decoded in the shader by this one translation
		// instead of being refused by the materialization below and translated a second time.
		static const bool no_table_check = EnvSwitch("KYTY_NO_FIRST_DISPATCH_TABLE_CHECK");
		if (!no_table_check) {
			options.refuse_indirect_buffer_table =
			    [&runtime](const ShaderRecompiler::IR::Program&                    program,
			               std::span<const ShaderRecompiler::IR::DescriptorSource> sources,
			               uint32_t                                                source) {
				    return ShaderRecompiler::IR::IndirectBufferTableRefused(program, sources,
				                                                            source, runtime);
			    };
		}
		// Decoded once per cache source; a program with SWAPPC calls keeps its source so later
		// dispatches can follow the callees it links.
		ShaderRecompiler::ShaderSource source;
		if (entry == programs.end()) {
			source = ShaderRecompiler::PrepareShaderSource(params.code, options);
		}
		auto* current = entry == programs.end() ? &source : entry->second.call_source.get();
		ShaderRecompiler::TranslateResult translated;
		if (current != nullptr) {
			if (current->call) {
				if (entry == programs.end()) {
					ShaderRecompiler::RefreshShaderSource(*current, runtime);
				}
				// An unlinked call falls back to the any-hit stub in the plain program.
				translated = ShaderRecompiler::TranslateProgram(
				    current->linked ? current->linked->decoded : current->decoded, options);
				translated.program.source_reads = current->reads;
			} else {
				translated = ShaderRecompiler::TranslateProgram(current->decoded, options);
			}
		} else {
			translated = ShaderRecompiler::TranslateProgram(params.code, options);
		}
		options.refuse_indirect_buffer_table = nullptr;
		// A rejected recompile leaves no usable program: a CFG-build rejection returns an empty
		// one, and resource tracking rewrites the program in place so a rejected pass leaves it
		// half-written - extracting a plan from that indexes past the end of a resource list.
		// Checked before the cache branch because the other arm falls through to
		// CompilePermutation, which would emit SPIR-V from the same unusable program.
		if (!translated.status.ok) {
			ReportSkipped(lookup_key, stage, params.hash, translated.status.pc,
			              translated.status.reason);
			return {};
		}
		if (translated.program.uses_bvh_intersect_stub) {
			ReportStubbed(stage, params.hash);
		}
		if (translated.program.uses_call_stub) {
			ReportCallStubbed(stage, params.hash);
		}
		if (entry == programs.end()) {
			auto retained =
			    source.call ? std::make_unique<ShaderRecompiler::ShaderSource>(std::move(source))
			                : nullptr;
			entry = programs
			            .try_emplace(lookup_key,
			                         ShaderRecompiler::IR::ExtractResourcePlan(translated.program),
			                         std::move(retained))
			            .first;
			// A rejected plan is still cached: MaterializeResources fails on it again, so later
			// draws take the cheap cached path instead of re-translating the shader every time.
			if (!ShaderRecompiler::IR::MaterializeResources(
			        entry->second.resource_plan, runtime, entry->second.resources,
			        entry->second.specialization, &reported_unfoldable)) {
				return Unmaterialized(params, input_info, push_data_cursor, stage, code_size, key(),
				                      reported_unfoldable);
			}
		} else if (source_changed) {
			// The callees changed: the plan follows the re-linked program, and a permutation built
			// for the same callee code may already serve it.
			entry->second.resource_plan =
			    ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			if (!ShaderRecompiler::IR::MaterializeResources(
			        entry->second.resource_plan, runtime, entry->second.resources,
			        entry->second.specialization, &reported_unfoldable)) {
				return Unmaterialized(params, input_info, push_data_cursor, stage, code_size, key(),
				                      reported_unfoldable);
			}
			if (const auto* permutation = entry->second.FindPermutation(push_data_cursor, true)) {
				input_info.stage = {.program   = &permutation->program,
				                    .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}
		// Whether this permutation exists only because the unfoldable channel asked for it. Read
		// before the compile, because `options.unfoldable_pcs` is a span over `proven.pcs` and a
		// rollback below rewrites that vector.
		const bool channel_rebuild = !options.unfoldable_pcs.empty();
		auto       specialization  = entry->second.specialization;
		{
			std::vector<const ShaderRecompiler::IR::ResourceSpecialization*> donors;
			donors.reserve(entry->second.permutations.size());
			for (const auto& built: entry->second.permutations) {
				donors.push_back(&built.specialization);
			}
			ShaderRecompiler::IR::InheritUnboundShapes(donors, specialization);
		}
		entry->second.permutations.push_back(CompilePermutation(
		    stage_name, options, std::move(translated), std::move(specialization), push_data_cursor,
		    entry->second.FunctionCode()));
		if (!entry->second.permutations.back().handle) {
			auto rejected = std::move(entry->second.permutations.back());
			entry->second.permutations.pop_back();
			// A permutation the channel asked for and the host refused must not take the shader
			// with it. The skip set now carries the generation, so a refusal no longer disables
			// the one before this rebuild by itself - but that generation would still never be
			// asked for again, because `proven.generation` only moves forward. Roll it back to
			// the generation that draws: the shot keeps the missing effect it had before the
			// channel fired, which is the outcome the channel was trying to improve on and is
			// strictly better than a shader that stops drawing at all.
			if (channel_rebuild && RefuseRebuild(unfoldable.Get(key()))) {
				ReportRebuildRefused(stage, params.hash, rejected.reason);
				return {};
			}
			ReportSkipped(lookup_key, stage, params.hash, 0, rejected.reason);
			return {};
		}
		const auto& permutation = entry->second.permutations.back();
		input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
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

	// Why the permutation search rejected every cached permutation of a shader it had already
	// translated. A candidate that matches the specialization but not the push-data start is the
	// vertex stage's start moving with whichever pixel shader it was paired with; one that matches
	// the start but not the specialization is named by the first field that differs. "table offset"
	// must stay at zero - the table offsets are directory slots now and cannot differ for one
	// program, so a count there means that regression is back.
	static void ReportPermutationMiss(const std::vector<Permutation>& permutations,
	                                  const ShaderRecompiler::IR::ResourceSpecialization& wanted,
	                                  uint32_t push_data_cursor) {
		if (permutations.empty()) {
			return;
		}
		bool        push_data_only = false;
		const char* difference     = nullptr;
		for (const auto& candidate: permutations) {
			const auto& layout = candidate.program.bindings;
			const bool  start_matches =
			    layout.push_data_start_dword == ShaderRecompiler::IR::PushData::StartFor(
			                                        push_data_cursor, layout.ShaderDataDwords());
			if (ShaderRecompiler::IR::SpecializationServes(candidate.specialization, wanted)) {
				// Same module state, rejected only for where its push data starts.
				push_data_only = true;
				break;
			}
			if (start_matches && difference == nullptr) {
				difference = ShaderRecompiler::IR::FirstSpecializationDifference(
				    candidate.specialization, wanted);
			}
		}
		static std::mutex                      mutex;
		static uint64_t                        push_data_misses      = 0;
		static uint64_t                        specialization_misses = 0;
		static uint64_t                        reported              = 0;
		static std::map<std::string, uint64_t> fields;
		std::string                            line;
		{
			const std::lock_guard<std::mutex> lock(mutex);
			if (push_data_only) {
				push_data_misses++;
			} else {
				specialization_misses++;
				fields[difference != nullptr ? difference : "unclassified"]++;
			}
			const auto total = push_data_misses + specialization_misses;
			// First sighting and every doubling, so a run that misses thousands of times does not
			// pay for a line each one.
			if ((total & (total - 1u)) != 0u || total == reported) {
				return;
			}
			reported = total;
			line = fmt::format("PermutationMiss: push_data={} specialization={}", push_data_misses,
			                   specialization_misses);
			for (const auto& [field, count]: fields) {
				line += fmt::format(" | {}={}", field, count);
			}
		}
		std::printf("%s\n", line.c_str());
	}

	explicit ProgramCache(vk::Device device, uint32_t subgroup_size, bool robust_buffer_access2)
	    : host_subgroup_size(subgroup_size == 0u ? 64u : subgroup_size),
	      robust_buffer_access2(robust_buffer_access2), device(device) {
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

	// Identifies a shader by the words it is made of. The declared hash is not enough on its own:
	// it is the guest's, and guest code can be rewritten under one. Hashing here is free against
	// what it guards - the only caller is about to spend hundreds of milliseconds translating.
	static uint64_t ShaderCodeKey(std::span<const uint32_t> code,
	                              std::span<const uint32_t> back_code) {
		auto key = XXH3_64bits(code.data(), code.size_bytes());
		if (!back_code.empty()) {
			// A merged-stage program decodes both halves into one graph, so the back half is
			// part of what the graph is a function of.
			const auto back = XXH3_64bits(back_code.data(), back_code.size_bytes());
			key             = XXH3_64bits_withSeed(&back, sizeof(back), key);
		}
		return key;
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	// What draws have proved about a shader's descriptors, keyed by the code words rather than by
	// the declared hash: guest code is rewritten under one, and a pc recorded against the old code
	// names nothing in the new. UnfoldableSet, Learn and RefuseRebuild live in unfoldableSet.h so
	// the rollback can be tested without a device.
	UnfoldableSets unfoldable;
	// Keyed on the whole ProgramKey: see ReportSkipped. `reported_shaders` stays on the hash, so
	// the log still carries one line per shader however many of its shapes refuse.
	std::unordered_set<ProgramKey, ProgramKeyHash> skipped_shaders;
	// Log throttle only: a hash here has been reported once, whether the cause was permanent or
	// transient. Kept apart from skipped_shaders so a transient failure does not disable a shader.
	std::unordered_set<uint64_t> reported_shaders;
	std::unordered_set<uint64_t> stubbed_shaders;
	std::unordered_set<uint64_t> call_stubbed_shaders;
	std::unordered_set<uint64_t> relearned_shaders;
	ProgramKey                   lookup_key;
	// A vertex or pixel stage has no workgroup input to carry the host subgroup width, and the
	// translator needs it to know whether a wave64 guest mask has an upper half at all.
	uint32_t   host_subgroup_size = 64;
	// See CompileOptions::robust_buffer_access2.
	bool       robust_buffer_access2 = false;
	vk::Device device;
	uint64_t   next_shader_id = 0;
};

template <typename Key, typename KeyHash>
PipelineLibraryCache<Key, KeyHash>::~PipelineLibraryCache() {
	Common::LockGuard lock(m_mutex);
	LogHitRate();
	for (const auto& [key, entry]: m_entries) {
		(void)key;
		m_graphics.device.destroyPipeline(entry->library, nullptr);
		m_graphics.device.destroyPipelineLayout(entry->layout, nullptr);
		m_graphics.device.destroyDescriptorSetLayout(entry->set_layout, nullptr);
	}
	m_entries.clear();
}

template <typename Key, typename KeyHash>
const typename PipelineLibraryCache<Key, KeyHash>::Entry*
PipelineLibraryCache<Key, KeyHash>::Find(const Key& key) {
	Common::LockGuard lock(m_mutex);
	const auto        iter = m_entries.find(key);
	if (iter == m_entries.end()) {
		m_misses++;
		MaybeLogHitRateLocked();
		return nullptr;
	}
	m_hits++;
	MaybeLogHitRateLocked();
	return iter->second.get();
}

template <typename Key, typename KeyHash>
const typename PipelineLibraryCache<Key, KeyHash>::Entry*
PipelineLibraryCache<Key, KeyHash>::Insert(const Key& key, const Entry& entry) {
	Common::LockGuard lock(m_mutex);
	const auto        iter = m_entries.find(key);
	if (iter != m_entries.end()) {
		// Another thread built the same fragment subset while this one was inside the driver. Its
		// entry is already published and may already be linked into a pipeline, so this one is the
		// loser and is destroyed here rather than leaked.
		m_graphics.device.destroyPipeline(entry.library, nullptr);
		m_graphics.device.destroyPipelineLayout(entry.layout, nullptr);
		m_graphics.device.destroyDescriptorSetLayout(entry.set_layout, nullptr);
		return iter->second.get();
	}
	auto [inserted_iter, inserted] = m_entries.emplace(key, std::make_unique<Entry>(entry));
	EXIT_IF(!inserted);
	return inserted_iter->second.get();
}

template <typename Key, typename KeyHash>
void PipelineLibraryCache<Key, KeyHash>::MaybeLogHitRateLocked() const {
	const auto total = m_hits + m_misses;
	if (total == 0 || (total & (total - 1u)) != 0u || total == m_reported) {
		return;
	}
	m_reported = total;
	LogHitRate();
}

template <typename Key, typename KeyHash>
void PipelineLibraryCache<Key, KeyHash>::LogHitRate() const {
	const auto total = m_hits + m_misses;
	if (total == 0) {
		return;
	}
	const auto since_built  = m_misses - m_reported_misses;
	const auto since_reused = m_hits - m_reported_hits;
	const auto since_total  = since_built + since_reused;
	m_reported_hits         = m_hits;
	m_reported_misses       = m_misses;
	// Note 156 estimated 34-47 % of a run's fragment libraries would be repeats, from a static
	// sweep of the dumped shader corpus. This is that estimate measured on the run that just ran.
	std::printf("%s: built %" PRIu64 " | reused %" PRIu64 " | lookups %" PRIu64
	            " | hit rate %.1f%% | since last: built %" PRIu64 " reused %" PRIu64 " (%.1f%%)\n",
	            m_name, m_misses, m_hits, total,
	            100.0 * static_cast<double>(m_hits) / static_cast<double>(total), since_built,
	            since_reused,
	            since_total == 0
	                ? 0.0
	                : 100.0 * static_cast<double>(since_reused) / static_cast<double>(since_total));
	std::fflush(stdout);
}

template class PipelineLibraryCache<FragmentLibraryKey, FragmentLibraryKeyHash>;
template class PipelineLibraryCache<PreRasterLibraryKey, PreRasterLibraryKeyHash>;

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics),
      m_program_cache(std::make_unique<ProgramCache>(graphics.device, graphics.subgroup_size,
                                                     graphics.robust_buffer_access2_enabled)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	EnsurePipelineStallWatchdog();
	InitializeDriverCache();
	if (graphics.pipeline_library_fast_linking) {
		m_libraries = std::make_unique<PipelineLibraries>(graphics);
	}
}

void PipelineCache::DestroyPipelineObjects(const Pipeline& pipeline) {
	// The pre-rasterization and fragment libraries are shared and owned by PipelineLibraries; these
	// two belong to this pipeline alone. Destroying a null handle is a no-op.
	m_graphics.device.destroyPipeline(pipeline.library_vertex_input, nullptr);
	m_graphics.device.destroyPipeline(pipeline.library_fragment_output, nullptr);
	m_graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(pipeline.pixel_descriptor_set_layout, nullptr);
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
	// After the pipelines, because a linked pipeline references its shared libraries.
	m_libraries.reset();
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
	// Still the CFG cache's stamp: a stored graph depends on the structurizer that built it.
	m_build_hash = EmulatorBinaryHash();

	const std::filesystem::path folder("_PipelineCache");
	InitializeCfgCache(folder, title_id);
	const auto driver_key = DriverCacheKey(m_graphics.GetPhysicalDeviceProperties());
	const auto name       = fmt::format("{}-{:016x}.bin", title_id, driver_key);
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
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
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
	FlushCfgCacheIfDirtyLocked();
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

// m_build_hash stamps the file: any change to CFG::BuildGraph, CFG::Structurize or CFG::Graph's
// layout implies a different executable. 0 refuses the file outright.
void PipelineCache::InitializeCfgCache(const std::filesystem::path& folder,
                                       const std::string&           title_id) {
	if (m_build_hash == 0) {
		PipelineCacheLog("Shader CFG cache: disabled (binary could not be fingerprinted)");
		return;
	}
	const auto path = folder / "cfg" / fmt::format("{}.cfgcache", title_id);
	ShaderRecompiler::OpenCfgCacheFile(Common::PathToString(path), m_build_hash);
	m_cfg_cache_enabled = true;
	const auto stats    = ShaderRecompiler::CfgCacheStatistics();
	PipelineCacheLog("Shader CFG cache: {} stamp={:016x} loaded={} rejected={}",
	                 Common::PathToString(path), m_build_hash, stats.loaded, stats.rejected);
}

void PipelineCache::FlushCfgCacheIfDirtyLocked() {
	if (!m_cfg_cache_enabled) {
		return;
	}
	const auto stats = ShaderRecompiler::CfgCacheStatistics();
	if (stats.misses == m_cfg_flushed_misses) {
		return;
	}
	m_cfg_flushed_misses = stats.misses;
	ShaderRecompiler::FlushCfgCacheFile();
	if (CfgCacheLogEnabled()) {
		const auto written = ShaderRecompiler::CfgCacheStatistics();
		PipelineCacheLog(
		    "Shader CFG cache: wrote {} entries, {} bytes held, avoided {} ms this run ({} ms of "
		    "it from the file)",
		    written.stored, written.bytes,
		    (written.avoided_file_us + written.avoided_memory_us) / 1000u,
		    written.avoided_file_us / 1000u);
	}
}

void PipelineCache::MaybeFlushCfgCacheLocked() {
	if (!m_cfg_cache_enabled) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - m_last_cfg_flush < CfgCacheFlushPeriod) {
		return;
	}
	m_last_cfg_flush = now;
	FlushCfgCacheIfDirtyLocked();
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
		if (result == vk::Result::eSuccess && size > MaxDriverCacheBytes) {
			static bool reported = false;
			if (!std::exchange(reported, true)) {
				PipelineCacheLog("Vulkan pipeline cache: not saving, {} bytes is over the {} byte "
				                 "budget (the file already on disk is kept)",
				                 size, MaxDriverCacheBytes);
			}
			return false;
		}
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
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)", vk::to_string(result),
		                 size);
		return false;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
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
	for (auto& stage: vertex_info) {
		stage.param_duplicate_mask = 0;
	}
	static const bool share_reads = !EnvSwitch("KYTY_NO_SHARED_STAGE_READS");
	ShaderReadCache().Reset();
	t_stage_reads_shared = share_reads;
	struct ShareEnd {
		~ShareEnd() { t_stage_reads_shared = false; }
	} share_end;
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
			static std::mutex                   mutex;
			static std::unordered_set<uint64_t> reported;
			const std::lock_guard<std::mutex>   lock(mutex);
			if (reported.insert(vertex_params[0].hash).second) {
				PipelineCacheLog("mesh shader exceeds host limits, skipping draws: hash=0x{:016x} "
				                 "threads={} vertices={} primitives={} LDS={}",
				                 vertex_params[0].hash, host_threads, mesh.max_vertices,
				                 mesh.max_primitives, mesh.lds_size_dwords);
			}
			return {};
		}
	}
	ShaderParams pixel_params;
	bool         replay_stencil_export = false;
	if (pixel_active) {
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		if (!mesh_active && !tess_active && Prospero::IsRectList(user_config.GetPrimType())) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::Rectangle;
		} else if (context.GetModeControl().provoking_vtx_last) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::LastVertex;
		} else {
			pixel_info.parameter_mode = ShaderPixelParameterMode::FirstVertex;
		}
		const auto& blend = context.GetBlendControl(0);
		const bool  export0_to_cb0 = ShaderPixelExportTarget(pixel_info.target_shader_mask, 0) == 0;
		pixel_info.dual_source_blending =
		    export0_to_cb0 && blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
		} else if (export0_to_cb0 && blend.enable &&
		           !context.GetRenderTarget(0).info.blend_bypass &&
		           pixel_info.target_output_mode[0] != 0 && pixel_info.target_output_mode[0] != 7 &&
		           std::all_of(std::begin(pixel_info.target_output_mode) + 1,
		                       std::end(pixel_info.target_output_mode),
		                       [](uint8_t mode) { return mode == 0; })) {
			switch (ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0])) {
				case BlendMappingSupport::SourceAlpha:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlpha;
					break;
				case BlendMappingSupport::SourceAlphaOne:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaOne;
					break;
				case BlendMappingSupport::SourceAlphaZero:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaZero;
					break;
				default: break;
			}
			if (pixel_info.alpha_blend_source != ShaderAlphaBlendSource::None) {
				pixel_info.dual_source_blending     = true;
				pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
				pixel_info.target_export_mapping[1] = {};
			}
		}
		// NVIDIA has never exposed VK_EXT_shader_stencil_export, so a guest shader that
		// writes a stencil reference has to fall back to a register-stamped plane.
		if (!m_graphics.shader_stencil_export_enabled &&
		    (pixel_info.ps_stencil_test_val_export_enable ||
		     pixel_info.ps_stencil_op_val_export_enable)) {
			replay_stencil_export = pixel_info.ps_stencil_op_val_export_enable &&
			                        !pixel_info.ps_stencil_test_val_export_enable && !mesh_active;
#if defined(__APPLE__)
			// The replay turns colour writes off through VK_EXT_color_write_enable.
			replay_stencil_export = false;
#endif
			pixel_info.ps_stencil_test_val_export_enable = false;
			pixel_info.ps_stencil_op_val_export_enable   = false;
			static std::atomic<bool> reported {false};
			if (!reported.exchange(true, std::memory_order_relaxed)) {
				LOGF("PipelineCache: guest asks for a shader stencil reference but %s is not "
				     "exposed by the selected device (%s); %s\n",
				     VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME,
				     m_graphics.GetPhysicalDeviceProperties().deviceName.data(),
				     replay_stencil_export
				         ? "an exported op value is replayed into the plane one bit per pass"
				         : "stencil-gated passes will test a register-stamped plane");
			}
		}
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
	uint32_t push_data_cursor =
	    mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms result;
	if (pixel_active) {
		const auto pixel_cursor = push_data_cursor;
		result.pixel            = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
		if (!result.pixel) {
			return {};
		}
		if (replay_stencil_export) {
			const auto& sm = context.GetStencilMask();
			const auto  back =
			    context.GetDepthControl().backface_enable ? sm.stencil_writemask_bf : uint8_t {0};
			const auto wants = static_cast<uint8_t>(sm.stencil_writemask | back);
			GetStencilBitPrograms(pixel_params, pixel_info, pixel_cursor, push_data_cursor, wants,
			                      result);
		}
		if (pixel_info.stage.program != nullptr) {
			std::array<uint32_t, 32> active {};
			uint32_t                 active_count = 0;
			for (const auto& input: pixel_info.stage.program->info.inputs) {
				if (input.kind == ShaderRecompiler::IR::StageInputKind::Parameter &&
				    active_count < active.size()) {
					active[active_count++] = input.location;
				}
			}
			auto& last = vertex_info[tess_active ? 2u : 0u];
			ShaderPixelParameterDuplicates(pixel_info, {active.data(), active_count},
			                               last.param_duplicate_mask, last.param_duplicate_source);
		}
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] = m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor);
	}
	return result;
}

void PipelineCache::GetStencilBitPrograms(const ShaderParams&         pixel_params,
                                          const ShaderPixelInputInfo& pixel_info,
                                          uint32_t pixel_cursor, uint32_t next_cursor, uint8_t bits,
                                          GraphicsPrograms& result) {
	const auto refuse = [&] { result.stencil_bit_mask = 0; };
	if (bits == 0) {
		return;
	}
	const auto& base = *pixel_info.stage.program;
	if (HasShaderBufferWrites(pixel_info.stage) ||
	    std::ranges::any_of(base.info.images, [](const auto& image) { return image.written; }) ||
	    base.bindings.descriptor_counts[static_cast<size_t>(
	        ShaderRecompiler::IR::DescriptorBindingKind::Gds)] != 0) {
		refuse();
		return;
	}
	for (uint32_t bit = 0; bit < 8; bit++) {
		if ((bits & (1u << bit)) == 0) {
			continue;
		}
		auto variant                = pixel_info;
		variant.stage               = {};
		variant.ps_stencil_bit_pass = static_cast<uint8_t>(bit + 1u);
		uint32_t   cursor           = pixel_cursor;
		const auto program          = m_program_cache->Get(pixel_params, variant, cursor);
		if (!program || !variant.stage) {
			refuse();
			return;
		}
		if (cursor != next_cursor || !(variant.stage.program->bindings == base.bindings)) {
			refuse();
			return;
		}
		result.stencil_bit_pixel[bit] = program;
		result.stencil_bit_stage[bit] = variant.stage;
		result.stencil_bit_mask |= static_cast<uint8_t>(1u << bit);
	}
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	KYTY_PROFILER_FUNCTION();
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto params             = PrepareProgram(regs, sh, input_info);
	input_info.lds_storage        = input_info.lds_size_dwords * 4u >
	                         m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	uint32_t push_data_cursor = input_info.dispatch_thread_dimensions
	                                ? ShaderRecompiler::IR::PushData::DispatchLimitDwordCount
	                                : 0u;
	return m_program_cache->Get(params, input_info, push_data_cursor);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

// Field by field rather than memcmp: the struct has padding, and a library served on a padding
// byte would be served on nothing at all.
bool FragmentLibraryKey::operator==(const FragmentLibraryKey& other) const noexcept {
	return ps_shader_id == other.ps_shader_id && push_stages == other.push_stages &&
	       samples == other.samples && sample_shading_enable == other.sample_shading_enable &&
	       stencil_test_enable == other.stencil_test_enable &&
	       std::memcmp(&stencil_front, &other.stencil_front, sizeof(stencil_front)) == 0 &&
	       std::memcmp(&stencil_back, &other.stencil_back, sizeof(stencil_back)) == 0 &&
	       depth_format == other.depth_format && stencil_format == other.stencil_format;
}

// The same mix PipelineCache uses for its own keys, repeated here because that one is a private
// nested type and this hash is not a member of anything.
static void MixFragmentKey(std::size_t& hash, std::size_t value) {
	hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) + (hash >> 2u);
}

std::size_t FragmentLibraryKeyHash::operator()(const FragmentLibraryKey& key) const noexcept {
	std::size_t hash = 0;
	MixFragmentKey(hash, static_cast<std::size_t>(key.ps_shader_id));
	MixFragmentKey(hash, key.push_stages);
	MixFragmentKey(hash, key.samples);
	MixFragmentKey(hash, static_cast<std::size_t>(key.sample_shading_enable) |
	                         (static_cast<std::size_t>(key.stencil_test_enable) << 1u));
	const auto* front = reinterpret_cast<const uint8_t*>(&key.stencil_front);
	for (std::size_t i = 0; i < sizeof(key.stencil_front); i++) {
		MixFragmentKey(hash, front[i]);
	}
	const auto* back = reinterpret_cast<const uint8_t*>(&key.stencil_back);
	for (std::size_t i = 0; i < sizeof(key.stencil_back); i++) {
		MixFragmentKey(hash, back[i]);
	}
	MixFragmentKey(hash, static_cast<std::size_t>(key.depth_format));
	MixFragmentKey(hash, static_cast<std::size_t>(key.stencil_format));
	return hash;
}

std::size_t PreRasterLibraryKeyHash::operator()(const PreRasterLibraryKey& key) const noexcept {
	std::size_t hash = 0;
	for (const auto id: key.vertex_shader_ids) {
		MixFragmentKey(hash, static_cast<std::size_t>(id));
	}
	MixFragmentKey(hash, static_cast<std::size_t>(key.rect_list_ps_shader_id));
	MixFragmentKey(hash, key.push_stages);
	MixFragmentKey(hash, key.patch_control_points);
	MixFragmentKey(hash, static_cast<std::size_t>(key.rect_list) |
	                         (static_cast<std::size_t>(key.negative_one_to_one) << 1u) |
	                         (static_cast<std::size_t>(key.depth_clip_enable) << 2u) |
	                         (static_cast<std::size_t>(key.cull_front) << 3u) |
	                         (static_cast<std::size_t>(key.cull_back) << 4u) |
	                         (static_cast<std::size_t>(key.face) << 5u) |
	                         (static_cast<std::size_t>(key.provoking_vtx_last) << 6u));
	MixFragmentKey(hash, static_cast<std::size_t>(key.polygon_mode));
	return hash;
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

	auto& ctx = command.GetRegisters();

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
		const auto& rt           = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc           = ctx.GetBlendControl(colors[i].target_slot);
		auto        alpha_source = ShaderAlphaBlendSource::None;
		if (slot == 0 && ps_input_info != nullptr) {
			alpha_source = ps_input_info->alpha_blend_source;
		}
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && alpha_source == ShaderAlphaBlendSource::None &&
		    ClassifyBlendMapping(bc, colors[i].export_mapping) != BlendMappingSupport::Direct) {
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned   = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: blending disabled for unsupported color mapping "
				    "(slot={} mapping=0x{:02x} color={}/{} alpha={}/{} separate={}).\n",
				    slot, colors[i].export_mapping.packed, bc.color_srcblend, bc.color_destblend,
				    bc.alpha_srcblend, bc.alpha_destblend, bc.separate_alpha_blend ? 1 : 0));
			}
		}
		if (static_params.blend_enable[slot]) {
			auto blend = bc;
			switch (alpha_source) {
				case ShaderAlphaBlendSource::SourceAlpha:
					blend.color_srcblend  = RemapSourceAlphaFactor(blend.color_srcblend);
					blend.color_destblend = RemapSourceAlphaFactor(blend.color_destblend);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::SourceAlphaOne:
				case ShaderAlphaBlendSource::SourceAlphaZero:
					// The second source carries the mapped source factor; its alpha stays logical Sa.
					blend.color_srcblend = static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Color);
					blend.color_destblend =
					    static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::None: break;
			}
			static_params.color_srcblend[slot]       = blend.color_srcblend;
			static_params.color_comb_fcn[slot]       = blend.color_comb_fcn;
			static_params.color_destblend[slot]      = blend.color_destblend;
			static_params.separate_alpha_blend[slot] = blend.separate_alpha_blend;
			if (blend.separate_alpha_blend) {
				static_params.alpha_srcblend[slot]  = blend.alpha_srcblend;
				static_params.alpha_comb_fcn[slot]  = blend.alpha_comb_fcn;
				static_params.alpha_destblend[slot] = blend.alpha_destblend;
			}
		}
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
	const bool rect_list             = Prospero::IsRectList(command.GetUserConfig().GetPrimType());
	static_params.cull_back          = !rect_list && mc.cull_back;
	static_params.cull_front         = !rect_list && mc.cull_front;
	static_params.face               = mc.face;
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
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer                 = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			EXIT_IF(binding < 0 || binding >= vs_input_info.buffers_num);
			key.vertex_input.attributes[attribute] = {
			    .offset  = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                     vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
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
		                       ps_input_info, programs, static_params, m_driver_cache,
		                       m_libraries.get());
	}

	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);
	MaybeSaveDriverCacheLocked();
	MaybeFlushCfgCacheLocked();

	return *iter->second;
}

PipelineCache::Pipeline& PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                                           const ShaderProgram& compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

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
	MaybeFlushCfgCacheLocked();

	return *iter->second;
}
} // namespace Libs::Graphics
