#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <cstring>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

bool CoherentBufferAccess(const IR::MemoryInfo& mem) {
	// Not mem.glc: the frontend already dropped the bit on atomics, where it has no cache meaning.
	return mem.cache_bypass;
}

uint32_t EmitShaderDataDwordLoad(EmitterState& state, uint32_t dword_index) {
	if (state.program.bindings.UsesPushData()) {
		dword_index += state.program.bindings.push_data_start_dword;
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(state),
		                          pointer, state.push_constant_variable, ConstantU32(state, 0),
		                          ConstantU32(state, dword_index));
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
		return value;
	}
	if (state.shader_data_storage_variable != 0) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state),
		                          pointer, state.shader_data_storage_variable,
		                          ConstantU32(state, 0), ConstantU32(state, dword_index));
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
		return value;
	}
	return ConstantU32(state, 0);
}

uint32_t EmitShaderDataDwordLoadDynamic(EmitterState& state, uint32_t dword_index) {
	const auto Load = [&](uint32_t variable, uint32_t pointer_type, uint32_t index) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, pointer_type, pointer, variable,
		                          ConstantU32(state, 0), index);
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
		return value;
	};
	if (state.program.bindings.UsesPushData()) {
		return Load(state.push_constant_variable, TypePushConstantElementPointer(state),
		            EmitBinaryU32(state, spv::OpIAdd, dword_index,
		                          ConstantU32(state, state.program.bindings.push_data_start_dword)));
	}
	if (state.shader_data_storage_variable != 0) {
		return Load(state.shader_data_storage_variable, TypeStorageBufferElementPointer(state),
		            dword_index);
	}
	return ConstantU32(state, 0);
}

uint32_t EmitDynamicMemoryByteOffset(EmitterState& state, uint32_t resource_index) {
	if (state.program.bindings
	        .descriptor_counts[static_cast<size_t>(IR::DescriptorBindingKind::Buffers)] == 0) {
		return ConstantU32(state, 0);
	}
	const auto word_index = EmitBinaryU32(
	    state, spv::OpIAdd, ConstantU32(state, state.program.bindings.memory_offset_dword),
	    EmitBinaryU32(state, spv::OpShiftRightLogical, resource_index, ConstantU32(state, 2u)));
	const auto word  = EmitShaderDataDwordLoadDynamic(state, word_index);
	const auto shift = EmitBinaryU32(
	    state, spv::OpShiftLeftLogical,
	    EmitBinaryU32(state, spv::OpBitwiseAnd, resource_index, ConstantU32(state, 3u)),
	    ConstantU32(state, 3u));
	return EmitBinaryU32(state, spv::OpBitwiseAnd,
	                     EmitBinaryU32(state, spv::OpShiftRightLogical, word, shift),
	                     ConstantU32(state, 0xffu));
}

uint32_t EmitBinaryU32(EmitterState& state, spv::Op opcode, uint32_t lhs, uint32_t rhs) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction(opcode, TypeU32(state), ret, lhs, rhs);
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

void EmitMemoryOffsets(EmitterState& state) {
	const auto count = state.program.bindings.descriptor_counts[static_cast<size_t>(IR::DescriptorBindingKind::Buffers)];
	for (uint32_t i = 0; i < count; i += 4u) {
		const auto word =
		    EmitShaderDataDwordLoad(state, state.program.bindings.memory_offset_dword + i / 4u);
		for (uint32_t lane = 0; lane < std::min(4u, count - i); ++lane) {
			state.memory_byte_offsets[i + lane] = EmitNative<spv::OpBitFieldUExtract, IR::Type::U32>(
			    state, word, ConstantU32(state, lane * 8u), ConstantU32(state, 8u));
		}
	}
}

uint32_t LdsDwordCount(const EmitterState& state) {
	const auto* workgroup = ShaderWorkgroupInput(state.program.stage, state.input_info);
	if (workgroup != nullptr) {
		return workgroup->lds_size_dwords;
	}
	return state.requirements.function_lds_dwords != 0 ? state.requirements.function_lds_dwords
	                                                   : 8192u;
}

void EnsureLdsStorage(EmitterState& state) {
	if (state.lds_variable != 0) {
		return;
	}
	if (state.lds_storage_class != spv::StorageClassWorkgroup) {
		EXIT("LDS backing was not prepared before SPIR-V function emission\n");
	}
	const auto define = [&](uint32_t type, uint32_t bytes) {
		const auto array = state.builder.DecoratedType(
		    spv::OpTypeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, bytes}}}, type,
		    ConstantU32(state, std::max(LdsDwordCount(state) * 4u / bytes, 1u)));
		const auto block = state.builder.DecoratedType(
		    spv::OpTypeStruct, {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
		                        {spv::OpDecorate, {spv::DecorationBlock}}}, array);
		const auto variable = state.builder.DefineGlobalVariable(
		    TypePointer(state, spv::StorageClassWorkgroup, block), spv::StorageClassWorkgroup);
		state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationAliased);
		return variable;
	};
	if (state.program.info.shared_int64_atomics) {
		state.lds_variable = define(TypeU32(state), 4u);
		state.lds_u64_variable = define(TypeU64(state), 8u);
		state.builder.AddName(state.lds_u64_variable, "lds_qwords");
	} else {
		state.lds_variable = state.builder.DefineGlobalVariable(
		    TypeU32ArrayPointer(state, spv::StorageClassWorkgroup, LdsDwordCount(state)),
		    spv::StorageClassWorkgroup);
	}
	state.builder.AddName(state.lds_variable, "lds_dwords");
}

