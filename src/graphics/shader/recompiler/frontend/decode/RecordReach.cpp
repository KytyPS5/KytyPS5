#include "graphics/shader/recompiler/frontend/decode/RecordReach.h"

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <magic_enum.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

namespace {

constexpr uint32_t None  = RecordReach::None;
constexpr uint32_t VccLo = 106;
constexpr uint32_t VccHi = 107;

// What a register holds, for the lanes the instruction that wrote it ran on.
enum class Kind : uint8_t {
	Unknown,
	Constant,          // a: the value
	Record,            // the index of the thread's record
	Thread,            // the thread the search compared
	Field,             // a: the field (a dword of the record)
	Byte,              // a: the field, b: its byte
	Vertex,            // Thread - the record's first thread
	Scaled,            // stride * Vertex
	Offset,            // an attribute's value (a: field, b: byte; a None: zero) + Scaled
	Aligned,           // (Offset & ~3) + c
	PointerLow,        // the dwords of the table entry of the record's pointer field
	PointerHigh,
	PointerHighSigned, // the low word of PointerHigh, sign extended
	AddressLow,        // PointerLow + Aligned (a, b, c), d: the instruction
	AddressHigh,       // PointerHighSigned + the carry of the AddressLow of instruction d
	Carry,             // d: the instruction
	SavedExec,         // a: the EXEC state
	LaneMask,          // lanes of the EXEC state a
	Descriptor,        // dword b of a V# loaded from a pointer; a: the load, c: the group's first dword
	FirstLane,         // the field a of the first active lane's record
	TableOffset,       // FirstLane * 16
	Target,            // dword b of the table entry that field a names
};

struct Tag {
	Kind     kind  = Kind::Unknown;
	uint32_t a     = 0;
	uint32_t b     = 0;
	uint32_t c     = 0;
	uint32_t d     = 0;
	// VGPRs: the EXEC state it was written in.
	uint32_t state = 0;
};

bool IsNoOp(Opcode opcode) {
	return opcode == Opcode::S_NOP || opcode == Opcode::S_WAITCNT || opcode == Opcode::S_INST_PREFETCH;
}

uint32_t ScalarIndex(const Operand& operand) {
	switch (operand.kind) {
		case OperandKind::Sgpr: return operand.reg;
		case OperandKind::VccLo: return VccLo;
		case OperandKind::VccHi: return VccHi;
		default: return None;
	}
}

bool SameScalar(const Operand& a, const Operand& b) {
	return ScalarIndex(a) != None && ScalarIndex(a) == ScalarIndex(b);
}

bool IsVgpr(const Operand& operand, uint32_t reg) {
	return operand.kind == OperandKind::Vgpr && operand.reg == reg && operand.sdwa_sel == 6;
}

bool IsValue(const Operand& operand, uint32_t value) {
	return (operand.kind == OperandKind::IntegerInlineConstant || operand.kind == OperandKind::LiteralConstant) &&
	       operand.value == value;
}

bool FullDestination(const Instruction& inst) {
	return !inst.dst.explicit_sdwa_dst || inst.dst.sdwa_sel == 6;
}

// The registers the instruction writes: write(vector, reg). In waves of 32 lanes.
template <typename Func>
void ForEachWrite(const Instruction& inst, Func&& write) {
	const auto name = magic_enum::enum_name(inst.opcode);
	if (inst.dst.kind == OperandKind::Vgpr && name.find("_STORE") == std::string_view::npos) {
		uint32_t count = 1;
		switch (inst.family) {
			case Family::MIMG: count = std::max(inst.data_dwords, 1u) + (((inst.raw[0] >> 16u) & 3u) != 0 ? 1u : 0u); break;
			case Family::MUBUF:
			case Family::MTBUF:
			case Family::FLAT: count = std::max(inst.data_dwords, 1u); break;
			case Family::DS: count = std::max(inst.data_dwords, 1u) * 2u; break;
			default: count = name.find("64") != std::string_view::npos ? 2u : 1u; break;
		}
		for (uint32_t i = 0; i < count && inst.dst.reg + i < 256u; i++) {
			write(true, inst.dst.reg + i);
		}
	}
	const bool wide  = !name.starts_with("V_CMP") && name.find("64") != std::string_view::npos &&
	                   name.find("_I32_") == std::string_view::npos;
	const auto count = inst.family == Family::SMEM ? std::max(inst.data_dwords, 1u) : (wide ? 2u : 1u);
	if (const auto first = ScalarIndex(inst.dst); first != None) {
		for (uint32_t i = 0; i < count && first + i < 128u; i++) {
			write(false, first + i);
		}
	}
	if (const auto carry = ScalarIndex(inst.dst2); carry != None) {
		write(false, carry);
	} else if (inst.opcode == Opcode::V_ADD_I32 || inst.opcode == Opcode::V_SUB_I32 ||
	           inst.opcode == Opcode::V_SUBREV_I32 || inst.opcode == Opcode::V_ADDC_U32 ||
	           inst.opcode == Opcode::V_SUB_CO_CI_U32 || inst.opcode == Opcode::V_SUBREV_CO_CI_U32) {
		// The two-operand forms leave their carry in VCC.
		write(false, VccLo);
	}
}

class Finder {
public:
	explicit Finder(const Program& program): m_inst(program.instructions) {}

