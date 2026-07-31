#include "common/assert.h"
#include "graphics/shader/recompiler/emitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t EmitCubeAxisF32(EmitterState& state, uint32_t value) {
	const auto normalized = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpFSub, state.float_type, normalized, value, ConstantF32(state, 0x3f800000u)});
	return normalized;
}

uint32_t EmitCubeLayerF32(EmitterState& state, uint32_t face_id) {
	// Sampled RDNA2 cubemaps encode face_id as slice * 8 + face. The native
	// 2D-array view stores six contiguous faces per slice, so remove the two
	// reserved face IDs from every preceding slice.
	const auto guest_layer = state.builder.AllocateId();
	const auto slice       = state.builder.AllocateId();
	const auto padding     = state.builder.AllocateId();
	const auto host_layer  = state.builder.AllocateId();
	const auto result      = state.builder.AllocateId();
	state.builder.AddFunction({OpConvertFToU, state.uint_type, guest_layer, face_id});
	state.builder.AddFunction(
	    {OpShiftRightLogical, state.uint_type, slice, guest_layer, ConstantU32(state, 3)});
	state.builder.AddFunction(
	    {OpShiftLeftLogical, state.uint_type, padding, slice, ConstantU32(state, 1)});
	state.builder.AddFunction({OpISub, state.uint_type, host_layer, guest_layer, padding});
	state.builder.AddFunction({OpConvertUToF, state.float_type, result, host_layer});
	return result;
}

uint32_t EmitImageCoordF32Impl(EmitterState& state, const IR::Instruction& inst,
                               const IR::Operand& address, uint32_t first_component,
                               uint32_t components) {
	auto x = EmitImageAddressFloatLoad(state, inst, address, first_component);
	if (components == 1u) {
		return x;
	}
	auto y = inst.memory.image_address_components > first_component + 1u
	             ? EmitImageAddressFloatLoad(state, inst, address, first_component + 1u)
	             : EmitZeroF32(state);
	if (inst.memory.image_cube) {
		// RDNA2 sampled cubemap S/T coordinates are biased by +1 relative to
		// normalized 2D-array coordinates.
		x = EmitCubeAxisF32(state, x);
		y = EmitCubeAxisF32(state, y);
	}
	const auto coord = state.builder.AllocateId();
	if (components == 3u) {
		auto z = inst.memory.image_address_components > first_component + 2u
		             ? EmitImageAddressFloatLoad(state, inst, address, first_component + 2u)
		             : EmitZeroF32(state);
		if (inst.memory.image_cube) {
			z = EmitCubeLayerF32(state, z);
		}
		state.builder.AddFunction({OpCompositeConstruct, state.vec3_float_type, coord, x, y, z});
	} else {
		state.builder.AddFunction({OpCompositeConstruct, state.vec2_float_type, coord, x, y});
	}
	return coord;
}

} // namespace

bool HasImageSampleFlag(const IR::Instruction& inst, uint32_t flag) {
	return (inst.memory.image_sample_flags & flag) != 0;
}

ImageSampleLayout MakeImageSampleLayout(const IR::Instruction& inst, ImageViewKind view) {
	ImageSampleLayout layout;
	uint32_t          cursor = 0;
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagOffset)) {
		layout.offset = cursor++;
	}
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagBias)) {
		layout.bias = cursor++;
	}
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagCompare)) {
		layout.dref = cursor++;
	}
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagDerivative)) {
		const auto components = ImageViewSpatialComponents(view);
		layout.grad_x         = cursor;
		cursor += components;
		layout.grad_y = cursor;
		cursor += components;
	}
	layout.coord = cursor;
	cursor += ImageViewCoordinateComponents(view);
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagLod)) {
		layout.lod = cursor++;
	}
	return layout;
}

uint32_t EmitImageCoordF32(EmitterState& state, const IR::Instruction& inst,
                           const ImageSampleLayout& layout, ImageViewKind view) {
	return EmitImageCoordF32Impl(state, inst, inst.src[0], layout.coord,
	                             ImageViewCoordinateComponents(view));
}

uint32_t EmitImageLodF32(EmitterState& state, const IR::Instruction& inst,
                         const ImageSampleLayout& layout) {
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagLevelZero) ||
	    layout.lod == NoImageComponent || inst.memory.image_address_components <= layout.lod) {
		return EmitZeroF32(state);
	}
	return EmitImageAddressFloatLoad(state, inst, inst.src[0], layout.lod);
}

uint32_t EmitImageDrefF32(EmitterState& state, const IR::Instruction& inst,
                          const ImageSampleLayout& layout) {
	if (layout.dref == NoImageComponent || inst.memory.image_address_components <= layout.dref) {
		return EmitZeroF32(state);
	}
	return EmitImageAddressFloatLoad(state, inst, inst.src[0], layout.dref);
}

