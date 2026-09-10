#include "common/assert.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {
namespace {

bool IsExecOrVcc(const Decoder::Operand& operand) {
	switch (operand.kind) {
		case Decoder::OperandKind::ExecLo:
		case Decoder::OperandKind::ExecHi:
		case Decoder::OperandKind::VccLo:
		case Decoder::OperandKind::VccHi: return true;
		default: return false;
	}
}

Decoder::Operand ConditionOperand(Decoder::OperandKind kind) {
	Decoder::Operand operand;
	operand.kind = kind;
	return operand;
}

} // namespace

void Translator::S_SUBVECTOR_LOOP(const Decoder::Instruction& inst, bool begin) {
	const auto zero  = IR::U32(IR::Value(0u));
	const auto lo    = ir.GetExecLo();
	const auto hi    = ir.GetExecHi();
	const auto saved = ReadU32(inst.dst);
	if (begin) {
		const auto low_active = ir.INotEqual(lo, zero);
		instruction_branch_condition = ir.IEqual(ir.BitwiseOr(lo, hi), zero);
		WriteRawU32(inst.dst, ir.Select(instruction_branch_condition, saved,
		                               ir.Select(low_active, hi, lo)));
		// Keep the ISA assignment order: SDST may itself name an EXEC half.
		WriteRawU32(ConditionOperand(Decoder::OperandKind::ExecHi),
		            ir.Select(low_active, zero, ir.GetExecHi()));
	} else {
		const auto high_active = ir.INotEqual(hi, zero);
		instruction_branch_condition =
		    ir.LogicalAnd(ir.LogicalNot(high_active), ir.INotEqual(saved, zero));
		WriteRawU32(ConditionOperand(Decoder::OperandKind::ExecHi),
		            ir.Select(instruction_branch_condition, saved, hi));
		WriteRawU32(inst.dst,
		            ir.Select(instruction_branch_condition, lo, ReadU32(inst.dst)));
		WriteRawU32(ConditionOperand(Decoder::OperandKind::ExecLo),
		            ir.Select(high_active, saved,
		                      ir.Select(instruction_branch_condition, zero, ir.GetExecLo())));
	}
}

void Translator::S_SAVEEXEC(const Decoder::Instruction& inst, IR::ValueOpcode operation,
                            bool negate_exec, bool negate_source, bool write_64) {
	if (!write_64) {
		// Read the encoded scalar word and preserve EXEC_HI, including in wave32.
		const auto old = ir.GetExecLo();
		const auto src = ReadU32(inst.src0);
		const auto lhs = negate_exec ? ir.BitwiseNot(old) : old;
		const auto rhs = negate_source ? ir.BitwiseNot(src) : src;
		IR::U32 result;
		switch (operation) {
			case IR::ValueOpcode::LogicalAnd: result = ir.BitwiseAnd(lhs, rhs); break;
			case IR::ValueOpcode::LogicalOr: result = ir.BitwiseOr(lhs, rhs); break;
			default: EXIT("unsupported SAVEEXEC operation");
		}
		WriteRawU32(inst.dst, old);
		WriteRawU32(ConditionOperand(Decoder::OperandKind::ExecLo), result);
		ir.SetScc(ir.INotEqual(result, IR::U32(IR::Value(0u))));
		return;
	}
	const auto old    = ir.GetExec();
	const auto src    = ReadMask(inst.src0);
	const auto lhs    = negate_exec ? ir.LogicalNot(old) : old;
	const auto rhs    = negate_source ? ir.LogicalNot(src) : src;
	const auto result = IR::U1(ir.Emit(operation, {lhs, rhs}));
	WriteMask(inst.dst, old, true);
	const auto mask = BallotMask(result);
	ir.SetExec(result);
	ir.SetExecLo(mask[0]);
	ir.SetExecHi(mask[1]);
	ir.SetScc(ir.INotEqual(ir.BitwiseOr(mask[0], mask[1]), IR::U32(IR::Value(0u))));
}

