#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_OPENXRHEADSET_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_OPENXRHEADSET_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <vector>

namespace Libs::Graphics {
struct GraphicContext;
struct VulkanImage;
class CommandBuffer;
} // namespace Libs::Graphics

// Output to a real headset through OpenXR. Everything here is inert unless the virtual headset is
// enabled (--vr) and an OpenXR runtime with a headset is available; KYTY_OPENXR=0 turns it off.
namespace Libs::Vr {

// Creates the OpenXR instance. Call before the Vulkan instance exists, because the runtime names
// the Vulkan extensions and the GPU it needs.
void OpenXrInit();

// Appends the Vulkan instance or device extensions the runtime needs, when available.
void OpenXrAppendExtensions(std::vector<const char*>&                   out,
                            const std::vector<vk::ExtensionProperties>& available, bool device);

// The physical device the runtime renders with. False when OpenXR is not in use.
bool OpenXrPhysicalDevice(vk::Instance instance, vk::PhysicalDevice& out);

// Creates the session and starts the headset thread. Call once the device and queue exist.
void OpenXrStartSession(Graphics::GraphicContext& graphics);

// Stops the headset thread and destroys the session. Call before the device is destroyed.
void OpenXrShutdown();

// Render thread, headset flips only: scales the side-by-side eye image into a staging image for the
// headset thread. The source is in transfer-source layout.
void OpenXrQueueEyeSource(Graphics::CommandBuffer& command, const Graphics::VulkanImage& source);

} // namespace Libs::Vr

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_OPENXRHEADSET_H_
