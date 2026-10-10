#include "graphics/host_gpu/renderer/cache/pageDiff.h"

#include "common/assert.h"
#include "gpu_blit_shaders/gpu_page_diff_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <array>
#include <iterator>

namespace Libs::Graphics {

namespace {

struct Params {
	uint32_t words      = 0;
	uint32_t page_words = 0;
};

} // namespace

PageDiff::PageDiff(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = static_cast<uint32_t>(std::size(bindings));
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_layout),
	    "create page diff descriptor layout");
	const vk::PushConstantRange  push {vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_pipeline_layout),
	    "create page diff pipeline layout");
}

PageDiff::~PageDiff() {
	if (m_pipeline != nullptr) {
		m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	}
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_layout, nullptr);
}

vk::Pipeline PageDiff::GetPipeline() {
	if (m_pipeline != nullptr) {
		return m_pipeline;
	}
	const auto                        module = CompileSPV(GPU_PAGE_DIFF_SPV, m_graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_pipeline_layout;
	const auto cache     = m_scheduler.Context().GetPipelineCache().DriverCache();
	const auto result =
	    m_graphics.device.createComputePipelines(cache, 1, &pipeline_info, nullptr, &m_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create page diff pipeline");
	return m_pipeline;
}

void PageDiff::Compare(vk::CommandBuffer command, vk::Buffer current, uint64_t current_offset,
                       vk::Buffer snapshot, vk::Buffer changed, uint64_t size) {
	const auto pages = size / PageSize;
	EXIT_IF(command == nullptr || size == 0 || size % PageSize != 0 || pages > MaxPages);
	const Params                   params {static_cast<uint32_t>(size / sizeof(uint32_t)),
	                                       static_cast<uint32_t>(PageSize / sizeof(uint32_t))};
	const vk::DescriptorBufferInfo infos[] {
	    {current, current_offset, size},
	    {snapshot, 0, size},
	    {changed, 0, (pages + 31u) / 32u * sizeof(uint32_t)},
	};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	vk::MemoryBarrier2 before {};
	before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	before.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
	before.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	before.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	vk::MemoryBarrier2 after {};
	after.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	after.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	after.dstStageMask =
	    vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost;
	after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eHostRead;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &before;
	command.pipelineBarrier2(dependency);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, GetPipeline());
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, writes);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
	                      &params);
	command.dispatch(static_cast<uint32_t>(pages), 1, 1);
	dependency.pMemoryBarriers = &after;
	command.pipelineBarrier2(dependency);
}

} // namespace Libs::Graphics
