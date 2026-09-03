#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

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

uint32_t Unary(EmitterState& state, uint32_t opcode, uint32_t type, uint32_t value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({opcode, type, result, value});
	return result;
}

uint32_t Binary(EmitterState& state, uint32_t opcode, uint32_t type, uint32_t lhs, uint32_t rhs) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({opcode, type, result, lhs, rhs});
	return result;
}

bool HasFlag(const IR::MemoryInfo& mem, uint32_t flag) {
	return (mem.image_sample_flags & flag) != 0u;
}

bool UsesA16(const IR::MemoryInfo& mem) {
	return HasFlag(mem, Decoder::ImageSampleFlagA16);
}

bool ComponentUsesA16(const IR::MemoryInfo& mem, uint32_t component) {
	if (!UsesA16(mem)) return false;
	uint32_t cursor = 0;
	if (HasFlag(mem, Decoder::ImageSampleFlagOffset)) {
		if (component == cursor) return false;
		cursor++;
	}
	if (HasFlag(mem, Decoder::ImageSampleFlagBias)) {
		if (component == cursor) return true;
		cursor++;
	}
	if (HasFlag(mem, Decoder::ImageSampleFlagCompare) && component == cursor) return false;
	return true;
}

uint32_t HalfComponent(const IR::MemoryInfo& mem, uint32_t component) {
	if (!UsesA16(mem)) return component * 2u;
	uint32_t half = 0;
	for (uint32_t index = 0; index < component; index++) {
		const auto width = ComponentUsesA16(mem, index) ? 1u : 2u;
		if (width == 2u && (half & 1u) != 0u) half++;
		half += width;
	}
	if (!ComponentUsesA16(mem, component) && (half & 1u) != 0u) half++;
	return half;
}

uint32_t AddressU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                    uint32_t component) {
	const auto half   = HalfComponent(mem, component);
	const auto packed = half / 2u;
	if (packed >= address.NumArgs()) return ConstantU32(ctx.state, 0);
	auto value = ctx.Def(address.Arg(packed));
	if (ComponentUsesA16(mem, component)) {
		if ((half & 1u) != 0u) {
			value = Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state), value,
			               ConstantU32(ctx.state, 16));
		}
		value = Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), value,
		               ConstantU32(ctx.state, 0xffffu));
	}
	return value;
}

uint32_t AddressF32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                    uint32_t component) {
	const auto value = AddressU32(ctx, mem, address, component);
	return ComponentUsesA16(mem, component)
	           ? EmitF16BitsToF32(ctx.state, value)
	           : Unary(ctx.state, OpBitcast, TypeF32(ctx.state), value);
}

ImageSampleLayout Layout(const IR::MemoryInfo& mem, ImageDimension dimension) {
	ImageSampleLayout layout;
	uint32_t          cursor = 0;
	const auto&       info   = ImageDimensionInfoFor(dimension);
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

uint32_t CubeAxis(EmitterState& state, uint32_t value) {
	return Binary(state, OpFSub, TypeF32(state), value, ConstantF32(state, 0x3f800000u));
}

uint32_t CubeLayer(EmitterState& state, uint32_t value) {
	const auto guest = state.builder.AllocateId();
	state.builder.AddFunction({OpConvertFToU, TypeU32(state), guest, value});
	const auto padding =
	    Binary(state, OpShiftLeftLogical, TypeU32(state),
	           Binary(state, OpShiftRightLogical, TypeU32(state), guest, ConstantU32(state, 3)),
	           ConstantU32(state, 1));
	const auto host   = Binary(state, OpISub, TypeU32(state), guest, padding);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpConvertUToF, TypeF32(state), result, host});
	return result;
}

uint32_t CoordF32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                  uint32_t first, uint32_t components) {
	const bool cube = ctx.state.program.info.images.at(mem.resource).cube;
	auto x = AddressF32(ctx, mem, address, first);
	if (components == 1u) return x;
	auto y = mem.image_address_components > first + 1u ? AddressF32(ctx, mem, address, first + 1u)
	                                                   : ZeroF32(ctx.state);
	if (cube) {
		x = CubeAxis(ctx.state, x);
		y = CubeAxis(ctx.state, y);
	}
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		auto z = mem.image_address_components > first + 2u
		             ? AddressF32(ctx, mem, address, first + 2u)
		             : ZeroF32(ctx.state);
		if (cube) z = CubeLayer(ctx.state, z);
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, TypeF32Vector(ctx.state, 3), result, x, y, z});
	} else {
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, TypeF32Vector(ctx.state, 2), result, x, y});
	}
	return result;
}

