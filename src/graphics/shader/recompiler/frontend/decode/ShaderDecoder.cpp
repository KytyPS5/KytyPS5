#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include "graphics/shader/recompiler/frontend/decode/ExportOps.h"
#include "graphics/shader/recompiler/frontend/decode/ImageOps.h"
#include "graphics/shader/recompiler/frontend/decode/MemoryOps.h"
#include "graphics/shader/recompiler/frontend/decode/ScalarAluOps.h"
#include "graphics/shader/recompiler/frontend/decode/VectorAluOps.h"

#include <algorithm>
#include <bit>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

uint32_t FloatBits(float value) {
	return std::bit_cast<uint32_t>(value);
}

void SetError(std::string* error, const char* message) {
	if (error != nullptr) {
		*error = message;
	}
}

bool HasLiteral(const Instruction& inst) {
	return inst.src0.kind == OperandKind::LiteralConstant ||
	       inst.src1.kind == OperandKind::LiteralConstant ||
	       inst.src2.kind == OperandKind::LiteralConstant ||
	       inst.src3.kind == OperandKind::LiteralConstant;
}

bool IsControlFlowBranch(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_BRANCH:
		case Opcode::S_CBRANCH_SCC0:
		case Opcode::S_CBRANCH_SCC1:
		case Opcode::S_CBRANCH_VCCZ:
		case Opcode::S_CBRANCH_VCCNZ:
		case Opcode::S_CBRANCH_EXECZ:
		case Opcode::S_CBRANCH_EXECNZ: return true;
		default: return false;
	}
}

void ApplyLiteral(Operand& operand, uint32_t literal) {
	if (operand.kind == OperandKind::LiteralConstant) {
		operand.value      = literal;
		operand.signed_val = static_cast<int32_t>(literal);
	}
}

std::string RawWordsToString(const Instruction& inst) {
	std::string text;
	for (uint32_t i = 0; i < inst.raw_count; i++) {
		if (i != 0) {
			text += " ";
		}
		text += fmt::format("0x{:08x}", inst.raw[i]);
	}
	return text;
}

std::string FormatBinary(const Instruction& inst) {
	std::string text = fmt::format("0x{:08x}: {} {}", inst.pc, magic_enum::enum_name(inst.opcode),
	                               OperandToString(inst.dst).c_str());
	if (inst.dst2.kind != OperandKind::Unknown) {
		text += ", ";
		text += OperandToString(inst.dst2);
	}
	const Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2, &inst.src3};
	for (uint32_t i = 0; i < inst.src_count && i < 4u; i++) {
		text += ", ";
		text += OperandToString(*sources[i]);
	}
	return text;
}

std::string FormatSources(const Instruction& inst) {
	std::string    text = fmt::format("0x{:08x}: {}", inst.pc, magic_enum::enum_name(inst.opcode));
	const Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2, &inst.src3};
	for (uint32_t i = 0; i < inst.src_count && i < 4u; i++) {
		text += i == 0 ? " " : ", ";
		text += OperandToString(*sources[i]);
	}
	return text;
}

std::string FormatMemory(const Instruction& inst) {
	std::string text = fmt::format("0x{:08x}: {} {}", inst.pc, magic_enum::enum_name(inst.opcode),
	                               OperandToString(inst.dst).c_str());
	const Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2, &inst.src3};
	for (uint32_t i = 0; i < inst.src_count && i < 4u; i++) {
		text += ", ";
		text += OperandToString(*sources[i]);
	}
	text += fmt::format(" ; offset={} offset2={} dwords={} bits={} dfmt={} nfmt={} signed={} "
	                    "typed={} formatted={} segment={} glc={} slc={} idxen={} offen={}",
	                    inst.offset, inst.secondary_offset, inst.data_dwords, inst.data_bits,
	                    inst.data_format, inst.number_format, inst.data_signed ? 1u : 0u,
	                    inst.typed ? 1u : 0u, inst.formatted ? 1u : 0u, inst.memory_segment,
	                    inst.glc ? 1u : 0u, inst.slc ? 1u : 0u, inst.idxen ? 1u : 0u,
	                    inst.offen ? 1u : 0u);
	return text;
}

