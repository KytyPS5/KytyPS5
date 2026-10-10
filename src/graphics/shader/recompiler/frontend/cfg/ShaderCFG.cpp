#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"

#include "common/assert.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fmt/format.h>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::CFG {
namespace {

using Decoder::Instruction;
using Decoder::Opcode;

struct SetpcTargetInfo {
	uint32_t              target        = 0;
	bool                  indirect      = false;
	uint32_t              pc_sgpr       = UINT32_MAX;
	uint32_t              selector_code = UINT32_MAX;
	uint32_t              table_load_pc = UINT32_MAX;
	std::vector<uint32_t> target_pcs;
	std::vector<uint32_t> selector_values;
	std::vector<uint32_t> selector_target_pcs;
};

void SetFailure(Graph& graph, FailureKind kind, uint32_t block_id, const std::string& message) {
	graph.unsupported        = true;
	graph.failure_kind       = kind;
	graph.failure_block      = block_id;
	graph.unsupported_reason = message;
}

[[noreturn]] void ExitBuildFailure(Graph& graph, FailureKind kind, uint32_t block_id,
                                   const std::string& message) {
	SetFailure(graph, kind, block_id, message);
	EXIT("shader CFG build failed: %s", message.c_str());
	std::abort();
}

uint32_t InstructionEndPc(const Instruction& inst) {
	return inst.pc + inst.word_count * 4u;
}

uint32_t ProgramEndPc(const Decoder::Program& program) {
	if (program.instructions.empty()) {
		return 0;
	}
	return InstructionEndPc(program.instructions.back());
}

BranchCondition ConditionForOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_BRANCH: return BranchCondition::Always;
		case Opcode::S_CBRANCH_SCC0: return BranchCondition::SccZero;
		case Opcode::S_CBRANCH_SCC1: return BranchCondition::SccNonZero;
		case Opcode::S_CBRANCH_VCCZ: return BranchCondition::VccZero;
		case Opcode::S_CBRANCH_VCCNZ: return BranchCondition::VccNonZero;
		case Opcode::S_CBRANCH_EXECZ: return BranchCondition::ExecZero;
		case Opcode::S_CBRANCH_EXECNZ: return BranchCondition::ExecNonZero;
		case Opcode::S_SUBVECTOR_LOOP_BEGIN:
		case Opcode::S_SUBVECTOR_LOOP_END: return BranchCondition::ScalarInstruction;
		default: return BranchCondition::Unknown;
	}
}

bool IsRegister(const Decoder::Operand& operand, Decoder::OperandKind kind, uint32_t reg) {
	return operand.kind == kind && operand.reg == reg;
}

bool ScalarOperandCode(const Decoder::Operand& operand, uint32_t& code) {
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: code = operand.reg; return true;
		case Decoder::OperandKind::VccLo: code = 106u; return true;
		case Decoder::OperandKind::VccHi: code = 107u; return true;
		case Decoder::OperandKind::M0: code = 124u; return true;
		case Decoder::OperandKind::ExecLo: code = 126u; return true;
		case Decoder::OperandKind::ExecHi: code = 127u; return true;
		default: return false;
	}
}

bool IsScalarCode(const Decoder::Operand& operand, uint32_t code) {
	uint32_t operand_code = 0;
	return ScalarOperandCode(operand, operand_code) && operand_code == code;
}

bool IsImmediate(const Decoder::Operand& operand, uint32_t& value) {
	if (operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::LiteralConstant ||
	    operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		value = operand.value;
		return true;
	}
	return false;
}

bool IsImmediateSigned(const Decoder::Operand& operand, int32_t& value) {
	if (operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::LiteralConstant) {
		value = operand.signed_val;
		return true;
	}
	if (operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		value = static_cast<int32_t>(operand.value);
		return true;
	}
	return false;
}

bool OtherScalarSource(const Instruction& inst, uint32_t known_code,
                       const Decoder::Operand** other) {
	if (other == nullptr) {
		return false;
	}
	if (IsScalarCode(inst.src0, known_code)) {
		*other = &inst.src1;
		return true;
	}
	if (IsScalarCode(inst.src1, known_code)) {
		*other = &inst.src0;
		return true;
	}
	return false;
}

bool InstructionWritesScalarCode(const Instruction& inst, uint32_t code) {
	uint32_t dst_code = 0;
	if (!ScalarOperandCode(inst.dst, dst_code)) {
		return false;
	}
	const uint32_t count = std::max<uint32_t>(1u, inst.data_dwords);
	return code >= dst_code && code < dst_code + count;
}

bool ScalarCodeWrittenInRange(const Decoder::Program& program, uint32_t begin_index,
                              uint32_t end_index, uint32_t code);

bool FindPreviousGetpc(const Decoder::Program& program, uint32_t before_index, uint32_t dst_code,
                       uint32_t* index) {
	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t candidate_index = i - 1u;
		const auto&    candidate       = program.instructions[candidate_index];
		if (candidate.opcode == Opcode::S_GETPC_B64 && IsScalarCode(candidate.dst, dst_code)) {
			if (index != nullptr) {
				*index = candidate_index;
			}
			return true;
		}
		if (InstructionWritesScalarCode(candidate, dst_code)) {
			return false;
		}
	}
	return false;
}

bool ResolvePcRelativeBase(const Decoder::Program& program, uint32_t before_index,
                           uint32_t base_code, uint32_t* pc) {
	if (pc == nullptr) {
		return false;
	}
	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t candidate_index = i - 1u;
		const auto&    candidate       = program.instructions[candidate_index];
		if (!InstructionWritesScalarCode(candidate, base_code)) {
			continue;
		}
		const bool adds =
		    candidate.opcode == Opcode::S_ADD_U32 || candidate.opcode == Opcode::S_ADD_I32;
		const bool subs =
		    candidate.opcode == Opcode::S_SUB_U32 || candidate.opcode == Opcode::S_SUB_I32;
		if (!adds && !subs) {
			return false;
		}

		const Decoder::Operand* offset      = nullptr;
		int32_t                 imm         = 0;
		uint32_t                getpc_index = 0;
		if (adds) {
			if (!OtherScalarSource(candidate, base_code, &offset)) {
				return false;
			}
		} else {
			if (!IsScalarCode(candidate.src0, base_code)) {
				return false;
			}
			offset = &candidate.src1;
		}
		if (!IsImmediateSigned(*offset, imm) ||
		    !FindPreviousGetpc(program, candidate_index, base_code, &getpc_index)) {
			return false;
		}

		if (candidate_index + 1u >= before_index) {
			return false;
		}
		const auto&             high       = program.instructions[candidate_index + 1u];
		const Decoder::Operand* high_other = nullptr;
		uint32_t                zero       = 0;
		if (high.opcode != (adds ? Opcode::S_ADDC_U32 : Opcode::S_SUBB_U32) ||
		    !IsScalarCode(high.dst, base_code + 1u)) {
			return false;
		}
		if (adds) {
			if (!OtherScalarSource(high, base_code + 1u, &high_other)) {
				return false;
			}
		} else {
			if (!IsScalarCode(high.src0, base_code + 1u)) {
				return false;
			}
			high_other = &high.src1;
		}
		if (!IsImmediate(*high_other, zero) || zero != 0u ||
		    ScalarCodeWrittenInRange(program, candidate_index + 2u, before_index, base_code + 1u)) {
			return false;
		}

		const auto base = InstructionEndPc(program.instructions[getpc_index]);
		*pc = adds ? base + static_cast<uint32_t>(imm) : base - static_cast<uint32_t>(imm);
		*pc &= ~3u;
		return true;
	}
	return false;
}

bool AddSignedByteOffset(uint32_t base, uint32_t encoded_offset, uint32_t* address) {
	const auto value = static_cast<int64_t>(base) + static_cast<int32_t>(encoded_offset);
	if (address == nullptr || value < 0 || value > UINT32_MAX) {
		return false;
	}
	*address = static_cast<uint32_t>(value);
	return true;
}

bool FindPreviousScalarLoad(const Decoder::Program& program, uint32_t before_index,
                            uint32_t dst_code, Opcode opcode, uint32_t* index) {
	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t candidate_index = i - 1u;
		const auto&    candidate       = program.instructions[candidate_index];
		if (InstructionWritesScalarCode(candidate, dst_code)) {
			if (candidate.opcode == opcode && IsScalarCode(candidate.dst, dst_code)) {
				if (index != nullptr) {
					*index = candidate_index;
				}
				return true;
			}
			return false;
		}
	}
	return false;
}

