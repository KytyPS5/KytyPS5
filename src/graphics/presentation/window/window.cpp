#include "graphics/presentation/window.h"

#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_hints.h"
#include "SDL_joystick.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_pixels.h"
#include "SDL_rwops.h"
#include "SDL_stdinc.h"
#include "SDL_surface.h"
#include "SDL_thread.h"
#include "SDL_touch.h"
#include "SDL_video.h"
#include "SDL_vulkan.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/windowInternal.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vk_platform.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include "stb_image.h"

#include <fmt/format.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Libs::Graphics {

constexpr float FPS_UPDATE_TIME        = 1.0f;
constexpr int   KEYBOARD_CONTROLLER_ID = -1000;

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

static uint32_t KeyboardKeyToPadButton(int key_code) {
	switch (key_code) {
		case SDLK_w: return Controller::PAD_BUTTON_UP;
		case SDLK_a: return Controller::PAD_BUTTON_LEFT;
		case SDLK_s: return Controller::PAD_BUTTON_DOWN;
		case SDLK_d: return Controller::PAD_BUTTON_RIGHT;
		case SDLK_j: return Controller::PAD_BUTTON_CROSS;
		case SDLK_i: return Controller::PAD_BUTTON_TRIANGLE;
		case SDLK_k: return Controller::PAD_BUTTON_SQUARE;
		case SDLK_l: return Controller::PAD_BUTTON_CIRCLE;
		case SDLK_q: return Controller::PAD_BUTTON_L1;
		case SDLK_e: return Controller::PAD_BUTTON_R1;
		case SDLK_RETURN:
		case SDLK_RETURN2: return Controller::PAD_BUTTON_OPTIONS;
		case SDLK_BACKSPACE:
		case SDLK_TAB: return Controller::PAD_BUTTON_TOUCH_PAD;
		default: return 0;
	}
}

static uint32_t ControllerButtonToPadButton(int button) {
	switch (button) {
		case SDL_CONTROLLER_BUTTON_A: return Controller::PAD_BUTTON_CROSS;
		case SDL_CONTROLLER_BUTTON_B: return Controller::PAD_BUTTON_CIRCLE;
		case SDL_CONTROLLER_BUTTON_X: return Controller::PAD_BUTTON_SQUARE;
		case SDL_CONTROLLER_BUTTON_Y: return Controller::PAD_BUTTON_TRIANGLE;
		case SDL_CONTROLLER_BUTTON_BACK: return Controller::PAD_BUTTON_TOUCH_PAD;
		case SDL_CONTROLLER_BUTTON_START: return Controller::PAD_BUTTON_OPTIONS;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: return Controller::PAD_BUTTON_L3;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return Controller::PAD_BUTTON_R3;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Controller::PAD_BUTTON_L1;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Controller::PAD_BUTTON_R1;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return Controller::PAD_BUTTON_UP;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Controller::PAD_BUTTON_DOWN;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Controller::PAD_BUTTON_LEFT;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Controller::PAD_BUTTON_RIGHT;
		default: return 0;
	}
}

static Controller::Axis ControllerAxisFromSdl(int axis_id) {
	switch (axis_id) {
		case SDL_CONTROLLER_AXIS_LEFTX: return Controller::Axis::LeftX;
		case SDL_CONTROLLER_AXIS_LEFTY: return Controller::Axis::LeftY;
		case SDL_CONTROLLER_AXIS_RIGHTX: return Controller::Axis::RightX;
		case SDL_CONTROLLER_AXIS_RIGHTY: return Controller::Axis::RightY;
		case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return Controller::Axis::TriggerLeft;
		case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return Controller::Axis::TriggerRight;
		default: return Controller::Axis::AxisMax;
	}
}

