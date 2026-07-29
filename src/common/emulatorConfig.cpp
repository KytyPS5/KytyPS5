#include "common/emulatorConfig.h"

#include "common/assert.h"

#include <algorithm>
#include <memory>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

KYTY_SUBSYSTEM_INIT(Config) {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Config) {}

KYTY_SUBSYSTEM_DESTROY(Config) {}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);

	*g_config = cfg;
}

uint32_t GetScreenWidth() {
	return g_config->screen_width;
}

uint32_t GetScreenHeight() {
	return g_config->screen_height;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_config->vblank_frequency, 30u, 360u);
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

ShaderLogDirection GetShaderLogDirection() {
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

OutputDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

ProfilerDirection GetProfilerDirection() {
	return g_config->profiler_direction;
}

bool SpirvDebugPrintfEnabled() {
	return g_config->spirv_debug_printf_enabled;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool NggRectlistDrawEnabled() {
	return g_config->ngg_rectlist_draw_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return g_config->readback_linear_images;
}

const std::unordered_map<std::string, std::string>& GetKeymap() {
	static std::unordered_map<std::string, std::string> parsed;
	// Parse lazily on first access (and reparse if the underlying string
	// changed). The emulator process is single-threaded for config access.
	static std::string last_raw;
	if (parsed.empty() || last_raw != g_config->keymap) {
		parsed.clear();
		last_raw = g_config->keymap;
		size_t i = 0;
		while (i < last_raw.size()) {
			size_t semi = last_raw.find(';', i);
			if (semi == std::string::npos) {
				semi = last_raw.size();
			}
			auto token = last_raw.substr(i, semi - i);
			auto eq = token.find('=');
			if (eq != std::string::npos && eq + 1 < token.size()) {
				parsed[token.substr(0, eq)] = token.substr(eq + 1);
			}
			i = semi + 1;
		}
	}
	return parsed;
}

bool KeymapIsActive() {
	return g_config != nullptr && !g_config->keymap.empty();
}

} // namespace Config
