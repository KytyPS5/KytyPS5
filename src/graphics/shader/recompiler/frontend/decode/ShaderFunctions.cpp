#include "graphics/shader/recompiler/frontend/decode/ShaderFunctions.h"

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/shaderBindings.h"
#include "common/logging/log.h"

#include <fmt/format.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

constexpr uint32_t MaxFunctionWords  = 4096;
constexpr uint32_t MaxExpandedWords  = 524288;
constexpr uint64_t MaxCallTableBytes = 16u * 1024u * 1024u;
constexpr size_t   MaxCallTargets    = 256;
// A call table can name many more functions, as long as their code is that of at most
// MaxCallTargets bodies (Gran Turismo 7's any-hit table: 10614 rows, 1695 addresses).
constexpr size_t   MaxCallTableTargets = 4096;
// CallSite::consumed names VGPR n as VgprHeld + n.
constexpr uint32_t VgprHeld          = 0x10000;

bool IsCall(uint32_t word) {
	return (word & 0xff80ff00u) == 0xbe802100u && ((word >> 16u) & 127u) != 125u;
}

uint32_t DestinationWords(const Instruction& inst, uint32_t wave_size) {
	const auto name = magic_enum::enum_name(inst.opcode);
	if (name.starts_with("V_CMP")) return wave_size / 32u;
	const bool pair = name.find("64") != std::string_view::npos;
	return std::max(inst.data_dwords, pair ? 2u : 1u);
}

bool Writes(const Instruction& inst, uint32_t reg, uint32_t wave_size) {
	if (IsCall(inst.raw[0])) {
		const auto dst = (inst.raw[0] >> 16u) & 127u;
		return reg == dst || reg == dst + 1u;
	}
	return (inst.dst.kind == OperandKind::Sgpr && reg >= inst.dst.reg &&
	        reg < inst.dst.reg + DestinationWords(inst, wave_size)) ||
	       (inst.dst2.kind == OperandKind::Sgpr && reg >= inst.dst2.reg &&
	        reg < inst.dst2.reg + wave_size / 32u);
}

// Whether the instruction can write VGPR `reg` (image results conservatively wide).
bool WritesVgpr(const Instruction& inst, uint32_t reg) {
	uint32_t count = 1;
	switch (inst.family) {
		case Family::MIMG: count = 8; break;
		case Family::MUBUF:
		case Family::MTBUF:
		case Family::FLAT: count = std::max(inst.data_dwords, 1u); break;
		case Family::DS: count = std::max(inst.data_dwords, 1u) * 2u; break;
		default: count = magic_enum::enum_name(inst.opcode).find("64") != std::string_view::npos ? 2u : 1u; break;
	}
	const auto covers = [&](const Operand& operand) {
		return operand.kind == OperandKind::Vgpr && reg >= operand.reg && reg < operand.reg + count;
	};
	return covers(inst.dst) || covers(inst.dst2);
}

struct Function {
	std::vector<uint32_t>    code;
	std::vector<Instruction> instructions;
};

bool ReadsScc(const Instruction& inst) {
	switch (inst.opcode) {
		case Opcode::S_CSELECT_B32:
		case Opcode::S_CSELECT_B64:
		case Opcode::S_CMOV_B64:
		case Opcode::S_ADDC_U32:
		case Opcode::S_SUBB_U32:
		case Opcode::S_CBRANCH_SCC0:
		case Opcode::S_CBRANCH_SCC1: return true;
		default: break;
	}
	for (const auto* operand: {&inst.src0, &inst.src1, &inst.src2, &inst.src3}) {
		if (operand->kind == OperandKind::Scc) return true;
	}
	return false;
}

// Whether the instruction always writes SCC. Instructions missing here count as preserving it,
// which can only keep an SCC live, never drop a live one.
bool WritesScc(const Instruction& inst) {
	if (inst.family == Family::SOPC) return true;
	if (inst.family != Family::SOP1 && inst.family != Family::SOP2 && inst.family != Family::SOPK)
		return false;
	switch (inst.opcode) {
		case Opcode::S_ADD_U32:
		case Opcode::S_SUB_U32:
		case Opcode::S_ADD_I32: // also S_ADDK_I32
		case Opcode::S_SUB_I32:
		case Opcode::S_ADDC_U32:
		case Opcode::S_SUBB_U32:
		case Opcode::S_MIN_I32:
		case Opcode::S_MIN_U32:
		case Opcode::S_MAX_I32:
		case Opcode::S_MAX_U32:
		case Opcode::S_AND_B32:
		case Opcode::S_AND_B64:
		case Opcode::S_OR_B32:
		case Opcode::S_OR_B64:
		case Opcode::S_XOR_B32:
		case Opcode::S_XOR_B64:
		case Opcode::S_ANDN2_B32:
		case Opcode::S_ANDN2_B64:
		case Opcode::S_ORN2_B32:
		case Opcode::S_ORN2_B64:
		case Opcode::S_NAND_B32:
		case Opcode::S_NAND_B64:
		case Opcode::S_NOR_B32:
		case Opcode::S_NOR_B64:
		case Opcode::S_XNOR_B32:
		case Opcode::S_XNOR_B64:
		case Opcode::S_LSHL_B32:
		case Opcode::S_LSHL_B64:
		case Opcode::S_LSHR_B32:
		case Opcode::S_LSHR_B64:
		case Opcode::S_ASHR_I32:
		case Opcode::S_ASHR_I64:
		case Opcode::S_BFE_U32:
		case Opcode::S_BFE_I32:
		case Opcode::S_BFE_U64:
		case Opcode::S_ABSDIFF_I32:
		case Opcode::S_LSHL1_ADD_U32:
		case Opcode::S_LSHL2_ADD_U32:
		case Opcode::S_LSHL3_ADD_U32:
		case Opcode::S_LSHL4_ADD_U32:
		case Opcode::S_NOT_B32:
		case Opcode::S_NOT_B64:
		case Opcode::S_WQM_B32:
		case Opcode::S_WQM_B64:
		case Opcode::S_BCNT0_I32_B32:
		case Opcode::S_BCNT0_I32_B64:
		case Opcode::S_BCNT1_I32_B32:
		case Opcode::S_BCNT1_I32_B64:
		case Opcode::S_AND_SAVEEXEC_B32:
		case Opcode::S_AND_SAVEEXEC_B64:
		case Opcode::S_ORN2_SAVEEXEC_B32:
		case Opcode::S_ORN2_SAVEEXEC_B64:
		case Opcode::S_ANDN1_SAVEEXEC_B32:
		case Opcode::S_ANDN1_SAVEEXEC_B64:
		case Opcode::S_QUADMASK_B32:
		case Opcode::S_QUADMASK_B64:
		case Opcode::S_ABS_I32: return true;
		default: return false;
	}
}

// Whether the SCC on entry to instructions[first] can be read: some path from there reaches an
// SCC read before an SCC write. A return (S_SETPC_B64) gives `at_return`, a call or anything else
// that leaves the instructions `true`; S_ENDPGM ends a path.
bool SccLiveAt(const std::vector<Instruction>& instructions, size_t first, bool at_return) {
	std::map<uint32_t, size_t> index_of_pc;
	for (size_t i = 0; i < instructions.size(); i++)
		index_of_pc[instructions[i].pc] = i;
	std::vector<bool>   visited(instructions.size(), false);
	std::vector<size_t> pending {first};
	while (!pending.empty()) {
		const auto at = pending.back();
		pending.pop_back();
		if (at >= instructions.size()) return true;
		if (visited[at]) continue;
		visited[at] = true;
		const auto& inst = instructions[at];
		if (ReadsScc(inst) || IsCall(inst.raw[0])) return true;
		if (inst.opcode == Opcode::S_SETPC_B64) {
			if (at_return) return true;
			continue;
		}
		if (WritesScc(inst) || inst.opcode == Opcode::S_ENDPGM) continue;
		if (IsDirectBranch(inst.opcode)) {
			const auto target = index_of_pc.find(inst.branch_target);
			if (target == index_of_pc.end()) return true;
			pending.push_back(target->second);
			if (inst.opcode == Opcode::S_BRANCH) continue;
		}
		pending.push_back(at + 1u);
	}
	return false;
}

