#include "common/assert.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/recompiler/shaderIR/ShaderIRInternal.h"

#include <algorithm>
#include <fmt/format.h>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

std::string RegisterToString(Register reg) {
	switch (reg.file) {
		case RegisterFile::Scalar: return fmt::format("s{}", reg.index);
		case RegisterFile::Vector: return fmt::format("v{}", reg.index);
		case RegisterFile::Vcc:
			return reg.index == 0 ? std::string("vcc_lo") : std::string("vcc_hi");
		case RegisterFile::Exec:
			return reg.index == 0 ? std::string("exec_lo") : std::string("exec_hi");
		case RegisterFile::Scc:
			if (reg.index == static_cast<uint32_t>(Decoder::OperandKind::Scc)) {
				return "scc";
			}
			if (reg.index == static_cast<uint32_t>(Decoder::OperandKind::VccZ)) {
				return "vccz";
			}
			if (reg.index == static_cast<uint32_t>(Decoder::OperandKind::ExecZ)) {
				return "execz";
			}
			return fmt::format("scc_like_{}", reg.index);
		case RegisterFile::M0: return "m0";
		default: return "unknown_reg";
	}
}

std::string OperandToString(const Operand& operand) {
	std::string text;
	switch (operand.kind) {
		case OperandKind::Register: text = RegisterToString(operand.reg); break;
		case OperandKind::ImmediateU32: text = fmt::format("0x{:08x}", operand.imm); break;
		case OperandKind::PcRelativeU32: text = fmt::format("pc_rel(0x{:08x})", operand.imm); break;
		case OperandKind::Null: text = "null"; break;
		default: text = "unknown"; break;
	}
	if (operand.sdwa_sel != 6 || operand.sdwa_sext) {
		text += fmt::format(".sdwa(sel={},sext={})", operand.sdwa_sel, operand.sdwa_sext ? 1u : 0u);
	}
	if (operand.absolute) {
		text += ".abs";
	}
	if (operand.negate) {
		text += ".neg";
	}
	if (operand.op_sel || operand.op_sel_hi || operand.negate_hi) {
		text += fmt::format(".opsel(lo={},hi={},neghi={})", operand.op_sel ? 1u : 0u,
		                    operand.op_sel_hi ? 1u : 0u, operand.negate_hi ? 1u : 0u);
	}
	if (operand.omod != 0) {
		text += fmt::format(".omod({})", operand.omod);
	}
	if (operand.clamp) {
		text += ".clamp";
	}
	if (operand.dpp) {
		text += fmt::format(".dpp(ctrl=0x{:x},fi={},bc={})", operand.dpp_ctrl,
		                    operand.dpp_fetch_inactive ? 1u : 0u, operand.dpp_bound_ctrl ? 1u : 0u);
	}
	return text;
}

std::string ResourceKindToString(ResourceKind kind) {
	switch (kind) {
		case ResourceKind::ScalarBuffer: return "scalar_buffer";
		case ResourceKind::Buffer: return "buffer";
		case ResourceKind::Flat: return "flat";
		case ResourceKind::Global: return "global";
		case ResourceKind::Scratch: return "scratch";
		case ResourceKind::Lds: return "lds";
		case ResourceKind::Gds: return "gds";
		case ResourceKind::Image: return "image";
		case ResourceKind::ImageUint: return "image_uint";
		case ResourceKind::StorageImage: return "storage_image";
		case ResourceKind::StorageImageUint: return "storage_image_uint";
		case ResourceKind::Sampler: return "sampler";
		default: return "none";
	}
}

const char* ImageDimensionToString(Decoder::ImageDimension dimension) {
	switch (dimension) {
		case Decoder::ImageDimension::Dim2D: return "2d";
		case Decoder::ImageDimension::Dim3D: return "3d";
		case Decoder::ImageDimension::Dim2DArray: return "2d_array";
		default: return "unknown";
	}
}

