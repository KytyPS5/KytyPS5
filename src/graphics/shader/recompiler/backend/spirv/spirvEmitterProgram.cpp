#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include "common/assert.h"

#include <algorithm>
#include <bit>
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
	const auto inactive    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLogicalNot, TypeBool(state), inactive, active);
	state.builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, inactive, kill_label, merge_label);
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

struct DeferredPhiPatch {
	DeferredPhi     phi;
	const IR::Inst* instruction = nullptr;
	uint32_t        half        = 0;
};

struct DeferredContinuePatch {
	DeferredLoopMerge loop;
	const IR::Block*  body = nullptr;
};

struct StructuredFunctionState {
	std::unordered_map<const IR::Block*, uint32_t> dedicated_continues;
	std::unordered_map<const IR::Block*,
	                   std::pair<const IR::Block*, const IR::Block*>>
	    budgeted_loop_continues;
	std::vector<DeferredContinuePatch>             deferred_continues;
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

const IR::Block* TargetBlock(const IR::Program& program, uint32_t id) {
	const auto found = std::ranges::find_if(
	    program.block_info, [&](const IR::BlockInfo& info) { return info.id == id; });
	if (found == program.block_info.end()) {
		return nullptr;
	}
	return program.blocks[static_cast<size_t>(found - program.block_info.begin())];
}

// Move only a proved simple while-loop's SPIR-V continue target past its
// guest body. Generated bounds checks in that body can terminate invalid paths
// with OpUnreachable; those paths must not belong to the continue construct.
// More general early-continue/nested-selection graphs retain their existing
// targets: moving them requires restructuring the selection exits as well.
const IR::Block* DedicatedContinueBody(const IR::Program& program, const IR::Block* header,
                                       const IR::BlockInfo& header_info) {
	const auto& loop = header_info.terminator;
	if (!loop.loop_header) {
		return nullptr;
	}
	const auto* merge = TargetBlock(program, loop.merge_block);
	const auto* body = TargetBlock(program, loop.continue_block);
	if (merge == nullptr || body == nullptr || merge == header || body == header || merge == body) {
		return nullptr;
	}
	const auto info_for = [&](const IR::Block* block) -> const IR::BlockInfo* {
		const auto found = std::ranges::find(program.blocks, block);
		return found == program.blocks.end() ? nullptr :
		       &program.block_info[static_cast<size_t>(found - program.blocks.begin())];
	};
	const auto* body_info = info_for(body);
	if (body_info == nullptr || body_info->terminator.loop_header ||
	    body_info->terminator.kind != CFG::TerminatorKind::Branch ||
	    TargetBlock(program, body_info->terminator.true_block) != header) {
		return nullptr;
	}
	std::unordered_set<const IR::Block*> visited;
	const auto* guard = header;
	for (;;) {
		if (guard == body || guard == merge || !visited.insert(guard).second) {
			return nullptr;
		}
		const auto* info = info_for(guard);
		if (info == nullptr || (guard != header && info->terminator.loop_header)) {
			return nullptr;
		}
		const auto& term = info->terminator;
		if (term.kind == CFG::TerminatorKind::ConditionalBranch) {
			const auto* on_true = TargetBlock(program, term.true_block);
			const auto* on_false = TargetBlock(program, term.false_block);
			if (info->condition.IsEmpty() ||
			    (guard != header && term.merge_block != UINT32_MAX) ||
			    !((on_true == body && on_false == merge) || (on_true == merge && on_false == body))) {
				return nullptr;
			}
			const auto predecessors = body->ImmPredecessors();
			return predecessors.size() == 1u && predecessors.front() == guard ? body : nullptr;
		}
		if (term.kind != CFG::TerminatorKind::Branch) {
			return nullptr;
		}
		const auto* next = TargetBlock(program, term.true_block);
		if (next == nullptr || next->ImmPredecessors().size() != 1u ||
		    next->ImmPredecessors().front() != guard) {
			return nullptr;
		}
		guard = next;
	}
}

void EmitReturn(ValueEmitContext& ctx) {
	EmitKillIfPixelValidMaskInactive(ctx.state);
	ctx.state.builder.AddFunction(spv::OpReturn);
}

uint32_t BranchCondition(ValueEmitContext& ctx, const IR::BlockInfo& info) {
	// Scalar-instruction conditions already test the full wave's raw register values.
	if (ctx.other_half == nullptr ||
	    info.terminator.condition == CFG::BranchCondition::ScalarInstruction ||
	    info.terminator.condition == CFG::BranchCondition::GotoVariable) {
		return ctx.Def(info.condition);
	}
	const auto ballot = ctx.Ballot(info.condition);
	const auto low    = ctx.state.builder.AllocateId();
	const auto high   = ctx.state.builder.AllocateId();
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state), low, ballot, 0);
	ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state), high, ballot, 1);
	const auto kind     = info.terminator.condition;
	const bool zero     = kind == CFG::BranchCondition::ExecZero ||
	                      kind == CFG::BranchCondition::VccZero ||
	                      kind == CFG::BranchCondition::SccZero;
	const auto combined =
	    EmitBinaryU32(ctx.state, zero ? spv::OpBitwiseAnd : spv::OpBitwiseOr, low, high);
	ctx.state.builder.AddFunction(zero ? spv::OpIEqual : spv::OpINotEqual, TypeBool(ctx.state),
	                              result, combined, ConstantU32(ctx.state, zero ? ~0u : 0u));
	return result;
}

