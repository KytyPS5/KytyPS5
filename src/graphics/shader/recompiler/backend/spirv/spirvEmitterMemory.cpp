#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <utility>
#include <vector>

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
	const auto pointer = FaultElementPointer(state, word);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	state.builder.AddFunction(spv::OpStore, pointer,
	                          Binary(state, spv::OpBitwiseOr, TypeU32(state), value, bit));
}

constexpr auto BdaPageBytes = static_cast<uint32_t>(BufferCache::CACHING_PAGESIZE);

uint32_t LoadHostDword(EmitterState& state, uint32_t bda, bool coherent) {
	auto pointer = state.builder.AllocateId();
	if (coherent) {
		const auto block =
		    state.builder.DecoratedType(spv::OpTypeStruct,
		                                {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
		                                 {spv::OpMemberDecorate, {0, spv::DecorationCoherent}},
		                                 {spv::OpDecorate, {spv::DecorationBlock}}},
		                                TypeU32(state));
		state.builder.AddFunction(spv::OpConvertUToPtr,
		                          TypePointer(state, spv::StorageClassPhysicalStorageBuffer, block),
		                          pointer, bda);
		const auto base = pointer;
		pointer         = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, TypePhysicalU32Pointer(state), pointer, base,
		                          ConstantU32(state, 0));
	} else {
		state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer,
		                          bda);
	}
	const auto         value     = state.builder.AllocateId();
	constexpr uint32_t alignment = sizeof(uint32_t);
	state.builder.AddFunction(
	    spv::OpLoad, TypeU32(state), value, pointer,
	    spv::MemoryAccessAlignedMask |
	        (coherent ? spv::MemoryAccessVolatileMask : spv::MemoryAccessMaskNone),
	    alignment);
	return value;
}

uint32_t LoadBdaDword(ValueEmitContext& ctx, uint32_t address, bool coherent = false) {
	auto& state = ctx.state;
	if (state.bda_pointer_function == 0) {
		const auto bda = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpFunctionCall, TypeU64(state), bda,
		                          state.bda_dword_function, address);
		return LoadHostDword(state, bda, coherent);
	}
	const auto bda     = GetBdaPointer(state, address);
	const auto present =
	    Binary(state, spv::OpINotEqual, TypeBool(state), bda, ConstantU64(state, 0));
	// A page that no buffer backs yet reads as zero, except in BVH traversal shaders: there it
	// reads as all ones, the absent-node link bvh_intersect returns, because guest list walks end
	// on negative links and would advance over zero entries forever.
	const auto missing = ConstantU32(state, state.program.info.bvh ? ~0u : 0u);
	return EmitValueOrDefaultIfCondition(state, present, TypeU32(state), missing,
	                                     [&]() { return LoadHostDword(state, bda, coherent); });
}

uint32_t TypeU64Pair(EmitterState& state) {
	return state.builder.Type(spv::OpTypeVector, TypeU64(state), 2);
}

// The first load of a run looks up its page and the run's last; the others are in one of them.
uint32_t LoadScalarAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto& state = ctx.state;
	if (state.bda_pointer_function != 0) {
		return LoadBdaDword(ctx, GuestAddress(ctx, inst, mem));
	}
	const auto found = state.bda_runs.find(&inst);
	if (found == state.bda_runs.end()) {
		ctx.Fail(inst, "is a scalar load outside every run");
	}
	const auto& run = found->second;
	if (run.first == &inst) {
		const auto     address = GuestAddress(ctx, inst, mem);
		BdaRunPointers pointers {.label = state.current_label};
		if (run.length == sizeof(uint32_t)) {
			pointers.first_page = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpFunctionCall, TypeU64(state), pointers.first_page,
			                          state.bda_dword_function, address);
		} else {
			const auto pair = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpFunctionCall, TypeU64Pair(state), pair,
			                          state.bda_run_function, address,
			                          ConstantU32(state, run.length));
			pointers.first_page = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeU64(state), pointers.first_page,
			                          pair, 0u);
			pointers.last_page = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeU64(state), pointers.last_page,
			                          pair, 1u);
			pointers.page_offset = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
			                              Unary(state, spv::OpUConvert, TypeU32(state), address),
			                              ConstantU32(state, BdaPageBytes - 1));
		}
		state.bda_run_pointers.insert_or_assign(&inst, pointers);
		return LoadHostDword(state, pointers.first_page, false);
	}
	const auto& pointers = state.bda_run_pointers.at(run.first);
	// No load of a run opens a block, so the first one's lookup is still in scope.
	EXIT_IF(pointers.label != state.current_label);
	const auto in_first_page =
	    Binary(state, spv::OpULessThan, TypeBool(state),
	           Binary(state, spv::OpIAdd, TypeU32(state), pointers.page_offset,
	                  ConstantU32(state, run.distance)),
	           ConstantU32(state, BdaPageBytes));
	const auto base =
	    Select(state, TypeU64(state), in_first_page, pointers.first_page, pointers.last_page);
	return LoadHostDword(
	    state, Binary(state, spv::OpIAdd, TypeU64(state), base, ConstantU64(state, run.distance)),
	    false);
}

uint32_t LoadBda(ValueEmitContext& ctx, uint32_t address, uint32_t active, uint32_t bits,
                 bool coherent = false) {
	auto& state = ctx.state;
	return EmitValueOrZeroIfCondition(state, active, [&]() {
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
			return LoadBdaDword(ctx, Binary(state, spv::OpIAdd, TypeU64(state), aligned,
			                                ConstantU64(state, sizeof(uint32_t))), coherent);
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

uint32_t ByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                     uint32_t prefix = 0) {
	if (mem.kind == IR::ResourceKind::Buffer) {
		if (prefix == 0u) {
			const auto resource = ctx.state.program.info.buffers.at(mem.resource).descriptor_index;
			EXIT_IF(resource == UINT32_MAX);
			prefix = ctx.state.memory_byte_offsets[resource];
		}
		const auto address = Binary(ctx.state, spv::OpIAdd, TypeU32(ctx.state),
		                            BufferByteAddress(ctx, inst, mem), prefix);
		// The host binding prefix must not wrap an out-of-range guest address into the buffer.
		return Select(ctx.state, TypeU32(ctx.state),
		              Binary(ctx.state, spv::OpUGreaterThanEqual, TypeBool(ctx.state), address, prefix),
		              address, ConstantU32(ctx.state, UINT32_MAX));
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

uint32_t DwordIndex(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                    uint32_t prefix = 0) {
	return Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state),
	              ByteAddress(ctx, inst, mem, prefix), ConstantU32(ctx.state, 2));
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index);

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits);

// Emits `emit` on the buffer an access reaches. A GPU-selected table reaches the candidate its
// mapping word names, as the class of its shape; word 0 names nothing.
template <typename Fn>
auto EmitBufferAccess(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem,
                      uint32_t components, Fn&& emit) {
	auto& state = ctx.state;
	constexpr bool has_result = !std::is_void_v<
	    std::invoke_result_t<Fn, const IR::MemoryInfo&, const MemoryResourceAccess&>>;
	const auto access = [&](uint32_t resource, uint32_t slot) {
		mem.resource = resource;
		if (mem.kind == IR::ResourceKind::Buffer) {
			const auto& buffer = state.program.info.buffers[resource];
			if (buffer.packed_stride == 0u &&
			    buffer.source < state.program.descriptor_sources.size() &&
			    state.program.descriptor_sources[buffer.source].indirect_descriptor) {
				// Empty candidates still carry the format/swizzle needed for OOB loads.
				return emit(mem, MemoryResourceAccess{});
			}
		}
		return emit(mem, PrepareMemoryResourceAccess(state, mem, slot));
	};
	if (mem.kind != IR::ResourceKind::Buffer ||
	    state.program.info.buffers[mem.resource].indirect_root != mem.resource) {
		return access(mem.resource, 0u);
	}
	// A table's children are its classes, after the table itself.
	const auto& classes = state.program.info.buffers[mem.resource].indirect_resources;
	const auto  arms    = static_cast<uint32_t>(classes.size()) - 1u;
	const auto  word    = ctx.Arg(*inst.Arg(0).ResolveInstruction(), 0);
	const auto  slot =
	    EmitAddU32(state,
	               Binary(state, spv::OpBitwiseAnd, TypeU32(state), word,
	                      ConstantU32(state, (1u << IR::CandidateClassShift) - 1u)),
	               EmitShaderDataDwordLoad(state, state.program.bindings.candidate_buffer_dword));
	const auto selected_access = [&]() {
		if (arms == 1u) return access(classes[1], slot);
		const auto arm = Binary(state, spv::OpISub, TypeU32(state),
		                        Binary(state, spv::OpShiftRightLogical, TypeU32(state), word,
		                               ConstantU32(state, IR::CandidateClassShift)),
		                        ConstantU32(state, 1u));
		return EmitIndexSwitch(state, arm, arms,
		                       has_result ? TypeU32Composite(state, components) : 0u,
		                       [&](uint32_t item) { return access(classes[item + 1u], slot); });
	};
	const auto valid =
	    Binary(state, spv::OpINotEqual, TypeBool(state), word, ConstantU32(state, 0u));
	if constexpr (has_result) {
		return EmitValueOrDefaultIfCondition(state, valid, TypeU32Composite(state, components),
		                                     ConstantU32CompositeZero(state, components),
		                                     selected_access);
	} else {
		EmitIfCondition(state, valid, selected_access);
	}
}

uint32_t BufferIndexInBounds(ValueEmitContext& ctx, const IR::Inst& inst,
                             const IR::MemoryInfo& mem, const MemoryResourceAccess& resource,
                             uint32_t payload_in_bounds) {
	if (mem.kind != IR::ResourceKind::Buffer) return payload_in_bounds;
	auto& state = ctx.state;
	const auto& buffer = state.program.info.buffers[mem.resource];
	const auto& sources = state.program.descriptor_sources;
	if (buffer.source >= sources.size() || !sources[buffer.source].indirect_descriptor ||
	    !sources[buffer.source].indirect_descriptor->indexed) return payload_in_bounds;
	const auto bytes = Binary(state, spv::OpIMul, TypeU32(state), resource.length,
	                          ConstantU32(state, resource.element_bits / 8u));
	const auto available = Binary(state, spv::OpISub, TypeU32(state), bytes, resource.byte_offset);
	// Test the original index before multiplication can wrap. A truncated binding
	// may omit final-record padding; the payload check below still checks every byte.
	const auto last_record = Binary(state, spv::OpUDiv, TypeU32(state),
	    Binary(state, spv::OpISub, TypeU32(state), available, ConstantU32(state, 1u)),
	    ConstantU32(state, buffer.packed_stride & 0x3fffu));
	return AndCondition(state, payload_in_bounds,
	    AndCondition(state,
	        Binary(state, spv::OpUGreaterThan, TypeBool(state), bytes, resource.byte_offset),
	        Binary(state, spv::OpULessThanEqual, TypeBool(state), ctx.Arg(inst, 1), last_record)));
}

uint32_t LoadWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource) {
	const auto index = DwordIndex(ctx, inst, mem, resource.byte_offset);
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitStorageElementInBounds(ctx.state, resource, index),
	    [&]() { return LoadWordInBounds(ctx, resource, index); });
}

