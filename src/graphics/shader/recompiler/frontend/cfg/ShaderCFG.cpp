#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"

#include "common/assert.h"
#include "common/logging/log.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fmt/format.h>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>

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
		block.post_dominators.clear();
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

void ComputePostDominators(Graph& graph) {
	const auto count = static_cast<uint32_t>(graph.blocks.size());
	const auto all   = AllBlockIds(count);

	for (auto& block: graph.blocks) {
		block.post_dominators = block.successors.empty() ? std::vector<uint32_t> {block.id} : all;
	}

	bool changed = true;
	while (changed) {
		changed = false;
		// Post-dominance flows from successors, so a reverse sweep converges in a few passes.
		for (auto it = graph.blocks.rbegin(); it != graph.blocks.rend(); ++it) {
			auto&                 block = *it;
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

uint32_t AppendClonedSemanticBlock(Graph& graph, uint32_t source_id) {
	const auto* source = graph.FindBlock(source_id);
	if (source == nullptr) {
		return UINT32_MAX;
	}
	BasicBlock block           = *source;
	block.id                   = static_cast<uint32_t>(graph.blocks.size());
	block.predecessors.clear();
	block.dominators.clear();
	block.post_dominators.clear();
	block.terminator.loop_header    = false;
	block.terminator.merge_block    = UINT32_MAX;
	block.terminator.continue_block = UINT32_MAX;
	graph.blocks.push_back(std::move(block));
	return graph.blocks.back().id;
}

bool IsLoopHeaderOrContinue(const Graph& graph, uint32_t block_id) {
	return std::ranges::any_of(graph.natural_loops, [&](const auto& loop) {
		return block_id == loop.header || block_id == loop.continue_block;
	});
}

bool FeedsLoopHeader(const Graph& graph, uint32_t block_id) {
	const auto* block = graph.FindBlock(block_id);
	if (block == nullptr) {
		return false;
	}
	return std::ranges::any_of(block->successors, [&](uint32_t succ) {
		return std::ranges::any_of(graph.natural_loops, [&](const auto& loop) {
			return loop.header == succ;
		});
	});
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

std::string BlockLoopRole(const Graph& graph, uint32_t block_id) {
	std::string roles;
	for (const auto& loop: graph.natural_loops) {
		if (block_id == loop.header) {
			roles += fmt::format(" header#{}", loop.header);
		}
		if (block_id == loop.continue_block) {
			roles += fmt::format(" continue#{}", loop.header);
		}
		if (block_id == loop.merge) {
			roles += fmt::format(" merge#{}", loop.header);
		}
	}
	const auto* inner = FindInnermostContainingLoop(graph, block_id);
	return fmt::format("inner={} roles=[{}]",
	                   inner != nullptr ? fmt::format("{}", inner->header) : std::string("none"),
	                   roles.empty() ? " none" : roles);
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

bool HasLinearPathToTerminal(const Graph& graph, uint32_t start) {
	std::vector<bool> visited(graph.blocks.size(), false);
	for (auto block_id = start;;) {
		const auto* block = graph.FindBlock(block_id);
		if (block == nullptr || block_id >= visited.size() || visited[block_id]) {
			return false;
		}
		if (block->successors.empty()) {
			return true;
		}
		if (block->successors.size() != 1) {
			return false;
		}
		visited[block_id] = true;
		block_id          = block->successors.front();
	}
}

bool IsEnclosingLinearExit(const Graph& graph, uint32_t header, uint32_t block_id) {
	// AGC commonly lowers nested early returns through a terminal epilogue shared with an
	// enclosing conditional. Such a path is an exit boundary, not part of the inner selection.
	if (!HasLinearPathToTerminal(graph, block_id)) {
		return false;
	}
	const auto* block = graph.FindBlock(block_id);
	while (block != nullptr && graph.Dominates(header, block->id) &&
	       block->successors.size() == 1u) {
		block = graph.FindBlock(block->successors.front());
	}
	if (block == nullptr || graph.Dominates(header, block->id) ||
	    block->predecessors.empty()) {
		return false;
	}
	return std::ranges::all_of(block->predecessors, [&](uint32_t predecessor) {
		return graph.Dominates(header, predecessor) || graph.Dominates(predecessor, header);
	});
}

bool CanReachBefore(const Graph& graph, uint32_t start, uint32_t target, uint32_t stop) {
	std::vector<uint32_t> pending = {start};
	std::vector<bool>     visited(graph.blocks.size(), false);
	while (!pending.empty()) {
		const auto block_id = pending.back();
		pending.pop_back();
		if (block_id == target) {
			return true;
		}
		if (block_id == stop || block_id >= visited.size() || visited[block_id]) {
			continue;
		}
		visited[block_id] = true;
		const auto* block = graph.FindBlock(block_id);
		if (block != nullptr) {
			pending.insert(pending.end(), block->successors.begin(), block->successors.end());
		}
	}
	return false;
}

std::vector<bool> ReachableFrom(const Graph& graph, uint32_t start) {
	std::vector<bool>     seen(graph.blocks.size(), false);
	std::vector<uint32_t> pending = {start};
	while (!pending.empty()) {
		const auto block_id = pending.back();
		pending.pop_back();
		if (block_id >= seen.size() || seen[block_id]) {
			continue;
		}
		seen[block_id]    = true;
		const auto* block = graph.FindBlock(block_id);
		if (block != nullptr) {
			pending.insert(pending.end(), block->successors.begin(), block->successors.end());
		}
	}
	return seen;
}

uint32_t FindSharedContinueJoin(const Graph& graph, uint32_t header, uint32_t true_target,
                                uint32_t false_target) {
	// Both arms can return and also fall through to a later join. Returns do not
	// post-dominate, so nearest-common-post-dominator is empty; SPIR-V still allows
	// OpReturn inside a selection whose merge is that continue join.
	const auto from_true  = ReachableFrom(graph, true_target);
	const auto from_false = ReachableFrom(graph, false_target);
	std::vector<uint32_t> candidates;
	for (const auto& block: graph.blocks) {
		if (block.id == header || block.successors.empty() ||
		    block.id >= from_true.size() || block.id >= from_false.size() ||
		    !from_true[block.id] || !from_false[block.id] ||
		    !graph.Dominates(header, block.id) || IsLoopHeaderOrContinue(graph, block.id)) {
			continue;
		}
		candidates.push_back(block.id);
	}
	for (const auto candidate: candidates) {
		if (std::ranges::all_of(candidates, [&](uint32_t other) {
			    return other == candidate || graph.Dominates(candidate, other);
		    })) {
			return candidate;
		}
	}
	return UINT32_MAX;
}

uint32_t FindSelectionMerge(const Graph& graph, const BasicBlock& block) {
	const auto  global_merge = graph.FindNearestCommonPostDominator(block.terminator.true_block,
	                                                                block.terminator.false_block);
	const auto* loop         = FindInnermostContainingLoop(graph, block.id);
	if (loop == nullptr) {
		const auto* global_block = graph.FindBlock(global_merge);
		const auto  true_target  = block.terminator.true_block;
		const auto  false_target = block.terminator.false_block;
		if (global_block == nullptr || global_block->successors.empty()) {
			const bool false_reaches_true =
			    CanReachBefore(graph, false_target, true_target, global_merge);
			const bool true_reaches_false =
			    CanReachBefore(graph, true_target, false_target, global_merge);
			if (false_reaches_true != true_reaches_false) {
				return false_reaches_true ? true_target : false_target;
			}
			if (global_merge == UINT32_MAX) {
				// An enclosing selection's shared return needs its own inner merge
				// gateway; the other arm can return directly without joining live state.
				if (IsEnclosingLinearExit(graph, block.id, true_target)) {
					return true_target;
				}
				if (IsEnclosingLinearExit(graph, block.id, false_target)) {
					return false_target;
				}
				// A return can leave a selection without reaching its merge. Keep the
				// continuing arm as the merge instead of joining live state with a return.
				// The continue arm need not be dominated yet: a shared tail is split
				// into a private merge by SplitSharedMergeBlock.
				if (HasLinearPathToTerminal(graph, true_target)) {
					return false_target;
				}
				if (HasLinearPathToTerminal(graph, false_target)) {
					return true_target;
				}
				const auto continue_join =
				    FindSharedContinueJoin(graph, block.id, true_target, false_target);
				if (continue_join != UINT32_MAX) {
					return continue_join;
				}
			}
		}
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
	const bool true_in_body  = Contains(loop->body_blocks, true_target);
	const bool false_in_body = Contains(loop->body_blocks, false_target);
	if (true_in_body != false_in_body) {
		return true;
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

bool CanonicalizeNaturalLoops(Graph& graph) {
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
	           "CFG loop canonicalization exceeded rewrite budget");
	return false;
}

bool IsolateSemanticLoopHeaders(Graph& graph) {
	const auto isolation_budget = graph.natural_loops.size() + 1u;
	for (size_t isolation = 0; isolation < isolation_budget; isolation++) {
		const auto loop = std::find_if(
		    graph.natural_loops.begin(), graph.natural_loops.end(), [&](const auto& value) {
			    const auto* header = graph.FindBlock(value.header);
			    return header != nullptr && header->inst_begin != header->inst_end;
		    });
		if (loop == graph.natural_loops.end()) {
			return true;
		}

		// SPIR-V requires OpLoopMerge and its branch to remain in the loop header's
		// physical block. Keep that header as a dedicated control node, exactly like
		// a separate Loop node in a structured AST. Guest instructions live
		// in the body so later translation may introduce bounds/EXEC control flow without
		// displacing OpLoopMerge into a helper-created block.
		if (!IsolateSemanticLoopHeader(graph, loop->header)) {
			SetFailure(graph, FailureKind::StructuredControlFlow, loop->header,
			           fmt::format("failed to isolate semantic loop header {}", loop->header));
			return false;
		}
	}

	SetFailure(graph, FailureKind::StructuredControlFlow, graph.entry_block,
	           "CFG semantic loop-header isolation exceeded rewrite budget");
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
	// Force-splitting an already-empty gateway just wraps the same join forever.
	if (force_split && external_predecessors.empty() &&
	    merge_block->inst_begin == merge_block->inst_end &&
	    merge_block->terminator.kind == TerminatorKind::Branch) {
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
		// A return terminates its own block; a branch to a shared return still has to
		// obey selection entry/exit rules, just like any other branch.
		AddUnique(region, block_id);
		pending.insert(pending.end(), block->successors.begin(), block->successors.end());
	}
	SortUnique(region);
	return region;
}

bool SelectionArmReaches(const Graph& graph, uint32_t arm, uint32_t target, uint32_t merge) {
	return arm == target || CanReachBefore(graph, arm, target, merge);
}

void LogCloneDecision(const Graph& graph, uint32_t header_id, uint32_t merge, uint32_t member_id,
                      const std::vector<uint32_t>& region, const char* decision,
                      const char* reason) {
	const auto* header = graph.FindBlock(header_id);
	const auto* member = graph.FindBlock(member_id);
	std::string preds;
	if (member != nullptr) {
		for (size_t i = 0; i < member->predecessors.size(); i++) {
			const auto pred      = member->predecessors[i];
			const bool internal  = pred == header_id || Contains(region, pred);
			if (i != 0) {
				preds += ' ';
			}
			preds += fmt::format("{}:{}({})", pred, internal ? "in" : "ext",
			                     BlockLoopRole(graph, pred));
		}
	}
	LOGF("ShaderCFG clone %s: header=%u member=%u merge=%u true=%u false=%u "
	     "member_loop=(%s) reason=%s preds=[%s]\n",
	     decision, header_id, member_id, merge,
	     header != nullptr ? header->terminator.true_block : UINT32_MAX,
	     header != nullptr ? header->terminator.false_block : UINT32_MAX,
	     BlockLoopRole(graph, member_id).c_str(), reason, preds.c_str());
}

bool SplitExternalSelectionEntry(Graph& graph, uint32_t header_id, uint32_t merge,
                                 const std::vector<uint32_t>& region, uint32_t member_id,
                                 bool log_clone) {
	const auto* header = graph.FindBlock(header_id);
	const auto* member = graph.FindBlock(member_id);
	if (header == nullptr || member == nullptr) {
		if (log_clone) {
			LogCloneDecision(graph, header_id, merge, member_id, region, "refuse", "missing_block");
		}
		return false;
	}

	const auto header_loop = std::find_if(
	    graph.natural_loops.begin(), graph.natural_loops.end(),
	    [&](const auto& loop) { return loop.header == member_id; });
	if (header_loop != graph.natural_loops.end()) {
		// Cloned selection copies can share one nested loop. Unify extra preheaders
		// instead of cloning the loop header (that would duplicate the back-edge).
		std::vector<uint32_t> non_latch;
		for (auto pred: member->predecessors) {
			if (pred != header_loop->continue_block && pred != header_loop->latch) {
				AddUnique(non_latch, pred);
			}
		}
		if (non_latch.size() >= 2u) {
			const auto preheader = AppendSyntheticBranchBlock(graph, member_id);
			for (auto pred: non_latch) {
				auto* block = graph.FindBlock(pred);
				if (block == nullptr) {
					continue;
				}
				ReplaceValue(block->successors, member_id, preheader);
				ReplaceTerminatorTarget(block->terminator, member_id, preheader);
			}
			MoveBlockBefore(graph, preheader, member_id);
			if (log_clone) {
				LogCloneDecision(graph, header_id, merge, member_id, region, "preheader",
				                 "loop_multi_preheader");
			}
			return true;
		}
		if (log_clone) {
			LogCloneDecision(graph, header_id, merge, member_id, region, "refuse",
			                 "loop_header_or_continue");
		}
		return false;
	}
	if (IsLoopHeaderOrContinue(graph, member_id)) {
		if (log_clone) {
			LogCloneDecision(graph, header_id, merge, member_id, region, "refuse",
			                 "loop_header_or_continue");
		}
		return false;
	}
	if (FeedsLoopHeader(graph, member_id)) {
		if (log_clone) {
			LogCloneDecision(graph, header_id, merge, member_id, region, "refuse",
			                 "loop_preheader");
		}
		return false;
	}
	// Cloning a selection/switch duplicates every nested loop it feeds and then
	// never converges (shared merge split wraps the same join forever). Only
	// linear tails — returns and single-successor blocks — are safe to copy.
	if (member->terminator.kind != TerminatorKind::Branch &&
	    member->terminator.kind != TerminatorKind::Return) {
		if (log_clone) {
			LogCloneDecision(graph, header_id, merge, member_id, region, "refuse",
			                 "non_linear_member");
		}
		return false;
	}

	const auto* member_loop = FindInnermostContainingLoop(graph, member_id);
	std::vector<uint32_t> internal_predecessors;
	std::vector<uint32_t> external_predecessors;
	for (auto pred: member->predecessors) {
		if (pred == header_id || Contains(region, pred)) {
			AddUnique(internal_predecessors, pred);
		} else {
			AddUnique(external_predecessors, pred);
		}
	}
	if (internal_predecessors.empty() || external_predecessors.empty()) {
		if (log_clone) {
			LogCloneDecision(graph, header_id, merge, member_id, region, "refuse",
			                 internal_predecessors.empty() ? "no_internal_preds"
			                                               : "no_external_preds");
		}
		return false;
	}
	const bool member_is_loop_merge =
	    member_loop != nullptr && member_id == member_loop->merge;
	if (!member_is_loop_merge && member_loop != nullptr &&
	    std::ranges::any_of(external_predecessors, [&](uint32_t pred) {
		    return FindInnermostContainingLoop(graph, pred) != member_loop;
	    })) {
		if (log_clone) {
			LogCloneDecision(graph, header_id, merge, member_id, region, "refuse",
			                 "nonlocal_loop_entry");
		}
		return false;
	}

	const bool both_arms =
	    SelectionArmReaches(graph, header->terminator.true_block, member_id, merge) &&
	    SelectionArmReaches(graph, header->terminator.false_block, member_id, merge);
	if (both_arms) {
		const auto construct_blocks = DominatedBlocks(graph, header_id, member_id);
		const auto force_split      = MergeLeavesContainingLoop(graph, header_id, member_id);
		if (SplitSharedMergeBlock(graph, member_id, construct_blocks, force_split)) {
			if (log_clone) {
				LogCloneDecision(graph, header_id, merge, member_id, region, "split_merge",
				                 "shared_merge_split");
			}
			return true;
		}
	}

	const auto clone_id = AppendClonedSemanticBlock(graph, member_id);
	if (clone_id == UINT32_MAX) {
		if (log_clone) {
			LogCloneDecision(graph, header_id, merge, member_id, region, "refuse",
			                 "clone_append_failed");
		}
		return false;
	}
	for (auto pred: internal_predecessors) {
		auto* block = graph.FindBlock(pred);
		if (block == nullptr) {
			continue;
		}
		ReplaceValue(block->successors, member_id, clone_id);
		ReplaceTerminatorTarget(block->terminator, member_id, clone_id);
	}
	MoveBlockBefore(graph, clone_id, member_id);
	if (log_clone) {
		std::string internal;
		for (size_t i = 0; i < internal_predecessors.size(); i++) {
			if (i != 0) {
				internal += ',';
			}
			internal += fmt::format("{}", internal_predecessors[i]);
		}
		LOGF("ShaderCFG clone ok: header=%u member=%u clone=%u merge=%u internal=[%s]\n", header_id,
		     member_id, clone_id, merge, internal.c_str());
	}
	return true;
}

bool SplitOneSelectionMerge(Graph& graph, bool allow_clone) {
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
			const auto* member_block = graph.FindBlock(member);
			return member_block != nullptr &&
			       std::ranges::any_of(member_block->predecessors, [&](uint32_t predecessor) {
				       return predecessor != block_id && !Contains(region, predecessor);
			       });
		});
		if (external != region.end()) {
			if (allow_clone &&
			    SplitExternalSelectionEntry(graph, block_id, merge, region, *external, true)) {
				return true;
			}
			const auto* external_block = graph.FindBlock(*external);
			const bool skippable_external =
			    IsLoopHeaderOrContinue(graph, *external) || FeedsLoopHeader(graph, *external) ||
			    (allow_clone && external_block != nullptr &&
			     external_block->terminator.kind != TerminatorKind::Branch &&
			     external_block->terminator.kind != TerminatorKind::Return);
			if (!skippable_external) {
				const auto* member = graph.FindBlock(*external);
				std::string preds;
				if (member != nullptr) {
					for (size_t i = 0; i < member->predecessors.size(); i++) {
						if (i != 0) {
							preds += ',';
						}
						preds += fmt::format("{}", member->predecessors[i]);
					}
				}
				SetFailure(
				    graph, FailureKind::StructuredControlFlow, block_id,
				    fmt::format("selection header block {} has externally entered region block {} "
				                "preds=[{}] merge={} true={} false={}",
				                block_id, *external, preds, merge, block->terminator.true_block,
				                block->terminator.false_block));
				return false;
			}
			// A nested loop in this region is shared with another path. Force-splitting
			// the merge wraps the same join forever; isolate mixed preds only, then
			// leave this header for preheader unification on a later pass.
			const auto construct_blocks = DominatedBlocks(graph, block_id, merge);
			if (SplitSharedMergeBlock(graph, merge, construct_blocks, false)) {
				return true;
			}
			continue;
		}
		const auto construct_blocks = DominatedBlocks(graph, block_id, merge);
		const auto force_split      = MergeLeavesContainingLoop(graph, block_id, merge);
		if (SplitSharedMergeBlock(graph, merge, construct_blocks, force_split)) {
			return true;
		}
	}
	return false;
}

bool SplitSharedMergeBlocks(Graph& graph, bool allow_clone) {
	const auto original_block_count = static_cast<uint32_t>(graph.blocks.size());
	const auto split_budget         = std::max<uint32_t>(16u, original_block_count * 4u);
	for (uint32_t splits = 0; splits < split_budget; splits++) {
		if (!SplitOneLoopMerge(graph) && !SplitOneSelectionMerge(graph, allow_clone)) {
			return !graph.unsupported;
		}
		RebuildPredecessors(graph);
		RecomputeAnalyses(graph);
	}
	SetFailure(graph, FailureKind::StructuredControlFlow, graph.entry_block,
	           fmt::format("CFG shared merge splitting exceeded budget: original_blocks={} "
	                       "current_blocks={} split_budget={}",
	                       original_block_count, static_cast<uint64_t>(graph.blocks.size()),
	                       split_budget));
	return false;
}

uint32_t AppendEmptyDispatchBlock(Graph& graph) {
	BasicBlock block;
	block.id                  = static_cast<uint32_t>(graph.blocks.size());
	block.terminator.kind     = TerminatorKind::Branch;
	block.terminator.condition = BranchCondition::Always;
	graph.blocks.push_back(std::move(block));
	return graph.blocks.back().id;
}

uint32_t AppendDispatchSetter(Graph& graph, uint32_t next_state, uint32_t continue_block) {
	const auto id = AppendEmptyDispatchBlock(graph);
	auto*      block = graph.FindBlock(id);
	block->terminator.kind          = TerminatorKind::Branch;
	block->terminator.dispatch_next = next_state;
	block->terminator.true_block    = continue_block;
	block->successors               = {continue_block};
	return id;
}

uint32_t AppendDispatchJoin(Graph& graph, uint32_t target) {
	return AppendDispatchSetter(graph, UINT32_MAX, target);
}

void RetargetDispatchBranch(Graph& graph, uint32_t block_id, uint32_t target) {
	auto* block = graph.FindBlock(block_id);
	block->terminator.true_block = target;
	block->successors            = {target};
}

void RemapTerminatorIds(Terminator& terminator, const std::vector<uint32_t>& id_map) {
	terminator.true_block     = RemapId(terminator.true_block, id_map);
	terminator.false_block    = RemapId(terminator.false_block, id_map);
	terminator.merge_block    = RemapId(terminator.merge_block, id_map);
	terminator.continue_block = RemapId(terminator.continue_block, id_map);
	terminator.dispatch_next  = RemapId(terminator.dispatch_next, id_map);
	for (auto& target: terminator.indirect_targets) {
		target = RemapId(target, id_map);
	}
	for (auto& target: terminator.indirect_selector_targets) {
		target = RemapId(target, id_map);
	}
	if (terminator.kind == TerminatorKind::DispatchSwitch) {
		for (auto& value: terminator.indirect_selector_values) {
			value = RemapId(value, id_map);
		}
	}
}

void CompactBlocksInOrder(Graph& graph, const std::vector<uint32_t>& order) {
	std::vector<BasicBlock> old_blocks = std::move(graph.blocks);
	std::vector<BasicBlock> new_blocks;
	new_blocks.reserve(order.size());
	for (auto old_id: order) {
		if (old_id >= old_blocks.size()) {
			continue;
		}
		new_blocks.push_back(std::move(old_blocks[old_id]));
	}

	std::vector<uint32_t> id_map(old_blocks.size(), UINT32_MAX);
	for (uint32_t i = 0; i < new_blocks.size(); i++) {
		id_map[new_blocks[i].id] = i;
	}

	graph.blocks        = std::move(new_blocks);
	graph.entry_block   = RemapId(graph.entry_block, id_map);
	graph.failure_block = RemapId(graph.failure_block, id_map);
	for (auto& block: graph.blocks) {
		block.id = RemapId(block.id, id_map);
		block.predecessors.clear();
		block.dominators.clear();
		block.post_dominators.clear();
		RemapTerminatorIds(block.terminator, id_map);
	}
}

void RebuildSuccessorsFromTerminators(Graph& graph) {
	for (auto& block: graph.blocks) {
		block.successors.clear();
		switch (block.terminator.kind) {
			case TerminatorKind::Branch:
				AddUnique(block.successors, block.terminator.true_block);
				break;
			case TerminatorKind::ConditionalBranch:
				AddUnique(block.successors, block.terminator.true_block);
				AddUnique(block.successors, block.terminator.false_block);
				break;
			case TerminatorKind::IndirectBranch:
			case TerminatorKind::DispatchSwitch:
				for (auto target: block.terminator.indirect_targets) {
					AddUnique(block.successors, target);
				}
				if (block.terminator.kind == TerminatorKind::DispatchSwitch &&
				    block.terminator.true_block != UINT32_MAX) {
					AddUnique(block.successors, block.terminator.true_block);
				}
				break;
			case TerminatorKind::Return:
			case TerminatorKind::Unsupported: break;
		}
	}
}

void ClearGraphFailure(Graph& graph) {
	graph.unsupported        = false;
	graph.irreducible        = false;
	graph.failure_kind       = FailureKind::None;
	graph.failure_block      = UINT32_MAX;
	graph.unsupported_reason.clear();
}

void ClearStructuredTerminators(Graph& graph);

bool RunDispatcherFull(Graph& graph, const StructurizeOptions& options, StructurizeStats& stats) {
	if (graph.blocks.empty() || graph.entry_block == UINT32_MAX ||
	    graph.FindBlock(graph.entry_block) == nullptr) {
		SetFailure(graph, FailureKind::InvalidInput, graph.entry_block,
		           "CFG dispatcher lowering received an empty graph");
		return false;
	}

	const auto original_entry = graph.entry_block;
	const auto semantic_count = static_cast<uint32_t>(graph.blocks.size());
	stats.original_blocks     = semantic_count;

	ClearGraphFailure(graph);
	ClearStructuredTerminators(graph);
	graph.blocks.reserve(semantic_count * 4u + 16u);

	const auto loop_header    = AppendEmptyDispatchBlock(graph);
	const auto switch_block   = AppendEmptyDispatchBlock(graph);
	const auto after_switch   = AppendEmptyDispatchBlock(graph);
	const auto continue_block = AppendEmptyDispatchBlock(graph);
	const auto exit_block     = AppendEmptyDispatchBlock(graph);
	const auto entry_init     = AppendEmptyDispatchBlock(graph);

	for (uint32_t id = 0; id < semantic_count; id++) {
		auto* block = graph.FindBlock(id);
		if (block == nullptr) {
			SetFailure(graph, FailureKind::InvalidInput, id,
			           "CFG dispatcher lowering lost a semantic block");
			return false;
		}
		const auto original = block->terminator;
		block->terminator.loop_header    = false;
		block->terminator.merge_block    = UINT32_MAX;
		block->terminator.continue_block = UINT32_MAX;
		block->successors.clear();

		switch (original.kind) {
			case TerminatorKind::Return:
			case TerminatorKind::Unsupported:
				block->terminator.kind          = TerminatorKind::Branch;
				block->terminator.condition     = BranchCondition::Always;
				block->terminator.dispatch_next = kDispatchHaltState;
				block->terminator.true_block    = after_switch;
				block->successors               = {after_switch};
				break;
			case TerminatorKind::Branch:
				block->terminator.kind          = TerminatorKind::Branch;
				block->terminator.condition     = BranchCondition::Always;
				block->terminator.dispatch_next = original.true_block;
				block->terminator.true_block    = after_switch;
				block->successors               = {after_switch};
				break;
			case TerminatorKind::ConditionalBranch: {
				const auto true_setter  =
				    AppendDispatchSetter(graph, original.true_block, after_switch);
				const auto false_setter =
				    AppendDispatchSetter(graph, original.false_block, after_switch);
				const auto join = AppendDispatchJoin(graph, after_switch);
				RetargetDispatchBranch(graph, true_setter, join);
				RetargetDispatchBranch(graph, false_setter, join);
				block                               = graph.FindBlock(id);
				block->terminator.kind              = TerminatorKind::ConditionalBranch;
				block->terminator.condition         = original.condition;
				block->terminator.true_block        = true_setter;
				block->terminator.false_block       = false_setter;
				block->terminator.merge_block       = join;
				block->terminator.goto_variable     = original.goto_variable;
				block->terminator.goto_value        = original.goto_value;
				block->successors                   = {true_setter, false_setter};
				break;
			}
			case TerminatorKind::IndirectBranch: {
				const auto& source_targets =
				    !original.indirect_selector_targets.empty() &&
				            original.indirect_selector_targets.size() ==
				                original.indirect_selector_values.size()
				        ? original.indirect_selector_targets
				        : original.indirect_targets;
				std::vector<uint32_t> mapped_targets;
				mapped_targets.reserve(source_targets.size());
				for (auto target: source_targets) {
					mapped_targets.push_back(AppendDispatchSetter(graph, target, after_switch));
				}
				const auto join = AppendDispatchJoin(graph, after_switch);
				for (auto target: mapped_targets) {
					RetargetDispatchBranch(graph, target, join);
				}
				block                                  = graph.FindBlock(id);
				block->terminator                      = original;
				block->terminator.loop_header          = false;
				block->terminator.continue_block       = UINT32_MAX;
				block->terminator.merge_block          = join;
				block->terminator.indirect_targets     = mapped_targets;
				block->successors                      = mapped_targets;
				if (!original.indirect_selector_values.empty() &&
				    original.indirect_selector_values.size() == source_targets.size()) {
					block->terminator.indirect_selector_targets = mapped_targets;
					block->terminator.indirect_selector_values  = original.indirect_selector_values;
				} else {
					block->terminator.indirect_selector_targets.clear();
					block->terminator.indirect_selector_values.clear();
				}
				break;
			}
			case TerminatorKind::DispatchSwitch:
				SetFailure(graph, FailureKind::StructuredControlFlow, id,
				           "CFG dispatcher lowering cannot wrap an already-dispatched graph");
				return false;
		}
	}

	{
		auto* block = graph.FindBlock(entry_init);
		block->terminator.kind          = TerminatorKind::Branch;
		block->terminator.dispatch_next = original_entry;
		block->terminator.true_block    = loop_header;
		block->successors               = {loop_header};
	}
	{
		auto* block = graph.FindBlock(loop_header);
		block->terminator.kind           = TerminatorKind::ConditionalBranch;
		block->terminator.condition      = BranchCondition::DispatchDone;
		block->terminator.true_block     = exit_block;
		block->terminator.false_block    = switch_block;
		block->terminator.loop_header    = true;
		block->terminator.merge_block    = exit_block;
		block->terminator.continue_block = continue_block;
		block->successors                = {exit_block, switch_block};
	}
	{
		auto* block = graph.FindBlock(switch_block);
		block->terminator.kind        = TerminatorKind::DispatchSwitch;
		block->terminator.true_block  = after_switch;
		block->terminator.merge_block = after_switch;
		block->terminator.indirect_selector_values.clear();
		block->terminator.indirect_selector_targets.clear();
		block->terminator.indirect_targets.clear();
		for (uint32_t id = 0; id < semantic_count; id++) {
			block->terminator.indirect_selector_values.push_back(id);
			block->terminator.indirect_selector_targets.push_back(id);
			block->terminator.indirect_targets.push_back(id);
		}
		block->successors = block->terminator.indirect_targets;
		AddUnique(block->successors, after_switch);
	}
	{
		auto* block = graph.FindBlock(after_switch);
		block->terminator.kind       = TerminatorKind::Branch;
		block->terminator.true_block = continue_block;
		block->successors            = {continue_block};
	}
	{
		auto* block = graph.FindBlock(continue_block);
		block->terminator.kind       = TerminatorKind::Branch;
		block->terminator.true_block = loop_header;
		block->successors            = {loop_header};
	}
	{
		auto* block                  = graph.FindBlock(exit_block);
		block->terminator.kind       = TerminatorKind::Return;
		block->terminator.true_block = UINT32_MAX;
		block->successors.clear();
	}

	std::vector<uint32_t> layout;
	layout.reserve(graph.blocks.size());
	layout.push_back(entry_init);
	layout.push_back(loop_header);
	layout.push_back(switch_block);
	for (uint32_t id = 0; id < static_cast<uint32_t>(graph.blocks.size()); id++) {
		if (id != entry_init && id != loop_header && id != switch_block && id != after_switch &&
		    id != continue_block && id != exit_block) {
			layout.push_back(id);
		}
	}
	layout.push_back(after_switch);
	layout.push_back(continue_block);
	layout.push_back(exit_block);

	std::vector<char> seen(graph.blocks.size(), 0);
	for (auto id: layout) {
		if (id >= seen.size() || seen[id]) {
			SetFailure(graph, FailureKind::StructuredControlFlow, graph.entry_block,
			           "CFG dispatcher layout is not a permutation of block ids");
			return false;
		}
		seen[id] = 1;
	}
	if (layout.size() != graph.blocks.size()) {
		SetFailure(graph, FailureKind::StructuredControlFlow, graph.entry_block,
		           fmt::format("CFG dispatcher layout size mismatch: layout={} blocks={}",
		                       static_cast<uint64_t>(layout.size()),
		                       static_cast<uint64_t>(graph.blocks.size())));
		return false;
	}

	graph.entry_block = entry_init;
	CompactBlocksInOrder(graph, layout);
	RebuildSuccessorsFromTerminators(graph);
	RebuildPredecessors(graph);
	RecomputeAnalyses(graph);

	const auto growth_limit =
	    semantic_count + std::max<uint32_t>(16u, (semantic_count * options.max_block_growth_percent) /
	                                                 100u);
	if (graph.blocks.size() > growth_limit) {
		SetFailure(graph, FailureKind::StructuredControlFlow, graph.entry_block,
		           fmt::format("CFG dispatcher lowering exceeded growth limit: original={} "
		                       "current={} limit={}",
		                       semantic_count, static_cast<uint64_t>(graph.blocks.size()),
		                       growth_limit));
		return false;
	}

	stats.final_blocks           = static_cast<uint32_t>(graph.blocks.size());
	stats.synthetic_blocks       = stats.final_blocks - semantic_count;
	stats.dispatcher_cases       = semantic_count;
	stats.cloned_semantic_blocks = 0;
	stats.rewrites               = 1;
	return true;
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
	uint32_t              before = UINT32_MAX;
	std::vector<uint32_t> blocks;
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

void AppendEnclosingRouteBlocks(Graph& graph, uint32_t original_block_count,
                                uint32_t route_variable, uint32_t shared, uint32_t other,
                                uint32_t route_select, uint32_t route_root,
                                std::vector<uint32_t>& route_blocks) {
	for (uint32_t depth = 0; depth < original_block_count; depth++) {
		uint32_t predecessor = UINT32_MAX;
		uint32_t leaf        = UINT32_MAX;
		for (uint32_t candidate = 0; candidate < original_block_count; candidate++) {
			const auto* candidate_block = graph.FindBlock(candidate);
			if (candidate_block == nullptr ||
			    candidate_block->terminator.kind != TerminatorKind::ConditionalBranch ||
			    IsInnermostLoopControlConditional(graph, *candidate_block)) {
				continue;
			}
			const auto candidate_true  = candidate_block->terminator.true_block;
			const auto candidate_false = candidate_block->terminator.false_block;
			if (candidate_true == route_root &&
			    (candidate_false == shared || candidate_false == other)) {
				predecessor = candidate;
				leaf        = candidate_false;
				break;
			}
			if (candidate_false == route_root &&
			    (candidate_true == shared || candidate_true == other)) {
				predecessor = candidate;
				leaf        = candidate_true;
				break;
			}
		}
		if (predecessor == UINT32_MAX) {
			break;
		}

		const auto routed_leaf =
		    AppendGotoSetBlock(graph, route_variable, leaf == other, route_select);
		auto* predecessor_block = graph.FindBlock(predecessor);
		ReplaceValue(predecessor_block->successors, leaf, routed_leaf);
		ReplaceTerminatorTarget(predecessor_block->terminator, leaf, routed_leaf);
		route_blocks.push_back(routed_leaf);
		route_root = predecessor;
	}
}

// Turn H0 -> shared/H1, H1 -> shared/other into nested selections whose empty
// forwarding blocks set one typed SSA route value. Guest semantic blocks remain unique.
// Preserve loop-control branches just as SplitOneSelectionMerge does. Moving a loop
// exit through the route selector can create an inner cycle whose merge is also the
// enclosing loop merge, so splitting that shared exit can never separate the loops.
bool RouteOneSharedArm(Graph& graph, uint32_t original_block_count, uint32_t outer_id,
                       uint32_t route_variable, GotoRouteBlocks& route) {
	auto* outer = graph.FindBlock(outer_id);
	if (outer == nullptr || outer->inst_begin == outer->inst_end ||
	    outer->terminator.kind != TerminatorKind::ConditionalBranch ||
	    IsInnermostLoopControlConditional(graph, *outer)) {
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
		    inner->terminator.kind != TerminatorKind::ConditionalBranch ||
		    IsInnermostLoopControlConditional(graph, *inner)) {
			continue;
		}

		const bool true_is_shared  = inner->terminator.true_block == shared;
		const bool false_is_shared = inner->terminator.false_block == shared;
		if (true_is_shared != false_is_shared) {
			const auto other =
			    true_is_shared ? inner->terminator.false_block : inner->terminator.true_block;
			if (other == outer_id || other == inner_id || other == shared) {
				continue;
			}
			const auto first_arm = std::min(shared, other);
			if (first_arm >= original_block_count || outer_id >= inner_id ||
			    inner_id >= first_arm) {
				continue;
			}

			const auto route_select = AppendGotoSelectBlock(graph, route_variable, other, shared);
			const auto inner_merge  = AppendSyntheticBranchBlock(graph, route_select);
			const auto outer_shared =
			    AppendGotoSetBlock(graph, route_variable, false, route_select);
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

			std::vector<uint32_t> route_blocks = {inner_shared, inner_other, inner_merge,
			                                      outer_shared};
			AppendEnclosingRouteBlocks(graph, original_block_count, route_variable, shared, other,
			                           route_select, outer_id, route_blocks);
			route_blocks.push_back(route_select);
			route = {first_arm, std::move(route_blocks)};
			return true;
		}

		// H0 -> arm/H1, H1 -> exit/body, and both arms reach a common continuation.
		// Route exit/continuation after joining H0 and H1 so neither has a nonlocal edge.
		for (const bool exit_is_true: {true, false}) {
			const auto other =
			    exit_is_true ? inner->terminator.true_block : inner->terminator.false_block;
			const auto body =
			    exit_is_true ? inner->terminator.false_block : inner->terminator.true_block;
			if (other == outer_id || other == inner_id || other == shared || body == outer_id ||
			    body == inner_id || body == shared || !graph.Dominates(inner_id, body)) {
				continue;
			}

			const auto continuation = graph.FindNearestCommonPostDominator(shared, body);
			const auto* continuation_block = graph.FindBlock(continuation);
			if (continuation_block == nullptr || continuation == other ||
			    CanReachBefore(graph, other, continuation, UINT32_MAX)) {
				continue;
			}
			std::vector<uint32_t> outer_predecessors;
			std::vector<uint32_t> inner_predecessors;
			bool                  external_predecessor = false;
			for (const auto predecessor: continuation_block->predecessors) {
				if (predecessor == outer_id || graph.Dominates(shared, predecessor)) {
					outer_predecessors.push_back(predecessor);
				} else if (predecessor == inner_id || graph.Dominates(body, predecessor)) {
					inner_predecessors.push_back(predecessor);
				} else {
					external_predecessor = true;
				}
			}
			const auto first_arm = std::min(continuation, other);
			if (outer_predecessors.empty() || inner_predecessors.empty() ||
			    external_predecessor || first_arm >= original_block_count ||
			    outer_id >= inner_id || inner_id >= first_arm) {
				continue;
			}

			const auto route_select =
			    AppendGotoSelectBlock(graph, route_variable, other, continuation);
			const auto inner_merge  = AppendSyntheticBranchBlock(graph, route_select);
			const auto outer_continue =
			    AppendGotoSetBlock(graph, route_variable, false, route_select);
			const auto inner_continue =
			    AppendGotoSetBlock(graph, route_variable, false, inner_merge);
			const auto inner_other = AppendGotoSetBlock(graph, route_variable, true, inner_merge);

			auto* mutable_inner = graph.FindBlock(inner_id);
			if (exit_is_true) {
				mutable_inner->terminator.true_block = inner_other;
			} else {
				mutable_inner->terminator.false_block = inner_other;
			}
			ReplaceValue(mutable_inner->successors, other, inner_other);
			for (const auto predecessor: outer_predecessors) {
				auto* predecessor_block = graph.FindBlock(predecessor);
				ReplaceValue(predecessor_block->successors, continuation, outer_continue);
				ReplaceTerminatorTarget(predecessor_block->terminator, continuation,
				                        outer_continue);
			}
			for (const auto predecessor: inner_predecessors) {
				auto* predecessor_block = graph.FindBlock(predecessor);
				ReplaceValue(predecessor_block->successors, continuation, inner_continue);
				ReplaceTerminatorTarget(predecessor_block->terminator, continuation,
				                        inner_continue);
			}

			std::vector<uint32_t> route_blocks = {inner_continue, inner_other, inner_merge,
			                                      outer_continue};
			if (continuation == shared) {
				AppendEnclosingRouteBlocks(graph, original_block_count, route_variable, shared,
				                           other, route_select, outer_id, route_blocks);
			}
			route_blocks.push_back(route_select);
			route = {first_arm, std::move(route_blocks)};
			return true;
		}
	}
	return false;
}

bool RouteSharedSelectionArm(Graph& graph, uint32_t route_variable) {
	if (graph.irreducible) {
		return false;
	}
	const auto block_count = static_cast<uint32_t>(graph.blocks.size());
	// Route later/deeper candidates first so earlier shared arms remain structured.
	for (uint32_t block_id = block_count; block_id-- > 0;) {
		GotoRouteBlocks route;
		if (RouteOneSharedArm(graph, block_count, block_id, route_variable, route)) {
			PlaceGotoRouteBlocks(graph, block_count, route);
			RebuildPredecessors(graph);
			RecomputeAnalyses(graph);
			return true;
		}
	}
	return false;
}

} // namespace

Graph BuildGraph(const Decoder::Program& program) {
	Graph graph;
	if (program.instructions.empty()) {
		ExitBuildFailure(graph, FailureKind::InvalidLabel, UINT32_MAX,
		                 "cannot build CFG for empty shader");
	}

	const auto first_pc = program.instructions.front().pc;
	const auto end_pc   = ProgramEndPc(program);

	std::set<uint32_t> instruction_pcs;
	for (const auto& inst: program.instructions) {
		instruction_pcs.insert(inst.pc);
		if (inst.opcode == Opcode::UNSUPPORTED) {
			ExitBuildFailure(
			    graph, FailureKind::UnsupportedInstruction, UINT32_MAX,
			    fmt::format("unsupported decoded instruction in CFG at pc 0x{:08x}: {}", inst.pc,
			                Decoder::InstructionToString(inst).c_str()));
		}
	}

	std::set<uint32_t> labels;
	labels.insert(first_pc);
	labels.insert(end_pc);

	std::map<uint32_t, SetpcTargetInfo> setpc_targets;
	for (uint32_t i = 0; i < program.instructions.size(); i++) {
		const auto& inst    = program.instructions[i];
		const auto  next_pc = InstructionEndPc(inst);
		if (IsBranch(inst.opcode)) {
			if (!IsValidTarget(inst.branch_target, instruction_pcs, first_pc, end_pc)) {
				ExitBuildFailure(graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
				                 fmt::format("branch at pc 0x{:08x} targets invalid pc 0x{:08x}",
				                             inst.pc, inst.branch_target));
			}
			labels.insert(inst.branch_target);
			if (next_pc <= end_pc) {
				labels.insert(next_pc);
			}
		} else if (inst.opcode == Opcode::S_SETPC_B64) {
			SetpcTargetInfo target_info;
			if (!ResolveSetpcTargets(program, i, target_info)) {
				ExitBuildFailure(
				    graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
				    fmt::format("unsupported dynamic S_SETPC_B64 at pc 0x{:08x}", inst.pc));
			}
			const auto& target_pcs = target_info.indirect
			                             ? target_info.target_pcs
			                             : std::vector<uint32_t> {target_info.target};
			for (const auto target: target_pcs) {
				if (!IsValidTarget(target, instruction_pcs, first_pc, end_pc)) {
					ExitBuildFailure(
					    graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
					    fmt::format("S_SETPC_B64 at pc 0x{:08x} targets invalid pc 0x{:08x}",
					                inst.pc, target));
				}
				labels.insert(target);
			}
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
			ExitBuildFailure(
			    graph, FailureKind::InvalidLabel, UINT32_MAX,
			    fmt::format("CFG label does not start on an instruction: 0x{:08x}", start));
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
			block.terminator.kind = TerminatorKind::Return;
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
				ExitBuildFailure(
				    graph, FailureKind::MissingFallthrough, block.id,
				    fmt::format("conditional branch at pc 0x{:08x} has no fallthrough block",
				                last.pc));
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
			case TerminatorKind::DispatchSwitch:
				for (const auto target: block.terminator.indirect_targets) {
					AddUnique(block.successors, target);
				}
				if (block.terminator.kind == TerminatorKind::DispatchSwitch &&
				    block.terminator.true_block != UINT32_MAX) {
					AddUnique(block.successors, block.terminator.true_block);
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

	return graph;
}

namespace {

// After merges are assigned, verify no block leaves an enclosing construct except through
// its merge (or a break/continue of an enclosing loop). StructurizeImpl gives every
// selection a merge block but never checks the interior edges against it, so a fall-through
// that branches to an *outer* construct's merge is accepted here yet rejected later by
// spirv-val ("block X exits the selection headed by Y, but not via a structured exit"),
// producing a module the driver miscompiles into a GPU hang. Flag it so the auto strategy
// falls back to the dispatcher structurizer for this shader.
bool ValidateStructuredExits(Graph& graph) {
	struct Construct {
		uint32_t              header  = UINT32_MAX;
		uint32_t              merge   = UINT32_MAX;
		uint32_t              cont    = UINT32_MAX;
		bool                  is_loop = false;
		std::vector<uint32_t> region; // sorted; includes header, excludes merge
	};

	std::vector<Construct> constructs;
	for (const auto& block: graph.blocks) {
		if (block.terminator.merge_block == UINT32_MAX) {
			continue;
		}
		Construct c;
		c.header  = block.id;
		c.merge   = block.terminator.merge_block;
		c.is_loop = block.terminator.loop_header;
		c.cont    = c.is_loop ? block.terminator.continue_block : UINT32_MAX;
		c.region  = DominatedBlocks(graph, block.id, c.merge);
		if (!c.region.empty()) {
			constructs.push_back(std::move(c));
		}
	}

	const auto region_has = [](const Construct& c, uint32_t id) {
		return std::binary_search(c.region.begin(), c.region.end(), id);
	};

	for (const auto& c: constructs) {
		for (const auto member_id: c.region) {
			const auto* member = graph.FindBlock(member_id);
			if (member == nullptr) {
				continue;
			}
			for (const auto succ: member->successors) {
				if (succ == c.merge || region_has(c, succ)) {
					continue; // stays inside, or structured exit to own merge
				}
				if (c.is_loop && succ == c.cont) {
					continue; // continue of this loop
				}
				// Leaving c is only structured as a break/continue of an enclosing loop.
				bool structured = false;
				for (const auto& outer: constructs) {
					if (outer.header == c.header || !outer.is_loop) {
						continue;
					}
					if ((succ == outer.merge || succ == outer.cont) &&
					    region_has(outer, c.header)) {
						structured = true;
						break;
					}
				}
				if (!structured) {
					SetFailure(graph, FailureKind::StructuredControlFlow, member_id,
					           fmt::format("block {} exits the construct headed by {} to {} "
					                       "without a structured exit (merge {})",
					                       member_id, c.header, succ, c.merge));
					return false;
				}
			}
		}
	}
	return true;
}

bool StructurizeImpl(Graph& graph, bool allow_clone = false) {
	if (graph.unsupported || graph.irreducible) {
		if (graph.unsupported_reason.empty()) {
			graph.unsupported_reason = "unsupported CFG";
		}
		return false;
	}

	if (!CanonicalizeNaturalLoops(graph)) {
		return false;
	}
	if (!SplitSharedMergeBlocks(graph, allow_clone)) {
		return false;
	}
	if (!IsolateSemanticLoopHeaders(graph)) {
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
			        merge, header, it->second));
			return false;
		}
		return true;
	};

	for (const auto& loop: graph.natural_loops) {
		auto* header = graph.FindBlock(loop.header);
		if (header == nullptr || loop.merge == UINT32_MAX || loop.continue_block == UINT32_MAX) {
			SetFailure(
			    graph, FailureKind::StructuredControlFlow, loop.header,
			    fmt::format("loop at block {} has no structured merge/continue", loop.header));
			return false;
		}
		if (header->inst_begin != header->inst_end ||
		    header->terminator.kind != TerminatorKind::Branch) {
			SetFailure(
			    graph, FailureKind::StructuredControlFlow, loop.header,
			    fmt::format("loop header {} is not a dedicated empty control block", loop.header));
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
			           fmt::format("conditional block {} has no structured merge", block.id));
			return false;
		}

		if (!reserve_merge_block(block.id, merge)) {
			return false;
		}
		block.terminator.merge_block = merge;
	}

	return ValidateStructuredExits(graph);
}

bool StructurizeLegacy(Graph& graph) {
	Graph original = graph;
	if (StructurizeImpl(graph)) {
		return true;
	}

	Graph failed_graph      = std::move(graph);
	graph                   = original;
	const auto route_budget = static_cast<uint32_t>(graph.blocks.size());
	for (uint32_t route_variable = 0; route_variable < route_budget; route_variable++) {
		if (!RouteSharedSelectionArm(graph, route_variable)) {
			break;
		}
		Graph routed = graph;
		if (StructurizeImpl(routed)) {
			graph = std::move(routed);
			return true;
		}
	}

	Graph clone_retry = original;
	if (StructurizeImpl(clone_retry, true)) {
		graph = std::move(clone_retry);
		return true;
	}

	LOGF("ShaderCFG clone retry failed: blocks=%" PRIu64 " loops=%" PRIu64
	     " failure=%s block=%" PRIu32 " reason=%s (first_pass=%s)\n",
	     static_cast<uint64_t>(clone_retry.blocks.size()),
	     static_cast<uint64_t>(clone_retry.natural_loops.size()),
	     FailureKindToString(clone_retry.failure_kind).c_str(), clone_retry.failure_block,
	     clone_retry.unsupported_reason.c_str(), failed_graph.unsupported_reason.c_str());
	graph = std::move(clone_retry);
	return false;
}

bool IsStructuralPathology(const Graph& graph) {
	return graph.failure_kind == FailureKind::StructuredControlFlow;
}

void FillStructurizeStats(const Graph& graph, StructurizeStats& stats, uint32_t original_blocks,
                          std::chrono::steady_clock::time_point started) {
	stats.final_blocks = static_cast<uint32_t>(graph.blocks.size());
	if (stats.original_blocks == 0) {
		stats.original_blocks = original_blocks;
	}
	if (stats.kind == StructurizerKind::LegacySplit && stats.final_blocks >= stats.original_blocks &&
	    stats.synthetic_blocks == 0) {
		stats.synthetic_blocks = stats.final_blocks - stats.original_blocks;
	}
	stats.success        = !graph.unsupported && graph.failure_kind == FailureKind::None;
	stats.failure_reason = graph.unsupported_reason;
	stats.elapsed_us     = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
	                                                            started)
            .count());
	if (stats.success) {
		LOGF("[CFG-STRUCT] strategy=%s original_blocks=%u final_blocks=%u synthetic=%u "
		     "clones=%u dispatch_cases=%u elapsed_us=%" PRIu64 " success=1\n",
		     StructurizerKindToString(stats.kind).c_str(), stats.original_blocks,
		     stats.final_blocks, stats.synthetic_blocks, stats.cloned_semantic_blocks,
		     stats.dispatcher_cases, stats.elapsed_us);
	} else {
		LOGF("[CFG-STRUCT-FAIL] strategy=%s original_blocks=%u current_blocks=%u reason=%s "
		     "elapsed_us=%" PRIu64 "\n",
		     StructurizerKindToString(stats.kind).c_str(), stats.original_blocks, stats.final_blocks,
		     stats.failure_reason.c_str(), stats.elapsed_us);
	}
}

} // namespace

