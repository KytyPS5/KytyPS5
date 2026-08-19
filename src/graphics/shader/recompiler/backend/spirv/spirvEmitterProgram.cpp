#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>
#include <bit>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

void EmitKillIfBoolFalse(EmitterState& state, uint32_t active) {
	const auto kill_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	const auto inactive    = state.builder.AllocateId();
	state.builder.AddFunction({OpLogicalNot, TypeBool(state), inactive, active});
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction({OpBranchConditional, inactive, kill_label, merge_label});
	EmitLabel(state, kill_label);
	state.builder.AddFunction({OpKill});
	EmitLabel(state, merge_label);
}

void EmitKillIfPixelValidMaskInactive(EmitterState& state) {
	if (state.pixel_valid_mask_variable == 0) {
		return;
	}

	const auto mask_value = state.builder.AllocateId();
	const auto active     = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpLoad, TypeU32(state), mask_value, state.pixel_valid_mask_variable});
	state.builder.AddFunction(
	    {OpINotEqual, TypeBool(state), active, mask_value, ConstantU32(state, 0)});
	EmitKillIfBoolFalse(state, active);
}

uint32_t SpillPointerType(EmitterState& state, IR::Type type) {
	switch (type) {
		case IR::Type::U1: return TypePointer(state, StorageClassFunction, TypeBool(state));
		case IR::Type::U8:
		case IR::Type::U16:
		case IR::Type::U32:
		case IR::Type::F16: return TypePointer(state, StorageClassFunction, TypeU32(state));
		case IR::Type::F32: return TypePointer(state, StorageClassFunction, TypeF32(state));
		case IR::Type::U64:
		case IR::Type::U32x2: return TypePointer(state, StorageClassFunction, TypeU32Pair(state));
		case IR::Type::U32x4:
			return TypePointer(state, StorageClassFunction, TypeU32Vector(state, 4));
		case IR::Type::F32x2:
			return TypePointer(state, StorageClassFunction, TypeF32Vector(state, 2));
		default: return 0;
	}
}

struct DeferredPhiPatch {
	DeferredPhi     phi;
	const IR::Inst* instruction = nullptr;
};

struct StructuredFunctionState {
	std::unordered_map<const IR::Block*, uint32_t> block_exit_labels;
	std::vector<DeferredPhiPatch>                  deferred_phis;
};

struct DispatcherFunctionState {
	std::unordered_map<const IR::Inst*, uint32_t> spills;
	uint32_t                                      pc_variable        = 0;
	uint32_t                                      header_label       = 0;
	uint32_t                                      select_label       = 0;
	uint32_t                                      default_label      = 0;
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
				ctx.state.builder.AddFunction(
				    {OpStore, dispatcher.spills.at(&phi), ctx.Def(phi.Arg(index))});
				break;
			}
		}
	}
}

const IR::Block* TargetBlock(const IR::ValueProgram& program, uint32_t id) {
	const auto found = std::ranges::find_if(
	    program.block_info, [&](const IR::ValueBlockInfo& info) { return info.id == id; });
	if (found == program.block_info.end()) {
		return nullptr;
	}
	return program.blocks[static_cast<size_t>(found - program.block_info.begin())];
}

void EmitReturn(ValueEmitContext& ctx) {
	EmitKillIfPixelValidMaskInactive(ctx.state);
	ctx.state.builder.AddFunction({OpReturn});
}