uint32_t LoadWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadWordPrepared(ctx, inst, mem, resource);
	});
}

uint32_t LoadSubwordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                             const MemoryResourceAccess& resource, uint32_t bits) {
	const auto address = ByteAddress(ctx, inst, mem, resource.byte_offset);
	const auto index = Binary(
	    ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	    ConstantU32(ctx.state, std::countr_zero(resource.element_bits / 8u)));
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitStorageElementInBounds(ctx.state, resource, index),
	    [&]() { return LoadSubwordInBounds(ctx, resource, address, index, bits); });
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index) {
	const auto value   = ctx.state.builder.AllocateId();
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), value, pointer,
	                              resource.memory_access);
	return value;
}

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits) {
	uint32_t value;
	if (resource.element_bits < 32u) {
		const auto loaded = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpLoad,
		                              ctx.state.storage_buffers[bits == 8u ? 0 : 1].element_type, loaded,
		                              EmitMemoryElementPointer(ctx.state, resource, index),
		                              resource.memory_access);
		value = Unary(ctx.state, spv::OpUConvert, TypeU32(ctx.state), loaded);
	} else {
		const auto word = LoadWordInBounds(ctx, resource, index);
		const auto byte = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
		                         ConstantU32(ctx.state, 3));
		const auto shift = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), byte,
		                          ConstantU32(ctx.state, 3));
		value = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state),
		               Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), word, shift),
		               ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu));
	}
	return value;
}

uint32_t LoadSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem,
                     uint32_t bits) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadSubwordPrepared(ctx, inst, mem, resource, bits);
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
	if (resource.object_pointer == 0u) return;
	const auto address = ByteAddress(ctx, inst, mem, resource.byte_offset);
	const auto index = Binary(
	    ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	    ConstantU32(ctx.state, std::countr_zero(resource.element_bits / 8u)));
	// 32-bit views merge subwords in a compare-exchange loop, and out-of-range atomics return
	// undefined values.
	const auto in_bounds = resource.element_bits < 32u
	                           ? EmitStorageElementInBounds(ctx.state, resource, index)
	                           : EmitMemoryElementInBounds(ctx.state, resource, index);
	EmitIfCondition(ctx.state, BufferIndexInBounds(ctx, inst, mem, resource, in_bounds), [&]() {
		StoreSubwordInBounds(ctx, mem, resource, address, index, bits, data);
	});
}

void StoreSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		EmitBufferAccess(ctx, inst, mem, 0u, [&](const auto& selected, const auto& resource) {
			StoreSubwordPrepared(ctx, inst, selected, resource, bits, ctx.Arg(inst, inst.NumArgs() - 2));
		});
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
		const auto selected = Binary(state, spv::OpIEqual, TypeBool(state), high, ConstantU32(state, aperture));
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
	if (resource.object_pointer == 0u) return;
	const auto index = DwordIndex(ctx, inst, mem, resource.byte_offset);
	const auto in_bounds = BufferIndexInBounds(ctx, inst, mem, resource,
	    EmitStorageElementInBounds(ctx.state, resource, index));
	EmitIfCondition(ctx.state, in_bounds, [&]() {
		StoreWordInBounds(ctx, resource, index, data);
	});
}

void StoreWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		EmitBufferAccess(ctx, inst, mem, 0u, [&](const auto& selected, const auto& resource) {
			StoreWordPrepared(ctx, inst, selected, resource, ctx.Arg(inst, inst.NumArgs() - 2));
		});
	});
}

template <typename Fn>
uint32_t EmitAtomicAccess(ValueEmitContext& ctx, const IR::Inst& inst,
                          const IR::MemoryInfo& mem, Fn&& operation) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		auto index = DwordIndex(ctx, inst, mem);
		if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
			// Keep region-address truncation local to DS atomics.
			index = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), index,
			               ConstantU32(ctx.state, 0x3fffu));
		}
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return EmitValueOrZeroIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
			    return operation(EmitMemoryElementPointer(ctx.state, resource, index));
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
	const auto start = ByteAddress(ctx, inst, mem, resource.byte_offset);
	// Check the whole, unaligned format span before aligning the actual memory address.
	const auto end = Binary(ctx.state, spv::OpIAdd, TypeU32(ctx.state), start,
	                        ConstantU32(ctx.state, info.byte_size - 1u));
	const auto last_index = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), end,
	                               ConstantU32(ctx.state, std::countr_zero(resource.element_bits / 8u)));
	plan.in_bounds = BufferIndexInBounds(ctx, inst, mem, resource, AndCondition(
	    ctx.state, Binary(ctx.state, spv::OpUGreaterThanEqual, TypeBool(ctx.state), end, start),
	    EmitMemoryElementInBounds(ctx.state, plan.resource, last_index)));
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
		                            FormattedComponentAddress(ctx.state, plan, component).second);
	    },
	    [&](uint32_t component, uint32_t bits) {
		    const auto [address, index] = FormattedComponentAddress(ctx.state, plan, component);
		    return LoadSubwordInBounds(ctx, plan.resource, address, index, bits);
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

uint32_t LoadFormattedPrepared(ValueEmitContext& ctx, const IR::Inst& inst,
                               const IR::MemoryInfo& mem, const MemoryResourceAccess& resource,
                               const Format::BufferFormatInfo& info, uint32_t components) {
	auto&      state = ctx.state;
	const auto count = mem.data_bits == 16u ? mem.component_count : components;
	const auto plan = resource.object_pointer == 0u ? PreparedFormattedMemory{.info = info}
	                                              : PrepareFormattedMemory(ctx, inst, mem, resource, info);
	const auto out_of_bounds = FormattedOutOfBoundsValue(ctx, mem, plan, count);
	const auto value = resource.object_pointer == 0u ? out_of_bounds : EmitValueOrDefaultIfCondition(
	    state, plan.in_bounds, TypeU32Composite(state, count), out_of_bounds, [&]() {
		    std::array<uint32_t, 4> values {};
		    for (uint32_t component = 0; component < count; component++) {
			    values[component] = LoadFormattedInBounds(ctx, mem, plan, component);
		    }
		    return ConstructU32Composite(state, count, values);
	    });
	if (mem.data_bits != 16u) return value;
	const bool integer = info.type == Format::ComponentType::Uint ||
	                     info.type == Format::ComponentType::Sint;
	const auto scalar = [&](uint32_t component) {
		if (component >= count) return integer ? ConstantU32(state, 0u) : ConstantF32(state, 0);
		auto result = value;
		if (count != 1u) {
			result = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), result, value,
			                          component);
		}
		if (info.type == Format::ComponentType::Uint) {
			return EmitUMin32(state, result, ConstantU32(state, 0xffffu));
		}
		if (info.type == Format::ComponentType::Sint) {
			const auto lower = EmitSMax32(state, result, ConstantU32(state, 0xffff8000u));
			return EmitSMin32(state, lower, ConstantU32(state, 0x7fffu));
		}
		return EmitBitCastF32U32(state, result);
	};
	std::array<uint32_t, 4> packed {};
	for (uint32_t word = 0; word < components; word++) {
		const auto low  = scalar(word * 2u);
		const auto high = scalar(word * 2u + 1u);
		if (integer) {
			packed[word] = Binary(
			    state, spv::OpBitwiseOr, TypeU32(state),
			    Binary(state, spv::OpBitwiseAnd, TypeU32(state), low, ConstantU32(state, 0xffffu)),
			    Binary(state, spv::OpShiftLeftLogical, TypeU32(state), high, ConstantU32(state, 16u)));
		} else {
			const auto pair = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, 2), pair,
			                          low, high);
			packed[word] = EmitPackHalf2x16(state, pair);
		}
	}
	return ConstructU32Composite(state, components, packed);
}