uint32_t EmitLdsQwordPointer(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain,
	                          TypeU64ElementPointer(state, spv::StorageClassWorkgroup), pointer,
	                          state.lds_u64_variable, ConstantU32(state, 0), index);
	return pointer;
}

MemoryResourceAccess PrepareStorageBufferResourceAccess(EmitterState& state,
                                                         const IR::MemoryInfo& mem,
                                                         BufferDefinition& buffer) {
	if (buffer.variable == 0) {
		ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "storage buffer descriptor array was not emitted");
	}
	if (buffer.pointer_type == 0) {
		buffer.pointer_type = TypePointer(state, spv::StorageClassStorageBuffer, buffer.type);
	}
	const auto array_index =
	    state.program.info.buffers.at(mem.resource).descriptor_index;
	// An indirect buffer table resolves one access against whichever candidate its key search
	// picked, so the slot is an id rather than a literal. The index is only wave-uniform, which is
	// what NonUniform states; the decoration on the index is placed where the index is computed.
	const bool dynamic =
	    state.dynamic_buffer_index != 0 && mem.resource == state.dynamic_buffer_resource;
	MemoryResourceAccess access {
	    .kind = mem.kind,
	    .memory_access = mem.coherent ? spv::MemoryAccessVolatileMask : spv::MemoryAccessMaskNone};
	access.object_pointer = state.builder.AllocateId();
	state.builder.AddFunction(
	    spv::OpAccessChain, buffer.pointer_type, access.object_pointer, buffer.variable,
	    dynamic ? state.dynamic_buffer_index : ConstantU32(state, array_index));
	if (dynamic) {
		state.builder.AddAnnotation(spv::OpDecorate, access.object_pointer,
		                            spv::DecorationNonUniform);
	}
	access.byte_offset =
	    dynamic ? state.dynamic_buffer_byte_offset : state.memory_byte_offsets[array_index];
	access.length      = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpArrayLength, TypeU32(state), access.length,
	                          access.object_pointer, 0);
	return access;
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
			if (state.gds_length == 0) {
				EXIT("GDS length was not prepared at function entry\n");
			}
			access.length = state.gds_length;
			return access;
		case IR::ResourceKind::Scratch:
			if (state.scratch_variable[state.lane_half] == 0) {
				EXIT("scratch storage was not prepared before SPIR-V function emission\n");
			}
			access.object_pointer = state.scratch_variable[state.lane_half];
			access.length         = ConstantU32(state, state.program.scratch_dwords);
			return access;
		case IR::ResourceKind::ScalarAddress:
		case IR::ResourceKind::Flat:
		case IR::ResourceKind::Global:
			EXIT("physical address memory must use the BDA emitter\n");
		case IR::ResourceKind::ScalarBuffer:
		case IR::ResourceKind::Buffer: {
			// glc=1 takes the Coherent alias as whole dwords; glc=0 its element width's variable.
			const auto coherent =
			    CoherentBufferAccess(mem) && state.coherent_storage_buffer.variable != 0;
			const auto bits   = coherent || mem.kind != IR::ResourceKind::Buffer
			                        ? 32u
			                        : IR::StorageBufferElementBits(state.program, mem);
			auto* buffer = &state.storage_buffers[2];
			if (coherent) {
				buffer = &state.coherent_storage_buffer;
			} else if (bits == 8u) {
				buffer = &state.storage_buffers[0];
			} else if (bits == 16u) {
				buffer = &state.storage_buffers[1];
			}
			access = PrepareStorageBufferResourceAccess(state, mem, *buffer);
			access.element_bits = bits;
			access.coherent     = CoherentBufferAccess(mem);
			return access;
		}
		default: EXIT("unsupported memory resource kind: %u\n", static_cast<unsigned>(mem.kind));
	}
}

uint32_t EmitMemoryElementInBounds(EmitterState& state, const MemoryResourceAccess& access,
                                   uint32_t index) {
	const auto in_bounds = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpULessThan, TypeBool(state), in_bounds, index, access.length);
	return in_bounds;
}

uint32_t EmitMemoryElementPointer(EmitterState& state, const MemoryResourceAccess& access,
                                  uint32_t index) {
	if (access.kind == IR::ResourceKind::Lds || access.kind == IR::ResourceKind::Scratch) {
		const auto storage_class = access.kind == IR::ResourceKind::Scratch
		                               ? spv::StorageClassFunction : state.lds_storage_class;
		if (storage_class == spv::StorageClassStorageBuffer) {
			return EmitStorageBufferElementPointer(
			    state, access, EmitAddU32(state, state.lds_base_dwords, index),
			    TypeStorageBufferElementPointer(state, 32));
		}
		const auto pointer = state.builder.AllocateId();
		if (access.kind == IR::ResourceKind::Lds && state.program.info.shared_int64_atomics) {
			state.builder.AddFunction(spv::OpAccessChain, TypeU32ElementPointer(state, storage_class),
			                          pointer, access.object_pointer, ConstantU32(state, 0), index);
			return pointer;
		}
		state.builder.AddFunction(spv::OpAccessChain, TypeU32ElementPointer(state, storage_class),
		                          pointer, access.object_pointer, index);
		return pointer;
	}
	return EmitStorageBufferElementPointer(state, access, index,
	                                       TypeStorageBufferElementPointer(state, access.element_bits));
}

