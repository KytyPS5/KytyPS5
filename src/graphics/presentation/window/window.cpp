#include "graphics/presentation/window.h"

#include <SDL3/SDL.h>

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/timer.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCompileProgress.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/window/hostInput.h"
#include "graphics/presentation/window/windowInternal.h"
#include "kytyGitVersion.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <cstdlib>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vk_platform.h>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include "stb_image.h"

// IWYU pragma: no_include <intrin.h>

#define KYTY_DBG_INPUT

namespace Libs::Graphics {

struct EventKeyboard {
	bool     down;
	bool     up;
	bool     pressed;
	bool     released;
	bool     repeat;
	int      scan_code;
	int      key_code;
	uint16_t mod;
	double   timestamp_seconds;
};

static uint32_t ControllerButtonToPadButton(int button) {
	switch (button) {
		case SDL_GAMEPAD_BUTTON_SOUTH: return Controller::PAD_BUTTON_CROSS;
		case SDL_GAMEPAD_BUTTON_EAST: return Controller::PAD_BUTTON_CIRCLE;
		case SDL_GAMEPAD_BUTTON_WEST: return Controller::PAD_BUTTON_SQUARE;
		case SDL_GAMEPAD_BUTTON_NORTH: return Controller::PAD_BUTTON_TRIANGLE;
		case SDL_GAMEPAD_BUTTON_START: return Controller::PAD_BUTTON_OPTIONS;
		case SDL_GAMEPAD_BUTTON_LEFT_STICK: return Controller::PAD_BUTTON_L3;
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return Controller::PAD_BUTTON_R3;
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Controller::PAD_BUTTON_L1;
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Controller::PAD_BUTTON_R1;
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return Controller::PAD_BUTTON_UP;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Controller::PAD_BUTTON_DOWN;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Controller::PAD_BUTTON_LEFT;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Controller::PAD_BUTTON_RIGHT;
		case SDL_GAMEPAD_BUTTON_TOUCHPAD: return Controller::PAD_BUTTON_TOUCH_PAD;
		default: return 0;
	}
}

static Controller::Axis ControllerAxisFromSdl(int axis_id) {
	switch (axis_id) {
		case SDL_GAMEPAD_AXIS_LEFTX: return Controller::Axis::LeftX;
		case SDL_GAMEPAD_AXIS_LEFTY: return Controller::Axis::LeftY;
		case SDL_GAMEPAD_AXIS_RIGHTX: return Controller::Axis::RightX;
		case SDL_GAMEPAD_AXIS_RIGHTY: return Controller::Axis::RightY;
		case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return Controller::Axis::TriggerLeft;
		case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return Controller::Axis::TriggerRight;
		default: return Controller::Axis::AxisMax;
	}
}

static bool ControllerAxisIsTrigger(int axis_id) {
	return axis_id == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
	       axis_id == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
}

static int ControllerAxisValueFromSdl(int axis_id, int axis_value) {
	return ControllerAxisIsTrigger(axis_id)
	           ? Controller::controller_get_axis(0, SDL_JOYSTICK_AXIS_MAX, axis_value)
	           : Controller::controller_get_axis(SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX,
	                                             axis_value);
}

struct EventMouse {
	bool   down;
	bool   up;
	bool   left;
	bool   middle;
	bool   right;
	bool   x1;
	bool   x2;
	bool   touch;
	bool   pressed;
	bool   released;
	int    num_of_clicks;
	bool   wheel;
	int    x;
	int    y;
	bool   motion;
	int    motion_x;
	int    motion_y;
	double timestamp_seconds;
};

struct EventFinger {
	bool   down;
	bool   up;
	bool   motion;
	int    touch_id;
	int    finger_id;
	float  x;
	float  y;
	float  dx;
	float  dy;
	float  pressure;
	double timestamp_seconds;
};

struct EventController {
	int    id;
	int    button;
	int    axis_id;
	int    axis_value;
	bool   down;
	bool   up;
	bool   added;
	bool   removed;
	bool   remapped;
	bool   axis;
	bool   pressed;
	bool   released;
	double timestamp_seconds;
};

namespace {

std::unique_ptr<WindowContext> g_window;

} // namespace

constexpr const char* KYTY_SDL_WINDOW_CAPTION = "Game";

static void SetPause(WindowLoopState& game, bool flag) {
	LOGF("Pause: %s\n", flag ? "true" : "false");

	game.paused.store(flag, std::memory_order_release);
}

static void GameEventQuit(WindowLoopState& game) {
	LOGF("Event: quit\n");

	game.need_exit = true;
}

static void GameEventTerminate(WindowLoopState& game) {
	LOGF("Event: terminate\n");

	game.need_exit = true;
}

static void ToggleDesktopFullscreen() {
	if (g_window == nullptr || g_window->window == nullptr) {
		return;
	}

	const auto flags = static_cast<uint32_t>(SDL_GetWindowFlags(g_window->window));
	const bool fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0u;
	if (!SDL_SetWindowFullscreen(g_window->window, !fullscreen)) {
		LOGF("Toggle fullscreen failed: %s\n", SDL_GetError());
	}
}

static void GameEventKeyboard(const EventKeyboard& key) {
	static SDL_Keycode fullscreen_key = SDLK_UNKNOWN;

#ifdef KYTY_DBG_INPUT
	LOGF("Key: time = %.04f, %s%s, %s%s, %s, scan = %d, key = %d, mod = %04" PRIx16 "\n",
	     key.timestamp_seconds, (key.down ? "down" : ""), (key.up ? "up" : ""),
	     (key.pressed ? "pressed" : ""), (key.released ? "released" : ""),
	     (key.repeat ? "repeat" : ""), key.scan_code, key.key_code, key.mod);
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	if (key.down) {
		switch (key.key_code) {
			case SDLK_F1:
				if (!key.repeat) {
					RenderDocRequestCapture();
				}
				break;
			case SDLK_F11:
				if (!key.repeat) {
					ToggleDesktopFullscreen();
				}
				break;
			case SDLK_RETURN:
			case SDLK_KP_ENTER:
				if (!key.repeat && (key.mod & SDL_KMOD_ALT) != 0) {
					ToggleDesktopFullscreen();
					fullscreen_key = key.key_code;
				}
				break;
			default: break;
		}
	}

#endif

	const bool fullscreen_key_event =
	    fullscreen_key != SDLK_UNKNOWN && key.key_code == fullscreen_key;
	if (fullscreen_key_event && key.up) {
		fullscreen_key = SDLK_UNKNOWN;
	}
	if ((key.down || key.up) && !key.repeat && !fullscreen_key_event) {
		HostInputKey(key.key_code, key.down);
	}
}

static void GameEventMouse([[maybe_unused]] const EventMouse& mb) {
#ifdef KYTY_DBG_INPUT
	if (mb.wheel) {
		LOGF("Mouse wheel: time = %.04f, %s[%d, %d]\n", mb.timestamp_seconds,
		     (mb.touch ? "touch, " : ""), mb.x, mb.y);
	} else if (mb.motion) {
		LOGF("Mouse motion: time = %.04f, %s%s%s%s%s%s, [%d, %d], (%d, %d)\n", mb.timestamp_seconds,
		     (mb.left ? "left" : ""), (mb.middle ? "middle" : ""), (mb.right ? "right" : ""),
		     (mb.x1 ? "x1" : ""), (mb.x2 ? "x2" : ""), (mb.touch ? "_touch" : ""), mb.x, mb.y,
		     mb.motion_x, mb.motion_y);
	} else {
		LOGF("Mouse click: time = %.04f, %d, %s%s%s%s%s%s, %s%s, %s%s, [%d, %d]\n",
		     mb.timestamp_seconds, mb.num_of_clicks, (mb.left ? "left" : ""),
		     (mb.middle ? "middle" : ""), (mb.right ? "right" : ""), (mb.x1 ? "x1" : ""),
		     (mb.x2 ? "x2" : ""), (mb.touch ? "_touch" : ""), (mb.down ? "down" : ""),
		     (mb.up ? "up" : ""), (mb.pressed ? "pressed" : ""), (mb.released ? "released" : ""),
		     mb.x, mb.y);
	}
#endif

	if (mb.down || mb.up) {
		uint8_t mouse_button = 0;
		if (mb.left) {
			mouse_button = SDL_BUTTON_LEFT;
		} else if (mb.middle) {
			mouse_button = SDL_BUTTON_MIDDLE;
		} else if (mb.right) {
			mouse_button = SDL_BUTTON_RIGHT;
		} else if (mb.x1) {
			mouse_button = SDL_BUTTON_X1;
		} else if (mb.x2) {
			mouse_button = SDL_BUTTON_X2;
		}

		HostInputMouseButton(mouse_button, mb.down);
	}
}

static void GameEventFinger([[maybe_unused]] const EventFinger& f) {
#ifdef KYTY_DBG_INPUT
	if (f.motion) {
		LOGF("Finger motion: time = %.04f, %d, %d, (x,y) = [%f, %f], (dx,dy) = [%f, %f], pressure "
		     "= %f\n",
		     f.timestamp_seconds, f.touch_id, f.finger_id, f.x, f.y, f.dx, f.dy, f.pressure);
	} else {
		LOGF("Finger press: time = %.04f, %d, %d, %s%s, (x,y) = [%f, %f], (dx,dy) = [%f, %f], "
		     "pressure = %f\n",
		     f.timestamp_seconds, f.touch_id, f.finger_id, (f.down ? "down" : ""),
		     (f.up ? "up" : ""), f.x, f.y, f.dx, f.dy, f.pressure);
	}
#endif
}

static void GameEventController([[maybe_unused]] const EventController& f) {
	EXIT_NOT_IMPLEMENTED(f.remapped);

#ifdef KYTY_DBG_INPUT
	if (f.added || f.removed) {
		LOGF("Controller %s: %d, time = %.04f\n", (f.added ? "added" : "removed"), f.id,
		     f.timestamp_seconds);
	} else if (f.axis) {
		LOGF("Controller axis: %d, axis = %d, value = %d, time = %.04f\n", f.id, f.axis_id,
		     f.axis_value, f.timestamp_seconds);
	} else {
		LOGF("Controller button: "
		     "%d, %s%s, %s%s, button = %d, time = %.04f\n",
		     f.id, (f.down ? "down" : ""), (f.up ? "up" : ""), (f.pressed ? "pressed" : ""),
		     (f.released ? "released" : ""), f.button, f.timestamp_seconds);
	}
#endif

	if (f.added) {
		auto* pad = SDL_OpenGamepad(f.id);
		EXIT_NOT_IMPLEMENTED(pad == nullptr);
		int id = SDL_GetJoystickID(SDL_GetGamepadJoystick(pad));
		Controller::Connect(id);
	}

	if (f.removed) {
		Controller::Disconnect(f.id);
		SDL_CloseGamepad(SDL_GetGamepadFromID(f.id));
	}

	if (f.down || f.up) {
		const auto button = ControllerButtonToPadButton(f.button);
		if (button != 0) {
			Controller::SetButton(f.id, button, f.down);
		}
	}

	if (f.axis) {
		const auto axis = ControllerAxisFromSdl(f.axis_id);
		if (axis != Controller::Axis::AxisMax) {
			Controller::SetAxis(f.id, axis, ControllerAxisValueFromSdl(f.axis_id, f.axis_value));
		}
	}
}

static void GameEventLowMemory() {
	LOGF("Event: low_memory\n");
}

static void GameEventWillEnterBackground(WindowLoopState& game) {
	LOGF("Event: will_enter_background\n");

	SetPause(game, true);
}

static void GameEventDidEnterBackground() {
	LOGF("Event: did_enter_background\n");
}

static void GameEventWillEnterForeground() {
	LOGF("Event: will_enter_foreground\n");
}

static void GameEventDidEnterForeground(WindowLoopState& game) {
	LOGF("Event: did_enter_foreground\n");

	SetPause(game, false);
}

void WindowContext::Resize(uint32_t new_width, uint32_t new_height) {
	EXIT_IF(new_width == 0 || new_height == 0);
	Common::LockGuard lock(mutex);
	graphic_ctx.screen_width  = new_width;
	graphic_ctx.screen_height = new_height;
}

void WindowContext::ProcessWindowEvent(const SDL_WindowEvent& event) {
	const auto& window_event = event;
	switch (window_event.type) {
		case SDL_EVENT_WINDOW_SHOWN:
			LOGF("Window %" PRIu32 " shown\n", window_event.windowID);
			break;

		case SDL_EVENT_WINDOW_HIDDEN:
			LOGF("Window %" PRIu32 " hidden\n", window_event.windowID);
			break;

		case SDL_EVENT_WINDOW_EXPOSED:
			LOGF("Window %" PRIu32 " exposed\n", window_event.windowID);
			break;

		case SDL_EVENT_WINDOW_MOVED:
			LOGF("Window %" PRIu32 " moved to %" PRId32 ",%" PRId32 "\n", window_event.windowID,
			     window_event.data1, window_event.data2);
			break;

		case SDL_EVENT_WINDOW_RESIZED:
			LOGF("Window %" PRIu32 " resized to %" PRId32 "x%" PRId32 "\n", window_event.windowID,
			     window_event.data1, window_event.data2);
			break;

		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			LOGF("Window %" PRIu32 " size changed to %" PRId32 "x%" PRId32 "\n",
			     window_event.windowID, window_event.data1, window_event.data2);

			Resize(window_event.data1, window_event.data2);

			break;

		case SDL_EVENT_WINDOW_MINIMIZED:
			LOGF("Window %" PRIu32 " minimized\n", window_event.windowID);
			break;
		case SDL_EVENT_WINDOW_MAXIMIZED:
			LOGF("Window %" PRIu32 " maximized\n", window_event.windowID);
			break;
		case SDL_EVENT_WINDOW_RESTORED:
			LOGF("Window %" PRIu32 " restored\n", window_event.windowID);
			break;
		case SDL_EVENT_WINDOW_MOUSE_ENTER:
			LOGF("Mouse entered window %" PRIu32 "\n", window_event.windowID);
			break;
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			LOGF("Mouse left window %" PRIu32 "\n", window_event.windowID);
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			LOGF("Window %" PRIu32 " gained keyboard focus\n", window_event.windowID);
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			LOGF("Window %" PRIu32 " lost keyboard focus\n", window_event.windowID);
			break;
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			LOGF("Window %" PRIu32 " closed\n", window_event.windowID);
			break;
		default:
			LOGF("Window %" PRIu32 " got unknown event %" PRIu32 "\n", window_event.windowID,
			     static_cast<uint32_t>(window_event.type));
			break;
	}
}

void WindowContext::ProcessDisplayEvent(const SDL_DisplayEvent& display) {
	switch (display.type) {
		case SDL_EVENT_DISPLAY_ORIENTATION: {
			LOGF("Display %" PRIu32 " changed orientation to %d - ", display.displayID,
			     static_cast<int>(display.data1));

			switch (display.data1) {
				case SDL_ORIENTATION_UNKNOWN: LOGF("UNKNOWN\n"); break;
				case SDL_ORIENTATION_LANDSCAPE: LOGF("LANDSCAPE\n"); break;
				case SDL_ORIENTATION_LANDSCAPE_FLIPPED: LOGF("LANDSCAPE_FLIPPED\n"); break;
				case SDL_ORIENTATION_PORTRAIT: LOGF("PORTRAIT\n"); break;
				case SDL_ORIENTATION_PORTRAIT_FLIPPED: LOGF("PORTRAIT_FLIPPED\n"); break;
				default: LOGF("???\n");
			}

			break;
		}
		default:
			LOGF("Display %" PRIu32 " got unknown event 0x%" PRIx32 "\n", display.displayID,
			     static_cast<uint32_t>(display.type));
			break;
	}
}

void WindowContext::ProcessEvent(double time_s) {
	auto& game  = loop;
	auto* event = &game.event;
	if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
	    event->key.key == SDLK_F7) {
		if (event->type == SDL_EVENT_KEY_DOWN && event->key.repeat == 0) {
			HostInputToggleMouseToJoystick();
		}
		return;
	}
	if (ProcessSystemOverlayInput(*event)) {
		return;
	}
	if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST) {
		ProcessWindowEvent(event->window);
		return;
	}
	if (event->type >= SDL_EVENT_DISPLAY_FIRST && event->type <= SDL_EVENT_DISPLAY_LAST) {
		ProcessDisplayEvent(event->display);
		return;
	}

	switch (event->type) {
		case SDL_EVENT_QUIT: GameEventQuit(game); break;

		case SDL_EVENT_TERMINATING: GameEventTerminate(game); break;

		case SDL_EVENT_LOW_MEMORY: GameEventLowMemory(); break;

		case SDL_EVENT_WILL_ENTER_BACKGROUND: GameEventWillEnterBackground(game); break;

		case SDL_EVENT_DID_ENTER_BACKGROUND: GameEventDidEnterBackground(); break;

		case SDL_EVENT_WILL_ENTER_FOREGROUND: GameEventWillEnterForeground(); break;

		case SDL_EVENT_DID_ENTER_FOREGROUND: GameEventDidEnterForeground(game); break;

		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: {
			EventKeyboard key {};

			key.down              = (event->type == SDL_EVENT_KEY_DOWN);
			key.up                = (event->type == SDL_EVENT_KEY_UP);
			key.pressed           = event->key.down;
			key.released          = !event->key.down;
			key.repeat            = (event->key.repeat != 0u);
			key.scan_code         = event->key.scancode;
			key.key_code          = event->key.key;
			key.mod               = event->key.mod;
			key.timestamp_seconds = time_s;

			GameEventKeyboard(key);

			break;
		}

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			EventMouse mb {};

			mb.down              = (event->button.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			mb.up                = (event->button.type == SDL_EVENT_MOUSE_BUTTON_UP);
			mb.left              = (event->button.button == SDL_BUTTON_LEFT);
			mb.middle            = (event->button.button == SDL_BUTTON_MIDDLE);
			mb.right             = (event->button.button == SDL_BUTTON_RIGHT);
			mb.x1                = (event->button.button == SDL_BUTTON_X1);
			mb.x2                = (event->button.button == SDL_BUTTON_X2);
			mb.touch             = (event->button.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = event->button.down;
			mb.released          = !event->button.down;
			mb.num_of_clicks     = event->button.clicks;
			mb.wheel             = false;
			mb.x                 = event->button.x;
			mb.y                 = event->button.y;
			mb.motion            = false;
			mb.motion_x          = 0;
			mb.motion_y          = 0;
			mb.timestamp_seconds = time_s;

			GameEventMouse(mb);

			break;
		}

		case SDL_EVENT_MOUSE_WHEEL: {
			EventMouse mb {};

			mb.down              = false;
			mb.up                = false;
			mb.left              = false;
			mb.middle            = false;
			mb.right             = false;
			mb.x1                = false;
			mb.x2                = false;
			mb.touch             = (event->wheel.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = false;
			mb.released          = false;
			mb.num_of_clicks     = 0;
			mb.wheel             = true;
			mb.x                 = event->wheel.x;
			mb.y                 = event->wheel.y;
			mb.motion            = false;
			mb.motion_x          = 0;
			mb.motion_y          = 0;
			mb.timestamp_seconds = time_s;

			GameEventMouse(mb);

			break;
		}

		case SDL_EVENT_MOUSE_MOTION: {
			EventMouse mb {};

			mb.down              = false;
			mb.up                = false;
			mb.left              = ((event->motion.state & SDL_BUTTON_LMASK) != 0u);
			mb.middle            = ((event->motion.state & SDL_BUTTON_MMASK) != 0u);
			mb.right             = ((event->motion.state & SDL_BUTTON_RMASK) != 0u);
			mb.x1                = ((event->motion.state & SDL_BUTTON_X1MASK) != 0u);
			mb.x2                = ((event->motion.state & SDL_BUTTON_X2MASK) != 0u);
			mb.touch             = (event->motion.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = false;
			mb.released          = false;
			mb.num_of_clicks     = 0;
			mb.wheel             = false;
			mb.x                 = event->motion.x;
			mb.y                 = event->motion.y;
			mb.motion            = true;
			mb.motion_x          = event->motion.xrel;
			mb.motion_y          = event->motion.yrel;
			mb.timestamp_seconds = time_s;

			GameEventMouse(mb);

			break;
		}

		case SDL_EVENT_FINGER_MOTION:
		case SDL_EVENT_FINGER_DOWN:
		case SDL_EVENT_FINGER_UP: {
			EventFinger f {};

			f.down              = (event->tfinger.type == SDL_EVENT_FINGER_DOWN);
			f.up                = (event->tfinger.type == SDL_EVENT_FINGER_UP);
			f.motion            = (event->tfinger.type == SDL_EVENT_FINGER_MOTION);
			f.finger_id         = static_cast<int>(event->tfinger.fingerID);
			f.touch_id          = static_cast<int>(event->tfinger.touchID);
			f.x                 = event->tfinger.x;
			f.y                 = event->tfinger.y;
			f.dx                = event->tfinger.dx;
			f.dy                = event->tfinger.dy;
			f.pressure          = event->tfinger.pressure;
			f.timestamp_seconds = time_s;

			GameEventFinger(f);

			break;
		}

		case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
			EventController c {};

			c.id                = event->gaxis.which;
			c.button            = SDL_GAMEPAD_BUTTON_INVALID;
			c.axis_id           = event->gaxis.axis;
			c.axis_value        = event->gaxis.value;
			c.down              = false;
			c.up                = false;
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = true;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			GameEventController(c);

			break;
		}

		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP: {
			EventController c {};

			c.id                = event->gbutton.which;
			c.button            = event->gbutton.button;
			c.axis_id           = SDL_GAMEPAD_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = event->gbutton.down;
			c.up                = !event->gbutton.down;
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = false;
			c.pressed           = event->gbutton.down;
			c.released          = !event->gbutton.down;
			c.timestamp_seconds = time_s;

			GameEventController(c);

			break;
		}

		case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
		case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
		case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
			if (event->gtouchpad.touchpad == 0) {
				Controller::SetTouchPad(event->gtouchpad.which, event->gtouchpad.finger,
				                        event->gtouchpad.type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP,
				                        event->gtouchpad.x, event->gtouchpad.y);
			}
			break;

		case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
			const auto& sensor = event->gsensor;
			if (sensor.sensor == SDL_SENSOR_ACCEL || sensor.sensor == SDL_SENSOR_GYRO) {
				Controller::SetSensor(sensor.which,
				                      sensor.sensor == SDL_SENSOR_ACCEL ? Controller::Sensor::Accel
				                                                        : Controller::Sensor::Gyro,
				                      sensor.data,
				                      sensor.sensor_timestamp != 0
				                          ? sensor.sensor_timestamp / 1000
				                          : sensor.timestamp / 1000);
			}
			break;
		}

		case SDL_EVENT_GAMEPAD_ADDED:
		case SDL_EVENT_GAMEPAD_REMOVED:
		case SDL_EVENT_GAMEPAD_REMAPPED: {
			EventController c {};

			c.id                = event->gdevice.which;
			c.button            = SDL_GAMEPAD_BUTTON_INVALID;
			c.axis_id           = SDL_GAMEPAD_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = false;
			c.up                = false;
			c.added             = (event->gdevice.type == SDL_EVENT_GAMEPAD_ADDED);
			c.removed           = (event->gdevice.type == SDL_EVENT_GAMEPAD_REMOVED);
			c.remapped          = (event->gdevice.type == SDL_EVENT_GAMEPAD_REMAPPED);
			c.axis              = false;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			GameEventController(c);

			break;
		}
	}
}

