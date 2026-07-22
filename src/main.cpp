#include "common/common.h"
#include "common/commonSubsystem.h"
#include "common/dateTime.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/magicEnum.h"
#include "common/platform/sysDbg.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "emulator.h"
#include "kytyGitVersion.h"

#include <charconv>
#include <cstdio>
#include <fmt/format.h>
#include <string_view>
#include <vector>

using namespace Common;
using namespace Emulator;

static std::string GetBuildString() {
	Date date = Date::FromMacros(std::string(__DATE__));

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	std::string type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	std::string type = "Release";
#else
	std::string type = "????";
#endif

	std::string compiler =
	    Debug::GetCompiler() + "-" + Debug::GetLinker() + "-" + Debug::GetBitness();

	std::string str =
	    fmt::format("{}, {}, ver = {}, git = {}, date = {}", type.c_str(), compiler.c_str(),
	                KYTY_VERSION, KYTY_GIT_VERSION, date.ToString().c_str());

	return str;
}

static void PrintUsage() {
	::printf("%s\n", GetBuildString().c_str());
	::printf("kyty_emulator --game <dir|elf> [options]\n\n");
	::printf("Options:\n");
	::printf("  --game <dir|elf>                     Game directory or ELF to load.\n");
	::printf("  --screen-width <num>                 Window width. Default: 1280.\n");
	::printf("  --screen-height <num>                Window height. Default: 720.\n");
	::printf("  --vblank-frequency <num>             Virtual vblank frequency. Default: 60.\n");
	::printf("  --vulkan-validation <true|false>     Enable Vulkan validation.\n");
	::printf("  --shader-validation <true|false>     Enable shader validation.\n");
	::printf("  --shader-optimization-type <value>   None, Size, or Performance.\n");
	::printf("  --shader-log-direction <value>       Silent, Console, or File.\n");
	::printf("  --shader-log-folder <path>           Shader log output folder.\n");
	::printf("  --command-buffer-dump <true|false>   Enable command buffer dumps.\n");
	::printf("  --command-buffer-dump-folder <path>  Command buffer dump folder.\n");
	::printf("  --graphics-debug-dump <true|false>   Enable graphics debug dumps.\n");
	::printf("  --printf-direction <value>           Silent, Console, or File.\n");
	::printf("  --printf-output-file <path>          Guest printf output file.\n");
	::printf("  --profiler-direction <value>         None or Network.\n");
	::printf("  --spirv-debug-printf <true|false>    Enable SPIR-V debug printf.\n");
	::printf("  --ngg-rectlist-draw <true|false>     Draw rect-list auto draws using the NGG "
	         "4-vertex path.\n");
	::printf("  --rd                                 Enable RenderDoc capture.\n");
	::printf("  --gpu-capture <path>                 Record a commands-only GPU trace.\n");
	::printf("  --regression-record <path>           Record selected presented frames.\n");
	::printf("  --regression-compare <path>          Compare against a frame baseline.\n");
	::printf("  --regression-report <path>           Comparison report path.\n");
	::printf("  --regression-frames <n[,n...]>       Zero-based presentation ordinals.\n");
	::printf("  --regression-test-id <id>            Stable game/revision/checkpoint identity.\n");
	::printf("  --regression-save-raw <true|false>   Save raw frames and previews.\n");
	::printf("  --regression-exit-on-complete <true|false> Exit after selected frames.\n");
	::printf("  --regression-allow-environment-mismatch <true|false> Override provenance checks.\n");
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

static bool ParseFrameOrdinals(std::string_view value, std::vector<uint64_t>& out) {
	out.clear();
	while (!value.empty()) {
		const auto comma = value.find(',');
		const auto token = value.substr(0, comma);
		uint64_t   ordinal = 0;
		const auto result = std::from_chars(token.data(), token.data() + token.size(), ordinal);
		if (token.empty() || result.ec != std::errc {} ||
		    result.ptr != token.data() + token.size()) {
			return false;
		}
		out.push_back(ordinal);
		if (comma == std::string_view::npos) {
			break;
		}
		if (comma + 1 == value.size()) {
			return false;
		}
		value.remove_prefix(comma + 1);
	}
	return !out.empty();
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

static bool ParseArgs(int argc, char* argv[], RunOptions& options, bool& show_help) {
	show_help = false;

	for (int i = 1; i < argc; i++) {
		std::string arg = std::string(argv[i]);
		std::string value;

		if (arg == "--help" || arg == "-h") {
			show_help = true;
			continue;
		}

		if (arg == "--rd") {
			options.config.renderdoc_enabled = true;
			continue;
		}

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
		} else if (arg == "--screen-width") {
			options.config.screen_width = static_cast<uint32_t>(Common::ToInt32(value));
		} else if (arg == "--screen-height") {
			options.config.screen_height = static_cast<uint32_t>(Common::ToInt32(value));
		} else if (arg == "--vblank-frequency") {
			const int32_t vblank_frequency = Common::ToInt32(value);
			options.config.vblank_frequency =
			    static_cast<uint32_t>(vblank_frequency < 0 ? 0 : vblank_frequency);
		} else if (arg == "--vulkan-validation") {
			if (!ParseBool(value, options.config.vulkan_validation_enabled)) {
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
		} else if (arg == "--ngg-rectlist-draw") {
			if (!ParseBool(value, options.config.ngg_rectlist_draw_enabled)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--gpu-capture") {
			options.config.gpu_capture_file = value;
		} else if (arg == "--regression-record" || arg == "--regression-compare") {
			if (options.config.frame_regression_mode != Config::FrameRegressionMode::None) {
				::printf("regression mode can only be specified once\n");
				return false;
			}
			options.config.frame_regression_mode =
			    arg == "--regression-record" ? Config::FrameRegressionMode::Record
			                                 : Config::FrameRegressionMode::Compare;
			options.config.frame_regression_baseline = value;
		} else if (arg == "--regression-report") {
			options.config.frame_regression_report = value;
		} else if (arg == "--regression-frames") {
			if (!ParseFrameOrdinals(value, options.config.frame_regression_frames)) {
				::printf("invalid frame ordinal list: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--regression-test-id") {
			options.config.frame_regression_test_id = value;
		} else if (arg == "--regression-save-raw") {
			if (!ParseBool(value, options.config.frame_regression_save_raw)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--regression-exit-on-complete") {
			if (!ParseBool(value, options.config.frame_regression_exit_on_complete)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else if (arg == "--regression-allow-environment-mismatch") {
			if (!ParseBool(value, options.config.frame_regression_allow_environment_mismatch)) {
				::printf("invalid boolean for %s: %s\n", arg.c_str(), value.c_str());
				return false;
			}
		} else {
			::printf("unknown option: %s\n", arg.c_str());
			return false;
		}
	}

	if (options.config.frame_regression_mode != Config::FrameRegressionMode::None &&
	    options.config.frame_regression_frames.empty()) {
		::printf("--regression-frames is required with a regression mode\n");
		return false;
	}
	if (options.config.frame_regression_mode != Config::FrameRegressionMode::None &&
	    options.config.frame_regression_test_id.empty()) {
		::printf("--regression-test-id is required with a regression mode\n");
		return false;
	}
	std::string path_error;
	if (!Config::ValidateFrameRegressionPaths(options.config, path_error)) {
		::printf("%s\n", path_error.c_str());
		return false;
	}

	return show_help || (!options.app0_dir.empty() && !options.elf.empty());
}

int main(int argc, char* argv[]) {
	auto& slist = *SubsystemsList::Instance();

	slist.SetArgs(argc, argv);

	auto* core    = CommonSubsystem::Instance();
	auto* threads = ThreadsSubsystem::Instance();

	slist.Add(core, {});
	slist.Add(threads, {core});

	if (!slist.InitAll(false)) {
		::printf("Failed to initialize '%s' subsystem: %s\n", slist.GetFailName(),
		         slist.GetFailMsg());
		return 1;
	}

	RunOptions options;
	bool       show_help = false;

	if (argc < 2) {
		PrintUsage();
		slist.DestroyAll(false);
		return 0;
	}

	if (!ParseArgs(argc, argv, options, show_help)) {
		PrintUsage();
		slist.DestroyAll(false);
		return 1;
	}

	if (show_help) {
		PrintUsage();
		slist.DestroyAll(false);
		return 0;
	}

	Run(options);

	slist.DestroyAll(false);

	return 0;
}
