#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace PackedTextureRowDetail {

using Domain = DescriptorSource::InlineDescriptor::AddressTable::TextureRowDomain;
using Channel = DescriptorSource::InlineDescriptor::AddressTable::TextureRowChannel;

// This is a conditional IR proof, not a memory-content reader. Every image
// origin means one byte of an RGBA8_UINT ImageRead. The caller must separately
// prove the runtime format, clean contents and all current-draw writers.
class Proof {
public:
    Proof(const Program& program, Value row): program(program), use(row.Resolve().TryInstruction()) {}

    std::optional<Domain> Run(Value row) {
        Domain result;
        if ((program.wave_size != 32u && program.wave_size != 64u) ||
            !Reduction(row, {}, result) || result.images.empty()) return {};
        AddConstant(result, 0u); // Bounds/disabled-source lane moves can produce zero.
        std::ranges::sort(result.images, {}, &Channel::image);
        std::ranges::sort(result.constants);
        return result;
    }

private:
    const Program& program;
    const Inst* use;
    size_t remaining = 4096;
    std::unordered_set<const Inst*> reduction_active;
    struct Memo { Value value; Value active; uint32_t byte; Domain domain; };
    std::vector<Memo> reduction_memos, byte_memos;
    std::vector<std::pair<const Block*, bool>> exit_memos;

    Value Canonical(Value value) const {
        value = value.Resolve();
        const auto invariant = ResolveInvariantPhi(program, value);
        return invariant.IsEmpty() ? value : invariant.Resolve();
    }
    bool Same(Value left, Value right) const {
        return Canonical(left) == Canonical(right);
    }
    bool Spend() { return remaining != 0u && --remaining != 0u; }
    static bool Immediate(Value value, uint32_t& result) {
        value = value.Resolve();
        if (!value.IsImmediate() || value.GetType() != Type::U32) return false;
        result = value.U32(); return true;
    }
    static void AddConstant(Domain& output, uint32_t value) {
        if (std::ranges::find(output.constants, value) == output.constants.end()) output.constants.push_back(value);
    }
    static void Merge(Domain& output, const Domain& input) {
        for (const auto value: input.constants) AddConstant(output, value);
        for (const auto origin: input.images) {
            auto found = std::ranges::find(output.images, origin.image, &Channel::image);
            if (found == output.images.end()) output.images.push_back(origin);
            else found->channels |= origin.channels;
        }
    }
    static bool ConstantOnly(const Domain& value, uint32_t constant) {
        return value.images.empty() && value.constants.size() == 1u && value.constants[0] == constant;
    }
    bool True(Value value) {
        if (!Spend()) return false;
        value = Canonical(value);
        if (value.IsImmediate()) return value.GetType() == Type::U1 && value.U1();
        const auto* inst = value.TryInstruction();
        if (inst == nullptr || inst->NumArgs() != 2u) return false;
        if (inst->GetOpcode() == ValueOpcode::LogicalAnd) return True(inst->Arg(0)) && True(inst->Arg(1));
        if (inst->GetOpcode() != ValueOpcode::LogicalOr) return false;
        for (size_t i = 0; i < 2u; ++i) {
            const auto* negate = Canonical(inst->Arg(i)).TryInstruction();
            if (negate != nullptr && negate->GetOpcode() == ValueOpcode::LogicalNot &&
                negate->NumArgs() == 1u && Same(negate->Arg(0), inst->Arg(i ^ 1u))) return true;
        }
        return false;
    }
    bool Conjuncts(Value value, std::vector<Value>& leaves) {
        if (!Spend()) return false;
        value = Canonical(value);
        if (value.IsImmediate() && value.GetType() == Type::U1 && value.U1()) return true;
        const auto* inst = value.TryInstruction();
        if (inst != nullptr && inst->GetOpcode() == ValueOpcode::LogicalAnd && inst->NumArgs() == 2u)
            return Conjuncts(inst->Arg(0), leaves) && Conjuncts(inst->Arg(1), leaves);
        if (std::ranges::find(leaves, value) == leaves.end()) leaves.push_back(value);
        return true;
    }
    bool EqualMasks(Value left, Value right) {
        std::vector<Value> a, b;
        if (!Conjuncts(left, a) || !Conjuncts(right, b) || a.size() != b.size()) return false;
        return std::ranges::all_of(a, [&](Value value) { return std::ranges::find(b, value) != b.end(); });
    }
    bool Implies(Value selected, Value required) {
        if (required.IsEmpty()) return false;
        if (True(required)) return true;
        if (selected.IsEmpty()) return false;
        std::vector<Value> a, b;
        if (!Conjuncts(selected, a) || !Conjuncts(required, b)) return false;
        return std::ranges::all_of(b, [&](Value value) { return std::ranges::find(a, value) != a.end(); });
    }
    const BlockInfo* Info(const Block* block) const {
        const auto found = std::ranges::find(program.blocks, block);
        const auto index = static_cast<size_t>(found - program.blocks.begin());
        return found == program.blocks.end() || index >= program.block_info.size() ? nullptr : &program.block_info[index];
    }
    const Block* BlockById(uint32_t id) const {
        const auto found = std::ranges::find(program.block_info, id, &BlockInfo::id);
        const auto index = static_cast<size_t>(found - program.block_info.begin());
        return found == program.block_info.end() || index >= program.blocks.size() ? nullptr : program.blocks[index];
    }
    bool Reachable(const Block* start, const Block* target, const Block* avoid = nullptr) {
        std::vector<const Block*> pending{start}, seen;
        while (!pending.empty()) {
            if (!Spend()) return true; // Exhaustion must make dominance fail conservatively.
            const auto* block = pending.back(); pending.pop_back();
            if (block == nullptr || block == avoid || std::ranges::find(seen, block) != seen.end()) continue;
            if (block == target) return true;
            seen.push_back(block);
            for (const auto* next: block->ImmSuccessors()) pending.push_back(next);
        }
        return false;
    }
    bool ExitDominatesUse(const Block* exit) {
        for (const auto [block, result]: exit_memos) if (block == exit) return result;
        const bool result = use != nullptr && use->Parent() != nullptr && !program.blocks.empty() &&
            Reachable(program.blocks.front(), use->Parent()) &&
            !Reachable(program.blocks.front(), use->Parent(), exit);
        exit_memos.emplace_back(exit, result);
        return result;
    }

