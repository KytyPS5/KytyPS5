#include "graphics/shader/recompiler/MemoryOps.h"

#include <fmt/format.h>
#include <iterator>

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

struct MemoryOpcodeInfo {
	uint32_t opcode      = 0;
	Opcode   decoded     = Opcode::Unsupported;
	uint32_t data_dwords = 1;
	uint32_t data_bits   = 32;
	bool     data_signed = false;
	bool     typed       = false;
	bool     formatted   = false;
};

constexpr MemoryOpcodeInfo SMEM_OPS[] = {
    {0x00u, Opcode::SLoadDword, 1, 32},         {0x01u, Opcode::SLoadDwordx2, 2, 32},
    {0x02u, Opcode::SLoadDwordx4, 4, 32},       {0x03u, Opcode::SLoadDwordx8, 8, 32},
    {0x04u, Opcode::SLoadDwordx16, 16, 32},     {0x08u, Opcode::SBufferLoadDword, 1, 32},
    {0x09u, Opcode::SBufferLoadDwordx2, 2, 32}, {0x0au, Opcode::SBufferLoadDwordx4, 4, 32},
    {0x0bu, Opcode::SBufferLoadDwordx8, 8, 32}, {0x0cu, Opcode::SBufferLoadDwordx16, 16, 32},
};

constexpr MemoryOpcodeInfo MUBUF_OPS[] = {
    {0x00u, Opcode::BufferLoadFormatX, 1, 32, false, false, true},
    {0x01u, Opcode::BufferLoadFormatXy, 2, 32, false, false, true},
    {0x02u, Opcode::BufferLoadFormatXyz, 3, 32, false, false, true},
    {0x03u, Opcode::BufferLoadFormatXyzw, 4, 32, false, false, true},
    {0x04u, Opcode::BufferStoreFormatX, 1, 32, false, false, true},
    {0x05u, Opcode::BufferStoreFormatXy, 2, 32, false, false, true},
    {0x06u, Opcode::BufferStoreFormatXyz, 3, 32, false, false, true},
    {0x07u, Opcode::BufferStoreFormatXyzw, 4, 32, false, false, true},
    {0x08u, Opcode::BufferLoadUbyte, 1, 8},
    {0x09u, Opcode::BufferLoadSbyte, 1, 8, true},
    {0x0au, Opcode::BufferLoadUshort, 1, 16},
    {0x0bu, Opcode::BufferLoadSshort, 1, 16, true},
    {0x0cu, Opcode::BufferLoadDword, 1, 32},
    {0x0du, Opcode::BufferLoadDwordx2, 2, 32},
    {0x0eu, Opcode::BufferLoadDwordx4, 4, 32},
    {0x0fu, Opcode::BufferLoadDwordx3, 3, 32},
    {0x18u, Opcode::BufferStoreByte, 1, 8},
    {0x1au, Opcode::BufferStoreShort, 1, 16},
    {0x1cu, Opcode::BufferStoreDword, 1, 32},
    {0x1du, Opcode::BufferStoreDwordx2, 2, 32},
    {0x1eu, Opcode::BufferStoreDwordx4, 4, 32},
    {0x1fu, Opcode::BufferStoreDwordx3, 3, 32},
    {0x30u, Opcode::BufferAtomicSwap, 1, 32},
    {0x32u, Opcode::BufferAtomicAdd, 1, 32},
    {0x33u, Opcode::BufferAtomicSub, 1, 32},
    {0x35u, Opcode::BufferAtomicSMin, 1, 32},
    {0x36u, Opcode::BufferAtomicUMin, 1, 32},
    {0x37u, Opcode::BufferAtomicSMax, 1, 32},
    {0x38u, Opcode::BufferAtomicUMax, 1, 32},
    {0x39u, Opcode::BufferAtomicAnd, 1, 32},
    {0x3au, Opcode::BufferAtomicOr, 1, 32},
    {0x3bu, Opcode::BufferAtomicXor, 1, 32},
};

