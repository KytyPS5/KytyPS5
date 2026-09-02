#include "graphics/shader/recompiler/ir/passes/WaterfallDescriptor.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint32_t LaneIndexBits = 5u;
constexpr uint32_t LaneIndexMask = (1u << LaneIndexBits) - 1u;

bool ImmediateU32(Value value, uint32_t& result) {
	value = value.Resolve();
	if (!value.IsImmediate() || value.GetType() != Type::U32) {
		return false;
	}
	result = value.U32();
	return true;
}

const Inst* Match(Value value, ValueOpcode opcode, size_t args) {
	const auto* inst = value.Resolve().TryInstruction();
	return inst != nullptr && inst->GetOpcode() == opcode && inst->NumArgs() == args ? inst
	                                                                                : nullptr;
}

bool CommutativeImmediate(const Inst& inst, uint32_t& immediate, Value& other) {
	if (ImmediateU32(inst.Arg(1), immediate)) {
		other = inst.Arg(0);
		return true;
	}
	if (ImmediateU32(inst.Arg(0), immediate)) {
		other = inst.Arg(1);
		return true;
	}
	return false;
}

const Inst* MatchLowestSetBit(Value value, const Inst& mask_phi) {
	const auto* shift = Match(value, ValueOpcode::ShiftLeftLogical32, 2);
	uint32_t    one   = 0;
	if (shift == nullptr || !ImmediateU32(shift->Arg(0), one) || one != 1u) {
		return nullptr;
	}
	const auto* masked = Match(shift->Arg(1), ValueOpcode::BitwiseAnd32, 2);
	uint32_t    bits   = 0;
	Value       index;
	if (masked == nullptr || !CommutativeImmediate(*masked, bits, index) || bits != LaneIndexMask) {
		return nullptr;
	}
	const auto* lsb = Match(index, ValueOpcode::FindILsb32, 1);
	if (lsb == nullptr || lsb->Arg(0).Resolve().TryInstruction() != &mask_phi) {
		return nullptr;
	}
	return lsb;
}

const Inst* MatchClearedMask(Value latch, const Inst& mask_phi) {
	const auto* cleared = Match(latch, ValueOpcode::BitwiseXor32, 2);
	if (cleared == nullptr) {
		return nullptr;
	}
	for (size_t side = 0; side < 2u; side++) {
		if (cleared->Arg(side).Resolve().TryInstruction() != &mask_phi) {
			continue;
		}
		if (const auto* lsb = MatchLowestSetBit(cleared->Arg(1u - side), mask_phi)) {
			return lsb;
		}
	}
	return nullptr;
}

bool Reaches(Value value, const Inst& target, std::vector<const Inst*>& visited) {
	const auto* inst = value.Resolve().TryInstruction();
	if (inst == nullptr) {
		return false;
	}
	if (inst == &target) {
		return true;
	}
	if (std::ranges::find(visited, inst) != visited.end()) {
		return false;
	}
	visited.push_back(inst);
	for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
		if (Reaches(inst->Arg(arg), target, visited)) {
			return true;
		}
	}
	return false;
}

bool MatchKey(const Inst& index, Value& key, const Inst*& compare_out) {
	const Inst* compare = nullptr;
	size_t      operand = 0;
	for (const auto& use: index.Uses()) {
		if (use.user == nullptr || use.user->GetOpcode() != ValueOpcode::IEqual32 ||
		    use.user->NumArgs() != 2u) {
			continue;
		}
		if (compare != nullptr) {
			return false;
		}
		compare = use.user;
		operand = use.operand;
	}
	if (compare == nullptr) {
		return false;
	}
	key         = compare->Arg(operand == 0u ? 1u : 0u);
	compare_out = compare;
	return true;
}