    // A terminating shrinking-mask loop initializes every initially-active lane:
    // the only removed lanes are precisely those whose accumulator is written.
    // Arbitrary inactive-lane accumulator seeds are deliberately not admitted.
    bool Waterfall(const Inst& select, Value active, Domain& result, uint32_t byte) {
        if (select.NumArgs() != 3u || active.IsEmpty()) return false;
        const auto* accumulator = select.Arg(2).Resolve().TryInstruction();
        if (accumulator == nullptr || accumulator->GetOpcode() != ValueOpcode::Phi || accumulator->NumArgs() != 2u) return false;
        const auto* header = accumulator->Parent();
        const auto* body = select.Parent();
        const auto* header_info = Info(header);
        const auto* body_info = Info(body);
        if (header == nullptr || body == nullptr || header_info == nullptr || body_info == nullptr ||
            header->ImmSuccessors().size() != 1u || header->ImmSuccessors()[0] != body ||
            body->ImmSuccessors().size() != 2u || body_info->terminator.kind != CFG::TerminatorKind::ConditionalBranch) return false;
        bool backedge = false;
        for (size_t i = 0; i < 2u; ++i)
            if (accumulator->PhiBlock(i) == body && Same(accumulator->Arg(i), Value(const_cast<Inst*>(&select)))) backedge = true;
        if (!backedge) return false;
        Value condition = body_info->condition.Resolve();
        bool negate = false;
        while (const auto* inst = condition.TryInstruction()) {
            if (inst->GetOpcode() == ValueOpcode::LogicalNot && inst->NumArgs() == 1u) {
                negate = !negate; condition = inst->Arg(0).Resolve(); continue;
            }
            break;
        }
        const auto* branch = condition.TryInstruction();
        if (branch == nullptr || branch->GetOpcode() != ValueOpcode::ConditionRef || branch->NumArgs() != 1u) return false;
        const auto kind = branch->Flags<CFG::BranchCondition>();
        if (kind != CFG::BranchCondition::ExecNonZero && kind != CFG::BranchCondition::ExecZero) return false;
        const bool true_is_nonzero = (kind == CFG::BranchCondition::ExecNonZero) != negate;
        const auto* repeat = BlockById(true_is_nonzero ? body_info->terminator.true_block : body_info->terminator.false_block);
        const auto* exit = BlockById(true_is_nonzero ? body_info->terminator.false_block : body_info->terminator.true_block);
        if (repeat != header || exit == nullptr || exit == body || exit == header || !ExitDominatesUse(exit)) return false;
        const Value after = branch->Arg(0).Resolve();
        std::vector<Value> factors;
        if (!Conjuncts(after, factors) || factors.size() != 2u) return false;
        for (size_t i = 0; i < 2u; ++i) {
            const Value remaining_mask = factors[i];
            const auto* remaining_phi = remaining_mask.TryInstruction();
            const auto* remove = factors[i ^ 1u].TryInstruction();
            if (remaining_phi == nullptr || remaining_phi->GetOpcode() != ValueOpcode::Phi ||
                remaining_phi->Parent() != header || remaining_phi->NumArgs() != 2u ||
                remove == nullptr || remove->GetOpcode() != ValueOpcode::LogicalNot || remove->NumArgs() != 1u) continue;
            bool initial = false, updated = false;
            for (size_t operand = 0; operand < 2u; ++operand) {
                if (remaining_phi->PhiBlock(operand) == body) updated = Same(remaining_phi->Arg(operand), after);
                else initial = Same(remaining_phi->Arg(operand), active);
            }
            if (!initial || !updated) continue;
            // remaining && !removed == remaining && !written, modulo repeated ANDs.
            const auto written = select.Arg(0);
            auto& combined = scratch.emplace_back(ValueOpcode::LogicalAnd);
            combined.SetArg(0, remaining_mask); combined.SetArg(1, remove->Arg(0));
            if (!EqualMasks(written, Value(&combined))) continue;
            const auto* extract = select.Arg(1).Resolve().TryInstruction();
            if (extract == nullptr || extract->GetOpcode() != ValueOpcode::CompositeExtractU32x4 || extract->NumArgs() != 2u) continue;
            const auto* image = extract->Arg(0).Resolve().TryInstruction();
            if (image == nullptr || image->GetOpcode() != ValueOpcode::ImageRead || image->Parent() != body ||
                image->NumArgs() != 3u || !EqualMasks(image->Arg(2), written)) continue;
            return ImageComponent(*extract, result, byte);
        }
        return false;
    }
    std::list<Inst> scratch;

