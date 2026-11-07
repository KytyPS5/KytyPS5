#ifndef EMULATOR_SRC_LIBS_EA_EMULATION_PATCH_PLAN_H_
#define EMULATOR_SRC_LIBS_EA_EMULATION_PATCH_PLAN_H_

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::EaEmulation {

// The modulus (RSA 2048) of "GOS 2015 Certificate Authority", the authority that EA's Blaze client
// trusts. A game that talks to EA's game servers has it in its data.
const std::vector<uint8_t>& AuthorityModulus();

// A patch plan (GoldHEN-style JSON, see loader/gamePatch.h) for the game that is loaded: it
// replaces EA's authority key, wherever the game keeps it, by `new_modulus`. The title, the version
// and the process are the game's own, so the plan fits any game and any version. Empty when
// `new_modulus` is not the size of EA's key.
std::string MakePatchPlan(const std::string& title_id, const std::string& version, const std::string& process,
                          const std::vector<uint8_t>& new_modulus);

// Whether the server starts for a game that holds EA's key: for every such game while EA emulation
// is on (Config::EaEmulationEnabled). A game without a title gets none.
bool ShouldStartServer(const std::string& title_id, bool ea_emulation_enabled);

std::string Hex(const std::vector<uint8_t>& bytes);

} // namespace Libs::EaEmulation

#endif // EMULATOR_SRC_LIBS_EA_EMULATION_PATCH_PLAN_H_
