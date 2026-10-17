#include "graphics/shader/recompiler/frontend/translate/LaunchedMasks.h"

#include <algorithm>
#include <deque>
#include <magic_enum.hpp>
#include <string_view>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::Frontend {
namespace {

using Decoder::OperandKind;

int ScalarCode(const Decoder::Operand& operand) {
	switch (operand.kind) {
		case OperandKind::Sgpr: return operand.reg < 106u ? static_cast<int>(operand.reg) : -1;
		case OperandKind::VccLo: return 106;
		case OperandKind::VccHi: return 107;
		case OperandKind::ExecLo: return 126;
		case OperandKind::ExecHi: return 127;
		default: return -1;
	}
}

bool IsConstant(const Decoder::Operand& operand) {
	return operand.kind == OperandKind::IntegerInlineConstant ||
	       operand.kind == OperandKind::LiteralConstant;
}

bool IsAllOnes(const Decoder::Operand& operand) {
	return operand.kind == OperandKind::IntegerInlineConstant && operand.signed_val == -1;
}

uint32_t ScalarWriteWidth(const Decoder::Instruction& inst, const Decoder::Operand& dst) {
	if (inst.family == Decoder::Family::SMEM) {
		return std::max(inst.data_dwords, 1u);
	}
	if (&dst == &inst.dst2) {
		return 2u;
	}
	const auto name = magic_enum::enum_name(inst.opcode);
	return name.find("64") != std::string_view::npos ? 2u : 1u;
}

} // namespace

bool LaunchedMasks::Launched(const Decoder::Operand& operand) const {
	if (IsConstant(operand)) {
		return operand.value == 0u && operand.signed_val == 0;
	}
	const auto code = ScalarCode(operand);
	return code >= 0 && launched[code];
}

void LaunchedMasks::Update(const Decoder::Instruction& inst, bool whole_quads) {
	using Decoder::Opcode;
	const auto clear = [&](int first, int last) {
		for (int code = std::max(first, 0); code < std::min(last, 128); code++) {
			launched[code]    = false;
			equals_exec[code] = false;
		}
	};
	const auto set_pair = [&](int code, bool value, bool equal) {
		if (code < 0) {
			return;
		}
		clear(code - 1, code + 2);
		launched[code]    = value;
		equals_exec[code] = equal;
	};
	const auto set_exec = [&](bool value, bool full, int equal_code) {
		clear(Exec - 1, Exec + 2);
		equals_exec.reset();
		launched[Exec] = value;
		exec_full      = full;
		if (equal_code >= 0 && equal_code != static_cast<int>(Exec)) {
			equals_exec[equal_code] = true;
		}
	};
	const auto equal_to_exec = [&](const Decoder::Operand& operand) {
		const auto code = ScalarCode(operand);
		return code == static_cast<int>(Exec) || (code >= 0 && equals_exec[code]);
	};
	const auto dst      = ScalarCode(inst.dst);
	const bool exec_was = launched[Exec];

	switch (inst.opcode) {
		case Opcode::S_MOV_B64: {
			const bool value = Launched(inst.src0);
			const bool equal = equal_to_exec(inst.src0);
			if (dst == static_cast<int>(Exec)) {
				set_exec(value, IsAllOnes(inst.src0) || (exec_full && equal),
				         ScalarCode(inst.src0));
			} else {
				set_pair(dst, value, equal);
			}
			return;
		}
		case Opcode::S_AND_B64:
		case Opcode::S_ANDN2_B64:
		case Opcode::S_OR_B64:
		case Opcode::S_XOR_B64:
		case Opcode::S_CSELECT_B64:
		case Opcode::S_WQM_B64: {
			const bool lhs   = Launched(inst.src0);
			const bool rhs   = Launched(inst.src1);
			bool       value = false;
			switch (inst.opcode) {
				case Opcode::S_AND_B64: value = lhs || rhs; break;
				case Opcode::S_ANDN2_B64: value = lhs; break;
				case Opcode::S_WQM_B64: value = whole_quads && lhs; break;
				default: value = lhs && rhs; break;
			}
			if (dst == static_cast<int>(Exec)) {
				set_exec(value, false, -1);
			} else {
				set_pair(dst, value, false);
			}
			return;
		}
		case Opcode::S_AND_SAVEEXEC_B64:
		case Opcode::S_ORN2_SAVEEXEC_B64:
		case Opcode::S_ANDN1_SAVEEXEC_B64: {
			const bool source = Launched(inst.src0);
			// S | ~EXEC is all ones when S holds EXEC.
			const bool full =
			    inst.opcode == Opcode::S_ORN2_SAVEEXEC_B64 && equal_to_exec(inst.src0);
			bool value = false;
			if (inst.opcode == Opcode::S_AND_SAVEEXEC_B64) {
				value = exec_was || source;
			} else if (inst.opcode == Opcode::S_ANDN1_SAVEEXEC_B64) {
				value = source;
			}
			set_pair(dst, exec_was, false);
			set_exec(value, full, -1);
			return;
		}
		default: break;
	}

	const auto name = magic_enum::enum_name(inst.opcode);
	if (name.starts_with("V_CMPX_")) {
		set_exec(exec_was, false, -1);
		return;
	}
	if (name.starts_with("V_CMP_") && dst >= 0 && dst != static_cast<int>(Exec)) {
		// Lanes disabled by EXEC write zero.
		set_pair(dst, exec_was, false);
		return;
	}
	if (name.starts_with("S_MOVRELD_")) {
		clear(0, static_cast<int>(Exec) - 1);
	}
	for (const auto* operand: {&inst.dst, &inst.dst2}) {
		const auto code = ScalarCode(*operand);
		if (code < 0) {
			continue;
		}
		if (code >= static_cast<int>(Exec)) {
			set_exec(false, false, -1);
			continue;
		}
		clear(code - 1, code + static_cast<int>(ScalarWriteWidth(inst, *operand)));
	}
}

