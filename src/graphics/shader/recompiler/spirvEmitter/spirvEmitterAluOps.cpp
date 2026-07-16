#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

void EmitPerInvocationMask(EmitterState* state, const IR::Operand& dst, uint32_t value_bool) {
	const auto value = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->uint_type, value, value_bool,
	                            ConstantU32(state, 1), ConstantU32(state, 0)});
	EmitStoreU32(state, dst, value);
	EmitStoreU32(state, OffsetRegisterOperand(dst, 1), ConstantU32(state, 0));
}

uint32_t EmitLogicalBinary(EmitterState* state, uint32_t opcode, uint32_t lhs, uint32_t rhs) {
	const auto logical_opcode = opcode == OpBitwiseAnd  ? OpLogicalAnd
	                            : opcode == OpBitwiseOr ? OpLogicalOr
	                                                    : OpLogicalNotEqual;
	const auto value          = state->builder.AllocateId();
	state->builder.AddFunction({logical_opcode, state->bool_type, value, lhs, rhs});
	return value;
}

void EmitAbsI32(EmitterState* state, const IR::Instruction& inst) {
	const auto src  = EmitValueLoad(state, inst.src[0]);
	const auto neg  = state->builder.AllocateId();
	const auto cond = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction({OpSNegate, state->uint_type, neg, src});
	state->builder.AddFunction({OpSLessThan, state->bool_type, cond, src, ConstantU32(state, 0)});
	state->builder.AddFunction({OpSelect, state->uint_type, ret, cond, neg, src});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitUnaryU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto src = EmitValueLoad(state, inst.src[0]);
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, ret, src});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitUnaryU64(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	if (state->per_invocation_masks) {
		const auto ret = state->builder.AllocateId();
		state->builder.AddFunction({OpLogicalNot, state->bool_type, ret,
		                            EmitLaneMaskOperandActiveBool(state, inst.src[0])});
		EmitPerInvocationMask(state, inst.dst, ret);
		return;
	}
	const auto low      = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto high     = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto ret_low  = state->builder.AllocateId();
	const auto ret_high = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, ret_low, low});
	state->builder.AddFunction({opcode, state->uint_type, ret_high, high});
	EmitStoreU32(state, inst.dst, ret_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ret_high);
}

void EmitFindLsbU32(EmitterState* state, const IR::Instruction& inst) {
	const auto src = EmitValueLoad(state, inst.src[0]);
	const auto i32 = state->builder.AllocateId();
	const auto u32 = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->int_type, i32, state->glsl_std450, GlslFindILsb, src});
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, i32});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitFindMsbFromHighU32(EmitterState* state, const IR::Instruction& inst) {
	const auto src      = EmitValueLoad(state, inst.src[0]);
	const auto i32      = state->builder.AllocateId();
	const auto msb      = state->builder.AllocateId();
	const auto pos      = state->builder.AllocateId();
	const auto non_zero = state->builder.AllocateId();
	const auto ret      = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->int_type, i32, state->glsl_std450, GlslFindUMsb, src});
	state->builder.AddFunction({OpBitcast, state->uint_type, msb, i32});
	state->builder.AddFunction({OpISub, state->uint_type, pos, ConstantU32(state, 31), msb});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, non_zero, src, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, ret, non_zero, pos, ConstantU32(state, 0xffffffffu)});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitFindMsbFromHighU64(EmitterState* state, const IR::Instruction& inst) {
	const auto low           = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto high          = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto high_i32      = state->builder.AllocateId();
	const auto high_msb      = state->builder.AllocateId();
	const auto high_pos      = state->builder.AllocateId();
	const auto high_non_zero = state->builder.AllocateId();
	const auto low_i32       = state->builder.AllocateId();
	const auto low_msb       = state->builder.AllocateId();
	const auto low_pos       = state->builder.AllocateId();
	const auto low_total     = state->builder.AllocateId();
	const auto low_non_zero  = state->builder.AllocateId();
	const auto low_ret       = state->builder.AllocateId();
	const auto ret           = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->int_type, high_i32, state->glsl_std450, GlslFindUMsb, high});
	state->builder.AddFunction({OpBitcast, state->uint_type, high_msb, high_i32});
	state->builder.AddFunction(
	    {OpISub, state->uint_type, high_pos, ConstantU32(state, 31), high_msb});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, high_non_zero, high, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpExtInst, state->int_type, low_i32, state->glsl_std450, GlslFindUMsb, low});
	state->builder.AddFunction({OpBitcast, state->uint_type, low_msb, low_i32});
	state->builder.AddFunction(
	    {OpISub, state->uint_type, low_pos, ConstantU32(state, 31), low_msb});
	state->builder.AddFunction(
	    {OpIAdd, state->uint_type, low_total, low_pos, ConstantU32(state, 32)});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, low_non_zero, low, ConstantU32(state, 0)});
	state->builder.AddFunction({OpSelect, state->uint_type, low_ret, low_non_zero, low_total,
	                            ConstantU32(state, 0xffffffffu)});
	state->builder.AddFunction({OpSelect, state->uint_type, ret, high_non_zero, high_pos, low_ret});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBitCountU64(EmitterState* state, const IR::Instruction& inst) {
	const auto low        = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto high       = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto low_count  = state->builder.AllocateId();
	const auto high_count = state->builder.AllocateId();
	const auto ret        = state->builder.AllocateId();
	state->builder.AddFunction({OpBitCount, state->uint_type, low_count, low});
	state->builder.AddFunction({OpBitCount, state->uint_type, high_count, high});
	state->builder.AddFunction({OpIAdd, state->uint_type, ret, low_count, high_count});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBitCountU32(EmitterState* state, const IR::Instruction& inst) {
	const auto bits  = EmitValueLoad(state, inst.src[0]);
	const auto count = state->builder.AllocateId();
	state->builder.AddFunction({OpBitCount, state->uint_type, count, bits});
	EmitStoreU32(state, inst.dst, count);
}

void EmitBitCountAddU32(EmitterState* state, const IR::Instruction& inst) {
	const auto bits  = EmitValueLoad(state, inst.src[0]);
	const auto add   = EmitValueLoad(state, inst.src[1]);
	const auto count = state->builder.AllocateId();
	const auto ret   = state->builder.AllocateId();
	state->builder.AddFunction({OpBitCount, state->uint_type, count, bits});
	state->builder.AddFunction({OpIAdd, state->uint_type, ret, count, add});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBinaryU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs = EmitValueLoad(state, inst.src[0]);
	auto       rhs = EmitValueLoad(state, inst.src[1]);
	if (opcode == OpShiftLeftLogical || opcode == OpShiftRightLogical ||
	    opcode == OpShiftRightArithmetic) {
		rhs = EmitAndConstant(state, rhs, 31u);
	}
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, ret, lhs, rhs});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBinaryNotRhsU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs     = EmitValueLoad(state, inst.src[0]);
	const auto rhs     = EmitValueLoad(state, inst.src[1]);
	const auto not_rhs = state->builder.AllocateId();
	const auto ret     = state->builder.AllocateId();
	state->builder.AddFunction({OpNot, state->uint_type, not_rhs, rhs});
	state->builder.AddFunction({opcode, state->uint_type, ret, lhs, not_rhs});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBinaryThenNotU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs = EmitValueLoad(state, inst.src[0]);
	const auto rhs = EmitValueLoad(state, inst.src[1]);
	const auto tmp = state->builder.AllocateId();
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, tmp, lhs, rhs});
	state->builder.AddFunction({OpNot, state->uint_type, ret, tmp});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitMaskedBitCountU32(EmitterState* state, const IR::Instruction& inst, uint32_t exec_index) {
	const auto bits        = EmitValueLoad(state, inst.src[0]);
	const auto add         = EmitValueLoad(state, inst.src[1]);
	const auto thread_mask = EmitThreadMaskBelowPartU32(state, exec_index);
	const auto active_bits = state->builder.AllocateId();
	const auto count       = state->builder.AllocateId();
	const auto ret         = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, active_bits, bits, thread_mask});
	state->builder.AddFunction({OpBitCount, state->uint_type, count, active_bits});
	state->builder.AddFunction({OpIAdd, state->uint_type, ret, count, add});
	EmitStoreU32(state, inst.dst, ret);
}

uint32_t EmitMask5U32(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, ret, value, ConstantU32(state, 31)});
	return ret;
}

void EmitChainedBinaryU32(EmitterState* state, const IR::Instruction& inst, uint32_t first_op,
                          uint32_t second_op, bool mask_first_rhs, bool mask_second_rhs) {
	const auto src0     = EmitValueLoad(state, inst.src[0]);
	const auto src1_raw = EmitValueLoad(state, inst.src[1]);
	const auto src2_raw = EmitValueLoad(state, inst.src[2]);
	const auto src1     = mask_first_rhs ? EmitMask5U32(state, src1_raw) : src1_raw;
	const auto src2     = mask_second_rhs ? EmitMask5U32(state, src2_raw) : src2_raw;
	const auto tmp      = state->builder.AllocateId();
	const auto ret      = state->builder.AllocateId();
	state->builder.AddFunction({first_op, state->uint_type, tmp, src0, src1});
	state->builder.AddFunction({second_op, state->uint_type, ret, tmp, src2});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBinaryU64(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	if (state->per_invocation_masks) {
		const auto ret =
		    EmitLogicalBinary(state, opcode, EmitLaneMaskOperandActiveBool(state, inst.src[0]),
		                      EmitLaneMaskOperandActiveBool(state, inst.src[1]));
		EmitPerInvocationMask(state, inst.dst, ret);
		return;
	}
	const auto lhs_low  = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto lhs_high = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto rhs_low  = EmitSequentialValueLoad(state, inst.src[1], 0);
	const auto rhs_high = EmitSequentialValueLoad(state, inst.src[1], 1);
	const auto ret_low  = state->builder.AllocateId();
	const auto ret_high = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, ret_low, lhs_low, rhs_low});
	state->builder.AddFunction({opcode, state->uint_type, ret_high, lhs_high, rhs_high});
	EmitStoreU32(state, inst.dst, ret_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ret_high);
}

void EmitBinaryNotRhsU64(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	if (state->per_invocation_masks) {
		const auto not_rhs = state->builder.AllocateId();
		state->builder.AddFunction({OpLogicalNot, state->bool_type, not_rhs,
		                            EmitLaneMaskOperandActiveBool(state, inst.src[1])});
		const auto ret = EmitLogicalBinary(
		    state, opcode, EmitLaneMaskOperandActiveBool(state, inst.src[0]), not_rhs);
		EmitPerInvocationMask(state, inst.dst, ret);
		return;
	}
	const auto lhs_low  = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto lhs_high = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto rhs_low  = EmitSequentialValueLoad(state, inst.src[1], 0);
	const auto rhs_high = EmitSequentialValueLoad(state, inst.src[1], 1);
	const auto not_low  = state->builder.AllocateId();
	const auto not_high = state->builder.AllocateId();
	const auto ret_low  = state->builder.AllocateId();
	const auto ret_high = state->builder.AllocateId();
	state->builder.AddFunction({OpNot, state->uint_type, not_low, rhs_low});
	state->builder.AddFunction({OpNot, state->uint_type, not_high, rhs_high});
	state->builder.AddFunction({opcode, state->uint_type, ret_low, lhs_low, not_low});
	state->builder.AddFunction({opcode, state->uint_type, ret_high, lhs_high, not_high});
	EmitStoreU32(state, inst.dst, ret_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ret_high);
}

void EmitBinaryThenNotU64(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	if (state->per_invocation_masks) {
		const auto value =
		    EmitLogicalBinary(state, opcode, EmitLaneMaskOperandActiveBool(state, inst.src[0]),
		                      EmitLaneMaskOperandActiveBool(state, inst.src[1]));
		const auto ret = state->builder.AllocateId();
		state->builder.AddFunction({OpLogicalNot, state->bool_type, ret, value});
		EmitPerInvocationMask(state, inst.dst, ret);
		return;
	}
	const auto lhs_low  = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto lhs_high = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto rhs_low  = EmitSequentialValueLoad(state, inst.src[1], 0);
	const auto rhs_high = EmitSequentialValueLoad(state, inst.src[1], 1);
	const auto tmp_low  = state->builder.AllocateId();
	const auto tmp_high = state->builder.AllocateId();
	const auto ret_low  = state->builder.AllocateId();
	const auto ret_high = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, tmp_low, lhs_low, rhs_low});
	state->builder.AddFunction({opcode, state->uint_type, tmp_high, lhs_high, rhs_high});
	state->builder.AddFunction({OpNot, state->uint_type, ret_low, tmp_low});
	state->builder.AddFunction({OpNot, state->uint_type, ret_high, tmp_high});
	EmitStoreU32(state, inst.dst, ret_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ret_high);
}

void EmitSelectU64(EmitterState* state, const IR::Instruction& inst) {
	const auto cond       = EmitConditionBool(state, inst.src[0]);
	const auto true_low   = EmitSequentialValueLoad(state, inst.src[1], 0);
	const auto true_high  = EmitSequentialValueLoad(state, inst.src[1], 1);
	const auto false_low  = EmitSequentialValueLoad(state, inst.src[2], 0);
	const auto false_high = EmitSequentialValueLoad(state, inst.src[2], 1);
	const auto ret_low    = state->builder.AllocateId();
	const auto ret_high   = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->uint_type, ret_low, cond, true_low, false_low});
	state->builder.AddFunction({OpSelect, state->uint_type, ret_high, cond, true_high, false_high});
	EmitStoreU32(state, inst.dst, ret_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ret_high);
}

uint32_t EmitSelectValueU32(EmitterState* state, uint32_t cond, uint32_t true_value,
                            uint32_t false_value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->uint_type, ret, cond, true_value, false_value});
	return ret;
}

void EmitStoreSccBool(EmitterState* state, uint32_t cond) {
	const auto value =
	    EmitSelectValueU32(state, cond, ConstantU32(state, 1), ConstantU32(state, 0));
	EmitStoreU32(state, SccOperand(), value);
}

uint32_t EmitAndConstant(EmitterState* state, uint32_t value, uint32_t mask);

void EmitShiftLeftLogicalU64Values(EmitterState* state, uint32_t low, uint32_t high, uint32_t shift,
                                   uint32_t* out_low, uint32_t* out_high) {
	const auto amount           = EmitAndConstant(state, shift, 63u);
	const auto dword_shift      = EmitAndConstant(state, amount, 31u);
	const auto less32           = state->builder.AllocateId();
	const auto non_zero         = state->builder.AllocateId();
	const auto carry_count_base = state->builder.AllocateId();
	const auto carry_count      = state->builder.AllocateId();
	const auto low_part         = state->builder.AllocateId();
	const auto high_part        = state->builder.AllocateId();
	const auto carry_part       = state->builder.AllocateId();
	const auto carry            = state->builder.AllocateId();
	const auto high_less        = state->builder.AllocateId();
	const auto high_ge          = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpULessThan, state->bool_type, less32, amount, ConstantU32(state, 32)});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, non_zero, amount, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpISub, state->uint_type, carry_count_base, ConstantU32(state, 32), dword_shift});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, carry_count, carry_count_base, ConstantU32(state, 31)});
	state->builder.AddFunction({OpShiftLeftLogical, state->uint_type, low_part, low, dword_shift});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, high_part, high, dword_shift});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, carry_part, low, carry_count});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, carry, non_zero, carry_part, ConstantU32(state, 0)});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, high_less, high_part, carry});
	state->builder.AddFunction({OpShiftLeftLogical, state->uint_type, high_ge, low, dword_shift});
	*out_low  = EmitSelectValueU32(state, less32, low_part, ConstantU32(state, 0));
	*out_high = EmitSelectValueU32(state, less32, high_less, high_ge);
}