void EmitStructuredTerminator(ValueEmitContext& ctx, const IR::Block* block,
                              const IR::ValueBlockInfo& info) {
	const auto& term       = info.terminator;
	const auto  emit_merge = [&]() {
		if (term.loop_header) {
			const auto* merge = TargetBlock(ctx.program, term.merge_block);
			const auto* cont  = TargetBlock(ctx.program, term.continue_block);
			if (merge != nullptr && cont != nullptr) {
				ctx.state.builder.AddFunction(
				    {OpLoopMerge, ctx.Label(merge), ctx.Label(cont), LoopControlNone});
			}
		} else if (term.kind == CFG::TerminatorKind::ConditionalBranch &&
		           term.merge_block != UINT32_MAX) {
			if (const auto* merge = TargetBlock(ctx.program, term.merge_block); merge != nullptr) {
				ctx.state.builder.AddFunction(
				    {OpSelectionMerge, ctx.Label(merge), SelectionControlNone});
			}
		}
	};

	switch (term.kind) {
		case CFG::TerminatorKind::Branch: {
			const auto* target = TargetBlock(ctx.program, term.true_block);
			if (target == nullptr) {
				EmitReturn(ctx);
				return;
			}
			emit_merge();
			ctx.state.builder.AddFunction({OpBranch, ctx.Label(target)});
			return;
		}
		case CFG::TerminatorKind::ConditionalBranch: {
			const auto* true_block  = TargetBlock(ctx.program, term.true_block);
			const auto* false_block = TargetBlock(ctx.program, term.false_block);
			if (true_block == nullptr || false_block == nullptr || info.condition.IsEmpty()) {
				EmitReturn(ctx);
				return;
			}
			const auto condition = ctx.Def(info.condition);
			emit_merge();
			ctx.state.builder.AddFunction(
			    {OpBranchConditional, condition, ctx.Label(true_block), ctx.Label(false_block)});
			return;
		}
		default: EmitReturn(ctx); return;
	}
}

void EmitDispatcherTarget(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                          const IR::Block* from, uint32_t target) {
	const auto* block = TargetBlock(ctx.program, target);
	if (block != nullptr) {
		StoreDispatcherPhiEdge(ctx, dispatcher, from, block);
	}
}

void EmitDispatcherTerminator(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                              const IR::Block* block, const IR::ValueBlockInfo& info,
                              uint32_t exit_label) {
	const auto& term = info.terminator;
	switch (term.kind) {
		case CFG::TerminatorKind::Branch:
			EmitDispatcherTarget(ctx, dispatcher, block, term.true_block);
			ctx.state.builder.AddFunction(
			    {OpStore, dispatcher.pc_variable, ConstantU32(ctx.state, term.true_block)});
			break;
		case CFG::TerminatorKind::ConditionalBranch: {
			EmitDispatcherTarget(ctx, dispatcher, block, term.true_block);
			EmitDispatcherTarget(ctx, dispatcher, block, term.false_block);
			const auto selected = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction({OpSelect, TypeU32(ctx.state), selected,
			                               ctx.Def(info.condition),
			                               ConstantU32(ctx.state, term.true_block),
			                               ConstantU32(ctx.state, term.false_block)});
			ctx.state.builder.AddFunction({OpStore, dispatcher.pc_variable, selected});
			break;
		}
		case CFG::TerminatorKind::IndirectBranch: {
			for (const auto target: term.indirect_targets) {
				EmitDispatcherTarget(ctx, dispatcher, block, target);
			}
			for (const auto target: term.indirect_selector_targets) {
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
					ctx.state.builder.AddFunction({OpIEqual, TypeBool(ctx.state), match, selector,
					                               ConstantU32(ctx.state, values[index])});
					ctx.state.builder.AddFunction({OpSelect, TypeU32(ctx.state), next, match,
					                               ConstantU32(ctx.state, targets[index]),
					                               selected});
					selected = next;
				}
			}
			ctx.state.builder.AddFunction({OpStore, dispatcher.pc_variable, selected});
			break;
		}
		default:
			ctx.state.builder.AddFunction(
			    {OpStore, dispatcher.pc_variable, ConstantU32(ctx.state, UINT32_MAX)});
			break;
	}
	ctx.state.builder.AddFunction({OpBranch, exit_label});
}

bool EmitDirectInstruction(ValueEmitContext& ctx, const IR::Inst& inst) {
	if (EmitValueFlow(ctx, inst) || EmitValueAlu(ctx, inst) || EmitValueMemory(ctx, inst) ||
	    EmitValueImage(ctx, inst)) {
		return true;
	}
	ctx.Fail(inst, "has no direct SPIR-V emitter");
	return false;
}

