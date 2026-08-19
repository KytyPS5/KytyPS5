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

uint32_t Select(EmitterState& state, uint32_t type, uint32_t condition, uint32_t true_value,
                uint32_t false_value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, type, result, condition, true_value, false_value});
	return result;
}

uint32_t AndCondition(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	return Binary(state, OpLogicalAnd, TypeBool(state), lhs, rhs);
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

uint32_t AddressByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                            uint32_t low, uint32_t high) {
	auto& state     = ctx.state;
	auto  immediate = static_cast<int32_t>(mem.offset);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
		low       = Binary(state, OpBitwiseAnd, TypeU32(state), low, ConstantU32(state, ~3u));
	}
	const auto immediate_low  = ConstantU32(state, static_cast<uint32_t>(immediate));
	const auto immediate_high = ConstantU32(state, immediate < 0 ? UINT32_MAX : 0u);
	const auto base           = state.program.info.addresses[mem.resource].specialized_base;
	if (mem.kind == IR::ResourceKind::Scratch) {
		low              = AddU64Low(state, low, high, immediate_low, immediate_high, high);
		const auto valid = Binary(state, OpIEqual, TypeBool(state), high, ConstantU32(state, 0));
		return Select(state, TypeU32(state), valid, low, ConstantU32(state, UINT32_MAX));
	}
	if (mem.kind == IR::ResourceKind::Flat ||
	    (mem.kind == IR::ResourceKind::Global &&
	     state.program.info.addresses[mem.resource].unbased)) {
		low                      = AddU64Low(state, low, high, immediate_low, immediate_high, high);
		const auto base_low      = ConstantU32(state, static_cast<uint32_t>(base));
		const auto base_high     = ConstantU32(state, static_cast<uint32_t>(base >> 32u));
		const auto relative      = Binary(state, OpISub, TypeU32(state), low, base_low);
		const auto borrow        = Binary(state, OpULessThan, TypeBool(state), low, base_low);
		const auto relative_high = Binary(
		    state, OpISub, TypeU32(state), Binary(state, OpISub, TypeU32(state), high, base_high),
		    Select(state, TypeU32(state), borrow, ConstantU32(state, 1), ConstantU32(state, 0)));
		const auto valid =
		    Binary(state, OpIEqual, TypeBool(state), relative_high, ConstantU32(state, 0));
		return Select(state, TypeU32(state), valid, relative, ConstantU32(state, UINT32_MAX));
	}
	const auto initial       = base + static_cast<uint64_t>(static_cast<int64_t>(immediate));
	auto       relative_high = ConstantU32(state, static_cast<uint32_t>(initial >> 32u));
	const auto relative      = AddU64Low(state, ConstantU32(state, static_cast<uint32_t>(initial)),
	                                     relative_high, low, ConstantU32(state, 0), relative_high);
	const auto valid =
	    Binary(state, OpIEqual, TypeBool(state), relative_high, ConstantU32(state, 0));
	return Select(state, TypeU32(state), valid, relative, ConstantU32(state, UINT32_MAX));
}

uint32_t ByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	if (mem.kind == IR::ResourceKind::Buffer) {
		return BufferByteAddress(ctx, inst, mem, ctx.Arg(inst, 1), ctx.Arg(inst, 2),
		                         ctx.Arg(inst, 3));
	}
	if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
		return ctx.Arg(inst, 1);
	}
	return AddressByteAddress(ctx, inst, mem, ctx.Arg(inst, 1), ctx.Arg(inst, 2));
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

uint32_t LoadWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		return EmitValueOrZeroIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    const auto value = ctx.state.builder.AllocateId();
			    ctx.state.builder.AddFunction(
			        {OpLoad, TypeU32(ctx.state), value,
			         EmitMemoryElementPointer(ctx.state, access.resource, access.index)});
			    return value;
		    });
	});
}