	bool Find(RecordReach& reach) {
		if (!FindSearch()) {
			return false;
		}
		// The code before the search is straight: the V#s it loads.
		for (size_t i = 0; i < m_head; i++) {
			if (IsDirectBranch(m_inst[i].opcode) || !Step(i)) {
				return false;
			}
		}
		for (size_t i = m_head; i < m_start; i++) {
			Invalidate(i);
		}
		m_states         = {0};
		m_state          = 0;
		m_vector[m_record] = {.kind = Kind::Record};
		m_vector[m_thread] = {.kind = Kind::Thread};
		struct Loop {
			size_t   head  = 0;
			size_t   back  = 0;
			uint32_t entry = 0;
		};
		struct Skip {
			size_t   target = 0;
			size_t   branch = 0;
			uint32_t state  = 0;
		};
		std::vector<Loop> loops;
		std::vector<Skip> skips;
		for (size_t i = m_start; i < m_inst.size(); i++) {
			const auto name = magic_enum::enum_name(m_inst[i].opcode);
			if (name.find("SETPC") != std::string_view::npos || name.find("SWAPPC") != std::string_view::npos ||
			    name.find("CALL") != std::string_view::npos || name.find("SUBVECTOR") != std::string_view::npos) {
				return false;
			}
			if (!IsDirectBranch(m_inst[i].opcode)) {
				continue;
			}
			const auto target = IndexOf(m_inst[i].branch_target);
			if (target == npos || target < m_start) {
				return false;
			}
			if (target <= i) {
				loops.push_back({target, i, 0});
			}
		}
		for (size_t i = m_start; i < m_inst.size(); i++) {
			// Code that branches skip: what they wrote is unknown here, and so is EXEC unless every way
			// in agrees.
			bool     arrived = false;
			bool     agree   = true;
			uint32_t state   = 0;
			for (const auto& skip: skips) {
				if (skip.target != i) {
					continue;
				}
				for (size_t j = skip.branch + 1; j < i; j++) {
					Invalidate(j);
				}
				agree   = agree && (!arrived || state == skip.state);
				state   = skip.state;
				arrived = true;
			}
			if (arrived) {
				const bool falls = m_inst[i - 1].opcode != Opcode::S_BRANCH;
				m_state          = agree && (!falls || state == m_state) ? state : NewRoot();
			}
			// A loop: what its body writes is unknown at its head, where EXEC is some of the lanes
			// that entered it.
			for (auto& loop: loops) {
				if (loop.head != i) {
					continue;
				}
				for (size_t j = loop.head; j <= loop.back; j++) {
					Invalidate(j);
				}
				loop.entry = m_state;
				m_state    = NewChild(m_state);
			}
			const auto& inst = m_inst[i];
			if (IsDirectBranch(inst.opcode)) {
				const auto target = IndexOf(inst.branch_target);
				if (target > i) {
					skips.push_back({target, i, m_state});
				} else {
					const auto loop = std::ranges::find(loops, i, &Loop::back);
					if (!Within(loop->entry, m_state)) {
						return false;
					}
				}
				continue;
			}
			if (!Step(i)) {
				return false;
			}
		}
		// The pointers' V#: loaded once from a user data pointer, outside the loops.
		const auto load = IndexOf(m_pointers_pc);
		if (m_reads.empty() || m_stride_field == None || load == npos || !UserPointerLoad(load) ||
		    std::ranges::any_of(loops, [&](const Loop& loop) { return loop.head <= load && load <= loop.back; }) ||
		    std::ranges::any_of(m_store_tables, [&](const auto& table) {
			    return table != std::pair {m_pointers_pc, m_pointers_dword};
		    })) {
			return false;
		}
		reach.header        = m_header;
		reach.header_offset = m_header_offset;
		reach.records       = m_records;
		reach.record_size   = m_record_size;
		reach.threads_field = m_threads_field;
		reach.first_field   = m_threads_field + 4u;
		reach.pointers      = {m_inst[load].src0.reg, m_inst[load].offset + m_pointers_dword * 4u};
		reach.pointer_field = m_pointer_field;
		reach.stride_field  = m_stride_field;
		reach.stride_byte   = m_stride_byte;
		reach.attributes    = m_reads;
		reach.store_fields  = m_store_fields;
		reach.read_pcs      = m_read_pcs;
		reach.store_pcs     = m_store_pcs;
		const auto in_record = [&](uint32_t field) { return field == None || field <= m_record_size - 4u; };
		reach.valid = m_record_size >= 4u && in_record(reach.threads_field) && in_record(reach.first_field) &&
		              in_record(reach.pointer_field) && in_record(reach.stride_field) &&
		              std::ranges::all_of(reach.attributes, in_record, &RecordReach::Attribute::field) &&
		              std::ranges::all_of(reach.store_fields, in_record);
		return reach.valid;
	}

private:
	static constexpr size_t npos = SIZE_MAX;

