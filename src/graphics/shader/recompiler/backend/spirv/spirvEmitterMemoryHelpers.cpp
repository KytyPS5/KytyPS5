#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t EmitShaderDataDwordLoad(EmitterState& state, uint32_t dword_index) {
	if (state.push_constant_variable != 0) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain, TypePushConstantElementPointer(state), pointer,
		                           state.push_constant_variable, ConstantU32(state, 0),
		                           ConstantU32(state, dword_index)});
		state.builder.AddFunction({OpLoad, TypeU32(state), value, pointer});
		return value;
	}
	if (state.vsharp_storage_variable != 0) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
		                           state.vsharp_storage_variable, ConstantU32(state, 0),
		                           ConstantU32(state, dword_index)});
		state.builder.AddFunction({OpLoad, TypeU32(state), value, pointer});
		return value;
	}
	return ConstantU32(state, 0);
}

uint32_t EmitAddU32(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpIAdd, TypeU32(state), ret, lhs, rhs});
	return ret;
}

uint32_t EmitBinaryU32(EmitterState& state, uint32_t opcode, uint32_t lhs, uint32_t rhs) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({opcode, TypeU32(state), ret, lhs, rhs});
	return ret;
}

uint32_t StorageBufferPackedStride(const EmitterState& state, const IR::MemoryInfo& mem) {
	if (mem.resource >= state.program.info.buffers.size()) {
		ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "buffer specialization is missing");
	}
	return state.program.info.buffers[mem.resource].packed_stride;
}

Prospero::BufferFormat StorageBufferFormat(const EmitterState& state, const IR::MemoryInfo& mem) {
	if (mem.resource >= state.program.info.buffers.size()) {
		ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "buffer specialization is missing");
	}
	return state.program.info.buffers[mem.resource].descriptor_format;
}

void EmitStorageBufferOffsets(EmitterState& state) {
	for (uint32_t i = 0; i < state.program.bindings.buffer_offset_count; i++) {
		const auto word =
		    EmitShaderDataDwordLoad(state, state.program.bindings.buffer_offset_dword + i / 4u);
		const auto shift                = ConstantU32(state, (i % 4u) * 8u + 2u);
		state.storage_buffer_offsets[i] = EmitBinaryU32(
		    state, OpBitwiseAnd, EmitBinaryU32(state, OpShiftRightLogical, word, shift),
		    ConstantU32(state, 0x3fu));
	}
}

uint32_t LdsDwordCount(const EmitterState& state) {
	return state.stage == ShaderType::Compute ? state.input_info.compute->lds_size_dwords : 8192u;
}

static void EnsureLdsStorage(EmitterState& state) {
	if (state.lds_variable != 0) {
		return;
	}
	if (state.stage != ShaderType::Compute) {
		EXIT("function LDS was not prepared before SPIR-V function emission\n");
	}
	state.lds_variable = state.builder.DefineGlobalVariable(
	    TypeLdsArrayPointer(state, StorageClassWorkgroup), StorageClassWorkgroup);
	state.builder.AddName(state.lds_variable, "lds_dwords");
}

