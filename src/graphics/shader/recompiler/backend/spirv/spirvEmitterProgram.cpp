#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include "common/assert.h"

#include <algorithm>
#include <string>
#include <cstdlib>
#include <bit>
#include <cstdlib>
#include <functional>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

void EmitKillIfBoolFalse(EmitterState& state, uint32_t active) {
	const auto kill_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, active, merge_label, kill_label);
	EmitLabel(state, kill_label);
	state.builder.AddFunction(spv::OpKill);
	EmitLabel(state, merge_label);
}

void EmitKillIfPixelValidMaskInactive(EmitterState& state) {
	if (state.pixel_valid_mask_variable == 0) {
		return;
	}

	const auto mask_value = state.builder.AllocateId();
	const auto active     = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), mask_value,
	                          state.pixel_valid_mask_variable);
	state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), active, mask_value,
	                          ConstantU32(state, 0));
	EmitKillIfBoolFalse(state, active);
}

uint32_t SpillPointerType(ValueEmitContext& ctx, IR::Type type) {
	const auto value_type = TypeId(ctx.state, type);
	return value_type == 0 ? 0 : TypePointer(ctx.state, spv::StorageClassFunction, value_type);
}

// The code of an inlined call target, emitted as a SPIR-V function of its own that main calls.
// A whole program of inlined hit-group functions in one SPIR-V function (Wolverine's ray-tracing
// compute 0xe6d76d24f59f8015: 20 of them, 1.6 M words) runs the NVIDIA compiler out of memory;
// DontInline functions are compiled one at a time.
// The region is every block the entry block dominates; it must leave through edges to one block,
// `exit`. It receives the values defined before it that it reads, and returns the values the
// exit block's Phis take on its edges and the values that code after it reads directly.
struct OutlinedRegion {
	const IR::Block*                     entry = nullptr;
	const IR::Block*                     exit  = nullptr;
	std::unordered_set<const IR::Block*> blocks;
	std::vector<const IR::Inst*>         inputs;
	std::vector<const IR::Inst*>         outputs; // the exit block's Phis first, then direct values
	size_t                               phi_outputs = 0;
	uint32_t                             function    = 0;
	uint32_t                             input_type  = 0;
	uint32_t                             output_type = 0;
	uint32_t                             call_label  = 0;
	std::vector<uint32_t>                output_ids; // in main, per output and half
};

// The indirect call of a compute program with outlined call targets as the expansion of the call
// table makes it: one level per target, each a test of the first word of the target address (block
// `a`), of the second (block `b`) and the call of the target (the region), and the next level only
// when no call was made. Written out, that is a nest of as many levels as targets, each with two
// merge blocks that give every register of the program a Phi, and the NVIDIA compiler works through
// all of it for each target (the 20 targets of Wolverine's 0x1a04c58a9fbc2052: a dispatch loop that
// takes 36 s alone). The tests are about the whole wave (votes) and the first level that matches
// wins, so it is also one switch on the index of that level, with one merge block and one Phi per
// register.
struct CallLadder {
	struct Level {
		const IR::Block* a      = nullptr; // false -> m2
		const IR::Block* b      = nullptr; // false -> m1
		const IR::Block* m1     = nullptr; // Phis; the exit of the region
		const IR::Block* m2     = nullptr; // Phis; true -> the next level, false -> fin
		const IR::Block* fin    = nullptr; // merge of the levels from this one on: Phis, falls into the previous level's
		OutlinedRegion*  region = nullptr;
	};
	std::vector<Level>                   levels;
	const IR::Block*                     join = nullptr; // after a call: the first level's fin
	const IR::Block*                     none = nullptr; // after the last level, without a call
	std::unordered_set<const IR::Block*> skipped; // every block but the first level's a
};

struct PhiSlot {
	size_t                arg    = 0;       // the operand, unless region is set
	const OutlinedRegion* region = nullptr; // one operand for all of a region's edges
	size_t                output = 0;       // the region's output for this Phi
};

struct DeferredPhiPatch {
	DeferredPhi          phi;
	const IR::Inst*      instruction = nullptr;
	uint32_t             half        = 0;
	std::vector<PhiSlot> slots;
};

struct StructuredFunctionState {
	std::unordered_map<const IR::Block*, uint32_t>              block_exit_labels;
	std::vector<DeferredPhiPatch>                               deferred_phis;
	std::unordered_map<const IR::Block*, const OutlinedRegion*> region_of; // main only
	const CallLadder*                                           ladder = nullptr; // main only
};

bool HasSpirvType(IR::Type type) {
	switch (type) {
		case IR::Type::U1:
		case IR::Type::U8:
		case IR::Type::U16:
		case IR::Type::U32:
		case IR::Type::F16:
		case IR::Type::U64:
		case IR::Type::U32x2:
		case IR::Type::F32:
		case IR::Type::F64:
		case IR::Type::U32x3:
		case IR::Type::U32x4:
		case IR::Type::F32x2: return true;
		default: return false;
	}
}

// Immediate dominators over the IR CFG (Cooper, Harvey and Kennedy); unreachable blocks have none.
std::unordered_map<const IR::Block*, const IR::Block*> ImmediateDominators(const IR::Program& program) {
	std::vector<const IR::Block*>                 order;
	std::unordered_map<const IR::Block*, size_t>  rpo;
	std::unordered_set<const IR::Block*>          seen;
	std::vector<std::pair<const IR::Block*, size_t>> stack {{program.blocks.front(), 0}};
	seen.insert(program.blocks.front());
	while (!stack.empty()) {
		auto& [block, next] = stack.back();
		const auto successors = block->ImmSuccessors();
		if (next < successors.size()) {
			const auto* successor = successors[next++];
			if (seen.insert(successor).second) stack.emplace_back(successor, 0);
			continue;
		}
		order.push_back(block);
		stack.pop_back();
	}
	std::reverse(order.begin(), order.end());
	for (size_t i = 0; i < order.size(); i++)
		rpo[order[i]] = i;
	std::unordered_map<const IR::Block*, const IR::Block*> idom {{order.front(), order.front()}};
	const auto intersect = [&](const IR::Block* a, const IR::Block* b) {
		while (a != b) {
			while (rpo.at(a) > rpo.at(b)) a = idom.at(a);
			while (rpo.at(b) > rpo.at(a)) b = idom.at(b);
		}
		return a;
	};
	for (bool changed = true; changed;) {
		changed = false;
		for (size_t i = 1; i < order.size(); i++) {
			const IR::Block* dominator = nullptr;
			for (const auto* predecessor: order[i]->ImmPredecessors()) {
				if (!idom.contains(predecessor)) continue;
				dominator = dominator == nullptr ? predecessor : intersect(predecessor, dominator);
			}
			if (dominator != nullptr && idom[order[i]] != dominator) {
				idom[order[i]] = dominator;
				changed = true;
			}
		}
	}
	return idom;
}