	size_t IndexOf(uint32_t pc) const {
		const auto found = std::ranges::lower_bound(m_inst, pc, {}, &Instruction::pc);
		return found != m_inst.end() && found->pc == pc ? static_cast<size_t>(found - m_inst.begin()) : npos;
	}

	bool Writes(size_t i, bool vector, uint32_t reg) const {
		bool writes = false;
		ForEachWrite(m_inst[i], [&](bool is_vector, uint32_t written) { writes |= is_vector == vector && written == reg; });
		return writes;
	}

	// The last instruction before `end` that writes SGPR `reg`.
	size_t ScalarWriter(size_t end, uint32_t reg) const {
		for (size_t i = end; i-- > 0;) {
			if (Writes(i, false, reg)) {
				return i;
			}
		}
		return npos;
	}

	// S_LOAD_DWORDXn of a V# from a pointer in a user data register pair, at a constant offset.
	bool UserPointerLoad(size_t i) const {
		const auto& load = m_inst[i];
		if (load.family != Family::SMEM || !magic_enum::enum_name(load.opcode).starts_with("S_LOAD_DWORDX") ||
		    load.src0.kind != OperandKind::Sgpr || load.src1.kind != OperandKind::Null) {
			return false;
		}
		return ScalarWriter(i, load.src0.reg) == npos && ScalarWriter(i, load.src0.reg + 1u) == npos;
	}

	// The V# in SGPRs `reg`..`reg` + 3 before instruction `end`: one S_LOAD_DWORDX4 from a user data pointer.
	bool UserDescriptor(size_t end, uint32_t reg, RecordReach::Descriptor& descriptor) const {
		const auto load = ScalarWriter(end, reg);
		if (load == npos || m_inst[load].opcode != Opcode::S_LOAD_DWORDX4 || ScalarIndex(m_inst[load].dst) != reg ||
		    !UserPointerLoad(load)) {
			return false;
		}
		for (uint32_t i = 1; i < 4; i++) {
			if (ScalarWriter(end, reg + i) != load) {
				return false;
			}
		}
		descriptor = {m_inst[load].src0.reg, m_inst[load].offset};
		return true;
	}

