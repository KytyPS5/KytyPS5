#include "graphics/host_gpu/renderer/image/blitHelper.h"

#include "common/assert.h"
#include "gpu_blit_shaders/gpu_blit_color_to_depth_spv.h"
#include "gpu_blit_shaders/gpu_blit_color_to_ms_depth_spv.h"
#include "gpu_blit_shaders/gpu_blit_depth_to_color_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics {

BlitHelper::BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	vk::DescriptorSetLayoutBinding texture_binding {};
	texture_binding.binding         = 0;
	texture_binding.descriptorType  = vk::DescriptorType::eSampledImage;
	texture_binding.descriptorCount = 1;
	texture_binding.stageFlags      = vk::ShaderStageFlagBits::eFragment;

	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = 1;
	descriptor_info.pBindings    = &texture_binding;
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_descriptor_layout),
	                     "create BlitHelper descriptor layout");

	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts    = &m_descriptor_layout;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_pipeline_layout),
	    "create BlitHelper pipeline layout");

	m_vertex_shader   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, m_graphics.device);
	m_fragment_shader = CompileSPV(GPU_BLIT_COLOR_TO_MS_DEPTH_SPV, m_graphics.device);
	m_copy_shader     = CompileSPV(GPU_BLIT_COLOR_TO_DEPTH_SPV, m_graphics.device);
}

BlitHelper::~BlitHelper() {
	for (const auto& pipeline: m_pipelines) {
		m_graphics.device.destroyPipeline(pipeline.handle, nullptr);
	}
	if (m_to_color_pipeline != nullptr) {
		m_graphics.device.destroyPipeline(m_to_color_pipeline, nullptr);
		m_graphics.device.destroyPipelineLayout(m_to_color_layout, nullptr);
		m_graphics.device.destroyDescriptorSetLayout(m_to_color_descriptor_layout, nullptr);
	}
	m_graphics.device.destroyShaderModule(m_copy_shader, nullptr);
	m_graphics.device.destroyShaderModule(m_fragment_shader, nullptr);
	m_graphics.device.destroyShaderModule(m_vertex_shader, nullptr);
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
}

vk::Pipeline BlitHelper::GetPipeline(PipelineKey key) {
	const auto cached = std::ranges::find(m_pipelines, key, &Pipeline::key);
	if (cached != m_pipelines.end()) {
		return cached->handle;
	}

	const auto samples = vulkan_sample_count(key.samples);
	EXIT_IF(samples == vk::SampleCountFlagBits {} || key.format == vk::Format::eUndefined);

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = m_vertex_shader;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = key.copy ? m_copy_shader : m_fragment_shader;
	stages[1].pName  = "main";

	vk::PipelineVertexInputStateCreateInfo vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = samples;
	vk::PipelineDepthStencilStateCreateInfo depth {};
	depth.depthTestEnable  = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp   = vk::CompareOp::eAlways;
	vk::PipelineColorBlendStateCreateInfo color_blend {};
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();

	vk::PipelineRenderingCreateInfo rendering {};
	rendering.depthAttachmentFormat = key.format;

	vk::GraphicsPipelineCreateInfo create {};
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
	    "create color-to-depth pipeline");
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
	depth_attachment.imageView               = destination_view;
	depth_attachment.imageLayout             = ColorToMsDepthLayout;
	depth_attachment.loadOp                  = vk::AttachmentLoadOp::eClear;
	depth_attachment.storeOp                 = vk::AttachmentStoreOp::eStore;
	depth_attachment.clearValue.depthStencil = {0.0f, 0};

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent = {destination_info.extent.width, destination_info.extent.height};
	rendering.layerCount        = 1;
	rendering.pDepthAttachment  = &depth_attachment;
	command.beginRendering(&rendering);

	vk::DescriptorImageInfo descriptor_image {};
	descriptor_image.imageView   = source_view;
	descriptor_image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	vk::WriteDescriptorSet descriptor_write {};
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
	// The helper pipeline has static state that invalidates the guest draw's dynamic state.
	m_scheduler.GetDynamicState().Invalidate();
}

