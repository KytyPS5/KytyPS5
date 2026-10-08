#include "graphics/host_gpu/renderer/indirectDraw.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "gpu_tiler_shaders/gpu_indirect_draw_prepare_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Libs::Graphics {

namespace {

constexpr uint64_t CommandRingSize = 4ull * 1024 * 1024;
constexpr uint32_t StateCount      = 4096;
constexpr uint32_t StateDwords     = 4;

constexpr uint32_t FlagIndexed       = 1u;
constexpr uint32_t FlagCountIndirect = 2u;
constexpr uint32_t FlagKeepOffsets   = 4u;

struct PrepareParameters {
	uint32_t max_count     = 0;
	uint32_t stride_dwords = 0;
	uint32_t flags         = 0;
	uint32_t index_limit   = 0;
};

uint64_t CommandBytes(uint32_t max_count, bool indexed) {
	return (1ull + uint64_t {max_count} * (indexed ? IndirectDrawPrepare::IndexedCommandDwords
	                                               : IndirectDrawPrepare::AutoCommandDwords)) *
	       sizeof(uint32_t);
}

} // namespace

IndirectDrawPrepare::IndirectDrawPrepare(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_commands(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                 vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
                 CommandRingSize),
      m_states(graphics, scheduler, MemoryUsage::Download, 0,
               vk::BufferUsageFlagBits::eStorageBuffer,
               uint64_t {StateCount} * StateDwords * sizeof(uint32_t)),
      m_state_ticks(StateCount, 0) {
	SetVulkanObjectNameF(m_graphics.device, m_commands.Handle(), "Kyty.IndirectDrawCommands");
	SetVulkanObjectNameF(m_graphics.device, m_states.Handle(), "Kyty.IndirectDrawStates");
	std::array<vk::DescriptorSetLayoutBinding, 4> bindings {};
	for (uint32_t index = 0; index < bindings.size(); index++) {
		bindings[index] = {index, vk::DescriptorType::eStorageBuffer, 1,
		                   vk::ShaderStageFlagBits::eCompute, nullptr};
	}
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_info.pBindings    = bindings.data();
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_set_layout),
	    "create indirect draw descriptor layout");
	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                         sizeof(PrepareParameters)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_pipeline_layout),
	    "create indirect draw pipeline layout");
	const auto module = CompileSPV(GPU_INDIRECT_DRAW_PREPARE_SPV, m_graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_pipeline_layout;
	const auto result =
	    m_graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create indirect draw pipeline");
}

IndirectDrawPrepare::~IndirectDrawPrepare() {
	m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_set_layout, nullptr);
}

bool IndirectDrawPrepare::Fits(const Request& request) const {
	return request.max_count != 0 &&
	       CommandBytes(request.max_count, request.indexed) <= m_commands.Size() / 2;
}

IndirectDrawPrepare::Commands IndirectDrawPrepare::Record(vk::CommandBuffer command,
                                                          const Request&    request) {
	EXIT_IF(!Fits(request) || request.arguments == nullptr || request.stride % 4 != 0);
	const auto alignment = std::max<uint64_t>(
	    m_graphics.GetPhysicalDeviceProperties().limits.minStorageBufferOffsetAlignment, 16);
	const auto bytes = CommandBytes(request.max_count, request.indexed);
	auto       start = Common::AlignUp(m_cursor, alignment);
	if (start + bytes > m_commands.Size()) {
		start = 0;
	}
	m_cursor = start + bytes;

	const auto state = m_next_state;
	m_next_state     = (m_next_state + 1) % StateCount;
	if (const auto tick = m_state_ticks[state]; tick != 0 && !m_scheduler.IsFree(tick)) {
		m_scheduler.Wait(tick);
	}
	m_state_ticks[state]    = m_scheduler.CurrentTick();
	const auto state_offset = uint64_t {state} * StateDwords * sizeof(uint32_t);
	const auto state_bytes  = uint64_t {StateDwords} * sizeof(uint32_t);
	std::memset(m_states.Mapped().data() + state_offset, 0, state_bytes);
	m_states.Flush(state_offset, state_bytes);

	PrepareParameters parameters {.max_count     = request.max_count,
	                              .stride_dwords = request.stride / 4,
	                              .flags = (request.indexed ? FlagIndexed : 0u) |
	                                       (request.count != nullptr ? FlagCountIndirect : 0u) |
	                                       (request.keep_offsets ? FlagKeepOffsets : 0u),
	                              .index_limit = request.index_limit};

	// Argument writes become visible; earlier draws stop reading the reused command range.
	vk::MemoryBarrier2 before {};
	before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	before.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
	before.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	before.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &before;
	command.pipelineBarrier2(dependency);

	const auto* count = request.count != nullptr ? request.count : request.arguments;
	const vk::DescriptorBufferInfo infos[] {
	    {request.arguments->Handle(), request.arguments_offset, request.arguments_size},
	    {count->Handle(),
	     request.count != nullptr ? request.count_offset : request.arguments_offset,
	     sizeof(uint32_t)},
	    {m_commands.Handle(), start, bytes},
	    {m_states.Handle(), state_offset, state_bytes},
	};
	std::array<vk::WriteDescriptorSet, 4> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, writes);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                      sizeof(parameters), &parameters);
	command.dispatch((request.max_count + 63) / 64, 1, 1);

	vk::MemoryBarrier2 after {};
	after.srcStageMask         = vk::PipelineStageFlagBits2::eComputeShader;
	after.srcAccessMask        = vk::AccessFlagBits2::eShaderWrite;
	after.dstStageMask         = vk::PipelineStageFlagBits2::eDrawIndirect |
	                             vk::PipelineStageFlagBits2::eAllCommands |
	                             vk::PipelineStageFlagBits2::eHost;
	after.dstAccessMask        = vk::AccessFlagBits2::eIndirectCommandRead |
	                             vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eHostRead;
	dependency.pMemoryBarriers = &after;
	command.pipelineBarrier2(dependency);
	return {.buffer          = m_commands.Handle(),
	        .count_offset    = start,
	        .commands_offset = start + sizeof(uint32_t),
	        .state           = state};
}

IndirectDrawPrepare::Latched IndirectDrawPrepare::Read(uint32_t state) {
	EXIT_IF(state >= StateCount || m_state_ticks[state] == 0);
	m_scheduler.Wait(m_state_ticks[state]);
	Check(state);
	const auto offset = uint64_t {state} * StateDwords * sizeof(uint32_t);
	uint32_t   words[StateDwords] {};
	std::memcpy(words, m_states.Mapped().data() + offset, sizeof(words));
	return {.executed = words[0] != 0, .instances = words[1]};
}

void IndirectDrawPrepare::Check(uint32_t state) {
	const auto offset = uint64_t {state} * StateDwords * sizeof(uint32_t);
	m_states.Invalidate(offset, uint64_t {StateDwords} * sizeof(uint32_t));
	uint32_t errors = 0;
	std::memcpy(&errors, m_states.Mapped().data() + offset + 2 * sizeof(uint32_t), sizeof(errors));
	if (errors != 0) {
		EXIT("indirect draw indices exceed INDEX_BUFFER_SIZE (errors=0x%x)\n", errors);
	}
}

} // namespace Libs::Graphics
