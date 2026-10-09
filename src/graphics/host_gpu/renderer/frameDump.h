#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEDUMP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEDUMP_H_

// "Poor man's RenderDoc": KYTY_DBG_FRAME_DUMP=1 enables a frame dump that is triggered with F2 in the
// game window. The next complete guest frame (flip to flip) is captured: every draw/dispatch is
// logged to <captures>/frame_<n>/frame.txt and every render pass's color/depth targets and every
// dispatch's written storage images are read back and saved as PNG. Zero cost when the flag is off
// (one relaxed atomic load per draw/dispatch/end-of-render-pass).

#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct TextureBinding;

namespace FrameDump {

extern std::atomic<bool> g_active;

[[nodiscard]] inline bool Active() noexcept {
	return g_active.load(std::memory_order_relaxed);
}

// True when KYTY_DBG_FRAME_DUMP is set (cached).
[[nodiscard]] bool Enabled() noexcept;

// F2 in the game window: capture the next complete guest frame.
void Request();

// Called when the guest submits a flip (command processor thread). Starts or finishes a capture.
void OnGuestFlip(CommandBuffer& buffer);

struct TargetRef {
	vk::ImageView                   view = nullptr;
	ImageId                         image_id;
	uint32_t                        slot = 0;
	vk::ImageLayout                 layout = vk::ImageLayout::eUndefined;
	const TextureCache::ImageDesc*  desc   = nullptr;
	uint32_t                        mip    = 0;
	uint32_t                        layer  = 0;
	vk::Extent2D                    extent {};
};

struct DrawLog {
	uint64_t                         vs_hash        = 0;
	uint64_t                         ps_hash        = 0;
	uint32_t                         count          = 0;
	uint32_t                         instances      = 0;
	bool                             indexed        = false;
	bool                             mesh           = false;
	bool                             indirect       = false;
	bool                             ps_active      = false;
	bool                             depth_test     = false;
	bool                             depth_write    = false;
	vk::CompareOp                    depth_compare  = vk::CompareOp::eNever;
	std::span<const TargetRef>       colors;
	const TargetRef*                 depth = nullptr;
	const std::vector<TextureBinding>* vs_images[3] {};
	const std::vector<TextureBinding>* ps_images = nullptr;
};

// Registers the draw's render targets (so the end of the render pass can read them back) and logs it.
void OnDraw(RenderContext& context, const DrawLog& draw);

// Called from CommandBuffer::EndRendering after vkCmdEndRendering, with the pass that just ended.
void OnEndRendering(const CommandBuffer& buffer, const RenderState& pass);

struct StorageUse {
	const TextureBinding* binding = nullptr;
	bool                  written = false;
	bool                  storage = false;
};

// Called after a dispatch was recorded; logs it and reads back the storage images it wrote.
void OnDispatch(RenderContext& context, CommandBuffer& buffer, uint64_t cs_hash, uint32_t gx,
                uint32_t gy, uint32_t gz, bool indirect, std::span<const StorageUse> images);

} // namespace FrameDump
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEDUMP_H_
