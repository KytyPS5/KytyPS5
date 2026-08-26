#include "common/magicEnum.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

namespace {

using IR::ResourceKind;

const Decoder::Operand& DecodedSourceAt(const Decoder::Instruction& decoded, uint32_t index) {
	switch (index) {
		case 0: return decoded.src0;
		case 1: return decoded.src1;
		case 2: return decoded.src2;
		default: return decoded.src3;
	}
}

Decoder::Operand OffsetDecodedRegister(const Decoder::Operand& operand, uint32_t index) {
	if (index == 0) {
		return operand;
	}
	auto result               = operand;
	result.sdwa_sel           = 6;
	result.sdwa_dst_unused    = 2;
	result.omod               = 0;
	result.sdwa_sext          = false;
	result.op_sel             = false;
	result.op_sel_hi          = false;
	result.negate             = false;
	result.negate_hi          = false;
	result.absolute           = false;
	result.dpp_ctrl           = 0;
	result.dpp_row_mask       = 0xf;
	result.dpp_bank_mask      = 0xf;
	result.dpp_fetch_inactive = false;
	result.dpp_bound_ctrl     = false;
	result.dpp                = false;
	if (result.kind == Decoder::OperandKind::Vgpr || result.kind == Decoder::OperandKind::Sgpr) {
		result.reg += index;
	}
	return result;
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

uint32_t RawScalarLoadBase(const Decoder::Operand& operand) {
	if (operand.kind == Decoder::OperandKind::Sgpr) {
		return operand.reg;
	}
	return operand.kind == Decoder::OperandKind::VccLo ? 106u : 0u;
}

bool TryGetEncodedScalarCode(const Decoder::Operand& operand, uint32_t& code) {
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: code = operand.reg; return true;
		case Decoder::OperandKind::VccLo: code = 106u; return true;
		case Decoder::OperandKind::VccHi: code = 107u; return true;
		case Decoder::OperandKind::ExecLo: code = 126u; return true;
		case Decoder::OperandKind::ExecHi: code = 127u; return true;
		default: return false;
	}
}

ResourceKind FlatSegmentResourceKind(uint32_t segment) {
	switch (segment) {
		case 1u: return ResourceKind::Scratch;
		case 2u: return ResourceKind::Global;
		default: return ResourceKind::Flat;
	}
}

ResourceKind MemoryKind(const Decoder::Instruction& decoded) {
	switch (decoded.family) {
		case Decoder::Family::SMEM:
			return decoded.opcode == Decoder::Opcode::S_LOAD_DWORD ||
			               decoded.opcode == Decoder::Opcode::S_LOAD_DWORDX2 ||
			               decoded.opcode == Decoder::Opcode::S_LOAD_DWORDX4 ||
			               decoded.opcode == Decoder::Opcode::S_LOAD_DWORDX8 ||
			               decoded.opcode == Decoder::Opcode::S_LOAD_DWORDX16
			           ? ResourceKind::ScalarAddress
			           : ResourceKind::ScalarBuffer;
		case Decoder::Family::MUBUF:
		case Decoder::Family::MTBUF: return ResourceKind::Buffer;
		case Decoder::Family::FLAT: return FlatSegmentResourceKind(decoded.memory_segment);
		case Decoder::Family::DS: return decoded.gds ? ResourceKind::Gds : ResourceKind::Lds;
		case Decoder::Family::MIMG:
			switch (decoded.opcode) {
				case Decoder::Opcode::IMAGE_STORE:
				case Decoder::Opcode::IMAGE_STORE_MIP: return ResourceKind::StorageImage;
				case Decoder::Opcode::IMAGE_ATOMIC_ADD:
				case Decoder::Opcode::IMAGE_ATOMIC_UMIN:
				case Decoder::Opcode::IMAGE_ATOMIC_UMAX:
				case Decoder::Opcode::IMAGE_ATOMIC_AND:
				case Decoder::Opcode::IMAGE_ATOMIC_OR:
				case Decoder::Opcode::IMAGE_ATOMIC_XOR: return ResourceKind::StorageImageUint;
				default: return ResourceKind::Image;
			}
		default: return ResourceKind::None;
	}
}

IR::MemoryInfo MemoryInfoFromDecoded(const Decoder::Instruction& decoded) {
	IR::MemoryInfo memory;
	memory.kind             = MemoryKind(decoded);
	memory.offset           = decoded.offset;
	memory.secondary_offset = decoded.secondary_offset;
	memory.dmask            = decoded.dmask;
	memory.data_dwords      = decoded.data_dwords;
	memory.data_bits        = decoded.data_bits;
	memory.component_count =
	    decoded.data_components != 0u ? decoded.data_components : decoded.data_dwords;
	memory.data_format              = decoded.data_format;
	memory.number_format            = decoded.number_format;
	memory.image_sample_flags       = decoded.image_sample_flags;
	memory.image_dimension          = decoded.image_dimension;
	memory.image_address_components = decoded.image_address_components;
	memory.image_nsa_dwords         = decoded.image_nsa_dwords;
	for (uint32_t index = 0; index < Decoder::MaxImageNsaAddressComponents; index++) {
		memory.image_nsa_addr[index] = decoded.image_nsa_addr[index];
	}
	memory.memory_segment = decoded.memory_segment;
	memory.address_is_full =
	    memory.kind == ResourceKind::Flat ||
	    (memory.kind == ResourceKind::Global && decoded.src1.kind == Decoder::OperandKind::Vgpr);
	memory.data_signed   = decoded.data_signed;
	memory.typed         = decoded.typed;
	memory.formatted     = decoded.formatted;
	memory.image_has_mip = decoded.opcode == Decoder::Opcode::IMAGE_LOAD_MIP ||
	                       decoded.opcode == Decoder::Opcode::IMAGE_STORE_MIP;
	memory.image_r128    = decoded.image_r128;
	memory.glc           = decoded.glc;
	memory.slc           = decoded.slc;
	memory.idxen         = decoded.idxen;
	memory.offen         = decoded.offen;
	memory.resource      = ResourceIndexFromOperand(decoded.src1);
	memory.sampler       = ResourceIndexFromOperand(decoded.src2);
	if (memory.kind == ResourceKind::ScalarAddress) {
		memory.resource = RawScalarLoadBase(decoded.src0);
	} else if (memory.kind == ResourceKind::ScalarBuffer) {
		memory.resource = ResourceIndexFromOperand(decoded.src0);
	} else if (memory.kind == ResourceKind::Lds || memory.kind == ResourceKind::Gds ||
	           memory.kind == ResourceKind::Scratch) {
		memory.resource = 0;
		memory.sampler  = 0;
	}
	if (decoded.opcode == Decoder::Opcode::DS_READ2_B64 ||
	    decoded.opcode == Decoder::Opcode::DS_READ2ST64_B64 ||
	    decoded.opcode == Decoder::Opcode::DS_WRITE2_B64 ||
	    decoded.opcode == Decoder::Opcode::DS_WRITE2ST64_B64) {
		memory.data_dwords = 4u;
	} else if (decoded.opcode == Decoder::Opcode::DS_READ2_B32 ||
	           decoded.opcode == Decoder::Opcode::DS_READ2ST64_B32 ||
	           decoded.opcode == Decoder::Opcode::DS_WRITE2_B32 ||
	           decoded.opcode == Decoder::Opcode::DS_WRITE2ST64_B32) {
		memory.data_dwords = 2u;
	}
	if (memory.kind == ResourceKind::StorageImage ||
	    memory.kind == ResourceKind::StorageImageUint) {
		memory.sampler = 0;
	}
	return memory;
}

bool IsScalarAddressLoad(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::S_LOAD_DWORD:
		case Decoder::Opcode::S_LOAD_DWORDX2:
		case Decoder::Opcode::S_LOAD_DWORDX4:
		case Decoder::Opcode::S_LOAD_DWORDX8:
		case Decoder::Opcode::S_LOAD_DWORDX16: return true;
		default: return false;
	}
}

