#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <iterator>
#include <map>
#include <set>
#include <stack>

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

void SetError(std::string* error, const std::string& message) {
	if (error != nullptr) {
		*error = message;
	}
}

void SetFailure(Graph& graph, FailureKind kind, uint32_t block_id, const std::string& message,
                std::string* error) {
	graph.unsupported        = true;
	graph.failure_kind       = kind;
	graph.failure_block      = block_id;
	graph.unsupported_reason = message;
	SetError(error, message);
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

bool IsUnconditionalBranch(Opcode opcode) {
	return opcode == Opcode::S_BRANCH;
}

bool IsConditionalBranch(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_CBRANCH_SCC0:
		case Opcode::S_CBRANCH_SCC1:
		case Opcode::S_CBRANCH_VCCZ:
		case Opcode::S_CBRANCH_VCCNZ:
		case Opcode::S_CBRANCH_EXECZ:
		case Opcode::S_CBRANCH_EXECNZ: return true;
		default: return false;
	}
}

bool IsBranch(Opcode opcode) {
	return IsUnconditionalBranch(opcode) || IsConditionalBranch(opcode);
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

bool AddUniqueTargetPc(std::vector<uint32_t>& targets, uint32_t target) {
	if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
		targets.push_back(target);
	}
	return true;
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
		AddUniqueTargetPc(targets, target_pc);
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
		AddUniqueTargetPc(targets, target_pc);
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

bool IsValidTarget(uint32_t target, const std::set<uint32_t>& instruction_pcs, uint32_t first_pc,
                   uint32_t end_pc) {
	return target == end_pc || (target >= first_pc && instruction_pcs.contains(target));
}

void AddUnique(std::vector<uint32_t>& values, uint32_t value) {
	if (std::find(values.begin(), values.end(), value) == values.end()) {
		values.push_back(value);
	}
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

bool ReplaceValue(std::vector<uint32_t>& values, uint32_t old_value, uint32_t new_value) {
	bool changed = false;
	for (auto& value: values) {
		if (value == old_value) {
			value   = new_value;
			changed = true;
		}
	}
	if (changed) {
		SortUnique(values);
	}
	return changed;
}

bool RemoveValue(std::vector<uint32_t>& values, uint32_t value) {
	const auto old_size = values.size();
	values.erase(std::remove(values.begin(), values.end(), value), values.end());
	return values.size() != old_size;
}

bool ReplaceTerminatorTarget(Terminator& terminator, uint32_t old_value, uint32_t new_value) {
	bool changed = false;
	if (terminator.true_block == old_value) {
		terminator.true_block = new_value;
		changed               = true;
	}
	if (terminator.false_block == old_value) {
		terminator.false_block = new_value;
		changed                = true;
	}
	return changed;
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

void ComputePostDominators(Graph& graph) {
	const auto count = static_cast<uint32_t>(graph.blocks.size());
	const auto all   = AllBlockIds(count);

	for (auto& block: graph.blocks) {
		block.post_dominators = block.successors.empty() ? std::vector<uint32_t> {block.id} : all;
	}

	bool changed = true;
	while (changed) {
		changed = false;
		for (auto& block: graph.blocks) {
			std::vector<uint32_t> next;
			if (block.successors.empty()) {
				next = {block.id};
			} else {
				next = graph.blocks[block.successors.front()].post_dominators;
				for (uint32_t i = 1; i < block.successors.size(); i++) {
					next = IntersectSorted(next, graph.blocks[block.successors[i]].post_dominators);
				}
				AddUnique(next, block.id);
				SortUnique(next);
			}
			if (next != block.post_dominators) {
				block.post_dominators = std::move(next);
				changed               = true;
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
		loop.header         = edge.to;
		loop.latch          = edge.from;
		loop.continue_block = edge.from;
		loop.body_blocks    = body;

		for (auto block_id: body) {
			const auto* block = graph.FindBlock(block_id);
			if (block == nullptr) {
				continue;
			}
			for (auto succ: block->successors) {
				if (!Contains(body, succ)) {
					AddUnique(loop.exit_blocks, succ);
				}
			}
		}
		SortUnique(loop.exit_blocks);

		if (!loop.exit_blocks.empty()) {
			uint32_t merge = loop.exit_blocks.front();
			for (uint32_t i = 1; i < loop.exit_blocks.size(); i++) {
				merge = graph.FindNearestCommonPostDominator(merge, loop.exit_blocks[i]);
			}
			loop.merge = merge;
		}
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
	ComputePostDominators(graph);
	ComputeBackEdges(graph);
	ComputeNaturalLoops(graph);
	ComputeComponents(graph);
}

uint32_t MoveBlockBefore(Graph& graph, uint32_t block_id, uint32_t before_id) {
	if (block_id == before_id || block_id >= graph.blocks.size() ||
	    before_id >= graph.blocks.size()) {
		return block_id;
	}

	const auto              block_pos  = block_id;
	const auto              before_pos = before_id;
	std::vector<BasicBlock> old_blocks = std::move(graph.blocks);
	std::vector<BasicBlock> new_blocks;
	new_blocks.reserve(old_blocks.size());

	for (uint32_t i = 0; i < old_blocks.size(); i++) {
		if (i == before_pos) {
			new_blocks.push_back(std::move(old_blocks[block_pos]));
		}
		if (i != block_pos) {
			new_blocks.push_back(std::move(old_blocks[i]));
		}
	}

	std::vector<uint32_t> id_map(new_blocks.size(), UINT32_MAX);
	for (uint32_t i = 0; i < new_blocks.size(); i++) {
		id_map[new_blocks[i].id] = i;
	}

	graph.blocks      = std::move(new_blocks);
	graph.entry_block = RemapId(graph.entry_block, id_map);
	for (auto& block: graph.blocks) {
		block.id = RemapId(block.id, id_map);
		RemapIds(block.predecessors, id_map);
		RemapIds(block.successors, id_map);
		RemapIds(block.dominators, id_map);
		RemapIds(block.post_dominators, id_map);
		block.terminator.true_block     = RemapId(block.terminator.true_block, id_map);
		block.terminator.false_block    = RemapId(block.terminator.false_block, id_map);
		block.terminator.merge_block    = RemapId(block.terminator.merge_block, id_map);
		block.terminator.continue_block = RemapId(block.terminator.continue_block, id_map);
	}

	return RemapId(block_id, id_map);
}

std::vector<uint32_t> DominatedBlocks(const Graph& graph, uint32_t header,
                                      uint32_t stop_block = UINT32_MAX) {
	std::vector<uint32_t> blocks;
	std::vector<uint32_t> stack = {header};
	blocks.reserve(graph.blocks.size());
	stack.reserve(graph.blocks.size());

	while (!stack.empty()) {
		const auto block_id = stack.back();
		stack.pop_back();
		if (block_id == stop_block || Contains(blocks, block_id) ||
		    !graph.Dominates(header, block_id)) {
			continue;
		}

		const auto* block = graph.FindBlock(block_id);
		if (block == nullptr) {
			continue;
		}

		AddUnique(blocks, block_id);
		for (auto succ: block->successors) {
			if (succ != stop_block && graph.Dominates(header, succ)) {
				stack.push_back(succ);
			}
		}
	}

	SortUnique(blocks);
	return blocks;
}

uint32_t AppendSyntheticBranchBlock(Graph& graph, uint32_t target) {
	const auto* target_block = graph.FindBlock(target);

	BasicBlock block;
	block.id                    = static_cast<uint32_t>(graph.blocks.size());
	block.start_pc              = target_block != nullptr ? target_block->start_pc : 0u;
	block.end_pc                = block.start_pc;
	block.inst_begin            = target_block != nullptr ? target_block->inst_begin : 0u;
	block.inst_end              = block.inst_begin;
	block.successors            = {target};
	block.terminator.kind       = TerminatorKind::Branch;
	block.terminator.condition  = BranchCondition::Always;
	block.terminator.true_block = target;
	graph.blocks.push_back(std::move(block));
	return graph.blocks.back().id;
}

bool IsolateSemanticLoopHeader(Graph& graph, uint32_t old_header) {
	const auto* header = graph.FindBlock(old_header);
	if (header == nullptr || header->inst_begin == header->inst_end) {
		return false;
	}

	const auto predecessors = header->predecessors;
	const auto new_header   = AppendSyntheticBranchBlock(graph, old_header);
	for (auto pred: predecessors) {
		auto* block = graph.FindBlock(pred);
		if (block != nullptr) {
			ReplaceValue(block->successors, old_header, new_header);
			ReplaceTerminatorTarget(block->terminator, old_header, new_header);
		}
	}
	if (graph.entry_block == old_header) {
		graph.entry_block = new_header;
	}
	MoveBlockBefore(graph, new_header, old_header);
	RebuildPredecessors(graph);
	RecomputeAnalyses(graph);
	return true;
}

const NaturalLoop* FindInnermostContainingLoop(const Graph& graph, uint32_t block_id) {
	const NaturalLoop* innermost = nullptr;
	for (const auto& loop: graph.natural_loops) {
		if (Contains(loop.body_blocks, block_id) &&
		    (innermost == nullptr || loop.body_blocks.size() < innermost->body_blocks.size())) {
			innermost = &loop;
		}
	}
	return innermost;
}

bool IsInsideLoopConstruct(const Graph& graph, const NaturalLoop& loop, uint32_t block_id) {
	return block_id != UINT32_MAX && block_id != loop.merge && block_id != loop.continue_block &&
	       graph.Dominates(loop.header, block_id) && !graph.Dominates(loop.merge, block_id);
}

bool IsLoopControlGateway(const Graph& graph, const NaturalLoop& loop, uint32_t block_id) {
	const auto* block = graph.FindBlock(block_id);
	if (block == nullptr || block->terminator.kind != TerminatorKind::ConditionalBranch) {
		return false;
	}
	const auto is_control_target = [&](uint32_t target) {
		return target == loop.merge || target == loop.continue_block;
	};
	return is_control_target(block->terminator.true_block) &&
	       is_control_target(block->terminator.false_block);
}

uint32_t FindSelectionMerge(const Graph& graph, const BasicBlock& block) {
	const auto  global_merge = graph.FindNearestCommonPostDominator(block.terminator.true_block,
	                                                                block.terminator.false_block);
	const auto* loop         = FindInnermostContainingLoop(graph, block.id);
	if (loop == nullptr) {
		return global_merge;
	}

	// A selection can join at the loop-control gateway even when another arm breaks directly to
	// the enclosing loop merge. Global post-dominance sees that break and incorrectly expands the
	// selection through the backedge.
	const auto true_target  = block.terminator.true_block;
	const auto false_target = block.terminator.false_block;
	if (IsLoopControlGateway(graph, *loop, true_target) && graph.Dominates(block.id, true_target) &&
	    IsInsideLoopConstruct(graph, *loop, false_target)) {
		return true_target;
	}
	if (IsLoopControlGateway(graph, *loop, false_target) &&
	    graph.Dominates(block.id, false_target) &&
	    IsInsideLoopConstruct(graph, *loop, true_target)) {
		return false_target;
	}
	return global_merge;
}

bool IsInnermostLoopControlConditional(const Graph& graph, const BasicBlock& block) {
	if (block.terminator.kind != TerminatorKind::ConditionalBranch) {
		return false;
	}
	const auto* loop = FindInnermostContainingLoop(graph, block.id);
	if (loop == nullptr || loop->merge == UINT32_MAX || loop->continue_block == UINT32_MAX) {
		return false;
	}
	const auto true_target  = block.terminator.true_block;
	const auto false_target = block.terminator.false_block;
	if (block.id == loop->continue_block) {
		const auto is_repeat_target = [&](uint32_t target) {
			return target == loop->header || target == loop->merge;
		};
		return is_repeat_target(true_target) && is_repeat_target(false_target);
	}
	const auto is_control_target = [&](uint32_t target) {
		return target == loop->merge || target == loop->continue_block;
	};
	return (is_control_target(true_target) &&
	        (is_control_target(false_target) ||
	         IsInsideLoopConstruct(graph, *loop, false_target))) ||
	       (is_control_target(false_target) && IsInsideLoopConstruct(graph, *loop, true_target));
}

bool MergeLeavesContainingLoop(const Graph& graph, uint32_t header, uint32_t merge) {
	for (const auto& loop: graph.natural_loops) {
		if (loop.header != header && IsInsideLoopConstruct(graph, loop, header) &&
		    !IsInsideLoopConstruct(graph, loop, merge)) {
			return true;
		}
	}
	return false;
}

bool CanonicalizeNaturalLoops(Graph& graph, std::string* error) {
	const auto rewrite_budget = graph.blocks.size() * 2u + 16u;
	for (size_t rewrite = 0; rewrite < rewrite_budget; rewrite++) {
		bool changed = false;
		for (const auto& loop: graph.natural_loops) {
			std::vector<uint32_t> latches;
			for (const auto& edge: graph.back_edges) {
				if (edge.to == loop.header) {
					AddUnique(latches, edge.from);
				}
			}
			if (latches.size() <= 1u) {
				continue;
			}

			const auto continue_block = AppendSyntheticBranchBlock(graph, loop.header);
			for (auto latch: latches) {
				auto* block = graph.FindBlock(latch);
				if (block != nullptr) {
					ReplaceValue(block->successors, loop.header, continue_block);
					ReplaceTerminatorTarget(block->terminator, loop.header, continue_block);
				}
			}
			RebuildPredecessors(graph);
			RecomputeAnalyses(graph);
			changed = true;
			break;
		}
		if (changed) {
			continue;
		}

		for (const auto& loop: graph.natural_loops) {
			const auto* header                 = graph.FindBlock(loop.header);
			const auto  is_loop_control_target = [&](uint32_t target) {
				return target == loop.merge || target == loop.continue_block;
			};
			if (header == nullptr || header->terminator.kind != TerminatorKind::ConditionalBranch ||
			    is_loop_control_target(header->terminator.true_block) ||
			    is_loop_control_target(header->terminator.false_block) ||
			    !Contains(loop.body_blocks, header->terminator.true_block) ||
			    !Contains(loop.body_blocks, header->terminator.false_block)) {
				continue;
			}

			IsolateSemanticLoopHeader(graph, loop.header);
			changed = true;
			break;
		}
		if (!changed) {
			return true;
		}
	}

	SetFailure(graph, FailureKind::StructuredControlFlow, graph.entry_block,
	           "CFG loop canonicalization exceeded rewrite budget", error);
	return false;
}

bool SplitSharedMergeBlock(Graph& graph, uint32_t merge,
                           const std::vector<uint32_t>& construct_blocks,
                           bool                         force_split = false) {
	if (merge == UINT32_MAX || merge >= graph.blocks.size() || Contains(construct_blocks, merge)) {
		return false;
	}

	const auto* merge_block = graph.FindBlock(merge);
	if (merge_block == nullptr) {
		return false;
	}

	std::vector<uint32_t> construct_predecessors;
	std::vector<uint32_t> external_predecessors;
	for (auto pred: merge_block->predecessors) {
		if (Contains(construct_blocks, pred)) {
			AddUnique(construct_predecessors, pred);
		} else {
			AddUnique(external_predecessors, pred);
		}
	}

	if (construct_predecessors.empty() || (!force_split && external_predecessors.empty())) {
		return false;
	}
	const auto synthetic_merge = AppendSyntheticBranchBlock(graph, merge);
	auto*      synthetic_block = graph.FindBlock(synthetic_merge);
	if (synthetic_block != nullptr) {
		synthetic_block->predecessors = construct_predecessors;
		SortUnique(synthetic_block->predecessors);
	}

	for (auto pred: construct_predecessors) {
		auto* block = graph.FindBlock(pred);
		if (block == nullptr) {
			continue;
		}
		ReplaceValue(block->successors, merge, synthetic_merge);
		ReplaceTerminatorTarget(block->terminator, merge, synthetic_merge);
	}

	auto* old_merge = graph.FindBlock(merge);
	if (old_merge != nullptr) {
		for (auto pred: construct_predecessors) {
			RemoveValue(old_merge->predecessors, pred);
		}
		AddUnique(old_merge->predecessors, synthetic_merge);
		SortUnique(old_merge->predecessors);
	}

	MoveBlockBefore(graph, synthetic_merge, merge);
	return true;
}

bool SplitOneLoopMerge(Graph& graph) {
	const auto& loops = graph.natural_loops;
	for (const auto& loop: loops) {
		const auto construct_blocks = DominatedBlocks(graph, loop.header, loop.merge);
		const auto force_split      = MergeLeavesContainingLoop(graph, loop.header, loop.merge);
		if (SplitSharedMergeBlock(graph, loop.merge, construct_blocks, force_split)) {
			return true;
		}
	}
	return false;
}

std::vector<uint32_t> SelectionRegion(const Graph& graph, const BasicBlock& header,
                                      uint32_t merge) {
	std::vector<uint32_t> region;
	std::vector<uint32_t> pending = {header.terminator.true_block, header.terminator.false_block};
	const auto*           loop    = FindInnermostContainingLoop(graph, header.id);
	while (!pending.empty()) {
		const auto block_id = pending.back();
		pending.pop_back();
		if (block_id == merge || Contains(region, block_id) ||
		    (loop != nullptr && (block_id == loop->merge || block_id == loop->continue_block))) {
			continue;
		}
		const auto* block = graph.FindBlock(block_id);
		if (block == nullptr) {
			continue;
		}
		AddUnique(region, block_id);
		pending.insert(pending.end(), block->successors.begin(), block->successors.end());
	}
	SortUnique(region);
	return region;
}

bool SplitOneSelectionMerge(Graph& graph, std::string* error) {
	std::vector<uint32_t> loop_headers;
	loop_headers.reserve(graph.natural_loops.size());
	for (const auto& loop: graph.natural_loops) {
		AddUnique(loop_headers, loop.header);
	}

	std::vector<uint32_t> selection_headers;
	for (const auto& block: graph.blocks) {
		if (block.terminator.kind == TerminatorKind::ConditionalBranch &&
		    !Contains(loop_headers, block.id)) {
			selection_headers.push_back(block.id);
		}
	}
	std::sort(selection_headers.begin(), selection_headers.end(), [&](uint32_t lhs, uint32_t rhs) {
		const auto* lhs_block = graph.FindBlock(lhs);
		const auto* rhs_block = graph.FindBlock(rhs);
		const auto  lhs_depth = lhs_block != nullptr ? lhs_block->dominators.size() : 0u;
		const auto  rhs_depth = rhs_block != nullptr ? rhs_block->dominators.size() : 0u;
		return lhs_depth != rhs_depth ? lhs_depth > rhs_depth : lhs < rhs;
	});

	for (const auto block_id: selection_headers) {
		const auto* block = graph.FindBlock(block_id);
		if (block == nullptr) {
			continue;
		}
		if (IsInnermostLoopControlConditional(graph, *block)) {
			continue;
		}

		const auto merge = FindSelectionMerge(graph, *block);
		if (merge == UINT32_MAX || graph.FindBlock(merge) == nullptr) {
			continue;
		}
		const auto region   = SelectionRegion(graph, *block, merge);
		const auto external = std::find_if(region.begin(), region.end(), [&](uint32_t member) {
			return !graph.Dominates(block_id, member);
		});
		if (external != region.end()) {
			SetFailure(
			    graph, FailureKind::StructuredControlFlow, block_id,
			    fmt::format("selection header block {} has externally entered region block {}; "
			                "semantic block cloning is disabled",
			                block_id, *external),
			    error);
			return false;
		}
		const auto construct_blocks = DominatedBlocks(graph, block_id, merge);
		const auto force_split      = MergeLeavesContainingLoop(graph, block_id, merge);
		if (SplitSharedMergeBlock(graph, merge, construct_blocks, force_split)) {
			return true;
		}
	}
	return false;
}

bool SplitSharedMergeBlocks(Graph& graph, std::string* error) {
	const auto original_block_count = static_cast<uint32_t>(graph.blocks.size());
	const auto split_budget         = std::max<uint32_t>(16u, original_block_count * 4u);
	for (uint32_t splits = 0; splits < split_budget; splits++) {
		if (!SplitOneLoopMerge(graph) && !SplitOneSelectionMerge(graph, error)) {
			return !graph.unsupported;
		}
		RebuildPredecessors(graph);
		RecomputeAnalyses(graph);
	}
	SetFailure(graph, FailureKind::StructuredControlFlow, graph.entry_block,
	           fmt::format("CFG shared merge splitting exceeded budget: original_blocks={} "
	                       "current_blocks={} split_budget={}",
	                       original_block_count, static_cast<uint64_t>(graph.blocks.size()),
	                       split_budget),
	           error);
	return false;
}

void ClearStructuredTerminators(Graph& graph) {
	for (auto& block: graph.blocks) {
		block.terminator.merge_block    = UINT32_MAX;
		block.terminator.continue_block = UINT32_MAX;
		block.terminator.loop_header    = false;
	}
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

bool Graph::PostDominates(uint32_t post_dominator, uint32_t block) const {
	const auto* target = FindBlock(block);
	return target != nullptr && Contains(target->post_dominators, post_dominator);
}

uint32_t Graph::FindNearestCommonPostDominator(uint32_t block_a, uint32_t block_b) const {
	const auto* a = FindBlock(block_a);
	const auto* b = FindBlock(block_b);
	if (a == nullptr || b == nullptr) {
		return UINT32_MAX;
	}

	const auto common = IntersectSorted(a->post_dominators, b->post_dominators);
	for (auto candidate: common) {
		bool nearest = true;
		for (auto other: common) {
			if (other != candidate && !PostDominates(other, candidate)) {
				nearest = false;
				break;
			}
		}
		if (nearest) {
			return candidate;
		}
	}
	return common.empty() ? UINT32_MAX : common.front();
}

namespace {

struct GotoRouteBlocks {
	uint32_t                before = UINT32_MAX;
	std::array<uint32_t, 5> blocks {};
};

void PlaceGotoRouteBlocks(Graph& graph, uint32_t original_block_count,
                          const GotoRouteBlocks& route) {
	std::vector<BasicBlock> old_blocks = std::move(graph.blocks);
	std::vector<BasicBlock> new_blocks;
	new_blocks.reserve(old_blocks.size());
	for (uint32_t block_id = 0; block_id < original_block_count; block_id++) {
		if (block_id == route.before) {
			for (const auto route_block: route.blocks) {
				new_blocks.push_back(std::move(old_blocks[route_block]));
			}
		}
		new_blocks.push_back(std::move(old_blocks[block_id]));
	}

	std::vector<uint32_t> id_map(new_blocks.size(), UINT32_MAX);
	for (uint32_t block_id = 0; block_id < new_blocks.size(); block_id++) {
		id_map[new_blocks[block_id].id] = block_id;
	}

	graph.blocks      = std::move(new_blocks);
	graph.entry_block = RemapId(graph.entry_block, id_map);
	for (auto& block: graph.blocks) {
		block.id = RemapId(block.id, id_map);
		RemapIds(block.predecessors, id_map);
		RemapIds(block.successors, id_map);
		RemapIds(block.dominators, id_map);
		RemapIds(block.post_dominators, id_map);
		block.terminator.true_block     = RemapId(block.terminator.true_block, id_map);
		block.terminator.false_block    = RemapId(block.terminator.false_block, id_map);
		block.terminator.merge_block    = RemapId(block.terminator.merge_block, id_map);
		block.terminator.continue_block = RemapId(block.terminator.continue_block, id_map);
	}
}

uint32_t AppendGotoSelectBlock(Graph& graph, uint32_t route_variable, uint32_t true_target,
                               uint32_t false_target) {
	const auto* target = graph.FindBlock(false_target);
	BasicBlock  block;
	block.id                       = static_cast<uint32_t>(graph.blocks.size());
	block.start_pc                 = target != nullptr ? target->start_pc : 0u;
	block.end_pc                   = block.start_pc;
	block.inst_begin               = target != nullptr ? target->inst_begin : 0u;
	block.inst_end                 = block.inst_begin;
	block.successors               = {true_target, false_target};
	block.terminator.kind          = TerminatorKind::ConditionalBranch;
	block.terminator.condition     = BranchCondition::GotoVariable;
	block.terminator.true_block    = true_target;
	block.terminator.false_block   = false_target;
	block.terminator.goto_variable = route_variable;
	graph.blocks.push_back(std::move(block));
	return graph.blocks.back().id;
}

uint32_t AppendGotoSetBlock(Graph& graph, uint32_t route_variable, bool value, uint32_t target) {
	const auto block_id             = AppendSyntheticBranchBlock(graph, target);
	auto*      block                = graph.FindBlock(block_id);
	block->terminator.goto_variable = route_variable;
	block->terminator.goto_value    = value ? 1 : 0;
	return block_id;
}

// Turn H0 -> shared/H1, H1 -> shared/other into nested selections whose empty
// forwarding blocks set one typed SSA route value. Guest semantic blocks remain unique.
bool RouteOneSharedArm(Graph& graph, uint32_t original_block_count, uint32_t outer_id,
                       uint32_t route_variable, GotoRouteBlocks& route) {
	auto* outer = graph.FindBlock(outer_id);
	if (outer == nullptr || outer->inst_begin == outer->inst_end ||
	    outer->terminator.kind != TerminatorKind::ConditionalBranch) {
		return false;
	}

	for (const bool inner_is_true: {true, false}) {
		const auto inner_id =
		    inner_is_true ? outer->terminator.true_block : outer->terminator.false_block;
		const auto shared =
		    inner_is_true ? outer->terminator.false_block : outer->terminator.true_block;
		const auto* inner = graph.FindBlock(inner_id);
		if (inner == nullptr || inner->inst_begin == inner->inst_end ||
		    inner->predecessors != std::vector<uint32_t> {outer_id} ||
		    inner->terminator.kind != TerminatorKind::ConditionalBranch) {
			continue;
		}

		const bool true_is_shared  = inner->terminator.true_block == shared;
		const bool false_is_shared = inner->terminator.false_block == shared;
		if (true_is_shared == false_is_shared) {
			continue;
		}
		const auto other =
		    true_is_shared ? inner->terminator.false_block : inner->terminator.true_block;
		if (other == outer_id || other == inner_id || other == shared) {
			continue;
		}
		const auto first_arm = std::min(shared, other);
		if (first_arm >= original_block_count || outer_id >= inner_id || inner_id >= first_arm) {
			continue;
		}
		if (graph.FindNearestCommonPostDominator(shared, other) == UINT32_MAX) {
			continue;
		}

		const auto route_select = AppendGotoSelectBlock(graph, route_variable, other, shared);
		const auto inner_merge  = AppendSyntheticBranchBlock(graph, route_select);
		const auto outer_shared = AppendGotoSetBlock(graph, route_variable, false, route_select);
		const auto inner_shared = AppendGotoSetBlock(graph, route_variable, false, inner_merge);
		const auto inner_other  = AppendGotoSetBlock(graph, route_variable, true, inner_merge);

		outer                         = graph.FindBlock(outer_id);
		auto* mutable_inner           = graph.FindBlock(inner_id);
		outer->terminator.true_block  = inner_is_true ? inner_id : outer_shared;
		outer->terminator.false_block = inner_is_true ? outer_shared : inner_id;
		outer->successors = {outer->terminator.true_block, outer->terminator.false_block};
		mutable_inner->terminator.true_block  = true_is_shared ? inner_shared : inner_other;
		mutable_inner->terminator.false_block = true_is_shared ? inner_other : inner_shared;
		mutable_inner->successors             = {mutable_inner->terminator.true_block,
		                                         mutable_inner->terminator.false_block};
		route = {first_arm, {inner_shared, inner_other, inner_merge, outer_shared, route_select}};
		return true;
	}
	return false;
}

bool RouteSharedSelectionArms(Graph& graph) {
	if (graph.irreducible) {
		return false;
	}
	const auto route_budget = static_cast<uint32_t>(graph.blocks.size());
	bool       found        = false;
	for (uint32_t route_variable = 0; route_variable < route_budget; route_variable++) {
		const auto block_count = static_cast<uint32_t>(graph.blocks.size());
		bool       routed      = false;
		for (uint32_t block_id = 0; block_id < block_count; block_id++) {
			GotoRouteBlocks route;
			if (!RouteOneSharedArm(graph, block_count, block_id, route_variable, route)) {
				continue;
			}
			PlaceGotoRouteBlocks(graph, block_count, route);
			RebuildPredecessors(graph);
			RecomputeAnalyses(graph);
			found  = true;
			routed = true;
			break;
		}
		if (!routed) {
			return found;
		}
	}
	return false;
}

} // namespace

bool BuildGraph(const Decoder::Program& program, Graph& graph, std::string* error) {
	graph = {};

	if (program.instructions.empty()) {
		SetError(error, "cannot build CFG for empty shader");
		return false;
	}

	const auto first_pc = program.instructions.front().pc;
	const auto end_pc   = ProgramEndPc(program);

	std::set<uint32_t> instruction_pcs;
	for (const auto& inst: program.instructions) {
		instruction_pcs.insert(inst.pc);
		if (inst.opcode == Opcode::UNSUPPORTED) {
			SetFailure(graph, FailureKind::UnsupportedInstruction, UINT32_MAX,
			           fmt::format("unsupported decoded instruction in CFG at pc 0x{:08x}: {}",
			                       inst.pc, Decoder::InstructionToString(inst).c_str()),
			           error);
			return false;
		}
	}

	std::set<uint32_t> labels;
	labels.insert(first_pc);
	labels.insert(end_pc);

	std::map<uint32_t, SetpcTargetInfo> setpc_targets;
	bool                                indirect_setpc = false;
	for (uint32_t i = 0; i < program.instructions.size(); i++) {
		const auto& inst    = program.instructions[i];
		const auto  next_pc = InstructionEndPc(inst);
		if (IsBranch(inst.opcode)) {
			if (!IsValidTarget(inst.branch_target, instruction_pcs, first_pc, end_pc)) {
				SetFailure(graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
				           fmt::format("branch at pc 0x{:08x} targets invalid pc 0x{:08x}", inst.pc,
				                       inst.branch_target),
				           error);
				return false;
			}
			labels.insert(inst.branch_target);
			if (next_pc <= end_pc) {
				labels.insert(next_pc);
			}
		} else if (inst.opcode == Opcode::S_SETPC_B64) {
			SetpcTargetInfo target_info;
			if (!ResolveSetpcTargets(program, i, target_info)) {
				SetFailure(graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
				           fmt::format("unsupported dynamic S_SETPC_B64 at pc 0x{:08x}", inst.pc),
				           error);
				return false;
			}
			const auto& target_pcs = target_info.indirect
			                             ? target_info.target_pcs
			                             : std::vector<uint32_t> {target_info.target};
			for (const auto target: target_pcs) {
				if (!IsValidTarget(target, instruction_pcs, first_pc, end_pc)) {
					SetFailure(graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
					           fmt::format("S_SETPC_B64 at pc 0x{:08x} targets invalid pc 0x{:08x}",
					                       inst.pc, target),
					           error);
					return false;
				}
				labels.insert(target);
			}
			indirect_setpc = indirect_setpc || target_info.indirect;
			setpc_targets.emplace(inst.pc, std::move(target_info));
			if (next_pc <= end_pc) {
				labels.insert(next_pc);
			}
		} else if (inst.opcode == Opcode::S_ENDPGM) {
			labels.insert(next_pc);
		}
	}

	std::vector<uint32_t> sorted_labels(labels.begin(), labels.end());
	std::sort(sorted_labels.begin(), sorted_labels.end());
	for (uint32_t i = 0; i < sorted_labels.size(); i++) {
		const auto start = sorted_labels[i];
		if (start > end_pc) {
			continue;
		}
		if (start != end_pc && !instruction_pcs.contains(start)) {
			SetFailure(graph, FailureKind::InvalidLabel, UINT32_MAX,
			           fmt::format("CFG label does not start on an instruction: 0x{:08x}", start),
			           error);
			return false;
		}

		BasicBlock block;
		block.id         = static_cast<uint32_t>(graph.blocks.size());
		block.start_pc   = start;
		block.end_pc     = i + 1u < sorted_labels.size() ? sorted_labels[i + 1u] : end_pc;
		block.inst_begin = static_cast<uint32_t>(
		    std::lower_bound(program.instructions.begin(), program.instructions.end(), start,
		                     [](const Instruction& inst, uint32_t pc) { return inst.pc < pc; }) -
		    program.instructions.begin());
		block.inst_end = static_cast<uint32_t>(
		    std::lower_bound(program.instructions.begin(), program.instructions.end(), block.end_pc,
		                     [](const Instruction& inst, uint32_t pc) { return inst.pc < pc; }) -
		    program.instructions.begin());
		graph.blocks.push_back(std::move(block));
	}

	graph.entry_block = 0;

	std::map<uint32_t, uint32_t> pc_to_block;
	for (const auto& block: graph.blocks) {
		pc_to_block.emplace(block.start_pc, block.id);
	}

	for (auto& block: graph.blocks) {
		block.terminator = {};
		if (block.inst_begin == block.inst_end) {
			block.terminator.kind = TerminatorKind::Return;
			continue;
		}

		const auto& last    = program.instructions[block.inst_end - 1u];
		const auto  next_pc = InstructionEndPc(last);
		if (last.opcode == Opcode::S_ENDPGM) {
			block.terminator.kind       = TerminatorKind::Branch;
			block.terminator.condition  = BranchCondition::Always;
			block.terminator.true_block = pc_to_block.at(end_pc);
		} else if (last.opcode == Opcode::S_SETPC_B64) {
			const auto& target_info = setpc_targets.at(last.pc);
			if (target_info.indirect) {
				block.terminator.kind                   = TerminatorKind::IndirectBranch;
				block.terminator.condition              = BranchCondition::Always;
				block.terminator.indirect_pc_sgpr       = target_info.pc_sgpr;
				block.terminator.indirect_selector_code = target_info.selector_code;
				for (const auto target_pc: target_info.target_pcs) {
					block.terminator.indirect_target_pcs.push_back(target_pc);
					block.terminator.indirect_targets.push_back(pc_to_block.at(target_pc));
				}
				const auto selector_count = std::min(target_info.selector_values.size(),
				                                     target_info.selector_target_pcs.size());
				for (uint32_t i = 0; i < selector_count; i++) {
					block.terminator.indirect_selector_values.push_back(
					    target_info.selector_values[i]);
					block.terminator.indirect_selector_targets.push_back(
					    pc_to_block.at(target_info.selector_target_pcs[i]));
				}
				if (target_info.table_load_pc != UINT32_MAX) {
					AddUnique(graph.code_table_load_pcs, target_info.table_load_pc);
				}
			} else {
				block.terminator.kind       = TerminatorKind::Branch;
				block.terminator.condition  = BranchCondition::Always;
				block.terminator.true_block = pc_to_block.at(target_info.target);
			}
		} else if (IsUnconditionalBranch(last.opcode)) {
			block.terminator.kind       = TerminatorKind::Branch;
			block.terminator.condition  = BranchCondition::Always;
			block.terminator.true_block = pc_to_block.at(last.branch_target);
		} else if (IsConditionalBranch(last.opcode)) {
			block.terminator.kind       = TerminatorKind::ConditionalBranch;
			block.terminator.condition  = ConditionForOpcode(last.opcode);
			block.terminator.true_block = pc_to_block.at(last.branch_target);
			const auto fallthrough      = pc_to_block.find(next_pc);
			if (fallthrough == pc_to_block.end()) {
				SetFailure(graph, FailureKind::MissingFallthrough, block.id,
				           fmt::format("conditional branch at pc 0x{:08x} has no fallthrough block",
				                       last.pc),
				           error);
				return false;
			}
			block.terminator.false_block = fallthrough->second;
		} else {
			auto next = pc_to_block.find(block.end_pc);
			if (next != pc_to_block.end() && block.end_pc != block.start_pc) {
				block.terminator.kind       = TerminatorKind::Branch;
				block.terminator.condition  = BranchCondition::Always;
				block.terminator.true_block = next->second;
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
	SortUnique(graph.code_table_load_pcs);

	ComputeDominators(graph);
	ComputePostDominators(graph);
	ComputeBackEdges(graph);
	ComputeNaturalLoops(graph);
	ComputeComponents(graph);

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

	return true;
}

namespace {

bool StructurizeImpl(Graph& graph, std::string* error) {
	if (graph.unsupported || graph.irreducible) {
		if (graph.unsupported_reason.empty()) {
			graph.unsupported_reason = "unsupported CFG";
		}
		SetError(error, graph.unsupported_reason);
		return false;
	}

	if (!CanonicalizeNaturalLoops(graph, error)) {
		return false;
	}
	if (!SplitSharedMergeBlocks(graph, error)) {
		return false;
	}
	ClearStructuredTerminators(graph);

	std::map<uint32_t, uint32_t> merge_headers;
	auto                         reserve_merge_block = [&](uint32_t header, uint32_t merge) {
		const auto [it, inserted] = merge_headers.emplace(merge, header);
		if (!inserted && it->second != header) {
			SetFailure(
			    graph, FailureKind::StructuredControlFlow, header,
			    fmt::format(
			        "duplicate structured merge block {} for header {} (already used by header {})",
			        merge, header, it->second),
			    error);
			return false;
		}
		return true;
	};

	for (const auto& loop: graph.natural_loops) {
		auto* header = graph.FindBlock(loop.header);
		if (header == nullptr || loop.merge == UINT32_MAX || loop.continue_block == UINT32_MAX) {
			SetFailure(
			    graph, FailureKind::StructuredControlFlow, loop.header,
			    fmt::format("loop at block {} has no structured merge/continue", loop.header),
			    error);
			return false;
		}
		if (!reserve_merge_block(loop.header, loop.merge)) {
			return false;
		}
		header->terminator.loop_header    = true;
		header->terminator.merge_block    = loop.merge;
		header->terminator.continue_block = loop.continue_block;
	}

	for (auto& block: graph.blocks) {
		if (block.terminator.kind != TerminatorKind::ConditionalBranch ||
		    block.terminator.loop_header) {
			continue;
		}
		if (IsInnermostLoopControlConditional(graph, block)) {
			continue;
		}

		const auto merge = FindSelectionMerge(graph, block);
		if (merge == UINT32_MAX) {
			SetFailure(graph, FailureKind::StructuredControlFlow, block.id,
			           fmt::format("conditional block {} has no structured merge", block.id),
			           error);
			return false;
		}

		if (!reserve_merge_block(block.id, merge)) {
			return false;
		}
		block.terminator.merge_block = merge;
	}

	return true;
}

} // namespace

bool Structurize(Graph& graph, std::string* error) {
	Graph original = graph;
	if (StructurizeImpl(graph, error)) {
		return true;
	}
	if (!RouteSharedSelectionArms(original)) {
		return false;
	}
	Graph       failed_graph = std::move(graph);
	std::string failed_error = error != nullptr ? *error : std::string {};
	graph                    = std::move(original);
	if (error != nullptr) {
		error->clear();
	}
	if (StructurizeImpl(graph, error)) {
		return true;
	}
	graph = std::move(failed_graph);
	if (error != nullptr) {
		*error = std::move(failed_error);
	}
	return false;
}

bool IsolateLoopHeader(Graph& graph, uint32_t header_id, std::string* error) {
	const auto loop =
	    std::find_if(graph.natural_loops.begin(), graph.natural_loops.end(),
	                 [header_id](const auto& value) { return value.header == header_id; });
	if (loop == graph.natural_loops.end()) {
		SetFailure(graph, FailureKind::StructuredControlFlow, header_id,
		           fmt::format("block {} is not a natural loop header", header_id), error);
		return false;
	}

	ClearStructuredTerminators(graph);
	if (!IsolateSemanticLoopHeader(graph, header_id)) {
		SetFailure(graph, FailureKind::StructuredControlFlow, header_id,
		           fmt::format("loop header block {} has no semantic instructions", header_id),
		           error);
		return false;
	}

	graph.unsupported   = false;
	graph.failure_kind  = FailureKind::None;
	graph.failure_block = UINT32_MAX;
	graph.unsupported_reason.clear();
	return Structurize(graph, error);
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
		case BranchCondition::GotoVariable: return "goto_variable";
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
		text += fmt::format("  dominators=[{}] post_dominators=[{}]\n",
		                    VectorToString(block.dominators).c_str(),
		                    VectorToString(block.post_dominators).c_str());
		text += fmt::format(
		    "  terminator={} condition={} true={} false={} merge={} continue={} loop_header={} "
		    "indirect_sgpr={} indirect_selector={} indirect_targets=[{}] selector_values=[{}] "
		    "goto_variable={} goto_value={}\n",
		    static_cast<uint32_t>(block.terminator.kind),
		    BranchConditionToString(block.terminator.condition).c_str(),
		    block.terminator.true_block, block.terminator.false_block, block.terminator.merge_block,
		    block.terminator.continue_block, block.terminator.loop_header ? 1u : 0u,
		    block.terminator.indirect_pc_sgpr, block.terminator.indirect_selector_code,
		    VectorToString(block.terminator.indirect_targets).c_str(),
		    VectorToString(block.terminator.indirect_selector_values).c_str(),
		    block.terminator.goto_variable, block.terminator.goto_value);
	}

	for (const auto& edge: graph.back_edges) {
		text += fmt::format("backedge {} -> {} natural={}\n", edge.from, edge.to,
		                    edge.natural ? 1u : 0u);
	}
	for (const auto& loop: graph.natural_loops) {
		text += fmt::format("loop header={} latch={} merge={} continue={} body=[{}] exits=[{}]\n",
		                    loop.header, loop.latch, loop.merge, loop.continue_block,
		                    VectorToString(loop.body_blocks).c_str(),
		                    VectorToString(loop.exit_blocks).c_str());
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
