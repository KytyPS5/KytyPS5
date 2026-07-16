#include "graphics/shader/recompiler/ScalarProvenance.h"

#include <algorithm>
#include <array>
#include <compare>
#include <deque>
#include <fmt/format.h>
#include <map>

namespace Libs::Graphics::ShaderRecompiler::IR {
uint32_t ScalarValueArgCount(ScalarValueOp op) {
	switch (op) {
		case ScalarValueOp::Not: return 1;
		case ScalarValueOp::Add:
		case ScalarValueOp::Sub:
		case ScalarValueOp::Mul:
		case ScalarValueOp::And:
		case ScalarValueOp::AndNot:
		case ScalarValueOp::Or:
		case ScalarValueOp::OrNot:
		case ScalarValueOp::Xor:
		case ScalarValueOp::ShiftLeft:
		case ScalarValueOp::ShiftRight:
		case ScalarValueOp::ShiftRightArithmetic:
		case ScalarValueOp::BitFieldMaskU32:
		case ScalarValueOp::BitFieldMaskU64Low:
		case ScalarValueOp::BitFieldMaskU64High: return 2;
		case ScalarValueOp::AddCarry:
		case ScalarValueOp::Carry:
		case ScalarValueOp::SubBorrow:
		case ScalarValueOp::Borrow:
		case ScalarValueOp::Add3:
		case ScalarValueOp::ShiftLeftAdd:
		case ScalarValueOp::ShiftLeftAddCarry:
		case ScalarValueOp::AddShiftLeft:
		case ScalarValueOp::XorAdd:
		case ScalarValueOp::ShiftLeftOr:
		case ScalarValueOp::ReadConst: return 3;
		case ScalarValueOp::ReadConstBuffer: return 5;
		default: return 0;
	}
}

namespace {

constexpr uint32_t ScalarRegisters = 128;
constexpr uint32_t VectorRegisters = 256;

struct ScalarState {
	std::array<uint32_t, ScalarRegisters> regs          = {};
	std::array<uint32_t, VectorRegisters> address_bases = {};
	uint32_t                              scc           = ScalarProvenance::Undefined;
	uint32_t                              m0            = ScalarProvenance::Undefined;
	std::map<uint64_t, uint32_t>          vector_lanes;

	void Fill(uint32_t value) {
		regs.fill(value);
		address_bases.fill(value);
		scc = value;
		m0  = value;
		vector_lanes.clear();
	}

	bool operator==(const ScalarState&) const = default;
};

struct ValueKey {
	ScalarValueOp           op   = ScalarValueOp::Undefined;
	uint32_t                imm  = 0;
	std::array<uint32_t, 6> args = {};

	auto operator<=>(const ValueKey&) const = default;
};

struct DescriptorKey {
	std::array<uint32_t, 8> dwords      = {};
	uint32_t                dword_count = 0;

	auto operator<=>(const DescriptorKey&) const = default;
};

bool ScalarRegister(const Operand& operand, uint32_t& reg) {
	if (operand.kind != OperandKind::Register) {
		return false;
	}
	if (operand.reg.file == RegisterFile::Scalar && operand.reg.index < ScalarRegisters) {
		reg = operand.reg.index;
		return true;
	}
	if (operand.reg.file == RegisterFile::Vcc && operand.reg.index < 2) {
		reg = 106u + operand.reg.index;
		return true;
	}
	return false;
}

bool VectorRegister(const Operand& operand, uint32_t& reg) {
	if (operand.kind != OperandKind::Register || operand.reg.file != RegisterFile::Vector ||
	    operand.reg.index >= VectorRegisters) {
		return false;
	}
	reg = operand.reg.index;
	return true;
}

bool FlatStore(Opcode op) {
	switch (op) {
		case Opcode::FlatStoreByte:
		case Opcode::FlatStoreShort:
		case Opcode::FlatStoreDword: return true;
		default: return false;
	}
}

bool PairDwordOpcode(Opcode op) {
	switch (op) {
		case Opcode::MoveU64:
		case Opcode::WqmB64:
		case Opcode::SaveexecB64:
		case Opcode::BitwiseAndU64:
		case Opcode::BitwiseAndNotU64:
		case Opcode::BitwiseOrU64:
		case Opcode::BitwiseOrNotU64:
		case Opcode::BitwiseXorU64:
		case Opcode::BitwiseNandU64:
		case Opcode::BitwiseNorU64:
		case Opcode::BitwiseXnorU64:
		case Opcode::BitwiseNotU64:
		case Opcode::BitFieldMaskU64:
		case Opcode::BitFieldExtractU64:
		case Opcode::BitReplicateB64B32:
		case Opcode::ShiftLeftLogicalU64:
		case Opcode::ShiftRightLogicalU64:
		case Opcode::SelectU64: return true;
		default: return false;
	}
}

bool ScalarMemoryLoad(Opcode op) {
	return op == Opcode::SLoadDword || op == Opcode::SBufferLoadDword;
}

bool ValueResolved(const ScalarProvenance& provenance, uint32_t id, std::vector<uint8_t>* visited) {
	if (id <= ScalarProvenance::Unknown || id >= provenance.values.size()) {
		return false;
	}
	if ((*visited)[id] == 1) {
		return false;
	}
	if ((*visited)[id] == 2) {
		return true;
	}
	if ((*visited)[id] == 3) {
		return false;
	}
	(*visited)[id]       = 1;
	const auto& value    = provenance.values[id];
	bool        resolved = true;
	if (value.op == ScalarValueOp::Phi) {
		resolved = !value.phi_args.empty() &&
		           std::all_of(value.phi_args.begin(), value.phi_args.end(), [&](uint32_t arg) {
			           return ValueResolved(provenance, arg, visited);
		           });
	} else {
		const auto args = ScalarValueArgCount(value.op);
		for (uint32_t i = 0; i < args && resolved; i++) {
			resolved = ValueResolved(provenance, value.args[i], visited);
		}
	}
	(*visited)[id] = resolved ? 2 : 3;
	return resolved;
}

class Builder {
public:
	explicit Builder(Program* program): m_program(program), m_graph(program->provenance) {}

