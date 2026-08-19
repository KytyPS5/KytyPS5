#include "common/assert.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/ShaderIRInternal.h"

#include <algorithm>
#include <fmt/format.h>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

Opcode BufferLoadOpcode(const Decoder::Instruction& decoded) {
	switch (decoded.data_bits) {
		case 8u: return decoded.data_signed ? Opcode::BufferLoadSbyte : Opcode::BufferLoadUbyte;
		case 16u: return decoded.data_signed ? Opcode::BufferLoadSshort : Opcode::BufferLoadUshort;
		default: return Opcode::BufferLoadDword;
	}
}

Opcode BufferStoreOpcode(uint32_t data_bits) {
	switch (data_bits) {
		case 8u: return Opcode::BufferStoreByte;
		case 16u: return Opcode::BufferStoreShort;
		default: return Opcode::BufferStoreDword;
	}
}

Opcode DsReadOpcode(const Decoder::Instruction& decoded) {
	switch (decoded.data_bits) {
		case 8u: return decoded.data_signed ? Opcode::DsReadSbyte : Opcode::DsReadUbyte;
		case 16u: return decoded.data_signed ? Opcode::DsReadSshort : Opcode::DsReadUshort;
		default: return Opcode::DsReadB32;
	}
}

Opcode FlatLoadOpcode(const Decoder::Instruction& decoded) {
	switch (decoded.data_bits) {
		case 8u: return decoded.data_signed ? Opcode::FlatLoadSbyte : Opcode::FlatLoadUbyte;
		case 16u: return decoded.data_signed ? Opcode::FlatLoadSshort : Opcode::FlatLoadUshort;
		default: return Opcode::FlatLoadDword;
	}
}

Opcode FlatStoreOpcode(uint32_t data_bits) {
	switch (data_bits) {
		case 8u: return Opcode::FlatStoreByte;
		case 16u: return Opcode::FlatStoreShort;
		default: return Opcode::FlatStoreDword;
	}
}

Opcode DsWriteOpcode(uint32_t data_bits) {
	switch (data_bits) {
		case 8u: return Opcode::DsWriteByte;
		case 16u: return Opcode::DsWriteShort;
		default: return Opcode::DsWriteB32;
	}
}

Opcode LoweredImageOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::IMAGE_GET_RESINFO: return Opcode::ImageGetResinfo;
		case Decoder::Opcode::IMAGE_GET_LOD: return Opcode::ImageGetLod;
		case Decoder::Opcode::IMAGE_LOAD:
		case Decoder::Opcode::IMAGE_LOAD_MIP: return Opcode::ImageLoad;
		case Decoder::Opcode::IMAGE_GATHER4_LZ:
		case Decoder::Opcode::IMAGE_GATHER4_C:
		case Decoder::Opcode::IMAGE_GATHER4_C_LZ:
		case Decoder::Opcode::IMAGE_GATHER4_LZ_O:
		case Decoder::Opcode::IMAGE_GATHER4_C_O:
		case Decoder::Opcode::IMAGE_GATHER4_C_LZ_O:
		case Decoder::Opcode::IMAGE_GATHER4H: return Opcode::ImageGather4;
		default: return Opcode::ImageSample;
	}
}

