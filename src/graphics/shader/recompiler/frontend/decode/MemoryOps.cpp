#include "graphics/shader/recompiler/frontend/decode/MemoryOps.h"

#include "graphics/shader/recompiler/frontend/decode/OpcodeTable.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

struct MemoryOpcodeInfo {
	uint32_t encoding    = 0;
	Opcode   decoded     = Opcode::UNSUPPORTED;
	uint32_t data_dwords = 1;
	uint32_t data_bits   = 32;
	bool     data_signed = false;
	bool     typed       = false;
	bool     formatted   = false;
};

constexpr MemoryOpcodeInfo SMEM_OPCODE_LIST[] = {
    {0x00u, Opcode::S_LOAD_DWORD, 1, 32},          {0x01u, Opcode::S_LOAD_DWORDX2, 2, 32},
    {0x02u, Opcode::S_LOAD_DWORDX4, 4, 32},        {0x03u, Opcode::S_LOAD_DWORDX8, 8, 32},
    {0x04u, Opcode::S_LOAD_DWORDX16, 16, 32},      {0x08u, Opcode::S_BUFFER_LOAD_DWORD, 1, 32},
    {0x09u, Opcode::S_BUFFER_LOAD_DWORDX2, 2, 32}, {0x0au, Opcode::S_BUFFER_LOAD_DWORDX4, 4, 32},
    {0x0bu, Opcode::S_BUFFER_LOAD_DWORDX8, 8, 32}, {0x0cu, Opcode::S_BUFFER_LOAD_DWORDX16, 16, 32},
};

constexpr MemoryOpcodeInfo MUBUF_OPCODE_LIST[] = {
    {0x00u, Opcode::BUFFER_LOAD_FORMAT_X, 1, 32, false, false, true},
    {0x01u, Opcode::BUFFER_LOAD_FORMAT_XY, 2, 32, false, false, true},
    {0x02u, Opcode::BUFFER_LOAD_FORMAT_XYZ, 3, 32, false, false, true},
    {0x03u, Opcode::BUFFER_LOAD_FORMAT_XYZW, 4, 32, false, false, true},
    {0x04u, Opcode::BUFFER_STORE_FORMAT_X, 1, 32, false, false, true},
    {0x05u, Opcode::BUFFER_STORE_FORMAT_XY, 2, 32, false, false, true},
    {0x06u, Opcode::BUFFER_STORE_FORMAT_XYZ, 3, 32, false, false, true},
    {0x07u, Opcode::BUFFER_STORE_FORMAT_XYZW, 4, 32, false, false, true},
    {0x08u, Opcode::BUFFER_LOAD_UBYTE, 1, 8},
    {0x09u, Opcode::BUFFER_LOAD_SBYTE, 1, 8, true},
    {0x0au, Opcode::BUFFER_LOAD_USHORT, 1, 16},
    {0x0bu, Opcode::BUFFER_LOAD_SSHORT, 1, 16, true},
    {0x0cu, Opcode::BUFFER_LOAD_DWORD, 1, 32},
    {0x0du, Opcode::BUFFER_LOAD_DWORDX2, 2, 32},
    {0x0eu, Opcode::BUFFER_LOAD_DWORDX4, 4, 32},
    {0x0fu, Opcode::BUFFER_LOAD_DWORDX3, 3, 32},
    {0x18u, Opcode::BUFFER_STORE_BYTE, 1, 8},
    {0x1au, Opcode::BUFFER_STORE_SHORT, 1, 16},
    {0x1cu, Opcode::BUFFER_STORE_DWORD, 1, 32},
    {0x1du, Opcode::BUFFER_STORE_DWORDX2, 2, 32},
    {0x1eu, Opcode::BUFFER_STORE_DWORDX4, 4, 32},
    {0x1fu, Opcode::BUFFER_STORE_DWORDX3, 3, 32},
    {0x30u, Opcode::BUFFER_ATOMIC_SWAP, 1, 32},
    {0x32u, Opcode::BUFFER_ATOMIC_ADD, 1, 32},
    {0x33u, Opcode::BUFFER_ATOMIC_SUB, 1, 32},
    {0x35u, Opcode::BUFFER_ATOMIC_SMIN, 1, 32},
    {0x36u, Opcode::BUFFER_ATOMIC_UMIN, 1, 32},
    {0x37u, Opcode::BUFFER_ATOMIC_SMAX, 1, 32},
    {0x38u, Opcode::BUFFER_ATOMIC_UMAX, 1, 32},
    {0x39u, Opcode::BUFFER_ATOMIC_AND, 1, 32},
    {0x3au, Opcode::BUFFER_ATOMIC_OR, 1, 32},
    {0x3bu, Opcode::BUFFER_ATOMIC_XOR, 1, 32},
    {0x3fu, Opcode::BUFFER_ATOMIC_FMIN, 1, 32},
    {0x40u, Opcode::BUFFER_ATOMIC_FMAX, 1, 32},
};