bool ResolveJumpTableEntryCount(const Decoder::Program& program, uint32_t before_index,
                                const Decoder::Operand& byte_offset_operand, uint32_t stride_shift,
                                uint32_t* entry_count) {
	if (entry_count == nullptr) {
		return false;
	}
	uint32_t byte_offset_code = 0;
	if (!ScalarOperandCode(byte_offset_operand, byte_offset_code)) {
		return false;
	}

	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t shift_index = i - 1u;
		const auto&    shift       = program.instructions[shift_index];
		if (!InstructionWritesScalarCode(shift, byte_offset_code)) {
			continue;
		}
		if (shift.opcode != Opcode::S_LSHL_B32 || !IsScalarCode(shift.dst, byte_offset_code)) {
			return false;
		}

		const Decoder::Operand* shift_amount_operand = nullptr;
		uint32_t                shift_amount         = 0;
		if (!OtherScalarSource(shift, byte_offset_code, &shift_amount_operand) ||
		    !IsImmediate(*shift_amount_operand, shift_amount) || shift_amount != stride_shift) {
			return false;
		}

		uint32_t index_code = byte_offset_code;

		for (uint32_t j = shift_index; j > 0; j--) {
			const uint32_t clamp_index = j - 1u;
			const auto&    clamp       = program.instructions[clamp_index];
			if (!InstructionWritesScalarCode(clamp, index_code)) {
				continue;
			}
			if (clamp.opcode != Opcode::S_MIN_U32 || !IsScalarCode(clamp.dst, index_code)) {
				return false;
			}

			uint32_t max_index = 0;
			if (!IsImmediate(clamp.src0, max_index) && !IsImmediate(clamp.src1, max_index)) {
				return false;
			}
			*entry_count = max_index + 1u;
			return *entry_count != 0;
		}
		return false;
	}
	return false;
}

void AddUnique(std::vector<uint32_t>& values, uint32_t value) {
	if (std::find(values.begin(), values.end(), value) == values.end()) {
		values.push_back(value);
	}
}

bool ScalarCodeWrittenInRange(const Decoder::Program& program, uint32_t begin_index,
                              uint32_t end_index, uint32_t code) {
	const auto end =
	    std::min<uint32_t>(end_index, static_cast<uint32_t>(program.instructions.size()));
	for (uint32_t i = begin_index; i < end; i++) {
		if (InstructionWritesScalarCode(program.instructions[i], code)) {
			return true;
		}
	}
	return false;
}

bool ResolveSetpcJumpTable(const Decoder::Program& program, uint32_t setpc_index,
                           SetpcTargetInfo& info) {
	if (setpc_index < 4u || setpc_index >= program.instructions.size()) {
		return false;
	}

	const auto& setpc  = program.instructions[setpc_index];
	uint32_t    pc_reg = 0;
	if (setpc.opcode != Opcode::S_SETPC_B64 || !ScalarOperandCode(setpc.src0, pc_reg) ||
	    setpc.src0.kind != Decoder::OperandKind::Sgpr) {
		return false;
	}

	const auto& getpc = program.instructions[setpc_index - 3u];
	const auto& add   = program.instructions[setpc_index - 2u];
	const auto& addc  = program.instructions[setpc_index - 1u];
	if (getpc.opcode != Opcode::S_GETPC_B64 || !IsScalarCode(getpc.dst, pc_reg) ||
	    add.opcode != Opcode::S_ADD_U32 || !IsScalarCode(add.dst, pc_reg) ||
	    addc.opcode != Opcode::S_ADDC_U32 || !IsScalarCode(addc.dst, pc_reg + 1u)) {
		return false;
	}

	const Decoder::Operand* offset_low_operand  = nullptr;
	const Decoder::Operand* offset_high_operand = nullptr;
	uint32_t                offset_low_code     = 0;
	uint32_t                offset_high_code    = 0;
	if (!OtherScalarSource(add, pc_reg, &offset_low_operand) ||
	    !OtherScalarSource(addc, pc_reg + 1u, &offset_high_operand) ||
	    !ScalarOperandCode(*offset_low_operand, offset_low_code) ||
	    !ScalarOperandCode(*offset_high_operand, offset_high_code) ||
	    offset_high_code != offset_low_code + 1u) {
		return false;
	}

	uint32_t load_index = 0;
	if (!FindPreviousScalarLoad(program, setpc_index - 3u, offset_low_code, Opcode::S_LOAD_DWORDX2,
	                            &load_index)) {
		return false;
	}
	const auto& load            = program.instructions[load_index];
	uint32_t    table_base_code = 0;
	if (!ScalarOperandCode(load.src0, table_base_code)) {
		return false;
	}

	uint32_t table_pc = 0;
	if (!ResolvePcRelativeBase(program, load_index, table_base_code, &table_pc) ||
	    !AddSignedByteOffset(table_pc, load.offset, &table_pc)) {
		return false;
	}

	uint32_t entry_count = 0;
	if (!ResolveJumpTableEntryCount(program, load_index, load.src1, 3u, &entry_count)) {
		return false;
	}
	uint32_t selector_code = UINT32_MAX;
	if (!ScalarOperandCode(load.src1, selector_code) ||
	    ScalarCodeWrittenInRange(program, load_index + 1u, setpc_index, selector_code)) {
		return false;
	}

	const uint32_t target_base = InstructionEndPc(getpc);
	const size_t   table_word  = table_pc / 4u;
	const size_t   table_words = static_cast<size_t>(entry_count) * 2u;
	if ((table_pc & 3u) != 0 || table_word > program.code.size() ||
	    table_words > program.code.size() - table_word) {
		return false;
	}

	std::vector<uint32_t> targets;
	std::vector<uint32_t> selector_values;
	std::vector<uint32_t> selector_target_pcs;
	for (uint32_t i = 0; i < entry_count; i++) {
		const uint32_t low  = program.code[table_word + i * 2u];
		const uint32_t high = program.code[table_word + i * 2u + 1u];
		if (high != 0u && high != UINT32_MAX) {
			return false;
		}
		const auto target_pc = (target_base + low) & ~3u;
		AddUnique(targets, target_pc);
		selector_values.push_back(i * 8u);
		selector_target_pcs.push_back(target_pc);
	}
	if (targets.empty()) {
		return false;
	}

	info.indirect            = true;
	info.pc_sgpr             = pc_reg;
	info.selector_code       = selector_code;
	info.table_load_pc       = load.pc;
	info.target_pcs          = std::move(targets);
	info.selector_values     = std::move(selector_values);
	info.selector_target_pcs = std::move(selector_target_pcs);
	return true;
}

bool ResolveSetpcDwordJumpTable(const Decoder::Program& program, uint32_t setpc_index,
                                SetpcTargetInfo& info) {
	if (setpc_index < 3u || setpc_index >= program.instructions.size()) {
		return false;
	}

	const auto& setpc  = program.instructions[setpc_index];
	uint32_t    pc_reg = 0;
	if (setpc.opcode != Opcode::S_SETPC_B64 || setpc.src0.kind != Decoder::OperandKind::Sgpr ||
	    !ScalarOperandCode(setpc.src0, pc_reg)) {
		return false;
	}
	const auto& low_sub     = program.instructions[setpc_index - 2u];
	const auto& high_sub    = program.instructions[setpc_index - 1u];
	uint32_t    offset_code = 0;
	uint32_t    zero        = 0;
	if (low_sub.opcode != Opcode::S_SUB_U32 || !IsScalarCode(low_sub.dst, pc_reg) ||
	    !IsScalarCode(low_sub.src0, pc_reg) || !ScalarOperandCode(low_sub.src1, offset_code) ||
	    high_sub.opcode != Opcode::S_SUBB_U32 || !IsScalarCode(high_sub.dst, pc_reg + 1u) ||
	    !IsScalarCode(high_sub.src0, pc_reg + 1u) || !IsImmediate(high_sub.src1, zero) ||
	    zero != 0u || offset_code == pc_reg || offset_code == pc_reg + 1u) {
		return false;
	}

	uint32_t load_index = 0;
	if (!FindPreviousScalarLoad(program, setpc_index - 2u, offset_code, Opcode::S_LOAD_DWORD,
	                            &load_index)) {
		return false;
	}
	const auto& load = program.instructions[load_index];
	if (!IsScalarCode(load.src0, pc_reg)) {
		return false;
	}

	uint32_t target_base = 0;
	if (!ResolvePcRelativeBase(program, load_index, pc_reg, &target_base)) {
		return false;
	}
	uint32_t table_pc = 0;
	if (!AddSignedByteOffset(target_base, load.offset, &table_pc)) {
		return false;
	}

	uint32_t entry_count = 0;
	if (!ResolveJumpTableEntryCount(program, load_index, load.src1, 2u, &entry_count)) {
		return false;
	}
	uint32_t selector_code = UINT32_MAX;
	if (!ScalarOperandCode(load.src1, selector_code) || selector_code == offset_code ||
	    ScalarCodeWrittenInRange(program, load_index + 1u, setpc_index, selector_code)) {
		return false;
	}

	const size_t table_word = table_pc / 4u;
	if ((table_pc & 3u) != 0 || table_word > program.code.size() ||
	    entry_count > program.code.size() - table_word) {
		return false;
	}

	std::vector<uint32_t> targets;
	std::vector<uint32_t> selector_values;
	std::vector<uint32_t> selector_target_pcs;
	for (uint32_t i = 0; i < entry_count; i++) {
		const uint32_t offset = program.code[table_word + i];
		if (offset > target_base) {
			return false;
		}
		const auto target_pc = (target_base - offset) & ~3u;
		AddUnique(targets, target_pc);
		selector_values.push_back(i * 4u);
		selector_target_pcs.push_back(target_pc);
	}
	if (targets.empty()) {
		return false;
	}

	info.indirect            = true;
	info.pc_sgpr             = pc_reg;
	info.selector_code       = selector_code;
	info.table_load_pc       = load.pc;
	info.target_pcs          = std::move(targets);
	info.selector_values     = std::move(selector_values);
	info.selector_target_pcs = std::move(selector_target_pcs);
	return true;
}

