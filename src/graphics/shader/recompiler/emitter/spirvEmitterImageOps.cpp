#include "graphics/shader/recompiler/emitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

namespace {

uint32_t ConstantVec2I32(EmitterState& state, int32_t x, int32_t y) {
	const auto value = state.builder.AllocateId();
	state.builder.AddType({OpConstantComposite, state.vec2_int_type, value, ConstantI32(state, x),
	                       ConstantI32(state, y)});
	return value;
}

uint32_t ConstantImageGatherHorizontalOffsets(EmitterState& state, ImageViewKind view) {
	if (ImageViewSpatialComponents(view) == 1u) {
		const auto type = state.builder.AllocateId();
		state.builder.AddType({OpTypeArray, type, state.int_type, ConstantU32(state, 4)});
		const auto value = state.builder.AllocateId();
		state.builder.AddType({OpConstantComposite, type, value, ConstantI32(state, -1),
		                       ConstantI32(state, 0), ConstantI32(state, 1),
		                       ConstantI32(state, 2)});
		return value;
	}
	const auto offset0 = ConstantVec2I32(state, -1, 0);
	const auto offset1 = ConstantVec2I32(state, 0, 0);
	const auto offset2 = ConstantVec2I32(state, 1, 0);
	const auto offset3 = ConstantVec2I32(state, 2, 0);
	const auto type    = state.builder.AllocateId();
	state.builder.AddType({OpTypeArray, type, state.vec2_int_type, ConstantU32(state, 4)});
	const auto value = state.builder.AllocateId();
	state.builder.AddType({OpConstantComposite, type, value, offset0, offset1, offset2, offset3});
	return value;
}

uint32_t LoadStorageImageDescriptorAtIndex(EmitterState& state, uint32_t resource,
                                           uint32_t array_index, bool uint_image,
                                           ImageViewKind view) {
	const auto  kind        = StorageBindingKind(uint_image, view);
	const auto& descriptors = state.storage_images[StorageImageIndex(uint_image, view)];
	const auto  pointer =
	    DescriptorElementPointer(state, descriptors.pointer_type, descriptors.variable, array_index,
	                             kind, resource, "storage image descriptor array was not emitted");
	const auto image = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, descriptors.image_type, image, pointer});
	return image;
}

} // namespace

uint32_t EmitImageGetResinfoComponent(EmitterState& state, uint32_t image, uint32_t size,
                                      uint32_t component_index, ImageViewKind view) {
	const auto components = ImageViewCoordinateComponents(view);
	switch (component_index) {
		case 0: {
			if (components == 1u) {
				return size;
			}
			const auto width = state.builder.AllocateId();
			state.builder.AddFunction({OpCompositeExtract, state.uint_type, width, size, 0});
			return width;
		}
		case 1: {
			if (components < 2u) {
				return ConstantU32(state, 0);
			}
			const auto height = state.builder.AllocateId();
			state.builder.AddFunction({OpCompositeExtract, state.uint_type, height, size, 1});
			return height;
		}
		case 2:
			if (components == 3u) {
				const auto depth = state.builder.AllocateId();
				state.builder.AddFunction({OpCompositeExtract, state.uint_type, depth, size, 2});
				return depth;
			}
			return ConstantU32(state, 0);
		case 3: {
			const auto levels = state.builder.AllocateId();
			state.builder.AddFunction({OpImageQueryLevels, state.uint_type, levels, image});
			return levels;
		}
		default: return ConstantU32(state, 0);
	}
}

void EmitImageGetResinfo(EmitterState& state, const IR::Instruction& inst) {
	const auto view      = SampledImageViewKind(state, inst.memory, inst.pc);
	const auto image     = LoadSampledImageDescriptor(state, inst.memory, inst.pc, view);
	const auto mip_level = EmitValueLoad(state, inst.src[0]);

	const auto size = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpImageQuerySizeLod, ImageViewSizeType(state, view), size, image, mip_level});

	const auto dmask     = inst.memory.dmask != 0 ? inst.memory.dmask : 1u;
	uint32_t   dst_index = 0;
	for (uint32_t component_index = 0; component_index < 4u; component_index++) {
		if (((dmask >> component_index) & 1u) == 0) {
			continue;
		}
		const auto component =
		    EmitImageGetResinfoComponent(state, image, size, component_index, view);
		EmitStoreU32(state, OffsetRegisterOperand(inst.dst, dst_index++), component);
	}
}

