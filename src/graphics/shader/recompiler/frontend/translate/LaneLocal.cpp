#include "graphics/shader/recompiler/frontend/translate/Translate.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <deque>
#include <string_view>
#include <unordered_map>
#include <vector>

// Whether a wave64 compute program's results in each lane depend on no lane outside the lane's 32-lane half of the
// wave (IsWaveLaneLocal). A host with 32-wide subgroups then runs it one GCN lane per invocation, each subgroup a half
// wave with its own copy of the scalar state, instead of two lanes per invocation (each value twice, every heavy
// shader at the 255-register cap).
//
// The proof is conservative. Per instruction it refuses every one that reads another lane (lane reads and writes,
// lane counts, LDS, cross-row DPP) and every use of a lane mask as data (a count, a compare, an address, a value
// broadcast to the lanes, SCC made from one). What a half computes for its lanes stays what the wave computed for
// them:
// - a lane mask only selects lanes (EXEC, a V_CNDMASK or carry select, masks combined bit by bit) and is made from
//   compares and EXEC (or is 0 / -1): other scalar bits hold the wave's layout, lanes 32-63 in the high word;
// - V_READFIRSTLANE, with an active lane in the half, reads either a value the same in every lane (computed from
//   scalar values under the same EXEC) or the head of a waterfall loop (the lanes equal to it run the loop body, then
//   leave: every lane runs once with its own value, whichever lane was first);
// - DPP within a row of 16 lanes, V_PERMLANE(X)16 within a half, quad modes.
// A branch on EXEC or VCC goes another way for a half than for the wave only when none of the half's lanes is active
// (VCC: a mask holding EXEC's lanes, or zero in the whole wave). Up to the merge of the two ways, both ways must keep
// that half's lanes inactive (EXEC narrowed, or set from masks without them) and pass no barrier, and the scalar
// registers read after the merge must be the same both ways: not written, or masks without the half's lanes.
namespace Libs::Graphics::ShaderRecompiler::Frontend {

namespace {

using Decoder::Family;
using Decoder::Instruction;
using Decoder::Opcode;
using Decoder::Operand;
using Decoder::OperandKind;

constexpr int VccLo  = 106;
constexpr int VccHi  = 107;
constexpr int M0     = 124;
constexpr int Scc    = 125; // (the null register's number: SCC in the sets of scalar registers)
constexpr int ExecLo = 126;
constexpr int ExecHi = 127;

using Scalars = std::bitset<128>;

// The scalar register (0-105, VCC, M0, EXEC) an operand names, -1 for any other.
int ScalarIndex(const Operand& operand) {
	switch (operand.kind) {
		case OperandKind::Sgpr: return operand.reg <= 105u ? static_cast<int>(operand.reg) : -1;
		case OperandKind::VccLo: return VccLo;
		case OperandKind::VccHi: return VccHi;
		case OperandKind::M0: return M0;
		case OperandKind::ExecLo: return ExecLo;
		case OperandKind::ExecHi: return ExecHi;
		default: return -1;
	}
}

std::string ScalarName(int reg) {
	switch (reg) {
		case VccLo: return "vcc_lo";
		case VccHi: return "vcc_hi";
		case M0: return "m0";
		case Scc: return "scc";
		case ExecLo: return "exec_lo";
		case ExecHi: return "exec_hi";
		default: return fmt::format("s{}", reg);
	}
}

// A register pair holding a 64-bit value (an even SGPR, VCC, EXEC).
bool PairBase(int reg) {
	return reg >= 0 && reg % 2 == 0 && (reg < M0 || reg == ExecLo);
}

bool NameHas64(Opcode opcode) {
	const auto name = magic_enum::enum_name(opcode);
	return name.find("64") != std::string_view::npos;
}

// Scalar ALU operations whose 64-bit destination is a pair (the others write one register).
bool ScalarDestination64(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_MOV_B64:
		case Opcode::S_CMOV_B64:
		case Opcode::S_NOT_B64:
		case Opcode::S_WQM_B64:
		case Opcode::S_QUADMASK_B64:
		case Opcode::S_BITREPLICATE_B64_B32:
		case Opcode::S_GETPC_B64:
		case Opcode::S_AND_SAVEEXEC_B64:
		case Opcode::S_ORN2_SAVEEXEC_B64:
		case Opcode::S_ANDN1_SAVEEXEC_B64:
		case Opcode::S_AND_B64:
		case Opcode::S_ANDN2_B64:
		case Opcode::S_OR_B64:
		case Opcode::S_ORN2_B64:
		case Opcode::S_XOR_B64:
		case Opcode::S_NAND_B64:
		case Opcode::S_NOR_B64:
		case Opcode::S_XNOR_B64:
		case Opcode::S_LSHL_B64:
		case Opcode::S_LSHR_B64:
		case Opcode::S_BFE_U64:
		case Opcode::S_BFM_B64:
		case Opcode::S_CSELECT_B64:
		case Opcode::S_BITSET0_B64:
		case Opcode::S_BITSET1_B64: return true;
		default: return false;
	}
}

// Scalar ALU operations that may leave (part of) their destination as it was.
bool PartialWrite(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_CMOV_B64:
		case Opcode::S_BITSET0_B32:
		case Opcode::S_BITSET1_B32:
		case Opcode::S_BITSET0_B64:
		case Opcode::S_BITSET1_B64: return true;
		default: return false;
	}
}

// Scalar ALU operations that always write SCC (others may too).
bool WritesScc(Opcode opcode) {
	static constexpr std::string_view Prefixes[] = {"S_CMP_", "S_BITCMP", "S_ADD_", "S_SUB_", "S_ADDC_", "S_SUBB_",
	                                                "S_MIN_", "S_MAX_",   "S_ABS",  "S_AND",  "S_OR",    "S_XOR",
	                                                "S_NAND", "S_NOR",    "S_XNOR", "S_NOT_", "S_WQM_",  "S_LSHL",
	                                                "S_LSHR", "S_ASHR",   "S_BFE_", "S_BCNT"};
	const auto name = magic_enum::enum_name(opcode);
	return std::ranges::any_of(Prefixes, [&](std::string_view prefix) { return name.starts_with(prefix); });
}

