#include "graphics/shader/recompiler/ir/passes/RayQueryTraversal.h"

#include "common/logging/log.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

// The descriptor of traversals over 64-bit node pointers: base 0, maximum size, sorted boxes and
// triangle barycentrics.
constexpr std::array<uint32_t, 4> TraversalDescriptor {0u, 0x80000000u, 0xffffffffu, 0x810003ffu};
constexpr uint32_t                One = 0x3f800000u;

// Byte offsets in an instance record: BLAS header, root link and flags (mask in bits 8..15).
constexpr uint32_t InstanceBlas = 0, InstanceRoot = 56, InstanceFlags = 60;

Inst* Producer(Value value, ValueOpcode opcode) {
	auto* inst = value.Resolve().TryInstruction();
	return inst != nullptr && inst->GetOpcode() == opcode ? inst : nullptr;
}

std::optional<uint32_t> Immediate(Value value) {
	value = value.Resolve();
	if (!value.IsImmediate() || value.GetType() != Type::U32) return std::nullopt;
	return value.U32();
}

size_t Incoming(const Inst& phi, const Block* from) {
	for (size_t index = 0; index < phi.NumArgs(); ++index) {
		if (phi.PhiBlock(index) == from) return index;
	}
	return phi.NumArgs();
}

// What a value the loop carries can be after an iteration: Select arms and phis, up to the header.
void Collect(Value value, const Block* header, std::unordered_set<const Inst*>& reached) {
	const auto* inst = value.Resolve().TryInstruction();
	if (inst == nullptr || !reached.insert(inst).second) return;
	if (inst->GetOpcode() == ValueOpcode::SelectU32) {
		Collect(inst->Arg(1), header, reached);
		Collect(inst->Arg(2), header, reached);
	} else if (inst->GetOpcode() == ValueOpcode::Phi && inst->Parent() != header) {
		for (size_t index = 0; index < inst->NumArgs(); ++index) {
			Collect(inst->Arg(index), header, reached);
		}
	}
}

// Read other than as a phi operand or the value a Select keeps.
bool DirectlyUsed(const Inst& inst) {
	return std::ranges::any_of(inst.Uses(), [](const Use& use) {
		const auto op = use.user->GetOpcode();
		return op != ValueOpcode::Phi && (op != ValueOpcode::SelectU32 || use.operand != 2);
	});
}

bool GlobalLoad(const Program& program, const Inst& inst, uint32_t offset) {
	if (inst.GetOpcode() != ValueOpcode::LoadAddressU32) return false;
	const auto& memory = program.memory_info[inst.Flags<MemoryFlags>().index];
	return memory.kind == ResourceKind::Global && memory.address_is_full && memory.offset == offset;
}

void Redirect(Inst& inst, Value replacement, const Inst& keep) {
	const auto uses = inst.Uses();
	for (const auto& use: uses) {
		if (use.user != &keep) use.user->SetArg(use.operand, replacement);
	}
}

struct Inserter {
	Block*          block;
	Block::iterator at;

	Value operator()(ValueOpcode opcode, std::initializer_list<Value> args,
	                 uint64_t flags = 0) const {
		auto& inst = *block->PrependNewInst(at, opcode, args, flags);
		return TypeOf(opcode) == Type::Void ? Value {} : Value(&inst);
	}
};

Block::iterator Position(Inst& inst) {
	return std::ranges::find_if(*inst.Parent(), [&](const Inst& other) { return &other == &inst; });
}

size_t BlockIndex(const Program& program, const Block* block) {
	return static_cast<size_t>(std::ranges::find(program.blocks, block) - program.blocks.begin());
}

// The innermost loop around a block: the nearest header before it whose latch is at or after it.
Block* EnclosingLoop(const Program& program, const Block* block) {
	const auto at = BlockIndex(program, block);
	for (auto header = at; header-- > 0;) {
		const auto& term = program.blocks[header]->terminator;
		if (term.loop_header && term.continue_block != nullptr &&
		    BlockIndex(program, term.continue_block) >= at) {
			return program.blocks[header];
		}
	}
	return nullptr;
}