// The regions of a compute program's inlined call targets that can become functions.
std::vector<OutlinedRegion> FindOutlinedRegions(const IR::Program& program) {
	std::vector<OutlinedRegion> regions;
	if (program.inlined_call_entries.empty() || program.dispatcher_fallback ||
	    program.stage != ShaderType::Compute || program.blocks.empty())
		return regions;
	const auto idom = ImmediateDominators(program);
	const auto dominates = [&](const IR::Block* dominator, const IR::Block* block) {
		if (!idom.contains(block)) return false;
		for (;;) {
			if (block == dominator) return true;
			const auto* parent = idom.at(block);
			if (parent == block) return false;
			block = parent;
		}
	};
	std::unordered_set<const IR::Block*> claimed;
	for (const auto pc: program.inlined_call_entries) {
		const auto found = std::ranges::find_if(program.blocks, [&](const IR::Block* block) {
			return block->start_pc == pc && block->end_pc > pc;
		});
		if (found == program.blocks.end() || *found == program.blocks.front() || claimed.contains(*found) ||
		    (*found)->ImmPredecessors().size() != 1u)
			continue;
		OutlinedRegion region;
		region.entry = *found;
		for (const auto* block: program.blocks)
			if (dominates(region.entry, block)) region.blocks.insert(block);
		std::unordered_set<const IR::Block*> exits;
		bool ok = true;
		for (const auto* block: program.blocks) {
			if (!region.blocks.contains(block)) continue;
			for (const auto* successor: block->ImmSuccessors())
				if (!region.blocks.contains(successor)) exits.insert(successor);
			const auto& term = block->terminator;
			// A return would end the function, not the invocation.
			ok &= (term.kind == CFG::TerminatorKind::Branch && term.true_block != nullptr) ||
			      (term.kind == CFG::TerminatorKind::ConditionalBranch && term.true_block != nullptr &&
			       term.false_block != nullptr && !block->condition.IsEmpty());
		}
		if (!ok || exits.size() != 1u) continue;
		region.exit = *exits.begin();
		for (const auto* block: program.blocks) {
			if (!region.blocks.contains(block)) continue;
			for (const auto* inst: {block->terminator.merge_block, block->terminator.continue_block})
				ok &= inst == nullptr || region.blocks.contains(inst) || inst == region.exit;
		}
		if (!ok) continue;
		// Inputs: the outside values the region's instructions and conditions read, through
		// operands without a SPIR-V value (resource handles) as well.
		std::unordered_set<const IR::Inst*> inputs, visited;
		const auto collect = [&](auto&& self, IR::Value value) -> void {
			value = value.Resolve();
			const auto* inst = value.TryInstruction();
			if (inst == nullptr || region.blocks.contains(inst->Parent()) || !visited.insert(inst).second) return;
			if (HasSpirvType(inst->GetType())) {
				if (inputs.insert(inst).second) region.inputs.push_back(inst);
				return;
			}
			for (size_t index = 0; index < inst->NumArgs(); index++) self(self, inst->Arg(index));
		};
		for (const auto* block: program.blocks) {
			if (!region.blocks.contains(block)) continue;
			for (const auto& inst: *block)
				for (size_t index = 0; index < inst.NumArgs(); index++) collect(collect, inst.Arg(index));
			collect(collect, block->condition);
			collect(collect, block->indirect_target);
		}
		// The values the exit block's Phis take on the region's edges that the region does not
		// define (live through the call target unchanged).
		for (const auto& phi: *region.exit) {
			if (phi.GetOpcode() != IR::ValueOpcode::Phi) break;
			for (size_t index = 0; index < phi.NumArgs(); index++)
				if (region.blocks.contains(phi.PhiBlock(index))) collect(collect, phi.Arg(index));
		}
		// Outputs: the exit block's Phis on the region's edges, then the values used outside.
		for (const auto& phi: *region.exit) {
			if (phi.GetOpcode() != IR::ValueOpcode::Phi) break;
			bool from_region = false;
			for (size_t index = 0; index < phi.NumArgs(); index++)
				from_region |= region.blocks.contains(phi.PhiBlock(index));
			if (!from_region) continue;
			ok &= HasSpirvType(phi.GetType());
			region.outputs.push_back(&phi);
		}
		region.phi_outputs = region.outputs.size();
		std::unordered_set<const IR::Inst*> direct;
		const auto used_outside = [&](const IR::Inst& inst, const IR::Block* consumer) {
			if (!region.blocks.contains(consumer) && direct.insert(&inst).second) {
				ok &= HasSpirvType(inst.GetType());
				region.outputs.push_back(&inst);
			}
		};
		for (const auto* block: program.blocks) {
			if (region.blocks.contains(block)) continue;
			for (const auto& inst: *block) {
				for (size_t index = 0; index < inst.NumArgs(); index++) {
					const auto* arg = inst.Arg(index).Resolve().TryInstruction();
					if (arg == nullptr || !region.blocks.contains(arg->Parent())) continue;
					if (inst.GetOpcode() == IR::ValueOpcode::Phi && region.blocks.contains(inst.PhiBlock(index))) {
						ok &= block == region.exit;
						continue;
					}
					used_outside(*arg, block);
				}
			}
			for (const auto& value: {block->condition, block->indirect_target}) {
				const auto* arg = value.IsEmpty() ? nullptr : value.Resolve().TryInstruction();
				if (arg != nullptr && region.blocks.contains(arg->Parent())) used_outside(*arg, block);
			}
		}
		if (!ok) continue;
		for (const auto* block: region.blocks) claimed.insert(block);
		regions.push_back(std::move(region));
	}
	return regions;
}

std::vector<PhiSlot> PhiSlots(const StructuredFunctionState& structured, const IR::Inst& phi) {
	std::vector<PhiSlot> slots;
	if (structured.ladder != nullptr && phi.Parent() == structured.ladder->join) {
		// One operand per level: the value after its call (see PatchLadderPhi).
		for (size_t level = 0; level < structured.ladder->levels.size(); level++)
			slots.push_back({level, structured.ladder->levels[level].region, 0});
		return slots;
	}
	for (size_t index = 0; index < phi.NumArgs(); index++) {
		const auto found = structured.region_of.find(phi.PhiBlock(index));
		if (found == structured.region_of.end()) {
			slots.push_back({index});
			continue;
		}
		const auto* region = found->second;
		if (std::ranges::any_of(slots, [&](const PhiSlot& slot) { return slot.region == region; })) continue;
		const auto output = std::ranges::find(region->outputs, &phi) - region->outputs.begin();
		slots.push_back({index, region, static_cast<size_t>(output)});
	}
	return slots;
}

struct DispatcherFunctionState {
	std::array<std::unordered_map<const IR::Inst*, uint32_t>, 2> spills;
	uint32_t                                      header_label       = 0;
	uint32_t                                      select_label       = 0;
	uint32_t                                      after_switch_label = 0;
	uint32_t                                      continue_label     = 0;
	uint32_t                                      merge_label        = 0;
};

void StoreDispatcherPhiEdge(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                            const IR::Block* from, const IR::Block* to) {
	if (to == nullptr) {
		return;
	}
	for (const auto& phi: *to) {
		if (phi.GetOpcode() != IR::ValueOpcode::Phi) {
			break;
		}
		for (size_t index = 0; index < phi.NumArgs(); index++) {
			if (phi.PhiBlock(index) == from) {
				ctx.state.builder.AddFunction(spv::OpStore, dispatcher.spills[ctx.half].at(&phi),
				                              ctx.Def(phi.Arg(index)));
				break;
			}
		}
	}
}

void EmitReturn(ValueEmitContext& ctx) {
	EmitKillIfPixelValidMaskInactive(ctx.state);
	if (ctx.state.object_primitive_id_variable != 0) {
		auto& state = ctx.state;
		const auto primitive = EmitGetBuiltin(
		    ctx, IR::Value {static_cast<uint32_t>(IR::StageInputKind::PrimitiveId)}, IR::Value {0u});
		const auto object = EmitPushDataLoad(state, ConstantU32(state, 0));
		state.builder.AddFunction(spv::OpStore, state.object_primitive_id_variable,
		                          EmitAddU32(state, object, primitive));
	}
	ctx.state.builder.AddFunction(spv::OpReturn);
}

void EmitStructuredTerminator(ValueEmitContext& ctx, const IR::Block* block) {
	const auto& term       = block->terminator;
	const auto  emit_merge = [&]() {
		if (term.loop_header) {
			const auto* merge = term.merge_block;
			const auto* cont  = term.continue_block;
			if (merge != nullptr && cont != nullptr) {
				ctx.state.builder.AddFunction(spv::OpLoopMerge, merge->Definition(),
				                              cont->Definition(), spv::LoopControlMaskNone);
			}
		} else if (term.kind == CFG::TerminatorKind::ConditionalBranch &&
		           term.merge_block != nullptr) {
			ctx.state.builder.AddFunction(spv::OpSelectionMerge, term.merge_block->Definition(),
			                              spv::SelectionControlMaskNone);
		}
	};

	switch (term.kind) {
		case CFG::TerminatorKind::Branch: {
			const auto* target = term.true_block;
			if (target == nullptr) {
				EmitReturn(ctx);
				return;
			}
			emit_merge();
			ctx.state.builder.AddFunction(spv::OpBranch, target->Definition());
			return;
		}
		case CFG::TerminatorKind::ConditionalBranch: {
			const auto* true_block  = term.true_block;
			const auto* false_block = term.false_block;
			if (true_block == nullptr || false_block == nullptr || block->condition.IsEmpty()) {
				EmitReturn(ctx);
				return;
			}
			const auto condition = ctx.Def(block->condition);
			emit_merge();
			ctx.state.builder.AddFunction(spv::OpBranchConditional, condition,
			                              true_block->Definition(), false_block->Definition());
			return;
		}
		default: EmitReturn(ctx); return;
	}
}

void EmitDispatcherTarget(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                          const IR::Block* from, const IR::Block* block) {
	if (block != nullptr) {
		StoreDispatcherPhiEdge(ctx, dispatcher, from, block);
		if (ctx.other_half != nullptr) {
			StoreDispatcherPhiEdge(*ctx.other_half, dispatcher, from, block);
		}
	}
}

