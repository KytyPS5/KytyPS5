#include "graphics/shader/recompiler/ir/ValueProgram.h"

#include <fmt/format.h>
#include <map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

bool Fail(std::string* error, const std::string& message) {
	if (error != nullptr) {
		*error = message;
	}
	return false;
}

bool IsRegisterGet(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetThreadBitScalarRegister:
		case ValueOpcode::GetScalarRegister:
		case ValueOpcode::GetVectorRegister:
		case ValueOpcode::GetGotoVariable:
		case ValueOpcode::GetScc:
		case ValueOpcode::GetExec:
		case ValueOpcode::GetExecLo:
		case ValueOpcode::GetExecHi:
		case ValueOpcode::GetVcc:
		case ValueOpcode::GetVccLo:
		case ValueOpcode::GetVccHi:
		case ValueOpcode::GetM0: return true;
		default: return false;
	}
}

bool IsRuntimeRead(ValueOpcode opcode) {
	return opcode == ValueOpcode::LoadAddressU32 || opcode == ValueOpcode::ReadConstBuffer;
}

bool EquivalentValue(const ValueProgram& program, Value left, Value right,
                     std::vector<std::pair<const Inst*, const Inst*>>& visited) {
	left  = left.Resolve();
	right = right.Resolve();
	if (left == right) {
		return true;
	}
	if (left.IsImmediate() || right.IsImmediate() || left.GetType() != right.GetType()) {
		return false;
	}
	const auto* lhs = left.TryInstruction();
	const auto* rhs = right.TryInstruction();
	if (lhs == nullptr || rhs == nullptr || lhs->GetOpcode() != rhs->GetOpcode() ||
	    lhs->NumArgs() != rhs->NumArgs()) {
		return false;
	}
	if (std::ranges::find(visited, std::pair {lhs, rhs}) != visited.end()) {
		return true;
	}
	visited.emplace_back(lhs, rhs);
	if (IsRuntimeRead(lhs->GetOpcode())) {
		const auto li = lhs->Flags<MemoryFlags>().index;
		const auto ri = rhs->Flags<MemoryFlags>().index;
		if (li >= program.memory_info.size() || ri >= program.memory_info.size() ||
		    program.memory_info[li] != program.memory_info[ri]) {
			return false;
		}
	} else if (lhs->Flags<uint64_t>() != rhs->Flags<uint64_t>()) {
		return false;
	}
	for (size_t index = 0; index < lhs->NumArgs(); index++) {
		if (lhs->GetOpcode() == ValueOpcode::Phi && lhs->PhiBlock(index) != rhs->PhiBlock(index)) {
			return false;
		}
		if (!EquivalentValue(program, lhs->Arg(index), rhs->Arg(index), visited)) {
			return false;
		}
	}
	return true;
}

} // namespace

ValueProgram::~ValueProgram() {
	// Values may cross block boundaries. Detach all arguments before any block starts destroying
	// its instruction storage so reverse-use links always point to live definitions.
	for (auto* block: blocks) {
		for (auto& inst: *block) {
			inst.Invalidate();
		}
	}
}

bool EquivalentValue(const ValueProgram& program, Value left, Value right) {
	std::vector<std::pair<const Inst*, const Inst*>> visited;
	return EquivalentValue(program, left, right, visited);
}

Value ResolveInvariantPhi(const ValueProgram& program, Value value) {
	value            = value.Resolve();
	const auto* root = value.TryInstruction();
	if (root == nullptr || root->GetOpcode() != ValueOpcode::Phi) {
		return value;
	}
	Value                           invariant;
	std::vector<Value>              pending {value};
	std::unordered_set<const Inst*> visited;
	while (!pending.empty()) {
		const auto current = pending.back().Resolve();
		pending.pop_back();
		const auto* inst = current.TryInstruction();
		if (inst != nullptr && inst->GetOpcode() == ValueOpcode::Phi) {
			if (!visited.insert(inst).second) {
				continue;
			}
			for (size_t index = 0; index < inst->NumArgs(); index++) {
				pending.push_back(inst->Arg(index));
			}
			continue;
		}
		if (invariant.IsEmpty()) {
			invariant = current;
		} else if (!EquivalentValue(program, invariant, current)) {
			return {};
		}
	}
	return invariant;
}