bool EmitStructuredInstruction(ValueEmitContext& ctx, StructuredFunctionState& structured,
                               const IR::Inst& inst) {
	if (inst.GetOpcode() == IR::ValueOpcode::Phi) {
		const auto type = ctx.TypeId(inst.GetType());
		if (type == 0 || inst.NumArgs() == 0) {
			ctx.Fail(inst, "has no native SPIR-V representation");
			return false;
		}
		for (size_t index = 0; index < inst.NumArgs(); index++) {
			const auto* predecessor = inst.PhiBlock(index);
			if (predecessor == nullptr || !ctx.labels.contains(predecessor)) {
				ctx.Fail(inst, "has a predecessor outside the structured function");
				return false;
			}
		}
		structured.deferred_phis.push_back(
		    {ctx.state.builder.AddDeferredPhi(type, ctx.Result(inst), inst.NumArgs()), &inst});
		return true;
	}
	return EmitDirectInstruction(ctx, inst);
}

bool EmitDispatcherInstruction(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher,
                               const IR::Inst& inst) {
	if (inst.GetOpcode() == IR::ValueOpcode::Phi) {
		const auto type = ctx.TypeId(inst.GetType());
		if (type == 0) {
			ctx.Fail(inst, "cannot be loaded by the dispatcher");
			return false;
		}
		ctx.state.builder.AddFunction(
		    {OpLoad, type, ctx.Result(inst), dispatcher.spills.at(&inst)});
		return true;
	}
	if (!EmitDirectInstruction(ctx, inst)) {
		return false;
	}
	if (const auto found = dispatcher.spills.find(&inst); found != dispatcher.spills.end()) {
		ctx.state.builder.AddFunction(
		    {OpStore, found->second, ctx.Def(IR::Value(const_cast<IR::Inst*>(&inst)))});
	}
	return true;
}

template <typename EmitInstruction>
void EmitBlock(ValueEmitContext& ctx, const IR::Block* block, EmitInstruction&& emit_instruction) {
	ctx.current_block = block;
	EmitLabel(ctx.state, ctx.Label(block));
	bool emitted_non_phi = false;
	for (const auto& inst: *block) {
		if (inst.GetOpcode() == IR::ValueOpcode::Phi) {
			if (emitted_non_phi) {
				ctx.Fail(inst, "appears after a non-Phi instruction");
				return;
			}
		} else {
			emitted_non_phi = true;
		}
		if (!emit_instruction(inst)) {
			return;
		}
	}
}

void PatchStructuredPhis(ValueEmitContext& ctx, StructuredFunctionState& structured) {
	for (const auto& deferred: structured.deferred_phis) {
		for (size_t index = 0; index < deferred.instruction->NumArgs(); index++) {
			const auto* predecessor = deferred.instruction->PhiBlock(index);
			const auto  found       = structured.block_exit_labels.find(predecessor);
			if (found == structured.block_exit_labels.end()) {
				ctx.failed = true;
				ctx.error  = "typed Phi predecessor was not emitted";
				return;
			}
			ctx.state.builder.PatchDeferredPhi(
			    deferred.phi, index, ctx.Def(deferred.instruction->Arg(index)), found->second);
			if (ctx.failed) {
				return;
			}
		}
	}
}

void EmitStructuredFunction(ValueEmitContext& ctx) {
	StructuredFunctionState structured;
	ctx.state.builder.AddFunction({OpBranch, ctx.Label(ctx.program.blocks.front())});
	for (size_t index = 0; index < ctx.program.blocks.size() && !ctx.failed; index++) {
		const auto* block = ctx.program.blocks[index];
		EmitBlock(ctx, block, [&](const IR::Inst& inst) {
			return EmitStructuredInstruction(ctx, structured, inst);
		});
		if (ctx.failed) {
			break;
		}
		structured.block_exit_labels.emplace(block, ctx.state.current_label);
		EmitStructuredTerminator(ctx, block, ctx.program.block_info[index]);
	}
	if (!ctx.failed) {
		PatchStructuredPhis(ctx, structured);
	}
}