std::string MemoryInfoToString(const MemoryInfo& mem) {
	if (mem.kind == ResourceKind::None) {
		return "";
	}
	std::string text = fmt::format(
	    " ; {} resource={} sampler={} offset={} offset2={} dmask=0x{:x} data_dwords={} "
	    "data_bits={} component={}/{} dfmt={} nfmt={} signed={} typed={} formatted={} segment={} "
	    "glc={} slc={} idxen={} offen={} image_flags=0x{:x} image_dim={} image_addr={} "
	    "image_mip={}",
	    ResourceKindToString(mem.kind).c_str(), mem.resource, mem.sampler, mem.offset,
	    mem.secondary_offset, mem.dmask, mem.data_dwords, mem.data_bits, mem.component_index,
	    mem.component_count, mem.data_format, mem.number_format, mem.data_signed ? 1u : 0u,
	    mem.typed ? 1u : 0u, mem.formatted ? 1u : 0u, mem.memory_segment, mem.glc ? 1u : 0u,
	    mem.slc ? 1u : 0u, mem.idxen ? 1u : 0u, mem.offen ? 1u : 0u, mem.image_sample_flags,
	    ImageDimensionToString(mem.image_dimension), mem.image_address_components,
	    mem.image_has_mip ? 1u : 0u);
	if (mem.image_nsa_dwords != 0) {
		text += fmt::format(" image_nsa_dwords={} image_nsa_addr=", mem.image_nsa_dwords);
		const auto count =
		    std::min<uint32_t>(mem.image_nsa_dwords * 4u, Decoder::MaxImageNsaAddressComponents);
		for (uint32_t i = 0; i < count; i++) {
			if (i != 0) {
				text += ",";
			}
			text += fmt::format("{}", mem.image_nsa_addr[i]);
		}
	}
	return text;
}

std::string ExportInfoToString(const ExportInfo& exp) {
	return fmt::format(" ; target={} index={} raw=0x{:02x} en=0x{:x} done={} compr={} vm={}",
	                   ExportTargetKindToString(exp.kind).c_str(), exp.index, exp.target, exp.en,
	                   exp.done ? 1u : 0u, exp.compr ? 1u : 0u, exp.vm ? 1u : 0u);
}

std::string InputInfoToString(Opcode op, const InputInfo& input) {
	if (op != Opcode::LoadInputF32) {
		return "";
	}
	return fmt::format(" ; input_attr={} input_chan={}", input.attr, input.chan);
}

std::string TerminatorToString(const CFG::Terminator& term) {
	return fmt::format(
	    "term={} cond={} true={} false={} merge={} continue={} loop={} indirect_sgpr={} "
	    "indirect_selector={} indirect_targets={} selector_values={}",
	    static_cast<uint32_t>(term.kind), CFG::BranchConditionToString(term.condition).c_str(),
	    term.true_block, term.false_block, term.merge_block, term.continue_block,
	    term.loop_header ? 1u : 0u, term.indirect_pc_sgpr, term.indirect_selector_code,
	    static_cast<uint32_t>(term.indirect_targets.size()),
	    static_cast<uint32_t>(term.indirect_selector_values.size()));
}

std::string ExportSourcesToString(const Instruction& inst) {
	std::string text;
	for (uint32_t i = 0; i < inst.src_count && i < 4u; i++) {
		text += i == 0 ? " " : ", ";
		text += OperandToString(inst.src[i]);
	}
	return text;
}