constexpr MemoryOpcodeInfo MTBUF_OPS[] = {
    {0x00u, Opcode::TBufferLoadFormatX, 1, 32, false, true, true},
    {0x01u, Opcode::TBufferLoadFormatXy, 2, 32, false, true, true},
    {0x02u, Opcode::TBufferLoadFormatXyz, 3, 32, false, true, true},
    {0x03u, Opcode::TBufferLoadFormatXyzw, 4, 32, false, true, true},
    {0x04u, Opcode::TBufferStoreFormatX, 1, 32, false, true, true},
    {0x05u, Opcode::TBufferStoreFormatXy, 2, 32, false, true, true},
    {0x06u, Opcode::TBufferStoreFormatXyz, 3, 32, false, true, true},
    {0x07u, Opcode::TBufferStoreFormatXyzw, 4, 32, false, true, true},
};

constexpr MemoryOpcodeInfo FLAT_OPS[] = {
    {0x08u, Opcode::FlatLoadUbyte, 1, 8},     {0x09u, Opcode::FlatLoadSbyte, 1, 8, true},
    {0x0au, Opcode::FlatLoadUshort, 1, 16},   {0x0bu, Opcode::FlatLoadSshort, 1, 16, true},
    {0x0cu, Opcode::FlatLoadDword, 1, 32},    {0x0du, Opcode::FlatLoadDwordx2, 2, 32},
    {0x0eu, Opcode::FlatLoadDwordx4, 4, 32},  {0x0fu, Opcode::FlatLoadDwordx3, 3, 32},
    {0x18u, Opcode::FlatStoreByte, 1, 8},     {0x1au, Opcode::FlatStoreShort, 1, 16},
    {0x1cu, Opcode::FlatStoreDword, 1, 32},   {0x1du, Opcode::FlatStoreDwordx2, 2, 32},
    {0x1eu, Opcode::FlatStoreDwordx4, 4, 32}, {0x1fu, Opcode::FlatStoreDwordx3, 3, 32},
};

constexpr MemoryOpcodeInfo DS_OPS[] = {
    {0x00u, Opcode::DsAddU32, 1, 32},           {0x01u, Opcode::DsSubU32, 1, 32},
    {0x05u, Opcode::DsMinI32, 1, 32},           {0x06u, Opcode::DsMaxI32, 1, 32},
    {0x07u, Opcode::DsMinU32, 1, 32},           {0x08u, Opcode::DsMaxU32, 1, 32},
    {0x09u, Opcode::DsAndB32, 1, 32},           {0x0au, Opcode::DsOrB32, 1, 32},
    {0x0bu, Opcode::DsXorB32, 1, 32},           {0x0du, Opcode::DsWriteB32, 1, 32},
    {0x0eu, Opcode::DsWrite2B32, 2, 32},        {0x0fu, Opcode::DsWrite2St64B32, 2, 32},
    {0x12u, Opcode::DsMinF32, 1, 32},           {0x13u, Opcode::DsMaxF32, 1, 32},
    {0x1eu, Opcode::DsWriteByte, 1, 8},         {0x1fu, Opcode::DsWriteShort, 1, 16},
    {0x20u, Opcode::DsAddRtnU32, 1, 32},        {0x21u, Opcode::DsSubRtnU32, 1, 32},
    {0x25u, Opcode::DsMinRtnI32, 1, 32},        {0x26u, Opcode::DsMaxRtnI32, 1, 32},
    {0x27u, Opcode::DsMinRtnU32, 1, 32},        {0x28u, Opcode::DsMaxRtnU32, 1, 32},
    {0x29u, Opcode::DsAndRtnB32, 1, 32},        {0x2au, Opcode::DsOrRtnB32, 1, 32},
    {0x2bu, Opcode::DsXorRtnB32, 1, 32},        {0x2du, Opcode::DsWrxchgRtnB32, 1, 32},
    {0x35u, Opcode::DsSwizzleB32, 1, 32},       {0x36u, Opcode::DsReadB32, 1, 32},
    {0x37u, Opcode::DsRead2B32, 2, 32},         {0x38u, Opcode::DsRead2B32, 2, 32},
    {0x39u, Opcode::DsReadSbyte, 1, 8, true},   {0x3au, Opcode::DsReadUbyte, 1, 8},
    {0x3bu, Opcode::DsReadSshort, 1, 16, true}, {0x3cu, Opcode::DsReadUshort, 1, 16},
    {0x3du, Opcode::DsConsume, 1, 32},          {0x3eu, Opcode::DsAppend, 1, 32},
    {0x4du, Opcode::DsWriteB64, 2, 32},         {0x4eu, Opcode::DsWrite2B64, 4, 32},
    {0x4fu, Opcode::DsWrite2St64B64, 4, 32},    {0x76u, Opcode::DsReadB64, 2, 32},
    {0x77u, Opcode::DsRead2B64, 4, 32},         {0x78u, Opcode::DsRead2St64B64, 4, 32},
    {0xb0u, Opcode::DsWriteAddtidB32, 1, 32},   {0xb1u, Opcode::DsReadAddtidB32, 1, 32},
    {0xdeu, Opcode::DsWriteB96, 3, 32},         {0xdfu, Opcode::DsWriteB128, 4, 32},
    {0xfeu, Opcode::DsReadB96, 3, 32},          {0xffu, Opcode::DsReadB128, 4, 32},
};

