#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_IMEDIALOG_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_IMEDIALOG_H_

#include "common/abi.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Libs::Dialog::ImeDialog {

constexpr uint32_t IME_DIALOG_MAX_TEXT_LENGTH        = 2048;
constexpr uint32_t IME_DIALOG_MAX_TITLE_LENGTH       = 128;
constexpr uint32_t IME_DIALOG_MAX_PLACEHOLDER_LENGTH = 64;

enum class Status : uint32_t { None = 0, Running = 1, Finished = 2 };
enum class EndStatus : uint32_t { Ok = 0, UserCanceled = 1, Aborted = 2 };
enum class Type : uint32_t { Default = 0, BasicLatin = 1, Url = 2, Mail = 3, Number = 4 };
enum class EnterLabel : uint32_t { Default = 0, Send = 1, Search = 2, Go = 3 };
enum class Alignment : uint32_t { Start = 0, Center = 1, End = 2 };

enum Option : uint32_t {
	OPTION_MULTILINE            = 0x00000001,
	OPTION_NO_AUTO_CAPITALIZE   = 0x00000002,
	OPTION_PASSWORD             = 0x00000004,
	OPTION_LANGUAGES_FORCED     = 0x00000008,
	OPTION_EXT_KEYBOARD         = 0x00000010,
	OPTION_NO_LEARNING          = 0x00000020,
	OPTION_FIXED_POSITION       = 0x00000040,
	OPTION_DISABLE_COPY_PASTE   = 0x00000080,
	OPTION_DISABLE_RESUME       = 0x00000100,
	OPTION_DISABLE_AUTO_SPACE   = 0x00000200,
	OPTION_DISABLE_POSITION_ADJ = 0x00000800,
	OPTION_EXPANDED_PREEDIT     = 0x00001000,
	OPTION_JAPANESE_CAPS_LOCK   = 0x00002000,
	OPTION_USE_OVER_2K          = 0x00004000,
};

enum DisableDevice : uint32_t {
	DISABLE_DEVICE_CONTROLLER   = 0x00000001,
	DISABLE_DEVICE_EXT_KEYBOARD = 0x00000002,
	DISABLE_DEVICE_REMOTE_OSK   = 0x00000004,
};

struct Color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
};

struct Keycode;

using TextFilter        = int32_t(KYTY_SYSV_ABI*)(char16_t* out_text, uint32_t* out_text_length,
                                                  const char16_t* source_text,
                                                  uint32_t        source_text_length);
using ExtKeyboardFilter = int(KYTY_SYSV_ABI*)(const Keycode* source_keycode, uint16_t* out_keycode,
                                              uint32_t* out_status, void* reserved);

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

struct ExtendedParam {
	uint32_t          option;
	Color             color_base;
	Color             color_line;
	Color             color_text_field;
	Color             color_preedit;
	Color             color_button_default;
	Color             color_button_function;
	Color             color_button_symbol;
	Color             color_text;
	Color             color_special;
	uint32_t          priority;
	const char*       additional_dictionary_path;
	ExtKeyboardFilter ext_keyboard_filter;
	uint32_t          disable_device;
	uint32_t          ext_keyboard_mode;
	int8_t            reserved[60];
};

struct PositionAndForm {
	uint32_t  type;
	float     posx;
	float     posy;
	Alignment horizontal_alignment;
	Alignment vertical_alignment;
	uint32_t  width;
	uint32_t  height;
};

struct Keycode {
	uint16_t keycode;
	char16_t character;
	uint32_t status;
	uint32_t type;
	int32_t  user_id;
	uint32_t resource_id;
	uint64_t timestamp;
};

enum class ExternalAction : uint8_t {
	None,
	Text,
	Backspace,
	MoveLeft,
	MoveRight,
	Cancel,
	Accept,
	Newline,
};

struct ExternalInput {
	Keycode        key;
	ExternalAction action;
	std::u16string text;
};

static_assert(sizeof(Param) == 0x60);
static_assert(offsetof(Param, input_text_buffer) == 0x28);
static_assert(offsetof(Param, title) == 0x48);
static_assert(sizeof(Result) == 0x10);
static_assert(sizeof(ExtendedParam) == 0x88);
static_assert(offsetof(ExtendedParam, additional_dictionary_path) == 0x30);
static_assert(sizeof(PositionAndForm) == 0x1c);
static_assert(sizeof(Keycode) == 0x20);

struct VisualState {
	bool     active;
	uint64_t revision;
};

struct HostSnapshot {
	uint64_t       generation;
	Type           type;
	EnterLabel     enter_label;
	uint32_t       option;
	uint32_t       max_text_length;
	uint32_t       cursor;
	uint32_t       disable_device;
	bool           key_panel_visible;
	float          posx;
	float          posy;
	Alignment      horizontal_alignment;
	Alignment      vertical_alignment;
	uint32_t       panel_width;
	uint32_t       panel_height;
	std::u16string text;
	std::u16string title;
	std::u16string placeholder;
};

using VisibilityCallback = void (*)(bool visible, uint64_t generation);

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