void Translator::ADD_U32(const Decoder::Instruction& inst, bool vector, bool use_carry_in) {
	const auto zero     = IR::U32(IR::Value(0u));
	const auto lhs      = ReadU32(inst.src0);
	const auto rhs      = ReadU32(inst.src1);
	const auto carry_in = !use_carry_in ? zero
	                      : vector      ? ConditionBit(inst.src2)
	                                    : ConditionBit(ConditionOperand(Decoder::OperandKind::Scc));
	const auto add0     = ir.Emit(IR::ValueOpcode::IAddCarry32, {lhs, rhs});
	const auto partial  = ir.CompositeExtract(add0, 0);
	const auto carry0   = ir.CompositeExtract(add0, 1);
	const auto add1     = ir.Emit(IR::ValueOpcode::IAddCarry32, {partial, carry_in});
	const auto result   = ir.CompositeExtract(add1, 0);
	const auto carry1   = ir.CompositeExtract(add1, 1);
	const auto carry    = ir.INotEqual(ir.BitwiseOr(carry0, carry1), zero);
	WriteOperand(DestinationOperand(inst), result);
	if (vector) {
		WriteMask(inst.dst2, ir.LogicalAnd(ir.GetExec(), carry));
	} else {
		ir.SetScc(carry);
	}
}

void Translator::SUB_U32(const Decoder::Instruction& inst, bool vector, bool reverse) {
	const auto lhs    = ReadU32(reverse ? inst.src1 : inst.src0);
	const auto rhs    = ReadU32(reverse ? inst.src0 : inst.src1);
	const auto result = ir.ISub(lhs, rhs);
	const auto borrow = ir.UGreaterThan(rhs, lhs);
	WriteOperand(DestinationOperand(inst), result);
	if (vector) {
		WriteMask(inst.dst2, ir.LogicalAnd(ir.GetExec(), borrow));
	} else {
		ir.SetScc(borrow);
	}
}

void Translator::SUBB_U32(const Decoder::Instruction& inst, bool vector, bool reverse) {
	const auto lhs       = ReadU32(reverse ? inst.src1 : inst.src0);
	const auto rhs       = ReadU32(reverse ? inst.src0 : inst.src1);
	const auto borrow_in = vector ? ConditionBit(inst.src2)
	                              : ConditionBit(ConditionOperand(Decoder::OperandKind::Scc));
	const auto partial   = ir.ISub(lhs, rhs);
	const auto result    = ir.ISub(partial, borrow_in);
	const auto borrow0   = ir.UGreaterThan(rhs, lhs);
	const auto borrow1   = ir.UGreaterThan(borrow_in, partial);
	const auto borrow    = ir.LogicalOr(borrow0, borrow1);
	WriteOperand(DestinationOperand(inst), result);
	if (vector) {
		WriteMask(inst.dst2, ir.LogicalAnd(ir.GetExec(), borrow));
	} else {
		ir.SetScc(borrow);
	}
}

void Translator::S_ABSDIFF_I32(const Decoder::Instruction& inst) {
	const auto difference = ir.ISub(ReadU32(inst.src0), ReadU32(inst.src1));
	const auto result     = IR::U32(ir.Emit(IR::ValueOpcode::IAbs32, {difference}));
	WriteOperand(DestinationOperand(inst), result);
	ir.SetScc(ir.INotEqual(result, IR::U32(IR::Value(0u))));
}

void Translator::S_ADD_SUB_I32(const Decoder::Instruction& inst, bool subtract) {
	const auto lhs      = ReadU32(inst.src0);
	const auto rhs      = ReadU32(inst.src1);
	const auto result   = subtract ? ir.ISub(lhs, rhs) : ir.IAdd(lhs, rhs);
	const auto shift    = IR::U32(IR::Value(31u));
	const auto lhs_sign = ir.ShiftRightLogical(lhs, shift);
	const auto rhs_sign = ir.ShiftRightLogical(rhs, shift);
	const auto out_sign = ir.ShiftRightLogical(result, shift);
	const auto inputs = subtract ? ir.INotEqual(lhs_sign, rhs_sign) : ir.IEqual(lhs_sign, rhs_sign);
	const auto changed = ir.INotEqual(lhs_sign, out_sign);
	WriteOperand(DestinationOperand(inst), result);
	ir.SetScc(ir.LogicalAnd(inputs, changed));
}

void Translator::S_LSHL_ADD_U32(const Decoder::Instruction& inst, uint32_t shift_amount) {
	const auto lhs           = ReadU32(inst.src0);
	const auto shift         = IR::U32(IR::Value(shift_amount));
	const auto rhs           = ReadU32(inst.src1);
	const auto shifted       = ir.ShiftLeftLogical(lhs, shift);
	const auto result        = ir.IAdd(shifted, rhs);
	const auto add_carry     = ir.ULessThan(result, shifted);
	const auto inverse_shift = ir.ISub(IR::U32(IR::Value(32u)), shift);
	const auto shifted_out   = ir.ShiftRightLogical(lhs, inverse_shift);
	const auto shift_carry   = ir.INotEqual(shifted_out, IR::U32(IR::Value(0u)));
	WriteOperand(DestinationOperand(inst), result);
	ir.SetScc(ir.LogicalOr(add_carry, shift_carry));
}