void StoreFormattedPrepared(ValueEmitContext& ctx, const IR::Inst& inst,
                             const IR::MemoryInfo& mem, const MemoryResourceAccess& resource,
                             const Format::BufferFormatInfo& info, uint32_t data,
                             uint32_t components) {
	if (resource.object_pointer == 0u) return;
	// RDNA2 formatted stores transfer the entire format, zero-filling missing VGPRs.
	const auto count = mem.data_bits == 16u ? mem.component_count : components;
	const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info);
	EmitIfCondition(ctx.state, plan.in_bounds, [&]() {
		auto packed = ConstantU32(ctx.state, 0);
		for (uint32_t component = 0; component < info.component_count; component++) {
			auto value = ConstantU32(ctx.state, 0);
			if (component < count) {
				const auto word = mem.data_bits == 16u ? component / 2u : component;
				value = data;
				if (components != 1u) {
					value = ctx.state.builder.AllocateId();
					ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state),
					                              value, data, word);
				}
				if (mem.data_bits == 16u && (component & 1u) != 0u)
					value = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), value,
					               ConstantU32(ctx.state, 16u));
			}
			value = EncodeFormattedStoreComponent(ctx.state, info, component, value, mem.data_bits);
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
		    const auto base = PackU64(state,
		                              Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		                                     ctx.Arg(handle, 0), ConstantU32(state, ~3u)),
		                              Binary(state, spv::OpBitwiseAnd, TypeU32(state), word1,
		                                     ConstantU32(state, 0xffffu)));
		    return LoadBdaDword(ctx, Binary(state, spv::OpIAdd, TypeU64(state), base, offset));
	    });
}

struct IndirectBufferAccess {
	uint32_t address;
	uint32_t offset;
	uint32_t stride;
	uint32_t swizzle;
	uint32_t format;
	uint32_t selector;
	uint32_t dst_sel; // DST_SEL_XYZW of a formatted read, three bits per component.
	uint32_t mode;
	uint32_t records;
	uint32_t index_in_bounds;
	uint32_t scalar_in_bounds;
	uint32_t raw_records;
	uint32_t raw_index_in_bounds;
};

std::array<uint32_t, 8> IndirectBufferArgs(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	const auto& handle = *inst.Arg(0).ResolveInstruction();
	auto offset = ctx.Arg(inst, 2);
	if (ctx.Memory(inst).offset != 0u) {
		const auto immediate = ConstantU32(state, ctx.Memory(inst).offset);
		offset = offset == ConstantU32(state, 0) ? immediate
		    : Binary(state, spv::OpIAdd, TypeU32(state), offset, immediate);
	}
	return {ctx.Arg(handle, 0), ctx.Arg(handle, 1), ctx.Arg(handle, 2), ctx.Arg(handle, 3),
	        ctx.Arg(inst, 1), offset, ctx.Arg(inst, 3), BufferLane(state)};
}

IndirectBufferAccess PrepareIndirectBuffer(EmitterState& state,
                                           const std::array<uint32_t, 8>& args, bool formatted) {
	const auto& [word0, word1, records, word3, input_index, offset, soffset, lane] = args;
	const auto field = [&](uint32_t word, uint32_t first, uint32_t count) {
		return EmitBitFieldUExtract(state, word, ConstantU32(state, first), ConstantU32(state, count));
	};
	const auto nonzero = [&](uint32_t value) {
		return Binary(state, spv::OpINotEqual, TypeBool(state), value, ConstantU32(state, 0));
	};
	const auto stride  = field(word1, 16, 14);
	const auto swizzle = AndCondition(state, nonzero(stride), nonzero(field(word1, 31, 1)));
	const auto index = Binary(
	    state, spv::OpIAdd, TypeU32(state), input_index,
	    Select(state, TypeU32(state), nonzero(field(word3, 23, 1)), lane,
	           ConstantU32(state, 0)));
	const auto address = CalculateBufferAddress(state, index, offset, soffset, 0u, stride, swizzle,
	                                            field(word3, 21, 2));
	const auto base        = PackU64(state, word0, field(word1, 0, 16));
	const auto raw_records = Binary(state, spv::OpISub, TypeU32(state), records, soffset);
	return {
	    .address          = Binary(state, spv::OpIAdd, TypeU64(state), base,
	                               Unary(state, spv::OpUConvert, TypeU64(state), address.byte)),
	    .offset           = address.offset,
	    .stride           = stride,
	    .swizzle          = swizzle,
	    .format           = field(word3, 12, 7),
	    .selector         = formatted ? field(word3, 0, 3) : 0u,
	    .dst_sel          = formatted ? field(word3, 0, 12) : 0u,
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
		    state, Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), size, bytes),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state), offset,
		           Binary(state, spv::OpISub, TypeU32(state), size, bytes)));
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

uint32_t LoadIndirectBuffer(ValueEmitContext& ctx, const IndirectBufferAccess& buffer,
                            uint32_t bits, uint32_t components, bool coherent) {
	auto& state = ctx.state;
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
		                        IndirectBufferInBounds(state, buffer, component * 4u,
		                                               ConstantU32(state, bits / 8u), false)),
		    [&]() {
			    const auto value = LoadBdaDword(ctx, address, coherent);
			    if (bits == 32u) return value;
			    const auto byte = Binary(
			        state, spv::OpBitwiseAnd, TypeU32(state),
			        Unary(state, spv::OpUConvert, TypeU32(state), buffer.address),
			        ConstantU32(state, bits == 8u ? 3u : 2u));
			    return EmitBitFieldUExtract(
			        state, value,
			        Binary(state, spv::OpShiftLeftLogical, TypeU32(state), byte, ConstantU32(state, 3u)),
			        ConstantU32(state, bits));
		    });
	}
	return ConstructU32Composite(state, components, values);
}

// One component of a formatted read: `selector` is that component's DST_SEL.
uint32_t LoadIndirectFormattedComponent(ValueEmitContext& ctx, IndirectBufferAccess buffer,
                                        uint32_t selector) {
	auto& state         = ctx.state;
	buffer.selector     = selector;
	const auto geometry = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeU32Composite(state, 4), geometry,
	                          state.indirect_format_geometry_function, buffer.format);
	const auto field = [&](uint32_t index) {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), value, geometry, index);
		return value;
	};
	const auto bytes = field(0);
	const auto component = Binary(
	    state, spv::OpUMod, TypeU32(state),
	    Binary(state, spv::OpISub, TypeU32(state), buffer.selector, ConstantU32(state, 4)), field(1));
	const auto alignment = Unary(state, spv::OpUConvert, TypeU64(state), field(3));
	const auto base = Binary(state, spv::OpBitwiseAnd, TypeU64(state), buffer.address,
	                         Unary(state, spv::OpNot, TypeU64(state), alignment));
	const auto address = Binary(
	    state, spv::OpIAdd, TypeU64(state), base,
	    Unary(state, spv::OpUConvert, TypeU64(state),
	          Binary(state, spv::OpIMul, TypeU32(state), component, field(2))));
	const auto memory = AndCondition(
	    state, Binary(state, spv::OpINotEqual, TypeBool(state), bytes, ConstantU32(state, 0)),
	    Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), buffer.selector,
	           ConstantU32(state, 4)));
	const auto raw = EmitValueOrZeroIfCondition(
	    state, AndCondition(state, memory, IndirectBufferInBounds(state, buffer, 0u, bytes, true)),
	    [&]() {
		    // Format alignment and component spacing keep every selected component within one DWORD.
		    const auto word = LoadBdaDword(
		        ctx, Binary(state, spv::OpBitwiseAnd, TypeU64(state), address,
		                    ConstantU64(state, ~uint64_t {3})));
		    const auto byte = Binary(
		        state, spv::OpBitwiseAnd, TypeU32(state),
		        Unary(state, spv::OpUConvert, TypeU32(state), address), ConstantU32(state, 3));
		    return Binary(state, spv::OpShiftRightLogical, TypeU32(state), word,
		                  Binary(state, spv::OpShiftLeftLogical, TypeU32(state), byte,
		                         ConstantU32(state, 3)));
	    });
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeU32(state), result,
	                          state.indirect_format_function, buffer.format, buffer.selector,
	                          component, raw);
	return result;
}