const MemoryOpcodeInfo* LookupMemoryOpcode(const MemoryOpcodeInfo* ops, uint32_t count,
                                           uint32_t opcode) {
	for (uint32_t i = 0; i < count; i++) {
		if (ops[i].opcode == opcode) {
			return &ops[i];
		}
	}
	return nullptr;
}

uint32_t SignExtendU32(uint32_t value, uint32_t bits) {
	if (bits == 0u || bits >= 32u) {
		return value;
	}
	const uint32_t sign = 1u << (bits - 1u);
	return (value ^ sign) - sign;
}

void MarkMemoryUnsupported(Instruction* inst, Family family, uint32_t opcode, const char* reason) {
	inst->family    = family;
	inst->opcode_id = opcode;
	inst->opcode    = Opcode::Unsupported;
	SetUnsupported(inst, family, opcode, reason);
}

void ApplyMemoryInfo(Instruction* inst, const MemoryOpcodeInfo* info) {
	if (info == nullptr) {
		inst->opcode = Opcode::Unsupported;
		return;
	}
	inst->opcode      = info->decoded;
	inst->data_dwords = info->data_dwords;
	inst->data_bits   = info->data_bits;
	inst->data_signed = info->data_signed;
	inst->typed       = info->typed;
	inst->formatted   = info->formatted;
}

bool IsDsWriteOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::DsWriteByte:
		case Opcode::DsWriteShort:
		case Opcode::DsWrite2B32:
		case Opcode::DsWrite2St64B32:
		case Opcode::DsWrite2B64:
		case Opcode::DsWrite2St64B64:
		case Opcode::DsWriteB32:
		case Opcode::DsWriteB64:
		case Opcode::DsWriteB96:
		case Opcode::DsWriteB128: return true;
		default: return false;
	}
}

bool IsDsAtomicOpcode(Opcode opcode) {
	switch (opcode) {
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
		case Opcode::DsWrxchgRtnB32: return true;
		default: return false;
	}
}

uint32_t DsSourceCount(Opcode opcode) {
	switch (opcode) {
		case Opcode::DsWrite2B32:
		case Opcode::DsWrite2St64B32:
		case Opcode::DsWrite2B64:
		case Opcode::DsWrite2St64B64:
		case Opcode::DsMinF32:
		case Opcode::DsMaxF32: return 3u;
		case Opcode::DsReadAddtidB32:
		case Opcode::DsConsume:
		case Opcode::DsAppend: return 0u;
		default: return IsDsWriteOpcode(opcode) || IsDsAtomicOpcode(opcode) ? 2u : 1u;
	}
}

