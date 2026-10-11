#ifndef EMULATOR_INCLUDE_EMULATOR_COMMON_AMDCPUMODE_H_
#define EMULATOR_INCLUDE_EMULATOR_COMMON_AMDCPUMODE_H_

#include <optional>
#include <string>
#include <string_view>

namespace Common {

// The guest is built for an AMD CPU. Without the patcher every SSE4a/RDPID/CLWB instruction the host lacks
// raises SIGILL and is emulated in a signal handler (about ten thousand signals per frame on Rosetta, which
// reports an Intel CPU without SSE4a). Auto turns the patcher on only when the host lacks one of them.
enum class AmdCpuMode { Auto, On, Off };

// Instructions the AMD patcher rewrites, as reported by CPUID of the host.
struct AmdCpuHostSupport {
	bool sse4a = false;
	bool rdpid = false;
	bool clwb  = false;
};

struct AmdCpuDecision {
	bool        enabled = false;
	std::string reason;
};

// "auto", "on"/"1"/"true", "off"/"0"/"false"; anything else (including empty) is not a mode.
inline std::optional<AmdCpuMode> ParseAmdCpuMode(std::string_view text) {
	if (text == "auto") return AmdCpuMode::Auto;
	if (text == "on" || text == "1" || text == "true") return AmdCpuMode::On;
	if (text == "off" || text == "0" || text == "false") return AmdCpuMode::Off;
	return std::nullopt;
}

inline const char* AmdCpuModeName(AmdCpuMode mode) {
	return mode == AmdCpuMode::On ? "on" : mode == AmdCpuMode::Off ? "off" : "auto";
}

// An explicit command line flag wins over KYTY_AMD_CPU; an unset or unparsable variable leaves auto.
inline AmdCpuMode ResolveAmdCpuMode(std::optional<AmdCpuMode> command_line, const char* env) {
	if (command_line) return *command_line;
	if (env != nullptr) {
		if (const auto mode = ParseAmdCpuMode(env)) return *mode;
	}
	return AmdCpuMode::Auto;
}

// The launcher setting. A stored amd_cpu_mode wins; the older amd_cpu_enabled bool maps true -> on, but false -> auto:
// that default was written for every configuration whether or not the user touched the checkbox, so it is no
// choice to honour. Neither key (or an unparsable mode) is auto.
inline AmdCpuMode AmdCpuModeFromStored(std::optional<std::string_view> mode, std::optional<bool> legacy_enabled) {
	if (mode) {
		if (const auto parsed = ParseAmdCpuMode(*mode)) return *parsed;
	}
	if (legacy_enabled && *legacy_enabled) return AmdCpuMode::On;
	return AmdCpuMode::Auto;
}

inline AmdCpuDecision DecideAmdCpu(AmdCpuMode mode, AmdCpuHostSupport host) {
	if (mode == AmdCpuMode::On) return {true, "forced on (--amd-cpu or KYTY_AMD_CPU=1)"};
	if (mode == AmdCpuMode::Off) return {false, "forced off (--no-amd-cpu or KYTY_AMD_CPU=0)"};
	std::string missing;
	const auto  add = [&missing](bool supported, const char* name) {
		if (supported) return;
		if (!missing.empty()) missing += ", ";
		missing += name;
	};
	add(host.sse4a, "SSE4a");
	add(host.rdpid, "RDPID");
	add(host.clwb, "CLWB");
	if (missing.empty()) return {false, "auto: host supports SSE4a, RDPID and CLWB"};
	return {true, "auto: host lacks " + missing};
}

} // namespace Common

#endif /* EMULATOR_INCLUDE_EMULATOR_COMMON_AMDCPUMODE_H_ */