constexpr MemoryOpcodeInfo MTBUF_OPCODE_LIST[] = {
    {0x00u, Opcode::TBUFFER_LOAD_FORMAT_X, 1, 32, false, true, true},
    {0x01u, Opcode::TBUFFER_LOAD_FORMAT_XY, 2, 32, false, true, true},
    {0x02u, Opcode::TBUFFER_LOAD_FORMAT_XYZ, 3, 32, false, true, true},
    {0x03u, Opcode::TBUFFER_LOAD_FORMAT_XYZW, 4, 32, false, true, true},
    {0x04u, Opcode::TBUFFER_STORE_FORMAT_X, 1, 32, false, true, true},
    {0x05u, Opcode::TBUFFER_STORE_FORMAT_XY, 2, 32, false, true, true},
    {0x06u, Opcode::TBUFFER_STORE_FORMAT_XYZ, 3, 32, false, true, true},
    {0x07u, Opcode::TBUFFER_STORE_FORMAT_XYZW, 4, 32, false, true, true},
};

constexpr MemoryOpcodeInfo FLAT_OPCODE_LIST[] = {
    {0x08u, Opcode::FLAT_LOAD_UBYTE, 1, 8},     {0x09u, Opcode::FLAT_LOAD_SBYTE, 1, 8, true},
    {0x0au, Opcode::FLAT_LOAD_USHORT, 1, 16},   {0x0bu, Opcode::FLAT_LOAD_SSHORT, 1, 16, true},
    {0x0cu, Opcode::FLAT_LOAD_DWORD, 1, 32},    {0x0du, Opcode::FLAT_LOAD_DWORDX2, 2, 32},
    {0x0eu, Opcode::FLAT_LOAD_DWORDX4, 4, 32},  {0x0fu, Opcode::FLAT_LOAD_DWORDX3, 3, 32},
    {0x18u, Opcode::FLAT_STORE_BYTE, 1, 8},     {0x1au, Opcode::FLAT_STORE_SHORT, 1, 16},
    {0x1cu, Opcode::FLAT_STORE_DWORD, 1, 32},   {0x1du, Opcode::FLAT_STORE_DWORDX2, 2, 32},
    {0x1eu, Opcode::FLAT_STORE_DWORDX4, 4, 32}, {0x1fu, Opcode::FLAT_STORE_DWORDX3, 3, 32},
};