struct Traversal {
	Block*               preheader = nullptr;
	Block*               header    = nullptr;
	Block*               latch     = nullptr;
	Inst*                bvh       = nullptr;
	Inst*                exec      = nullptr; // The intersection's lanes.
	Inst*                extent    = nullptr; // The closest committed t.
	Inst*                vote      = nullptr; // The latch's vote on whether lanes go on.
	std::vector<Inst*>   lanes;               // Phis that carry the voted lanes.
	std::array<Value, 7> ray {};              // World origin xyz, direction xyz, t_max.
	// Words 0 and 1 of the current prim record, the current instance address and flags.
	std::array<Inst*, 5> state {};
	// The loads of prim record words 0 and 1 and of instance flags.
	std::array<const Inst*, 3> loads {};
	std::array<uint32_t, 2>    tlas {};
	uint32_t                   cull_mask = 0;
};

bool Match(const Program& program, Inst& bvh, Traversal& t) {
	const auto* descriptor = Producer(bvh.Arg(0), ValueOpcode::CompositeConstructU32x4);
	const auto* address    = Producer(bvh.Arg(1), ValueOpcode::MakeImageAddress);
	if (descriptor == nullptr || address == nullptr || bvh.Flags<uint32_t>() != 2u) return false;
	for (uint32_t word = 0; word < 4; ++word) {
		if (Immediate(descriptor->Arg(word)) != TraversalDescriptor[word]) return false;
	}
	if (!std::ranges::all_of(bvh.Uses(), [](const Use& use) {
		    return use.user->GetOpcode() == ValueOpcode::CompositeExtractU32x4 &&
			       Immediate(use.user->Arg(1)) < 4u;
	    })) {
		return false;
	}

	// The innermost loop around the intersection, entered from one preheader.
	t.header = EnclosingLoop(program, bvh.Parent());
	if (t.header == nullptr || t.header->terminator.kind != CFG::TerminatorKind::Branch ||
	    t.header->terminator.true_block == nullptr) {
		return false;
	}
	t.latch            = t.header->terminator.continue_block;
	const auto entries = t.header->ImmPredecessors();
	if (entries.size() != 2 || std::ranges::find(entries, t.latch) == entries.end()) return false;
	t.preheader      = entries[0] == t.latch ? entries[1] : entries[0];
	const auto entry = [&](const Inst& phi) { return phi.Arg(Incoming(phi, t.preheader)); };
	const auto back  = [&](const Inst& phi) { return phi.Arg(Incoming(phi, t.latch)); };
	const auto phi   = [&](Value value) {
		auto* inst = Producer(value, ValueOpcode::Phi);
		return inst != nullptr && inst->Parent() == t.header ? inst : nullptr;
	};

	// The ray: extent, origin and direction phis start as the world ray and its t_max.
	t.bvh    = &bvh;
	t.exec   = phi(bvh.Arg(2));
	t.extent = phi(address->Arg(2));
	if (t.exec == nullptr) return false;
	for (uint32_t arg = 2; arg < 9; ++arg) {
		const auto* ray = phi(address->Arg(arg));
		if (ray == nullptr) return false;
		t.ray[(arg + 4) % 7] = entry(*ray);
	}

	// The node address is node + BVH header >> 3; the header starts as the TLAS, read from the
	// flattened SRT.
	const auto  exec  = Value(t.exec);
	const auto  lanes = entry(*t.exec);
	const auto* sum   = Producer(ResolveActiveU32(address->Arg(0), exec), ValueOpcode::IAdd32);
	if (sum == nullptr) return false;
	auto* node      = phi(sum->Arg(0));
	auto  base_part = sum->Arg(1);
	if (node == nullptr) {
		node      = phi(sum->Arg(1));
		base_part = sum->Arg(0);
	}
	const auto* low = Producer(ResolveActiveU32(base_part, exec), ValueOpcode::CompositeExtractU64);
	const auto* shift =
	    low != nullptr ? Producer(low->Arg(0), ValueOpcode::ShiftRightLogical64) : nullptr;
	const auto* base =
	    shift != nullptr ? Producer(shift->Arg(0), ValueOpcode::CompositeConstructU64) : nullptr;
	if (node == nullptr || base == nullptr || Immediate(low->Arg(1)) != 0u ||
	    Immediate(shift->Arg(1)) != 3u) {
		return false;
	}
	for (uint32_t word = 0; word < 2; ++word) {
		const auto* header = phi(base->Arg(word));
		const auto* read   = header != nullptr ? Producer(ResolveActiveU32(entry(*header), lanes),
		                                                  ValueOpcode::ReadConst)
		                                       : nullptr;
		const auto  slot   = read != nullptr ? Immediate(read->Arg(1)) : std::nullopt;
		if (!slot) return false;
		t.tlas[word] = *slot;
	}

	// The latch votes on the lanes the intersection runs on; its code goes before the vote, which
	// must follow what the back edge carries.
	const auto alive = back(*t.exec);
	for (auto& inst: *t.header) {
		if (inst.GetOpcode() != ValueOpcode::Phi || inst.GetType() != Type::U1 ||
		    back(inst) != alive) {
			continue;
		}
		if (entry(inst) != lanes) return false;
		t.lanes.push_back(&inst);
	}
	const auto& term = t.latch->terminator;
	t.vote           = Producer(t.latch->condition, ValueOpcode::ConditionRef);
	if (term.kind != CFG::TerminatorKind::ConditionalBranch || term.true_block != t.header ||
	    t.vote == nullptr || t.vote->Arg(0) != alive ||
	    t.vote->Flags<CFG::BranchCondition>() != CFG::BranchCondition::ExecNonZero) {
		return false;
	}
	auto* voting = t.vote->Parent();
	if (voting != t.latch &&
	    (t.latch->ImmPredecessors().size() != 1 || t.latch->ImmPredecessors()[0] != voting)) {
		return false;
	}
	const auto* moved = back(*t.extent).Resolve().TryInstruction();
	for (auto it = Position(*t.vote); it != voting->end(); ++it) {
		if (&*it == moved) return false;
	}
	if (voting != t.latch && moved != nullptr && moved->Parent() == t.latch) return false;

	// The node takes the instance root link (+56) and prim record word 0; the record and the
	// instance are the addresses of those loads.
	std::unordered_set<const Inst*> nodes;
	Collect(back(*node), t.header, nodes);
	const Inst* root  = nullptr;
	const Inst* word0 = nullptr;
	for (const auto* inst: nodes) {
		for (auto [load, offset]: {std::pair {&root, InstanceRoot}, std::pair {&word0, 0u}}) {
			if (!GlobalLoad(program, *inst, offset)) continue;
			if (*load != nullptr) return false;
			*load = inst;
		}
	}
	if (root == nullptr || word0 == nullptr) return false;
	const auto loop = std::span(program.blocks.begin() + BlockIndex(program, t.header),
	                            program.blocks.begin() + BlockIndex(program, t.latch) + 1);

	// The query's iterations skip the box path, which pushes intersection results, and run the rest
	// on stale state: it must have no other side effects and leave the loop only at the latch. Prim
	// record word 1 and the instance flags and BLAS are each loaded once.
	const auto inside = [&](const Block* block) {
		return std::ranges::find(loop, block) != loop.end();
	};
	const std::array<std::pair<const Inst*, uint32_t>, 3> wanted {
	    {{word0, 4}, {root, InstanceFlags}, {root, InstanceBlas}}};
	std::array<const Inst*, 3> found {};
	for (const auto* block: loop) {
		if (block->terminator.kind == CFG::TerminatorKind::Return ||
		    (block != t.latch && !std::ranges::all_of(block->ImmSuccessors(), inside))) {
			return false;
		}
		for (const auto& inst: *block) {
			for (size_t load = 0; load < wanted.size(); ++load) {
				const auto& [like, offset] = wanted[load];
				if (!GlobalLoad(program, inst, offset) || inst.Arg(1) != like->Arg(1) ||
				    inst.Arg(2) != like->Arg(2)) {
					continue;
				}
				if (found[load] != nullptr) return false;
				found[load] = &inst;
			}
			const auto op = inst.GetOpcode();
			if (op == ValueOpcode::BvhIntersect && &inst != &bvh) return false;
			if (!HasSideEffects(op) || op == ValueOpcode::Reference) continue;
			uint32_t data = 3;
			if (op == ValueOpcode::WriteSharedU32) {
				data = 1;
			} else if (op != ValueOpcode::StoreAddressU32) {
				return false;
			}
			const auto* pushed = Producer(ResolveActiveU32(inst.Arg(data), exec),
			                              ValueOpcode::CompositeExtractU32x4);
			if (pushed == nullptr || pushed->Arg(0) != Value(&bvh)) return false;
		}
	}
	const auto [word1, flags, blas] = found;
	if (word1 == nullptr || flags == nullptr || blas == nullptr) return false;
	t.loads = {word0, word1, flags};

	// The phi the iteration's code reads for each of them.
	std::vector<std::pair<Inst*, std::unordered_set<const Inst*>>> carried;
	for (auto& inst: *t.header) {
		if (inst.GetOpcode() == ValueOpcode::Phi && inst.GetType() == Type::U32 &&
		    DirectlyUsed(inst)) {
			carried.push_back({&inst, {}});
			Collect(back(inst), t.header, carried.back().second);
		}
	}
	const std::array<const Inst*, 5> targets {word0, word1, root->Arg(1).Resolve().TryInstruction(),
	                                          root->Arg(2).Resolve().TryInstruction(), flags};
	t.state[0] = node;
	for (uint32_t role = 1; role < targets.size(); ++role) {
		for (auto& [carrier, values]: carried) {
			if (!values.contains(targets[role])) continue;
			if (t.state[role] != nullptr) return false;
			t.state[role] = carrier;
		}
		if (t.state[role] == nullptr ||
		    std::ranges::find(t.state.begin(), t.state.begin() + role, t.state[role]) !=
		        t.state.begin() + role) {
			return false;
		}
	}

	// Instance BLAS loads run under the root load's lanes and the mask test, flags << k < 0: mask
	// bit 23 - k.
	const auto  leaf = root->Arg(3);
	const auto* both = Producer(blas->Arg(3), ValueOpcode::LogicalAnd);
	if (both == nullptr || (both->Arg(0) != leaf && both->Arg(1) != leaf)) return false;
	const auto* sign =
	    Producer(both->Arg(0) == leaf ? both->Arg(1) : both->Arg(0), ValueOpcode::SGreaterThan32);
	const auto* mask = sign != nullptr ? Producer(ResolveActiveU32(sign->Arg(1), leaf),
	                                              ValueOpcode::ShiftLeftLogical32)
	                                   : nullptr;
	const auto  bit  = mask != nullptr ? Immediate(mask->Arg(1)) : std::nullopt;
	if (!bit || Immediate(sign->Arg(0)) != 0u || *bit < 16u || *bit > 23u ||
	    ResolveActiveU32(mask->Arg(0), leaf).TryInstruction() != flags) {
		return false;
	}
	t.cull_mask = 1u << (23u - *bit);
	return true;
}

