#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_hints.h"
#include "SDL_joystick.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_pixels.h"
#include "SDL_rwops.h"
#include "SDL_stdinc.h"
#include "SDL_surface.h"
#include "SDL_thread.h"
#include "SDL_touch.h"
#include "SDL_video.h"
#include "SDL_vulkan.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/windowInternal.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Libs::Graphics {

VulkanSwapchain::~VulkanSwapchain() = default;

[[maybe_unused]] static VkSwapchainKHR VulkanCreateSwapchainInternal(
    VkDevice device, VkSurfaceKHR surface, uint32_t width, uint32_t height, uint32_t image_count,
    SurfaceCapabilities* r, VkFormat* swapchain_format, VkExtent2D* swapchain_extent,
    std::unique_ptr<VkImage[]>*     swapchain_images,
    std::unique_ptr<VkImageView[]>* swapchain_image_views, uint32_t* swapchain_images_count) {
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(r == nullptr);
	EXIT_IF(swapchain_format == nullptr);
	EXIT_IF(swapchain_extent == nullptr);
	EXIT_IF(swapchain_images == nullptr);
	EXIT_IF(swapchain_image_views == nullptr);
	EXIT_IF(swapchain_images_count == nullptr);

	EXIT_NOT_IMPLEMENTED(r->formats.empty());

	VkExtent2D extent {};
	extent.width  = std::clamp(width, r->capabilities.minImageExtent.width,
	                           r->capabilities.maxImageExtent.width);
	extent.height = std::clamp(height, r->capabilities.minImageExtent.height,
	                           r->capabilities.maxImageExtent.height);

	image_count =
	    std::clamp(image_count, r->capabilities.minImageCount, r->capabilities.maxImageCount);

	VkSwapchainCreateInfoKHR create_info {};
	create_info.sType         = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create_info.pNext         = nullptr;
	create_info.flags         = 0;
	create_info.surface       = surface;
	create_info.minImageCount = image_count;

	if (r->format_unorm_bgra32) {
		create_info.imageFormat     = VK_FORMAT_B8G8R8A8_UNORM;
		create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	} else if (r->format_srgb_bgra32) {
		create_info.imageFormat     = VK_FORMAT_B8G8R8A8_SRGB;
		create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	} else {
		const auto& format          = r->formats[0];
		create_info.imageFormat     = format.format;
		create_info.imageColorSpace = format.colorSpace;
	}

	create_info.imageExtent      = extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create_info.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
	create_info.queueFamilyIndexCount = 0;
	create_info.pQueueFamilyIndices   = nullptr;
	create_info.preTransform          = r->capabilities.currentTransform;
	create_info.compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create_info.presentMode           = VK_PRESENT_MODE_FIFO_KHR;
	create_info.clipped               = VK_TRUE;
	create_info.oldSwapchain          = nullptr;

	*swapchain_format = create_info.imageFormat;
	*swapchain_extent = extent;

	VkSwapchainKHR swapchain = nullptr;

	vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain);

	vkGetSwapchainImagesKHR(device, swapchain, swapchain_images_count, nullptr);
	EXIT_NOT_IMPLEMENTED(*swapchain_images_count == 0);

	*swapchain_images = std::make_unique<VkImage[]>(*swapchain_images_count);
	vkGetSwapchainImagesKHR(device, swapchain, swapchain_images_count, swapchain_images->get());

	*swapchain_image_views = std::make_unique<VkImageView[]>(*swapchain_images_count);
	for (uint32_t i = 0; i < *swapchain_images_count; i++) {
		VkImageViewCreateInfo create_info {};
		create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		create_info.pNext                           = nullptr;
		create_info.flags                           = 0;
		create_info.image                           = (*swapchain_images)[i];
		create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
		create_info.format                          = *swapchain_format;
		create_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		create_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		create_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		create_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		create_info.subresourceRange.baseArrayLayer = 0;
		create_info.subresourceRange.baseMipLevel   = 0;
		create_info.subresourceRange.layerCount     = 1;
		create_info.subresourceRange.levelCount     = 1;

		vkCreateImageView(device, &create_info, nullptr, &((*swapchain_image_views)[i]));
	}

	return swapchain;
}

