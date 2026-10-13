#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t AndCondition(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	return Binary(state, spv::OpLogicalAnd, TypeBool(state), lhs, rhs);
}

uint32_t EmitDsMaskedLaneRead(EmitterState& state, uint32_t source, uint32_t target,
                              uint32_t exec) {
	if (state.lane_count == 2) {
		target = Binary(state, spv::OpBitwiseAnd, TypeU32(state), target, ConstantU32(state, 31));
	}
	const auto shuffled = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeU32(state), shuffled,
	                          ConstantU32(state, spv::ScopeSubgroup), source, target);
	const auto source_exec = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeBool(state), source_exec,
	                          ConstantU32(state, spv::ScopeSubgroup), exec, target);
	const auto source_active =
	    AndCondition(state, source_exec, EmitSubgroupLaneActiveBool(state, target));
	return Select(state, TypeU32(state), source_active, shuffled, ConstantU32(state, 0));
}

struct BufferAddress {
	uint32_t offset;
	uint32_t byte;
};


BufferAddress CalculateBufferAddress(EmitterState& state, uint32_t index, uint32_t offset,
                                     uint32_t soffset, uint32_t immediate, uint32_t stride,
                                     uint32_t swizzle, uint32_t index_stride) {
	const auto zero = ConstantU32(state, 0);
	const auto one  = ConstantU32(state, 1);
	const auto add = [&](uint32_t lhs, uint32_t rhs) {
		return lhs == zero ? rhs : rhs == zero ? lhs
		                                      : Binary(state, spv::OpIAdd, TypeU32(state), lhs, rhs);
	};
	const auto mul = [&](uint32_t lhs, uint32_t rhs) {
		return lhs == zero || rhs == zero ? zero
		       : lhs == one              ? rhs
		       : rhs == one              ? lhs
		                                 : Binary(state, spv::OpIMul, TypeU32(state), lhs, rhs);
	};
	if (immediate != 0u) {
		offset = add(offset, ConstantU32(state, immediate));
	}
	auto address = add(mul(index, stride), offset);
	if (swizzle != 0u) {
		const auto index_shift =
		    Binary(state, spv::OpIAdd, TypeU32(state), index_stride, ConstantU32(state, 3));
		const auto indices = Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
		                            ConstantU32(state, 1), index_shift);
		const auto index_msb =
		    Binary(state, spv::OpShiftRightLogical, TypeU32(state), index, index_shift);
		const auto index_lsb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), index,
		           Binary(state, spv::OpISub, TypeU32(state), indices, ConstantU32(state, 1)));
		const auto offset_msb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, ~3u));
		const auto offset_lsb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, 3u));
		const auto msb = mul(add(mul(index_msb, stride), offset_msb), indices);
		const auto lsb = add(Binary(state, spv::OpShiftLeftLogical, TypeU32(state), index_lsb,
		                            ConstantU32(state, 2u)), offset_lsb);
		address = Select(state, TypeU32(state), swizzle, add(msb, lsb), address);
	}
	return {offset, add(address, soffset)};
}

uint32_t BufferLane(EmitterState& state) {
	return Binary(state, spv::OpBitwiseAnd, TypeU32(state), EmitSubgroupLocalInvocationId(state),
	              ConstantU32(state, 63));
}

uint32_t BufferByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto&      state  = ctx.state;
	const auto packed = StorageBufferPackedStride(state, mem);
	const auto stride = packed & 0x3fffu;
	auto       index  = ctx.Arg(inst, 1);
	if ((packed & (1u << 20u)) != 0u) {
		index = Binary(state, spv::OpIAdd, TypeU32(state), index, BufferLane(state));
	}
	const bool swizzle = stride != 0u && (packed & (1u << 14u)) != 0u;
	return CalculateBufferAddress(state, index, ctx.Arg(inst, 2), ctx.Arg(inst, 3), mem.offset,
	                              ConstantU32(state, stride),
	                              swizzle ? ConstantBool(state, true) : 0u,
	                              ConstantU32(state, (packed >> 16u) & 3u))
	    .byte;
}

uint32_t AddU64Low(EmitterState& state, uint32_t low, uint32_t high, uint32_t add_low,
                   uint32_t add_high, uint32_t& out_high) {
	const auto result = Binary(state, spv::OpIAdd, TypeU32(state), low, add_low);
	const auto carry  = Binary(state, spv::OpULessThan, TypeBool(state), result, low);
	out_high =
	    Binary(state, spv::OpIAdd, TypeU32(state),
	           Binary(state, spv::OpIAdd, TypeU32(state), high, add_high),
	           Select(state, TypeU32(state), carry, ConstantU32(state, 1), ConstantU32(state, 0)));
	return result;
}

uint32_t AddAddressOffset(EmitterState& state, const IR::MemoryInfo& mem, uint32_t low,
                          uint32_t& high) {
	const auto immediate = static_cast<int32_t>(mem.offset);
	if (immediate == 0) return low;
	const auto immediate_low  = ConstantU32(state, static_cast<uint32_t>(immediate));
	const auto immediate_high = ConstantU32(state, immediate < 0 ? UINT32_MAX : 0u);
	return AddU64Low(state, low, high, immediate_low, immediate_high, high);
}

uint32_t ScratchByteAddress(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t low,
                            uint32_t high) {
	auto& state = ctx.state;
	low = AddAddressOffset(state, mem, low, high);
	const auto valid = Binary(state, spv::OpIEqual, TypeBool(state), high, ConstantU32(state, 0));
	return Select(state, TypeU32(state), valid, low, ConstantU32(state, UINT32_MAX));
}

uint32_t GuestAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto& state = ctx.state;
	auto  low   = ctx.Arg(inst, 1);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		low = Binary(state, spv::OpBitwiseAnd, TypeU32(state), low, ConstantU32(state, ~3u));
	}
	uint32_t address = 0;
	if (mem.address_is_full) {
		address = PackU64(state, low, ctx.Arg(inst, 2));
	} else {
		const auto* handle = inst.Arg(0).Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != IR::ValueOpcode::GetAddressResource ||
		    handle->NumArgs() != 2) {
			ctx.Fail(inst, "has no address base pair");
			return ConstantU64(state, 0);
		}
		auto base_low = ctx.Arg(*handle, 0);
		if (mem.kind == IR::ResourceKind::ScalarAddress) {
			base_low = Binary(state, spv::OpBitwiseAnd, TypeU32(state), base_low,
			                  ConstantU32(state, ~3u));
		}
		const auto base = PackU64(state, base_low, ctx.Arg(*handle, 1));
		address         = Binary(state, spv::OpIAdd, TypeU64(state), base,
		                         Unary(state, spv::OpUConvert, TypeU64(state), low));
	}
	auto immediate = static_cast<int32_t>(mem.offset);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
	}
	return immediate == 0
	           ? address
	           : Binary(state, spv::OpIAdd, TypeU64(state), address,
	                    ConstantU64(state, static_cast<uint64_t>(static_cast<int64_t>(immediate))));
}

uint32_t FaultElementPointer(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                          state.fault_buffer_variable, ConstantU32(state, 0), index);
	return pointer;
}

void RecordBdaFault(EmitterState& state, uint32_t page) {
	const auto word =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), page, ConstantU32(state, 5));
	const auto bit =
	    Binary(state, spv::OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
	           Binary(state, spv::OpBitwiseAnd, TypeU32(state), page, ConstantU32(state, 31)));
	// Lanes faulting pages of one word set their bits together; a load/or/store loses some.
	const auto pointer = FaultElementPointer(state, word);
	const auto old     = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAtomicOr, TypeU32(state), old, pointer,
	                          ConstantU32(state, spv::ScopeDevice),
	                          ConstantU32(state, spv::MemorySemanticsMaskNone), bit);
}

uint32_t GetBdaStorePointer(ValueEmitContext& ctx, uint32_t address) {
	auto&      state  = ctx.state;
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeU64(state), result,
	                          state.bda_store_pointer_function, address);
	return result;
}

// glc rides along as Volatile beside the alignment the raw pointer path already declares.
uint32_t BdaAccessMask(bool coherent) {
	return coherent ? (spv::MemoryAccessAlignedMask | spv::MemoryAccessVolatileMask)
	                : spv::MemoryAccessAlignedMask;
}

// glc rides as Volatile through a Coherent member, and the barrier keeps a coherent read from
// being hoisted.
uint32_t LoadBdaDword(ValueEmitContext& ctx, uint32_t address, bool coherent = false) {
	auto&      state   = ctx.state;
	const auto bda     = GetBdaPointer(state, address);
	const auto present =
	    Binary(state, spv::OpINotEqual, TypeBool(state), bda, ConstantU64(state, 0));
	return EmitValueOrZeroIfCondition(state, present, [&]() {
		if (coherent) {
			EmitAtomicMemoryBarrier(state, IR::ResourceKind::Buffer);
		}
		auto pointer = state.builder.AllocateId();
		if (coherent) {
			const auto block = state.builder.DecoratedType(
			    spv::OpTypeStruct, {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
			                        {spv::OpMemberDecorate, {0, spv::DecorationCoherent}},
			                        {spv::OpDecorate, {spv::DecorationBlock}}}, TypeU32(state));
			state.builder.AddFunction(
			    spv::OpConvertUToPtr, TypePointer(state, spv::StorageClassPhysicalStorageBuffer, block),
			    pointer, bda);
			const auto base = pointer;
			pointer = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpAccessChain, TypePhysicalU32Pointer(state), pointer,
			                          base, ConstantU32(state, 0));
		} else {
			state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer,
			                          bda);
		}
		const auto         value     = state.builder.AllocateId();
		constexpr uint32_t alignment = sizeof(uint32_t);
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer,
		                          BdaAccessMask(coherent), alignment);
		return value;
	});
}

// The address form: the caller has already applied the access's active mask.
uint32_t LoadBdaAddress(ValueEmitContext& ctx, uint32_t address, uint32_t bits, bool coherent) {
	auto& state = ctx.state;
	{
		const auto aligned = Binary(state, spv::OpBitwiseAnd, TypeU64(state), address,
		                            ConstantU64(state, ~uint64_t {3}));
		const auto first   = LoadBdaDword(ctx, aligned, coherent);
		const auto byte =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		           Unary(state, spv::OpUConvert, TypeU32(state), address), ConstantU32(state, 3));
		const auto crosses =
		    bits == 8u ? ConstantBool(state, false)
		               : Binary(state, bits == 16u ? spv::OpUGreaterThan : spv::OpINotEqual,
		                        TypeBool(state), byte, ConstantU32(state, bits == 16u ? 2u : 0u));
		const auto second = EmitValueOrZeroIfCondition(state, crosses, [&]() {
			return LoadBdaDword(ctx,
			                    Binary(state, spv::OpIAdd, TypeU64(state), aligned,
			                           ConstantU64(state, sizeof(uint32_t))),
			                    coherent);
		});
		const auto shift =
		    Binary(state, spv::OpShiftLeftLogical, TypeU32(state), byte, ConstantU32(state, 3));
		const auto upper_shift =
		    Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
		           Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		                  Binary(state, spv::OpISub, TypeU32(state), ConstantU32(state, 4), byte),
		                  ConstantU32(state, 3)),
		           ConstantU32(state, 3));
		const auto merged =
		    Binary(state, spv::OpBitwiseOr, TypeU32(state),
		           Binary(state, spv::OpShiftRightLogical, TypeU32(state), first, shift),
		           Binary(state, spv::OpShiftLeftLogical, TypeU32(state), second, upper_shift));
		return bits == 32u ? merged
		                   : Binary(state, spv::OpBitwiseAnd, TypeU32(state), merged,
		                            ConstantU32(state, bits == 8u ? 0xffu : 0xffffu));
	}
}

uint32_t LoadBda(ValueEmitContext& ctx, uint32_t address, uint32_t active, uint32_t bits,
                 bool coherent) {
	return EmitValueOrZeroIfCondition(
	    ctx.state, active, [&]() { return LoadBdaAddress(ctx, address, bits, coherent); });
}

// An unmapped page faults and drops the write; an untracked mapped page faults but keeps it.
void StoreBdaDword(ValueEmitContext& ctx, uint32_t address, uint32_t data, bool coherent) {
	auto&      state   = ctx.state;
	const auto bda     = GetBdaStorePointer(ctx, address);
	const auto present = Binary(state, spv::OpINotEqual, TypeBool(state), bda,
	                            ConstantU64(state, 0));
	EmitIfCondition(state, present, [&]() {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer,
		                          bda);
		state.builder.AddFunction(spv::OpStore, pointer, data, BdaAccessMask(coherent),
		                          static_cast<uint32_t>(sizeof(uint32_t)));
	});
}

// Not atomic: two lanes touching different bytes of one dword can lose a write, where hardware
// merges them in the memory pipeline.
void StoreBdaMasked(ValueEmitContext& ctx, uint32_t address, uint32_t mask, uint32_t value,
                    bool coherent) {
	auto&      state   = ctx.state;
	const auto bda     = GetBdaStorePointer(ctx, address);
	const auto present = Binary(state, spv::OpINotEqual, TypeBool(state), bda,
	                            ConstantU64(state, 0));
	EmitIfCondition(state, present, [&]() {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer,
		                          bda);
		const auto old = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), old, pointer,
		                          BdaAccessMask(coherent), static_cast<uint32_t>(sizeof(uint32_t)));
		const auto kept = Binary(state, spv::OpBitwiseAnd, TypeU32(state), old,
		                         Unary(state, spv::OpNot, TypeU32(state), mask));
		state.builder.AddFunction(spv::OpStore, pointer,
		                          Binary(state, spv::OpBitwiseOr, TypeU32(state), kept, value),
		                          BdaAccessMask(coherent), static_cast<uint32_t>(sizeof(uint32_t)));
	});
}

