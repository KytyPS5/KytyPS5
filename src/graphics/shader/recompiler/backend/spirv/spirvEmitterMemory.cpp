#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include "graphics/host_gpu/renderer/cache/bufferCache.h"

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

uint32_t Select(EmitterState& state, uint32_t type, uint32_t condition, uint32_t true_value,
                uint32_t false_value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, type, result, condition, true_value, false_value});
	return result;
}

uint32_t AndCondition(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	return Binary(state, OpLogicalAnd, TypeBool(state), lhs, rhs);
}

uint32_t EmitDsMaskedLaneRead(EmitterState& state, uint32_t source, uint32_t target,
                              uint32_t exec) {
	const auto shuffled = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformShuffle, TypeU32(state), shuffled,
	                           ConstantU32(state, ScopeSubgroup), source, target});
	const auto source_exec = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformShuffle, TypeBool(state), source_exec,
	                           ConstantU32(state, ScopeSubgroup), exec, target});
	const auto source_active =
	    AndCondition(state, source_exec, EmitSubgroupLaneActiveBool(state, target));
	return Select(state, TypeU32(state), source_active, shuffled, ConstantU32(state, 0));
}

uint32_t BufferByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                           uint32_t index, uint32_t offset, uint32_t soffset) {
	auto&          state   = ctx.state;
	const uint32_t packed  = StorageBufferPackedStride(state, mem);
	const uint32_t stride  = packed & 0x3fffu;
	const bool     swizzle = stride != 0u && ((packed >> 14u) & 1u) != 0u;
	if (((packed >> 20u) & 1u) != 0u) {
		const auto lane = Binary(state, OpBitwiseAnd, TypeU32(state),
		                         EmitSubgroupLocalInvocationId(state), ConstantU32(state, 63));
		index           = Binary(state, OpIAdd, TypeU32(state), index, lane);
	}
	if (mem.offset != 0u) {
		offset = Binary(state, OpIAdd, TypeU32(state), offset, ConstantU32(state, mem.offset));
	}

	uint32_t address = 0;
	if (!swizzle) {
		if (stride == 0u) {
			address = offset;
		} else {
			const auto indexed = stride == 1u ? index
			                                  : Binary(state, OpIMul, TypeU32(state), index,
			                                           ConstantU32(state, stride));
			address            = Binary(state, OpIAdd, TypeU32(state), indexed, offset);
		}
	} else {
		const uint32_t stride_enum  = (packed >> 16u) & 3u;
		const uint32_t index_stride = 8u << stride_enum;
		const auto     index_msb    = Binary(state, OpShiftRightLogical, TypeU32(state), index,
		                                     ConstantU32(state, stride_enum + 3u));
		const auto     index_lsb    = Binary(state, OpBitwiseAnd, TypeU32(state), index,
		                                     ConstantU32(state, index_stride - 1u));
		const auto     offset_msb =
		    Binary(state, OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, ~3u));
		const auto offset_lsb =
		    Binary(state, OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, 3u));
		const auto indexed_msb = stride == 1u ? index_msb
		                                      : Binary(state, OpIMul, TypeU32(state), index_msb,
		                                               ConstantU32(state, stride));
		const auto msb = Binary(state, OpIMul, TypeU32(state),
		                        Binary(state, OpIAdd, TypeU32(state), indexed_msb, offset_msb),
		                        ConstantU32(state, index_stride));
		const auto lsb = Binary(
		    state, OpIAdd, TypeU32(state),
		    Binary(state, OpShiftLeftLogical, TypeU32(state), index_lsb, ConstantU32(state, 2u)),
		    offset_lsb);
		address = Binary(state, OpIAdd, TypeU32(state), msb, lsb);
	}

	const auto soffset_value = inst.Arg(3).Resolve();
	if (soffset_value.IsImmediate() && soffset_value.GetType() == IR::Type::U32 &&
	    soffset_value.U32() == 0u) {
		return address;
	}
	return Binary(state, OpIAdd, TypeU32(state), address, soffset);
}

uint32_t AddU64Low(EmitterState& state, uint32_t low, uint32_t high, uint32_t add_low,
                   uint32_t add_high, uint32_t& out_high) {
	const auto result = Binary(state, OpIAdd, TypeU32(state), low, add_low);
	const auto carry  = Binary(state, OpULessThan, TypeBool(state), result, low);
	out_high =
	    Binary(state, OpIAdd, TypeU32(state), Binary(state, OpIAdd, TypeU32(state), high, add_high),
	           Select(state, TypeU32(state), carry, ConstantU32(state, 1), ConstantU32(state, 0)));
	return result;
}

uint32_t ScratchByteAddress(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t low,
                            uint32_t high) {
	auto& state     = ctx.state;
	auto  immediate = static_cast<int32_t>(mem.offset);
	const auto immediate_low  = ConstantU32(state, static_cast<uint32_t>(immediate));
	const auto immediate_high = ConstantU32(state, immediate < 0 ? UINT32_MAX : 0u);
	low                      = AddU64Low(state, low, high, immediate_low, immediate_high, high);
	const auto valid =
	    Binary(state, OpIEqual, TypeBool(state), high, ConstantU32(state, 0));
	return Select(state, TypeU32(state), valid, low, ConstantU32(state, UINT32_MAX));
}

uint32_t ConstantDeviceAddress(EmitterState& state, uint64_t value) {
	return state.builder.Constant(OpConstant, TypeDeviceAddress(state),
	                              {static_cast<uint32_t>(value),
	                               static_cast<uint32_t>(value >> 32u)});
}

uint32_t DeviceAddressFromWords(EmitterState& state, uint32_t low, uint32_t high) {
	const auto low64  = Unary(state, OpUConvert, TypeDeviceAddress(state), low);
	const auto high64 = Binary(state, OpShiftLeftLogical, TypeDeviceAddress(state),
	                           Unary(state, OpUConvert, TypeDeviceAddress(state), high),
	                           ConstantDeviceAddress(state, 32));
	return Binary(state, OpBitwiseOr, TypeDeviceAddress(state), low64, high64);
}