void EmitShiftLeftLogicalU64(EmitterState* state, const IR::Instruction& inst) {
	const auto low      = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto high     = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto shift    = EmitValueLoad(state, inst.src[1]);
	uint32_t   ret_low  = 0;
	uint32_t   ret_high = 0;
	EmitShiftLeftLogicalU64Values(state, low, high, shift, &ret_low, &ret_high);
	EmitStoreU32(state, inst.dst, ret_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ret_high);
}

void EmitShiftRightLogicalU64Values(EmitterState* state, uint32_t low, uint32_t high,
                                    uint32_t shift, uint32_t* out_low, uint32_t* out_high) {
	const auto amount           = EmitAndConstant(state, shift, 63u);
	const auto dword_shift      = EmitAndConstant(state, amount, 31u);
	const auto less32           = state->builder.AllocateId();
	const auto non_zero         = state->builder.AllocateId();
	const auto carry_count_base = state->builder.AllocateId();
	const auto carry_count      = state->builder.AllocateId();
	const auto low_part         = state->builder.AllocateId();
	const auto high_part        = state->builder.AllocateId();
	const auto carry_part       = state->builder.AllocateId();
	const auto carry            = state->builder.AllocateId();
	const auto low_less         = state->builder.AllocateId();
	const auto low_ge           = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpULessThan, state->bool_type, less32, amount, ConstantU32(state, 32)});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, non_zero, amount, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpISub, state->uint_type, carry_count_base, ConstantU32(state, 32), dword_shift});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, carry_count, carry_count_base, ConstantU32(state, 31)});
	state->builder.AddFunction({OpShiftRightLogical, state->uint_type, low_part, low, dword_shift});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, high_part, high, dword_shift});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, carry_part, high, carry_count});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, carry, non_zero, carry_part, ConstantU32(state, 0)});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, low_less, low_part, carry});
	state->builder.AddFunction({OpShiftRightLogical, state->uint_type, low_ge, high, dword_shift});
	*out_low  = EmitSelectValueU32(state, less32, low_less, low_ge);
	*out_high = EmitSelectValueU32(state, less32, high_part, ConstantU32(state, 0));
}

void EmitShiftRightLogicalU64(EmitterState* state, const IR::Instruction& inst) {
	const auto low      = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto high     = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto shift    = EmitValueLoad(state, inst.src[1]);
	uint32_t   ret_low  = 0;
	uint32_t   ret_high = 0;
	EmitShiftRightLogicalU64Values(state, low, high, shift, &ret_low, &ret_high);
	EmitStoreU32(state, inst.dst, ret_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ret_high);
}

void EmitAdd3U32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs = EmitValueLoad(state, inst.src[0]);
	const auto rhs = EmitValueLoad(state, inst.src[1]);
	const auto add = EmitValueLoad(state, inst.src[2]);
	const auto tmp = state->builder.AllocateId();
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpIAdd, state->uint_type, tmp, lhs, rhs});
	state->builder.AddFunction({OpIAdd, state->uint_type, ret, tmp, add});
	EmitStoreU32(state, inst.dst, ret);
}

uint32_t EmitAndConstant(EmitterState* state, uint32_t value, uint32_t mask) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, ret, value, ConstantU32(state, mask)});
	return ret;
}

void EmitShiftU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs = EmitValueLoad(state, inst.src[0]);
	const auto rhs = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 31u);
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, ret, lhs, rhs});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBitClearU32(EmitterState* state, const IR::Instruction& inst) {
	const auto value = EmitValueLoad(state, inst.src[0]);
	const auto bit   = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 31u);
	const auto mask  = state->builder.AllocateId();
	const auto inv   = state->builder.AllocateId();
	const auto ret   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, mask, ConstantU32(state, 1), bit});
	state->builder.AddFunction({OpNot, state->uint_type, inv, mask});
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, ret, value, inv});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBitSetU32(EmitterState* state, const IR::Instruction& inst) {
	const auto value = EmitValueLoad(state, inst.src[0]);
	const auto bit   = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 31u);
	const auto mask  = state->builder.AllocateId();
	const auto ret   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, mask, ConstantU32(state, 1), bit});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, ret, value, mask});
	EmitStoreU32(state, inst.dst, ret);
}

uint32_t EmitNegateU32(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpSNegate, state->uint_type, ret, value});
	return ret;
}

uint32_t EmitSignExtendLow16U32(EmitterState* state, uint32_t value) {
	const auto signed_value = state->builder.AllocateId();
	const auto extracted    = state->builder.AllocateId();
	const auto bits         = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, signed_value, value});
	state->builder.AddFunction({OpBitFieldSExtract, state->int_type, extracted, signed_value,
	                            ConstantU32(state, 0), ConstantU32(state, 16)});
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, extracted});
	return bits;
}

uint32_t EmitU16LaneBits(EmitterState* state, const IR::Operand& operand, bool high_lane,
                         bool sign_extend) {
	const auto raw  = EmitValueLoad(state, operand);
	const auto lane = high_lane ? (operand.op_sel_hi ? 1u : 0u) : (operand.op_sel ? 1u : 0u);
	auto       bits = lane != 0u ? EmitShiftRightConstant(state, raw, 16) : raw;
	bits            = EmitAndConstant(state, bits, 0xffffu);
	if (high_lane ? operand.negate_hi : operand.negate) {
		bits = EmitAndConstant(state, EmitNegateU32(state, bits), 0xffffu);
	}
	return sign_extend ? EmitSignExtendLow16U32(state, bits) : bits;
}

void EmitShiftU16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode,
                  bool arithmetic) {
	const auto lhs = EmitU16LaneBits(state, inst.src[0], false, arithmetic);
	const auto rhs = EmitAndConstant(state, EmitU16LaneBits(state, inst.src[1], false), 0xfu);
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, ret, lhs, rhs});
	EmitStoreU32(state, inst.dst, EmitAndConstant(state, ret, 0xffffu));
}

void EmitBinaryU16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs = EmitU16LaneBits(state, inst.src[0], false);
	const auto rhs = EmitU16LaneBits(state, inst.src[1], false);
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, ret, lhs, rhs});
	EmitStoreU32(state, inst.dst, EmitAndConstant(state, ret, 0xffffu));
}

void EmitMinMaxU16(EmitterState* state, const IR::Instruction& inst, bool signed_value,
                   bool max_value) {
	const auto lhs  = EmitU16LaneBits(state, inst.src[0], false, signed_value);
	const auto rhs  = EmitU16LaneBits(state, inst.src[1], false, signed_value);
	const auto cond = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	const auto op   = signed_value ? (max_value ? OpSGreaterThan : OpSLessThan)
	                               : (max_value ? OpUGreaterThan : OpULessThan);
	state->builder.AddFunction({op, state->bool_type, cond, lhs, rhs});
	state->builder.AddFunction({OpSelect, state->uint_type, ret, cond, lhs, rhs});
	EmitStoreU32(state, inst.dst, EmitAndConstant(state, ret, 0xffffu));
}

uint32_t EmitXorConstant(EmitterState* state, uint32_t value, uint32_t mask) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseXor, state->uint_type, ret, value, ConstantU32(state, mask)});
	return ret;
}

AddCarryResult EmitAddCarryValues(EmitterState* state, uint32_t lhs, uint32_t rhs,
                                  uint32_t carry_in) {
	const auto pair0  = state->builder.AllocateId();
	const auto sum0   = state->builder.AllocateId();
	const auto carry0 = state->builder.AllocateId();
	const auto pair1  = state->builder.AllocateId();
	const auto sum1   = state->builder.AllocateId();
	const auto carry1 = state->builder.AllocateId();
	const auto carry  = state->builder.AllocateId();
	state->builder.AddFunction({OpIAddCarry, state->uint_pair_type, pair0, lhs, rhs});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, sum0, pair0, 0});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, carry0, pair0, 1});
	state->builder.AddFunction({OpIAddCarry, state->uint_pair_type, pair1, sum0, carry_in});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, sum1, pair1, 0});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, carry1, pair1, 1});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, carry, carry0, carry1});
	return {sum1, carry};
}

void EmitLaneMaskPairFromBool(EmitterState* state, const IR::Operand& dst, uint32_t value_bool) {
	const auto write_mask = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpLogicalAnd, state->bool_type, write_mask, value_bool, EmitExecActiveBool(state)});
	if (state->per_invocation_masks) {
		EmitPerInvocationMask(state, dst, write_mask);
		return;
	}
	const auto ballot = state->builder.AllocateId();
	state->builder.AddFunction({OpGroupNonUniformBallot, state->vec4_uint_type, ballot,
	                            ConstantU32(state, ScopeSubgroup), write_mask});
	for (uint32_t part = 0; part < 2u; part++) {
		uint32_t result = ConstantU32(state, 0);
		if (state->wave_size != 32u || part == 0u) {
			result = state->builder.AllocateId();
			state->builder.AddFunction(
			    {OpCompositeExtract, state->uint_type, result, ballot, part});
		}
		EmitStoreU32(state, OffsetRegisterOperand(dst, part), result);
	}
}

void EmitIAddCarryU32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs             = EmitValueLoad(state, inst.src[0]);
	const auto rhs             = EmitValueLoad(state, inst.src[1]);
	const auto carry_in_active = EmitLaneMaskOperandActiveBool(state, inst.src[2]);
	const auto carry_in        = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->uint_type, carry_in, carry_in_active,
	                            ConstantU32(state, 1), ConstantU32(state, 0)});
	const auto result     = EmitAddCarryValues(state, lhs, rhs, carry_in);
	const auto carry_bool = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, carry_bool, result.carry, ConstantU32(state, 0)});
	EmitStoreU32(state, inst.dst, result.sum);
	EmitLaneMaskPairFromBool(state, inst.dst2, carry_bool);
}

void EmitISubBorrowU32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs    = EmitValueLoad(state, inst.src[0]);
	const auto rhs    = EmitValueLoad(state, inst.src[1]);
	const auto result = state->builder.AllocateId();
	const auto borrow = state->builder.AllocateId();
	state->builder.AddFunction({OpISub, state->uint_type, result, lhs, rhs});
	state->builder.AddFunction({OpUGreaterThan, state->bool_type, borrow, rhs, lhs});
	EmitStoreU32(state, inst.dst, result);
	EmitLaneMaskPairFromBool(state, inst.dst2, borrow);
}

void EmitScalarAddCarryU32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs      = EmitValueLoad(state, inst.src[0]);
	const auto rhs      = EmitValueLoad(state, inst.src[1]);
	const auto carry_in = EmitAndConstant(state, EmitValueLoad(state, inst.src[2]), 1u);
	const auto result   = EmitAddCarryValues(state, lhs, rhs, carry_in);
	EmitStoreU32(state, inst.dst, result.sum);
	EmitStoreU32(state, SccOperand(), result.carry);
}

void EmitScalarSubBorrowU32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs    = EmitValueLoad(state, inst.src[0]);
	const auto rhs    = EmitValueLoad(state, inst.src[1]);
	const auto result = state->builder.AllocateId();
	const auto borrow = state->builder.AllocateId();
	state->builder.AddFunction({OpISub, state->uint_type, result, lhs, rhs});
	state->builder.AddFunction({OpUGreaterThan, state->bool_type, borrow, rhs, lhs});
	EmitStoreU32(state, inst.dst, result);
	EmitStoreSccBool(state, borrow);
}

void EmitScalarSubBorrowCarryU32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs       = EmitValueLoad(state, inst.src[0]);
	const auto rhs       = EmitValueLoad(state, inst.src[1]);
	const auto borrow_in = EmitAndConstant(state, EmitValueLoad(state, inst.src[2]), 1u);
	const auto partial   = state->builder.AllocateId();
	const auto result    = state->builder.AllocateId();
	const auto borrow0   = state->builder.AllocateId();
	const auto borrow1   = state->builder.AllocateId();
	state->builder.AddFunction({OpISub, state->uint_type, partial, lhs, rhs});
	state->builder.AddFunction({OpUGreaterThan, state->bool_type, borrow0, rhs, lhs});
	state->builder.AddFunction({OpISub, state->uint_type, result, partial, borrow_in});
	state->builder.AddFunction({OpUGreaterThan, state->bool_type, borrow1, borrow_in, partial});
	EmitStoreU32(state, inst.dst, result);
	EmitStoreSccBool(state, EmitLogicalOrBool(state, borrow0, borrow1));
}

void EmitUMadU64U32(EmitterState* state, const IR::Instruction& inst) {
	const auto src0        = EmitValueLoad(state, inst.src[0]);
	const auto src1        = EmitValueLoad(state, inst.src[1]);
	const auto add_low     = EmitSequentialValueLoad(state, inst.src[2], 0);
	const auto add_high    = EmitSequentialValueLoad(state, inst.src[2], 1);
	const auto mul_pair    = state->builder.AllocateId();
	const auto mul_low     = state->builder.AllocateId();
	const auto mul_high    = state->builder.AllocateId();
	const auto low_pair    = state->builder.AllocateId();
	const auto result_low  = state->builder.AllocateId();
	const auto carry_low   = state->builder.AllocateId();
	const auto high_pair   = state->builder.AllocateId();
	const auto high_sum    = state->builder.AllocateId();
	const auto carry_high0 = state->builder.AllocateId();
	const auto carry_pair  = state->builder.AllocateId();
	const auto result_high = state->builder.AllocateId();
	const auto carry_high1 = state->builder.AllocateId();
	const auto carry       = state->builder.AllocateId();
	const auto carry_bool  = state->builder.AllocateId();

	state->builder.AddFunction({OpUMulExtended, state->uint_pair_type, mul_pair, src0, src1});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, mul_low, mul_pair, 0});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, mul_high, mul_pair, 1});
	state->builder.AddFunction({OpIAddCarry, state->uint_pair_type, low_pair, mul_low, add_low});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, result_low, low_pair, 0});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, carry_low, low_pair, 1});
	state->builder.AddFunction({OpIAddCarry, state->uint_pair_type, high_pair, mul_high, add_high});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, high_sum, high_pair, 0});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, carry_high0, high_pair, 1});
	state->builder.AddFunction(
	    {OpIAddCarry, state->uint_pair_type, carry_pair, high_sum, carry_low});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, result_high, carry_pair, 0});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, carry_high1, carry_pair, 1});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, carry, carry_high0, carry_high1});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, carry_bool, carry, ConstantU32(state, 0)});

	EmitStoreU32(state, inst.dst, result_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), result_high);
	EmitLaneMaskPairFromBool(state, inst.dst2, carry_bool);
}

void EmitScalarSignedOverflowI32(EmitterState* state, const IR::Instruction& inst, bool subtract) {
	const auto lhs         = EmitValueLoad(state, inst.src[0]);
	const auto rhs         = EmitValueLoad(state, inst.src[1]);
	const auto result      = state->builder.AllocateId();
	const auto lhs_sign    = state->builder.AllocateId();
	const auto rhs_sign    = state->builder.AllocateId();
	const auto result_sign = state->builder.AllocateId();
	const auto sign_match  = state->builder.AllocateId();
	const auto sign_change = state->builder.AllocateId();
	const auto overflow    = state->builder.AllocateId();
	state->builder.AddFunction({subtract ? OpISub : OpIAdd, state->uint_type, result, lhs, rhs});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, lhs_sign, lhs, ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, rhs_sign, rhs, ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, result_sign, result, ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {subtract ? OpINotEqual : OpIEqual, state->bool_type, sign_match, lhs_sign, rhs_sign});
	state->builder.AddFunction({OpINotEqual, state->bool_type, sign_change, lhs_sign, result_sign});
	state->builder.AddFunction({OpLogicalAnd, state->bool_type, overflow, sign_match, sign_change});
	EmitStoreU32(state, inst.dst, result);
	EmitStoreSccBool(state, overflow);
}