// The address form: the caller has already applied the access's active mask.
void StoreBdaAddress(ValueEmitContext& ctx, uint32_t address, uint32_t bits, uint32_t data,
                     bool coherent) {
	auto&      state = ctx.state;
	const auto u32   = TypeU32(state);
	{
		const auto aligned = Binary(state, spv::OpBitwiseAnd, TypeU64(state), address,
		                            ConstantU64(state, ~uint64_t {3}));
		const auto byte    = Binary(state, spv::OpBitwiseAnd, u32,
		                            Unary(state, spv::OpUConvert, u32, address),
		                            ConstantU32(state, 3));
		const auto shift =
		    Binary(state, spv::OpShiftLeftLogical, u32, byte, ConstantU32(state, 3));
		const auto width   = ConstantU32(state, bits == 8u    ? 0xffu
		                                        : bits == 16u ? 0xffffu
		                                                      : 0xffffffffu);
		const auto payload =
		    bits == 32u ? data : Binary(state, spv::OpBitwiseAnd, u32, data, width);
		const auto next_dword = [&]() {
			return Binary(state, spv::OpIAdd, TypeU64(state), aligned,
			              ConstantU64(state, sizeof(uint32_t)));
		};
		const auto upper_shift = [&]() {
			return Binary(state, spv::OpShiftLeftLogical, u32,
			              Binary(state, spv::OpISub, u32, ConstantU32(state, 4), byte),
			              ConstantU32(state, 3));
		};
		if (bits == 32u) {
			const auto is_aligned =
			    Binary(state, spv::OpIEqual, TypeBool(state), byte, ConstantU32(state, 0));
			EmitIfCondition(state, is_aligned,
			                [&]() { StoreBdaDword(ctx, aligned, payload, coherent); });
			EmitIfCondition(
			    state, Unary(state, spv::OpLogicalNot, TypeBool(state), is_aligned), [&]() {
				    StoreBdaMasked(ctx, aligned,
				                   Binary(state, spv::OpShiftLeftLogical, u32, width, shift),
				                   Binary(state, spv::OpShiftLeftLogical, u32, payload, shift),
				                   coherent);
				    const auto upper = upper_shift();
				    StoreBdaMasked(ctx, next_dword(),
				                   Binary(state, spv::OpShiftRightLogical, u32, width, upper),
				                   Binary(state, spv::OpShiftRightLogical, u32, payload, upper),
				                   coherent);
			    });
			return;
		}
		StoreBdaMasked(ctx, aligned, Binary(state, spv::OpShiftLeftLogical, u32, width, shift),
		               Binary(state, spv::OpShiftLeftLogical, u32, payload, shift), coherent);
		if (bits == 8u) {
			return;
		}
		const auto crosses =
		    Binary(state, spv::OpUGreaterThan, TypeBool(state), byte, ConstantU32(state, 2));
		EmitIfCondition(state, crosses, [&]() {
			const auto upper = upper_shift();
			StoreBdaMasked(ctx, next_dword(),
			               Binary(state, spv::OpShiftRightLogical, u32, width, upper),
			               Binary(state, spv::OpShiftRightLogical, u32, payload, upper), coherent);
		});
	}
}

void StoreBda(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
              uint32_t bits) {
	const auto address = GuestAddress(ctx, inst, mem);
	const auto data    = ctx.Arg(inst, inst.NumArgs() - 2);
	const auto active  = ctx.Arg(inst, inst.NumArgs() - 1);
	EmitIfCondition(ctx.state, active,
	                [&]() { StoreBdaAddress(ctx, address, bits, data, CoherentBufferAccess(mem)); });
}

struct DynamicBufferElement {
	uint32_t address   = 0;
	uint32_t in_bounds = 0;
};

DynamicBufferElement DynamicBufferAccess(ValueEmitContext& ctx, const IR::Inst& inst,
                                         const IR::MemoryInfo& mem, uint32_t component_offset,
                                         uint32_t bytes) {
	auto&       state  = ctx.state;
	const auto* handle = inst.Arg(0).Resolve().TryInstruction();
	if (handle == nullptr || handle->GetOpcode() != IR::ValueOpcode::GetBufferResource) {
		ctx.Fail(inst, "dynamic buffer has no GetBufferResource handle");
		return {ConstantU64(state, 0), ConstantBool(state, false)};
	}
	const auto word1   = ctx.Arg(*handle, 1);
	const auto records = ctx.Arg(*handle, 2);
	const auto word3   = ctx.Arg(*handle, 3);
	const auto field   = [&](uint32_t word, uint32_t first, uint32_t count) {
		return EmitBitFieldUExtract(state, word, ConstantU32(state, first),
		                            ConstantU32(state, count));
	};
	const auto nonzero = [&](uint32_t value) {
		return Binary(state, spv::OpINotEqual, TypeBool(state), value, ConstantU32(state, 0));
	};
	const auto has_bytes = [&](uint32_t offset, uint32_t size) {
		return AndCondition(
		    state,
		    Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), size,
		           ConstantU32(state, bytes)),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state), offset,
		           Binary(state, spv::OpISub, TypeU32(state), size, ConstantU32(state, bytes))));
	};
	const auto stride       = field(word1, 16, 14);
	const auto swizzle      = AndCondition(state, nonzero(stride), nonzero(field(word1, 31, 1)));
	const auto index_stride = field(word3, 21, 2);
	const auto add_tid      = nonzero(field(word3, 23, 1));
	const auto index =
	    Binary(state, spv::OpIAdd, TypeU32(state), ctx.Arg(inst, 1),
	           Select(state, TypeU32(state), add_tid, BufferLane(state), ConstantU32(state, 0)));
	const auto soffset = ctx.Arg(inst, 3);
	const auto address =
	    CalculateBufferAddress(state, index, ctx.Arg(inst, 2), soffset,
	                           mem.offset + component_offset, stride, swizzle, index_stride);
	const auto mode            = field(word3, 28, 2);
	const auto index_in_bounds = Binary(state, spv::OpULessThan, TypeBool(state), index, records);
	const auto structured_bounds =
	    AndCondition(state, index_in_bounds,
	                 Binary(state, spv::OpULessThan, TypeBool(state), address.offset, stride));
	const auto raw_records = Binary(state, spv::OpISub, TypeU32(state), records, soffset);
	const auto raw_bounds  = AndCondition(
	    state, Binary(state, spv::OpULessThanEqual, TypeBool(state), soffset, records),
	    Select(state, TypeBool(state), swizzle,
	           AndCondition(state,
	                        Binary(state, spv::OpULessThan, TypeBool(state), index, raw_records),
	                        has_bytes(address.offset, stride)),
	           has_bytes(address.offset, raw_records)));
	auto in_bounds =
	    Select(state, TypeBool(state),
	           Binary(state, spv::OpIEqual, TypeBool(state), mode, ConstantU32(state, 0)),
	           structured_bounds, index_in_bounds);
	in_bounds = Select(
	    state, TypeBool(state),
	    Binary(state, spv::OpULessThan, TypeBool(state), mode, ConstantU32(state, 2)), in_bounds,
	    Select(state, TypeBool(state),
	           Binary(state, spv::OpIEqual, TypeBool(state), mode, ConstantU32(state, 2)),
	           nonzero(records), raw_bounds));
	const auto base = PackU64(state, ctx.Arg(*handle, 0), field(word1, 0, 16));
	return {Binary(state, spv::OpIAdd, TypeU64(state), base,
	               Unary(state, spv::OpUConvert, TypeU64(state), address.byte)),
	        AndCondition(state, nonzero(field(word3, 12, 7)), in_bounds)};
}

// DWORD buffer accesses are forced to DWORD alignment, as the native path's DwordIndex is.
uint32_t AlignedDwordAddress(EmitterState& state, uint32_t address) {
	return Binary(state, spv::OpBitwiseAnd, TypeU64(state), address,
	              ConstantU64(state, ~uint64_t {3}));
}

// S_BUFFER_LOAD ignores index, swizzle and ADD_TID; the stride only scales the range check.
uint32_t ReadDynamicConstBuffer(ValueEmitContext& ctx, const IR::Inst& inst,
                                const IR::MemoryInfo& mem) {
	auto&       state  = ctx.state;
	const auto* handle = inst.Arg(0).Resolve().TryInstruction();
	if (handle == nullptr || handle->GetOpcode() != IR::ValueOpcode::GetBufferResource) {
		ctx.Fail(inst, "dynamic buffer has no GetBufferResource handle");
		return ConstantU32(state, 0);
	}
	const auto u64    = TypeU64(state);
	const auto word1  = ctx.Arg(*handle, 1);
	// Scalar buffer addressing aligns the base and each offset independently, without wrapping.
	auto offset_64 = Unary(
	    state, spv::OpUConvert, u64,
	    Binary(state, spv::OpBitwiseAnd, TypeU32(state), ctx.Arg(inst, 1), ConstantU32(state, ~3u)));
	if ((mem.offset & ~3u) != 0u) {
		offset_64 = Binary(state, spv::OpIAdd, u64, offset_64,
		                   ConstantU64(state, mem.offset & ~3u));
	}
	const auto stride =
	    Unary(state, spv::OpUConvert, u64,
	          EmitBitFieldUExtract(state, word1, ConstantU32(state, 16u), ConstantU32(state, 14u)));
	const auto records = Unary(state, spv::OpUConvert, u64, ctx.Arg(*handle, 2));
	const auto size =
	    Select(state, u64,
	           Binary(state, spv::OpIEqual, TypeBool(state), stride, ConstantU64(state, 0)),
	           records, Binary(state, spv::OpIMul, u64, records, stride));
	const auto in_bounds =
	    Binary(state, spv::OpULessThanEqual, TypeBool(state),
	           Binary(state, spv::OpIAdd, u64, offset_64, ConstantU64(state, 4u)), size);
	return EmitValueOrZeroIfCondition(state, in_bounds, [&]() {
		const auto base = PackU64(
		    state, ctx.Arg(*handle, 0),
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), word1, ConstantU32(state, 0xffffu)));
		const auto address =
		    Binary(state, spv::OpBitwiseAnd, u64, Binary(state, spv::OpIAdd, u64, base, offset_64),
		           ConstantU64(state, ~uint64_t {3}));
		return LoadBdaDword(ctx, address, mem.glc);
	});
}

template <typename Fn>
uint32_t EmitDynamicBufferAtomic(ValueEmitContext& ctx, const IR::Inst& inst,
                                 const IR::MemoryInfo& mem, uint32_t result_type, uint32_t zero,
                                 uint32_t pointer_type, uint32_t alignment, Fn&& operation) {
	auto& state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), result_type, zero, [&]() {
		    const auto element = DynamicBufferAccess(ctx, inst, mem, 0u, alignment);
		    return EmitValueOrDefaultIfCondition(
		        state, element.in_bounds, result_type, zero, [&]() {
			        const auto address = Binary(
			            state, spv::OpBitwiseAnd, TypeU64(state), element.address,
			            ConstantU64(state, ~static_cast<uint64_t>(alignment - 1u)));
			        const auto bda     = GetBdaPointer(ctx.state, address);
			        const auto present = Binary(state, spv::OpINotEqual, TypeBool(state), bda,
			                                    ConstantU64(state, 0));
			        return EmitValueOrDefaultIfCondition(state, present, result_type, zero, [&]() {
				        const auto pointer = state.builder.AllocateId();
				        state.builder.AddFunction(spv::OpConvertUToPtr, pointer_type, pointer, bda);
				        return operation(pointer);
			        });
		        });
	    });
}

uint32_t ByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	if (mem.kind == IR::ResourceKind::Buffer) {
		auto&      state   = ctx.state;
		const bool dynamic = state.dynamic_buffer_index != 0 &&
		                     mem.resource == state.dynamic_buffer_resource;
		const auto prefix =
		    dynamic ? state.dynamic_buffer_byte_offset
		            : state.memory_byte_offsets[state.program.info.buffers.at(mem.resource)
		                                            .descriptor_index];
		const auto address = Binary(state, spv::OpIAdd, TypeU32(state),
		                            BufferByteAddress(ctx, inst, mem), prefix);
		// The host binding prefix must not wrap an out-of-range guest address into the buffer.
		return Select(state, TypeU32(state),
		              Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), address, prefix),
		              address, ConstantU32(state, UINT32_MAX));
	}
	if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
		if (mem.offset == 0u) {
			return ctx.Arg(inst, 0);
		}
		return Binary(ctx.state, spv::OpIAdd, TypeU32(ctx.state), ctx.Arg(inst, 0),
		              ConstantU32(ctx.state, mem.offset));
	}
	if (mem.kind != IR::ResourceKind::Scratch) {
		EXIT("physical address memory must use the BDA emitter\n");
	}
	return ScratchByteAddress(ctx, mem, ctx.Arg(inst, 1), ctx.Arg(inst, 2));
}

uint32_t DwordIndex(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto address = ByteAddress(ctx, inst, mem);
	if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
		// RDNA2 DS region addresses retain bits [15:2] after adding the byte offset.
		address = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
		                 ConstantU32(ctx.state, 0xffffu));
	}
	return Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state),
	              address, ConstantU32(ctx.state, 2));
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index, bool is_volatile = false);

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend,
                             bool is_volatile = false);