uint32_t LoadRawBuffer(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                       const MemoryResourceAccess& resource, uint32_t components) {
	auto&      state  = ctx.state;
	const auto scalar = [&] {
		std::array<uint32_t, 4> values {};
		for (uint32_t component = 0; component < components; component++) {
			values[component] =
			    LoadWordPrepared(ctx, inst, RebaseRawComponent(mem, component), resource);
		}
		return ConstructU32Composite(state, components, values);
	};
	if (!mem.IsCoherentBufferPairLoad(inst.GetOpcode())) return scalar();

	const auto first  = ByteAddress(ctx, inst, mem, resource.byte_offset);
	const auto second = ByteAddress(ctx, inst, RebaseRawComponent(mem, 1), resource.byte_offset);
	const auto wide =
	    PrepareStorageBufferResourceAccess(state, mem, state.storage_buffers[3],
	                                       resource.non_uniform ? resource.descriptor_index : 0u);
	const auto index =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), first, ConstantU32(state, 3));
	auto atomic =
	    Binary(state, spv::OpIEqual, TypeBool(state),
	           Binary(state, spv::OpBitwiseAnd, TypeU32(state), first, ConstantU32(state, 7)),
	           ConstantU32(state, 0));
	atomic = AndCondition(state, atomic,
	                      Binary(state, spv::OpIEqual, TypeBool(state), second,
	                             EmitAddU32(state, first, ConstantU32(state, 4))));
	// The 64-bit alias requires the entire pair; partial and swizzled reads retain DWORD bounds.
	atomic = AndCondition(state, atomic, EmitMemoryElementInBounds(state, wide, index));
	const auto atomic_label = state.builder.AllocateId(), scalar_label = state.builder.AllocateId();
	const auto merge = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, atomic, atomic_label, scalar_label);
	EmitLabel(state, atomic_label);
	const auto pointer = EmitStorageBufferElementPointer(
	    state, wide, index, TypeStorageBufferElementPointer(state, 64));
	const auto snapshot = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAtomicLoad, TypeU64(state), snapshot, pointer,
	                          ConstantU32(state, spv::ScopeDevice),
	                          ConstantU32(state, spv::MemorySemanticsAcquireMask |
	                                                 spv::MemorySemanticsUniformMemoryMask));
	const std::array<uint32_t, 4> words {
	    EmitCompositeExtractU64(state, snapshot, IR::Value(0u)),
	    EmitCompositeExtractU64(state, snapshot, IR::Value(1u))};
	const auto pair = ConstructU32Composite(state, 2, words);
	state.builder.AddFunction(spv::OpBranch, merge);
	EmitLabel(state, scalar_label);
	const auto fallback    = scalar();
	const auto scalar_exit = state.current_label;
	state.builder.AddFunction(spv::OpBranch, merge);
	EmitLabel(state, merge);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpPhi, TypeU32Composite(state, 2), result, pair, atomic_label,
	                          fallback, scalar_exit);
	return result;
}

uint32_t LoadBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	const auto mem = ctx.Memory(inst);
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU32Composite(state, components),
	    ConstantU32CompositeZero(state, components), [&]() {
		    if (mem.kind == IR::ResourceKind::IndirectBuffer) {
			    const auto function = state.indirect_buffer_load_functions.at(
			        {mem.data_bits, components, uint32_t(mem.coherent), uint32_t(mem.formatted)});
			    const auto args = IndirectBufferArgs(ctx, inst);
			    const auto result = state.builder.AllocateId();
			    state.builder.AddFunction(spv::OpFunctionCall, TypeU32Composite(state, components),
			                              result, function, args);
			    return result;
		    }
		    return EmitBufferAccess(ctx, inst, mem, components, [&](const auto& selected, const auto& resource) {
			    const auto info = Format::GetFormatInfo(
			        selected.formatted ? BufferFormat(ctx, selected) : Prospero::BufferFormat::kInvalid);
			    if (info.type != Format::ComponentType::Unknown) {
				    return LoadFormattedPrepared(ctx, inst, selected, resource, info, components);
			    }
			    if (resource.object_pointer == 0u) return ConstantU32CompositeZero(state, components);
			    return LoadRawBuffer(ctx, inst, selected, resource, components);
		    });
	    });
}

void StoreBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto mem = ctx.Memory(inst);
		const auto composite = ctx.Arg(inst, inst.NumArgs() - 2);
		EmitBufferAccess(ctx, inst, mem, 0u, [&](const auto& selected, const auto& resource) {
			const auto info = Format::GetFormatInfo(
			    selected.formatted ? BufferFormat(ctx, selected) : Prospero::BufferFormat::kInvalid);
			if (info.type != Format::ComponentType::Unknown) {
				StoreFormattedPrepared(ctx, inst, selected, resource, info, composite, components);
				return;
			}
			for (uint32_t component = 0; component < components; component++) {
				auto data = composite;
				if (components != 1u) {
					data = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), data, composite,
					                          component);
				}
				StoreWordPrepared(ctx, inst, RebaseRawComponent(selected, component), resource, data);
			}
		});
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

// A formatted indirect read takes its format's geometry and conversion from two tables per module,
// a row per format and an entry per conversion kind, so the shader's constant data does not grow
// with its reads. Per-format switches did: a driver that inlines each call keeps a copy of their
// constants per call, and past the hardware constant bank (64 KiB) the dispatch fails at launch
// (PPSA02015, NVIDIA SKEDCHECK23_CONSTANT_BUFFER_SIZE, device lost).
//
// Row words: 0 = byte size | component count << 8 | packed << 12 | signed << 13 | unknown << 14 |
// integer << 15; 1 and 2 = components 0-1 and 2-3, 16 bits each (bit offset, bit count << 5,
// conversion kind << 11); 3 = first component bytes | read alignment mask << 8.
// Kind words: 0 = the conversion, 1 = the factor of a scaled or normalized component.
struct IndirectFormatKind {
	Format::ComponentType type = Format::ComponentType::Unknown;
	uint32_t              bits = 0;
	bool                  operator==(const IndirectFormatKind&) const = default;
};

// Bits pass through; a factor scales the component's integer, with a floor of -1 when signed and
// normalized; the floats widen to 32 bits.
enum class IndirectFormatConversion : uint32_t {
	Bits,
	Factor,
	FactorFloor,
	Half,
	Float11,
	Float10
};

