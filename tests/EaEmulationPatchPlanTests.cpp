// Tests for the patch plan that makes a game trust the stand-in for EA's game server.

#include "libs/eaEmulation/eaPatchPlan.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace Libs::EaEmulation;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

// The plan names the game it is made for: whatever game is loaded, not one fixed title.
void TestThePlanTakesItsGameFromItsArguments() {
	const std::vector<uint8_t> new_modulus(AuthorityModulus().size(), 0xab);
	for (const auto* title: {"TEST00001", "TEST00002"}) {
		const auto text = MakePatchPlan(title, "01.002.003", "eboot.bin", new_modulus);
		const auto plan = nlohmann::json::parse(text, nullptr, false);
		Check(plan.is_object(), "the plan is JSON");
		Check(plan.value("id", "") == title, "the title id comes from the argument");
		Check(plan.value("version", "") == "01.002.003", "the version comes from the argument");
		Check(plan.value("process", "") == "eboot.bin", "the process comes from the argument");
	}
}

// One write: EA's authority key, wherever the game keeps it, becomes ours.
void TestTheWriteReplacesTheAuthorityKeyWhereverItIs() {
	const std::vector<uint8_t> new_modulus(AuthorityModulus().size(), 0xab);
	const auto plan = nlohmann::json::parse(MakePatchPlan("T", "1", "eboot.bin", new_modulus), nullptr, false);
	Check(plan.is_object() && plan["mods"].size() == 1 && plan["mods"][0]["memory"].size() == 1,
	      "one mod with one write");
	const auto& write = plan["mods"][0]["memory"][0];
	Check(write["off"].get<std::string>().substr(0, 4) == "cc6d", "the write looks for EA's authority key");
	Check(write["off"].get<std::string>().size() == AuthorityModulus().size() * 2, "the whole key is the pattern");
	std::string expected;
	for (size_t i = 0; i < AuthorityModulus().size(); i++) expected += "ab";
	Check(write["on"].get<std::string>() == expected, "our key is written");
	Check(write["offset"].get<std::string>() == "0x0", "no address: the key is found by search");
}

// A key of another size would shift the bytes after it.
void TestAKeyOfAnotherSizeMakesNoPlan() {
	const std::vector<uint8_t> short_modulus(128, 0x01);
	Check(MakePatchPlan("T", "1", "eboot.bin", short_modulus).empty(), "a key of another size is refused");
}

void TestTheAuthorityKeyIsAnRsa2048Modulus() {
	const auto& modulus = AuthorityModulus();
	Check(modulus.size() == 256, "256 bytes");
	Check(modulus[0] == 0xcc && modulus[1] == 0x6d, "starts as EA's GOS 2015 key does");
}

// Every game that holds EA's key gets the server while EA emulation is on; no title is special.
void TestEveryTitleStartsWhileEaEmulationIsOn() {
	Check(ShouldStartServer("TEST00001", true), "a title starts while EA emulation is on");
	Check(ShouldStartServer("TEST00002", true), "any other title starts the same way");
	Check(!ShouldStartServer("TEST00001", false), "no title starts while EA emulation is off");
	Check(!ShouldStartServer("", true), "no title, no server");
}

} // namespace

int main() {
	TestEveryTitleStartsWhileEaEmulationIsOn();
	TestThePlanTakesItsGameFromItsArguments();
	TestTheWriteReplacesTheAuthorityKeyWhereverItIs();
	TestAKeyOfAnotherSizeMakesNoPlan();
	TestTheAuthorityKeyIsAnRsa2048Modulus();
	if (g_failures != 0) {
		std::cerr << g_failures << " failure(s)\n";
		return 1;
	}
	std::cout << "EA emulation patch plan tests passed\n";
	return 0;
}
