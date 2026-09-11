#include "common/common.h"
#include "common/dateTime.h"
#include "common/debug.h"
#include "common/diagnostics.h"
#include "common/file.h"
#include "common/magicEnum.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "graphics/host_gpu/vulkanDiagnostics.h"
#include "kytyGitVersion.h"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>

using namespace Common;
using namespace Emulator;

static void PrintUsage() {
	::printf("%s\n", Common::Diagnostics::BuildString().c_str());
	::printf("kyty_emulator --game <dir|elf> [options]\n\n");
	::printf("Options:\n");
	::printf("  --game <dir|elf>                     Game directory or ELF to load.\n");
	::printf("  --game-patch <json>                  ETAHen cheat file.\n");
	::printf("  --screen-width <num>                 Window width. Default: 1280.\n");
	::printf("  --screen-height <num>                Window height. Default: 720.\n");
	::printf(
	    "  --user-name <name>                   Local user name (1-16 bytes). Default: Kyty.\n");
	::printf("  --user-id <num>                      Local user ID. Default: %d.\n",
	         Config::DEFAULT_USER_ID);
	::printf(
	    "  --present-mode <value>               Fifo, Mailbox, or Immediate. Default: Fifo.\n");
	::printf(
	    "  --gpu <index>                        Vulkan physical device index. Default: auto.\n");
	::printf("  --fullscreen                         Run in borderless desktop fullscreen.\n");
	::printf("  --vblank-frequency <num>             Virtual vblank frequency. Default: 60.\n");
	::printf("  --console-language <0-29>            Console language. Default: 1 (English US).\n");
	::printf("  --vulkan-validation <true|false>     Enable Vulkan validation.\n");
	::printf("  --gpu-assisted-validation <t|f>      Bounds-check shader accesses on the GPU.\n"
	         "                                       Implies --vulkan-validation; very slow.\n");
	::printf("  --shader-validation <true|false>     Enable shader validation.\n");
	::printf("  --shader-optimization-type <value>   None, Size, or Performance.\n");
	::printf("  --shader-log-direction <value>       Silent, Console, or File.\n");
	::printf("  --shader-log-folder <path>           Shader log output folder.\n");
	::printf("  --command-buffer-dump <true|false>   Enable command buffer dumps.\n");
	::printf("  --command-buffer-dump-folder <path>  Command buffer dump folder.\n");
	::printf("  --graphics-debug-dump <true|false>   Enable graphics debug dumps.\n");
	::printf("  --validate-shader-ir <true|false>    Recompiler IR consistency check, twice per\n"
	         "                                       distinct shader (post-translate, pre-SPIR-V).\n"
	         "                                       Hard-aborts on a malformed IR bug instead of\n"
	         "                                       letting it produce wrong output silently.\n"
	         "                                       Measured 6.64%% of sampled CPU self-time on a\n"
	         "                                       real launch (2026-09-10) -- default true;\n"
	         "                                       set false to trust the recompiler and skip it.\n");
	::printf("  --approximate-divergent-phi <t|f>    EXPERIMENTAL. A shader whose descriptor\n"
	         "                                       selection is control-flow-dependent (per-\n"
	         "                                       invocation resource choice) normally skips\n"
	         "                                       its whole draw/dispatch. This picks the first\n"
	         "                                       viable candidate instead (refusing zero/\n"
	         "                                       unevaluable ones), matching how real AMD\n"
	         "                                       hardware/compilers force a divergent\n"
	         "                                       descriptor uniform. Default false: unproven\n"
	         "                                       for any specific game yet -- opt-in only.\n");
	::printf("  --draw-dump <true|false>             Dump each color render target to a PNG\n"
	         "                                       whenever it stops being the active\n"
	         "                                       rendering target. Debug only; slow.\n");
	::printf("  --draw-dump-folder <path>            Draw dump output folder.\n");
	::printf(
	    "  --draw-log-frame-first <n>           Only spend per-draw diagnostic log budget\n"
	    "                                       (LogDrawTargetState/LogDrawInputState/\n"
	    "                                       LegacyRectDraw/RenderPassBreak/ResolvedTexture),\n"
	    "                                       and --draw-dump's PNG encoding, from guest frame\n"
	    "                                       n onward. Default -1 (unbounded -- --draw-dump\n"
	    "                                       then dumps every draw of the whole run, which is\n"
	    "                                       slow on a draw-heavy scene).\n");
	::printf(
	    "  --draw-log-frame-last <n>            Same, upper bound (inclusive). Default -1\n"
	    "                                       (unbounded).\n");
	::printf(
	    "  --present-dump <true|false>          Dump the actual presented/flipped frame (not\n"
	    "                                       an intermediate render target) every\n"
	    "                                       --present-dump-every guest frames. Debug only.\n");
	::printf(
	    "  --present-dump-every <n>             Frame interval for --present-dump. Default 300.\n");
	::printf("  --present-dump-folder <path>         Present dump output folder.\n");
	::printf(
	    "  --input-script <path>                Replay scripted controller input from a text\n"
	    "                                       file (one \"<guest_frame> <event> <args...>\"\n"
	    "                                       line per event, e.g. \"120 button cross down\").\n"
	    "                                       Drives the same GameController state the real\n"
	    "                                       SDL host-input path uses, so it can push a guest\n"
	    "                                       through UI it would otherwise sit on forever\n"
	    "                                       unattended. Must point to an existing file.\n");
	::printf("  --printf-direction <value>           Silent, Console, or File.\n");
	::printf("  --printf-output-file <path>          Guest printf output file. Capped at\n"
	         "                                       --log-file-max-bytes on disk (default\n"
	         "                                       6.9 GB); wraps and overwrites from the\n"
	         "                                       start once reached, regardless of\n"
	         "                                       --log-repeat-limit.\n");
	::printf(
	    "  --log-file-max-bytes <n>             Cap for --printf-output-file, in bytes.\n"
	    "                                       Default 6900000000 (6.9 GB). 0 disables the\n"
	    "                                       cap for a deliberate full capture -- prints an\n"
	    "                                       unsilenceable free-space warning on startup.\n");
	::printf(
	    "  --shader-log-filter-hash <hex>       Restrict per-descriptor shader diagnostics\n"
	    "                                       (e.g. SrtInactiveDescriptorSource) to this\n"
	    "                                       shader hash; may be repeated. Default:\n"
	    "                                       unrestricted.\n");
	::printf(
	    "  --force-shader-disk-cache <true|false> Skip the \"dirty build\" check that normally\n"
	    "                                       disables the pipeline/shader disk cache during\n"
	    "                                       active development. Opt-in: a cache entry may\n"
	    "                                       have been produced by different recompiler code\n"
	    "                                       than the current dirty tree. Default: false.\n");
	::printf("  --profiler-direction <value>         None or Network.\n");
	::printf("  --spirv-debug-printf <true|false>    Enable SPIR-V debug printf.\n");
	::printf(
	    "  --readback-linear-images <true|false> Read back writable linear images on submit.\n");
	::printf("  --playgo-hack                       Use the supplied PlayGo stub fallback.\n");
	::printf("  --stub-bvh                          Use a stub for MIMG BVH opcodes (ray tracing is not implemented). Default: off.\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::printf("  --redzone                            Protect the guest SysV red zone.\n");
#endif
	::printf("  --keymap <Control=Input>             DualSense mapping; may be repeated.\n");
	::printf("  --gamepad-map <Control=SdlName>       Physical-gamepad button/axis mapping\n"
	         "                                       (SDL_GameController names); may be repeated.\n");
	::printf("  --gamepad-deadzone <0.0-0.95>         Stick deadzone fraction. Default: 0.\n");
	::printf("  --rd                                 Enable RenderDoc capture.\n");
	::printf("  --diagnostics                        Print a report for bug reports, then exit.\n");
	::printf("  --log-repeat-limit <num>             Messages one log line may write before it\n"
	         "                                       is sampled. 0 disables. Default: 256.\n");
}

static bool NextArg(int argc, char* argv[], int& index, std::string& out) {
	if (index + 1 >= argc) {
		return false;
	}

	index++;
	out = argv[index];
	return true;
}

static bool ParseBool(const std::string& value, bool& out) {
	if (Common::EqualNoCase(value, "true") || value == "1" || Common::EqualNoCase(value, "yes") ||
	    Common::EqualNoCase(value, "on")) {
		out = true;
		return true;
	}

	if (Common::EqualNoCase(value, "false") || value == "0" || Common::EqualNoCase(value, "no") ||
	    Common::EqualNoCase(value, "off")) {
		out = false;
		return true;
	}

	return false;
}

template <typename E>
static bool ParseEnum(const std::string& value, E& out) {
	auto enum_value = magic_enum::enum_cast<E>(value.c_str());
	if (!enum_value.has_value()) {
		return false;
	}

	out = enum_value.value();
	return true;
}

static bool ParseConsoleLanguage(const std::string& value, uint32_t& out) {
	uint32_t language = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), language);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    language > Config::MAX_CONSOLE_LANGUAGE) {
		return false;
	}
	out = language;
	return true;
}

