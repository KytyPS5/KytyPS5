#include "common/assert.h"
#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

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
		layout.grad_x = cursor;
		cursor += 2u;
		layout.grad_y = cursor;
		cursor += 2u;
	}
	layout.coord = cursor;
	cursor += ImageViewCoordinateComponents(view);
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagLod)) {
		layout.lod = cursor++;
	}
	return layout;
}

uint32_t EmitImageCoordF32(EmitterState* state, const IR::Instruction& inst,
                           const ImageSampleLayout& layout, ImageViewKind view) {
	const auto x     = EmitImageAddressFloatLoad(state, inst, inst.src[0], layout.coord);
	const auto y     = inst.memory.image_address_components > layout.coord + 1u
	                       ? EmitImageAddressFloatLoad(state, inst, inst.src[0], layout.coord + 1u)
	                       : EmitZeroF32(state);
	const auto coord = state->builder.AllocateId();
	if (ImageViewCoordinateComponents(view) == 3u) {
		const auto z = inst.memory.image_address_components > layout.coord + 2u
		                   ? EmitImageAddressFloatLoad(state, inst, inst.src[0], layout.coord + 2u)
		                   : EmitZeroF32(state);
		state->builder.AddFunction({OpCompositeConstruct, state->vec3_float_type, coord, x, y, z});
	} else {
		state->builder.AddFunction({OpCompositeConstruct, state->vec2_float_type, coord, x, y});
	}
	return coord;
}

uint32_t EmitImageLodF32(EmitterState* state, const IR::Instruction& inst,
                         const ImageSampleLayout& layout) {
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagLevelZero) ||
	    layout.lod == NoImageComponent || inst.memory.image_address_components <= layout.lod) {
		return EmitZeroF32(state);
	}
	return EmitImageAddressFloatLoad(state, inst, inst.src[0], layout.lod);
}

uint32_t EmitImageDrefF32(EmitterState* state, const IR::Instruction& inst,
                          const ImageSampleLayout& layout) {
	if (layout.dref == NoImageComponent || inst.memory.image_address_components <= layout.dref) {
		return EmitZeroF32(state);
	}
	return EmitImageAddressFloatLoad(state, inst, inst.src[0], layout.dref);
}

uint32_t EmitImageBiasF32(EmitterState* state, const IR::Instruction& inst,
                          const ImageSampleLayout& layout) {
	if (layout.bias == NoImageComponent || inst.memory.image_address_components <= layout.bias) {
		return EmitZeroF32(state);
	}
	return EmitImageAddressFloatLoad(state, inst, inst.src[0], layout.bias);
}

uint32_t EmitImageGradientF32(EmitterState* state, const IR::Instruction& inst,
                              uint32_t first_component) {
	const auto x = inst.memory.image_address_components > first_component
	                   ? EmitImageAddressFloatLoad(state, inst, inst.src[0], first_component)
	                   : EmitZeroF32(state);
	const auto y = inst.memory.image_address_components > first_component + 1u
	                   ? EmitImageAddressFloatLoad(state, inst, inst.src[0], first_component + 1u)
	                   : EmitZeroF32(state);
	const auto grad = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec2_float_type, grad, x, y});
	return grad;
}

uint32_t EmitImagePackedOffset2I32(EmitterState* state, const IR::Instruction& inst,
                                   const ImageSampleLayout& layout) {
	if (layout.offset == NoImageComponent ||
	    inst.memory.image_address_components <= layout.offset) {
		const auto zero = ConstantI32(state, 0);
		const auto ret  = state->builder.AllocateId();
		state->builder.AddFunction({OpCompositeConstruct, state->vec2_int_type, ret, zero, zero});
		return ret;
	}

	const auto packed_bits = EmitImageAddressValueLoad(state, inst, inst.src[0], layout.offset);
	const auto packed_i32  = state->builder.AllocateId();
	const auto offset_x    = state->builder.AllocateId();
	const auto offset_y    = state->builder.AllocateId();
	const auto offset      = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, packed_i32, packed_bits});
	state->builder.AddFunction({OpBitFieldSExtract, state->int_type, offset_x, packed_i32,
	                            ConstantI32(state, 0), ConstantI32(state, 6)});
	state->builder.AddFunction({OpBitFieldSExtract, state->int_type, offset_y, packed_i32,
	                            ConstantI32(state, 8), ConstantI32(state, 6)});
	state->builder.AddFunction(
	    {OpCompositeConstruct, state->vec2_int_type, offset, offset_x, offset_y});
	return offset;
}