bool IsFlatStoreOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::FlatStoreByte:
		case Opcode::FlatStoreShort:
		case Opcode::FlatStoreDword:
		case Opcode::FlatStoreDwordx2:
		case Opcode::FlatStoreDwordx3:
		case Opcode::FlatStoreDwordx4: return true;
		default: return false;
	}
}

} // namespace

bool DecodeSmem(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated SMEM instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word0   = code[word_index];
	const uint32_t word1   = code[word_index + 1u];
	const uint32_t opcode  = (word0 >> 18u) & 0xffu;
	const uint32_t sdst    = (word0 >> 6u) & 0x7fu;
	const uint32_t sbase   = word0 & 0x3fu;
	const uint32_t soffset = (word1 >> 25u) & 0x7fu;

	inst->pc         = pc;
	inst->word       = word0;
	inst->word_count = 2;
	inst->offset     = SignExtendU32(word1 & 0x1fffffu, 21u);
	inst->glc        = ((word0 >> 16u) & 1u) != 0;
	inst->family     = Family::SMEM;
	inst->opcode_id  = opcode;
	const auto* info =
	    LookupMemoryOpcode(SMEM_OPS, static_cast<uint32_t>(std::size(SMEM_OPS)), opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);
	if (inst->opcode == Opcode::Unsupported) {
		MarkMemoryUnsupported(inst, Family::SMEM, opcode, "SMEM opcode is not implemented");
	}

	DecodeScalarDestination(sdst, pc, &inst->dst, nullptr);
	// SMEM encodes SBASE in SGPR pairs. Scalar-buffer loads still use the same
	// pair index; their descriptor operand consumes four SGPRs from that base.
	DecodeScalarSource(sbase * 2u, pc, &inst->src0, nullptr);
	DecodeScalarSource(soffset, pc, &inst->src1, nullptr);
	inst->src_count = 2;
	return true;
}

bool DecodeMubuf(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction* inst, std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated MUBUF instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word0   = code[word_index];
	const uint32_t word1   = code[word_index + 1u];
	const uint32_t opcode  = ((word0 >> 18u) & 0x7fu) | (((word0 >> 25u) & 1u) << 7u);
	const uint32_t vdata   = (word1 >> 8u) & 0xffu;
	const uint32_t vaddr   = word1 & 0xffu;
	const uint32_t srsrc   = (word1 >> 16u) & 0x1fu;
	const uint32_t soffset = (word1 >> 24u) & 0xffu;

	inst->pc         = pc;
	inst->word       = word0;
	inst->word_count = 2;
	inst->offset     = word0 & 0xfffu;
	inst->idxen      = ((word0 >> 13u) & 1u) != 0;
	inst->offen      = ((word0 >> 12u) & 1u) != 0;
	inst->glc        = ((word0 >> 14u) & 1u) != 0;
	inst->slc        = ((word1 >> 22u) & 1u) != 0;
	inst->family     = Family::MUBUF;
	inst->opcode_id  = opcode;
	const auto* info =
	    LookupMemoryOpcode(MUBUF_OPS, static_cast<uint32_t>(std::size(MUBUF_OPS)), opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);
	if (inst->opcode == Opcode::Unsupported) {
		MarkMemoryUnsupported(inst, Family::MUBUF, opcode, "MUBUF opcode is not implemented");
	}

	DecodeVectorGpr(vdata, &inst->dst, nullptr);
	DecodeVectorGpr(vaddr, &inst->src0, nullptr);
	DecodeScalarSource(srsrc * 4u, pc, &inst->src1, nullptr);
	DecodeScalarSource(soffset, pc, &inst->src2, nullptr);
	inst->src_count = 3;
	return true;
}

