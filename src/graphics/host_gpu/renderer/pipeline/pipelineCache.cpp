#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hackNotice.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/execHelpers.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/nativeRayTracing.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/Tessellation.h"
#include "graphics/shader/recompiler/ir/Serialize.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

constexpr auto DriverCacheSaveInterval = std::chrono::minutes {1};
// After a pipeline that took this long to create, the driver cache is written sooner, but no more
// often than SlowCreationSaveInterval: the whole cache is written each time (hundreds of MB for some
// games), and with cold caches (a manifest replayed while the game loads) nearly every creation is
// slow.
constexpr auto SlowCreation             = std::chrono::seconds {5};
constexpr auto SlowCreationSaveInterval = std::chrono::seconds {20};
constexpr auto DriverCacheTimerPeriod   = std::chrono::milliseconds {250};
// While a dispatch waits for a helper compiling its pipeline (see PipelineHelpers): how often it
// looks for the result. It sleeps, so that its core goes to the helper.
constexpr auto HelperPoll = std::chrono::milliseconds {1};

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
	// PA_SU_SC_MODE_CNTL.POLY_MODE disables both per-face modes when it is zero.
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

// Whether the dispatch of a patched program reads a null TLAS header address from its flattened
// SRT (ShaderInfo::ray_query_header_slots): it has nothing to trace with the host structures.
bool ReadsNullTlas(const ShaderModule& module, const ShaderRecompiler::IR::ResourceSnapshot& resources) {
	const auto& info  = module.info.info;
	const auto& slots = info.ray_query_header_slots;
	const auto& flat  = resources.flattened_srt;
	return info.ray_query && slots[0] < flat.size() && slots[1] < flat.size() && flat[slots[0]] == 0 &&
	       flat[slots[1]] == 0;
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	// Driver cache entries already distinguish shader code and pipeline state across builds.
	return fmt::format("KytyPC2:{:08x}:{:08x}:{:08x}:{}\n",
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

bool ReadAheadStrict(void* userdata, uint64_t address, std::span<uint32_t> values) {
	return !values.empty() && static_cast<GuestReadLog*>(userdata)->Read(address, values.data(),
	                                                                     values.size_bytes(), true);
}

bool ReadAheadShaderState(void* userdata, uint64_t address, void* data, uint64_t size) {
	// A table pointer below guest memory (0x40000) is null: the speculative walk follows both sides
	// of a branch it cannot decide and reads through it on the side the shader never takes.
	// Reading zero there is what the speculative walk means, as in upstream's x86 walker, and the
	// bytes are zero for every thread, so they need neither the backing nor a validation entry.
	if (address < 0x40000u && size != 0u) {
		std::memset(data, 0, size);
		return true;
	}
	return static_cast<GuestReadLog*>(userdata)->Read(address, data, size, false);
}

// The buffer ranges the dispatches and draws resolved lately ahead of execution write (resolution
// thread only): the GPU produces their bytes, and a walk ahead does not read there a load that
// only decides control flow (see SrtRuntime::gpu_produced), as the writer may not have run yet.
// Walks ask for most such loads, nearly all of them outside these ranges: a counting filter of
// the 64 KB blocks the ranges cover answers those without scanning the ranges.
struct AheadWrites {
	static constexpr size_t   Capacity        = 256;
	static constexpr uint64_t BlockShift      = 16;
	static constexpr size_t   FilterBits      = 12;
	static constexpr uint64_t MaxFilterBlocks = 4096; // Wider ranges (256 MB) are always scanned.
	std::array<std::pair<uint64_t, uint64_t>, Capacity> ranges {};
	std::array<uint16_t, size_t {1} << FilterBits>     filter {};
	uint32_t                                            wide = 0;
	size_t                                              next = 0;

	static size_t Slot(uint64_t block) {
		return static_cast<size_t>((block * 0x9e3779b97f4a7c15ull) >> (64u - FilterBits));
	}
	void Count(uint64_t begin, uint64_t end, int delta) {
		if (begin >= end) return;
		const auto first = begin >> BlockShift;
		const auto last  = (end - 1) >> BlockShift;
		if (last - first >= MaxFilterBlocks) {
			wide = static_cast<uint32_t>(static_cast<int>(wide) + delta);
			return;
		}
		for (auto block = first; block <= last; block++) {
			auto& count = filter[Slot(block)];
			count       = static_cast<uint16_t>(count + delta);
		}
	}
	void Add(uint64_t begin, uint64_t end) {
		for (const auto& [b, e]: ranges) {
			if (b == begin && e == end) return;
		}
		Count(ranges[next].first, ranges[next].second, -1);
		ranges[next] = {begin, end};
		Count(begin, end, 1);
		next = (next + 1) % Capacity;
	}
	[[nodiscard]] bool Contains(uint64_t address) const {
		if (wide == 0 && filter[Slot(address >> BlockShift)] == 0) return false;
		for (const auto& [begin, end]: ranges) {
			if (address >= begin && address < end) return true;
		}
		return false;
	}
};
thread_local AheadWrites t_ahead_writes;

// Guest memory writes with known bytes (WRITE_DATA) of operations the resolution thread passed
// and the execution thread has not run yet, oldest first: walks ahead of later operations read
// their bytes, which the backing store gets only when they execute. GTA V's ray tracing writes
// the parameters of each BVH build pass (cs 5c02d7970bd792ec, f85e944c4ed88b61) with WRITE_DATA
// just before its dispatch, over the previous frame's.
struct AheadGuestWrites {
	struct Write {
		uint64_t              sequence = 0;
		uint64_t              address  = 0;
		std::vector<uint32_t> dwords;

		[[nodiscard]] uint64_t End() const { return address + dwords.size() * sizeof(uint32_t); }
	};
	static constexpr size_t   FilterBits = 12;
	static constexpr uint64_t PageShift  = 12;
	std::deque<Write>                              writes;
	// How many writes touch each 4 KB page, by hash: most reads touch none.
	std::array<uint16_t, size_t {1} << FilterBits> filter {};

	static size_t Slot(uint64_t page) {
		return static_cast<size_t>((page * 0x9e3779b97f4a7c15ull) >> (64u - FilterBits));
	}
	void Count(const Write& write, int delta) {
		for (auto page = write.address >> PageShift; page <= (write.End() - 1) >> PageShift; page++) {
			auto& count = filter[Slot(page)];
			count       = static_cast<uint16_t>(count + delta);
		}
	}
	[[nodiscard]] bool MayTouch(uint64_t address, uint64_t size) const {
		if (writes.empty() || size == 0) return false;
		for (auto page = address >> PageShift; page <= (address + size - 1) >> PageShift; page++) {
			if (filter[Slot(page)] != 0) return true;
		}
		return false;
	}
	// Writes the bytes of [address, address + size) that the writes leave, oldest first, into
	// `data`; false when none touches the range.
	bool Apply(uint64_t address, void* data, uint64_t size) const {
		if (!MayTouch(address, size)) return false;
		bool applied = false;
		for (const auto& write: writes) {
			const auto begin = std::max(address, write.address);
			const auto end   = std::min(address + size, write.End());
			if (begin >= end) continue;
			std::memcpy(static_cast<uint8_t*>(data) + (begin - address),
			            reinterpret_cast<const uint8_t*>(write.dwords.data()) + (begin - write.address),
			            end - begin);
			applied = true;
		}
		return applied;
	}
};
thread_local AheadGuestWrites t_ahead_guest_writes;

bool AheadPendingWrite(uint64_t address, uint32_t& value) {
	auto& state = t_ahead_guest_writes;
	if (!state.MayTouch(address, sizeof(value))) return false;
	if (!Libs::LibKernel::Memory::TryReadBacking(address, &value, sizeof(value))) value = 0;
	return state.Apply(address, &value, sizeof(value));
}
// Whether the walk of the stage being resolved left a GPU-produced load unread.
thread_local bool t_ahead_unread_load = false;

// Bounds tested for zero that the execution thread found on pages the GPU owns (see
// GuestReadLog::ZeroTestHolds()): a walk ahead of execution reads them before their writer runs.
// Few addresses: GTA V's ray tracing passes count their work lists at fixed addresses, which
// dispatches write on the GPU that a walk ahead may not have resolved.
struct GpuProducedBounds {
	static constexpr size_t                     Capacity = 64;
	std::array<std::atomic<uint64_t>, Capacity> addresses {};
	std::atomic<uint32_t>                       count {0};

	[[nodiscard]] bool Contains(uint64_t address) const {
		const auto known = std::min<size_t>(count.load(std::memory_order_acquire), Capacity);
		for (size_t i = 0; i < known; i++) {
			if (addresses[i].load(std::memory_order_relaxed) == address) return true;
		}
		return false;
	}
	// Execution thread only.
	void Add(uint64_t address) {
		if (Contains(address)) return;
		const auto index = count.load(std::memory_order_relaxed);
		addresses[index % Capacity].store(address, std::memory_order_relaxed);
		count.store(index + 1, std::memory_order_release);
	}
};
GpuProducedBounds g_gpu_produced_bounds;

void LearnGpuProducedBound(uint64_t address) {
	g_gpu_produced_bounds.Add(address);
}

bool AheadGpuProduced(uint64_t address) {
	if (!t_ahead_writes.Contains(address) && !g_gpu_produced_bounds.Contains(address)) return false;
	t_ahead_unread_load = true;
	return true;
}

void NoteAheadWrites(const ShaderRecompiler::IR::ResourcePlan&     plan,
                     const ShaderRecompiler::IR::ResourceSnapshot& resources) {
	for (size_t i = 0; i < plan.info.buffers.size() && i < resources.buffers.size(); i++) {
		const auto& descriptor = resources.buffers[i];
		if (!plan.info.buffers[i].written || descriptor.dword_count != 4u) continue;
		// The V#'s base and size (NUM_RECORDS records of STRIDE bytes, or bytes without a stride).
		const uint64_t base    = descriptor.dwords[0] | (uint64_t {descriptor.dwords[1] & 0xffffu} << 32u);
		const uint32_t stride  = (descriptor.dwords[1] >> 16u) & 0x3fffu;
		const uint64_t size    = uint64_t {descriptor.dwords[2]} * (stride != 0u ? stride : 1u);
		if (base != 0 && size != 0) t_ahead_writes.Add(base, base + size);
	}
}

void RecordAheadLoad(void* userdata, uint64_t address, uint32_t value, uint32_t flat_slot) {
	static_cast<GuestReadLog*>(userdata)->RecordLoad(address, value, flat_slot);
}

class ProgramLockGuard {
public:
	explicit ProgramLockGuard(std::atomic_flag& flag): m_flag(flag) {
		while (m_flag.test_and_set(std::memory_order_acquire)) {
			while (m_flag.test(std::memory_order_relaxed)) {
				Common::SpinPause();
			}
		}
	}
	~ProgramLockGuard() { m_flag.clear(std::memory_order_release); }
	ProgramLockGuard(const ProgramLockGuard&)            = delete;
	ProgramLockGuard& operator=(const ProgramLockGuard&) = delete;

private:
	std::atomic_flag& m_flag;
};

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	// Scalar and unformatted buffer dependencies use the same backing as native raw loads.
	// Image synchronization belongs to formatted buffer bindings, not these reads.
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(), values.size_bytes());
}

void DumpShader(std::span<const uint32_t> words, const std::string& name) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	const auto path = Config::GetShaderLogFolder() / name;
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(words.data(), words.size_bytes());
}