	bool Run(std::string* error) {
		m_graph = {};
		m_graph.values.resize(2);
		m_graph.values[ScalarProvenance::Undefined].op = ScalarValueOp::Undefined;
		m_graph.values[ScalarProvenance::Unknown].op   = ScalarValueOp::Unknown;

		const auto block_count = m_program->blocks.size();
		m_entry.resize(block_count);
		m_exit.resize(block_count);
		m_phi.resize(block_count);
		m_exit_ready.assign(block_count, false);
		m_queued.assign(block_count, false);
		for (auto& state: m_entry) {
			state.Fill(ScalarProvenance::Undefined);
		}
		for (auto& state: m_exit) {
			state.Fill(ScalarProvenance::Undefined);
		}
		for (auto& phis: m_phi) {
			phis.Fill(ScalarProvenance::Undefined);
		}

		m_user_data.Fill(ScalarProvenance::Undefined);
		for (uint32_t reg = 0; reg < ScalarRegisters; reg++) {
			m_user_data.regs[reg] =
			    reg >= m_program->user_data_base &&
			            reg - m_program->user_data_base < m_program->user_data_count
			        ? InternValue({ScalarValueOp::UserData, 0, reg})
			        : ScalarProvenance::Unknown;
		}
		m_user_data.scc = ScalarProvenance::Unknown;
		m_user_data.m0  = ScalarProvenance::Unknown;

		if (block_count == 0) {
			return true;
		}
		for (const auto& block: m_program->blocks) {
			for (const auto predecessor: block.predecessors) {
				if (predecessor >= block_count) {
					return Fail(error, fmt::format("scalar provenance has invalid predecessor {}",
					                               predecessor));
				}
			}
			for (const auto successor: block.successors) {
				if (successor >= block_count) {
					return Fail(error, fmt::format("scalar provenance has invalid successor {}",
					                               successor));
				}
			}
			if (!ValidateScalarMemoryGroups(block, error)) {
				return false;
			}
		}
		Queue(0);
		while (!m_work.empty()) {
			const auto block_index = m_work.front();
			m_work.pop_front();
			m_queued[block_index] = false;

			auto       next_entry    = MergeEntry(block_index);
			const bool entry_changed = next_entry != m_entry[block_index];
			m_entry[block_index]     = next_entry;

			auto       next_exit = Execute(m_program->blocks[block_index], next_entry, false);
			const bool was_ready = m_exit_ready[block_index];
			if (was_ready && !entry_changed && next_exit == m_exit[block_index]) {
				continue;
			}
			m_exit[block_index]       = next_exit;
			m_exit_ready[block_index] = true;
			for (const auto successor: m_program->blocks[block_index].successors) {
				Queue(successor);
			}
		}

		for (size_t block = 0; block < block_count; block++) {
			Execute(m_program->blocks[block], m_entry[block], true);
		}
		return true;
	}

private:
	bool Fail(std::string* error, std::string text) const {
		if (error != nullptr) {
			*error = std::move(text);
		}
		return false;
	}

	uint32_t AddValue(ScalarValue value) {
		m_graph.values.push_back(std::move(value));
		return static_cast<uint32_t>(m_graph.values.size() - 1);
	}

