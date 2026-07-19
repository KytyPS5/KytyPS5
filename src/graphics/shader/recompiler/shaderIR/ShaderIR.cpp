#include "graphics/shader/recompiler/ShaderIR.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/shaderIR/ShaderIRInternal.h"

#include <algorithm>
#include <fmt/format.h>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

Operand MakeSccOperand() {
	Operand operand;
	operand.kind      = OperandKind::Register;
	operand.reg.file  = RegisterFile::Scc;
	operand.reg.index = static_cast<uint32_t>(Decoder::OperandKind::Scc);
	return operand;
}

Operand MakeImmediateU32(uint32_t value) {
	Operand operand;
	operand.kind = OperandKind::ImmediateU32;
	operand.imm  = value;
	return operand;
}

Operand MakePcRelativeU32(uint32_t value) {
	Operand operand;
	operand.kind = OperandKind::PcRelativeU32;
	operand.imm  = value;
	return operand;
}

bool AppendScalarResultSccNonZero(const Decoder::Instruction& decoded, BasicBlock* block,
                                  std::string* error) {
	if (!ScalarResultWritesSccNonZero(decoded.opcode)) {
		return true;
	}
	if (block == nullptr || block->instructions.empty()) {
		SetError(error, "internal IR error: missing scalar result for SCC update");
		return false;
	}

	const auto result = block->instructions.back().dst;
	if (result.kind != OperandKind::Register) {
		SetError(error, "internal IR error: scalar SCC update without register result");
		return false;
	}

	// Scalar SOP result instructions update SCC from result != 0. Text pixel shaders commonly
	// branch on SCC immediately after masking EXEC with VCC via s_andn2_b64.
	Instruction scc_update;
	scc_update.pc = decoded.pc;
	scc_update.op =
	    ScalarResultIs64Bit(decoded.opcode) ? Opcode::CompareNeU64 : Opcode::CompareNeU32;
	scc_update.dst       = MakeSccOperand();
	scc_update.src[0]    = result;
	scc_update.src[1]    = MakeImmediateU32(0);
	scc_update.src_count = 2;
	block->instructions.push_back(scc_update);
	return true;
}

uint32_t VectorByteConvertIndex(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::VCvtF32Ubyte0: return 0;
		case Decoder::Opcode::VCvtF32Ubyte1: return 1;
		case Decoder::Opcode::VCvtF32Ubyte2: return 2;
		case Decoder::Opcode::VCvtF32Ubyte3: return 3;
		default: return 0xffffffffu;
	}
}

void CopyOperandModifiers(const Decoder::Operand& decoded, Operand& operand) {
	operand.sdwa_sel           = decoded.sdwa_sel;
	operand.omod               = decoded.omod;
	operand.sdwa_sext          = decoded.sdwa_sext;
	operand.op_sel             = decoded.op_sel;
	operand.op_sel_hi          = decoded.op_sel_hi;
	operand.negate             = decoded.negate;
	operand.negate_hi          = decoded.negate_hi;
	operand.absolute           = decoded.absolute;
	operand.clamp              = decoded.clamp;
	operand.dpp_ctrl           = decoded.dpp_ctrl;
	operand.dpp_row_mask       = decoded.dpp_row_mask;
	operand.dpp_bank_mask      = decoded.dpp_bank_mask;
	operand.dpp_fetch_inactive = decoded.dpp_fetch_inactive;
	operand.dpp_bound_ctrl     = decoded.dpp_bound_ctrl;
	operand.dpp                = decoded.dpp;
}

bool LowerRegisterOperand(const Decoder::Operand& decoded, Operand* operand, std::string* error) {
	if (operand == nullptr) {
		SetError(error, "internal IR error: null operand");
		return false;
	}

	*operand = {};
	CopyOperandModifiers(decoded, *operand);

	switch (decoded.kind) {
		case Decoder::OperandKind::Sgpr:
			operand->kind      = OperandKind::Register;
			operand->reg.file  = RegisterFile::Scalar;
			operand->reg.index = decoded.reg;
			return true;
		case Decoder::OperandKind::Vgpr:
			operand->kind      = OperandKind::Register;
			operand->reg.file  = RegisterFile::Vector;
			operand->reg.index = decoded.reg;
			return true;
		case Decoder::OperandKind::VccLo:
			operand->kind      = OperandKind::Register;
			operand->reg.file  = RegisterFile::Vcc;
			operand->reg.index = 0;
			return true;
		case Decoder::OperandKind::VccHi:
			operand->kind      = OperandKind::Register;
			operand->reg.file  = RegisterFile::Vcc;
			operand->reg.index = 1;
			return true;
		case Decoder::OperandKind::ExecLo:
			operand->kind      = OperandKind::Register;
			operand->reg.file  = RegisterFile::Exec;
			operand->reg.index = 0;
			return true;
		case Decoder::OperandKind::ExecHi:
			operand->kind      = OperandKind::Register;
			operand->reg.file  = RegisterFile::Exec;
			operand->reg.index = 1;
			return true;
		case Decoder::OperandKind::Scc:
		case Decoder::OperandKind::VccZ:
		case Decoder::OperandKind::ExecZ:
			operand->kind      = OperandKind::Register;
			operand->reg.file  = RegisterFile::Scc;
			operand->reg.index = static_cast<uint32_t>(decoded.kind);
			return true;
		case Decoder::OperandKind::M0:
			operand->kind      = OperandKind::Register;
			operand->reg.file  = RegisterFile::M0;
			operand->reg.index = 0;
			return true;
		case Decoder::OperandKind::Null: operand->kind = OperandKind::Null; return true;
		default: SetError(error, "decoded operand cannot be used as an IR register"); return false;
	}
}

