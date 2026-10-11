#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// A whole-lane U32 domain, independent of branch predicates and live memory.
// Lane-selection instructions are deliberately handled by the caller, which
// must establish a nonempty selection before using this all-lane domain.
inline std::optional<std::vector<uint32_t>> ProveFiniteConstantSelectors(
    const ResourcePlan& program, Value value) {
    std::unordered_set<uint32_t> constants;
    std::unordered_set<const Inst*> visiting, proved;
    size_t remaining = 1024u;
    const auto visit = [&](auto&& self, Value current) -> bool {
        if (remaining == 0u) return false;
        --remaining;
        current = current.Resolve();
        const auto invariant = ResolveInvariantPhi(program, current);
        if (!invariant.IsEmpty()) current = invariant.Resolve();
        if (current.IsImmediate()) {
            if (current.GetType() != Type::U32) return false;
            constants.insert(current.U32());
            return constants.size() <= 256u;
        }
        const auto* inst = current.TryInstruction();
        if (inst == nullptr || inst->GetType() != Type::U32) return false;
        if (proved.contains(inst)) return true;
        if (!visiting.insert(inst).second) return false;
        const auto opcode = inst->GetOpcode();
        bool result = false;
        if (opcode == ValueOpcode::SelectU32 && inst->NumArgs() == 3u) {
            result = self(self, inst->Arg(1)) && self(self, inst->Arg(2));
        } else if (opcode == ValueOpcode::Phi && inst->NumArgs() != 0u) {
            result = true;
            for (size_t i = 0; result && i < inst->NumArgs(); ++i) result = self(self, inst->Arg(i));
        }
        visiting.erase(inst);
        if (result) proved.insert(inst);
        return result;
    };
    if (!visit(visit, value) || constants.empty()) return {};
    std::vector<uint32_t> result(constants.begin(), constants.end());
    std::ranges::sort(result);
    return result;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
