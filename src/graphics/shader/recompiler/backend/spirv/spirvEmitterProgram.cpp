#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"
#include "graphics/shader/recompiler/ir/BindlessBindings.h"

#include "common/assert.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <optional>
#include <type_traits>
#include <unordered_map>
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

void EmitReturn(ValueEmitContext& ctx) {
	EmitKillIfPixelValidMaskInactive(ctx.state);
	ctx.state.builder.AddFunction(spv::OpReturn);
}

void EmitStructuredTerminator(ValueEmitContext& ctx, const IR::Block* block,
                              const IR::BlockInfo& info) {
	const auto& program = ctx.state.program;
	const auto& term       = info.terminator;
	const auto  emit_merge = [&]() {
		if (term.loop_header) {
			const auto* merge = TargetBlock(program, term.merge_block);
			const auto* cont  = TargetBlock(program, term.continue_block);
			if (merge != nullptr && cont != nullptr) {
				ctx.state.builder.AddFunction(spv::OpLoopMerge, ctx.Label(merge), ctx.Label(cont),
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
			emit_merge();
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
			const auto condition = ctx.Def(info.condition);
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
			    spv::OpSelect, TypeU32(ctx.state), selected, ctx.Def(info.condition),
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

void EmitDirectInstruction(ValueEmitContext& ctx, const IR::Inst& inst) {
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

// Research: a loop watchdog for shaders that read memory through device addresses. A page such
// a shader touches first reads as zero until the fault buffer maps it for the next dispatch;
// a list walk fed zeros never ends and the device is lost before that dispatch. Each loop
// whose continue block branches straight back to its header counts its iterations and leaves
// through its merge block after LoopWatchdogLimit of them.
constexpr uint32_t LoopWatchdogLimit = 4096;

struct LoopWatchdog {
	const IR::Block* header     = nullptr;
	const IR::Block* cont       = nullptr;
	const IR::Block* merge      = nullptr;
	uint32_t         counter    = 0; // phi in the header
	uint32_t         next       = 0; // counter + 1, defined in the continue block
	Spirv::DeferredPhi phi {};
	bool               has_phi  = false;
	size_t             operands = 0;
	// The latch branched straight back: its new exit adds a predecessor to the merge block,
	// whose phis take an undefined value from it (the loop was cut short).
	bool                                   exit_edge = false;
	std::unordered_map<uint32_t, uint32_t> undefs; // phi type -> OpUndef in the latch
};

struct WatchdogExtraIncoming {
	Spirv::DeferredPhi phi {};
	uint32_t           type  = 0;
	size_t             slot  = 0;
	const IR::Block*   latch = nullptr;
};

// Research: a tripped watchdog appends a report (layout in BindlessBindings.h) from its merge
// block, which the header dominates: the shader, the loop, where it ran, user data s0-s7 and
// the header's phi values (the state of the last iteration) for each lane half.
void EmitWatchdogReport(ValueEmitContext& ctx, const LoopWatchdog& watchdog, uint32_t loop_index) {
	auto& state = ctx.state;
	if (state.bindless_feedback_variable == 0) {
		return;
	}
	const auto store = [&](uint32_t index, uint32_t value) {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state),
		                          pointer, state.bindless_feedback_variable, ConstantU32(state, 0),
		                          index);
		state.builder.AddFunction(spv::OpStore, pointer, value);
	};
	const auto tripped = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state),
	                            watchdog.counter, ConstantU32(state, LoopWatchdogLimit - 1u));
	EmitIfCondition(state, tripped, [&] {
		const auto counter = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state),
		                          counter, state.bindless_feedback_variable, ConstantU32(state, 0),
		                          ConstantU32(state, IR::WatchdogReportBase));
		const auto slot = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAtomicIAdd, TypeU32(state), slot, counter,
		                          ConstantU32(state, spv::ScopeDevice), ConstantU32(state, 0),
		                          ConstantU32(state, 1));
		const auto has_room = Binary(state, spv::OpULessThan, TypeBool(state), slot,
		                             ConstantU32(state, IR::WatchdogReportSlots));
		EmitIfCondition(state, has_room, [&] {
			const auto base =
			    EmitAddU32(state,
			               EmitBinaryU32(state, spv::OpIMul, slot,
			                             ConstantU32(state, IR::WatchdogReportWords)),
			               ConstantU32(state, IR::WatchdogReportBase + IR::WatchdogReportWords));
			const auto word = [&](uint32_t offset) {
				return EmitAddU32(state, base, ConstantU32(state, offset));
			};
			const auto hash = state.program.shader_hash;
			store(word(0), ConstantU32(state, 0x57440000u | (loop_index & 0xffffu)));
			store(word(1), ConstantU32(state, static_cast<uint32_t>(hash)));
			store(word(2), ConstantU32(state, static_cast<uint32_t>(hash >> 32u)));
			store(word(3), watchdog.counter);
			store(word(4), EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0));
			store(word(5), EmitSubgroupLocalInvocationId(state));
			for (uint32_t reg = 0; reg < 8; reg++) {
				store(word(6 + reg), EmitGetUserData(state, static_cast<IR::ScalarReg>(reg)));
			}
			for (uint32_t half = 0; half < state.lane_count; half++) {
				auto&    lane  = half == 0 ? ctx : *ctx.other_half;
				uint32_t index = 0;
				for (const auto& inst: *watchdog.header) {
					if (inst.GetOpcode() != IR::ValueOpcode::Phi || index >= IR::WatchdogReportPhis) {
						break;
					}
					const auto value = lane.Def(IR::Value(const_cast<IR::Inst*>(&inst)));
					uint32_t   bits  = 0;
					switch (inst.GetType()) {
						case IR::Type::U8:
						case IR::Type::U16:
						case IR::Type::U32:
						case IR::Type::F16: bits = value; break;
						case IR::Type::F32:
							bits = state.builder.AllocateId();
							state.builder.AddFunction(spv::OpBitcast, TypeU32(state), bits, value);
							break;
						case IR::Type::U1:
							bits = state.builder.AllocateId();
							state.builder.AddFunction(spv::OpSelect, TypeU32(state), bits, value,
							                          ConstantU32(state, 1), ConstantU32(state, 0));
							break;
						case IR::Type::U64:
							bits = state.builder.AllocateId();
							state.builder.AddFunction(spv::OpUConvert, TypeU32(state), bits, value);
							break;
						default: bits = ConstantU32(state, 0xdeadbeefu); break;
					}
					store(word(16 + half * IR::WatchdogReportPhis + index), bits);
					index++;
				}
			}
		});
	});
}

