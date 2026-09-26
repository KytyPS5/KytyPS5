#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>

#include <queue>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

struct ChainResult {
	Value value;
	Inst* write = nullptr;
};

bool IsLaneInvariantOpcode(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IMul32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::FPRecip32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPTrunc32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::SelectF32:
		case ValueOpcode::SelectU32: return true;
		default: return false;
	}
}

bool CanRebuildLaneInvariant(const Program& program, Value value, Value exec,
                             std::unordered_map<Inst*, bool>& memo,
                             std::unordered_set<Inst*>&       visiting) {
	value = value.Resolve();
	if (value.IsImmediate()) return true;
	auto* inst = value.TryInstruction();
	if (inst == nullptr) return false;
	if (const auto it = memo.find(inst); it != memo.end()) return it->second;
	if (!visiting.insert(inst).second) return false;

	bool result = false;
	if (inst->GetOpcode() == ValueOpcode::GetUserData ||
	    inst->GetOpcode() == ValueOpcode::GetShaderBase ||
	    inst->GetOpcode() == ValueOpcode::GetScalarRegister) {
		result = true;
	} else if (inst->GetOpcode() == ValueOpcode::ReadConstBuffer) {
		result = ValidateRuntimeValue(program, value, RuntimeValueType::Integer);
	} else if (inst->GetOpcode() == ValueOpcode::SelectU32 &&
	           inst->Arg(0).Resolve() == exec.Resolve()) {
		result = CanRebuildLaneInvariant(program, inst->Arg(1), exec, memo, visiting);
	} else if (IsLaneInvariantOpcode(inst->GetOpcode())) {
		result = true;
		for (size_t index = 0; result && index < inst->NumArgs(); index++) {
			result = CanRebuildLaneInvariant(program, inst->Arg(index), exec, memo, visiting);
		}
	}

	visiting.erase(inst);
	memo.emplace(inst, result);
	return result;
}

Value CloneLaneInvariant(Inst& inst, Block& block, Block::iterator insertion_point,
                         const std::vector<Value>& args) {
	const auto opcode = inst.GetOpcode();
	const auto flags  = inst.Flags<uint64_t>();
	switch (args.size()) {
		case 0: return Value(&*block.PrependNewInst(insertion_point, opcode, {}, flags));
		case 1: return Value(&*block.PrependNewInst(insertion_point, opcode, {args[0]}, flags));
		case 2:
			return Value(
			    &*block.PrependNewInst(insertion_point, opcode, {args[0], args[1]}, flags));
		case 3:
			return Value(&*block.PrependNewInst(insertion_point, opcode,
			                                    {args[0], args[1], args[2]}, flags));
		default: EXIT("unsupported lane-invariant opcode arity");
	}
	return {};
}

Value RebuildLaneInvariant(Value value, Value exec, Block& block, Block::iterator insertion_point,
                           std::unordered_map<Inst*, Value>& memo) {
	value = value.Resolve();
	if (value.IsImmediate()) return value;
	auto* inst = value.ResolveInstruction();
	if (const auto it = memo.find(inst); it != memo.end()) return it->second;
	if (inst->GetOpcode() == ValueOpcode::GetUserData ||
	    inst->GetOpcode() == ValueOpcode::GetShaderBase ||
	    inst->GetOpcode() == ValueOpcode::GetScalarRegister ||
	    inst->GetOpcode() == ValueOpcode::ReadConstBuffer) {
		return value;
	}
	if (inst->GetOpcode() == ValueOpcode::SelectU32 && inst->Arg(0).Resolve() == exec.Resolve()) {
		return RebuildLaneInvariant(inst->Arg(1), exec, block, insertion_point, memo);
	}

	std::vector<Value> args;
	args.reserve(inst->NumArgs());
	for (size_t index = 0; index < inst->NumArgs(); index++) {
		args.push_back(RebuildLaneInvariant(inst->Arg(index), exec, block, insertion_point, memo));
	}
	const auto rebuilt = CloneLaneInvariant(*inst, block, insertion_point, args);
	memo.emplace(inst, rebuilt);
	return rebuilt;
}

ChainResult SearchChain(Value value, uint32_t lane, uint32_t wave_size) {
	for (;;) {
		value      = value.Resolve();
		auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::WriteLane) {
			return {value};
		}
		const auto selector = inst->Arg(2).Resolve();
		if (!selector.IsImmediate() || selector.GetType() != Type::U32) {
			return {value};
		}
		if (selector.U32() % wave_size == lane) {
			return {value, inst};
		}
		value = inst->Arg(0);
	}
}