void EmitImageGetLod(EmitterState& state, const IR::Instruction& inst) {
	const auto view          = SampledImageViewKind(state, inst.memory, inst.pc);
	const auto sampled_image = MakeSampledImage(state, inst.memory, inst.pc, view);

	const auto lod = state.builder.AllocateId();
	state.builder.AddFunction({OpImageQueryLod, state.vec2_float_type, lod, sampled_image,
	                           EmitImageQueryCoordF32(state, inst, view)});

	const auto dmask     = inst.memory.dmask != 0 ? inst.memory.dmask : 1u;
	uint32_t   dst_index = 0;
	for (uint32_t component_index = 0; component_index < 2u; component_index++) {
		if (((dmask >> component_index) & 1u) == 0) {
			continue;
		}
		const auto component = state.builder.AllocateId();
		const auto bits      = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpCompositeExtract, state.float_type, component, lod, component_index});
		state.builder.AddFunction({OpBitcast, state.uint_type, bits, component});
		EmitStoreU32(state, OffsetRegisterOperand(inst.dst, dst_index++), bits);
	}
}

void EmitImageLoad(EmitterState& state, const IR::Instruction& inst) {
	const auto view    = SampledImageViewKind(state, inst.memory, inst.pc);
	const auto image   = LoadSampledImageDescriptor(state, inst.memory, inst.pc, view);
	const bool integer = inst.memory.kind == IR::ResourceKind::ImageUint;

	const auto color = state.builder.AllocateId();
	const auto coord = EmitImageLoadCoordU32(state, inst, view);
	if (ImageSpirvMultisampled(view) != 0) {
		const auto sample = EmitImageAddressValueLoad(state, inst, inst.src[0],
		                                              ImageViewCoordinateComponents(view));
		state.builder.AddFunction({OpImageFetch,
		                           integer ? state.vec4_uint_type : state.vec4_float_type, color,
		                           image, coord, ImageOperandsSampleMask, sample});
	} else {
		state.builder.AddFunction(
		    {OpImageFetch, integer ? state.vec4_uint_type : state.vec4_float_type, color, image,
		     coord, ImageOperandsLodMask, EmitImageMipLodU32(state, inst, inst.src[0], view)});
	}

	const auto dmask     = inst.memory.dmask != 0 ? inst.memory.dmask : 1u;
	uint32_t   dst_index = 0;
	for (uint32_t component_index = 0; component_index < 4u; component_index++) {
		if (((dmask >> component_index) & 1u) == 0) {
			continue;
		}
		const auto component = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeExtract, integer ? state.uint_type : state.float_type,
		                           component, color, component_index});
		const auto bits = integer ? component : state.builder.AllocateId();
		if (!integer) {
			state.builder.AddFunction({OpBitcast, state.uint_type, bits, component});
		}
		EmitStoreU32(state, OffsetRegisterOperand(inst.dst, dst_index++), bits);
	}
}

void EmitImageStore(EmitterState& state, const IR::Instruction& inst) {
	const auto uint_image = inst.memory.kind == IR::ResourceKind::StorageImageUint;
	const auto view       = StorageImageViewKind(state, inst.memory, uint_image, inst.pc);
	const auto binding =
	    ResourceForDescriptor(state, StorageBindingKind(uint_image, view), inst.memory.resource);
	const auto image = LoadStorageImageDescriptorAtIndex(state, inst.memory.resource,
	                                                     binding.array_index, uint_image, view);

	state.builder.AddFunction(
	    {OpImageWrite, image, EmitImageCoordU32(state, inst, view),
	     uint_image ? EmitImageStoreTexelU32(state, inst) : EmitImageStoreTexelF32(state, inst)});
}