uint32_t LoadSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits,
                     bool sign_extend) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto address   = ByteAddress(ctx, inst, mem);
		const auto raw_index = Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state), address,
		                              ConstantU32(ctx.state, 2));
		const auto access    = PrepareMemoryElement(ctx, mem, raw_index);
		return EmitValueOrZeroIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    const auto word = ctx.state.builder.AllocateId();
			    ctx.state.builder.AddFunction(
			        {OpLoad, TypeU32(ctx.state), word,
			         EmitMemoryElementPointer(ctx.state, access.resource, access.index)});
			    const auto byte  = Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), address,
			                              ConstantU32(ctx.state, 3));
			    const auto shift = Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state), byte,
			                              ConstantU32(ctx.state, 3));
			    const auto value =
			        Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state),
			               Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state), word, shift),
			               ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu));
			    if (!sign_extend) return value;
			    const auto left = Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state), value,
			                             ConstantU32(ctx.state, 32u - bits));
			    return Binary(ctx.state, OpShiftRightArithmetic, TypeU32(ctx.state), left,
			                  ConstantU32(ctx.state, 32u - bits));
		    });
	});
}

Prospero::BufferFormat BufferFormat(const ValueEmitContext& ctx, const IR::Inst& inst,
                                    const IR::MemoryInfo& mem) {
	return mem.typed ? Format::DecodeTBufferFormat(mem.data_format, mem.number_format)
	                 : StorageBufferFormat(ctx.state, mem);
}

IR::MemoryInfo RebaseFormattedComponent(IR::MemoryInfo mem, Prospero::BufferFormat format,
                                        uint32_t component) {
	if (!mem.typed) {
		const auto component_slot = mem.component_index * 4u;
		mem.offset                = mem.offset >= component_slot ? mem.offset - component_slot : 0u;
		mem.offset += Format::GetFormatComponentByteOffset(format, component);
	}
	return mem;
}

uint32_t FormattedLoad(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	const auto format = BufferFormat(ctx, inst, mem);
	if (!Format::IsKnownFormat(format)) return LoadWord(ctx, inst, mem);
	const auto info      = Format::GetFormatInfo(format);
	auto       component = mem.component_index;
	if (!mem.typed) {
		const auto selector =
		    GetDstSel(ctx.state.program.info.buffers[mem.resource].descriptor_swizzle, component);
		if (selector == 0u) return ConstantU32(ctx.state, 0);
		if (selector == 1u) {
			const auto integer = info.type == Format::ComponentType::Uint ||
			                     info.type == Format::ComponentType::Sint;
			return ConstantU32(ctx.state, integer ? 1u : 0x3f800000u);
		}
		if (selector < 4u) {
			ExitDescriptorBindingFailure(ctx.state, IR::DescriptorBindingKind::Buffers,
			                             mem.resource, "buffer descriptor has reserved dst_sel");
		}
		component = selector - 4u;
	}
	if (component >= info.component_count) return ConstantU32(ctx.state, 0);
	uint32_t   raw  = 0;
	const auto bits = info.component_bits[component];
	if (info.packed_bitfield) {
		raw = LoadWord(ctx, inst, RebaseFormattedComponent(mem, format, component));
		const auto type =
		    IsSignedFormatComponent(info.type) ? TypeI32(ctx.state) : TypeU32(ctx.state);
		const auto source =
		    type == TypeI32(ctx.state) ? Unary(ctx.state, OpBitcast, type, raw) : raw;
		const auto extracted = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(
		    {IsSignedFormatComponent(info.type) ? OpBitFieldSExtract : OpBitFieldUExtract, type,
		     extracted, source, ConstantU32(ctx.state, info.component_bit_offset[component]),
		     ConstantU32(ctx.state, bits)});
		raw = type == TypeI32(ctx.state)
		          ? Unary(ctx.state, OpBitcast, TypeU32(ctx.state), extracted)
		          : extracted;
	} else if (bits == 32u) {
		raw = LoadWord(ctx, inst, RebaseFormattedComponent(mem, format, component));
	} else {
		raw = LoadSubword(ctx, inst, RebaseFormattedComponent(mem, format, component), bits,
		                  IsSignedFormatComponent(info.type));
	}
	return NormalizeFormatComponent(ctx.state, info, component, raw);
}