void ValidateShaderSpirv(const char* stage, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv, bool maintenance8_enabled) {
	if (!Config::ShaderValidationEnabled()) {
		return;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	spvtools::ValidatorOptions validator_options;
	validator_options.SetAllowOffsetTextureOperand(maintenance8_enabled);
	if (tools.Validate(spirv.data(), spirv.size(), validator_options)) {
		return;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF("%s\n", text.c_str());
	EXIT("ShaderRecompiler %s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s", stage,
	     shader_hash, messages.c_str());
}

// Formats with a narrower host variant drop the components the shader does not read.
vk::Format NarrowInputFormat(vk::Format format, uint32_t used_components) {
	switch (format) {
		case vk::Format::eR32G32B32A32Sfloat:
			switch (used_components) {
				case 1: return vk::Format::eR32Sfloat;
				case 2: return vk::Format::eR32G32Sfloat;
				case 3: return vk::Format::eR32G32B32Sfloat;
				default: return format;
			}
		case vk::Format::eR32G32B32Sfloat:
			switch (used_components) {
				case 1: return vk::Format::eR32Sfloat;
				case 2: return vk::Format::eR32G32Sfloat;
				default: return format;
			}
		case vk::Format::eR16G16B16A16Sfloat:
			switch (used_components) {
				case 1: return vk::Format::eR16Sfloat;
				case 2: return vk::Format::eR16G16Sfloat;
				default: return format;
			}
		case vk::Format::eR8G8B8A8Unorm:
			switch (used_components) {
				case 1: return vk::Format::eR8Unorm;
				case 2: return vk::Format::eR8G8Unorm;
				default: return format;
			}
		case vk::Format::eR8G8B8A8Snorm:
			return used_components == 2 ? vk::Format::eR8G8Snorm : format;
		case vk::Format::eR8G8B8A8Uint:
			switch (used_components) {
				case 1: return vk::Format::eR8Uint;
				case 2: return vk::Format::eR8G8Uint;
				default: return format;
			}
		default: return format;
	}
}

vk::Format InputFormat(const ShaderBufferResource& resource, uint32_t used_components) {
	switch (resource.RawFormat()) {
		// Temporary: invalid PS5 buffer format 113 (VertexAttribFormat::k16_16SInt).
		case 113: return NarrowInputFormat(vk::Format::eR32G32B32A32Sfloat, used_components);
		case 121: return vk::Format::eR16G16Sfloat;
		default: break;
	}
	const auto format = VulkanFormat(resource.Format());
	if (format == vk::Format::eUndefined ||
	    ShaderRecompiler::Format::GetFormatInfo(resource.Format()).component_count == 0) {
		EXIT("unknown vertex format: fmt = %u\n", resource.RawFormat());
	}
	return NarrowInputFormat(format, used_components);
}

// The host format of each vertex input attribute, narrowed to the components the stage reads.
std::vector<vk::Format> VertexInputFormats(const ShaderVertexInputInfo&                    input_info,
                                           const ShaderRecompiler::IR::CompiledShaderInfo& program) {
	std::vector<vk::Format> formats(input_info.resources_num);
	for (int index = 0; index < input_info.resources_num; index++) {
		const auto registers_num = input_info.resources_dst[index].registers_num;
		EXIT_IF(registers_num < 1 || registers_num > 4);
		const auto fetched = program.info.vertex_fetch_components[index];
		formats[index] = InputFormat(input_info.resources[index],
		                             fetched > 0 ? fetched : static_cast<uint32_t>(registers_num));
	}
	return formats;
}

// A shader whose call targets are GPU data runs the functions its call tables name: a table that
// does not expand (not mapped, a target that is not a leaf function) leaves nothing to run.
[[noreturn]] void ExitCallExpansionFailure(uint64_t hash, const ShaderRecompiler::ShaderSource& source) {
	EXIT("shader 0x%016" PRIx64 ": its call tables do not expand: %s\n", hash,
	     source.refresh_failure.c_str());
}

} // namespace

void AppendNativeRayTracingKey(ShaderType stage, std::vector<uint32_t>& key, bool native) {
	if (native && stage == ShaderType::Compute && NativeRayTracing::PatchShaders()) {
		key.push_back(0x52545041u);
		// They count the rays they send back to the guest traversal (--rt-stats).
		if (Config::RayTracingStatsEnabled()) {
			key.push_back(0x52545354u);
		}
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

ShaderRecompiler::Spirv::Profile SpirvProfile(const GraphicContext& graphics) {
	// AMD GPUs (vendor 0x1002) run NMin/NMax as v_min/v_max, which give the guest's NaN and
	// signed-zero results; GLSL.std.450 alone does not order -0 below +0.
	return {.maintenance8           = graphics.maintenance8_enabled,
	        .native_float_min_max   = graphics.physical_device_properties.vendorID == 0x1002u,
	        .device_clock_period_ps = graphics.device_clock_period_ps,
	        .robust_buffer_access   = graphics.robust_buffer_access_enabled,
	        .r8_srgb_views          = graphics.r8_srgb_views,
	        .r8g8_srgb_views        = graphics.r8g8_srgb_views,
	        .relaxed_float          = Config::RelaxedShaderFloatEnabled(),
	        .ray_query              = graphics.ray_query_enabled};
}

std::filesystem::path ShaderStorePath() {
	if (KYTY_BUILD != KYTY_BUILD_RELEASE || !Config::ShaderCacheEnabled() ||
	    Config::GraphicsDebugDumpEnabled()) {
		return {};
	}
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return {};
	}
	return std::filesystem::path("_PipelineCache") / (title_id + ".shaders");
}

void GuestReadLog::Append(uint64_t address, const void* data, uint32_t size, bool strict,
                          bool zero_test) {
	m_gpu_written = m_gpu_written || Libs::LibKernel::Memory::MayBeGpuWritten(address, size);
	const auto offset = static_cast<uint32_t>(m_bytes.size());
	m_bytes.insert(m_bytes.end(), static_cast<const uint8_t*>(data),
	               static_cast<const uint8_t*>(data) + size);
	// Contiguous reads of the same kind are checked as one (strict reads up to the size the
	// check reads at once).
	if (!m_entries.empty() && !zero_test) {
		auto& last = m_entries.back();
		if (last.strict == strict && !last.zero_test && last.address + last.size == address &&
		    last.offset + last.size == offset &&
		    (!strict || last.size + size <= StrictCheckBytes)) {
			last.size += size;
			return;
		}
	}
	m_entries.push_back({.address = address, .size = size, .offset = offset, .strict = strict,
	                     .zero_test = zero_test});
}

bool GuestReadLog::Read(uint64_t address, void* data, uint64_t size, bool strict) {
	if (size == 0 || size > UINT32_MAX ||
	    !Libs::LibKernel::Memory::TryReadBacking(address, data, size)) {
		m_failed = true;
		return false;
	}
	t_ahead_guest_writes.Apply(address, data, size);
	Append(address, data, static_cast<uint32_t>(size), strict);
	return true;
}

void GuestReadLog::RecordLoad(uint64_t address, uint32_t value, uint32_t flat_slot) {
	if (flat_slot == ShaderRecompiler::IR::ZeroTestSlot) {
		Append(address, &value, sizeof(value), false, true);
		return;
	}
	if (ShaderRecompiler::IR::IsDescriptorSlot(flat_slot)) {
		const auto source = ShaderRecompiler::IR::DescriptorSlotSource(flat_slot);
		const auto word   = ShaderRecompiler::IR::DescriptorSlotWord(flat_slot);
		const auto kind   = m_plan != nullptr && source < m_plan->descriptor_refresh.size()
		                        ? m_plan->descriptor_refresh[source]
		                        : ShaderRecompiler::IR::DescriptorRefresh::None;
		const auto offset = static_cast<uint32_t>(m_bytes.size());
		Append(address, &value, sizeof(value), false);
		const auto mask = ShaderRecompiler::IR::DescriptorRefreshMask(kind, word);
		if (mask != 0) {
			auto&      entry = m_entries.back();
			const auto index = static_cast<uint32_t>(m_words.size());
			m_words.push_back(
			    {.offset = offset, .stage = m_stage, .source = source, .word = word, .mask = mask});
			if (entry.words_begin == entry.words_end) {
				entry.words_begin = index;
			}
			entry.words_end = index + 1;
		}
		return;
	}
	if (flat_slot != ShaderRecompiler::IR::NoFlatSlot) {
		if (!m_flat_loads.empty()) {
			auto& last = m_flat_loads.back();
			const auto next = last.address + uint64_t {last.count} * sizeof(uint32_t);
			if (last.stage == m_stage && next == address && last.slot + last.count == flat_slot &&
			    (address & 0xfffu) != 0) {
				last.count++;
				return;
			}
		}
		m_flat_loads.push_back({address, m_stage, flat_slot, 1});
		return;
	}
	Append(address, &value, sizeof(value), false);
}

void GuestReadLog::RefreshFlatLoads(std::span<ShaderRecompiler::IR::ResourceSnapshot> resources,
                                    bool gpu_copies) const {
	for (auto& snapshot: resources) {
		snapshot.gpu_flat_loads.clear();
	}
	// As the execution thread's own walk loads: around the GPU's writes on the pages it owns,
	// directly elsewhere.
	// The loads of a draw alternate between a few pages, mostly those StillValid() just decided:
	// its answers still hold. A page it read back since no longer faults, and reading it around
	// the GPU's writes then finds no dirty bytes and takes the backing, which holds them.
	auto& checked = m_checked;
	for (const auto& load: m_flat_loads) {
		if (load.stage >= resources.size() ||
		    uint64_t {load.slot} + load.count > resources[load.stage].flattened_srt.size()) {
			continue;
		}
		const auto page_index = load.address >> 12u;
		auto&      page       = checked[page_index % checked.size()];
		if (page.page != page_index) {
			page.page    = page_index;
			page.owned   = Libs::LibKernel::Memory::MayFaultOnGpuRead(load.address & ~0xfffull, 4096);
			page.backing = page.owned ? Libs::LibKernel::Memory::FindAroundBackingPage(load.address)
			                          : nullptr;
		}
		auto*      values = resources[load.stage].flattened_srt.data() + load.slot;
		const auto bytes  = load.count * sizeof(uint32_t);
		// Bytes the GPU wrote to a cached buffer (no GPU-written image covers the page): reading
		// them here waits for the GPU (through the mirror or a readback). The shader reads them
		// when it runs, after the work that writes them: a copy on the GPU puts them there in time.
		if (gpu_copies && page.owned && page.backing != nullptr &&
		    Libs::LibKernel::Memory::HasGpuWrittenBytes(load.address, bytes)) {
			resources[load.stage].gpu_flat_loads.push_back({load.address, load.slot, load.count});
			continue;
		}
		const bool around =
		    page.owned &&
		    (page.backing != nullptr ? Libs::LibKernel::Memory::TryReadAroundGpuWritesOnPage(
		                                   load.address, values, bytes, page.backing)
		                             : Libs::LibKernel::Memory::TryReadAroundGpuWrites(
		                                   load.address, values, bytes));
		if (!around) {
			// The mirror did not hold them yet: the same copy instead of a readback.
			if (gpu_copies && page.owned && page.backing != nullptr) {
				resources[load.stage].gpu_flat_loads.push_back({load.address, load.slot, load.count});
				continue;
			}
			std::memcpy(values, reinterpret_cast<const void*>(load.address), bytes);
		}
	}
}

void GuestReadLog::ReadNow(const Entry& entry, uint8_t* bytes) const {
	if (!Libs::LibKernel::Memory::MayFaultOnGpuRead(entry.address, entry.size) ||
	    !Libs::LibKernel::Memory::TryReadAroundGpuWrites(entry.address, bytes, entry.size)) {
		std::memcpy(bytes, reinterpret_cast<const void*>(entry.address), entry.size);
	}
}

bool GuestReadLog::ZeroTestHolds(const Entry& entry) const {
	uint32_t previous = 0;
	uint32_t current  = 0;
	std::memcpy(&previous, m_bytes.data() + entry.offset, sizeof(previous));
	// A bound the GPU writes: later walks ahead of execution leave it unread (see
	// AheadGpuProduced()), as they read its bytes before the writer runs.
	if (Libs::LibKernel::Memory::MayFaultOnGpuRead(entry.address, sizeof(current))) {
		LearnGpuProducedBound(entry.address);
	}
	ReadNow(entry, reinterpret_cast<uint8_t*>(&current));
	return (current != 0) == (previous != 0);
}

bool GuestReadLog::OnlyRefreshableBitsChanged(const Entry& entry) const {
	static thread_local std::vector<uint8_t> now;
	now.resize(entry.size);
	ReadNow(entry, now.data());
	const auto* recorded = m_bytes.data() + entry.offset;
	auto        word     = entry.words_begin;
	for (uint32_t done = 0; done < entry.size; done += sizeof(uint32_t)) {
		uint32_t current  = 0;
		uint32_t previous = 0;
		const auto bytes  = std::min<uint32_t>(entry.size - done, sizeof(uint32_t));
		std::memcpy(&current, now.data() + done, bytes);
		std::memcpy(&previous, recorded + done, bytes);
		while (word < entry.words_end && m_words[word].offset < entry.offset + done) {
			word++;
		}
		const auto mask = word < entry.words_end && m_words[word].offset == entry.offset + done
		                      ? m_words[word].mask
		                      : 0u;
		if (((current ^ previous) & ~mask) != 0) {
			return false;
		}
	}
	return true;
}

void GuestReadLog::RefreshDescriptorWords(
    std::span<ShaderRecompiler::IR::ResourceSnapshot>          resources,
    std::span<const ShaderRecompiler::IR::ResourcePlan* const> plans) const {
	using ShaderRecompiler::IR::DescriptorRefresh;
	static thread_local std::vector<uint8_t> now;
	for (const auto index: m_refresh) {
		const auto& entry = m_entries[index];
		now.resize(entry.size);
		ReadNow(entry, now.data());
		for (uint32_t w = entry.words_begin; w < entry.words_end; w++) {
			const auto& load = m_words[w];
			uint32_t    value    = 0;
			uint32_t    previous = 0;
			std::memcpy(&value, now.data() + (load.offset - entry.offset), sizeof(value));
			std::memcpy(&previous, m_bytes.data() + load.offset, sizeof(previous));
			if (value == previous || load.stage >= resources.size() || load.stage >= plans.size() ||
			    plans[load.stage] == nullptr) {
				continue;
			}
			const auto& plan     = *plans[load.stage];
			auto&       snapshot = resources[load.stage];
			// The shader reads the same dword from the flattened SRT.
			const auto flat_word = uint64_t {load.source} * 8u + load.word;
			if (flat_word < plan.descriptor_word_flat.size() &&
			    plan.descriptor_word_flat[flat_word] < snapshot.flattened_srt.size()) {
				snapshot.flattened_srt[plan.descriptor_word_flat[flat_word]] = value;
			}
			// Descriptors the materialization cleared (invalid, unreached or of another type)
			// stay cleared: the refreshed bits do not decide that.
			const auto refresh = [&](ShaderRecompiler::IR::DescriptorValue& descriptor) {
				if (load.word >= descriptor.dword_count ||
				    std::ranges::all_of(descriptor.dwords, [](uint32_t dword) { return dword == 0; })) {
					return;
				}
				descriptor.dwords[load.word] =
				    (descriptor.dwords[load.word] & ~load.mask) | (value & load.mask);
			};
			const auto kind = load.source < plan.descriptor_refresh.size()
			                      ? plan.descriptor_refresh[load.source]
			                      : DescriptorRefresh::None;
			if (kind == DescriptorRefresh::Buffer) {
				for (uint32_t k = 0; k < plan.info.buffers.size() && k < snapshot.buffers.size(); ++k) {
					if (plan.info.buffers[k].source == load.source) refresh(snapshot.buffers[k]);
				}
			} else if (kind == DescriptorRefresh::Image) {
				for (uint32_t k = 0; k < plan.info.images.size() && k < snapshot.images.size(); ++k) {
					if (plan.info.images[k].source == load.source) refresh(snapshot.images[k]);
				}
			}
		}
	}
	m_refresh.clear();
}

// Most logged reads are a few dwords: compare those without a call.
static bool SameGuestBytes(const void* guest, const uint8_t* recorded, uint32_t size) {
	const auto load = [](const void* address, uint32_t offset, auto value) {
		std::memcpy(&value, static_cast<const uint8_t*>(address) + offset, sizeof(value));
		return value;
	};
	switch (size) {
		case 4: return load(guest, 0, uint32_t {}) == load(recorded, 0, uint32_t {});
		case 8: return load(guest, 0, uint64_t {}) == load(recorded, 0, uint64_t {});
		case 16:
			return ((load(guest, 0, uint64_t {}) ^ load(recorded, 0, uint64_t {})) |
			        (load(guest, 8, uint64_t {}) ^ load(recorded, 8, uint64_t {}))) == 0;
		case 32:
			return ((load(guest, 0, uint64_t {}) ^ load(recorded, 0, uint64_t {})) |
			        (load(guest, 8, uint64_t {}) ^ load(recorded, 8, uint64_t {})) |
			        (load(guest, 16, uint64_t {}) ^ load(recorded, 16, uint64_t {})) |
			        (load(guest, 24, uint64_t {}) ^ load(recorded, 24, uint64_t {}))) == 0;
		default: return std::memcmp(guest, recorded, size) == 0;
	}
}

bool GuestReadLog::StillValid() const {
	m_refresh.clear();
	if (m_failed) {
		return false;
	}
	std::array<uint8_t, StrictCheckBytes> buffer {};
	// Most entries lie on pages the GPU does not own: decide that once per page. The entries of
	// a draw alternate between a few pages (its descriptor tables and the GPU-owned pages around
	// them), so the last pages are kept, with the backing bytes of the GPU-owned ones for reading
	// around the GPU's writes there (see FindAroundBackingPage()). RefreshFlatLoads() reuses them.
	auto& checked = m_checked;
	checked.fill({});
	// An entry with descriptor words compares its other bits.
	const auto refreshable = [&](const Entry& entry) {
		if (entry.words_begin == entry.words_end || !OnlyRefreshableBitsChanged(entry)) {
			return false;
		}
		m_refresh.push_back(static_cast<uint32_t>(&entry - m_entries.data()));
		return true;
	};
	for (const auto& entry: m_entries) {
		const auto* recorded = m_bytes.data() + entry.offset;
		if (entry.zero_test) {
			if (!ZeroTestHolds(entry)) {
				return false;
			}
			continue;
		}
		if (!entry.strict) {
			const auto  first_page   = entry.address >> 12u;
			const auto  last_page    = (entry.address + entry.size - 1) >> 12u;
			bool        gpu_owned    = false;
			const auto* page_backing = static_cast<const uint8_t*>(nullptr);
			if (first_page == last_page) {
				auto& page = checked[first_page % checked.size()];
				if (page.page != first_page) {
					page.page    = first_page;
					page.owned   = Libs::LibKernel::Memory::MayFaultOnGpuRead(entry.address, entry.size);
					page.backing = page.owned
					                   ? Libs::LibKernel::Memory::FindAroundBackingPage(entry.address)
					                   : nullptr;
				}
				gpu_owned    = page.owned;
				page_backing = page.backing;
			} else {
				gpu_owned = Libs::LibKernel::Memory::MayFaultOnGpuRead(entry.address, entry.size);
			}
			// On GPU-owned pages a direct load faults and reads back all the GPU's queued work:
			// bytes the GPU did not write compare from the backing store.
			bool around = gpu_owned;
			for (uint32_t done = 0; around && done < entry.size; done += sizeof(buffer)) {
				const auto bytes = std::min<uint32_t>(entry.size - done, sizeof(buffer));
				around           = page_backing != nullptr
				                       ? Libs::LibKernel::Memory::TryReadAroundGpuWritesOnPage(
				                             entry.address + done, buffer.data(), bytes, page_backing)
				                       : Libs::LibKernel::Memory::TryReadAroundGpuWrites(
				                             entry.address + done, buffer.data(), bytes);
				if (around && std::memcmp(buffer.data(), recorded + done, bytes) != 0) {
					if (!refreshable(entry)) {
						return false;
					}
					break;
				}
			}
			if (around) {
				continue;
			}
			// A plain load, as the execution thread's own walk reads: GPU-owned pages fault and
			// read the GPU's bytes back.
			if (!SameGuestBytes(reinterpret_cast<const void*>(entry.address), recorded,
			                    entry.size) &&
			    !refreshable(entry)) {
				return false;
			}
			continue;
		}
		// As specialization memory is read on the execution thread: GPU-owned bytes fail. The
		// bytes compare in place: large reads are the key arrays of bindless tables (GTA V's are
		// 2 MB, read by four operations a frame), compared by the helpers too.
		if (!Libs::LibKernel::Memory::GpuCleanBackingEquals(entry.address, recorded, entry.size, ParallelEqual)) {
			return false;
		}
	}
	return true;
}

bool GraphicsPipelineKey::operator==(const GraphicsPipelineKey& other) const {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	return XXH3_64bits(&key, sizeof(key));
}

std::span<const uint32_t> PipelineCache::Program::FunctionCode() const {
	return call_source
	    ? std::span<const uint32_t>(call_source->linked->code).subspan(call_source->code.size())
	    : std::span<const uint32_t>{};
}

bool PipelineCache::Program::RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime) {
	const auto revision = call_source->revision;
	ShaderRecompiler::RefreshShaderSource(*call_source, runtime);
	resource_plan.source_reads = call_source->reads;
	return revision != call_source->revision;
}

template <bool HasCalls>
ShaderModule* PipelineCache::Program::FindModule(const ShaderRecompiler::IR::SrtRuntime& runtime,
                                                 uint32_t push_data_cursor, bool& failed) {
	failed = !ShaderRecompiler::IR::MaterializeResources(resource_plan, runtime, resources,
	                                                     specialization);
	if (failed) {
		return nullptr;
	}
	const auto matches = [&](const ShaderModule& candidate) {
		const auto& layout = candidate.info.bindings;
		if (layout.push_data_start_dword != ShaderRecompiler::IR::PushData::StartFor(
		        push_data_cursor, layout.ShaderDataDwords()) ||
		    candidate.specialization != specialization) {
			return false;
		}
		if constexpr (HasCalls) {
			if (!std::ranges::equal(candidate.function_code, FunctionCode())) return false;
		}
		return true;
	};
	auto& last = last_module[GuestGpu::IsGpuThread() ? 0u : 1u];
	if (last < modules.size() && matches(modules[last])) {
		return &modules[last];
	}
	const auto found = std::ranges::find_if(modules, matches);
	if (found == modules.end()) {
		return nullptr;
	}
	last = static_cast<size_t>(found - modules.begin());
	return &*found;
}

ShaderModule PipelineCache::BuildModule(const ProgramKey&                                  key,
                                       const ShaderRecompiler::CompileOptions&            options,
                                       ShaderRecompiler::TranslateResult                  translated,
                                       const ShaderRecompiler::IR::ResourceSpecialization& specialization,
                                       std::span<const uint32_t> function_code,
                                       uint32_t push_data_cursor, std::vector<uint32_t>& spirv,
                                       std::vector<uint32_t>& plain_spirv) const {
	Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::ShadersCompiled,
	                                         Common::DebugCounters::Counter::ShaderCompileNs,
	                                         Common::DebugCounters::Peak::ShaderCompileNs);
	const auto profile         = SpirvProfile(m_graphics);
	auto       compile_options = options;
	compile_options.profile    = &profile;
	compile_options.plain_mip_stats_variant = key.stage == ShaderType::Pixel;
	auto result = ShaderRecompiler::CompileProgram(std::move(translated), compile_options,
	                                               specialization, push_data_cursor);
	auto info = std::move(result.program).TakeCompiledInfo();

	// The same inputs give the same module id across runs.
	ShaderRecompiler::IR::Writer inputs;
	ShaderRecompiler::IR::IoAll(inputs, key.stage, key.hash, key.user_data_count, key.static_state,
	                            info.bindings.push_data_start_dword, specialization);
	inputs.Raw(function_code.data(), function_code.size_bytes()); // Last, so it needs no size.
	const auto id = XXH3_64bits(inputs.bytes.data(), inputs.bytes.size());

	const auto* stage = ShaderRecompiler::StageName(key.stage);
	DumpShader(result.spirv, fmt::format("{}_{:016x}_{:016x}.spv", stage, key.hash, id));
	ValidateShaderSpirv(stage, key.hash, result.spirv, profile.maintenance8);
	ShaderModule module {
	    .id             = id,
	    .module         = CompileSPV(result.spirv, m_graphics.device),
	    .info           = std::move(info),
	    .specialization = specialization,
	    .function_code  = {function_code.begin(), function_code.end()},
	};
	module.spirv_words = static_cast<uint32_t>(result.spirv.size());
	// The inputs of these come from the stage's static key.
	if (key.stage == ShaderType::Vertex || key.stage == ShaderType::Local) {
		module.vertex_formats = VertexInputFormats(*options.input_info.vertex, module.info);
	} else if (key.stage == ShaderType::Pixel &&
	           options.input_info.pixel->parameter_mode == ShaderPixelParameterMode::Rectangle) {
		module.rect_parameters = RectListParameters(*options.input_info.pixel, module.info);
	} else if (key.stage == ShaderType::Mesh) {
		module.mesh_task = options.input_info.vertex->indirect_draw;
	} else if (key.stage == ShaderType::Compute) {
		module.guest_traversal = NativeRayTracing::PatchShaders() && !options.native_ray_tracing;
	}
	if (!result.spirv_plain.empty()) {
		ValidateShaderSpirv(stage, key.hash, result.spirv_plain, profile.maintenance8);
		module.plain = MakePlainModule(module, result.spirv_plain, m_graphics.device);
		EXIT_IF(module.plain == nullptr);
	}
	spirv       = std::move(result.spirv);
	plain_spirv = std::move(result.spirv_plain);
	return module;
}