uint32_t EmitGraphicsLoopWithinBudget(ValueEmitContext& ctx) {
	auto& state = ctx.state;
	EXIT_IF(state.graphics_loop_counter_variable == 0);
	constexpr uint32_t MaxGraphicsLoopIterations = 256;
	const auto counter = state.builder.AllocateId();
	const auto within  = state.builder.AllocateId();
	const auto next    = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpLoad, TypeU32(state), counter, state.graphics_loop_counter_variable});
	state.builder.AddFunction({OpULessThan, TypeBool(state), within, counter,
	                           ConstantU32(state, MaxGraphicsLoopIterations)});
	state.builder.AddFunction(
	    {OpIAdd, TypeU32(state), next, counter, ConstantU32(state, 1)});
	state.builder.AddFunction({OpStore, state.graphics_loop_counter_variable, next});
	return within;
}

void EmitStructuredTerminator(ValueEmitContext& ctx, StructuredFunctionState& structured, const IR::Block* block,
                              const IR::BlockInfo& info) {
	const auto& program = ctx.state.program;
	const auto& term       = info.terminator;
	const auto  emit_merge = [&]() {
		if (term.loop_header) {
			const auto* merge = TargetBlock(program, term.merge_block);
			const auto* cont  = TargetBlock(program, term.continue_block);
			if (merge != nullptr && cont != nullptr) {
				const auto bridge = structured.dedicated_continues.find(cont);
				const auto continue_label = bridge == structured.dedicated_continues.end()
				                                ? ctx.Label(cont) : bridge->second;
				ctx.state.builder.AddFunction(spv::OpLoopMerge, ctx.Label(merge), continue_label,
				                              spv::LoopControlMaskNone);
			}
		} else if (term.kind == CFG::TerminatorKind::ConditionalBranch &&
		           term.merge_block != UINT32_MAX) {
			if (const auto* merge = TargetBlock(program, term.merge_block); merge != nullptr) {
				ctx.state.builder.AddFunction(spv::OpSelectionMerge, ctx.Label(merge),
				                              spv::SelectionControlMaskNone);
			}
		}
	};

	switch (term.kind) {
		case CFG::TerminatorKind::Branch: {
			const auto* target = TargetBlock(program, term.true_block);
			if (target == nullptr) {
				EmitReturn(ctx);
				return;
			}
			uint32_t         within = 0;
			const IR::Block* merge  = nullptr;
			if (const auto loop = structured.budgeted_loop_continues.find(block);
			    loop != structured.budgeted_loop_continues.end() && target == loop->second.first) {
				within = EmitGraphicsLoopWithinBudget(ctx);
				merge  = loop->second.second;
			}
			emit_merge();
			if (within != 0) {
			 ctx.state.builder.AddFunction(spv::OpBranchConditional, within, ctx.Label(target), ctx.Label(merge));
			 return;
			}
			ctx.state.builder.AddFunction(spv::OpBranch, ctx.Label(target));
			return;
		}
		case CFG::TerminatorKind::ConditionalBranch: {
			const auto* true_block  = TargetBlock(program, term.true_block);
			const auto* false_block = TargetBlock(program, term.false_block);
			if (true_block == nullptr || false_block == nullptr || info.condition.IsEmpty()) {
				EmitReturn(ctx);
				return;
			}
			auto condition = BranchCondition(ctx, info);
			if (const auto loop = structured.budgeted_loop_continues.find(block);
			    loop != structured.budgeted_loop_continues.end()) {
				const auto* header = loop->second.first;
				const auto* merge  = loop->second.second;
				const auto  within = EmitGraphicsLoopWithinBudget(ctx);
				if (true_block == header && false_block == merge) {
					const auto guarded = ctx.state.builder.AllocateId();
					ctx.state.builder.AddFunction(
					    {OpLogicalAnd, TypeBool(ctx.state), guarded, condition, within});
					condition = guarded;
				} else if (true_block == merge && false_block == header) {
					const auto outside = ctx.state.builder.AllocateId();
					ctx.state.builder.AddFunction(
					    {OpLogicalNot, TypeBool(ctx.state), outside, within});
					const auto guarded = ctx.state.builder.AllocateId();
					ctx.state.builder.AddFunction(
					    {OpLogicalOr, TypeBool(ctx.state), guarded, condition, outside});
					condition = guarded;
				}
			}
			emit_merge();
			ctx.state.builder.AddFunction(spv::OpBranchConditional, condition,
			                              ctx.Label(true_block), ctx.Label(false_block));
			return;
		}
		default: EmitReturn(ctx); return;
	}
}

