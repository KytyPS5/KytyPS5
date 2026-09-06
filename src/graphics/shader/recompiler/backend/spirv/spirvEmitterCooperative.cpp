#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>
#include <limits>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {
using O = IR::ValueOpcode;
constexpr uint32_t Waiting = 0x80000000u;
constexpr uint32_t Finished = UINT32_MAX;

struct Segment {
	const IR::Block* block = nullptr;
	std::vector<const IR::Inst*> instructions;
	uint32_t label = 0;
	uint32_t barrier_successor = Finished;
};

uint32_t Binary(EmitterState& state, uint32_t opcode, uint32_t type, uint32_t lhs, uint32_t rhs) {
	const auto id = state.builder.AllocateId();
	state.builder.AddFunction({opcode, type, id, lhs, rhs});
	return id;
}

uint32_t Select(EmitterState& state, uint32_t condition, uint32_t yes, uint32_t no) {
	const auto id = state.builder.AllocateId();
	state.builder.AddFunction({OpSelect, TypeU32(state), id, condition, yes, no});
	return id;
}

uint32_t LoadPc(EmitterState& state, uint32_t variable) {
	const auto id = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, TypeU32(state), id, variable});
	return id;
}

void Rendezvous(EmitterState& state) {
	state.builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
	                           ConstantU32(state, ScopeWorkgroup), ConstantU32(state,
	                           MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory)});
}

uint32_t ScratchPointer(EmitterState& state, uint32_t index) {
	const auto id = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, TypeU32ElementPointer(state, StorageClassWorkgroup),
	                           id, state.wave_scratch_variable, index});
	return id;
}

template <typename Body>
void Guard(EmitterState& state, uint32_t active, Body&& body) {
	const auto yes = state.builder.AllocateId();
	const auto merge = state.builder.AllocateId();
	state.builder.AddFunction({OpSelectionMerge, merge, SelectionControlNone});
	state.builder.AddFunction({OpBranchConditional, active, yes, merge});
	EmitLabel(state, yes);
	body();
	state.builder.AddFunction({OpBranch, merge});
	EmitLabel(state, merge);
}

bool IsCollective(O op) {
	switch (op) {
		case O::Ballot: case O::ReadLane: case O::ReadFirstLane: case O::WqmMask:
		case O::DppMoveU32: case O::Dpp8MoveU32: case O::Permlane16U32:
		case O::SwizzleU32: case O::BpermuteU32: return true;
		default: return false;
	}
}

bool IsLds(const IR::Program& program, const IR::Inst& inst) {
	if (IR::SharedAccessOf(inst.GetOpcode()) == IR::SharedAccess::None) return false;
	const auto index = inst.Flags<IR::MemoryFlags>().index;
	return index < program.memory_info.size() && program.memory_info[index].kind == IR::ResourceKind::Lds;
}

void StoreResult(ValueEmitContext& ctx, const CooperativeFunctionState& function, const IR::Inst& inst) {
	if (const auto slot = function.spills.find(&inst); slot != function.spills.end()) {
		const auto value = ctx.definitions.find(&inst);
		if (value == ctx.definitions.end()) ctx.Fail(inst, "cooperative instruction did not define its result");
		ctx.state.builder.AddFunction({OpStore, slot->second, value->second});
	}
}

