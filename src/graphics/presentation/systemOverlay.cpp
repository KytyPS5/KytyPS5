#include "graphics/presentation/systemOverlay.h"

#include <SDL3/SDL.h>

#include "common/assert.h"
#include "common/file.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCompileProgress.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "libs/controller.h"
#include "libs/dialog.h"
#include "libs/ime.h"
#include "libs/imeDialog.h"
#include "loader/systemContent.h"
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

namespace CoreIme     = Libs::Ime;
namespace DialogIme   = Libs::Dialog::ImeDialog;
namespace ErrorDialog = Libs::Dialog::ErrorDialog;

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

enum class OverlayKind : uint8_t { None, Ime, Error };

struct OverlaySession {
	OverlayKind kind       = OverlayKind::None;
	uint64_t    generation = 0;

	bool operator==(const OverlaySession&) const = default;
};

struct OverlaySnapshot {
	OverlaySession            session;
	Ime::HostSnapshot         ime;
	ErrorDialog::HostSnapshot error;
};

bool GetOverlaySnapshot(OverlaySnapshot* snapshot) {
	if (ErrorDialog::GetHostSnapshot(&snapshot->error)) {
		snapshot->session = {OverlayKind::Error, snapshot->error.generation};
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
bool                         g_input_active           = false;
bool                         g_input_controller       = false;
bool                         g_input_keyboard         = false;
bool                         g_input_multiline        = false;
bool                         g_input_lifecycle_active = false;
bool                         g_controller_captured    = false;
OverlaySession               g_session;
SDL_Window*                  g_input_window               = nullptr;

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
	if (snapshot.session.kind == OverlayKind::Error) {
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
	ErrorDialog::SetVisibilityCallback(RefreshVisibility);
	RefreshVisibility();
}

void ShutdownSystemOverlayInput() {
	CoreIme::SetVisibilityCallback(nullptr);
	DialogIme::SetVisibilityCallback(nullptr);
	ErrorDialog::SetVisibilityCallback(nullptr);
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
	const auto core      = CoreIme::GetVisualState();
	const auto dialog    = DialogIme::GetVisualState();
	const auto error     = ErrorDialog::GetVisualState();
	const auto compile   = PipelineCompileProgress::GetSnapshot();
	const bool compiling = PipelineCompileProgress::ShouldShowOverlay(compile);
	return {core.active || dialog.active || error.active || compiling,
	        core.revision + dialog.revision + error.revision +
	            PipelineCompileProgress::OverlayRevision(compile)};
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
	if (keyboard_event && session.kind == OverlayKind::Error) {
		if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
		    (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)) {
			ErrorDialog::HostAccept(generation);
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

struct SystemOverlay::Impl {
	explicit Impl(GraphicContext& context): graphics(context) {}

	~Impl() {
		ReleaseVulkan();
		if (imgui_context != nullptr) {
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
		ImGui::StyleColorsDark();
		auto& style          = ImGui::GetStyle();
		style.WindowRounding = 10.0f;
		style.FrameRounding  = 6.0f;
		style.ItemSpacing    = {8.0f, 8.0f};
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

	void DrawError(const ErrorDialog::HostSnapshot& snapshot, vk::Extent2D extent) {
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
		ImGui::Begin("##SystemError", nullptr, flags);
		ImGui::TextUnformatted("Error");
		ImGui::Separator();
		const auto error_code = static_cast<uint32_t>(snapshot.error_code);
		ImGui::TextWrapped("%s", error_code == 0x80550006u
		                             ? "You are not signed in to PlayStation Network."
		                             : "An error has occurred.");
		ImGui::TextDisabled("Error code: 0x%08X", error_code);
		const float button_width = std::min(140.0f * scale, ImGui::GetContentRegionAvail().x);
		ImGui::SetCursorPosX((ImGui::GetWindowSize().x - button_width) * 0.5f);
		const bool accepted = ImGui::Button("OK", {button_width, 44.0f * scale});
		if (focus_pending) {
			ImGui::SetItemDefaultFocus();
			focus_pending = false;
		}
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar(2);
		if (accepted) {
			ErrorDialog::HostAccept(snapshot.generation);
		}
	}

	// Deliberately independent of OverlaySession/OverlayKind: unlike Ime/Error, this has no
	// guest-driven modal state, needs no input capture, and should never compete with either of
	// them for exclusivity -- it's an ambient corner HUD, not a dialog.
	// ImGui's built-in bitmap font pixelates badly at panel sizes and the project ships no font of
	// its own, so borrow the platform's. Falls back to the built-in face when none is found.
	// ImGui's built-in bitmap font pixelates badly at panel sizes and the project ships no font of
	// its own, so borrow the platform's. Falls back to the built-in face when none is found.
	// ImGui's built-in bitmap font pixelates badly at panel sizes and the project ships no font of
	// its own, so borrow the platform's. Falls back to the built-in face when none is found.
	ImFont* LoadOverlayFont() {
		static ImFont* font = []() -> ImFont* {
			static const char* const candidates[] = {
			    "C:/Windows/Fonts/segoeui.ttf",
			    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
			    "/System/Library/Fonts/Helvetica.ttc",
			};
			for (const char* path: candidates) {
				std::error_code ec;
				if (std::filesystem::exists(path, ec)) {
					return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 32.0f);
				}
			}
			return nullptr;
		}();
		return font;
	}

	struct OverlayIcon {
		ImFontAtlasRectId id = ImFontAtlasRectId_Invalid;
	};

	// stbi_load() opens the path itself through fopen(), which on Windows resolves it in the ANSI
	// code page and fails for any title whose icon sits behind a non-ASCII path. Common::File goes
	// through CreateFileW, so the bytes are read here and decoded from memory instead.
	static std::vector<uint8_t> ReadImageBytes(const std::filesystem::path& path) {
		Common::File file;
		if (!file.Open(path, Common::File::Mode::Read)) {
			return {};
		}
		std::vector<uint8_t> bytes(static_cast<size_t>(file.Size()));
		uint32_t             read = 0;
		file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
		file.Close();
		bytes.resize(read);
		return bytes;
	}

	// Packed into the font atlas rather than uploaded as its own image: no VkImage, no staging
	// buffer and nothing to release. The pixels would be lost if the atlas were ever rebuilt, which
	// cannot happen here because the font is loaded once at startup and never changes.
	OverlayIcon LoadGameIcon() {
		static OverlayIcon icon = []() -> OverlayIcon {
			std::filesystem::path path;
			if (!Loader::SystemContentGetIconPath(&path) || path.empty()) {
				return {};
			}
			const auto     png    = ReadImageBytes(path);
			int            w      = 0;
			int            h      = 0;
			int            comp   = 0;
			unsigned char* pixels =
			    stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &comp, 4);
			if (pixels == nullptr || w <= 0 || h <= 0) {
				stbi_image_free(pixels);
				return {};
			}
			ImFontAtlas*            atlas = ImGui::GetIO().Fonts;
			const ImFontAtlasRectId id    = atlas->AddCustomRect(w, h);
			ImFontAtlasRect         rect {};
			if (id == ImFontAtlasRectId_Invalid || !atlas->GetCustomRect(id, &rect) ||
			    atlas->TexData == nullptr || atlas->TexData->BytesPerPixel != 4) {
				stbi_image_free(pixels);
				return {};
			}
			for (int y = 0; y < h; y++) {
				std::memcpy(atlas->TexData->GetPixelsAt(rect.x, rect.y + y),
				            pixels + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u,
				            static_cast<size_t>(w) * 4u);
			}
			stbi_image_free(pixels);
			return {id};
		}();
		return icon;
	}

	// pic0.png is the key art the launcher already shows; downscaled before packing so it cannot
	// blow up the font atlas that carries it.
	static ImFontAtlasRectId PackPng(const std::filesystem::path& path, int max_dim, int* out_w,
	                                 int* out_h) {
		const auto     png    = ReadImageBytes(path);
		int            w      = 0;
		int            h      = 0;
		int            comp   = 0;
		unsigned char* pixels =
		    stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &comp, 4);
		if (pixels == nullptr || w <= 0 || h <= 0) {
			stbi_image_free(pixels);
			return ImFontAtlasRectId_Invalid;
		}
		int step = 1;
		while (w / step > max_dim || h / step > max_dim) {
			step++;
		}
		const int               dw    = w / step;
		const int               dh    = h / step;
		ImFontAtlas*            atlas = ImGui::GetIO().Fonts;
		const ImFontAtlasRectId id    = atlas->AddCustomRect(dw, dh);
		ImFontAtlasRect         rect {};
		if (id == ImFontAtlasRectId_Invalid || !atlas->GetCustomRect(id, &rect) ||
		    atlas->TexData == nullptr || atlas->TexData->BytesPerPixel != 4) {
			stbi_image_free(pixels);
			return ImFontAtlasRectId_Invalid;
		}
		for (int y = 0; y < dh; y++) {
			auto* dst =
			    static_cast<unsigned char*>(atlas->TexData->GetPixelsAt(rect.x, rect.y + y));
			for (int x = 0; x < dw; x++) {
				unsigned int acc[4] = {0, 0, 0, 0};
				for (int sy = 0; sy < step; sy++) {
					for (int sx = 0; sx < step; sx++) {
						const unsigned char* src =
						    pixels + ((static_cast<size_t>(y) * step + sy) * w +
						              (static_cast<size_t>(x) * step + sx)) *
						                 4u;
						for (int ch = 0; ch < 4; ch++) {
							acc[ch] += src[ch];
						}
					}
				}
				const unsigned int n =
				    static_cast<unsigned int>(step) * static_cast<unsigned int>(step);
				for (int ch = 0; ch < 4; ch++) {
					dst[x * 4 + ch] = static_cast<unsigned char>(acc[ch] / n);
				}
			}
		}
		stbi_image_free(pixels);
		if (out_w != nullptr) {
			*out_w = dw;
		}
		if (out_h != nullptr) {
			*out_h = dh;
		}
		return id;
	}

	static ImFontAtlasRectId LoadGameBanner() {
		static ImFontAtlasRectId id = []() -> ImFontAtlasRectId {
			std::filesystem::path icon_path;
			if (!Loader::SystemContentGetIconPath(&icon_path) || icon_path.empty()) {
				return ImFontAtlasRectId_Invalid;
			}
			const auto      banner = icon_path.parent_path() / "pic0.png";
			std::error_code ec;
			if (!std::filesystem::exists(banner, ec)) {
				return ImFontAtlasRectId_Invalid;
			}
			return PackPng(banner, 1024, nullptr, nullptr);
		}();
		return id;
	}
	std::string GroupDigits(uint64_t value) {
		const std::string digits = std::to_string(value);
		std::string       out;
		out.reserve(digits.size() + digits.size() / 3);
		for (size_t i = 0; i < digits.size(); i++) {
			if (i != 0 && (digits.size() - i) % 3 == 0) {
				out.push_back(',');
			}
			out.push_back(digits[i]);
		}
		return out;
	}

	void DrawCompileProgress(const PipelineCompileProgress::Snapshot& progress,
	                         vk::Extent2D                             frame_extent) {
		const ImVec2  display(static_cast<float>(frame_extent.width),
		                      static_cast<float>(frame_extent.height));
		const float   scale = std::max(std::min(display.x / 1280.0f, display.y / 720.0f), 0.5f);
		ImFont* const font  = LoadOverlayFont();
		const ImVec4  dim {0.68f, 0.70f, 0.74f, 1.0f};
		const ImVec4  faint {0.55f, 0.57f, 0.61f, 1.0f};
		const ImVec4  white {1.0f, 1.0f, 1.0f, 1.0f};

		const ImFontAtlasRectId banner = LoadGameBanner();
		ImFontAtlasRect         banner_rect {};
		if (banner != ImFontAtlasRectId_Invalid &&
		    ImGui::GetIO().Fonts->GetCustomRect(banner, &banner_rect)) {
			ImGui::GetBackgroundDrawList()->AddImage(ImGui::GetIO().Fonts->TexRef,
			                                         ImVec2(0.0f, 0.0f), display, banner_rect.uv0,
			                                         banner_rect.uv1);
			ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), display,
			                                              IM_COL32(0, 0, 0, 90));
		}
		ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always,
		                        {0.5f, 0.5f});
		ImGui::SetNextWindowSize({720.0f * scale, 0.0f}, ImGuiCond_Always);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {26.0f * scale, 26.0f * scale});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 8.0f * scale});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18.0f * scale);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * scale);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.11f, 0.12f, 0.90f));
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.62f, 0.78f, 0.95f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings |
		                                   ImGuiWindowFlags_NoInputs;
		ImGui::Begin("##Loading", nullptr, flags);

		const OverlayIcon icon = LoadGameIcon();
		ImFontAtlasRect   icon_rect {};
		if (icon.id != ImFontAtlasRectId_Invalid &&
		    ImGui::GetIO().Fonts->GetCustomRect(icon.id, &icon_rect)) {
			const float side = 134.0f * scale;
			ImGui::Image(ImGui::GetIO().Fonts->TexRef, ImVec2(side, side), icon_rect.uv0,
			             icon_rect.uv1);
			ImGui::SameLine(0.0f, 22.0f * scale);
		}

		ImGui::BeginGroup();
		const float content_w = ImGui::GetContentRegionAvail().x;

		ImGui::PushFont(font, 30.0f * scale);
		ImGui::TextColored(white, "%s",
		                   progress.title.empty() ? "Loading" : progress.title.c_str());
		ImGui::PopFont();

		ImGui::PushFont(font, 19.0f * scale);
		ImGui::TextColored(dim, "Preparing shaders");
		ImGui::PopFont();

		ImGui::Dummy(ImVec2(0.0f, 6.0f * scale));

		const bool     have_total = progress.estimated_total > 0;
		const uint64_t done       = PipelineCompileProgress::OverlayCompletedOfTotal(progress);
		ImGui::PushFont(font, 22.0f * scale);
		if (have_total) {
			const std::string counts =
			    GroupDigits(done) + "  /  " + GroupDigits(progress.estimated_total);
			ImGui::TextColored(white, "%s", counts.c_str());
			const int percent = static_cast<int>(done * 100u / progress.estimated_total);
			const std::string pct   = std::to_string(percent) + "%";
			const float       pct_w = ImGui::CalcTextSize(pct.c_str()).x;
			ImGui::SameLine();
			ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + content_w - pct_w);
			ImGui::TextColored(dim, "%s", pct.c_str());
		} else {
			ImGui::TextColored(white, "%s", GroupDigits(progress.compiled).c_str());
		}
		ImGui::PopFont();

		// A cold run has no idea how many shaders the title needs, so an indeterminate sweep is the
		// only honest bar; ImGui draws one for any negative fraction.
		const float fraction = have_total ? static_cast<float>(done) /
		                                        static_cast<float>(progress.estimated_total)
		                                  : -1.0f * static_cast<float>(ImGui::GetTime());
		ImGui::ProgressBar(fraction, ImVec2(content_w, 8.0f * scale), "");

		ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));
		ImGui::PushFont(font, 17.0f * scale);
		ImGui::TextColored(dim, "Compiling GPU pipelines");
		if (have_total && done > 0 && progress.compile_elapsed_ms > 0) {
			const uint64_t remaining_ms =
			    progress.compile_elapsed_ms * (progress.estimated_total - done) / done;
			const uint64_t seconds = remaining_ms / 1000u;
			if (seconds >= 60u) {
				ImGui::TextColored(faint, "Shader cache  -  %llum %llus remaining",
				                   static_cast<unsigned long long>(seconds / 60u),
				                   static_cast<unsigned long long>(seconds % 60u));
			} else {
				ImGui::TextColored(faint, "Shader cache  -  %llus remaining",
				                   static_cast<unsigned long long>(seconds));
			}
		} else {
			ImGui::TextColored(faint, "Shader cache  -  building");
		}
		ImGui::PopFont();
		ImGui::EndGroup();

		ImGui::End();
		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(4);
	}
	bool PrepareFrame(vk::Extent2D frame_extent, vk::Format format, uint32_t image_count) {
		OverlaySnapshot snapshot;
		const bool      has_dialog   = GetOverlaySnapshot(&snapshot);
		const auto      compile      = PipelineCompileProgress::GetSnapshot();
		const bool      show_compile = PipelineCompileProgress::ShouldShowOverlay(compile);
		if (!has_dialog && !show_compile) {
			return false;
		}
		const auto prepared_session = snapshot.session;
		EnsureVulkan(format, image_count);
		if (has_dialog) {
			if (session != snapshot.session) {
				session       = snapshot.session;
				focus_pending = true;
				shift         = (snapshot.ime.option & Ime::OPTION_NO_AUTO_CAPITALIZE) == 0;
				symbol_mode   = false;
				panel_offset  = {};
				right_stick   = {};
				auto& io      = ImGui::GetIO();
				io.ClearEventsQueue();
				io.ClearInputKeys();
				io.ClearInputMouse();
			}
			DrainInput(snapshot.session);
		}

		auto& io       = ImGui::GetIO();
		io.DisplaySize = {static_cast<float>(frame_extent.width),
		                  static_cast<float>(frame_extent.height)};
		const auto now = std::chrono::steady_clock::now();
		io.DeltaTime   = last_frame == std::chrono::steady_clock::time_point {}
		                     ? 1.0f / 60.0f
		                     : std::clamp(std::chrono::duration<float>(now - last_frame).count(),
		                                  1.0f / 1000.0f, 0.1f);
		last_frame     = now;
		LoadOverlayFont(); // atlas changes must land outside a frame
		LoadGameIcon();
		LoadGameBanner(); // before NewFrame
		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		const bool dialog_still_valid =
		    has_dialog && GetOverlaySnapshot(&snapshot) && snapshot.session == prepared_session;
		if (dialog_still_valid) {
			if (snapshot.session.kind == OverlayKind::Error) {
				DrawError(snapshot.error, frame_extent);
			} else {
				DrawIme(snapshot.ime, frame_extent);
			}
		}
		if (show_compile) {
			DrawCompileProgress(compile, frame_extent);
		}
		if (!dialog_still_valid && !show_compile) {
			ImGui::EndFrame();
			return false;
		}
		ImGui::Render();
		extent = frame_extent;
		return true;
	}

	void Record(vk::CommandBuffer command, vk::ImageView target) {
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
	OverlaySession                        session;
	vk::Extent2D                          extent {};
	std::chrono::steady_clock::time_point last_frame;
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
