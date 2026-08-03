#include "graphics/host_gpu/renderer/image/blitHelper.h"

#include "common/assert.h"
#include "gpu_blit_shaders/gpu_blit_color_to_ms_depth_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace Libs::Graphics {

BlitHelper::BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	vk::DescriptorSetLayoutBinding texture_binding {};
	texture_binding.binding         = 0;
	texture_binding.descriptorType  = vk::DescriptorType::eSampledImage;
	texture_binding.descriptorCount = 1;
	texture_binding.stageFlags      = vk::ShaderStageFlagBits::eFragment;

	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.sType        = vk::StructureType::eDescriptorSetLayoutCreateInfo;
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = 1;
	descriptor_info.pBindings    = &texture_binding;
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_descriptor_layout),
	                     "create BlitHelper descriptor layout");

	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.sType          = vk::StructureType::ePipelineLayoutCreateInfo;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts    = &m_descriptor_layout;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_pipeline_layout),
	    "create BlitHelper pipeline layout");

	m_vertex_shader = CreateShader(GPU_BLIT_FS_TRIANGLE_SPV, std::size(GPU_BLIT_FS_TRIANGLE_SPV));
	m_fragment_shader =
	    CreateShader(GPU_BLIT_COLOR_TO_MS_DEPTH_SPV, std::size(GPU_BLIT_COLOR_TO_MS_DEPTH_SPV));
}

BlitHelper::~BlitHelper() {
	for (const auto& pipeline: m_pipelines) {
		m_graphics.device.destroyPipeline(pipeline.handle, nullptr);
	}
	if (m_fragment_shader != nullptr) {
		m_graphics.device.destroyShaderModule(m_fragment_shader, nullptr);
	}
	if (m_vertex_shader != nullptr) {
		m_graphics.device.destroyShaderModule(m_vertex_shader, nullptr);
	}
	if (m_pipeline_layout != nullptr) {
		m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	}
	if (m_descriptor_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
	}
}

vk::ShaderModule BlitHelper::CreateShader(const uint32_t* code, size_t words) const {
	EXIT_IF(code == nullptr || words == 0);
	vk::ShaderModuleCreateInfo create {};
	create.sType            = vk::StructureType::eShaderModuleCreateInfo;
	create.codeSize         = words * sizeof(uint32_t);
	create.pCode            = code;
	vk::ShaderModule module = nullptr;
	RequireVulkanSuccess(m_graphics.device.createShaderModule(&create, nullptr, &module),
	                     "create BlitHelper shader module");
	return module;
}

vk::Pipeline BlitHelper::GetPipeline(PipelineKey key) {
	const auto cached = std::ranges::find(m_pipelines, key, &Pipeline::key);
	if (cached != m_pipelines.end()) {
		return cached->handle;
	}

	const auto samples = vulkan_sample_count(key.samples);
	EXIT_IF(samples == vk::SampleCountFlagBits {} || key.format == vk::Format::eUndefined);

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].sType  = vk::StructureType::ePipelineShaderStageCreateInfo;
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = m_vertex_shader;
	stages[0].pName  = "main";
	stages[1].sType  = vk::StructureType::ePipelineShaderStageCreateInfo;
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = m_fragment_shader;
	stages[1].pName  = "main";

	vk::PipelineVertexInputStateCreateInfo vertex_input {};
	vertex_input.sType = vk::StructureType::ePipelineVertexInputStateCreateInfo;
	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.sType    = vk::StructureType::ePipelineInputAssemblyStateCreateInfo;
	input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.sType         = vk::StructureType::ePipelineViewportStateCreateInfo;
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.sType       = vk::StructureType::ePipelineRasterizationStateCreateInfo;
	rasterization.polygonMode = vk::PolygonMode::eFill;
	rasterization.cullMode    = vk::CullModeFlagBits::eNone;
	rasterization.lineWidth   = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.sType                = vk::StructureType::ePipelineMultisampleStateCreateInfo;
	multisample.rasterizationSamples = samples;
	vk::PipelineDepthStencilStateCreateInfo depth {};
	depth.sType            = vk::StructureType::ePipelineDepthStencilStateCreateInfo;
	depth.depthTestEnable  = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp   = vk::CompareOp::eAlways;
	vk::PipelineColorBlendStateCreateInfo color_blend {};
	color_blend.sType = vk::StructureType::ePipelineColorBlendStateCreateInfo;
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.sType             = vk::StructureType::ePipelineDynamicStateCreateInfo;
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();

	vk::PipelineRenderingCreateInfo rendering {};
	rendering.sType                 = vk::StructureType::ePipelineRenderingCreateInfo;
	rendering.depthAttachmentFormat = key.format;

	vk::GraphicsPipelineCreateInfo create {};
	create.sType               = vk::StructureType::eGraphicsPipelineCreateInfo;
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &input_assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pDepthStencilState  = &depth;
	create.pColorBlendState    = &color_blend;
	create.pDynamicState       = &dynamic;
	create.layout              = m_pipeline_layout;

	vk::Pipeline pipeline = nullptr;
	RequireVulkanSuccess(
	    m_graphics.device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipeline),
	    "create color-to-MS-depth pipeline");
	m_pipelines.push_back({key, pipeline});
	return pipeline;
}

