#include "graphics/shader/recompiler/frontend/translate/Translator.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

bool Translator::TranslateStateOperation(const IR::Instruction& inst) {
	const auto zero = IR::U32(IR::Value(0u));
	switch (inst.op) {
		case IR::Opcode::SaveexecB32:
		case IR::Opcode::SaveexecB64: {
			const auto old_low  = ir.GetExecLo();
			const auto old_high = ir.GetExecHi();
			const auto src_low  = ReadRawU32(inst.src[0]);
			const auto src_high = inst.op == IR::Opcode::SaveexecB64
			                          ? ReadRawU32(OffsetOperand(inst.src[0], 1))
			                          : IR::U32(IR::Value(0u));
			const auto apply    = [&](IR::U32 old_value, IR::U32 src_value) {
				switch (inst.saveexec_mode) {
					case IR::SaveexecMode::And: return ir.BitwiseAnd(old_value, src_value);
					case IR::SaveexecMode::Andn1:
						return ir.BitwiseAnd(old_value, ir.BitwiseNot(src_value));
					case IR::SaveexecMode::Orn2:
						return ir.BitwiseOr(ir.BitwiseNot(old_value), src_value);
				}
				return old_value;
			};
			const auto result_low = apply(old_low, src_low);
			const auto result_high =
			    inst.op == IR::Opcode::SaveexecB64 ? apply(old_high, src_high) : old_high;
			if (inst.op == IR::Opcode::SaveexecB64) {
				WriteOperand(inst.dst, ir.PackUint2x32(old_low, old_high));
			} else {
				WriteOperand(inst.dst, old_low);
			}
			ir.SetExecLo(result_low);
			ir.SetExecHi(result_high);
			ir.SetExec(ThreadBit(result_low, result_high));
			const auto nonzero = inst.op == IR::Opcode::SaveexecB64
			                         ? ir.BitwiseOr(result_low, result_high)
			                         : result_low;
			ir.SetScc(ir.INotEqual(nonzero, zero));
			return true;
		}
		case IR::Opcode::ScalarAddCarryU32:
		case IR::Opcode::IAddCarryU32: {
			const auto lhs      = ReadU32(inst.src[0]);
			const auto rhs      = ReadU32(inst.src[1]);
			const auto carry_in = ConditionBit(inst.src[2]);
			const auto add0     = ir.Emit(IR::ValueOpcode::IAddCarry32, {lhs, rhs});
			const auto partial  = ir.CompositeExtract(add0, 0);
			const auto carry0   = ir.CompositeExtract(add0, 1);
			const auto add1     = ir.Emit(IR::ValueOpcode::IAddCarry32, {partial, carry_in});
			const auto result   = ir.CompositeExtract(add1, 0);
			const auto carry1   = ir.CompositeExtract(add1, 1);
			const auto carry = ir.INotEqual(ir.BitwiseOr(carry0, carry1), IR::U32(IR::Value(0u)));
			WriteOperand(inst.dst, result);
			if (inst.op == IR::Opcode::ScalarAddCarryU32) {
				ir.SetScc(carry);
			} else {
				WriteMask(inst.dst2, ir.LogicalAnd(ir.GetExec(), carry));
			}
			return true;
		}
		case IR::Opcode::ScalarSubBorrowU32:
		case IR::Opcode::ISubBorrowU32: {
			const auto lhs    = ReadU32(inst.src[0]);
			const auto rhs    = ReadU32(inst.src[1]);
			const auto result = ir.ISub(lhs, rhs);
			const auto borrow = ir.UGreaterThan(rhs, lhs);
			WriteOperand(inst.dst, result);
			if (inst.op == IR::Opcode::ScalarSubBorrowU32) {
				ir.SetScc(borrow);
			} else {
				WriteMask(inst.dst2, ir.LogicalAnd(ir.GetExec(), borrow));
			}
			return true;
		}
		case IR::Opcode::ScalarSubBorrowCarryU32:
		case IR::Opcode::ISubBorrowCarryU32: {
			const auto lhs       = ReadU32(inst.src[0]);
			const auto rhs       = ReadU32(inst.src[1]);
			const auto borrow_in = ConditionBit(inst.src[2]);
			const auto partial   = ir.ISub(lhs, rhs);
			const auto result    = ir.ISub(partial, borrow_in);
			const auto borrow0   = ir.UGreaterThan(rhs, lhs);
			const auto borrow1   = ir.UGreaterThan(borrow_in, partial);
			WriteOperand(inst.dst, result);
			const auto borrow = ir.LogicalOr(borrow0, borrow1);
			if (inst.op == IR::Opcode::ScalarSubBorrowCarryU32) {
				ir.SetScc(borrow);
			} else {
				WriteMask(inst.dst2, ir.LogicalAnd(ir.GetExec(), borrow));
			}
			return true;
		}
		case IR::Opcode::ScalarSignedAddOverflowI32:
		case IR::Opcode::ScalarSignedSubOverflowI32: {
			const auto lhs      = ReadU32(inst.src[0]);
			const auto rhs      = ReadU32(inst.src[1]);
			const bool subtract = inst.op == IR::Opcode::ScalarSignedSubOverflowI32;
			const auto result   = subtract ? ir.ISub(lhs, rhs) : ir.IAdd(lhs, rhs);
			const auto shift    = IR::U32(IR::Value(31u));
			const auto lhs_sign = ir.ShiftRightLogical(lhs, shift);
			const auto rhs_sign = ir.ShiftRightLogical(rhs, shift);
			const auto out_sign = ir.ShiftRightLogical(result, shift);
			const auto inputs =
			    subtract ? ir.INotEqual(lhs_sign, rhs_sign) : ir.IEqual(lhs_sign, rhs_sign);
			const auto changed = ir.INotEqual(lhs_sign, out_sign);
			WriteOperand(inst.dst, result);
			ir.SetScc(ir.LogicalAnd(inputs, changed));
			return true;
		}
		case IR::Opcode::ScalarShiftLeftAddCarryU32: {
			const auto lhs           = ReadU32(inst.src[0]);
			const auto shift         = ReadU32(inst.src[1]);
			const auto rhs           = ReadU32(inst.src[2]);
			const auto shifted       = ir.ShiftLeftLogical(lhs, shift);
			const auto result        = ir.IAdd(shifted, rhs);
			const auto add_carry     = ir.ULessThan(result, shifted);
			const auto inverse_shift = ir.ISub(IR::U32(IR::Value(32u)), shift);
			const auto shifted_out   = ir.ShiftRightLogical(lhs, inverse_shift);
			const auto shift_carry   = ir.INotEqual(shifted_out, zero);
			WriteOperand(inst.dst, result);
			ir.SetScc(ir.LogicalOr(add_carry, shift_carry));
			return true;
		}
		default: return false;
	}
}

