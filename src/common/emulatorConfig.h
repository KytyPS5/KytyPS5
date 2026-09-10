#ifndef KYTY_COMMON_EMULATOR_CONFIG_H_
#define KYTY_COMMON_EMULATOR_CONFIG_H_

#include "common/common.h"

#include <cstddef>
#include <filesystem>
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

enum class ShaderLogDirection { Silent, Console, File };

enum class ProfilerDirection { None, Network };

enum class OutputDirection { Silent, Console, File };

enum class PresentMode { Fifo, Mailbox, Immediate };

using Keymap = std::vector<std::string>;

constexpr uint32_t DEFAULT_CONSOLE_LANGUAGE = 1;
constexpr uint32_t MAX_CONSOLE_LANGUAGE     = 29;
constexpr std::size_t MAX_USER_NAME_LENGTH = 16;
constexpr int32_t DEFAULT_USER_ID           = 1000;

constexpr bool IsConfiguredUserIdValid(int32_t user_id) {
	constexpr int32_t USER_ID_EVERYONE = 0xfe;
	constexpr int32_t USER_ID_SYSTEM   = 0xff;
	return user_id >= 0 && user_id != USER_ID_EVERYONE && user_id != USER_ID_SYSTEM;
}

struct ConfigOptions {
	uint32_t               screen_width                = 1280;
	uint32_t               screen_height               = 720;
	std::string            user_name                   = "Kyty";
	int32_t                user_id                     = DEFAULT_USER_ID;
	PresentMode            present_mode                = PresentMode::Fifo;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	uint32_t               vblank_frequency            = 60;
	uint32_t               console_language            = DEFAULT_CONSOLE_LANGUAGE;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	ShaderLogDirection     shader_log_direction        = ShaderLogDirection::Silent;
	std::filesystem::path  shader_log_folder           = "_Shaders";
	// Driver pipeline cache/shader disk cache are both gated off whenever the build's git hash
	// ends in "-dirty" (see pipelineCache.cpp), a deliberate safety measure: a WIP recompiler
	// build could silently serve a stale cache produced by different, already-forgotten code.
	// Real cost, measured (workflow/kytyps5_performance.md, workflow/study_tooling_perf.md):
	// every dev-tree run recompiles every shader from scratch, and this codebase's tree is
	// dirty for the entirety of active development, so the cache never helps mid-investigation.
	// This flag opts back into caching despite a dirty tree -- deliberately narrow: it skips
	// ONLY the "-dirty" check, not the Release-build or unknown-git-revision checks, which guard
	// against different, still-relevant problems. Off by default; the developer opting in is
	// accepting that a cache entry may have been produced by different recompiler code than the
	// current dirty tree, same as manually deleting _ShaderCache.bin/_PipelineCache.bin whenever
	// a recompiler change might invalidate them.
	bool                   force_shader_disk_cache_enabled = false;
	// Driver pipeline cache, persisted between runs so the second launch of a
	// title does not recompile every pipeline again. Empty disables it.
	std::filesystem::path  pipeline_cache_file         = "_PipelineCache.bin";
	// Recompiled-shader cache, persisted between runs so the GCN -> SPIR-V
	// translation is not redone every launch. Empty disables it.
	std::filesystem::path  shader_cache_file           = "_ShaderCache.bin";
	// How many messages one LOGF call site may write before it is sampled
	// instead. Zero means no limit. Logs reaching several GB in two minutes
	// (issue #200) are almost entirely a few sites in per-draw loops.
	uint64_t               log_repeat_limit            = 256;
	// Hard cap, in bytes, on the printf output file (see printf_output_file below).
	// Default matches the incident-driven 6.9 GB cap in log.cpp. Zero disables the
	// cap entirely (an uncapped basic_file_sink instead of a rotating one) for
	// sessions that deliberately want a full, uncut capture -- see log.cpp for the
	// disk-filling incident this cap exists to prevent, and the stdout warning
	// printed when it is disabled.
	uint64_t               log_file_max_bytes          = 6'900'000'000ull;
	// Restricts SrtWalker's per-descriptor diagnostic logging (e.g.
	// SrtInactiveDescriptorSource) to only the named shader hashes. Empty means
	// unrestricted (default, matching prior behavior). That site has no frame
	// number to gate on the way draw-log-frame-first/-last does -- the recompiler
	// is deliberately frame-agnostic -- so a hash allowlist is the only filter
	// this layer can express, and fires 100,000+ times per real run otherwise.
	std::vector<uint64_t>  shader_log_filter_hashes;
	bool                   command_buffer_dump_enabled = false;
	std::filesystem::path  command_buffer_dump_folder  = "_Buffers";
	bool                   graphics_debug_dump_enabled = false;
	// ValidateProgram (recompiler/ir/Program.cpp) is a hard-abort IR consistency check: it
	// rebuilds several hash containers over every block/instruction and walks the CFG, twice per
	// distinct shader (once post-translate, once pre-SPIR-V-emit). Real cost, measured via `perf`
	// on a clean ASTRO's Playroom launch (2026-09-10): 6.64% of sampled CPU self-time in a single
	// function, on top of allocator churn it specifically causes -- and it never produces wrong
	// output on its own, only a diagnostic abort if the recompiler already produced malformed IR.
	// Defaults to enabled (current behavior, unchanged) because this codebase's recompiler is
	// under active development and this is the only hard safety net against a malformed-IR bug
	// silently producing wrong shader output instead of a clear, immediate abort. Set false for a
	// real playthrough once the recompiler path in use is trusted.
	bool                   validate_shader_ir_enabled  = true;
	// SrtWalker.cpp's EvaluatePhi: when a shader's descriptor selection is control-flow-
	// dependent (the guest computes "use resource A or B" per invocation) and the Phi merging
	// the two candidates is not invariant, the emulator's default behavior is to skip the whole
	// draw/dispatch rather than guess -- session 18/21/22's ASTRO's Playroom investigation found
	// 15 distinct compute shaders hitting exactly this. Real AMD hardware/compilers face the
	// identical constraint (MIMG/MUBUF descriptors must be uniform SGPRs) and solve it by
	// forcing the value uniform via v_readfirstlane_b32 -- see
	// mesa/src/amd/compiler/aco_lower_to_hw_instr.cpp. This flag opts into the same mechanism:
	// evaluate every candidate through the normal runtime path, refuse (falling back to today's
	// skip-the-draw behavior) if every candidate is zero/unevaluable, otherwise use the first
	// viable one. Defaults to OFF: unlike ACO's compile-time-certain case, this session has not
	// verified it is safe for every affected shader in every game -- a wrong pick for a compute
	// output the guest's own CPU code later trusts as a pointer/index/count could misbehave in
	// a way "the draw was skipped" cannot. Opt-in for testing until real evidence says otherwise.
	bool                   approximate_divergent_phi_enabled = false;
	// Debug tool for localizing rendering defects when RenderDoc capture is unavailable
	// (e.g. no native-Wayland Vulkan surface support in RenderDoc). Writes a PNG of each
	// color render target's contents to draw_dump_folder every time it stops being the
	// active rendering target (CommandBuffer::EndRendering), which is coarser than a
	// per-draw-call dump but needs no change to how draws batch into Vulkan render passes.
	bool                   draw_dump_enabled           = false;
	std::filesystem::path  draw_dump_folder            = "_Draws";
	// Per-draw diagnostic logging (LogDrawTargetState/LogDrawInputState/LegacyRectDraw in
	// renderDraw.cpp) is itself hard-capped (see Log::RateLimit), and a long run reaches those
	// caps long before an intermittent defect's frame comes up, leaving the exact draw of
	// interest with zero log lines. This narrows which frames spend that budget at all, so caps
	// are spent on the frames actually being investigated instead of on frame 1. -1 means
	// unbounded (the default: no frame filtering, matching prior behavior).
	int64_t                draw_log_frame_first        = -1;
	int64_t                draw_log_frame_last         = -1;
	// Dumps the actual presented/flipped frame (Swapchain::RecordPresentCommands' blit source)
	// every present_dump_every frames, instead of an intermediate render target -- the decisive
	// artifact for "is the picture actually right", sidestepping whether some intermediate G-buffer
	// target is a correct pass or a genuine defect. 0/negative means "every frame dumped" is NOT
	// intended; present_dump_enabled gates whether this runs at all (default off, matching
	// draw_dump_enabled's own opt-in shape).
	bool                   present_dump_enabled        = false;
	int64_t                present_dump_every          = 300;
	std::filesystem::path  present_dump_folder         = "_PresentDumps";
	// Scripted controller-input replay: a text file of "<guest_frame> <event> <args...>" lines
	// (see src/libs/controller.cpp's InputScriptPlayer), fed into the same GameController ring
	// buffer the real SDL host-input path uses. Empty disables it (default: no script, matching
	// every other opt-in dump/debug flag's shape). Exists so a run can be driven and verified
	// without a physical controller attached.
	std::filesystem::path  input_script_path           = "";
	OutputDirection        printf_direction            = OutputDirection::Silent;
	std::filesystem::path  printf_output_file          = "_kyty.txt";
	ProfilerDirection      profiler_direction          = ProfilerDirection::None;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   gpu_assisted_validation_enabled = false;
	bool                   renderdoc_enabled           = false;
	bool                   readback_linear_images      = false;
	bool                   playgo_hack_enabled         = false;
	bool                   bvh_stub_enabled            = false;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = false;
#endif
	Keymap keymap;
	// Physical-gamepad equivalent of `keymap`: "Control=SdlButtonName" or
	// "Control=SdlAxisName" (e.g. "Cross=a", "LeftStickLeft=leftx"), where the
	// names are whatever SDL_GameControllerGetStringForButton/Axis returns.
	// Empty means "use the built-in Xbox-layout defaults" (see window.cpp).
	Keymap gamepad_keymap;
	// Fraction of a stick's travel (0.0-1.0) to treat as centered/neutral,
	// applied to both sticks equally. 0 disables deadzone filtering.
	float  gamepad_deadzone = 0.0f;
};

