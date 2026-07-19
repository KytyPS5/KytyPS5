#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t EmitDppWriteActiveBool(EmitterState* state, const IR::Operand& dst) {
	const auto exec_active = EmitExecActiveBool(state);
	if (!dst.dpp) {
		return exec_active;
	}

	const auto subid       = EmitSubgroupLocalInvocationId(state);
	const auto bank_shift  = state->builder.AllocateId();
	const auto row_shift   = state->builder.AllocateId();
	const auto bank        = state->builder.AllocateId();
	const auto row         = state->builder.AllocateId();
	const auto bank_bit    = state->builder.AllocateId();
	const auto row_bit     = state->builder.AllocateId();
	const auto bank_hit    = state->builder.AllocateId();
	const auto row_hit     = state->builder.AllocateId();
	const auto bank_active = state->builder.AllocateId();
	const auto row_active  = state->builder.AllocateId();
	const auto dpp_active  = state->builder.AllocateId();
	const auto ret         = state->builder.AllocateId();

	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, bank_shift, subid, ConstantU32(state, 2)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, bank, bank_shift, ConstantU32(state, 3)});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, row_shift, subid, ConstantU32(state, 4)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, row, row_shift, ConstantU32(state, 3)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, bank_bit, ConstantU32(state, 1), bank});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, row_bit, ConstantU32(state, 1), row});
	state->builder.AddFunction({OpBitwiseAnd, state->uint_type, bank_hit,
	                            ConstantU32(state, dst.dpp_bank_mask), bank_bit});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, row_hit, ConstantU32(state, dst.dpp_row_mask), row_bit});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, bank_active, bank_hit, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpINotEqual, state->bool_type, row_active, row_hit, ConstantU32(state, 0)});
	state->builder.AddFunction(
	    {OpLogicalAnd, state->bool_type, dpp_active, bank_active, row_active});
	uint32_t write_active = dpp_active;
	if (!dst.dpp_bound_ctrl) {
		const auto target  = EmitDppTargetLane(state, dst.dpp_ctrl);
		const auto bounded = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpLogicalAnd, state->bool_type, bounded, write_active, target.valid});
		write_active = bounded;
	}
	state->builder.AddFunction({OpLogicalAnd, state->bool_type, ret, exec_active, write_active});
	return ret;
}

uint32_t EmitSdwaDestinationMerge(EmitterState* state, uint32_t pointer, const IR::Operand& dst,
                                  uint32_t value) {
	if (dst.sdwa_sel == 6u) {
		return value;
	}

	uint32_t offset = 0;
	uint32_t width  = 0;
	if (!SdwaSelectorOffsetWidth(dst.sdwa_sel, offset, width) || width == 32u) {
		return value;
	}

	const auto value_mask = width == 16u ? 0x0000ffffu : 0x000000ffu;
	const auto field_mask = value_mask << offset;
	const auto part       = state->builder.AllocateId();
	const auto shifted    = offset != 0u ? state->builder.AllocateId() : part;
	const auto old_value  = state->builder.AllocateId();
	const auto kept       = state->builder.AllocateId();
	const auto merged     = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, part, value, ConstantU32(state, value_mask)});
	if (offset != 0u) {
		state->builder.AddFunction(
		    {OpShiftLeftLogical, state->uint_type, shifted, part, ConstantU32(state, offset)});
	}
	state->builder.AddFunction({OpLoad, state->uint_type, old_value, pointer});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, kept, old_value, ConstantU32(state, ~field_mask)});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, merged, kept, shifted});
	return merged;
}

void EmitStoreU32(EmitterState* state, const IR::Operand& dst, uint32_t value) {
	if (dst.kind != IR::OperandKind::Register) {
		return;
	}
	const auto pointer = PointerForRegister(*state, dst.reg);
	if (pointer != 0) {
		const auto store_value = EmitSdwaDestinationMerge(state, pointer, dst, value);
		const auto wave_value =
		    IsInactiveWave32ExecHigh(*state, dst.reg) ? ConstantU32(state, 0) : store_value;
		if (dst.reg.file == IR::RegisterFile::Vector) {
			const auto old_value = state->builder.AllocateId();
			const auto selected  = state->builder.AllocateId();
			state->builder.AddFunction({OpLoad, state->uint_type, old_value, pointer});
			state->builder.AddFunction({OpSelect, state->uint_type, selected,
			                            EmitDppWriteActiveBool(state, dst), wave_value, old_value});
			state->builder.AddFunction({OpStore, pointer, selected});
			return;
		}
		state->builder.AddFunction({OpStore, pointer, wave_value});
	}
}

uint32_t EmitAddU32(EmitterState* state, uint32_t lhs, uint32_t rhs) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpIAdd, state->uint_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitBinaryU32(EmitterState* state, uint32_t opcode, uint32_t lhs, uint32_t rhs) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({opcode, state->uint_type, ret, lhs, rhs});
	return ret;
}

uint32_t EmitNotEqualZeroBool(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpINotEqual, state->bool_type, ret, value, ConstantU32(state, 0)});
	return ret;
}

uint32_t EmitSelectU32Value(EmitterState* state, uint32_t condition, uint32_t true_value,
                            uint32_t false_value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, ret, condition, true_value, false_value});
	return ret;
}

uint32_t EmitByteAddress(EmitterState* state, const IR::Instruction& inst, uint32_t first_src,
                         uint32_t src_count) {
	uint32_t address = ConstantU32(state, inst.memory.offset);
	for (uint32_t i = 0; i < src_count; i++) {
		address = EmitAddU32(state, address, EmitValueLoad(state, inst.src[first_src + i]));
	}
	return address;
}

uint32_t StorageBufferPackedStride(const EmitterState& state, const IR::MemoryInfo& mem,
                                   uint32_t use_pc) {
	(void)use_pc;
	if (mem.resource >= state.program->info.buffers.size()) {
		ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "buffer specialization is missing");
	}
	return state.program->info.buffers[mem.resource].packed_stride;
}

uint32_t StorageBufferFormat(const EmitterState& state, const IR::MemoryInfo& mem,
                             uint32_t use_pc) {
	(void)use_pc;
	if (mem.resource >= state.program->info.buffers.size()) {
		ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "buffer specialization is missing");
	}
	return state.program->info.buffers[mem.resource].descriptor_format;
}

bool ShouldApplyBufferAddTid(const IR::Instruction& inst) {
	return inst.memory.kind == IR::ResourceKind::Buffer && inst.memory.formatted &&
	       !inst.memory.typed && inst.op == IR::Opcode::BufferStoreDword;
}

uint32_t EmitBufferIndexWithAddTid(EmitterState* state, const IR::Instruction& inst, uint32_t index,
                                   uint32_t packed) {
	if (!ShouldApplyBufferAddTid(inst)) {
		return index;
	}

	const auto add_tid_bit =
	    EmitBinaryU32(state, OpBitwiseAnd,
	                  EmitBinaryU32(state, OpShiftRightLogical, packed, ConstantU32(state, 20)),
	                  ConstantU32(state, 1));
	const auto add_tid = EmitNotEqualZeroBool(state, add_tid_bit);
	const auto lane =
	    EmitBinaryU32(state, OpBitwiseAnd, EmitLocalInvocationIndex(state), ConstantU32(state, 63));
	const auto tid_index = EmitAddU32(state, index, lane);
	return EmitSelectU32Value(state, add_tid, tid_index, index);
}

uint32_t EmitOptionalLogicalAndBool(EmitterState* state, uint32_t lhs, uint32_t rhs) {
	if (lhs == 0) {
		return rhs;
	}
	if (rhs == 0) {
		return lhs;
	}
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpLogicalAnd, state->bool_type, ret, lhs, rhs});
	return ret;
}

bool IsStorageBufferMemoryKind(IR::ResourceKind kind) {
	switch (kind) {
		case IR::ResourceKind::ScalarBuffer:
		case IR::ResourceKind::Buffer:
		case IR::ResourceKind::Flat:
		case IR::ResourceKind::Global:
		case IR::ResourceKind::Scratch: return true;
		default: return false;
	}
}