bool BlitHelper::CanCopyDepthColor(const Image& depth, const Image& color) {
	const auto& d = depth.backing;
	const auto& c = color.backing;
	// Color images other than R32_UINT are read and written through R32_UINT views.
	const bool color_format =
	    c.format == vk::Format::eR32Uint ||
	    ((c.format == vk::Format::eR32Sint || c.format == vk::Format::eR32Sfloat) &&
	     static_cast<bool>(c.flags & vk::ImageCreateFlagBits::eMutableFormat));
	constexpr auto color_usage =
	    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
	constexpr auto depth_usage =
	    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eDepthStencilAttachment;
	return color_format &&
	       (d.format == vk::Format::eD32Sfloat || d.format == vk::Format::eD32SfloatS8Uint) &&
	       (c.usage & color_usage) == color_usage && (d.usage & depth_usage) == depth_usage &&
	       d.image_type == vk::ImageType::e2D && c.image_type == vk::ImageType::e2D &&
	       d.samples == 1 && c.samples == 1 && d.mip_levels == 1 && c.mip_levels == 1 &&
	       d.layers == 1 && c.layers == 1 && d.extent.width == c.extent.width &&
	       d.extent.height == c.extent.height && d.image != nullptr && c.image != nullptr;
}

void BlitHelper::CopyDepthToColor(Image& depth, Image& color) {
	EXIT_IF(!CanCopyDepthColor(depth, color));
	m_scheduler.EndRendering();
	if (m_to_color_pipeline == nullptr) {
		std::array<vk::DescriptorSetLayoutBinding, 2> bindings {};
		bindings[0].binding         = 0;
		bindings[0].descriptorType  = vk::DescriptorType::eSampledImage;
		bindings[0].descriptorCount = 1;
		bindings[0].stageFlags      = vk::ShaderStageFlagBits::eCompute;
		bindings[1].binding         = 1;
		bindings[1].descriptorType  = vk::DescriptorType::eStorageImage;
		bindings[1].descriptorCount = 1;
		bindings[1].stageFlags      = vk::ShaderStageFlagBits::eCompute;
		vk::DescriptorSetLayoutCreateInfo descriptor_info {};
		descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
		descriptor_info.pBindings    = bindings.data();
		RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(
		                         &descriptor_info, nullptr, &m_to_color_descriptor_layout),
		                     "create depth-to-color descriptor layout");
		vk::PipelineLayoutCreateInfo layout_info {};
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts    = &m_to_color_descriptor_layout;
		RequireVulkanSuccess(
		    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_to_color_layout),
		    "create depth-to-color pipeline layout");
		const auto module = CompileSPV(GPU_BLIT_DEPTH_TO_COLOR_SPV, m_graphics.device);
		vk::ComputePipelineCreateInfo create {};
		create.stage.stage  = vk::ShaderStageFlagBits::eCompute;
		create.stage.module = module;
		create.stage.pName  = "main";
		create.layout       = m_to_color_layout;
		RequireVulkanSuccess(m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr,
		                                                              &m_to_color_pipeline),
		                     "create depth-to-color pipeline");
		m_graphics.device.destroyShaderModule(module, nullptr);
	}

	ImageViewInfo depth_view_info {};
	depth_view_info.format = depth.backing.format;
	depth_view_info.type   = vk::ImageViewType::e2D;
	depth_view_info.aspect = vk::ImageAspectFlagBits::eDepth;
	depth_view_info.usage  = vk::ImageUsageFlagBits::eSampled;
	ImageViewInfo color_view_info {};
	color_view_info.format = vk::Format::eR32Uint;
	color_view_info.type   = vk::ImageViewType::e2D;
	color_view_info.aspect = vk::ImageAspectFlagBits::eColor;
	color_view_info.usage  = vk::ImageUsageFlagBits::eStorage;
	const auto depth_view  = depth.FindView(depth_view_info);
	const auto color_view  = color.FindView(color_view_info);

	auto command = m_scheduler.Current().Handle();
	depth.Transit(vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	              vk::AccessFlagBits2::eShaderSampledRead, {}, command);
	color.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderStorageWrite, {}, command);
	std::array<vk::DescriptorImageInfo, 2> images {};
	images[0].imageView   = depth_view;
	images[0].imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
	images[1].imageView   = color_view;
	images[1].imageLayout = vk::ImageLayout::eGeneral;
	std::array<vk::WriteDescriptorSet, 2> writes {};
	writes[0].dstBinding      = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType  = vk::DescriptorType::eSampledImage;
	writes[0].pImageInfo      = &images[0];
	writes[1].dstBinding      = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType  = vk::DescriptorType::eStorageImage;
	writes[1].pImageInfo      = &images[1];
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_to_color_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_to_color_pipeline);
	command.dispatch((color.backing.extent.width + 7u) / 8u,
	                 (color.backing.extent.height + 7u) / 8u, 1);
}