uint32_t CoordU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                  ImageDimension dimension) {
	const auto components = ImageDimensionInfoFor(dimension).coordinate_components;
	const auto x          = AddressU32(ctx, mem, address, 0);
	if (components == 1u) return x;
	const auto y      = mem.image_address_components > 1u ? AddressU32(ctx, mem, address, 1)
	                                                      : ConstantU32(ctx.state, 0);
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		const auto z = mem.image_address_components > 2u ? AddressU32(ctx, mem, address, 2)
		                                                 : ConstantU32(ctx.state, 0);
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, TypeU32Vector(ctx.state, 3), result, x, y, z});
	} else {
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, TypeU32Vector(ctx.state, 2), result, x, y});
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
	return Unary(ctx.state, OpBitcast, TypeU32(ctx.state), value);
}

uint32_t SampledComponentBits(ValueEmitContext& ctx, uint32_t value,
                              Prospero::TextureNumericClass numeric_class) {
	if (numeric_class == Prospero::TextureNumericClass::Uint) {
		return value;
	}
	return Unary(ctx.state, OpBitcast, TypeU32(ctx.state), value);
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
			ctx.state.builder.AddFunction({OpCompositeExtract,
			                               ImageScalarType(ctx.state, value_class), result, value,
			                               component});
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
				packed[word]         = Binary(
				    ctx.state, OpBitwiseOr, TypeU32(ctx.state),
				    Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), low_bits, mask),
				    Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state),
				           Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), high_bits, mask),
				           ConstantU32(ctx.state, 16u)));
			} else {
				const auto pair = ctx.state.builder.AllocateId();
				ctx.state.builder.AddFunction(
				    {OpCompositeConstruct, TypeF32Vector(ctx.state, 2), pair, low, high});
				packed[word] = ctx.state.builder.AllocateId();
				ctx.state.builder.AddFunction({OpExtInst, TypeU32(ctx.state), packed[word],
				                               GlslStd450(ctx.state), GlslPackHalf2x16, pair});
			}
		}
		const auto result = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(ctx.state, 4), result,
		                               packed[0], packed[1], packed[2], packed[3]});
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
		    {OpCompositeExtract, ImageScalarType(ctx.state, value_class), scalar, value, index});
		component[index] = SampledComponentBits(ctx, scalar, value_class);
	}
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(ctx.state, 4), result,
	                               component[0], component[1], component[2], component[3]});
	return result;
}

uint32_t QueryDimensions(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                         const IR::Inst& address) {
	const auto  dimension = ctx.state.program.info.images.at(mem.resource).dimension;
	const auto& info      = ImageDimensionInfoFor(dimension);
	const auto  image     = LoadSampledImageDescriptor(ctx.state, mem.resource);
	const auto  size      = ctx.state.builder.AllocateId();
	if (info.multisampled != 0u) {
		ctx.state.builder.AddFunction(
		    {OpImageQuerySize, ImageViewSizeType(ctx.state, dimension), size, image});
	} else {
		ctx.state.builder.AddFunction({OpImageQuerySizeLod, ImageViewSizeType(ctx.state, dimension),
		                               size, image, AddressU32(ctx, mem, address, 0)});
	}
	const auto components = info.coordinate_components;
	uint32_t   result[4]  = {ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0),
	                         ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0)};
	if (components == 1u) {
		result[0] = size;
	} else {
		for (uint32_t index = 0; index < components; index++) {
			result[index] = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(
			    {OpCompositeExtract, TypeU32(ctx.state), result[index], size, index});
		}
	}
	if (info.multisampled == 0u) {
		result[3] = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction({OpImageQueryLevels, TypeU32(ctx.state), result[3], image});
	}
	const auto vector = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(ctx.state, 4), vector,
	                               result[0], result[1], result[2], result[3]});
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
			ctx.state.builder.AddFunction(
			    {OpCompositeConstruct, TypeI32Vector(ctx.state, 3), result, zero, zero, zero});
		} else {
			ctx.state.builder.AddFunction(
			    {OpCompositeConstruct, TypeI32Vector(ctx.state, 2), result, zero, zero});
		}
		return result;
	}
	const auto packed    = Unary(ctx.state, OpBitcast, TypeI32(ctx.state),
	                             AddressU32(ctx, mem, address, layout.offset));
	uint32_t   values[3] = {zero, zero, zero};
	for (uint32_t index = 0; index < components; index++) {
		values[index] = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction({OpBitFieldSExtract, TypeI32(ctx.state), values[index],
		                               packed, ConstantU32(ctx.state, index * 8u),
		                               ConstantU32(ctx.state, 6)});
	}
	if (components == 1u) return values[0];
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		ctx.state.builder.AddFunction({OpCompositeConstruct, TypeI32Vector(ctx.state, 3), result,
		                               values[0], values[1], values[2]});
	} else {
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, TypeI32Vector(ctx.state, 2), result, values[0], values[1]});
	}
	return result;
}

