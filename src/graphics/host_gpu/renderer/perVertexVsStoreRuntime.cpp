#include "graphics/host_gpu/renderer/perVertexVsStoreRuntime.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <spirv-tools/libspirv.hpp>

namespace Libs::Graphics {
namespace {

// 4M vec4 = 64 MiB per slab; a frame needs a few slabs at most (draws that find none are counted as
// ring fallbacks).
constexpr uint64_t kSlabVec4s     = uint64_t {1} << 22u;
constexpr uint32_t kMaxSlabs      = 8;
constexpr uint64_t kStatsInterval = 8192;

struct StoreShader {
	ShaderType            stage = ShaderType::Unknown;
	std::vector<uint32_t> words;    // empty: emission or validation failed
	uint32_t              mask = 0; // host locations the stage stores (VS) / reads per vertex (PS)
};
std::mutex                                                 s_mutex;
std::map<uint64_t, StoreShader>                            s_shaders;
std::map<std::pair<uint64_t, uint64_t>, PvVsStorePrograms> s_pairs;
vk::DescriptorSetLayout                                    s_set_layout = nullptr;
std::atomic<bool>                                          s_enabled {false};

bool HasPerVertexParameterInput(const ShaderRecompiler::IR::Program& program) {
	return std::ranges::any_of(program.info.inputs, [](const auto& input) {
		return input.per_vertex && input.kind == ShaderRecompiler::IR::StageInputKind::Parameter;
	});
}

void LogStats() {
	const auto& stats = PvVsStoreStatsRef();
	const auto  get   = [&](PvVsStoreFallback f) {
		return stats.by_fallback[static_cast<size_t>(f)].load();
	};
	LOGF("PvVsStore: draws=%llu (instanced=%llu records=%llu) fallback: topology=%llu range=%llu "
	     "instanced=%llu "
	     "ring=%llu param31=%llu interface=%llu index_tail=%llu scanned_indices=%llu\n",
	     static_cast<unsigned long long>(get(PvVsStoreFallback::None)),
	     static_cast<unsigned long long>(stats.instanced_draws.load()),
	     static_cast<unsigned long long>(stats.records.load()),
	     static_cast<unsigned long long>(get(PvVsStoreFallback::Topology)),
	     static_cast<unsigned long long>(get(PvVsStoreFallback::Range)),
	     static_cast<unsigned long long>(get(PvVsStoreFallback::Instanced)),
	     static_cast<unsigned long long>(get(PvVsStoreFallback::Ring)),
	     static_cast<unsigned long long>(get(PvVsStoreFallback::Param31)),
	     static_cast<unsigned long long>(get(PvVsStoreFallback::Interface)),
	     static_cast<unsigned long long>(get(PvVsStoreFallback::IndexTail)),
	     static_cast<unsigned long long>(stats.scanned_indices.load()));
}

} // namespace

PvVsStoreStats& PvVsStoreStatsRef() {
	static PvVsStoreStats stats;
	return stats;
}

void CountPvVsStoreDraw(PvVsStoreFallback fallback) {
	auto& stats = PvVsStoreStatsRef();
	stats.by_fallback[static_cast<size_t>(fallback)].fetch_add(1, std::memory_order_relaxed);
	uint64_t total = 0;
	for (const auto& count: stats.by_fallback)
		total += count.load(std::memory_order_relaxed);
	if (total == 1 || total % kStatsInterval == 0) LogStats();
}

bool PvVsStoreEnabled() {
	return s_enabled.load(std::memory_order_relaxed);
}

bool ResolvePvVsStoreEnabled(bool fragment_shader_barycentric, bool moltenvk) {
	const auto mode    = ParsePvVsStoreSwitch(std::getenv("KYTY_PV_VSSTORE"));
	const bool enabled = PvVsStoreEnabledFor(fragment_shader_barycentric, moltenvk, mode);
	s_enabled.store(enabled, std::memory_order_relaxed);
	LOGF("PvVsStore: %s (fragment_shader_barycentric=%s moltenvk=%s KYTY_PV_VSSTORE=%s)\n",
	     enabled ? "enabled" : "disabled", fragment_shader_barycentric ? "true" : "false",
	     moltenvk ? "true" : "false", PvVsStoreSwitchName(mode));
	return enabled;
}

void RememberPvVsStoreShader(const ShaderProgram& program, ShaderRecompiler::CompileResult& result,
                             const ShaderRecompiler::CompileOptions& options,
                             uint32_t                                push_data_start_dword) {
	if (!PvVsStoreEnabled()) return;
	using Role        = ShaderRecompiler::Spirv::PvStoreInfo::Role;
	const bool vertex = options.stage == ShaderType::Vertex && options.input_info.vertex != nullptr;
	const bool pixel  = options.stage == ShaderType::Pixel && options.input_info.pixel != nullptr &&
	                    HasPerVertexParameterInput(result.program);
	if (!vertex && !pixel) return;
	StoreShader entry;
	entry.stage = options.stage;
	ShaderRecompiler::Spirv::PvStoreInfo info;
	info.role          = vertex ? Role::Vertex : Role::Pixel;
	info.coherent      = true; // Coherent cap + a barrier after the VS stores
	info.location_mask = &entry.mask;
	// Emitting again writes the same backend ids and descriptor indices into the program, so the
	// layout the pipeline is built from does not change.
	auto words = ShaderRecompiler::Spirv::EmitProgram(result.program, options.input_info,
	                                                  push_data_start_dword, &info);
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	tools.SetMessageConsumer(
	    [&](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
		    LOGF("PvVsStore: %s 0x%016llx SPIR-V word %zu: %s\n", vertex ? "VS" : "PS",
		         static_cast<unsigned long long>(program.id), position.index, message);
	    });
	if (tools.Validate(words)) {
		entry.words = std::move(words);
	} else {
		LOGF("PvVsStore: %s 0x%016llx variant rejected, its draws keep the PerVertexKHR path\n",
		     vertex ? "VS" : "PS", static_cast<unsigned long long>(program.id));
	}
	std::lock_guard lock(s_mutex);
	s_shaders.emplace(program.id, std::move(entry));
}

vk::DescriptorSetLayout PvVsStoreSetLayout(GraphicContext& graphics) {
	if (s_set_layout != nullptr) return s_set_layout;
	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1,
	     vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment}};
	vk::DescriptorSetLayoutCreateInfo create {};
	create.bindingCount = 2;
	create.pBindings    = bindings;
	EXIT_IF(graphics.device.createDescriptorSetLayout(&create, nullptr, &s_set_layout) !=
	        vk::Result::eSuccess);
	return s_set_layout;
}