bool IsScalarBufferLoad(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::S_BUFFER_LOAD_DWORD:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX2:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX4:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX8:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX16: return true;
		default: return false;
	}
}

bool IsDsReturnAtomic(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::DS_ADD_RTN_U32:
		case Decoder::Opcode::DS_SUB_RTN_U32:
		case Decoder::Opcode::DS_MIN_RTN_I32:
		case Decoder::Opcode::DS_MAX_RTN_I32:
		case Decoder::Opcode::DS_MIN_RTN_U32:
		case Decoder::Opcode::DS_MAX_RTN_U32:
		case Decoder::Opcode::DS_AND_RTN_B32:
		case Decoder::Opcode::DS_OR_RTN_B32:
		case Decoder::Opcode::DS_XOR_RTN_B32:
		case Decoder::Opcode::DS_WRXCHG_RTN_B32: return true;
		default: return false;
	}
}

bool MemoryOpcodeMatchesFamily(Decoder::Opcode opcode, Decoder::Family family) {
	switch (family) {
		case Decoder::Family::SMEM:
			return IsScalarAddressLoad(opcode) || IsScalarBufferLoad(opcode);
		case Decoder::Family::MUBUF:
			return (opcode >= Decoder::Opcode::BUFFER_LOAD_FORMAT_X &&
			        opcode <= Decoder::Opcode::BUFFER_STORE_DWORDX4) ||
			       (opcode >= Decoder::Opcode::BUFFER_ATOMIC_SWAP &&
			        opcode <= Decoder::Opcode::BUFFER_ATOMIC_FMAX);
		case Decoder::Family::MTBUF:
			return opcode >= Decoder::Opcode::TBUFFER_LOAD_FORMAT_X &&
			       opcode <= Decoder::Opcode::TBUFFER_STORE_FORMAT_XYZW;
		case Decoder::Family::FLAT:
			return opcode >= Decoder::Opcode::FLAT_LOAD_UBYTE &&
			       opcode <= Decoder::Opcode::FLAT_STORE_DWORDX4;
		case Decoder::Family::DS:
			return opcode >= Decoder::Opcode::DS_ADD_U32 &&
			       opcode <= Decoder::Opcode::DS_READ_ADDTID_B32;
		case Decoder::Family::MIMG:
			return opcode >= Decoder::Opcode::IMAGE_GET_RESINFO &&
			       opcode <= Decoder::Opcode::IMAGE_GATHER4H;
		default: return false;
	}
}

bool UsesDestinationAsMemorySource(Decoder::Opcode opcode) {
	return (opcode >= Decoder::Opcode::BUFFER_STORE_FORMAT_X &&
	        opcode <= Decoder::Opcode::BUFFER_STORE_FORMAT_XYZW) ||
	       (opcode >= Decoder::Opcode::BUFFER_STORE_BYTE &&
	        opcode <= Decoder::Opcode::BUFFER_STORE_DWORDX4) ||
	       (opcode >= Decoder::Opcode::TBUFFER_STORE_FORMAT_X &&
	        opcode <= Decoder::Opcode::TBUFFER_STORE_FORMAT_XYZW) ||
	       (opcode >= Decoder::Opcode::BUFFER_ATOMIC_SWAP &&
	        opcode <= Decoder::Opcode::BUFFER_ATOMIC_FMAX) ||
	       (opcode >= Decoder::Opcode::FLAT_STORE_BYTE &&
	        opcode <= Decoder::Opcode::FLAT_STORE_DWORDX4) ||
	       opcode == Decoder::Opcode::IMAGE_STORE ||
	       opcode == Decoder::Opcode::IMAGE_STORE_MIP ||
	       (opcode >= Decoder::Opcode::IMAGE_ATOMIC_ADD &&
	        opcode <= Decoder::Opcode::IMAGE_ATOMIC_XOR);
}

bool IsRegisterOperand(const Decoder::Operand& operand) {
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr:
		case Decoder::OperandKind::Vgpr:
		case Decoder::OperandKind::VccLo:
		case Decoder::OperandKind::VccHi:
		case Decoder::OperandKind::VccZ:
		case Decoder::OperandKind::ExecLo:
		case Decoder::OperandKind::ExecHi:
		case Decoder::OperandKind::ExecZ:
		case Decoder::OperandKind::Scc:
		case Decoder::OperandKind::M0: return true;
		default: return false;
	}
}

bool RequiresRegisterDestination(const Decoder::Instruction& inst) {
	if (IsScalarAddressLoad(inst.opcode) || IsScalarBufferLoad(inst.opcode)) {
		return true;
	}
	if (inst.family == Decoder::Family::MUBUF || inst.family == Decoder::Family::MTBUF) {
		const bool load   = (inst.opcode >= Decoder::Opcode::BUFFER_LOAD_FORMAT_X &&
		                     inst.opcode <= Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZW) ||
		                    (inst.opcode >= Decoder::Opcode::BUFFER_LOAD_UBYTE &&
		                     inst.opcode <= Decoder::Opcode::BUFFER_LOAD_DWORDX4) ||
		                    (inst.opcode >= Decoder::Opcode::TBUFFER_LOAD_FORMAT_X &&
		                     inst.opcode <= Decoder::Opcode::TBUFFER_LOAD_FORMAT_XYZW);
		const bool atomic = inst.opcode >= Decoder::Opcode::BUFFER_ATOMIC_SWAP &&
		                    inst.opcode <= Decoder::Opcode::BUFFER_ATOMIC_FMAX;
		return load || (atomic && inst.glc);
	}
	if (inst.family == Decoder::Family::FLAT) {
		return inst.opcode >= Decoder::Opcode::FLAT_LOAD_UBYTE &&
		       inst.opcode <= Decoder::Opcode::FLAT_LOAD_DWORDX4;
	}
	if (inst.family == Decoder::Family::MIMG) {
		const bool atomic = inst.opcode >= Decoder::Opcode::IMAGE_ATOMIC_ADD &&
		                    inst.opcode <= Decoder::Opcode::IMAGE_ATOMIC_XOR;
		const bool load   = inst.opcode == Decoder::Opcode::IMAGE_GET_RESINFO ||
		                    inst.opcode == Decoder::Opcode::IMAGE_GET_LOD ||
		                    inst.opcode == Decoder::Opcode::IMAGE_LOAD ||
		                    inst.opcode == Decoder::Opcode::IMAGE_LOAD_MIP ||
		                    inst.opcode == Decoder::Opcode::IMAGE_SAMPLE ||
		                    (inst.opcode >= Decoder::Opcode::IMAGE_GATHER4_LZ &&
		                     inst.opcode <= Decoder::Opcode::IMAGE_GATHER4H);
		return load || (atomic && inst.glc);
	}
	if (inst.family == Decoder::Family::DS) {
		switch (inst.opcode) {
			case Decoder::Opcode::DS_READ2_B32:
			case Decoder::Opcode::DS_READ2ST64_B32:
			case Decoder::Opcode::DS_READ2_B64:
			case Decoder::Opcode::DS_READ2ST64_B64:
			case Decoder::Opcode::DS_READ_I8:
			case Decoder::Opcode::DS_READ_U8:
			case Decoder::Opcode::DS_READ_I16:
			case Decoder::Opcode::DS_READ_U16:
			case Decoder::Opcode::DS_READ_U16_D16:
			case Decoder::Opcode::DS_READ_B32:
			case Decoder::Opcode::DS_READ_B64:
			case Decoder::Opcode::DS_READ_B96:
			case Decoder::Opcode::DS_READ_B128:
			case Decoder::Opcode::DS_SWIZZLE_B32:
			case Decoder::Opcode::DS_CONSUME:
			case Decoder::Opcode::DS_APPEND:
			case Decoder::Opcode::DS_READ_ADDTID_B32: return true;
			default: return IsDsReturnAtomic(inst.opcode);
		}
	}
	return false;
}