uint32_t EmitDispatcherNextPc(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                              const IR::Block* block) {
	const auto& term = block->terminator;
	switch (term.kind) {
		case CFG::TerminatorKind::Branch:
			EmitDispatcherTarget(ctx, dispatcher, block, term.true_block);
			return ConstantU32(ctx.state, term.true_block->id);
		case CFG::TerminatorKind::ConditionalBranch: {
			EmitDispatcherTarget(ctx, dispatcher, block, term.true_block);
			EmitDispatcherTarget(ctx, dispatcher, block, term.false_block);
			const auto selected = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpSelect, TypeU32(ctx.state), selected,
			                              ctx.Def(block->condition),
			                              ConstantU32(ctx.state, term.true_block->id),
			                              ConstantU32(ctx.state, term.false_block->id));
			return selected;
		}
		case CFG::TerminatorKind::IndirectBranch: {
			for (const auto* target: block->ImmSuccessors()) {
				EmitDispatcherTarget(ctx, dispatcher, block, target);
			}
			uint32_t selected = ConstantU32(ctx.state, UINT32_MAX);
			if (!block->indirect_target.IsEmpty()) {
				const auto selector = ctx.Def(block->indirect_target);
				for (const auto& branch: term.cases) {
					const auto match = ctx.state.builder.AllocateId();
					const auto next  = ctx.state.builder.AllocateId();
					ctx.state.builder.AddFunction(spv::OpIEqual, TypeBool(ctx.state), match,
					                              selector, ConstantU32(ctx.state, branch.value));
					ctx.state.builder.AddFunction(spv::OpSelect, TypeU32(ctx.state), next, match,
					                              ConstantU32(ctx.state, branch.target->id),
					                              selected);
					selected = next;
				}
			}
			return selected;
		}
		default: return ConstantU32(ctx.state, UINT32_MAX);
	}
}

template <typename T>
decltype(auto) Arg(ValueEmitContext& ctx, const IR::Inst& inst, size_t index) {
	if constexpr (std::is_same_v<T, const IR::Inst&>) {
		return inst;
	} else if constexpr (std::is_same_v<T, IR::Value>) {
		return inst.Arg(index);
	} else if constexpr (std::is_same_v<T, IR::ScalarReg>) {
		return inst.Arg(index).ScalarRegister();
	} else {
		static_assert(std::is_same_v<T, uint32_t>);
		return ctx.Def(inst.Arg(index));
	}
}

template <typename Context, typename Return, typename... Args>
void Invoke(Return (*emit)(Context&, Args...), ValueEmitContext& ctx, const IR::Inst& inst) {
	// A full instruction keeps metadata and predicated/lane operand loads lazy.
	static_assert(std::is_same_v<Context, ValueEmitContext> ||
	              std::is_same_v<Context, EmitterState>);
	auto& context = [&]() -> Context& {
		if constexpr (std::is_same_v<Context, EmitterState>)
			return ctx.state;
		else
			return ctx;
	}();
	constexpr bool has_inst = (std::is_same_v<Args, const IR::Inst&> || ...);
	[&]<size_t... I>(std::index_sequence<I...>) {
		static_assert(((!std::is_same_v<Args, const IR::Inst&> || I == 0) && ...));
		const auto call = [&] {
			return emit(context, Arg<Args>(ctx, inst, I - (has_inst && I != 0))...);
		};
		if constexpr (std::is_void_v<Return>) {
			call();
		} else {
			static_assert(std::is_same_v<Return, uint32_t>);
			ctx.Define(inst, call());
		}
	}(std::index_sequence_for<Args...> {});
}

void EmitDirectInstruction(ValueEmitContext& ctx, const IR::Inst& inst) {
	using Op = IR::ValueOpcode;
	const auto op = inst.GetOpcode();
	if (ctx.half != 0 &&
	    (op == Op::Ballot || op == Op::ReadFirstLane || op == Op::SubgroupReduceU32 || op == Op::RayQueryNext ||
	     op == Op::ReadConstBuffer || op == Op::ReadRealtimeClock || op == Op::ReadCallWord ||
	     (op == Op::LoadAddressU32 && ctx.Memory(inst).kind == IR::ResourceKind::ScalarAddress))) {
		// Whole-wave results, clocks and SMEM loads (SGPR operands, no EXEC) match in both halves.
		ctx.Define(inst, ctx.other_half->Result(inst));
		return;
	}
	switch (op) {
#define VALUE_OPCODE(name, ...)                                                                    \
	case IR::ValueOpcode::name: return Invoke(Emit##name, ctx, inst);
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.inc"
#undef VALUE_OPCODE
		default: ctx.Fail(inst, "has no direct SPIR-V emitter");
	}
}

void EmitStructuredInstruction(ValueEmitContext& ctx, StructuredFunctionState& structured,
                               const IR::Inst& inst) {
	if (inst.GetOpcode() == IR::ValueOpcode::Phi) {
		const auto type = TypeId(ctx.state, inst.GetType());
		if (type == 0 || inst.NumArgs() == 0) {
			ctx.Fail(inst, "has no native SPIR-V representation");
		}
		for (size_t index = 0; index < inst.NumArgs(); index++) {
			const auto* predecessor = inst.PhiBlock(index);
			if (predecessor == nullptr || predecessor->Definition() == 0) {
				ctx.Fail(inst, "has a predecessor outside the structured function");
			}
		}
		auto slots = PhiSlots(structured, inst);
		structured.deferred_phis.push_back(
		    {ctx.state.builder.AddDeferredPhi(type, ctx.Result(inst), slots.size()), &inst, ctx.half,
		     std::move(slots)});
		return;
	}
	EmitDirectInstruction(ctx, inst);
}

void EmitDispatcherInstruction(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                               const IR::Inst& inst) {
	if (inst.GetOpcode() == IR::ValueOpcode::Phi) {
		const auto type = TypeId(ctx.state, inst.GetType());
		if (type == 0) {
			ctx.Fail(inst, "cannot be loaded by the dispatcher");
		}
		ctx.state.builder.AddFunction(spv::OpLoad, type, ctx.Result(inst),
		                              dispatcher.spills[ctx.half].at(&inst));
		return;
	}
	EmitDirectInstruction(ctx, inst);
	if (const auto found = dispatcher.spills[ctx.half].find(&inst);
	    found != dispatcher.spills[ctx.half].end()) {
		ctx.state.builder.AddFunction(spv::OpStore, found->second,
		                              ctx.Def(IR::Value(const_cast<IR::Inst*>(&inst))));
	}
}

template <typename EmitInstruction>
void EmitBlockInstructions(ValueEmitContext& ctx, const IR::Block* block, EmitInstruction&& emit_instruction) {
	ctx.state.current_block = block;
	bool emitted_non_phi = false;
	for (const auto& inst: *block) {
		if (inst.GetOpcode() == IR::ValueOpcode::Phi) {
			if (emitted_non_phi) {
				ctx.Fail(inst, "appears after a non-Phi instruction");
			}
		} else {
			emitted_non_phi = true;
		}
		for (uint32_t half = 0; half < ctx.state.lane_count; half++) {
			auto& lane          = half == 0 ? ctx : *ctx.other_half;
			ctx.state.lane_half = half;
			if (half == 0 || (inst.GetOpcode() != IR::ValueOpcode::Barrier &&
			                  inst.GetOpcode() != IR::ValueOpcode::StoreCompletion &&
			                  inst.GetOpcode() != IR::ValueOpcode::DataCacheInvalidate &&
			                  inst.GetOpcode() != IR::ValueOpcode::MeshAllocate)) {
				emit_instruction(lane, inst);
			}
		}
		ctx.state.lane_half = 0;
	}
}

template <typename EmitInstruction>
void EmitBlock(ValueEmitContext& ctx, const IR::Block* block, EmitInstruction&& emit_instruction) {
	ctx.state.current_block = block;
	EmitLabel(ctx.state, block->Definition());
	EmitBlockInstructions(ctx, block, emit_instruction);
}

// The value of `value` where level `k` of a ladder has made its call and reached the join (see
// CallLadder): the Phis of the merge blocks it passes take the result of the region, the registers
// the region left alone, and what the levels after it add on the way out.
uint32_t ResolveLadderValue(ValueEmitContext& lane, const CallLadder& ladder, size_t k, IR::Value value) {
	value = value.Resolve();
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || inst->GetOpcode() != IR::ValueOpcode::Phi) return lane.Def(value);
	const auto& level = ladder.levels[k];
	const auto  incoming = [&](const IR::Block* from) {
		for (size_t index = 0; index < inst->NumArgs(); index++)
			if (inst->PhiBlock(index) == from) return inst->Arg(index);
		lane.Fail(*inst, "has no operand for the call of its level");
	};
	for (size_t j = 1; j <= k; j++) {
		if (inst->Parent() == ladder.levels[j].fin)
			return ResolveLadderValue(lane, ladder, k,
			                          incoming(j == k ? ladder.levels[k].m2 : ladder.levels[j + 1].fin));
	}
	if (inst->Parent() == level.m2) return ResolveLadderValue(lane, ladder, k, incoming(level.m1));
	if (inst->Parent() != level.m1) return lane.Def(value);
	const auto output = std::ranges::find(level.region->outputs, inst) - level.region->outputs.begin();
	if (output >= static_cast<ptrdiff_t>(level.region->phi_outputs))
		lane.Fail(*inst, "is not an output of the call of its level");
	return level.region->output_ids.at(static_cast<size_t>(output) * lane.state.lane_count + lane.half);
}

