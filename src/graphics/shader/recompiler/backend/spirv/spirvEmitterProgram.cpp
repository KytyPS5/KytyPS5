#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include "common/assert.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

#include <algorithm>
#include <bit>
#include <functional>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

void EmitKillIfBoolFalse(EmitterState& state, uint32_t active) {
	const auto kill_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	const auto inactive    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLogicalNot, TypeBool(state), inactive, active);
	state.builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, inactive, kill_label, merge_label);
	EmitLabel(state, kill_label);
	state.builder.AddFunction(spv::OpKill);
	EmitLabel(state, merge_label);
}

void EmitKillIfVariableZero(EmitterState& state, uint32_t variable) {
	if (variable == 0) {
		return;
	}

	const auto mask_value = state.builder.AllocateId();
	const auto active     = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), mask_value, variable);
	state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), active, mask_value,
	                          ConstantU32(state, 0));
	EmitKillIfBoolFalse(state, active);
}

void EmitKillIfPixelValidMaskInactive(EmitterState& state) {
	EmitKillIfVariableZero(state, state.pixel_valid_mask_variable);
	EmitKillIfVariableZero(state, state.stencil_bit_pass_variable);
}

uint32_t SpillPointerType(ValueEmitContext& ctx, IR::Type type) {
	const auto value_type = TypeId(ctx.state, type);
	return value_type == 0 ? 0 : TypePointer(ctx.state, spv::StorageClassFunction, value_type);
}

struct DeferredPhiPatch {
	DeferredPhi     phi;
	const IR::Inst* instruction = nullptr;
	uint32_t        half        = 0;
};

struct StructuredFunctionState {
	std::unordered_map<const IR::Block*, uint32_t> block_exit_labels;
	std::vector<DeferredPhiPatch>                  deferred_phis;
};

struct DispatcherFunctionState {
	std::array<std::unordered_map<const IR::Inst*, uint32_t>, 2> spills;
	uint32_t                                      header_label       = 0;
	uint32_t                                      select_label       = 0;
	uint32_t                                      after_switch_label = 0;
	uint32_t                                      continue_label     = 0;
	uint32_t                                      merge_label        = 0;
};

// Slot, value. A terminator's whole phi update is one parallel copy, so every value is read
// before any slot is written - see CollectDispatcherPhiEdge.
using DispatcherPhiCopies = std::vector<std::pair<uint32_t, uint32_t>>;

// Taking an edge updates every phi of `to` at once, and the terminator takes exactly one of its
// edges, so the whole terminator - not one target - is the parallel copy. Two things make the
// distinction load-bearing. One phi's incoming value can be another phi of the same block. And
// one target's phi can take a phi of the *other* target as its incoming value, which is what a
// loop-exit edge reading the header's loop variable looks like: the header and the exit are the
// two arms of the latch's conditional branch, and the exit's phi names the header's. Either way
// the value must be read as it arrived at this block, not after a store has replaced it, so this
// only collects; `StoreDispatcherPhiCopies` writes once the whole terminator has been read.
void CollectDispatcherPhiEdge(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                              const IR::Block* from, const IR::Block* to,
                              DispatcherPhiCopies& copies) {
	if (to == nullptr) {
		return;
	}
	for (const auto& phi: *to) {
		if (phi.GetOpcode() != IR::ValueOpcode::Phi) {
			break;
		}
		for (size_t index = 0; index < phi.NumArgs(); index++) {
			if (phi.PhiBlock(index) == from) {
				copies.emplace_back(dispatcher.spills[ctx.half].at(&phi), ctx.Def(phi.Arg(index)));
				break;
			}
		}
	}
}

void StoreDispatcherPhiCopies(ValueEmitContext& ctx, const DispatcherPhiCopies& copies) {
	for (const auto& [slot, value]: copies) {
		ctx.state.builder.AddFunction(spv::OpStore, slot, value);
	}
}