    bool ImageComponent(const Inst& extract, Domain& result, uint32_t byte) {
        uint32_t channel;
        if (extract.NumArgs() != 2u || !Immediate(extract.Arg(1), channel) || channel >= 4u) return false;
        const auto* image = extract.Arg(0).Resolve().TryInstruction();
        if (image == nullptr || image->GetOpcode() != ValueOpcode::ImageRead || image->NumArgs() != 3u) return false;
        const auto index = image->Flags<MemoryFlags>().index;
        if (index >= program.memory_info.size()) return false;
        const auto& memory = program.memory_info[index];
        if (memory.kind != ResourceKind::Image || memory.resource >= program.info.images.size()) return false;
        if (byte != 0u) { AddConstant(result, 0u); return true; }
        result.images.push_back({memory.resource, 1u << channel});
        return true;
    }

    bool PreservesPacked(Value value, const Inst* anchor, std::unordered_set<const Inst*>& visiting) {
        if (!Spend()) return false;
        value = value.Resolve();
        const auto* inst = value.TryInstruction();
        if (inst == anchor) return true;
        if (inst == nullptr || !visiting.insert(inst).second) return false;
        bool result = false;
        if (inst->GetOpcode() == ValueOpcode::ShiftRightLogical32 && inst->NumArgs() == 2u) {
            uint32_t shift;
            result = Immediate(inst->Arg(1), shift) && shift == 8u && inst->Arg(0).Resolve().TryInstruction() == anchor;
        } else if (inst->GetOpcode() == ValueOpcode::Phi || inst->GetOpcode() == ValueOpcode::SelectU32) {
            const auto first = inst->GetOpcode() == ValueOpcode::SelectU32 ? 1u : 0u;
            result = inst->NumArgs() > first;
            for (size_t i = first; result && i < inst->NumArgs(); ++i) result = PreservesPacked(inst->Arg(i), anchor, visiting);
        }
        visiting.erase(inst); return result;
    }
    bool Byte(Value value, uint32_t byte, Value active, Domain& output) {
        value = Canonical(value); active = Canonical(active);
        for (const auto& memo: byte_memos)
            if (memo.value == value && memo.active == active && memo.byte == byte) { Merge(output, memo.domain); return true; }
        Domain domain;
        if (!ByteImpl(value, byte, active, domain)) return false;
        byte_memos.push_back({value, active, byte, domain}); Merge(output, domain); return true;
    }
    bool ByteImpl(Value value, uint32_t byte, Value active, Domain& output) {
        if (!Spend() || byte >= 4u) return false;
        value = Canonical(value);
        uint32_t immediate;
        if (Immediate(value, immediate)) { AddConstant(output, (immediate >> (byte * 8u)) & 255u); return true; }
        const auto* inst = value.TryInstruction();
        if (inst == nullptr) return false;
        if (inst->GetOpcode() == ValueOpcode::CompositeExtractU32x4) {
            const auto* image = inst->Arg(0).Resolve().TryInstruction();
            return image != nullptr && image->GetOpcode() == ValueOpcode::ImageRead && image->NumArgs() == 3u &&
                Implies(active, image->Arg(2)) && ImageComponent(*inst, output, byte);
        }
        if (inst->GetOpcode() == ValueOpcode::SelectU32 && inst->NumArgs() == 3u) {
            if (True(inst->Arg(0)) || Implies(active, inst->Arg(0))) return Byte(inst->Arg(1), byte, active, output);
            return Waterfall(*inst, active, output, byte);
        }
        if (inst->GetOpcode() == ValueOpcode::Phi && inst->NumArgs() == 2u) {
            for (size_t back = 0; back < 2u; ++back) {
                std::unordered_set<const Inst*> visiting;
                if (!PreservesPacked(inst->Arg(back), inst, visiting)) continue;
                Domain domain;
                // Every later byte is an original byte or shift-fill zero.
                for (uint32_t initial_byte = 0; initial_byte < 4u; ++initial_byte) {
                    Domain initial;
                    if (!Byte(inst->Arg(back ^ 1u), initial_byte, active, initial)) return false;
                    Merge(domain, initial);
                }
                AddConstant(domain, 0u); Merge(output, domain); return true;
            }
            return false;
        }
        if ((inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 || inst->GetOpcode() == ValueOpcode::ShiftRightLogical32) && inst->NumArgs() == 2u) {
            uint32_t shift;
            if (!Immediate(inst->Arg(1), shift) || shift >= 32u || shift % 8u != 0u) return false;
            const int input_byte = inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 ? int(byte) - int(shift / 8u) : int(byte) + int(shift / 8u);
            if (input_byte < 0 || input_byte >= 4) { AddConstant(output, 0u); return true; }
            return Byte(inst->Arg(0), uint32_t(input_byte), active, output);
        }
        if (inst->GetOpcode() == ValueOpcode::BitwiseOr32 && inst->NumArgs() == 2u) {
            Domain left, right;
            if (!Byte(inst->Arg(0), byte, active, left) || !Byte(inst->Arg(1), byte, active, right)) return false;
            if (ConstantOnly(left, 255u) || ConstantOnly(right, 255u)) { AddConstant(output, 255u); return true; }
            if (ConstantOnly(left, 0u)) { Merge(output, right); return true; }
            if (ConstantOnly(right, 0u)) { Merge(output, left); return true; }
            return false; // An OR of two unrelated bytes can create a new row.
        }
        return false;
    }

