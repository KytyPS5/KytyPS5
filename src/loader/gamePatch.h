#ifndef KYTY_LOADER_GAME_PATCH_H_
#define KYTY_LOADER_GAME_PATCH_H_

#include "common/common.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Loader {

struct Program;

namespace GamePatch {

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs);
// A plan the emulator made itself (a JSON string of the same form). The first write's old bytes
// are found by search, so its offset is not used. False: it does not match the loaded game, or
// could not be applied; the caller goes on without it.
bool ApplyBuiltIn(const std::string& json, Program* main_program, const std::vector<Program*>& programs);
// Fixes the emulator knows for games it cannot run as they are (see gamePatch.cpp). Each is keyed by
// the title and by the code it changes, so it only applies to the build it was made for.
// KYTY_GAME_FIXES=0 turns them off. True: one was applied.
bool ApplyKnownFixes(Program* main_program, const std::vector<Program*>& programs);
// A module of the game holds these bytes in its loaded data.
bool ContainsBytes(const std::vector<Program*>& programs, const std::vector<uint8_t>& bytes);
bool ApplyPending(Program* program);
void Clear();

} // namespace GamePatch

} // namespace Loader

#endif // KYTY_LOADER_GAME_PATCH_H_
