#ifndef KYTY_LOADER_GAME_PATCH_H_
#define KYTY_LOADER_GAME_PATCH_H_

#include "common/common.h"

#include <filesystem>

namespace Loader {

struct Program;

namespace GamePatch {

bool Apply(const std::filesystem::path& plan_path, Program* program);

} // namespace GamePatch

} // namespace Loader

#endif // KYTY_LOADER_GAME_PATCH_H_
