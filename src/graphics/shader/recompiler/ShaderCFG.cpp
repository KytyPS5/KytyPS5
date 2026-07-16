#include "graphics/shader/recompiler/ShaderCFG.h"

#include <algorithm>
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

void SetFailure(Graph* graph, FailureKind kind, uint32_t block_id, const std::string& message,
                std::string* error) {
	if (graph != nullptr) {
		graph->unsupported        = true;
		graph->failure_kind       = kind;
		graph->failure_block      = block_id;
		graph->unsupported_reason = message;
	}
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
	return opcode == Opcode::SBranch;
}

bool IsConditionalBranch(Opcode opcode) {
	switch (opcode) {
		case Opcode::SCbranchScc0:
		case Opcode::SCbranchScc1:
		case Opcode::SCbranchVccz:
		case Opcode::SCbranchVccnz:
		case Opcode::SCbranchExecz:
		case Opcode::SCbranchExecnz: return true;
		default: return false;
	}
}

bool IsBranch(Opcode opcode) {
	return IsUnconditionalBranch(opcode) || IsConditionalBranch(opcode);
}

BranchCondition ConditionForOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::SBranch: return BranchCondition::Always;
		case Opcode::SCbranchScc0: return BranchCondition::SccZero;
		case Opcode::SCbranchScc1: return BranchCondition::SccNonZero;
		case Opcode::SCbranchVccz: return BranchCondition::VccZero;
		case Opcode::SCbranchVccnz: return BranchCondition::VccNonZero;
		case Opcode::SCbranchExecz: return BranchCondition::ExecZero;
		case Opcode::SCbranchExecnz: return BranchCondition::ExecNonZero;
		default: return BranchCondition::Unknown;
	}
}

bool IsRegister(const Decoder::Operand& operand, Decoder::OperandKind kind, uint32_t reg) {
	return operand.kind == kind && operand.reg == reg;
}

bool ScalarOperandCode(const Decoder::Operand& operand, uint32_t* code) {
	if (code == nullptr) {
		return false;
	}
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: *code = operand.reg; return true;
		case Decoder::OperandKind::VccLo: *code = 106u; return true;
		case Decoder::OperandKind::VccHi: *code = 107u; return true;
		case Decoder::OperandKind::M0: *code = 124u; return true;
		case Decoder::OperandKind::ExecLo: *code = 126u; return true;
		case Decoder::OperandKind::ExecHi: *code = 127u; return true;
		default: return false;
	}
}

bool IsScalarCode(const Decoder::Operand& operand, uint32_t code) {
	uint32_t operand_code = 0;
	return ScalarOperandCode(operand, &operand_code) && operand_code == code;
}

bool IsImmediate(const Decoder::Operand& operand, uint32_t* value) {
	if (operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::LiteralConstant ||
	    operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		if (value != nullptr) {
			*value = operand.value;
		}
		return true;
	}
	return false;
}