uint32_t EmitImageOffsetCoordF32(EmitterState* state, const IR::Instruction& inst,
                                 const ImageSampleLayout& layout, uint32_t sampled_image,
                                 uint32_t coord, ImageViewKind view) {
	if (!HasImageSampleFlag(inst, Decoder::ImageSampleFlagOffset)) {
		return coord;
	}

	const auto offset_i32 = EmitImagePackedOffset2I32(state, inst, layout);
	const auto offset_f32 = state->builder.AllocateId();
	const auto image      = state->builder.AllocateId();
	const auto size_u32   = state->builder.AllocateId();
	const auto size_xy_u32 =
	    ImageViewCoordinateComponents(view) == 3u ? state->builder.AllocateId() : size_u32;
	const auto size_f32   = state->builder.AllocateId();
	const auto normalized = state->builder.AllocateId();
	const auto adjusted   = state->builder.AllocateId();
	state->builder.AddFunction({OpConvertSToF, state->vec2_float_type, offset_f32, offset_i32});
	state->builder.AddFunction(
	    {OpImage, ImageViewImageType(*state, view, inst.memory.kind == IR::ResourceKind::ImageUint),
	     image, sampled_image});
	state->builder.AddFunction({OpImageQuerySizeLod, ImageViewSizeType(*state, view), size_u32,
	                            image, ConstantU32(state, 0)});
	if (ImageViewCoordinateComponents(view) == 3u) {
		const auto width  = state->builder.AllocateId();
		const auto height = state->builder.AllocateId();
		state->builder.AddFunction({OpCompositeExtract, state->uint_type, width, size_u32, 0});
		state->builder.AddFunction({OpCompositeExtract, state->uint_type, height, size_u32, 1});
		state->builder.AddFunction(
		    {OpCompositeConstruct, state->vec2_uint_type, size_xy_u32, width, height});
	}
	state->builder.AddFunction({OpConvertUToF, state->vec2_float_type, size_f32, size_xy_u32});
	state->builder.AddFunction({OpFDiv, state->vec2_float_type, normalized, offset_f32, size_f32});
	if (ImageViewCoordinateComponents(view) == 2u) {
		state->builder.AddFunction({OpFAdd, state->vec2_float_type, adjusted, coord, normalized});
		return adjusted;
	}

	const auto x            = state->builder.AllocateId();
	const auto y            = state->builder.AllocateId();
	const auto z            = state->builder.AllocateId();
	const auto normalized_x = state->builder.AllocateId();
	const auto normalized_y = state->builder.AllocateId();
	const auto adjusted_x   = state->builder.AllocateId();
	const auto adjusted_y   = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeExtract, state->float_type, x, coord, 0});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, y, coord, 1});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, z, coord, 2});
	state->builder.AddFunction(
	    {OpCompositeExtract, state->float_type, normalized_x, normalized, 0});
	state->builder.AddFunction(
	    {OpCompositeExtract, state->float_type, normalized_y, normalized, 1});
	state->builder.AddFunction({OpFAdd, state->float_type, adjusted_x, x, normalized_x});
	state->builder.AddFunction({OpFAdd, state->float_type, adjusted_y, y, normalized_y});
	state->builder.AddFunction(
	    {OpCompositeConstruct, state->vec3_float_type, adjusted, adjusted_x, adjusted_y, z});
	return adjusted;
}

uint32_t EmitImageCoordU32(EmitterState* state, const IR::Instruction& inst, ImageViewKind view) {
	const auto x     = EmitImageAddressValueLoad(state, inst, inst.src[1], 0);
	const auto y     = inst.memory.image_address_components > 1u
	                       ? EmitImageAddressValueLoad(state, inst, inst.src[1], 1)
	                       : ConstantU32(state, 0);
	const auto coord = state->builder.AllocateId();
	if (ImageViewCoordinateComponents(view) == 3u) {
		const auto z = inst.memory.image_address_components > 2u
		                   ? EmitImageAddressValueLoad(state, inst, inst.src[1], 2)
		                   : ConstantU32(state, 0);
		state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, coord, x, y, z});
	} else {
		state->builder.AddFunction({OpCompositeConstruct, state->vec2_uint_type, coord, x, y});
	}
	return coord;
}

