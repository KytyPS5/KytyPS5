#include <atomic>
#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/controller.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/padData.h"
#include "loader/symbolDatabase.h"

#include <cstring>

namespace Libs {

LIB_VERSION("Pad", 1, "Pad", 1, 1);

static int KYTY_SYSV_ABI PadSetVibrationMode(int handle, int mode) {
	PRINT_NAME();

	LOGF("\t handle      = %d\n"
	     "\t mode        = %d\n",
	     handle, mode);

	return 0;
}

struct PadTriggerEffectStateInformation {
	int32_t state[2];
};

static int KYTY_SYSV_ABI PadGetTriggerEffectState(int                               handle,
                                                  PadTriggerEffectStateInformation* info) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	if (info == nullptr) {
		return -2137653243; /* 0x80960005 */
	}

	info->state[0] = 0;
	info->state[1] = 0;

	return 0;
}

// Sony sends a mode plus a small parameter block per trigger; a DualSense wants an 11-byte HID
// block (effect type then ten parameters). Modes: 0 off, 1 feedback (position, strength),
// 2 weapon (start, end, strength), 3 vibration (position, amplitude, frequency).
static void trigger_effect_to_hid(uint32_t mode, const uint8_t* data, uint8_t* out) {
	std::memset(out, 0, 11);
	// Sony works in 0-9 positions and 0-8 strengths; a DualSense wants 0-255 for both. Passing the
	// raw value through put a "position 9" effect at 9/255 - resistance from 3.5% of the pull, which
	// feels permanently engaged instead of biting near the end.
	const auto position = [](uint8_t v) {
		const uint8_t clamped = v > 9 ? 9 : v;
		const auto    scaled  = static_cast<uint32_t>((static_cast<float>(clamped) * 255.0f / 9.0f) *
                                                   0.35f);
		return static_cast<uint8_t>(scaled > 255u ? 255u : scaled);
	};
	const auto force = [](uint8_t v) {
		const uint8_t clamped = v > 8 ? 8 : v;
		const auto    scaled =
		    static_cast<uint32_t>((static_cast<float>(clamped) * 255.0f / 8.0f) *
		                          4.0f);
		return static_cast<uint8_t>(scaled > 255u ? 255u : scaled);
	};
	switch (mode) {
		case 1: { // feedback: constant resistance from a position
			if (data[1] == 0) {
				return;
			}
			out[0] = 0x01;
			out[1] = position(data[0]);
			out[2] = force(data[1]);
			break;
		}
		case 2: { // weapon: resistance between two positions
			if (data[2] == 0 || data[1] <= data[0]) {
				return;
			}
			out[0] = 0x02;
			out[1] = position(data[0]);
			out[2] = position(data[1]);
			out[3] = force(data[2]);
			break;
		}
		case 3: { // vibration
			if (data[1] == 0 || data[2] == 0) {
				return;
			}
			out[0] = 0x06;
			out[1] = position(data[0]);
			out[2] = force(data[1]);
			out[3] = data[2];
			break;
		}
		default: break; // off
	}
}

static int KYTY_SYSV_ABI PadSetTriggerEffect(int handle, const void* param) {
	PRINT_NAME();

	LOGF("\t handle = %d\n"
	     "\t param  = 0x%016" PRIx64 "\n",
	     handle, reinterpret_cast<uint64_t>(param));

	if (handle != 1) {
		return -2137915389; /* 0x80920003 */
	}
	if (param == nullptr) {
		return -2137915391; /* 0x80920001 */
	}

	// Log what the title actually asks for: the effect mode and parameter bytes per trigger are
	// what a DualSense HID output report has to carry, and nothing is known about them yet.
	static std::atomic<uint32_t> trig_log {0};
	if (trig_log.fetch_add(1, std::memory_order_relaxed) < 40) {
		const auto* raw = static_cast<const uint8_t*>(param);
		char        hex[3 * 32 + 1] = {};
		for (int k = 0; k < 32; k++) {
			::snprintf(hex + k * 3, 4, "%02x ", raw[k]);
		}
	}
	// mask, then a mode per trigger, then a data block per trigger.
	{
		const auto*   raw      = static_cast<const uint8_t*>(param);
		const uint8_t mask     = raw[0];
		uint32_t      modes[2] = {};
		std::memcpy(&modes[0], raw + 4, sizeof(uint32_t));
		std::memcpy(&modes[1], raw + 8, sizeof(uint32_t));
		uint8_t left[11]  = {};
		uint8_t right[11] = {};
		if ((mask & 0x1u) != 0) {
			trigger_effect_to_hid(modes[0], raw + 12, left);
		}
		if ((mask & 0x2u) != 0) {
			trigger_effect_to_hid(modes[1], raw + 16, right);
		}
		Controller::ControllerTriggerEffect(left, right);
	}

	return OK;
}