void PatchLadderPhi(ValueEmitContext& ctx, const CallLadder& ladder, const DeferredPhiPatch& deferred) {
	auto& lane = deferred.half == 0 ? ctx : *ctx.other_half;
	for (size_t level = 0; level < ladder.levels.size(); level++) {
		const auto& entry = ladder.levels[level];
		const auto& phi   = *deferred.instruction;
		// The first level reaches the join from its m2, the others from the fin of the second.
		const auto* from  = level == 0 ? ladder.levels[0].m2 : ladder.levels[1].fin;
		size_t      index = 0;
		while (index < phi.NumArgs() && phi.PhiBlock(index) != from) index++;
		if (index == phi.NumArgs()) ctx.Fail(phi, "has no operand for a level of its call table");
		ctx.state.builder.PatchDeferredPhi(deferred.phi, level,
		                                   ResolveLadderValue(lane, ladder, level, phi.Arg(index)),
		                                   entry.region->call_label);
	}
}

void PatchStructuredPhis(ValueEmitContext& ctx, StructuredFunctionState& structured) {
	for (const auto& deferred: structured.deferred_phis) {
		auto& lane = deferred.half == 0 ? ctx : *ctx.other_half;
		if (structured.ladder != nullptr && deferred.instruction->Parent() == structured.ladder->join) {
			PatchLadderPhi(ctx, *structured.ladder, deferred);
			continue;
		}
		for (size_t slot = 0; slot < deferred.slots.size(); slot++) {
			const auto& source = deferred.slots[slot];
			if (source.region != nullptr) {
				ctx.state.builder.PatchDeferredPhi(
				    deferred.phi, slot,
				    source.region->output_ids.at(source.output * ctx.state.lane_count + deferred.half),
				    source.region->call_label);
				continue;
			}
			const auto* predecessor = deferred.instruction->PhiBlock(source.arg);
			const auto  found       = structured.block_exit_labels.find(predecessor);
			if (found == structured.block_exit_labels.end()) {
				ctx.Fail(*deferred.instruction, "has a predecessor that was not emitted");
			}
			ctx.state.builder.PatchDeferredPhi(
			    deferred.phi, slot, lane.Def(deferred.instruction->Arg(source.arg)), found->second);
		}
	}
}

// Values computed at a function's entry that the instructions use.
void EmitFunctionEntryValues(EmitterState& state) {
	if (state.gds_variable != 0) {
		state.gds_length = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpArrayLength, TypeU32(state), state.gds_length,
		                          state.gds_variable, 0);
	}
	if (state.lds_storage_class == spv::StorageClassStorageBuffer && state.lds_variable != 0) {
		const auto group_x = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0);
		const auto group_y = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 1);
		const auto group_z = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 2);
		const auto count_x = EmitInputComponentU32(state, IR::StageInputKind::NumWorkgroups, 0);
		const auto count_y = EmitInputComponentU32(state, IR::StageInputKind::NumWorkgroups, 1);
		const auto row = EmitAddU32(state, group_y, EmitBinaryU32(state, spv::OpIMul, group_z, count_y));
		const auto index = EmitAddU32(state, group_x, EmitBinaryU32(state, spv::OpIMul, row, count_x));
		state.lds_base_dwords = EmitBinaryU32(state, spv::OpIMul, index,
		                                     ConstantU32(state, LdsDwordCount(state)));
	}
}

void EmitOutlinedFunction(ValueEmitContext& ctx, OutlinedRegion& region) {
	auto&       state   = ctx.state;
	const auto& program = state.program;
	const auto  lanes   = state.lane_count;
	std::vector<uint32_t> input_members, output_members;
	for (const auto* input: region.inputs)
		for (uint32_t half = 0; half < lanes; half++) input_members.push_back(TypeId(state, input->GetType()));
	for (const auto* output: region.outputs)
		for (uint32_t half = 0; half < lanes; half++) output_members.push_back(TypeId(state, output->GetType()));
	region.input_type  = input_members.empty() ? 0u
	    : state.builder.Type(spv::OpTypeStruct, std::span<const uint32_t>(input_members));
	region.output_type = output_members.empty() ? TypeVoid(state)
	    : state.builder.Type(spv::OpTypeStruct, std::span<const uint32_t>(output_members));
	const auto function_type = region.input_type == 0u
	    ? state.builder.Type(spv::OpTypeFunction, region.output_type)
	    : state.builder.Type(spv::OpTypeFunction, region.output_type, region.input_type);
	region.function = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunction, region.output_type, region.function,
	                          spv::FunctionControlDontInlineMask, function_type);
	const auto parameter = region.input_type == 0u ? 0u : state.builder.AllocateId();
	if (parameter != 0u) state.builder.AddFunction(spv::OpFunctionParameter, region.input_type, parameter);
	state.current_block = nullptr;
	EmitLabel(state, state.builder.AllocateId());
	EmitFunctionEntryValues(state);
	EmitMemoryBindings(state);
	uint32_t member = 0;
	for (const auto* input: region.inputs) {
		for (uint32_t half = 0; half < lanes; half++) {
			const auto id = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeId(state, input->GetType()), id,
			                          parameter, member++);
			input->SetDefinition(half, id);
		}
	}
	// Inside the function the entry block has a label of its own and the exit block is the
	// return block.
	const auto main_entry = region.entry->Definition();
	const auto main_exit  = region.exit->Definition();
	const auto return_label = state.builder.AllocateId();
	region.entry->SetDefinition(state.builder.AllocateId());
	region.exit->SetDefinition(return_label);
	state.builder.AddFunction(spv::OpBranch, region.entry->Definition());
	StructuredFunctionState structured;
	for (const auto* block: program.blocks) {
		if (!region.blocks.contains(block)) continue;
		EmitBlock(ctx, block, [&](ValueEmitContext& lane, const IR::Inst& inst) {
			if (block == region.entry && inst.GetOpcode() == IR::ValueOpcode::Phi) {
				lane.Define(inst, lane.Def(inst.Arg(0))); // the entry block has one predecessor
				return;
			}
			EmitStructuredInstruction(lane, structured, inst);
		});
		structured.block_exit_labels.emplace(block, state.current_label);
		EmitStructuredTerminator(ctx, block);
	}
	state.current_block = nullptr;
	EmitLabel(state, return_label);
	std::vector<const IR::Block*> exit_edges;
	for (const auto* block: program.blocks)
		if (region.blocks.contains(block) && std::ranges::find(block->ImmSuccessors(), region.exit) !=
		                                         block->ImmSuccessors().end())
			exit_edges.push_back(block);
	std::vector<uint32_t> results;
	for (size_t output = 0; output < region.outputs.size(); output++) {
		const auto* inst = region.outputs[output];
		for (uint32_t half = 0; half < lanes; half++) {
			auto& lane = half == 0 ? ctx : *ctx.other_half;
			if (output >= region.phi_outputs) {
				results.push_back(0u); // after the Phis
				continue;
			}
			std::vector<std::pair<uint32_t, uint32_t>> incoming;
			for (const auto* edge: exit_edges) {
				for (size_t index = 0; index < inst->NumArgs(); index++) {
					if (inst->PhiBlock(index) == edge) {
						incoming.emplace_back(lane.Def(inst->Arg(index)), structured.block_exit_labels.at(edge));
						break;
					}
				}
			}
			if (incoming.size() == 1u) {
				results.push_back(incoming.front().first);
				continue;
			}
			std::vector<uint32_t> words {spv::OpPhi, TypeId(state, inst->GetType()), state.builder.AllocateId()};
			for (const auto& [value, label]: incoming) {
				words.push_back(value);
				words.push_back(label);
			}
			state.builder.AddFunction(words);
			results.push_back(words[2]);
		}
	}
	for (size_t output = region.phi_outputs; output < region.outputs.size(); output++)
		for (uint32_t half = 0; half < lanes; half++)
			results[output * lanes + half] = (half == 0 ? ctx : *ctx.other_half).Def(IR::Value(const_cast<IR::Inst*>(region.outputs[output])));
	if (results.empty()) {
		state.builder.AddFunction(spv::OpReturn);
	} else {
		const auto composite = state.builder.AllocateId();
		std::vector<uint32_t> words {spv::OpCompositeConstruct, region.output_type, composite};
		words.insert(words.end(), results.begin(), results.end());
		state.builder.AddFunction(words);
		state.builder.AddFunction(spv::OpReturnValue, composite);
	}
	state.builder.AddFunction(spv::OpFunctionEnd);
	PatchStructuredPhis(ctx, structured);
	region.entry->SetDefinition(main_entry);
	region.exit->SetDefinition(main_exit);
	state.gds_length      = 0;
	state.lds_base_dwords = 0;
	state.memory_byte_offsets.clear();
	state.memory_byte_sizes.clear();
	// Every outside value the region read came in as an input; main defines its own ids.
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (!region.blocks.contains(block) && inst.Definition(0) != 0u &&
			    std::ranges::find(region.inputs, &inst) == region.inputs.end()) {
				ctx.Fail(inst, "is read by an outlined call target without being passed to it");
			}
			inst.ResetDefinitions();
		}
	}
}

