#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERREPLAY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERREPLAY_H_

#include "common/common.h"

// Offline shader tools. They run inside the emulator binary before any emulator subsystem starts,
// so a captured shader (see shaderCapture.h) compiles in about a second without the game.
//
//   --shader-replay <capture> [--out file.spv] [--no-dump] [--no-validate]
//   --shader-replay-all <captures> [--jobs N] [--timeout seconds] [--dump]
//   --shader-disasm <code.bin|capture> [--pc 0x86c] [--window 20]
//
// Exit status: 0 success, 64 bad input, 65 fatal error (same as in game), 66 resource
// materialization failed, 67 SPIR-V validation failed. --shader-replay-all exits 1 if any capture
// failed.
namespace Libs::Graphics {

inline constexpr int SHADER_REPLAY_EXIT_USAGE       = 64;
inline constexpr int SHADER_REPLAY_EXIT_FATAL       = 65;
inline constexpr int SHADER_REPLAY_EXIT_MATERIALIZE = 66;
inline constexpr int SHADER_REPLAY_EXIT_VALIDATE    = 67;

[[nodiscard]] bool IsShaderToolCommand(const char* arg);

// `argv[1]` must satisfy IsShaderToolCommand. Returns the process exit status.
[[nodiscard]] int RunShaderToolCommand(int argc, char* argv[]);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERREPLAY_H_ */