Decoder::Operand MakeM0Operand() {
	Decoder::Operand operand;
	operand.kind = Decoder::OperandKind::M0;
	return operand;
}

Decoder::Operand MakeImmediate(uint32_t value) {
	Decoder::Operand operand;
	operand.kind  = Decoder::OperandKind::LiteralConstant;
	operand.value = value;
	return operand;
}

Decoder::Operand MemorySourceAt(const Decoder::Instruction& decoded, uint32_t index) {
	if (IsScalarAddressLoad(decoded.opcode) || IsScalarBufferLoad(decoded.opcode)) {
		return decoded.src1;
	}
	if (decoded.family == Decoder::Family::MUBUF || decoded.family == Decoder::Family::MTBUF) {
		const bool store_or_atomic =
		    (decoded.opcode >= Decoder::Opcode::BUFFER_STORE_FORMAT_X &&
		     decoded.opcode <= Decoder::Opcode::BUFFER_STORE_FORMAT_XYZW) ||
		    (decoded.opcode >= Decoder::Opcode::BUFFER_STORE_BYTE &&
		     decoded.opcode <= Decoder::Opcode::BUFFER_STORE_DWORDX4) ||
		    (decoded.opcode >= Decoder::Opcode::TBUFFER_STORE_FORMAT_X &&
		     decoded.opcode <= Decoder::Opcode::TBUFFER_STORE_FORMAT_XYZW) ||
		    (decoded.opcode >= Decoder::Opcode::BUFFER_ATOMIC_SWAP &&
		     decoded.opcode <= Decoder::Opcode::BUFFER_ATOMIC_FMAX);
		uint32_t   cursor          = 0;
		if (store_or_atomic) {
			if (index == cursor++) return decoded.dst;
		}
		if (decoded.idxen) {
			if (index == cursor++) return decoded.src0;
		}
		if (decoded.offen) {
			if (index == cursor++)
				return decoded.idxen ? OffsetDecodedRegister(decoded.src0, 1u) : decoded.src0;
		}
		if (index == cursor) return decoded.src2;
		return MakeImmediate(0u);
	}
	if (decoded.family == Decoder::Family::FLAT) {
		const bool store = decoded.opcode == Decoder::Opcode::FLAT_STORE_BYTE ||
		                   decoded.opcode == Decoder::Opcode::FLAT_STORE_SHORT ||
		                   decoded.opcode == Decoder::Opcode::FLAT_STORE_DWORD ||
		                   decoded.opcode == Decoder::Opcode::FLAT_STORE_DWORDX2 ||
		                   decoded.opcode == Decoder::Opcode::FLAT_STORE_DWORDX3 ||
		                   decoded.opcode == Decoder::Opcode::FLAT_STORE_DWORDX4;
		if (store) {
			return index == 0u ? decoded.dst : DecodedSourceAt(decoded, index - 1u);
		}
		return DecodedSourceAt(decoded, index);
	}
	if (decoded.family == Decoder::Family::MIMG) {
		const bool store_or_atomic = decoded.opcode == Decoder::Opcode::IMAGE_STORE ||
		                             decoded.opcode == Decoder::Opcode::IMAGE_STORE_MIP ||
		                             (decoded.opcode >= Decoder::Opcode::IMAGE_ATOMIC_ADD &&
		                              decoded.opcode <= Decoder::Opcode::IMAGE_ATOMIC_XOR);
		return store_or_atomic ? (index == 0u ? decoded.dst : decoded.src0) : decoded.src0;
	}
	if (decoded.family == Decoder::Family::DS) {
		switch (decoded.opcode) {
			case Decoder::Opcode::DS_SWIZZLE_B32:
				return index == 0u ? decoded.src0 : MakeImmediate(decoded.offset & 0xffffu);
			case Decoder::Opcode::DS_CONSUME:
			case Decoder::Opcode::DS_APPEND:
			case Decoder::Opcode::DS_READ_ADDTID_B32: return MakeM0Operand();
			case Decoder::Opcode::DS_WRITE_ADDTID_B32:
				return index == 0u ? decoded.src1 : MakeM0Operand();
			case Decoder::Opcode::DS_MIN_F32:
			case Decoder::Opcode::DS_MAX_F32:
				return index == 0u ? decoded.src1 : index == 1u ? decoded.src0 : decoded.src2;
			case Decoder::Opcode::DS_WRITE_B8:
			case Decoder::Opcode::DS_WRITE_B16:
			case Decoder::Opcode::DS_WRITE_B32:
			case Decoder::Opcode::DS_WRITE_B64:
			case Decoder::Opcode::DS_WRITE_B96:
			case Decoder::Opcode::DS_WRITE_B128:
			case Decoder::Opcode::DS_WRITE2_B32:
			case Decoder::Opcode::DS_WRITE2ST64_B32:
			case Decoder::Opcode::DS_WRITE2_B64:
			case Decoder::Opcode::DS_WRITE2ST64_B64:
				return index == 0u ? decoded.src1 : index == 1u ? decoded.src0 : decoded.src2;
			default:
				if (decoded.opcode >= Decoder::Opcode::DS_ADD_U32 &&
				    decoded.opcode <= Decoder::Opcode::DS_WRXCHG_RTN_B32) {
					return index == 0u ? decoded.src1 : decoded.src0;
				}
				return decoded.src0;
		}
	}
	return DecodedSourceAt(decoded, index);
}

} // namespace

IR::MemoryFlags Translator::AddMemoryInfo(const IR::MemoryInfo& memory, uint32_t pc) {
	const auto index = static_cast<uint32_t>(program.memory_info.size());
	program.memory_info.push_back(memory);
	return {.index = index, .pc = pc};
}

IR::U32 Translator::GetResourceDword(uint32_t index, uint32_t dword) {
	return ReadScalarCode(index * 4u + dword);
}

IR::Value Translator::GetBufferResource(const IR::MemoryInfo& memory) {
	return ir.Emit(IR::ValueOpcode::GetBufferResource,
	               {GetResourceDword(memory.resource, 0), GetResourceDword(memory.resource, 1),
	                GetResourceDword(memory.resource, 2), GetResourceDword(memory.resource, 3)});
}

IR::Value Translator::GetAddressResource(IR::Value low, IR::Value high) {
	return ir.Emit(IR::ValueOpcode::GetAddressResource, {low, high});
}

Translator::AddressOperands Translator::ReadAddressOperands(const Decoder::Instruction& inst,
                                                            uint32_t first_source) {
	const auto memory       = MemoryInfoFromDecoded(inst);
	const auto low          = ReadU32(MemorySourceAt(inst, first_source));
	const auto high_or_base = MemorySourceAt(inst, first_source + 1u);
	if (memory.kind == IR::ResourceKind::Scratch) {
		const auto offset =
		    high_or_base.kind != Decoder::OperandKind::Vgpr ? ReadU32(high_or_base) : low;
		return {ir.Emit(IR::ValueOpcode::GetScratchResource), offset, IR::Value(0u)};
	}
	if (memory.kind == IR::ResourceKind::Global &&
	    high_or_base.kind != Decoder::OperandKind::Vgpr) {
		const auto base_low  = ReadU32(high_or_base);
		const auto base_high = ReadU32(OffsetOperand(high_or_base, 1u));
		return {GetAddressResource(base_low, base_high), low, IR::Value(0u)};
	}
	const auto high = ReadU32(high_or_base);
	return {GetAddressResource(low, high), low, high};
}

IR::Value Translator::GetScalarAddressResource(uint32_t base) {
	return GetAddressResource(ReadScalarCode(base), ReadScalarCode(base + 1u));
}

IR::Value Translator::GetImageResource(const IR::MemoryInfo& memory) {
	const auto dword = [&](uint32_t index) {
		return memory.image_r128 && index >= 4u ? IR::U32(IR::Value(0u))
		                                        : GetResourceDword(memory.resource, index);
	};
	return ir.Emit(IR::ValueOpcode::GetImageResource, {dword(0), dword(1), dword(2), dword(3),
	                                                   dword(4), dword(5), dword(6), dword(7)});
}