std::shared_ptr<const ShaderModule> MakePlainModule(const ShaderModule&       module,
                                                    std::span<const uint32_t> spirv,
                                                    vk::Device                device) {
	auto plain = std::make_shared<ShaderModule>();
	// Module ids are hashes: a fixed change of the instrumented module's id names its variant.
	plain->id     = module.id ^ 0x9e3779b97f4a7c15ull;
	plain->module = CompileSPV(spirv, device);
	if (plain->module == nullptr) {
		return nullptr;
	}
	plain->info            = module.info;
	plain->specialization  = module.specialization;
	plain->function_code   = module.function_code;
	plain->vertex_formats  = module.vertex_formats;
	plain->rect_parameters = module.rect_parameters;
	plain->spirv_words     = static_cast<uint32_t>(spirv.size());
	return plain;
}

void PipelineCache::AddedModule(const ProgramKey& key, const Program& program,
                                const ShaderModule& module, const std::vector<uint32_t>& spirv,
                                const std::vector<uint32_t>&            plain_spirv,
                                const ShaderRecompiler::CompileOptions& options,
                                std::span<const uint32_t> code, uint32_t push_data_cursor) {
	m_compiled[static_cast<size_t>(key.stage)]++;
	// Guest geometry shaders are compiled through the host mesh stage.
	std::printf("Shaders: VS %u | PS %u | CS %u | GS %u | LS %u | HS %u | TES %u\n",
	            m_compiled[static_cast<size_t>(ShaderType::Vertex)],
	            m_compiled[static_cast<size_t>(ShaderType::Pixel)],
	            m_compiled[static_cast<size_t>(ShaderType::Compute)],
	            m_compiled[static_cast<size_t>(ShaderType::Mesh)],
	            m_compiled[static_cast<size_t>(ShaderType::Local)],
	            m_compiled[static_cast<size_t>(ShaderType::TessellationControl)],
	            m_compiled[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
	// Programs with GPU-selected calls depend on their callees' current code.
	if (program.call_source == nullptr) {
		StoreModule(key, program, module, spirv, plain_spirv);
		RecordModule(key, options, code, push_data_cursor, module);
	}
}

void PipelineCache::DestroyModule(const ShaderModule& module) const {
	m_graphics.device.destroyShaderModule(module.module, nullptr);
	if (module.plain != nullptr) {
		m_graphics.device.destroyShaderModule(module.plain->module, nullptr);
	}
}

ShaderModule& PipelineCache::CompileModule(Program& program, const ProgramKey& key,
                                           ShaderRecompiler::CompileOptions  options,
                                           ShaderRecompiler::TranslateResult translated,
                                           uint32_t                          push_data_cursor,
                                           std::span<const uint32_t>         code) {
	std::vector<uint32_t> spirv;
	std::vector<uint32_t> plain_spirv;
	auto& module = program.modules.emplace_back(
	    BuildModule(key, options, std::move(translated), program.specialization,
	                program.FunctionCode(), push_data_cursor, spirv, plain_spirv));
	AddedModule(key, program, module, spirv, plain_spirv, options, code, push_data_cursor);
	return module;
}

template <typename InputInfo>
const ShaderModule* PipelineCache::GetProgram(const ShaderParams& params, InputInfo& input_info,
                                              uint32_t& push_data_cursor) {
	const auto user_data = std::span(params.user_data).first(params.user_data_count);
	ShaderRecompiler::IR::SrtRuntime runtime {
	    .user_data                  = user_data,
	    .shader_base                = params.Base(),
	    .read_specialization_memory = ReadShaderGuestMemory,
	    .image_heap                 = m_image_heap,
	};
	ShaderRecompiler::CompileOptions options {.wave_size = input_info.wave_size};
	if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
		options.stage            = input_info.logical_stage;
		options.input_info.vertex = &input_info;
		options.user_data_base   = 8;
		if (options.stage == ShaderType::Mesh ||
		    options.stage == ShaderType::TessellationControl) {
			options.user_data_base = 0;
		}
		if (options.stage == ShaderType::Mesh) {
			options.wave_size = input_info.mesh.wave_size;
		}
	} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
		options.stage            = ShaderType::Pixel;
		options.input_info.pixel = &input_info;
	} else {
		static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
		options.stage              = ShaderType::Compute;
		options.input_info.compute = &input_info;
		runtime.workgroup_counts   = input_info.workgroup_counts;
		runtime.indirect_args_address = input_info.indirect_args_address;
	}
	m_lookup_key.stage           = options.stage;
	m_lookup_key.hash            = params.hash;
	m_lookup_key.user_data_count = params.user_data_count;
	BuildStageStaticKey(input_info, m_lookup_key.static_state);
	AppendNativeRayTracingKey(options.stage, m_lookup_key.static_state, !m_guest_traversal);
	auto entry = m_program_cache.find(m_lookup_key);
	const auto bind = [&](const ShaderModule& module) {
		input_info.stage = {.module = &module, .resources = &entry->second.resources};
		module.info.bindings.AdvancePushData(push_data_cursor);
		return &module;
	};
	// The resources of a draw or dispatch can fail to resolve, as the guest rewrites a
	// descriptor table it reads (Demon's Souls: an image table read half rewritten has candidates
	// of incompatible types): it is skipped, as the resolution ahead of execution is. [HACK] until
	// the cause is fixed: the draw is not rendered.
	bool       failed       = false;
	const auto skip_failure = [&] {
		Common::HackNotice::Report("draw or dispatch with unresolvable resources skipped",
		                           "PipelineCache::GetProgram", params.hash, 0);
		return nullptr;
	};
	bool source_changed = false;
	if (entry != m_program_cache.end()) {
		auto& cached = entry->second;
		if (cached.skip_dispatch) return nullptr;
		ShaderModule* module = nullptr;
		if (cached.call_source) {
			source_changed = cached.RefreshCallSource(runtime);
			if (cached.call_source->refresh_failed) {
				ExitCallExpansionFailure(params.hash, *cached.call_source);
			}
			runtime.call_words = cached.call_source->call_words;
			if (!source_changed) module = cached.FindModule<true>(runtime, push_data_cursor, failed);
		} else {
			module = cached.FindModule<false>(runtime, push_data_cursor, failed);
		}
		if (failed) return skip_failure();
		if (module != nullptr) return bind(*module);
	}
	// A worker may be compiling this program: use it once it is there.
	if (WaitForCompile(m_lookup_key)) {
		return GetProgram(params, input_info, push_data_cursor);
	}

	options.shader_hash     = params.hash;
	options.user_data       = user_data;
	options.back_code       = params.back_code;
	options.dump_ir         = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
	options.emit_debug_info = Config::GraphicsDebugDumpEnabled();
	options.native_ray_tracing = NativeRayTracing::PatchShaders() && !m_guest_traversal;
	options.ray_stats          = options.native_ray_tracing && Config::RayTracingStatsEnabled();
	ShaderRecompiler::ShaderSource source;
	if (entry == m_program_cache.end()) {
		DumpShader(params.code, fmt::format("original/{}_{:016x}.bin",
		                                    ShaderRecompiler::StageName(options.stage), params.hash));
		source = ShaderRecompiler::PrepareShaderSource(params.code, options);
		if (source.unsupported_call_pc != UINT32_MAX && !source.expand_calls) {
			PipelineCacheLog("!!! TEMPORARY COMPUTE DISPATCH SKIP !!! shader=0x{:016x} pc=0x{:08x} "
			                 "reason=unsupported GPU-selected callable target; rendering is incomplete",
			                 params.hash, source.unsupported_call_pc);
			m_program_cache.emplace(m_lookup_key, Program {.skip_dispatch = true});
			return nullptr;
		}
	}
	auto* current = entry == m_program_cache.end() ? &source : entry->second.call_source.get();
	ShaderRecompiler::TranslateResult translated;
	if (current != nullptr) {
		if (current->HasCalls()) {
			if (entry == m_program_cache.end()) {
				ShaderRecompiler::RefreshShaderSource(*current, runtime);
				if (current->refresh_failed) ExitCallExpansionFailure(params.hash, *current);
				runtime.call_words = current->call_words;
			}
			translated = ShaderRecompiler::TranslateProgram(current->linked->decoded, options);
			translated.program.source_reads = current->reads;
		} else {
			translated = ShaderRecompiler::TranslateProgram(current->decoded, options);
		}
	} else {
		translated = ShaderRecompiler::TranslateProgram(params.code, options);
	}
	if (entry == m_program_cache.end()) {
		auto retained = source.HasCalls()
		                    ? std::make_unique<ShaderRecompiler::ShaderSource>(std::move(source))
		                    : nullptr;
		entry = m_program_cache.emplace(m_lookup_key, Program {
		    .call_source   = std::move(retained),
		    .resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program),
		}).first;
		auto& program = entry->second;
		if (!ShaderRecompiler::IR::MaterializeResources(program.resource_plan, runtime,
		                                                program.resources, program.specialization)) {
			return skip_failure();
		}
	} else if (source_changed) {
		entry->second.resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
		if (const auto* module = entry->second.FindModule<true>(runtime, push_data_cursor, failed)) {
			return bind(*module);
		}
		if (failed) return skip_failure();
	}
	return bind(CompileModule(entry->second, entry->first, options, std::move(translated),
	                          push_data_cursor, params.code));
}