void EmitOutlinedCall(ValueEmitContext& ctx, OutlinedRegion& region, uint32_t exit_label = 0) {
	auto& state = ctx.state;
	const auto lanes = state.lane_count;
	state.current_block = region.entry;
	EmitLabel(state, region.entry->Definition());
	uint32_t argument = 0;
	if (region.input_type != 0u) {
		argument = state.builder.AllocateId();
		std::vector<uint32_t> words {spv::OpCompositeConstruct, region.input_type, argument};
		for (const auto* input: region.inputs)
			for (uint32_t half = 0; half < lanes; half++)
				words.push_back((half == 0 ? ctx : *ctx.other_half).Def(IR::Value(const_cast<IR::Inst*>(input))));
		state.builder.AddFunction(words);
	}
	const auto result = state.builder.AllocateId();
	if (argument != 0u) {
		state.builder.AddFunction(spv::OpFunctionCall, region.output_type, result, region.function, argument);
	} else {
		state.builder.AddFunction(spv::OpFunctionCall, region.output_type, result, region.function);
	}
	uint32_t member = 0;
	region.output_ids.clear();
	for (size_t output = 0; output < region.outputs.size(); output++) {
		const auto* inst = region.outputs[output];
		for (uint32_t half = 0; half < lanes; half++) {
			const auto id = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeId(state, inst->GetType()), id, result,
			                          member++);
			region.output_ids.push_back(id);
			if (output >= region.phi_outputs) (half == 0 ? ctx : *ctx.other_half).Define(*inst, id);
		}
	}
	region.call_label = state.current_label;
	state.builder.AddFunction(spv::OpBranch, exit_label != 0u ? exit_label : region.exit->Definition());
}

std::optional<CallLadder> FindCallLadder(const IR::Program& program, std::vector<OutlinedRegion>& regions) {
	using Op = IR::ValueOpcode;
	std::unordered_map<const IR::Block*, CallLadder::Level> by_a;
	// The merge blocks hold their Phis and the references that keep the branch condition alive.
	const auto only_phis = [](const IR::Block* block) {
		return std::ranges::all_of(*block, [](const IR::Inst& inst) {
			return inst.GetOpcode() == Op::Phi || inst.GetOpcode() == Op::Reference ||
			       inst.GetOpcode() == Op::ReferenceU32;
		});
	};
	// Whatever a level hoists runs for every level, so it only reads.
	const auto only_reads = [](const IR::Block* block) {
		return std::ranges::all_of(*block, [](const IR::Inst& inst) {
			const auto op = inst.GetOpcode();
			return op != Op::Phi && (!inst.MayHaveSideEffects() || op == Op::Reference ||
			                         op == Op::ReferenceU32 || op == Op::ConditionRef);
		});
	};
	const auto branches = [](const IR::Block* block, const IR::Block* on_true, const IR::Block* on_false) {
		const auto& term = block->terminator;
		return term.kind == CFG::TerminatorKind::ConditionalBranch && term.true_block == on_true &&
		       term.false_block == on_false && !block->condition.IsEmpty();
	};
	const auto negation_of = [](IR::Value value, IR::Value of) {
		const auto* inst = value.Resolve().TryInstruction();
		return inst != nullptr && inst->GetOpcode() == Op::LogicalNot && !of.IsEmpty() &&
		       inst->Arg(0).Resolve().TryInstruction() == of.Resolve().TryInstruction() &&
		       of.Resolve().TryInstruction() != nullptr;
	};
	for (auto& region: regions) {
		const auto* c = region.entry;
		if (c->ImmPredecessors().size() != 1u) continue;
		const auto* b = c->ImmPredecessors()[0];
		if (b->ImmPredecessors().size() != 1u) continue;
		const auto* a  = b->ImmPredecessors()[0];
		const auto* m1 = region.exit;
		if (m1->terminator.kind != CFG::TerminatorKind::Branch || m1->terminator.true_block == nullptr) continue;
		const auto* m2 = m1->terminator.true_block;
		const auto& m2_term = m2->terminator;
		if (!branches(b, c, m1) || !branches(a, b, m2) || m2_term.kind != CFG::TerminatorKind::ConditionalBranch ||
		    m2_term.true_block == nullptr || m2_term.false_block == nullptr || m2->condition.IsEmpty() ||
		    !only_phis(m1) || !only_phis(m2) || !only_reads(b) ||
		    m2->ImmPredecessors().size() != 2u)
			continue;
		bool exits_ok = true;
		for (const auto* predecessor: m1->ImmPredecessors())
			exits_ok &= predecessor == b || region.blocks.contains(predecessor);
		for (const auto* predecessor: m2->ImmPredecessors()) exits_ok &= predecessor == a || predecessor == m1;
		// Not matched is the negation of the test that failed.
		const auto* condition = m2->condition.Resolve().TryInstruction();
		exits_ok &= condition != nullptr && condition->GetOpcode() == Op::Phi && condition->Parent() == m2 &&
		            condition->NumArgs() == 2u;
		if (exits_ok) {
			for (size_t index = 0; index < 2u; index++) {
				const auto* from = condition->PhiBlock(index);
				exits_ok &= (from == a && negation_of(condition->Arg(index), a->condition)) ||
				            (from == m1 && negation_of(condition->Arg(index), b->condition));
			}
		}
		if (!exits_ok) continue;
		by_a.emplace(a, CallLadder::Level {a, b, m1, m2, nullptr, &region});
	}
	if (by_a.size() < 2u) return std::nullopt;
	std::unordered_set<const IR::Block*> nexts;
	for (const auto& [a, level]: by_a)
		if (by_a.contains(level.m2->terminator.true_block)) nexts.insert(level.m2->terminator.true_block);
	CallLadder ladder;
	for (const auto& [a, level]: by_a)
		if (!nexts.contains(a)) {
			if (!ladder.levels.empty()) return std::nullopt;
			ladder.levels.push_back(level);
		}
	if (ladder.levels.empty()) return std::nullopt;
	while (by_a.contains(ladder.levels.back().m2->terminator.true_block))
		ladder.levels.push_back(by_a.at(ladder.levels.back().m2->terminator.true_block));
	if (ladder.levels.size() != by_a.size()) return std::nullopt;
	ladder.none = ladder.levels.back().m2->terminator.true_block;
	if (std::ranges::any_of(*ladder.none, [](const IR::Inst& inst) { return inst.GetOpcode() == Op::Phi; }) ||
	    ladder.none->ImmPredecessors().size() != 1u)
		return std::nullopt;
	std::unordered_set<const IR::Block*> members;
	// The levels nest: the false edge of a level's m2 leads to its fin, which the fin of the next level
	// falls into, so the join has two predecessors however many levels there are.
	for (size_t k = 0; k < ladder.levels.size(); k++) {
		auto& level = ladder.levels[k];
		level.fin   = level.m2->terminator.false_block;
		for (const auto* block: {level.a, level.b, level.m1, level.m2, level.fin}) members.insert(block);
		const bool last = k + 1u == ladder.levels.size();
		const auto expected = last ? 1u : 2u;
		if (level.fin == ladder.none || level.fin->ImmPredecessors().size() != expected ||
		    std::ranges::find(level.fin->ImmPredecessors(), level.m2) == level.fin->ImmPredecessors().end() ||
		    (!last && std::ranges::find(level.fin->ImmPredecessors(), ladder.levels[k + 1].m2->terminator.false_block) ==
		                  level.fin->ImmPredecessors().end()))
			return std::nullopt;
		if (k > 0) {
			if (!only_phis(level.fin) || level.fin->terminator.kind != CFG::TerminatorKind::Branch ||
			    level.fin->terminator.true_block != ladder.levels[k - 1].fin)
				return std::nullopt;
			ladder.skipped.insert(level.fin);
		}
		// The first level's block stays where it is (it is the loop's, with whatever the loop does first);
		// the others are hoisted into it, so they only read.
		if (k > 0 && !only_reads(level.a)) return std::nullopt;
		for (const auto* block: {level.b, level.m1, level.m2}) ladder.skipped.insert(block);
		if (level.a != ladder.levels.front().a) ladder.skipped.insert(level.a);
	}
	ladder.join = ladder.levels.front().fin;
	// The first level is entered from one block, which then falls into the switch.
	if (ladder.levels.front().a->ImmPredecessors().size() != 1u)
		return std::nullopt;
	// What the merge blocks define is read after the calls through the Phis of the join only (which are the
	// values after the calls, and may be read anywhere).
	members.erase(ladder.join);
	for (const auto* block: program.blocks) {
		if (members.contains(block)) continue;
		if (std::ranges::any_of(ladder.levels, [&](const auto& level) { return level.region->blocks.contains(block); }))
			continue;
		for (const auto& inst: *block) {
			if (block == ladder.join && inst.GetOpcode() == Op::Phi) continue;
			for (size_t index = 0; index < inst.NumArgs(); index++) {
				const auto* arg = inst.Arg(index).Resolve().TryInstruction();
				if (arg != nullptr && members.contains(arg->Parent()) && arg->GetOpcode() == Op::Phi)
					return std::nullopt;
			}
		}
		for (const auto& value: {block->condition, block->indirect_target}) {
			const auto* arg = value.IsEmpty() ? nullptr : value.Resolve().TryInstruction();
			if (arg != nullptr && members.contains(arg->Parent()) && arg->GetOpcode() == Op::Phi)
				return std::nullopt;
		}
	}
	return ladder;
}