uint32_t HorizontalOffsets(EmitterState& state, ImageDimension dimension) {
	const auto components   = ImageDimensionInfoFor(dimension).spatial_components;
	const auto count        = ConstantU32(state, 4);
	const auto element_type = components == 1u ? TypeI32(state) : TypeI32Vector(state, 2);
	const auto array_type   = state.builder.Type(OpTypeArray, {element_type, count});
	uint32_t   offsets[4] {};
	for (uint32_t index = 0; index < 4u; index++) {
		const auto x = ConstantI32(state, static_cast<int32_t>(index) - 1);
		if (components == 1u) {
			offsets[index] = x;
		} else {
			offsets[index] = state.builder.Constant(OpConstantComposite, TypeI32Vector(state, 2),
			                                        {x, ConstantI32(state, 0)});
		}
	}
	return state.builder.Constant(OpConstantComposite, array_type,
	                              {offsets[0], offsets[1], offsets[2], offsets[3]});
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
	EXIT_IF(Prospero::SampledTextureNumericClass(format) != Prospero::TextureNumericClass::Uint ||
	        Prospero::RemapTextureFormat(format) == format ||
	        info.type != Format::ComponentType::Uint || !info.packed_bitfield ||
	        info.byte_size != sizeof(uint32_t) || info.component_count == 0u ||
	        info.component_count > 4u);
	return info;
}

uint32_t UnpackImageTexel(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t texel) {
	const auto info = ImageConversionFormat(ctx.state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return texel;

	const auto packed = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpCompositeExtract, TypeU32(ctx.state), packed, texel, 0u});
	uint32_t components[4] = {ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0),
	                          ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0)};
	for (uint32_t component = 0; component < info.component_count; component++) {
		components[component] = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction({OpBitFieldUExtract, TypeU32(ctx.state),
		                               components[component], packed,
		                               ConstantU32(ctx.state, info.component_bit_offset[component]),
		                               ConstantU32(ctx.state, info.component_bits[component])});
	}
	for (uint32_t component = info.component_count; component < 4u; component++) {
		components[component] = components[component % info.component_count];
	}

	const auto swizzle = ctx.state.program.info.images[mem.resource].shader_swizzle;
	uint32_t   selected[4] {};
	for (uint32_t component = 0; component < 4u; component++) {
		const auto selector = (swizzle >> (component * 3u)) & 7u;
		if (selector == 1u) {
			selected[component] = ConstantU32(ctx.state, 1u);
		} else if (selector >= 4u) {
			selected[component] = components[selector - 4u];
		} else {
			selected[component] = ConstantU32(ctx.state, 0u);
		}
	}
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(ctx.state, 4), result,
	                               selected[0], selected[1], selected[2], selected[3]});
	return result;
}

uint32_t UnpackImageGather(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t gathered) {
	const auto info = ImageConversionFormat(ctx.state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return gathered;

	const auto component = ImageGatherComponent(mem.dmask);
	const auto selector =
	    (ctx.state.program.info.images[mem.resource].shader_swizzle >> (component * 3u)) & 7u;
	if (selector < 4u) {
		const auto value = ConstantU32(ctx.state, selector == 1u ? 1u : 0u);
		return ctx.state.builder.Constant(OpConstantComposite, TypeU32Vector(ctx.state, 4),
		                                  {value, value, value, value});
	}

	uint32_t values[4] {};
	for (uint32_t lane = 0; lane < 4u; lane++) {
		const auto packed = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(
		    {OpCompositeExtract, TypeU32(ctx.state), packed, gathered, lane});
		values[lane]        = ctx.state.builder.AllocateId();
		const auto physical = (selector - 4u) % info.component_count;
		ctx.state.builder.AddFunction({OpBitFieldUExtract, TypeU32(ctx.state), values[lane], packed,
		                               ConstantU32(ctx.state, info.component_bit_offset[physical]),
		                               ConstantU32(ctx.state, info.component_bits[physical])});
	}
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(ctx.state, 4), result,
	                               values[0], values[1], values[2], values[3]});
	return result;
}