constexpr MemoryOpcodeInfo DS_OPCODE_LIST[] = {
    {0x00u, Opcode::DS_ADD_U32, 1, 32},          {0x01u, Opcode::DS_SUB_U32, 1, 32},
    {0x05u, Opcode::DS_MIN_I32, 1, 32},          {0x06u, Opcode::DS_MAX_I32, 1, 32},
    {0x07u, Opcode::DS_MIN_U32, 1, 32},          {0x08u, Opcode::DS_MAX_U32, 1, 32},
    {0x09u, Opcode::DS_AND_B32, 1, 32},          {0x0au, Opcode::DS_OR_B32, 1, 32},
    {0x0bu, Opcode::DS_XOR_B32, 1, 32},          {0x0du, Opcode::DS_WRITE_B32, 1, 32},
    {0x0eu, Opcode::DS_WRITE2_B32, 2, 32},       {0x0fu, Opcode::DS_WRITE2ST64_B32, 2, 32},
    {0x12u, Opcode::DS_MIN_F32, 1, 32},          {0x13u, Opcode::DS_MAX_F32, 1, 32},
    {0x1eu, Opcode::DS_WRITE_B8, 1, 8},          {0x1fu, Opcode::DS_WRITE_B16, 1, 16},
    {0x20u, Opcode::DS_ADD_RTN_U32, 1, 32},      {0x21u, Opcode::DS_SUB_RTN_U32, 1, 32},
    {0x25u, Opcode::DS_MIN_RTN_I32, 1, 32},      {0x26u, Opcode::DS_MAX_RTN_I32, 1, 32},
    {0x27u, Opcode::DS_MIN_RTN_U32, 1, 32},      {0x28u, Opcode::DS_MAX_RTN_U32, 1, 32},
    {0x29u, Opcode::DS_AND_RTN_B32, 1, 32},      {0x2au, Opcode::DS_OR_RTN_B32, 1, 32},
    {0x2bu, Opcode::DS_XOR_RTN_B32, 1, 32},      {0x2du, Opcode::DS_WRXCHG_RTN_B32, 1, 32},
    {0x35u, Opcode::DS_SWIZZLE_B32, 1, 32},      {0x36u, Opcode::DS_READ_B32, 1, 32},
    {0x37u, Opcode::DS_READ2_B32, 2, 32},        {0x38u, Opcode::DS_READ2ST64_B32, 2, 32},
    {0x39u, Opcode::DS_READ_I8, 1, 8, true},     {0x3au, Opcode::DS_READ_U8, 1, 8},
    {0x3bu, Opcode::DS_READ_I16, 1, 16, true},   {0x3cu, Opcode::DS_READ_U16, 1, 16},
    {0x3du, Opcode::DS_CONSUME, 1, 32},          {0x3eu, Opcode::DS_APPEND, 1, 32},
    {0x4du, Opcode::DS_WRITE_B64, 2, 32},        {0x4eu, Opcode::DS_WRITE2_B64, 4, 32},
    {0x4fu, Opcode::DS_WRITE2ST64_B64, 4, 32},   {0x76u, Opcode::DS_READ_B64, 2, 32},
    {0x77u, Opcode::DS_READ2_B64, 4, 32},        {0x78u, Opcode::DS_READ2ST64_B64, 4, 32},
    {0xa6u, Opcode::DS_READ_U16_D16, 1, 16},
    {0xb0u, Opcode::DS_WRITE_ADDTID_B32, 1, 32}, {0xb1u, Opcode::DS_READ_ADDTID_B32, 1, 32},
    {0xdeu, Opcode::DS_WRITE_B96, 3, 32},        {0xdfu, Opcode::DS_WRITE_B128, 4, 32},
    {0xfeu, Opcode::DS_READ_B96, 3, 32},         {0xffu, Opcode::DS_READ_B128, 4, 32},
};

constexpr auto SMEM_OPS  = Detail::MakeOpcodeTable<0x100>(SMEM_OPCODE_LIST);
constexpr auto MUBUF_OPS = Detail::MakeOpcodeTable<0x100>(MUBUF_OPCODE_LIST);
constexpr auto MTBUF_OPS = Detail::MakeOpcodeTable<0x10>(MTBUF_OPCODE_LIST);
constexpr auto FLAT_OPS  = Detail::MakeOpcodeTable<0x80>(FLAT_OPCODE_LIST);
constexpr auto DS_OPS    = Detail::MakeOpcodeTable<0x100>(DS_OPCODE_LIST);

uint32_t SignExtendU32(uint32_t value, uint32_t bits) {
	if (bits == 0u || bits >= 32u) {
		return value;
	}
	const uint32_t sign = 1u << (bits - 1u);
	return (value ^ sign) - sign;
}

void MarkMemoryUnsupported(Instruction& inst, Family family, uint32_t opcode, const char* reason) {
	inst.family    = family;
	inst.opcode_id = opcode;
	inst.opcode    = Opcode::UNSUPPORTED;
	SetUnsupported(inst, family, opcode, reason);
}