	// The search for the thread's record (see RecordReach), instruction by instruction.
	bool FindSearch() {
		std::vector<size_t> real;
		for (size_t i = 0; i < m_inst.size(); i++) {
			if (!IsNoOp(m_inst[i].opcode)) {
				real.push_back(i);
			}
		}
		constexpr size_t Before = 12;
		constexpr size_t Count  = 30;
		for (size_t at = Before; at + Count - Before <= real.size(); at++) {
			if (m_inst[real[at]].opcode != Opcode::S_MULK_I32) {
				continue;
			}
			const auto in = [&](size_t k) -> const Instruction& { return m_inst[real[at - Before + k]]; };
			const auto is = [&](size_t k, Opcode opcode) { return in(k).opcode == opcode; };
			constexpr std::array opcodes {
			    Opcode::S_CBRANCH_EXECZ, Opcode::V_MOV_B32,   Opcode::S_CMP_LT_U32, Opcode::S_CSELECT_B32,
			    Opcode::S_ORN2_B32,      Opcode::S_MOV_B32,   Opcode::S_AND_B32,    Opcode::S_AND_B32,
			    Opcode::S_CBRANCH_VCCZ,  Opcode::V_MOV_B32,   Opcode::S_ADD_I32,    Opcode::S_ADD_I32,
			    Opcode::S_MULK_I32,      Opcode::S_BUFFER_LOAD_DWORDX2, Opcode::S_ADD_I32, Opcode::V_CMP_LE_U32,
			    Opcode::V_CMP_GT_U32,    Opcode::S_AND_B32,   Opcode::V_CNDMASK_B32, Opcode::V_CMPX_EQ_U32,
			    Opcode::S_CBRANCH_EXECZ, Opcode::V_MOV_B32,   Opcode::S_MOV_B32,    Opcode::S_BRANCH,
			    Opcode::S_MOV_B32,       Opcode::V_CMPX_EQ_U32, Opcode::V_ADD_NC_U32, Opcode::S_MOV_B32,
			    Opcode::V_CMPX_NE_U32,   Opcode::S_CBRANCH_EXECZ};
			bool match = true;
			for (size_t k = 0; k < Count && match; k++) {
				match = is(k, opcodes[k]);
			}
			if (!match) {
				continue;
			}
			const auto exec  = [](const Operand& operand) { return operand.kind == OperandKind::ExecLo; };
			const auto vcc   = [](const Operand& operand) { return operand.kind == OperandKind::VccLo; };
			const auto vgpr  = [](const Operand& operand) { return operand.kind == OperandKind::Vgpr && operand.sdwa_sel == 6; };
			const auto exit  = in(24).pc;
			const auto& counter = in(2).src0;
			const auto& count   = in(2).src1;
			const auto& mask    = in(4).dst;
			const auto& saved   = in(6).dst;
			const auto& first   = in(10).src0;
			const auto& next    = in(11).dst;
			const auto  record  = in(1).dst.reg;
			const auto  step    = in(9).dst.reg;
			const auto  found   = in(18).dst.reg;
			const auto  threads = ScalarIndex(in(13).dst);
			const auto& thread  = in(15).src1;
			const auto& outer   = in(27).src0;
			if (!(in(0).branch_target == exit && in(8).branch_target == exit && in(20).branch_target == exit &&
			      in(23).branch_target == in(0).pc && in(29).branch_target > in(29).pc &&
			      vgpr(in(1).dst) && FullDestination(in(1)) && IsValue(in(1).src0, 0xffffffffu) &&
			      ScalarIndex(counter) != None && count.kind == OperandKind::Sgpr &&
			      vcc(in(3).dst) && exec(in(3).src0) && IsValue(in(3).src1, 0) &&
			      ScalarIndex(mask) != None && vcc(in(4).src0) && exec(in(4).src1) &&
			      vcc(in(5).dst) && SameScalar(in(5).src0, mask) &&
			      ScalarIndex(saved) != None && SameScalar(in(6).src0, saved) && SameScalar(in(6).src1, mask) &&
			      exec(in(7).dst) && exec(in(7).src0) && SameScalar(in(7).src1, mask) &&
			      vgpr(in(9).dst) && FullDestination(in(9)) && SameScalar(in(9).src0, counter) &&
			      vcc(in(10).dst) && first.kind == OperandKind::Sgpr && SameScalar(in(10).src1, counter) &&
			      first.reg + 1u == count.reg &&
			      ScalarIndex(next) != None && SameScalar(in(11).src0, counter) && IsValue(in(11).src1, 1) &&
			      vcc(in(12).dst) && in(12).src1.value >= 4u && in(12).src1.value < 0x1000u &&
			      threads != None && threads < 104u && in(13).src0.kind == OperandKind::Sgpr && vcc(in(13).src1) &&
			      in(13).offset <= in(12).src1.value - 8u &&
			      vcc(in(14).dst) &&
			      ((ScalarIndex(in(14).src0) == threads + 1u && ScalarIndex(in(14).src1) == threads) ||
			       (ScalarIndex(in(14).src0) == threads && ScalarIndex(in(14).src1) == threads + 1u)) &&
			      ScalarIndex(in(15).dst) != None && ScalarIndex(in(15).src0) == threads + 1u && vgpr(thread) &&
			      vcc(in(16).dst) && vcc(in(16).src0) && IsVgpr(in(16).src1, thread.reg) &&
			      vcc(in(17).dst) &&
			      ((SameScalar(in(17).src0, in(15).dst) && vcc(in(17).src1)) ||
			       (vcc(in(17).src0) && SameScalar(in(17).src1, in(15).dst))) &&
			      !vcc(in(15).dst) &&
			      vgpr(in(18).dst) && FullDestination(in(18)) && IsValue(in(18).src0, 2) && IsValue(in(18).src1, 3) &&
			      (in(18).src_count < 3 || vcc(in(18).src2)) &&
			      IsValue(in(19).src0, 2) && IsVgpr(in(19).src1, found) &&
			      IsVgpr(in(21).dst, found) && FullDestination(in(21)) && IsValue(in(21).src0, 0) &&
			      SameScalar(in(22).dst, counter) && SameScalar(in(22).src0, next) &&
			      exec(in(24).dst) && SameScalar(in(24).src0, saved) &&
			      IsValue(in(25).src0, 3) && IsVgpr(in(25).src1, found) &&
			      IsVgpr(in(26).dst, record) && FullDestination(in(26)) && SameScalar(in(26).src0, first) &&
			      IsVgpr(in(26).src1, step) &&
			      exec(in(27).dst) && ScalarIndex(outer) != None &&
			      IsValue(in(28).src0, 0xffffffffu) && IsVgpr(in(28).src1, record) &&
			      record != step && record != found && step != found && thread.reg != record &&
			      thread.reg != step && thread.reg != found)) {
				continue;
			}
			// The registers the search reads keep what the code before it gave them.
			const size_t head     = real[at - Before];
			const auto   is_move  = [&](uint32_t reg, auto&& source) {
				const auto writer = ScalarWriter(head, reg);
				return writer != npos && m_inst[writer].opcode == Opcode::S_MOV_B32 && source(m_inst[writer].src0);
			};
			const auto header_load = ScalarWriter(head, first.reg);
			if (!is_move(ScalarIndex(counter), [](const Operand& source) { return IsValue(source, 0); }) ||
			    !is_move(ScalarIndex(saved), exec) || !is_move(ScalarIndex(outer), exec) ||
			    ScalarIndex(saved) == ScalarIndex(outer) || header_load == npos ||
			    m_inst[header_load].opcode != Opcode::S_BUFFER_LOAD_DWORDX2 ||
			    ScalarIndex(m_inst[header_load].dst) != first.reg || ScalarWriter(head, count.reg) != header_load ||
			    m_inst[header_load].src0.kind != OperandKind::Sgpr || m_inst[header_load].src1.kind != OperandKind::Null ||
			    !UserDescriptor(header_load, m_inst[header_load].src0.reg, m_header) ||
			    !UserDescriptor(head, in(13).src0.reg, m_records)) {
				continue;
			}
			// Nothing else enters the search.
			const size_t last = real[at - Before + Count - 1];
			bool         closed = true;
			for (size_t i = 0; i < m_inst.size() && closed; i++) {
				const auto& branch = m_inst[i];
				closed = !IsDirectBranch(branch.opcode) || (i >= head && i <= last) ||
				         branch.branch_target < m_inst[head].pc || branch.branch_target > m_inst[last].pc;
			}
			if (!closed) {
				continue;
			}
			m_head          = head;
			m_start         = last + 1;
			m_record        = record;
			m_thread        = thread.reg;
			m_header_offset = m_inst[header_load].offset;
			m_record_size   = in(12).src1.value;
			m_threads_field = in(13).offset;
			m_records_load  = m_inst[ScalarWriter(head, in(13).src0.reg)].pc;
			return true;
		}
		return false;
	}