bool IsPossibleToEliminate(Value source, uint32_t lane, uint32_t wave_size) {
	std::queue<Value>         queue;
	std::unordered_set<Inst*> visited;
	queue.push(source);

	while (!queue.empty()) {
		const auto chain = SearchChain(queue.front(), lane, wave_size);
		queue.pop();
		if (chain.write != nullptr) {
			continue;
		}
		auto* inst = chain.value.TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::Phi || inst->NumArgs() == 0) {
			return false;
		}
		if (!visited.insert(inst).second) {
			continue;
		}
		for (size_t index = inst->NumArgs(); index-- > 0;) {
			queue.push(inst->Arg(index));
		}
	}
	return true;
}

using PhiMap = std::unordered_map<Inst*, Inst*>;

Value GetRealValue(PhiMap& phi_map, Value source, uint32_t lane, uint32_t wave_size) {
	const auto chain = SearchChain(source, lane, wave_size);
	if (chain.write != nullptr) {
		return chain.write->Arg(1);
	}

	auto* inst = chain.value.ResolveInstruction();
	EXIT_IF(inst->GetOpcode() != ValueOpcode::Phi);
	const auto [entry, is_new] = phi_map.try_emplace(inst);
	if (!is_new) {
		return Value(entry->second);
	}

	auto* block           = inst->Parent();
	auto  insertion_point = std::find_if(block->begin(), block->end(),
	                                     [&](const Inst& candidate) { return &candidate == inst; });
	EXIT_IF(insertion_point == block->end());
	auto& phi = *block->PrependNewInst(insertion_point, ValueOpcode::Phi);
	phi.SetFlags(Type::U32);
	entry->second = &phi;

	std::vector<Value> arguments;
	arguments.reserve(inst->NumArgs());
	for (size_t index = 0; index < inst->NumArgs(); index++) {
		arguments.push_back(GetRealValue(phi_map, inst->Arg(index), lane, wave_size));
	}
	const auto first = arguments.front().Resolve();
	if (std::ranges::all_of(arguments,
	                        [&](Value argument) { return argument.Resolve() == first; })) {
		phi.ReplaceUsesWith(first);
	} else {
		for (size_t index = 0; index < arguments.size(); index++) {
			phi.AddPhiOperand(inst->PhiBlock(index), arguments[index]);
		}
	}
	return Value(&phi);
}

} // namespace

ReadLaneStats EliminateReadLane(Program& program, uint32_t wave_size) {
	ReadLaneStats stats;
	if (wave_size != 32u && wave_size != 64u) {
		return stats;
	}

	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::ReadFirstLane) {
				const auto                      source = inst.Arg(0);
				const auto                      exec   = inst.Arg(1);
				std::unordered_map<Inst*, bool> validation_memo;
				std::unordered_set<Inst*>       visiting;
				if (!CanRebuildLaneInvariant(program, source, exec, validation_memo, visiting)) {
					continue;
				}
				auto* current_block = inst.Parent();
				auto  insertion_point =
				    std::find_if(current_block->begin(), current_block->end(),
				                 [&](const Inst& candidate) { return &candidate == &inst; });
				std::unordered_map<Inst*, Value> rebuild_memo;
				const auto rebuilt = RebuildLaneInvariant(source, exec, *current_block,
				                                          insertion_point, rebuild_memo);
				inst.ReplaceUsesWith(rebuilt);
				stats.rewritten_reads++;
				continue;
			}
			if (inst.GetOpcode() != ValueOpcode::ReadLane) {
				continue;
			}
			const auto selector = inst.Arg(1).Resolve();
			if (!selector.IsImmediate() || selector.GetType() != Type::U32) {
				continue;
			}

			const auto lane  = selector.U32() % wave_size;
			const auto chain = SearchChain(inst.Arg(0), lane, wave_size);
			if (chain.write != nullptr) {
				inst.ReplaceUsesWith(chain.write->Arg(1));
				stats.rewritten_reads++;
				continue;
			}
			auto* producer = chain.value.TryInstruction();
			if (producer == nullptr || producer->GetOpcode() != ValueOpcode::Phi ||
			    !IsPossibleToEliminate(chain.value, lane, wave_size)) {
				continue;
			}

			PhiMap phi_map;
			inst.ReplaceUsesWith(GetRealValue(phi_map, chain.value, lane, wave_size));
			stats.rewritten_reads++;
		}
	}
	return stats;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