uint32_t GuestAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto& state = ctx.state;
	auto  low   = ctx.Arg(inst, 1);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		low = Binary(state, OpBitwiseAnd, TypeU32(state), low, ConstantU32(state, ~3u));
	}
	uint32_t address = 0;
	if (mem.address_is_full) {
		address = DeviceAddressFromWords(state, low, ctx.Arg(inst, 2));
	} else {
		const auto* handle = inst.Arg(0).Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != IR::ValueOpcode::GetAddressResource ||
		    handle->NumArgs() != 2) {
			ctx.Fail(inst, "has no address base pair");
			return ConstantDeviceAddress(state, 0);
		}
		const auto base = DeviceAddressFromWords(state, ctx.Arg(*handle, 0), ctx.Arg(*handle, 1));
		address = Binary(state, OpIAdd, TypeDeviceAddress(state), base,
		                 Unary(state, OpUConvert, TypeDeviceAddress(state), low));
	}
	auto immediate = static_cast<int32_t>(mem.offset);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
	}
	return immediate == 0
	           ? address
	           : Binary(state, OpIAdd, TypeDeviceAddress(state), address,
	                    ConstantDeviceAddress(state,
	                                          static_cast<uint64_t>(static_cast<int64_t>(immediate))));
}

uint32_t FaultElementPointer(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                           state.fault_buffer_variable, ConstantU32(state, 0), index});
	return pointer;
}

void RecordBdaFault(EmitterState& state, uint32_t page) {
	const auto word = Binary(state, OpShiftRightLogical, TypeU32(state), page,
	                         ConstantU32(state, 5));
	const auto bit = Binary(
	    state, OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
	    Binary(state, OpBitwiseAnd, TypeU32(state), page, ConstantU32(state, 31)));
	const auto pointer = FaultElementPointer(state, word);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, TypeU32(state), value, pointer});
	state.builder.AddFunction(
	    {OpStore, pointer, Binary(state, OpBitwiseOr, TypeU32(state), value, bit)});
}

uint32_t GetBdaPointer(ValueEmitContext& ctx, uint32_t address) {
	auto&      state  = ctx.state;
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpFunctionCall, TypeDeviceAddress(state), result, state.bda_pointer_function, address});
	return result;
}

uint32_t LoadBdaDword(ValueEmitContext& ctx, uint32_t address) {
	auto&      state   = ctx.state;
	const auto bda     = GetBdaPointer(ctx, address);
	const auto present = Binary(state, OpINotEqual, TypeBool(state), bda,
	                            ConstantDeviceAddress(state, 0));
	return EmitValueOrZeroIfCondition(state, present, [&]() {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer, bda});
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpLoad, TypeU32(state), value, pointer, MemoryAccessAlignedMask, sizeof(uint32_t)});
		return value;
	});
}

uint32_t LoadBda(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
	             uint32_t bits) {
	auto&      state   = ctx.state;
	const auto address = GuestAddress(ctx, inst, mem);
	const auto active  = ctx.Arg(inst, inst.NumArgs() - 1);
	return EmitValueOrZeroIfCondition(state, active, [&]() {
		const auto aligned = Binary(state, OpBitwiseAnd, TypeDeviceAddress(state), address,
		                            ConstantDeviceAddress(state, ~uint64_t {3}));
		const auto first   = LoadBdaDword(ctx, aligned);
		const auto byte = Binary(state, OpBitwiseAnd, TypeU32(state),
		                         Unary(state, OpUConvert, TypeU32(state), address),
		                         ConstantU32(state, 3));
		const auto crosses = bits == 8u
		                          ? ConstantBool(state, false)
		                          : Binary(state, bits == 16u ? OpUGreaterThan : OpINotEqual,
		                                   TypeBool(state), byte,
		                                   ConstantU32(state, bits == 16u ? 2u : 0u));
		const auto second = EmitValueOrZeroIfCondition(state, crosses, [&]() {
			return LoadBdaDword(
			    ctx, Binary(state, OpIAdd, TypeDeviceAddress(state), aligned,
			                ConstantDeviceAddress(state, sizeof(uint32_t))));
		});
		const auto shift = Binary(state, OpShiftLeftLogical, TypeU32(state), byte,
		                          ConstantU32(state, 3));
		const auto upper_shift = Binary(
		    state, OpShiftLeftLogical, TypeU32(state),
		    Binary(state, OpBitwiseAnd, TypeU32(state),
		           Binary(state, OpISub, TypeU32(state), ConstantU32(state, 4), byte),
		           ConstantU32(state, 3)),
		    ConstantU32(state, 3));
		const auto merged = Binary(
		    state, OpBitwiseOr, TypeU32(state),
		    Binary(state, OpShiftRightLogical, TypeU32(state), first, shift),
		    Binary(state, OpShiftLeftLogical, TypeU32(state), second, upper_shift));
		return bits == 32u
		           ? merged
		           : Binary(state, OpBitwiseAnd, TypeU32(state), merged,
		                    ConstantU32(state, bits == 8u ? 0xffu : 0xffffu));
	});
}

uint32_t ByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	if (mem.kind == IR::ResourceKind::Buffer) {
		return BufferByteAddress(ctx, inst, mem, ctx.Arg(inst, 1), ctx.Arg(inst, 2),
		                         ctx.Arg(inst, 3));
	}
	if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
		if (mem.offset == 0u) {
			return ctx.Arg(inst, 0);
		}
		return Binary(ctx.state, OpIAdd, TypeU32(ctx.state), ctx.Arg(inst, 0),
		              ConstantU32(ctx.state, mem.offset));
	}
	if (mem.kind != IR::ResourceKind::Scratch) {
		EXIT("physical address memory must use the BDA emitter\n");
	}
	return ScratchByteAddress(ctx, mem, ctx.Arg(inst, 1), ctx.Arg(inst, 2));
}

uint32_t DwordIndex(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	return Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state), ByteAddress(ctx, inst, mem),
	              ConstantU32(ctx.state, 2));
}

struct PreparedMemoryElement {
	MemoryResourceAccess resource;
	uint32_t             index = 0;
};

PreparedMemoryElement PrepareMemoryElement(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                           uint32_t raw_index) {
	auto       resource = PrepareMemoryResourceAccess(ctx.state, mem);
	const auto index    = EmitMemoryElementIndex(ctx.state, resource, raw_index);
	return {.resource = resource, .index = index};
}

PreparedMemoryElement PrepareMemoryElement(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                           const MemoryResourceAccess& resource,
                                           uint32_t                    raw_index) {
	return {.resource = resource, .index = EmitMemoryElementIndex(ctx.state, resource, raw_index)};
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index);

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend);

uint32_t LoadWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource) {
	const auto access = PrepareMemoryElement(ctx, mem, resource, DwordIndex(ctx, inst, mem));
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index),
	    [&]() { return LoadWordInBounds(ctx, access.resource, access.index); });
}

uint32_t LoadWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadWordPrepared(ctx, inst, mem, resource);
	});
}

