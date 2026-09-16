#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include <algorithm>

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

// ds_permute_b32 scatters, and SPIR-V only gathers: ballot each bit of the target lane index
// and intersect, leaving the one source lane that selected this one.
uint32_t EmitDsForwardPermute(EmitterState& state, uint32_t value, uint32_t address,
                              uint32_t exec) {
	const auto u32       = TypeU32(state);
	const auto boolean   = TypeBool(state);
	const auto ballot_ty = TypeU32Vector(state, 4);
	const auto scope     = ConstantU32(state, spv::ScopeSubgroup);
	const auto id        = EmitSubgroupLocalInvocationId(state);
	const auto row       = Binary(state, spv::OpBitwiseAnd, u32, id, ConstantU32(state, ~31u));
	const auto lane      = Binary(
	    state, spv::OpBitwiseAnd, u32,
	    Binary(state, spv::OpShiftRightLogical, u32, address, ConstantU32(state, 2)),
	    ConstantU32(state, 31));
	const auto target = Binary(state, spv::OpBitwiseOr, u32, row, lane);
	const auto ballot = [&](uint32_t predicate) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformBallot, ballot_ty, result, scope, predicate);
		return result;
	};
	const auto bit_of = [&](uint32_t word, uint32_t index) {
		const auto bit =
		    Binary(state, spv::OpBitwiseAnd, u32,
		           Binary(state, spv::OpShiftRightLogical, u32, word, ConstantU32(state, index)),
		           ConstantU32(state, 1));
		return Binary(state, spv::OpINotEqual, boolean, bit, ConstantU32(state, 0));
	};
	auto matches = ballot(exec);
	// Bit 5 keeps a native 64-lane subgroup from matching sources aimed at the other half.
	const auto bits = state.lane_count == 2 ? 5u : 6u;
	for (uint32_t index = 0; index < bits; index++) {
		const auto sources = ballot(bit_of(target, index));
		const auto wanted  = Select(state, u32, bit_of(id, index), ConstantU32(state, 0xffffffffu),
		                            ConstantU32(state, 0));
		const auto spread  = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeConstruct, ballot_ty, spread, wanted, wanted,
		                          wanted, wanted);
		const auto same = Unary(state, spv::OpNot, ballot_ty,
		                        Binary(state, spv::OpBitwiseXor, ballot_ty, sources, spread));
		matches         = Binary(state, spv::OpBitwiseAnd, ballot_ty, matches, same);
	}
	const auto low = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, u32, low, matches, 0);
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, u32, high, matches, 1);
	const auto found = Binary(state, spv::OpINotEqual, boolean,
	                          Binary(state, spv::OpBitwiseOr, u32, low, high), ConstantU32(state, 0));
	// The ISA gives a destination selected by several sources to the highest-numbered one.
	const auto msb = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformBallotFindMSB, u32, msb, scope, matches);
	const auto source_lane = Select(state, u32, found, msb, ConstantU32(state, 0));
	const auto shuffled    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, u32, shuffled, scope, value,
	                          source_lane);
	return Select(state, u32, found, shuffled, ConstantU32(state, 0));
}

uint32_t BufferByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                           uint32_t index, uint32_t offset, uint32_t soffset) {
	auto&          state   = ctx.state;
	const uint32_t packed  = StorageBufferPackedStride(state, mem);
	const uint32_t stride  = packed & 0x3fffu;
	const bool     swizzle = stride != 0u && ((packed >> 14u) & 1u) != 0u;
	if (((packed >> 20u) & 1u) != 0u) {
		const auto lane = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		                         EmitSubgroupLocalInvocationId(state), ConstantU32(state, 63));
		index           = Binary(state, spv::OpIAdd, TypeU32(state), index, lane);
	}
	if (mem.offset != 0u) {
		offset = Binary(state, spv::OpIAdd, TypeU32(state), offset, ConstantU32(state, mem.offset));
	}

	uint32_t address = 0;
	if (!swizzle) {
		if (stride == 0u) {
			address = offset;
		} else {
			const auto indexed = stride == 1u ? index
			                                  : Binary(state, spv::OpIMul, TypeU32(state), index,
			                                           ConstantU32(state, stride));
			address            = Binary(state, spv::OpIAdd, TypeU32(state), indexed, offset);
		}
	} else {
		const uint32_t stride_enum  = (packed >> 16u) & 3u;
		const uint32_t index_stride = 8u << stride_enum;
		const auto     index_msb    = Binary(state, spv::OpShiftRightLogical, TypeU32(state), index,
		                                     ConstantU32(state, stride_enum + 3u));
		const auto     index_lsb    = Binary(state, spv::OpBitwiseAnd, TypeU32(state), index,
		                                     ConstantU32(state, index_stride - 1u));
		const auto     offset_msb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, ~3u));
		const auto offset_lsb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, 3u));
		const auto indexed_msb = stride == 1u ? index_msb
		                                      : Binary(state, spv::OpIMul, TypeU32(state),
		                                               index_msb, ConstantU32(state, stride));
		const auto msb = Binary(state, spv::OpIMul, TypeU32(state),
		                        Binary(state, spv::OpIAdd, TypeU32(state), indexed_msb, offset_msb),
		                        ConstantU32(state, index_stride));
		const auto lsb = Binary(state, spv::OpIAdd, TypeU32(state),
		                        Binary(state, spv::OpShiftLeftLogical, TypeU32(state), index_lsb,
		                               ConstantU32(state, 2u)),
		                        offset_lsb);
		address        = Binary(state, spv::OpIAdd, TypeU32(state), msb, lsb);
	}

	const auto soffset_value = inst.Arg(3).Resolve();
	if (soffset_value.IsImmediate() && soffset_value.GetType() == IR::Type::U32 &&
	    soffset_value.U32() == 0u) {
		return address;
	}
	return Binary(state, spv::OpIAdd, TypeU32(state), address, soffset);
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

