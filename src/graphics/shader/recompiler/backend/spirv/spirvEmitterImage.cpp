#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"
#include "graphics/shader/recompiler/frontend/decode/ImageOps.h"

#include <algorithm>
#include <bit>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t DmaskComponentIndex(uint32_t dmask, uint32_t component) {
	uint32_t index = 0;
	for (uint32_t i = 0; i < component; i++) {
		index += (dmask >> i) & 1u;
	}
	return index;
}

uint32_t DmaskComponent(uint32_t dmask, uint32_t index) {
	for (uint32_t component = 0; component < 4u; component++) {
		if (((dmask >> component) & 1u) != 0u && index-- == 0u) return component;
	}
	return 0u;
}

uint32_t ImageGatherComponent(uint32_t dmask) {
	switch (dmask) {
		case 0x2u: return 1;
		case 0x4u: return 2;
		case 0x8u: return 3;
		default: return 0;
	}
}

bool HasFlag(const IR::MemoryInfo& mem, uint32_t flag) {
	return (mem.image_sample_flags & flag) != 0u;
}

uint32_t AddressU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                    uint32_t component) {
	const auto layout = Decoder::ImageAddressComponentLayout(mem.image_sample_flags, component);
	const auto packed = layout.bit_offset / 32u;
	if (packed >= address.NumArgs()) return ConstantU32(ctx.state, 0);
	auto value = ctx.Def(address.Arg(packed));
	if (layout.bit_width == 16u) {
		if ((layout.bit_offset & 31u) != 0u) {
			value = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), value,
			               ConstantU32(ctx.state, 16));
		}
		value = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), value,
		               ConstantU32(ctx.state, 0xffffu));
	}
	return value;
}

uint32_t AddressF32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                    uint32_t component) {
	const auto value = AddressU32(ctx, mem, address, component);
	return Decoder::ImageAddressComponentLayout(mem.image_sample_flags, component).bit_width == 16u
	           ? EmitF16BitsToF32(ctx.state, value)
	           : Unary(ctx.state, spv::OpBitcast, TypeF32(ctx.state), value);
}

ImageSampleLayout Layout(const IR::MemoryInfo& mem) {
	ImageSampleLayout layout;
	uint32_t          cursor = 0;
	const auto&       info   = ImageDimensionInfoFor(mem.image_dimension);
	if (HasFlag(mem, Decoder::ImageSampleFlagOffset)) layout.offset = cursor++;
	if (HasFlag(mem, Decoder::ImageSampleFlagBias)) layout.bias = cursor++;
	if (HasFlag(mem, Decoder::ImageSampleFlagCompare)) layout.dref = cursor++;
	if (HasFlag(mem, Decoder::ImageSampleFlagDerivative)) {
		layout.grad_x = cursor;
		cursor += info.spatial_components;
		layout.grad_y = cursor;
		cursor += info.spatial_components;
	}
	layout.coord = cursor;
	cursor += info.coordinate_components;
	if (HasFlag(mem, Decoder::ImageSampleFlagLod)) layout.lod = cursor++;
	return layout;
}

uint32_t ZeroF32(EmitterState& state) {
	return ConstantF32(state, 0);
}

uint32_t GatherMip(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                   const IR::Inst& address, const ImageSampleLayout& layout, uint32_t mip_count) {
	auto& state = ctx.state;
	const auto Extract = [&](uint32_t word, uint32_t first, uint32_t count) {
		return EmitBitFieldUExtract(state, word, ConstantU32(state, first),
		                           ConstantU32(state, count));
	};
	const auto LodFixed = [&](uint32_t word, uint32_t first) {
		return Binary(state, spv::OpFMul, TypeF32(state),
		              Unary(state, spv::OpConvertUToF, TypeF32(state), Extract(word, first, 12)),
		              ConstantF32Value(state, 1.0f / 256.0f));
	};
	const auto sampler_control = ctx.Arg(inst, 6);
	const auto preclamp = Binary(state, spv::OpINotEqual, TypeBool(state),
	                            Extract(sampler_control, 28, 1), ConstantU32(state, 0));
	const auto half = ConstantF32Value(state, 0.5f);
	const auto zero = ZeroF32(state);
	const auto base = Unary(state, spv::OpConvertUToF, TypeF32(state),
	                        Extract(ctx.Arg(inst, 4), 12, 4));
	const auto minimum = Binary(state, spv::OpFSub, TypeF32(state),
	                            LodFixed(ctx.Arg(inst, 3), 8), base);
	const auto last = ConstantF32Value(state, static_cast<float>(mip_count - 1u));
	auto lod = Binary(state, spv::OpFAdd, TypeF32(state),
	                  AddressF32(ctx, mem, address, layout.lod),
	                  Select(state, TypeF32(state), preclamp, half, zero));
	lod = EmitGlsl<GLSLstd450FClamp, IR::Type::F32>(
	    state, lod, LodFixed(ctx.Arg(inst, 5), 0), LodFixed(ctx.Arg(inst, 5), 12));
	// T# minimum LOD is in physical mip space and applies after the S# view-relative clamp.
	lod = EmitGlsl<GLSLstd450FMax, IR::Type::F32>(state, lod, minimum);
	lod = EmitGlsl<GLSLstd450FClamp, IR::Type::F32>(state, lod, zero, last);
	lod = Binary(state, spv::OpFAdd, TypeF32(state), lod,
	             Select(state, TypeF32(state), preclamp, zero, half));
	// The clamped value is nonnegative, so conversion performs the integer mip rounding.
	const auto mip = EmitUMin32(state, Unary(state, spv::OpConvertFToU, TypeU32(state), lod),
	                           ConstantU32(state, mip_count - 1u));
	const auto mip_none = Binary(state, spv::OpIEqual, TypeBool(state),
	                            Extract(sampler_control, 26, 2), ConstantU32(state, 0));
	return Select(state, TypeU32(state), mip_none, ConstantU32(state, 0), mip);
}