uint32_t EmitImageLoadCoordU32(EmitterState* state, const IR::Instruction& inst,
                               ImageViewKind view) {
	const auto x     = EmitImageAddressValueLoad(state, inst, inst.src[0], 0);
	const auto y     = inst.memory.image_address_components > 1u
	                       ? EmitImageAddressValueLoad(state, inst, inst.src[0], 1)
	                       : ConstantU32(state, 0);
	const auto coord = state->builder.AllocateId();
	if (ImageViewCoordinateComponents(view) == 3u) {
		const auto z = inst.memory.image_address_components > 2u
		                   ? EmitImageAddressValueLoad(state, inst, inst.src[0], 2)
		                   : ConstantU32(state, 0);
		state->builder.AddFunction({OpCompositeConstruct, state->vec3_uint_type, coord, x, y, z});
	} else {
		state->builder.AddFunction({OpCompositeConstruct, state->vec2_uint_type, coord, x, y});
	}
	return coord;
}

uint32_t EmitImageMipLodU32(EmitterState* state, const IR::Instruction& inst,
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

uint32_t EmitImageQueryCoordF32(EmitterState* state, const IR::Instruction& inst,
                                ImageViewKind view) {
	const auto x     = EmitImageAddressFloatLoad(state, inst, inst.src[0], 0);
	const auto y     = inst.memory.image_address_components > 1u
	                       ? EmitImageAddressFloatLoad(state, inst, inst.src[0], 1)
	                       : EmitZeroF32(state);
	const auto coord = state->builder.AllocateId();
	if (ImageViewCoordinateComponents(view) == 3u) {
		const auto z = inst.memory.image_address_components > 2u
		                   ? EmitImageAddressFloatLoad(state, inst, inst.src[0], 2)
		                   : EmitZeroF32(state);
		state->builder.AddFunction({OpCompositeConstruct, state->vec3_float_type, coord, x, y, z});
	} else {
		state->builder.AddFunction({OpCompositeConstruct, state->vec2_float_type, coord, x, y});
	}
	return coord;
}

uint32_t DmaskComponentIndex(uint32_t dmask, uint32_t component) {
	uint32_t index = 0;
	for (uint32_t i = 0; i < component; i++) {
		index += (dmask >> i) & 1u;
	}
	return index;
}

uint32_t EmitImageStoreComponentF32(EmitterState* state, const IR::Instruction& inst,
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
	if (state.program == nullptr || inst.memory.resource >= state.program->info.images.size()) {
		EXIT("storage image instruction has no specialized swizzle\n");
	}
	return state.program->info.images[inst.memory.resource].storage_swizzle;
}

uint32_t EmitImageStoreTexelF32(EmitterState* state, const IR::Instruction& inst) {
	const auto swizzle         = StorageImageSwizzle(*state, inst);
	const auto component_value = [&](uint32_t component) {
		const auto source = InverseStorageSwizzleComponent(swizzle, component);
		return source < 4u ? EmitImageStoreComponentF32(state, inst, source) : EmitZeroF32(state);
	};
	const auto x     = component_value(0);
	const auto y     = component_value(1);
	const auto z     = component_value(2);
	const auto w     = component_value(3);
	const auto texel = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec4_float_type, texel, x, y, z, w});
	return texel;
}

uint32_t EmitImageStoreComponentU32(EmitterState* state, const IR::Instruction& inst,
                                    uint32_t component) {
	const auto dmask = inst.memory.dmask != 0 ? inst.memory.dmask : 1u;
	if (((dmask >> component) & 1u) == 0) {
		return ConstantU32(state, 0);
	}
	return EmitSequentialValueLoad(state, inst.src[0], DmaskComponentIndex(dmask, component));
}

uint32_t EmitImageStoreTexelU32(EmitterState* state, const IR::Instruction& inst) {
	const auto swizzle         = StorageImageSwizzle(*state, inst);
	const auto component_value = [&](uint32_t component) {
		const auto source = InverseStorageSwizzleComponent(swizzle, component);
		return source < 4u ? EmitImageStoreComponentU32(state, inst, source)
		                   : ConstantU32(state, 0);
	};
	const auto x     = component_value(0);
	const auto y     = component_value(1);
	const auto z     = component_value(2);
	const auto w     = component_value(3);
	const auto texel = state->builder.AllocateId();
	state->builder.AddFunction({OpCompositeConstruct, state->vec4_uint_type, texel, x, y, z, w});
	return texel;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