uint32_t ScratchByteAddress(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t low,
                            uint32_t high) {
	auto& state     = ctx.state;
	auto  immediate = static_cast<int32_t>(mem.offset);
	const auto immediate_low  = ConstantU32(state, static_cast<uint32_t>(immediate));
	const auto immediate_high = ConstantU32(state, immediate < 0 ? UINT32_MAX : 0u);
	low                      = AddU64Low(state, low, high, immediate_low, immediate_high, high);
	const auto valid = Binary(state, spv::OpIEqual, TypeBool(state), high, ConstantU32(state, 0));
	return Select(state, TypeU32(state), valid, low, ConstantU32(state, UINT32_MAX));
}

uint32_t ConstantDeviceAddress(EmitterState& state, uint64_t value) {
	return state.builder.Constant(spv::OpConstant, TypeScalarU64(state),
	                              static_cast<uint32_t>(value),
	                              static_cast<uint32_t>(value >> 32u));
}

uint32_t DeviceAddressFromWords(EmitterState& state, uint32_t low, uint32_t high) {
	const auto low64  = Unary(state, spv::OpUConvert, TypeScalarU64(state), low);
	const auto high64 = Binary(state, spv::OpShiftLeftLogical, TypeScalarU64(state),
	                           Unary(state, spv::OpUConvert, TypeScalarU64(state), high),
	                           ConstantDeviceAddress(state, 32));
	return Binary(state, spv::OpBitwiseOr, TypeScalarU64(state), low64, high64);
}

uint32_t GuestAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto& state = ctx.state;
	auto  low   = ctx.Arg(inst, 1);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		low = Binary(state, spv::OpBitwiseAnd, TypeU32(state), low, ConstantU32(state, ~3u));
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
		address         = Binary(state, spv::OpIAdd, TypeScalarU64(state), base,
		                         Unary(state, spv::OpUConvert, TypeScalarU64(state), low));
	}
	auto immediate = static_cast<int32_t>(mem.offset);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
	}
	return immediate == 0
	           ? address
	           : Binary(state, spv::OpIAdd, TypeScalarU64(state), address,
	                    ConstantDeviceAddress(
	                        state, static_cast<uint64_t>(static_cast<int64_t>(immediate))));
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
	const auto pointer = FaultElementPointer(state, word);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	state.builder.AddFunction(spv::OpStore, pointer,
	                          Binary(state, spv::OpBitwiseOr, TypeU32(state), value, bit));
}

uint32_t GetBdaPointer(ValueEmitContext& ctx, uint32_t address) {
	auto&      state  = ctx.state;
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeScalarU64(state), result,
	                          state.bda_pointer_function, address);
	return result;
}

uint32_t GetBdaStorePointer(ValueEmitContext& ctx, uint32_t address) {
	auto&      state  = ctx.state;
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeScalarU64(state), result,
	                          state.bda_store_pointer_function, address);
	return result;
}

// glc rides along as Volatile beside the alignment the raw pointer path already declares.
uint32_t BdaAccessMask(bool coherent) {
	return coherent ? (spv::MemoryAccessAlignedMask | spv::MemoryAccessVolatileMask)
	                : spv::MemoryAccessAlignedMask;
}

uint32_t LoadBdaDword(ValueEmitContext& ctx, uint32_t address, bool coherent) {
	auto&      state   = ctx.state;
	const auto bda     = GetBdaPointer(ctx, address);
	const auto present =
	    Binary(state, spv::OpINotEqual, TypeBool(state), bda, ConstantDeviceAddress(state, 0));
	return EmitValueOrZeroIfCondition(state, present, [&]() {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer,
		                          bda);
		const auto         value     = state.builder.AllocateId();
		constexpr uint32_t alignment = sizeof(uint32_t);
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer,
		                          BdaAccessMask(coherent), alignment);
		return value;
	});
}