void EmitDispatcherFunction(ValueEmitContext& ctx, const DispatcherFunctionState& dispatcher) {
	auto&       state = ctx.state;
	const auto* entry = ctx.program.blocks.front();
	state.builder.AddFunction({OpBranch, ctx.Label(entry)});
	EmitBlock(ctx, entry, [&](const IR::Inst& inst) {
		return EmitDispatcherInstruction(ctx, dispatcher, inst);
	});
	if (ctx.failed) {
		return;
	}
	EmitDispatcherTerminator(ctx, dispatcher, entry, ctx.program.block_info.front(),
	                         dispatcher.header_label);

	EmitLabel(state, dispatcher.header_label);
	const auto pc = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, TypeU32(state), pc, dispatcher.pc_variable});
	const auto done = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpIEqual, TypeBool(state), done, pc, ConstantU32(ctx.state, UINT32_MAX)});
	state.builder.AddFunction(
	    {OpLoopMerge, dispatcher.merge_label, dispatcher.continue_label, LoopControlNone});
	state.builder.AddFunction(
	    {OpBranchConditional, done, dispatcher.merge_label, dispatcher.select_label});

	EmitLabel(state, dispatcher.select_label);
	state.builder.AddFunction(
	    {OpSelectionMerge, dispatcher.after_switch_label, SelectionControlNone});
	std::vector<uint32_t> words {OpSwitch, pc, dispatcher.default_label};
	for (size_t index = 1; index < ctx.program.blocks.size(); index++) {
		words.push_back(ctx.program.block_info[index].id);
		words.push_back(ctx.Label(ctx.program.blocks[index]));
	}
	state.builder.AddFunction(words);
	EmitLabel(state, dispatcher.default_label);
	state.builder.AddFunction(
	    {OpStore, dispatcher.pc_variable, ConstantU32(ctx.state, UINT32_MAX)});
	state.builder.AddFunction({OpBranch, dispatcher.after_switch_label});

	for (size_t index = 1; index < ctx.program.blocks.size() && !ctx.failed; index++) {
		EmitBlock(ctx, ctx.program.blocks[index], [&](const IR::Inst& inst) {
			return EmitDispatcherInstruction(ctx, dispatcher, inst);
		});
		if (ctx.failed) {
			break;
		}
		EmitDispatcherTerminator(ctx, dispatcher, ctx.program.blocks[index],
		                         ctx.program.block_info[index], dispatcher.after_switch_label);
	}
	if (ctx.failed) {
		return;
	}
	EmitLabel(state, dispatcher.after_switch_label);
	state.builder.AddFunction({OpBranch, dispatcher.continue_label});
	EmitLabel(state, dispatcher.continue_label);
	state.builder.AddFunction({OpBranch, dispatcher.header_label});
	EmitLabel(state, dispatcher.merge_label);
	EmitReturn(ctx);
}

} // namespace

uint32_t ValueEmitContext::TypeId(IR::Type type) const {
	switch (type) {
		case IR::Type::U1: return TypeBool(state);
		case IR::Type::U8:
		case IR::Type::U16:
		case IR::Type::U32:
		case IR::Type::F16: return TypeU32(state);
		case IR::Type::U64:
		case IR::Type::U32x2: return TypeU32Pair(state);
		case IR::Type::F32: return TypeF32(state);
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
		failed = true;
		error  = "direct SPIR-V emitter received a non-value argument";
		return ConstantU32(state, 0);
	}
	if (dispatcher_spills != nullptr && current_block != nullptr &&
	    inst->Parent() != current_block) {
		if (const auto found = dispatcher_spills->find(inst); found != dispatcher_spills->end()) {
			const auto id = state.builder.AllocateId();
			state.builder.AddFunction({OpLoad, TypeId(inst->GetType()), id, found->second});
			return id;
		}
	}
	if (const auto found = definitions.find(inst); found != definitions.end()) {
		return found->second;
	}
	const auto id = state.builder.AllocateId();
	definitions.emplace(inst, id);
	return id;
}