bool DeviceBoundsChecked(const EmitterState& state, const MemoryResourceAccess& resource) {
	return state.program.robust_buffer_access2 && (resource.kind == IR::ResourceKind::Buffer ||
	                                               resource.kind == IR::ResourceKind::ScalarBuffer);
}

uint32_t LoadWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource) {
	const auto index = DwordIndex(ctx, inst, mem);
	if (DeviceBoundsChecked(ctx.state, resource)) {
		return LoadWordInBounds(ctx, resource, index, mem.glc);
	}
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index),
	    [&]() { return LoadWordInBounds(ctx, resource, index, mem.glc); });
}

uint32_t LoadWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		if (mem.dynamic_buffer) {
			const auto element = DynamicBufferAccess(ctx, inst, mem, 0u, 4u);
			return LoadBda(ctx, AlignedDwordAddress(ctx.state, element.address),
			               element.in_bounds, 32u, mem.glc);
		}
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadWordPrepared(ctx, inst, mem, resource);
	});
}

uint32_t LoadSubwordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                             const MemoryResourceAccess& resource, uint32_t bits,
                             bool sign_extend) {
	const auto address = ByteAddress(ctx, inst, mem);
	const auto index   = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	                            ConstantU32(ctx.state, std::countr_zero(resource.element_bits / 8u)));
	if (DeviceBoundsChecked(ctx.state, resource)) {
		return LoadSubwordInBounds(ctx, resource, address, index, bits, sign_extend, mem.glc);
	}
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		    return LoadSubwordInBounds(ctx, resource, address, index, bits, sign_extend, mem.glc);
	    });
}

// A glc access becomes Volatile: never hoisted, never served from a non-coherent cache.
uint32_t LoadResourceWord(EmitterState& state, const MemoryResourceAccess& resource,
                          uint32_t value, uint32_t pointer) {
	if (resource.coherent) {
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer,
		                          spv::MemoryAccessVolatileMask);
	} else {
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	}
	return value;
}

void StoreResourceWord(EmitterState& state, const MemoryResourceAccess& resource,
                       uint32_t pointer, uint32_t data) {
	if (resource.coherent) {
		state.builder.AddFunction(spv::OpStore, pointer, data, spv::MemoryAccessVolatileMask);
	} else {
		state.builder.AddFunction(spv::OpStore, pointer, data);
	}
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index, bool is_volatile) {
	if (is_volatile) {
		if (resource.kind == IR::ResourceKind::Lds) {
			const auto semantics =
			    spv::MemorySemanticsAcquireReleaseMask | spv::MemorySemanticsWorkgroupMemoryMask;
			ctx.state.builder.AddFunction(spv::OpMemoryBarrier,
			                              ConstantU32(ctx.state, spv::ScopeWorkgroup),
			                              ConstantU32(ctx.state, semantics));
		} else {
			EmitAtomicMemoryBarrier(ctx.state, IR::ResourceKind::Buffer);
		}
	}
	const auto value   = ctx.state.builder.AllocateId();
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), value, pointer,
	                              resource.memory_access);
	return value;
}

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend,
                             bool is_volatile) {
	uint32_t value;
	if (resource.element_bits < 32u) {
		const auto loaded = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpLoad,
		                              ctx.state.storage_buffers[bits == 8u ? 0 : 1].element_type, loaded,
		                              EmitMemoryElementPointer(ctx.state, resource, index),
		                              resource.memory_access);
		value = Unary(ctx.state, spv::OpUConvert, TypeU32(ctx.state), loaded);
	} else {
		const auto word  = LoadWordInBounds(ctx, resource, index, is_volatile);
		const auto byte  = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
		                          ConstantU32(ctx.state, 3));
		const auto shift = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), byte,
		                          ConstantU32(ctx.state, 3));
		value = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state),
		               Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), word, shift),
		               ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu));
	}
	if (!sign_extend) return value;
	const auto left = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), value,
	                         ConstantU32(ctx.state, 32u - bits));
	return Binary(ctx.state, spv::OpShiftRightArithmetic, TypeU32(ctx.state), left,
	              ConstantU32(ctx.state, 32u - bits));
}

uint32_t LoadSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits,
                     bool sign_extend) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		if (mem.dynamic_buffer) {
			const auto element = DynamicBufferAccess(ctx, inst, mem, 0u, bits / 8u);
			const auto value   = LoadBda(ctx, element.address, element.in_bounds, bits, mem.glc);
			if (!sign_extend) return value;
			const auto left = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), value,
			                         ConstantU32(ctx.state, 32u - bits));
			return Binary(ctx.state, spv::OpShiftRightArithmetic, TypeU32(ctx.state), left,
			              ConstantU32(ctx.state, 32u - bits));
		}
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadSubwordPrepared(ctx, inst, mem, resource, bits, sign_extend);
	});
}

Prospero::BufferFormat BufferFormat(const ValueEmitContext& ctx, const IR::MemoryInfo& mem) {
	return mem.typed ? Format::DecodeTBufferFormat(mem.data_format, mem.number_format)
	                 : StorageBufferFormat(ctx.state, mem);
}

IR::MemoryInfo RebaseRawComponent(IR::MemoryInfo mem, uint32_t component) {
	mem.offset += component * 4u;
	mem.data_dwords     = 1u;
	mem.component_index = component;
	return mem;
}

using Format::FormattedSource;
using Format::FormattedSourceKind;

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
	const auto source   = Format::ResolveFormattedSource(info, selector);
	if (source.kind == FormattedSourceKind::Invalid) {
		ExitDescriptorBindingFailure(ctx.state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "buffer descriptor has reserved dst_sel");
	}
	return source;
}

uint32_t FormattedConstant(ValueEmitContext& ctx, const Format::BufferFormatInfo& info,
                           FormattedSourceKind kind) {
	return ConstantU32(ctx.state, Format::FormattedConstantBits(info, kind));
}

template <typename LoadWordFn, typename LoadSubwordFn>
uint32_t LoadFormattedComponent(ValueEmitContext& ctx, const Format::BufferFormatInfo& info,
                                FormattedSource source, LoadWordFn&& load_word,
                                LoadSubwordFn&& load_subword) {
	if (source.kind != FormattedSourceKind::Memory) {
		return FormattedConstant(ctx, info, source.kind);
	}
	const auto component = source.component;
	const auto bits      = info.component_bits[component];
	auto raw = info.packed_bitfield || bits == 32u ? load_word(component)
	                                                : load_subword(component, bits);
	if (info.packed_bitfield || (bits < 32u && IsSignedFormatComponent(info.type))) {
		const auto type =
		    IsSignedFormatComponent(info.type) ? TypeI32(ctx.state) : TypeU32(ctx.state);
		const auto source_value =
		    type == TypeI32(ctx.state) ? Unary(ctx.state, spv::OpBitcast, type, raw) : raw;
		const auto extracted = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(IsSignedFormatComponent(info.type) ? spv::OpBitFieldSExtract
		                                                                 : spv::OpBitFieldUExtract,
		                              type, extracted, source_value,
		                              ConstantU32(ctx.state, info.packed_bitfield
		                                                         ? info.component_bit_offset[component]
		                                                         : 0u),
		                              ConstantU32(ctx.state, bits));
		raw = type == TypeI32(ctx.state)
		          ? Unary(ctx.state, spv::OpBitcast, TypeU32(ctx.state), extracted)
		          : extracted;
	}
	return NormalizeFormatComponent(ctx.state, info, component, raw);
}

void StoreSubwordInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource, uint32_t address, uint32_t index,
                          uint32_t bits, uint32_t data) {
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	if (resource.element_bits < 32u) {
		ctx.state.builder.AddFunction(
		    spv::OpStore, pointer,
		    Unary(ctx.state, spv::OpUConvert,
		          ctx.state.storage_buffers[bits == 8u ? 0 : 1].element_type, data),
		    resource.memory_access);
		return;
	}
	const auto shift   = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                            Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
	                                   ConstantU32(ctx.state, 3)),
	                            ConstantU32(ctx.state, 3));
	const auto mask    = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                            ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu), shift);
	const auto value   = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                            Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), data,
	                                   ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu)),
	                            shift);
	const auto merge   = [&](uint32_t old) {
		return Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state),
		              Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), old,
		                     Unary(ctx.state, spv::OpNot, TypeU32(ctx.state), mask)),
		              value);
	};
	if (mem.kind == IR::ResourceKind::Scratch) {
		const auto old = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), old, pointer);
		ctx.state.builder.AddFunction(spv::OpStore, pointer, merge(old));
	} else {
		AtomicUpdate(ctx.state, pointer, mem.kind, merge);
	}
}

void StoreSubwordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource, uint32_t bits, uint32_t data) {
	const auto address = ByteAddress(ctx, inst, mem);
	const auto index   = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	                            ConstantU32(ctx.state, std::countr_zero(resource.element_bits / 8u)));
	// A wider element is stored by an atomic loop, which robustness leaves undefined out of range.
	if (DeviceBoundsChecked(ctx.state, resource) && resource.element_bits < 32u) {
		StoreSubwordInBounds(ctx, mem, resource, address, index, bits, data);
		return;
	}
	EmitIfCondition(ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		StoreSubwordInBounds(ctx, mem, resource, address, index, bits, data);
	});
}

void StoreSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		if (mem.dynamic_buffer) {
			const auto element = DynamicBufferAccess(ctx, inst, mem, 0u, bits / 8u);
			EmitIfCondition(ctx.state, element.in_bounds, [&]() {
				StoreBdaAddress(ctx, element.address, bits, ctx.Arg(inst, inst.NumArgs() - 2),
				                mem.glc);
			});
			return;
		}
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		StoreSubwordPrepared(ctx, inst, mem, resource, bits, ctx.Arg(inst, inst.NumArgs() - 2));
	});
}

void StoreWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource, uint32_t index,
                       uint32_t data) {
	ctx.state.builder.AddFunction(spv::OpStore,
	                              EmitMemoryElementPointer(ctx.state, resource, index), data,
	                              resource.memory_access);
}

template <typename Fn>
void ForEachLocalFlatAccess(ValueEmitContext& ctx, const IR::Inst& inst, Fn&& emit) {
	auto& state = ctx.state;
	auto mem = ctx.Memory(inst);
	auto high = ctx.Arg(inst, 2);
	const auto low = AddAddressOffset(state, mem, ctx.Arg(inst, 1), high);
	const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), low, ConstantU32(state, 2));
	const auto aligned = Binary(state, spv::OpIEqual, TypeBool(state), ConstantU32(state, 0),
	    Binary(state, spv::OpBitwiseAnd, TypeU32(state), low, ConstantU32(state, 3)));
	for (const auto kind: {IR::ResourceKind::Scratch, IR::ResourceKind::Lds}) {
		mem.kind = kind;
		const auto resource = PrepareMemoryResourceAccess(state, mem);
		const auto aperture = kind == IR::ResourceKind::Scratch ? Decoder::PrivateApertureHigh
		                                                       : Decoder::SharedApertureHigh;
		const auto selected =
		    Binary(state, spv::OpIEqual, TypeBool(state), high, ConstantU32(state, aperture));
		const auto valid = AndCondition(state, aligned,
		    AndCondition(state, selected, EmitMemoryElementInBounds(state, resource, index)));
		emit(resource, index, valid);
	}
}

uint32_t LoadLocalFlat(ValueEmitContext& ctx, const IR::Inst& inst) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		uint32_t result = 0;
		ForEachLocalFlatAccess(ctx, inst, [&](const auto& resource, uint32_t index, uint32_t valid) {
			const auto value = EmitValueOrZeroIfCondition(ctx.state, valid, [&]() {
				return LoadWordInBounds(ctx, resource, index);
			});
			result = result == 0 ? value : Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state), result, value);
		});
		return result;
	});
}

void StoreLocalFlat(ValueEmitContext& ctx, const IR::Inst& inst) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		ForEachLocalFlatAccess(ctx, inst, [&](const auto& resource, uint32_t index, uint32_t valid) {
			EmitIfCondition(ctx.state, valid, [&]() {
				StoreWordInBounds(ctx, resource, index, ctx.Arg(inst, inst.NumArgs() - 2));
			});
		});
	});
}

void StoreWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                       const MemoryResourceAccess& resource, uint32_t data) {
	const auto index = DwordIndex(ctx, inst, mem);
	if (DeviceBoundsChecked(ctx.state, resource)) {
		StoreWordInBounds(ctx, resource, index, data);
		return;
	}
	EmitIfCondition(ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		StoreWordInBounds(ctx, resource, index, data);
	});
}

void StoreWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		if (mem.dynamic_buffer) {
			const auto element = DynamicBufferAccess(ctx, inst, mem, 0u, 4u);
			EmitIfCondition(ctx.state, element.in_bounds, [&]() {
				StoreBdaDword(ctx, AlignedDwordAddress(ctx.state, element.address),
				              ctx.Arg(inst, inst.NumArgs() - 2), mem.glc);
			});
			return;
		}
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		StoreWordPrepared(ctx, inst, mem, resource, ctx.Arg(inst, inst.NumArgs() - 2));
	});
}


// The trailing barrier is the acquire edge only; the last-child idiom needs the release too.
uint32_t AtomicSemantics(const IR::MemoryInfo& mem) {
	return spv::MemorySemanticsAcquireReleaseMask |
	       (mem.kind == IR::ResourceKind::Lds ? spv::MemorySemanticsWorkgroupMemoryMask
	                                          : spv::MemorySemanticsUniformMemoryMask);
}