template <typename Fn>
uint32_t AtomicUpdate(EmitterState& state, uint32_t pointer, IR::ResourceKind kind, Fn&& desired) {
	const auto scope     = kind == IR::ResourceKind::Lds ? ScopeWorkgroup : ScopeDevice;
	const auto memory    = kind == IR::ResourceKind::Lds ? MemorySemanticsWorkgroupMemory
	                                                     : MemorySemanticsUniformMemory;
	const auto preheader = state.builder.AllocateId();
	const auto header    = state.builder.AllocateId();
	const auto cont      = state.builder.AllocateId();
	const auto merge     = state.builder.AllocateId();
	const auto initial   = state.builder.AllocateId();
	const auto observed  = state.builder.AllocateId();
	const auto exchanged = state.builder.AllocateId();
	state.builder.AddFunction({OpBranch, preheader});
	EmitLabel(state, preheader);
	state.builder.AddFunction({OpAtomicLoad, TypeU32(state), initial, pointer,
	                           ConstantU32(state, scope), ConstantU32(state, MemorySemanticsNone)});
	state.builder.AddFunction({OpBranch, header});
	EmitLabel(state, header);
	state.builder.AddFunction(
	    {OpPhi, TypeU32(state), observed, initial, preheader, exchanged, cont});
	const auto next = desired(observed);
	state.builder.AddFunction({OpAtomicCompareExchange, TypeU32(state), exchanged, pointer,
	                           ConstantU32(state, scope), ConstantU32(state, MemorySemanticsNone),
	                           ConstantU32(state, MemorySemanticsNone), next, observed});
	const auto success = Binary(state, OpIEqual, TypeBool(state), exchanged, observed);
	state.builder.AddFunction({OpLoopMerge, merge, cont, LoopControlNone});
	state.builder.AddFunction({OpBranchConditional, success, merge, cont});
	EmitLabel(state, cont);
	state.builder.AddFunction({OpBranch, header});
	EmitLabel(state, merge);
	state.builder.AddFunction({OpMemoryBarrier, ConstantU32(state, scope),
	                           ConstantU32(state, MemorySemanticsAcquireRelease | memory)});
	return observed;
}

void StoreSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto address   = ByteAddress(ctx, inst, mem);
		const auto raw_index = Binary(ctx.state, OpShiftRightLogical, TypeU32(ctx.state), address,
		                              ConstantU32(ctx.state, 2));
		const auto access    = PrepareMemoryElement(ctx, mem, raw_index);
		EmitIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    const auto pointer =
			        EmitMemoryElementPointer(ctx.state, access.resource, access.index);
			    const auto shift = Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state),
			                              Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state),
			                                     address, ConstantU32(ctx.state, 3)),
			                              ConstantU32(ctx.state, 3));
			    const auto mask =
			        Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state),
			               ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu), shift);
			    const auto value =
			        Binary(ctx.state, OpShiftLeftLogical, TypeU32(ctx.state),
			               Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state),
			                      ctx.Arg(inst, inst.NumArgs() - 2),
			                      ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu)),
			               shift);
			    const auto merge = [&](uint32_t old) {
				    return Binary(ctx.state, OpBitwiseOr, TypeU32(ctx.state),
				                  Binary(ctx.state, OpBitwiseAnd, TypeU32(ctx.state), old,
				                         Unary(ctx.state, OpNot, TypeU32(ctx.state), mask)),
				                  value);
			    };
			    if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
				    AtomicUpdate(ctx.state, pointer, mem.kind, merge);
			    } else {
				    const auto old = ctx.state.builder.AllocateId();
				    ctx.state.builder.AddFunction({OpLoad, TypeU32(ctx.state), old, pointer});
				    ctx.state.builder.AddFunction({OpStore, pointer, merge(old)});
			    }
		    });
	});
}

void StoreWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		EmitIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    ctx.state.builder.AddFunction(
			        {OpStore, EmitMemoryElementPointer(ctx.state, access.resource, access.index),
			         ctx.Arg(inst, inst.NumArgs() - 2)});
		    });
	});
}

void FormattedStore(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	const auto format = BufferFormat(ctx, inst, mem);
	if (!Format::IsKnownFormat(format)) {
		StoreWord(ctx, inst, mem);
		return;
	}
	const auto info = Format::GetFormatInfo(format);
	if (mem.component_index >= info.component_count) return;
	const auto bits          = info.component_bits[mem.component_index];
	const auto component_mem = RebaseFormattedComponent(mem, format, mem.component_index);
	if (bits == 8u || bits == 16u) {
		StoreSubword(ctx, inst, component_mem, bits);
	} else {
		StoreWord(ctx, inst, component_mem);
	}
}