void EmitImageSampleResult(EmitterState& state, const IR::Instruction& inst, uint32_t sample,
                           bool dref, bool integer) {
	if (dref) {
		const auto bits = state.builder.AllocateId();
		state.builder.AddFunction({OpBitcast, state.uint_type, bits, sample});
		for (uint32_t i = 0; i < inst.memory.data_dwords; i++) {
			EmitStoreU32(state, OffsetRegisterOperand(inst.dst, i), bits);
		}
		return;
	}

	const auto dmask     = inst.memory.dmask != 0 ? inst.memory.dmask : 1u;
	uint32_t   dst_index = 0;
	for (uint32_t component_index = 0; component_index < 4u; component_index++) {
		if (((dmask >> component_index) & 1u) == 0) {
			continue;
		}
		const auto component = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeExtract, integer ? state.uint_type : state.float_type,
		                           component, sample, component_index});
		const auto bits = integer ? component : state.builder.AllocateId();
		if (!integer) {
			state.builder.AddFunction({OpBitcast, state.uint_type, bits, component});
		}
		EmitStoreU32(state, OffsetRegisterOperand(inst.dst, dst_index++), bits);
	}
}

bool ImageSampleNeedsExplicitLod(const EmitterState& state, const IR::Instruction& inst) {
	// RDNA2 plain IMAGE_SAMPLE is derivative-based in pixel shaders. Do not translate it
	// to Lod(0): only _L/_LZ/_D variants supply explicit LOD/gradient information.
	// SPIR-V implicit-lod sampling is fragment-only, so non-pixel stages still need
	// an explicit operand even for the unsuffixed sample opcode.
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagDerivative) ||
	    HasImageSampleFlag(inst, Decoder::ImageSampleFlagLod) ||
	    HasImageSampleFlag(inst, Decoder::ImageSampleFlagLevelZero)) {
		return true;
	}
	return state.stage != ShaderType::Pixel;
}

void AddImageSampleOperands(EmitterState& state, const IR::Instruction& inst,
                            const ImageSampleLayout& layout, bool explicit_lod,
                            std::vector<uint32_t>& words) {
	uint32_t              mask = 0;
	std::vector<uint32_t> operands;

	// Keep the SPIR-V operand class aligned with the RDNA2 opcode suffix. Grad/Lod
	// operands belong to explicit-lod instructions; bias belongs to implicit-lod
	// sampling because it modifies the hardware-computed LOD.
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagDerivative)) {
		mask |= ImageOperandsGradMask;
		const auto view = SampledImageViewKind(state, inst.memory, inst.pc);
		operands.push_back(EmitImageGradientF32(state, inst, layout.grad_x, view));
		operands.push_back(EmitImageGradientF32(state, inst, layout.grad_y, view));
	} else if (explicit_lod) {
		mask |= ImageOperandsLodMask;
		operands.push_back(EmitImageLodF32(state, inst, layout));
	} else if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagBias)) {
		mask |= ImageOperandsBiasMask;
		operands.push_back(EmitImageBiasF32(state, inst, layout));
	}
	if (mask != 0) {
		words.push_back(mask);
		words.insert(words.end(), operands.begin(), operands.end());
	}
}

uint32_t ImageSampleOpcode(const EmitterState& state, const IR::Instruction& inst) {
	const auto flags        = inst.memory.image_sample_flags;
	const bool dref         = (flags & Decoder::ImageSampleFlagCompare) != 0;
	const bool explicit_lod = ImageSampleNeedsExplicitLod(state, inst);
	if (explicit_lod) {
		return dref ? OpImageSampleDrefExplicitLod : OpImageSampleExplicitLod;
	}
	return dref ? OpImageSampleDrefImplicitLod : OpImageSampleImplicitLod;
}