void LaunchedMasks::Meet(const LaunchedMasks& other) {
	launched &= other.launched;
	equals_exec &= other.equals_exec;
	exec_full = exec_full && other.exec_full;
}

std::vector<LaunchedMasks> ComputeEntryLaunchedMasks(const Decoder::Program& decoded,
                                                     const CFG::Graph& cfg, bool whole_quads) {
	LaunchedMasks top;
	top.launched.set();
	top.equals_exec.set();
	top.exec_full = true;
	std::vector<LaunchedMasks>           entry(cfg.blocks.size(), top);
	std::unordered_map<uint32_t, size_t> index_of;
	for (size_t index = 0; index < cfg.blocks.size(); index++) {
		index_of.emplace(cfg.blocks[index].id, index);
	}
	std::deque<size_t> worklist;
	std::vector<bool>  queued(cfg.blocks.size(), false);
	std::vector<bool>  reached(cfg.blocks.size(), false);
	if (const auto found = index_of.find(cfg.entry_block); found != index_of.end()) {
		LaunchedMasks start;
		start.launched[LaunchedMasks::Exec] = true;
		entry[found->second]                = start;
		reached[found->second]              = true;
		worklist.push_back(found->second);
		queued[found->second] = true;
	} else {
		for (auto& state: entry) {
			state = LaunchedMasks {};
		}
		return entry;
	}
	while (!worklist.empty()) {
		const auto index = worklist.front();
		worklist.pop_front();
		queued[index]     = false;
		const auto& block = cfg.blocks[index];
		auto        state = entry[index];
		for (uint32_t inst = block.inst_begin; inst < block.inst_end; inst++) {
			state.Update(decoded.instructions[inst], whole_quads);
		}
		for (const auto successor: block.successors) {
			const auto found = index_of.find(successor);
			if (found == index_of.end()) {
				continue;
			}
			auto next = entry[found->second];
			next.Meet(state);
			if (!reached[found->second] || !(next == entry[found->second])) {
				reached[found->second] = true;
				entry[found->second]   = next;
				if (!queued[found->second]) {
					queued[found->second] = true;
					worklist.push_back(found->second);
				}
			}
		}
	}
	// Blocks the CFG does not connect to the entry make no claim.
	for (size_t index = 0; index < entry.size(); index++) {
		if (!reached[index]) {
			entry[index] = LaunchedMasks {};
		}
	}
	return entry;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