// KYTY_CS_LOOP_BUDGET: the per-invocation ceiling on loop-header executions, summed over every
// loop in the module. 0, the default, disables the guard and the emitted module is byte-identical
// to an unguarded build. The largest provable per-invocation total over a 186-binary corpus is
// 128, so 65536 leaves ample headroom while still cutting a corrupted grid-stride bound.
uint32_t LoopBudgetLimit() {
	static const uint32_t limit = [] {
		const char* text = std::getenv("KYTY_CS_LOOP_BUDGET");
		if (text == nullptr) {
			return 0u;
		}
		const auto value = std::strtoull(text, nullptr, 0);
		return value > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(value);
	}();
	return limit;
}

// A truncated loop is a wrong picture, so one line per shader names how much the guard covers.
// Compute only: see where loop_budget_limit is set.
void ReportLoopBudget(const EmitterState& state) {
	if (state.loop_budget_limit == 0 ||
	    (state.loop_budget_guarded == 0 && state.loop_budget_unguardable == 0)) {
		return;
	}
	static std::mutex                  mutex;
	static std::unordered_set<uint64_t> seen;
	{
		const std::lock_guard<std::mutex> lock(mutex);
		if (!seen.insert(state.program.shader_hash).second) {
			return;
		}
	}
	std::fprintf(stdout,
	             "LOOPGUARD: shader=0x%016llx budget=%u guarded=%u loops, unguardable=%u - an "
	             "invocation that exhausts the budget returns immediately, dropping the rest of "
	             "its work, not just the rest of the loop\n",
	             static_cast<unsigned long long>(state.program.shader_hash),
	             state.loop_budget_limit, state.loop_budget_guarded,
	             state.loop_budget_unguardable);
	std::fflush(stdout);
}

// Returns "this invocation may keep looping": under budget and, unless `bda_exit` is false, no BDA
// miss in its wave so far, or 0 when there is nothing to check. A wave leaves together: a lane that
// returns alone stays set in the guest's SGPR lane masks.
uint32_t ChargeLoopBudget(EmitterState& state, bool bda_exit) {
	uint32_t allowed = 0;
	if (state.loop_budget_variable != 0) {
		const auto before = state.builder.AllocateId();
		const auto after  = state.builder.AllocateId();
		allowed           = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), before, state.loop_budget_variable);
		state.builder.AddFunction(spv::OpIAdd, TypeU32(state), after, before,
		                          ConstantU32(state, 1));
		state.builder.AddFunction(spv::OpStore, state.loop_budget_variable, after);
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), allowed, after,
		                          ConstantU32(state, state.loop_budget_limit));
	}
	if (state.bda_miss_variable != 0 && bda_exit) {
		const auto missed      = state.builder.AllocateId();
		const auto wave_missed = state.builder.AllocateId();
		const auto served      = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpLoad, TypeBool(state), missed, state.bda_miss_variable);
		state.builder.AddFunction(spv::OpGroupNonUniformAny, TypeBool(state), wave_missed,
		                          ConstantU32(state, spv::ScopeSubgroup), missed);
		state.builder.AddFunction(spv::OpLogicalNot, TypeBool(state), served, wave_missed);
		if (allowed == 0) {
			allowed = served;
		} else {
			const auto both = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(state), both, allowed, served);
			allowed = both;
		}
	}
	return allowed;
}

void EmitReturn(ValueEmitContext& ctx) {
	EmitKillIfPixelValidMaskInactive(ctx.state);
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
			auto       condition = ctx.Def(block->condition);
			const auto latch     = ctx.state.loop_budget_latch.find(block);
			const auto allowed =
			    latch != ctx.state.loop_budget_latch.end()
			        ? ChargeLoopBudget(ctx.state, !ctx.state.loop_bda_exit_exempt.contains(block))
			        : 0u;
			if (allowed != 0) {
				const auto folded = ctx.state.builder.AllocateId();
				if (term.true_block->id == latch->second) {
					// Back edge on the true arm: stay in the loop only while under budget.
					ctx.state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(ctx.state), folded,
					                              condition, allowed);
				} else {
					const auto exhausted = ctx.state.builder.AllocateId();
					ctx.state.builder.AddFunction(spv::OpLogicalNot, TypeBool(ctx.state), exhausted,
					                              allowed);
					ctx.state.builder.AddFunction(spv::OpLogicalOr, TypeBool(ctx.state), folded,
					                              condition, exhausted);
				}
				condition = folded;
			}
			emit_merge();
			ctx.state.builder.AddFunction(spv::OpBranchConditional, condition,
			                              true_block->Definition(), false_block->Definition());
			return;
		}
		default: EmitReturn(ctx); return;
	}
}