bool LowerSourceOperand(const Decoder::Operand& decoded, Operand* operand, std::string* error) {
	switch (decoded.kind) {
		case Decoder::OperandKind::LiteralConstant:
		case Decoder::OperandKind::IntegerInlineConstant:
		case Decoder::OperandKind::FloatInlineConstant:
			*operand = {};
			CopyOperandModifiers(decoded, *operand);
			operand->kind    = OperandKind::ImmediateU32;
			operand->imm     = decoded.value;
			operand->sext_64 = decoded.kind == Decoder::OperandKind::IntegerInlineConstant &&
			                   decoded.signed_val < 0;
			return true;
		case Decoder::OperandKind::Null:
		case Decoder::OperandKind::PopsExitingWaveId:
			*operand = {};
			CopyOperandModifiers(decoded, *operand);
			operand->kind = OperandKind::ImmediateU32;
			operand->imm  = 0;
			return true;
		default: return LowerRegisterOperand(decoded, operand, error);
	}
}

void ApplyDppDestinationMask(const Decoder::Instruction& decoded, Operand& dst) {
	if (dst.kind != OperandKind::Register || dst.reg.file != RegisterFile::Vector) {
		return;
	}

	const Decoder::Operand* sources[] = {&decoded.src0, &decoded.src1, &decoded.src2};
	for (uint32_t i = 0; i < decoded.src_count && i < 3u; i++) {
		if (!sources[i]->dpp) {
			continue;
		}
		dst.dpp                = true;
		dst.dpp_ctrl           = sources[i]->dpp_ctrl;
		dst.dpp_row_mask       = sources[i]->dpp_row_mask;
		dst.dpp_bank_mask      = sources[i]->dpp_bank_mask;
		dst.dpp_fetch_inactive = sources[i]->dpp_fetch_inactive;
		dst.dpp_bound_ctrl     = sources[i]->dpp_bound_ctrl;
		return;
	}
}

uint32_t ResourceIndexFromOperand(const Decoder::Operand& operand) {
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: return operand.reg / 4u;
		case Decoder::OperandKind::Vgpr: return operand.reg;
		case Decoder::OperandKind::IntegerInlineConstant:
		case Decoder::OperandKind::LiteralConstant: return operand.value;
		default: return 0;
	}
}

MemoryInfo MemoryInfoFromDecoded(const Decoder::Instruction& decoded, ResourceKind kind) {
	MemoryInfo mem;
	mem.kind                     = kind;
	mem.offset                   = decoded.offset;
	mem.secondary_offset         = decoded.secondary_offset;
	mem.dmask                    = decoded.dmask;
	mem.data_dwords              = decoded.data_dwords;
	mem.data_bits                = decoded.data_bits;
	mem.component_count          = decoded.data_dwords;
	mem.data_format              = decoded.data_format;
	mem.number_format            = decoded.number_format;
	mem.image_sample_flags       = decoded.image_sample_flags;
	mem.image_dimension          = decoded.image_dimension;
	mem.image_address_components = decoded.image_address_components;
	mem.image_nsa_dwords         = decoded.image_nsa_dwords;
	for (uint32_t i = 0; i < Decoder::MaxImageNsaAddressComponents; i++) {
		mem.image_nsa_addr[i] = decoded.image_nsa_addr[i];
	}
	mem.memory_segment = decoded.memory_segment;
	mem.data_signed    = decoded.data_signed;
	mem.typed          = decoded.typed;
	mem.formatted      = decoded.formatted;
	mem.image_has_mip  = decoded.opcode == Decoder::Opcode::ImageLoadMip ||
	                     decoded.opcode == Decoder::Opcode::ImageStoreMip;
	mem.glc            = decoded.glc;
	mem.slc            = decoded.slc;
	mem.idxen          = decoded.idxen;
	mem.offen          = decoded.offen;
	mem.resource       = ResourceIndexFromOperand(decoded.src1);
	mem.sampler        = ResourceIndexFromOperand(decoded.src2);
	if (kind == ResourceKind::Lds || kind == ResourceKind::Gds) {
		mem.resource = 0;
		mem.sampler  = 0;
	}
	return mem;
}

constexpr uint32_t SCALAR_DST_VCC_LO  = 106u;
constexpr uint32_t SCALAR_DST_VCC_HI  = 107u;
constexpr uint32_t SCALAR_DST_EXEC_LO = 126u;
constexpr uint32_t SCALAR_DST_EXEC_HI = 127u;

uint32_t RawScalarLoadBase(const Decoder::Operand& operand) {
	if (operand.kind == Decoder::OperandKind::Sgpr) {
		return operand.reg;
	}
	return operand.kind == Decoder::OperandKind::VccLo ? SCALAR_DST_VCC_LO : 0u;
}