// Scalar ALU operations that leave SCC as it was.
bool KeepsScc(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_MOV_B32:
		case Opcode::S_MOV_B64:
		case Opcode::S_CMOV_B64:
		case Opcode::S_MOVK_I32:
		case Opcode::S_CSELECT_B32:
		case Opcode::S_CSELECT_B64:
		case Opcode::S_BREV_B32:
		case Opcode::S_FF1_I32_B32:
		case Opcode::S_FF1_I32_B64:
		case Opcode::S_FLBIT_I32_B32:
		case Opcode::S_FLBIT_I32_B64:
		case Opcode::S_BITSET0_B32:
		case Opcode::S_BITSET1_B32:
		case Opcode::S_BITSET0_B64:
		case Opcode::S_BITSET1_B64:
		case Opcode::S_MUL_I32:
		case Opcode::S_MUL_HI_U32:
		case Opcode::S_MULK_I32:
		case Opcode::S_BFM_B32:
		case Opcode::S_BFM_B64:
		case Opcode::S_PACK_LL_B32_B16:
		case Opcode::S_PACK_LH_B32_B16:
		case Opcode::S_PACK_HH_B32_B16: return true;
		default: return false;
	}
}

bool ReadsScc(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_CSELECT_B32:
		case Opcode::S_CSELECT_B64:
		case Opcode::S_CMOV_B64:
		case Opcode::S_ADDC_U32:
		case Opcode::S_SUBB_U32:
		case Opcode::S_CBRANCH_SCC0:
		case Opcode::S_CBRANCH_SCC1: return true;
		default: return false;
	}
}

// Scalar operations a lane mask may pass through (it stays a mask): moves, selects, bitwise logic, EXEC saves.
bool MaskOperation(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_MOV_B32:
		case Opcode::S_MOV_B64:
		case Opcode::S_CMOV_B64:
		case Opcode::S_NOT_B32:
		case Opcode::S_NOT_B64:
		case Opcode::S_WQM_B32:
		case Opcode::S_WQM_B64:
		case Opcode::S_AND_SAVEEXEC_B32:
		case Opcode::S_ORN2_SAVEEXEC_B32:
		case Opcode::S_ANDN1_SAVEEXEC_B32:
		case Opcode::S_AND_SAVEEXEC_B64:
		case Opcode::S_ORN2_SAVEEXEC_B64:
		case Opcode::S_ANDN1_SAVEEXEC_B64:
		case Opcode::S_AND_B32:
		case Opcode::S_AND_B64:
		case Opcode::S_ANDN2_B32:
		case Opcode::S_ANDN2_B64:
		case Opcode::S_OR_B32:
		case Opcode::S_OR_B64:
		case Opcode::S_ORN2_B32:
		case Opcode::S_ORN2_B64:
		case Opcode::S_XOR_B32:
		case Opcode::S_XOR_B64:
		case Opcode::S_NAND_B32:
		case Opcode::S_NAND_B64:
		case Opcode::S_NOR_B32:
		case Opcode::S_NOR_B64:
		case Opcode::S_XNOR_B32:
		case Opcode::S_XNOR_B64:
		case Opcode::S_CSELECT_B32:
		case Opcode::S_CSELECT_B64: return true;
		default: return false;
	}
}

bool SaveExec(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_AND_SAVEEXEC_B32:
		case Opcode::S_ORN2_SAVEEXEC_B32:
		case Opcode::S_ANDN1_SAVEEXEC_B32:
		case Opcode::S_AND_SAVEEXEC_B64:
		case Opcode::S_ORN2_SAVEEXEC_B64:
		case Opcode::S_ANDN1_SAVEEXEC_B64: return true;
		default: return false;
	}
}

// Instructions that read or write other lanes, or depend on the wave's layout: never lane-local here.
const char* RefusedOpcode(const Instruction& inst) {
	switch (inst.opcode) {
		case Opcode::V_READLANE_B32: return "V_READLANE";
		case Opcode::V_WRITELANE_B32: return "V_WRITELANE";
		case Opcode::V_MBCNT_LO_U32_B32:
		case Opcode::V_MBCNT_HI_U32_B32: return "V_MBCNT";
		case Opcode::V_MOVRELD_B32:
		case Opcode::V_MOVRELS_B32: return "V_MOVREL";
		case Opcode::S_QUADMASK_B64:
		case Opcode::S_BITREPLICATE_B64_B32: return "lane-mask layout";
		case Opcode::S_GETPC_B64:
		case Opcode::S_SETPC_B64:
		case Opcode::S_SUBVECTOR_LOOP_BEGIN:
		case Opcode::S_SUBVECTOR_LOOP_END: return "program counter";
		case Opcode::S_SENDMSG:
		case Opcode::S_SETREG_B32:
		case Opcode::S_TRAP:
		case Opcode::S_TTRACEDATA: return "wave message";
		case Opcode::IMAGE_GET_LOD: return "compute derivatives"; // (their quads: the two-lane layout's)
		case Opcode::UNKNOWN:
		case Opcode::UNSUPPORTED: return "unknown instruction";
		default: break;
	}
	// LDS: a wave's lanes may exchange data there without a barrier (a 64-thread group has none).
	if (inst.family == Family::DS) return "LDS";
	return nullptr;
}

// DPP within a row of 16 lanes (quad permutes, row shifts and rotates, mirrors, RDNA row share/xmask); not the wave
// shifts or row broadcasts.
bool LaneLocalDpp(uint32_t control) {
	return control <= 0xffu || (control >= 0x101u && control <= 0x12fu) || control == 0x140u || control == 0x141u ||
	       (control >= 0x150u && control <= 0x16fu);
}

bool IsCompare(const Instruction& inst) {
	if (inst.family == Family::VOPC) return true;
	return inst.family == Family::VOP3 && inst.dst.kind != OperandKind::Vgpr &&
	       magic_enum::enum_name(inst.opcode).starts_with("V_CMP");
}

struct State {
	Scalars          mask;     // scalar registers holding (values made from) lane masks
	Scalars          covers;   // 64-bit masks holding every active lane, or zero in the whole wave
	Scalars          within;   // 64-bit masks without inactive lanes
	Scalars          nonzero;  // 64-bit masks with a lane in each half
	Scalars          lanes;    // 64-bit lane masks on every path, per lane whatever the layout (from compares, EXEC,
	                           // 0 / -1); other scalar bits (in the wave's layout) are no lane mask for a half
	std::bitset<256> uniform;  // VGPRs the same in every active lane (computed from scalar values under this EXEC)
	bool             scc_mask = false;
	bool             active   = true; // each half has an active lane
	bool             operator==(const State&) const = default;

	void Join(const State& other) {
		mask |= other.mask;
		covers &= other.covers;
		within &= other.within;
		nonzero &= other.nonzero;
		lanes &= other.lanes;
		uniform &= other.uniform;
		scc_mask = scc_mask || other.scc_mask;
		active   = active && other.active;
	}
};