void CollectDispatcherTarget(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                             const IR::Block* from, const IR::Block* block,
                             DispatcherPhiCopies& copies) {
	if (block != nullptr) {
		CollectDispatcherPhiEdge(ctx, dispatcher, from, block, copies);
		if (ctx.other_half != nullptr) {
			CollectDispatcherPhiEdge(*ctx.other_half, dispatcher, from, block, copies);
		}
	}
}

// The pc this block hands the loop, and the phi stores that go with taking its edge. Every store
// is emitted last, after the selection has read whatever it needs: the branch condition and the
// indirect selector are themselves values this block may have to load out of a spill slot, and a
// slot a target's phi is about to be given is not the value the terminator was reached with.
uint32_t EmitDispatcherNextPc(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                              const IR::Block* block) {
	const auto&         term = block->terminator;
	DispatcherPhiCopies copies;
	const auto          finish = [&](uint32_t selected) {
		StoreDispatcherPhiCopies(ctx, copies);
		return selected;
	};
	switch (term.kind) {
		case CFG::TerminatorKind::Branch:
			CollectDispatcherTarget(ctx, dispatcher, block, term.true_block, copies);
			return finish(ConstantU32(ctx.state, term.true_block->id));
		case CFG::TerminatorKind::ConditionalBranch: {
			CollectDispatcherTarget(ctx, dispatcher, block, term.true_block, copies);
			CollectDispatcherTarget(ctx, dispatcher, block, term.false_block, copies);
			const auto selected = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpSelect, TypeU32(ctx.state), selected,
			                              ctx.Def(block->condition),
			                              ConstantU32(ctx.state, term.true_block->id),
			                              ConstantU32(ctx.state, term.false_block->id));
			return finish(selected);
		}
		case CFG::TerminatorKind::IndirectBranch: {
			for (const auto* target: block->ImmSuccessors()) {
				CollectDispatcherTarget(ctx, dispatcher, block, target, copies);
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
			return finish(selected);
		}
		default: return finish(ConstantU32(ctx.state, UINT32_MAX));
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
	if (ctx.half != 0 && (inst.GetOpcode() == IR::ValueOpcode::Ballot ||
	                     inst.GetOpcode() == IR::ValueOpcode::ReadFirstLane)) {
		// Both operations already combine both emulated halves into one whole-wave result.
		ctx.Define(inst, ctx.other_half->Result(inst));
		return;
	}
	switch (inst.GetOpcode()) {
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
		structured.deferred_phis.push_back(
		    {ctx.state.builder.AddDeferredPhi(type, ctx.Result(inst), inst.NumArgs()), &inst,
		     ctx.half});
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
void EmitBlock(ValueEmitContext& ctx, const IR::Block* block, EmitInstruction&& emit_instruction) {
	ctx.state.current_block = block;
	EmitLabel(ctx.state, block->Definition());
	bool emitted_non_phi = false;
	for (const auto& inst: *block) {
		if (ctx.state.Refused()) {
			return;
		}
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
			                  inst.GetOpcode() != IR::ValueOpcode::MeshAllocate)) {
				emit_instruction(lane, inst);
			}
		}
		ctx.state.lane_half = 0;
	}
}