std::string StructurizerKindToString(StructurizerKind kind) {
	switch (kind) {
		case StructurizerKind::Auto: return "auto";
		case StructurizerKind::LegacySplit: return "legacy";
		case StructurizerKind::DispatcherFull: return "dispatch";
		default: return "unknown";
	}
}

StructurizeOptions ReadStructurizeOptions() {
	StructurizeOptions options;
	if (const char* env = std::getenv("KYTY_SHADER_CFG_STRATEGY")) {
		std::string value(env);
		for (auto& ch: value) {
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		}
		if (value == "legacy") {
			options.kind = StructurizerKind::LegacySplit;
		} else if (value == "dispatch" || value == "dispatch-full") {
			options.kind = StructurizerKind::DispatcherFull;
		} else if (value == "auto") {
			options.kind = StructurizerKind::Auto;
		}
	}
	if (const char* env = std::getenv("KYTY_SHADER_CFG_DUMP")) {
		options.dump_graph_on_failure = env[0] != '0';
	}
	if (const char* env = std::getenv("KYTY_SHADER_CFG_MAX_GROWTH")) {
		char*      end    = nullptr;
		const auto parsed = std::strtoul(env, &end, 10);
		if (end != env) {
			options.max_block_growth_percent = static_cast<uint32_t>(parsed);
		}
	}
	return options;
}

