#ifndef KYTY_LOADER_GAME_PATCH_H_
#define KYTY_LOADER_GAME_PATCH_H_

#include "common/common.h"

#include <filesystem>
#include <vector>

namespace Loader {

struct Program;

namespace GamePatch {

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs);
bool ApplyPending(Program* program);
void Clear();

} // namespace GamePatch

} // namespace Loader

#endif // KYTY_LOADER_GAME_PATCH_H_