	bool ValidateScalarMemoryGroups(const BasicBlock& block, std::string* error) const {
		for (size_t index = 0; index < block.instructions.size();) {
			const auto& first = block.instructions[index];
			if (!ScalarMemoryLoad(first.op) || first.memory.component_count == 1) {
				index++;
				continue;
			}
			const auto count     = first.memory.component_count;
			uint32_t   first_dst = 0;
			if (count == 0 || first.memory.component_index != 0 ||
			    index + count > block.instructions.size() ||
			    !ScalarRegister(first.dst, first_dst)) {
				return Fail(error,
				            fmt::format("malformed scalar-memory group at pc=0x{:x}", first.pc));
			}
			for (uint32_t component = 0; component < count; component++) {
				const auto& inst = block.instructions[index + component];
				uint32_t    dst  = 0;
				if (inst.op != first.op || inst.pc != first.pc ||
				    inst.memory.component_count != count ||
				    inst.memory.component_index != component ||
				    inst.memory.resource != first.memory.resource ||
				    inst.memory.offset != first.memory.offset + component * 4u ||
				    inst.src_count != first.src_count ||
				    !std::equal(std::begin(inst.src), std::end(inst.src), std::begin(first.src)) ||
				    !ScalarRegister(inst.dst, dst) || dst != first_dst + component) {
					return Fail(error,
					            fmt::format("malformed scalar-memory component {} at pc=0x{:x}",
					                        component, first.pc));
				}
			}
			index += count;
		}
		return true;
	}

	uint32_t InternValue(ScalarValue value) {
		const ValueKey key {value.op, value.imm, value.args};
		if (const auto found = m_values.find(key); found != m_values.end()) {
			return found->second;
		}
		const auto id = AddValue(std::move(value));
		m_values.emplace(key, id);
		return id;
	}

	uint32_t Constant(uint32_t value) { return InternValue({ScalarValueOp::Constant, 0, value}); }

	static uint64_t VectorLaneKey(uint32_t reg, uint32_t lane) {
		return (static_cast<uint64_t>(reg) << 32u) | lane;
	}

	uint32_t OperandValue(const Operand& operand, const ScalarState& state) {
		uint32_t reg = 0;
		if (ScalarRegister(operand, reg)) {
			return state.regs[reg];
		}
		if (operand.kind == OperandKind::Register && operand.reg.file == RegisterFile::Scc) {
			return state.scc;
		}
		if (operand.kind == OperandKind::Register && operand.reg.file == RegisterFile::M0) {
			return state.m0;
		}
		if (operand.kind == OperandKind::ImmediateU32 ||
		    operand.kind == OperandKind::PcRelativeU32) {
			if (operand.kind == OperandKind::PcRelativeU32) {
				return InternValue({ScalarValueOp::PcRelativeLow, 0, operand.imm});
			}
			return Constant(operand.imm);
		}
		return ScalarProvenance::Unknown;
	}

	uint32_t Define(const Instruction& inst, ScalarValueOp op, const ScalarState& state,
	                uint32_t imm = 0) {
		ScalarValue node;
		node.op  = op;
		node.pc  = inst.pc;
		node.imm = imm;
		for (uint32_t i = 0; i < inst.src_count && i < node.args.size(); i++) {
			node.args[i] = OperandValue(inst.src[i], state);
		}
		return InternValue(std::move(node));
	}

	uint32_t DefineBorrow(const Instruction& inst, const ScalarState& state) {
		ScalarValue node;
		node.op = ScalarValueOp::Borrow;
		node.pc = inst.pc;
		node.args[0] =
		    inst.src_count > 0 ? OperandValue(inst.src[0], state) : ScalarProvenance::Unknown;
		node.args[1] =
		    inst.src_count > 1 ? OperandValue(inst.src[1], state) : ScalarProvenance::Unknown;
		node.args[2] = inst.src_count > 2 ? OperandValue(inst.src[2], state) : Constant(0);
		return InternValue(std::move(node));
	}

	bool ConstantOperand(const Operand& operand, const ScalarState& state, uint32_t* value) {
		if (value == nullptr) {
			return false;
		}
		const auto id = OperandValue(operand, state);
		if (id >= m_graph.values.size() || m_graph.values[id].op != ScalarValueOp::Constant) {
			return false;
		}
		*value = m_graph.values[id].imm;
		return true;
	}

	uint32_t ReadVectorLane(const Instruction& inst, const ScalarState& state) {
		if (inst.src_count < 2 || inst.src[0].kind != OperandKind::Register ||
		    inst.src[0].reg.file != RegisterFile::Vector ||
		    (m_program->wave_size != 32 && m_program->wave_size != 64)) {
			return ScalarProvenance::Unknown;
		}
		uint32_t lane = 0;
		if (!ConstantOperand(inst.src[1], state, &lane)) {
			return ScalarProvenance::Unknown;
		}
		const auto found = state.vector_lanes.find(
		    VectorLaneKey(inst.src[0].reg.index, lane % m_program->wave_size));
		return found != state.vector_lanes.end() ? found->second : ScalarProvenance::Unknown;
	}