void EmitStructuredFunction(ValueEmitContext& ctx) {
	const auto& program = ctx.state.program;
	StructuredFunctionState structured;
	std::vector<LoopWatchdog>          watchdogs;
	std::vector<WatchdogExtraIncoming> extra_incomings;
	if (program.info.uses_dma) {
		for (size_t index = 0; index < program.blocks.size(); index++) {
			const auto& term = program.block_info[index].terminator;
			if (!term.loop_header) {
				continue;
			}
			const auto* header = program.blocks[index];
			const auto* cont   = TargetBlock(program, term.continue_block);
			const auto* merge  = TargetBlock(program, term.merge_block);
			if (cont == nullptr || merge == nullptr || cont == header) {
				continue;
			}
			const auto cont_it = std::ranges::find(program.blocks, cont);
			if (cont_it == program.blocks.end()) {
				continue;
			}
			const auto& cont_term =
			    program.block_info[static_cast<size_t>(cont_it - program.blocks.begin())].terminator;
			const auto  targets_header = [&](uint32_t id) { return TargetBlock(program, id) == header; };
			const bool  back_edge =
			    (cont_term.kind == CFG::TerminatorKind::Branch && targets_header(cont_term.true_block)) ||
			    (cont_term.kind == CFG::TerminatorKind::ConditionalBranch &&
			     (targets_header(cont_term.true_block) || targets_header(cont_term.false_block)));
			if (!back_edge || cont_term.loop_header) {
				continue;
			}
			LoopWatchdog watchdog;
			watchdog.header    = header;
			watchdog.cont      = cont;
			watchdog.merge     = merge;
			watchdog.exit_edge = cont_term.kind == CFG::TerminatorKind::Branch;
			watchdog.counter = ctx.state.builder.AllocateId();
			watchdog.next    = ctx.state.builder.AllocateId();
			watchdogs.push_back(watchdog);
		}
	}
	ctx.state.builder.AddFunction(spv::OpBranch, ctx.Label(program.blocks.front()));
	for (size_t index = 0; index < program.blocks.size(); index++) {
		const auto* block = program.blocks[index];
		const LoopWatchdog* cut_merge = nullptr;
		for (const auto& watchdog: watchdogs) {
			if (watchdog.exit_edge && watchdog.merge == block) {
				cut_merge = &watchdog;
			}
		}
		EmitBlock(ctx, block, [&](ValueEmitContext& lane, const IR::Inst& inst) {
			if (cut_merge != nullptr && inst.GetOpcode() == IR::ValueOpcode::Phi) {
				const auto type = TypeId(lane.state, inst.GetType());
				if (type == 0 || inst.NumArgs() == 0) {
					lane.Fail(inst, "has no native SPIR-V representation");
				}
				const auto phi =
				    lane.state.builder.AddDeferredPhi(type, lane.Result(inst), inst.NumArgs() + 1u);
				structured.deferred_phis.push_back({phi, &inst, lane.half});
				extra_incomings.push_back({phi, type, inst.NumArgs(), cut_merge->cont});
				return;
			}
			EmitStructuredInstruction(lane, structured, inst);
		});
		for (auto& watchdog: watchdogs) {
			if (watchdog.header == block) {
				// The header is a dedicated empty block: its phis come right after the label.
				std::vector<const IR::Block*> preds;
				for (const auto* pred: block->ImmPredecessors()) {
					if (std::ranges::find(preds, pred) == preds.end()) {
						preds.push_back(pred);
					}
				}
				watchdog.operands = preds.size();
				watchdog.phi      = ctx.state.builder.AddDeferredPhi(TypeU32(ctx.state),
				                                                     watchdog.counter, preds.size());
				watchdog.has_phi  = true;
			}
		}
		if (program.info.watchdog_reports && !program.block_info[index].terminator.loop_header) {
			for (const auto& watchdog: watchdogs) {
				if (watchdog.merge == block) {
					const auto header_it = std::ranges::find(program.blocks, watchdog.header);
					EmitWatchdogReport(
					    ctx, watchdog, static_cast<uint32_t>(header_it - program.blocks.begin()));
				}
			}
		}
		structured.block_exit_labels.emplace(block, ctx.state.current_label);
		LoopWatchdog* latch = nullptr;
		for (auto& watchdog: watchdogs) {
			if (watchdog.cont == block) {
				latch = &watchdog;
			}
		}
		if (latch == nullptr) {
			EmitStructuredTerminator(ctx, block, program.block_info[index]);
			continue;
		}
		auto&       state = ctx.state;
		const auto& info  = program.block_info[index];
		const auto& term  = info.terminator;
		state.builder.AddFunction(spv::OpIAdd, TypeU32(state), latch->next, latch->counter,
		                          ConstantU32(state, 1u));
		const auto under = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), under, latch->next,
		                          ConstantU32(state, LoopWatchdogLimit));
		const auto* header = latch->header;
		const auto* merge  = latch->merge;
		if (term.kind == CFG::TerminatorKind::Branch) {
			// One undefined value per phi type of the merge block, for the new edge.
			for (const auto& inst: *merge) {
				if (inst.GetOpcode() != IR::ValueOpcode::Phi) {
					break;
				}
				const auto type = TypeId(state, inst.GetType());
				if (type != 0 && !latch->undefs.contains(type)) {
					const auto undef = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpUndef, type, undef);
					latch->undefs.emplace(type, undef);
				}
			}
			state.builder.AddFunction(spv::OpBranchConditional, under, ctx.Label(header),
			                          ctx.Label(merge));
			continue;
		}
		const auto* true_block  = TargetBlock(program, term.true_block);
		const auto* false_block = TargetBlock(program, term.false_block);
		const auto  condition   = ctx.Def(info.condition);
		const auto  guarded     = state.builder.AllocateId();
		if (true_block == header) {
			// Stay only while the loop wants to and the budget lasts.
			state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(state), guarded, condition,
			                          under);
		} else {
			const auto over = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpLogicalNot, TypeBool(state), over, under);
			state.builder.AddFunction(spv::OpLogicalOr, TypeBool(state), guarded, condition, over);
		}
		const auto* exit_block = true_block == header ? false_block : true_block;
		if (exit_block != merge) {
			// The loop leaves elsewhere; keep the original branch (the counter still runs).
			state.builder.AddFunction(spv::OpBranchConditional, condition, ctx.Label(true_block),
			                          ctx.Label(false_block));
			continue;
		}
		state.builder.AddFunction(spv::OpBranchConditional, guarded, ctx.Label(true_block),
		                          ctx.Label(false_block));
	}
	for (const auto& watchdog: watchdogs) {
		if (!watchdog.has_phi) {
			continue;
		}
		size_t operand = 0;
		std::vector<const IR::Block*> preds;
		for (const auto* pred: watchdog.header->ImmPredecessors()) {
			if (std::ranges::find(preds, pred) != preds.end()) {
				continue;
			}
			preds.push_back(pred);
			const auto found = structured.block_exit_labels.find(pred);
			if (found == structured.block_exit_labels.end()) {
				continue;
			}
			ctx.state.builder.PatchDeferredPhi(
			    watchdog.phi, operand++,
			    pred == watchdog.cont ? watchdog.next : ConstantU32(ctx.state, 0u), found->second);
		}
	}
	for (const auto& extra: extra_incomings) {
		const LoopWatchdog* owner = nullptr;
		for (const auto& watchdog: watchdogs) {
			if (watchdog.cont == extra.latch) {
				owner = &watchdog;
			}
		}
		const auto label = structured.block_exit_labels.find(extra.latch);
		if (owner == nullptr || label == structured.block_exit_labels.end() ||
		    !owner->undefs.contains(extra.type)) {
			EXIT("loop watchdog: merge-block phi has no latch value\n");
		}
		ctx.state.builder.PatchDeferredPhi(extra.phi, extra.slot, owner->undefs.at(extra.type),
		                                   label->second);
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
	// Research safety net: a guest loop whose trip count comes from bad data (a blur radius read
	// from a buffer an unemulated pass left unwritten) hung the GPU until the driver reset it.
	// Leave the dispatcher after a bounded number of block transitions instead: the invocation's
	// results are wrong, the device survives.
	constexpr uint32_t MaxDispatcherTransitions = 4096;
	const auto         iteration                = state.builder.AllocateId();
	const auto         next_iteration           = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpPhi, TypeU32(state), iteration, ConstantU32(state, 0u),
	                          initial_parent, next_iteration, dispatcher.continue_label);
	const auto finished = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), finished, pc,
	                          ConstantU32(ctx.state, UINT32_MAX));
	const auto exhausted = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpUGreaterThanEqual, TypeBool(state), exhausted, iteration,
	                          ConstantU32(state, MaxDispatcherTransitions));
	const auto done = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLogicalOr, TypeBool(state), done, finished, exhausted);
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
	state.builder.AddFunction(spv::OpIAdd, TypeU32(state), next_iteration, iteration,
	                          ConstantU32(state, 1u));
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

uint32_t ValueEmitContext::FirstLane(uint32_t ballot) {
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
	if (state.program.stage == ShaderType::Pixel && state.requirements.pixel_valid_mask) {
		state.pixel_valid_mask_variable = state.builder.AllocateId();
		state.builder.AddName(state.pixel_valid_mask_variable, "pixel_valid_mask_active");
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
	DefineGetBdaPointer(state);
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
	if (state.gds_variable != 0) {
		state.gds_length = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpArrayLength, TypeU32(state), state.gds_length,
		                          state.gds_variable, 0);
	}
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction(spv::OpStore, state.pixel_valid_mask_variable,
		                          ConstantU32(state, 1));
	}
	EmitMemoryOffsets(state);
	if (program.blocks.empty()) {
		EmitReturn(ctx);
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

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