void Rewrite(Traversal& t) {
	using Op = ValueOpcode;
	// The preheader starts the query on the loop's lanes, which then run while it has candidates.
	const Inserter entry {t.preheader, t.preheader->end()};
	const auto     lanes = t.exec->Arg(Incoming(*t.exec, t.preheader));
	const auto     valid = entry(Op::RayQueryValid, {});
	const auto     start = entry(Op::LogicalAnd, {valid, lanes});
	entry(Op::RayQueryInitialize, {t.ray[0], t.ray[1], t.ray[2], t.ray[3], t.ray[4], t.ray[5],
	                               t.ray[6], Value(t.cull_mask), start});
	const auto entering = entry(Op::SelectU1, {valid, entry(Op::RayQueryProceed, {start}), lanes});
	for (auto* lane: t.lanes) {
		lane->SetArg(Incoming(*lane, t.preheader), entering);
	}

	// The body reads the candidate's prim record and instance where the loop reads its own.
	auto*          body = t.header->terminator.true_block;
	const Inserter at {body, std::ranges::find_if(*body, [](const Inst& inst) {
		                   return inst.GetOpcode() != Op::Phi;
	                   })};
	const auto     current   = at(Op::LogicalAnd, {valid, Value(t.exec)});
	const auto     candidate = at(Op::RayQueryCandidate, {current});
	const auto     instance  = at(Op::RayQueryCandidateInstance, {current});
	const auto     part      = [&](Op op, Value value, uint32_t index) {
		return at(op, {value, Value(index)});
	};
	const auto load = [&](const Inst& like, Value low, Value high) {
		return at(Op::LoadAddressU32, {at(Op::GetAddressResource, {low, high}), low, high, current},
		          like.Flags<uint64_t>());
	};
	const auto& [word0, word1, flags] = t.loads;
	const auto instance_low           = part(Op::CompositeExtractU64, instance, 0);
	const auto instance_high          = part(Op::CompositeExtractU64, instance, 1);
	// The BLAS header is instance words 0 and 1, loaded like those of a prim record.
	const auto blas   = at(Op::CompositeConstructU64, {load(*word0, instance_low, instance_high),
	                                                   load(*word1, instance_low, instance_high)});
	const auto offset = at(
	    Op::ShiftLeftLogical64,
	    {at(Op::CompositeConstructU64, {part(Op::CompositeExtractU32x4, candidate, 3), Value(0u)}),
		 Value(3u)});
	const auto       record = at(Op::IAdd64, {blas, offset});
	const auto       low    = part(Op::CompositeExtractU64, record, 0);
	const auto       high   = part(Op::CompositeExtractU64, record, 1);
	const std::array native {load(*word0, low, high), load(*word1, low, high), instance_low,
	                         instance_high, load(*flags, instance_low, instance_high)};
	for (size_t index = 0; index < native.size(); ++index) {
		auto*      phi    = t.state[index];
		const auto chosen = at(Op::SelectU32, {valid, native[index], Value(phi)});
		Redirect(*phi, chosen, *chosen.Instruction());
	}

	// The intersection runs only without the query, whose candidate is a hit at t with u, v.
	const std::array result {part(Op::CompositeExtractU32x4, candidate, 0), Value(One),
	                         part(Op::CompositeExtractU32x4, candidate, 1),
	                         part(Op::CompositeExtractU32x4, candidate, 2)};
	const Inserter   intersect {t.bvh->Parent(), Position(*t.bvh)};
	t.bvh->SetArg(2,
	              intersect(Op::LogicalAnd, {Value(t.exec), intersect(Op::LogicalNot, {valid})}));
	const auto uses = t.bvh->Uses();
	for (const auto& use: uses) {
		auto&          extract = *use.user;
		const Inserter after {extract.Parent(), std::next(Position(extract))};
		const auto     chosen =
		    after(Op::SelectU32, {valid, result[extract.Arg(1).U32()], Value(&extract)});
		Redirect(extract, chosen, *chosen.Instruction());
	}

	// The latch commits a closer hit to the query and takes the next candidate.
	const Inserter vote {t.vote->Parent(), Position(*t.vote)};
	const auto     moved =
	    vote(Op::INotEqual32, {t.extent->Arg(Incoming(*t.extent, t.latch)), Value(t.extent)});
	vote(Op::RayQueryConfirm, {vote(Op::LogicalAnd, {current, moved})});
	const auto alive =
	    vote(Op::SelectU1, {valid, vote(Op::RayQueryProceed, {current}), t.vote->Arg(0)});
	t.vote->SetArg(0, alive);
	for (auto* lane: t.lanes) {
		lane->SetArg(Incoming(*lane, t.latch), alive);
	}
}

} // namespace

uint32_t TraceTraversalsWithRayQueries(Program& program) {
	if (program.stage != ShaderType::Compute || program.wave_size != 32) return 0;
	std::vector<Inst*> intersections;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::BvhIntersect) intersections.push_back(&inst);
		}
	}
	uint32_t traced = 0;
	for (auto* bvh: intersections) {
		Traversal  traversal;
		const bool traceable = Match(program, *bvh, traversal) &&
		                       (traced == 0 || traversal.tlas == program.info.ray_query_tlas);
		if (traceable) {
			Rewrite(traversal);
			program.info.ray_query_tlas = traversal.tlas;
			++traced;
		} else if (EnclosingLoop(program, bvh->Parent()) != nullptr) {
			Log::WriteToConsoleAndLog(
			    fmt::format("ShaderRecompiler BVH traversal left to emulation: hash=0x{:016x}\n",
				            program.shader_hash));
		}
	}
	return traced;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