uint32_t EmitBufferAddressFromParts(EmitterState* state, const IR::Instruction& inst,
                                    uint32_t index, uint32_t offset, uint32_t soffset) {
	const auto& mem           = inst.memory;
	const auto  packed        = ConstantU32(state, StorageBufferPackedStride(*state, mem, inst.pc));
	const auto  address_index = EmitBufferIndexWithAddTid(state, inst, index, packed);
	const auto  stride = EmitBinaryU32(state, OpBitwiseAnd, packed, ConstantU32(state, 0x3fffu));
	const auto  swizzle_bit =
	    EmitBinaryU32(state, OpBitwiseAnd,
	                  EmitBinaryU32(state, OpShiftRightLogical, packed, ConstantU32(state, 14)),
	                  ConstantU32(state, 1));
	const auto stride_nonzero  = EmitNotEqualZeroBool(state, stride);
	const auto swizzle_nonzero = EmitNotEqualZeroBool(state, swizzle_bit);
	const auto use_swizzle     = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpLogicalAnd, state->bool_type, use_swizzle, swizzle_nonzero, stride_nonzero});

	const auto index_stride_enum =
	    EmitBinaryU32(state, OpBitwiseAnd,
	                  EmitBinaryU32(state, OpShiftRightLogical, packed, ConstantU32(state, 16)),
	                  ConstantU32(state, 3));
	const auto index_shift = EmitAddU32(state, index_stride_enum, ConstantU32(state, 3));
	const auto index_stride =
	    EmitBinaryU32(state, OpShiftLeftLogical, ConstantU32(state, 8), index_stride_enum);
	const auto index_stride_mask =
	    EmitBinaryU32(state, OpISub, index_stride, ConstantU32(state, 1));

	const auto linear_index = EmitBinaryU32(state, OpIMul, address_index, stride);
	const auto linear       = EmitAddU32(state, linear_index, offset);

	const auto index_msb = EmitBinaryU32(state, OpShiftRightLogical, address_index, index_shift);
	const auto index_lsb = EmitBinaryU32(state, OpBitwiseAnd, address_index, index_stride_mask);
	const auto offset_msb_bytes =
	    EmitBinaryU32(state, OpBitwiseAnd, offset, ConstantU32(state, ~3u));
	const auto offset_lsb = EmitBinaryU32(state, OpBitwiseAnd, offset, ConstantU32(state, 3));
	const auto index_part = EmitBinaryU32(state, OpIMul, index_msb, stride);
	const auto msb_sum    = EmitAddU32(state, index_part, offset_msb_bytes);
	const auto msb        = EmitBinaryU32(state, OpIMul, msb_sum, index_stride);
	const auto lsb_index =
	    EmitBinaryU32(state, OpShiftLeftLogical, index_lsb, ConstantU32(state, 2));
	const auto lsb           = EmitAddU32(state, lsb_index, offset_lsb);
	const auto swizzled      = EmitAddU32(state, msb, lsb);
	const auto buffer_offset = EmitSelectU32Value(state, use_swizzle, swizzled, linear);
	return EmitAddU32(state, buffer_offset, soffset);
}

uint32_t EmitBufferByteAddress(EmitterState* state, const IR::Instruction& inst, uint32_t first_src,
                               uint32_t src_count) {
	const auto end               = first_src + src_count;
	auto       cursor            = first_src;
	auto       LoadAddressSource = [&]() {
		if (cursor >= end) {
			return ConstantU32(state, 0);
		}
		return EmitValueLoad(state, inst.src[cursor++]);
	};

	uint32_t index = ConstantU32(state, 0);
	if (inst.memory.idxen) {
		index = LoadAddressSource();
	}
	uint32_t offset = ConstantU32(state, inst.memory.offset);
	if (inst.memory.offen) {
		offset = EmitAddU32(state, offset, LoadAddressSource());
	}
	const auto soffset = LoadAddressSource();
	return EmitBufferAddressFromParts(state, inst, index, offset, soffset);
}

bool IsFlatMemoryKind(IR::ResourceKind kind) {
	return kind == IR::ResourceKind::Flat || kind == IR::ResourceKind::Global ||
	       kind == IR::ResourceKind::Scratch;
}

uint32_t EmitRelativeAddress(EmitterState* state, const IR::Instruction& inst, uint32_t first_src,
                             uint32_t src_count, bool vector_only = true,
                             bool align_components = false) {
	const auto delta     = state->program->info.addresses[inst.memory.resource].specialized_base;
	auto       immediate = static_cast<int32_t>(inst.memory.offset);
	if (align_components) {
		immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
	}
	const auto initial = delta + static_cast<uint64_t>(static_cast<int64_t>(immediate));
	uint32_t   low     = ConstantU32(state, static_cast<uint32_t>(initial));
	uint32_t   high    = ConstantU32(state, static_cast<uint32_t>(initial >> 32u));
	for (uint32_t i = 0; i < src_count; i++) {
		const auto& src = inst.src[first_src + i];
		if (!vector_only ||
		    (src.kind == IR::OperandKind::Register && src.reg.file == IR::RegisterFile::Vector)) {
			auto value = EmitValueLoad(state, src);
			if (align_components) {
				value = EmitBinaryU32(state, OpBitwiseAnd, value, ConstantU32(state, ~3u));
			}
			const auto next  = EmitAddU32(state, low, value);
			const auto carry = state->builder.AllocateId();
			state->builder.AddFunction({OpULessThan, state->bool_type, carry, next, low});
			high = EmitAddU32(
			    state, high,
			    EmitSelectU32Value(state, carry, ConstantU32(state, 1), ConstantU32(state, 0)));
			low = next;
		}
	}
	const auto valid = state->builder.AllocateId();
	state->builder.AddFunction({OpIEqual, state->bool_type, valid, high, ConstantU32(state, 0)});
	return EmitSelectU32Value(state, valid, low, ConstantU32(state, UINT32_MAX));
}

uint32_t EmitFlatVirtualAddress(EmitterState* state, const IR::Instruction& inst,
                                uint32_t first_src, uint32_t src_count) {
	if (state->resources == nullptr || inst.memory.resource >= state->resources->addresses.size() ||
	    src_count < 2) {
		ExitDescriptorBindingFailure(*state, IR::DescriptorBindingKind::AddressMemory,
		                             inst.memory.resource, "flat address snapshot is missing");
	}
	const auto low0       = EmitValueLoad(state, inst.src[first_src]);
	const auto high0      = EmitValueLoad(state, inst.src[first_src + 1u]);
	const auto immediate  = ConstantU32(state, inst.memory.offset);
	const auto low        = EmitAddU32(state, low0, immediate);
	const auto carry_bool = state->builder.AllocateId();
	state->builder.AddFunction({OpULessThan, state->bool_type, carry_bool, low, low0});
	const auto carry =
	    EmitSelectU32Value(state, carry_bool, ConstantU32(state, 1), ConstantU32(state, 0));
	const auto immediate_high =
	    ConstantU32(state, (inst.memory.offset & 0x80000000u) != 0 ? UINT32_MAX : 0u);
	const auto high = EmitAddU32(state, EmitAddU32(state, high0, immediate_high), carry);

	const auto base         = state->program->info.addresses[inst.memory.resource].specialized_base;
	const auto base_low     = static_cast<uint32_t>(base);
	const auto base_high    = static_cast<uint32_t>(base >> 32u);
	const auto relative_low = EmitBinaryU32(state, OpISub, low, ConstantU32(state, base_low));
	const auto borrow_bool  = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpULessThan, state->bool_type, borrow_bool, low, ConstantU32(state, base_low)});
	const auto borrow =
	    EmitSelectU32Value(state, borrow_bool, ConstantU32(state, 1), ConstantU32(state, 0));
	const auto relative_high = EmitBinaryU32(
	    state, OpISub, EmitBinaryU32(state, OpISub, high, ConstantU32(state, base_high)), borrow);
	const auto valid = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpIEqual, state->bool_type, valid, relative_high, ConstantU32(state, 0)});
	return EmitSelectU32Value(state, valid, relative_low, ConstantU32(state, UINT32_MAX));
}

uint32_t EmitMemoryByteAddress(EmitterState* state, const IR::Instruction& inst,
                               const IR::MemoryInfo& mem, uint32_t first_src, uint32_t src_count) {
	if (mem.kind == IR::ResourceKind::Buffer) {
		return EmitBufferByteAddress(state, inst, first_src, src_count);
	}
	if (mem.kind == IR::ResourceKind::Flat ||
	    ((mem.kind == IR::ResourceKind::Global || mem.kind == IR::ResourceKind::Scratch) &&
	     state->program->info.addresses[mem.resource].source == IR::ScalarProvenance::Unknown)) {
		return EmitFlatVirtualAddress(state, inst, first_src, src_count);
	}
	if (mem.kind == IR::ResourceKind::Global || mem.kind == IR::ResourceKind::Scratch) {
		return EmitRelativeAddress(state, inst, first_src, src_count);
	}
	return EmitByteAddress(state, inst, first_src, src_count);
}

uint32_t EmitDwordIndex(EmitterState* state, const IR::Instruction& inst, uint32_t first_src,
                        uint32_t src_count) {
	const auto address = EmitByteAddress(state, inst, first_src, src_count);
	const auto index   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, index, address, ConstantU32(state, 2)});
	return index;
}

uint32_t EmitMemoryDwordIndex(EmitterState* state, const IR::Instruction& inst,
                              const IR::MemoryInfo& mem, uint32_t first_src, uint32_t src_count) {
	const auto address = EmitMemoryByteAddress(state, inst, mem, first_src, src_count);
	const auto index   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, index, address, ConstantU32(state, 2)});
	return index;
}

DescriptorResourceBinding
StorageBufferBindingForMemory(EmitterState* state, const IR::MemoryInfo& mem, uint32_t use_pc) {
	(void)use_pc;
	const auto binding =
	    ResourceForDescriptor(*state, IR::DescriptorBindingKind::Buffers, mem.resource);
	if (state->storage_buffer_variable == 0) {
		ExitDescriptorBindingFailure(*state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "storage buffer descriptor array was not emitted");
	}
	return binding;
}

uint32_t EmitStorageBufferObjectPointer(EmitterState* state, const IR::MemoryInfo& mem,
                                        uint32_t use_pc) {
	if (IsFlatMemoryKind(mem.kind)) {
		if (state->address_memory_variable == 0) {
			ExitDescriptorBindingFailure(*state, IR::DescriptorBindingKind::AddressMemory,
			                             mem.resource, "address memory binding was not emitted");
		}
		const auto binding =
		    ResourceForDescriptor(*state, IR::DescriptorBindingKind::AddressMemory, mem.resource);
		const auto pointer = state->builder.AllocateId();
		state->builder.AddFunction({OpAccessChain, state->ptr_storage_buffer, pointer,
		                            state->address_memory_variable,
		                            ConstantU32(state, binding.array_index)});
		return pointer;
	}
	const auto binding = StorageBufferBindingForMemory(state, mem, use_pc);
	const auto pointer = state->builder.AllocateId();
	state->builder.AddFunction({OpAccessChain, state->ptr_storage_buffer, pointer,
	                            state->storage_buffer_variable,
	                            ConstantU32(state, binding.array_index)});
	return pointer;
}