uint32_t AtomicOpcode(IR::ValueOpcode opcode) {
	switch (opcode) {
		case IR::ValueOpcode::BufferAtomicSwap32:
		case IR::ValueOpcode::SharedAtomicSwap32:
		case IR::ValueOpcode::GdsAtomicSwap32: return OpAtomicExchange;
		case IR::ValueOpcode::BufferAtomicIAdd32:
		case IR::ValueOpcode::SharedAtomicIAdd32:
		case IR::ValueOpcode::GdsAtomicIAdd32: return OpAtomicIAdd;
		case IR::ValueOpcode::BufferAtomicISub32:
		case IR::ValueOpcode::SharedAtomicISub32:
		case IR::ValueOpcode::GdsAtomicISub32: return OpAtomicISub;
		case IR::ValueOpcode::BufferAtomicSMin32:
		case IR::ValueOpcode::SharedAtomicSMin32:
		case IR::ValueOpcode::GdsAtomicSMin32: return OpAtomicSMin;
		case IR::ValueOpcode::BufferAtomicUMin32:
		case IR::ValueOpcode::SharedAtomicUMin32:
		case IR::ValueOpcode::GdsAtomicUMin32: return OpAtomicUMin;
		case IR::ValueOpcode::BufferAtomicSMax32:
		case IR::ValueOpcode::SharedAtomicSMax32:
		case IR::ValueOpcode::GdsAtomicSMax32: return OpAtomicSMax;
		case IR::ValueOpcode::BufferAtomicUMax32:
		case IR::ValueOpcode::SharedAtomicUMax32:
		case IR::ValueOpcode::GdsAtomicUMax32: return OpAtomicUMax;
		case IR::ValueOpcode::BufferAtomicAnd32:
		case IR::ValueOpcode::SharedAtomicAnd32:
		case IR::ValueOpcode::GdsAtomicAnd32: return OpAtomicAnd;
		case IR::ValueOpcode::BufferAtomicOr32:
		case IR::ValueOpcode::SharedAtomicOr32:
		case IR::ValueOpcode::GdsAtomicOr32: return OpAtomicOr;
		case IR::ValueOpcode::BufferAtomicXor32:
		case IR::ValueOpcode::SharedAtomicXor32:
		case IR::ValueOpcode::GdsAtomicXor32: return OpAtomicXor;
		default: return 0;
	}
}