bool Translator::TranslateControlOperation(const IR::Instruction& inst) {
	switch (inst.op) {
		case IR::Opcode::ControlNop: ir.Emit(IR::ValueOpcode::ControlNop); return true;
		case IR::Opcode::Waitcnt: ir.Emit(IR::ValueOpcode::Waitcnt); return true;
		case IR::Opcode::Barrier: ir.Emit(IR::ValueOpcode::Barrier); return true;
		case IR::Opcode::Sendmsg: ir.Emit(IR::ValueOpcode::Sendmsg); return true;
		case IR::Opcode::TtraceData: ir.Emit(IR::ValueOpcode::TtraceData); return true;
		case IR::Opcode::InstPrefetch: ir.Emit(IR::ValueOpcode::InstPrefetch); return true;
		default: return false;
	}
}

bool Translator::TranslateMove(const IR::Instruction& inst) {
	switch (inst.op) {
		case IR::Opcode::MoveU32:
			if (inst.src[0].kind == IR::OperandKind::Register &&
			    (inst.src[0].reg.file == IR::RegisterFile::Exec ||
			     inst.src[0].reg.file == IR::RegisterFile::Vcc) &&
			    inst.dst.kind == IR::OperandKind::Register &&
			    (inst.dst.reg.file == IR::RegisterFile::Exec ||
			     inst.dst.reg.file == IR::RegisterFile::Vcc)) {
				WriteMask(inst.dst, ReadMask(inst.src[0]));
			} else {
				WriteOperand(inst.dst, ReadOperand(inst.src[0], IR::Type::U32));
			}
			return true;
		case IR::Opcode::MoveF32Bits:
			WriteOperand(inst.dst, ReadOperand(inst.src[0], IR::Type::F32));
			return true;
		case IR::Opcode::MoveU64: {
			const bool scalar_pair = inst.dst.kind == IR::OperandKind::Register &&
			                         inst.src[0].kind == IR::OperandKind::Register &&
			                         inst.dst.reg.file == IR::RegisterFile::Scalar &&
			                         inst.src[0].reg.file == IR::RegisterFile::Scalar;
			WriteOperand(inst.dst, ReadOperand(inst.src[0], IR::Type::U64));
			if (scalar_pair) {
				ir.SetThreadBitScalarReg(
				    static_cast<IR::ScalarReg>(inst.dst.reg.index),
				    ir.GetThreadBitScalarReg(static_cast<IR::ScalarReg>(inst.src[0].reg.index)));
			}
			return true;
		}
		case IR::Opcode::WqmB64: {
			if (!current_per_invocation_masks) {
				const auto result = IR::U64(ir.Emit(
				    IR::ValueOpcode::WqmU64, {ReadOperand(inst.src[0], IR::Type::U64)}));
				WriteOperand(inst.dst, result);
				ir.SetScc(IR::U1(
				    ir.Emit(IR::ValueOpcode::INotEqual64, {result, IR::Value(uint64_t {0})})));
				return true;
			}
			const auto result = IR::U1(ir.Emit(IR::ValueOpcode::WqmMask, {ReadMask(inst.src[0])}));
			WriteMask64(inst.dst, result);
			ir.SetScc(result);
			return true;
		}
		default: return false;
	}
}