template <typename Fn>
uint32_t EmitAtomicAccess(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          Fn&& operation) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto index    = DwordIndex(ctx, inst, mem);
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return EmitValueOrZeroIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index),
		    [&]() { return operation(EmitMemoryElementPointer(ctx.state, resource, index)); });
	});
}

template <typename Fn>
uint32_t EmitAtomicUpdate(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          Fn&& replacement) {
	const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
	return EmitAtomicAccess(ctx, inst, mem, [&](uint32_t pointer) {
		return AtomicUpdate(ctx.state, pointer, mem.kind,
		                    [&](uint32_t old) { return replacement(ctx.state, old, value); });
	});
}

uint32_t AtomicIncrement(EmitterState& state, uint32_t old, uint32_t limit) {
	// old >= limit ? 0 : old + 1 (unsigned).
	const auto wrap = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), old, limit);
	const auto next = Binary(state, spv::OpIAdd, TypeU32(state), old, ConstantU32(state, 1));
	return Select(state, TypeU32(state), wrap, ConstantU32(state, 0), next);
}

uint32_t AtomicDecrement(EmitterState& state, uint32_t old, uint32_t limit) {
	// old == 0 || old > limit ? limit : old - 1 (unsigned).
	const auto zero  = Binary(state, spv::OpIEqual, TypeBool(state), old, ConstantU32(state, 0));
	const auto above = Binary(state, spv::OpUGreaterThan, TypeBool(state), old, limit);
	const auto wrap  = Binary(state, spv::OpLogicalOr, TypeBool(state), zero, above);
	const auto next  = Binary(state, spv::OpISub, TypeU32(state), old, ConstantU32(state, 1));
	return Select(state, TypeU32(state), wrap, limit, next);
}

struct PreparedFormattedMemory {
	Format::BufferFormatInfo info;
	MemoryResourceAccess     resource;
	uint32_t                 base = 0;
	uint32_t                 in_bounds = 0;
};

PreparedFormattedMemory PrepareFormattedMemory(ValueEmitContext& ctx, const IR::Inst& inst,
                                               const IR::MemoryInfo& mem,
                                               const MemoryResourceAccess& resource,
                                               const Format::BufferFormatInfo& info) {
	PreparedFormattedMemory plan;
	plan.info      = info;
	plan.resource  = resource;
	const auto start = ByteAddress(ctx, inst, mem);
	// Check the whole, unaligned format span before aligning the actual memory address.
	const auto end = Binary(ctx.state, spv::OpIAdd, TypeU32(ctx.state), start,
	                        ConstantU32(ctx.state, info.byte_size - 1u));
	const auto last_index = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), end,
	                               ConstantU32(ctx.state, std::countr_zero(resource.element_bits / 8u)));
	plan.in_bounds = AndCondition(
	    ctx.state, Binary(ctx.state, spv::OpUGreaterThanEqual, TypeBool(ctx.state), end, start),
	    EmitMemoryElementInBounds(ctx.state, plan.resource, last_index));
	plan.base = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), start,
	                   ConstantU32(ctx.state, ~(std::min(4u, info.byte_size) - 1u)));
	return plan;
}

std::pair<uint32_t, uint32_t> FormattedComponentAddress(EmitterState& state,
                                                       const PreparedFormattedMemory& plan,
                                                       uint32_t component) {
	// Components advance sequentially from one address; do not reapply buffer swizzling.
	const auto offset = Format::GetFormatComponentByteOffset(plan.info, component);
	const auto address = offset == 0u ? plan.base : Binary(
	    state, spv::OpIAdd, TypeU32(state), plan.base, ConstantU32(state, offset));
	return {address, Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
	                        ConstantU32(state, std::countr_zero(plan.resource.element_bits / 8u)))};
}

uint32_t LoadFormattedInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                               const PreparedFormattedMemory& plan, uint32_t output_component) {
	return LoadFormattedComponent(
	    ctx, plan.info, ResolveFormattedSource(ctx, mem, plan.info, output_component),
	    [&](uint32_t component) {
		    return LoadWordInBounds(ctx, plan.resource,
		                            FormattedComponentAddress(ctx.state, plan, component).second,
		                            mem.glc);
	    },
	    [&](uint32_t component, uint32_t bits) {
		    const auto [address, index] = FormattedComponentAddress(ctx.state, plan, component);
		    return LoadSubwordInBounds(ctx, plan.resource, address, index, bits, false, mem.glc);
	    });
}

uint32_t ConstructU32Composite(EmitterState& state, uint32_t components,
                               const std::array<uint32_t, 4>& values) {
	if (components == 1u) return values[0];
	const auto            result = state.builder.AllocateId();
	std::vector<uint32_t> words {spv::OpCompositeConstruct, TypeU32Composite(state, components),
	                             result};
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

uint32_t FormattedLoad(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		const auto info = Format::GetFormatInfo(BufferFormat(ctx, mem));
		if (info.type == Format::ComponentType::Unknown) {
			return LoadWordPrepared(ctx, inst, RebaseRawComponent(mem, 0u), resource);
		}
		const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info);
		const auto value = EmitValueOrDefaultIfCondition(
		    ctx.state, plan.in_bounds, TypeU32(ctx.state),
		    FormattedOutOfBoundsValue(ctx, mem, plan, 1u),
		    [&]() { return LoadFormattedInBounds(ctx, mem, plan, 0u); });
		if (mem.data_bits != 16u) {
			return value;
		}
		if (info.type == Format::ComponentType::Uint) {
			return EmitUMin32(ctx.state, value, ConstantU32(ctx.state, 0xffffu));
		}
		if (info.type == Format::ComponentType::Sint) {
			const auto lower = EmitSMax32(ctx.state, value, ConstantU32(ctx.state, 0xffff8000u));
			return EmitSMin32(ctx.state, lower, ConstantU32(ctx.state, 0x7fffu));
		}
		return EmitConvertF16F32(ctx.state, EmitBitCastF32U32(ctx.state, value));
	});
}

void StoreFormattedPrepared(ValueEmitContext& ctx, const IR::Inst& inst,
                             const IR::MemoryInfo& mem, const MemoryResourceAccess& resource,
                             const Format::BufferFormatInfo& info, uint32_t data,
                             uint32_t components) {
	// RDNA2 formatted stores transfer the entire format, zero-filling missing VGPRs.
	const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info);
	EmitIfCondition(ctx.state, plan.in_bounds, [&]() {
		auto packed = ConstantU32(ctx.state, 0);
		for (uint32_t component = 0; component < info.component_count; component++) {
			auto value = ConstantU32(ctx.state, 0);
			if (component < components) {
				value = data;
				if (components != 1u) {
					value = ctx.state.builder.AllocateId();
					ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state),
					                              value, data, component);
				}
			}
			// D16 stores carry the low half of each VGPR.
			if (mem.data_bits == 16u) {
				if (info.type == Format::ComponentType::Uint) {
					value = EmitBitFieldUExtract(ctx.state, value, ConstantU32(ctx.state, 0),
					                             ConstantU32(ctx.state, 16));
				} else if (info.type == Format::ComponentType::Sint) {
					value = EmitBitFieldSExtract(ctx.state, value, ConstantU32(ctx.state, 0),
					                             ConstantU32(ctx.state, 16));
				} else {
					value = EmitBitCastU32F32(ctx.state, EmitF16BitsToF32(ctx.state, value));
				}
			}
			value = EncodeFormatComponent(ctx.state, info, component, value);
			const auto bits = info.component_bits[component];
			if (info.packed_bitfield) {
				packed = EmitBitFieldInsert(
				    ctx.state, packed, value,
				    ConstantU32(ctx.state, info.component_bit_offset[component]),
				    ConstantU32(ctx.state, bits));
			} else {
				const auto [address, index] = FormattedComponentAddress(ctx.state, plan, component);
				if (bits == 8u || bits == 16u) {
					StoreSubwordInBounds(ctx, mem, plan.resource, address, index, bits, value);
				} else {
					StoreWordInBounds(ctx, plan.resource, index, value);
				}
			}
		}
		if (info.packed_bitfield) {
			StoreWordInBounds(ctx, plan.resource,
			                  FormattedComponentAddress(ctx.state, plan, 0u).second, packed);
		}
	});
}

void FormattedStore(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		const auto data     = ctx.Arg(inst, inst.NumArgs() - 2);
		const auto info     = Format::GetFormatInfo(BufferFormat(ctx, mem));
		if (info.type == Format::ComponentType::Unknown) {
			StoreWordPrepared(ctx, inst, RebaseRawComponent(mem, 0u), resource, data);
			return;
		}
		StoreFormattedPrepared(ctx, inst, mem, resource, info, data, 1u);
	});
}

uint32_t LoadIndirectScalarBuffer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&       state  = ctx.state;
	const auto& handle = *inst.Arg(0).ResolveInstruction();
	const auto  word1  = ctx.Arg(handle, 1);
	const auto  stride = EmitBitFieldUExtract(state, word1, ConstantU32(state, 16),
	                                         ConstantU32(state, 14));
	const auto  size   = Binary(state, spv::OpIMul, TypeU64(state),
	                            Unary(state, spv::OpUConvert, TypeU64(state),
	                                  EmitUMax32(state, stride, ConstantU32(state, 1))),
	                            Unary(state, spv::OpUConvert, TypeU64(state), ctx.Arg(handle, 2)));
	const auto  offset = Binary(state, spv::OpIAdd, TypeU64(state),
	                            Unary(state, spv::OpUConvert, TypeU64(state),
	                                  Binary(state, spv::OpBitwiseAnd, TypeU32(state),
	                                         ctx.Arg(inst, 1), ConstantU32(state, ~3u))),
	                            ConstantU64(state, ctx.Memory(inst).offset & ~3u));
	const auto  end =
	    Binary(state, spv::OpIAdd, TypeU64(state), offset, ConstantU64(state, sizeof(uint32_t)));
	return EmitValueOrZeroIfCondition(
	    state, Binary(state, spv::OpULessThanEqual, TypeBool(state), end, size), [&]() {
		    const auto base = PackU64(
		        state, Binary(state, spv::OpBitwiseAnd, TypeU32(state), ctx.Arg(handle, 0),
		                      ConstantU32(state, ~3u)),
		        Binary(state, spv::OpBitwiseAnd, TypeU32(state), word1, ConstantU32(state, 0xffffu)));
		    return LoadBdaDword(ctx, Binary(state, spv::OpIAdd, TypeU64(state), base, offset),
		                        CoherentBufferAccess(ctx.Memory(inst)));
	    });
}

struct IndirectBufferAccess {
	uint32_t address;
	uint32_t offset;
	uint32_t stride;
	uint32_t swizzle;
	uint32_t format;
	uint32_t selector;
	uint32_t mode;
	uint32_t records;
	uint32_t index_in_bounds;
	uint32_t scalar_in_bounds;
	uint32_t raw_records;
	uint32_t raw_index_in_bounds;
};

IndirectBufferAccess PrepareIndirectBuffer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&       state  = ctx.state;
	const auto& handle = *inst.Arg(0).ResolveInstruction();
	const auto word1   = ctx.Arg(handle, 1);
	const auto records = ctx.Arg(handle, 2);
	const auto word3   = ctx.Arg(handle, 3);
	const auto field = [&](uint32_t word, uint32_t first, uint32_t count) {
		return EmitBitFieldUExtract(state, word, ConstantU32(state, first), ConstantU32(state, count));
	};
	const auto nonzero = [&](uint32_t value) {
		return Binary(state, spv::OpINotEqual, TypeBool(state), value, ConstantU32(state, 0));
	};
	const auto stride  = field(word1, 16, 14);
	const auto swizzle = AndCondition(state, nonzero(stride), nonzero(field(word1, 31, 1)));
	const auto index = Binary(
	    state, spv::OpIAdd, TypeU32(state), ctx.Arg(inst, 1),
	    Select(state, TypeU32(state), nonzero(field(word3, 23, 1)), BufferLane(state),
	           ConstantU32(state, 0)));
	const auto soffset = ctx.Arg(inst, 3);
	const auto address = CalculateBufferAddress(state, index, ctx.Arg(inst, 2), soffset,
	                                            ctx.Memory(inst).offset, stride, swizzle,
	                                            field(word3, 21, 2));
	const auto base        = PackU64(state, ctx.Arg(handle, 0), field(word1, 0, 16));
	const auto raw_records = Binary(state, spv::OpISub, TypeU32(state), records, soffset);
	return {
	    .address          = Binary(state, spv::OpIAdd, TypeU64(state), base,
	                               Unary(state, spv::OpUConvert, TypeU64(state), address.byte)),
	    .offset           = address.offset,
	    .stride           = stride,
	    .swizzle          = swizzle,
	    .format           = field(word3, 12, 7),
	    .selector         = ctx.Memory(inst).formatted ? field(word3, 0, 3) : 0u,
	    .mode             = field(word3, 28, 2),
	    .records          = records,
	    .index_in_bounds  = Binary(state, spv::OpULessThan, TypeBool(state), index, records),
	    .scalar_in_bounds = Binary(state, spv::OpULessThanEqual, TypeBool(state), soffset, records),
	    .raw_records      = raw_records,
	    .raw_index_in_bounds = Binary(state, spv::OpULessThan, TypeBool(state), index, raw_records),
	};
}

