#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstring>

namespace Libs {

LIB_VERSION("SystemService", 1, "SystemService", 1, 1);

namespace SystemService {

[[maybe_unused]] constexpr int PARAM_ID_LANG                = 1;
[[maybe_unused]] constexpr int PARAM_ID_DATE_FORMAT         = 2;
[[maybe_unused]] constexpr int PARAM_ID_TIME_FORMAT         = 3;
[[maybe_unused]] constexpr int PARAM_ID_TIME_ZONE           = 4;
[[maybe_unused]] constexpr int PARAM_ID_SUMMERTIME          = 5;
[[maybe_unused]] constexpr int PARAM_ID_SYSTEM_NAME         = 6;
[[maybe_unused]] constexpr int PARAM_ID_GAME_PARENTAL_LEVEL = 7;
[[maybe_unused]] constexpr int PARAM_ID_ENTER_BUTTON_ASSIGN = 1000;

[[maybe_unused]] constexpr int PARAM_LANG_JAPANESE      = 0;
[[maybe_unused]] constexpr int PARAM_LANG_ENGLISH_US    = 1;
[[maybe_unused]] constexpr int PARAM_LANG_FRENCH        = 2;
[[maybe_unused]] constexpr int PARAM_LANG_SPANISH       = 3;
[[maybe_unused]] constexpr int PARAM_LANG_GERMAN        = 4;
[[maybe_unused]] constexpr int PARAM_LANG_ITALIAN       = 5;
[[maybe_unused]] constexpr int PARAM_LANG_DUTCH         = 6;
[[maybe_unused]] constexpr int PARAM_LANG_PORTUGUESE_PT = 7;
[[maybe_unused]] constexpr int PARAM_LANG_RUSSIAN       = 8;
[[maybe_unused]] constexpr int PARAM_LANG_KOREAN        = 9;
[[maybe_unused]] constexpr int PARAM_LANG_CHINESE_T     = 10;
[[maybe_unused]] constexpr int PARAM_LANG_CHINESE_S     = 11;
[[maybe_unused]] constexpr int PARAM_LANG_FINNISH       = 12;
[[maybe_unused]] constexpr int PARAM_LANG_SWEDISH       = 13;
[[maybe_unused]] constexpr int PARAM_LANG_DANISH        = 14;
[[maybe_unused]] constexpr int PARAM_LANG_NORWEGIAN     = 15;
[[maybe_unused]] constexpr int PARAM_LANG_POLISH        = 16;
[[maybe_unused]] constexpr int PARAM_LANG_PORTUGUESE_BR = 17;
[[maybe_unused]] constexpr int PARAM_LANG_ENGLISH_GB    = 18;
[[maybe_unused]] constexpr int PARAM_LANG_TURKISH       = 19;
[[maybe_unused]] constexpr int PARAM_LANG_SPANISH_LA    = 20;
[[maybe_unused]] constexpr int PARAM_LANG_ARABIC        = 21;
[[maybe_unused]] constexpr int PARAM_LANG_FRENCH_CA     = 22;
[[maybe_unused]] constexpr int PARAM_LANG_CZECH         = 23;
[[maybe_unused]] constexpr int PARAM_LANG_HUNGARIAN     = 24;
[[maybe_unused]] constexpr int PARAM_LANG_GREEK         = 25;
[[maybe_unused]] constexpr int PARAM_LANG_ROMANIAN      = 26;
[[maybe_unused]] constexpr int PARAM_LANG_THAI          = 27;
[[maybe_unused]] constexpr int PARAM_LANG_VIETNAMESE    = 28;
[[maybe_unused]] constexpr int PARAM_LANG_INDONESIAN    = 29;

[[maybe_unused]] constexpr int PARAM_DATE_FORMAT_YYYYMMDD = 0;
[[maybe_unused]] constexpr int PARAM_DATE_FORMAT_DDMMYYYY = 1;
[[maybe_unused]] constexpr int PARAM_DATE_FORMAT_MMDDYYYY = 2;

[[maybe_unused]] constexpr int PARAM_TIME_FORMAT_12HOUR = 0;
[[maybe_unused]] constexpr int PARAM_TIME_FORMAT_24HOUR = 1;

[[maybe_unused]] constexpr int MAX_SYSTEM_NAME_LENGTH = 65;

[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_OFF     = 0;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL01 = 1;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL02 = 2;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL03 = 3;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL04 = 4;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL05 = 5;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL06 = 6;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL07 = 7;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL08 = 8;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL09 = 9;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL10 = 10;
[[maybe_unused]] constexpr int PARAM_GAME_PARENTAL_LEVEL11 = 11;

[[maybe_unused]] constexpr int PARAM_ENTER_BUTTON_ASSIGN_CIRCLE = 0;
[[maybe_unused]] constexpr int PARAM_ENTER_BUTTON_ASSIGN_CROSS  = 1;

struct SystemServiceStatus {
	int32_t event_num                  = 0;
	bool    is_system_ui_overlaid      = false;
	bool    is_in_background_execution = false;
	bool    is_vr_play_area_overlaid   = false;
	uint8_t reserved[127]              = {};
};

struct SystemServiceEvent {
	int32_t event_type;
	uint8_t data[8192];
};

struct SystemServiceDisplaySafeAreaInfo {
	float   ratio;
	uint8_t reserved[128];
};

struct SystemServiceHdrToneMapLuminance {
	float max_full_frame_tone_map_luminance;
	float max_tone_map_luminance;
	float min_tone_map_luminance;
};

static int KYTY_SYSV_ABI SystemServiceHideSplashScreen() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI SystemServiceParamGetInt(int param_id, int* value) {
	PRINT_NAME();

	if (value == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	int v = 0;

	switch (param_id) {
		case PARAM_ID_LANG: v = PARAM_LANG_ENGLISH_US; break;
		case PARAM_ID_DATE_FORMAT: v = PARAM_DATE_FORMAT_DDMMYYYY; break;
		case PARAM_ID_TIME_FORMAT: v = PARAM_TIME_FORMAT_24HOUR; break;
		case PARAM_ID_TIME_ZONE: v = +180; break;
		case PARAM_ID_SUMMERTIME: v = 0; break;
		case PARAM_ID_GAME_PARENTAL_LEVEL: v = PARAM_GAME_PARENTAL_OFF; break;
		case PARAM_ID_ENTER_BUTTON_ASSIGN: v = PARAM_ENTER_BUTTON_ASSIGN_CROSS; break;
		default: EXIT("unknown param_id: %d\n", param_id);
	}

	LOGF(" %d = %d\n", param_id, v);

	*value = v;

	return OK;
}

static int KYTY_SYSV_ABI SystemServiceParamGetString(int param_id, char* buf, size_t buf_size) {
	PRINT_NAME();

	if (buf == nullptr || buf_size == 0) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	const char* value = nullptr;
	switch (param_id) {
		case PARAM_ID_SYSTEM_NAME: value = "Kyty"; break;
		default: EXIT("unknown string param_id: %d\n", param_id);
	}

	const auto len = std::strlen(value);
	if (len + 1 > buf_size) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	std::memcpy(buf, value, len + 1);
	LOGF(" %d = %s\n", param_id, value);

	return OK;
}

static int KYTY_SYSV_ABI SystemServiceReceiveEvent(SystemServiceEvent* event) {
	PRINT_NAME();

	if (event == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	event->event_type = -1;
	std::memset(event->data, 0, sizeof(event->data));

	return SYSTEM_SERVICE_ERROR_NO_EVENT;
}

static int KYTY_SYSV_ABI SystemServiceGetStatus(SystemServiceStatus* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	*status = SystemServiceStatus();

	return OK;
}

static int KYTY_SYSV_ABI
SystemServiceGetDisplaySafeAreaInfo(SystemServiceDisplaySafeAreaInfo* info) {
	PRINT_NAME();

	if (info == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	info->ratio = 1.0f;

	return OK;
}

static int KYTY_SYSV_ABI
SystemServiceGetHdrToneMapLuminance(SystemServiceHdrToneMapLuminance* luminance) {
	PRINT_NAME();

	if (luminance == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	luminance->max_full_frame_tone_map_luminance = 80.0f;
	luminance->max_tone_map_luminance            = 1000.0f;
	luminance->min_tone_map_luminance            = 0.0f;

	return OK;
}

static int KYTY_SYSV_ABI SystemServiceGetNoticeScreenSkipFlag(bool* value) {
	PRINT_NAME();

	if (value == nullptr) {
		return SYSTEM_SERVICE_ERROR_PARAMETER;
	}

	*value = false;

	return OK;
}

static int KYTY_SYSV_ABI SystemServiceDisableNoticeScreenSkipFlagAutoSet() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI SystemServiceSetNoticeScreenSkipFlag() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI SystemServicePowerTick() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI SystemServiceReportAbnormalTermination(const void* info) {
	PRINT_NAME();

	LOGF("\t info = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(info));

	return OK;
}

} // namespace SystemService

namespace SystemGesture {

LIB_VERSION("SystemGesture", 1, "SystemGesture", 1, 1);

constexpr int SYSTEM_GESTURE_INPUT_TOUCH_PAD = 0;

constexpr int SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT     = -2138505215; /* 0x80890001 */
constexpr int SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND = -2138505211; /* 0x80890005 */
constexpr int SYSTEM_GESTURE_ERROR_INVALID_HANDLE       = -2138505210; /* 0x80890006 */

constexpr int32_t SYSTEM_GESTURE_HANDLE = 1;

struct SystemGestureVector2 {
	float x;
	float y;
};

struct SystemGesturePrimitiveTouchEvent {
	int32_t              event_state;
	uint16_t             primitive_id;
	uint8_t              is_updated;
	uint8_t              reserved0;
	SystemGestureVector2 pressed_position;
	SystemGestureVector2 current_position;
	SystemGestureVector2 delta_vector;
	uint64_t             delta_time;
	uint64_t             elapsed_time;
	uint8_t              reserve[32];
};

struct SystemGestureRectangle {
	float   x;
	float   y;
	float   width;
	float   height;
	uint8_t reserve[8];
};

struct SystemGestureTouchRecognizer {
	uint64_t reserve[361];
};

struct SystemGestureTouchRecognizerInformation {
	int32_t                gesture_type;
	SystemGestureRectangle rectangle;
	uint64_t               updated_time;
	uint8_t                reserve[256];
};

struct SystemGestureTouchEvent {
	uint8_t reserve[168];
};

static bool SystemGestureIsValidHandle(int32_t gesture_handle) {
	return gesture_handle == SYSTEM_GESTURE_HANDLE;
}

static int KYTY_SYSV_ABI SystemGestureInitializePrimitiveTouchRecognizer(const void*) {
	return OK;
}

static int KYTY_SYSV_ABI SystemGestureFinalizePrimitiveTouchRecognizer() {
	return OK;
}

static int32_t KYTY_SYSV_ABI SystemGestureOpen(int32_t input_type, const void*) {
	if (input_type != SYSTEM_GESTURE_INPUT_TOUCH_PAD) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	return SYSTEM_GESTURE_HANDLE;
}

static int KYTY_SYSV_ABI SystemGestureClose(int32_t gesture_handle) {
	return SystemGestureIsValidHandle(gesture_handle) ? OK : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int KYTY_SYSV_ABI SystemGestureResetPrimitiveTouchRecognizer(int32_t gesture_handle) {
	return SystemGestureIsValidHandle(gesture_handle) ? OK : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int KYTY_SYSV_ABI SystemGestureUpdatePrimitiveTouchRecognizer(int32_t gesture_handle,
                                                                     const void*) {
	return SystemGestureIsValidHandle(gesture_handle) ? OK : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int KYTY_SYSV_ABI SystemGestureGetPrimitiveTouchEvents(
    int32_t gesture_handle, SystemGesturePrimitiveTouchEvent* event_buffer,
    uint32_t capacity_of_buffer, uint32_t* number_of_event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (number_of_event == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	*number_of_event = 0;
	if (event_buffer != nullptr && capacity_of_buffer != 0) {
		std::memset(event_buffer, 0, sizeof(SystemGesturePrimitiveTouchEvent));
	}

	return OK;
}

static int KYTY_SYSV_ABI SystemGestureGetPrimitiveTouchEventsCount(int32_t gesture_handle) {
	return SystemGestureIsValidHandle(gesture_handle) ? 0 : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int KYTY_SYSV_ABI SystemGestureGetPrimitiveTouchEventByIndex(
    int32_t gesture_handle, uint32_t, SystemGesturePrimitiveTouchEvent* event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (event != nullptr) {
		std::memset(event, 0, sizeof(SystemGesturePrimitiveTouchEvent));
	}
	return SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND;
}

static int KYTY_SYSV_ABI SystemGestureGetPrimitiveTouchEventByPrimitiveID(
    int32_t gesture_handle, uint16_t, SystemGesturePrimitiveTouchEvent* event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (event != nullptr) {
		std::memset(event, 0, sizeof(SystemGesturePrimitiveTouchEvent));
	}
	return SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND;
}

static int KYTY_SYSV_ABI
SystemGestureCreateTouchRecognizer(int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer,
                                   int32_t, const SystemGestureRectangle*, const void*) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	std::memset(recognizer, 0, sizeof(SystemGestureTouchRecognizer));

	return OK;
}

static int KYTY_SYSV_ABI SystemGestureAppendTouchRecognizer(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	return (recognizer != nullptr ? OK : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI SystemGestureRemoveTouchRecognizer(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	return (recognizer != nullptr ? OK : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI SystemGestureResetTouchRecognizer(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer != nullptr) {
		std::memset(recognizer, 0, sizeof(SystemGestureTouchRecognizer));
	}
	return (recognizer != nullptr ? OK : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI SystemGestureGetTouchRecognizerInformation(
    int32_t gesture_handle, const SystemGestureTouchRecognizer* recognizer,
    SystemGestureTouchRecognizerInformation* information) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr || information == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	std::memset(information, 0, sizeof(SystemGestureTouchRecognizerInformation));

	return OK;
}

static int KYTY_SYSV_ABI SystemGestureUpdateTouchRecognizer(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	return (recognizer != nullptr ? OK : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI SystemGestureUpdateAllTouchRecognizer(int32_t gesture_handle) {
	return SystemGestureIsValidHandle(gesture_handle) ? OK : SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
}

static int KYTY_SYSV_ABI SystemGestureUpdateTouchRecognizerRectangle(
    int32_t gesture_handle, SystemGestureTouchRecognizer* recognizer,
    const SystemGestureRectangle* rectangle) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr || rectangle == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	return OK;
}

static int KYTY_SYSV_ABI SystemGestureGetTouchEvents(int32_t gesture_handle,
                                                     const SystemGestureTouchRecognizer* recognizer,
                                                     SystemGestureTouchEvent* event_buffer,
                                                     uint32_t                 capacity_of_buffer,
                                                     uint32_t*                number_of_event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr || number_of_event == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}

	*number_of_event = 0;
	if (event_buffer != nullptr && capacity_of_buffer != 0) {
		std::memset(event_buffer, 0, sizeof(SystemGestureTouchEvent));
	}

	return OK;
}

static int KYTY_SYSV_ABI SystemGestureGetTouchEventsCount(
    int32_t gesture_handle, const SystemGestureTouchRecognizer* recognizer) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	return (recognizer != nullptr ? 0 : SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI SystemGestureGetTouchEventByIndex(
    int32_t gesture_handle, const SystemGestureTouchRecognizer* recognizer, uint32_t,
    SystemGestureTouchEvent* event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	if (event != nullptr) {
		std::memset(event, 0, sizeof(SystemGestureTouchEvent));
	}
	return SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND;
}

static int KYTY_SYSV_ABI SystemGestureGetTouchEventByEventID(
    int32_t gesture_handle, const SystemGestureTouchRecognizer* recognizer, uint32_t,
    SystemGestureTouchEvent* event) {
	if (!SystemGestureIsValidHandle(gesture_handle)) {
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (recognizer == nullptr) {
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	if (event != nullptr) {
		std::memset(event, 0, sizeof(SystemGestureTouchEvent));
	}
	return SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND;
}

LIB_DEFINE(InitSystemGesture_1) {
	LIB_FUNC("qpo-mEOwje0", SystemGesture::SystemGestureOpen);
	LIB_FUNC("j4yXIA2jJ68", SystemGesture::SystemGestureClose);
	LIB_FUNC("3pcAvmwKCvM", SystemGesture::SystemGestureInitializePrimitiveTouchRecognizer);
	LIB_FUNC("3QYCmMlOlCY", SystemGesture::SystemGestureFinalizePrimitiveTouchRecognizer);
	LIB_FUNC("o11J529VaAE", SystemGesture::SystemGestureResetPrimitiveTouchRecognizer);
	LIB_FUNC("GgFMb22sbbI", SystemGesture::SystemGestureUpdatePrimitiveTouchRecognizer);
	LIB_FUNC("L8YmemOeSNY", SystemGesture::SystemGestureGetPrimitiveTouchEvents);
	LIB_FUNC("JhwByySf9FY", SystemGesture::SystemGestureGetPrimitiveTouchEventsCount);
	LIB_FUNC("KAeP0+cQPVU", SystemGesture::SystemGestureGetPrimitiveTouchEventByIndex);
	LIB_FUNC("yBaQ0h9m1NM", SystemGesture::SystemGestureGetPrimitiveTouchEventByPrimitiveID);
	LIB_FUNC("FWF8zkhr854", SystemGesture::SystemGestureCreateTouchRecognizer);
	LIB_FUNC("1MMK0W-kMgA", SystemGesture::SystemGestureAppendTouchRecognizer);
	LIB_FUNC("ELvBVG-LKT0", SystemGesture::SystemGestureRemoveTouchRecognizer);
	LIB_FUNC("oBuH3zFWYIg", SystemGesture::SystemGestureResetTouchRecognizer);
	LIB_FUNC("0KrW5eMnrwY", SystemGesture::SystemGestureGetTouchRecognizerInformation);
	LIB_FUNC("j4h82CQWENo", SystemGesture::SystemGestureUpdateTouchRecognizer);
	LIB_FUNC("wPJGwI2RM2I", SystemGesture::SystemGestureUpdateAllTouchRecognizer);
	LIB_FUNC("4WOA1eTx3V8", SystemGesture::SystemGestureUpdateTouchRecognizerRectangle);
	LIB_FUNC("fLTseA7XiWY", SystemGesture::SystemGestureGetTouchEvents);
	LIB_FUNC("h8uongcBNVs", SystemGesture::SystemGestureGetTouchEventsCount);
	LIB_FUNC("TSKvgSz5ChU", SystemGesture::SystemGestureGetTouchEventByIndex);
	LIB_FUNC("lpsXm7tzeoc", SystemGesture::SystemGestureGetTouchEventByEventID);
}

} // namespace SystemGesture

LIB_DEFINE(InitSystemService_1) {
	LIB_FUNC("Vo5V8KAwCmk", SystemService::SystemServiceHideSplashScreen);
	LIB_FUNC("fZo48un7LK4", SystemService::SystemServiceParamGetInt);
	LIB_FUNC("SsC-m-S9JTA", SystemService::SystemServiceParamGetString);
	LIB_FUNC("656LMQSrg6U", SystemService::SystemServiceReceiveEvent);
	LIB_FUNC("rPo6tV8D9bM", SystemService::SystemServiceGetStatus);
	LIB_FUNC("1n37q1Bvc5Y", SystemService::SystemServiceGetDisplaySafeAreaInfo);
	LIB_FUNC("mPpPxv5CZt4", SystemService::SystemServiceGetHdrToneMapLuminance);
	LIB_FUNC("3RQ5aQfnstU", SystemService::SystemServiceGetNoticeScreenSkipFlag);
	LIB_FUNC("8Lo6Zv94aho", SystemService::SystemServiceDisableNoticeScreenSkipFlagAutoSet);
	LIB_FUNC("Q3utJvma4Mo", SystemService::SystemServiceSetNoticeScreenSkipFlag);
	LIB_FUNC("XbbJC3E+L5M", SystemService::SystemServicePowerTick);
	LIB_FUNC("3s8cHiCBKBE", SystemService::SystemServiceReportAbnormalTermination);
	SystemGesture::InitSystemGesture_1(s);
}

} // namespace Libs
