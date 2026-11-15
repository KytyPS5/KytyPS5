#include "graphics/shader/recompiler/ir/passes/RayQueryTraversal.h"

#include "common/logging/log.h"

#include <algorithm>
#include <array>
#include <bit>
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
// Dword 3 of a BVH T# selects barycentric triangle results with bit 24.
constexpr uint32_t Barycentrics = 1u << 24;

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

// The flattened SRT slot that `value` reads, if any.
std::optional<uint32_t> Slot(Value value) {
	const auto* read = Producer(value, ValueOpcode::ReadConst);
	return read != nullptr ? Immediate(read->Arg(1)) : std::nullopt;
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
	Block*                  preheader = nullptr;
	Block*                  header    = nullptr;
	Block*                  latch     = nullptr;
	std::span<Block* const> loop; // Header through latch.
	Inst*                   bvh    = nullptr;
	Inst*                   exec   = nullptr; // The intersection's lanes.
	Inst*                   extent = nullptr; // The closest committed t.
	Inst*                   vote   = nullptr; // The latch's vote on whether lanes go on.
	std::vector<Inst*>      lanes;            // Phis that carry the voted lanes.
	std::array<Value, 7>    ray {};           // World origin xyz, direction xyz, t_max.
	// Words 0 and 1 of the current prim record, the current instance address and flags; a nested
	// walk carries only the record words.
	std::array<Inst*, 5> state {};
	// A flat walk's loads of prim record words 0 and 1 and of instance flags.
	std::array<const Inst*, 3> loads {};
	// A nested walk's instance address and BLAS address words, defined before its loop, its prim
	// record load and the loop's scalar loads whose address they give.
	std::array<Inst*, 4>    instance {};
	Inst*                   record = nullptr;
	std::vector<Inst*>      scalar;
	std::array<uint32_t, 2> tlas {};
	uint32_t                cull_mask = 0;
};

Value Entry(const Traversal& t, const Inst& phi) {
	return phi.Arg(Incoming(phi, t.preheader));
}

Value Back(const Traversal& t, const Inst& phi) {
	return phi.Arg(Incoming(phi, t.latch));
}

Inst* HeaderPhi(const Traversal& t, Value value) {
	auto* inst = Producer(value, ValueOpcode::Phi);
	return inst != nullptr && inst->Parent() == t.header ? inst : nullptr;
}

bool Inside(const Traversal& t, const Block* block) {
	return std::ranges::find(t.loop, block) != t.loop.end();
}

