#include "graphics/shader/recompiler/ir/passes/DynamicBuffer.h"

#include "graphics/shader/recompiler/ir/Block.h"

#include <algorithm>
#include <bit>
#include <vector>

// A buffer read whose V# is assembled from a loop-carried value has no descriptor that can be
// resolved before the draw -- the address only exists once the loop is running. Ray-tracing
// kernels hit this when they fetch per-instance geometry descriptors while walking a BVH.
//
// An untyped scalar buffer read through such a V# is just base + offset, so lower it to a raw
// address load and drop the descriptor entirely. This only fires where resolution would have
// failed and the dispatch would have been skipped, so it can only turn a dropped shader into a
// running one.

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint32_t MaxSearchDepth = 64u;

bool ReachesPhi(Value value, std::vector<const Inst*>& visited, uint32_t depth) {
	const auto* inst = value.Resolve().TryInstruction();
	if (inst == nullptr || depth > MaxSearchDepth) {
		return false;
	}
	if (inst->GetOpcode() == ValueOpcode::Phi) {
		return true;
	}
	if (std::ranges::find(visited, inst) != visited.end()) {
		return false;
	}
	visited.push_back(inst);
	for (size_t index = 0; index < inst->NumArgs(); index++) {
		if (ReachesPhi(inst->Arg(index), visited, depth + 1u)) {
			return true;
		}
	}
	return false;
}

} // namespace

uint32_t LowerDynamicBufferReads(Program& program) {
	std::vector<Inst*> targets;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::ReadConstBuffer || inst.NumArgs() != 2u) {
				continue;
			}
			const auto* resource = inst.Arg(0).Resolve().TryInstruction();
			if (resource == nullptr ||
			    resource->GetOpcode() != ValueOpcode::GetBufferResource ||
			    resource->NumArgs() != 4u) {
				continue;
			}
			bool dynamic = false;
			for (size_t dword = 0; dword < 4u && !dynamic; dword++) {
				std::vector<const Inst*> visited;
				dynamic = ReachesPhi(resource->Arg(dword), visited, 0u);
			}
			if (dynamic) {
				targets.push_back(&inst);
			}
		}
	}

	for (auto* inst: targets) {
		const auto* resource = inst->Arg(0).Resolve().TryInstruction();
		auto*       block    = inst->Parent();
		const auto  at =
		    std::ranges::find_if(*block, [&](const Inst& other) { return &other == inst; });
		const auto flags     = inst->Flags<MemoryFlags>();
		const auto immediate = flags.index < program.memory_info.size()
		                           ? program.memory_info[flags.index].offset
		                           : 0u;

		// A V# holds a 48-bit base: all of dword 0 and the low half of dword 1.
		const auto base_lo = resource->Arg(0);
		const auto base_hi = Value(&*block->PrependNewInst(at, ValueOpcode::BitwiseAnd32,
		                                                   {resource->Arg(1), Value(0xffffu)}));
		auto       offset  = inst->Arg(1);
		if (immediate != 0u) {
			offset = Value(
			    &*block->PrependNewInst(at, ValueOpcode::IAdd32, {offset, Value(immediate)}));
		}
		const auto address_lo =
		    Value(&*block->PrependNewInst(at, ValueOpcode::IAdd32, {base_lo, offset}));
		const auto wrapped = Value(
		    &*block->PrependNewInst(at, ValueOpcode::ULessThan32, {address_lo, base_lo}));
		const auto carry = Value(&*block->PrependNewInst(at, ValueOpcode::SelectU32,
		                                                 {wrapped, Value(1u), Value(0u)}));
		const auto address_hi =
		    Value(&*block->PrependNewInst(at, ValueOpcode::IAdd32, {base_hi, carry}));
		const auto address = Value(&*block->PrependNewInst(
		    at, ValueOpcode::GetAddressResource, {address_lo, address_hi}));

		MemoryInfo info;
		info.kind            = ResourceKind::Flat;
		info.data_bits       = 32u;
		info.data_dwords     = 1u;
		info.component_count = 1u;
		info.component_index = 0u;
		info.address_is_full = true;
		const MemoryFlags load_flags {
		    .index = static_cast<uint32_t>(program.memory_info.size()), .pc = flags.pc};
		program.memory_info.push_back(info);

		const auto load = block->PrependNewInst(at, ValueOpcode::LoadAddressU32,
		                                        {address, address_lo, address_hi, Value(true)},
		                                        std::bit_cast<uint64_t>(load_flags));
		inst->ReplaceUsesWith(Value(&*load));
	}
	return static_cast<uint32_t>(targets.size());
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