PipelineCache::PipelineCache(GraphicContext&                         graphics,
                             ShaderRecompiler::IR::ImageHeapSource* image_heap,
                             const std::filesystem::path&           shader_store)
    : m_graphics(graphics), m_image_heap(image_heap),
      m_next_driver_cache_save(std::chrono::steady_clock::now() + DriverCacheSaveInterval) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
	// A recording takes what the session compiles: programs the store compiled would be missing
	// from it, as the store keeps no stage input.
	if (Config::PipelineManifestRecord() && !shader_store.empty()) {
		PipelineCacheLog("Shader cache: not used while the pipeline manifest records");
		WarmUp({});
	} else {
		WarmUp(shader_store);
	}
	// Before the manifest's threads, which give them work.
	StartHelpers();
	OpenManifest();
	for (uint32_t i = 0; i < CompileWorkers; i++) {
		m_workers.emplace_back([this] { CompileWorker(); });
	}
	if (Config::AsyncPipelinesEnabled()) {
		constexpr uint32_t AsyncThreads = 3;
		for (uint32_t i = 0; i < AsyncThreads; i++) {
			m_async_threads.emplace_back([this] { AsyncPipelineWorker(); });
		}
	}
	// The execution thread takes the pipelines background threads and helpers created from the
	// m_async_ready maps.
	m_async_handoff = !m_async_threads.empty() || m_manifest_warm || m_helpers != nullptr;
}

