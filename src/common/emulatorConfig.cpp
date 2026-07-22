#include "common/emulatorConfig.h"

#include "common/assert.h"
#include "common/outputReservation.h"

#include <algorithm>
#include <cctype>
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

namespace {

std::string ComparablePath(const std::filesystem::path& path) {
	std::error_code error;
	auto normalized = std::filesystem::weakly_canonical(path, error);
	if (error) {
		normalized = std::filesystem::absolute(path, error);
		if (error) {
			normalized = path;
		}
	}
	auto text = normalized.lexically_normal().generic_string();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::transform(text.begin(), text.end(), text.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
	return text;
}

std::filesystem::path FrameDirectory(const std::filesystem::path& manifest) {
	return manifest.parent_path() / (manifest.filename().string() + "_frames");
}

bool SameOrWithin(const std::filesystem::path& candidate,
	              const std::filesystem::path& directory) {
	const auto path = ComparablePath(candidate);
	auto       root = ComparablePath(directory);
	if (path == root) {
		return true;
	}
	if (!root.empty() && root.back() != '/') {
		root.push_back('/');
	}
	return path.starts_with(root);
}

} // namespace

bool ValidateFrameRegressionPaths(const ConfigOptions& cfg, std::string& error) {
	error.clear();
	if (cfg.frame_regression_mode == FrameRegressionMode::None) {
		return true;
	}
	const auto& baseline = cfg.frame_regression_baseline;
	const auto& report   = cfg.frame_regression_report;
	const auto& capture  = cfg.gpu_capture_file;
	if (Common::OutputReservation::IsReservedPath(baseline) ||
	    (cfg.frame_regression_mode == FrameRegressionMode::Compare &&
	     Common::OutputReservation::IsReservedPath(report)) ||
	    (!capture.empty() && Common::OutputReservation::IsReservedPath(capture))) {
		error = "regression output uses the reserved .kyty-lock suffix";
		return false;
	}
	if (cfg.frame_regression_mode == FrameRegressionMode::Compare &&
	    (ComparablePath(baseline) == ComparablePath(report) ||
	     ComparablePath(baseline) ==
	         ComparablePath(Common::OutputReservation::LockPath(report)) ||
	     ComparablePath(FrameDirectory(baseline)) == ComparablePath(FrameDirectory(report)) ||
	     SameOrWithin(report, FrameDirectory(baseline)) ||
	     SameOrWithin(Common::OutputReservation::LockPath(report),
	                  FrameDirectory(baseline)))) {
		error = "regression baseline and report outputs collide";
		return false;
	}
	if (!capture.empty() &&
	    (ComparablePath(capture) == ComparablePath(baseline) ||
	     ComparablePath(Common::OutputReservation::LockPath(capture)) ==
	         ComparablePath(baseline) ||
	     SameOrWithin(capture, FrameDirectory(baseline)) ||
	     SameOrWithin(Common::OutputReservation::LockPath(capture), FrameDirectory(baseline)) ||
	     (cfg.frame_regression_mode == FrameRegressionMode::Compare &&
	      (ComparablePath(capture) == ComparablePath(report) ||
	       ComparablePath(capture) ==
	           ComparablePath(Common::OutputReservation::LockPath(report)) ||
	       ComparablePath(Common::OutputReservation::LockPath(capture)) ==
	           ComparablePath(report) ||
	       SameOrWithin(capture, FrameDirectory(report)) ||
	       SameOrWithin(Common::OutputReservation::LockPath(capture),
	                    FrameDirectory(report)))))) {
		error = "GPU capture and regression outputs collide";
		return false;
	}
	return true;
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

std::filesystem::path GetGpuCaptureFile() {
	return g_config->gpu_capture_file;
}

FrameRegressionMode GetFrameRegressionMode() {
	return g_config->frame_regression_mode;
}

std::filesystem::path GetFrameRegressionBaseline() {
	return g_config->frame_regression_baseline;
}

std::filesystem::path GetFrameRegressionReport() {
	return g_config->frame_regression_report;
}

const std::vector<uint64_t>& GetFrameRegressionFrames() {
	return g_config->frame_regression_frames;
}

const std::string& GetFrameRegressionTestId() {
	return g_config->frame_regression_test_id;
}

bool FrameRegressionSaveRaw() {
	return g_config->frame_regression_save_raw;
}

bool FrameRegressionExitOnComplete() {
	return g_config->frame_regression_exit_on_complete;
}

bool FrameRegressionAllowEnvironmentMismatch() {
	return g_config->frame_regression_allow_environment_mismatch;
}

} // namespace Config