bool ResolveSetpcTarget(const Decoder::Program& program, uint32_t setpc_index, uint32_t& target) {
	if (setpc_index >= program.instructions.size()) {
		return false;
	}

	const auto& setpc = program.instructions[setpc_index];
	if (setpc.opcode != Opcode::S_SETPC_B64 || setpc.src0.kind != Decoder::OperandKind::Sgpr) {
		return false;
	}

	const auto pc_reg = setpc.src0.reg;
	if (setpc_index >= 2u) {
		const auto& arith = program.instructions[setpc_index - 1u];
		const auto& getpc = program.instructions[setpc_index - 2u];
		if (getpc.opcode == Opcode::S_GETPC_B64 && getpc.dst.kind == Decoder::OperandKind::Sgpr &&
		    getpc.dst.reg == pc_reg && arith.dst.kind == Decoder::OperandKind::Sgpr &&
		    arith.dst.reg == pc_reg) {
			uint32_t imm  = 0;
			bool     adds = arith.opcode == Opcode::S_ADD_U32 || arith.opcode == Opcode::S_ADD_I32;
			bool     subs = arith.opcode == Opcode::S_SUB_U32 || arith.opcode == Opcode::S_SUB_I32;
			if ((adds || subs) &&
			    (IsRegister(arith.src0, Decoder::OperandKind::Sgpr, pc_reg) ||
			     IsRegister(arith.src1, Decoder::OperandKind::Sgpr, pc_reg)) &&
			    (IsImmediate(arith.src0, imm) || IsImmediate(arith.src1, imm))) {
				const auto base = InstructionEndPc(getpc);
				target          = (adds ? base + imm : base - imm) & ~3u;
				return true;
			}
		}
	}

	if (setpc_index >= 1u) {
		const auto& getpc = program.instructions[setpc_index - 1u];
		if (getpc.opcode == Opcode::S_GETPC_B64 && getpc.dst.kind == Decoder::OperandKind::Sgpr &&
		    getpc.dst.reg == pc_reg) {
			target = InstructionEndPc(getpc);
			return true;
		}
	}

	return false;
}

bool ResolveSetpcTargets(const Decoder::Program& program, uint32_t setpc_index,
                         SetpcTargetInfo& info) {
	info = {};
	if (ResolveSetpcDwordJumpTable(program, setpc_index, info)) {
		return true;
	}
	if (ResolveSetpcJumpTable(program, setpc_index, info)) {
		return true;
	}
	uint32_t target = 0;
	if (ResolveSetpcTarget(program, setpc_index, target)) {
		info.target = target;
		return true;
	}
	return false;
}

bool IsValidTarget(uint32_t target, std::span<const Instruction> instructions, uint32_t end_pc) {
	return target == end_pc ||
	       std::ranges::binary_search(instructions, target, {}, &Instruction::pc);
}

std::vector<uint32_t> AllBlockIds(uint32_t count) {
	std::vector<uint32_t> ids;
	ids.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		ids.push_back(i);
	}
	return ids;
}

std::vector<uint32_t> IntersectSorted(const std::vector<uint32_t>& a,
                                      const std::vector<uint32_t>& b) {
	std::vector<uint32_t> ret;
	ret.reserve(std::min(a.size(), b.size()));
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(ret));
	return ret;
}

