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

bool Translator::TranslateStateOperation(const Decoder::Instruction& inst) {
	const auto zero = IR::U32(IR::Value(0u));
	switch (inst.opcode) {
		case Decoder::Opcode::S_AND_SAVEEXEC_B32:
		case Decoder::Opcode::S_ANDN1_SAVEEXEC_B32:
		case Decoder::Opcode::S_AND_SAVEEXEC_B64:
		case Decoder::Opcode::S_ANDN1_SAVEEXEC_B64:
		case Decoder::Opcode::S_ORN2_SAVEEXEC_B64: {
			const auto old = ir.GetExec();
			const auto src = ReadMask(inst.src0);
			IR::U1     result;
			switch (inst.opcode) {
				case Decoder::Opcode::S_AND_SAVEEXEC_B32:
				case Decoder::Opcode::S_AND_SAVEEXEC_B64: result = ir.LogicalAnd(old, src); break;
				case Decoder::Opcode::S_ANDN1_SAVEEXEC_B32:
				case Decoder::Opcode::S_ANDN1_SAVEEXEC_B64:
					result = ir.LogicalAnd(old, ir.LogicalNot(src));
					break;
				case Decoder::Opcode::S_ORN2_SAVEEXEC_B64:
					result = ir.LogicalOr(ir.LogicalNot(old), src);
					break;
				default: EXIT("invalid saveexec opcode");
			}
			if (inst.opcode == Decoder::Opcode::S_AND_SAVEEXEC_B64 ||
			    inst.opcode == Decoder::Opcode::S_ANDN1_SAVEEXEC_B64 ||
			    inst.opcode == Decoder::Opcode::S_ORN2_SAVEEXEC_B64) {
				WriteMask64(inst.dst, old);
			} else {
				WriteMask(inst.dst, old);
			}
			const auto mask = BallotMask(result);
			ir.SetExec(result);
			ir.SetExecLo(mask[0]);
			ir.SetExecHi(mask[1]);
			ir.SetScc(result);
			return true;
		}
		case Decoder::Opcode::S_ADD_U32:
		case Decoder::Opcode::S_ADDC_U32:
		case Decoder::Opcode::V_ADD_I32:
		case Decoder::Opcode::V_ADDC_U32: {
			const auto lhs      = ReadU32(inst.src0);
			const auto rhs      = ReadU32(inst.src1);
			const auto carry_in = inst.opcode == Decoder::Opcode::S_ADDC_U32
			                          ? ConditionBit(ConditionOperand(Decoder::OperandKind::Scc))
			                      : inst.opcode == Decoder::Opcode::V_ADDC_U32
			                          ? ConditionBit(inst.src2)
			                          : zero;
			const auto add0     = ir.Emit(IR::ValueOpcode::IAddCarry32, {lhs, rhs});
			const auto partial  = ir.CompositeExtract(add0, 0);
			const auto carry0   = ir.CompositeExtract(add0, 1);
			const auto add1     = ir.Emit(IR::ValueOpcode::IAddCarry32, {partial, carry_in});
			const auto result   = ir.CompositeExtract(add1, 0);
			const auto carry1   = ir.CompositeExtract(add1, 1);
			const auto carry    = ir.INotEqual(ir.BitwiseOr(carry0, carry1), zero);
			WriteOperand(DestinationOperand(inst), result);
			if (inst.opcode == Decoder::Opcode::S_ADD_U32 ||
			    inst.opcode == Decoder::Opcode::S_ADDC_U32) {
				ir.SetScc(carry);
			} else {
				WriteMask(inst.dst2, ir.LogicalAnd(ir.GetExec(), carry));
			}
			return true;
		}
		case Decoder::Opcode::S_SUB_U32:
		case Decoder::Opcode::V_SUB_I32:
		case Decoder::Opcode::V_SUBREV_I32: {
			const bool reverse = inst.opcode == Decoder::Opcode::V_SUBREV_I32;
			const auto lhs     = ReadU32(reverse ? inst.src1 : inst.src0);
			const auto rhs     = ReadU32(reverse ? inst.src0 : inst.src1);
			const auto result  = ir.ISub(lhs, rhs);
			const auto borrow  = ir.UGreaterThan(rhs, lhs);
			WriteOperand(DestinationOperand(inst), result);
			if (inst.opcode == Decoder::Opcode::S_SUB_U32) {
				ir.SetScc(borrow);
			} else {
				WriteMask(inst.dst2, ir.LogicalAnd(ir.GetExec(), borrow));
			}
			return true;
		}
		case Decoder::Opcode::S_SUBB_U32:
		case Decoder::Opcode::V_SUBREV_CO_CI_U32: {
			const bool vector    = inst.opcode == Decoder::Opcode::V_SUBREV_CO_CI_U32;
			const auto lhs       = ReadU32(vector ? inst.src1 : inst.src0);
			const auto rhs       = ReadU32(vector ? inst.src0 : inst.src1);
			const auto borrow_in = vector
			                           ? ConditionBit(inst.src2)
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
			return true;
		}
		case Decoder::Opcode::S_ADD_I32:
		case Decoder::Opcode::S_SUB_I32: {
			const auto lhs      = ReadU32(inst.src0);
			const auto rhs      = ReadU32(inst.src1);
			const bool subtract = inst.opcode == Decoder::Opcode::S_SUB_I32;
			const auto result   = subtract ? ir.ISub(lhs, rhs) : ir.IAdd(lhs, rhs);
			const auto shift    = IR::U32(IR::Value(31u));
			const auto lhs_sign = ir.ShiftRightLogical(lhs, shift);
			const auto rhs_sign = ir.ShiftRightLogical(rhs, shift);
			const auto out_sign = ir.ShiftRightLogical(result, shift);
			const auto inputs =
			    subtract ? ir.INotEqual(lhs_sign, rhs_sign) : ir.IEqual(lhs_sign, rhs_sign);
			const auto changed = ir.INotEqual(lhs_sign, out_sign);
			WriteOperand(DestinationOperand(inst), result);
			ir.SetScc(ir.LogicalAnd(inputs, changed));
			return true;
		}
		case Decoder::Opcode::S_LSHL1_ADD_U32:
		case Decoder::Opcode::S_LSHL2_ADD_U32:
		case Decoder::Opcode::S_LSHL3_ADD_U32:
		case Decoder::Opcode::S_LSHL4_ADD_U32: {
			const uint32_t shift_amount  = 1u + static_cast<uint32_t>(inst.opcode) -
			                               static_cast<uint32_t>(Decoder::Opcode::S_LSHL1_ADD_U32);
			const auto     lhs           = ReadU32(inst.src0);
			const auto     shift         = IR::U32(IR::Value(shift_amount));
			const auto     rhs           = ReadU32(inst.src1);
			const auto     shifted       = ir.ShiftLeftLogical(lhs, shift);
			const auto     result        = ir.IAdd(shifted, rhs);
			const auto     add_carry     = ir.ULessThan(result, shifted);
			const auto     inverse_shift = ir.ISub(IR::U32(IR::Value(32u)), shift);
			const auto     shifted_out   = ir.ShiftRightLogical(lhs, inverse_shift);
			const auto     shift_carry   = ir.INotEqual(shifted_out, zero);
			WriteOperand(DestinationOperand(inst), result);
			ir.SetScc(ir.LogicalOr(add_carry, shift_carry));
			return true;
		}
		case Decoder::Opcode::S_MIN_I32:
		case Decoder::Opcode::S_MAX_I32:
		case Decoder::Opcode::S_MIN_U32:
		case Decoder::Opcode::S_MAX_U32: {
			const auto      lhs = ReadU32(inst.src0);
			const auto      rhs = ReadU32(inst.src1);
			IR::ValueOpcode value_opcode;
			IR::ValueOpcode compare_opcode;
			switch (inst.opcode) {
				case Decoder::Opcode::S_MIN_I32:
					value_opcode   = IR::ValueOpcode::SMin32;
					compare_opcode = IR::ValueOpcode::SLessThan32;
					break;
				case Decoder::Opcode::S_MAX_I32:
					value_opcode   = IR::ValueOpcode::SMax32;
					compare_opcode = IR::ValueOpcode::SGreaterThan32;
					break;
				case Decoder::Opcode::S_MIN_U32:
					value_opcode   = IR::ValueOpcode::UMin32;
					compare_opcode = IR::ValueOpcode::ULessThan32;
					break;
				case Decoder::Opcode::S_MAX_U32:
					value_opcode   = IR::ValueOpcode::UMax32;
					compare_opcode = IR::ValueOpcode::UGreaterThan32;
					break;
				default: EXIT("invalid scalar min/max opcode");
			}
			WriteOperand(DestinationOperand(inst), ir.Emit(value_opcode, {lhs, rhs}));
			ir.SetScc(IR::U1(ir.Emit(compare_opcode, {lhs, rhs})));
			return true;
		}
		default: return false;
	}
}