VulkanSwapchain* VulkanCreateSwapchain(GraphicContext* ctx, uint32_t image_count) {
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->screen_width == 0);
	EXIT_IF(ctx->screen_height == 0);

	Common::LockGuard lock(g_window_ctx->mutex);

	auto  swapchain_owner = std::make_unique<VulkanSwapchain>();
	auto* s               = swapchain_owner.get();

	s->swapchain = VulkanCreateSwapchainInternal(
	    ctx->device, g_window_ctx->surface, ctx->screen_width, ctx->screen_height, image_count,
	    g_window_ctx->surface_capabilities, &s->swapchain_format, &s->swapchain_extent,
	    &s->swapchain_images, &s->swapchain_image_views, &s->swapchain_images_count);
	if (s->swapchain == nullptr) {
		EXIT("Could not create swapchain");
	}

	s->current_index = static_cast<uint32_t>(-1);
	s->present_frame = 0;

	VkSemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphore_info.pNext = nullptr;
	semaphore_info.flags = 0;

	s->image_acquired_semaphores  = std::make_unique<VkSemaphore[]>(s->swapchain_images_count);
	s->render_complete_semaphores = std::make_unique<VkSemaphore[]>(s->swapchain_images_count);
	for (uint32_t i = 0; i < s->swapchain_images_count; i++) {
		auto result = vkCreateSemaphore(ctx->device, &semaphore_info, nullptr,
		                                &s->image_acquired_semaphores[i]);
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

		result = vkCreateSemaphore(ctx->device, &semaphore_info, nullptr,
		                           &s->render_complete_semaphores[i]);
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	}

	return swapchain_owner.release();
}

static void VulkanDeleteSwapchain(GraphicContext* ctx, VulkanSwapchain* s) {
	if (ctx == nullptr || s == nullptr) {
		return;
	}
	auto swapchain_owner = std::unique_ptr<VulkanSwapchain>(s);

	VulkanDeviceWaitIdle(ctx);

	s->present_command_buffers.clear();

	if (s->image_acquired_semaphores != nullptr) {
		for (uint32_t i = 0; i < s->swapchain_images_count; i++) {
			if (s->image_acquired_semaphores[i] != nullptr) {
				vkDestroySemaphore(ctx->device, s->image_acquired_semaphores[i], nullptr);
			}
		}
	}
	if (s->render_complete_semaphores != nullptr) {
		for (uint32_t i = 0; i < s->swapchain_images_count; i++) {
			if (s->render_complete_semaphores[i] != nullptr) {
				vkDestroySemaphore(ctx->device, s->render_complete_semaphores[i], nullptr);
			}
		}
	}
	if (s->swapchain_image_views != nullptr) {
		for (uint32_t i = 0; i < s->swapchain_images_count; i++) {
			if (s->swapchain_image_views[i] != nullptr) {
				vkDestroyImageView(ctx->device, s->swapchain_image_views[i], nullptr);
			}
		}
	}

	if (s->swapchain != nullptr) {
		vkDestroySwapchainKHR(ctx->device, s->swapchain, nullptr);
	}
}

static void VulkanRefreshSurfaceSize() {
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->window == nullptr);
	EXIT_IF(g_window_ctx->surface_capabilities == nullptr);

	int width  = 0;
	int height = 0;
	SDL_Vulkan_GetDrawableSize(g_window_ctx->window, &width, &height);
	if (width > 0 && height > 0) {
		g_window_ctx->graphic_ctx.screen_width  = static_cast<uint32_t>(width);
		g_window_ctx->graphic_ctx.screen_height = static_cast<uint32_t>(height);
	}

	VulkanGetSurfaceCapabilities(g_window_ctx->graphic_ctx.physical_device, g_window_ctx->surface,
	                             g_window_ctx->surface_capabilities);
}

static void VulkanRecreateSwapchain() {
	EXIT_IF(g_window_ctx == nullptr);

	LOGF("Recreating Vulkan swapchain\n");
	VulkanRefreshSurfaceSize();
	VulkanDeleteSwapchain(&g_window_ctx->graphic_ctx, g_window_ctx->swapchain);
	g_window_ctx->swapchain = VulkanCreateSwapchain(&g_window_ctx->graphic_ctx, 2);
}

static CommandBuffer* WindowGetPresentCommandBuffer(VulkanSwapchain* swapchain,
                                                    uint32_t         present_frame) {
	EXIT_IF(swapchain == nullptr);
	EXIT_IF(swapchain->swapchain_images_count == 0);
	EXIT_IF(present_frame >= swapchain->swapchain_images_count);

	if (swapchain->present_command_buffers.empty()) {
		swapchain->present_command_buffers.resize(swapchain->swapchain_images_count);
	}
	EXIT_IF(swapchain->present_command_buffers.size() != swapchain->swapchain_images_count);

	auto& buffer = swapchain->present_command_buffers[present_frame];
	if (buffer == nullptr) {
		buffer = std::make_unique<CommandBuffer>(GraphicContext::QUEUE_GFX);
	}

	return buffer.get();
}