uint32_t CubeAxis(EmitterState& state, uint32_t value) {
	return Binary(state, spv::OpFSub, TypeF32(state), value, ConstantF32(state, 0x3f800000u));
}

uint32_t CubeLayer(EmitterState& state, uint32_t value) {
	const auto guest = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertFToU, TypeU32(state), guest, value);
	const auto padding = Binary(
	    state, spv::OpShiftLeftLogical, TypeU32(state),
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), guest, ConstantU32(state, 3)),
	    ConstantU32(state, 1));
	const auto host   = Binary(state, spv::OpISub, TypeU32(state), guest, padding);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertUToF, TypeF32(state), result, host);
	return result;
}

// The instruction's dimension supplies the coordinates; a candidate of another dimension reads
// the missing ones as zero, never the operands that follow the coordinates.
uint32_t SuppliedCoordinates(const IR::MemoryInfo& mem, uint32_t first) {
	const auto supplied = ImageDimensionInfoFor(mem.image_dimension).coordinate_components;
	return std::min(
	    supplied, mem.image_address_components > first ? mem.image_address_components - first : 0u);
}

uint32_t CoordF32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                  uint32_t first, uint32_t components, bool cube = false) {
	const auto supplied = SuppliedCoordinates(mem, first);
	const auto read     = [&](uint32_t index) {
		return index < supplied ? AddressF32(ctx, mem, address, first + index) : ZeroF32(ctx.state);
	};
	auto x = AddressF32(ctx, mem, address, first);
	if (components == 1u) return x;
	auto y = read(1u);
	if (cube) {
		x = CubeAxis(ctx.state, x);
		y = CubeAxis(ctx.state, y);
	}
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		auto z = read(2u);
		if (cube) z = CubeLayer(ctx.state, z);
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(ctx.state, 3),
		                              result, x, y, z);
	} else {
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(ctx.state, 2),
		                              result, x, y);
	}
	return result;
}

uint32_t CoordU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                  ImageDimension dimension) {
	const auto components = ImageDimensionInfoFor(dimension).coordinate_components;
	const auto supplied   = SuppliedCoordinates(mem, 0u);
	const auto read       = [&](uint32_t index) {
		return index < supplied ? AddressU32(ctx, mem, address, index) : ConstantU32(ctx.state, 0);
	};
	const auto x = AddressU32(ctx, mem, address, 0);
	if (components == 1u) return x;
	const auto y      = read(1u);
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		const auto z = read(2u);
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 3),
		                              result, x, y, z);
	} else {
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 2),
		                              result, x, y);
	}
	return result;
}

uint32_t LodU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                ImageDimension dimension) {
	const auto component = ImageDimensionInfoFor(dimension).coordinate_components;
	return mem.image_has_mip && mem.image_address_components > component
	           ? AddressU32(ctx, mem, address, component)
	           : ConstantU32(ctx.state, 0);
}

uint32_t FloatBits(ValueEmitContext& ctx, uint32_t value) {
	return Unary(ctx.state, spv::OpBitcast, TypeU32(ctx.state), value);
}

uint32_t SampledComponentBits(ValueEmitContext& ctx, uint32_t value,
                              Prospero::TextureNumericClass numeric_class) {
	if (numeric_class == Prospero::TextureNumericClass::Uint) {
		return value;
	}
	return Unary(ctx.state, spv::OpBitcast, TypeU32(ctx.state), value);
}

uint32_t SampledComponentZero(EmitterState& state, Prospero::TextureNumericClass numeric_class) {
	switch (numeric_class) {
		case Prospero::TextureNumericClass::Float: return ZeroF32(state);
		case Prospero::TextureNumericClass::Uint: return ConstantU32(state, 0);
		case Prospero::TextureNumericClass::Sint: return ConstantI32(state, 0);
		case Prospero::TextureNumericClass::Unsupported: break;
	}
	EXIT("invalid sampled image numeric class");
}