uint32_t ValueEmitContext::Arg(const IR::Inst& inst, size_t index) {
	return Def(inst.Arg(index));
}

uint32_t ValueEmitContext::Result(const IR::Inst& inst) {
	if (const auto found = definitions.find(&inst); found != definitions.end()) {
		return found->second;
	}
	const auto id = state.builder.AllocateId();
	definitions.emplace(&inst, id);
	return id;
}

uint32_t ValueEmitContext::Emit(const IR::Inst& inst, uint32_t opcode, IR::Type type,
                                std::initializer_list<uint32_t> args) {
	std::vector<uint32_t> words {opcode, TypeId(type), Result(inst)};
	words.insert(words.end(), args.begin(), args.end());
	state.builder.AddFunction(words);
	return Result(inst);
}

uint32_t ValueEmitContext::Define(const IR::Inst& inst, uint32_t value) {
	if (const auto found = definitions.find(&inst); found != definitions.end()) {
		if (found->second != value) {
			state.builder.AddFunction({OpCopyObject, TypeId(inst.GetType()), found->second, value});
		}
		return found->second;
	}
	definitions.emplace(&inst, value);
	return value;
}

uint32_t ValueEmitContext::ResourceIndex(IR::Value value, IR::ValueOpcode opcode) {
	const auto* inst = value.ResolveInstruction();
	if (inst == nullptr || inst->GetOpcode() != opcode) {
		failed = true;
		error  = "typed resource handle has the wrong producer";
		return 0;
	}
	return inst->Flags<uint32_t>();
}

const IR::Inst* ValueEmitContext::ImageAddress(IR::Value value) {
	const auto* inst = value.ResolveInstruction();
	if (inst == nullptr || inst->GetOpcode() != IR::ValueOpcode::MakeImageAddress) {
		failed = true;
		error  = "typed image address was not constructed by MakeImageAddress";
		return nullptr;
	}
	return inst;
}

const IR::MemoryInfo& ValueEmitContext::Memory(const IR::Inst& inst) const {
	return program.memory_info.at(inst.Flags<IR::MemoryFlags>().index);
}

const IR::ExportInfo& ValueEmitContext::Export(const IR::Inst& inst) const {
	return program.export_info.at(inst.Flags<IR::ExportFlags>().index);
}

uint32_t ValueEmitContext::Label(const IR::Block* block) const {
	return labels.at(block);
}

size_t ValueEmitContext::BlockIndex(const IR::Block* block) const {
	return static_cast<size_t>(std::ranges::find(program.blocks, block) - program.blocks.begin());
}

void ValueEmitContext::Fail(const IR::Inst& inst, const char* reason) {
	failed = true;
	error  = fmt::format("typed opcode {} {}", IR::ValueOpcodeName(inst.GetOpcode()), reason);
}