void SortUnique(std::vector<uint32_t>& values) {
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool Contains(const std::vector<uint32_t>& values, uint32_t value) {
	return std::find(values.begin(), values.end(), value) != values.end();
}

uint32_t RemapId(uint32_t id, const std::vector<uint32_t>& id_map) {
	return id != UINT32_MAX && id < id_map.size() ? id_map[id] : id;
}

void RemapIds(std::vector<uint32_t>& values, const std::vector<uint32_t>& id_map) {
	for (auto& value: values) {
		value = RemapId(value, id_map);
	}
	SortUnique(values);
}

void RebuildPredecessors(Graph& graph) {
	for (auto& block: graph.blocks) {
		block.predecessors.clear();
		SortUnique(block.successors);
	}
	for (const auto& block: graph.blocks) {
		for (auto succ: block.successors) {
			if (succ < graph.blocks.size()) {
				AddUnique(graph.blocks[succ].predecessors, block.id);
			}
		}
	}
	for (auto& block: graph.blocks) {
		SortUnique(block.predecessors);
	}
}

void PruneUnreachableBlocks(Graph& graph) {
	if (graph.entry_block >= graph.blocks.size()) {
		return;
	}

	std::vector<bool>     reachable(graph.blocks.size(), false);
	std::vector<uint32_t> pending {graph.entry_block};
	while (!pending.empty()) {
		const auto block_id = pending.back();
		pending.pop_back();
		if (block_id >= graph.blocks.size() || reachable[block_id]) {
			continue;
		}
		reachable[block_id] = true;
		for (const auto successor: graph.blocks[block_id].successors) {
			pending.push_back(successor);
		}
	}
	if (std::ranges::all_of(reachable, [](bool value) { return value; })) {
		return;
	}

	std::vector<uint32_t>   id_map(graph.blocks.size(), UINT32_MAX);
	std::vector<BasicBlock> blocks;
	blocks.reserve(std::ranges::count(reachable, true));
	for (uint32_t old_id = 0; old_id < graph.blocks.size(); old_id++) {
		if (!reachable[old_id]) {
			continue;
		}
		id_map[old_id] = static_cast<uint32_t>(blocks.size());
		blocks.push_back(std::move(graph.blocks[old_id]));
	}

	graph.entry_block   = RemapId(graph.entry_block, id_map);
	graph.failure_block = RemapId(graph.failure_block, id_map);
	graph.blocks        = std::move(blocks);
	for (auto& block: graph.blocks) {
		block.id = RemapId(block.id, id_map);
		RemapIds(block.successors, id_map);
		block.predecessors.clear();
		block.dominators.clear();
		auto& terminator          = block.terminator;
		terminator.true_block     = RemapId(terminator.true_block, id_map);
		terminator.false_block    = RemapId(terminator.false_block, id_map);
		terminator.merge_block    = RemapId(terminator.merge_block, id_map);
		terminator.continue_block = RemapId(terminator.continue_block, id_map);
		for (auto& target: terminator.indirect_targets) {
			target = RemapId(target, id_map);
		}
		for (auto& target: terminator.indirect_selector_targets) {
			target = RemapId(target, id_map);
		}
	}
	graph.back_edges.clear();
	graph.natural_loops.clear();
	graph.components.clear();
	RebuildPredecessors(graph);
}

void ComputeDominators(Graph& graph) {
	const auto count = static_cast<uint32_t>(graph.blocks.size());
	const auto all   = AllBlockIds(count);

	for (auto& block: graph.blocks) {
		block.dominators = (block.id == graph.entry_block ? std::vector<uint32_t> {block.id} : all);
	}

	bool changed = true;
	while (changed) {
		changed = false;
		for (auto& block: graph.blocks) {
			if (block.id == graph.entry_block) {
				continue;
			}
			std::vector<uint32_t> next;
			if (block.predecessors.empty()) {
				next = {block.id};
			} else {
				next = graph.blocks[block.predecessors.front()].dominators;
				for (uint32_t i = 1; i < block.predecessors.size(); i++) {
					next = IntersectSorted(next, graph.blocks[block.predecessors[i]].dominators);
				}
				AddUnique(next, block.id);
				SortUnique(next);
			}
			if (next != block.dominators) {
				block.dominators = std::move(next);
				changed          = true;
			}
		}
	}
}

void ComputeBackEdges(Graph& graph) {
	graph.back_edges.clear();
	for (const auto& block: graph.blocks) {
		for (auto succ: block.successors) {
			if (graph.Dominates(succ, block.id)) {
				graph.back_edges.push_back({block.id, succ, true});
			}
		}
	}
}

std::vector<uint32_t> NaturalLoopBody(const Graph& graph, uint32_t header, uint32_t latch,
                                      bool* natural) {
	std::vector<uint32_t> body;
	std::vector<uint32_t> stack;
	body.reserve(graph.blocks.size());
	stack.reserve(graph.blocks.size());
	AddUnique(body, header);
	AddUnique(body, latch);
	if (latch != header) {
		stack.push_back(latch);
	}
	if (natural != nullptr) {
		*natural = true;
	}

	while (!stack.empty()) {
		const auto block_id = stack.back();
		stack.pop_back();
		const auto* block = graph.FindBlock(block_id);
		if (block == nullptr) {
			continue;
		}
		for (auto pred: block->predecessors) {
			if (!graph.Dominates(header, pred) && natural != nullptr) {
				*natural = false;
			}
			if (!Contains(body, pred)) {
				body.push_back(pred);
				if (pred != header) {
					stack.push_back(pred);
				}
			}
		}
	}
	SortUnique(body);
	return body;
}

void ComputeNaturalLoops(Graph& graph) {
	graph.natural_loops.clear();
	for (auto& edge: graph.back_edges) {
		bool natural = true;
		auto body    = NaturalLoopBody(graph, edge.to, edge.from, &natural);
		edge.natural = natural;

		NaturalLoop loop;
		loop.header      = edge.to;
		loop.latch       = edge.from;
		loop.body_blocks = std::move(body);
		graph.natural_loops.push_back(std::move(loop));
	}
}

struct TarjanState {
	const Graph*                            graph      = nullptr;
	uint32_t                                next_index = 0;
	std::vector<uint32_t>                   index;
	std::vector<uint32_t>                   lowlink;
	std::vector<bool>                       on_stack;
	std::vector<uint32_t>                   stack;
	std::vector<StronglyConnectedComponent> components;
};

void TarjanVisit(TarjanState& state, uint32_t block_id) {
	state.index[block_id]   = state.next_index;
	state.lowlink[block_id] = state.next_index;
	state.next_index++;
	state.stack.push_back(block_id);
	state.on_stack[block_id] = true;

	const auto* block = state.graph->FindBlock(block_id);
	if (block != nullptr) {
		for (auto succ: block->successors) {
			if (state.index[succ] == UINT32_MAX) {
				TarjanVisit(state, succ);
				state.lowlink[block_id] = std::min(state.lowlink[block_id], state.lowlink[succ]);
			} else if (state.on_stack[succ]) {
				state.lowlink[block_id] = std::min(state.lowlink[block_id], state.index[succ]);
			}
		}
	}

	if (state.lowlink[block_id] != state.index[block_id]) {
		return;
	}

	StronglyConnectedComponent component;
	for (;;) {
		const auto member = state.stack.back();
		state.stack.pop_back();
		state.on_stack[member] = false;
		component.blocks.push_back(member);
		if (member == block_id) {
			break;
		}
	}
	SortUnique(component.blocks);

	bool cyclic = component.blocks.size() > 1u;
	for (auto member: component.blocks) {
		const auto* member_block = state.graph->FindBlock(member);
		if (member_block != nullptr && Contains(member_block->successors, member)) {
			cyclic = true;
		}
		if (member_block != nullptr) {
			for (auto pred: member_block->predecessors) {
				if (!Contains(component.blocks, pred)) {
					AddUnique(component.entry_blocks, member);
				}
			}
		}
	}
	SortUnique(component.entry_blocks);
	component.irreducible = cyclic && component.entry_blocks.size() > 1u;
	state.components.push_back(std::move(component));
}

void ComputeComponents(Graph& graph) {
	TarjanState state;
	state.graph = &graph;
	state.index.assign(graph.blocks.size(), UINT32_MAX);
	state.lowlink.assign(graph.blocks.size(), UINT32_MAX);
	state.on_stack.assign(graph.blocks.size(), false);
	state.stack.reserve(graph.blocks.size());
	state.components.reserve(graph.blocks.size());

	for (const auto& block: graph.blocks) {
		if (state.index[block.id] == UINT32_MAX) {
			TarjanVisit(state, block.id);
		}
	}

	graph.components  = std::move(state.components);
	graph.irreducible = false;
	for (const auto& component: graph.components) {
		if (component.irreducible) {
			graph.irreducible   = true;
			graph.failure_kind  = FailureKind::IrreducibleControlFlow;
			graph.failure_block = component.entry_blocks.empty() ? component.blocks.front()
			                                                     : component.entry_blocks.front();
			graph.unsupported_reason = "irreducible CFG: cyclic component has multiple entries";
			break;
		}
	}
}

void RecomputeAnalyses(Graph& graph) {
	ComputeDominators(graph);
	ComputeBackEdges(graph);
	ComputeNaturalLoops(graph);
	ComputeComponents(graph);
}

std::vector<uint32_t> ApplyBlockOrder(Graph& graph, std::vector<BasicBlock> blocks) {
	std::vector<uint32_t> id_map(blocks.size(), UINT32_MAX);
	for (uint32_t i = 0; i < blocks.size(); i++) {
		id_map[blocks[i].id] = i;
	}

	graph.blocks      = std::move(blocks);
	graph.entry_block = RemapId(graph.entry_block, id_map);
	for (auto& block: graph.blocks) {
		block.id = RemapId(block.id, id_map);
		RemapIds(block.predecessors, id_map);
		RemapIds(block.successors, id_map);
		RemapIds(block.dominators, id_map);
		block.terminator.true_block     = RemapId(block.terminator.true_block, id_map);
		block.terminator.false_block    = RemapId(block.terminator.false_block, id_map);
		block.terminator.merge_block    = RemapId(block.terminator.merge_block, id_map);
		block.terminator.continue_block = RemapId(block.terminator.continue_block, id_map);
	}

	return id_map;
}

std::string VectorToString(const std::vector<uint32_t>& values) {
	std::string text;
	for (uint32_t i = 0; i < values.size(); i++) {
		if (i != 0) {
			text += ",";
		}
		text += fmt::format("{}", values[i]);
	}
	return text;
}

} // namespace

const BasicBlock* Graph::FindBlock(uint32_t id) const {
	if (id < blocks.size() && blocks[id].id == id) {
		return &blocks[id];
	}
	for (const auto& block: blocks) {
		if (block.id == id) {
			return &block;
		}
	}
	return nullptr;
}

BasicBlock* Graph::FindBlock(uint32_t id) {
	return const_cast<BasicBlock*>(static_cast<const Graph*>(this)->FindBlock(id));
}

const BasicBlock* Graph::FindBlockByPc(uint32_t pc) const {
	for (const auto& block: blocks) {
		if (block.start_pc == pc) {
			return &block;
		}
	}
	return nullptr;
}

BasicBlock* Graph::FindBlockByPc(uint32_t pc) {
	return const_cast<BasicBlock*>(static_cast<const Graph*>(this)->FindBlockByPc(pc));
}

bool Graph::Dominates(uint32_t dominator, uint32_t block) const {
	const auto* target = FindBlock(block);
	return target != nullptr && Contains(target->dominators, dominator);
}

namespace {

// Eliminate gotos by moving them through a structured statement tree. Every Code node
// retains exactly one guest block; only Boolean route values cross construct boundaries.
// This follows the outward/inward/lifting transformations used by shadPS4's GotoPass.
class GotoStructurizer {
	using ExprOp = ConditionExpression::Op;
	enum class Kind { Root, Code, Label, Goto, If, Loop, Break, Return, Set };
	struct Node;
	using List = std::list<Node*>;
	struct Node {
		Kind           kind;
		Node*          parent = nullptr;
		Node*          target = nullptr;
		List           children;
		List::iterator position;
		uint32_t       condition = 1;
		uint32_t       id        = UINT32_MAX;
	};

public:
	explicit GotoStructurizer(const Graph& source): m_source(source) {
		m_graph.expressions         = {{ExprOp::Constant, 0}, {ExprOp::Constant, 1}};
		m_graph.code_table_load_pcs = source.code_table_load_pcs;
		m_root                      = New(Kind::Root);
		std::vector<Node*> labels;
		for (const auto& block: source.blocks) {
			auto* label = Insert(m_root, m_root->children.end(), Kind::Label);
			label->id   = block.id;
			labels.push_back(label);
		}
		// Give every natural loop one backedge before eliminating gotos. Otherwise
		// multiple latches become nested loops that retest the same exit predicate.
		struct Continue {
			uint32_t header;
			uint32_t after;
			size_t   body_size;
			Node*    label;
		};
		std::vector<Continue> continues;
		for (const auto& loop: source.natural_loops) {
			if (std::ranges::any_of(
			        continues, [&](const auto& value) { return value.header == loop.header; }) ||
			    std::ranges::count(source.natural_loops, loop.header, &NaturalLoop::header) < 2)
				continue;
			std::vector<uint32_t> body;
			for (const auto& member: source.natural_loops) {
				if (member.header == loop.header)
					body.insert(body.end(), member.body_blocks.begin(), member.body_blocks.end());
			}
			SortUnique(body);
			auto* label = New(Kind::Label);
			label->id   = static_cast<uint32_t>(labels.size());
			labels.push_back(label);
			continues.push_back({loop.header, body.back(), body.size(), label});
		}
		std::ranges::sort(continues, [](const auto& a, const auto& b) {
			return a.after != b.after ? a.after < b.after : a.body_size < b.body_size;
		});
		m_route_expressions.resize(labels.size(), UINT32_MAX);
		for (const auto& block: source.blocks) {
			auto*      label    = labels.at(block.id);
			const auto end      = std::next(label->position);
			auto*      code     = Insert(m_root, end, Kind::Code);
			code->id            = block.id;
			const auto add_goto = [&](uint32_t condition, uint32_t target) {
				auto* node      = Insert(m_root, end, Kind::Goto);
				node->condition = condition;
				node->target    = labels.at(target);
				for (const auto& value: continues) {
					if (value.header == target && source.Dominates(target, block.id))
						node->target = value.label;
				}
				m_gotos.push_back(node);
			};
			const auto& term = block.terminator;
			if (term.kind == TerminatorKind::ConditionalBranch) {
				// Capture at the semantic block, before movement can pass an SCC/EXEC write.
				const auto condition = Expression(ExprOp::Variable, CaptureVariable(block.id));
				add_goto(condition, term.true_block);
				add_goto(1, term.false_block);
			} else if (term.kind == TerminatorKind::Branch) {
				add_goto(1, term.true_block);
			} else if (term.kind == TerminatorKind::Return) {
				Insert(m_root, end, Kind::Return);
			} else {
				Fail("indirect control flow requires dispatcher lowering");
			}
			for (const auto& value: continues) {
				if (value.after != block.id) continue;
				value.label->parent   = m_root;
				value.label->position = m_root->children.insert(end, value.label);
				auto* repeat          = Insert(m_root, end, Kind::Goto);
				repeat->target        = labels[value.header];
				m_gotos.push_back(repeat);
			}
		}
	}

