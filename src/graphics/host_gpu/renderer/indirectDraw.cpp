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

constexpr uint32_t FlagIndexed       = 1u;
constexpr uint32_t FlagCountIndirect = 2u;
constexpr uint32_t FlagKeepOffsets   = 4u;
constexpr uint32_t FlagInstances     = 8u;

struct PrepareParameters {
	uint32_t max_count     = 0;
	uint32_t stride_dwords = 0;
	uint32_t flags         = 0;
	// Dword offsets of the arguments and the count inside their aligned descriptors.
	uint32_t arguments_base = 0;
	uint32_t count_base     = 0;
	uint32_t instances      = 0;
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
               vk::BufferUsageFlagBits::eStorageBuffer, sizeof(uint32_t)) {
	SetVulkanObjectNameF(m_graphics.device, m_commands.Handle(), "Kyty.IndirectDrawCommands");
	SetVulkanObjectNameF(m_graphics.device, m_states.Handle(), "Kyty.IndirectDrawInstances");
	// The only host write, before any GPU access: NUM_INSTANCES resets to 1.
	const uint32_t instances = 1;
	std::memcpy(m_states.Mapped().data(), &instances, sizeof(instances));
	m_states.Flush(0, sizeof(instances));
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
	const auto* count = request.count != nullptr ? request.count : request.arguments;
	const auto  count_offset =
	    request.count != nullptr ? request.count_offset : request.arguments_offset;
	EXIT_IF(request.arguments_offset % 4 != 0 || count_offset % 4 != 0);
	const auto storage_alignment = std::max<uint64_t>(
	    m_graphics.GetPhysicalDeviceProperties().limits.minStorageBufferOffsetAlignment, 4);
	const auto bytes = CommandBytes(request.max_count, request.indexed);
	auto       start = Common::AlignUp(m_cursor, std::max<uint64_t>(storage_alignment, 16));
	if (start + bytes > m_commands.Size()) {
		start = 0;
	}
	m_cursor = start + bytes;

	// Guest addresses are only dword aligned: bind from an aligned offset, index past it.
	const auto arguments_descriptor =
	    Common::AlignDown(request.arguments_offset, storage_alignment);
	const auto        count_descriptor = Common::AlignDown(count_offset, storage_alignment);
	const auto        arguments_base   = request.arguments_offset - arguments_descriptor;
	const auto        count_base       = count_offset - count_descriptor;
	PrepareParameters parameters {.max_count     = request.max_count,
	                              .stride_dwords = request.stride / 4,
	                              .flags = (request.indexed ? FlagIndexed : 0u) |
	                                       (request.count != nullptr ? FlagCountIndirect : 0u) |
	                                       (request.keep_offsets ? FlagKeepOffsets : 0u) |
	                                       (request.instances.has_value() ? FlagInstances : 0u),
	                              .arguments_base = static_cast<uint32_t>(arguments_base / 4),
	                              .count_base     = static_cast<uint32_t>(count_base / 4),
	                              .instances      = request.instances.value_or(0)};

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

	const vk::DescriptorBufferInfo infos[] {
	    {request.arguments->Handle(), arguments_descriptor,
	     arguments_base + request.arguments_size},
	    {count->Handle(), count_descriptor, count_base + sizeof(uint32_t)},
	    {m_commands.Handle(), start, bytes},
	    {m_states.Handle(), 0, sizeof(uint32_t)},
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
	m_state_tick = m_scheduler.CurrentTick();
	return {.buffer          = m_commands.Handle(),
	        .count_offset    = start,
	        .commands_offset = start + sizeof(uint32_t)};
}

uint32_t IndirectDrawPrepare::ReadInstances() {
	EXIT_IF(m_state_tick == 0);
	m_scheduler.Wait(m_state_tick);
	m_states.Invalidate(0, sizeof(uint32_t));
	uint32_t instances = 0;
	std::memcpy(&instances, m_states.Mapped().data(), sizeof(instances));
	return instances;
}

} // namespace Libs::Graphics