void EmitScalarShiftLeftAddCarryU32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs       = EmitValueLoad(state, inst.src[0]);
	const auto rhs       = EmitValueLoad(state, inst.src[2]);
	const auto shift     = inst.src[1].kind == IR::OperandKind::ImmediateU32 ? inst.src[1].imm : 0u;
	const auto shifted   = state->builder.AllocateId();
	const auto pair      = state->builder.AllocateId();
	const auto result    = state->builder.AllocateId();
	const auto add_carry = state->builder.AllocateId();
	const auto shifted_out    = state->builder.AllocateId();
	const auto shift_carry    = state->builder.AllocateId();
	const auto add_carry_bool = state->builder.AllocateId();
	const auto carry          = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, shifted, lhs, ConstantU32(state, shift)});
	state->builder.AddFunction({OpIAddCarry, state->uint_pair_type, pair, shifted, rhs});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, result, pair, 0});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, add_carry, pair, 1});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, shifted_out, lhs, ConstantU32(state, 32u - shift)});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, shift_carry, shifted_out, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, add_carry_bool, add_carry, ConstantU32(state, 0)});
	state->builder.AddFunction({OpLogicalOr, state->bool_type, carry, shift_carry, add_carry_bool});
	EmitStoreU32(state, inst.dst, result);
	EmitStoreSccBool(state, carry);
}

void EmitMulU24U32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs = EmitAndConstant(state, EmitValueLoad(state, inst.src[0]), 0x00ffffffu);
	const auto rhs = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 0x00ffffffu);
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpIMul, state->uint_type, ret, lhs, rhs});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitMulI24U32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs_bits = EmitValueLoad(state, inst.src[0]);
	const auto rhs_bits = EmitValueLoad(state, inst.src[1]);
	const auto lhs_i32  = state->builder.AllocateId();
	const auto rhs_i32  = state->builder.AllocateId();
	const auto lhs_i24  = state->builder.AllocateId();
	const auto rhs_i24  = state->builder.AllocateId();
	const auto mul      = state->builder.AllocateId();
	const auto ret      = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, lhs_i32, lhs_bits});
	state->builder.AddFunction({OpBitcast, state->int_type, rhs_i32, rhs_bits});
	state->builder.AddFunction({OpBitFieldSExtract, state->int_type, lhs_i24, lhs_i32,
	                            ConstantU32(state, 0), ConstantU32(state, 24)});
	state->builder.AddFunction({OpBitFieldSExtract, state->int_type, rhs_i24, rhs_i32,
	                            ConstantU32(state, 0), ConstantU32(state, 24)});
	state->builder.AddFunction({OpIMul, state->int_type, mul, lhs_i24, rhs_i24});
	state->builder.AddFunction({OpBitcast, state->uint_type, ret, mul});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitMulHighU32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs  = EmitValueLoad(state, inst.src[0]);
	const auto rhs  = EmitValueLoad(state, inst.src[1]);
	const auto full = state->builder.AllocateId();
	const auto high = state->builder.AllocateId();
	state->builder.AddFunction({OpUMulExtended, state->uint_pair_type, full, lhs, rhs});
	state->builder.AddFunction({OpCompositeExtract, state->uint_type, high, full, 1});
	EmitStoreU32(state, inst.dst, high);
}

void EmitMulHighI32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs_bits = EmitValueLoad(state, inst.src[0]);
	const auto rhs_bits = EmitValueLoad(state, inst.src[1]);
	const auto lhs      = state->builder.AllocateId();
	const auto rhs      = state->builder.AllocateId();
	const auto full     = state->builder.AllocateId();
	const auto high_i32 = state->builder.AllocateId();
	const auto high_u32 = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, lhs, lhs_bits});
	state->builder.AddFunction({OpBitcast, state->int_type, rhs, rhs_bits});
	state->builder.AddFunction({OpSMulExtended, state->int_pair_type, full, lhs, rhs});
	state->builder.AddFunction({OpCompositeExtract, state->int_type, high_i32, full, 1});
	state->builder.AddFunction({OpBitcast, state->uint_type, high_u32, high_i32});
	EmitStoreU32(state, inst.dst, high_u32);
}

void EmitMadU32U24(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs = EmitAndConstant(state, EmitValueLoad(state, inst.src[0]), 0x00ffffffu);
	const auto rhs = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 0x00ffffffu);
	const auto add = EmitValueLoad(state, inst.src[2]);
	const auto mul = state->builder.AllocateId();
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpIMul, state->uint_type, mul, lhs, rhs});
	state->builder.AddFunction({OpIAdd, state->uint_type, ret, mul, add});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitMadI32I24(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs_bits = EmitValueLoad(state, inst.src[0]);
	const auto rhs_bits = EmitValueLoad(state, inst.src[1]);
	const auto add_bits = EmitValueLoad(state, inst.src[2]);
	const auto lhs_i32  = state->builder.AllocateId();
	const auto rhs_i32  = state->builder.AllocateId();
	const auto add_i32  = state->builder.AllocateId();
	const auto lhs_i24  = state->builder.AllocateId();
	const auto rhs_i24  = state->builder.AllocateId();
	const auto mul      = state->builder.AllocateId();
	const auto add      = state->builder.AllocateId();
	const auto ret      = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, lhs_i32, lhs_bits});
	state->builder.AddFunction({OpBitcast, state->int_type, rhs_i32, rhs_bits});
	state->builder.AddFunction({OpBitcast, state->int_type, add_i32, add_bits});
	state->builder.AddFunction({OpBitFieldSExtract, state->int_type, lhs_i24, lhs_i32,
	                            ConstantU32(state, 0), ConstantU32(state, 24)});
	state->builder.AddFunction({OpBitFieldSExtract, state->int_type, rhs_i24, rhs_i32,
	                            ConstantU32(state, 0), ConstantU32(state, 24)});
	state->builder.AddFunction({OpIMul, state->int_type, mul, lhs_i24, rhs_i24});
	state->builder.AddFunction({OpIAdd, state->int_type, add, mul, add_i32});
	state->builder.AddFunction({OpBitcast, state->uint_type, ret, add});
	EmitStoreU32(state, inst.dst, ret);
}

uint32_t EmitShiftLeftConstant(EmitterState* state, uint32_t value, uint32_t shift) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, ret, value, ConstantU32(state, shift)});
	return ret;
}

uint32_t EmitShiftRightConstant(EmitterState* state, uint32_t value, uint32_t shift) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, ret, value, ConstantU32(state, shift)});
	return ret;
}

uint32_t EmitOrU32(EmitterState* state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitBitReplicateHalfU32(EmitterState* state, uint32_t value) {
	auto ret = EmitOrU32(state, value, EmitShiftLeftConstant(state, value, 8));
	ret      = EmitAndConstant(state, ret, 0x00ff00ffu);
	ret      = EmitOrU32(state, ret, EmitShiftLeftConstant(state, ret, 4));
	ret      = EmitAndConstant(state, ret, 0x0f0f0f0fu);
	ret      = EmitOrU32(state, ret, EmitShiftLeftConstant(state, ret, 2));
	ret      = EmitAndConstant(state, ret, 0x33333333u);
	ret      = EmitOrU32(state, ret, EmitShiftLeftConstant(state, ret, 1));
	ret      = EmitAndConstant(state, ret, 0x55555555u);
	return EmitOrU32(state, ret, EmitShiftLeftConstant(state, ret, 1));
}

void EmitBitReplicateB64B32(EmitterState* state, const IR::Instruction& inst) {
	const auto src  = EmitValueLoad(state, inst.src[0]);
	const auto low  = EmitBitReplicateHalfU32(state, EmitAndConstant(state, src, 0x0000ffffu));
	const auto high = EmitBitReplicateHalfU32(state, EmitShiftRightConstant(state, src, 16));
	EmitStoreU32(state, inst.dst, low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), high);
}

uint32_t EmitBitFieldExtractConstant(EmitterState* state, uint32_t value, uint32_t offset,
                                     uint32_t count) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitFieldUExtract, state->uint_type, ret, value,
	                            ConstantU32(state, offset), ConstantU32(state, count)});
	return ret;
}

uint32_t EmitRightAlignedMaskU32(EmitterState* state, uint32_t count) {
	const auto shifted_one = state->builder.AllocateId();
	const auto mask        = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, shifted_one, ConstantU32(state, 1), count});
	state->builder.AddFunction(
	    {OpISub, state->uint_type, mask, shifted_one, ConstantU32(state, 1)});
	return mask;
}

void EmitBitFieldMaskU32(EmitterState* state, const IR::Instruction& inst) {
	const auto count  = EmitAndConstant(state, EmitValueLoad(state, inst.src[0]), 31u);
	const auto offset = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 31u);
	const auto ret    = state->builder.AllocateId();
	state->builder.AddFunction({OpBitFieldInsert, state->uint_type, ret, ConstantU32(state, 0),
	                            ConstantU32(state, 0xffffffffu), offset, count});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitRightAlignedMaskU64(EmitterState* state, uint32_t count, uint32_t* out_low,
                             uint32_t* out_high) {
	const auto count_lt32 = state->builder.AllocateId();
	const auto count_gt32 = state->builder.AllocateId();
	const auto low_count  = state->builder.AllocateId();
	const auto high_base  = state->builder.AllocateId();
	const auto high_count = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpULessThan, state->bool_type, count_lt32, count, ConstantU32(state, 32)});
	state->builder.AddFunction(
	    {OpUGreaterThan, state->bool_type, count_gt32, count, ConstantU32(state, 32)});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, low_count, count_lt32, count, ConstantU32(state, 32)});
	state->builder.AddFunction(
	    {OpISub, state->uint_type, high_base, count, ConstantU32(state, 32)});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, high_count, count_gt32, high_base, ConstantU32(state, 0)});
	*out_low  = state->builder.AllocateId();
	*out_high = state->builder.AllocateId();
	state->builder.AddFunction({OpBitFieldInsert, state->uint_type, *out_low, ConstantU32(state, 0),
	                            ConstantU32(state, 0xffffffffu), ConstantU32(state, 0), low_count});
	state->builder.AddFunction({OpBitFieldInsert, state->uint_type, *out_high,
	                            ConstantU32(state, 0), ConstantU32(state, 0xffffffffu),
	                            ConstantU32(state, 0), high_count});
}

void EmitBitFieldMaskU64(EmitterState* state, const IR::Instruction& inst) {
	const auto count = EmitAndConstant(state, EmitValueLoad(state, inst.src[0]), 63u);
	if (state->per_invocation_masks && inst.dst.kind == IR::OperandKind::Register &&
	    IsMaskRegisterFile(inst.dst.reg.file)) {
		const auto active = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpINotEqual, state->bool_type, active, count, ConstantU32(state, 0)});
		EmitPerInvocationMask(state, inst.dst, active);
		return;
	}
	const auto offset       = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 63u);
	uint32_t   low_mask     = 0;
	uint32_t   high_mask    = 0;
	uint32_t   shifted_low  = 0;
	uint32_t   shifted_high = 0;
	EmitRightAlignedMaskU64(state, count, &low_mask, &high_mask);
	EmitShiftLeftLogicalU64Values(state, low_mask, high_mask, offset, &shifted_low, &shifted_high);
	EmitStoreU32(state, inst.dst, shifted_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), shifted_high);
}

void EmitBitFieldExtractU64(EmitterState* state, const IR::Instruction& inst) {
	const auto src_low      = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto src_high     = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto field        = EmitValueLoad(state, inst.src[1]);
	const auto offset       = EmitBitFieldExtractConstant(state, field, 0, 6);
	const auto raw_count    = EmitBitFieldExtractConstant(state, field, 16, 7);
	const auto available    = state->builder.AllocateId();
	const auto use_raw      = state->builder.AllocateId();
	const auto count        = state->builder.AllocateId();
	uint32_t   shifted_low  = 0;
	uint32_t   shifted_high = 0;
	uint32_t   mask_low     = 0;
	uint32_t   mask_high    = 0;
	const auto ret_low      = state->builder.AllocateId();
	const auto ret_high     = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpISub, state->uint_type, available, ConstantU32(state, 64), offset});
	state->builder.AddFunction({OpULessThan, state->bool_type, use_raw, raw_count, available});
	state->builder.AddFunction({OpSelect, state->uint_type, count, use_raw, raw_count, available});
	EmitShiftRightLogicalU64Values(state, src_low, src_high, offset, &shifted_low, &shifted_high);
	EmitRightAlignedMaskU64(state, count, &mask_low, &mask_high);
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, ret_low, shifted_low, mask_low});
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, ret_high, shifted_high, mask_high});
	EmitStoreU32(state, inst.dst, ret_low);
	EmitStoreU32(state, OffsetRegisterOperand(inst.dst, 1), ret_high);
}