bool IsImmediateSigned(const Decoder::Operand& operand, int32_t* value) {
	if (operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::LiteralConstant) {
		if (value != nullptr) {
			*value = operand.signed_val;
		}
		return true;
	}
	if (operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		if (value != nullptr) {
			*value = static_cast<int32_t>(operand.value);
		}
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
	if (!ScalarOperandCode(inst.dst, &dst_code)) {
		return false;
	}
	const uint32_t count = std::max<uint32_t>(1u, inst.data_dwords);
	return code >= dst_code && code < dst_code + count;
}

bool FindPreviousGetpc(const Decoder::Program& program, uint32_t before_index, uint32_t dst_code,
                       uint32_t* index) {
	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t candidate_index = i - 1u;
		const auto&    candidate       = program.instructions[candidate_index];
		if (candidate.opcode == Opcode::SGetpcB64 && IsScalarCode(candidate.dst, dst_code)) {
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
		    candidate.opcode == Opcode::SAddU32 || candidate.opcode == Opcode::SAddI32;
		const bool subs =
		    candidate.opcode == Opcode::SSubU32 || candidate.opcode == Opcode::SSubI32;
		if (!adds && !subs) {
			return false;
		}

		const Decoder::Operand* offset      = nullptr;
		int32_t                 imm         = 0;
		uint32_t                getpc_index = 0;
		if (!OtherScalarSource(candidate, base_code, &offset) ||
		    !IsImmediateSigned(*offset, &imm) ||
		    !FindPreviousGetpc(program, candidate_index, base_code, &getpc_index)) {
			return false;
		}

		const auto base = InstructionEndPc(program.instructions[getpc_index]);
		*pc = adds ? base + static_cast<uint32_t>(imm) : base - static_cast<uint32_t>(imm);
		*pc &= ~3u;
		return true;
	}
	return false;
}

bool FindPreviousScalarLoadPair(const Decoder::Program& program, uint32_t before_index,
                                uint32_t dst_code, uint32_t* index) {
	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t candidate_index = i - 1u;
		const auto&    candidate       = program.instructions[candidate_index];
		if (InstructionWritesScalarCode(candidate, dst_code)) {
			if (candidate.opcode == Opcode::SLoadDwordx2 && IsScalarCode(candidate.dst, dst_code)) {
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
                                const Decoder::Operand& byte_offset_operand,
                                uint32_t*               entry_count) {
	if (entry_count == nullptr) {
		return false;
	}
	uint32_t byte_offset_code = 0;
	if (!ScalarOperandCode(byte_offset_operand, &byte_offset_code)) {
		return false;
	}

	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t shift_index = i - 1u;
		const auto&    shift       = program.instructions[shift_index];
		if (!InstructionWritesScalarCode(shift, byte_offset_code)) {
			continue;
		}
		if (shift.opcode != Opcode::SLshlB32 || !IsScalarCode(shift.dst, byte_offset_code)) {
			return false;
		}

		const Decoder::Operand* shift_amount_operand = nullptr;
		uint32_t                shift_amount         = 0;
		if (!OtherScalarSource(shift, byte_offset_code, &shift_amount_operand) ||
		    !IsImmediate(*shift_amount_operand, &shift_amount) || shift_amount != 3u) {
			return false;
		}

		uint32_t index_code = byte_offset_code;

		for (uint32_t j = shift_index; j > 0; j--) {
			const uint32_t clamp_index = j - 1u;
			const auto&    clamp       = program.instructions[clamp_index];
			if (!InstructionWritesScalarCode(clamp, index_code)) {
				continue;
			}
			if (clamp.opcode != Opcode::SMinU32 || !IsScalarCode(clamp.dst, index_code)) {
				return false;
			}

			const Decoder::Operand* clamp_other = nullptr;
			uint32_t                max_index   = 0;
			if (!OtherScalarSource(clamp, index_code, &clamp_other) ||
			    !IsImmediate(*clamp_other, &max_index)) {
				return false;
			}
			*entry_count = max_index + 1u;
			return *entry_count != 0;
		}
		return false;
	}
	return false;
}

bool AddUniqueTargetPc(std::vector<uint32_t>* targets, uint32_t target) {
	if (targets == nullptr) {
		return false;
	}
	if (std::find(targets->begin(), targets->end(), target) == targets->end()) {
		targets->push_back(target);
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
                           SetpcTargetInfo* info) {
	if (info == nullptr || setpc_index < 4u || setpc_index >= program.instructions.size()) {
		return false;
	}

	const auto& setpc  = program.instructions[setpc_index];
	uint32_t    pc_reg = 0;
	if (setpc.opcode != Opcode::SSetpcB64 || !ScalarOperandCode(setpc.src0, &pc_reg) ||
	    setpc.src0.kind != Decoder::OperandKind::Sgpr) {
		return false;
	}

	const auto& getpc = program.instructions[setpc_index - 3u];
	const auto& add   = program.instructions[setpc_index - 2u];
	const auto& addc  = program.instructions[setpc_index - 1u];
	if (getpc.opcode != Opcode::SGetpcB64 || !IsScalarCode(getpc.dst, pc_reg) ||
	    add.opcode != Opcode::SAddU32 || !IsScalarCode(add.dst, pc_reg) ||
	    addc.opcode != Opcode::SAddcU32 || !IsScalarCode(addc.dst, pc_reg + 1u)) {
		return false;
	}

	const Decoder::Operand* offset_low_operand  = nullptr;
	const Decoder::Operand* offset_high_operand = nullptr;
	uint32_t                offset_low_code     = 0;
	uint32_t                offset_high_code    = 0;
	if (!OtherScalarSource(add, pc_reg, &offset_low_operand) ||
	    !OtherScalarSource(addc, pc_reg + 1u, &offset_high_operand) ||
	    !ScalarOperandCode(*offset_low_operand, &offset_low_code) ||
	    !ScalarOperandCode(*offset_high_operand, &offset_high_code) ||
	    offset_high_code != offset_low_code + 1u) {
		return false;
	}

	uint32_t load_index = 0;
	if (!FindPreviousScalarLoadPair(program, setpc_index - 3u, offset_low_code, &load_index)) {
		return false;
	}
	const auto& load            = program.instructions[load_index];
	uint32_t    table_base_code = 0;
	if (!ScalarOperandCode(load.src0, &table_base_code)) {
		return false;
	}

	uint32_t table_pc = 0;
	if (!ResolvePcRelativeBase(program, load_index, table_base_code, &table_pc)) {
		return false;
	}

	uint32_t entry_count = 0;
	if (!ResolveJumpTableEntryCount(program, load_index, load.src1, &entry_count)) {
		return false;
	}
	uint32_t selector_code = UINT32_MAX;
	if (!ScalarOperandCode(load.src1, &selector_code) ||
	    ScalarCodeWrittenInRange(program, load_index + 1u, setpc_index, selector_code)) {
		return false;
	}

	const uint32_t target_base = InstructionEndPc(getpc);
	const uint32_t table_word  = table_pc / 4u;
	if ((table_pc & 3u) != 0 || table_word + entry_count * 2u > program.code.size()) {
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
		AddUniqueTargetPc(&targets, target_pc);
		selector_values.push_back(i * 8u);
		selector_target_pcs.push_back(target_pc);
	}
	if (targets.empty()) {
		return false;
	}

	info->indirect            = true;
	info->pc_sgpr             = pc_reg;
	info->selector_code       = selector_code;
	info->table_load_pc       = load.pc;
	info->target_pcs          = std::move(targets);
	info->selector_values     = std::move(selector_values);
	info->selector_target_pcs = std::move(selector_target_pcs);
	return true;
}

bool ResolveSetpcTarget(const Decoder::Program& program, uint32_t setpc_index, uint32_t* target) {
	if (target == nullptr || setpc_index >= program.instructions.size()) {
		return false;
	}

	const auto& setpc = program.instructions[setpc_index];
	if (setpc.opcode != Opcode::SSetpcB64 || setpc.src0.kind != Decoder::OperandKind::Sgpr) {
		return false;
	}

	const auto pc_reg = setpc.src0.reg;
	if (setpc_index >= 2u) {
		const auto& arith = program.instructions[setpc_index - 1u];
		const auto& getpc = program.instructions[setpc_index - 2u];
		if (getpc.opcode == Opcode::SGetpcB64 && getpc.dst.kind == Decoder::OperandKind::Sgpr &&
		    getpc.dst.reg == pc_reg && arith.dst.kind == Decoder::OperandKind::Sgpr &&
		    arith.dst.reg == pc_reg) {
			uint32_t imm  = 0;
			bool     adds = arith.opcode == Opcode::SAddU32 || arith.opcode == Opcode::SAddI32;
			bool     subs = arith.opcode == Opcode::SSubU32 || arith.opcode == Opcode::SSubI32;
			if ((adds || subs) &&
			    (IsRegister(arith.src0, Decoder::OperandKind::Sgpr, pc_reg) ||
			     IsRegister(arith.src1, Decoder::OperandKind::Sgpr, pc_reg)) &&
			    (IsImmediate(arith.src0, &imm) || IsImmediate(arith.src1, &imm))) {
				const auto base = InstructionEndPc(getpc);
				*target         = adds ? base + imm : base - imm;
				*target &= ~3u;
				return true;
			}
		}
	}

	if (setpc_index >= 1u) {
		const auto& getpc = program.instructions[setpc_index - 1u];
		if (getpc.opcode == Opcode::SGetpcB64 && getpc.dst.kind == Decoder::OperandKind::Sgpr &&
		    getpc.dst.reg == pc_reg) {
			*target = InstructionEndPc(getpc);
			return true;
		}
	}

	return false;
}

bool ResolveSetpcTargets(const Decoder::Program& program, uint32_t setpc_index,
                         SetpcTargetInfo* info) {
	if (info == nullptr) {
		return false;
	}
	*info = {};
	if (ResolveSetpcJumpTable(program, setpc_index, info)) {
		return true;
	}
	uint32_t target = 0;
	if (ResolveSetpcTarget(program, setpc_index, &target)) {
		info->target = target;
		return true;
	}
	return false;
}

bool IsValidTarget(uint32_t target, const std::set<uint32_t>& instruction_pcs, uint32_t first_pc,
                   uint32_t end_pc) {
	return target == end_pc || (target >= first_pc && instruction_pcs.contains(target));
}

void AddUnique(std::vector<uint32_t>* values, uint32_t value) {
	if (std::find(values->begin(), values->end(), value) == values->end()) {
		values->push_back(value);
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

void SortUnique(std::vector<uint32_t>* values) {
	std::sort(values->begin(), values->end());
	values->erase(std::unique(values->begin(), values->end()), values->end());
}

bool Contains(const std::vector<uint32_t>& values, uint32_t value) {
	return std::find(values.begin(), values.end(), value) != values.end();
}

bool ReplaceValue(std::vector<uint32_t>* values, uint32_t old_value, uint32_t new_value) {
	bool changed = false;
	for (auto& value: *values) {
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

bool RemoveValue(std::vector<uint32_t>* values, uint32_t value) {
	const auto old_size = values->size();
	values->erase(std::remove(values->begin(), values->end(), value), values->end());
	return values->size() != old_size;
}

bool ReplaceTerminatorTarget(Terminator* terminator, uint32_t old_value, uint32_t new_value) {
	bool changed = false;
	if (terminator->true_block == old_value) {
		terminator->true_block = new_value;
		changed                = true;
	}
	if (terminator->false_block == old_value) {
		terminator->false_block = new_value;
		changed                 = true;
	}
	return changed;
}

uint32_t RemapId(uint32_t id, const std::vector<uint32_t>& id_map) {
	return id != UINT32_MAX && id < id_map.size() ? id_map[id] : id;
}

void RemapIds(std::vector<uint32_t>* values, const std::vector<uint32_t>& id_map) {
	for (auto& value: *values) {
		value = RemapId(value, id_map);
	}
	SortUnique(values);
}

void RebuildPredecessors(Graph* graph) {
	for (auto& block: graph->blocks) {
		block.predecessors.clear();
		SortUnique(&block.successors);
	}
	for (const auto& block: graph->blocks) {
		for (auto succ: block.successors) {
			if (succ < graph->blocks.size()) {
				AddUnique(&graph->blocks[succ].predecessors, block.id);
			}
		}
	}
	for (auto& block: graph->blocks) {
		SortUnique(&block.predecessors);
	}
}

void ComputeDominators(Graph* graph) {
	const auto count = static_cast<uint32_t>(graph->blocks.size());
	const auto all   = AllBlockIds(count);

	for (auto& block: graph->blocks) {
		block.dominators =
		    (block.id == graph->entry_block ? std::vector<uint32_t> {block.id} : all);
	}

	bool changed = true;
	while (changed) {
		changed = false;
		for (auto& block: graph->blocks) {
			if (block.id == graph->entry_block) {
				continue;
			}
			std::vector<uint32_t> next;
			if (block.predecessors.empty()) {
				next = {block.id};
			} else {
				next = graph->blocks[block.predecessors.front()].dominators;
				for (uint32_t i = 1; i < block.predecessors.size(); i++) {
					next = IntersectSorted(next, graph->blocks[block.predecessors[i]].dominators);
				}
				AddUnique(&next, block.id);
				SortUnique(&next);
			}
			if (next != block.dominators) {
				block.dominators = std::move(next);
				changed          = true;
			}
		}
	}
}

void ComputePostDominators(Graph* graph) {
	const auto count = static_cast<uint32_t>(graph->blocks.size());
	const auto all   = AllBlockIds(count);

	for (auto& block: graph->blocks) {
		block.post_dominators = block.successors.empty() ? std::vector<uint32_t> {block.id} : all;
	}

	bool changed = true;
	while (changed) {
		changed = false;
		for (auto& block: graph->blocks) {
			std::vector<uint32_t> next;
			if (block.successors.empty()) {
				next = {block.id};
			} else {
				next = graph->blocks[block.successors.front()].post_dominators;
				for (uint32_t i = 1; i < block.successors.size(); i++) {
					next =
					    IntersectSorted(next, graph->blocks[block.successors[i]].post_dominators);
				}
				AddUnique(&next, block.id);
				SortUnique(&next);
			}
			if (next != block.post_dominators) {
				block.post_dominators = std::move(next);
				changed               = true;
			}
		}
	}
}

void ComputeBackEdges(Graph* graph) {
	graph->back_edges.clear();
	for (const auto& block: graph->blocks) {
		for (auto succ: block.successors) {
			if (graph->Dominates(succ, block.id)) {
				graph->back_edges.push_back({block.id, succ, true});
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
	AddUnique(&body, header);
	AddUnique(&body, latch);
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
	SortUnique(&body);
	return body;
}

void ComputeNaturalLoops(Graph* graph) {
	graph->natural_loops.clear();
	for (auto& edge: graph->back_edges) {
		bool natural = true;
		auto body    = NaturalLoopBody(*graph, edge.to, edge.from, &natural);
		edge.natural = natural;

		NaturalLoop loop;
		loop.header         = edge.to;
		loop.latch          = edge.from;
		loop.continue_block = edge.from;
		loop.body_blocks    = body;

		for (auto block_id: body) {
			const auto* block = graph->FindBlock(block_id);
			if (block == nullptr) {
				continue;
			}
			for (auto succ: block->successors) {
				if (!Contains(body, succ)) {
					AddUnique(&loop.exit_blocks, succ);
				}
			}
		}
		SortUnique(&loop.exit_blocks);

		if (!loop.exit_blocks.empty()) {
			uint32_t merge = loop.exit_blocks.front();
			for (uint32_t i = 1; i < loop.exit_blocks.size(); i++) {
				merge = graph->FindNearestCommonPostDominator(merge, loop.exit_blocks[i]);
			}
			loop.merge = merge;
		}
		graph->natural_loops.push_back(std::move(loop));
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

void TarjanVisit(TarjanState* state, uint32_t block_id) {
	state->index[block_id]   = state->next_index;
	state->lowlink[block_id] = state->next_index;
	state->next_index++;
	state->stack.push_back(block_id);
	state->on_stack[block_id] = true;

	const auto* block = state->graph->FindBlock(block_id);
	if (block != nullptr) {
		for (auto succ: block->successors) {
			if (state->index[succ] == UINT32_MAX) {
				TarjanVisit(state, succ);
				state->lowlink[block_id] = std::min(state->lowlink[block_id], state->lowlink[succ]);
			} else if (state->on_stack[succ]) {
				state->lowlink[block_id] = std::min(state->lowlink[block_id], state->index[succ]);
			}
		}
	}

	if (state->lowlink[block_id] != state->index[block_id]) {
		return;
	}

	StronglyConnectedComponent component;
	for (;;) {
		const auto member = state->stack.back();
		state->stack.pop_back();
		state->on_stack[member] = false;
		component.blocks.push_back(member);
		if (member == block_id) {
			break;
		}
	}
	SortUnique(&component.blocks);

	bool cyclic = component.blocks.size() > 1u;
	for (auto member: component.blocks) {
		const auto* member_block = state->graph->FindBlock(member);
		if (member_block != nullptr && Contains(member_block->successors, member)) {
			cyclic = true;
		}
		if (member_block != nullptr) {
			for (auto pred: member_block->predecessors) {
				if (!Contains(component.blocks, pred)) {
					AddUnique(&component.entry_blocks, member);
				}
			}
		}
	}
	SortUnique(&component.entry_blocks);
	component.irreducible = cyclic && component.entry_blocks.size() > 1u;
	state->components.push_back(std::move(component));
}

void ComputeComponents(Graph* graph) {
	TarjanState state;
	state.graph = graph;
	state.index.assign(graph->blocks.size(), UINT32_MAX);
	state.lowlink.assign(graph->blocks.size(), UINT32_MAX);
	state.on_stack.assign(graph->blocks.size(), false);
	state.stack.reserve(graph->blocks.size());
	state.components.reserve(graph->blocks.size());

	for (const auto& block: graph->blocks) {
		if (state.index[block.id] == UINT32_MAX) {
			TarjanVisit(&state, block.id);
		}
	}

	graph->components  = std::move(state.components);
	graph->irreducible = false;
	for (const auto& component: graph->components) {
		if (component.irreducible) {
			graph->irreducible   = true;
			graph->failure_kind  = FailureKind::IrreducibleControlFlow;
			graph->failure_block = component.entry_blocks.empty() ? component.blocks.front()
			                                                      : component.entry_blocks.front();
			graph->unsupported_reason = "irreducible CFG: cyclic component has multiple entries";
			break;
		}
	}
}

void RecomputeAnalyses(Graph* graph) {
	ComputeDominators(graph);
	ComputePostDominators(graph);
	ComputeBackEdges(graph);
	ComputeNaturalLoops(graph);
	ComputeComponents(graph);
}

uint32_t MoveBlockBefore(Graph* graph, uint32_t block_id, uint32_t before_id) {
	if (block_id == before_id || block_id >= graph->blocks.size() ||
	    before_id >= graph->blocks.size()) {
		return block_id;
	}

	const auto              block_pos  = block_id;
	const auto              before_pos = before_id;
	std::vector<BasicBlock> old_blocks = std::move(graph->blocks);
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

	graph->blocks      = std::move(new_blocks);
	graph->entry_block = RemapId(graph->entry_block, id_map);
	for (auto& block: graph->blocks) {
		block.id = RemapId(block.id, id_map);
		RemapIds(&block.predecessors, id_map);
		RemapIds(&block.successors, id_map);
		RemapIds(&block.dominators, id_map);
		RemapIds(&block.post_dominators, id_map);
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

		AddUnique(&blocks, block_id);
		for (auto succ: block->successors) {
			if (succ != stop_block && graph.Dominates(header, succ)) {
				stack.push_back(succ);
			}
		}
	}

	SortUnique(&blocks);
	return blocks;
}

uint32_t AppendSyntheticMergeBlock(Graph* graph, uint32_t old_merge) {
	const auto* merge = graph->FindBlock(old_merge);

	BasicBlock block;
	block.id                    = static_cast<uint32_t>(graph->blocks.size());
	block.start_pc              = merge != nullptr ? merge->start_pc : 0u;
	block.end_pc                = block.start_pc;
	block.inst_begin            = merge != nullptr ? merge->inst_begin : 0u;
	block.inst_end              = block.inst_begin;
	block.successors            = {old_merge};
	block.terminator.kind       = TerminatorKind::Branch;
	block.terminator.condition  = BranchCondition::Always;
	block.terminator.true_block = old_merge;
	graph->blocks.push_back(std::move(block));
	return graph->blocks.back().id;
}

bool IsSyntheticMergeForwarder(const Graph& graph, uint32_t block_id, uint32_t merge) {
	const auto* block = graph.FindBlock(block_id);
	return block != nullptr && block->inst_begin == block->inst_end &&
	       block->successors.size() == 1u && block->successors.front() == merge &&
	       block->terminator.kind == TerminatorKind::Branch &&
	       block->terminator.condition == BranchCondition::Always &&
	       block->terminator.true_block == merge;
}

bool IsInsideLoopConstruct(const Graph& graph, const NaturalLoop& loop, uint32_t block_id) {
	return block_id != UINT32_MAX && block_id != loop.merge && block_id != loop.continue_block &&
	       graph.Dominates(loop.header, block_id) &&
	       (loop.merge == UINT32_MAX || !graph.Dominates(loop.merge, block_id));
}

bool SelectionMergeLeavesContainingLoop(const Graph& graph, uint32_t header, uint32_t merge) {
	for (const auto& loop: graph.natural_loops) {
		if (IsInsideLoopConstruct(graph, loop, header) &&
		    !IsInsideLoopConstruct(graph, loop, merge)) {
			return true;
		}
	}
	return false;
}

bool SplitSharedMergeBlock(Graph* graph, uint32_t merge,
                           const std::vector<uint32_t>& construct_blocks,
                           bool                         force_split = false) {
	if (graph == nullptr || merge == UINT32_MAX || merge >= graph->blocks.size() ||
	    Contains(construct_blocks, merge)) {
		return false;
	}

	const auto* merge_block = graph->FindBlock(merge);
	if (merge_block == nullptr) {
		return false;
	}

	std::vector<uint32_t> construct_predecessors;
	std::vector<uint32_t> external_predecessors;
	for (auto pred: merge_block->predecessors) {
		if (Contains(construct_blocks, pred)) {
			AddUnique(&construct_predecessors, pred);
		} else {
			AddUnique(&external_predecessors, pred);
		}
	}

	if (construct_predecessors.empty() || (!force_split && external_predecessors.empty())) {
		return false;
	}
	std::vector<uint32_t> predecessors_to_split;
	for (auto pred: construct_predecessors) {
		if (!IsSyntheticMergeForwarder(*graph, pred, merge)) {
			AddUnique(&predecessors_to_split, pred);
		}
	}
	if (predecessors_to_split.empty()) {
		return false;
	}

	const auto synthetic_merge = AppendSyntheticMergeBlock(graph, merge);
	auto*      synthetic_block = graph->FindBlock(synthetic_merge);
	if (synthetic_block != nullptr) {
		synthetic_block->predecessors = predecessors_to_split;
		SortUnique(&synthetic_block->predecessors);
	}

	for (auto pred: predecessors_to_split) {
		auto* block = graph->FindBlock(pred);
		if (block == nullptr) {
			continue;
		}
		ReplaceValue(&block->successors, merge, synthetic_merge);
		ReplaceTerminatorTarget(&block->terminator, merge, synthetic_merge);
	}

	auto* old_merge = graph->FindBlock(merge);
	if (old_merge != nullptr) {
		for (auto pred: predecessors_to_split) {
			RemoveValue(&old_merge->predecessors, pred);
		}
		AddUnique(&old_merge->predecessors, synthetic_merge);
		SortUnique(&old_merge->predecessors);
	}

	MoveBlockBefore(graph, synthetic_merge, merge);
	return true;
}

bool SplitOneLoopMerge(Graph* graph) {
	const auto& loops = graph->natural_loops;
	for (const auto& loop: loops) {
		if (SplitSharedMergeBlock(graph, loop.merge, loop.body_blocks)) {
			return true;
		}
	}
	return false;
}

bool SplitOneSelectionMerge(Graph* graph) {
	std::vector<uint32_t> loop_headers;
	loop_headers.reserve(graph->natural_loops.size());
	for (const auto& loop: graph->natural_loops) {
		AddUnique(&loop_headers, loop.header);
	}

	const auto original_block_count = static_cast<uint32_t>(graph->blocks.size());
	for (uint32_t block_id = 0; block_id < original_block_count; block_id++) {
		const auto* block = graph->FindBlock(block_id);
		if (block == nullptr || block->terminator.kind != TerminatorKind::ConditionalBranch ||
		    Contains(loop_headers, block_id)) {
			continue;
		}

		const auto merge = graph->FindNearestCommonPostDominator(block->terminator.true_block,
		                                                         block->terminator.false_block);
		const auto construct_blocks = DominatedBlocks(*graph, block_id, merge);
		const auto force_split      = SelectionMergeLeavesContainingLoop(*graph, block_id, merge);
		if (SplitSharedMergeBlock(graph, merge, construct_blocks, force_split)) {
			return true;
		}
	}
	return false;
}

bool SplitSharedMergeBlocks(Graph* graph, std::string* error) {
	const auto original_block_count = static_cast<uint32_t>(graph->blocks.size());
	const auto split_budget =
	    std::max<uint32_t>(16u, std::min<uint32_t>(128u, original_block_count));
	for (uint32_t splits = 0; splits < split_budget; splits++) {
		if (!SplitOneLoopMerge(graph) && !SplitOneSelectionMerge(graph)) {
			return true;
		}
		RebuildPredecessors(graph);
		RecomputeAnalyses(graph);
	}
	SetFailure(graph, FailureKind::StructuredControlFlow, graph->entry_block,
	           fmt::format("CFG shared merge splitting exceeded budget: original_blocks={} "
	                       "current_blocks={} split_budget={}",
	                       original_block_count, static_cast<uint64_t>(graph->blocks.size()),
	                       split_budget),
	           error);
	return false;
}

void ClearStructuredTerminators(Graph* graph) {
	for (auto& block: graph->blocks) {
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

bool BuildGraph(const Decoder::Program& program, Graph* graph, std::string* error) {
	if (graph == nullptr) {
		SetError(error, "invalid CFG output");
		return false;
	}
	*graph = {};

	if (program.instructions.empty()) {
		SetError(error, "cannot build CFG for empty shader");
		return false;
	}

	const auto first_pc = program.instructions.front().pc;
	const auto end_pc   = ProgramEndPc(program);

	std::set<uint32_t> instruction_pcs;
	for (const auto& inst: program.instructions) {
		instruction_pcs.insert(inst.pc);
		if (inst.opcode == Opcode::Unsupported) {
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
		} else if (inst.opcode == Opcode::SSetpcB64) {
			SetpcTargetInfo target_info;
			if (!ResolveSetpcTargets(program, i, &target_info)) {
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
		} else if (inst.opcode == Opcode::SEndpgm) {
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
		block.id         = static_cast<uint32_t>(graph->blocks.size());
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
		graph->blocks.push_back(std::move(block));
	}

	graph->entry_block = 0;

	std::map<uint32_t, uint32_t> pc_to_block;
	for (const auto& block: graph->blocks) {
		pc_to_block.emplace(block.start_pc, block.id);
	}

	for (auto& block: graph->blocks) {
		block.terminator = {};
		if (block.inst_begin == block.inst_end) {
			block.terminator.kind = TerminatorKind::Return;
			continue;
		}

		const auto& last    = program.instructions[block.inst_end - 1u];
		const auto  next_pc = InstructionEndPc(last);
		if (last.opcode == Opcode::SEndpgm) {
			block.terminator.kind       = TerminatorKind::Branch;
			block.terminator.condition  = BranchCondition::Always;
			block.terminator.true_block = pc_to_block.at(end_pc);
		} else if (last.opcode == Opcode::SSetpcB64) {
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
					AddUnique(&graph->code_table_load_pcs, target_info.table_load_pc);
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
				AddUnique(&block.successors, block.terminator.true_block);
				break;
			case TerminatorKind::ConditionalBranch:
				AddUnique(&block.successors, block.terminator.true_block);
				AddUnique(&block.successors, block.terminator.false_block);
				break;
			case TerminatorKind::IndirectBranch:
				for (const auto target: block.terminator.indirect_targets) {
					AddUnique(&block.successors, target);
				}
				break;
			case TerminatorKind::Return:
			case TerminatorKind::Unsupported: break;
		}
	}

	for (auto& block: graph->blocks) {
		SortUnique(&block.successors);
		for (auto succ: block.successors) {
			AddUnique(&graph->blocks[succ].predecessors, block.id);
		}
	}
	for (auto& block: graph->blocks) {
		SortUnique(&block.predecessors);
	}
	SortUnique(&graph->code_table_load_pcs);

	ComputeDominators(graph);
	ComputePostDominators(graph);
	ComputeBackEdges(graph);
	ComputeNaturalLoops(graph);
	ComputeComponents(graph);

	if (indirect_setpc) {
		graph->irreducible        = true;
		graph->unsupported        = false;
		graph->failure_kind       = FailureKind::IrreducibleControlFlow;
		graph->unsupported_reason = "indirect S_SETPC_B64 jump table requires dispatcher fallback";
	}

	if (graph->irreducible) {
		graph->unsupported = false;
	}

	return true;
}

bool Structurize(Graph* graph, std::string* error) {
	if (graph == nullptr) {
		SetError(error, "invalid CFG input");
		return false;
	}
	if (graph->unsupported || graph->irreducible) {
		if (graph->unsupported_reason.empty()) {
			graph->unsupported_reason = "unsupported CFG";
		}
		SetError(error, graph->unsupported_reason);
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

	for (const auto& loop: graph->natural_loops) {
		auto* header = graph->FindBlock(loop.header);
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

	for (auto& block: graph->blocks) {
		if (block.terminator.kind != TerminatorKind::ConditionalBranch ||
		    block.terminator.loop_header) {
			continue;
		}

		const auto merge = graph->FindNearestCommonPostDominator(block.terminator.true_block,
		                                                         block.terminator.false_block);
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

std::string BranchConditionToString(BranchCondition condition) {
	switch (condition) {
		case BranchCondition::Always: return "always";
		case BranchCondition::SccZero: return "scc0";
		case BranchCondition::SccNonZero: return "scc1";
		case BranchCondition::VccZero: return "vccz";
		case BranchCondition::VccNonZero: return "vccnz";
		case BranchCondition::ExecZero: return "execz";
		case BranchCondition::ExecNonZero: return "execnz";
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

std::string FormatBlockDiagnostic(const Graph& graph, uint32_t block_id, const char* stage,
                                  const std::string& reason) {
	const auto* block      = graph.FindBlock(block_id);
	const char* stage_text = stage != nullptr ? stage : "unknown";
	if (block == nullptr) {
		return fmt::format("stage={} block={} reason={}", stage_text, block_id, reason.c_str());
	}
	return fmt::format("stage={} block={} pc=0x{:08x}..0x{:08x} preds={} succs={} reason={}",
	                   stage_text, block->id, block->start_pc, block->end_pc,
	                   static_cast<uint32_t>(block->predecessors.size()),
	                   static_cast<uint32_t>(block->successors.size()), reason.c_str());
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
		    "indirect_sgpr={} indirect_selector={} indirect_targets=[{}] selector_values=[{}]\n",
		    static_cast<uint32_t>(block.terminator.kind),
		    BranchConditionToString(block.terminator.condition).c_str(),
		    block.terminator.true_block, block.terminator.false_block, block.terminator.merge_block,
		    block.terminator.continue_block, block.terminator.loop_header ? 1u : 0u,
		    block.terminator.indirect_pc_sgpr, block.terminator.indirect_selector_code,
		    VectorToString(block.terminator.indirect_targets).c_str(),
		    VectorToString(block.terminator.indirect_selector_values).c_str());
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