uint32_t PvVsStorePushOffset() {
	return ShaderRecompiler::IR::NativePushConstantSize;
}

const PvVsStorePrograms* GetPvVsStorePrograms(GraphicContext&                        graphics,
                                              const PipelineCache::GraphicsPrograms& programs) {
	if (!PvVsStoreEnabled() || programs.VertexStageCount() != 1) return nullptr;
	const auto      key = std::pair {programs.vertex[0].id, programs.pixel.id};
	std::lock_guard lock(s_mutex);
	if (const auto it = s_pairs.find(key); it != s_pairs.end()) {
		return it->second.stride_vec4 == UINT32_MAX ? nullptr : &it->second;
	}
	PvVsStorePrograms value;
	const auto        vs = s_shaders.find(key.first);
	const auto        ps = s_shaders.find(key.second);
	if (vs == s_shaders.end() || ps == s_shaders.end() || vs->second.words.empty() ||
	    ps->second.words.empty()) {
		value.stride_vec4 = UINT32_MAX; // negative cache: not a candidate
		s_pairs.emplace(key, value);
		return nullptr;
	}
	value.verdict = ClassifyPvVsStoreInterface(vs->second.mask, ps->second.mask);
	if (value.verdict == PvVsStoreInterfaceVerdict::Use) {
		value.stride_vec4               = PvVsStoreStrideFor(vs->second.mask);
		value.graphics                  = programs;
		value.graphics.vertex[0].module = CompileSPV(vs->second.words, graphics.device);
		value.graphics.pixel.module     = CompileSPV(ps->second.words, graphics.device);
		// Distinct pipeline-cache identity from the PerVertexKHR variants of the same programs.
		value.graphics.vertex[0].id |= 1ull << 61;
		value.graphics.pixel.id |= 1ull << 61;
		value.graphics.pv_store_layout = PvVsStoreSetLayout(graphics);
		LOGF("PvVsStore: pair VS=%llu PS=%llu stride=%u vec4\n",
		     static_cast<unsigned long long>(key.first),
		     static_cast<unsigned long long>(key.second), value.stride_vec4);
	}
	return &s_pairs.emplace(key, std::move(value)).first->second;
}

void ShutdownPvVsStore(vk::Device device) {
	std::lock_guard lock(s_mutex);
	for (auto& [key, value]: s_pairs) {
		// Only an accepted pair owns modules (CompileSPV); the rest of `graphics` is borrowed.
		if (value.verdict != PvVsStoreInterfaceVerdict::Use) continue;
		if (value.graphics.vertex[0].module != nullptr)
			device.destroyShaderModule(value.graphics.vertex[0].module, nullptr);
		if (value.graphics.pixel.module != nullptr)
			device.destroyShaderModule(value.graphics.pixel.module, nullptr);
	}
	s_pairs.clear();
	s_shaders.clear();
	if (s_set_layout != nullptr) device.destroyDescriptorSetLayout(s_set_layout, nullptr);
	s_set_layout = nullptr;
}

PvVsStoreRuntime::PvVsStoreRuntime(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler), m_slabs(kSlabVec4s, kMaxSlabs) {}

PvVsStoreRuntime::~PvVsStoreRuntime() = default;

PvVsStoreRuntime::Region PvVsStoreRuntime::Place(uint64_t records, uint32_t stride_vec4) {
	Region region;
	if (stride_vec4 == 0 || records == 0) return region;
	// The shaders address record * stride + slot, so a region starts at a multiple of the stride.
	const uint64_t vec4s = records * stride_vec4 + stride_vec4;
	const auto placement = m_slabs.Place(vec4s, m_scheduler.CurrentTick(),
	                                     [&](uint64_t tick) { return m_scheduler.IsFree(tick); });
	if (!placement.ok) return region;
	if (placement.created) {
		EXIT_IF(placement.slab != m_buffers.size());
		m_buffers.push_back(std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Stream,
		                                             0, AllFlags, kSlabVec4s * 16));
	}
	const uint64_t aligned =
	    (uint64_t {placement.base} + stride_vec4 - 1) / stride_vec4 * stride_vec4;
	region.ok          = true;
	region.slab        = placement.slab;
	region.base_record = static_cast<uint32_t>(aligned / stride_vec4);
	region.buffer      = m_buffers.at(placement.slab)->Handle();
	region.buffer_size = m_buffers.at(placement.slab)->Size();
	return region;
}

void PvVsStoreRuntime::Touch(const Region& region) {
	m_slabs.Touch(region.slab, m_scheduler.CurrentTick());
}

} // namespace Libs::Graphics