	Graph Run() {
		for (auto it = m_gotos.rbegin(); it != m_gotos.rend() && m_failure == nullptr; ++it) {
			Eliminate(*it);
		}
		if (m_failure != nullptr) return Failed();
		const auto entry    = Allocate();
		m_graph.entry_block = entry;
		Place(entry);
		Branch(Visit(m_root, entry, UINT32_MAX), UINT32_MAX);
		if (m_failure != nullptr) return Failed();
		std::vector<BasicBlock> ordered;
		ordered.reserve(m_order.size());
		for (const auto id: m_order) {
			ordered.push_back(std::move(m_graph.blocks[id]));
		}
		ApplyBlockOrder(m_graph, std::move(ordered));
		RebuildPredecessors(m_graph);
		if (std::ranges::any_of(m_graph.blocks, [&](const auto& block) {
			    return block.id != m_graph.entry_block && block.predecessors.empty();
		    })) {
			Fail("structured control flow requires an unreachable merge");
			return Failed();
		}
		RecomputeAnalyses(m_graph);
		if (m_graph.irreducible) {
			Fail("goto elimination produced irreducible control flow");
			return Failed();
		}
		return std::move(m_graph);
	}

private:
	void  Fail(const char* reason) { m_failure = reason; }
	Graph Failed() {
		SetFailure(m_graph, FailureKind::StructuredControlFlow, UINT32_MAX, m_failure);
		return std::move(m_graph);
	}
	uint32_t CaptureVariable(uint32_t block) const {
		return static_cast<uint32_t>(m_route_expressions.size()) + block;
	}
	Node* New(Kind kind) {
		m_nodes.emplace_back();
		m_nodes.back().kind = kind;
		return &m_nodes.back();
	}
	Node* Insert(Node* parent, List::iterator before, Kind kind) {
		auto* node     = New(kind);
		node->parent   = parent;
		node->position = parent->children.insert(before, node);
		return node;
	}
	void     Erase(Node* node) { node->parent->children.erase(node->position); }
	uint32_t Expression(ExprOp op, uint32_t lhs, uint32_t rhs = UINT32_MAX) {
		if (op == ExprOp::Not) {
			const auto& inner = m_graph.expressions[lhs];
			if (inner.op == ExprOp::Constant) return inner.lhs ? 0 : 1;
			if (inner.op == ExprOp::Not) return inner.lhs;
		}
		if (op == ExprOp::Or) {
			if (lhs == 1 || rhs == 1) return 1;
			if (lhs == 0 || lhs == rhs) return rhs;
			if (rhs == 0) return lhs;
		}
		const auto result = static_cast<uint32_t>(m_graph.expressions.size());
		m_graph.expressions.push_back({op, lhs, rhs});
		return result;
	}
	uint32_t RouteVariable(Node* label) {
		auto& expression = m_route_expressions[label->id];
		if (expression == UINT32_MAX) {
			expression = Expression(ExprOp::Variable, label->id);
			// SSA initializes route variables to false; clear them again on each label visit.
			auto* reset      = Insert(label->parent, std::next(label->position), Kind::Set);
			reset->id        = label->id;
			reset->condition = 0;
		}
		return expression;
	}
	static size_t Depth(Node* node) {
		size_t depth = 0;
		for (; node->parent != nullptr; node = node->parent)
			++depth;
		return depth;
	}
	static bool Related(Node* a, Node* b) {
		auto a_depth = Depth(a);
		auto b_depth = Depth(b);
		while (a_depth > b_depth) {
			a = a->parent;
			--a_depth;
		}
		while (b_depth > a_depth) {
			b = b->parent;
			--b_depth;
		}
		return a->parent == b->parent;
	}
	static bool Ordered(Node* a, Node* b) {
		for (auto it = a->position; it != a->parent->children.end(); ++it) {
			if (*it == b) return true;
		}
		return false;
	}
	static Node* Sibling(Node* uncle, Node* nephew) {
		while (nephew->parent != uncle->parent)
			nephew = nephew->parent;
		return nephew;
	}
	static void Adopt(Node* parent) {
		for (auto* child: parent->children)
			child->parent = parent;
	}
	void SetBefore(Node* node, uint32_t id, uint32_t expression) {
		auto* set      = Insert(node->parent, node->position, Kind::Set);
		set->id        = id;
		set->condition = expression;
	}
	Node* GotoAfter(Node* construct, Node* label, uint32_t condition) {
		auto* node      = Insert(construct->parent, std::next(construct->position), Kind::Goto);
		node->target    = label;
		node->condition = condition;
		return node;
	}
	Node* Outward(Node* node) {
		auto*      parent   = node->parent;
		auto*      target   = node->target;
		const auto variable = RouteVariable(target);
		SetBefore(node, target->id, node->condition);
		if (parent->kind == Kind::If) {
			auto* rest      = Insert(parent, node->position, Kind::If);
			rest->condition = Expression(ExprOp::Not, variable);
			rest->children.splice(rest->children.end(), parent->children, std::next(node->position),
			                      parent->children.end());
			Adopt(rest);
		} else if (parent->kind == Kind::Loop) {
			auto* stop      = Insert(parent, node->position, Kind::Break);
			stop->condition = variable;
		} else {
			Fail("goto has no enclosing selection or loop");
			return node;
		}
		Erase(node);
		return GotoAfter(parent, target, variable);
	}
	Node* Inward(Node* node) {
		auto*      parent   = node->parent;
		auto*      target   = node->target;
		auto*      nested   = Sibling(node, target);
		const auto variable = RouteVariable(target);
		SetBefore(node, target->id, node->condition);
		auto* rest      = Insert(parent, node->position, Kind::If);
		rest->condition = Expression(ExprOp::Not, variable);
		rest->children.splice(rest->children.end(), parent->children, std::next(node->position),
		                      nested->position);
		Adopt(rest);
		Erase(node);
		if (nested->kind == Kind::If) {
			nested->condition = Expression(ExprOp::Or, variable, nested->condition);
		} else if (nested->kind != Kind::Loop) {
			Fail("goto target has no enclosing construct");
			return node;
		}
		auto* moved      = Insert(nested, nested->children.begin(), Kind::Goto);
		moved->target    = target;
		moved->condition = variable;
		return moved;
	}
	Node* Lift(Node* node) {
		auto*      parent   = node->parent;
		auto*      target   = node->target;
		auto*      nested   = Sibling(node, target);
		const auto variable = RouteVariable(target);
		auto*      loop     = Insert(parent, node->position, Kind::Loop);
		loop->condition     = variable;
		loop->children.splice(loop->children.end(), parent->children, nested->position,
		                      loop->position);
		Adopt(loop);
		if (std::ranges::any_of(loop->children,
		                        [](auto* child) { return child->kind == Kind::Break; })) {
			Fail("goto lifting would capture an enclosing loop exit");
			return node;
		}
		auto* moved      = Insert(loop, loop->children.begin(), Kind::Goto);
		moved->target    = target;
		moved->condition = variable;
		auto* set        = Insert(loop, loop->children.end(), Kind::Set);
		set->id          = target->id;
		set->condition   = node->condition;
		Erase(node);
		return moved;
	}
	void Eliminate(Node* node) {
		auto* target = node->target;
		while (node->parent != target->parent && m_failure == nullptr) {
			if (!Related(node, target) || Depth(node) > Depth(target)) {
				node = Outward(node);
			} else if (Ordered(Sibling(node, target), node)) {
				node = Lift(node);
			} else {
				node = Inward(node);
			}
		}
		if (m_failure != nullptr) return;
		if (std::next(node->position) == target->position) {
			Erase(node);
			return;
		}
		auto* parent = node->parent;
		if (Ordered(node, target)) {
			auto* selection      = Insert(parent, node->position, Kind::If);
			selection->condition = Expression(ExprOp::Not, node->condition);
			selection->children.splice(selection->children.end(), parent->children,
			                           std::next(node->position), target->position);
			Adopt(selection);
		} else {
			auto* loop      = Insert(parent, node->position, Kind::Loop);
			loop->condition = node->condition;
			loop->children.splice(loop->children.end(), parent->children, target->position,
			                      loop->position);
			Adopt(loop);
		}
		Erase(node);
	}

