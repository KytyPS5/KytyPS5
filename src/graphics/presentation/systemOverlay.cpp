#include "graphics/presentation/systemOverlay.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/localization.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "gpu_blit_shaders/gpu_imgui_hdr_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/presentation/ddsImage.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/window/hostInput.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"
#include "libs/controller.h"
#include "libs/dialog.h"
#include "libs/ime.h"
#include "libs/imeDialog.h"
#include "loader/systemContent.h"
#include "stb_image.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

using Clock = std::chrono::steady_clock;

namespace CoreIme      = Libs::Ime;
namespace DialogIme    = Libs::Dialog::ImeDialog;
namespace SystemDialog = Libs::Dialog::SystemDialog;

namespace Ime {

using Type           = ImeCommon::Type;
using EnterLabel     = ImeCommon::EnterLabel;
using Alignment      = ImeCommon::Alignment;
using ExternalAction = ImeCommon::ExternalAction;
using ExternalInput  = ImeCommon::ExternalInput;
using HostSnapshot   = ImeCommon::HostSnapshot;

constexpr uint32_t OPTION_MULTILINE            = ImeCommon::OPTION_MULTILINE;
constexpr uint32_t OPTION_NO_AUTO_CAPITALIZE   = ImeCommon::OPTION_NO_AUTO_CAPITALIZE;
constexpr uint32_t OPTION_PASSWORD             = ImeCommon::OPTION_PASSWORD;
constexpr uint32_t OPTION_FIXED_POSITION       = ImeCommon::OPTION_FIXED_POSITION;
constexpr uint32_t OPTION_DISABLE_POSITION_ADJ = ImeCommon::OPTION_DISABLE_POSITION_ADJUST;
constexpr uint32_t OPTION_USE_OVER_2K          = ImeCommon::OPTION_USE_OVER_2K;
constexpr uint32_t DISABLE_DEVICE_CONTROLLER   = ImeCommon::DISABLE_DEVICE_CONTROLLER;
constexpr uint32_t DISABLE_DEVICE_EXT_KEYBOARD = ImeCommon::DISABLE_DEVICE_EXT_KEYBOARD;
constexpr uint64_t CORE_GENERATION_BIT         = uint64_t {1} << 63;

uint64_t PackCoreGeneration(uint64_t generation) {
	return generation | CORE_GENERATION_BIT;
}

bool IsCoreGeneration(uint64_t generation) {
	return (generation & CORE_GENERATION_BIT) != 0;
}

uint64_t UnpackGeneration(uint64_t generation) {
	return generation & ~CORE_GENERATION_BIT;
}

bool GetHostSnapshot(HostSnapshot* snapshot) {
	if (snapshot == nullptr) {
		return false;
	}
	if (CoreIme::GetHostSnapshot(snapshot)) {
		snapshot->generation = PackCoreGeneration(snapshot->generation);
		return true;
	}
	return DialogIme::GetHostSnapshot(snapshot);
}

bool HostInsertText(uint64_t generation, std::u16string_view text) {
	return IsCoreGeneration(generation)
	           ? CoreIme::HostInsertText(UnpackGeneration(generation), text)
	           : DialogIme::HostInsertText(generation, text);
}

bool HostBackspace(uint64_t generation) {
	return IsCoreGeneration(generation) ? CoreIme::HostBackspace(UnpackGeneration(generation))
	                                    : DialogIme::HostBackspace(generation);
}

bool HostAccept(uint64_t generation) {
	return IsCoreGeneration(generation) ? CoreIme::HostAccept(UnpackGeneration(generation))
	                                    : DialogIme::HostAccept(generation);
}

bool HostCancel(uint64_t generation) {
	return IsCoreGeneration(generation) ? CoreIme::HostCancel(UnpackGeneration(generation))
	                                    : DialogIme::HostCancel(generation);
}

bool HostQueueExternalInput(uint64_t generation, ExternalInput input) {
	return IsCoreGeneration(generation)
	           ? CoreIme::HostQueueExternalInput(UnpackGeneration(generation), std::move(input))
	           : DialogIme::HostQueueExternalInput(generation, std::move(input));
}

} // namespace Ime

enum class OverlayKind : uint8_t { None, Ime, Dialog };

struct OverlaySession {
	OverlayKind kind       = OverlayKind::None;
	uint64_t    generation = 0;

	bool operator==(const OverlaySession&) const = default;
};

struct OverlaySnapshot {
	OverlaySession             session;
	Ime::HostSnapshot          ime;
	SystemDialog::HostSnapshot dialog;
};

bool GetOverlaySnapshot(OverlaySnapshot* snapshot) {
	if (SystemDialog::GetHostSnapshot(&snapshot->dialog)) {
		snapshot->session = {OverlayKind::Dialog, snapshot->dialog.generation};
		return true;
	}
	if (Ime::GetHostSnapshot(&snapshot->ime)) {
		snapshot->session = {OverlayKind::Ime, snapshot->ime.generation};
		return true;
	}
	snapshot->session = {};
	return false;
}

constexpr size_t INPUT_QUEUE_CAPACITY = 128;
constexpr int    MAX_TOUCH_CURSOR_IMAGE_DIMENSION = 2048;
constexpr float  TOUCH_CURSOR_IMAGE_SIZE          = 48.0f;

enum class TouchParticleSkin : uint8_t { Classic, BlueNeon, Monochrome, Crimson, RedBlue };

TouchParticleSkin GetTouchParticleSkin() {
	static const auto skin = [] {
		for (const auto& value: Config::GetKeymap()) {
			constexpr std::string_view key = "TouchParticleSkin=";
			if (std::string_view(value).starts_with(key)) {
				const auto name = std::string_view(value).substr(key.size());
				if (name == "BlueNeon") {
					return TouchParticleSkin::BlueNeon;
				}
				if (name == "Monochrome") {
					return TouchParticleSkin::Monochrome;
				}
				if (name == "Crimson") {
					return TouchParticleSkin::Crimson;
				}
				if (name == "RedBlue") {
					return TouchParticleSkin::RedBlue;
				}
			}
		}
		return TouchParticleSkin::Classic;
	}();
	return skin;
}

bool TouchCursorImageEnabled() {
	static const bool enabled = [] {
		for (const auto& value: Config::GetKeymap()) {
			if (std::string_view(value) == "TouchCursorImage=Off") {
				return false;
			}
		}
		return true;
	}();
	return enabled;
}

enum class InputKind : uint8_t {
	Button,
	Axis,
	MousePosition,
	MouseButton,
	MouseWheel,
	ResetController
};

struct InputEvent {
	InputKind      kind;
	OverlaySession session;
	int            id;
	float          x;
	float          y;
};

struct VisibilityUpdate {
	OverlaySession session;
	bool           visible;
	bool           capture_controller;
	bool           capture_keyboard;
	bool           text_input;
	bool           multiline;
};

std::atomic<Uint32>          g_visibility_event {static_cast<Uint32>(-1)};
std::mutex                   g_visibility_mutex;
std::mutex                   g_input_mutex;
std::deque<InputEvent>       g_input_events;
std::deque<VisibilityUpdate> g_visibility_updates;
size_t                       g_missing_visibility_wakeups = 0;
bool                         g_input_reset_requested      = false;
uint16_t                     g_last_external_keycode      = 0;
uint32_t                     g_last_external_status       = 0;
OverlaySession               g_input_session;
bool                         g_input_active               = false;
bool                         g_input_controller           = false;
bool                         g_input_keyboard             = false;
bool                         g_input_multiline            = false;
bool                         g_input_lifecycle_active     = false;
bool                         g_controller_captured        = false;
OverlaySession               g_session;
SDL_Window*                  g_input_window               = nullptr;
struct TrophyNotification {
	std::string            name;
	int32_t                grade;
	std::vector<std::byte> icon_png;
};
std::mutex                     g_trophy_notification_mutex;
std::deque<TrophyNotification> g_trophy_notifications;
std::atomic<bool>              g_trophy_notification_active {false};
std::array<std::filesystem::path, 2> g_splash_paths;
std::atomic<bool>                    g_splash_active {false};
std::atomic<Clock::duration::rep>    g_splash_hidden_at {0};
bool                                 g_splash_fast = false;

std::vector<std::string_view> WrapTrophyTitle(std::string_view text, float font_size, float width) {
	std::vector<std::string_view> lines;
	auto* const                  font = ImGui::GetFont();
	for (;;) {
		const auto newline   = text.find('\n');
		const auto paragraph = text.substr(0, newline);
		auto       line      = paragraph.substr(0, 0);
		for (auto start = paragraph.find_first_not_of(" \t\r"); start != std::string_view::npos;) {
			const auto end  = std::min(paragraph.find_first_of(" \t\r", start), paragraph.size());
			const auto word = paragraph.substr(start, end - start);
			const std::string_view candidate(line.empty() ? word.begin() : line.begin(),
			                                 word.end());
			// Keep an oversized word intact; the toast's clip rectangle handles overflow.
			if (!line.empty() && font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(),
			                                       0.0f, candidate.data(),
			                                       candidate.data() + candidate.size()).x > width) {
				lines.push_back(line);
				line = word;
			} else {
				line = candidate;
			}
			start = paragraph.find_first_not_of(" \t\r", end);
		}
		lines.push_back(line);
		if (newline == std::string_view::npos) {
			return lines;
		}
		text.remove_prefix(newline + 1);
	}
}