bool ValidateValueProgram(const ValueProgram& program, bool require_ssa, std::string* error) {
	if (program.blocks.size() != program.block_info.size() ||
	    program.blocks.size() != program.block_storage.size()) {
		return Fail(error, "value IR block storage is inconsistent");
	}
	for (size_t block_index = 0; block_index < program.blocks.size(); block_index++) {
		const auto* block = program.blocks[block_index];
		if (block != program.block_storage[block_index].get()) {
			return Fail(error, "value IR block pointer is inconsistent");
		}
		for (const auto& inst: *block) {
			if (inst.Parent() != block) {
				return Fail(error, "value IR instruction has the wrong parent block");
			}
			if (require_ssa && IsRegisterGet(inst.GetOpcode())) {
				return Fail(error, fmt::format("register getter {} survived SSA rewrite",
				                               ValueOpcodeName(inst.GetOpcode())));
			}
			if (inst.GetOpcode() != ValueOpcode::Phi && inst.GetOpcode() != ValueOpcode::Identity &&
			    inst.GetType() == Type::Opaque) {
				return Fail(error, fmt::format("untyped opcode {} survived translation",
				                               ValueOpcodeName(inst.GetOpcode())));
			}
			for (size_t arg_index = 0; arg_index < inst.NumArgs(); arg_index++) {
				const auto arg = inst.Arg(arg_index);
				if (arg.IsEmpty()) {
					return Fail(error, fmt::format("{} has an empty argument",
					                               ValueOpcodeName(inst.GetOpcode())));
				}
				if (const auto* definition = arg.TryInstruction();
				    definition != nullptr && definition->Parent() == nullptr) {
					return Fail(error, "value IR argument has a detached definition");
				}
				if (const auto* definition = arg.TryInstruction(); definition != nullptr) {
					const auto& uses = definition->Uses();
					const auto  use  = std::ranges::find_if(uses, [&](const Use& candidate) {
						return candidate.user == &inst && candidate.operand == arg_index;
					});
					if (use == uses.end()) {
						return Fail(error,
						            fmt::format("{} argument {} is absent from {} reverse uses",
						                        ValueOpcodeName(inst.GetOpcode()), arg_index,
						                        ValueOpcodeName(definition->GetOpcode())));
					}
				}
			}
		}
	}
	return true;
}

std::string ValueProgramToString(const ValueProgram& program) {
	std::map<const Inst*, size_t> ids;
	size_t                        next_id = 1;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			ids.emplace(&inst, next_id++);
		}
	}
	const auto value_string = [&](Value value) {
		if (value.IsEmpty()) {
			return std::string("<null>");
		}
		if (const auto* inst = value.TryInstruction(); inst != nullptr) {
			return fmt::format("%{}", ids.at(inst));
		}
		switch (value.GetType()) {
			case Type::ScalarReg: return fmt::format("s{}", RegIndex(value.ScalarRegister()));
			case Type::VectorReg: return fmt::format("v{}", RegIndex(value.VectorRegister()));
			case Type::U1: return std::string(value.U1() ? "true" : "false");
			case Type::U8: return fmt::format("{}u8", value.U8());
			case Type::U16: return fmt::format("{}u16", value.U16());
			case Type::U32: return fmt::format("0x{:08x}", value.U32());
			case Type::U64: return fmt::format("0x{:016x}", value.U64());
			case Type::F16: return fmt::format("f16(0x{:04x})", value.F16Bits());
			case Type::F32: return fmt::format("{}f", value.F32Value());
			default: return fmt::format("<{}>", TypeName(value.GetType()));
		}
	};

	std::string text =
	    fmt::format("mode={}\n", program.dispatcher_fallback ? "dispatcher" : "structured");
	for (size_t block_index = 0; block_index < program.blocks.size(); block_index++) {
		text += fmt::format("Block ${} pc=0x{:08x}..0x{:08x}\n", block_index,
		                    program.block_info[block_index].start_pc,
		                    program.block_info[block_index].end_pc);
		for (const auto& inst: *program.blocks[block_index]) {
			const auto type = inst.GetType();
			if (type != Type::Void) {
				text +=
				    fmt::format("  %{:<5} = {}", ids.at(&inst), ValueOpcodeName(inst.GetOpcode()));
			} else {
				text += fmt::format("          {}", ValueOpcodeName(inst.GetOpcode()));
			}
			for (size_t index = 0; index < inst.NumArgs(); index++) {
				text += index == 0 ? " " : ", ";
				if (inst.GetOpcode() == ValueOpcode::Phi) {
					const auto predecessor =
					    std::ranges::find(program.blocks, inst.PhiBlock(index));
					text += fmt::format("[{}, ${}]", value_string(inst.Arg(index)),
					                    std::distance(program.blocks.begin(), predecessor));
				} else {
					text += value_string(inst.Arg(index));
				}
			}
			text +=
			    fmt::format(" ({}; uses={})\n", TypeName(Value(const_cast<Inst*>(&inst)).GetType()),
			                inst.UseCount());
		}
	}
	return text;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