void EmitBitFieldExtractU32(EmitterState* state, const IR::Instruction& inst) {
	const auto src    = EmitValueLoad(state, inst.src[0]);
	const auto field  = EmitValueLoad(state, inst.src[1]);
	const auto offset = EmitBitFieldExtractConstant(state, field, 0, 5);
	const auto count  = EmitBitFieldExtractConstant(state, field, 16, 7);
	const auto ret    = state->builder.AllocateId();
	state->builder.AddFunction({OpBitFieldUExtract, state->uint_type, ret, src, offset, count});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBitFieldExtract3U32(EmitterState* state, const IR::Instruction& inst, bool signed_value) {
	const auto src    = EmitValueLoad(state, inst.src[0]);
	const auto offset = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 31u);
	const auto count  = EmitAndConstant(state, EmitValueLoad(state, inst.src[2]), 31u);
	const auto ret    = state->builder.AllocateId();
	if (signed_value) {
		const auto shifted = state->builder.AllocateId();
		const auto mask    = EmitRightAlignedMaskU32(state, count);
		state->builder.AddFunction(
		    {OpShiftRightArithmetic, state->uint_type, shifted, src, offset});
		state->builder.AddFunction({OpBitwiseAnd, state->uint_type, ret, shifted, mask});
		EmitStoreU32(state, inst.dst, ret);
		return;
	}
	state->builder.AddFunction({OpBitFieldUExtract, state->uint_type, ret, src, offset, count});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBitFieldInsertSelectU32(EmitterState* state, const IR::Instruction& inst) {
	const auto mask      = EmitValueLoad(state, inst.src[0]);
	const auto insert    = EmitValueLoad(state, inst.src[1]);
	const auto base      = EmitValueLoad(state, inst.src[2]);
	const auto not_mask  = state->builder.AllocateId();
	const auto selected  = state->builder.AllocateId();
	const auto preserved = state->builder.AllocateId();
	const auto ret       = state->builder.AllocateId();
	state->builder.AddFunction({OpNot, state->uint_type, not_mask, mask});
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, selected, mask, insert});
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, preserved, not_mask, base});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, ret, selected, preserved});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitAlignBitU32(EmitterState* state, const IR::Instruction& inst) {
	const auto hi           = EmitValueLoad(state, inst.src[0]);
	const auto lo           = EmitValueLoad(state, inst.src[1]);
	const auto shift        = EmitAndConstant(state, EmitValueLoad(state, inst.src[2]), 31u);
	const auto lo_part      = state->builder.AllocateId();
	const auto hi_shift_raw = state->builder.AllocateId();
	const auto hi_shift     = state->builder.AllocateId();
	const auto hi_part_raw  = state->builder.AllocateId();
	const auto non_zero     = state->builder.AllocateId();
	const auto hi_part      = state->builder.AllocateId();
	const auto ret          = state->builder.AllocateId();
	state->builder.AddFunction({OpShiftRightLogical, state->uint_type, lo_part, lo, shift});
	state->builder.AddFunction(
	    {OpISub, state->uint_type, hi_shift_raw, ConstantU32(state, 32), shift});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, hi_shift, hi_shift_raw, ConstantU32(state, 31)});
	state->builder.AddFunction({OpShiftLeftLogical, state->uint_type, hi_part_raw, hi, hi_shift});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, non_zero, shift, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, hi_part, non_zero, hi_part_raw, ConstantU32(state, 0)});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, ret, lo_part, hi_part});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitSelectU32(EmitterState* state, const IR::Instruction& inst) {
	const auto cond = EmitConditionBool(state, inst.src[0]);
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->uint_type, ret, cond,
	                            EmitValueLoad(state, inst.src[1]),
	                            EmitValueLoad(state, inst.src[2])});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitSelectMaskU32(EmitterState* state, const IR::Instruction& inst) {
	const auto cond = EmitLaneMaskOperandActiveBool(state, inst.src[0]);
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->uint_type, ret, cond,
	                            EmitValueLoad(state, inst.src[1]),
	                            EmitValueLoad(state, inst.src[2])});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitSelectF32Bits(EmitterState* state, const IR::Instruction& inst) {
	const auto cond     = EmitConditionBool(state, inst.src[0]);
	const auto selected = state->builder.AllocateId();
	const auto ret      = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->float_type, selected, cond,
	                            EmitFloatLoad(state, inst.src[1]),
	                            EmitFloatLoad(state, inst.src[2])});
	state->builder.AddFunction({OpBitcast, state->uint_type, ret, selected});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitSelectMaskF32Bits(EmitterState* state, const IR::Instruction& inst) {
	const auto cond     = EmitLaneMaskOperandActiveBool(state, inst.src[0]);
	const auto selected = state->builder.AllocateId();
	const auto ret      = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->float_type, selected, cond,
	                            EmitFloatLoad(state, inst.src[1]),
	                            EmitFloatLoad(state, inst.src[2])});
	state->builder.AddFunction({OpBitcast, state->uint_type, ret, selected});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitPackU16(EmitterState* state, const IR::Instruction& inst, bool high_src0, bool high_src1) {
	const auto src0    = EmitValueLoad(state, inst.src[0]);
	const auto src1    = EmitValueLoad(state, inst.src[1]);
	const auto lo      = high_src0 ? EmitBitFieldExtractConstant(state, src0, 16, 16)
	                               : EmitAndConstant(state, src0, 0xffffu);
	const auto hi      = high_src1 ? EmitBitFieldExtractConstant(state, src1, 16, 16)
	                               : EmitAndConstant(state, src1, 0xffffu);
	const auto shifted = EmitShiftLeftConstant(state, hi, 16);
	const auto ret     = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, ret, lo, shifted});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitPackU8F32(EmitterState* state, const IR::Instruction& inst) {
	const auto src        = EmitFloatLoad(state, inst.src[0]);
	const auto byte_index = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 3u);
	const auto base       = EmitValueLoad(state, inst.src[2]);

	const auto truncated  = EmitTruncF32Value(state, src);
	const auto below_zero = state->builder.AllocateId();
	const auto above_max  = state->builder.AllocateId();
	const auto safe_low   = state->builder.AllocateId();
	const auto safe_value = state->builder.AllocateId();
	const auto converted  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpFOrdLessThanEqual, state->bool_type, below_zero, src, ConstantF32(state, 0)});
	state->builder.AddFunction({OpFOrdGreaterThanEqual, state->bool_type, above_max, src,
	                            ConstantF32(state, 0x437f0000u)});
	const auto zero_or_nan = EmitLogicalOrBool(state, EmitClassifyF32(state, src).nan, below_zero);
	state->builder.AddFunction(
	    {OpSelect, state->float_type, safe_low, zero_or_nan, ConstantF32(state, 0), truncated});
	state->builder.AddFunction({OpSelect, state->float_type, safe_value, above_max,
	                            ConstantF32(state, 0x437f0000u), safe_low});
	state->builder.AddFunction({OpConvertFToU, state->uint_type, converted, safe_value});
	const auto byte_value = EmitAndConstant(state, converted, 0xffu);

	const auto shift = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, shift, byte_index, ConstantU32(state, 3)});
	const auto byte_mask = state->builder.AllocateId();
	const auto inv_mask  = state->builder.AllocateId();
	const auto cleared   = state->builder.AllocateId();
	const auto shifted   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, byte_mask, ConstantU32(state, 0xffu), shift});
	state->builder.AddFunction({OpNot, state->uint_type, inv_mask, byte_mask});
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, cleared, base, inv_mask});
	state->builder.AddFunction({OpShiftLeftLogical, state->uint_type, shifted, byte_value, shift});
	EmitStoreU32(state, inst.dst, EmitOrU32(state, cleared, shifted));
}

void EmitBinaryF32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs = EmitFloatLoad(state, inst.src[0]);
	const auto rhs = EmitFloatLoad(state, inst.src[1]);
	const auto f32 = state->builder.AllocateId();
	const auto u32 = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->float_type, f32, lhs, rhs});
	const auto result = ApplyResultModifiersF32(state, f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitLdexpF32(EmitterState* state, const IR::Instruction& inst) {
	const auto value    = EmitFloatLoad(state, inst.src[0]);
	const auto exp_bits = EmitValueLoad(state, inst.src[1]);
	const auto exp_i32  = state->builder.AllocateId();
	const auto ret_f32  = state->builder.AllocateId();
	const auto ret_u32  = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, exp_i32, exp_bits});
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, ret_f32, state->glsl_std450, GlslLdexp, value, exp_i32});
	const auto result = ApplyResultModifiersF32(state, ret_f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, ret_u32, result});
	EmitStoreU32(state, inst.dst, ret_u32);
}

uint32_t EmitCompareU32Constant(EmitterState* state, uint32_t opcode, uint32_t value,
                                uint32_t constant) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {opcode, state->bool_type, ret, value, ConstantU32(state, constant)});
	return ret;
}

uint32_t EmitSubConstantMinusU32(EmitterState* state, uint32_t constant, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpISub, state->uint_type, ret, ConstantU32(state, constant), value});
	return ret;
}

uint32_t EmitF32ToF16RtzBits(EmitterState* state, uint32_t f32) {
	const auto bits = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, f32});

	const auto sign = EmitAndConstant(state, EmitShiftRightConstant(state, bits, 16), 0x8000u);
	const auto exp  = EmitAndConstant(state, EmitShiftRightConstant(state, bits, 23), 0xffu);
	const auto mant = EmitAndConstant(state, bits, 0x007fffffu);

	const auto half_exp       = state->builder.AllocateId();
	const auto normal_exp     = state->builder.AllocateId();
	const auto normal_mant    = EmitShiftRightConstant(state, mant, 13);
	const auto normal_payload = state->builder.AllocateId();
	const auto normal         = state->builder.AllocateId();
	state->builder.AddFunction({OpISub, state->uint_type, half_exp, exp, ConstantU32(state, 112)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, normal_exp, half_exp, ConstantU32(state, 10)});
	state->builder.AddFunction(
	    {OpBitwiseOr, state->uint_type, normal_payload, normal_exp, normal_mant});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, normal, sign, normal_payload});

	const auto mant_with_hidden = EmitOrU32(state, mant, ConstantU32(state, 0x00800000u));
	const auto raw_sub_shift    = EmitSubConstantMinusU32(state, 126, exp);
	const auto exp_lt_103       = EmitCompareU32Constant(state, OpULessThan, exp, 103);
	const auto exp_gt_112       = EmitCompareU32Constant(state, OpUGreaterThan, exp, 112);
	const auto sub_shift_low =
	    EmitSelectValueU32(state, exp_lt_103, ConstantU32(state, 31), raw_sub_shift);
	const auto sub_shift =
	    EmitSelectValueU32(state, exp_gt_112, ConstantU32(state, 14), sub_shift_low);
	const auto sub_mant  = state->builder.AllocateId();
	const auto subnormal = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, sub_mant, mant_with_hidden, sub_shift});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, subnormal, sign, sub_mant});

	const auto nan_payload =
	    EmitOrU32(state, EmitShiftRightConstant(state, mant, 13), ConstantU32(state, 0x0200u));
	const auto nan =
	    EmitOrU32(state, sign, EmitOrU32(state, ConstantU32(state, 0x7c00u), nan_payload));
	const auto inf        = EmitOrU32(state, sign, ConstantU32(state, 0x7c00u));
	const auto max_finite = EmitOrU32(state, sign, ConstantU32(state, 0x7bffu));
	const auto mant_zero  = EmitCompareU32Constant(state, OpIEqual, mant, 0);
	const auto special    = EmitSelectValueU32(state, mant_zero, inf, nan);

	const auto exp_le_112 = EmitCompareU32Constant(state, OpULessThanEqual, exp, 112);
	const auto exp_ge_143 = EmitCompareU32Constant(state, OpUGreaterThanEqual, exp, 143);
	const auto exp_eq_255 = EmitCompareU32Constant(state, OpIEqual, exp, 255);
	const auto finite0    = EmitSelectValueU32(state, exp_le_112, subnormal, normal);
	const auto finite1    = EmitSelectValueU32(state, exp_lt_103, sign, finite0);
	const auto finite2    = EmitSelectValueU32(state, exp_ge_143, max_finite, finite1);
	return EmitAndConstant(state, EmitSelectValueU32(state, exp_eq_255, special, finite2), 0xffffu);
}

void EmitPackF32ToF16Rtz(EmitterState* state, const IR::Instruction& inst) {
	const auto lo      = EmitF32ToF16RtzBits(state, EmitFloatLoad(state, inst.src[0]));
	const auto hi      = EmitF32ToF16RtzBits(state, EmitFloatLoad(state, inst.src[1]));
	const auto shifted = EmitShiftLeftConstant(state, hi, 16);
	const auto ret     = EmitOrU32(state, lo, shifted);
	EmitStoreU32(state, inst.dst, ret);
}

void EmitPackNormalizedF32(EmitterState* state, const IR::Instruction& inst, uint32_t ext_inst) {
	const auto src0 = EmitFloatLoad(state, inst.src[0]);
	const auto src1 = EmitFloatLoad(state, inst.src[1]);
	const auto pair = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec2_float_type, pair, src0, src1});
	state->builder.AddFunction(
	    {OpExtInst, state->uint_type, ret, state->glsl_std450, ext_inst, pair});
	EmitStoreU32(state, inst.dst, ret);
}

uint32_t EmitMinMaxU32Value(EmitterState* state, uint32_t lhs, uint32_t rhs, bool max_value) {
	const auto cond = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {max_value ? OpUGreaterThan : OpULessThan, state->bool_type, cond, lhs, rhs});
	state->builder.AddFunction({OpSelect, state->uint_type, ret, cond, lhs, rhs});
	return ret;
}

void EmitMinMaxU32(EmitterState* state, const IR::Instruction& inst, bool max_value) {
	const auto lhs = EmitValueLoad(state, inst.src[0]);
	const auto rhs = EmitValueLoad(state, inst.src[1]);
	const auto ret = EmitMinMaxU32Value(state, lhs, rhs, max_value);
	EmitStoreU32(state, inst.dst, ret);
}

void EmitSadU32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs  = EmitValueLoad(state, inst.src[0]);
	const auto rhs  = EmitValueLoad(state, inst.src[1]);
	const auto add  = EmitValueLoad(state, inst.src[2]);
	const auto high = EmitMinMaxU32Value(state, lhs, rhs, true);
	const auto low  = EmitMinMaxU32Value(state, lhs, rhs, false);
	const auto diff = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction({OpISub, state->uint_type, diff, high, low});
	state->builder.AddFunction({OpIAdd, state->uint_type, ret, diff, add});
	EmitStoreU32(state, inst.dst, ret);
}

uint32_t EmitMinMaxI32Value(EmitterState* state, uint32_t lhs, uint32_t rhs, bool max_value) {
	const auto cond = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {max_value ? OpSGreaterThan : OpSLessThan, state->bool_type, cond, lhs, rhs});
	state->builder.AddFunction({OpSelect, state->uint_type, ret, cond, lhs, rhs});
	return ret;
}

void EmitMinMaxI32(EmitterState* state, const IR::Instruction& inst, bool max_value) {
	const auto lhs = EmitValueLoad(state, inst.src[0]);
	const auto rhs = EmitValueLoad(state, inst.src[1]);
	const auto ret = EmitMinMaxI32Value(state, lhs, rhs, max_value);
	EmitStoreU32(state, inst.dst, ret);
}

void EmitMinMax3U32(EmitterState* state, const IR::Instruction& inst, bool signed_value,
                    bool max_value) {
	const auto src0 = EmitValueLoad(state, inst.src[0]);
	const auto src1 = EmitValueLoad(state, inst.src[1]);
	const auto src2 = EmitValueLoad(state, inst.src[2]);
	const auto tmp  = signed_value ? EmitMinMaxI32Value(state, src0, src1, max_value)
	                               : EmitMinMaxU32Value(state, src0, src1, max_value);
	const auto ret  = signed_value ? EmitMinMaxI32Value(state, tmp, src2, max_value)
	                               : EmitMinMaxU32Value(state, tmp, src2, max_value);
	EmitStoreU32(state, inst.dst, ret);
}

void EmitMed3U32(EmitterState* state, const IR::Instruction& inst, bool signed_value) {
	const auto src0      = EmitValueLoad(state, inst.src[0]);
	const auto src1      = EmitValueLoad(state, inst.src[1]);
	const auto src2      = EmitValueLoad(state, inst.src[2]);
	const auto low_pair  = signed_value ? EmitMinMaxI32Value(state, src0, src1, false)
	                                    : EmitMinMaxU32Value(state, src0, src1, false);
	const auto high_pair = signed_value ? EmitMinMaxI32Value(state, src0, src1, true)
	                                    : EmitMinMaxU32Value(state, src0, src1, true);
	const auto high_min  = signed_value ? EmitMinMaxI32Value(state, high_pair, src2, false)
	                                    : EmitMinMaxU32Value(state, high_pair, src2, false);
	const auto ret       = signed_value ? EmitMinMaxI32Value(state, low_pair, high_min, true)
	                                    : EmitMinMaxU32Value(state, low_pair, high_min, true);
	EmitStoreU32(state, inst.dst, ret);
}

uint32_t EmitBitcastF32ToU32(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->uint_type, ret, value});
	return ret;
}

uint32_t EmitBitcastU32ToF32(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->float_type, ret, value});
	return ret;
}

