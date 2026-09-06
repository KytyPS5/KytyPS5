#pragma once

#include "graphics/shader/recompiler/ShaderHostProfile.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct F64Certificate {
	bool needs_native64 = false;
	bool needs_fma64 = false;
	bool needs_narrow_f32 = false;
	const Inst* failed_inst = nullptr;
	std::string error;
};

// Analyze final live SSA. This admits only zero/normal, finite binary64 arithmetic
// with proved bounds; it never infers guest MODE or host capabilities from defaults.
[[nodiscard]] F64Certificate AnalyzeF64Program(
    const Program& program, const ShaderFloatingPointState& initial_fp_state,
    const ShaderHostProfile& host_profile);

} // namespace Libs::Graphics::ShaderRecompiler::IR
