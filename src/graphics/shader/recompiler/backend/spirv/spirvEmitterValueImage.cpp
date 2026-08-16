#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

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
			value = Binary(ctx.state, OpShiftRightLogical, ctx.state.uint_type, value,
			               ConstantU32(ctx.state, 16));
		}
		value = Binary(ctx.state, OpBitwiseAnd, ctx.state.uint_type, value,
		               ConstantU32(ctx.state, 0xffffu));
	}
	return value;
}

uint32_t AddressF32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                    uint32_t component) {
	const auto value = AddressU32(ctx, mem, address, component);
	return ComponentUsesA16(mem, component)
	           ? EmitF16BitsToF32(ctx.state, value)
	           : Unary(ctx.state, OpBitcast, ctx.state.float_type, value);
}

ImageSampleLayout Layout(const IR::MemoryInfo& mem, ImageViewKind view) {
	ImageSampleLayout layout;
	uint32_t          cursor = 0;
	if (HasFlag(mem, Decoder::ImageSampleFlagOffset)) layout.offset = cursor++;
	if (HasFlag(mem, Decoder::ImageSampleFlagBias)) layout.bias = cursor++;
	if (HasFlag(mem, Decoder::ImageSampleFlagCompare)) layout.dref = cursor++;
	if (HasFlag(mem, Decoder::ImageSampleFlagDerivative)) {
		layout.grad_x = cursor;
		cursor += ImageViewSpatialComponents(view);
		layout.grad_y = cursor;
		cursor += ImageViewSpatialComponents(view);
	}
	layout.coord = cursor;
	cursor += ImageViewCoordinateComponents(view);
	if (HasFlag(mem, Decoder::ImageSampleFlagLod)) layout.lod = cursor++;
	return layout;
}

uint32_t ZeroF32(EmitterState& state) {
	return ConstantF32(state, 0);
}

uint32_t ZeroU32x4(EmitterState& state) {
	const auto zero  = ConstantU32(state, 0);
	const auto value = state.builder.AllocateId();
	state.builder.AddType(
	    {OpConstantComposite, state.vec4_uint_type, value, zero, zero, zero, zero});
	return value;
}

uint32_t CubeAxis(EmitterState& state, uint32_t value) {
	return Binary(state, OpFSub, state.float_type, value, ConstantF32(state, 0x3f800000u));
}

uint32_t CubeLayer(EmitterState& state, uint32_t value) {
	const auto guest = state.builder.AllocateId();
	state.builder.AddFunction({OpConvertFToU, state.uint_type, guest, value});
	const auto padding =
	    Binary(state, OpShiftLeftLogical, state.uint_type,
	           Binary(state, OpShiftRightLogical, state.uint_type, guest, ConstantU32(state, 3)),
	           ConstantU32(state, 1));
	const auto host   = Binary(state, OpISub, state.uint_type, guest, padding);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpConvertUToF, state.float_type, result, host});
	return result;
}

uint32_t CoordF32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                  uint32_t first, uint32_t components) {
	auto x = AddressF32(ctx, mem, address, first);
	if (components == 1u) return x;
	auto y = mem.image_address_components > first + 1u ? AddressF32(ctx, mem, address, first + 1u)
	                                                   : ZeroF32(ctx.state);
	if (mem.image_cube) {
		x = CubeAxis(ctx.state, x);
		y = CubeAxis(ctx.state, y);
	}
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		auto z = mem.image_address_components > first + 2u
		             ? AddressF32(ctx, mem, address, first + 2u)
		             : ZeroF32(ctx.state);
		if (mem.image_cube) z = CubeLayer(ctx.state, z);
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, ctx.state.vec3_float_type, result, x, y, z});
	} else {
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, ctx.state.vec2_float_type, result, x, y});
	}
	return result;
}

