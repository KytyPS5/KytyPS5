#include "libs/eaEmulation/eaPatchPlan.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string_view>

namespace Libs::EaEmulation {

namespace {

// The modulus of "GOS 2015 Certificate Authority" as the games carry it.
constexpr std::string_view AUTHORITY_MODULUS_HEX =
"cc6d54b6f4e484e7207602d79748757e7efb439c3da19647c15d078b3073bf9dfe75945521d08874664ca2b7fe9fc03b"
    "f060a0db08332b6ef80205b9879dac65d5069d05e8d1b6f5de7da5a47d8acb9931b6859ba2ce39e28c65aa07fc153307"
    "00d17215130d870f5ca25ed0d5bfd9033262aff5ef5336a834dab6a3ec5c6ac067f8be379fb3c82df0364a6f6b06eeb7"
    "85f27f736c018483e4da46d0239a6df1777c0581904f6a4483783b71ad12c048c87389f198787bb4084abae85957e2fc2"
    "9acbff5a29d4f2c64dcd792191cc5fadb92c0904ba8e9f20d941ab25fdd33aeff669097b2a8a51bfa6f41b284ba52349"
    "74ad3c7b23fdddbc9b1138277e86acd";

std::vector<uint8_t> ParseHex(std::string_view text) {
	std::vector<uint8_t> bytes;
	for (size_t i = 0; i + 1 < text.size(); i += 2) {
		unsigned value = 0;
		std::sscanf(std::string(text.substr(i, 2)).c_str(), "%2x", &value);
		bytes.push_back(static_cast<uint8_t>(value));
	}
	return bytes;
}

} // namespace

const std::vector<uint8_t>& AuthorityModulus() {
	static const std::vector<uint8_t> modulus = ParseHex(AUTHORITY_MODULUS_HEX);
	return modulus;
}

bool ShouldStartServer(const std::string& title_id, bool ea_emulation_enabled) {
	return ea_emulation_enabled && !title_id.empty();
}

std::string Hex(const std::vector<uint8_t>& bytes) {
	std::string out;
	char        pair[3];
	for (const auto byte: bytes) {
		std::snprintf(pair, sizeof(pair), "%02x", byte);
		out += pair;
	}
	return out;
}

std::string MakePatchPlan(const std::string& title_id, const std::string& version, const std::string& process,
                          const std::vector<uint8_t>& new_modulus) {
	if (new_modulus.size() != AuthorityModulus().size()) return {};
	nlohmann::json plan;
	plan["id"]      = title_id;
	plan["version"] = version;
	plan["process"] = process;
	// Offset 0: the loader finds the old key by search, at whatever address the game has it.
	plan["mods"] = nlohmann::json::array({nlohmann::json {
	    {"name", "Trust the emulated EA server"},
	    {"type", "data"},
	    {"memory", nlohmann::json::array({nlohmann::json {{"offset", "0x0"},
	                                                       {"off", Hex(AuthorityModulus())},
	                                                       {"on", Hex(new_modulus)}}})}}});
	return plan.dump();
}

} // namespace Libs::EaEmulation