// What is known of a 64-bit lane mask.
struct MaskFacts {
	bool covers  = false;
	bool within  = false;
	bool nonzero = false;
};

// The bits of a half's lanes in a scalar register on the ways after a branch the half's EXEC (or VCC) took alone:
// as at the branch, zero (a mask without the half's lanes), or unknown.
enum class Sym : uint8_t { Original, Zero, Unknown };
using Syms = std::array<Sym, 128>;

class Analysis {
public:
	Analysis(const Decoder::Program& program, const CFG::Graph& cfg): m_program(program), m_cfg(cfg) {}

	bool Run(std::string* reason) {
		const auto& blocks = m_cfg.blocks;
		if (blocks.empty() || m_cfg.irreducible) return Refuse(reason, "control flow");
		for (size_t i = 0; i < blocks.size(); ++i) m_index_of[blocks[i].id] = i;
		std::vector<State> in(blocks.size());
		std::vector<bool>  reached(blocks.size(), false);
		size_t             entry = 0;
		if (const auto found = m_index_of.find(m_cfg.entry_block); found != m_index_of.end()) entry = found->second;
		in[entry].mask.set(ExecLo).set(ExecHi);
		reached[entry] = true;
		// The masks a block may receive (union) and the facts it surely receives (intersection), to a fixpoint.
		std::deque<size_t> work {entry};
		std::vector<bool>  queued(blocks.size(), false);
		queued[entry] = true;
		for (uint32_t steps = 0; !work.empty(); ++steps) {
			if (steps > 100000) return Refuse(reason, "analysis did not converge");
			const auto block = work.front();
			work.pop_front();
			queued[block] = false;
			State state   = in[block];
			for (auto i = blocks[block].inst_begin; i < blocks[block].inst_end; ++i)
				Transfer(m_program.instructions[i], state);
			for (const auto successor_id: blocks[block].successors) {
				const auto found = m_index_of.find(successor_id);
				if (found == m_index_of.end()) continue;
				const auto successor = found->second;
				State      edge      = OnEdge(blocks[block].terminator, successor_id, state);
				if (reached[successor]) {
					State merged = in[successor];
					merged.Join(edge);
					if (merged == in[successor]) continue;
					edge = merged;
				}
				reached[successor] = true;
				in[successor]      = edge;
				if (!queued[successor]) {
					queued[successor] = true;
					work.push_back(successor);
				}
			}
		}
		// The checks, on the fixpoint's states.
		std::vector<State> out(blocks.size());
		for (size_t block = 0; block < blocks.size(); ++block) {
			if (!reached[block]) continue;
			State state = in[block];
			for (auto i = blocks[block].inst_begin; i < blocks[block].inst_end; ++i) {
				if (const char* refused = Check(block, i, state)) {
					if (reason != nullptr) *reason = fmt::format("{} (0x{:x})", refused, m_program.instructions[i].pc);
					return false;
				}
				Transfer(m_program.instructions[i], state);
			}
			out[block] = state;
		}
		return WaveBranches(out, reached, reason);
	}

private:
	static bool Refuse(std::string* reason, std::string_view why) {
		if (reason != nullptr) *reason = why;
		return false;
	}

	static void ReadScalar(const Instruction& inst, const Operand& operand, const State& state, bool& mask) {
		const auto index = ScalarIndex(operand);
		if (index < 0) return;
		mask = mask || state.mask.test(static_cast<size_t>(index));
		if (NameHas64(inst.opcode) && index + 1 < 128) mask = mask || state.mask.test(static_cast<size_t>(index + 1));
	}

	static void WriteScalar(const Operand& operand, uint32_t count, bool mask, State& state) {
		const auto index = ScalarIndex(operand);
		if (index < 0) return;
		for (uint32_t k = 0; k < count && index + static_cast<int>(k) < 128; ++k) {
			const auto reg = static_cast<size_t>(index) + k;
			// (EXEC is always a lane mask.)
			state.mask.set(reg, mask || reg == ExecLo || reg == ExecHi);
		}
	}

	static const Operand* Source(const Instruction& inst, uint32_t index) {
		switch (index) {
			case 0: return &inst.src0;
			case 1: return &inst.src1;
			case 2: return &inst.src2;
			default: return &inst.src3;
		}
	}

	// The scalar registers source `s` names: whole descriptors and address pairs, 64-bit operands.
	static uint32_t SourceWidth(const Instruction& inst, uint32_t s) {
		switch (inst.family) {
			case Family::SMEM: return s == 0 ? (magic_enum::enum_name(inst.opcode).starts_with("S_BUFFER_") ? 4u : 2u) : 1u;
			case Family::MUBUF:
			case Family::MTBUF: return s == 1 ? 4u : 1u;
			case Family::MIMG: {
				// (The sampler: only an instruction that samples reads it.)
				const auto name = magic_enum::enum_name(inst.opcode);
				const bool sampler =
				    name.starts_with("IMAGE_SAMPLE") || name.starts_with("IMAGE_GATHER") || name == "IMAGE_GET_LOD";
				return s == 1 ? (inst.image_r128 ? 4u : 8u) : s == 2 ? (sampler ? 4u : 0u) : 1u;
			}
			case Family::FLAT: return 2u;
			default: return static_cast<int>(s) == LaneSelectSource(inst) || NameHas64(inst.opcode) ? 2u : 1u;
		}
	}

	// Whether one of the `count` scalar registers from the operand's holds a lane mask.
	static bool MaskIn(const Operand& operand, uint32_t count, const State& state) {
		const auto reg = ScalarIndex(operand);
		for (uint32_t k = 0; reg >= 0 && k < count && reg + static_cast<int>(k) < 128; ++k)
			if (state.mask.test(static_cast<size_t>(reg) + k)) return true;
		return false;
	}

	// The lane-select source of a VALU instruction (a mask there selects per lane), or -1.
	static int LaneSelectSource(const Instruction& inst) {
		switch (inst.opcode) {
			case Opcode::V_CNDMASK_B32:
			case Opcode::V_ADDC_U32:
			case Opcode::V_SUB_CO_CI_U32:
			case Opcode::V_SUBREV_CO_CI_U32: return 2;
			default: return -1;
		}
	}

	static bool WritesExec(const Instruction& inst) {
		const auto index = ScalarIndex(inst.dst);
		return index == ExecLo || index == ExecHi || SaveExec(inst.opcode);
	}