IR::Value Translator::GetSamplerResource(const IR::MemoryInfo& memory) {
	return ir.Emit(IR::ValueOpcode::GetSamplerResource,
	               {GetResourceDword(memory.sampler, 0), GetResourceDword(memory.sampler, 1),
	                GetResourceDword(memory.sampler, 2), GetResourceDword(memory.sampler, 3)});
}

IR::Value Translator::MakeImageAddress(const Decoder::Instruction& inst,
                                       const Decoder::Operand&     base) {
	const auto                memory = MemoryInfoFromDecoded(inst);
	std::array<IR::Value, 13> components {};
	components[0] = ReadRawU32(PlainOperand(base));
	const auto nsa_components =
	    std::min(memory.image_nsa_dwords * 4u, Decoder::MaxImageNsaAddressComponents);
	for (uint32_t index = 1; index < components.size(); index++) {
		if (index - 1u < nsa_components) {
			components[index] =
			    ir.GetVectorReg(static_cast<IR::VectorReg>(memory.image_nsa_addr[index - 1u]));
		} else {
			components[index] = ReadRawU32(OffsetOperand(PlainOperand(base), index));
		}
	}
	return ir.Emit(IR::ValueOpcode::MakeImageAddress,
	               {components[0], components[1], components[2], components[3], components[4],
	                components[5], components[6], components[7], components[8], components[9],
	                components[10], components[11], components[12]});
}

IR::Value Translator::ConstructU32x4(const Decoder::Operand& base, uint32_t count) {
	std::array<IR::Value, 4> components {IR::Value(0u), IR::Value(0u), IR::Value(0u),
	                                     IR::Value(0u)};
	for (uint32_t index = 0; index < std::min(count, 4u); index++) {
		components[index] = ReadRawU32(OffsetOperand(PlainOperand(base), index));
	}
	return ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,
	               {components[0], components[1], components[2], components[3]});
}

void Translator::WriteImageComponents(const Decoder::Operand& dst, IR::Value value,
                                      const IR::MemoryInfo& memory, uint32_t component_limit) {
	if (memory.data_bits == 16u) {
		for (uint32_t index = 0; index < memory.data_dwords; index++) {
			WriteOperand(OffsetOperand(dst, index), ir.Emit(IR::ValueOpcode::CompositeExtractU32x4,
			                                                {value, IR::Value(index)}));
		}
		return;
	}
	const auto mask      = memory.dmask != 0u ? memory.dmask : 1u;
	uint32_t   dst_index = 0;
	for (uint32_t component = 0; component < component_limit; component++) {
		if (((mask >> component) & 1u) == 0u) {
			continue;
		}
		WriteOperand(
		    OffsetOperand(dst, dst_index++),
		    ir.Emit(IR::ValueOpcode::CompositeExtractU32x4, {value, IR::Value(component)}));
	}
}

IR::ValueOpcode Translator::ImageAtomicOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::IMAGE_ATOMIC_ADD: return IR::ValueOpcode::ImageAtomicIAdd32;
		case Decoder::Opcode::IMAGE_ATOMIC_UMIN: return IR::ValueOpcode::ImageAtomicUMin32;
		case Decoder::Opcode::IMAGE_ATOMIC_UMAX: return IR::ValueOpcode::ImageAtomicUMax32;
		case Decoder::Opcode::IMAGE_ATOMIC_AND: return IR::ValueOpcode::ImageAtomicAnd32;
		case Decoder::Opcode::IMAGE_ATOMIC_OR: return IR::ValueOpcode::ImageAtomicOr32;
		case Decoder::Opcode::IMAGE_ATOMIC_XOR: return IR::ValueOpcode::ImageAtomicXor32;
		default: EXIT("invalid image atomic opcode");
	}
}

Translator::BufferAddress Translator::ReadBufferAddress(const Decoder::Instruction& inst,
                                                        uint32_t                    first_source) {
	const auto memory  = MemoryInfoFromDecoded(inst);
	uint32_t   cursor  = first_source;
	const auto next    = [&]() { return ReadU32(MemorySourceAt(inst, cursor++)); };
	const auto index   = memory.idxen ? next() : IR::U32(IR::Value(0u));
	const auto offset  = memory.offen ? next() : IR::U32(IR::Value(0u));
	const auto soffset = next();
	return {index, offset, soffset};
}

IR::U32 Translator::WidenSubdword(IR::Value value, uint32_t bits, bool sign) {
	IR::U32 widened = bits == 8u ? IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U8, {value}))
	                             : IR::U32(ir.Emit(IR::ValueOpcode::ConvertU32U16, {value}));
	if (sign) {
		widened = IR::U32(
		    ir.Emit(IR::ValueOpcode::BitFieldSExtract, {widened, IR::Value(0u), IR::Value(bits)}));
	}
	return widened;
}

IR::Value Translator::NarrowSubdword(IR::U32 value, uint32_t bits) {
	return bits == 8u ? ir.Emit(IR::ValueOpcode::ConvertU8U32, {value})
	                  : ir.Emit(IR::ValueOpcode::ConvertU16U32, {value});
}

IR::ValueOpcode Translator::BufferAtomicOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::BUFFER_ATOMIC_SWAP: return IR::ValueOpcode::BufferAtomicSwap32;
		case Decoder::Opcode::BUFFER_ATOMIC_ADD: return IR::ValueOpcode::BufferAtomicIAdd32;
		case Decoder::Opcode::BUFFER_ATOMIC_SUB: return IR::ValueOpcode::BufferAtomicISub32;
		case Decoder::Opcode::BUFFER_ATOMIC_SMIN: return IR::ValueOpcode::BufferAtomicSMin32;
		case Decoder::Opcode::BUFFER_ATOMIC_UMIN: return IR::ValueOpcode::BufferAtomicUMin32;
		case Decoder::Opcode::BUFFER_ATOMIC_SMAX: return IR::ValueOpcode::BufferAtomicSMax32;
		case Decoder::Opcode::BUFFER_ATOMIC_UMAX: return IR::ValueOpcode::BufferAtomicUMax32;
		case Decoder::Opcode::BUFFER_ATOMIC_AND: return IR::ValueOpcode::BufferAtomicAnd32;
		case Decoder::Opcode::BUFFER_ATOMIC_OR: return IR::ValueOpcode::BufferAtomicOr32;
		case Decoder::Opcode::BUFFER_ATOMIC_XOR: return IR::ValueOpcode::BufferAtomicXor32;
		case Decoder::Opcode::BUFFER_ATOMIC_FMIN: return IR::ValueOpcode::BufferAtomicFMin32;
		case Decoder::Opcode::BUFFER_ATOMIC_FMAX: return IR::ValueOpcode::BufferAtomicFMax32;
		default: EXIT("invalid buffer atomic opcode");
	}
}