// ImGui's built-in font covers Latin only. Merge installed fonts for the selected console language.
void AddSystemFontFallbacks(ImGuiIO& io) {
	io.Fonts->AddFontDefault();
	const auto add = [&](std::initializer_list<const char*> paths) {
		for (const char* path: paths) {
			std::error_code error;
			if (std::filesystem::is_regular_file(path, error)) {
				ImFontConfig config;
				config.MergeMode = true;
				config.Flags = ImFontFlags_NoLoadError;
				if (io.Fonts->AddFontFromFileTTF(path, 0.0f, &config) != nullptr) {
					return true;
				}
			}
		}
		return false;
	};
	bool found =
	    add({"C:/Windows/Fonts/segoeui.ttf", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
	         "/Library/Fonts/Arial Unicode.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	         "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf"});
	const auto  locale     = Common::Localization::LocaleName(Config::GetConsoleLanguage());
	const char* noto_cjk[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
	                          "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
	                          "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc"};
	if (locale == "ja-JP") {
		found = add({"C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/YuGothM.ttc",
		             "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", noto_cjk[0], noto_cjk[1],
		             noto_cjk[2]});
	} else if (locale == "ko-KR") {
		found = add({"C:/Windows/Fonts/malgun.ttf", "/System/Library/Fonts/AppleSDGothicNeo.ttc",
		             noto_cjk[0], noto_cjk[1], noto_cjk[2]});
	} else if (locale == "zh-Hans" || locale == "zh-Hant") {
		found =
		    add({locale == "zh-Hans" ? "C:/Windows/Fonts/msyh.ttc" : "C:/Windows/Fonts/msjh.ttc",
		         "/System/Library/Fonts/PingFang.ttc", noto_cjk[0], noto_cjk[1], noto_cjk[2]});
	} else if (locale == "th-TH") {
		found = add({"C:/Windows/Fonts/leelawui.ttf", "C:/Windows/Fonts/tahoma.ttf",
		             "/System/Library/Fonts/Supplemental/Ayuthaya.ttf",
		             "/usr/share/fonts/truetype/tlwg/Garuda.ttf",
		             "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
		             "/usr/share/fonts/noto/NotoSansThai-Regular.ttf"});
	} else if (locale == "ar-AE") {
		found = add({"C:/Windows/Fonts/tahoma.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf",
		             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		             "/usr/share/fonts/TTF/DejaVuSans.ttf",
		             "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
		             "/usr/share/fonts/noto/NotoSansArabic-Regular.ttf"});
	}
	if (!found) {
		LOGF("[Trophy] no installed fallback font for %.*s\n", static_cast<int>(locale.size()),
		     locale.data());
	}
}

void ClearInputEvents() {
	std::scoped_lock lock(g_input_mutex);
	g_input_events.clear();
	g_input_reset_requested = false;
}

void QueueInput(InputEvent event) {
	std::scoped_lock lock(g_input_mutex);
	const bool       replaceable =
	    event.kind == InputKind::Axis || event.kind == InputKind::MousePosition;
	if (replaceable && !g_input_events.empty()) {
		auto& last = g_input_events.back();
		if (last.session == event.session && last.kind == event.kind && last.id == event.id) {
			last = event;
			return;
		}
	}
	if (g_input_events.size() == INPUT_QUEUE_CAPACITY) {
		g_input_events.clear();
		g_input_reset_requested = true;
	}
	g_input_events.push_back(event);
}

void RetryVisibilityWakeup() {
	const Uint32 type = g_visibility_event.load(std::memory_order_acquire);
	if (type == static_cast<Uint32>(-1)) {
		return;
	}
	std::scoped_lock lock(g_input_mutex);
	if (g_missing_visibility_wakeups == 0) {
		return;
	}
	SDL_Event event {};
	event.type = type;
	if (SDL_PushEvent(&event)) {
		g_missing_visibility_wakeups--;
	}
}

void RefreshVisibility() {
	std::scoped_lock visibility_lock(g_visibility_mutex);
	if (!g_input_lifecycle_active) {
		return;
	}
	OverlaySnapshot snapshot;
	const bool      visible = GetOverlaySnapshot(&snapshot);
	if (snapshot.session == g_session) {
		return;
	}
	g_session               = snapshot.session;
	bool capture_controller = false;
	bool capture_keyboard   = false;
	bool text_input         = false;
	bool multiline          = false;
	if (snapshot.session.kind == OverlayKind::Dialog) {
		capture_controller = true;
		capture_keyboard   = true;
	} else if (snapshot.session.kind == OverlayKind::Ime) {
		capture_controller = (snapshot.ime.disable_device & Ime::DISABLE_DEVICE_CONTROLLER) == 0;
		capture_keyboard   = (snapshot.ime.disable_device & Ime::DISABLE_DEVICE_EXT_KEYBOARD) == 0;
		text_input         = capture_keyboard;
		multiline          = (snapshot.ime.option & Ime::OPTION_MULTILINE) != 0;
	}
	const bool was_controller = std::exchange(g_controller_captured, capture_controller);
	if (capture_controller || was_controller) {
		Controller::ResetInputState();
	}
	const Uint32 type = g_visibility_event.load(std::memory_order_acquire);
	if (type != static_cast<Uint32>(-1)) {
		SDL_Event event {};
		event.type = type;
		std::scoped_lock input_lock(g_input_mutex);
		g_visibility_updates.push_back({snapshot.session, visible, capture_controller,
		                                capture_keyboard, text_input, multiline});
		if (g_missing_visibility_wakeups != 0 || !SDL_PushEvent(&event)) {
			g_missing_visibility_wakeups++;
		}
	}
}

void OnCoreVisibilityChanged(bool, uint64_t) {
	RefreshVisibility();
}

void OnDialogVisibilityChanged(bool, uint64_t) {
	RefreshVisibility();
}

uint32_t ExternalKeyStatus(SDL_Keymod modifiers, bool character_valid) {
	uint32_t status = 0x00000001 | (character_valid ? 0x00000002 : 0);
	if ((modifiers & SDL_KMOD_LCTRL) != 0) status |= 0x00000100;
	if ((modifiers & SDL_KMOD_LSHIFT) != 0) status |= 0x00000200;
	if ((modifiers & SDL_KMOD_LALT) != 0) status |= 0x00000400;
	if ((modifiers & SDL_KMOD_LGUI) != 0) status |= 0x00000800;
	if ((modifiers & SDL_KMOD_RCTRL) != 0) status |= 0x00001000;
	if ((modifiers & SDL_KMOD_RSHIFT) != 0) status |= 0x00002000;
	if ((modifiers & SDL_KMOD_RALT) != 0) status |= 0x00004000;
	if ((modifiers & SDL_KMOD_RGUI) != 0) status |= 0x00008000;
	if ((modifiers & SDL_KMOD_NUM) != 0) status |= 0x00010000;
	if ((modifiers & SDL_KMOD_CAPS) != 0) status |= 0x00020000;
	return status;
}

Ime::ExternalInput MakeExternalInput(Ime::ExternalAction action, uint16_t keycode,
                                     uint32_t status) {
	Ime::ExternalInput input {};
	input.key.keycode = keycode;
	input.key.status  = status;
	input.key.type    = 4;
	input.action      = action;
	return input;
}

std::u16string Utf8ToUtf16(std::string_view text) {
	std::u16string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size();) {
		const auto first     = static_cast<uint8_t>(text[i++]);
		uint32_t   codepoint = 0;
		uint32_t   remaining = 0;
		if (first < 0x80) {
			codepoint = first;
		} else if ((first & 0xe0) == 0xc0) {
			codepoint = first & 0x1f;
			remaining = 1;
		} else if ((first & 0xf0) == 0xe0) {
			codepoint = first & 0x0f;
			remaining = 2;
		} else if ((first & 0xf8) == 0xf0) {
			codepoint = first & 0x07;
			remaining = 3;
		} else {
			continue;
		}
		if (i + remaining > text.size()) {
			break;
		}
		bool valid = true;
		for (uint32_t j = 0; j < remaining; j++) {
			const auto next = static_cast<uint8_t>(text[i++]);
			if ((next & 0xc0) != 0x80) {
				valid = false;
				break;
			}
			codepoint = (codepoint << 6) | (next & 0x3f);
		}
		if (!valid || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
			continue;
		}
		if (codepoint <= 0xffff) {
			result.push_back(static_cast<char16_t>(codepoint));
		} else {
			codepoint -= 0x10000;
			result.push_back(static_cast<char16_t>(0xd800 + (codepoint >> 10)));
			result.push_back(static_cast<char16_t>(0xdc00 + (codepoint & 0x3ff)));
		}
	}
	return result;
}

// The Arabic notification is a single RTL run. Use presentation forms because ImGui does not
// shape Arabic. This intentionally applies only to the catalog subtitle, never game-provided names.
std::string PrepareTrophySubtitle(int language) {
	const auto text = Common::Localization::Get(language, "trophy.toast.earned");
	if (Common::Localization::LocaleName(language) != "ar-AE") {
		return std::string(text);
	}
	struct ArabicForm {
		char16_t letter;
		char16_t isolated;
		bool     joins_next;
	};
	static constexpr ArabicForm FORMS[] = {
	    {0x0621, 0xfe80, false}, {0x0622, 0xfe81, false}, {0x0623, 0xfe83, false},
	    {0x0624, 0xfe85, false}, {0x0625, 0xfe87, false}, {0x0626, 0xfe89, true},
	    {0x0627, 0xfe8d, false}, {0x0628, 0xfe8f, true},  {0x0629, 0xfe93, false},
	    {0x062a, 0xfe95, true},  {0x062b, 0xfe99, true},  {0x062c, 0xfe9d, true},
	    {0x062d, 0xfea1, true},  {0x062e, 0xfea5, true},  {0x062f, 0xfea9, false},
	    {0x0630, 0xfeab, false}, {0x0631, 0xfead, false}, {0x0632, 0xfeaf, false},
	    {0x0633, 0xfeb1, true},  {0x0634, 0xfeb5, true},  {0x0635, 0xfeb9, true},
	    {0x0636, 0xfebd, true},  {0x0637, 0xfec1, true},  {0x0638, 0xfec5, true},
	    {0x0639, 0xfec9, true},  {0x063a, 0xfecd, true},  {0x0641, 0xfed1, true},
	    {0x0642, 0xfed5, true},  {0x0643, 0xfed9, true},  {0x0644, 0xfedd, true},
	    {0x0645, 0xfee1, true},  {0x0646, 0xfee5, true},  {0x0647, 0xfee9, true},
	    {0x0648, 0xfeed, false}, {0x0649, 0xfeef, false}, {0x064a, 0xfef1, true},
	};
	const auto form = [](char16_t c) -> const ArabicForm* {
		for (const auto& value: FORMS) {
			if (value.letter == c) {
				return &value;
			}
		}
		return nullptr;
	};
	const auto logical = Utf8ToUtf16(text);
	if (std::none_of(logical.begin(), logical.end(),
	                 [](char16_t c) { return c >= 0x0600 && c <= 0x06ff; })) {
		return std::string(text);
	}
	auto visual = logical;
	for (size_t i = 0; i < logical.size(); ++i) {
		const auto* current = form(logical[i]);
		if (current == nullptr) {
			continue;
		}
		const auto* previous = i > 0 ? form(logical[i - 1]) : nullptr;
		const auto* next     = i + 1 < logical.size() ? form(logical[i + 1]) : nullptr;
		const bool  joins_previous =
		    previous != nullptr && previous->joins_next && current->letter != 0x0621;
		const bool joins_next = current->joins_next && next != nullptr && next->letter != 0x0621;
		visual[i]             = static_cast<char16_t>(current->isolated + (joins_next ? 2 : 0) +
		                                              (joins_previous ? 1 : 0));
	}
	std::reverse(visual.begin(), visual.end());
	return Common::Utf16ToUtf8(visual);
}

std::string VisibleText(const Ime::HostSnapshot& snapshot, size_t max_units) {
	const size_t cursor        = std::min<size_t>(snapshot.cursor, snapshot.text.size());
	const size_t payload_units = max_units > 4 ? max_units - 4 : 1;
	size_t       begin         = cursor > payload_units / 2 ? cursor - payload_units / 2 : 0;
	size_t       end           = std::min(snapshot.text.size(), begin + payload_units);
	if (end == snapshot.text.size() && end - begin < payload_units) {
		begin = end > payload_units ? end - payload_units : 0;
	}
	if (begin > 0 && snapshot.text[begin] >= 0xdc00 && snapshot.text[begin] <= 0xdfff) {
		begin--;
	}
	if (end < snapshot.text.size() && end > begin && snapshot.text[end - 1] >= 0xd800 &&
	    snapshot.text[end - 1] <= 0xdbff) {
		end--;
	}

	std::u16string visible;
	if (begin != 0) {
		visible.push_back(u'\u2026');
	}
	const size_t caret = visible.size() + cursor - begin;
	if ((snapshot.option & Ime::OPTION_PASSWORD) != 0) {
		visible.append(end - begin, u'*');
	} else {
		visible.append(snapshot.text, begin, end - begin);
		std::replace(visible.begin(), visible.end(), u'\n', u'\u21b5');
		std::replace(visible.begin(), visible.end(), u'\r', u'\u21b5');
	}
	visible.insert(std::min(caret, visible.size()), 1, u'|');
	if (end != snapshot.text.size()) {
		visible.push_back(u'\u2026');
	}
	return Common::Utf16ToUtf8(visible.c_str());
}

const char* EnterLabel(Ime::EnterLabel label) {
	switch (label) {
		case Ime::EnterLabel::Send: return "Send";
		case Ime::EnterLabel::Search: return "Search";
		case Ime::EnterLabel::Go: return "Go";
		default: return "Done";
	}
}

float AlignmentPivot(Ime::Alignment alignment) {
	switch (alignment) {
		case Ime::Alignment::Start: return 0.0f;
		case Ime::Alignment::End: return 1.0f;
		default: return 0.5f;
	}
}

ImGuiKey ControllerButtonToKey(int button) {
	switch (button) {
		case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
		case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
		case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
		case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
		default: return ImGuiKey_None;
	}
}

PFN_vkVoidFunction LoadVulkanFunction(const char* name, void* user_data) {
	auto& graphics = *static_cast<GraphicContext*>(user_data);
	return graphics.instance.getProcAddr(name);
}

void CheckVulkanResult(VkResult result) {
	EXIT_IF(result != VK_SUCCESS);
}

} // namespace