bool LowerScalarMemoryLoadDword(const Decoder::Instruction& decoded, BasicBlock& block, Opcode op,
                                std::string* error) {
	uint32_t destination_code = 0;
	const auto alignment = decoded.data_dwords == 1u ? 1u
	                       : decoded.data_dwords == 2u ? 2u
	                                                    : 4u;
	if (!TryGetEncodedScalarCode(decoded.dst, destination_code) ||
	    destination_code % alignment != 0u ||
	    destination_code + decoded.data_dwords > 108u) {
		SetError(error, "scalar-memory destination has an invalid aligned SGPR span");
		return false;
	}
	uint32_t base_code = 0;
	const auto base_alignment = op == Opcode::SLoadDword ? 2u : 4u;
	const auto base_width     = op == Opcode::SLoadDword ? 2u : 4u;
	if (!TryGetEncodedScalarCode(decoded.src0, base_code) ||
	    base_code % base_alignment != 0u || base_code + base_width > 108u) {
		SetError(error, "scalar-memory base has an invalid aligned SGPR span");
		return false;
	}
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = op;
	inst.src_count = 1;
	const auto kind =
	    op == Opcode::SLoadDword ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer;
	inst.memory = MemoryInfoFromDecoded(decoded, kind);
	// Raw s_load uses a two-SGPR pointer base; s_buffer_load uses a four-SGPR
	// descriptor resource index.
	inst.memory.resource = op == Opcode::SLoadDword ? RawScalarLoadBase(decoded.src0)
	                                                : ResourceIndexFromOperand(decoded.src0);
	if (!LowerRegisterOperand(decoded.dst, inst.dst, error) ||
	    !LowerSourceOperand(decoded.src1, inst.src[0], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerBufferAddressSources(const Decoder::Instruction& decoded, Instruction& inst,
                               uint32_t first_src, std::string* error) {
	uint32_t src = first_src;
	if (decoded.idxen) {
		if (!LowerSourceOperand(decoded.src0, inst.src[src++], error)) {
			return false;
		}
	}
	if (decoded.offen) {
		const auto offset_source =
		    decoded.idxen ? OffsetDecodedRegister(decoded.src0, 1) : decoded.src0;
		if (!LowerSourceOperand(offset_source, inst.src[src++], error)) {
			return false;
		}
	}
	if (!LowerSourceOperand(decoded.src2, inst.src[src++], error)) {
		return false;
	}
	inst.src_count = src;
	return true;
}

bool LowerBufferLoad(const Decoder::Instruction& decoded, BasicBlock& block, std::string* error) {
	Instruction inst;
	inst.pc     = decoded.pc;
	inst.op     = BufferLoadOpcode(decoded);
	inst.memory = MemoryInfoFromDecoded(decoded, ResourceKind::Buffer);
	if (!LowerRegisterOperand(decoded.dst, inst.dst, error) ||
	    !LowerBufferAddressSources(decoded, inst, 0, error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerBufferStore(const Decoder::Instruction& decoded, BasicBlock& block, std::string* error) {
	Instruction inst;
	inst.pc       = decoded.pc;
	inst.op       = BufferStoreOpcode(decoded.data_bits);
	inst.memory   = MemoryInfoFromDecoded(decoded, ResourceKind::Buffer);
	inst.dst.kind = OperandKind::Null;
	if (!LowerSourceOperand(decoded.dst, inst.src[0], error) ||
	    !LowerBufferAddressSources(decoded, inst, 1, error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerBufferAtomicDword(const Decoder::Instruction& decoded, BasicBlock& block, Opcode op,
                            std::string* error) {
	Instruction inst;
	inst.pc       = decoded.pc;
	inst.op       = op;
	inst.memory   = MemoryInfoFromDecoded(decoded, ResourceKind::Buffer);
	inst.dst.kind = OperandKind::Null;
	if ((decoded.glc && !LowerRegisterOperand(decoded.dst, inst.dst, error)) ||
	    !LowerSourceOperand(decoded.dst, inst.src[0], error) ||
	    !LowerBufferAddressSources(decoded, inst, 1, error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

ResourceKind DsMemoryKind(const Decoder::Instruction& decoded) {
	return decoded.gds ? ResourceKind::Gds : ResourceKind::Lds;
}

bool LowerDsRead(const Decoder::Instruction& decoded, BasicBlock& block, std::string* error) {
	const auto  ir_op = DsReadOpcode(decoded);
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = ir_op;
	inst.src_count = 1;
	inst.memory    = MemoryInfoFromDecoded(decoded, DsMemoryKind(decoded));
	if (!LowerRegisterOperand(decoded.dst, inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, inst.src[0], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerDsRead2(const Decoder::Instruction& decoded, BasicBlock& block, uint32_t dwords_per_read,
                  std::string* error) {
	Instruction inst;
	inst.pc                 = decoded.pc;
	inst.op                 = Opcode::DsRead2B32;
	inst.src_count          = 1;
	inst.memory             = MemoryInfoFromDecoded(decoded, DsMemoryKind(decoded));
	inst.memory.data_dwords = dwords_per_read * 2u;
	if (!LowerRegisterOperand(decoded.dst, inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, inst.src[0], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerDsAtomicU32(const Decoder::Instruction& decoded, BasicBlock& block, Opcode op,
                      bool return_old, std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = op;
	inst.src_count = 2;
	inst.memory    = MemoryInfoFromDecoded(decoded, DsMemoryKind(decoded));
	inst.dst.kind  = return_old ? OperandKind::Register : OperandKind::Null;
	if ((return_old && !LowerRegisterOperand(decoded.dst, inst.dst, error)) ||
	    !LowerSourceOperand(decoded.src1, inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src0, inst.src[1], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerDsSwizzleB32(const Decoder::Instruction& decoded, BasicBlock& block, std::string* error) {
	Instruction inst;
	inst.pc          = decoded.pc;
	inst.op          = Opcode::DsSwizzleB32;
	inst.src_count   = 2;
	inst.src[1].kind = OperandKind::ImmediateU32;
	inst.src[1].imm  = decoded.offset & 0xffffu;
	if (!LowerRegisterOperand(decoded.dst, inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, inst.src[0], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerDsFloatMinMaxF32(const Decoder::Instruction& decoded, BasicBlock& block, Opcode op,
                           std::string* error) {
	Instruction inst;
	inst.pc              = decoded.pc;
	inst.op              = op;
	inst.src_count       = 2;
	inst.memory          = MemoryInfoFromDecoded(decoded, DsMemoryKind(decoded));
	inst.memory.resource = 0;
	inst.dst.kind        = OperandKind::Null;
	if (!LowerSourceOperand(decoded.src1, inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src0, inst.src[1], error) ||
	    !LowerSourceOperand(decoded.src2, inst.src[2], error)) {
		return false;
	}
	inst.src_count = 3;
	block.instructions.push_back(inst);
	return true;
}

bool LowerDsWriteAddtidB32(const Decoder::Instruction& decoded, BasicBlock& block,
                           std::string* error) {
	Instruction inst;
	inst.pc              = decoded.pc;
	inst.op              = Opcode::DsWriteAddtidB32;
	inst.src_count       = 2;
	inst.memory          = MemoryInfoFromDecoded(decoded, DsMemoryKind(decoded));
	inst.memory.resource = 0;
	inst.dst.kind        = OperandKind::Null;
	const auto m0        = M0Operand();
	if (!LowerSourceOperand(decoded.src1, inst.src[0], error) ||
	    !LowerSourceOperand(m0, inst.src[1], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerDsReadAddtidB32(const Decoder::Instruction& decoded, BasicBlock& block,
                          std::string* error) {
	Instruction inst;
	inst.pc              = decoded.pc;
	inst.op              = Opcode::DsReadAddtidB32;
	inst.src_count       = 1;
	inst.memory          = MemoryInfoFromDecoded(decoded, DsMemoryKind(decoded));
	inst.memory.resource = 0;
	const auto m0        = M0Operand();
	if (!LowerRegisterOperand(decoded.dst, inst.dst, error) ||
	    !LowerSourceOperand(m0, inst.src[0], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerDsAppendConsume(const Decoder::Instruction& decoded, BasicBlock& block, Opcode op,
                          std::string* error) {
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = op;
	inst.src_count = 1;
	inst.memory    = MemoryInfoFromDecoded(decoded, DsMemoryKind(decoded));
	const auto m0  = M0Operand();
	if (!LowerRegisterOperand(decoded.dst, inst.dst, error) ||
	    !LowerSourceOperand(m0, inst.src[0], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

Opcode ImageAtomicIrOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::IMAGE_ATOMIC_ADD: return Opcode::AtomicAddU32;
		case Decoder::Opcode::IMAGE_ATOMIC_UMIN: return Opcode::AtomicUMinU32;
		case Decoder::Opcode::IMAGE_ATOMIC_UMAX: return Opcode::AtomicUMaxU32;
		case Decoder::Opcode::IMAGE_ATOMIC_AND: return Opcode::AtomicAndU32;
		case Decoder::Opcode::IMAGE_ATOMIC_OR: return Opcode::AtomicOrU32;
		case Decoder::Opcode::IMAGE_ATOMIC_XOR: return Opcode::AtomicXorU32;
		default: return Opcode::AtomicAddU32;
	}
}

bool LowerImageAtomicU32(const Decoder::Instruction& decoded, BasicBlock& block,
                         std::string* error) {
	Instruction inst;
	inst.pc             = decoded.pc;
	inst.op             = ImageAtomicIrOpcode(decoded.opcode);
	inst.src_count      = 2;
	inst.memory         = MemoryInfoFromDecoded(decoded, ResourceKind::StorageImageUint);
	inst.memory.sampler = 0;
	inst.dst.kind       = OperandKind::Null;
	if ((decoded.glc && !LowerRegisterOperand(decoded.dst, inst.dst, error)) ||
	    !LowerSourceOperand(decoded.dst, inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src0, inst.src[1], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerFlatLoad(const Decoder::Instruction& decoded, BasicBlock& block, std::string* error) {
	const auto kind  = FlatSegmentResourceKind(decoded.memory_segment);
	const auto ir_op = FlatLoadOpcode(decoded);
	const auto count = decoded.data_bits == 32u ? decoded.data_dwords : 1u;
	if (decoded.data_bits == 32u && count > 1u) {
		Instruction inst;
		inst.pc              = decoded.pc;
		inst.op              = ir_op;
		inst.src_count       = decoded.src_count;
		inst.memory          = MemoryInfoFromDecoded(decoded, kind);
		inst.memory.resource = 0;
		if (!LowerRegisterOperand(decoded.dst, inst.dst, error)) {
			return false;
		}
		for (uint32_t i = 0; i < decoded.src_count && i < 3u; i++) {
			if (!LowerSourceOperand(DecodedSourceAt(decoded, i), inst.src[i], error)) {
				return false;
			}
		}
		block.instructions.push_back(inst);
		return true;
	}

	for (uint32_t dword = 0; dword < count; dword++) {
		Instruction inst;
		inst.pc              = decoded.pc;
		inst.op              = ir_op;
		inst.src_count       = decoded.src_count;
		inst.memory          = OffsetMemoryInfo(decoded, kind, dword);
		inst.memory.resource = 0;
		if (!LowerRegisterOperand(OffsetDecodedRegister(decoded.dst, dword), inst.dst, error)) {
			return false;
		}
		for (uint32_t i = 0; i < decoded.src_count && i < 3u; i++) {
			if (!LowerSourceOperand(DecodedSourceAt(decoded, i), inst.src[i], error)) {
				return false;
			}
		}
		block.instructions.push_back(inst);
	}
	return true;
}

bool LowerFlatStore(const Decoder::Instruction& decoded, BasicBlock& block, std::string* error) {
	const auto kind  = FlatSegmentResourceKind(decoded.memory_segment);
	const auto ir_op = FlatStoreOpcode(decoded.data_bits);
	const auto count = decoded.data_bits == 32u ? decoded.data_dwords : 1u;
	for (uint32_t dword = 0; dword < count; dword++) {
		Instruction inst;
		inst.pc              = decoded.pc;
		inst.op              = ir_op;
		inst.src_count       = decoded.src_count + 1u;
		inst.memory          = OffsetMemoryInfo(decoded, kind, dword);
		inst.memory.resource = 0;
		inst.dst.kind        = OperandKind::Null;
		if (!LowerSourceOperand(OffsetDecodedRegister(decoded.dst, dword), inst.src[0], error)) {
			return false;
		}
		for (uint32_t i = 0; i < decoded.src_count && i + 1u < 3u; i++) {
			if (!LowerSourceOperand(DecodedSourceAt(decoded, i), inst.src[i + 1u], error)) {
				return false;
			}
		}
		block.instructions.push_back(inst);
	}
	return true;
}

bool LowerDsWrite(const Decoder::Instruction& decoded, BasicBlock& block, std::string* error) {
	const auto  ir_op = DsWriteOpcode(decoded.data_bits);
	Instruction inst;
	inst.pc        = decoded.pc;
	inst.op        = ir_op;
	inst.src_count = 2;
	inst.memory    = MemoryInfoFromDecoded(decoded, DsMemoryKind(decoded));
	inst.dst.kind  = OperandKind::Null;
	if (!LowerSourceOperand(decoded.src1, inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src0, inst.src[1], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerDsWrite2(const Decoder::Instruction& decoded, BasicBlock& block,
                   uint32_t dwords_per_write, std::string* error) {
	Instruction inst;
	inst.pc                 = decoded.pc;
	inst.op                 = Opcode::DsWrite2B32;
	inst.src_count          = 3;
	inst.memory             = MemoryInfoFromDecoded(decoded, DsMemoryKind(decoded));
	inst.memory.data_dwords = dwords_per_write * 2u;
	inst.dst.kind           = OperandKind::Null;
	if (!LowerSourceOperand(decoded.src1, inst.src[0], error) ||
	    !LowerSourceOperand(decoded.src0, inst.src[1], error) ||
	    !LowerSourceOperand(decoded.src2, inst.src[2], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

bool LowerImageOperation(const Decoder::Instruction& decoded, BasicBlock& block,
                         std::string* error) {
	Instruction inst;
	inst.pc = decoded.pc;
	if (decoded.opcode == Decoder::Opcode::IMAGE_STORE ||
	    decoded.opcode == Decoder::Opcode::IMAGE_STORE_MIP) {
		inst.op             = Opcode::ImageStore;
		inst.src_count      = 2;
		inst.memory         = MemoryInfoFromDecoded(decoded, ResourceKind::StorageImage);
		inst.memory.sampler = 0;
		inst.dst.kind       = OperandKind::Null;
		if (!LowerSourceOperand(decoded.dst, inst.src[0], error) ||
		    !LowerSourceOperand(decoded.src0, inst.src[1], error)) {
			return false;
		}
		block.instructions.push_back(inst);
		return true;
	}

	inst.op        = LoweredImageOpcode(decoded.opcode);
	inst.src_count = 1;
	inst.memory    = MemoryInfoFromDecoded(decoded, ResourceKind::Image);
	if (!LowerRegisterOperand(decoded.dst, inst.dst, error) ||
	    !LowerSourceOperand(decoded.src0, inst.src[0], error)) {
		return false;
	}
	block.instructions.push_back(inst);
	return true;
}

} // namespace

bool LowerMemoryInstruction(const Decoder::Instruction& decoded, BasicBlock& block,
                            std::string* error) {
	switch (decoded.opcode) {
		case Decoder::Opcode::S_LOAD_DWORD:
		case Decoder::Opcode::S_LOAD_DWORDX2:
		case Decoder::Opcode::S_LOAD_DWORDX4:
		case Decoder::Opcode::S_LOAD_DWORDX8:
		case Decoder::Opcode::S_LOAD_DWORDX16:
			return LowerScalarMemoryLoadDword(decoded, block, Opcode::SLoadDword, error);
		case Decoder::Opcode::S_BUFFER_LOAD_DWORD:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX2:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX4:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX8:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX16:
			return LowerScalarMemoryLoadDword(decoded, block, Opcode::SBufferLoadDword, error);
		case Decoder::Opcode::BUFFER_LOAD_UBYTE:
		case Decoder::Opcode::BUFFER_LOAD_SBYTE:
		case Decoder::Opcode::BUFFER_LOAD_USHORT:
		case Decoder::Opcode::BUFFER_LOAD_SSHORT:
		case Decoder::Opcode::BUFFER_LOAD_DWORD:
		case Decoder::Opcode::BUFFER_LOAD_DWORDX2:
		case Decoder::Opcode::BUFFER_LOAD_DWORDX3:
		case Decoder::Opcode::BUFFER_LOAD_DWORDX4:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_X:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XY:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZ:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZW:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_X:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XY:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XYZ:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XYZW:
			return LowerBufferLoad(decoded, block, error);
		case Decoder::Opcode::BUFFER_STORE_DWORD:
		case Decoder::Opcode::BUFFER_STORE_DWORDX2:
		case Decoder::Opcode::BUFFER_STORE_DWORDX3:
		case Decoder::Opcode::BUFFER_STORE_DWORDX4:
		case Decoder::Opcode::BUFFER_STORE_BYTE:
		case Decoder::Opcode::BUFFER_STORE_SHORT:
		case Decoder::Opcode::BUFFER_STORE_FORMAT_X:
		case Decoder::Opcode::BUFFER_STORE_FORMAT_XY:
		case Decoder::Opcode::BUFFER_STORE_FORMAT_XYZ:
		case Decoder::Opcode::BUFFER_STORE_FORMAT_XYZW:
		case Decoder::Opcode::TBUFFER_STORE_FORMAT_X:
		case Decoder::Opcode::TBUFFER_STORE_FORMAT_XY:
		case Decoder::Opcode::TBUFFER_STORE_FORMAT_XYZ:
		case Decoder::Opcode::TBUFFER_STORE_FORMAT_XYZW:
			return LowerBufferStore(decoded, block, error);
		case Decoder::Opcode::BUFFER_ATOMIC_SWAP:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicSwapU32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_ADD:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicAddU32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_SUB:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicSubU32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_SMIN:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicSMinI32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_UMIN:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicUMinU32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_SMAX:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicSMaxI32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_UMAX:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicUMaxU32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_AND:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicAndU32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_OR:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicOrU32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_XOR:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicXorU32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_FMIN:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicFMinF32, error);
		case Decoder::Opcode::BUFFER_ATOMIC_FMAX:
			return LowerBufferAtomicDword(decoded, block, Opcode::AtomicFMaxF32, error);
		case Decoder::Opcode::FLAT_LOAD_UBYTE:
		case Decoder::Opcode::FLAT_LOAD_SBYTE:
		case Decoder::Opcode::FLAT_LOAD_USHORT:
		case Decoder::Opcode::FLAT_LOAD_SSHORT:
		case Decoder::Opcode::FLAT_LOAD_DWORD:
		case Decoder::Opcode::FLAT_LOAD_DWORDX2:
		case Decoder::Opcode::FLAT_LOAD_DWORDX3:
		case Decoder::Opcode::FLAT_LOAD_DWORDX4: return LowerFlatLoad(decoded, block, error);
		case Decoder::Opcode::FLAT_STORE_BYTE:
		case Decoder::Opcode::FLAT_STORE_SHORT:
		case Decoder::Opcode::FLAT_STORE_DWORD:
		case Decoder::Opcode::FLAT_STORE_DWORDX2:
		case Decoder::Opcode::FLAT_STORE_DWORDX3:
		case Decoder::Opcode::FLAT_STORE_DWORDX4: return LowerFlatStore(decoded, block, error);
		case Decoder::Opcode::DS_ADD_U32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicAddU32, false, error);
		case Decoder::Opcode::DS_ADD_RTN_U32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicAddU32, true, error);
		case Decoder::Opcode::DS_SUB_U32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicSubU32, false, error);
		case Decoder::Opcode::DS_SUB_RTN_U32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicSubU32, true, error);
		case Decoder::Opcode::DS_MIN_I32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicSMinI32, false, error);
		case Decoder::Opcode::DS_MIN_RTN_I32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicSMinI32, true, error);
		case Decoder::Opcode::DS_MAX_I32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicSMaxI32, false, error);
		case Decoder::Opcode::DS_MAX_RTN_I32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicSMaxI32, true, error);
		case Decoder::Opcode::DS_MIN_U32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicUMinU32, false, error);
		case Decoder::Opcode::DS_MIN_RTN_U32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicUMinU32, true, error);
		case Decoder::Opcode::DS_MAX_U32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicUMaxU32, false, error);
		case Decoder::Opcode::DS_MAX_RTN_U32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicUMaxU32, true, error);
		case Decoder::Opcode::DS_AND_B32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicAndU32, false, error);
		case Decoder::Opcode::DS_AND_RTN_B32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicAndU32, true, error);
		case Decoder::Opcode::DS_OR_B32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicOrU32, false, error);
		case Decoder::Opcode::DS_OR_RTN_B32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicOrU32, true, error);
		case Decoder::Opcode::DS_XOR_B32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicXorU32, false, error);
		case Decoder::Opcode::DS_XOR_RTN_B32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicXorU32, true, error);
		case Decoder::Opcode::DS_WRXCHG_RTN_B32:
			return LowerDsAtomicU32(decoded, block, Opcode::AtomicSwapU32, true, error);
		case Decoder::Opcode::DS_MIN_F32:
			return LowerDsFloatMinMaxF32(decoded, block, Opcode::DsMinF32, error);
		case Decoder::Opcode::DS_MAX_F32:
			return LowerDsFloatMinMaxF32(decoded, block, Opcode::DsMaxF32, error);
		case Decoder::Opcode::DS_SWIZZLE_B32: return LowerDsSwizzleB32(decoded, block, error);
		case Decoder::Opcode::DS_CONSUME:
			return LowerDsAppendConsume(decoded, block, Opcode::DsConsume, error);
		case Decoder::Opcode::DS_APPEND:
			return LowerDsAppendConsume(decoded, block, Opcode::DsAppend, error);
		case Decoder::Opcode::DS_WRITE_ADDTID_B32:
			return LowerDsWriteAddtidB32(decoded, block, error);
		case Decoder::Opcode::DS_READ_ADDTID_B32:
			return LowerDsReadAddtidB32(decoded, block, error);
		case Decoder::Opcode::DS_READ2_B32:
		case Decoder::Opcode::DS_READ2ST64_B32: return LowerDsRead2(decoded, block, 1, error);
		case Decoder::Opcode::DS_READ2_B64:
		case Decoder::Opcode::DS_READ2ST64_B64: return LowerDsRead2(decoded, block, 2, error);
		case Decoder::Opcode::DS_READ_I8:
		case Decoder::Opcode::DS_READ_U8:
		case Decoder::Opcode::DS_READ_I16:
		case Decoder::Opcode::DS_READ_U16:
		case Decoder::Opcode::DS_READ_B32:
		case Decoder::Opcode::DS_READ_B64:
		case Decoder::Opcode::DS_READ_B96:
		case Decoder::Opcode::DS_READ_B128: return LowerDsRead(decoded, block, error);
		case Decoder::Opcode::DS_WRITE2_B32:
		case Decoder::Opcode::DS_WRITE2ST64_B32: return LowerDsWrite2(decoded, block, 1, error);
		case Decoder::Opcode::DS_WRITE2_B64:
		case Decoder::Opcode::DS_WRITE2ST64_B64: return LowerDsWrite2(decoded, block, 2, error);
		case Decoder::Opcode::DS_WRITE_B8:
		case Decoder::Opcode::DS_WRITE_B16:
		case Decoder::Opcode::DS_WRITE_B32:
		case Decoder::Opcode::DS_WRITE_B64:
		case Decoder::Opcode::DS_WRITE_B96:
		case Decoder::Opcode::DS_WRITE_B128: return LowerDsWrite(decoded, block, error);
		case Decoder::Opcode::IMAGE_GET_RESINFO:
		case Decoder::Opcode::IMAGE_GET_LOD:
		case Decoder::Opcode::IMAGE_LOAD:
		case Decoder::Opcode::IMAGE_LOAD_MIP:
		case Decoder::Opcode::IMAGE_STORE:
		case Decoder::Opcode::IMAGE_STORE_MIP:
		case Decoder::Opcode::IMAGE_GATHER4_LZ:
		case Decoder::Opcode::IMAGE_GATHER4_C:
		case Decoder::Opcode::IMAGE_GATHER4_C_LZ:
		case Decoder::Opcode::IMAGE_GATHER4_LZ_O:
		case Decoder::Opcode::IMAGE_GATHER4_C_O:
		case Decoder::Opcode::IMAGE_GATHER4_C_LZ_O:
		case Decoder::Opcode::IMAGE_GATHER4H:
		case Decoder::Opcode::IMAGE_SAMPLE: return LowerImageOperation(decoded, block, error);
		case Decoder::Opcode::IMAGE_ATOMIC_ADD:
		case Decoder::Opcode::IMAGE_ATOMIC_UMIN:
		case Decoder::Opcode::IMAGE_ATOMIC_UMAX:
		case Decoder::Opcode::IMAGE_ATOMIC_AND:
		case Decoder::Opcode::IMAGE_ATOMIC_OR:
		case Decoder::Opcode::IMAGE_ATOMIC_XOR: return LowerImageAtomicU32(decoded, block, error);
		default:
			if (error != nullptr) {
				*error = fmt::format("memory opcode has no specialized IR lowering: {}",
				                     magic_enum::enum_name(decoded.opcode));
			}
			return false;
	}
}

bool IsMemoryOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::S_LOAD_DWORD:
		case Decoder::Opcode::S_LOAD_DWORDX2:
		case Decoder::Opcode::S_LOAD_DWORDX4:
		case Decoder::Opcode::S_LOAD_DWORDX8:
		case Decoder::Opcode::S_LOAD_DWORDX16:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORD:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX2:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX4:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX8:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX16:
		case Decoder::Opcode::BUFFER_LOAD_UBYTE:
		case Decoder::Opcode::BUFFER_LOAD_SBYTE:
		case Decoder::Opcode::BUFFER_LOAD_USHORT:
		case Decoder::Opcode::BUFFER_LOAD_SSHORT:
		case Decoder::Opcode::BUFFER_LOAD_DWORD:
		case Decoder::Opcode::BUFFER_LOAD_DWORDX2:
		case Decoder::Opcode::BUFFER_LOAD_DWORDX3:
		case Decoder::Opcode::BUFFER_LOAD_DWORDX4:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_X:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XY:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZ:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZW:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_X:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XY:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XYZ:
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XYZW:
		case Decoder::Opcode::BUFFER_STORE_DWORD:
		case Decoder::Opcode::BUFFER_STORE_DWORDX2:
		case Decoder::Opcode::BUFFER_STORE_DWORDX3:
		case Decoder::Opcode::BUFFER_STORE_DWORDX4:
		case Decoder::Opcode::BUFFER_STORE_BYTE:
		case Decoder::Opcode::BUFFER_STORE_SHORT:
		case Decoder::Opcode::BUFFER_STORE_FORMAT_X:
		case Decoder::Opcode::BUFFER_STORE_FORMAT_XY:
		case Decoder::Opcode::BUFFER_STORE_FORMAT_XYZ:
		case Decoder::Opcode::BUFFER_STORE_FORMAT_XYZW:
		case Decoder::Opcode::TBUFFER_STORE_FORMAT_X:
		case Decoder::Opcode::TBUFFER_STORE_FORMAT_XY:
		case Decoder::Opcode::TBUFFER_STORE_FORMAT_XYZ:
		case Decoder::Opcode::TBUFFER_STORE_FORMAT_XYZW:
		case Decoder::Opcode::BUFFER_ATOMIC_SWAP:
		case Decoder::Opcode::BUFFER_ATOMIC_ADD:
		case Decoder::Opcode::BUFFER_ATOMIC_SUB:
		case Decoder::Opcode::BUFFER_ATOMIC_SMIN:
		case Decoder::Opcode::BUFFER_ATOMIC_UMIN:
		case Decoder::Opcode::BUFFER_ATOMIC_SMAX:
		case Decoder::Opcode::BUFFER_ATOMIC_UMAX:
		case Decoder::Opcode::BUFFER_ATOMIC_AND:
		case Decoder::Opcode::BUFFER_ATOMIC_OR:
		case Decoder::Opcode::BUFFER_ATOMIC_XOR:
		case Decoder::Opcode::BUFFER_ATOMIC_FMIN:
		case Decoder::Opcode::BUFFER_ATOMIC_FMAX:
		case Decoder::Opcode::FLAT_LOAD_UBYTE:
		case Decoder::Opcode::FLAT_LOAD_SBYTE:
		case Decoder::Opcode::FLAT_LOAD_USHORT:
		case Decoder::Opcode::FLAT_LOAD_SSHORT:
		case Decoder::Opcode::FLAT_LOAD_DWORD:
		case Decoder::Opcode::FLAT_LOAD_DWORDX2:
		case Decoder::Opcode::FLAT_LOAD_DWORDX3:
		case Decoder::Opcode::FLAT_LOAD_DWORDX4:
		case Decoder::Opcode::FLAT_STORE_BYTE:
		case Decoder::Opcode::FLAT_STORE_SHORT:
		case Decoder::Opcode::FLAT_STORE_DWORD:
		case Decoder::Opcode::FLAT_STORE_DWORDX2:
		case Decoder::Opcode::FLAT_STORE_DWORDX3:
		case Decoder::Opcode::FLAT_STORE_DWORDX4:
		case Decoder::Opcode::DS_ADD_U32:
		case Decoder::Opcode::DS_ADD_RTN_U32:
		case Decoder::Opcode::DS_SUB_U32:
		case Decoder::Opcode::DS_SUB_RTN_U32:
		case Decoder::Opcode::DS_MIN_I32:
		case Decoder::Opcode::DS_MIN_RTN_I32:
		case Decoder::Opcode::DS_MAX_I32:
		case Decoder::Opcode::DS_MAX_RTN_I32:
		case Decoder::Opcode::DS_MIN_U32:
		case Decoder::Opcode::DS_MIN_RTN_U32:
		case Decoder::Opcode::DS_MAX_U32:
		case Decoder::Opcode::DS_MAX_RTN_U32:
		case Decoder::Opcode::DS_AND_B32:
		case Decoder::Opcode::DS_AND_RTN_B32:
		case Decoder::Opcode::DS_OR_B32:
		case Decoder::Opcode::DS_OR_RTN_B32:
		case Decoder::Opcode::DS_XOR_B32:
		case Decoder::Opcode::DS_XOR_RTN_B32:
		case Decoder::Opcode::DS_WRXCHG_RTN_B32:
		case Decoder::Opcode::DS_MIN_F32:
		case Decoder::Opcode::DS_MAX_F32:
		case Decoder::Opcode::DS_SWIZZLE_B32:
		case Decoder::Opcode::DS_CONSUME:
		case Decoder::Opcode::DS_APPEND:
		case Decoder::Opcode::DS_READ_I8:
		case Decoder::Opcode::DS_READ_U8:
		case Decoder::Opcode::DS_READ_I16:
		case Decoder::Opcode::DS_READ_U16:
		case Decoder::Opcode::DS_READ2_B32:
		case Decoder::Opcode::DS_READ2ST64_B32:
		case Decoder::Opcode::DS_READ_B32:
		case Decoder::Opcode::DS_READ_B64:
		case Decoder::Opcode::DS_READ2_B64:
		case Decoder::Opcode::DS_READ2ST64_B64:
		case Decoder::Opcode::DS_READ_B96:
		case Decoder::Opcode::DS_READ_B128:
		case Decoder::Opcode::DS_WRITE_B8:
		case Decoder::Opcode::DS_WRITE_B16:
		case Decoder::Opcode::DS_WRITE2_B32:
		case Decoder::Opcode::DS_WRITE2ST64_B32:
		case Decoder::Opcode::DS_WRITE2_B64:
		case Decoder::Opcode::DS_WRITE2ST64_B64:
		case Decoder::Opcode::DS_WRITE_B32:
		case Decoder::Opcode::DS_WRITE_B64:
		case Decoder::Opcode::DS_WRITE_B96:
		case Decoder::Opcode::DS_WRITE_B128:
		case Decoder::Opcode::DS_WRITE_ADDTID_B32:
		case Decoder::Opcode::DS_READ_ADDTID_B32:
		case Decoder::Opcode::IMAGE_GET_RESINFO:
		case Decoder::Opcode::IMAGE_GET_LOD:
		case Decoder::Opcode::IMAGE_LOAD:
		case Decoder::Opcode::IMAGE_LOAD_MIP:
		case Decoder::Opcode::IMAGE_STORE:
		case Decoder::Opcode::IMAGE_STORE_MIP:
		case Decoder::Opcode::IMAGE_ATOMIC_ADD:
		case Decoder::Opcode::IMAGE_ATOMIC_UMIN:
		case Decoder::Opcode::IMAGE_ATOMIC_UMAX:
		case Decoder::Opcode::IMAGE_ATOMIC_AND:
		case Decoder::Opcode::IMAGE_ATOMIC_OR:
		case Decoder::Opcode::IMAGE_ATOMIC_XOR:
		case Decoder::Opcode::IMAGE_GATHER4_LZ:
		case Decoder::Opcode::IMAGE_GATHER4_C:
		case Decoder::Opcode::IMAGE_GATHER4_C_LZ:
		case Decoder::Opcode::IMAGE_GATHER4_LZ_O:
		case Decoder::Opcode::IMAGE_GATHER4_C_O:
		case Decoder::Opcode::IMAGE_GATHER4_C_LZ_O:
		case Decoder::Opcode::IMAGE_GATHER4H:
		case Decoder::Opcode::IMAGE_SAMPLE: return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