uint32_t LoadBda(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
	             uint32_t bits) {
	auto&      state    = ctx.state;
	const auto address  = GuestAddress(ctx, inst, mem);
	const auto active   = ctx.Arg(inst, inst.NumArgs() - 1);
	const auto coherent = CoherentBufferAccess(mem);
	return EmitValueOrZeroIfCondition(state, active, [&]() {
		const auto aligned = Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state), address,
		                            ConstantDeviceAddress(state, ~uint64_t {3}));
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
			                    Binary(state, spv::OpIAdd, TypeScalarU64(state), aligned,
			                           ConstantDeviceAddress(state, sizeof(uint32_t))),
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
	});
}

// An unmapped page faults and drops the write; an untracked mapped page faults but keeps it.
void StoreBdaDword(ValueEmitContext& ctx, uint32_t address, uint32_t data, bool coherent) {
	auto&      state   = ctx.state;
	const auto bda     = GetBdaStorePointer(ctx, address);
	const auto present = Binary(state, spv::OpINotEqual, TypeBool(state), bda,
	                            ConstantDeviceAddress(state, 0));
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
	                            ConstantDeviceAddress(state, 0));
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

void StoreBda(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
              uint32_t bits) {
	auto&      state    = ctx.state;
	const auto address  = GuestAddress(ctx, inst, mem);
	const auto data     = ctx.Arg(inst, inst.NumArgs() - 2);
	const auto active   = ctx.Arg(inst, inst.NumArgs() - 1);
	const auto coherent = CoherentBufferAccess(mem);
	const auto u32      = TypeU32(state);
	EmitIfCondition(state, active, [&]() {
		const auto aligned = Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state), address,
		                            ConstantDeviceAddress(state, ~uint64_t {3}));
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
			return Binary(state, spv::OpIAdd, TypeScalarU64(state), aligned,
			              ConstantDeviceAddress(state, sizeof(uint32_t)));
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
		return Binary(ctx.state, spv::OpIAdd, TypeU32(ctx.state), ctx.Arg(inst, 0),
		              ConstantU32(ctx.state, mem.offset));
	}
	if (mem.kind != IR::ResourceKind::Scratch) {
		EXIT("physical address memory must use the BDA emitter\n");
	}
	return ScratchByteAddress(ctx, mem, ctx.Arg(inst, 1), ctx.Arg(inst, 2));
}

uint32_t DwordIndex(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	return Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state),
	              ByteAddress(ctx, inst, mem), ConstantU32(ctx.state, 2));
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

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index);

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend);

uint32_t LoadWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource) {
	const auto index = EmitMemoryElementIndex(ctx.state, resource, DwordIndex(ctx, inst, mem));
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index),
	    [&]() { return LoadWordInBounds(ctx, resource, index); });
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
	const auto raw_index = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	                              ConstantU32(ctx.state, 2));
	const auto index     = EmitMemoryElementIndex(ctx.state, resource, raw_index);
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		    return LoadSubwordInBounds(ctx, resource, address, index, bits, sign_extend);
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
                          uint32_t index) {
	const auto value   = ctx.state.builder.AllocateId();
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	return LoadResourceWord(ctx.state, resource, value, pointer);
}

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend) {
	const auto word = LoadWordInBounds(ctx, resource, index);
	const auto byte  = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
	                          ConstantU32(ctx.state, 3));
	const auto shift = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), byte,
	                          ConstantU32(ctx.state, 3));
	const auto value =
	    Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state),
	           Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), word, shift),
	           ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu));
	if (!sign_extend) return value;
	const auto left = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), value,
	                         ConstantU32(ctx.state, 32u - bits));
	return Binary(ctx.state, spv::OpShiftRightArithmetic, TypeU32(ctx.state), left,
	              ConstantU32(ctx.state, 32u - bits));
}

uint32_t LoadSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits,
                     bool sign_extend) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadSubwordPrepared(ctx, inst, mem, resource, bits, sign_extend);
	});
}

Prospero::BufferFormat BufferFormat(const ValueEmitContext& ctx, const IR::MemoryInfo& mem) {
	return mem.typed ? Format::DecodeTBufferFormat(mem.data_format, mem.number_format)
	                 : StorageBufferFormat(ctx.state, mem);
}

