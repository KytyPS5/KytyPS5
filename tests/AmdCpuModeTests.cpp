#include "common/amdCpuMode.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "AmdCpuModeTests: failed: %s\n", text);
		std::exit(1); // not abort: ctest WILL_FAIL does not invert a crash
	}
}

} // namespace

int main() {
	using Common::AmdCpuMode;
	using Common::AmdCpuHostSupport;

	// Parsing of KYTY_AMD_CPU and of the launcher setting.
	Check(Common::ParseAmdCpuMode("auto") == AmdCpuMode::Auto, "auto not parsed");
	Check(Common::ParseAmdCpuMode("1") == AmdCpuMode::On, "1 not parsed as on");
	Check(Common::ParseAmdCpuMode("on") == AmdCpuMode::On, "on not parsed");
	Check(Common::ParseAmdCpuMode("0") == AmdCpuMode::Off, "0 not parsed as off");
	Check(Common::ParseAmdCpuMode("off") == AmdCpuMode::Off, "off not parsed");
	Check(!Common::ParseAmdCpuMode("").has_value(), "empty accepted");
	Check(!Common::ParseAmdCpuMode("maybe").has_value(), "garbage accepted");
	Check(!Common::ParseAmdCpuMode("AUTO").has_value(), "case-insensitive parse not intended");

	// Precedence: command line flag > KYTY_AMD_CPU > auto; a bad or unset variable leaves auto.
	Check(Common::ResolveAmdCpuMode(std::nullopt, nullptr) == AmdCpuMode::Auto, "default is not auto");
	Check(Common::ResolveAmdCpuMode(std::nullopt, "1") == AmdCpuMode::On, "env on ignored");
	Check(Common::ResolveAmdCpuMode(std::nullopt, "0") == AmdCpuMode::Off, "env off ignored");
	Check(Common::ResolveAmdCpuMode(std::nullopt, "auto") == AmdCpuMode::Auto, "env auto ignored");
	Check(Common::ResolveAmdCpuMode(std::nullopt, "garbage") == AmdCpuMode::Auto, "bad env not auto");
	Check(Common::ResolveAmdCpuMode(AmdCpuMode::On, "0") == AmdCpuMode::On, "--amd-cpu lost to env");
	Check(Common::ResolveAmdCpuMode(AmdCpuMode::Off, "1") == AmdCpuMode::Off, "--no-amd-cpu lost to env");

	// Auto with an injected CPUID result. Rosetta reports an Intel CPU: no SSE4a (RDPID/CLWB vary).
	const AmdCpuHostSupport amd {true, true, true};
	auto                    d = Common::DecideAmdCpu(AmdCpuMode::Auto, amd);
	Check(!d.enabled, "auto patches a host that supports everything");
	Check(d.reason.find("auto") != std::string::npos, "auto reason missing");
	d = Common::DecideAmdCpu(AmdCpuMode::Auto, {false, true, true});
	Check(d.enabled && d.reason.find("SSE4a") != std::string::npos, "no SSE4a: not enabled / not named");
	Check(d.reason.find("RDPID") == std::string::npos && d.reason.find("CLWB") == std::string::npos,
	      "supported instruction named as missing");
	d = Common::DecideAmdCpu(AmdCpuMode::Auto, {true, false, true});
	Check(d.enabled && d.reason.find("RDPID") != std::string::npos, "no RDPID: not enabled / not named");
	d = Common::DecideAmdCpu(AmdCpuMode::Auto, {true, true, false});
	Check(d.enabled && d.reason.find("CLWB") != std::string::npos, "no CLWB: not enabled / not named");
	d = Common::DecideAmdCpu(AmdCpuMode::Auto, {false, false, false});
	Check(d.enabled && d.reason.find("SSE4a, RDPID, CLWB") != std::string::npos, "nothing supported");

	// On/off do not look at the host.
	Check(Common::DecideAmdCpu(AmdCpuMode::On, amd).enabled, "forced on ignored");
	Check(!Common::DecideAmdCpu(AmdCpuMode::Off, {false, false, false}).enabled, "forced off ignored");
	Check(!Common::DecideAmdCpu(AmdCpuMode::Off, {false, false, false}).reason.empty(), "no reason");

	Check(std::string_view(Common::AmdCpuModeName(AmdCpuMode::Auto)) == "auto" &&
	          std::string_view(Common::AmdCpuModeName(AmdCpuMode::On)) == "on" &&
	          std::string_view(Common::AmdCpuModeName(AmdCpuMode::Off)) == "off",
	      "mode names");

	// Launcher setting: a stored mode wins; the old bool maps true -> on, false -> auto (it was a default, not a choice).
	Check(Common::AmdCpuModeFromStored(std::nullopt, std::nullopt) == AmdCpuMode::Auto, "no key is not auto");
	Check(Common::AmdCpuModeFromStored(std::nullopt, false) == AmdCpuMode::Auto, "old false is not auto");
	Check(Common::AmdCpuModeFromStored(std::nullopt, true) == AmdCpuMode::On, "old true is not on");
	Check(Common::AmdCpuModeFromStored("off", true) == AmdCpuMode::Off, "stored mode lost to the old bool");
	Check(Common::AmdCpuModeFromStored("on", false) == AmdCpuMode::On, "stored on lost to the old bool");
	Check(Common::AmdCpuModeFromStored("auto", true) == AmdCpuMode::Auto, "stored auto lost to the old bool");
	Check(Common::AmdCpuModeFromStored("garbage", std::nullopt) == AmdCpuMode::Auto, "bad stored mode is not auto");

	std::puts("AmdCpuModeTests: ok");
	return 0;
}
