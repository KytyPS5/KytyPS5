#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_IMEDIALOG_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_IMEDIALOG_H_

#include "libs/imeCommon.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Libs::Dialog::ImeDialog {

constexpr uint32_t IME_DIALOG_MAX_TEXT_LENGTH        = ImeCommon::MAX_TEXT_LENGTH;
constexpr uint32_t IME_DIALOG_MAX_TITLE_LENGTH       = 128;
constexpr uint32_t IME_DIALOG_MAX_PLACEHOLDER_LENGTH = 64;

enum class Status : uint32_t { None = 0, Running = 1, Finished = 2 };
enum class EndStatus : uint32_t { Ok = 0, UserCanceled = 1, Aborted = 2 };
using Type       = ImeCommon::Type;
using EnterLabel = ImeCommon::EnterLabel;
using Alignment  = ImeCommon::Alignment;

enum Option : uint32_t {
	OPTION_MULTILINE            = ImeCommon::OPTION_MULTILINE,
	OPTION_NO_AUTO_CAPITALIZE   = ImeCommon::OPTION_NO_AUTO_CAPITALIZE,
	OPTION_PASSWORD             = ImeCommon::OPTION_PASSWORD,
	OPTION_LANGUAGES_FORCED     = 0x00000008,
	OPTION_EXT_KEYBOARD         = ImeCommon::OPTION_EXT_KEYBOARD,
	OPTION_NO_LEARNING          = 0x00000020,
	OPTION_FIXED_POSITION       = ImeCommon::OPTION_FIXED_POSITION,
	OPTION_DISABLE_COPY_PASTE   = 0x00000080,
	OPTION_DISABLE_RESUME       = 0x00000100,
	OPTION_DISABLE_AUTO_SPACE   = 0x00000200,
	OPTION_DISABLE_POSITION_ADJ = ImeCommon::OPTION_DISABLE_POSITION_ADJUST,
	OPTION_EXPANDED_PREEDIT     = ImeCommon::OPTION_EXPANDED_PREEDIT,
	OPTION_JAPANESE_CAPS_LOCK   = 0x00002000,
	OPTION_USE_OVER_2K          = ImeCommon::OPTION_USE_OVER_2K,
};

enum DisableDevice : uint32_t {
	DISABLE_DEVICE_CONTROLLER   = ImeCommon::DISABLE_DEVICE_CONTROLLER,
	DISABLE_DEVICE_EXT_KEYBOARD = ImeCommon::DISABLE_DEVICE_EXT_KEYBOARD,
	DISABLE_DEVICE_REMOTE_OSK   = 0x00000004,
};

using Color             = ImeCommon::Color;
using Keycode           = ImeCommon::Keycode;
using TextFilter        = ImeCommon::TextFilter;
using ExtKeyboardFilter = ImeCommon::ExtKeyboardFilter;

struct Param {
	int32_t         user_id;
	Type            type;
	uint64_t        supported_languages;
	EnterLabel      enter_label;
	uint32_t        input_method;
	TextFilter      filter;
	uint32_t        option;
	uint32_t        max_text_length;
	char16_t*       input_text_buffer;
	float           posx;
	float           posy;
	Alignment       horizontal_alignment;
	Alignment       vertical_alignment;
	const char16_t* placeholder;
	const char16_t* title;
	int8_t          reserved[16];
};

struct Result {
	EndStatus endstatus;
	int8_t    reserved[12];
};

using ExtendedParam = ImeCommon::ExtendedParam;

struct PositionAndForm {
	uint32_t  type;
	float     posx;
	float     posy;
	Alignment horizontal_alignment;
	Alignment vertical_alignment;
	uint32_t  width;
	uint32_t  height;
};

using ExternalAction = ImeCommon::ExternalAction;
using ExternalInput  = ImeCommon::ExternalInput;

static_assert(sizeof(Param) == 0x60);
static_assert(offsetof(Param, input_text_buffer) == 0x28);
static_assert(offsetof(Param, title) == 0x48);
static_assert(sizeof(Result) == 0x10);
static_assert(sizeof(ExtendedParam) == 0x88);
static_assert(offsetof(ExtendedParam, additional_dictionary_path) == 0x30);
static_assert(sizeof(PositionAndForm) == 0x1c);
static_assert(sizeof(Keycode) == 0x20);

using VisualState        = ImeCommon::VisualState;
using HostSnapshot       = ImeCommon::HostSnapshot;
using VisibilityCallback = ImeCommon::VisibilityCallback;

int KYTY_SYSV_ABI ImeDialogGetPanelSize(const Param* param, uint32_t* width, uint32_t* height);
int KYTY_SYSV_ABI ImeDialogGetPanelSizeExtended(const Param* param, const ExtendedParam* extended,
                                                uint32_t* width, uint32_t* height);
int KYTY_SYSV_ABI ImeDialogInit(const Param* param, const ExtendedParam* extended);
int KYTY_SYSV_ABI ImeDialogGetStatus();
int KYTY_SYSV_ABI ImeDialogAbort();
int KYTY_SYSV_ABI ImeDialogGetResult(Result* result);
int KYTY_SYSV_ABI ImeDialogTerm();
int KYTY_SYSV_ABI ImeDialogGetPanelPositionAndForm(PositionAndForm* form);

VisualState GetVisualState() noexcept;
void        SetVisibilityCallback(VisibilityCallback callback) noexcept;
bool        GetHostSnapshot(HostSnapshot* snapshot);
bool        HostInsertText(uint64_t generation, std::u16string_view text);
bool        HostBackspace(uint64_t generation);
bool        HostMoveCursor(uint64_t generation, int delta);
bool        HostAccept(uint64_t generation);
bool        HostCancel(uint64_t generation);
bool        HostQueueExternalInput(uint64_t generation, ExternalInput input);

} // namespace Libs::Dialog::ImeDialog

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_IMEDIALOG_H_