const Inst* MatchTableOffset(const Inst& index, uint32_t& stride_shift, uint32_t& table_offset,
                             const Inst*& scaled_out) {
	const Inst* scaled = nullptr;
	for (const auto& use: index.Uses()) {
		if (use.user == nullptr || use.user->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
		    use.user->NumArgs() != 2u || use.operand != 0u) {
			continue;
		}
		if (scaled != nullptr) {
			return nullptr;
		}
		scaled = use.user;
	}
	if (scaled == nullptr || !ImmediateU32(scaled->Arg(1), stride_shift) || stride_shift == 0u ||
	    stride_shift > 31u) {
		return nullptr;
	}
	const Inst* based = nullptr;
	for (const auto& use: scaled->Uses()) {
		uint32_t immediate = 0;
		Value    other;
		if (use.user == nullptr || use.user->GetOpcode() != ValueOpcode::IAdd32 ||
		    use.user->NumArgs() != 2u || !CommutativeImmediate(*use.user, immediate, other)) {
			continue;
		}
		if (based != nullptr) {
			return nullptr;
		}
		scaled_out   = scaled;
		based        = use.user;
		table_offset = immediate;
	}
	return based;
}

bool LaneIndexSource(Value value, Value& source, uint32_t& shift) {
	while (const auto* select = Match(value, ValueOpcode::SelectU32, 3)) {
		value = select->Arg(1);
	}
	if (const auto* extract = Match(value, ValueOpcode::BitFieldUExtract, 3)) {
		uint32_t width = 0;
		if (!ImmediateU32(extract->Arg(1), shift) || !ImmediateU32(extract->Arg(2), width) ||
		    shift + width != 32u) {
			return false;
		}
		source = extract->Arg(0);
		return true;
	}
	if (const auto* shifted = Match(value, ValueOpcode::ShiftRightLogical32, 2)) {
		if (!ImmediateU32(shifted->Arg(1), shift)) {
			return false;
		}
		source = shifted->Arg(0);
		return true;
	}
	source = value;
	shift  = 0;
	return true;
}

bool SameLaneIndex(const Program& program, Value left, Value right) {
	while (const auto* select = Match(left, ValueOpcode::SelectU32, 3)) {
		left = select->Arg(1);
	}
	while (const auto* select = Match(right, ValueOpcode::SelectU32, 3)) {
		right = select->Arg(1);
	}
	if (EquivalentValue(program, left, right)) {
		return true;
	}
	Value    left_source;
	Value    right_source;
	uint32_t left_shift  = 0;
	uint32_t right_shift = 0;
	return LaneIndexSource(left, left_source, left_shift) &&
	       LaneIndexSource(right, right_source, right_shift) && left_shift == right_shift &&
	       EquivalentValue(program, left_source, right_source);
}

bool IsLaneReduction(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::SelectU32:
		case ValueOpcode::DppMoveU32:
		case ValueOpcode::DppUpdateU32:
		case ValueOpcode::Permlane16U32:
		case ValueOpcode::ReadLane: return true;
		default: return false;
	}
}

const Inst* FindBallotBit(const Program& program, Value entry, Value key,
                          std::vector<const Inst*>& visited) {
	const auto* inst = entry.Resolve().TryInstruction();
	if (inst == nullptr || std::ranges::find(visited, inst) != visited.end()) {
		return nullptr;
	}
	visited.push_back(inst);
	if (inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 && inst->NumArgs() == 2u) {
		uint32_t    one    = 0;
		uint32_t    bits   = 0;
		Value       index;
		const auto* masked = Match(inst->Arg(1), ValueOpcode::BitwiseAnd32, 2);
		if (ImmediateU32(inst->Arg(0), one) && one == 1u && masked != nullptr &&
		    CommutativeImmediate(*masked, bits, index) && bits == LaneIndexMask &&
		    SameLaneIndex(program, index, key)) {
			return inst;
		}
	}
	for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
		if (const auto* found = FindBallotBit(program, inst->Arg(arg), key, visited)) {
			return found;
		}
	}
	return nullptr;
}

