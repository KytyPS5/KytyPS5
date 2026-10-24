#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/presentation/vrInput.h"
#include "kernel/pthread.h"
#include "libs/controller.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/padData.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>

// Virtual headset only: the PS VR2 Sense controller ports. A headset runtime drives them when one
// is running; otherwise each hand reads its half of the first player's keyboard or gamepad.
namespace Libs::Controller {

LIB_NAME("Pad", "Pad");

constexpr int PAD_PORT_TYPE_VR_CONTROLLER_LEFT = 3;

// The buttons of each Sense controller. The left Create button is reserved for the system.
constexpr uint32_t HAND_BUTTONS[2] = {
    PAD_BUTTON_L1 | PAD_BUTTON_L2 | PAD_BUTTON_L3 | PAD_BUTTON_TRIANGLE | PAD_BUTTON_SQUARE,
    PAD_BUTTON_R1 | PAD_BUTTON_R2 | PAD_BUTTON_R3 | PAD_BUTTON_CIRCLE | PAD_BUTTON_CROSS |
        PAD_BUTTON_OPTIONS};

struct PadVrControllerData {
	uint32_t buttons;
	uint8_t  stick_x;
	uint8_t  stick_y;
	uint8_t  stick_padding[2];
	uint8_t  analog_l2;
	uint8_t  analog_r2;
	uint8_t  analog_padding[2];
	uint32_t finger_touch;
	uint8_t  analog_finger_touch[4];
	float    acceleration[3];
	float    angular_velocity[3];
	float    orientation[4];
	uint8_t  reserve1[4];
	bool     connected;
	uint8_t  connected_count;
	uint64_t timestamp;
	uint8_t  reserve2[24];
};

static_assert(sizeof(PadVrControllerData) == 104);
static_assert(offsetof(PadVrControllerData, finger_touch) == 12);
static_assert(offsetof(PadVrControllerData, acceleration) == 20);
static_assert(offsetof(PadVrControllerData, orientation) == 44);
static_assert(offsetof(PadVrControllerData, connected) == 64);
static_assert(offsetof(PadVrControllerData, timestamp) == 72);

static std::atomic<bool> g_open[2];

int PadVrPortHand(int type) {
	const bool vr =
	    type == PAD_PORT_TYPE_VR_CONTROLLER_LEFT || type == PAD_PORT_TYPE_VR_CONTROLLER_LEFT + 1;
	return vr && Config::VrEnabled() ? type - PAD_PORT_TYPE_VR_CONTROLLER_LEFT : -1;
}

int PadVrPort(int user_id, int hand, int index, bool open) {
	if (!PadInitialized()) {
		return PAD_ERROR_NOT_INITIALIZED;
	}
	const int handle = PAD_HANDLE_VR_LEFT + hand;
	if (user_id != Config::GetUserId() || index != 0) {
		return open ? PAD_ERROR_INVALID_ARG : PAD_ERROR_NO_HANDLE;
	}
	if (!open) {
		return g_open[hand].load() ? handle : PAD_ERROR_NO_HANDLE;
	}
	return g_open[hand].exchange(true) ? PAD_ERROR_ALREADY_OPENED : handle;
}

int PadVrCheckHandle(int handle) {
	const int hand = PadVrHand(handle);
	return hand >= 0 && g_open[hand].load() ? OK : PAD_ERROR_INVALID_HANDLE;
}

int PadVrClose(int handle) {
	const int hand = PadVrHand(handle);
	return hand >= 0 && g_open[hand].exchange(false) ? OK : PAD_ERROR_INVALID_HANDLE;
}

static uint8_t ToByte(float value) {
	return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

int KYTY_SYSV_ABI PadVrControllerReadState(int handle, PadVrControllerData* data) {
	PRINT_NAME();

	const int hand = PadVrHand(handle);
	if (hand < 0) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr) {
		return PAD_ERROR_INVALID_ARG;
	}
	PadData pad {};
	if (const auto error = PadReadState(handle, &pad); error != OK) {
		return error;
	}

	*data         = {};
	auto& trigger = hand == 0 ? data->analog_l2 : data->analog_r2;
	data->buttons = pad.buttons & HAND_BUTTONS[hand];
	data->stick_x = hand == 0 ? pad.left_stick_x : pad.right_stick_x;
	data->stick_y = hand == 0 ? pad.left_stick_y : pad.right_stick_y;
	trigger       = hand == 0 ? pad.analog_buttons_l2 : pad.analog_buttons_r2;
	std::memcpy(data->acceleration, &pad.acceleration_x, sizeof(data->acceleration));
	std::memcpy(data->angular_velocity, &pad.angular_velocity_x, sizeof(data->angular_velocity));
	std::memcpy(data->orientation, &pad.orientation_x, sizeof(data->orientation));
	data->connected       = pad.connected;
	data->connected_count = pad.connected_count;
	data->timestamp       = pad.timestamp;

	if (const auto xr = Vr::OpenXrGetController(hand); xr.connected) {
		// Touch controllers have no button left for the right Options, so the left Menu stands in.
		const bool options = hand == 1 && Vr::OpenXrGetController(0).menu;
		// Each mapping names both hands; HAND_BUTTONS keeps this hand's half.
		const uint32_t buttons = (xr.trigger > 0.5f ? PAD_BUTTON_L2 | PAD_BUTTON_R2 : 0u) |
		                         (xr.squeeze > 0.5f ? PAD_BUTTON_L1 | PAD_BUTTON_R1 : 0u) |
		                         (xr.stick_click ? PAD_BUTTON_L3 | PAD_BUTTON_R3 : 0u) |
		                         (xr.primary ? PAD_BUTTON_SQUARE | PAD_BUTTON_CROSS : 0u) |
		                         (xr.secondary ? PAD_BUTTON_TRIANGLE | PAD_BUTTON_CIRCLE : 0u) |
		                         (options ? PAD_BUTTON_OPTIONS : 0u);
		data->buttons          = buttons & HAND_BUTTONS[hand];
		data->stick_x          = ToByte(xr.stick_x * 0.5f + 0.5f);
		data->stick_y          = ToByte(0.5f - xr.stick_y * 0.5f);
		trigger                = ToByte(xr.trigger);
		if (xr.pose.valid) {
			std::memcpy(data->orientation, xr.pose.orientation, sizeof(data->orientation));
		}
		data->connected       = true;
		data->connected_count = std::max<uint8_t>(data->connected_count, 1);
		data->timestamp       = LibKernel::KernelGetProcessTime();
	}
	return OK;
}

int KYTY_SYSV_ABI PadVrControllerSetTriggerEffects(int left_handle, int right_handle,
                                                   const PadTriggerEffectParam* param) {
	PRINT_NAME();
	LOGF("\t left_handle  = %d\n"
	     "\t right_handle = %d\n",
	     left_handle, right_handle);

	if (PadVrHand(left_handle) != 0 || PadVrCheckHandle(left_handle) != OK ||
	    PadVrHand(right_handle) != 1 || PadVrCheckHandle(right_handle) != OK) {
		return PAD_ERROR_INVALID_HANDLE;
	}
	return param != nullptr ? OK : PAD_ERROR_INVALID_ARG;
}

} // namespace Libs::Controller
