#include "graphics/host_gpu/renderer/image/colorClearHelper.h"

#include "common/assert.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_color_clear_classify_spv.h"
#include "gpu_blit_shaders/gpu_color_clear_sint_spv.h"
#include "gpu_blit_shaders/gpu_color_clear_spv.h"
#include "gpu_blit_shaders/gpu_color_clear_uint_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

#include <algorithm>
#include <string_view>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

struct Record {
	uint32_t                vertex_count   = 0;
	uint32_t                instance_count = 0;
	uint32_t                first_vertex   = 0;
	uint32_t                first_instance = 0;
	std::array<uint32_t, 4> color {};
};
static_assert(sizeof(Record) == 32);

struct ClassifyParams {
	std::array<std::array<uint32_t, 4>, ColorClearHelper::MaxCandidates> colors {};
	std::array<uint32_t, 2>                                              codes {};
	uint32_t                                                             slice_words = 0;
	uint32_t                                                             code_count  = 0;
	uint32_t                                                             consume     = 0;
};
static_assert(sizeof(ClassifyParams) == 100);

} // namespace

ColorClearHelper::ColorClearHelper(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_records(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                MaxSlices * sizeof(Record)) {
	SetVulkanObjectNameF(m_graphics.device, m_records.Handle(), "Kyty.ColorClearRecords");

	const vk::DescriptorSetLayoutBinding classify_bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = static_cast<uint32_t>(std::size(classify_bindings));
	layout_info.pBindings    = classify_bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_classify_layout),
	    "create colour clear classify descriptor layout");
	const vk::PushConstantRange classify_push {vk::ShaderStageFlagBits::eCompute, 0,
	                                           sizeof(ClassifyParams)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_classify_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &classify_push;
	RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                            &m_classify_pipeline_layout),
	                     "create colour clear classify pipeline layout");

	const vk::DescriptorSetLayoutBinding draw_binding {
	    0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment, nullptr};
	layout_info.bindingCount = 1;
	layout_info.pBindings    = &draw_binding;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_draw_layout),
	    "create colour clear draw descriptor layout");
	const vk::PushConstantRange draw_push {vk::ShaderStageFlagBits::eFragment, 0,
	                                       sizeof(uint32_t)};
	pipeline_layout_info.pSetLayouts         = &m_draw_layout;
	pipeline_layout_info.pPushConstantRanges = &draw_push;
	RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                            &m_draw_pipeline_layout),
	                     "create colour clear draw pipeline layout");

	m_vertex_shader        = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, m_graphics.device);
	m_fragment_shader      = CompileSPV(GPU_COLOR_CLEAR_SPV, m_graphics.device);
	m_fragment_shader_uint = CompileSPV(GPU_COLOR_CLEAR_UINT_SPV, m_graphics.device);
	m_fragment_shader_sint = CompileSPV(GPU_COLOR_CLEAR_SINT_SPV, m_graphics.device);
}

ColorClearHelper::~ColorClearHelper() {
	for (const auto& pipeline: m_draw_pipelines) {
		m_graphics.device.destroyPipeline(pipeline.handle, nullptr);
	}
	if (m_classify_pipeline != nullptr) {
		m_graphics.device.destroyPipeline(m_classify_pipeline, nullptr);
	}
	m_graphics.device.destroyShaderModule(m_fragment_shader_sint, nullptr);
	m_graphics.device.destroyShaderModule(m_fragment_shader_uint, nullptr);
	m_graphics.device.destroyShaderModule(m_fragment_shader, nullptr);
	m_graphics.device.destroyShaderModule(m_vertex_shader, nullptr);
	m_graphics.device.destroyPipelineLayout(m_draw_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_draw_layout, nullptr);
	m_graphics.device.destroyPipelineLayout(m_classify_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_classify_layout, nullptr);
}

bool ColorClearHelper::SupportsFormat(vk::Format format) const {
	if (format == vk::Format::eUndefined || vk::componentCount(format) == 0) {
		return false;
	}
	const auto properties = m_graphics.GetFormatProperties(format);
	return static_cast<bool>(properties.optimalTilingFeatures &
	                         vk::FormatFeatureFlagBits::eColorAttachment);
}