bool Translator::TranslateControlOperation(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::V_NOP: return true;
		case Decoder::Opcode::S_NOP:
		case Decoder::Opcode::S_SETREG_B32:
		case Decoder::Opcode::S_SLEEP:
		case Decoder::Opcode::S_TRAP: ir.Emit(IR::ValueOpcode::ControlNop); return true;
		case Decoder::Opcode::S_WAITCNT:
		case Decoder::Opcode::S_WAITCNT_DEPCTR: ir.Emit(IR::ValueOpcode::Waitcnt); return true;
		case Decoder::Opcode::S_BARRIER: ir.Emit(IR::ValueOpcode::Barrier); return true;
		case Decoder::Opcode::S_SENDMSG: ir.Emit(IR::ValueOpcode::Sendmsg); return true;
		case Decoder::Opcode::S_TTRACEDATA: ir.Emit(IR::ValueOpcode::TtraceData); return true;
		case Decoder::Opcode::S_INST_PREFETCH: ir.Emit(IR::ValueOpcode::InstPrefetch); return true;
		default: return false;
	}
}

bool Translator::TranslateMove(const Decoder::Instruction& inst, std::string* error) {
	switch (inst.opcode) {
		case Decoder::Opcode::S_GETPC_B64: {
			const auto base    = IR::U64(ir.Emit(IR::ValueOpcode::GetShaderBase));
			const auto address = IR::U64(ir.Emit(
			    IR::ValueOpcode::IAdd64, {base, IR::Value(static_cast<uint64_t>(inst.pc) + 4u)}));
			if (inst.dst.kind == Decoder::OperandKind::Null) {
				return true;
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
						if (error != nullptr) {
							*error = "S_GETPC_B64 destination does not name a valid scalar pair";
						}
						return false;
					}
					break;
				case Decoder::OperandKind::VccLo: high.kind = Decoder::OperandKind::VccHi; break;
				case Decoder::OperandKind::M0: high.kind = Decoder::OperandKind::Null; break;
				case Decoder::OperandKind::ExecLo: high.kind = Decoder::OperandKind::ExecHi; break;
				default:
					if (error != nullptr) {
						*error = "S_GETPC_B64 destination does not name a valid scalar pair";
					}
					return false;
			}
			const auto words = ExtractU64(address);
			WriteOperand(inst.dst, words[0]);
			WriteOperand(high, words[1]);
			return true;
		}
		case Decoder::Opcode::S_CSELECT_B32: {
			const auto result = ir.Select(ir.GetScc(), ReadU32(inst.src0), ReadU32(inst.src1));
			WriteOperand(DestinationOperand(inst), result);
			return true;
		}
		case Decoder::Opcode::S_CSELECT_B64: {
			const auto condition     = ir.GetScc();
			const auto lhs           = ReadU32Pair(inst.src0);
			const auto rhs           = ReadU32Pair(inst.src1);
			const auto selected_mask = IR::U1(ir.Emit(
			    IR::ValueOpcode::SelectU1, {condition, ReadMask(inst.src0), ReadMask(inst.src1)}));
			const auto selected_mask_valid =
			    IR::U1(ir.Emit(IR::ValueOpcode::SelectU1,
			                   {condition, ReadMaskValid(inst.src0), ReadMaskValid(inst.src1)}));
			if (IsExecOrVcc(inst.dst)) {
				WriteMask64(inst.dst, selected_mask);
				return true;
			}
			WriteU32Pair(inst.dst, {ir.Select(condition, lhs[0], rhs[0]),
			                        ir.Select(condition, lhs[1], rhs[1])});
			if (inst.dst.kind == Decoder::OperandKind::Sgpr) {
				const auto dst = static_cast<IR::ScalarReg>(inst.dst.reg);
				ir.SetThreadBitScalarReg(dst, selected_mask);
				ir.SetScalarMaskTag(dst, selected_mask_valid);
			}
			return true;
		}
		case Decoder::Opcode::S_MOV_B32:
		case Decoder::Opcode::S_MOVK_I32:
		case Decoder::Opcode::V_MOV_B32:
			if (IsExecOrVcc(inst.src0) && IsExecOrVcc(inst.dst)) {
				WriteMask(inst.dst, ReadMask(inst.src0));
			} else if (inst.opcode == Decoder::Opcode::V_MOV_B32 &&
			           (inst.src0.negate || inst.src0.absolute)) {
				WriteOperand(DestinationOperand(inst), ReadOperand(inst.src0, IR::Type::F32));
			} else {
				WriteOperand(DestinationOperand(inst), ReadOperand(inst.src0, IR::Type::U32));
			}
			return true;
		case Decoder::Opcode::S_MOV_B64: {
			if (IsExecOrVcc(inst.dst) || IsExecOrVcc(inst.src0)) {
				WriteMask64(inst.dst, ReadMask(inst.src0));
				return true;
			}
			const bool scalar_copy = inst.dst.kind == Decoder::OperandKind::Sgpr &&
			                         inst.src0.kind == Decoder::OperandKind::Sgpr;
			IR::U1     source_mask;
			IR::U1     source_mask_valid;
			if (scalar_copy) {
				source_mask = ir.GetThreadBitScalarReg(static_cast<IR::ScalarReg>(inst.src0.reg));
				source_mask_valid = ir.GetScalarMaskTag(static_cast<IR::ScalarReg>(inst.src0.reg));
			}
			WriteU32Pair(inst.dst, ReadU32Pair(inst.src0));
			if (scalar_copy) {
				ir.SetThreadBitScalarReg(static_cast<IR::ScalarReg>(inst.dst.reg), source_mask);
				ir.SetScalarMaskTag(static_cast<IR::ScalarReg>(inst.dst.reg), source_mask_valid);
			}
			return true;
		}
		case Decoder::Opcode::S_WQM_B64: {
			if (!IsExecOrVcc(inst.dst) && !IsExecOrVcc(inst.src0)) {
				const auto mask_valid = ReadMaskValid(inst.src0);
				const auto invocation_result =
				    IR::U1(ir.Emit(IR::ValueOpcode::WqmMask, {ReadMask(inst.src0)}));
				const auto result = IR::U64(
				    ir.Emit(IR::ValueOpcode::WqmU64, {ReadOperand(inst.src0, IR::Type::U64)}));
				WriteOperand(DestinationOperand(inst), result);
				if (inst.dst.kind == Decoder::OperandKind::Sgpr) {
					const auto dst = static_cast<IR::ScalarReg>(inst.dst.reg);
					ir.SetThreadBitScalarReg(dst, invocation_result);
					ir.SetScalarMaskTag(dst, mask_valid);
					const auto raw_nonzero = IR::U1(
					    ir.Emit(IR::ValueOpcode::INotEqual64, {result, IR::Value(uint64_t {0})}));
					ir.SetScc(IR::U1(ir.Emit(IR::ValueOpcode::SelectU1,
					                         {mask_valid, invocation_result, raw_nonzero})));
				} else {
					ir.SetScc(IR::U1(
					    ir.Emit(IR::ValueOpcode::INotEqual64, {result, IR::Value(uint64_t {0})})));
				}
				return true;
			}
			const auto result = IR::U1(ir.Emit(IR::ValueOpcode::WqmMask, {ReadMask(inst.src0)}));
			WriteMask64(inst.dst, result);
			ir.SetScc(result);
			return true;
		}
		default: return false;
	}
}