uint32_t ResultVector(ValueEmitContext& ctx, uint32_t value,
                      Prospero::TextureNumericClass numeric_class, bool dref,
                      const IR::MemoryInfo& mem, bool gather = false) {
	auto value_class = numeric_class;
	if (dref) {
		value_class = Prospero::TextureNumericClass::Float;
	}
	const bool integer = value_class == Prospero::TextureNumericClass::Uint ||
	                     value_class == Prospero::TextureNumericClass::Sint;
	if (mem.data_bits == 16u) {
		uint32_t   packed[4] = {ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0),
		                        ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0)};
		const auto scalar    = [&](uint32_t index) {
			if (dref) return value;
			const auto component = gather ? index : DmaskComponent(mem.dmask, index);
			const auto result    = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpCompositeExtract,
			                              ImageScalarType(ctx.state, value_class), result, value,
			                              component);
			return result;
		};
		for (uint32_t word = 0; word < mem.data_dwords; word++) {
			const auto low_index  = word * 2u;
			const auto high_index = low_index + 1u;
			const auto low        = scalar(low_index);
			const auto high       = high_index < mem.component_count
			                            ? scalar(high_index)
			                            : SampledComponentZero(ctx.state, value_class);
			if (integer) {
				const auto low_bits  = SampledComponentBits(ctx, low, value_class);
				const auto high_bits = SampledComponentBits(ctx, high, value_class);
				const auto mask      = ConstantU32(ctx.state, 0xffffu);
				packed[word] =
				    Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state),
				           Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), low_bits, mask),
				           Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
				                  Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state),
				                         high_bits, mask),
				                  ConstantU32(ctx.state, 16u)));
			} else {
				const auto pair = ctx.state.builder.AllocateId();
				ctx.state.builder.AddFunction(spv::OpCompositeConstruct,
				                              TypeF32Vector(ctx.state, 2), pair, low, high);
				packed[word] = ctx.state.builder.AllocateId();
				ctx.state.builder.AddFunction(spv::OpExtInst, TypeU32(ctx.state), packed[word],
				                              GlslStd450(ctx.state), GLSLstd450PackHalf2x16, pair);
			}
		}
		const auto result = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 4),
		                              result, packed[0], packed[1], packed[2], packed[3]);
		return result;
	}
	uint32_t component[4] {};
	for (uint32_t index = 0; index < 4u; index++) {
		if (dref) {
			component[index] = index == 0u ? FloatBits(ctx, value) : ConstantU32(ctx.state, 0);
			continue;
		}
		const auto scalar = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(
		    spv::OpCompositeExtract, ImageScalarType(ctx.state, value_class), scalar, value, index);
		component[index] = SampledComponentBits(ctx, scalar, value_class);
	}
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 4), result,
	                              component[0], component[1], component[2], component[3]);
	return result;
}

uint32_t QueryDimensions(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                         const IR::Inst& address) {
	const auto  dimension = ctx.state.program.info.images.at(mem.resource).dimension;
	const auto& info      = ImageDimensionInfoFor(dimension);
	const auto  image     = LoadImageDescriptor(ctx.state, mem.resource);
	const auto  size      = ctx.state.builder.AllocateId();
	if (info.multisampled != 0u) {
		ctx.state.builder.AddFunction(spv::OpImageQuerySize,
		                              ImageViewSizeType(ctx.state, dimension), size, image);
	} else {
		ctx.state.builder.AddFunction(spv::OpImageQuerySizeLod,
		                              ImageViewSizeType(ctx.state, dimension), size, image,
		                              AddressU32(ctx, mem, address, 0));
	}
	const auto components = info.coordinate_components;
	uint32_t   result[4]  = {ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0),
	                         ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0)};
	if (components == 1u) {
		result[0] = size;
	} else {
		for (uint32_t index = 0; index < components; index++) {
			result[index] = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state),
			                              result[index], size, index);
		}
	}
	if (info.multisampled == 0u) {
		result[3] = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpImageQueryLevels, TypeU32(ctx.state), result[3],
		                              image);
	}
	const auto vector = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 4), vector,
	                              result[0], result[1], result[2], result[3]);
	return vector;
}

uint32_t PackedOffset(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                      const ImageSampleLayout& layout, ImageDimension dimension) {
	const auto components = ImageDimensionInfoFor(dimension).spatial_components;
	const auto zero       = ConstantI32(ctx.state, 0);
	if (layout.offset == NoImageComponent || mem.image_address_components <= layout.offset) {
		if (components == 1u) return zero;
		const auto result = ctx.state.builder.AllocateId();
		if (components == 3u) {
			ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeI32Vector(ctx.state, 3),
			                              result, zero, zero, zero);
		} else {
			ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeI32Vector(ctx.state, 2),
			                              result, zero, zero);
		}
		return result;
	}
	const auto packed    = Unary(ctx.state, spv::OpBitcast, TypeI32(ctx.state),
	                             AddressU32(ctx, mem, address, layout.offset));
	uint32_t   values[3] = {zero, zero, zero};
	for (uint32_t index = 0; index < components; index++) {
		values[index] = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpBitFieldSExtract, TypeI32(ctx.state), values[index],
		                              packed, ConstantU32(ctx.state, index * 8u),
		                              ConstantU32(ctx.state, 6));
	}
	if (components == 1u) return values[0];
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeI32Vector(ctx.state, 3),
		                              result, values[0], values[1], values[2]);
	} else {
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeI32Vector(ctx.state, 2),
		                              result, values[0], values[1]);
	}
	return result;
}

uint32_t HorizontalOffsets(EmitterState& state, ImageDimension dimension) {
	const auto components   = ImageDimensionInfoFor(dimension).spatial_components;
	const auto count        = ConstantU32(state, 4);
	const auto element_type = components == 1u ? TypeI32(state) : TypeI32Vector(state, 2);
	const auto array_type   = state.builder.Type(spv::OpTypeArray, element_type, count);
	uint32_t   offsets[4] {};
	for (uint32_t index = 0; index < 4u; index++) {
		const auto x = ConstantI32(state, static_cast<int32_t>(index) - 1);
		if (components == 1u) {
			offsets[index] = x;
		} else {
			offsets[index] = state.builder.Constant(
			    spv::OpConstantComposite, TypeI32Vector(state, 2), x, ConstantI32(state, 0));
		}
	}
	return state.builder.Constant(spv::OpConstantComposite, array_type, offsets[0], offsets[1],
	                              offsets[2], offsets[3]);
}

uint32_t InverseSwizzle(uint32_t swizzle, uint32_t component) {
	for (uint32_t source = 0; source < 4u; source++) {
		if (((swizzle >> (source * 3u)) & 7u) == 4u + component) return source;
	}
	return UINT32_MAX;
}