uint32_t EmitStorageBufferElementPointer(EmitterState& state,
                                         const MemoryResourceAccess& access, uint32_t index,
                                         uint32_t pointer_type) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, pointer_type, pointer, access.object_pointer,
	                          ConstantU32(state, 0), index);
	return pointer;
}

uint32_t EmitTBufferBitcastU32ToI32(EmitterState& state, uint32_t value) {
	const auto ret = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, TypeI32(state), ret, value);
	return ret;
}

bool IsSignedFormatComponent(Format::ComponentType type) {
	return type == Format::ComponentType::Sint || type == Format::ComponentType::Snorm ||
	       type == Format::ComponentType::Sscaled;
}

uint32_t EmitUFloatToF32Bits(EmitterState& state, uint32_t raw, uint32_t bits) {
	const auto mantissa_bits = bits == 11u ? 6u : 5u;
	const auto mantissa_mask = (1u << mantissa_bits) - 1u;
	const auto mantissa =
	    EmitBinaryU32(state, spv::OpBitwiseAnd, raw, ConstantU32(state, mantissa_mask));
	const auto exponent = EmitBinaryU32(
	    state, spv::OpBitwiseAnd,
	    EmitBinaryU32(state, spv::OpShiftRightLogical, raw, ConstantU32(state, mantissa_bits)),
	    ConstantU32(state, 0x1fu));

	const auto exponent_32 = EmitAddU32(state, exponent, ConstantU32(state, 127u - 15u));
	const auto exponent_bits =
	    EmitBinaryU32(state, spv::OpShiftLeftLogical, exponent_32, ConstantU32(state, 23));
	const auto mantissa_bits_32 = EmitBinaryU32(state, spv::OpShiftLeftLogical, mantissa,
	                                            ConstantU32(state, 23u - mantissa_bits));
	const auto normal_bits =
	    EmitBinaryU32(state, spv::OpBitwiseOr, exponent_bits, mantissa_bits_32);
	const auto normal      = EmitBitcastU32ToF32(state, normal_bits);

	const auto special_bits =
	    EmitBinaryU32(state, spv::OpBitwiseOr, ConstantU32(state, 0x7f800000u), mantissa_bits_32);
	const auto special = EmitBitcastU32ToF32(state, special_bits);

	const auto mantissa_f32 = state.builder.AllocateId();
	const auto subnormal    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertUToF, TypeF32(state), mantissa_f32, mantissa);
	state.builder.AddFunction(
	    spv::OpFMul, TypeF32(state), subnormal, mantissa_f32,
	    ConstantF32Value(state, std::ldexp(1.0f, 1 - 15 - static_cast<int>(mantissa_bits))));

	const auto zero_exp    = EmitCompareU32Constant(state, spv::OpIEqual, exponent, 0);
	const auto special_exp = EmitCompareU32Constant(state, spv::OpIEqual, exponent, 31);
	const auto finite      = EmitTBufferSelectF32(state, zero_exp, subnormal, normal);
	const auto result      = EmitTBufferSelectF32(state, special_exp, special, finite);
	return EmitBitcastF32ToU32(state, result);
}

uint32_t NormalizeFormatComponent(EmitterState& state, const Format::BufferFormatInfo& info,
                                  uint32_t component, uint32_t raw) {
	const auto bits = info.component_bits[component];
	switch (info.type) {
		case Format::ComponentType::Uint:
		case Format::ComponentType::Sint: return raw;
		case Format::ComponentType::Uscaled: {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpConvertUToF, TypeF32(state), value, raw);
			return EmitBitcastF32ToU32(state, value);
		}
		case Format::ComponentType::Sscaled: {
			const auto signed_raw = EmitTBufferBitcastU32ToI32(state, raw);
			const auto value      = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpConvertSToF, TypeF32(state), value, signed_raw);
			return EmitBitcastF32ToU32(state, value);
		}
		case Format::ComponentType::Unorm: {
			const auto value      = state.builder.AllocateId();
			const auto normalized = state.builder.AllocateId();
			const auto max_value  = static_cast<float>((1u << bits) - 1u);
			state.builder.AddFunction(spv::OpConvertUToF, TypeF32(state), value, raw);
			state.builder.AddFunction(spv::OpFDiv, TypeF32(state), normalized, value,
			                          ConstantF32Value(state, max_value));
			return EmitBitcastF32ToU32(state, normalized);
		}
		case Format::ComponentType::Snorm: {
			const auto signed_raw = EmitTBufferBitcastU32ToI32(state, raw);
			const auto value      = state.builder.AllocateId();
			const auto normalized = state.builder.AllocateId();
			const auto clamped    = state.builder.AllocateId();
			const auto max_value  = static_cast<float>((1u << (bits - 1u)) - 1u);
			state.builder.AddFunction(spv::OpConvertSToF, TypeF32(state), value, signed_raw);
			state.builder.AddFunction(spv::OpFDiv, TypeF32(state), normalized, value,
			                          ConstantF32Value(state, max_value));
			state.builder.AddFunction(spv::OpExtInst, TypeF32(state), clamped, GlslStd450(state),
			                          GLSLstd450FMax, normalized, ConstantF32Value(state, -1.0f));
			return EmitBitcastF32ToU32(state, clamped);
		}
		case Format::ComponentType::Float:
			if (bits == 32u) {
				return raw;
			}
			if (bits == 16u) {
				return EmitBitcastF32ToU32(state, EmitF16BitsToF32(state, raw));
			}
			return EmitUFloatToF32Bits(state, raw, bits);
		default: return raw;
	}
}