bool Translator::TranslateLaneOperation(const Decoder::Instruction& inst, std::string* error) {
	const auto lane_mask = IR::U32(IR::Value(current_wave_size == 32u ? 31u : 63u));
	switch (inst.opcode) {
		case Decoder::Opcode::V_MOVRELS_B32: {
			if (inst.dst.kind != Decoder::OperandKind::Vgpr ||
			    inst.src0.kind != Decoder::OperandKind::Vgpr) {
				if (error != nullptr) {
					*error = "V_MOVRELS_B32 requires VGPR source and destination";
				}
				return false;
			}
			if (inst.dst.sdwa_sel != 6u || inst.dst.omod != 0u || inst.dst.clamp ||
			    inst.src0.sdwa_sel != 6u || inst.src0.sdwa_sext || inst.src0.negate ||
			    inst.src0.absolute || inst.src0.dpp) {
				if (error != nullptr) {
					*error = "V_MOVRELS_B32 modifiers are not implemented";
				}
				return false;
			}
			const auto base     = inst.src0.reg;
			const auto m0       = ir.BitwiseAnd(ReadU32(ConditionOperand(Decoder::OperandKind::M0)),
			                                    IR::U32(IR::Value(0xffu)));
			auto       selected = ir.GetVectorReg(static_cast<IR::VectorReg>(base));
			for (uint32_t index = base + 1u; index < current_vector_limit; index++) {
				const auto match = ir.IEqual(m0, IR::U32(IR::Value(index - base)));
				selected =
				    ir.Select(match, ir.GetVectorReg(static_cast<IR::VectorReg>(index)), selected);
			}
			WriteOperand(DestinationOperand(inst), selected);
			return true;
		}
		case Decoder::Opcode::V_MOVRELD_B32: {
			if (inst.dst.kind != Decoder::OperandKind::Vgpr) {
				if (error != nullptr) {
					*error = "V_MOVRELD_B32 requires VGPR destination";
				}
				return false;
			}
			if (inst.dst.sdwa_sel != 6u || inst.dst.omod != 0u || inst.dst.clamp ||
			    inst.src0.sdwa_sel != 6u || inst.src0.sdwa_sext || inst.src0.negate ||
			    inst.src0.absolute || inst.src0.dpp) {
				if (error != nullptr) {
					*error = "V_MOVRELD_B32 modifiers are not implemented";
				}
				return false;
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
			return true;
		}
		case Decoder::Opcode::V_READFIRSTLANE_B32: {
			const auto result =
			    ir.Emit(IR::ValueOpcode::ReadFirstLane, {ReadU32(inst.src0), ir.GetExec()});
			WriteOperand(DestinationOperand(inst), result);
			return true;
		}
		case Decoder::Opcode::V_READLANE_B32: {
			const auto lane = ir.BitwiseAnd(ReadU32(inst.src1), lane_mask);
			WriteOperand(DestinationOperand(inst),
			             ir.Emit(IR::ValueOpcode::ReadLane, {ReadU32(inst.src0), lane}));
			return true;
		}
		case Decoder::Opcode::V_WRITELANE_B32: {
			EXIT_IF(inst.dst.kind != Decoder::OperandKind::Vgpr);
			const auto reg    = static_cast<IR::VectorReg>(inst.dst.reg);
			const auto lane   = ir.BitwiseAnd(ReadU32(inst.src1), lane_mask);
			const auto result = IR::U32(ir.Emit(IR::ValueOpcode::WriteLane,
			                                    {ir.GetVectorReg(reg), ReadU32(inst.src0), lane}));
			ir.SetVectorReg(reg, result);
			return true;
		}
		case Decoder::Opcode::V_PERMLANE16_B32:
		case Decoder::Opcode::V_PERMLANEX16_B32: {
			const IR::PermlaneFlags flags {
			    .x16            = inst.opcode == Decoder::Opcode::V_PERMLANEX16_B32,
			    .fetch_inactive = inst.dst.op_sel,
			    .bound_control  = inst.dst.op_sel_hi,
			};
			const auto result = ir.Emit(
			    IR::ValueOpcode::Permlane16U32,
			    {ReadU32(inst.src0), ReadU32(inst.src1), ReadU32(inst.src2), ir.GetExec()}, flags);
			auto dst      = DestinationOperand(inst);
			dst.op_sel    = false;
			dst.op_sel_hi = false;
			WriteOperand(dst, result);
			return true;
		}
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