uint32_t EmitStorageBufferElementInBounds(EmitterState* state, const IR::MemoryInfo& mem,
                                          uint32_t index, uint32_t use_pc) {
	const auto object    = EmitStorageBufferObjectPointer(state, mem, use_pc);
	const auto length    = state->builder.AllocateId();
	const auto in_bounds = state->builder.AllocateId();
	state->builder.AddFunction({OpArrayLength, state->uint_type, length, object, 0});
	state->builder.AddFunction({OpULessThan, state->bool_type, in_bounds, index, length});
	return in_bounds;
}

uint32_t EmitStorageBufferElementPointer(EmitterState* state, const IR::MemoryInfo& mem,
                                         uint32_t index, uint32_t use_pc) {
	if (IsFlatMemoryKind(mem.kind)) {
		if (state->address_memory_variable == 0) {
			ExitDescriptorBindingFailure(*state, IR::DescriptorBindingKind::AddressMemory,
			                             mem.resource, "address memory binding was not emitted");
		}
		const auto binding =
		    ResourceForDescriptor(*state, IR::DescriptorBindingKind::AddressMemory, mem.resource);
		const auto pointer = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpAccessChain, state->ptr_storage_buffer_uint, pointer, state->address_memory_variable,
		     ConstantU32(state, binding.array_index), ConstantU32(state, 0), index});
		return pointer;
	}
	const auto binding = StorageBufferBindingForMemory(state, mem, use_pc);
	const auto pointer = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpAccessChain, state->ptr_storage_buffer_uint, pointer, state->storage_buffer_variable,
	     ConstantU32(state, binding.array_index), ConstantU32(state, 0), index});
	return pointer;
}

uint32_t EmitLdsElementPointer(EmitterState* state, uint32_t index) {
	const auto pointer = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpAccessChain, state->ptr_workgroup_uint, pointer, state->lds_variable, index});
	return pointer;
}

uint32_t EmitLdsElementInBounds(EmitterState* state, uint32_t index) {
	const auto in_bounds = state->builder.AllocateId();
	const auto dwords    = state->needs_function_lds ? 8192u : 1024u;
	state->builder.AddFunction(
	    {OpULessThan, state->bool_type, in_bounds, index, ConstantU32(state, dwords)});
	return in_bounds;
}

uint32_t EmitMemoryElementPointer(EmitterState* state, const IR::MemoryInfo& mem, uint32_t index,
                                  uint32_t use_pc) {
	if (mem.kind == IR::ResourceKind::Lds) {
		return EmitLdsElementPointer(state, index);
	}
	if (mem.kind == IR::ResourceKind::Gds) {
		return EmitGdsElementPointer(state, index);
	}
	return EmitStorageBufferElementPointer(state, mem, index, use_pc);
}

uint32_t EmitMemoryLoadDwordValueU32(EmitterState* state, const IR::Instruction& inst,
                                     IR::ResourceKind kind, uint32_t first_src,
                                     uint32_t src_count) {
	auto mem                   = inst.memory;
	mem.kind                   = kind;
	const auto index           = EmitMemoryDwordIndex(state, inst, mem, first_src, src_count);
	auto       LoadFromPointer = [&](uint32_t pointer) {
		const auto value = state->builder.AllocateId();
		state->builder.AddFunction({OpLoad, state->uint_type, value, pointer});
		return value;
	};
	if (mem.kind == IR::ResourceKind::Lds) {
		return LoadFromPointer(EmitLdsElementPointer(state, index));
	}
	if (mem.kind == IR::ResourceKind::Gds) {
		const auto in_bounds = EmitGdsElementInBounds(state, index);
		return EmitValueOrZeroIfCondition(state, in_bounds, [&]() {
			return LoadFromPointer(EmitGdsElementPointer(state, index));
		});
	}
	if (IsStorageBufferMemoryKind(mem.kind)) {
		const auto in_bounds = EmitStorageBufferElementInBounds(state, mem, index, inst.pc);
		return EmitValueOrZeroIfCondition(state, in_bounds, [&]() {
			return LoadFromPointer(EmitStorageBufferElementPointer(state, mem, index, inst.pc));
		});
	}
	return ConstantU32(state, 0);
}

void EmitMemoryLoadU32(EmitterState* state, const IR::Instruction& inst, IR::ResourceKind kind,
                       uint32_t first_src, uint32_t src_count) {
	const auto value = EmitMemoryLoadDwordValueU32(state, inst, kind, first_src, src_count);
	EmitStoreU32(state, inst.dst, value);
}

void EmitMemoryLoadDwordsU32(EmitterState* state, const IR::Instruction& inst,
                             IR::ResourceKind kind, uint32_t first_src, uint32_t src_count) {
	const auto count = std::max(1u, inst.memory.data_dwords);
	if (count == 1u) {
		EmitMemoryLoadU32(state, inst, kind, first_src, src_count);
		return;
	}

	std::vector<uint32_t> values;
	values.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		auto load_inst = inst;
		load_inst.memory.offset += i * 4u;
		load_inst.memory.data_dwords     = 1u;
		load_inst.memory.component_index = i;
		load_inst.memory.component_count = count;
		values.push_back(EmitMemoryLoadDwordValueU32(state, load_inst, kind, first_src, src_count));
	}
	for (uint32_t i = 0; i < count; i++) {
		EmitStoreU32(state, OffsetRegisterOperand(inst.dst, i), values[i]);
	}
}

uint32_t EmitMemoryLoadSubDwordValueU32(EmitterState* state, const IR::Instruction& inst,
                                        IR::ResourceKind kind, uint32_t first_src,
                                        uint32_t src_count, uint32_t data_bits, bool sign_extend) {
	auto mem           = inst.memory;
	mem.kind           = kind;
	const auto address = EmitMemoryByteAddress(state, inst, mem, first_src, src_count);
	const auto index   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, index, address, ConstantU32(state, 2)});
	auto LoadFromPointer = [&](uint32_t pointer) {
		const auto word         = state->builder.AllocateId();
		const auto byte_in_word = state->builder.AllocateId();
		const auto shift        = state->builder.AllocateId();
		const auto shifted      = state->builder.AllocateId();
		const auto masked       = state->builder.AllocateId();
		const auto value        = sign_extend ? state->builder.AllocateId() : masked;
		const auto mask         = data_bits == 8u ? 0xffu : 0xffffu;
		state->builder.AddFunction({OpLoad, state->uint_type, word, pointer});
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, byte_in_word, address, ConstantU32(state, 3)});
		state->builder.AddFunction(
		    {OpShiftLeftLogical, state->uint_type, shift, byte_in_word, ConstantU32(state, 3)});
		state->builder.AddFunction({OpShiftRightLogical, state->uint_type, shifted, word, shift});
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, masked, shifted, ConstantU32(state, mask)});
		if (sign_extend) {
			const auto left       = state->builder.AllocateId();
			const auto sign_shift = 32u - data_bits;
			state->builder.AddFunction({OpShiftLeftLogical, state->uint_type, left, masked,
			                            ConstantU32(state, sign_shift)});
			state->builder.AddFunction({OpShiftRightArithmetic, state->uint_type, value, left,
			                            ConstantU32(state, sign_shift)});
		}
		return value;
	};
	if (mem.kind == IR::ResourceKind::Lds) {
		return LoadFromPointer(EmitLdsElementPointer(state, index));
	}
	if (mem.kind == IR::ResourceKind::Gds) {
		const auto in_bounds = EmitGdsElementInBounds(state, index);
		return EmitValueOrZeroIfCondition(state, in_bounds, [&]() {
			return LoadFromPointer(EmitGdsElementPointer(state, index));
		});
	}
	if (IsStorageBufferMemoryKind(mem.kind)) {
		const auto in_bounds = EmitStorageBufferElementInBounds(state, mem, index, inst.pc);
		return EmitValueOrZeroIfCondition(state, in_bounds, [&]() {
			return LoadFromPointer(EmitStorageBufferElementPointer(state, mem, index, inst.pc));
		});
	}
	return ConstantU32(state, 0);
}

void EmitMemoryLoadSubDwordU32(EmitterState* state, const IR::Instruction& inst,
                               IR::ResourceKind kind, uint32_t first_src, uint32_t src_count,
                               uint32_t data_bits, bool sign_extend) {
	const auto value = EmitMemoryLoadSubDwordValueU32(state, inst, kind, first_src, src_count,
	                                                  data_bits, sign_extend);
	EmitStoreU32(state, inst.dst, value);
}