void WindowContext::Run() {
	Common::Timer timer;
	timer.Start();

	loop.event     = {};
	loop.need_exit = false;
	loop.paused.store(false, std::memory_order_release);

	while (!loop.need_exit) {
		if (loop.paused.load(std::memory_order_acquire)) {
			if (!timer.IsPaused()) {
				timer.Pause();
			}
		} else if (timer.IsPaused()) {
			timer.Resume();
		}

		if (!HostInputWaitEvent(&loop.event)) {
			continue;
		}
		ProcessEvent(timer.GetTimeS());
	}
}

static void WindowCreate(WindowContext& context) {
	EXIT_IF(context.window != nullptr);
	EXIT_IF(context.graphic_ctx.screen_width == 0);
	EXIT_IF(context.graphic_ctx.screen_height == 0);

	int width  = static_cast<int>(context.graphic_ctx.screen_width);
	int height = static_cast<int>(context.graphic_ctx.screen_height);

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	// RenderDoc can hide Wayland Vulkan surface support; prefer X11 for captures.
	if (Config::RenderDocEnabled()) {
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11,wayland");
	}
#endif
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		EXIT("%s\n", SDL_GetError());
	}

	LOGF("WindowCreate(): width = %d, height = %d\n", width, height);

	uint32_t window_flags = WindowContext::InitialWindowFlags(Config::FullscreenEnabled());