void InitializeSplashScreen(const std::filesystem::path& sce_sys) {
	const auto language = Config::GetConsoleLanguage();
	for (size_t i = 0; i < g_splash_paths.size(); ++i) {
		auto& path = g_splash_paths[i];
		path       = sce_sys / fmt::format("pic{}_{:02}.dds", i + 1, language);
		if (!Common::File::IsFileExisting(path)) {
			path = sce_sys / fmt::format("pic{}.dds", i + 1);
		}
	}
	int32_t attribute = 0;
	Loader::SystemContentParamSfoGetInt("ATTRIBUTE3", &attribute);
	g_splash_fast = (attribute & 0x1000) != 0;
	g_splash_hidden_at.store(0, std::memory_order_release);
	g_splash_active.store(Common::File::IsFileExisting(g_splash_paths[0]) &&
	                          Common::File::IsFileExisting(g_splash_paths[1]),
	                      std::memory_order_release);
}

void HideSplashScreen() {
	Clock::duration::rep expected = 0;
	g_splash_hidden_at.compare_exchange_strong(expected, Clock::now().time_since_epoch().count());
}

void InitializeSystemOverlayInput(SDL_Window* window) {
	EXIT_IF(window == nullptr);
	const Uint32 type = SDL_RegisterEvents(1);
	EXIT_IF(type == static_cast<Uint32>(-1));
	g_input_window = window;
	{
		std::scoped_lock lock(g_visibility_mutex);
		g_input_lifecycle_active = true;
		g_visibility_event.store(type, std::memory_order_release);
	}
	CoreIme::SetVisibilityCallback(OnCoreVisibilityChanged);
	DialogIme::SetVisibilityCallback(OnDialogVisibilityChanged);
	SystemDialog::SetVisibilityCallback(RefreshVisibility);
	RefreshVisibility();
}

void ShutdownSystemOverlayInput() {
	g_splash_active.store(false, std::memory_order_release);
	CoreIme::SetVisibilityCallback(nullptr);
	DialogIme::SetVisibilityCallback(nullptr);
	SystemDialog::SetVisibilityCallback(nullptr);
	{
		std::scoped_lock lock(g_visibility_mutex);
		g_input_lifecycle_active = false;
		g_session                = {};
		if (std::exchange(g_controller_captured, false)) {
			Controller::ResetInputState();
		}
		g_visibility_event.store(static_cast<Uint32>(-1), std::memory_order_release);
	}
	ClearInputEvents();
	{
		std::scoped_lock lock(g_input_mutex);
		g_visibility_updates.clear();
		g_missing_visibility_wakeups = 0;
	}
	g_input_active     = false;
	g_input_session    = {};
	g_input_controller = false;
	g_input_keyboard   = false;
	g_input_multiline  = false;
	if (g_input_window != nullptr && SDL_TextInputActive(g_input_window)) {
		SDL_StopTextInput(g_input_window);
	}
	g_input_window = nullptr;
}

SystemOverlayVisualState GetSystemOverlayVisualState() noexcept {
	const auto core   = CoreIme::GetVisualState();
	const auto dialog = DialogIme::GetVisualState();
	const auto system = SystemDialog::GetVisualState();
	const bool trophy_active = g_trophy_notification_active.load(std::memory_order_acquire);
	const bool splash        = g_splash_active.load(std::memory_order_acquire);
	const auto cursor        = GetHostInputTouchPadVisualState();
	return {core.active || dialog.active || system.active || trophy_active || splash ||
	            (cursor.enabled && cursor.cursor_visible),
	        core.revision + dialog.revision + system.revision + (trophy_active ? 1u : 0u) +
	            (splash ? 2u : 0u) + static_cast<decltype(core.revision)>(cursor.revision) +
	            PerformanceOverlayRevision()};
}

bool ProcessSystemOverlayInput(const SDL_Event& event) {
	RetryVisibilityWakeup();
	if (event.type == g_visibility_event.load(std::memory_order_acquire)) {
		VisibilityUpdate update {};
		{
			std::scoped_lock lock(g_input_mutex);
			if (g_visibility_updates.empty()) {
				return true;
			}
			update = g_visibility_updates.front();
			g_visibility_updates.pop_front();
		}
		ClearInputEvents();
		g_last_external_keycode = 0;
		g_last_external_status  = 0;
		g_input_session         = update.session;
		g_input_active          = update.visible;
		g_input_controller      = update.capture_controller;
		g_input_keyboard        = update.capture_keyboard;
		g_input_multiline       = update.multiline;
		if (update.text_input) {
			SDL_StartTextInput(g_input_window);
		} else if (SDL_TextInputActive(g_input_window)) {
			SDL_StopTextInput(g_input_window);
		}
		return true;
	}
	if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
		if (g_input_active && g_input_controller) {
			QueueInput({InputKind::ResetController, g_input_session, 0, 0.0f, 0.0f});
		}
		return false;
	}
	if (!g_input_active) {
		return false;
	}

	const auto     session          = g_input_session;
	const uint64_t generation       = session.generation;
	const bool     controller_event = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
	                                  event.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
	                                  event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
	if (controller_event && !g_input_controller) {
		return false;
	}
	const bool keyboard_event = event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_TEXT_EDITING ||
	                            event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP;
	if (keyboard_event && !g_input_keyboard) {
		return false;
	}
	if (keyboard_event && session.kind == OverlayKind::Dialog) {
		if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
		    (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER ||
		     event.key.key == SDLK_ESCAPE)) {
			SystemDialog::HostClose(generation);
		}
		return true;
	}
	switch (event.type) {
		case SDL_EVENT_TEXT_INPUT: {
			const auto text = Utf8ToUtf16(event.text.text);
			if (!text.empty()) {
				auto input = MakeExternalInput(Ime::ExternalAction::Text, g_last_external_keycode,
				                               g_last_external_status | 0x00000002);
				input.key.character = text.front();
				input.text          = text;
				Ime::HostQueueExternalInput(generation, std::move(input));
			}
			return true;
		}
		case SDL_EVENT_TEXT_EDITING: return true;
		case SDL_EVENT_KEY_DOWN: {
			g_last_external_keycode = static_cast<uint16_t>(event.key.scancode);
			g_last_external_status  = ExternalKeyStatus(event.key.mod, false);
			auto action = Ime::ExternalAction::Text;
			bool queue  = true;
			if (event.key.key == SDLK_BACKSPACE) {
				action = Ime::ExternalAction::Backspace;
			} else if (event.key.key == SDLK_LEFT) {
				action = Ime::ExternalAction::MoveLeft;
			} else if (event.key.key == SDLK_RIGHT) {
				action = Ime::ExternalAction::MoveRight;
			} else if (event.key.key == SDLK_ESCAPE) {
				action = Ime::ExternalAction::Cancel;
			} else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
				action =
				    g_input_multiline ? Ime::ExternalAction::Newline : Ime::ExternalAction::Accept;
			} else if (event.key.key == SDLK_TAB) {
				action = Ime::ExternalAction::None;
			} else {
				queue = false;
			}
			if (queue) {
				Ime::HostQueueExternalInput(
				    generation,
				    MakeExternalInput(action, g_last_external_keycode, g_last_external_status));
			}
			return true;
		}
		case SDL_EVENT_KEY_UP: return true;
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
			QueueInput({InputKind::Button, session, event.gbutton.button,
			            event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1.0f : 0.0f, 0.0f});
			return true;
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			QueueInput({InputKind::Axis, session, event.gaxis.axis,
			            static_cast<float>(event.gaxis.value), 0.0f});
			return true;
		case SDL_EVENT_MOUSE_MOTION:
			QueueInput({InputKind::MousePosition, session, 0, static_cast<float>(event.motion.x),
			            static_cast<float>(event.motion.y)});
			return true;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			QueueInput({InputKind::MousePosition, session, 0, static_cast<float>(event.button.x),
			            static_cast<float>(event.button.y)});
			QueueInput({InputKind::MouseButton, session, event.button.button,
			            event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1.0f : 0.0f, 0.0f});
			return true;
		case SDL_EVENT_MOUSE_WHEEL:
			QueueInput({InputKind::MouseWheel, session, 0, static_cast<float>(event.wheel.x),
			            static_cast<float>(event.wheel.y)});
			return true;
		default: return false;
	}
}