std::string InstructionToString(const Instruction& inst) {
	std::string text = fmt::format("0x{:08x}: ", inst.pc);
	if (inst.op == Opcode::Export) {
		text += "Export";
		text += ExportSourcesToString(inst);
		text += ExportInfoToString(inst.export_info);
		return text;
	}
	switch (inst.op) {
		case Opcode::ControlNop: text += "ControlNop"; break;
		case Opcode::Waitcnt: text += "Waitcnt"; break;
		case Opcode::Barrier: text += "Barrier"; break;
		case Opcode::Sendmsg: text += "Sendmsg"; break;
		case Opcode::TtraceData: text += "TtraceData"; break;
		case Opcode::InstPrefetch: text += "InstPrefetch"; break;
		case Opcode::MoveU32: text += "MoveU32"; break;
		case Opcode::MoveF32Bits: text += "MoveF32Bits"; break;
		case Opcode::MoveRelDestU32: text += "MoveRelDestU32"; break;
		case Opcode::MoveRelSourceU32: text += "MoveRelSourceU32"; break;
		case Opcode::MoveU64: text += "MoveU64"; break;
		case Opcode::WqmB64: text += "WqmB64"; break;
		case Opcode::SaveexecB32: text += "SaveexecB32"; break;
		case Opcode::SaveexecB64: text += "SaveexecB64"; break;
		case Opcode::ReadFirstLaneU32: text += "ReadFirstLaneU32"; break;
		case Opcode::ReadLaneU32: text += "ReadLaneU32"; break;
		case Opcode::WriteLaneU32: text += "WriteLaneU32"; break;
		case Opcode::Permlane16B32: text += "Permlane16B32"; break;
		case Opcode::Permlanex16B32: text += "Permlanex16B32"; break;
		case Opcode::AbsI32: text += "AbsI32"; break;
		case Opcode::IAddU32: text += "IAddU32"; break;
		case Opcode::IAddCarryU32: text += "IAddCarryU32"; break;
		case Opcode::ISubBorrowU32: text += "ISubBorrowU32"; break;
		case Opcode::ScalarAddCarryU32: text += "ScalarAddCarryU32"; break;
		case Opcode::ScalarSubBorrowU32: text += "ScalarSubBorrowU32"; break;
		case Opcode::ScalarSubBorrowCarryU32: text += "ScalarSubBorrowCarryU32"; break;
		case Opcode::ScalarSignedAddOverflowI32: text += "ScalarSignedAddOverflowI32"; break;
		case Opcode::ScalarSignedSubOverflowI32: text += "ScalarSignedSubOverflowI32"; break;
		case Opcode::ScalarShiftLeftAddCarryU32: text += "ScalarShiftLeftAddCarryU32"; break;
		case Opcode::ISubU32: text += "ISubU32"; break;
		case Opcode::IMulU32: text += "IMulU32"; break;
		case Opcode::UMulHighU32: text += "UMulHighU32"; break;
		case Opcode::SMulHighI32: text += "SMulHighI32"; break;
		case Opcode::IMadI24U32: text += "IMadI24U32"; break;
		case Opcode::UMadU24U32: text += "UMadU24U32"; break;
		case Opcode::UMadU64U32: text += "UMadU64U32"; break;
		case Opcode::SadU32: text += "SadU32"; break;
		case Opcode::IAdd3U32: text += "IAdd3U32"; break;
		case Opcode::IMulI24U32: text += "IMulI24U32"; break;
		case Opcode::UMulU24U32: text += "UMulU24U32"; break;
		case Opcode::IMinI32: text += "IMinI32"; break;
		case Opcode::IMaxI32: text += "IMaxI32"; break;
		case Opcode::IMin3I32: text += "IMin3I32"; break;
		case Opcode::IMax3I32: text += "IMax3I32"; break;
		case Opcode::IMed3I32: text += "IMed3I32"; break;
		case Opcode::UMinU32: text += "UMinU32"; break;
		case Opcode::UMaxU32: text += "UMaxU32"; break;
		case Opcode::UMin3U32: text += "UMin3U32"; break;
		case Opcode::UMax3U32: text += "UMax3U32"; break;
		case Opcode::UMed3U32: text += "UMed3U32"; break;
		case Opcode::BitwiseAndU32: text += "BitwiseAndU32"; break;
		case Opcode::BitwiseAndU64: text += "BitwiseAndU64"; break;
		case Opcode::BitwiseAndNotU32: text += "BitwiseAndNotU32"; break;
		case Opcode::BitwiseAndNotU64: text += "BitwiseAndNotU64"; break;
		case Opcode::BitwiseOrU32: text += "BitwiseOrU32"; break;
		case Opcode::BitwiseOrU64: text += "BitwiseOrU64"; break;
		case Opcode::BitwiseOrNotU32: text += "BitwiseOrNotU32"; break;
		case Opcode::BitwiseOrNotU64: text += "BitwiseOrNotU64"; break;
		case Opcode::BitwiseAndOrU32: text += "BitwiseAndOrU32"; break;
		case Opcode::BitwiseOr3U32: text += "BitwiseOr3U32"; break;
		case Opcode::BitwiseXorU32: text += "BitwiseXorU32"; break;
		case Opcode::BitwiseXorU64: text += "BitwiseXorU64"; break;
		case Opcode::BitwiseXor3U32: text += "BitwiseXor3U32"; break;
		case Opcode::BitwiseNandU32: text += "BitwiseNandU32"; break;
		case Opcode::BitwiseNandU64: text += "BitwiseNandU64"; break;
		case Opcode::BitwiseNorU32: text += "BitwiseNorU32"; break;
		case Opcode::BitwiseNorU64: text += "BitwiseNorU64"; break;
		case Opcode::BitwiseXnorU32: text += "BitwiseXnorU32"; break;
		case Opcode::BitwiseXnorU64: text += "BitwiseXnorU64"; break;
		case Opcode::BitwiseNotU32: text += "BitwiseNotU32"; break;
		case Opcode::BitwiseNotU64: text += "BitwiseNotU64"; break;
		case Opcode::BitClearU32: text += "BitClearU32"; break;
		case Opcode::BitSetU32: text += "BitSetU32"; break;
		case Opcode::BitReverseU32: text += "BitReverseU32"; break;
		case Opcode::BitCountU32: text += "BitCountU32"; break;
		case Opcode::BitCountU64: text += "BitCountU64"; break;
		case Opcode::BitReplicateB64B32: text += "BitReplicateB64B32"; break;
		case Opcode::BitCountAddU32: text += "BitCountAddU32"; break;
		case Opcode::MaskedBitCountLowU32: text += "MaskedBitCountLowU32"; break;
		case Opcode::MaskedBitCountHighU32: text += "MaskedBitCountHighU32"; break;
		case Opcode::FindLsbU32: text += "FindLsbU32"; break;
		case Opcode::FindMsbFromHighU32: text += "FindMsbFromHighU32"; break;
		case Opcode::FindMsbFromHighU64: text += "FindMsbFromHighU64"; break;
		case Opcode::BitFieldMaskU32: text += "BitFieldMaskU32"; break;
		case Opcode::BitFieldMaskU64: text += "BitFieldMaskU64"; break;
		case Opcode::BitFieldExtractU32: text += "BitFieldExtractU32"; break;
		case Opcode::BitFieldExtractU64: text += "BitFieldExtractU64"; break;
		case Opcode::BitFieldExtract3U32: text += "BitFieldExtract3U32"; break;
		case Opcode::BitFieldExtract3I32: text += "BitFieldExtract3I32"; break;
		case Opcode::BitFieldInsertSelectU32: text += "BitFieldInsertSelectU32"; break;
		case Opcode::BitCompare0B32: text += "BitCompare0B32"; break;
		case Opcode::BitCompare1B32: text += "BitCompare1B32"; break;
		case Opcode::AlignBitU32: text += "AlignBitU32"; break;
		case Opcode::ShiftLeftAddU32: text += "ShiftLeftAddU32"; break;
		case Opcode::AddShiftLeftU32: text += "AddShiftLeftU32"; break;
		case Opcode::XorAddU32: text += "XorAddU32"; break;
		case Opcode::ShiftLeftOrU32: text += "ShiftLeftOrU32"; break;
		case Opcode::ShiftLeftLogicalU32: text += "ShiftLeftLogicalU32"; break;
		case Opcode::ShiftLeftLogicalU64: text += "ShiftLeftLogicalU64"; break;
		case Opcode::ShiftLeftLogicalU16: text += "ShiftLeftLogicalU16"; break;
		case Opcode::ShiftRightLogicalU32: text += "ShiftRightLogicalU32"; break;
		case Opcode::ShiftRightLogicalU64: text += "ShiftRightLogicalU64"; break;
		case Opcode::ShiftRightLogicalU16: text += "ShiftRightLogicalU16"; break;
		case Opcode::ShiftRightArithmeticI32: text += "ShiftRightArithmeticI32"; break;
		case Opcode::ShiftRightArithmeticI16: text += "ShiftRightArithmeticI16"; break;
		case Opcode::SelectU32: text += "SelectU32"; break;
		case Opcode::SelectMaskU32: text += "SelectMaskU32"; break;
		case Opcode::SelectF32Bits: text += "SelectF32Bits"; break;
		case Opcode::SelectMaskF32Bits: text += "SelectMaskF32Bits"; break;
		case Opcode::SelectU64: text += "SelectU64"; break;
		case Opcode::PackLowLowU16: text += "PackLowLowU16"; break;
		case Opcode::PackLowHighU16: text += "PackLowHighU16"; break;
		case Opcode::PackHighHighU16: text += "PackHighHighU16"; break;
		case Opcode::CompareFalse: text += "CompareFalse"; break;
		case Opcode::CompareTrue: text += "CompareTrue"; break;
		case Opcode::CompareEqU32: text += "CompareEqU32"; break;
		case Opcode::CompareNeU32: text += "CompareNeU32"; break;
		case Opcode::CompareGtU32: text += "CompareGtU32"; break;
		case Opcode::CompareGeU32: text += "CompareGeU32"; break;
		case Opcode::CompareLtU32: text += "CompareLtU32"; break;
		case Opcode::CompareLeU32: text += "CompareLeU32"; break;
		case Opcode::CompareNeU64: text += "CompareNeU64"; break;
		case Opcode::CompareMaskEqU32: text += "CompareMaskEqU32"; break;
		case Opcode::CompareMaskNeU32: text += "CompareMaskNeU32"; break;
		case Opcode::CompareMaskGtU32: text += "CompareMaskGtU32"; break;
		case Opcode::CompareMaskGeU32: text += "CompareMaskGeU32"; break;
		case Opcode::CompareMaskLtU32: text += "CompareMaskLtU32"; break;
		case Opcode::CompareMaskLeU32: text += "CompareMaskLeU32"; break;
		case Opcode::CompareEqI32: text += "CompareEqI32"; break;
		case Opcode::CompareNeI32: text += "CompareNeI32"; break;
		case Opcode::CompareGtI32: text += "CompareGtI32"; break;
		case Opcode::CompareGeI32: text += "CompareGeI32"; break;
		case Opcode::CompareLtI32: text += "CompareLtI32"; break;
		case Opcode::CompareLeI32: text += "CompareLeI32"; break;
		case Opcode::CompareEqI16: text += "CompareEqI16"; break;
		case Opcode::CompareNeI16: text += "CompareNeI16"; break;
		case Opcode::CompareGtI16: text += "CompareGtI16"; break;
		case Opcode::CompareGeI16: text += "CompareGeI16"; break;
		case Opcode::CompareLtI16: text += "CompareLtI16"; break;
		case Opcode::CompareLeI16: text += "CompareLeI16"; break;
		case Opcode::CompareMaskEqI32: text += "CompareMaskEqI32"; break;
		case Opcode::CompareMaskNeI32: text += "CompareMaskNeI32"; break;
		case Opcode::CompareMaskGtI32: text += "CompareMaskGtI32"; break;
		case Opcode::CompareMaskGeI32: text += "CompareMaskGeI32"; break;
		case Opcode::CompareMaskLtI32: text += "CompareMaskLtI32"; break;
		case Opcode::CompareMaskLeI32: text += "CompareMaskLeI32"; break;
		case Opcode::CompareEqU16: text += "CompareEqU16"; break;
		case Opcode::CompareNeU16: text += "CompareNeU16"; break;
		case Opcode::CompareGtU16: text += "CompareGtU16"; break;
		case Opcode::CompareGeU16: text += "CompareGeU16"; break;
		case Opcode::CompareLtU16: text += "CompareLtU16"; break;
		case Opcode::CompareLeU16: text += "CompareLeU16"; break;
		case Opcode::CompareEqF32: text += "CompareEqF32"; break;
		case Opcode::CompareNeF32: text += "CompareNeF32"; break;
		case Opcode::CompareGtF32: text += "CompareGtF32"; break;
		case Opcode::CompareGeF32: text += "CompareGeF32"; break;
		case Opcode::CompareLtF32: text += "CompareLtF32"; break;
		case Opcode::CompareLeF32: text += "CompareLeF32"; break;
		case Opcode::CompareOrderedF32: text += "CompareOrderedF32"; break;
		case Opcode::CompareUnorderedF32: text += "CompareUnorderedF32"; break;
		case Opcode::CompareUnordEqF32: text += "CompareUnordEqF32"; break;
		case Opcode::CompareUnordNeF32: text += "CompareUnordNeF32"; break;
		case Opcode::CompareUnordGtF32: text += "CompareUnordGtF32"; break;
		case Opcode::CompareUnordGeF32: text += "CompareUnordGeF32"; break;
		case Opcode::CompareUnordLtF32: text += "CompareUnordLtF32"; break;
		case Opcode::CompareUnordLeF32: text += "CompareUnordLeF32"; break;
		case Opcode::CompareClassF32: text += "CompareClassF32"; break;
		case Opcode::CompareEqF16: text += "CompareEqF16"; break;
		case Opcode::CompareNeF16: text += "CompareNeF16"; break;
		case Opcode::CompareGtF16: text += "CompareGtF16"; break;
		case Opcode::CompareGeF16: text += "CompareGeF16"; break;
		case Opcode::CompareLtF16: text += "CompareLtF16"; break;
		case Opcode::CompareLeF16: text += "CompareLeF16"; break;
		case Opcode::CompareUnordNeF16: text += "CompareUnordNeF16"; break;
		case Opcode::CompareMaskEqF16: text += "CompareMaskEqF16"; break;
		case Opcode::CompareMaskNeF16: text += "CompareMaskNeF16"; break;
		case Opcode::CompareMaskGtF16: text += "CompareMaskGtF16"; break;
		case Opcode::CompareMaskGeF16: text += "CompareMaskGeF16"; break;
		case Opcode::CompareMaskLtF16: text += "CompareMaskLtF16"; break;
		case Opcode::CompareMaskLeF16: text += "CompareMaskLeF16"; break;
		case Opcode::CompareMaskUnordNeF16: text += "CompareMaskUnordNeF16"; break;
		case Opcode::CompareMaskUnordGeF16: text += "CompareMaskUnordGeF16"; break;
		case Opcode::CompareMaskEqF32: text += "CompareMaskEqF32"; break;
		case Opcode::CompareMaskNeF32: text += "CompareMaskNeF32"; break;
		case Opcode::CompareMaskGtF32: text += "CompareMaskGtF32"; break;
		case Opcode::CompareMaskGeF32: text += "CompareMaskGeF32"; break;
		case Opcode::CompareMaskLtF32: text += "CompareMaskLtF32"; break;
		case Opcode::CompareMaskLeF32: text += "CompareMaskLeF32"; break;
		case Opcode::CompareMaskUnordEqF32: text += "CompareMaskUnordEqF32"; break;
		case Opcode::CompareMaskUnordNeF32: text += "CompareMaskUnordNeF32"; break;
		case Opcode::CompareMaskUnordGtF32: text += "CompareMaskUnordGtF32"; break;
		case Opcode::CompareMaskUnordGeF32: text += "CompareMaskUnordGeF32"; break;
		case Opcode::CompareMaskUnordLtF32: text += "CompareMaskUnordLtF32"; break;
		case Opcode::CompareMaskUnordLeF32: text += "CompareMaskUnordLeF32"; break;
		case Opcode::ConvertByteU32ToF32: text += "ConvertByteU32ToF32"; break;
		case Opcode::ConvertU32ToF32: text += "ConvertU32ToF32"; break;
		case Opcode::ConvertI32ToF32: text += "ConvertI32ToF32"; break;
		case Opcode::ConvertF32ToU32: text += "ConvertF32ToU32"; break;
		case Opcode::ConvertF32ToI32: text += "ConvertF32ToI32"; break;
		case Opcode::ConvertF32ToF16: text += "ConvertF32ToF16"; break;
		case Opcode::ConvertF16ToF32: text += "ConvertF16ToF32"; break;
		case Opcode::ConvertU16ToF16: text += "ConvertU16ToF16"; break;
		case Opcode::ConvertF16ToU16: text += "ConvertF16ToU16"; break;
		case Opcode::ConvertI16ToF16: text += "ConvertI16ToF16"; break;
		case Opcode::ConvertF16ToI16: text += "ConvertF16ToI16"; break;
		case Opcode::ConvertRoundPlusInfF32ToI32: text += "ConvertRoundPlusInfF32ToI32"; break;
		case Opcode::ConvertFloorF32ToI32: text += "ConvertFloorF32ToI32"; break;
		case Opcode::ConvertI4ToOffsetF32: text += "ConvertI4ToOffsetF32"; break;
		case Opcode::LdexpF32: text += "LdexpF32"; break;
		case Opcode::PackF32ToF16Rtz: text += "PackF32ToF16Rtz"; break;
		case Opcode::PackSnorm2x16F32: text += "PackSnorm2x16F32"; break;
		case Opcode::PackUnorm2x16F32: text += "PackUnorm2x16F32"; break;
		case Opcode::PackU16U32: text += "PackU16U32"; break;
		case Opcode::PackU8F32: text += "PackU8F32"; break;
		case Opcode::PackB32F16: text += "PackB32F16"; break;
		case Opcode::PackedMadI16: text += "PackedMadI16"; break;
		case Opcode::PackedMulLoU16: text += "PackedMulLoU16"; break;
		case Opcode::PackedAddI16: text += "PackedAddI16"; break;
		case Opcode::PackedSubI16: text += "PackedSubI16"; break;
		case Opcode::PackedLshlrevB16: text += "PackedLshlrevB16"; break;
		case Opcode::PackedLshrrevB16: text += "PackedLshrrevB16"; break;
		case Opcode::PackedAshrrevI16: text += "PackedAshrrevI16"; break;
		case Opcode::PackedMaxI16: text += "PackedMaxI16"; break;
		case Opcode::PackedMinI16: text += "PackedMinI16"; break;
		case Opcode::PackedMadU16: text += "PackedMadU16"; break;
		case Opcode::PackedAddU16: text += "PackedAddU16"; break;
		case Opcode::PackedSubU16: text += "PackedSubU16"; break;
		case Opcode::PackedMaxU16: text += "PackedMaxU16"; break;
		case Opcode::PackedMinU16: text += "PackedMinU16"; break;
		case Opcode::PackedAddF16: text += "PackedAddF16"; break;
		case Opcode::PackedMulF16: text += "PackedMulF16"; break;
		case Opcode::PackedMinF16: text += "PackedMinF16"; break;
		case Opcode::PackedMaxF16: text += "PackedMaxF16"; break;
		case Opcode::PackedFmaF16: text += "PackedFmaF16"; break;
		case Opcode::AddF16: text += "AddF16"; break;
		case Opcode::SubF16: text += "SubF16"; break;
		case Opcode::MulF16: text += "MulF16"; break;
		case Opcode::MinF16: text += "MinF16"; break;
		case Opcode::MaxF16: text += "MaxF16"; break;
		case Opcode::FmaF16: text += "FmaF16"; break;
		case Opcode::MadMixF16: text += "MadMixF16"; break;
		case Opcode::IAddU16: text += "IAddU16"; break;
		case Opcode::ISubI16: text += "ISubI16"; break;
		case Opcode::IMinI16: text += "IMinI16"; break;
		case Opcode::IMaxI16: text += "IMaxI16"; break;
		case Opcode::UMinU16: text += "UMinU16"; break;
		case Opcode::UMaxU16: text += "UMaxU16"; break;
		case Opcode::RcpF32: text += "RcpF32"; break;
		case Opcode::FractF32: text += "FractF32"; break;
		case Opcode::TruncF32: text += "TruncF32"; break;
		case Opcode::CeilF32: text += "CeilF32"; break;
		case Opcode::RoundEvenF32: text += "RoundEvenF32"; break;
		case Opcode::FloorF32: text += "FloorF32"; break;
		case Opcode::Exp2F32: text += "Exp2F32"; break;
		case Opcode::Log2F32: text += "Log2F32"; break;
		case Opcode::InverseSqrtF32: text += "InverseSqrtF32"; break;
		case Opcode::SqrtF32: text += "SqrtF32"; break;
		case Opcode::RcpF16: text += "RcpF16"; break;
		case Opcode::SqrtF16: text += "SqrtF16"; break;
		case Opcode::InverseSqrtF16: text += "InverseSqrtF16"; break;
		case Opcode::Log2F16: text += "Log2F16"; break;
		case Opcode::Exp2F16: text += "Exp2F16"; break;
		case Opcode::FloorF16: text += "FloorF16"; break;
		case Opcode::CeilF16: text += "CeilF16"; break;
		case Opcode::TruncF16: text += "TruncF16"; break;
		case Opcode::RoundEvenF16: text += "RoundEvenF16"; break;
		case Opcode::SinF32: text += "SinF32"; break;
		case Opcode::CosF32: text += "CosF32"; break;
		case Opcode::CubeIdF32: text += "CubeIdF32"; break;
		case Opcode::CubeScF32: text += "CubeScF32"; break;
		case Opcode::CubeTcF32: text += "CubeTcF32"; break;
		case Opcode::CubeMaF32: text += "CubeMaF32"; break;
		case Opcode::FAddF32: text += "FAddF32"; break;
		case Opcode::FSubF32: text += "FSubF32"; break;
		case Opcode::FMulF32: text += "FMulF32"; break;
		case Opcode::FMinF32: text += "FMinF32"; break;
		case Opcode::FMaxF32: text += "FMaxF32"; break;
		case Opcode::FMadF32: text += "FMadF32"; break;
		case Opcode::Dot2AccF32F16: text += "Dot2AccF32F16"; break;
		case Opcode::FMin3F32: text += "FMin3F32"; break;
		case Opcode::FMax3F32: text += "FMax3F32"; break;
		case Opcode::FMed3F32: text += "FMed3F32"; break;
		case Opcode::Min3F16: text += "Min3F16"; break;
		case Opcode::Max3F16: text += "Max3F16"; break;
		case Opcode::Med3F16: text += "Med3F16"; break;
		case Opcode::LoadSrtDword: text += "LoadSrtDword"; break;
		case Opcode::SLoadDword: text += "SLoadDword"; break;
		case Opcode::SBufferLoadDword: text += "SBufferLoadDword"; break;
		case Opcode::BufferLoadUbyte: text += "BufferLoadUbyte"; break;
		case Opcode::BufferLoadSbyte: text += "BufferLoadSbyte"; break;
		case Opcode::BufferLoadUshort: text += "BufferLoadUshort"; break;
		case Opcode::BufferLoadSshort: text += "BufferLoadSshort"; break;
		case Opcode::BufferLoadDword: text += "BufferLoadDword"; break;
		case Opcode::BufferStoreByte: text += "BufferStoreByte"; break;
		case Opcode::BufferStoreShort: text += "BufferStoreShort"; break;
		case Opcode::BufferStoreDword: text += "BufferStoreDword"; break;
		case Opcode::AtomicSwapU32: text += "AtomicSwapU32"; break;
		case Opcode::AtomicAddU32: text += "AtomicAddU32"; break;
		case Opcode::AtomicSubU32: text += "AtomicSubU32"; break;
		case Opcode::AtomicSMinI32: text += "AtomicSMinI32"; break;
		case Opcode::AtomicUMinU32: text += "AtomicUMinU32"; break;
		case Opcode::AtomicSMaxI32: text += "AtomicSMaxI32"; break;
		case Opcode::AtomicUMaxU32: text += "AtomicUMaxU32"; break;
		case Opcode::AtomicAndU32: text += "AtomicAndU32"; break;
		case Opcode::AtomicOrU32: text += "AtomicOrU32"; break;
		case Opcode::AtomicXorU32: text += "AtomicXorU32"; break;
		case Opcode::FlatLoadUbyte: text += "FlatLoadUbyte"; break;
		case Opcode::FlatLoadSbyte: text += "FlatLoadSbyte"; break;
		case Opcode::FlatLoadUshort: text += "FlatLoadUshort"; break;
		case Opcode::FlatLoadSshort: text += "FlatLoadSshort"; break;
		case Opcode::FlatLoadDword: text += "FlatLoadDword"; break;
		case Opcode::FlatStoreByte: text += "FlatStoreByte"; break;
		case Opcode::FlatStoreShort: text += "FlatStoreShort"; break;
		case Opcode::FlatStoreDword: text += "FlatStoreDword"; break;
		case Opcode::DsReadUbyte: text += "DsReadUbyte"; break;
		case Opcode::DsReadSbyte: text += "DsReadSbyte"; break;
		case Opcode::DsReadUshort: text += "DsReadUshort"; break;
		case Opcode::DsReadSshort: text += "DsReadSshort"; break;
		case Opcode::DsReadB32: text += "DsReadB32"; break;
		case Opcode::DsWriteByte: text += "DsWriteByte"; break;
		case Opcode::DsWriteShort: text += "DsWriteShort"; break;
		case Opcode::DsWriteB32: text += "DsWriteB32"; break;
		case Opcode::DsMinF32: text += "DsMinF32"; break;
		case Opcode::DsMaxF32: text += "DsMaxF32"; break;
		case Opcode::DsSwizzleB32: text += "DsSwizzleB32"; break;
		case Opcode::DsConsume: text += "DsConsume"; break;
		case Opcode::DsAppend: text += "DsAppend"; break;
		case Opcode::DsWriteAddtidB32: text += "DsWriteAddtidB32"; break;
		case Opcode::DsReadAddtidB32: text += "DsReadAddtidB32"; break;
		case Opcode::ImageGetResinfo: text += "ImageGetResinfo"; break;
		case Opcode::ImageGetLod: text += "ImageGetLod"; break;
		case Opcode::ImageLoad: text += "ImageLoad"; break;
		case Opcode::ImageStore: text += "ImageStore"; break;
		case Opcode::ImageSample: text += "ImageSample"; break;
		case Opcode::ImageGather4: text += "ImageGather4"; break;
		case Opcode::LoadInputF32: text += "LoadInputF32"; break;
		case Opcode::Export: text += "Export"; break;
	}
	text += " ";
	text += OperandToString(inst.dst);
	if (inst.dst2.kind != OperandKind::Null) {
		text += ", ";
		text += OperandToString(inst.dst2);
	}
	for (uint32_t i = 0; i < inst.src_count; i++) {
		text += i == 0 ? ", " : ", ";
		text += OperandToString(inst.src[i]);
	}
	text += MemoryInfoToString(inst.memory);
	text += InputInfoToString(inst.op, inst.input_info);
	return text;
}