uint32_t CoordU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                  ImageViewKind view) {
	const auto components = ImageViewCoordinateComponents(view);
	const auto x          = AddressU32(ctx, mem, address, 0);
	if (components == 1u) return x;
	const auto y      = mem.image_address_components > 1u ? AddressU32(ctx, mem, address, 1)
	                                                      : ConstantU32(ctx.state, 0);
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		const auto z = mem.image_address_components > 2u ? AddressU32(ctx, mem, address, 2)
		                                                 : ConstantU32(ctx.state, 0);
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, ctx.state.vec3_uint_type, result, x, y, z});
	} else {
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, ctx.state.vec2_uint_type, result, x, y});
	}
	return result;
}

uint32_t LodU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                ImageViewKind view) {
	const auto component = ImageViewCoordinateComponents(view);
	return mem.image_has_mip && mem.image_address_components > component
	           ? AddressU32(ctx, mem, address, component)
	           : ConstantU32(ctx.state, 0);
}

uint32_t FloatBits(ValueEmitContext& ctx, uint32_t value) {
	return Unary(ctx.state, OpBitcast, ctx.state.uint_type, value);
}

uint32_t ResultVector(ValueEmitContext& ctx, uint32_t value, bool integer, bool dref) {
	uint32_t component[4] {};
	for (uint32_t index = 0; index < 4u; index++) {
		if (dref) {
			component[index] = index == 0u ? FloatBits(ctx, value) : ConstantU32(ctx.state, 0);
			continue;
		}
		const auto scalar = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction({OpCompositeExtract,
		                               integer ? ctx.state.uint_type : ctx.state.float_type, scalar,
		                               value, index});
		component[index] = integer ? scalar : FloatBits(ctx, scalar);
	}
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpCompositeConstruct, ctx.state.vec4_uint_type, result,
	                               component[0], component[1], component[2], component[3]});
	return result;
}

uint32_t QueryDimensions(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                         const IR::Inst& address) {
	const auto pc    = inst.Flags<IR::MemoryFlags>().pc;
	const auto view  = SampledImageViewKind(ctx.state, mem, pc);
	const auto image = LoadSampledImageDescriptor(ctx.state, mem, pc, view);
	const auto size  = ctx.state.builder.AllocateId();
	if (ImageSpirvMultisampled(view) != 0u) {
		ctx.state.builder.AddFunction(
		    {OpImageQuerySize, ImageViewSizeType(ctx.state, view), size, image});
	} else {
		ctx.state.builder.AddFunction({OpImageQuerySizeLod, ImageViewSizeType(ctx.state, view),
		                               size, image, AddressU32(ctx, mem, address, 0)});
	}
	const auto components = ImageViewCoordinateComponents(view);
	uint32_t   result[4]  = {ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0),
	                         ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0)};
	if (components == 1u) {
		result[0] = size;
	} else {
		for (uint32_t index = 0; index < components; index++) {
			result[index] = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(
			    {OpCompositeExtract, ctx.state.uint_type, result[index], size, index});
		}
	}
	if (ImageSpirvMultisampled(view) == 0u) {
		result[3] = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction({OpImageQueryLevels, ctx.state.uint_type, result[3], image});
	}
	const auto vector = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpCompositeConstruct, ctx.state.vec4_uint_type, vector,
	                               result[0], result[1], result[2], result[3]});
	return vector;
}

uint32_t PackedOffset(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                      const ImageSampleLayout& layout, ImageViewKind view) {
	const auto components = ImageViewSpatialComponents(view);
	const auto zero       = ConstantI32(ctx.state, 0);
	if (layout.offset == NoImageComponent || mem.image_address_components <= layout.offset) {
		if (components == 1u) return zero;
		const auto result = ctx.state.builder.AllocateId();
		if (components == 3u) {
			ctx.state.builder.AddFunction(
			    {OpCompositeConstruct, ctx.state.vec3_int_type, result, zero, zero, zero});
		} else {
			ctx.state.builder.AddFunction(
			    {OpCompositeConstruct, ctx.state.vec2_int_type, result, zero, zero});
		}
		return result;
	}
	const auto packed    = Unary(ctx.state, OpBitcast, ctx.state.int_type,
	                             AddressU32(ctx, mem, address, layout.offset));
	uint32_t   values[3] = {zero, zero, zero};
	for (uint32_t index = 0; index < components; index++) {
		values[index] = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction({OpBitFieldSExtract, ctx.state.int_type, values[index],
		                               packed, ConstantU32(ctx.state, index * 8u),
		                               ConstantU32(ctx.state, 6)});
	}
	if (components == 1u) return values[0];
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		ctx.state.builder.AddFunction({OpCompositeConstruct, ctx.state.vec3_int_type, result,
		                               values[0], values[1], values[2]});
	} else {
		ctx.state.builder.AddFunction(
		    {OpCompositeConstruct, ctx.state.vec2_int_type, result, values[0], values[1]});
	}
	return result;
}