void NotifyTrophyUnlocked(std::string_view name, int32_t grade,
                          std::span<const std::byte> icon_png) {
	std::scoped_lock lock(g_trophy_notification_mutex);
	g_trophy_notifications.push_back(
	    {std::string(name), grade, std::vector<std::byte>(icon_png.begin(), icon_png.end())});
	g_trophy_notification_active.store(true, std::memory_order_release);
}

struct SystemOverlay::Impl {
	enum class TouchParticleIcon : uint8_t { Square, Triangle, Cross, Circle };

	struct TouchParticle {
		ImVec2            position;
		Clock::time_point born;
		TouchParticleIcon icon;
	};

	struct TouchTrailSegment {
		ImVec2            from;
		ImVec2            to;
		Clock::time_point born;
	};

	explicit Impl(GraphicContext& context): graphics(context) {}

	~Impl() {
		SDL_DestroyAudioStream(trophy_sound_stream);
		SDL_free(trophy_sound_data);
		if (trophy_sound_initialized) {
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
		}
		ReleaseVulkan();
		if (imgui_context != nullptr) {
			ImGui::SetCurrentContext(imgui_context);
			if (hand_cursor_image) {
				ImGui::UnregisterUserTexture(hand_cursor_image.get());
			}
			if (drawing_cursor_image) {
				ImGui::UnregisterUserTexture(drawing_cursor_image.get());
			}
			if (trophy_notification_image) {
				ImGui::UnregisterUserTexture(trophy_notification_image.get());
			}
			for (const auto& texture: retired_trophy_images) {
				ImGui::UnregisterUserTexture(texture.get());
			}
			ImGui::DestroyContext(imgui_context);
		}
	}

	void EnsureContext() {
		if (imgui_context != nullptr) {
			ImGui::SetCurrentContext(imgui_context);
			return;
		}
		IMGUI_CHECKVERSION();
		imgui_context = ImGui::CreateContext();
		ImGui::SetCurrentContext(imgui_context);
		auto& io       = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
		io.ConfigNavCursorVisibleAlways = true;
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		io.BackendPlatformName = "Kyty system overlay input";
		AddSystemFontFallbacks(io);
		ImGui::StyleColorsDark();
		auto& style          = ImGui::GetStyle();
		style.WindowRounding = 10.0f;
		style.FrameRounding  = 6.0f;
		style.ItemSpacing    = {8.0f, 8.0f};
		hand_cursor_image    = LoadTouchCursorImage("hand.png");
		drawing_cursor_image = LoadTouchCursorImage("drawing.png");
	}

