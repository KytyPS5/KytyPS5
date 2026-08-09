#include "libs/ime.h"

#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Libs::Ime {
namespace {

constexpr int Error(uint32_t value) {
	return static_cast<int32_t>(value);
}

constexpr int ERROR_BUSY                    = Error(0x80bc0001);
constexpr int ERROR_NOT_OPENED              = Error(0x80bc0002);
constexpr int ERROR_CONNECTION_FAILED       = Error(0x80bc0004);
constexpr int ERROR_EVENT_OVERFLOW          = Error(0x80bc0007);
constexpr int ERROR_INVALID_TEXT            = Error(0x80bc0006);
constexpr int ERROR_INVALID_USER_ID         = Error(0x80bc0010);
constexpr int ERROR_INVALID_TYPE            = Error(0x80bc0011);
constexpr int ERROR_INVALID_LANGUAGES       = Error(0x80bc0012);
constexpr int ERROR_INVALID_ENTER_LABEL     = Error(0x80bc0013);
constexpr int ERROR_INVALID_INPUT_METHOD    = Error(0x80bc0014);
constexpr int ERROR_INVALID_OPTION          = Error(0x80bc0015);
constexpr int ERROR_INVALID_MAX_TEXT_LENGTH = Error(0x80bc0016);
constexpr int ERROR_INVALID_TEXT_BUFFER     = Error(0x80bc0017);
constexpr int ERROR_INVALID_POSX            = Error(0x80bc0018);
constexpr int ERROR_INVALID_POSY            = Error(0x80bc0019);
constexpr int ERROR_INVALID_HALIGN          = Error(0x80bc001a);
constexpr int ERROR_INVALID_VALIGN          = Error(0x80bc001b);
constexpr int ERROR_INVALID_EXTENDED        = Error(0x80bc001c);
constexpr int ERROR_INVALID_WORK            = Error(0x80bc0020);
constexpr int ERROR_INVALID_HANDLER         = Error(0x80bc0022);
constexpr int ERROR_NO_RESOURCE_ID          = Error(0x80bc0023);
constexpr int ERROR_INVALID_MODE            = Error(0x80bc0024);
constexpr int ERROR_INVALID_PARAM           = Error(0x80bc0030);
constexpr int ERROR_INVALID_ADDRESS         = Error(0x80bc0031);
constexpr int ERROR_INVALID_RESERVED        = Error(0x80bc0032);

constexpr uint32_t VALID_OPTIONS           = 0x00007bff;
constexpr uint32_t VALID_EXTENDED_OPTIONS  = 0x00004fde;
constexpr uint32_t VALID_EXT_KEYBOARD_MODE = 0x1c000003;
constexpr uint32_t VALID_KEYBOARD_OPTIONS  = 0x0000003f;
constexpr uint32_t VALID_KEYBOARD_MODE     = 0x0000007f;
constexpr uint64_t VALID_LANGUAGES         = 0x00000001ff1fffffULL;

constexpr uint32_t OPTION_MULTILINE        = 0x00000001;
constexpr uint32_t OPTION_PASSWORD         = 0x00000004;
constexpr uint32_t OPTION_EXT_KEYBOARD     = 0x00000010;
constexpr uint32_t OPTION_EXPANDED_PREEDIT = 0x00001000;
constexpr uint32_t OPTION_USE_OVER_2K      = 0x00004000;

constexpr uint32_t EXT_OPTION_HIDE_KEY_PANEL = 0x00000400;
constexpr size_t   EVENT_QUEUE_CAPACITY      = 128;

struct QueuedEvent {
	Event          event {};
	bool           text_payload = false;
	std::u16string text;
};

struct State {
	bool                       open            = false;
	bool                       event_overflow  = false;
	uint64_t                   generation      = 0;
	uint64_t                   revision        = 0;
	uint32_t                   option          = 0;
	uint32_t                   max_text_length = 0;
	uint32_t                   cursor          = 0;
	Param                      param {};
	ExtendedParam              extended {};
	void*                      arg     = nullptr;
	EventHandler               handler = nullptr;
	std::u16string             text;
	std::deque<QueuedEvent>    events;
	std::vector<ExternalInput> external_inputs;
};

struct KeyboardState {
	bool                    open    = false;
	void*                   arg     = nullptr;
	EventHandler            handler = nullptr;
	std::deque<QueuedEvent> events;
};

std::mutex                      g_mutex;
State                           g_state;
KeyboardState                   g_keyboard_state;
std::atomic<bool>               g_open {false};
std::atomic<uint64_t>           g_revision {0};
std::atomic<VisibilityCallback> g_visibility_callback {nullptr};

bool AllZero(const int8_t* data, size_t size) {
	return std::all_of(data, data + size, [](int8_t value) { return value == 0; });
}

bool ValidateText(const char16_t* text, uint32_t length, bool multiline, uint32_t* actual_length) {
	if (text == nullptr) {
		return false;
	}
	*actual_length = 0;
	for (uint32_t i = 0; i < length; i++) {
		const char16_t value = text[i];
		if (value == u'\0') {
			return true;
		}
		if (!multiline && (value == u'\n' || value == u'\r')) {
			return false;
		}
		if (value >= 0xd800 && value <= 0xdbff) {
			if (++i >= length || text[i] < 0xdc00 || text[i] > 0xdfff) {
				return false;
			}
		} else if (value >= 0xdc00 && value <= 0xdfff) {
			return false;
		}
		*actual_length = i + 1;
	}
	return true;
}

bool IsAllowedInput(char16_t value, Type type, uint32_t option) {
	if (value == u'\r' || value == u'\0') {
		return false;
	}
	if (value == u'\n') {
		return (option & OPTION_MULTILINE) != 0;
	}
	if (type == Type::Number) {
		return (value >= u'0' && value <= u'9') || value == u',' || value == u'-' || value == u'.';
	}
	if (type == Type::BasicLatin) {
		return value >= u' ' && value <= u'~';
	}
	return true;
}

char16_t HidCharacter(uint16_t keycode, uint32_t status) {
	const bool shift = (status & 0x00002200) != 0;
	const bool caps  = (status & 0x00020000) != 0;
	if (keycode >= 4 && keycode <= 29) {
		return static_cast<char16_t>(((shift != caps) ? u'A' : u'a') + keycode - 4);
	}
	if (keycode >= 30 && keycode <= 39) {
		static constexpr char16_t plain[]   = u"1234567890";
		static constexpr char16_t shifted[] = u"!@#$%^&*()";
		return (shift ? shifted : plain)[keycode - 30];
	}
	if (keycode == 44) {
		return u' ';
	}
	if (keycode >= 45 && keycode <= 56) {
		static constexpr char16_t plain[]   = u"-=[]\\#;'`,./";
		static constexpr char16_t shifted[] = u"_+{}|~:\"~<>?";
		return (shift ? shifted : plain)[keycode - 45];
	}
	if (keycode >= 89 && keycode <= 98) {
		static constexpr char16_t keypad[] = u"1234567890";
		return keypad[keycode - 89];
	}
	return keycode == 99 ? u'.' : u'\0';
}

bool ApplyKeyboardFilterOutput(ExternalInput* input, uint16_t keycode, uint32_t status,
                               bool multiline) {
	constexpr uint32_t KEYCODE_VALID   = 0x00000001;
	constexpr uint32_t CHARACTER_VALID = 0x00000002;
	if (keycode == input->key.keycode && status == input->key.status) {
		return true;
	}
	if ((status & KEYCODE_VALID) == 0 || keycode == 0) {
		return input->action == ExternalAction::Text && (status & CHARACTER_VALID) != 0;
	}
	input->key.keycode = keycode;
	input->key.status  = status;
	input->text.clear();
	switch (keycode) {
		case 40:
		case 88:
		case 158:
			input->action = multiline ? ExternalAction::Newline : ExternalAction::Accept;
			break;
		case 41: input->action = ExternalAction::Cancel; break;
		case 42:
		case 187: input->action = ExternalAction::Backspace; break;
		case 43:
		case 186: input->action = ExternalAction::None; break;
		case 79: input->action = ExternalAction::MoveRight; break;
		case 80: input->action = ExternalAction::MoveLeft; break;
		default: {
			const char16_t character = HidCharacter(keycode, status);
			if (character == u'\0') {
				return false;
			}
			input->action        = ExternalAction::Text;
			input->key.character = character;
			input->text.push_back(character);
			break;
		}
	}
	return true;
}

uint32_t NormalizeCursor(std::u16string_view text, uint32_t cursor) {
	cursor = std::min<uint32_t>(cursor, static_cast<uint32_t>(text.size()));
	if (cursor > 0 && cursor < text.size() && text[cursor - 1] >= 0xd800 &&
	    text[cursor - 1] <= 0xdbff && text[cursor] >= 0xdc00 && text[cursor] <= 0xdfff) {
		cursor++;
	}
	return cursor;
}

void NotifyVisibility(bool visible, uint64_t generation) {
	if (const auto callback = g_visibility_callback.load(std::memory_order_acquire);
	    callback != nullptr) {
		callback(visible, generation);
	}
}

void UpdateRevisionLocked() {
	g_state.revision++;
	g_revision.store(g_state.revision, std::memory_order_release);
}

void SyncGuestText(char16_t* work, char16_t* input, uint32_t max_length, std::u16string_view text) {
	const size_t size = std::min<size_t>(text.size(), max_length);
	if (work != nullptr) {
		std::memcpy(work, text.data(), size * sizeof(char16_t));
		work[size] = u'\0';
	}
	if (input != nullptr) {
		std::memcpy(input, text.data(), size * sizeof(char16_t));
		input[size] = u'\0';
	}
}

void SyncTextBuffersLocked() {
	SyncGuestText(static_cast<char16_t*>(g_state.param.work), g_state.param.input_text_buffer,
	              g_state.max_text_length, g_state.text);
}

bool QueueEventLocked(QueuedEvent event) {
	if (g_state.events.size() == EVENT_QUEUE_CAPACITY) {
		g_state.event_overflow = true;
		return false;
	}
	g_state.events.push_back(event);
	return true;
}

QueuedEvent MakeTextEventLocked(uint32_t id, uint32_t edit_index, int32_t edit_length) {
	QueuedEvent queued;
	queued.event.id                      = id;
	queued.event.param.text.str          = static_cast<char16_t*>(g_state.param.work);
	queued.event.param.text.caret_index  = g_state.cursor;
	queued.event.param.text.area_num     = 1;
	queued.event.param.text.text_area[0] = {TextAreaMode::Edit, edit_index, edit_length};
	queued.text_payload                  = true;
	queued.text                          = g_state.text;
	return queued;
}

bool CommitFilteredText(uint64_t generation, uint64_t revision, std::u16string old_text,
                        std::u16string candidate, uint32_t cursor, TextFilter filter,
                        uint32_t max_length, bool multiline) {
	if (filter != nullptr) {
		std::vector<char16_t> output(max_length + 1, u'\0');
		uint32_t              output_length = max_length;
		if (filter(output.data(), &output_length, candidate.c_str(),
		           static_cast<uint32_t>(candidate.size())) == 0 &&
		    output_length <= max_length) {
			uint32_t actual_length = 0;
			if (ValidateText(output.data(), output_length, multiline, &actual_length) &&
			    actual_length == output_length) {
				candidate.assign(output.data(), output.data() + output_length);
			}
		}
	}

	std::scoped_lock lock(g_mutex);
	if (!g_state.open || g_state.generation != generation || g_state.revision != revision ||
	    g_state.text != old_text || candidate == old_text) {
		return false;
	}
	size_t prefix = 0;
	while (prefix < old_text.size() && prefix < candidate.size() &&
	       old_text[prefix] == candidate[prefix]) {
		prefix++;
	}
	size_t old_tail = old_text.size();
	size_t new_tail = candidate.size();
	while (old_tail > prefix && new_tail > prefix &&
	       old_text[old_tail - 1] == candidate[new_tail - 1]) {
		old_tail--;
		new_tail--;
	}
	const int32_t edit_length =
	    static_cast<int32_t>(new_tail - prefix) - static_cast<int32_t>(old_tail - prefix);
	g_state.text   = std::move(candidate);
	g_state.cursor = NormalizeCursor(g_state.text, cursor);
	QueueEventLocked(MakeTextEventLocked(1, static_cast<uint32_t>(prefix), edit_length));
	UpdateRevisionLocked();
	return true;
}

void ApplyExternalInputs();

bool ReadInitialText(const Param& param, std::u16string* text) {
	uint32_t length = 0;
	while (length <= param.max_text_length && param.input_text_buffer[length] != u'\0') {
		length++;
	}
	if (length > param.max_text_length) {
		return false;
	}
	uint32_t actual = 0;
	if (!ValidateText(param.input_text_buffer, length, (param.option & OPTION_MULTILINE) != 0,
	                  &actual)) {
		return false;
	}
	text->assign(param.input_text_buffer, param.input_text_buffer + actual);
	return true;
}

bool RangesOverlap(uintptr_t first, size_t first_size, uintptr_t second, size_t second_size) {
	if (first > UINTPTR_MAX - first_size || second > UINTPTR_MAX - second_size) {
		return true;
	}
	return first < second + second_size && second < first + first_size;
}

int ValidateExtended(const ExtendedParam* extended) {
	if (extended == nullptr) {
		return OK;
	}
	if ((extended->option & ~VALID_EXTENDED_OPTIONS) != 0 ||
	    ((extended->option & 0x00004000) != 0 && (extended->option & 0x00000080) == 0) ||
	    extended->priority > 3 || extended->disable_device > 7 ||
	    (extended->ext_keyboard_mode & ~VALID_EXT_KEYBOARD_MODE) != 0 ||
	    !AllZero(extended->reserved, sizeof(extended->reserved))) {
		return ERROR_INVALID_EXTENDED;
	}
	return OK;
}

int ValidateOpen(const Param* param, const ExtendedParam* extended, std::u16string* text) {
	if (param == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	if (param->user_id < 0 || param->user_id == 0xff) {
		return ERROR_INVALID_USER_ID;
	}
	if (static_cast<uint32_t>(param->type) > static_cast<uint32_t>(Type::Number)) {
		return ERROR_INVALID_TYPE;
	}
	if ((param->supported_languages & ~VALID_LANGUAGES) != 0) {
		return ERROR_INVALID_LANGUAGES;
	}
	if (static_cast<uint32_t>(param->enter_label) > static_cast<uint32_t>(EnterLabel::Go)) {
		return ERROR_INVALID_ENTER_LABEL;
	}
	if (param->input_method != 0) {
		return ERROR_INVALID_INPUT_METHOD;
	}
	if ((param->option & ~VALID_OPTIONS) != 0) {
		return ERROR_INVALID_OPTION;
	}
	const bool multiline = (param->option & OPTION_MULTILINE) != 0;
	const bool password  = (param->option & OPTION_PASSWORD) != 0;
	if ((multiline && password) ||
	    (multiline && param->type != Type::Default && param->type != Type::BasicLatin) ||
	    (password && param->type != Type::BasicLatin && param->type != Type::Number)) {
		return ERROR_INVALID_PARAM;
	}
	if (param->max_text_length == 0 || param->max_text_length > MAX_TEXT_LENGTH) {
		return ERROR_INVALID_MAX_TEXT_LENGTH;
	}
	if (param->input_text_buffer == nullptr) {
		return ERROR_INVALID_TEXT_BUFFER;
	}
	const bool  over_2k = (param->option & OPTION_USE_OVER_2K) != 0;
	const float max_x   = over_2k ? 3840.0f : 1920.0f;
	const float max_y   = over_2k ? 2160.0f : 1080.0f;
	if (!std::isfinite(param->posx) || param->posx < 0.0f || param->posx >= max_x) {
		return ERROR_INVALID_POSX;
	}
	if (!std::isfinite(param->posy) || param->posy < 0.0f || param->posy >= max_y) {
		return ERROR_INVALID_POSY;
	}
	if (static_cast<uint32_t>(param->horizontal_alignment) > 2) {
		return ERROR_INVALID_HALIGN;
	}
	if (static_cast<uint32_t>(param->vertical_alignment) > 2) {
		return ERROR_INVALID_VALIGN;
	}
	const int extended_result = ValidateExtended(extended);
	if (extended_result != OK) {
		return extended_result;
	}
	if (param->work == nullptr || (reinterpret_cast<uintptr_t>(param->work) & 3u) != 0) {
		return ERROR_INVALID_WORK;
	}
	if (param->handler == nullptr) {
		return ERROR_INVALID_HANDLER;
	}
	if (!AllZero(param->reserved, sizeof(param->reserved))) {
		return ERROR_INVALID_RESERVED;
	}
	const size_t text_size =
	    static_cast<size_t>(param->max_text_length +
	                        ((param->option & OPTION_EXPANDED_PREEDIT) != 0 ? 0x79u : 0x1fu)) *
	    sizeof(char16_t);
	if (RangesOverlap(reinterpret_cast<uintptr_t>(param->input_text_buffer), text_size,
	                  reinterpret_cast<uintptr_t>(param->work), WORK_BUFFER_SIZE)) {
		return ERROR_INVALID_PARAM;
	}
	return ReadInitialText(*param, text) ? OK : ERROR_INVALID_TEXT;
}

} // namespace

void KYTY_SYSV_ABI ImeParamInit(Param* param) {
	if (param == nullptr) {
		return;
	}
	std::memset(param, 0, sizeof(*param));
	param->user_id = -1;
}

int KYTY_SYSV_ABI ImeGetPanelSize(const Param* param, uint32_t* width, uint32_t* height) {
	if (param == nullptr || width == nullptr || height == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	if (static_cast<uint32_t>(param->type) > static_cast<uint32_t>(Type::Number)) {
		return ERROR_INVALID_TYPE;
	}
	if ((param->option & ~VALID_OPTIONS) != 0) {
		return ERROR_INVALID_OPTION;
	}
	*width  = param->type == Type::Number ? 370 : 793;
	*height = param->type == Type::Number ? 402 : 408;
	if ((param->option & OPTION_USE_OVER_2K) != 0) {
		*width *= 2;
		*height *= 2;
	}
	return OK;
}

int KYTY_SYSV_ABI ImeOpen(const Param* param, const ExtendedParam* extended) {
	uint64_t generation = 0;
	{
		std::scoped_lock lock(g_mutex);
		if (g_state.open) {
			return ERROR_BUSY;
		}
		std::u16string text;
		const int      validation = ValidateOpen(param, extended, &text);
		if (validation != OK) {
			return validation;
		}
		const uint64_t next_generation = g_state.generation + 1;
		const uint64_t next_revision   = g_state.revision + 1;
		g_state                        = {};
		g_state.open                   = true;
		g_state.generation             = next_generation;
		g_state.revision               = next_revision;
		g_state.option                 = param->option;
		g_state.max_text_length        = param->max_text_length;
		g_state.cursor                 = static_cast<uint32_t>(text.size());
		g_state.param                  = *param;
		if (extended != nullptr) {
			g_state.extended = *extended;
		}
		g_state.arg     = param->arg;
		g_state.handler = param->handler;
		g_state.text    = std::move(text);
		std::memset(param->work, 0, WORK_BUFFER_SIZE);
		SyncTextBuffersLocked();

		QueuedEvent open;
		open.event.id                = 0;
		open.event.param.rect.x      = param->posx;
		open.event.param.rect.y      = param->posy;
		open.event.param.rect.width  = param->type == Type::Number ? 370 : 793;
		open.event.param.rect.height = param->type == Type::Number ? 402 : 408;
		if ((param->option & OPTION_USE_OVER_2K) != 0) {
			open.event.param.rect.width *= 2;
			open.event.param.rect.height *= 2;
		}
		QueueEventLocked(std::move(open));
		generation = g_state.generation;
		g_open.store(true, std::memory_order_release);
		g_revision.store(g_state.revision, std::memory_order_release);
	}
	NotifyVisibility(true, generation);
	return OK;
}

int KYTY_SYSV_ABI ImeUpdate(EventHandler handler) {
	bool apply_inputs = false;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_state.open && !g_keyboard_state.open) {
			return ERROR_NOT_OPENED;
		}
		const bool update_ime      = g_state.open && handler == g_state.handler;
		const bool update_keyboard = g_keyboard_state.open && handler == g_keyboard_state.handler;
		if (!update_ime && !update_keyboard) {
			return ERROR_NOT_OPENED;
		}
		apply_inputs = update_ime;
	}
	if (apply_inputs) {
		ApplyExternalInputs();
	}
	struct Dispatch {
		void*       arg;
		QueuedEvent queued;
		char16_t*   work;
		char16_t*   input;
		uint32_t    max_length;
		uint64_t    generation;
	};
	std::vector<Dispatch> dispatch;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_state.open && !g_keyboard_state.open) {
			return ERROR_NOT_OPENED;
		}
		const bool update_ime      = g_state.open && handler == g_state.handler;
		const bool update_keyboard = g_keyboard_state.open && handler == g_keyboard_state.handler;
		if (!update_ime && !update_keyboard) {
			return ERROR_NOT_OPENED;
		}
		if (update_ime && std::exchange(g_state.event_overflow, false)) {
			return ERROR_EVENT_OVERFLOW;
		}
		if (update_ime) {
			while (!g_state.events.empty()) {
				dispatch.push_back({g_state.arg, std::move(g_state.events.front()),
				                    static_cast<char16_t*>(g_state.param.work),
				                    g_state.param.input_text_buffer, g_state.max_text_length,
				                    g_state.generation});
				g_state.events.pop_front();
			}
		}
		if (update_keyboard) {
			while (!g_keyboard_state.events.empty()) {
				dispatch.push_back({g_keyboard_state.arg,
				                    std::move(g_keyboard_state.events.front()), nullptr, nullptr, 0,
				                    0});
				g_keyboard_state.events.pop_front();
			}
		}
	}
	for (auto& item: dispatch) {
		if (item.generation != 0) {
			std::scoped_lock lock(g_mutex);
			if (!g_state.open || g_state.generation != item.generation) {
				continue;
			}
		}
		if (item.queued.text_payload) {
			SyncGuestText(item.work, item.input, item.max_length, item.queued.text);
			item.queued.event.param.text.str = item.work;
		}
		handler(item.arg, &item.queued.event);
	}
	return OK;
}

