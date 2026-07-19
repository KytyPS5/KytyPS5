#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

struct GraphicContext;
class CommandBuffer;
struct VideoOutVulkanImage;
struct PreparedFrame;

vk::SurfaceCapabilitiesKHR* VulkanGetSurfaceCapabilities();

GraphicContext* WindowGetGraphicContext();

void           WindowInit(uint32_t width, uint32_t height);
void           WindowRun();
void           WindowWaitForGraphicInitialized();
PreparedFrame* WindowPrepareFrame(CommandBuffer* buffer, VideoOutVulkanImage* image);
PreparedFrame* WindowPrepareBlankFrame(CommandBuffer* buffer, uint32_t width, uint32_t height,
                                       bool opaque);
void           WindowPresentFrame(PreparedFrame* frame);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