static uint32_t EmitGlslF32(EmitterState& state, GLSLstd450 op, std::initializer_list<uint32_t> args) {
	const auto            result = state.builder.AllocateId();
	std::vector<uint32_t> words {spv::OpExtInst, TypeF32(state), result, GlslStd450(state),
	                             static_cast<uint32_t>(op)};
	words.insert(words.end(), args.begin(), args.end());
	state.builder.AddFunction(words);
	return result;
}

// NaN is written as zero, the D3D rule for float to fixed-point conversion.
static uint32_t EmitF32NanToZero(EmitterState& state, uint32_t bits) {
	return EmitTBufferSelectF32(state, EmitClassifyF32Bits(state, bits).nan, ConstantF32(state, 0),
	                            EmitBitcastU32ToF32(state, bits));
}

// Positive values round to nearest, overflow saturates to the largest finite value, negative
// values and -Inf become zero, NaN becomes the all-ones exponent and mantissa.
static uint32_t EmitF32ToUFloatBits(EmitterState& state, uint32_t bits, uint32_t width) {
	const auto mantissa_bits = width == 11u ? 6u : 5u;
	const auto mantissa_mask = (1u << mantissa_bits) - 1u;
	const auto infinity      = 31u << mantissa_bits;
	const auto max_finite    = (30u << mantissa_bits) | mantissa_mask;
	const auto drop          = 23u - mantissa_bits;

	const auto exponent = EmitAndConstant(state, EmitShiftRightConstant(state, bits, 23), 0xffu);
	const auto mantissa = EmitAndConstant(state, bits, 0x007fffffu);
	const auto negative = EmitCompareU32Constant(state, spv::OpUGreaterThanEqual, bits, 0x80000000u);
	const auto special  = EmitCompareU32Constant(state, spv::OpIEqual, exponent, 255u);
	const auto nan = EmitLogicalAndBool(state, special,
	                                    EmitCompareU32Constant(state, spv::OpINotEqual, mantissa, 0));

	// A carry out of the rounded mantissa lands in the exponent field, which is the correct result.
	const auto normal_exponent = EmitBinaryU32(state, spv::OpShiftLeftLogical,
	                                           EmitBinaryU32(state, spv::OpISub, exponent,
	                                                         ConstantU32(state, 127u - 15u)),
	                                           ConstantU32(state, mantissa_bits));
	const auto rounded_mantissa =
	    EmitShiftRightConstant(state, EmitAddU32(state, mantissa, ConstantU32(state, 1u << (drop - 1u))),
	                           drop);
	const auto normal     = EmitAddU32(state, normal_exponent, rounded_mantissa);
	const auto overflow   = EmitCompareU32Constant(state, spv::OpUGreaterThanEqual, normal, infinity);
	const auto normal_sat = EmitSelectValueU32(state, overflow, ConstantU32(state, max_finite), normal);

	const auto scaled = state.builder.AllocateId();
	state.builder.AddFunction(
	    spv::OpFMul, TypeF32(state), scaled, EmitBitcastU32ToF32(state, bits),
	    ConstantF32Value(state, std::ldexp(1.0f, 15 - 1 + static_cast<int>(mantissa_bits))));
	const auto subnormal = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertFToU, TypeU32(state), subnormal,
	                          EmitGlslF32(state, GLSLstd450RoundEven, {scaled}));

	const auto is_subnormal = EmitCompareU32Constant(state, spv::OpULessThanEqual, exponent, 127u - 15u);
	const auto finite       = EmitSelectValueU32(state, is_subnormal, subnormal, normal_sat);
	const auto non_negative = EmitSelectValueU32(state, special, ConstantU32(state, infinity), finite);
	const auto sign_applied =
	    EmitSelectValueU32(state, negative, ConstantU32(state, 0), non_negative);
	return EmitSelectValueU32(state, nan, ConstantU32(state, infinity | mantissa_mask), sign_applied);
}

