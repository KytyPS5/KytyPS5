#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERDOC_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERDOC_H_

namespace Libs::Graphics {

void RenderDocInit();
void RenderDocRequestCapture();
void RenderDocStartCapture();
void RenderDocEndCapture();
void RenderDocOnGuestFlip();

[[nodiscard]] bool RenderDocCaptureRequested();
[[nodiscard]] bool RenderDocCaptureInProgress();

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERDOC_H_ */