Format::BufferFormatInfo ImageConversionFormat(const EmitterState&   state,
                                               const IR::MemoryInfo& mem) {
	const auto format = state.program.info.images[mem.resource].conversion_format;
	if (format == Prospero::BufferFormat::kInvalid) return {};
	const auto info = Format::GetFormatInfo(format);
	EXIT_IF(Prospero::RemapTextureFormat(format) == format || info.component_count == 0u ||
	        info.component_count > 4u);
	EXIT_IF(info.type != Format::ComponentType::Uscaled &&
	        (info.type != Format::ComponentType::Uint || !info.packed_bitfield ||
	         info.byte_size != sizeof(uint32_t)));
	return info;
}

uint32_t ImageGatherSource(const EmitterState& state, const IR::MemoryInfo& mem) {
	const auto component = ImageGatherComponent(mem.dmask);
	const auto info      = ImageConversionFormat(state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return component;
	if (info.packed_bitfield) return 0u;
	const auto selector =
	    (state.program.info.images[mem.resource].shader_swizzle >> (component * 3u)) & 7u;
	return selector >= 4u ? (selector - 4u) % info.component_count : 0u;
}

uint32_t UnpackImageTexel(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t texel) {
	const auto info = ImageConversionFormat(ctx.state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return texel;

	const auto numeric_class = ctx.state.program.info.images[mem.resource].numeric_class;
	const auto scalar_type   = ImageScalarType(ctx.state, numeric_class);
	uint32_t   packed        = 0;
	if (info.packed_bitfield) {
		packed = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpCompositeExtract, scalar_type, packed, texel, 0u);
	}
	uint32_t components[4] {};
	for (uint32_t component = 0; component < info.component_count; component++) {
		components[component] = ctx.state.builder.AllocateId();
		if (info.packed_bitfield) {
			ctx.state.builder.AddFunction(spv::OpBitFieldUExtract, scalar_type,
			                              components[component], packed,
			                              ConstantU32(ctx.state, info.component_bit_offset[component]),
			                              ConstantU32(ctx.state, info.component_bits[component]));
		} else {
			ctx.state.builder.AddFunction(spv::OpCompositeExtract, scalar_type,
			                              components[component], texel, component);
			// UNorm backing preserves the guest's filtering; scaling precedes swizzle constants.
			components[component] = Binary(ctx.state, spv::OpFMul, scalar_type, components[component],
			                               ConstantF32Value(ctx.state, 255.0f));
		}
	}
	for (uint32_t component = info.component_count; component < 4u; component++) {
		components[component] = components[component % info.component_count];
	}

	const auto swizzle = ctx.state.program.info.images[mem.resource].shader_swizzle;
	uint32_t   selected[4] {};
	for (uint32_t component = 0; component < 4u; component++) {
		const auto selector = (swizzle >> (component * 3u)) & 7u;
		if (selector == 1u) {
			selected[component] = info.packed_bitfield ? ConstantU32(ctx.state, 1u)
			                                          : ConstantF32Value(ctx.state, 1.0f);
		} else if (selector >= 4u) {
			selected[component] = components[selector - 4u];
		} else {
			selected[component] = info.packed_bitfield ? ConstantU32(ctx.state, 0u)
			                                          : ZeroF32(ctx.state);
		}
	}
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct,
	                              ImageVectorType(ctx.state, numeric_class, 4), result, selected[0],
	                              selected[1], selected[2], selected[3]);
	return result;
}

uint32_t UnpackImageGather(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t gathered) {
	const auto info = ImageConversionFormat(ctx.state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return gathered;

	const auto component = ImageGatherComponent(mem.dmask);
	const auto selector =
	    (ctx.state.program.info.images[mem.resource].shader_swizzle >> (component * 3u)) & 7u;
	const auto numeric_class = ctx.state.program.info.images[mem.resource].numeric_class;
	const auto vector_type   = ImageVectorType(ctx.state, numeric_class, 4);
	if (selector < 4u) {
		uint32_t value;
		switch (info.type) {
			case Format::ComponentType::Uscaled:
				value = ConstantF32Value(ctx.state, selector == 1u ? 1.0f : 0.0f);
				break;
			default: value = ConstantU32(ctx.state, selector == 1u ? 1u : 0u); break;
		}
		return ctx.state.builder.Constant(spv::OpConstantComposite, vector_type, value, value,
		                                  value, value);
	}
	if (!info.packed_bitfield) {
		const auto scale  = ConstantF32Value(ctx.state, 255.0f);
		const auto scales = ctx.state.builder.Constant(spv::OpConstantComposite, vector_type,
		                                                scale, scale, scale, scale);
		return Binary(ctx.state, spv::OpFMul, vector_type, gathered, scales);
	}

	uint32_t values[4] {};
	for (uint32_t lane = 0; lane < 4u; lane++) {
		const auto packed = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state), packed, gathered,
		                              lane);
		values[lane]        = ctx.state.builder.AllocateId();
		const auto physical = (selector - 4u) % info.component_count;
		ctx.state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(ctx.state), values[lane],
		                              packed,
		                              ConstantU32(ctx.state, info.component_bit_offset[physical]),
		                              ConstantU32(ctx.state, info.component_bits[physical]));
	}
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 4), result,
	                              values[0], values[1], values[2], values[3]);
	return result;
}