void DefineIndirectBufferFormats(ValueEmitContext& ctx) {
	auto& state = ctx.state;
	const auto type = TypeU32(state);
	const auto     row_type = TypeU32Composite(state, 4);
	constexpr auto format_count =
	    static_cast<uint32_t>(Prospero::BufferFormat::k32_32_32_32Float) + 1u;

	// Rows name each distinct conversion by index. Integers pass their bits through and scaled
	// formats convert whole values, so only normalized and float components convert by their width.
	std::vector<IndirectFormatKind> kinds;
	const auto kind_of = [&](Format::ComponentType component_type, uint32_t bits) {
		using Type          = Format::ComponentType;
		const bool by_width = component_type == Type::Unorm || component_type == Type::Snorm ||
		                      component_type == Type::Float;
		const IndirectFormatKind kind {component_type == Type::Sint ? Type::Uint : component_type,
		                               by_width ? bits : 0u};
		auto                     found = std::ranges::find(kinds, kind);
		if (found == kinds.end()) found = kinds.insert(kinds.end(), kind);
		return static_cast<uint32_t>(found - kinds.begin());
	};
	std::vector<uint32_t> rows;
	rows.reserve(format_count);
	for (uint32_t format = 0; format < format_count; ++format) {
		const auto info    = Format::GetFormatInfo(static_cast<Prospero::BufferFormat>(format));
		const bool unknown = info.type == Format::ComponentType::Unknown;
		const bool integer =
		    info.type == Format::ComponentType::Uint || info.type == Format::ComponentType::Sint;
		EXIT_IF(info.byte_size > 0xffu || info.component_count > 4u);
		std::array<uint32_t, 4> words {};
		words[0] = info.byte_size | info.component_count << 8u |
		           uint32_t {info.packed_bitfield} << 12u |
		           uint32_t {IsSignedFormatComponent(info.type)} << 13u |
		           uint32_t {unknown} << 14u | uint32_t {integer} << 15u;
		for (uint32_t component = 0; !unknown && component < info.component_count; ++component) {
			const auto bits   = info.component_bits[component];
			const auto offset = info.packed_bitfield ? info.component_bit_offset[component] : 0u;
			EXIT_IF(bits == 0u || bits > 32u || offset + bits > 32u);
			const auto field = offset | bits << 5u | kind_of(info.type, bits) << 11u;
			words[1u + component / 2u] |= field << (component % 2u * 16u);
		}
		words[3] = (info.packed_bitfield ? 0u : info.component_bits[0] / 8u) |
		           (info.byte_size == 0u ? 0u : std::min(4u, info.byte_size) - 1u) << 8u;
		rows.push_back(
		    state.builder.Constant(spv::OpConstantComposite, row_type, ConstantU32(state, words[0]),
		                           ConstantU32(state, words[1]), ConstantU32(state, words[2]),
		                           ConstantU32(state, words[3])));
	}
	EXIT_IF(kinds.empty() || kinds.size() > 32u);
	const auto table_type =
	    state.builder.Type(spv::OpTypeArray, row_type, ConstantU32(state, format_count));
	const auto table = state.builder.DefineGlobalVariable(
	    TypePointer(state, spv::StorageClassPrivate, table_type), spv::StorageClassPrivate,
	    state.builder.Constant(spv::OpConstantComposite, table_type,
	                           std::span<const uint32_t> {rows}));
	state.builder.AddName(table, "indirect_buffer_formats");
	const auto            kind_type = TypeU32Composite(state, 2);
	std::vector<uint32_t> conversions;
	for (const auto& kind: kinds) {
		using Type       = Format::ComponentType;
		auto  conversion = IndirectFormatConversion::Bits;
		float factor     = 0.0f;
		if (kind.type == Type::Uscaled || kind.type == Type::Sscaled) {
			conversion = IndirectFormatConversion::Factor;
			factor     = 1.0f;
		} else if (kind.type == Type::Unorm || kind.type == Type::Snorm) {
			// The reciprocal a host folds its division by the constant maximum into.
			const auto value_bits = kind.bits - (kind.type == Type::Snorm ? 1u : 0u);
			EXIT_IF(value_bits == 0u || value_bits > 24u);
			conversion = kind.type == Type::Snorm ? IndirectFormatConversion::FactorFloor
			                                      : IndirectFormatConversion::Factor;
			factor     = 1.0f / static_cast<float>((1u << value_bits) - 1u);
		} else if (kind.type == Type::Float && kind.bits != 32u) {
			conversion = kind.bits == 16u   ? IndirectFormatConversion::Half
			             : kind.bits == 11u ? IndirectFormatConversion::Float11
			                                : IndirectFormatConversion::Float10;
		}
		conversions.push_back(
		    state.builder.Constant(spv::OpConstantComposite, kind_type,
		                           ConstantU32(state, static_cast<uint32_t>(conversion)),
		                           ConstantU32(state, std::bit_cast<uint32_t>(factor))));
	}
	const auto kind_table_type = state.builder.Type(
	    spv::OpTypeArray, kind_type, ConstantU32(state, static_cast<uint32_t>(kinds.size())));
	const auto kind_table = state.builder.DefineGlobalVariable(
	    TypePointer(state, spv::StorageClassPrivate, kind_table_type), spv::StorageClassPrivate,
	    state.builder.Constant(spv::OpConstantComposite, kind_table_type,
	                           std::span<const uint32_t> {conversions}));
	state.builder.AddName(kind_table, "indirect_buffer_format_kinds");
	// A format past the table reads as format 0, as the switches' default case did.
	const auto load_row = [&](uint32_t format) {
		const auto index   = Select(state, type,
		                            Binary(state, spv::OpULessThan, TypeBool(state), format,
		                                   ConstantU32(state, format_count)),
		                            format, ConstantU32(state, 0));
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain,
		                          TypePointer(state, spv::StorageClassPrivate, row_type), pointer,
		                          table, index);
		const auto row = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpLoad, row_type, row, pointer);
		return row;
	};
	const auto word_of = [&](uint32_t row, uint32_t index) {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, type, value, row, index);
		return value;
	};
	const auto bits_of = [&](uint32_t value, uint32_t offset, uint32_t count) {
		const auto extracted = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpBitFieldUExtract, type, extracted, value,
		                          ConstantU32(state, offset), ConstantU32(state, count));
		return extracted;
	};
	const auto flag = [&](uint32_t value, uint32_t bit) {
		return Binary(state, spv::OpINotEqual, TypeBool(state), bits_of(value, bit, 1u),
		              ConstantU32(state, 0));
	};

	{
		const auto function_type = state.builder.Type(spv::OpTypeFunction, row_type, type);
		state.indirect_format_geometry_function = state.builder.AllocateId();
		const auto format = state.builder.AllocateId();
		state.builder.AddName(state.indirect_format_geometry_function,
		                      "indirect_buffer_format_geometry");
		state.builder.AddFunction(spv::OpFunction, row_type,
		                          state.indirect_format_geometry_function,
		                          spv::FunctionControlMaskNone, function_type);
		state.builder.AddFunction(spv::OpFunctionParameter, type, format);
		EmitLabel(state, state.builder.AllocateId());
		const auto row    = load_row(format);
		const auto head   = word_of(row, 0u);
		const auto tail   = word_of(row, 3u);
		const auto count  = bits_of(head, 8u, 4u);
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeConstruct, row_type, result,
		                          bits_of(head, 0u, 8u),
		                          EmitUMax32(state, count, ConstantU32(state, 1)),
		                          bits_of(tail, 0u, 8u), bits_of(tail, 8u, 8u));
		state.builder.AddFunction(spv::OpReturnValue, result);
		state.builder.AddFunction(spv::OpFunctionEnd);
	}

	const auto function_type = state.builder.Type(spv::OpTypeFunction, type, type, type, type, type);
	state.indirect_format_function = state.builder.AllocateId();
	const auto format = state.builder.AllocateId();
	const auto selector = state.builder.AllocateId();
	const auto component = state.builder.AllocateId();
	const auto raw = state.builder.AllocateId();
	state.builder.AddName(state.indirect_format_function, "convert_indirect_buffer_format");
	state.builder.AddFunction(spv::OpFunction, type, state.indirect_format_function,
	                          spv::FunctionControlMaskNone, function_type);
	state.builder.AddFunction(spv::OpFunctionParameter, type, format);
	state.builder.AddFunction(spv::OpFunctionParameter, type, selector);
	state.builder.AddFunction(spv::OpFunctionParameter, type, component);
	state.builder.AddFunction(spv::OpFunctionParameter, type, raw);
	EmitLabel(state, state.builder.AllocateId());
	const auto row       = load_row(format);
	const auto head      = word_of(row, 0u);
	const auto packed    = flag(head, 12u);
	const auto is_signed = flag(head, 13u);
	const auto count     = bits_of(head, 8u, 4u);
	// DST_SEL 1 reads the format's one, 0 and the reserved 2-3 read zero.
	const auto one      = Select(state, type, flag(head, 15u), ConstantU32(state, 1u),
	                             ConstantU32(state, 0x3f800000u));
	const auto constant = Select(
	    state, type, Binary(state, spv::OpIEqual, TypeBool(state), selector, ConstantU32(state, 1)),
	    one, ConstantU32(state, 0));
	// A packed format's components each have their own bits in the word; any other reads its
	// first component from the word the address selected.
	const auto selected =
	    Select(state, type,
	           AndCondition(state, packed,
	                        Binary(state, spv::OpULessThan, TypeBool(state), component, count)),
	           component, ConstantU32(state, 0));
	const auto pair =
	    Select(state, type,
	           Binary(state, spv::OpULessThan, TypeBool(state), selected, ConstantU32(state, 2)),
	           word_of(row, 1u), word_of(row, 2u));
	const auto field = state.builder.AllocateId();
	state.builder.AddFunction(
	    spv::OpBitFieldUExtract, type, field, pair,
	    Binary(state, spv::OpShiftLeftLogical, type,
	           Binary(state, spv::OpBitwiseAnd, type, selected, ConstantU32(state, 1)),
	           ConstantU32(state, 4)),
	    ConstantU32(state, 16));
	const auto offset         = bits_of(field, 0u, 5u);
	const auto bits           = bits_of(field, 5u, 6u);
	const auto kind           = bits_of(field, 11u, 5u);
	const auto unsigned_value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitFieldUExtract, type, unsigned_value, raw, offset, bits);
	const auto signed_value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitFieldSExtract, TypeI32(state), signed_value,
	                          Unary(state, spv::OpBitcast, TypeI32(state), raw), offset, bits);
	const auto extracted = Select(state, type, is_signed,
	                              Unary(state, spv::OpBitcast, type, signed_value), unsigned_value);
	// Every conversion is computed and the kind's picked: no branch, and no constant per format.
	const auto kind_pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain,
	                          TypePointer(state, spv::StorageClassPrivate, kind_type), kind_pointer,
	                          kind_table, kind);
	const auto kind_words = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, kind_type, kind_words, kind_pointer);
	const auto conversion = word_of(kind_words, 0u);
	const auto scaled =
	    Binary(state, spv::OpFMul, TypeF32(state),
	           Select(state, TypeF32(state), is_signed,
	                  Unary(state, spv::OpConvertSToF, TypeF32(state), signed_value),
	                  Unary(state, spv::OpConvertUToF, TypeF32(state), unsigned_value)),
	           Unary(state, spv::OpBitcast, TypeF32(state), word_of(kind_words, 1u)));
	const auto floored = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32(state), floored, GlslStd450(state),
	                          GLSLstd450FMax, scaled, ConstantF32Value(state, -1.0f));
	const auto widened = [&](uint32_t float_bits) {
		Format::BufferFormatInfo info {};
		info.type              = Format::ComponentType::Float;
		info.component_count   = 1u;
		info.component_bits[0] = float_bits;
		return NormalizeFormatComponent(state, info, 0u, extracted);
	};
	auto       converted = extracted;
	const auto pick      = [&](IndirectFormatConversion picked, uint32_t value) {
		converted = Select(state, type,
		                   Binary(state, spv::OpIEqual, TypeBool(state), conversion,
		                          ConstantU32(state, static_cast<uint32_t>(picked))),
		                   value, converted);
	};
	pick(IndirectFormatConversion::Factor, Unary(state, spv::OpBitcast, type, scaled));
	pick(IndirectFormatConversion::FactorFloor, Unary(state, spv::OpBitcast, type, floored));
	pick(IndirectFormatConversion::Half, widened(16u));
	pick(IndirectFormatConversion::Float11, widened(11u));
	pick(IndirectFormatConversion::Float10, widened(10u));
	const auto value = Select(
	    state, type,
	    Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), selector, ConstantU32(state, 4)),
	    converted, constant);
	const auto result = Select(state, type, flag(head, 14u), ConstantU32(state, 0), value);
	state.builder.AddFunction(spv::OpReturnValue, result);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace

