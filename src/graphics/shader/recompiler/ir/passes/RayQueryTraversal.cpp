#include "graphics/shader/recompiler/ir/passes/RayQueryTraversal.h"

#include "common/logging/log.h"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

// The descriptor of traversals over 64-bit node pointers: base 0, maximum size, sorted boxes and
// triangle barycentrics.
constexpr std::array<uint32_t, 4> TraversalDescriptor {0u, 0x80000000u, 0xffffffffu, 0x810003ffu};
constexpr uint32_t                One = 0x3f800000u;
// Bit (u | v << 2) is set for the remaps of a triangle node's barycentric vertices u and v, each
// 3 for 0, that order the triangle's vertices oddly.
constexpr uint32_t OddRemaps = 0x2942u;
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

	// A copy of `inst` over `args`.
	Value operator()(const Inst& inst, std::span<const Value> args) const {
		auto& copy = *block->Instructions().emplace(at, inst.GetOpcode(), inst.Flags<uint64_t>());
		copy.SetParent(block);
		for (size_t arg = 0; arg < args.size(); ++arg) {
			copy.SetArg(arg, args[arg]);
		}
		return Value(&copy);
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

// A loop entered from one preheader: its header through its latch.
struct Loop {
	Block*                  preheader = nullptr;
	Block*                  header    = nullptr;
	Block*                  latch     = nullptr;
	std::span<Block* const> loop;
};

struct Traversal: Loop {
	Inst*                bvh    = nullptr;
	Inst*                exec   = nullptr; // The intersection's lanes.
	Inst*                extent = nullptr; // The closest committed t.
	Inst*                vote   = nullptr; // The latch's vote on whether lanes go on.
	std::vector<Inst*>   lanes;            // Phis that carry the voted lanes.
	std::array<Value, 7> ray {};           // World origin xyz, direction xyz, t_max.
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

Value Entry(const Loop& t, const Inst& phi) {
	return phi.Arg(Incoming(phi, t.preheader));
}

Value Back(const Loop& t, const Inst& phi) {
	return phi.Arg(Incoming(phi, t.latch));
}

Inst* HeaderPhi(const Loop& t, Value value) {
	auto* inst = Producer(value, ValueOpcode::Phi);
	return inst != nullptr && inst->Parent() == t.header ? inst : nullptr;
}

bool Inside(const Loop& t, const Block* block) {
	return std::ranges::find(t.loop, block) != t.loop.end();
}

// Whether `header` heads a loop entered from one preheader, which `l` then holds.
bool Enter(const Program& program, Block* header, Loop& l) {
	if (header == nullptr || header->ImmPredecessors().size() != 2) return false;
	const auto entries = header->ImmPredecessors();
	l.header           = header;
	l.latch            = header->terminator.continue_block;
	l.preheader        = entries[0] == l.latch ? entries[1] : entries[0];
	l.loop             = std::span(program.blocks.begin() + BlockIndex(program, header),
	                               program.blocks.begin() + BlockIndex(program, l.latch) + 1);
	return std::ranges::find(entries, l.latch) != entries.end();
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
	if (!Enter(program, EnclosingLoop(program, bvh.Parent()), t) ||
	    t.header->terminator.kind != CFG::TerminatorKind::Branch ||
	    t.header->terminator.true_block == nullptr) {
		return false;
	}
	t.bvh    = &bvh;
	t.exec   = HeaderPhi(t, bvh.Arg(2));
	t.extent = HeaderPhi(t, extent);
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

// Whether the low instance address word, a sum, is the TLAS header's instance array, read from the
// flattened SRT `slot`, plus (node >> 3) << 7.
bool InstanceArray(const Inst& low, uint32_t slot) {
	for (uint32_t arg = 0; arg < 2; ++arg) {
		const auto* part = Producer(low.Arg(arg), ValueOpcode::CompositeExtractU64);
		const auto* scale =
		    part != nullptr ? Producer(part->Arg(0), ValueOpcode::ShiftLeftLogical64) : nullptr;
		const auto* index = scale != nullptr
		                        ? Producer(scale->Arg(0), ValueOpcode::CompositeConstructU64)
		                        : nullptr;
		const auto* node =
		    index != nullptr ? Producer(index->Arg(0), ValueOpcode::ShiftRightLogical32) : nullptr;
		const auto* array = Producer(low.Arg(1 - arg), ValueOpcode::IAdd32);
		if (node != nullptr && array != nullptr && Immediate(part->Arg(1)) == 0u &&
		    Immediate(scale->Arg(1)) == 7u && Immediate(index->Arg(1)) == 0u &&
		    Immediate(node->Arg(1)) == 3u &&
		    (Slot(array->Arg(0)) == slot || Slot(array->Arg(1)) == slot)) {
			return true;
		}
	}
	return false;
}

// The BLAS address words 0 and 1 of a V# over the BLAS's prim records (stride 8), or nulls.
std::array<const Inst*, 2> RecordBase(Value resource) {
	const auto* buffer = Producer(resource, ValueOpcode::GetBufferResource);
	const auto* stride =
	    buffer != nullptr ? Producer(buffer->Arg(1), ValueOpcode::BitwiseOr32) : nullptr;
	const auto bits = stride != nullptr ? Immediate(stride->Arg(1)) : std::nullopt;
	if (!bits || ((*bits >> 16) & 0x3fffu) != 8u) return {};
	return {buffer->Arg(0).Resolve().TryInstruction(), stride->Arg(0).Resolve().TryInstruction()};
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

	if (!InstanceArray(*low, t.tlas[0])) return false;

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
	if (record == nullptr ||
	    RecordBase(record->Arg(0)) != std::array<const Inst*, 2> {word0, word1} ||
	    Immediate(record->Arg(2)) != 0u || Immediate(record->Arg(3)) != 0u ||
	    !Inside(t, record->Parent())) {
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

// A walk that takes TLAS and BLAS nodes off one stack its wave shares: a loop around a BLAS loop
// around a loop over a leaf's prim records, whose triangle tests commit the first hit a lane
// accepts, which then leaves.
struct Walk: Loop {
	Inst*                skip  = nullptr; // The vote that enters the walk's body.
	Inst*                leave = nullptr; // The vote that leaves the walk.
	Value                lanes;
	std::array<Value, 7> ray {}; // World origin xyz, direction xyz, t_max.
	Inst*                bvh   = nullptr;
	Inst*                flags = nullptr;         // The instance flags load.
	Inst*                word1 = nullptr;         // The prim record loop's phi of record word 1.
	std::array<Inst*, 2> instance {};             // The instance address.
	std::array<Inst*, 3> lanes_in {};             // What the walk's body, its record loop and that
	                                              // loop's entry run on.
	Inst*                accepted = nullptr;      // Whether a lane tests the instance's records.
	Inst*                commit   = nullptr;      // Whether a lane commits the hit it tests.
	std::vector<std::pair<Inst*, Value>> commits; // The walk's exit phi and its committed value.
	std::array<uint32_t, 2>              tlas {};
};

// The values the per-candidate code of a walk takes from the candidate.
enum Leaf { Lanes, Numerator, Denominator, Flags, Word1, InstanceLow, InstanceHigh, None };

Leaf LeafOf(const Walk& w, const Inst& inst) {
	if (std::ranges::find(w.lanes_in, &inst) != w.lanes_in.end()) return Lanes;
	if (inst.GetOpcode() == ValueOpcode::CompositeExtractU32x4 && inst.Arg(0) == Value(w.bvh)) {
		const auto index = Immediate(inst.Arg(1));
		if (index == 0u) return Numerator;
		return index == 1u ? Denominator : None;
	}
	if (&inst == w.flags) return Flags;
	if (&inst == w.word1) return Word1;
	if (&inst == w.instance[0]) return InstanceLow;
	return &inst == w.instance[1] ? InstanceHigh : None;
}

// A select that keeps a value for lanes the walk does not run: on a candidate, its first arm.
bool LaneSelect(const Walk& w, const Inst& inst) {
	const auto* lanes = inst.Arg(0).Resolve().TryInstruction();
	return (inst.GetOpcode() == ValueOpcode::SelectU32 ||
	        inst.GetOpcode() == ValueOpcode::SelectU1) &&
	       lanes != nullptr && LeafOf(w, *lanes) == Lanes;
}

// Whether an operation computes a lane's value from that lane's arguments alone, not from wave
// state, other lanes or memory.
bool PerLane(ValueOpcode op) {
	using Op = ValueOpcode;
	constexpr std::array excluded {Op::ConditionRef,  Op::ReadFirstLane,       Op::ReadLane,
	                               Op::WriteLane,     Op::SubgroupUMinU32,     Op::SubgroupUMaxU32,
	                               Op::SubgroupOrU32, Op::Permlane16U32,       Op::PermuteU32,
	                               Op::BpermuteU32,   Op::SwizzleU32,          Op::DppMoveU32,
	                               Op::DppUpdateU32,  Op::LoadDeviceAddressU32};
	const auto           plain = [](Type type) {
		return type == Type::U1 || type == Type::U32 || type == Type::F32;
	};
	if (!plain(TypeOf(op)) || NumArgsOf(op) == 0 || HasSideEffects(op) ||
	    SharedAccessOf(op) != SharedAccess::None ||
	    std::ranges::find(excluded, op) != excluded.end()) {
		return false;
	}
	for (size_t arg = 0; arg < NumArgsOf(op); ++arg) {
		if (!plain(ArgTypeOf(op, arg))) return false;
	}
	return true;
}

// What the walk's per-candidate code computes for `value`, copied to `at` from the candidate's
// `natives`, or without `at` `value` itself; empty unless it depends, past the leaves, only on
// values from before the walk.
Value Copy(const Walk& w, Value value, std::unordered_map<const Inst*, Value>& copies,
           const Inserter* at = nullptr, std::span<const Value> natives = {}) {
	value            = value.Resolve();
	auto* const inst = value.TryInstruction();
	if (inst == nullptr || !Inside(w, inst->Parent())) return value;
	if (const auto leaf = LeafOf(w, *inst); leaf != None) {
		return at != nullptr ? natives[leaf] : value;
	}
	if (const auto found = copies.find(inst); found != copies.end()) return found->second;
	Value copy;
	if (LaneSelect(w, *inst)) {
		copy = Copy(w, inst->Arg(1), copies, at, natives);
	} else if (PerLane(inst->GetOpcode())) {
		std::vector<Value> args;
		for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
			args.push_back(Copy(w, inst->Arg(arg), copies, at, natives));
		}
		if (std::ranges::none_of(args, &Value::IsEmpty)) {
			copy = at != nullptr ? (*at)(*inst, args) : value;
		}
	}
	copies.emplace(inst, copy);
	return copy;
}

// The mask M of a conjunct ((flags >> 8) & M) != 0 of `test`, M from before the walk, or 0xff.
Value CullMask(const Walk& w, Value test) {
	const auto* inst = test.Resolve().TryInstruction();
	for (uint32_t arg = 0; inst != nullptr && arg < 2; ++arg) {
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			if (const auto mask = CullMask(w, inst->Arg(arg)); mask != Value(0xffu)) return mask;
			continue;
		}
		const auto* bits =
		    inst->GetOpcode() == ValueOpcode::INotEqual32 && Immediate(inst->Arg(1 - arg)) == 0u
		        ? Producer(inst->Arg(arg), ValueOpcode::BitwiseAnd32)
		        : nullptr;
		for (uint32_t side = 0; bits != nullptr && side < 2; ++side) {
			const auto* shift = Producer(bits->Arg(side), ValueOpcode::ShiftRightLogical32);
			const auto* mask  = bits->Arg(1 - side).Resolve().TryInstruction();
			if (shift != nullptr && shift->Arg(0) == Value(w.flags) &&
			    Immediate(shift->Arg(1)) == 8u && (mask == nullptr || !Inside(w, mask->Parent()))) {
				return bits->Arg(1 - side);
			}
		}
	}
	return Value(0xffu);
}

bool MatchWalk(const Program& program, Inst& bvh, std::span<Inst* const> intersections, Walk& w) {
	Loop records, blases;
	if (!Enter(program, EnclosingLoop(program, bvh.Parent()), records) ||
	    !Enter(program, EnclosingLoop(program, records.header), blases) ||
	    !Enter(program, EnclosingLoop(program, blases.header), w) ||
	    w.header->terminator.kind != CFG::TerminatorKind::Branch ||
	    w.header->terminator.true_block == nullptr || w.latch->ImmPredecessors().size() != 1) {
		return false;
	}
	// The body runs from a vote after the header; a vote before the latch leaves.
	auto* enter = w.header->terminator.true_block;
	auto* vote  = w.latch->ImmPredecessors()[0];
	w.skip      = Producer(enter->condition, ValueOpcode::ConditionRef);
	w.leave     = Producer(vote->condition, ValueOpcode::ConditionRef);
	if (w.skip == nullptr || w.leave == nullptr ||
	    enter->terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
	    enter->terminator.false_block != vote ||
	    vote->terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
	    vote->terminator.false_block != w.latch || Inside(w, vote->terminator.true_block)) {
		return false;
	}
	const auto inside = [&](const Block* block) { return Inside(w, block); };
	for (const auto* block: w.loop) {
		if (block != vote && !std::ranges::all_of(block->ImmSuccessors(), inside)) return false;
		for (const auto& inst: *block) {
			if (HasSideEffects(inst.GetOpcode()) && inst.GetOpcode() != ValueOpcode::Reference) {
				return false;
			}
		}
	}
	const auto* body = Producer(w.skip->Arg(0), ValueOpcode::LogicalAnd);
	for (uint32_t arg = 0; body != nullptr && arg < 2; ++arg) {
		const auto* lanes = body->Arg(arg).Resolve().TryInstruction();
		if (lanes != nullptr && !Inside(w, lanes->Parent())) w.lanes = body->Arg(arg);
	}
	auto* exec = HeaderPhi(records, bvh.Arg(2));
	if (w.lanes.IsEmpty() || exec == nullptr) return false;
	w.bvh      = &bvh;
	w.lanes_in = {w.skip->Arg(0).Resolve().TryInstruction(), exec,
	              Entry(records, *exec).Resolve().TryInstruction()};

	// The TLAS walk's intersection, in the walk's own loop, takes the world ray.
	uint32_t found = 0;
	for (const auto* other: intersections) {
		if (other->Flags<uint32_t>() != 1u || EnclosingLoop(program, other->Parent()) != w.header) {
			continue;
		}
		const auto* words = Producer(other->Arg(0), ValueOpcode::CompositeConstructU32x4);
		const auto* tlas  = words != nullptr ? DescriptorAddress(*words) : nullptr;
		const auto* ray   = Producer(other->Arg(1), ValueOpcode::MakeImageAddress);
		const auto  low   = tlas != nullptr ? Slot(tlas->Arg(0)) : std::nullopt;
		const auto  high  = tlas != nullptr ? Slot(tlas->Arg(1)) : std::nullopt;
		if (!low || !high || ray == nullptr || ++found > 1) return false;
		w.tlas = {*low, *high};
		for (uint32_t arg = 1; arg < 8; ++arg) {
			const auto* value = ray->Arg(arg).Resolve().TryInstruction();
			if (value != nullptr &&
			    BlockIndex(program, value->Parent()) >= BlockIndex(program, w.header)) {
				return false;
			}
			w.ray[(arg + 5) % 7] = ray->Arg(arg);
		}
	}
	if (found == 0) return false;

	// The record loop's commit: a branch on whether a lane commits into selects of the committed
	// values over record loop phis, which the BLAS loop's phis start from the walk's header phis;
	// exit phis take those past a skipped body.
	for (auto* block: records.loop) {
		const auto* ref = Producer(block->condition, ValueOpcode::ConditionRef);
		if (ref == nullptr || block->terminator.kind != CFG::TerminatorKind::ConditionalBranch) {
			continue;
		}
		// Each select's old record loop phi and new value.
		std::vector<std::pair<const Inst*, Value>> selects;
		bool                                       kept = true;
		for (auto& inst: *block->terminator.true_block) {
			if (inst.GetOpcode() != ValueOpcode::SelectU32 || inst.Arg(0) != ref->Arg(0)) continue;
			const auto* old = HeaderPhi(records, inst.Arg(2));
			kept            = kept && old != nullptr;
			selects.emplace_back(old, inst.Arg(1));
		}
		if (selects.empty() || !kept) continue;
		if (w.commit != nullptr) return false;
		w.commit = ref->Arg(0).Resolve().TryInstruction();
		for (const auto& [old, committed]: selects) {
			const auto* outer = HeaderPhi(blases, Entry(records, *old));
			auto*       value = outer != nullptr ? HeaderPhi(w, Entry(blases, *outer)) : nullptr;
			if (value == nullptr) return false;
			Inst* exit = nullptr;
			for (auto& inst: *vote) {
				if (inst.GetOpcode() != ValueOpcode::Phi ||
				    inst.Arg(Incoming(inst, enter)) != Value(value)) {
					continue;
				}
				if (exit != nullptr) return false;
				exit = &inst;
			}
			if (exit == nullptr) return false;
			w.commits.emplace_back(exit, committed);
		}
	}
	if (w.commit == nullptr) return false;

	// The instance flags load reads the instance the TLAS leaf names, and its block's branch into
	// the BLAS loop is whether a lane tests the instance's records.
	for (auto* block: w.loop) {
		for (auto& inst: *block) {
			if (AddressWord(program, &inst, InstanceFlags) == nullptr) continue;
			if (w.flags != nullptr) return false;
			w.flags = &inst;
		}
	}
	const auto* address = AddressWord(program, w.flags, InstanceFlags);
	const auto* ref     = address != nullptr
	                          ? Producer(w.flags->Parent()->condition, ValueOpcode::ConditionRef)
	                          : nullptr;
	if (ref == nullptr || w.flags->Parent()->terminator.true_block != blases.preheader) {
		return false;
	}
	w.instance = {Producer(address->Arg(0), ValueOpcode::IAdd32),
	              address->Arg(1).Resolve().TryInstruction()};
	w.accepted = ref->Arg(0).Resolve().TryInstruction();
	if (w.instance[0] == nullptr || w.instance[1] == nullptr || w.accepted == nullptr ||
	    !InstanceArray(*w.instance[0], w.tlas[0])) {
		return false;
	}

	// Record word 1 is read at +4 of each record through a V# over the BLAS at the instance's +0.
	const auto blas = [&](const Inst* word, uint32_t offset) {
		const auto* at = AddressWord(program, word, offset);
		return at != nullptr && at->Arg(0) == address->Arg(0) && at->Arg(1) == address->Arg(1);
	};
	for (auto& phi: *records.header) {
		if (phi.GetOpcode() != ValueOpcode::Phi) continue;
		bool word1 = phi.NumArgs() != 0;
		for (size_t arg = 0; arg < phi.NumArgs(); ++arg) {
			const auto* read = Producer(phi.Arg(arg), ValueOpcode::ReadConstBuffer);
			const auto  base =
			    read != nullptr ? RecordBase(read->Arg(0)) : std::array<const Inst*, 2> {};
			word1 = word1 && blas(base[0], InstanceBlas) && blas(base[1], InstanceBlas + 4) &&
			        program.memory_info[read->Flags<MemoryFlags>().index].offset == 4u;
		}
		if (!word1) continue;
		if (w.word1 != nullptr) return false;
		w.word1 = &phi;
	}

	std::unordered_map<const Inst*, Value> copies;
	const auto copied = [&](Value value) { return !Copy(w, value, copies).IsEmpty(); };
	return w.word1 != nullptr && copied(Value(w.accepted)) && copied(Value(w.commit)) &&
	       std::ranges::all_of(w.commits,
	                           [&](const auto& commit) { return copied(commit.second); });
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

// Under the query, the walk skips its body: each iteration takes a lane's next candidate and runs
// copies of the walk's instance and commit tests on it, and a lane leaves at the first it commits.
void RewriteWalk(Program& program, Walk& w) {
	using Op = ValueOpcode;
	const Inserter entry {w.preheader, w.preheader->end()};
	const auto     valid = entry(Op::RayQueryValid, {});
	const auto     start = entry(Op::LogicalAnd, {valid, w.lanes});
	entry(Op::RayQueryInitialize, {w.ray[0], w.ray[1], w.ray[2], w.ray[3], w.ray[4], w.ray[5],
	                               w.ray[6], CullMask(w, Value(w.accepted)), start});
	const Inserter skip {w.skip->Parent(), Position(*w.skip)};
	w.skip->SetArg(0, skip(Op::LogicalAnd, {w.skip->Arg(0), skip(Op::LogicalNot, {valid})}));
	auto& alive = *w.header->PrependNewInst(w.header->begin(), Op::Phi);
	alive.SetFlags(Type::U1);
	alive.AddPhiOperand(w.preheader, start);

	auto*          vote = w.leave->Parent();
	const Inserter at {vote, std::ranges::find_if(*vote, [](const Inst& inst) {
		                   return inst.GetOpcode() != Op::Phi;
	                   })};
	const auto     current   = at(Op::RayQueryProceed, {Value(&alive)});
	const auto     candidate = at(Op::RayQueryCandidate, {current});
	const auto     with      = [&](Op op, Value value, uint32_t immediate) {
		return at(op, {value, Value(immediate)});
	};
	const auto halves = [&](Value address) {
		return std::array {with(Op::CompositeExtractU64, address, 0),
		                   with(Op::CompositeExtractU64, address, 1)};
	};
	// A per-lane load of the guest dword at `offset` from an address.
	const auto load = [&](const std::array<Value, 2>& address, uint32_t offset) {
		const MemoryFlags flags {.index = static_cast<uint32_t>(program.memory_info.size()),
		                         .pc    = w.flags->Flags<MemoryFlags>().pc};
		program.memory_info.push_back(
		    {.kind = ResourceKind::Global, .offset = offset, .address_is_full = true});
		const auto& [low, high] = address;
		return at(Op::LoadAddressU32, {at(Op::GetAddressResource, {low, high}), low, high, current},
		          std::bit_cast<uint64_t>(flags));
	};
	const auto u64 = [&](Value low) { return at(Op::CompositeConstructU64, {low, Value(0u)}); };
	const auto instance = halves(at(Op::RayQueryCandidateInstance, {current}));
	const auto blas     = at(Op::CompositeConstructU64,
	                         {load(instance, InstanceBlas), load(instance, InstanceBlas + 4)});
	const auto record =
	    halves(at(Op::IAdd64, {blas, with(Op::ShiftLeftLogical64,
		                                  u64(with(Op::CompositeExtractU32x4, candidate, 3)), 3)}));
	// The guest's t numerator and denominator have the sign of its facing, in the node's vertex
	// order, which the mirror reverses for odd remaps of a triangle's barycentric vertices: those
	// of node word 15.
	const auto triangle = load(record, 0);
	const auto vertices =
	    halves(at(Op::IAdd64, {blas, with(Op::ShiftLeftLogical64,
		                                  u64(with(Op::ShiftRightLogical32, triangle, 3)), 6)}));
	const auto shift = with(Op::ShiftLeftLogical32, with(Op::BitwiseAnd32, triangle, 7), 3);
	const auto order =
	    with(Op::BitwiseAnd32, at(Op::ShiftRightLogical32, {load(vertices, 60), shift}), 0xf);
	const auto odd =
	    with(Op::INotEqual32,
		     with(Op::BitwiseAnd32, at(Op::ShiftRightLogical32, {Value(OddRemaps), order}), 1), 0);
	const auto faces = at(Op::LogicalXor, {at(Op::RayQueryCandidateFrontFace, {current}), odd});
	const auto t     = with(Op::CompositeExtractU32x4, candidate, 0);
	const std::array<Value, None> natives {
	    current,
	    at(Op::SelectU32, {faces, t, with(Op::BitwiseOr32, t, 0x80000000u)}),
	    at(Op::SelectU32, {faces, Value(One), Value(0xbf800000u)}),
	    load(instance, InstanceFlags),
	    load(record, 4),
	    instance[0],
	    instance[1]};

	std::unordered_map<const Inst*, Value> copies;
	const auto copy = [&](Value value) { return Copy(w, value, copies, &at, natives); };
	const auto accept =
	    at(Op::LogicalAnd,
		   {current, at(Op::LogicalAnd, {copy(Value(w.accepted)), copy(Value(w.commit))})});
	for (const auto& [exit, value]: w.commits) {
		const auto chosen = at(Op::SelectU32, {accept, copy(value), Value(exit)});
		Redirect(*exit, chosen, *chosen.Instruction());
	}
	const auto later = at(Op::LogicalAnd, {current, at(Op::LogicalNot, {accept})});
	alive.AddPhiOperand(w.latch, later);

	// The walk leaves once no lane has a candidate left.
	const Inserter leave {vote, Position(*w.leave)};
	const auto     any =
	    leave(Op::ConditionRef, {later}, static_cast<uint64_t>(CFG::BranchCondition::ExecNonZero));
	w.leave->SetArg(0, leave(Op::SelectU1, {valid, leave(Op::LogicalNot, {any}), w.leave->Arg(0)}));
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
		Traversal   traversal;
		Walk        walk;
		const bool  nested  = bvh->Flags<uint32_t>() == 1u;
		const bool  matched = nested ? MatchNested(program, *bvh, intersections, traversal)
		                             : Match(program, *bvh, traversal);
		const bool  walked  = !matched && nested && MatchWalk(program, *bvh, intersections, walk);
		const auto& tlas    = walked ? walk.tlas : traversal.tlas;
		if ((matched || walked) && (traced == 0 || tlas == program.info.ray_query_tlas)) {
			walked ? RewriteWalk(program, walk) : Rewrite(program, traversal);
			program.info.ray_query_tlas = tlas;
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