uint32_t EmitOneDimensionalGatherLz(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                    uint32_t coord, Prospero::TextureNumericClass numeric_class) {
	auto& state = ctx.state;
	state.builder.RequireCapability(spv::CapabilityImageQuery);
	const auto image = LoadImageDescriptor(state, mem.resource);
	const auto width = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpImageQuerySizeLod, TypeU32(state), width, image,
	                          ConstantU32(state, 0));
	const auto width_f32 = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertUToF, TypeF32(state), width_f32, width);
	const auto left = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32(state), left, GlslStd450(state),
	                          GLSLstd450Floor,
	                          Binary(state, spv::OpFSub, TypeF32(state),
	                                 Binary(state, spv::OpFMul, TypeF32(state), coord, width_f32),
	                                 ConstantF32(state, 0x3f000000u)));

	const auto sampled = MakeSampledImage(state, mem.resource,
	                                     LoadSamplerDescriptor(state, mem.sampler));
	const auto vector_type = ImageVectorType(state, numeric_class, 4);
	const auto scalar_type = ImageScalarType(state, numeric_class);
	const auto component = ImageGatherSource(state, mem);
	uint32_t values[2] {};
	for (uint32_t index = 0; index < 2u; index++) {
		const auto sample_coord =
		    Binary(state, spv::OpFDiv, TypeF32(state),
		           Binary(state, spv::OpFAdd, TypeF32(state), left,
		                  ConstantF32(state, index == 0u ? 0x3f000000u : 0x3fc00000u)),
		           width_f32);
		const auto texel = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpImageSampleExplicitLod, vector_type, texel, sampled,
		                          sample_coord, spv::ImageOperandsLodMask, ZeroF32(state));
		values[index] = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, scalar_type, values[index], texel,
		                          component);
	}
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, vector_type, result, values[0], values[1],
	                          values[1], values[0]);
	return result;
}

uint32_t PackImageTexel(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t texel) {
	const auto info = ImageConversionFormat(ctx.state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return texel;
	EXIT_IF(info.type != Format::ComponentType::Uint || !info.packed_bitfield);

	auto packed = ConstantU32(ctx.state, 0u);
	for (uint32_t component = 0; component < info.component_count; component++) {
		const auto value = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state), value, texel,
		                              component);
		const auto maximum =
		    ConstantU32(ctx.state, info.component_bits[component] == 32u
		                               ? UINT32_MAX
		                               : (1u << info.component_bits[component]) - 1u);
		const auto within =
		    Binary(ctx.state, spv::OpULessThan, TypeBool(ctx.state), value, maximum);
		const auto clamped = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpSelect, TypeU32(ctx.state), clamped, within, value,
		                              maximum);
		const auto shifted =
		    info.component_bit_offset[component] == 0u
		        ? clamped
		        : Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), clamped,
		                 ConstantU32(ctx.state, info.component_bit_offset[component]));
		packed = Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state), packed, shifted);
	}
	const auto zero   = ConstantU32(ctx.state, 0u);
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 4), result,
	                              packed, zero, zero, zero);
	return result;
}

uint32_t StoreTexel(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t data, bool integer) {
	const auto swizzle = ctx.state.program.info.images[mem.resource].shader_swizzle;
	uint32_t   values[4] {};
	const auto dmask = mem.dmask != 0u ? mem.dmask : 1u;
	for (uint32_t component = 0; component < 4u; component++) {
		const auto source = InverseSwizzle(swizzle, component);
		uint32_t   raw    = ConstantU32(ctx.state, 0);
		if (source < 4u && ((dmask >> source) & 1u) != 0u) {
			const auto packed_index = DmaskComponentIndex(dmask, source);
			raw                     = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state), raw, data,
			                              mem.data_bits == 16u ? packed_index / 2u : packed_index);
			if (mem.data_bits == 16u) {
				if ((packed_index & 1u) != 0u) {
					raw = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), raw,
					             ConstantU32(ctx.state, 16u));
				}
				raw = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), raw,
				             ConstantU32(ctx.state, 0xffffu));
			}
		}
		values[component] = integer ? raw
		                    : mem.data_bits == 16u
		                        ? EmitF16BitsToF32(ctx.state, raw)
		                        : Unary(ctx.state, spv::OpBitcast, TypeF32(ctx.state), raw);
	}
	const auto texel = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct,
	                              integer ? TypeU32Vector(ctx.state, 4)
	                                      : TypeF32Vector(ctx.state, 4),
	                              texel, values[0], values[1], values[2], values[3]);
	return PackImageTexel(ctx, mem, texel);
}