uint32_t GetBdaPointer(EmitterState& state, uint32_t address) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeU64(state), result,
	                          state.bda_pointer_function, address);
	return result;
}

void DefineIndirectBufferLoads(ValueEmitContext& ctx) {
	auto& state = ctx.state;
	for (const auto* block: state.program.blocks) {
		for (const auto& inst: *block) {
			const auto components = IR::BufferComponentCount(inst.GetOpcode());
			if (components == 0u || inst.GetOpcode() == IR::ValueOpcode::ReadConstBuffer) continue;
			const auto& memory = ctx.Memory(inst);
			if (memory.kind != IR::ResourceKind::IndirectBuffer ||
			    !memory.SupportsIndirectBufferLoad(inst.GetOpcode())) continue;
			auto [entry, inserted] = state.indirect_buffer_load_functions.try_emplace(
			    std::array {memory.data_bits, components, uint32_t(memory.coherent),
			                uint32_t(memory.formatted)}, 0u);
			if (!inserted) continue;
			if (memory.formatted && state.indirect_format_function == 0u) {
				DefineIndirectBufferFormats(ctx);
			}
			const auto function = entry->second = state.builder.AllocateId();
			const auto type = TypeU32(state);
			const auto result_type = TypeU32Composite(state, components);
			const auto signature = state.builder.Type(
			    spv::OpTypeFunction, result_type, type, type, type, type, type, type, type, type);
			const auto name = "load_indirect_buffer_" + std::to_string(memory.data_bits) + "x" +
			                  std::to_string(components) + (memory.coherent ? "_coherent" : "") +
			                  (memory.formatted ? "_formatted" : "");
			state.builder.AddName(function, name.c_str());
			state.builder.AddFunction(spv::OpFunction, result_type, function,
			                          spv::FunctionControlMaskNone, signature);
			std::array<uint32_t, 8> args;
			for (auto& arg: args) {
				arg = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpFunctionParameter, type, arg);
			}
			EmitLabel(state, state.builder.AllocateId());
			const auto buffer = PrepareIndirectBuffer(state, args, memory.formatted);
			uint32_t   result = 0u;
			if (!memory.formatted) {
				result =
				    LoadIndirectBuffer(ctx, buffer, memory.data_bits, components, memory.coherent);
			} else if (components == 1u) {
				result = LoadIndirectFormattedComponent(ctx, buffer, buffer.selector);
			} else {
				// Each component reads the channel its DST_SEL names from the same element.
				std::array<uint32_t, 4> values {};
				for (uint32_t component = 0; component < components; ++component)
					values[component] = LoadIndirectFormattedComponent(
					    ctx, buffer,
					    EmitBitFieldUExtract(state, buffer.dst_sel,
					                         ConstantU32(state, component * 3u),
					                         ConstantU32(state, 3u)));
				result = ConstructU32Composite(state, components, values);
			}
			state.builder.AddFunction(spv::OpReturnValue, result);
			state.builder.AddFunction(spv::OpFunctionEnd);
		}
	}
}

namespace {

struct BdaPage {
	uint32_t index    = 0;
	uint32_t in_space = 0;
};

BdaPage BdaPageOf(EmitterState& state, uint32_t address) {
	const auto type     = TypeU64(state);
	const auto extended = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), address,
	                             ConstantU64(state, LibKernel::Memory::kExtendedMemoryBase));
	const auto packed   = Select(
	    state, type, extended,
	    Binary(state, spv::OpISub, type, address,
	           ConstantU64(state, LibKernel::Memory::kExtendedMemoryBase - LOWER_ADDRESS_SIZE)),
	    address);
	const auto page = Binary(state, spv::OpShiftRightLogical, type, packed,
	                         ConstantU32(state, BufferCache::CACHING_PAGEBITS));
	return {Unary(state, spv::OpUConvert, TypeU32(state), page),
	        Binary(state, spv::OpULessThan, TypeBool(state), page,
	               ConstantU64(state, BufferCache::CACHING_NUMPAGES))};
}

uint32_t LoadPageEntry(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state, 64),
	                          pointer, state.bda_pagetable_variable, ConstantU32(state, 0), index);
	const auto entry = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU64(state), entry, pointer);
	return entry;
}

// Where a page reads from, and the bit that tells the host: its fault bit or its region's use bit.
struct BdaMapping {
	uint32_t host    = 0;
	uint32_t pointer = 0;
	uint32_t bit     = 0;
	uint32_t untold  = 0;
};

BdaMapping MapBdaPage(EmitterState& state, BdaPage page, uint32_t zeros) {
	const auto u32 = TypeU32(state);
	const auto entry =
	    LoadPageEntry(state, Select(state, u32, page.in_space, page.index,
	                                ConstantU32(state, BufferCache::BDA_ZERO_ENTRY)));
	const auto backed =
	    Binary(state, spv::OpINotEqual, TypeBool(state), entry, ConstantU64(state, 0));
	const auto region =
	    Binary(state, spv::OpShiftRightLogical, u32, page.index,
	           ConstantU32(state, BufferCache::BDA_USE_REGIONBITS - BufferCache::CACHING_PAGEBITS));
	const auto unit = Select(state, u32, backed, region, page.index);
	const auto word =
	    Binary(state, spv::OpIAdd, u32,
	           Binary(state, spv::OpShiftRightLogical, u32, unit, ConstantU32(state, 5)),
	           Select(state, u32, backed,
	                  ConstantU32(state, BufferCache::FAULT_BITMAP_SIZE / sizeof(uint32_t)),
	                  ConstantU32(state, 0)));
	BdaMapping mapping;
	mapping.host = Select(state, TypeU64(state), backed, entry, zeros);
	mapping.pointer =
	    FaultElementPointer(state, Select(state, u32, page.in_space, word, ConstantU32(state, 0)));
	mapping.bit =
	    Select(state, u32, page.in_space,
	           Binary(state, spv::OpShiftLeftLogical, u32, ConstantU32(state, 1),
	                  Binary(state, spv::OpBitwiseAnd, u32, unit, ConstantU32(state, 31))),
	           ConstantU32(state, 0));
	const auto told = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, u32, told, mapping.pointer);
	mapping.untold = AndCondition(state, page.in_space,
	                              Binary(state, spv::OpIEqual, TypeBool(state),
	                                     Binary(state, spv::OpBitwiseAnd, u32, told, mapping.bit),
	                                     ConstantU32(state, 0)));
	return mapping;
}

void TellHost(EmitterState& state, const BdaMapping& mapping) {
	state.builder.AddFunction(spv::OpAtomicOr, TypeU32(state), state.builder.AllocateId(),
	                          mapping.pointer, ConstantU32(state, spv::ScopeDevice),
	                          ConstantU32(state, spv::MemorySemanticsMaskNone), mapping.bit);
}

