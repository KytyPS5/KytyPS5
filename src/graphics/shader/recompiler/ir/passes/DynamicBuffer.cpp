#include "graphics/shader/recompiler/ir/passes/DynamicBuffer.h"

#include "graphics/shader/recompiler/ir/Block.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <bit>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

uint32_t LowerDynamicBufferReads(Program& program) {
	std::vector<Inst*> targets;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::ReadConstBuffer || inst.NumArgs() != 2u) continue;
			const auto flags = inst.Flags<MemoryFlags>();
			if (flags.index >= program.memory_info.size()) continue;
			const auto& memory = program.memory_info[flags.index];
			if (memory.kind != ResourceKind::ScalarBuffer || memory.typed || memory.formatted ||
			    memory.planning_only || memory.data_bits != 32u || memory.data_dwords != 1u)
				continue;
			const auto* handle = inst.Arg(0).Resolve().TryInstruction();
			if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetBufferResource ||
			    handle->NumArgs() != 4u)
				continue;
			if (ValidateRuntimeValue(program, inst.Arg(0))) continue;
			targets.push_back(&inst);
		}
	}
	for (auto* inst: targets) {
		const auto* handle    = inst->Arg(0).Resolve().TryInstruction();
		const auto  flags     = inst->Flags<MemoryFlags>();
		const auto  immediate = program.memory_info[flags.index].offset;
		auto*       block     = inst->Parent();
		const auto  at =
		    std::ranges::find_if(*block, [&](const Inst& candidate) { return &candidate == inst; });
		const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
			return Value(&*block->PrependNewInst(at, opcode, args));
		};
		const auto wide = [&](Value value) {
			return emit(ValueOpcode::CompositeConstructU64, {value, Value(0u)});
		};
		const auto low = [&](Value value) {
			return emit(ValueOpcode::CompositeExtractU64, {value, Value(0u)});
		};
		const auto high = [&](Value value) {
			return emit(ValueOpcode::CompositeExtractU64, {value, Value(1u)});
		};

		// Match EmitReadConstBuffer: wrap the byte offset in U32, then select its word.
		auto offset = inst->Arg(1);
		if (immediate != 0u) offset = emit(ValueOpcode::IAdd32, {offset, Value(immediate)});
		const auto aligned_offset = emit(ValueOpcode::BitwiseAnd32, {offset, Value(~3u)});
		const auto word   = emit(ValueOpcode::ShiftRightLogical32, {aligned_offset, Value(2u)});
		const auto stride = emit(
		    ValueOpcode::BitwiseAnd32,
		    {emit(ValueOpcode::ShiftRightLogical32, {handle->Arg(1), Value(16u)}), Value(0x3fffu)});
		const auto byte_stride =
		    emit(ValueOpcode::SelectU32,
		         {emit(ValueOpcode::IEqual32, {stride, Value(0u)}), Value(1u), stride});
		const auto size  = emit(ValueOpcode::IMul64, {wide(byte_stride), wide(handle->Arg(2))});
		const auto words = emit(ValueOpcode::ShiftRightLogical64, {size, Value(2u)});
		// The index fits U32. A nonzero high word of the length therefore guarantees bounds.
		const auto size_in_bounds =
		    emit(ValueOpcode::LogicalOr, {emit(ValueOpcode::INotEqual32, {high(words), Value(0u)}),
		                                  emit(ValueOpcode::ULessThan32, {word, low(words)})});

		const auto valid_type = emit(
		    ValueOpcode::IEqual32,
		    {emit(ValueOpcode::BitwiseAnd32, {handle->Arg(3), Value(0xc0000000u)}), Value(0u)});
		const auto in_bounds = emit(ValueOpcode::LogicalAnd, {valid_type, size_in_bounds});

		const auto base_low     = emit(ValueOpcode::BitwiseAnd32, {handle->Arg(0), Value(~3u)});
		const auto base_high    = emit(ValueOpcode::BitwiseAnd32, {handle->Arg(1), Value(0xffffu)});
		const auto base         = emit(ValueOpcode::CompositeConstructU64, {base_low, base_high});
		const auto address      = emit(ValueOpcode::IAdd64, {base, wide(aligned_offset)});
		const auto address_low  = low(address);
		const auto address_high = high(address);
		const auto resource = emit(ValueOpcode::GetAddressResource, {address_low, address_high});
		MemoryInfo memory;
		memory.kind            = ResourceKind::Flat;
		memory.address_is_full = true;
		const MemoryFlags load_flags {.index = static_cast<uint32_t>(program.memory_info.size()),
		                              .pc    = flags.pc};
		program.memory_info.push_back(memory);
		const auto load = block->PrependNewInst(at, ValueOpcode::LoadAddressU32,
		                                        {resource, address_low, address_high, in_bounds},
		                                        std::bit_cast<uint64_t>(load_flags));
		// LoadBda emits zero without dereferencing memory when the predicate is false.
		inst->ReplaceUsesWith(Value(&*load));
	}
	return static_cast<uint32_t>(targets.size());
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