	// Sources the same in every active lane: constants, scalar values (not masks), uniform VGPRs.
	static bool UniformSources(const Instruction& inst, const State& state) {
		for (uint32_t s = 0; s < inst.src_count && s < 4; ++s) {
			const auto& source = *Source(inst, s);
			if (source.dpp) return false;
			switch (source.kind) {
				case OperandKind::Vgpr:
					if (source.reg >= 256 || !state.uniform.test(source.reg)) return false;
					if (NameHas64(inst.opcode) && (source.reg + 1 >= 256 || !state.uniform.test(source.reg + 1))) return false;
					break;
				case OperandKind::IntegerInlineConstant:
				case OperandKind::FloatInlineConstant:
				case OperandKind::LiteralConstant: break;
				default: {
					bool mask = false;
					ReadScalar(inst, source, state, mask);
					if (ScalarIndex(source) < 0 || mask) return false;
				}
			}
		}
		return true;
	}

	static MaskFacts PairFacts(int reg, const State& state) {
		if (reg == ExecLo) return {true, true, state.active};
		if (!PairBase(reg)) return {};
		const auto both = [&](const Scalars& set) {
			return set.test(static_cast<size_t>(reg)) && set.test(static_cast<size_t>(reg) + 1);
		};
		return {both(state.covers), both(state.within), both(state.nonzero)};
	}

	static MaskFacts Facts(const Operand& operand, const State& state) {
		if (operand.kind == OperandKind::IntegerInlineConstant) {
			if (operand.value == 0u) return {true, true, false};
			if (operand.signed_val == -1) return {true, false, true};
			return {};
		}
		return PairFacts(ScalarIndex(operand), state);
	}

	static void SetPair(Scalars& set, int reg, bool value) {
		if (!PairBase(reg) || reg == ExecLo) return;
		set.set(static_cast<size_t>(reg), value).set(static_cast<size_t>(reg) + 1, value);
	}

	// The lane-mask facts after the instruction; whether it wrote EXEC within the old one.
	static bool UpdateMaskFacts(const Instruction& inst, const State& before, State& state) {
		Scalars written;
		ScalarDefs(inst, false, written);
		const bool writes_exec = written.test(ExecLo) || written.test(ExecHi);
		const auto a           = Facts(inst.src0, before);
		const auto b           = Facts(inst.src1, before);
		const int  dst         = ScalarIndex(inst.dst);
		MaskFacts  value; // (of the 64-bit value written)
		bool       pair = true;
		switch (inst.opcode) {
			case Opcode::S_MOV_B64: value = a; break;
			case Opcode::S_CSELECT_B64: // (SCC made from a lane mask is refused: SCC the same in both halves)
				value = {a.covers && b.covers, a.within && b.within, a.nonzero && b.nonzero};
				break;
			case Opcode::S_CMOV_B64: {
				const auto old = PairFacts(dst, before);
				value          = {a.covers && old.covers, a.within && old.within, a.nonzero && old.nonzero};
				break;
			}
			case Opcode::S_AND_B64: value = {a.covers && b.covers, a.within || b.within, false}; break;
			case Opcode::S_OR_B64: value = {a.covers && b.covers, a.within && b.within, a.nonzero || b.nonzero}; break;
			case Opcode::S_ANDN2_B64: value = {false, a.within, false}; break;
			default:
				// A compare: inactive lanes clear; holding EXEC's lanes when the condition is the same in all.
				pair = IsCompare(inst);
				if (pair) value = {UniformSources(inst, before), true, false};
				break;
		}
		const bool save      = SaveExec(inst.opcode);
		const bool narrowing = save ? inst.opcode == Opcode::S_AND_SAVEEXEC_B64 || inst.opcode == Opcode::S_ANDN1_SAVEEXEC_B64
		                            : dst == ExecLo && pair && value.within;
		state.covers &= ~written;
		state.within &= ~written;
		state.nonzero &= ~written;
		if (writes_exec) {
			if (!narrowing) state.covers.reset();
			state.within.reset();
			state.active = !save && dst == ExecLo && pair && value.nonzero;
			// The masks that now hold EXEC's lanes, or lie within them.
			if (!save && dst == ExecLo) {
				switch (inst.opcode) {
					case Opcode::S_MOV_B64:
						SetPair(state.covers, ScalarIndex(inst.src0), true);
						SetPair(state.within, ScalarIndex(inst.src0), true);
						break;
					case Opcode::S_AND_B64:
						SetPair(state.covers, ScalarIndex(inst.src0), true);
						SetPair(state.covers, ScalarIndex(inst.src1), true);
						break;
					case Opcode::S_ANDN2_B64: SetPair(state.covers, ScalarIndex(inst.src0), true); break;
					case Opcode::S_OR_B64:
						SetPair(state.within, ScalarIndex(inst.src0), true);
						SetPair(state.within, ScalarIndex(inst.src1), true);
						break;
					default: break;
				}
			}
		}
		if (save) {
			// The old EXEC: it holds the new one's lanes when that narrowed.
			SetPair(state.covers, dst, narrowing);
			SetPair(state.nonzero, dst, before.active);
		} else if (pair && dst != ExecLo) {
			SetPair(state.covers, dst, value.covers);
			SetPair(state.within, dst, value.within);
			SetPair(state.nonzero, dst, value.nonzero);
		}
		return writes_exec && narrowing;
	}