uint32_t PageOffset64(EmitterState& state, uint32_t address) {
	return Binary(state, spv::OpBitwiseAnd, TypeU64(state), address,
	              ConstantU64(state, BufferCache::CACHING_PAGESIZE - 1));
}

// The host address of a guest DWORD.
void DefineBdaDword(EmitterState& state) {
	const auto type          = TypeU64(state);
	state.bda_dword_function = state.builder.AllocateId();
	const auto address       = state.builder.AllocateId();
	state.builder.AddName(state.bda_dword_function, "get_bda_dword");
	state.builder.AddFunction(spv::OpFunction, type, state.bda_dword_function,
	                          spv::FunctionControlMaskNone,
	                          state.builder.Type(spv::OpTypeFunction, type, type));
	state.builder.AddFunction(spv::OpFunctionParameter, type, address);
	EmitLabel(state, state.builder.AllocateId());
	const auto mapping =
	    MapBdaPage(state, BdaPageOf(state, address),
	               LoadPageEntry(state, ConstantU32(state, BufferCache::BDA_ZERO_ENTRY)));
	EmitIfCondition(state, mapping.untold, [&]() { TellHost(state, mapping); });
	state.builder.AddFunction(spv::OpReturnValue, Binary(state, spv::OpIAdd, type, mapping.host,
	                                                     PageOffset64(state, address)));
	state.builder.AddFunction(spv::OpFunctionEnd);
}

// The host addresses of a run's first DWORD in the mapping of its page and of its last DWORD's.
void DefineBdaRun(EmitterState& state) {
	const auto type        = TypeU64(state);
	const auto u32         = TypeU32(state);
	const auto pair        = TypeU64Pair(state);
	state.bda_run_function = state.builder.AllocateId();
	const auto address     = state.builder.AllocateId();
	const auto length      = state.builder.AllocateId();
	state.builder.AddName(state.bda_run_function, "get_bda_run");
	state.builder.AddFunction(spv::OpFunction, pair, state.bda_run_function,
	                          spv::FunctionControlMaskNone,
	                          state.builder.Type(spv::OpTypeFunction, pair, type, u32));
	state.builder.AddFunction(spv::OpFunctionParameter, type, address);
	state.builder.AddFunction(spv::OpFunctionParameter, u32, length);
	EmitLabel(state, state.builder.AllocateId());
	const auto last = Binary(
	    state, spv::OpIAdd, type, address,
	    Unary(state, spv::OpUConvert, type,
	          Binary(state, spv::OpISub, u32, length, ConstantU32(state, sizeof(uint32_t)))));
	const auto zeros  = LoadPageEntry(state, ConstantU32(state, BufferCache::BDA_ZERO_ENTRY));
	const auto first  = MapBdaPage(state, BdaPageOf(state, address), zeros);
	const auto second = MapBdaPage(state, BdaPageOf(state, last), zeros);
	EmitIfCondition(state,
	                Binary(state, spv::OpLogicalOr, TypeBool(state), first.untold, second.untold),
	                [&]() {
		                TellHost(state, first);
		                TellHost(state, second);
	                });
	const auto offset = PageOffset64(state, address);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, pair, result,
	                          Binary(state, spv::OpIAdd, type, first.host, offset),
	                          Binary(state, spv::OpISub, type,
	                                 Binary(state, spv::OpIAdd, type, second.host, offset),
	                                 ConstantU64(state, BufferCache::CACHING_PAGESIZE)));
	state.builder.AddFunction(spv::OpReturnValue, result);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

// A run: scalar loads that follow each other from one pointer and offset value, within 64 bytes.
void PlanBdaRuns(EmitterState& state) {
	constexpr int64_t run_bytes = 64;
	for (const auto* block: state.program.blocks) {
		const IR::Inst* first  = nullptr;
		int64_t         origin = 0;
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != IR::ValueOpcode::LoadAddressU32) {
				first = nullptr;
				continue;
			}
			const auto& mem = state.program.memory_info.at(inst.Flags<IR::MemoryFlags>().index);
			if (mem.kind != IR::ResourceKind::ScalarAddress) {
				first = nullptr;
				continue;
			}
			if (mem.planning_only) {
				continue;
			}
			const auto offset = int64_t {static_cast<int32_t>(mem.offset & ~3u)};
			const bool joins  = first != nullptr && !mem.address_is_full && offset >= origin &&
			                    offset - origin <= run_bytes - 4 &&
			                    inst.Arg(0).Resolve() == first->Arg(0).Resolve() &&
			                    inst.Arg(1).Resolve() == first->Arg(1).Resolve();
			if (!joins) {
				// A full address names no pointer another load could share.
				first  = mem.address_is_full ? nullptr : &inst;
				origin = offset;
				state.bda_runs.insert_or_assign(&inst, BdaRun {&inst, 0, sizeof(uint32_t)});
				continue;
			}
			const auto distance = static_cast<uint32_t>(offset - origin);
			state.bda_runs.insert_or_assign(&inst, BdaRun {first, distance, 0});
			auto& length = state.bda_runs.at(first).length;
			length       = std::max<uint32_t>(length, distance + sizeof(uint32_t));
		}
	}
}

} // namespace

void DefineGetBdaPointer(EmitterState& state) {
	if (!state.program.info.uses_dma) {
		return;
	}
	if (!state.program.info.bvh && !state.program.info.bvh8) {
		PlanBdaRuns(state);
		DefineBdaDword(state);
		if (std::ranges::any_of(state.bda_runs, [](const auto& run) {
			    return run.second.length > sizeof(uint32_t);
		    })) {
			DefineBdaRun(state);
		}
		return;
	}
	const auto type            = TypeU64(state);
	const auto function_type   = state.builder.Type(spv::OpTypeFunction, type, type);
	state.bda_pointer_function = state.builder.AllocateId();
	const auto address         = state.builder.AllocateId();
	const auto entry_label     = state.builder.AllocateId();
	state.builder.AddName(state.bda_pointer_function, "get_bda_pointer");
	state.builder.AddFunction(spv::OpFunction, type, state.bda_pointer_function,
	                          spv::FunctionControlMaskNone, function_type);
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
	                                  ConstantU32(state, BufferCache::CACHING_PAGEBITS));
	// An address past the guest's GPU space has no page entry: it is never mapped, so it reads as
	// missing without touching the page table or recording a fault the host could never back.
	const auto in_space = Binary(state, spv::OpULessThan, TypeBool(state), page64,
	                             ConstantU64(state, BufferCache::CACHING_NUMPAGES));
	const auto page =
	    Select(state, TypeU32(state), in_space,
	           Unary(state, spv::OpUConvert, TypeU32(state), page64), ConstantU32(state, 0));
	const auto entry_pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state, 64),
	                          entry_pointer, state.bda_pagetable_variable, ConstantU32(state, 0),
	                          page);
	const auto loaded = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, loaded, entry_pointer);
	const auto entry = Select(state, type, in_space, loaded, ConstantU64(state, 0));
	const auto unbacked =
	    Binary(state, spv::OpIEqual, TypeBool(state), entry, ConstantU64(state, 0));
	const auto fault_label = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, unbacked, fault_label, merge_label);

	EmitLabel(state, fault_label);
	const auto record_label   = state.builder.AllocateId();
	const auto recorded_label = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelectionMerge, recorded_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, in_space, record_label, recorded_label);
	EmitLabel(state, record_label);
	RecordBdaFault(state, page);
	state.builder.AddFunction(spv::OpBranch, recorded_label);
	EmitLabel(state, recorded_label);
	state.builder.AddFunction(spv::OpReturnValue, ConstantU64(state, 0));

	EmitLabel(state, merge_label);
	const auto offset = Binary(state, spv::OpBitwiseAnd, type, address,
	                           ConstantU64(state, BufferCache::CACHING_PAGESIZE - 1));
	const auto pointer = Binary(state, spv::OpIAdd, type, entry, offset);
	// The buffer GC sees BDA-only use through these marks.
	const auto region =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), page,
	           ConstantU32(state, BufferCache::BDA_USE_REGIONBITS - BufferCache::CACHING_PAGEBITS));
	const auto use_pointer = FaultElementPointer(
	    state, Binary(state, spv::OpIAdd, TypeU32(state),
	                  Binary(state, spv::OpShiftRightLogical, TypeU32(state), region,
	                         ConstantU32(state, 5)),
	                  ConstantU32(state, BufferCache::FAULT_BITMAP_SIZE / sizeof(uint32_t))));
	const auto use_bit =
	    Binary(state, spv::OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
	           Binary(state, spv::OpBitwiseAnd, TypeU32(state), region, ConstantU32(state, 31)));
	const auto use_word = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), use_word, use_pointer);
	const auto unmarked = Binary(
	    state, spv::OpIEqual, TypeBool(state),
	    Binary(state, spv::OpBitwiseAnd, TypeU32(state), use_word, use_bit), ConstantU32(state, 0));
	const auto mark_label   = state.builder.AllocateId();
	const auto marked_label = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelectionMerge, marked_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, unmarked, mark_label, marked_label);
	EmitLabel(state, mark_label);
	state.builder.AddFunction(spv::OpAtomicOr, TypeU32(state), state.builder.AllocateId(),
	                          use_pointer, ConstantU32(state, spv::ScopeDevice),
	                          ConstantU32(state, spv::MemorySemanticsMaskNone), use_bit);
	state.builder.AddFunction(spv::OpBranch, marked_label);
	EmitLabel(state, marked_label);
	state.builder.AddFunction(spv::OpReturnValue, pointer);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

