#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>
#include <bit>
#include <fmt/format.h>
#include <functional>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

void EmitKillIfBoolFalse(EmitterState& state, uint32_t active) {
	const auto kill_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	const auto inactive    = state.builder.AllocateId();
	state.builder.AddFunction({OpLogicalNot, state.bool_type, inactive, active});
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction({OpBranchConditional, inactive, kill_label, merge_label});
	state.builder.AddFunction({OpLabel, kill_label});
	state.builder.AddFunction({OpKill});
	state.builder.AddFunction({OpLabel, merge_label});
}

void EmitKillIfPixelValidMaskInactive(EmitterState& state) {
	if (state.pixel_valid_mask_variable == 0) {
		return;
	}

	const auto mask_value = state.builder.AllocateId();
	const auto active     = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpLoad, state.uint_type, mask_value, state.pixel_valid_mask_variable});
	state.builder.AddFunction(
	    {OpINotEqual, state.bool_type, active, mask_value, ConstantU32(state, 0)});
	EmitKillIfBoolFalse(state, active);
}

uint32_t BoolConstant(EmitterState& state, bool value) {
	const auto id = state.builder.AllocateId();
	state.builder.AddType({value ? 41u : 42u, state.bool_type, id});
	return id;
}

uint32_t ConstantPair(EmitterState& state, uint64_t value) {
	const auto id = state.builder.AllocateId();
	state.builder.AddType({OpConstantComposite, state.uint_pair_type, id,
	                       ConstantU32(state, static_cast<uint32_t>(value)),
	                       ConstantU32(state, static_cast<uint32_t>(value >> 32u))});
	return id;
}

uint32_t PhiPointerType(const EmitterState& state, IR::Type type) {
	switch (type) {
		case IR::Type::U1: return state.ptr_func_bool;
		case IR::Type::U8:
		case IR::Type::U16:
		case IR::Type::U32:
		case IR::Type::F16: return state.ptr_func_uint;
		case IR::Type::F32: return state.ptr_func_float;
		case IR::Type::U64:
		case IR::Type::U32x2: return state.ptr_func_uint_pair;
		case IR::Type::U32x4: return state.ptr_func_vec4_uint;
		case IR::Type::F32x2: return state.ptr_func_vec2_float;
		default: return 0;
	}
}

void StorePhiEdge(ValueEmitContext& ctx, const IR::Block* from, const IR::Block* to) {
	if (to == nullptr) {
		return;
	}
	for (const auto& phi: *to) {
		if (phi.GetOpcode() != IR::ValueOpcode::Phi) {
			continue;
		}
		for (size_t index = 0; index < phi.NumArgs(); index++) {
			if (phi.PhiBlock(index) == from) {
				ctx.state.builder.AddFunction(
				    {OpStore, ctx.phi_variables.at(&phi), ctx.Def(phi.Arg(index))});
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
			StorePhiEdge(ctx, block, target);
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
			StorePhiEdge(ctx, block, true_block);
			StorePhiEdge(ctx, block, false_block);
			const auto condition = ctx.Def(info.condition);
			emit_merge();
			ctx.state.builder.AddFunction(
			    {OpBranchConditional, condition, ctx.Label(true_block), ctx.Label(false_block)});
			return;
		}
		default: EmitReturn(ctx); return;
	}
}

void EmitDispatcherTarget(ValueEmitContext& ctx, const IR::Block* from, uint32_t target) {
	const auto* block = TargetBlock(ctx.program, target);
	if (block != nullptr) {
		StorePhiEdge(ctx, from, block);
	}
}

void EmitDispatcherTerminator(ValueEmitContext& ctx, const IR::Block* block,
                              const IR::ValueBlockInfo& info, uint32_t exit_label) {
	const auto& term = info.terminator;
	switch (term.kind) {
		case CFG::TerminatorKind::Branch:
			EmitDispatcherTarget(ctx, block, term.true_block);
			ctx.state.builder.AddFunction(
			    {OpStore, ctx.state.dispatch_pc_variable, ConstantU32(ctx.state, term.true_block)});
			break;
		case CFG::TerminatorKind::ConditionalBranch: {
			EmitDispatcherTarget(ctx, block, term.true_block);
			EmitDispatcherTarget(ctx, block, term.false_block);
			const auto selected = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction({OpSelect, ctx.state.uint_type, selected,
			                               ctx.Def(info.condition),
			                               ConstantU32(ctx.state, term.true_block),
			                               ConstantU32(ctx.state, term.false_block)});
			ctx.state.builder.AddFunction({OpStore, ctx.state.dispatch_pc_variable, selected});
			break;
		}
		case CFG::TerminatorKind::IndirectBranch: {
			for (const auto target: term.indirect_targets) {
				EmitDispatcherTarget(ctx, block, target);
			}
			for (const auto target: term.indirect_selector_targets) {
				EmitDispatcherTarget(ctx, block, target);
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
					ctx.state.builder.AddFunction({OpIEqual, ctx.state.bool_type, match, selector,
					                               ConstantU32(ctx.state, values[index])});
					ctx.state.builder.AddFunction({OpSelect, ctx.state.uint_type, next, match,
					                               ConstantU32(ctx.state, targets[index]),
					                               selected});
					selected = next;
				}
			}
			ctx.state.builder.AddFunction({OpStore, ctx.state.dispatch_pc_variable, selected});
			break;
		}
		default:
			ctx.state.builder.AddFunction(
			    {OpStore, ctx.state.dispatch_pc_variable, ConstantU32(ctx.state, UINT32_MAX)});
			break;
	}
	ctx.state.builder.AddFunction({OpBranch, exit_label});
}