void PatchStructuredPhis(ValueEmitContext& ctx, StructuredFunctionState& structured) {
	for (const auto& deferred: structured.deferred_phis) {
		auto& lane = deferred.half == 0 ? ctx : *ctx.other_half;
		for (size_t index = 0; index < deferred.instruction->NumArgs(); index++) {
			const auto* predecessor = deferred.instruction->PhiBlock(index);
			const auto  found       = structured.block_exit_labels.find(predecessor);
			if (found == structured.block_exit_labels.end()) {
				ctx.Fail(*deferred.instruction, "has a predecessor that was not emitted");
			}
			ctx.state.builder.PatchDeferredPhi(
			    deferred.phi, index, lane.Def(deferred.instruction->Arg(index)), found->second);
		}
	}
}

// A loop with an atomic in it may be how invocations hand work to each other, like a persistent-
// threads queue whose idle workgroups spin until the last worker signs off. A worker that leaves
// after a BDA miss never signs off, so its loop must not take the BDA exit. The body runs from the
// header up to the merge block in structured order; without a merge after the header, assume one.
size_t LoopEnd(const IR::Program& program, size_t header_index) {
	const auto* merge_block = program.blocks[header_index]->terminator.merge_block;
	const auto  merge       = std::ranges::find(program.blocks, merge_block);
	const auto  end         = static_cast<size_t>(merge - program.blocks.begin());
	return merge_block != nullptr && merge != program.blocks.end() && end > header_index ? end : 0;
}

bool LoopHasAtomics(const IR::Program& program, size_t header_index) {
	const auto end = LoopEnd(program, header_index);
	if (end == 0) {
		return true;
	}
	for (size_t index = header_index; index < end && index < program.blocks.size(); index++) {
		for (const auto& inst: *program.blocks[index]) {
			const auto op     = inst.GetOpcode();
			const auto shared = IR::SharedAccessOf(op);
			if (IR::BufferAccessOf(op) == IR::BufferAccess::Atomic ||
			    IR::ImageOpcodeInfoOf(op).access == IR::ImageAccess::Atomic ||
			    shared == IR::SharedAccess::Atomic || shared == IR::SharedAccess::Append ||
			    shared == IR::SharedAccess::Consume) {
				return true;
			}
		}
	}
	return false;
}

// The exit returns from the whole invocation, so it also skips every loop around this one.
bool LoopNestHasAtomics(const IR::Program& program, size_t header_index) {
	for (size_t index = 0; index <= header_index; index++) {
		if (!program.blocks[index]->terminator.loop_header) {
			continue;
		}
		const auto end      = LoopEnd(program, index);
		const bool encloses = index == header_index || (end != 0 && header_index < end);
		if (encloses && LoopHasAtomics(program, index)) {
			return true;
		}
	}
	return false;
}