uint32_t LoadSubwordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                             const MemoryResourceAccess& resource, uint32_t bits,
                             bool sign_extend) {
	const auto address   = ByteAddress(ctx, inst, mem);
	const auto raw_index = Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state), address,
	                              ConstantU32(ctx.state, 2));
	const auto access    = PrepareMemoryElement(ctx, mem, resource, raw_index);
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
		    return LoadSubwordInBounds(ctx, access.resource, address, access.index, bits,
		                               sign_extend);
	    });
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index) {
	const auto value   = ctx.state.builder.AllocateId();
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	ctx.state.builder.AddFunction({OpLoad, TypeU32(ctx.state), value, pointer});
	return value;
}

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend) {
	const auto word = LoadWordInBounds(ctx, resource, index);
	const auto byte =
	    Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), address, ConstantU32(ctx.state, 3));
	const auto shift =
	    Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state), byte, ConstantU32(ctx.state, 3));
	const auto value =
	    Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state),
	           Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state), word, shift),
	           ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu));
	if (!sign_extend) return value;
	const auto left = Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state), value,
	                         ConstantU32(ctx.state, 32u - bits));
	return Binary(ctx.state, OpShiftRightArithmetic, TypeU32(ctx.state), left,
	              ConstantU32(ctx.state, 32u - bits));
}

uint32_t LoadSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits,
                     bool sign_extend) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadSubwordPrepared(ctx, inst, mem, resource, bits, sign_extend);
	});
}

Prospero::BufferFormat BufferFormat(const ValueEmitContext& ctx, const IR::Inst& inst,
                                    const IR::MemoryInfo& mem) {
	return mem.typed ? Format::DecodeTBufferFormat(mem.data_format, mem.number_format)
	                 : StorageBufferFormat(ctx.state, mem);
}

IR::MemoryInfo RebaseFormattedComponent(IR::MemoryInfo mem, Prospero::BufferFormat format,
                                        uint32_t component) {
	mem.offset += Format::GetFormatComponentByteOffset(format, component);
	mem.data_dwords     = 1u;
	mem.component_index = component;
	return mem;
}

IR::MemoryInfo RebaseRawComponent(IR::MemoryInfo mem, uint32_t component) {
	mem.offset += component * 4u;
	mem.data_dwords     = 1u;
	mem.component_index = component;
	return mem;
}

enum class FormattedSourceKind { Memory, Zero, One };

struct FormattedSource {
	FormattedSourceKind kind      = FormattedSourceKind::Zero;
	uint32_t            component = 0;
};

FormattedSource ResolveFormattedSource(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                       const Format::BufferFormatInfo& info,
                                       uint32_t                        output_component) {
	if (mem.typed) {
		return output_component < info.component_count
		           ? FormattedSource {FormattedSourceKind::Memory, output_component}
		           : FormattedSource {};
	}
	const auto selector = GetDstSel(ctx.state.program.info.buffers[mem.resource].descriptor_swizzle,
	                                output_component);
	if (selector == 0u) return {};
	if (selector == 1u) return {FormattedSourceKind::One, 0};
	if (selector < 4u) {
		ExitDescriptorBindingFailure(ctx.state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "buffer descriptor has reserved dst_sel");
	}
	const auto component = selector - 4u;
	return component < info.component_count
	           ? FormattedSource {FormattedSourceKind::Memory, component}
	           : FormattedSource {};
}

uint32_t FormattedConstant(ValueEmitContext& ctx, const Format::BufferFormatInfo& info,
                           FormattedSourceKind kind) {
	if (kind != FormattedSourceKind::One) return ConstantU32(ctx.state, 0);
	const auto integer =
	    info.type == Format::ComponentType::Uint || info.type == Format::ComponentType::Sint;
	return ConstantU32(ctx.state, integer ? 1u : 0x3f800000u);
}

template <typename LoadWordFn, typename LoadSubwordFn>
uint32_t LoadFormattedComponent(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                Prospero::BufferFormat format, const Format::BufferFormatInfo& info,
                                uint32_t output_component, LoadWordFn&& load_word,
                                LoadSubwordFn&& load_subword) {
	const auto source = ResolveFormattedSource(ctx, mem, info, output_component);
	if (source.kind != FormattedSourceKind::Memory) {
		return FormattedConstant(ctx, info, source.kind);
	}
	const auto component = source.component;
	const auto bits      = info.component_bits[component];
	uint32_t   raw       = 0;
	if (info.packed_bitfield) {
		raw = load_word(component);
		const auto type =
		    IsSignedFormatComponent(info.type) ? TypeI32(ctx.state) : TypeU32(ctx.state);
		const auto source_value =
		    type == TypeI32(ctx.state) ? Unary(ctx.state, OpBitcast, type, raw) : raw;
		const auto extracted = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(
		    {IsSignedFormatComponent(info.type) ? OpBitFieldSExtract : OpBitFieldUExtract, type,
		     extracted, source_value, ConstantU32(ctx.state, info.component_bit_offset[component]),
		     ConstantU32(ctx.state, bits)});
		raw = type == TypeI32(ctx.state)
		          ? Unary(ctx.state, OpBitcast, TypeU32(ctx.state), extracted)
		          : extracted;
	} else if (bits == 32u) {
		raw = load_word(component);
	} else {
		raw = load_subword(component, bits, IsSignedFormatComponent(info.type));
	}
	return NormalizeFormatComponent(ctx.state, info, component, raw);
}

uint32_t FormattedLoadPrepared(ValueEmitContext& ctx, const IR::Inst& inst,
                               const IR::MemoryInfo& mem, uint32_t output_component,
                               const MemoryResourceAccess& resource) {
	const auto format = BufferFormat(ctx, inst, mem);
	if (!Format::IsKnownFormat(format)) {
		return LoadWordPrepared(ctx, inst, RebaseRawComponent(mem, output_component), resource);
	}
	const auto info = Format::GetFormatInfo(format);
	return LoadFormattedComponent(
	    ctx, mem, format, info, output_component,
	    [&](uint32_t component) {
		    return LoadWordPrepared(ctx, inst, RebaseFormattedComponent(mem, format, component),
		                            resource);
	    },
	    [&](uint32_t component, uint32_t bits, bool sign_extend) {
		    return LoadSubwordPrepared(ctx, inst, RebaseFormattedComponent(mem, format, component),
		                               resource, bits, sign_extend);
	    });
}

uint32_t FormattedLoad(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return FormattedLoadPrepared(ctx, inst, mem, 0u, resource);
	});
}

void StoreSubwordInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource, uint32_t address, uint32_t index,
                          uint32_t bits, uint32_t data) {
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	const auto shift   = Binary(
	    ctx.state, OpShiftLeftLogical, TypeU32(ctx.state),
	    Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), address, ConstantU32(ctx.state, 3)),
	    ConstantU32(ctx.state, 3));
	const auto mask  = Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state),
	                          ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu), shift);
	const auto value = Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state),
	                          Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), data,
	                                 ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu)),
	                          shift);
	const auto merge = [&](uint32_t old) {
		return Binary(ctx.state, OpBitwiseOr, TypeU32(ctx.state),
		              Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), old,
		                     Unary(ctx.state, OpNot, TypeU32(ctx.state), mask)),
		              value);
	};
	if (mem.kind == IR::ResourceKind::Scratch) {
		const auto old = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction({OpLoad, TypeU32(ctx.state), old, pointer});
		ctx.state.builder.AddFunction({OpStore, pointer, merge(old)});
	} else {
		AtomicUpdate(ctx.state, pointer, mem.kind, merge);
	}
}

void StoreSubwordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource, uint32_t bits, uint32_t data) {
	const auto address   = ByteAddress(ctx, inst, mem);
	const auto raw_index = Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state), address,
	                              ConstantU32(ctx.state, 2));
	const auto access    = PrepareMemoryElement(ctx, mem, resource, raw_index);
	EmitIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
		    StoreSubwordInBounds(ctx, mem, access.resource, address, access.index, bits, data);
	    });
}

void StoreSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		StoreSubwordPrepared(ctx, inst, mem, resource, bits, ctx.Arg(inst, inst.NumArgs() - 2));
	});
}

void StoreWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                       const MemoryResourceAccess& resource, uint32_t data) {
	const auto access = PrepareMemoryElement(ctx, mem, resource, DwordIndex(ctx, inst, mem));
	EmitIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
		    ctx.state.builder.AddFunction(
		        {OpStore, EmitMemoryElementPointer(ctx.state, access.resource, access.index),
		         data});
	    });
}

void StoreWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource, uint32_t index,
                       uint32_t data) {
	ctx.state.builder.AddFunction(
	    {OpStore, EmitMemoryElementPointer(ctx.state, resource, index), data});
}

void StoreWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		StoreWordPrepared(ctx, inst, mem, resource, ctx.Arg(inst, inst.NumArgs() - 2));
	});
}

void FormattedStorePrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                            uint32_t component, const MemoryResourceAccess& resource,
                            uint32_t data) {
	const auto format = BufferFormat(ctx, inst, mem);
	if (!Format::IsKnownFormat(format)) {
		StoreWordPrepared(ctx, inst, RebaseRawComponent(mem, component), resource, data);
		return;
	}
	const auto info = Format::GetFormatInfo(format);
	if (component >= info.component_count) return;
	const auto bits          = info.component_bits[component];
	const auto component_mem = RebaseFormattedComponent(mem, format, component);
	if (bits == 8u || bits == 16u) {
		StoreSubwordPrepared(ctx, inst, component_mem, resource, bits, data);
	} else {
		StoreWordPrepared(ctx, inst, component_mem, resource, data);
	}
}

void FormattedStore(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		FormattedStorePrepared(ctx, inst, mem, 0u, resource, ctx.Arg(inst, inst.NumArgs() - 2));
	});
}

uint32_t SpirvAtomicOpcode(IR::ValueOpcode opcode) {
	switch (opcode) {
		case IR::ValueOpcode::BufferAtomicCmpSwap32: return OpAtomicCompareExchange;
		case IR::ValueOpcode::BufferAtomicSwap32:
		case IR::ValueOpcode::BufferAtomicSwap64:
		case IR::ValueOpcode::SharedAtomicSwap32: return OpAtomicExchange;
		case IR::ValueOpcode::BufferAtomicIAdd32:
		case IR::ValueOpcode::SharedAtomicIAdd32: return OpAtomicIAdd;
		case IR::ValueOpcode::BufferAtomicISub32:
		case IR::ValueOpcode::SharedAtomicISub32: return OpAtomicISub;
		case IR::ValueOpcode::BufferAtomicSMin32:
		case IR::ValueOpcode::SharedAtomicSMin32: return OpAtomicSMin;
		case IR::ValueOpcode::BufferAtomicUMin32:
		case IR::ValueOpcode::SharedAtomicUMin32: return OpAtomicUMin;
		case IR::ValueOpcode::BufferAtomicSMax32:
		case IR::ValueOpcode::SharedAtomicSMax32: return OpAtomicSMax;
		case IR::ValueOpcode::BufferAtomicUMax32:
		case IR::ValueOpcode::SharedAtomicUMax32: return OpAtomicUMax;
		case IR::ValueOpcode::BufferAtomicAnd32:
		case IR::ValueOpcode::SharedAtomicAnd32: return OpAtomicAnd;
		case IR::ValueOpcode::BufferAtomicOr32:
		case IR::ValueOpcode::BufferAtomicOr64:
		case IR::ValueOpcode::SharedAtomicOr32: return OpAtomicOr;
		case IR::ValueOpcode::BufferAtomicXor32:
		case IR::ValueOpcode::SharedAtomicXor32: return OpAtomicXor;
		default: return 0;
	}
}

uint32_t EmitAtomicOperation(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t pointer,
                             uint32_t scope) {
	const auto old = ctx.state.builder.AllocateId();
	if (inst.GetOpcode() == IR::ValueOpcode::BufferAtomicCmpSwap32) {
		const auto desired    = ctx.Arg(inst, inst.NumArgs() - 3);
		const auto comparator = ctx.Arg(inst, inst.NumArgs() - 2);
		ctx.state.builder.AddFunction(
		    {OpAtomicCompareExchange, TypeU32(ctx.state), old, pointer, ConstantU32(ctx.state, scope),
		     ConstantU32(ctx.state, MemorySemanticsNone),
		     ConstantU32(ctx.state, MemorySemanticsNone), desired, comparator});
	} else {
		const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
		ctx.state.builder.AddFunction(
		    {SpirvAtomicOpcode(inst.GetOpcode()), TypeU32(ctx.state), old, pointer,
		     ConstantU32(ctx.state, scope), ConstantU32(ctx.state, MemorySemanticsNone), value});
	}
	return old;
}