bool EmitInstruction(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto finish = [&]() {
		if (const auto found = ctx.dispatcher_variables.find(&inst);
		    found != ctx.dispatcher_variables.end()) {
			ctx.state.builder.AddFunction(
			    {OpStore, found->second, ctx.Def(IR::Value(const_cast<IR::Inst*>(&inst)))});
		}
		return true;
	};
	if (inst.GetOpcode() == IR::ValueOpcode::Phi) {
		ctx.state.builder.AddFunction(
		    {OpLoad, ctx.TypeId(inst.GetType()), ctx.Result(inst), ctx.phi_variables.at(&inst)});
		return finish();
	}
	if (EmitValueFlow(ctx, inst) || EmitValueAlu(ctx, inst) || EmitValueMemory(ctx, inst) ||
	    EmitValueImage(ctx, inst)) {
		return finish();
	}
	ctx.Fail(inst, "has no direct SPIR-V emitter");
	return false;
}

void EmitBlock(ValueEmitContext& ctx, const IR::Block* block) {
	ctx.current_block = block;
	ctx.state.builder.AddFunction({OpLabel, ctx.Label(block)});
	for (const auto& inst: *block) {
		if (!EmitInstruction(ctx, inst)) {
			return;
		}
	}
}

void EmitStructuredFunction(ValueEmitContext& ctx) {
	ctx.state.builder.AddFunction({OpBranch, ctx.Label(ctx.program.blocks.front())});
	for (size_t index = 0; index < ctx.program.blocks.size() && !ctx.failed; index++) {
		EmitBlock(ctx, ctx.program.blocks[index]);
		EmitStructuredTerminator(ctx, ctx.program.blocks[index], ctx.program.block_info[index]);
	}
}

void EmitDispatcherFunction(ValueEmitContext& ctx) {
	auto&       state = ctx.state;
	const auto* entry = ctx.program.blocks.front();
	state.builder.AddFunction({OpBranch, ctx.Label(entry)});
	EmitBlock(ctx, entry);
	EmitDispatcherTerminator(ctx, entry, ctx.program.block_info.front(),
	                         state.dispatch_header_label);

	state.builder.AddFunction({OpLabel, state.dispatch_header_label});
	const auto pc = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, state.uint_type, pc, state.dispatch_pc_variable});
	const auto done = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpIEqual, state.bool_type, done, pc, ConstantU32(ctx.state, UINT32_MAX)});
	state.builder.AddFunction(
	    {OpLoopMerge, state.dispatch_merge_label, state.dispatch_continue_label, LoopControlNone});
	state.builder.AddFunction(
	    {OpBranchConditional, done, state.dispatch_merge_label, state.dispatch_select_label});

	state.builder.AddFunction({OpLabel, state.dispatch_select_label});
	state.builder.AddFunction(
	    {OpSelectionMerge, state.dispatch_after_switch_label, SelectionControlNone});
	std::vector<uint32_t> words {OpSwitch, pc, state.dispatch_default_label};
	for (size_t index = 1; index < ctx.program.blocks.size(); index++) {
		words.push_back(ctx.program.block_info[index].id);
		words.push_back(ctx.Label(ctx.program.blocks[index]));
	}
	state.builder.AddFunction(words);
	state.builder.AddFunction({OpLabel, state.dispatch_default_label});
	state.builder.AddFunction(
	    {OpStore, state.dispatch_pc_variable, ConstantU32(ctx.state, UINT32_MAX)});
	state.builder.AddFunction({OpBranch, state.dispatch_after_switch_label});

	for (size_t index = 1; index < ctx.program.blocks.size() && !ctx.failed; index++) {
		EmitBlock(ctx, ctx.program.blocks[index]);
		EmitDispatcherTerminator(ctx, ctx.program.blocks[index], ctx.program.block_info[index],
		                         state.dispatch_after_switch_label);
	}
	state.builder.AddFunction({OpLabel, state.dispatch_after_switch_label});
	state.builder.AddFunction({OpBranch, state.dispatch_continue_label});
	state.builder.AddFunction({OpLabel, state.dispatch_continue_label});
	state.builder.AddFunction({OpBranch, state.dispatch_header_label});
	state.builder.AddFunction({OpLabel, state.dispatch_merge_label});
	EmitReturn(ctx);
}

} // namespace