#if defined(__APPLE__)
	// SDL loads Vulkan while creating a Vulkan window, so select the bundled
	// MoltenVK loader before calling SDL_CreateWindow. Keep an explicit user
	// override, and fall back to SDL's normal loader search when no bundle is present.
	if (std::getenv("SDL_VULKAN_LIBRARY") == nullptr) {
		if (const char* base_path = SDL_GetBasePath(); base_path != nullptr) {
			const std::string base_path_str = base_path;
			std::string moltenvk_path = base_path_str + "libMoltenVK.dylib";
			if (!Common::File::IsFileExisting(moltenvk_path)) {
				moltenvk_path = base_path_str + "../Frameworks/libMoltenVK.dylib";
			}
			if (Common::File::IsFileExisting(moltenvk_path) &&
			    SDL_setenv_unsafe("SDL_VULKAN_LIBRARY", moltenvk_path.c_str(), 0) == 0) {
				LOGF("Vulkan loader: %s\n", moltenvk_path.c_str());
			}
		}
	}

	// macOS 26 window chrome (CoreUI asset decode, SwiftUI titlebar) has been observed
	// throwing NSExceptions under Rosetta during the first CATransaction commit. A
	// borderless window skips that machinery entirely.
	if (std::getenv("KYTY_BORDERLESS") != nullptr) {
		window_flags |= static_cast<uint32_t>(SDL_WINDOW_BORDERLESS);
	}