uint32_t EmitAtomic(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                    bool return_value) {
	const auto result =
	    EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		    const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		    return EmitValueOrZeroIfCondition(
		        ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index),
		        [&]() {
			        const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
			        const auto old   = ctx.state.builder.AllocateId();
			        const auto scope =
			            mem.kind == IR::ResourceKind::Lds ? ScopeWorkgroup : ScopeDevice;
			        ctx.state.builder.AddFunction(
			            {AtomicOpcode(inst.GetOpcode()), TypeU32(ctx.state), old,
			             EmitMemoryElementPointer(ctx.state, access.resource, access.index),
			             ConstantU32(ctx.state, scope), ConstantU32(ctx.state, MemorySemanticsNone),
			             value});
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
	return return_value ? result : ConstantU32(ctx.state, 0);
}

uint32_t FloatAtomicReplacement(EmitterState& state, uint32_t old, uint32_t source,
                                bool max_value) {
	struct OrderedBits {
		uint32_t nan;
		uint32_t zero;
		uint32_t key;
	};
	const auto classify = [&](uint32_t bits) {
		const auto value    = Unary(state, OpBitcast, TypeF32(state), bits);
		const auto cls      = EmitClassifyF32(state, value);
		const auto negative = Binary(
		    state, OpINotEqual, TypeBool(state),
		    Binary(state, OpBitwiseAnd, TypeU32(state), bits, ConstantU32(state, 0x80000000u)),
		    ConstantU32(state, 0));
		const auto negative_key = Unary(state, OpNot, TypeU32(state), bits);
		const auto positive_key =
		    Binary(state, OpBitwiseXor, TypeU32(state), bits, ConstantU32(state, 0x80000000u));
		return OrderedBits {cls.nan, cls.zero,
		                    Select(state, TypeU32(state), negative, negative_key, positive_key)};
	};
	const auto source_class = classify(source);
	const auto old_class    = classify(old);
	const auto unordered =
	    Binary(state, OpLogicalOr, TypeBool(state),
	           Binary(state, OpLogicalOr, TypeBool(state), source_class.nan, old_class.nan),
	           Binary(state, OpLogicalAnd, TypeBool(state), source_class.zero, old_class.zero));
	const auto ordered = Unary(state, OpLogicalNot, TypeBool(state), unordered);
	const auto compare = Binary(state, max_value ? OpUGreaterThan : OpULessThan, TypeBool(state),
	                            source_class.key, old_class.key);
	return Select(state, TypeU32(state),
	              Binary(state, OpLogicalAnd, TypeBool(state), ordered, compare), source, old);
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
				    return FloatAtomicReplacement(ctx.state, old, source, max_value);
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
			        {OpStore, ctx.scratch_u32_variable, ctx.Arg(inst, 2)});
			    const auto data = ctx.state.builder.AllocateId();
			    ctx.state.builder.AddFunction(
			        {OpLoad, TypeU32(ctx.state), data, ctx.scratch_u32_variable});
			    AtomicUpdate(
			        ctx.state, EmitMemoryElementPointer(ctx.state, access.resource, access.index),
			        mem.kind, [&](uint32_t old) {
				        const auto old_f = Unary(ctx.state, OpBitcast, TypeF32(ctx.state), old);
				        const auto compare_f =
				            Unary(ctx.state, OpBitcast, TypeF32(ctx.state), ctx.Arg(inst, 3));
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

uint32_t AppendConsume(ValueEmitContext& ctx, const IR::Inst& inst, bool gds, bool append) {
	auto&      state = ctx.state;
	const auto m0    = ctx.Arg(inst, 1);
	const auto base =
	    Binary(state, OpShiftRightLogical, TypeU32(state), m0, ConstantU32(state, 16));
	const auto size = Binary(state, OpBitwiseAnd, TypeU32(state), m0, ConstantU32(state, 0xffffu));
	const auto address =
	    Binary(state, OpIAdd, TypeU32(state), base, ConstantU32(state, ctx.Memory(inst).offset));
	const auto raw_index =
	    Binary(state, OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
	auto mem          = ctx.Memory(inst);
	mem.kind          = gds ? IR::ResourceKind::Gds : IR::ResourceKind::Lds;
	const auto access = PrepareMemoryResourceAccess(state, mem);
	const auto index  = EmitMemoryElementIndex(state, access, raw_index);
	const auto exec   = ctx.Arg(inst, 2);
	const auto ballot = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformBallot, TypeU32Vector(state, 4), ballot,
	                           ConstantU32(state, ScopeSubgroup), exec});
	const auto low  = state.builder.AllocateId();
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeExtract, TypeU32(state), low, ballot, 0});
	state.builder.AddFunction({OpCompositeExtract, TypeU32(state), high, ballot, 1});
	const auto count_low  = state.per_invocation_masks ? low : ctx.Arg(inst, 3);
	const auto count_high = state.per_invocation_masks ? high : ctx.Arg(inst, 4);
	const auto count =
	    Binary(state, OpIAdd, TypeU32(state), Unary(state, OpBitCount, TypeU32(state), count_low),
	           Unary(state, OpBitCount, TypeU32(state), count_high));
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformBallotFindLSB, TypeU32(state), first,
	                           ConstantU32(state, ScopeSubgroup), ballot});
	const auto is_first =
	    Binary(state, OpIEqual, TypeBool(state), EmitSubgroupLocalInvocationId(state), first);
	const auto storage_bounds = EmitMemoryElementInBounds(state, access, index);
	const auto m0_bounds =
	    gds ? Binary(state, OpINotEqual, TypeBool(state), size, ConstantU32(state, 0))
	        : Binary(state, OpULessThan, TypeBool(state),
	                 ConstantU32(state, ctx.Memory(inst).offset + 3u), size);
	const auto condition = AndCondition(
	    state, is_first, AndCondition(state, exec, AndCondition(state, storage_bounds, m0_bounds)));
	const auto atomic = EmitValueOrZeroIfCondition(state, condition, [&]() {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction({append ? OpAtomicIAdd : OpAtomicISub, TypeU32(state), value,
		                           EmitMemoryElementPointer(state, access, index),
		                           ConstantU32(state, gds ? ScopeDevice : ScopeWorkgroup),
		                           ConstantU32(state, MemorySemanticsNone), count});
		return value;
	});
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformShuffle, TypeU32(state), result,
	                           ConstantU32(state, ScopeSubgroup), atomic, first});
	return result;
}

} // namespace

