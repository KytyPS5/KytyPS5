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

enum class LogDirection { Silent, Console, File };

enum class PresentMode { Fifo, Mailbox, Immediate };

enum class PssrUpscaler { Linear, DLSS, FSR };

// What the emulator does when the pipelines it has to compile ahead of the game would take long:
// ask the player whether to wait for them or play at once, wait without asking, or play at once.
enum class PipelinePrecompile { Ask, Wait, Play };

using Keymap = std::vector<std::string>;
using ControllerColor = std::array<uint8_t, 3>;

// Stages of the emulated GPU that run on threads of their own; see GpuPipelineStages().
enum GpuPipelineStage : uint32_t {
	GPU_PIPELINE_RECORDING = 1u << 0u, // Vulkan command recording and submission.
	// The command processor runs ahead of the execution of the operations it produces.
	GPU_PIPELINE_COMMAND_PROCESSOR = 1u << 1u,
	// Shader programs and resource tables of draws are resolved ahead of their execution
	// (requires the command processor stage).
	GPU_PIPELINE_RESOLVE = 1u << 2u,
	GPU_PIPELINE_ALL =
	    GPU_PIPELINE_RECORDING | GPU_PIPELINE_COMMAND_PROCESSOR | GPU_PIPELINE_RESOLVE,
};

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
	// Rumble pads that are not DualSenses with the peak of the haptic audio port.
	bool                   pad_haptic_rumble           = false;
	PresentMode            present_mode                = PresentMode::Mailbox;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	bool                   hdr_enabled                 = false;
	bool                   hide_cursor_enabled         = false;
	bool                   stretch_enabled             = false;
	bool                   splash_enabled              = true;
	bool                   vr_enabled                  = false;
	bool                   pro_enabled                 = false;
	PssrUpscaler           pssr_upscaler               = PssrUpscaler::Linear;
	std::filesystem::path  dlss_folder;
	uint32_t               dlss_preset                 = 0; // 0 is the model default
	std::filesystem::path  fsr_folder;
	bool                   amd_cpu_enabled             = false;
	bool                   guest_tsc_enabled           = true;
	bool                   network_connected           = false;
	bool                   psn_emulation_enabled       = false;
	uint32_t               vblank_frequency            = 60;
	uint32_t               console_language            = DEFAULT_CONSOLE_LANGUAGE;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	bool                   shader_cache_enabled        = false;
	bool                   pipeline_manifest_enabled   = true;
	bool                   pipeline_manifest_record    = false;
	PipelinePrecompile     pipeline_precompile         = PipelinePrecompile::Ask;
	LogDirection           shader_log_direction        = LogDirection::Silent;
	std::filesystem::path  shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	std::filesystem::path  command_buffer_dump_folder  = "_Buffers";
	bool                   graphics_debug_dump_enabled = false;
	LogDirection           printf_direction            = LogDirection::Silent;
	std::filesystem::path  printf_output_file          = "_kyty.txt";
	// See GetSessionLogFile(). Empty: none.
	std::filesystem::path  session_log_file;
	bool                   profiler_enabled            = false;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   gpu_assisted_validation_enabled = false;
	bool                   renderdoc_enabled           = false;
	bool                   readback_linear_images      = true;
	bool                   readback_tiled_images       = false;
	bool                   readback_storage_images     = false;
	bool                   sync_raw_image_buffers      = false;
	uint32_t               flush_draws                 = 0; // 0 is off
	bool                   tessellation_enabled        = true;
	bool                   occlusion_queries_enabled   = true;
	bool                   relaxed_shader_float        = false;
	bool                   native_rt                   = false;
	bool                   async_pipelines_enabled     = true;
	bool                   rt_stats_enabled            = false;
	bool                   trophy_enabled              = true;
	uint32_t               trophy_duration_seconds     = 6;
	bool                   perf_overlay_enabled            = false;
	bool                   playgo_hack_enabled         = false;
	bool                   skip_notice_screen          = false;
	std::string            launch_activity;
	uint32_t               gpu_pipeline_stages         = GPU_PIPELINE_ALL;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = true;
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
bool     PadHapticRumbleEnabled();
PresentMode GetPresentMode();
int32_t GetGpuIndex();
bool     FullscreenEnabled();
bool     HdrEnabled();
bool     HideCursorEnabled();
bool     StretchEnabled();
bool     VrEnabled();
bool     Ps5ProEnabled();
PssrUpscaler GetPssrUpscaler();
const std::filesystem::path& GetDlssFolder();
uint32_t GetDlssPreset();
const std::filesystem::path& GetFsrFolder();
bool     AmdCpuEnabled();
bool     GuestTscEnabled();
bool     IsNetworkConnected();
bool     PsnEmulationEnabled();
uint32_t GetVblankFrequency();
uint32_t GetConsoleLanguage();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
bool                   ShaderCacheEnabled();
// Programs and pipelines named in the title's pipeline manifest are created once the guest maps
// their shaders.
bool                   PipelineManifestEnabled();
// The programs and pipelines the session creates are added to the title's manifest in
// _PipelineCache.
bool                   PipelineManifestRecord();
// --pipeline-precompile: see PipelinePrecompile.
PipelinePrecompile     GetPipelinePrecompile();
LogDirection           GetShaderLogDirection();
std::filesystem::path  GetShaderLogFolder();