uint32_t IndirectBufferInBounds(EmitterState& state, const IndirectBufferAccess& buffer,
                                uint32_t displacement, uint32_t bytes, bool formatted) {
	const auto offset = displacement == 0u ? buffer.offset : Binary(
	    state, spv::OpIAdd, TypeU32(state), buffer.offset, ConstantU32(state, displacement));
	const auto has_payload = [&](uint32_t size) {
		return AndCondition(
		    state, Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), size,
		                  ConstantU32(state, bytes)),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state), offset,
		           Binary(state, spv::OpISub, TypeU32(state), size, ConstantU32(state, bytes))));
	};
	const auto structured = AndCondition(
	    state, buffer.index_in_bounds,
	    formatted ? has_payload(buffer.stride)
	              : Binary(state, spv::OpULessThan, TypeBool(state), offset, buffer.stride));
	// OOB_SELECT=3 reduces NUM_RECORDS by SOFFSET before its offset/index checks.
	const auto raw = AndCondition(
	    state, buffer.scalar_in_bounds,
	    Select(state, TypeBool(state), buffer.swizzle,
	           AndCondition(state, buffer.raw_index_in_bounds, has_payload(buffer.stride)),
	           has_payload(buffer.raw_records)));
	auto in_bounds = Select(
	    state, TypeBool(state),
	    Binary(state, spv::OpIEqual, TypeBool(state), buffer.mode, ConstantU32(state, 0)),
	    structured, buffer.index_in_bounds);
	in_bounds = Select(
	    state, TypeBool(state),
	    Binary(state, spv::OpULessThan, TypeBool(state), buffer.mode, ConstantU32(state, 2)),
	    in_bounds,
	    Select(state, TypeBool(state),
	           Binary(state, spv::OpIEqual, TypeBool(state), buffer.mode, ConstantU32(state, 2)),
	           Binary(state, spv::OpINotEqual, TypeBool(state), buffer.records, ConstantU32(state, 0)),
	           raw));
	return in_bounds;
}

uint32_t LoadIndirectBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	const auto buffer = PrepareIndirectBuffer(ctx, inst);
	const auto valid_format = Binary(state, spv::OpINotEqual, TypeBool(state), buffer.format,
	                                  ConstantU32(state, 0));
	const auto base         = Binary(state, spv::OpBitwiseAnd, TypeU64(state), buffer.address,
	                                 ConstantU64(state, ~uint64_t {3}));
	std::array<uint32_t, 4> values {};
	for (uint32_t component = 0; component < components; ++component) {
		const auto address = component == 0u ? base
		                                     : Binary(state, spv::OpIAdd, TypeU64(state), base,
		                                              ConstantU64(state, component * 4u));
		values[component] = EmitValueOrZeroIfCondition(
		    state, AndCondition(state, valid_format,
		                        IndirectBufferInBounds(state, buffer, component * 4u, 4u, false)),
		    [&]() { return LoadBdaDword(ctx, address, CoherentBufferAccess(ctx.Memory(inst))); });
	}
	return ConstructU32Composite(state, components, values);
}

uint32_t LoadIndirectFormattedX(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	return EmitValueOrZeroIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto buffer = PrepareIndirectBuffer(ctx, inst);
		return EmitIndexSwitch(
		    state, buffer.format, static_cast<uint32_t>(Prospero::BufferFormat::k32_32_32_32Float) + 1u,
		    TypeU32(state), [&](uint32_t format) {
			    const auto info = Format::GetFormatInfo(static_cast<Prospero::BufferFormat>(format));
			    if (info.type == Format::ComponentType::Unknown) return ConstantU32(state, 0);
			    const auto constant = Select(
			        state, TypeU32(state),
			        Binary(state, spv::OpIEqual, TypeBool(state), buffer.selector, ConstantU32(state, 1)),
			        FormattedConstant(ctx, info, FormattedSourceKind::One), ConstantU32(state, 0));
			    return EmitValueOrDefaultIfCondition(
			        state, Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), buffer.selector,
			                      ConstantU32(state, 4)),
			        TypeU32(state), constant, [&]() {
				        const auto in_bounds = IndirectBufferInBounds(state, buffer, 0u, info.byte_size, true);
				        const auto base = Binary(
				            state, spv::OpBitwiseAnd, TypeU64(state), buffer.address,
				            ConstantU64(state, ~(uint64_t(std::min(4u, info.byte_size)) - 1u)));
				        const auto load = [&](uint32_t component, uint32_t bits) {
					        const auto offset = Format::GetFormatComponentByteOffset(info, component);
					        const auto address = offset == 0u ? base : Binary(
					            state, spv::OpIAdd, TypeU64(state), base,
					            ConstantU64(state, offset));
					        return LoadBda(ctx, address, in_bounds, bits,
					                       CoherentBufferAccess(ctx.Memory(inst)));
				        };
				        const auto emit_component = [&](uint32_t component) {
					        return LoadFormattedComponent(
					            ctx, info, {FormattedSourceKind::Memory, component},
					            [&](uint32_t source) { return load(source, 32u); }, load);
				        };
				        if (info.component_count == 1u) return emit_component(0u);
				        const auto component = Binary(
				            state, spv::OpUMod, TypeU32(state),
				            Binary(state, spv::OpISub, TypeU32(state), buffer.selector, ConstantU32(state, 4)),
				            ConstantU32(state, info.component_count));
				        return EmitIndexSwitch(state, component, info.component_count, TypeU32(state),
				                               emit_component);
			        });
		    });
	});
}

uint32_t LoadWideBuffer(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                        uint32_t components) {
	auto& state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU32Composite(state, components),
	    ConstantU32CompositeZero(state, components), [&]() {
		    if (mem.kind == IR::ResourceKind::IndirectBuffer) {
			    return LoadIndirectBuffer(ctx, inst, components);
		    }
		    if (mem.dynamic_buffer) {
			    // RDNA2 range-checks each dword of a raw vector on its own.
			    std::array<uint32_t, 4> values {};
			    for (uint32_t component = 0; component < components; component++) {
				    const auto element = DynamicBufferAccess(ctx, inst, mem, component * 4u, 4u);
				    values[component] = LoadBda(ctx, AlignedDwordAddress(state, element.address),
				                                element.in_bounds, 32u, mem.glc);
			    }
			    return ConstructU32Composite(state, components, values);
		    }
		    const auto resource = PrepareMemoryResourceAccess(state, mem);
		    const auto info     = Format::GetFormatInfo(
		        mem.formatted ? BufferFormat(ctx, mem) : Prospero::BufferFormat::kInvalid);
		    if (info.type != Format::ComponentType::Unknown) {
			    const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info);
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
		    // A glc DWORDX2 is one 64-bit access: look-back scans poll a SWAP_X2 pair.
		    if (components == 2 && mem.glc && mem.kind == IR::ResourceKind::Buffer &&
		        state.storage_buffers[3].variable != 0) {
			    const auto u32          = TypeU32(state);
			    const auto byte_address = ByteAddress(ctx, inst, mem);
			    const auto aligned      = Binary(
			        state, spv::OpIEqual, TypeBool(state),
			        Binary(state, spv::OpBitwiseAnd, u32, byte_address, ConstantU32(state, 7u)),
			        ConstantU32(state, 0u));
			    const auto wide_resource =
			        PrepareStorageBufferResourceAccess(state, mem, state.storage_buffers[3]);
			    const auto index = Binary(state, spv::OpShiftRightLogical, u32, byte_address,
			                              ConstantU32(state, 3u));
			    const auto wide  = EmitValueOrDefaultIfCondition(
			        state,
			        Binary(state, spv::OpLogicalAnd, TypeBool(state), aligned,
			               EmitMemoryElementInBounds(state, wide_resource, index)),
			        TypeU64(state), ConstantU64(state, 0), [&]() {
				        const auto pointer = EmitStorageBufferElementPointer(
				            state, wide_resource, index,
				            TypeStorageBufferElementPointer(state, 64));
				        const auto loaded = state.builder.AllocateId();
				        state.builder.AddFunction(
				            spv::OpAtomicLoad, TypeU64(state), loaded, pointer,
				            ConstantU32(state, spv::ScopeDevice),
				            ConstantU32(state, spv::MemorySemanticsAcquireMask |
				                                   spv::MemorySemanticsUniformMemoryMask));
				        return loaded;
			        });
			    const auto low  = Unary(state, spv::OpUConvert, u32, wide);
			    const auto high = Unary(state, spv::OpUConvert, u32,
			                            Binary(state, spv::OpShiftRightLogical, TypeU64(state),
			                                   wide, ConstantU32(state, 32u)));
			    values[0]       = Select(state, u32, aligned, low, values[0]);
			    values[1]       = Select(state, u32, aligned, high, values[1]);
		    }
		    return ConstructU32Composite(state, components, values);
	    });
}

void StoreWideBuffer(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                     uint32_t components) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto composite = ctx.Arg(inst, inst.NumArgs() - 2);
		if (mem.dynamic_buffer) {
			for (uint32_t component = 0; component < components; component++) {
				const uint32_t c    = mem.glc ? (components - 1u - component) : component;
				const auto     data = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), data, composite,
				                          c);
				const auto element = DynamicBufferAccess(ctx, inst, mem, c * 4u, 4u);
				EmitIfCondition(state, element.in_bounds,
				                [&]() {
					                StoreBdaDword(ctx, AlignedDwordAddress(state, element.address),
					                              data, mem.glc);
				                });
			}
			return;
		}
		const auto resource = PrepareMemoryResourceAccess(state, mem);
		const auto info = Format::GetFormatInfo(mem.formatted ? BufferFormat(ctx, mem)
		                                                      : Prospero::BufferFormat::kInvalid);
		if (info.type != Format::ComponentType::Unknown) {
			StoreFormattedPrepared(ctx, inst, mem, resource, info, composite, components);
			return;
		}
		const auto store_words = [&]() {
			for (uint32_t component = 0; component < components; component++) {
				const uint32_t c    = mem.glc ? (components - 1u - component) : component;
				const auto     data = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), data, composite,
				                          c);
				StoreWordPrepared(ctx, inst, RebaseRawComponent(mem, c), resource, data);
			}
		};
		// A glc DWORDX2 is one 64-bit access: look-back scans publish a {flag, value} pair with it.
		if (components == 2 && mem.glc && mem.kind == IR::ResourceKind::Buffer &&
		    state.storage_buffers[3].variable != 0) {
			const auto u32           = TypeU32(state);
			const auto u64           = TypeU64(state);
			const auto byte_address  = ByteAddress(ctx, inst, mem);
			const auto wide_resource =
			    PrepareStorageBufferResourceAccess(state, mem, state.storage_buffers[3]);
			const auto index =
			    Binary(state, spv::OpShiftRightLogical, u32, byte_address, ConstantU32(state, 3u));
			const auto whole = Binary(
			    state, spv::OpLogicalAnd, TypeBool(state),
			    Binary(state, spv::OpIEqual, TypeBool(state),
			           Binary(state, spv::OpBitwiseAnd, u32, byte_address, ConstantU32(state, 7u)),
			           ConstantU32(state, 0u)),
			    EmitMemoryElementInBounds(state, wide_resource, index));
			EmitIfCondition(state, whole, [&]() {
				std::array<uint32_t, 2> words {};
				for (uint32_t c = 0; c < 2; c++) {
					words[c] = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpCompositeExtract, u32, words[c], composite, c);
				}
				const auto value = Binary(
				    state, spv::OpBitwiseOr, u64, Unary(state, spv::OpUConvert, u64, words[0]),
				    Binary(state, spv::OpShiftLeftLogical, u64,
				           Unary(state, spv::OpUConvert, u64, words[1]), ConstantU32(state, 32u)));
				const auto pointer = EmitStorageBufferElementPointer(
				    state, wide_resource, index, TypeStorageBufferElementPointer(state, 64));
				state.builder.AddFunction(
				    spv::OpAtomicStore, pointer, ConstantU32(state, spv::ScopeDevice),
				    ConstantU32(state, spv::MemorySemanticsReleaseMask |
				                           spv::MemorySemanticsUniformMemoryMask),
				    value);
			});
			EmitIfCondition(state, Unary(state, spv::OpLogicalNot, TypeBool(state), whole),
			                store_words);
			return;
		}
		store_words();
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
			    const auto address   = component == 0u
			                               ? base
			                               : Binary(state, spv::OpIAdd, TypeU32(state), base,
			                                        ConstantU32(state, component * 4u));
			    const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state),
			                                  address, ConstantU32(state, 2));
			    values[component]    = EmitValueOrZeroIfCondition(
			        state, EmitMemoryElementInBounds(state, resource, index),
			        [&]() { return LoadWordInBounds(ctx, resource, index); });
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
			                                     : Binary(state, spv::OpIAdd, TypeU32(state), base,
			                                              ConstantU32(state, component * 4u));
			const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
			                              ConstantU32(state, 2));
			EmitIfCondition(state, EmitMemoryElementInBounds(state, resource, index),
			                [&]() {
				                StoreWordInBounds(ctx, resource, index, ctx.Arg(inst, component + 1u));
			                });
		}
	});
}

} // namespace

uint32_t GetBdaPointer(EmitterState& state, uint32_t address) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeU64(state), result,
	                          state.bda_pointer_function, address);
	return result;
}

