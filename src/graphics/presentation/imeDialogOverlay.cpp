#include "graphics/presentation/imeDialogOverlay.h"

#include "SDL.h"
#include "common/assert.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/graphicContext.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "libs/controller.h"
#include "libs/imeDialog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Libs::Graphics {

namespace {

namespace Ime = Libs::Dialog::ImeDialog;

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
	InputKind kind;
	uint64_t  generation;
	int       id;
	float     x;
	float     y;
};

struct VisibilityUpdate {
	uint64_t generation;
	bool     visible;
	bool     capture_controller;
	bool     capture_keyboard;
	bool     multiline;
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
uint64_t                     g_input_generation           = 0;
bool                         g_input_active               = false;
bool                         g_input_controller           = false;
bool                         g_input_keyboard             = false;
bool                         g_input_multiline            = false;
bool                         g_input_lifecycle_active     = false;
bool                         g_active                     = false;
bool                         g_controller_captured        = false;
uint64_t                     g_generation                 = 0;

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
		if (last.generation == event.generation && last.kind == event.kind && last.id == event.id) {
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
	if (SDL_PushEvent(&event) > 0) {
		g_missing_visibility_wakeups--;
	}
}

void OnVisibilityChanged(bool visible, uint64_t generation) {
	std::scoped_lock visibility_lock(g_visibility_mutex);
	if (!g_input_lifecycle_active) {
		return;
	}
	if (generation < g_generation || (generation == g_generation && visible == g_active)) {
		return;
	}

	g_generation            = generation;
	bool capture_controller = false;
	bool capture_keyboard   = false;
	bool multiline          = false;
	if (visible) {
		Ime::HostSnapshot snapshot;
		if (Ime::GetHostSnapshot(&snapshot) && snapshot.generation == generation) {
			capture_controller = (snapshot.disable_device & Ime::DISABLE_DEVICE_CONTROLLER) == 0;
			capture_keyboard   = (snapshot.disable_device & Ime::DISABLE_DEVICE_EXT_KEYBOARD) == 0;
			multiline          = (snapshot.option & Ime::OPTION_MULTILINE) != 0;
		} else {
			visible = false;
		}
	}
	const bool was_controller = std::exchange(g_controller_captured, capture_controller);
	if (capture_controller || was_controller) {
		Controller::ControllerResetInputState();
	}
	g_active = visible;

	const Uint32 type = g_visibility_event.load(std::memory_order_acquire);
	if (type != static_cast<Uint32>(-1)) {
		SDL_Event event {};
		event.type = type;
		std::scoped_lock input_lock(g_input_mutex);
		g_visibility_updates.push_back(
		    {generation, visible, capture_controller, capture_keyboard, multiline});
		if (g_missing_visibility_wakeups != 0 || SDL_PushEvent(&event) <= 0) {
			g_missing_visibility_wakeups++;
		}
	}
}

uint32_t ExternalKeyStatus(SDL_Keymod modifiers, bool character_valid) {
	uint32_t status = 0x00000001 | (character_valid ? 0x00000002 : 0);
	if ((modifiers & KMOD_LCTRL) != 0) status |= 0x00000100;
	if ((modifiers & KMOD_LSHIFT) != 0) status |= 0x00000200;
	if ((modifiers & KMOD_LALT) != 0) status |= 0x00000400;
	if ((modifiers & KMOD_LGUI) != 0) status |= 0x00000800;
	if ((modifiers & KMOD_RCTRL) != 0) status |= 0x00001000;
	if ((modifiers & KMOD_RSHIFT) != 0) status |= 0x00002000;
	if ((modifiers & KMOD_RALT) != 0) status |= 0x00004000;
	if ((modifiers & KMOD_RGUI) != 0) status |= 0x00008000;
	if ((modifiers & KMOD_NUM) != 0) status |= 0x00010000;
	if ((modifiers & KMOD_CAPS) != 0) status |= 0x00020000;
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
		case SDL_CONTROLLER_BUTTON_A: return ImGuiKey_GamepadFaceDown;
		case SDL_CONTROLLER_BUTTON_B: return ImGuiKey_GamepadFaceRight;
		case SDL_CONTROLLER_BUTTON_X: return ImGuiKey_GamepadFaceLeft;
		case SDL_CONTROLLER_BUTTON_Y: return ImGuiKey_GamepadFaceUp;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
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

void InitializeImeDialogInput() {
	const Uint32 type = SDL_RegisterEvents(1);
	EXIT_IF(type == static_cast<Uint32>(-1));
	{
		std::scoped_lock lock(g_visibility_mutex);
		g_input_lifecycle_active = true;
		g_visibility_event.store(type, std::memory_order_release);
	}
	Ime::SetVisibilityCallback(OnVisibilityChanged);
	Ime::HostSnapshot snapshot;
	if (Ime::GetHostSnapshot(&snapshot)) {
		OnVisibilityChanged(true, snapshot.generation);
	}
}

void ShutdownImeDialogInput() {
	Ime::SetVisibilityCallback(nullptr);
	{
		std::scoped_lock lock(g_visibility_mutex);
		g_input_lifecycle_active = false;
		g_active                 = false;
		if (std::exchange(g_controller_captured, false)) {
			Controller::ControllerResetInputState();
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
	g_input_controller = false;
	g_input_keyboard   = false;
	g_input_multiline  = false;
	if (SDL_IsTextInputActive() == SDL_TRUE) {
		SDL_StopTextInput();
	}
}

bool ProcessImeDialogInput(const SDL_Event& event) {
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
		g_input_generation      = update.generation;
		g_input_active          = update.visible;
		g_input_controller      = update.capture_controller;
		g_input_keyboard        = update.capture_keyboard;
		g_input_multiline       = update.multiline;
		if (g_input_keyboard) {
			SDL_StartTextInput();
		} else if (SDL_IsTextInputActive() == SDL_TRUE) {
			SDL_StopTextInput();
		}
		return true;
	}
	if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
		if (g_input_active && g_input_controller) {
			QueueInput({InputKind::ResetController, g_input_generation, 0, 0.0f, 0.0f});
		}
		return false;
	}
	if (!g_input_active) {
		return false;
	}

	const uint64_t generation       = g_input_generation;
	const bool     controller_event = event.type == SDL_CONTROLLERBUTTONDOWN ||
	                                  event.type == SDL_CONTROLLERBUTTONUP ||
	                                  event.type == SDL_CONTROLLERAXISMOTION;
	if (controller_event && !g_input_controller) {
		return false;
	}
	const bool keyboard_event = event.type == SDL_TEXTINPUT || event.type == SDL_TEXTEDITING ||
	                            event.type == SDL_KEYDOWN || event.type == SDL_KEYUP;
	if (keyboard_event && !g_input_keyboard) {
		return false;
	}
	switch (event.type) {
		case SDL_TEXTINPUT: {
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
		case SDL_TEXTEDITING: return true;
		case SDL_KEYDOWN: {
			g_last_external_keycode = static_cast<uint16_t>(event.key.keysym.scancode);
			g_last_external_status =
			    ExternalKeyStatus(static_cast<SDL_Keymod>(event.key.keysym.mod), false);
			auto action = Ime::ExternalAction::Text;
			bool queue  = true;
			if (event.key.keysym.sym == SDLK_BACKSPACE) {
				action = Ime::ExternalAction::Backspace;
			} else if (event.key.keysym.sym == SDLK_LEFT) {
				action = Ime::ExternalAction::MoveLeft;
			} else if (event.key.keysym.sym == SDLK_RIGHT) {
				action = Ime::ExternalAction::MoveRight;
			} else if (event.key.keysym.sym == SDLK_ESCAPE) {
				action = Ime::ExternalAction::Cancel;
			} else if (event.key.keysym.sym == SDLK_RETURN ||
			           event.key.keysym.sym == SDLK_KP_ENTER) {
				action =
				    g_input_multiline ? Ime::ExternalAction::Newline : Ime::ExternalAction::Accept;
			} else if (event.key.keysym.sym == SDLK_TAB) {
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
		case SDL_KEYUP: return true;
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
			QueueInput({InputKind::Button, generation, event.cbutton.button,
			            event.type == SDL_CONTROLLERBUTTONDOWN ? 1.0f : 0.0f, 0.0f});
			return true;
		case SDL_CONTROLLERAXISMOTION:
			QueueInput({InputKind::Axis, generation, event.caxis.axis,
			            static_cast<float>(event.caxis.value), 0.0f});
			return true;
		case SDL_MOUSEMOTION:
			QueueInput({InputKind::MousePosition, generation, 0, static_cast<float>(event.motion.x),
			            static_cast<float>(event.motion.y)});
			return true;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			QueueInput({InputKind::MouseButton, generation, event.button.button,
			            event.type == SDL_MOUSEBUTTONDOWN ? 1.0f : 0.0f, 0.0f});
			return true;
		case SDL_MOUSEWHEEL:
			QueueInput({InputKind::MouseWheel, generation, 0, static_cast<float>(event.wheel.x),
			            static_cast<float>(event.wheel.y)});
			return true;
		default: return false;
	}
}

struct ImeDialogOverlay::Impl {
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
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		io.BackendPlatformName = "Kyty ImeDialog input";
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

	void DrainInput(uint64_t generation) {
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
			if (event.generation != generation) {
				continue;
			}
			switch (event.kind) {
				case InputKind::Button: {
					const bool down = event.x != 0.0f;
					if (down && event.id == SDL_CONTROLLER_BUTTON_B) {
						Ime::HostCancel(generation);
					} else if (down && event.id == SDL_CONTROLLER_BUTTON_Y) {
						Ime::HostBackspace(generation);
					}
					const ImGuiKey key = ControllerButtonToKey(event.id);
					if (key != ImGuiKey_None) {
						io.AddKeyEvent(key, down);
					}
					break;
				}
				case InputKind::Axis: {
					const float value = event.x / (event.x < 0.0f ? 32768.0f : 32767.0f);
					if (event.id == SDL_CONTROLLER_AXIS_LEFTX) {
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, value < -0.25f,
						                     std::max(-value, 0.0f));
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, value > 0.25f,
						                     std::max(value, 0.0f));
					} else if (event.id == SDL_CONTROLLER_AXIS_LEFTY) {
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, value < -0.25f,
						                     std::max(-value, 0.0f));
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, value > 0.25f,
						                     std::max(value, 0.0f));
					} else if (event.id == SDL_CONTROLLER_AXIS_RIGHTX) {
						right_stick.x = std::abs(value) > 0.2f ? value : 0.0f;
					} else if (event.id == SDL_CONTROLLER_AXIS_RIGHTY) {
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

	void DrawDialog(const Ime::HostSnapshot& snapshot, vk::Extent2D extent) {
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
		ImGui::Begin("##ImeDialog", nullptr, flags);

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

	bool PrepareFrame(vk::Extent2D frame_extent, vk::Format format, uint32_t image_count) {
		Ime::HostSnapshot snapshot;
		if (!Ime::GetHostSnapshot(&snapshot)) {
			return false;
		}
		const uint64_t prepared_generation = snapshot.generation;
		EnsureVulkan(format, image_count);
		if (generation != snapshot.generation) {
			generation    = snapshot.generation;
			focus_pending = true;
			shift         = (snapshot.option & Ime::OPTION_NO_AUTO_CAPITALIZE) == 0;
			symbol_mode   = false;
			panel_offset  = {};
			right_stick   = {};
			auto& io      = ImGui::GetIO();
			io.ClearEventsQueue();
			io.ClearInputKeys();
			io.ClearInputMouse();
		}
		DrainInput(snapshot.generation);

		auto& io       = ImGui::GetIO();
		io.DisplaySize = {static_cast<float>(frame_extent.width),
		                  static_cast<float>(frame_extent.height)};
		const auto now = std::chrono::steady_clock::now();
		io.DeltaTime   = last_frame == std::chrono::steady_clock::time_point {}
		                     ? 1.0f / 60.0f
		                     : std::clamp(std::chrono::duration<float>(now - last_frame).count(),
		                                  1.0f / 1000.0f, 0.1f);
		last_frame     = now;
		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		if (!Ime::GetHostSnapshot(&snapshot) || snapshot.generation != prepared_generation) {
			ImGui::EndFrame();
			return false;
		}
		DrawDialog(snapshot, frame_extent);
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
	uint64_t                              generation = 0;
	vk::Extent2D                          extent {};
	std::chrono::steady_clock::time_point last_frame;
};

ImeDialogOverlay::ImeDialogOverlay(GraphicContext& graphics)
    : m_impl(std::make_unique<Impl>(graphics)) {}

ImeDialogOverlay::~ImeDialogOverlay() = default;

bool ImeDialogOverlay::PrepareFrame(vk::Extent2D extent, vk::Format format, uint32_t image_count) {
	return m_impl->PrepareFrame(extent, format, image_count);
}

void ImeDialogOverlay::Record(vk::CommandBuffer command, vk::ImageView target) {
	m_impl->Record(command, target);
}

void ImeDialogOverlay::ReleaseVulkan() {
	m_impl->ReleaseVulkan();
}

} // namespace Libs::Graphics
