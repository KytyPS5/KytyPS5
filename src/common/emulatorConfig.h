#ifndef KYTY_COMMON_EMULATOR_CONFIG_H_
#define KYTY_COMMON_EMULATOR_CONFIG_H_

#include "common/common.h"

#include <cstddef>
#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Config {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Config";
	static constexpr auto        initialize = Config::Initialize;
	static constexpr auto        shutdown   = Config::Shutdown;
};

enum class ShaderOptimizationType { None, Size, Performance };

enum class LogDirection { Silent, Console, File };

enum class PresentMode { Fifo, Mailbox, Immediate };

// The output resolution the emulated console reports to games (videoOutResolution.h).
enum class VideoOutResolution { Title, FullHd, Uhd };

using Keymap = std::vector<std::string>;
using ControllerColor = std::array<uint8_t, 3>;

constexpr uint32_t DEFAULT_CONSOLE_LANGUAGE = 1;
constexpr uint32_t MAX_CONSOLE_LANGUAGE     = 29;
constexpr std::size_t MAX_USER_NAME_LENGTH = 16;
constexpr int32_t DEFAULT_USER_ID           = 1000;

constexpr int     MAX_PLAYERS         = 4;
constexpr int32_t PLAYER_USER_ID_STEP = 0x100;
constexpr int32_t MAX_USER_ID =
    std::numeric_limits<int32_t>::max() - (MAX_PLAYERS - 1) * PLAYER_USER_ID_STEP;

constexpr bool IsConfiguredUserIdValid(int32_t user_id) {
	constexpr int32_t USER_ID_EVERYONE = 0xfe;
	constexpr int32_t USER_ID_SYSTEM   = 0xff;
	return user_id >= 0 && user_id <= MAX_USER_ID && user_id != USER_ID_EVERYONE &&
	       user_id != USER_ID_SYSTEM;
}

struct ConfigOptions {
	uint32_t               screen_width                = 1280;
	uint32_t               screen_height               = 720;
	std::string            user_name                   = "Kyty";
	int32_t                user_id                     = DEFAULT_USER_ID;
	std::string            audio_input_device;
	std::optional<ControllerColor> controller_color;
	uint32_t               controller_speaker_volume      = 50;
	uint32_t               controller_vibration_intensity = 100;
	// 0 shares player 1 with the first gamepad; 1..MAX_PLAYERS dedicates that player to the keyboard.
	uint32_t               keyboard_player                = 0;
	PresentMode            present_mode                = PresentMode::Mailbox;
	VideoOutResolution     video_out_resolution        = VideoOutResolution::Title;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	bool                   hdr_enabled                 = false;
	bool                   hide_cursor_enabled         = false;
	bool                   stretch_enabled             = false;
	bool                   splash_enabled              = true;
	bool                   vr_enabled                  = false;
	bool                   pro_enabled                 = false;
	bool                   amd_cpu_enabled             = false;
	bool                   network_connected           = false;
	bool                   psn_emulation_enabled       = false;
	bool                   ea_emulation_enabled        = true;
	uint32_t               vblank_frequency            = 60;
	// Guest frames (suspend points) recorded before the host GPU completes the oldest of them.
	uint32_t               gpu_frames_in_flight        = 2;
	// The GPU reads and writes guest direct memory in place where the host can import it
	// (Graphics::GuestMemoryImport); otherwise buffers are copies, synchronized both ways.
	bool                   gpu_guest_memory_import     = true;
	uint32_t               console_language            = DEFAULT_CONSOLE_LANGUAGE;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	bool                   pipeline_wait_enabled       = false;
	bool                   shader_cache_enabled        = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	LogDirection           shader_log_direction        = LogDirection::Silent;
	std::filesystem::path  shader_log_folder           = "_Shaders";
	// Empty: the host's per-user cache folder (cacheFolder.h).
	std::filesystem::path  pipeline_cache_folder;
	// Empty: no benchmark script (benchScript.h).
	std::filesystem::path  bench_script;
	bool                   command_buffer_dump_enabled = false;
	std::filesystem::path  command_buffer_dump_folder  = "_Buffers";
	bool                   graphics_debug_dump_enabled = false;
	// Name Vulkan objects and label passes, draws, dispatches and copies, so a GPU capture reads
	// as the emulator's work and not as anonymous commands.
	bool                   gpu_debug_labels_enabled        = false;
	LogDirection           printf_direction            = LogDirection::Silent;
	std::filesystem::path  printf_output_file          = "_kyty.txt";
	bool                   profiler_enabled            = false;
	// Count the events that cost frame time and print them per frame (perfCounters.h).
	bool                   perf_counters_enabled       = false;
	// Empty: no trace (trace.h). Else the folder of a measured run's reports.
	std::filesystem::path  trace_dir;
	uint32_t               trace_screenshot_seconds        = 10;
	uint32_t               trace_hang_seconds              = 30;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   gpu_assisted_validation_enabled = false;
	bool                   renderdoc_enabled           = false;
	bool                   readback_linear_images      = false;
	bool                   readback_tiled_images       = false;
	bool                   sync_raw_image_buffers      = false;
	bool                   tessellation_enabled        = false;
	bool                   occlusion_queries_enabled   = false;
	bool                   trophy_enabled              = true;
	uint32_t               trophy_duration_seconds     = 6;
	bool                   perf_overlay_enabled            = false;
	bool                   playgo_hack_enabled         = false;
	bool                   skip_notice_screen          = false;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = false;
#endif
	Keymap keymap;
	// Ordinary boots need a non-null argv[0]; --guest-exec supplies the caller's arguments.
	std::vector<std::string> guest_arguments {""};
};