    bool Reduction(Value value, Value active, Domain& output) {
        value = Canonical(value); active = Canonical(active);
        for (const auto& memo: reduction_memos)
            if (memo.value == value && memo.active == active) { Merge(output, memo.domain); return true; }
        Domain domain;
        if (!ReductionImpl(value, active, domain)) return false;
        reduction_memos.push_back({value, active, 0u, domain}); Merge(output, domain); return true;
    }
    bool ReductionImpl(Value value, Value active, Domain& output) {
        if (!Spend()) return false;
        value = Canonical(value);
        uint32_t immediate;
        if (Immediate(value, immediate)) { AddConstant(output, immediate); return true; }
        const auto* inst = value.TryInstruction();
        if (inst == nullptr || !reduction_active.insert(inst).second) return false;
        bool result = false;
        const auto opcode = inst->GetOpcode();
        if (opcode == ValueOpcode::UMin32 || opcode == ValueOpcode::UMinTri32) {
            result = true;
            for (size_t i = 0; result && i < inst->NumArgs(); ++i) result = Reduction(inst->Arg(i), active, output);
        } else if (opcode == ValueOpcode::ReadLane && inst->NumArgs() == 2u) {
            uint32_t lane;
            result = Immediate(inst->Arg(1), lane) && lane < program.wave_size && Reduction(inst->Arg(0), {}, output);
        } else if (opcode == ValueOpcode::ReadFirstLane && inst->NumArgs() == 2u) {
            // An empty selection can read undefined lane data. A nontrivial
            // mask needs an independent nonempty-on-entry proof before use.
            result = True(inst->Arg(1)) && Reduction(inst->Arg(0), {}, output);
        } else if (opcode == ValueOpcode::SelectU32 && inst->NumArgs() == 3u) {
            if (True(inst->Arg(0)) || Implies(active, inst->Arg(0))) result = Reduction(inst->Arg(1), active, output);
            else if (Immediate(inst->Arg(2), immediate)) {
                AddConstant(output, immediate);
                result = Reduction(inst->Arg(1), inst->Arg(0), output);
            }
        } else if (opcode == ValueOpcode::DppMoveU32 && inst->NumArgs() == 2u) {
            const auto flags = inst->Flags<DppMoveFlags>();
            result = !flags.fetch_inactive && True(inst->Arg(1)) && Reduction(inst->Arg(0), {}, output);
        } else if (opcode == ValueOpcode::DppUpdateU32 && inst->NumArgs() == 3u) {
            result = True(inst->Arg(2)) && Reduction(inst->Arg(0), {}, output) && Reduction(inst->Arg(1), {}, output);
        } else if (opcode == ValueOpcode::Permlane16U32 && inst->NumArgs() == 4u) {
            const auto flags = inst->Flags<PermlaneFlags>();
            result = !flags.fetch_inactive && True(inst->Arg(3)) && Reduction(inst->Arg(0), {}, output);
        } else if (opcode == ValueOpcode::BitwiseAnd32 && inst->NumArgs() == 2u) {
            for (size_t i = 0; i < 2u; ++i)
                if (Immediate(inst->Arg(i), immediate) && immediate == 255u) { result = Byte(inst->Arg(i ^ 1u), 0u, active, output); break; }
        } else if (opcode == ValueOpcode::BitFieldUExtract && inst->NumArgs() == 3u) {
            uint32_t offset, width;
            result = Immediate(inst->Arg(1), offset) && offset == 0u && Immediate(inst->Arg(2), width) && width == 8u && Byte(inst->Arg(0), 0u, active, output);
        } else if (opcode == ValueOpcode::CompositeExtractU32x4) result = Byte(value, 0u, active, output);
        reduction_active.erase(inst); return result;
    }
};

} // namespace PackedTextureRowDetail

inline std::optional<DescriptorSource::InlineDescriptor::AddressTable::TextureRowDomain>
ProveTextureWaveRowDomain(const Program& program, Value row) {
    return PackedTextureRowDetail::Proof(program, row).Run(row);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