	uint32_t Allocate() {
		BasicBlock block;
		block.id = static_cast<uint32_t>(m_graph.blocks.size());
		m_graph.blocks.push_back(std::move(block));
		return m_graph.blocks.back().id;
	}
	void Place(uint32_t block) { m_order.push_back(block); }
	void Branch(uint32_t from, uint32_t to) {
		if (from == UINT32_MAX) return;
		auto& block           = m_graph.blocks[from];
		block.terminator.kind = to == UINT32_MAX ? TerminatorKind::Return : TerminatorKind::Branch;
		block.terminator.true_block = to;
		block.successors = to == UINT32_MAX ? std::vector<uint32_t> {} : std::vector<uint32_t> {to};
	}
	void Conditional(uint32_t block_id, uint32_t condition, uint32_t yes, uint32_t no,
	                 uint32_t merge = UINT32_MAX) {
		auto& block      = m_graph.blocks[block_id];
		auto& term       = block.terminator;
		term.kind        = TerminatorKind::ConditionalBranch;
		term.expression  = condition;
		term.condition   = BranchCondition::Expression;
		term.true_block  = yes;
		term.false_block = no;
		term.merge_block = merge;
		block.successors = {yes, no};
	}
	uint32_t Advance(uint32_t current) {
		const auto next = Allocate();
		Branch(current, next);
		Place(next);
		return next;
	}
	bool RefersTo(uint32_t expression, uint32_t variable) const {
		const auto& value = m_graph.expressions[expression];
		switch (value.op) {
			case ExprOp::Variable: return value.lhs == variable;
			case ExprOp::Not: return RefersTo(value.lhs, variable);
			case ExprOp::Or: return RefersTo(value.lhs, variable) || RefersTo(value.rhs, variable);
			default: return false;
		}
	}
	bool Modifies(Node* node, uint32_t expression) const {
		if (node->kind == Kind::Set && RefersTo(expression, node->id)) return true;
		if (node->kind == Kind::Code &&
		    m_source.blocks[node->id].terminator.kind == TerminatorKind::ConditionalBranch &&
		    RefersTo(expression, CaptureVariable(node->id)))
			return true;
		return std::ranges::any_of(node->children,
		                           [&](auto* child) { return Modifies(child, expression); });
	}
	bool Complements(uint32_t first, uint32_t second) const {
		const auto& a = m_graph.expressions[first];
		const auto& b = m_graph.expressions[second];
		return (a.op == ExprOp::Not && a.lhs == second) || (b.op == ExprOp::Not && b.lhs == first);
	}
	uint32_t Visit(Node* parent, uint32_t current, uint32_t break_block) {
		for (auto it = parent->children.begin(); it != parent->children.end(); ++it) {
			auto* node = *it;
			if (node->kind == Kind::Label) continue;
			if (current == UINT32_MAX) {
				current = Allocate();
				Place(current);
			}
			switch (node->kind) {
				case Kind::Code: {
					const auto& source   = m_source.blocks[node->id];
					const auto& previous = m_graph.blocks[current];
					// Route assignments cannot observe guest writes; native predicates must stay at
					// their original Code.
					const bool captures_native =
					    std::ranges::any_of(previous.assignments, [&](const auto& assignment) {
						    return m_graph.expressions[assignment.expression].op == ExprOp::Native;
					    });
					if (captures_native || (previous.inst_begin != previous.inst_end &&
					                        previous.inst_end != source.inst_begin)) {
						current = Advance(current);
					}
					auto& block = m_graph.blocks[current];
					if (block.inst_begin == block.inst_end) {
						block.start_pc   = source.start_pc;
						block.inst_begin = source.inst_begin;
					}
					block.end_pc   = source.end_pc;
					block.inst_end = source.inst_end;
					if (source.terminator.kind == TerminatorKind::ConditionalBranch) {
						block.assignments.push_back(
						    {CaptureVariable(source.id),
						     Expression(ExprOp::Native,
						                static_cast<uint32_t>(source.terminator.condition))});
					}
					break;
				}
				case Kind::Set: {
					m_graph.blocks[current].assignments.push_back({node->id, node->condition});
					break;
				}
				case Kind::If: {
					if (node->condition == 0 || node->children.empty()) break;
					if (node->condition == 1) {
						current = Visit(node, current, break_block);
						break;
					}
					auto next = std::next(it);
					while (next != parent->children.end() && (*next)->kind == Kind::Label)
						++next;
					Node*      other       = next != parent->children.end() ? *next : nullptr;
					const bool paired      = other != nullptr && other->kind == Kind::If &&
					                         Complements(node->condition, other->condition) &&
					                         !Modifies(node, node->condition);
					const auto body        = Allocate();
					const auto merge       = Allocate();
					const auto alternative = paired ? Allocate() : merge;
					Conditional(current, node->condition, body, alternative, merge);
					Place(body);
					Branch(Visit(node, body, break_block), merge);
					if (paired) {
						Place(alternative);
						Branch(Visit(other, alternative, break_block), merge);
						it = next;
					}
					Place(merge);
					current = merge;
					break;
				}
				case Kind::Loop: {
					const auto& previous = m_graph.blocks[current];
					const bool  empty =
					    previous.inst_begin == previous.inst_end && previous.assignments.empty();
					const auto header = empty ? current : Advance(current);
					const auto body   = Allocate();
					const auto merge  = Allocate();
					Branch(header, body);
					Place(body);
					auto repeat = Visit(node, body, merge);
					// A construct merge belongs to the loop body, not its continue construct.
					if (repeat == UINT32_MAX ||
					    std::ranges::any_of(m_graph.blocks, [&](const auto& block) {
						    return block.terminator.merge_block == repeat;
					    })) {
						repeat = Advance(repeat);
					}
					auto& term          = m_graph.blocks[header].terminator;
					term.loop_header    = true;
					term.merge_block    = merge;
					term.continue_block = repeat;
					if (node->condition <= 1)
						Branch(repeat, node->condition ? header : merge);
					else
						Conditional(repeat, node->condition, header, merge);
					Place(merge);
					current = merge;
					break;
				}
				case Kind::Break: {
					if (break_block == UINT32_MAX) {
						Fail("break outside a loop");
						return UINT32_MAX;
					}
					if (node->condition == 0) break;
					if (node->condition == 1) {
						Branch(current, break_block);
						current = UINT32_MAX;
					} else {
						const auto next = Allocate();
						Conditional(current, node->condition, break_block, next);
						Place(next);
						current = next;
					}
					break;
				}
				case Kind::Return:
					Branch(current, UINT32_MAX);
					current = UINT32_MAX;
					break;
				default: Fail("uneliminated control-flow statement"); return UINT32_MAX;
			}
			if (m_failure != nullptr) return UINT32_MAX;
		}
		return current;
	}

	const char*           m_failure = nullptr;
	const Graph&          m_source;
	Graph                 m_graph;
	std::deque<Node>      m_nodes;
	Node*                 m_root = nullptr;
	std::vector<Node*>    m_gotos;
	std::vector<uint32_t> m_route_expressions;
	std::vector<uint32_t> m_order;
};

} // namespace