template <typename Emit>
uint32_t EmitImageAccess(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t result_type,
                         const Emit& emit) {
	auto& state = ctx.state;
	const auto& mem = ctx.Memory(inst);
	const auto& image = state.program.info.images.at(mem.resource);
	if (image.indirect_root != mem.resource) return emit(mem.resource, 0u);
	const auto* handle = inst.Arg(0).ResolveInstruction();
	const auto* source = image.source < state.program.descriptor_sources.size()
	                         ? &state.program.descriptor_sources[image.source]
	                         : nullptr;
	if (handle == nullptr || source == nullptr || !source->indirect_descriptor.has_value() ||
	    handle->NumArgs() == 0u) {
		ctx.Fail(inst, "has invalid indirect image key provenance");
	}
	const auto key = ctx.Def(handle->Arg(0));
	if (state.flattened_srt_variable == 0 ||
	    image.indirect_resources.size() < 2u) {
		ctx.Fail(inst, "has no indirect image runtime mapping");
	}
	const auto selected = EmitIndirectResourceIndex(
	    state, key, image.indirect_mapping_offset, image.indirect_search_iterations, 0u);
	struct ImageRun {
		uint32_t first;
		uint32_t count;
		uint32_t resource;
		uint32_t slot_bias;
	};
	std::vector<ImageRun> runs;
	for (uint32_t ordinal = 0; ordinal < image.indirect_resources.size(); ++ordinal) {
		const auto resource = image.indirect_resources[ordinal];
		const auto& candidate = state.program.info.images[resource];
		const auto kind = *IR::DescriptorBindingForImage(candidate);
		if (!runs.empty()) {
			auto& run = runs.back();
			const auto& first = state.program.info.images[run.resource];
			const auto next_slot = run.slot_bias + ordinal;
			if (candidate.dimension == first.dimension && candidate.cube == first.cube &&
			    IR::DescriptorBindingForImage(first) == kind &&
			    candidate.mip_count == 1u && first.mip_count == 1u &&
			    (ordinal == 1u || candidate.descriptor_index == next_slot)) {
				// Native roots precede appended children; only the root may have a slot gap.
				if (ordinal == 1u)
					run.slot_bias = candidate.descriptor_index - ordinal;
				++run.count;
				continue;
			}
		}
		runs.push_back({ordinal, 1u, resource,
		                candidate.descriptor_index - ordinal});
	}
	const auto EmitRun = [&](uint32_t index) {
		const auto& run = runs[index];
		if (run.count == 1u) return emit(run.resource, 0u);
		auto slot = Binary(state, spv::OpIAdd, TypeU32(state), selected,
		                   ConstantU32(state, run.slot_bias));
		if (run.first == 0u) {
			const auto root_slot = image.descriptor_index;
			if (root_slot != run.slot_bias) {
				const auto is_root = Binary(state, spv::OpIEqual, TypeBool(state), selected,
				                            ConstantU32(state, 0u));
				const auto root_or_child = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpSelect, TypeU32(state), root_or_child,
				                          is_root, ConstantU32(state, root_slot), slot);
				slot = root_or_child;
			}
		}
		return emit(run.resource, slot);
	};
	auto run_index = ConstantU32(state, 0u);
	for (uint32_t index = 1; index < runs.size(); ++index) {
		const auto at_or_after = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state),
		                               selected, ConstantU32(state, runs[index].first));
		const auto next = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpSelect, TypeU32(state), next, at_or_after,
		                          ConstantU32(state, index), run_index);
		run_index = next;
	}
	return runs.size() == 1u
	           ? EmitRun(0u)
	           : EmitIndexSwitch(state, run_index, static_cast<uint32_t>(runs.size()),
	                             result_type, EmitRun);
}

} // namespace

