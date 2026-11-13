#pragma once

#include "graphics/host_gpu/regionDefinitions.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
struct Program;
struct ResourceSnapshot;

// Conservative memory effects of the emitted address loads. Unknown offsets
// cover every U32 value; an unknown base or any address store rejects the plan.
// No shader hash, observed address, or allocation boundary is used as proof.
struct BdaReadWord {
	enum class Kind : uint8_t { Immediate, UserData, Flat };
	Kind     kind                                 = Kind::Immediate;
	uint32_t value                                = 0;
	bool     operator==(const BdaReadWord&) const = default;
};
struct BdaReadSpan {
	BdaReadWord low, high;
	int64_t     begin = 0, end = 0; // byte offsets, exclusive end
	bool        scalar_base = false;
	bool        operator==(const BdaReadSpan&) const = default;
};
struct BdaReadPlan {
	static constexpr uint32_t Capacity = 64;
	std::vector<BdaReadSpan>  spans;
	bool                      complete = false;
	bool                      operator==(const BdaReadPlan&) const = default;
};

[[nodiscard]] BdaReadPlan BuildBdaReadPlan(const Program& program);
// Transactional: a missing field, invalid address or overflow preserves output.
[[nodiscard]] bool EvaluateBdaReadPlan(const BdaReadPlan& plan, const ResourceSnapshot& snapshot,
                                       std::vector<GuestRange>& output);
} // namespace Libs::Graphics::ShaderRecompiler::IR