bool DecodeMtbuf(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction* inst, std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated MTBUF instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word0   = code[word_index];
	const uint32_t word1   = code[word_index + 1u];
	const uint32_t opcode  = ((word0 >> 16u) & 0x7u) | (((word1 >> 21u) & 1u) << 3u);
	const uint32_t dfmt    = (word0 >> 19u) & 0xfu;
	const uint32_t nfmt    = (word0 >> 23u) & 0x7u;
	const uint32_t vdata   = (word1 >> 8u) & 0xffu;
	const uint32_t vaddr   = word1 & 0xffu;
	const uint32_t srsrc   = (word1 >> 16u) & 0x1fu;
	const uint32_t soffset = (word1 >> 24u) & 0xffu;

	inst->pc            = pc;
	inst->word          = word0;
	inst->word_count    = 2;
	inst->offset        = word0 & 0xfffu;
	inst->idxen         = ((word0 >> 13u) & 1u) != 0;
	inst->offen         = ((word0 >> 12u) & 1u) != 0;
	inst->glc           = ((word0 >> 14u) & 1u) != 0;
	inst->slc           = ((word1 >> 22u) & 1u) != 0;
	inst->family        = Family::MTBUF;
	inst->opcode_id     = opcode;
	inst->data_format   = dfmt;
	inst->number_format = nfmt;
	const auto* info =
	    LookupMemoryOpcode(MTBUF_OPS, static_cast<uint32_t>(std::size(MTBUF_OPS)), opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);
	if (inst->opcode == Opcode::Unsupported) {
		MarkMemoryUnsupported(inst, Family::MTBUF, opcode, "MTBUF opcode is not implemented");
	}

	DecodeVectorGpr(vdata, &inst->dst, nullptr);
	DecodeVectorGpr(vaddr, &inst->src0, nullptr);
	DecodeScalarSource(srsrc * 4u, pc, &inst->src1, nullptr);
	DecodeScalarSource(soffset, pc, &inst->src2, nullptr);
	inst->src_count = 3;
	return true;
}

bool DecodeFlat(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated FLAT instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word0  = code[word_index];
	const uint32_t word1  = code[word_index + 1u];
	const uint32_t offset = word0 & 0xfffu;
	const uint32_t dlc    = (word0 >> 12u) & 1u;
	const uint32_t lds    = (word0 >> 13u) & 1u;
	const uint32_t seg    = (word0 >> 14u) & 0x3u;
	const uint32_t opcode = (word0 >> 18u) & 0x7fu;
	const uint32_t vdst   = (word1 >> 24u) & 0xffu;
	const uint32_t saddr  = (word1 >> 16u) & 0x7fu;
	const uint32_t data   = (word1 >> 8u) & 0xffu;
	const uint32_t addr   = word1 & 0xffu;

	inst->pc             = pc;
	inst->word           = word0;
	inst->word_count     = 2;
	inst->offset         = seg == 0u ? (offset & 0x7ffu) : SignExtendU32(offset, 12u);
	inst->glc            = ((word0 >> 16u) & 1u) != 0;
	inst->slc            = ((word0 >> 17u) & 1u) != 0;
	inst->family         = Family::FLAT;
	inst->opcode_id      = opcode;
	inst->memory_segment = seg;
	const auto* info =
	    LookupMemoryOpcode(FLAT_OPS, static_cast<uint32_t>(std::size(FLAT_OPS)), opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);

	if (dlc != 0 || lds != 0 || inst->glc || inst->slc || seg == 3u) {
		SetUnsupported(inst, Family::FLAT, opcode, "FLAT modifiers or segment are not implemented");
		return true;
	}
	if (inst->opcode == Opcode::Unsupported) {
		MarkMemoryUnsupported(inst, Family::FLAT, opcode, "FLAT opcode is not implemented");
		return true;
	}

	DecodeVectorGpr(IsFlatStoreOpcode(inst->opcode) ? data : vdst, &inst->dst, nullptr);
	DecodeVectorGpr(addr, &inst->src0, nullptr);
	inst->src_count = 1;
	if (seg == 0u || saddr == 0x7du || saddr == 0x7fu) {
		DecodeVectorGpr(addr + 1u, &inst->src1, nullptr);
		inst->src_count = 2;
	} else {
		DecodeScalarSource(saddr, pc, &inst->src1, nullptr);
		inst->src_count = 2;
	}
	return true;
}

