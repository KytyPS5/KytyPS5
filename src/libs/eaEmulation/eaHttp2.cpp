#include "libs/eaEmulation/eaHttp2.h"

#include "common/logging/log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <map>
#include <unordered_map>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Libs::EaEmulation {

namespace {

struct HuffmanCode {
	uint32_t code;
	uint8_t  bits;
};

// RFC 7541 Appendix B: the code of each symbol (0-255, then EOS) and its length in bits.
constexpr std::array<HuffmanCode, 257> HUFFMAN = {{
    {0x1ff8, 13},     {0x7fffd8, 23},   {0xfffffe2, 28},  {0xfffffe3, 28},  {0xfffffe4, 28},
    {0xfffffe5, 28},  {0xfffffe6, 28},  {0xfffffe7, 28},  {0xfffffe8, 28},  {0xffffea, 24},
    {0x3ffffffc, 30}, {0xfffffe9, 28},  {0xfffffea, 28},  {0x3ffffffd, 30}, {0xfffffeb, 28},
    {0xfffffec, 28},  {0xfffffed, 28},  {0xfffffee, 28},  {0xfffffef, 28},  {0xffffff0, 28},
    {0xffffff1, 28},  {0xffffff2, 28},  {0x3ffffffe, 30}, {0xffffff3, 28},  {0xffffff4, 28},
    {0xffffff5, 28},  {0xffffff6, 28},  {0xffffff7, 28},  {0xffffff8, 28},  {0xffffff9, 28},
    {0xffffffa, 28},  {0xffffffb, 28},  {0x14, 6},        {0x3f8, 10},      {0x3f9, 10},
    {0xffa, 12},      {0x1ff9, 13},     {0x15, 6},        {0xf8, 8},        {0x7fa, 11},
    {0x3fa, 10},      {0x3fb, 10},      {0xf9, 8},        {0x7fb, 11},      {0xfa, 8},
    {0x16, 6},        {0x17, 6},        {0x18, 6},        {0x0, 5},         {0x1, 5},
    {0x2, 5},         {0x19, 6},        {0x1a, 6},        {0x1b, 6},        {0x1c, 6},
    {0x1d, 6},        {0x1e, 6},        {0x1f, 6},        {0x5c, 7},        {0xfb, 8},
    {0x7ffc, 15},     {0x20, 6},        {0xffb, 12},      {0x3fc, 10},      {0x1ffa, 13},
    {0x21, 6},        {0x5d, 7},        {0x5e, 7},        {0x5f, 7},        {0x60, 7},
    {0x61, 7},        {0x62, 7},        {0x63, 7},        {0x64, 7},        {0x65, 7},
    {0x66, 7},        {0x67, 7},        {0x68, 7},        {0x69, 7},        {0x6a, 7},
    {0x6b, 7},        {0x6c, 7},        {0x6d, 7},        {0x6e, 7},        {0x6f, 7},
    {0x70, 7},        {0x71, 7},        {0x72, 7},        {0xfc, 8},        {0x73, 7},
    {0xfd, 8},        {0x1ffb, 13},     {0x7fff0, 19},    {0x1ffc, 13},     {0x3ffc, 14},
    {0x22, 6},        {0x7ffd, 15},     {0x3, 5},         {0x23, 6},        {0x4, 5},
    {0x24, 6},        {0x5, 5},         {0x25, 6},        {0x26, 6},        {0x27, 6},
    {0x6, 5},         {0x74, 7},        {0x75, 7},        {0x28, 6},        {0x29, 6},
    {0x2a, 6},        {0x7, 5},         {0x2b, 6},        {0x76, 7},        {0x2c, 6},
    {0x8, 5},         {0x9, 5},         {0x2d, 6},        {0x77, 7},        {0x78, 7},
    {0x79, 7},        {0x7a, 7},        {0x7b, 7},        {0x7ffe, 15},     {0x7fc, 11},
    {0x3ffd, 14},     {0x1ffd, 13},     {0xffffffc, 28},  {0xfffe6, 20},    {0x3fffd2, 22},
    {0xfffe7, 20},    {0xfffe8, 20},    {0x3fffd3, 22},   {0x3fffd4, 22},   {0x3fffd5, 22},
    {0x7fffd9, 23},   {0x3fffd6, 22},   {0x7fffda, 23},   {0x7fffdb, 23},   {0x7fffdc, 23},
    {0x7fffdd, 23},   {0x7fffde, 23},   {0xffffeb, 24},   {0x7fffdf, 23},   {0xffffec, 24},
    {0xffffed, 24},   {0x3fffd7, 22},   {0x7fffe0, 23},   {0xffffee, 24},   {0x7fffe1, 23},
    {0x7fffe2, 23},   {0x7fffe3, 23},   {0x7fffe4, 23},   {0x1fffdc, 21},   {0x3fffd8, 22},
    {0x7fffe5, 23},   {0x3fffd9, 22},   {0x7fffe6, 23},   {0x7fffe7, 23},   {0xffffef, 24},
    {0x3fffda, 22},   {0x1fffdd, 21},   {0xfffe9, 20},    {0x3fffdb, 22},   {0x3fffdc, 22},
    {0x7fffe8, 23},   {0x7fffe9, 23},   {0x1fffde, 21},   {0x7fffea, 23},   {0x3fffdd, 22},
    {0x3fffde, 22},   {0xfffff0, 24},   {0x1fffdf, 21},   {0x3fffdf, 22},   {0x7fffeb, 23},
    {0x7fffec, 23},   {0x1fffe0, 21},   {0x1fffe1, 21},   {0x3fffe0, 22},   {0x1fffe2, 21},
    {0x7fffed, 23},   {0x3fffe1, 22},   {0x7fffee, 23},   {0x7fffef, 23},   {0xfffea, 20},
    {0x3fffe2, 22},   {0x3fffe3, 22},   {0x3fffe4, 22},   {0x7ffff0, 23},   {0x3fffe5, 22},
    {0x3fffe6, 22},   {0x7ffff1, 23},   {0x3ffffe0, 26},  {0x3ffffe1, 26},  {0xfffeb, 20},
    {0x7fff1, 19},    {0x3fffe7, 22},   {0x7ffff2, 23},   {0x3fffe8, 22},   {0x1ffffec, 25},
    {0x3ffffe2, 26},  {0x3ffffe3, 26},  {0x3ffffe4, 26},  {0x7ffffde, 27},  {0x7ffffdf, 27},
    {0x3ffffe5, 26},  {0xfffff1, 24},   {0x1ffffed, 25},  {0x7fff2, 19},    {0x1fffe3, 21},
    {0x3ffffe6, 26},  {0x7ffffe0, 27},  {0x7ffffe1, 27},  {0x3ffffe7, 26},  {0x7ffffe2, 27},
    {0xfffff2, 24},   {0x1fffe4, 21},   {0x1fffe5, 21},   {0x3ffffe8, 26},  {0x3ffffe9, 26},
    {0xffffffd, 28},  {0x7ffffe3, 27},  {0x7ffffe4, 27},  {0x7ffffe5, 27},  {0xfffec, 20},
    {0xfffff3, 24},   {0xfffed, 20},    {0x1fffe6, 21},   {0x3fffe9, 22},   {0x1fffe7, 21},
    {0x1fffe8, 21},   {0x7ffff3, 23},   {0x3fffea, 22},   {0x3fffeb, 22},   {0x1ffffee, 25},
    {0x1ffffef, 25},  {0xfffff4, 24},   {0xfffff5, 24},   {0x3ffffea, 26},  {0x7ffff4, 23},
    {0x3ffffeb, 26},  {0x7ffffe6, 27},  {0x3ffffec, 26},  {0x3ffffed, 26},  {0x7ffffe7, 27},
    {0x7ffffe8, 27},  {0x7ffffe9, 27},  {0x7ffffea, 27},  {0x7ffffeb, 27},  {0xffffffe, 28},
    {0x7ffffec, 27},  {0x7ffffed, 27},  {0x7ffffee, 27},  {0x7ffffef, 27},  {0x7fffff0, 27},
    {0x3ffffee, 26},  {0x3fffffff, 30},
}};

// RFC 7541 Appendix A.
constexpr std::array<std::pair<const char*, const char*>, 61> STATIC_TABLE = {{
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
}};

// The symbol of each code, by its length and value.
const std::unordered_map<uint64_t, uint16_t>& HuffmanSymbols() {
	static const auto symbols = [] {
		std::unordered_map<uint64_t, uint16_t> map;
		for (uint16_t symbol = 0; symbol < HUFFMAN.size(); symbol++) {
			map.emplace((static_cast<uint64_t>(HUFFMAN[symbol].bits) << 32) | HUFFMAN[symbol].code,
			            symbol);
		}
		return map;
	}();
	return symbols;
}

bool HuffmanDecode(std::span<const uint8_t> data, std::string& text) {
	const auto& symbols = HuffmanSymbols();
	uint32_t    code    = 0;
	uint8_t     bits    = 0;
	for (const uint8_t byte: data) {
		for (int bit = 7; bit >= 0; bit--) {
			code = (code << 1u) | ((byte >> bit) & 1u);
			bits++;
			if (bits > 30) {
				return false;
			}
			const auto found = symbols.find((static_cast<uint64_t>(bits) << 32) | code);
			if (found != symbols.end()) {
				if (found->second == 256) {
					return false; // EOS inside a string is an error (RFC 7541, 5.2).
				}
				text.push_back(static_cast<char>(found->second));
				code = 0;
				bits = 0;
			}
		}
	}
	// The padding is the most significant bits of EOS: up to seven ones.
	return bits < 8 && code == (1u << bits) - 1u;
}

bool ReadInteger(std::span<const uint8_t> data, size_t& pos, uint8_t prefix_bits, uint64_t& value) {
	if (pos >= data.size()) {
		return false;
	}
	const uint8_t max = static_cast<uint8_t>((1u << prefix_bits) - 1u);
	value             = data[pos++] & max;
	if (value < max) {
		return true;
	}
	for (int shift = 0; shift < 63; shift += 7) {
		if (pos >= data.size()) {
			return false;
		}
		const uint8_t byte = data[pos++];
		value += static_cast<uint64_t>(byte & 0x7fu) << shift;
		if ((byte & 0x80u) == 0) {
			return true;
		}
	}
	return false;
}

bool ReadString(std::span<const uint8_t> data, size_t& pos, std::string& text) {
	if (pos >= data.size()) {
		return false;
	}
	const bool huffman = (data[pos] & 0x80u) != 0;
	uint64_t   length  = 0;
	if (!ReadInteger(data, pos, 7, length) || length > data.size() - pos) {
		return false;
	}
	const auto bytes = data.subspan(pos, length);
	pos += length;
	if (huffman) {
		return HuffmanDecode(bytes, text);
	}
	text.assign(bytes.begin(), bytes.end());
	return true;
}

void WriteInteger(std::vector<uint8_t>& out, uint8_t first, uint8_t prefix_bits, uint64_t value) {
	const uint8_t max = static_cast<uint8_t>((1u << prefix_bits) - 1u);
	if (value < max) {
		out.push_back(static_cast<uint8_t>(first | value));
		return;
	}
	out.push_back(static_cast<uint8_t>(first | max));
	value -= max;
	while (value >= 0x80) {
		out.push_back(static_cast<uint8_t>((value & 0x7fu) | 0x80u));
		value >>= 7;
	}
	out.push_back(static_cast<uint8_t>(value));
}

void WriteString(std::vector<uint8_t>& out, const std::string& text) {
	WriteInteger(out, 0, 7, text.size());
	out.insert(out.end(), text.begin(), text.end());
}

} // namespace