IR::MemoryInfo RebaseFormattedComponent(IR::MemoryInfo mem, const Format::BufferFormatInfo& info,
                                        uint32_t component) {
	mem.offset += Format::GetFormatComponentByteOffset(info, component);
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
	const auto source = Format::ResolveFormattedSource(info, selector);
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
uint32_t LoadFormattedComponent(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                const Format::BufferFormatInfo& info,
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
		    type == TypeI32(ctx.state) ? Unary(ctx.state, spv::OpBitcast, type, raw) : raw;
		const auto extracted = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(IsSignedFormatComponent(info.type) ? spv::OpBitFieldSExtract
		                                                                 : spv::OpBitFieldUExtract,
		                              type, extracted, source_value,
		                              ConstantU32(ctx.state, info.component_bit_offset[component]),
		                              ConstantU32(ctx.state, bits));
		raw = type == TypeI32(ctx.state)
		          ? Unary(ctx.state, spv::OpBitcast, TypeU32(ctx.state), extracted)
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
	const auto info = Format::GetFormatInfo(BufferFormat(ctx, mem));
	if (info.type == Format::ComponentType::Unknown) {
		return LoadWordPrepared(ctx, inst, RebaseRawComponent(mem, output_component), resource);
	}
	return LoadFormattedComponent(
	    ctx, mem, info, output_component,
	    [&](uint32_t component) {
		    return LoadWordPrepared(ctx, inst, RebaseFormattedComponent(mem, info, component),
		                            resource);
	    },
	    [&](uint32_t component, uint32_t bits, bool sign_extend) {
		    return LoadSubwordPrepared(ctx, inst, RebaseFormattedComponent(mem, info, component),
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
	const auto address   = ByteAddress(ctx, inst, mem);
	const auto raw_index = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	                              ConstantU32(ctx.state, 2));
	const auto index     = EmitMemoryElementIndex(ctx.state, resource, raw_index);
	EmitIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		    StoreSubwordInBounds(ctx, mem, resource, address, index, bits, data);
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
	const auto index = EmitMemoryElementIndex(ctx.state, resource, DwordIndex(ctx, inst, mem));
	EmitIfCondition(ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		StoreResourceWord(ctx.state, resource, EmitMemoryElementPointer(ctx.state, resource, index),
		                  data);
	});
}

void StoreWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource, uint32_t index,
                       uint32_t data) {
	StoreResourceWord(ctx.state, resource, EmitMemoryElementPointer(ctx.state, resource, index),
	                  data);
}

void StoreWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		StoreWordPrepared(ctx, inst, mem, resource, ctx.Arg(inst, inst.NumArgs() - 2));
	});
}

// Packed components share one dword, so a store replaces only its own bitfield.
void StorePackedComponentInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                  const MemoryResourceAccess& resource, uint32_t index,
                                  const Format::BufferFormatInfo& info, uint32_t component,
                                  uint32_t encoded) {
	const auto bits      = info.component_bits[component];
	const auto offset    = info.component_bit_offset[component];
	const auto low_mask  = bits >= 32u ? 0xffffffffu : (1u << bits) - 1u;
	const auto keep_mask = ~(low_mask << offset);
	const auto pointer   = EmitMemoryElementPointer(ctx.state, resource, index);
	const auto value     = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                              Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), encoded,
	                                     ConstantU32(ctx.state, low_mask)),
	                              ConstantU32(ctx.state, offset));
	const auto merge     = [&](uint32_t old) {
		return Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state),
		              Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), old,
		                     ConstantU32(ctx.state, keep_mask)),
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

void FormattedStorePrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                            uint32_t component, const MemoryResourceAccess& resource,
                            uint32_t raw_data) {
	const auto info = Format::GetFormatInfo(BufferFormat(ctx, mem));
	if (info.type == Format::ComponentType::Unknown) {
		StoreWordPrepared(ctx, inst, RebaseRawComponent(mem, component), resource, raw_data);
		return;
	}
	if (component >= info.component_count) return;
	const auto bits          = info.component_bits[component];
	const auto component_mem = RebaseFormattedComponent(mem, info, component);
	const auto data          = EncodeFormatComponent(ctx.state, info, component, raw_data);
	if (info.packed_bitfield) {
		const auto index =
		    EmitMemoryElementIndex(ctx.state, resource, DwordIndex(ctx, inst, component_mem));
		EmitIfCondition(ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
			StorePackedComponentInBounds(ctx, component_mem, resource, index, info, component, data);
		});
	} else if (bits == 8u || bits == 16u) {
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

spv::Op SpirvAtomicOpcode(IR::ValueOpcode opcode) {
	switch (opcode) {
		case IR::ValueOpcode::BufferAtomicCmpSwap32: return spv::OpAtomicCompareExchange;
		case IR::ValueOpcode::BufferAtomicSwap32:
		case IR::ValueOpcode::BufferAtomicSwap64:
		case IR::ValueOpcode::SharedAtomicSwap32: return spv::OpAtomicExchange;
		case IR::ValueOpcode::BufferAtomicIAdd32:
		case IR::ValueOpcode::SharedAtomicIAdd32: return spv::OpAtomicIAdd;
		case IR::ValueOpcode::BufferAtomicISub32:
		case IR::ValueOpcode::SharedAtomicISub32: return spv::OpAtomicISub;
		case IR::ValueOpcode::BufferAtomicSMin32:
		case IR::ValueOpcode::SharedAtomicSMin32: return spv::OpAtomicSMin;
		case IR::ValueOpcode::BufferAtomicUMin32:
		case IR::ValueOpcode::SharedAtomicUMin32: return spv::OpAtomicUMin;
		case IR::ValueOpcode::BufferAtomicSMax32:
		case IR::ValueOpcode::SharedAtomicSMax32: return spv::OpAtomicSMax;
		case IR::ValueOpcode::BufferAtomicUMax32:
		case IR::ValueOpcode::SharedAtomicUMax32: return spv::OpAtomicUMax;
		case IR::ValueOpcode::BufferAtomicAnd32:
		case IR::ValueOpcode::SharedAtomicAnd32: return spv::OpAtomicAnd;
		case IR::ValueOpcode::BufferAtomicOr32:
		case IR::ValueOpcode::BufferAtomicOr64:
		case IR::ValueOpcode::SharedAtomicOr32: return spv::OpAtomicOr;
		case IR::ValueOpcode::BufferAtomicXor32:
		case IR::ValueOpcode::SharedAtomicXor32: return spv::OpAtomicXor;
		default: return spv::OpNop;
	}
}

// The trailing barrier is the acquire edge only; the last-child idiom needs the release too.
uint32_t AtomicSemantics(const IR::MemoryInfo& mem) {
	return spv::MemorySemanticsAcquireReleaseMask |
	       (mem.kind == IR::ResourceKind::Lds ? spv::MemorySemanticsWorkgroupMemoryMask
	                                          : spv::MemorySemanticsUniformMemoryMask);
}

uint32_t EmitAtomicOperation(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t pointer,
                             uint32_t scope, const IR::MemoryInfo& mem) {
	const auto old       = ctx.state.builder.AllocateId();
	const auto semantics = AtomicSemantics(mem);
	if (inst.GetOpcode() == IR::ValueOpcode::BufferAtomicCmpSwap32) {
		const auto desired    = ctx.Arg(inst, inst.NumArgs() - 3);
		const auto comparator = ctx.Arg(inst, inst.NumArgs() - 2);
		// An unequal compare-exchange performs no store, so its semantics carry no release.
		ctx.state.builder.AddFunction(
		    spv::OpAtomicCompareExchange, TypeU32(ctx.state), old, pointer,
		    ConstantU32(ctx.state, scope), ConstantU32(ctx.state, semantics),
		    ConstantU32(ctx.state, spv::MemorySemanticsMaskNone), desired, comparator);
	} else {
		const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
		ctx.state.builder.AddFunction(SpirvAtomicOpcode(inst.GetOpcode()), TypeU32(ctx.state), old,
		                              pointer, ConstantU32(ctx.state, scope),
		                              ConstantU32(ctx.state, semantics), value);
	}
	return old;
}

template <typename Fn>
uint32_t EmitAtomicAccess(ValueEmitContext& ctx, const IR::Inst& inst,
                          const IR::MemoryInfo& mem, Fn&& operation) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		return EmitValueOrZeroIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    return operation(EmitMemoryElementPointer(ctx.state, access.resource, access.index));
		    });
	});
}

template <typename Fn>
uint32_t EmitAtomicUpdate(ValueEmitContext& ctx, const IR::Inst& inst,
                          const IR::MemoryInfo& mem, Fn&& replacement) {
	const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
	return EmitAtomicAccess(ctx, inst, mem, [&](uint32_t pointer) {
		return AtomicUpdate(ctx.state, pointer, mem.kind, [&](uint32_t old) {
			return replacement(ctx.state, old, value);
		});
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
	std::array<uint32_t, 4>  addresses {};
	std::array<uint32_t, 4>  indices {};
	uint32_t                 in_bounds = 0;
};

enum class FormattedAccess { Load, Store };

PreparedFormattedMemory PrepareFormattedMemory(ValueEmitContext& ctx, const IR::Inst& inst,
                                               const IR::MemoryInfo&       mem,
                                               const MemoryResourceAccess& resource,
                                               const Format::BufferFormatInfo& info,
                                               uint32_t components, FormattedAccess access) {
	PreparedFormattedMemory plan;
	plan.info     = info;
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
		const auto byte_offset = Format::GetFormatComponentByteOffset(plan.info, component);
		bool       reused      = false;
		for (uint32_t previous = 0; previous < component; previous++) {
			if (required_components[previous] &&
			    Format::GetFormatComponentByteOffset(plan.info, previous) == byte_offset) {
				plan.addresses[component] = plan.addresses[previous];
				plan.indices[component]   = plan.indices[previous];
				reused                    = true;
				break;
			}
		}
		if (reused) continue;
		const auto component_mem  = RebaseFormattedComponent(mem, plan.info, component);
		plan.addresses[component] = ByteAddress(ctx, inst, component_mem);
		const auto raw_index      = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state),
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
	    ctx, mem, plan.info, output_component,
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

void StoreFormattedInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                            const PreparedFormattedMemory& plan, uint32_t component,
                            uint32_t raw_data) {
	if (component >= plan.info.component_count) return;
	const auto bits = plan.info.component_bits[component];
	const auto data = EncodeFormatComponent(ctx.state, plan.info, component, raw_data);
	if (plan.info.packed_bitfield) {
		StorePackedComponentInBounds(ctx, mem, plan.resource, plan.indices[component], plan.info,
		                             component, data);
	} else if (bits == 8u || bits == 16u) {
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
		    const auto info = Format::GetFormatInfo(
		        mem.formatted ? BufferFormat(ctx, mem) : Prospero::BufferFormat::kInvalid);
		    if (info.type != Format::ComponentType::Unknown) {
			    const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info, components,
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
		const auto info = Format::GetFormatInfo(
		    mem.formatted ? BufferFormat(ctx, mem) : Prospero::BufferFormat::kInvalid);
		if (info.type != Format::ComponentType::Unknown) {
			const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info, components,
			                                         FormattedAccess::Store);
			EmitIfCondition(state, plan.in_bounds, [&]() {
				for (uint32_t component = 0; component < components; component++) {
					const auto data = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), data,
					                          composite, component);
					StoreFormattedInBounds(ctx, mem, plan, component, data);
				}
			});
			return;
		}
		for (uint32_t component = 0; component < components; component++) {
			const auto data = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), data, composite,
			                          component);
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
			    const auto address   = component == 0u
			                               ? base
			                               : Binary(state, spv::OpIAdd, TypeU32(state), base,
			                                        ConstantU32(state, component * 4u));
			    const auto raw_index = Binary(state, spv::OpShiftRightLogical, TypeU32(state),
			                                  address, ConstantU32(state, 2));
			    const auto index     = EmitMemoryElementIndex(state, resource, raw_index);
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
			const auto raw_index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
			                              ConstantU32(state, 2));
			const auto index = EmitMemoryElementIndex(state, resource, raw_index);
			EmitIfCondition(state, EmitMemoryElementInBounds(state, resource, index),
			                [&]() {
				                StoreWordInBounds(ctx, resource, index, ctx.Arg(inst, component + 1u));
			                });
		}
	});
}

} // namespace