bool ReadFunction(uint64_t address, uint32_t return_reg, const ShaderCodeReader& read,
                  Function& function, std::string& reason, uint32_t wave_size) {
	uint32_t next            = 0;
	uint32_t furthest_target = 0;
	bool     returned        = false;
	while (next + MaxInstructionRawWords <= MaxFunctionWords) {
		while (function.code.size() < next + MaxInstructionRawWords) {
			std::array<uint32_t, 64> chunk {};
			if (!read(address + function.code.size() * 4u, chunk)) {
				reason = "shader function code is not mapped";
				return false;
			}
			function.code.insert(function.code.end(), chunk.begin(), chunk.end());
		}
		if (GetInstructionFamily(function.code[next]) == Family::Unknown) {
			reason = "shader function has an unknown instruction encoding";
			return false;
		}
		Instruction inst;
		DecodeInstruction(function.code, next, inst);
		if (IsCall(inst.raw[0]) || inst.opcode == Opcode::S_ENDPGM ||
		    inst.opcode == Opcode::UNSUPPORTED || Writes(inst, return_reg, wave_size) ||
		    Writes(inst, return_reg + 1u, wave_size)) {
			reason = fmt::format("shader function needs nested calls or unsupported instructions (instruction word "
			                     "0x{:08x} at 0x{:x}, offset {} words; {})",
			                     inst.raw[0], address, next,
			                     IsCall(inst.raw[0])                    ? "a call"
			                     : inst.opcode == Opcode::S_ENDPGM      ? "S_ENDPGM"
			                     : inst.opcode == Opcode::UNSUPPORTED   ? "an instruction the decoder does not support"
			                                                            : "a write to the return address");
			return false;
		}
		function.instructions.push_back(inst);
		next += inst.word_count;
		if (IsDirectBranch(inst.opcode)) {
			furthest_target = std::max(furthest_target, inst.branch_target);
		}
		if (inst.opcode == Opcode::S_SETPC_B64) {
			if (inst.src0.kind != OperandKind::Sgpr || inst.src0.reg != return_reg) {
				reason = "shader function returns through a different scalar pair";
				return false;
			}
			returned = true;
			if (next * 4u > furthest_target) break;
		}
	}
	if (!returned || function.instructions.back().opcode != Opcode::S_SETPC_B64) {
		reason = "shader function exceeds the bounded code range";
		return false;
	}
	function.code.resize(next);
	std::set<uint32_t> starts;
	for (const auto& inst: function.instructions)
		starts.insert(inst.pc);
	for (const auto& inst: function.instructions) {
		if (IsDirectBranch(inst.opcode) && !starts.contains(inst.branch_target)) {
			reason = "shader function branches outside its instruction range";
			return false;
		}
	}
	return true;
}

struct CallTarget {
	uint64_t address;
	Function function;
};

struct Call {
	uint32_t                source_reg;
	uint32_t                return_reg;
	bool                    dynamic;
	std::vector<CallTarget> targets;
};

bool IsScalarLoad(Opcode opcode) {
	return opcode == Opcode::S_LOAD_DWORD || opcode == Opcode::S_LOAD_DWORDX2 ||
	       opcode == Opcode::S_LOAD_DWORDX4 || opcode == Opcode::S_LOAD_DWORDX8 ||
	       opcode == Opcode::S_LOAD_DWORDX16;
}

bool IsScalarBufferLoad(Opcode opcode) {
	return opcode == Opcode::S_BUFFER_LOAD_DWORD || opcode == Opcode::S_BUFFER_LOAD_DWORDX2 ||
	       opcode == Opcode::S_BUFFER_LOAD_DWORDX4 || opcode == Opcode::S_BUFFER_LOAD_DWORDX8 ||
	       opcode == Opcode::S_BUFFER_LOAD_DWORDX16;
}

std::optional<uint32_t> Immediate(const Operand& operand) {
	if (operand.kind == OperandKind::Null) return 0;
	if (operand.kind == OperandKind::LiteralConstant ||
	    operand.kind == OperandKind::IntegerInlineConstant)
		return operand.value;
	return {};
}

std::optional<uint32_t> ScalarIndex(const Operand& operand) {
	switch (operand.kind) {
		case OperandKind::Sgpr: return operand.reg;
		case OperandKind::VccLo: return 106;
		case OperandKind::VccHi: return 107;
		case OperandKind::M0: return 124;
		case OperandKind::ExecLo: return 126;
		case OperandKind::ExecHi: return 127;
		default: return {};
	}
}

template <typename Move>
bool EmitOriginalPc(std::vector<uint32_t>& code, const Instruction& inst, uint64_t base,
                    std::string& reason, const Move& move) {
	if (inst.dst.kind == OperandKind::Null) {
		code.push_back(0xbf800000u); // S_NOP retains an instruction boundary.
		return true;
	}
	const auto reg = (inst.raw[0] >> 16u) & 127u;
	if (!((inst.dst.kind == OperandKind::Sgpr && inst.dst.reg <= 105u) ||
	      inst.dst.kind == OperandKind::VccLo || inst.dst.kind == OperandKind::M0 ||
	      inst.dst.kind == OperandKind::ExecLo)) {
		reason = "shader GETPC destination is not a supported scalar pair";
		return false;
	}
	// Expansion changes instruction positions, but GETPC must still address the
	// original shader's constants/code. MOV does not change SCC, matching GETPC.
	const auto pc = base + inst.pc + 4u;
	move(reg, static_cast<uint32_t>(pc));
	move(reg + 1u, static_cast<uint32_t>(pc >> 32u));
	return true;
}

// The runtime words of an expansion (see ShaderFunctionExpander::RuntimeLiterals()).
struct ExpansionWords {
	std::vector<RuntimeLiteral> literals;
	std::vector<uint32_t>       words;
};

// Code identity of a call target: its instructions (branch offsets are relative).
uint64_t FunctionContentKey(const Function& function) {
	uint64_t key = 0xcbf29ce484222325ull;
	for (const auto& inst: function.instructions) {
		for (uint32_t i = 0; i < inst.word_count; ++i) {
			key = (key ^ inst.raw[i]) * 0x100000001b3ull;
		}
	}
	return key;
}

using Branches = std::vector<std::pair<uint32_t, uint32_t>>;

bool RelocateBranches(std::vector<uint32_t>& code, const Branches& branches,
                      std::vector<LongBranch>& long_branches) {
	// Relocation stays symbolic until every function has been inserted. An edge beyond the
	// 16-bit offset keeps offset 0 and becomes a long branch of the decoded program: branch
	// islands placed inside other callees' code made the expansion's control flow
	// unstructured (a goto into the middle of another body), and the structurizer then routed
	// every inlined callee through the same blocks.
	long_branches.clear();
	for (const auto [at, target]: branches) {
		const auto delta = int64_t(target) - int64_t(at) - 1;
		if (delta >= std::numeric_limits<int16_t>::min() &&
		    delta <= std::numeric_limits<int16_t>::max()) {
			code[at] = (code[at] & 0xffff0000u) | (static_cast<uint32_t>(delta) & 0xffffu);
		} else {
			code[at] &= 0xffff0000u;
			long_branches.push_back({at * 4u, target * 4u});
		}
	}
	std::ranges::sort(long_branches, {}, &LongBranch::pc);
	return true;
}

// The decoded shader: everything the expansion needs that depends only on its code.
struct ShaderAnalysis {
	std::vector<uint32_t> code;
	Program               program;
	std::set<uint32_t>    labels;
	bool                  has_calls = false;
	bool                  has_setpc = false;
	// DominatingWriter's answers (they depend on the code alone): (register, use, wave size).
	mutable std::map<std::tuple<uint32_t, size_t, uint32_t>, std::optional<size_t>> dominating_writers;
};

// What an instruction does to the register a reaching definition is looked for.
enum class Definition { None, Writes, Unknown };