std::string ExportTargetKindToString(ExportTargetKind kind) {
	switch (kind) {
		case ExportTargetKind::Null: return "null";
		case ExportTargetKind::Position: return "position";
		case ExportTargetKind::Primitive: return "primitive";
		case ExportTargetKind::Parameter: return "parameter";
		case ExportTargetKind::Mrt: return "mrt";
		case ExportTargetKind::MrtZ: return "mrtz";
		default: return "unknown";
	}
}

std::string ProgramToString(const Program& program) {
	std::string text;
	text += fmt::format("mode={} masks={} fallback={} reason={}\n",
	                    program.dispatcher_fallback ? "dispatcher" : "structured",
	                    program.lane_mask_mode == ShaderLaneMaskMode::NativeWave ? "native-wave"
	                                                                             : "per-invocation",
	                    CFG::FailureKindToString(program.cfg_failure_kind).c_str(),
	                    program.fallback_reason.c_str());
	for (const auto& block: program.blocks) {
		text += fmt::format("block_{} pc=0x{:08x} end=0x{:08x} preds={} succs={} {}\n", block.id,
		                    block.start_pc, block.end_pc,
		                    static_cast<uint32_t>(block.predecessors.size()),
		                    static_cast<uint32_t>(block.successors.size()),
		                    TerminatorToString(block.terminator).c_str());
		for (const auto& inst: block.instructions) {
			text += "  ";
			text += InstructionToString(inst);
			text += "\n";
		}
	}
	return text;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