bool EmitValueMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto op    = inst.GetOpcode();
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
	const bool load_address = op == IR::ValueOpcode::LoadAddressU8 ||
	                          op == IR::ValueOpcode::LoadAddressU16 ||
	                          op == IR::ValueOpcode::LoadAddressU32;
	const bool load_buffer  = op == IR::ValueOpcode::LoadBufferU8 ||
	                          op == IR::ValueOpcode::LoadBufferU16 ||
	                          op == IR::ValueOpcode::LoadBufferU32;
	const bool load_shared =
	    op == IR::ValueOpcode::LoadSharedU8 || op == IR::ValueOpcode::LoadSharedU16 ||
	    op == IR::ValueOpcode::LoadSharedU32 || op == IR::ValueOpcode::LoadGdsU8 ||
	    op == IR::ValueOpcode::LoadGdsU16 || op == IR::ValueOpcode::LoadGdsU32;
	if (load_address || load_buffer || load_shared) {
		auto mem = ctx.Memory(inst);
		if (load_shared)
			mem.kind = op == IR::ValueOpcode::LoadGdsU8 || op == IR::ValueOpcode::LoadGdsU16 ||
			                   op == IR::ValueOpcode::LoadGdsU32
			               ? IR::ResourceKind::Gds
			               : IR::ResourceKind::Lds;
		uint32_t value = 0;
		if (op == IR::ValueOpcode::LoadBufferU32 && mem.formatted)
			value = FormattedLoad(ctx, inst, mem);
		else if (op == IR::ValueOpcode::LoadAddressU8 || op == IR::ValueOpcode::LoadBufferU8 ||
		         op == IR::ValueOpcode::LoadSharedU8 || op == IR::ValueOpcode::LoadGdsU8)
			value = LoadSubword(ctx, inst, mem, 8, false);
		else if (op == IR::ValueOpcode::LoadAddressU16 || op == IR::ValueOpcode::LoadBufferU16 ||
		         op == IR::ValueOpcode::LoadSharedU16 || op == IR::ValueOpcode::LoadGdsU16)
			value = LoadSubword(ctx, inst, mem, 16, false);
		else
			value = LoadWord(ctx, inst, mem);
		ctx.Define(inst, value);
		return true;
	}
	const bool store_address = op == IR::ValueOpcode::StoreAddressU8 ||
	                           op == IR::ValueOpcode::StoreAddressU16 ||
	                           op == IR::ValueOpcode::StoreAddressU32;
	const bool store_buffer  = op == IR::ValueOpcode::StoreBufferU8 ||
	                           op == IR::ValueOpcode::StoreBufferU16 ||
	                           op == IR::ValueOpcode::StoreBufferU32;
	const bool store_shared =
	    op == IR::ValueOpcode::WriteSharedU8 || op == IR::ValueOpcode::WriteSharedU16 ||
	    op == IR::ValueOpcode::WriteSharedU32 || op == IR::ValueOpcode::WriteGdsU8 ||
	    op == IR::ValueOpcode::WriteGdsU16 || op == IR::ValueOpcode::WriteGdsU32;
	if (store_address || store_buffer || store_shared) {
		auto mem = ctx.Memory(inst);
		if (store_shared)
			mem.kind = op == IR::ValueOpcode::WriteGdsU8 || op == IR::ValueOpcode::WriteGdsU16 ||
			                   op == IR::ValueOpcode::WriteGdsU32
			               ? IR::ResourceKind::Gds
			               : IR::ResourceKind::Lds;
		if (op == IR::ValueOpcode::StoreBufferU32 && mem.formatted)
			FormattedStore(ctx, inst, mem);
		else if (op == IR::ValueOpcode::StoreAddressU8 || op == IR::ValueOpcode::StoreBufferU8 ||
		         op == IR::ValueOpcode::WriteSharedU8 || op == IR::ValueOpcode::WriteGdsU8)
			StoreSubword(ctx, inst, mem, 8);
		else if (op == IR::ValueOpcode::StoreAddressU16 || op == IR::ValueOpcode::StoreBufferU16 ||
		         op == IR::ValueOpcode::WriteSharedU16 || op == IR::ValueOpcode::WriteGdsU16)
			StoreSubword(ctx, inst, mem, 16);
		else
			StoreWord(ctx, inst, mem);
		return true;
	}
	const auto atomic_opcode = AtomicOpcode(op);
	if (atomic_opcode != 0) {
		auto mem = ctx.Memory(inst);
		if (op >= IR::ValueOpcode::SharedAtomicSwap32 && op <= IR::ValueOpcode::SharedAtomicXor32)
			mem.kind = IR::ResourceKind::Lds;
		else if (op >= IR::ValueOpcode::GdsAtomicSwap32 && op <= IR::ValueOpcode::GdsAtomicXor32)
			mem.kind = IR::ResourceKind::Gds;
		ctx.Define(inst, EmitAtomic(ctx, inst, mem, true));
		return true;
	}
	if (op == IR::ValueOpcode::BufferAtomicFMin32 || op == IR::ValueOpcode::BufferAtomicFMax32) {
		ctx.Define(inst, FloatAtomic(ctx, inst, ctx.Memory(inst),
		                             op == IR::ValueOpcode::BufferAtomicFMax32));
		return true;
	}
	if (op == IR::ValueOpcode::SharedAtomicFMin32 || op == IR::ValueOpcode::SharedAtomicFMax32 ||
	    op == IR::ValueOpcode::GdsAtomicFMin32 || op == IR::ValueOpcode::GdsAtomicFMax32) {
		auto mem = ctx.Memory(inst);
		mem.kind = op == IR::ValueOpcode::GdsAtomicFMin32 || op == IR::ValueOpcode::GdsAtomicFMax32
		               ? IR::ResourceKind::Gds
		               : IR::ResourceKind::Lds;
		SharedFloatAtomic(ctx, inst, mem,
		                  op == IR::ValueOpcode::SharedAtomicFMax32 ||
		                      op == IR::ValueOpcode::GdsAtomicFMax32);
		return true;
	}
	if (op == IR::ValueOpcode::DataAppend || op == IR::ValueOpcode::DataConsume ||
	    op == IR::ValueOpcode::GdsDataAppend || op == IR::ValueOpcode::GdsDataConsume) {
		ctx.Define(inst, AppendConsume(ctx, inst,
		                               op == IR::ValueOpcode::GdsDataAppend ||
		                                   op == IR::ValueOpcode::GdsDataConsume,
		                               op == IR::ValueOpcode::DataAppend ||
		                                   op == IR::ValueOpcode::GdsDataAppend));
		return true;
	}
	if (op == IR::ValueOpcode::SwizzleU32) {
		ctx.state.builder.AddFunction({OpStore, ctx.scratch_u32_variable, ctx.Arg(inst, 0)});
		const auto source = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(
		    {OpLoad, TypeU32(ctx.state), source, ctx.scratch_u32_variable});
		const auto target =
		    EmitDsSwizzleTargetLane(state, EmitSubgroupLocalInvocationId(state),
		                            inst.Arg(1).IsImmediate() ? inst.Arg(1).U32() : 0);
		const auto shuffled = state.builder.AllocateId();
		state.builder.AddFunction({OpGroupNonUniformShuffle, TypeU32(state), shuffled,
		                           ConstantU32(state, ScopeSubgroup), source, target});
		const auto source_exec = state.builder.AllocateId();
		state.builder.AddFunction({OpGroupNonUniformShuffle, TypeBool(state), source_exec,
		                           ConstantU32(state, ScopeSubgroup), ctx.Arg(inst, 2), target});
		const auto source_active =
		    AndCondition(state, source_exec, EmitSubgroupLaneActiveBool(state, target));
		ctx.Define(inst,
		           Select(state, TypeU32(state), source_active, shuffled, ConstantU32(state, 0)));
		return true;
	}
	return false;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