// The innermost loop around an intersection with extent `extent`, entered from one preheader, whose
// latch votes on the lanes the intersection runs on. The query's iterations skip the box path,
// which pushes intersection results, and run the rest on stale state: the loop must have no other
// side effects and leave only at the latch.
bool MatchLoop(const Program& program, Inst& bvh, Value extent, Traversal& t) {
	if (!std::ranges::all_of(bvh.Uses(), [](const Use& use) {
		    return use.user->GetOpcode() == ValueOpcode::CompositeExtractU32x4 &&
			       Immediate(use.user->Arg(1)) < 4u;
	    })) {
		return false;
	}
	t.header = EnclosingLoop(program, bvh.Parent());
	if (t.header == nullptr || t.header->terminator.kind != CFG::TerminatorKind::Branch ||
	    t.header->terminator.true_block == nullptr) {
		return false;
	}
	t.latch            = t.header->terminator.continue_block;
	const auto entries = t.header->ImmPredecessors();
	if (entries.size() != 2 || std::ranges::find(entries, t.latch) == entries.end()) return false;
	t.preheader = entries[0] == t.latch ? entries[1] : entries[0];
	t.loop      = std::span(program.blocks.begin() + BlockIndex(program, t.header),
	                        program.blocks.begin() + BlockIndex(program, t.latch) + 1);
	t.bvh       = &bvh;
	t.exec      = HeaderPhi(t, bvh.Arg(2));
	t.extent    = HeaderPhi(t, extent);
	if (t.exec == nullptr || t.extent == nullptr) return false;

	// The latch's code goes before the vote, which must follow what the back edge carries.
	const auto lanes = Entry(t, *t.exec);
	const auto alive = Back(t, *t.exec);
	for (auto& inst: *t.header) {
		if (inst.GetOpcode() != ValueOpcode::Phi || inst.GetType() != Type::U1 ||
		    Back(t, inst) != alive) {
			continue;
		}
		if (Entry(t, inst) != lanes) return false;
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
	const auto* moved = Back(t, *t.extent).Resolve().TryInstruction();
	for (auto it = Position(*t.vote); it != voting->end(); ++it) {
		if (&*it == moved) return false;
	}
	if (voting != t.latch && moved != nullptr && moved->Parent() == t.latch) return false;

	const auto exec   = Value(t.exec);
	const auto inside = [&](const Block* block) { return Inside(t, block); };
	for (const auto* block: t.loop) {
		if (block->terminator.kind == CFG::TerminatorKind::Return ||
		    (block != t.latch && !std::ranges::all_of(block->ImmSuccessors(), inside))) {
			return false;
		}
		for (const auto& inst: *block) {
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
	return true;
}

// A walk over 64-bit node pointers: one loop takes the TLAS and, from instance records, the BLASes.
bool Match(const Program& program, Inst& bvh, Traversal& t) {
	const auto* descriptor = Producer(bvh.Arg(0), ValueOpcode::CompositeConstructU32x4);
	const auto* address    = Producer(bvh.Arg(1), ValueOpcode::MakeImageAddress);
	if (descriptor == nullptr || address == nullptr || bvh.Flags<uint32_t>() != 2u ||
	    !MatchLoop(program, bvh, address->Arg(2), t)) {
		return false;
	}
	for (uint32_t word = 0; word < 4; ++word) {
		if (Immediate(descriptor->Arg(word)) != TraversalDescriptor[word]) return false;
	}

	// The ray: extent, origin and direction phis start as the world ray and its t_max.
	for (uint32_t arg = 2; arg < 9; ++arg) {
		const auto* ray = HeaderPhi(t, address->Arg(arg));
		if (ray == nullptr) return false;
		t.ray[(arg + 4) % 7] = Entry(t, *ray);
	}

	// The node address is node + BVH header >> 3; the header starts as the TLAS, read from the
	// flattened SRT.
	const auto  exec  = Value(t.exec);
	const auto  lanes = Entry(t, *t.exec);
	const auto* sum   = Producer(ResolveActiveU32(address->Arg(0), exec), ValueOpcode::IAdd32);
	if (sum == nullptr) return false;
	auto* node      = HeaderPhi(t, sum->Arg(0));
	auto  base_part = sum->Arg(1);
	if (node == nullptr) {
		node      = HeaderPhi(t, sum->Arg(1));
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
		const auto* header = HeaderPhi(t, base->Arg(word));
		const auto  slot =
		    header != nullptr ? Slot(ResolveActiveU32(Entry(t, *header), lanes)) : std::nullopt;
		if (!slot) return false;
		t.tlas[word] = *slot;
	}

	// The node takes the instance root link (+56) and prim record word 0; the record and the
	// instance are the addresses of those loads.
	std::unordered_set<const Inst*> nodes;
	Collect(Back(t, *node), t.header, nodes);
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

	// Prim record word 1 and the instance flags and BLAS are each loaded once in the loop.
	const std::array<std::pair<const Inst*, uint32_t>, 3> wanted {
	    {{word0, 4}, {root, InstanceFlags}, {root, InstanceBlas}}};
	std::array<const Inst*, 3> found {};
	for (const auto* block: t.loop) {
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
			Collect(Back(t, inst), t.header, carried.back().second);
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

	// The instance BLAS load, or a select that moves it into the next header, runs under the root
	// load's lanes and the mask test flags << k < 0: mask bit 23 - k.
	const auto leaf = root->Arg(3);
	const auto test = [&](Value condition) -> std::optional<uint32_t> {
		const auto* both = Producer(condition, ValueOpcode::LogicalAnd);
		if (both == nullptr || (both->Arg(0) != leaf && both->Arg(1) != leaf)) return std::nullopt;
		const auto* sign = Producer(both->Arg(0) == leaf ? both->Arg(1) : both->Arg(0),
		                            ValueOpcode::SGreaterThan32);
		const auto* mask = sign != nullptr ? Producer(ResolveActiveU32(sign->Arg(1), leaf),
		                                              ValueOpcode::ShiftLeftLogical32)
		                                   : nullptr;
		const auto  bit  = mask != nullptr ? Immediate(mask->Arg(1)) : std::nullopt;
		const bool  kept = bit && Immediate(sign->Arg(0)) == 0u && *bit >= 16u && *bit <= 23u &&
		                   ResolveActiveU32(mask->Arg(0), leaf).TryInstruction() == flags;
		return kept ? bit : std::nullopt;
	};
	auto                            bit = test(blas->Arg(3));
	std::unordered_set<const Inst*> next;
	if (!bit) Collect(Back(t, *HeaderPhi(t, base->Arg(0))), t.header, next);
	for (std::vector<const Inst*> taking {blas}; !bit && !taking.empty();) {
		const auto* value = taking.back();
		taking.pop_back();
		for (const auto& use: value->Uses()) {
			if (!bit && use.user->GetOpcode() == ValueOpcode::SelectU32 && use.operand == 1 &&
			    next.contains(use.user)) {
				bit = test(use.user->Arg(0));
				taking.push_back(use.user);
			}
		}
	}
	if (!bit) return false;
	t.cull_mask = 1u << (23u - *bit);
	return true;
}

// The address whose bits 8..47 a BVH T#'s base holds, or nullptr; dword 3 must select barycentric
// triangle results.
Inst* DescriptorAddress(const Inst& descriptor) {
	const auto* low   = Producer(descriptor.Arg(0), ValueOpcode::CompositeExtractU64);
	const auto* field = low != nullptr ? Producer(low->Arg(0), ValueOpcode::BitwiseAnd64) : nullptr;
	const auto* shift =
	    field != nullptr ? Producer(field->Arg(0), ValueOpcode::ShiftRightLogical64) : nullptr;
	const auto* flags = Producer(descriptor.Arg(3), ValueOpcode::BitwiseOr32);
	const auto  bits  = flags != nullptr ? Immediate(flags->Arg(1)) : std::nullopt;
	if (shift == nullptr || Immediate(low->Arg(1)) != 0u || Immediate(shift->Arg(1)) != 8u ||
	    !bits || (*bits & Barycentrics) == 0u) {
		return nullptr;
	}
	return Producer(shift->Arg(0), ValueOpcode::CompositeConstructU64);
}

// The address resource of `load` if it reads the guest dword at `offset` from it, or nullptr.
const Inst* AddressWord(const Program& program, const Inst* load, uint32_t offset) {
	if (load == nullptr || load->GetOpcode() != ValueOpcode::LoadAddressU32) return nullptr;
	const auto& memory = program.memory_info[load->Flags<MemoryFlags>().index];
	const bool  based  = memory.kind == ResourceKind::ScalarAddress ||
	                     (memory.kind == ResourceKind::Global && !memory.address_is_full);
	if (!based || memory.offset != offset || Immediate(load->Arg(1)) != 0u) return nullptr;
	return Producer(load->Arg(0), ValueOpcode::GetAddressResource);
}

// The instance mask bits a branch on (flags & mask) != 0 tests, for the flags of `instance`.
std::optional<uint32_t> MaskTest(const Program& program, const Inst& branch, const Inst& instance) {
	const auto* test = Producer(branch.Arg(0), ValueOpcode::INotEqual32);
	const auto* bits = test != nullptr && Immediate(test->Arg(1)) == 0u
	                       ? Producer(test->Arg(0), ValueOpcode::BitwiseAnd32)
	                       : nullptr;
	const auto  mask = bits != nullptr ? Immediate(bits->Arg(1)) : std::nullopt;
	if (!mask || *mask == 0u || (*mask & ~0xff00u) != 0u ||
	    AddressWord(program, bits->Arg(0).Resolve().TryInstruction(), InstanceFlags) != &instance) {
		return std::nullopt;
	}
	return *mask >> 8;
}

// Whether a header phi's value is only kept, by a select on the loop's lanes, for lanes it skips.
bool Kept(const Inst& phi, const Inst& exec) {
	return std::ranges::all_of(phi.Uses(), [&](const Use& use) {
		return use.user->GetOpcode() == ValueOpcode::SelectU32 && use.operand == 2 &&
		       use.user->Arg(0).Resolve().TryInstruction() == &exec;
	});
}

// Whether `user` in the loop body takes the candidate's instance address and BLAS words in place
// of the walk's: all but the intersection's T# and address and the prim record V# do.
bool Replaced(const Traversal& t, const Inst& user) {
	const auto other = [&](Value value) { return value.Resolve().TryInstruction() != &user; };
	return user.Parent() != t.header && Inside(t, user.Parent()) && other(t.bvh->Arg(0)) &&
	       other(t.bvh->Arg(1)) && other(t.record->Arg(0));
}

// A BLAS walk over 32-bit node pointers inside a TLAS walk. The TLAS walk intersects its nodes with
// the world ray under a T# over the TLAS header the flattened SRT holds; its instance leaf node
// loads instance record node >> 3 of the header's instance array and enters a loop under a T# over
// the BLAS at the record's +0. That loop reads prim records through a V# over the BLAS with stride
// 8: word 0 is the next node and word 1 a value the loop keeps.
bool MatchNested(const Program& program, Inst& bvh, std::span<Inst* const> intersections,
                 Traversal& t) {
	const auto* descriptor = Producer(bvh.Arg(0), ValueOpcode::CompositeConstructU32x4);
	const auto* address    = Producer(bvh.Arg(1), ValueOpcode::MakeImageAddress);
	if (descriptor == nullptr || address == nullptr ||
	    !MatchLoop(program, bvh, address->Arg(1), t)) {
		return false;
	}
	auto*       node = HeaderPhi(t, address->Arg(0));
	const auto* blas = DescriptorAddress(*descriptor);
	auto* word0 = blas != nullptr ? Producer(blas->Arg(0), ValueOpcode::LoadAddressU32) : nullptr;
	auto* word1 = blas != nullptr ? Producer(blas->Arg(1), ValueOpcode::LoadAddressU32) : nullptr;
	const auto* instance = AddressWord(program, word0, InstanceBlas);
	if (node == nullptr || instance == nullptr ||
	    AddressWord(program, word1, InstanceBlas + 4) != instance || Inside(t, word0->Parent()) ||
	    Inside(t, word1->Parent())) {
		return false;
	}
	auto* low  = Producer(instance->Arg(0), ValueOpcode::IAdd32);
	auto* high = instance->Arg(1).Resolve().TryInstruction();
	if (low == nullptr || high == nullptr) return false;
	t.instance = {low, high, word0, word1};

	// The TLAS walk's intersection, in a loop around this one.
	const Inst* walk = nullptr;
	for (const auto* other: intersections) {
		const auto* around = EnclosingLoop(program, other->Parent());
		if (other->Flags<uint32_t>() != 1u || around == nullptr ||
		    BlockIndex(program, around) >= BlockIndex(program, t.header) ||
		    BlockIndex(program, around->terminator.continue_block) < BlockIndex(program, t.latch)) {
			continue;
		}
		const auto* words     = Producer(other->Arg(0), ValueOpcode::CompositeConstructU32x4);
		const auto* tlas      = words != nullptr ? DescriptorAddress(*words) : nullptr;
		const auto  low_slot  = tlas != nullptr ? Slot(tlas->Arg(0)) : std::nullopt;
		const auto  high_slot = tlas != nullptr ? Slot(tlas->Arg(1)) : std::nullopt;
		if (!low_slot || !high_slot) continue;
		if (walk != nullptr) return false;
		walk   = other;
		t.tlas = {*low_slot, *high_slot};
	}
	if (walk == nullptr) return false;

	// Its world ray is defined before the outermost loop around it; t_max is this loop's extent.
	const auto* ray   = Producer(walk->Arg(1), ValueOpcode::MakeImageAddress);
	auto*       outer = EnclosingLoop(program, walk->Parent());
	while (auto* up = EnclosingLoop(program, outer)) {
		outer = up;
	}
	if (ray == nullptr) return false;
	for (uint32_t arg = 2; arg < 8; ++arg) {
		const auto* value = ray->Arg(arg).Resolve().TryInstruction();
		if (value != nullptr &&
		    BlockIndex(program, value->Parent()) >= BlockIndex(program, outer)) {
			return false;
		}
		t.ray[arg - 2] = ray->Arg(arg);
	}
	t.ray[6] = Entry(t, *t.extent);

	// The instance address is the header's instance array plus (node >> 3) << 7.
	const Inst* array = nullptr;
	for (uint32_t arg = 0; arg < 2; ++arg) {
		const auto* part = Producer(low->Arg(arg), ValueOpcode::CompositeExtractU64);
		const auto* scale =
		    part != nullptr ? Producer(part->Arg(0), ValueOpcode::ShiftLeftLogical64) : nullptr;
		const auto* index = scale != nullptr
		                        ? Producer(scale->Arg(0), ValueOpcode::CompositeConstructU64)
		                        : nullptr;
		const auto* node_index =
		    index != nullptr ? Producer(index->Arg(0), ValueOpcode::ShiftRightLogical32) : nullptr;
		if (node_index != nullptr && Immediate(part->Arg(1)) == 0u &&
		    Immediate(scale->Arg(1)) == 7u && Immediate(index->Arg(1)) == 0u &&
		    Immediate(node_index->Arg(1)) == 3u) {
			array = Producer(low->Arg(1 - arg), ValueOpcode::IAdd32);
		}
	}
	if (array == nullptr ||
	    (Slot(array->Arg(0)) != t.tlas[0] && Slot(array->Arg(1)) != t.tlas[0])) {
		return false;
	}

	// The prim record load whose word 0 is the next node, and the phi that keeps word 1.
	std::unordered_set<const Inst*> next;
	Collect(Back(t, *node), t.header, next);
	Inst* record = nullptr;
	for (const auto* inst: next) {
		auto* load =
		    inst->GetOpcode() == ValueOpcode::CompositeExtractU32x2 && Immediate(inst->Arg(1)) == 0u
		        ? Producer(inst->Arg(0), ValueOpcode::LoadBufferU32x2)
		        : nullptr;
		if (load == nullptr) continue;
		if (record != nullptr && record != load) return false;
		record = load;
	}
	t.record = record;
	const auto* resource =
	    record != nullptr ? Producer(record->Arg(0), ValueOpcode::GetBufferResource) : nullptr;
	const auto* stride =
	    resource != nullptr ? Producer(resource->Arg(1), ValueOpcode::BitwiseOr32) : nullptr;
	const auto bits = stride != nullptr ? Immediate(stride->Arg(1)) : std::nullopt;
	if (!bits || resource->Arg(0) != Value(word0) || stride->Arg(0) != Value(word1) ||
	    ((*bits >> 16) & 0x3fffu) != 8u || Immediate(record->Arg(2)) != 0u ||
	    Immediate(record->Arg(3)) != 0u || !Inside(t, record->Parent())) {
		return false;
	}
	const auto& memory = program.memory_info[record->Flags<MemoryFlags>().index];
	if (!memory.idxen || memory.offen || memory.offset != 0u) return false;
	const auto record_uses = record->Uses();
	const auto second      = std::ranges::find_if(record_uses, [](const Use& use) {
		return use.user->GetOpcode() == ValueOpcode::CompositeExtractU32x2 &&
		       Immediate(use.user->Arg(1)) == 1u;
	});
	if (second == record_uses.end()) return false;
	for (auto& inst: *t.header) {
		std::unordered_set<const Inst*> values;
		if (inst.GetOpcode() == ValueOpcode::Phi && inst.GetType() == Type::U32 && &inst != node &&
		    DirectlyUsed(inst)) {
			Collect(Back(t, inst), t.header, values);
		}
		if (!values.contains(second->user)) continue;
		if (t.state[1] != nullptr) return false;
		t.state[1] = &inst;
	}
	if (t.state[1] == nullptr) return false;
	t.state[0] = node;

	// Of what the loop reads of the instance, the candidate's address and BLAS words replace its
	// own; the rest must be the intersection's T# and ray, the record V#, or the start of a header
	// phi the query overrides or that only lanes the loop skips keep. A branch between the
	// instance and the loop may test the instance mask, which the query's cull mask applies.
	const auto                            first = BlockIndex(program, instance->Parent());
	const auto                            begin = BlockIndex(program, t.header);
	const auto                            last  = BlockIndex(program, t.latch);
	std::optional<uint32_t>               mask;
	std::unordered_set<const Inst*>       seen;
	std::vector<std::pair<Inst*, size_t>> reads;
	for (std::vector<const Inst*> work {low, high}; !work.empty();) {
		const auto* value = work.back();
		work.pop_back();
		if (!seen.insert(value).second) continue;
		const bool swapped = std::ranges::find(t.instance, value) != t.instance.end();
		for (const auto& use: value->Uses()) {
			auto*      user = use.user;
			const auto at   = BlockIndex(program, user->Parent());
			if (at < first || at > last) continue;
			if (at < begin && user->GetOpcode() != ValueOpcode::ConditionRef) {
				work.push_back(user);
			} else if (at < begin) {
				if (mask) return false;
				mask = MaskTest(program, *user, *instance);
				if (!mask) return false;
			} else if (user->Parent() == t.header) {
				if (user != node && !Kept(*user, *t.exec)) return false;
			} else if (Replaced(t, *user)) {
				if (!swapped) return false;
				reads.emplace_back(user, use.operand);
			}
		}
	}
	t.cull_mask = mask.value_or(0xffu);

	// Per lane, the candidate's instance and BLAS reach the loop's code that read the walk's. Its
	// scalar loads become per-lane loads; whole-wave operations take per-lane values, but V#s, T#s,
	// S#s, constant buffer reads and lane numbers must stay uniform.
	while (!reads.empty()) {
		const auto [inst, operand] = reads.back();
		reads.pop_back();
		const auto op = inst->GetOpcode();
		if (!Inside(t, inst->Parent())) continue;
		if (op == ValueOpcode::ReadConstBuffer || op == ValueOpcode::GetBufferResource ||
		    op == ValueOpcode::GetImageResource || op == ValueOpcode::GetSamplerResource ||
		    ((op == ValueOpcode::ReadLane || op == ValueOpcode::WriteLane) && operand != 0)) {
			return false;
		}
		if (op == ValueOpcode::Ballot || op == ValueOpcode::ReadFirstLane ||
		    op == ValueOpcode::ReadLane || op == ValueOpcode::WriteLane ||
		    op == ValueOpcode::SubgroupUMinU32 || op == ValueOpcode::SubgroupUMaxU32 ||
		    op == ValueOpcode::SubgroupOrU32 || !seen.insert(inst).second) {
			continue;
		}
		if (op == ValueOpcode::LoadAddressU32 &&
		    program.memory_info[inst->Flags<MemoryFlags>().index].kind ==
		        ResourceKind::ScalarAddress) {
			t.scalar.push_back(inst);
		}
		for (const auto& use: inst->Uses()) {
			reads.emplace_back(use.user, use.operand);
		}
	}
	return true;
}

void Rewrite(Program& program, Traversal& t) {
	using Op = ValueOpcode;
	// The preheader starts the query on the loop's lanes, which then run while it has candidates.
	const Inserter entry {t.preheader, t.preheader->end()};
	const auto     lanes   = Entry(t, *t.exec);
	const auto     valid   = entry(Op::RayQueryValid, {});
	const auto     invalid = entry(Op::LogicalNot, {valid});
	const auto     start   = entry(Op::LogicalAnd, {valid, lanes});
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
	const auto instance_low  = part(Op::CompositeExtractU64, instance, 0);
	const auto instance_high = part(Op::CompositeExtractU64, instance, 1);
	const auto offset        = at(
	    Op::ShiftLeftLogical64,
	    {at(Op::CompositeConstructU64, {part(Op::CompositeExtractU32x4, candidate, 3), Value(0u)}),
		 Value(3u)});
	const auto choose = [&](Inst& phi, Value native) {
		const auto chosen = at(Op::SelectU32, {valid, native, Value(&phi)});
		Redirect(phi, chosen, *chosen.Instruction());
	};
	// Per-lane loads of guest dwords 0 and 1 from an address: a flat walk's like its prim record
	// loads, a nested walk's under new full-address memory infos.
	const auto info = [&](uint32_t word) {
		if (t.instance[0] == nullptr) return t.loads[word]->Flags<uint64_t>();
		const MemoryFlags flags {.index = static_cast<uint32_t>(program.memory_info.size()),
		                         .pc    = t.instance[2]->Flags<MemoryFlags>().pc};
		program.memory_info.push_back(
		    {.kind = ResourceKind::Global, .offset = word * 4u, .address_is_full = true});
		return std::bit_cast<uint64_t>(flags);
	};
	const std::array words {info(0), info(1)};
	const auto       load = [&](Value low, Value high, uint64_t flags) {
		return at(Op::LoadAddressU32, {at(Op::GetAddressResource, {low, high}), low, high, current},
		          flags);
	};
	// The BLAS header is instance words 0 and 1.
	const std::array blas {load(instance_low, instance_high, words[0]),
	                       load(instance_low, instance_high, words[1])};
	const auto record = at(Op::IAdd64, {at(Op::CompositeConstructU64, {blas[0], blas[1]}), offset});
	const auto low    = part(Op::CompositeExtractU64, record, 0);
	const auto high   = part(Op::CompositeExtractU64, record, 1);
	choose(*t.state[0], load(low, high, words[0]));
	choose(*t.state[1], load(low, high, words[1]));
	if (t.instance[0] == nullptr) {
		choose(*t.state[2], instance_low);
		choose(*t.state[3], instance_high);
		choose(*t.state[4], load(instance_low, instance_high, t.loads[2]->Flags<uint64_t>()));
	} else {
		// The loop's own instance address and BLAS words become the candidate's, which its scalar
		// loads read per lane. The record load only runs without the query.
		const std::array native {instance_low, instance_high, blas[0], blas[1]};
		for (size_t index = 0; index < native.size(); ++index) {
			auto&      value  = *t.instance[index];
			const auto chosen = at(Op::SelectU32, {valid, native[index], Value(&value)});
			const auto uses   = value.Uses();
			for (const auto& use: uses) {
				if (use.user != chosen.Instruction() && Replaced(t, *use.user)) {
					use.user->SetArg(use.operand, chosen);
				}
			}
		}
		for (auto* scalar: t.scalar) {
			// Each loads its one dword of the group.
			auto& memory           = program.memory_info[scalar->Flags<MemoryFlags>().index];
			memory.kind            = ResourceKind::Global;
			memory.component_index = 0;
			memory.component_count = 1;
			const Inserter before {scalar->Parent(), Position(*scalar)};
			scalar->SetArg(3, before(Op::SelectU1, {valid, current, scalar->Arg(3)}));
		}
		const Inserter before {t.record->Parent(), Position(*t.record)};
		t.record->SetArg(4, before(Op::LogicalAnd, {t.record->Arg(4), invalid}));
	}

	// The intersection runs only without the query, whose candidate is a hit at t with u, v.
	const std::array result {part(Op::CompositeExtractU32x4, candidate, 0), Value(One),
	                         part(Op::CompositeExtractU32x4, candidate, 1),
	                         part(Op::CompositeExtractU32x4, candidate, 2)};
	const Inserter   intersect {t.bvh->Parent(), Position(*t.bvh)};
	t.bvh->SetArg(2, intersect(Op::LogicalAnd, {Value(t.exec), invalid}));
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
	const auto     moved = vote(Op::INotEqual32, {Back(t, *t.extent), Value(t.extent)});
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
	if (program.stage != ShaderType::Compute) return 0;
	std::vector<Inst*> intersections;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::BvhIntersect) intersections.push_back(&inst);
		}
	}
	uint32_t traced = 0;
	for (auto* bvh: intersections) {
		Traversal  traversal;
		const bool matched = bvh->Flags<uint32_t>() == 1u
		                         ? MatchNested(program, *bvh, intersections, traversal)
		                         : Match(program, *bvh, traversal);
		if (matched && (traced == 0 || traversal.tlas == program.info.ray_query_tlas)) {
			Rewrite(program, traversal);
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