IR::ValueOpcode Translator::SharedAtomicOpcode(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::DS_WRXCHG_RTN_B32: return IR::ValueOpcode::SharedAtomicSwap32;
		case Decoder::Opcode::DS_ADD_U32:
		case Decoder::Opcode::DS_ADD_RTN_U32: return IR::ValueOpcode::SharedAtomicIAdd32;
		case Decoder::Opcode::DS_SUB_U32:
		case Decoder::Opcode::DS_SUB_RTN_U32: return IR::ValueOpcode::SharedAtomicISub32;
		case Decoder::Opcode::DS_MIN_I32:
		case Decoder::Opcode::DS_MIN_RTN_I32: return IR::ValueOpcode::SharedAtomicSMin32;
		case Decoder::Opcode::DS_MIN_U32:
		case Decoder::Opcode::DS_MIN_RTN_U32: return IR::ValueOpcode::SharedAtomicUMin32;
		case Decoder::Opcode::DS_MAX_I32:
		case Decoder::Opcode::DS_MAX_RTN_I32: return IR::ValueOpcode::SharedAtomicSMax32;
		case Decoder::Opcode::DS_MAX_U32:
		case Decoder::Opcode::DS_MAX_RTN_U32: return IR::ValueOpcode::SharedAtomicUMax32;
		case Decoder::Opcode::DS_AND_B32:
		case Decoder::Opcode::DS_AND_RTN_B32: return IR::ValueOpcode::SharedAtomicAnd32;
		case Decoder::Opcode::DS_OR_B32:
		case Decoder::Opcode::DS_OR_RTN_B32: return IR::ValueOpcode::SharedAtomicOr32;
		case Decoder::Opcode::DS_XOR_B32:
		case Decoder::Opcode::DS_XOR_RTN_B32: return IR::ValueOpcode::SharedAtomicXor32;
		default: EXIT("invalid shared atomic opcode");
	}
}

bool Translator::TranslateScalarMemory(const Decoder::Instruction& inst, std::string* error) {
	if (!IsScalarAddressLoad(inst.opcode) && !IsScalarBufferLoad(inst.opcode)) {
		return false;
	}
	const auto memory    = MemoryInfoFromDecoded(inst);
	uint32_t   dst_code  = 0;
	const auto alignment = memory.data_dwords == 1u ? 1u : memory.data_dwords == 2u ? 2u : 4u;
	if (!TryGetEncodedScalarCode(inst.dst, dst_code) || dst_code % alignment != 0u ||
	    dst_code + memory.data_dwords > 108u) {
		if (error != nullptr) {
			*error = "scalar-memory destination has an invalid aligned SGPR span";
		}
		return false;
	}
	uint32_t   base_code      = 0;
	const bool raw            = IsScalarAddressLoad(inst.opcode);
	const auto base_alignment = raw ? 2u : 4u;
	const auto base_width     = raw ? 2u : 4u;
	if (!TryGetEncodedScalarCode(inst.src0, base_code) || base_code % base_alignment != 0u ||
	    base_code + base_width > 108u) {
		if (error != nullptr) {
			*error = "scalar-memory base has an invalid aligned SGPR span";
		}
		return false;
	}
	const auto resource =
	    raw ? GetScalarAddressResource(memory.resource) : GetBufferResource(memory);
	const auto                offset = ReadU32(inst.src1);
	std::array<IR::Value, 16> loaded {};
	for (uint32_t component = 0; component < memory.data_dwords; component++) {
		auto scalar = memory;
		scalar.offset += component * sizeof(uint32_t);
		scalar.data_dwords     = 1u;
		scalar.component_index = component;
		if (!raw) {
			loaded[component] = ir.Emit(IR::ValueOpcode::ReadConstBuffer, {resource, offset},
			                            AddMemoryInfo(scalar, inst.pc));
		} else {
			loaded[component] = ir.Emit(IR::ValueOpcode::LoadAddressU32,
			                            {resource, offset, IR::Value(0u), IR::Value(true)},
			                            AddMemoryInfo(scalar, inst.pc));
		}
	}
	for (uint32_t component = 0; component < memory.data_dwords; component++) {
		WriteOperand(ScalarDestinationOperand(inst.dst, component), loaded[component]);
	}
	return true;
}

bool Translator::TranslateBufferLoad(const Decoder::Instruction& inst, std::string*) {
	const auto      memory = MemoryInfoFromDecoded(inst);
	IR::ValueOpcode opcode;
	const auto      bits = memory.data_bits;
	const auto      sign = memory.data_signed;
	switch (bits) {
		case 8u: opcode = IR::ValueOpcode::LoadBufferU8; break;
		case 16u: opcode = IR::ValueOpcode::LoadBufferU16; break;
		case 32u:
			switch (memory.data_dwords) {
				case 1u: opcode = IR::ValueOpcode::LoadBufferU32; break;
				case 2u: opcode = IR::ValueOpcode::LoadBufferU32x2; break;
				case 3u: opcode = IR::ValueOpcode::LoadBufferU32x3; break;
				case 4u: opcode = IR::ValueOpcode::LoadBufferU32x4; break;
				default: return false;
			}
			break;
		default: return false;
	}
	const auto resource = GetBufferResource(memory);
	const auto address  = ReadBufferAddress(inst, 0);
	const auto loaded =
	    ir.Emit(opcode, {resource, address.index, address.offset, address.soffset, ir.GetExec()},
	            AddMemoryInfo(memory, inst.pc));
	if (bits != 32u) {
		WriteOperand(inst.dst, WidenSubdword(loaded, bits, sign));
	} else if (memory.data_dwords == 1u) {
		WriteOperand(inst.dst, loaded);
	} else {
		for (uint32_t component = 0; component < memory.data_dwords; component++) {
			WriteOperand(OffsetOperand(inst.dst, component),
			             ir.CompositeExtract(loaded, component));
		}
	}
	return true;
}

bool Translator::TranslateBufferStore(const Decoder::Instruction& inst, std::string*) {
	const auto      memory   = MemoryInfoFromDecoded(inst);
	const auto      resource = GetBufferResource(memory);
	const auto      address  = ReadBufferAddress(inst, 1);
	const auto      data_src = MemorySourceAt(inst, 0);
	const auto      data     = ReadU32(data_src);
	IR::ValueOpcode opcode;
	IR::Value       value;
	switch (memory.data_bits) {
		case 8u:
			opcode = IR::ValueOpcode::StoreBufferU8;
			value  = NarrowSubdword(data, 8u);
			break;
		case 16u:
			opcode = IR::ValueOpcode::StoreBufferU16;
			value  = NarrowSubdword(data, 16u);
			break;
		case 32u:
			switch (memory.data_dwords) {
				case 1u:
					opcode = IR::ValueOpcode::StoreBufferU32;
					value  = data;
					break;
				case 2u:
					opcode = IR::ValueOpcode::StoreBufferU32x2;
					value  = ir.Emit(IR::ValueOpcode::CompositeConstructU32x2,
					                 {data, ReadU32(OffsetOperand(data_src, 1u))});
					break;
				case 3u:
					opcode = IR::ValueOpcode::StoreBufferU32x3;
					value  = ir.Emit(IR::ValueOpcode::CompositeConstructU32x3,
					                 {data, ReadU32(OffsetOperand(data_src, 1u)),
					                  ReadU32(OffsetOperand(data_src, 2u))});
					break;
				case 4u:
					opcode = IR::ValueOpcode::StoreBufferU32x4;
					value  = ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,
					                 {data, ReadU32(OffsetOperand(data_src, 1u)),
					                  ReadU32(OffsetOperand(data_src, 2u)),
					                  ReadU32(OffsetOperand(data_src, 3u))});
					break;
				default: return false;
			}
			break;
		default: return false;
	}
	ir.Emit(opcode, {resource, address.index, address.offset, address.soffset, value, ir.GetExec()},
	        AddMemoryInfo(memory, inst.pc));
	return true;
}