bool Translator::TranslateLaneOperation(const IR::Instruction& inst) {
	const auto lane_mask = IR::U32(IR::Value(current_wave_size == 32u ? 31u : 63u));
	switch (inst.op) {
		case IR::Opcode::MoveRelSourceU32: {
			EXIT_IF(inst.src[0].kind != IR::OperandKind::Register ||
			        inst.src[0].reg.file != IR::RegisterFile::Vector);
			const auto base     = inst.src[0].reg.index;
			const auto m0       = ir.BitwiseAnd(ReadU32(inst.src[1]), IR::U32(IR::Value(0xffu)));
			auto       selected = ir.GetVectorReg(static_cast<IR::VectorReg>(base));
			for (uint32_t index = base + 1u; index < current_vector_limit; index++) {
				const auto match = ir.IEqual(m0, IR::U32(IR::Value(index - base)));
				selected =
				    ir.Select(match, ir.GetVectorReg(static_cast<IR::VectorReg>(index)), selected);
			}
			WriteOperand(inst.dst, selected);
			return true;
		}
		case IR::Opcode::MoveRelDestU32: {
			EXIT_IF(inst.dst.kind != IR::OperandKind::Register ||
			        inst.dst.reg.file != IR::RegisterFile::Vector);
			const auto base  = inst.dst.reg.index;
			const auto value = ReadU32(inst.src[0]);
			const auto m0    = ir.BitwiseAnd(ReadU32(inst.src[1]), IR::U32(IR::Value(0xffu)));
			for (uint32_t index = base; index < current_vector_limit; index++) {
				const auto reg   = static_cast<IR::VectorReg>(index);
				const auto match = ir.IEqual(m0, IR::U32(IR::Value(index - base)));
				const auto write = ir.LogicalAnd(ir.GetExec(), match);
				ir.SetVectorReg(reg, ir.Select(write, value, ir.GetVectorReg(reg)));
			}
			return true;
		}
		case IR::Opcode::ReadFirstLaneU32: {
			const auto result =
			    ir.Emit(IR::ValueOpcode::ReadFirstLane, {ReadU32(inst.src[0]), ir.GetExec()});
			WriteOperand(inst.dst, result);
			return true;
		}
		case IR::Opcode::ReadLaneU32: {
			const auto lane = ir.BitwiseAnd(ReadU32(inst.src[1]), lane_mask);
			WriteOperand(inst.dst,
			             ir.Emit(IR::ValueOpcode::ReadLane, {ReadU32(inst.src[0]), lane}));
			return true;
		}
		case IR::Opcode::WriteLaneU32: {
			EXIT_IF(inst.dst.kind != IR::OperandKind::Register ||
			        inst.dst.reg.file != IR::RegisterFile::Vector);
			const auto reg    = static_cast<IR::VectorReg>(inst.dst.reg.index);
			const auto lane   = ir.BitwiseAnd(ReadU32(inst.src[1]), lane_mask);
			const auto result = IR::U32(ir.Emit(
			    IR::ValueOpcode::WriteLane, {ir.GetVectorReg(reg), ReadU32(inst.src[0]), lane}));
			// V_WRITELANE_B32 ignores EXEC; bypass the ordinary VGPR destination path.
			ir.SetVectorReg(reg, result);
			return true;
		}
		case IR::Opcode::Permlane16B32:
		case IR::Opcode::Permlanex16B32: {
			const IR::PermlaneFlags flags {
			    .x16            = inst.op == IR::Opcode::Permlanex16B32,
			    .fetch_inactive = inst.dst.op_sel,
			    .bound_control  = inst.dst.op_sel_hi,
			};
			const auto result = ir.Emit(
			    IR::ValueOpcode::Permlane16U32,
			    {ReadU32(inst.src[0]), ReadU32(inst.src[1]), ReadU32(inst.src[2]), ir.GetExec()},
			    flags);
			auto dst      = inst.dst;
			dst.op_sel    = false;
			dst.op_sel_hi = false;
			WriteOperand(dst, result);
			return true;
		}
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