// The instruction whose write of a register reaches instruction `use` on every path: walking back
// from `use`, every path meets that same writer before any other one and before the entry (a call
// returns to the instruction after it). `defines` tells what an instruction does to the register.
template <typename Defines>
std::optional<size_t> ReachingWriter(const Program& program, size_t use, const Defines& defines) {
	const auto& instructions = program.instructions;
	if (use >= instructions.size()) {
		return {};
	}
	std::unordered_map<uint32_t, size_t> index_of_pc;
	for (size_t i = 0; i < instructions.size(); i++) {
		index_of_pc.emplace(instructions[i].pc, i);
	}
	std::vector<std::vector<size_t>> predecessors(instructions.size());
	for (size_t i = 0; i < instructions.size(); i++) {
		const auto& inst = instructions[i];
		if (inst.opcode == Opcode::S_ENDPGM || inst.opcode == Opcode::S_SETPC_B64) {
			continue;
		}
		if (IsDirectBranch(inst.opcode)) {
			const auto target = index_of_pc.find(inst.branch_target);
			if (target == index_of_pc.end()) {
				return {};
			}
			predecessors[target->second].push_back(i);
			if (inst.opcode == Opcode::S_BRANCH) {
				continue;
			}
		}
		if (i + 1 < instructions.size()) {
			predecessors[i + 1].push_back(i);
		}
	}
	std::optional<size_t> writer;
	std::vector<bool>     seen(instructions.size(), false);
	std::vector<size_t>   pending {use};
	while (!pending.empty()) {
		const auto i = pending.back();
		pending.pop_back();
		if (i == 0) {
			return {}; // The entry: a path without a write.
		}
		for (const auto j: predecessors[i]) {
			if (seen[j]) {
				continue;
			}
			seen[j] = true;
			switch (defines(instructions[j])) {
				case Definition::Unknown: return {};
				case Definition::Writes:
					if (writer && *writer != j) {
						return {};
					}
					writer = j;
					break;
				case Definition::None: pending.push_back(j); break;
			}
		}
	}
	return writer;
}

// The instruction whose write of SGPR `reg` reaches instruction `use` on every path, with no other
// write of it in between: the register holds that write's value at `use` (Gran Turismo 7 loads the
// V# of its any-hit call table once, before its traversal loop, and reuses the SGPRs after it).
std::optional<size_t> DominatingWriter(const ShaderAnalysis& analysis, uint32_t reg, size_t use, uint32_t wave_size) {
	const auto key = std::tuple {reg, use, wave_size};
	if (const auto found = analysis.dominating_writers.find(key); found != analysis.dominating_writers.end()) {
		return found->second;
	}
	const auto writer = ReachingWriter(analysis.program, use, [&](const Instruction& inst) {
		return Writes(inst, reg, wave_size) ? Definition::Writes : Definition::None;
	});
	analysis.dominating_writers.emplace(key, writer);
	return writer;
}

// The V_WRITELANE_B32 <vgpr>, <sgpr>, `lane` whose write reaches instruction `use` on every path
// (other writes of the VGPR in between must be V_WRITELANE_B32s of other constant lanes): a
// V_READLANE_B32 of the lane at `use` reads the SGPR the compiler spilled there (Gran Turismo 7
// keeps the pointer to a table of V#s so).
std::optional<size_t> SpillWriter(const ShaderAnalysis& analysis, uint32_t vgpr, uint32_t lane, size_t use,
                                  uint32_t wave_size) {
	const auto key = std::tuple {0x80000000u | (vgpr << 8u) | lane, use, wave_size};
	if (const auto found = analysis.dominating_writers.find(key); found != analysis.dominating_writers.end()) {
		return found->second;
	}
	const auto writer = ReachingWriter(analysis.program, use, [&](const Instruction& inst) {
		if (!WritesVgpr(inst, vgpr)) {
			return Definition::None;
		}
		const auto written = inst.opcode == Opcode::V_WRITELANE_B32 && inst.dst.reg == vgpr &&
		                             inst.src1.kind != OperandKind::Null
		                         ? Immediate(inst.src1)
		                         : std::nullopt;
		if (!written) {
			return Definition::Unknown;
		}
		if (*written % wave_size != lane) {
			return Definition::None;
		}
		return inst.src0.kind == OperandKind::Sgpr ? Definition::Writes : Definition::Unknown;
	});
	analysis.dominating_writers.emplace(key, writer);
	return writer;
}

void AnalyzeShader(std::span<const uint32_t> code, ShaderAnalysis& analysis) {
	analysis.code.assign(code.begin(), code.end());
	if (std::none_of(code.begin(), code.end(), IsCall)) return;
	DecodeProgram(analysis.code, analysis.program);
	for (const auto& inst: analysis.program.instructions) {
		analysis.has_calls |= IsCall(inst.raw[0]);
		if (IsDirectBranch(inst.opcode)) analysis.labels.insert(inst.branch_target);
		analysis.has_setpc |= inst.opcode == Opcode::S_SETPC_B64;
	}
}

// Whether a function returns with SGPR `reg` holding the value it had at the call: it does not
// write the register, or every path restores it (Gran Turismo 7's any-hit functions save the
// SGPRs they use, s_mov_b32 s<n>, s84 at the entry and s_mov_b32 s84, s<n> before returning).
// Values are followed through S_MOV_B32/S_MOV_B64 and through VGPR lanes (V_WRITELANE_B32 and
// V_READLANE_B32 of constant lanes); any other write loses them.
bool ReturnsWithEntryValue(const Function& function, uint32_t reg, uint32_t wave_size) {
	const auto& instructions = function.instructions;
	if (std::ranges::none_of(instructions, [&](const Instruction& inst) { return Writes(inst, reg, wave_size); })) {
		return true;
	}
	// What each register holds: the entry value of an SGPR (Unknown: anything else). An SGPR
	// that is not in the map holds its own entry value; a VGPR lane that is not, nothing known.
	constexpr uint32_t Unknown = UINT32_MAX;
	struct State {
		std::map<uint32_t, uint32_t>                       sgprs;
		std::map<std::pair<uint32_t, uint32_t>, uint32_t> lanes;
		bool operator==(const State&) const = default;
	};
	const auto sgpr = [](const State& state, uint32_t r) {
		const auto found = state.sgprs.find(r);
		return found != state.sgprs.end() ? found->second : r;
	};
	const auto join = [&](State& into, const State& from) {
		State joined;
		std::set<uint32_t> keys;
		for (const auto& [r, value]: into.sgprs) keys.insert(r);
		for (const auto& [r, value]: from.sgprs) keys.insert(r);
		for (const auto r: keys) {
			const auto a = sgpr(into, r), b = sgpr(from, r);
			if (a != r || b != r) joined.sgprs[r] = a == b ? a : Unknown;
		}
		for (const auto& [lane, value]: into.lanes) {
			const auto other = from.lanes.find(lane);
			if (other != from.lanes.end() && other->second == value) joined.lanes[lane] = value;
		}
		const bool changed = !(joined == into);
		into               = std::move(joined);
		return changed;
	};
	std::unordered_map<uint32_t, size_t> index_of_pc;
	for (size_t i = 0; i < instructions.size(); i++) {
		index_of_pc.emplace(instructions[i].pc, i);
	}
	std::vector<std::optional<State>> in(instructions.size());
	in[0] = State {};
	std::vector<size_t> pending {0};
	while (!pending.empty()) {
		const auto i = pending.back();
		pending.pop_back();
		auto        state = *in[i];
		const auto& inst  = instructions[i];
		if (inst.opcode == Opcode::S_SETPC_B64) {
			if (sgpr(state, reg) != reg) {
				return false;
			}
			continue;
		}
		const auto lane_of = [&](const Operand& operand) -> std::optional<uint32_t> {
			const auto lane = operand.kind == OperandKind::Null ? std::nullopt : Immediate(operand);
			return lane ? std::optional<uint32_t> {*lane % wave_size} : std::nullopt;
		};
		if ((inst.opcode == Opcode::S_MOV_B32 || inst.opcode == Opcode::S_MOV_B64) && inst.dst.kind == OperandKind::Sgpr &&
		    inst.src0.kind == OperandKind::Sgpr) {
			const auto words = inst.opcode == Opcode::S_MOV_B64 ? 2u : 1u;
			std::array<uint32_t, 2> values {};
			for (uint32_t w = 0; w < words; w++) values[w] = sgpr(state, inst.src0.reg + w);
			for (uint32_t w = 0; w < words; w++) state.sgprs[inst.dst.reg + w] = values[w];
		} else if (inst.opcode == Opcode::V_WRITELANE_B32 && inst.dst.kind == OperandKind::Vgpr &&
		           inst.src0.kind == OperandKind::Sgpr && lane_of(inst.src1)) {
			state.lanes[{inst.dst.reg, *lane_of(inst.src1)}] = sgpr(state, inst.src0.reg);
		} else if (inst.opcode == Opcode::V_READLANE_B32 && inst.dst.kind == OperandKind::Sgpr &&
		           inst.src0.kind == OperandKind::Vgpr && lane_of(inst.src1)) {
			const auto found = state.lanes.find({inst.src0.reg, *lane_of(inst.src1)});
			state.sgprs[inst.dst.reg] = found != state.lanes.end() ? found->second : Unknown;
		} else {
			for (uint32_t r = 0; r < 128; r++) {
				if (Writes(inst, r, wave_size)) state.sgprs[r] = Unknown;
			}
			std::erase_if(state.lanes, [&](const auto& entry) { return WritesVgpr(inst, entry.first.first); });
		}
		const auto flow = [&](size_t to) {
			if (to >= instructions.size()) {
				return;
			}
			if (!in[to]) {
				in[to] = state;
				pending.push_back(to);
			} else if (join(*in[to], state)) {
				pending.push_back(to);
			}
		};
		if (IsDirectBranch(inst.opcode)) {
			const auto target = index_of_pc.find(inst.branch_target);
			if (target == index_of_pc.end()) {
				return false;
			}
			flow(target->second);
			if (inst.opcode == Opcode::S_BRANCH) {
				continue;
			}
		}
		flow(i + 1);
	}
	return true;
}