#endif
	context.window = SDL_CreateWindow(KYTY_SDL_WINDOW_CAPTION, width, height, window_flags);

	if (context.window == nullptr) {
		EXIT("%s\n", SDL_GetError());
	}

	SDL_SetWindowPosition(context.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	HostInputInit(context.window);
	InitializeSystemOverlayInput(context.window);
	context.UpdateIcon();
}

uint32_t WindowContext::InitialWindowFlags(bool fullscreen) noexcept {
	auto flags = static_cast<uint32_t>(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	if (fullscreen) {
		flags |= static_cast<uint32_t>(SDL_WINDOW_FULLSCREEN);
	}
	return flags;
}

Presenter& WindowInit(uint32_t width, uint32_t height) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	EXIT_IF(g_window != nullptr);

	auto window = std::make_unique<WindowContext>();

	window->graphic_ctx.screen_width  = width;
	window->graphic_ctx.screen_height = height;

	WindowCreate(*window);
	window->CreateVulkan();
	auto& presenter = *window->presenter;
	g_window        = std::move(window);
	return presenter;
}

void WindowRun() {
	KYTY_PROFILER_THREAD("Thread_Window");
	EXIT_IF(g_window == nullptr);

	g_window->Run();
	g_window->render_context->GetPipelineCache().Save();
}

