#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_

#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <filesystem>

namespace Emulator {

struct RunOptions {
	Config::ConfigOptions config;
	std::filesystem::path app0_dir;
	std::filesystem::path elf;
	// M1W3: when true, the rendering thread prints a once-per-second
	// FPS summary to stdout (e.g. "[FPS] 23.4 frames/sec (t=15.0s)").
	// Independent of any in-window text overlay; intended for headless
	// measurement sessions.
	bool show_fps = false;
};

void Run(const RunOptions& options);

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