bool HpackDecoder::Entry(uint64_t index, Header& header) const {
	if (index == 0) {
		return false;
	}
	if (index <= STATIC_TABLE.size()) {
		header = {STATIC_TABLE[index - 1].first, STATIC_TABLE[index - 1].second};
		return true;
	}
	index -= STATIC_TABLE.size() + 1;
	if (index >= m_dynamic.size()) {
		return false;
	}
	header = m_dynamic[index];
	return true;
}

void HpackDecoder::Insert(Header header) {
	// RFC 7541, 4.1: an entry takes its name and value plus 32 octets.
	const size_t size = header.first.size() + header.second.size() + 32;
	m_dynamic.insert(m_dynamic.begin(), std::move(header));
	m_size += size;
	Evict();
}

void HpackDecoder::Evict() {
	while (m_size > m_max_size && !m_dynamic.empty()) {
		m_size -= m_dynamic.back().first.size() + m_dynamic.back().second.size() + 32;
		m_dynamic.pop_back();
	}
}

bool HpackDecoder::Decode(std::span<const uint8_t> block, Headers& headers) {
	size_t pos = 0;
	while (pos < block.size()) {
		const uint8_t first = block[pos];
		uint64_t      index = 0;
		if ((first & 0x80u) != 0) { // Indexed (6.1).
			Header header;
			if (!ReadInteger(block, pos, 7, index) || !Entry(index, header)) {
				return false;
			}
			headers.push_back(std::move(header));
			continue;
		}
		if ((first & 0xe0u) == 0x20u) { // Dynamic table size update (6.3).
			if (!ReadInteger(block, pos, 5, index) || index > 4096) {
				return false;
			}
			m_max_size = index;
			Evict();
			continue;
		}
		// A literal (6.2): with incremental indexing (01), without (0000) or never indexed (0001).
		const bool    indexing    = (first & 0xc0u) == 0x40u;
		const uint8_t prefix_bits = indexing ? 6 : 4;
		Header        header;
		if (!ReadInteger(block, pos, prefix_bits, index)) {
			return false;
		}
		if (index != 0) {
			Header named;
			if (!Entry(index, named)) {
				return false;
			}
			header.first = std::move(named.first);
		} else if (!ReadString(block, pos, header.first)) {
			return false;
		}
		if (!ReadString(block, pos, header.second)) {
			return false;
		}
		if (indexing) {
			Insert(header);
		}
		headers.push_back(std::move(header));
	}
	return true;
}