void WindowShutdown() {
	if (g_window != nullptr) {
		HostInputShutdown();
		Controller::EmergencyShutdown();
		g_window.reset();
	}
}

static int WindowIconRead(void* user, char* data, int size) {
	auto*    src        = static_cast<Common::File*>(user);
	uint32_t bytes_read = 0;
	src->Read(data, static_cast<uint32_t>(size), &bytes_read);
	return static_cast<int>(bytes_read);
}

static void WindowIconSkip(void* user, int n) {
	auto*          src      = static_cast<Common::File*>(user);
	const uint64_t position = src->Tell();

	if (n >= 0) {
		src->Seek(position + static_cast<uint64_t>(n));
	} else {
		const uint64_t distance = static_cast<uint64_t>(-static_cast<int64_t>(n));
		EXIT_IF(distance > position);
		src->Seek(position - distance);
	}
}

static int WindowIconEof(void* user) {
	auto* src = static_cast<Common::File*>(user);
	return src->IsEOF() ? 1 : 0;
}

struct WindowIcon {
	SDL_Surface* surface = nullptr;
	void*        pixels  = nullptr;

	~WindowIcon() {
		SDL_DestroySurface(surface);
		stbi_image_free(pixels);
	}
};

static void WindowLoadPngIcon(const std::filesystem::path& path, WindowIcon* icon) {
	Common::File f;
	if (!f.Open(path, Common::File::Mode::Read)) {
		EXIT("Can't open icon file %s\n", Common::PathToString(path).c_str());
	}

	int width  = 0;
	int height = 0;

	stbi_io_callbacks cb {};
	cb.read = WindowIconRead;
	cb.skip = WindowIconSkip;
	cb.eof  = WindowIconEof;

	icon->pixels = stbi_load_from_callbacks(&cb, &f, &width, &height, nullptr, 4);
	f.Close();

	EXIT_IF(icon->pixels == nullptr);

	icon->surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, icon->pixels,
	                                      width * 4);
	EXIT_NOT_IMPLEMENTED(icon->surface == nullptr);
}