uint32_t EmitOneDimensionalGatherLz(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                    uint32_t coord, Prospero::TextureNumericClass numeric_class) {
	auto& state = ctx.state;
	state.builder.RequireCapability(CapabilityImageQuery);
	const auto image = LoadSampledImageDescriptor(state, mem.resource);
	const auto width = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpImageQuerySizeLod, TypeU32(state), width, image, ConstantU32(state, 0)});
	const auto width_f32 = state.builder.AllocateId();
	state.builder.AddFunction({OpConvertUToF, TypeF32(state), width_f32, width});
	const auto left = state.builder.AllocateId();
	state.builder.AddFunction({OpExtInst, TypeF32(state), left, GlslStd450(state), GlslFloor,
	                           Binary(state, OpFSub, TypeF32(state),
	                                  Binary(state, OpFMul, TypeF32(state), coord, width_f32),
	                                  ConstantF32(state, 0x3f000000u))});

	const auto sampled     = MakeSampledImage(state, mem.resource, mem.sampler);
	const auto vector_type = ImageVectorType(state, numeric_class, 4);
	const auto scalar_type = ImageScalarType(state, numeric_class);
	const auto component =
	    ImageConversionFormat(state, mem).format == Prospero::BufferFormat::kInvalid
	        ? ImageGatherComponent(mem.dmask)
	        : 0u;
	uint32_t values[2] {};
	for (uint32_t index = 0; index < 2u; index++) {
		const auto sample_coord =
		    Binary(state, OpFDiv, TypeF32(state),
		           Binary(state, OpFAdd, TypeF32(state), left,
		                  ConstantF32(state, index == 0u ? 0x3f000000u : 0x3fc00000u)),
		           width_f32);
		const auto texel = state.builder.AllocateId();
		state.builder.AddFunction({OpImageSampleExplicitLod, vector_type, texel, sampled,
		                           sample_coord, ImageOperandsLodMask, ZeroF32(state)});
		values[index] = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpCompositeExtract, scalar_type, values[index], texel, component});
	}
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpCompositeConstruct, vector_type, result, values[0], values[1], values[1], values[0]});
	return result;
}

uint32_t PackImageTexel(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t texel) {
	const auto info = ImageConversionFormat(ctx.state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return texel;

	auto packed = ConstantU32(ctx.state, 0u);
	for (uint32_t component = 0; component < info.component_count; component++) {
		const auto value = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(
		    {OpCompositeExtract, TypeU32(ctx.state), value, texel, component});
		const auto maximum =
		    ConstantU32(ctx.state, info.component_bits[component] == 32u
		                               ? UINT32_MAX
		                               : (1u << info.component_bits[component]) - 1u);
		const auto within  = Binary(ctx.state, OpULessThan, TypeBool(ctx.state), value, maximum);
		const auto clamped = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(
		    {OpSelect, TypeU32(ctx.state), clamped, within, value, maximum});
		const auto shifted =
		    info.component_bit_offset[component] == 0u
		        ? clamped
		        : Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state), clamped,
		                 ConstantU32(ctx.state, info.component_bit_offset[component]));
		packed = Binary(ctx.state, OpBitwiseOr, TypeU32(ctx.state), packed, shifted);
	}
	const auto zero   = ConstantU32(ctx.state, 0u);
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(
	    {OpCompositeConstruct, TypeU32Vector(ctx.state, 4), result, packed, zero, zero, zero});
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
			ctx.state.builder.AddFunction(
			    {OpCompositeExtract, TypeU32(ctx.state), raw, data,
			     mem.data_bits == 16u ? packed_index / 2u : packed_index});
			if (mem.data_bits == 16u) {
				if ((packed_index & 1u) != 0u) {
					raw = Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state), raw,
					             ConstantU32(ctx.state, 16u));
				}
				raw = Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), raw,
				             ConstantU32(ctx.state, 0xffffu));
			}
		}
		values[component] = integer ? raw
		                    : mem.data_bits == 16u
		                        ? EmitF16BitsToF32(ctx.state, raw)
		                        : Unary(ctx.state, OpBitcast, TypeF32(ctx.state), raw);
	}
	const auto texel = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(
	    {OpCompositeConstruct, integer ? TypeU32Vector(ctx.state, 4) : TypeF32Vector(ctx.state, 4),
	     texel, values[0], values[1], values[2], values[3]});
	return PackImageTexel(ctx, mem, texel);
}