uint32_t EmitImageBiasF32(EmitterState& state, const IR::Instruction& inst,
                          const ImageSampleLayout& layout) {
	if (layout.bias == NoImageComponent || inst.memory.image_address_components <= layout.bias) {
		return EmitZeroF32(state);
	}
	return EmitImageAddressFloatLoad(state, inst, inst.src[0], layout.bias);
}

uint32_t EmitImageGradientF32(EmitterState& state, const IR::Instruction& inst,
                              uint32_t first_component, ImageViewKind view) {
	const auto x = inst.memory.image_address_components > first_component
	                   ? EmitImageAddressFloatLoad(state, inst, inst.src[0], first_component)
	                   : EmitZeroF32(state);
	const auto components = ImageViewSpatialComponents(view);
	if (components == 1u) {
		return x;
	}
	const auto y = inst.memory.image_address_components > first_component + 1u
	                   ? EmitImageAddressFloatLoad(state, inst, inst.src[0], first_component + 1u)
	                   : EmitZeroF32(state);
	const auto grad = state.builder.AllocateId();
	if (components == 3u) {
		const auto z =
		    inst.memory.image_address_components > first_component + 2u
		        ? EmitImageAddressFloatLoad(state, inst, inst.src[0], first_component + 2u)
		        : EmitZeroF32(state);
		state.builder.AddFunction({OpCompositeConstruct, state.vec3_float_type, grad, x, y, z});
	} else {
		state.builder.AddFunction({OpCompositeConstruct, state.vec2_float_type, grad, x, y});
	}
	return grad;
}

uint32_t EmitImagePackedOffsetI32(EmitterState& state, const IR::Instruction& inst,
                                  const ImageSampleLayout& layout, ImageViewKind view) {
	const auto components = ImageViewSpatialComponents(view);
	if (layout.offset == NoImageComponent ||
	    inst.memory.image_address_components <= layout.offset) {
		const auto zero = ConstantI32(state, 0);
		if (components == 1u) {
			return zero;
		}
		const auto ret = state.builder.AllocateId();
		if (components == 3u) {
			state.builder.AddFunction(
			    {OpCompositeConstruct, state.vec3_int_type, ret, zero, zero, zero});
		} else {
			state.builder.AddFunction({OpCompositeConstruct, state.vec2_int_type, ret, zero, zero});
		}
		return ret;
	}

	const auto packed_bits = EmitImageAddressValueLoad(state, inst, inst.src[0], layout.offset);
	const auto packed_i32  = state.builder.AllocateId();
	const auto offset_x    = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, state.int_type, packed_i32, packed_bits});
	state.builder.AddFunction({OpBitFieldSExtract, state.int_type, offset_x, packed_i32,
	                           ConstantI32(state, 0), ConstantI32(state, 6)});
	if (components == 1u) {
		return offset_x;
	}
	const auto offset_y = state.builder.AllocateId();
	const auto offset   = state.builder.AllocateId();
	state.builder.AddFunction({OpBitFieldSExtract, state.int_type, offset_y, packed_i32,
	                           ConstantI32(state, 8), ConstantI32(state, 6)});
	if (components == 3u) {
		const auto offset_z = state.builder.AllocateId();
		state.builder.AddFunction({OpBitFieldSExtract, state.int_type, offset_z, packed_i32,
		                           ConstantI32(state, 16), ConstantI32(state, 6)});
		state.builder.AddFunction(
		    {OpCompositeConstruct, state.vec3_int_type, offset, offset_x, offset_y, offset_z});
	} else {
		state.builder.AddFunction(
		    {OpCompositeConstruct, state.vec2_int_type, offset, offset_x, offset_y});
	}
	return offset;
}

uint32_t EmitImageCoordU32(EmitterState& state, const IR::Instruction& inst, ImageViewKind view) {
	const auto x          = EmitImageAddressValueLoad(state, inst, inst.src[1], 0);
	const auto components = ImageViewCoordinateComponents(view);
	if (components == 1u) {
		return x;
	}
	const auto y     = inst.memory.image_address_components > 1u
	                       ? EmitImageAddressValueLoad(state, inst, inst.src[1], 1)
	                       : ConstantU32(state, 0);
	const auto coord = state.builder.AllocateId();
	if (components == 3u) {
		const auto z = inst.memory.image_address_components > 2u
		                   ? EmitImageAddressValueLoad(state, inst, inst.src[1], 2)
		                   : ConstantU32(state, 0);
		state.builder.AddFunction({OpCompositeConstruct, state.vec3_uint_type, coord, x, y, z});
	} else {
		state.builder.AddFunction({OpCompositeConstruct, state.vec2_uint_type, coord, x, y});
	}
	return coord;
}

