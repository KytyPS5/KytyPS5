#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_IMECOMMON_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_IMECOMMON_H_

#include "common/abi.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Libs::ImeCommon {

constexpr uint32_t MAX_TEXT_LENGTH                = 2048;
constexpr uint32_t OPTION_MULTILINE               = 0x00000001;
constexpr uint32_t OPTION_NO_AUTO_CAPITALIZE      = 0x00000002;
constexpr uint32_t OPTION_PASSWORD                = 0x00000004;
constexpr uint32_t OPTION_EXT_KEYBOARD            = 0x00000010;
constexpr uint32_t OPTION_FIXED_POSITION          = 0x00000040;
constexpr uint32_t OPTION_DISABLE_POSITION_ADJUST = 0x00000800;
constexpr uint32_t OPTION_EXPANDED_PREEDIT        = 0x00001000;
constexpr uint32_t OPTION_USE_OVER_2K             = 0x00004000;
constexpr uint32_t EXT_OPTION_HIDE_KEY_PANEL      = 0x00000400;
constexpr uint32_t DISABLE_DEVICE_CONTROLLER      = 0x00000001;
constexpr uint32_t DISABLE_DEVICE_EXT_KEYBOARD    = 0x00000002;

enum class Type : uint32_t { Default = 0, BasicLatin = 1, Url = 2, Mail = 3, Number = 4 };
enum class EnterLabel : uint32_t { Default = 0, Send = 1, Search = 2, Go = 3 };
enum class Alignment : uint32_t { Start = 0, Center = 1, End = 2 };

struct Color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
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

using TextFilter        = int32_t(KYTY_SYSV_ABI*)(char16_t* out_text, uint32_t* out_text_length,
                                                  const char16_t* source_text,
                                                  uint32_t        source_text_length);
using ExtKeyboardFilter = int(KYTY_SYSV_ABI*)(const Keycode* source_keycode, uint16_t* out_keycode,
                                              uint32_t* out_status, void* reserved);

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
	Keycode        key {};
	ExternalAction action = ExternalAction::None;
	std::u16string text;
};

struct VisualState {
	bool     active;
	uint64_t revision;
};

struct HostSnapshot {
	uint64_t       generation           = 0;
	Type           type                 = Type::Default;
	EnterLabel     enter_label          = EnterLabel::Default;
	uint32_t       option               = 0;
	uint32_t       max_text_length      = 0;
	uint32_t       cursor               = 0;
	uint32_t       disable_device       = 0;
	bool           key_panel_visible    = true;
	float          posx                 = 0.0f;
	float          posy                 = 0.0f;
	Alignment      horizontal_alignment = Alignment::Start;
	Alignment      vertical_alignment   = Alignment::Start;
	uint32_t       panel_width          = 0;
	uint32_t       panel_height         = 0;
	std::u16string text;
	std::u16string title;
	std::u16string placeholder;
};

using VisibilityCallback = void (*)(bool visible, uint64_t generation);

struct EditDelta {
	uint32_t index  = 0;
	int32_t  length = 0;
};

class TextEditEngine {
public:
	void Reset(Type type, uint32_t option, uint32_t max_length, std::u16string text);

	bool Insert(std::u16string_view text, EditDelta* edit = nullptr);
	bool Backspace(EditDelta* edit = nullptr);
	bool MoveCursor(int delta);

	void ReplaceText(std::u16string text, uint32_t cursor);
	void SetCursor(uint32_t cursor);

	[[nodiscard]] Type                  GetType() const { return m_type; }
	[[nodiscard]] uint32_t              GetOption() const { return m_option; }
	[[nodiscard]] uint32_t              GetMaxLength() const { return m_max_length; }
	[[nodiscard]] uint32_t              GetCursor() const { return m_cursor; }
	[[nodiscard]] const std::u16string& GetText() const { return m_text; }

private:
	Type           m_type       = Type::Default;
	uint32_t       m_option     = 0;
	uint32_t       m_max_length = 0;
	uint32_t       m_cursor     = 0;
	std::u16string m_text;
};

[[nodiscard]] bool      IsValidUtf16(std::u16string_view text);
[[nodiscard]] bool      IsValidInputText(std::u16string_view text, bool multiline);
[[nodiscard]] bool      IsAllowedInput(char16_t value, Type type, uint32_t option);
[[nodiscard]] uint32_t  NormalizeCursor(std::u16string_view text, uint32_t cursor);
void                    ClampText(std::u16string* text, uint32_t limit);
[[nodiscard]] EditDelta ComputeEditDelta(std::u16string_view before, std::u16string_view after);
[[nodiscard]] bool      RunTextFilter(TextFilter filter, const std::u16string& source,
                                      uint32_t output_capacity, std::u16string* output);
[[nodiscard]] bool      ApplyKeyboardFilterOutput(ExternalInput* input, uint16_t keycode,
                                                  uint32_t status, bool multiline);

} // namespace Libs::ImeCommon

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_IMECOMMON_H_