// The first level's block through the switch: what every level tests is computed first, then the
// regions are called from the cases of a switch on the first level that matches.
void EmitCallLadder(ValueEmitContext& ctx, StructuredFunctionState& structured, const CallLadder& ladder) {
	auto& state = ctx.state;
	const auto instructions = [&](const IR::Block* block) {
		EmitBlockInstructions(ctx, block, [&](ValueEmitContext& lane, const IR::Inst& inst) {
			EmitStructuredInstruction(lane, structured, inst);
		});
	};
	EmitLabel(state, ladder.levels.front().a->Definition());
	for (const auto& level: ladder.levels) {
		instructions(level.a);
		instructions(level.b);
		// Until a level matches, the Phis of the merge blocks are what the level was entered with.
		for (uint32_t half = 0; half < state.lane_count; half++) {
			auto& lane = half == 0 ? ctx : *ctx.other_half;
			for (const auto* merge: {level.m1, level.m2}) {
				const auto* from = merge == level.m1 ? level.b : level.a;
				for (const auto& phi: *merge) {
					if (phi.GetOpcode() != IR::ValueOpcode::Phi) continue;
					size_t index = 0;
					while (index < phi.NumArgs() && phi.PhiBlock(index) != from) index++;
					if (index == phi.NumArgs()) ctx.Fail(phi, "has no operand for a level of its call table");
					lane.Define(phi, lane.Def(phi.Arg(index)));
				}
			}
		}
	}
	uint32_t selected = ConstantU32(state, 0);
	for (size_t level = ladder.levels.size(); level-- > 0;) {
		const auto& entry   = ladder.levels[level];
		const auto  matched = Binary(state, spv::OpLogicalAnd, TypeBool(state), ctx.Def(entry.a->condition),
		                             ctx.Def(entry.b->condition));
		selected = Select(state, TypeU32(state), matched, ConstantU32(state, static_cast<uint32_t>(level) + 1u),
		                  selected);
	}
	structured.block_exit_labels.emplace(ladder.levels.front().a, state.current_label);
	state.current_block = nullptr;
	state.builder.AddFunction(spv::OpSelectionMerge, ladder.join->Definition(), spv::SelectionControlMaskNone);
	std::vector<uint32_t> words {spv::OpSwitch, selected, ladder.none->Definition()};
	for (size_t level = 0; level < ladder.levels.size(); level++) {
		words.push_back(static_cast<uint32_t>(level) + 1u);
		words.push_back(ladder.levels[level].region->entry->Definition());
	}
	state.builder.AddFunction(words);
	for (const auto& level: ladder.levels) EmitOutlinedCall(ctx, *level.region, ladder.join->Definition());
}

void EmitStructuredFunction(ValueEmitContext& ctx, std::vector<OutlinedRegion>& regions) {
	const auto& program = ctx.state.program;
	StructuredFunctionState structured;
	for (const auto& region: regions)
		for (const auto* block: region.blocks) structured.region_of.emplace(block, &region);
	const auto ladder = FindCallLadder(program, regions);
	std::unordered_set<const OutlinedRegion*> ladder_regions;
	if (ladder) {
		LOGF("ShaderRecompiler hash=0x%016" PRIx64 " emitted the %" PRIu64 " levels of its call table as one switch\n",
		     ctx.state.program.shader_hash, static_cast<uint64_t>(ladder->levels.size()));
		structured.ladder = &*ladder;
		for (const auto& level: ladder->levels) ladder_regions.insert(level.region);
	}
	ctx.state.builder.AddFunction(spv::OpBranch, program.blocks.front()->Definition());
	for (size_t index = 0; index < program.blocks.size(); index++) {
		const auto* block = program.blocks[index];
		if (const auto found = structured.region_of.find(block); found != structured.region_of.end()) {
			if (found->second->entry == block && !ladder_regions.contains(found->second))
				EmitOutlinedCall(ctx, *const_cast<OutlinedRegion*>(found->second));
			continue;
		}
		if (ladder && ladder->skipped.contains(block)) continue;
		if (ladder && block == ladder->levels.front().a) {
			EmitCallLadder(ctx, structured, *ladder);
			continue;
		}
		EmitBlock(ctx, block, [&](ValueEmitContext& lane, const IR::Inst& inst) {
			EmitStructuredInstruction(lane, structured, inst);
		});
		structured.block_exit_labels.emplace(block, ctx.state.current_label);
		EmitStructuredTerminator(ctx, block);
	}
	PatchStructuredPhis(ctx, structured);
}

void EmitDispatcherFunction(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher) {
	auto&       state = ctx.state;
	const auto* entry = state.program.blocks.front();
	state.builder.AddFunction(spv::OpBranch, entry->Definition());
	EmitBlock(ctx, entry, [&](ValueEmitContext& lane, const IR::Inst& inst) {
		EmitDispatcherInstruction(lane, dispatcher, inst);
	});
	const auto initial_pc     = EmitDispatcherNextPc(ctx, dispatcher, entry);
	const auto initial_parent = state.current_label;
	state.builder.AddFunction(spv::OpBranch, dispatcher.header_label);

	state.current_block = nullptr;
	EmitLabel(state, dispatcher.header_label);
	const auto pc      = state.builder.AllocateId();
	const auto next_pc = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpPhi, TypeU32(state), pc, initial_pc, initial_parent, next_pc,
	                          dispatcher.continue_label);
	const auto done = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), done, pc,
	                          ConstantU32(ctx.state, UINT32_MAX));
	state.builder.AddFunction(spv::OpLoopMerge, dispatcher.merge_label, dispatcher.continue_label,
	                          spv::LoopControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, done, dispatcher.merge_label,
	                          dispatcher.select_label);

	EmitLabel(state, dispatcher.select_label);
	state.builder.AddFunction(spv::OpSelectionMerge, dispatcher.after_switch_label,
	                          spv::SelectionControlMaskNone);
	std::vector<uint32_t> words {spv::OpSwitch, pc, dispatcher.after_switch_label};
	for (size_t index = 1; index < state.program.blocks.size(); index++) {
		words.push_back(state.program.blocks[index]->id);
		words.push_back(state.program.blocks[index]->Definition());
	}
	state.builder.AddFunction(words);
	std::vector<uint32_t> next_pc_words {spv::OpPhi, TypeU32(state), next_pc,
	                                     ConstantU32(state, UINT32_MAX), dispatcher.select_label};

	for (size_t index = 1; index < state.program.blocks.size(); index++) {
		EmitBlock(ctx, state.program.blocks[index],
		          [&](ValueEmitContext& lane, const IR::Inst& inst) {
			          EmitDispatcherInstruction(lane, dispatcher, inst);
		          });
		const auto selected = EmitDispatcherNextPc(ctx, dispatcher, state.program.blocks[index]);
		next_pc_words.push_back(selected);
		next_pc_words.push_back(state.current_label);
		state.builder.AddFunction(spv::OpBranch, dispatcher.after_switch_label);
	}
	state.current_block = nullptr;
	EmitLabel(state, dispatcher.after_switch_label);
	state.builder.AddFunction(next_pc_words);
	state.builder.AddFunction(spv::OpBranch, dispatcher.continue_label);
	EmitLabel(state, dispatcher.continue_label);
	state.builder.AddFunction(spv::OpBranch, dispatcher.header_label);
	EmitLabel(state, dispatcher.merge_label);
	EmitReturn(ctx);
}

} // namespace

