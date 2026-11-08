#include "libs/httpOfflineServices.h"

namespace Libs::Network::OfflineServices {

bool Handle(const Request& /*request*/, Response& /*response*/) {
	// No service answers here yet: the caller fails the transfer as an unreachable server does.
	return false;
}

} // namespace Libs::Network::OfflineServices