bool Translator::TranslateAtomicMemory(const Decoder::Instruction& inst, std::string*) {
	const auto memory = MemoryInfoFromDecoded(inst);
	IR::Value  result;
	switch (memory.kind) {
		case IR::ResourceKind::Buffer: {
			const auto resource = GetBufferResource(memory);
			const auto address  = ReadBufferAddress(inst, 1);
			result              = ir.Emit(BufferAtomicOpcode(inst.opcode),
			                              {resource, address.index, address.offset, address.soffset,
			                               ReadU32(MemorySourceAt(inst, 0)), ir.GetExec()},
			                              AddMemoryInfo(memory, inst.pc));
			break;
		}
		case IR::ResourceKind::Image:
		case IR::ResourceKind::ImageUint:
		case IR::ResourceKind::StorageImage:
		case IR::ResourceKind::StorageImageUint: {
			const auto resource = GetImageResource(memory);
			const auto address  = MakeImageAddress(inst, MemorySourceAt(inst, 1));
			const auto flags    = AddMemoryInfo(memory, inst.pc);
			result =
			    ir.Emit(ImageAtomicOpcode(inst.opcode),
			            {resource, address, ReadU32(MemorySourceAt(inst, 0)), ir.GetExec()}, flags);
			break;
		}
		case IR::ResourceKind::Lds:
		case IR::ResourceKind::Gds: {
			const auto address = ReadU32(MemorySourceAt(inst, 1));
			result             = ir.Emit(SharedAtomicOpcode(inst.opcode),
			                             {address, ReadU32(MemorySourceAt(inst, 0)), ir.GetExec()},
			                             AddMemoryInfo(memory, inst.pc));
			break;
		}
		default: return false;
	}
	if (inst.glc || IsDsReturnAtomic(inst.opcode)) {
		WriteOperand(inst.dst, result);
	}
	return true;
}

bool Translator::TranslateFlatLoad(const Decoder::Instruction& inst, std::string*) {
	const auto      memory = MemoryInfoFromDecoded(inst);
	IR::ValueOpcode opcode;
	const auto      bits = memory.data_bits;
	const auto      sign = memory.data_signed;
	switch (bits) {
		case 8u: opcode = IR::ValueOpcode::LoadAddressU8; break;
		case 16u: opcode = IR::ValueOpcode::LoadAddressU16; break;
		case 32u: opcode = IR::ValueOpcode::LoadAddressU32; break;
		default: return false;
	}
	const auto address = ReadAddressOperands(inst, 0);
	const auto active  = ir.GetExec();
	const auto count   = bits == 32u ? std::min(memory.data_dwords, 4u) : 1u;
	for (uint32_t index = 0; index < count; index++) {
		auto component = memory;
		component.offset += index * 4u;
		component.data_dwords     = 1u;
		component.component_index = index;
		const auto loaded = ir.Emit(opcode, {address.resource, address.low, address.high, active},
		                            AddMemoryInfo(component, inst.pc));
		WriteOperand(OffsetOperand(inst.dst, index),
		             bits == 32u ? loaded : WidenSubdword(loaded, bits, sign));
	}
	return true;
}

bool Translator::TranslateFlatStore(const Decoder::Instruction& inst, std::string*) {
	const auto      memory  = MemoryInfoFromDecoded(inst);
	const auto      data_op = MemorySourceAt(inst, 0);
	const auto      address = ReadAddressOperands(inst, 1);
	IR::ValueOpcode opcode;
	switch (memory.data_bits) {
		case 8u: opcode = IR::ValueOpcode::StoreAddressU8; break;
		case 16u: opcode = IR::ValueOpcode::StoreAddressU16; break;
		case 32u: opcode = IR::ValueOpcode::StoreAddressU32; break;
		default: return false;
	}
	const auto count = memory.data_bits == 32u ? memory.data_dwords : 1u;
	for (uint32_t index = 0; index < count; index++) {
		auto component = memory;
		component.offset += index * 4u;
		component.data_dwords     = 1u;
		component.component_index = index;
		auto value                = IR::Value(ReadU32(OffsetOperand(data_op, index)));
		if (memory.data_bits != 32u) {
			value = NarrowSubdword(IR::U32(value), memory.data_bits);
		}
		ir.Emit(opcode, {address.resource, address.low, address.high, value, ir.GetExec()},
		        AddMemoryInfo(component, inst.pc));
	}
	return true;
}