static bool ParseUserId(const std::string& value, int32_t& out) {
	int32_t user_id   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), user_id);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    !Config::IsConfiguredUserIdValid(user_id)) {
		return false;
	}
	out = user_id;
	return true;
}

static bool ParseArgs(int argc, char* argv[], RunOptions& options, bool& show_help,
                      bool& show_diagnostics) {
	show_help        = false;
	show_diagnostics = false;

	for (int i = 1; i < argc; i++) {
		std::string arg = std::string(argv[i]);
		std::string value;

		if (arg == "--help" || arg == "-h") {
			show_help = true;
			continue;
		}

		if (arg == "--diagnostics") {
			show_diagnostics = true;
			continue;
		}

		if (arg == "--rd") {
			options.config.renderdoc_enabled = true;
			continue;
		}

		if (arg == "--fullscreen") {
			options.config.fullscreen_enabled = true;
			continue;
		}

		if (arg == "--playgo-hack") {
			options.config.playgo_hack_enabled = true;
			continue;
		}

		if (arg == "--stub-bvh") {
			options.config.bvh_stub_enabled = true;
			continue;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (arg == "--redzone") {
			options.config.red_zone_protection_enabled = true;
			continue;
		}
#endif

		if (!Common::StartsWith(arg, "--")) {
			::printf("game input must be provided with --game\n");
			return false;
		}

		if (!NextArg(argc, argv, i, value)) {
			::printf("missing value for %s\n", arg.c_str());
			return false;
		}

		if (arg == "--game") {
			if (!options.app0_dir.empty()) {
				::printf("--game can only be specified once\n");
				return false;
			}

			value = Common::FixFilenameSlash(value);
			if (Common::File::IsDirectoryExisting(value)) {
				options.app0_dir = value;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::File::IsFileExisting(value)) {
				options.app0_dir = Common::DirectoryWithoutFilename(value);
				if (options.app0_dir.empty()) {
					options.app0_dir = ".";
				}
				options.elf = "/app0/" + Common::FilenameWithoutDirectory(value);
			} else {
				::printf("--game must point to an existing directory or ELF: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--game-patch") {
			if (!options.game_patch.empty()) {
				::printf("--game-patch can only be specified once\n");
				return false;
			}
			value = Common::FixFilenameSlash(value);
			if (!Common::File::IsFileExisting(value)) {
				::printf("--game-patch must point to an existing file: %s\n", value.c_str());
				return false;
			}
			options.game_patch = value;
		} else if (arg == "--screen-width") {
			options.config.screen_width = static_cast<uint32_t>(Common::ToInt32(value));
		} else if (arg == "--screen-height") {
			options.config.screen_height = static_cast<uint32_t>(Common::ToInt32(value));
		} else if (arg == "--user-name") {
			if (value.empty() || value.size() > Config::MAX_USER_NAME_LENGTH) {
				::printf("invalid user name: must contain 1-%zu bytes\n",
				         Config::MAX_USER_NAME_LENGTH);
				return false;
			}
			options.config.user_name = value;
		} else if (arg == "--user-id") {
			if (!ParseUserId(value, options.config.user_id)) {
				::printf("invalid user ID: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--present-mode") {
			if (!ParseEnum(value, options.config.present_mode)) {
				::printf("invalid present mode: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu") {
			options.config.gpu_index = Common::ToInt32(value);
		} else if (arg == "--vblank-frequency") {
			const int32_t vblank_frequency = Common::ToInt32(value);
			options.config.vblank_frequency =
			    static_cast<uint32_t>(vblank_frequency < 0 ? 0 : vblank_frequency);
		} else if (arg == "--log-repeat-limit") {
			uint64_t   limit = 0;
			const auto parsed =
			    std::from_chars(value.data(), value.data() + value.size(), limit);
			if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()) {
				::printf("invalid number for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
			options.config.log_repeat_limit = limit;
		} else if (arg == "--log-file-max-bytes") {
			uint64_t   max_bytes = 0;
			const auto parsed =
			    std::from_chars(value.data(), value.data() + value.size(), max_bytes);
			if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()) {
				::printf("invalid number for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
			options.config.log_file_max_bytes = max_bytes;
		} else if (arg == "--shader-log-filter-hash") {
			uint64_t   hash    = 0;
			const auto trimmed = std::string_view(value).substr(
			    Common::StartsWith(value, "0x") || Common::StartsWith(value, "0X") ? 2 : 0);
			const auto parsed = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(),
			                                     hash, 16);
			if (parsed.ec != std::errc {} || parsed.ptr != trimmed.data() + trimmed.size()) {
				::printf("invalid hex shader hash for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
			options.config.shader_log_filter_hashes.push_back(hash);
		} else if (arg == "--force-shader-disk-cache") {
			if (!ParseBool(value, options.config.force_shader_disk_cache_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--console-language") {
			if (!ParseConsoleLanguage(value, options.config.console_language)) {
				::printf("invalid console language: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--vulkan-validation") {
			if (!ParseBool(value, options.config.vulkan_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gpu-assisted-validation") {
			if (!ParseBool(value, options.config.gpu_assisted_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-validation") {
			if (!ParseBool(value, options.config.shader_validation_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--shader-optimization-type") {
			if (!ParseEnum(value, options.config.shader_optimization_type)) {
				::printf("invalid shader optimization type: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-direction") {
			if (!ParseEnum(value, options.config.shader_log_direction)) {
				::printf("invalid shader log direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-folder") {
			options.config.shader_log_folder = value;
		} else if (arg == "--command-buffer-dump") {
			if (!ParseBool(value, options.config.command_buffer_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--command-buffer-dump-folder") {
			options.config.command_buffer_dump_folder = value;
		} else if (arg == "--graphics-debug-dump") {
			if (!ParseBool(value, options.config.graphics_debug_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--validate-shader-ir") {
			if (!ParseBool(value, options.config.validate_shader_ir_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--approximate-divergent-phi") {
			if (!ParseBool(value, options.config.approximate_divergent_phi_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--draw-dump") {
			if (!ParseBool(value, options.config.draw_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--draw-dump-folder") {
			options.config.draw_dump_folder = value;
		} else if (arg == "--draw-log-frame-first") {
			int64_t    frame  = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frame);
			if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()) {
				::printf("invalid number for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
			options.config.draw_log_frame_first = frame;
		} else if (arg == "--draw-log-frame-last") {
			int64_t    frame  = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frame);
			if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()) {
				::printf("invalid number for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
			options.config.draw_log_frame_last = frame;
		} else if (arg == "--present-dump") {
			if (!ParseBool(value, options.config.present_dump_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--present-dump-every") {
			int64_t    every  = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), every);
			if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()) {
				::printf("invalid number for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
			options.config.present_dump_every = every;
		} else if (arg == "--present-dump-folder") {
			options.config.present_dump_folder = value;
		} else if (arg == "--input-script") {
			value = Common::FixFilenameSlash(value);
			if (!Common::File::IsFileExisting(value)) {
				::printf("--input-script must point to an existing file: %s\n", value.c_str());
				return false;
			}
			options.config.input_script_path = value;
		} else if (arg == "--printf-direction") {
			if (!ParseEnum(value, options.config.printf_direction)) {
				::printf("invalid printf direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--printf-output-file") {
			options.config.printf_output_file = value;
		} else if (arg == "--profiler-direction") {
			if (!ParseEnum(value, options.config.profiler_direction)) {
				::printf("invalid profiler direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--spirv-debug-printf") {
			if (!ParseBool(value, options.config.spirv_debug_printf_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--readback-linear-images") {
			if (!ParseBool(value, options.config.readback_linear_images)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--keymap") {
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				::printf("invalid keymap: %s\n", value.c_str());
				return false;
			}
			options.config.keymap.push_back(value);
		} else if (arg == "--gamepad-map") {
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				::printf("invalid gamepad-map: %s\n", value.c_str());
				return false;
			}
			options.config.gamepad_keymap.push_back(value);
		} else if (arg == "--gamepad-deadzone") {
			// Not std::from_chars: Apple's libc++ marks the floating-point overloads
			// unavailable before macOS 26 (a real SDK availability annotation, not a
			// deployment-target guard we can raise around it), and this needs to build
			// on the macOS 15 CI runner. strtof carries none of that restriction and is
			// available everywhere else this project targets; every other from_chars
			// call in this file parses an integer, which macOS has always supported.
			errno          = 0;
			char*      end = nullptr;
			const auto deadzone = std::strtof(value.c_str(), &end);
			if (end == value.c_str() || errno == ERANGE || end != value.c_str() + value.size() ||
			    deadzone < 0.0f || deadzone > 0.95f) {
				::printf("invalid gamepad-deadzone (expected 0.0-0.95): %s\n", value.c_str());
				return false;
			}
			options.config.gamepad_deadzone = deadzone;
		} else {
			::printf("unknown option: %s\n", arg.c_str());
			return false;
		}
	}

	if (options.config.gpu_assisted_validation_enabled) {
		options.config.vulkan_validation_enabled = true;
	}

	return show_help || show_diagnostics || (!options.app0_dir.empty() && !options.elf.empty());
}

int main(int argc, char* argv[]) {
	VirtualMemory::Init();
	InitializeThreads();

	RunOptions options;
	bool       show_help        = false;
	bool       show_diagnostics = false;

	if (argc < 2) {
		PrintUsage();
		return 0;
	}

	if (!ParseArgs(argc, argv, options, show_help, show_diagnostics)) {
		PrintUsage();
		return 1;
	}

	if (show_help) {
		PrintUsage();
		return 0;
	}

	// Before anything that needs a game: the report is most wanted by people
	// whose emulator will not start at all.
	if (show_diagnostics) {
		::printf("%s", Common::Diagnostics::BuildReport().c_str());
		::printf("%s", Libs::Graphics::BuildVulkanReport().c_str());
		return 0;
	}

	Run(options);

	return 0;
}
