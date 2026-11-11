#include "graphics/host_gpu/renderer/image/blitHelper.h"

#include "common/assert.h"
#include "gpu_blit_shaders/gpu_blit_color_to_ms_depth_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_eqaa_color_expand_spv.h"
#include "gpu_blit_shaders/gpu_eqaa_color_select_spv.h"
#include "gpu_blit_shaders/gpu_eqaa_depth_expand_spv.h"
#include "gpu_blit_shaders/gpu_eqaa_depth_select_spv.h"
#include "gpu_blit_shaders/gpu_eqaa_float_expand_spv.h"
#include "gpu_blit_shaders/gpu_eqaa_float_select_spv.h"
#include "gpu_blit_shaders/gpu_eqaa_stencil_expand_spv.h"
#include "gpu_blit_shaders/gpu_eqaa_stencil_select_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vulkan/vulkan_format_traits.hpp>

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

	const vk::PushConstantRange push_range {vk::ShaderStageFlagBits::eFragment, 0,
	                                        2 * sizeof(uint32_t)};
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_eqaa_layout),
	    "create EQAA pipeline layout");
	m_eqaa_shaders = {CompileSPV(GPU_EQAA_COLOR_SELECT_SPV, m_graphics.device),
	                  CompileSPV(GPU_EQAA_COLOR_EXPAND_SPV, m_graphics.device),
	                  CompileSPV(GPU_EQAA_DEPTH_SELECT_SPV, m_graphics.device),
	                  CompileSPV(GPU_EQAA_DEPTH_EXPAND_SPV, m_graphics.device),
	                  CompileSPV(GPU_EQAA_STENCIL_SELECT_SPV, m_graphics.device),
	                  CompileSPV(GPU_EQAA_STENCIL_EXPAND_SPV, m_graphics.device),
	                  CompileSPV(GPU_EQAA_FLOAT_SELECT_SPV, m_graphics.device),
	                  CompileSPV(GPU_EQAA_FLOAT_EXPAND_SPV, m_graphics.device)};
}

BlitHelper::~BlitHelper() {
	for (const auto& pipeline: m_pipelines) {
		m_graphics.device.destroyPipeline(pipeline.handle, nullptr);
	}
	for (const auto& [key, pipeline]: m_eqaa_pipelines) {
		m_graphics.device.destroyPipeline(pipeline, nullptr);
	}
	for (const auto shader: m_eqaa_shaders) {
		if (shader != nullptr) {
			m_graphics.device.destroyShaderModule(shader, nullptr);
		}
	}
	if (m_eqaa_layout != nullptr) {
		m_graphics.device.destroyPipelineLayout(m_eqaa_layout, nullptr);
	}
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
	stages[1].module = m_fragment_shader;
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

vk::Format BlitHelper::EqaaColorFormat(vk::Format format) {
	if (DepthAspectTransferFormat(format) != vk::Format::eUndefined ||
	    vk::componentCount(format) == 0 ||
	    vk::blockExtent(format) != std::array<uint8_t, 3> {1, 1, 1}) {
		return vk::Format::eUndefined;
	}
	if (EqaaColorIsFloat(format)) {
		return format;
	}
	vk::Format view = vk::Format::eUndefined;
	switch (vk::blockSize(format)) {
		case 1: view = vk::Format::eR8Uint; break;
		case 2: view = vk::Format::eR16Uint; break;
		case 4: view = vk::Format::eR32Uint; break;
		case 8: view = vk::Format::eR32G32Uint; break;
		case 16: view = vk::Format::eR32G32B32A32Uint; break;
		default: break;
	}
	return view != vk::Format::eUndefined && ImageViewOps::FormatsCompatible(format, view)
	           ? view
	           : vk::Format::eUndefined;
}

vk::Pipeline BlitHelper::GetEqaaPipeline(EqaaPipelineKey key) {
	const auto cached =
	    std::ranges::find(m_eqaa_pipelines, key, &std::pair<EqaaPipelineKey, vk::Pipeline>::first);
	if (cached != m_eqaa_pipelines.end()) {
		return cached->second;
	}
	const auto samples = vulkan_sample_count(key.samples);
	EXIT_IF(samples == vk::SampleCountFlagBits {} || key.format == vk::Format::eUndefined);
	const bool color   = key.kind == EqaaKind::ColorSelect || key.kind == EqaaKind::ColorExpand ||
	                     key.kind == EqaaKind::FloatSelect || key.kind == EqaaKind::FloatExpand;
	const bool stencil = key.kind == EqaaKind::StencilSelect || key.kind == EqaaKind::StencilExpand;

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = m_vertex_shader;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = m_eqaa_shaders[static_cast<size_t>(key.kind)];
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
	vk::PipelineDepthStencilStateCreateInfo depth {};
	if (stencil) {
		// Each pass sets one bit of the cleared plane where the source has it.
		depth.stencilTestEnable = VK_TRUE;
		depth.front             = {vk::StencilOp::eKeep,
		                           vk::StencilOp::eReplace,
		                           vk::StencilOp::eKeep,
		                           vk::CompareOp::eAlways,
		                           0xffu,
		                           0u,
		                           0xffu};
		depth.back              = depth.front;
	} else if (!color) {
		depth.depthTestEnable  = VK_TRUE;
		depth.depthWriteEnable = VK_TRUE;
		depth.depthCompareOp   = vk::CompareOp::eAlways;
	}
	vk::PipelineColorBlendAttachmentState blend_attachment {};
	blend_attachment.colorWriteMask =
	    vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo color_blend {};
	if (color) {
		color_blend.attachmentCount = 1;
		color_blend.pAttachments    = &blend_attachment;
	}
	std::vector<vk::DynamicState> dynamic_states {vk::DynamicState::eViewport,
	                                              vk::DynamicState::eScissor};
	if (stencil) {
		dynamic_states.push_back(vk::DynamicState::eStencilWriteMask);
	}
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();

	vk::PipelineRenderingCreateInfo rendering {};
	if (color) {
		rendering.colorAttachmentCount    = 1;
		rendering.pColorAttachmentFormats = &key.format;
	} else {
		const auto aspects = ImageViewOps::DepthAspectMask(key.format);
		if (aspects & vk::ImageAspectFlagBits::eDepth) {
			rendering.depthAttachmentFormat = key.format;
		}
		if (aspects & vk::ImageAspectFlagBits::eStencil) {
			rendering.stencilAttachmentFormat = key.format;
		}
	}

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
	create.layout              = m_eqaa_layout;

	vk::Pipeline pipeline = nullptr;
	RequireVulkanSuccess(
	    m_graphics.device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipeline),
	    "create EQAA pipeline");
	m_eqaa_pipelines.emplace_back(key, pipeline);
	return pipeline;
}

