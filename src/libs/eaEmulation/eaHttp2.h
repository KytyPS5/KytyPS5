#ifndef EMULATOR_INCLUDE_EMULATOR_EA_EMULATION_HTTP2_H_
#define EMULATOR_INCLUDE_EMULATOR_EA_EMULATION_HTTP2_H_

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The HTTP/2 side of EA's account services (gRPC), inside the emulator. The service directory
// that the emulator answers names these services with http:// addresses, so a game speaks HTTP/2
// to them in cleartext, with prior knowledge (RFC 9113, section 3.3): no TLS and no upgrade.
namespace Libs::EaEmulation {

using Header  = std::pair<std::string, std::string>;
using Headers = std::vector<Header>;

// Reads header blocks (RFC 7541). One decoder per connection: its dynamic table carries over
// from block to block.
class HpackDecoder {
public:
	// False when the block is malformed; `headers` then holds what was read before the error.
	bool Decode(std::span<const uint8_t> block, Headers& headers);

private:
	bool Entry(uint64_t index, Header& header) const;
	void Insert(Header header);
	// Drops the oldest entries until the table fits its size.
	void Evict();

	// The newest entry first, as the RFC indexes them.
	std::vector<Header> m_dynamic;
	size_t              m_size     = 0;
	size_t              m_max_size = 4096;
};

// A header block of literals that the peer does not index, without Huffman coding.
std::vector<uint8_t> HpackEncode(const Headers& headers);

// A gRPC call: the method's path (/package.Service/Method) and the request message. The handler
// sets the reply message and returns true, or returns false for a method it does not answer.
using GrpcHandler = std::function<bool(const std::string& path, std::span<const uint8_t> request,
                                       std::vector<uint8_t>& reply)>;

// Serves one HTTP/2 connection on the connected socket `fd` until the peer closes it, then
// closes the socket. Every stream is a unary gRPC call.
void ServeGrpcConnection(int fd, const GrpcHandler& handler);

} // namespace Libs::EaEmulation

#endif // EMULATOR_INCLUDE_EMULATOR_EA_EMULATION_HTTP2_H_