void ApplyMemoryInfo(Instruction& inst, const MemoryOpcodeInfo* info) {
	if (info == nullptr) {
		inst.opcode = Opcode::UNSUPPORTED;
		return;
	}
	inst.opcode      = info->decoded;
	inst.data_dwords = info->data_dwords;
	inst.data_bits   = info->data_bits;
	inst.data_signed = info->data_signed;
	inst.typed       = info->typed;
	inst.formatted   = info->formatted;
}

bool IsDsWriteOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::DS_WRITE_B8:
		case Opcode::DS_WRITE_B16:
		case Opcode::DS_WRITE2_B32:
		case Opcode::DS_WRITE2ST64_B32:
		case Opcode::DS_WRITE2_B64:
		case Opcode::DS_WRITE2ST64_B64:
		case Opcode::DS_WRITE_B32:
		case Opcode::DS_WRITE_B64:
		case Opcode::DS_WRITE_B96:
		case Opcode::DS_WRITE_B128: return true;
		default: return false;
	}
}

bool IsDsAtomicOpcode(Opcode opcode) {
	switch (opcode) {
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
		case Opcode::DS_WRXCHG_RTN_B32: return true;
		default: return false;
	}
}

uint32_t DsSourceCount(Opcode opcode) {
	switch (opcode) {
		case Opcode::DS_WRITE2_B32:
		case Opcode::DS_WRITE2ST64_B32:
		case Opcode::DS_WRITE2_B64:
		case Opcode::DS_WRITE2ST64_B64:
		case Opcode::DS_MIN_F32:
		case Opcode::DS_MAX_F32: return 3u;
		case Opcode::DS_READ_ADDTID_B32:
		case Opcode::DS_CONSUME:
		case Opcode::DS_APPEND: return 0u;
		default: return IsDsWriteOpcode(opcode) || IsDsAtomicOpcode(opcode) ? 2u : 1u;
	}
}

bool IsFlatStoreOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::FLAT_STORE_BYTE:
		case Opcode::FLAT_STORE_SHORT:
		case Opcode::FLAT_STORE_DWORD:
		case Opcode::FLAT_STORE_DWORDX2:
		case Opcode::FLAT_STORE_DWORDX3:
		case Opcode::FLAT_STORE_DWORDX4: return true;
		default: return false;
	}
}

} // namespace

void DecodeSmem(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst) {
	const uint32_t word0   = code[word_index];
	const uint32_t word1   = code[word_index + 1u];
	const uint32_t opcode  = (word0 >> 18u) & 0xffu;
	const uint32_t sdst    = (word0 >> 6u) & 0x7fu;
	const uint32_t sbase   = word0 & 0x3fu;
	const uint32_t soffset = (word1 >> 25u) & 0x7fu;

	inst.pc          = pc;
	inst.word        = word0;
	inst.word_count  = 2;
	inst.offset      = SignExtendU32(word1 & 0x1fffffu, 21u);
	inst.glc         = ((word0 >> 16u) & 1u) != 0;
	inst.family      = Family::SMEM;
	inst.opcode_id   = opcode;
	const auto* info = Detail::FindOpcode(SMEM_OPS, opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);
	if (inst.opcode == Opcode::UNSUPPORTED) {
		MarkMemoryUnsupported(inst, Family::SMEM, opcode, "SMEM opcode is not implemented");
	}

	DecodeScalarDestination(sdst, pc, inst.dst, nullptr);
	// SMEM encodes SBASE in SGPR pairs. Scalar-buffer loads still use the same
	// pair index; their descriptor operand consumes four SGPRs from that base.
	DecodeScalarSource(sbase * 2u, pc, inst.src0, nullptr);
	DecodeScalarSource(soffset, pc, inst.src1, nullptr);
	inst.src_count = 2;
}