	void ClearVectorLanes(uint32_t reg, ScalarState* state) {
		const auto first = state->vector_lanes.lower_bound(VectorLaneKey(reg, 0));
		const auto last = state->vector_lanes.lower_bound((static_cast<uint64_t>(reg) + 1u) << 32u);
		state->vector_lanes.erase(first, last);
	}

	void WriteVectorDestination(const Instruction& inst, ScalarState* state) {
		if (inst.dst.kind != OperandKind::Register || inst.dst.reg.file != RegisterFile::Vector) {
			return;
		}
		const uint32_t reg          = inst.dst.reg.index;
		uint32_t       address_base = ScalarProvenance::Unknown;
		if (inst.op == Opcode::MoveU32 || inst.op == Opcode::IAddU32 ||
		    inst.op == Opcode::IAddCarryU32 || inst.op == Opcode::ISubU32) {
			for (uint32_t i = 0; i < inst.src_count; i++) {
				uint32_t src_reg = 0;
				if (VectorRegister(inst.src[i], src_reg) &&
				    state->address_bases[src_reg] > ScalarProvenance::Unknown) {
					address_base = state->address_bases[src_reg];
					break;
				}
			}
			if (address_base == ScalarProvenance::Unknown) {
				for (uint32_t i = 0; i < inst.src_count; i++) {
					uint32_t src_reg = 0;
					if (ScalarRegister(inst.src[i], src_reg) &&
					    state->regs[src_reg] > ScalarProvenance::Unknown) {
						address_base = state->regs[src_reg];
						break;
					}
				}
			}
		}
		state->address_bases[reg] = ScalarProvenance::Unknown;
		if (inst.op == Opcode::MoveRelDestU32) {
			state->vector_lanes.clear();
			return;
		}
		if (inst.op != Opcode::WriteLaneU32) {
			uint32_t dwords = std::max(inst.memory.data_dwords, 1u);
			if (PairDwordOpcode(inst.op) || inst.op == Opcode::UMadU64U32) {
				dwords = std::max(dwords, 2u);
			}
			for (uint32_t i = 0; i < dwords && reg <= UINT32_MAX - i; i++) {
				ClearVectorLanes(reg + i, state);
				if (reg + i < state->address_bases.size()) {
					state->address_bases[reg + i] = ScalarProvenance::Unknown;
				}
			}
			state->address_bases[reg] = address_base;
			return;
		}
		if (inst.src_count < 2 || (m_program->wave_size != 32 && m_program->wave_size != 64)) {
			ClearVectorLanes(reg, state);
			return;
		}
		uint32_t lane = 0;
		if (!ConstantOperand(inst.src[1], *state, &lane)) {
			ClearVectorLanes(reg, state);
			return;
		}
		state->vector_lanes[VectorLaneKey(reg, lane % m_program->wave_size)] =
		    OperandValue(inst.src[0], *state);
	}

	uint32_t ReadConst(const Instruction& inst, const ScalarState& state, bool buffer) {
		ScalarValue node;
		node.op         = buffer ? ScalarValueOp::ReadConstBuffer : ScalarValueOp::ReadConst;
		node.pc         = inst.pc;
		const auto base = buffer ? inst.memory.resource * 4u : inst.memory.resource;
		if (base + 1u >= ScalarRegisters) {
			return ScalarProvenance::Unknown;
		}
		node.imm = inst.memory.offset;
		if (buffer) {
			if (base + 3u >= ScalarRegisters) {
				return ScalarProvenance::Unknown;
			}
			for (uint32_t i = 0; i < 4; i++) {
				node.args[i] = state.regs[base + i];
			}
			node.args[4] = inst.src_count != 0 ? OperandValue(inst.src[0], state) : Constant(0);
		} else {
			node.args[0] = state.regs[base];
			node.args[1] = state.regs[base + 1u];
			node.args[2] = inst.src_count != 0 ? OperandValue(inst.src[0], state) : Constant(0);
		}
		return InternValue(std::move(node));
	}

