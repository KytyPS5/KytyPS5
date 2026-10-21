#ifndef KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_
#define KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_

#include <cstdint>

struct SDL_Window;
union SDL_Event;

namespace Libs::Graphics {

struct HostInputTouchPadVisualState {
	bool     enabled        = false;
	bool     cursor_visible = true;
	bool     drawing        = false;
	bool     position_valid = false;
	float    x              = 0.5f;
	float    y              = 0.5f;
	uint64_t revision       = 0;
	uint64_t gesture        = 0;
};

void               HostInputInit(SDL_Window* window);
void               HostInputShutdown();
void               HostInputKey(int key_code, bool down);
void               HostInputMouseButton(uint8_t mouse_button, bool down);
void               HostInputMouseTouchPad(bool down, float x, float y);
void               HostInputMouseMotion(float x, float y);
void               HostInputPadButton(int id, int sdl_button, bool down);
void               HostInputPadTrigger(int id, bool left, int value);
void               HostInputDisconnectGamepad(int id);
void               HostInputSetTouchPadMode(bool enabled);
void               HostInputToggleTouchPadMode();
void               HostInputToggleGamepadTouchPad(int id);
[[nodiscard]] bool HostInputGamepadDrawButton(int id, bool down);
[[nodiscard]] bool HostInputGamepadAxis(int id, bool horizontal, int value);
void               HostInputDisconnectGamepadTouchPad(int id);
[[nodiscard]] HostInputTouchPadVisualState GetHostInputTouchPadVisualState();
void               HostInputToggleMouseToJoystick();
[[nodiscard]] bool HostInputWaitEvent(SDL_Event* event, int max_wait_ms);
// The PS button of a gamepad went down or up. True when a short press ended; a hold closes the
// game (gamepadQuit.h).
[[nodiscard]] bool HostInputGamepadGuide(int gamepad, bool down);
// A trigger of a gamepad moved to `value` (0 to 32767): a hold of both closes the game
// (gamepadQuit.h).
void               HostInputGamepadTrigger(int gamepad, bool right, int value);
void               HostInputGamepadRemoved(int gamepad);
// Player 1 held the close buttons of its gamepad long enough.
[[nodiscard]] bool HostInputQuitRequested();

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_ */