void DecodeMubuf(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction& inst) {
	const uint32_t word0   = code[word_index];
	const uint32_t word1   = code[word_index + 1u];
	const uint32_t opcode  = ((word0 >> 18u) & 0x7fu) | (((word0 >> 25u) & 1u) << 7u);
	const uint32_t vdata   = (word1 >> 8u) & 0xffu;
	const uint32_t vaddr   = word1 & 0xffu;
	const uint32_t srsrc   = (word1 >> 16u) & 0x1fu;
	const uint32_t soffset = (word1 >> 24u) & 0xffu;

	inst.pc          = pc;
	inst.word        = word0;
	inst.word_count  = 2;
	inst.offset      = word0 & 0xfffu;
	inst.idxen       = ((word0 >> 13u) & 1u) != 0;
	inst.offen       = ((word0 >> 12u) & 1u) != 0;
	inst.glc         = ((word0 >> 14u) & 1u) != 0;
	inst.slc         = ((word1 >> 22u) & 1u) != 0;
	inst.family      = Family::MUBUF;
	inst.opcode_id   = opcode;
	const auto* info = Detail::FindOpcode(MUBUF_OPS, opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);
	if (inst.opcode == Opcode::UNSUPPORTED) {
		MarkMemoryUnsupported(inst, Family::MUBUF, opcode, "MUBUF opcode is not implemented");
	}

	DecodeVectorGpr(vdata, inst.dst);
	DecodeVectorGpr(vaddr, inst.src0);
	DecodeScalarSource(srsrc * 4u, pc, inst.src1, nullptr);
	DecodeScalarSource(soffset, pc, inst.src2, nullptr);
	inst.src_count = 3;
}

void DecodeMtbuf(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                 Instruction& inst) {
	const uint32_t word0   = code[word_index];
	const uint32_t word1   = code[word_index + 1u];
	const uint32_t opcode  = ((word0 >> 16u) & 0x7u) | (((word1 >> 21u) & 1u) << 3u);
	const uint32_t dfmt    = (word0 >> 19u) & 0xfu;
	const uint32_t nfmt    = (word0 >> 23u) & 0x7u;
	const uint32_t vdata   = (word1 >> 8u) & 0xffu;
	const uint32_t vaddr   = word1 & 0xffu;
	const uint32_t srsrc   = (word1 >> 16u) & 0x1fu;
	const uint32_t soffset = (word1 >> 24u) & 0xffu;

	inst.pc            = pc;
	inst.word          = word0;
	inst.word_count    = 2;
	inst.offset        = word0 & 0xfffu;
	inst.idxen         = ((word0 >> 13u) & 1u) != 0;
	inst.offen         = ((word0 >> 12u) & 1u) != 0;
	inst.glc           = ((word0 >> 14u) & 1u) != 0;
	inst.slc           = ((word1 >> 22u) & 1u) != 0;
	inst.family        = Family::MTBUF;
	inst.opcode_id     = opcode;
	inst.data_format   = dfmt;
	inst.number_format = nfmt;
	const auto* info   = Detail::FindOpcode(MTBUF_OPS, opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);
	if (inst.opcode == Opcode::UNSUPPORTED) {
		MarkMemoryUnsupported(inst, Family::MTBUF, opcode, "MTBUF opcode is not implemented");
	}

	DecodeVectorGpr(vdata, inst.dst);
	DecodeVectorGpr(vaddr, inst.src0);
	DecodeScalarSource(srsrc * 4u, pc, inst.src1, nullptr);
	DecodeScalarSource(soffset, pc, inst.src2, nullptr);
	inst.src_count = 3;
}

void DecodeFlat(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst) {
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

	inst.pc             = pc;
	inst.word           = word0;
	inst.word_count     = 2;
	inst.offset         = seg == 0u ? (offset & 0x7ffu) : SignExtendU32(offset, 12u);
	inst.glc            = ((word0 >> 16u) & 1u) != 0;
	inst.slc            = ((word0 >> 17u) & 1u) != 0;
	inst.family         = Family::FLAT;
	inst.opcode_id      = opcode;
	inst.memory_segment = seg;
	const auto* info    = Detail::FindOpcode(FLAT_OPS, opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);

	if (dlc != 0 || lds != 0 || inst.glc || inst.slc || seg == 3u) {
		SetUnsupported(inst, Family::FLAT, opcode, "FLAT modifiers or segment are not implemented");
		return;
	}
	if (inst.opcode == Opcode::UNSUPPORTED) {
		MarkMemoryUnsupported(inst, Family::FLAT, opcode, "FLAT opcode is not implemented");
		return;
	}

	DecodeVectorGpr(IsFlatStoreOpcode(inst.opcode) ? data : vdst, inst.dst);
	DecodeVectorGpr(addr, inst.src0);
	inst.src_count = 1;
	if (seg == 0u || saddr == 0x7du || saddr == 0x7fu) {
		DecodeVectorGpr(addr + 1u, inst.src1);
		inst.src_count = 2;
	} else {
		DecodeScalarSource(saddr, pc, inst.src1, nullptr);
		inst.src_count = 2;
	}
}

