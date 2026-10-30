#ifndef EMULATOR_SRC_LIBS_EA_EMULATION_PRE_AUTH_H_
#define EMULATOR_SRC_LIBS_EA_EMULATION_PRE_AUTH_H_

#include <cstdint>
#include <vector>

namespace Libs::EaEmulation {

// The reply to Util.preAuth (component 9, command 7), the first call of a Blaze client: the
// configuration of the server. It is built from the request, so it fits any game of EA. The names of
// the game (service, client, environment, SDK version) come from its CDAT and CINF; everything else
// is the same for every game: the standard Blaze components, client settings with no secrets, EA's
// sign-in addresses, and no QoS ping sites (the game finds no servers to play on and stays offline).
std::vector<uint8_t> PreAuthReply(const std::vector<uint8_t>& request);

} // namespace Libs::EaEmulation

#endif // EMULATOR_SRC_LIBS_EA_EMULATION_PRE_AUTH_H_
