#include "graphics/host_gpu/renderer/threadDispatch.h"

#include "common/assert.h"
#include "common/common.h"
#include "gpu_blit_shaders/gpu_dispatch_threads_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>

namespace Libs::Graphics {

namespace {

struct ConvertParameters {
	uint32_t source_word;
	uint32_t target_word;
	uint32_t local_size[3];
	uint32_t max_groups[3];
};

} // namespace

ThreadDispatcher::ThreadDispatcher(RenderContext& context)
    : m_context(context),
      m_records(context.GetGraphics(), context.GetCommandScheduler(), MemoryUsage::DeviceLocal, 0,
                vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                    vk::BufferUsageFlagBits::eTransferDst |
                    vk::BufferUsageFlagBits::eShaderDeviceAddress,
                uint64_t {RecordBytes} * RecordCount) {
	auto&                                graphics = context.GetGraphics();
	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_set_layout),
	    "create thread-dispatch descriptor layout");

	const vk::PushConstantRange  push {vk::ShaderStageFlagBits::eCompute, 0,
	                                   sizeof(ConvertParameters)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push;
	RequireVulkanSuccess(
	    graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_pipeline_layout),
	    "create thread-dispatch pipeline layout");

	const auto                    module = CompileSPV(GPU_DISPATCH_THREADS_SPV, graphics.device);
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName  = "main";
	pipeline_info.layout       = m_pipeline_layout;
	const auto result          = graphics.device.createComputePipelines(
	    context.GetPipelineCache().DriverCache(), 1, &pipeline_info, nullptr, &m_pipeline);
	graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create thread-dispatch pipeline");
	SetVulkanObjectNameF(graphics.device, m_pipeline, "Thread Dispatch Records");
}

ThreadDispatcher::~ThreadDispatcher() {
	auto& device = m_context.GetGraphics().device;
	device.destroyPipeline(m_pipeline, nullptr);
	device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	device.destroyDescriptorSetLayout(m_set_layout, nullptr);
}

ThreadDispatcher::Record ThreadDispatcher::Next() {
	const auto offset = vk::DeviceSize {m_next} * RecordBytes;
	m_next            = (m_next + 1u) % RecordCount;
	return {m_records.Handle(), offset, m_records.BufferDeviceAddress() + offset + 16u};
}

ThreadDispatcher::Record ThreadDispatcher::Write(vk::CommandBuffer       command,
                                                 std::array<uint32_t, 3> threads) {
	const auto     record = Next();
	const uint32_t words[3] {threads[0], threads[1], threads[2]};
	command.updateBuffer(record.buffer, record.groups_offset + 16u, sizeof(words), words);
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &barrier, 0, nullptr,
	                        0, nullptr);
	return record;
}

ThreadDispatcher::Record ThreadDispatcher::Convert(vk::CommandBuffer command, vk::Buffer source,
                                                   vk::DeviceSize          source_offset,
                                                   std::array<uint32_t, 3> local_size,
                                                   std::array<uint32_t, 3> max_groups) {
	const auto                     record    = Next();
	const auto                     alignment = m_context.GetGraphics().StorageMinAlignment();
	const auto                     aligned   = source_offset / alignment * alignment;
	const vk::DescriptorBufferInfo infos[] {
	    {source, aligned, source_offset - aligned + 12u},
	    {record.buffer, 0, m_records.Size()},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	const ConvertParameters parameters {static_cast<uint32_t>((source_offset - aligned) / 4u),
	                                    static_cast<uint32_t>(record.groups_offset / 4u),
	                                    {local_size[0], local_size[1], local_size[2]},
	                                    {max_groups[0], max_groups[1], max_groups[2]}};

	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite |
	                       vk::AccessFlagBits::eShaderRead |
	                       vk::AccessFlagBits::eIndirectCommandRead;
	before.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &before, 0, nullptr,
	                        0, nullptr);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, writes);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                      sizeof(parameters), &parameters);
	command.dispatch(1, 1, 1);
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask =
	    vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eShaderRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eDrawIndirect |
	                            vk::PipelineStageFlagBits::eComputeShader,
	                        {}, 1, &after, 0, nullptr, 0, nullptr);
	return record;
}

} // namespace Libs::Graphics