uint32_t EmitAndU32(EmitterState* state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitLogicalAndBool(EmitterState* state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpLogicalAnd, state->bool_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitLogicalOrBool(EmitterState* state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpLogicalOr, state->bool_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitLogicalNotBool(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpLogicalNot, state->bool_type, ret, value});
	return ret;
}

F32Class EmitClassifyF32(EmitterState* state, uint32_t value) {
	F32Class cls;
	cls.bits                 = EmitBitcastF32ToU32(state, value);
	const auto abs_bits      = EmitAndConstant(state, cls.bits, 0x7fffffffu);
	const auto exponent_bits = EmitAndConstant(state, abs_bits, 0x7f800000u);
	const auto mantissa_bits = EmitAndConstant(state, abs_bits, 0x007fffffu);
	const auto exponent_max  = EmitCompareU32Constant(state, OpIEqual, exponent_bits, 0x7f800000u);
	const auto mantissa_nonzero = EmitCompareU32Constant(state, OpINotEqual, mantissa_bits, 0);
	const auto quiet_bit_clear =
	    EmitCompareU32Constant(state, OpIEqual, EmitAndConstant(state, cls.bits, 0x00400000u), 0);
	cls.nan        = EmitLogicalAndBool(state, exponent_max, mantissa_nonzero);
	cls.snan       = EmitLogicalAndBool(state, cls.nan, quiet_bit_clear);
	cls.zero       = EmitCompareU32Constant(state, OpIEqual, abs_bits, 0);
	cls.quiet_bits = EmitOrU32(state, cls.bits, ConstantU32(state, 0x00400000u));
	return cls;
}

uint32_t EmitClassMaskBitMatch(EmitterState* state, uint32_t mask, uint32_t bit,
                               uint32_t class_match) {
	const auto selected =
	    EmitCompareU32Constant(state, OpINotEqual, EmitAndConstant(state, mask, 1u << bit), 0);
	return EmitLogicalAndBool(state, selected, class_match);
}

uint32_t EmitClassMaskF32(EmitterState* state, uint32_t value, uint32_t mask) {
	const auto bits          = EmitBitcastF32ToU32(state, value);
	const auto sign_bits     = EmitAndConstant(state, bits, 0x80000000u);
	const auto abs_bits      = EmitAndConstant(state, bits, 0x7fffffffu);
	const auto exponent_bits = EmitAndConstant(state, abs_bits, 0x7f800000u);
	const auto mantissa_bits = EmitAndConstant(state, abs_bits, 0x007fffffu);
	const auto quiet_bits    = EmitAndConstant(state, mantissa_bits, 0x00400000u);

	const auto sign             = EmitCompareU32Constant(state, OpINotEqual, sign_bits, 0);
	const auto positive         = EmitLogicalNotBool(state, sign);
	const auto exponent_zero    = EmitCompareU32Constant(state, OpIEqual, exponent_bits, 0);
	const auto exponent_nonzero = EmitLogicalNotBool(state, exponent_zero);
	const auto exponent_inf = EmitCompareU32Constant(state, OpIEqual, exponent_bits, 0x7f800000u);
	const auto finite_exponent  = EmitLogicalNotBool(state, exponent_inf);
	const auto mantissa_zero    = EmitCompareU32Constant(state, OpIEqual, mantissa_bits, 0);
	const auto mantissa_nonzero = EmitLogicalNotBool(state, mantissa_zero);
	const auto quiet            = EmitCompareU32Constant(state, OpINotEqual, quiet_bits, 0);

	const auto nan_common = EmitLogicalAndBool(state, exponent_inf, mantissa_nonzero);
	const auto snan       = EmitLogicalAndBool(state, nan_common, EmitLogicalNotBool(state, quiet));
	const auto qnan       = EmitLogicalAndBool(state, nan_common, quiet);
	const auto inf        = EmitLogicalAndBool(state, exponent_inf, mantissa_zero);
	const auto normal     = EmitLogicalAndBool(state, exponent_nonzero, finite_exponent);
	const auto denorm     = EmitLogicalAndBool(state, exponent_zero, mantissa_nonzero);
	const auto zero       = EmitCompareU32Constant(state, OpIEqual, abs_bits, 0);

	uint32_t match = EmitClassMaskBitMatch(state, mask, 0, snan);
	match          = EmitLogicalOrBool(state, match, EmitClassMaskBitMatch(state, mask, 1, qnan));
	match          = EmitLogicalOrBool(
	    state, match, EmitClassMaskBitMatch(state, mask, 2, EmitLogicalAndBool(state, inf, sign)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 3, EmitLogicalAndBool(state, normal, sign)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 4, EmitLogicalAndBool(state, denorm, sign)));
	match = EmitLogicalOrBool(
	    state, match, EmitClassMaskBitMatch(state, mask, 5, EmitLogicalAndBool(state, zero, sign)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 6, EmitLogicalAndBool(state, zero, positive)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 7, EmitLogicalAndBool(state, denorm, positive)));
	match = EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 8, EmitLogicalAndBool(state, normal, positive)));
	return EmitLogicalOrBool(
	    state, match,
	    EmitClassMaskBitMatch(state, mask, 9, EmitLogicalAndBool(state, inf, positive)));
}

uint32_t EmitMinMaxF32Value(EmitterState* state, uint32_t lhs, uint32_t rhs, bool max_value) {
	const auto lhs_class = EmitClassifyF32(state, lhs);
	const auto rhs_class = EmitClassifyF32(state, rhs);

	const auto numeric_cond = state->builder.AllocateId();
	state->builder.AddFunction({max_value ? OpFOrdGreaterThanEqual : OpFOrdLessThan,
	                            state->bool_type, numeric_cond, lhs, rhs});
	const auto ordered_bits =
	    EmitSelectValueU32(state, numeric_cond, lhs_class.bits, rhs_class.bits);

	const auto both_zero    = EmitLogicalAndBool(state, lhs_class.zero, rhs_class.zero);
	const auto zero_bits    = max_value ? EmitAndU32(state, lhs_class.bits, rhs_class.bits)
	                                    : EmitOrU32(state, lhs_class.bits, rhs_class.bits);
	const auto numeric_bits = EmitSelectValueU32(state, both_zero, zero_bits, ordered_bits);

	const auto rhs_nan_bits =
	    EmitSelectValueU32(state, rhs_class.nan, lhs_class.bits, numeric_bits);
	const auto non_snan_bits =
	    EmitSelectValueU32(state, lhs_class.nan, rhs_class.bits, rhs_nan_bits);
	const auto rhs_snan_bits =
	    EmitSelectValueU32(state, rhs_class.snan, rhs_class.quiet_bits, non_snan_bits);
	const auto result_bits =
	    EmitSelectValueU32(state, lhs_class.snan, lhs_class.quiet_bits, rhs_snan_bits);
	return EmitBitcastU32ToF32(state, result_bits);
}

void EmitMinMaxF32(EmitterState* state, const IR::Instruction& inst, bool max_value) {
	const auto lhs    = EmitFloatLoad(state, inst.src[0]);
	const auto rhs    = EmitFloatLoad(state, inst.src[1]);
	const auto u32    = state->builder.AllocateId();
	const auto f32    = EmitMinMaxF32Value(state, lhs, rhs, max_value);
	const auto result = ApplyResultModifiersF32(state, f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitCompareResult(EmitterState* state, const IR::Operand& dst, uint32_t cond) {
	if (IsSccOperand(dst)) {
		const auto ret = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpSelect, state->uint_type, ret, cond, ConstantU32(state, 1), ConstantU32(state, 0)});
		EmitStoreU32(state, dst, ret);
		return;
	}

	EmitLaneMaskPairFromBool(state, dst, cond);
}

void EmitCompareU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs  = EmitValueLoad(state, inst.src[0]);
	const auto rhs  = EmitValueLoad(state, inst.src[1]);
	const auto cond = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->bool_type, cond, lhs, rhs});
	EmitCompareResult(state, inst.dst, cond);
}

void EmitCompareU16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs  = EmitAndConstant(state, EmitValueLoad(state, inst.src[0]), 0xffffu);
	const auto rhs  = EmitAndConstant(state, EmitValueLoad(state, inst.src[1]), 0xffffu);
	const auto cond = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->bool_type, cond, lhs, rhs});
	EmitCompareResult(state, inst.dst, cond);
}

void EmitCompareI16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs  = EmitSignExtendLow16U32(state, EmitValueLoad(state, inst.src[0]));
	const auto rhs  = EmitSignExtendLow16U32(state, EmitValueLoad(state, inst.src[1]));
	const auto cond = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->bool_type, cond, lhs, rhs});
	EmitCompareResult(state, inst.dst, cond);
}

void EmitBitCompareB32(EmitterState* state, const IR::Instruction& inst, bool bit_set) {
	const auto value     = EmitValueLoad(state, inst.src[0]);
	const auto bit_index = state->builder.AllocateId();
	const auto bit       = state->builder.AllocateId();
	const auto cond      = state->builder.AllocateId();
	const auto ret       = state->builder.AllocateId();
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, bit_index,
	                            EmitValueLoad(state, inst.src[1]), ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {OpBitFieldUExtract, state->uint_type, bit, value, bit_index, ConstantU32(state, 1)});
	state->builder.AddFunction(
	    {OpIEqual, state->bool_type, cond, bit, ConstantU32(state, bit_set ? 1u : 0u)});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, ret, cond, ConstantU32(state, 1), ConstantU32(state, 0)});
	EmitStoreU32(state, inst.dst, ret);
}

void EmitCompareNeU64(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs_low  = EmitSequentialValueLoad(state, inst.src[0], 0);
	const auto lhs_high = EmitSequentialValueLoad(state, inst.src[0], 1);
	const auto rhs_low  = EmitSequentialValueLoad(state, inst.src[1], 0);
	const auto rhs_high = EmitSequentialValueLoad(state, inst.src[1], 1);
	const auto ne_low   = state->builder.AllocateId();
	const auto ne_high  = state->builder.AllocateId();
	const auto cond     = state->builder.AllocateId();
	state->builder.AddFunction({OpINotEqual, state->bool_type, ne_low, lhs_low, rhs_low});
	state->builder.AddFunction({OpINotEqual, state->bool_type, ne_high, lhs_high, rhs_high});
	state->builder.AddFunction({OpLogicalOr, state->bool_type, cond, ne_low, ne_high});
	EmitCompareResult(state, inst.dst, cond);
}

void EmitCompareConstant(EmitterState* state, const IR::Instruction& inst, bool value) {
	const auto cond = state->builder.AllocateId();
	state->builder.AddFunction({value ? OpIEqual : OpINotEqual, state->bool_type, cond,
	                            ConstantU32(state, 0), ConstantU32(state, 0)});
	EmitCompareResult(state, inst.dst, cond);
}

void EmitCompareMaskU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs  = EmitValueLoad(state, inst.src[0]);
	const auto rhs  = EmitValueLoad(state, inst.src[1]);
	const auto cond = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->bool_type, cond, lhs, rhs});
	EmitCompareResult(state, inst.dst, cond);
}

void EmitCompareF32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs  = EmitFloatLoad(state, inst.src[0]);
	const auto rhs  = EmitFloatLoad(state, inst.src[1]);
	const auto cond = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->bool_type, cond, lhs, rhs});
	EmitCompareResult(state, inst.dst, cond);
}

void EmitCompareOrderedF32(EmitterState* state, const IR::Instruction& inst, bool ordered) {
	const auto lhs       = EmitFloatLoad(state, inst.src[0]);
	const auto rhs       = EmitFloatLoad(state, inst.src[1]);
	const auto lhs_check = state->builder.AllocateId();
	const auto rhs_check = state->builder.AllocateId();
	const auto cond      = state->builder.AllocateId();
	if (ordered) {
		state->builder.AddFunction({OpFOrdEqual, state->bool_type, lhs_check, lhs, lhs});
		state->builder.AddFunction({OpFOrdEqual, state->bool_type, rhs_check, rhs, rhs});
		state->builder.AddFunction({OpLogicalAnd, state->bool_type, cond, lhs_check, rhs_check});
	} else {
		state->builder.AddFunction({OpFUnordNotEqual, state->bool_type, lhs_check, lhs, lhs});
		state->builder.AddFunction({OpFUnordNotEqual, state->bool_type, rhs_check, rhs, rhs});
		state->builder.AddFunction({OpLogicalOr, state->bool_type, cond, lhs_check, rhs_check});
	}
	EmitCompareResult(state, inst.dst, cond);
}

void EmitCompareClassF32(EmitterState* state, const IR::Instruction& inst) {
	const auto value = EmitFloatLoad(state, inst.src[0]);
	const auto mask  = EmitValueLoad(state, inst.src[1]);
	EmitCompareResult(state, inst.dst, EmitClassMaskF32(state, value, mask));
}

void EmitCompareMaskF32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs  = EmitFloatLoad(state, inst.src[0]);
	const auto rhs  = EmitFloatLoad(state, inst.src[1]);
	const auto cond = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->bool_type, cond, lhs, rhs});
	EmitCompareResult(state, inst.dst, cond);
}

void EmitConvertU32ToF32(EmitterState* state, const IR::Instruction& inst) {
	const auto src = EmitValueLoad(state, inst.src[0]);
	const auto f32 = state->builder.AllocateId();
	const auto u32 = state->builder.AllocateId();
	state->builder.AddFunction({OpConvertUToF, state->float_type, f32, src});
	const auto result = ApplyResultModifiersF32(state, f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitConvertI32ToF32(EmitterState* state, const IR::Instruction& inst) {
	const auto src = EmitValueLoad(state, inst.src[0]);
	const auto i32 = state->builder.AllocateId();
	const auto f32 = state->builder.AllocateId();
	const auto u32 = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, i32, src});
	state->builder.AddFunction({OpConvertSToF, state->float_type, f32, i32});
	const auto result = ApplyResultModifiersF32(state, f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

uint32_t EmitTruncF32Value(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, ret, state->glsl_std450, GlslTrunc, value});
	return ret;
}

void EmitConvertF32ToU32(EmitterState* state, const IR::Instruction& inst) {
	const auto src        = EmitFloatLoad(state, inst.src[0]);
	const auto trunc      = EmitTruncF32Value(state, src);
	const auto converted  = state->builder.AllocateId();
	const auto below_zero = state->builder.AllocateId();
	const auto above_max  = state->builder.AllocateId();
	state->builder.AddFunction({OpConvertFToU, state->uint_type, converted, trunc});
	state->builder.AddFunction(
	    {OpFOrdLessThanEqual, state->bool_type, below_zero, src, ConstantF32(state, 0)});
	state->builder.AddFunction({OpFOrdGreaterThanEqual, state->bool_type, above_max, src,
	                            ConstantF32(state, 0x4f800000u)});
	const auto zero_or_nan = EmitLogicalOrBool(state, EmitClassifyF32(state, src).nan, below_zero);
	const auto saturated =
	    EmitSelectValueU32(state, above_max, ConstantU32(state, 0xffffffffu), converted);
	const auto ret = EmitSelectValueU32(state, zero_or_nan, ConstantU32(state, 0), saturated);
	EmitStoreU32(state, inst.dst, ret);
}