namespace {

uint32_t BeginBdaPointerFunction(EmitterState& state, uint32_t function, const char* name,
                                 uint32_t& page, uint32_t& offset) {
	const auto type          = TypeScalarU64(state);
	const auto function_type = state.builder.Type(spv::OpTypeFunction, type, type);
	const auto address       = state.builder.AllocateId();
	const auto entry_label   = state.builder.AllocateId();
	state.builder.AddName(function, name);
	state.builder.AddFunction(spv::OpFunction, type, function, spv::FunctionControlMaskNone,
	                          function_type);
	state.builder.AddFunction(spv::OpFunctionParameter, type, address);
	EmitLabel(state, entry_label);

	const auto page64        = Binary(state, spv::OpShiftRightLogical, type, address,
	                                  ConstantDeviceAddress(state, BufferCache::CACHING_PAGEBITS));
	page                     = Unary(state, spv::OpUConvert, TypeU32(state), page64);
	offset                   = Binary(state, spv::OpBitwiseAnd, type, address,
	                                  ConstantDeviceAddress(state, BufferCache::CACHING_PAGESIZE - 1));
	const auto entry_pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferU64ElementPointer(state),
	                          entry_pointer, state.bda_pagetable_variable, ConstantU32(state, 0),
	                          page);
	const auto base = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, base, entry_pointer);
	return base;
}

uint32_t UntaggedBdaBase(EmitterState& state, uint32_t base) {
	return Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state), base,
	              ConstantDeviceAddress(state, ~BufferCache::BDA_STORE_TRACKED_BIT));
}

// Faults on every page whose entry lacks the tracked tag, so the host learns which pages to own.
void DefineGetBdaStorePointer(EmitterState& state) {
	const auto type                  = TypeScalarU64(state);
	state.bda_store_pointer_function = state.builder.AllocateId();
	uint32_t   page                  = 0;
	uint32_t   offset                = 0;
	const auto base = BeginBdaPointerFunction(state, state.bda_store_pointer_function,
	                                          "get_bda_store_pointer", page, offset);
	const auto untracked =
	    Binary(state, spv::OpIEqual, TypeBool(state),
	           Binary(state, spv::OpBitwiseAnd, type, base,
	                  ConstantDeviceAddress(state, BufferCache::BDA_STORE_TRACKED_BIT)),
	           ConstantDeviceAddress(state, 0));
	EmitIfCondition(state, untracked, [&]() { RecordBdaFault(state, page); });
	const auto missing =
	    Binary(state, spv::OpIEqual, TypeBool(state), base, ConstantDeviceAddress(state, 0));
	const auto available = Binary(state, spv::OpIAdd, type, UntaggedBdaBase(state, base), offset);
	const auto result    = Select(state, type, missing, ConstantDeviceAddress(state, 0), available);
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
	const auto type            = TypeScalarU64(state);
	state.bda_pointer_function = state.builder.AllocateId();
	uint32_t   page            = 0;
	uint32_t   offset          = 0;
	const auto base =
	    BeginBdaPointerFunction(state, state.bda_pointer_function, "get_bda_pointer", page, offset);
	const auto missing =
	    Binary(state, spv::OpIEqual, TypeBool(state), base, ConstantDeviceAddress(state, 0));
	const auto fault_label     = state.builder.AllocateId();
	const auto available_label = state.builder.AllocateId();
	const auto merge_label     = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, missing, fault_label, available_label);

	EmitLabel(state, fault_label);
	RecordBdaFault(state, page);
	state.builder.AddFunction(spv::OpBranch, merge_label);

	EmitLabel(state, available_label);
	const auto available = Binary(state, spv::OpIAdd, type, UntaggedBdaBase(state, base), offset);
	state.builder.AddFunction(spv::OpBranch, merge_label);

	EmitLabel(state, merge_label);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpPhi, type, result, ConstantDeviceAddress(state, 0),
	                          fault_label, available, available_label);
	state.builder.AddFunction(spv::OpReturnValue, result);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

