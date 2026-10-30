// Tests for the reply of the stand-in for EA's game server to Util.preAuth.

#include "libs/eaEmulation/eaPreAuth.h"
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

// A Util.preAuth request as a Blaze client sends it, with made-up names.
std::vector<uint8_t> Request() {
	Writer w;
	w.BeginStruct(Tag("CDAT"));
	w.Integer(Tag("CTRY"), 21843);
	w.Text(Tag("SVCN"), "game-2099-ps5");
	w.Integer(Tag("TYPE"), 6);
	w.EndStruct();
	w.BeginStruct(Tag("CINF"));
	w.Text(Tag("BSDK"), "21.0.1");
	w.Text(Tag("CLNT"), "GAME99");
	w.Text(Tag("ENV "), "test");
	w.EndStruct();
	return std::move(w.out);
}

bool Has(const std::string& text, const std::string& part) {
	return text.find(part) != std::string::npos;
}

void TestTheReplyTakesTheGamesNamesFromTheRequest() {
	const auto reply = PreAuthReply(Request());
	const auto text  = DumpTdf(reply.data(), reply.size());
	Check(!Has(text, "undecoded"), "the reply decodes");
	Check(Has(text, "INST") && Has(text, "\"game-2099-ps5\""), "the service is the request's");
	Check(Has(text, "CLID") && Has(text, "\"GAME99\""), "the client is the request's");
	Check(Has(text, "\"blazeSdkGuestClientId\" = \"GAME99_GST\""), "the guest client follows the client");
	Check(Has(text, "\"Blaze 21.0.1\""), "the server version is the request's SDK");
	Check(Has(text, "RELT") && Has(text, "\"Env_override\" = \"test\""), "the environment is the request's");
}

void TestTheReplyHasNoSecretsAndNoGamesOwnComponents() {
	auto       reply = PreAuthReply(Request());
	const auto size  = reply.size();
	Check(BlankSecrets(reply) == 0 && reply.size() == size, "no setting is a secret");

	Reader reader {reply.data(), reply.size()};
	bool   found = false;
	while (reader.ok && reader.at < reader.size) {
		const uint32_t tag  = reader.TagValue();
		const uint8_t  type = reader.Byte();
		if (tag == Tag("CIDS") && type == List) {
			found               = true;
			const uint8_t item  = reader.Byte();
			const auto    count = reader.Var();
			Check(item == Int && count > 0, "the components are a list of numbers");
			for (int64_t i = 0; i < count && reader.ok; i++) {
				Check(reader.Var() < 0x7800, "no component of a game's own range");
			}
			break;
		}
		reader.SkipValue(type);
	}
	Check(found, "the reply lists its components");
}

void TestARequestWithoutNamesStillGetsAReply() {
	const auto reply = PreAuthReply({});
	const auto text  = DumpTdf(reply.data(), reply.size());
	Check(!reply.empty() && !Has(text, "undecoded"), "an empty request gets a reply that decodes");
	Check(Has(text, "\"Env_override\" = \"prod\""), "the environment defaults to prod");
}

void TestTheMembersAreInTheOrderOfTheirTags() {
	const auto reply = PreAuthReply(Request());
	Reader     reader {reply.data(), reply.size()};
	uint32_t   last = 0;
	while (reader.ok && reader.at < reader.size) {
		const uint32_t tag = reader.TagValue();
		Check(tag > last, "each member's tag is after the one before");
		last = tag;
		reader.SkipValue(reader.Byte());
	}
	Check(reader.ok, "the reply reads to its end");
}

} // namespace

int main() {
	TestTheReplyTakesTheGamesNamesFromTheRequest();
	TestTheReplyHasNoSecretsAndNoGamesOwnComponents();
	TestARequestWithoutNamesStillGetsAReply();
	TestTheMembersAreInTheOrderOfTheirTags();
	if (g_failures != 0) {
		std::cerr << g_failures << " EA emulation preAuth check(s) failed\n";
		return 1;
	}
	std::cout << "EA emulation preAuth tests passed\n";
	return 0;
}
