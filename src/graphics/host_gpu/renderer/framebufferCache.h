#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEBUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEBUFFERCACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderState.h"

#include <vector>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

static constexpr VkImageLayout RENDER_COLOR_IMAGE_LAYOUT = VK_IMAGE_LAYOUT_GENERAL;

struct VulkanFramebuffer {
	VkRenderPass  render_pass                                = nullptr;
	uint64_t      render_pass_id                             = 0;
	VkFramebuffer framebuffer                                = nullptr;
	VkImageLayout color_layout[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	VkImageLayout depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
};

inline uint64_t render_pass_compat_id(uint32_t color_count, const VkFormat* color_formats,
                                      bool with_depth, VkFormat depth_format,
                                      VkImageLayout depth_layout) {
	uint64_t id  = 0xcbf29ce484222325ull;
	auto     mix = [&id](uint64_t v) {
		id ^= v;
		id *= 0x100000001b3ull;
	};

	mix(color_count);
	for (uint32_t i = 0; i < color_count; i++) {
		mix(static_cast<uint32_t>(color_formats[i]));
	}
	mix(with_depth ? 1u : 0u);
	mix(static_cast<uint32_t>(depth_format));
	mix(static_cast<uint32_t>(depth_layout));
	mix(VK_SAMPLE_COUNT_1_BIT);

	return id;
}

class FramebufferCache {
public:
	FramebufferCache() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~FramebufferCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(FramebufferCache);

	VulkanFramebuffer* CreateFramebuffer(RenderColorInfo* colors, uint32_t color_count,
	                                     RenderDepthInfo* depth);
	void               FreeFramebufferByColor(VulkanImage* image);
	void               FreeFramebufferByDepth(DepthStencilVulkanImage* image);

private:
	VideoOutVulkanImage* CreateDummyBuffer(VkFormat format, uint32_t width, uint32_t height);

	struct Framebuffer {
		VulkanFramebuffer* framebuffer                                      = nullptr;
		uint64_t           image_id[RENDER_COLOR_ATTACHMENTS_MAX]           = {};
		VkImageView        color_view[RENDER_COLOR_ATTACHMENTS_MAX]         = {};
		uint64_t           depth_id                                         = 0;
		bool               color_clear_enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
		VkImageLayout      color_layout[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
		bool               depth_clear_enable                               = false;
		bool               stencil_clear_enable                             = false;
		bool               depth_read_only                                  = false;
	};

	Common::Mutex                     m_mutex;
	std::vector<Framebuffer>          m_framebuffers;
	std::vector<VideoOutVulkanImage*> m_dummy_buffers;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEBUFFERCACHE_H_
