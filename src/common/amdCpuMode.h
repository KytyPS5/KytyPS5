#ifndef EMULATOR_INCLUDE_EMULATOR_COMMON_AMDCPUMODE_H_
#define EMULATOR_INCLUDE_EMULATOR_COMMON_AMDCPUMODE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Common {

// The guest is built for an AMD CPU. Without the patcher every SSE4a/RDPID/CLWB instruction the
// host lacks raises SIGILL and is emulated in a signal handler (about ten thousand signals per
// frame on Rosetta, which reports an Intel CPU without SSE4a). Auto turns the patcher on only when
// the host lacks one of them.
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

// An explicit command line flag wins over KYTY_AMD_CPU; an unset or unparsable variable leaves
// auto.
inline AmdCpuMode ResolveAmdCpuMode(std::optional<AmdCpuMode> command_line, const char* env) {
	if (command_line) return *command_line;
	if (env != nullptr) {
		if (const auto mode = ParseAmdCpuMode(env)) return *mode;
	}
	return AmdCpuMode::Auto;
}

// The old amd_cpu_enabled value as the launcher exports it: only "true" and "false" are a bool.
inline std::optional<bool> ParseLegacyAmdCpuEnabled(std::string_view text) {
	if (text == "true") return true;
	if (text == "false") return false;
	return std::nullopt;
}

// The launcher setting. A stored amd_cpu_mode wins; the older amd_cpu_enabled bool maps true -> on,
// but false -> auto: that default was written for every configuration whether or not the user
// touched the checkbox, so it is no choice to honour. Neither key (or an unparsable mode) is auto.
inline AmdCpuMode AmdCpuModeFromStored(std::optional<std::string_view> mode,
                                       std::optional<bool>             legacy_enabled) {
	if (mode) {
		if (const auto parsed = ParseAmdCpuMode(*mode)) return *parsed;
	}
	if (legacy_enabled && *legacy_enabled) return AmdCpuMode::On;
	return AmdCpuMode::Auto;
}

// Which instruction groups the patcher rewrites once it is on (KYTY_AMD_PATCH): sse4a =
// EXTRQ/INSERTQ, rsqrt = VRSQRTPS, cpuext = RDPID/CLWB. The automatic mode rewrites only sse4a. On
// Rosetta the VRSQRTPS patch leaves about a thousand call sites that still trap (+550 SIGILL per
// frame measured) and is slower than the native rsqrtps Rosetta translates, and its result is an
// exact reciprocal square root where AMD hardware gives the approximation, so it is opt-in.
// RDPID/CLWB are opt-in too. --amd-cpu (mode on) keeps rewriting everything.
enum AmdPatchKind : uint32_t {
	AmdPatchSse4a  = 1u << 0,
	AmdPatchRsqrt  = 1u << 1,
	AmdPatchCpuExt = 1u << 2,
	AmdPatchAll    = AmdPatchSse4a | AmdPatchRsqrt | AmdPatchCpuExt,
};

// Auto turns the patcher on only when the host lacks an instruction that one of the selected kinds
// rewrites (SSE4a for sse4a; RDPID and CLWB for cpuext); otherwise it would load every module with
// the trampoline area and decode all functions without being able to change anything. auto_allowed
// is false on platforms where an unrelocatable site cannot be left to the trap handler.
inline AmdCpuDecision DecideAmdCpu(AmdCpuMode mode, AmdCpuHostSupport host,
                                   uint32_t kinds = AmdPatchAll, bool auto_allowed = true) {
	if (mode == AmdCpuMode::On) return {true, "forced on (--amd-cpu or KYTY_AMD_CPU=1)"};
	if (mode == AmdCpuMode::Off) return {false, "forced off (--no-amd-cpu or KYTY_AMD_CPU=0)"};
	if (!auto_allowed) return {false, "auto: not available on this platform"};
	std::string missing;
	const auto  add = [&missing](bool supported, const char* name) {
		if (supported) return;
		if (!missing.empty()) missing += ", ";
		missing += name;
	};
	add(host.sse4a || (kinds & AmdPatchSse4a) == 0, "SSE4a");
	add(host.rdpid || (kinds & AmdPatchCpuExt) == 0, "RDPID");
	add(host.clwb || (kinds & AmdPatchCpuExt) == 0, "CLWB");
	if (missing.empty())
		return {false, "auto: nothing the selected patches rewrite is missing on the host"};
	return {true, "auto: host lacks " + missing};
}

inline uint32_t DefaultAmdPatchKinds(AmdCpuMode mode) {
	return mode == AmdCpuMode::On ? AmdPatchAll : mode == AmdCpuMode::Auto ? AmdPatchSse4a : 0u;
}

// "all", "none", or a comma separated list of sse4a, rsqrt, cpuext; anything else (including empty,
// "auto" and an empty list item) is not a selection.
inline std::optional<uint32_t> ParseAmdPatchKinds(std::string_view text) {
	if (text == "all") return AmdPatchAll;
	if (text == "none") return 0u;
	uint32_t kinds = 0;
	while (true) {
		const std::size_t      comma = text.find(',');
		const std::string_view item  = text.substr(0, comma);
		if (item == "sse4a")
			kinds |= AmdPatchSse4a;
		else if (item == "rsqrt")
			kinds |= AmdPatchRsqrt;
		else if (item == "cpuext")
			kinds |= AmdPatchCpuExt;
		else
			return std::nullopt;
		if (comma == std::string_view::npos) return kinds;
		text.remove_prefix(comma + 1);
	}
}

// An unset or unparsable variable keeps the default set of the mode.
inline uint32_t ResolveAmdPatchKinds(const char* env, AmdCpuMode mode) {
	if (env != nullptr) {
		if (const auto kinds = ParseAmdPatchKinds(env)) return *kinds;
	}
	return DefaultAmdPatchKinds(mode);
}

inline std::string AmdPatchKindsName(uint32_t kinds) {
	if (kinds == AmdPatchAll) return "all";
	if (kinds == 0) return "none";
	std::string name;
	const auto  add = [&](uint32_t bit, const char* text) {
		if ((kinds & bit) == 0) return;
		if (!name.empty()) name += ",";
		name += text;
	};
	add(AmdPatchSse4a, "sse4a");
	add(AmdPatchRsqrt, "rsqrt");
	add(AmdPatchCpuExt, "cpuext");
	return name;
}

} // namespace Common

#endif /* EMULATOR_INCLUDE_EMULATOR_COMMON_AMDCPUMODE_H_ */
