#pragma once

#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <bitset>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

// Wave64 scalar masks known to have no bit set for a lane the wave did not launch. The host
// subgroup only holds launched lanes, so such a mask selects no lane the host cannot represent.
struct LaunchedMasks {
	static constexpr uint32_t Vcc  = 106u;
	static constexpr uint32_t Exec = 126u;

	// Indexed by the scalar operand code of the pair's low register.
	std::bitset<128> launched;
	std::bitset<128> equals_exec;
	// EXEC is all ones, so lanes that were not launched execute too.
	bool exec_full = false;

	[[nodiscard]] bool Launched(const Decoder::Operand& operand) const;
	void               Update(const Decoder::Instruction& inst, bool whole_quads);
	void               Meet(const LaunchedMasks& other);
	bool               operator==(const LaunchedMasks& other) const = default;
};

// Masks at the entry of each CFG block, in cfg.blocks order. Wave entry EXEC is launched.
[[nodiscard]] std::vector<LaunchedMasks>
ComputeEntryLaunchedMasks(const Decoder::Program& decoded, const CFG::Graph& cfg, bool whole_quads);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
