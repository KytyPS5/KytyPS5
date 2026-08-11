#include "libs/imeDialog.h"

#include "libs/errno.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace Libs::Dialog::ImeDialog {

namespace {

constexpr int Error(uint32_t value) {
	return static_cast<int32_t>(value);
}

constexpr int ERROR_BUSY                    = Error(0x80bc0001);
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
constexpr int ERROR_INVALID_PARAM           = Error(0x80bc0030);
constexpr int ERROR_INVALID_ADDRESS         = Error(0x80bc0031);
constexpr int ERROR_INVALID_RESERVED        = Error(0x80bc0032);
constexpr int ERROR_INVALID_TITLE           = Error(0x80bc0101);
constexpr int ERROR_NOT_RUNNING             = Error(0x80bc0105);
constexpr int ERROR_NOT_FINISHED            = Error(0x80bc0106);
constexpr int ERROR_NOT_IN_USE              = Error(0x80bc0107);

constexpr uint32_t VALID_OPTIONS           = 0x00007bff;
constexpr uint32_t VALID_EXTENDED_OPTIONS  = 0x00005fde;
constexpr uint32_t VALID_EXT_KEYBOARD_MODE = 0x1c000003;
constexpr uint64_t VALID_LANGUAGES         = 0x00000001ff1fffffULL;

struct State {
	Status                     status     = Status::None;
	EndStatus                  end_status = EndStatus::Ok;
	uint64_t                   generation = 0;
	uint64_t                   revision   = 0;
	Param                      param {};
	ExtendedParam              extended {};
	bool                       input_changed  = false;
	bool                       commit_pending = false;
	std::u16string             original_text;
	ImeCommon::TextEditEngine  editor;
	std::u16string             title;
	std::u16string             placeholder;
	std::vector<ExternalInput> external_inputs;
};

std::mutex                      g_mutex;
State                           g_state;
std::atomic<Status>             g_status {Status::None};
std::atomic<uint64_t>           g_revision {0};
std::atomic<VisibilityCallback> g_visibility_callback {nullptr};

bool AllZero(const int8_t* data, size_t size) {
	return std::all_of(data, data + size, [](int8_t value) { return value == 0; });
}

bool ReadBounded(const char16_t* text, uint32_t limit, std::u16string* out) {
	out->clear();
	if (text == nullptr) {
		return true;
	}
	out->reserve(limit);
	for (uint32_t i = 0; i <= limit; ++i) {
		const char16_t value = text[i];
		if (value == u'\0') {
			return ImeCommon::IsValidUtf16(*out);
		}
		if (i == limit) {
			return false;
		}
		out->push_back(value);
	}
	return false;
}

void WriteGuestText(const State& state, const std::u16string& text) {
	if (state.param.input_text_buffer == nullptr) {
		return;
	}
	const size_t size = std::min<size_t>(text.size(), state.param.max_text_length);
	std::memcpy(state.param.input_text_buffer, text.data(), size * sizeof(char16_t));
	state.param.input_text_buffer[size] = u'\0';
}

void NotifyVisibility(bool visible, uint64_t generation) {
	if (const auto callback = g_visibility_callback.load(std::memory_order_acquire);
	    callback != nullptr) {
		callback(visible, generation);
	}
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

int ValidateParam(const Param* param, const ExtendedParam* extended, std::u16string* initial,
                  std::u16string* title, std::u16string* placeholder) {
	if (param == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	if (static_cast<uint32_t>(param->type) > static_cast<uint32_t>(Type::Number)) {
		return ERROR_INVALID_TYPE;
	}
	if ((param->option & ~VALID_OPTIONS) != 0) {
		return ERROR_INVALID_OPTION;
	}
	if ((param->supported_languages & ~VALID_LANGUAGES) != 0) {
		return ERROR_INVALID_LANGUAGES;
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
	const bool multiline = (param->option & OPTION_MULTILINE) != 0;
	const bool password  = (param->option & OPTION_PASSWORD) != 0;
	if ((multiline && password) ||
	    (multiline && param->type != Type::Default && param->type != Type::BasicLatin) ||
	    (password && param->type != Type::BasicLatin && param->type != Type::Number)) {
		return ERROR_INVALID_PARAM;
	}
	if (param->user_id < 0 || param->user_id == 0xff) {
		return ERROR_INVALID_USER_ID;
	}
	if (!AllZero(param->reserved, sizeof(param->reserved))) {
		return ERROR_INVALID_RESERVED;
	}
	if (param->input_text_buffer == nullptr) {
		return ERROR_INVALID_TEXT_BUFFER;
	}
	const int extended_result = ValidateExtended(extended);
	if (extended_result != OK) {
		return extended_result;
	}
	if (static_cast<uint32_t>(param->enter_label) > static_cast<uint32_t>(EnterLabel::Go)) {
		return ERROR_INVALID_ENTER_LABEL;
	}
	if (param->input_method != 0) {
		return ERROR_INVALID_INPUT_METHOD;
	}
	if (param->max_text_length == 0 || param->max_text_length > IME_DIALOG_MAX_TEXT_LENGTH) {
		return ERROR_INVALID_MAX_TEXT_LENGTH;
	}
	if (!ReadBounded(param->input_text_buffer, param->max_text_length, initial)) {
		return ERROR_INVALID_TEXT_BUFFER;
	}
	if (!ReadBounded(param->title, IME_DIALOG_MAX_TITLE_LENGTH, title)) {
		return ERROR_INVALID_TITLE;
	}
	if (!ReadBounded(param->placeholder, IME_DIALOG_MAX_PLACEHOLDER_LENGTH, placeholder)) {
		return ERROR_INVALID_PARAM;
	}
	return OK;
}

void ComputePanelSize(const Param& param, const ExtendedParam* extended, uint32_t* width,
                      uint32_t* height) {
	const bool multiline     = (param.option & OPTION_MULTILINE) != 0;
	const bool hide_keyboard = extended != nullptr && (param.option & OPTION_EXT_KEYBOARD) != 0 &&
	                           (extended->option & 0x00000400) != 0;
	if (param.type == Type::Number) {
		*width  = 370;
		*height = hide_keyboard ? 102 : 522;
	} else if (param.type == Type::BasicLatin) {
		*width = 793;
		if (hide_keyboard) {
			*height = multiline ? 203 : 103;
		} else {
			*height = multiline ? 628 : 528;
		}
	} else {
		*width = 793;
		if (hide_keyboard) {
			*height = multiline ? 268 : 168;
		} else {
			*height = multiline ? 628 : 528;
		}
	}
	if ((param.option & OPTION_USE_OVER_2K) != 0) {
		*width *= 2;
		*height *= 2;
	}
}

void ApplyFilterAndCommit() {
	TextFilter     filter = nullptr;
	std::u16string source;
	uint32_t       max_length = 0;
	uint64_t       generation = 0;
	uint64_t       revision   = 0;
	{
		std::scoped_lock lock(g_mutex);
		if (g_state.status == Status::None || (!g_state.input_changed && !g_state.commit_pending)) {
			return;
		}
		filter                = g_state.input_changed ? g_state.param.filter : nullptr;
		source                = g_state.editor.GetText();
		max_length            = g_state.param.max_text_length;
		generation            = g_state.generation;
		revision              = g_state.revision;
		g_state.input_changed = false;
	}

	if (filter != nullptr) {
		std::u16string filtered;
		if (ImeCommon::RunTextFilter(filter, source, IME_DIALOG_MAX_TEXT_LENGTH, &filtered)) {
			source = std::move(filtered);
			ImeCommon::ClampText(&source, max_length);
		}
	}

	std::scoped_lock lock(g_mutex);
	if (g_state.generation != generation || g_state.revision != revision ||
	    g_state.status == Status::None) {
		if (g_state.generation == generation &&
		    (g_state.status == Status::Running ||
		     (g_state.status == Status::Finished && g_state.end_status == EndStatus::Ok))) {
			g_state.input_changed  = true;
			g_state.commit_pending = true;
		}
		return;
	}
	if (!g_state.input_changed) {
		g_state.editor.ReplaceText(std::move(source), g_state.editor.GetCursor());
	}
	const auto& committed =
	    g_state.status == Status::Finished && g_state.end_status != EndStatus::Ok
	        ? g_state.original_text
	        : g_state.editor.GetText();
	WriteGuestText(g_state, committed);
	g_state.commit_pending = false;
}

bool MatchRunningGeneration(uint64_t generation) {
	return g_state.status == Status::Running && g_state.generation == generation;
}

bool FinishFromHost(uint64_t generation, EndStatus end_status) {
	uint64_t notify_generation = 0;
	{
		std::scoped_lock lock(g_mutex);
		if (!MatchRunningGeneration(generation)) {
			return false;
		}
		g_state.status         = Status::Finished;
		g_state.end_status     = end_status;
		g_state.commit_pending = true;
		g_state.revision++;
		notify_generation = g_state.generation;
		g_status.store(Status::Finished, std::memory_order_release);
		g_revision.store(g_state.revision, std::memory_order_release);
	}
	NotifyVisibility(false, notify_generation);
	return true;
}

void ApplyExternalInputs() {
	std::vector<ExternalInput> inputs;
	ExtKeyboardFilter          filter     = nullptr;
	uint64_t                   generation = 0;
	int32_t                    user_id    = 0;
	bool                       multiline  = false;
	{
		std::scoped_lock lock(g_mutex);
		if (g_state.status != Status::Running || g_state.external_inputs.empty()) {
			return;
		}
		inputs.swap(g_state.external_inputs);
		filter     = g_state.extended.ext_keyboard_filter;
		generation = g_state.generation;
		user_id    = g_state.param.user_id;
		multiline  = (g_state.param.option & OPTION_MULTILINE) != 0;
	}

	for (auto& input: inputs) {
		{
			std::scoped_lock lock(g_mutex);
			if (!MatchRunningGeneration(generation)) {
				break;
			}
		}
		input.key.user_id = user_id;
		bool accepted     = true;
		if (filter != nullptr) {
			uint16_t output_keycode = input.key.keycode;
			uint32_t output_status  = input.key.status;
			if (filter(&input.key, &output_keycode, &output_status, nullptr) == 0) {
				accepted = ImeCommon::ApplyKeyboardFilterOutput(&input, output_keycode,
				                                                output_status, multiline);
			}
		}
		if (!accepted) {
			continue;
		}
		switch (input.action) {
			case ExternalAction::None: break;
			case ExternalAction::Text: HostInsertText(generation, input.text); break;
			case ExternalAction::Backspace: HostBackspace(generation); break;
			case ExternalAction::MoveLeft: HostMoveCursor(generation, -1); break;
			case ExternalAction::MoveRight: HostMoveCursor(generation, 1); break;
			case ExternalAction::Cancel: HostCancel(generation); break;
			case ExternalAction::Accept: HostAccept(generation); break;
			case ExternalAction::Newline: HostInsertText(generation, u"\n"); break;
		}
	}
}

} // namespace

int KYTY_SYSV_ABI ImeDialogGetPanelSize(const Param* param, uint32_t* width, uint32_t* height) {
	return ImeDialogGetPanelSizeExtended(param, nullptr, width, height);
}

int KYTY_SYSV_ABI ImeDialogGetPanelSizeExtended(const Param* param, const ExtendedParam* extended,
                                                uint32_t* width, uint32_t* height) {
	if (param == nullptr || width == nullptr || height == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	if (static_cast<uint32_t>(param->type) > static_cast<uint32_t>(Type::Number)) {
		return ERROR_INVALID_TYPE;
	}
	if ((param->option & ~VALID_OPTIONS) != 0) {
		return ERROR_INVALID_OPTION;
	}
	if ((param->supported_languages & ~VALID_LANGUAGES) != 0) {
		return ERROR_INVALID_LANGUAGES;
	}
	const int extended_result = ValidateExtended(extended);
	if (extended_result != OK) {
		return extended_result;
	}
	ComputePanelSize(*param, extended, width, height);
	return OK;
}

int KYTY_SYSV_ABI ImeDialogInit(const Param* param, const ExtendedParam* extended) {
	if (g_status.load(std::memory_order_acquire) != Status::None) {
		return ERROR_BUSY;
	}

	std::u16string initial;
	std::u16string title;
	std::u16string placeholder;
	const int      validation = ValidateParam(param, extended, &initial, &title, &placeholder);
	if (validation != OK) {
		return validation;
	}

	uint64_t generation = 0;
	{
		std::scoped_lock lock(g_mutex);
		if (g_state.status != Status::None) {
			return ERROR_BUSY;
		}
		const uint64_t next_generation = g_state.generation + 1;
		const uint64_t next_revision   = g_state.revision + 1;
		g_state                        = {};
		g_state.status                 = Status::Running;
		g_state.generation             = next_generation;
		g_state.revision               = next_revision;
		g_state.param                  = *param;
		if (extended != nullptr) {
			g_state.extended = *extended;
		}
		g_state.original_text = initial;
		g_state.editor.Reset(param->type, param->option, param->max_text_length,
		                     std::move(initial));
		g_state.title          = std::move(title);
		g_state.placeholder    = std::move(placeholder);
		g_state.commit_pending = true;
		generation             = g_state.generation;
		g_status.store(Status::Running, std::memory_order_release);
		g_revision.store(g_state.revision, std::memory_order_release);
	}
	NotifyVisibility(true, generation);
	return OK;
}

int KYTY_SYSV_ABI ImeDialogGetStatus() {
	ApplyExternalInputs();
	ApplyFilterAndCommit();
	return static_cast<int>(g_status.load(std::memory_order_acquire));
}

int KYTY_SYSV_ABI ImeDialogAbort() {
	uint64_t generation = 0;
	{
		std::scoped_lock lock(g_mutex);
		if (g_state.status == Status::None) {
			return ERROR_NOT_IN_USE;
		}
		if (g_state.status != Status::Running) {
			return ERROR_NOT_RUNNING;
		}
		generation = g_state.generation;
	}
	if (!FinishFromHost(generation, EndStatus::Aborted)) {
		return ERROR_NOT_RUNNING;
	}
	ApplyFilterAndCommit();
	return OK;
}

int KYTY_SYSV_ABI ImeDialogGetResult(Result* result) {
	{
		std::scoped_lock lock(g_mutex);
		if (g_state.status == Status::None) {
			return ERROR_NOT_IN_USE;
		}
	}
	if (result == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	if (!AllZero(result->reserved, sizeof(result->reserved))) {
		return ERROR_INVALID_RESERVED;
	}
	{
		std::scoped_lock lock(g_mutex);
		if (g_state.status != Status::Finished) {
			return ERROR_NOT_FINISHED;
		}
	}
	ApplyFilterAndCommit();
	std::scoped_lock lock(g_mutex);
	if (g_state.status == Status::None) {
		return ERROR_NOT_IN_USE;
	}
	if (g_state.status != Status::Finished) {
		return ERROR_NOT_FINISHED;
	}
	result->endstatus = g_state.end_status;
	return OK;
}

int KYTY_SYSV_ABI ImeDialogTerm() {
	{
		std::scoped_lock lock(g_mutex);
		if (g_state.status == Status::None) {
			return ERROR_NOT_IN_USE;
		}
		if (g_state.status != Status::Finished) {
			return ERROR_NOT_FINISHED;
		}
	}
	ApplyFilterAndCommit();
	std::scoped_lock lock(g_mutex);
	if (g_state.status == Status::None) {
		return ERROR_NOT_IN_USE;
	}
	if (g_state.status != Status::Finished) {
		return ERROR_NOT_FINISHED;
	}
	const uint64_t generation = g_state.generation;
	const uint64_t revision   = g_state.revision;
	g_state                   = {};
	g_state.generation        = generation;
	g_state.revision          = revision;
	g_status.store(Status::None, std::memory_order_release);
	return OK;
}

int KYTY_SYSV_ABI ImeDialogGetPanelPositionAndForm(PositionAndForm* form) {
	std::scoped_lock lock(g_mutex);
	if (g_state.status == Status::None) {
		return ERROR_NOT_IN_USE;
	}
	if (form == nullptr) {
		return ERROR_INVALID_ADDRESS;
	}
	form->type                 = 2;
	form->posx                 = g_state.param.posx;
	form->posy                 = g_state.param.posy;
	form->horizontal_alignment = g_state.param.horizontal_alignment;
	form->vertical_alignment   = g_state.param.vertical_alignment;
	ComputePanelSize(g_state.param, &g_state.extended, &form->width, &form->height);
	return OK;
}

VisualState GetVisualState() noexcept {
	return {g_status.load(std::memory_order_acquire) == Status::Running,
	        g_revision.load(std::memory_order_acquire)};
}

void SetVisibilityCallback(VisibilityCallback callback) noexcept {
	g_visibility_callback.store(callback, std::memory_order_release);
}

bool GetHostSnapshot(HostSnapshot* snapshot) {
	if (snapshot == nullptr) {
		return false;
	}
	std::scoped_lock lock(g_mutex);
	if (g_state.status != Status::Running) {
		return false;
	}
	snapshot->generation           = g_state.generation;
	snapshot->type                 = g_state.param.type;
	snapshot->enter_label          = g_state.param.enter_label;
	snapshot->option               = g_state.param.option;
	snapshot->max_text_length      = g_state.param.max_text_length;
	snapshot->cursor               = g_state.editor.GetCursor();
	snapshot->disable_device       = g_state.extended.disable_device;
	snapshot->key_panel_visible    = (g_state.param.option & OPTION_EXT_KEYBOARD) == 0 ||
	                                 (g_state.extended.option & 0x00000400) == 0;
	snapshot->posx                 = g_state.param.posx;
	snapshot->posy                 = g_state.param.posy;
	snapshot->horizontal_alignment = g_state.param.horizontal_alignment;
	snapshot->vertical_alignment   = g_state.param.vertical_alignment;
	ComputePanelSize(g_state.param, &g_state.extended, &snapshot->panel_width,
	                 &snapshot->panel_height);
	snapshot->text        = g_state.editor.GetText();
	snapshot->title       = g_state.title;
	snapshot->placeholder = g_state.placeholder;
	return true;
}

bool HostInsertText(uint64_t generation, std::u16string_view text) {
	std::scoped_lock lock(g_mutex);
	if (!MatchRunningGeneration(generation) || !g_state.editor.Insert(text)) {
		return false;
	}
	g_state.input_changed  = true;
	g_state.commit_pending = true;
	return true;
}

bool HostBackspace(uint64_t generation) {
	std::scoped_lock lock(g_mutex);
	if (!MatchRunningGeneration(generation) || !g_state.editor.Backspace()) {
		return false;
	}
	g_state.input_changed  = true;
	g_state.commit_pending = true;
	return true;
}

bool HostMoveCursor(uint64_t generation, int delta) {
	std::scoped_lock lock(g_mutex);
	return MatchRunningGeneration(generation) && g_state.editor.MoveCursor(delta);
}

bool HostAccept(uint64_t generation) {
	return FinishFromHost(generation, EndStatus::Ok);
}

bool HostCancel(uint64_t generation) {
	return FinishFromHost(generation, EndStatus::UserCanceled);
}

bool HostQueueExternalInput(uint64_t generation, ExternalInput input) {
	std::scoped_lock lock(g_mutex);
	if (!MatchRunningGeneration(generation) || g_state.external_inputs.size() >= 128) {
		return false;
	}
	g_state.external_inputs.push_back(std::move(input));
	return true;
}

} // namespace Libs::Dialog::ImeDialog