namespace {

uint32_t BeginBdaPointerFunction(EmitterState& state, uint32_t function, const char* name,
                                 uint32_t& page, uint32_t& offset) {
	const auto type          = TypeU64(state);
	const auto function_type = state.builder.Type(spv::OpTypeFunction, type, type);
	const auto address       = state.builder.AllocateId();
	const auto entry_label   = state.builder.AllocateId();
	state.builder.AddName(function, name);
	state.builder.AddFunction(spv::OpFunction, type, function, spv::FunctionControlMaskNone,
	                          function_type);
	state.builder.AddFunction(spv::OpFunctionParameter, type, address);
	EmitLabel(state, entry_label);

	const auto extended = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), address,
	                             ConstantU64(state, LibKernel::Memory::kExtendedMemoryBase));
	const auto packed   = Select(
	    state, type, extended,
	    Binary(state, spv::OpISub, type, address,
	           ConstantU64(state, LibKernel::Memory::kExtendedMemoryBase - LOWER_ADDRESS_SIZE)),
	    address);
	const auto page64        = Binary(state, spv::OpShiftRightLogical, type, packed,
	                                  ConstantU64(state, BufferCache::CACHING_PAGEBITS));
	page                     = Unary(state, spv::OpUConvert, TypeU32(state), page64);
	offset                   = Binary(state, spv::OpBitwiseAnd, type, address,
	                                  ConstantU64(state, BufferCache::CACHING_PAGESIZE - 1));
	const auto entry_pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state, 64),
	                          entry_pointer, state.bda_pagetable_variable, ConstantU32(state, 0),
	                          page);
	const auto base = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, base, entry_pointer);
	return base;
}

uint32_t UntaggedBdaBase(EmitterState& state, uint32_t base) {
	return Binary(state, spv::OpBitwiseAnd, TypeU64(state), base,
	              ConstantU64(state, ~BufferCache::BDA_STORE_TRACKED_BIT));
}

// Faults on every page whose entry lacks the tracked tag, so the host learns which pages to own.
void DefineGetBdaStorePointer(EmitterState& state) {
	const auto type                  = TypeU64(state);
	state.bda_store_pointer_function = state.builder.AllocateId();
	uint32_t   page                  = 0;
	uint32_t   offset                = 0;
	const auto base = BeginBdaPointerFunction(state, state.bda_store_pointer_function,
	                                          "get_bda_store_pointer", page, offset);
	const auto untracked =
	    Binary(state, spv::OpIEqual, TypeBool(state),
	           Binary(state, spv::OpBitwiseAnd, type, base,
	                  ConstantU64(state, BufferCache::BDA_STORE_TRACKED_BIT)),
	           ConstantU64(state, 0));
	EmitIfCondition(state, untracked, [&]() { RecordBdaFault(state, page); });
	const auto missing =
	    Binary(state, spv::OpIEqual, TypeBool(state), base, ConstantU64(state, 0));
	const auto available = Binary(state, spv::OpIAdd, type, UntaggedBdaBase(state, base), offset);
	const auto result    = Select(state, type, missing, ConstantU64(state, 0), available);
	state.builder.AddFunction(spv::OpReturnValue, result);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace

void DefineGetBdaPointer(EmitterState& state) {
	if (!state.program.info.uses_dma) {
		return;
	}
	if (state.program.info.writes_dma) {
		DefineGetBdaStorePointer(state);
	}
	const auto type            = TypeU64(state);
	state.bda_pointer_function = state.builder.AllocateId();
	uint32_t   page            = 0;
	uint32_t   offset          = 0;
	const auto base =
	    BeginBdaPointerFunction(state, state.bda_pointer_function, "get_bda_pointer", page, offset);
	const auto missing =
	    Binary(state, spv::OpIEqual, TypeBool(state), base, ConstantU64(state, 0));
	const auto fault_label     = state.builder.AllocateId();
	const auto available_label = state.builder.AllocateId();
	const auto merge_label     = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, missing, fault_label, available_label);

	EmitLabel(state, fault_label);
	RecordBdaFault(state, page);
	if (state.bda_miss_variable != 0) {
		state.builder.AddFunction(spv::OpStore, state.bda_miss_variable, ConstantBool(state, true));
	}
	state.builder.AddFunction(spv::OpBranch, merge_label);

	EmitLabel(state, available_label);
	const auto available = Binary(state, spv::OpIAdd, type, UntaggedBdaBase(state, base), offset);
	state.builder.AddFunction(spv::OpBranch, merge_label);

	EmitLabel(state, merge_label);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpPhi, type, result, ConstantU64(state, 0), fault_label,
	                          available, available_label);
	state.builder.AddFunction(spv::OpReturnValue, result);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

uint32_t EmitBufferAtomic64For(ValueEmitContext& ctx, const IR::Inst& inst,
                               const IR::MemoryInfo& mem);

// Ordinal 0 is the arm for a key that matched nothing; materialization left it naming no memory.
bool IsIndirectBufferRoot(const ValueEmitContext& ctx, const IR::MemoryInfo& mem) {
	if ((mem.kind != IR::ResourceKind::Buffer && mem.kind != IR::ResourceKind::ScalarBuffer) ||
	    mem.planning_only || mem.dynamic_buffer || ctx.state.flattened_srt_variable == 0) {
		return false;
	}
	const auto& buffers = ctx.state.program.info.buffers;
	if (mem.resource >= buffers.size()) {
		return false;
	}
	const auto& buffer = buffers[mem.resource];
	return buffer.indirect_root == mem.resource && buffer.indirect_search_iterations != 0u &&
	       buffer.indirect_resources.size() >= 2u;
}

// Stride matters only for indexed/swizzled/lane-offset accesses, format only when formatted.
struct IndirectBufferShape {
	uint32_t               packed_stride      = 0;
	Prospero::BufferFormat descriptor_format  = Prospero::BufferFormat::kInvalid;
	uint32_t               descriptor_swizzle = 0;

	bool operator==(const IndirectBufferShape&) const = default;
};

IndirectBufferShape IndirectBufferShapeOf(const EmitterState& state, const IR::Inst& inst,
                                          const IR::MemoryInfo& mem, bool addressed,
                                          uint32_t resource) {
	const auto&         buffer = state.program.info.buffers[resource];
	IndirectBufferShape shape {};
	if (addressed) {
		const auto index    = inst.Arg(1).Resolve();
		const bool no_index = index.IsImmediate() && index.U32() == 0u &&
		                      (buffer.packed_stride & (1u << 20u)) == 0u;
		const bool swizzle =
		    (buffer.packed_stride & 0x3fffu) != 0u && (buffer.packed_stride & (1u << 14u)) != 0u;
		shape.packed_stride = no_index && !swizzle ? 0u : buffer.packed_stride;
	}
	if (mem.formatted) {
		shape.descriptor_format  = buffer.descriptor_format;
		shape.descriptor_swizzle = buffer.descriptor_swizzle;
	}
	return shape;
}

// Scopes the descriptor slot an access to `resource` indexes with instead of its own.
struct DynamicBufferSlot {
	DynamicBufferSlot(EmitterState& state_, uint32_t resource, uint32_t index, uint32_t byte_offset)
	    : state(state_) {
		state.dynamic_buffer_resource    = resource;
		state.dynamic_buffer_index       = index;
		state.dynamic_buffer_byte_offset = byte_offset;
	}
	~DynamicBufferSlot() {
		state.dynamic_buffer_index       = 0;
		state.dynamic_buffer_resource    = 0;
		state.dynamic_buffer_byte_offset = 0;
	}
	DynamicBufferSlot(const DynamicBufferSlot&)            = delete;
	DynamicBufferSlot& operator=(const DynamicBufferSlot&) = delete;

	EmitterState& state;
};

// A pointer cannot be phi'd in logical addressing, so each arm holds its whole access.
// Pass 0 as `result_type` for an access that produces no value.
template <typename Fn>
uint32_t EmitIndirectBufferSwitch(ValueEmitContext& ctx, const IR::Inst& inst,
                                  const IR::MemoryInfo& mem, uint32_t result_type, bool addressed,
                                  Fn&& emit_for) {
	auto&       state  = ctx.state;
	const auto& buffer = state.program.info.buffers[mem.resource];
	const auto* handle = inst.Arg(0).Resolve().TryInstruction();
	if (handle == nullptr || handle->NumArgs() == 0u) {
		ctx.Fail(inst, "indirect buffer access has no runtime key");
		return result_type != 0 ? ConstantU32(state, 0) : 0u;
	}
	const auto  u32        = TypeU32(state);
	const auto& candidates = buffer.indirect_resources;
	const auto  count      = static_cast<uint32_t>(candidates.size());
	const auto  key        = ctx.Def(handle->Arg(0));
	const auto  search_key = std::make_pair(
        state.current_block, std::array {state.lane_half, buffer.indirect_mapping_offset,
                                         buffer.indirect_search_iterations, key});
	auto found = 0u;
	if (const auto cached = state.block_key_searches.find(search_key);
	    state.current_block != nullptr && cached != state.block_key_searches.end()) {
		found = cached->second;
	} else {
		found = EmitUnrolledCandidateSearch(state, buffer.indirect_mapping_offset,
		                                    buffer.indirect_search_iterations, key);
		if (state.current_block != nullptr) {
			state.block_key_searches.emplace(search_key, found);
		}
	}

	// Ordinal order, so group 0 holds the root, the arm a key that matched nothing falls to.
	std::vector<uint32_t>            group_of(count, 0u);
	std::vector<uint32_t>            group_ordinal;
	std::vector<uint32_t>            group_size;
	std::vector<IndirectBufferShape> group_shape;
	for (uint32_t ordinal = 0; ordinal < count; ordinal++) {
		const auto shape = IndirectBufferShapeOf(state, inst, mem, addressed, candidates[ordinal]);
		const auto found_group = std::ranges::find(group_shape, shape);
		const auto group       = static_cast<uint32_t>(found_group - group_shape.begin());
		if (found_group == group_shape.end()) {
			group_shape.push_back(shape);
			group_ordinal.push_back(ordinal);
			group_size.push_back(0u);
		}
		group_of[ordinal] = group;
		group_size[group]++;
	}
	// Ordinal k must sit at the first candidate's slot + k - 1, or each candidate keeps an arm.
	bool indexed = count >= 3u && std::ranges::any_of(group_size, [](uint32_t size) {
		               return size > 1u;
	               });
	const auto first_slot =
	    count >= 2u ? state.program.info.buffers.at(candidates[1]).descriptor_index : 0u;
	for (uint32_t ordinal = 2; indexed && ordinal < count; ordinal++) {
		indexed = state.program.info.buffers.at(candidates[ordinal]).descriptor_index ==
		          first_slot + ordinal - 1u;
	}
	if (!indexed) {
		group_ordinal.resize(count);
		group_size.assign(count, 1u);
		for (uint32_t ordinal = 0; ordinal < count; ordinal++) {
			group_ordinal[ordinal] = ordinal;
			group_of[ordinal]      = ordinal;
		}
	}
	// Clamped: the slot indexes the descriptor array directly, with no default arm.
	const auto selected = indexed ? Select(state, u32,
	                                       Binary(state, spv::OpULessThan, TypeBool(state), found,
	                                              ConstantU32(state, count)),
	                                       found, ConstantU32(state, 0u))
	                              : found;
	uint32_t slot        = 0;
	uint32_t byte_offset = 0;
	if (indexed) {
		const auto root_slot = state.program.info.buffers.at(candidates[0]).descriptor_index;
		slot = Select(state, u32,
		              Binary(state, spv::OpIEqual, TypeBool(state), selected, ConstantU32(state, 0u)),
		              ConstantU32(state, root_slot),
		              Binary(state, spv::OpIAdd, u32, selected, ConstantU32(state, first_slot - 1u)));
		state.builder.AddAnnotation(spv::OpDecorate, slot, spv::DecorationNonUniform);
		state.builder.RequireExtension("SPV_EXT_descriptor_indexing");
		state.builder.RequireCapability(spv::CapabilityShaderNonUniform);
		state.builder.RequireCapability(spv::CapabilityStorageBufferArrayNonUniformIndexing);
		byte_offset = EmitDynamicMemoryByteOffset(state, slot);
	}
	const auto emit_arm = [&](uint32_t group) {
		auto per_candidate     = mem;
		per_candidate.resource = candidates[group_ordinal[group]];
		if (group_size[group] == 1u) {
			return emit_for(per_candidate);
		}
		const DynamicBufferSlot scope(state, per_candidate.resource, slot, byte_offset);
		return emit_for(per_candidate);
	};
	if (group_ordinal.size() == 1u) {
		return emit_arm(0u);
	}
	const auto            default_label = state.builder.AllocateId();
	const auto            merge_label   = state.builder.AllocateId();
	std::vector<uint32_t> labels(group_ordinal.size(), default_label);
	for (uint32_t group = 1; group < group_ordinal.size(); group++) {
		labels[group] = state.builder.AllocateId();
	}
	// Only an ordinal outside the root's group needs a literal; the rest reach it by default.
	std::vector<uint32_t> switch_words {spv::OpSwitch, selected, default_label};
	for (uint32_t ordinal = 1; ordinal < count; ordinal++) {
		if (group_of[ordinal] != 0u) {
			switch_words.push_back(ordinal);
			switch_words.push_back(labels[group_of[ordinal]]);
		}
	}
	state.builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(switch_words);
	std::vector<uint32_t> phi_words {spv::OpPhi, result_type, state.builder.AllocateId()};
	for (uint32_t group = 0; group < group_ordinal.size(); group++) {
		EmitLabel(state, labels[group]);
		const auto value = emit_arm(group);
		if (result_type != 0) {
			phi_words.push_back(value);
			phi_words.push_back(state.current_label);
		}
		state.builder.AddFunction(spv::OpBranch, merge_label);
	}
	EmitLabel(state, merge_label);
	if (result_type == 0) {
		return 0u;
	}
	state.builder.AddFunction(phi_words);
	return phi_words[2];
}

