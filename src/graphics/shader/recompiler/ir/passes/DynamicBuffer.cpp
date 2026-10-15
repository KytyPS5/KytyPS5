#include "graphics/shader/recompiler/ir/passes/DynamicBuffer.h"

#include "graphics/shader/recompiler/ir/Block.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
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

bool AddressReachesDynamicPhi(const Program& program, Value value) {
	std::vector<const Inst*> visited;
	const auto               walk = [&](const auto& self, Value v, uint32_t depth) -> bool {
		const auto* inst = v.Resolve().TryInstruction();
		if (inst == nullptr || depth > MaxSearchDepth) {
			return false;
		}
		// A phi that collapses to one value is the same on every path, so it stays describable. One
		// that does not is exactly the value the planner cannot turn into a descriptor.
		if (inst->GetOpcode() == ValueOpcode::Phi) {
			return ResolveInvariantPhi(program, v).IsEmpty();
		}
		if (std::ranges::find(visited, inst) != visited.end()) {
			return false;
		}
		visited.push_back(inst);
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			if (self(self, inst->Arg(index), depth + 1u)) {
				return true;
			}
		}
		return false;
	};
	return walk(walk, value, 0u);
}

namespace {

uint32_t LowerConstBufferReads(Program& program) {
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
		// Scalar buffer loads ignore the two low bits of the register offset (the non-dynamic path aligns it the
		// same way). The BVH traversal kernels rely on it: they index with node pointers whose low bits are the node
		// type, and an unaligned read here returned garbage and sent the traversal into a loop that never ended.
		auto       offset  = Value(&*block->PrependNewInst(at, ValueOpcode::BitwiseAnd32,
		                                                   {inst->Arg(1), Value(0xfffffffcu)}));
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

// A raw dword load whose address is assembled from a lane-varying value has the same problem as the
// V# above: the address only exists once the loop around it is running, so the planner hands it a
// flat SRT slot it can never fill and the dispatch aborts. Unlike the V# case the address arithmetic
// is already there -- the emitter adds the handle pair to the offset -- so the fix is only to take
// the load off the descriptor path. Retyping it to FLAT keeps the per-lane address and lets the BDA
// path issue it, exactly as a guest FLAT_LOAD does.
uint32_t LowerDynamicAddressReads(Program& program) {
	std::vector<Inst*> targets;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::LoadAddressU32 || inst.NumArgs() != 4u) {
				continue;
			}
			const auto flags = inst.Flags<MemoryFlags>();
			if (flags.index >= program.memory_info.size()) {
				continue;
			}
			const auto& memory = program.memory_info[flags.index];
			// FLAT keeps the base in the handle pair and allows at most four components, so anything
			// wider was never a scalar address read.
			if (memory.kind != ResourceKind::ScalarAddress || memory.component_count > 4u ||
			    memory.component_index != 0u) {
				continue;
			}
			const auto* handle = inst.Arg(0).Resolve().TryInstruction();
			if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource ||
			    handle->NumArgs() != 2u) {
				continue;
			}
			if (AddressReachesDynamicPhi(program, inst.Arg(0)) ||
			    AddressReachesDynamicPhi(program, inst.Arg(1)) ||
			    AddressReachesDynamicPhi(program, inst.Arg(2))) {
				targets.push_back(&inst);
			}
		}
	}

	for (auto* inst: targets) {
		const auto flags       = inst->Flags<MemoryFlags>();
		auto        info        = program.memory_info[flags.index];
		info.kind              = ResourceKind::Flat;
		info.resource          = 0;
		info.planning_only     = false;
		// The base stays in the handle pair. The emitter masks nothing for FLAT, so the lane-varying
		// low half of the address survives to the load.
		info.address_is_full   = false;
		const MemoryFlags load_flags {
		    .index = static_cast<uint32_t>(program.memory_info.size()), .pc = flags.pc};
		program.memory_info.push_back(info);
		inst->SetFlags(std::bit_cast<uint64_t>(load_flags));
	}
	return static_cast<uint32_t>(targets.size());
}

} // namespace

DynamicReadStats LowerDynamicBufferReads(Program& program) {
	return {LowerConstBufferReads(program),
	        LowerDynamicAddressReads(program)};
}

} // namespace Libs::Graphics::ShaderRecompiler::IR