	static void Transfer(const Instruction& inst, State& state) {
		const State before   = state;
		const bool  narrowed = UpdateMaskFacts(inst, before, state);
		// A write to EXEC that may add lanes: the writes under the old EXEC skipped them.
		if (WritesExec(inst) && !narrowed) state.uniform.reset();
		{
			Scalars written;
			ScalarDefs(inst, false, written);
			state.lanes &= ~written;
		}
		switch (inst.family) {
			case Family::SOP1:
			case Family::SOP2:
			case Family::SOPK:
			case Family::SOPC: {
				bool mask = false;
				for (uint32_t s = 0; s < std::max(inst.src_count, 2u); ++s) ReadScalar(inst, *Source(inst, s), before, mask);
				if (ReadsScc(inst.opcode)) mask = mask || before.scc_mask;
				const bool result_mask = mask && MaskOperation(inst.opcode);
				if (SaveExec(inst.opcode)) {
					WriteScalar(inst.dst, 2, true, state); // (the old EXEC)
					SetPair(state.lanes, ScalarIndex(inst.dst), NameHas64(inst.opcode));
					// SCC: whether the new EXEC has a lane, the wave's answer (a half's lanes alone could tell the halves
					// apart: MaskSccSaveexecBranchesTheWholeWave).
					state.scc_mask = true;
					break;
				}
				if (inst.family != Family::SOPC && inst.dst.kind != OperandKind::Unknown) {
					WriteScalar(inst.dst, ScalarDestination64(inst.opcode) ? 2 : 1, result_mask, state);
					// A 64-bit mask operation on lane masks makes one.
					bool lanes = MaskOperation(inst.opcode) && NameHas64(inst.opcode);
					for (uint32_t s = 0; lanes && s < inst.src_count && s < 4; ++s) lanes = LaneMask(*Source(inst, s), before);
					if (inst.opcode == Opcode::S_CMOV_B64) lanes = lanes && LaneMask(inst.dst, before);
					SetPair(state.lanes, ScalarIndex(inst.dst), lanes);
				}
				// (SCC from a lane mask, or kept; an operation that may write it, either.)
				if (WritesScc(inst.opcode)) state.scc_mask = mask;
				else if (!KeepsScc(inst.opcode)) state.scc_mask = before.scc_mask || mask;
				break;
			}
			case Family::SMEM: WriteScalar(inst.dst, std::max(inst.data_dwords, 1u), false, state); break;
			case Family::VOPC:
				WriteScalar(inst.dst, 2, true, state);
				SetPair(state.lanes, ScalarIndex(inst.dst), true);
				break;
			case Family::VOP1:
			case Family::VOP2:
			case Family::VOP3:
			case Family::VOP3P: {
				if (inst.opcode == Opcode::V_READFIRSTLANE_B32 || inst.opcode == Opcode::V_READLANE_B32) {
					WriteScalar(inst.dst, 1, false, state);
					break;
				}
				if (inst.family == Family::VOP3 && inst.dst.kind != OperandKind::Vgpr) {
					// A VOP3-encoded compare (scalar destination): a lane mask.
					WriteScalar(inst.dst, 2, true, state);
					SetPair(state.lanes, ScalarIndex(inst.dst), IsCompare(inst));
					break;
				}
				if (inst.dst2.kind != OperandKind::Unknown && inst.dst2.kind != OperandKind::Null) {
					WriteScalar(inst.dst2, 2, true, state); // (a carry out: a lane mask)
					SetPair(state.lanes, ScalarIndex(inst.dst2), true);
				}
				if (inst.dst.kind == OperandKind::Vgpr) {
					const bool uniform = LaneSelectSource(inst) < 0 && inst.opcode != Opcode::V_PERMLANE16_B32 &&
					                     inst.opcode != Opcode::V_PERMLANEX16_B32 && UniformSources(inst, before);
					// (A write of part of the register keeps the rest: an SDWA selection, a 16-bit result (names with
					// 16, but packed ones).)
					const auto     name    = magic_enum::enum_name(inst.opcode);
					const bool     partial = inst.dst.sdwa_sel != 6u ||
					                     (name.find("16") != std::string_view::npos && !name.starts_with("V_PK_"));
					const uint32_t count   = NameHas64(inst.opcode) ? 2u : 1u;
					for (uint32_t k = 0; k < count && inst.dst.reg + k < 256; ++k)
						state.uniform.set(inst.dst.reg + k, uniform && (!partial || before.uniform.test(inst.dst.reg + k)));
				}
				break;
			}
			case Family::MUBUF:
			case Family::MTBUF:
			case Family::FLAT:
			case Family::MIMG:
			case Family::DS:
				if (inst.dst.kind == OperandKind::Vgpr) {
					// (The VGPRs a load writes: its dwords, components, enabled channels; an image one more, its
					// texture-fail status.)
					const auto written = std::max({inst.data_dwords, inst.data_components,
					                               static_cast<uint32_t>(std::popcount(inst.dmask)), 1u}) +
					                     (inst.family == Family::MIMG ? 1u : 0u);
					for (uint32_t k = 0; k < written && inst.dst.reg + k < 256; ++k) state.uniform.reset(inst.dst.reg + k);
				}
				break;
			default: break;
		}
	}

	// A half that goes this way after a branch on its EXEC (or on VCC within EXEC) has an active lane.
	static State OnEdge(const CFG::Terminator& terminator, uint32_t successor, State state) {
		if (terminator.kind != CFG::TerminatorKind::ConditionalBranch || terminator.true_block == terminator.false_block)
			return state;
		const bool vcc_within = PairFacts(VccLo, state).within;
		switch (terminator.condition) {
			case CFG::BranchCondition::ExecZero: state.active = state.active || successor == terminator.false_block; break;
			case CFG::BranchCondition::ExecNonZero: state.active = state.active || successor == terminator.true_block; break;
			case CFG::BranchCondition::VccZero:
				state.active = state.active || (vcc_within && successor == terminator.false_block);
				break;
			case CFG::BranchCondition::VccNonZero:
				state.active = state.active || (vcc_within && successor == terminator.true_block);
				break;
			default: break;
		}
		return state;
	}

	// A mask per lane whatever the wave's layout: one made from compares and EXEC by 64-bit mask operations (the
	// translator keeps its lanes' bits), or 0 / -1. Other scalar bits are in the wave's layout (lanes 32-63 in the high
	// word), which a half's subgroup reads as its own.
	static bool LaneMask(const Operand& operand, const State& state) {
		if (operand.kind == OperandKind::IntegerInlineConstant) return operand.value == 0u || operand.signed_val == -1;
		const int reg = ScalarIndex(operand);
		if (reg == ExecLo) return true;
		return PairBase(reg) && state.lanes.test(static_cast<size_t>(reg)) && state.lanes.test(static_cast<size_t>(reg) + 1);
	}