	void Invalidate(size_t i) {
		ForEachWrite(m_inst[i], [&](bool vector, uint32_t reg) { (vector ? m_vector[reg] : m_scalar[reg]) = {}; });
	}

	uint32_t NewChild(uint32_t parent) {
		m_states.push_back(parent);
		return static_cast<uint32_t>(m_states.size() - 1);
	}

	// An EXEC whose lanes nothing is known about.
	uint32_t NewRoot() {
		m_states.push_back(static_cast<uint32_t>(m_states.size()));
		return static_cast<uint32_t>(m_states.size() - 1);
	}

	// Whether the lanes of EXEC state `state` are lanes of `outer`.
	bool Within(uint32_t outer, uint32_t state) const {
		for (;;) {
			if (state == outer) {
				return true;
			}
			if (m_states[state] == state) {
				return false;
			}
			state = m_states[state];
		}
	}

	// A VGPR's value holds for the lanes it was written on.
	Tag Vector(uint32_t reg) const {
		return reg < 256u && Within(m_vector[reg].state, m_state) ? m_vector[reg] : Tag {};
	}

	Tag Read(const Operand& operand) const {
		if (operand.negate || operand.absolute) {
			return {};
		}
		switch (operand.kind) {
			case OperandKind::IntegerInlineConstant:
			case OperandKind::LiteralConstant: return {.kind = Kind::Constant, .a = operand.value};
			case OperandKind::Sgpr:
			case OperandKind::VccLo:
			case OperandKind::VccHi: return m_scalar[ScalarIndex(operand)];
			case OperandKind::Vgpr: {
				const auto tag = Vector(operand.reg);
				if (operand.sdwa_sel == 6) {
					return tag;
				}
				if (tag.kind == Kind::Field && operand.sdwa_sel < 4 && !operand.sdwa_sext) {
					return {.kind = Kind::Byte, .a = tag.a, .b = operand.sdwa_sel};
				}
				if (tag.kind == Kind::PointerHigh && operand.sdwa_sel == 4 && operand.sdwa_sext) {
					return {.kind = Kind::PointerHighSigned};
				}
				return {};
			}
			default: return {};
		}
	}

	// The V# in SGPRs `reg`..`reg` + 3: {the load, its first dword}, or {None, 0}.
	std::pair<uint32_t, uint32_t> Descriptor(const Operand& operand) const {
		if (operand.kind != OperandKind::Sgpr || operand.reg + 3u >= 104u) {
			return {None, 0};
		}
		const auto& first = m_scalar[operand.reg];
		if (first.kind != Kind::Descriptor || first.b % 4u != 0) {
			return {None, 0};
		}
		for (uint32_t i = 1; i < 4; i++) {
			const auto& tag = m_scalar[operand.reg + i];
			if (tag.kind != Kind::Descriptor || tag.a != first.a || tag.b != first.b + i) {
				return {None, 0};
			}
		}
		return {first.a, first.b};
	}