void EmitMemoryStoreU32(EmitterState* state, const IR::Instruction& inst, IR::ResourceKind kind,
                        uint32_t first_src, uint32_t src_count) {
	auto store_inst        = inst;
	store_inst.memory.kind = kind;
	const auto value       = EmitValueLoad(state, inst.src[0]);
	uint32_t   in_bounds   = 0;
	const auto index =
	    EmitMemoryDwordIndex(state, store_inst, store_inst.memory, first_src, src_count);
	if (IsStorageBufferMemoryKind(store_inst.memory.kind)) {
		in_bounds = EmitOptionalLogicalAndBool(
		    state, in_bounds,
		    EmitStorageBufferElementInBounds(state, store_inst.memory, index, inst.pc));
	} else if (store_inst.memory.kind == IR::ResourceKind::Gds) {
		in_bounds = EmitGdsElementInBounds(state, index);
	}
	EmitIfCondition(state, in_bounds, [&]() {
		const auto pointer = EmitMemoryElementPointer(state, store_inst.memory, index, inst.pc);
		state->builder.AddFunction({OpStore, pointer, value});
	});
}

template <typename Fn>
void EmitAtomicUpdateU32(EmitterState* state, uint32_t pointer, IR::ResourceKind kind,
                         Fn&& desired_value) {
	const auto scope          = kind == IR::ResourceKind::Lds ? ScopeWorkgroup : ScopeDevice;
	const auto memory         = kind == IR::ResourceKind::Lds ? MemorySemanticsWorkgroupMemory
	                                                          : MemorySemanticsUniformMemory;
	const auto preheader      = state->builder.AllocateId();
	const auto header         = state->builder.AllocateId();
	const auto continue_label = state->builder.AllocateId();
	const auto merge          = state->builder.AllocateId();
	const auto initial        = state->builder.AllocateId();
	const auto observed       = state->builder.AllocateId();
	const auto exchanged      = state->builder.AllocateId();

	state->builder.AddFunction({OpBranch, preheader});
	state->builder.AddFunction({OpLabel, preheader});
	state->builder.AddFunction({OpAtomicLoad, state->uint_type, initial, pointer,
	                            ConstantU32(state, scope),
	                            ConstantU32(state, MemorySemanticsNone)});
	state->builder.AddFunction({OpBranch, header});
	state->builder.AddFunction({OpLabel, header});
	state->builder.AddFunction(
	    {OpPhi, state->uint_type, observed, initial, preheader, exchanged, continue_label});
	const auto desired = desired_value(observed);
	state->builder.AddFunction({OpAtomicCompareExchange, state->uint_type, exchanged, pointer,
	                            ConstantU32(state, scope), ConstantU32(state, MemorySemanticsNone),
	                            ConstantU32(state, MemorySemanticsNone), desired, observed});
	const auto success = state->builder.AllocateId();
	state->builder.AddFunction({OpIEqual, state->bool_type, success, exchanged, observed});
	state->builder.AddFunction({OpLoopMerge, merge, continue_label, LoopControlNone});
	state->builder.AddFunction({OpBranchConditional, success, merge, continue_label});
	state->builder.AddFunction({OpLabel, continue_label});
	state->builder.AddFunction({OpBranch, header});
	state->builder.AddFunction({OpLabel, merge});
	const auto semantics = MemorySemanticsAcquireRelease | memory;
	state->builder.AddFunction(
	    {OpMemoryBarrier, ConstantU32(state, scope), ConstantU32(state, semantics)});
}

void EmitMemoryStoreSubDwordU32(EmitterState* state, const IR::Instruction& inst,
                                IR::ResourceKind kind, uint32_t first_src, uint32_t src_count,
                                uint32_t data_bits) {
	auto store_inst        = inst;
	store_inst.memory.kind = kind;
	uint32_t   in_bounds   = 0;
	const auto address =
	    EmitMemoryByteAddress(state, store_inst, store_inst.memory, first_src, src_count);
	const auto index = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, index, address, ConstantU32(state, 2)});
	if (IsStorageBufferMemoryKind(store_inst.memory.kind)) {
		in_bounds = EmitOptionalLogicalAndBool(
		    state, in_bounds,
		    EmitStorageBufferElementInBounds(state, store_inst.memory, index, inst.pc));
	} else if (store_inst.memory.kind == IR::ResourceKind::Gds) {
		in_bounds = EmitGdsElementInBounds(state, index);
	}
	EmitIfCondition(state, in_bounds, [&]() {
		const auto pointer = EmitMemoryElementPointer(state, store_inst.memory, index, inst.pc);
		const auto value   = EmitValueLoad(state, inst.src[0]);
		const auto byte_in_word  = state->builder.AllocateId();
		const auto shift         = state->builder.AllocateId();
		const auto field_mask    = state->builder.AllocateId();
		const auto value_masked  = state->builder.AllocateId();
		const auto value_shifted = state->builder.AllocateId();
		const auto mask          = data_bits == 8u ? 0xffu : 0xffffu;
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, byte_in_word, address, ConstantU32(state, 3)});
		state->builder.AddFunction(
		    {OpShiftLeftLogical, state->uint_type, shift, byte_in_word, ConstantU32(state, 3)});
		state->builder.AddFunction(
		    {OpShiftLeftLogical, state->uint_type, field_mask, ConstantU32(state, mask), shift});
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, value_masked, value, ConstantU32(state, mask)});
		state->builder.AddFunction(
		    {OpShiftLeftLogical, state->uint_type, value_shifted, value_masked, shift});
		auto Merge = [&](uint32_t word) {
			const auto inverted = state->builder.AllocateId();
			const auto cleared  = state->builder.AllocateId();
			const auto merged   = state->builder.AllocateId();
			state->builder.AddFunction({OpNot, state->uint_type, inverted, field_mask});
			state->builder.AddFunction({OpBitwiseAnd, state->uint_type, cleared, word, inverted});
			state->builder.AddFunction(
			    {OpBitwiseOr, state->uint_type, merged, cleared, value_shifted});
			return merged;
		};
		if (store_inst.memory.kind == IR::ResourceKind::Lds ||
		    store_inst.memory.kind == IR::ResourceKind::Gds) {
			EmitAtomicUpdateU32(state, pointer, store_inst.memory.kind, Merge);
		} else {
			const auto word = state->builder.AllocateId();
			state->builder.AddFunction({OpLoad, state->uint_type, word, pointer});
			state->builder.AddFunction({OpStore, pointer, Merge(word)});
		}
	});
}

uint32_t AddressSourceCount(const IR::Instruction& inst, uint32_t first_src) {
	return inst.src_count > first_src ? inst.src_count - first_src : 0u;
}

bool IsFormattedBufferComponent(const IR::Instruction& inst) {
	return inst.memory.kind == IR::ResourceKind::Buffer && inst.memory.formatted &&
	       inst.memory.data_dwords == 1u;
}

Prospero::BufferFormat FormattedBufferFormat(const EmitterState&    state,
                                             const IR::Instruction& inst) {
	return inst.memory.typed
	           ? Format::DecodeTBufferFormat(inst.memory.data_format, inst.memory.number_format)
	           : static_cast<Prospero::BufferFormat>(
	                 StorageBufferFormat(state, inst.memory, inst.pc));
}

IR::Instruction WithFormatComponentByteOffset(const IR::Instruction& inst,
                                              Prospero::BufferFormat format) {
	auto rebased = inst;
	if (inst.memory.typed || inst.memory.component_index == 0u) {
		return rebased;
	}

	const auto default_component_offset = inst.memory.component_index * 4u;
	if (rebased.memory.offset >= default_component_offset) {
		rebased.memory.offset =
		    rebased.memory.offset - default_component_offset +
		    Format::GetFormatComponentByteOffset(format, inst.memory.component_index);
	}
	return rebased;
}

uint32_t EmitTBufferBitcastF32ToU32(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->uint_type, ret, value});
	return ret;
}

uint32_t EmitTBufferBitcastU32ToF32(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->float_type, ret, value});
	return ret;
}

uint32_t EmitTBufferBitcastU32ToI32(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitcast, state->int_type, ret, value});
	return ret;
}

uint32_t EmitTBufferCompareU32Constant(EmitterState* state, uint32_t opcode, uint32_t value,
                                       uint32_t constant) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {opcode, state->bool_type, ret, value, ConstantU32(state, constant)});
	return ret;
}

uint32_t EmitTBufferSelectF32(EmitterState* state, uint32_t condition, uint32_t true_value,
                              uint32_t false_value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpSelect, state->float_type, ret, condition, true_value, false_value});
	return ret;
}

bool IsSignedFormatComponent(Format::ComponentType type) {
	return type == Format::ComponentType::Sint || type == Format::ComponentType::Snorm ||
	       type == Format::ComponentType::Sscaled;
}

uint32_t EmitExtractFormatFieldU32(EmitterState* state, uint32_t raw_word, uint32_t offset,
                                   uint32_t bits, bool sign_extend) {
	if (bits == 32u && offset == 0u) {
		return raw_word;
	}

	if (sign_extend) {
		const auto signed_word = EmitTBufferBitcastU32ToI32(state, raw_word);
		const auto extracted   = state->builder.AllocateId();
		state->builder.AddFunction({OpBitFieldSExtract, state->int_type, extracted, signed_word,
		                            ConstantU32(state, offset), ConstantU32(state, bits)});
		const auto ret = state->builder.AllocateId();
		state->builder.AddFunction({OpBitcast, state->uint_type, ret, extracted});
		return ret;
	}

	const auto extracted = state->builder.AllocateId();
	state->builder.AddFunction({OpBitFieldUExtract, state->uint_type, extracted, raw_word,
	                            ConstantU32(state, offset), ConstantU32(state, bits)});
	return extracted;
}