bool ReducesInto(const Inst& node, const Inst& root, std::vector<const Inst*>& visited) {
	if (&node == &root) {
		return true;
	}
	if (std::ranges::find(visited, &node) != visited.end()) {
		return false;
	}
	visited.push_back(&node);
	for (const auto& use: node.Uses()) {
		if (use.user != nullptr && IsLaneReduction(use.user->GetOpcode()) &&
		    ReducesInto(*use.user, root, visited)) {
			return true;
		}
	}
	return false;
}
const Inst* MatchImageHandle(const Program& program, const Inst& based, Value& heap) {
	const Inst* handle = nullptr;
	for (const auto& use: based.Uses()) {
		if (use.user == nullptr || use.user->GetOpcode() != ValueOpcode::LoadAddressU32) {
			continue;
		}
		for (const auto& consumer: use.user->Uses()) {
			if (consumer.user == nullptr ||
			    consumer.user->GetOpcode() != ValueOpcode::GetImageResource) {
				continue;
			}
			if (handle != nullptr && handle != consumer.user) {
				return nullptr;
			}
			handle = consumer.user;
		}
	}
	if (handle == nullptr || handle->NumArgs() != 8u) {
		return nullptr;
	}
	for (size_t dword = 0; dword < handle->NumArgs(); dword++) {
		const auto* load = Match(handle->Arg(dword), ValueOpcode::LoadAddressU32, 4);
		if (load == nullptr) {
			return nullptr;
		}
		if (load->Arg(1).Resolve().TryInstruction() != &based) {
			uint32_t    immediate = 0;
			Value       other;
			const auto* shifted = Match(load->Arg(1), ValueOpcode::IAdd32, 2);
			if (shifted == nullptr || !CommutativeImmediate(*shifted, immediate, other) ||
			    other.Resolve().TryInstruction() != &based) {
				return nullptr;
			}
		}
		if (heap.IsEmpty()) {
			heap = load->Arg(0);
		} else if (!EquivalentValue(program, heap, load->Arg(0))) {
			return nullptr;
		}
	}
	return handle;
}

}

std::vector<WaterfallDescriptor> FindWaterfallDescriptors(const Program& program) {
	std::vector<WaterfallDescriptor> result;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::Phi || inst.NumArgs() != 2u) {
				continue;
			}
			for (size_t latch = 0; latch < 2u; latch++) {
				const auto* lsb = MatchClearedMask(inst.Arg(latch), inst);
				if (lsb == nullptr) {
					continue;
				}
				const auto               entry = inst.Arg(1u - latch);
				std::vector<const Inst*> visited;
				if (Reaches(entry, inst, visited)) {
					continue;
				}
				WaterfallDescriptor found;
				found.mask_phi = &inst;
				found.index    = lsb;
				found.entry    = entry;
				found.latch = latch;
				if (!MatchKey(*lsb, found.key, found.compare)) {
					break;
				}
				std::vector<const Inst*> seen;
				const auto* bit = FindBallotBit(program, found.entry, found.key, seen);
				std::vector<const Inst*> reduction;
				const auto* root = found.entry.Resolve().TryInstruction();
				if (bit == nullptr || root == nullptr || !ReducesInto(*bit, *root, reduction)) {
					break;
				}
				const auto* based =
				    MatchTableOffset(*lsb, found.stride_shift, found.table_offset, found.scaled);
				if (based == nullptr) {
					break;
				}
				found.handle = MatchImageHandle(program, *based, found.heap);
				if (found.handle == nullptr) {
					break;
				}
				result.push_back(found);
				break;
			}
		}
	}
	return result;
}

uint32_t RewriteWaterfallDescriptors(Program& program) {
	uint32_t rewritten = 0;
	for (const auto& found: FindWaterfallDescriptors(program)) {
		const_cast<Inst*>(found.scaled)->SetArg(0, found.key);
		auto*      compare = const_cast<Inst*>(found.compare);
		auto*      block   = compare->Parent();
		const auto at      = std::ranges::find_if(
            *block, [&](const Inst& inst) { return &inst == compare; });
		const auto bound = block->PrependNewInst(
		    at, ValueOpcode::ULessThan32, {found.key, Value(uint32_t {1} << LaneIndexBits)});
		compare->ReplaceUsesWith(Value(&*bound));
		auto* mask = const_cast<Inst*>(found.mask_phi);
		mask->SetArg(found.latch, Value(0u));
		mask->SetArg(found.latch == 0u ? 1u : 0u, Value(1u));
		rewritten++;
	}
	return rewritten;
}

}