void WindowContext::UpdateIcon() {
	static WindowIcon icon;
	static bool       icon_loaded = false;

	if (!icon_loaded) {
		std::filesystem::path icon_path;
		if (Loader::SystemContentGetIconPath(&icon_path)) {
			WindowLoadPngIcon(icon_path, &icon);
		}
		icon_loaded = true;
	}

	if (icon.surface != nullptr) {
		SDL_SetWindowIcon(window, icon.surface);
	}
}

void WindowContext::UpdateTitle() {
	static char title[128];
	static char title_id[12];
	static char app_ver[12];
	static bool has_title = Loader::SystemContentParamSfoGetString("TITLE", title, sizeof(title));
	static bool has_title_id =
	    Loader::SystemContentParamSfoGetString("TITLE_ID", title_id, sizeof(title_id));
	static bool has_app_ver =
	    Loader::SystemContentParamSfoGetString("APP_VER", app_ver, sizeof(app_ver));
	static const std::string processor_name = Common::GetSystemInfo().ProcessorName;
	static uint64_t          fps_start      = Common::Timer::QueryPerformanceCounter();
	static uint64_t          frame_num      = 0;
	static uint64_t          fps_frames     = 0;
	static double            current_fps    = 0.0;

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	static constexpr auto build_type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	static constexpr auto build_type = "Release";
#else
	static constexpr auto build_type = "Unknown";
#endif

	const auto now       = Common::Timer::QueryPerformanceCounter();
	const auto frequency = Common::Timer::QueryPerformanceFrequency();
	frame_num++;
	fps_frames++;
	if (now - fps_start >= frequency) {
		current_fps = static_cast<double>(fps_frames) * static_cast<double>(frequency) /
		              static_cast<double>(now - fps_start);
		fps_start  = now;
		fps_frames = 0;
	}

	if (has_title) {
		PipelineCompileProgress::SetTitleName(title);
	}

	const auto* device_name = graphic_ctx.GetPhysicalDeviceProperties().deviceName.data();
	auto        text        = fmt::format(
        "[{} | {}] {}{}{}{}{}{}[{}] [{}], frame: {}, fps: {:.0f}", KYTY_BUILD_LABEL, build_type,
        (has_title ? title : ""), (has_title ? ", " : ""), (has_title_id ? title_id : ""),
        (has_title_id ? ", " : ""), (has_app_ver ? app_ver : ""), (has_app_ver ? " " : ""),
        device_name, processor_name, frame_num, current_fps);

	struct TitleUpdate {
		SDL_Window*  window;
		std::string* text;
	} update {window, &text};
	EXIT_IF(!SDL_RunOnMainThread(
	    [](void* data) {
		    auto& title = *static_cast<TitleUpdate*>(data);
		    SDL_SetWindowTitle(title.window, title.text->c_str());
	    },
	    &update, true));
}

} // namespace Libs::Graphics
