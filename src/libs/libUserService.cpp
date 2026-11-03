#include "common/abi.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/localization.h"
#include "libs/controller.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstring>
#include <string>

namespace Libs {

LIB_VERSION("UserService", 1, "UserService", 1, 1);

namespace UserService {

struct UserServiceLoginUserIdList {
	int user_id[4];
};

enum UserServiceEventType { UserServiceEventTypeLogin, UserServiceEventTypeLogout };

struct SceUserServiceEvent {
	UserServiceEventType event_type;
	int                  user_id;
};

struct UserServiceGamePresets {
	size_t   this_size;
	uint32_t difficulty;
	uint32_t priority;
	uint32_t invert_vertical_view_for_1st_person_view;
	uint32_t invert_horizontal_view_for_1st_person_view;
	uint32_t invert_vertical_view_for_3rd_person_view;
	uint32_t invert_horizontal_view_for_3rd_person_view;
	uint32_t display_sub_titles;
	uint32_t audio_language;
};

static_assert(sizeof(UserServiceGamePresets) == 40);

static bool IsPlayerLoggedIn(int player) {
	return player == 0 || (player > 0 && Controller::IsPlayerConnected(player));
}

static int GetLoggedInPlayer(int user_id) {
	const int player = Config::GetPlayerFromUserId(user_id);
	return IsPlayerLoggedIn(player) ? player : -1;
}

static KYTY_SYSV_ABI int UserServiceInitialize(const void* /*params*/) {
	PRINT_NAME();

	return OK;
}

static KYTY_SYSV_ABI int UserServiceInitialize2() {
	PRINT_NAME();

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetInitialUser(int* user_id) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(user_id == nullptr);

	*user_id = Config::GetUserId();

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetEvent(SceUserServiceEvent* event) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(event == nullptr);

	static bool logged_in[Config::MAX_PLAYERS] = {};

	for (int player = 0; player < Config::MAX_PLAYERS; player++) {
		const bool connected = IsPlayerLoggedIn(player);
		if (connected != logged_in[player]) {
			logged_in[player] = connected;
			event->event_type = connected ? UserServiceEventTypeLogin : UserServiceEventTypeLogout;
			event->user_id    = Config::GetPlayerUserId(player);
			return OK;
		}
	}

	return USER_SERVICE_ERROR_NO_EVENT;
}

static KYTY_SYSV_ABI int UserServiceGetLoginUserIdList(UserServiceLoginUserIdList* user_id_list) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(user_id_list == nullptr);

	int count = 0;
	for (int player = 0; player < Config::MAX_PLAYERS; player++) {
		if (IsPlayerLoggedIn(player)) {
			user_id_list->user_id[count++] = Config::GetPlayerUserId(player);
		}
	}
	while (count < Config::MAX_PLAYERS) {
		user_id_list->user_id[count++] = -1;
	}

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetUserName(int user_id, char* name, size_t size) {
	if (name == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	const int player = GetLoggedInPlayer(user_id);
	if (player < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	const auto  user_name =
	    player > 0 ? std::string(Common::Localization::Get(Config::GetConsoleLanguage(), "user.guest")) +
	                     " " + std::to_string(player + 1)
	               : Config::GetUserName();
	if (size <= user_name.size()) {
		return USER_SERVICE_ERROR_BUFFER_TOO_SHORT;
	}
	std::memcpy(name, user_name.c_str(), user_name.size() + 1);

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetUserNumber(int user_id, int32_t* number) {
	PRINT_NAME();

	if (number == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	const int player = GetLoggedInPlayer(user_id);
	if (player < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*number = player + 1;

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetGamePresets(int user_id, UserServiceGamePresets* presets) {
	PRINT_NAME();

	if (presets == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	auto this_size = presets->this_size;
	if (this_size == 0 || this_size > sizeof(UserServiceGamePresets)) {
		this_size = sizeof(UserServiceGamePresets);
	}

	std::memset(presets, 0, this_size);
	presets->this_size = sizeof(UserServiceGamePresets);

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityVibration(int user_id, int32_t* vibration) {
	PRINT_NAME();

	if (vibration == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*vibration = 1;

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityTriggerEffect(int      user_id,
                                                                  int32_t* trigger_effect) {
	PRINT_NAME();

	if (trigger_effect == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*trigger_effect = 1;

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAgeLevel(int user_id, uint32_t* age_level) {
	PRINT_NAME();

	if (age_level == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*age_level = 9;

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityChatTranscription(int      user_id,
                                                                      int32_t* chat_transcription) {
	PRINT_NAME();

	if (chat_transcription == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*chat_transcription = 0;

	return OK;
}

static KYTY_SYSV_ABI int
UserServiceGetAccessibilityPressAndHoldDelay(int user_id, int32_t* press_and_hold_delay) {
	PRINT_NAME();

	if (press_and_hold_delay == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*press_and_hold_delay = 0;

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityZoomEnabled(int      user_id,
                                                                int32_t* zoom_enabled) {
	PRINT_NAME();

	if (zoom_enabled == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*zoom_enabled = 0;

	return OK;
}

static KYTY_SYSV_ABI int UserServiceGetAccessibilityZoomFollowFocus(int      user_id,
                                                                    int32_t* zoom_follow_focus) {
	PRINT_NAME();

	if (zoom_follow_focus == nullptr) {
		return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	}
	if (GetLoggedInPlayer(user_id) < 0) {
		return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	}

	*zoom_follow_focus = 0;

	return OK;
}

static KYTY_SYSV_ABI int UserServicePlatformPrivacyWs1Stub(uint64_t, uint64_t, uint64_t, uint64_t,
                                                           uint64_t, uint64_t) {
	return OK;
}

} // namespace UserService

LIB_DEFINE(InitUserService_1) {
	LIB_FUNC("j3YMu1MVNNo", UserService::UserServiceInitialize);
	LIB_FUNC("az-0R6eviZ0", UserService::UserServiceInitialize2);
	LIB_FUNC("CdWp0oHWGr0", UserService::UserServiceGetInitialUser);
	LIB_FUNC("yH17Q6NWtVg", UserService::UserServiceGetEvent);
	LIB_FUNC("fPhymKNvK-A", UserService::UserServiceGetLoginUserIdList);
	LIB_FUNC("1xxcMiGu2fo", UserService::UserServiceGetUserName);
	LIB_FUNC("bwFjS+bX9mA", UserService::UserServiceGetUserNumber);
	LIB_FUNC("qbwy0Ub8b3M", UserService::UserServiceGetUserNumber);
	LIB_FUNC("-sD02mFDBh4", UserService::UserServiceGetGamePresets);
	LIB_FUNC("woNpu+45RLk", UserService::UserServiceGetAgeLevel);
	LIB_FUNC("rnEhHqG-4xo", UserService::UserServiceGetAccessibilityChatTranscription);
	LIB_FUNC("ZKJtxdgvzwg", UserService::UserServiceGetAccessibilityPressAndHoldDelay);
	LIB_FUNC("qWYHOFwqCxY", UserService::UserServiceGetAccessibilityVibration);
	LIB_FUNC("-3Y5GO+-i78", UserService::UserServiceGetAccessibilityTriggerEffect);
	LIB_FUNC("hD-H81EN9Vg", UserService::UserServiceGetAccessibilityZoomEnabled);
	LIB_FUNC("O6IW1-Dwm-w", UserService::UserServiceGetAccessibilityZoomFollowFocus);
	LIB_FUNC("D-CzAxQL0XI", UserService::UserServicePlatformPrivacyWs1Stub);
}

} // namespace Libs