uint32_t HorizontalOffsets(EmitterState& state, ImageViewKind view) {
	const auto components   = ImageViewSpatialComponents(view);
	const auto array_type   = state.builder.AllocateId();
	const auto count        = ConstantU32(state, 4);
	const auto element_type = components == 1u ? state.int_type : state.vec2_int_type;
	state.builder.AddType({OpTypeArray, array_type, element_type, count});
	uint32_t offsets[4] {};
	for (uint32_t index = 0; index < 4u; index++) {
		const auto x = ConstantI32(state, static_cast<int32_t>(index) - 1);
		if (components == 1u) {
			offsets[index] = x;
		} else {
			offsets[index] = state.builder.AllocateId();
			state.builder.AddType({OpConstantComposite, state.vec2_int_type, offsets[index], x,
			                       ConstantI32(state, 0)});
		}
	}
	const auto value = state.builder.AllocateId();
	state.builder.AddType(
	    {OpConstantComposite, array_type, value, offsets[0], offsets[1], offsets[2], offsets[3]});
	return value;
}

uint32_t InverseSwizzle(uint32_t swizzle, uint32_t component) {
	for (uint32_t source = 0; source < 4u; source++) {
		if (((swizzle >> (source * 3u)) & 7u) == 4u + component) return source;
	}
	return UINT32_MAX;
}