void EmitDispatcherTarget(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                          const IR::Block* from, uint32_t target) {
	const auto* block = TargetBlock(ctx.state.program, target);
	if (block != nullptr) {
		StoreDispatcherPhiEdge(ctx, dispatcher, from, block);
		if (ctx.other_half != nullptr) {
			StoreDispatcherPhiEdge(*ctx.other_half, dispatcher, from, block);
		}
	}
}

uint32_t EmitDispatcherNextPc(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                              const IR::Block* block, const IR::BlockInfo& info) {
	const auto& term = info.terminator;
	switch (term.kind) {
		case CFG::TerminatorKind::Branch:
			EmitDispatcherTarget(ctx, dispatcher, block, term.true_block);
			return ConstantU32(ctx.state, term.true_block);
		case CFG::TerminatorKind::ConditionalBranch: {
			EmitDispatcherTarget(ctx, dispatcher, block, term.true_block);
			EmitDispatcherTarget(ctx, dispatcher, block, term.false_block);
			const auto selected = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(
			    spv::OpSelect, TypeU32(ctx.state), selected, BranchCondition(ctx, info),
			    ConstantU32(ctx.state, term.true_block), ConstantU32(ctx.state, term.false_block));
			return selected;
		}
		case CFG::TerminatorKind::IndirectBranch: {
			for (const auto target: term.indirect_targets) {
				EmitDispatcherTarget(ctx, dispatcher, block, target);
			}
			uint32_t selected = ConstantU32(ctx.state, UINT32_MAX);
			if (!info.indirect_target.IsEmpty()) {
				const auto  selector = ctx.Def(info.indirect_target);
				const auto& values   = term.indirect_selector_code != UINT32_MAX
				                           ? term.indirect_selector_values
				                           : term.indirect_target_pcs;
				const auto& targets  = term.indirect_selector_code != UINT32_MAX
				                           ? term.indirect_selector_targets
				                           : term.indirect_targets;
				for (size_t index = 0; index < std::min(values.size(), targets.size()); index++) {
					const auto match = ctx.state.builder.AllocateId();
					const auto next  = ctx.state.builder.AllocateId();
					ctx.state.builder.AddFunction(spv::OpIEqual, TypeBool(ctx.state), match,
					                              selector, ConstantU32(ctx.state, values[index]));
					ctx.state.builder.AddFunction(spv::OpSelect, TypeU32(ctx.state), next, match,
					                              ConstantU32(ctx.state, targets[index]), selected);
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

void EmitRawDirectInstruction(ValueEmitContext& ctx, const IR::Inst& inst) {
	switch (inst.GetOpcode()) {
#define VALUE_OPCODE(name, ...)                                                                    \
	case IR::ValueOpcode::name: Invoke(Emit##name, ctx, inst); break;
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.inc"
#undef VALUE_OPCODE
		default: ctx.Fail(inst, "has no direct SPIR-V emitter");
	}
}

// A bounded descriptor is a runtime choice among fully specialized native
// buffers. Emit each access against its own descriptor metadata so formats,
// strides, bounds, and byte offsets remain tied to the selected resource.
bool EmitBoundedBufferMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	if (IR::BufferAccessOf(inst.GetOpcode()) == IR::BufferAccess::None) return false;
	const auto memory = ctx.Memory(inst);
	if (memory.buffer_table == UINT32_MAX) return false;
	auto& state = ctx.state;
	if (memory.buffer_table >= state.program.info.buffer_tables.size()) {
		ctx.Fail(inst, "bounded buffer table specialization is missing");
	}
	const auto& table = state.program.info.buffer_tables[memory.buffer_table];
	const bool has_result = inst.GetType() != IR::Type::Void;
	if (table.count == 0) {
		if (!table.resources.empty()) ctx.Fail(inst, "empty bounded buffer table retained candidates");
		if (has_result) ctx.Define(inst, state.builder.Constant(OpUndef, TypeId(state, inst.GetType()), {}));
		return true;
	}
	const auto* handle = inst.Arg(0).ResolveInstruction();
	if (handle == nullptr || handle->GetOpcode() != IR::ValueOpcode::GetBufferResource ||
	    table.resources.empty()) {
		ctx.Fail(inst, "bounded buffer table has no index or typed candidates");
	}
	const auto selected = EmitBoundedFlatWord(state, ctx.Arg(*handle, 0), table.count,
	                                          table.mapping_flat_offset);
	const auto merge_label = state.builder.AllocateId();
	const auto invalid_label = state.builder.AllocateId();
	const auto result = has_result ? ctx.Result(inst) : 0u;
	std::vector<uint32_t> labels;
	std::vector<uint32_t> switch_words {OpSwitch, selected, invalid_label};
	for (const auto resource: table.resources) {
		if (resource >= state.program.info.buffers.size() ||
		    std::count(table.resources.begin(), table.resources.end(), resource) != 1) {
			ctx.Fail(inst, "bounded buffer table contains an invalid candidate");
		}
		labels.push_back(state.builder.AllocateId());
		switch_words.push_back(resource);
		switch_words.push_back(labels.back());
	}
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction(switch_words);
	EmitLabel(state, invalid_label);
	state.builder.AddFunction({OpUnreachable});
	std::vector<uint32_t> phi {OpPhi, has_result ? TypeId(state, inst.GetType()) : 0u, result};
	const auto* previous_inst = ctx.memory_override_inst;
	const auto* previous_memory = ctx.memory_override;
	for (size_t candidate = 0; candidate < table.resources.size(); ++candidate) {
		EmitLabel(state, labels[candidate]);
		auto specialized = memory;
		specialized.resource = table.resources[candidate];
		specialized.buffer_table = UINT32_MAX;
		ctx.memory_override_inst = &inst;
		ctx.memory_override = &specialized;
		ctx.definitions.erase(&inst);
		EmitRawDirectInstruction(ctx, inst);
		if (has_result) {
			phi.push_back(ctx.definitions.at(&inst));
			phi.push_back(state.current_label);
		}
		state.builder.AddFunction({OpBranch, merge_label});
	}
	ctx.memory_override_inst = previous_inst;
	ctx.memory_override = previous_memory;
	ctx.definitions.erase(&inst);
	EmitLabel(state, merge_label);
	if (has_result) {
		state.builder.AddFunction(phi);
		ctx.definitions.emplace(&inst, result);
	}
	return true;
}

void EmitDirectInstruction(ValueEmitContext& ctx, const IR::Inst& inst) {
	if (!EmitBoundedBufferMemory(ctx, inst)) EmitRawDirectInstruction(ctx, inst);
		const auto shared_access = IR::SharedAccessOf(inst.GetOpcode());
		const auto buffer_access = IR::BufferAccessOf(inst.GetOpcode());
		const auto address_access = IR::AddressOpcodeInfoOf(inst.GetOpcode()).access;
		const auto image_access = IR::ImageOpcodeInfoOf(inst.GetOpcode()).access;
		if (ctx.state.compute_execution.IsSplitWave64() &&
		    !ctx.state.compute_execution.IsCooperativeWave64() &&
		    shared_access != IR::SharedAccess::None) {
			// The planner admits LDS only when one complete guest wave remains
			// one host workgroup and every branch is wave-uniform. Finish each
			// DS instruction across both native halves, including reads before
			// a later overwrite. EXEC and bounds guards have merged at this point,
			// so inactive lanes also reach the barrier. Guest LDS and collective
			// scratch are distinct Workgroup variables covered by these semantics.
			auto& state = ctx.state;
			state.builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
			                           ConstantU32(state, ScopeWorkgroup), ConstantU32(state,
			                           MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory)});
		}
		bool planning_only = false;
		if (inst.GetOpcode() == IR::ValueOpcode::LoadAddressU32 ||
		    inst.GetOpcode() == IR::ValueOpcode::ReadConstBuffer) {
			const auto index = inst.Flags<IR::MemoryFlags>().index;
			planning_only = index < ctx.state.program.memory_info.size() &&
			                ctx.state.program.memory_info[index].planning_only;
		}
		// Sampled fetches and other reads consume already-published values.
		// Attaching ImageMemory control barriers to OpImageFetch inside loops
		// is not required for split-half write→read order and aborts some
		// native32 compilers. Rendezvous only after operations that publish.
		const bool publishes_external =
		    buffer_access == IR::BufferAccess::Write ||
		    buffer_access == IR::BufferAccess::Atomic ||
		    address_access == IR::AddressAccess::Write ||
		    image_access == IR::ImageAccess::Write ||
		    image_access == IR::ImageAccess::Atomic;
		if (ctx.state.compute_execution.SynchronizesSplitWaveMemory() &&
		    publishes_external && !planning_only) {
			auto& state = ctx.state;
			uint32_t semantics = MemorySemanticsAcquireRelease;
			if (buffer_access == IR::BufferAccess::Write ||
			    buffer_access == IR::BufferAccess::Atomic ||
			    address_access == IR::AddressAccess::Write) {
				semantics |= MemorySemanticsUniformMemory;
			}
			if (image_access == IR::ImageAccess::Write ||
			    image_access == IR::ImageAccess::Atomic) {
				semantics |= MemorySemanticsImageMemory;
			}
			state.builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
			                           ConstantU32(state, ScopeDevice),
			                           ConstantU32(state, semantics)});
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
			if (predecessor == nullptr || !ctx.state.labels.contains(predecessor)) {
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
	EmitLabel(ctx.state, ctx.Label(block));
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

void EmitStructuredFunction(ValueEmitContext& ctx) {
	const auto& program = ctx.state.program;
	StructuredFunctionState structured;
	for (size_t index = 0; index < ctx.program.blocks.size(); index++) {
		const auto& info = ctx.program.block_info[index];
		if (const auto* body = DedicatedContinueBody(ctx.program, ctx.program.blocks[index], info);
		    body != nullptr) {
			structured.dedicated_continues.emplace(body, ctx.state.builder.AllocateId());
		}
		if (ctx.state.graphics_loop_counter_variable != 0 && info.terminator.loop_header) {
			const auto* header = ctx.program.blocks[index];
			const auto* merge  = TargetBlock(ctx.program, info.terminator.merge_block);
			const auto* cont   = TargetBlock(ctx.program, info.terminator.continue_block);
			if (merge != nullptr && cont != nullptr) {
				structured.budgeted_loop_continues.emplace(cont, std::pair {header, merge});
			}
		}
	}
	ctx.state.builder.AddFunction(spv::OpBranch, ctx.Label(program.blocks.front()));
	for (size_t index = 0; index < program.blocks.size(); index++) {
		const auto* block = program.blocks[index];
		EmitBlock(ctx, block, [&](ValueEmitContext& lane, const IR::Inst& inst) {
			EmitStructuredInstruction(lane, structured, inst);
		});
		if (const auto bridge = structured.dedicated_continues.find(block);
		    bridge != structured.dedicated_continues.end()) {
			// A generated selection/loop merge cannot also be this loop's
			// continue target: its construct must remain inside the loop body.
			ctx.state.builder.AddFunction({OpBranch, bridge->second});
			EmitLabel(ctx.state, bridge->second);
		}
		// Header Phis must name the actual back-edge label after the body and
		// its generated selections, including a dedicated continue bridge when needed.
		structured.block_exit_labels.emplace(block, ctx.state.current_label);
		EmitStructuredTerminator(ctx, structured, block, program.block_info[index]);
	}
	PatchStructuredPhis(ctx, structured);
}

void EmitDispatcherFunction(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher) {
	auto&       state = ctx.state;
	const auto* entry = state.program.blocks.front();
	state.builder.AddFunction(spv::OpBranch, ctx.Label(entry));
	EmitBlock(ctx, entry, [&](ValueEmitContext& lane, const IR::Inst& inst) {
		EmitDispatcherInstruction(lane, dispatcher, inst);
	});
	const auto initial_pc =
	    EmitDispatcherNextPc(ctx, dispatcher, entry, state.program.block_info.front());
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
		words.push_back(state.program.block_info[index].id);
		words.push_back(ctx.Label(state.program.blocks[index]));
	}
	state.builder.AddFunction(words);
	std::vector<uint32_t> next_pc_words {spv::OpPhi, TypeU32(state), next_pc,
	                                     ConstantU32(state, UINT32_MAX), dispatcher.select_label};

	for (size_t index = 1; index < state.program.blocks.size(); index++) {
		EmitBlock(ctx, state.program.blocks[index],
		          [&](ValueEmitContext& lane, const IR::Inst& inst) {
			          EmitDispatcherInstruction(lane, dispatcher, inst);
		          });
		const auto selected = EmitDispatcherNextPc(ctx, dispatcher, state.program.blocks[index],
		                                           state.program.block_info[index]);
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
		case IR::Type::U64:
		case IR::Type::F64: return TypeU64(state);
		case IR::Type::U32x2: return TypeU32Pair(state);
		case IR::Type::F32: return TypeF32(state);
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
			case IR::Type::F64: return ConstantU64(state, value.F64Bits());
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
	if (cooperative_spills != nullptr) {
		if (const auto slot = cooperative_spills->find(inst); slot != cooperative_spills->end()) {
			if (cooperative_phase != 0 && cooperative_phases != nullptr) {
				const auto phase  = cooperative_phases->find(inst);
				const auto direct = definitions.find(inst);
				if (phase != cooperative_phases->end() && phase->second == cooperative_phase &&
				    direct != definitions.end()) {
					return direct->second;
				}
			}
			const auto type = TypeId(state, inst->GetType());
			const auto loaded = state.builder.AllocateId();
			state.builder.AddFunction({OpLoad, type, loaded, slot->second});
			if (cooperative_collective_active == 0) return loaded;
			// Other guest waves participate only in the physical rendezvous.
			// Their uninitialized/stale private values must not affect even the
			// temporary collective's control flow. No guest state is committed.
			if (inst->GetType() == IR::Type::U32x2) {
				std::array<uint32_t, 2> selected_words{};
				for (uint32_t word = 0; word < 2; ++word) {
					const auto raw = state.builder.AllocateId();
					selected_words[word] = state.builder.AllocateId();
					state.builder.AddFunction({OpCompositeExtract, TypeU32(state), raw, loaded, word});
					state.builder.AddFunction({OpSelect, TypeU32(state), selected_words[word],
					                           cooperative_collective_active, raw, ConstantU32(state, 0)});
				}
				const auto pair = state.builder.AllocateId();
				state.builder.AddFunction({OpCompositeConstruct, type, pair, selected_words[0], selected_words[1]});
				return pair;
			}
			uint32_t components = 1;
			switch (inst->GetType()) {
				case IR::Type::U64: case IR::Type::F64: case IR::Type::F32x2:
					components = 2; break;
				case IR::Type::U32x3: components = 3; break;
				case IR::Type::U32x4: components = 4; break;
				default: break;
			}
			uint32_t condition = cooperative_collective_active;
			if (components != 1) {
				condition = state.builder.AllocateId();
				std::vector<uint32_t> words{OpCompositeConstruct, TypeBoolVector(state, components), condition};
				words.insert(words.end(), components, cooperative_collective_active);
				state.builder.AddFunction(words);
			}
			const auto selected = state.builder.AllocateId();
			state.builder.AddFunction({OpSelect, type, selected, condition, loaded,
			                           state.builder.Constant(spv::OpConstantNull, type)});
			return selected;
		}
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
	if (state.compute_execution.IsSplitWave64()) return EmitWaveBallot(state, Def(predicate));
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
	if (state.compute_execution.IsSplitWave64()) return EmitWaveFindFirst(state, ballot);
	if (other_half == nullptr) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformBallotFindLSB, TypeU32(state), result,
		                          ConstantU32(state, spv::ScopeSubgroup), ballot);
		return result;
	}
	const auto low        = state.builder.AllocateId();
	const auto high       = state.builder.AllocateId();
	const auto low_first  = state.builder.AllocateId();
	const auto high_first = state.builder.AllocateId();
	const auto low_active = state.builder.AllocateId();
	const auto result     = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, ballot, 0);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, ballot, 1);
	state.builder.AddFunction(spv::OpExtInst, TypeU32(state), low_first, GlslStd450(state),
	                          GLSLstd450FindILsb, low);
	state.builder.AddFunction(spv::OpExtInst, TypeU32(state), high_first, GlslStd450(state),
	                          GLSLstd450FindILsb, high);
	state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), low_active, low,
	                          ConstantU32(state, 0));
	state.builder.AddFunction(spv::OpSelect, TypeU32(state), result, low_active, low_first,
	                          EmitAddU32(state, high_first, ConstantU32(state, 32)));
	return result;
}