void EmitSegmentInstructions(ValueEmitContext& ctx, const CooperativeFunctionState& function,
                             const Segment& segment, uint32_t active) {
	// All invocations execute each software collective. Only the selected guest
	// waves commit results. Ordinary instructions (including scalar reads and
	// bounds checks) execute solely for those waves, before touching any address.
	for (size_t index = 0; index < segment.instructions.size();) {
		const auto& inst = *segment.instructions[index];
		if (inst.GetOpcode() == O::Phi) { ++index; continue; }
		if (IsCollective(inst.GetOpcode())) {
			ctx.cooperative_collective_active = active;
			EmitDirectValueInstruction(ctx, inst);
			ctx.cooperative_collective_active = 0;
			Guard(ctx.state, active, [&] { StoreResult(ctx, function, inst); });
			++index;
			continue;
		}
		bool lds = false;
		Guard(ctx.state, active, [&] {
			// Coalesce ordinary instructions into one selection. A DS phase ends
			// it, so its rendezvous remains outside every wave/EXEC/bounds guard.
			do {
				const auto& current = *segment.instructions[index++];
				if (current.GetOpcode() != O::Phi) {
					EmitDirectValueInstruction(ctx, current);
					StoreResult(ctx, function, current);
				}
				lds = IsLds(ctx.program, current);
			} while (!lds && index < segment.instructions.size() &&
			         !IsCollective(segment.instructions[index]->GetOpcode()));
		});
		// Atomic completion is a phase too: OpMemoryBarrier in an active atomic
		// path alone does not rendezvous the two native halves of a guest wave.
		if (lds) Rendezvous(ctx.state);
	}
}

void CopyPhiEdge(ValueEmitContext& ctx, const CooperativeFunctionState& function,
                 const IR::Block* from, const IR::Block* to) {
	// Phi assignments are parallel and belong only to the chosen guest edge.
	// Load every incoming value before overwriting any destination, including
	// mutually dependent loop-carried values and entry/same-block definitions.
	std::vector<std::pair<uint32_t, uint32_t>> copies;
	for (const auto& phi : *to) {
		if (phi.GetOpcode() != O::Phi) break;
		bool found = false;
		for (size_t arg = 0; arg < phi.NumArgs(); ++arg) {
			if (phi.PhiBlock(arg) != from) continue;
			copies.emplace_back(function.spills.at(&phi), ctx.Def(phi.Arg(arg)));
			found = true;
			break;
		}
		if (!found) ctx.Fail(phi, "cooperative edge is missing a Phi input");
	}
	for (const auto& [slot, value] : copies) ctx.state.builder.AddFunction({OpStore, slot, value});
}
} // namespace

CooperativeFunctionState PrepareCooperativeFunction(ValueEmitContext& ctx) {
	CooperativeFunctionState function;
	function.pc_variable = ctx.state.builder.AllocateId();
	function.cursor_variable = ctx.state.builder.AllocateId();
	for (const auto* block : ctx.program.blocks) for (const auto& inst : *block) {
		// Opaque resource/address recipes are compile-time structures. Every
		// runtime scalar/vector leaf receives a slot, even across a barrier
		// inside one IR block. Planning-only raw SRT reads never execute.
		if (ctx.TypeId(inst.GetType()) == 0) continue;
		if ((inst.GetOpcode() == O::LoadAddressU32 || inst.GetOpcode() == O::ReadConstBuffer) &&
		    ctx.Memory(inst).planning_only) continue;
		function.spills.emplace(&inst, ctx.state.builder.AllocateId());
	}
	return function;
}

void DeclareCooperativeFunctionVariables(ValueEmitContext& ctx, const CooperativeFunctionState& function) {
	const auto word_pointer = TypePointer(ctx.state, StorageClassFunction, TypeU32(ctx.state));
	for (const auto variable : {function.pc_variable, function.cursor_variable})
		ctx.state.builder.AddFunction({OpVariable, word_pointer, variable, StorageClassFunction});
	// Follow instruction order to keep generated modules deterministic.
	for (const auto* block : ctx.program.blocks) for (const auto& inst : *block) {
		if (const auto found = function.spills.find(&inst); found != function.spills.end())
			ctx.state.builder.AddFunction({OpVariable,
			    TypePointer(ctx.state, StorageClassFunction, ctx.TypeId(inst.GetType())),
			    found->second, StorageClassFunction});
	}
}

