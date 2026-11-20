#ifndef KYTY_LOADER_GAME_ARGS_H_
#define KYTY_LOADER_GAME_ARGS_H_

#include <cstdint>
#include <filesystem>
#include <optional>

// Engine console variables through the game's own command-line file. Demon's Souls passes every
// "+name=value" line of the PackageCmdLineArgs.txt it ships to its console at boot: a read-only
// open of that file gets a copy with lines added (the game's files stay as they are). The lines are
// Demon's Souls' defaults (see gameArgs.cpp) and those of KYTY_GAME_CONVARS="name=value;..." (','
// also separates entries; a name given there replaces the default's value; "0" adds none at all).
namespace Loader::GameArgs {

// The host file a read-only open of `real` should open instead (empty: `real` itself).
std::filesystem::path RedirectRead(const std::filesystem::path& real);
// A stat of `real`: the size of the copy a read would get (nothing: its own).
std::optional<uint64_t> RedirectedSize(const std::filesystem::path& real);

} // namespace Loader::GameArgs

#endif /* KYTY_LOADER_GAME_ARGS_H_ */