void Translator::ScalarMinMax32(const Decoder::Instruction& inst, IR::ValueOpcode value_opcode,
                                IR::ValueOpcode compare_opcode) {
	const auto lhs = ReadU32(inst.src0);
	const auto rhs = ReadU32(inst.src1);
	WriteOperand(DestinationOperand(inst), ir.Emit(value_opcode, {lhs, rhs}));
	ir.SetScc(IR::U1(ir.Emit(compare_opcode, {lhs, rhs})));
}

void Translator::EmitControlNop() {
	ir.Emit(IR::ValueOpcode::ControlNop);
}

void Translator::EmitWaitcnt() {
	ir.Emit(IR::ValueOpcode::Waitcnt);
}

void Translator::S_BARRIER() {
	ir.Emit(IR::ValueOpcode::Barrier);
}

void Translator::S_SENDMSG(const Decoder::Instruction& inst) {
	if (program.stage == ShaderType::Mesh) {
		EXIT_NOT_IMPLEMENTED(inst.src0.value != 9u); // MSG_GS_ALLOC_REQ
		ir.Emit(IR::ValueOpcode::MeshAllocate, {ir.GetM0()});
	} else {
		ir.Emit(IR::ValueOpcode::Sendmsg);
	}
}

void Translator::S_TTRACEDATA() {
	ir.Emit(IR::ValueOpcode::TtraceData);
}

void Translator::S_INST_PREFETCH() {
	ir.Emit(IR::ValueOpcode::InstPrefetch);
}

void Translator::S_GETPC_B64(const Decoder::Instruction& inst) {
	const auto base    = IR::U64(ir.Emit(IR::ValueOpcode::GetShaderBase));
	const auto address = IR::U64(
	    ir.Emit(IR::ValueOpcode::IAdd64, {base, IR::Value(static_cast<uint64_t>(inst.pc) + 4u)}));
	if (inst.dst.kind == Decoder::OperandKind::Null) {
		return;
	}
	auto high = PlainOperand(inst.dst);
	switch (inst.dst.kind) {
		case Decoder::OperandKind::Sgpr:
			if (inst.dst.reg < 105u) {
				high.reg++;
			} else if (inst.dst.reg == 105u) {
				high.kind = Decoder::OperandKind::VccLo;
				high.reg  = 0u;
			} else {
				EXIT("S_GETPC_B64 destination does not name a valid scalar pair at pc 0x%08x",
				     inst.pc);
			}
			break;
		case Decoder::OperandKind::VccLo: high.kind = Decoder::OperandKind::VccHi; break;
		case Decoder::OperandKind::M0: high.kind = Decoder::OperandKind::Null; break;
		case Decoder::OperandKind::ExecLo: high.kind = Decoder::OperandKind::ExecHi; break;
		default:
			EXIT("S_GETPC_B64 destination does not name a valid scalar pair at pc 0x%08x", inst.pc);
	}
	const auto words = ExtractU64(address);
	WriteOperand(inst.dst, words[0]);
	WriteOperand(high, words[1]);
}

void Translator::S_CSELECT_B32(const Decoder::Instruction& inst) {
	const auto result = ir.Select(ir.GetScc(), ReadU32(inst.src0), ReadU32(inst.src1));
	WriteOperand(DestinationOperand(inst), result);
}

void Translator::ScalarSelect64(const Decoder::Instruction& inst,
                                 const Decoder::Operand& false_source) {
	const auto condition     = ir.GetScc();
	const auto lhs           = ReadU32Pair(inst.src0);
	const auto rhs           = ReadU32Pair(false_source);
	const auto selected_mask = IR::U1(
	    ir.Emit(IR::ValueOpcode::SelectU1, {condition, ReadMask(inst.src0), ReadMask(false_source)}));
	const auto selected_mask_valid =
	    IR::U1(ir.Emit(IR::ValueOpcode::SelectU1,
	                   {condition, ReadMaskValid(inst.src0), ReadMaskValid(false_source)}));
	if (IsExecOrVcc(inst.dst)) {
		WriteMask(inst.dst, selected_mask, true);
		return;
	}
	WriteU32Pair(inst.dst,
	             {ir.Select(condition, lhs[0], rhs[0]), ir.Select(condition, lhs[1], rhs[1])});
	if (inst.dst.kind == Decoder::OperandKind::Sgpr) {
		const auto dst = static_cast<IR::ScalarReg>(inst.dst.reg);
		ir.SetThreadBitScalarReg(dst, selected_mask);
		ir.SetScalarMaskTag(dst, selected_mask_valid);
	}
}