PipelineCache::~PipelineCache() {
	StopWorkers();
	CloseManifest();
	StopHelpers();
	StopAsyncPipelines();
	Save();
	for (const auto cache: m_helper_caches) {
		m_graphics.device.destroyPipelineCache(cache, nullptr);
	}
	m_graphics.device.destroyPipelineCache(m_helper_cache, nullptr);
	auto destroy = [this](const auto& pipelines) {
		for (const auto& pipeline: pipelines | std::views::values) {
			m_graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	destroy(m_async_ready);
	destroy(m_async_ready_compute);
	for (const auto& program: m_program_cache | std::views::values) {
		for (const auto& module: program.modules) {
			DestroyModule(module);
		}
	}
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
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
	m_driver_cache_path     = std::filesystem::path("_PipelineCache") / (title_id + ".bin");
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
				    "Vulkan pipeline cache: invalidating {} (driver or data mismatch)",
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
		PipelineCacheLog("Vulkan pipeline cache: loaded {:.2f} MB from {}",
		                 static_cast<double>(initial_data.size()) / 1'000'000.0, path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
	// The pipelines created last are written although no other creation follows to find the
	// cache due.
	m_driver_cache_posted = std::chrono::steady_clock::now();
	m_driver_cache_timer = std::jthread([this](const std::stop_token& stop) {
		while (!stop.stop_requested()) {
			std::this_thread::sleep_for(DriverCacheTimerPeriod);
			std::lock_guard lock(m_driver_cache_mutex);
			PostDueDriverCache();
		}
	});
}

void PipelineCache::CheckpointDriverCache(bool slow_creation) {
	if (m_driver_cache == nullptr) {
		return;
	}
	std::lock_guard lock(m_driver_cache_mutex);
	m_driver_cache_dirty = true;
	if (slow_creation) {
		m_next_driver_cache_save = std::min(m_next_driver_cache_save,
		                                    m_driver_cache_posted + SlowCreationSaveInterval);
	}
	PostDueDriverCache();
}

void PipelineCache::NoteCreatedPipeline(std::chrono::steady_clock::time_point started) {
	CheckpointDriverCache(std::chrono::steady_clock::now() - started >= SlowCreation);
}

void PipelineCache::PostDueDriverCache() {
	if (!m_driver_cache_dirty) {
		return;
	}
	if (const auto now = std::chrono::steady_clock::now(); now >= m_next_driver_cache_save) {
		m_driver_cache_dirty     = false;
		m_driver_cache_posted    = now;
		m_next_driver_cache_save = now + DriverCacheSaveInterval;
		// The cache object is internally synchronized, so pipelines may be created meanwhile.
		m_store.Post([this] { WriteDriverCache(); });
	}
}

void PipelineCache::Save() {
	m_driver_cache_timer = {};
	m_store.Finish();
	bool dirty = false;
	{
		std::lock_guard lock(m_driver_cache_mutex);
		dirty = std::exchange(m_driver_cache_dirty, false);
	}
	if (dirty) {
		WriteDriverCache();
	}
}

void PipelineCache::WriteDriverCache() {
	// What the helpers compiled goes into the file with the driver cache. No pipeline is created
	// from the caches merged into, which only this thread touches; the others are only read.
	std::vector<vk::PipelineCache> helper_caches;
	{
		std::scoped_lock lock(m_helper_caches_mutex);
		helper_caches.swap(m_helper_caches);
	}
	const vk::PipelineCacheCreateInfo empty {};
	if (!helper_caches.empty() && m_helper_cache == nullptr &&
	    m_graphics.device.createPipelineCache(&empty, nullptr, &m_helper_cache) != vk::Result::eSuccess) {
		m_helper_cache = nullptr;
	}
	if (m_helper_cache != nullptr && !helper_caches.empty()) {
		(void)m_graphics.device.mergePipelineCaches(m_helper_cache,
		                                            static_cast<uint32_t>(helper_caches.size()),
		                                            helper_caches.data());
	}
	for (const auto cache: helper_caches) {
		m_graphics.device.destroyPipelineCache(cache, nullptr);
	}
	vk::PipelineCache source = m_driver_cache;
	vk::PipelineCache merged = nullptr;
	if (m_helper_cache != nullptr &&
	    m_graphics.device.createPipelineCache(&empty, nullptr, &merged) == vk::Result::eSuccess) {
		const std::array sources {m_driver_cache, m_helper_cache};
		if (m_graphics.device.mergePipelineCaches(merged, static_cast<uint32_t>(sources.size()),
		                                          sources.data()) == vk::Result::eSuccess) {
			source = merged;
		}
	}
	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(source, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(source, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	m_graphics.device.destroyPipelineCache(merged, nullptr);
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {:.2f} MB)",
		                 vk::to_string(result), static_cast<double>(size) / 1'000'000.0);
		return;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return;
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
	const bool written = prefix_written == prefix.size() && payload_written == payload.size() && flushed;
	std::error_code error;
	if (written) {
		std::filesystem::rename(temp_path, m_driver_cache_path, error);
	}
	if (!written || error) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {:.2f} MB to {}",
	                 static_cast<double>(payload.size()) / 1'000'000.0,
	                 Common::PathToString(m_driver_cache_path));
}

void PipelineCache::GetGraphicsPrograms(const CommandBuffer& command, DrawRenderState& state) {
	ResolveGraphicsPrograms(command.GetRegisters(), command.GetUserConfig(), command.GetShaders(),
	                        state, nullptr);
}

bool PipelineCache::GetGraphicsProgramsAhead(const HW::Context&    context,
                                             const HW::UserConfig& user_config,
                                             const HW::Shader& shaders, DrawRenderState& state,
                                             ProgramResolution& ahead) {
	return ResolveGraphicsPrograms(context, user_config, shaders, state, &ahead);
}

bool PipelineCache::ResolveGraphicsPrograms(const HW::Context&    context,
                                            const HW::UserConfig& user_config,
                                            const HW::Shader& shaders, DrawRenderState& state,
                                            ProgramResolution* ahead) {
	const auto& sh                = context.GetShaderRegisters();
	const auto& vertex_regs       = shaders.GetVs();
	const auto& pixel_regs        = shaders.GetPs();
	auto&       vertex_info       = state.vertex_info;
	auto&       pixel_info        = state.ps_input_info;
	const bool  object_primitive_id = context.HasObjectPrimitiveIdOutput();
	const auto  write_mask          = context.GetRenderTargetMask() & sh.m_cbShaderMask;
	uint32_t    color_output_mask   = object_primitive_id ? 1u << 7u : 0;
	for (uint32_t index = 0; index < RENDER_COLOR_ATTACHMENTS_MAX; index++) {
		const auto slot = ShaderPixelExportTarget(sh.m_cbShaderMask, index);
		if (slot >= RENDER_COLOR_ATTACHMENTS_MAX) {
			break;
		}
		if (sh.target_output_mode[index] != 0 && render_target_mask_slot(write_mask, slot) != 0) {
			color_output_mask |= 1u << slot;
		}
	}
	const auto& db = sh.db_shader_control;
	// Without color outputs, a pixel shader still runs for its depth or coverage side effects.
	const bool pixel_active =
	    pixel_regs.ps_regs.data_addr != 0 &&
	    (color_output_mask != 0 || db.shader_kill_enable || db.shader_z_export_enable ||
	     db.shader_mask_export_enable || db.shader_dual_export_enable ||
	     db.shader_execute_on_noop || sh.AlphaToMask());
	state.ps_active = pixel_active;
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	// Ahead of execution, the vertex fetch tables are read through the resolution's log.
	const ShaderGuestReaderScope reader(ahead != nullptr ? ReadAheadShaderState : nullptr,
	                                    ahead != nullptr ? &ahead->reads : nullptr);
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		// Guest geometry shaders run as host mesh shaders, which cannot follow tessellation.
		if ((context.GetShaderStages() & 0x20u) != 0) {
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(
				    "Warning: dropping tessellation draws with a geometry shader.\n");
			}
			return false;
		}
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
		// Reflection decodes both programs; the key also covers the VGT fields in tess.
		const uint64_t layout_key[] = {vertex_params[0].hash, vertex_params[1].hash,
		                               uint64_t {sh.m_vgtLsHsConfig & 0xfff00u} << 32u |
		                                   sh.m_vgtTfParam};
		// The analysis is remembered per program pair; ahead of execution only a remembered one
		// is used.
		const auto layout_hash = XXH3_64bits(layout_key, sizeof(layout_key));
		ShaderTessellationInputInfo tess;
		{
			const ProgramLockGuard lock(m_program_lock);
			if (ahead != nullptr) {
				const auto found = m_tess_layouts.find(layout_hash);
				if (found == m_tess_layouts.end()) {
					ahead->reads.Fail();
					return false;
				}
				tess = found->second;
			} else {
				const auto [layout, inserted] =
				    m_tess_layouts.try_emplace(layout_hash, vertex_info[0].tess);
				if (inserted) {
					if (!ShaderRecompiler::AnalyzeTessellationPrograms(
					        vertex_params[0].code, vertex_params[1].code, layout->second)) {
						EXIT("unsupported tessellation program shape: LS 0x%016" PRIx64
						     " HS 0x%016" PRIx64 "\n",
						     vertex_params[0].hash, vertex_params[1].hash);
					}
					StoreLayout(layout->first, layout->second);
				}
				tess = layout->second;
			}
		}
		for (auto& stage: vertex_info) {
			stage.tess = tess;
		}
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	if (ahead != nullptr && ahead->reads.Failed()) {
		return false;
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (!tess_active && !mesh_active) {
		vertex_info[0].indirect_offsets = state.indirect_offsets;
		vertex_info[0].indirect_draw    = state.indirect_draw;
	}
	if (object_primitive_id) {
		// PA_CL_OBJPRIM_ID_CNTL = 2: VGT_OBJECT_ID plus the pipelined 32-bit primitive ID.
		EXIT_NOT_IMPLEMENTED(sh.m_paClObjprimIdCntl != 2u || mesh_active || tess_active ||
		                     !pixel_active || (sh.m_vgtDrawPayloadCntl & 8u) == 0 ||
		                     user_config.GetPrimType() != Prospero::PrimitiveType::kTriList);
		const auto& target = context.GetRenderTarget(7);
		EXIT_NOT_IMPLEMENTED(Prospero::ResolveRenderTargetFormat(target.info.format,
		                                                       target.info.channel_type)
		                         .buffer_format != Prospero::BufferFormat::k32UInt);
	}
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
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: dropping mesh draws the host does not support (shader 0x{:016x}).\n",
				    vertex_params[0].hash));
			}
			if (ahead != nullptr) {
				ahead->reads.Fail();
			}
			return false;
		}
		// A task shader launches each record's groups and passes its draw parameters in a
		// payload beside the LDS, the allocation pair and the Layer array; without room for
		// it, the CPU reads the records.
		if (state.indirect_draw &&
		    (mesh.lds_size_dwords + 2u + mesh.max_vertices +
		     ShaderRecompiler::IR::PushData::MeshTaskPayloadDwordCount) * sizeof(uint32_t) <=
		        limits.maxMeshPayloadAndSharedMemorySize) {
			vertex_info[0].indirect_offsets = state.indirect_offsets;
			vertex_info[0].indirect_draw    = true;
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
		    target_export_mapping {};
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
			const auto& rt = context.GetRenderTarget(slot);
			if ((color_output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
				target_export_mapping[slot] =
				    TextureGetRenderTargetFormat(m_graphics, rt.info.format, rt.info.channel_type,
				                                 rt.info.channel_order)
				        .export_mapping;
			}
		}
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		pixel_info.object_primitive_id = object_primitive_id;
		for (uint32_t export_index = 0; export_index < 8u; export_index++) {
			const auto slot = ShaderPixelExportTarget(pixel_info.target_shader_mask, export_index);
			if (slot >= 8u || pixel_info.target_output_mode[export_index] == 0u) continue;
			const auto& rt = context.GetRenderTarget(slot);
			const auto target_mask = render_target_mask_slot(context.GetRenderTargetMask(), slot);
			if (rt.base.addr == 0 || target_mask == 0u) continue;
			if (rt.info.channel_type == Prospero::ChannelType::kUInt) {
				pixel_info.target_uint_mask |= 1u << export_index;
			} else if (rt.info.channel_type == Prospero::ChannelType::kSInt) {
				pixel_info.target_sint_mask |= 1u << export_index;
			}
			const auto format = Prospero::ResolveRenderTargetFormat(rt.info.format, rt.info.channel_type);
			if (format.buffer_format != Prospero::BufferFormat::k11_11_10UNorm) continue;
			const auto mapping = pixel_info.target_export_mapping[export_index];
			const auto shader_mask = render_target_mask_slot(pixel_info.target_shader_mask, slot);
			const bool blend = context.GetBlendControl(slot).enable && !rt.info.blend_bypass;
			if (blend || (mapping.ApplyMask(target_mask) & 7u) != 7u ||
			    (mapping.ApplyMask(shader_mask) & 7u) != 7u ||
			    pixel_info.target_output_mode[export_index] != 9u) {
				EXIT("unsupported packed UNorm render-target export: slot=%u export=%u mode=%u "
				     "target_mask=0x%x shader_mask=0x%x mapping=0x%02x blend=%u\n",
				     slot, export_index, pixel_info.target_output_mode[export_index], target_mask,
				     shader_mask, mapping.packed, static_cast<uint32_t>(blend));
			}
			pixel_info.target_packed_unorm_mask |= 1u << export_index;
			pixel_info.target_round_mode |= static_cast<uint8_t>(rt.info.round_mode) << export_index;
		}
		if (!mesh_active && !tess_active && Prospero::IsRectList(user_config.GetPrimType())) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::Rectangle;
		} else if (context.GetModeControl().provoking_vtx_last) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::LastVertex;
		} else {
			pixel_info.parameter_mode = ShaderPixelParameterMode::FirstVertex;
		}
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0; blended targets are not integer.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
			pixel_info.target_uint_mask &= ~2u;
			pixel_info.target_sint_mask &= ~2u;
		} else if (blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
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
	}
	auto&       last_stage   = vertex_info[tess_active ? 2u : 0u];
	const auto& clip_control = context.GetClipControl();
	if (clip_control.clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = last_stage.clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	} else if (clip_control.user_clip_planes != 0) {
		// CLIP_DISABLE also disables user planes. How they combine with shader clip and cull
		// distances is unverified; draw push constants only reach VS/mesh and PS, and the
		// generated RECTLIST stages do not forward clip distances.
		EXIT_NOT_IMPLEMENTED((last_stage.pa_cl_vs_out_cntl & 0xffffu) != 0 || tess_active ||
		                     Prospero::IsRectList(user_config.GetPrimType()));
		last_stage.user_clip_planes = clip_control.user_clip_planes;
	}
	uint32_t push_data_cursor = ShaderRecompiler::IR::PushData::GraphicsDrawDwords(
	    mesh_active, object_primitive_id, last_stage.user_clip_planes);
	const ProgramLockGuard lock(m_program_lock);
	if (ahead != nullptr) {
		const ShaderModule* pixel_module = nullptr;
		if (pixel_active) {
			pixel_module = GetAhead(pixel_params, pixel_info, push_data_cursor, *ahead);
		}
		for (uint32_t i = 0; i < (tess_active ? 3u : 1u) && !ahead->reads.Failed(); i++) {
			GetAhead(vertex_params[i], vertex_info[i], push_data_cursor, *ahead);
		}
		if (ahead->reads.Failed()) {
			return false;
		}
		if (pixel_module != nullptr) {
			for (const auto& output: pixel_module->info.info.outputs) {
				if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt ||
				    output.kind == ShaderRecompiler::IR::StageOutputKind::ObjectPrimitiveId) {
					state.mrt_mask |= 1u << output.location;
				}
			}
			state.mrt_mask &= color_output_mask;
		}
		return true;
	}
	if (pixel_active) {
		const auto* pixel_module = GetProgram(pixel_params, pixel_info, push_data_cursor);
		if (pixel_module == nullptr) {
			// Skipped: the draw has no vertex program either.
			vertex_info[0].stage = {};
			return true;
		}
		for (const auto& output: pixel_module->info.info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt ||
			    output.kind == ShaderRecompiler::IR::StageOutputKind::ObjectPrimitiveId) {
				state.mrt_mask |= 1u << output.location;
			}
		}
		state.mrt_mask &= color_output_mask;
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		GetProgram(vertex_params[i], vertex_info[i], push_data_cursor);
	}
	return true;
}