void EmitCooperativeFunction(ValueEmitContext& ctx, const CooperativeFunctionState& function) {
	auto& state = ctx.state;
	std::vector<Segment> segments;
	std::unordered_map<const IR::Block*, uint32_t> first_segment;
	std::unordered_map<uint32_t, const IR::Block*> blocks;
	std::unordered_map<const IR::Block*, const IR::BlockInfo*> info;
	for (size_t index = 0; index < ctx.program.blocks.size(); ++index) {
		const auto* block = ctx.program.blocks[index];
		blocks.emplace(ctx.program.block_info[index].id, block);
		info.emplace(block, &ctx.program.block_info[index]);
		first_segment.emplace(block, static_cast<uint32_t>(segments.size()));
		segments.push_back({block, {}, state.builder.AllocateId()});
		for (const auto& inst : *block) {
			if (inst.GetOpcode() == O::Barrier) {
				if (segments.size() >= Waiting) ctx.Fail("cooperative segment count overflows");
				segments.back().barrier_successor = static_cast<uint32_t>(segments.size());
				segments.push_back({block, {}, state.builder.AllocateId()});
			} else {
				segments.back().instructions.push_back(&inst);
			}
		}
	}
	if (segments.empty() || segments.size() >= Waiting) ctx.Fail("invalid cooperative segment count");
	const auto header = state.builder.AllocateId();
	const auto dispatch = state.builder.AllocateId();
	const auto invalid = state.builder.AllocateId();
	const auto after_switch = state.builder.AllocateId();
	const auto continuation = state.builder.AllocateId();
	const auto exit = state.builder.AllocateId();
	const auto& shape = state.compute_execution.layout.host_size;
	const auto wave_count = shape[0] * shape[1] * shape[2] / 64u;
	const auto zero = ConstantU32(state, 0);
	const auto finished = ConstantU32(state, Finished);
	state.builder.AddFunction({OpStore, function.pc_variable, zero});
	state.builder.AddFunction({OpStore, function.cursor_variable, zero});
	state.builder.AddFunction({OpBranch, header});
	EmitLabel(state, header);
	const auto own_pc = LoadPc(state, function.pc_variable);
	const auto cursor = LoadPc(state, function.cursor_variable);
	state.builder.AddFunction({OpStore, ScratchPointer(state, EmitHostLocalInvocationIndex(state)), own_pc});
	Rendezvous(state);
	std::vector<uint32_t> pcs;
	for (uint32_t wave = 0; wave < wave_count; ++wave)
		pcs.push_back(LoadPc(state, ScratchPointer(state, ConstantU32(state, wave * 64u))));
	// All invocations read the same wave PCs before any wave helper can reuse
	// scratch. Finished guest waves remain physical participants until all end.
	Rendezvous(state);
	uint32_t same = ConstantBool(state, true);
	for (uint32_t wave = 1; wave < wave_count; ++wave)
		same = Binary(state, OpLogicalAnd, TypeBool(state), same,
		              Binary(state, OpIEqual, TypeBool(state), pcs[0], pcs[wave]));
	const auto all_done = Binary(state, OpLogicalAnd, TypeBool(state), same,
	                            Binary(state, OpIEqual, TypeBool(state), pcs[0], finished));
	const auto waiting = Binary(state, OpUGreaterThanEqual, TypeBool(state), pcs[0], ConstantU32(state, Waiting));
	const auto release = Binary(state, OpLogicalAnd, TypeBool(state), same,
	    Binary(state, OpLogicalAnd, TypeBool(state), waiting,
	           Binary(state, OpINotEqual, TypeBool(state), pcs[0], finished)));
	uint32_t selected_pc = finished;
	uint32_t selected_wave = zero;
	uint32_t best_distance = ConstantU32(state, wave_count);
	for (uint32_t wave = 0; wave < wave_count; ++wave) {
		const auto distance = Binary(state, OpUMod, TypeU32(state),
		    Binary(state, OpISub, TypeU32(state), ConstantU32(state, wave + wave_count), cursor),
		    ConstantU32(state, wave_count));
		const auto choose = Binary(state, OpLogicalAnd, TypeBool(state),
		    Binary(state, OpULessThan, TypeBool(state), pcs[wave], ConstantU32(state, Waiting)),
		    Binary(state, OpULessThan, TypeBool(state), distance, best_distance));
		selected_pc = Select(state, choose, pcs[wave], selected_pc);
		selected_wave = Select(state, choose, ConstantU32(state, wave), selected_wave);
		best_distance = Select(state, choose, distance, best_distance);
	}
	const auto released_pc = Binary(state, OpBitwiseAnd, TypeU32(state), pcs[0], ConstantU32(state, ~Waiting));
	selected_pc = Select(state, release, released_pc, selected_pc);
	const auto current_pc = Select(state, release, released_pc, own_pc);
	state.builder.AddFunction({OpStore, function.pc_variable, current_pc});
	const auto next_cursor = Binary(state, OpUMod, TypeU32(state),
	    Binary(state, OpIAdd, TypeU32(state), selected_wave, ConstantU32(state, 1)),
	    ConstantU32(state, wave_count));
	state.builder.AddFunction({OpStore, function.cursor_variable, next_cursor});
	state.builder.AddFunction({OpLoopMerge, exit, continuation, LoopControlNone});
	state.builder.AddFunction({OpBranchConditional, all_done, exit, dispatch});
	EmitLabel(state, dispatch);
	const auto active = Binary(state, OpIEqual, TypeBool(state), current_pc, selected_pc);
	state.builder.AddFunction({OpSelectionMerge, after_switch, SelectionControlNone});
	std::vector<uint32_t> switches{OpSwitch, selected_pc, invalid};
	for (uint32_t index = 0; index < segments.size(); ++index) {
		switches.push_back(index);
		switches.push_back(segments[index].label);
	}
	state.builder.AddFunction(switches);
	EmitLabel(state, invalid);
	// The planner proves all barrier sites form an acyclic dominating chain;
	// non-finished waves therefore cannot get stuck at different barriers.
	// An invalid PC is not permission to report successful guest completion.
	state.builder.AddFunction({OpUnreachable});
	for (const auto& segment : segments) {
		ctx.current_block = segment.block;
		EmitLabel(state, segment.label);
		EmitSegmentInstructions(ctx, function, segment, active);
		Guard(state, active, [&] {
			if (segment.barrier_successor != Finished) {
				state.builder.AddFunction({OpStore, function.pc_variable,
				                           ConstantU32(state, Waiting | segment.barrier_successor)});
				return;
			}
			const auto& branch = *info.at(segment.block);
			const auto edge = [&](uint32_t id) {
				const auto target = blocks.find(id);
				if (target == blocks.end()) ctx.Fail("cooperative branch has an unknown target");
				CopyPhiEdge(ctx, function, segment.block, target->second);
				state.builder.AddFunction({OpStore, function.pc_variable,
				                           ConstantU32(state, first_segment.at(target->second))});
			};
			switch (branch.terminator.kind) {
				case CFG::TerminatorKind::Branch: edge(branch.terminator.true_block); break;
				case CFG::TerminatorKind::ConditionalBranch: {
					const auto yes = state.builder.AllocateId();
					const auto no = state.builder.AllocateId();
					const auto merge = state.builder.AllocateId();
					const auto condition = ctx.Def(branch.condition);
					state.builder.AddFunction({OpSelectionMerge, merge, SelectionControlNone});
					state.builder.AddFunction({OpBranchConditional, condition, yes, no});
					EmitLabel(state, yes); edge(branch.terminator.true_block);
					state.builder.AddFunction({OpBranch, merge});
					EmitLabel(state, no); edge(branch.terminator.false_block);
					state.builder.AddFunction({OpBranch, merge});
					EmitLabel(state, merge);
					break;
				}
				case CFG::TerminatorKind::Return:
					state.builder.AddFunction({OpStore, function.pc_variable, finished}); break;
				default: ctx.Fail("cooperative execution requires direct guest control flow");
			}
		});
		state.builder.AddFunction({OpBranch, after_switch});
	}
	EmitLabel(state, after_switch);
	state.builder.AddFunction({OpBranch, continuation});
	EmitLabel(state, continuation);
	state.builder.AddFunction({OpBranch, header});
	EmitLabel(state, exit);
	state.builder.AddFunction({OpReturn});
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