// One call instruction and every address it can reach.
struct CallSite {
	uint32_t              pc         = 0;
	uint32_t              source_reg = 0;
	uint32_t              return_reg = 0;
	bool                  dynamic    = false;
	std::vector<uint64_t> addresses; // ascending
	// The registers its targets were read through outside the call's block, each with the
	// instruction whose value of it was read: user data, a register written before the loop,
	// a VGPR (VgprHeld + n) whose lane held a spilled SGPR. A callee may write one only if no call
	// returns into a path that reaches that instruction (ascending).
	std::vector<std::pair<uint32_t, uint32_t>> consumed;

	bool operator==(const CallSite&) const = default;
};

// Resolves each call's targets from the user data and the memory holding call tables. Which
// registers it consults depends on the code alone; their values reach the result only through
// the targets.
bool ResolveCalls(const ShaderAnalysis& analysis, uint64_t base,
                  std::span<const uint32_t> user_data, const ShaderCodeReader& read,
                  uint32_t wave_size, std::vector<CallSite>& sites,
                  std::set<uint32_t>& dependencies, std::string& reason) {
	const auto& program = analysis.program;
	const auto& labels  = analysis.labels;
	std::set<std::pair<uint32_t, uint32_t>> consumed;
	const auto resolve = [&](auto&& self, uint32_t reg, size_t before, size_t first, uint32_t depth,
	                         bool load_memory = false) -> std::optional<uint32_t> {
		if (reg >= 128 || depth > 32) return {};
		const bool initial =
		    reg < user_data.size() &&
		    std::none_of(
		        program.instructions.begin(), program.instructions.begin() + before,
		        [&](const auto& inst) { return Writes(inst, reg, wave_size); });
		if (initial) {
			dependencies.insert(reg);
			consumed.emplace(reg, static_cast<uint32_t>(before));
			return user_data[reg];
		}
		const auto block_of = [&](size_t at) {
			while (at > 0 && !labels.contains(program.instructions[at].pc) &&
			       !IsDirectBranch(program.instructions[at - 1].opcode))
				--at;
			return at;
		};
		// Past the start of the block, a register the shader writes once, on every path to here
		// (only in memory loads: the value read stays what the shader saw).
		const auto earlier = [&]() -> std::optional<uint32_t> {
			const auto writer = load_memory ? DominatingWriter(analysis, reg, before, wave_size) : std::nullopt;
			if (!writer) return {};
			consumed.emplace(reg, static_cast<uint32_t>(before));
			return self(self, reg, *writer + 1u, block_of(*writer), depth + 1, load_memory);
		};
		for (size_t i = before; i > first;) {
			const auto& inst = program.instructions[--i];
			if (IsDirectBranch(inst.opcode) || IsCall(inst.raw[0])) return {};
			if (Writes(inst, reg, wave_size)) {
				if (load_memory && inst.opcode == Opcode::V_READLANE_B32 && inst.src0.kind == OperandKind::Vgpr) {
					// An SGPR spilled into a lane of a VGPR, read back: the SGPR at the spill.
					const auto lane  = Immediate(inst.src1);
					const auto spill = lane ? SpillWriter(analysis, inst.src0.reg, *lane % wave_size, i, wave_size)
					                        : std::nullopt;
					if (!spill) return {};
					consumed.emplace(VgprHeld + inst.src0.reg, static_cast<uint32_t>(i));
					return self(self, program.instructions[*spill].src0.reg, *spill, block_of(*spill), depth + 1,
					            load_memory);
				}
				if (inst.opcode == Opcode::S_GETPC_B64 && inst.dst.kind == OperandKind::Sgpr) {
					const auto pc = base + inst.pc + 4u;
					return static_cast<uint32_t>(pc >> ((reg - inst.dst.reg) * 32u));
				}
				if (load_memory && IsScalarLoad(inst.opcode) &&
				    inst.src0.kind == OperandKind::Sgpr) {
					const auto low    = self(self, inst.src0.reg, i, first, depth + 1, true);
					const auto high   = self(self, inst.src0.reg + 1u, i, first, depth + 1, true);
					const auto offset = inst.src1.kind == OperandKind::Sgpr
					                        ? self(self, inst.src1.reg, i, first, depth + 1, true)
					                        : Immediate(inst.src1);
					if (!low || !high || !offset) return {};
					const uint64_t          address     = (uint64_t {*high} << 32u) | *low;
					const uint32_t          byte_offset = *offset + inst.offset;
					std::array<uint32_t, 1> value {};
					if (!read(address + byte_offset + (reg - inst.dst.reg) * 4u, value)) return {};
					return value[0];
				}
				if ((inst.opcode != Opcode::S_MOV_B32 && inst.opcode != Opcode::S_MOV_B64) ||
				    inst.dst.kind != OperandKind::Sgpr)
					return {};
				if (inst.src0.kind == OperandKind::Sgpr) {
					return self(self, inst.src0.reg + reg - inst.dst.reg, i, first, depth + 1,
					            load_memory);
				}
				if (inst.opcode == Opcode::S_MOV_B32 &&
				    (inst.src0.kind == OperandKind::LiteralConstant ||
				     inst.src0.kind == OperandKind::IntegerInlineConstant))
					return inst.src0.value;
				return {};
			}
			if (labels.contains(inst.pc)) return earlier();
		}
		return earlier();
	};
	// Follow only straight-line scalar copies back to one buffer load. The
	// runtime load and its data arguments are retained in the expanded program.
	const auto origin = [&](auto&& self, uint32_t reg, size_t before, size_t first,
	                        uint32_t depth) -> std::optional<std::pair<size_t, uint32_t>> {
		if (reg >= 105u || depth > 32) return {};
		for (size_t i = before; i > first;) {
			const auto& inst = program.instructions[--i];
			if (IsDirectBranch(inst.opcode) || IsCall(inst.raw[0])) return {};
			if (Writes(inst, reg, wave_size)) {
				if (IsScalarBufferLoad(inst.opcode)) return std::pair {i, reg - inst.dst.reg};
				if ((inst.opcode == Opcode::S_MOV_B32 || inst.opcode == Opcode::S_MOV_B64) &&
				    inst.src0.kind == OperandKind::Sgpr)
					return self(self, inst.src0.reg + reg - inst.dst.reg, i, first, depth + 1);
				return {};
			}
			if (labels.contains(inst.pc)) return {};
		}
		return {};
	};
	for (size_t i = 0; i < program.instructions.size(); i++) {
		const auto& inst = program.instructions[i];
		if (!IsCall(inst.raw[0])) continue;
		const auto src   = inst.raw[0] & 255u;
		const auto dst   = (inst.raw[0] >> 16u) & 127u;
		size_t     first = i;
		while (first > 0 && !labels.contains(program.instructions[first].pc) &&
		       !IsDirectBranch(program.instructions[first - 1].opcode))
			--first;
		consumed.clear();
		const auto low  = resolve(resolve, src, i, first, 0);
		const auto high = resolve(resolve, src + 1u, i, first, 0);
		if (src >= 105u || dst >= 105u) {
			reason = "shader call uses a special scalar register pair";
			return false;
		}
		const bool         dynamic = !low || !high;
		std::set<uint64_t> addresses;
		if (!dynamic) {
			addresses.insert((uint64_t {*high} << 32u) | *low);
		} else {
			const auto lo = origin(origin, src, i, first, 0);
			const auto hi = origin(origin, src + 1u, i, first, 0);
			if (!lo || !hi || lo->first != hi->first || hi->second != lo->second + 1u) {
				reason = "shader call target is not a scalar buffer table load";
				return false;
			}
			const auto& load = program.instructions[lo->first];
			if (load.src0.kind != OperandKind::Sgpr) {
				reason = "shader call table descriptor is not a scalar register tuple";
				return false;
			}
			ShaderBufferResource descriptor;
			for (uint32_t word = 0; word < 4; ++word) {
				const auto value =
				    resolve(resolve, load.src0.reg + word, lo->first, first, 0, true);
				if (!value) {
					reason = "shader call table descriptor is not statically readable";
					return false;
				}
				descriptor.fields[word] = *value;
			}
			// Prove the dynamic byte offset's alignment from its last definition.
			// This bounds all possible target slots without assuming a runtime index.
			uint32_t   shift      = 0;
			const auto offset_reg = ScalarIndex(load.src1);
			for (size_t at = lo->first; at > first;) {
				const auto& def = program.instructions[--at];
				if (IsDirectBranch(def.opcode) || IsCall(def.raw[0])) break;
				const auto dst_reg  = ScalarIndex(def.dst);
				const auto dst2_reg = ScalarIndex(def.dst2);
				if (offset_reg && dst_reg && *offset_reg >= *dst_reg &&
				    *offset_reg < *dst_reg + DestinationWords(def, wave_size)) {
					if (def.opcode == Opcode::S_LSHL_B32 && *offset_reg == *dst_reg) {
						if (const auto amount = Immediate(def.src1)) shift = *amount & 31u;
					}
					break;
				}
				if (offset_reg && dst2_reg && *offset_reg >= *dst2_reg &&
				    *offset_reg < *dst2_reg + wave_size / 32u)
					break;
				if (labels.contains(def.pc)) break;
			}
			const uint64_t size = descriptor.GetSize();
			if (shift < 3u || shift > 16u || size == 0 || size > MaxCallTableBytes ||
			    descriptor.Base48() == 0 || descriptor.Type() != 0 || descriptor.SwizzleEnabled() ||
			    descriptor.AddTid() || (descriptor.Base48() & 3u) != 0 || (load.offset & 3u) != 0) {
				reason = "shader call table needs unsupported bounds or addressing";
				return false;
			}
			std::vector<uint32_t> table(size / 4u);
			if (!read(descriptor.Base48(), table)) {
				reason = "shader call table is not fully mapped";
				return false;
			}
			const uint64_t step       = uint64_t {1} << shift;
			const uint64_t first_slot = (load.offset % step) + lo->second * 4u;
			for (uint64_t offset = first_slot; offset + 8u <= size; offset += step) {
				const uint64_t address =
				    table[offset / 4u] | (uint64_t {table[offset / 4u + 1u]} << 32u);
				if (address == 0) continue; // A runtime null target takes the explicit trap path.
				addresses.insert(address);
			}
			if (addresses.size() > MaxCallTableTargets) {
				reason = "shader call table exceeds the bounded target count (" + std::to_string(addresses.size()) +
				         " targets in " + std::to_string((size - first_slot + step - 1) / step) + " slots of " +
				         std::to_string(step) + " bytes)";
				return false;
			}
		}
		sites.push_back({inst.pc, src, dst, dynamic, {addresses.begin(), addresses.end()},
		                 {consumed.begin(), consumed.end()}});
	}
	return true;
}

