#ifndef EMULATOR_INCLUDE_EMULATOR_EA_EMULATION_NEXUS_H_
#define EMULATOR_INCLUDE_EMULATOR_EA_EMULATION_NEXUS_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// A stand-in for EA's account services (eadp.nexus.connect.grpc.v1, "Nucleus connect"), inside
// the emulator. A game of EA signs in there before it logs in to its Blaze server: it trades the
// platform's authorization code for an EA one, then the EA code for an access token. Without an
// answer the game stays signed out and offers only its offline menus, with "lost your online
// connection" and trial-version notices.
namespace Libs::EaEmulation {

// The host the service directory names for these services (httpOfflineServices.cpp).
inline constexpr const char* NEXUS_HOST = "accounts.grpc.ea.com";

// The reply to the gRPC call `path`, or false for a call the stand-in does not answer.
bool NexusReply(const std::string& path, std::span<const uint8_t> request,
                std::vector<uint8_t>& reply);

// The fields of a protobuf message, one per line, for the log (KYTY_EA_TRACE).
std::string DescribeProtobuf(std::span<const uint8_t> message, int depth = 0);

} // namespace Libs::EaEmulation

#endif // EMULATOR_INCLUDE_EMULATOR_EA_EMULATION_NEXUS_H_