uint32_t EmitAtomic(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	const auto result =
	    EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		    const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		    return EmitValueOrZeroIfCondition(
		        ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index),
		        [&]() {
			        const auto scope =
			            mem.kind == IR::ResourceKind::Lds ? ScopeWorkgroup : ScopeDevice;
			        const auto pointer =
			            EmitMemoryElementPointer(ctx.state, access.resource, access.index);
			        const auto old = EmitAtomicOperation(ctx, inst, pointer, scope);
			        if (mem.kind == IR::ResourceKind::Lds) {
				        const auto semantics =
				            MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory;
				        ctx.state.builder.AddFunction({OpMemoryBarrier,
				                                       ConstantU32(ctx.state, ScopeWorkgroup),
				                                       ConstantU32(ctx.state, semantics)});
			        } else {
				        EmitDeviceAtomicMemoryBarrier(ctx.state);
			        }
			        return old;
		        });
	    });
	return result;
}

uint32_t EmitBufferAtomic64(ValueEmitContext& ctx, const IR::Inst& inst,
                            const IR::MemoryInfo& mem) {
	auto& state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU64(state), ConstantU64(state, 0), [&]() {
		    const auto resource = PrepareStorageBufferResourceAccess(
		        state, mem, state.storage_buffer_u64_variable, TypeStorageBufferU64Pointer(state));
		    const auto byte_address = Binary(state, OpIAdd, TypeU32(state),
		                                     ByteAddress(ctx, inst, mem), resource.byte_offset);
		    const auto index = Binary(state, OpShiftRightLogical, TypeU32(state), byte_address,
		                              ConstantU32(state, 3u));
		    return EmitValueOrDefaultIfCondition(
		        state, EmitMemoryElementInBounds(state, resource, index), TypeU64(state),
		        ConstantU64(state, 0), [&]() {
			        const auto value = Unary(state, OpBitcast, TypeScalarU64(state),
			                                 ctx.Arg(inst, inst.NumArgs() - 2));
			        const auto old   = state.builder.AllocateId();
			        state.builder.AddFunction(
			            {SpirvAtomicOpcode(inst.GetOpcode()), TypeScalarU64(state), old,
			             EmitStorageBufferElementPointer(
			                 state, resource, index, TypeStorageBufferU64ElementPointer(state)),
			             ConstantU32(state, ScopeDevice),
			             ConstantU32(state, MemorySemanticsNone), value});
			        EmitDeviceAtomicMemoryBarrier(state);
			        return Unary(state, OpBitcast, TypeU64(state), old);
		        });
	    });
}

uint32_t FloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                     bool max_value) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		return EmitValueOrZeroIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    const auto pointer =
			        EmitMemoryElementPointer(ctx.state, access.resource, access.index);
			    const auto source = ctx.Arg(inst, inst.NumArgs() - 2);
			    return AtomicUpdate(ctx.state, pointer, mem.kind, [&](uint32_t old) {
				    return EmitFloatAtomicReplacement(ctx.state, old, source, max_value);
			    });
		    });
	});
}

uint32_t SharedFloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                           bool max_value) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		EmitIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    ctx.state.builder.AddFunction(
			        {OpStore, ctx.scratch_u32_variable, ctx.Arg(inst, 1)});
			    const auto data = ctx.state.builder.AllocateId();
			    ctx.state.builder.AddFunction(
			        {OpLoad, TypeU32(ctx.state), data, ctx.scratch_u32_variable});
			    AtomicUpdate(
			        ctx.state, EmitMemoryElementPointer(ctx.state, access.resource, access.index),
			        mem.kind, [&](uint32_t old) {
				        const auto old_f = Unary(ctx.state, OpBitcast, TypeF32(ctx.state), old);
				        const auto compare_f =
				            Unary(ctx.state, OpBitcast, TypeF32(ctx.state), ctx.Arg(inst, 2));
				        const auto data_f = Unary(ctx.state, OpBitcast, TypeF32(ctx.state), data);
				        const auto compare =
				            Binary(ctx.state, max_value ? OpFOrdGreaterThan : OpFOrdLessThan,
				                   TypeBool(ctx.state), max_value ? old_f : compare_f,
				                   max_value ? compare_f : old_f);
				        return Unary(ctx.state, OpBitcast, TypeU32(ctx.state),
				                     Select(ctx.state, TypeF32(ctx.state), compare, data_f, old_f));
			        });
		    });
	});
	return 0;
}

uint32_t AppendConsume(ValueEmitContext& ctx, const IR::Inst& inst, bool append) {
	auto&      state = ctx.state;
	const auto m0    = ctx.Arg(inst, 0);
	const auto base =
	    Binary(state, OpShiftRightLogical, TypeU32(state), m0, ConstantU32(state, 16));
	const auto size = Binary(state, OpBitwiseAnd, TypeU32(state), m0, ConstantU32(state, 0xffffu));
	const auto address =
	    Binary(state, OpIAdd, TypeU32(state), base, ConstantU32(state, ctx.Memory(inst).offset));
	const auto raw_index =
	    Binary(state, OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
	const auto mem    = ctx.Memory(inst);
	const auto access = PrepareMemoryResourceAccess(state, mem);
	const auto index  = EmitMemoryElementIndex(state, access, raw_index);
	const auto exec   = ctx.Arg(inst, 1);
	const auto ballot = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformBallot, TypeU32Vector(state, 4), ballot,
	                           ConstantU32(state, ScopeSubgroup), exec});
	const auto low  = state.builder.AllocateId();
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeExtract, TypeU32(state), low, ballot, 0});
	state.builder.AddFunction({OpCompositeExtract, TypeU32(state), high, ballot, 1});
	const auto count =
	    Binary(state, OpIAdd, TypeU32(state), Unary(state, OpBitCount, TypeU32(state), low),
	           Unary(state, OpBitCount, TypeU32(state), high));
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformBallotFindLSB, TypeU32(state), first,
	                           ConstantU32(state, ScopeSubgroup), ballot});
	const auto is_first =
	    Binary(state, OpIEqual, TypeBool(state), EmitSubgroupLocalInvocationId(state), first);
	const auto storage_bounds = EmitMemoryElementInBounds(state, access, index);
	const auto m0_bounds =
	    mem.kind == IR::ResourceKind::Gds
	        ? Binary(state, OpINotEqual, TypeBool(state), size, ConstantU32(state, 0))
	        : Binary(state, OpULessThan, TypeBool(state),
	                 ConstantU32(state, ctx.Memory(inst).offset + 3u), size);
	const auto condition = AndCondition(
	    state, is_first, AndCondition(state, exec, AndCondition(state, storage_bounds, m0_bounds)));
	const auto atomic = EmitValueOrZeroIfCondition(state, condition, [&]() {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(
		    {append ? OpAtomicIAdd : OpAtomicISub, TypeU32(state), value,
		     EmitMemoryElementPointer(state, access, index),
		     ConstantU32(state, mem.kind == IR::ResourceKind::Gds ? ScopeDevice : ScopeWorkgroup),
		     ConstantU32(state, MemorySemanticsNone), count});
		return value;
	});
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformShuffle, TypeU32(state), result,
	                           ConstantU32(state, ScopeSubgroup), atomic, first});
	return result;
}