	ScalarValueOp Operation(Opcode op) const {
		switch (op) {
			case Opcode::IAddU32:
			case Opcode::ScalarSignedAddOverflowI32: return ScalarValueOp::Add;
			case Opcode::IAddCarryU32:
			case Opcode::ScalarAddCarryU32: return ScalarValueOp::AddCarry;
			case Opcode::ISubU32:
			case Opcode::ScalarSubBorrowU32:
			case Opcode::ScalarSignedSubOverflowI32: return ScalarValueOp::Sub;
			case Opcode::ScalarSubBorrowCarryU32: return ScalarValueOp::SubBorrow;
			case Opcode::IMulU32: return ScalarValueOp::Mul;
			case Opcode::BitwiseAndU32: return ScalarValueOp::And;
			case Opcode::BitwiseAndNotU32: return ScalarValueOp::AndNot;
			case Opcode::BitwiseOrU32: return ScalarValueOp::Or;
			case Opcode::BitwiseOrNotU32: return ScalarValueOp::OrNot;
			case Opcode::BitwiseXorU32: return ScalarValueOp::Xor;
			case Opcode::BitwiseNotU32: return ScalarValueOp::Not;
			case Opcode::ShiftLeftLogicalU32: return ScalarValueOp::ShiftLeft;
			case Opcode::ShiftRightLogicalU32: return ScalarValueOp::ShiftRight;
			case Opcode::ShiftRightArithmeticI32: return ScalarValueOp::ShiftRightArithmetic;
			case Opcode::BitFieldMaskU32: return ScalarValueOp::BitFieldMaskU32;
			case Opcode::IAdd3U32: return ScalarValueOp::Add3;
			case Opcode::ScalarShiftLeftAddCarryU32:
			case Opcode::ShiftLeftAddU32: return ScalarValueOp::ShiftLeftAdd;
			case Opcode::AddShiftLeftU32: return ScalarValueOp::AddShiftLeft;
			case Opcode::XorAddU32: return ScalarValueOp::XorAdd;
			case Opcode::ShiftLeftOrU32: return ScalarValueOp::ShiftLeftOr;
			default: return ScalarValueOp::Unknown;
		}
	}

	void UpdateScc(const Instruction& inst, const ScalarState& before, ScalarState* state) {
		switch (inst.op) {
			case Opcode::ScalarAddCarryU32:
				state->scc = Define(inst, ScalarValueOp::Carry, before);
				break;
			case Opcode::ScalarSubBorrowU32:
			case Opcode::ScalarSubBorrowCarryU32: state->scc = DefineBorrow(inst, before); break;
			case Opcode::ScalarShiftLeftAddCarryU32:
				state->scc = Define(inst, ScalarValueOp::ShiftLeftAddCarry, before);
				break;
			default: break;
		}
	}

	void WriteDestination(const Instruction& inst, ScalarState* state) {
		if (inst.dst.kind == OperandKind::Register && inst.dst.reg.file == RegisterFile::Scc) {
			state->scc = inst.op == Opcode::MoveU32 && inst.src_count != 0
			                 ? OperandValue(inst.src[0], *state)
			                 : ScalarProvenance::Unknown;
			return;
		}
		if (inst.dst.kind == OperandKind::Register && inst.dst.reg.file == RegisterFile::M0) {
			const auto before = *state;
			const auto op     = Operation(inst.op);
			state->m0         = inst.op == Opcode::MoveU32 && inst.src_count != 0
			                        ? OperandValue(inst.src[0], before)
			                    : op != ScalarValueOp::Unknown ? Define(inst, op, before)
			                                                   : ScalarProvenance::Unknown;
			UpdateScc(inst, before, state);
			return;
		}

		uint32_t dst = 0;
		if (!ScalarRegister(inst.dst, dst)) {
			return;
		}

		const auto before = *state;
		state->regs[dst]  = ScalarProvenance::Unknown;
		if (PairDwordOpcode(inst.op) && dst + 1 < ScalarRegisters) {
			state->regs[dst + 1] = ScalarProvenance::Unknown;
		}

		uint32_t value = ScalarProvenance::Unknown;
		switch (inst.op) {
			case Opcode::MoveU32:
			case Opcode::MoveF32Bits:
				value = inst.src_count == 0 ? ScalarProvenance::Unknown
				        : inst.src[0].kind == OperandKind::PcRelativeU32
				            ? InternValue({ScalarValueOp::PcRelativeLow, inst.pc, inst.src[0].imm})
				            : OperandValue(inst.src[0], before);
				if (inst.op == Opcode::MoveU32 && inst.src_count != 0 &&
				    inst.src[0].kind == OperandKind::ImmediateU32 && inst.src[0].imm == 0 &&
				    dst != 0) {
					const auto low = before.regs[dst - 1];
					if (low < m_graph.values.size() &&
					    m_graph.values[low].op == ScalarValueOp::PcRelativeLow &&
					    m_graph.values[low].pc == inst.pc) {
						value = InternValue(
						    {ScalarValueOp::PcRelativeHigh, inst.pc, m_graph.values[low].imm});
					}
				}
				break;
			case Opcode::MoveU64: {
				uint32_t src = 0;
				if (inst.src_count != 0 && dst + 1 < ScalarRegisters) {
					if (ScalarRegister(inst.src[0], src) && src + 1 < ScalarRegisters) {
						value                = before.regs[src];
						state->regs[dst + 1] = before.regs[src + 1];
					} else {
						value                = OperandValue(inst.src[0], before);
						state->regs[dst + 1] = Constant(
						    inst.src[0].kind == OperandKind::ImmediateU32 && inst.src[0].sext_64
						        ? UINT32_MAX
						        : 0u);
					}
				}
				break;
			}
			case Opcode::BitFieldMaskU64:
				value = Define(inst, ScalarValueOp::BitFieldMaskU64Low, before);
				if (dst + 1 < ScalarRegisters) {
					state->regs[dst + 1] = Define(inst, ScalarValueOp::BitFieldMaskU64High, before);
				}
				break;
			case Opcode::SLoadDword: value = ReadConst(inst, before, false); break;
			case Opcode::SBufferLoadDword: value = ReadConst(inst, before, true); break;
			case Opcode::ReadLaneU32: value = ReadVectorLane(inst, before); break;
			default: {
				const auto op = Operation(inst.op);
				if (op != ScalarValueOp::Unknown) {
					value = Define(inst, op, before);
				}
				break;
			}
		}
		state->regs[dst] = value;
		UpdateScc(inst, before, state);

		uint32_t dst2 = 0;
		if (ScalarRegister(inst.dst2, dst2)) {
			state->regs[dst2] =
			    inst.src_count > 1 ? OperandValue(inst.src[1], before) : ScalarProvenance::Unknown;
		}
	}