	void PlayTrophySound() {
		if (!trophy_sound_attempted) {
			trophy_sound_attempted   = true;
			trophy_sound_initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
			if (!trophy_sound_initialized) {
				LOGF("[Trophy] could not initialize audio: %s\n", SDL_GetError());
				return;
			}
			const char* base_path = SDL_GetBasePath();
			if (base_path == nullptr) {
				LOGF("[Trophy] could not locate sound: %s\n", SDL_GetError());
				return;
			}
			const std::string path = std::string(base_path) + "assets/sounds/trophy-unlock.wav";
			SDL_AudioSpec     spec {};
			if (!SDL_LoadWAV(path.c_str(), &spec, &trophy_sound_data, &trophy_sound_size)) {
				LOGF("[Trophy] could not load %s: %s\n", path.c_str(), SDL_GetError());
				return;
			}
			if (trophy_sound_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
				LOGF("[Trophy] sound is too large: %s\n", path.c_str());
				return;
			}
			trophy_sound_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
			                                                &spec, nullptr, nullptr);
			if (trophy_sound_stream == nullptr) {
				LOGF("[Trophy] could not open audio stream: %s\n", SDL_GetError());
				return;
			}
		}
		if (trophy_sound_stream != nullptr &&
		    (!SDL_PutAudioStreamData(trophy_sound_stream, trophy_sound_data,
		                             static_cast<int>(trophy_sound_size)) ||
		     !SDL_ResumeAudioStreamDevice(trophy_sound_stream))) {
			LOGF("[Trophy] could not play sound: %s\n", SDL_GetError());
		}
	}

	std::unique_ptr<ImTextureData> LoadTouchCursorImage(std::string_view filename) {
		const char* base_path = SDL_GetBasePath();
		if (base_path == nullptr) {
			LOGF("Could not locate the emulator folder for touchpad cursor images: %s\n",
			     SDL_GetError());
			return nullptr;
		}
		const std::filesystem::path path =
		    std::filesystem::path(base_path) / "assets" / "cursors" / std::string(filename);

		Common::File file;
		if (!file.Open(path, Common::File::Mode::Read)) {
			LOGF("Touchpad cursor image not found: %s\n", Common::PathToString(path).c_str());
			return nullptr;
		}
		const auto encoded = file.ReadWholeBuffer();
		if (encoded.empty() ||
		    encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
			LOGF("Touchpad cursor image is empty or too large: %s\n",
			     Common::PathToString(path).c_str());
			return nullptr;
		}

		int width = 0;
		int height = 0;
		auto* pixels = stbi_load_from_memory(
		    reinterpret_cast<const stbi_uc*>(encoded.data()), static_cast<int>(encoded.size()),
		    &width, &height, nullptr, 4);
		if (pixels == nullptr || width <= 0 || height <= 0 ||
		    width > MAX_TOUCH_CURSOR_IMAGE_DIMENSION ||
		    height > MAX_TOUCH_CURSOR_IMAGE_DIMENSION) {
			LOGF("Could not load touchpad cursor image %s: %s\n",
			     Common::PathToString(path).c_str(),
			     pixels == nullptr ? stbi_failure_reason() : "image dimensions exceed 2048x2048");
			stbi_image_free(pixels);
			return nullptr;
		}

		auto texture = std::make_unique<ImTextureData>();
		texture->Create(ImTextureFormat_RGBA32, width, height);
		std::memcpy(texture->Pixels, pixels,
		            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
		texture->UseColors = true;
		stbi_image_free(pixels);
		ImGui::RegisterUserTexture(texture.get());
		LOGF("Loaded touchpad cursor image: %s\n", Common::PathToString(path).c_str());
		return texture;
	}

	std::unique_ptr<ImTextureData> LoadTrophyIcon(std::span<const std::byte> encoded) {
		if (encoded.empty() ||
		    encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
			return nullptr;
		}

		const auto* data   = reinterpret_cast<const stbi_uc*>(encoded.data());
		const int   size   = static_cast<int>(encoded.size());
		int         width  = 0;
		int         height = 0;
		if (!stbi_info_from_memory(data, size, &width, &height, nullptr) || width <= 0 ||
		    height <= 0 || width > 512 || height > 512) {
			LOGF("[Trophy] invalid icon or dimensions exceed 512x512\n");
			return nullptr;
		}
		auto* pixels = stbi_load_from_memory(data, size, &width, &height, nullptr, 4);
		if (pixels == nullptr) {
			LOGF("[Trophy] could not decode trophy icon: %s\n", stbi_failure_reason());
			return nullptr;
		}

		auto texture = std::make_unique<ImTextureData>();
		texture->Create(ImTextureFormat_RGBA32, width, height);
		std::memcpy(texture->Pixels, pixels,
		            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
		texture->UseColors = true;
		stbi_image_free(pixels);
		ImGui::RegisterUserTexture(texture.get());
		return texture;
	}

	void FinishSplash() {
		g_splash_active.store(false, std::memory_order_release);
		if (splash_dds[0] || splash_dds[1]) {
			Common::LockGuard lock(graphics.queue_mutex);
			RequireVulkanSuccess(graphics.queue.waitIdle(), "retire splash images");
			splash_dds = {};
		}
	}

	void DrawSplash(vk::Extent2D frame_extent) {
		if (!g_splash_active.load(std::memory_order_acquire)) {
			return;
		}
		for (size_t i = 0; i < g_splash_paths.size(); ++i) {
			if (splash_dds[i]) {
				continue;
			}
			const auto& path = g_splash_paths[i];
			auto image = std::make_unique<DdsImage>(graphics);
			if (!image->Load(path)) {
				LOGF("[Splash] could not load %s\n", path.string().c_str());
				FinishSplash();
				return;
			}
			splash_dds[i] = std::move(image);
		}
		const auto now = Clock::now();
		if (splash_born == Clock::time_point {}) {
			splash_born = now;
		}
		const auto  hidden = g_splash_hidden_at.load(std::memory_order_acquire);
		const float fade =
		    hidden == 0
		        ? 0.0f
		        : std::chrono::duration<float>(now.time_since_epoch() - Clock::duration {hidden})
		                  .count() /
		              0.3f;
		const float alpha = 1.0f - std::clamp(fade, 0.0f, 1.0f);
		if (alpha == 0.0f) {
			FinishSplash();
			return;
		}
		const float  age  = std::chrono::duration<float>(now - splash_born).count();
		const float  logo = g_splash_fast ? 1.0f : std::clamp((age - 1.4f) / 2.2f, 0.0f, 1.0f);
		const ImVec2 screen {static_cast<float>(frame_extent.width),
		                     static_cast<float>(frame_extent.height)};
		const float  fit = std::min(screen.x / 3840.0f, screen.y / 2160.0f);
		const ImVec2 size {3840.0f * fit, 2160.0f * fit};
		const ImVec2 begin {(screen.x - size.x) * 0.5f, (screen.y - size.y) * 0.5f};
		const ImVec2 end {begin.x + size.x, begin.y + size.y};
		auto*        draw = ImGui::GetBackgroundDrawList();
		draw->AddRectFilled({0, 0}, screen, IM_COL32(0, 0, 0, static_cast<int>(255.0f * alpha)));
		for (size_t i = 0; i < g_splash_paths.size(); ++i) {
			draw->AddImage(
			    splash_dds[i]->Texture(), begin, end, {0, 0}, {1, 1},
			    IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha * (i == 0 ? 1.0f : logo))));
		}
	}

	void EnsureVulkan(vk::Format format, uint32_t image_count) {
		EnsureContext();
		if (vulkan_initialized) {
			return;
		}
		EXIT_IF(image_count < 2);
		EXIT_IF(!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, LoadVulkanFunction, &graphics));

		const VkFormat            color_format = static_cast<VkFormat>(format);
		ImGui_ImplVulkan_InitInfo info {};
		info.ApiVersion                   = VK_API_VERSION_1_3;
		info.Instance                     = static_cast<VkInstance>(graphics.instance);
		info.PhysicalDevice               = static_cast<VkPhysicalDevice>(graphics.physical_device);
		info.Device                       = static_cast<VkDevice>(graphics.device);
		info.QueueFamily                  = graphics.queue_family;
		info.Queue                        = static_cast<VkQueue>(graphics.queue);
		info.DescriptorPoolSize           = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
		info.MinImageCount                = image_count;
		info.ImageCount                   = image_count;
		info.UseDynamicRendering          = true;
		info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
		    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
		info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
		info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
		info.CheckVkResultFn = CheckVulkanResult;
		if (IsHdrSwapchainFormat(format)) {
			info.CustomShaderFragCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			info.CustomShaderFragCreateInfo.codeSize = sizeof(GPU_IMGUI_HDR_SPV);
			info.CustomShaderFragCreateInfo.pCode    = GPU_IMGUI_HDR_SPV;
		}
		EXIT_IF(!ImGui_ImplVulkan_Init(&info));
		vulkan_initialized = true;
	}

	void DrainInput(OverlaySession session) {
		std::deque<InputEvent> events;
		bool                   reset = false;
		{
			std::scoped_lock lock(g_input_mutex);
			events.swap(g_input_events);
			reset                   = g_input_reset_requested;
			g_input_reset_requested = false;
		}
		auto& io = ImGui::GetIO();
		if (reset) {
			io.ClearEventsQueue();
			io.ClearInputKeys();
			io.ClearInputMouse();
			right_stick = {};
		}
		for (const auto& event: events) {
			if (event.session != session) {
				continue;
			}
			switch (event.kind) {
				case InputKind::Button: {
					const bool down = event.x != 0.0f;
					if (session.kind == OverlayKind::Ime && down) {
						if (event.id == SDL_GAMEPAD_BUTTON_EAST) {
							Ime::HostCancel(session.generation);
						} else if (event.id == SDL_GAMEPAD_BUTTON_NORTH) {
							Ime::HostBackspace(session.generation);
						}
					}
					const ImGuiKey key = ControllerButtonToKey(event.id);
					if (key != ImGuiKey_None) {
						io.AddKeyEvent(key, down);
					}
					break;
				}
				case InputKind::Axis: {
					const float value = event.x / (event.x < 0.0f ? 32768.0f : 32767.0f);
					if (event.id == SDL_GAMEPAD_AXIS_LEFTX) {
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, value < -0.25f,
						                     std::max(-value, 0.0f));
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, value > 0.25f,
						                     std::max(value, 0.0f));
					} else if (event.id == SDL_GAMEPAD_AXIS_LEFTY) {
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, value < -0.25f,
						                     std::max(-value, 0.0f));
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, value > 0.25f,
						                     std::max(value, 0.0f));
					} else if (event.id == SDL_GAMEPAD_AXIS_RIGHTX) {
						right_stick.x = std::abs(value) > 0.2f ? value : 0.0f;
					} else if (event.id == SDL_GAMEPAD_AXIS_RIGHTY) {
						right_stick.y = std::abs(value) > 0.2f ? value : 0.0f;
					}
					break;
				}
				case InputKind::MousePosition: io.AddMousePosEvent(event.x, event.y); break;
				case InputKind::MouseButton: {
					int button = -1;
					if (event.id == SDL_BUTTON_LEFT) button = 0;
					if (event.id == SDL_BUTTON_RIGHT) button = 1;
					if (event.id == SDL_BUTTON_MIDDLE) button = 2;
					if (button >= 0) io.AddMouseButtonEvent(button, event.x != 0.0f);
					break;
				}
				case InputKind::MouseWheel: io.AddMouseWheelEvent(event.x, event.y); break;
				case InputKind::ResetController:
					io.ClearEventsQueue();
					io.ClearInputKeys();
					right_stick = {};
					break;
			}
		}
	}

	void KeyButton(std::string_view label, char16_t value, uint64_t generation, float width,
	               bool default_focus) {
		const bool pressed = ImGui::Button(label.data(), {width, button_height});
		if (default_focus) {
			ImGui::SetItemDefaultFocus();
		}
		if (pressed) {
			Ime::HostInsertText(generation, std::u16string_view(&value, 1));
		}
	}

	void DrawKeyRows(const Ime::HostSnapshot& snapshot, float width) {
		static constexpr std::array<std::string_view, 4> lower   = {"1234567890", "qwertyuiop",
		                                                            "asdfghjkl", "zxcvbnm"};
		static constexpr std::array<std::string_view, 4> upper   = {"1234567890", "QWERTYUIOP",
		                                                            "ASDFGHJKL", "ZXCVBNM"};
		static constexpr std::array<std::string_view, 3> symbols = {"1234567890", "!@#$%^&*()",
		                                                            "-_=+/:?."};
		static constexpr std::array<std::string_view, 4> number  = {"123", "456", "789", "-0."};

		std::span<const std::string_view> rows;
		if (snapshot.type == Ime::Type::Number) {
			rows = number;
		} else if (symbol_mode) {
			rows = symbols;
		} else {
			rows = shift ? std::span<const std::string_view>(upper)
			             : std::span<const std::string_view>(lower);
		}
		bool   first     = focus_pending;
		size_t row_index = 0;
		for (const auto row: rows) {
			const float key_width =
			    std::min(56.0f * ui_scale, (width - 8.0f * (row.size() - 1)) / row.size());
			const float row_width = key_width * row.size() + 8.0f * (row.size() - 1);
			ImGui::SetCursorPosX((width - row_width) * 0.5f);
			ImGui::PushID(static_cast<int>(row_index++));
			for (size_t i = 0; i < row.size(); i++) {
				ImGui::PushID(static_cast<int>(i));
				const char label[2] = {row[i], '\0'};
				KeyButton(label, static_cast<char16_t>(row[i]), snapshot.generation, key_width,
				          first);
				first = false;
				ImGui::PopID();
				if (i + 1 < row.size()) ImGui::SameLine();
			}
			ImGui::PopID();
		}
		focus_pending = false;
	}

	void DrawIme(const Ime::HostSnapshot& snapshot, vk::Extent2D extent) {
		const ImVec2 display(static_cast<float>(extent.width), static_cast<float>(extent.height));
		const bool   over_2k          = (snapshot.option & Ime::OPTION_USE_OVER_2K) != 0;
		const float  reference_width  = over_2k ? 3840.0f : 1920.0f;
		const float  reference_height = over_2k ? 2160.0f : 1080.0f;
		const float  scale_x          = display.x / reference_width;
		const float  scale_y          = display.y / reference_height;
		const float  width            = static_cast<float>(snapshot.panel_width) * scale_x;
		const float  height           = static_cast<float>(snapshot.panel_height) * scale_y;
		ui_scale                      = std::min(scale_x, scale_y) * (over_2k ? 2.0f : 1.0f);
		button_height                 = std::max(28.0f, 42.0f * ui_scale);

		const ImVec2 pivot {AlignmentPivot(snapshot.horizontal_alignment),
		                    AlignmentPivot(snapshot.vertical_alignment)};
		if ((snapshot.option & Ime::OPTION_FIXED_POSITION) == 0) {
			const float movement = 600.0f * ImGui::GetIO().DeltaTime;
			panel_offset.x += right_stick.x * movement;
			panel_offset.y += right_stick.y * movement;
		}
		const ImVec2 base_position {snapshot.posx * scale_x - pivot.x * width,
		                            snapshot.posy * scale_y - pivot.y * height};
		ImVec2       position {base_position.x + panel_offset.x, base_position.y + panel_offset.y};
		if ((snapshot.option & Ime::OPTION_DISABLE_POSITION_ADJ) == 0) {
			position.x   = std::clamp(position.x, 0.0f, std::max(display.x - width, 0.0f));
			position.y   = std::clamp(position.y, 0.0f, std::max(display.y - height, 0.0f));
			panel_offset = {position.x - base_position.x, position.y - base_position.y};
		}
		ImGui::SetNextWindowPos(position, ImGuiCond_Always);
		ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings;
		ImGui::Begin("##Ime", nullptr, flags);

		const std::u16string title = snapshot.title.empty() ? u"Enter text" : snapshot.title;
		if (!snapshot.key_panel_visible) {
			std::string compact     = Common::Utf16ToUtf8(title.c_str()) + ": ";
			const float glyph_width = std::max(ImGui::CalcTextSize("M").x, 1.0f);
			const float text_width =
			    std::max(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(compact.c_str()).x,
			             glyph_width);
			const size_t visible_units =
			    std::max<size_t>(6, static_cast<size_t>(text_width / glyph_width));
			compact += snapshot.text.empty() && !snapshot.placeholder.empty()
			               ? Common::Utf16ToUtf8(snapshot.placeholder.c_str())
			               : VisibleText(snapshot, visible_units);
			ImGui::TextUnformatted(compact.c_str());
			const float compact_height = std::min(button_height, 28.0f * ui_scale);
			if (ImGui::Button("Cancel", {90.0f * ui_scale, compact_height})) {
				Ime::HostCancel(snapshot.generation);
			}
			ImGui::SameLine();
			if (ImGui::Button(EnterLabel(snapshot.enter_label),
			                  {90.0f * ui_scale, compact_height})) {
				Ime::HostAccept(snapshot.generation);
			}
			ImGui::End();
			return;
		}

		ImGui::TextUnformatted(Common::Utf16ToUtf8(title.c_str()).c_str());
		ImGui::Separator();
		const float text_height = std::max(
		    32.0f, ((snapshot.option & Ime::OPTION_MULTILINE) != 0 ? 80.0f : 48.0f) * ui_scale);
		ImGui::BeginChild("##ImeText", {0.0f, text_height}, true);
		if (snapshot.text.empty() && !snapshot.placeholder.empty()) {
			ImGui::TextDisabled("%s", Common::Utf16ToUtf8(snapshot.placeholder.c_str()).c_str());
		} else {
			const float  glyph_width = std::max(ImGui::CalcTextSize("M").x, 1.0f);
			const size_t columns     = std::max<size_t>(
			    8, static_cast<size_t>(ImGui::GetContentRegionAvail().x / glyph_width));
			const size_t lines =
			    std::max<size_t>(1, static_cast<size_t>(ImGui::GetContentRegionAvail().y /
			                                            ImGui::GetTextLineHeightWithSpacing()));
			const auto text = VisibleText(snapshot, columns * lines);
			ImGui::TextWrapped("%s", text.c_str());
		}
		ImGui::EndChild();
		ImGui::TextDisabled("%zu / %u", snapshot.text.size(), snapshot.max_text_length);
		ImGui::Separator();

		const float content_width = ImGui::GetContentRegionAvail().x;
		DrawKeyRows(snapshot, content_width);
		if (snapshot.type != Ime::Type::Number) {
			if (ImGui::Button(shift ? "Lower" : "Shift", {84.0f * ui_scale, button_height})) {
				shift = !shift;
			}
			ImGui::SameLine();
			if (ImGui::Button(symbol_mode ? "ABC" : "Symbols", {84.0f * ui_scale, button_height})) {
				symbol_mode = !symbol_mode;
			}
			ImGui::SameLine();
			if (ImGui::Button("Space", {120.0f * ui_scale, button_height})) {
				Ime::HostInsertText(snapshot.generation, u" ");
			}
			ImGui::SameLine();
		}
		const float action_width = snapshot.type == Ime::Type::Number
		                               ? std::max((content_width - 16.0f) / 3.0f, 48.0f)
		                               : 100.0f * ui_scale;
		if (ImGui::Button("Backspace", {action_width, button_height})) {
			Ime::HostBackspace(snapshot.generation);
		}
		ImGui::SameLine();
		if ((snapshot.option & Ime::OPTION_MULTILINE) != 0) {
			if (ImGui::Button("Newline", {action_width, button_height})) {
				Ime::HostInsertText(snapshot.generation, u"\n");
			}
			ImGui::SameLine();
		}
		if (ImGui::Button("Cancel", {action_width, button_height})) {
			Ime::HostCancel(snapshot.generation);
		}
		ImGui::SameLine();
		if (ImGui::Button(EnterLabel(snapshot.enter_label), {action_width, button_height})) {
			Ime::HostAccept(snapshot.generation);
		}
		ImGui::End();
	}

	void DrawDialog(const SystemDialog::HostSnapshot& snapshot, vk::Extent2D extent) {
		const ImVec2 display(static_cast<float>(extent.width), static_cast<float>(extent.height));
		const float  scale = std::max(std::min(display.x / 1280.0f, display.y / 720.0f), 0.5f);
		ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, display,
		                                              IM_COL32(0, 0, 0, 160));
		ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always,
		                        {0.5f, 0.5f});
		ImGui::SetNextWindowSize(
		    {std::max(std::min(620.0f * scale, display.x - 32.0f), 1.0f), 0.0f}, ImGuiCond_Always);
		if (focus_pending) {
			ImGui::SetNextWindowFocus();
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0f * scale, 24.0f * scale});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.0f * scale, 16.0f * scale});
		ImGui::PushFont(nullptr, 20.0f * scale);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings |
		                                   ImGuiWindowFlags_AlwaysAutoResize;
		const bool                 signin = snapshot.kind == SystemDialog::Kind::Signin;
		ImGui::Begin("##SystemDialog", nullptr, flags);
		ImGui::TextUnformatted(signin ? "Sign in to PlayStation Network" : "Error");
		ImGui::Separator();
		const auto error_code = static_cast<uint32_t>(snapshot.error_code);
		ImGui::TextWrapped("%s", signin ? "PlayStation Network sign-in is unavailable."
		                         : error_code == 0x80550006u
		                             ? "You are not signed in to PlayStation Network."
		                             : "An error has occurred.");
		if (!signin) {
			ImGui::TextDisabled("Error code: 0x%08X", error_code);
		}
		const float button_width = std::min(140.0f * scale, ImGui::GetContentRegionAvail().x);
		ImGui::SetCursorPosX((ImGui::GetWindowSize().x - button_width) * 0.5f);
		const bool accepted =
		    ImGui::Button(signin ? "Cancel" : "OK", {button_width, 44.0f * scale}) ||
		    (signin && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
		if (focus_pending) {
			ImGui::SetItemDefaultFocus();
			focus_pending = false;
		}
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar(2);
		if (accepted) {
			SystemDialog::HostClose(snapshot.generation);
		}
	}

	void EmitTouchParticles(ImVec2 from, ImVec2 to, Clock::time_point now) {
		const float dx       = to.x - from.x;
		const float dy       = to.y - from.y;
		const float distance = std::hypot(dx, dy);
		if (distance < 0.01f) {
			return;
		}
		if (distance >= 1.0f) {
			trail_segments.push_back({from, to, now});
			if (trail_segments.size() > MAX_TOUCH_TRAIL_SEGMENTS) {
				trail_segments.erase(trail_segments.begin());
			}
		}

		constexpr float PARTICLE_SPACING = 24.0f;
		float           traveled         = 0.0f;
		while (particle_distance + distance - traveled >= PARTICLE_SPACING) {
			traveled += PARTICLE_SPACING - particle_distance;
			const float fraction = std::clamp(traveled / distance, 0.0f, 1.0f);
			particles.push_back(
			    {{from.x + dx * fraction, from.y + dy * fraction}, now, next_particle_icon});
			next_particle_icon = static_cast<TouchParticleIcon>(
			    (static_cast<uint8_t>(next_particle_icon) + 1) % 4);
			if (particles.size() > MAX_TOUCH_PARTICLES) {
				particles.erase(particles.begin());
			}
			particle_distance = 0.0f;
		}
		particle_distance += distance - traveled;
	}

	void DrawTouchParticles(ImDrawList* draw, Clock::time_point now) {
		const auto skin = GetTouchParticleSkin();
		constexpr float PARTICLE_LIFETIME = 1.0f;
		constexpr float TRAIL_LIFETIME    = 0.35f;
		std::erase_if(trail_segments, [now](const auto& segment) {
			return std::chrono::duration<float>(now - segment.born).count() >= TRAIL_LIFETIME;
		});
		std::erase_if(particles, [now](const auto& particle) {
			return std::chrono::duration<float>(now - particle.born).count() >=
			       PARTICLE_LIFETIME;
		});
		for (const auto& segment: trail_segments) {
			const float age =
			    std::chrono::duration<float>(now - segment.born).count() / TRAIL_LIFETIME;
			const float fade = 1.0f - age;
			if (skin == TouchParticleSkin::RedBlue) {
				draw->AddLine(segment.from, segment.to,
				              IM_COL32(20, 75, 220, static_cast<int>(105.0f * fade)),
				              9.0f * (1.0f - age * 0.5f));
				draw->AddLine(segment.from, segment.to,
				              IM_COL32(255, 45, 65, static_cast<int>(230.0f * fade)),
				              4.0f * (1.0f - age * 0.5f));
			} else if (skin == TouchParticleSkin::Crimson) {
				draw->AddLine(segment.from, segment.to,
				              IM_COL32(110, 0, 8, static_cast<int>(130.0f * fade)),
				              9.0f * (1.0f - age * 0.5f));
				draw->AddLine(segment.from, segment.to,
				              IM_COL32(255, 35, 20, static_cast<int>(235.0f * fade)),
				              4.0f * (1.0f - age * 0.5f));
			} else {
				draw->AddLine(segment.from, segment.to,
				              IM_COL32(35, 145, 255, static_cast<int>(180.0f * fade)),
				              5.0f * (1.0f - age * 0.5f));
			}
		}

		for (const auto& particle: particles) {
			const float age =
			    std::chrono::duration<float>(now - particle.born).count() / PARTICLE_LIFETIME;
			const float alpha = 1.0f - age;
			const float size  = 9.0f * (1.0f - age * 0.35f);
			const float drift = (static_cast<uint8_t>(particle.icon) % 2 == 0 ? -1.0f : 1.0f) *
			                    8.0f * age;
			const ImVec2 center {particle.position.x + drift, particle.position.y - 18.0f * age};
			const auto faded = [alpha, skin, icon = particle.icon](int r, int g, int b) {
				if (skin == TouchParticleSkin::BlueNeon) {
					r = 75;
					g = 220;
					b = 255;
				} else if (skin == TouchParticleSkin::RedBlue) {
					const bool red = icon == TouchParticleIcon::Square ||
					                 icon == TouchParticleIcon::Cross;
					r = red ? 255 : 75;
					g = red ? 65 : 175;
					b = red ? 85 : 255;
				} else if (skin == TouchParticleSkin::Monochrome) {
					r = 235;
					g = 245;
					b = 255;
				} else if (skin == TouchParticleSkin::Crimson) {
					const bool ember = icon == TouchParticleIcon::Triangle ||
					                   icon == TouchParticleIcon::Circle;
					r = 255;
					g = ember ? 205 : 115;
					b = ember ? 70 : 15;
				}
				return IM_COL32(r, g, b, static_cast<int>(255.0f * alpha));
			};

			if (skin != TouchParticleSkin::Classic) {
				const ImU32 glow =
				    skin == TouchParticleSkin::BlueNeon
				        ? IM_COL32(0, 115, 255, static_cast<int>(38.0f * alpha))
				        : (skin == TouchParticleSkin::RedBlue
				               ? IM_COL32(75, 100, 255, static_cast<int>(55.0f * alpha))
				               : (skin == TouchParticleSkin::Crimson
				               ? IM_COL32(255, 70, 0, static_cast<int>(70.0f * alpha))
				               : IM_COL32(115, 165, 255, static_cast<int>(30.0f * alpha))));
				draw->AddCircleFilled(center, size + 6.0f, glow);
			}
			const ImU32 particle_glow =
			    skin == TouchParticleSkin::Crimson
			        ? (particle.icon == TouchParticleIcon::Triangle ||
			                   particle.icon == TouchParticleIcon::Circle
			               ? IM_COL32(255, 195, 30, static_cast<int>(135.0f * alpha))
			               : IM_COL32(255, 95, 0, static_cast<int>(135.0f * alpha)))
			        : (skin == TouchParticleSkin::RedBlue
			               ? (particle.icon == TouchParticleIcon::Square ||
			                          particle.icon == TouchParticleIcon::Cross
			                      ? IM_COL32(255, 40, 65, static_cast<int>(105.0f * alpha))
			                      : IM_COL32(35, 125, 255, static_cast<int>(105.0f * alpha)))
			               : IM_COL32(35, 145, 255,
			                          static_cast<int>((skin == TouchParticleSkin::BlueNeon
			                                                ? 100.0f
			                                                : 65.0f) *
			                                           alpha)));
			draw->AddCircleFilled(center, size + 2.0f, particle_glow);
			switch (particle.icon) {
				case TouchParticleIcon::Square:
					draw->AddRect({center.x - size * 0.65f, center.y - size * 0.65f},
					              {center.x + size * 0.65f, center.y + size * 0.65f},
					              faded(255, 105, 180), 2.0f, 0, 2.2f);
					break;
				case TouchParticleIcon::Triangle:
					draw->AddLine({center.x, center.y - size}, {center.x - size, center.y + size},
					              faded(80, 230, 130), 2.2f);
					draw->AddLine({center.x - size, center.y + size},
					              {center.x + size, center.y + size}, faded(80, 230, 130), 2.2f);
					draw->AddLine({center.x + size, center.y + size},
					              {center.x, center.y - size}, faded(80, 230, 130), 2.2f);
					break;
				case TouchParticleIcon::Cross:
					draw->AddLine({center.x - size * 0.75f, center.y - size * 0.75f},
					              {center.x + size * 0.75f, center.y + size * 0.75f},
					              faded(90, 170, 255), 2.5f);
					draw->AddLine({center.x + size * 0.75f, center.y - size * 0.75f},
					              {center.x - size * 0.75f, center.y + size * 0.75f},
					              faded(90, 170, 255), 2.5f);
					break;
				case TouchParticleIcon::Circle:
					draw->AddCircle(center, size * 0.8f, faded(255, 105, 105), 0, 2.2f);
					break;
			}
		}
	}

	void DrawTouchPadCursor(const HostInputTouchPadVisualState& state) {
		if (!TouchCursorImageEnabled()) {
			particles.clear();
			trail_segments.clear();
			return;
		}
		const auto now = Clock::now();
		if (state.enabled && state.position_valid) {
			const ImVec2 target {state.x * extent.width, state.y * extent.height};
			if (!cursor_initialized || state.gesture != cursor_gesture) {
				cursor_position    = target;
				cursor_initialized = true;
				cursor_gesture     = state.gesture;
				particle_distance  = 0.0f;
			} else {
				const float dt = last_cursor_update == Clock::time_point {}
				                     ? 1.0f / 60.0f
				                     : std::clamp(std::chrono::duration<float>(now -
				                                                             last_cursor_update)
				                                      .count(),
				                                  1.0f / 1000.0f, 0.05f);
				const ImVec2 previous = cursor_position;
				const float  follow   = 1.0f - std::exp(-18.0f * dt);
				cursor_position.x += (target.x - cursor_position.x) * follow;
				cursor_position.y += (target.y - cursor_position.y) * follow;
				if (state.drawing) {
					EmitTouchParticles(previous, cursor_position, now);
				}
			}
			last_cursor_update = now;
		}
		auto* draw = ImGui::GetForegroundDrawList();
		DrawTouchParticles(draw, now);
		if (!state.enabled || !state.cursor_visible ||
		    !state.position_valid || !cursor_initialized) {
			return;
		}

		const ImVec2 position = cursor_position;
		const auto& cursor_image =
		    state.drawing ? drawing_cursor_image : hand_cursor_image;
		if (cursor_image != nullptr) {
			const float scale = TOUCH_CURSOR_IMAGE_SIZE /
			                    static_cast<float>(std::max(cursor_image->Height, 1));
			const ImVec2 size {static_cast<float>(cursor_image->Width) * scale,
			                   static_cast<float>(cursor_image->Height) * scale};
			draw->AddImage(cursor_image->GetTexRef(), position,
			               {position.x + size.x, position.y + size.y});
			return;
		}

		const ImU32  blue     = IM_COL32(35, 145, 255, 220);
		const ImVec2 hand[] = {
		    {position.x, position.y},         {position.x, position.y + 20.0f},
		    {position.x + 4.0f, position.y + 16.0f},
		    {position.x + 9.0f, position.y + 27.0f},
		    {position.x + 13.0f, position.y + 25.0f},
		    {position.x + 10.0f, position.y + 17.0f},
		    {position.x + 18.0f, position.y + 18.0f},
		    {position.x + 20.0f, position.y + 15.0f},
		    {position.x + 19.0f, position.y + 12.0f},
		    {position.x + 12.0f, position.y + 10.0f},
		    {position.x + 12.0f, position.y + 4.0f},
		    {position.x + 10.0f, position.y + 2.0f},
		    {position.x + 8.0f, position.y + 3.0f},
		    {position.x + 8.0f, position.y + 10.0f},
		    {position.x + 6.0f, position.y + 2.0f},
		    {position.x + 4.0f, position.y + 2.0f},
		    {position.x + 3.0f, position.y + 10.0f},
		    {position.x + 2.0f, position.y + 1.0f},
		};
		draw->AddConvexPolyFilled(hand, IM_ARRAYSIZE(hand), IM_COL32(250, 250, 250, 255));
		for (std::size_t i = 0; i < IM_ARRAYSIZE(hand); i++) {
			draw->AddLine(hand[i], hand[(i + 1) % IM_ARRAYSIZE(hand)],
			              IM_COL32(20, 25, 35, 255), 1.5f);
		}
		if (state.drawing) {
			const ImVec2 pen_start {position.x + 12.0f, position.y + 8.0f};
			const ImVec2 pen_end {position.x + 23.0f, position.y - 3.0f};
			draw->AddLine(pen_start, pen_end, IM_COL32(10, 45, 95, 255), 5.0f);
			draw->AddLine(pen_start, pen_end, blue, 2.5f);
		}
	}

	void DrawTrophyNotification(vk::Extent2D frame_extent, Clock::time_point now, float min_top) {
		std::erase_if(retired_trophy_images, [](const auto& texture) {
			if (texture->Status != ImTextureStatus_Destroyed) {
				return false;
			}
			ImGui::UnregisterUserTexture(texture.get());
			return true;
		});
		const float TOAST_LIFETIME = static_cast<float>(Config::GetTrophyDurationSeconds());
		if (trophy_notification &&
		    std::chrono::duration<float>(now - trophy_notification_born).count() >=
		        TOAST_LIFETIME) {
			trophy_notification.reset();
			if (trophy_notification_image) {
				// Keep the texture registered until the backend retires in-flight frames.
				trophy_notification_image->WantDestroyNextFrame = true;
				retired_trophy_images.push_back(std::move(trophy_notification_image));
			}
		}
		if (!trophy_notification) {
			{
				std::scoped_lock lock(g_trophy_notification_mutex);
				if (g_trophy_notifications.empty()) {
					g_trophy_notification_active.store(!retired_trophy_images.empty(),
					                                   std::memory_order_release);
					return;
				}
				trophy_notification = std::move(g_trophy_notifications.front());
				g_trophy_notifications.pop_front();
			}
			trophy_notification_subtitle = PrepareTrophySubtitle(Config::GetConsoleLanguage());
			trophy_notification_born     = now;
			trophy_notification_image    = LoadTrophyIcon(trophy_notification->icon_png);
			PlayTrophySound();
		}
		const float age = std::chrono::duration<float>(now - trophy_notification_born).count();
		const float screen_scale =
		    std::clamp(std::min(static_cast<float>(frame_extent.width) / 1920.0f,
		                        static_cast<float>(frame_extent.height) / 1080.0f),
		               0.65f, 1.25f);
		const float width = std::max(
		    0.0f, std::min(520.0f * screen_scale, static_cast<float>(frame_extent.width) - 32.0f));
		constexpr std::array GRADE_COLORS {
		    IM_COL32(161, 225, 245, 255), IM_COL32(255, 207, 82, 255), IM_COL32(211, 220, 232, 255),
		    IM_COL32(211, 142, 91, 255)};
		const int   grade        = trophy_notification->grade;
		const bool  has_grade    = grade > 0 && grade <= static_cast<int>(GRADE_COLORS.size());
		const float pad          = 10.0f * screen_scale;
		const float base_height  = 104.0f * screen_scale;
		const float icon_size    = base_height - 2.0f * pad;
		const float text_offset  = trophy_notification_image != nullptr
		                               ? pad + icon_size + 18.0f * screen_scale
		                               : 20.0f * screen_scale;
		const float title_offset = text_offset + (has_grade ? 30.0f * screen_scale : 0.0f);
		const float title_size   = 26.0f * screen_scale;
		const float line_height  = 30.0f * screen_scale;
		const float text_padding = 16.0f * screen_scale;
		const auto  title_lines  = WrapTrophyTitle(trophy_notification->name, title_size,
		                                           width - title_offset - text_padding);
		const float block_height =
		    static_cast<float>(title_lines.size()) * line_height + 28.0f * screen_scale;
		const float  height   = std::max(base_height, block_height + 2.0f * text_padding);
		const float  rounding = 14.0f * screen_scale;
		const float  fade_in  = std::clamp(age / 0.22f, 0.0f, 1.0f);
		const float  fade_out = std::clamp((TOAST_LIFETIME - age) / 0.4f, 0.0f, 1.0f);
		const float  alpha    = std::min(fade_in, fade_out);
		const float  slide    = (1.0f - fade_in) * (width + 24.0f);
		const ImVec2 top_left {static_cast<float>(frame_extent.width) - 32.0f - width + slide,
		                       std::max(32.0f, min_top + 12.0f)};
		const ImVec2 bottom_right {top_left.x + width, top_left.y + height};
		auto*        draw = ImGui::GetForegroundDrawList();
		const auto   fade = [alpha](int r, int g, int b, int a) {
            return IM_COL32(r, g, b, static_cast<int>(static_cast<float>(a) * alpha));
		};
		draw->AddRectFilled({top_left.x + 3.0f, top_left.y + 5.0f},
		                    {bottom_right.x + 3.0f, bottom_right.y + 5.0f}, fade(0, 0, 0, 90),
		                    rounding);
		draw->AddRectFilled(top_left, bottom_right, fade(24, 26, 32, 235), rounding);
		draw->AddRect(top_left, bottom_right, fade(255, 255, 255, 28), rounding,
		              ImDrawFlags_RoundCornersAll, 1.0f);

		const float text_left = top_left.x + text_offset;
		if (trophy_notification_image != nullptr) {
			const ImVec2 icon_min {top_left.x + pad, top_left.y + (height - icon_size) * 0.5f};
			const ImVec2 icon_max {icon_min.x + icon_size, icon_min.y + icon_size};
			draw->AddImageRounded(trophy_notification_image->GetTexRef(), icon_min, icon_max,
			                      {0, 0}, {1, 1}, fade(255, 255, 255, 255), 8.0f * screen_scale);
			draw->AddRect(icon_min, icon_max, fade(255, 255, 255, 40), 8.0f * screen_scale,
			              ImDrawFlags_RoundCornersAll, 1.0f);
		}

		const float title_y = top_left.y + (height - block_height) * 0.5f;
		const float title_x = top_left.x + title_offset;
		if (has_grade) {
			// Small cup glyph tinted with the trophy grade.
			const ImU32 cup = ImGui::GetColorU32(GRADE_COLORS[grade - 1], alpha);
			const float u   = screen_scale;
			const float cx  = text_left + 9.0f * u;
			const float cy  = title_y + 15.0f * u;
			draw->AddRectFilled({cx - 8.0f * u, cy - 11.0f * u}, {cx + 8.0f * u, cy + 2.0f * u},
			                    cup, 2.0f * u, ImDrawFlags_RoundCornersBottom);
			draw->AddRect({cx - 11.0f * u, cy - 9.0f * u}, {cx - 6.0f * u, cy - 3.0f * u}, cup,
			              2.0f * u, ImDrawFlags_RoundCornersLeft, 1.8f * u);
			draw->AddRect({cx + 6.0f * u, cy - 9.0f * u}, {cx + 11.0f * u, cy - 3.0f * u}, cup,
			              2.0f * u, ImDrawFlags_RoundCornersRight, 1.8f * u);
			draw->AddRectFilled({cx - 1.5f * u, cy + 2.0f * u}, {cx + 1.5f * u, cy + 7.0f * u}, cup);
			draw->AddRectFilled({cx - 5.0f * u, cy + 7.0f * u}, {cx + 5.0f * u, cy + 10.0f * u}, cup,
			                    1.5f * u);
		}
		draw->PushClipRect(top_left, {bottom_right.x - text_padding, bottom_right.y}, true);
		float line_y = title_y;
		for (const auto line : title_lines) {
			draw->AddText(nullptr, title_size, {title_x, line_y}, fade(255, 255, 255, 255),
			              line.data(), line.data() + line.size());
			line_y += line_height;
		}
		const float subtitle_size = 20.0f * screen_scale;
		const float subtitle_width =
		    ImGui::GetFont()
		        ->CalcTextSizeA(subtitle_size, std::numeric_limits<float>::max(), 0.0f,
		                        trophy_notification_subtitle.c_str())
		        .x;
		const float available_width = std::max(1.0f, width - text_offset - text_padding);
		const float fitted_size = std::max(
		    1.0f, subtitle_size * std::min(1.0f, available_width / std::max(1.0f, subtitle_width)));
		draw->AddText(nullptr, fitted_size, {text_left, line_y + 8.0f * screen_scale},
		              fade(176, 180, 192, 255), trophy_notification_subtitle.c_str());
		draw->PopClipRect();
	}

	bool PrepareFrame(vk::Extent2D frame_extent, vk::Format format, uint32_t image_count) {
		OverlaySnapshot snapshot;
		const bool      has_overlay = GetOverlaySnapshot(&snapshot);
		const auto now = Clock::now();
		if (!has_overlay && !g_trophy_notification_active.load(std::memory_order_acquire) &&
		    !g_splash_active.load(std::memory_order_acquire) &&
		    !GetHostInputTouchPadVisualState().enabled && !PerformanceOverlayEnabled()) {
			return false;
		}
		const auto prepared_session = snapshot.session;
		EnsureVulkan(format, image_count);
		if (session != snapshot.session) {
			session       = snapshot.session;
			focus_pending = true;
			shift         = session.kind == OverlayKind::Ime &&
			                (snapshot.ime.option & Ime::OPTION_NO_AUTO_CAPITALIZE) == 0;
			symbol_mode   = false;
			panel_offset  = {};
			right_stick   = {};
			auto& io      = ImGui::GetIO();
			io.ClearEventsQueue();
			io.ClearInputKeys();
			io.ClearInputMouse();
		}
		DrainInput(snapshot.session);

		auto& io       = ImGui::GetIO();
		io.DisplaySize = {static_cast<float>(frame_extent.width),
		                  static_cast<float>(frame_extent.height)};
		io.DeltaTime   = last_frame == std::chrono::steady_clock::time_point {}
		                     ? 1.0f / 60.0f
		                     : std::clamp(std::chrono::duration<float>(now - last_frame).count(),
		                                  1.0f / 1000.0f, 0.1f);
		last_frame     = now;
		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		DrawSplash(frame_extent);
		const float performance_bottom = DrawPerformanceOverlay(graphics, frame_extent);
		GetOverlaySnapshot(&snapshot);
		if (has_overlay && snapshot.session == prepared_session) {
			if (snapshot.session.kind == OverlayKind::Dialog) {
				DrawDialog(snapshot.dialog, frame_extent);
			} else {
				DrawIme(snapshot.ime, frame_extent);
			}
		}
		DrawTrophyNotification(frame_extent, now, performance_bottom);
		DrawTouchPadCursor(GetHostInputTouchPadVisualState());
		ImGui::Render();
		extent = frame_extent;
		return true;
	}

	void Record(vk::CommandBuffer command, vk::ImageView target) {
		for (const auto& image: splash_dds) {
			if (image) {
				image->Upload(command);
			}
		}
		vk::RenderingAttachmentInfo color {};
		color.sType       = vk::StructureType::eRenderingAttachmentInfo;
		color.imageView   = target;
		color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		color.loadOp      = vk::AttachmentLoadOp::eLoad;
		color.storeOp     = vk::AttachmentStoreOp::eStore;
		vk::RenderingInfo rendering {};
		rendering.sType                = vk::StructureType::eRenderingInfo;
		rendering.renderArea.extent    = extent;
		rendering.layerCount           = 1;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &color;
		command.beginRendering(rendering);
		{
			Common::LockGuard queue_lock(graphics.queue_mutex);
			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
			                                static_cast<VkCommandBuffer>(command));
		}
		command.endRendering();
	}

	void ReleaseVulkan() {
		if (!vulkan_initialized) {
			return;
		}
		ImGui::SetCurrentContext(imgui_context);
		splash_dds = {};
		ImGui_ImplVulkan_Shutdown();
		vulkan_initialized = false;
	}

	GraphicContext&                       graphics;
	ImGuiContext*                         imgui_context      = nullptr;
	bool                                  vulkan_initialized = false;
	bool                                  shift              = false;
	bool                                  symbol_mode        = false;
	bool                                  focus_pending      = true;
	float                                 ui_scale           = 1.0f;
	float                                 button_height      = 42.0f;
	ImVec2                                panel_offset {};
	ImVec2                                right_stick {};
	OverlaySession                              session;
	std::unique_ptr<ImTextureData>              trophy_notification_image;
	std::unique_ptr<ImTextureData>              hand_cursor_image;
	std::unique_ptr<ImTextureData>              drawing_cursor_image;
	bool                                        cursor_initialized = false;
	ImVec2                                      cursor_position {};
	float                                       particle_distance  = 0.0f;
	TouchParticleIcon                           next_particle_icon = TouchParticleIcon::Square;
	Clock::time_point                           last_cursor_update;
	uint64_t                                    cursor_gesture = 0;
	std::vector<TouchParticle>                  particles;
	std::vector<TouchTrailSegment>              trail_segments;
	static constexpr std::size_t                MAX_TOUCH_PARTICLES      = 64;
	static constexpr std::size_t                MAX_TOUCH_TRAIL_SEGMENTS = 128;
	std::optional<TrophyNotification>           trophy_notification;
	std::string                                 trophy_notification_subtitle;
	Clock::time_point                           trophy_notification_born;
	std::vector<std::unique_ptr<ImTextureData>> retired_trophy_images;
	std::array<std::unique_ptr<DdsImage>, 2>      splash_dds;
	Clock::time_point                             splash_born;
	SDL_AudioStream*                            trophy_sound_stream      = nullptr;
	uint8_t*                                    trophy_sound_data        = nullptr;
	uint32_t                                    trophy_sound_size        = 0;
	bool                                        trophy_sound_attempted   = false;
	bool                                        trophy_sound_initialized = false;
	vk::Extent2D                                extent {};
	std::chrono::steady_clock::time_point       last_frame;
};

SystemOverlay::SystemOverlay(GraphicContext& graphics): m_impl(std::make_unique<Impl>(graphics)) {}

SystemOverlay::~SystemOverlay() = default;

bool SystemOverlay::PrepareFrame(vk::Extent2D extent, vk::Format format, uint32_t image_count) {
	return m_impl->PrepareFrame(extent, format, image_count);
}

void SystemOverlay::Record(vk::CommandBuffer command, vk::ImageView target) {
	m_impl->Record(command, target);
}

void SystemOverlay::ReleaseVulkan() {
	m_impl->ReleaseVulkan();
}

} // namespace Libs::Graphics