	// The reason the instruction is not lane-local in this state, or null.
	const char* Check(size_t block, uint32_t index, const State& state) const {
		const auto& inst = m_program.instructions[index];
		if (const char* refused = RefusedOpcode(inst)) return refused;
		{
			// EXEC is set by compares and by 64-bit mask operations (their sources checked below).
			Scalars written;
			ScalarDefs(inst, false, written);
			const bool sop = inst.family == Family::SOP1 || inst.family == Family::SOP2;
			if ((written.test(ExecLo) || written.test(ExecHi)) && !IsCompare(inst) &&
			    !(sop && MaskOperation(inst.opcode) && NameHas64(inst.opcode)))
				return "EXEC set from the wave's layout";
		}
		for (uint32_t s = 0; s < 4; ++s) {
			const auto& source = *Source(inst, s);
			if (source.dpp && !LaneLocalDpp(source.dpp_ctrl)) return "cross-row DPP";
			// (The write condition takes the row from the half's lane: rows 2-3 of the wave read as 0-1 in the second.)
			if (source.dpp && (source.dpp_row_mask & 3u) != ((source.dpp_row_mask >> 2u) & 3u))
				return "a DPP row mask that treats the halves apart";
			if (source.kind == OperandKind::VccZ || source.kind == OperandKind::ExecZ) return "VCCZ/EXECZ as data";
			if (source.kind == OperandKind::Scc && state.scc_mask) return "SCC made from a lane mask";
		}
		// (SCC from a lane mask tests the half's lanes, not the wave's.)
		if (state.scc_mask && ReadsScc(inst.opcode)) return "SCC made from a lane mask";
		const auto is_mask = [&](const Operand& operand) {
			const auto reg = ScalarIndex(operand);
			if (reg < 0) return false;
			if (state.mask.test(static_cast<size_t>(reg))) return true;
			return NameHas64(inst.opcode) && reg + 1 < 128 && state.mask.test(static_cast<size_t>(reg + 1));
		};
		switch (inst.family) {
			case Family::SOP1:
			case Family::SOP2:
			case Family::SOPK:
			case Family::SOPC: {
				bool mask = false;
				for (uint32_t s = 0; s < std::max(inst.src_count, 2u); ++s) mask = mask || is_mask(*Source(inst, s));
				if (mask && !MaskOperation(inst.opcode)) return "a lane mask used as a scalar value";
				const int dst = ScalarIndex(inst.dst);
				if (dst == ExecLo || dst == ExecHi || SaveExec(inst.opcode)) {
					bool layout = !NameHas64(inst.opcode);
					for (uint32_t s = 0; s < inst.src_count && s < 4; ++s) layout = layout || !LaneMask(*Source(inst, s), state);
					if (inst.opcode == Opcode::S_CMOV_B64) layout = layout || !LaneMask(inst.dst, state);
					if (layout) return "EXEC set from the wave's layout";
				}
				break;
			}
			case Family::SOPP:
				// A branch on VCC goes another way for a half than for the wave only when the half has no active lane:
				// VCC must hold EXEC's lanes (or be zero in the whole wave).
				if ((inst.opcode == Opcode::S_CBRANCH_VCCZ || inst.opcode == Opcode::S_CBRANCH_VCCNZ) &&
				    (!PairFacts(VccLo, state).covers || !state.lanes.test(VccLo) || !state.lanes.test(VccHi)))
					return "a branch on VCC without EXEC's lanes";
				break;
			case Family::SMEM:
			case Family::MUBUF:
			case Family::MTBUF:
			case Family::FLAT:
			case Family::MIMG:
				for (uint32_t s = 0; s < 4; ++s)
					if (MaskIn(*Source(inst, s), SourceWidth(inst, s), state)) return "a lane mask used as an address or resource";
				break;
			case Family::VOP1:
			case Family::VOP2:
			case Family::VOP3:
			case Family::VOP3P:
			case Family::VOPC: {
				const auto select = LaneSelectSource(inst);
				for (uint32_t s = 0; s < inst.src_count && s < 4; ++s)
					if (static_cast<int>(s) != select && is_mask(*Source(inst, s))) return "a lane mask broadcast to the lanes";
				if (select >= 0) {
					// (VOP2: VCC.)
					Operand selector = *Source(inst, static_cast<uint32_t>(select));
					if (selector.kind == OperandKind::Unknown || selector.kind == OperandKind::Null)
						selector.kind = OperandKind::VccLo;
					if (!LaneMask(selector, state)) return "a lane select from the wave's layout";
				}
				if (inst.opcode == Opcode::V_READFIRSTLANE_B32) {
					// (A half without an active lane would read none of the wave's.)
					if (!state.active) return "V_READFIRSTLANE where a half may have no active lane";
					const auto& source = inst.src0;
					const bool  uniform =
					    source.kind == OperandKind::Vgpr ? source.reg < 256 && state.uniform.test(source.reg) : !is_mask(source);
					if (!uniform && !Waterfall(block, index)) return "V_READFIRSTLANE of a lane value outside a waterfall loop";
				}
				break;
			}
			default: break;
		}
		return nullptr;
	}

	// Scalar registers (and SCC) an instruction may read: whole descriptors and address pairs, 64-bit masks, implicit
	// VCC and SCC.
	static void ScalarUses(const Instruction& inst, Scalars& uses) {
		const auto add = [&](int reg, uint32_t count) {
			for (uint32_t k = 0; reg >= 0 && k < count && reg + static_cast<int>(k) < 128; ++k)
				uses.set(static_cast<size_t>(reg) + k);
		};
		for (uint32_t s = 0; s < 4; ++s) {
			const auto& source = *Source(inst, s);
			if (source.kind == OperandKind::Scc) uses.set(Scc);
			add(ScalarIndex(source), SourceWidth(inst, s));
		}
		if (ReadsScc(inst.opcode)) uses.set(Scc);
		if (inst.opcode == Opcode::S_CBRANCH_VCCZ || inst.opcode == Opcode::S_CBRANCH_VCCNZ ||
		    (inst.opcode == Opcode::V_CNDMASK_B32 && inst.family == Family::VOP2))
			add(VccLo, 2);
	}

	// Scalar registers (and SCC) an instruction writes: all it may write, or (sure) those it always writes whole.
	static void ScalarDefs(const Instruction& inst, bool sure, Scalars& defs) {
		const auto add = [&](const Operand& operand, uint32_t count) {
			const auto reg = ScalarIndex(operand);
			for (uint32_t k = 0; reg >= 0 && k < count && reg + static_cast<int>(k) < 128; ++k)
				defs.set(static_cast<size_t>(reg) + k);
		};
		switch (inst.family) {
			case Family::SOP1:
			case Family::SOP2:
			case Family::SOPK:
			case Family::SOPC: {
				const bool wide = ScalarDestination64(inst.opcode) || (!sure && NameHas64(inst.opcode));
				if (!sure || !PartialWrite(inst.opcode)) add(inst.dst, wide ? 2u : 1u);
				if (SaveExec(inst.opcode)) {
					defs.set(ExecLo);
					if (NameHas64(inst.opcode)) defs.set(ExecHi);
				}
				if (!sure || WritesScc(inst.opcode)) defs.set(Scc);
				break;
			}
			case Family::SMEM: add(inst.dst, std::max(inst.data_dwords, 1u)); break;
			case Family::VOPC: add(inst.dst, 2); break;
			case Family::VOP1:
			case Family::VOP2:
			case Family::VOP3:
			case Family::VOP3P:
				if (inst.opcode == Opcode::V_READFIRSTLANE_B32 || inst.opcode == Opcode::V_READLANE_B32) add(inst.dst, 1);
				else if (inst.dst.kind != OperandKind::Vgpr) add(inst.dst, 2);
				if (inst.dst2.kind != OperandKind::Unknown && inst.dst2.kind != OperandKind::Null) add(inst.dst2, 2);
				break;
			default: break;
		}
	}