// A buffer whose source is an indirect descriptor follows upstream's store contract: a key the
// mapping lacks stores nothing, and a null candidate is never written.
bool KeyedStoreBuffer(const EmitterState& state, uint32_t resource) {
	const auto& program = state.program;
	if (resource >= program.info.buffers.size()) {
		return false;
	}
	const auto source = program.info.buffers[resource].source;
	return source < program.descriptor_sources.size() &&
	       program.descriptor_sources[source].indirect_descriptor.has_value();
}

bool NullKeyedStoreBuffer(const EmitterState& state, const IR::MemoryInfo& mem) {
	return mem.kind == IR::ResourceKind::Buffer && KeyedStoreBuffer(state, mem.resource) &&
	       state.program.info.buffers[mem.resource].packed_stride == 0u;
}

template <typename Fn>
void EmitKeyedBufferStore(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          Fn&& store) {
	auto&       state  = ctx.state;
	const auto& buffer = state.program.info.buffers[mem.resource];
	const auto* handle = inst.Arg(0).Resolve().TryInstruction();
	if (handle == nullptr || handle->NumArgs() == 0u) {
		ctx.Fail(inst, "indirect buffer store has no runtime key");
		return;
	}
	uint32_t   matched  = 0;
	const auto found    = EmitUnrolledCandidateSearch(state, buffer.indirect_mapping_offset,
	                                                  buffer.indirect_search_iterations,
	                                                  ctx.Def(handle->Arg(0)), &matched);
	const auto selected = Select(state, TypeU32(state), matched, found,
	                             ConstantU32(state, UINT32_MAX));
	const auto& candidates = buffer.indirect_resources;
	const auto  merge      = state.builder.AllocateId();
	std::vector<uint32_t> labels(candidates.size());
	std::vector<uint32_t> branches {spv::OpSwitch, selected, merge};
	for (uint32_t ordinal = 0; ordinal < labels.size(); ordinal++) {
		labels[ordinal] = state.builder.AllocateId();
		branches.push_back(ordinal);
		branches.push_back(labels[ordinal]);
	}
	state.builder.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
	state.builder.AddFunction(branches);
	for (uint32_t ordinal = 0; ordinal < labels.size(); ordinal++) {
		EmitLabel(state, labels[ordinal]);
		auto per_candidate     = mem;
		per_candidate.resource = candidates[ordinal];
		store(per_candidate);
		state.builder.AddFunction(spv::OpBranch, merge);
	}
	EmitLabel(state, merge);
}

uint32_t EmitAtomic32(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem = ctx.Memory(inst);
	if (mem.dynamic_buffer) {
		return EmitDynamicBufferAtomic(ctx, inst, mem, TypeU32(ctx.state), ConstantU32(ctx.state, 0),
		                               TypePhysicalU32Pointer(ctx.state), 4u, [&](uint32_t pointer) {
			                               const auto old = EmitAtomicOperation(
			                                   ctx, inst, pointer, spv::ScopeDevice,
			                                   AtomicSemantics(mem));
			                               EmitAtomicMemoryBarrier(ctx.state, mem.kind);
			                               return old;
		                               });
	}
	const auto atomic = [&](const IR::MemoryInfo& access) {
		return EmitAtomicAccess(ctx, inst, access, [&](uint32_t pointer) {
			const auto scope =
			    access.kind == IR::ResourceKind::Lds ? spv::ScopeWorkgroup : spv::ScopeDevice;
			const auto old =
			    EmitAtomicOperation(ctx, inst, pointer, scope, AtomicSemantics(access));
			EmitAtomicMemoryBarrier(ctx.state, access.kind);
			return old;
		});
	};
	if (IsIndirectBufferRoot(ctx, mem)) {
		return EmitIndirectBufferSwitch(ctx, inst, mem, TypeU32(ctx.state), true, atomic);
	}
	return atomic(mem);
}

uint32_t EmitBufferAtomic64(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem   = ctx.Memory(inst);
	if (mem.dynamic_buffer) {
		auto& state = ctx.state;
		return EmitDynamicBufferAtomic(
		    ctx, inst, mem, TypeU64(state), ConstantU64(state, 0),
		    TypePointer(state, spv::StorageClassPhysicalStorageBuffer, TypeU64(state)), 8u,
		    [&](uint32_t pointer) {
			    const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
			    const auto old = state.builder.AllocateId();
			    state.builder.AddFunction(SpirvAtomicOpcode(inst.GetOpcode()), TypeU64(state),
			                              old, pointer, ConstantU32(state, spv::ScopeDevice),
			                              ConstantU32(state, spv::MemorySemanticsMaskNone), value);
			    EmitAtomicMemoryBarrier(state, IR::ResourceKind::Buffer);
			    return old;
		    });
	}
	if (IsIndirectBufferRoot(ctx, mem)) {
		return EmitIndirectBufferSwitch(ctx, inst, mem, TypeU64(ctx.state), true,
		                                [&](const IR::MemoryInfo& access) {
			                                return EmitBufferAtomic64For(ctx, inst, access);
		                                });
	}
	return EmitBufferAtomic64For(ctx, inst, mem);
}

uint32_t EmitBufferAtomic64For(ValueEmitContext& ctx, const IR::Inst& inst,
                               const IR::MemoryInfo& mem) {
	auto&       state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU64(state), ConstantU64(state, 0), [&]() {
		    const auto resource =
		        PrepareStorageBufferResourceAccess(state, mem, state.storage_buffers[3]);
		    const auto byte_address = ByteAddress(ctx, inst, mem);
		    const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), byte_address,
		                              ConstantU32(state, 3u));
		    return EmitValueOrDefaultIfCondition(
		        state, EmitMemoryElementInBounds(state, resource, index), TypeU64(state),
		        ConstantU64(state, 0), [&]() {
			        const auto pointer = EmitStorageBufferElementPointer(
			            state, resource, index, TypeStorageBufferElementPointer(state, 64));
			        const auto old = EmitAtomicOperation(ctx, inst, pointer, spv::ScopeDevice);
			        EmitAtomicMemoryBarrier(state, mem.kind);
			        return old;
		        });
	    });
}

void EmitSharedAtomic64(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	const auto& mem = ctx.Memory(inst);
	EnsureLdsStorage(state);
	EmitIfCondition(state, ctx.Arg(inst, 2), [&]() {
		const auto address = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		                            ByteAddress(ctx, inst, mem), ConstantU32(state, 0xfff8u));
		const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
		                          ConstantU32(state, 3u));
		const auto in_bounds = Binary(state, spv::OpULessThan, TypeBool(state), index,
		                              ConstantU32(state, LdsDwordCount(state) / 2u));
		EmitIfCondition(state, in_bounds, [&]() {
			auto native_index = index;
			auto semantics = spv::MemorySemanticsWorkgroupMemoryMask;
			if (state.lds_storage_class == spv::StorageClassStorageBuffer) {
				native_index = EmitAddU32(state, index, EmitBinaryU32(
				    state, spv::OpShiftRightLogical, state.lds_base_dwords, ConstantU32(state, 1)));
				semantics = spv::MemorySemanticsUniformMemoryMask;
			}
			const auto pointer = state.builder.AllocateId();
			state.builder.AddFunction(
			    spv::OpAccessChain, TypePointer(state, state.lds_storage_class, TypeU64(state)),
			    pointer, state.lds_u64_variable, ConstantU32(state, 0), native_index);
			const auto value = ctx.Arg(inst, 1);
			state.builder.AddFunction(
			    SpirvAtomicOpcode(inst.GetOpcode()), TypeU64(state), state.builder.AllocateId(),
			    pointer, ConstantU32(state, spv::ScopeWorkgroup),
			    ConstantU32(state, spv::MemorySemanticsAcquireReleaseMask | semantics), value);
		});
	});
}

uint32_t EmitBufferFloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem       = ctx.Memory(inst);
	const bool  max_value = inst.GetOpcode() == IR::ValueOpcode::BufferAtomicFMax32;
	if (mem.dynamic_buffer) {
		const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
		return EmitDynamicBufferAtomic(ctx, inst, mem, TypeU32(ctx.state), ConstantU32(ctx.state, 0),
		                               TypePhysicalU32Pointer(ctx.state), 4u, [&](uint32_t pointer) {
			                               return AtomicUpdate(ctx.state, pointer, mem.kind,
			                                                   [&](uint32_t old) {
				                                                   return EmitFloatAtomicReplacement(
				                                                       ctx.state, old, value,
				                                                       max_value);
			                                                   });
		                               });
	}
	return EmitAtomicUpdate(ctx, inst, mem,
	                        [max_value](EmitterState& state, uint32_t old, uint32_t value) {
		                        return EmitFloatAtomicReplacement(state, old, value, max_value);
	                        });
}

void EmitSharedFloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem       = ctx.Memory(inst);
	const bool  max_value = inst.GetOpcode() == IR::ValueOpcode::SharedAtomicFMax32;
	EmitAtomicUpdate(ctx, inst, mem,
	                 [max_value](EmitterState& state, uint32_t old, uint32_t value) {
		                 return EmitDsFloatAtomicReplacement(state, old, value, max_value);
	                 });
}

uint32_t EmitAppendConsume(ValueEmitContext& ctx, const IR::Inst& inst) {
	const bool append = inst.GetOpcode() == IR::ValueOpcode::DataAppend;
	auto&      state  = ctx.state;
	if (ctx.half == 1) {
		return ctx.other_half->Def(IR::Value(const_cast<IR::Inst*>(&inst)));
	}
	const auto mem           = ctx.Memory(inst);
	const auto offset        = mem.offset & 0xfffcu;
	auto       address       = ConstantU32(state, offset);
	uint32_t   region_bounds = 0;
	if (mem.kind == IR::ResourceKind::Gds) {
		const auto m0 = ctx.Arg(inst, 0);
		const auto base =
		    Binary(state, spv::OpShiftRightLogical, TypeU32(state), m0, ConstantU32(state, 16));
		const auto size =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), m0, ConstantU32(state, 0xffffu));
		address = Binary(state, spv::OpIAdd, TypeU32(state), base, address);
		// The entire M0 region must fit the PS5's 48 KiB GDS partition.
		region_bounds = AndCondition(
		    state, Binary(state, spv::OpULessThan, TypeBool(state),
		                  ConstantU32(state, offset + 3u), size),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state),
		           Binary(state, spv::OpIAdd, TypeU32(state), base, size),
		           ConstantU32(state, 0xc000u)));
	}
	const auto index =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
	const auto access = PrepareMemoryResourceAccess(state, mem);
	const auto exec   = ctx.Arg(inst, 1);
	const auto ballot = ctx.Ballot(inst.Arg(1));
	const auto low    = state.builder.AllocateId();
	const auto high   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, ballot, 0);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, ballot, 1);
	const auto count = Binary(state, spv::OpIAdd, TypeU32(state),
	                          Unary(state, spv::OpBitCount, TypeU32(state), low),
	                          Unary(state, spv::OpBitCount, TypeU32(state), high));
	const auto first = ctx.FirstLane(ballot);
	const auto source_lane =
	    state.lane_count == 2
	        ? Binary(state, spv::OpBitwiseAnd, TypeU32(state), first, ConstantU32(state, 31))
	        : first;
	const auto is_first       = Binary(state, spv::OpIEqual, TypeBool(state),
	                                   EmitSubgroupLocalInvocationId(state), source_lane);
	auto bounds = EmitMemoryElementInBounds(state, access, index);
	if (mem.kind == IR::ResourceKind::Gds) {
		bounds = AndCondition(state, bounds, region_bounds);
	}
	const auto condition = AndCondition(
	    state, is_first,
	    AndCondition(state,
	                 state.lane_count == 2 ? Binary(state, spv::OpINotEqual, TypeBool(state), count,
	                                                ConstantU32(state, 0))
	                                       : exec,
	                 bounds));
	const auto atomic = EmitValueOrZeroIfCondition(state, condition, [&]() {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(append ? spv::OpAtomicIAdd : spv::OpAtomicISub, TypeU32(state),
		                          value, EmitMemoryElementPointer(state, access, index),
		                          ConstantU32(state, mem.kind == IR::ResourceKind::Gds
		                                                 ? spv::ScopeDevice
		                                                 : spv::ScopeWorkgroup),
		                          ConstantU32(state, spv::MemorySemanticsMaskNone), count);
		return value;
	});
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeU32(state), result,
	                          ConstantU32(state, spv::ScopeSubgroup), atomic, source_lane);
	return result;
}

uint32_t EmitReadConst(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	if (state.flattened_srt_variable == 0) {
		ctx.Fail(inst, "requires the flattened SRT descriptor");
	}
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                          state.flattened_srt_variable, ConstantU32(state, 0),
	                          ctx.Arg(inst, 1));
	return EmitNative<spv::OpLoad, IR::Type::U32>(state, pointer);
}

// KYTY_NO_SCALAR_BOUNDS_CAP=1 keeps the explicit test and select on every scalar buffer read.
bool ClampScalarBufferBounds() {
	static const bool enabled = [] {
		const char* text = std::getenv("KYTY_NO_SCALAR_BOUNDS_CAP");
		return text == nullptr || text[0] == '\0' || text[0] == '0';
	}();
	return enabled;
}