	// The field of the table entry in SGPRs `reg`..`reg` + 3, or None.
	uint32_t Target(const Operand& operand) const {
		if (operand.kind != OperandKind::Sgpr || operand.reg + 3u >= 104u) {
			return None;
		}
		for (uint32_t i = 0; i < 4; i++) {
			const auto& tag = m_scalar[operand.reg + i];
			if (tag.kind != Kind::Target || tag.a != m_scalar[operand.reg].a || tag.b != i) {
				return None;
			}
		}
		return m_scalar[operand.reg].a;
	}

	// Follows one instruction; false when it reaches memory through an address this does not follow.
	bool Step(size_t i) {
		const auto& inst = m_inst[i];
		const auto  name = magic_enum::enum_name(inst.opcode);
		const auto  a    = Read(inst.src0);
		const auto  b    = Read(inst.src1);
		const auto  c    = Read(inst.src2);
		const auto  is   = [](const Tag& tag, Kind kind) { return tag.kind == kind; };
		const auto  constant = [](const Tag& tag, uint32_t value) { return tag.kind == Kind::Constant && tag.a == value; };
		// The new values of the registers it writes: {vector, register, value}.
		struct Result {
			bool     vector = false;
			uint32_t reg    = 0;
			Tag      tag;
		};
		std::array<Result, 16> results {};
		size_t                 count = 0;
		const auto             set   = [&](bool vector, uint32_t reg, Tag tag) {
            if (count < results.size() && reg != None) {
                results[count++] = {vector, reg, tag};
            }
		};
		const bool vector_result = inst.dst.kind == OperandKind::Vgpr && FullDestination(inst);
		const auto scalar_dst    = ScalarIndex(inst.dst);
		switch (inst.opcode) {
			case Opcode::V_MOV_B32:
				if (vector_result) {
					set(true, inst.dst.reg, a);
				}
				break;
			case Opcode::V_SUB_NC_U32:
				if (vector_result && is(a, Kind::Thread) && is(b, Kind::Field) && b.a == m_threads_field + 4u) {
					set(true, inst.dst.reg, {.kind = Kind::Vertex});
				}
				break;
			case Opcode::V_LSHRREV_B32:
				if (vector_result && constant(a, 24) && is(b, Kind::Field)) {
					set(true, inst.dst.reg, {.kind = Kind::Byte, .a = b.a, .b = 3});
				}
				break;
			case Opcode::V_MUL_LO_U32: {
				const auto& byte = is(a, Kind::Byte) ? a : b;
				if (vector_result && is(byte, Kind::Byte) && is(is(a, Kind::Byte) ? b : a, Kind::Vertex) &&
				    (m_stride_field == None || (m_stride_field == byte.a && m_stride_byte == byte.b))) {
					m_stride_field = byte.a;
					m_stride_byte  = byte.b;
					set(true, inst.dst.reg, {.kind = Kind::Scaled});
				}
				break;
			}
			case Opcode::V_AND_B32: {
				const auto& value = constant(a, 0xfffffffcu) ? b : a;
				if (vector_result && (constant(a, 0xfffffffcu) || constant(b, 0xfffffffcu))) {
					if (is(value, Kind::Scaled)) {
						set(true, inst.dst.reg, {.kind = Kind::Aligned, .a = None, .b = 4});
					} else if (is(value, Kind::Offset)) {
						set(true, inst.dst.reg, {.kind = Kind::Aligned, .a = value.a, .b = value.b});
					}
				}
				break;
			}
			case Opcode::V_ADD_NC_U32: {
				if (!vector_result) {
					break;
				}
				const auto& other = is(a, Kind::Scaled) ? b : a;
				if (is(a, Kind::Scaled) || is(b, Kind::Scaled)) {
					if (is(other, Kind::Field)) {
						set(true, inst.dst.reg, {.kind = Kind::Offset, .a = other.a, .b = 4});
					} else if (is(other, Kind::Byte)) {
						set(true, inst.dst.reg, {.kind = Kind::Offset, .a = other.a, .b = other.b});
					}
					break;
				}
				const auto& aligned = is(a, Kind::Aligned) ? a : b;
				const auto& added   = is(a, Kind::Aligned) ? b : a;
				if (is(aligned, Kind::Aligned) && is(added, Kind::Constant) && added.a <= 64u) {
					set(true, inst.dst.reg, {.kind = Kind::Aligned, .a = aligned.a, .b = aligned.b, .c = aligned.c + added.a});
				}
				break;
			}
			case Opcode::V_ADD_I32: {
				const auto& aligned = is(a, Kind::Aligned) ? a : b;
				if (vector_result && is(aligned, Kind::Aligned) && is(is(a, Kind::Aligned) ? b : a, Kind::PointerLow)) {
					set(true, inst.dst.reg,
					    {.kind = Kind::AddressLow, .a = aligned.a, .b = aligned.b, .c = aligned.c, .d = inst.pc});
					const auto carry = ScalarIndex(inst.dst2);
					set(false, carry != None ? carry : VccLo, {.kind = Kind::Carry, .d = inst.pc});
				}
				break;
			}
			case Opcode::V_ADDC_U32:
				if (vector_result && is(c, Kind::Carry) &&
				    ((constant(a, 0) && is(b, Kind::PointerHighSigned)) ||
				     (constant(b, 0) && is(a, Kind::PointerHighSigned)))) {
					set(true, inst.dst.reg, {.kind = Kind::AddressHigh, .d = c.d});
				}
				break;
			case Opcode::BUFFER_LOAD_DWORD:
			case Opcode::BUFFER_LOAD_DWORDX2:
			case Opcode::BUFFER_LOAD_DWORDX3:
			case Opcode::BUFFER_LOAD_DWORDX4: {
				const auto descriptor = Descriptor(inst.src1);
				if (!inst.idxen || inst.offen || !constant(c, 0) || descriptor.first == None) {
					break;
				}
				if (is(a, Kind::Record) && descriptor == std::pair {m_records_load, 0u}) {
					for (uint32_t dword = 0; dword < inst.data_dwords; dword++) {
						set(true, inst.dst.reg + dword, {.kind = Kind::Field, .a = inst.offset + dword * 4u});
					}
				} else if (is(a, Kind::Field) && inst.opcode == Opcode::BUFFER_LOAD_DWORDX2 && inst.offset == 0 &&
				           (m_pointers_pc == None ||
				            (descriptor == std::pair {m_pointers_pc, m_pointers_dword} && m_pointer_field == a.a))) {
					m_pointers_pc    = descriptor.first;
					m_pointers_dword = descriptor.second;
					m_pointer_field  = a.a;
					set(true, inst.dst.reg, {.kind = Kind::PointerLow});
					set(true, inst.dst.reg + 1u, {.kind = Kind::PointerHigh});
				}
				break;
			}
			case Opcode::V_READFIRSTLANE_B32:
				if (is(a, Kind::Field)) {
					set(false, scalar_dst, {.kind = Kind::FirstLane, .a = a.a});
				}
				break;
			case Opcode::S_LSHL_B32:
				if (is(a, Kind::FirstLane) && constant(b, 4)) {
					set(false, scalar_dst, {.kind = Kind::TableOffset, .a = a.a});
				}
				break;
			case Opcode::S_MOV_B32:
				if (inst.src0.kind == OperandKind::ExecLo) {
					set(false, scalar_dst, {.kind = Kind::SavedExec, .a = m_state});
				} else if (!is(a, Kind::Descriptor) && !is(a, Kind::Target)) {
					set(false, scalar_dst, a);
				}
				break;
			case Opcode::S_AND_SAVEEXEC_B32: set(false, scalar_dst, {.kind = Kind::SavedExec, .a = m_state}); break;
			case Opcode::S_BUFFER_LOAD_DWORDX4:
				if (const auto descriptor = Descriptor(inst.src0);
				    descriptor.first != None && is(b, Kind::TableOffset) && inst.offset == 0 && scalar_dst != None) {
					for (uint32_t dword = 0; dword < 4; dword++) {
						set(false, scalar_dst + dword, {.kind = Kind::Target, .a = b.a, .b = dword});
					}
					if (std::ranges::find(m_store_tables, descriptor) == m_store_tables.end()) {
						m_store_tables.push_back(descriptor);
					}
				}
				break;
			default:
				if (inst.family == Family::FLAT) {
					// A device-address read: of the vertex, or one this does not follow.
					const auto high = inst.src0.kind == OperandKind::Vgpr ? Vector(inst.src0.reg + 1u) : Tag {};
					if (inst.opcode != Opcode::FLAT_LOAD_DWORD || !is(a, Kind::AddressLow) || !is(high, Kind::AddressHigh) ||
					    a.d != high.d || ((inst.raw[1] >> 16u) & 0x7fu) != 0x7du || inst.offset >= 0x800u) {
						return false;
					}
					const auto end  = a.c + inst.offset + 4u;
					const auto read = std::ranges::find_if(m_reads, [&](const RecordReach::Attribute& attribute) {
						return attribute.field == a.a && attribute.byte == a.b;
					});
					if (read != m_reads.end()) {
						read->end = std::max(read->end, end);
					} else {
						m_reads.push_back({a.a, a.b, end});
					}
					m_read_pcs.push_back(inst.pc);
				} else if (inst.family == Family::SMEM && name.starts_with("S_LOAD_DWORDX") && scalar_dst != None &&
				           inst.src1.kind == OperandKind::Null) {
					for (uint32_t dword = 0; dword < inst.data_dwords; dword++) {
						set(false, scalar_dst + dword, {.kind = Kind::Descriptor, .a = inst.pc, .b = dword});
					}
				} else if (inst.family == Family::MUBUF && name.find("_STORE") != std::string_view::npos) {
					if (const auto field = Target(inst.src1); field != None) {
						if (std::ranges::find(m_store_fields, field) == m_store_fields.end()) {
							m_store_fields.push_back(field);
						}
						m_store_pcs.push_back(inst.pc);
					}
				} else if (name.starts_with("V_CMP") && !name.starts_with("V_CMPX") && scalar_dst != None) {
					set(false, scalar_dst, {.kind = Kind::LaneMask, .a = m_state});
				}
				break;
		}
		Invalidate(i);
		for (size_t result = 0; result < count; result++) {
			auto& [vector, reg, tag] = results[result];
			tag.state                = m_state;
			if (vector ? reg < 256u : reg < 128u) {
				(vector ? m_vector[reg] : m_scalar[reg]) = tag;
			}
		}
		// EXEC: fewer of its lanes, the lanes a register saved, or lanes this does not know.
		const auto lanes_of = [&](const Tag& tag) { return is(tag, Kind::SavedExec) || is(tag, Kind::LaneMask); };
		if (name.starts_with("V_CMPX") || inst.opcode == Opcode::S_AND_SAVEEXEC_B32) {
			m_state = NewChild(m_state);
		} else if (name.find("SAVEEXEC") != std::string_view::npos || inst.dst.kind == OperandKind::ExecHi) {
			m_state = NewRoot();
		} else if (inst.dst.kind == OperandKind::ExecLo) {
			const bool from_exec = inst.src0.kind == OperandKind::ExecLo || inst.src1.kind == OperandKind::ExecLo;
			if (inst.opcode == Opcode::S_MOV_B32 && is(a, Kind::SavedExec)) {
				m_state = a.a;
			} else if (inst.opcode == Opcode::S_MOV_B32 && is(a, Kind::LaneMask)) {
				m_state = NewChild(a.a);
			} else if (inst.opcode == Opcode::S_ANDN2_B32 && lanes_of(a)) {
				m_state = NewChild(a.a);
			} else if (inst.opcode == Opcode::S_ANDN2_B32 && inst.src0.kind == OperandKind::ExecLo) {
				m_state = NewChild(m_state);
			} else if (inst.opcode == Opcode::S_AND_B32 && from_exec) {
				m_state = NewChild(m_state);
			} else if (inst.opcode == Opcode::S_AND_B32 && (lanes_of(a) || lanes_of(b))) {
				m_state = NewChild(lanes_of(a) ? a.a : b.a);
			} else {
				m_state = NewRoot();
			}
		}
		return true;
	}