	// Through an instruction on a way where the half's lanes are inactive; refuses EXEC gaining them and barriers
	// (the other half would wait at it alone).
	static const char* SymTransfer(const Instruction& inst, Syms& syms) {
		if (inst.opcode == Opcode::S_BARRIER) return "a barrier";
		Scalars written;
		ScalarDefs(inst, false, written);
		if (written.none()) return nullptr;
		const int  dst = ScalarIndex(inst.dst);
		const auto pair = [&](const Operand& operand) {
			if (operand.kind == OperandKind::IntegerInlineConstant && operand.value == 0u) return Sym::Zero;
			const int reg = ScalarIndex(operand);
			if (!PairBase(reg) || syms[reg] != syms[reg + 1]) return Sym::Unknown;
			// (Original is a register's own value: copied elsewhere, unknown.)
			return syms[reg] == Sym::Original && reg != dst ? Sym::Unknown : syms[reg];
		};
		const Sym a     = pair(inst.src0);
		const Sym b     = pair(inst.src1);
		Sym       value = Sym::Unknown; // (of the 64-bit value written)
		Sym       exec  = Sym::Unknown; // (EXEC after a SAVEEXEC)
		switch (inst.opcode) {
			case Opcode::S_MOV_B64: value = a; break;
			case Opcode::S_CSELECT_B64: value = a == b ? a : Sym::Unknown; break;
			case Opcode::S_CMOV_B64: value = a == pair(inst.dst) ? a : Sym::Unknown; break;
			case Opcode::S_AND_B64: value = a == Sym::Zero || b == Sym::Zero ? Sym::Zero : a == b ? a : Sym::Unknown; break;
			case Opcode::S_OR_B64:
			case Opcode::S_XOR_B64: value = a == Sym::Zero ? b : b == Sym::Zero ? a : Sym::Unknown; break;
			case Opcode::S_ANDN2_B64: value = a == Sym::Zero ? Sym::Zero : b == Sym::Zero ? a : Sym::Unknown; break;
			case Opcode::S_AND_SAVEEXEC_B64:
			case Opcode::S_ANDN1_SAVEEXEC_B64:
				value = syms[ExecLo] == syms[ExecHi] ? syms[ExecLo] : Sym::Unknown;
				exec  = value == Sym::Zero ? Sym::Zero : Sym::Unknown;
				break;
			default:
				if (IsCompare(inst)) value = syms[ExecLo] == Sym::Zero && syms[ExecHi] == Sym::Zero ? Sym::Zero : Sym::Unknown;
				break;
		}
		for (size_t r = 0; r < 128; ++r)
			if (written.test(r)) syms[r] = Sym::Unknown;
		if (SaveExec(inst.opcode)) syms[ExecLo] = syms[ExecHi] = exec;
		if (PairBase(dst) && (ScalarDestination64(inst.opcode) || IsCompare(inst))) syms[dst] = syms[dst + 1] = value;
		if (syms[ExecLo] != Sym::Zero || syms[ExecHi] != Sym::Zero) return "EXEC gaining a half's lanes";
		return nullptr;
	}

	// The scalar registers at `merge` after the ways from `seed` (none reaching it: all unknown).
	bool Ways(size_t seed, size_t merge, const Syms& entry, const std::vector<std::vector<size_t>>& successors,
	          Syms& at_merge, std::string* reason) const {
		const auto& blocks = m_cfg.blocks;
		const auto  join   = [](Syms& into, const Syms& from) {
            bool changed = false;
            for (size_t r = 0; r < into.size(); ++r)
                if (into[r] != from[r] && into[r] != Sym::Unknown) {
                    into[r] = Sym::Unknown;
                    changed = true;
                }
            return changed;
		};
		at_merge.fill(Sym::Unknown);
		if (seed == merge) {
			at_merge = entry;
			return true;
		}
		bool               merged = false;
		std::vector<Syms>  in(blocks.size());
		std::vector<bool>  seen(blocks.size(), false);
		std::vector<bool>  queued(blocks.size(), false);
		std::deque<size_t> work {seed};
		in[seed]     = entry;
		seen[seed]   = true;
		queued[seed] = true;
		while (!work.empty()) {
			const auto block = work.front();
			work.pop_front();
			queued[block] = false;
			Syms syms     = in[block];
			for (auto i = blocks[block].inst_begin; i < blocks[block].inst_end; ++i) {
				const auto& inst = m_program.instructions[i];
				if (const char* refused = SymTransfer(inst, syms)) {
					if (reason != nullptr)
						*reason = fmt::format("{} between a lane-mask branch and its merge (0x{:x})", refused, inst.pc);
					return false;
				}
			}
			for (const auto s: successors[block]) {
				if (s == merge) {
					if (!merged) at_merge = syms;
					else join(at_merge, syms);
					merged = true;
					continue;
				}
				if (!seen[s]) {
					seen[s] = true;
					in[s]   = syms;
				} else if (!join(in[s], syms)) {
					continue;
				}
				if (!queued[s]) {
					queued[s] = true;
					work.push_back(s);
				}
			}
		}
		return true;
	}

