#ifndef EMULATOR_INCLUDE_EMULATOR_EA_EMULATION_UPSTREAM_H_
#define EMULATOR_INCLUDE_EMULATOR_EA_EMULATION_UPSTREAM_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A connection to EA's real Blaze server, for the calls the built-in server does not answer yet
// (KYTY_EA_UPSTREAM=1). It asks EA's server picker for a server with the game's own question,
// and does not check the certificate of the server.
namespace Libs::EaEmulation {

// A game's question to EA's server picker: the picker's host, and the body of its getServerInstance
// request.
struct ServerPickerQuestion {
	std::string host;
	std::string body;
};

class Upstream {
public:
	Upstream();
	~Upstream();
	Upstream(const Upstream&)            = delete;
	Upstream& operator=(const Upstream&) = delete;

	// Asks the server picker the game's `question`, and connects to the server it names. False: no
	// question yet, no server was found, or the connection failed. Call once.
	bool Connect(const ServerPickerQuestion& question);

	// Sends a request frame (header and payload) and reads the reply frame into `reply`.
	bool Exchange(const std::vector<uint8_t>& request, std::vector<uint8_t>& reply);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::EaEmulation

#endif // EMULATOR_INCLUDE_EMULATOR_EA_EMULATION_UPSTREAM_H_