uint32_t EmitHalfToF32Bits(EmitterState* state, uint32_t raw) {
	const auto unpacked = state->builder.AllocateId();
	const auto value    = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->vec2_float_type, unpacked, state->glsl_std450, GlslUnpackHalf2x16, raw});
	state->builder.AddFunction({OpCompositeExtract, state->float_type, value, unpacked, 0});
	return EmitTBufferBitcastF32ToU32(state, value);
}

uint32_t EmitUFloatToF32Bits(EmitterState* state, uint32_t raw, uint32_t bits) {
	const auto mantissa_bits = bits == 11u ? 6u : 5u;
	const auto mantissa_mask = (1u << mantissa_bits) - 1u;
	const auto mantissa =
	    EmitBinaryU32(state, OpBitwiseAnd, raw, ConstantU32(state, mantissa_mask));
	const auto exponent = EmitBinaryU32(
	    state, OpBitwiseAnd,
	    EmitBinaryU32(state, OpShiftRightLogical, raw, ConstantU32(state, mantissa_bits)),
	    ConstantU32(state, 0x1fu));

	const auto exponent_32 = EmitAddU32(state, exponent, ConstantU32(state, 127u - 15u));
	const auto exponent_bits =
	    EmitBinaryU32(state, OpShiftLeftLogical, exponent_32, ConstantU32(state, 23));
	const auto mantissa_bits_32 =
	    EmitBinaryU32(state, OpShiftLeftLogical, mantissa, ConstantU32(state, 23u - mantissa_bits));
	const auto normal_bits = EmitBinaryU32(state, OpBitwiseOr, exponent_bits, mantissa_bits_32);
	const auto normal      = EmitTBufferBitcastU32ToF32(state, normal_bits);

	const auto special_bits =
	    EmitBinaryU32(state, OpBitwiseOr, ConstantU32(state, 0x7f800000u), mantissa_bits_32);
	const auto special = EmitTBufferBitcastU32ToF32(state, special_bits);

	const auto mantissa_f32 = state->builder.AllocateId();
	const auto subnormal    = state->builder.AllocateId();
	state->builder.AddFunction({OpConvertUToF, state->float_type, mantissa_f32, mantissa});
	state->builder.AddFunction(
	    {OpFMul, state->float_type, subnormal, mantissa_f32,
	     ConstantF32Value(state, std::ldexp(1.0f, 1 - 15 - static_cast<int>(mantissa_bits)))});

	const auto zero_exp    = EmitTBufferCompareU32Constant(state, OpIEqual, exponent, 0);
	const auto special_exp = EmitTBufferCompareU32Constant(state, OpIEqual, exponent, 31);
	const auto finite      = EmitTBufferSelectF32(state, zero_exp, subnormal, normal);
	const auto result      = EmitTBufferSelectF32(state, special_exp, special, finite);
	return EmitTBufferBitcastF32ToU32(state, result);
}

uint32_t EmitFormatRawComponent(EmitterState* state, const IR::Instruction& inst,
                                const Format::BufferFormatInfo& info) {
	const auto component = inst.memory.component_index;
	if (component >= info.component_count) {
		return ConstantU32(state, 0);
	}

	auto       load_inst   = WithFormatComponentByteOffset(inst, info.format);
	const auto bits        = info.component_bits[component];
	const auto sign_extend = IsSignedFormatComponent(info.type);
	if (info.packed_bitfield) {
		const auto raw_word = EmitMemoryLoadDwordValueU32(
		    state, load_inst, IR::ResourceKind::Buffer, 0, AddressSourceCount(load_inst, 0));
		return EmitExtractFormatFieldU32(state, raw_word, info.component_bit_offset[component],
		                                 bits, sign_extend);
	}

	if (bits == 32u) {
		return EmitMemoryLoadDwordValueU32(state, load_inst, IR::ResourceKind::Buffer, 0,
		                                   AddressSourceCount(load_inst, 0));
	}

	return EmitMemoryLoadSubDwordValueU32(state, load_inst, IR::ResourceKind::Buffer, 0,
	                                      AddressSourceCount(load_inst, 0), bits, sign_extend);
}

uint32_t NormalizeFormatComponent(EmitterState* state, const Format::BufferFormatInfo& info,
                                  uint32_t component, uint32_t raw) {
	const auto bits = info.component_bits[component];
	switch (info.type) {
		case Format::ComponentType::Uint:
		case Format::ComponentType::Sint: return raw;
		case Format::ComponentType::Uscaled: {
			const auto value = state->builder.AllocateId();
			state->builder.AddFunction({OpConvertUToF, state->float_type, value, raw});
			return EmitTBufferBitcastF32ToU32(state, value);
		}
		case Format::ComponentType::Sscaled: {
			const auto signed_raw = EmitTBufferBitcastU32ToI32(state, raw);
			const auto value      = state->builder.AllocateId();
			state->builder.AddFunction({OpConvertSToF, state->float_type, value, signed_raw});
			return EmitTBufferBitcastF32ToU32(state, value);
		}
		case Format::ComponentType::Unorm: {
			const auto value      = state->builder.AllocateId();
			const auto normalized = state->builder.AllocateId();
			const auto max_value  = static_cast<float>((1u << bits) - 1u);
			state->builder.AddFunction({OpConvertUToF, state->float_type, value, raw});
			state->builder.AddFunction(
			    {OpFDiv, state->float_type, normalized, value, ConstantF32Value(state, max_value)});
			return EmitTBufferBitcastF32ToU32(state, normalized);
		}
		case Format::ComponentType::Snorm: {
			const auto signed_raw = EmitTBufferBitcastU32ToI32(state, raw);
			const auto value      = state->builder.AllocateId();
			const auto normalized = state->builder.AllocateId();
			const auto clamped    = state->builder.AllocateId();
			const auto max_value  = static_cast<float>((1u << (bits - 1u)) - 1u);
			state->builder.AddFunction({OpConvertSToF, state->float_type, value, signed_raw});
			state->builder.AddFunction(
			    {OpFDiv, state->float_type, normalized, value, ConstantF32Value(state, max_value)});
			state->builder.AddFunction({OpExtInst, state->float_type, clamped, state->glsl_std450,
			                            GlslFMax, normalized, ConstantF32Value(state, -1.0f)});
			return EmitTBufferBitcastF32ToU32(state, clamped);
		}
		case Format::ComponentType::Float:
			if (bits == 32u) {
				return raw;
			}
			if (bits == 16u) {
				return EmitHalfToF32Bits(state, raw);
			}
			return EmitUFloatToF32Bits(state, raw, bits);
		default: return raw;
	}
}

uint32_t UnpackTBufferFormat(EmitterState* state, const IR::Instruction& inst,
                             const Format::BufferFormatInfo& info) {
	const auto raw = EmitFormatRawComponent(state, inst, info);
	return NormalizeFormatComponent(state, info, inst.memory.component_index, raw);
}

bool EmitTypedTBufferLoad(EmitterState* state, const IR::Instruction& inst,
                          const Format::BufferFormatInfo& info) {
	if (!Format::CanUseTypedBufferLoad(info.format)) {
		return false;
	}
	const auto value = EmitMemoryLoadDwordValueU32(state, inst, IR::ResourceKind::Buffer, 0,
	                                               AddressSourceCount(inst, 0));
	EmitStoreU32(state, inst.dst, value);
	return true;
}

bool EmitFormattedBufferLoad(EmitterState* state, const IR::Instruction& inst) {
	if (!IsFormattedBufferComponent(inst)) {
		return false;
	}

	const auto format = FormattedBufferFormat(*state, inst);
	if (!Format::IsKnownFormat(format)) {
		return false;
	}

	const auto info = Format::GetFormatInfo(format);
	if (inst.memory.component_index >= info.component_count) {
		EmitStoreU32(state, inst.dst, ConstantU32(state, 0));
		return true;
	}

	if (EmitTypedTBufferLoad(state, inst, info)) {
		return true;
	}

	EmitStoreU32(state, inst.dst, UnpackTBufferFormat(state, inst, info));
	return true;
}

uint32_t FormattedBufferDwordStoreComponentCount(Prospero::BufferFormat format,
                                                 uint32_t               opcode_components) {
	switch (format) {
		case Prospero::BufferFormat::k32UInt:
		case Prospero::BufferFormat::k32SInt:
		case Prospero::BufferFormat::k32Float: return opcode_components >= 1u ? 1u : 0u;
		case Prospero::BufferFormat::k32_32UInt:
		case Prospero::BufferFormat::k32_32SInt:
		case Prospero::BufferFormat::k32_32Float: return opcode_components >= 2u ? 2u : 0u;
		case Prospero::BufferFormat::k32_32_32UInt:
		case Prospero::BufferFormat::k32_32_32SInt:
		case Prospero::BufferFormat::k32_32_32Float: return opcode_components >= 3u ? 3u : 0u;
		case Prospero::BufferFormat::k32_32_32_32UInt:
		case Prospero::BufferFormat::k32_32_32_32SInt:
		case Prospero::BufferFormat::k32_32_32_32Float: return opcode_components >= 4u ? 4u : 0u;
		default: return 0u;
	}
}