void EmitImage(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto op         = inst.GetOpcode();
	const auto image_info = IR::ImageOpcodeInfoOf(op);
	auto&       state     = ctx.state;
	const auto& mem       = ctx.Memory(inst);
	const auto  image_arg = inst.Arg(0);
	ctx.ResourceIndex(image_arg, IR::ValueOpcode::GetImageResource);
	const auto& image   = state.program.info.images.at(mem.resource);
	const auto* address = ctx.ImageAddress(inst.Arg(image_info.needs_sampler ? 2 : 1));
	if (op == IR::ValueOpcode::ImageQueryDimensions) {
		state.builder.RequireCapability(spv::CapabilityImageQuery);
		ctx.Define(inst, QueryDimensions(ctx, mem, *address));
		return;
	}
	if (op == IR::ValueOpcode::ImageQueryLod) {
		state.builder.RequireCapability(spv::CapabilityImageQuery);
		const auto dimension = image.dimension;
		const auto sampled = MakeSampledImage(state, mem.resource,
		                                     LoadSamplerDescriptor(state, mem.sampler));
		const auto lod       = state.builder.AllocateId();
		state.builder.AddFunction(
		    spv::OpImageQueryLod, TypeF32Vector(state, 2), lod, sampled,
		    CoordF32(ctx, mem, *address, 0, ImageDimensionInfoFor(dimension).spatial_components,
		             image.cube));
		uint32_t values[4] = {ConstantU32(state, 0), ConstantU32(state, 0), ConstantU32(state, 0),
		                      ConstantU32(state, 0)};
		for (uint32_t index = 0; index < 2u; index++) {
			const auto component = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), component, lod,
			                          index);
			values[index] = FloatBits(ctx, component);
		}
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 4), result,
		                          values[0], values[1], values[2], values[3]);
		ctx.Define(inst, result);
		return;
	}
	if (op == IR::ValueOpcode::ImageRead) {
		const auto numeric_class = image.numeric_class;
		const auto condition = ctx.Arg(inst, 2);
		ctx.Define(
		    inst,
		    EmitValueOrDefaultIfCondition(
		        state, condition, TypeU32Vector(state, 4), ConstantU32CompositeZero(state, 4),
		        [&]() {
			        const auto color = EmitImageAccess(
			            ctx, inst, ImageVectorType(state, numeric_class, 4),
			            [&](uint32_t resource, uint32_t array_index) {
				        const auto& candidate = state.program.info.images[resource];
				        const auto dimension = candidate.dimension;
				        const auto& dimension_info = ImageDimensionInfoFor(dimension);
				        const auto descriptor = LoadImageDescriptor(state, resource, 0u, array_index);
				        const auto fetched = state.builder.AllocateId();
				        const auto coord = CoordU32(ctx, mem, *address, dimension);
				        if (dimension_info.multisampled != 0u) {
					        state.builder.AddFunction(
					            spv::OpImageFetch,
					            ImageVectorType(state, candidate.numeric_class, 4), fetched,
					            descriptor, coord, spv::ImageOperandsSampleMask,
					            AddressU32(ctx, mem, *address,
					                       ImageDimensionInfoFor(mem.image_dimension)
					                           .coordinate_components));
				        } else {
					        state.builder.AddFunction(
					            spv::OpImageFetch, ImageVectorType(state, candidate.numeric_class, 4), fetched,
					            descriptor, coord, spv::ImageOperandsLodMask,
					            LodU32(ctx, mem, *address, mem.image_dimension));
				        }
				        return fetched;
			            });
			        return ResultVector(ctx, UnpackImageTexel(ctx, mem, color), numeric_class,
			                            false, mem);
		        }));
		return;
	}
	if (op == IR::ValueOpcode::ImageWrite) {
		const bool uint_image = image.numeric_class == Prospero::TextureNumericClass::Uint;
		const auto dimension  = image.dimension;
		EmitIfCondition(state, ctx.Arg(inst, 3), [&]() {
			const auto mip_lod =
			    state.program.info.images[mem.resource].mip_mode == IR::ImageMipMode::Dynamic
			        ? LodU32(ctx, mem, *address, dimension)
			        : 0u;
			const auto coord = CoordU32(ctx, mem, *address, dimension);
			const auto texel = StoreTexel(ctx, mem, ctx.Arg(inst, 2), uint_image);
			EmitStorageImageWrite(state, mem.resource, mip_lod, coord, texel);
		});
		return;
	}
	if (op == IR::ValueOpcode::ImageSampleRaw || op == IR::ValueOpcode::ImageGatherRaw) {
		const auto  dimension      = image.dimension;
		const auto& dimension_info = ImageDimensionInfoFor(dimension);
		const auto  layout         = Layout(mem);
		const auto  numeric_class  = image.numeric_class;
		const bool  dref           = HasFlag(mem, Decoder::ImageSampleFlagCompare);
		if (dref && state.program.info.images[mem.resource].conversion_format !=
		                Prospero::BufferFormat::kInvalid) {
			ctx.Fail(inst, "uses depth comparison with a converted image");
			return;
		}
		if (op == IR::ValueOpcode::ImageGatherRaw) {
			const auto coord = CoordF32(ctx, mem, *address, layout.coord,
			                            dimension_info.coordinate_components, image.cube);
			if (dimension == ImageDimension::Dim1D) {
				if (dref || !HasFlag(mem, Decoder::ImageSampleFlagLevelZero) ||
				    HasFlag(mem, Decoder::ImageSampleFlagOffset) ||
				    HasFlag(mem, Decoder::ImageSampleFlagGatherHorizontal)) {
					ctx.Fail(inst, "has an unsupported 1D gather variant");
					return;
				}
				const auto sample = EmitOneDimensionalGatherLz(ctx, mem, coord, numeric_class);
				ctx.Define(inst, ResultVector(ctx, UnpackImageGather(ctx, mem, sample),
				                              numeric_class, false, mem, true));
				return;
			}
			if (dimension == ImageDimension::Dim1DArray) {
				ctx.Fail(inst, "has an unsupported 1D-array gather");
				return;
			}
			const auto result_numeric_class = dref ? Prospero::TextureNumericClass::Float
			                                       : numeric_class;
			const auto result_type = ImageVectorType(state, result_numeric_class, 4);
			uint32_t component_or_dref;
			if (dref) {
				component_or_dref = layout.dref != NoImageComponent
				                        ? AddressF32(ctx, mem, *address, layout.dref)
				                        : ZeroF32(state);
			} else {
				component_or_dref = ConstantU32(state, ImageGatherSource(state, mem));
			}
			uint32_t operand_mask = 0;
			uint32_t offset = 0;
			if (HasFlag(mem, Decoder::ImageSampleFlagGatherHorizontal)) {
				operand_mask = spv::ImageOperandsConstOffsetsMask;
				offset = HorizontalOffsets(state, dimension);
			} else if (layout.offset != NoImageComponent) {
				operand_mask = spv::ImageOperandsOffsetMask;
				offset = PackedOffset(ctx, mem, *address, layout, dimension);
			}
			const auto sampler_id = LoadSamplerDescriptor(state, mem.sampler);
			const auto EmitGather = [&](uint32_t mip) {
				const auto sampled = MakeSampledImage(state, mem.resource, sampler_id, mip);
				const auto sample = state.builder.AllocateId();
				const std::array operands {operand_mask, offset};
				state.builder.AddFunction(
				    dref ? spv::OpImageDrefGather : spv::OpImageGather, result_type,
				    sample, sampled, coord, component_or_dref,
				    std::span(operands).first(operand_mask != 0u ? 2u : 0u));
				return sample;
			};
			const auto sample = image.mip_mode == IR::ImageMipMode::Dynamic && image.mip_count > 1u
			                        ? EmitIndexSwitch(
			                              state, GatherMip(ctx, inst, mem, *address, layout,
			                                               image.mip_count),
			                              image.mip_count, result_type, EmitGather)
			                        : EmitGather(0);
			ctx.Define(inst, ResultVector(ctx, UnpackImageGather(ctx, mem, sample),
			                              result_numeric_class, false, mem, true));
			return;
		}
		const bool explicit_lod = HasFlag(mem, Decoder::ImageSampleFlagDerivative) ||
		                          HasFlag(mem, Decoder::ImageSampleFlagLod) ||
		                          HasFlag(mem, Decoder::ImageSampleFlagLevelZero) ||
		                          state.program.stage != ShaderType::Pixel;
		auto       opcode       = spv::OpImageSampleImplicitLod;
		if (explicit_lod) {
			opcode = dref ? spv::OpImageSampleDrefExplicitLod : spv::OpImageSampleExplicitLod;
		} else if (dref) {
			opcode = spv::OpImageSampleDrefImplicitLod;
		}
		uint32_t result_type = ImageVectorType(state, numeric_class, 4);
		uint32_t dref_value  = 0;
		if (dref) {
			result_type = TypeF32(state);
			dref_value  = ZeroF32(state);
			if (layout.dref != NoImageComponent) {
				dref_value = AddressF32(ctx, mem, *address, layout.dref);
			}
		}
		std::array<uint32_t, 3> operands {};
		size_t operand_count = 0;
		if (HasFlag(mem, Decoder::ImageSampleFlagDerivative)) {
			operands[0] = spv::ImageOperandsGradMask;
			operands[1] = CoordF32(ctx, mem, *address, layout.grad_x, dimension_info.spatial_components);
			operands[2] = CoordF32(ctx, mem, *address, layout.grad_y, dimension_info.spatial_components);
			operand_count = 3;
		} else if (explicit_lod) {
			auto lod = ZeroF32(state);
			if (HasFlag(mem, Decoder::ImageSampleFlagLod) && layout.lod != NoImageComponent) {
				lod = AddressF32(ctx, mem, *address, layout.lod);
			}
			operands[0] = spv::ImageOperandsLodMask;
			operands[1] = lod;
			operand_count = 2;
		} else if (layout.bias != NoImageComponent) {
			operands[0] = spv::ImageOperandsBiasMask;
			operands[1] = AddressF32(ctx, mem, *address, layout.bias);
			operand_count = 2;
		}
		const auto& samplers = state.program.info.samplers.at(mem.sampler).indirect_resources;
		uint32_t sampler_index = 0;
		if (!samplers.empty()) {
			const auto* handle = inst.Arg(1).ResolveInstruction();
			if (handle == nullptr || handle->GetOpcode() != IR::ValueOpcode::GetSamplerResource)
				ctx.Fail(inst, "has invalid finite sampler key provenance");
			sampler_index = EmitIndexSwitch(
			    state, ctx.Def(handle->Arg(0)), static_cast<uint32_t>(samplers.size()),
			    TypeU32(state), [&](uint32_t ordinal) { return ConstantU32(state, samplers[ordinal]); });
		}
		const auto sampler_id = LoadSamplerDescriptor(state, mem.sampler, sampler_index);
		const auto EmitSample = [&](uint32_t resource, uint32_t array_index) {
			const auto& candidate = state.program.info.images[resource];
			const auto coord =
			    CoordF32(ctx, mem, *address, layout.coord,
			             ImageDimensionInfoFor(candidate.dimension).coordinate_components,
			             candidate.cube);
			const auto sampled = MakeSampledImage(state, resource, sampler_id, 0u, array_index,
			                                      sampler_index != 0u);
			const auto sample = state.builder.AllocateId();
			state.builder.AddFunction(opcode, result_type, sample, sampled, coord,
			                          std::span(&dref_value, dref ? 1u : 0u),
			                          std::span<const uint32_t>(operands).first(operand_count));
			return sample;
		};
		auto result = EmitImageAccess(ctx, inst, result_type, EmitSample);
		if (!dref) {
			result = UnpackImageTexel(ctx, mem, result);
		}
		ctx.Define(inst, ResultVector(ctx, result, numeric_class, dref, mem));
		return;
	}
	if (image_info.access == IR::ImageAccess::Atomic) {
		const auto dimension = image.dimension;
		const auto result_type = TypeId(state, inst.GetType());
		const auto zero = image.atomic64 ? ConstantU64(state, 0) : ConstantU32(state, 0);
		ctx.Define(inst, EmitValueOrDefaultIfCondition(
		                     state, ctx.Arg(inst, inst.NumArgs() - 1), result_type, zero, [&]() {
			           const auto pointer      = state.builder.AllocateId();
			           const auto pointer_type =
			               state.builder.Type(spv::OpTypePointer, spv::StorageClassImage,
			                                  image.atomic64 ? TypeU64(state) : TypeU32(state));
			           state.builder.AddFunction(spv::OpImageTexelPointer, pointer_type, pointer,
			                                     ImageDescriptorPointer(state, mem.resource),
			                                     CoordU32(ctx, mem, *address, dimension),
			                                     ConstantU32(state, 0));
			           if (op == IR::ValueOpcode::ImageAtomicFMin32 ||
			               op == IR::ValueOpcode::ImageAtomicFMax32) {
				           return AtomicUpdate(state, pointer, IR::ResourceKind::Image,
				                               [&](uint32_t old) {
					                               return EmitFloatAtomicReplacement(
					                                   state, old, ctx.Arg(inst, 2),
					                                   op == IR::ValueOpcode::ImageAtomicFMax32);
				                               });
			           }
			           const auto old = EmitAtomicOperation(ctx, inst, pointer, spv::ScopeDevice);
			           EmitAtomicMemoryBarrier(state, IR::ResourceKind::Image);
			           return old;
		           }));
		return;
	}
	ctx.Fail(inst, "has no image SPIR-V emitter");
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