void Translator::MOV_B32(const Decoder::Instruction& inst, bool apply_float_modifiers) {
	if (apply_float_modifiers && (inst.src0.negate || inst.src0.absolute)) {
		WriteOperand(DestinationOperand(inst), ReadOperand(inst.src0, IR::Type::F32));
	} else {
		WriteOperand(DestinationOperand(inst), ReadOperand(inst.src0, IR::Type::U32));
	}
}

void Translator::S_MOV_B64(const Decoder::Instruction& inst) {
	const bool mask_source = inst.src0.kind == Decoder::OperandKind::Sgpr ||
	                         inst.src0.kind == Decoder::OperandKind::ExecLo ||
	                         inst.src0.kind == Decoder::OperandKind::VccLo;
	IR::U1 source_mask;
	IR::U1 source_mask_valid;
	if (mask_source) {
		// A full VCC copy carries its predicate even in wave32; ReadMask also handles
		// individual 32-bit VCC halves, which cannot preserve that provenance.
		source_mask = inst.src0.kind == Decoder::OperandKind::VccLo ? ir.GetVcc()
		                                                          : ReadMask(inst.src0);
		source_mask_valid = ReadMaskValid(inst.src0);
	}
	// Preserve all 64 scalar bits independently of the per-thread predicate.
	WriteU32Pair(inst.dst, ReadU32Pair(inst.src0));
	if (mask_source) {
		switch (inst.dst.kind) {
			case Decoder::OperandKind::ExecLo: ir.SetExec(source_mask); break;
			case Decoder::OperandKind::VccLo: ir.SetVcc(source_mask); break;
			case Decoder::OperandKind::Sgpr:
				ir.SetThreadBitScalarReg(static_cast<IR::ScalarReg>(inst.dst.reg), source_mask);
				ir.SetScalarMaskTag(static_cast<IR::ScalarReg>(inst.dst.reg), source_mask_valid);
				break;
			default: break;
		}
	}
}

void Translator::S_WQM(const Decoder::Instruction& inst, bool wide) {
	const auto source = wide ? ReadU64(inst.src0)
	                         : ir.ConstructU64(ReadU32(inst.src0), IR::U32(IR::Value(0u)));
	const auto result = IR::U64(ir.Emit(IR::ValueOpcode::WqmU64, {source}));
	if (!wide) {
		const auto low = ir.CompositeExtract(result, 0);
		// A 32-bit write preserves the other EXEC/VCC half and invalidates any
		// overlapping SGPR-pair mask provenance through the ordinary scalar path.
		WriteRawU32(inst.dst, low);
		ir.SetScc(ir.INotEqual(low, IR::U32(IR::Value(0u))));
		return;
	}
	const auto mask_valid = ReadMaskValid(inst.src0);
	WriteOperand(DestinationOperand(inst), result);
	if (inst.dst.kind == Decoder::OperandKind::Sgpr) {
		const auto dst = static_cast<IR::ScalarReg>(inst.dst.reg);
		ir.SetThreadBitScalarReg(dst, ThreadBit(ExtractU64(result)));
		ir.SetScalarMaskTag(dst, mask_valid);
	}
	ir.SetScc(IR::U1(ir.Emit(IR::ValueOpcode::INotEqual64, {result, IR::Value(uint64_t {0})})));
}