bool EmitBufferIntegerFormatStore(EmitterState* state, const IR::Instruction& inst) {
	if (!IsFormattedBufferComponent(inst)) {
		return false;
	}

	const auto format = FormattedBufferFormat(*state, inst);
	if (!inst.memory.typed) {
		const auto dword_components =
		    FormattedBufferDwordStoreComponentCount(format, inst.memory.component_count);
		if (dword_components != 0u) {
			if (inst.memory.component_index >= dword_components) {
				return true;
			}
			EmitMemoryStoreU32(state, inst, IR::ResourceKind::Buffer, 1,
			                   AddressSourceCount(inst, 1));
			return true;
		}

		switch (format) {
			case Prospero::BufferFormat::k8UInt:
			case Prospero::BufferFormat::k8SInt:
				if (inst.memory.component_index != 0u) {
					return true;
				}
				EmitMemoryStoreSubDwordU32(state, inst, IR::ResourceKind::Buffer, 1,
				                           AddressSourceCount(inst, 1), 8);
				return true;
			case Prospero::BufferFormat::k16UInt:
			case Prospero::BufferFormat::k16SInt:
				if (inst.memory.component_index != 0u) {
					return true;
				}
				EmitMemoryStoreSubDwordU32(state, inst, IR::ResourceKind::Buffer, 1,
				                           AddressSourceCount(inst, 1), 16);
				return true;
			case Prospero::BufferFormat::k8_8UInt:
			case Prospero::BufferFormat::k8_8SInt: {
				if (inst.memory.component_index >= 2u) {
					return true;
				}
				const auto rebased = WithFormatComponentByteOffset(inst, format);
				EmitMemoryStoreSubDwordU32(state, rebased, IR::ResourceKind::Buffer, 1,
				                           AddressSourceCount(rebased, 1), 8);
				return true;
			}
			case Prospero::BufferFormat::k16_16UInt:
			case Prospero::BufferFormat::k16_16SInt: {
				if (inst.memory.component_index >= 2u) {
					return true;
				}
				const auto rebased = WithFormatComponentByteOffset(inst, format);
				EmitMemoryStoreSubDwordU32(state, rebased, IR::ResourceKind::Buffer, 1,
				                           AddressSourceCount(rebased, 1), 16);
				return true;
			}
			default: return false;
		}
	}

	switch (format) {
		case Prospero::BufferFormat::k8UInt:
		case Prospero::BufferFormat::k8SInt:
			EmitMemoryStoreSubDwordU32(state, inst, IR::ResourceKind::Buffer, 1,
			                           AddressSourceCount(inst, 1), 8);
			return true;
		case Prospero::BufferFormat::k16UInt:
		case Prospero::BufferFormat::k16SInt:
			EmitMemoryStoreSubDwordU32(state, inst, IR::ResourceKind::Buffer, 1,
			                           AddressSourceCount(inst, 1), 16);
			return true;
		case Prospero::BufferFormat::k8_8UInt:
		case Prospero::BufferFormat::k8_8SInt:
			EmitMemoryStoreSubDwordU32(state, inst, IR::ResourceKind::Buffer, 1,
			                           AddressSourceCount(inst, 1), 8);
			return true;
		case Prospero::BufferFormat::k16_16UInt:
		case Prospero::BufferFormat::k16_16SInt:
			EmitMemoryStoreSubDwordU32(state, inst, IR::ResourceKind::Buffer, 1,
			                           AddressSourceCount(inst, 1), 16);
			return true;
		default: break;
	}
	return false;
}

uint32_t EmitAtomicPointer(EmitterState* state, const IR::Instruction& inst) {
	switch (inst.memory.kind) {
		case IR::ResourceKind::Lds:
			return EmitLdsElementPointer(state, EmitDwordIndex(state, inst, 1, 1));
		case IR::ResourceKind::Gds:
			return EmitGdsElementPointer(state, EmitDwordIndex(state, inst, 1, 1));
		case IR::ResourceKind::Buffer: {
			const auto index =
			    EmitMemoryDwordIndex(state, inst, inst.memory, 1, AddressSourceCount(inst, 1));
			return EmitStorageBufferElementPointer(state, inst.memory, index, inst.pc);
		}
		case IR::ResourceKind::StorageImageUint: {
			const auto view = StorageImageViewKind(*state, inst.memory, true, inst.pc);
			const auto image_pointer =
			    StorageImageDescriptorPointer(state, inst.memory.resource, true, inst.pc, view);
			const auto pointer = state->builder.AllocateId();
			state->builder.AddFunction({OpImageTexelPointer, state->ptr_image_uint, pointer,
			                            image_pointer, EmitImageCoordU32(state, inst, view),
			                            ConstantU32(state, 0)});
			return pointer;
		}
		default: return 0;
	}
}

void EmitDeviceAtomicMemoryBarrier(EmitterState* state) {
	const auto semantics = MemorySemanticsAcquireRelease | MemorySemanticsUniformMemory;
	state->builder.AddFunction(
	    {OpMemoryBarrier, ConstantU32(state, ScopeDevice), ConstantU32(state, semantics)});
}

void EmitAtomicU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode) {
	if (IsStorageBufferMemoryKind(inst.memory.kind)) {
		const auto index =
		    EmitMemoryDwordIndex(state, inst, inst.memory, 1, AddressSourceCount(inst, 1));
		const auto in_bounds = EmitStorageBufferElementInBounds(state, inst.memory, index, inst.pc);
		const auto value     = EmitValueLoad(state, inst.src[0]);
		const auto old       = EmitValueOrZeroIfCondition(state, in_bounds, [&]() {
			const auto pointer =
			    EmitStorageBufferElementPointer(state, inst.memory, index, inst.pc);
			const auto result = state->builder.AllocateId();
			state->builder.AddFunction({opcode, state->uint_type, result, pointer,
			                            ConstantU32(state, ScopeDevice),
			                            ConstantU32(state, MemorySemanticsNone), value});
			EmitDeviceAtomicMemoryBarrier(state);
			return result;
		});
		EmitStoreU32(state, inst.dst, old);
		return;
	}
	if (inst.memory.kind == IR::ResourceKind::Gds) {
		const auto index     = EmitDwordIndex(state, inst, 1, 1);
		const auto in_bounds = EmitGdsElementInBounds(state, index);
		const auto value     = EmitValueLoad(state, inst.src[0]);
		const auto old       = EmitValueOrZeroIfCondition(state, in_bounds, [&]() {
			const auto pointer = EmitGdsElementPointer(state, index);
			const auto result  = state->builder.AllocateId();
			state->builder.AddFunction({opcode, state->uint_type, result, pointer,
			                            ConstantU32(state, ScopeDevice),
			                            ConstantU32(state, MemorySemanticsNone), value});
			EmitDeviceAtomicMemoryBarrier(state);
			return result;
		});
		EmitStoreU32(state, inst.dst, old);
		return;
	}

	const auto pointer = EmitAtomicPointer(state, inst);
	if (pointer == 0) {
		EmitStoreU32(state, inst.dst, ConstantU32(state, 0));
		return;
	}

	const auto value = EmitValueLoad(state, inst.src[0]);
	const auto old   = state->builder.AllocateId();
	const auto scope = inst.memory.kind == IR::ResourceKind::Lds ? ScopeWorkgroup : ScopeDevice;
	state->builder.AddFunction({opcode, state->uint_type, old, pointer, ConstantU32(state, scope),
	                            ConstantU32(state, MemorySemanticsNone), value});
	if (inst.memory.kind == IR::ResourceKind::StorageImageUint ||
	    inst.memory.kind == IR::ResourceKind::Gds) {
		EmitDeviceAtomicMemoryBarrier(state);
	}
	EmitStoreU32(state, inst.dst, old);
}

void EmitSLoadDword(EmitterState* state, const IR::Instruction& inst) {
	if (state->address_memory_variable == 0) {
		ExitDescriptorBindingFailure(*state, IR::DescriptorBindingKind::AddressMemory,
		                             inst.memory.resource,
		                             "scalar memory descriptor array was not emitted");
	}
	const auto binding = ResourceForDescriptor(*state, IR::DescriptorBindingKind::AddressMemory,
	                                           inst.memory.resource);
	const auto address = EmitRelativeAddress(state, inst, 0, 1, false, true);
	const auto index   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, index, address, ConstantU32(state, 2)});
	const auto object = state->builder.AllocateId();
	state->builder.AddFunction({OpAccessChain, state->ptr_storage_buffer, object,
	                            state->address_memory_variable,
	                            ConstantU32(state, binding.array_index)});
	const auto length    = state->builder.AllocateId();
	const auto in_bounds = state->builder.AllocateId();
	state->builder.AddFunction({OpArrayLength, state->uint_type, length, object, 0});
	state->builder.AddFunction({OpULessThan, state->bool_type, in_bounds, index, length});
	const auto value = EmitValueOrZeroIfCondition(state, in_bounds, [&]() {
		const auto pointer = state->builder.AllocateId();
		const auto loaded  = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpAccessChain, state->ptr_storage_buffer_uint, pointer, state->address_memory_variable,
		     ConstantU32(state, binding.array_index), ConstantU32(state, 0), index});
		state->builder.AddFunction({OpLoad, state->uint_type, loaded, pointer});
		return loaded;
	});
	EmitStoreU32(state, inst.dst, value);
}

void EmitLoadSrtDword(EmitterState* state, const IR::Instruction& inst) {
	if (state->flattened_srt_variable == 0 || inst.src_count != 1 ||
	    inst.src[0].kind != IR::OperandKind::ImmediateU32) {
		ExitDescriptorBindingFailure(*state, IR::DescriptorBindingKind::FlattenedSrt,
		                             inst.src_count != 0 ? inst.src[0].imm : 0,
		                             "invalid flattened SRT load");
	}
	const auto pointer = state->builder.AllocateId();
	const auto value   = state->builder.AllocateId();
	state->builder.AddFunction({OpAccessChain, state->ptr_storage_buffer_uint, pointer,
	                            state->flattened_srt_variable, ConstantU32(state, 0),
	                            ConstantU32(state, inst.src[0].imm)});
	state->builder.AddFunction({OpLoad, state->uint_type, value, pointer});
	EmitStoreU32(state, inst.dst, value);
}