void EmitImageSample(EmitterState& state, const IR::Instruction& inst) {
	const auto view          = SampledImageViewKind(state, inst.memory, inst.pc);
	const auto sampled_image = MakeSampledImage(state, inst.memory, inst.pc, view);

	const auto layout      = MakeImageSampleLayout(inst, view);
	const auto base_coord  = EmitImageCoordF32(state, inst, layout, view);
	const auto sample      = state.builder.AllocateId();
	const auto dref        = HasImageSampleFlag(inst, Decoder::ImageSampleFlagCompare);
	const bool integer     = inst.memory.kind == IR::ResourceKind::ImageUint;
	uint32_t   result_type = state.vec4_float_type;
	if (dref) {
		result_type = state.float_type;
	} else if (integer) {
		result_type = state.vec4_uint_type;
	}
	const auto            explicit_lod = ImageSampleNeedsExplicitLod(state, inst);
	const auto            opcode       = ImageSampleOpcode(state, inst);
	std::vector<uint32_t> words        = {opcode, result_type, sample, sampled_image, base_coord};
	if (dref) {
		words.push_back(EmitImageDrefF32(state, inst, layout));
	}
	AddImageSampleOperands(state, inst, layout, explicit_lod, words);
	state.builder.AddFunction(words);
	EmitImageSampleResult(state, inst, sample, dref, integer);
}

uint32_t ImageGatherComponent(uint32_t dmask) {
	switch (dmask) {
		case 0x2u: return 1;
		case 0x4u: return 2;
		case 0x8u: return 3;
		default: return 0;
	}
}

void AddImageGatherOperands(EmitterState& state, const IR::Instruction& inst,
                            const ImageSampleLayout& layout, ImageViewKind view,
                            std::vector<uint32_t>& words) {
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagGatherHorizontal)) {
		words.push_back(ImageOperandsConstOffsetsMask);
		words.push_back(ConstantImageGatherHorizontalOffsets(state, view));
		return;
	}
	if (HasImageSampleFlag(inst, Decoder::ImageSampleFlagOffset)) {
		words.push_back(ImageOperandsOffsetMask);
		words.push_back(EmitImagePackedOffsetI32(state, inst, layout, view));
	}
}

void EmitImageGather4(EmitterState& state, const IR::Instruction& inst) {
	const auto view          = SampledImageViewKind(state, inst.memory, inst.pc);
	const auto sampled_image = MakeSampledImage(state, inst.memory, inst.pc, view);

	const auto            layout      = MakeImageSampleLayout(inst, view);
	const auto            coord       = EmitImageCoordF32(state, inst, layout, view);
	const auto            texels      = state.builder.AllocateId();
	const auto            dref        = HasImageSampleFlag(inst, Decoder::ImageSampleFlagCompare);
	const bool            integer     = inst.memory.kind == IR::ResourceKind::ImageUint;
	const auto            result_type = integer ? state.vec4_uint_type : state.vec4_float_type;
	std::vector<uint32_t> words =
	    dref ? std::vector<uint32_t> {OpImageDrefGather,
	                                  state.vec4_float_type,
	                                  texels,
	                                  sampled_image,
	                                  coord,
	                                  EmitImageDrefF32(state, inst, layout)}
	         : std::vector<uint32_t> {
	               OpImageGather, result_type,
	               texels,        sampled_image,
	               coord,         ConstantU32(state, ImageGatherComponent(inst.memory.dmask))};
	AddImageGatherOperands(state, inst, layout, view, words);
	state.builder.AddFunction(words);

	for (uint32_t i = 0; i < inst.memory.data_dwords; i++) {
		const auto component = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeExtract, integer ? state.uint_type : state.float_type,
		                           component, texels, i});
		const auto bits = integer ? component : state.builder.AllocateId();
		if (!integer) {
			state.builder.AddFunction({OpBitcast, state.uint_type, bits, component});
		}
		EmitStoreU32(state, OffsetRegisterOperand(inst.dst, i), bits);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