uint32_t EncodeFormatComponent(EmitterState& state, const Format::BufferFormatInfo& info,
                               uint32_t component, uint32_t data) {
	const auto bits          = info.component_bits[component];
	const auto mask          = bits >= 32u ? 0xffffffffu : (1u << bits) - 1u;
	const auto signed_to_raw = [&](uint32_t value) {
		const auto raw = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpBitcast, TypeU32(state), raw, value);
		return EmitAndConstant(state, raw, mask);
	};
	// Two-bit packed channels are unsigned even in signed formats.
	auto type = info.type;
	if (bits == 2u && type == Format::ComponentType::Snorm) {
		type = Format::ComponentType::Unorm;
	} else if (bits == 2u && type == Format::ComponentType::Sscaled) {
		type = Format::ComponentType::Uscaled;
	}
	switch (type) {
		case Format::ComponentType::Uint:
		case Format::ComponentType::Sint: return data;
		// Normalized stores round to nearest (ties to even), the D3D and Vulkan conversion rule.
		case Format::ComponentType::Unorm: {
			const auto clamped = EmitGlslF32(
			    state, GLSLstd450FClamp,
			    {EmitF32NanToZero(state, data), ConstantF32Value(state, 0.0f), ConstantF32Value(state, 1.0f)});
			const auto scaled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpFMul, TypeF32(state), scaled, clamped,
			                          ConstantF32Value(state, static_cast<float>(mask)));
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpConvertFToU, TypeU32(state), value,
			                          EmitGlslF32(state, GLSLstd450RoundEven, {scaled}));
			return value;
		}
		// Clamping to -1 never produces the most negative code, which the load also reads as -1.
		case Format::ComponentType::Snorm: {
			const auto clamped = EmitGlslF32(
			    state, GLSLstd450FClamp,
			    {EmitF32NanToZero(state, data), ConstantF32Value(state, -1.0f), ConstantF32Value(state, 1.0f)});
			const auto scaled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpFMul, TypeF32(state), scaled, clamped,
			                          ConstantF32Value(state, static_cast<float>(mask >> 1u)));
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpConvertFToS, TypeI32(state), value,
			                          EmitGlslF32(state, GLSLstd450RoundEven, {scaled}));
			return signed_to_raw(value);
		}
		// Scaled stores are a float to integer cast: truncate toward zero and saturate.
		case Format::ComponentType::Uscaled: {
			const auto clamped = EmitGlslF32(
			    state, GLSLstd450FClamp,
			    {EmitF32NanToZero(state, data), ConstantF32Value(state, 0.0f),
			     ConstantF32Value(state, static_cast<float>(mask))});
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpConvertFToU, TypeU32(state), value,
			                          EmitGlslF32(state, GLSLstd450Trunc, {clamped}));
			return value;
		}
		case Format::ComponentType::Sscaled: {
			const auto max_value = static_cast<float>(mask >> 1u);
			const auto clamped   = EmitGlslF32(
			      state, GLSLstd450FClamp,
			      {EmitF32NanToZero(state, data), ConstantF32Value(state, -max_value - 1.0f),
			       ConstantF32Value(state, max_value)});
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpConvertFToS, TypeI32(state), value,
			                          EmitGlslF32(state, GLSLstd450Trunc, {clamped}));
			return signed_to_raw(value);
		}
		case Format::ComponentType::Float: {
			if (bits == 32u) {
				return data;
			}
			if (bits == 16u) {
				const auto pair = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, 2), pair,
				                          EmitBitcastU32ToF32(state, data), ConstantF32(state, 0));
				const auto packed = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpExtInst, TypeU32(state), packed, GlslStd450(state),
				                          GLSLstd450PackHalf2x16, pair);
				return EmitAndConstant(state, packed, 0xffffu);
			}
			return EmitF32ToUFloatBits(state, data, bits);
		}
		default: return data;
	}
}

spv::Op SpirvAtomicOpcode(IR::ValueOpcode opcode) {
	switch (opcode) {
		case IR::ValueOpcode::ImageAtomicCompareSwap32:
		case IR::ValueOpcode::BufferAtomicCmpSwap32: return spv::OpAtomicCompareExchange;
		case IR::ValueOpcode::ImageAtomicSwap32:
		case IR::ValueOpcode::ImageAtomicSwap64:
		case IR::ValueOpcode::BufferAtomicSwap32:
		case IR::ValueOpcode::BufferAtomicSwap64:
		case IR::ValueOpcode::SharedAtomicSwap32: return spv::OpAtomicExchange;
		case IR::ValueOpcode::ImageAtomicIAdd32:
		case IR::ValueOpcode::ImageAtomicIAdd64:
		case IR::ValueOpcode::BufferAtomicIAdd32:
		case IR::ValueOpcode::SharedAtomicIAdd64:
		case IR::ValueOpcode::SharedAtomicIAdd32: return spv::OpAtomicIAdd;
		case IR::ValueOpcode::BufferAtomicISub32:
		case IR::ValueOpcode::SharedAtomicISub32: return spv::OpAtomicISub;
		case IR::ValueOpcode::ImageAtomicSMin32:
		case IR::ValueOpcode::BufferAtomicSMin32:
		case IR::ValueOpcode::SharedAtomicSMin32: return spv::OpAtomicSMin;
		case IR::ValueOpcode::ImageAtomicUMin32:
		case IR::ValueOpcode::ImageAtomicUMin64:
		case IR::ValueOpcode::BufferAtomicUMin32:
		case IR::ValueOpcode::SharedAtomicUMin32: return spv::OpAtomicUMin;
		case IR::ValueOpcode::ImageAtomicSMax32:
		case IR::ValueOpcode::BufferAtomicSMax32:
		case IR::ValueOpcode::SharedAtomicSMax32: return spv::OpAtomicSMax;
		case IR::ValueOpcode::ImageAtomicUMax32:
		case IR::ValueOpcode::ImageAtomicUMax64:
		case IR::ValueOpcode::BufferAtomicUMax32:
		case IR::ValueOpcode::SharedAtomicUMax32: return spv::OpAtomicUMax;
		case IR::ValueOpcode::ImageAtomicAnd32:
		case IR::ValueOpcode::ImageAtomicAnd64:
		case IR::ValueOpcode::BufferAtomicAnd32:
		case IR::ValueOpcode::BufferAtomicAnd64:
		case IR::ValueOpcode::SharedAtomicAnd32: return spv::OpAtomicAnd;
		case IR::ValueOpcode::ImageAtomicOr32:
		case IR::ValueOpcode::ImageAtomicOr64:
		case IR::ValueOpcode::BufferAtomicOr32:
		case IR::ValueOpcode::BufferAtomicOr64:
		case IR::ValueOpcode::SharedAtomicOr64:
		case IR::ValueOpcode::SharedAtomicOr32: return spv::OpAtomicOr;
		case IR::ValueOpcode::ImageAtomicXor32:
		case IR::ValueOpcode::ImageAtomicXor64:
		case IR::ValueOpcode::BufferAtomicXor32:
		case IR::ValueOpcode::SharedAtomicXor32: return spv::OpAtomicXor;
		default: return spv::OpNop;
	}
}