uint32_t EmitAtomic32(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem = ctx.Memory(inst);
	return EmitAtomicAccess(ctx, inst, mem, [&](uint32_t pointer) {
		const auto scope =
		    mem.kind == IR::ResourceKind::Lds ? spv::ScopeWorkgroup : spv::ScopeDevice;
		const auto old = EmitAtomicOperation(ctx, inst, pointer, scope, mem);
		if (mem.kind == IR::ResourceKind::Lds) {
			const auto semantics =
			    spv::MemorySemanticsAcquireReleaseMask | spv::MemorySemanticsWorkgroupMemoryMask;
			ctx.state.builder.AddFunction(spv::OpMemoryBarrier, ConstantU32(ctx.state, scope),
			                              ConstantU32(ctx.state, semantics));
		} else {
			EmitDeviceAtomicMemoryBarrier(ctx.state);
		}
		return old;
	});
}

uint32_t EmitBufferAtomic64(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem   = ctx.Memory(inst);
	auto&       state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU64(state), ConstantU64(state, 0), [&]() {
		    const auto resource = PrepareStorageBufferResourceAccess(
		        state, mem, state.storage_buffer_u64_variable, TypeStorageBufferU64Pointer(state));
		    const auto byte_address = Binary(state, spv::OpIAdd, TypeU32(state),
		                                     ByteAddress(ctx, inst, mem), resource.byte_offset);
		    const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), byte_address,
		                              ConstantU32(state, 3u));
		    return EmitValueOrDefaultIfCondition(
		        state, EmitMemoryElementInBounds(state, resource, index), TypeU64(state),
		        ConstantU64(state, 0), [&]() {
			        const auto value = Unary(state, spv::OpBitcast, TypeScalarU64(state),
			                                 ctx.Arg(inst, inst.NumArgs() - 2));
			        const auto old   = state.builder.AllocateId();
			        state.builder.AddFunction(
			            SpirvAtomicOpcode(inst.GetOpcode()), TypeScalarU64(state), old,
			            EmitStorageBufferElementPointer(state, resource, index,
			                                            TypeStorageBufferU64ElementPointer(state)),
			            ConstantU32(state, spv::ScopeDevice),
			            ConstantU32(state, spv::MemorySemanticsMaskNone), value);
			        EmitDeviceAtomicMemoryBarrier(state);
			        return Unary(state, spv::OpBitcast, TypeU64(state), old);
		        });
	    });
}

uint32_t EmitBufferFloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem       = ctx.Memory(inst);
	const bool  max_value = inst.GetOpcode() == IR::ValueOpcode::BufferAtomicFMax32;
	return EmitAtomicUpdate(ctx, inst, mem,
	                        [max_value](EmitterState& state, uint32_t old, uint32_t value) {
		                        return EmitFloatAtomicReplacement(state, old, value, max_value);
	                        });
}

void EmitSharedFloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem       = ctx.Memory(inst);
	const bool  max_value = inst.GetOpcode() == IR::ValueOpcode::SharedAtomicFMax32;
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		EmitIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    ctx.state.builder.AddFunction(spv::OpStore, ctx.scratch_u32_variable,
			                                  ctx.Arg(inst, 1));
			    const auto data = ctx.state.builder.AllocateId();
			    ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), data,
			                                  ctx.scratch_u32_variable);
			    AtomicUpdate(
			        ctx.state, EmitMemoryElementPointer(ctx.state, access.resource, access.index),
			        mem.kind, [&](uint32_t old) {
				        return EmitFloatAtomicReplacement(ctx.state, old, data, max_value);
			        });
		    });
	});
}

void EmitSharedMaskedOr(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem = ctx.Memory(inst);
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		EmitIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    const auto mask  = ctx.Arg(inst, 1);
			    const auto value = ctx.Arg(inst, 2);
			    const auto keep  = Unary(ctx.state, spv::OpNot, TypeU32(ctx.state), mask);
			    AtomicUpdate(ctx.state,
			                 EmitMemoryElementPointer(ctx.state, access.resource, access.index),
			                 mem.kind, [&](uint32_t old) {
				                 return Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state),
				                               Binary(ctx.state, spv::OpBitwiseAnd,
				                                      TypeU32(ctx.state), old, keep),
				                               value);
			                 });
		    });
	});
}

