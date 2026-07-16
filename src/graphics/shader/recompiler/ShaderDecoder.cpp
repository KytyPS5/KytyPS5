#include "graphics/shader/recompiler/ShaderDecoder.h"

#include "graphics/shader/recompiler/ExportOps.h"
#include "graphics/shader/recompiler/ImageOps.h"
#include "graphics/shader/recompiler/MemoryOps.h"
#include "graphics/shader/recompiler/ScalarAluOps.h"
#include "graphics/shader/recompiler/VectorAluOps.h"

#include <algorithm>
#include <bit>
#include <fmt/format.h>
#include <set>

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
		case Opcode::SBranch:
		case Opcode::SCbranchScc0:
		case Opcode::SCbranchScc1:
		case Opcode::SCbranchVccz:
		case Opcode::SCbranchVccnz:
		case Opcode::SCbranchExecz:
		case Opcode::SCbranchExecnz: return true;
		default: return false;
	}
}

void ApplyLiteral(Operand* operand, uint32_t literal) {
	if (operand->kind == OperandKind::LiteralConstant) {
		operand->value      = literal;
		operand->signed_val = static_cast<int32_t>(literal);
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
	std::string text = fmt::format("0x{:08x}: {} {}", inst.pc, OpcodeToString(inst.opcode).c_str(),
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

std::string FormatMemory(const Instruction& inst) {
	std::string text = fmt::format("0x{:08x}: {} {}", inst.pc, OpcodeToString(inst.opcode).c_str(),
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
	                          FamilyToString(inst.family).c_str(), inst.opcode_id,
	                          RawWordsToString(inst).c_str(), inst.unsupported_reason.c_str());
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
	return text;
}

const char* ImageDimensionToString(ImageDimension dimension) {
	switch (dimension) {
		case ImageDimension::Dim2D: return "2d";
		case ImageDimension::Dim3D: return "3d";
		case ImageDimension::Dim2DArray: return "2d_array";
		default: return "unknown";
	}
}

std::string FormatMimg(const Instruction& inst) {
	const char* sample_name =
	    inst.opcode == Opcode::ImageSample ? MimgSampleOpcodeName(inst.opcode_id) : nullptr;
	const std::string name =
	    sample_name != nullptr ? std::string(sample_name) : OpcodeToString(inst.opcode);
	std::string text = fmt::format(
	    "0x{:08x}: {} {}, {}, {}, {} ; dmask=0x{:x} image_dim={}", inst.pc, name.c_str(),
	    OperandToString(inst.dst).c_str(), OperandToString(inst.src0).c_str(),
	    OperandToString(inst.src1).c_str(), OperandToString(inst.src2).c_str(), inst.dmask,
	    ImageDimensionToString(inst.image_dimension));
	switch (inst.opcode) {
		case Opcode::ImageSample:
		case Opcode::ImageGather4Lz:
		case Opcode::ImageGather4C:
		case Opcode::ImageGather4CLz:
		case Opcode::ImageGather4LzO:
		case Opcode::ImageGather4CO:
		case Opcode::ImageGather4CLzO:
		case Opcode::ImageGather4H:
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

bool DecodeScalarSource(uint32_t code, uint32_t pc, Operand* operand, std::string* error) {
	if (operand == nullptr) {
		SetError(error, "internal decoder error: null source operand");
		return false;
	}

	*operand = {};

	if (code <= 105u) {
		operand->kind = OperandKind::Sgpr;
		operand->reg  = code;
		return true;
	}
	if (code >= 128u && code <= 192u) {
		operand->kind       = OperandKind::IntegerInlineConstant;
		operand->signed_val = static_cast<int32_t>(code - 128u);
		operand->value      = static_cast<uint32_t>(operand->signed_val);
		return true;
	}
	if (code >= 193u && code <= 208u) {
		operand->kind       = OperandKind::IntegerInlineConstant;
		operand->signed_val = 192 - static_cast<int32_t>(code);
		operand->value      = static_cast<uint32_t>(operand->signed_val);
		return true;
	}
	if (code >= 240u && code <= 247u) {
		constexpr float values[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
		operand->kind            = OperandKind::FloatInlineConstant;
		operand->float_val       = values[code - 240u];
		operand->value           = FloatBits(operand->float_val);
		return true;
	}
	if (code >= 256u && code <= 511u) {
		return DecodeVectorGpr(code - 256u, operand, error);
	}

	switch (code) {
		case 106u: operand->kind = OperandKind::VccLo; return true;
		case 107u: operand->kind = OperandKind::VccHi; return true;
		case 124u: operand->kind = OperandKind::M0; return true;
		case 125u: operand->kind = OperandKind::Null; return true;
		case 126u: operand->kind = OperandKind::ExecLo; return true;
		case 127u: operand->kind = OperandKind::ExecHi; return true;
		case 239u: operand->kind = OperandKind::PopsExitingWaveId; return true;
		case 248u:
			operand->kind      = OperandKind::FloatInlineConstant;
			operand->float_val = 0.15915494309189535f;
			operand->value     = FloatBits(operand->float_val);
			return true;
		case 251u: operand->kind = OperandKind::VccZ; return true;
		case 252u: operand->kind = OperandKind::ExecZ; return true;
		case 253u: operand->kind = OperandKind::Scc; return true;
		case 255u: operand->kind = OperandKind::LiteralConstant; return true;
		default:
			if (error != nullptr) {
				*error = fmt::format("unsupported scalar source operand 0x{:08x} at pc 0x{:08x}",
				                     code, pc);
			}
			return false;
	}
}

bool DecodeScalarDestination(uint32_t code, uint32_t pc, Operand* operand, std::string* error) {
	if (operand == nullptr) {
		SetError(error, "internal decoder error: null destination operand");
		return false;
	}

	*operand = {};

	if (code <= 105u) {
		operand->kind = OperandKind::Sgpr;
		operand->reg  = code;
		return true;
	}

	switch (code) {
		case 106u: operand->kind = OperandKind::VccLo; return true;
		case 107u: operand->kind = OperandKind::VccHi; return true;
		case 124u: operand->kind = OperandKind::M0; return true;
		case 125u: operand->kind = OperandKind::Null; return true;
		case 126u: operand->kind = OperandKind::ExecLo; return true;
		case 127u: operand->kind = OperandKind::ExecHi; return true;
		default:
			if (error != nullptr) {
				*error = fmt::format(
				    "unsupported scalar destination operand 0x{:08x} at pc 0x{:08x}", code, pc);
			}
			return false;
	}
}

bool DecodeVectorGpr(uint32_t reg, Operand* operand, std::string* error) {
	if (operand == nullptr) {
		SetError(error, "internal decoder error: null VGPR operand");
		return false;
	}
	if (reg > 255u) {
		SetError(error, "VGPR index is out of range");
		return false;
	}
	*operand      = {};
	operand->kind = OperandKind::Vgpr;
	operand->reg  = reg;
	return true;
}

bool ReadLiteralOperands(std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                         std::string* error) {
	if (!HasLiteral(*inst)) {
		return true;
	}
	if (word_index + inst->word_count >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("missing literal constant at pc 0x{:08x}", inst->pc);
		}
		return false;
	}

	const auto literal = code[word_index + inst->word_count];
	ApplyLiteral(&inst->src0, literal);
	ApplyLiteral(&inst->src1, literal);
	ApplyLiteral(&inst->src2, literal);
	ApplyLiteral(&inst->src3, literal);
	inst->word_count++;
	SetRawWords(inst, code, word_index, inst->word_count);
	return true;
}

void SetRawWords(Instruction* inst, std::span<const uint32_t> code, uint32_t word_index,
                 uint32_t word_count) {
	inst->word_count = word_count;
	inst->raw_count  = std::min<uint32_t>(word_count, MaxInstructionRawWords);
	for (uint32_t i = 0; i < inst->raw_count; i++) {
		inst->raw[i] = code[word_index + i];
	}
}

void SetUnsupported(Instruction* inst, Family family, uint32_t opcode_id, const char* reason) {
	inst->opcode             = Opcode::Unsupported;
	inst->family             = family;
	inst->opcode_id          = opcode_id;
	inst->unsupported_reason = reason;
}

bool DecodeProgram(std::span<const uint32_t> code, Program* program, std::string* error) {
	if (code.empty() || code.size() > UINT32_MAX / sizeof(uint32_t) || program == nullptr) {
		SetError(error, "invalid shader decoder input");
		return false;
	}

	program->instructions.clear();
	program->code = code;

	std::set<uint32_t> branch_targets;
	for (uint32_t word_index = 0; word_index < code.size();) {
		const uint32_t pc   = word_index * 4u;
		const uint32_t word = code[word_index];

		Instruction inst;
		bool        ok = false;
		if ((word & 0x80000000u) == 0u) {
			ok = DecodeVop2(pc, code, word_index, &inst, error);
		} else if ((word & 0xc0000000u) == 0x80000000u) {
			const auto opcode = (word >> 23u) & 0x7fu;
			switch (opcode) {
				case 0x7du: ok = DecodeSop1(pc, code, word_index, &inst, error); break;
				case 0x7eu: ok = DecodeSopc(pc, code, word_index, &inst, error); break;
				case 0x7fu: ok = DecodeSopp(pc, code, word_index, &inst, error); break;
				default:
					ok = opcode >= 0x60u ? DecodeSopk(pc, code, word_index, &inst, error)
					                     : DecodeSop2(pc, code, word_index, &inst, error);
					break;
			}
		} else {
			switch (word >> 26u) {
				case 0x32u: ok = DecodeVintrp(pc, code, word_index, &inst, error); break;
				case 0x33u: ok = DecodeVop3p(pc, code, word_index, &inst, error); break;
				case 0x35u: ok = DecodeVop3(pc, code, word_index, &inst, error); break;
				case 0x36u: ok = DecodeDs(pc, code, word_index, &inst, error); break;
				case 0x37u: ok = DecodeFlat(pc, code, word_index, &inst, error); break;
				case 0x38u: ok = DecodeMubuf(pc, code, word_index, &inst, error); break;
				case 0x3au: ok = DecodeMtbuf(pc, code, word_index, &inst, error); break;
				case 0x3cu: ok = DecodeMimg(pc, code, word_index, &inst, error); break;
				case 0x3du: ok = DecodeSmem(pc, code, word_index, &inst, error); break;
				case 0x3eu: ok = DecodeExp(pc, code, word_index, &inst, error); break;
				default:
					if (error != nullptr) {
						*error = fmt::format(
						    "unknown RDNA2 instruction family at pc 0x{:08x}, raw=0x{:08x}", pc,
						    word);
					}
					return false;
			}
		}

		if (!ok) {
			return false;
		}

		program->instructions.push_back(inst);
		word_index += inst.word_count;

		if (IsControlFlowBranch(inst.opcode)) {
			branch_targets.insert(inst.branch_target);
		}
		if (inst.opcode == Opcode::SEndpgm &&
		    (word_index >= code.size() || !branch_targets.contains(word_index * 4u))) {
			return true;
		}
	}

	SetError(error, "shader decode reached the code boundary before S_ENDPGM");
	return false;
}

std::string FamilyToString(Family family) {
	switch (family) {
		case Family::SOP1: return "SOP1";
		case Family::SOP2: return "SOP2";
		case Family::SOPK: return "SOPK";
		case Family::SOPC: return "SOPC";
		case Family::SOPP: return "SOPP";
		case Family::VOP1: return "VOP1";
		case Family::VOP2: return "VOP2";
		case Family::VOP3: return "VOP3";
		case Family::VOP3P: return "VOP3P";
		case Family::VOPC: return "VOPC";
		case Family::VINTRP: return "VINTRP";
		case Family::SMEM: return "SMEM";
		case Family::MUBUF: return "MUBUF";
		case Family::MTBUF: return "MTBUF";
		case Family::FLAT: return "FLAT";
		case Family::DS: return "DS";
		case Family::MIMG: return "MIMG";
		case Family::EXP: return "EXP";
		default: return "Unknown";
	}
}

std::string OpcodeToString(Opcode opcode) {
	switch (opcode) {
		case Opcode::SMovB32: return "s_mov_b32";
		case Opcode::SMovB64: return "s_mov_b64";
		case Opcode::SMovkI32: return "s_movk_i32";
		case Opcode::SAbsI32: return "s_abs_i32";
		case Opcode::SBrevB32: return "s_brev_b32";
		case Opcode::SBcnt1I32B32: return "s_bcnt1_i32_b32";
		case Opcode::SBcnt1I32B64: return "s_bcnt1_i32_b64";
		case Opcode::SFf1I32B32: return "s_ff1_i32_b32";
		case Opcode::SFlbitI32B64: return "s_flbit_i32_b64";
		case Opcode::SBitreplicateB64B32: return "s_bitreplicate_b64_b32";
		case Opcode::SGetpcB64: return "s_getpc_b64";
		case Opcode::SSetpcB64: return "s_setpc_b64";
		case Opcode::SAndSaveexecB32: return "s_and_saveexec_b32";
		case Opcode::SAndn1SaveexecB32: return "s_andn1_saveexec_b32";
		case Opcode::SAndSaveexecB64: return "s_and_saveexec_b64";
		case Opcode::SOrn2SaveexecB64: return "s_orn2_saveexec_b64";
		case Opcode::SAndn1SaveexecB64: return "s_andn1_saveexec_b64";
		case Opcode::SNotB32: return "s_not_b32";
		case Opcode::SNotB64: return "s_not_b64";
		case Opcode::SWqmB64: return "s_wqm_b64";
		case Opcode::SAddU32: return "s_add_u32";
		case Opcode::SAddcU32: return "s_addc_u32";
		case Opcode::SSubbU32: return "s_subb_u32";
		case Opcode::SSubU32: return "s_sub_u32";
		case Opcode::SAddI32: return "s_add_i32";
		case Opcode::SSubI32: return "s_sub_i32";
		case Opcode::SBitcmp0B32: return "s_bitcmp0_b32";
		case Opcode::SBitcmp1B32: return "s_bitcmp1_b32";
		case Opcode::SBitset0B32: return "s_bitset0_b32";
		case Opcode::SBitset1B32: return "s_bitset1_b32";
		case Opcode::SMinI32: return "s_min_i32";
		case Opcode::SMaxI32: return "s_max_i32";
		case Opcode::SMinU32: return "s_min_u32";
		case Opcode::SMaxU32: return "s_max_u32";
		case Opcode::SAndB32: return "s_and_b32";
		case Opcode::SAndB64: return "s_and_b64";
		case Opcode::SAndn2B32: return "s_andn2_b32";
		case Opcode::SAndn2B64: return "s_andn2_b64";
		case Opcode::SOrB32: return "s_or_b32";
		case Opcode::SOrB64: return "s_or_b64";
		case Opcode::SOrn2B32: return "s_orn2_b32";
		case Opcode::SOrn2B64: return "s_orn2_b64";
		case Opcode::SXorB32: return "s_xor_b32";
		case Opcode::SXorB64: return "s_xor_b64";
		case Opcode::SNandB32: return "s_nand_b32";
		case Opcode::SNandB64: return "s_nand_b64";
		case Opcode::SNorB32: return "s_nor_b32";
		case Opcode::SNorB64: return "s_nor_b64";
		case Opcode::SXnorB32: return "s_xnor_b32";
		case Opcode::SXnorB64: return "s_xnor_b64";
		case Opcode::SLshlB32: return "s_lshl_b32";
		case Opcode::SLshlB64: return "s_lshl_b64";
		case Opcode::SLshl1AddU32: return "s_lshl1_add_u32";
		case Opcode::SLshl2AddU32: return "s_lshl2_add_u32";
		case Opcode::SLshl3AddU32: return "s_lshl3_add_u32";
		case Opcode::SLshl4AddU32: return "s_lshl4_add_u32";
		case Opcode::SLshrB32: return "s_lshr_b32";
		case Opcode::SLshrB64: return "s_lshr_b64";
		case Opcode::SAshrI32: return "s_ashr_i32";
		case Opcode::SMulI32: return "s_mul_i32";
		case Opcode::SMulHiU32: return "s_mul_hi_u32";
		case Opcode::SMulkI32: return "s_mulk_i32";
		case Opcode::SBfeU32: return "s_bfe_u32";
		case Opcode::SBfeU64: return "s_bfe_u64";
		case Opcode::SBfmB32: return "s_bfm_b32";
		case Opcode::SBfmB64: return "s_bfm_b64";
		case Opcode::SCselectB32: return "s_cselect_b32";
		case Opcode::SCselectB64: return "s_cselect_b64";
		case Opcode::SPackLlB32B16: return "s_pack_ll_b32_b16";
		case Opcode::SPackLhB32B16: return "s_pack_lh_b32_b16";
		case Opcode::SPackHhB32B16: return "s_pack_hh_b32_b16";
		case Opcode::SCmpEqI32: return "s_cmp_eq_i32";
		case Opcode::SCmpLgI32: return "s_cmp_lg_i32";
		case Opcode::SCmpGtI32: return "s_cmp_gt_i32";
		case Opcode::SCmpGeI32: return "s_cmp_ge_i32";
		case Opcode::SCmpLtI32: return "s_cmp_lt_i32";
		case Opcode::SCmpLeI32: return "s_cmp_le_i32";
		case Opcode::SCmpEqU32: return "s_cmp_eq_u32";
		case Opcode::SCmpLgU32: return "s_cmp_lg_u32";
		case Opcode::SCmpGtU32: return "s_cmp_gt_u32";
		case Opcode::SCmpGeU32: return "s_cmp_ge_u32";
		case Opcode::SCmpLtU32: return "s_cmp_lt_u32";
		case Opcode::SCmpLeU32: return "s_cmp_le_u32";
		case Opcode::SCmpLgU64: return "s_cmp_lg_u64";
		case Opcode::VNop: return "v_nop";
		case Opcode::VMovB32: return "v_mov_b32";
		case Opcode::VMovreldB32: return "v_movreld_b32";
		case Opcode::VMovrelsB32: return "v_movrels_b32";
		case Opcode::VReadfirstlaneB32: return "v_readfirstlane_b32";
		case Opcode::VReadlaneB32: return "v_readlane_b32";
		case Opcode::VWritelaneB32: return "v_writelane_b32";
		case Opcode::VPermlane16B32: return "v_permlane16_b32";
		case Opcode::VPermlanex16B32: return "v_permlanex16_b32";
		case Opcode::VCubeidF32: return "v_cubeid_f32";
		case Opcode::VCubescF32: return "v_cubesc_f32";
		case Opcode::VCubetcF32: return "v_cubetc_f32";
		case Opcode::VCubemaF32: return "v_cubema_f32";
		case Opcode::VCndmaskB32: return "v_cndmask_b32";
		case Opcode::VDot2cF32F16: return "v_dot2c_f32_f16";
		case Opcode::VCvtF32I32: return "v_cvt_f32_i32";
		case Opcode::VCvtF32U32: return "v_cvt_f32_u32";
		case Opcode::VCvtU32F32: return "v_cvt_u32_f32";
		case Opcode::VCvtI32F32: return "v_cvt_i32_f32";
		case Opcode::VCvtF16F32: return "v_cvt_f16_f32";
		case Opcode::VCvtF32F16: return "v_cvt_f32_f16";
		case Opcode::VCvtF16U16: return "v_cvt_f16_u16";
		case Opcode::VCvtU16F16: return "v_cvt_u16_f16";
		case Opcode::VCvtF16I16: return "v_cvt_f16_i16";
		case Opcode::VCvtI16F16: return "v_cvt_i16_f16";
		case Opcode::VCvtRpiI32F32: return "v_cvt_rpi_i32_f32";
		case Opcode::VCvtFlrI32F32: return "v_cvt_flr_i32_f32";
		case Opcode::VCvtOffF32I4: return "v_cvt_off_f32_i4";
		case Opcode::VCvtF32Ubyte0: return "v_cvt_f32_ubyte0";
		case Opcode::VCvtF32Ubyte1: return "v_cvt_f32_ubyte1";
		case Opcode::VCvtF32Ubyte2: return "v_cvt_f32_ubyte2";
		case Opcode::VCvtF32Ubyte3: return "v_cvt_f32_ubyte3";
		case Opcode::VRcpF32: return "v_rcp_f32";
		case Opcode::VFractF32: return "v_fract_f32";
		case Opcode::VTruncF32: return "v_trunc_f32";
		case Opcode::VCeilF32: return "v_ceil_f32";
		case Opcode::VRndneF32: return "v_rndne_f32";
		case Opcode::VFloorF32: return "v_floor_f32";
		case Opcode::VExpF32: return "v_exp_f32";
		case Opcode::VLogF32: return "v_log_f32";
		case Opcode::VRsqF32: return "v_rsq_f32";
		case Opcode::VSqrtF32: return "v_sqrt_f32";
		case Opcode::VRcpF16: return "v_rcp_f16";
		case Opcode::VSqrtF16: return "v_sqrt_f16";
		case Opcode::VRsqF16: return "v_rsq_f16";
		case Opcode::VLogF16: return "v_log_f16";
		case Opcode::VExpF16: return "v_exp_f16";
		case Opcode::VFloorF16: return "v_floor_f16";
		case Opcode::VCeilF16: return "v_ceil_f16";
		case Opcode::VTruncF16: return "v_trunc_f16";
		case Opcode::VRndneF16: return "v_rndne_f16";
		case Opcode::VSinF32: return "v_sin_f32";
		case Opcode::VCosF32: return "v_cos_f32";
		case Opcode::VNotB32: return "v_not_b32";
		case Opcode::VBfrevB32: return "v_bfrev_b32";
		case Opcode::VFfbhU32: return "v_ffbh_u32";
		case Opcode::VFfblB32: return "v_ffbl_b32";
		case Opcode::VAddF32: return "v_add_f32";
		case Opcode::VSubF32: return "v_sub_f32";
		case Opcode::VSubrevF32: return "v_subrev_f32";
		case Opcode::VMulF32: return "v_mul_f32";
		case Opcode::VMacF32: return "v_mac_f32";
		case Opcode::VMadmkF32: return "v_madmk_f32";
		case Opcode::VMadakF32: return "v_madak_f32";
		case Opcode::VMinF32: return "v_min_f32";
		case Opcode::VMaxF32: return "v_max_f32";
		case Opcode::VAddF16: return "v_add_f16";
		case Opcode::VSubF16: return "v_sub_f16";
		case Opcode::VSubrevF16: return "v_subrev_f16";
		case Opcode::VMulF16: return "v_mul_f16";
		case Opcode::VFmacF16: return "v_fmac_f16";
		case Opcode::VFmamkF16: return "v_fmamk_f16";
		case Opcode::VFmaakF16: return "v_fmaak_f16";
		case Opcode::VMinI32: return "v_min_i32";
		case Opcode::VMaxI32: return "v_max_i32";
		case Opcode::VMulI32I24: return "v_mul_i32_i24";
		case Opcode::VMulU32U24: return "v_mul_u32_u24";
		case Opcode::VMulLoU32: return "v_mul_lo_u32";
		case Opcode::VMulHiU32: return "v_mul_hi_u32";
		case Opcode::VMulLoI32: return "v_mul_lo_i32";
		case Opcode::VMulHiI32: return "v_mul_hi_i32";
		case Opcode::VCvtPkrtzF16F32: return "v_cvt_pkrtz_f16_f32";
		case Opcode::VCvtPkU8F32: return "v_cvt_pk_u8_f32";
		case Opcode::VMadF32: return "v_mad_f32";
		case Opcode::VMadI32I24: return "v_mad_i32_i24";
		case Opcode::VMadU32U24: return "v_mad_u32_u24";
		case Opcode::VMadU64U32: return "v_mad_u64_u32";
		case Opcode::VFmaF32: return "v_fma_f32";
		case Opcode::VFmaF16: return "v_fma_f16";
		case Opcode::VPackB32F16: return "v_pack_b32_f16";
		case Opcode::VBfeU32: return "v_bfe_u32";
		case Opcode::VBfeI32: return "v_bfe_i32";
		case Opcode::VBfiB32: return "v_bfi_b32";
		case Opcode::VAlignbitB32: return "v_alignbit_b32";
		case Opcode::VMin3F32: return "v_min3_f32";
		case Opcode::VMin3I32: return "v_min3_i32";
		case Opcode::VMin3U32: return "v_min3_u32";
		case Opcode::VMin3F16: return "v_min3_f16";
		case Opcode::VMax3F32: return "v_max3_f32";
		case Opcode::VMax3I32: return "v_max3_i32";
		case Opcode::VMax3U32: return "v_max3_u32";
		case Opcode::VMax3F16: return "v_max3_f16";
		case Opcode::VMed3F32: return "v_med3_f32";
		case Opcode::VMed3I32: return "v_med3_i32";
		case Opcode::VMed3U32: return "v_med3_u32";
		case Opcode::VMed3F16: return "v_med3_f16";
		case Opcode::VSadU32: return "v_sad_u32";
		case Opcode::VAdd3U32: return "v_add3_u32";
		case Opcode::VLshlAddU32: return "v_lshl_add_u32";
		case Opcode::VAddLshlU32: return "v_add_lshl_u32";
		case Opcode::VXadU32: return "v_xad_u32";
		case Opcode::VLshlOrB32: return "v_lshl_or_b32";
		case Opcode::VAndOrB32: return "v_and_or_b32";
		case Opcode::VOr3B32: return "v_or3_b32";
		case Opcode::VXor3B32: return "v_xor3_b32";
		case Opcode::VAddI32: return "v_add_i32";
		case Opcode::VSubI32: return "v_sub_i32";
		case Opcode::VSubrevI32: return "v_subrev_i32";
		case Opcode::VBfmB32: return "v_bfm_b32";
		case Opcode::VLdexpF32: return "v_ldexp_f32";
		case Opcode::VCvtPknormI16F32: return "v_cvt_pknorm_i16_f32";
		case Opcode::VCvtPknormU16F32: return "v_cvt_pknorm_u16_f32";
		case Opcode::VCvtPkU16U32: return "v_cvt_pk_u16_u32";
		case Opcode::VPkMadI16: return "v_pk_mad_i16";
		case Opcode::VPkMulLoU16: return "v_pk_mul_lo_u16";
		case Opcode::VPkAddI16: return "v_pk_add_i16";
		case Opcode::VPkSubI16: return "v_pk_sub_i16";
		case Opcode::VPkLshlrevB16: return "v_pk_lshlrev_b16";
		case Opcode::VPkLshrrevB16: return "v_pk_lshrrev_b16";
		case Opcode::VPkAshrrevI16: return "v_pk_ashrrev_i16";
		case Opcode::VPkMaxI16: return "v_pk_max_i16";
		case Opcode::VPkMinI16: return "v_pk_min_i16";
		case Opcode::VPkMadU16: return "v_pk_mad_u16";
		case Opcode::VPkAddF16: return "v_pk_add_f16";
		case Opcode::VPkMulF16: return "v_pk_mul_f16";
		case Opcode::VPkMinF16: return "v_pk_min_f16";
		case Opcode::VPkMaxF16: return "v_pk_max_f16";
		case Opcode::VPkFmaF16: return "v_pk_fma_f16";
		case Opcode::VPkFmacF16: return "v_pk_fmac_f16";
		case Opcode::VPkAddU16: return "v_pk_add_u16";
		case Opcode::VPkSubU16: return "v_pk_sub_u16";
		case Opcode::VPkMaxU16: return "v_pk_max_u16";
		case Opcode::VPkMinU16: return "v_pk_min_u16";
		case Opcode::VMadMixloF16: return "v_mad_mixlo_f16";
		case Opcode::VMadMixhiF16: return "v_mad_mixhi_f16";
		case Opcode::VAddNcU32: return "v_add_nc_u32";
		case Opcode::VAddcU32: return "v_addc_u32";
		case Opcode::VSubNcU32: return "v_sub_nc_u32";
		case Opcode::VSubrevNcU32: return "v_subrev_nc_u32";
		case Opcode::VAddNcU16: return "v_add_nc_u16";
		case Opcode::VSubNcU16: return "v_sub_nc_u16";
		case Opcode::VMaxU16: return "v_max_u16";
		case Opcode::VMaxI16: return "v_max_i16";
		case Opcode::VMinU16: return "v_min_u16";
		case Opcode::VMinI16: return "v_min_i16";
		case Opcode::VAddNcI16: return "v_add_nc_i16";
		case Opcode::VSubNcI16: return "v_sub_nc_i16";
		case Opcode::VAndB32: return "v_and_b32";
		case Opcode::VOrB32: return "v_or_b32";
		case Opcode::VXorB32: return "v_xor_b32";
		case Opcode::VXnorB32: return "v_xnor_b32";
		case Opcode::VLshlB32: return "v_lshl_b32";
		case Opcode::VLshlrevB32: return "v_lshlrev_b32";
		case Opcode::VLshrB32: return "v_lshr_b32";
		case Opcode::VLshrrevB32: return "v_lshrrev_b32";
		case Opcode::VAshrI32: return "v_ashr_i32";
		case Opcode::VAshrrevI32: return "v_ashrrev_i32";
		case Opcode::VLshlrevB16: return "v_lshlrev_b16";
		case Opcode::VLshrrevB16: return "v_lshrrev_b16";
		case Opcode::VAshrrevI16: return "v_ashrrev_i16";
		case Opcode::VBcntU32B32: return "v_bcnt_u32_b32";
		case Opcode::VMbcntLoU32B32: return "v_mbcnt_lo_u32_b32";
		case Opcode::VMbcntHiU32B32: return "v_mbcnt_hi_u32_b32";
		case Opcode::VMinU32: return "v_min_u32";
		case Opcode::VMaxU32: return "v_max_u32";
		case Opcode::VMaxF16: return "v_max_f16";
		case Opcode::VMinF16: return "v_min_f16";
		case Opcode::VCmpFF32: return "v_cmp_f_f32";
		case Opcode::VCmpLtF32: return "v_cmp_lt_f32";
		case Opcode::VCmpEqF32: return "v_cmp_eq_f32";
		case Opcode::VCmpLeF32: return "v_cmp_le_f32";
		case Opcode::VCmpGtF32: return "v_cmp_gt_f32";
		case Opcode::VCmpLgF32: return "v_cmp_lg_f32";
		case Opcode::VCmpGeF32: return "v_cmp_ge_f32";
		case Opcode::VCmpOF32: return "v_cmp_o_f32";
		case Opcode::VCmpUF32: return "v_cmp_u_f32";
		case Opcode::VCmpNgeF32: return "v_cmp_nge_f32";
		case Opcode::VCmpNlgF32: return "v_cmp_nlg_f32";
		case Opcode::VCmpNgtF32: return "v_cmp_ngt_f32";
		case Opcode::VCmpNleF32: return "v_cmp_nle_f32";
		case Opcode::VCmpNeqF32: return "v_cmp_neq_f32";
		case Opcode::VCmpNltF32: return "v_cmp_nlt_f32";
		case Opcode::VCmpTruF32: return "v_cmp_tru_f32";
		case Opcode::VCmpxLtF32: return "v_cmpx_lt_f32";
		case Opcode::VCmpxEqF32: return "v_cmpx_eq_f32";
		case Opcode::VCmpxLeF32: return "v_cmpx_le_f32";
		case Opcode::VCmpxGtF32: return "v_cmpx_gt_f32";
		case Opcode::VCmpxLgF32: return "v_cmpx_lg_f32";
		case Opcode::VCmpxGeF32: return "v_cmpx_ge_f32";
		case Opcode::VCmpxNgeF32: return "v_cmpx_nge_f32";
		case Opcode::VCmpxNlgF32: return "v_cmpx_nlg_f32";
		case Opcode::VCmpxNgtF32: return "v_cmpx_ngt_f32";
		case Opcode::VCmpxNleF32: return "v_cmpx_nle_f32";
		case Opcode::VCmpxNeqF32: return "v_cmpx_neq_f32";
		case Opcode::VCmpxNltF32: return "v_cmpx_nlt_f32";
		case Opcode::VCmpFI32: return "v_cmp_f_i32";
		case Opcode::VCmpLtI32: return "v_cmp_lt_i32";
		case Opcode::VCmpEqI32: return "v_cmp_eq_i32";
		case Opcode::VCmpLeI32: return "v_cmp_le_i32";
		case Opcode::VCmpGtI32: return "v_cmp_gt_i32";
		case Opcode::VCmpNeI32: return "v_cmp_ne_i32";
		case Opcode::VCmpGeI32: return "v_cmp_ge_i32";
		case Opcode::VCmpTI32: return "v_cmp_t_i32";
		case Opcode::VCmpClassF32: return "v_cmp_class_f32";
		case Opcode::VCmpLtI16: return "v_cmp_lt_i16";
		case Opcode::VCmpEqI16: return "v_cmp_eq_i16";
		case Opcode::VCmpLeI16: return "v_cmp_le_i16";
		case Opcode::VCmpGtI16: return "v_cmp_gt_i16";
		case Opcode::VCmpNeI16: return "v_cmp_ne_i16";
		case Opcode::VCmpGeI16: return "v_cmp_ge_i16";
		case Opcode::VCmpLtF16: return "v_cmp_lt_f16";
		case Opcode::VCmpEqF16: return "v_cmp_eq_f16";
		case Opcode::VCmpLeF16: return "v_cmp_le_f16";
		case Opcode::VCmpGtF16: return "v_cmp_gt_f16";
		case Opcode::VCmpLgF16: return "v_cmp_lg_f16";
		case Opcode::VCmpGeF16: return "v_cmp_ge_f16";
		case Opcode::VCmpNeqF16: return "v_cmp_neq_f16";
		case Opcode::VCmpxLtF16: return "v_cmpx_lt_f16";
		case Opcode::VCmpxEqF16: return "v_cmpx_eq_f16";
		case Opcode::VCmpxLeF16: return "v_cmpx_le_f16";
		case Opcode::VCmpxGtF16: return "v_cmpx_gt_f16";
		case Opcode::VCmpxGeF16: return "v_cmpx_ge_f16";
		case Opcode::VCmpxNeqF16: return "v_cmpx_neq_f16";
		case Opcode::VCmpxNltF16: return "v_cmpx_nlt_f16";
		case Opcode::VCmpxLtI32: return "v_cmpx_lt_i32";
		case Opcode::VCmpxEqI32: return "v_cmpx_eq_i32";
		case Opcode::VCmpxLeI32: return "v_cmpx_le_i32";
		case Opcode::VCmpxGtI32: return "v_cmpx_gt_i32";
		case Opcode::VCmpxNeI32: return "v_cmpx_ne_i32";
		case Opcode::VCmpxGeI32: return "v_cmpx_ge_i32";
		case Opcode::VCmpLtU16: return "v_cmp_lt_u16";
		case Opcode::VCmpEqU16: return "v_cmp_eq_u16";
		case Opcode::VCmpLeU16: return "v_cmp_le_u16";
		case Opcode::VCmpGtU16: return "v_cmp_gt_u16";
		case Opcode::VCmpNeU16: return "v_cmp_ne_u16";
		case Opcode::VCmpGeU16: return "v_cmp_ge_u16";
		case Opcode::VCmpFU32: return "v_cmp_f_u32";
		case Opcode::VCmpLtU32: return "v_cmp_lt_u32";
		case Opcode::VCmpEqU32: return "v_cmp_eq_u32";
		case Opcode::VCmpLeU32: return "v_cmp_le_u32";
		case Opcode::VCmpGtU32: return "v_cmp_gt_u32";
		case Opcode::VCmpNeU32: return "v_cmp_ne_u32";
		case Opcode::VCmpGeU32: return "v_cmp_ge_u32";
		case Opcode::VCmpTU32: return "v_cmp_t_u32";
		case Opcode::VCmpNeU64: return "v_cmp_ne_u64";
		case Opcode::VCmpxLtU32: return "v_cmpx_lt_u32";
		case Opcode::VCmpxEqU32: return "v_cmpx_eq_u32";
		case Opcode::VCmpxLeU32: return "v_cmpx_le_u32";
		case Opcode::VCmpxGtU32: return "v_cmpx_gt_u32";
		case Opcode::VCmpxNeU32: return "v_cmpx_ne_u32";
		case Opcode::VCmpxGeU32: return "v_cmpx_ge_u32";
		case Opcode::SLoadDword: return "s_load_dword";
		case Opcode::SLoadDwordx2: return "s_load_dwordx2";
		case Opcode::SLoadDwordx4: return "s_load_dwordx4";
		case Opcode::SLoadDwordx8: return "s_load_dwordx8";
		case Opcode::SLoadDwordx16: return "s_load_dwordx16";
		case Opcode::SBufferLoadDword: return "s_buffer_load_dword";
		case Opcode::SBufferLoadDwordx2: return "s_buffer_load_dwordx2";
		case Opcode::SBufferLoadDwordx4: return "s_buffer_load_dwordx4";
		case Opcode::SBufferLoadDwordx8: return "s_buffer_load_dwordx8";
		case Opcode::SBufferLoadDwordx16: return "s_buffer_load_dwordx16";
		case Opcode::BufferLoadFormatX: return "buffer_load_format_x";
		case Opcode::BufferLoadFormatXy: return "buffer_load_format_xy";
		case Opcode::BufferLoadFormatXyz: return "buffer_load_format_xyz";
		case Opcode::BufferLoadFormatXyzw: return "buffer_load_format_xyzw";
		case Opcode::BufferStoreFormatX: return "buffer_store_format_x";
		case Opcode::BufferStoreFormatXy: return "buffer_store_format_xy";
		case Opcode::BufferStoreFormatXyz: return "buffer_store_format_xyz";
		case Opcode::BufferStoreFormatXyzw: return "buffer_store_format_xyzw";
		case Opcode::BufferLoadUbyte: return "buffer_load_ubyte";
		case Opcode::BufferLoadSbyte: return "buffer_load_sbyte";
		case Opcode::BufferLoadUshort: return "buffer_load_ushort";
		case Opcode::BufferLoadSshort: return "buffer_load_sshort";
		case Opcode::BufferLoadDword: return "buffer_load_dword";
		case Opcode::BufferLoadDwordx2: return "buffer_load_dwordx2";
		case Opcode::BufferLoadDwordx3: return "buffer_load_dwordx3";
		case Opcode::BufferLoadDwordx4: return "buffer_load_dwordx4";
		case Opcode::BufferStoreByte: return "buffer_store_byte";
		case Opcode::BufferStoreShort: return "buffer_store_short";
		case Opcode::BufferStoreDword: return "buffer_store_dword";
		case Opcode::BufferStoreDwordx2: return "buffer_store_dwordx2";
		case Opcode::BufferStoreDwordx3: return "buffer_store_dwordx3";
		case Opcode::BufferStoreDwordx4: return "buffer_store_dwordx4";
		case Opcode::TBufferLoadFormatX: return "tbuffer_load_format_x";
		case Opcode::TBufferLoadFormatXy: return "tbuffer_load_format_xy";
		case Opcode::TBufferLoadFormatXyz: return "tbuffer_load_format_xyz";
		case Opcode::TBufferLoadFormatXyzw: return "tbuffer_load_format_xyzw";
		case Opcode::TBufferStoreFormatX: return "tbuffer_store_format_x";
		case Opcode::TBufferStoreFormatXy: return "tbuffer_store_format_xy";
		case Opcode::TBufferStoreFormatXyz: return "tbuffer_store_format_xyz";
		case Opcode::TBufferStoreFormatXyzw: return "tbuffer_store_format_xyzw";
		case Opcode::BufferAtomicSwap: return "buffer_atomic_swap";
		case Opcode::BufferAtomicAdd: return "buffer_atomic_add";
		case Opcode::BufferAtomicSub: return "buffer_atomic_sub";
		case Opcode::BufferAtomicSMin: return "buffer_atomic_smin";
		case Opcode::BufferAtomicUMin: return "buffer_atomic_umin";
		case Opcode::BufferAtomicSMax: return "buffer_atomic_smax";
		case Opcode::BufferAtomicUMax: return "buffer_atomic_umax";
		case Opcode::BufferAtomicAnd: return "buffer_atomic_and";
		case Opcode::BufferAtomicOr: return "buffer_atomic_or";
		case Opcode::BufferAtomicXor: return "buffer_atomic_xor";
		case Opcode::FlatLoadUbyte: return "flat_load_ubyte";
		case Opcode::FlatLoadSbyte: return "flat_load_sbyte";
		case Opcode::FlatLoadUshort: return "flat_load_ushort";
		case Opcode::FlatLoadSshort: return "flat_load_sshort";
		case Opcode::FlatLoadDword: return "flat_load_dword";
		case Opcode::FlatLoadDwordx2: return "flat_load_dwordx2";
		case Opcode::FlatLoadDwordx3: return "flat_load_dwordx3";
		case Opcode::FlatLoadDwordx4: return "flat_load_dwordx4";
		case Opcode::FlatStoreByte: return "flat_store_byte";
		case Opcode::FlatStoreShort: return "flat_store_short";
		case Opcode::FlatStoreDword: return "flat_store_dword";
		case Opcode::FlatStoreDwordx2: return "flat_store_dwordx2";
		case Opcode::FlatStoreDwordx3: return "flat_store_dwordx3";
		case Opcode::FlatStoreDwordx4: return "flat_store_dwordx4";
		case Opcode::DsAddU32: return "ds_add_u32";
		case Opcode::DsAddRtnU32: return "ds_add_rtn_u32";
		case Opcode::DsSubU32: return "ds_sub_u32";
		case Opcode::DsSubRtnU32: return "ds_sub_rtn_u32";
		case Opcode::DsMinI32: return "ds_min_i32";
		case Opcode::DsMinRtnI32: return "ds_min_rtn_i32";
		case Opcode::DsMaxI32: return "ds_max_i32";
		case Opcode::DsMaxRtnI32: return "ds_max_rtn_i32";
		case Opcode::DsMinU32: return "ds_min_u32";
		case Opcode::DsMinRtnU32: return "ds_min_rtn_u32";
		case Opcode::DsMaxU32: return "ds_max_u32";
		case Opcode::DsMaxRtnU32: return "ds_max_rtn_u32";
		case Opcode::DsAndB32: return "ds_and_b32";
		case Opcode::DsAndRtnB32: return "ds_and_rtn_b32";
		case Opcode::DsOrB32: return "ds_or_b32";
		case Opcode::DsOrRtnB32: return "ds_or_rtn_b32";
		case Opcode::DsXorB32: return "ds_xor_b32";
		case Opcode::DsXorRtnB32: return "ds_xor_rtn_b32";
		case Opcode::DsWrxchgRtnB32: return "ds_wrxchg_rtn_b32";
		case Opcode::DsMinF32: return "ds_min_f32";
		case Opcode::DsMaxF32: return "ds_max_f32";
		case Opcode::DsSwizzleB32: return "ds_swizzle_b32";
		case Opcode::DsConsume: return "ds_consume";
		case Opcode::DsAppend: return "ds_append";
		case Opcode::DsReadSbyte: return "ds_read_i8";
		case Opcode::DsReadUbyte: return "ds_read_u8";
		case Opcode::DsReadSshort: return "ds_read_i16";
		case Opcode::DsReadUshort: return "ds_read_u16";
		case Opcode::DsRead2B32: return "ds_read2_b32";
		case Opcode::DsReadB32: return "ds_read_b32";
		case Opcode::DsReadB64: return "ds_read_b64";
		case Opcode::DsRead2B64: return "ds_read2_b64";
		case Opcode::DsRead2St64B64: return "ds_read2st64_b64";
		case Opcode::DsReadB96: return "ds_read_b96";
		case Opcode::DsReadB128: return "ds_read_b128";
		case Opcode::DsWriteByte: return "ds_write_b8";
		case Opcode::DsWriteShort: return "ds_write_b16";
		case Opcode::DsWrite2B32: return "ds_write2_b32";
		case Opcode::DsWrite2St64B32: return "ds_write2st64_b32";
		case Opcode::DsWrite2B64: return "ds_write2_b64";
		case Opcode::DsWrite2St64B64: return "ds_write2st64_b64";
		case Opcode::DsWriteB32: return "ds_write_b32";
		case Opcode::DsWriteB64: return "ds_write_b64";
		case Opcode::DsWriteB96: return "ds_write_b96";
		case Opcode::DsWriteB128: return "ds_write_b128";
		case Opcode::DsWriteAddtidB32: return "ds_write_addtid_b32";
		case Opcode::DsReadAddtidB32: return "ds_read_addtid_b32";
		case Opcode::ImageGetResinfo: return "image_get_resinfo";
		case Opcode::ImageGetLod: return "image_get_lod";
		case Opcode::ImageLoad: return "image_load";
		case Opcode::ImageLoadMip: return "image_load_mip";
		case Opcode::ImageStore: return "image_store";
		case Opcode::ImageStoreMip: return "image_store_mip";
		case Opcode::ImageAtomicAdd: return "image_atomic_add";
		case Opcode::ImageAtomicUMin: return "image_atomic_umin";
		case Opcode::ImageAtomicUMax: return "image_atomic_umax";
		case Opcode::ImageAtomicAnd: return "image_atomic_and";
		case Opcode::ImageAtomicOr: return "image_atomic_or";
		case Opcode::ImageAtomicXor: return "image_atomic_xor";
		case Opcode::ImageSample: return "image_sample";
		case Opcode::ImageGather4Lz: return "image_gather4_lz";
		case Opcode::ImageGather4C: return "image_gather4_c";
		case Opcode::ImageGather4CLz: return "image_gather4_c_lz";
		case Opcode::ImageGather4LzO: return "image_gather4_lz_o";
		case Opcode::ImageGather4CO: return "image_gather4_c_o";
		case Opcode::ImageGather4CLzO: return "image_gather4_c_lz_o";
		case Opcode::ImageGather4H: return "image_gather4h";
		case Opcode::VInterpP1F32: return "v_interp_p1_f32";
		case Opcode::VInterpP2F32: return "v_interp_p2_f32";
		case Opcode::VInterpMovF32: return "v_interp_mov_f32";
		case Opcode::SNop: return "s_nop";
		case Opcode::SWaitcnt: return "s_waitcnt";
		case Opcode::SBarrier: return "s_barrier";
		case Opcode::SBranch: return "s_branch";
		case Opcode::SCbranchScc0: return "s_cbranch_scc0";
		case Opcode::SCbranchScc1: return "s_cbranch_scc1";
		case Opcode::SCbranchVccz: return "s_cbranch_vccz";
		case Opcode::SCbranchVccnz: return "s_cbranch_vccnz";
		case Opcode::SCbranchExecz: return "s_cbranch_execz";
		case Opcode::SCbranchExecnz: return "s_cbranch_execnz";
		case Opcode::SSendmsg: return "s_sendmsg";
		case Opcode::SSetregB32: return "s_setreg_b32";
		case Opcode::SSleep: return "s_sleep";
		case Opcode::STtraceData: return "s_ttracedata";
		case Opcode::SInstPrefetch: return "s_inst_prefetch";
		case Opcode::SEndpgm: return "s_endpgm";
		case Opcode::Exp: return "exp";
		case Opcode::Unsupported: return "unsupported";
		default: return "unknown";
	}
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
	if (inst.opcode == Opcode::Unsupported) {
		return fmt::format("0x{:08x}: unsupported family={} opcode=0x{:02x} raw=[{}] reason={}",
		                   inst.pc, FamilyToString(inst.family).c_str(), inst.opcode_id,
		                   RawWordsToString(inst).c_str(), inst.unsupported_reason.c_str());
	}

	switch (inst.opcode) {
		case Opcode::SMovB32:
		case Opcode::SMovB64:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: {} {}, {}", inst.pc,
			                                               OpcodeToString(inst.opcode).c_str(),
			                                               OperandToString(inst.dst).c_str(),
			                                               OperandToString(inst.src0).c_str()));
		case Opcode::SAbsI32:
		case Opcode::SBrevB32:
		case Opcode::SBcnt1I32B32:
		case Opcode::SFf1I32B32:
		case Opcode::SNotB64:
		case Opcode::SWqmB64:
		case Opcode::SAndSaveexecB32:
		case Opcode::SAndn1SaveexecB32:
		case Opcode::SAndSaveexecB64:
		case Opcode::SOrn2SaveexecB64:
		case Opcode::SAndn1SaveexecB64:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: {} {}, {}", inst.pc,
			                                               OpcodeToString(inst.opcode).c_str(),
			                                               OperandToString(inst.dst).c_str(),
			                                               OperandToString(inst.src0).c_str()));
		case Opcode::SGetpcB64:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_getpc_b64 {}", inst.pc,
			                                               OperandToString(inst.dst).c_str()));
		case Opcode::SSetpcB64:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_setpc_b64 {}", inst.pc,
			                                               OperandToString(inst.src0).c_str()));
		case Opcode::SSetregB32:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_setreg_b32 {}, {}", inst.pc,
			                                               OperandToString(inst.src0).c_str(),
			                                               OperandToString(inst.src1).c_str()));
		case Opcode::SNop:
		case Opcode::SWaitcnt:
		case Opcode::SSleep:
		case Opcode::SSendmsg:
		case Opcode::STtraceData:
		case Opcode::SInstPrefetch:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: {} {}", inst.pc,
			                                               OpcodeToString(inst.opcode).c_str(),
			                                               OperandToString(inst.src0).c_str()));
		case Opcode::SBarrier:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_barrier", inst.pc));
		case Opcode::VNop:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: v_nop", inst.pc));
		case Opcode::SEndpgm:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: s_endpgm", inst.pc));
		case Opcode::SBranch:
		case Opcode::SCbranchScc0:
		case Opcode::SCbranchScc1:
		case Opcode::SCbranchVccz:
		case Opcode::SCbranchVccnz:
		case Opcode::SCbranchExecz:
		case Opcode::SCbranchExecnz:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: {} 0x{:08x}", inst.pc,
			                                               OpcodeToString(inst.opcode).c_str(),
			                                               inst.branch_target));
		case Opcode::Exp: return WithUnsupportedReason(inst, FormatExp(inst));
		case Opcode::ImageSample:
		case Opcode::ImageStore:
		case Opcode::ImageStoreMip:
		case Opcode::ImageAtomicAdd:
		case Opcode::ImageAtomicUMin:
		case Opcode::ImageAtomicUMax:
		case Opcode::ImageAtomicAnd:
		case Opcode::ImageAtomicOr:
		case Opcode::ImageAtomicXor:
		case Opcode::ImageLoad:
		case Opcode::ImageLoadMip:
		case Opcode::ImageGetResinfo:
		case Opcode::ImageGetLod:
		case Opcode::ImageGather4Lz:
		case Opcode::ImageGather4C:
		case Opcode::ImageGather4CLz:
		case Opcode::ImageGather4LzO:
		case Opcode::ImageGather4CO:
		case Opcode::ImageGather4CLzO:
		case Opcode::ImageGather4H: return WithUnsupportedReason(inst, FormatMimg(inst));
		case Opcode::SLoadDword:
		case Opcode::SLoadDwordx2:
		case Opcode::SLoadDwordx4:
		case Opcode::SLoadDwordx8:
		case Opcode::SLoadDwordx16:
		case Opcode::SBufferLoadDword:
		case Opcode::SBufferLoadDwordx2:
		case Opcode::SBufferLoadDwordx4:
		case Opcode::SBufferLoadDwordx8:
		case Opcode::SBufferLoadDwordx16:
		case Opcode::BufferLoadFormatX:
		case Opcode::BufferLoadFormatXy:
		case Opcode::BufferLoadFormatXyz:
		case Opcode::BufferLoadFormatXyzw:
		case Opcode::BufferStoreFormatX:
		case Opcode::BufferStoreFormatXy:
		case Opcode::BufferStoreFormatXyz:
		case Opcode::BufferStoreFormatXyzw:
		case Opcode::BufferLoadUbyte:
		case Opcode::BufferLoadUshort:
		case Opcode::BufferLoadDword:
		case Opcode::BufferLoadDwordx2:
		case Opcode::BufferLoadDwordx3:
		case Opcode::BufferLoadDwordx4:
		case Opcode::BufferStoreByte:
		case Opcode::BufferStoreShort:
		case Opcode::BufferStoreDword:
		case Opcode::BufferStoreDwordx2:
		case Opcode::BufferStoreDwordx3:
		case Opcode::BufferStoreDwordx4:
		case Opcode::TBufferLoadFormatX:
		case Opcode::TBufferLoadFormatXy:
		case Opcode::TBufferLoadFormatXyz:
		case Opcode::TBufferLoadFormatXyzw:
		case Opcode::TBufferStoreFormatX:
		case Opcode::TBufferStoreFormatXy:
		case Opcode::TBufferStoreFormatXyz:
		case Opcode::TBufferStoreFormatXyzw:
		case Opcode::BufferAtomicSwap:
		case Opcode::BufferAtomicAdd:
		case Opcode::BufferAtomicSub:
		case Opcode::BufferAtomicSMin:
		case Opcode::BufferAtomicUMin:
		case Opcode::BufferAtomicSMax:
		case Opcode::BufferAtomicUMax:
		case Opcode::BufferAtomicAnd:
		case Opcode::BufferAtomicOr:
		case Opcode::BufferAtomicXor:
		case Opcode::BufferLoadSbyte:
		case Opcode::BufferLoadSshort:
		case Opcode::FlatLoadUbyte:
		case Opcode::FlatLoadSbyte:
		case Opcode::FlatLoadUshort:
		case Opcode::FlatLoadSshort:
		case Opcode::FlatLoadDword:
		case Opcode::FlatLoadDwordx2:
		case Opcode::FlatLoadDwordx3:
		case Opcode::FlatLoadDwordx4:
		case Opcode::FlatStoreByte:
		case Opcode::FlatStoreShort:
		case Opcode::FlatStoreDword:
		case Opcode::FlatStoreDwordx2:
		case Opcode::FlatStoreDwordx3:
		case Opcode::FlatStoreDwordx4:
		case Opcode::DsAddU32:
		case Opcode::DsAddRtnU32:
		case Opcode::DsSubU32:
		case Opcode::DsSubRtnU32:
		case Opcode::DsMinI32:
		case Opcode::DsMinRtnI32:
		case Opcode::DsMaxI32:
		case Opcode::DsMaxRtnI32:
		case Opcode::DsMinU32:
		case Opcode::DsMinRtnU32:
		case Opcode::DsMaxU32:
		case Opcode::DsMaxRtnU32:
		case Opcode::DsAndB32:
		case Opcode::DsAndRtnB32:
		case Opcode::DsOrB32:
		case Opcode::DsOrRtnB32:
		case Opcode::DsXorB32:
		case Opcode::DsXorRtnB32:
		case Opcode::DsWrxchgRtnB32:
		case Opcode::DsMinF32:
		case Opcode::DsMaxF32:
		case Opcode::DsSwizzleB32:
		case Opcode::DsReadSbyte:
		case Opcode::DsReadUbyte:
		case Opcode::DsReadSshort:
		case Opcode::DsReadUshort:
		case Opcode::DsRead2B32:
		case Opcode::DsReadB32:
		case Opcode::DsReadB64:
		case Opcode::DsRead2B64:
		case Opcode::DsRead2St64B64:
		case Opcode::DsReadB96:
		case Opcode::DsReadB128:
		case Opcode::DsWriteByte:
		case Opcode::DsWriteShort:
		case Opcode::DsWrite2B32:
		case Opcode::DsWrite2St64B32:
		case Opcode::DsWrite2B64:
		case Opcode::DsWrite2St64B64:
		case Opcode::DsWriteB32:
		case Opcode::DsWriteB64:
		case Opcode::DsWriteB96:
		case Opcode::DsWriteB128:
		case Opcode::DsWriteAddtidB32:
		case Opcode::DsReadAddtidB32: return WithUnsupportedReason(inst, FormatMemory(inst));
		case Opcode::SCmpEqU32:
		case Opcode::SCmpEqI32:
		case Opcode::SCmpLgU32:
		case Opcode::SCmpLgI32:
		case Opcode::SCmpGtU32:
		case Opcode::SCmpGtI32:
		case Opcode::SCmpGeU32:
		case Opcode::SCmpGeI32:
		case Opcode::SCmpLtU32:
		case Opcode::SCmpLtI32:
		case Opcode::SCmpLeU32:
		case Opcode::SCmpLeI32:
		case Opcode::SBitcmp0B32:
		case Opcode::SBitcmp1B32:
		case Opcode::SCmpLgU64:
			return WithUnsupportedReason(inst, fmt::format("0x{:08x}: {} {}, {}", inst.pc,
			                                               OpcodeToString(inst.opcode).c_str(),
			                                               OperandToString(inst.src0).c_str(),
			                                               OperandToString(inst.src1).c_str()));
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