template <typename InputInfo>
const ShaderModule* PipelineCache::GetAhead(const ShaderParams& params, InputInfo& input_info,
                                            uint32_t& push_data_cursor, ProgramResolution& ahead) {
	if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
		m_ahead_key.stage = input_info.logical_stage;
	} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
		m_ahead_key.stage = ShaderType::Pixel;
	} else {
		static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
		m_ahead_key.stage = ShaderType::Compute;
	}
	m_ahead_key.hash            = params.hash;
	m_ahead_key.user_data_count = params.user_data_count;
	BuildStageStaticKey(input_info, m_ahead_key.static_state);
	AppendNativeRayTracingKey(m_ahead_key.stage, m_ahead_key.static_state);
	const auto entry = m_program_cache.find(m_ahead_key);
	if (entry == m_program_cache.end() && ahead.stages != ProgramResolution::MaxStages) {
		QueueCompile(params, input_info, push_data_cursor, nullptr);
	}
	// SWAPPC programs link the functions they call from guest memory, and image heap views are
	// acquired in draw order: the execution thread resolves them.
	if (entry == m_program_cache.end() || ahead.stages == ProgramResolution::MaxStages ||
	    entry->second.skip_dispatch || entry->second.call_source ||
	    std::ranges::any_of(entry->second.modules, [](const ShaderModule& module) {
		    return module.info.info.uses_image_heap;
	    })) {
		ahead.reads.Fail();
		return nullptr;
	}
	const auto stage_index    = ahead.stages++;
	auto&      resources      = ahead.resources[stage_index];
	auto&      specialization = ahead.specializations[stage_index];
	ahead.plans[stage_index] = &entry->second.resource_plan;
	ahead.reads.SetStage(stage_index, &entry->second.resource_plan);
	// The walker's loads of GPU-owned pages read the backing store instead of downloading
	// GPU-written bytes, and the loads that still fault fail the walk; every load is logged with
	// the value it read, for the execution thread to check.
	ShaderRecompiler::IR::SrtRuntime runtime {
	    .user_data                  = std::span(params.user_data).first(params.user_data_count),
	    .shader_base                = params.Base(),
	    .userdata                   = &ahead.reads,
	    .read_specialization_memory = ReadAheadStrict,
	    .record_load                = RecordAheadLoad,
	    .gpu_produced               = AheadGpuProduced,
	    .pending_write              = AheadPendingWrite,
	};
	if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
		// As GetProgram(): descriptor tables indexed by the workgroup id probe one entry per group.
		runtime.workgroup_counts      = input_info.workgroup_counts;
		runtime.indirect_args_address = input_info.indirect_args_address;
	}
	t_ahead_unread_load = false;
	if (!ShaderRecompiler::IR::MaterializeResources(entry->second.resource_plan, runtime, resources,
	                                                specialization)) {
		ahead.reads.Fail();
		return nullptr;
	}
	NoteAheadWrites(entry->second.resource_plan, resources);
	const auto matches = [&](const ShaderModule& candidate) {
		const auto& layout = candidate.info.bindings;
		return layout.push_data_start_dword ==
		           ShaderRecompiler::IR::PushData::StartFor(push_data_cursor,
		                                                    layout.ShaderDataDwords()) &&
		       candidate.specialization == specialization;
	};
	auto& modules = entry->second.modules;
	auto& last    = entry->second.last_module[1];
	auto  module  = last < modules.size() && matches(modules[last])
	                    ? modules.begin() + static_cast<std::ptrdiff_t>(last)
	                    : std::ranges::find_if(modules, matches);
	if (module == modules.end()) {
		// A specialization made of stale bytes of GPU-written memory is usually one the execution
		// thread never asks for (in GTA V, V#s of garbage strides: thousands of permutations of
		// some compute shaders, each compiled, stored and given a pipeline). So is one whose walk
		// left a GPU-produced branch condition unread: it reached both sides, and the resources
		// of the side the shader does not run hold leftovers (in GTA SA, user data V#s with
		// ADD_TID or reserved dst_sel that no vertex program compiles). Only the execution
		// thread's own lookup compiles those.
		if (!ahead.reads.MayHaveReadGpuWrites() && !t_ahead_unread_load) {
			QueueCompile(params, input_info, push_data_cursor, &specialization);
		}
		ahead.reads.Fail();
		return nullptr;
	}
	if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
		// The program without the patch serves a dispatch without a TLAS: the execution thread
		// resolves it (see GetComputeProgram()).
		if (ReadsNullTlas(*module, resources)) {
			ahead.reads.Fail();
			return nullptr;
		}
	}
	last = static_cast<size_t>(module - modules.begin());
	input_info.stage = {.module = &*module, .resources = &resources};
	module->info.bindings.AdvancePushData(push_data_cursor);
	return &*module;
}

struct PipelineCache::CompileJob {
	ProgramKey                                                  key;
	ShaderParams                                                params;
	std::unique_ptr<ShaderVertexInputInfo>                      vertex;
	std::unique_ptr<ShaderPixelInputInfo>                       pixel;
	std::unique_ptr<ShaderComputeInputInfo>                     compute;
	uint32_t                                                    push_data_cursor = 0;
	std::optional<ShaderRecompiler::IR::ResourceSpecialization> specialization;
};

void PipelineCache::LockPrograms() {
	while (m_program_lock.test_and_set(std::memory_order_acquire)) {
		while (m_program_lock.test(std::memory_order_relaxed)) {
			Common::SpinPause();
		}
	}
}

void PipelineCache::UnlockPrograms() {
	m_program_lock.clear(std::memory_order_release);
}

template <typename InputInfo>
void PipelineCache::QueueCompile(const ShaderParams& params, const InputInfo& input_info,
                                 uint32_t                                            push_data_cursor,
                                 const ShaderRecompiler::IR::ResourceSpecialization* specialization) {
	if (m_in_flight.contains(m_ahead_key)) {
		return;
	}
	auto job              = std::make_unique<CompileJob>();
	job->key              = m_ahead_key;
	job->params           = params;
	job->push_data_cursor = push_data_cursor;
	if (specialization != nullptr) {
		job->specialization = *specialization;
	}
	if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
		job->vertex = std::make_unique<ShaderVertexInputInfo>(input_info);
	} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
		job->pixel = std::make_unique<ShaderPixelInputInfo>(input_info);
	} else {
		job->compute = std::make_unique<ShaderComputeInputInfo>(input_info);
	}
	m_in_flight[m_ahead_key]++;
	{
		std::lock_guard lock(m_job_mutex);
		m_jobs.push_back(std::move(job));
	}
	m_job_available.notify_one();
}

bool PipelineCache::WaitForCompile(const ProgramKey& key) {
	if (!m_in_flight.contains(key)) {
		return false;
	}
	const auto copy = key;
	do {
		UnlockPrograms();
		// The worker reads the shader code from guest memory: on a page the GPU owns, it faults
		// and waits for this thread to read the GPU's bytes back.
		GuestGpu::RunUrgentCommandsWhileWaiting();
		std::this_thread::yield();
		LockPrograms();
	} while (m_in_flight.contains(copy));
	return true;
}