void BlitHelper::RecordEqaaLayer(Image& source, uint32_t source_level, uint32_t source_layer,
                                 Image& target, uint32_t target_level, uint32_t target_layer,
                                 vk::ImageAspectFlags aspects, bool select,
                                 uint16_t anchor_select) {
	m_scheduler.EndRendering();
	auto                        command = m_scheduler.Current().Handle();
	const auto                  width   = std::max(target.backing.extent.width >> target_level, 1u);
	const auto                  height = std::max(target.backing.extent.height >> target_level, 1u);
	const ImageSubresourceRange source_range {source_level, 1, source_layer, 1};
	const ImageSubresourceRange target_range {target_level, 1, target_layer, 1};
	const auto                  target_samples = target.backing.samples;
	const bool              color = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eColor);
	std::array<uint32_t, 2> push {anchor_select, 0};

	const auto view_of = [](Image& image, vk::Format format, vk::ImageAspectFlags aspect,
	                        uint32_t level, uint32_t layer, vk::ImageUsageFlags usage) {
		ImageViewInfo info {};
		info.format     = format;
		info.type       = vk::ImageViewType::e2D;
		info.aspect     = aspect;
		info.base_level = level;
		info.base_layer = layer;
		info.usage      = usage;
		return image.FindView(info);
	};
	const auto bind = [&](EqaaKind kind, vk::Format format, vk::ImageView source_view,
	                      vk::ImageLayout source_layout) {
		vk::DescriptorImageInfo descriptor_image {};
		descriptor_image.imageView   = source_view;
		descriptor_image.imageLayout = source_layout;
		vk::WriteDescriptorSet descriptor_write {};
		descriptor_write.dstBinding      = 0;
		descriptor_write.descriptorCount = 1;
		descriptor_write.descriptorType  = vk::DescriptorType::eSampledImage;
		descriptor_write.pImageInfo      = &descriptor_image;
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_eqaa_layout, 0, 1,
		                             &descriptor_write);
		command.bindPipeline(vk::PipelineBindPoint::eGraphics,
		                     GetEqaaPipeline({kind, format, target_samples}));
	};
	const auto draw = [&] {
		command.pushConstants(m_eqaa_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push),
		                      push.data());
		command.draw(3, 1, 0, 0);
	};

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent = vk::Extent2D {width, height};
	rendering.layerCount        = 1;
	vk::RenderingAttachmentInfo color_attachment {};
	vk::RenderingAttachmentInfo depth_attachment {};
	vk::RenderingAttachmentInfo stencil_attachment {};
	if (color) {
		const auto format = EqaaColorFormat(target.backing.format);
		EXIT_IF(format == vk::Format::eUndefined || aspects != vk::ImageAspectFlagBits::eColor);
		const auto source_view =
		    view_of(source, format, vk::ImageAspectFlagBits::eColor, source_level, source_layer,
		            vk::ImageUsageFlagBits::eSampled);
		const auto target_view =
		    view_of(target, format, vk::ImageAspectFlagBits::eColor, target_level, target_layer,
		            vk::ImageUsageFlagBits::eColorAttachment);
		source.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
		               source_range, command);
		target.Transit(vk::ImageLayout::eColorAttachmentOptimal,
		               vk::AccessFlagBits2::eColorAttachmentWrite, target_range, command);
		color_attachment.imageView     = target_view;
		color_attachment.imageLayout   = vk::ImageLayout::eColorAttachmentOptimal;
		color_attachment.loadOp        = vk::AttachmentLoadOp::eDontCare;
		color_attachment.storeOp       = vk::AttachmentStoreOp::eStore;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &color_attachment;
		command.beginRendering(&rendering);
		const bool as_float = EqaaColorIsFloat(format);
		bind(as_float ? (select ? EqaaKind::FloatSelect : EqaaKind::FloatExpand)
		              : (select ? EqaaKind::ColorSelect : EqaaKind::ColorExpand),
		     format, source_view, vk::ImageLayout::eShaderReadOnlyOptimal);
	} else {
		const auto format    = target.backing.format;
		const auto available = ImageViewOps::DepthAspectMask(format);
		EXIT_IF(format != source.backing.format || !aspects || (aspects & ~available));
		constexpr auto read_layout   = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
		constexpr auto target_layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
		source.Transit(read_layout, vk::AccessFlagBits2::eShaderRead, source_range, command);
		target.Transit(target_layout,
		               vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                   vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		               target_range, command);
		const auto target_view = view_of(target, format, available, target_level, target_layer,
		                                 vk::ImageUsageFlagBits::eDepthStencilAttachment);
		if (available & vk::ImageAspectFlagBits::eDepth) {
			depth_attachment.imageView   = target_view;
			depth_attachment.imageLayout = target_layout;
			depth_attachment.loadOp      = (aspects & vk::ImageAspectFlagBits::eDepth)
			                                   ? vk::AttachmentLoadOp::eDontCare
			                                   : vk::AttachmentLoadOp::eLoad;
			depth_attachment.storeOp     = vk::AttachmentStoreOp::eStore;
			rendering.pDepthAttachment   = &depth_attachment;
		}
		if (available & vk::ImageAspectFlagBits::eStencil) {
			stencil_attachment.imageView   = target_view;
			stencil_attachment.imageLayout = target_layout;
			stencil_attachment.loadOp      = (aspects & vk::ImageAspectFlagBits::eStencil)
			                                     ? vk::AttachmentLoadOp::eClear
			                                     : vk::AttachmentLoadOp::eLoad;
			stencil_attachment.storeOp     = vk::AttachmentStoreOp::eStore;
			stencil_attachment.clearValue.depthStencil = vk::ClearDepthStencilValue {0.0f, 0u};
			rendering.pStencilAttachment               = &stencil_attachment;
		}
		command.beginRendering(&rendering);
		if (aspects & vk::ImageAspectFlagBits::eDepth) {
			bind(select ? EqaaKind::DepthSelect : EqaaKind::DepthExpand, format,
			     view_of(source, format, vk::ImageAspectFlagBits::eDepth, source_level,
			             source_layer, vk::ImageUsageFlagBits::eSampled),
			     read_layout);
		}
	}
	const vk::Viewport viewport {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
	                             0.0f, 1.0f};
	const vk::Rect2D   scissor {{0, 0}, {width, height}};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	if (aspects & (vk::ImageAspectFlagBits::eColor | vk::ImageAspectFlagBits::eDepth)) {
		draw();
	}
	if (aspects & vk::ImageAspectFlagBits::eStencil) {
		const auto format = target.backing.format;
		bind(select ? EqaaKind::StencilSelect : EqaaKind::StencilExpand, format,
		     view_of(source, format, vk::ImageAspectFlagBits::eStencil, source_level, source_layer,
		             vk::ImageUsageFlagBits::eSampled),
		     vk::ImageLayout::eDepthStencilReadOnlyOptimal);
		command.setViewport(0, 1, &viewport);
		command.setScissor(0, 1, &scissor);
		for (uint32_t bit = 0; bit < 8; bit++) {
			command.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, 1u << bit);
			push[1] = bit;
			draw();
		}
	}
	command.endRendering();
	m_scheduler.GetDynamicState().Invalidate();
}

void BlitHelper::EqaaSelect(Image& owner, EqaaShadow& shadow, vk::ImageAspectFlags aspects) {
	EXIT_IF(shadow.image == nullptr || owner.backing.samples != 1);
	for (uint32_t layer = 0; layer < shadow.layers; layer++) {
		RecordEqaaLayer(*shadow.image, 0, layer, owner, shadow.mip, shadow.base_layer + layer,
		                aspects, true, shadow.anchor_select);
	}
}

void BlitHelper::EqaaExpand(Image& owner, EqaaShadow& shadow, vk::ImageAspectFlags aspects) {
	EXIT_IF(shadow.image == nullptr || owner.backing.samples != 1);
	for (uint32_t layer = 0; layer < shadow.layers; layer++) {
		RecordEqaaLayer(owner, shadow.mip, shadow.base_layer + layer, *shadow.image, 0, layer,
		                aspects, false, 0);
	}
}

} // namespace Libs::Graphics