// The instructions from which a call site can be reached (the call sites included), following the
// shader's own branches; a call returns to the instruction after it.
std::vector<bool> ReachesCall(const Decoder::Program& program, const std::map<uint32_t, Call>& calls) {
	const auto&                 instructions = program.instructions;
	std::map<uint32_t, size_t>  index_of_pc;
	for (size_t i = 0; i < instructions.size(); i++) {
		index_of_pc[instructions[i].pc] = i;
	}
	std::vector<std::vector<size_t>> predecessors(instructions.size());
	const auto edge = [&](size_t from, size_t to) {
		if (to < instructions.size()) predecessors[to].push_back(from);
	};
	for (size_t i = 0; i < instructions.size(); i++) {
		const auto& inst = instructions[i];
		if (inst.opcode == Opcode::S_ENDPGM || inst.opcode == Opcode::S_SETPC_B64) continue;
		if (IsDirectBranch(inst.opcode)) {
			if (const auto target = index_of_pc.find(inst.branch_target); target != index_of_pc.end()) {
				edge(i, target->second);
			}
			if (inst.opcode == Opcode::S_BRANCH) continue;
		}
		edge(i, i + 1u);
	}
	std::vector<bool>   reaches(instructions.size(), false);
	std::vector<size_t> pending;
	for (const auto& [pc, call]: calls) {
		if (const auto at = index_of_pc.find(pc); at != index_of_pc.end() && !reaches[at->second]) {
			reaches[at->second] = true;
			pending.push_back(at->second);
		}
	}
	while (!pending.empty()) {
		const auto current = pending.back();
		pending.pop_back();
		for (const auto previous: predecessors[current]) {
			if (!reaches[previous]) {
				reaches[previous] = true;
				pending.push_back(previous);
			}
		}
	}
	return reaches;
}