std::vector<uint8_t> HpackEncode(const Headers& headers) {
	std::vector<uint8_t> out;
	for (const auto& [name, value]: headers) {
		out.push_back(0x00); // A literal without indexing, with a new name.
		WriteString(out, name);
		WriteString(out, value);
	}
	return out;
}

namespace {

enum FrameType : uint8_t {
	DATA          = 0x0,
	HEADERS       = 0x1,
	RST_STREAM    = 0x3,
	SETTINGS      = 0x4,
	PING          = 0x6,
	GOAWAY        = 0x7,
	WINDOW_UPDATE = 0x8,
	CONTINUATION  = 0x9,
};

constexpr uint8_t END_STREAM  = 0x1;
constexpr uint8_t ACK         = 0x1;
constexpr uint8_t END_HEADERS = 0x4;
constexpr uint8_t PADDED      = 0x8;
constexpr uint8_t PRIORITY    = 0x20;

// KYTY_EA_TRACE=1 also writes every frame the game sends.
bool TraceFrames() {
	static const bool on = std::getenv("KYTY_EA_TRACE") != nullptr;
	return on;
}

constexpr std::string_view PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

bool ReadExactly(int fd, uint8_t* data, size_t size) {
	while (size > 0) {
		const auto got = ::recv(fd, reinterpret_cast<char*>(data), static_cast<int>(size), 0);
		if (got <= 0) {
			return false;
		}
		data += got;
		size -= static_cast<size_t>(got);
	}
	return true;
}

bool WriteAll(int fd, const std::vector<uint8_t>& data) {
	size_t sent = 0;
	while (sent < data.size()) {
		const auto put = ::send(fd, reinterpret_cast<const char*>(data.data() + sent),
		                        static_cast<int>(data.size() - sent), 0);
		if (put <= 0) {
			return false;
		}
		sent += static_cast<size_t>(put);
	}
	return true;
}

bool SendFrame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
               std::span<const uint8_t> payload) {
	std::vector<uint8_t> frame = {static_cast<uint8_t>(payload.size() >> 16u),
	                              static_cast<uint8_t>(payload.size() >> 8u),
	                              static_cast<uint8_t>(payload.size()),
	                              type,
	                              flags,
	                              static_cast<uint8_t>(stream >> 24u),
	                              static_cast<uint8_t>(stream >> 16u),
	                              static_cast<uint8_t>(stream >> 8u),
	                              static_cast<uint8_t>(stream)};
	frame.insert(frame.end(), payload.begin(), payload.end());
	return WriteAll(fd, frame);
}