struct PreparedFormattedMemory {
	Prospero::BufferFormat   format = Prospero::BufferFormat::kInvalid;
	Format::BufferFormatInfo info;
	MemoryResourceAccess     resource;
	std::array<uint32_t, 4>  addresses {};
	std::array<uint32_t, 4>  indices {};
	uint32_t                 in_bounds = 0;
};

enum class FormattedAccess { Load, Store };

PreparedFormattedMemory PrepareFormattedMemory(ValueEmitContext& ctx, const IR::Inst& inst,
                                               const IR::MemoryInfo&       mem,
                                               const MemoryResourceAccess& resource,
                                               uint32_t components, FormattedAccess access) {
	PreparedFormattedMemory plan;
	plan.format   = BufferFormat(ctx, inst, mem);
	plan.info     = Format::GetFormatInfo(plan.format);
	plan.resource = resource;
	std::array<bool, 4> required_components {};
	if (access == FormattedAccess::Load) {
		for (uint32_t output = 0; output < components; output++) {
			const auto source = ResolveFormattedSource(ctx, mem, plan.info, output);
			if (source.kind == FormattedSourceKind::Memory) {
				required_components[source.component] = true;
			}
		}
	} else {
		for (uint32_t component = 0; component < std::min(components, plan.info.component_count);
		     component++) {
			required_components[component] = true;
		}
	}
	bool first_bound = true;
	for (uint32_t component = 0; component < plan.info.component_count; component++) {
		if (!required_components[component]) continue;
		const auto byte_offset = Format::GetFormatComponentByteOffset(plan.format, component);
		bool       reused      = false;
		for (uint32_t previous = 0; previous < component; previous++) {
			if (required_components[previous] &&
			    Format::GetFormatComponentByteOffset(plan.format, previous) == byte_offset) {
				plan.addresses[component] = plan.addresses[previous];
				plan.indices[component]   = plan.indices[previous];
				reused                    = true;
				break;
			}
		}
		if (reused) continue;
		const auto component_mem  = RebaseFormattedComponent(mem, plan.format, component);
		plan.addresses[component] = ByteAddress(ctx, inst, component_mem);
		const auto raw_index      = Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state),
		                                   plan.addresses[component], ConstantU32(ctx.state, 2));
		plan.indices[component]   = EmitMemoryElementIndex(ctx.state, resource, raw_index);
		const auto component_bound =
		    EmitMemoryElementInBounds(ctx.state, resource, plan.indices[component]);
		if (first_bound) {
			plan.in_bounds = component_bound;
			first_bound    = false;
		} else {
			plan.in_bounds = AndCondition(ctx.state, plan.in_bounds, component_bound);
		}
	}
	if (first_bound) plan.in_bounds = ConstantBool(ctx.state, true);
	return plan;
}

uint32_t LoadFormattedInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                               const PreparedFormattedMemory& plan, uint32_t output_component) {
	return LoadFormattedComponent(
	    ctx, mem, plan.format, plan.info, output_component,
	    [&](uint32_t component) {
		    return LoadWordInBounds(ctx, plan.resource, plan.indices[component]);
	    },
	    [&](uint32_t component, uint32_t bits, bool sign_extend) {
		    return LoadSubwordInBounds(ctx, plan.resource, plan.addresses[component],
		                               plan.indices[component], bits, sign_extend);
	    });
}

uint32_t ConstructU32Composite(EmitterState& state, uint32_t components,
                               const std::array<uint32_t, 4>& values) {
	const auto            result = state.builder.AllocateId();
	std::vector<uint32_t> words {OpCompositeConstruct, TypeU32Composite(state, components), result};
	words.insert(words.end(), values.begin(), values.begin() + components);
	state.builder.AddFunction(words);
	return result;
}

uint32_t FormattedOutOfBoundsValue(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                   const PreparedFormattedMemory& plan, uint32_t components) {
	std::array<uint32_t, 4> values {};
	for (uint32_t component = 0; component < components; component++) {
		const auto source = ResolveFormattedSource(ctx, mem, plan.info, component);
		values[component] = FormattedConstant(ctx, plan.info, source.kind);
	}
	return ConstructU32Composite(ctx.state, components, values);
}

void StoreFormattedInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                            const PreparedFormattedMemory& plan, uint32_t component,
                            uint32_t data) {
	if (component >= plan.info.component_count) return;
	const auto bits = plan.info.component_bits[component];
	if (bits == 8u || bits == 16u) {
		StoreSubwordInBounds(ctx, mem, plan.resource, plan.addresses[component],
		                     plan.indices[component], bits, data);
	} else {
		StoreWordInBounds(ctx, plan.resource, plan.indices[component], data);
	}
}

uint32_t LoadWideBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU32Composite(state, components),
	    ConstantU32CompositeZero(state, components), [&]() {
		    const auto mem      = ctx.Memory(inst);
		    const auto resource = PrepareMemoryResourceAccess(state, mem);
		    const auto format =
		        mem.formatted ? BufferFormat(ctx, inst, mem) : Prospero::BufferFormat::kInvalid;
		    if (Format::IsKnownFormat(format)) {
			    const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, components,
			                                             FormattedAccess::Load);
			    return EmitValueOrDefaultIfCondition(
			        state, plan.in_bounds, TypeU32Composite(state, components),
			        FormattedOutOfBoundsValue(ctx, mem, plan, components), [&]() {
				        std::array<uint32_t, 4> values {};
				        for (uint32_t component = 0; component < components; component++) {
					        values[component] = LoadFormattedInBounds(ctx, mem, plan, component);
				        }
				        return ConstructU32Composite(state, components, values);
			        });
		    }
		    std::array<uint32_t, 4> values {};
		    for (uint32_t component = 0; component < components; component++) {
			    values[component] =
			        LoadWordPrepared(ctx, inst, RebaseRawComponent(mem, component), resource);
		    }
		    return ConstructU32Composite(state, components, values);
	    });
}

void StoreWideBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto mem       = ctx.Memory(inst);
		const auto resource  = PrepareMemoryResourceAccess(state, mem);
		const auto composite = ctx.Arg(inst, inst.NumArgs() - 2);
		const auto format =
		    mem.formatted ? BufferFormat(ctx, inst, mem) : Prospero::BufferFormat::kInvalid;
		if (Format::IsKnownFormat(format)) {
			const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, components,
			                                         FormattedAccess::Store);
			EmitIfCondition(state, plan.in_bounds, [&]() {
				for (uint32_t component = 0; component < components; component++) {
					const auto data = state.builder.AllocateId();
					state.builder.AddFunction(
					    {OpCompositeExtract, TypeU32(state), data, composite, component});
					StoreFormattedInBounds(ctx, mem, plan, component, data);
				}
			});
			return;
		}
		for (uint32_t component = 0; component < components; component++) {
			const auto data = state.builder.AllocateId();
			state.builder.AddFunction(
			    {OpCompositeExtract, TypeU32(state), data, composite, component});
			StoreWordPrepared(ctx, inst, RebaseRawComponent(mem, component), resource, data);
		}
	});
}

uint32_t LoadWideShared(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU32Composite(state, components),
	    ConstantU32CompositeZero(state, components), [&]() {
		    const auto              mem      = ctx.Memory(inst);
		    const auto              resource = PrepareMemoryResourceAccess(state, mem);
		    const auto              base     = ByteAddress(ctx, inst, mem);
		    std::array<uint32_t, 4> values {};
		    for (uint32_t component = 0; component < components; component++) {
			    const auto address   = component == 0u ? base
			                                           : Binary(state, OpIAdd, TypeU32(state), base,
			                                                    ConstantU32(state, component * 4u));
			    const auto raw_index = Binary(state, OpShiftRightLogical, TypeU32(state), address,
			                                  ConstantU32(state, 2));
			    const auto access    = PrepareMemoryElement(ctx, mem, resource, raw_index);
			    values[component]    = EmitValueOrZeroIfCondition(
			        state, EmitMemoryElementInBounds(state, access.resource, access.index),
			        [&]() { return LoadWordInBounds(ctx, access.resource, access.index); });
		    }
		    return ConstructU32Composite(state, components, values);
	    });
}

void StoreWideShared(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto mem      = ctx.Memory(inst);
		const auto resource = PrepareMemoryResourceAccess(state, mem);
		const auto base     = ByteAddress(ctx, inst, mem);
		for (uint32_t component = 0; component < components; component++) {
			const auto address = component == 0u ? base
			                                     : Binary(state, OpIAdd, TypeU32(state), base,
			                                              ConstantU32(state, component * 4u));
			const auto raw_index =
			    Binary(state, OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
			const auto access = PrepareMemoryElement(ctx, mem, resource, raw_index);
			EmitIfCondition(state, EmitMemoryElementInBounds(state, access.resource, access.index),
			                [&]() {
				                StoreWordInBounds(ctx, access.resource, access.index,
				                                  ctx.Arg(inst, component + 1u));
			                });
		}
	});
}

} // namespace

void DefineGetBdaPointer(EmitterState& state) {
	if (!state.program.info.uses_dma) {
		return;
	}
	const auto type            = TypeDeviceAddress(state);
	const auto function_type   = state.builder.Type(OpTypeFunction, {type, type});
	state.bda_pointer_function = state.builder.AllocateId();
	const auto address         = state.builder.AllocateId();
	const auto entry_label     = state.builder.AllocateId();
	state.builder.AddName(state.bda_pointer_function, "get_bda_pointer");
	state.builder.AddFunction(
	    {OpFunction, type, state.bda_pointer_function, FunctionControlNone, function_type});
	state.builder.AddFunction({OpFunctionParameter, type, address});
	EmitLabel(state, entry_label);

	const auto page64 = Binary(
	    state, OpShiftRightLogical, type, address,
	    ConstantDeviceAddress(state, BufferCache::CACHING_PAGEBITS));
	const auto page = Unary(state, OpUConvert, TypeU32(state), page64);
	const auto entry_pointer = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, TypeDeviceAddressStoragePointer(state), entry_pointer,
	                           state.bda_pagetable_variable, ConstantU32(state, 0), page});
	const auto base = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, type, base, entry_pointer});
	const auto missing =
	    Binary(state, OpIEqual, TypeBool(state), base, ConstantDeviceAddress(state, 0));
	const auto fault_label     = state.builder.AllocateId();
	const auto available_label = state.builder.AllocateId();
	const auto merge_label     = state.builder.AllocateId();
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction(
	    {OpBranchConditional, missing, fault_label, available_label});

	EmitLabel(state, fault_label);
	RecordBdaFault(state, page);
	state.builder.AddFunction({OpBranch, merge_label});

	EmitLabel(state, available_label);
	const auto offset = Binary(
	    state, OpBitwiseAnd, type, address,
	    ConstantDeviceAddress(state, BufferCache::CACHING_PAGESIZE - 1));
	const auto available = Binary(state, OpIAdd, type, base, offset);
	state.builder.AddFunction({OpBranch, merge_label});

	EmitLabel(state, merge_label);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpPhi, type, result, ConstantDeviceAddress(state, 0), fault_label,
	                           available, available_label});
	state.builder.AddFunction({OpReturnValue, result});
	state.builder.AddFunction({OpFunctionEnd});
}