void ClearRegisterOffsetModifiers(Decoder::Operand& operand) {
	operand.sdwa_sel           = 6;
	operand.omod               = 0;
	operand.sdwa_sext          = false;
	operand.op_sel             = false;
	operand.op_sel_hi          = false;
	operand.negate             = false;
	operand.negate_hi          = false;
	operand.absolute           = false;
	operand.dpp_ctrl           = 0;
	operand.dpp_row_mask       = 0xf;
	operand.dpp_bank_mask      = 0xf;
	operand.dpp_fetch_inactive = false;
	operand.dpp_bound_ctrl     = false;
	operand.dpp                = false;
}

bool TryGetScalarDestinationCode(const Decoder::Operand& operand, uint32_t& code) {
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: code = operand.reg; return true;
		case Decoder::OperandKind::VccLo: code = SCALAR_DST_VCC_LO; return true;
		case Decoder::OperandKind::VccHi: code = SCALAR_DST_VCC_HI; return true;
		case Decoder::OperandKind::ExecLo: code = SCALAR_DST_EXEC_LO; return true;
		case Decoder::OperandKind::ExecHi: code = SCALAR_DST_EXEC_HI; return true;
		default: return false;
	}
}

bool TryOffsetScalarDestination(const Decoder::Operand& operand, uint32_t index,
                                Decoder::Operand& decoded) {
	// Wide scalar destinations advance through the encoded SGPR destination space, where
	// aliases such as VCC and EXEC sit beside ordinary SGPRs. Re-decode after adding the
	// dword index so s_buffer_load_dwordx2 vcc_lo writes vcc_lo, then vcc_hi.
	uint32_t code = 0;
	return TryGetScalarDestinationCode(operand, code) &&
	       Decoder::DecodeScalarDestination(code + index, 0, &decoded, nullptr);
}

Decoder::Operand OffsetDecodedRegister(const Decoder::Operand& operand, uint32_t index) {
	if (index == 0) {
		return operand;
	}
	auto ret = operand;
	ClearRegisterOffsetModifiers(ret);
	switch (ret.kind) {
		case Decoder::OperandKind::Vgpr: ret.reg += index; break;
		case Decoder::OperandKind::Sgpr:
		case Decoder::OperandKind::VccLo:
		case Decoder::OperandKind::VccHi:
		case Decoder::OperandKind::ExecLo:
		case Decoder::OperandKind::ExecHi: {
			Decoder::Operand decoded;
			if (TryOffsetScalarDestination(ret, index, decoded)) {
				ret = decoded;
			}
			break;
		}
		default: break;
	}
	return ret;
}

MemoryInfo OffsetMemoryInfo(const Decoder::Instruction& decoded, ResourceKind kind,
                            uint32_t dword_index) {
	auto mem = MemoryInfoFromDecoded(decoded, kind);
	mem.offset += dword_index * 4u;
	mem.data_dwords     = 1;
	mem.component_index = dword_index;
	mem.component_count = decoded.data_dwords;
	return mem;
}

uint32_t TypedBufferComponentOffsetBytes(const Decoder::Instruction& decoded,
                                         uint32_t                    component_index) {
	if (!decoded.typed || !decoded.formatted) {
		return component_index * 4u;
	}

	const auto format = Format::DecodeTBufferFormat(decoded.data_format, decoded.number_format);
	if (!Format::IsKnownFormat(format)) {
		return component_index * 4u;
	}
	return Format::GetFormatComponentByteOffset(format, component_index);
}

uint32_t TypedBufferFormatComponentCount(const Decoder::Instruction& decoded) {
	if (!decoded.typed || !decoded.formatted) {
		return decoded.data_dwords;
	}

	const auto format = Format::DecodeTBufferFormat(decoded.data_format, decoded.number_format);
	if (!Format::IsKnownFormat(format)) {
		return decoded.data_dwords;
	}
	return Format::GetFormatComponentCount(format);
}