std::vector<uint8_t> WindowIncrement(uint32_t bytes) {
	return {static_cast<uint8_t>(bytes >> 24u), static_cast<uint8_t>(bytes >> 16u),
	        static_cast<uint8_t>(bytes >> 8u), static_cast<uint8_t>(bytes)};
}

// The payload of a HEADERS or DATA frame without its padding (and priority, for HEADERS).
bool Unpadded(uint8_t flags, bool headers, std::span<const uint8_t>& payload) {
	size_t pad = 0;
	if ((flags & PADDED) != 0) {
		if (payload.empty()) return false;
		pad     = payload[0];
		payload = payload.subspan(1);
	}
	if (headers && (flags & PRIORITY) != 0) {
		if (payload.size() < 5) return false;
		payload = payload.subspan(5);
	}
	if (pad > payload.size()) return false;
	payload = payload.first(payload.size() - pad);
	return true;
}

struct Stream {
	std::vector<uint8_t> header_block;
	bool                 headers_done = false;
	bool                 ended        = false;
	Headers              headers;
	std::vector<uint8_t> body;
};

// Answers one finished call: the response headers, the reply message, then the trailers; or,
// for a method the handler does not answer, a trailers-only response with UNIMPLEMENTED (12).
bool Answer(int fd, uint32_t id, const Stream& stream, const GrpcHandler& handler) {
	std::string path;
	for (const auto& [name, value]: stream.headers) {
		if (name == ":path") path = value;
	}
	// The gRPC message: a compressed flag, a 32-bit big-endian length, then the message.
	std::span<const uint8_t> message;
	bool                     compressed = false;
	if (stream.body.size() >= 5) {
		compressed            = stream.body[0] != 0;
		const uint32_t length = (uint32_t {stream.body[1]} << 24u) |
		                        (uint32_t {stream.body[2]} << 16u) |
		                        (uint32_t {stream.body[3]} << 8u) | stream.body[4];
		message =
		    std::span(stream.body).subspan(5, std::min<size_t>(length, stream.body.size() - 5));
	}
	std::vector<uint8_t> reply;
	const bool           answered = !compressed && handler(path, message, reply);
	if (!answered) {
		const auto block = HpackEncode({{":status", "200"},
		                                {"content-type", "application/grpc"},
		                                {"grpc-status", compressed ? "13" : "12"}});
		return SendFrame(fd, HEADERS, END_HEADERS | END_STREAM, id, block);
	}
	std::vector<uint8_t> data = {
	    0, static_cast<uint8_t>(reply.size() >> 24u), static_cast<uint8_t>(reply.size() >> 16u),
	    static_cast<uint8_t>(reply.size() >> 8u), static_cast<uint8_t>(reply.size())};
	data.insert(data.end(), reply.begin(), reply.end());
	if (!SendFrame(fd, HEADERS, END_HEADERS, id,
	               HpackEncode({{":status", "200"}, {"content-type", "application/grpc"}}))) {
		return false;
	}
	// A frame holds at most SETTINGS_MAX_FRAME_SIZE bytes, 16384 unless the peer allows more.
	constexpr size_t MAX_FRAME = 16384;
	for (size_t sent = 0; sent < data.size(); sent += MAX_FRAME) {
		const auto part = std::span(data).subspan(sent, std::min(MAX_FRAME, data.size() - sent));
		if (!SendFrame(fd, DATA, 0, id, part)) {
			return false;
		}
	}
	return SendFrame(fd, HEADERS, END_HEADERS | END_STREAM, id,
	                 HpackEncode({{"grpc-status", "0"}}));
}

} // namespace