MemoryResourceAccess PrepareMemoryResourceAccess(EmitterState& state, const IR::MemoryInfo& mem) {
	MemoryResourceAccess access {.kind = mem.kind};
	switch (mem.kind) {
		case IR::ResourceKind::Lds:
			EnsureLdsStorage(state);
			access.object_pointer = state.lds_variable;
			access.length         = ConstantU32(state, LdsDwordCount(state));
			return access;
		case IR::ResourceKind::Gds:
			if (state.gds_variable == 0) {
				ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Gds, mem.resource,
				                             "GDS binding was not emitted");
			}
			access.object_pointer = state.gds_variable;
			break;
		case IR::ResourceKind::ScalarAddress:
		case IR::ResourceKind::Flat:
		case IR::ResourceKind::Global:
		case IR::ResourceKind::Scratch: {
			if (state.address_memory_variable == 0) {
				ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::AddressMemory,
				                             mem.resource,
				                             "address memory binding was not emitted");
			}
			const auto binding = ResourceForDescriptor(
			    state, IR::DescriptorBindingKind::AddressMemory, mem.resource);
			access.object_pointer = state.builder.AllocateId();
			state.builder.AddFunction({OpAccessChain, TypeStorageBufferPointer(state),
			                           access.object_pointer, state.address_memory_variable,
			                           ConstantU32(state, binding.array_index)});
			break;
		}
		case IR::ResourceKind::ScalarBuffer:
		case IR::ResourceKind::Buffer: {
			if (state.storage_buffer_variable == 0) {
				ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers,
				                             mem.resource,
				                             "storage buffer descriptor array was not emitted");
			}
			const auto binding =
			    ResourceForDescriptor(state, IR::DescriptorBindingKind::Buffers, mem.resource);
			access.object_pointer = state.builder.AllocateId();
			state.builder.AddFunction({OpAccessChain, TypeStorageBufferPointer(state),
			                           access.object_pointer, state.storage_buffer_variable,
			                           ConstantU32(state, binding.array_index)});
			access.index_offset     = state.storage_buffer_offsets[binding.array_index];
			access.add_index_offset = true;
			break;
		}
		default: EXIT("unsupported memory resource kind: %u\n", static_cast<unsigned>(mem.kind));
	}
	access.length = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpArrayLength, TypeU32(state), access.length, access.object_pointer, 0});
	return access;
}

uint32_t EmitMemoryElementIndex(EmitterState& state, const MemoryResourceAccess& access,
                                uint32_t raw_index) {
	return access.add_index_offset ? EmitAddU32(state, raw_index, access.index_offset) : raw_index;
}

uint32_t EmitMemoryElementInBounds(EmitterState& state, const MemoryResourceAccess& access,
                                   uint32_t index) {
	const auto in_bounds = state.builder.AllocateId();
	state.builder.AddFunction({OpULessThan, TypeBool(state), in_bounds, index, access.length});
	return in_bounds;
}

uint32_t EmitMemoryElementPointer(EmitterState& state, const MemoryResourceAccess& access,
                                  uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	if (access.kind == IR::ResourceKind::Lds) {
		state.builder.AddFunction({OpAccessChain,
		                           TypeLdsElementPointer(state, state.stage == ShaderType::Compute
		                                                            ? StorageClassWorkgroup
		                                                            : StorageClassFunction),
		                           pointer, access.object_pointer, index});
	} else {
		state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
		                           access.object_pointer, ConstantU32(state, 0), index});
	}
	return pointer;
}

uint32_t EmitTBufferBitcastF32ToU32(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, TypeU32(state), ret, value});
	return ret;
}

uint32_t EmitTBufferBitcastU32ToF32(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, TypeF32(state), ret, value});
	return ret;
}

uint32_t EmitTBufferBitcastU32ToI32(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, TypeI32(state), ret, value});
	return ret;
}

uint32_t EmitTBufferCompareU32Constant(EmitterState& state, uint32_t opcode, uint32_t value,
                                       uint32_t constant) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({opcode, TypeBool(state), ret, value, ConstantU32(state, constant)});
	return ret;
}

uint32_t EmitTBufferSelectF32(EmitterState& state, uint32_t condition, uint32_t true_value,
                              uint32_t false_value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, TypeF32(state), ret, condition, true_value, false_value});
	return ret;
}

bool IsSignedFormatComponent(Format::ComponentType type) {
	return type == Format::ComponentType::Sint || type == Format::ComponentType::Snorm ||
	       type == Format::ComponentType::Sscaled;
}

uint32_t EmitHalfToF32Bits(EmitterState& state, uint32_t raw) {
	const auto unpacked = state.builder.AllocateId();
	const auto value    = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpExtInst, TypeF32Vector(state, 2), unpacked, GlslStd450(state), GlslUnpackHalf2x16, raw});
	state.builder.AddFunction({OpCompositeExtract, TypeF32(state), value, unpacked, 0});
	return EmitTBufferBitcastF32ToU32(state, value);
}

