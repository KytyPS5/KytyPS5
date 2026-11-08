#ifndef EMULATOR_INCLUDE_EMULATOR_HTTP_OFFLINE_SERVICES_H_
#define EMULATOR_INCLUDE_EMULATOR_HTTP_OFFLINE_SERVICES_H_

#include <string>
#include <vector>

// Answers the HTTP requests a game sends to its publisher's services, from inside the emulator.
// No packet leaves the machine, and no player is signed in to any service.
namespace Libs::Network::OfflineServices {

struct Header {
	std::string name;
	std::string value;
};

struct Request {
	std::string         method;
	std::string         host;
	std::string         path;
	std::string         body;
	std::vector<Header> headers;
};

struct Response {
	int                 status = 0;
	std::vector<Header> headers;
	std::string         body;
};

// True: the request is answered in `response`. False: no service here knows the host or the path,
// and the caller fails the transfer the way an unreachable server does.
bool Handle(const Request& request, Response& response);

} // namespace Libs::Network::OfflineServices

#endif /* EMULATOR_INCLUDE_EMULATOR_HTTP_OFFLINE_SERVICES_H_ */