void WindowDrawBuffer(VideoOutVulkanImage* image) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(image == nullptr);
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->swapchain == nullptr);

	if (g_window_ctx->window_hidden) {
		WindowUpdateIcon();

		SDL_ShowWindow(g_window_ctx->window);

		g_window_ctx->window_hidden = false;
		VulkanRecreateSwapchain();
	}

	auto* swapchain = g_window_ctx->swapchain;

	const auto present_frame = swapchain->present_frame;
	EXIT_IF(present_frame >= swapchain->swapchain_images_count);

	auto& buffer = *WindowGetPresentCommandBuffer(swapchain, present_frame);
	buffer.WaitForFenceAndReset();

	swapchain->current_index = static_cast<uint32_t>(-1);

	auto result = vkAcquireNextImageKHR(
	    g_window_ctx->graphic_ctx.device, swapchain->swapchain, UINT64_MAX,
	    swapchain->image_acquired_semaphores[present_frame], nullptr, &swapchain->current_index);

	switch (result) {
		case VK_SUCCESS: break;
		case VK_SUBOPTIMAL_KHR: LOGF("vkAcquireNextImageKHR returned VK_SUBOPTIMAL_KHR\n"); break;
		case VK_ERROR_OUT_OF_DATE_KHR:
			LOGF("vkAcquireNextImageKHR returned VK_ERROR_OUT_OF_DATE_KHR\n");
			VulkanRecreateSwapchain();
			return;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", string_VkResult(result));
	}
	EXIT_NOT_IMPLEMENTED(swapchain->current_index == static_cast<uint32_t>(-1));

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	auto* vk_buffer = buffer.GetPool()->buffers[buffer.GetIndex()];

	buffer.Begin();

	UtilBlitImage(&buffer, image, swapchain);

	VkImageMemoryBarrier pre_present_barrier {};
	pre_present_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	pre_present_barrier.pNext                           = nullptr;
	pre_present_barrier.srcAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
	pre_present_barrier.dstAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
	pre_present_barrier.oldLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	pre_present_barrier.newLayout                       = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	pre_present_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	pre_present_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	pre_present_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	pre_present_barrier.subresourceRange.baseMipLevel   = 0;
	pre_present_barrier.subresourceRange.levelCount     = 1;
	pre_present_barrier.subresourceRange.baseArrayLayer = 0;
	pre_present_barrier.subresourceRange.layerCount     = 1;
	pre_present_barrier.image = swapchain->swapchain_images[swapchain->current_index];
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &pre_present_barrier);

	buffer.End();

	auto render_complete_semaphore =
	    swapchain->render_complete_semaphores[swapchain->current_index];
	const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	buffer.ExecuteWithSemaphore(swapchain->image_acquired_semaphores[present_frame], wait_stage,
	                            render_complete_semaphore);

	VkPresentInfoKHR present;
	present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.pNext              = nullptr;
	present.swapchainCount     = 1;
	present.pSwapchains        = &swapchain->swapchain;
	present.pImageIndices      = &swapchain->current_index;
	present.pWaitSemaphores    = &render_complete_semaphore;
	present.waitSemaphoreCount = 1;
	present.pResults           = nullptr;

	const auto& queue = g_window_ctx->graphic_ctx.queues[GraphicContext::QUEUE_PRESENT];

	if (queue.mutex != nullptr) {
		queue.mutex->Lock();
	}
	result = vkQueuePresentKHR(queue.vk_queue, &present);
	if (queue.mutex != nullptr) {
		queue.mutex->Unlock();
	}
	switch (result) {
		case VK_SUCCESS: break;
		case VK_SUBOPTIMAL_KHR: LOGF("vkQueuePresentKHR returned VK_SUBOPTIMAL_KHR\n"); break;
		case VK_ERROR_OUT_OF_DATE_KHR:
			LOGF("vkQueuePresentKHR returned VK_ERROR_OUT_OF_DATE_KHR\n");
			VulkanRecreateSwapchain();
			return;
		default: EXIT("vkQueuePresentKHR failed: %s\n", string_VkResult(result));
	}

	swapchain->present_frame = (present_frame + 1u) % swapchain->swapchain_images_count;

	RenderDocOnPresent();
	WindowUpdateTitle();
}

} // namespace Libs::Graphics