void Translator::V_MOVRELS_B32(const Decoder::Instruction& inst) {
	if (inst.dst.kind != Decoder::OperandKind::Vgpr ||
	    inst.src0.kind != Decoder::OperandKind::Vgpr) {
		EXIT("V_MOVRELS_B32 requires VGPR source and destination at pc 0x%08x", inst.pc);
	}
	if (inst.dst.sdwa_sel != 6u || inst.dst.omod != 0u || inst.dst.clamp ||
	    inst.src0.sdwa_sel != 6u || inst.src0.sdwa_sext || inst.src0.negate || inst.src0.absolute ||
	    inst.src0.dpp) {
		EXIT("V_MOVRELS_B32 modifiers are not implemented at pc 0x%08x", inst.pc);
	}
	const auto base     = inst.src0.reg;
	const auto m0       = ir.BitwiseAnd(ReadU32(ConditionOperand(Decoder::OperandKind::M0)),
	                                    IR::U32(IR::Value(0xffu)));
	auto       selected = ir.GetVectorReg(static_cast<IR::VectorReg>(base));
	for (uint32_t index = base + 1u; index < current_vector_limit; index++) {
		const auto match = ir.IEqual(m0, IR::U32(IR::Value(index - base)));
		selected = ir.Select(match, ir.GetVectorReg(static_cast<IR::VectorReg>(index)), selected);
	}
	WriteOperand(DestinationOperand(inst), selected);
}

void Translator::V_MOVRELD_B32(const Decoder::Instruction& inst) {
	if (inst.dst.kind != Decoder::OperandKind::Vgpr) {
		EXIT("V_MOVRELD_B32 requires VGPR destination at pc 0x%08x", inst.pc);
	}
	if (inst.dst.sdwa_sel != 6u || inst.dst.omod != 0u || inst.dst.clamp ||
	    inst.src0.sdwa_sel != 6u || inst.src0.sdwa_sext || inst.src0.negate || inst.src0.absolute ||
	    inst.src0.dpp) {
		EXIT("V_MOVRELD_B32 modifiers are not implemented at pc 0x%08x", inst.pc);
	}
	const auto base  = inst.dst.reg;
	const auto value = ReadU32(inst.src0);
	const auto m0    = ir.BitwiseAnd(ReadU32(ConditionOperand(Decoder::OperandKind::M0)),
	                                 IR::U32(IR::Value(0xffu)));
	for (uint32_t index = base; index < current_vector_limit; index++) {
		const auto reg   = static_cast<IR::VectorReg>(index);
		const auto match = ir.IEqual(m0, IR::U32(IR::Value(index - base)));
		const auto write = ir.LogicalAnd(ir.GetExec(), match);
		ir.SetVectorReg(reg, ir.Select(write, value, ir.GetVectorReg(reg)));
	}
}

void Translator::V_READFIRSTLANE_B32(const Decoder::Instruction& inst) {
	const auto result = ir.Emit(IR::ValueOpcode::ReadFirstLane, {ReadU32(inst.src0), ir.GetExec()});
	WriteOperand(DestinationOperand(inst), result);
}

void Translator::V_READLANE_B32(const Decoder::Instruction& inst) {
	const auto lane_mask = IR::U32(IR::Value(program.wave_size == 32u ? 31u : 63u));
	const auto lane      = ir.BitwiseAnd(ReadU32(inst.src1), lane_mask);
	WriteOperand(DestinationOperand(inst),
	             ir.Emit(IR::ValueOpcode::ReadLane, {ReadU32(inst.src0), lane}));
}

void Translator::V_WRITELANE_B32(const Decoder::Instruction& inst) {
	EXIT_IF(inst.dst.kind != Decoder::OperandKind::Vgpr);
	const auto lane_mask = IR::U32(IR::Value(program.wave_size == 32u ? 31u : 63u));
	const auto reg       = static_cast<IR::VectorReg>(inst.dst.reg);
	const auto lane      = ir.BitwiseAnd(ReadU32(inst.src1), lane_mask);
	const auto result    = IR::U32(
	    ir.Emit(IR::ValueOpcode::WriteLane, {ir.GetVectorReg(reg), ReadU32(inst.src0), lane}));
	ir.SetVectorReg(reg, result);
}

void Translator::V_PERMLANE16_B32(const Decoder::Instruction& inst, bool x16) {
	const IR::PermlaneFlags flags {
	    .x16            = x16,
	    .fetch_inactive = inst.dst.op_sel,
	    .bound_control  = inst.dst.op_sel_hi,
	};
	const auto result =
	    ir.Emit(IR::ValueOpcode::Permlane16U32,
	            {ReadU32(inst.src0), ReadU32(inst.src1), ReadU32(inst.src2), ir.GetExec()}, flags);
	auto dst      = DestinationOperand(inst);
	dst.op_sel    = false;
	dst.op_sel_hi = false;
	WriteOperand(dst, result);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