bool Translator::TranslateImageMemory(const Decoder::Instruction& inst, std::string*) {
	const auto memory = MemoryInfoFromDecoded(inst);
	const bool image  = memory.kind == IR::ResourceKind::Image ||
	                    memory.kind == IR::ResourceKind::ImageUint ||
	                    memory.kind == IR::ResourceKind::StorageImage ||
	                    memory.kind == IR::ResourceKind::StorageImageUint;
	if (!image) {
		return false;
	}
	const auto resource = GetImageResource(memory);
	const bool store    = inst.opcode == Decoder::Opcode::IMAGE_STORE ||
	                      inst.opcode == Decoder::Opcode::IMAGE_STORE_MIP;
	const auto address  = MakeImageAddress(inst, MemorySourceAt(inst, store ? 1u : 0u));
	const auto flags    = AddMemoryInfo(memory, inst.pc);
	switch (inst.opcode) {
		case Decoder::Opcode::IMAGE_GET_RESINFO: {
			const auto result =
			    ir.Emit(IR::ValueOpcode::ImageQueryDimensions, {resource, address}, flags);
			WriteImageComponents(inst.dst, result, memory, 4u);
			return true;
		}
		case Decoder::Opcode::IMAGE_GET_LOD: {
			const auto sampler = GetSamplerResource(memory);
			const auto result =
			    ir.Emit(IR::ValueOpcode::ImageQueryLod, {resource, sampler, address}, flags);
			WriteImageComponents(inst.dst, result, memory, 2u);
			return true;
		}
		case Decoder::Opcode::IMAGE_LOAD:
		case Decoder::Opcode::IMAGE_LOAD_MIP: {
			const auto result =
			    ir.Emit(IR::ValueOpcode::ImageRead, {resource, address, ir.GetExec()}, flags);
			WriteImageComponents(inst.dst, result, memory, 4u);
			return true;
		}
		case Decoder::Opcode::IMAGE_STORE:
		case Decoder::Opcode::IMAGE_STORE_MIP: {
			const auto data = ConstructU32x4(MemorySourceAt(inst, 0), memory.data_dwords);
			ir.Emit(IR::ValueOpcode::ImageWrite, {resource, address, data, ir.GetExec()}, flags);
			return true;
		}
		case Decoder::Opcode::IMAGE_SAMPLE:
		case Decoder::Opcode::IMAGE_GATHER4_LZ:
		case Decoder::Opcode::IMAGE_GATHER4_C:
		case Decoder::Opcode::IMAGE_GATHER4_C_LZ:
		case Decoder::Opcode::IMAGE_GATHER4_LZ_O:
		case Decoder::Opcode::IMAGE_GATHER4_C_O:
		case Decoder::Opcode::IMAGE_GATHER4_C_LZ_O:
		case Decoder::Opcode::IMAGE_GATHER4H: {
			const auto sampler = GetSamplerResource(memory);
			const bool sample  = inst.opcode == Decoder::Opcode::IMAGE_SAMPLE;
			const auto opcode =
			    sample ? IR::ValueOpcode::ImageSampleRaw : IR::ValueOpcode::ImageGatherRaw;
			const auto result = ir.Emit(opcode, {resource, sampler, address}, flags);
			const bool dref   = (memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0u;
			if (sample && dref && memory.data_bits != 16u) {
				const auto component =
				    ir.Emit(IR::ValueOpcode::CompositeExtractU32x4, {result, IR::Value(0u)});
				for (uint32_t index = 0; index < memory.data_dwords; index++) {
					WriteOperand(OffsetOperand(inst.dst, index), component);
				}
			} else if (!sample) {
				for (uint32_t index = 0; index < memory.data_dwords; index++) {
					WriteOperand(OffsetOperand(inst.dst, index),
					             ir.Emit(IR::ValueOpcode::CompositeExtractU32x4,
					                     {result, IR::Value(index)}));
				}
			} else {
				WriteImageComponents(inst.dst, result, memory, 4u);
			}
			return true;
		}
		default: return false;
	}
}

bool Translator::TranslateSharedMemory(const Decoder::Instruction& inst, std::string*) {
	const auto memory = MemoryInfoFromDecoded(inst);
	const bool shared =
	    memory.kind == IR::ResourceKind::Lds || memory.kind == IR::ResourceKind::Gds;
	if (!shared && inst.opcode != Decoder::Opcode::DS_SWIZZLE_B32) {
		return false;
	}
	const auto shared_address = [&](uint32_t source) {
		return ReadU32(MemorySourceAt(inst, source));
	};
	const auto load_u32 = [&](uint32_t width, IR::U32 address, const IR::MemoryInfo& source) {
		IR::ValueOpcode opcode;
		switch (width) {
			case 1u: opcode = IR::ValueOpcode::LoadSharedU32; break;
			case 2u: opcode = IR::ValueOpcode::LoadSharedU32x2; break;
			case 3u: opcode = IR::ValueOpcode::LoadSharedU32x3; break;
			case 4u: opcode = IR::ValueOpcode::LoadSharedU32x4; break;
			default: EXIT("invalid shared load width");
		}
		return ir.Emit(opcode, {address, ir.GetExec()}, AddMemoryInfo(source, inst.pc));
	};
	const auto extract_u32 = [&](IR::Value value, uint32_t width, uint32_t index) {
		if (width == 1u) {
			return value;
		}
		const auto opcode = width == 2u   ? IR::ValueOpcode::CompositeExtractU32x2
		                    : width == 3u ? IR::ValueOpcode::CompositeExtractU32x3
		                                  : IR::ValueOpcode::CompositeExtractU32x4;
		return ir.Emit(opcode, {value, IR::Value(index)});
	};
	const auto write_u32 = [&](uint32_t width, IR::U32 address,
	                           const std::array<IR::Value, 4>& values,
	                           const IR::MemoryInfo&           source) {
		switch (width) {
			case 1u:
				ir.Emit(IR::ValueOpcode::WriteSharedU32, {address, values[0], ir.GetExec()},
				        AddMemoryInfo(source, inst.pc));
				break;
			case 2u:
				ir.Emit(IR::ValueOpcode::WriteSharedU32x2,
				        {address, values[0], values[1], ir.GetExec()},
				        AddMemoryInfo(source, inst.pc));
				break;
			case 3u:
				ir.Emit(IR::ValueOpcode::WriteSharedU32x3,
				        {address, values[0], values[1], values[2], ir.GetExec()},
				        AddMemoryInfo(source, inst.pc));
				break;
			case 4u:
				ir.Emit(IR::ValueOpcode::WriteSharedU32x4,
				        {address, values[0], values[1], values[2], values[3], ir.GetExec()},
				        AddMemoryInfo(source, inst.pc));
				break;
			default: EXIT("invalid shared store width");
		}
	};
	switch (inst.opcode) {
		case Decoder::Opcode::DS_READ_I8:
		case Decoder::Opcode::DS_READ_U8:
		case Decoder::Opcode::DS_READ_I16:
		case Decoder::Opcode::DS_READ_U16:
		case Decoder::Opcode::DS_READ_U16_D16:
		case Decoder::Opcode::DS_READ_B32:
		case Decoder::Opcode::DS_READ_B64:
		case Decoder::Opcode::DS_READ_B96:
		case Decoder::Opcode::DS_READ_B128: {
			IR::ValueOpcode opcode;
			uint32_t        bits;
			bool            sign;
			if (memory.data_bits == 8u) {
				opcode = IR::ValueOpcode::LoadSharedU8;
				bits   = 8u;
				sign   = memory.data_signed;
			} else if (memory.data_bits == 16u) {
				opcode = IR::ValueOpcode::LoadSharedU16;
				bits   = 16u;
				sign   = memory.data_signed;
			} else {
				const auto width  = memory.data_dwords;
				const auto loaded = load_u32(width, shared_address(0), memory);
				for (uint32_t index = 0; index < width; index++) {
					WriteOperand(OffsetOperand(inst.dst, index), extract_u32(loaded, width, index));
				}
				return true;
			}
			const auto loaded =
			    ir.Emit(opcode, {shared_address(0), ir.GetExec()}, AddMemoryInfo(memory, inst.pc));
			WriteOperand(inst.dst, bits == 32u ? loaded : WidenSubdword(loaded, bits, sign));
			return true;
		}
		case Decoder::Opcode::DS_READ2_B32:
		case Decoder::Opcode::DS_READ2ST64_B32:
		case Decoder::Opcode::DS_READ2_B64:
		case Decoder::Opcode::DS_READ2ST64_B64: {
			const auto width        = memory.data_dwords / 2u;
			const auto address      = shared_address(0);
			auto       first        = memory;
			first.data_dwords       = width;
			first.component_count   = width;
			const auto first_value  = load_u32(width, address, first);
			IR::Value  second_value = first_value;
			if (memory.secondary_offset != memory.offset) {
				auto second   = first;
				second.offset = memory.secondary_offset;
				second_value  = load_u32(width, address, second);
			}
			for (uint32_t index = 0; index < width; index++) {
				WriteOperand(OffsetOperand(inst.dst, index),
				             extract_u32(first_value, width, index));
				WriteOperand(OffsetOperand(inst.dst, width + index),
				             extract_u32(second_value, width, index));
			}
			return true;
		}
		case Decoder::Opcode::DS_WRITE_B8:
		case Decoder::Opcode::DS_WRITE_B16:
		case Decoder::Opcode::DS_WRITE_B32:
		case Decoder::Opcode::DS_WRITE_B64:
		case Decoder::Opcode::DS_WRITE_B96:
		case Decoder::Opcode::DS_WRITE_B128: {
			const auto               width        = memory.data_dwords;
			const auto               data_operand = MemorySourceAt(inst, 0);
			std::array<IR::Value, 4> values {};
			for (uint32_t index = 0; index < width; index++) {
				values[index] = ReadU32(OffsetOperand(data_operand, index));
			}
			const auto      data    = IR::U32(values[0]);
			const auto      address = shared_address(1);
			IR::ValueOpcode opcode;
			IR::Value       value;
			if (memory.data_bits == 8u) {
				opcode = IR::ValueOpcode::WriteSharedU8;
				value  = NarrowSubdword(data, 8u);
			} else if (memory.data_bits == 16u) {
				opcode = IR::ValueOpcode::WriteSharedU16;
				value  = NarrowSubdword(data, 16u);
			} else {
				write_u32(width, address, values, memory);
				return true;
			}
			ir.Emit(opcode, {address, value, ir.GetExec()}, AddMemoryInfo(memory, inst.pc));
			return true;
		}
		case Decoder::Opcode::DS_WRITE2_B32:
		case Decoder::Opcode::DS_WRITE2ST64_B32:
		case Decoder::Opcode::DS_WRITE2_B64:
		case Decoder::Opcode::DS_WRITE2ST64_B64: {
			const auto               width       = memory.data_dwords / 2u;
			const auto               address     = shared_address(1);
			const auto               first_data  = MemorySourceAt(inst, 0);
			const auto               second_data = MemorySourceAt(inst, 2);
			std::array<IR::Value, 4> first_values {};
			std::array<IR::Value, 4> second_values {};
			for (uint32_t index = 0; index < width; index++) {
				first_values[index]  = ReadU32(OffsetOperand(first_data, index));
				second_values[index] = ReadU32(OffsetOperand(second_data, index));
			}
			auto first            = memory;
			first.data_dwords     = width;
			first.component_count = width;
			write_u32(width, address, first_values, first);
			if (memory.secondary_offset != memory.offset) {
				auto second   = first;
				second.offset = memory.secondary_offset;
				write_u32(width, address, second_values, second);
			}
			return true;
		}
		case Decoder::Opcode::DS_MIN_F32:
		case Decoder::Opcode::DS_MAX_F32: {
			const auto opcode = inst.opcode == Decoder::Opcode::DS_MIN_F32
			                        ? IR::ValueOpcode::SharedAtomicFMin32
			                        : IR::ValueOpcode::SharedAtomicFMax32;
			ir.Emit(opcode,
			        {shared_address(1), ReadU32(MemorySourceAt(inst, 0)),
			         ReadU32(MemorySourceAt(inst, 2)), ir.GetExec()},
			        AddMemoryInfo(memory, inst.pc));
			return true;
		}
		case Decoder::Opcode::DS_APPEND:
		case Decoder::Opcode::DS_CONSUME: {
			const bool append = inst.opcode == Decoder::Opcode::DS_APPEND;
			const auto opcode = append ? IR::ValueOpcode::DataAppend : IR::ValueOpcode::DataConsume;
			WriteOperand(inst.dst, ir.Emit(opcode,
			                               {ReadU32(MemorySourceAt(inst, 0)), ir.GetExec(),
			                                ir.GetExecLo(), ir.GetExecHi()},
			                               AddMemoryInfo(memory, inst.pc)));
			return true;
		}
		case Decoder::Opcode::DS_WRITE_ADDTID_B32:
		case Decoder::Opcode::DS_READ_ADDTID_B32: {
			const auto m0_index = inst.opcode == Decoder::Opcode::DS_WRITE_ADDTID_B32 ? 1u : 0u;
			const auto base =
			    ir.BitwiseAnd(ReadU32(MemorySourceAt(inst, m0_index)), IR::U32(IR::Value(0xffffu)));
			const auto lane    = IR::U32(ir.Emit(IR::ValueOpcode::LaneId));
			const auto address = ir.IAdd(base, ir.ShiftLeftLogical(lane, IR::U32(IR::Value(2u))));
			if (inst.opcode == Decoder::Opcode::DS_WRITE_ADDTID_B32) {
				ir.Emit(IR::ValueOpcode::WriteSharedU32,
				        {address, ReadU32(MemorySourceAt(inst, 0)), ir.GetExec()},
				        AddMemoryInfo(memory, inst.pc));
			} else {
				WriteOperand(inst.dst,
				             ir.Emit(IR::ValueOpcode::LoadSharedU32, {address, ir.GetExec()},
				                     AddMemoryInfo(memory, inst.pc)));
			}
			return true;
		}
		case Decoder::Opcode::DS_SWIZZLE_B32:
			WriteOperand(inst.dst, ir.Emit(IR::ValueOpcode::SwizzleU32,
			                               {ReadU32(MemorySourceAt(inst, 0)),
			                                ReadU32(MemorySourceAt(inst, 1)), ir.GetExec()}));
			return true;
		default: return false;
	}
}

bool Translator::TranslateMemoryOperation(const Decoder::Instruction& inst, std::string* error) {
	if (!MemoryOpcodeMatchesFamily(inst.opcode, inst.family)) {
		if (error != nullptr) {
			*error = fmt::format("memory opcode {} does not belong to decoded family {}",
			                     magic_enum::enum_name(inst.opcode), magic_enum::enum_name(inst.family));
		}
		return false;
	}
	if (RequiresRegisterDestination(inst) && !IsRegisterOperand(inst.dst)) {
		if (error != nullptr) {
			*error = "decoded operand cannot be used as an IR register";
		}
		return false;
	}
	if (UsesDestinationAsMemorySource(inst.opcode) &&
	    inst.dst.kind == Decoder::OperandKind::Unknown) {
		if (error != nullptr) {
			*error = "decoded operand cannot be used as an IR register";
		}
		return false;
	}
	for (uint32_t index = 0; index < inst.src_count; index++) {
		if (DecodedSourceAt(inst, index).kind == Decoder::OperandKind::Unknown) {
			if (error != nullptr) {
				*error = "decoded operand cannot be used as an IR register";
			}
			return false;
		}
	}
	switch (inst.opcode) {
		case Decoder::Opcode::S_LOAD_DWORD:
		case Decoder::Opcode::S_LOAD_DWORDX2:
		case Decoder::Opcode::S_LOAD_DWORDX4:
		case Decoder::Opcode::S_LOAD_DWORDX8:
		case Decoder::Opcode::S_LOAD_DWORDX16:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORD:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX2:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX4:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX8:
		case Decoder::Opcode::S_BUFFER_LOAD_DWORDX16: return TranslateScalarMemory(inst, error);

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
		case Decoder::Opcode::TBUFFER_LOAD_FORMAT_XYZW: return TranslateBufferLoad(inst, error);

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
		case Decoder::Opcode::TBUFFER_STORE_FORMAT_XYZW: return TranslateBufferStore(inst, error);

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
		case Decoder::Opcode::IMAGE_ATOMIC_ADD:
		case Decoder::Opcode::IMAGE_ATOMIC_UMIN:
		case Decoder::Opcode::IMAGE_ATOMIC_UMAX:
		case Decoder::Opcode::IMAGE_ATOMIC_AND:
		case Decoder::Opcode::IMAGE_ATOMIC_OR:
		case Decoder::Opcode::IMAGE_ATOMIC_XOR: return TranslateAtomicMemory(inst, error);

		case Decoder::Opcode::FLAT_LOAD_UBYTE:
		case Decoder::Opcode::FLAT_LOAD_SBYTE:
		case Decoder::Opcode::FLAT_LOAD_USHORT:
		case Decoder::Opcode::FLAT_LOAD_SSHORT:
		case Decoder::Opcode::FLAT_LOAD_DWORD:
		case Decoder::Opcode::FLAT_LOAD_DWORDX2:
		case Decoder::Opcode::FLAT_LOAD_DWORDX3:
		case Decoder::Opcode::FLAT_LOAD_DWORDX4: return TranslateFlatLoad(inst, error);

		case Decoder::Opcode::FLAT_STORE_BYTE:
		case Decoder::Opcode::FLAT_STORE_SHORT:
		case Decoder::Opcode::FLAT_STORE_DWORD:
		case Decoder::Opcode::FLAT_STORE_DWORDX2:
		case Decoder::Opcode::FLAT_STORE_DWORDX3:
		case Decoder::Opcode::FLAT_STORE_DWORDX4: return TranslateFlatStore(inst, error);

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
		case Decoder::Opcode::IMAGE_SAMPLE: return TranslateImageMemory(inst, error);

		case Decoder::Opcode::DS_MIN_F32:
		case Decoder::Opcode::DS_MAX_F32:
		case Decoder::Opcode::DS_SWIZZLE_B32:
		case Decoder::Opcode::DS_CONSUME:
		case Decoder::Opcode::DS_APPEND:
		case Decoder::Opcode::DS_WRITE_ADDTID_B32:
		case Decoder::Opcode::DS_READ_ADDTID_B32:
		case Decoder::Opcode::DS_READ2_B32:
		case Decoder::Opcode::DS_READ2ST64_B32:
		case Decoder::Opcode::DS_READ2_B64:
		case Decoder::Opcode::DS_READ2ST64_B64:
		case Decoder::Opcode::DS_READ_I8:
		case Decoder::Opcode::DS_READ_U8:
		case Decoder::Opcode::DS_READ_I16:
		case Decoder::Opcode::DS_READ_U16:
		case Decoder::Opcode::DS_READ_U16_D16:
		case Decoder::Opcode::DS_READ_B32:
		case Decoder::Opcode::DS_READ_B64:
		case Decoder::Opcode::DS_READ_B96:
		case Decoder::Opcode::DS_READ_B128:
		case Decoder::Opcode::DS_WRITE2_B32:
		case Decoder::Opcode::DS_WRITE2ST64_B32:
		case Decoder::Opcode::DS_WRITE2_B64:
		case Decoder::Opcode::DS_WRITE2ST64_B64:
		case Decoder::Opcode::DS_WRITE_B8:
		case Decoder::Opcode::DS_WRITE_B16:
		case Decoder::Opcode::DS_WRITE_B32:
		case Decoder::Opcode::DS_WRITE_B64:
		case Decoder::Opcode::DS_WRITE_B96:
		case Decoder::Opcode::DS_WRITE_B128: return TranslateSharedMemory(inst, error);
		default:
			if (error != nullptr) {
				*error = fmt::format("memory-family opcode has no specialized IR translation: {}",
				                     magic_enum::enum_name(inst.opcode));
			}
			return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