void Load(const ConfigOptions& cfg);

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
const std::string& GetUserName();
int32_t  GetUserId();
PresentMode GetPresentMode();
int32_t GetGpuIndex();
bool     FullscreenEnabled();
uint32_t GetVblankFrequency();
uint32_t GetConsoleLanguage();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
ShaderOptimizationType GetShaderOptimizationType();
ShaderLogDirection     GetShaderLogDirection();
std::filesystem::path  GetPipelineCacheFile();
std::filesystem::path  GetShaderCacheFile();
uint64_t               GetLogRepeatLimit();
uint64_t               GetLogFileMaxBytes();
bool                   ShaderLogHashAllowed(uint64_t hash);
bool                   ForceShaderDiskCacheEnabled();
std::filesystem::path  GetShaderLogFolder();

bool                  CommandBufferDumpEnabled();
std::filesystem::path GetCommandBufferDumpFolder();

bool GraphicsDebugDumpEnabled();
bool ValidateShaderIrEnabled();
bool ApproximateDivergentPhiEnabled();

bool                  DrawDumpEnabled();
std::filesystem::path GetDrawDumpFolder();
int64_t               GetDrawLogFrameFirst();
int64_t               GetDrawLogFrameLast();

bool                  PresentDumpEnabled();
int64_t               GetPresentDumpEvery();
std::filesystem::path GetPresentDumpFolder();

std::filesystem::path GetInputScriptPath();

OutputDirection       GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

ProfilerDirection GetProfilerDirection();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool PlayGoHackEnabled();
bool BvhStubEnabled();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
#endif

const Keymap& GetKeymap();
const Keymap& GetGamepadKeymap();
float         GetGamepadDeadzone();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