Graph BuildGraph(const Decoder::Program& program) {
	Graph graph;
	if (program.instructions.empty()) {
		ExitBuildFailure(graph, FailureKind::InvalidLabel, UINT32_MAX,
		                 "cannot build CFG for empty shader");
	}

	const auto first_pc = program.instructions.front().pc;
	const auto end_pc   = ProgramEndPc(program);

	for (const auto& inst: program.instructions) {
		if (inst.opcode == Opcode::UNSUPPORTED) {
			ExitBuildFailure(
			    graph, FailureKind::UnsupportedInstruction, UINT32_MAX,
			    fmt::format("unsupported decoded instruction in CFG at pc 0x{:08x}: {}", inst.pc,
			                Decoder::InstructionToString(inst).c_str()));
		}
	}

	std::vector<uint32_t> labels {first_pc, end_pc};

	std::map<uint32_t, SetpcTargetInfo> setpc_targets;
	for (uint32_t i = 0; i < program.instructions.size(); i++) {
		const auto& inst    = program.instructions[i];
		const auto  next_pc = InstructionEndPc(inst);
		if (Decoder::IsDirectBranch(inst.opcode) ||
		    (inst.opcode == Opcode::S_SWAPPC_B64 && inst.branch_target != UINT32_MAX)) {
			if (!IsValidTarget(inst.branch_target, program.instructions, end_pc)) {
				ExitBuildFailure(graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
				                 fmt::format("branch at pc 0x{:08x} targets invalid pc 0x{:08x}",
				                             inst.pc, inst.branch_target));
			}
			labels.push_back(inst.branch_target);
			if (next_pc <= end_pc) {
				labels.push_back(next_pc);
			}
		} else if (inst.opcode == Opcode::S_SETPC_B64) {
			SetpcTargetInfo target_info;
			if (!ResolveSetpcTargets(program, i, target_info)) {
				ExitBuildFailure(
				    graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
				    fmt::format("unsupported dynamic S_SETPC_B64 at pc 0x{:08x}", inst.pc));
			}
			const auto target_pcs = target_info.indirect
			                            ? std::span<const uint32_t>(target_info.target_pcs)
			                            : std::span<const uint32_t>(&target_info.target, 1);
			for (const auto target: target_pcs) {
				if (!IsValidTarget(target, program.instructions, end_pc)) {
					ExitBuildFailure(
					    graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
					    fmt::format("S_SETPC_B64 at pc 0x{:08x} targets invalid pc 0x{:08x}",
					                inst.pc, target));
				}
				labels.push_back(target);
			}
			setpc_targets.emplace(inst.pc, std::move(target_info));
			if (next_pc <= end_pc) {
				labels.push_back(next_pc);
			}
		} else if (inst.opcode == Opcode::S_ENDPGM || inst.opcode == Opcode::S_SWAPPC_B64) {
			labels.push_back(next_pc);
		}
	}

	SortUnique(labels);
	graph.blocks.reserve(labels.size());
	for (uint32_t i = 0; i < labels.size(); i++) {
		const auto start = labels[i];
		if (start > end_pc) {
			continue;
		}
		const auto begin =
		    std::ranges::lower_bound(program.instructions, start, {}, &Instruction::pc);
		if (start != end_pc && (begin == program.instructions.end() || begin->pc != start)) {
			ExitBuildFailure(
			    graph, FailureKind::InvalidLabel, UINT32_MAX,
			    fmt::format("CFG label does not start on an instruction: 0x{:08x}", start));
		}

		BasicBlock block;
		block.id         = static_cast<uint32_t>(graph.blocks.size());
		block.start_pc   = start;
		block.end_pc     = i + 1u < labels.size() ? labels[i + 1u] : end_pc;
		block.inst_begin = static_cast<uint32_t>(begin - program.instructions.begin());
		block.inst_end   = static_cast<uint32_t>(
		    std::lower_bound(program.instructions.begin(), program.instructions.end(), block.end_pc,
		                     [](const Instruction& inst, uint32_t pc) { return inst.pc < pc; }) -
		    program.instructions.begin());
		graph.blocks.push_back(std::move(block));
	}

	graph.entry_block = 0;

	const auto block_at_pc = [&](uint32_t pc) {
		const auto found = std::ranges::lower_bound(graph.blocks, pc, {}, &BasicBlock::start_pc);
		return found != graph.blocks.end() && found->start_pc == pc ? found->id : UINT32_MAX;
	};

	for (auto& block: graph.blocks) {
		block.terminator = {};
		if (block.inst_begin == block.inst_end) {
			block.terminator.kind = TerminatorKind::Return;
			continue;
		}

		const auto& last    = program.instructions[block.inst_end - 1u];
		const auto  next_pc = InstructionEndPc(last);
		if (last.opcode == Opcode::S_ENDPGM) {
			block.terminator.kind = TerminatorKind::Return;
		} else if (last.opcode == Opcode::S_SWAPPC_B64 && last.branch_target != UINT32_MAX) {
			block.terminator.kind       = TerminatorKind::Branch;
			block.terminator.true_block = block_at_pc(last.branch_target);
		} else if (last.opcode == Opcode::S_SETPC_B64) {
			const auto& target_info = setpc_targets.at(last.pc);
			if (target_info.indirect) {
				block.terminator.kind                   = TerminatorKind::IndirectBranch;
				block.terminator.condition              = BranchCondition::Always;
				block.terminator.indirect_pc_sgpr       = target_info.pc_sgpr;
				block.terminator.indirect_selector_code = target_info.selector_code;
				for (const auto target_pc: target_info.target_pcs) {
					block.terminator.indirect_target_pcs.push_back(target_pc);
					block.terminator.indirect_targets.push_back(block_at_pc(target_pc));
				}
				const auto selector_count = std::min(target_info.selector_values.size(),
				                                     target_info.selector_target_pcs.size());
				for (uint32_t i = 0; i < selector_count; i++) {
					block.terminator.indirect_selector_values.push_back(
					    target_info.selector_values[i]);
					block.terminator.indirect_selector_targets.push_back(
					    block_at_pc(target_info.selector_target_pcs[i]));
				}
			} else {
				block.terminator.kind       = TerminatorKind::Branch;
				block.terminator.condition  = BranchCondition::Always;
				block.terminator.true_block = block_at_pc(target_info.target);
			}
		} else if (last.opcode == Opcode::S_BRANCH) {
			block.terminator.kind       = TerminatorKind::Branch;
			block.terminator.condition  = BranchCondition::Always;
			block.terminator.true_block = block_at_pc(last.branch_target);
		} else if (Decoder::IsConditionalBranch(last.opcode)) {
			block.terminator.kind       = TerminatorKind::ConditionalBranch;
			block.terminator.condition  = ConditionForOpcode(last.opcode);
			block.terminator.true_block = block_at_pc(last.branch_target);
			const auto fallthrough      = block_at_pc(next_pc);
			if (fallthrough == UINT32_MAX) {
				ExitBuildFailure(
				    graph, FailureKind::MissingFallthrough, block.id,
				    fmt::format("conditional branch at pc 0x{:08x} has no fallthrough block",
				                last.pc));
			}
			block.terminator.false_block = fallthrough;
		} else {
			const auto next = block_at_pc(block.end_pc);
			if (next != UINT32_MAX && block.end_pc != block.start_pc) {
				block.terminator.kind       = TerminatorKind::Branch;
				block.terminator.condition  = BranchCondition::Always;
				block.terminator.true_block = next;
			} else {
				block.terminator.kind = TerminatorKind::Return;
			}
		}

		switch (block.terminator.kind) {
			case TerminatorKind::Branch:
				AddUnique(block.successors, block.terminator.true_block);
				break;
			case TerminatorKind::ConditionalBranch:
				AddUnique(block.successors, block.terminator.true_block);
				AddUnique(block.successors, block.terminator.false_block);
				break;
			case TerminatorKind::IndirectBranch:
				for (const auto target: block.terminator.indirect_targets) {
					AddUnique(block.successors, target);
				}
				break;
			case TerminatorKind::Return:
			case TerminatorKind::Unsupported: break;
		}
	}

	for (auto& block: graph.blocks) {
		SortUnique(block.successors);
		for (auto succ: block.successors) {
			AddUnique(graph.blocks[succ].predecessors, block.id);
		}
	}
	for (auto& block: graph.blocks) {
		SortUnique(block.predecessors);
	}
	PruneUnreachableBlocks(graph);
	graph.code_table_load_pcs.clear();
	bool indirect_setpc = false;
	for (const auto& block: graph.blocks) {
		if (block.terminator.kind != TerminatorKind::IndirectBranch) {
			continue;
		}
		indirect_setpc = true;
		if (block.inst_begin >= block.inst_end || block.inst_end > program.instructions.size()) {
			continue;
		}
		const auto found = setpc_targets.find(program.instructions[block.inst_end - 1u].pc);
		if (found != setpc_targets.end() && found->second.table_load_pc != UINT32_MAX) {
			AddUnique(graph.code_table_load_pcs, found->second.table_load_pc);
		}
	}
	SortUnique(graph.code_table_load_pcs);

	RecomputeAnalyses(graph);

	if (indirect_setpc) {
		graph.irreducible  = true;
		graph.unsupported  = false;
		graph.failure_kind = FailureKind::IrreducibleControlFlow;
		const auto indirect =
		    std::find_if(graph.blocks.begin(), graph.blocks.end(), [](const auto& block) {
			    return block.terminator.kind == TerminatorKind::IndirectBranch;
		    });
		graph.failure_block = indirect != graph.blocks.end() ? indirect->id : graph.entry_block;
		graph.unsupported_reason = "indirect S_SETPC_B64 jump table requires dispatcher fallback";
	}

	if (graph.irreducible) {
		graph.unsupported = false;
	}

	return graph;
}