void EmitConvertF32ToI32(EmitterState* state, const IR::Instruction& inst) {
	const auto src       = EmitFloatLoad(state, inst.src[0]);
	const auto trunc     = EmitTruncF32Value(state, src);
	const auto i32       = state->builder.AllocateId();
	const auto converted = state->builder.AllocateId();
	const auto below_min = state->builder.AllocateId();
	const auto above_max = state->builder.AllocateId();
	state->builder.AddFunction({OpConvertFToS, state->int_type, i32, trunc});
	state->builder.AddFunction({OpBitcast, state->uint_type, converted, i32});
	state->builder.AddFunction(
	    {OpFOrdLessThanEqual, state->bool_type, below_min, src, ConstantF32(state, 0xcf000000u)});
	state->builder.AddFunction({OpFOrdGreaterThanEqual, state->bool_type, above_max, src,
	                            ConstantF32(state, 0x4f000000u)});
	const auto high_sat =
	    EmitSelectValueU32(state, above_max, ConstantU32(state, 0x7fffffffu), converted);
	const auto low_sat =
	    EmitSelectValueU32(state, below_min, ConstantU32(state, 0x80000000u), high_sat);
	const auto ret =
	    EmitSelectValueU32(state, EmitClassifyF32(state, src).nan, ConstantU32(state, 0), low_sat);
	EmitStoreU32(state, inst.dst, ret);
}

void EmitConvertF32ToF16(EmitterState* state, const IR::Instruction& inst) {
	const auto src  = EmitFloatLoad(state, inst.src[0]);
	const auto pair = state->builder.AllocateId();
	const auto u32  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpCompositeConstruct, state->vec2_float_type, pair, src, ConstantF32(state, 0)});
	state->builder.AddFunction(
	    {OpExtInst, state->uint_type, u32, state->glsl_std450, GlslPackHalf2x16, pair});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitConvertF16ToF32(EmitterState* state, const IR::Instruction& inst) {
	const auto src      = EmitValueLoad(state, inst.src[0]);
	const auto unpacked = state->builder.AllocateId();
	auto       f32      = state->builder.AllocateId();
	const auto u32      = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->vec2_float_type, unpacked, state->glsl_std450, GlslUnpackHalf2x16, src});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, f32, unpacked, 0});
	if (inst.src[0].absolute) {
		const auto abs_value = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpExtInst, state->float_type, abs_value, state->glsl_std450, GlslFAbs, f32});
		f32 = abs_value;
	}
	if (inst.src[0].negate) {
		const auto neg_value = state->builder.AllocateId();
		state->builder.AddFunction({OpFNegate, state->float_type, neg_value, f32});
		f32 = neg_value;
	}
	const auto result = ApplyResultModifiersF32(state, f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitConvertF16ToU16(EmitterState* state, const IR::Instruction& inst) {
	const auto src      = EmitValueLoad(state, inst.src[0]);
	const auto unpacked = state->builder.AllocateId();
	auto       f32      = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->vec2_float_type, unpacked, state->glsl_std450, GlslUnpackHalf2x16, src});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, f32, unpacked, 0});
	if (inst.src[0].absolute) {
		const auto abs_value = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpExtInst, state->float_type, abs_value, state->glsl_std450, GlslFAbs, f32});
		f32 = abs_value;
	}
	if (inst.src[0].negate) {
		const auto neg_value = state->builder.AllocateId();
		state->builder.AddFunction({OpFNegate, state->float_type, neg_value, f32});
		f32 = neg_value;
	}

	const auto truncated  = EmitTruncF32Value(state, f32);
	const auto below_zero = state->builder.AllocateId();
	const auto above_max  = state->builder.AllocateId();
	const auto safe_low   = state->builder.AllocateId();
	const auto safe_value = state->builder.AllocateId();
	const auto converted  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpFOrdLessThanEqual, state->bool_type, below_zero, f32, ConstantF32(state, 0)});
	state->builder.AddFunction({OpFOrdGreaterThanEqual, state->bool_type, above_max, f32,
	                            ConstantF32(state, 0x477fff00u)});
	const auto zero_or_nan = EmitLogicalOrBool(state, EmitClassifyF32(state, f32).nan, below_zero);
	state->builder.AddFunction(
	    {OpSelect, state->float_type, safe_low, zero_or_nan, ConstantF32(state, 0), truncated});
	state->builder.AddFunction({OpSelect, state->float_type, safe_value, above_max,
	                            ConstantF32(state, 0x477fff00u), safe_low});
	state->builder.AddFunction({OpConvertFToU, state->uint_type, converted, safe_value});
	EmitStoreU32(state, inst.dst, EmitAndConstant(state, converted, 0xffffu));
}

void EmitConvertU16ToF16(EmitterState* state, const IR::Instruction& inst) {
	const auto src_bits = EmitU16LaneBits(state, inst.src[0], false, false);
	const auto f32      = state->builder.AllocateId();
	state->builder.AddFunction({OpConvertUToF, state->float_type, f32, src_bits});
	EmitStoreU32(state, inst.dst, EmitPackLowF32ToF16Bits(state, f32));
}

void EmitConvertI16ToF16(EmitterState* state, const IR::Instruction& inst) {
	const auto src_bits = EmitU16LaneBits(state, inst.src[0], false, true);
	const auto i32      = state->builder.AllocateId();
	const auto f32      = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, i32, src_bits});
	state->builder.AddFunction({OpConvertSToF, state->float_type, f32, i32});
	EmitStoreU32(state, inst.dst, EmitPackLowF32ToF16Bits(state, f32));
}

void EmitConvertF16ToI16(EmitterState* state, const IR::Instruction& inst) {
	const auto src      = EmitValueLoad(state, inst.src[0]);
	const auto unpacked = state->builder.AllocateId();
	auto       f32      = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->vec2_float_type, unpacked, state->glsl_std450, GlslUnpackHalf2x16, src});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, f32, unpacked, 0});
	if (inst.src[0].absolute) {
		const auto abs_value = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpExtInst, state->float_type, abs_value, state->glsl_std450, GlslFAbs, f32});
		f32 = abs_value;
	}
	if (inst.src[0].negate) {
		const auto neg_value = state->builder.AllocateId();
		state->builder.AddFunction({OpFNegate, state->float_type, neg_value, f32});
		f32 = neg_value;
	}

	const auto truncated  = EmitTruncF32Value(state, f32);
	const auto below_min  = state->builder.AllocateId();
	const auto above_max  = state->builder.AllocateId();
	const auto safe_low   = state->builder.AllocateId();
	const auto safe_value = state->builder.AllocateId();
	const auto nan_safe   = state->builder.AllocateId();
	const auto i32        = state->builder.AllocateId();
	const auto u32        = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpFOrdLessThanEqual, state->bool_type, below_min, f32, ConstantF32(state, 0xc7000000u)});
	state->builder.AddFunction({OpFOrdGreaterThanEqual, state->bool_type, above_max, f32,
	                            ConstantF32(state, 0x46fffe00u)});
	state->builder.AddFunction({OpSelect, state->float_type, safe_low, below_min,
	                            ConstantF32(state, 0xc7000000u), truncated});
	state->builder.AddFunction({OpSelect, state->float_type, safe_value, above_max,
	                            ConstantF32(state, 0x46fffe00u), safe_low});
	state->builder.AddFunction({OpSelect, state->float_type, nan_safe,
	                            EmitClassifyF32(state, f32).nan, ConstantF32(state, 0),
	                            safe_value});
	state->builder.AddFunction({OpConvertFToS, state->int_type, i32, nan_safe});
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, i32});
	EmitStoreU32(state, inst.dst, EmitAndConstant(state, u32, 0xffffu));
}

void EmitConvertRoundPlusInfF32ToI32(EmitterState* state, const IR::Instruction& inst) {
	const auto src     = EmitFloatLoad(state, inst.src[0]);
	const auto biased  = state->builder.AllocateId();
	const auto rounded = state->builder.AllocateId();
	const auto i32     = state->builder.AllocateId();
	const auto u32     = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpFAdd, state->float_type, biased, src, ConstantF32(state, 0x3f000000u)});
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, rounded, state->glsl_std450, GlslFloor, biased});
	state->builder.AddFunction({OpConvertFToS, state->int_type, i32, rounded});
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, i32});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitConvertFloorF32ToI32(EmitterState* state, const IR::Instruction& inst) {
	const auto src   = EmitFloatLoad(state, inst.src[0]);
	const auto floor = state->builder.AllocateId();
	const auto i32   = state->builder.AllocateId();
	const auto u32   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, floor, state->glsl_std450, GlslFloor, src});
	state->builder.AddFunction({OpConvertFToS, state->int_type, i32, floor});
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, i32});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitConvertI4ToOffsetF32(EmitterState* state, const IR::Instruction& inst) {
	const auto src     = EmitValueLoad(state, inst.src[0]);
	const auto src_i32 = state->builder.AllocateId();
	const auto nibble  = state->builder.AllocateId();
	const auto f32     = state->builder.AllocateId();
	const auto scaled  = state->builder.AllocateId();
	const auto u32     = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, src_i32, src});
	state->builder.AddFunction({OpBitFieldSExtract, state->int_type, nibble, src_i32,
	                            ConstantU32(state, 0), ConstantU32(state, 4)});
	state->builder.AddFunction({OpConvertSToF, state->float_type, f32, nibble});
	state->builder.AddFunction(
	    {OpFDiv, state->float_type, scaled, f32, ConstantF32(state, 0x41800000u)});
	const auto result = ApplyResultModifiersF32(state, scaled, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitConvertByteU32ToF32(EmitterState* state, const IR::Instruction& inst) {
	const auto src   = EmitValueLoad(state, inst.src[0]);
	const auto index = inst.src[1].kind == IR::OperandKind::ImmediateU32 ? inst.src[1].imm : 0u;
	const auto byte  = EmitBitFieldExtractConstant(state, src, (index & 3u) * 8u, 8u);
	const auto f32   = state->builder.AllocateId();
	const auto u32   = state->builder.AllocateId();
	state->builder.AddFunction({OpConvertUToF, state->float_type, f32, byte});
	const auto result = ApplyResultModifiersF32(state, f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

uint32_t EmitFlushF32DenormToSignedZero(EmitterState* state, uint32_t value) {
	const auto bits      = state->builder.AllocateId();
	const auto abs_bits  = state->builder.AllocateId();
	const auto sign_bits = state->builder.AllocateId();
	const auto non_zero  = state->builder.AllocateId();
	const auto subnormal = state->builder.AllocateId();
	const auto flush     = state->builder.AllocateId();
	const auto selected  = state->builder.AllocateId();
	const auto ret       = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, value});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, abs_bits, bits, ConstantU32(state, 0x7fffffffu)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, sign_bits, bits, ConstantU32(state, 0x80000000u)});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, non_zero, abs_bits, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpULessThan, state->bool_type, subnormal, abs_bits, ConstantU32(state, 0x00800000u)});
	state->builder.AddFunction({OpLogicalAnd, state->bool_type, flush, non_zero, subnormal});
	state->builder.AddFunction({OpSelect, state->uint_type, selected, flush, sign_bits, bits});
	state->builder.AddFunction({OpBitcast, state->float_type, ret, selected});
	return ret;
}

void EmitRcpF32(EmitterState* state, const IR::Instruction& inst) {
	const auto src = EmitFlushF32DenormToSignedZero(state, EmitFloatLoad(state, inst.src[0]));
	const auto f32 = state->builder.AllocateId();
	const auto u32 = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpFDiv, state->float_type, f32, ConstantF32(state, 0x3f800000u), src});
	const auto result = ApplyResultModifiersF32(state, f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

uint32_t EmitTrigCycleF32(EmitterState* state, uint32_t src, bool preserve_signed_zero) {
	const auto fract        = state->builder.AllocateId();
	const auto bits         = state->builder.AllocateId();
	const auto abs_bits     = state->builder.AllocateId();
	const auto large        = state->builder.AllocateId();
	const auto finite       = state->builder.AllocateId();
	const auto large_finite = state->builder.AllocateId();
	const auto reduced      = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, fract, state->glsl_std450, GlslFract, src});
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, src});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, abs_bits, bits, ConstantU32(state, 0x7fffffffu)});
	state->builder.AddFunction(
	    {OpUGreaterThanEqual, state->bool_type, large, abs_bits, ConstantU32(state, 0x4b000000u)});
	state->builder.AddFunction(
	    {OpULessThan, state->bool_type, finite, abs_bits, ConstantU32(state, 0x7f800000u)});
	state->builder.AddFunction({OpLogicalAnd, state->bool_type, large_finite, large, finite});
	state->builder.AddFunction(
	    {OpSelect, state->float_type, reduced, large_finite, ConstantF32(state, 0), fract});
	if (!preserve_signed_zero) {
		return reduced;
	}
	const auto zero = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction({OpIEqual, state->bool_type, zero, abs_bits, ConstantU32(state, 0)});
	state->builder.AddFunction({OpSelect, state->float_type, ret, zero, src, reduced});
	return ret;
}

void EmitFloatExtInst(EmitterState* state, const IR::Instruction& inst, uint32_t ext_inst,
                      bool scale_by_two_pi, bool flush_denorm) {
	auto src = EmitFloatLoad(state, inst.src[0]);
	if (flush_denorm) {
		src = EmitFlushF32DenormToSignedZero(state, src);
	}
	if (scale_by_two_pi) {
		src               = EmitTrigCycleF32(state, src, ext_inst == GlslSin);
		const auto scaled = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpFMul, state->float_type, scaled, src, ConstantF32(state, 0x40c90fdbu)});
		src = scaled;
	}
	const auto f32 = state->builder.AllocateId();
	const auto u32 = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, f32, state->glsl_std450, ext_inst, src});
	const auto result = ApplyResultModifiersF32(state, f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitMadF32(EmitterState* state, const IR::Instruction& inst) {
	const auto lhs = EmitMixF32Load(state, inst.src[0]);
	const auto rhs = EmitMixF32Load(state, inst.src[1]);
	const auto add = EmitMixF32Load(state, inst.src[2]);
	const auto f32 = state->builder.AllocateId();
	const auto u32 = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, f32, state->glsl_std450, GlslFma, lhs, rhs, add});
	const auto result = ApplyResultModifiersF32(state, f32, inst.dst);
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, result});
	EmitStoreU32(state, inst.dst, u32);
}

uint32_t EmitFNegateValue(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpFNegate, state->float_type, ret, value});
	return ret;
}

uint32_t EmitFAbsValue(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, ret, state->glsl_std450, GlslFAbs, value});
	return ret;
}

uint32_t EmitFCompareValue(EmitterState* state, uint32_t opcode, uint32_t lhs, uint32_t rhs) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->bool_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitLogicalAndValue(EmitterState* state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpLogicalAnd, state->bool_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitFSelectValue(EmitterState* state, uint32_t cond, uint32_t true_value,
                          uint32_t false_value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->float_type, ret, cond, true_value, false_value});
	return ret;
}

uint32_t EmitFMulConstant(EmitterState* state, uint32_t value, uint32_t bits) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpFMul, state->float_type, ret, value, ConstantF32(state, bits)});
	return ret;
}

CubeF32Values EmitCubeF32Values(EmitterState* state, const IR::Instruction& inst) {
	CubeF32Values cube;
	cube.x            = EmitFloatLoad(state, inst.src[0]);
	cube.y            = EmitFloatLoad(state, inst.src[1]);
	cube.z            = EmitFloatLoad(state, inst.src[2]);
	cube.nx           = EmitFNegateValue(state, cube.x);
	cube.ny           = EmitFNegateValue(state, cube.y);
	cube.nz           = EmitFNegateValue(state, cube.z);
	const auto ax     = EmitFAbsValue(state, cube.x);
	const auto ay     = EmitFAbsValue(state, cube.y);
	const auto az     = EmitFAbsValue(state, cube.z);
	const auto z_ge_x = EmitFCompareValue(state, OpFOrdGreaterThanEqual, az, ax);
	const auto z_ge_y = EmitFCompareValue(state, OpFOrdGreaterThanEqual, az, ay);
	cube.z_face       = EmitLogicalAndValue(state, z_ge_x, z_ge_y);
	cube.y_face       = EmitFCompareValue(state, OpFOrdGreaterThanEqual, ay, ax);
	cube.x_neg        = EmitFCompareValue(state, OpFOrdLessThan, cube.x, ConstantF32(state, 0));
	cube.y_neg        = EmitFCompareValue(state, OpFOrdLessThan, cube.y, ConstantF32(state, 0));
	cube.z_neg        = EmitFCompareValue(state, OpFOrdLessThan, cube.z, ConstantF32(state, 0));
	return cube;
}