static int KYTY_SYSV_ABI PadSetVibrationTriggerEffectWeakWhileEmbeddedMicInUse(bool enabled) {
	PRINT_NAME();

	LOGF("\t enabled = %d\n", enabled ? 1 : 0);

	return OK;
}

struct PadDeviceClassExtendedInformation {
	int32_t device_class;
	uint8_t reserved[4];
	uint8_t class_data[12];
};

struct PadDeviceClassData {
	int32_t device_class;
	bool    data_valid;
	uint8_t reserved[3];
	union {
		struct {
			float    steering_wheel_angle;
			uint16_t steering_wheel;
			uint16_t accelerator_pedal;
			uint16_t brake_pedal;
			uint16_t clutch_pedal;
			uint16_t hand_brake;
			uint8_t  gear;
			uint8_t  reserved[1];
		} steering_wheel;
		struct {
			uint8_t data_len;
			uint8_t reserved[3];
			uint8_t data[12];
		} others;
		uint8_t raw[16];
	} class_data;
};

static_assert(sizeof(PadDeviceClassData) == 24);

static int KYTY_SYSV_ABI
PadDeviceClassGetExtendedInformation(int handle, PadDeviceClassExtendedInformation* info) {
	PRINT_NAME();

	constexpr int pad_error_invalid_handle = -2137915389; /* 0x80920003 */
	constexpr int pad_error_invalid_arg    = -2137915391; /* 0x80920001 */

	if (handle != 1) {
		return pad_error_invalid_handle;
	}
	if (info == nullptr) {
		return pad_error_invalid_arg;
	}

	std::memset(info, 0, sizeof(*info));

	return OK;
}

static int KYTY_SYSV_ABI PadDeviceClassParseData(int handle, const Controller::PadData* data,
                                                 PadDeviceClassData* class_data) {
	PRINT_NAME();

	constexpr int pad_error_invalid_handle = -2137915389; /* 0x80920003 */
	constexpr int pad_error_invalid_arg    = -2137915391; /* 0x80920001 */

	LOGF("\t handle     = %d\n"
	     "\t data       = 0x%016" PRIx64 "\n"
	     "\t class_data = 0x%016" PRIx64 "\n",
	     handle, reinterpret_cast<uint64_t>(data), reinterpret_cast<uint64_t>(class_data));

	if (handle != 1) {
		return pad_error_invalid_handle;
	}
	if (data == nullptr || class_data == nullptr) {
		return pad_error_invalid_arg;
	}

	std::memset(class_data, 0, sizeof(*class_data));
	class_data->device_class = 0; // SCE_PAD_DEVICE_CLASS_STANDARD
	class_data->data_valid   = data->connected;

	if (data->device_unique_data_len > 0) {
		const auto data_len =
		    (data->device_unique_data_len > 12 ? 12 : data->device_unique_data_len);
		class_data->device_class = -1; // SCE_PAD_DEVICE_CLASS_INVALID/unsupported device payload
		class_data->class_data.others.data_len = data_len;
		std::memcpy(class_data->class_data.others.data, data->device_unique_data, data_len);
	}

	return OK;
}

static int KYTY_SYSV_ABI PadSetTiltCorrectionState(int handle, bool enabled) {
	PRINT_NAME();

	LOGF("\t handle  = %d\n"
	     "\t enabled = %d\n",
	     handle, enabled ? 1 : 0);

	return 0;
}

static int KYTY_SYSV_ABI PadClose(int handle) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	return OK;
}

static int KYTY_SYSV_ABI PadUnknownN3kSX62fgNo(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                               uint64_t arg3, uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	LOGF("\t argc-style args = (0x%016" PRIx64 ", 0x%016" PRIx64 ", 0x%016" PRIx64 ", 0x%016" PRIx64
	     ", 0x%016" PRIx64 ", 0x%016" PRIx64 ")\n",
	     arg0, arg1, arg2, arg3, arg4, arg5);

	if (arg0 >= 0x10000 && arg3 > 0 && arg3 <= 0x1000) {
		std::memset(reinterpret_cast<void*>(arg0), 0, static_cast<size_t>(arg3));
	}

	return OK;
}