void EmitBufferLoadUbyte(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		EmitMemoryLoadSubDwordU32(state, inst, IR::ResourceKind::Buffer, 0,
		                          AddressSourceCount(inst, 0), 8, false);
	});
}

void EmitBufferLoadSbyte(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		EmitMemoryLoadSubDwordU32(state, inst, IR::ResourceKind::Buffer, 0,
		                          AddressSourceCount(inst, 0), 8, true);
	});
}

void EmitBufferLoadUshort(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		EmitMemoryLoadSubDwordU32(state, inst, IR::ResourceKind::Buffer, 0,
		                          AddressSourceCount(inst, 0), 16, false);
	});
}

void EmitBufferLoadSshort(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		EmitMemoryLoadSubDwordU32(state, inst, IR::ResourceKind::Buffer, 0,
		                          AddressSourceCount(inst, 0), 16, true);
	});
}

void EmitBufferLoadDword(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		if (EmitFormattedBufferLoad(state, inst)) {
			return;
		}
		EmitMemoryLoadU32(state, inst, IR::ResourceKind::Buffer, 0, AddressSourceCount(inst, 0));
	});
}

void EmitBufferStoreDword(EmitterState* state, const IR::Instruction& inst) {
	if (EmitBufferIntegerFormatStore(state, inst)) {
		return;
	}
	EmitMemoryStoreU32(state, inst, IR::ResourceKind::Buffer, 1, AddressSourceCount(inst, 1));
}

void EmitFlatLoadUbyte(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		EmitMemoryLoadSubDwordU32(state, inst, inst.memory.kind, 0, inst.src_count, 8, false);
	});
}

void EmitFlatLoadSbyte(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		EmitMemoryLoadSubDwordU32(state, inst, inst.memory.kind, 0, inst.src_count, 8, true);
	});
}

void EmitFlatLoadUshort(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		EmitMemoryLoadSubDwordU32(state, inst, inst.memory.kind, 0, inst.src_count, 16, false);
	});
}

void EmitFlatLoadSshort(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		EmitMemoryLoadSubDwordU32(state, inst, inst.memory.kind, 0, inst.src_count, 16, true);
	});
}

void EmitFlatLoadDword(EmitterState* state, const IR::Instruction& inst) {
	EmitGuardedByExec(state, [&]() {
		EmitMemoryLoadDwordsU32(state, inst, inst.memory.kind, 0, inst.src_count);
	});
}

uint32_t EmitDsAddtidDwordIndex(EmitterState* state, const IR::Instruction& inst,
                                uint32_t m0_src_index) {
	if (state->stage != ShaderType::Compute) {
		const auto m0 = m0_src_index < inst.src_count ? EmitValueLoad(state, inst.src[m0_src_index])
		                                              : ConstantU32(state, 0);
		const auto m0_lo = state->builder.AllocateId();
		const auto base  = state->builder.AllocateId();
		const auto index = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, m0_lo, m0, ConstantU32(state, 0x0000ffffu)});
		state->builder.AddFunction(
		    {OpIAdd, state->uint_type, base, m0_lo, ConstantU32(state, inst.memory.offset)});
		state->builder.AddFunction(
		    {OpShiftRightLogical, state->uint_type, index, base, ConstantU32(state, 2)});
		return index;
	}

	const auto local_index = EmitLocalInvocationIndex(state);
	const auto lane        = state->builder.AllocateId();
	const auto lane_bytes  = state->builder.AllocateId();
	const auto m0    = m0_src_index < inst.src_count ? EmitValueLoad(state, inst.src[m0_src_index])
	                                                 : ConstantU32(state, 0);
	const auto m0_lo = state->builder.AllocateId();
	const auto base  = state->builder.AllocateId();
	const auto address = state->builder.AllocateId();
	const auto index   = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane, local_index, ConstantU32(state, 63)});
	state->builder.AddFunction(
	    {OpShiftLeftLogical, state->uint_type, lane_bytes, lane, ConstantU32(state, 2)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, m0_lo, m0, ConstantU32(state, 0x0000ffffu)});
	state->builder.AddFunction(
	    {OpIAdd, state->uint_type, base, m0_lo, ConstantU32(state, inst.memory.offset)});
	state->builder.AddFunction({OpIAdd, state->uint_type, address, base, lane_bytes});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, index, address, ConstantU32(state, 2)});
	return index;
}

void EmitDsWriteAddtidB32(EmitterState* state, const IR::Instruction& inst) {
	const auto value   = EmitValueLoad(state, inst.src[0]);
	const auto index   = EmitDsAddtidDwordIndex(state, inst, 1);
	const auto pointer = EmitLdsElementPointer(state, index);
	state->builder.AddFunction({OpStore, pointer, value});
}

void EmitDsReadAddtidB32(EmitterState* state, const IR::Instruction& inst) {
	const auto index   = EmitDsAddtidDwordIndex(state, inst, 0);
	const auto pointer = EmitLdsElementPointer(state, index);
	const auto value   = state->builder.AllocateId();
	state->builder.AddFunction({OpLoad, state->uint_type, value, pointer});
	EmitStoreU32(state, inst.dst, value);
}

struct DsCounterAddress {
	uint32_t index         = 0;
	uint32_t size          = 0;
	uint32_t in_lds_bounds = 0;
};

DsCounterAddress EmitAppendConsumeAddress(EmitterState* state, const IR::Instruction& inst) {
	const auto m0 = inst.src_count > 0 ? EmitValueLoad(state, inst.src[0]) : ConstantU32(state, 0);
	const auto base      = state->builder.AllocateId();
	const auto size      = state->builder.AllocateId();
	const auto address   = state->builder.AllocateId();
	const auto index     = state->builder.AllocateId();
	const auto in_bounds = state->builder.AllocateId();
	// RDNA2 puts the DS append/consume byte base in M0[31:16].
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, base, m0, ConstantU32(state, 16)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, size, m0, ConstantU32(state, 0xffffu)});
	state->builder.AddFunction(
	    {OpIAdd, state->uint_type, address, base, ConstantU32(state, inst.memory.offset)});
	state->builder.AddFunction(
	    {OpShiftRightLogical, state->uint_type, index, address, ConstantU32(state, 2)});
	state->builder.AddFunction({OpULessThan, state->bool_type, in_bounds,
	                            ConstantU32(state, inst.memory.offset + 3u), size});
	return {index, size, in_bounds};
}

uint32_t EmitGdsElementInBounds(EmitterState* state, uint32_t index) {
	const auto length    = state->builder.AllocateId();
	const auto in_bounds = state->builder.AllocateId();
	state->builder.AddFunction({OpArrayLength, state->uint_type, length, state->gds_variable, 0});
	state->builder.AddFunction({OpULessThan, state->bool_type, in_bounds, index, length});
	return in_bounds;
}

uint32_t EmitGdsElementPointer(EmitterState* state, uint32_t index) {
	const auto pointer = state->builder.AllocateId();
	state->builder.AddFunction({OpAccessChain, state->ptr_storage_buffer_uint, pointer,
	                            state->gds_variable, ConstantU32(state, 0), index});
	return pointer;
}

uint32_t EmitBitCountU32Value(EmitterState* state, uint32_t value) {
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction({OpBitCount, state->uint_type, ret, value});
	return ret;
}

uint32_t EmitFindLsbU32Value(EmitterState* state, uint32_t value) {
	const auto i32 = state->builder.AllocateId();
	const auto ret = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpExtInst, state->int_type, i32, state->glsl_std450, GlslFindILsb, value});
	state->builder.AddFunction({OpBitcast, state->uint_type, ret, i32});
	return ret;
}

struct ExecMaskInfo {
	uint32_t active_count = 0;
	uint32_t first_lane   = 0;
	uint32_t any_active   = 0;
};

ExecMaskInfo EmitExecMaskInfo(EmitterState* state) {
	uint32_t exec_lo = 0;
	uint32_t exec_hi = 0;
	if (state->per_invocation_masks) {
		const auto ballot = state->builder.AllocateId();
		state->builder.AddFunction({OpGroupNonUniformBallot, state->vec4_uint_type, ballot,
		                            ConstantU32(state, ScopeSubgroup), EmitExecActiveBool(state)});
		exec_lo = state->builder.AllocateId();
		state->builder.AddFunction({OpCompositeExtract, state->uint_type, exec_lo, ballot, 0});
		if (state->wave_size == 64u) {
			exec_hi = state->builder.AllocateId();
			state->builder.AddFunction({OpCompositeExtract, state->uint_type, exec_hi, ballot, 1});
		} else {
			exec_hi = ConstantU32(state, 0);
		}
	} else {
		exec_lo = EmitRegisterLoad(state, {IR::RegisterFile::Exec, 0});
		exec_hi = WaveMaskHighLoad(state, IR::RegisterFile::Exec);
	}
	const auto lo_count     = EmitBitCountU32Value(state, exec_lo);
	const auto hi_count     = EmitBitCountU32Value(state, exec_hi);
	const auto active_count = state->builder.AllocateId();
	state->builder.AddFunction({OpIAdd, state->uint_type, active_count, lo_count, hi_count});

	const auto lo_nonzero = EmitNotEqualZeroBool(state, exec_lo);
	const auto hi_nonzero = EmitNotEqualZeroBool(state, exec_hi);
	const auto any_active = EmitLogicalOrBool(state, lo_nonzero, hi_nonzero);
	const auto hi_lsb     = EmitFindLsbU32Value(state, exec_hi);
	const auto hi_lane    = state->builder.AllocateId();
	state->builder.AddFunction({OpIAdd, state->uint_type, hi_lane, hi_lsb, ConstantU32(state, 32)});
	const auto lo_lsb   = EmitFindLsbU32Value(state, exec_lo);
	const auto selected = state->builder.AllocateId();
	state->builder.AddFunction({OpSelect, state->uint_type, selected, lo_nonzero, lo_lsb, hi_lane});
	const auto first_lane = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, first_lane, any_active, selected, ConstantU32(state, 0)});
	return {active_count, first_lane, any_active};
}