// Reads the callees and emits the expanded program: a function of the code, the resolved
// targets, the consulted registers and the callee code.
bool BuildExpansion(const ShaderAnalysis& analysis, uint64_t base,
                    const std::vector<CallSite>& sites, const std::set<uint32_t>& dependencies,
                    const ShaderCodeReader& read, uint32_t wave_size,
                    std::vector<uint32_t>& expanded, std::vector<LongBranch>& long_branches,
                    std::string& reason, std::vector<InlinedCallBody>* bodies = nullptr,
                    ExpansionWords* runtime = nullptr) {
	if (bodies != nullptr) bodies->clear();
	if (runtime != nullptr) *runtime = {};
	long_branches.clear();
	const auto&              program = analysis.program;
	std::map<uint32_t, Call> calls;
	for (const auto& site: sites) {
		// Without runtime words every target gets a body of its own.
		if (site.addresses.size() > MaxCallTargets && runtime == nullptr) {
			reason = "shader call table exceeds the bounded target count";
			return false;
		}
		Call call {site.source_reg, site.return_reg, site.dynamic, {}};
		std::string unreadable;
		size_t      left_out = 0;
		for (const auto address: site.addresses) {
			CallTarget target {address, {}};
			std::string why;
			if ((address & 3u) != 0 || !ReadFunction(address, site.return_reg, read, target.function, why, wave_size)) {
				if (why.empty()) why = "shader call target is not DWORD aligned";
				// A table also holds addresses that are not functions (Marvel's Wolverine's lists the
				// code of pixel shaders, which end the wave): of a call through a table they are left
				// out of the expansion, like a function that overwrites a dependency (its rows, if one
				// were reached, end at the miss trap). A call through an immutable pointer fails.
				if (!site.dynamic) {
					reason = why;
					return false;
				}
				if (left_out++ == 0) unreadable = fmt::format("{} (function at 0x{:x})", why, address);
				continue;
			}
			call.targets.push_back(std::move(target));
		}
		if (call.targets.empty()) {
			reason = unreadable.empty() ? "shader call table has no target" : unreadable;
			return false;
		}
		if (left_out != 0) {
			static std::atomic<uint32_t> reported = 0;
			if (reported.fetch_add(1) < 8) {
				LOGF("Shader call table at pc 0x%x: %zu of %zu targets are not functions and are left out; first: %s\n",
				     site.pc, left_out, site.addresses.size(), unreadable.c_str());
			}
		}
		calls.emplace(site.pc, std::move(call));
	}
	const bool has_backedge =
	    std::any_of(program.instructions.begin(), program.instructions.end(), [](const auto& inst) {
		    return IsDirectBranch(inst.opcode) && inst.branch_target <= inst.pc;
	    });
	if (has_backedge) {
		// A call sees the registers its target depends on as the shader entered with them unless
		// a write to one can reach the call: a write after the last call of the loop that no path
		// leads back from (the shader reloads the register for its own later use) cannot change
		// any target.
		const auto reaches_call = ReachesCall(program, calls);
		for (const auto reg: dependencies) {
			for (size_t i = 0; i < program.instructions.size(); i++) {
				const auto& inst = program.instructions[i];
				if (reaches_call[i] && Writes(inst, reg, wave_size)) {
					reason = "shader loop may overwrite a call-target dependency s" +
					         std::to_string(reg) + " at pc " + std::to_string(inst.pc);
					return false;
				}
			}
		}
	}
	// A callee must not invalidate the values the targets were resolved from: a register a callee
	// writes must not reach, from the return of its call, an instruction whose value of it was read
	// (the shader's own writes of it on the way end the path). A function that saves an SGPR and
	// restores it on every path leaves its value. Of a call through a table, a function that does
	// overwrite one is not a target: after it the caller would read its next target through the
	// overwritten registers, on the console too (Gran Turismo 7's table also names functions that
	// load their own descriptors over the caller's table V#). It is left out of the expansion, and
	// its rows, if one were reached, end at the miss trap. A call through an immutable pointer, or
	// a table with no target left, fails.
	{
		const auto writes = [&](const Instruction& inst, uint32_t reg) {
			return reg >= VgprHeld ? WritesVgpr(inst, reg - VgprHeld) : Writes(inst, reg, wave_size);
		};
		std::unordered_map<uint32_t, size_t> index_of_pc;
		for (size_t i = 0; i < program.instructions.size(); i++) {
			index_of_pc.emplace(program.instructions[i].pc, i);
		}
		std::set<std::pair<uint32_t, uint32_t>> consumed;
		for (const auto& site: sites) {
			consumed.insert(site.consumed.begin(), site.consumed.end());
		}
		std::map<std::pair<std::vector<uint32_t>, uint32_t>, bool> preserved;
		// The instruction of `target` that leaves `reg` overwritten, if any.
		const auto overwrite = [&](const CallTarget& target, uint32_t reg) -> const Instruction* {
			const auto found = std::ranges::find_if(target.function.instructions,
			                                        [&](const Instruction& inst) { return writes(inst, reg); });
			if (found == target.function.instructions.end()) {
				return nullptr;
			}
			if (reg < VgprHeld) {
				const auto key   = std::pair {target.function.code, reg};
				auto       known = preserved.find(key);
				if (known == preserved.end()) {
					known = preserved.emplace(key, ReturnsWithEntryValue(target.function, reg, wave_size)).first;
				}
				if (known->second) {
					return nullptr;
				}
			}
			return &*found;
		};
		// Whether the instruction at `at` is reached from `from` on a path that does not write `reg`.
		const auto reaches = [&](size_t from, uint32_t reg, size_t at) {
			std::vector<bool>   seen(program.instructions.size(), false);
			std::vector<size_t> pending {from};
			while (!pending.empty()) {
				const auto i = pending.back();
				pending.pop_back();
				if (i >= program.instructions.size() || seen[i]) {
					continue;
				}
				seen[i] = true;
				if (i == at) {
					return true;
				}
				const auto& inst = program.instructions[i];
				if (writes(inst, reg) || inst.opcode == Opcode::S_ENDPGM || inst.opcode == Opcode::S_SETPC_B64) {
					continue;
				}
				if (IsDirectBranch(inst.opcode)) {
					if (const auto target = index_of_pc.find(inst.branch_target); target != index_of_pc.end()) {
						pending.push_back(target->second);
					}
					if (inst.opcode == Opcode::S_BRANCH) {
						continue;
					}
				}
				pending.push_back(i + 1u);
			}
			return false;
		};
		const auto name = [](uint32_t reg) {
			return reg >= VgprHeld ? "v" + std::to_string(reg - VgprHeld) : "s" + std::to_string(reg);
		};
		for (auto& [pc, call]: calls) {
			const auto call_index = index_of_pc.at(pc);
			// The registers whose read the return of the call reaches.
			std::vector<std::pair<uint32_t, uint32_t>> exposed;
			for (const auto& [reg, at]: consumed) {
				if (std::ranges::any_of(call.targets, [&](const CallTarget& target) { return overwrite(target, reg); }) &&
				    reaches(call_index + 1u, reg, at)) {
					exposed.emplace_back(reg, at);
				}
			}
			if (exposed.empty()) {
				continue;
			}
			std::vector<CallTarget> kept;
			std::string             first;
			for (auto& target: call.targets) {
				const auto clobbered = std::ranges::find_if(exposed, [&](const auto& entry) {
					return overwrite(target, entry.first) != nullptr;
				});
				if (clobbered == exposed.end()) {
					kept.push_back(std::move(target));
				} else if (first.empty()) {
					first = name(clobbered->first) + ", read at pc " +
					        std::to_string(program.instructions[clobbered->second].pc) + ", written at +" +
					        std::to_string(overwrite(target, clobbered->first)->pc) + " of the function at " +
					        std::to_string(target.address);
				}
			}
			if (!call.dynamic || kept.empty()) {
				reason = "shader function modifies a call-target dependency (" + first + ", call at pc " +
				         std::to_string(pc) + ")";
				return false;
			}
			call.targets = std::move(kept);
		}
	}
	std::vector<uint32_t>                      result;
	// Address literals: with runtime words, the code holds a word index and the address goes to
	// the words (the shader reads it at run time).
	const auto address_literal = [&](uint32_t at, uint32_t value) {
		if (runtime == nullptr) return value;
		runtime->literals.push_back({at * 4u, static_cast<uint32_t>(runtime->words.size())});
		runtime->words.push_back(value);
		return runtime->literals.back().word;
	};
	const auto move_address = [&](uint32_t reg, uint32_t value) {
		const auto at = static_cast<uint32_t>(result.size());
		result.push_back(0xbe8003ffu | (reg << 16u)); // S_MOV_B32 s[reg], literal
		result.push_back(address_literal(at, value));
	};
	const auto compare_address = [&](uint32_t reg, uint32_t value) {
		const auto at = static_cast<uint32_t>(result.size());
		result.push_back(0xbf06ff00u | reg); // S_CMP_EQ_U32 s[reg], literal
		result.push_back(address_literal(at, value));
	};
	std::map<uint32_t, uint32_t>               main_pc;
	std::vector<std::pair<uint32_t, uint32_t>> main_branches;
	Branches                                   relocations;
	for (const auto& inst: program.instructions) {
		main_pc[inst.pc] = static_cast<uint32_t>(result.size());
		const auto found = calls.find(inst.pc);
		if (found == calls.end()) {
			if (inst.opcode == Opcode::S_ENDPGM) {
				result.push_back(0xbf810000u);
			} else if (inst.opcode == Opcode::S_GETPC_B64) {
				if (!EmitOriginalPc(result, inst, base, reason, move_address)) return false;
			} else {
				if (IsDirectBranch(inst.opcode))
					main_branches.emplace_back(result.size(), inst.branch_target);
				result.insert(result.end(), inst.raw, inst.raw + inst.word_count);
			}
		} else {
			const auto&           call      = found->second;
			const uint64_t        return_pc = base + inst.pc + inst.word_count * 4u;
			std::vector<uint32_t> returns;
			auto                  emit_target = [&](const CallTarget& target,
			                                    std::span<const CallTarget* const> aliases = {}) -> bool {
				const auto body_begin = static_cast<uint32_t>(result.size());
				move_address(call.return_reg, static_cast<uint32_t>(return_pc));
				move_address(call.return_reg + 1u, static_cast<uint32_t>(return_pc >> 32u));
				std::map<uint32_t, uint32_t>               local_pc;
				std::vector<std::pair<uint32_t, uint32_t>> branches;
				for (const auto& body: target.function.instructions) {
					const auto at     = static_cast<uint32_t>(result.size());
					local_pc[body.pc] = at;
					if (body.opcode == Opcode::S_SETPC_B64) {
						returns.push_back(at);
						result.push_back(0xbf820000u); // S_BRANCH to the caller's continuation.
					} else if (body.opcode == Opcode::S_GETPC_B64) {
						if (!EmitOriginalPc(result, body, target.address, reason, move_address)) return false;
					} else {
						if (IsDirectBranch(body.opcode))
							branches.emplace_back(at, body.branch_target);
						result.insert(result.end(), body.raw, body.raw + body.word_count);
					}
				}
				for (const auto& [at, target]: branches) {
					relocations.emplace_back(at, local_pc.at(target));
				}
				if (bodies != nullptr) {
					bodies->push_back({body_begin * 4u, static_cast<uint32_t>(result.size()) * 4u,
					                   target.address});
					for (const auto* alias: aliases) bodies->back().aliases.push_back(alias->address);
				}
				return true;
			};
			if (call.dynamic) {
				// SWAPPC preserves SCC, which the decision tree's compares overwrite. When a
				// target, or the caller after a target that leaves SCC untouched, may read it,
				// keep separate decision trees and callee bodies for its two incoming values
				// (without borrowing a guest SGPR); otherwise one tree serves both.
				// Group by the high address word so each low-word comparison has
				// one failure edge, rather than two branches sharing a later arm.
				const auto call_index = static_cast<size_t>(
				    std::ranges::find(program.instructions, inst.pc, &Instruction::pc) -
				    program.instructions.begin());
				const bool after_call = SccLiveAt(program.instructions, call_index + 1u, true);
				const bool keep_scc   = std::ranges::any_of(call.targets, [&](const CallTarget& target) {
					return SccLiveAt(target.function.instructions, 0, after_call);
				});
				const auto select_scc = static_cast<uint32_t>(result.size());
				if (keep_scc) result.push_back(0xbf850000u);
				// With runtime words the targets follow their code, not their addresses (the
				// game loads its functions in a different order on each start), and each
				// compares both address words, which the shader reads at run time.
				// Targets with the same code share one body: each one's compares branch to it (a
				// function that reads its own address, S_GETPC_B64, keeps a body of its own).
				std::vector<std::vector<const CallTarget*>> groups;
				if (runtime != nullptr) {
					std::vector<const CallTarget*> ordered;
					for (const auto& target: call.targets) ordered.push_back(&target);
					std::ranges::stable_sort(ordered, {}, [](const CallTarget* target) {
						return FunctionContentKey(target->function);
					});
					const auto same_code = [](const Function& a, const Function& b) {
						return std::ranges::equal(a.instructions, b.instructions, [](const auto& x, const auto& y) {
							return x.word_count == y.word_count && std::equal(x.raw, x.raw + x.word_count, y.raw);
						});
					};
					const auto reads_pc = [](const Function& function) {
						return std::ranges::any_of(function.instructions, [](const Instruction& inst) {
							return inst.opcode == Opcode::S_GETPC_B64;
						});
					};
					for (const auto* target: ordered) {
						if (!groups.empty() && !reads_pc(target->function) &&
						    FunctionContentKey(groups.back().front()->function) == FunctionContentKey(target->function) &&
						    same_code(groups.back().front()->function, target->function)) {
							groups.back().push_back(target);
						} else {
							groups.push_back({target});
						}
					}
					if (groups.size() > MaxCallTargets) {
						reason = "shader call table exceeds the bounded body count (" + std::to_string(groups.size()) +
						         " bodies for " + std::to_string(call.targets.size()) + " targets)";
						return false;
					}
				}
				for (uint32_t scc = 0; runtime != nullptr && scc < (keep_scc ? 2u : 1u); ++scc) {
					if (scc != 0) relocations.emplace_back(select_scc, result.size());
					for (const auto& group: groups) {
						// Each target but the last branches to the body; the last one falls into it.
						std::vector<uint32_t> hits;
						for (size_t member = 0; member < group.size(); ++member) {
							const auto* target = group[member];
							compare_address(call.source_reg + 1u, static_cast<uint32_t>(target->address >> 32u));
							const auto high_miss = static_cast<uint32_t>(result.size());
							result.push_back(0xbf840000u);
							compare_address(call.source_reg, static_cast<uint32_t>(target->address));
							const auto low_miss = static_cast<uint32_t>(result.size());
							result.push_back(0xbf840000u);
							if (keep_scc) result.push_back(scc != 0 ? 0xbf068080u : 0xbf068180u);
							if (member + 1u < group.size()) {
								hits.push_back(static_cast<uint32_t>(result.size()));
								result.push_back(0xbf820000u); // S_BRANCH to the body.
								relocations.emplace_back(high_miss, result.size());
								relocations.emplace_back(low_miss, result.size());
								continue;
							}
							for (const auto at: hits) relocations.emplace_back(at, result.size());
							if (!emit_target(*group.front(), std::span(group).subspan(1))) return false;
							relocations.emplace_back(high_miss, result.size());
							relocations.emplace_back(low_miss, result.size());
						}
					}
					result.push_back(0xbf920000u | ShaderCallMissTrapCode);
					result.push_back(0xbf810000u);
				}
				for (uint32_t scc = 0; runtime == nullptr && scc < (keep_scc ? 2u : 1u); ++scc) {
					if (scc != 0) relocations.emplace_back(select_scc, result.size());
					std::map<uint32_t, std::vector<const CallTarget*>> groups;
					for (const auto& target: call.targets)
						groups[static_cast<uint32_t>(target.address >> 32u)].push_back(&target);
					for (const auto& [high, targets]: groups) {
						result.push_back(0xbf06ff00u | (call.source_reg + 1u));
						result.push_back(high);
						const auto high_miss = static_cast<uint32_t>(result.size());
						result.push_back(0xbf840000u);
						for (const auto* target: targets) {
							result.push_back(0xbf06ff00u | call.source_reg);
							result.push_back(static_cast<uint32_t>(target->address));
							const auto low_miss = static_cast<uint32_t>(result.size());
							result.push_back(0xbf840000u);
							if (keep_scc) result.push_back(scc != 0 ? 0xbf068080u : 0xbf068180u);
							if (!emit_target(*target)) return false;
							relocations.emplace_back(low_miss, result.size());
						}
						result.push_back(0xbf920000u | ShaderCallMissTrapCode);
						result.push_back(0xbf810000u);
						relocations.emplace_back(high_miss, result.size());
					}
					result.push_back(0xbf920000u | ShaderCallMissTrapCode);
					result.push_back(0xbf810000u);
				}
			} else {
				for (const auto& target: call.targets)
					if (!emit_target(target)) return false;
			}
			for (const auto at: returns)
				relocations.emplace_back(at, result.size());
		}
		if (result.size() > MaxExpandedWords) {
			reason = "expanded shader exceeds the bounded code size (" +
			         std::to_string(result.size()) + " words so far)";
			return false;
		}
	}
	for (const auto& [at, target]: main_branches) {
		const auto found = main_pc.find(target);
		if (found == main_pc.end()) {
			reason = "shader branches outside its instruction range";
			return false;
		}
		relocations.emplace_back(at, found->second);
	}
	if (!RelocateBranches(result, relocations, long_branches)) return false;
	expanded = std::move(result);
	return true;
}