uint32_t ImageAtomicOpcode(IR::ValueOpcode opcode) {
	switch (opcode) {
		case IR::ValueOpcode::ImageAtomicIAdd32: return OpAtomicIAdd;
		case IR::ValueOpcode::ImageAtomicUMin32: return OpAtomicUMin;
		case IR::ValueOpcode::ImageAtomicUMax32: return OpAtomicUMax;
		case IR::ValueOpcode::ImageAtomicAnd32: return OpAtomicAnd;
		case IR::ValueOpcode::ImageAtomicOr32: return OpAtomicOr;
		case IR::ValueOpcode::ImageAtomicXor32: return OpAtomicXor;
		default: return 0;
	}
}

} // namespace

bool EmitValueImage(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto op         = inst.GetOpcode();
	const auto image_info = IR::ImageOpcodeInfoOf(op);
	if (image_info.access == IR::ImageAccess::None) {
		return false;
	}
	auto&       state     = ctx.state;
	const auto& mem       = ctx.Memory(inst);
	const auto  image_arg = inst.Arg(0);
	ctx.ResourceIndex(image_arg, IR::ValueOpcode::GetImageResource);
	const auto& image   = state.program.info.images.at(mem.resource);
	const auto* address = ctx.ImageAddress(inst.Arg(image_info.needs_sampler ? 2 : 1));
	if (address == nullptr) return true;
	if (op == IR::ValueOpcode::ImageQueryDimensions) {
		state.builder.RequireCapability(CapabilityImageQuery);
		ctx.Define(inst, QueryDimensions(ctx, mem, *address));
		return true;
	}
	if (op == IR::ValueOpcode::ImageQueryLod) {
		state.builder.RequireCapability(CapabilityImageQuery);
		const auto dimension = image.dimension;
		const auto sampled   = MakeSampledImage(state, mem.resource, mem.sampler);
		const auto lod       = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpImageQueryLod, TypeF32Vector(state, 2), lod, sampled,
		     CoordF32(ctx, mem, *address, 0, ImageDimensionInfoFor(dimension).spatial_components)});
		uint32_t values[4] = {ConstantU32(state, 0), ConstantU32(state, 0), ConstantU32(state, 0),
		                      ConstantU32(state, 0)};
		for (uint32_t index = 0; index < 2u; index++) {
			const auto component = state.builder.AllocateId();
			state.builder.AddFunction({OpCompositeExtract, TypeF32(state), component, lod, index});
			values[index] = FloatBits(ctx, component);
		}
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(state, 4), result, values[0],
		                           values[1], values[2], values[3]});
		ctx.Define(inst, result);
		return true;
	}
	if (op == IR::ValueOpcode::ImageRead) {
		const auto  dimension      = image.dimension;
		const auto& dimension_info = ImageDimensionInfoFor(dimension);
		const auto  numeric_class  = image.numeric_class;
		const auto  condition      = ctx.Arg(inst, 2);
		ctx.Define(
		    inst,
		    EmitValueOrDefaultIfCondition(
		        state, condition, TypeU32Vector(state, 4), ConstantU32CompositeZero(state, 4),
		        [&]() {
			        const auto descriptor = LoadSampledImageDescriptor(state, mem.resource);
			        const auto color      = state.builder.AllocateId();
			        const auto coord      = CoordU32(ctx, mem, *address, dimension);
			        if (dimension_info.multisampled != 0u) {
				        state.builder.AddFunction(
				            {OpImageFetch, ImageVectorType(state, numeric_class, 4), color,
				             descriptor, coord, ImageOperandsSampleMask,
				             AddressU32(ctx, mem, *address, dimension_info.coordinate_components)});
			        } else {
				        state.builder.AddFunction({OpImageFetch,
				                                   ImageVectorType(state, numeric_class, 4), color,
				                                   descriptor, coord, ImageOperandsLodMask,
				                                   LodU32(ctx, mem, *address, dimension)});
			        }
			        return ResultVector(ctx, UnpackImageTexel(ctx, mem, color), numeric_class,
			                            false, mem);
		        }));
		return true;
	}
	if (op == IR::ValueOpcode::ImageWrite) {
		const bool uint_image = image.numeric_class == Prospero::TextureNumericClass::Uint;
		const auto dimension  = image.dimension;
		EmitIfCondition(state, ctx.Arg(inst, 3), [&]() {
			const auto mip_lod =
			    state.program.info.images[mem.resource].mip_mode == IR::ImageMipMode::DynamicStorage
			        ? LodU32(ctx, mem, *address, dimension)
			        : 0u;
			const auto coord = CoordU32(ctx, mem, *address, dimension);
			const auto texel = StoreTexel(ctx, mem, ctx.Arg(inst, 2), uint_image);
			EmitStorageImageWrite(state, mem.resource, mip_lod, coord, texel);
		});
		return true;
	}
	if (op == IR::ValueOpcode::ImageSampleRaw || op == IR::ValueOpcode::ImageGatherRaw) {
		const auto  dimension      = image.dimension;
		const auto& dimension_info = ImageDimensionInfoFor(dimension);
		const auto  layout         = Layout(mem, dimension);
		const auto  numeric_class  = image.numeric_class;
		const bool  dref           = HasFlag(mem, Decoder::ImageSampleFlagCompare);
		if (dref && state.program.info.images[mem.resource].conversion_format !=
		                Prospero::BufferFormat::kInvalid) {
			ctx.Fail(inst, "uses depth comparison with a packed integer image");
			return true;
		}
		const auto coord =
		    CoordF32(ctx, mem, *address, layout.coord, dimension_info.coordinate_components);
		if (op == IR::ValueOpcode::ImageGatherRaw) {
			if (dimension == ImageDimension::Dim1D) {
				if (dref || !HasFlag(mem, Decoder::ImageSampleFlagLevelZero) ||
				    HasFlag(mem, Decoder::ImageSampleFlagOffset) ||
				    HasFlag(mem, Decoder::ImageSampleFlagGatherHorizontal)) {
					ctx.Fail(inst, "has an unsupported 1D gather variant");
					return true;
				}
				const auto sample = EmitOneDimensionalGatherLz(ctx, mem, coord, numeric_class);
				ctx.Define(inst, ResultVector(ctx, UnpackImageGather(ctx, mem, sample),
				                              numeric_class, false, mem, true));
				return true;
			}
			if (dimension == ImageDimension::Dim1DArray) {
				ctx.Fail(inst, "has an unsupported 1D-array gather");
				return true;
			}
			const auto            sampled = MakeSampledImage(state, mem.resource, mem.sampler);
			const auto            sample  = state.builder.AllocateId();
			std::vector<uint32_t> words;
			if (dref) {
				auto dref_value = ZeroF32(state);
				if (layout.dref != NoImageComponent) {
					dref_value = AddressF32(ctx, mem, *address, layout.dref);
				}
				words = {OpImageDrefGather, TypeF32Vector(state, 4), sample, sampled, coord,
				         dref_value};
			} else {
				uint32_t component = 0;
				if (ImageConversionFormat(state, mem).format == Prospero::BufferFormat::kInvalid) {
					component = ImageGatherComponent(mem.dmask);
				}
				words = {OpImageGather, ImageVectorType(state, numeric_class, 4),
				         sample,        sampled,
				         coord,         ConstantU32(state, component)};
			}
			if (HasFlag(mem, Decoder::ImageSampleFlagGatherHorizontal)) {
				words.push_back(ImageOperandsConstOffsetsMask);
				words.push_back(HorizontalOffsets(state, dimension));
			} else if (layout.offset != NoImageComponent) {
				words.push_back(ImageOperandsOffsetMask);
				words.push_back(PackedOffset(ctx, mem, *address, layout, dimension));
			}
			state.builder.AddFunction(words);
			auto result_numeric_class = numeric_class;
			if (dref) {
				result_numeric_class = Prospero::TextureNumericClass::Float;
			}
			ctx.Define(inst, ResultVector(ctx, UnpackImageGather(ctx, mem, sample),
			                              result_numeric_class, false, mem, true));
			return true;
		}
		const bool explicit_lod = HasFlag(mem, Decoder::ImageSampleFlagDerivative) ||
		                          HasFlag(mem, Decoder::ImageSampleFlagLod) ||
		                          HasFlag(mem, Decoder::ImageSampleFlagLevelZero) ||
		                          state.stage != ShaderType::Pixel;
		uint32_t opcode = OpImageSampleImplicitLod;
		if (explicit_lod) {
			opcode = dref ? OpImageSampleDrefExplicitLod : OpImageSampleExplicitLod;
		} else if (dref) {
			opcode = OpImageSampleDrefImplicitLod;
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
		uint32_t              operand_mask = 0;
		std::vector<uint32_t> operands;
		if (HasFlag(mem, Decoder::ImageSampleFlagDerivative)) {
			operand_mask |= ImageOperandsGradMask;
			operands.push_back(
			    CoordF32(ctx, mem, *address, layout.grad_x, dimension_info.spatial_components));
			operands.push_back(
			    CoordF32(ctx, mem, *address, layout.grad_y, dimension_info.spatial_components));
		} else if (explicit_lod) {
			operand_mask |= ImageOperandsLodMask;
			auto lod = ZeroF32(state);
			if (HasFlag(mem, Decoder::ImageSampleFlagLod) && layout.lod != NoImageComponent) {
				lod = AddressF32(ctx, mem, *address, layout.lod);
			}
			operands.push_back(lod);
		} else if (layout.bias != NoImageComponent) {
			operand_mask |= ImageOperandsBiasMask;
			operands.push_back(AddressF32(ctx, mem, *address, layout.bias));
		}
		const auto EmitSample = [&](uint32_t resource) {
			const auto            sampled = MakeSampledImage(state, resource, mem.sampler);
			const auto            sample  = state.builder.AllocateId();
			std::vector<uint32_t> words {opcode, result_type, sample, sampled, coord};
			if (dref) {
				words.push_back(dref_value);
			}
			if (operand_mask != 0u) {
				words.push_back(operand_mask);
				words.insert(words.end(), operands.begin(), operands.end());
			}
			state.builder.AddFunction(words);
			return sample;
		};
		if (image.indirect_root != mem.resource) {
			const auto sample = EmitSample(mem.resource);
			auto       result = sample;
			if (!dref) {
				result = UnpackImageTexel(ctx, mem, sample);
			}
			ctx.Define(inst, ResultVector(ctx, result, numeric_class, dref, mem));
			return true;
		}
		const auto* handle = image_arg.ResolveInstruction();
		const auto* source = image.source < ctx.program.descriptor_sources.size()
		                         ? &ctx.program.descriptor_sources[image.source]
		                         : nullptr;
		if (handle == nullptr || source == nullptr || !source->indirect_image.has_value() ||
		    source->indirect_image->key_arg >= handle->NumArgs()) {
			ctx.Fail(inst, "has invalid indirect image key provenance");
			return true;
		}
		const auto key = ctx.Def(handle->Arg(source->indirect_image->key_arg));
		if (state.flattened_srt_variable == 0 || image.indirect_search_iterations == 0u ||
		    image.indirect_resources.size() < 2u) {
			ctx.Fail(inst, "has no indirect image runtime mapping");
			return true;
		}
		const auto LoadMapping = [&](uint32_t index) {
			const auto pointer = state.builder.AllocateId();
			state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state),
			                           pointer, state.flattened_srt_variable, ConstantU32(state, 0),
			                           index});
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction({OpLoad, TypeU32(state), value, pointer});
			return value;
		};
		const auto mapping  = ConstantU32(state, image.indirect_mapping_offset);
		auto       low      = ConstantU32(state, 0u);
		auto       high     = LoadMapping(mapping);
		auto       selected = ConstantU32(state, 0u);
		for (uint32_t iteration = 0; iteration < image.indirect_search_iterations;
		     iteration++) {
			const auto active = Binary(state, OpULessThan, TypeBool(state), low, high);
			const auto mid =
			    Binary(state, OpShiftRightLogical, TypeU32(state),
			           Binary(state, OpIAdd, TypeU32(state), low, high), ConstantU32(state, 1u));
			const auto probe = state.builder.AllocateId();
			state.builder.AddFunction(
			    {OpSelect, TypeU32(state), probe, active, mid, ConstantU32(state, 0u)});
			const auto entry      = Binary(state, OpIAdd, TypeU32(state), mapping,
			                               Binary(state, OpIAdd, TypeU32(state),
			                                      Binary(state, OpShiftLeftLogical, TypeU32(state),
			                                             probe, ConstantU32(state, 1u)),
			                                      ConstantU32(state, 1u)));
			const auto mapped_key = LoadMapping(entry);
			const auto candidate =
			    LoadMapping(Binary(state, OpIAdd, TypeU32(state), entry, ConstantU32(state, 1u)));
			const auto equal         = Binary(state, OpIEqual, TypeBool(state), mapped_key, key);
			const auto match         = Binary(state, OpLogicalAnd, TypeBool(state), active, equal);
			const auto next_selected = state.builder.AllocateId();
			state.builder.AddFunction(
			    {OpSelect, TypeU32(state), next_selected, match, candidate, selected});
			selected              = next_selected;
			const auto less       = Binary(state, OpULessThan, TypeBool(state), mapped_key, key);
			const auto take_upper = Binary(state, OpLogicalAnd, TypeBool(state), active, less);
			const auto take_lower = Binary(state, OpLogicalAnd, TypeBool(state), active,
			                               Unary(state, OpLogicalNot, TypeBool(state), less));
			const auto next_low   = state.builder.AllocateId();
			state.builder.AddFunction(
			    {OpSelect, TypeU32(state), next_low, take_upper,
			     Binary(state, OpIAdd, TypeU32(state), mid, ConstantU32(state, 1u)), low});
			low                  = next_low;
			const auto next_high = state.builder.AllocateId();
			state.builder.AddFunction({OpSelect, TypeU32(state), next_high, take_lower, mid, high});
			high = next_high;
		}
		const auto            default_label = state.builder.AllocateId();
		const auto            merge_label   = state.builder.AllocateId();
		std::vector<uint32_t> labels(image.indirect_resources.size() - 1u);
		std::vector<uint32_t> switch_words {OpSwitch, selected, default_label};
		for (uint32_t candidate = 1; candidate < image.indirect_resources.size(); candidate++) {
			labels[candidate - 1u] = state.builder.AllocateId();
			switch_words.push_back(candidate);
			switch_words.push_back(labels[candidate - 1u]);
		}
		state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
		state.builder.AddFunction(switch_words);
		std::vector<uint32_t> phi_words {OpPhi, result_type, state.builder.AllocateId()};
		EmitLabel(state, default_label);
		phi_words.push_back(EmitSample(image.indirect_resources[0]));
		phi_words.push_back(default_label);
		state.builder.AddFunction({OpBranch, merge_label});
		for (uint32_t candidate = 1; candidate < image.indirect_resources.size(); candidate++) {
			EmitLabel(state, labels[candidate - 1u]);
			phi_words.push_back(EmitSample(image.indirect_resources[candidate]));
			phi_words.push_back(labels[candidate - 1u]);
			state.builder.AddFunction({OpBranch, merge_label});
		}
		EmitLabel(state, merge_label);
		state.builder.AddFunction(phi_words);
		auto result = phi_words[2];
		if (!dref) {
			result = UnpackImageTexel(ctx, mem, result);
		}
		ctx.Define(inst, ResultVector(ctx, result, numeric_class, dref, mem));
		return true;
	}
	const auto atomic_opcode = ImageAtomicOpcode(op);
	if (atomic_opcode != 0u) {
		const auto dimension = image.dimension;
		ctx.Define(inst, EmitValueOrZeroIfCondition(state, ctx.Arg(inst, 3), [&]() {
			           const auto pointer = state.builder.AllocateId();
			           const auto pointer_type =
			               state.builder.Type(OpTypePointer, {StorageClassImage, TypeU32(state)});
			           state.builder.AddFunction(
			               {OpImageTexelPointer, pointer_type, pointer,
			                StorageImageDescriptorPointer(state, mem.resource),
			                CoordU32(ctx, mem, *address, dimension), ConstantU32(state, 0)});
			           const auto old = state.builder.AllocateId();
			           state.builder.AddFunction({atomic_opcode, TypeU32(state), old, pointer,
			                                      ConstantU32(state, ScopeDevice),
			                                      ConstantU32(state, MemorySemanticsNone),
			                                      ctx.Arg(inst, 2)});
			           EmitDeviceAtomicMemoryBarrier(state);
			           return old;
		           }));
		return true;
	}
	return false;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