	const std::vector<Instruction>& m_inst;
	std::array<Tag, 128>            m_scalar {};
	std::array<Tag, 256>            m_vector {};
	// The EXEC states: each one's lanes are lanes of its parent (its own index: of nothing known).
	std::vector<uint32_t>           m_states {0};
	uint32_t                        m_state = 0;
	// The search: its first instruction, the first one after it, and the registers of its results.
	size_t                          m_head   = 0;
	size_t                          m_start  = 0;
	uint32_t                        m_record = 0;
	uint32_t                        m_thread = 0;
	RecordReach::Descriptor         m_header;
	RecordReach::Descriptor         m_records;
	uint32_t                        m_header_offset = 0;
	uint32_t                        m_record_size   = 0;
	uint32_t                        m_threads_field = 0;
	// The loads (pc) of the V#s of the records and of the pointers, and the latter's first dword.
	uint32_t                        m_records_load   = None;
	uint32_t                        m_pointers_pc    = None;
	uint32_t                        m_pointers_dword = 0;
	uint32_t                        m_pointer_field  = 0;
	uint32_t                        m_stride_field   = None;
	uint32_t                        m_stride_byte    = 0;
	std::vector<RecordReach::Attribute>        m_reads;
	std::vector<uint32_t>                      m_store_fields;
	std::vector<std::pair<uint32_t, uint32_t>> m_store_tables;
	std::vector<uint32_t>                      m_read_pcs;
	std::vector<uint32_t>                      m_store_pcs;
};

} // namespace

bool DeriveRecordReach(const Program& program, uint32_t wave_size, RecordReach& reach) {
	reach = {};
	// Lane masks and EXEC as single registers.
	if (wave_size != 32 || program.has_swap_pc || !program.call_bodies.empty()) {
		return false;
	}
	return Finder(program).Find(reach);
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
