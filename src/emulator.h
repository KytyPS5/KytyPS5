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
	std::filesystem::path game_patch;
	uint32_t wait_for_process = 0;
};

void SetHostArguments(int argc, char* argv[]);
int LoadExec(const char* path, const char* const argv[]);
void Run(const RunOptions& options);

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