uint32_t EmitAtomicOperation(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t pointer,
                             uint32_t scope, uint32_t semantics) {
	const auto opcode = SpirvAtomicOpcode(inst.GetOpcode());
	const auto old    = ctx.state.builder.AllocateId();
	const bool wide   = inst.GetType() == IR::Type::U64;
	const auto type   = wide ? TypeU64(ctx.state) : TypeU32(ctx.state);
	if (opcode == spv::OpAtomicCompareExchange) {
		const auto desired    = ctx.Arg(inst, inst.NumArgs() - 3);
		const auto comparator = ctx.Arg(inst, inst.NumArgs() - 2);
		// An unequal compare-exchange performs no store, so its semantics carry no release.
		ctx.state.builder.AddFunction(
		    spv::OpAtomicCompareExchange, TypeU32(ctx.state), old, pointer,
		    ConstantU32(ctx.state, scope), ConstantU32(ctx.state, semantics),
		    ConstantU32(ctx.state, spv::MemorySemanticsMaskNone), desired, comparator);
	} else {
		const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
		ctx.state.builder.AddFunction(opcode, type, old, pointer,
		                              ConstantU32(ctx.state, scope),
		                              ConstantU32(ctx.state, semantics), value);
	}
	return old;
}

void EmitAtomicMemoryBarrier(EmitterState& state, IR::ResourceKind kind) {
	const auto scope  = kind == IR::ResourceKind::Lds ? spv::ScopeWorkgroup : spv::ScopeDevice;
	const auto memory = [&] {
		switch (kind) {
			case IR::ResourceKind::Lds:
				return state.lds_storage_class == spv::StorageClassStorageBuffer
				           ? spv::MemorySemanticsUniformMemoryMask
				           : spv::MemorySemanticsWorkgroupMemoryMask;
			case IR::ResourceKind::Image: return spv::MemorySemanticsImageMemoryMask;
			default: return spv::MemorySemanticsUniformMemoryMask;
		}
	}();
	state.builder.AddFunction(spv::OpMemoryBarrier, ConstantU32(state, scope),
	                          ConstantU32(state, spv::MemorySemanticsAcquireReleaseMask | memory));
}

namespace {

uint32_t FloatOrderKey(EmitterState& state, uint32_t bits) {
	const auto negative = EmitCompareU32Constant(state, spv::OpINotEqual,
	                                             EmitAndConstant(state, bits, 0x80000000u), 0u);
	const auto negative_key = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpNot, TypeU32(state), negative_key, bits);
	const auto positive_key =
	    EmitBinaryU32(state, spv::OpBitwiseXor, bits, ConstantU32(state, 0x80000000u));
	return EmitSelectValueU32(state, negative, negative_key, positive_key);
}

} // namespace

uint32_t EmitFloatAtomicReplacement(EmitterState& state, uint32_t old, uint32_t source,
                                    bool max_value) {
	struct OrderedBits {
		uint32_t nan;
		uint32_t zero;
		uint32_t key;
	};
	const auto classify = [&](uint32_t bits) {
		const auto cls = EmitClassifyF32Bits(state, bits);
		return OrderedBits {cls.nan, cls.zero, FloatOrderKey(state, bits)};
	};
	const auto source_class = classify(source);
	const auto old_class    = classify(old);
	const auto unordered = EmitLogicalOrBool(
	    state, EmitLogicalOrBool(state, source_class.nan, old_class.nan),
	    EmitLogicalAndBool(state, source_class.zero, old_class.zero));
	const auto compare = state.builder.AllocateId();
	state.builder.AddFunction(max_value ? spv::OpUGreaterThan : spv::OpULessThan, TypeBool(state),
	                          compare, source_class.key, old_class.key);
	return EmitSelectValueU32(
	    state, EmitLogicalAndBool(state, EmitLogicalNotBool(state, unordered), compare), source,
	    old);
}