uint32_t EmitAppendConsume(ValueEmitContext& ctx, const IR::Inst& inst) {
	const bool append = inst.GetOpcode() == IR::ValueOpcode::DataAppend;
	auto&      state  = ctx.state;
	if (ctx.half == 1) {
		return ctx.other_half->Def(IR::Value(const_cast<IR::Inst*>(&inst)));
	}
	const auto m0 = ctx.Arg(inst, 0);
	const auto base =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), m0, ConstantU32(state, 16));
	const auto size =
	    Binary(state, spv::OpBitwiseAnd, TypeU32(state), m0, ConstantU32(state, 0xffffu));
	const auto address = Binary(state, spv::OpIAdd, TypeU32(state), base,
	                            ConstantU32(state, ctx.Memory(inst).offset));
	const auto raw_index =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
	const auto mem    = ctx.Memory(inst);
	const auto access = PrepareMemoryResourceAccess(state, mem);
	const auto index  = EmitMemoryElementIndex(state, access, raw_index);
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
	const auto storage_bounds = EmitMemoryElementInBounds(state, access, index);
	const auto m0_bounds =
	    mem.kind == IR::ResourceKind::Gds
	        ? Binary(state, spv::OpINotEqual, TypeBool(state), size, ConstantU32(state, 0))
	        : Binary(state, spv::OpULessThan, TypeBool(state),
	                 ConstantU32(state, ctx.Memory(inst).offset + 3u), size);
	const auto condition = AndCondition(
	    state, is_first,
	    AndCondition(state,
	                 state.lane_count == 2 ? Binary(state, spv::OpINotEqual, TypeBool(state), count,
	                                                ConstantU32(state, 0))
	                                       : exec,
	                 AndCondition(state, storage_bounds, m0_bounds)));
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

void EmitReadConstBuffer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto mem = ctx.Memory(inst);
	if (mem.planning_only) return;
	auto& state        = ctx.state;
	mem.kind           = IR::ResourceKind::ScalarBuffer;
	const auto address = Binary(state, spv::OpIAdd, TypeU32(state), ctx.Arg(inst, 1),
	                            ConstantU32(state, mem.offset));
	const auto index =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
	const auto access    = PrepareMemoryResourceAccess(state, mem);
	const auto element   = EmitMemoryElementIndex(state, access, index);
	const auto condition = EmitMemoryElementInBounds(state, access, element);
	ctx.Define(inst, EmitValueOrZeroIfCondition(state, condition, [&]() {
		           return LoadResourceWord(state, access, state.builder.AllocateId(),
		                                   EmitMemoryElementPointer(state, access, element));
	           }));
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
	uint32_t   value;
	if (buffer_components > 1u)
		value = LoadWideBuffer(ctx, inst, buffer_components);
	else if (shared_components > 1u)
		value = LoadWideShared(ctx, inst, shared_components);
	else if (address_info.access == IR::AddressAccess::Read &&
	         mem.kind != IR::ResourceKind::Scratch)
		value = LoadBda(ctx, inst, mem, address_info.data_bits);
	else if (op == IR::ValueOpcode::LoadBufferU32 && mem.formatted)
		value = FormattedLoad(ctx, inst, mem);
	else if (inst.GetType() == IR::Type::U8)
		value = LoadSubword(ctx, inst, mem, 8, false);
	else if (inst.GetType() == IR::Type::U16)
		value = LoadSubword(ctx, inst, mem, 16, false);
	else
		value = LoadWord(ctx, inst, mem);
	ctx.Define(inst, value);
}

void EmitStoreMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto  op                = inst.GetOpcode();
	const auto& mem               = ctx.Memory(inst);
	const auto  buffer_components = IR::BufferComponentCount(op);
	const auto  shared_components = IR::SharedComponentCount(op);
	const auto  type              = inst.Arg(inst.NumArgs() - 2).GetType();
	const auto  address_info      = IR::AddressOpcodeInfoOf(op);
	if (buffer_components > 1u)
		StoreWideBuffer(ctx, inst, buffer_components);
	else if (shared_components > 1u)
		StoreWideShared(ctx, inst, shared_components);
	else if (address_info.access == IR::AddressAccess::Write &&
	         mem.kind != IR::ResourceKind::Scratch)
		StoreBda(ctx, inst, mem, address_info.data_bits);
	else if (op == IR::ValueOpcode::StoreBufferU32 && mem.formatted)
		FormattedStore(ctx, inst, mem);
	else if (type == IR::Type::U8)
		StoreSubword(ctx, inst, mem, 8);
	else if (type == IR::Type::U16)
		StoreSubword(ctx, inst, mem, 16);
	else
		StoreWord(ctx, inst, mem);
}

uint32_t EmitSharedIncDec(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto replacement =
	    inst.GetOpcode() == IR::ValueOpcode::SharedAtomicInc32 ? AtomicIncrement : AtomicDecrement;
	return EmitAtomicUpdate(ctx, inst, ctx.Memory(inst), replacement);
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
	return EmitDsForwardPermute(ctx.state, ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2));
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