LIB_DEFINE(InitPad_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("hv1luiJrqQM", Controller::PadInit);
	LIB_FUNC("xk0AcarP3V4", Controller::PadOpen);
	LIB_FUNC("u1GRHp+oWoY", Controller::PadGetHandle);
	LIB_FUNC("AcslpN1jHR8", PadDeviceClassGetExtendedInformation);
	LIB_FUNC("IHPqcbc0zCA", PadDeviceClassParseData);
	LIB_FUNC("clVvL4ZDntw", Controller::PadSetMotionSensorState);
	LIB_FUNC("r44mAxdSG+U", Controller::PadSetAngularVelocityDeadbandState);
	LIB_FUNC("rIZnR6eSpvk", Controller::PadResetOrientation);
	LIB_FUNC("vDLMoJLde8I", PadSetTiltCorrectionState);
	LIB_FUNC("gjP9-KQzoUk", Controller::PadGetControllerInformation);
	LIB_FUNC("2JgFB2n9oUM", PadSetTriggerEffect);
	LIB_FUNC("YndgXqQVV7c", Controller::PadReadState);
	LIB_FUNC("q1cHNfGycLI", Controller::PadRead);
	LIB_FUNC("yFVnOdGxvZY", Controller::PadSetVibration);
	LIB_FUNC("W2G-yoyMF5U", PadSetVibrationMode);
	LIB_FUNC("Yq0zOH7YNOM", PadSetVibrationTriggerEffectWeakWhileEmbeddedMicInUse);
	LIB_FUNC("znaWI0gpuo8", PadGetTriggerEffectState);
	LIB_FUNC("DscD1i9HX1w", Controller::PadResetLightBar);
	LIB_FUNC("RR4novUEENY", Controller::PadSetLightBar);
	LIB_FUNC("6ncge5+l5Qs", PadClose);
	LIB_FUNC("n3kSX62fgNo", PadUnknownN3kSX62fgNo);
}

namespace LibMouse {

LIB_VERSION("Mouse", 1, "Mouse", 1, 1);

namespace Mouse {

constexpr int MOUSE_ERROR_INVALID_ARG    = -2132869119; /* 0x80DF0001 */
constexpr int MOUSE_ERROR_INVALID_HANDLE = -2132869117; /* 0x80DF0003 */

struct MouseData {
	uint64_t timestamp;
	bool     connected;
	uint8_t  padding[3];
	uint32_t buttons;
	int32_t  x_axis;
	int32_t  y_axis;
	int32_t  wheel;
	int32_t  tilt;
	uint8_t  reserved[8];
};

static int KYTY_SYSV_ABI MouseInit() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI MouseOpen(int user_id, int32_t type, int32_t index, const void* param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %" PRId32 "\n"
	     "\t index   = %" PRId32 "\n"
	     "\t param   = 0x%016" PRIx64 "\n",
	     user_id, type, index, reinterpret_cast<uint64_t>(param));

	if (type != 0 || index < 0 || index >= 2) {
		return MOUSE_ERROR_INVALID_ARG;
	}

	return 1;
}

static int KYTY_SYSV_ABI MouseClose(int32_t handle) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n", handle);

	if (handle <= 0) {
		return MOUSE_ERROR_INVALID_HANDLE;
	}

	return OK;
}

static int KYTY_SYSV_ABI MouseRead(int32_t handle, MouseData* data, int32_t num) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n"
	     "\t data   = 0x%016" PRIx64 "\n"
	     "\t num    = %" PRId32 "\n",
	     handle, reinterpret_cast<uint64_t>(data), num);

	if (handle <= 0) {
		return MOUSE_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr || num <= 0) {
		return MOUSE_ERROR_INVALID_ARG;
	}

	std::memset(data, 0, sizeof(MouseData) * static_cast<size_t>(num));

	return 0;
}

} // namespace Mouse

LIB_DEFINE(InitMouse_1) {
	LIB_FUNC("cAnT0Rw-IwU", Mouse::MouseClose);
	LIB_FUNC("Qs0wWulgl7U", Mouse::MouseInit);
	LIB_FUNC("RaqxZIf6DvE", Mouse::MouseOpen);
	LIB_FUNC("x8qnXqh-tiM", Mouse::MouseRead);
}

} // namespace LibMouse

