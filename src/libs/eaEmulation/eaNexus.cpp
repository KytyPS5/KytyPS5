#include "libs/eaEmulation/eaNexus.h"

#include "common/logging/log.h"
#include "libs/eaEmulation/eaAuthentication.h"

#include <algorithm>
#include <cstdlib>
#include <fmt/format.h>
#include <string_view>

namespace Libs::EaEmulation {

namespace {

constexpr std::string_view SERVICE = "/eadp.nexus.connect.grpc.v1.";

// The codes and tokens of the stand-in. The game only passes them on: to the next call here,
// and to the Blaze server of the emulator, which does not check them.
constexpr const char* AUTH_CODE    = "kyty-emulated-auth-code";
constexpr const char* ACCESS_TOKEN = "kyty-emulated-access-token";

constexpr uint64_t TOKEN_LIFETIME_SECONDS = 3600;

// The field numbers of eadp.nexus.connect.grpc.v1.TokenInfo, as the game's parser and its debug
// printer have them. Each value is in a wrapper message (StringValue, Int64Value,
// BoolValue).
namespace TokenInfoField {
constexpr uint32_t CLIENT_ID    = 1;    // clientId, StringValue
constexpr uint32_t SCOPE        = 2;    // scope, StringValue
constexpr uint32_t EXPIRE_IN    = 3;    // expireIn, Int64Value
constexpr uint32_t PID_ID       = 401;  // pidId, StringValue
constexpr uint32_t PID_TYPE     = 402;  // pidType, StringValue
constexpr uint32_t USER_ID      = 501;  // userId, StringValue
constexpr uint32_t PERSONA_ID   = 2053; // personaId, Int64Value
constexpr uint32_t STOP_PROCESS = 3131; // stopProcess, StringValue
constexpr uint32_t CONSOLE_ENV  = 4004; // consoleEnv, StringValue
constexpr uint32_t IS_UNDERAGE  = 8008; // isUnderage, BoolValue
} // namespace TokenInfoField

bool Trace() {
	static const bool on = std::getenv("KYTY_EA_TRACE") != nullptr;
	return on;
}

void Log(const std::string& text) {
	Log::WriteToConsoleAndLog(fmt::format("EA emulation: {}\n", text));
}

// A protobuf message, written field by field.
class Message {
public:
	Message& Text(uint32_t field, std::string_view text) {
		Key(field, 2);
		Varint(text.size());
		m_bytes.insert(m_bytes.end(), text.begin(), text.end());
		return *this;
	}
	Message& Integer(uint32_t field, uint64_t value) {
		Key(field, 0);
		Varint(value);
		return *this;
	}
	Message& Nested(uint32_t field, std::span<const uint8_t> bytes) {
		Key(field, 2);
		Varint(bytes.size());
		m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
		return *this;
	}
	// EA's messages hold every value in a wrapper message (eadp.foundation.protobuf.StringValue,
	// Int64Value, BoolValue), with the value in field 1 of the wrapper.
	Message& StringValue(uint32_t field, std::string_view text) {
		return Nested(field, Message().Text(1, text).Take());
	}
	Message& Int64Value(uint32_t field, uint64_t value) {
		return Nested(field, Message().Integer(1, value).Take());
	}
	Message& BoolValue(uint32_t field, bool value) {
		return Nested(field, Message().Integer(1, value ? 1 : 0).Take());
	}
	[[nodiscard]] std::vector<uint8_t> Take() { return std::move(m_bytes); }

private:
	void Key(uint32_t field, uint32_t wire_type) { Varint((uint64_t {field} << 3u) | wire_type); }
	void Varint(uint64_t value) {
		while (value >= 0x80) {
			m_bytes.push_back(static_cast<uint8_t>((value & 0x7fu) | 0x80u));
			value >>= 7;
		}
		m_bytes.push_back(static_cast<uint8_t>(value));
	}