std::string WithUnsupportedReason(const Instruction& inst, const std::string& text) {
	if (inst.unsupported_reason.empty()) {
		return text;
	}
	return text + fmt::format(" ; family={} opcode=0x{:02x} raw=[{}] reason={}",
	                          magic_enum::enum_name(inst.family), inst.opcode_id,
	                          RawWordsToString(inst).c_str(), inst.unsupported_reason);
}

void AppendFlag(std::string* text, bool* first, uint32_t flags, uint32_t flag, const char* name) {
	if ((flags & flag) == 0) {
		return;
	}
	if (!*first) {
		*text += "|";
	}
	*text += name;
	*first = false;
}

std::string ImageSampleFlagsToString(uint32_t flags) {
	if (flags == 0) {
		return "none";
	}
	std::string text;
	bool        first = true;
	AppendFlag(&text, &first, flags, ImageSampleFlagLod, "lod");
	AppendFlag(&text, &first, flags, ImageSampleFlagBias, "bias");
	AppendFlag(&text, &first, flags, ImageSampleFlagDerivative, "derivative");
	AppendFlag(&text, &first, flags, ImageSampleFlagCompare, "compare");
	AppendFlag(&text, &first, flags, ImageSampleFlagOffset, "offset");
	AppendFlag(&text, &first, flags, ImageSampleFlagLevelZero, "level_zero");
	AppendFlag(&text, &first, flags, ImageSampleFlagLodClamp, "lod_clamp");
	AppendFlag(&text, &first, flags, ImageSampleFlagA16, "a16");
	AppendFlag(&text, &first, flags, ImageSampleFlagCd, "cd");
	AppendFlag(&text, &first, flags, ImageSampleFlagGatherHorizontal, "gather_horizontal");
	AppendFlag(&text, &first, flags, ImageSampleFlagAdjust, "adjust");
	return text;
}

std::string FormatMimg(const Instruction& inst) {
	const char* sample_name =
	    inst.opcode == Opcode::IMAGE_SAMPLE ? MimgSampleOpcodeName(inst.opcode_id) : nullptr;
	const std::string_view name =
	    sample_name != nullptr ? sample_name : magic_enum::enum_name(inst.opcode);
	std::string text =
	    fmt::format("0x{:08x}: {} {}, {}, {}, {} ; dmask=0x{:x} image_dim={}", inst.pc, name,
	                OperandToString(inst.dst).c_str(), OperandToString(inst.src0).c_str(),
	                OperandToString(inst.src1).c_str(), OperandToString(inst.src2).c_str(),
	                inst.dmask, ImageDimensionToString(inst.image_dimension));
	switch (inst.opcode) {
		case Opcode::IMAGE_SAMPLE:
		case Opcode::IMAGE_GATHER4_LZ:
		case Opcode::IMAGE_GATHER4_C:
		case Opcode::IMAGE_GATHER4_C_LZ:
		case Opcode::IMAGE_GATHER4_LZ_O:
		case Opcode::IMAGE_GATHER4_C_O:
		case Opcode::IMAGE_GATHER4_C_LZ_O:
		case Opcode::IMAGE_GATHER4H:
			text += fmt::format(" sample_flags={} addr_components={}",
			                    ImageSampleFlagsToString(inst.image_sample_flags).c_str(),
			                    inst.image_address_components);
			break;
		default: break;
	}
	if (inst.image_nsa_dwords != 0) {
		text += fmt::format(" nsa_dwords={} nsa_addr=", inst.image_nsa_dwords);
		const auto count =
		    std::min<uint32_t>(inst.image_nsa_dwords * 4u, MaxImageNsaAddressComponents);
		for (uint32_t i = 0; i < count; i++) {
			if (i != 0) {
				text += ",";
			}
			text += fmt::format("{}", inst.image_nsa_addr[i]);
		}
	}
	return text;
}

