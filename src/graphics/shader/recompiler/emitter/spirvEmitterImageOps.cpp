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

uint32_t SamplerCompareFunc(const EmitterState& state, const IR::Instruction& inst) {
	if (inst.memory.sampler >= state.resources.samplers.size()) {
		return 3u;
	}
	const auto& sampler = state.resources.samplers[inst.memory.sampler];
	if (sampler.dword_count == 0) {
		return 3u;
	}
	return (sampler.dwords[0] >> 12u) & 0x7u;
}

bool ImageUsesAluDepthCompare(const EmitterState& state, const IR::Instruction& inst) {
	if (inst.memory.resource < state.program.info.images.size() &&
	    state.program.info.images[inst.memory.resource].alu_depth_compare) {
		return true;
	}
	// Color SAMPLE_C: only compare when the guest sampler actually has a depth-compare op.
	// zfunc 0 (Never) is treated as a regular color sample — bitmap fonts use SAMPLE_C with
	// a non-comparison sampler and expect the texel, not 0/1 coverage.
	if (inst.memory.sampler >= state.resources.samplers.size()) {
		return false;
	}
	const auto& sampler = state.resources.samplers[inst.memory.sampler];
	if (sampler.dword_count == 0) {
		return false;
	}
	return ((sampler.dwords[0] >> 12u) & 0x7u) != 0u;
}

uint32_t EmitAluDepthCompareF32(EmitterState& state, uint32_t sampled, uint32_t dref,
                                uint32_t compare_func) {
	const auto one  = ConstantF32Value(state, 1.0f);
	const auto zero = EmitZeroF32(state);
	switch (compare_func) {
		case 0: return zero;
		case 7: return one;
		default: break;
	}

	uint32_t opcode = OpFOrdLessThanEqual;
	switch (compare_func) {
		case 1: opcode = OpFOrdLessThan; break;
		case 2: opcode = OpFOrdEqual; break;
		case 3: opcode = OpFOrdLessThanEqual; break;
		case 4: opcode = OpFOrdGreaterThan; break;
		case 5: opcode = OpFOrdNotEqual; break;
		case 6: opcode = OpFOrdGreaterThanEqual; break;
		default: break;
	}

	const auto pass = state.builder.AllocateId();
	state.builder.AddFunction({opcode, state.bool_type, pass, sampled, dref});
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, state.float_type, result, pass, one, zero});
	return result;
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
	// GCN IMAGE_SAMPLE_C compares the sampled red/depth channel in the shader. Host sampled
	// views are color formats (including depth sampled as R16/R32), which do not support
	// VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT, so Vulkan Dref ops are illegal.
	return ImageSampleNeedsExplicitLod(state, inst) ? OpImageSampleExplicitLod
	                                                : OpImageSampleImplicitLod;
}

void EmitImageSample(EmitterState& state, const IR::Instruction& inst) {
	const auto view          = SampledImageViewKind(state, inst.memory, inst.pc);
	const auto sampled_image = MakeSampledImage(state, inst.memory, inst.pc, view);

	const auto            layout       = MakeImageSampleLayout(inst, view);
	const auto            base_coord   = EmitImageCoordF32(state, inst, layout, view);
	const auto            sample       = state.builder.AllocateId();
	const auto            dref         = HasImageSampleFlag(inst, Decoder::ImageSampleFlagCompare);
	const bool            integer      = inst.memory.kind == IR::ResourceKind::ImageUint;
	const auto            result_type  = integer ? state.vec4_uint_type : state.vec4_float_type;
	const auto            explicit_lod = ImageSampleNeedsExplicitLod(state, inst);
	const auto            opcode       = ImageSampleOpcode(state, inst);
	std::vector<uint32_t> words        = {opcode, result_type, sample, sampled_image, base_coord};
	AddImageSampleOperands(state, inst, layout, explicit_lod, words);
	state.builder.AddFunction(words);

	const bool alu_compare =
	    dref && !integer && ImageUsesAluDepthCompare(state, inst);
	if (!alu_compare) {
		EmitImageSampleResult(state, inst, sample, false, integer);
		return;
	}

	const auto red = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeExtract, state.float_type, red, sample, 0});
	const auto compared =
	    EmitAluDepthCompareF32(state, red, EmitImageDrefF32(state, inst, layout),
	                           SamplerCompareFunc(state, inst));
	EmitImageSampleResult(state, inst, compared, true, false);
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

	const auto            layout       = MakeImageSampleLayout(inst, view);
	const auto            coord        = EmitImageCoordF32(state, inst, layout, view);
	const auto            texels       = state.builder.AllocateId();
	const auto            dref         = HasImageSampleFlag(inst, Decoder::ImageSampleFlagCompare);
	const bool            integer      = inst.memory.kind == IR::ResourceKind::ImageUint;
	const auto            result_type  = integer ? state.vec4_uint_type : state.vec4_float_type;
	const auto            component    = dref && ImageUsesAluDepthCompare(state, inst)
	                                         ? 0u
	                                         : ImageGatherComponent(inst.memory.dmask);
	std::vector<uint32_t> words = {OpImageGather, result_type, texels, sampled_image, coord,
	                               ConstantU32(state, component)};
	AddImageGatherOperands(state, inst, layout, view, words);
	state.builder.AddFunction(words);

	const bool alu_compare =
	    dref && !integer && ImageUsesAluDepthCompare(state, inst);
	const auto dref_value   = alu_compare ? EmitImageDrefF32(state, inst, layout) : 0;
	const auto compare_func = alu_compare ? SamplerCompareFunc(state, inst) : 0;
	for (uint32_t i = 0; i < inst.memory.data_dwords; i++) {
		const auto gathered = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeExtract, integer ? state.uint_type : state.float_type,
		                           gathered, texels, i});
		const auto compared =
		    alu_compare ? EmitAluDepthCompareF32(state, gathered, dref_value, compare_func)
		                : gathered;
		const auto bits = integer ? compared : state.builder.AllocateId();
		if (!integer) {
			state.builder.AddFunction({OpBitcast, state.uint_type, bits, compared});
		}
		EmitStoreU32(state, OffsetRegisterOperand(inst.dst, i), bits);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
