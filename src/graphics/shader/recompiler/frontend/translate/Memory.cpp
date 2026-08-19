#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail {

IR::MemoryFlags Translator::AddMemoryInfo(const IR::Instruction& inst) {
	const auto index = static_cast<uint32_t>(value_program.memory_info.size());
	value_program.memory_info.push_back(inst.memory);
	return {.index = index, .pc = inst.pc};
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

Translator::AddressOperands Translator::ReadAddressOperands(const IR::Instruction& inst,
                                                            uint32_t               first_source) {
	const auto  low          = ReadU32(inst.src[first_source]);
	const auto& high_or_base = inst.src[first_source + 1u];
	if (inst.memory.kind == IR::ResourceKind::Scratch) {
		const auto offset = high_or_base.kind == IR::OperandKind::Register &&
		                            high_or_base.reg.file != IR::RegisterFile::Vector
		                        ? ReadU32(high_or_base)
		                        : low;
		return {GetAddressResource(offset, IR::Value(0u)), offset, IR::Value(0u)};
	}
	if (inst.memory.kind == IR::ResourceKind::Global &&
	    high_or_base.kind == IR::OperandKind::Register &&
	    high_or_base.reg.file != IR::RegisterFile::Vector) {
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
	return ir.Emit(IR::ValueOpcode::GetImageResource,
	               {GetResourceDword(memory.resource, 0), GetResourceDword(memory.resource, 1),
	                GetResourceDword(memory.resource, 2), GetResourceDword(memory.resource, 3),
	                GetResourceDword(memory.resource, 4), GetResourceDword(memory.resource, 5),
	                GetResourceDword(memory.resource, 6), GetResourceDword(memory.resource, 7)});
}

IR::Value Translator::GetSamplerResource(const IR::MemoryInfo& memory) {
	return ir.Emit(IR::ValueOpcode::GetSamplerResource,
	               {GetResourceDword(memory.sampler, 0), GetResourceDword(memory.sampler, 1),
	                GetResourceDword(memory.sampler, 2), GetResourceDword(memory.sampler, 3)});
}

IR::Value Translator::GetSharedResource(IR::ResourceKind kind) {
	return ir.Emit(kind == IR::ResourceKind::Gds ? IR::ValueOpcode::GetGdsResource
	                                             : IR::ValueOpcode::GetLdsResource);
}

IR::Value Translator::MakeImageAddress(const IR::Instruction& inst, const IR::Operand& base) {
	std::array<IR::Value, 13> components {};
	components[0] = ReadRawU32(PlainOperand(base));
	const auto nsa_components =
	    std::min(inst.memory.image_nsa_dwords * 4u, Decoder::MaxImageNsaAddressComponents);
	for (uint32_t index = 1; index < components.size(); index++) {
		if (index - 1u < nsa_components) {
			components[index] =
			    ir.GetVectorReg(static_cast<IR::VectorReg>(inst.memory.image_nsa_addr[index - 1u]));
		} else {
			components[index] = ReadRawU32(OffsetOperand(PlainOperand(base), index));
		}
	}
	return ir.Emit(IR::ValueOpcode::MakeImageAddress,
	               {components[0], components[1], components[2], components[3], components[4],
	                components[5], components[6], components[7], components[8], components[9],
	                components[10], components[11], components[12]});
}

IR::Value Translator::ConstructU32x4(const IR::Operand& base, uint32_t count) {
	std::array<IR::Value, 4> components {IR::Value(0u), IR::Value(0u), IR::Value(0u),
	                                     IR::Value(0u)};
	for (uint32_t index = 0; index < std::min(count, 4u); index++) {
		components[index] = ReadRawU32(OffsetOperand(PlainOperand(base), index));
	}
	return ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,
	               {components[0], components[1], components[2], components[3]});
}

void Translator::WriteImageComponents(const IR::Operand& dst, IR::Value value, uint32_t dmask,
                                      uint32_t component_limit) {
	const auto mask      = dmask != 0u ? dmask : 1u;
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

IR::ValueOpcode Translator::ImageAtomicOpcode(IR::Opcode opcode) {
	switch (opcode) {
		case IR::Opcode::AtomicAddU32: return IR::ValueOpcode::ImageAtomicIAdd32;
		case IR::Opcode::AtomicUMinU32: return IR::ValueOpcode::ImageAtomicUMin32;
		case IR::Opcode::AtomicUMaxU32: return IR::ValueOpcode::ImageAtomicUMax32;
		case IR::Opcode::AtomicAndU32: return IR::ValueOpcode::ImageAtomicAnd32;
		case IR::Opcode::AtomicOrU32: return IR::ValueOpcode::ImageAtomicOr32;
		case IR::Opcode::AtomicXorU32: return IR::ValueOpcode::ImageAtomicXor32;
		default: EXIT("invalid image atomic opcode");
	}
}

Translator::BufferAddress Translator::ReadBufferAddress(const IR::Instruction& inst,
                                                        uint32_t               first_source) {
	uint32_t   cursor = first_source;
	const auto next   = [&]() {
		return cursor < inst.src_count ? ReadU32(inst.src[cursor++]) : IR::U32(IR::Value(0u));
	};
	const auto index   = inst.memory.idxen ? next() : IR::U32(IR::Value(0u));
	const auto offset  = inst.memory.offen ? next() : IR::U32(IR::Value(0u));
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

IR::ValueOpcode Translator::BufferAtomicOpcode(IR::Opcode opcode) {
	switch (opcode) {
		case IR::Opcode::AtomicSwapU32: return IR::ValueOpcode::BufferAtomicSwap32;
		case IR::Opcode::AtomicAddU32: return IR::ValueOpcode::BufferAtomicIAdd32;
		case IR::Opcode::AtomicSubU32: return IR::ValueOpcode::BufferAtomicISub32;
		case IR::Opcode::AtomicSMinI32: return IR::ValueOpcode::BufferAtomicSMin32;
		case IR::Opcode::AtomicUMinU32: return IR::ValueOpcode::BufferAtomicUMin32;
		case IR::Opcode::AtomicSMaxI32: return IR::ValueOpcode::BufferAtomicSMax32;
		case IR::Opcode::AtomicUMaxU32: return IR::ValueOpcode::BufferAtomicUMax32;
		case IR::Opcode::AtomicAndU32: return IR::ValueOpcode::BufferAtomicAnd32;
		case IR::Opcode::AtomicOrU32: return IR::ValueOpcode::BufferAtomicOr32;
		case IR::Opcode::AtomicXorU32: return IR::ValueOpcode::BufferAtomicXor32;
		case IR::Opcode::AtomicFMinF32: return IR::ValueOpcode::BufferAtomicFMin32;
		case IR::Opcode::AtomicFMaxF32: return IR::ValueOpcode::BufferAtomicFMax32;
		default: EXIT("invalid buffer atomic opcode");
	}
}

IR::ValueOpcode Translator::SharedAtomicOpcode(IR::Opcode opcode, bool gds) {
	if (gds) {
		switch (opcode) {
			case IR::Opcode::AtomicSwapU32: return IR::ValueOpcode::GdsAtomicSwap32;
			case IR::Opcode::AtomicAddU32: return IR::ValueOpcode::GdsAtomicIAdd32;
			case IR::Opcode::AtomicSubU32: return IR::ValueOpcode::GdsAtomicISub32;
			case IR::Opcode::AtomicSMinI32: return IR::ValueOpcode::GdsAtomicSMin32;
			case IR::Opcode::AtomicUMinU32: return IR::ValueOpcode::GdsAtomicUMin32;
			case IR::Opcode::AtomicSMaxI32: return IR::ValueOpcode::GdsAtomicSMax32;
			case IR::Opcode::AtomicUMaxU32: return IR::ValueOpcode::GdsAtomicUMax32;
			case IR::Opcode::AtomicAndU32: return IR::ValueOpcode::GdsAtomicAnd32;
			case IR::Opcode::AtomicOrU32: return IR::ValueOpcode::GdsAtomicOr32;
			case IR::Opcode::AtomicXorU32: return IR::ValueOpcode::GdsAtomicXor32;
			default: EXIT("invalid GDS atomic opcode");
		}
	}
	switch (opcode) {
		case IR::Opcode::AtomicSwapU32: return IR::ValueOpcode::SharedAtomicSwap32;
		case IR::Opcode::AtomicAddU32: return IR::ValueOpcode::SharedAtomicIAdd32;
		case IR::Opcode::AtomicSubU32: return IR::ValueOpcode::SharedAtomicISub32;
		case IR::Opcode::AtomicSMinI32: return IR::ValueOpcode::SharedAtomicSMin32;
		case IR::Opcode::AtomicUMinU32: return IR::ValueOpcode::SharedAtomicUMin32;
		case IR::Opcode::AtomicSMaxI32: return IR::ValueOpcode::SharedAtomicSMax32;
		case IR::Opcode::AtomicUMaxU32: return IR::ValueOpcode::SharedAtomicUMax32;
		case IR::Opcode::AtomicAndU32: return IR::ValueOpcode::SharedAtomicAnd32;
		case IR::Opcode::AtomicOrU32: return IR::ValueOpcode::SharedAtomicOr32;
		case IR::Opcode::AtomicXorU32: return IR::ValueOpcode::SharedAtomicXor32;
		default: EXIT("invalid LDS atomic opcode");
	}
}

bool Translator::IsScalarMemoryLoadOperation(IR::Opcode opcode) {
	return opcode == IR::Opcode::SLoadDword || opcode == IR::Opcode::SBufferLoadDword;
}

ScalarMemorySourceValues Translator::ReadScalarMemorySource(const IR::Instruction& inst) {
	const auto resource = inst.op == IR::Opcode::SLoadDword
	                          ? GetScalarAddressResource(inst.memory.resource)
	                          : GetBufferResource(inst.memory);
	return {resource, ReadU32(inst.src[0])};
}

bool Translator::TranslateScalarMemory(const IR::Instruction&          inst,
                                       const ScalarMemorySourceValues* source_snapshot) {
	switch (inst.op) {
		case IR::Opcode::LoadSrtDword: {
			const auto resource = ir.Emit(IR::ValueOpcode::GetSrtResource);
			WriteOperand(inst.dst,
			             ir.Emit(IR::ValueOpcode::ReadConst, {resource, ReadU32(inst.src[0])}));
			return true;
		}
		case IR::Opcode::SBufferLoadDword: {
			const auto source =
			    source_snapshot != nullptr ? *source_snapshot : ReadScalarMemorySource(inst);
			WriteOperand(inst.dst, ir.Emit(IR::ValueOpcode::ReadConstBuffer,
			                               {source.resource, source.offset}, AddMemoryInfo(inst)));
			return true;
		}
		case IR::Opcode::SLoadDword: {
			const auto source =
			    source_snapshot != nullptr ? *source_snapshot : ReadScalarMemorySource(inst);
			WriteOperand(inst.dst,
			             ir.Emit(IR::ValueOpcode::LoadAddressU32,
			                     {source.resource, source.offset, IR::Value(0u), IR::Value(true)},
			                     AddMemoryInfo(inst)));
			return true;
		}
		default: return false;
	}
}

bool Translator::TranslateBufferLoad(const IR::Instruction& inst) {
	IR::ValueOpcode opcode;
	uint32_t        bits;
	bool            sign;
	switch (inst.op) {
		case IR::Opcode::BufferLoadUbyte:
		case IR::Opcode::BufferLoadSbyte:
			opcode = IR::ValueOpcode::LoadBufferU8;
			bits   = 8u;
			sign   = inst.op == IR::Opcode::BufferLoadSbyte;
			break;
		case IR::Opcode::BufferLoadUshort:
		case IR::Opcode::BufferLoadSshort:
			opcode = IR::ValueOpcode::LoadBufferU16;
			bits   = 16u;
			sign   = inst.op == IR::Opcode::BufferLoadSshort;
			break;
		case IR::Opcode::BufferLoadDword:
			switch (inst.memory.data_dwords) {
				case 1u: opcode = IR::ValueOpcode::LoadBufferU32; break;
				case 2u: opcode = IR::ValueOpcode::LoadBufferU32x2; break;
				case 3u: opcode = IR::ValueOpcode::LoadBufferU32x3; break;
				case 4u: opcode = IR::ValueOpcode::LoadBufferU32x4; break;
				default: return false;
			}
			bits = 32u;
			sign = false;
			break;
		default: return false;
	}
	const auto resource = GetBufferResource(inst.memory);
	const auto address  = ReadBufferAddress(inst, 0);
	const auto loaded =
	    ir.Emit(opcode, {resource, address.index, address.offset, address.soffset, ir.GetExec()},
	            AddMemoryInfo(inst));
	if (bits != 32u) {
		WriteOperand(inst.dst, WidenSubdword(loaded, bits, sign));
	} else if (inst.memory.data_dwords == 1u) {
		WriteOperand(inst.dst, loaded);
	} else {
		for (uint32_t component = 0; component < inst.memory.data_dwords; component++) {
			WriteOperand(OffsetOperand(inst.dst, component),
			             ir.CompositeExtract(loaded, component));
		}
	}
	return true;
}

bool Translator::TranslateBufferStore(const IR::Instruction& inst) {
	const auto      resource = GetBufferResource(inst.memory);
	const auto      address  = ReadBufferAddress(inst, 1);
	const auto      data     = ReadU32(inst.src[0]);
	IR::ValueOpcode opcode;
	IR::Value       value;
	switch (inst.op) {
		case IR::Opcode::BufferStoreByte:
			opcode = IR::ValueOpcode::StoreBufferU8;
			value  = NarrowSubdword(data, 8u);
			break;
		case IR::Opcode::BufferStoreShort:
			opcode = IR::ValueOpcode::StoreBufferU16;
			value  = NarrowSubdword(data, 16u);
			break;
		case IR::Opcode::BufferStoreDword:
			switch (inst.memory.data_dwords) {
				case 1u:
					opcode = IR::ValueOpcode::StoreBufferU32;
					value  = data;
					break;
				case 2u:
					opcode = IR::ValueOpcode::StoreBufferU32x2;
					value  = ir.Emit(IR::ValueOpcode::CompositeConstructU32x2,
					                 {data, ReadU32(OffsetOperand(inst.src[0], 1u))});
					break;
				case 3u:
					opcode = IR::ValueOpcode::StoreBufferU32x3;
					value  = ir.Emit(IR::ValueOpcode::CompositeConstructU32x3,
					                 {data, ReadU32(OffsetOperand(inst.src[0], 1u)),
					                  ReadU32(OffsetOperand(inst.src[0], 2u))});
					break;
				case 4u:
					opcode = IR::ValueOpcode::StoreBufferU32x4;
					value  = ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,
					                 {data, ReadU32(OffsetOperand(inst.src[0], 1u)),
					                  ReadU32(OffsetOperand(inst.src[0], 2u)),
					                  ReadU32(OffsetOperand(inst.src[0], 3u))});
					break;
				default: return false;
			}
			break;
		default: return false;
	}
	ir.Emit(opcode, {resource, address.index, address.offset, address.soffset, value, ir.GetExec()},
	        AddMemoryInfo(inst));
	return true;
}

bool Translator::TranslateAtomicMemory(const IR::Instruction& inst) {
	IR::Value result;
	switch (inst.memory.kind) {
		case IR::ResourceKind::Buffer: {
			const auto resource = GetBufferResource(inst.memory);
			const auto address  = ReadBufferAddress(inst, 1);
			result              = ir.Emit(BufferAtomicOpcode(inst.op),
			                              {resource, address.index, address.offset, address.soffset,
			                               ReadU32(inst.src[0]), ir.GetExec()},
			                              AddMemoryInfo(inst));
			break;
		}
		case IR::ResourceKind::Image:
		case IR::ResourceKind::ImageUint:
		case IR::ResourceKind::StorageImage:
		case IR::ResourceKind::StorageImageUint: {
			const auto resource = GetImageResource(inst.memory);
			const auto address  = MakeImageAddress(inst, inst.src[1]);
			const auto flags    = AddMemoryInfo(inst);
			result = ir.Emit(ImageAtomicOpcode(inst.op),
			                 {resource, address, ReadU32(inst.src[0]), ir.GetExec()}, flags);
			break;
		}
		case IR::ResourceKind::Lds:
		case IR::ResourceKind::Gds: {
			const bool gds      = inst.memory.kind == IR::ResourceKind::Gds;
			const auto resource = GetSharedResource(inst.memory.kind);
			const auto address =
			    ir.IAdd(ReadU32(inst.src[1]), IR::U32(IR::Value(inst.memory.offset)));
			result = ir.Emit(SharedAtomicOpcode(inst.op, gds),
			                 {resource, address, ReadU32(inst.src[0]), ir.GetExec()},
			                 AddMemoryInfo(inst));
			break;
		}
		default: return false;
	}
	if (inst.dst.kind != IR::OperandKind::Null) {
		WriteOperand(inst.dst, result);
	}
	return true;
}

bool Translator::TranslateFlatLoad(const IR::Instruction& inst) {
	IR::ValueOpcode opcode;
	uint32_t        bits;
	bool            sign;
	switch (inst.op) {
		case IR::Opcode::FlatLoadUbyte:
		case IR::Opcode::FlatLoadSbyte:
			opcode = IR::ValueOpcode::LoadAddressU8;
			bits   = 8u;
			sign   = inst.op == IR::Opcode::FlatLoadSbyte;
			break;
		case IR::Opcode::FlatLoadUshort:
		case IR::Opcode::FlatLoadSshort:
			opcode = IR::ValueOpcode::LoadAddressU16;
			bits   = 16u;
			sign   = inst.op == IR::Opcode::FlatLoadSshort;
			break;
		case IR::Opcode::FlatLoadDword:
			opcode = IR::ValueOpcode::LoadAddressU32;
			bits   = 32u;
			sign   = false;
			break;
		default: return false;
	}
	const auto address = ReadAddressOperands(inst, 0);
	const auto active  = ir.GetExec();
	const auto count   = bits == 32u ? std::min(inst.memory.data_dwords, 4u) : 1u;
	for (uint32_t index = 0; index < count; index++) {
		auto component = inst;
		component.memory.offset += index * 4u;
		component.memory.data_dwords     = 1u;
		component.memory.component_index = index;
		const auto loaded = ir.Emit(opcode, {address.resource, address.low, address.high, active},
		                            AddMemoryInfo(component));
		WriteOperand(OffsetOperand(inst.dst, index),
		             bits == 32u ? loaded : WidenSubdword(loaded, bits, sign));
	}
	return true;
}

bool Translator::TranslateFlatStore(const IR::Instruction& inst) {
	const auto      data    = ReadU32(inst.src[0]);
	const auto      address = ReadAddressOperands(inst, 1);
	IR::ValueOpcode opcode;
	IR::Value       value;
	switch (inst.op) {
		case IR::Opcode::FlatStoreByte:
			opcode = IR::ValueOpcode::StoreAddressU8;
			value  = NarrowSubdword(data, 8u);
			break;
		case IR::Opcode::FlatStoreShort:
			opcode = IR::ValueOpcode::StoreAddressU16;
			value  = NarrowSubdword(data, 16u);
			break;
		case IR::Opcode::FlatStoreDword:
			opcode = IR::ValueOpcode::StoreAddressU32;
			value  = data;
			break;
		default: return false;
	}
	ir.Emit(opcode, {address.resource, address.low, address.high, value, ir.GetExec()},
	        AddMemoryInfo(inst));
	return true;
}

bool Translator::TranslateImageMemory(const IR::Instruction& inst) {
	const bool image = inst.memory.kind == IR::ResourceKind::Image ||
	                   inst.memory.kind == IR::ResourceKind::ImageUint ||
	                   inst.memory.kind == IR::ResourceKind::StorageImage ||
	                   inst.memory.kind == IR::ResourceKind::StorageImageUint;
	if (!image) {
		return false;
	}
	const auto resource = GetImageResource(inst.memory);
	const auto address =
	    MakeImageAddress(inst, inst.op == IR::Opcode::ImageStore ? inst.src[1] : inst.src[0]);
	const auto flags = AddMemoryInfo(inst);
	switch (inst.op) {
		case IR::Opcode::ImageGetResinfo: {
			const auto result =
			    ir.Emit(IR::ValueOpcode::ImageQueryDimensions, {resource, address}, flags);
			WriteImageComponents(inst.dst, result, inst.memory.dmask, 4u);
			return true;
		}
		case IR::Opcode::ImageGetLod: {
			const auto sampler = GetSamplerResource(inst.memory);
			const auto result =
			    ir.Emit(IR::ValueOpcode::ImageQueryLod, {resource, sampler, address}, flags);
			WriteImageComponents(inst.dst, result, inst.memory.dmask, 2u);
			return true;
		}
		case IR::Opcode::ImageLoad: {
			const auto result =
			    ir.Emit(IR::ValueOpcode::ImageRead, {resource, address, ir.GetExec()}, flags);
			WriteImageComponents(inst.dst, result, inst.memory.dmask, 4u);
			return true;
		}
		case IR::Opcode::ImageStore: {
			const auto data = ConstructU32x4(inst.src[0], inst.memory.data_dwords);
			ir.Emit(IR::ValueOpcode::ImageWrite, {resource, address, data, ir.GetExec()}, flags);
			return true;
		}
		case IR::Opcode::ImageSample:
		case IR::Opcode::ImageGather4: {
			const auto sampler = GetSamplerResource(inst.memory);
			const auto opcode  = inst.op == IR::Opcode::ImageSample
			                         ? IR::ValueOpcode::ImageSampleRaw
			                         : IR::ValueOpcode::ImageGatherRaw;
			const auto result  = ir.Emit(opcode, {resource, sampler, address}, flags);
			const bool dref =
			    (inst.memory.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0u;
			if (inst.op == IR::Opcode::ImageSample && dref) {
				const auto component =
				    ir.Emit(IR::ValueOpcode::CompositeExtractU32x4, {result, IR::Value(0u)});
				for (uint32_t index = 0; index < inst.memory.data_dwords; index++) {
					WriteOperand(OffsetOperand(inst.dst, index), component);
				}
			} else if (inst.op == IR::Opcode::ImageGather4) {
				for (uint32_t index = 0; index < inst.memory.data_dwords; index++) {
					WriteOperand(OffsetOperand(inst.dst, index),
					             ir.Emit(IR::ValueOpcode::CompositeExtractU32x4,
					                     {result, IR::Value(index)}));
				}
			} else {
				WriteImageComponents(inst.dst, result, inst.memory.dmask, 4u);
			}
			return true;
		}
		default: return false;
	}
}

bool Translator::TranslateSharedMemory(const IR::Instruction& inst) {
	const bool shared =
	    inst.memory.kind == IR::ResourceKind::Lds || inst.memory.kind == IR::ResourceKind::Gds;
	if (!shared && inst.op != IR::Opcode::DsSwizzleB32) {
		return false;
	}
	const bool gds            = inst.memory.kind == IR::ResourceKind::Gds;
	const auto resource       = shared ? GetSharedResource(inst.memory.kind) : IR::Value {};
	const auto shared_address = [&](uint32_t source) {
		return ir.IAdd(ReadU32(inst.src[source]), IR::U32(IR::Value(inst.memory.offset)));
	};
	switch (inst.op) {
		case IR::Opcode::DsReadUbyte:
		case IR::Opcode::DsReadSbyte:
		case IR::Opcode::DsReadUshort:
		case IR::Opcode::DsReadSshort:
		case IR::Opcode::DsReadB32: {
			IR::ValueOpcode opcode;
			uint32_t        bits;
			bool            sign;
			if (inst.op == IR::Opcode::DsReadUbyte || inst.op == IR::Opcode::DsReadSbyte) {
				opcode = gds ? IR::ValueOpcode::LoadGdsU8 : IR::ValueOpcode::LoadSharedU8;
				bits   = 8u;
				sign   = inst.op == IR::Opcode::DsReadSbyte;
			} else if (inst.op == IR::Opcode::DsReadUshort || inst.op == IR::Opcode::DsReadSshort) {
				opcode = gds ? IR::ValueOpcode::LoadGdsU16 : IR::ValueOpcode::LoadSharedU16;
				bits   = 16u;
				sign   = inst.op == IR::Opcode::DsReadSshort;
			} else {
				opcode = gds ? IR::ValueOpcode::LoadGdsU32 : IR::ValueOpcode::LoadSharedU32;
				bits   = 32u;
				sign   = false;
			}
			const auto loaded =
			    ir.Emit(opcode, {resource, shared_address(0), ir.GetExec()}, AddMemoryInfo(inst));
			WriteOperand(inst.dst, bits == 32u ? loaded : WidenSubdword(loaded, bits, sign));
			return true;
		}
		case IR::Opcode::DsWriteByte:
		case IR::Opcode::DsWriteShort:
		case IR::Opcode::DsWriteB32: {
			const auto      data    = ReadU32(inst.src[0]);
			const auto      address = shared_address(1);
			IR::ValueOpcode opcode;
			IR::Value       value;
			if (inst.op == IR::Opcode::DsWriteByte) {
				opcode = gds ? IR::ValueOpcode::WriteGdsU8 : IR::ValueOpcode::WriteSharedU8;
				value  = NarrowSubdword(data, 8u);
			} else if (inst.op == IR::Opcode::DsWriteShort) {
				opcode = gds ? IR::ValueOpcode::WriteGdsU16 : IR::ValueOpcode::WriteSharedU16;
				value  = NarrowSubdword(data, 16u);
			} else {
				opcode = gds ? IR::ValueOpcode::WriteGdsU32 : IR::ValueOpcode::WriteSharedU32;
				value  = data;
			}
			ir.Emit(opcode, {resource, address, value, ir.GetExec()}, AddMemoryInfo(inst));
			return true;
		}
		case IR::Opcode::DsMinF32:
		case IR::Opcode::DsMaxF32: {
			const auto opcode =
			    gds ? (inst.op == IR::Opcode::DsMinF32 ? IR::ValueOpcode::GdsAtomicFMin32
			                                           : IR::ValueOpcode::GdsAtomicFMax32)
			        : (inst.op == IR::Opcode::DsMinF32 ? IR::ValueOpcode::SharedAtomicFMin32
			                                           : IR::ValueOpcode::SharedAtomicFMax32);
			ir.Emit(opcode,
			        {resource, shared_address(1), ReadU32(inst.src[0]), ReadU32(inst.src[2]),
			         ir.GetExec()},
			        AddMemoryInfo(inst));
			return true;
		}
		case IR::Opcode::DsAppend:
		case IR::Opcode::DsConsume: {
			const bool append = inst.op == IR::Opcode::DsAppend;
			const auto opcode =
			    gds ? (append ? IR::ValueOpcode::GdsDataAppend : IR::ValueOpcode::GdsDataConsume)
			        : (append ? IR::ValueOpcode::DataAppend : IR::ValueOpcode::DataConsume);
			WriteOperand(inst.dst, ir.Emit(opcode,
			                               {resource, ReadU32(inst.src[0]), ir.GetExec(),
			                                ir.GetExecLo(), ir.GetExecHi()},
			                               AddMemoryInfo(inst)));
			return true;
		}
		case IR::Opcode::DsWriteAddtidB32:
		case IR::Opcode::DsReadAddtidB32: {
			const auto m0_index = inst.op == IR::Opcode::DsWriteAddtidB32 ? 1u : 0u;
			const auto base =
			    ir.BitwiseAnd(ReadU32(inst.src[m0_index]), IR::U32(IR::Value(0xffffu)));
			const auto lane    = IR::U32(ir.Emit(IR::ValueOpcode::LaneId));
			const auto address = ir.IAdd(ir.IAdd(base, IR::U32(IR::Value(inst.memory.offset))),
			                             ir.ShiftLeftLogical(lane, IR::U32(IR::Value(2u))));
			if (inst.op == IR::Opcode::DsWriteAddtidB32) {
				ir.Emit(IR::ValueOpcode::WriteSharedU32,
				        {resource, address, ReadU32(inst.src[0]), ir.GetExec()},
				        AddMemoryInfo(inst));
			} else {
				WriteOperand(inst.dst,
				             ir.Emit(IR::ValueOpcode::LoadSharedU32,
				                     {resource, address, ir.GetExec()}, AddMemoryInfo(inst)));
			}
			return true;
		}
		case IR::Opcode::DsSwizzleB32:
			WriteOperand(inst.dst,
			             ir.Emit(IR::ValueOpcode::SwizzleU32,
			                     {ReadU32(inst.src[0]), ReadU32(inst.src[1]), ir.GetExec()}));
			return true;
		default: return false;
	}
}

bool Translator::TranslateMemoryOperation(const IR::Instruction&          inst,
                                          const ScalarMemorySourceValues* scalar_source_snapshot) {
	switch (inst.op) {
		case IR::Opcode::LoadSrtDword:
		case IR::Opcode::SLoadDword:
		case IR::Opcode::SBufferLoadDword:
			return TranslateScalarMemory(inst, scalar_source_snapshot);

		case IR::Opcode::BufferLoadUbyte:
		case IR::Opcode::BufferLoadSbyte:
		case IR::Opcode::BufferLoadUshort:
		case IR::Opcode::BufferLoadSshort:
		case IR::Opcode::BufferLoadDword: return TranslateBufferLoad(inst);

		case IR::Opcode::BufferStoreByte:
		case IR::Opcode::BufferStoreShort:
		case IR::Opcode::BufferStoreDword: return TranslateBufferStore(inst);

		case IR::Opcode::AtomicSwapU32:
		case IR::Opcode::AtomicAddU32:
		case IR::Opcode::AtomicSubU32:
		case IR::Opcode::AtomicSMinI32:
		case IR::Opcode::AtomicUMinU32:
		case IR::Opcode::AtomicSMaxI32:
		case IR::Opcode::AtomicUMaxU32:
		case IR::Opcode::AtomicAndU32:
		case IR::Opcode::AtomicOrU32:
		case IR::Opcode::AtomicXorU32:
		case IR::Opcode::AtomicFMinF32:
		case IR::Opcode::AtomicFMaxF32: return TranslateAtomicMemory(inst);

		case IR::Opcode::FlatLoadUbyte:
		case IR::Opcode::FlatLoadSbyte:
		case IR::Opcode::FlatLoadUshort:
		case IR::Opcode::FlatLoadSshort:
		case IR::Opcode::FlatLoadDword: return TranslateFlatLoad(inst);

		case IR::Opcode::FlatStoreByte:
		case IR::Opcode::FlatStoreShort:
		case IR::Opcode::FlatStoreDword: return TranslateFlatStore(inst);

		case IR::Opcode::ImageGetResinfo:
		case IR::Opcode::ImageGetLod:
		case IR::Opcode::ImageLoad:
		case IR::Opcode::ImageStore:
		case IR::Opcode::ImageSample:
		case IR::Opcode::ImageGather4: return TranslateImageMemory(inst);

		case IR::Opcode::DsReadUbyte:
		case IR::Opcode::DsReadSbyte:
		case IR::Opcode::DsReadUshort:
		case IR::Opcode::DsReadSshort:
		case IR::Opcode::DsReadB32:
		case IR::Opcode::DsWriteByte:
		case IR::Opcode::DsWriteShort:
		case IR::Opcode::DsWriteB32:
		case IR::Opcode::DsMinF32:
		case IR::Opcode::DsMaxF32:
		case IR::Opcode::DsSwizzleB32:
		case IR::Opcode::DsConsume:
		case IR::Opcode::DsAppend:
		case IR::Opcode::DsWriteAddtidB32:
		case IR::Opcode::DsReadAddtidB32: return TranslateSharedMemory(inst);
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend::Detail
