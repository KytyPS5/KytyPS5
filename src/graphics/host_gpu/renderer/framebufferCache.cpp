#include "graphics/host_gpu/renderer/framebufferCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <atomic>

namespace Libs::Graphics {

VulkanFramebuffer* FramebufferCache::CreateFramebuffer(RenderColorInfo* colors,
                                                       uint32_t         requested_color_count,
                                                       RenderDepthInfo* depth) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);

	EXIT_IF(colors == nullptr);
	EXIT_IF(depth == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(requested_color_count > RENDER_COLOR_ATTACHMENTS_MAX);

	bool with_depth = (depth->format != vk::Format::eUndefined && depth->vulkan_buffer != nullptr);
	bool with_color[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t     color_count                      = 0;
	VulkanImage* first_color                      = nullptr;
	vk::Extent2D first_color_extent               = {};
	for (uint32_t i = 0; i < requested_color_count; i++) {
		with_color[i] = (colors[i].vulkan_buffer != nullptr);
		if (!with_color[i]) {
			break;
		}
		if (first_color == nullptr) {
			first_color        = colors[i].vulkan_buffer;
			first_color_extent = colors[i].extent;
		} else if (colors[i].extent.width != first_color_extent.width ||
		           colors[i].extent.height != first_color_extent.height) {
			LOGF("Framebuffer: temporary: dropping mismatched MRT%u attachment color0=%ux%u "
			     "color%u=%ux%u\n",
			     i, first_color_extent.width, first_color_extent.height, i, colors[i].extent.width,
			     colors[i].extent.height);
			with_color[i] = false;
			break;
		}
		color_count++;
	}
	vk::ImageLayout color_layout[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (auto& layout: color_layout) {
		layout = RENDER_COLOR_IMAGE_LAYOUT;
	}
	auto depth_layout = (with_depth ? depth_attachment_layout(depth)
	                                : vk::ImageLayout::eDepthStencilAttachmentOptimal);
	auto depth_read_only =
	    (with_depth && depth_layout == vk::ImageLayout::eDepthStencilReadOnlyOptimal);

	if (with_depth && first_color != nullptr &&
	    (first_color_extent.width != depth->vulkan_buffer->extent.width ||
	     first_color_extent.height != depth->vulkan_buffer->extent.height)) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Framebuffer: temporary: dropping mismatched PS5 depth attachment color=%ux%u "
			     "depth=%ux%u format=%s\n",
			     first_color_extent.width, first_color_extent.height,
			     depth->vulkan_buffer->extent.width, depth->vulkan_buffer->extent.height,
			     VulkanToString(depth->format).c_str());
		}
		depth->format                   = vk::Format::eUndefined;
		depth->vulkan_buffer            = nullptr;
		depth->vulkan_view              = nullptr;
		depth->depth_test_enable        = false;
		depth->depth_write_enable       = false;
		depth->depth_bounds_test_enable = false;
		depth->stencil_test_enable      = false;
		depth->depth_clear_enable       = false;
		depth->depth_load_clear_enable  = false;
		depth->stencil_clear_enable     = false;
		with_depth                      = false;
		depth_layout                    = vk::ImageLayout::eDepthStencilAttachmentOptimal;
		depth_read_only                 = false;
	}

	for (auto& f: m_framebuffers) {
		bool color_match = (f.framebuffer != nullptr);
		for (uint32_t i = 0; color_match && i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
			const uint64_t image_id =
			    (i < color_count && with_color[i] ? colors[i].vulkan_buffer->memory.unique_id : 0);
			color_match =
			    color_match && f.image_id[i] == image_id &&
			    f.color_view[i] ==
			        (i < color_count && with_color[i] ? colors[i].vulkan_view : nullptr) &&
			    f.color_clear_enable[i] ==
			        (i < color_count && with_color[i] && colors[i].color_clear_enable) &&
			    f.color_layout[i] == color_layout[i];
		}
		if (color_match &&
		    f.depth_id == (with_depth ? depth->vulkan_buffer->memory.unique_id : 0) &&
		    f.depth_view == (with_depth ? depth->vulkan_view : nullptr) &&
		    f.depth_clear_enable == depth->depth_load_clear_enable &&
		    f.stencil_clear_enable == depth->stencil_clear_enable &&
		    f.depth_read_only == depth_read_only) {
			return f.framebuffer;
		}
	}

	if (!with_depth && color_count == 0) {
		LOGF("Framebuffer: warning: no color or depth attachment\n");
		return nullptr;
	}

	EXIT_NOT_IMPLEMENTED(with_depth && first_color != nullptr &&
	                     (first_color_extent.width != depth->vulkan_buffer->extent.width ||
	                      first_color_extent.height != depth->vulkan_buffer->extent.height));

	if (first_color == nullptr) {
		first_color_extent = depth->vulkan_buffer->extent;
	}

	auto* framebuffer        = new VulkanFramebuffer;
	framebuffer->render_pass = nullptr;
	framebuffer->framebuffer = nullptr;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		framebuffer->color_layout[i] = color_layout[i];
	}
	framebuffer->depth_layout = depth_layout;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	vk::AttachmentDescription attachments[RENDER_COLOR_ATTACHMENTS_MAX + 1] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		attachments[i].flags         = {};
		attachments[i].format        = colors[i].format;
		attachments[i].samples       = vk::SampleCountFlagBits::e1;
		attachments[i].loadOp        = (colors[i].color_clear_enable ? vk::AttachmentLoadOp::eClear
		                                                             : vk::AttachmentLoadOp::eLoad);
		attachments[i].storeOp       = vk::AttachmentStoreOp::eStore;
		attachments[i].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
		attachments[i].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
		attachments[i].initialLayout  = color_layout[i];
		attachments[i].finalLayout    = RENDER_COLOR_IMAGE_LAYOUT;
	}

	const uint32_t depth_attachment       = color_count;
	attachments[depth_attachment].flags   = {};
	attachments[depth_attachment].format  = depth->format;
	attachments[depth_attachment].samples = vk::SampleCountFlagBits::e1;
	attachments[depth_attachment].loadOp =
	    (depth->depth_load_clear_enable ? vk::AttachmentLoadOp::eClear
	                                    : vk::AttachmentLoadOp::eLoad);
	attachments[depth_attachment].storeOp = vk::AttachmentStoreOp::eStore;
	attachments[depth_attachment].stencilLoadOp =
	    (depth->stencil_clear_enable ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad);
	attachments[depth_attachment].stencilStoreOp = vk::AttachmentStoreOp::eStore;
	attachments[depth_attachment].initialLayout  = depth_layout;
	attachments[depth_attachment].finalLayout    = depth_layout;

	vk::AttachmentReference color_attachment_ref[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		color_attachment_ref[i].attachment = (i < color_count ? i : VK_ATTACHMENT_UNUSED);
		color_attachment_ref[i].layout     = RENDER_COLOR_IMAGE_LAYOUT;
	}

	vk::AttachmentReference depth_attachment_ref {};
	depth_attachment_ref.attachment = depth_attachment;
	depth_attachment_ref.layout     = depth_layout;

	vk::SubpassDescription subpass {};
	subpass.flags                   = {};
	subpass.pipelineBindPoint       = vk::PipelineBindPoint::eGraphics;
	subpass.inputAttachmentCount    = 0;
	subpass.pInputAttachments       = nullptr;
	subpass.colorAttachmentCount    = color_count;
	subpass.pColorAttachments       = (color_count > 0 ? color_attachment_ref : nullptr);
	subpass.pResolveAttachments     = nullptr;
	subpass.pDepthStencilAttachment = (with_depth ? &depth_attachment_ref : nullptr);
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments    = nullptr;

	const auto attachment_stage_mask =
	    static_cast<vk::PipelineStageFlags>(vk::PipelineStageFlagBits::eColorAttachmentOutput |
	                                        vk::PipelineStageFlagBits::eEarlyFragmentTests |
	                                        vk::PipelineStageFlagBits::eLateFragmentTests);
	const auto attachment_access_mask = static_cast<vk::AccessFlags>(
	    vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite |
	    vk::AccessFlagBits::eDepthStencilAttachmentRead |
	    vk::AccessFlagBits::eDepthStencilAttachmentWrite);

	vk::SubpassDependency dependencies[2] = {};
	dependencies[0].srcSubpass            = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass            = 0;
	dependencies[0].srcStageMask          = vk::PipelineStageFlagBits::eAllCommands;
	dependencies[0].dstStageMask          = attachment_stage_mask;
	dependencies[0].srcAccessMask =
	    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	dependencies[0].dstAccessMask   = attachment_access_mask;
	dependencies[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;

	dependencies[1].srcSubpass    = 0;
	dependencies[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask  = attachment_stage_mask;
	dependencies[1].dstStageMask  = vk::PipelineStageFlagBits::eAllCommands;
	dependencies[1].srcAccessMask = attachment_access_mask;
	dependencies[1].dstAccessMask =
	    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	dependencies[1].dependencyFlags = vk::DependencyFlagBits::eByRegion;

	vk::RenderPassCreateInfo render_pass_info {};
	render_pass_info.sType           = vk::StructureType::eRenderPassCreateInfo;
	render_pass_info.pNext           = nullptr;
	render_pass_info.flags           = {};
	render_pass_info.attachmentCount = color_count + (with_depth ? 1u : 0u);
	render_pass_info.pAttachments    = attachments;
	render_pass_info.subpassCount    = 1;
	render_pass_info.pSubpasses      = &subpass;
	render_pass_info.dependencyCount = 2;
	render_pass_info.pDependencies   = dependencies;

	auto result =
	    gctx->device.createRenderPass(&render_pass_info, nullptr, &framebuffer->render_pass);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	vk::Format color_formats[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		color_formats[i] = colors[i].format;
	}
	framebuffer->render_pass_id =
	    render_pass_compat_id(color_count, color_formats, with_depth, depth->format, depth_layout);

	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	vk::ImageView views[RENDER_COLOR_ATTACHMENTS_MAX + 1] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		if (colors[i].vulkan_view == nullptr) {
			EXIT("Framebuffer: color attachment view is missing at slot %u\n", i);
		}
		views[i] = colors[i].vulkan_view;
	}
	if (with_depth) {
		if (depth->vulkan_view == nullptr) {
			EXIT("Framebuffer: depth attachment view is missing\n");
		}
		views[color_count] = depth->vulkan_view;
	}

	vk::FramebufferCreateInfo framebuffer_info {};
	framebuffer_info.sType           = vk::StructureType::eFramebufferCreateInfo;
	framebuffer_info.pNext           = nullptr;
	framebuffer_info.flags           = {};
	framebuffer_info.renderPass      = framebuffer->render_pass;
	framebuffer_info.attachmentCount = color_count + (with_depth ? 1u : 0u);
	framebuffer_info.pAttachments    = views;
	framebuffer_info.width           = first_color_extent.width;
	framebuffer_info.height          = first_color_extent.height;
	framebuffer_info.layers          = 1;

	result = gctx->device.createFramebuffer(&framebuffer_info, nullptr, &framebuffer->framebuffer);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	EXIT_NOT_IMPLEMENTED(framebuffer->framebuffer == nullptr);

	Framebuffer fnew;
	fnew.framebuffer = framebuffer;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		fnew.image_id[i] =
		    (i < color_count && with_color[i] ? colors[i].vulkan_buffer->memory.unique_id : 0);
		fnew.color_view[i] = (i < color_count && with_color[i] ? colors[i].vulkan_view : nullptr);
		fnew.color_clear_enable[i] =
		    (i < color_count && with_color[i] && colors[i].color_clear_enable);
		fnew.color_layout[i] = color_layout[i];
	}
	fnew.depth_id             = (with_depth ? depth->vulkan_buffer->memory.unique_id : 0);
	fnew.depth_view           = (with_depth ? depth->vulkan_view : nullptr);
	fnew.depth_clear_enable   = depth->depth_load_clear_enable;
	fnew.stencil_clear_enable = depth->stencil_clear_enable;
	fnew.depth_read_only      = depth_read_only;

	bool updated = false;

	for (auto& f: m_framebuffers) {
		if (f.framebuffer == nullptr) {
			f       = fnew;
			updated = true;
			break;
		}
	}

	if (!updated) {
		m_framebuffers.push_back(fnew);
	}

	return framebuffer;
}

