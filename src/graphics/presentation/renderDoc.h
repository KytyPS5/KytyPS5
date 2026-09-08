#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERDOC_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERDOC_H_

namespace Libs::Graphics {

class RenderContext;

void RenderDocInit();
void RenderDocRequestCapture();
// Called by the presentation thread after releasing video-out locks.
void RenderDocOnGuestFlip(RenderContext& renderer);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERDOC_H_ */
