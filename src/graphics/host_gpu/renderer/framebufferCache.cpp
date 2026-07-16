#include "graphics/host_gpu/renderer/framebufferCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/utils.h"

#include <algorithm>
#include <atomic>
#include <vulkan/vk_enum_string_helper.h>

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

	bool     with_depth = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	bool     with_color[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t color_count                              = 0;
	VulkanImage* first_color                          = nullptr;
	VkExtent2D   first_color_extent                   = {};
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
	VkImageLayout color_layout[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (auto& layout: color_layout) {
		layout = RENDER_COLOR_IMAGE_LAYOUT;
	}
	auto depth_layout = (with_depth ? depth_attachment_layout(depth)
	                                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	auto depth_read_only =
	    (with_depth && depth_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

	if (with_depth && first_color != nullptr &&
	    (first_color_extent.width != depth->vulkan_buffer->extent.width ||
	     first_color_extent.height != depth->vulkan_buffer->extent.height)) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Framebuffer: temporary: dropping mismatched PS5 depth attachment color=%ux%u "
			     "depth=%ux%u format=%s\n",
			     first_color_extent.width, first_color_extent.height,
			     depth->vulkan_buffer->extent.width, depth->vulkan_buffer->extent.height,
			     string_VkFormat(depth->format));
		}
		depth->format                   = VK_FORMAT_UNDEFINED;
		depth->vulkan_buffer            = nullptr;
		depth->depth_test_enable        = false;
		depth->depth_write_enable       = false;
		depth->depth_bounds_test_enable = false;
		depth->stencil_test_enable      = false;
		depth->depth_clear_enable       = false;
		depth->depth_load_clear_enable  = false;
		depth->stencil_clear_enable     = false;
		with_depth                      = false;
		depth_layout                    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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
		    f.depth_clear_enable == depth->depth_load_clear_enable &&
		    f.stencil_clear_enable == depth->stencil_clear_enable &&
		    f.depth_read_only == depth_read_only) {
			return f.framebuffer;
		}
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

	if (!with_depth && color_count == 0) {
		LOGF("Framebuffer: warning: no color or depth attachment\n");
		return nullptr;
	}

	EXIT_NOT_IMPLEMENTED(!with_depth && color_count == 0);
	EXIT_NOT_IMPLEMENTED(with_depth && first_color != nullptr &&
	                     (first_color_extent.width != depth->vulkan_buffer->extent.width ||
	                      first_color_extent.height != depth->vulkan_buffer->extent.height));

	if (first_color == nullptr) {
		first_color = CreateDummyBuffer(VK_FORMAT_B8G8R8A8_SRGB, depth->vulkan_buffer->extent.width,
		                                depth->vulkan_buffer->extent.height);
		first_color_extent = first_color->extent;
	}

	VkAttachmentDescription attachments[RENDER_COLOR_ATTACHMENTS_MAX + 1] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		attachments[i].flags          = 0;
		attachments[i].format         = colors[i].format;
		attachments[i].samples        = VK_SAMPLE_COUNT_1_BIT;
		attachments[i].loadOp         = (colors[i].color_clear_enable ? VK_ATTACHMENT_LOAD_OP_CLEAR
		                                                              : VK_ATTACHMENT_LOAD_OP_LOAD);
		attachments[i].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[i].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[i].initialLayout  = color_layout[i];
		attachments[i].finalLayout    = RENDER_COLOR_IMAGE_LAYOUT;
	}

	const uint32_t depth_attachment       = color_count;
	attachments[depth_attachment].flags   = 0;
	attachments[depth_attachment].format  = depth->format;
	attachments[depth_attachment].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[depth_attachment].loadOp =
	    (depth->depth_load_clear_enable ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
	attachments[depth_attachment].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[depth_attachment].stencilLoadOp =
	    (depth->stencil_clear_enable ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
	attachments[depth_attachment].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[depth_attachment].initialLayout  = depth_layout;
	attachments[depth_attachment].finalLayout    = depth_layout;

	VkAttachmentReference color_attachment_ref[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		color_attachment_ref[i].attachment = (i < color_count ? i : VK_ATTACHMENT_UNUSED);
		color_attachment_ref[i].layout     = RENDER_COLOR_IMAGE_LAYOUT;
	}

	VkAttachmentReference depth_attachment_ref {};
	depth_attachment_ref.attachment = depth_attachment;
	depth_attachment_ref.layout     = depth_layout;

	VkSubpassDescription subpass {};
	subpass.flags                   = 0;
	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.inputAttachmentCount    = 0;
	subpass.pInputAttachments       = nullptr;
	subpass.colorAttachmentCount    = color_count;
	subpass.pColorAttachments       = (color_count > 0 ? color_attachment_ref : nullptr);
	subpass.pResolveAttachments     = nullptr;
	subpass.pDepthStencilAttachment = (with_depth ? &depth_attachment_ref : nullptr);
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments    = nullptr;

	const auto attachment_stage_mask = static_cast<VkPipelineStageFlags>(
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
	    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
	const auto attachment_access_mask = static_cast<VkAccessFlags>(
	    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

	VkSubpassDependency dependencies[2] = {};
	dependencies[0].srcSubpass          = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass          = 0;
	dependencies[0].srcStageMask        = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	dependencies[0].dstStageMask        = attachment_stage_mask;
	dependencies[0].srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	dependencies[0].dstAccessMask       = attachment_access_mask;
	dependencies[0].dependencyFlags     = VK_DEPENDENCY_BY_REGION_BIT;

	dependencies[1].srcSubpass      = 0;
	dependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask    = attachment_stage_mask;
	dependencies[1].dstStageMask    = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	dependencies[1].srcAccessMask   = attachment_access_mask;
	dependencies[1].dstAccessMask   = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkRenderPassCreateInfo render_pass_info {};
	render_pass_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_info.pNext           = nullptr;
	render_pass_info.flags           = 0;
	render_pass_info.attachmentCount = color_count + (with_depth ? 1u : 0u);
	render_pass_info.pAttachments    = attachments;
	render_pass_info.subpassCount    = 1;
	render_pass_info.pSubpasses      = &subpass;
	render_pass_info.dependencyCount = 2;
	render_pass_info.pDependencies   = dependencies;

	auto result =
	    vkCreateRenderPass(gctx->device, &render_pass_info, nullptr, &framebuffer->render_pass);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	VkFormat color_formats[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		color_formats[i] = colors[i].format;
	}
	framebuffer->render_pass_id =
	    render_pass_compat_id(color_count, color_formats, with_depth, depth->format, depth_layout);

	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	VkImageView views[RENDER_COLOR_ATTACHMENTS_MAX + 1] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		if (colors[i].vulkan_view == nullptr) {
			EXIT("Framebuffer: color attachment view is missing at slot %u\n", i);
		}
		views[i] = colors[i].vulkan_view;
	}
	if (with_depth) {
		views[color_count] = depth->vulkan_buffer->image_view[VulkanImage::VIEW_DEFAULT];
	}

	VkFramebufferCreateInfo framebuffer_info {};
	framebuffer_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.pNext           = nullptr;
	framebuffer_info.flags           = 0;
	framebuffer_info.renderPass      = framebuffer->render_pass;
	framebuffer_info.attachmentCount = color_count + (with_depth ? 1u : 0u);
	framebuffer_info.pAttachments    = views;
	framebuffer_info.width           = first_color_extent.width;
	framebuffer_info.height          = first_color_extent.height;
	framebuffer_info.layers          = 1;

	result =
	    vkCreateFramebuffer(gctx->device, &framebuffer_info, nullptr, &framebuffer->framebuffer);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

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

			vkDestroyFramebuffer(gctx->device, f.framebuffer->framebuffer, nullptr);

			vkDestroyRenderPass(gctx->device, f.framebuffer->render_pass, nullptr);

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

			vkDestroyFramebuffer(gctx->device, f.framebuffer->framebuffer, nullptr);

			vkDestroyRenderPass(gctx->device, f.framebuffer->render_pass, nullptr);

			delete f.framebuffer;

			f.framebuffer = nullptr;
		}
	}
}

