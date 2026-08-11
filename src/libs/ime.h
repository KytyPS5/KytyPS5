#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_IME_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_IME_H_

#include "libs/imeCommon.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Libs::Ime {

constexpr uint32_t WORK_BUFFER_SIZE = 20 * 1024;
constexpr uint32_t MAX_TEXT_LENGTH  = ImeCommon::MAX_TEXT_LENGTH;

using Type       = ImeCommon::Type;
using EnterLabel = ImeCommon::EnterLabel;
using Alignment  = ImeCommon::Alignment;

struct Event;

using Keycode           = ImeCommon::Keycode;
using TextFilter        = ImeCommon::TextFilter;
using EventHandler      = void(KYTY_SYSV_ABI*)(void* arg, const Event* event);
using ExtKeyboardFilter = ImeCommon::ExtKeyboardFilter;
using Color             = ImeCommon::Color;

struct Caret {
	float    x;
	float    y;
	uint32_t height;
	uint32_t index;
};

struct TextGeometry {
	float    x;
	float    y;
	uint32_t width;
	uint32_t height;
};

struct Rect {
	float    x;
	float    y;
	uint32_t width;
	uint32_t height;
};

enum class TextAreaMode : uint32_t { Disable = 0, Edit = 1, Preedit = 2, Select = 3 };

struct TextAreaProperty {
	TextAreaMode mode;
	uint32_t     index;
	int32_t      length;
};

struct EditText {
	char16_t*        str;
	uint32_t         caret_index;
	uint32_t         area_num;
	TextAreaProperty text_area[4];
};

struct KeyboardResourceIdArray {
	int32_t  user_id;
	uint32_t resource_id[5];
};

union EventParam {
	Rect                    rect;
	EditText                text;
	uint32_t                caret_move;
	Keycode                 keycode;
	KeyboardResourceIdArray resource_id_array;
	uint64_t                reserved[8];
};

struct Event {
	uint32_t   id;
	EventParam param;
};

struct Param {
	int32_t      user_id;
	Type         type;
	uint64_t     supported_languages;
	EnterLabel   enter_label;
	uint32_t     input_method;
	TextFilter   filter;
	uint32_t     option;
	uint32_t     max_text_length;
	char16_t*    input_text_buffer;
	float        posx;
	float        posy;
	Alignment    horizontal_alignment;
	Alignment    vertical_alignment;
	void*        work;
	void*        arg;
	EventHandler handler;
	int8_t       reserved[8];
};

using ExtendedParam = ImeCommon::ExtendedParam;

struct KeyboardParam {
	uint32_t     option;
	int8_t       reserved1[4];
	void*        arg;
	EventHandler handler;
	int8_t       reserved2[8];
};

struct KeyboardInfo {
	int32_t  user_id;
	uint32_t device;
	uint32_t type;
	uint32_t repeat_delay;
	uint32_t repeat_rate;
	uint32_t status;
	int8_t   reserved[12];
};

using ExternalAction = ImeCommon::ExternalAction;
using ExternalInput  = ImeCommon::ExternalInput;

static_assert(sizeof(Caret) == 0x10);
static_assert(sizeof(TextGeometry) == 0x10);
static_assert(sizeof(TextAreaProperty) == 0x0c);
static_assert(sizeof(EditText) == 0x40);
static_assert(sizeof(EventParam) == 0x40);
static_assert(alignof(EventParam) == 0x08);
static_assert(sizeof(Event) == 0x48);
static_assert(offsetof(Event, param) == 0x08);
static_assert(sizeof(Param) == 0x60);
static_assert(offsetof(Param, input_text_buffer) == 0x28);
static_assert(offsetof(Param, work) == 0x40);
static_assert(offsetof(Param, handler) == 0x50);
static_assert(sizeof(ExtendedParam) == 0x88);
static_assert(offsetof(ExtendedParam, additional_dictionary_path) == 0x30);
static_assert(sizeof(KeyboardParam) == 0x20);
static_assert(sizeof(KeyboardResourceIdArray) == 0x18);
static_assert(sizeof(KeyboardInfo) == 0x24);
static_assert(sizeof(Keycode) == 0x20);

using VisualState        = ImeCommon::VisualState;
using HostSnapshot       = ImeCommon::HostSnapshot;
using VisibilityCallback = ImeCommon::VisibilityCallback;

void KYTY_SYSV_ABI ImeParamInit(Param* param);
int KYTY_SYSV_ABI  ImeGetPanelSize(const Param* param, uint32_t* width, uint32_t* height);
int KYTY_SYSV_ABI  ImeOpen(const Param* param, const ExtendedParam* extended);
int KYTY_SYSV_ABI  ImeUpdate(EventHandler handler);
int KYTY_SYSV_ABI  ImeClose();
int KYTY_SYSV_ABI  ImeSetText(const char16_t* text, uint32_t length);
int KYTY_SYSV_ABI  ImeSetCaret(const Caret* caret);
int KYTY_SYSV_ABI  ImeSetTextGeometry(TextAreaMode mode, const TextGeometry* geometry);
int KYTY_SYSV_ABI  ImeKeyboardOpen(int32_t user_id, const KeyboardParam* param);
int KYTY_SYSV_ABI  ImeKeyboardClose(int32_t user_id);
int KYTY_SYSV_ABI  ImeKeyboardGetResourceId(int32_t user_id, KeyboardResourceIdArray* resource_ids);
int KYTY_SYSV_ABI  ImeKeyboardGetInfo(uint32_t resource_id, KeyboardInfo* info);
int KYTY_SYSV_ABI  ImeKeyboardSetMode(int32_t user_id, uint32_t mode);

VisualState GetVisualState() noexcept;
void        SetVisibilityCallback(VisibilityCallback callback) noexcept;
bool        GetHostSnapshot(HostSnapshot* snapshot);
bool        HostInsertText(uint64_t generation, std::u16string_view text);
bool        HostBackspace(uint64_t generation);
bool        HostAccept(uint64_t generation);
bool        HostCancel(uint64_t generation);
bool        HostQueueExternalInput(uint64_t generation, ExternalInput input);

} // namespace Libs::Ime

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_IME_H_
