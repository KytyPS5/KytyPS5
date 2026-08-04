#ifndef KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_
#define KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_

#include <cstdint>

namespace Libs::Graphics {

void HostInputInit();
void HostInputKey(int key_code, bool down);
void HostInputMouseButton(uint8_t mouse_button, bool down);

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_ */