uint32_t EmitCubeIdF32(EmitterState* state, const CubeF32Values& cube) {
	const auto x_face =
	    EmitFSelectValue(state, cube.x_neg, ConstantF32(state, 0x3f800000u), ConstantF32(state, 0));
	const auto y_face = EmitFSelectValue(state, cube.y_neg, ConstantF32(state, 0x40400000u),
	                                     ConstantF32(state, 0x40000000u));
	const auto z_face = EmitFSelectValue(state, cube.z_neg, ConstantF32(state, 0x40a00000u),
	                                     ConstantF32(state, 0x40800000u));
	const auto xy     = EmitFSelectValue(state, cube.y_face, y_face, x_face);
	return EmitFSelectValue(state, cube.z_face, z_face, xy);
}

uint32_t EmitCubeScF32(EmitterState* state, const CubeF32Values& cube) {
	const auto x_sc = EmitFSelectValue(state, cube.x_neg, cube.z, cube.nz);
	const auto z_sc = EmitFSelectValue(state, cube.z_neg, cube.nx, cube.x);
	const auto xy   = EmitFSelectValue(state, cube.y_face, cube.x, x_sc);
	return EmitFSelectValue(state, cube.z_face, z_sc, xy);
}

uint32_t EmitCubeTcF32(EmitterState* state, const CubeF32Values& cube) {
	const auto y_tc = EmitFSelectValue(state, cube.y_neg, cube.nz, cube.z);
	const auto xy   = EmitFSelectValue(state, cube.y_face, y_tc, cube.ny);
	return EmitFSelectValue(state, cube.z_face, cube.ny, xy);
}

uint32_t EmitCubeMaF32(EmitterState* state, const CubeF32Values& cube) {
	const auto x_ma = EmitFMulConstant(state, cube.x, 0x40000000u);
	const auto y_ma = EmitFMulConstant(state, cube.y, 0x40000000u);
	const auto z_ma = EmitFMulConstant(state, cube.z, 0x40000000u);
	const auto xy   = EmitFSelectValue(state, cube.y_face, y_ma, x_ma);
	return EmitFSelectValue(state, cube.z_face, z_ma, xy);
}

void EmitCubeF32(EmitterState* state, const IR::Instruction& inst) {
	const auto cube = EmitCubeF32Values(state, inst);
	uint32_t   ret  = 0;
	switch (inst.op) {
		case IR::Opcode::CubeIdF32: ret = EmitCubeIdF32(state, cube); break;
		case IR::Opcode::CubeScF32: ret = EmitCubeScF32(state, cube); break;
		case IR::Opcode::CubeTcF32: ret = EmitCubeTcF32(state, cube); break;
		case IR::Opcode::CubeMaF32: ret = EmitCubeMaF32(state, cube); break;
		default: ret = ConstantF32(state, 0); break;
	}
	const auto bits = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->uint_type, bits, ret});
	EmitStoreU32(state, inst.dst, bits);
}

void EmitDot2AccF32F16(EmitterState* state, const IR::Instruction& inst) {
	const auto src0 = EmitValueLoad(state, inst.src[0]);
	const auto src1 = EmitValueLoad(state, inst.src[1]);
	const auto vec0 = state->builder.AllocateId();
	const auto vec1 = state->builder.AllocateId();
	const auto a_lo = state->builder.AllocateId();
	const auto a_hi = state->builder.AllocateId();
	const auto b_lo = state->builder.AllocateId();
	const auto b_hi = state->builder.AllocateId();
	const auto acc  = EmitFloatLoad(state, inst.src[2]);
	const auto lo   = state->builder.AllocateId();
	const auto dot  = state->builder.AllocateId();
	const auto u32  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->vec2_float_type, vec0, state->glsl_std450, GlslUnpackHalf2x16, src0});
	state->builder.AddFunction(
	    {OpExtInst, state->vec2_float_type, vec1, state->glsl_std450, GlslUnpackHalf2x16, src1});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, a_lo, vec0, 0});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, a_hi, vec0, 1});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, b_lo, vec1, 0});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, b_hi, vec1, 1});
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, lo, state->glsl_std450, GlslFma, a_lo, b_lo, acc});
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, dot, state->glsl_std450, GlslFma, a_hi, b_hi, lo});
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, dot});
	EmitStoreU32(state, inst.dst, u32);
}

uint32_t EmitPackedF16Lane(EmitterState* state, const IR::Operand& operand, bool high_lane) {
	const auto raw      = EmitValueLoad(state, operand);
	const auto unpacked = state->builder.AllocateId();
	auto       value    = state->builder.AllocateId();
	const auto lane     = high_lane ? (operand.op_sel_hi ? 1u : 0u) : (operand.op_sel ? 1u : 0u);
	state->builder.AddFunction(
	    {OpExtInst, state->vec2_float_type, unpacked, state->glsl_std450, GlslUnpackHalf2x16, raw});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, value, unpacked, lane});
	if (operand.absolute) {
		const auto abs_value = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpExtInst, state->float_type, abs_value, state->glsl_std450, GlslFAbs, value});
		value = abs_value;
	}
	if (high_lane ? operand.negate_hi : operand.negate) {
		const auto neg_value = state->builder.AllocateId();
		state->builder.AddFunction({OpFNegate, state->float_type, neg_value, value});
		value = neg_value;
	}
	return value;
}

void EmitCompareF16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs  = EmitPackedF16Lane(state, inst.src[0], false);
	const auto rhs  = EmitPackedF16Lane(state, inst.src[1], false);
	const auto cond = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->bool_type, cond, lhs, rhs});
	EmitCompareResult(state, inst.dst, cond);
}

uint32_t EmitPackedF16LaneBits(EmitterState* state, const IR::Operand& operand, bool high_lane) {
	const auto raw  = EmitValueLoad(state, operand);
	const auto lane = high_lane ? (operand.op_sel_hi ? 1u : 0u) : (operand.op_sel ? 1u : 0u);
	auto       bits = lane != 0u ? EmitShiftRightConstant(state, raw, 16) : raw;
	bits            = EmitAndConstant(state, bits, 0xffffu);
	if (operand.absolute) {
		bits = EmitAndConstant(state, bits, 0x7fffu);
	}
	if (high_lane ? operand.negate_hi : operand.negate) {
		bits = EmitXorConstant(state, bits, 0x8000u);
	}
	return bits;
}

uint32_t EmitF16BitsToF32(EmitterState* state, uint32_t bits) {
	const auto unpacked = state->builder.AllocateId();
	const auto ret      = state->builder.AllocateId();
	state->builder.AddFunction({OpExtInst, state->vec2_float_type, unpacked, state->glsl_std450,
	                            GlslUnpackHalf2x16, bits});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, ret, unpacked, 0});
	return ret;
}

uint32_t EmitPackLowF32ToF16Bits(EmitterState* state, uint32_t value) {
	const auto pair = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpCompositeConstruct, state->vec2_float_type, pair, value, ConstantF32(state, 0)});
	state->builder.AddFunction(
	    {OpExtInst, state->uint_type, ret, state->glsl_std450, GlslPackHalf2x16, pair});
	return EmitAndConstant(state, ret, 0xffffu);
}

void EmitPackB32F16(EmitterState* state, const IR::Instruction& inst) {
	const auto lo = EmitPackedF16LaneBits(state, inst.src[0], false);
	const auto hi =
	    EmitShiftLeftConstant(state, EmitPackedF16LaneBits(state, inst.src[1], false), 16);
	EmitStoreU32(state, inst.dst, EmitOrU32(state, lo, hi));
}

F16Class EmitClassifyF16Bits(EmitterState* state, uint32_t value) {
	F16Class cls;
	cls.bits                    = EmitAndConstant(state, value, 0xffffu);
	const auto abs_bits         = EmitAndConstant(state, cls.bits, 0x7fffu);
	const auto exponent_bits    = EmitAndConstant(state, abs_bits, 0x7c00u);
	const auto mantissa_bits    = EmitAndConstant(state, abs_bits, 0x03ffu);
	const auto exponent_max     = EmitCompareU32Constant(state, OpIEqual, exponent_bits, 0x7c00u);
	const auto mantissa_nonzero = EmitCompareU32Constant(state, OpINotEqual, mantissa_bits, 0);
	const auto quiet_bit_clear =
	    EmitCompareU32Constant(state, OpIEqual, EmitAndConstant(state, cls.bits, 0x0200u), 0);
	cls.nan        = EmitLogicalAndBool(state, exponent_max, mantissa_nonzero);
	cls.snan       = EmitLogicalAndBool(state, cls.nan, quiet_bit_clear);
	cls.zero       = EmitCompareU32Constant(state, OpIEqual, abs_bits, 0);
	cls.quiet_bits = EmitOrU32(state, cls.bits, ConstantU32(state, 0x0200u));
	return cls;
}

uint32_t EmitNegativeNumericF16Bool(EmitterState* state, const F16Class& cls) {
	const auto sign =
	    EmitCompareU32Constant(state, OpINotEqual, EmitAndConstant(state, cls.bits, 0x8000u), 0);
	const auto nonzero = EmitLogicalNotBool(state, cls.zero);
	return EmitLogicalAndBool(state, EmitLogicalAndBool(state, sign, nonzero),
	                          EmitLogicalNotBool(state, cls.nan));
}

uint32_t EmitRcpF16Bits(EmitterState* state, uint32_t bits) {
	const auto f32 = EmitF16BitsToF32(state, bits);
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpFDiv, state->float_type, ret, ConstantF32(state, 0x3f800000u), f32});
	return EmitPackLowF32ToF16Bits(state, ret);
}

uint32_t EmitSqrtF16Bits(EmitterState* state, uint32_t bits) {
	const auto cls  = EmitClassifyF16Bits(state, bits);
	const auto f32  = EmitF16BitsToF32(state, cls.bits);
	const auto sqrt = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, sqrt, state->glsl_std450, GlslSqrt, f32});
	const auto packed  = EmitPackLowF32ToF16Bits(state, sqrt);
	const auto numeric = EmitSelectValueU32(state, EmitNegativeNumericF16Bool(state, cls),
	                                        ConstantU32(state, 0xfe00u), packed);
	return EmitSelectValueU32(state, cls.zero, cls.bits, numeric);
}

uint32_t EmitRsqF16Bits(EmitterState* state, uint32_t bits) {
	const auto cls = EmitClassifyF16Bits(state, bits);
	const auto f32 = EmitF16BitsToF32(state, cls.bits);
	const auto rsq = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, rsq, state->glsl_std450, GlslInverseSqrt, f32});
	const auto packed = EmitPackLowF32ToF16Bits(state, rsq);
	const auto sign =
	    EmitCompareU32Constant(state, OpINotEqual, EmitAndConstant(state, cls.bits, 0x8000u), 0);
	const auto zero_result =
	    EmitSelectValueU32(state, sign, ConstantU32(state, 0xfc00u), ConstantU32(state, 0x7c00u));
	const auto numeric = EmitSelectValueU32(state, EmitNegativeNumericF16Bool(state, cls),
	                                        ConstantU32(state, 0xfe00u), packed);
	return EmitSelectValueU32(state, cls.zero, zero_result, numeric);
}

uint32_t EmitLog2F16Bits(EmitterState* state, uint32_t bits) {
	const auto cls  = EmitClassifyF16Bits(state, bits);
	const auto f32  = EmitF16BitsToF32(state, cls.bits);
	const auto log2 = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, log2, state->glsl_std450, GlslLog2, f32});
	const auto packed = EmitPackLowF32ToF16Bits(state, log2);
	return EmitSelectValueU32(state, EmitNegativeNumericF16Bool(state, cls),
	                          ConstantU32(state, 0xfe00u), packed);
}

uint32_t EmitExp2F16Bits(EmitterState* state, uint32_t bits) {
	const auto f32  = EmitF16BitsToF32(state, bits);
	const auto exp2 = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, exp2, state->glsl_std450, GlslExp2, f32});
	return EmitPackLowF32ToF16Bits(state, exp2);
}

uint32_t EmitF16MathExtInstBits(EmitterState* state, uint32_t bits, uint32_t ext_inst) {
	const auto f32    = EmitF16BitsToF32(state, bits);
	const auto result = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, result, state->glsl_std450, ext_inst, f32});
	return EmitPackLowF32ToF16Bits(state, result);
}

void EmitSpecialF16(EmitterState* state, const IR::Instruction& inst) {
	const auto bits = EmitPackedF16LaneBits(state, inst.src[0], false);
	uint32_t   ret  = ConstantU32(state, 0);
	switch (inst.op) {
		case IR::Opcode::RcpF16: ret = EmitRcpF16Bits(state, bits); break;
		case IR::Opcode::SqrtF16: ret = EmitSqrtF16Bits(state, bits); break;
		case IR::Opcode::InverseSqrtF16: ret = EmitRsqF16Bits(state, bits); break;
		case IR::Opcode::Log2F16: ret = EmitLog2F16Bits(state, bits); break;
		case IR::Opcode::Exp2F16: ret = EmitExp2F16Bits(state, bits); break;
		case IR::Opcode::FloorF16: ret = EmitF16MathExtInstBits(state, bits, GlslFloor); break;
		case IR::Opcode::CeilF16: ret = EmitF16MathExtInstBits(state, bits, GlslCeil); break;
		case IR::Opcode::TruncF16: ret = EmitF16MathExtInstBits(state, bits, GlslTrunc); break;
		case IR::Opcode::RoundEvenF16:
			ret = EmitF16MathExtInstBits(state, bits, GlslRoundEven);
			break;
		default: break;
	}
	EmitStoreU32(state, inst.dst, ret);
}

void EmitBinaryF16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	const auto lhs   = EmitPackedF16Lane(state, inst.src[0], false);
	const auto rhs   = EmitPackedF16Lane(state, inst.src[1], false);
	auto       value = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->float_type, value, lhs, rhs});
	value = ApplyResultModifiersF32(state, value, inst.dst);
	EmitStoreU32(state, inst.dst, EmitPackLowF32ToF16Bits(state, value));
}

void EmitFmaF16(EmitterState* state, const IR::Instruction& inst) {
	const auto fma =
	    ApplyResultModifiersF32(state, EmitPackedF16FmaLane(state, inst, false), inst.dst);
	EmitStoreU32(state, inst.dst, EmitPackLowF32ToF16Bits(state, fma));
}