static bool ControllerAxisIsTrigger(int axis_id) {
	return axis_id == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
	       axis_id == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
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

enum class DisplayOrientation {
	Unknown,   /* The display orientation can't be determined */
	Landscape, /* The display is in landscape mode, with the right side up, relative to portrait
	              mode */
	LandscapeFlipped, /* The display is in landscape mode, with the left side up, relative to
	                     portrait mode */
	Portrait,         /* The display is in portrait mode */
	PortraitFlipped,  /* The display is in portrait mode, upside down */

	DisplayEventOrientation = 0xF0
};

struct EventDisplay {
	DisplayOrientation orientation;
};

constexpr uint32_t KYTY_SDL_BUTTON_LMASK  = SDL_BUTTON_LMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_MMASK  = SDL_BUTTON_MMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_RMASK  = SDL_BUTTON_RMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_X1MASK = SDL_BUTTON_X1MASK; // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_X2MASK = SDL_BUTTON_X2MASK; // NOLINT(hicpp-signed-bitwise)

struct WindowGame {
	void* private_data = nullptr;
	void* event        = nullptr;

	bool     m_game_need_exit        = {false};
	bool     m_game_is_paused        = {false};
	uint32_t m_screen_width          = {0};
	uint32_t m_screen_height         = {0};
	double   m_current_time_seconds  = {0.0};
	double   m_previous_time_seconds = {0.0};
	int      m_update_num            = {0};
	int      m_frame_num             = {0};
	double   m_update_time_seconds   = {0.0};
	double   m_current_fps           = {0.0};
	int      m_max_updates_per_frame = {4};
	double   m_update_fixed_time     = 1.0 / 60.0;
	int      m_fps_frames_num        = {0};
	double   m_fps_start_time        = {0};
};

struct WindowGamePrivate {
	WindowGamePrivate() = default;

	Common::Mutex   mutex;
	int             skip_frames = 0;
	GraphicContext* ctx         = nullptr;
};

WindowContext* g_window_ctx = nullptr;

constexpr const char* KYTY_SDL_WINDOW_CAPTION = "Game";
constexpr uint32_t    KYTY_SDL_WINDOW_FLAGS =
    (static_cast<uint32_t>(SDL_WINDOW_HIDDEN) | static_cast<uint32_t>(SDL_WINDOW_VULKAN));
constexpr int KYTY_SDL_WINDOWPOS_CENTERED = SDL_WINDOWPOS_CENTERED; /*NOLINT(hicpp-signed-bitwise)*/

static void CalcFrameTime(WindowGame* game, double game_time_s) {
	game->m_previous_time_seconds = game->m_current_time_seconds;
	game->m_current_time_seconds  = game_time_s;

	game->m_frame_num++;
	game->m_fps_frames_num++;

	const auto fps_time = game->m_current_time_seconds - game->m_fps_start_time;
	if (fps_time > FPS_UPDATE_TIME) {
		game->m_current_fps    = static_cast<double>(game->m_fps_frames_num) / fps_time;
		game->m_fps_frames_num = 0;
		game->m_fps_start_time = game->m_current_time_seconds;
	}
}

static bool Init(WindowGame* /*game*/) {
	return true;
}
static bool Update(WindowGame* /*game*/) {
	return true;
}
static bool Render(WindowGame* /*game*/) {
	return true;
}
static bool Close(WindowGame* /*game*/) {
	return true;
}
static void SetPause(WindowGame* game, bool flag) {
	LOGF("Pause: %s\n", flag ? "true" : "false");

	game->m_game_is_paused = flag;
}

static bool RenderAndUpdate(WindowGame* game) {
	static double lag = 0.0;

	lag += game->m_current_time_seconds - game->m_previous_time_seconds;

	int num = 0;

	bool ok = true;

	while (lag >= game->m_update_fixed_time) {
		if (num < game->m_max_updates_per_frame) {
			ok = ok && Update(game);

			game->m_update_num++;
			num++;
			game->m_update_time_seconds = game->m_update_num * game->m_update_fixed_time;
		}

		lag -= game->m_update_fixed_time;
	}

	ok = ok && Render(game);

	return ok;
}

bool GameInit(WindowGame* game, const Common::Timer& timer, void* data) {
	EXIT_IF(game == nullptr);
	EXIT_IF(data == nullptr);
	EXIT_IF(game->private_data || game->event);

	auto* ctx = static_cast<GraphicContext*>(data);

	EXIT_IF(ctx->screen_width == 0 || ctx->screen_height == 0);

	auto* pdata = new WindowGamePrivate;
	pdata->ctx  = ctx;

	game->private_data = pdata;
	game->event        = new SDL_Event;

	game->m_screen_width  = ctx->screen_width;
	game->m_screen_height = ctx->screen_height;

	CalcFrameTime(game, timer.GetTimeS());

	return Init(game);
}

bool GameClose(WindowGame* game) {
	EXIT_IF(!game);

	EXIT_IF(!game->private_data || !game->event);

	delete (static_cast<WindowGamePrivate*>(game->private_data));
	delete (static_cast<SDL_Event*>(game->event));

	return Close(game);
}

void GameShowWindow(WindowGame* game, const Common::Timer& timer) {
	EXIT_IF(!game);

	auto* p = static_cast<WindowGamePrivate*>(game->private_data);

	EXIT_IF(!p);

	p->mutex.Lock();
	{
		if (p->skip_frames > 0) {
			p->skip_frames--;
			LOGF("skip frame %d\n", p->skip_frames);
		} else {
			VideoOut::VideoOutBeginVblank();
			if (VideoOut::VideoOutFlipWindow(0)) {
				CalcFrameTime(game, timer.GetTimeS());
			}
			VideoOut::VideoOutEndVblank();
		}
	}
	p->mutex.Unlock();
}

void GameEventQuit(WindowGame* game) {
	LOGF("Event: quit\n");

	game->m_game_need_exit = true;
}

void GameEventTerminate(WindowGame* game) {
	LOGF("Event: terminate\n");

	game->m_game_need_exit = true;
}

void GameEventKeyboard(WindowGame* game, const EventKeyboard* key) {
#ifdef KYTY_DBG_INPUT
	LOGF("Key: time = %.04f, %s%s, %s%s, %s, scan = %d, key = %d, mod = %04" PRIx16 "\n",
	     key->timestamp_seconds, (key->down ? "down" : ""), (key->up ? "up" : ""),
	     (key->pressed ? "pressed" : ""), (key->released ? "released" : ""),
	     (key->repeat ? "repeat" : ""), key->scan_code, key->key_code, key->mod);
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	if (key->down) {
		switch (key->key_code) {
			case SDLK_ESCAPE: game->m_game_need_exit = true; break;
			case SDLK_SPACE: SetPause(game, !game->m_game_is_paused); break;
			case SDLK_F1:
				if (!key->repeat) {
					RenderDocRequestCapture();
				}
				break;
			default: break;
		}
	}

	const auto button = KeyboardKeyToPadButton(key->key_code);
	if (button != 0 && (key->down || key->up) && !key->repeat) {
		static bool keyboard_connected = false;
		if (!keyboard_connected) {
			Controller::ControllerConnect(KEYBOARD_CONTROLLER_ID);
			keyboard_connected = true;
		}
		Controller::ControllerButton(KEYBOARD_CONTROLLER_ID, button, key->down);
	}
#endif
}

void GameEventMouse([[maybe_unused]] WindowGame* game, [[maybe_unused]] const EventMouse* mb) {
#ifdef KYTY_DBG_INPUT
	if (mb->wheel) {
		LOGF("Mouse wheel: time = %.04f, %s[%d, %d]\n", mb->timestamp_seconds,
		     (mb->touch ? "touch, " : ""), mb->x, mb->y);
	} else if (mb->motion) {
		LOGF("Mouse motion: time = %.04f, %s%s%s%s%s%s, [%d, %d], (%d, %d)\n",
		     mb->timestamp_seconds, (mb->left ? "left" : ""), (mb->middle ? "middle" : ""),
		     (mb->right ? "right" : ""), (mb->x1 ? "x1" : ""), (mb->x2 ? "x2" : ""),
		     (mb->touch ? "_touch" : ""), mb->x, mb->y, mb->motion_x, mb->motion_y);
	} else {
		LOGF("Mouse click: time = %.04f, %d, %s%s%s%s%s%s, %s%s, %s%s, [%d, %d]\n",
		     mb->timestamp_seconds, mb->num_of_clicks, (mb->left ? "left" : ""),
		     (mb->middle ? "middle" : ""), (mb->right ? "right" : ""), (mb->x1 ? "x1" : ""),
		     (mb->x2 ? "x2" : ""), (mb->touch ? "_touch" : ""), (mb->down ? "down" : ""),
		     (mb->up ? "up" : ""), (mb->pressed ? "pressed" : ""), (mb->released ? "released" : ""),
		     mb->x, mb->y);
	}
#endif
}

void GameEventFinger([[maybe_unused]] WindowGame* game, [[maybe_unused]] const EventFinger* f) {
#ifdef KYTY_DBG_INPUT
	if (f->motion) {
		LOGF("Finger motion: time = %.04f, %d, %d, (x,y) = [%f, %f], (dx,dy) = [%f, %f], pressure "
		     "= %f\n",
		     f->timestamp_seconds, f->touch_id, f->finger_id, f->x, f->y, f->dx, f->dy,
		     f->pressure);
	} else {
		LOGF("Finger press: time = %.04f, %d, %d, %s%s, (x,y) = [%f, %f], (dx,dy) = [%f, %f], "
		     "pressure = %f\n",
		     f->timestamp_seconds, f->touch_id, f->finger_id, (f->down ? "down" : ""),
		     (f->up ? "up" : ""), f->x, f->y, f->dx, f->dy, f->pressure);
	}
#endif
}

void GameEventController([[maybe_unused]] WindowGame*            game,
                         [[maybe_unused]] const EventController* f) {
	EXIT_NOT_IMPLEMENTED(f->remapped);

#ifdef KYTY_DBG_INPUT
	if (f->added || f->removed) {
		LOGF("Controller %s: %d, time = %.04f\n", (f->added ? "added" : "removed"), f->id,
		     f->timestamp_seconds);
	} else if (f->axis) {
		LOGF("Controller axis: %d, axis = %d, value = %d, time = %.04f\n", f->id, f->axis_id,
		     f->axis_value, f->timestamp_seconds);
	} else {
		LOGF("Controller button: "
		     "%d, %s%s, %s%s, button = %d, time = %.04f\n",
		     f->id, (f->down ? "down" : ""), (f->up ? "up" : ""), (f->pressed ? "pressed" : ""),
		     (f->released ? "released" : ""), f->button, f->timestamp_seconds);
	}
#endif

	if (f->added) {
		auto* pad = SDL_GameControllerOpen(f->id);
		EXIT_NOT_IMPLEMENTED(pad == nullptr);
		int id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
		Controller::ControllerConnect(id);
	}

	if (f->removed) {
		Controller::ControllerDisconnect(f->id);
		SDL_GameControllerClose(SDL_GameControllerFromInstanceID(f->id));
	}

	if (f->down || f->up) {
		const auto button = ControllerButtonToPadButton(f->button);
		if (button != 0) {
			Controller::ControllerButton(f->id, button, f->down);
		}
	}

	if (f->axis) {
		const auto axis = ControllerAxisFromSdl(f->axis_id);
		if (axis != Controller::Axis::AxisMax) {
			Controller::ControllerAxis(f->id, axis,
			                           ControllerAxisValueFromSdl(f->axis_id, f->axis_value));
		}
	}
}

void GameEventDisplay([[maybe_unused]] WindowGame* game, [[maybe_unused]] const EventDisplay* d) {
	auto* p   = static_cast<WindowGamePrivate*>(game->private_data);
	auto* ctx = static_cast<GraphicContext*>(p->ctx);

	p->mutex.Lock();
	game->m_screen_width  = ctx->screen_width;
	game->m_screen_height = ctx->screen_height;
	p->mutex.Unlock();
}

void GameEventLowMemory(WindowGame* /*game*/) {
	LOGF("Event: low_memory\n");
}

void GameEventWillEnterBackground(WindowGame* game) {
	LOGF("Event: will_enter_background\n");

	SetPause(game, true);
}

void GameEventDidEnterBackground(WindowGame* /*game*/) {
	LOGF("Event: did_enter_background\n");
}

void GameEventWillEnterForeground(WindowGame* /*game*/) {
	LOGF("Event: will_enter_foreground\n");
}

void GameEventDidEnterForeground(WindowGame* game) {
	LOGF("Event: did_enter_foreground\n");

	SetPause(game, false);
}

void GameEventResize(WindowGame* game, uint32_t new_width, uint32_t new_height) {
	EXIT_IF(new_width == 0 || new_height == 0);
	EXIT_IF(!game);

	auto* p = static_cast<WindowGamePrivate*>(game->private_data);
	EXIT_IF(p == nullptr);

	auto* ctx = static_cast<GraphicContext*>(p->ctx);
	EXIT_IF(ctx == nullptr);

	p->mutex.Lock();
	{
		p->skip_frames++;
		ctx->screen_width  = new_width;
		ctx->screen_height = new_height;

		game->m_screen_width  = ctx->screen_width;
		game->m_screen_height = ctx->screen_height;
	}
	p->mutex.Unlock();
}

static void ProcessWindowEvent(WindowGame* game, SDL_WindowEvent window) {
	switch (window.event) {
		case SDL_WINDOWEVENT_SHOWN: LOGF("Window %" PRIu32 " shown\n", window.windowID); break;

		case SDL_WINDOWEVENT_HIDDEN: LOGF("Window %" PRIu32 " hidden\n", window.windowID); break;

		case SDL_WINDOWEVENT_EXPOSED: LOGF("Window %" PRIu32 " exposed\n", window.windowID); break;

		case SDL_WINDOWEVENT_MOVED:
			LOGF("Window %" PRIu32 " moved to %" PRId32 ",%" PRId32 "\n", window.windowID,
			     window.data1, window.data2);
			break;

		case SDL_WINDOWEVENT_RESIZED:
			LOGF("Window %" PRIu32 " resized to %" PRId32 "x%" PRId32 "\n", window.windowID,
			     window.data1, window.data2);

			LOGF("m: %d\n", static_cast<int>(SDL_ThreadID()));
			GameEventResize(game, window.data1, window.data2);

			break;

		case SDL_WINDOWEVENT_SIZE_CHANGED:
			LOGF("Window %" PRIu32 " size changed to %" PRId32 "x%" PRId32 "\n", window.windowID,
			     window.data1, window.data2);

			LOGF("m: %d\n", static_cast<int>(SDL_ThreadID()));
			GameEventResize(game, window.data1, window.data2);

			break;

		case SDL_WINDOWEVENT_MINIMIZED:
			LOGF("Window %" PRIu32 " minimized\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_MAXIMIZED:
			LOGF("Window %" PRIu32 " maximized\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_RESTORED:
			LOGF("Window %" PRIu32 " restored\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_ENTER:
			LOGF("Mouse entered window %" PRIu32 "\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_LEAVE: LOGF("Mouse left window %" PRIu32 "\n", window.windowID); break;
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			LOGF("Window %" PRIu32 " gained keyboard focus\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			LOGF("Window %" PRIu32 " lost keyboard focus\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_CLOSE: LOGF("Window %" PRIu32 " closed\n", window.windowID); break;
		default:
			LOGF("Window %" PRIu32 " got unknown event %" PRIu8 "\n", window.windowID,
			     window.event);
			break;
	}
}

static void ProcessDisplayEvent(WindowGame* game, SDL_DisplayEvent display) {
	bool sdl = false;

	switch (display.event) {
		case SDL_DISPLAYEVENT_ORIENTATION: sdl = true; [[fallthrough]];
		case static_cast<Uint8>(DisplayOrientation::DisplayEventOrientation): {
			LOGF("Display %" PRIu32 "[%s] changed orientation to %d - ", display.display,
			     sdl ? "SDL" : "Kyty", static_cast<int>(display.data1));

			EventDisplay d {};
			d.orientation = DisplayOrientation::Unknown;

			switch (display.data1) {
				case SDL_ORIENTATION_UNKNOWN: LOGF("UNKNOWN\n"); break;
				case SDL_ORIENTATION_LANDSCAPE:
					LOGF("LANDSCAPE\n");
					d.orientation = DisplayOrientation::Landscape;
					break;
				case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
					LOGF("LANDSCAPE_FLIPPED\n");
					d.orientation = DisplayOrientation::LandscapeFlipped;
					break;
				case SDL_ORIENTATION_PORTRAIT:
					LOGF("PORTRAIT\n");
					d.orientation = DisplayOrientation::Portrait;
					break;
				case SDL_ORIENTATION_PORTRAIT_FLIPPED:
					d.orientation = DisplayOrientation::PortraitFlipped;
					LOGF("PORTRAIT_FLIPPED\n");
					break;
				default: LOGF("???\n");
			}

			if (!sdl) {
				GameEventDisplay(game, &d);
			}

			break;
		}
		default:
			LOGF("Display %" PRIu32 " got unknown event 0x%" PRIx8 "\n", display.display,
			     display.event);
			break;
	}
}

int GamePollEvent(WindowGame* game) {
	EXIT_IF(!game);

	auto* event = static_cast<SDL_Event*>(game->event);

	EXIT_IF(!event);

	return SDL_PollEvent(event);
}

int GameWaitEvent(WindowGame* game) {
	EXIT_IF(!game);

	auto* event = static_cast<SDL_Event*>(game->event);

	EXIT_IF(!event);

	return SDL_WaitEvent(event);
}

void GameProcessEvent(WindowGame* game, double time_s) {
	EXIT_IF(!game);

	auto* event = static_cast<SDL_Event*>(game->event);

	EXIT_IF(!event);

	EXIT_IF(SDL_GetEventState(SDL_DISPLAYEVENT) != SDL_ENABLE);

	switch (event->type) {
		case SDL_QUIT: GameEventQuit(game); break;

		case SDL_APP_TERMINATING: GameEventTerminate(game); break;

		case SDL_APP_LOWMEMORY: GameEventLowMemory(game); break;

		case SDL_APP_WILLENTERBACKGROUND: GameEventWillEnterBackground(game); break;

		case SDL_APP_DIDENTERBACKGROUND: GameEventDidEnterBackground(game); break;

		case SDL_APP_WILLENTERFOREGROUND: GameEventWillEnterForeground(game); break;

		case SDL_APP_DIDENTERFOREGROUND: GameEventDidEnterForeground(game); break;

		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			EventKeyboard key {};

			key.down              = (event->type == SDL_KEYDOWN);
			key.up                = (event->type == SDL_KEYUP);
			key.pressed           = (event->key.state == SDL_PRESSED);
			key.released          = (event->key.state == SDL_RELEASED);
			key.repeat            = (event->key.repeat != 0u);
			key.scan_code         = event->key.keysym.scancode;
			key.key_code          = event->key.keysym.sym;
			key.mod               = event->key.keysym.mod;
			key.timestamp_seconds = time_s;

			GameEventKeyboard(game, &key);

			break;
		}

		case SDL_WINDOWEVENT: ProcessWindowEvent(game, event->window); break;

		case SDL_DISPLAYEVENT: ProcessDisplayEvent(game, event->display); break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			EventMouse mb {};

			mb.down              = (event->button.type == SDL_MOUSEBUTTONDOWN);
			mb.up                = (event->button.type == SDL_MOUSEBUTTONUP);
			mb.left              = (event->button.button == SDL_BUTTON_LEFT);
			mb.middle            = (event->button.button == SDL_BUTTON_MIDDLE);
			mb.right             = (event->button.button == SDL_BUTTON_RIGHT);
			mb.x1                = (event->button.button == SDL_BUTTON_X1);
			mb.x2                = (event->button.button == SDL_BUTTON_X2);
			mb.touch             = (event->button.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = (event->button.state == SDL_PRESSED);
			mb.released          = (event->button.state == SDL_RELEASED);
			mb.num_of_clicks     = event->button.clicks;
			mb.wheel             = false;
			mb.x                 = event->button.x;
			mb.y                 = event->button.y;
			mb.motion            = false;
			mb.motion_x          = 0;
			mb.motion_y          = 0;
			mb.timestamp_seconds = time_s;

			GameEventMouse(game, &mb);

			break;
		}

		case SDL_MOUSEWHEEL: {
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

			GameEventMouse(game, &mb);

			break;
		}

		case SDL_MOUSEMOTION: {
			EventMouse mb {};

			mb.down              = false;
			mb.up                = false;
			mb.left              = ((event->motion.state & KYTY_SDL_BUTTON_LMASK) != 0u);
			mb.middle            = ((event->motion.state & KYTY_SDL_BUTTON_MMASK) != 0u);
			mb.right             = ((event->motion.state & KYTY_SDL_BUTTON_RMASK) != 0u);
			mb.x1                = ((event->motion.state & KYTY_SDL_BUTTON_X1MASK) != 0u);
			mb.x2                = ((event->motion.state & KYTY_SDL_BUTTON_X2MASK) != 0u);
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

			GameEventMouse(game, &mb);

			break;
		}

		case SDL_FINGERMOTION:
		case SDL_FINGERDOWN:
		case SDL_FINGERUP: {
			EventFinger f {};

			f.down              = (event->tfinger.type == SDL_FINGERDOWN);
			f.up                = (event->tfinger.type == SDL_FINGERUP);
			f.motion            = (event->tfinger.type == SDL_FINGERMOTION);
			f.finger_id         = static_cast<int>(event->tfinger.fingerId);
			f.touch_id          = static_cast<int>(event->tfinger.touchId);
			f.x                 = event->tfinger.x;
			f.y                 = event->tfinger.y;
			f.dx                = event->tfinger.dx;
			f.dy                = event->tfinger.dy;
			f.pressure          = event->tfinger.pressure;
			f.timestamp_seconds = time_s;

			GameEventFinger(game, &f);

			break;
		}

		case SDL_CONTROLLERAXISMOTION: {
			EventController c {};

			c.id                = event->caxis.which;
			c.button            = SDL_CONTROLLER_BUTTON_INVALID;
			c.axis_id           = event->caxis.axis;
			c.axis_value        = event->caxis.value;
			c.down              = false;
			c.up                = false;
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = true;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			GameEventController(game, &c);

			break;
		}

		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP: {
			EventController c {};

			c.id                = event->cbutton.which;
			c.button            = event->cbutton.button;
			c.axis_id           = SDL_CONTROLLER_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = (event->cbutton.type == SDL_CONTROLLERBUTTONDOWN);
			c.up                = (event->cbutton.type == SDL_CONTROLLERBUTTONUP);
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = false;
			c.pressed           = (event->cbutton.state == SDL_PRESSED);
			c.released          = (event->cbutton.state == SDL_RELEASED);
			c.timestamp_seconds = time_s;

			GameEventController(game, &c);

			break;
		}

		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED:
		case SDL_CONTROLLERDEVICEREMAPPED: {
			EventController c {};

			c.id                = event->cdevice.which;
			c.button            = SDL_CONTROLLER_BUTTON_INVALID;
			c.axis_id           = SDL_CONTROLLER_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = false;
			c.up                = false;
			c.added             = (event->cdevice.type == SDL_CONTROLLERDEVICEADDED);
			c.removed           = (event->cdevice.type == SDL_CONTROLLERDEVICEREMOVED);
			c.remapped          = (event->cdevice.type == SDL_CONTROLLERDEVICEREMAPPED);
			c.axis              = false;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			GameEventController(game, &c);

			break;
		}
	}
}

void GameMainLoop(WindowGame* game, void* data) {
	bool need_exit = false;

	Common::Timer timer;
	timer.Start();

	if (!GameInit(game, timer, data)) {
		need_exit = true;
	}

	for (;;) {
		if (need_exit) {
			break;
		}

		if (GamePollEvent(game) != 0) {
			GameProcessEvent(game, timer.GetTimeS());
			continue;
		}

		if (game->m_game_is_paused) {
			if (!timer.IsPaused()) {
				timer.Pause();
			}

			GameWaitEvent(game);

			GameProcessEvent(game, timer.GetTimeS());
			need_exit = game->m_game_need_exit;
			continue;
		}

		need_exit = game->m_game_need_exit;

		if (game->m_game_is_paused) {
			if (!timer.IsPaused()) {
				timer.Pause();
			}
		} else {
			if (timer.IsPaused()) {
				timer.Resume();
			}

			if (!need_exit) {
				need_exit = !RenderAndUpdate(game);
			}

			if (!need_exit) {
				GameShowWindow(game, timer);
			}
		}
	}

	GameClose(game);
}

static void WindowCreate(WindowContext* ctx) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->window != nullptr);
	EXIT_IF(ctx->graphic_ctx.screen_width == 0);
	EXIT_IF(ctx->graphic_ctx.screen_height == 0);

	int width  = static_cast<int>(ctx->graphic_ctx.screen_width);
	int height = static_cast<int>(ctx->graphic_ctx.screen_height);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "0");
#endif

	if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
		EXIT("%s\n", SDL_GetError());
	}

	LOGF("WindowCreate(): width = %d, height = %d\n", width, height);

	ctx->window =
	    SDL_CreateWindow(KYTY_SDL_WINDOW_CAPTION, KYTY_SDL_WINDOWPOS_CENTERED,
	                     KYTY_SDL_WINDOWPOS_CENTERED, width, height, KYTY_SDL_WINDOW_FLAGS);

	ctx->window_hidden = true;

	if (ctx->window == nullptr) {
		EXIT("%s\n", SDL_GetError());
	}

	SDL_SetWindowResizable(ctx->window, SDL_FALSE);
}

void WindowInit(uint32_t width, uint32_t height) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	EXIT_IF(g_window_ctx != nullptr);

	g_window_ctx = new WindowContext;

	g_window_ctx->graphic_ctx.screen_width  = width;
	g_window_ctx->graphic_ctx.screen_height = height;
}