uint32_t ValueEmitContext::TypeId(IR::Type type) const {
	switch (type) {
		case IR::Type::U1: return state.bool_type;
		case IR::Type::U8:
		case IR::Type::U16:
		case IR::Type::U32:
		case IR::Type::F16: return state.uint_type;
		case IR::Type::U64:
		case IR::Type::U32x2: return state.uint_pair_type;
		case IR::Type::F32: return state.float_type;
		case IR::Type::U32x4: return state.vec4_uint_type;
		case IR::Type::F32x2: return state.vec2_float_type;
		default: return 0;
	}
}

uint32_t ValueEmitContext::Def(IR::Value value) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		switch (value.GetType()) {
			case IR::Type::U1: return BoolConstant(state, value.U1());
			case IR::Type::U8: return ConstantU32(state, value.U8());
			case IR::Type::U16: return ConstantU32(state, value.U16());
			case IR::Type::U32: return ConstantU32(state, value.U32());
			case IR::Type::U64: return ConstantPair(state, value.U64());
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
	if (program.dispatcher_fallback && current_block != nullptr &&
	    inst->Parent() != current_block) {
		if (const auto found = dispatcher_variables.find(inst);
		    found != dispatcher_variables.end()) {
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
	ValueEmitContext ctx(state, program);
	if (state.stage == ShaderType::Pixel && state.needs_pixel_valid_mask) {
		state.pixel_valid_mask_variable = state.builder.AllocateId();
		state.builder.AddName(state.pixel_valid_mask_variable, "pixel_valid_mask_active");
	}
	for (const auto* block: program.blocks) {
		ctx.labels.emplace(block, state.builder.AllocateId());
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != IR::ValueOpcode::Phi) {
				continue;
			}
			const auto ptr_type = PhiPointerType(state, inst.GetType());
			if (ptr_type == 0) {
				ctx.failed = true;
				ctx.error  = fmt::format("typed opcode Phi of type {} cannot be spilled across a "
				                         "control-flow edge",
				                         IR::TypeName(inst.GetType()));
				break;
			}
			ctx.phi_variables.emplace(&inst, state.builder.AllocateId());
		}
	}
	if (ctx.failed) {
		SetError(error, ctx.error.c_str());
		return false;
	}
	if (program.dispatcher_fallback) {
		const auto mark_cross_block = [&](IR::Value value, const IR::Block* consumer) {
			value                  = value.Resolve();
			const auto* definition = value.TryInstruction();
			if (definition == nullptr || definition->Parent() == consumer ||
			    PhiPointerType(state, definition->GetType()) == 0) {
				return;
			}
			if (!ctx.dispatcher_variables.contains(definition)) {
				ctx.dispatcher_variables.emplace(definition, state.builder.AllocateId());
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
		state.dispatcher_fallback         = true;
		state.dispatch_pc_variable        = state.builder.AllocateId();
		state.dispatch_header_label       = state.builder.AllocateId();
		state.dispatch_select_label       = state.builder.AllocateId();
		state.dispatch_default_label      = state.builder.AllocateId();
		state.dispatch_after_switch_label = state.builder.AllocateId();
		state.dispatch_continue_label     = state.builder.AllocateId();
		state.dispatch_merge_label        = state.builder.AllocateId();
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
	    {OpFunction, state.void_type, state.main_func, FunctionControlNone, state.func_type});
	state.builder.AddFunction({OpLabel, state.entry_label});
	if (state.needs_function_lds) {
		state.builder.AddFunction(
		    {OpVariable, state.ptr_workgroup_array, state.lds_variable, StorageClassFunction});
	}
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction({OpVariable, state.ptr_func_uint, state.pixel_valid_mask_variable,
		                           StorageClassFunction});
	}
	if (program.dispatcher_fallback) {
		state.builder.AddFunction(
		    {OpVariable, state.ptr_func_uint, state.dispatch_pc_variable, StorageClassFunction});
	}
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != IR::ValueOpcode::Phi) {
				continue;
			}
			state.builder.AddFunction({OpVariable, PhiPointerType(state, inst.GetType()),
			                           ctx.phi_variables.at(&inst), StorageClassFunction});
		}
	}
	if (program.dispatcher_fallback) {
		for (const auto* block: program.blocks) {
			for (const auto& inst: *block) {
				if (const auto found = ctx.dispatcher_variables.find(&inst);
				    found != ctx.dispatcher_variables.end()) {
					state.builder.AddFunction({OpVariable, PhiPointerType(state, inst.GetType()),
					                           found->second, StorageClassFunction});
				}
			}
		}
	}
	if (ctx.scratch_u32_variable != 0) {
		state.builder.AddFunction(
		    {OpVariable, state.ptr_func_uint, ctx.scratch_u32_variable, StorageClassFunction});
	}
	if (state.pixel_valid_mask_variable != 0) {
		state.builder.AddFunction(
		    {OpStore, state.pixel_valid_mask_variable, ConstantU32(state, 1)});
	}
	EmitStorageBufferOffsets(state);
	if (program.blocks.empty()) {
		EmitReturn(ctx);
	} else if (program.dispatcher_fallback) {
		EmitDispatcherFunction(ctx);
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