	uint32_t AddDescriptor(DescriptorValue descriptor) {
		const DescriptorKey key {descriptor.dwords, descriptor.dword_count};
		if (const auto found = m_descriptors.find(key); found != m_descriptors.end()) {
			return found->second;
		}
		m_graph.descriptors.push_back(descriptor);
		const auto id = static_cast<uint32_t>(m_graph.descriptors.size() - 1) + 2u;
		m_descriptors.emplace(key, id);
		return id;
	}

	uint32_t AddDescriptor(const ScalarState& state, uint32_t base, uint32_t dwords) {
		if (base >= ScalarRegisters || dwords == 0 || dwords > 8 ||
		    base + dwords > ScalarRegisters) {
			return ScalarProvenance::Unknown;
		}
		DescriptorValue descriptor;
		descriptor.dword_count = dwords;
		for (uint32_t i = 0; i < dwords; i++) {
			descriptor.dwords[i] = state.regs[base + i];
		}
		return AddDescriptor(descriptor);
	}

	uint32_t AddFlatAddressDescriptor(const Instruction& inst, const ScalarState& state) {
		const uint32_t first = FlatStore(inst.op) ? 1u : 0u;
		if (inst.src_count < first + 2u) {
			return ScalarProvenance::Unknown;
		}
		uint32_t low  = 0;
		uint32_t high = 0;
		if (!VectorRegister(inst.src[first], low) || !VectorRegister(inst.src[first + 1u], high) ||
		    state.address_bases[low] <= ScalarProvenance::Unknown ||
		    state.address_bases[high] <= ScalarProvenance::Unknown) {
			return ScalarProvenance::Unknown;
		}
		DescriptorValue descriptor;
		descriptor.dword_count = 2;
		descriptor.dwords[0]   = state.address_bases[low];
		descriptor.dwords[1]   = state.address_bases[high];
		return AddDescriptor(descriptor);
	}

	void AttachSources(Instruction* inst, const ScalarState& state) {
		inst->memory.resource_source = 0;
		inst->memory.sampler_source  = 0;
		if (inst->op == Opcode::SLoadDword) {
			inst->memory.resource_source = AddDescriptor(state, inst->memory.resource, 2);
			return;
		}
		if (inst->memory.kind == ResourceKind::Flat || inst->memory.kind == ResourceKind::Global ||
		    inst->memory.kind == ResourceKind::Scratch) {
			inst->memory.resource_source = ScalarProvenance::Unknown;
			if (inst->memory.kind == ResourceKind::Flat) {
				inst->memory.resource_source = AddFlatAddressDescriptor(*inst, state);
			}
			for (uint32_t i = 0; i < inst->src_count; i++) {
				const auto& source = inst->src[i];
				if (inst->memory.resource_source == ScalarProvenance::Unknown &&
				    source.kind == OperandKind::Register &&
				    source.reg.file == RegisterFile::Scalar) {
					inst->memory.resource_source = AddDescriptor(state, source.reg.index, 2);
					break;
				}
			}
			return;
		}
		if (inst->memory.kind == ResourceKind::Buffer ||
		    (inst->memory.kind == ResourceKind::ScalarBuffer &&
		     inst->op == Opcode::SBufferLoadDword)) {
			const auto base              = inst->memory.resource * 4u;
			inst->memory.resource_source = AddDescriptor(state, base, 4);
			return;
		}
		if (inst->memory.kind == ResourceKind::Image ||
		    inst->memory.kind == ResourceKind::ImageUint ||
		    inst->memory.kind == ResourceKind::StorageImage ||
		    inst->memory.kind == ResourceKind::StorageImageUint) {
			inst->memory.resource_source = AddDescriptor(state, inst->memory.resource * 4u, 8);
			if (inst->op == Opcode::ImageSample || inst->op == Opcode::ImageGather4 ||
			    inst->op == Opcode::ImageGetLod) {
				inst->memory.sampler_source = AddDescriptor(state, inst->memory.sampler * 4u, 4);
			}
		}
	}