void DecodeDs(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction& inst) {
	const uint32_t word0   = code[word_index];
	const uint32_t word1   = code[word_index + 1u];
	const uint32_t opcode  = (word0 >> 18u) & 0xffu;
	const uint32_t offset0 = word0 & 0xffu;
	const uint32_t offset1 = (word0 >> 8u) & 0xffu;
	const uint32_t vdst    = (word1 >> 24u) & 0xffu;
	const uint32_t data1   = (word1 >> 16u) & 0xffu;
	const uint32_t data0   = (word1 >> 8u) & 0xffu;
	const uint32_t addr    = word1 & 0xffu;

	inst.pc          = pc;
	inst.word        = word0;
	inst.word_count  = 2;
	inst.offset      = offset0 | (offset1 << 8u);
	inst.gds         = ((word0 >> 17u) & 1u) != 0u;
	inst.family      = Family::DS;
	inst.opcode_id   = opcode;
	const auto* info = Detail::FindOpcode(DS_OPS, opcode);
	ApplyMemoryInfo(inst, info);
	SetRawWords(inst, code, word_index, 2);
	if (inst.opcode == Opcode::UNSUPPORTED) {
		MarkMemoryUnsupported(inst, Family::DS, opcode, "DS opcode is not implemented");
	}
	if (inst.opcode == Opcode::DS_SWIZZLE_B32 && inst.offset >= 0xe000u) {
		SetUnsupported(inst, Family::DS, opcode, "DS swizzle FFT mode is not implemented");
	}
	if (inst.gds &&
	    (inst.opcode == Opcode::DS_SWIZZLE_B32 || inst.opcode == Opcode::DS_WRITE_ADDTID_B32 ||
	     inst.opcode == Opcode::DS_READ_ADDTID_B32)) {
		SetUnsupported(inst, Family::DS, opcode, "DS swizzle/addtid is available only for LDS");
	}
	if (inst.opcode == Opcode::DS_WRITE_ADDTID_B32 && data1 != 0u) {
		SetUnsupported(inst, Family::DS, opcode,
		               "DS write addtid data1 operand is not implemented");
	}
	if (inst.opcode == Opcode::DS_READ_ADDTID_B32 && (data0 != 0u || data1 != 0u)) {
		SetUnsupported(inst, Family::DS, opcode,
		               "DS read addtid data operands are not implemented");
	}
	if (inst.opcode == Opcode::DS_WRITE2_B32 || inst.opcode == Opcode::DS_READ2_B32) {
		inst.offset           = offset0 * 4u;
		inst.secondary_offset = offset1 * 4u;
	} else if (inst.opcode == Opcode::DS_WRITE2ST64_B32 ||
	           inst.opcode == Opcode::DS_READ2ST64_B32) {
		inst.offset           = offset0 * 256u;
		inst.secondary_offset = offset1 * 256u;
	} else if (inst.opcode == Opcode::DS_WRITE2_B64 || inst.opcode == Opcode::DS_READ2_B64) {
		inst.offset           = offset0 * 8u;
		inst.secondary_offset = offset1 * 8u;
	} else if (inst.opcode == Opcode::DS_WRITE2ST64_B64 ||
	           inst.opcode == Opcode::DS_READ2ST64_B64) {
		inst.offset           = offset0 * 512u;
		inst.secondary_offset = offset1 * 512u;
	}

	DecodeVectorGpr(vdst, inst.dst);
	if (inst.opcode == Opcode::DS_READ_U16_D16) {
		// Native D16 reads update only the selected destination half. Reuse the partial
		// destination representation so typed IR translation preserves the upper word.
		inst.dst.sdwa_sel = 4u;
	}
	DecodeVectorGpr(addr, inst.src0);
	DecodeVectorGpr(data0, inst.src1);
	DecodeVectorGpr(data1, inst.src2);
	inst.src_count = DsSourceCount(inst.opcode);
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
