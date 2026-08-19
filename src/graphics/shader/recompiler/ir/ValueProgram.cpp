#include "graphics/shader/recompiler/ir/ValueProgram.h"

#include "graphics/shader/shader.h"

#include <fmt/format.h>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {

using ShaderError::Fail;

namespace {

bool IsRegisterStatePseudo(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetThreadBitScalarRegister:
		case ValueOpcode::SetThreadBitScalarRegister:
		case ValueOpcode::GetScalarRegister:
		case ValueOpcode::SetScalarRegister:
		case ValueOpcode::GetVectorRegister:
		case ValueOpcode::SetVectorRegister:
		case ValueOpcode::GetGotoVariable:
		case ValueOpcode::SetGotoVariable:
		case ValueOpcode::GetScc:
		case ValueOpcode::SetScc:
		case ValueOpcode::GetExec:
		case ValueOpcode::SetExec:
		case ValueOpcode::GetExecLo:
		case ValueOpcode::SetExecLo:
		case ValueOpcode::GetExecHi:
		case ValueOpcode::SetExecHi:
		case ValueOpcode::GetVcc:
		case ValueOpcode::SetVcc:
		case ValueOpcode::GetVccLo:
		case ValueOpcode::SetVccLo:
		case ValueOpcode::GetVccHi:
		case ValueOpcode::SetVccHi:
		case ValueOpcode::GetM0:
		case ValueOpcode::SetM0: return true;
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
	std::unordered_set<const Block*>           blocks;
	std::unordered_map<uint32_t, const Block*> blocks_by_id;
	std::unordered_set<const Inst*>            instructions;
	for (size_t block_index = 0; block_index < program.blocks.size(); block_index++) {
		const auto* block = program.blocks[block_index];
		if (block == nullptr || program.block_storage[block_index] == nullptr ||
		    block != program.block_storage[block_index].get()) {
			return Fail(error, "value IR block pointer is inconsistent");
		}
		if (!blocks.insert(block).second) {
			return Fail(error, "value IR block pointer is duplicated");
		}
		if (!blocks_by_id.emplace(program.block_info[block_index].id, block).second) {
			return Fail(error, "value IR block id is duplicated");
		}
		for (const auto& inst: *block) {
			if (!instructions.insert(&inst).second) {
				return Fail(error, "value IR instruction is duplicated");
			}
		}
	}

	for (size_t block_index = 0; block_index < program.blocks.size(); block_index++) {
		const auto*                      block = program.blocks[block_index];
		std::unordered_set<const Block*> predecessors;
		for (const auto* predecessor: block->ImmPredecessors()) {
			if (predecessor == nullptr || !blocks.contains(predecessor)) {
				return Fail(error, "value IR block has a foreign predecessor");
			}
			if (!predecessors.insert(predecessor).second) {
				return Fail(error, "value IR block predecessor is duplicated");
			}
			if (std::ranges::find(predecessor->ImmSuccessors(), block) ==
			    predecessor->ImmSuccessors().end()) {
				return Fail(error, "value IR predecessor edge is not reciprocal");
			}
		}

		std::unordered_set<const Block*> successors;
		for (const auto* successor: block->ImmSuccessors()) {
			if (successor == nullptr || !blocks.contains(successor)) {
				return Fail(error, "value IR block has a foreign successor");
			}
			if (!successors.insert(successor).second) {
				return Fail(error, "value IR block successor is duplicated");
			}
			if (std::ranges::find(successor->ImmPredecessors(), block) ==
			    successor->ImmPredecessors().end()) {
				return Fail(error, "value IR successor edge is not reciprocal");
			}
		}

		std::unordered_set<const Block*> expected_successors;
		const auto                       add_target = [&](uint32_t id) {
			const auto found = blocks_by_id.find(id);
			if (found == blocks_by_id.end()) {
				return false;
			}
			expected_successors.insert(found->second);
			return true;
		};
		const auto& terminator             = program.block_info[block_index].terminator;
		const auto  validate_control_value = [&](Value value, Type type) {
			if (value.IsEmpty() || value.GetType() != type) {
				return false;
			}
			const auto* definition = value.TryInstruction();
			return definition == nullptr || instructions.contains(definition);
		};
		switch (terminator.kind) {
			case CFG::TerminatorKind::Branch:
				if (!add_target(terminator.true_block)) {
					return Fail(error, "value IR branch target is missing");
				}
				break;
			case CFG::TerminatorKind::ConditionalBranch:
				if (!add_target(terminator.true_block) || !add_target(terminator.false_block)) {
					return Fail(error, "value IR conditional branch target is missing");
				}
				if (!validate_control_value(program.block_info[block_index].condition, Type::U1)) {
					return Fail(error, "value IR conditional branch condition is invalid");
				}
				break;
			case CFG::TerminatorKind::IndirectBranch:
				if (!validate_control_value(program.block_info[block_index].indirect_target,
				                            Type::U32)) {
					return Fail(error, "value IR indirect branch selector is invalid");
				}
				for (const auto target: terminator.indirect_targets) {
					if (!add_target(target)) {
						return Fail(error, "value IR indirect branch target is missing");
					}
				}
				if (terminator.indirect_selector_values.size() !=
				    terminator.indirect_selector_targets.size()) {
					return Fail(error, "value IR indirect selector table is inconsistent");
				}
				for (const auto target: terminator.indirect_selector_targets) {
					const auto found = blocks_by_id.find(target);
					if (found == blocks_by_id.end() ||
					    !expected_successors.contains(found->second)) {
						return Fail(error,
						            "value IR indirect selector target is not a CFG successor");
					}
				}
				break;
			case CFG::TerminatorKind::Return:
			case CFG::TerminatorKind::Unsupported: break;
		}
		if ((terminator.merge_block != UINT32_MAX &&
		     !blocks_by_id.contains(terminator.merge_block)) ||
		    (terminator.continue_block != UINT32_MAX &&
		     !blocks_by_id.contains(terminator.continue_block))) {
			return Fail(error, "value IR structured control target is missing");
		}
		if (successors != expected_successors) {
			return Fail(error, "value IR terminator and successor graph disagree");
		}

		bool saw_non_phi = false;
		for (const auto& inst: *block) {
			if (inst.Parent() != block) {
				return Fail(error, "value IR instruction has the wrong parent block");
			}
			if (inst.GetOpcode() == ValueOpcode::Phi) {
				if (saw_non_phi) {
					return Fail(error, "value IR Phi appears after a non-Phi instruction");
				}
				if (inst.NumPhiBlocks() != inst.NumArgs()) {
					return Fail(error, "value IR Phi parent table is inconsistent");
				}
				if (inst.NumArgs() == 0) {
					return Fail(error, "value IR Phi has no incoming values");
				}
				std::unordered_set<const Block*> incoming_blocks;
				for (size_t arg_index = 0; arg_index < inst.NumArgs(); arg_index++) {
					const auto* predecessor = inst.PhiBlock(arg_index);
					if (predecessor == nullptr || !blocks.contains(predecessor) ||
					    !predecessors.contains(predecessor)) {
						return Fail(error, "value IR Phi has a foreign or non-predecessor parent");
					}
					if (!incoming_blocks.insert(predecessor).second) {
						return Fail(error, "value IR Phi parent is duplicated");
					}
					if (inst.Arg(arg_index).GetType() != inst.GetType()) {
						return Fail(error, "value IR Phi incoming type does not match its result");
					}
				}
				if (incoming_blocks != predecessors) {
					return Fail(error, "value IR Phi does not cover every predecessor");
				}
			} else {
				saw_non_phi = true;
			}
			if (require_ssa && IsRegisterStatePseudo(inst.GetOpcode())) {
				return Fail(error, fmt::format("register-state pseudo {} survived SSA rewrite",
				                               ValueOpcodeName(inst.GetOpcode())));
			}
			if (inst.GetOpcode() != ValueOpcode::Phi && inst.GetOpcode() != ValueOpcode::Identity &&
			    inst.GetType() == Type::Opaque) {
				return Fail(error, fmt::format("untyped opcode {} survived translation",
				                               ValueOpcodeName(inst.GetOpcode())));
			}
			const bool fixed_signature =
			    inst.GetOpcode() != ValueOpcode::Phi && inst.GetOpcode() != ValueOpcode::Identity;
			if (fixed_signature && inst.NumArgs() != NumArgsOf(inst.GetOpcode())) {
				return Fail(error, fmt::format("{} has {} arguments, expected {}",
				                               ValueOpcodeName(inst.GetOpcode()), inst.NumArgs(),
				                               NumArgsOf(inst.GetOpcode())));
			}
			const auto buffer_components = BufferComponentCount(inst.GetOpcode());
			if (buffer_components != 0u) {
				const auto memory_index = inst.Flags<MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					return Fail(error, fmt::format("{} has an invalid memory-info index",
					                               ValueOpcodeName(inst.GetOpcode())));
				}
				const auto& memory = program.memory_info[memory_index];
				if (memory.kind != ResourceKind::Buffer &&
				    memory.kind != ResourceKind::ScalarBuffer) {
					return Fail(error, fmt::format("{} has a non-buffer resource kind",
					                               ValueOpcodeName(inst.GetOpcode())));
				}
				if (buffer_components > 1u &&
				    (memory.kind != ResourceKind::Buffer || memory.data_bits != 32u ||
				     memory.data_dwords != buffer_components || memory.component_index != 0u)) {
					return Fail(error, fmt::format("{} has inconsistent native-wide metadata",
					                               ValueOpcodeName(inst.GetOpcode())));
				}
				if (buffer_components == 1u &&
				    (inst.GetOpcode() == ValueOpcode::LoadBufferU32 ||
				     inst.GetOpcode() == ValueOpcode::StoreBufferU32) &&
				    memory.data_dwords != 1u) {
					return Fail(error, fmt::format("{} retains scalar-sibling width metadata",
					                               ValueOpcodeName(inst.GetOpcode())));
				}
			}
			const auto shared_components = SharedComponentCount(inst.GetOpcode());
			if (shared_components != 0u) {
				const auto memory_index = inst.Flags<MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					return Fail(error, fmt::format("{} has an invalid memory-info index",
					                               ValueOpcodeName(inst.GetOpcode())));
				}
				const auto& memory = program.memory_info[memory_index];
				if ((memory.kind != ResourceKind::Lds && memory.kind != ResourceKind::Gds) ||
				    memory.resource != 0u || memory.sampler != 0u || memory.component_count == 0u ||
				    memory.component_index >= memory.component_count) {
					return Fail(error, fmt::format("{} has invalid shared-memory metadata",
					                               ValueOpcodeName(inst.GetOpcode())));
				}
				uint32_t expected_bits = 32u;
				if (inst.GetOpcode() == ValueOpcode::LoadSharedU8 ||
				    inst.GetOpcode() == ValueOpcode::WriteSharedU8) {
					expected_bits = 8u;
				} else if (inst.GetOpcode() == ValueOpcode::LoadSharedU16 ||
				           inst.GetOpcode() == ValueOpcode::WriteSharedU16) {
					expected_bits = 16u;
				}
				if (memory.data_bits != expected_bits || memory.data_dwords != shared_components ||
				    (shared_components > 1u && memory.component_index != 0u)) {
					return Fail(error, fmt::format("{} has inconsistent shared-memory width",
					                               ValueOpcodeName(inst.GetOpcode())));
				}
			}
			uint32_t composite_components = 0u;
			switch (inst.GetOpcode()) {
				case ValueOpcode::CompositeExtractU32x2: composite_components = 2u; break;
				case ValueOpcode::CompositeExtractU32x3: composite_components = 3u; break;
				case ValueOpcode::CompositeExtractU32x4: composite_components = 4u; break;
				default: break;
			}
			if (composite_components != 0u &&
			    (!inst.Arg(1).IsImmediate() || inst.Arg(1).GetType() != Type::U32 ||
			     inst.Arg(1).U32() >= composite_components)) {
				return Fail(error, fmt::format("{} has an invalid component index",
				                               ValueOpcodeName(inst.GetOpcode())));
			}
			for (size_t arg_index = 0; arg_index < inst.NumArgs(); arg_index++) {
				const auto arg = inst.Arg(arg_index);
				if (arg.IsEmpty()) {
					return Fail(error, fmt::format("{} has an empty argument",
					                               ValueOpcodeName(inst.GetOpcode())));
				}
				if (fixed_signature && arg.GetType() != ArgTypeOf(inst.GetOpcode(), arg_index)) {
					return Fail(error,
					            fmt::format("{} argument {} has type {}, expected {}",
					                        ValueOpcodeName(inst.GetOpcode()), arg_index,
					                        TypeName(arg.GetType()),
					                        TypeName(ArgTypeOf(inst.GetOpcode(), arg_index))));
				}
				if (const auto* definition = arg.TryInstruction();
				    definition != nullptr && !instructions.contains(definition)) {
					return Fail(error, "value IR argument has a foreign definition");
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