int KYTY_SYSV_ABI ImeClose() {
	uint64_t generation = 0;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_state.open) {
			return ERROR_NOT_OPENED;
		}
		generation              = g_state.generation;
		const uint64_t revision = g_state.revision + 1;
		g_state                 = {};
		g_state.generation      = generation;
		g_state.revision        = revision;
		g_open.store(false, std::memory_order_release);
		g_revision.store(revision, std::memory_order_release);
	}
	NotifyVisibility(false, generation);
	return OK;
}

int KYTY_SYSV_ABI ImeSetText(const char16_t* text, uint32_t length) {
	std::scoped_lock lock(g_mutex);
	if (!g_state.open) {
		return ERROR_NOT_OPENED;
	}
	if (text == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	const uint32_t clamped_length = std::min(length, g_state.max_text_length);
	uint32_t       actual_length  = 0;
	if (!ValidateText(text, clamped_length, (g_state.option & OPTION_MULTILINE) != 0,
	                  &actual_length)) {
		return ERROR_INVALID_TEXT;
	}
	g_state.text.assign(text, text + actual_length);
	g_state.cursor = NormalizeCursor(g_state.text, g_state.cursor);
	SyncTextBuffersLocked();
	UpdateRevisionLocked();
	return OK;
}

int KYTY_SYSV_ABI ImeSetCaret(const Caret* caret) {
	std::scoped_lock lock(g_mutex);
	if (!g_state.open) {
		return ERROR_NOT_OPENED;
	}
	if (caret == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	if (caret->index > g_state.text.size()) {
		return ERROR_INVALID_PARAM;
	}
	g_state.cursor = NormalizeCursor(g_state.text, caret->index);
	UpdateRevisionLocked();
	return OK;
}

int KYTY_SYSV_ABI ImeSetTextGeometry(TextAreaMode mode, const TextGeometry* geometry) {
	std::scoped_lock lock(g_mutex);
	if (!g_state.open) {
		return ERROR_NOT_OPENED;
	}
	if (geometry == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	const bool  over_2k = (g_state.option & OPTION_USE_OVER_2K) != 0;
	const float max_x   = over_2k ? 3840.0f : 1920.0f;
	const float max_y   = over_2k ? 2160.0f : 1080.0f;
	if (!std::isfinite(geometry->x) || !std::isfinite(geometry->y) || geometry->x < 0.0f ||
	    geometry->x >= max_x || geometry->y < 0.0f || geometry->y >= max_y ||
	    (mode != TextAreaMode::Preedit && mode != TextAreaMode::Select)) {
		return ERROR_INVALID_PARAM;
	}
	return OK;
}

int KYTY_SYSV_ABI ImeKeyboardOpen(int32_t user_id, const KeyboardParam* param) {
	std::scoped_lock lock(g_mutex);
	if (param == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	if (user_id == -1) {
		return ERROR_INVALID_USER_ID;
	}
	if (param->handler == nullptr) {
		return ERROR_INVALID_HANDLER;
	}
	if ((param->option & ~VALID_KEYBOARD_OPTIONS) != 0) {
		return ERROR_INVALID_OPTION;
	}
	if (!AllZero(param->reserved1, sizeof(param->reserved1)) ||
	    !AllZero(param->reserved2, sizeof(param->reserved2))) {
		return ERROR_INVALID_RESERVED;
	}
	if (g_keyboard_state.open) {
		return ERROR_BUSY;
	}
	g_keyboard_state         = {};
	g_keyboard_state.open    = true;
	g_keyboard_state.arg     = param->arg;
	g_keyboard_state.handler = param->handler;
	QueuedEvent open;
	open.event.id                              = 256;
	open.event.param.resource_id_array.user_id = user_id;
	g_keyboard_state.events.push_back(std::move(open));
	return OK;
}

int KYTY_SYSV_ABI ImeKeyboardClose(int32_t user_id) {
	std::scoped_lock lock(g_mutex);
	if (!g_keyboard_state.open) {
		return ERROR_NOT_OPENED;
	}
	if (user_id == -1) {
		return ERROR_INVALID_USER_ID;
	}
	g_keyboard_state = {};
	return OK;
}

int KYTY_SYSV_ABI ImeKeyboardGetResourceId(int32_t user_id, KeyboardResourceIdArray* resource_ids) {
	if (resource_ids == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	if (user_id == -1) {
		return ERROR_INVALID_USER_ID;
	}
	std::memset(resource_ids, 0, sizeof(*resource_ids));
	resource_ids->user_id = user_id;
	std::scoped_lock lock(g_mutex);
	return g_keyboard_state.open ? ERROR_CONNECTION_FAILED : ERROR_NOT_OPENED;
}

int KYTY_SYSV_ABI ImeKeyboardGetInfo(uint32_t resource_id, KeyboardInfo* info) {
	if (info == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	std::scoped_lock lock(g_mutex);
	if (!g_keyboard_state.open) {
		return ERROR_NOT_OPENED;
	}
	(void)resource_id;
	return ERROR_NO_RESOURCE_ID;
}

int KYTY_SYSV_ABI ImeKeyboardSetMode(int32_t user_id, uint32_t mode) {
	if (user_id == -1) {
		return ERROR_INVALID_USER_ID;
	}
	std::scoped_lock lock(g_mutex);
	if (!g_keyboard_state.open) {
		return ERROR_NOT_OPENED;
	}
	return (mode & ~VALID_KEYBOARD_MODE) == 0 ? OK : ERROR_INVALID_MODE;
}

VisualState GetVisualState() noexcept {
	return {g_open.load(std::memory_order_acquire), g_revision.load(std::memory_order_acquire)};
}

void SetVisibilityCallback(VisibilityCallback callback) noexcept {
	g_visibility_callback.store(callback, std::memory_order_release);
}

bool GetHostSnapshot(HostSnapshot* snapshot) {
	if (snapshot == nullptr) {
		return false;
	}
	std::scoped_lock lock(g_mutex);
	if (!g_state.open) {
		return false;
	}
	snapshot->generation           = g_state.generation;
	snapshot->type                 = g_state.param.type;
	snapshot->enter_label          = g_state.param.enter_label;
	snapshot->option               = g_state.option;
	snapshot->max_text_length      = g_state.max_text_length;
	snapshot->cursor               = g_state.cursor;
	snapshot->disable_device       = g_state.extended.disable_device;
	snapshot->key_panel_visible    = (g_state.option & OPTION_EXT_KEYBOARD) == 0 ||
	                                 (g_state.extended.option & EXT_OPTION_HIDE_KEY_PANEL) == 0;
	snapshot->posx                 = g_state.param.posx;
	snapshot->posy                 = g_state.param.posy;
	snapshot->horizontal_alignment = g_state.param.horizontal_alignment;
	snapshot->vertical_alignment   = g_state.param.vertical_alignment;
	snapshot->panel_width          = g_state.param.type == Type::Number ? 370 : 793;
	snapshot->panel_height         = g_state.param.type == Type::Number ? 402 : 408;
	if ((g_state.option & OPTION_USE_OVER_2K) != 0) {
		snapshot->panel_width *= 2;
		snapshot->panel_height *= 2;
	}
	snapshot->text = g_state.text;
	return true;
}

static bool ApplyInsertText(uint64_t generation, std::u16string_view text) {
	uint64_t       revision   = 0;
	uint32_t       cursor     = 0;
	uint32_t       max_length = 0;
	bool           multiline  = false;
	TextFilter     filter     = nullptr;
	std::u16string old_text;
	std::u16string candidate;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_state.open || g_state.generation != generation || text.empty()) {
			return false;
		}
		uint32_t valid_length = 0;
		multiline             = (g_state.option & OPTION_MULTILINE) != 0;
		if (!ValidateText(text.data(), static_cast<uint32_t>(text.size()), multiline,
		                  &valid_length) ||
		    valid_length != text.size()) {
			return false;
		}
		std::u16string allowed;
		for (const char16_t value: text) {
			if (IsAllowedInput(value, g_state.param.type, g_state.option)) {
				allowed.push_back(value);
			}
		}
		const size_t available = g_state.max_text_length - g_state.text.size();
		if (allowed.empty() || available == 0) {
			return false;
		}
		allowed.resize(std::min(allowed.size(), available));
		if (!allowed.empty() && allowed.back() >= 0xd800 && allowed.back() <= 0xdbff) {
			allowed.pop_back();
		}
		if (allowed.empty()) {
			return false;
		}
		revision   = g_state.revision;
		max_length = g_state.max_text_length;
		filter     = g_state.param.filter;
		old_text   = g_state.text;
		candidate  = old_text;
		candidate.insert(g_state.cursor, allowed);
		cursor = g_state.cursor + static_cast<uint32_t>(allowed.size());
	}
	return CommitFilteredText(generation, revision, std::move(old_text), std::move(candidate),
	                          cursor, filter, max_length, multiline);
}

static bool ApplyBackspace(uint64_t generation) {
	uint64_t       revision   = 0;
	uint32_t       cursor     = 0;
	uint32_t       max_length = 0;
	bool           multiline  = false;
	TextFilter     filter     = nullptr;
	std::u16string old_text;
	std::u16string candidate;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_state.open || g_state.generation != generation || g_state.cursor == 0) {
			return false;
		}
		uint32_t first = g_state.cursor - 1;
		if (first > 0 && g_state.text[first] >= 0xdc00 && g_state.text[first] <= 0xdfff &&
		    g_state.text[first - 1] >= 0xd800 && g_state.text[first - 1] <= 0xdbff) {
			first--;
		}
		revision   = g_state.revision;
		max_length = g_state.max_text_length;
		multiline  = (g_state.option & OPTION_MULTILINE) != 0;
		filter     = g_state.param.filter;
		old_text   = g_state.text;
		candidate  = old_text;
		candidate.erase(first, g_state.cursor - first);
		cursor = first;
	}
	return CommitFilteredText(generation, revision, std::move(old_text), std::move(candidate),
	                          cursor, filter, max_length, multiline);
}

static bool ApplyMoveCursor(uint64_t generation, int delta) {
	std::scoped_lock lock(g_mutex);
	if (!g_state.open || g_state.generation != generation || delta == 0) {
		return false;
	}
	const uint32_t old_cursor = g_state.cursor;
	int            next =
	    std::clamp(static_cast<int>(old_cursor) + delta, 0, static_cast<int>(g_state.text.size()));
	if (delta < 0 && next > 0 && next < static_cast<int>(g_state.text.size()) &&
	    g_state.text[next] >= 0xdc00 && g_state.text[next] <= 0xdfff &&
	    g_state.text[next - 1] >= 0xd800 && g_state.text[next - 1] <= 0xdbff) {
		next--;
	} else if (delta > 0 && next > 0 && next < static_cast<int>(g_state.text.size()) &&
	           g_state.text[next - 1] >= 0xd800 && g_state.text[next - 1] <= 0xdbff &&
	           g_state.text[next] >= 0xdc00 && g_state.text[next] <= 0xdfff) {
		next++;
	}
	g_state.cursor = static_cast<uint32_t>(next);
	if (g_state.cursor == old_cursor) {
		return false;
	}
	const uint32_t direction = g_state.cursor > old_cursor ? 2 : 1;
	const uint32_t steps     = static_cast<uint32_t>(
	    std::abs(static_cast<int>(g_state.cursor) - static_cast<int>(old_cursor)));
	for (uint32_t i = 0; i < steps; i++) {
		QueuedEvent event;
		event.event.id               = 2;
		event.event.param.caret_move = direction;
		QueueEventLocked(std::move(event));
	}
	UpdateRevisionLocked();
	return true;
}

static bool ApplyAccept(uint64_t generation) {
	std::scoped_lock lock(g_mutex);
	if (!g_state.open || g_state.generation != generation) {
		return false;
	}
	QueuedEvent event;
	event.event.id = 5;
	return QueueEventLocked(std::move(event));
}

static bool ApplyCancel(uint64_t generation) {
	std::scoped_lock lock(g_mutex);
	if (!g_state.open || g_state.generation != generation) {
		return false;
	}
	QueuedEvent event;
	event.event.id = 4;
	return QueueEventLocked(std::move(event));
}

bool HostInsertText(uint64_t generation, std::u16string_view text) {
	ExternalInput input;
	input.action = ExternalAction::Text;
	input.text   = text;
	return HostQueueExternalInput(generation, std::move(input));
}

bool HostBackspace(uint64_t generation) {
	ExternalInput input;
	input.action = ExternalAction::Backspace;
	return HostQueueExternalInput(generation, std::move(input));
}

bool HostAccept(uint64_t generation) {
	ExternalInput input;
	input.action = ExternalAction::Accept;
	return HostQueueExternalInput(generation, std::move(input));
}

bool HostCancel(uint64_t generation) {
	ExternalInput input;
	input.action = ExternalAction::Cancel;
	return HostQueueExternalInput(generation, std::move(input));
}

bool HostQueueExternalInput(uint64_t generation, ExternalInput input) {
	std::scoped_lock lock(g_mutex);
	if (!g_state.open || g_state.generation != generation) {
		return false;
	}
	if (g_state.external_inputs.size() == EVENT_QUEUE_CAPACITY) {
		g_state.event_overflow = true;
		return false;
	}
	g_state.external_inputs.push_back(std::move(input));
	return true;
}

namespace {

void ApplyExternalInputs() {
	uint64_t                   generation = 0;
	ExtKeyboardFilter          filter     = nullptr;
	int32_t                    user_id    = -1;
	bool                       multiline  = false;
	std::vector<ExternalInput> inputs;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_state.open || g_state.external_inputs.empty()) {
			return;
		}
		generation = g_state.generation;
		filter     = g_state.extended.ext_keyboard_filter;
		user_id    = g_state.param.user_id;
		multiline  = (g_state.option & OPTION_MULTILINE) != 0;
		inputs.swap(g_state.external_inputs);
	}
	for (auto& input: inputs) {
		{
			std::scoped_lock lock(g_mutex);
			if (!g_state.open || g_state.generation != generation) {
				break;
			}
		}
		input.key.user_id = user_id;
		bool accepted     = true;
		if (filter != nullptr && input.key.status != 0) {
			uint16_t keycode = input.key.keycode;
			uint32_t status  = input.key.status;
			if (filter(&input.key, &keycode, &status, nullptr) == 0) {
				accepted = ApplyKeyboardFilterOutput(&input, keycode, status, multiline);
			}
		}
		if (!accepted) {
			continue;
		}
		switch (input.action) {
			case ExternalAction::None: break;
			case ExternalAction::Text: ApplyInsertText(generation, input.text); break;
			case ExternalAction::Backspace: ApplyBackspace(generation); break;
			case ExternalAction::MoveLeft: ApplyMoveCursor(generation, -1); break;
			case ExternalAction::MoveRight: ApplyMoveCursor(generation, 1); break;
			case ExternalAction::Cancel: ApplyCancel(generation); break;
			case ExternalAction::Accept: ApplyAccept(generation); break;
			case ExternalAction::Newline: ApplyInsertText(generation, u"\n"); break;
		}
	}
}

} // namespace

LIB_VERSION("Ime", 1, "Ime", 1, 1);

LIB_DEFINE(InitPlatform_1_Ime) {
	LIB_FUNC("ieCNrVrzKd4", ImeSetText);
	LIB_FUNC("WLxUN2WMim8", ImeSetCaret);
	LIB_FUNC("WmYDzdC4EHI", ImeParamInit);
	LIB_FUNC("ziPDcIjO0Vk", ImeGetPanelSize);
	LIB_FUNC("RPydv-Jr1bc", ImeOpen);
	LIB_FUNC("-4GCfYdNF1s", ImeUpdate);
	LIB_FUNC("TmVP8LzcFcY", ImeClose);
	LIB_FUNC("TXYHFRuL8UY", ImeSetTextGeometry);
	LIB_FUNC("eaFXjfJv3xs", ImeKeyboardOpen);
	LIB_FUNC("PMVehSlfZ94", ImeKeyboardClose);
	LIB_FUNC("dKadqZFgKKQ", ImeKeyboardGetResourceId);
	LIB_FUNC("VkqLPArfFdc", ImeKeyboardGetInfo);
	LIB_FUNC("ua+13Hk9kKs", ImeKeyboardSetMode);
}

} // namespace Libs::Ime