bool EmitValueMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state             = ctx.state;
	const auto op                = inst.GetOpcode();
	const auto buffer_components = IR::BufferComponentCount(op);
	if (buffer_components > 1u) {
		const auto access = IR::BufferAccessOf(op);
		if (access == IR::BufferAccess::Read) {
			ctx.Define(inst, LoadWideBuffer(ctx, inst, buffer_components));
			return true;
		}
		if (access == IR::BufferAccess::Write) {
			StoreWideBuffer(ctx, inst, buffer_components);
			return true;
		}
	}
	const auto shared_components = IR::SharedComponentCount(op);
	if (shared_components > 1u) {
		if (IR::SharedAccessOf(op) == IR::SharedAccess::Read) {
			ctx.Define(inst, LoadWideShared(ctx, inst, shared_components));
		} else {
			StoreWideShared(ctx, inst, shared_components);
		}
		return true;
	}
	if ((op == IR::ValueOpcode::LoadAddressU32 || op == IR::ValueOpcode::ReadConstBuffer) &&
	    ctx.Memory(inst).planning_only) {
		return true;
	}
	if (op == IR::ValueOpcode::ReadConst) {
		if (state.flattened_srt_variable == 0) {
			ctx.Fail(inst, "requires the flattened SRT descriptor");
			return true;
		}
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
		                           state.flattened_srt_variable, ConstantU32(state, 0),
		                           ctx.Arg(inst, 1)});
		ctx.Emit(inst, OpLoad, IR::Type::U32, {pointer});
		return true;
	}
	if (op == IR::ValueOpcode::ReadConstBuffer) {
		auto mem = ctx.Memory(inst);
		mem.kind = IR::ResourceKind::ScalarBuffer;
		const auto address =
		    Binary(state, OpIAdd, TypeU32(state), ctx.Arg(inst, 1), ConstantU32(state, mem.offset));
		const auto index =
		    Binary(state, OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
		const auto access    = PrepareMemoryResourceAccess(state, mem);
		const auto element   = EmitMemoryElementIndex(state, access, index);
		const auto condition = EmitMemoryElementInBounds(state, access, element);
		ctx.Define(inst, EmitValueOrZeroIfCondition(state, condition, [&]() {
			           const auto value = state.builder.AllocateId();
			           state.builder.AddFunction(
			               {OpLoad, TypeU32(state), value,
			                EmitMemoryElementPointer(state, access, element)});
			           return value;
		           }));
		return true;
	}
	const auto address_info = IR::AddressOpcodeInfoOf(op);
	const bool load_address = address_info.access == IR::AddressAccess::Read;
	if (load_address && ctx.Memory(inst).kind != IR::ResourceKind::Scratch) {
		ctx.Define(inst, LoadBda(ctx, inst, ctx.Memory(inst), address_info.data_bits));
		return true;
	}
	const bool load_buffer  = op == IR::ValueOpcode::LoadBufferU8 ||
	                          op == IR::ValueOpcode::LoadBufferU16 ||
	                          op == IR::ValueOpcode::LoadBufferU32;
	const bool load_shared  = IR::SharedAccessOf(op) == IR::SharedAccess::Read;
	if (load_address || load_buffer || load_shared) {
		const auto mem   = ctx.Memory(inst);
		uint32_t   value = 0;
		if (op == IR::ValueOpcode::LoadBufferU32 && mem.formatted)
			value = FormattedLoad(ctx, inst, mem);
		else if (op == IR::ValueOpcode::LoadAddressU8 || op == IR::ValueOpcode::LoadBufferU8 ||
		         op == IR::ValueOpcode::LoadSharedU8)
			value = LoadSubword(ctx, inst, mem, 8, false);
		else if (op == IR::ValueOpcode::LoadAddressU16 || op == IR::ValueOpcode::LoadBufferU16 ||
		         op == IR::ValueOpcode::LoadSharedU16)
			value = LoadSubword(ctx, inst, mem, 16, false);
		else
			value = LoadWord(ctx, inst, mem);
		ctx.Define(inst, value);
		return true;
	}
	const bool store_address = address_info.access == IR::AddressAccess::Write;
	const bool store_buffer  = op == IR::ValueOpcode::StoreBufferU8 ||
	                           op == IR::ValueOpcode::StoreBufferU16 ||
	                           op == IR::ValueOpcode::StoreBufferU32;
	const bool store_shared  = IR::SharedAccessOf(op) == IR::SharedAccess::Write;
	if (store_address || store_buffer || store_shared) {
		const auto mem = ctx.Memory(inst);
		if (op == IR::ValueOpcode::StoreBufferU32 && mem.formatted)
			FormattedStore(ctx, inst, mem);
		else if (op == IR::ValueOpcode::StoreAddressU8 || op == IR::ValueOpcode::StoreBufferU8 ||
		         op == IR::ValueOpcode::WriteSharedU8)
			StoreSubword(ctx, inst, mem, 8);
		else if (op == IR::ValueOpcode::StoreAddressU16 || op == IR::ValueOpcode::StoreBufferU16 ||
		         op == IR::ValueOpcode::WriteSharedU16)
			StoreSubword(ctx, inst, mem, 16);
		else
			StoreWord(ctx, inst, mem);
		return true;
	}
	const auto atomic_opcode = SpirvAtomicOpcode(op);
	if (atomic_opcode != 0 && inst.GetType() == IR::Type::U64) {
		ctx.Define(inst, EmitBufferAtomic64(ctx, inst, ctx.Memory(inst)));
		return true;
	}
	if (atomic_opcode != 0) {
		ctx.Define(inst, EmitAtomic(ctx, inst, ctx.Memory(inst)));
		return true;
	}
	if (op == IR::ValueOpcode::BufferAtomicFMin32 || op == IR::ValueOpcode::BufferAtomicFMax32) {
		ctx.Define(inst, FloatAtomic(ctx, inst, ctx.Memory(inst),
		                             op == IR::ValueOpcode::BufferAtomicFMax32));
		return true;
	}
	if (op == IR::ValueOpcode::SharedAtomicFMin32 || op == IR::ValueOpcode::SharedAtomicFMax32) {
		SharedFloatAtomic(ctx, inst, ctx.Memory(inst), op == IR::ValueOpcode::SharedAtomicFMax32);
		return true;
	}
	if (op == IR::ValueOpcode::DataAppend || op == IR::ValueOpcode::DataConsume) {
		ctx.Define(inst, AppendConsume(ctx, inst, op == IR::ValueOpcode::DataAppend));
		return true;
	}
	if (op == IR::ValueOpcode::SwizzleU32 || op == IR::ValueOpcode::BpermuteU32) {
		uint32_t source = ctx.Arg(inst, 0);
		uint32_t target = 0;
		if (op == IR::ValueOpcode::SwizzleU32) {
			state.builder.AddFunction({OpStore, ctx.scratch_u32_variable, source});
			source = state.builder.AllocateId();
			state.builder.AddFunction({OpLoad, TypeU32(state), source, ctx.scratch_u32_variable});
			target = EmitDsSwizzleTargetLane(state, EmitSubgroupLocalInvocationId(state),
			                                 inst.Arg(1).IsImmediate() ? inst.Arg(1).U32() : 0);
		} else {
			const auto index = Binary(
			    state, OpBitwiseAnd, TypeU32(state),
			    Binary(state, OpShiftRightLogical, TypeU32(state), ctx.Arg(inst, 1),
			           ConstantU32(state, 2)),
			    ConstantU32(state, 31));
			const auto base =
			    Binary(state, OpBitwiseAnd, TypeU32(state), EmitSubgroupLocalInvocationId(state),
			           ConstantU32(state, ~31u));
			target = Binary(state, OpBitwiseOr, TypeU32(state), base, index);
		}
		ctx.Define(inst, EmitDsMaskedLaneRead(state, source, target, ctx.Arg(inst, 2)));
		return true;
	}
	return false;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
