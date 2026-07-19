#include "graphics/host_gpu/renderer/renderTargetBarriers.h"

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
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/displayBuffer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>

namespace Libs::Graphics {

void GraphicsRenderMemoryBarrier(CommandBuffer* buffer) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Common::LockGuard lock(g_render_ctx->GetMutex());

	auto vk_buffer = buffer->Handle();

	VulkanMemoryBarrier mem_barrier {};
	mem_barrier.sType         = vk::StructureType::eMemoryBarrier;
	mem_barrier.pNext         = nullptr;
	mem_barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
	mem_barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;

	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                          vk::PipelineStageFlagBits::eAllCommands, vk::DependencyFlags {}, 1,
	                          &mem_barrier, 0, nullptr, 0, nullptr);
}

void GraphicsRenderTextureBarrier(vk::CommandBuffer vk_buffer, VulkanImage* image) {
	EXIT_IF(image == nullptr);

	vk::ImageMemoryBarrier image_memory_barrier {};
	image_memory_barrier.sType = vk::StructureType::eImageMemoryBarrier;
	image_memory_barrier.pNext = nullptr;
	image_memory_barrier.srcAccessMask =
	    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eTransferWrite |
	    vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eMemoryRead;
	image_memory_barrier.dstAccessMask                   = vk::AccessFlagBits::eShaderRead;
	image_memory_barrier.oldLayout                       = image->layout;
	image_memory_barrier.newLayout                       = RENDER_COLOR_IMAGE_LAYOUT;
	image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.image                           = image->image;
	image_memory_barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	image_memory_barrier.subresourceRange.baseMipLevel   = 0;
	image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	image_memory_barrier.subresourceRange.baseArrayLayer = 0;
	image_memory_barrier.subresourceRange.layerCount     = image->layers;

	vk_buffer.pipelineBarrier(
	    vk::PipelineStageFlagBits::eColorAttachmentOutput |
	        vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer |
	        vk::PipelineStageFlagBits::eFragmentShader,
	    vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader |
	        vk::PipelineStageFlagBits::eComputeShader,
	    vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &image_memory_barrier);

	image->layout = image_memory_barrier.newLayout;
}

void GraphicsRenderColorImageBarrier(vk::CommandBuffer vk_buffer, VulkanImage* image,
                                     vk::ImageLayout new_layout) {
	EXIT_IF(image == nullptr);

	if (image->layout != new_layout) {
		if (image->type == VulkanImageType::VideoOut &&
		    image->layout == vk::ImageLayout::eTransferDstOptimal) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 128) {
				LOGF("GraphicsRenderColorImageBarrier: image=%p type=%d layout=%s -> %s\n",
				     VulkanHandleToPointer(image->image), static_cast<int>(image->type),
				     VulkanToString(image->layout).c_str(), VulkanToString(new_layout).c_str());
			}
		}
		vk::ImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType = vk::StructureType::eImageMemoryBarrier;
		image_memory_barrier.pNext = nullptr;
		image_memory_barrier.srcAccessMask =
		    vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eMemoryRead;
		image_memory_barrier.dstAccessMask =
		    vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eMemoryRead;
		image_memory_barrier.oldLayout                       = image->layout;
		image_memory_barrier.newLayout                       = new_layout;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = image->image;
		image_memory_barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = image->layers;

		const auto stages = static_cast<vk::PipelineStageFlags>(
		    vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader |
		    vk::PipelineStageFlagBits::eTransfer);
		vk_buffer.pipelineBarrier(stages, stages, vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1,
		                          &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}
}

void GraphicsRenderDepthStencilImageBarrier(vk::CommandBuffer vk_buffer, VulkanImage* image,
                                            vk::ImageLayout new_layout) {
	EXIT_IF(image == nullptr);
	EXIT_IF(image->type != VulkanImageType::DepthStencil);
	if (new_layout == vk::ImageLayout::eUndefined ||
	    new_layout == vk::ImageLayout::ePreinitialized) {
		EXIT("invalid destination depth/stencil image layout: %s\n",
		     VulkanToString(new_layout).c_str());
	}
	if (image->layout == new_layout) {
		return;
	}
	vk::ImageMemoryBarrier barrier {};
	barrier.sType         = vk::StructureType::eImageMemoryBarrier;
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	barrier.oldLayout     = image->layout;
	barrier.newLayout     = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image->image;
	barrier.subresourceRange    = {ImageViewOps::DepthAspectMask(image->format), 0,
	                               VK_REMAINING_MIP_LEVELS, 0, image->layers};
	const auto stages           = static_cast<vk::PipelineStageFlags>(
	    vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader |
	    vk::PipelineStageFlagBits::eTransfer);
	vk_buffer.pipelineBarrier(stages, stages, vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1,
	                          &barrier);
	image->layout = new_layout;
}

void GraphicsRenderDepthStencilBarrier(vk::CommandBuffer vk_buffer, VulkanImage* image) {
	EXIT_IF(image == nullptr);

	EXIT_IF(image->type != VulkanImageType::DepthStencil);

	auto* depth = static_cast<DepthStencilVulkanImage*>(image);
	if (depth->compressed) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF(
			    "DepthTexture: decompressing depth target for shader read format=%s extent=%ux%u\n",
			    VulkanToString(depth->format).c_str(), depth->extent.width, depth->extent.height);
		}
		depth->compressed = false;
	}

	if (image->layout != vk::ImageLayout::eDepthStencilReadOnlyOptimal) {
		vk::ImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType = vk::StructureType::eImageMemoryBarrier;
		image_memory_barrier.pNext = nullptr;
		image_memory_barrier.srcAccessMask =
		    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		image_memory_barrier.dstAccessMask       = vk::AccessFlagBits::eShaderRead;
		image_memory_barrier.oldLayout           = image->layout;
		image_memory_barrier.newLayout           = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
		image_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image               = image->image;
		image_memory_barrier.subresourceRange.aspectMask =
		    ImageViewOps::DepthAspectMask(image->format);
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = image->layers;

		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader |
		        vk::PipelineStageFlagBits::eTransfer,
		    vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader |
		        vk::PipelineStageFlagBits::eComputeShader,
		    vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}
}

void GraphicsRenderTextureBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size) {
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Common::LockGuard lock(g_render_ctx->GetMutex());

	auto vk_buffer = buffer->Handle();

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

	auto  vk_buffer = buffer->Handle();
	auto* native    = g_render_ctx->GetTextureCache()->FindDepthTargetByRange(buffer, vaddr, size);
	if (native == nullptr) {
		EXIT("depth-target barrier range has no cached image\n");
	}
	GraphicsRenderDepthStencilBarrier(vk_buffer, native);
}

} // namespace Libs::Graphics