using Reads = std::vector<std::pair<uint64_t, std::vector<uint32_t>>>;

// Merges contiguous and overlapping reads into ranges (overlapping reads saw the same memory),
// so that validating them takes a few reads instead of one per 64-word chunk.
void MergeReads(Reads& reads) {
	std::sort(reads.begin(), reads.end(),
	          [](const auto& a, const auto& b) { return a.first < b.first; });
	Reads merged;
	for (auto& [address, words]: reads) {
		if (!merged.empty()) {
			auto&      last = merged.back();
			const auto end  = last.first + last.second.size() * sizeof(uint32_t);
			if (address <= end) {
				const auto skip = (end - address) / sizeof(uint32_t);
				if (skip < words.size()) {
					last.second.insert(last.second.end(),
					                   words.begin() + static_cast<std::ptrdiff_t>(skip),
					                   words.end());
				}
				continue;
			}
		}
		merged.emplace_back(address, std::move(words));
	}
	reads = std::move(merged);
}

} // namespace

bool InlineShaderFunctions(std::span<const uint32_t> code, uint64_t base,
                           std::span<const uint32_t> user_data, const ShaderCodeReader& read,
                           std::vector<uint32_t>& expanded, std::vector<LongBranch>& long_branches,
                           std::string& reason, uint32_t wave_size,
                           std::vector<uint32_t>* consulted_user_data) {
	expanded.clear();
	long_branches.clear();
	reason.clear();
	if (consulted_user_data != nullptr) {
		consulted_user_data->clear();
	}
	if (wave_size != 32u && wave_size != 64u) {
		reason = "shader calls require a known wave size";
		return false;
	}
	ShaderAnalysis analysis;
	AnalyzeShader(code, analysis);
	if (!analysis.has_calls) return true;
	if (analysis.has_setpc) {
		reason = "shader calls combined with indirect branches are unsupported";
		return false;
	}
	std::vector<CallSite> sites;
	std::set<uint32_t>    dependencies;
	const bool            ok =
	    ResolveCalls(analysis, base, user_data, read, wave_size, sites, dependencies, reason) &&
	    BuildExpansion(analysis, base, sites, dependencies, read, wave_size, expanded, long_branches,
	                   reason);
	if (consulted_user_data != nullptr) {
		consulted_user_data->assign(dependencies.begin(), dependencies.end());
	}
	return ok;
}