uint32_t EmitMinMaxF16Bits(EmitterState* state, uint32_t lhs, uint32_t rhs, bool max_value) {
	const auto lhs_class = EmitClassifyF16Bits(state, lhs);
	const auto rhs_class = EmitClassifyF16Bits(state, rhs);
	const auto lhs_f32   = EmitF16BitsToF32(state, lhs_class.bits);
	const auto rhs_f32   = EmitF16BitsToF32(state, rhs_class.bits);

	const auto numeric_cond = state->builder.AllocateId();
	state->builder.AddFunction({max_value ? OpFOrdGreaterThanEqual : OpFOrdLessThan,
	                            state->bool_type, numeric_cond, lhs_f32, rhs_f32});
	const auto ordered_bits =
	    EmitSelectValueU32(state, numeric_cond, lhs_class.bits, rhs_class.bits);

	const auto both_zero    = EmitLogicalAndBool(state, lhs_class.zero, rhs_class.zero);
	const auto zero_bits    = max_value ? EmitAndU32(state, lhs_class.bits, rhs_class.bits)
	                                    : EmitOrU32(state, lhs_class.bits, rhs_class.bits);
	const auto numeric_bits = EmitSelectValueU32(state, both_zero, zero_bits, ordered_bits);

	const auto rhs_nan_bits =
	    EmitSelectValueU32(state, rhs_class.nan, lhs_class.bits, numeric_bits);
	const auto non_snan_bits =
	    EmitSelectValueU32(state, lhs_class.nan, rhs_class.bits, rhs_nan_bits);
	const auto rhs_snan_bits =
	    EmitSelectValueU32(state, rhs_class.snan, rhs_class.quiet_bits, non_snan_bits);
	const auto result_bits =
	    EmitSelectValueU32(state, lhs_class.snan, lhs_class.quiet_bits, rhs_snan_bits);
	return EmitAndConstant(state, result_bits, 0xffffu);
}

void EmitMinMaxF16(EmitterState* state, const IR::Instruction& inst, bool max_value) {
	const auto bits =
	    EmitMinMaxF16Bits(state, EmitPackedF16LaneBits(state, inst.src[0], false),
	                      EmitPackedF16LaneBits(state, inst.src[1], false), max_value);
	EmitStoreU32(state, inst.dst, bits);
}

uint32_t EmitMinMax3F16Bits(EmitterState* state, uint32_t src0, uint32_t src1, uint32_t src2,
                            bool max_value) {
	return EmitMinMaxF16Bits(state, EmitMinMaxF16Bits(state, src0, src1, max_value), src2,
	                         max_value);
}

void EmitMinMax3F16(EmitterState* state, const IR::Instruction& inst, bool max_value) {
	const auto bits =
	    EmitMinMax3F16Bits(state, EmitPackedF16LaneBits(state, inst.src[0], false),
	                       EmitPackedF16LaneBits(state, inst.src[1], false),
	                       EmitPackedF16LaneBits(state, inst.src[2], false), max_value);
	EmitStoreU32(state, inst.dst, bits);
}

uint32_t EmitF16BitsEqualBool(EmitterState* state, uint32_t lhs, uint32_t rhs) {
	const auto lhs_f32 = EmitF16BitsToF32(state, lhs);
	const auto rhs_f32 = EmitF16BitsToF32(state, rhs);
	const auto ret     = state->builder.AllocateId();
	state->builder.AddFunction({OpFOrdEqual, state->bool_type, ret, lhs_f32, rhs_f32});
	return ret;
}

uint32_t EmitAnyNanF16Bool(EmitterState* state, uint32_t src0, uint32_t src1, uint32_t src2) {
	const auto nan01 = EmitLogicalOrBool(state, EmitClassifyF16Bits(state, src0).nan,
	                                     EmitClassifyF16Bits(state, src1).nan);
	return EmitLogicalOrBool(state, nan01, EmitClassifyF16Bits(state, src2).nan);
}

void EmitMed3F16(EmitterState* state, const IR::Instruction& inst) {
	const auto src0 = EmitPackedF16LaneBits(state, inst.src[0], false);
	const auto src1 = EmitPackedF16LaneBits(state, inst.src[1], false);
	const auto src2 = EmitPackedF16LaneBits(state, inst.src[2], false);

	const auto min3     = EmitMinMax3F16Bits(state, src0, src1, src2, false);
	const auto max3     = EmitMinMax3F16Bits(state, src0, src1, src2, true);
	const auto med_src0 = EmitMinMaxF16Bits(state, src1, src2, true);
	const auto med_src1 = EmitMinMaxF16Bits(state, src0, src2, true);
	const auto med_src2 = EmitMinMaxF16Bits(state, src0, src1, true);
	const auto med01 =
	    EmitSelectValueU32(state, EmitF16BitsEqualBool(state, max3, src1), med_src1, med_src2);
	const auto median =
	    EmitSelectValueU32(state, EmitF16BitsEqualBool(state, max3, src0), med_src0, med01);
	const auto bits =
	    EmitSelectValueU32(state, EmitAnyNanF16Bool(state, src0, src1, src2), min3, median);
	EmitStoreU32(state, inst.dst, bits);
}

uint32_t EmitPackedMinMaxF16(EmitterState* state, const IR::Instruction& inst, bool max_value) {
	const auto lo = EmitMinMaxF16Bits(state, EmitPackedF16LaneBits(state, inst.src[0], false),
	                                  EmitPackedF16LaneBits(state, inst.src[1], false), max_value);
	const auto hi = EmitMinMaxF16Bits(state, EmitPackedF16LaneBits(state, inst.src[0], true),
	                                  EmitPackedF16LaneBits(state, inst.src[1], true), max_value);
	return EmitOrU32(state, lo, EmitShiftLeftConstant(state, hi, 16));
}

uint32_t EmitPackedF16BinaryLane(EmitterState* state, const IR::Instruction& inst, bool high_lane,
                                 uint32_t opcode) {
	const auto lhs = EmitPackedF16Lane(state, inst.src[0], high_lane);
	const auto rhs = EmitPackedF16Lane(state, inst.src[1], high_lane);
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->float_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitPackedF16FmaLane(EmitterState* state, const IR::Instruction& inst, bool high_lane) {
	const auto lhs = EmitPackedF16Lane(state, inst.src[0], high_lane);
	const auto rhs = EmitPackedF16Lane(state, inst.src[1], high_lane);
	const auto add = EmitPackedF16Lane(state, inst.src[2], high_lane);
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, ret, state->glsl_std450, GlslFma, lhs, rhs, add});
	return ret;
}

uint32_t EmitPackedF16BitsWithResultModifiers(EmitterState* state, uint32_t bits,
                                              const IR::Operand& dst) {
	if (!dst.clamp && dst.omod == 0u) {
		return bits;
	}
	auto lo = ApplyResultModifiersF32(
	    state, EmitF16BitsToF32(state, EmitAndConstant(state, bits, 0xffffu)), dst);
	auto hi = ApplyResultModifiersF32(
	    state, EmitF16BitsToF32(state, EmitShiftRightConstant(state, bits, 16)), dst);
	const auto pair = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec2_float_type, pair, lo, hi});
	state->builder.AddFunction(
	    {OpExtInst, state->uint_type, ret, state->glsl_std450, GlslPackHalf2x16, pair});
	return ret;
}

void EmitPackedF16(EmitterState* state, const IR::Instruction& inst) {
	uint32_t lo = 0;
	uint32_t hi = 0;
	switch (inst.op) {
		case IR::Opcode::PackedAddF16:
			lo = EmitPackedF16BinaryLane(state, inst, false, OpFAdd);
			hi = EmitPackedF16BinaryLane(state, inst, true, OpFAdd);
			break;
		case IR::Opcode::PackedMulF16:
			lo = EmitPackedF16BinaryLane(state, inst, false, OpFMul);
			hi = EmitPackedF16BinaryLane(state, inst, true, OpFMul);
			break;
		case IR::Opcode::PackedMinF16:
			EmitStoreU32(state, inst.dst,
			             EmitPackedF16BitsWithResultModifiers(
			                 state, EmitPackedMinMaxF16(state, inst, false), inst.dst));
			return;
		case IR::Opcode::PackedMaxF16:
			EmitStoreU32(state, inst.dst,
			             EmitPackedF16BitsWithResultModifiers(
			                 state, EmitPackedMinMaxF16(state, inst, true), inst.dst));
			return;
		case IR::Opcode::PackedFmaF16:
			lo = EmitPackedF16FmaLane(state, inst, false);
			hi = EmitPackedF16FmaLane(state, inst, true);
			break;
		default:
			lo = ConstantF32(state, 0);
			hi = ConstantF32(state, 0);
			break;
	}

	lo              = ApplyResultModifiersF32(state, lo, inst.dst);
	hi              = ApplyResultModifiersF32(state, hi, inst.dst);
	const auto pair = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec2_float_type, pair, lo, hi});
	state->builder.AddFunction(
	    {OpExtInst, state->uint_type, ret, state->glsl_std450, GlslPackHalf2x16, pair});
	EmitStoreU32(state, inst.dst, ret);
}

uint32_t EmitPackedInteger16Select(EmitterState* state, uint32_t lhs, uint32_t rhs,
                                   bool signed_compare, bool max_value) {
	const auto cond = state->builder.AllocateId();
	const auto ret  = state->builder.AllocateId();
	const auto op   = signed_compare ? (max_value ? OpSGreaterThanEqual : OpSLessThan)
	                                 : (max_value ? OpUGreaterThanEqual : OpULessThan);
	state->builder.AddFunction({op, state->bool_type, cond, lhs, rhs});
	state->builder.AddFunction({OpSelect, state->uint_type, ret, cond, lhs, rhs});
	return ret;
}

uint32_t EmitPackedInteger16Lane(EmitterState* state, const IR::Instruction& inst, bool high_lane) {
	const auto opcode = inst.op;
	if (opcode == IR::Opcode::PackedLshlrevB16 || opcode == IR::Opcode::PackedLshrrevB16 ||
	    opcode == IR::Opcode::PackedAshrrevI16) {
		const auto count =
		    EmitAndConstant(state, EmitU16LaneBits(state, inst.src[0], high_lane), 0xfu);
		const auto arithmetic = opcode == IR::Opcode::PackedAshrrevI16;
		const auto value      = EmitU16LaneBits(state, inst.src[1], high_lane, arithmetic);
		const auto ret        = state->builder.AllocateId();
		const auto shift_op   = opcode == IR::Opcode::PackedLshlrevB16 ? OpShiftLeftLogical
		                        : arithmetic                           ? OpShiftRightArithmetic
		                                                               : OpShiftRightLogical;
		state->builder.AddFunction({shift_op, state->uint_type, ret, value, count});
		return ret;
	}

	const auto signed_compare =
	    opcode == IR::Opcode::PackedMaxI16 || opcode == IR::Opcode::PackedMinI16;
	const auto lhs = EmitU16LaneBits(state, inst.src[0], high_lane, signed_compare);
	const auto rhs = EmitU16LaneBits(state, inst.src[1], high_lane, signed_compare);

	switch (opcode) {
		case IR::Opcode::PackedMadI16:
		case IR::Opcode::PackedMadU16: {
			const auto addend =
			    EmitU16LaneBits(state, inst.src[2], high_lane, opcode == IR::Opcode::PackedMadI16);
			const auto mul = state->builder.AllocateId();
			const auto ret = state->builder.AllocateId();
			state->builder.AddFunction({OpIMul, state->uint_type, mul, lhs, rhs});
			state->builder.AddFunction({OpIAdd, state->uint_type, ret, mul, addend});
			return ret;
		}
		case IR::Opcode::PackedMulLoU16: {
			const auto ret = state->builder.AllocateId();
			state->builder.AddFunction({OpIMul, state->uint_type, ret, lhs, rhs});
			return ret;
		}
		case IR::Opcode::PackedAddI16:
		case IR::Opcode::PackedAddU16: {
			const auto ret = state->builder.AllocateId();
			state->builder.AddFunction({OpIAdd, state->uint_type, ret, lhs, rhs});
			return ret;
		}
		case IR::Opcode::PackedSubI16:
		case IR::Opcode::PackedSubU16: {
			const auto ret = state->builder.AllocateId();
			state->builder.AddFunction({OpISub, state->uint_type, ret, lhs, rhs});
			return ret;
		}
		case IR::Opcode::PackedMaxI16:
		case IR::Opcode::PackedMaxU16:
			return EmitPackedInteger16Select(state, lhs, rhs, signed_compare, true);
		case IR::Opcode::PackedMinI16:
		case IR::Opcode::PackedMinU16:
			return EmitPackedInteger16Select(state, lhs, rhs, signed_compare, false);
		default: break;
	}

	return ConstantU32(state, 0);
}

void EmitPackedInteger16(EmitterState* state, const IR::Instruction& inst) {
	const auto lo      = EmitPackedInteger16Lane(state, inst, false);
	const auto hi      = EmitPackedInteger16Lane(state, inst, true);
	const auto lo_bits = EmitAndConstant(state, lo, 0xffffu);
	const auto hi_bits = EmitShiftLeftConstant(state, EmitAndConstant(state, hi, 0xffffu), 16);
	EmitStoreU32(state, inst.dst, EmitOrU32(state, lo_bits, hi_bits));
}

void EmitMadMixF16(EmitterState* state, const IR::Instruction& inst) {
	const auto src0   = EmitMixF32Load(state, inst.src[0]);
	const auto src1   = EmitMixF32Load(state, inst.src[1]);
	const auto src2   = EmitMixF32Load(state, inst.src[2]);
	const auto fma    = state->builder.AllocateId();
	const auto pair   = state->builder.AllocateId();
	const auto packed = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->float_type, fma, state->glsl_std450, GlslFma, src0, src1, src2});
	const auto result = ApplyResultModifiersF32(state, fma, inst.dst);
	state->builder.AddFunction(
	    {OpCompositeConstruct, state->vec2_float_type, pair, result, ConstantF32(state, 0)});
	state->builder.AddFunction(
	    {OpExtInst, state->uint_type, packed, state->glsl_std450, GlslPackHalf2x16, pair});
	EmitStoreU32(state, inst.dst, packed);
}

void EmitMinMax3F32(EmitterState* state, const IR::Instruction& inst, bool max_value) {
	const auto src0 = EmitFloatLoad(state, inst.src[0]);
	const auto src1 = EmitFloatLoad(state, inst.src[1]);
	const auto src2 = EmitFloatLoad(state, inst.src[2]);
	const auto tmp  = EmitMinMaxF32Value(state, src0, src1, max_value);
	const auto base = EmitMinMaxF32Value(state, tmp, src2, max_value);
	const auto f32  = ApplyResultModifiersF32(state, base, inst.dst);
	const auto u32  = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, f32});
	EmitStoreU32(state, inst.dst, u32);
}

void EmitMed3F32(EmitterState* state, const IR::Instruction& inst) {
	const auto src0      = EmitFloatLoad(state, inst.src[0]);
	const auto src1      = EmitFloatLoad(state, inst.src[1]);
	const auto src2      = EmitFloatLoad(state, inst.src[2]);
	const auto low_pair  = EmitMinMaxF32Value(state, src0, src1, false);
	const auto min3      = EmitMinMaxF32Value(state, low_pair, src2, false);
	const auto high_pair = EmitMinMaxF32Value(state, src0, src1, true);
	const auto high_min  = EmitMinMaxF32Value(state, high_pair, src2, false);
	const auto median    = EmitMinMaxF32Value(state, low_pair, high_min, true);
	const auto nan01     = EmitLogicalOrBool(state, EmitClassifyF32(state, src0).nan,
	                                         EmitClassifyF32(state, src1).nan);
	const auto any_nan   = EmitLogicalOrBool(state, nan01, EmitClassifyF32(state, src2).nan);
	const auto base      = EmitFSelectValue(state, any_nan, min3, median);
	const auto f32       = ApplyResultModifiersF32(state, base, inst.dst);
	const auto u32       = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->uint_type, u32, f32});
	EmitStoreU32(state, inst.dst, u32);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