std::string FormatExp(const Instruction& inst) {
	std::string text = fmt::format("0x{:08x}: exp target=0x{:02x} en=0x{:x} done={} compr={} vm={}",
	                               inst.pc, inst.exp.target, inst.exp.en, inst.exp.done ? 1u : 0u,
	                               inst.exp.compr ? 1u : 0u, inst.exp.vm ? 1u : 0u);
	const Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2, &inst.src3};
	for (uint32_t i = 0; i < inst.src_count && i < 4u; i++) {
		text += i == 0 ? " " : ", ";
		text += OperandToString(*sources[i]);
	}
	return text;
}

} // namespace

const char* ImageDimensionToString(ImageDimension dimension) {
	switch (dimension) {
		case ImageDimension::Dim1D: return "1d";
		case ImageDimension::Dim1DArray: return "1d_array";
		case ImageDimension::Dim2D: return "2d";
		case ImageDimension::Dim3D: return "3d";
		case ImageDimension::Dim2DArray: return "2d_array";
		case ImageDimension::Dim2DMsaa: return "2d_msaa";
		case ImageDimension::Dim2DMsaaArray: return "2d_msaa_array";
		default: return "unknown";
	}
}

bool DecodeScalarSource(uint32_t code, uint32_t pc, Operand& operand, std::string* error) {
	operand = {};

	if (code <= 105u) {
		operand.kind = OperandKind::Sgpr;
		operand.reg  = code;
		return true;
	}
	if (code >= 128u && code <= 192u) {
		operand.kind       = OperandKind::IntegerInlineConstant;
		operand.signed_val = static_cast<int32_t>(code - 128u);
		operand.value      = static_cast<uint32_t>(operand.signed_val);
		return true;
	}
	if (code >= 193u && code <= 208u) {
		operand.kind       = OperandKind::IntegerInlineConstant;
		operand.signed_val = 192 - static_cast<int32_t>(code);
		operand.value      = static_cast<uint32_t>(operand.signed_val);
		return true;
	}
	if (code >= 240u && code <= 247u) {
		constexpr float values[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
		operand.kind             = OperandKind::FloatInlineConstant;
		operand.float_val        = values[code - 240u];
		operand.value            = FloatBits(operand.float_val);
		return true;
	}
	if (code >= 256u && code <= 511u) {
		DecodeVectorGpr(code - 256u, operand);
		return true;
	}

	switch (code) {
		case 106u: operand.kind = OperandKind::VccLo; return true;
		case 107u: operand.kind = OperandKind::VccHi; return true;
		case 124u: operand.kind = OperandKind::M0; return true;
		case 125u: operand.kind = OperandKind::Null; return true;
		case 126u: operand.kind = OperandKind::ExecLo; return true;
		case 127u: operand.kind = OperandKind::ExecHi; return true;
		case 239u: operand.kind = OperandKind::PopsExitingWaveId; return true;
		case 248u:
			operand.kind      = OperandKind::FloatInlineConstant;
			operand.float_val = 0.15915494309189535f;
			operand.value     = FloatBits(operand.float_val);
			return true;
		case 251u: operand.kind = OperandKind::VccZ; return true;
		case 252u: operand.kind = OperandKind::ExecZ; return true;
		case 253u: operand.kind = OperandKind::Scc; return true;
		case 255u: operand.kind = OperandKind::LiteralConstant; return true;
		default:
			if (error != nullptr) {
				*error = fmt::format("unsupported scalar source operand 0x{:08x} at pc 0x{:08x}",
				                     code, pc);
			}
			return false;
	}
}

bool DecodeScalarDestination(uint32_t code, uint32_t pc, Operand& operand, std::string* error) {
	operand = {};

	if (code <= 105u) {
		operand.kind = OperandKind::Sgpr;
		operand.reg  = code;
		return true;
	}

	switch (code) {
		case 106u: operand.kind = OperandKind::VccLo; return true;
		case 107u: operand.kind = OperandKind::VccHi; return true;
		case 124u: operand.kind = OperandKind::M0; return true;
		case 125u: operand.kind = OperandKind::Null; return true;
		case 126u: operand.kind = OperandKind::ExecLo; return true;
		case 127u: operand.kind = OperandKind::ExecHi; return true;
		default:
			if (error != nullptr) {
				*error = fmt::format(
				    "unsupported scalar destination operand 0x{:08x} at pc 0x{:08x}", code, pc);
			}
			return false;
	}
}

void DecodeVectorGpr(uint32_t reg, Operand& operand) {
	operand      = {};
	operand.kind = OperandKind::Vgpr;
	operand.reg  = reg;
}

void ReadLiteralOperands(std::span<const uint32_t> code, uint32_t word_index, Instruction& inst) {
	if (!HasLiteral(inst)) {
		return;
	}

	const auto literal = code[word_index + inst.word_count];
	ApplyLiteral(inst.src0, literal);
	ApplyLiteral(inst.src1, literal);
	ApplyLiteral(inst.src2, literal);
	ApplyLiteral(inst.src3, literal);
	inst.word_count++;
	SetRawWords(inst, code, word_index, inst.word_count);
}

void SetRawWords(Instruction& inst, std::span<const uint32_t> code, uint32_t word_index,
                 uint32_t word_count) {
	inst.word_count = word_count;
	inst.raw_count  = word_count;
	for (uint32_t i = 0; i < inst.raw_count; i++) {
		inst.raw[i] = code[word_index + i];
	}
}

void SetUnsupported(Instruction& inst, Family family, uint32_t opcode_id, const char* reason) {
	inst.opcode             = Opcode::UNSUPPORTED;
	inst.family             = family;
	inst.opcode_id          = opcode_id;
	inst.unsupported_reason = reason;
}

Family GetInstructionFamily(uint32_t word) {
	if ((word & 0x80000000u) == 0u) {
		switch ((word >> 25u) & 0x3fu) {
			case 0x3eu: return Family::VOPC;
			case 0x3fu: return Family::VOP1;
			default: return Family::VOP2;
		}
	}
	if ((word & 0xc0000000u) == 0x80000000u) {
		const auto opcode = (word >> 23u) & 0x7fu;
		switch (opcode) {
			case 0x7du: return Family::SOP1;
			case 0x7eu: return Family::SOPC;
			case 0x7fu: return Family::SOPP;
			default: return opcode >= 0x60u ? Family::SOPK : Family::SOP2;
		}
	}

	switch (word >> 26u) {
		case 0x32u: return Family::VINTRP;
		case 0x33u: return Family::VOP3P;
		case 0x35u: return Family::VOP3;
		case 0x36u: return Family::DS;
		case 0x37u: return Family::FLAT;
		case 0x38u: return Family::MUBUF;
		case 0x3au: return Family::MTBUF;
		case 0x3cu: return Family::MIMG;
		case 0x3du: return Family::SMEM;
		case 0x3eu: return Family::EXP;
		default: return Family::Unknown;
	}
}

bool DecodeInstruction(std::span<const uint32_t> code, uint32_t word_index, Instruction& inst,
                       std::string* error) {
	const uint32_t pc = word_index * sizeof(uint32_t);
	switch (GetInstructionFamily(code[word_index])) {
		case Family::SOP1: return DecodeSop1(pc, code, word_index, inst, error);
		case Family::SOP2: return DecodeSop2(pc, code, word_index, inst, error);
		case Family::SOPK: return DecodeSopk(pc, code, word_index, inst, error);
		case Family::SOPC: return DecodeSopc(pc, code, word_index, inst, error);
		case Family::SOPP: DecodeSopp(pc, code, word_index, inst); return true;
		case Family::VOP1: return DecodeVop1(pc, code, word_index, inst, error);
		case Family::VOP2: return DecodeVop2(pc, code, word_index, inst, error);
		case Family::VOP3: return DecodeVop3(pc, code, word_index, inst, error);
		case Family::VOP3P: return DecodeVop3p(pc, code, word_index, inst, error);
		case Family::VOPC: return DecodeVopc(pc, code, word_index, inst, error);
		case Family::VINTRP: DecodeVintrp(pc, code, word_index, inst); return true;
		case Family::SMEM: DecodeSmem(pc, code, word_index, inst); return true;
		case Family::MUBUF: DecodeMubuf(pc, code, word_index, inst); return true;
		case Family::MTBUF: DecodeMtbuf(pc, code, word_index, inst); return true;
		case Family::FLAT: DecodeFlat(pc, code, word_index, inst); return true;
		case Family::DS: DecodeDs(pc, code, word_index, inst); return true;
		case Family::MIMG: DecodeMimg(pc, code, word_index, inst); return true;
		case Family::EXP: DecodeExp(pc, code, word_index, inst); return true;
		default:
			if (error != nullptr) {
				*error =
				    fmt::format("unknown RDNA2 instruction family at pc 0x{:08x}, raw=0x{:08x}", pc,
				                code[word_index]);
			}
			return false;
	}
}

bool DecodeProgram(std::span<const uint32_t> code, Program& program, std::string* error) {
	program.instructions.clear();
	program.instructions.reserve(code.size());
	program.code = code;

	std::vector<bool> branch_targets;
	for (uint32_t word_index = 0; word_index < code.size();) {
		program.instructions.emplace_back();
		if (!DecodeInstruction(code, word_index, program.instructions.back(), error)) {
			program.instructions.pop_back();
			return false;
		}

		const auto& inst = program.instructions.back();
		word_index += inst.word_count;

		if (IsControlFlowBranch(inst.opcode)) {
			const auto target_index = inst.branch_target / sizeof(uint32_t);
			if (branch_targets.empty()) {
				branch_targets.resize(code.size());
			}
			branch_targets[target_index] = true;
		}
		if (inst.opcode == Opcode::S_ENDPGM &&
		    (word_index >= code.size() || branch_targets.empty() || !branch_targets[word_index])) {
			return true;
		}
	}

	SetError(error, "shader decode reached the code boundary before S_ENDPGM");
	return false;
}

std::string OperandToString(const Operand& operand) {
	std::string text;
	switch (operand.kind) {
		case OperandKind::LiteralConstant: text = fmt::format("0x{:08x}", operand.value); break;
		case OperandKind::IntegerInlineConstant:
			text = fmt::format("{}", operand.signed_val);
			break;
		case OperandKind::FloatInlineConstant: text = fmt::format("{:f}", operand.float_val); break;
		case OperandKind::Sgpr: text = fmt::format("s{}", operand.reg); break;
		case OperandKind::Vgpr: text = fmt::format("v{}", operand.reg); break;
		case OperandKind::VccLo: text = "vcc_lo"; break;
		case OperandKind::VccHi: text = "vcc_hi"; break;
		case OperandKind::VccZ: text = "vccz"; break;
		case OperandKind::ExecLo: text = "exec_lo"; break;
		case OperandKind::ExecHi: text = "exec_hi"; break;
		case OperandKind::ExecZ: text = "execz"; break;
		case OperandKind::Scc: text = "scc"; break;
		case OperandKind::M0: text = "m0"; break;
		case OperandKind::PopsExitingWaveId: text = "pops_exiting_wave_id"; break;
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

std::string InstructionToString(const Instruction& inst) {
	if (inst.opcode == Opcode::UNSUPPORTED) {
		return fmt::format("0x{:08x}: unsupported family={} opcode=0x{:02x} raw=[{}] reason={}",
		                   inst.pc, magic_enum::enum_name(inst.family), inst.opcode_id,
		                   RawWordsToString(inst).c_str(), inst.unsupported_reason);
	}
	if (inst.family == Family::SOPC) {
		return WithUnsupportedReason(inst, FormatSources(inst));
	}

	switch (inst.opcode) {
		case Opcode::S_MOV_B32:
		case Opcode::S_MOV_B64:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: {} {}, {}", inst.pc,
			                                               magic_enum::enum_name(inst.opcode),
			                                               OperandToString(inst.dst).c_str(),
			                                               OperandToString(inst.src0).c_str()));
		case Opcode::S_ABS_I32:
		case Opcode::S_BREV_B32:
		case Opcode::S_BCNT1_I32_B32:
		case Opcode::S_FF1_I32_B32:
		case Opcode::S_FF1_I32_B64:
		case Opcode::S_NOT_B64:
		case Opcode::S_WQM_B64:
		case Opcode::S_QUADMASK_B64:
		case Opcode::S_AND_SAVEEXEC_B32:
		case Opcode::S_ANDN1_SAVEEXEC_B32:
		case Opcode::S_AND_SAVEEXEC_B64:
		case Opcode::S_ORN2_SAVEEXEC_B64:
		case Opcode::S_ANDN1_SAVEEXEC_B64:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: {} {}, {}", inst.pc,
			                                               magic_enum::enum_name(inst.opcode),
			                                               OperandToString(inst.dst).c_str(),
			                                               OperandToString(inst.src0).c_str()));
		case Opcode::S_GETPC_B64:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_getpc_b64 {}", inst.pc,
			                                               OperandToString(inst.dst).c_str()));
		case Opcode::S_SETPC_B64:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_setpc_b64 {}", inst.pc,
			                                               OperandToString(inst.src0).c_str()));
		case Opcode::S_SETREG_B32:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_setreg_b32 {}, {}", inst.pc,
			                                               OperandToString(inst.src0).c_str(),
			                                               OperandToString(inst.src1).c_str()));
		case Opcode::S_NOP:
		case Opcode::S_WAITCNT:
		case Opcode::S_WAITCNT_DEPCTR:
		case Opcode::S_SLEEP:
		case Opcode::S_TRAP:
		case Opcode::S_SENDMSG:
		case Opcode::S_TTRACEDATA:
		case Opcode::S_INST_PREFETCH:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: {} {}", inst.pc,
			                                               magic_enum::enum_name(inst.opcode),
			                                               OperandToString(inst.src0).c_str()));
		case Opcode::S_BARRIER:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_barrier", inst.pc));
		case Opcode::V_NOP:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: v_nop", inst.pc));
		case Opcode::S_ENDPGM:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_endpgm", inst.pc));
		case Opcode::S_BRANCH:
		case Opcode::S_CBRANCH_SCC0:
		case Opcode::S_CBRANCH_SCC1:
		case Opcode::S_CBRANCH_VCCZ:
		case Opcode::S_CBRANCH_VCCNZ:
		case Opcode::S_CBRANCH_EXECZ:
		case Opcode::S_CBRANCH_EXECNZ:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: {} 0x{:08x}", inst.pc,
			                                               magic_enum::enum_name(inst.opcode),
			                                               inst.branch_target));
		case Opcode::EXP: return WithUnsupportedReason(inst, FormatExp(inst));
		case Opcode::IMAGE_SAMPLE:
		case Opcode::IMAGE_STORE:
		case Opcode::IMAGE_STORE_MIP:
		case Opcode::IMAGE_ATOMIC_ADD:
		case Opcode::IMAGE_ATOMIC_UMIN:
		case Opcode::IMAGE_ATOMIC_UMAX:
		case Opcode::IMAGE_ATOMIC_AND:
		case Opcode::IMAGE_ATOMIC_OR:
		case Opcode::IMAGE_ATOMIC_XOR:
		case Opcode::IMAGE_LOAD:
		case Opcode::IMAGE_LOAD_MIP:
		case Opcode::IMAGE_GET_RESINFO:
		case Opcode::IMAGE_GET_LOD:
		case Opcode::IMAGE_GATHER4_LZ:
		case Opcode::IMAGE_GATHER4_C:
		case Opcode::IMAGE_GATHER4_C_LZ:
		case Opcode::IMAGE_GATHER4_LZ_O:
		case Opcode::IMAGE_GATHER4_C_O:
		case Opcode::IMAGE_GATHER4_C_LZ_O:
		case Opcode::IMAGE_GATHER4H: return WithUnsupportedReason(inst, FormatMimg(inst));
		case Opcode::S_LOAD_DWORD:
		case Opcode::S_LOAD_DWORDX2:
		case Opcode::S_LOAD_DWORDX4:
		case Opcode::S_LOAD_DWORDX8:
		case Opcode::S_LOAD_DWORDX16:
		case Opcode::S_BUFFER_LOAD_DWORD:
		case Opcode::S_BUFFER_LOAD_DWORDX2:
		case Opcode::S_BUFFER_LOAD_DWORDX4:
		case Opcode::S_BUFFER_LOAD_DWORDX8:
		case Opcode::S_BUFFER_LOAD_DWORDX16:
		case Opcode::BUFFER_LOAD_FORMAT_X:
		case Opcode::BUFFER_LOAD_FORMAT_XY:
		case Opcode::BUFFER_LOAD_FORMAT_XYZ:
		case Opcode::BUFFER_LOAD_FORMAT_XYZW:
		case Opcode::BUFFER_STORE_FORMAT_X:
		case Opcode::BUFFER_STORE_FORMAT_XY:
		case Opcode::BUFFER_STORE_FORMAT_XYZ:
		case Opcode::BUFFER_STORE_FORMAT_XYZW:
		case Opcode::BUFFER_LOAD_UBYTE:
		case Opcode::BUFFER_LOAD_USHORT:
		case Opcode::BUFFER_LOAD_DWORD:
		case Opcode::BUFFER_LOAD_DWORDX2:
		case Opcode::BUFFER_LOAD_DWORDX3:
		case Opcode::BUFFER_LOAD_DWORDX4:
		case Opcode::BUFFER_STORE_BYTE:
		case Opcode::BUFFER_STORE_SHORT:
		case Opcode::BUFFER_STORE_DWORD:
		case Opcode::BUFFER_STORE_DWORDX2:
		case Opcode::BUFFER_STORE_DWORDX3:
		case Opcode::BUFFER_STORE_DWORDX4:
		case Opcode::TBUFFER_LOAD_FORMAT_X:
		case Opcode::TBUFFER_LOAD_FORMAT_XY:
		case Opcode::TBUFFER_LOAD_FORMAT_XYZ:
		case Opcode::TBUFFER_LOAD_FORMAT_XYZW:
		case Opcode::TBUFFER_STORE_FORMAT_X:
		case Opcode::TBUFFER_STORE_FORMAT_XY:
		case Opcode::TBUFFER_STORE_FORMAT_XYZ:
		case Opcode::TBUFFER_STORE_FORMAT_XYZW:
		case Opcode::BUFFER_ATOMIC_SWAP:
		case Opcode::BUFFER_ATOMIC_ADD:
		case Opcode::BUFFER_ATOMIC_SUB:
		case Opcode::BUFFER_ATOMIC_SMIN:
		case Opcode::BUFFER_ATOMIC_UMIN:
		case Opcode::BUFFER_ATOMIC_SMAX:
		case Opcode::BUFFER_ATOMIC_UMAX:
		case Opcode::BUFFER_ATOMIC_AND:
		case Opcode::BUFFER_ATOMIC_OR:
		case Opcode::BUFFER_ATOMIC_XOR:
		case Opcode::BUFFER_ATOMIC_FMIN:
		case Opcode::BUFFER_ATOMIC_FMAX:
		case Opcode::BUFFER_LOAD_SBYTE:
		case Opcode::BUFFER_LOAD_SSHORT:
		case Opcode::FLAT_LOAD_UBYTE:
		case Opcode::FLAT_LOAD_SBYTE:
		case Opcode::FLAT_LOAD_USHORT:
		case Opcode::FLAT_LOAD_SSHORT:
		case Opcode::FLAT_LOAD_DWORD:
		case Opcode::FLAT_LOAD_DWORDX2:
		case Opcode::FLAT_LOAD_DWORDX3:
		case Opcode::FLAT_LOAD_DWORDX4:
		case Opcode::FLAT_STORE_BYTE:
		case Opcode::FLAT_STORE_SHORT:
		case Opcode::FLAT_STORE_DWORD:
		case Opcode::FLAT_STORE_DWORDX2:
		case Opcode::FLAT_STORE_DWORDX3:
		case Opcode::FLAT_STORE_DWORDX4:
		case Opcode::DS_ADD_U32:
		case Opcode::DS_ADD_RTN_U32:
		case Opcode::DS_SUB_U32:
		case Opcode::DS_SUB_RTN_U32:
		case Opcode::DS_MIN_I32:
		case Opcode::DS_MIN_RTN_I32:
		case Opcode::DS_MAX_I32:
		case Opcode::DS_MAX_RTN_I32:
		case Opcode::DS_MIN_U32:
		case Opcode::DS_MIN_RTN_U32:
		case Opcode::DS_MAX_U32:
		case Opcode::DS_MAX_RTN_U32:
		case Opcode::DS_AND_B32:
		case Opcode::DS_AND_RTN_B32:
		case Opcode::DS_OR_B32:
		case Opcode::DS_OR_RTN_B32:
		case Opcode::DS_XOR_B32:
		case Opcode::DS_XOR_RTN_B32:
		case Opcode::DS_WRXCHG_RTN_B32:
		case Opcode::DS_MIN_F32:
		case Opcode::DS_MAX_F32:
		case Opcode::DS_SWIZZLE_B32:
		case Opcode::DS_READ_I8:
		case Opcode::DS_READ_U8:
		case Opcode::DS_READ_I16:
		case Opcode::DS_READ_U16:
		case Opcode::DS_READ2_B32:
		case Opcode::DS_READ2ST64_B32:
		case Opcode::DS_READ_B32:
		case Opcode::DS_READ_B64:
		case Opcode::DS_READ2_B64:
		case Opcode::DS_READ2ST64_B64:
		case Opcode::DS_READ_B96:
		case Opcode::DS_READ_B128:
		case Opcode::DS_WRITE_B8:
		case Opcode::DS_WRITE_B16:
		case Opcode::DS_WRITE2_B32:
		case Opcode::DS_WRITE2ST64_B32:
		case Opcode::DS_WRITE2_B64:
		case Opcode::DS_WRITE2ST64_B64:
		case Opcode::DS_WRITE_B32:
		case Opcode::DS_WRITE_B64:
		case Opcode::DS_WRITE_B96:
		case Opcode::DS_WRITE_B128:
		case Opcode::DS_WRITE_ADDTID_B32:
		case Opcode::DS_READ_ADDTID_B32: return WithUnsupportedReason(inst, FormatMemory(inst));
		default: return WithUnsupportedReason(inst, FormatBinary(inst));
	}
}

std::string ProgramToString(const Program& program) {
	std::string text;
	for (const auto& inst: program.instructions) {
		text += InstructionToString(inst);
		text += "\n";
	}
	return text;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