struct ShaderFunctionExpander::Impl {
	struct Expansion {
		uint64_t              base            = 0;
		uint32_t              wave_size       = 0;
		size_t                user_data_count = 0;
		std::vector<CallSite> sites;
		Reads                 reads;  // the callee code, as read
		Reads                 failed; // reads that failed: (address, requested words)
		bool                  ok = false;
		std::string           reason;
		std::vector<uint32_t> code;
		std::vector<LongBranch> long_branches;
		std::vector<InlinedCallBody> bodies;
		ExpansionWords              runtime;
	};
	struct Shader {
		uint64_t               code_hash = 0;
		ShaderAnalysis         analysis;
		std::vector<Expansion> expansions; // most recent last
		// The user-data registers the last complete call resolution consulted (code-dependent
		// only); valid while consulted_known.
		std::vector<uint32_t> consulted;
		uint32_t              consulted_wave  = 0;
		bool                  consulted_known = false;
	};
	static constexpr size_t MaxExpansions = 8;

	std::unordered_map<uint64_t, Shader> shaders; // by code address
	std::vector<InlinedCallBody>         bodies;  // of the last expansion
	std::vector<LongBranch>              long_branches; // of the last expansion
	ExpansionWords                       runtime;       // of the last expansion
	std::vector<uint32_t>                scratch;

	bool StillReads(const Expansion& expansion, const ShaderCodeReader& read) {
		for (const auto& [address, words]: expansion.reads) {
			scratch.resize(words.size());
			if (!read(address, scratch) || scratch != words) return false;
		}
		for (const auto& [address, words]: expansion.failed) {
			scratch.resize(words.size());
			if (read(address, scratch)) return false;
		}
		return true;
	}
};

ShaderFunctionExpander::ShaderFunctionExpander(): m_impl(std::make_unique<Impl>()) {}

const std::vector<InlinedCallBody>& ShaderFunctionExpander::Bodies() const {
	return m_impl->bodies;
}

const std::vector<LongBranch>& ShaderFunctionExpander::LongBranches() const {
	return m_impl->long_branches;
}

const std::vector<RuntimeLiteral>& ShaderFunctionExpander::RuntimeLiterals() const {
	return m_impl->runtime.literals;
}

const std::vector<uint32_t>& ShaderFunctionExpander::RuntimeWords() const {
	return m_impl->runtime.words;
}
ShaderFunctionExpander::~ShaderFunctionExpander() = default;

bool ShaderFunctionExpander::HasCalls(std::span<const uint32_t> code) const {
	const auto found = m_impl->shaders.find(reinterpret_cast<uint64_t>(code.data()));
	if (found == m_impl->shaders.end() || found->second.analysis.code.size() != code.size()) {
		return true;
	}
	return found->second.analysis.has_calls || found->second.analysis.has_setpc;
}

const std::vector<uint32_t>* ShaderFunctionExpander::ConsultedUserData(
    std::span<const uint32_t> code, uint64_t code_hash, uint32_t wave_size) const {
	const auto found = m_impl->shaders.find(reinterpret_cast<uint64_t>(code.data()));
	if (code_hash == 0 || found == m_impl->shaders.end() || !found->second.consulted_known ||
	    found->second.consulted_wave != wave_size ||
	    found->second.code_hash != code_hash ||
	    found->second.analysis.code.size() != code.size()) {
		return nullptr;
	}
	return &found->second.consulted;
}

bool ShaderFunctionExpander::Expand(std::span<const uint32_t> code, uint64_t base,
                                    std::span<const uint32_t> user_data,
                                    const ShaderCodeReader& read, std::vector<uint32_t>& expanded,
                                    std::string& reason, uint32_t wave_size, uint64_t code_hash) {
	expanded.clear();
	reason.clear();
	m_impl->long_branches.clear();
	if (wave_size != 32u && wave_size != 64u) {
		reason = "shader calls require a known wave size";
		return false;
	}
	auto& shader = m_impl->shaders[reinterpret_cast<uint64_t>(code.data())];
	// Comparing the whole code on every dispatch was ~4 us a dispatch (6 % of the GPU thread in
	// the save's opening fight, 2026-10-09); the content hash names the same code.
	const bool same_hash = code_hash != 0 && shader.code_hash == code_hash &&
	                       shader.analysis.code.size() == code.size();
	if (!same_hash && (shader.analysis.code.size() != code.size() ||
	                   !std::equal(code.begin(), code.end(), shader.analysis.code.begin()))) {
		shader = {};
		AnalyzeShader(code, shader.analysis);
	}
	shader.code_hash = code_hash;
	const auto& analysis = shader.analysis;
	if (!analysis.has_calls) return true;
	if (analysis.has_setpc) {
		reason = "shader calls combined with indirect branches are unsupported";
		return false;
	}
	std::vector<CallSite> sites;
	std::set<uint32_t>    dependencies;
	if (!ResolveCalls(analysis, base, user_data, read, wave_size, sites, dependencies, reason)) {
		shader.consulted_known = false;
		return false;
	}
	shader.consulted.assign(dependencies.begin(), dependencies.end());
	shader.consulted_wave  = wave_size;
	shader.consulted_known = true;
	for (auto& expansion: shader.expansions) {
		if (expansion.base == base && expansion.wave_size == wave_size &&
		    expansion.user_data_count == user_data.size() && expansion.sites == sites &&
		    m_impl->StillReads(expansion, read)) {
			expanded = expansion.code;
			reason   = expansion.reason;
			m_impl->bodies        = expansion.bodies;
			m_impl->long_branches = expansion.long_branches;
			m_impl->runtime       = expansion.runtime;
			return expansion.ok;
		}
	}
	Impl::Expansion expansion;
	expansion.base            = base;
	expansion.wave_size       = wave_size;
	expansion.user_data_count = user_data.size();
	expansion.sites           = std::move(sites);
	const ShaderCodeReader recording = [&](uint64_t address, std::span<uint32_t> words) {
		const bool ok = read(address, words);
		(ok ? expansion.reads : expansion.failed)
		    .emplace_back(address, std::vector<uint32_t>(words.begin(), words.end()));
		return ok;
	};
	expansion.ok = BuildExpansion(analysis, base, expansion.sites, dependencies, recording,
	                              wave_size, expansion.code, expansion.long_branches,
	                              expansion.reason, &expansion.bodies, &expansion.runtime);
	m_impl->bodies        = expansion.bodies;
	m_impl->long_branches = expansion.long_branches;
	m_impl->runtime       = expansion.runtime;
	MergeReads(expansion.reads);
	expanded = expansion.code;
	reason   = expansion.reason;
	if (shader.expansions.size() >= Impl::MaxExpansions) {
		shader.expansions.erase(shader.expansions.begin());
	}
	shader.expansions.push_back(std::move(expansion));
	return shader.expansions.back().ok;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