bool EmitValueProgram(EmitterState& state, const IR::ValueProgram& program, std::string* error) {
	ValueEmitContext                       ctx(state, program);
	std::optional<DispatcherFunctionState> dispatcher;
	if (state.stage == ShaderType::Pixel && state.requirements.pixel_valid_mask) {
		state.pixel_valid_mask_variable = state.builder.AllocateId();
		state.builder.AddName(state.pixel_valid_mask_variable, "pixel_valid_mask_active");
	}
	for (const auto* block: program.blocks) {
		ctx.labels.emplace(block, state.builder.AllocateId());
	}
	if (program.dispatcher_fallback) {
		auto& dispatch = dispatcher.emplace();
		for (const auto* block: program.blocks) {
			for (const auto& inst: *block) {
				if (inst.GetOpcode() != IR::ValueOpcode::Phi) {
					continue;
				}
				if (SpillPointerType(state, inst.GetType()) == 0) {
					ctx.Fail(inst, "cannot be stored by the dispatcher");
					break;
				}
				dispatch.spills.emplace(&inst, state.builder.AllocateId());
			}
		}
		const auto mark_cross_block = [&](IR::Value value, const IR::Block* consumer) {
			value                  = value.Resolve();
			const auto* definition = value.TryInstruction();
			if (definition == nullptr || definition->Parent() == consumer) {
				return;
			}
			if (SpillPointerType(state, definition->GetType()) == 0) {
				ctx.Fail(*definition, "cannot be stored by the dispatcher");
				return;
			}
			if (!dispatch.spills.contains(definition)) {
				dispatch.spills.emplace(definition, state.builder.AllocateId());
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
		dispatch.pc_variable        = state.builder.AllocateId();
		dispatch.header_label       = state.builder.AllocateId();
		dispatch.select_label       = state.builder.AllocateId();
		dispatch.default_label      = state.builder.AllocateId();
		dispatch.after_switch_label = state.builder.AllocateId();
		dispatch.continue_label     = state.builder.AllocateId();
		dispatch.merge_label        = state.builder.AllocateId();
		ctx.dispatcher_spills       = &dispatch.spills;
	}
	if (ctx.failed) {
		SetError(error, ctx.error.c_str());
		return false;
	}
	for (const auto* block: program.blocks) {
		if (std::ranges::any_of(*block, [](const IR::Inst& inst) {
			    return inst.GetOpcode() == IR::ValueOpcode::SwizzleU32 ||
			           inst.GetOpcode() == IR::ValueOpcode::SharedAtomicFMin32 ||
			           inst.GetOpcode() == IR::ValueOpcode::SharedAtomicFMax32 ||
			           inst.GetOpcode() == IR::ValueOpcode::GdsAtomicFMin32 ||
			           inst.GetOpcode() == IR::ValueOpcode::GdsAtomicFMax32;
		    })) {
			ctx.scratch_u32_variable = state.builder.AllocateId();
			break;
		}
	}
	state.builder.AddFunction(
	    {OpFunction, TypeVoid(state), state.main_func, FunctionControlNone, TypeFunction(state)});
	EmitLabel(state, state.entry_label);
	if (state.requirements.function_lds) {
		state.builder.AddFunction({OpVariable, TypeLdsArrayPointer(state, StorageClassFunction),
		                           state.lds_variable, StorageClassFunction});
	}
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction({OpVariable,
		                           TypePointer(state, StorageClassFunction, TypeU32(state)),
		                           state.pixel_valid_mask_variable, StorageClassFunction});
	}
	if (program.dispatcher_fallback) {
		state.builder.AddFunction({OpVariable,
		                           TypePointer(state, StorageClassFunction, TypeU32(state)),
		                           dispatcher->pc_variable, StorageClassFunction});
		for (const auto* block: program.blocks) {
			for (const auto& inst: *block) {
				if (const auto found = dispatcher->spills.find(&inst);
				    found != dispatcher->spills.end()) {
					state.builder.AddFunction({OpVariable, SpillPointerType(state, inst.GetType()),
					                           found->second, StorageClassFunction});
				}
			}
		}
	}
	if (ctx.scratch_u32_variable != 0) {
		state.builder.AddFunction({OpVariable,
		                           TypePointer(state, StorageClassFunction, TypeU32(state)),
		                           ctx.scratch_u32_variable, StorageClassFunction});
	}
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction(
		    {OpStore, state.pixel_valid_mask_variable, ConstantU32(state, 1)});
	}
	EmitStorageBufferOffsets(state);
	if (program.blocks.empty()) {
		EmitReturn(ctx);
	} else if (program.dispatcher_fallback) {
		EmitDispatcherFunction(ctx, *dispatcher);
	} else {
		EmitStructuredFunction(ctx);
	}
	state.builder.AddFunction({OpFunctionEnd});
	if (ctx.failed) {
		SetError(error, ctx.error.c_str());
		return false;
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