uint32_t EmitDsFloatAtomicReplacement(EmitterState& state, uint32_t old, uint32_t source,
                                      bool max_value) {
	struct OrderedBits {
		uint32_t nan;
		uint32_t signaling_nan;
		uint32_t key;
	};
	const auto classify = [&](uint32_t bits) {
		const auto abs = EmitAndConstant(state, bits, 0x7fffffffu);
		const auto nan = EmitCompareU32Constant(state, spv::OpUGreaterThan, abs, 0x7f800000u);
		const auto signaling_nan = EmitLogicalAndBool(
		    state, nan, EmitCompareU32Constant(state, spv::OpIEqual,
		                                      EmitAndConstant(state, bits, 0x00400000u), 0u));
		const auto small = EmitCompareU32Constant(state, spv::OpULessThan, abs, 0x00800000u);
		const auto sign = EmitAndConstant(state, bits, 0x80000000u);
		const auto positive_denorm = EmitLogicalAndBool(
		    state, EmitCompareU32Constant(state, spv::OpIEqual, sign, 0u),
		    EmitCompareU32Constant(state, spv::OpINotEqual, abs, 0u));
		// Negative denorms tie with -0; positive denorms tie only with each other.
		const auto small_bits = EmitSelectValueU32(
		    state, positive_denorm, ConstantU32(state, 1u), sign);
		const auto key_bits = EmitSelectValueU32(state, small, small_bits, bits);
		return OrderedBits {nan, signaling_nan, FloatOrderKey(state, key_bits)};
	};
	const auto lhs = classify(old);
	const auto rhs = classify(source);
	const auto choose_source = state.builder.AllocateId();
	state.builder.AddFunction(max_value ? spv::OpUGreaterThan : spv::OpULessThanEqual,
	                          TypeBool(state), choose_source, rhs.key, lhs.key);
	// Keep original bits: ordering denorms does not flush the selected operand.
	auto result = EmitSelectValueU32(state, choose_source, source, old);
	result = EmitSelectValueU32(state, lhs.nan, source, result);
	result = EmitSelectValueU32(state, rhs.nan, old, result);
	result = EmitSelectValueU32(state, rhs.signaling_nan,
	                            EmitOrU32(state, source, ConstantU32(state, 0x00400000u)), result);
	return EmitSelectValueU32(state, lhs.signaling_nan,
	                          EmitOrU32(state, old, ConstantU32(state, 0x00400000u)), result);
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
		state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), lane2, subid,
		                          ConstantU32(state, 3));
		state.builder.AddFunction(spv::OpShiftLeftLogical, TypeU32(state), shift, lane2,
		                          ConstantU32(state, 1));
		state.builder.AddFunction(spv::OpShiftRightLogical, TypeU32(state), perm0,
		                          ConstantU32(state, control), shift);
		state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), perm, perm0,
		                          ConstantU32(state, 3));
		state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), base, subid,
		                          ConstantU32(state, 0xfffffffcu));
		state.builder.AddFunction(spv::OpBitwiseOr, TypeU32(state), target, base, perm);
		return target;
	}

	const auto lane   = state.builder.AllocateId();
	const auto masked = state.builder.AllocateId();
	const auto ored   = state.builder.AllocateId();
	const auto xored  = state.builder.AllocateId();
	const auto base   = state.builder.AllocateId();
	const auto target = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), lane, subid,
	                          ConstantU32(state, 31));
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), masked, lane,
	                          ConstantU32(state, control & 0x1fu));
	state.builder.AddFunction(spv::OpBitwiseOr, TypeU32(state), ored, masked,
	                          ConstantU32(state, (control >> 5u) & 0x1fu));
	state.builder.AddFunction(spv::OpBitwiseXor, TypeU32(state), xored, ored,
	                          ConstantU32(state, (control >> 10u) & 0x1fu));
	state.builder.AddFunction(spv::OpBitwiseAnd, TypeU32(state), base, subid,
	                          ConstantU32(state, 0xffffffe0u));
	state.builder.AddFunction(spv::OpBitwiseOr, TypeU32(state), target, base, xored);
	return target;
}

// One word of the flattened SRT, by constant index. Used for the directory, whose words hold the
// runtime offsets that must not be baked into the module.
uint32_t LoadFlattenedSrtWord(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                          state.flattened_srt_variable, ConstantU32(state, 0),
	                          ConstantU32(state, index));
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

uint32_t EmitUnrolledCandidateSearch(EmitterState& state, uint32_t mapping_slot,
                                     uint32_t iterations, uint32_t key, uint32_t* matched) {
	const auto LoadMapping = [&](uint32_t index) {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state),
		                          pointer, state.flattened_srt_variable, ConstantU32(state, 0),
		                          index);
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
		return value;
	};
	// Where the mapping sits is an allocation result: it moves when any earlier table's key count
	// moves, and a constant here would put that in the module and in the permutation key, so one
	// table gaining a key would rebuild every table after it. The directory slot is a function of
	// the resource index alone, so it is the constant, and the offset is loaded through it.
	const auto mapping  = LoadMapping(ConstantU32(state, mapping_slot));
	auto       low      = ConstantU32(state, 0u);
	auto       high     = LoadMapping(mapping);
	auto       selected = ConstantU32(state, 0u);
	auto       any      = matched != nullptr ? ConstantBool(state, false) : 0u;
	for (uint32_t iteration = 0; iteration < iterations; iteration++) {
		const auto active = Binary(state, spv::OpULessThan, TypeBool(state), low, high);
		const auto mid    = Binary(state, spv::OpShiftRightLogical, TypeU32(state),
		                           Binary(state, spv::OpIAdd, TypeU32(state), low, high),
		                           ConstantU32(state, 1u));
		const auto probe  = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpSelect, TypeU32(state), probe, active, mid,
		                          ConstantU32(state, 0u));
		const auto entry = Binary(state, spv::OpIAdd, TypeU32(state), mapping,
		                          Binary(state, spv::OpIAdd, TypeU32(state),
		                                 Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
		                                        probe, ConstantU32(state, 1u)),
		                                 ConstantU32(state, 1u)));
		const auto mapped_key = LoadMapping(entry);
		const auto candidate  = LoadMapping(
		    Binary(state, spv::OpIAdd, TypeU32(state), entry, ConstantU32(state, 1u)));
		const auto equal = Binary(state, spv::OpIEqual, TypeBool(state), mapped_key, key);
		const auto match = Binary(state, spv::OpLogicalAnd, TypeBool(state), active, equal);
		const auto next_selected = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpSelect, TypeU32(state), next_selected, match, candidate,
		                          selected);
		selected              = next_selected;
		if (matched != nullptr) {
			any = Binary(state, spv::OpLogicalOr, TypeBool(state), any, match);
		}
		const auto less = Binary(state, spv::OpULessThan, TypeBool(state), mapped_key, key);
		const auto take_upper = Binary(state, spv::OpLogicalAnd, TypeBool(state), active, less);
		const auto take_lower = Binary(state, spv::OpLogicalAnd, TypeBool(state), active,
		                               Unary(state, spv::OpLogicalNot, TypeBool(state), less));
		const auto next_low   = state.builder.AllocateId();
		state.builder.AddFunction(
		    spv::OpSelect, TypeU32(state), next_low, take_upper,
		    Binary(state, spv::OpIAdd, TypeU32(state), mid, ConstantU32(state, 1u)), low);
		low                  = next_low;
		const auto next_high = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpSelect, TypeU32(state), next_high, take_lower, mid, high);
		high = next_high;
	}
	if (matched != nullptr) {
		*matched = any;
	}
	return selected;
}