Graph Structurize(const Graph& graph) {
	if (graph.unsupported || graph.irreducible) {
		Graph failed;
		SetFailure(failed, graph.failure_kind, graph.failure_block, graph.unsupported_reason);
		return failed;
	}
	return GotoStructurizer(graph).Run();
}

bool MayWriteScalarRegister(const Decoder::Instruction& inst, uint32_t code) {
	uint32_t destination = UINT32_MAX;
	// Scalar ALU pair widths are not all decoded. The typed producer must confirm this word.
	return InstructionWritesScalarCode(inst, code) ||
	       (ScalarOperandCode(inst.dst, destination) && code == destination + 1u);
}

uint32_t FindScalarDefinition(const Decoder::Program& program, const Graph& graph, uint32_t before,
                              uint32_t code, const std::function<bool(uint32_t)>& confirm) {
	EXIT_IF(before >= program.instructions.size());
	const auto found = std::ranges::find_if(graph.blocks, [before](const auto& block) {
		return block.inst_begin <= before && before < block.inst_end;
	});
	EXIT_IF(found == graph.blocks.end());
	using BlockT = std::remove_cvref_t<decltype(*found)>;

	std::set<uint32_t> defs;    // definitions found (UINT32_MAX = comes from outside shader)
	std::set<uint32_t> visited; // blocks already fully scanned
	std::vector<std::pair<const BlockT*, uint32_t>> work;
	work.emplace_back(&*found, before);

	// --- DIAGNOSTIC: FSD entry ---
	const bool fsd_trace = (std::getenv("KYTY_FSD_TRACE") != nullptr);
	if (fsd_trace) {
		printf("    FSD: s%u before=%u start_block=%u [%u,%u)\n", code, before, (unsigned)found->id,
		       found->inst_begin, found->inst_end);
	}

	while (!work.empty()) {
		auto [block, from] = work.back();
		work.pop_back();
		// --- DIAGNOSTIC: FSD worklist step ---
		if (fsd_trace) {
			printf("    FSD:  scan block=%u from=%u [%u,%u)\n", (unsigned)block->id, from,
			       block->inst_begin, block->inst_end);
		}
		bool done = false;
		for (uint32_t i = from; i > block->inst_begin;) {
			const auto& inst = program.instructions[--i];
			if (!MayWriteScalarRegister(inst, code)) {
				continue;
			}
			// --- DIAGNOSTIC: candidate found, check confirm ---
			const bool confirmed = !confirm || confirm(i);
			if (fsd_trace) {
				printf("    FSD:   candidate @%u opcode=%d confirmed=%s\n", i, (int)inst.opcode,
				       confirmed ? "yes" : "no");
			}
			if (!confirmed) {
				continue; // false positive "dst + 1"
			}
			defs.insert(i);
			done = true;
			break;
		}
		if (done) continue;
		if (block->id == graph.entry_block) {
			if (fsd_trace) {
				printf("    FSD:   reached entry block -> user_data (UINT32_MAX)\n");
			}
			defs.insert(UINT32_MAX);
			continue;
		}
		EXIT_IF(block->predecessors.empty());
		for (auto p: block->predecessors) {
			const auto* pb = graph.FindBlock(p);
			EXIT_IF(pb == nullptr);
			if (!visited.insert(static_cast<uint32_t>(pb->id)).second) {
				if (fsd_trace) {
					printf("    FSD:   pred block=%u already visited, skip\n", (unsigned)pb->id);
				}
				continue;
			}
			if (fsd_trace) {
				printf("    FSD:   enqueue pred block=%u [%u,%u)\n", (unsigned)pb->id,
				       pb->inst_begin, pb->inst_end);
			}
			work.emplace_back(pb, pb->inst_end);
		}
	}
	if (defs.size() != 1u) {
		printf("FSD: s%u from %u has %zu distinct definitions:", code, before, defs.size());
		for (auto d: defs)
			printf(" %d", d == UINT32_MAX ? -1 : (int)d);
		printf("\n");
	} else {
		const auto def = *defs.begin();
		if (fsd_trace) {
			if (def == UINT32_MAX) {
				printf("    FSD: s%u result -> user_data (UINT32_MAX)\n", code);
			} else {
				printf("    FSD: s%u result -> def @%u opcode=%d\n", code, def,
				       (int)program.instructions[def].opcode);
			}
		}
	}
	EXIT_NOT_IMPLEMENTED(defs.size() != 1u);
	return *defs.begin();
}

std::string BranchConditionToString(BranchCondition condition) {
	switch (condition) {
		case BranchCondition::Always: return "always";
		case BranchCondition::SccZero: return "scc0";
		case BranchCondition::SccNonZero: return "scc1";
		case BranchCondition::VccZero: return "vccz";
		case BranchCondition::VccNonZero: return "vccnz";
		case BranchCondition::ExecZero: return "execz";
		case BranchCondition::ExecNonZero: return "execnz";
		case BranchCondition::ScalarInstruction: return "scalar_instruction";
		case BranchCondition::Expression: return "expression";
		default: return "unknown";
	}
}

std::string FailureKindToString(FailureKind kind) {
	switch (kind) {
		case FailureKind::None: return "None";
		case FailureKind::InvalidInput: return "InvalidInput";
		case FailureKind::UnsupportedInstruction: return "UnsupportedInstruction";
		case FailureKind::InvalidBranchTarget: return "InvalidBranchTarget";
		case FailureKind::MissingFallthrough: return "MissingFallthrough";
		case FailureKind::InvalidLabel: return "InvalidLabel";
		case FailureKind::IrreducibleControlFlow: return "IrreducibleControlFlow";
		case FailureKind::StructuredControlFlow: return "StructuredControlFlow";
		default: return "Unknown";
	}
}

std::string GraphToString(const Graph& graph) {
	std::string text;
	text +=
	    fmt::format("entry_block={} irreducible={} unsupported={} failure={} failure_block={}\n",
	                graph.entry_block, graph.irreducible ? 1u : 0u, graph.unsupported ? 1u : 0u,
	                FailureKindToString(graph.failure_kind).c_str(), graph.failure_block);
	if (!graph.unsupported_reason.empty()) {
		text += "unsupported_reason=";
		text += graph.unsupported_reason;
		text += "\n";
	}

	for (const auto& block: graph.blocks) {
		text += fmt::format("block_{} pc=0x{:08x} end=0x{:08x} inst=[{},{})\n", block.id,
		                    block.start_pc, block.end_pc, block.inst_begin, block.inst_end);
		text += fmt::format("  predecessors=[{}] successors=[{}]\n",
		                    VectorToString(block.predecessors).c_str(),
		                    VectorToString(block.successors).c_str());
		text += fmt::format("  dominators=[{}]\n", VectorToString(block.dominators).c_str());
		text += fmt::format(
		    "  terminator={} condition={} true={} false={} merge={} continue={} loop_header={} "
		    "indirect_sgpr={} indirect_selector={} indirect_targets=[{}] selector_values=[{}] "
		    "expression={}\n",
		    static_cast<uint32_t>(block.terminator.kind),
		    BranchConditionToString(block.terminator.condition).c_str(),
		    block.terminator.true_block, block.terminator.false_block, block.terminator.merge_block,
		    block.terminator.continue_block, block.terminator.loop_header ? 1u : 0u,
		    block.terminator.indirect_pc_sgpr, block.terminator.indirect_selector_code,
		    VectorToString(block.terminator.indirect_targets).c_str(),
		    VectorToString(block.terminator.indirect_selector_values).c_str(),
		    block.terminator.expression);
		for (const auto& assignment: block.assignments) {
			text += fmt::format("  variable_{} = expression_{}", assignment.variable,
			                    assignment.expression);
			const auto& expression = graph.expressions[assignment.expression];
			if (expression.op == ConditionExpression::Op::Native)
				text += fmt::format(
				    " condition={}",
				    BranchConditionToString(static_cast<BranchCondition>(expression.lhs)));
			text += "\n";
		}
	}

	for (const auto& edge: graph.back_edges) {
		text += fmt::format("backedge {} -> {} natural={}\n", edge.from, edge.to,
		                    edge.natural ? 1u : 0u);
	}
	for (const auto& loop: graph.natural_loops) {
		text += fmt::format("loop header={} latch={} body=[{}]\n", loop.header, loop.latch,
		                    VectorToString(loop.body_blocks).c_str());
	}
	for (const auto& component: graph.components) {
		text += fmt::format("scc blocks=[{}] entries=[{}] irreducible={}\n",
		                    VectorToString(component.blocks).c_str(),
		                    VectorToString(component.entry_blocks).c_str(),
		                    component.irreducible ? 1u : 0u);
	}
	return text;
}

} // namespace Libs::Graphics::ShaderRecompiler::CFG