	std::vector<uint8_t> m_bytes;
};

bool ReadVarint(std::span<const uint8_t> data, size_t& pos, uint64_t& value) {
	value = 0;
	for (int shift = 0; shift < 64 && pos < data.size(); shift += 7) {
		const uint8_t byte = data[pos++];
		value |= static_cast<uint64_t>(byte & 0x7fu) << shift;
		if ((byte & 0x80u) == 0) return true;
	}
	return false;
}

// The text of a StringValue in `field` of `message` (the text in field 1 of the wrapper), or "".
std::string WrappedText(std::span<const uint8_t> message, uint32_t field) {
	size_t pos = 0;
	while (pos < message.size()) {
		uint64_t key   = 0;
		uint64_t value = 0;
		if (!ReadVarint(message, pos, key)) return {};
		switch (key & 7u) {
			case 0:
				if (!ReadVarint(message, pos, value)) return {};
				break;
			case 1: pos += 8; break;
			case 5: pos += 4; break;
			case 2: {
				if (!ReadVarint(message, pos, value) || value > message.size() - pos) return {};
				const auto bytes = message.subspan(pos, value);
				pos += value;
				if ((key >> 3u) != field) break;
				// The wrapper: field 1, length-delimited.
				size_t   inner        = 0;
				uint64_t inner_key    = 0;
				uint64_t inner_length = 0;
				if (ReadVarint(bytes, inner, inner_key) && inner_key == ((1u << 3u) | 2u) &&
				    ReadVarint(bytes, inner, inner_length) &&
				    inner_length <= bytes.size() - inner) {
					return {bytes.begin() + static_cast<std::ptrdiff_t>(inner),
					        bytes.begin() + static_cast<std::ptrdiff_t>(inner + inner_length)};
				}
				return {};
			}
			default: return {};
		}
	}
	return {};
}

} // namespace

std::string DescribeProtobuf(std::span<const uint8_t> message, int depth) {
	std::string text;
	size_t      pos    = 0;
	const auto  indent = std::string(static_cast<size_t>(depth) * 2 + 2, ' ');
	while (pos < message.size()) {
		uint64_t key = 0;
		if (!ReadVarint(message, pos, key)) return text + indent + "(malformed)\n";
		const auto field = key >> 3u;
		switch (key & 7u) {
			case 0: {
				uint64_t value = 0;
				if (!ReadVarint(message, pos, value)) return text + indent + "(malformed)\n";
				text += fmt::format("{}{}: {}\n", indent, field, value);
				break;
			}
			case 2: {
				uint64_t length = 0;
				if (!ReadVarint(message, pos, length) || length > message.size() - pos) {
					return text + indent + "(malformed)\n";
				}
				const auto bytes = message.subspan(pos, length);
				pos += length;
				const bool printable = std::all_of(bytes.begin(), bytes.end(),
				                                   [](uint8_t c) { return c >= 0x20 && c < 0x7f; });
				if (printable) {
					text += fmt::format("{}{}: \"{}\"\n", indent, field,
					                    std::string(bytes.begin(), bytes.end()));
				} else {
					text += fmt::format("{}{}: {{\n{}{}}}\n", indent, field,
					                    DescribeProtobuf(bytes, depth + 1), indent);
				}
				break;
			}
			case 1:
				pos += 8;
				text += fmt::format("{}{}: (64-bit)\n", indent, field);
				break;
			case 5:
				pos += 4;
				text += fmt::format("{}{}: (32-bit)\n", indent, field);
				break;
			default: return text + indent + "(malformed)\n";
		}
	}
	return text;
}

bool NexusReply(const std::string& path, std::span<const uint8_t> request,
                std::vector<uint8_t>& reply) {
	if (Trace()) {
		Log(fmt::format("account service call {}\n{}", path, DescribeProtobuf(request)));
	}
	if (path.size() <= SERVICE.size() || path.compare(0, SERVICE.size(), SERVICE) != 0) {
		return false;
	}
	const auto method = std::string_view(path).substr(SERVICE.size());
	// The fields as the game's own parsers read them: GetAuthResponse has the code in field
	// 1, a StringValue. GrantTokenResponse has StringValue fields 1, 2, 4 and 6 and Int64Value
	// fields 3 and 5; the names are not in the game, so the OAuth order is assumed: the access
	// token, its type, its lifetime in seconds.
	if (method.starts_with("AuthService/GetAuthFor")) {
		reply = Message().StringValue(1, AUTH_CODE).Take();
		return true;
	}
	if (method == "TokenService/GrantTokenByAuthorizationCode") {
		reply = Message()
		            .StringValue(1, ACCESS_TOKEN)
		            .StringValue(2, "Bearer")
		            .Int64Value(3, 3600)
		            .Take();
		return true;
	}
	if (method == "TokenInfoService/GetTokenInfo") {
		// GetTokenInfoResponse: the TokenInfo in field 1. The client id is the game's own (field 1
		// of the request); the account is the stand-in's.
		const auto info =
		    Message()
		        .StringValue(TokenInfoField::CLIENT_ID, WrappedText(request, 1))
		        .StringValue(TokenInfoField::SCOPE, "offline basic.identity basic.persona signin")
		        .Int64Value(TokenInfoField::EXPIRE_IN, TOKEN_LIFETIME_SECONDS)
		        .StringValue(TokenInfoField::PID_ID, EMULATED_ACCOUNT_ID_TEXT)
		        .StringValue(TokenInfoField::PID_TYPE, "NUCLEUS")
		        .StringValue(TokenInfoField::USER_ID, EMULATED_ACCOUNT_ID_TEXT)
		        .Int64Value(TokenInfoField::PERSONA_ID, EMULATED_PERSONA_ID)
		        .StringValue(TokenInfoField::STOP_PROCESS, "OFF")
		        .StringValue(TokenInfoField::CONSOLE_ENV, "prod")
		        .BoolValue(TokenInfoField::IS_UNDERAGE, false)
		        .Take();
		reply = Message().Nested(1, info).Take();
		return true;
	}
	Log(fmt::format("account service call {} is not answered", path));
	return false;
}

} // namespace Libs::EaEmulation