bool StructurizeWithStrategy(Graph& graph, const StructurizeOptions& options,
                             StructurizeStats* out_stats) {
	const auto started         = std::chrono::steady_clock::now();
	const auto original_blocks = static_cast<uint32_t>(graph.blocks.size());
	Graph      original        = graph;

	auto run = [&](StructurizerKind kind, StructurizeStats& stats) -> bool {
		graph                 = original;
		stats                 = {};
		stats.kind            = kind;
		stats.original_blocks = original_blocks;
		bool success          = false;
		switch (kind) {
			case StructurizerKind::LegacySplit: success = StructurizeLegacy(graph); break;
			case StructurizerKind::DispatcherFull:
				success = RunDispatcherFull(graph, options, stats);
				break;
			case StructurizerKind::Auto:
			default: success = false; break;
		}
		FillStructurizeStats(graph, stats, original_blocks, started);
		return success;
	};

	StructurizeStats stats;
	bool             success = false;
	if (options.kind == StructurizerKind::Auto) {
		success = run(StructurizerKind::LegacySplit, stats);
		if (!success && IsStructuralPathology(graph)) {
			StructurizeStats dispatch_stats;
			success             = run(StructurizerKind::DispatcherFull, dispatch_stats);
			dispatch_stats.kind = StructurizerKind::Auto;
			stats               = std::move(dispatch_stats);
		}
	} else {
		success = run(options.kind, stats);
	}

	if (out_stats != nullptr) {
		*out_stats = stats;
	}
	return success;
}

bool Structurize(Graph& graph) {
	return StructurizeWithStrategy(graph, ReadStructurizeOptions(), nullptr);
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
		case BranchCondition::DispatchDone: return "dispatch_done";
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
		    "goto_variable={} goto_value={} dispatch_next={}\n",
		    static_cast<uint32_t>(block.terminator.kind),
		    BranchConditionToString(block.terminator.condition).c_str(),
		    block.terminator.true_block, block.terminator.false_block, block.terminator.merge_block,
		    block.terminator.continue_block, block.terminator.loop_header ? 1u : 0u,
		    block.terminator.indirect_pc_sgpr, block.terminator.indirect_selector_code,
		    VectorToString(block.terminator.indirect_targets).c_str(),
		    VectorToString(block.terminator.indirect_selector_values).c_str(),
		    block.terminator.goto_variable, block.terminator.goto_value,
		    block.terminator.dispatch_next);
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