void EmitDsAppendConsume(EmitterState* state, const IR::Instruction& inst, uint32_t atomic_opcode) {
	const auto gds = inst.memory.kind == IR::ResourceKind::Gds;
	if (gds && state->gds_variable == 0) {
		EmitStoreU32(state, inst.dst, ConstantU32(state, 0));
		return;
	}

	const auto address        = EmitAppendConsumeAddress(state, inst);
	const auto storage_bounds = gds ? EmitGdsElementInBounds(state, address.index)
	                                : EmitLdsElementInBounds(state, address.index);
	const auto m0_bounds  = gds ? EmitNotEqualZeroBool(state, address.size) : address.in_lds_bounds;
	const auto in_bounds  = EmitLogicalAndBool(state, m0_bounds, storage_bounds);
	const auto exec       = EmitExecMaskInfo(state);
	const auto subid      = EmitSubgroupLocalInvocationId(state);
	const auto first_lane = state->builder.AllocateId();
	const auto do_atomic  = state->builder.AllocateId();
	state->builder.AddFunction({OpIEqual, state->bool_type, first_lane, subid, exec.first_lane});
	const auto first_active = EmitLogicalAndBool(state, first_lane, exec.any_active);
	state->builder.AddFunction(
	    {OpLogicalAnd, state->bool_type, do_atomic, first_active, in_bounds});

	const auto atomic_value = EmitValueOrZeroIfCondition(state, do_atomic, [&]() {
		const auto pointer = gds ? EmitGdsElementPointer(state, address.index)
		                         : EmitLdsElementPointer(state, address.index);
		const auto result  = state->builder.AllocateId();
		state->builder.AddFunction({atomic_opcode, state->uint_type, result, pointer,
		                            ConstantU32(state, gds ? ScopeDevice : ScopeWorkgroup),
		                            ConstantU32(state, MemorySemanticsNone), exec.active_count});
		if (gds) {
			EmitDeviceAtomicMemoryBarrier(state);
		} else {
			const auto semantics = MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory;
			state->builder.AddFunction({OpMemoryBarrier, ConstantU32(state, ScopeWorkgroup),
			                            ConstantU32(state, semantics)});
		}
		return result;
	});
	const auto broadcast    = state->builder.AllocateId();
	state->builder.AddFunction({OpGroupNonUniformShuffle, state->uint_type, broadcast,
	                            ConstantU32(state, ScopeSubgroup), atomic_value, exec.first_lane});
	const auto value = EmitSelectU32Value(state, exec.any_active, broadcast, ConstantU32(state, 0));
	EmitStoreU32(state, inst.dst, value);
}

void EmitDsFloatMinMaxF32(EmitterState* state, const IR::Instruction& inst, bool max_value) {
	const auto index = EmitDwordIndex(state, inst, 1, 1);
	const auto in_bounds =
	    inst.memory.kind == IR::ResourceKind::Gds ? EmitGdsElementInBounds(state, index) : 0u;
	EmitIfCondition(state, in_bounds, [&]() {
		const auto pointer  = EmitMemoryElementPointer(state, inst.memory, index, inst.pc);
		const auto data_f32 = EmitFloatLoad(state, inst.src[0]);
		const auto cmp_f32  = EmitFloatLoad(state, inst.src[2]);
		EmitAtomicUpdateU32(state, pointer, inst.memory.kind, [&](uint32_t old_u32) {
			const auto old_f32   = state->builder.AllocateId();
			const auto store_src = state->builder.AllocateId();
			const auto value_f32 = state->builder.AllocateId();
			const auto value_u32 = state->builder.AllocateId();
			state->builder.AddFunction({OpBitcast, state->float_type, old_f32, old_u32});
			state->builder.AddFunction({max_value ? OpFOrdGreaterThan : OpFOrdLessThan,
			                            state->bool_type, store_src, max_value ? old_f32 : cmp_f32,
			                            max_value ? cmp_f32 : old_f32});
			state->builder.AddFunction(
			    {OpSelect, state->float_type, value_f32, store_src, data_f32, old_f32});
			state->builder.AddFunction({OpBitcast, state->uint_type, value_u32, value_f32});
			return value_u32;
		});
	});
}

uint32_t EmitDsSwizzleTargetLane(EmitterState* state, uint32_t subid, uint32_t control) {
	if ((control & 0xc000u) == 0xc000u) {
		const uint32_t mask         = control & 0x1fu;
		const uint32_t rotate       = (control >> 5u) & 0x1fu;
		const uint32_t rotate_delta = (control & 0x400u) != 0u ? ((32u - rotate) & 0x1fu) : rotate;
		const auto     lane         = EmitAndConstant(state, subid, 31);
		const auto     rotated_sum  = EmitAddU32(state, lane, ConstantU32(state, rotate_delta));
		const auto     rotated      = EmitAndConstant(state, rotated_sum, 31);
		const auto     kept         = EmitAndConstant(state, lane, mask);
		const auto     moved        = EmitAndConstant(state, rotated, (~mask) & 31u);
		const auto     combined     = EmitOrU32(state, kept, moved);
		const auto     base         = EmitAndConstant(state, subid, 0xffffffe0u);
		return EmitOrU32(state, base, combined);
	}

	if ((control & 0x8000u) != 0) {
		const auto lane2  = state->builder.AllocateId();
		const auto shift  = state->builder.AllocateId();
		const auto perm0  = state->builder.AllocateId();
		const auto perm   = state->builder.AllocateId();
		const auto base   = state->builder.AllocateId();
		const auto target = state->builder.AllocateId();
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, lane2, subid, ConstantU32(state, 3)});
		state->builder.AddFunction(
		    {OpShiftLeftLogical, state->uint_type, shift, lane2, ConstantU32(state, 1)});
		state->builder.AddFunction(
		    {OpShiftRightLogical, state->uint_type, perm0, ConstantU32(state, control), shift});
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, perm, perm0, ConstantU32(state, 3)});
		state->builder.AddFunction(
		    {OpBitwiseAnd, state->uint_type, base, subid, ConstantU32(state, 0xfffffffcu)});
		state->builder.AddFunction({OpBitwiseOr, state->uint_type, target, base, perm});
		return target;
	}

	const auto lane   = state->builder.AllocateId();
	const auto masked = state->builder.AllocateId();
	const auto ored   = state->builder.AllocateId();
	const auto xored  = state->builder.AllocateId();
	const auto base   = state->builder.AllocateId();
	const auto target = state->builder.AllocateId();
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, lane, subid, ConstantU32(state, 31)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, masked, lane, ConstantU32(state, control & 0x1fu)});
	state->builder.AddFunction(
	    {OpBitwiseOr, state->uint_type, ored, masked, ConstantU32(state, (control >> 5u) & 0x1fu)});
	state->builder.AddFunction({OpBitwiseXor, state->uint_type, xored, ored,
	                            ConstantU32(state, (control >> 10u) & 0x1fu)});
	state->builder.AddFunction(
	    {OpBitwiseAnd, state->uint_type, base, subid, ConstantU32(state, 0xffffffe0u)});
	state->builder.AddFunction({OpBitwiseOr, state->uint_type, target, base, xored});
	return target;
}

void EmitDsSwizzleB32(EmitterState* state, const IR::Instruction& inst) {
	const auto source  = EmitValueLoad(state, inst.src[0]);
	const auto control = inst.src_count > 1u ? inst.src[1].imm & 0xffffu : 0u;
	const auto subid   = EmitSubgroupLocalInvocationId(state);
	const auto target  = EmitDsSwizzleTargetLane(state, subid, control);
	const auto value   = state->builder.AllocateId();
	state->builder.AddFunction({OpGroupNonUniformShuffle, state->uint_type, value,
	                            ConstantU32(state, ScopeSubgroup), source, target});
	const auto exec_active     = EmitLaneIndexActiveBool(state, target);
	const auto subgroup_active = EmitSubgroupLaneActiveBool(state, target);
	const auto source_active   = state->builder.AllocateId();
	const auto selected        = state->builder.AllocateId();
	// RDNA2 DS_SWIZZLE returns zero when the selected source thread is invalid.
	state->builder.AddFunction(
	    {OpLogicalAnd, state->bool_type, source_active, exec_active, subgroup_active});
	state->builder.AddFunction(
	    {OpSelect, state->uint_type, selected, source_active, value, ConstantU32(state, 0)});
	EmitStoreU32(state, inst.dst, selected);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