bool                  CommandBufferDumpEnabled();
std::filesystem::path GetCommandBufferDumpFolder();

bool GraphicsDebugDumpEnabled();

LogDirection          GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();
// --session-log: the console messages of the session and the report of a fatal error, without the
// printf log (see Log::WriteToSessionLog).
std::filesystem::path GetSessionLogFile();

bool ProfilerEnabled();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool ReadbackTiledImagesEnabled();
bool ReadbackStorageImagesEnabled();
bool SyncRawImageBuffersEnabled();
uint32_t GetFlushDraws();
bool TessellationEnabled();
bool OcclusionQueriesEnabled();
bool RelaxedShaderFloatEnabled();
bool NativeRtEnabled();
// Draws whose new pipeline the driver is still compiling are skipped instead of waiting for it.
bool AsyncPipelinesEnabled();
// Experimental, --native-rt (alias --native-raytracing), like NativeRtEnabled(): the ray tracing
// passes the shader recompiler recognizes trace with the host's ray tracing hardware (see
// RayTracing). Off, the device enables no ray tracing feature and no shader is patched.
bool NativeRayTracingEnabled();
// With native ray tracing: the patched shaders count on the GPU the rays they send back to the
// guest traversal, for the "[RT]" summaries (see RayTracing). Costs a ballot per wave.
bool RayTracingStatsEnabled();
bool TrophyEnabled();
uint32_t GetTrophyDurationSeconds();
bool PlayGoHackEnabled();
bool SkipNoticeScreen();
// Activity ID of the one-shot launchActivity game intent; empty sends none.
const std::string& GetLaunchActivity();
// GpuPipelineStage bits. The KYTY_GPU_PIPELINE environment variable overrides the option: 0 runs
// the whole emulated GPU inline on one thread, 1 enables every stage, other values are masks.
uint32_t GpuPipelineStages();
uint32_t ParseGpuPipelineStages(const char* value);

// Threads of a pipelined GPU. KYTY_GPU_THREAD_CPUS lists the logical processors the command
// processor, execution, recording, resolve, host copy and resolve helper threads are pinned to
// ("off" leaves them unpinned); KYTY_GPU_THREAD_PRIORITY=0 keeps their normal priority.
enum class GpuStageThread : uint32_t {
	CommandProcessor = 0,
	Execution        = 1,
	Recording        = 2,
	Resolve          = 3,
	HostCopy         = 4,
	ResolveHelper    = 5
};
// KYTY_RESOLVE_HELPER=1: a second thread resolves draws ahead of their execution beside the
// resolve thread.
bool ResolveHelperEnabled();
// Applies the priority and affinity of `stage` to the calling thread.
void ConfigureGpuStageThread(GpuStageThread stage);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
#endif

const Keymap& GetKeymap();
const std::vector<std::string>& GetGuestArguments();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