uint32_t EmitImageLoadCoordU32(EmitterState& state, const IR::Instruction& inst,
                               ImageViewKind view) {
	const auto x          = EmitImageAddressValueLoad(state, inst, inst.src[0], 0);
	const auto components = ImageViewCoordinateComponents(view);
	if (components == 1u) {
		return x;
	}
	const auto y     = inst.memory.image_address_components > 1u
	                       ? EmitImageAddressValueLoad(state, inst, inst.src[0], 1)
	                       : ConstantU32(state, 0);
	const auto coord = state.builder.AllocateId();
	if (components == 3u) {
		const auto z = inst.memory.image_address_components > 2u
		                   ? EmitImageAddressValueLoad(state, inst, inst.src[0], 2)
		                   : ConstantU32(state, 0);
		state.builder.AddFunction({OpCompositeConstruct, state.vec3_uint_type, coord, x, y, z});
	} else {
		state.builder.AddFunction({OpCompositeConstruct, state.vec2_uint_type, coord, x, y});
	}
	return coord;
}

uint32_t EmitImageMipLodU32(EmitterState& state, const IR::Instruction& inst,
                            const IR::Operand& address, ImageViewKind view) {
	if (!inst.memory.image_has_mip || inst.memory.image_address_components == 0u) {
		return ConstantU32(state, 0);
	}
	const auto lod_component = ImageViewCoordinateComponents(view);
	if (inst.memory.image_address_components <= lod_component) {
		return ConstantU32(state, 0);
	}
	return EmitImageAddressValueLoad(state, inst, address, lod_component);
}

uint32_t EmitImageQueryCoordF32(EmitterState& state, const IR::Instruction& inst,
                                ImageViewKind view) {
	// OpImageQueryLod takes only the spatial coordinates, even for arrayed images.
	return EmitImageCoordF32Impl(state, inst, inst.src[0], 0, ImageViewSpatialComponents(view));
}

uint32_t DmaskComponentIndex(uint32_t dmask, uint32_t component) {
	uint32_t index = 0;
	for (uint32_t i = 0; i < component; i++) {
		index += (dmask >> i) & 1u;
	}
	return index;
}

uint32_t EmitImageStoreComponentF32(EmitterState& state, const IR::Instruction& inst,
                                    uint32_t component) {
	const auto dmask = inst.memory.dmask != 0 ? inst.memory.dmask : 1u;
	if (((dmask >> component) & 1u) == 0) {
		return EmitZeroF32(state);
	}
	return EmitSequentialFloatLoad(state, inst.src[0], DmaskComponentIndex(dmask, component));
}

uint32_t InverseStorageSwizzleComponent(uint32_t swizzle, uint32_t component) {
	const auto target = 4u + component;
	for (uint32_t source = 0; source < 4u; source++) {
		if (((swizzle >> (source * 3u)) & 0x7u) == target) {
			return source;
		}
	}
	return UINT32_MAX;
}

uint32_t StorageImageSwizzle(const EmitterState& state, const IR::Instruction& inst) {
	if (inst.memory.resource >= state.program.info.images.size()) {
		EXIT("storage image instruction has no specialized swizzle\n");
	}
	return state.program.info.images[inst.memory.resource].storage_swizzle;
}

uint32_t EmitImageStoreTexelF32(EmitterState& state, const IR::Instruction& inst) {
	const auto swizzle         = StorageImageSwizzle(state, inst);
	const auto component_value = [&](uint32_t component) {
		const auto source = InverseStorageSwizzleComponent(swizzle, component);
		return source < 4u ? EmitImageStoreComponentF32(state, inst, source) : EmitZeroF32(state);
	};
	const auto x     = component_value(0);
	const auto y     = component_value(1);
	const auto z     = component_value(2);
	const auto w     = component_value(3);
	const auto texel = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeConstruct, state.vec4_float_type, texel, x, y, z, w});
	return texel;
}

uint32_t EmitImageStoreComponentU32(EmitterState& state, const IR::Instruction& inst,
                                    uint32_t component) {
	const auto dmask = inst.memory.dmask != 0 ? inst.memory.dmask : 1u;
	if (((dmask >> component) & 1u) == 0) {
		return ConstantU32(state, 0);
	}
	return EmitSequentialValueLoad(state, inst.src[0], DmaskComponentIndex(dmask, component));
}

uint32_t EmitImageStoreTexelU32(EmitterState& state, const IR::Instruction& inst) {
	const auto swizzle         = StorageImageSwizzle(state, inst);
	const auto component_value = [&](uint32_t component) {
		const auto source = InverseStorageSwizzleComponent(swizzle, component);
		return source < 4u ? EmitImageStoreComponentU32(state, inst, source)
		                   : ConstantU32(state, 0);
	};
	const auto x     = component_value(0);
	const auto y     = component_value(1);
	const auto z     = component_value(2);
	const auto w     = component_value(3);
	const auto texel = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeConstruct, state.vec4_uint_type, texel, x, y, z, w});
	return texel;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
