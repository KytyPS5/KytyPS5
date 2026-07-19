#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERDOC_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERDOC_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

struct SDL_Window;

namespace Libs::Graphics {

void RenderDocInit();
bool RenderDocIsLoaded();
void RenderDocSetActiveWindow(vk::Instance instance, SDL_Window* window);
void RenderDocRequestCapture();
void RenderDocOnPresent();

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERDOC_H_ */