void Load(const ConfigOptions& cfg);

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
const std::string& GetUserName();
int32_t  GetUserId();
const std::string& GetAudioInputDevice();
const std::optional<ControllerColor>& GetControllerColor();
uint32_t GetControllerSpeakerVolume();
uint32_t GetControllerVibrationIntensity();
uint32_t GetKeyboardPlayer();
// Player 0 is the configured user; later players derive their IDs from it.
int32_t  GetPlayerUserId(int player);
// Returns -1 when the user ID does not belong to any player.
int      GetPlayerFromUserId(int32_t user_id);
// Players after the first are guests: they have no profile, trophies or NP account.
bool     IsGuestUserId(int32_t user_id);
PresentMode GetPresentMode();
VideoOutResolution     GetVideoOutResolution();
int32_t GetGpuIndex();
bool     FullscreenEnabled();
bool     HdrEnabled();
bool     HideCursorEnabled();
bool     StretchEnabled();
bool     VrEnabled();
bool     Ps5ProEnabled();
bool     AmdCpuEnabled();
bool     IsNetworkConnected();
// For a title whose servers the emulator answers itself (libs/eaEmulation): its network is reachable.
void     SetNetworkConnected();
bool     PsnEmulationEnabled();
// A game of EA gets the built-in stand-in for EA's servers (libs/eaEmulation).
bool     EaEmulationEnabled();
uint32_t GetVblankFrequency();
inline constexpr uint32_t MAX_GPU_FRAMES_IN_FLIGHT = 3;
uint32_t                  GetGpuFramesInFlight();
bool                      GetGpuGuestMemoryImport();
uint32_t GetConsoleLanguage();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
// Every draw waits for its pipeline to be built, instead of being left out while it builds.
bool                   PipelineWaitEnabled();
bool                   ShaderCacheEnabled();
ShaderOptimizationType GetShaderOptimizationType();
LogDirection           GetShaderLogDirection();
std::filesystem::path  GetShaderLogFolder();
std::filesystem::path  GetPipelineCacheFolder();
std::filesystem::path  GetBenchScript();
std::filesystem::path  GetTraceDir();
uint32_t               GetTraceScreenshotSeconds();
uint32_t               GetTraceHangSeconds();

bool                  CommandBufferDumpEnabled();
std::filesystem::path GetCommandBufferDumpFolder();

bool GraphicsDebugDumpEnabled();
bool GpuDebugLabelsEnabled();

LogDirection          GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

bool ProfilerEnabled();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool ReadbackTiledImagesEnabled();
bool SyncRawImageBuffersEnabled();
bool TessellationEnabled();
bool OcclusionQueriesEnabled();
bool TrophyEnabled();
uint32_t GetTrophyDurationSeconds();
bool PlayGoHackEnabled();
bool SkipNoticeScreen();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
#endif

const Keymap& GetKeymap();
const std::vector<std::string>& GetGuestArguments();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