	void AttachScalarSources(Instruction* inst, const ScalarState& state) {
		std::fill(std::begin(inst->scalar_sources), std::end(inst->scalar_sources),
		          ScalarProvenance::Undefined);
		for (uint32_t i = 0; i < inst->src_count; i++) {
			const auto& source = inst->src[i];
			if (source.kind == OperandKind::Register && source.reg.file != RegisterFile::Vector &&
			    source.reg.file != RegisterFile::Exec) {
				inst->scalar_sources[i] = OperandValue(source, state);
			}
		}
	}

	ScalarState Execute(BasicBlock& block, ScalarState state, bool attach_sources) {
		for (size_t index = 0; index < block.instructions.size();) {
			auto&    inst      = block.instructions[index];
			uint32_t first_dst = 0;
			if (ScalarMemoryLoad(inst.op) && inst.memory.component_count > 1 &&
			    ScalarRegister(inst.dst, first_dst)) {
				const auto before = state;
				for (uint32_t component = 0; component < inst.memory.component_count; component++) {
					auto& component_inst = block.instructions[index + component];
					if (attach_sources) {
						component_inst.scalar_value = ScalarProvenance::Undefined;
						AttachScalarSources(&component_inst, before);
						AttachSources(&component_inst, before);
					}
					auto component_state = before;
					WriteDestination(component_inst, &component_state);
					uint32_t dst = 0;
					ScalarRegister(component_inst.dst, dst);
					state.regs[dst] = component_state.regs[dst];
					if (attach_sources) {
						component_inst.scalar_value = component_state.regs[dst];
					}
				}
				index += inst.memory.component_count;
				continue;
			}
			if (attach_sources) {
				inst.scalar_value = ScalarProvenance::Undefined;
				AttachScalarSources(&inst, state);
				AttachSources(&inst, state);
			}
			WriteVectorDestination(inst, &state);
			WriteDestination(inst, &state);
			if (attach_sources) {
				uint32_t dst = 0;
				if (ScalarRegister(inst.dst, dst)) {
					inst.scalar_value = state.regs[dst];
				}
			}
			index++;
		}
		return state;
	}

	ScalarState MergeEntry(size_t block_index) {
		ScalarState result;
		result.Fill(ScalarProvenance::Undefined);
		const auto& block = m_program->blocks[block_index];
		const auto  merge = [&](uint32_t initial, auto predecessor_value, uint32_t* phi) {
			std::vector<uint32_t> incoming;
			if (initial != ScalarProvenance::Undefined) {
				incoming.push_back(initial);
			}
			for (const auto predecessor: block.predecessors) {
				const auto value = predecessor_value(predecessor, m_exit[predecessor]);
				if (value != ScalarProvenance::Undefined &&
				    std::find(incoming.begin(), incoming.end(), value) == incoming.end()) {
					incoming.push_back(value);
				}
			}
			if (incoming.empty()) {
				return ScalarProvenance::Undefined;
			}
			if (incoming.size() == 1) {
				return incoming[0];
			}
			if (*phi == ScalarProvenance::Undefined) {
				*phi = AddValue({ScalarValueOp::Phi, block.start_pc});
			}
			m_graph.values[*phi].phi_args = std::move(incoming);
			return *phi;
		};

		for (uint32_t reg = 0; reg < ScalarRegisters; reg++) {
			const auto initial =
			    block_index == 0 ? m_user_data.regs[reg] : ScalarProvenance::Undefined;
			result.regs[reg] = merge(
			    initial, [reg](uint32_t, const ScalarState& state) { return state.regs[reg]; },
			    &m_phi[block_index].regs[reg]);
		}
		for (uint32_t reg = 0; reg < VectorRegisters; reg++) {
			result.address_bases[reg] = merge(
			    block_index == 0 ? ScalarProvenance::Unknown : ScalarProvenance::Undefined,
			    [reg](uint32_t, const ScalarState& state) { return state.address_bases[reg]; },
			    &m_phi[block_index].address_bases[reg]);
		}
		result.scc = merge(
		    block_index == 0 ? m_user_data.scc : ScalarProvenance::Undefined,
		    [](uint32_t, const ScalarState& state) { return state.scc; }, &m_phi[block_index].scc);
		result.m0 = merge(
		    block_index == 0 ? m_user_data.m0 : ScalarProvenance::Undefined,
		    [](uint32_t, const ScalarState& state) { return state.m0; }, &m_phi[block_index].m0);

		std::vector<uint64_t> vector_lanes;
		for (const auto predecessor: block.predecessors) {
			for (const auto& [lane, value]: m_exit[predecessor].vector_lanes) {
				(void)value;
				vector_lanes.push_back(lane);
			}
		}
		std::sort(vector_lanes.begin(), vector_lanes.end());
		vector_lanes.erase(std::unique(vector_lanes.begin(), vector_lanes.end()),
		                   vector_lanes.end());
		for (const auto lane: vector_lanes) {
			auto& phi                 = m_phi[block_index].vector_lanes[lane];
			result.vector_lanes[lane] = merge(
			    block_index == 0 ? ScalarProvenance::Unknown : ScalarProvenance::Undefined,
			    [this, lane](uint32_t predecessor, const ScalarState& state) {
				    if (!m_exit_ready[predecessor]) {
					    return ScalarProvenance::Undefined;
				    }
				    const auto found = state.vector_lanes.find(lane);
				    return found != state.vector_lanes.end() ? found->second
				                                             : ScalarProvenance::Unknown;
			    },
			    &phi);
		}
		return result;
	}