// Where the budget is charged: not the loop header, the block the header branches into. A header
// may only reach the body or the loop merge, and re-pointing either arm produces a module
// spirv-val rejects. The body block is entered once per iteration, a structured selection inside
// it is ordinary, and the early exit leaves through OpReturn, which adds no edge at all.
//
// A body block that is itself a loop header is skipped: splitting it would move its OpLoopMerge
// out of the block the back edge targets.
std::unordered_set<const IR::Block*> LoopChargeSites(ValueEmitContext& ctx) {
	std::unordered_set<const IR::Block*> sites;
	if (ctx.state.loop_budget_variable == 0 && ctx.state.bda_miss_variable == 0) {
		return sites;
	}
	const auto& program = ctx.state.program;
	// A continue target must be structurally post-dominated by its back-edge block, so splitting
	// one puts an OpReturn inside the continue construct that never reaches the back edge.
	std::unordered_set<const IR::Block*> continue_targets;
	for (const auto* block: program.blocks) {
		if (block->terminator.loop_header) {
			continue_targets.insert(block->terminator.continue_block);
		}
	}
	for (size_t index = 0; index < program.blocks.size(); index++) {
		const auto* header = program.blocks[index];
		const auto& term   = header->terminator;
		if (!term.loop_header) {
			continue;
		}
		if (term.true_block != nullptr && continue_targets.contains(term.true_block)) {
			// A tight loop whose body IS the latch cannot be split, but its latch already branches
			// to exactly (header, merge), so the budget folds into that test.
			const auto* latch      = term.true_block;
			const auto& latch_term = latch->terminator;
			const bool  foldable =
			    latch_term.kind == CFG::TerminatorKind::ConditionalBranch &&
			    ((latch_term.true_block == header && latch_term.false_block == term.merge_block) ||
			     (latch_term.false_block == header && latch_term.true_block == term.merge_block));
			if (foldable && ctx.state.loop_budget_latch.emplace(latch, header->id).second) {
				ctx.state.loop_budget_guarded++;
				if (LoopNestHasAtomics(program, index)) {
					ctx.state.loop_bda_exit_exempt.insert(latch);
				}
			} else if (!foldable) {
				ctx.state.loop_budget_unguardable++;
			}
			continue;
		}
		const auto* body = term.kind == CFG::TerminatorKind::Branch ? term.true_block : nullptr;
		if (body == nullptr || body == header) {
			ctx.state.loop_budget_unguardable++;
			continue;
		}
		if (body->terminator.loop_header) {
			ctx.state.loop_budget_unguardable++;
			continue;
		}
		if (sites.insert(body).second) {
			ctx.state.loop_budget_guarded++;
			if (LoopNestHasAtomics(program, index)) {
				ctx.state.loop_bda_exit_exempt.insert(body);
			}
		}
	}
	return sites;
}

void EmitLoopCharge(ValueEmitContext& ctx, const IR::Block* site) {
	const auto allowed =
	    ChargeLoopBudget(ctx.state, !ctx.state.loop_bda_exit_exempt.contains(site));
	if (allowed == 0) {
		return;
	}
	const auto abort   = ctx.state.builder.AllocateId();
	const auto rest    = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpSelectionMerge, rest, spv::SelectionControlMaskNone);
	ctx.state.builder.AddFunction(spv::OpBranchConditional, allowed, rest, abort);
	EmitLabel(ctx.state, abort);
	ctx.state.builder.AddFunction(spv::OpReturn);
	EmitLabel(ctx.state, rest);
}

void EmitStructuredFunction(ValueEmitContext& ctx) {
	const auto& program = ctx.state.program;
	StructuredFunctionState structured;
	const auto charge_sites = LoopChargeSites(ctx);
	ctx.state.builder.AddFunction(spv::OpBranch, program.blocks.front()->Definition());
	for (size_t index = 0; index < program.blocks.size(); index++) {
		const auto* block = program.blocks[index];
		EmitBlock(ctx, block, [&](ValueEmitContext& lane, const IR::Inst& inst) {
			EmitStructuredInstruction(lane, structured, inst);
		});
		if (ctx.state.Refused()) {
			return;
		}
		if (charge_sites.contains(block)) {
			EmitLoopCharge(ctx, block);
		}
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
	if (state.Refused()) {
		return;
	}
	const auto initial_pc     = EmitDispatcherNextPc(ctx, dispatcher, entry);
	const auto initial_parent = state.current_label;
	state.builder.AddFunction(spv::OpBranch, dispatcher.header_label);

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
		if (state.Refused()) {
			return;
		}
		const auto selected = EmitDispatcherNextPc(ctx, dispatcher, state.program.blocks[index]);
		next_pc_words.push_back(selected);
		next_pc_words.push_back(state.current_label);
		state.builder.AddFunction(spv::OpBranch, dispatcher.after_switch_label);
	}
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
		case IR::Type::U32:
		case IR::Type::F16: return TypeU32(state);
		case IR::Type::U64: return TypeU64(state);
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
			case IR::Type::F16: return ConstantU32(state, value.F16Bits());
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

// The lane a wave-wide read takes its value from.
//
// v_readfirstlane_b32 writes its scalar destination whatever EXEC is, and RDNA 2 names lane 0 when
// no lane is active. Neither operation underneath says that: OpGroupNonUniformBallotFindLSB is
// undefined on an empty ballot, and GLSL.std.450 FindILsb answers -1, which the `+ 32` below turns
// into lane 31. Name lane 0 for the empty ballot instead of inheriting either answer.
uint32_t ValueEmitContext::FirstLane(uint32_t ballot) {
	const auto low  = state.builder.AllocateId();
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, ballot, 0);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, ballot, 1);
	// A subgroup here is never wider than 64 lanes, so those two words hold every lane.
	const auto any_active =
	    Binary(state, spv::OpINotEqual, TypeBool(state),
	           Binary(state, spv::OpBitwiseOr, TypeU32(state), low, high), ConstantU32(state, 0));
	if (other_half == nullptr) {
		const auto found = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformBallotFindLSB, TypeU32(state), found,
		                          ConstantU32(state, spv::ScopeSubgroup), ballot);
		return Select(state, TypeU32(state), any_active, found, ConstantU32(state, 0));
	}
	const auto low_first  = state.builder.AllocateId();
	const auto high_first = state.builder.AllocateId();
	const auto low_active = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeU32(state), low_first, GlslStd450(state),
	                          GLSLstd450FindILsb, low);
	state.builder.AddFunction(spv::OpExtInst, TypeU32(state), high_first, GlslStd450(state),
	                          GLSLstd450FindILsb, high);
	state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), low_active, low,
	                          ConstantU32(state, 0));
	const auto upper =
	    Select(state, TypeU32(state), any_active,
	           EmitAddU32(state, high_first, ConstantU32(state, 32)), ConstantU32(state, 0));
	return Select(state, TypeU32(state), low_active, low_first, upper);
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