	// A branch on EXEC or VCC (within the checks above) goes another way for a half than for the wave only when the
	// half has no active lane. Both ways to the merge must keep the half's lanes inactive, and the scalar registers the
	// merge reads must be the same both ways.
	bool WaveBranches(const std::vector<State>& out, const std::vector<bool>& reached, std::string* reason) const {
		const auto&  blocks = m_cfg.blocks;
		const size_t count  = blocks.size();
		std::vector<std::vector<size_t>> successors(count);
		for (size_t b = 0; b < count; ++b)
			for (const auto id: blocks[b].successors)
				if (const auto found = m_index_of.find(id); found != m_index_of.end()) successors[b].push_back(found->second);
		// Liveness (backward, to a fixpoint).
		std::vector<Scalars> use(count), def(count), live_in(count);
		for (size_t b = 0; b < count; ++b)
			for (auto i = blocks[b].inst_begin; i < blocks[b].inst_end; ++i) {
				Scalars reads, writes;
				ScalarUses(m_program.instructions[i], reads);
				ScalarDefs(m_program.instructions[i], true, writes);
				use[b] |= reads & ~def[b];
				def[b] |= writes;
			}
		for (bool changed = true; changed;) {
			changed = false;
			for (size_t b = count; b-- > 0;) {
				Scalars out_live;
				for (const auto s: successors[b]) out_live |= live_in[s];
				const auto in = use[b] | (out_live & ~def[b]);
				if (in != live_in[b]) {
					live_in[b] = in;
					changed    = true;
				}
			}
		}
		// Post-dominators (a block's set: itself and what every path from it to an exit passes).
		std::vector<std::vector<bool>> post(count, std::vector<bool>(count, true));
		for (size_t b = 0; b < count; ++b)
			if (successors[b].empty()) {
				post[b].assign(count, false);
				post[b][b] = true;
			}
		for (bool changed = true; changed;) {
			changed = false;
			for (size_t b = count; b-- > 0;) {
				if (successors[b].empty()) continue;
				std::vector<bool> next(count, true);
				for (const auto s: successors[b])
					for (size_t k = 0; k < count; ++k) next[k] = next[k] && post[s][k];
				next[b] = true;
				if (next != post[b]) {
					post[b] = std::move(next);
					changed = true;
				}
			}
		}
		for (size_t b = 0; b < count; ++b) {
			const auto& terminator = blocks[b].terminator;
			if (!reached[b] || terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
			    terminator.true_block == terminator.false_block)
				continue;
			bool vcc = false, zero_taken = false; // (the way of a half without an active lane)
			switch (terminator.condition) {
				case CFG::BranchCondition::ExecZero: zero_taken = true; break;
				case CFG::BranchCondition::ExecNonZero: break;
				case CFG::BranchCondition::VccZero: vcc = zero_taken = true; break;
				case CFG::BranchCondition::VccNonZero: vcc = true; break;
				default: continue;
			}
			const auto half_way = m_index_of.find(zero_taken ? terminator.true_block : terminator.false_block);
			const auto wave_way = m_index_of.find(zero_taken ? terminator.false_block : terminator.true_block);
			if (half_way == m_index_of.end() || wave_way == m_index_of.end()) return Refuse(reason, "control flow");
			// The merge: the nearest block both ways pass (none: no code after both).
			std::vector<bool> common = post[half_way->second];
			for (size_t k = 0; k < count; ++k) common[k] = common[k] && post[wave_way->second][k];
			const auto members = static_cast<size_t>(std::ranges::count(common, true));
			size_t     merge   = count;
			for (size_t k = 0; k < count && merge == count; ++k)
				if (common[k] && static_cast<size_t>(std::ranges::count(post[k], true)) == members) merge = k;
			// At the branch, the half's bits are zero in EXEC, in VCC (a branch on VCC) and in masks within EXEC.
			Syms entry;
			entry.fill(Sym::Original);
			entry[ExecLo] = entry[ExecHi] = Sym::Zero;
			if (vcc) entry[VccLo] = entry[VccHi] = Sym::Zero;
			for (int r = 0; r < M0; r += 2)
				if (PairFacts(r, out[b]).within) entry[r] = entry[r + 1] = Sym::Zero;
			Syms half, wave;
			if (!Ways(half_way->second, merge, entry, successors, half, reason) ||
			    !Ways(wave_way->second, merge, entry, successors, wave, reason))
				return false;
			if (merge == count) continue;
			std::string escaping;
			for (size_t r = 0; r < 128; ++r)
				if (live_in[merge].test(r) && (half[r] == Sym::Unknown || half[r] != wave[r]))
					escaping += " " + ScalarName(static_cast<int>(r));
			if (!escaping.empty()) {
				if (reason != nullptr) {
					const auto& last = m_program.instructions[blocks[b].inst_end - 1];
					*reason = fmt::format("scalar state escapes a lane-mask branch (0x{:x}:{})", last.pc, escaping);
				}
				return false;
			}
		}
		return true;
	}

	// V_READFIRSTLANE at `index` heads a waterfall loop: the next compare in its block tests its source against it,
	// an EXEC save takes that compare's lanes, and its block is in a loop that repeats while EXEC is not zero.
	bool Waterfall(size_t block, uint32_t index) const {
		const auto& blocks = m_cfg.blocks;
		const auto& head   = m_program.instructions[index];
		if (head.src0.kind != OperandKind::Vgpr) return false;
		const auto value  = static_cast<int>(head.src0.reg);
		const auto scalar = ScalarIndex(head.dst);
		if (scalar < 0) return false;
		int compare_mask = -1;
		for (auto i = index + 1; i < blocks[block].inst_end; ++i) {
			const auto& inst = m_program.instructions[i];
			const auto  name = magic_enum::enum_name(inst.opcode);
			if (compare_mask < 0) {
				if (!name.starts_with("V_CMP_EQ_") && !name.starts_with("V_CMPX_EQ_")) continue;
				const bool wide    = NameHas64(inst.opcode);
				const auto matches = [&](const Operand& a, const Operand& b) {
					if (a.kind != OperandKind::Vgpr || ScalarIndex(b) < 0) return false;
					const auto dv = value - static_cast<int>(a.reg), ds = scalar - ScalarIndex(b);
					return dv == ds && dv >= 0 && dv <= (wide ? 1 : 0);
				};
				if (!matches(inst.src0, inst.src1) && !matches(inst.src1, inst.src0)) continue;
				if (inst.dst.kind == OperandKind::ExecLo) {
					compare_mask = ExecLo; // (V_CMPX: EXEC takes the lanes)
					break;
				}
				compare_mask = ScalarIndex(inst.dst);
				if (compare_mask < 0) return false;
				continue;
			}
			if (SaveExec(inst.opcode) && ScalarIndex(inst.src0) == compare_mask) {
				compare_mask = ExecLo;
				break;
			}
		}
		if (compare_mask != ExecLo) return false;
		const auto id = blocks[block].id;
		for (const auto& loop: m_cfg.natural_loops) {
			if (std::ranges::find(loop.body_blocks, id) == loop.body_blocks.end() && loop.header != id) continue;
			const auto* latch = m_cfg.FindBlock(loop.latch);
			if (latch == nullptr || latch->inst_end == latch->inst_begin) continue;
			if (m_program.instructions[latch->inst_end - 1].opcode == Opcode::S_CBRANCH_EXECNZ) return true;
		}
		return false;
	}

	const Decoder::Program&              m_program;
	const CFG::Graph&                    m_cfg;
	std::unordered_map<uint32_t, size_t> m_index_of;
};

} // namespace

bool IsWaveLaneLocal(const Decoder::Program& program, std::string* reason) {
	// (On the program's own branches: the structured graph routes some through variables, scalar state of its own.)
	const auto cfg = CFG::BuildGraph(program);
	return Analysis(program, cfg).Run(reason);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