IndirectCandidateSearch EmitIndirectCandidateSearch(EmitterState& state, uint32_t mapping_slot,
                                                    uint32_t key) {
	const auto LoadMapping = [&](uint32_t index) {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state),
		                          pointer, state.flattened_srt_variable, ConstantU32(state, 0),
		                          index);
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
		return value;
	};
	// Where the mapping sits is an allocation result: it moves when any earlier table's key count
	// moves, and a constant here would put that in the module and in the permutation key, so one
	// table gaining a key would rebuild every table after it. The directory slot is a function of
	// the resource index alone, so it is the constant, and the offset is loaded through it.
	IndirectCandidateSearch search;
	search.mapping     = LoadMapping(ConstantU32(state, mapping_slot));
	search.count       = LoadMapping(search.mapping);
	const auto entry   = state.current_label;
	const auto header  = state.builder.AllocateId();
	const auto body    = state.builder.AllocateId();
	const auto cont    = state.builder.AllocateId();
	const auto merge   = state.builder.AllocateId();
	const auto low     = state.builder.AllocateId();
	const auto high    = state.builder.AllocateId();
	search.selected    = state.builder.AllocateId();
	const auto next_low      = state.builder.AllocateId();
	const auto next_high     = state.builder.AllocateId();
	const auto next_selected = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBranch, header);
	EmitLabel(state, header);
	state.builder.AddFunction(spv::OpPhi, TypeU32(state), low, ConstantU32(state, 0u), entry,
	                          next_low, cont);
	state.builder.AddFunction(spv::OpPhi, TypeU32(state), high, search.count, entry, next_high,
	                          cont);
	state.builder.AddFunction(spv::OpPhi, TypeU32(state), search.selected, ConstantU32(state, 0u),
	                          entry, next_selected, cont);
	const auto active = Binary(state, spv::OpULessThan, TypeBool(state), low, high);
	state.builder.AddFunction(spv::OpLoopMerge, merge, cont, spv::LoopControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, active, body, merge);
	EmitLabel(state, body);
	// Halving the distance rather than the sum cannot wrap, so every step shrinks the range.
	const auto mid = Binary(state, spv::OpIAdd, TypeU32(state), low,
	                        Binary(state, spv::OpShiftRightLogical, TypeU32(state),
	                               Binary(state, spv::OpISub, TypeU32(state), high, low),
	                               ConstantU32(state, 1u)));
	const auto pair = Binary(state, spv::OpIAdd, TypeU32(state), search.mapping,
	                         Binary(state, spv::OpIAdd, TypeU32(state),
	                                Binary(state, spv::OpShiftLeftLogical, TypeU32(state), mid,
	                                       ConstantU32(state, 1u)),
	                                ConstantU32(state, 1u)));
	const auto mapped_key = LoadMapping(pair);
	const auto candidate =
	    LoadMapping(Binary(state, spv::OpIAdd, TypeU32(state), pair, ConstantU32(state, 1u)));
	const auto equal = Binary(state, spv::OpIEqual, TypeBool(state), mapped_key, key);
	state.builder.AddFunction(spv::OpSelect, TypeU32(state), next_selected, equal, candidate,
	                          search.selected);
	const auto less = Binary(state, spv::OpULessThan, TypeBool(state), mapped_key, key);
	state.builder.AddFunction(spv::OpSelect, TypeU32(state), next_low, less,
	                          Binary(state, spv::OpIAdd, TypeU32(state), mid,
	                                 ConstantU32(state, 1u)),
	                          low);
	state.builder.AddFunction(spv::OpSelect, TypeU32(state), next_high, less, high, mid);
	state.builder.AddFunction(spv::OpBranch, cont);
	EmitLabel(state, cont);
	state.builder.AddFunction(spv::OpBranch, header);
	EmitLabel(state, merge);
	return search;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