uint32_t EmitUFloatToF32Bits(EmitterState& state, uint32_t raw, uint32_t bits) {
	const auto mantissa_bits = bits == 11u ? 6u : 5u;
	const auto mantissa_mask = (1u << mantissa_bits) - 1u;
	const auto mantissa =
	    EmitBinaryU32(state, OpBitwiseAnd, raw, ConstantU32(state, mantissa_mask));
	const auto exponent = EmitBinaryU32(
	    state, OpBitwiseAnd,
	    EmitBinaryU32(state, OpShiftRightLogical, raw, ConstantU32(state, mantissa_bits)),
	    ConstantU32(state, 0x1fu));

	const auto exponent_32 = EmitAddU32(state, exponent, ConstantU32(state, 127u - 15u));
	const auto exponent_bits =
	    EmitBinaryU32(state, OpShiftLeftLogical, exponent_32, ConstantU32(state, 23));
	const auto mantissa_bits_32 =
	    EmitBinaryU32(state, OpShiftLeftLogical, mantissa, ConstantU32(state, 23u - mantissa_bits));
	const auto normal_bits = EmitBinaryU32(state, OpBitwiseOr, exponent_bits, mantissa_bits_32);
	const auto normal      = EmitTBufferBitcastU32ToF32(state, normal_bits);

	const auto special_bits =
	    EmitBinaryU32(state, OpBitwiseOr, ConstantU32(state, 0x7f800000u), mantissa_bits_32);
	const auto special = EmitTBufferBitcastU32ToF32(state, special_bits);

	const auto mantissa_f32 = state.builder.AllocateId();
	const auto subnormal    = state.builder.AllocateId();
	state.builder.AddFunction({OpConvertUToF, TypeF32(state), mantissa_f32, mantissa});
	state.builder.AddFunction(
	    {OpFMul, TypeF32(state), subnormal, mantissa_f32,
	     ConstantF32Value(state, std::ldexp(1.0f, 1 - 15 - static_cast<int>(mantissa_bits)))});

	const auto zero_exp    = EmitTBufferCompareU32Constant(state, OpIEqual, exponent, 0);
	const auto special_exp = EmitTBufferCompareU32Constant(state, OpIEqual, exponent, 31);
	const auto finite      = EmitTBufferSelectF32(state, zero_exp, subnormal, normal);
	const auto result      = EmitTBufferSelectF32(state, special_exp, special, finite);
	return EmitTBufferBitcastF32ToU32(state, result);
}

uint32_t NormalizeFormatComponent(EmitterState& state, const Format::BufferFormatInfo& info,
                                  uint32_t component, uint32_t raw) {
	const auto bits = info.component_bits[component];
	switch (info.type) {
		case Format::ComponentType::Uint:
		case Format::ComponentType::Sint: return raw;
		case Format::ComponentType::Uscaled: {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction({OpConvertUToF, TypeF32(state), value, raw});
			return EmitTBufferBitcastF32ToU32(state, value);
		}
		case Format::ComponentType::Sscaled: {
			const auto signed_raw = EmitTBufferBitcastU32ToI32(state, raw);
			const auto value      = state.builder.AllocateId();
			state.builder.AddFunction({OpConvertSToF, TypeF32(state), value, signed_raw});
			return EmitTBufferBitcastF32ToU32(state, value);
		}
		case Format::ComponentType::Unorm: {
			const auto value      = state.builder.AllocateId();
			const auto normalized = state.builder.AllocateId();
			const auto max_value  = static_cast<float>((1u << bits) - 1u);
			state.builder.AddFunction({OpConvertUToF, TypeF32(state), value, raw});
			state.builder.AddFunction(
			    {OpFDiv, TypeF32(state), normalized, value, ConstantF32Value(state, max_value)});
			return EmitTBufferBitcastF32ToU32(state, normalized);
		}
		case Format::ComponentType::Snorm: {
			const auto signed_raw = EmitTBufferBitcastU32ToI32(state, raw);
			const auto value      = state.builder.AllocateId();
			const auto normalized = state.builder.AllocateId();
			const auto clamped    = state.builder.AllocateId();
			const auto max_value  = static_cast<float>((1u << (bits - 1u)) - 1u);
			state.builder.AddFunction({OpConvertSToF, TypeF32(state), value, signed_raw});
			state.builder.AddFunction(
			    {OpFDiv, TypeF32(state), normalized, value, ConstantF32Value(state, max_value)});
			state.builder.AddFunction({OpExtInst, TypeF32(state), clamped, GlslStd450(state),
			                           GlslFMax, normalized, ConstantF32Value(state, -1.0f)});
			return EmitTBufferBitcastF32ToU32(state, clamped);
		}
		case Format::ComponentType::Float:
			if (bits == 32u) {
				return raw;
			}
			if (bits == 16u) {
				return EmitHalfToF32Bits(state, raw);
			}
			return EmitUFloatToF32Bits(state, raw, bits);
		default: return raw;
	}
}