void ServeGrpcConnection(int fd, const GrpcHandler& handler) {
	const auto close_socket = [fd] {
#if defined(_WIN32)
		::closesocket(fd);
#else
		::close(fd);
#endif
	};
	std::array<uint8_t, PREFACE.size()> preface {};
	if (!ReadExactly(fd, preface.data(), preface.size()) ||
	    std::memcmp(preface.data(), PREFACE.data(), PREFACE.size()) != 0) {
		close_socket();
		return;
	}
	// Our settings: the defaults.
	if (!SendFrame(fd, SETTINGS, 0, 0, {})) {
		close_socket();
		return;
	}
	HpackDecoder               decoder;
	std::map<uint32_t, Stream> streams;
	std::vector<uint8_t>       payload;
	for (;;) {
		std::array<uint8_t, 9> head {};
		if (!ReadExactly(fd, head.data(), head.size())) break;
		const uint32_t length = (uint32_t {head[0]} << 16u) | (uint32_t {head[1]} << 8u) | head[2];
		const uint8_t  type   = head[3];
		const uint8_t  flags  = head[4];
		const uint32_t id     = ((uint32_t {head[5]} << 24u) | (uint32_t {head[6]} << 16u) |
		                         (uint32_t {head[7]} << 8u) | head[8]) &
		                        0x7fffffffu;
		payload.resize(length);
		if (length > 0 && !ReadExactly(fd, payload.data(), length)) break;
		std::span<const uint8_t> body(payload);
		if (TraceFrames()) {
			// RST_STREAM and GOAWAY end with an error code (RFC 9113, 7).
			uint32_t error = 0;
			if ((type == RST_STREAM && length >= 4) || (type == GOAWAY && length >= 8)) {
				const auto* e = payload.data() + (type == GOAWAY ? 4 : 0);
				error         = (uint32_t {e[0]} << 24u) | (uint32_t {e[1]} << 16u) |
				                (uint32_t {e[2]} << 8u) | e[3];
			}
			Log::WriteToConsoleAndLog(fmt::format(
			    "EA emulation: HTTP/2 frame type {} flags 0x{:02x} stream {} length {} error {}\n",
			    type, flags, id, length, error));
		}

		bool ok = true;
		switch (type) {
			case SETTINGS:
				if ((flags & ACK) == 0) ok = SendFrame(fd, SETTINGS, ACK, 0, {});
				break;
			case PING:
				if ((flags & ACK) == 0) ok = SendFrame(fd, PING, ACK, 0, body);
				break;
			case HEADERS:
			case CONTINUATION: {
				auto& stream = streams[id];
				if (type == HEADERS) {
					ok = Unpadded(flags, true, body);
					if ((flags & END_STREAM) != 0) stream.ended = true;
				}
				stream.header_block.insert(stream.header_block.end(), body.begin(), body.end());
				if (ok && (flags & END_HEADERS) != 0) {
					ok                  = decoder.Decode(stream.header_block, stream.headers);
					stream.headers_done = true;
				}
				break;
			}
			case DATA: {
				auto& stream = streams[id];
				ok           = Unpadded(flags, false, body);
				stream.body.insert(stream.body.end(), body.begin(), body.end());
				if ((flags & END_STREAM) != 0) stream.ended = true;
				// Gives the bytes back to both flow-control windows (RFC 9113, 6.9).
				if (ok && length > 0) {
					const auto increment = WindowIncrement(length);
					ok                   = SendFrame(fd, WINDOW_UPDATE, 0, 0, increment) &&
					                       SendFrame(fd, WINDOW_UPDATE, 0, id, increment);
				}
				break;
			}
			case RST_STREAM: streams.erase(id); break;
			case GOAWAY: ok = false; break;
			default: break; // WINDOW_UPDATE, PRIORITY and unknown frames need no answer here.
		}
		if (!ok) break;
		if (const auto found = streams.find(id);
		    found != streams.end() && found->second.ended && found->second.headers_done) {
			ok = Answer(fd, id, found->second, handler);
			streams.erase(found);
			if (!ok) break;
		}
	}
	close_socket();
}

} // namespace Libs::EaEmulation
