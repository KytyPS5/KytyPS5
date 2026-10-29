// Tests for the decoder of Blaze payloads of the stand-in for EA's game server.

#include "libs/eaEmulation/eaTdf.h"

#include <iostream>
#include <string>

namespace {

using namespace Libs::EaEmulation;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

void AppendTag(std::vector<uint8_t>& out, uint32_t tag, uint8_t type) {
	out.push_back(static_cast<uint8_t>(tag >> 16));
	out.push_back(static_cast<uint8_t>(tag >> 8));
	out.push_back(static_cast<uint8_t>(tag));
	out.push_back(type);
}

void AppendText(std::vector<uint8_t>& out, const std::string& text) {
	AppendVar(out, static_cast<int64_t>(text.size() + 1));
	out.insert(out.end(), text.begin(), text.end());
	out.push_back(0);
}

// CONF { KEYS: map text -> text { "clientSecret": "abc123", "name": "visible" } }, then an Int.
std::vector<uint8_t> Sample() {
	std::vector<uint8_t> out;
	AppendTag(out, Tag("CONF"), Struct);
	AppendTag(out, Tag("KEYS"), Map);
	out.push_back(Str);
	out.push_back(Str);
	AppendVar(out, 2);
	AppendText(out, "clientSecret");
	AppendText(out, "abc123");
	AppendText(out, "name");
	AppendText(out, "visible");
	out.push_back(0); // end of the struct
	AppendTag(out, Tag("STIM"), Int);
	AppendVar(out, 1234567);
	return out;
}

void TestDump() {
	const auto  payload = Sample();
	const auto  text    = DumpTdf(payload.data(), payload.size());
	Check(text.find("CONF") != std::string::npos, "the dump names the struct");
	Check(text.find("\"clientSecret\" = \"abc123\"") != std::string::npos, "the dump shows a map entry");
	Check(text.find("STIM") != std::string::npos && text.find("1234567") != std::string::npos, "the dump shows an int");
	Check(text.find("undecoded") == std::string::npos, "the sample decodes");
}

void TestBlankSecrets() {
	auto       payload = Sample();
	const auto size    = payload.size();
	Check(BlankSecrets(payload) == 1, "one secret is blanked");
	Check(payload.size() == size, "the size does not change");
	const auto text = DumpTdf(payload.data(), payload.size());
	Check(text.find("\"clientSecret\" = \"xxxxxx\"") != std::string::npos, "the secret is filler");
	Check(text.find("\"name\" = \"visible\"") != std::string::npos, "another value stays");
}

void TestBadPayload() {
	const std::vector<uint8_t> payload = {0x87, 0x3c, 0xa3, 0x03}; // a struct that never ends
	const auto                 text    = DumpTdf(payload.data(), payload.size());
	Check(text.find("undecoded") != std::string::npos, "a bad payload is reported");
	auto copy = payload;
	Check(BlankSecrets(copy) == 0, "a bad payload blanks nothing");
}

} // namespace

int main() {
	TestDump();
	TestBlankSecrets();
	TestBadPayload();
	if (g_failures != 0) {
		std::cerr << g_failures << " EA emulation TDF check(s) failed\n";
		return 1;
	}
	std::cout << "EA emulation TDF tests passed\n";
	return 0;
}