void EmitDeviceAtomicMemoryBarrier(EmitterState& state) {
	const auto semantics = MemorySemanticsAcquireRelease | MemorySemanticsUniformMemory;
	state.builder.AddFunction(
	    {OpMemoryBarrier, ConstantU32(state, ScopeDevice), ConstantU32(state, semantics)});
}

uint32_t EmitDsSwizzleTargetLane(EmitterState& state, uint32_t subid, uint32_t control) {
	if ((control & 0xc000u) == 0xc000u) {
		const uint32_t mask         = control & 0x1fu;
		const uint32_t rotate       = (control >> 5u) & 0x1fu;
		const uint32_t rotate_delta = (control & 0x400u) != 0u ? ((32u - rotate) & 0x1fu) : rotate;
		const auto     lane         = EmitAndConstant(state, subid, 31);
		const auto     rotated_sum  = EmitAddU32(state, lane, ConstantU32(state, rotate_delta));
		const auto     rotated      = EmitAndConstant(state, rotated_sum, 31);
		const auto     kept         = EmitAndConstant(state, lane, mask);
		const auto     moved        = EmitAndConstant(state, rotated, (~mask) & 31u);
		const auto     combined     = EmitOrU32(state, kept, moved);
		const auto     base         = EmitAndConstant(state, subid, 0xffffffe0u);
		return EmitOrU32(state, base, combined);
	}

	if ((control & 0x8000u) != 0) {
		const auto lane2  = state.builder.AllocateId();
		const auto shift  = state.builder.AllocateId();
		const auto perm0  = state.builder.AllocateId();
		const auto perm   = state.builder.AllocateId();
		const auto base   = state.builder.AllocateId();
		const auto target = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpBitwiseAnd, TypeU32(state), lane2, subid, ConstantU32(state, 3)});
		state.builder.AddFunction(
		    {OpShiftLeftLogical, TypeU32(state), shift, lane2, ConstantU32(state, 1)});
		state.builder.AddFunction(
		    {OpShiftRightLogical, TypeU32(state), perm0, ConstantU32(state, control), shift});
		state.builder.AddFunction(
		    {OpBitwiseAnd, TypeU32(state), perm, perm0, ConstantU32(state, 3)});
		state.builder.AddFunction(
		    {OpBitwiseAnd, TypeU32(state), base, subid, ConstantU32(state, 0xfffffffcu)});
		state.builder.AddFunction({OpBitwiseOr, TypeU32(state), target, base, perm});
		return target;
	}

	const auto lane   = state.builder.AllocateId();
	const auto masked = state.builder.AllocateId();
	const auto ored   = state.builder.AllocateId();
	const auto xored  = state.builder.AllocateId();
	const auto base   = state.builder.AllocateId();
	const auto target = state.builder.AllocateId();
	state.builder.AddFunction({OpBitwiseAnd, TypeU32(state), lane, subid, ConstantU32(state, 31)});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), masked, lane, ConstantU32(state, control & 0x1fu)});
	state.builder.AddFunction(
	    {OpBitwiseOr, TypeU32(state), ored, masked, ConstantU32(state, (control >> 5u) & 0x1fu)});
	state.builder.AddFunction(
	    {OpBitwiseXor, TypeU32(state), xored, ored, ConstantU32(state, (control >> 10u) & 0x1fu)});
	state.builder.AddFunction(
	    {OpBitwiseAnd, TypeU32(state), base, subid, ConstantU32(state, 0xffffffe0u)});
	state.builder.AddFunction({OpBitwiseOr, TypeU32(state), target, base, xored});
	return target;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