vk::Pipeline ColorClearHelper::GetClassifyPipeline() {
	if (m_classify_pipeline != nullptr) {
		return m_classify_pipeline;
	}
	const auto module = CompileSPV(GPU_COLOR_CLEAR_CLASSIFY_SPV, m_graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_classify_pipeline_layout;
	const auto cache     = m_scheduler.Context().GetPipelineCache().DriverCache();
	const auto result    = m_graphics.device.createComputePipelines(cache, 1, &pipeline_info,
	                                                                nullptr, &m_classify_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create colour clear classify pipeline");
	return m_classify_pipeline;
}

vk::Pipeline ColorClearHelper::GetDrawPipeline(PipelineKey key) {
	const auto cached = std::ranges::find(m_draw_pipelines, key, &Pipeline::key);
	if (cached != m_draw_pipelines.end()) {
		return cached->handle;
	}
	const auto samples = vulkan_sample_count(key.samples);
	EXIT_IF(samples == vk::SampleCountFlagBits {} || key.format == vk::Format::eUndefined);

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = m_vertex_shader;
	stages[0].pName  = "main";
	// The fragment output's numeric type has to match the attachment's.
	const std::string_view numeric = vk::componentNumericFormat(key.format, 0);
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = numeric == "UINT"   ? m_fragment_shader_uint
	                   : numeric == "SINT" ? m_fragment_shader_sint
	                                       : m_fragment_shader;
	stages[1].pName  = "main";

	vk::PipelineVertexInputStateCreateInfo   vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = samples;
	vk::PipelineColorBlendAttachmentState blend_attachment {};
	blend_attachment.colorWriteMask =
	    vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo color_blend {};
	color_blend.attachmentCount = 1;
	color_blend.pAttachments    = &blend_attachment;
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();

	vk::PipelineRenderingCreateInfo rendering {};
	rendering.colorAttachmentCount    = 1;
	rendering.pColorAttachmentFormats = &key.format;

	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &input_assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pColorBlendState    = &color_blend;
	create.pDynamicState       = &dynamic;
	create.layout              = m_draw_pipeline_layout;

	vk::Pipeline pipeline = nullptr;
	RequireVulkanSuccess(m_graphics.device.createGraphicsPipelines(
	                         m_scheduler.Context().GetPipelineCache().DriverCache(), 1, &create,
	                         nullptr, &pipeline),
	                     "create colour clear draw pipeline");
	m_draw_pipelines.push_back({key, pipeline});
	return pipeline;
}

void ColorClearHelper::Classify(vk::CommandBuffer command, vk::Buffer metadata, uint64_t offset,
                                uint64_t slice_size, uint32_t slices,
                                std::span<const Candidate> candidates, bool consume) {
	EXIT_IF(command == nullptr || metadata == nullptr || slices == 0 || slices > MaxSlices ||
	        slice_size == 0 || slice_size % sizeof(uint32_t) != 0 ||
	        slice_size / sizeof(uint32_t) > UINT32_MAX || candidates.empty() ||
	        candidates.size() > MaxCandidates);
	ClassifyParams params {};
	for (uint32_t i = 0; i < candidates.size(); i++) {
		params.colors[i] = candidates[i].color;
		params.codes[i / 4u] |= static_cast<uint32_t>(candidates[i].code) << (8u * (i % 4u));
	}
	params.slice_words = static_cast<uint32_t>(slice_size / sizeof(uint32_t));
	params.code_count  = static_cast<uint32_t>(candidates.size());
	params.consume     = consume ? 1u : 0u;

	const vk::DescriptorBufferInfo infos[] {
	    {metadata, offset, slice_size * slices},
	    {m_records.Handle(), 0, static_cast<vk::DeviceSize>(slices) * sizeof(Record)},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}

	// The prior barrier also retires the previous materialization's reads of the records.
	vk::MemoryBarrier2 before {};
	before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	before.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
	before.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	before.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	vk::MemoryBarrier2 after {};
	after.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	after.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &before;
	command.pipelineBarrier2(dependency);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, GetClassifyPipeline());
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_classify_pipeline_layout, 0,
	                             writes);
	command.pushConstants(m_classify_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                      sizeof(params), &params);
	command.dispatch(slices, 1, 1);
	dependency.pMemoryBarriers = &after;
	command.pipelineBarrier2(dependency);
}

void ColorClearHelper::Draw(vk::CommandBuffer command, vk::ImageView view, vk::Format format,
                            uint32_t samples, vk::Extent2D extent, uint32_t slice) {
	EXIT_IF(command == nullptr || view == nullptr || slice >= MaxSlices || extent.width == 0 ||
	        extent.height == 0);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = view;
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);

	const vk::DescriptorBufferInfo info {m_records.Handle(), 0, m_records.Size()};
	vk::WriteDescriptorSet         write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eStorageBuffer;
	write.pBufferInfo     = &info;
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, GetDrawPipeline({samples, format}));
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_draw_pipeline_layout, 0, 1,
	                             &write);
	command.pushConstants(m_draw_pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
	                      sizeof(slice), &slice);
	const vk::Viewport viewport {0.0f, 0.0f, static_cast<float>(extent.width),
	                             static_cast<float>(extent.height), 0.0f, 1.0f};
	const vk::Rect2D   scissor {{0, 0}, extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.drawIndirect(m_records.Handle(), static_cast<vk::DeviceSize>(slice) * sizeof(Record),
	                     1, sizeof(Record));
	command.endRendering();
}

} // namespace Libs::Graphics
