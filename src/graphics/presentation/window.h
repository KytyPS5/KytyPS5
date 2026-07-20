#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "common/abi.h"
#include "common/common.h"

struct VkSurfaceCapabilitiesKHR;

namespace Libs::Graphics {

struct GraphicContext;
class CommandBuffer;
struct VideoOutVulkanImage;
struct PreparedFrame;

VkSurfaceCapabilitiesKHR* VulkanGetSurfaceCapabilities();

GraphicContext* WindowGetGraphicContext();

void           WindowInit(uint32_t width, uint32_t height);
void           WindowRun();
void           WindowWaitForGraphicInitialized();
PreparedFrame* WindowPrepareFrame(CommandBuffer* buffer, VideoOutVulkanImage* image);
PreparedFrame* WindowPrepareBlankFrame(CommandBuffer* buffer, uint32_t width, uint32_t height,
                                       bool opaque);
void           WindowPresentFrame(PreparedFrame* frame);

// M1W3: when enabled, the window thread prints a once-per-second
// FPS summary to stdout. Independent of the in-window title-bar FPS
// (which is always shown). Useful for headless measurement runs.
void           SetFpsStdoutEnabled(bool enabled);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */

