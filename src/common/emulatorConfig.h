#ifndef KYTY_COMMON_EMULATOR_CONFIG_H_
#define KYTY_COMMON_EMULATOR_CONFIG_H_

#include "common/common.h"
#include "common/subsystems.h"

#include <filesystem>
#include <vector>

namespace Config {

KYTY_SUBSYSTEM_DEFINE(Config);

enum class ShaderOptimizationType { None, Size, Performance };

enum class ShaderLogDirection { Silent, Console, File };

enum class ProfilerDirection { None, Network };

enum class OutputDirection { Silent, Console, File };

enum class FrameRegressionMode { None, Record, Compare };

struct ConfigOptions {
	uint32_t               screen_width                = 1280;
	uint32_t               screen_height               = 720;
	uint32_t               vblank_frequency            = 60;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	ShaderLogDirection     shader_log_direction        = ShaderLogDirection::Silent;
	std::filesystem::path  shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	std::filesystem::path  command_buffer_dump_folder  = "_Buffers";
	bool                   graphics_debug_dump_enabled = false;
	OutputDirection        printf_direction            = OutputDirection::Console;
	std::filesystem::path  printf_output_file          = "_kyty.txt";
	ProfilerDirection      profiler_direction          = ProfilerDirection::None;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   renderdoc_enabled           = false;
	bool                   ngg_rectlist_draw_enabled   = true;
	std::filesystem::path  gpu_capture_file;
	FrameRegressionMode    frame_regression_mode = FrameRegressionMode::None;
	std::filesystem::path  frame_regression_baseline;
	std::filesystem::path  frame_regression_report = "_Regression/report.json";
	std::vector<uint64_t>  frame_regression_frames;
	bool                   frame_regression_save_raw         = true;
	bool                   frame_regression_exit_on_complete = true;
};

void Load(const ConfigOptions& cfg);

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
uint32_t GetVblankFrequency();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
ShaderOptimizationType GetShaderOptimizationType();
ShaderLogDirection     GetShaderLogDirection();
std::filesystem::path  GetShaderLogFolder();

bool                  CommandBufferDumpEnabled();
std::filesystem::path GetCommandBufferDumpFolder();

bool GraphicsDebugDumpEnabled();

OutputDirection       GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

ProfilerDirection GetProfilerDirection();

bool SpirvDebugPrintfEnabled();

bool RenderDocEnabled();
bool NggRectlistDrawEnabled();

std::filesystem::path       GetGpuCaptureFile();
FrameRegressionMode         GetFrameRegressionMode();
std::filesystem::path       GetFrameRegressionBaseline();
std::filesystem::path       GetFrameRegressionReport();
const std::vector<uint64_t>& GetFrameRegressionFrames();
bool                        FrameRegressionSaveRaw();
bool                        FrameRegressionExitOnComplete();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