uint32_t StoreTexel(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t data, bool integer) {
	const auto swizzle = ctx.state.program.info.images[mem.resource].storage_swizzle;
	uint32_t   values[4] {};
	const auto dmask = mem.dmask != 0u ? mem.dmask : 1u;
	for (uint32_t component = 0; component < 4u; component++) {
		const auto source = InverseSwizzle(swizzle, component);
		uint32_t   raw    = ConstantU32(ctx.state, 0);
		if (source < 4u && ((dmask >> source) & 1u) != 0u) {
			const auto packed_index = DmaskComponentIndex(dmask, source);
			raw                     = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(
			    {OpCompositeExtract, ctx.state.uint_type, raw, data, packed_index});
		}
		values[component] = integer ? raw : Unary(ctx.state, OpBitcast, ctx.state.float_type, raw);
	}
	const auto texel = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction({OpCompositeConstruct,
	                               integer ? ctx.state.vec4_uint_type : ctx.state.vec4_float_type,
	                               texel, values[0], values[1], values[2], values[3]});
	return texel;
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
	const auto op = inst.GetOpcode();
	if (op < IR::ValueOpcode::ImageQueryDimensions || op > IR::ValueOpcode::ImageAtomicXor32) {
		return false;
	}
	auto&       state     = ctx.state;
	const auto& mem       = ctx.Memory(inst);
	const auto  pc        = inst.Flags<IR::MemoryFlags>().pc;
	const auto  image_arg = inst.Arg(0);
	ctx.ResourceIndex(image_arg, IR::ValueOpcode::GetImageResource);
	const bool  sampled = op == IR::ValueOpcode::ImageQueryLod ||
	                      op == IR::ValueOpcode::ImageSampleRaw ||
	                      op == IR::ValueOpcode::ImageGatherRaw;
	const auto* address = ctx.ImageAddress(inst.Arg(sampled ? 2 : 1));
	if (address == nullptr) return true;
	if (op == IR::ValueOpcode::ImageQueryDimensions) {
		ctx.Define(inst, QueryDimensions(ctx, inst, mem, *address));
		return true;
	}
	if (op == IR::ValueOpcode::ImageQueryLod) {
		const auto view    = SampledImageViewKind(state, mem, pc);
		const auto sampled = MakeSampledImage(state, mem, pc, view);
		const auto lod     = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpImageQueryLod, state.vec2_float_type, lod, sampled,
		     CoordF32(ctx, mem, *address, 0, ImageViewSpatialComponents(view))});
		uint32_t values[4] = {ConstantU32(state, 0), ConstantU32(state, 0), ConstantU32(state, 0),
		                      ConstantU32(state, 0)};
		for (uint32_t index = 0; index < 2u; index++) {
			const auto component = state.builder.AllocateId();
			state.builder.AddFunction(
			    {OpCompositeExtract, state.float_type, component, lod, index});
			values[index] = FloatBits(ctx, component);
		}
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeConstruct, state.vec4_uint_type, result, values[0],
		                           values[1], values[2], values[3]});
		ctx.Define(inst, result);
		return true;
	}
	if (op == IR::ValueOpcode::ImageRead) {
		const auto view      = SampledImageViewKind(state, mem, pc);
		const bool integer   = mem.kind == IR::ResourceKind::ImageUint;
		const auto condition = ctx.Arg(inst, 2);
		ctx.Define(
		    inst,
		    EmitValueOrDefaultIfCondition(
		        state, condition, state.vec4_uint_type, ZeroU32x4(state), [&]() {
			        const auto image = LoadSampledImageDescriptor(state, mem, pc, view);
			        const auto color = state.builder.AllocateId();
			        const auto coord = CoordU32(ctx, mem, *address, view);
			        if (ImageSpirvMultisampled(view) != 0u) {
				        state.builder.AddFunction(
				            {OpImageFetch, integer ? state.vec4_uint_type : state.vec4_float_type,
				             color, image, coord, ImageOperandsSampleMask,
				             AddressU32(ctx, mem, *address, ImageViewCoordinateComponents(view))});
			        } else {
				        state.builder.AddFunction(
				            {OpImageFetch, integer ? state.vec4_uint_type : state.vec4_float_type,
				             color, image, coord, ImageOperandsLodMask,
				             LodU32(ctx, mem, *address, view)});
			        }
			        return ResultVector(ctx, color, integer, false);
		        }));
		return true;
	}
	if (op == IR::ValueOpcode::ImageWrite) {
		const auto uint_image = mem.kind == IR::ResourceKind::StorageImageUint;
		const auto view       = StorageImageViewKind(state, mem, uint_image, pc);
		EmitIfCondition(state, ctx.Arg(inst, 3), [&]() {
			const auto mip_lod =
			    state.program.info.images[mem.resource].mip_mode == IR::ImageMipMode::DynamicStorage
			        ? LodU32(ctx, mem, *address, view)
			        : 0u;
			const auto coord = CoordU32(ctx, mem, *address, view);
			const auto texel = StoreTexel(ctx, mem, ctx.Arg(inst, 2), uint_image);
			EmitStorageImageWrite(state, mem.resource, uint_image, view, mip_lod, coord, texel);
		});
		return true;
	}
	if (op == IR::ValueOpcode::ImageSampleRaw || op == IR::ValueOpcode::ImageGatherRaw) {
		const auto view    = SampledImageViewKind(state, mem, pc);
		const auto layout  = Layout(mem, view);
		const bool integer = mem.kind == IR::ResourceKind::ImageUint;
		const bool dref    = HasFlag(mem, Decoder::ImageSampleFlagCompare);
		const auto sampled = MakeSampledImage(state, mem, pc, view);
		const auto coord =
		    CoordF32(ctx, mem, *address, layout.coord, ImageViewCoordinateComponents(view));
		const auto sample = state.builder.AllocateId();
		if (op == IR::ValueOpcode::ImageGatherRaw) {
			std::vector<uint32_t> words =
			    dref ? std::vector<uint32_t> {OpImageDrefGather,
			                                  state.vec4_float_type,
			                                  sample,
			                                  sampled,
			                                  coord,
			                                  layout.dref != NoImageComponent
			                                      ? AddressF32(ctx, mem, *address, layout.dref)
			                                      : ZeroF32(state)}
			         : std::vector<uint32_t> {
			               OpImageGather, integer ? state.vec4_uint_type : state.vec4_float_type,
			               sample,        sampled,
			               coord,         ConstantU32(state, ImageGatherComponent(mem.dmask))};
			if (HasFlag(mem, Decoder::ImageSampleFlagGatherHorizontal)) {
				words.push_back(ImageOperandsConstOffsetsMask);
				words.push_back(HorizontalOffsets(state, view));
			} else if (layout.offset != NoImageComponent) {
				words.push_back(ImageOperandsOffsetMask);
				words.push_back(PackedOffset(ctx, mem, *address, layout, view));
			}
			state.builder.AddFunction(words);
			ctx.Define(inst, ResultVector(ctx, sample, integer && !dref, false));
			return true;
		}
		const bool explicit_lod = HasFlag(mem, Decoder::ImageSampleFlagDerivative) ||
		                          HasFlag(mem, Decoder::ImageSampleFlagLod) ||
		                          HasFlag(mem, Decoder::ImageSampleFlagLevelZero) ||
		                          state.stage != ShaderType::Pixel;
		const auto opcode = explicit_lod
		                        ? (dref ? OpImageSampleDrefExplicitLod : OpImageSampleExplicitLod)
		                        : (dref ? OpImageSampleDrefImplicitLod : OpImageSampleImplicitLod);
		const auto result_type =
		    dref ? state.float_type : (integer ? state.vec4_uint_type : state.vec4_float_type);
		std::vector<uint32_t> words {opcode, result_type, sample, sampled, coord};
		if (dref) {
			words.push_back(layout.dref != NoImageComponent
			                    ? AddressF32(ctx, mem, *address, layout.dref)
			                    : ZeroF32(state));
		}
		uint32_t              operand_mask = 0;
		std::vector<uint32_t> operands;
		if (HasFlag(mem, Decoder::ImageSampleFlagDerivative)) {
			operand_mask |= ImageOperandsGradMask;
			operands.push_back(
			    CoordF32(ctx, mem, *address, layout.grad_x, ImageViewSpatialComponents(view)));
			operands.push_back(
			    CoordF32(ctx, mem, *address, layout.grad_y, ImageViewSpatialComponents(view)));
		} else if (explicit_lod) {
			operand_mask |= ImageOperandsLodMask;
			operands.push_back(HasFlag(mem, Decoder::ImageSampleFlagLod) &&
			                           layout.lod != NoImageComponent
			                       ? AddressF32(ctx, mem, *address, layout.lod)
			                       : ZeroF32(state));
		} else if (layout.bias != NoImageComponent) {
			operand_mask |= ImageOperandsBiasMask;
			operands.push_back(AddressF32(ctx, mem, *address, layout.bias));
		}
		if (operand_mask != 0u) {
			words.push_back(operand_mask);
			words.insert(words.end(), operands.begin(), operands.end());
		}
		state.builder.AddFunction(words);
		ctx.Define(inst, ResultVector(ctx, sample, integer, dref));
		return true;
	}
	const auto atomic_opcode = ImageAtomicOpcode(op);
	if (atomic_opcode != 0u) {
		const auto view = StorageImageViewKind(state, mem, true, pc);
		ctx.Define(inst, EmitValueOrZeroIfCondition(state, ctx.Arg(inst, 3), [&]() {
			           const auto pointer = state.builder.AllocateId();
			           state.builder.AddFunction(
			               {OpImageTexelPointer, state.ptr_image_uint, pointer,
			                StorageImageDescriptorPointer(state, mem.resource, true, pc, view),
			                CoordU32(ctx, mem, *address, view), ConstantU32(state, 0)});
			           const auto old = state.builder.AllocateId();
			           state.builder.AddFunction({atomic_opcode, state.uint_type, old, pointer,
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