uint32_t TypeId(EmitterState& state, IR::Type type) {
	switch (type) {
		case IR::Type::U1: return TypeBool(state);
		case IR::Type::U8:
		case IR::Type::U16:
		case IR::Type::U32: return TypeU32(state);
		case IR::Type::F16: return TypeF16(state);
		case IR::Type::U64: return TypeU64(state);
		case IR::Type::Bvh8Result: return TypeBvh8Result(state);
		case IR::Type::U32x2: return TypeU32Pair(state);
		case IR::Type::F32: return TypeF32(state);
		case IR::Type::F64: return TypeF64(state);
		case IR::Type::U32x3: return TypeU32Vector(state, 3);
		case IR::Type::U32x4: return TypeU32Vector(state, 4);
		case IR::Type::F32x2: return TypeF32Vector(state, 2);
		default: return 0;
	}
}

uint32_t ValueEmitContext::Def(IR::Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		switch (value.GetType()) {
			case IR::Type::U1: return ConstantBool(state, value.U1());
			case IR::Type::U8: return ConstantU32(state, value.U8());
			case IR::Type::U16: return ConstantU32(state, value.U16());
			case IR::Type::U32: return ConstantU32(state, value.U32());
			case IR::Type::U64: return ConstantU64(state, value.U64());
			case IR::Type::F16:
				return state.builder.Constant(spv::OpConstant, TypeF16(state), value.F16Bits());
			case IR::Type::F32:
				return ConstantF32(state, std::bit_cast<uint32_t>(value.F32Value()));
			default: break;
		}
	}
	const auto* inst = value.ResolveInstruction();
	if (inst == nullptr) {
		Fail("direct SPIR-V emitter received a non-value argument");
	}
	if (dispatcher_spills != nullptr && state.current_block != nullptr &&
	    inst->Parent() != state.current_block) {
		if (const auto found = dispatcher_spills->find(inst); found != dispatcher_spills->end()) {
			if (const auto loaded = dispatcher_block_loads.find(inst);
			    loaded != dispatcher_block_loads.end() &&
			    loaded->second.first == state.current_label) {
				return loaded->second.second;
			}
			const auto id = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpLoad, TypeId(state, inst->GetType()), id,
			                          found->second);
			dispatcher_block_loads.insert_or_assign(inst, std::pair {state.current_label, id});
			return id;
		}
	}
	return Result(*inst);
}

uint32_t ValueEmitContext::Arg(const IR::Inst& inst, size_t index) {
	return Def(inst.Arg(index));
}

uint32_t ValueEmitContext::HalfArg(const IR::Inst& inst, size_t index, uint32_t lane_half) {
	return lane_half == half ? Arg(inst, index) : other_half->Arg(inst, index);
}

uint32_t ValueEmitContext::Ballot(IR::Value predicate) {
	const auto ballot_type = TypeU32Vector(state, 4);
	const auto scope       = ConstantU32(state, spv::ScopeSubgroup);
	const auto low         = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformBallot, ballot_type, low, scope,
	                          other_half == nullptr || half == 0 ? Def(predicate)
	                                                             : other_half->Def(predicate));
	if (other_half == nullptr) {
		return low;
	}
	const auto high      = state.builder.AllocateId();
	const auto low_word  = state.builder.AllocateId();
	const auto high_word = state.builder.AllocateId();
	const auto ballot    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformBallot, ballot_type, high, scope,
	                          half == 1 ? Def(predicate) : other_half->Def(predicate));
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low_word, low, 0);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high_word, high, 0);
	state.builder.AddFunction(spv::OpCompositeConstruct, ballot_type, ballot, low_word, high_word,
	                          ConstantU32(state, 0), ConstantU32(state, 0));
	return ballot;
}

uint32_t ValueEmitContext::FirstLane(uint32_t ballot) {
	// An empty ballot (EXEC zero) gives lane 0, which V_READFIRSTLANE reads on hardware. FindILsb
	// returns -1 for zero, unlike BallotFindLSB, whose result is then undefined.
	const auto first = [&](uint32_t word) {
		const auto bits = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), bits, ballot, word);
		return std::pair {bits, EmitGlsl<GLSLstd450FindILsb, IR::Type::U32>(state, bits)};
	};
	const auto [low, low_first] = first(0);
	if (state.program.wave_size == 32u) {
		return EmitGlsl<GLSLstd450SMax, IR::Type::U32>(state, low_first, ConstantU32(state, 0));
	}
	const auto high_first = first(1).second;
	const auto high_lane  = EmitGlsl<GLSLstd450SMax, IR::Type::U32>(
	    state, EmitBinaryU32(state, spv::OpBitwiseOr, high_first, ConstantU32(state, 32)),
	    ConstantU32(state, 0));
	return Select(state, TypeU32(state),
	              Binary(state, spv::OpINotEqual, TypeBool(state), low, ConstantU32(state, 0)),
	              low_first, high_lane);
}

uint32_t ValueEmitContext::Shuffle(const IR::Inst& inst, size_t index, uint32_t lane) {
	const auto type  = TypeId(state, inst.Arg(index).GetType());
	const auto scope = ConstantU32(state, spv::ScopeSubgroup);
	const auto low   = state.builder.AllocateId();
	if (other_half == nullptr) {
		state.builder.AddFunction(spv::OpGroupNonUniformShuffle, type, low, scope, Arg(inst, index),
		                          lane);
		return low;
	}
	const auto physical_lane =
	    EmitBinaryU32(state, spv::OpBitwiseAnd, lane, ConstantU32(state, 31));
	const auto high          = state.builder.AllocateId();
	const auto in_high       = state.builder.AllocateId();
	const auto value         = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, type, low, scope,
	                          HalfArg(inst, index, 0), physical_lane);
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, type, high, scope,
	                          HalfArg(inst, index, 1), physical_lane);
	state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), in_high,
	                          EmitBinaryU32(state, spv::OpBitwiseAnd, lane, ConstantU32(state, 32)),
	                          ConstantU32(state, 0));
	state.builder.AddFunction(spv::OpSelect, type, value, in_high, high, low);
	return value;
}

uint32_t ValueEmitContext::Result(const IR::Inst& inst) {
	if (const auto id = inst.Definition(half); id != 0) {
		return id;
	}
	const auto id = state.builder.AllocateId();
	inst.SetDefinition(half, id);
	return id;
}

uint32_t ValueEmitContext::Define(const IR::Inst& inst, uint32_t value) {
	if (const auto id = inst.Definition(half); id != 0) {
		if (id != value) {
			state.builder.AddFunction(spv::OpCopyObject, TypeId(state, inst.GetType()),
			                          id, value);
		}
		return id;
	}
	inst.SetDefinition(half, value);
	return value;
}

uint32_t ValueEmitContext::ResourceIndex(IR::Value value, IR::ValueOpcode opcode) {
	const auto* inst = value.ResolveInstruction();
	if (inst == nullptr || inst->GetOpcode() != opcode) {
		Fail("typed resource handle has the wrong producer");
	}
	return inst->Flags<uint32_t>();
}

const IR::Inst* ValueEmitContext::ImageAddress(IR::Value value) {
	const auto* inst = value.ResolveInstruction();
	if (inst == nullptr || inst->GetOpcode() != IR::ValueOpcode::MakeImageAddress) {
		Fail("typed image address was not constructed by MakeImageAddress");
	}
	return inst;
}

const IR::MemoryInfo& ValueEmitContext::Memory(const IR::Inst& inst) const {
	return state.program.memory_info.at(inst.Flags<IR::MemoryFlags>().index);
}

const IR::ExportInfo& ValueEmitContext::Export(const IR::Inst& inst) const {
	return state.program.export_info.at(inst.Flags<IR::ExportFlags>().index);
}

