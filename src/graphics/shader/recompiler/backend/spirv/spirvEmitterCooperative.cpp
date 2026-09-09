#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

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

void Rendezvous(EmitterState& state, uint32_t memory = MemorySemanticsWorkgroupMemory) {
	state.builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
	                           ConstantU32(state, ScopeWorkgroup), ConstantU32(state,
	                           MemorySemanticsAcquireRelease | memory)});
}

uint32_t ScratchPointer(EmitterState& state, uint32_t index) {
	const auto id = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, TypeU32ElementPointer(state, StorageClassWorkgroup),
	                           id, state.wave_scratch_variable, EmitWaveScratchIndex(state, index)});
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

bool IsReadOnlyLds(const IR::Program& program, const IR::Inst& inst) {
	return IsLds(program, inst) &&
	       IR::SharedAccessOf(inst.GetOpcode()) == IR::SharedAccess::Read;
}

bool IsRuntimeScalarRead(const ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op == O::ReadConstBuffer) return !ctx.Memory(inst).planning_only;
	return op == O::LoadAddressU32 &&
	       ctx.Memory(inst).kind == IR::ResourceKind::ScalarAddress &&
	       !ctx.Memory(inst).planning_only;
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
		if (IsRuntimeScalarRead(ctx, inst)) {
			// Only the selected guest wave may touch its descriptor, but every
			// physical invocation must join the cross-subgroup rendezvous. Publish
			// the raw host loads first, broadcast this wave's lane zero, then commit
			// the architectural scalar result for the selected wave.
			ctx.cooperative_phase = function.phases.at(&inst);
			Guard(ctx.state, active, [&] {
				EmitDirectValueInstruction(ctx, inst);
				StoreResult(ctx, function, inst);
			});
			ctx.cooperative_phase = 0;
			ctx.cooperative_collective_active = active;
			const auto source = ctx.Def(IR::Value(const_cast<IR::Inst*>(&inst)));
			const auto value = EmitWaveReadLane(ctx.state, source, ConstantU32(ctx.state, 0));
			ctx.cooperative_collective_active = 0;
			Guard(ctx.state, active, [&] {
				ctx.state.builder.AddFunction({OpStore, function.spills.at(&inst), value});
			});
			++index;
			continue;
		}
		if (IsCollective(inst.GetOpcode())) {
			ctx.cooperative_phase = function.phases.at(&inst);
			ctx.cooperative_collective_active = active;
			EmitDirectValueInstruction(ctx, inst);
			ctx.cooperative_collective_active = 0;
			ctx.cooperative_phase = 0;
			Guard(ctx.state, active, [&] { StoreResult(ctx, function, inst); });
			++index;
			continue;
		}
		bool lds = false;
		const auto phase = function.phases.at(segment.instructions[index]);
		ctx.cooperative_phase = phase;
		Guard(ctx.state, active, [&] {
			// Coalesce ordinary instructions and consecutive read-only DS accesses
			// into one selection. Conflicting DS accesses retain separate phases,
			// so the rendezvous remains outside every wave/EXEC/bounds guard.
			do {
				const auto& current = *segment.instructions[index++];
				if (current.GetOpcode() != O::Phi) {
					EmitDirectValueInstruction(ctx, current);
					StoreResult(ctx, function, current);
				}
				lds |= IsLds(ctx.program, current);
			} while (index < segment.instructions.size() &&
			         segment.instructions[index]->GetOpcode() != O::Phi &&
			         !IsRuntimeScalarRead(ctx, *segment.instructions[index]) &&
			         !IsCollective(segment.instructions[index]->GetOpcode()) &&
			         function.phases.at(segment.instructions[index]) == phase);
		});
		ctx.cooperative_phase = 0;
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
	uint32_t next_phase = 1;
	for (const auto* block : ctx.program.blocks) {
		uint32_t ordinary_phase = 0;
		bool ordinary_phase_has_lds_read = false;
		for (const auto& inst : *block) {
			const auto op = inst.GetOpcode();
			if (op == O::Phi) {
				continue;
			}
			if (op == O::Barrier) {
				ordinary_phase = 0;
				ordinary_phase_has_lds_read = false;
				continue;
			}
			if (IsRuntimeScalarRead(ctx, inst) ||
			    IsCollective(op)) {
				function.phases.emplace(&inst, next_phase++);
				ordinary_phase = 0;
				ordinary_phase_has_lds_read = false;
				continue;
			}
			const bool read_only_lds = IsReadOnlyLds(ctx.program, inst);
			if (IsLds(ctx.program, inst) && !read_only_lds && ordinary_phase_has_lds_read) {
				ordinary_phase = 0;
				ordinary_phase_has_lds_read = false;
			}
			if (ordinary_phase == 0) {
				ordinary_phase = next_phase++;
			}
			function.phases.emplace(&inst, ordinary_phase);
			if (read_only_lds) {
				ordinary_phase_has_lds_read = true;
			} else if (IsLds(ctx.program, inst)) {
				ordinary_phase = 0;
				ordinary_phase_has_lds_read = false;
			}
		}
	}
	std::unordered_set<const IR::Inst*> branch_conditions;
	for (const auto& block : ctx.program.block_info) {
		if (const auto* condition = block.condition.Resolve().TryInstruction(); condition != nullptr) {
			branch_conditions.insert(condition);
		}
	}
	for (const auto* block : ctx.program.blocks) for (const auto& inst : *block) {
		// Opaque resource/address recipes are compile-time structures. Runtime
		// values need Function storage only when a scheduler phase, CFG edge, or
		// Phi assignment can separate their definition from a consumer.
		if (ctx.TypeId(inst.GetType()) == 0) continue;
		if ((inst.GetOpcode() == O::LoadAddressU32 || inst.GetOpcode() == O::ReadConstBuffer) &&
		    ctx.Memory(inst).planning_only) continue;
		const auto phase = function.phases.find(&inst);
		bool spill = inst.GetOpcode() == O::Phi || IsRuntimeScalarRead(ctx, inst) ||
		             branch_conditions.contains(&inst) || phase == function.phases.end();
		for (const auto& use : inst.Uses()) {
			const auto user_phase = function.phases.find(use.user);
			if (phase == function.phases.end() || user_phase == function.phases.end() ||
			    user_phase->second != phase->second) {
				spill = true;
				break;
			}
		}
		if (spill) function.spills.emplace(&inst, ctx.state.builder.AllocateId());
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
	const auto schedule = state.builder.AllocateId();
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
	state.builder.AddFunction({OpLoopMerge, exit, continuation, LoopControlNone});
	state.builder.AddFunction({OpBranch, schedule});
	EmitLabel(state, schedule);
	const auto own_pc = LoadPc(state, function.pc_variable);
	const auto local_index = EmitHostLocalInvocationIndex(state);
	state.builder.AddFunction({OpStore, ScratchPointer(state, local_index), own_pc});
	// Every physical invocation, including finished guest waves, publishes its
	// previous quantum before selecting the next one. UniformMemory and coherent
	// SSBO declarations jointly provide peer-buffer visibility.
	Rendezvous(state, MemorySemanticsWorkgroupMemory | MemorySemanticsUniformMemory);
	const auto cursor = LoadPc(state, function.cursor_variable);
	const bool power_of_two_wave_count = (wave_count & (wave_count - 1u)) == 0u;
	std::vector<uint32_t> pcs;
	for (uint32_t wave = 0; wave < wave_count; ++wave)
		pcs.push_back(LoadPc(state, ScratchPointer(state, ConstantU32(state, wave * 64u))));
	// All invocations finish reading the scheduler state before a selected wave
	// may reuse the same array for a software collective.
	Rendezvous(state);
	uint32_t all_done = ConstantBool(state, true);
	uint32_t has_waiter = ConstantBool(state, false);
	uint32_t all_satisfied = ConstantBool(state, true);
	for (uint32_t wave = 0; wave < wave_count; ++wave) {
		const auto is_finished = Binary(state, OpIEqual, TypeBool(state), pcs[wave], finished);
		const auto is_waiting = Binary(state, OpLogicalAnd, TypeBool(state),
		    Binary(state, OpUGreaterThanEqual, TypeBool(state), pcs[wave],
		           ConstantU32(state, Waiting)),
		    Binary(state, OpINotEqual, TypeBool(state), pcs[wave], finished));
		all_done = Binary(state, OpLogicalAnd, TypeBool(state), all_done, is_finished);
		has_waiter = Binary(state, OpLogicalOr, TypeBool(state), has_waiter, is_waiting);
		const auto satisfied = Binary(state, OpLogicalOr, TypeBool(state), is_finished, is_waiting);
		all_satisfied = Binary(state, OpLogicalAnd, TypeBool(state), all_satisfied, satisfied);
	}
	const auto release = Binary(state, OpLogicalAnd, TypeBool(state), has_waiter, all_satisfied);
	std::vector<uint32_t> effective_pcs;
	effective_pcs.reserve(wave_count);
	for (uint32_t wave = 0; wave < wave_count; ++wave) {
		const auto is_finished = Binary(state, OpIEqual, TypeBool(state), pcs[wave], finished);
		const auto successor = Binary(state, OpBitwiseAnd, TypeU32(state), pcs[wave],
		                              ConstantU32(state, ~Waiting));
		effective_pcs.push_back(
		    Select(state, release, Select(state, is_finished, finished, successor), pcs[wave]));
	}
	uint32_t selected_pc = finished;
	uint32_t selected_wave = zero;
	uint32_t best_distance = ConstantU32(state, wave_count);
	for (uint32_t wave = 0; wave < wave_count; ++wave) {
		const auto wave_id = ConstantU32(state, wave);
		const auto unwrapped_distance = Binary(state, OpISub, TypeU32(state),
		    ConstantU32(state, wave + wave_count), cursor);
		const auto distance = power_of_two_wave_count
		    ? Binary(state, OpBitwiseAnd, TypeU32(state), unwrapped_distance,
		          ConstantU32(state, wave_count - 1u))
		    : Binary(state, OpUMod, TypeU32(state), unwrapped_distance,
		          ConstantU32(state, wave_count));
		const auto choose = Binary(state, OpLogicalAnd, TypeBool(state),
		    Binary(state, OpULessThan, TypeBool(state), effective_pcs[wave],
		           ConstantU32(state, Waiting)),
		    Binary(state, OpULessThan, TypeBool(state), distance, best_distance));
		selected_pc = Select(state, choose, effective_pcs[wave], selected_pc);
		selected_wave = Select(state, choose, wave_id, selected_wave);
		best_distance = Select(state, choose, distance, best_distance);
	}
	const auto advanced_cursor = Binary(state, OpIAdd, TypeU32(state), selected_wave,
	                                    ConstantU32(state, 1));
	const auto next_cursor = power_of_two_wave_count
	    ? Binary(state, OpBitwiseAnd, TypeU32(state), advanced_cursor,
	          ConstantU32(state, wave_count - 1u))
	    : Binary(state, OpUMod, TypeU32(state), advanced_cursor,
	          ConstantU32(state, wave_count));
	state.builder.AddFunction({OpStore, function.cursor_variable, next_cursor});
	const auto own_finished = Binary(state, OpIEqual, TypeBool(state), own_pc, finished);
	const auto own_successor = Binary(state, OpBitwiseAnd, TypeU32(state), own_pc,
	                                 ConstantU32(state, ~Waiting));
	const auto released_own_pc = Select(state, own_finished, finished, own_successor);
	const auto current_pc = Select(state, release, released_own_pc, own_pc);
	state.builder.AddFunction({OpStore, function.pc_variable, current_pc});
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