	void Queue(uint32_t block) {
		if (!m_queued[block]) {
			m_queued[block] = true;
			m_work.push_back(block);
		}
	}

	Program*                          m_program;
	ScalarProvenance&                 m_graph;
	ScalarState                       m_user_data;
	std::vector<ScalarState>          m_entry;
	std::vector<ScalarState>          m_exit;
	std::vector<ScalarState>          m_phi;
	std::vector<bool>                 m_exit_ready;
	std::vector<bool>                 m_queued;
	std::deque<uint32_t>              m_work;
	std::map<ValueKey, uint32_t>      m_values;
	std::map<DescriptorKey, uint32_t> m_descriptors;
};

} // namespace

bool BuildScalarProvenance(Program* program, std::string* error) {
	if (program == nullptr) {
		if (error != nullptr) {
			*error = "invalid scalar provenance program";
		}
		return false;
	}
	if (program->resource_tracking_complete) {
		if (error != nullptr) {
			*error = "cannot rebuild scalar provenance after resource tracking";
		}
		return false;
	}
	if (program->user_data_count > 64) {
		if (error != nullptr) {
			*error = "scalar provenance user-data count exceeds 64 SGPRs";
		}
		return false;
	}
	if (program->user_data_base > ScalarRegisters ||
	    program->user_data_count > ScalarRegisters - program->user_data_base) {
		if (error != nullptr) {
			*error = "scalar provenance user-data window is out of range";
		}
		return false;
	}
	if (program->srt_patching_complete) {
		if (error != nullptr) {
			*error = "cannot rebuild scalar provenance after SRT patching";
		}
		return false;
	}
	program->srt               = {};
	program->srt_plan_complete = false;
	return Builder(program).Run(error);
}

const DescriptorValue* GetDescriptorSource(const Program& program, uint32_t source) {
	if (source < 2 || source - 2 >= program.provenance.descriptors.size()) {
		return nullptr;
	}
	return &program.provenance.descriptors[source - 2];
}

bool DescriptorSourceResolved(const Program& program, uint32_t source) {
	const auto* descriptor = GetDescriptorSource(program, source);
	if (descriptor == nullptr) {
		return false;
	}
	std::vector<uint8_t> visited(program.provenance.values.size());
	for (uint32_t i = 0; i < descriptor->dword_count; i++) {
		if (!ValueResolved(program.provenance, descriptor->dwords[i], &visited)) {
			return false;
		}
	}
	return true;
}

std::string ScalarValueToString(const ScalarProvenance& provenance, uint32_t value) {
	if (value >= provenance.values.size()) {
		return "invalid";
	}
	const auto& node = provenance.values[value];
	switch (node.op) {
		case ScalarValueOp::Undefined: return "undefined";
		case ScalarValueOp::Unknown: return "unknown";
		case ScalarValueOp::UserData: return fmt::format("ud{}", node.imm);
		case ScalarValueOp::Constant: return fmt::format("0x{:08x}", node.imm);
		case ScalarValueOp::PcRelativeLow: return fmt::format("pc_lo+0x{:x}", node.imm);
		case ScalarValueOp::PcRelativeHigh: return fmt::format("pc_hi+0x{:x}", node.imm);
		case ScalarValueOp::ReadConst:
			return fmt::format("read_const({}, {}, {}, +{})", node.args[0], node.args[1],
			                   node.args[2], node.imm);
		case ScalarValueOp::ReadConstBuffer:
			return fmt::format("read_const_buffer({}, {}, {}, {}, {}, +{})", node.args[0],
			                   node.args[1], node.args[2], node.args[3], node.args[4], node.imm);
		case ScalarValueOp::Phi: return fmt::format("phi{}", value);
		case ScalarValueOp::BitFieldMaskU32:
			return fmt::format("bfm_u32({}, {})", node.args[0], node.args[1]);
		default: return fmt::format("value{}", value);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