void BlitHelper::ReinterpretColorAsMsDepth(Image& source, Image& destination) {
	const auto& source_info      = source.info;
	const auto& destination_info = destination.info;
	EXIT_IF(DepthAspectTransferFormat(source_info.pixel_format) != vk::Format::eUndefined ||
	        DepthAspectTransferFormat(destination_info.pixel_format) == vk::Format::eUndefined ||
	        source_info.samples != 1 || destination_info.samples <= 1 ||
	        destination_info.samples > 4 || source.backing.image_type != vk::ImageType::e2D ||
	        destination.backing.image_type != vk::ImageType::e2D ||
	        source_info.extent.width != destination_info.extent.width ||
	        source_info.extent.height != destination_info.extent.height ||
	        source_info.extent.depth != 1 || destination_info.extent.depth != 1 ||
	        source.backing.image == nullptr || destination.backing.image == nullptr);
	m_scheduler.EndRendering();

	ImageViewInfo source_view_info {};
	source_view_info.format = source_info.pixel_format;
	source_view_info.type   = vk::ImageViewType::e2D;
	source_view_info.aspect = vk::ImageAspectFlagBits::eColor;
	source_view_info.usage  = vk::ImageUsageFlagBits::eSampled;
	const auto source_view  = source.FindView(source_view_info);

	ImageViewInfo destination_view_info {};
	destination_view_info.format = destination_info.pixel_format;
	destination_view_info.type   = vk::ImageViewType::e2D;
	destination_view_info.aspect = vk::ImageAspectFlagBits::eDepth;
	destination_view_info.usage  = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	const auto destination_view  = destination.FindView(destination_view_info);

	auto& command_buffer = m_scheduler.Current();
	auto  command        = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {},
	               command);
	destination.Transit(ColorToMsDepthLayout, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {},
	                    command);

	vk::RenderingAttachmentInfo depth_attachment {};
	depth_attachment.sType                   = vk::StructureType::eRenderingAttachmentInfo;
	depth_attachment.imageView               = destination_view;
	depth_attachment.imageLayout             = ColorToMsDepthLayout;
	depth_attachment.loadOp                  = vk::AttachmentLoadOp::eClear;
	depth_attachment.storeOp                 = vk::AttachmentStoreOp::eStore;
	depth_attachment.clearValue.depthStencil = {0.0f, 0};

	vk::RenderingInfo rendering {};
	rendering.sType             = vk::StructureType::eRenderingInfo;
	rendering.renderArea.extent = {destination_info.extent.width, destination_info.extent.height};
	rendering.layerCount        = 1;
	rendering.pDepthAttachment  = &depth_attachment;
	command.beginRendering(&rendering);

	vk::DescriptorImageInfo descriptor_image {};
	descriptor_image.imageView   = source_view;
	descriptor_image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	vk::WriteDescriptorSet descriptor_write {};
	descriptor_write.sType           = vk::StructureType::eWriteDescriptorSet;
	descriptor_write.dstBinding      = 0;
	descriptor_write.descriptorCount = 1;
	descriptor_write.descriptorType  = vk::DescriptorType::eSampledImage;
	descriptor_write.pImageInfo      = &descriptor_image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_pipeline_layout, 0, 1,
	                             &descriptor_write);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics,
	                     GetPipeline({destination_info.samples, destination_info.pixel_format}));

	const vk::Viewport viewport {0.0f,
	                             0.0f,
	                             static_cast<float>(destination_info.extent.width),
	                             static_cast<float>(destination_info.extent.height),
	                             0.0f,
	                             1.0f};
	const vk::Rect2D   scissor {{0, 0},
	                            {destination_info.extent.width, destination_info.extent.height}};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

} // namespace Libs::Graphics