bool DecodeDs(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
              std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated DS instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word0   = code[word_index];
	const uint32_t word1   = code[word_index + 1u];
	const uint32_t opcode  = (word0 >> 18u) & 0xffu;
	const uint32_t offset0 = word0 & 0xffu;
	const uint32_t offset1 = (word0 >> 8u) & 0xffu;
	const uint32_t vdst    = (word1 >> 24u) & 0xffu;
	const uint32_t data1   = (word1 >> 16u) & 0xffu;
	const uint32_t data0   = (word1 >> 8u) & 0xffu;
	const uint32_t addr    = word1 & 0xffu;

	inst->pc         = pc;
	inst->word       = word0;
	inst->word_count = 2;
	inst->offset     = offset0 | (offset1 << 8u);
	inst->gds        = ((word0 >> 17u) & 1u) != 0u;
	inst->family     = Family::DS;
	inst->opcode_id  = opcode;
	const auto* info = LookupMemoryOpcode(DS_OPS, static_cast<uint32_t>(std::size(DS_OPS)), opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);
	if (inst->opcode == Opcode::Unsupported) {
		MarkMemoryUnsupported(inst, Family::DS, opcode, "DS opcode is not implemented");
	}
	if (inst->opcode == Opcode::DsSwizzleB32 && inst->offset >= 0xe000u) {
		SetUnsupported(inst, Family::DS, opcode, "DS swizzle FFT mode is not implemented");
	}
	if (inst->gds &&
	    (inst->opcode == Opcode::DsSwizzleB32 || inst->opcode == Opcode::DsWriteAddtidB32 ||
	     inst->opcode == Opcode::DsReadAddtidB32)) {
		SetUnsupported(inst, Family::DS, opcode, "DS swizzle/addtid is available only for LDS");
	}
	if (inst->gds && (inst->opcode == Opcode::DsAppend || inst->opcode == Opcode::DsConsume) &&
	    inst->offset != 0u) {
		SetUnsupported(inst, Family::DS, opcode,
		               "GDS append/consume requires a zero instruction offset");
	}
	if (inst->opcode == Opcode::DsWriteAddtidB32 && data1 != 0u) {
		SetUnsupported(inst, Family::DS, opcode,
		               "DS write addtid data1 operand is not implemented");
	}
	if (inst->opcode == Opcode::DsReadAddtidB32 && (data0 != 0u || data1 != 0u)) {
		SetUnsupported(inst, Family::DS, opcode,
		               "DS read addtid data operands are not implemented");
	}
	if (inst->opcode == Opcode::DsWrite2B32 || opcode == 0x37u) {
		inst->offset           = offset0 * 4u;
		inst->secondary_offset = offset1 * 4u;
	} else if (inst->opcode == Opcode::DsWrite2St64B32 || opcode == 0x38u) {
		inst->offset           = offset0 * 256u;
		inst->secondary_offset = offset1 * 256u;
	} else if (inst->opcode == Opcode::DsWrite2B64 || inst->opcode == Opcode::DsRead2B64) {
		inst->offset           = offset0 * 8u;
		inst->secondary_offset = offset1 * 8u;
	} else if (inst->opcode == Opcode::DsWrite2St64B64 || inst->opcode == Opcode::DsRead2St64B64) {
		inst->offset           = offset0 * 512u;
		inst->secondary_offset = offset1 * 512u;
	}

	DecodeVectorGpr(vdst, &inst->dst, nullptr);
	DecodeVectorGpr(addr, &inst->src0, nullptr);
	DecodeVectorGpr(data0, &inst->src1, nullptr);
	DecodeVectorGpr(data1, &inst->src2, nullptr);
	inst->src_count = DsSourceCount(inst->opcode);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