bool LowerMoveImmediateU32(uint32_t pc, const Decoder::Operand& dst, uint32_t value,
                           BasicBlock* block, std::string* error) {
	Instruction inst;
	inst.pc        = pc;
	inst.op        = Opcode::MoveU32;
	inst.src[0]    = MakeImmediateU32(value);
	inst.src_count = 1;
	if (!LowerRegisterOperand(dst, &inst.dst, error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

MemoryInfo OffsetBufferMemoryInfo(const Decoder::Instruction& decoded, uint32_t component_index) {
	auto mem = MemoryInfoFromDecoded(decoded, ResourceKind::Buffer);
	mem.offset += TypedBufferComponentOffsetBytes(decoded, component_index);
	mem.data_dwords     = 1;
	mem.component_index = component_index;
	mem.component_count = decoded.data_dwords;
	return mem;
}

MemoryInfo ByteOffsetMemoryInfo(const Decoder::Instruction& decoded, ResourceKind kind,
                                uint32_t byte_offset) {
	auto mem             = MemoryInfoFromDecoded(decoded, kind);
	mem.offset           = byte_offset;
	mem.secondary_offset = 0;
	mem.data_dwords      = 1;
	mem.component_index  = 0;
	mem.component_count  = 1;
	return mem;
}

ResourceKind FlatSegmentResourceKind(uint32_t segment) {
	switch (segment) {
		case 1u: return ResourceKind::Scratch;
		case 2u: return ResourceKind::Global;
		default: return ResourceKind::Flat;
	}
}

const Decoder::Operand& DecodedSourceAt(const Decoder::Instruction& decoded, uint32_t index) {
	switch (index) {
		case 0: return decoded.src0;
		case 1: return decoded.src1;
		case 2: return decoded.src2;
		default: return decoded.src3;
	}
}

Decoder::Operand M0Operand() {
	Decoder::Operand operand;
	operand.kind = Decoder::OperandKind::M0;
	return operand;
}

namespace {

bool LowerScalarSelect(const Decoder::Instruction& decoded, BasicBlock* block, std::string* error) {
	Decoder::Operand scc;
	scc.kind = Decoder::OperandKind::Scc;

	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::SelectU32;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerRegisterOperand(scc, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[1], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerScalarSelect64(const Decoder::Instruction& decoded, BasicBlock* block,
                         std::string* error) {
	Decoder::Operand scc;
	scc.kind = Decoder::OperandKind::Scc;

	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::SelectU64;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerRegisterOperand(scc, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[1], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

uint32_t ScalarShiftLeftAddAmount(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SLshl1AddU32: return 1;
		case Decoder::Opcode::SLshl2AddU32: return 2;
		case Decoder::Opcode::SLshl3AddU32: return 3;
		case Decoder::Opcode::SLshl4AddU32: return 4;
		default: return 0;
	}
}

bool LowerScalarShiftLeftAdd(const Decoder::Instruction& decoded, BasicBlock* block,
                             std::string* error) {
	const auto shift = ScalarShiftLeftAddAmount(decoded.opcode);
	if (shift == 0) {
		SetError(error, "internal IR error: unknown scalar shift-left-add opcode");
		return false;
	}

	Instruction inst;
	inst.pc          = decoded.pc;
	inst.op          = Opcode::ScalarShiftLeftAddCarryU32;
	inst.src_count   = 3;
	inst.src[1].kind = OperandKind::ImmediateU32;
	inst.src[1].imm  = shift;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

Opcode ScalarMinMaxSccCompareOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SMinI32: return Opcode::CompareLtI32;
		case Decoder::Opcode::SMaxI32: return Opcode::CompareGtI32;
		case Decoder::Opcode::SMinU32: return Opcode::CompareLtU32;
		case Decoder::Opcode::SMaxU32: return Opcode::CompareGtU32;
		default: return Opcode::CompareFalse;
	}
}

bool IsScalarMinMaxOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SMinI32:
		case Decoder::Opcode::SMaxI32:
		case Decoder::Opcode::SMinU32:
		case Decoder::Opcode::SMaxU32: return true;
		default: return false;
	}
}

bool LowerScalarMinMaxWithScc(const Decoder::Instruction& decoded, BasicBlock* block,
                              std::string* error) {
	Instruction result;
	result.pc        = decoded.pc;
	result.op        = LookupIrOpcode(decoded.opcode);
	result.src_count = 2;
	if (!LowerRegisterOperand(decoded.dst, &result.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &result.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &result.src[1], error)) {
		return false;
	}
	block->instructions.push_back(result);

	Instruction scc;
	scc.pc        = decoded.pc;
	scc.op        = ScalarMinMaxSccCompareOpcode(decoded.opcode);
	scc.dst       = MakeSccOperand();
	scc.src_count = 2;
	if (!LowerSourceOperand(decoded.src0, &scc.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &scc.src[1], error)) {
		return false;
	}
	block->instructions.push_back(scc);
	return true;
}

bool LowerScalarAddCarry(const Decoder::Instruction& decoded, BasicBlock* block, bool carry_in,
                         std::string* error) {
	Decoder::Operand scc;
	scc.kind = Decoder::OperandKind::Scc;

	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::ScalarAddCarryU32;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error)) {
		return false;
	}
	if (carry_in) {
		if (!LowerRegisterOperand(scc, &inst.src[2], error)) {
			return false;
		}
	} else {
		inst.src[2].kind = OperandKind::ImmediateU32;
		inst.src[2].imm  = 0;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerScalarSubBorrowCarry(const Decoder::Instruction& decoded, BasicBlock* block,
                               std::string* error) {
	Decoder::Operand scc;
	scc.kind = Decoder::OperandKind::Scc;

	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::ScalarSubBorrowCarryU32;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error) ||
	    !LowerRegisterOperand(scc, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerScalarBinaryScc(const Decoder::Instruction& decoded, BasicBlock* block, Opcode op,
                          std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = op;
	inst.src_count = 2;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerScalarBitset0B32(const Decoder::Instruction& decoded, BasicBlock* block,
                           std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::BitClearU32;
	inst.src_count = 2;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.dst, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[1], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerScalarBitset1B32(const Decoder::Instruction& decoded, BasicBlock* block,
                           std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::BitSetU32;
	inst.src_count = 2;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.dst, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[1], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorCndmask(const Decoder::Instruction& decoded, BasicBlock* block,
                        std::string* error) {
	Decoder::Operand vcc;
	vcc.kind         = Decoder::OperandKind::VccLo;
	const auto& mask = decoded.src_count >= 3 ? decoded.src2 : vcc;

	Instruction inst;
	inst.pc = decoded.pc;
	inst.op =
	    decoded.src0.negate || decoded.src0.absolute || decoded.src1.negate || decoded.src1.absolute
	        ? Opcode::SelectMaskF32Bits
	        : Opcode::SelectMaskU32;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(mask, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorMoveB32(const Decoder::Instruction& decoded, BasicBlock* block,
                        std::string* error) {
	Instruction inst;
	inst.pc = decoded.pc;
	inst.op = decoded.src0.negate || decoded.src0.absolute ? Opcode::MoveF32Bits : Opcode::MoveU32;
	inst.src_count = 1;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error)) {
		return false;
	}
	ApplyDppDestinationMask(decoded, inst.dst);
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorMoveRelSource(const Decoder::Instruction& decoded, BasicBlock* block,
                              std::string* error) {
	if (decoded.dst.kind != Decoder::OperandKind::Vgpr ||
	    decoded.src0.kind != Decoder::OperandKind::Vgpr) {
		SetError(error, "V_MOVRELS_B32 requires VGPR source and destination");
		return false;
	}
	if (decoded.dst.sdwa_sel != 6u || decoded.dst.omod != 0u || decoded.dst.clamp ||
	    decoded.src0.sdwa_sel != 6u || decoded.src0.sdwa_sext || decoded.src0.negate ||
	    decoded.src0.absolute || decoded.src0.dpp) {
		SetError(error, "V_MOVRELS_B32 modifiers are not implemented");
		return false;
	}

	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::MoveRelSourceU32;
	inst.src_count = 2;
	const auto m0  = M0Operand();
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(m0, &inst.src[1], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorMoveRelDestination(const Decoder::Instruction& decoded, BasicBlock* block,
                                   std::string* error) {
	if (decoded.dst.kind != Decoder::OperandKind::Vgpr) {
		SetError(error, "V_MOVRELD_B32 requires VGPR destination");
		return false;
	}
	if (decoded.dst.sdwa_sel != 6u || decoded.dst.omod != 0u || decoded.dst.clamp ||
	    decoded.src0.sdwa_sel != 6u || decoded.src0.sdwa_sext || decoded.src0.negate ||
	    decoded.src0.absolute || decoded.src0.dpp) {
		SetError(error, "V_MOVRELD_B32 modifiers are not implemented");
		return false;
	}

	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::MoveRelDestU32;
	inst.src_count = 2;
	const auto m0  = M0Operand();
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(m0, &inst.src[1], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorAddCarry(const Decoder::Instruction& decoded, BasicBlock* block,
                         std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::IAddCarryU32;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerRegisterOperand(decoded.dst2, &inst.dst2, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error) ||
	    !LowerSourceOperand(decoded.src2, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorCarryOut(const Decoder::Instruction& decoded, BasicBlock* block,
                         std::string* error) {
	Instruction inst;
	inst.pc = decoded.pc;
	inst.op =
	    decoded.opcode == Decoder::Opcode::VAddI32 ? Opcode::IAddCarryU32 : Opcode::ISubBorrowU32;
	inst.src_count   = decoded.opcode == Decoder::Opcode::VAddI32 ? 3u : 2u;
	const auto& src0 = decoded.opcode == Decoder::Opcode::VSubrevI32 ? decoded.src1 : decoded.src0;
	const auto& src1 = decoded.opcode == Decoder::Opcode::VSubrevI32 ? decoded.src0 : decoded.src1;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerRegisterOperand(decoded.dst2, &inst.dst2, error) ||
	    !LowerSourceOperand(src0, &inst.src[0], error) ||
	    !LowerSourceOperand(src1, &inst.src[1], error)) {
		return false;
	}
	if (decoded.opcode == Decoder::Opcode::VAddI32) {
		inst.src[2] = MakeImmediateU32(0);
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorMadU64U32(const Decoder::Instruction& decoded, BasicBlock* block,
                          std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::UMadU64U32;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerRegisterOperand(decoded.dst2, &inst.dst2, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error) ||
	    !LowerSourceOperand(decoded.src2, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorMacF32(const Decoder::Instruction& decoded, BasicBlock* block, std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::FMadF32;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error) ||
	    !LowerSourceOperand(decoded.dst, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorPkFmacF16(const Decoder::Instruction& decoded, BasicBlock* block,
                          std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::PackedFmaF16;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error) ||
	    !LowerSourceOperand(decoded.dst, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorFmacF16(const Decoder::Instruction& decoded, BasicBlock* block,
                        std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::FmaF16;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error) ||
	    !LowerSourceOperand(decoded.dst, &inst.src[2], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorDot2cF32F16(const Decoder::Instruction& decoded, BasicBlock* block,
                            std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = Opcode::Dot2AccF32F16;
	inst.src_count = 3;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src1, &inst.src[1], error)) {
		return false;
	}
	inst.src[2] = inst.dst;
	block->instructions.push_back(inst);
	return true;
}

bool LowerVectorByteConvert(const Decoder::Instruction& decoded, BasicBlock* block,
                            std::string* error) {
	const auto byte_index = VectorByteConvertIndex(decoded.opcode);
	if (byte_index > 3u) {
		SetError(error, "internal IR error: unknown vector byte convert opcode");
		return false;
	}

	Instruction inst;
	inst.pc          = decoded.pc;
	inst.op          = Opcode::ConvertByteU32ToF32;
	inst.src_count   = 2;
	inst.src[1].kind = OperandKind::ImmediateU32;
	inst.src[1].imm  = byte_index;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerVInterpP1F32(const Decoder::Instruction& decoded, BasicBlock* block) {
	Instruction inst;
	inst.pc = decoded.pc;
	inst.op = Opcode::ControlNop;
	block->instructions.push_back(inst);
	return true;
}

bool LowerVInterpLoadF32(const Decoder::Instruction& decoded, BasicBlock* block,
                         std::string* error) {
	if (decoded.opcode == Decoder::Opcode::VInterpMovF32 && decoded.src0.value != 2u) {
		if (error != nullptr) {
			*error = fmt::format("v_interp_mov_f32 mode {} is not implemented at pc 0x{:08x}",
			                     decoded.src0.value, decoded.pc);
		}
		return false;
	}

	Instruction inst;
	inst.pc              = decoded.pc;
	inst.op              = Opcode::LoadInputF32;
	inst.input_info.attr = decoded.src1.value;
	inst.input_info.chan = decoded.src2.value;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

ExportTargetKind ExportTargetKindFromTarget(uint32_t target, uint32_t* index) {
	if (index != nullptr) {
		*index = 0;
	}
	switch (target) {
		case 0x08u: return ExportTargetKind::MrtZ;
		case 0x09u: return ExportTargetKind::Null;
		case 0x14u: return ExportTargetKind::Primitive;
		default: break;
	}
	if (target <= 0x07u) {
		if (index != nullptr) {
			*index = target;
		}
		return ExportTargetKind::Mrt;
	}
	if (target >= 0x0cu && target <= 0x0fu) {
		if (index != nullptr) {
			*index = target - 0x0cu;
		}
		return ExportTargetKind::Position;
	}
	if (target >= 0x20u && target <= 0x3fu) {
		if (index != nullptr) {
			*index = target - 0x20u;
		}
		return ExportTargetKind::Parameter;
	}
	return ExportTargetKind::Unknown;
}

bool LowerExportInstruction(const Decoder::Instruction& decoded, BasicBlock* block,
                            std::string* error) {
	uint32_t   index = 0;
	const auto kind  = ExportTargetKindFromTarget(decoded.exp.target, &index);
	if (kind == ExportTargetKind::Unknown) {
		if (error != nullptr) {
			*error = fmt::format("unsupported EXP target 0x{:02x} at pc 0x{:08x}",
			                     decoded.exp.target, decoded.pc);
		}
		return false;
	}

	Instruction inst;
	inst.pc                 = decoded.pc;
	inst.op                 = Opcode::Export;
	inst.dst.kind           = OperandKind::Null;
	inst.src_count          = decoded.src_count;
	inst.export_info.kind   = kind;
	inst.export_info.target = decoded.exp.target;
	inst.export_info.index  = index;
	inst.export_info.en     = decoded.exp.en;
	inst.export_info.done   = decoded.exp.done;
	inst.export_info.compr  = decoded.exp.compr;
	inst.export_info.vm     = decoded.exp.vm;

	const Decoder::Operand* sources[] = {&decoded.src0, &decoded.src1, &decoded.src2,
	                                     &decoded.src3};
	for (uint32_t i = 0; i < inst.src_count && i < 4u; i++) {
		if (!LowerSourceOperand(*sources[i], &inst.src[i], error)) {
			return false;
		}
	}

	block->instructions.push_back(inst);
	return true;
}

bool LowerControlMarker(const Decoder::Instruction& decoded, BasicBlock* block, Opcode op,
                        bool has_imm, std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = op;
	inst.dst.kind  = OperandKind::Null;
	inst.src_count = has_imm ? 1u : 0u;
	if (has_imm && !LowerSourceOperand(decoded.src0, &inst.src[0], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool LowerControlInstruction(const Decoder::Instruction& decoded, BasicBlock* block,
                             std::string* error) {
	switch (decoded.opcode) {
		case Decoder::Opcode::SNop:
			return LowerControlMarker(decoded, block, Opcode::ControlNop, true, error);
		case Decoder::Opcode::SWaitcnt:
			return LowerControlMarker(decoded, block, Opcode::Waitcnt, true, error);
		case Decoder::Opcode::SBarrier:
			return LowerControlMarker(decoded, block, Opcode::Barrier, false, error);
		case Decoder::Opcode::SSendmsg:
			return LowerControlMarker(decoded, block, Opcode::Sendmsg, true, error);
		case Decoder::Opcode::SSetregB32:
		case Decoder::Opcode::SSleep:
			return LowerControlMarker(decoded, block, Opcode::ControlNop, true, error);
		case Decoder::Opcode::STtraceData:
			return LowerControlMarker(decoded, block, Opcode::TtraceData, true, error);
		case Decoder::Opcode::SInstPrefetch:
			return LowerControlMarker(decoded, block, Opcode::InstPrefetch, true, error);
		default: return false;
	}
}

bool LowerScalarGetpcB64(const Decoder::Instruction& decoded, BasicBlock* block,
                         std::string* error) {
	if (block == nullptr) {
		SetError(error, "internal IR error: missing block for s_getpc_b64");
		return false;
	}

	for (uint32_t i = 0; i < 2u; i++) {
		Instruction inst;
		inst.pc        = decoded.pc;
		inst.op        = Opcode::MoveU32;
		inst.src[0]    = i == 0u ? MakePcRelativeU32(decoded.pc + 4u) : MakeImmediateU32(0u);
		inst.src_count = 1;
		if (!LowerRegisterOperand(OffsetDecodedRegister(decoded.dst, i), &inst.dst, error)) {
			return false;
		}
		block->instructions.push_back(inst);
	}
	return true;
}

bool IsScalarSaveexecOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SAndSaveexecB32:
		case Decoder::Opcode::SAndn1SaveexecB32:
		case Decoder::Opcode::SAndSaveexecB64:
		case Decoder::Opcode::SOrn2SaveexecB64:
		case Decoder::Opcode::SAndn1SaveexecB64: return true;
		default: return false;
	}
}

SaveexecMode ScalarSaveexecMode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SOrn2SaveexecB64: return SaveexecMode::Orn2;
		case Decoder::Opcode::SAndn1SaveexecB32:
		case Decoder::Opcode::SAndn1SaveexecB64: return SaveexecMode::Andn1;
		default: return SaveexecMode::And;
	}
}

bool LowerScalarSaveexec(const Decoder::Instruction& decoded, BasicBlock* block,
                         std::string* error) {
	Instruction inst;
	inst.pc            = decoded.pc;
	inst.op            = (decoded.opcode == Decoder::Opcode::SAndSaveexecB32 ||
	                      decoded.opcode == Decoder::Opcode::SAndn1SaveexecB32)
	                         ? Opcode::SaveexecB32
	                         : Opcode::SaveexecB64;
	inst.saveexec_mode = ScalarSaveexecMode(decoded.opcode);
	inst.src_count     = 1;
	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, &inst.src[0], error)) {
		return false;
	}
	block->instructions.push_back(inst);
	return true;
}

bool IsControlOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SNop:
		case Decoder::Opcode::SWaitcnt:
		case Decoder::Opcode::SBarrier:
		case Decoder::Opcode::SSendmsg:
		case Decoder::Opcode::SSetregB32:
		case Decoder::Opcode::SSleep:
		case Decoder::Opcode::STtraceData:
		case Decoder::Opcode::SInstPrefetch: return true;
		default: return false;
	}
}

bool IsTerminatorOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SEndpgm:
		case Decoder::Opcode::SBranch:
		case Decoder::Opcode::SCbranchScc0:
		case Decoder::Opcode::SCbranchScc1:
		case Decoder::Opcode::SCbranchVccz:
		case Decoder::Opcode::SCbranchVccnz:
		case Decoder::Opcode::SCbranchExecz:
		case Decoder::Opcode::SCbranchExecnz:
		case Decoder::Opcode::SSetpcB64: return true;
		default: return false;
	}
}

bool LowerImplemented(const Decoder::Instruction& decoded, BasicBlock* block, std::string* error);

bool LowerDecodedInstruction(const Decoder::Instruction& inst, BasicBlock* block,
                             std::string* error) {
	switch (inst.opcode) {
		case Decoder::Opcode::Unsupported:
			if (error != nullptr) {
				*error = fmt::format("unsupported decoded instruction: {}",
				                     Decoder::InstructionToString(inst).c_str());
			}
			return false;
		case Decoder::Opcode::SGetpcB64: return LowerScalarGetpcB64(inst, block, error);
		case Decoder::Opcode::VNop: return true;
		case Decoder::Opcode::Exp: return LowerExportInstruction(inst, block, error);
		case Decoder::Opcode::SCselectB32: return LowerScalarSelect(inst, block, error);
		case Decoder::Opcode::SCselectB64: return LowerScalarSelect64(inst, block, error);
		case Decoder::Opcode::SAddU32: return LowerScalarAddCarry(inst, block, false, error);
		case Decoder::Opcode::SAddcU32: return LowerScalarAddCarry(inst, block, true, error);
		case Decoder::Opcode::SSubbU32: return LowerScalarSubBorrowCarry(inst, block, error);
		case Decoder::Opcode::SSubU32:
			return LowerScalarBinaryScc(inst, block, Opcode::ScalarSubBorrowU32, error);
		case Decoder::Opcode::SBitset0B32: return LowerScalarBitset0B32(inst, block, error);
		case Decoder::Opcode::SBitset1B32: return LowerScalarBitset1B32(inst, block, error);
		case Decoder::Opcode::SAddI32:
			return LowerScalarBinaryScc(inst, block, Opcode::ScalarSignedAddOverflowI32, error);
		case Decoder::Opcode::SSubI32:
			return LowerScalarBinaryScc(inst, block, Opcode::ScalarSignedSubOverflowI32, error);
		case Decoder::Opcode::VCndmaskB32: return LowerVectorCndmask(inst, block, error);
		case Decoder::Opcode::VMovB32: return LowerVectorMoveB32(inst, block, error);
		case Decoder::Opcode::VMovreldB32: return LowerVectorMoveRelDestination(inst, block, error);
		case Decoder::Opcode::VMovrelsB32: return LowerVectorMoveRelSource(inst, block, error);
		case Decoder::Opcode::VAddcU32: return LowerVectorAddCarry(inst, block, error);
		case Decoder::Opcode::VMadU64U32: return LowerVectorMadU64U32(inst, block, error);
		case Decoder::Opcode::VMacF32: return LowerVectorMacF32(inst, block, error);
		case Decoder::Opcode::VPkFmacF16: return LowerVectorPkFmacF16(inst, block, error);
		case Decoder::Opcode::VFmacF16: return LowerVectorFmacF16(inst, block, error);
		case Decoder::Opcode::VDot2cF32F16: return LowerVectorDot2cF32F16(inst, block, error);
		case Decoder::Opcode::VInterpP1F32: return LowerVInterpP1F32(inst, block);
		case Decoder::Opcode::VInterpP2F32:
		case Decoder::Opcode::VInterpMovF32: return LowerVInterpLoadF32(inst, block, error);
		default: break;
	}

	if (IsTerminatorOpcode(inst.opcode)) {
		return true;
	}
	if (IsControlOpcode(inst.opcode)) {
		return LowerControlInstruction(inst, block, error);
	}
	if (IsScalarSaveexecOpcode(inst.opcode)) {
		return LowerScalarSaveexec(inst, block, error);
	}
	if (IsMemoryOpcode(inst.opcode)) {
		return LowerMemoryInstruction(inst, block, error);
	}
	if (ScalarShiftLeftAddAmount(inst.opcode) != 0) {
		return LowerScalarShiftLeftAdd(inst, block, error);
	}
	if (IsScalarMinMaxOpcode(inst.opcode)) {
		return LowerScalarMinMaxWithScc(inst, block, error);
	}
	if (IsVectorCarryOutOpcode(inst.opcode)) {
		return LowerVectorCarryOut(inst, block, error);
	}
	if (VectorByteConvertIndex(inst.opcode) <= 3u) {
		return LowerVectorByteConvert(inst, block, error);
	}
	if (!IsImplemented(inst.opcode)) {
		if (error != nullptr) {
			*error = fmt::format("decoded opcode has no IR lowering yet: {}",
			                     Decoder::InstructionToString(inst).c_str());
		}
		return false;
	}
	return LowerImplemented(inst, block, error);
}

bool LowerImplemented(const Decoder::Instruction& decoded, BasicBlock* block, std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = LookupIrOpcode(decoded.opcode);
	inst.src_count = decoded.src_count;

	if (!LowerRegisterOperand(decoded.dst, &inst.dst, error)) {
		return false;
	}
	ApplyDppDestinationMask(decoded, inst.dst);
	for (uint32_t i = 0; i < decoded.src_count && i < 3u; i++) {
		const auto src_index =
		    (IsReversedBinary(decoded.opcode) && decoded.src_count == 2u) ? 1u - i : i;
		if (!LowerSourceOperand(DecodedSourceAt(decoded, src_index), &inst.src[i], error)) {
			return false;
		}
	}

	block->instructions.push_back(inst);
	return AppendScalarResultSccNonZero(decoded, block, error);
}

} // namespace

bool LowerProgram(const Decoder::Program& decoded, const CFG::Graph& cfg, ShaderType stage,
                  uint32_t wave_size, Program* program, std::string* error) {
	if (program == nullptr) {
		SetError(error, "invalid IR output");
		return false;
	}
	if (wave_size != 32u && wave_size != 64u) {
		EXIT("invalid shader wave size: %u\n", wave_size);
	}

	*program                     = {};
	program->stage               = stage;
	program->wave_size           = wave_size;
	program->dispatcher_fallback = cfg.irreducible;
	program->cfg_failure_kind    = cfg.failure_kind;
	program->fallback_reason     = cfg.unsupported_reason;
	program->blocks.clear();

	if (cfg.blocks.empty()) {
		SetError(error, "cannot lower empty CFG");
		return false;
	}

	for (const auto& cfg_block: cfg.blocks) {
		BasicBlock block;
		block.id           = cfg_block.id;
		block.start_pc     = cfg_block.start_pc;
		block.end_pc       = cfg_block.end_pc;
		block.inst_begin   = cfg_block.inst_begin;
		block.inst_end     = cfg_block.inst_end;
		block.predecessors = cfg_block.predecessors;
		block.successors   = cfg_block.successors;
		block.terminator   = cfg_block.terminator;

		for (uint32_t i = cfg_block.inst_begin; i < cfg_block.inst_end; i++) {
			if (i >= decoded.instructions.size()) {
				SetError(error, "CFG block references instruction outside decoded program");
				return false;
			}
			if (std::find(cfg.code_table_load_pcs.begin(), cfg.code_table_load_pcs.end(),
			              decoded.instructions[i].pc) != cfg.code_table_load_pcs.end()) {
				continue;
			}
			if (!LowerDecodedInstruction(decoded.instructions[i], &block, error)) {
				return false;
			}
		}

		program->blocks.push_back(std::move(block));
	}

	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
