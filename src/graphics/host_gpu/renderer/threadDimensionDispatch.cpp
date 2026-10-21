#include "graphics/host_gpu/renderer/threadDimensionDispatch.h"

#include "common/assert.h"
#include "gpu_blit_shaders/dispatch_thread_dimensions_spv.h"
#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics {

namespace {

struct Push {
	std::array<uint32_t, 4> group_size {};
	uint32_t                args_dword   = 0;
	uint32_t                extent_dword = 0;
};

} // namespace

ThreadDimensionDispatch::ThreadDimensionDispatch(GraphicContext& graphics): m_graphics(graphics) {
	std::array<vk::DescriptorSetLayoutBinding, 3> bindings {};
	for (uint32_t index = 0; index < bindings.size(); ++index) {
		bindings[index] = {index, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute,
		                   nullptr};
	}
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_info.pBindings    = bindings.data();
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_descriptor_layout),
	                     "create thread-dimension dispatch descriptor layout");

	const vk::PushConstantRange push_range {vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_descriptor_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_pipeline_layout),
	                     "create thread-dimension dispatch pipeline layout");

	const auto module = CompileSPV(DISPATCH_THREAD_DIMENSIONS_SPV, m_graphics.device);
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName  = "main";
	pipeline_info.layout       = m_pipeline_layout;
	const auto result = m_graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create thread-dimension dispatch pipeline");
	SetVulkanObjectNameF(m_graphics.device, m_pipeline, "Thread-dimension dispatch arguments");
}

ThreadDimensionDispatch::~ThreadDimensionDispatch() {
	m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
}

void ThreadDimensionDispatch::Record(vk::CommandBuffer command, vk::Buffer args_buffer, uint64_t args_offset,
                                     std::array<uint32_t, 3> group_size, const Target& groups,
                                     const Target& extent, uint32_t extent_dword) {
	EXIT_IF((args_offset & 3u) != 0);
	// The arguments need not be at an offset a storage descriptor can start at.
	const auto args_start = args_offset / m_graphics.StorageMinAlignment() * m_graphics.StorageMinAlignment();
	const std::array infos {
	    vk::DescriptorBufferInfo {args_buffer, args_start, args_offset - args_start + 3u * sizeof(uint32_t)},
	    vk::DescriptorBufferInfo {groups.buffer, groups.offset, groups.size},
	    vk::DescriptorBufferInfo {extent.buffer, extent.offset, extent.size},
	};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	Push push {};
	push.group_size   = {group_size[0], group_size[1], group_size[2], 1u};
	push.args_dword   = static_cast<uint32_t>((args_offset - args_start) / sizeof(uint32_t));
	push.extent_dword = extent_dword;

	// Earlier commands may have written the arguments.
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	before.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader |
	                            vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &before, 0, nullptr, 0, nullptr);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, writes);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
	command.dispatch(1, 1, 1);
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eShaderRead |
	                      vk::AccessFlagBits::eUniformRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eDrawIndirect | vk::PipelineStageFlagBits::eComputeShader,
	                        {}, 1, &after, 0, nullptr, 0, nullptr);
}

} // namespace Libs::Graphics
