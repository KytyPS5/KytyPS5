#ifndef KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_
#define KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_

#include <cstdint>

union SDL_Event;

namespace Libs::Graphics {

enum class HostInputSource : uint8_t { None, Keyboard, Mouse, Touch, Controller, Microphone };

struct HostInputDiagnostics {
	HostInputSource source            = HostInputSource::None;
	uint32_t        capabilities      = 0;
	float           left_trigger      = 0.0f;
	float           right_trigger     = 0.0f;
	float           microphone        = 0.0f;
	float           gyro              = 0.0f;
	bool            analog_lock       = false;
	bool            adjustment_active = false;
	bool            gyro_active       = false;
	uint64_t        revision          = 0;
};

void                               HostInputInit();
void                               HostInputKey(int key_code, bool down);
void                               HostInputScancode(int scan_code, int key_code, bool down);
[[nodiscard]] bool                 HostInputCapsLockEnabled();
[[nodiscard]] HostInputDiagnostics HostInputGetDiagnostics();
void                               HostInputMouseButton(uint8_t mouse_button, bool down);
void                               HostInputMouseWheel(int direction);
void HostInputFinger(bool down, bool up, bool motion, float x, float y, float dx, float dy);
void HostInputToggleMouseToJoystick();
[[nodiscard]] bool HostInputWaitEvent(SDL_Event* event);

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_PRESENTATION_WINDOW_HOST_INPUT_H_ */
