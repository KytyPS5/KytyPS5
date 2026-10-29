#ifndef EMULATOR_SRC_LIBS_NP_WEB_API2_ENTITLEMENTS_H_
#define EMULATOR_SRC_LIBS_NP_WEB_API2_ENTITLEMENTS_H_

#include <string>

namespace Libs::Network {

// The body of a PSN entitlements answer for a signed-in account, for the request `path`: every
// entitlement the request names (the path segment after "/entitlements/", or the comma-separated
// list in "entitlementIds="), owned and active. A request that names none gets an empty list. A
// name that is not letters, digits, '_' and '-' is skipped.
std::string OwnedEntitlementsJson(const std::string& path);

} // namespace Libs::Network

#endif // EMULATOR_SRC_LIBS_NP_WEB_API2_ENTITLEMENTS_H_