uint32_t ValueEmitContext::Shuffle(const IR::Inst& inst, size_t index, uint32_t lane) {
	if (state.compute_execution.IsSplitWave64()) {
		const auto source = Arg(inst, index);
		if (inst.Arg(index).GetType() != IR::Type::U1) {
			return EmitWaveReadLane(state, source, lane);
		}
		// The shared wave64 lane exchange stores u32 words. Preserve boolean
		// predicates as 0/1 while crossing the two native subgroups.
		const auto word = state.builder.AllocateId();
		state.builder.AddFunction({OpSelect, TypeU32(state), word, source,
		                           ConstantU32(state, 1), ConstantU32(state, 0)});
		const auto shuffled = EmitWaveReadLane(state, word, lane);
		const auto predicate = state.builder.AllocateId();
		state.builder.AddFunction({OpINotEqual, TypeBool(state), predicate, shuffled,
		                           ConstantU32(state, 0)});
		return predicate;
	}
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
	if (const auto found = definitions.find(&inst); found != definitions.end()) {
		return found->second;
	}
	const auto id = state.builder.AllocateId();
	definitions.emplace(&inst, id);
	return id;
}

uint32_t ValueEmitContext::Define(const IR::Inst& inst, uint32_t value) {
	if (const auto found = definitions.find(&inst); found != definitions.end()) {
		if (found->second != value) {
			state.builder.AddFunction(spv::OpCopyObject, TypeId(state, inst.GetType()),
			                          found->second, value);
		}
		return found->second;
	}
	definitions.emplace(&inst, value);
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
	if (memory_override_inst == &inst && memory_override != nullptr) return *memory_override;
	return state.program.memory_info.at(inst.Flags<IR::MemoryFlags>().index);
}