[[noreturn]] void ValueEmitContext::Fail(const char* reason) const {
	EXIT("SPIR-V emission failed: hash=0x%016" PRIx64 " stage=%u reason=%s\n",
	     state.program.shader_hash, static_cast<unsigned>(state.program.stage), reason);
	std::abort();
}

[[noreturn]] void ValueEmitContext::Fail(const IR::Inst& inst, const char* reason) const {
	EXIT("SPIR-V emission failed: hash=0x%016" PRIx64 " stage=%u opcode=%s reason=%s\n",
	     state.program.shader_hash, static_cast<unsigned>(state.program.stage),
	     IR::ValueOpcodeName(inst.GetOpcode()), reason);
	std::abort();
}

void EmitProgram(EmitterState& state) {
	const auto&      program = state.program;
	ValueEmitContext ctx(state);
	ValueEmitContext high(state);
	if (state.lane_count == 2) {
		ctx.other_half  = &high;
		high.other_half = &ctx;
		high.half       = 1;
	}
	std::optional<DispatcherFunctionState> dispatcher;
	if (state.program.stage == ShaderType::Pixel && state.program.info.pixel_valid_mask) {
		state.pixel_valid_mask_variable = state.builder.AllocateId();
		state.builder.AddName(state.pixel_valid_mask_variable, "pixel_valid_mask_active");
	}
	for (const auto* block: program.blocks) {
		// Re-emission may use a different host subgroup width and ID allocation order.
		for (const auto& inst: *block) {
			inst.ResetDefinitions();
		}
		block->SetDefinition(state.builder.AllocateId());
	}
	if (state.program.dispatcher_fallback) {
		auto& dispatch = dispatcher.emplace();
		// Allocate both halves in program order so the SPIR-V is identical across runs.
		const auto add_spill = [&](const IR::Inst* inst) {
			for (uint32_t half = 0; half < state.lane_count; half++) {
				dispatch.spills[half].emplace(inst, state.builder.AllocateId());
			}
		};
		for (const auto* block: program.blocks) {
			for (const auto& inst: *block) {
				if (inst.GetOpcode() != IR::ValueOpcode::Phi) {
					continue;
				}
				if (SpillPointerType(ctx, inst.GetType()) == 0) {
					ctx.Fail(inst, "cannot be stored by the dispatcher");
					break;
				}
				add_spill(&inst);
			}
		}
		const auto mark_cross_block = [&](IR::Value value, const IR::Block* consumer) {
			value                  = value.Resolve();
			const auto* definition = value.TryInstruction();
			if (definition == nullptr || definition->Parent() == consumer ||
			    definition->Parent() == program.blocks.front()) {
				return;
			}
			if (SpillPointerType(ctx, definition->GetType()) == 0) {
				ctx.Fail(*definition, "cannot be stored by the dispatcher");
				return;
			}
			if (!dispatch.spills[0].contains(definition)) {
				add_spill(definition);
			}
		};
		for (const auto* block: program.blocks) {
			for (const auto& inst: *block) {
				for (size_t index = 0; index < inst.NumArgs(); index++) {
					const auto* consumer =
					    inst.GetOpcode() == IR::ValueOpcode::Phi ? inst.PhiBlock(index) : block;
					mark_cross_block(inst.Arg(index), consumer);
				}
			}
		}
		for (size_t index = 0; index < program.blocks.size(); index++) {
			mark_cross_block(program.blocks[index]->condition, program.blocks[index]);
			mark_cross_block(program.blocks[index]->indirect_target, program.blocks[index]);
		}
		dispatch.header_label       = state.builder.AllocateId();
		dispatch.select_label       = state.builder.AllocateId();
		dispatch.after_switch_label = state.builder.AllocateId();
		dispatch.continue_label     = state.builder.AllocateId();
		dispatch.merge_label        = state.builder.AllocateId();
		ctx.dispatcher_spills       = &dispatch.spills[0];
		high.dispatcher_spills      = &dispatch.spills[1];
	}
	state.converged_calls = program.stage == ShaderType::Compute && !program.inlined_call_entries.empty() &&
	                        !program.dispatcher_fallback && !FindOutlinedRegions(program).empty();
	DefineGetBdaPointer(state);
	DefineIndirectBufferLoads(ctx);
	DefineBvhIntersect(state);
	DefineTrinityFunctions(state);
	if (program.info.uses_swizzle) {
		ctx.scratch_u32_variable = state.builder.AllocateId();
		if (state.lane_count == 2) {
			high.scratch_u32_variable = state.builder.AllocateId();
		}
	}
	auto regions = FindOutlinedRegions(program);
	// Outlined functions reach per-invocation memory through Private variables.
	const auto per_invocation = regions.empty() ? spv::StorageClassFunction : spv::StorageClassPrivate;
	if (!regions.empty()) {
		state.scratch_storage_class = spv::StorageClassPrivate;
		if (state.program.info.function_lds) {
			state.lds_storage_class = spv::StorageClassPrivate;
			state.builder.DefineGlobalVariable(
			    state.lds_variable, TypeU32ArrayPointer(state, per_invocation, LdsDwordCount(state)),
			    per_invocation);
		}
		if (state.program.info.function_scratch) {
			for (uint32_t half = 0; half < state.lane_count; half++) {
				state.builder.DefineGlobalVariable(
				    state.scratch_variable[half],
				    TypeU32ArrayPointer(state, per_invocation, state.program.scratch_dwords), per_invocation);
			}
		}
		for (auto* lane: {&ctx, &high}) {
			if (lane->scratch_u32_variable != 0) {
				state.builder.DefineGlobalVariable(lane->scratch_u32_variable,
				                                   TypePointer(state, per_invocation, TypeU32(state)),
				                                   per_invocation);
			}
		}
		for (auto& region: regions) {
			EmitOutlinedFunction(ctx, region);
		}
		LOGF("ShaderRecompiler hash=0x%016" PRIx64 " outlined %" PRIu64 " inlined call targets into functions\n",
		     program.shader_hash, static_cast<uint64_t>(regions.size()));
	}
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state),
	                          state.mesh_guest_func != 0 ? state.mesh_guest_func : state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.entry_label);
	if (state.program.info.function_lds && per_invocation == spv::StorageClassFunction) {
		state.builder.AddFunction(
		    spv::OpVariable,
		    TypeU32ArrayPointer(state, spv::StorageClassFunction, LdsDwordCount(state)),
		    state.lds_variable, spv::StorageClassFunction);
	}
	if (state.program.info.function_scratch && per_invocation == spv::StorageClassFunction) {
		for (uint32_t half = 0; half < state.lane_count; half++) {
			state.builder.AddFunction(
			    spv::OpVariable,
			    TypeU32ArrayPointer(state, spv::StorageClassFunction, state.program.scratch_dwords),
			    state.scratch_variable[half], spv::StorageClassFunction);
		}
	}
	DeclareRayQueryVariables(state);
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction(spv::OpVariable,
		                          TypePointer(state, spv::StorageClassFunction, TypeU32(state)),
		                          state.pixel_valid_mask_variable, spv::StorageClassFunction);
	}
	for (uint32_t half = 0; half < state.lane_count; half++) {
		auto& lane = half == 0 ? ctx : high;
		if (state.program.dispatcher_fallback) {
			for (const auto* block: program.blocks) {
				for (const auto& inst: *block) {
					if (const auto found = dispatcher->spills[half].find(&inst);
					    found != dispatcher->spills[half].end()) {
						state.builder.AddFunction(spv::OpVariable,
						                          SpillPointerType(lane, inst.GetType()),
						                          found->second, spv::StorageClassFunction);
					}
				}
			}
		}
		if (lane.scratch_u32_variable != 0 && per_invocation == spv::StorageClassFunction) {
			state.builder.AddFunction(spv::OpVariable,
			                          TypePointer(state, spv::StorageClassFunction, TypeU32(state)),
			                          lane.scratch_u32_variable, spv::StorageClassFunction);
		}
	}
	EmitFunctionEntryValues(state); // pixel shaders use no buffer-backed LDS: GDS stays first
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction(spv::OpStore, state.pixel_valid_mask_variable,
		                          ConstantU32(state, 1));
	}
	InitializeRayQueryVariables(state);
	EmitTableMode(state);
	EmitMemoryBindings(state);
	if (program.blocks.empty()) {
		EmitReturn(ctx);
	} else if (state.program.dispatcher_fallback) {
		EmitDispatcherFunction(ctx, *dispatcher);
	} else {
		EmitStructuredFunction(ctx, regions);
	}
	state.builder.AddFunction(spv::OpFunctionEnd);
	state.current_block = nullptr;
	if (state.program.stage == ShaderType::Mesh) {
		EmitMeshEntryPoint(state);
	}
	DefineBdaLoads(state);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