void BlitHelper::CopyColorToDepth(Image& color, Image& depth) {
	EXIT_IF(!CanCopyDepthColor(depth, color));
	m_scheduler.EndRendering();

	ImageViewInfo color_view_info {};
	color_view_info.format = vk::Format::eR32Uint;
	color_view_info.type   = vk::ImageViewType::e2D;
	color_view_info.aspect = vk::ImageAspectFlagBits::eColor;
	color_view_info.usage  = vk::ImageUsageFlagBits::eSampled;
	ImageViewInfo depth_view_info {};
	depth_view_info.format = depth.backing.format;
	depth_view_info.type   = vk::ImageViewType::e2D;
	depth_view_info.aspect = vk::ImageAspectFlagBits::eDepth;
	depth_view_info.usage  = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	const auto color_view  = color.FindView(color_view_info);
	const auto depth_view  = depth.FindView(depth_view_info);

	auto command = m_scheduler.Current().Handle();
	color.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderSampledRead,
	              {}, command);
	// The pass loads the depth (loadOp LOAD) and writes it: the barrier makes the earlier writes
	// visible to the attachment read of the load too.
	depth.Transit(vk::ImageLayout::eDepthStencilAttachmentOptimal,
	              vk::AccessFlagBits2::eDepthStencilAttachmentRead |
	                  vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
	              {}, command);

	// Without a stencil attachment the stencil aspect is not accessed.
	const auto                  width  = depth.backing.extent.width;
	const auto                  height = depth.backing.extent.height;
	vk::RenderingAttachmentInfo depth_attachment {};
	depth_attachment.imageView   = depth_view;
	depth_attachment.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
	depth_attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	depth_attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent = vk::Extent2D {width, height};
	rendering.layerCount        = 1;
	rendering.pDepthAttachment  = &depth_attachment;
	command.beginRendering(&rendering);

	vk::DescriptorImageInfo descriptor_image {};
	descriptor_image.imageView   = color_view;
	descriptor_image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	vk::WriteDescriptorSet descriptor_write {};
	descriptor_write.dstBinding      = 0;
	descriptor_write.descriptorCount = 1;
	descriptor_write.descriptorType  = vk::DescriptorType::eSampledImage;
	descriptor_write.pImageInfo      = &descriptor_image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_pipeline_layout, 0, 1,
	                             &descriptor_write);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics,
	                     GetPipeline({.samples = 1, .format = depth.backing.format, .copy = true}));
	const vk::Viewport viewport {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
	                             0.0f, 1.0f};
	const vk::Rect2D   scissor {{0, 0}, {width, height}};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
	m_scheduler.GetDynamicState().Invalidate();
}

} // namespace Libs::Graphics