namespace LibKeyboard {

LIB_VERSION("Keyboard", 1, "Keyboard", 1, 1);

namespace Keyboard {

constexpr int KEYBOARD_ERROR_INVALID_ARG    = -2133196799; /* 0x80da0001 */
constexpr int KEYBOARD_ERROR_INVALID_HANDLE = -2133196797; /* 0x80da0003 */
constexpr int KEYBOARD_MAX_KEYCODES         = 16;
constexpr int KEYBOARD_MAX_DATA_NUM         = 16;
constexpr int KEYBOARD_MAPPING_101          = 0;
constexpr int KEYBOARD_MAPPING_106          = 1;

struct KeyboardData {
	uint64_t timestamp;
	bool     intercepted;
	uint8_t  reserve1[7];
	bool     connected;
	int32_t  length;
	uint32_t led;
	uint32_t modifier_key;
	uint16_t key_code[KEYBOARD_MAX_KEYCODES];
	uint8_t  reserve2[32];
};

struct KeyboardCharData {
	bool     processed;
	int32_t  length;
	uint16_t char_code;
	uint8_t  reserve[8];
};

static int KYTY_SYSV_ABI KeyboardInit() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI KeyboardOpen(int user_id, int32_t type, int32_t index, const void* param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %" PRId32 "\n"
	     "\t index   = %" PRId32 "\n"
	     "\t param   = 0x%016" PRIx64 "\n",
	     user_id, type, index, reinterpret_cast<uint64_t>(param));

	if (type != 0 || index < 0 || index >= 2) {
		return KEYBOARD_ERROR_INVALID_ARG;
	}

	return 1;
}

static int KYTY_SYSV_ABI KeyboardClose(int32_t handle) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n", handle);

	if (handle <= 0) {
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}

	return OK;
}

static int KYTY_SYSV_ABI KeyboardRead(int32_t handle, KeyboardData* data, int32_t num) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n"
	     "\t data   = 0x%016" PRIx64 "\n"
	     "\t num    = %" PRId32 "\n",
	     handle, reinterpret_cast<uint64_t>(data), num);

	if (handle <= 0) {
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr || num <= 0 || num > KEYBOARD_MAX_DATA_NUM) {
		return KEYBOARD_ERROR_INVALID_ARG;
	}

	std::memset(data, 0, sizeof(KeyboardData) * static_cast<size_t>(num));

	return 0;
}

static int KYTY_SYSV_ABI KeyboardReadState(int32_t handle, KeyboardData* data) {
	PRINT_NAME();

	LOGF("\t handle = %" PRId32 "\n"
	     "\t data   = 0x%016" PRIx64 "\n",
	     handle, reinterpret_cast<uint64_t>(data));

	if (handle <= 0) {
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr) {
		return KEYBOARD_ERROR_INVALID_ARG;
	}

	std::memset(data, 0, sizeof(KeyboardData));

	return 0;
}

static int KYTY_SYSV_ABI KeyboardGetKey2Char(int32_t handle, int32_t arrange, uint32_t led,
                                             uint32_t modifier_key, uint16_t key_code,
                                             KeyboardCharData* char_data) {
	PRINT_NAME();

	LOGF("\t handle       = %" PRId32 "\n"
	     "\t arrange      = %" PRId32 "\n"
	     "\t led          = %" PRIu32 "\n"
	     "\t modifier_key = %" PRIu32 "\n"
	     "\t key_code     = %" PRIu16 "\n"
	     "\t char_data    = 0x%016" PRIx64 "\n",
	     handle, arrange, led, modifier_key, key_code, reinterpret_cast<uint64_t>(char_data));

	if (handle <= 0) {
		return KEYBOARD_ERROR_INVALID_HANDLE;
	}
	if (char_data == nullptr ||
	    (arrange != KEYBOARD_MAPPING_101 && arrange != KEYBOARD_MAPPING_106)) {
		return KEYBOARD_ERROR_INVALID_ARG;
	}

	std::memset(char_data, 0, sizeof(KeyboardCharData));

	return OK;
}

} // namespace Keyboard

LIB_DEFINE(InitKeyboard_1) {
	LIB_FUNC("wadT3QBCGY0", Keyboard::KeyboardInit);
	LIB_FUNC("HJ+KnEHcaxI", Keyboard::KeyboardOpen);
	LIB_FUNC("0LWei+c7RNc", Keyboard::KeyboardClose);
	LIB_FUNC("6HpE68bzX6M", Keyboard::KeyboardReadState);
	LIB_FUNC("xybbGMCr738", Keyboard::KeyboardRead);
	LIB_FUNC("yO9JwdRhtSA", Keyboard::KeyboardGetKey2Char);
}

} // namespace LibKeyboard

} // namespace Libs