void FramebufferCache::FreeFramebufferByColor(VulkanImage* image) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(image == nullptr);

	Common::LockGuard lock(m_mutex);

	for (auto& f: m_framebuffers) {
		bool uses_image = false;
		for (auto image_id: f.image_id) {
			if (image_id == image->memory.unique_id) {
				uses_image = true;
				break;
			}
		}
		if (f.framebuffer != nullptr && uses_image) {
			auto* gctx = g_render_ctx->GetGraphicCtx();

			EXIT_IF(gctx == nullptr);

			gctx->device.destroyFramebuffer(f.framebuffer->framebuffer, nullptr);

			gctx->device.destroyRenderPass(f.framebuffer->render_pass, nullptr);

			delete f.framebuffer;

			f.framebuffer = nullptr;
		}
	}
}

void FramebufferCache::FreeFramebufferByDepth(DepthStencilVulkanImage* image) {
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(image == nullptr);

	Common::LockGuard lock(m_mutex);

	for (auto& f: m_framebuffers) {
		if (f.framebuffer != nullptr && f.depth_id == image->memory.unique_id) {
			auto* gctx = g_render_ctx->GetGraphicCtx();

			EXIT_IF(gctx == nullptr);

			gctx->device.destroyFramebuffer(f.framebuffer->framebuffer, nullptr);

			gctx->device.destroyRenderPass(f.framebuffer->render_pass, nullptr);

			delete f.framebuffer;

			f.framebuffer = nullptr;
		}
	}
}

} // namespace Libs::Graphics
