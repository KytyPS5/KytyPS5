#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_HARDWARE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_HARDWARE_H_

#include "graphics/host_gpu/graphicContext.h"

#include <vector>

namespace Libs::Graphics {

namespace Rt {

void AppendHardwareRayTracingDeviceExtensions(
    const std::vector<vk::ExtensionProperties>& available_extensions,
    std::vector<const char*>* device_extensions, GraphicContext* ctx);

void LoadHardwareRayTracingFunctions(GraphicContext* ctx);

} // namespace Rt

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_HARDWARE_H_ */
