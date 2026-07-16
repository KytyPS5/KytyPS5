#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderState.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/presentation/displayBuffer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <vulkan/vk_enum_string_helper.h>

namespace Libs::Graphics {

void GraphicsRenderMemoryBarrier(CommandBuffer* buffer) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Common::LockGuard lock(g_render_ctx->GetMutex());

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	VkMemoryBarrier mem_barrier {};
	mem_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mem_barrier.pNext         = nullptr;
	mem_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	mem_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mem_barrier, 0, nullptr, 0,
	                     nullptr);
}

void GraphicsRenderTextureBarrier(VkCommandBuffer vk_buffer, VulkanImage* image) {
	EXIT_IF(image == nullptr);

	VkImageMemoryBarrier image_memory_barrier {};
	image_memory_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	image_memory_barrier.pNext         = nullptr;
	image_memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	                                     VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
	                                     VK_ACCESS_MEMORY_READ_BIT;
	image_memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	image_memory_barrier.oldLayout     = image->layout;
	image_memory_barrier.newLayout     = RENDER_COLOR_IMAGE_LAYOUT;
	image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.image                           = image->image;
	image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	image_memory_barrier.subresourceRange.baseMipLevel   = 0;
	image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	image_memory_barrier.subresourceRange.baseArrayLayer = 0;
	image_memory_barrier.subresourceRange.layerCount     = image->layers;

	vkCmdPipelineBarrier(
	    vk_buffer,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
	        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
	        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	    0, 0, nullptr, 0, nullptr, 1, &image_memory_barrier);

	image->layout = image_memory_barrier.newLayout;
}

void GraphicsRenderColorImageBarrier(VkCommandBuffer vk_buffer, VulkanImage* image,
                                     VkImageLayout new_layout) {
	EXIT_IF(image == nullptr);

	if (image->layout != new_layout) {
		if (image->type == VulkanImageType::VideoOut &&
		    image->layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 128) {
				LOGF("GraphicsRenderColorImageBarrier: image=%p type=%d layout=%s -> %s\n",
				     reinterpret_cast<void*>(image->image), static_cast<int>(image->type),
				     string_VkImageLayout(image->layout), string_VkImageLayout(new_layout));
			}
		}
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext         = nullptr;
		image_memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.oldLayout     = image->layout;
		image_memory_barrier.newLayout     = new_layout;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = image->image;
		image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = image->layers;

		const auto stages = static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
		                                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
		                                                      VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdPipelineBarrier(vk_buffer, stages, stages, 0, 0, nullptr, 0, nullptr, 1,
		                     &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}
}

VkImageAspectFlags DepthStencilAspectMask(VkFormat format);

void GraphicsRenderDepthStencilImageBarrier(VkCommandBuffer vk_buffer, VulkanImage* image,
                                            VkImageLayout new_layout) {
	EXIT_IF(image == nullptr);
	EXIT_IF(image->type != VulkanImageType::DepthStencil);
	if (new_layout == VK_IMAGE_LAYOUT_UNDEFINED || new_layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
		EXIT("invalid destination depth/stencil image layout: %s\n",
		     string_VkImageLayout(new_layout));
	}
	if (image->layout == new_layout) {
		return;
	}
	VkImageMemoryBarrier barrier {};
	barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout           = image->layout;
	barrier.newLayout           = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image->image;
	barrier.subresourceRange = {DepthStencilAspectMask(image->format), 0, VK_REMAINING_MIP_LEVELS,
	                            0, image->layers};
	const auto stages = static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
	                                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
	                                                      VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdPipelineBarrier(vk_buffer, stages, stages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	image->layout = new_layout;
}

void GraphicsRenderDepthStencilBarrier(VkCommandBuffer vk_buffer, VulkanImage* image) {
	EXIT_IF(image == nullptr);

	EXIT_IF(image->type != VulkanImageType::DepthStencil);

	auto* depth = static_cast<DepthStencilVulkanImage*>(image);
	if (depth->compressed) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF(
			    "DepthTexture: decompressing depth target for shader read format=%s extent=%ux%u\n",
			    string_VkFormat(depth->format), depth->extent.width, depth->extent.height);
		}
		depth->compressed = false;
	}

	if (image->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext         = nullptr;
		image_memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		image_memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		image_memory_barrier.oldLayout     = image->layout;
		image_memory_barrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		image_memory_barrier.srcQueueFamilyIndex           = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex           = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                         = image->image;
		image_memory_barrier.subresourceRange.aspectMask   = DepthStencilAspectMask(image->format);
		image_memory_barrier.subresourceRange.baseMipLevel = 0;
		image_memory_barrier.subresourceRange.levelCount   = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = image->layers;

		vkCmdPipelineBarrier(
		    vk_buffer,
		    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
		        VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
		        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}
}

void GraphicsRenderTextureBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Common::LockGuard lock(g_render_ctx->GetMutex());

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	auto* native = g_render_ctx->GetTextureCache()->FindRenderTargetByRange(buffer, vaddr, size);
	if (native == nullptr) {
		EXIT("render-target barrier range has no cached image\n");
	}
	GraphicsRenderTextureBarrier(vk_buffer, native);
}

void GraphicsRenderDepthStencilBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Common::LockGuard lock(g_render_ctx->GetMutex());

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	auto* native    = g_render_ctx->GetTextureCache()->FindDepthTargetByRange(vaddr, size);
	if (native == nullptr) {
		EXIT("depth-target barrier range has no cached image\n");
	}
	GraphicsRenderDepthStencilBarrier(vk_buffer, native);
}

} // namespace Libs::Graphics