uint32_t EmitAtomic32(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem = ctx.Memory(inst);
	return EmitAtomicAccess(ctx, inst, mem, [&](uint32_t pointer) {
		const auto scope =
		    mem.kind == IR::ResourceKind::Lds ? spv::ScopeWorkgroup : spv::ScopeDevice;
		const auto old = EmitAtomicOperation(ctx, inst, pointer, scope);
		EmitAtomicMemoryBarrier(ctx.state, mem.kind);
		return old;
	});
}

uint32_t EmitBufferAtomic64(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem   = ctx.Memory(inst);
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
	const auto address = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
	                            ByteAddress(ctx, inst, mem), ConstantU32(state, 0xfff8u));
	const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
	                          ConstantU32(state, 3u));
	const auto in_bounds = Binary(state, spv::OpULessThan, TypeBool(state), index,
	                              ConstantU32(state, LdsDwordCount(state) / 2u));
	const auto active = EmitLogicalAndBool(state, ctx.Arg(inst, inst.NumArgs() - 1), in_bounds);
	const auto emit = [&]() {
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
		const auto old = state.builder.AllocateId();
		const auto opcode = SpirvAtomicOpcode(inst.GetOpcode());
		const auto scope = ConstantU32(state, spv::ScopeWorkgroup);
		const auto acquire_release =
		    ConstantU32(state, spv::MemorySemanticsAcquireReleaseMask | semantics);
		if (opcode == spv::OpAtomicCompareExchange) {
			// RDNA2 DS compares DATA and stores DATA2, opposite to BUFFER atomics.
			state.builder.AddFunction(
			    opcode, TypeU64(state), old, pointer, scope, acquire_release,
			    ConstantU32(state, spv::MemorySemanticsAcquireMask | semantics),
			    ctx.Arg(inst, 2), ctx.Arg(inst, 1));
		} else {
			state.builder.AddFunction(opcode, TypeU64(state), old, pointer, scope,
			                          acquire_release, ctx.Arg(inst, 1));
		}
		return old;
	};
	if (inst.HasUses()) {
		ctx.Define(inst, EmitValueOrDefaultIfCondition(
		    state, active, TypeU64(state), ConstantU64(state, 0), emit));
	} else {
		EmitIfCondition(state, active, emit);
	}
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
	// Elect and count from the guest EXEC words, which V_MBCNT also ranks lanes by.
	const auto low   = ctx.Arg(inst, 1);
	const auto high  = ctx.Arg(inst, 2);
	const auto count = Binary(state, spv::OpIAdd, TypeU32(state),
	                          Unary(state, spv::OpBitCount, TypeU32(state), low),
	                          Unary(state, spv::OpBitCount, TypeU32(state), high));
	const auto mask  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 4), mask, low, high,
	                          ConstantU32(state, 0), ConstantU32(state, 0));
	const auto first = ctx.FirstLane(mask);
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
	                 Binary(state, spv::OpINotEqual, TypeBool(state), count, ConstantU32(state, 0)),
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

void EmitReadConstBuffer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto mem = ctx.Memory(inst);
	if (mem.planning_only) return;
	if (mem.kind == IR::ResourceKind::IndirectBuffer) {
		ctx.Define(inst, LoadIndirectScalarBuffer(ctx, inst));
		return;
	}
	auto& state        = ctx.state;
	mem.kind           = IR::ResourceKind::ScalarBuffer;
	const auto offset = inst.Arg(1).Resolve();
	auto       index  = offset.IsImmediate()
	                        ? ConstantU32(state, (offset.U32() >> 2u) + (mem.offset >> 2u))
	                        : Binary(state, spv::OpShiftRightLogical, TypeU32(state),
	                                 ctx.Arg(inst, 1), ConstantU32(state, 2));
	if (!offset.IsImmediate() && mem.offset >= 4u) {
		index = Binary(state, spv::OpIAdd, TypeU32(state), index, ConstantU32(state, mem.offset >> 2u));
	}
	const auto access    = PrepareMemoryResourceAccess(state, mem);
	// Scalar buffer addressing aligns the base and each offset independently.
	auto element = Binary(
	    state, spv::OpIAdd, TypeU32(state), index,
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), access.byte_offset, ConstantU32(state, 2)));
	// The device computes byte offsets in 32 bits, so elements from 2^30 on would wrap back into
	// the range; the host prefix adds at most 63.
	constexpr uint32_t last_element = UINT32_MAX >> 2u;
	if (state.profile.robust_buffer_access &&
	    (!offset.IsImmediate() || (offset.U32() >> 2u) + (mem.offset >> 2u) > last_element - 63u)) {
		element = EmitGlsl<GLSLstd450UMin, IR::Type::U32>(state, element,
		                                                  ConstantU32(state, last_element));
	}
	const auto condition = EmitStorageElementInBounds(state, access, element);
	ctx.Define(inst, EmitValueOrZeroIfCondition(state, condition, [&]() {
		           return EmitNative<spv::OpLoad, IR::Type::U32>(
		               state, EmitMemoryElementPointer(state, access, element));
	           }));
}

uint32_t EmitLoadDeviceAddressU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	return EmitValueOrZeroIfCondition(state, ctx.Arg(inst, 3), [&]() {
		const auto address = Binary(
		    state, spv::OpIAdd, TypeU64(state), PackU64(state, ctx.Arg(inst, 0), ctx.Arg(inst, 1)),
		    Unary(state, spv::OpUConvert, TypeU64(state), ctx.Arg(inst, 2)));
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(
		    spv::OpLoad, TypeU32(state), value,
		    Unary(state, spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), address),
		    spv::MemoryAccessAlignedMask, 4u);
		return value;
	});
}

uint32_t EmitGetBufferResourceIndex(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& buffer = ctx.state.program.info.buffers.at(inst.Flags<uint32_t>());
	if (buffer.indirect_resources.empty()) return ConstantU32(ctx.state, 0u);
	return EmitIndirectResourceIndex(ctx.state, ctx.Arg(inst, 0), buffer.indirect_mapping_slot,
	                                 buffer.indirect_searched, 0u);
}

void EmitLoadMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto  op  = inst.GetOpcode();
	const auto& mem = ctx.Memory(inst);
	if (op == IR::ValueOpcode::LoadAddressU32 && mem.planning_only) return;
	const auto buffer_components = IR::BufferComponentCount(op);
	const auto shared_components = IR::SharedComponentCount(op);
	const auto address_info      = IR::AddressOpcodeInfoOf(op);
	uint32_t   value;
	if (mem.kind == IR::ResourceKind::FlatLocal)
		value = LoadLocalFlat(ctx, inst);
	else if (buffer_components > 1u ||
	         (buffer_components == 1u && mem.kind == IR::ResourceKind::IndirectBuffer) ||
	         (op == IR::ValueOpcode::LoadBufferU32 && mem.formatted))
		value = LoadBuffer(ctx, inst, buffer_components);
	else if (shared_components > 1u)
		value = LoadWideShared(ctx, inst, shared_components);
	else if (mem.kind == IR::ResourceKind::ScalarAddress)
		value = LoadScalarAddress(ctx, inst, mem);
	else if (address_info.access == IR::AddressAccess::Read &&
	         mem.kind != IR::ResourceKind::Scratch)
		value = LoadBda(ctx, GuestAddress(ctx, inst, mem), ctx.Arg(inst, inst.NumArgs() - 1),
		                address_info.data_bits, mem.coherent);
	else if (inst.GetType() == IR::Type::U8)
		value = LoadSubword(ctx, inst, mem, 8);
	else if (inst.GetType() == IR::Type::U16)
		value = LoadSubword(ctx, inst, mem, 16);
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
	if (mem.kind == IR::ResourceKind::FlatLocal)
		StoreLocalFlat(ctx, inst);
	else if (buffer_components > 1u)
		StoreBuffer(ctx, inst, buffer_components);
	else if (shared_components > 1u)
		StoreWideShared(ctx, inst, shared_components);
	else if (op == IR::ValueOpcode::StoreBufferU32 && mem.formatted)
		StoreBuffer(ctx, inst, 1u);
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

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