const IR::ExportInfo& ValueEmitContext::Export(const IR::Inst& inst) const {
	return state.program.export_info.at(inst.Flags<IR::ExportFlags>().index);
}

uint32_t ValueEmitContext::Label(const IR::Block* block) const {
	return state.labels.at(block);
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
	std::optional<CooperativeFunctionState> cooperative;
	if (state.program.stage == ShaderType::Pixel && state.requirements.pixel_valid_mask) {
		state.pixel_valid_mask_variable = state.builder.AllocateId();
		state.builder.AddName(state.pixel_valid_mask_variable, "pixel_valid_mask_active");
	}
	if (state.stage == ShaderType::Pixel && state.wave_size == 64u &&
	    state.native_subgroup_size == 32u &&
	    std::ranges::any_of(program.block_info,
	                        [](const IR::BlockInfo& info) { return info.terminator.loop_header; })) {
		state.graphics_loop_counter_variable = state.builder.AllocateId();
		state.builder.AddName(state.graphics_loop_counter_variable, "graphics_loop_counter");
	}
	for (const auto* block: program.blocks) {
		const auto label = state.builder.AllocateId();
		state.labels.emplace(block, label);
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
			mark_cross_block(program.block_info[index].condition, program.blocks[index]);
			mark_cross_block(program.block_info[index].indirect_target, program.blocks[index]);
		}
		dispatch.header_label       = state.builder.AllocateId();
		dispatch.select_label       = state.builder.AllocateId();
		dispatch.after_switch_label = state.builder.AllocateId();
		dispatch.continue_label     = state.builder.AllocateId();
		dispatch.merge_label        = state.builder.AllocateId();
		ctx.dispatcher_spills       = &dispatch.spills[0];
		if (state.lane_count == 2) {
			for (const auto& [inst, id]: dispatch.spills[0]) {
				dispatch.spills[1].emplace(inst, state.builder.AllocateId());
			}
			high.dispatcher_spills = &dispatch.spills[1];
		}
	}
	if (state.compute_execution.IsCooperativeWave64()) {
		cooperative.emplace(PrepareCooperativeFunction(ctx));
		ctx.cooperative_spills = &cooperative->spills;
		ctx.cooperative_phases = &cooperative->phases;
	}
	DefineGetBdaPointer(state);
	DefineCooperativeWaveFunctions(state);
	for (const auto* block: program.blocks) {
		if (std::ranges::any_of(*block, [](const IR::Inst& inst) {
			    return inst.GetOpcode() == IR::ValueOpcode::SwizzleU32 ||
			           inst.GetOpcode() == IR::ValueOpcode::SharedAtomicFMin32 ||
			           inst.GetOpcode() == IR::ValueOpcode::SharedAtomicFMax32;
		    })) {
			ctx.scratch_u32_variable = state.builder.AllocateId();
			if (state.lane_count == 2) {
				high.scratch_u32_variable = state.builder.AllocateId();
			}
			break;
		}
	}
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state),
	                          state.mesh_guest_func != 0 ? state.mesh_guest_func : state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.entry_label);
	if (state.requirements.function_lds) {
		state.builder.AddFunction(
		    spv::OpVariable,
		    TypeU32ArrayPointer(state, spv::StorageClassFunction, LdsDwordCount(state)),
		    state.lds_variable, spv::StorageClassFunction);
	}
	if (state.requirements.function_scratch) {
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
	if (state.graphics_loop_counter_variable != 0) {
		state.builder.AddFunction(spv::OpVariable,
		    TypePointer(state, spv::StorageClassFunction, TypeU32(state)),
		    state.graphics_loop_counter_variable, spv::StorageClassFunction);
	}
	if (cooperative) DeclareCooperativeFunctionVariables(ctx, *cooperative);
	if (state.gds_variable != 0) {
		state.gds_length = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpArrayLength, TypeU32(state), state.gds_length,
		                          state.gds_variable, 0);
	}
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction(spv::OpStore, state.pixel_valid_mask_variable,
		                          ConstantU32(state, 1));
	}
	if (state.graphics_loop_counter_variable != 0) {
		state.builder.AddFunction(
		    {OpStore, state.graphics_loop_counter_variable, ConstantU32(state, 0)});
	}
	if (state.compute_execution.IsSplitWave64()) EmitHostLocalInvocationIndex(state);
	EmitMemoryOffsets(state);
	if (program.blocks.empty()) {
		EmitReturn(ctx);
	} else if (cooperative) {
		EmitCooperativeFunction(ctx, *cooperative);
	} else if (state.program.dispatcher_fallback) {
		EmitDispatcherFunction(ctx, *dispatcher);
	} else {
		EmitStructuredFunction(ctx);
	}
	state.builder.AddFunction(spv::OpFunctionEnd);
	if (state.program.stage == ShaderType::Mesh) {
		EmitMeshEntryPoint(state);
	}
}

void EmitDirectValueInstruction(ValueEmitContext& ctx, const IR::Inst& inst) { EmitDirectInstruction(ctx, inst); }

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