uint32_t ValueEmitContext::Refuse(const IR::Inst& inst, const char* reason) const {
	state.Refuse(reason, &inst);
	return 0;
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
	if (state.program.stage == ShaderType::Pixel && state.input_info.pixel != nullptr &&
	    state.input_info.pixel->ps_stencil_bit_pass != 0) {
		if (state.input_info.pixel->ps_stencil_bit_pass > 8 || state.lane_count != 1) {
			state.Refuse("stencil replay variant needs a bit index and one lane per invocation");
		} else {
			state.stencil_bit_pass_variable = state.builder.AllocateId();
			state.builder.AddName(state.stencil_bit_pass_variable, "stencil_bit_pass_keep");
		}
	}
	// The guard's exit is a bare OpReturn, so outside compute it abandons the invocation rather
	// than truncating the loop.
	state.loop_budget_limit =
	    state.program.stage == ShaderType::Compute ? LoopBudgetLimit() : 0u;
	const bool has_loops = std::ranges::any_of(
	    program.blocks, [](const IR::Block* block) { return block->terminator.loop_header; });
	if (state.loop_budget_limit != 0 && has_loops) {
		state.loop_budget_variable = state.builder.AllocateId();
		state.builder.AddName(state.loop_budget_variable, "loop_budget");
	}
	if (state.program.stage == ShaderType::Compute && state.program.info.uses_dma && has_loops) {
		state.bda_miss_variable = state.builder.DefineGlobalVariable(
		    TypePointer(state, spv::StorageClassPrivate, TypeBool(state)),
		    spv::StorageClassPrivate);
		state.builder.AddName(state.bda_miss_variable, "bda_miss");
		state.builder.RequireCapability(spv::CapabilityGroupNonUniform);
		state.builder.RequireCapability(spv::CapabilityGroupNonUniformVote);
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
		for (const auto* block: program.blocks) {
			for (const auto& inst: *block) {
				if (inst.GetOpcode() != IR::ValueOpcode::Phi) {
					continue;
				}
				if (SpillPointerType(ctx, inst.GetType()) == 0) {
					ctx.Fail(inst, "cannot be stored by the dispatcher");
					break;
				}
				dispatch.spills[0].emplace(&inst, state.builder.AllocateId());
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
				dispatch.spills[0].emplace(definition, state.builder.AllocateId());
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
		if (state.lane_count == 2) {
			// In the low half's id order; the map is keyed by address, which varies per process.
			std::vector<std::pair<uint32_t, const IR::Inst*>> low_half;
			low_half.reserve(dispatch.spills[0].size());
			for (const auto& [inst, id]: dispatch.spills[0]) {
				low_half.emplace_back(id, inst);
			}
			std::ranges::sort(low_half);
			for (const auto& [id, inst]: low_half) {
				dispatch.spills[1].emplace(inst, state.builder.AllocateId());
			}
			high.dispatcher_spills = &dispatch.spills[1];
		}
	}
	DefineGetBdaPointer(state);
	DefineBvhIntersect(state);
	if (program.info.uses_swizzle) {
		ctx.scratch_u32_variable = state.builder.AllocateId();
		if (state.lane_count == 2) {
			high.scratch_u32_variable = state.builder.AllocateId();
		}
	}
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state),
	                          state.mesh_guest_func != 0 ? state.mesh_guest_func : state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.entry_label);
	if (state.program.info.function_lds) {
		state.builder.AddFunction(
		    spv::OpVariable,
		    TypeU32ArrayPointer(state, spv::StorageClassFunction, LdsDwordCount(state)),
		    state.lds_variable, spv::StorageClassFunction);
	}
	if (state.program.info.function_scratch) {
		for (uint32_t half = 0; half < state.lane_count; half++) {
			state.builder.AddFunction(
			    spv::OpVariable,
			    TypeU32ArrayPointer(state, spv::StorageClassFunction, state.program.scratch_dwords),
			    state.scratch_variable[half], spv::StorageClassFunction);
		}
	}
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction(spv::OpVariable,
		                          TypePointer(state, spv::StorageClassFunction, TypeU32(state)),
		                          state.pixel_valid_mask_variable, spv::StorageClassFunction);
	}
	if (state.stencil_bit_pass_variable != 0) {
		state.builder.AddFunction(spv::OpVariable,
		                          TypePointer(state, spv::StorageClassFunction, TypeU32(state)),
		                          state.stencil_bit_pass_variable, spv::StorageClassFunction);
	}
	if (state.loop_budget_variable != 0) {
		state.builder.AddFunction(spv::OpVariable,
		                          TypePointer(state, spv::StorageClassFunction, TypeU32(state)),
		                          state.loop_budget_variable, spv::StorageClassFunction);
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
		if (lane.scratch_u32_variable != 0) {
			state.builder.AddFunction(spv::OpVariable,
			                          TypePointer(state, spv::StorageClassFunction, TypeU32(state)),
			                          lane.scratch_u32_variable, spv::StorageClassFunction);
		}
	}
	if (state.gds_variable != 0) {
		state.gds_length = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpArrayLength, TypeU32(state), state.gds_length,
		                          state.gds_variable, 0);
	}
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction(spv::OpStore, state.pixel_valid_mask_variable,
		                          ConstantU32(state, 1));
	}
	if (state.stencil_bit_pass_variable != 0) {
		state.builder.AddFunction(spv::OpStore, state.stencil_bit_pass_variable,
		                          ConstantU32(state, 0));
	}
	if (state.loop_budget_variable != 0) {
		state.builder.AddFunction(spv::OpStore, state.loop_budget_variable, ConstantU32(state, 0));
	}
	if (state.bda_miss_variable != 0) {
		state.builder.AddFunction(spv::OpStore, state.bda_miss_variable,
		                          ConstantBool(state, false));
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
	EmitMemoryOffsets(state);
	if (program.blocks.empty()) {
		EmitReturn(ctx);
	} else if (state.program.dispatcher_fallback) {
		EmitDispatcherFunction(ctx, *dispatcher);
	} else {
		EmitStructuredFunction(ctx);
	}
	if (state.Refused()) {
		return;
	}
	state.builder.AddFunction(spv::OpFunctionEnd);
	if (state.program.stage == ShaderType::Mesh) {
		EmitMeshEntryPoint(state);
	}
	ReportLoopBudget(state);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