template <typename InputInfo>
void PipelineCache::RunCompileJob(CompileJob& job, InputInfo& input_info) {
	std::optional<ShaderModule>                       module;
	std::optional<ShaderRecompiler::IR::ResourcePlan> plan;
	std::vector<uint32_t>                             spirv;
	std::vector<uint32_t>                             plain_spirv;
	ShaderRecompiler::CompileOptions                  options {.wave_size = input_info.wave_size};
	[&] {
		const auto user_data = std::span(job.params.user_data).first(job.params.user_data_count);
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.stage             = input_info.logical_stage;
			options.input_info.vertex = &input_info;
			options.user_data_base    = 8;
			if (options.stage == ShaderType::Mesh ||
			    options.stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
			}
			if (options.stage == ShaderType::Mesh) {
				options.wave_size = input_info.mesh.wave_size;
			}
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			options.stage            = ShaderType::Pixel;
			options.input_info.pixel = &input_info;
		} else {
			options.stage              = ShaderType::Compute;
			options.input_info.compute = &input_info;
		}
		options.shader_hash     = job.params.hash;
		options.user_data       = user_data;
		options.back_code       = job.params.back_code;
		options.dump_ir         = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.emit_debug_info = Config::GraphicsDebugDumpEnabled();
		options.native_ray_tracing = NativeRayTracing::PatchShaders();
		options.ray_stats          = options.native_ray_tracing && Config::RayTracingStatsEnabled();
		// SWAPPC programs link the functions they call from guest memory: the execution thread
		// compiles them.
		auto source = ShaderRecompiler::PrepareShaderSource(job.params.code, options);
		if (!source.calls.empty() || source.unsupported_call_pc != UINT32_MAX) {
			return;
		}
		auto translated = ShaderRecompiler::TranslateProgram(source.decoded, options);
		plan            = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
		auto specialization = job.specialization;
		if (!specialization) {
			GuestReadLog                           reads;
			ShaderRecompiler::IR::ResourceSnapshot resources;
			specialization.emplace();
			ShaderRecompiler::IR::SrtRuntime runtime {
			    .user_data                  = user_data,
			    .shader_base                = job.params.Base(),
			    .userdata                   = &reads,
			    .read_specialization_memory = ReadAheadStrict,
			    .record_load                = RecordAheadLoad,
			};
			if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
				runtime.workgroup_counts      = input_info.workgroup_counts;
				runtime.indirect_args_address = input_info.indirect_args_address;
			}
			if (!ShaderRecompiler::IR::MaterializeResources(*plan, runtime, resources,
			                                                *specialization)) {
				specialization.reset();
			}
		}
		if (specialization) {
			module = BuildModule(job.key, options, std::move(translated), *specialization, {},
			                     job.push_data_cursor, spirv, plain_spirv);
		}
	}();
	// A compute module added: its pipeline is created ahead of its dispatch.
	const ShaderModule* ahead = nullptr;
	LockPrograms();
	if (module) {
		auto entry = m_program_cache.find(job.key);
		if (entry == m_program_cache.end()) {
			entry = m_program_cache.try_emplace(job.key, Program {.resource_plan = std::move(*plan)})
			            .first;
		}
		auto&       modules = entry->second.modules;
		const auto& layout  = module->info.bindings;
		const bool  known   = std::ranges::any_of(modules, [&](const ShaderModule& other) {
			return other.specialization == module->specialization &&
			       other.info.bindings.push_data_start_dword == layout.push_data_start_dword;
		});
		if (known) {
			DestroyModule(*module);
		} else {
			auto& added = modules.emplace_back(std::move(*module));
			AddedModule(job.key, entry->second, added, spirv, plain_spirv, options, job.params.code,
			            job.push_data_cursor);
			if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
				ahead = &added;
			}
		}
	}
	if (const auto job_count = m_in_flight.find(job.key); --job_count->second == 0) {
		m_in_flight.erase(job_count);
	}
	UnlockPrograms();
	if (ahead != nullptr) {
		CreateComputePipelineAhead(*ahead, spirv);
	}
}

void PipelineCache::CompileWorker() {
	KYTY_PROFILER_THREAD("Thread_ShaderCompile");
	for (;;) {
		std::unique_ptr<CompileJob> job;
		{
			std::unique_lock lock(m_job_mutex);
			m_job_available.wait(lock, [&] { return m_stopping || !m_jobs.empty(); });
			if (m_stopping) {
				return;
			}
			job = std::move(m_jobs.front());
			m_jobs.pop_front();
		}
		if (job->vertex != nullptr) {
			RunCompileJob(*job, *job->vertex);
		} else if (job->pixel != nullptr) {
			RunCompileJob(*job, *job->pixel);
		} else {
			RunCompileJob(*job, *job->compute);
		}
	}
}

void PipelineCache::StopWorkers() {
	{
		std::lock_guard lock(m_job_mutex);
		m_stopping = true;
	}
	m_job_available.notify_all();
	m_workers.clear();
}

void PipelineCache::QueueAsyncPipeline(const AsyncPipelineJob& job) {
	{
		std::lock_guard lock(m_async_mutex);
		// A dispatch waits for its pipeline, a draw is only skipped: compute pipelines first.
		if (job.compute) {
			m_async_jobs.push_front(job);
		} else {
			m_async_jobs.push_back(job);
		}
	}
	m_async_available.notify_one();
}

template <typename Matches>
bool PipelineCache::TakeQueuedAsyncPipeline(Matches&& matches) {
	const auto job = std::ranges::find_if(m_async_jobs, matches);
	if (job == m_async_jobs.end()) {
		return false;
	}
	m_async_jobs.erase(job);
	return true;
}

void PipelineCache::StopAsyncPipelines() {
	{
		std::lock_guard lock(m_async_mutex);
		m_async_stopping = true;
		m_async_jobs.clear();
	}
	m_async_available.notify_all();
	m_async_threads.clear();
}

void PipelineCache::AsyncPipelineWorker() {
	KYTY_PROFILER_THREAD("Thread_PipelineAsync");
	for (;;) {
		AsyncPipelineJob job;
		{
			std::unique_lock lock(m_async_mutex);
			m_async_available.wait(lock, [&] { return m_async_stopping || !m_async_jobs.empty(); });
			if (m_async_stopping) {
				return;
			}
			job = m_async_jobs.front();
			m_async_jobs.pop_front();
		}
		Pipeline   pipeline;
		const auto started = std::chrono::steady_clock::now();
		if (job.compute) {
			CreatePipelineInternal(m_graphics, pipeline, *job.module, m_driver_cache);
		} else {
			CreatePipelineInternal(m_graphics, pipeline, job.key, job.stages, m_driver_cache);
		}
		const bool created = pipeline.pipeline != nullptr && pipeline.pipeline_layout != nullptr;
		if (created) {
			NoteCreatedPipeline(started);
		}
		std::lock_guard lock(m_async_mutex);
		if (job.compute) {
			if (created) {
				m_async_ready_compute.try_emplace(job.module->id, pipeline);
			}
			m_async_pending_compute.erase(job.module->id);
		} else {
			if (created) {
				m_async_ready.try_emplace(job.key, pipeline);
			} else {
				m_async_failed_graphics.insert(job.key);
			}
			m_async_pending_graphics.erase(job.key);
		}
	}
}

void PipelineCache::ClaimPipeline(const GraphicsPipelineKey& key) {
	if (m_manifest_warm) {
		m_claimed_graphics.insert(key);
	}
}

void PipelineCache::ClaimComputePipeline(uint64_t module_id) {
	if (m_manifest_warm) {
		m_claimed_compute.insert(module_id);
	}
}

void PipelineCache::CreateComputePipelineAhead(const ShaderModule&          module,
                                               const std::vector<uint32_t>& spirv) {
	if (m_helpers == nullptr && m_async_threads.empty()) {
		return;
	}
	{
		std::lock_guard lock(m_async_mutex);
		if (m_async_ready_compute.contains(module.id) ||
		    (m_manifest_warm && m_claimed_compute.contains(module.id)) ||
		    !m_async_pending_compute.insert(module.id).second) {
			return;
		}
		ClaimComputePipeline(module.id);
	}
	// The game has just compiled its shader: a dispatch will want it soon.
	if (CreateComputePipelineElsewhere(module, spirv, true)) {
		return;
	}
	if (m_async_threads.empty()) {
		// Nobody creates it ahead: the manifest or the dispatch does.
		std::lock_guard lock(m_async_mutex);
		m_async_pending_compute.erase(module.id);
		m_claimed_compute.erase(module.id);
		return;
	}
	QueueAsyncPipeline({.compute = true, .module = &module});
}

void PipelineCache::StartHelpers() {
	if (m_driver_cache == nullptr) {
		return;
	}
	m_helpers = PipelineHelpers::Start(m_graphics, PipelineHelperProcesses(m_graphics),
	                                   [this](PipelineHelpers::Result&& result) {
		                                   OnHelperResult(std::move(result));
	                                   });
}

void PipelineCache::StopHelpers() {
	if (m_helpers == nullptr) {
		return;
	}
	m_helpers->LogProgress();
	PipelineCacheLog("Pipeline helpers: {} compute pipelines found in the driver cache, {} created "
	                 "from a helper's cache data, {} compiled here after a helper failed, {} taken "
	                 "back by a dispatch",
	                 m_helper_found.load(), m_helper_created.load(), m_helper_compiled_here.load(),
	                 m_helper_taken_back.load());
	m_helpers.reset();
}

bool PipelineCache::CreateComputePipelineElsewhere(const ShaderModule&          module,
                                                   const std::vector<uint32_t>& spirv, bool urgent) {
	if (m_helpers == nullptr || spirv.empty()) {
		return false;
	}
	const auto started = std::chrono::steady_clock::now();
	Pipeline   pipeline;
	if (FindComputePipeline(m_graphics, pipeline, module, m_driver_cache)) {
		m_helper_found++;
		PublishComputePipeline(module, pipeline, started);
		return true;
	}
	{
		std::lock_guard lock(m_async_mutex);
		m_helper_jobs.emplace(module.id, &module);
	}
	m_helpers->Queue(module.id, EncodeComputeModule(module, spirv), urgent,
	                 UseRank(module.info.shader_hash));
	return true;
}

void PipelineCache::OnHelperResult(PipelineHelpers::Result&& result) {
	const ShaderModule* module = nullptr;
	{
		std::lock_guard lock(m_async_mutex);
		const auto      job = m_helper_jobs.find(result.ticket);
		// A dispatch took it back.
		if (job == m_helper_jobs.end()) {
			return;
		}
		module = job->second;
		m_helper_jobs.erase(job);
	}
	const auto started = std::chrono::steady_clock::now();
	Pipeline   pipeline;
	bool       found = false;
	if (result.ok && !result.cache_data.empty()) {
		vk::PipelineCacheCreateInfo create {};
		create.initialDataSize  = result.cache_data.size();
		create.pInitialData     = result.cache_data.data();
		vk::PipelineCache cache = nullptr;
		if (m_graphics.device.createPipelineCache(&create, nullptr, &cache) == vk::Result::eSuccess) {
			found = FindComputePipeline(m_graphics, pipeline, *module, cache);
			if (found) {
				std::lock_guard lock(m_helper_caches_mutex);
				m_helper_caches.push_back(cache);
			} else {
				m_graphics.device.destroyPipelineCache(cache, nullptr);
			}
		}
	}
	if (found) {
		m_helper_created++;
	} else {
		// The helper failed, or its pipeline is not one this device finds: created here.
		m_helper_compiled_here++;
		Common::SoftExitScope soft_exit;
		try {
			CreatePipelineInternal(m_graphics, pipeline, *module, m_driver_cache);
		} catch (const std::exception& error) {
			PipelineCacheLog("Pipeline helpers: cs {:016x}: {}", module->info.shader_hash, error.what());
			pipeline = {};
		}
	}
	PublishComputePipeline(*module, pipeline, started);
}

void PipelineCache::PublishComputePipeline(const ShaderModule& module, const Pipeline& pipeline,
                                           std::chrono::steady_clock::time_point started) {
	const bool created = pipeline.pipeline != nullptr && pipeline.pipeline_layout != nullptr;
	if (created) {
		NoteCreatedPipeline(started);
	}
	std::lock_guard lock(m_async_mutex);
	if (created) {
		m_async_ready_compute.try_emplace(module.id, pipeline);
	} else {
		// A dispatch creates it, and reports the error.
		m_claimed_compute.erase(module.id);
	}
	m_async_pending_compute.erase(module.id);
}

const ShaderModule* PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                                     ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, input_info);
	input_info.lds_storage = input_info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	// Indirect thread counts reach the shader through its shader data buffer.
	uint32_t push_data_cursor =
	    input_info.dispatch_indirect_threads ? ShaderRecompiler::IR::PushData::NoStart : 0u;
	const ProgramLockGuard lock(m_program_lock);
	const auto             cursor = push_data_cursor;
	const auto*            module = GetProgram(params, input_info, push_data_cursor);
	// A dispatch without a TLAS runs the program without the native ray tracing patch. The
	// patched one would take its guest traversal, but much slower: Astro Bot dispatches most of
	// its ray tracing passes so, and the patched cs 45f8ddf6 took 3.6 ms instead of 0.7.
	if (module != nullptr && ReadsNullTlas(*module, *input_info.stage.resources)) {
		push_data_cursor  = cursor;
		m_guest_traversal = true;
		module            = GetProgram(params, input_info, push_data_cursor);
		m_guest_traversal = false;
	}
	return module;
}

void PipelineCache::NoteAheadGuestWrite(uint64_t sequence, uint64_t address,
                                        std::span<const uint32_t> data, bool one_address) {
	if (address == 0 || data.empty()) {
		return;
	}
	auto& state    = t_ahead_guest_writes;
	auto& write    = state.writes.emplace_back();
	write.sequence = sequence;
	write.address  = address;
	if (one_address) {
		write.dwords.assign(1, data.back());
	} else {
		write.dwords.assign(data.begin(), data.end());
	}
	state.Count(write, 1);
}

