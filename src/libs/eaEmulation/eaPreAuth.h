#ifndef EMULATOR_SRC_LIBS_EA_EMULATION_PRE_AUTH_H_
#define EMULATOR_SRC_LIBS_EA_EMULATION_PRE_AUTH_H_

#include <cstdint>
#include <vector>

namespace Libs::EaEmulation {

// The client secret of the reply. A game of EA signs in to its account services only with a
// secret in its settings, and nothing checks the value: this fixed placeholder is not a secret.
inline constexpr const char* CLIENT_SECRET_PLACEHOLDER = "kyty";

// The reply to Util.preAuth (component 9, command 7), the first call of a Blaze client: the
// configuration of the server. It is built from the request, so it fits any game of EA. The names
// of the game (service, client, environment, SDK version) come from its CDAT and CINF; everything
// else is the same for every game: the standard Blaze components, client settings with no secrets,
// EA's sign-in addresses, and no QoS ping sites (the game finds no servers to play on and stays
// offline).
std::vector<uint8_t> PreAuthReply(const std::vector<uint8_t>& request);

// Blaze component 9 (Util) and the commands the server answers.
inline constexpr uint16_t UTIL_COMPONENT                         = 0x0009;
inline constexpr uint16_t UTIL_PRE_AUTH                          = 0x0007;
inline constexpr uint16_t UTIL_FETCH_CLIENT_CONFIG               = 0x0001;
inline constexpr uint16_t UTIL_FETCH_CLIENT_CONFIG_AUTHENTICATED = 0x001e;
inline constexpr uint16_t UTIL_POST_AUTH                         = 0x0008;

// The reply to Util.postAuth, the call after a login: PostAuthResponse with no telemetry server, no
// ticker server and the default user options. A game waits for it before it counts as signed in.
std::vector<uint8_t> PostAuthReply();

// The reply to Util.fetchClientConfig and fetchClientConfigAuthenticated: FetchConfigResponse with
// an empty CONF map. The server holds no game's own configuration (the request names it in CFID),
// so the game keeps its built-in values.
std::vector<uint8_t> FetchConfigReply();

} // namespace Libs::EaEmulation

#endif // EMULATOR_SRC_LIBS_EA_EMULATION_PRE_AUTH_H_