void WindowWaitForGraphicInitialized() {
	EXIT_IF(g_window_ctx == nullptr);

	Common::LockGuard lock(g_window_ctx->mutex);

	while (!g_window_ctx->graphic_initialized) {
		g_window_ctx->graphic_initialized_condvar.Wait(&g_window_ctx->mutex);
	}
}

void WindowRun() {
	EXIT_IF(g_window_ctx == nullptr);

	KYTY_PROFILER_THREAD("Thread_Window");

	WindowGame game;

	g_window_ctx->mutex.Lock();
	{
		EXIT_IF(g_window_ctx->graphic_initialized);

		WindowCreate(g_window_ctx);
		VulkanCreate(g_window_ctx);

		g_window_ctx->game = &game;

		g_window_ctx->graphic_initialized = true;
		g_window_ctx->graphic_initialized_condvar.Signal();
	}
	g_window_ctx->mutex.Unlock();

	GraphicsRenderCreateContext();

	GameMainLoop(&game, &g_window_ctx->graphic_ctx);

	// TODO: replace std::_Exit shutdown with full Vulkan teardown, then destroy
	// the VMA allocator immediately before vkDestroyDevice.
	Common::SubsystemsListSingleton::Instance()->ShutdownAll();
	std::_Exit(0);
}

vk::SurfaceCapabilitiesKHR* VulkanGetSurfaceCapabilities() {
	EXIT_IF(g_window_ctx == nullptr);

	Common::LockGuard lock(g_window_ctx->mutex);

	return &g_window_ctx->surface_capabilities->capabilities;
}