VideoOutVulkanImage* FramebufferCache::CreateDummyBuffer(VkFormat format, uint32_t width,
                                                         uint32_t height) {
	for (auto* b: m_dummy_buffers) {
		if (b->extent.width == width && b->extent.height == height && b->format == format) {
			return b;
		}
	}

	auto* ctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(ctx == nullptr);

	auto* vk_obj = new VideoOutVulkanImage;

	vk_obj->extent.width  = width;
	vk_obj->extent.height = height;
	vk_obj->format        = format;
	vk_obj->image         = nullptr;
	vk_obj->layout        = VK_IMAGE_LAYOUT_UNDEFINED;

	UtilResetImageViews(vk_obj);

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent.width  = vk_obj->extent.width;
	image_info.extent.height = vk_obj->extent.height;
	image_info.extent.depth  = 1;
	image_info.mipLevels     = 1;
	image_info.arrayLayers   = 1;
	image_info.format        = vk_obj->format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = vk_obj->layout;
	image_info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;

	VulkanMemory mem;

	mem.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool created = VulkanCreateImage(ctx, &image_info, vk_obj, &mem);
	EXIT_NOT_IMPLEMENTED(!created);

	vk_obj->memory = mem;

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = nullptr;
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = vk_obj->format;
	create_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = 0;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = 1;

	vkCreateImageView(ctx->device, &create_info, nullptr,
	                  &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]);

	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);

	UtilSetImageLayoutOptimal(vk_obj);

	m_dummy_buffers.push_back(vk_obj);

	return vk_obj;
}

} // namespace Libs::Graphics