void EmitReadConstBuffer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto mem = ctx.Memory(inst);
	if (mem.planning_only) return;
	if (mem.dynamic_buffer) {
		ctx.Define(inst, ReadDynamicConstBuffer(ctx, inst, mem));
		return;
	}
	if (mem.kind == IR::ResourceKind::IndirectBuffer) {
		ctx.Define(inst, LoadIndirectScalarBuffer(ctx, inst));
		return;
	}
	auto& state        = ctx.state;
	mem.kind           = IR::ResourceKind::ScalarBuffer;
	auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), ctx.Arg(inst, 1),
	                    ConstantU32(state, 2));
	if (mem.offset >= 4u) {
		index = Binary(state, spv::OpIAdd, TypeU32(state), index, ConstantU32(state, mem.offset >> 2u));
	}
	const auto read = [&](const IR::MemoryInfo& info) {
		const auto access = PrepareMemoryResourceAccess(state, info);
		// Scalar buffer addressing aligns the base and each offset independently.
		const auto element   = Binary(state, spv::OpIAdd, TypeU32(state), index,
		                              Binary(state, spv::OpShiftRightLogical, TypeU32(state),
		                                     access.byte_offset, ConstantU32(state, 2)));
		if (DeviceBoundsChecked(state, access) && ClampScalarBufferBounds()) {
			const auto capped = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpExtInst, TypeU32(state), capped, GlslStd450(state),
			                          GLSLstd450UMin, element, ConstantU32(state, 0x3fffffffu));
			return LoadResourceWord(state, access, state.builder.AllocateId(),
			                        EmitMemoryElementPointer(state, access, capped));
		}
		// Kept under robustness2: a scalar offset that wraps 32 bits lands back in range.
		const auto condition = EmitMemoryElementInBounds(state, access, element);
		return EmitValueOrZeroIfCondition(state, condition, [&]() {
			return LoadResourceWord(state, access, state.builder.AllocateId(),
			                        EmitMemoryElementPointer(state, access, element));
		});
	};
	// An expanded table leaves the root naming nothing, so a scalar read of the record payload has
	// to pick its candidate the same way a load or a store does.
	if (IsIndirectBufferRoot(ctx, mem)) {
		ctx.Define(inst, EmitIndirectBufferSwitch(ctx, inst, mem, TypeU32(state), false, read));
		return;
	}
	ctx.Define(inst, read(mem));
}

void EmitLoadMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto  op  = inst.GetOpcode();
	const auto& mem = ctx.Memory(inst);
	if ((op == IR::ValueOpcode::LoadAddressU32 || op == IR::ValueOpcode::LoadBufferU32) &&
	    mem.planning_only)
		return;
	const auto buffer_components = IR::BufferComponentCount(op);
	const auto shared_components = IR::SharedComponentCount(op);
	const auto address_info      = IR::AddressOpcodeInfoOf(op);
	const auto load              = [&](const IR::MemoryInfo& access) -> uint32_t {
        if (access.kind == IR::ResourceKind::FlatLocal)
            return LoadLocalFlat(ctx, inst);
		if (buffer_components > 1u ||
		    (buffer_components == 1u && access.kind == IR::ResourceKind::IndirectBuffer &&
		     !access.formatted))
			return LoadWideBuffer(ctx, inst, access, buffer_components);
		if (shared_components > 1u) return LoadWideShared(ctx, inst, shared_components);
		if (access.kind == IR::ResourceKind::ScalarAddress)
			return LoadBdaDword(ctx, GuestAddress(ctx, inst, access));
		if (address_info.access == IR::AddressAccess::Read &&
		    access.kind != IR::ResourceKind::Scratch)
			return LoadBda(ctx, GuestAddress(ctx, inst, access), ctx.Arg(inst, inst.NumArgs() - 1),
			               address_info.data_bits, CoherentBufferAccess(access) || access.coherent);
		// A formatted access needs a host-decoded descriptor; a dynamic V# has none.
		if (op == IR::ValueOpcode::LoadBufferU32 && access.formatted && !access.dynamic_buffer)
			return access.kind == IR::ResourceKind::IndirectBuffer
			           ? LoadIndirectFormattedX(ctx, inst)
			           : FormattedLoad(ctx, inst, access);
		if (inst.GetType() == IR::Type::U8) return LoadSubword(ctx, inst, access, 8, false);
		if (inst.GetType() == IR::Type::U16) return LoadSubword(ctx, inst, access, 16, false);
		return LoadWord(ctx, inst, access);
	};
	if (IsIndirectBufferRoot(ctx, mem)) {
		const auto result_type = buffer_components > 1u
		                             ? TypeU32Composite(ctx.state, buffer_components)
		                             : TypeU32(ctx.state);
		ctx.Define(inst, EmitIndirectBufferSwitch(ctx, inst, mem, result_type, true, load));
		return;
	}
	ctx.Define(inst, load(mem));
}

// The 64-bit LDS atomics of the RDNA 2 ISA (DS_MAX_RTN_U64, DS_CMPST_RTN_B64): one indivisible
// eight-byte read-modify-write that returns the pre-op value. Wave size never enters them - the
// operand is an LDS byte address, not a lane index - so wave32 and wave64 emit the same thing.
uint32_t EmitSharedAtomic64Returning(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&       state = ctx.state;
	const auto& mem   = ctx.Memory(inst);
	EXIT_IF(mem.kind != IR::ResourceKind::Lds);
	const bool compare = inst.GetOpcode() == IR::ValueOpcode::SharedAtomicCmpSwap64;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU64(state), ConstantU64(state, 0), [&]() {
		    const auto resource = PrepareMemoryResourceAccess(state, mem);
		    const auto index    = Binary(state, spv::OpShiftRightLogical, TypeU32(state),
		                                 DwordIndex(ctx, inst, mem), ConstantU32(state, 1));
		    const auto qwords   = Binary(state, spv::OpShiftRightLogical, TypeU32(state),
		                                 resource.length, ConstantU32(state, 1));
		    const auto in_bounds =
		        Binary(state, spv::OpULessThan, TypeBool(state), index, qwords);
		    return EmitValueOrDefaultIfCondition(
		        state, in_bounds, TypeU64(state), ConstantU64(state, 0), [&]() {
			        const auto scalar  = TypeU64(state);
			        const auto pointer = EmitLdsQwordPointer(state, index);
			        const auto scope   = ConstantU32(state, spv::ScopeWorkgroup);
			        const auto old     = state.builder.AllocateId();
			        if (compare) {
				        const auto desired    = ctx.Arg(inst, inst.NumArgs() - 3);
				        const auto comparator = ctx.Arg(inst, inst.NumArgs() - 2);
				        // An unequal compare-exchange performs no store, so it carries no release.
				        state.builder.AddFunction(
				            spv::OpAtomicCompareExchange, scalar, old, pointer, scope,
				            ConstantU32(state, AtomicSemantics(mem)),
				            ConstantU32(state, spv::MemorySemanticsMaskNone), desired, comparator);
			        } else {
				        const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
				        state.builder.AddFunction(spv::OpAtomicUMax, scalar, old, pointer, scope,
				                                  ConstantU32(state, AtomicSemantics(mem)), value);
			        }
			        state.builder.AddFunction(
			            spv::OpMemoryBarrier, scope,
			            ConstantU32(state, spv::MemorySemanticsAcquireReleaseMask |
			                                   spv::MemorySemanticsWorkgroupMemoryMask));
			        return old;
		        });
	    });
}

void EmitStoreMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto  op                = inst.GetOpcode();
	const auto& mem               = ctx.Memory(inst);
	const auto  buffer_components = IR::BufferComponentCount(op);
	const auto  shared_components = IR::SharedComponentCount(op);
	const auto  type              = inst.Arg(inst.NumArgs() - 2).GetType();
	const auto  address_info      = IR::AddressOpcodeInfoOf(op);
	const auto store = [&](const IR::MemoryInfo& access) -> uint32_t {
		if (NullKeyedStoreBuffer(ctx.state, access))
			return 0u;
		if (access.kind == IR::ResourceKind::FlatLocal)
			StoreLocalFlat(ctx, inst);
		else if (buffer_components > 1u)
			StoreWideBuffer(ctx, inst, access, buffer_components);
		else if (shared_components > 1u)
			StoreWideShared(ctx, inst, shared_components);
		else if (address_info.access == IR::AddressAccess::Write &&
		         access.kind != IR::ResourceKind::Scratch)
			StoreBda(ctx, inst, access, address_info.data_bits);
		else if (op == IR::ValueOpcode::StoreBufferU32 && access.formatted &&
		         !access.dynamic_buffer)
			FormattedStore(ctx, inst, access);
		else if (type == IR::Type::U8)
			StoreSubword(ctx, inst, access, 8);
		else if (type == IR::Type::U16)
			StoreSubword(ctx, inst, access, 16);
		else
			StoreWord(ctx, inst, access);
		return 0u;
	};
	if (IsIndirectBufferRoot(ctx, mem)) {
		if (KeyedStoreBuffer(ctx.state, mem.resource)) {
			EmitKeyedBufferStore(ctx, inst, mem, store);
			return;
		}
		// No result to merge: the switch is emitted for its side effects and the phi is skipped.
		EmitIndirectBufferSwitch(ctx, inst, mem, 0u, true, store);
		return;
	}
	store(mem);
}

uint32_t EmitSharedIncDec(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto replacement =
	    inst.GetOpcode() == IR::ValueOpcode::SharedAtomicInc32 ? AtomicIncrement : AtomicDecrement;
	return EmitAtomicUpdate(ctx, inst, ctx.Memory(inst), replacement);
}

void EmitSharedAtomicMaskedOr32(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto keep = Unary(ctx.state, spv::OpNot, TypeU32(ctx.state), ctx.Arg(inst, 1));
	EmitAtomicUpdate(ctx, inst, ctx.Memory(inst),
	                 [keep](EmitterState& state, uint32_t old, uint32_t value) {
		                 return Binary(state, spv::OpBitwiseOr, TypeU32(state),
		                               Binary(state, spv::OpBitwiseAnd, TypeU32(state), old, keep), value);
	                 });
}

uint32_t EmitSwizzleU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	state.builder.AddFunction(spv::OpStore, ctx.scratch_u32_variable, ctx.Arg(inst, 0));
	const auto source = EmitNative<spv::OpLoad, IR::Type::U32>(state, ctx.scratch_u32_variable);
	const auto target = EmitDsSwizzleTargetLane(state, EmitSubgroupLocalInvocationId(state),
	                                            inst.Arg(1).IsImmediate() ? inst.Arg(1).U32() : 0);
	return EmitDsMaskedLaneRead(state, source, target, ctx.Arg(inst, 2));
}

uint32_t EmitBpermuteU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state  = ctx.state;
	const auto source = ctx.Arg(inst, 0);
	const auto index  = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
	                           Binary(state, spv::OpShiftRightLogical, TypeU32(state),
	                                  ctx.Arg(inst, 1), ConstantU32(state, 2)),
	                           ConstantU32(state, 31));
	const auto base   = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
	                           EmitSubgroupLocalInvocationId(state), ConstantU32(state, ~31u));
	const auto target = Binary(state, spv::OpBitwiseOr, TypeU32(state), base, index);
	return EmitDsMaskedLaneRead(state, source, target, ctx.Arg(inst, 2));
}

uint32_t EmitPermuteU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state   = ctx.state;
	auto       lane    = EmitSubgroupLocalInvocationId(state);
	if (state.lane_count == 2) {
		lane = Binary(state, spv::OpBitwiseAnd, TypeU32(state), lane, ConstantU32(state, 31));
	}
	const auto address = ctx.Arg(inst, 1);
	const auto word    = Binary(state, spv::OpShiftRightLogical, TypeU32(state), lane,
	                            ConstantU32(state, 5));
	const auto ballot_word = [&](uint32_t predicate) {
		const auto ballot = state.builder.AllocateId();
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformBallot, TypeU32Vector(state, 4), ballot,
		                          ConstantU32(state, spv::ScopeSubgroup), predicate);
		state.builder.AddFunction(spv::OpVectorExtractDynamic, TypeU32(state), result, ballot, word);
		return result;
	};
	// RDNA2 permutes independently within each 32-lane half. Intersect the source
	// address bit ballots to find this destination's enabled writers without LDS.
	auto writers = ballot_word(ctx.Arg(inst, 2));
	for (uint32_t bit = 0; bit < 5; ++bit) {
		const auto address_bit = Binary(state, spv::OpBitwiseAnd, TypeU32(state), address,
		                                ConstantU32(state, 1u << (bit + 2)));
		const auto mask = ballot_word(Binary(state, spv::OpINotEqual, TypeBool(state),
		                                      address_bit, ConstantU32(state, 0)));
		const auto lane_bit = Binary(state, spv::OpBitwiseAnd, TypeU32(state), lane,
		                             ConstantU32(state, 1u << bit));
		const auto selected = Select(
		    state, TypeU32(state),
		    Binary(state, spv::OpINotEqual, TypeBool(state), lane_bit, ConstantU32(state, 0)),
		    mask, Unary(state, spv::OpNot, TypeU32(state), mask));
		writers = Binary(state, spv::OpBitwiseAnd, TypeU32(state), writers, selected);
	}
	const auto active = Binary(state, spv::OpINotEqual, TypeBool(state), writers,
	                           ConstantU32(state, 0));
	const auto base = Binary(state, spv::OpBitwiseAnd, TypeU32(state), lane,
	                         ConstantU32(state, ~31u));
	const auto source = Select(state, TypeU32(state), active,
	                           Binary(state, spv::OpBitwiseOr, TypeU32(state), base,
	                                  EmitFindUMsb32(state, writers)), lane);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeU32(state), result,
	                          ConstantU32(state, spv::ScopeSubgroup), ctx.Arg(inst, 0), source);
	return Select(state, TypeU32(state), active, result, ConstantU32(state, 0));
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