GraphicContext* WindowGetGraphicContext() {
	EXIT_IF(g_window_ctx == nullptr);

	Common::LockGuard lock(g_window_ctx->mutex);

	return &g_window_ctx->graphic_ctx;
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
		SDL_FreeSurface(surface);
		stbi_image_free(pixels);
	}
};

static void WindowLoadPngIcon(const std::string& path, WindowIcon* icon) {
	Common::File f;
	if (!f.Open(path, Common::File::Mode::Read)) {
		EXIT("Can't open icon file %s\n", path.c_str());
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

	icon->surface = SDL_CreateRGBSurfaceWithFormatFrom(icon->pixels, width, height, 32, width * 4,
	                                                   SDL_PIXELFORMAT_RGBA32);
	EXIT_NOT_IMPLEMENTED(icon->surface == nullptr);
}

void WindowUpdateIcon() {
	EXIT_IF(g_window_ctx == nullptr);

	static WindowIcon icon;
	static bool       icon_loaded = false;

	if (!icon_loaded) {
		std::string icon_path;
		if (Loader::SystemContentGetIconPath(&icon_path)) {
			WindowLoadPngIcon(icon_path, &icon);
		}
		icon_loaded = true;
	}

	if (icon.surface != nullptr) {
		SDL_SetWindowIcon(g_window_ctx->window, icon.surface);
	}
}

void WindowUpdateTitle() {
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->game == nullptr);

	static char title[128];
	static char title_id[12];
	static char app_ver[12];
	static bool has_title = Loader::SystemContentParamSfoGetString("TITLE", title, sizeof(title));
	static bool has_title_id =
	    Loader::SystemContentParamSfoGetString("TITLE_ID", title_id, sizeof(title_id));
	static bool has_app_ver =
	    Loader::SystemContentParamSfoGetString("APP_VER", app_ver, sizeof(app_ver));

	auto fps = fmt::format("{}{}{}{}{}{}[{}] [{}], frame: {}, fps: {:f}", (has_title ? title : ""),
	                       (has_title ? ", " : ""), (has_title_id ? title_id : ""),
	                       (has_title_id ? ", " : ""), (has_app_ver ? app_ver : ""),
	                       (has_app_ver ? " " : ""), g_window_ctx->device_name,
	                       g_window_ctx->processor_name, g_window_ctx->game->m_frame_num,
	                       g_window_ctx->game->m_current_fps);

	SDL_SetWindowTitle(g_window_ctx->window, fps.c_str());
}

} // namespace Libs::Graphics
