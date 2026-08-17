#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t EmitAddU32(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpIAdd, state.uint_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitBinaryU32(EmitterState& state, uint32_t opcode, uint32_t lhs, uint32_t rhs) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({opcode, state.uint_type, ret, lhs, rhs});
	return ret;
}

uint32_t StorageBufferPackedStride(const EmitterState& state, const IR::MemoryInfo& mem,
                                   uint32_t use_pc) {
	(void)use_pc;
	if (mem.resource >= state.program.info.buffers.size()) {
		ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "buffer specialization is missing");
	}
	return state.program.info.buffers[mem.resource].packed_stride;
}

Prospero::BufferFormat StorageBufferFormat(const EmitterState& state, const IR::MemoryInfo& mem,
                                           uint32_t use_pc) {
	(void)use_pc;
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

bool IsAddressMemoryKind(IR::ResourceKind kind) {
	return kind == IR::ResourceKind::ScalarAddress || kind == IR::ResourceKind::Flat ||
	       kind == IR::ResourceKind::Global || kind == IR::ResourceKind::Scratch;
}

DescriptorResourceBinding
StorageBufferBindingForMemory(EmitterState& state, const IR::MemoryInfo& mem, uint32_t use_pc) {
	(void)use_pc;
	const auto binding =
	    ResourceForDescriptor(state, IR::DescriptorBindingKind::Buffers, mem.resource);
	if (state.storage_buffer_variable == 0) {
		ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "storage buffer descriptor array was not emitted");
	}
	return binding;
}

uint32_t EmitStorageBufferIndex(EmitterState& state, const IR::MemoryInfo& mem, uint32_t index,
                                uint32_t use_pc) {
	if (IsAddressMemoryKind(mem.kind)) {
		return index;
	}
	const auto binding = StorageBufferBindingForMemory(state, mem, use_pc);
	return EmitAddU32(state, index, state.storage_buffer_offsets[binding.array_index]);
}

uint32_t EmitStorageBufferObjectPointer(EmitterState& state, const IR::MemoryInfo& mem,
                                        uint32_t use_pc) {
	if (IsAddressMemoryKind(mem.kind)) {
		if (state.address_memory_variable == 0) {
			ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::AddressMemory,
			                             mem.resource, "address memory binding was not emitted");
		}
		const auto binding =
		    ResourceForDescriptor(state, IR::DescriptorBindingKind::AddressMemory, mem.resource);
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain, state.ptr_storage_buffer, pointer,
		                           state.address_memory_variable,
		                           ConstantU32(state, binding.array_index)});
		return pointer;
	}
	const auto binding = StorageBufferBindingForMemory(state, mem, use_pc);
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, state.ptr_storage_buffer, pointer,
	                           state.storage_buffer_variable,
	                           ConstantU32(state, binding.array_index)});
	return pointer;
}

uint32_t EmitStorageBufferElementInBounds(EmitterState& state, const IR::MemoryInfo& mem,
                                          uint32_t index, uint32_t use_pc) {
	index                = EmitStorageBufferIndex(state, mem, index, use_pc);
	const auto object    = EmitStorageBufferObjectPointer(state, mem, use_pc);
	const auto length    = state.builder.AllocateId();
	const auto in_bounds = state.builder.AllocateId();
	state.builder.AddFunction({OpArrayLength, state.uint_type, length, object, 0});
	state.builder.AddFunction({OpULessThan, state.bool_type, in_bounds, index, length});
	return in_bounds;
}

uint32_t EmitStorageBufferElementPointer(EmitterState& state, const IR::MemoryInfo& mem,
                                         uint32_t index, uint32_t use_pc) {
	index = EmitStorageBufferIndex(state, mem, index, use_pc);
	if (IsAddressMemoryKind(mem.kind)) {
		if (state.address_memory_variable == 0) {
			ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::AddressMemory,
			                             mem.resource, "address memory binding was not emitted");
		}
		const auto binding =
		    ResourceForDescriptor(state, IR::DescriptorBindingKind::AddressMemory, mem.resource);
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpAccessChain, state.ptr_storage_buffer_uint, pointer, state.address_memory_variable,
		     ConstantU32(state, binding.array_index), ConstantU32(state, 0), index});
		return pointer;
	}
	const auto binding = StorageBufferBindingForMemory(state, mem, use_pc);
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpAccessChain, state.ptr_storage_buffer_uint, pointer, state.storage_buffer_variable,
	     ConstantU32(state, binding.array_index), ConstantU32(state, 0), index});
	return pointer;
}

uint32_t EmitLdsElementPointer(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpAccessChain, state.ptr_workgroup_uint, pointer, state.lds_variable, index});
	return pointer;
}

uint32_t LdsDwordCount(const EmitterState& state) {
	return state.stage == ShaderType::Compute ? state.input_info.compute->lds_size_dwords : 8192u;
}

uint32_t EmitLdsElementInBounds(EmitterState& state, uint32_t index) {
	const auto in_bounds = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpULessThan, state.bool_type, in_bounds, index, ConstantU32(state, LdsDwordCount(state))});
	return in_bounds;
}

uint32_t EmitMemoryElementPointer(EmitterState& state, const IR::MemoryInfo& mem, uint32_t index,
                                  uint32_t use_pc) {
	if (mem.kind == IR::ResourceKind::Lds) {
		return EmitLdsElementPointer(state, index);
	}
	if (mem.kind == IR::ResourceKind::Gds) {
		return EmitGdsElementPointer(state, index);
	}
	return EmitStorageBufferElementPointer(state, mem, index, use_pc);
}

