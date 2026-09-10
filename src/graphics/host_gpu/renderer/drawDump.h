#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWDUMP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWDUMP_H_

#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

namespace Libs::Graphics {

class RenderContext;
class CommandScheduler;
class CommandBuffer;
struct VulkanImage;

// --draw-dump-folder debug tool: queues a GPU->CPU readback of every color attachment in
// `state`, and once the GPU has actually finished rendering to it, writes it out as a PNG.
// Meant to be called right before a CommandBuffer stops actively rendering into `state`
// (CommandScheduler::EndRendering), as a coarser, render-pass-batching-safe substitute for
// per-draw RenderDoc capture. Debug-only: does nothing unless Config::DrawDumpEnabled().
void DumpColorAttachments(RenderContext& context, const RenderState& state);

// --present-dump debug tool: records a GPU->CPU readback of the actual presented/flipped frame
// (the final composited swapchain blit source) every Config::GetPresentDumpEvery() guest frames.
// Unlike DumpColorAttachments, this reads a raw VulkanImage rather than a texture-cache-tracked
// one -- the present path holds no Image wrapper -- so it is not a DumpColorAttachments call,
// only a shared encode/write path. Meant to be called right after Swapchain::RecordPresentCommands,
// on the same CommandBuffer and with the same source image (already confirmed
// vk::ImageLayout::eTransferSrcOptimal by that call, so no additional image barrier is recorded
// here). `scheduler` must be the same CommandScheduler `command` was obtained from.
//
// The present path's CommandScheduler is a minimal blit/present scheduler that never binds a
// guest HW::Context, so it never satisfies CommandScheduler::Active() -- CommandScheduler::
// DeferOperation() is therefore unusable here (it asserts Active()). Instead, this function only
// *records* the copy-to-buffer and returns a finish callback; the caller must Submit(), then
// Wait() on the returned tick, then invoke the callback to invalidate the download buffer and
// write the PNG. Returns an empty (falsy) callback when nothing was dumped this frame. Debug-only:
// does nothing unless Config::PresentDumpEnabled().
[[nodiscard]] Common::UniqueFunction<void> DumpPresentedFrame(CommandBuffer& command,
                                                                 CommandScheduler& scheduler,
                                                                 VulkanImage&      source,
                                                                 int                frame_num);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWDUMP_H_