void PipelineCache::RetireAheadGuestWrites(uint64_t executed) {
	auto& state = t_ahead_guest_writes;
	while (!state.writes.empty() && state.writes.front().sequence <= executed) {
		state.Count(state.writes.front(), -1);
		state.writes.pop_front();
	}
}

const ShaderModule* PipelineCache::GetComputeProgramAhead(const HW::ComputeShaderInfo& regs,
                                                          ShaderComputeInputInfo&      input_info,
                                                          ProgramResolution&           ahead) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto params             = PrepareProgram(regs, input_info);
	input_info.lds_storage = input_info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	uint32_t push_data_cursor =
	    input_info.dispatch_indirect_threads ? ShaderRecompiler::IR::PushData::NoStart : 0u;
	const ProgramLockGuard lock(m_program_lock);
	return GetAhead(params, input_info, push_data_cursor, ahead);
}

// Pipelines whose stages are this small (SPIR-V words, all stages) compile on the execution
// thread: a few milliseconds, and their draws are never skipped. A skipped draw is lost for good
// when the game draws it once into a cached layer: Wolverine's title UI drew its logo once, during
// the cold-start compile backlog, and the logo stayed missing.
constexpr uint64_t PipelineSyncWords = 12000;
// Draws without depth test or write whose vertex shaders total at most this many SPIR-V words
// also compile on the execution thread, whatever the pixel shader's size: those are UI and 2D
// layer draws, which a game may also draw only once. On cold starts Wolverine's frosted title
// panels lost their blur (PS 0x09161b28ae4039bb, 80k words) and showed as dark boxes. Stencil
// does not count: UI libraries clip their panels with stencil masks.
constexpr uint64_t PipelineSyncUiVsWords = 8000;
// How long a draw waits for a pipeline it has just queued before it is skipped: a pipeline the
// driver has cached finishes within it.
constexpr std::chrono::milliseconds PipelineWait {20};
// Color targets at most this many pixels wide and high hold glyphs, icons and cached layers.
constexpr uint32_t SmallTargetSync = 1024;

// Glyphs, icons and other cached layers are drawn once into small offscreen targets, and a draw
// skipped while its pipeline compiles is lost for good: Wolverine's Leap Attack prompt lost
// letters ("L p Att k") in every new game whose session compiled them, and on a cold cache the
// main menu showed only some letters and the screen-reader notice lost its black background.
bool SmallColorTargetDraw(const HW::Context& context) {
	constexpr uint32_t limit = SmallTargetSync;
	const auto mask  = context.GetRenderTargetMask();
	bool       found = false;
	for (uint32_t slot = 0; slot < 8; slot++) {
		const auto& target = context.GetRenderTarget(slot);
		if (((mask >> (slot * 4u)) & 0x0fu) == 0 || target.base.addr == 0) {
			continue;
		}
		if (target.attrib2.width + 1u > limit || target.attrib2.height + 1u > limit) {
			return false;
		}
		found = true;
	}
	return found;
}

PipelineCache::Pipeline* PipelineCache::GetGraphicsPipeline(const CommandBuffer&   command,
                                                            const DrawRenderState& state,
                                                            const DrawTargets&     targets,
                                                            bool                   allow_async) {
	const auto* colors        = targets.colors;
	const auto& depth         = targets.depth;
	const auto  vertex_info   = state.VertexStages();
	const auto& ps_input_info = state.ps_input_info;
	const auto& vs_input_info = vertex_info.front();
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	const bool ps_active    = state.ps_active;
	const bool tessellation = vertex_info.size() == 3;
	std::array<const ShaderModule*, 4> stages {};
	for (uint32_t i = 0; i < vertex_info.size(); i++) {
		stages[i] = vertex_info[i].stage.module;
	}
	if (ps_active) {
		stages[3] = ps_input_info.stage.module;
		if (state.ps_plain && stages[3] != nullptr && stages[3]->plain != nullptr) {
			stages[3] = stages[3]->plain.get();
		}
	}
	const bool mesh        = stages[0]->info.stage == ShaderType::Mesh;
	const auto color_count = targets.color_count;

	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	GraphicsPipelineKey key {};
	for (uint32_t i = 0; i < stages.size(); i++) {
		if (stages[i] != nullptr) {
			key.stage_ids[i] = stages[i]->id;
		}
	}
	auto&    static_params      = key.static_params;
	auto&    rendering          = key.rendering;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		if (colors[i].desc.info.guest_format == Prospero::BufferFormat::k11_11_10UNorm) {
			static_params.color_mask[slot] = 1u;
		}
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		auto alpha_source = ShaderAlphaBlendSource::None;
		if (slot == 0 && ps_active) {
			alpha_source = ps_input_info.alpha_blend_source;
		}
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && alpha_source == ShaderAlphaBlendSource::None &&
		    ClassifyBlendMapping(bc, colors[i].export_mapping) != BlendMappingSupport::Direct) {
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned = false;
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

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = state.topology;
	static_params.primitive_restart_enable = state.primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info.ps_sample_shading;
	static_params.alpha_to_coverage_enable = ps_active && ctx.GetShaderRegisters().AlphaToMask();
	const bool rect_list = Prospero::IsRectList(command.GetUserConfig().GetPrimType());
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, !rect_list && mc.cull_front, !rect_list && mc.cull_back);
	if (tessellation) {
		static_params.patch_control_points = vs_input_info.tess.input_control_points;
	} else if (!mesh && static_params.topology == vk::PrimitiveTopology::ePatchList) {
		// Rectangle lists expand through generated tessellation stages.
		static_params.patch_control_points = 3;
	}

	if (!mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			EXIT_IF(binding < 0 || binding >= vs_input_info.buffers_num);
			key.vertex_input.attributes[attribute] = {
			    .offset = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                    vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
	}

	// Consecutive draws mostly use the pipeline of the draw before.
	if (m_last_graphics_pipeline != nullptr && key == m_last_graphics_key) {
		return m_last_graphics_pipeline;
	}
	if (const auto found = m_graphics_pipelines.find(key); found != m_graphics_pipelines.end()) {
		m_last_graphics_key      = key;
		m_last_graphics_pipeline = &found->second;
		if (!found->second.use_noted) {
			NoteUse(found->second, stages);
		}
		return &found->second;
	}
	if (allow_async) {
		// Small pipelines, and UI or layer draws, compile at once (PipelineSyncWords).
		uint64_t vertex_words = 0;
		for (uint32_t i = 0; i < 3u; i++) {
			if (stages[i] != nullptr) {
				vertex_words += stages[i]->spirv_words;
			}
		}
		const uint64_t spirv_words =
		    vertex_words + (stages[3] != nullptr ? stages[3]->spirv_words : 0u);
		const bool layer_draw = !depth.depth_test_enable && !depth.depth_write_enable &&
		                        vertex_words <= PipelineSyncUiVsWords;
		allow_async = spirv_words > PipelineSyncWords && !layer_draw;
	}
	if (m_async_handoff) {
		// Without background threads for them, draws wait for their pipelines.
		allow_async = allow_async && !m_async_threads.empty();
		std::unique_lock lock(m_async_mutex);
		// Set when this draw queues the pipeline: it waits that long before it is skipped.
		std::optional<std::chrono::steady_clock::time_point> wait_until;
		for (;;) {
			if (auto node = m_async_ready.extract(key); !node.empty()) {
				lock.unlock();
				auto& ready              = m_graphics_pipelines.insert(std::move(node)).position->second;
				m_last_graphics_key      = key;
				m_last_graphics_pipeline = &ready;
				StorePipeline(key);
				NoteUse(ready, stages);
				return &ready;
			}
			if (!m_async_pending_graphics.contains(key)) {
				if (!allow_async || m_async_failed_graphics.contains(key)) {
					ClaimPipeline(key);
					break;
				}
				// The driver compiles a new pipeline for up to half a second: the draws that need
				// it are skipped until a background thread created it.
				m_async_pending_graphics.insert(key);
				ClaimPipeline(key);
				lock.unlock();
				QueueAsyncPipeline({.key = key, .stages = stages});
				wait_until = std::chrono::steady_clock::now() + PipelineWait;
				lock.lock();
				continue;
			}
			if (allow_async) {
				if (!wait_until || std::chrono::steady_clock::now() >= *wait_until) {
					return nullptr;
				}
				lock.unlock();
				std::this_thread::yield();
				lock.lock();
				continue;
			}
			// A draw that cannot be skipped creates the pipeline itself while it is still queued
			// (the queue may hold many pipelines of skipped draws), or waits for the thread
			// creating it.
			if (TakeQueuedAsyncPipeline([&](const AsyncPipelineJob& job) {
				    return !job.compute && job.key == key;
			    })) {
				m_async_pending_graphics.erase(key);
				ClaimPipeline(key);
				break;
			}
			lock.unlock();
			std::this_thread::yield();
			lock.lock();
		}
	}
	auto& pipeline = m_graphics_pipelines[key];

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(ps_input_info);
		}
	}

	const auto started = std::chrono::steady_clock::now();
	{
		Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::PipelinesCreated,
		                                         Common::DebugCounters::Counter::PipelineCreateNs,
		                                         Common::DebugCounters::Peak::PipelineCreateNs);
		CreatePipelineInternal(m_graphics, pipeline, key, stages, m_driver_cache);
	}
	StorePipeline(key);
	NoteCreatedPipeline(started);
	NoteUse(pipeline, stages);
	m_last_graphics_key      = key;
	m_last_graphics_pipeline = &pipeline;

	return &pipeline;
}

PipelineCache::Pipeline& PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	const auto& module = *input_info.stage.module;

	const ShaderModule* const stages[] = {&module};
	if (const auto found = m_compute_pipelines.find(module.id); found != m_compute_pipelines.end()) {
		if (!found->second.use_noted) {
			NoteUse(found->second, stages);
		}
		return found->second;
	}
	if (m_async_handoff) {
		// Dispatches always run: take the pipeline a background thread created, create it here
		// while it is still queued, or wait for the thread creating it.
		std::unique_lock lock(m_async_mutex);
		for (;;) {
			if (auto node = m_async_ready_compute.extract(module.id); !node.empty()) {
				lock.unlock();
				auto& ready = m_compute_pipelines.insert(std::move(node)).position->second;
				NoteUse(ready, stages);
				return ready;
			}
			if (!m_async_pending_compute.contains(module.id)) {
				ClaimComputePipeline(module.id);
				break;
			}
			if (TakeQueuedAsyncPipeline([&](const AsyncPipelineJob& job) {
				    return job.compute && job.module->id == module.id;
			    })) {
				m_async_pending_compute.erase(module.id);
				ClaimComputePipeline(module.id);
				break;
			}
			// A helper's job still queued is created here. Once a helper has it, its result is
			// awaited: creating it again here would start from nothing (GTA V's take up to ~90 s),
			// and a helper that returns nothing for a long time is ended, and its job created by
			// OnHelperResult.
			bool in_helper = false;
			if (const auto job = m_helper_jobs.find(module.id); job != m_helper_jobs.end()) {
				if (m_helpers->Withdraw(module.id)) {
					m_helper_jobs.erase(job);
					m_async_pending_compute.erase(module.id);
					ClaimComputePipeline(module.id);
					m_helper_taken_back++;
					break;
				}
				in_helper = true;
			}
			lock.unlock();
			if (in_helper) {
				std::this_thread::sleep_for(HelperPoll);
			} else {
				std::this_thread::yield();
			}
			lock.lock();
		}
	}
	auto& pipeline = m_compute_pipelines[module.id];

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	const auto started = std::chrono::steady_clock::now();
	{
		Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::PipelinesCreated,
		                                         Common::DebugCounters::Counter::PipelineCreateNs,
		                                         Common::DebugCounters::Peak::PipelineCreateNs);
		CreatePipelineInternal(m_graphics, pipeline, module, m_driver_cache);
	}
	NoteCreatedPipeline(started);
	NoteUse(pipeline, stages);

	return pipeline;
}
} // namespace Libs::Graphics