uint32_t EmitTBufferBitcastF32ToU32(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, state.uint_type, ret, value});
	return ret;
}

uint32_t EmitTBufferBitcastU32ToF32(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, state.float_type, ret, value});
	return ret;
}

uint32_t EmitTBufferBitcastU32ToI32(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, state.int_type, ret, value});
	return ret;
}

uint32_t EmitTBufferCompareU32Constant(EmitterState& state, uint32_t opcode, uint32_t value,
                                       uint32_t constant) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction({opcode, state.bool_type, ret, value, ConstantU32(state, constant)});
	return ret;
}

uint32_t EmitTBufferSelectF32(EmitterState& state, uint32_t condition, uint32_t true_value,
                              uint32_t false_value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpSelect, state.float_type, ret, condition, true_value, false_value});
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
	    {OpExtInst, state.vec2_float_type, unpacked, state.glsl_std450, GlslUnpackHalf2x16, raw});
	state.builder.AddFunction({OpCompositeExtract, state.float_type, value, unpacked, 0});
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
	state.builder.AddFunction({OpConvertUToF, state.float_type, mantissa_f32, mantissa});
	state.builder.AddFunction(
	    {OpFMul, state.float_type, subnormal, mantissa_f32,
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
			state.builder.AddFunction({OpConvertUToF, state.float_type, value, raw});
			return EmitTBufferBitcastF32ToU32(state, value);
		}
		case Format::ComponentType::Sscaled: {
			const auto signed_raw = EmitTBufferBitcastU32ToI32(state, raw);
			const auto value      = state.builder.AllocateId();
			state.builder.AddFunction({OpConvertSToF, state.float_type, value, signed_raw});
			return EmitTBufferBitcastF32ToU32(state, value);
		}
		case Format::ComponentType::Unorm: {
			const auto value      = state.builder.AllocateId();
			const auto normalized = state.builder.AllocateId();
			const auto max_value  = static_cast<float>((1u << bits) - 1u);
			state.builder.AddFunction({OpConvertUToF, state.float_type, value, raw});
			state.builder.AddFunction(
			    {OpFDiv, state.float_type, normalized, value, ConstantF32Value(state, max_value)});
			return EmitTBufferBitcastF32ToU32(state, normalized);
		}
		case Format::ComponentType::Snorm: {
			const auto signed_raw = EmitTBufferBitcastU32ToI32(state, raw);
			const auto value      = state.builder.AllocateId();
			const auto normalized = state.builder.AllocateId();
			const auto clamped    = state.builder.AllocateId();
			const auto max_value  = static_cast<float>((1u << (bits - 1u)) - 1u);
			state.builder.AddFunction({OpConvertSToF, state.float_type, value, signed_raw});
			state.builder.AddFunction(
			    {OpFDiv, state.float_type, normalized, value, ConstantF32Value(state, max_value)});
			state.builder.AddFunction({OpExtInst, state.float_type, clamped, state.glsl_std450,
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

uint32_t EmitGdsElementInBounds(EmitterState& state, uint32_t index) {
	const auto length    = state.builder.AllocateId();
	const auto in_bounds = state.builder.AllocateId();
	state.builder.AddFunction({OpArrayLength, state.uint_type, length, state.gds_variable, 0});
	state.builder.AddFunction({OpULessThan, state.bool_type, in_bounds, index, length});
	return in_bounds;
}

uint32_t EmitGdsElementPointer(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, state.ptr_storage_buffer_uint, pointer,
	                           state.gds_variable, ConstantU32(state, 0), index});
	return pointer;
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
		    {OpBitwiseAnd, state.uint_type, lane2, subid, ConstantU32(state, 3)});
		state.builder.AddFunction(
		    {OpShiftLeftLogical, state.uint_type, shift, lane2, ConstantU32(state, 1)});
		state.builder.AddFunction(
		    {OpShiftRightLogical, state.uint_type, perm0, ConstantU32(state, control), shift});
		state.builder.AddFunction(
		    {OpBitwiseAnd, state.uint_type, perm, perm0, ConstantU32(state, 3)});
		state.builder.AddFunction(
		    {OpBitwiseAnd, state.uint_type, base, subid, ConstantU32(state, 0xfffffffcu)});
		state.builder.AddFunction({OpBitwiseOr, state.uint_type, target, base, perm});
		return target;
	}

	const auto lane   = state.builder.AllocateId();
	const auto masked = state.builder.AllocateId();
	const auto ored   = state.builder.AllocateId();
	const auto xored  = state.builder.AllocateId();
	const auto base   = state.builder.AllocateId();
	const auto target = state.builder.AllocateId();
	state.builder.AddFunction({OpBitwiseAnd, state.uint_type, lane, subid, ConstantU32(state, 31)});
	state.builder.AddFunction(
	    {OpBitwiseAnd, state.uint_type, masked, lane, ConstantU32(state, control & 0x1fu)});
	state.builder.AddFunction(
	    {OpBitwiseOr, state.uint_type, ored, masked, ConstantU32(state, (control >> 5u) & 0x1fu)});
	state.builder.AddFunction(
	    {OpBitwiseXor, state.uint_type, xored, ored, ConstantU32(state, (control >> 10u) & 0x1fu)});
	state.builder.AddFunction(
	    {OpBitwiseAnd, state.uint_type, base, subid, ConstantU32(state, 0xffffffe0u)});
	state.builder.AddFunction({OpBitwiseOr, state.uint_type, target, base, xored});
	return target;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
