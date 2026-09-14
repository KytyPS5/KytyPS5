#include "common/emulatorConfig.h"

#include "common/assert.h"

#include <algorithm>
#include <array>
#include <memory>

namespace Config {

namespace {
std::unique_ptr<ConfigOptions> g_config;
} // namespace

bool ValidateKeymapEntry(const std::string& entry, std::string* error) {
	if (entry.starts_with("MouseSensitivity=")) {
		return true;
	}
	const auto split = entry.find('=');
	if (split == std::string::npos || split == 0 || split + 1 == entry.size()) {
		if (error != nullptr) {
			*error = "expected Control=Input";
		}
		return false;
	}
	static constexpr std::array kControls = {
	    "L3",
	    "R3",
	    "Options",
	    "Up",
	    "Right",
	    "Down",
	    "Left",
	    "L2",
	    "R2",
	    "L1",
	    "R1",
	    "Triangle",
	    "Circle",
	    "Cross",
	    "Square",
	    "TouchPad",
	    "TouchPadRight",
	    "LeftStickLeft",
	    "LeftStickRight",
	    "LeftStickUp",
	    "LeftStickDown",
	    "RightStickLeft",
	    "RightStickRight",
	    "RightStickUp",
	    "RightStickDown",
	    "AnalogModifier",
	    "AnalogStepDown",
	    "AnalogStepMiddle",
	    "AnalogStepUp",
	    "AnalogLock",
	    "Gyro",
	};
	const auto control = entry.substr(0, split);
	if (std::find(kControls.begin(), kControls.end(), control) == kControls.end()) {
		if (error != nullptr) {
			*error = "unknown DualSense control: " + control;
		}
		return false;
	}
	return true;
}

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

void Shutdown() {
	g_config.reset();
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));

	*g_config = cfg;
}

uint32_t GetScreenWidth() {
	return g_config->screen_width;
}

uint32_t GetScreenHeight() {
	return g_config->screen_height;
}

const std::string& GetUserName() {
	return g_config->user_name;
}

int32_t GetUserId() {
	return g_config->user_id;
}

PresentMode GetPresentMode() {
	return g_config->present_mode;
}

int32_t GetGpuIndex() {
	return g_config->gpu_index;
}

bool FullscreenEnabled() {
	return g_config->fullscreen_enabled;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_config->vblank_frequency, 30u, 360u);
}

uint32_t GetConsoleLanguage() {
	return g_config->console_language;
}

bool VulkanValidationEnabled() {
	return g_config->vulkan_validation_enabled;
}

bool ShaderValidationEnabled() {
	return g_config->shader_validation_enabled;
}

ShaderOptimizationType GetShaderOptimizationType() {
	return g_config->shader_optimization_type;
}

LogDirection GetShaderLogDirection() {
	return g_config->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return g_config->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return g_config->command_buffer_dump_folder;
}

bool GraphicsDebugDumpEnabled() {
	return g_config->graphics_debug_dump_enabled;
}

LogDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

bool ProfilerEnabled() {
	return g_config->profiler_enabled;
}

bool SpirvDebugPrintfEnabled() {
	return g_config->spirv_debug_printf_enabled;
}

bool GpuAssistedValidationEnabled() {
	return g_config->gpu_assisted_validation_enabled && g_config->vulkan_validation_enabled;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return g_config->readback_linear_images;
}

bool PlayGoHackEnabled() {
	return g_config->playgo_hack_enabled;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return g_config->red_zone_protection_enabled;
}
#endif

const Keymap& GetKeymap() {
	return g_config->keymap;
}

const std::string& GetInputProfile() {
	return g_config->input_profile;
}

bool InputFeedbackEnabled() {
	return g_config->input_feedback_enabled;
}
bool InputFeedbackFrameEnabled() {
	return g_config->input_feedback_frame;
}
bool InputFeedbackGyroEnabled() {
	return g_config->input_feedback_gyro;
}
bool InputFeedbackMicrophoneEnabled() {
	return g_config->input_feedback_microphone;
}
bool InputFeedbackLockEnabled() {
	return g_config->input_feedback_lock;
}
bool InputFeedbackLabelsEnabled() {
	return g_config->input_feedback_labels;
}
bool InputFeedbackVibrationEnabled() {
	return g_config->input_feedback_vibration;
}
bool InputDebugLogEnabled() {
	return g_config->input_debug_log;
}

} // namespace Config
