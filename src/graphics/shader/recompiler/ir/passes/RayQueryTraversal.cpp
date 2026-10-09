#include "graphics/shader/recompiler/ir/passes/RayQueryTraversal.h"

#include "common/logging/log.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/RayQuery.h"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// The words of the layout (LoadGuestInstance in rt_common.inc). Record dwords take 6 bits: the low
// 4 in their field and the high 2 in word 3; the transform's are relative to a base dword.
//   0: transform[0..7] - base, 4 bits each
//   1: transform[8..11] - base, 4 bits each; instance_offset / 8 << 16; blas_dword << 20;
//      blas_packed << 24; root_dword << 25; flip_facing << 29; (record_stride - 128) / 32 << 30
//   2: mask_dword; mask_shift << 4; flags_dword << 9; has flags << 13; flags_shift << 14;
//      has mask << 19; base << 20; mask_width << 25
//   3: inline_offset / 8; inline_root << 8; inline_type << 16; has an inline type << 19;
//      blas_dword >> 4 << 20; root_dword >> 4 << 22; mask_dword >> 4 << 24; flags_dword >> 4 << 26;
//      inline_offset bit 2 << 28; node_count_80 << 29
// A layout of the first 16 dwords of a 128-byte record, without the newer fields, packs as
// before they existed.
std::array<uint32_t, 4> RayTracingLayout::Pack() const {
	std::array<uint32_t, 4> words {};
	const uint32_t          base = TransformBase();
	for (uint32_t i = 0; i < 12; i++) {
		words[i / 8] |= ((transform[i] - base) & 15u) << ((i % 8) * 4);
	}
	words[1] |= ((instance_offset / 8u) & 15u) << 16u | (blas_dword & 15u) << 20u | (blas_packed ? 1u : 0u) << 24u |
	            (root_dword & 15u) << 25u | (flip_facing ? 1u : 0u) << 29u | ((record_stride - 128u) / 32u & 3u) << 30u;
	const bool flags = flags_dword != NoField;
	const bool mask  = mask_dword != NoField;
	words[2] = (mask ? mask_dword & 15u : 0u) | (mask_shift & 31u) << 4u | (flags ? flags_dword & 15u : 0u) << 9u |
	           (flags ? 1u : 0u) << 13u | (flags_shift & 31u) << 14u | (mask ? 1u : 0u) << 19u | (base & 31u) << 20u |
	           (mask ? mask_width & 31u : 0u) << 25u;
	if (inline_type != NoField) {
		words[3] = ((inline_offset / 8u) & 0xffu) | (inline_root & 0xffu) << 8u | (inline_type & 7u) << 16u | 1u << 19u |
		           ((inline_offset >> 2u) & 1u) << 28u;
	}
	words[3] |= (blas_dword >> 4u & 3u) << 20u | (root_dword >> 4u & 3u) << 22u | (mask ? mask_dword >> 4u & 3u : 0u) << 24u |
	            (flags ? flags_dword >> 4u & 3u : 0u) << 26u | (node_count_80 ? 1u : 0u) << 29u;
	return words;
}

// The dword the transform's are counted from: 0 when they all are in the first 16 dwords.
uint32_t RayTracingLayout::TransformBase() const {
	const auto [low, high] = std::ranges::minmax(transform);
	return high < 16 ? 0u : low;
}

// The format word of RayQueryAlpha: material_dword, table_dword << 6, first_dword << 12,
// vertex_align << 18, vertex_mask << 19, blas_dword << 27, blas_packed << 31.
uint32_t RayTracingAlphaFields::Pack(const RayTracingLayout& layout) const {
	return (material_dword & 63u) | (table_dword & 63u) << 6u | (first_dword & 63u) << 12u |
	       (vertex_align ? 1u : 0u) << 18u | (vertex_mask & 0xffu) << 19u | (layout.blas_dword & 15u) << 27u |
	       (layout.blas_packed ? 1u : 0u) << 31u;
}

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
	// A nested walk's TLAS walk intersection, the TLAS address words of its T# and the address
	// resource of the instance record its BLAS address words read.
	const Inst*          walk     = nullptr;
	const Inst*          resource = nullptr;
	std::array<Value, 2> words {};
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
// side effects and leave only at the latch. An any hit walk (or one with a fixed t max) has no
// extent phi: t.extent is then null, which the ray query pass's rewrite does not take.
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
	if (t.exec == nullptr) return false;

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
	const auto* moved = t.extent != nullptr ? Back(t, *t.extent).Resolve().TryInstruction() : nullptr;
	for (auto it = Position(*t.vote); moved != nullptr && it != voting->end(); ++it) {
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

// The BLAS address words of a nested walk's T# over the instance record `instance`: its words +0
// and +4 (header formats 1, 3 and 6), or, in units of 8 bytes, its word +48 and the low 22 bits of
// +52 (sce::Psr's records in GTA V 01.011, cs 6aeb221de7ff018e). `wide` is then the address, which
// the prim record V# takes the words of.
struct BlasWords {
	Inst*       word0    = nullptr;
	Inst*       word1    = nullptr;
	const Inst* instance = nullptr;
	const Inst* wide     = nullptr;

	// Whether `inst` is word `k` of the BLAS address.
	bool Base(const Inst* inst, uint32_t k) const {
		if (wide == nullptr) return inst == (k == 0 ? word0 : word1);
		return inst != nullptr && inst->GetOpcode() == ValueOpcode::CompositeExtractU64 &&
		       inst->Arg(0).Resolve().TryInstruction() == wide && inst->Arg(1).Resolve() == Value(k);
	}
};

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

// The recognizers run after resource tracking (the ray query pass), where the TLAS words of a walk
// are reads of the flattened SRT, and before it (the native ray tracing forms, which give resource
// tracking the TLAS source and the code that stays in the BVH), where they are the walk's values
// from before its loops. Whether `value` reads the TLAS word `word` (with its `slot` when tracked).
bool TlasWord(Value value, Value word, uint32_t slot, bool tracked) {
	return tracked ? Slot(value) == slot : value.Resolve() == word.Resolve();
}

// Whether the low instance address word, a sum, is the TLAS header's instance array, read from the
// flattened SRT `slot` (or the TLAS address word `word`, see TlasWord), plus (node >> 3) << 7.
bool InstanceArray(const Inst& low, uint32_t slot, Value word = {}, bool tracked = true) {
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
		    (TlasWord(array->Arg(0), word, slot, tracked) ||
		     TlasWord(array->Arg(1), word, slot, tracked))) {
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

// The wide form of BlasWords: `wide`, the address in units of 8 bytes.
BlasWords WideBlasWords(const Program& program, const Inst* wide) {
	if (wide == nullptr || wide->GetOpcode() != ValueOpcode::ShiftLeftLogical64 ||
	    Immediate(wide->Arg(1)) != 3u) {
		return {};
	}
	const auto* pair = Producer(wide->Arg(0), ValueOpcode::CompositeConstructU64);
	const auto* bits = pair != nullptr ? Producer(pair->Arg(1), ValueOpcode::BitwiseAnd32) : nullptr;
	BlasWords   words {.wide = wide};
	words.word0 = pair != nullptr ? Producer(pair->Arg(0), ValueOpcode::LoadAddressU32) : nullptr;
	for (uint32_t arg = 0; bits != nullptr && arg < 2; ++arg) {
		if (Immediate(bits->Arg(1 - arg)) == 0x3fffffu) {
			words.word1 = Producer(bits->Arg(arg), ValueOpcode::LoadAddressU32);
		}
	}
	words.instance = AddressWord(program, words.word0, 48);
	if (words.word1 == nullptr || words.instance == nullptr ||
	    AddressWord(program, words.word1, 52) != words.instance) {
		return {};
	}
	return words;
}

BlasWords InstanceBlasWords(const Program& program, const Inst& descriptor, bool tracked) {
	const auto* low   = Producer(descriptor.Arg(0), ValueOpcode::CompositeExtractU64);
	const auto* field = low != nullptr ? Producer(low->Arg(0), ValueOpcode::BitwiseAnd64) : nullptr;
	const auto* shift =
	    field != nullptr ? Producer(field->Arg(0), ValueOpcode::ShiftRightLogical64) : nullptr;
	if (!tracked && DescriptorAddress(descriptor) == nullptr && shift != nullptr) {
		return WideBlasWords(program, shift->Arg(0).Resolve().TryInstruction());
	}
	const auto* blas = DescriptorAddress(descriptor);
	BlasWords   words;
	words.word0    = blas != nullptr ? Producer(blas->Arg(0), ValueOpcode::LoadAddressU32) : nullptr;
	words.word1    = blas != nullptr ? Producer(blas->Arg(1), ValueOpcode::LoadAddressU32) : nullptr;
	words.instance = AddressWord(program, words.word0, InstanceBlas);
	if (words.instance == nullptr ||
	    AddressWord(program, words.word1, InstanceBlas + 4) != words.instance) {
		return {};
	}
	return words;
}

// A BLAS walk over 32-bit node pointers inside a TLAS walk. The TLAS walk intersects its nodes with
// the world ray under a T# over the TLAS header the flattened SRT holds; its instance leaf node
// loads instance record node >> 3 of the header's instance array and enters a loop under a T# over
// the BLAS at the record's +0. That loop reads prim records through a V# over the BLAS with stride
// 8: word 0 is the next node and word 1 a value the loop keeps.
bool MatchNested(const Program& program, Inst& bvh, std::span<Inst* const> intersections,
                 Traversal& t, bool tracked = true) {
	const auto* descriptor = Producer(bvh.Arg(0), ValueOpcode::CompositeConstructU32x4);
	const auto* address    = Producer(bvh.Arg(1), ValueOpcode::MakeImageAddress);
	if (descriptor == nullptr || address == nullptr ||
	    !MatchLoop(program, bvh, address->Arg(1), t)) {
		return false;
	}
	auto*      node  = HeaderPhi(t, address->Arg(0));
	const auto words = InstanceBlasWords(program, *descriptor, tracked);
	auto* const word0    = words.word0;
	auto* const word1    = words.word1;
	const auto* instance = words.instance;
	if (node == nullptr || instance == nullptr || Inside(t, word0->Parent()) ||
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
		if (tlas == nullptr || (tracked && (!low_slot || !high_slot))) continue;
		if (walk != nullptr) return false;
		walk    = other;
		t.tlas  = {low_slot.value_or(0u), high_slot.value_or(0u)};
		t.words = {tlas->Arg(0), tlas->Arg(1)};
	}
	t.walk = walk;
	if (walk == nullptr) return false;

	// Its world ray (which the ray query pass needs from before the outermost loop around it, see
	// NestedReads); t_max is this loop's extent.
	const auto* ray   = Producer(walk->Arg(1), ValueOpcode::MakeImageAddress);
	auto*       outer = EnclosingLoop(program, walk->Parent());
	while (auto* up = EnclosingLoop(program, outer)) {
		outer = up;
	}
	if (ray == nullptr) return false;
	const auto before = [&](Value word) {
		const auto* value = word.Resolve().TryInstruction();
		return value == nullptr || BlockIndex(program, value->Parent()) < BlockIndex(program, outer);
	};
	for (uint32_t arg = 2; arg < 8; ++arg) {
		t.ray[arg - 2] = ray->Arg(arg);
	}
	t.ray[6] = t.extent != nullptr ? Entry(t, *t.extent) : address->Arg(1);

	if ((!tracked && (!before(t.words[0]) || !before(t.words[1]))) ||
	    !InstanceArray(*low, t.tlas[0], t.words[0], tracked)) {
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
	t.record        = record;
	const auto base = record != nullptr ? RecordBase(record->Arg(0)) : std::array<const Inst*, 2> {};
	if (record == nullptr || !words.Base(base[0], 0) || !words.Base(base[1], 1) ||
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

	t.resource = instance;
	return true;
}

// Whether the ray query pass can drive a nested walk (MatchNested), and its cull mask. Its rewrite
// needs the committed t (the extent).
bool NestedReads(const Program& program, Traversal& t) {
	if (t.extent == nullptr) return false;
	// The query starts before the walk, from the world ray, which must be defined before the
	// outermost loop around the TLAS walk.
	auto* outer = EnclosingLoop(program, t.walk->Parent());
	while (auto* up = EnclosingLoop(program, outer)) {
		outer = up;
	}
	for (uint32_t k = 0; k < 6; ++k) {
		const auto* value = t.ray[k].Resolve().TryInstruction();
		if (value != nullptr && BlockIndex(program, value->Parent()) >= BlockIndex(program, outer)) {
			return false;
		}
	}
	const auto* node     = t.state[0];
	const auto* instance = t.resource;
	auto* const low      = t.instance[0];
	auto* const high     = t.instance[1];
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
		    op == ValueOpcode::SubgroupReduceU32 || !seen.insert(inst).second) {
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
	const Inst*                          walk = nullptr; // The TLAS walk's intersection.
	std::array<Value, 2>                 words {};       // The TLAS address words of its T#.
	Loop                                 blas;           // The BLAS loop.
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
	constexpr std::array excluded {
	    Op::ConditionRef,      Op::ReadFirstLane, Op::ReadLane,     Op::WriteLane,
	    Op::SubgroupReduceU32, Op::Permlane16U32, Op::PermuteU32,   Op::BpermuteU32,
	    Op::SwizzleU32,        Op::DppMoveU32,    Op::DppUpdateU32, Op::LoadDeviceAddressU32};
	const auto plain = [](Type type) {
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

bool MatchWalk(const Program& program, Inst& bvh, std::span<Inst* const> intersections, Walk& w,
               bool tracked = true) {
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
		if (tlas == nullptr || (tracked && (!low || !high)) || ray == nullptr || ++found > 1) {
			return false;
		}
		w.tlas  = {low.value_or(0u), high.value_or(0u)};
		w.walk  = other;
		w.words = {tlas->Arg(0), tlas->Arg(1)};
		for (const auto word: w.words) {
			const auto* value = tracked ? nullptr : word.Resolve().TryInstruction();
			if (value != nullptr &&
			    BlockIndex(program, value->Parent()) >= BlockIndex(program, w.header)) {
				return false;
			}
		}
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
	// the BLAS loop is whether a lane tests the instance's records. (Before resource tracking, the
	// walk may read the flags again past that branch: GTA V 01.005, cs a31ec6a25be5e017.)
	for (auto* block: w.loop) {
		for (auto& inst: *block) {
			if (AddressWord(program, &inst, InstanceFlags) == nullptr ||
			    (!tracked && block->terminator.true_block != blases.preheader)) {
				continue;
			}
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
	    !InstanceArray(*w.instance[0], w.tlas[0], w.words[0], tracked)) {
		return false;
	}

	// Record word 1 is read at +4 of each record through a V# over the BLAS the instance names (see
	// BlasWords).
	const auto instance = [&](const Inst* at) {
		return at != nullptr && at->Arg(0) == address->Arg(0) && at->Arg(1) == address->Arg(1);
	};
	const auto blas = [&](std::array<const Inst*, 2> base) {
		if (instance(AddressWord(program, base[0], InstanceBlas)) &&
		    instance(AddressWord(program, base[1], InstanceBlas + 4))) {
			return true;
		}
		const auto* wide = !tracked && base[0] != nullptr &&
		                           base[0]->GetOpcode() == ValueOpcode::CompositeExtractU64
		                       ? base[0]->Arg(0).Resolve().TryInstruction()
		                       : nullptr;
		const auto words = WideBlasWords(program, wide);
		return instance(words.instance) && words.Base(base[0], 0) && words.Base(base[1], 1);
	};
	for (auto& phi: *records.header) {
		if (phi.GetOpcode() != ValueOpcode::Phi) continue;
		bool word1 = phi.NumArgs() != 0;
		for (size_t arg = 0; arg < phi.NumArgs(); ++arg) {
			const auto* read = Producer(phi.Arg(arg), ValueOpcode::ReadConstBuffer);
			const auto  base =
			    read != nullptr ? RecordBase(read->Arg(0)) : std::array<const Inst*, 2> {};
			word1 = word1 && blas(base) &&
			        program.memory_info[read->Flags<MemoryFlags>().index].offset == 4u;
		}
		if (!word1) continue;
		if (w.word1 != nullptr) return false;
		w.word1 = &phi;
	}

	w.blas = blases;
	return w.word1 != nullptr;
}

// Whether the ray query pass can drive a walk (MatchWalk): its per-candidate code copies.
bool WalkCopies(const Walk& w) {
	std::unordered_map<const Inst*, Value> copies;
	const auto copied = [&](Value value) { return !Copy(w, value, copies).IsEmpty(); };
	return copied(Value(w.accepted)) && copied(Value(w.commit)) &&
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
		// (The TLAS walks of native sites are not found either: other flags.)
		if ((bvh->Flags<uint32_t>() & BvhIntersectNative) != 0) continue;
		Traversal   traversal;
		Walk        walk;
		const bool  nested  = bvh->Flags<uint32_t>() == 1u;
		const bool  matched = nested ? MatchNested(program, *bvh, intersections, traversal) &&
		                                   NestedReads(program, traversal)
		                             : Match(program, *bvh, traversal);
		const bool  walked  = !matched && nested &&
		                      MatchWalk(program, *bvh, intersections, walk) && WalkCopies(walk);
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

// ---- The native ray tracing forms (sce::Psr's traversal templates), before resource tracking.
// ----
//
// An any hit traversal with lane masks (Marvel's Wolverine's shadow rays, waves of 32 lanes) walks
// the TLAS with a wave-wide stack of (node, lane mask) pairs in VGPR lanes, from the root node: it
// pops a pair at the end of each iteration (ReadLane at the stack pointer) and leaves when the
// stack or the live lanes are empty. At an instance node the instance setup (the instance mask
// test, the object space ray, the BLAS) runs for the lanes of the pair, then each lane walks the
// BLAS on its own (one loop, or two copies of it that the compiler made) and runs the leaf code for
// its triangles, loading each next leaf list entry from the node: an accepted hit writes the
// primitive word and the lane leaves the live lanes. With the host's structures, the stack yields
// the candidates of the lanes' ray queries instead: each pop takes the smallest instance any lane
// has and the lanes that have it, each lane's BLAS walk starts at its own smallest leaf of that
// instance (the leaf list entry, loaded as the walk loads the next one) and ends after it, and the
// wave leaves when no lane has a candidate. Every triangle is a candidate of the leaf code, which
// tests its facing and opacity. The first iteration runs without lanes (the walk goes straight to
// the pop, which takes the first candidate).

namespace {


const MemoryInfo& MemoryOf(const Program& program, const Inst& inst) {
	return program.memory_info[inst.Flags<MemoryFlags>().index];
}

// Through bit casts and what lane masked writes keep (Select(lanes, written, kept)).
Value Canonical(Value value) {
	for (uint32_t depth = 0; depth < 16; ++depth) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) break;
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::BitCastF32U32 || op == ValueOpcode::BitCastU32F32 ||
		    op == ValueOpcode::SelectU32) {
			value = inst->Arg(op == ValueOpcode::SelectU32 ? 1 : 0);
			continue;
		}
		break;
	}
	return value;
}

// The two operands of a commutative operation `op` as (one that `first` accepts, the other).
template <typename First>
std::optional<std::pair<Value, Value>> Operands(Value value, ValueOpcode op, First&& first) {
	const auto* inst = Producer(Canonical(value), op);
	if (inst == nullptr) return std::nullopt;
	for (uint32_t side = 0; side < 2; ++side) {
		if (first(inst->Arg(side))) return std::pair {inst->Arg(side), inst->Arg(1 - side)};
	}
	return std::nullopt;
}

// A term of the object space ray: without its sign.
Value Term(Value value) {
	value            = Canonical(value);
	const auto* inst = value.TryInstruction();
	return inst != nullptr && inst->GetOpcode() == ValueOpcode::FPNeg32 ? Canonical(inst->Arg(0))
	                                                                    : value;
}

// A 32-bit guest load at a constant byte offset of an address pair.
struct AddressLoad {
	Value    low;
	Value    high;
	uint32_t offset = 0;
};

std::optional<AddressLoad> LoadOf(const Program& program, Value value) {
	const auto* load = Producer(value, ValueOpcode::LoadAddressU32);
	if (load == nullptr || Immediate(load->Arg(1)) != 0u || Immediate(load->Arg(2)) != 0u)
		return std::nullopt;
	const auto* handle = Producer(load->Arg(0), ValueOpcode::GetAddressResource);
	if (handle == nullptr) return std::nullopt;
	return AddressLoad {handle->Arg(0).Resolve(), handle->Arg(1).Resolve(),
	                    MemoryOf(program, *load).offset};
}

// The 64-bit address of a descriptor's base word: (address >> 8) & mask.
const Inst* DescriptorAddress(Value word) {
	const auto* low  = Producer(word, ValueOpcode::CompositeExtractU64);
	const auto* mask = low != nullptr ? Producer(low->Arg(0), ValueOpcode::BitwiseAnd64) : nullptr;
	const auto* shift =
	    mask != nullptr ? Producer(mask->Arg(0), ValueOpcode::ShiftRightLogical64) : nullptr;
	if (shift == nullptr || Immediate(low->Arg(1)) != 0u || Immediate(shift->Arg(1)) != 8u)
		return nullptr;
	return shift->Arg(0).Resolve().TryInstruction();
}

std::optional<uint32_t> UserRegister(Value value) {
	const auto* user = Producer(value, ValueOpcode::GetUserData);
	if (user == nullptr) return std::nullopt;
	return static_cast<uint32_t>(user->Arg(0).ScalarRegister());
}

// The ballot whose lanes a wave mask (its low word: waves of 32 lanes) holds.
const Inst* BallotOf(Value mask) {
	const auto* low = Producer(mask, ValueOpcode::CompositeExtractU32x4);
	if (low == nullptr || Immediate(low->Arg(1)) != 0u) return nullptr;
	return Producer(low->Arg(0), ValueOpcode::Ballot);
}

Inst* LoopPhi(Value value, const Block* header) {
	auto* inst = Producer(value, ValueOpcode::Phi);
	return inst != nullptr && inst->Parent() == header ? inst : nullptr;
}

Block::iterator AfterPhis(Block& block) {
	return std::ranges::find_if(
	    block, [](const Inst& inst) { return inst.GetOpcode() != ValueOpcode::Phi; });
}

struct BlasWalk {
	Block*             preheader = nullptr;
	Block*             header    = nullptr;
	Block*             latch     = nullptr;
	Inst*              node      = nullptr; // The node phi (the leaf list entry's first word).
	Inst*              word      = nullptr; // The entry's second word (the primitive word).
	const Inst*        load      = nullptr; // The load of the next entry.
	Inst*              vote      = nullptr;
	std::vector<Inst*> lanes;
};

struct LaneMaskWalk {
	Block*                preheader = nullptr;
	Block*                header    = nullptr;
	Block*                latch     = nullptr;
	Block*                merge     = nullptr;
	Block*                exit      = nullptr;
	Inst*                 bvh       = nullptr;
	Inst*                 node      = nullptr;
	Inst*                 pair_mask = nullptr;
	Inst*                 exit_ref  = nullptr;
	Value                 lanes;
	Value                 lanes_without; // Lanes `lanes` leaves out (none: empty).
	std::array<Value, 7>  ray {}; // World origin xyz, direction xyz, t max.
	std::array<Value, 2>  tlas {};
	uint32_t              cull_mask = 0xff;
	// A ray mask the program computes before the walk (the instance mask's bits & it; empty: the
	// constant `cull_mask`).
	Value                 mask_value;
	// Whether the instance setup tests the instance mask (a walk without the test may take every
	// instance only when `mask_optional`).
	bool                  masked        = false;
	bool                  mask_optional = false;
	std::vector<BlasWalk> blases;
	// A BLAS the wave walks (scalar nodes; ClosestWave): its loops, the value its box test's node
	// starts at (the root, before them) and its box and triangle tests.
	Block*                wave_header    = nullptr;
	Block*                wave_latch     = nullptr;
	Value                 wave_root;
	Inst*                 wave_node      = nullptr; // The phi of the box test's node, when it starts at the root.
	Inst*                 wave_box       = nullptr;
	Inst*                 wave_triangles = nullptr;
	// The instance record address the setup loads the root from.
	AddressLoad           record;
	RayTracingLayout      layout;
	NativeTraversals      source;
};

// The ray mask of a walk's queries.
Value RayMask(const LaneMaskWalk& w) {
	return w.mask_value.IsEmpty() ? Value(w.cull_mask) : w.mask_value;
}

// The ConditionRef of a conditional branch, and whether the branch tests its negation.
std::pair<Inst*, bool> BranchCondition(const Block& block) {
	if (auto* ref = Producer(block.condition, ValueOpcode::ConditionRef); ref != nullptr) return {ref, false};
	const auto* negated = Producer(block.condition, ValueOpcode::LogicalNot);
	return {negated != nullptr ? Producer(negated->Arg(0), ValueOpcode::ConditionRef) : nullptr, true};
}

// The lane bit of a wave mask (waves of 32 lanes): (mask >> (lane id & 31)) & 1 != 0; its mask.
Value LaneMaskOf(const Inst* lane) {
	if (lane == nullptr || lane->GetOpcode() != ValueOpcode::INotEqual32) return {};
	for (uint32_t side = 0; side < 2; ++side) {
		const auto* bit = Producer(lane->Arg(side), ValueOpcode::BitwiseAnd32);
		if (bit == nullptr || Immediate(lane->Arg(1 - side)) != 0u) continue;
		for (uint32_t b = 0; b < 2; ++b) {
			const auto* shift = Producer(bit->Arg(b), ValueOpcode::ShiftRightLogical32);
			const auto* index = shift != nullptr ? Producer(shift->Arg(1), ValueOpcode::BitwiseAnd32) : nullptr;
			if (index == nullptr || Immediate(bit->Arg(1 - b)) != 1u) continue;
			for (uint32_t i = 0; i < 2; ++i) {
				if (Immediate(index->Arg(1 - i)) == 31u && Producer(index->Arg(i), ValueOpcode::LaneId) != nullptr)
					return shift->Arg(0).Resolve();
			}
		}
	}
	return {};
}

// A node type test of the TLAS walk: (node & 7) == type or != type; its type and whether it is ==.
std::optional<std::pair<uint32_t, bool>> TypeTest(Value condition, const Inst* node, uint32_t depth = 0) {
	const auto* test = condition.Resolve().TryInstruction();
	// (Or one in a conjunction with lane conditions: waves of 32 lanes branch on the lanes that take
	// the node.)
	if (test != nullptr && test->GetOpcode() == ValueOpcode::LogicalAnd && depth < 4) {
		const auto a = TypeTest(test->Arg(0), node, depth + 1), b = TypeTest(test->Arg(1), node, depth + 1);
		return a && !b ? a : b && !a ? b : std::nullopt;
	}
	// Or the lane bit of a wave mask made of it: its lanes (Select(test, lanes, 0)) and others.
	if (const auto mask = LaneMaskOf(test); !mask.IsEmpty() && depth < 4) {
		std::vector<Value> parts {mask};
		std::optional<std::pair<uint32_t, bool>> found;
		for (size_t at = 0; at < parts.size() && at < 16; ++at) {
			const auto* part = parts[at].Resolve().TryInstruction();
			if (part == nullptr) continue;
			if (part->GetOpcode() == ValueOpcode::BitwiseAnd32) {
				parts.push_back(part->Arg(0));
				parts.push_back(part->Arg(1));
			} else if (part->GetOpcode() == ValueOpcode::SelectU32 && Immediate(part->Arg(2)) == 0u) {
				if (const auto type = TypeTest(part->Arg(0), node, depth + 1)) {
					if (found) return std::nullopt;
					found = type;
				}
			}
		}
		return found;
	}
	if (test == nullptr ||
	    (test->GetOpcode() != ValueOpcode::IEqual32 && test->GetOpcode() != ValueOpcode::INotEqual32))
		return std::nullopt;
	for (uint32_t side = 0; side < 2; ++side) {
		const auto  type = Immediate(test->Arg(side));
		const auto* bits = Producer(test->Arg(1 - side), ValueOpcode::BitwiseAnd32);
		if (!type || *type >= 8u || bits == nullptr) continue;
		for (uint32_t b = 0; b < 2; ++b) {
			if (Immediate(bits->Arg(b)) == 7u && bits->Arg(1 - b).Resolve() == Value(const_cast<Inst*>(node)))
				return std::pair {*type, test->GetOpcode() == ValueOpcode::IEqual32};
		}
	}
	return std::nullopt;
}

// The node type of the TLAS leaves whose instance record the walk takes along the edge from
// `from` to `to`: that of the type test of the first conditional branch up from it (through
// blocks with one predecessor).
std::optional<uint32_t> EdgeType(const Block* to, const Block* from, const Inst* node) {
	for (uint32_t depth = 0; depth < 4 && from != nullptr; ++depth) {
		if (from->terminator.kind == CFG::TerminatorKind::ConditionalBranch) {
			const auto [ref, negated] = BranchCondition(*from);
			const auto test           = ref != nullptr ? TypeTest(ref->Arg(0), node) : std::nullopt;
			// The test's value on the edge.
			const bool taken = (to == from->terminator.true_block) != negated;
			return test && taken == test->second ? std::optional(test->first) : std::nullopt;
		}
		const auto preds = from->ImmPredecessors();
		if (preds.size() != 1) return std::nullopt;
		to   = from;
		from = preds[0];
	}
	return std::nullopt;
}

bool InLoop(const Program& program, const Block* block, const Block* header, const Block* latch) {
	const auto at = BlockIndex(program, block);
	return at >= BlockIndex(program, header) && at <= BlockIndex(program, latch);
}

bool DefinedBefore(const Program& program, Value value, const Block* header) {
	const auto* inst = value.Resolve().TryInstruction();
	return !value.IsEmpty() &&
	       (inst == nullptr || BlockIndex(program, inst->Parent()) < BlockIndex(program, header));
}

// A value from before the loop of `header`, or a guest load at a constant offset of an address from
// before it (user data: a V# the loop reads where it uses it), which can be read again before it
// (DescriptorBefore).
bool DescriptorDefinedBefore(const Program& program, Value value, const Block* header) {
	if (DefinedBefore(program, value, header)) return true;
	const auto* load   = Producer(value, ValueOpcode::LoadAddressU32);
	const auto* handle = load != nullptr ? Producer(load->Arg(0), ValueOpcode::GetAddressResource) : nullptr;
	return handle != nullptr && Immediate(load->Arg(1)) == 0u && Immediate(load->Arg(2)) == 0u &&
	       DefinedBefore(program, handle->Arg(0), header) && DefinedBefore(program, handle->Arg(1), header);
}

// A value of DescriptorDefinedBefore at `at`, before the loop of `header`.
Value DescriptorBefore(Program& program, Value value, const Block* header, const Inserter& at) {
	if (DefinedBefore(program, value, header)) return value;
	const auto* load   = Producer(value, ValueOpcode::LoadAddressU32);
	const auto* handle = Producer(load->Arg(0), ValueOpcode::GetAddressResource);
	const auto  again  = at(ValueOpcode::GetAddressResource, {handle->Arg(0), handle->Arg(1)}, handle->Flags<uint64_t>());
	auto        memory = load->Flags<MemoryFlags>();
	program.memory_info.push_back(program.memory_info[memory.index]);
	memory.index      = static_cast<uint32_t>(program.memory_info.size() - 1);
	const auto loaded = at(ValueOpcode::LoadAddressU32, {again, load->Arg(1), load->Arg(2), load->Arg(3)});
	loaded.Instruction()->SetFlags(memory);
	return loaded;
}

// The loop of `header`: its latch, merge and one entry.
bool LoopShape(Block* header, Block*& preheader, Block*& latch, Block*& merge) {
	if (header == nullptr || header->terminator.continue_block == nullptr ||
	    header->terminator.merge_block == nullptr) {
		return false;
	}
	latch              = header->terminator.continue_block;
	merge              = header->terminator.merge_block;
	const auto entries = header->ImmPredecessors();
	if (entries.size() != 2 || std::ranges::find(entries, latch) == entries.end()) return false;
	preheader = entries[0] == latch ? entries[1] : entries[0];
	return true;
}

// The value of `value` before the loop of `header`: it, a lane masked write's value, or a pure
// conversion of such values placed again in the preheader.
std::optional<Value> Hoist(const Program& program, Value value, const Block* header,
                           const Inserter& at, uint32_t depth = 0) {
	value      = value.Resolve();
	auto* inst = value.TryInstruction();
	if (inst == nullptr) return value;
	if (BlockIndex(program, inst->Parent()) < BlockIndex(program, header)) return value;
	if (depth > 4) return std::nullopt;
	const auto op = inst->GetOpcode();
	if (op == ValueOpcode::SelectU32) return Hoist(program, inst->Arg(1), header, at, depth + 1);
	if (op != ValueOpcode::BitCastU32F32 && op != ValueOpcode::BitCastF32U32 &&
	    op != ValueOpcode::ConvertF32S32 && op != ValueOpcode::ConvertF32U32) {
		return std::nullopt;
	}
	const auto arg = Hoist(program, inst->Arg(0), header, at, depth + 1);
	if (!arg) return std::nullopt;
	return at(op, {*arg}, inst->Flags<uint64_t>());
}

// An object space ray component as record dwords times world ray terms (origin 0..2, 1 for the
// translation (3), direction 4..6).
using LinearForm           = std::array<uint32_t, 7>;
constexpr uint32_t NoDword = 0xffffffffu;

template <typename Record>
bool Expand(Value value, const std::array<Value, 6>& world, const Record& record, LinearForm& form,
            uint32_t depth = 0) {
	if (depth > 16) return false;
	const auto canonical = Canonical(value);
	const auto term      = [&](Value v) -> std::optional<uint32_t> {
		const auto t = Term(v);
		for (uint32_t k = 0; k < 6; ++k) {
			if (t == world[k]) return k < 3 ? k : k + 1;
		}
		return std::nullopt;
	};
	const auto set = [&](uint32_t slot, uint32_t dword) {
		if (form[slot] != NoDword) return false;
		form[slot] = dword;
		return true;
	};
	const auto product = [&](Value a, Value b) {
		const auto da = record(Term(a)), db = record(Term(b));
		const auto ta = term(a), tb = term(b);
		if (da && tb) return set(*tb, *da);
		if (db && ta) return set(*ta, *db);
		return false;
	};
	if (const auto dword = record(Term(canonical)); dword) return set(3, *dword);
	const auto* inst = canonical.TryInstruction();
	if (inst == nullptr) return false;
	switch (inst->GetOpcode()) {
		case ValueOpcode::FPNeg32: return Expand(inst->Arg(0), world, record, form, depth + 1);
		case ValueOpcode::FPAdd32:
			return Expand(inst->Arg(0), world, record, form, depth + 1) &&
			       Expand(inst->Arg(1), world, record, form, depth + 1);
		case ValueOpcode::FPMul32: return product(inst->Arg(0), inst->Arg(1));
		case ValueOpcode::FPMad32:
			return product(inst->Arg(0), inst->Arg(1)) &&
			       Expand(inst->Arg(2), world, record, form, depth + 1);
		default: return false;
	}
}

// The instance records as the walk reads them (RayTracingLayout): the record of TLAS leaf `node` is at the TLAS + the 64-bit offset at TLAS +
// `instance_offset`, + (node >> 3) * stride; the BLAS walk's ray is the world ray times the
// record's world to object matrix, its root a record dword, its BLAS the record's 64-bit address
// (or 22 bits of it, << 3); the setup tests the instance mask with a constant ray mask.
bool DeriveLaneLayout(const Program& program, LaneMaskWalk& w) {
	auto& layout = w.layout;
	// The BLAS walk's root (its node before it) and its box test.
	Value       root_value;
	const Inst* intersect = nullptr;
	if (!w.blases.empty()) {
		const auto& blas = w.blases.front();
		root_value       = blas.node->Arg(Incoming(*blas.node, blas.preheader));
		for (const auto& use: blas.node->Uses()) {
			if (use.user->GetOpcode() == ValueOpcode::MakeImageAddress && use.operand == 0)
				intersect = use.user;
		}
	} else if (w.wave_box != nullptr) {
		root_value = w.wave_root;
		intersect  = Producer(w.wave_box->Arg(1), ValueOpcode::MakeImageAddress);
	}
	layout.inline_type = NoField;
	auto root          = LoadOf(program, Canonical(root_value));
	// Or a constant root for the TLAS leaves of another node type whose record is in the header
	// (inline records): Select(<type test>, root, load) or a phi of the root and the load (the
	// branch around the load).
	uint32_t inline_root = NoField;
	if (!root && !root_value.IsEmpty()) {
		const auto* inst = root_value.Resolve().TryInstruction();
		const auto  op   = inst != nullptr ? inst->GetOpcode() : ValueOpcode::Phi;
		for (uint32_t k = 0; inst != nullptr && k < 2 && !root; ++k) {
			const auto constant = op == ValueOpcode::SelectU32 ? Immediate(inst->Arg(1 + k))
			                      : op == ValueOpcode::Phi && inst->NumArgs() == 2 ? Immediate(inst->Arg(k))
			                                                                       : std::nullopt;
			if (!constant) continue;
			root        = LoadOf(program, Canonical(inst->Arg(op == ValueOpcode::SelectU32 ? 2 - k : 1 - k)));
			inline_root = *constant;
		}
	}
	if (!root || intersect == nullptr) return false;
	// rec_lo = base_lo + ((node >> 3) << S).low (or (node >> 3) * stride: records of 160 bytes,
	// ASTRO's PLAYROOM); base_lo = load(TLAS + instance_offset) + TLAS low. The inline records are
	// at TLAS low + a constant: the root's record address is a phi of the two (ASTRO BOT, cs
	// bdd0bb9bd8852dd4), or only the regular records' and the record fields' a phi of the two
	// (ASTRO's PLAYROOM, which branches around the root load).
	AddressLoad record_pair = *root;
	const auto  inline_phi  = [&](const Inst* phi, uint32_t k) {
		const auto* fixed = phi->NumArgs() == 2 ? Producer(phi->Arg(k), ValueOpcode::IAdd32) : nullptr;
		for (uint32_t side = 0; fixed != nullptr && side < 2; ++side) {
			const auto offset = Immediate(fixed->Arg(side));
			const auto type   = offset && fixed->Arg(1 - side).Resolve() == w.tlas[0]
			                        ? EdgeType(phi->Parent(), phi->PhiBlock(k), w.node)
			                        : std::nullopt;
			if (!type) continue;
			layout.inline_type   = *type;
			layout.inline_offset = *offset;
			layout.inline_root   = inline_root;
			return true;
		}
		return false;
	};
	const auto* sum = Producer(root->low, ValueOpcode::IAdd32);
	if (const auto* phi = Producer(root->low, ValueOpcode::Phi); phi != nullptr && inline_root != NoField) {
		for (uint32_t k = 0; k < 2 && sum == nullptr; ++k) {
			if (inline_phi(phi, k)) sum = Producer(phi->Arg(1 - k), ValueOpcode::IAdd32);
		}
	} else if (sum != nullptr && inline_root != NoField) {
		for (const auto& use: sum->Uses()) {
			if (use.user->GetOpcode() != ValueOpcode::Phi || !inline_phi(use.user, 1 - use.operand)) continue;
			for (const auto& address: use.user->Uses()) {
				if (address.user->GetOpcode() == ValueOpcode::GetAddressResource && address.operand == 0) {
					record_pair.low  = Value(use.user);
					record_pair.high = address.user->Arg(1).Resolve();
				}
			}
			break;
		}
	}
	if (sum == nullptr) return false;
	w.record   = record_pair;
	bool found = false;
	for (uint32_t side = 0; side < 2 && !found; ++side) {
		const auto* part = Producer(sum->Arg(side), ValueOpcode::CompositeExtractU64);
		const auto* scale =
		    part != nullptr ? Producer(part->Arg(0), ValueOpcode::ShiftLeftLogical64) : nullptr;
		const auto* wide = scale != nullptr
		                       ? Producer(scale->Arg(0), ValueOpcode::CompositeConstructU64)
		                       : nullptr;
		const auto* index =
		    wide != nullptr ? Producer(wide->Arg(0), ValueOpcode::ShiftRightLogical32) : nullptr;
		const auto bits   = scale != nullptr ? Immediate(scale->Arg(1)) : std::nullopt;
		uint32_t   stride = 0;
		if (index != nullptr && Immediate(part->Arg(1)) == 0u && Immediate(wide->Arg(1)) == 0u &&
		    Immediate(index->Arg(1)) == 3u && bits && *bits >= 7u && *bits <= 8u &&
		    LoopPhi(index->Arg(0), w.header) == w.node) {
			stride = 1u << *bits;
		} else if (const auto* mul = Producer(sum->Arg(side), ValueOpcode::IMul32); mul != nullptr) {
			for (uint32_t m = 0; m < 2; ++m) {
				const auto  scaled = Immediate(mul->Arg(m));
				const auto* by     = Producer(mul->Arg(1 - m), ValueOpcode::ShiftRightLogical32);
				if (scaled && by != nullptr && Immediate(by->Arg(1)) == 3u && LoopPhi(by->Arg(0), w.header) == w.node &&
				    *scaled % 32u == 0 && *scaled >= 128u && *scaled <= 224u)
					stride = *scaled;
			}
		}
		if (stride == 0) continue;
		const auto* base = Producer(sum->Arg(1 - side), ValueOpcode::IAdd32);
		if (base == nullptr) return false;
		for (uint32_t b = 0; b < 2 && !found; ++b) {
			const auto offset = LoadOf(program, base->Arg(b));
			if (offset && base->Arg(1 - b).Resolve() == w.tlas[0] && offset->low == w.tlas[0] &&
			    offset->high == w.tlas[1]) {
				layout.instance_offset = offset->offset;
				layout.record_stride   = stride;
				found                  = true;
			}
		}
	}
	if (!found || (root->offset & 3u) != 0) return false;
	const auto record = [&](Value value) -> std::optional<uint32_t> {
		const auto load = LoadOf(program, value);
		if (!load || !((load->low == root->low && load->high == root->high) ||
		               (load->low == record_pair.low && load->high == record_pair.high)) ||
		    (load->offset & 3u) != 0) {
			return std::nullopt;
		}
		return load->offset / 4u;
	};
	layout.root_dword = root->offset / 4u;
	// The world to object matrix: the BLAS box test's origin and direction rows.
	std::array<Value, 6> world {};
	for (uint32_t k = 0; k < 6; ++k)
		world[k] = Term(w.ray[k]);
	for (uint32_t row = 0; row < 3; ++row) {
		LinearForm origin, direction;
		origin.fill(NoDword);
		direction.fill(NoDword);
		if (!Expand(intersect->Arg(2 + row), world, record, origin) ||
		    !Expand(intersect->Arg(5 + row), world, record, direction)) {
			return false;
		}
		for (uint32_t c = 0; c < 3; ++c) {
			if (origin[c] == NoDword || origin[4 + c] != NoDword || direction[4 + c] != origin[c] ||
			    direction[c] != NoDword) {
				return false;
			}
			layout.transform[row * 4 + c] = static_cast<uint8_t>(origin[c]);
		}
		if (origin[3] == NoDword || direction[3] != NoDword) return false;
		layout.transform[row * 4 + 3] = static_cast<uint8_t>(origin[3]);
	}
	// The BLAS: its descriptor's base address, packed or not.
	const Inst* blas_bvh = nullptr;
	for (const auto& use: intersect->Uses()) {
		if (use.user->GetOpcode() == ValueOpcode::BvhIntersect) blas_bvh = use.user;
	}
	const auto* words = blas_bvh != nullptr
	                        ? Producer(blas_bvh->Arg(0), ValueOpcode::CompositeConstructU32x4)
	                        : nullptr;
	const auto* base  = words != nullptr ? DescriptorAddress(words->Arg(0)) : nullptr;
	if (base == nullptr) return false;
	const auto* pair = base;
	if (base->GetOpcode() == ValueOpcode::ShiftLeftLogical64) {
		if (Immediate(base->Arg(1)) != 3u) return false;
		pair               = base->Arg(0).Resolve().TryInstruction();
		layout.blas_packed = true;
	}
	if (pair == nullptr || pair->GetOpcode() != ValueOpcode::CompositeConstructU64) return false;
	const auto low  = record(pair->Arg(0));
	auto       high = pair->Arg(1);
	if (layout.blas_packed) {
		const auto* masked = Producer(high, ValueOpcode::BitwiseAnd32);
		if (masked == nullptr) return false;
		high = Immediate(masked->Arg(1)) == 0x3fffffu ? masked->Arg(0) : masked->Arg(1);
	}
	if (!low || record(high) != *low + 1u) return false;
	layout.blas_dword = *low;
	// The node count: the T# size of the BLAS header at +88, or its count at +80 (less one, as a
	// 64-bit value of which the T# takes 42 bits: sce::Psr version 1).
	Value size = words->Arg(2);
	if (const auto* part = Producer(size, ValueOpcode::CompositeExtractU64);
	    part != nullptr && Immediate(part->Arg(1)) == 0u) {
		const auto* masked = Producer(part->Arg(0), ValueOpcode::BitwiseAnd64);
		const auto* wide =
		    masked != nullptr ? Producer(masked->Arg(0), ValueOpcode::CompositeConstructU64) : nullptr;
		if (wide != nullptr) size = wide->Arg(0);
	}
	for (uint32_t depth = 0; depth < 4; ++depth) {
		const auto* add = Producer(size, ValueOpcode::IAdd32);
		if (add == nullptr) break;
		size = Immediate(add->Arg(0)) ? add->Arg(1) : add->Arg(0);
	}
	const auto count = LoadOf(program, size);
	if (!count || (count->offset != 80u && count->offset != 88u)) return false;
	layout.node_count_80 = count->offset == 80u;
	// The instance mask: the bits from `shift` up of a record dword against a ray mask the program
	// computes before the walk, (BitFieldUExtract(dword, shift, 8) or dword >> shift) & mask != 0
	// (GTA V's Performance RT, cs 48d704640ab9fb40 and 30c93330da43fd54); the record bits the
	// program then tests against constants are the instance flags.
	bool masked  = false;
	w.mask_value = {};
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			const auto dword = inst.GetOpcode() == ValueOpcode::LoadAddressU32 ? record(Value(&inst)) : std::nullopt;
			if (!dword || *dword == layout.blas_dword || *dword == layout.blas_dword + 1u) continue;
			for (const auto& use: inst.Uses()) {
				const auto* test  = use.user;
				const auto  shift = use.operand == 0 && ((test->GetOpcode() == ValueOpcode::BitFieldUExtract &&
				                                          Immediate(test->Arg(2)) == 8u) ||
				                                         test->GetOpcode() == ValueOpcode::ShiftRightLogical32)
				                        ? Immediate(test->Arg(1))
				                        : std::nullopt;
				for (const auto& u: test->Uses()) {
					const auto* both  = u.user;
					const auto  value = both->GetOpcode() == ValueOpcode::BitwiseAnd32 ? both->Arg(1 - u.operand) : Value {};
					if (!shift || *shift >= 32u || value.IsEmpty() || value.IsImmediate() ||
					    !DefinedBefore(program, value, w.header) || std::ranges::none_of(both->Uses(), [](const Use& t) {
						    return t.user->GetOpcode() == ValueOpcode::INotEqual32 && Immediate(t.user->Arg(1 - t.operand)) == 0u;
					    })) {
						continue;
					}
					if (masked && (layout.mask_dword != *dword || layout.mask_shift != *shift || w.mask_value != value.Resolve()))
						return false;
					layout.mask_dword = *dword;
					layout.mask_shift = *shift;
					layout.mask_width = 0;
					w.cull_mask       = 0xffu;
					w.mask_value      = value.Resolve();
					masked            = true;
				}
			}
		}
	}
	// Or a record dword & constant bits, tested against 0.
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			const auto dword = inst.GetOpcode() == ValueOpcode::LoadAddressU32
			                       ? record(Value(&inst))
			                       : std::nullopt;
			// (The BLAS address's high dword holds flags of the BLAS, not the instance mask.)
			if (!dword || *dword == layout.blas_dword || *dword == layout.blas_dword + 1u || masked) continue;
			for (const auto& use: inst.Uses()) {
				const auto* test = use.user;
				auto        bits = test->GetOpcode() == ValueOpcode::BitwiseAnd32
				                       ? Immediate(test->Arg(1 - use.operand))
				                       : std::nullopt;
				// Or one bit of it: BitFieldUExtract(dword, k, 1) == 1 (sce::Psr version 1).
				bool bit = false;
				if (test->GetOpcode() == ValueOpcode::BitFieldUExtract && use.operand == 0 &&
				    Immediate(test->Arg(2)) == 1u && Immediate(test->Arg(1)).value_or(32u) < 32u) {
					bits = 1u << *Immediate(test->Arg(1));
					bit  = true;
				}
				// Or any bit from `shift` up: dword > (1 << shift) - 1, or dword <= it (ASTRO BOT,
				// cs bdd0bb9bd8852dd4: dword 15 > 0xffffff).
				bool above = false;
				if ((test->GetOpcode() == ValueOpcode::UGreaterThan32 ||
				     test->GetOpcode() == ValueOpcode::ULessThanEqual32) &&
				    use.operand == 0) {
					const auto below = Immediate(test->Arg(1));
					if (below && *below != 0u && *below != 0xffffffffu && (*below & (*below + 1u)) == 0) {
						bits  = ~*below;
						above = true;
					}
				}
				// (A test in the leaf code of a BLAS the wave walks is one of the instance flags: GTA V
				// 01.005's Fidelity, cs a31ec6a25be5e017, tests bit 2 of the mask's dword there.)
				const auto in_leaf = [&](const Inst* user) {
					return w.wave_header != nullptr && InLoop(program, user->Parent(), w.wave_header, w.wave_latch);
				};
				if (!bits || *bits == 0u ||
				    (!above && std::ranges::none_of(test->Uses(), [&](const Use& u) {
					     const auto other = Immediate(u.user->Arg(1 - u.operand));
					     return ((u.user->GetOpcode() == ValueOpcode::INotEqual32 && other == 0u) ||
					             (bit && u.user->GetOpcode() == ValueOpcode::IEqual32 && other == 1u)) &&
					            !in_leaf(u.user);
				     }))) {
					continue;
				}
				// (Its bits need not be contiguous: Marvel's Wolverine, cs 1a04c58a9fbc2052, tests
				// 0x68000000; the host's instance mask is the 8 bits from the lowest. A contiguous
				// mask of more than 8 bits is `width` bits any of which counts, with mask 0xff.)
				const auto shift = static_cast<uint32_t>(std::countr_zero(*bits));
				auto       mask  = *bits >> shift;
				uint32_t   width = 0;
				if (mask > 0xffu && (mask & (mask + 1u)) == 0) {
					width = static_cast<uint32_t>(std::popcount(mask));
					mask  = 0xffu;
				}
				if ((masked && (layout.mask_dword != *dword || layout.mask_shift != shift ||
				                layout.mask_width != width || w.cull_mask != mask)) ||
				    mask > 0xffu) {
					return false;
				}
				layout.mask_dword = *dword;
				layout.mask_shift = shift;
				layout.mask_width = width;
				w.cull_mask       = mask;
				masked            = true;
			}
		}
	}
	w.masked = masked;
	if (!masked) {
		if (!w.mask_optional) return false;
		layout.mask_dword = NoField;
		layout.mask_shift = 0;
		layout.mask_width = 0;
	}
	layout.valid = true;
	return true;
}

// Where the TLAS header address comes from: a user data pointer + offset, or the base of a V# at
// a user data pointer + offset, + offset.
bool DeriveHeaderSource(const Program& program, LaneMaskWalk& w) {
	auto&       source = w.source;
	const auto  value  = Canonical(w.tlas[0]);
	const auto* read   = Producer(value, ValueOpcode::ReadConstBuffer);
	if (read != nullptr) {
		const auto* buffer = Producer(read->Arg(0), ValueOpcode::GetBufferResource);
		const auto  base   = buffer != nullptr ? LoadOf(program, buffer->Arg(0)) : std::nullopt;
		const auto  user   = base ? UserRegister(base->low) : std::nullopt;
		if (!user || UserRegister(base->high) != *user + 1u) return false;
		source.user_sgpr     = *user;
		source.header_vsharp = base->offset;
		source.header_offset = MemoryOf(program, *read).offset;
		return true;
	}
	const auto load = LoadOf(program, value);
	const auto user = load ? UserRegister(load->low) : std::nullopt;
	if (!user || UserRegister(load->high) != *user + 1u) return false;
	source.user_sgpr     = *user;
	source.header_offset = load->offset;
	return true;
}

// A per lane BLAS walk under the TLAS walk `tlas`: the ray query pass's recognizer of nested walks
// (MatchNested), which takes it whether the walk keeps the closest hit or takes any hit.
bool NestedBlasWalk(const Program& program, Inst& bvh, Inst& tlas, BlasWalk& b) {
	Traversal  nested;
	const auto walks = std::array {&tlas};
	if (!MatchNested(program, bvh, walks, nested, false) || nested.walk != &tlas) return false;
	b = {.preheader = nested.preheader,
	     .header    = nested.header,
	     .latch     = nested.latch,
	     .node      = nested.state[0],
	     .word      = nested.state[1],
	     .load      = nested.record,
	     .vote      = nested.vote,
	     .lanes     = nested.lanes};
	return true;
}

bool MatchLaneMaskWalk(Program& program, Inst& bvh, LaneMaskWalk& w) {
	const auto* descriptor = Producer(bvh.Arg(0), ValueOpcode::CompositeConstructU32x4);
	const auto* address    = Producer(bvh.Arg(1), ValueOpcode::MakeImageAddress);
	const auto* tlas = descriptor != nullptr ? DescriptorAddress(descriptor->Arg(0)) : nullptr;
	if (address == nullptr || tlas == nullptr ||
	    tlas->GetOpcode() != ValueOpcode::CompositeConstructU64)
		return false;
	w.tlas   = {tlas->Arg(0).Resolve(), tlas->Arg(1).Resolve()};
	w.bvh    = &bvh;
	w.header = EnclosingLoop(program, bvh.Parent());
	if (!LoopShape(w.header, w.preheader, w.latch, w.merge)) return false;
	const auto entry = [&](const Inst& phi) { return phi.Arg(Incoming(phi, w.preheader)); };
	const auto back  = [&](const Inst& phi) { return phi.Arg(Incoming(phi, w.latch)); };
	// The node starts at the root and takes a value popped from the stack.
	w.node          = LoopPhi(Canonical(address->Arg(0)), w.header);
	const auto root = w.node != nullptr ? Immediate(entry(*w.node)) : std::nullopt;
	if (!root) return false;
	const auto popped = [](Value value) -> const Inst* {
		const auto* phi = Producer(value, ValueOpcode::Phi);
		if (phi == nullptr) return nullptr;
		for (size_t arg = 0; arg < phi->NumArgs(); ++arg) {
			if (Producer(phi->Arg(arg), ValueOpcode::ReadLane) == nullptr) return nullptr;
		}
		return phi;
	};
	const auto* pop = popped(back(*w.node));
	if (pop == nullptr) return false;
	for (auto& inst: *w.header) {
		const auto* other =
		    inst.GetOpcode() == ValueOpcode::Phi && &inst != w.node ? popped(back(inst)) : nullptr;
		if (other == nullptr || other->Parent() != pop->Parent()) continue;
		if (w.pair_mask != nullptr) return false; // Waves of 64 lanes: masks in two words.
		w.pair_mask = &inst;
	}
	// The pair's lanes start as the lanes that trace: a ballot of them, or of some lanes without
	// the lanes of another (Marvel's Wolverine, cs 1a04c58a9fbc2052: S_ANDN2 of EXEC).
	if (w.pair_mask == nullptr) return false;
	const auto  initial = entry(*w.pair_mask);
	const auto* both    = Producer(initial, ValueOpcode::BitwiseAnd32);
	const auto* minus   = both ? Producer(both->Arg(1), ValueOpcode::BitwiseNot32) : nullptr;
	const auto* ballot  = BallotOf(minus != nullptr ? both->Arg(0) : initial);
	const auto* without = minus != nullptr ? BallotOf(minus->Arg(0)) : nullptr;
	if (ballot == nullptr || (minus != nullptr && without == nullptr)) return false;
	w.lanes         = ballot->Arg(0).Resolve();
	w.lanes_without = without != nullptr ? without->Arg(0).Resolve() : Value {};

	const auto first = BlockIndex(program, w.header), last = BlockIndex(program, w.latch);
	// One exit, before the pop.
	for (auto at = first; at <= last; ++at) {
		auto* block = program.blocks[at];
		if (std::ranges::find(block->ImmSuccessors(), w.merge) == block->ImmSuccessors().end())
			continue;
		if (w.exit != nullptr) return false;
		w.exit = block;
	}
	w.exit_ref =
	    w.exit != nullptr ? Producer(w.exit->condition, ValueOpcode::ConditionRef) : nullptr;
	if (w.exit_ref == nullptr || BlockIndex(program, w.exit) >= BlockIndex(program, pop->Parent()))
		return false;
	for (uint32_t arg = 1; arg < 8; ++arg) {
		w.ray[(arg + 5) % 7] = address->Arg(arg);
	}
	// The BLAS walks: every other intersection of the loop.
	for (auto at = first; at <= last; ++at) {
		for (auto& inst: *program.blocks[at]) {
			if (inst.GetOpcode() != ValueOpcode::BvhIntersect || &inst == &bvh) continue;
			BlasWalk blas;
			if (!NestedBlasWalk(program, inst, bvh, blas)) return false;
			w.blases.push_back(std::move(blas));
		}
	}
	if (w.blases.empty() || !DeriveLaneLayout(program, w) || !DeriveHeaderSource(program, w))
		return false;
	w.source.root   = *root;
	w.source.layout = w.layout.Pack();
	// The guest code that stays in the BVH: the loop's.
	uint32_t begin = UINT32_MAX;
	for (auto at = first; at <= last; ++at) {
		const auto* block = program.blocks[at];
		if (block->end_pc > block->start_pc) begin = std::min(begin, block->start_pc);
	}
	if (begin == UINT32_MAX || w.merge->start_pc <= begin) return false;
	w.source.reach        = {{begin, w.merge->start_pc}};
	w.source.source_valid = true;
	return true;
}

// Each BLAS walk starts at the lane's leaf list entry (its own of the selected instance) and
// ends after its leaf code.
void RewriteBlasWalks(Program& program, std::vector<BlasWalk>& blases, Value any, uint32_t lane_leaf) {
	using Op = ValueOpcode;
	for (auto& b: blases) {
		const Inserter pre {b.preheader, b.preheader->end()};
		const auto     lanes = b.lanes.front()->Arg(Incoming(*b.lanes.front(), b.preheader));
		const auto     index =
		    pre(Op::ShiftRightLogical32, {pre(Op::RayQuerySelectedLeaf, {}, lane_leaf), Value(3u)});
		const auto& guest = *Producer(b.load->Arg(0), ValueOpcode::GetBufferResource);
		const auto  buffer =
		    pre(Op::GetBufferResource, {guest.Arg(0), guest.Arg(1), guest.Arg(2), guest.Arg(3)},
		        guest.Flags<uint64_t>());
		auto memory = b.load->Flags<MemoryFlags>();
		program.memory_info.push_back(program.memory_info[memory.index]);
		memory.index      = static_cast<uint32_t>(program.memory_info.size() - 1);
		const auto loaded = pre(Op::LoadBufferU32x2, {buffer, index, b.load->Arg(2), b.load->Arg(3),
		                                              pre(Op::LogicalAnd, {lanes, any})});
		loaded.Instruction()->SetFlags(memory);
		for (auto [phi, word]: {std::pair {b.node, 0u}, std::pair {b.word, 1u}}) {
			const auto arg = Incoming(*phi, b.preheader);
			phi->SetArg(
			    arg, pre(Op::SelectU32, {any, pre(Op::CompositeExtractU32x2, {loaded, Value(word)}),
			                             phi->Arg(arg)}));
		}
		const Inserter vote {b.vote->Parent(), Position(*b.vote)};
		const auto     alive = vote(Op::SelectU1, {any, Value(false), b.vote->Arg(0)});
		b.vote->SetArg(0, alive);
		for (auto* lane: b.lanes) {
			lane->SetArg(Incoming(*lane, b.latch), alive);
		}
	}
}

void RewriteLaneMaskWalk(Program& program, LaneMaskWalk& w, uint32_t site) {
	using Op         = ValueOpcode;
	const auto flags = [site](auto variant) {
		return RayQueryFlags(site, static_cast<uint32_t>(variant));
	};
	const auto lane_leaf = flags(RayQueryKeyLaneLeaf);

	// The preheader starts the queries (every triangle a candidate of the leaf code, both facings).
	const Inserter entry {w.preheader, w.preheader->end()};
	if (!w.lanes_without.IsEmpty()) {
		w.lanes = entry(Op::LogicalAnd, {w.lanes, entry(Op::LogicalNot, {w.lanes_without})});
	}
	std::array<Value, 7> ray {};
	for (uint32_t k = 0; k < 7; ++k)
		ray[k] = *Hoist(program, w.ray[k], w.header, entry);
	const auto native =
	    entry(Op::RayQueryInit,
	          {ray[0], ray[1], ray[2], ray[3], ray[4], ray[5], ray[6], RayMask(w),
	           w.tlas[0], w.tlas[1], Value(0u), w.lanes, Value(true), Value(false), Value(true)},
	          flags(RayQueryInitBothFacings | RayQueryInitAllCandidates));
	const auto ballot = entry(Op::Ballot, {native});
	const auto any =
	    entry(Op::INotEqual32,
	          {entry(Op::BitwiseOr32, {entry(Op::CompositeExtractU32x4, {ballot, Value(0u)}),
	                                   entry(Op::CompositeExtractU32x4, {ballot, Value(1u)})}),
	           Value(0u)});
	const auto first = Incoming(*w.pair_mask, w.preheader);
	w.pair_mask->SetArg(first, entry(Op::SelectU32, {any, Value(0u), w.pair_mask->Arg(first)}));

	// Before the pop: the lanes of the last pair are done with their key; the next one, or the
	// exit.
	const Inserter at {w.exit, Position(*w.exit_ref)};
	at(Op::RayQueryMark, {}, flags(RayQueryMarkKind::Resolve));
	const auto next  = at(Op::RayQueryNext, {}, lane_leaf);
	const auto match = at(Op::RayQueryMatch, {}, lane_leaf);
	const auto has =
	    at(Op::INotEqual32, {at(Op::CompositeExtractU32x4, {next, Value(2u)}), Value(0u)});
	const auto instance = at(Op::CompositeExtractU32x4, {next, Value(0u)});
	const auto mask     = at(Op::CompositeExtractU32x4, {at(Op::Ballot, {match}), Value(0u)});
	const auto done     = at(Op::LogicalAnd, {w.lanes, at(Op::LogicalNot, {has})});
	w.exit_ref->SetArg(0, at(Op::SelectU1, {any, done, w.exit_ref->Arg(0)}));
	const Inserter back {w.latch, AfterPhis(*w.latch)};
	for (auto [phi, value]: {std::pair {w.node, instance}, std::pair {w.pair_mask, mask}}) {
		const auto index = Incoming(*phi, w.latch);
		phi->SetArg(index, back(Op::SelectU32, {any, value, phi->Arg(index)}));
	}
	const Inserter leave {w.merge, AfterPhis(*w.merge)};
	leave(Op::RayQueryMark, {}, flags(RayQueryMarkKind::Exit));

	RewriteBlasWalks(program, w.blases, any, lane_leaf);
}

// The per lane alpha test of GTA V's leaf code (RayQueryAlpha with the Lane layout, see
// DeriveLaneAlpha): its level of detail term, cone spread and reference (values from before the
// walk), the V#s of the material table and of the buffer table, the level's offset (float bits)
// and the format of the guest data.
struct LaneAlpha {
	bool                           valid = false;
	Value                          lod;
	Value                          spread;
	Value                          reference;
	std::array<Value, 4>           material {};
	std::array<Value, 4>           buffers {};
	uint32_t                       lod_offset = 0;
	RayTracingAlphaFields fields;
	const Inst*                    flag = nullptr; // Select(accepted, 1, 0): the test's result.
	// The reference's factor (threshold * (M0 bit 0 ? factor : 1); empty: none) and the wave-wide value
	// without which the leaf code takes another level of detail (empty: none).
	Value factor;
	Value level;
	// The leaf code of a wave-wide BLAS walk, which reads the guest data with scalar loads
	// (RayQueryAlphaScalar), and whether its reference is threshold * factor for every material.
	bool scalar        = false;
	bool factor_always = false;
};

// ---- Packet walks (sce::Psr version 1): Resident Evil Village, Marvel's Wolverine, GTA V. ----
//
// A TLAS walk whose node tests run for the lanes of a packet (its node, instance and BLAS the same
// in every lane) in two nested loops (the guest walk has two backward branches: the inner one goes
// down the boxes, the outer one after a pop of the packet's stack), with a per lane BLAS walk in
// its instance code. Three kinds of them run natively:
// - Entry (Resident Evil Village, cs be0b848d2e06ec9b, f969c192323c215b): the BLAS walk's leaf
//   code is the library's alpha test template (the machine code of the BLAS walk is that of the
//   template: EntryTemplateHash). The host traverses and alpha tests (RayQueryAlpha with the Entry
//   layout) before the walk; the walk then takes the shortest way to its exit (its conditions on
//   that way fixed) and the values the code after it reads of an accepted hit (the primitive
//   word, the index of the leaf list entry after the hit's and the instance record address) are
//   the host's hit, when nearer than the walk's t max.
// - ClosestLane (Marvel's Wolverine, cs 1a04c58a9fbc2052): the BLAS walk runs the guest's own leaf
//   code, which keeps the nearest hit itself, for every lane of the trace (no lane leaves the
//   walk). Each round of the outer loop takes the smallest instance a lane has a candidate leaf
//   of: the round's node is that instance, its packet the lanes with a leaf of it, and each lane's
//   BLAS walk runs once, from its own smallest leaf; the pop at the round's end is the next
//   instance, or the exit.
// - ClosestOpaque (Grand Theft Auto V, cs 84ed00053b3771da): the BLAS walk's leaf code keeps the
//   nearest triangle, every one opaque, or with the per lane alpha test of the triangles not
//   marked opaque (cs 6aeb221de7ff018e); as Entry without the template (MatchClosestOpaqueWalk).
// Entry and ClosestOpaque take the BLAS walk the ray query pass recognizes as nested under the TLAS
// walk (MatchNested, before resource tracking), and PacketWave the BLAS walk it recognizes on the
// TLAS walk's stack (MatchWalk).
struct PacketWalk {
	Block*              preheader = nullptr;
	Block*              header    = nullptr;
	Block*              latch     = nullptr;
	Block*              merge     = nullptr;
	std::vector<Block*> path; // The header to the exit's block.
	LaneMaskWalk        shape; // The inner loop's node, the BLAS walk.
	Value               lanes;
	Value               t_max;
	// Entry: the alpha test's tables, and the values read of an accepted hit.
	std::array<Value, 4>                              entries {};
	std::array<Value, 4>                              buffers {};
	std::vector<std::pair<Inst*, RayQueryResultKind>> results;
	// ClosestLane: the outer loop's node (the root, then a node popped from the stack) and the
	// packet (the inner loop's lanes: every lane of the trace when a round starts). ClosestLeaf: the
	// TLAS node and the walk's lanes (a phi of its header that starts at the lanes of the trace).
	Inst* outer_node = nullptr;
	Inst* packet     = nullptr;
	// The queries' facing: both, or that of the instance flags for the lanes `cull` (ClosestWave).
	bool  both_facings = true;
	Value cull {true};
	// ClosestOpaque and PacketLane: the per lane alpha test of the leaf code (none: every triangle
	// opaque); PacketWave: its scalar alpha test.
	LaneAlpha alpha;
	// PacketLane and PacketWave: any hit (no guest leaf code runs), the value that switches the alpha
	// test off (the instance opacity) and the condition under which the lanes cull the other facing.
	bool  any_hit = false;
	Value alpha_off;
	Value flip;
	// Whether the ray query pass's recognizer of walks over a stack the wave shares (MatchWalk) took
	// the wave-wide BLAS walk.
	bool walked = false;
	// ClosestLeaf: the leaf code of a ClosestWave walk accepts with a condition of its own (Gran Turismo 7's
	// any hit functions), so that it decides the triangles that are not opaque; the branches that leave
	// the loop over the leaf list from a block whose other way is its latch (and whether they leave when
	// true), and the distance of the nearest hit the leaf code keeps at a round's end.
	bool                                 leaf = false;
	std::vector<std::pair<Block*, bool>> list_exits;
	Value                                leaf_t;
};

// The machine code of the template's BLAS walk (FNV-1a of its instruction words).
constexpr uint64_t EntryTemplateHash = 0x777c0050ecd1ca9eull;

// Where `value`, a value of the walk, comes from on the first iteration along the path: the
// incoming values of the phis of the path's blocks, from the block before them (the preheader for
// the header).
Value PathEntry(const PacketWalk& w, Value value) {
	for (uint32_t depth = 0; depth < 64; ++depth) {
		value            = value.Resolve();
		const auto* phi  = Producer(value, ValueOpcode::Phi);
		const auto  at   = phi != nullptr ? std::ranges::find(w.path, phi->Parent()) : w.path.end();
		if (at == w.path.end()) break;
		const auto* from = at == w.path.begin() ? w.preheader : *(at - 1);
		const auto  arg  = Incoming(*phi, from);
		if (arg == phi->NumArgs()) return {};
		value = phi->Arg(arg);
	}
	return value;
}

// The guest code of a walk's loop [header, latch] (the code that stays in the BVH), up to `end`.
bool WalkReach(const Program& program, const Block* header, const Block* latch, uint32_t end,
               NativeTraversals& source) {
	uint32_t begin = UINT32_MAX;
	for (auto at = BlockIndex(program, header); at <= BlockIndex(program, latch); ++at) {
		const auto* block = program.blocks[at];
		if (block->end_pc > block->start_pc) begin = std::min(begin, block->start_pc);
	}
	if (begin == UINT32_MAX || end <= begin) return false;
	source.reach        = {{begin, end}};
	source.source_valid = true;
	return true;
}

// A BLAS the wave walks (scalar nodes: ASTRO BOT, cs bdd0bb9bd8852dd4): two intersections in the
// node test's loop with one descriptor, the box test `tests[box]`, whose node starts (through the
// phis of the BLAS walk's loops) at the root, and the triangle test of the leaf list.
// The value a wave-wide BLAS walk's box test node starts at (the root) in the outermost loop
// [header, latch] of the walk, entered from `pre`; empty if none.
Value WaveRoot(const Program& program, const Inst* address, const Block* header, const Block* pre,
               const Block* latch, Inst** phi = nullptr) {
	// The node: a phi of its header.
	std::vector<Value> nodes {Canonical(address->Arg(0))};
	const Inst*        node = nullptr;
	for (size_t at = 0; at < nodes.size() && at < 16 && node == nullptr; ++at) {
		const auto* phi = Producer(nodes[at], ValueOpcode::Phi);
		if (phi == nullptr || !InLoop(program, phi->Parent(), header, latch)) continue;
		if (phi->Parent() == header) {
			node = phi;
			break;
		}
		for (size_t arg = 0; arg < phi->NumArgs(); ++arg)
			nodes.push_back(Canonical(phi->Arg(arg)));
	}
	// (Or the top of the walk's stack of nodes in lanes, which the code before the walk pushes the root
	// on (GTA V's Fidelity, cs 0b41af19bddf132c): ReadLane(stack, (sp - 1) & 63) of phis of the header
	// that start at WriteLane(_, root, e & 63) and e + 1.)
	Value root;
	if (node == nullptr) {
		const auto* top   = Producer(nodes[0], ValueOpcode::ReadLane);
		const auto* stack = top != nullptr ? LoopPhi(top->Arg(0), header) : nullptr;
		const auto* push  = stack != nullptr ? Producer(stack->Arg(Incoming(*stack, pre)), ValueOpcode::WriteLane) : nullptr;
		const auto  low6  = [](Value value) {
			const auto pair = Operands(value, ValueOpcode::BitwiseAnd32, [](Value side) { return Immediate(side) == 63u; });
			return pair ? Canonical(pair->second) : Value {};
		};
		const auto  index = top != nullptr ? low6(top->Arg(1)) : Value {};
		const auto  below = !index.IsEmpty() ? Operands(index, ValueOpcode::IAdd32, [](Value side) { return Immediate(side) == ~0u; })
		                                     : std::nullopt;
		const auto* sp    = below ? LoopPhi(below->second, header) : nullptr;
		const auto  above = sp != nullptr ? Operands(sp->Arg(Incoming(*sp, pre)), ValueOpcode::IAdd32,
		                                             [](Value side) { return Immediate(side) == 1u; })
		                                  : std::nullopt;
		if (push == nullptr || !above || low6(push->Arg(2)).IsEmpty() || low6(push->Arg(2)) != Canonical(above->second))
			return {};
		root = push->Arg(1).Resolve();
	}
	if (phi != nullptr) *phi = const_cast<Inst*>(node);
	return node != nullptr ? node->Arg(Incoming(*node, pre)) : root;
}

bool MatchWaveBlas(const Program& program, std::span<Inst* const> tests, uint32_t box, LaneMaskWalk& s) {
	const auto* a = Producer(tests[0]->Arg(0), ValueOpcode::CompositeConstructU32x4);
	const auto* b = Producer(tests[1]->Arg(0), ValueOpcode::CompositeConstructU32x4);
	if (a == nullptr || b == nullptr) return false;
	for (size_t k = 0; k < 4; ++k) {
		if (a->Arg(k).Resolve() != b->Arg(k).Resolve()) return false;
	}
	const auto* address = Producer(tests[box]->Arg(1), ValueOpcode::MakeImageAddress);
	Block*      header  = EnclosingLoop(program, tests[box]->Parent());
	Block *     pre = nullptr, *latch = nullptr, *merge = nullptr;
	// (The triangle test is in the box test's loop: its own loop over the leaf list is inside it.)
	if (address == nullptr || header == s.header || !LoopShape(header, pre, latch, merge) ||
	    !InLoop(program, header, s.header, s.latch) || !InLoop(program, tests[1 - box]->Parent(), header, latch))
		return false;
	// The outermost loop of the BLAS walk in the node test's loop.
	for (;;) {
		const auto entries = pre->ImmPredecessors();
		auto*      outer   = entries.size() == 1 ? entries[0] : nullptr;
		Block *    outer_pre = nullptr, *outer_latch = nullptr, *outer_merge = nullptr;
		if (outer == nullptr || outer == s.header || !outer->terminator.loop_header ||
		    !LoopShape(outer, outer_pre, outer_latch, outer_merge) || !InLoop(program, header, outer, outer_latch) ||
		    !InLoop(program, outer, s.header, s.latch))
			break;
		header = outer;
		pre    = outer_pre;
		latch  = outer_latch;
	}
	const auto root = WaveRoot(program, address, header, pre, latch, &s.wave_node);
	if (root.IsEmpty()) return false;
	s.wave_header    = header;
	s.wave_latch     = latch;
	s.wave_root      = root;
	s.wave_box       = tests[box];
	s.wave_triangles = tests[1 - box];
	return true;
}

bool MatchPacketWalk(Program& program, Inst& bvh, PacketWalk& w) {
	const auto* descriptor = Producer(bvh.Arg(0), ValueOpcode::CompositeConstructU32x4);
	const auto* address    = Producer(bvh.Arg(1), ValueOpcode::MakeImageAddress);
	const auto* tlas = descriptor != nullptr ? DescriptorAddress(descriptor->Arg(0)) : nullptr;
	if (address == nullptr || tlas == nullptr ||
	    tlas->GetOpcode() != ValueOpcode::CompositeConstructU64)
		return false;
	auto& s = w.shape;
	s.tlas  = {tlas->Arg(0).Resolve(), tlas->Arg(1).Resolve()};
	s.bvh   = &bvh;
	// The node test's loop, and the loops around it it is the only code of (the guest walk has
	// two backward branches): the outermost is the walk.
	s.header = EnclosingLoop(program, bvh.Parent());
	if (!LoopShape(s.header, s.preheader, s.latch, s.merge)) return false;
	w.header    = s.header;
	w.preheader = s.preheader;
	w.latch     = s.latch;
	w.merge     = s.merge;
	for (;;) {
		const auto entries = w.preheader->ImmPredecessors();
		auto*      outer   = entries.size() == 1 ? entries[0] : nullptr;
		Block *    pre = nullptr, *latch = nullptr, *merge = nullptr;
		if (outer == nullptr || !outer->terminator.loop_header ||
		    !LoopShape(outer, pre, latch, merge) || !InLoop(program, w.header, outer, latch))
			break;
		w.header    = outer;
		w.preheader = pre;
		w.latch     = latch;
		w.merge     = merge;
	}
	// The way from the header to the exit: the shortest to the TLAS node test's branch, then the
	// shortest from there past it, with no other BVH intersection and no backward edge.
	const auto first = BlockIndex(program, w.header), last = BlockIndex(program, w.latch);
	const auto in_walk = [&](const Block* block) {
		const auto at = BlockIndex(program, block);
		return at >= first && at <= last;
	};
	const auto intersects = [](const Block* block) {
		return std::ranges::any_of(*block, [](const Inst& inst) {
			return inst.GetOpcode() == ValueOpcode::BvhIntersect;
		});
	};
	// The blocks from `from` to the first block that `goal` accepts as a successor.
	const auto shortest = [&](Block* from, auto&& goal) {
		std::unordered_map<const Block*, Block*> parent {{from, nullptr}};
		std::vector<Block*>                      queue {from}, way;
		Block*                                   end = nullptr;
		for (size_t at = 0; at < queue.size() && end == nullptr; ++at) {
			auto* block = queue[at];
			for (auto* next: block->ImmSuccessors()) {
				if (goal(next)) {
					end = block;
					break;
				}
				if (!in_walk(next) || BlockIndex(program, next) <= BlockIndex(program, block) ||
				    intersects(next) || parent.contains(next))
					continue;
				parent.emplace(next, block);
				queue.push_back(next);
			}
		}
		for (auto* block = end; block != nullptr; block = parent.at(block))
			way.insert(way.begin(), block);
		return way;
	};
	const auto preds = bvh.Parent()->ImmPredecessors();
	if (preds.size() != 1) return false;
	w.path = shortest(w.header, [&](const Block* next) { return next == preds[0]; });
	if (w.path.empty()) return false;
	w.path.push_back(preds[0]);
	const auto rest = shortest(preds[0], [&](const Block* next) { return next == w.merge; });
	if (rest.empty()) return false;
	w.path.insert(w.path.end(), rest.begin() + 1, rest.end());
	for (size_t at = 0; at < w.path.size(); ++at) {
		const auto* block = w.path[at];
		const auto  kind  = block->terminator.kind;
		if (kind != CFG::TerminatorKind::Branch &&
		    (kind != CFG::TerminatorKind::ConditionalBranch || BranchCondition(*block).first == nullptr))
			return false;
	}
	// The ray: the world ray of the packet's lanes (their lane masked writes) and the t max of the
	// first iteration.
	for (uint32_t arg = 1; arg < 8; ++arg) {
		s.ray[(arg + 5) % 7] = address->Arg(arg);
	}
	// (A ray from before that the packet's lanes do not write, GTA V's reflections, cs
	// 6aeb221de7ff018e: the lanes of their writes of its inverse direction.)
	const auto* origin = Producer(s.ray[0], ValueOpcode::SelectU32);
	for (uint32_t arg = 8; arg < 11 && origin == nullptr; ++arg) {
		const auto* inverse = Producer(address->Arg(arg), ValueOpcode::SelectU32);
		if (inverse == nullptr || (arg > 8 && inverse->Arg(0).Resolve() != w.lanes)) return false;
		w.lanes = inverse->Arg(0).Resolve();
		if (arg == 10) origin = inverse;
	}
	w.lanes = origin->Arg(0).Resolve();
	w.t_max = PathEntry(w, s.ray[6]);
	if (!DefinedBefore(program, w.lanes, w.header) || !DefinedBefore(program, w.t_max, w.header) ||
	    std::ranges::any_of(std::span(s.ray).first(6),
	                        [&](Value v) { return !DefinedBefore(program, v, w.header); }))
		return false;
	// The node: a phi of the inner loop, which starts at the root.
	std::vector<Value> nodes {Canonical(address->Arg(0))};
	for (size_t at = 0; at < nodes.size() && at < 8 && s.node == nullptr; ++at) {
		auto* phi = Producer(nodes[at], ValueOpcode::Phi);
		if (phi == nullptr) continue;
		if (phi->Parent() == s.header) {
			s.node = phi;
			break;
		}
		for (size_t arg = 0; arg < phi->NumArgs(); ++arg)
			nodes.push_back(phi->Arg(arg));
	}
	const auto root = s.node != nullptr ? Immediate(PathEntry(w, Value(s.node))) : std::nullopt;
	if (!root) return false;
	// The BLAS walk: the loop's other intersection (a per lane walk), or two (a wave-wide walk).
	std::vector<Inst*> others;
	for (auto at = first; at <= last; ++at) {
		for (auto& inst: *program.blocks[at]) {
			if (inst.GetOpcode() == ValueOpcode::BvhIntersect && &inst != &bvh) others.push_back(&inst);
		}
	}
	s.mask_optional = true;
	if (others.size() == 1) {
		// A BLAS walk nested under this TLAS walk.
		BlasWalk blas;
		if (!NestedBlasWalk(program, *others[0], bvh, blas)) return false;
		s.blases.push_back(std::move(blas));
		if (!DeriveLaneLayout(program, s)) return false;
	} else if (others.size() == 2) {
		// A BLAS the wave walks off a stack it shares with this TLAS walk (MatchWalk: the triangle
		// test of its prim record loop), or one of the wave-wide walks of the forms it does not take
		// yet (ClosestWave: MatchWaveBlas).
		const auto tlas  = std::array {&bvh};
		bool       found = false;
		for (uint32_t box = 0; box < 2 && !found; ++box) {
			Walk shared;
			if (!MatchWalk(program, *others[1 - box], tlas, shared, false) || shared.walk != &bvh) continue;
			const auto* address = Producer(others[box]->Arg(1), ValueOpcode::MakeImageAddress);
			const auto  root    = address != nullptr ? WaveRoot(program, address, shared.blas.header,
			                                                    shared.blas.preheader, shared.blas.latch)
			                                         : Value {};
			if (root.IsEmpty() || EnclosingLoop(program, others[box]->Parent()) != shared.blas.header) break;
			s.wave_header    = shared.blas.header;
			s.wave_latch     = shared.blas.latch;
			s.wave_root      = root;
			s.wave_box       = others[box];
			s.wave_triangles = others[1 - box];
			found = w.walked = DeriveLaneLayout(program, s);
		}
		if (!found && !(MatchWaveBlas(program, others, 0, s) && DeriveLaneLayout(program, s)) &&
		    !(MatchWaveBlas(program, others, 1, s) && DeriveLaneLayout(program, s))) {
			return false;
		}
	} else {
		return false;
	}
	if (!DeriveHeaderSource(program, s)) return false;
	s.source.root   = *root;
	s.source.layout = s.layout.Pack();
	return WalkReach(program, w.header, w.latch, w.merge->start_pc, s.source);
}

// The values the code after a walk reads of its accepted hit (into w.results): values of the walk
// read after it whose sources in the walk (through phis and lane masked writes) are of one kind of
// `kind_of`. Without `hit` every source is of it. With `hit` (a value the leaf code writes under its
// accept condition: the Select's condition) only the sources the value takes through an accepted
// hit's write count: the code after the walk reads the others (other values the walk keeps in the
// same registers, or what they hold from before the walk) only where the walk found no hit. The
// kinds found; none when a value of a kind is read in a phi of the walk's merge (which would not see
// the host's hit).
template <typename KindOf>
std::optional<std::array<bool, 8>> HitResults(const Program& program, PacketWalk& w, KindOf&& kind_of,
                                              Value hit = {}, bool negated = false, bool select_kinds = false) {
	const auto first = BlockIndex(program, w.header), last = BlockIndex(program, w.latch);
	const auto in_walk = [&](const Block* block) {
		const auto at = BlockIndex(program, block);
		return at >= first && at <= last;
	};
	const bool          strict = hit.IsEmpty();
	std::array<bool, 8> found {};
	// The accept condition (with `select_kinds`, also another lane bit of its wave mask: waves of 32
	// lanes read a lane mask SGPR once per V_CNDMASK_B32).
	const auto hit_mask = select_kinds ? LaneMaskOf(hit.Resolve().TryInstruction()) : Value {};
	const auto same     = [&](Value condition) {
		condition = condition.Resolve();
		return condition == hit.Resolve() ||
		       (!hit_mask.IsEmpty() && LaneMaskOf(condition.TryInstruction()) == hit_mask);
	};
	for (auto at = first; at <= last; ++at) {
		for (auto& inst: *program.blocks[at]) {
			if (std::ranges::none_of(inst.Uses(), [&](const Use& use) { return !in_walk(use.user->Parent()); }))
				continue;
			std::unordered_set<const Inst*>          seen[2];
			std::vector<std::pair<const Inst*, bool>> pending {{&inst, strict}};
			std::optional<RayQueryResultKind>         kind;
			bool                                      mixed = false;
			while (!pending.empty() && !mixed) {
				const auto [value, accepted] = pending.back();
				pending.pop_back();
				if (!seen[accepted ? 1 : 0].insert(value).second) continue;
				const auto op = value->GetOpcode();
				// (A phi of the walk can be a source: the record address of inline records; and with
				// `select_kinds` a lane masked write, whose kept value is another lane's.)
				const auto phi_kind = accepted && (op == ValueOpcode::Phi || (select_kinds && op == ValueOpcode::SelectU32))
				                          ? kind_of(value)
				                          : std::nullopt;
				if (phi_kind) {
					mixed = kind && *kind != *phi_kind;
					kind  = phi_kind;
					continue;
				}
				if (op == ValueOpcode::Phi || op == ValueOpcode::SelectU32) {
					for (size_t arg = op == ValueOpcode::Phi ? 0 : 1; arg < (op == ValueOpcode::Phi ? value->NumArgs() : 3); ++arg) {
						const auto* source = value->Arg(arg).Resolve().TryInstruction();
						// (The written value: the Select's first with the accept condition, its second with
						// the reject condition: `negated`.)
						const bool  write  = op == ValueOpcode::SelectU32 && arg == (negated ? 2u : 1u) && same(value->Arg(0));
						if (source != nullptr && in_walk(source->Parent())) pending.emplace_back(source, accepted || write);
					}
					continue;
				}
				if (!accepted) continue;
				const auto source = kind_of(value);
				if (!strict && !source) continue;
				mixed = !source || (kind && *kind != *source);
				kind  = source;
			}
			if (mixed || !kind) continue;
			if (std::ranges::any_of(inst.Uses(), [&](const Use& use) {
				    return use.user->Parent() == w.merge && use.user->GetOpcode() == ValueOpcode::Phi;
			    }))
				return std::nullopt;
			w.results.emplace_back(&inst, *kind);
			found[static_cast<uint32_t>(*kind)] = true;
		}
	}
	return found;
}

bool MatchEntryWalk(Program& program, const NativeTraversalOptions& options, PacketWalk& w) {
	auto& s = w.shape;
	if (options.decoded == nullptr || !s.masked || s.blases.empty()) return false;
	const auto& blas = s.blases.front();
	// The template: the machine code of the BLAS walk.
	uint32_t begin = UINT32_MAX, end = 0;
	for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch); ++at) {
		const auto* block = program.blocks[at];
		if (block->end_pc > block->start_pc) {
			begin = std::min(begin, block->start_pc);
			end   = std::max(end, block->end_pc);
		}
	}
	uint64_t hash = 1469598103934665603ull;
	for (const auto& inst: options.decoded->instructions) {
		if (inst.pc < begin || inst.pc >= end) continue;
		for (uint32_t k = 0; k < inst.word_count && k < Decoder::MaxInstructionRawWords; ++k)
			hash = (hash ^ inst.raw[k]) * 1099511628211ull;
	}
	if (hash != EntryTemplateHash) return false;
	// The template's tables: the V#s of its first two constant buffers (the leaf list entries, then
	// the buffers), set up before the walk; the instance flags are record dword 15.
	std::vector<const Inst*> tables;
	for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch); ++at) {
		for (const auto& inst: *program.blocks[at]) {
			if (inst.GetOpcode() != ValueOpcode::ReadConstBuffer) continue;
			const auto* table = Producer(inst.Arg(0), ValueOpcode::GetBufferResource);
			if (table == nullptr) return false;
			const auto same = [&](const Inst* other) {
				for (size_t k = 0; k < 4; ++k) {
					if (other->Arg(k).Resolve() != table->Arg(k).Resolve()) return false;
				}
				return true;
			};
			if (std::ranges::none_of(tables, same)) tables.push_back(table);
		}
	}
	if (tables.size() < 2) return false;
	for (size_t k = 0; k < 4; ++k) {
		w.entries[k] = tables[0]->Arg(k).Resolve();
		w.buffers[k] = tables[1]->Arg(k).Resolve();
		if (!DefinedBefore(program, w.entries[k], w.header) ||
		    !DefinedBefore(program, w.buffers[k], w.header))
			return false;
	}
	s.layout.flags_dword = 15;
	s.layout.flags_shift = 0;
	s.source.layout      = s.layout.Pack();
	// What the code after the walk reads of an accepted hit: values whose every source in the walk
	// is the instance record address, the entry's primitive word or the index after the entry.
	const auto record = LoadOf(program, Canonical(blas.node->Arg(Incoming(*blas.node, blas.preheader))));
	if (!record) return false;
	const auto kind_of = [&](const Inst* value) -> std::optional<RayQueryResultKind> {
		if (value == record->low.TryInstruction()) return RayQueryResultKind::InstanceLo;
		if (value == record->high.TryInstruction()) return RayQueryResultKind::InstanceHi;
		if (value->GetOpcode() == ValueOpcode::CompositeExtractU32x2 && Immediate(value->Arg(1)) == 1u &&
		    value->Arg(0).Resolve().TryInstruction() == blas.load)
			return RayQueryResultKind::Word;
		if (value->GetOpcode() == ValueOpcode::IAdd32) {
			for (uint32_t side = 0; side < 2; ++side) {
				if (Immediate(value->Arg(side)) == 1u && value->Arg(1 - side).Resolve() == blas.load->Arg(1).Resolve())
					return RayQueryResultKind::Node;
			}
		}
		return std::nullopt;
	};
	const auto found = HitResults(program, w, kind_of);
	return found && (*found)[static_cast<uint32_t>(RayQueryResultKind::Word)] &&
	       (*found)[static_cast<uint32_t>(RayQueryResultKind::Node)] &&
	       (*found)[static_cast<uint32_t>(RayQueryResultKind::InstanceLo)];
}

// The components of the BLAS walk's intersection `bvh` a value of the walk is where it is one of
// them (bits; through bit casts, lane masked writes and phis: the walk keeps other values in the same
// registers on other paths).
uint32_t Components(const Program& program, const PacketWalk& w, Value value, const Inst* bvh) {
	uint32_t                        components = 0;
	std::unordered_set<const Inst*> seen;
	std::vector<Value>              pending {value};
	while (!pending.empty() && seen.size() < 64) {
		auto* inst = pending.back().Resolve().TryInstruction();
		pending.pop_back();
		if (inst == nullptr || !InLoop(program, inst->Parent(), w.header, w.latch) || !seen.insert(inst).second) continue;
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::BitCastF32U32 || op == ValueOpcode::BitCastU32F32) {
			pending.push_back(inst->Arg(0));
		} else if (op == ValueOpcode::SelectU32 || op == ValueOpcode::Phi) {
			for (size_t arg = op == ValueOpcode::Phi ? 0 : 1; arg < inst->NumArgs(); ++arg)
				pending.push_back(inst->Arg(arg));
		} else if (op == ValueOpcode::CompositeExtractU32x4 && inst->Arg(0).Resolve().TryInstruction() == bvh) {
			components |= 1u << Immediate(inst->Arg(1)).value_or(0u);
		}
	}
	return components;
}

// Whether a value of the walk is the reciprocal of the triangle test's denominator (component 1)
// on some path.
bool Reciprocal(const Program& program, const PacketWalk& w, Value value, const Inst* bvh) {
	std::unordered_set<const Inst*> seen;
	std::vector<Value>              pending {value};
	while (!pending.empty() && seen.size() < 64) {
		auto* inst = pending.back().Resolve().TryInstruction();
		pending.pop_back();
		if (inst == nullptr || !InLoop(program, inst->Parent(), w.header, w.latch) || !seen.insert(inst).second) continue;
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::BitCastF32U32 || op == ValueOpcode::BitCastU32F32) {
			pending.push_back(inst->Arg(0));
		} else if (op == ValueOpcode::SelectU32 || op == ValueOpcode::Phi) {
			for (size_t arg = op == ValueOpcode::Phi ? 0 : 1; arg < inst->NumArgs(); ++arg)
				pending.push_back(inst->Arg(arg));
		} else if (op == ValueOpcode::FPRecip32 && (Components(program, w, inst->Arg(0), bvh) & 2u) != 0) {
			return true;
		}
	}
	return false;
}

// The distance or a barycentric of the triangle test `bvh` a value is as the written values of lane
// masked writes (without HitDistance's search through phis, which stops at 64 values: the leaf
// code of GTA V's reflections, cs 6aeb221de7ff018e, has more).
std::optional<RayQueryResultKind> WrittenHitDistance(Value value, const Inst* bvh) {
	const auto* mul = Producer(Canonical(value), ValueOpcode::FPMul32);
	if (mul == nullptr) return std::nullopt;
	const auto component = [&](Value operand) -> std::optional<uint32_t> {
		const auto* extract = Producer(Canonical(operand), ValueOpcode::CompositeExtractU32x4);
		if (extract == nullptr || extract->Arg(0).Resolve().TryInstruction() != bvh) return std::nullopt;
		return Immediate(extract->Arg(1));
	};
	for (uint32_t side = 0; side < 2; ++side) {
		const auto* recip = Producer(Canonical(mul->Arg(1 - side)), ValueOpcode::FPRecip32);
		if (recip == nullptr || component(recip->Arg(0)) != 1u) continue;
		const auto direct = component(mul->Arg(side));
		if (direct == 0u) return RayQueryResultKind::T;
		if (direct == 2u) return RayQueryResultKind::U;
		if (direct == 3u) return RayQueryResultKind::V;
	}
	return std::nullopt;
}

// The distance or a barycentric of the triangle test `bvh` a value of the walk is: a component of
// the test (0: the distance's numerator, 2 and 3: the barycentrics') times the reciprocal of its
// denominator (component 1).
std::optional<RayQueryResultKind> HitDistance(const Program& program, const PacketWalk& w, const Inst* value,
                                              const Inst* bvh) {
	const auto* mul = Producer(Canonical(Value(const_cast<Inst*>(value))), ValueOpcode::FPMul32);
	if (mul == nullptr) return std::nullopt;
	for (uint32_t side = 0; side < 2; ++side) {
		if (!Reciprocal(program, w, mul->Arg(1 - side), bvh)) continue;
		switch (Components(program, w, mul->Arg(side), bvh) & ~2u) {
			case 1u << 0: return RayQueryResultKind::T;
			case 1u << 2: return RayQueryResultKind::U;
			case 1u << 3: return RayQueryResultKind::V;
			default: return std::nullopt;
		}
	}
	return std::nullopt;
}

// Where the BVH headers hold their node count for the closest hit walks: as the decoded sites of
// the game's other programs that trace the same TLAS take it (from the TLAS's T#: its size the
// count at +80 less one, else the size at +88), so that they share its host structures.
bool TlasNodeCount80(const Program& program, const LaneMaskWalk& s) {
	Value size = Producer(s.bvh->Arg(0), ValueOpcode::CompositeConstructU32x4)->Arg(2);
	if (const auto* part = Producer(size, ValueOpcode::CompositeExtractU64);
	    part != nullptr && Immediate(part->Arg(1)) == 0u) {
		const auto* masked = Producer(part->Arg(0), ValueOpcode::BitwiseAnd64);
		const auto* wide =
		    masked != nullptr ? Producer(masked->Arg(0), ValueOpcode::CompositeConstructU64) : nullptr;
		if (wide != nullptr) size = wide->Arg(0);
	}
	const auto* less     = Producer(size, ValueOpcode::IAdd32);
	const bool  less_one = less != nullptr && (Immediate(less->Arg(0)) == 0xffffffffu ||
	                                          Immediate(less->Arg(1)) == 0xffffffffu);
	const auto  count    = LoadOf(program, less_one ? (Immediate(less->Arg(0)) ? less->Arg(1) : less->Arg(0)) : size);
	return less_one && count && count->offset == 80u;
}

// Through bit casts only.
Value Strip(Value value) {
	for (uint32_t depth = 0; depth < 4; ++depth) {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr ||
		    (inst->GetOpcode() != ValueOpcode::BitCastF32U32 && inst->GetOpcode() != ValueOpcode::BitCastU32F32))
			break;
		value = inst->Arg(0);
	}
	return value.Resolve();
}

std::optional<float> ImmediateF32(Value value) {
	value = value.Resolve();
	if (!value.IsImmediate() || value.GetType() != Type::F32) return std::nullopt;
	return value.F32Value();
}

// x * 3 as (x << 1) + x or x * 3, and x.
std::optional<Value> TimesThree(Value value) {
	if (const auto product = Operands(value, ValueOpcode::IMul32, [](Value side) { return Immediate(side) == 3u; }))
		return Canonical(product->second);
	std::optional<Value> base;
	const auto           pair = Operands(value, ValueOpcode::IAdd32, [&](Value side) {
		const auto* shift = Producer(Canonical(side), ValueOpcode::ShiftLeftLogical32);
		if (shift == nullptr || Immediate(shift->Arg(1)) != 1u) return false;
		base = Canonical(shift->Arg(0));
		return true;
	});
	if (!pair || Canonical(pair->second) != *base) return std::nullopt;
	return base;
}

// The element `k` of x * 3 + k an index is (k + (x << 1) + x), and x.
std::optional<std::pair<uint32_t, Value>> ElementOfThree(Value index) {
	if (const auto pair = Operands(index, ValueOpcode::IAdd32, [](Value side) { return Immediate(side).has_value(); })) {
		const auto base = TimesThree(pair->second);
		const auto k    = *Immediate(pair->first);
		if (base && k < 3) return std::pair {k, *base};
	}
	if (const auto base = TimesThree(index)) return std::pair {0u, *base};
	return std::nullopt;
}

// The value a waterfall loop's key is: (ReadFirstLane(value) << shift); or, `scalar`, (value << shift)
// of a value the same in every lane.
std::optional<Value> WaterfallKey(Value offset, uint32_t shift, bool scalar = false) {
	const auto* lsl = Producer(Canonical(offset), ValueOpcode::ShiftLeftLogical32);
	if (lsl == nullptr || Immediate(lsl->Arg(1)) != shift) return std::nullopt;
	if (scalar) return Canonical(lsl->Arg(0));
	const auto* first = Producer(lsl->Arg(0), ValueOpcode::ReadFirstLane);
	if (first == nullptr) return std::nullopt;
	return Canonical(first->Arg(0));
}

// The four dwords of a buffer resource.
std::array<Value, 4> Dwords(const Inst& resource) {
	return {resource.Arg(0).Resolve(), resource.Arg(1).Resolve(), resource.Arg(2).Resolve(), resource.Arg(3).Resolve()};
}

// A descriptor of `dwords` dwords read in a waterfall loop (or, `scalar`, with scalar loads) from
// element (value << shift) of a table: the table's V# and the value.
std::optional<std::pair<std::array<Value, 4>, Value>> TableDescriptor(std::span<const Value> dwords, uint32_t shift,
                                                                      bool scalar = false) {
	std::optional<std::array<Value, 4>> table;
	std::optional<Value>                key;
	for (const auto& dword: dwords) {
		const auto* read   = Producer(dword, ValueOpcode::ReadConstBuffer);
		const auto* vsharp = read != nullptr ? Producer(read->Arg(0), ValueOpcode::GetBufferResource) : nullptr;
		const auto  value  = read != nullptr ? WaterfallKey(read->Arg(1), shift, scalar) : std::nullopt;
		if (vsharp == nullptr || !value || (table && *table != Dwords(*vsharp)) || (key && *key != *value))
			return std::nullopt;
		table = Dwords(*vsharp);
		key   = value;
	}
	if (!table) return std::nullopt;
	return std::pair {*table, *key};
}

// The value v of an operand (v >> shift), else empty.
Value ShiftedRight(Value value, uint32_t shift) {
	const auto* lsr = Producer(Canonical(value), ValueOpcode::ShiftRightLogical32);
	return lsr != nullptr && Immediate(lsr->Arg(1)) == shift ? Canonical(lsr->Arg(0)) : Value {};
}

// The kind of `kind_of` that the sources in the walk of one of its values are (through phis and
// Selects; sources of no kind do not count), when one.
template <typename KindOf>
std::optional<RayQueryResultKind> SourceKind(const Program& program, const PacketWalk& w, Value value, KindOf&& kind_of) {
	std::optional<RayQueryResultKind> kind;
	std::unordered_set<const Inst*>   seen;
	std::vector<Value>                pending {value};
	while (!pending.empty() && seen.size() < 256) {
		const auto* inst = pending.back().Resolve().TryInstruction();
		pending.pop_back();
		if (inst == nullptr || !InLoop(program, inst->Parent(), w.header, w.latch) || !seen.insert(inst).second) continue;
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::Phi || op == ValueOpcode::SelectU32) {
			for (size_t arg = op == ValueOpcode::Phi ? 0 : 1; arg < inst->NumArgs(); ++arg)
				pending.push_back(inst->Arg(arg));
			continue;
		}
		const auto source = kind_of(inst);
		if (!source) continue;
		if (kind && *kind != *source) return std::nullopt;
		kind = source;
	}
	return kind;
}

// The alpha test of GTA V's per lane leaf code (reflections, cs 6aeb221de7ff018e): the W channel of
// T# M2 >> 4 of a bindless table (read in a waterfall loop) at the triangle's texture coordinates,
// at level min(M2 & 15, log2(2 * (spread * t + lod)) + offset) (on the way of a wave-wide condition,
// when the leaf code takes another level elsewhere), against a reference, a value from before the
// walk or threshold * (M0 bit 0 ? factor : 1) (sampled > reference sets the flag the hit's moves
// test); M0..M2 the elements m * 3 + k of the material
// table (a structured buffer), m = B[B[table] + ((N >> 3) - B[first]) * 4] + R[material] of the
// triangle node N, the BLAS B and the instance record R; the triangle's indices elements
// (W & 0x3fffffff) * 3 + k of V# M1 >> 8 of the buffer table (a waterfall loop), its vertices at
// the base address of element M0 >> 8 of the buffer table plus (M1 & mask) + index * (M0 & mask),
// aligned down to 4 bytes or not, + 4 for the second dword. `node` and `word` tell the hit's
// triangle node and primitive word.
// With `a.scalar` (GTA V's Fidelity, cs 0b41af19bddf132c (01.011) and a31ec6a25be5e017 (01.005): the
// leaf code of a wave-wide BLAS walk), the same with scalar loads (RayQueryAlphaScalar): M0..M2 the
// dwords of the 12 bytes at m * 12 of the material table, the T# and the V#s of the indices and of
// the vertices read without a waterfall loop, each vertex dword at (M1 & mask) + index * (M0 & mask)
// of its V#; the reference may also be threshold * factor for every material.
template <typename IsNode, typename IsWord>
bool DeriveLaneAlpha(const Program& program, const PacketWalk& w, const BlasWalk& blas, const Inst* bvh,
                     const AddressLoad& record, IsNode&& node, IsWord&& word, LaneAlpha& a) {
	using Op = ValueOpcode;
	const Inst* sample = nullptr;
	const Inst* base   = nullptr;
	for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch); ++at) {
		for (const auto& inst: *program.blocks[at]) {
			const auto op = inst.GetOpcode();
			if (op == Op::ImageGatherRaw || op == Op::ImageRead) return false;
			if (op != Op::ImageSampleRaw) continue;
			if (sample != nullptr) return false;
			sample = &inst;
		}
	}
	if (sample == nullptr) return false;
	const auto& sampled = MemoryOf(program, *sample);
	if (sampled.dmask != 8 || sampled.image_sample_flags != Decoder::ImageSampleFlagLod ||
	    sampled.image_dimension != Decoder::ImageDimension::Dim2D)
		return false;
	const auto defined = [&](Value value) { return DefinedBefore(program, value, w.header); };
	const auto* address = Producer(sample->Arg(2), Op::MakeImageAddress);
	if (address == nullptr) return false;
	// The level on the path of a wave-wide condition c != 0, c from before the walk (GTA V's
	// Performance RT, cs 48d704640ab9fb40): where it holds; elsewhere the leaf code takes another level
	// on another way (`other`, whose code the test does not take).
	auto                            level_value = address->Arg(2);
	std::unordered_set<const Block*> other;
	if (const auto* join = Producer(Canonical(level_value), Op::Phi); join != nullptr) {
		level_value = {};
		for (size_t arg = 0; arg < join->NumArgs() && level_value.IsEmpty(); ++arg) {
			const auto* from  = join->PhiBlock(arg);
			const auto  preds = from->ImmPredecessors();
			const auto* split = preds.size() == 1 ? preds[0] : nullptr;
			if (split == nullptr || split->terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
			    split->terminator.merge_block != join->Parent())
				continue;
			const auto [ref, negated] = BranchCondition(*split);
			const auto c = ref != nullptr && (from == split->terminator.true_block) != negated
			                   ? Operands(ref->Arg(0), Op::INotEqual32, [](Value side) { return Immediate(side) == 0u; })
			                   : std::nullopt;
			if (!c || !defined(c->second)) continue;
			a.level     = c->second.Resolve();
			level_value = join->Arg(arg);
			std::vector<const Block*> pending {from == split->terminator.true_block ? split->terminator.false_block
			                                                                         : split->terminator.true_block};
			while (!pending.empty()) {
				const auto* block = pending.back();
				pending.pop_back();
				if (block == join->Parent() || !InLoop(program, block, split, join->Parent()) || !other.insert(block).second)
					continue;
				for (const auto* next: block->ImmSuccessors())
					pending.push_back(next);
			}
		}
		if (level_value.IsEmpty()) return false;
	}
	std::vector<const Inst*> loads, buffer_loads, pairs, reads;
	for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch); ++at) {
		if (other.contains(program.blocks[at])) continue;
		for (const auto& inst: *program.blocks[at]) {
			switch (inst.GetOpcode()) {
				case Op::LoadAddressU32: loads.push_back(&inst); break;
				case Op::LoadBufferU32: buffer_loads.push_back(&inst); break;
				case Op::LoadBufferU32x2: pairs.push_back(&inst); break;
				case Op::ReadConstBuffer: reads.push_back(&inst); break;
				default: break;
			}
		}
	}
	// sampled.w > reference, and the flag it sets.
	const auto alpha = [&](Value value) {
		const auto* w3 = Producer(Canonical(value), Op::CompositeExtractU32x4);
		return w3 != nullptr && w3->Arg(0).Resolve().TryInstruction() == sample && Immediate(w3->Arg(1)) == 3u;
	};
	const Inst* compare = nullptr;
	for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch); ++at) {
		for (const auto& inst: *program.blocks[at]) {
			const auto op   = inst.GetOpcode();
			const int  side = op == Op::FPOrdGreaterThan32 && alpha(inst.Arg(0)) ? 1 : op == Op::FPOrdLessThan32 && alpha(inst.Arg(1)) ? 0 : -1;
			if (side < 0) continue;
			if (compare != nullptr) return false;
			compare     = &inst;
			a.reference = Strip(inst.Arg(side));
		}
	}
	if (compare == nullptr) return false;
	// Or the leaf code computes it: threshold * (M0 bit 0 ? factor : 1) (GTA V's Performance RT, cs
	// 48d704640ab9fb40), the factor from before the walk; scalar leaf code also threshold * factor
	// (GTA V 01.005's Fidelity, cs a31ec6a25be5e017, as a wave-wide value: ReadFirstLane of it).
	Value factor_m0;
	if (const auto* first = a.scalar ? Producer(Canonical(a.reference), Op::ReadFirstLane) : nullptr; first != nullptr)
		a.reference = Strip(Canonical(first->Arg(0)));
	if (!defined(a.reference)) {
		const auto product = Operands(a.reference, Op::FPMul32, [](Value side) { return ImmediateF32(side).has_value(); });
		if (!product) return false;
		const auto* choice = Producer(Strip(product->second), Op::SelectU32);
		for (uint32_t depth = 0; choice != nullptr && depth < 4 && Immediate(choice->Arg(2)) != 0x3f800000u; ++depth)
			choice = Producer(Strip(choice->Arg(1)), Op::SelectU32);
		if (a.scalar && defined(Strip(product->second))) {
			a.factor        = Strip(product->second);
			a.factor_always = true;
		} else {
			if (choice == nullptr || Immediate(choice->Arg(2)) != 0x3f800000u || !defined(Strip(choice->Arg(1)))) return false;
			// (M0 & 1) != 0 or BitFieldUExtract(M0, 0, 1) == 1, alone or with the lanes.
			std::vector<Value> conditions {choice->Arg(0)};
			if (const auto* both = Producer(choice->Arg(0), Op::LogicalAnd); both != nullptr) {
				conditions.push_back(both->Arg(0));
				conditions.push_back(both->Arg(1));
			}
			for (const auto& condition: conditions) {
				const auto* ne  = Producer(condition, Op::INotEqual32);
				const auto  low = ne != nullptr ? Operands(condition, Op::INotEqual32, [](Value side) { return Immediate(side) == 0u; })
				                                : std::nullopt;
				const auto  bit = low ? Operands(low->second, Op::BitwiseAnd32, [](Value side) { return Immediate(side) == 1u; })
				                      : std::nullopt;
				if (bit) factor_m0 = Canonical(bit->second);
				const auto* eq  = Producer(condition, Op::IEqual32);
				const auto  one = eq != nullptr ? Operands(condition, Op::IEqual32, [](Value side) { return Immediate(side) == 1u; })
				                                : std::nullopt;
				const auto* bfe = one ? Producer(Canonical(one->second), Op::BitFieldUExtract) : nullptr;
				if (bfe != nullptr && Immediate(bfe->Arg(1)) == 0u && Immediate(bfe->Arg(2)) == 1u)
					factor_m0 = Canonical(bfe->Arg(0));
			}
			if (factor_m0.IsEmpty()) return false;
			a.factor = Strip(choice->Arg(1));
		}
		a.reference = Value(std::bit_cast<uint32_t>(*ImmediateF32(product->first)));
	}
	for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch) && a.flag == nullptr; ++at) {
		for (const auto& inst: *program.blocks[at]) {
			if (inst.GetOpcode() != Op::SelectU32 || Immediate(inst.Arg(1)) != 1u || Immediate(inst.Arg(2)) != 0u) continue;
			const auto* condition = inst.Arg(0).Resolve().TryInstruction();
			const auto* both      = Producer(inst.Arg(0), Op::LogicalAnd);
			if (condition == compare ||
			    (both != nullptr && (both->Arg(0).Resolve().TryInstruction() == compare ||
			                         both->Arg(1).Resolve().TryInstruction() == compare))) {
				a.flag = &inst;
				break;
			}
		}
	}
	if (a.flag == nullptr) return false;
	// M2: the T#'s (M2 >> 4, of a table read in a waterfall loop) and the level's (M2 & 15).
	const auto* image = Producer(sample->Arg(0), Op::GetImageResource);
	if (image == nullptr) return false;
	std::array<Value, 8> tsharp;
	for (size_t k = 0; k < 8; ++k)
		tsharp[k] = image->Arg(k);
	const auto textures = TableDescriptor(tsharp, 5, a.scalar);
	const auto m2       = textures ? ShiftedRight(textures->second, 4) : Value {};
	const auto* m2_load = Producer(m2, a.scalar ? Op::ReadConstBuffer : Op::LoadBufferU32);
	const auto descriptor_defined = [&](Value value) { return DescriptorDefinedBefore(program, value, w.header); };
	if (m2_load == nullptr || !std::ranges::all_of(textures->first, descriptor_defined)) return false;
	// The material dwords: elements m * 3 + k of one V# (scalar: dword k of the 12 bytes at
	// (m * 3) << 2).
	const auto material_element = [&](const Inst* load) -> std::optional<std::pair<uint32_t, Value>> {
		if (!a.scalar) return ElementOfThree(load->Arg(1));
		// (The offset of the load and a constant the index adds.)
		auto       at    = MemoryOf(program, *load).offset;
		auto       index = Canonical(load->Arg(1));
		const auto plus  = Operands(index, Op::IAdd32, [](Value side) { return Immediate(side).has_value(); });
		if (plus) {
			at += *Immediate(plus->first);
			index = Canonical(plus->second);
		}
		const auto* lsl = Producer(index, Op::ShiftLeftLogical32);
		const auto  m   = lsl != nullptr && Immediate(lsl->Arg(1)) == 2u ? TimesThree(lsl->Arg(0)) : std::nullopt;
		if (!m || at % 4 != 0 || at / 4 >= 3) return std::nullopt;
		return std::pair {at / 4, *m};
	};
	const auto* material = Producer(m2_load->Arg(0), Op::GetBufferResource);
	const auto  element  = material_element(m2_load);
	if (material == nullptr || !element || element->first != 2) return false;
	a.material = Dwords(*material);
	if (!std::ranges::all_of(a.material, descriptor_defined)) return false;
	std::array<Value, 3> m {};
	for (const auto* load: a.scalar ? reads : buffer_loads) {
		const auto* vsharp = Producer(load->Arg(0), Op::GetBufferResource);
		const auto  at     = material_element(load);
		if (vsharp == nullptr || !at || at->second != element->second || Dwords(*vsharp) != a.material) continue;
		if (!m[at->first].IsEmpty() && m[at->first] != Value(const_cast<Inst*>(load))) return false;
		m[at->first] = Value(const_cast<Inst*>(load));
	}
	if (m[0].IsEmpty() || m[1].IsEmpty() || m[2].IsEmpty() || (!factor_m0.IsEmpty() && factor_m0 != m[0])) return false;
	// m = R[material] + B[B[table] + ((N >> 3) - B[first]) * 4].
	const auto  descriptor = Producer(bvh->Arg(0), Op::CompositeConstructU32x4);
	const auto* blas_base  = descriptor != nullptr ? DescriptorAddress(descriptor->Arg(0)) : nullptr;
	// (The same value, or a load of the same address: GTA V 01.005's Performance RT, cs
	// 30c93330da43fd54, loads the record dwords of the BLAS address again.)
	const auto same = [&](Value a, Value b) {
		if (a.Resolve() == b.Resolve()) return true;
		const auto x = LoadOf(program, a), y = LoadOf(program, b);
		return x && y && x->low == y->low && x->high == y->high && x->offset == y->offset;
	};
	const auto  of_blas    = [&](const Inst* handle) {
		// The halves of the BLAS address, or the record dwords it is made of (GTA V 01.005, cs
		// 77826b07f891b9f5).
		if (handle != nullptr && blas_base != nullptr && blas_base->GetOpcode() == Op::CompositeConstructU64 &&
		    same(blas_base->Arg(0), handle->Arg(0)) && same(blas_base->Arg(1), handle->Arg(1)))
			return true;
		const auto* low  = handle != nullptr ? Producer(handle->Arg(0), Op::CompositeExtractU64) : nullptr;
		const auto* high = handle != nullptr ? Producer(handle->Arg(1), Op::CompositeExtractU64) : nullptr;
		return low != nullptr && high != nullptr && blas_base != nullptr && Immediate(low->Arg(1)) == 0u &&
		       Immediate(high->Arg(1)) == 1u && low->Arg(0).Resolve().TryInstruction() == blas_base &&
		       high->Arg(0).Resolve().TryInstruction() == blas_base;
	};
	const auto dword_of = [](uint32_t offset, uint32_t limit) -> std::optional<uint32_t> {
		if (offset % 4 != 0 || offset / 4 >= limit) return std::nullopt;
		return offset / 4;
	};
	std::optional<uint32_t> material_dword, table_dword, first_dword;
	const Inst*             geometry = nullptr;
	const auto              sum      = Operands(element->second, Op::IAdd32, [&](Value side) {
		const auto load = LoadOf(program, Canonical(side));
		if (!load || load->low != record.low || load->high != record.high) return false;
		material_dword = dword_of(load->offset, 32);
		return material_dword.has_value();
	});
	if (sum) geometry = Producer(Canonical(sum->second), Op::LoadAddressU32);
	if (geometry == nullptr || MemoryOf(program, *geometry).offset != 0 ||
	    !of_blas(Producer(geometry->Arg(0), Op::GetAddressResource)))
		return false;
	const auto of_blas_field = [&](Value value, std::optional<uint32_t>& dword) {
		const auto* load = Producer(Canonical(value), Op::LoadAddressU32);
		if (load == nullptr || Immediate(load->Arg(1)) != 0u || Immediate(load->Arg(2)) != 0u ||
		    !of_blas(Producer(load->Arg(0), Op::GetAddressResource)))
			return false;
		dword = dword_of(MemoryOf(program, *load).offset, 64);
		return dword.has_value();
	};
	const auto offset = Operands(geometry->Arg(1), Op::IAdd32, [&](Value side) { return of_blas_field(side, table_dword); });
	const auto* scaled = offset ? Producer(Canonical(offset->second), Op::ShiftLeftLogical32) : nullptr;
	if (scaled == nullptr || Immediate(scaled->Arg(1)) != 2u) return false;
	auto local = Operands(scaled->Arg(0), Op::IAdd32, [&](Value side) {
		const auto* negate = Producer(Canonical(side), Op::ISub32);
		return negate != nullptr && Immediate(negate->Arg(0)) == 0u && of_blas_field(negate->Arg(1), first_dword);
	});
	// (Or (N >> 3) - B[first].)
	if (const auto* sub = Producer(Canonical(scaled->Arg(0)), Op::ISub32);
	    !local && sub != nullptr && of_blas_field(sub->Arg(1), first_dword))
		local = std::pair {sub->Arg(1), sub->Arg(0)};
	const auto* lsr = local ? Producer(Canonical(local->second), Op::ShiftRightLogical32) : nullptr;
	if (lsr == nullptr || Immediate(lsr->Arg(1)) != 3u || !node(lsr->Arg(0))) return false;
	// The indices: elements p * 3 + k, p = W & 0x3fffffff, of V# (M1 >> 8) of the buffer table.
	std::array<Value, 3> indices {};
	for (const auto* load: buffer_loads) {
		const auto* vsharp = Producer(load->Arg(0), Op::GetBufferResource);
		if (vsharp == nullptr) continue;
		const auto dwords = Dwords(*vsharp);
		const auto table  = TableDescriptor(dwords, 4, a.scalar);
		if (!table || ShiftedRight(table->second, 8) != m[1]) continue;
		const auto at = ElementOfThree(load->Arg(1));
		const auto primitive =
		    at ? Operands(at->second, Op::BitwiseAnd32, [](Value side) { return Immediate(side) == 0x3fffffffu; }) : std::nullopt;
		if (!primitive || !word(primitive->second) || !indices[at->first].IsEmpty() ||
		    (!a.buffers[0].IsEmpty() && table->first != a.buffers) || !std::ranges::all_of(table->first, descriptor_defined))
			return false;
		a.buffers           = table->first;
		indices[at->first]  = Value(const_cast<Inst*>(load));
	}
	if (indices[0].IsEmpty() || indices[1].IsEmpty() || indices[2].IsEmpty()) return false;
	// The vertices' base address: element M0 >> 8 of the buffer table (two dwords); scalar: their V#,
	// element M0 >> 8 of the buffer table, whose loads read each vertex dword.
	const auto vertex_table = [&](const Inst* load) {
		const auto* vsharp = Producer(load->Arg(0), Op::GetBufferResource);
		const auto  table  = vsharp != nullptr ? TableDescriptor(Dwords(*vsharp), 4, true) : std::nullopt;
		return table && table->first == a.buffers && ShiftedRight(table->second, 8) == m[0];
	};
	for (const auto* pair: a.scalar ? std::vector<const Inst*> {} : pairs) {
		const auto* table = Producer(pair->Arg(0), Op::GetBufferResource);
		if (table == nullptr || ShiftedRight(pair->Arg(1), 8) != m[0] || Dwords(*table) != a.buffers) continue;
		if (base != nullptr) return false;
		base = pair;
	}
	if (base == nullptr && !a.scalar) return false;
	// Each vertex dword at base + ((M1 & mask) + index * (M0 & mask)), aligned down to 4 bytes or
	// not, + 4 for the second one; the mask 0xfc or 0xff (a byte). Scalar: at that offset of the
	// vertices' V#, the second one 4 bytes after it (of the index of the wave's first lane).
	const auto masked = [&](Value value, Value of, uint32_t& mask) {
		value = Canonical(value);
		if (const auto* bits = Producer(value, Op::BitFieldUExtract);
		    bits != nullptr && Immediate(bits->Arg(1)) == 0u && Immediate(bits->Arg(2)) == 8u && Canonical(bits->Arg(0)) == of) {
			mask = 0xffu;
			return true;
		}
		const auto pair = Operands(value, Op::BitwiseAnd32, [](Value side) {
			return Immediate(side) == 0xfcu || Immediate(side) == 0xffu;
		});
		if (!pair || Canonical(pair->second) != of) return false;
		mask = *Immediate(pair->first);
		return true;
	};
	const auto at_base = [&](Value side) {
		const auto* low = Producer(Canonical(side), Op::CompositeExtractU32x2);
		return low != nullptr && low->Arg(0).Resolve().TryInstruction() == base && Immediate(low->Arg(1)) == 0u;
	};
	uint32_t vertices = 0, seconds = 0, vertex_mask = 0;
	bool     vertex_align = false;
	for (const auto* load: a.scalar ? reads : loads) {
		Value value;
		bool  second = false;
		if (a.scalar) {
			const auto at = MemoryOf(program, *load).offset;
			if (!vertex_table(load) || (at != 0 && at != 4)) continue;
			value  = Canonical(load->Arg(1));
			second = at == 4;
		} else {
			const auto* handle = Producer(load->Arg(0), Op::GetAddressResource);
			const auto  sum_of = handle != nullptr ? Operands(handle->Arg(0), Op::IAdd32, at_base) : std::nullopt;
			if (!sum_of || MemoryOf(program, *load).offset != 0) continue;
			value           = Canonical(sum_of->second);
			const auto plus = Operands(value, Op::IAdd32, [](Value side) { return Immediate(side) == 4u; });
			if (plus) value = Canonical(plus->second);
			second = plus.has_value();
		}
		const auto align = Operands(value, Op::BitwiseAnd32, [](Value side) { return Immediate(side) == 0xfffffffcu; });
		if (align) value = Canonical(align->second);
		uint32_t   uv_mask = 0, stride_mask = 0;
		const auto parts   = Operands(value, Op::IAdd32, [&](Value side) { return masked(side, m[1], uv_mask); });
		const auto product = parts ? Operands(parts->second, Op::IMul32, [&](Value side) { return masked(side, m[0], stride_mask); })
		                           : std::nullopt;
		auto       index   = product ? Canonical(product->second) : Value {};
		if (const auto* first = a.scalar && product ? Producer(index, Op::ReadFirstLane) : nullptr; first != nullptr)
			index = Canonical(first->Arg(0));
		if (!product || stride_mask != uv_mask ||
		    std::ranges::none_of(indices, [&](Value candidate) { return index == candidate; }) ||
		    (vertices != 0 && (uv_mask != vertex_mask || align.has_value() != vertex_align)))
			return false;
		vertex_mask  = uv_mask;
		vertex_align = align.has_value();
		seconds += second ? 1u : 0u;
		++vertices;
	}
	if (vertices != 6 || seconds != 3) return false;
	// The level: min(M2 & 15, log2(2 * (spread * t + lod)) + offset).
	const auto level = Operands(level_value, Op::FPMin32, [&](Value side) {
		const auto* convert = Producer(Canonical(side), Op::ConvertF32U32);
		const auto  pair    = convert != nullptr ? Operands(convert->Arg(0), Op::BitwiseAnd32, [](Value v) { return Immediate(v) == 15u; })
		                                         : std::nullopt;
		return pair && Canonical(pair->second) == m[2];
	});
	const auto offset_add = level ? Operands(level->second, Op::FPAdd32, [](Value side) { return ImmediateF32(side).has_value(); })
	                              : std::nullopt;
	const auto* log       = offset_add ? Producer(Canonical(offset_add->second), Op::FPLog2) : nullptr;
	const auto  twice     = log != nullptr ? Operands(log->Arg(0), Op::FPMul32, [](Value side) { return ImmediateF32(side) == 2.0f; })
	                                       : std::nullopt;
	const auto* mad       = twice ? Producer(Canonical(twice->second), Op::FPMad32) : nullptr;
	if (mad == nullptr) return false;
	for (uint32_t side = 0; side < 2; ++side) {
		const auto* t = Canonical(mad->Arg(side)).TryInstruction();
		if (t != nullptr && WrittenHitDistance(Value(const_cast<Inst*>(t)), bvh) == RayQueryResultKind::T) {
			a.spread = Strip(mad->Arg(1 - side));
			a.lod    = Strip(mad->Arg(2));
			break;
		}
	}
	if (a.spread.IsEmpty() || !defined(a.spread) || !defined(a.lod)) return false;
	a.lod_offset                = std::bit_cast<uint32_t>(*ImmediateF32(offset_add->first));
	a.fields.material_dword     = *material_dword;
	a.fields.table_dword        = *table_dword;
	a.fields.first_dword        = *first_dword;
	a.fields.vertex_mask        = vertex_mask;
	a.fields.vertex_align       = vertex_align;
	a.valid                     = true;
	return true;
}

// The accept condition of alpha tested leaf code: that of a lane masked write of a value of `kind`,
// (lanes) && flag != 0 with the flag a phi of the alpha test's flag and of 1 (the triangles that skip
// the test). Also the lanes (`lanes`: the other side of the condition) and the condition of the
// Select(c, 1, 0) the flag 1 comes through (`direct`, when one).
template <typename KindOf>
Value FlaggedAccept(const Program& program, const BlasWalk& blas, const LaneAlpha& alpha, RayQueryResultKind kind,
                    KindOf&& kind_of, Value* lanes = nullptr, Value* direct = nullptr) {
	// A phi arm of the flag: 1 through the alpha test's flag, 2 a constant 1, else 0.
	Value      one;
	const auto arm = [&](Value value) {
		for (uint32_t depth = 0; depth < 8; ++depth) {
			value = value.Resolve();
			if (value.TryInstruction() == alpha.flag) return 1;
			if (Immediate(value) == 1u) return 2;
			const auto* select = Producer(value, ValueOpcode::SelectU32);
			if (select == nullptr) return 0;
			if (Immediate(select->Arg(1)) == 1u && Immediate(select->Arg(2)) == 0u) one = select->Arg(0).Resolve();
			value = select->Arg(1);
		}
		return 0;
	};
	const auto flagged = [&](Value condition) {
		const auto* both = Producer(condition, ValueOpcode::LogicalAnd);
		for (uint32_t side = 0; both != nullptr && side < 2; ++side) {
			const auto* ne = Producer(both->Arg(side), ValueOpcode::INotEqual32);
			for (uint32_t k = 0; ne != nullptr && k < 2; ++k) {
				const auto* phi = Immediate(ne->Arg(k)) == 0u ? Producer(ne->Arg(1 - k), ValueOpcode::Phi) : nullptr;
				if (phi == nullptr) continue;
				uint32_t arms = 0;
				one           = {};
				for (size_t arg = 0; arg < phi->NumArgs(); ++arg)
					arms |= 1u << arm(phi->Arg(arg));
				if (arms != 6u) continue;
				if (lanes != nullptr) *lanes = both->Arg(1 - side).Resolve();
				if (direct != nullptr) *direct = one;
				return true;
			}
		}
		return false;
	};
	for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch); ++at) {
		for (const auto& inst: *program.blocks[at]) {
			if (inst.GetOpcode() != ValueOpcode::SelectU32) continue;
			const auto* written = Canonical(inst.Arg(1)).TryInstruction();
			if (written != nullptr && kind_of(written) == kind && flagged(inst.Arg(0))) return inst.Arg(0).Resolve();
		}
	}
	return {};
}

// ClosestOpaque (Grand Theft Auto V's short rays, cs 84ed00053b3771da): a packet walk with a per
// lane BLAS walk whose leaf code keeps the nearest triangle itself (both facings; every triangle
// opaque, or, with its reflections (cs 6aeb221de7ff018e, 85616b0a1db9efd4), the triangles marked
// opaque and those the per lane alpha test accepts: DeriveLaneAlpha). The host traverses before the walk,
// which then takes the shortest way to its exit, and the values the code after it reads of the hit
// (its distance, barycentrics, the triangle node and primitive word of its leaf list entry and the
// instance record address) are the host's hit, when nearer than the walk's t max.
bool MatchClosestOpaqueWalk(Program& program, PacketWalk& w) {
	auto& s = w.shape;
	if (s.blases.empty()) return false;
	const auto& blas = s.blases.front();
	const Inst* bvh  = nullptr;
	bool        textures = false;
	for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch); ++at) {
		for (const auto& inst: *program.blocks[at]) {
			const auto op = inst.GetOpcode();
			textures |= op == ValueOpcode::ImageSampleRaw || op == ValueOpcode::ImageGatherRaw || op == ValueOpcode::ImageRead;
			if (op == ValueOpcode::BvhIntersect) {
				if (bvh != nullptr) return false;
				bvh = &inst;
			}
		}
	}
	const auto record = LoadOf(program, Canonical(blas.node->Arg(Incoming(*blas.node, blas.preheader))));
	if (bvh == nullptr || !record) return false;
	const auto kind_of = [&](const Inst* value) -> std::optional<RayQueryResultKind> {
		if (value == record->low.TryInstruction()) return RayQueryResultKind::InstanceLo;
		if (value == record->high.TryInstruction()) return RayQueryResultKind::InstanceHi;
		if (value->GetOpcode() == ValueOpcode::CompositeExtractU32x2 &&
		    value->Arg(0).Resolve().TryInstruction() == blas.load) {
			const auto word = Immediate(value->Arg(1));
			if (word == 0u) return RayQueryResultKind::Node;
			if (word == 1u) return RayQueryResultKind::Word;
			return std::nullopt;
		}
		// (With the alpha test the leaf code has more values than HitDistance's search takes.)
		if (const auto written = w.alpha.valid ? WrittenHitDistance(Value(const_cast<Inst*>(value)), bvh) : std::nullopt)
			return written;
		return HitDistance(program, w, value, bvh);
	};
	// The leaf code's accept condition: that of its write of a barycentric. With the alpha test (the
	// only texture read of the BLAS walk), that of the hit's moves: (the nearer test) && flag != 0, the
	// flag 1 for the triangles marked opaque and the alpha test's for the others.
	const auto of_kind = [&](RayQueryResultKind kind) {
		return [&, kind](Value value) { return SourceKind(program, w, value, kind_of) == kind; };
	};
	w.alpha = {};
	if (textures && !DeriveLaneAlpha(program, w, blas, bvh, *record, of_kind(RayQueryResultKind::Node),
	                                 of_kind(RayQueryResultKind::Word), w.alpha))
		return false;
	Value accept;
	if (w.alpha.valid) {
		accept = FlaggedAccept(program, blas, w.alpha, RayQueryResultKind::U, kind_of);
	} else {
		for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch) && accept.IsEmpty(); ++at) {
			for (const auto& inst: *program.blocks[at]) {
				if (inst.GetOpcode() != ValueOpcode::SelectU32) continue;
				const auto* written = inst.Arg(1).Resolve().TryInstruction();
				if (written != nullptr && written->GetOpcode() != ValueOpcode::Phi && written->GetOpcode() != ValueOpcode::SelectU32 &&
				    kind_of(written) == RayQueryResultKind::U) {
					accept = inst.Arg(0).Resolve();
					break;
				}
			}
		}
	}
	const auto found = !accept.IsEmpty() ? HitResults(program, w, kind_of, accept) : std::nullopt;
	if (found) {
		s.layout.node_count_80 = TlasNodeCount80(program, s);
		s.source.layout        = s.layout.Pack();
	}
	using Kind = RayQueryResultKind;
	if (!found || !std::ranges::all_of(std::array {Kind::Word, Kind::Node, Kind::InstanceLo, Kind::InstanceHi, Kind::U, Kind::V},
	                                   [&](Kind kind) { return (*found)[static_cast<uint32_t>(kind)]; })) {
		w.alpha = {};
		return false;
	}
	return true;
}

// A lane condition of leaf code as a truth table (bit c: its value with atom k = bit k of c) over up
// to five atoms, the conditions it is made of (with lane masks of waves of 32 lanes: ballots of them,
// Selects and bitwise operations), with the lanes that run the leaf (`exec`, `exec_mask`) set. With
// `flags`, a value of 0/1 constants chosen by conditions compared with 0 is a condition too (Flag). With
// `before`, a condition defined before that block is one atom.
struct LaneTable {
	static constexpr std::array<uint32_t, 5> Tables {0xaaaaaaaau, 0xccccccccu, 0xf0f0f0f0u, 0xff00ff00u,
	                                                 0xffff0000u};

	const Inst*              exec = nullptr;
	Value                    exec_mask;
	std::vector<const Inst*> atoms;
	bool                     flags   = false;
	const Program*           program = nullptr;
	const Block*             before  = nullptr;

	std::optional<uint32_t> Atom(const Inst* inst) {
		const auto at = std::ranges::find(atoms, inst);
		if (at != atoms.end()) return Tables[at - atoms.begin()];
		if (atoms.size() == Tables.size()) return std::nullopt;
		atoms.push_back(inst);
		return Tables[atoms.size() - 1];
	}

	std::optional<uint32_t> Condition(Value value, uint32_t depth = 0) {
		value = value.Resolve();
		if (value.IsImmediate())
			return value.GetType() == Type::U1 ? std::optional(value.U1() ? ~0u : 0u) : std::nullopt;
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 32) return std::nullopt;
		if (inst == exec) return ~0u;
		const auto op = inst->GetOpcode();
		if (before != nullptr && (op == ValueOpcode::LogicalNot || op == ValueOpcode::LogicalAnd || op == ValueOpcode::LogicalOr) &&
		    DefinedBefore(*program, value, before))
			return Atom(inst);
		if (op == ValueOpcode::LogicalNot) {
			const auto a = Condition(inst->Arg(0), depth + 1);
			return a ? std::optional(~*a) : std::nullopt;
		}
		if (op == ValueOpcode::LogicalAnd || op == ValueOpcode::LogicalOr || op == ValueOpcode::LogicalXor) {
			const auto a = Condition(inst->Arg(0), depth + 1), b = Condition(inst->Arg(1), depth + 1);
			if (!a || !b) return std::nullopt;
			return op == ValueOpcode::LogicalAnd ? *a & *b : op == ValueOpcode::LogicalOr ? *a | *b : *a ^ *b;
		}
		if (const auto mask = LaneMaskOf(inst); !mask.IsEmpty()) return Mask(mask, depth + 1);
		if (flags && (op == ValueOpcode::IEqual32 || op == ValueOpcode::INotEqual32)) {
			for (uint32_t k = 0; k < 2; ++k) {
				if (Immediate(inst->Arg(k)) != 0u || inst->Arg(1 - k).Resolve().IsImmediate() || !IsFlag(inst->Arg(1 - k)))
					continue;
				const auto flag = Flag(inst->Arg(1 - k), depth + 1);
				if (!flag) return std::nullopt;
				return op == ValueOpcode::IEqual32 ? ~*flag : *flag;
			}
		}
		return Atom(inst);
	}

	// Whether a condition holds in every lane that runs the leaf: `exec`, or the value a loop's lanes (a phi
	// `exec`) start from where they only lose lanes (exec && ...) on the way back.
	[[nodiscard]] bool Covers(Value condition) const {
		condition = condition.Resolve();
		if (exec == nullptr || condition.TryInstruction() == exec) return exec != nullptr;
		if (exec->GetOpcode() != ValueOpcode::Phi) return false;
		bool entry = false;
		for (size_t arg = 0; arg < exec->NumArgs(); ++arg) {
			if (exec->Arg(arg).Resolve() == condition) {
				entry = true;
			} else if (!Within(exec->Arg(arg))) {
				return false;
			}
		}
		return entry;
	}

	// Whether a condition holds only in lanes of `exec` (exec && ...).
	[[nodiscard]] bool Within(Value value, uint32_t depth = 0) const {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == exec) return true;
		if (inst == nullptr || depth > 8 || inst->GetOpcode() != ValueOpcode::LogicalAnd) return false;
		return Within(inst->Arg(0), depth + 1) || Within(inst->Arg(1), depth + 1);
	}

	// Whether a value is 0/1 constants chosen by conditions (Selects, and bitwise ands and ors of them; a
	// Select by a condition that covers the leaf's lanes is its first value).
	bool IsFlag(Value value, uint32_t depth = 0) {
		value = value.Resolve();
		if (const auto constant = Immediate(value)) return *constant <= 1u;
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 16) return false;
		switch (inst->GetOpcode()) {
			case ValueOpcode::SelectU32:
				return IsFlag(inst->Arg(1), depth + 1) && (Covers(inst->Arg(0)) || IsFlag(inst->Arg(2), depth + 1));
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseOr32: return IsFlag(inst->Arg(0), depth + 1) && IsFlag(inst->Arg(1), depth + 1);
			default: return false;
		}
	}

	// The lanes where a value of 0/1 constants chosen by conditions (Selects, and bitwise ands and ors of
	// them) is not 0.
	std::optional<uint32_t> Flag(Value value, uint32_t depth = 0) {
		value = value.Resolve();
		if (const auto constant = Immediate(value)) {
			return *constant == 1u ? std::optional(~0u) : *constant == 0u ? std::optional(0u) : std::nullopt;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 32) return std::nullopt;
		if (inst->GetOpcode() == ValueOpcode::BitwiseAnd32 || inst->GetOpcode() == ValueOpcode::BitwiseOr32) {
			const auto a = Flag(inst->Arg(0), depth + 1), b = Flag(inst->Arg(1), depth + 1);
			if (!a || !b) return std::nullopt;
			return inst->GetOpcode() == ValueOpcode::BitwiseAnd32 ? *a & *b : *a | *b;
		}
		const auto* select = Producer(value, ValueOpcode::SelectU32);
		if (select == nullptr) return std::nullopt;
		if (Covers(select->Arg(0))) return Flag(select->Arg(1), depth + 1);
		const auto c = Condition(select->Arg(0), depth + 1);
		const auto a = Flag(select->Arg(1), depth + 1), b = Flag(select->Arg(2), depth + 1);
		if (!c || !a || !b) return std::nullopt;
		return (*c & *a) | (~*c & *b);
	}

	std::optional<uint32_t> Mask(Value value, uint32_t depth) {
		value = value.Resolve();
		if (const auto constant = Immediate(value); constant) {
			return *constant == 0u ? std::optional(0u) : *constant == ~0u ? std::optional(~0u) : std::nullopt;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || depth > 32) return std::nullopt;
		if (!exec_mask.IsEmpty() && value == exec_mask) return ~0u;
		switch (inst->GetOpcode()) {
			case ValueOpcode::BitwiseNot32: {
				const auto a = Mask(inst->Arg(0), depth + 1);
				return a ? std::optional(~*a) : std::nullopt;
			}
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitwiseXor32: {
				const auto a = Mask(inst->Arg(0), depth + 1), b = Mask(inst->Arg(1), depth + 1);
				if (!a || !b) return std::nullopt;
				return inst->GetOpcode() == ValueOpcode::BitwiseAnd32  ? *a & *b
				       : inst->GetOpcode() == ValueOpcode::BitwiseOr32 ? *a | *b
				                                                       : *a ^ *b;
			}
			case ValueOpcode::CompositeExtractU32x4: {
				const auto* ballot = BallotOf(value);
				return ballot != nullptr ? Condition(ballot->Arg(0), depth + 1) : std::nullopt;
			}
			case ValueOpcode::SelectU32: {
				const auto c = Condition(inst->Arg(0), depth + 1);
				const auto a = Mask(inst->Arg(1), depth + 1), b = Mask(inst->Arg(2), depth + 1);
				if (!c || !a || !b) return std::nullopt;
				return (*c & *a) | (~*c & *b);
			}
			default: return std::nullopt;
		}
	}
};

// A dword of the walk's instance record, or some of its bits (dword & mask).
std::optional<std::pair<uint32_t, uint32_t>> RecordField(const Program& program, const LaneMaskWalk& s, Value value) {
	value         = Canonical(value);
	uint32_t mask = ~0u;
	for (uint32_t depth = 0; depth < 3; ++depth) {
		const auto* bits = Producer(value, ValueOpcode::BitwiseAnd32);
		const auto  b    = bits == nullptr ? 2u : Immediate(bits->Arg(0)) ? 0u : Immediate(bits->Arg(1)) ? 1u : 2u;
		if (b == 2u) break;
		mask &= *Immediate(bits->Arg(b));
		value = Canonical(bits->Arg(1 - b));
	}
	const auto load = LoadOf(program, value);
	if (!load || load->low != s.record.low || load->high != s.record.high || (load->offset & 3u) != 0)
		return std::nullopt;
	return std::pair {load->offset / 4u, mask};
}

// A bit of a record dword: BitFieldUExtract(dword, bit, 1), (dword >> bit) & 1, or the dword's bits
// from `bit` up shifted down when they are that bit alone.
std::optional<std::pair<uint32_t, uint32_t>> RecordBit(const Program& program, const LaneMaskWalk& s, Value value) {
	const auto* inst = Canonical(value).TryInstruction();
	if (inst == nullptr) return std::nullopt;
	const auto op  = inst->GetOpcode();
	const auto bit = op == ValueOpcode::BitFieldUExtract || op == ValueOpcode::ShiftRightLogical32
	                     ? Immediate(inst->Arg(1))
	                     : std::nullopt;
	if (bit && *bit < 32u) {
		const auto f = RecordField(program, s, inst->Arg(0));
		if (f && ((f->second >> *bit) & 1u) != 0 &&
		    ((op == ValueOpcode::BitFieldUExtract && Immediate(inst->Arg(2)) == 1u) ||
		     (op == ValueOpcode::ShiftRightLogical32 && (f->second >> *bit) == 1u)))
			return std::pair {f->first, *bit};
	}
	for (uint32_t side = 0; op == ValueOpcode::BitwiseAnd32 && side < 2; ++side) {
		const auto* shift = Producer(inst->Arg(side), ValueOpcode::ShiftRightLogical32);
		const auto  by    = shift != nullptr ? Immediate(shift->Arg(1)) : std::nullopt;
		if (Immediate(inst->Arg(1 - side)) != 1u || !by || *by >= 32u) continue;
		if (const auto f = RecordField(program, s, shift->Arg(0)); f && ((f->second >> *by) & 1u) != 0)
			return std::pair {f->first, *by};
	}
	return std::nullopt;
}

// The facing the leaf code of a wave-wide BLAS walk accepts (ClosestWave), from its accept condition
// (`negated`: its reject condition), per lane
//   accept = hit & !far & (facing | flags bit 2 | !enable)
// of the triangle test `tri`'s compares, hit (its first result is not +inf), facing (flags bit 3 ==
// the sign of the first result, or != for the flipped facing) and far (the distance beyond the t
// max `current`), of the instance's flags bit 2 (both facings) and of an enable from before the walk
// (the lanes that cull), with the lanes that run the leaf set (the others keep their hit). Sets the
// layout's flags and flip, the lanes that cull and the queries' facing. With `flags`, the conditions may
// be 0/1 values compared with 0 (LaneTable::Flag; GTA V's cs 2f34bc2dfe6b04a3: enable & !both != 0).
// With `leaf`, the accept condition may also need a condition of the leaf code's own, which the pass does
// not model (Gran Turismo 7: what its any hit functions return): `leaf` is then set, and the guest leaf
// code has to decide the triangles that are not opaque.
template <typename KindOf>
bool LeafFacing(const Program& program, PacketWalk& w, Value condition, bool negated, const Inst* tri, Value current,
                KindOf&& kind_of, bool flags = false, bool* leaf = nullptr) {
	auto&     s = w.shape;
	LaneTable lanes;
	lanes.exec       = tri->Arg(2).Resolve().TryInstruction();
	lanes.exec_mask  = LaneMaskOf(lanes.exec);
	lanes.flags      = flags;
	lanes.program    = flags ? &program : nullptr;
	lanes.before     = flags ? w.header : nullptr;
	const auto table = lanes.Condition(condition);
	if (!table) return false;
	// A component of the triangle test as it gives it.
	const auto raw = [&](Value value, uint32_t component) {
		const auto* extract = Producer(Canonical(value), ValueOpcode::CompositeExtractU32x4);
		return extract != nullptr && extract->Arg(0).Resolve().TryInstruction() == tri &&
		       Immediate(extract->Arg(1)) == component;
	};
	const auto field      = [&](Value value) { return RecordField(program, s, value); };
	const auto record_bit = [&](Value value) { return RecordBit(program, s, value); };
	// A test of one bit of a record dword (and whether it tests that the bit is set).
	const auto bit_test = [&](const Inst* test) -> std::optional<std::tuple<uint32_t, uint32_t, bool>> {
		const auto op = test->GetOpcode();
		if (op != ValueOpcode::IEqual32 && op != ValueOpcode::INotEqual32) return std::nullopt;
		for (uint32_t side = 0; side < 2; ++side) {
			const auto constant = Immediate(test->Arg(side));
			const auto other    = test->Arg(1 - side);
			if (constant == 0u) {
				if (const auto f = field(other); f && std::popcount(f->second) == 1) {
					return std::tuple {f->first, static_cast<uint32_t>(std::countr_zero(f->second)),
					                   op == ValueOpcode::INotEqual32};
				}
				if (const auto bit = record_bit(other))
					return std::tuple {bit->first, bit->second, op == ValueOpcode::INotEqual32};
			} else if (constant == 1u) {
				if (const auto bit = record_bit(other))
					return std::tuple {bit->first, bit->second, op == ValueOpcode::IEqual32};
			}
		}
		return std::nullopt;
	};
	// What each atom is.
	constexpr size_t None = 8;
	size_t           hit = None, facing = None, far = None, both = None, enable = None, guest = None;
	bool             near = false, flip = false, both_set = true;
	uint32_t         flags_dword = 0, facing_bit = 0, both_dword = 0, both_bit = 0;
	for (size_t k = 0; k < lanes.atoms.size(); ++k) {
		const auto* atom = lanes.atoms[k];
		const auto  op   = atom->GetOpcode();
		if ((op == ValueOpcode::FPOrdGreaterThan32 || op == ValueOpcode::FPOrdLessThanEqual32) && far == None) {
			const auto* t = Canonical(atom->Arg(0)).TryInstruction();
			if (t != nullptr && kind_of(t) == RayQueryResultKind::T && Canonical(atom->Arg(1)) == current) {
				far  = k;
				near = op == ValueOpcode::FPOrdLessThanEqual32;
				continue;
			}
		}
		if ((op == ValueOpcode::IEqual32 || op == ValueOpcode::INotEqual32) && facing == None) {
			for (uint32_t side = 0; side < 2 && facing == None; ++side) {
				const auto* sign = Producer(Canonical(atom->Arg(side)), ValueOpcode::ShiftRightLogical32);
				const auto  bit  = sign != nullptr && Immediate(sign->Arg(1)) == 31u && raw(sign->Arg(0), 0)
				                       ? record_bit(atom->Arg(1 - side))
				                       : std::nullopt;
				if (bit && bit->second >= 3u) {
					facing      = k;
					flags_dword = bit->first;
					facing_bit  = bit->second;
					flip        = op == ValueOpcode::INotEqual32;
				}
			}
			if (facing == k) continue;
		}
		const bool compare = op == ValueOpcode::IEqual32 || op == ValueOpcode::INotEqual32 ||
		                     op == ValueOpcode::FPOrdNotEqual32 || op == ValueOpcode::FPUnordNotEqual32;
		if (compare && hit == None && (raw(atom->Arg(0), 0) || raw(atom->Arg(1), 0))) {
			hit = k;
			continue;
		}
		if (const auto test = bit_test(atom); test && both == None) {
			std::tie(both_dword, both_bit, both_set) = *test;
			both                                     = k;
			continue;
		}
		if (enable == None && DefinedBefore(program, Value(const_cast<Inst*>(atom)), w.header)) {
			enable = k;
			continue;
		}
		if (leaf != nullptr && guest == None) {
			guest = k;
			continue;
		}
		return false;
	}
	if (hit == None || facing == None || both == None || both_dword != flags_dword || both_bit + 1u != facing_bit)
		return false;
	const auto value = [&](size_t k, uint32_t c) { return k != None && ((c >> k) & 1u) != 0; };
	// (The leaf code's own condition is one more term of the conjunction: its value where the code accepts.)
	bool guest_set = true;
	for (uint32_t c = 0; c < 32 && guest != None; ++c) {
		if ((((*table >> c) & 1u) != 0) != negated) {
			guest_set = value(guest, c);
			break;
		}
	}
	for (uint32_t c = 0; c < 32; ++c) {
		const bool h = value(hit, c), f = value(facing, c), g = far != None && value(far, c) != near;
		const bool b2 = value(both, c) == both_set, e = enable == None || value(enable, c);
		const bool a  = guest == None || value(guest, c) == guest_set;
		if ((((*table >> c) & 1u) != 0) != ((h && !g && (f || b2 || !e) && a) != negated)) return false;
	}
	s.layout.flags_dword = flags_dword;
	s.layout.flags_shift = facing_bit - 3u;
	s.layout.flip_facing = flip;
	w.cull               = enable != None ? Value(const_cast<Inst*>(lanes.atoms[enable])) : Value(true);
	w.both_facings       = false;
	if (leaf != nullptr) *leaf = guest != None;
	return true;
}

// The BLAS address of a wave-wide BLAS walk's triangle test (the base of its descriptor).
const Inst* WaveBlasBase(const Inst* tri) {
	const auto* words = Producer(tri->Arg(0), ValueOpcode::CompositeConstructU32x4);
	return words != nullptr ? DescriptorAddress(words->Arg(0)) : nullptr;
}

// What a value of the leaf code of a wave-wide BLAS walk is (ClosestWave, PacketWave): the instance
// record address, a word of the leaf list entry or the distance or a barycentric of the triangle
// test `tri`.
std::optional<RayQueryResultKind> WaveResultKind(const Program& program, const LaneMaskWalk& s, const Inst* tri,
                                                 const Inst* value) {
	const auto* base = WaveBlasBase(tri);
	if (base == nullptr) return std::nullopt;
	const auto component = [&](Value operand) -> std::optional<uint32_t> {
		const auto* extract = Producer(Canonical(operand), ValueOpcode::CompositeExtractU32x4);
		return extract != nullptr && extract->Arg(0).Resolve().TryInstruction() == tri ? Immediate(extract->Arg(1))
		                                                                               : std::nullopt;
	};
	if (value == s.record.low.TryInstruction()) return RayQueryResultKind::InstanceLo;
	if (value == s.record.high.TryInstruction()) return RayQueryResultKind::InstanceHi;
	// The leaf list's entries (triangle node, primitive word): loads of the BLAS's buffer in its
	// walk.
	if (value->GetOpcode() == ValueOpcode::ReadConstBuffer) {
		// (Its base: the low word of the BLAS address, packed or as the record holds it.)
		const auto* resource = Producer(value->Arg(0), ValueOpcode::GetBufferResource);
		const auto* low =
		    resource != nullptr ? Producer(resource->Arg(0), ValueOpcode::CompositeExtractU64) : nullptr;
		const bool blas =
		    resource != nullptr &&
		    ((low != nullptr && Immediate(low->Arg(1)) == 0u && low->Arg(0).Resolve().TryInstruction() == base) ||
		     (base->GetOpcode() == ValueOpcode::CompositeConstructU64 &&
		      resource->Arg(0).Resolve() == base->Arg(0).Resolve()));
		if (!blas || !InLoop(program, value->Parent(), s.wave_header, s.wave_latch)) return std::nullopt;
		// (Where in the 8-byte entry: the load's offset, and a constant the index adds.)
		auto at = MemoryOf(program, *value).offset;
		if (const auto* add = Producer(value->Arg(1), ValueOpcode::IAdd32); add != nullptr)
			at += Immediate(add->Arg(0)).value_or(Immediate(add->Arg(1)).value_or(0u));
		at &= 7u;
		return at == 0u   ? std::optional(RayQueryResultKind::Node)
		       : at == 4u ? std::optional(RayQueryResultKind::Word)
		                  : std::nullopt;
	}
	// The distance and the barycentrics: a component of the triangle test (0, 2, 3) times the
	// reciprocal of component 1, as the test gives them (the leaf code reuses their registers).
	const auto* mul = Producer(Canonical(Value(const_cast<Inst*>(value))), ValueOpcode::FPMul32);
	for (uint32_t side = 0; mul != nullptr && side < 2; ++side) {
		const auto* rcp = Producer(Canonical(mul->Arg(1 - side)), ValueOpcode::FPRecip32);
		if (rcp == nullptr || component(rcp->Arg(0)) != 1u) continue;
		switch (component(mul->Arg(side)).value_or(1u)) {
			case 0: return RayQueryResultKind::T;
			case 2: return RayQueryResultKind::U;
			case 3: return RayQueryResultKind::V;
			default: return std::nullopt;
		}
	}
	return std::nullopt;
}

// ClosestWave (ASTRO BOT, cs bdd0bb9bd8852dd4; ASTRO's PLAYROOM's 2020 template): a packet walk
// whose BLAS walk is the wave's (scalar nodes: a box test and a test of the triangles of a leaf
// list) with leaf code that keeps the nearest triangle, every one opaque, of the facing the
// instance flags select (see LeafFacing): it writes the distance (and what it keeps of the hit: the
// list entry's primitive word and triangle node, the instance record address) under its accept
// condition, or keeps them under its reject condition. As for ClosestOpaque, the host traverses
// before the walk, now with that facing, and the values the code after the walk reads of the hit
// are the host's, when nearer than the walk's t max.
bool MatchClosestWaveWalk(Program& program, PacketWalk& w) {
	auto& s = w.shape;
	if (s.wave_triangles == nullptr || !s.blases.empty()) return false;
	const auto first = BlockIndex(program, s.wave_header), last = BlockIndex(program, s.wave_latch);
	const Inst* tri = s.wave_triangles;
	const auto  kind_of = [&](const Inst* value) { return WaveResultKind(program, s, tri, value); };
	if (WaveBlasBase(tri) == nullptr) return false;
	// The far test (T > t, or T <= t, t the distance the leaf code keeps) and the write of the
	// distance: Select(accept, T, t), or Select(reject, t, T).
	Value current;
	for (auto at = first; at <= last; ++at) {
		for (const auto& inst: *program.blocks[at]) {
			if (inst.GetOpcode() != ValueOpcode::FPOrdGreaterThan32 && inst.GetOpcode() != ValueOpcode::FPOrdLessThanEqual32)
				continue;
			const auto* t = Canonical(inst.Arg(0)).TryInstruction();
			if (t == nullptr || kind_of(t) != RayQueryResultKind::T) continue;
			if (!current.IsEmpty() && current != Canonical(inst.Arg(1))) return false;
			current = Canonical(inst.Arg(1));
		}
	}
	if (current.IsEmpty()) return false;
	Value accept;
	bool  negated = false;
	for (auto at = first; at <= last; ++at) {
		for (const auto& inst: *program.blocks[at]) {
			for (uint32_t arm = 1; inst.GetOpcode() == ValueOpcode::SelectU32 && arm <= 2; ++arm) {
				const auto* t = Canonical(inst.Arg(arm)).TryInstruction();
				if (t == nullptr || Canonical(inst.Arg(3 - arm)) != current || kind_of(t) != RayQueryResultKind::T)
					continue;
				if (!accept.IsEmpty() && (accept != inst.Arg(0).Resolve() || negated != (arm == 2))) return false;
				accept  = inst.Arg(0).Resolve();
				negated = arm == 2;
			}
		}
	}
	if (accept.IsEmpty() || !LeafFacing(program, w, accept, negated, tri, current, kind_of, false, &w.leaf)) return false;
	// (Without the guest leaf code, no alpha test of its own.)
	for (auto at = first; at <= last && !w.leaf; ++at) {
		for (const auto& inst: *program.blocks[at]) {
			const auto op = inst.GetOpcode();
			if (op == ValueOpcode::ImageSampleRaw || op == ValueOpcode::ImageGatherRaw || op == ValueOpcode::ImageRead)
				return false;
		}
	}
	const auto found = HitResults(program, w, kind_of, accept, negated, true);
	if (!found || !(*found)[static_cast<uint32_t>(RayQueryResultKind::Word)]) return false;
	s.layout.node_count_80 = TlasNodeCount80(program, s);
	s.source.layout        = s.layout.Pack();
	return true;
}

// ClosestLeaf (Gran Turismo 7, the program of TestRayTracingClosestLeafCalls): a ClosestWave walk whose
// leaf code accepts with a condition of its own (what the any hit functions of its inlined calls return,
// see LeafFacing). The walk's TLAS node is the root, then a node popped from its stack; its lanes a phi of
// its header that starts at the lanes of the trace; its exit the round's end, before the pop.
bool MatchClosestLeafWalk(const Program& program, PacketWalk& w) {
	auto& s = w.shape;
	if (!w.leaf || s.wave_node == nullptr) return false;
	const auto from = [](const Inst& phi, const Block* block) { return phi.Arg(Incoming(phi, block)); };
	w.outer_node    = w.header == s.header ? s.node : LoopPhi(from(*s.node, s.preheader), w.header);
	if (w.outer_node == nullptr || Immediate(from(*w.outer_node, w.preheader)) != s.source.root) return false;
	const auto  popped = from(*w.outer_node, w.latch);
	const auto* pop    = Producer(popped, ValueOpcode::Phi);
	for (size_t arg = 0; pop != nullptr && arg < pop->NumArgs(); ++arg) {
		if (Producer(pop->Arg(arg), ValueOpcode::ReadLane) == nullptr) return false;
	}
	if (pop == nullptr && Producer(popped, ValueOpcode::ReadLane) == nullptr) return false;
	w.packet = nullptr;
	for (auto& inst: *w.header) {
		if (inst.GetOpcode() != ValueOpcode::Phi || inst.GetType() != Type::U1) continue;
		const auto entry = from(inst, w.preheader);
		if (entry.IsImmediate() || !DefinedBefore(program, entry, w.header)) continue;
		if (w.packet != nullptr) return false;
		w.packet = &inst;
	}
	// The round's end: a test that leaves the walk or goes on to its latch; the distance the leaf code
	// keeps (the t max of the walk's tests) there.
	const auto* exit = w.path.back();
	const auto& term = exit->terminator;
	if (w.packet == nullptr || term.kind != CFG::TerminatorKind::ConditionalBranch ||
	    !((term.true_block == w.merge && term.false_block == w.latch) ||
	      (term.false_block == w.merge && term.true_block == w.latch)))
		return false;
	w.leaf_t = {};
	for (auto& inst: *w.header) {
		if (inst.GetOpcode() != ValueOpcode::Phi || from(inst, w.preheader).Resolve() != w.t_max.Resolve()) continue;
		const auto* kept = from(inst, w.latch).Resolve().TryInstruction();
		if (kept == nullptr || kept->Parent() != exit || !w.leaf_t.IsEmpty()) return false;
		w.leaf_t = Value(const_cast<Inst*>(kept));
	}
	if (w.leaf_t.IsEmpty()) return false;
	// The loop over the leaf list around the triangle test, in the BLAS walk.
	auto*  list     = EnclosingLoop(program, s.wave_triangles->Parent());
	Block *list_pre = nullptr, *list_latch = nullptr, *list_merge = nullptr;
	if (list == nullptr || list == s.wave_header || !LoopShape(list, list_pre, list_latch, list_merge) ||
	    !InLoop(program, list, s.wave_header, s.wave_latch))
		return false;
	w.list_exits.clear();
	for (auto at = BlockIndex(program, list); at <= BlockIndex(program, list_latch); ++at) {
		auto*       block = program.blocks[at];
		const auto& next  = block->terminator;
		if (next.kind != CFG::TerminatorKind::ConditionalBranch ||
		    (next.true_block != list_latch && next.false_block != list_latch))
			continue;
		auto* other = next.true_block == list_latch ? next.false_block : next.true_block;
		if (InLoop(program, other, list, list_latch)) continue;
		if (BranchCondition(*block).first == nullptr) return false;
		w.list_exits.emplace_back(block, next.true_block == other);
	}
	return !w.list_exits.empty();
}

// The host traverses with the opacity of the geometry and of the instance flags and confirms the opaque
// triangles. The guest walk then runs once per candidate the queries leave, the wave's smallest
// (instance, leaf) key, for the lanes that have it: its TLAS node is the key's instance, its BLAS walk
// starts at the key's leaf and stops after that entry of the leaf list, and the leaf code keeps the
// nearest hit it accepts (every lane goes on searching). After the walk, the values read of the hit are
// the host's where its opaque hit is nearer, and its lanes those that started it.
void RewriteClosestLeafWalk(Program& program, PacketWalk& w, uint32_t site) {
	using Op         = ValueOpcode;
	const auto flags = [site](auto variant) {
		return RayQueryFlags(site, static_cast<uint32_t>(variant));
	};
	auto&      s     = w.shape;
	const auto lanes = w.packet->Arg(Incoming(*w.packet, w.preheader)).Resolve();
	// What the code after the walk reads of it (taken before the walk changes): the values of the hit, and
	// the walk's lanes (values of its lanes phi, through phis and lane selects).
	const auto first = BlockIndex(program, w.header), last = BlockIndex(program, w.latch);
	const auto in_walk = [&](const Block* block) {
		const auto at = BlockIndex(program, block);
		return at >= first && at <= last;
	};
	const auto outside = [&](Inst* inst) {
		auto uses = inst->Uses();
		std::erase_if(uses, [&](const Use& use) { return in_walk(use.user->Parent()); });
		return uses;
	};
	const auto walk_lanes = [&](const Inst* value) {
		std::vector<const Inst*>        pending {value};
		std::unordered_set<const Inst*> seen;
		while (!pending.empty()) {
			const auto* at = pending.back();
			pending.pop_back();
			if (at == w.packet || !seen.insert(at).second) continue;
			const auto op = at->GetOpcode();
			if (!in_walk(at->Parent()) || (op != Op::Phi && op != Op::SelectU1)) return false;
			for (size_t arg = op == Op::Phi ? 0 : 1; arg < (op == Op::Phi ? at->NumArgs() : 3); ++arg) {
				const auto source = at->Arg(arg).Resolve();
				if (source == lanes) continue;
				if (source.TryInstruction() == nullptr) return false;
				pending.push_back(source.TryInstruction());
			}
		}
		return true;
	};
	using Uses = decltype(outside(nullptr));
	std::vector<Uses>                       hit;
	std::vector<std::pair<Inst*, Uses>>     left;
	for (auto& [inst, kind]: w.results) {
		hit.push_back(outside(inst));
	}
	for (auto at = first; at <= last; ++at) {
		for (auto& inst: *program.blocks[at]) {
			if (inst.GetType() != Type::U1) continue;
			auto uses = outside(&inst);
			if (!uses.empty() && walk_lanes(&inst)) left.emplace_back(&inst, std::move(uses));
		}
	}
	// Before the walk: the queries (the triangles that are not opaque are candidates of the leaf code).
	const Inserter entry {w.preheader, w.preheader->end()};
	const auto     native =
	    entry(Op::RayQueryInit,
	          {s.ray[0], s.ray[1], s.ray[2], s.ray[3], s.ray[4], s.ray[5], w.t_max, RayMask(s), s.tlas[0], s.tlas[1],
	           Value(0u), lanes, w.cull, Value(false), Value(true)},
	          flags(w.both_facings ? RayQueryInitBothFacings : 0u));
	const auto ballot = entry(Op::Ballot, {native});
	const auto any =
	    entry(Op::INotEqual32,
	          {entry(Op::BitwiseOr32, {entry(Op::CompositeExtractU32x4, {ballot, Value(0u)}),
	                                   entry(Op::CompositeExtractU32x4, {ballot, Value(1u)})}),
	           Value(0u)});
	// A round's node and lanes, set at the end of `from`: the wave's next key's instance, and the lanes with
	// it (whether there is one).
	const auto round = [&](const Inserter& at, Block* from) {
		const auto     next   = at(Op::RayQueryNext, {}, flags(0u));
		const auto     match  = at(Op::RayQueryMatch, {}, flags(0u));
		const auto     node   = Incoming(*w.outer_node, from);
		const auto     packet = Incoming(*w.packet, from);
		const Inserter end {from, from->end()};
		w.outer_node->SetArg(node, end(Op::SelectU32, {any, at(Op::CompositeExtractU32x4, {next, Value(0u)}),
		                                               w.outer_node->Arg(node)}));
		w.packet->SetArg(packet, end(Op::SelectU1, {any, match, w.packet->Arg(packet)}));
		return at(Op::INotEqual32, {at(Op::CompositeExtractU32x4, {next, Value(2u)}), Value(0u)});
	};
	(void)round(entry, w.preheader);
	// The BLAS walk starts at the key's leaf; the leaf code tests that entry of the list only (the queries
	// report the others as keys of their own).
	Block *pre = nullptr, *latch = nullptr, *merge = nullptr;
	LoopShape(s.wave_header, pre, latch, merge);
	const Inserter blas {pre, pre->end()};
	const auto     root = Incoming(*s.wave_node, pre);
	s.wave_node->SetArg(root, blas(Op::SelectU32, {any, blas(Op::RayQuerySelectedLeaf, {}, flags(0u)),
	                                               s.wave_node->Arg(root)}));
	for (const auto& [block, leaves_on_true]: w.list_exits) {
		const auto [ref, negated] = BranchCondition(*block);
		const Inserter in {ref->Parent(), Position(*ref)};
		ref->SetArg(0, in(Op::SelectU1, {any, Value(leaves_on_true != negated), ref->Arg(0)}));
	}
	// The round's end: the lanes of the key go on searching; the next key, or the exit.
	auto*          exit       = w.path.back();
	const auto [ref, negated] = BranchCondition(*exit);
	const Inserter at {exit, Position(*ref)};
	at(Op::RayQueryMark, {}, flags(RayQueryMarkKind::Resolve));
	const auto has            = round(at, w.latch);
	const bool leaves_on_true = exit->terminator.true_block == w.merge;
	ref->SetArg(0, at(Op::SelectU1, {any, leaves_on_true != negated ? at(Op::LogicalNot, {has}) : has, ref->Arg(0)}));
	// After the walk: the host's hit where nearer than the leaf code's, and the lanes of the trace.
	const Inserter after {w.merge, AfterPhis(*w.merge)};
	const auto     result = [&](RayQueryResultKind kind) { return after(Op::RayQueryResult, {}, flags(kind)); };
	const auto     commit =
	    after(Op::LogicalAnd, {after(Op::INotEqual32, {result(RayQueryResultKind::Has), Value(0u)}),
	                           after(Op::FPOrdLessThanEqual32, {after(Op::BitCastF32U32, {result(RayQueryResultKind::T)}),
	                                                            after(Op::BitCastF32U32, {w.leaf_t})})});
	for (size_t k = 0; k < hit.size(); ++k) {
		const auto [inst, kind] = w.results[k];
		const auto select       = after(Op::SelectU32, {commit, result(kind), Value(inst)});
		for (const auto& use: hit[k])
			use.user->SetArg(use.operand, select);
	}
	for (auto& [inst, uses]: left) {
		const auto select = after(Op::SelectU1, {any, lanes, Value(inst)});
		for (const auto& use: uses)
			use.user->SetArg(use.operand, select);
	}
	after(Op::RayQueryMark, {}, flags(RayQueryMarkKind::Exit));
}

// The opacity of any hit leaf code (PacketLane, PacketWave) over three atoms of a LaneTable: off (x != 0, or
// x == 0 negated, x from before the walk: every triangle opaque where x != 0), opaque ((flags | word >> 30) & 1 != 0, or bit 0 of it == 1:
// the instance flags' bit 0 or the primitive word's bit 30) and alpha tested ((flags & 2) != 0), the flags a
// record dword, shifted down or not, the same in both.
struct LeafOpacity {
	size_t                        off = 0, opaque = 0, alpha_tested = 0;
	std::pair<uint32_t, uint32_t> flags; // The record dword and the shift.
	Value                         off_value;
	bool                          off_negated = false; // The off atom is x == 0.

	// Whether the triangles of the atoms' values `c` skip the alpha test (flag 1).
	[[nodiscard]] bool Taken(uint32_t c) const {
		const auto atom = [c](size_t k) { return ((c >> k) & 1u) != 0; };
		return atom(off) != off_negated || (atom(opaque) && !atom(alpha_tested));
	}
};

template <typename IsWord>
std::optional<LeafOpacity> DeriveLeafOpacity(const Program& program, const PacketWalk& w,
                                             std::span<const Inst* const> atoms, IsWord&& is_word) {
	using Op = ValueOpcode;
	if (atoms.size() != 3) return std::nullopt;
	const auto& s = w.shape;
	// The instance flags as the leaf code reads them: a record dword, shifted down or not.
	const auto flags_of = [&](Value value) -> std::optional<std::pair<uint32_t, uint32_t>> {
		value             = Canonical(value);
		uint32_t    shift = 0;
		const auto* lsr   = Producer(value, Op::ShiftRightLogical32);
		if (lsr != nullptr) {
			const auto by = Immediate(lsr->Arg(1));
			if (!by || *by >= 29u) return std::nullopt;
			shift = *by;
			value = lsr->Arg(0);
		}
		const auto f = RecordField(program, s, value);
		if (!f || f->second != ~0u) return std::nullopt;
		return std::pair {f->first, shift};
	};
	constexpr size_t                             None = 8;
	LeafOpacity                                  o {None, None, None};
	std::optional<std::pair<uint32_t, uint32_t>> flags;
	for (size_t k = 0; k < atoms.size(); ++k) {
		// x != 0, or BitFieldUExtract(v, 0, 1) == 1 as (v & 1) != 0.
		Value value;
		if (atoms[k]->GetOpcode() == Op::INotEqual32) {
			const auto pair = Operands(Value(const_cast<Inst*>(atoms[k])), Op::INotEqual32,
			                           [](Value side) { return Immediate(side) == 0u; });
			if (pair) value = pair->second;
		}
		Value bit_of;
		if (atoms[k]->GetOpcode() == Op::IEqual32) {
			const auto  pair = Operands(Value(const_cast<Inst*>(atoms[k])), Op::IEqual32,
			                            [](Value side) { return Immediate(side) == 1u; });
			const auto* bfe  = pair ? Producer(Canonical(pair->second), Op::BitFieldUExtract) : nullptr;
			if (bfe != nullptr && Immediate(bfe->Arg(1)) == 0u && Immediate(bfe->Arg(2)) == 1u) bit_of = Canonical(bfe->Arg(0));
			const auto zero = Operands(Value(const_cast<Inst*>(atoms[k])), Op::IEqual32,
			                           [](Value side) { return Immediate(side) == 0u; });
			if (bit_of.IsEmpty() && zero && o.off == None && DefinedBefore(program, zero->second, w.header)) {
				o.off         = k;
				o.off_value   = zero->second.Resolve();
				o.off_negated = true;
				continue;
			}
		}
		if (value.IsEmpty() && bit_of.IsEmpty()) return std::nullopt;
		if (!value.IsEmpty() && o.off == None && DefinedBefore(program, value, w.header)) {
			o.off       = k;
			o.off_value = value.Resolve();
			continue;
		}
		// (flags | word >> 30) & 1, or flags & 2.
		const auto bit = bit_of.IsEmpty() ? Operands(value, Op::BitwiseAnd32, [](Value side) { return Immediate(side) == 1u; })
		                                  : std::optional(std::pair {Value(1u), bit_of});
		const auto any = bit ? Operands(bit->second, Op::BitwiseOr32,
		                                [&](Value side) {
			                                const auto* word = Producer(Canonical(side), Op::ShiftRightLogical32);
			                                return word != nullptr && Immediate(word->Arg(1)) == 30u && is_word(word->Arg(0));
		                                })
		                     : std::nullopt;
		const auto two = !value.IsEmpty() ? Operands(value, Op::BitwiseAnd32, [](Value side) { return Immediate(side) == 2u; })
		                                  : std::nullopt;
		const auto f   = any ? flags_of(any->second) : two ? flags_of(two->second) : std::nullopt;
		if (!f || (flags && *flags != *f)) return std::nullopt;
		flags = f;
		if (any && o.opaque == None) {
			o.opaque = k;
		} else if (!any && o.alpha_tested == None) {
			o.alpha_tested = k;
		} else {
			return std::nullopt;
		}
	}
	if (o.off == None || o.opaque == None || o.alpha_tested == None) return std::nullopt;
	o.flags = *flags;
	return o;
}

// PacketLane (GTA V's Performance RT, cs 48d704640ab9fb40 (01.011) and 30c93330da43fd54 (01.005)): a
// packet walk with a per lane BLAS walk whose leaf code takes any hit of the triangle test `tri`,
// per lane:
//   facing    hit & (!A | f) & (!B | !f): hit the first result is not +inf, f the sign of the first
//             result against the instance flags' facing bit (or its negation: the layout's flip), A
//             and B u & (the flags' both facings bit clear) with wave-wide values u from before the
//             walk, Select(x == 0, 1, 0) and Select(x != 0, 1, 0) of one x
//   distance  T <= the t max
//   opacity   the lanes with off | (opaque & !alpha tested) take flag 1 (off a value from before the
//             walk, opaque the flags' bit 0 or the primitive word's bit 30, alpha tested the flags'
//             bit 1), the others the alpha test's (DeriveLaneAlpha)
//   accept    (all of it) && flag != 0: it writes the distance, the primitive word and the instance
//             record address
// The host traverses before the walk with the facing and the opacity of the instance flags (the
// facing flipped where B's u holds, every instance opaque where off holds) and the alpha test, where
// the alpha test's level condition holds (elsewhere the queries keep the guest's walk); as Entry,
// the walk then takes the shortest way to its exit and the values the code after it reads of the
// hit are the host's.
bool MatchPacketLaneWalk(Program& program, PacketWalk& w) {
	using Op = ValueOpcode;
	auto& s  = w.shape;
	if (s.blases.size() != 1) return false;
	const auto& blas = s.blases.front();
	const Inst* tri  = nullptr;
	for (auto at = BlockIndex(program, blas.header); at <= BlockIndex(program, blas.latch); ++at) {
		for (const auto& inst: *program.blocks[at]) {
			if (inst.GetOpcode() != Op::BvhIntersect) continue;
			if (tri != nullptr) return false;
			tri = &inst;
		}
	}
	const auto record = LoadOf(program, Canonical(blas.node->Arg(Incoming(*blas.node, blas.preheader))));
	if (tri == nullptr || !record) return false;
	const auto kind_of = [&](const Inst* value) -> std::optional<RayQueryResultKind> {
		if (value == record->low.TryInstruction()) return RayQueryResultKind::InstanceLo;
		if (value == record->high.TryInstruction()) return RayQueryResultKind::InstanceHi;
		if (value->GetOpcode() == Op::CompositeExtractU32x2 && value->Arg(0).Resolve().TryInstruction() == blas.load) {
			const auto word = Immediate(value->Arg(1));
			if (word == 0u) return RayQueryResultKind::Node;
			if (word == 1u) return RayQueryResultKind::Word;
			return std::nullopt;
		}
		return WrittenHitDistance(Value(const_cast<Inst*>(value)), tri);
	};
	const auto of_kind = [&](RayQueryResultKind kind) {
		return [&, kind](Value value) { return SourceKind(program, w, value, kind_of) == kind; };
	};
	LaneAlpha alpha;
	if (!DeriveLaneAlpha(program, w, blas, tri, *record, of_kind(RayQueryResultKind::Node), of_kind(RayQueryResultKind::Word),
	                     alpha))
		return false;
	// accept = lanes && flag != 0, lanes = facing && T <= t max.
	Value       lanes, direct;
	const auto  accept = FlaggedAccept(program, blas, alpha, RayQueryResultKind::T, kind_of, &lanes, &direct);
	const auto* within = !accept.IsEmpty() && !direct.IsEmpty() ? Producer(lanes, Op::LogicalAnd) : nullptr;
	Value       facing;
	for (uint32_t side = 0; within != nullptr && side < 2 && facing.IsEmpty(); ++side) {
		const auto* near = Producer(within->Arg(side), Op::FPOrdLessThanEqual32);
		const auto* t    = near != nullptr ? Canonical(near->Arg(0)).TryInstruction() : nullptr;
		if (t != nullptr && kind_of(t) == RayQueryResultKind::T &&
		    (Canonical(near->Arg(1)) == Canonical(s.ray[6]) || Canonical(near->Arg(1)) == Canonical(w.t_max)))
			facing = within->Arg(1 - side);
	}
	const auto* facing_lanes = facing.IsEmpty() ? nullptr : Producer(facing, Op::LogicalAnd);
	if (facing_lanes == nullptr) return false;
	// x of x != 0.
	const auto nonzero = [](const Inst* atom) -> Value {
		const auto pair = atom->GetOpcode() == Op::INotEqual32
		                      ? Operands(Value(const_cast<Inst*>(atom)), Op::INotEqual32, [](Value side) { return Immediate(side) == 0u; })
		                      : std::nullopt;
		return pair ? pair->second : Value {};
	};
	const auto atom = [](size_t k, uint32_t c) { return ((c >> k) & 1u) != 0; };
	// The opacity: the conditions of flag 1 and of the alpha test's lanes over the atoms off, opaque
	// and alpha tested.
	const auto* compare = alpha.flag->Arg(0).Resolve().TryInstruction();
	const auto* tested  = Producer(alpha.flag->Arg(0), Op::LogicalAnd);
	if (tested == nullptr) return false;
	const auto path = tested->Arg(0).Resolve().TryInstruction() == compare ? tested->Arg(1) : tested->Arg(0);
	LaneTable  opacity;
	opacity.exec            = lanes.TryInstruction();
	const auto direct_table = opacity.Condition(direct);
	const auto path_table   = opacity.Condition(path);
	if (!direct_table || !path_table) return false;
	const auto leaf_opacity = DeriveLeafOpacity(program, w, opacity.atoms, of_kind(RayQueryResultKind::Word));
	if (!leaf_opacity) return false;
	for (uint32_t c = 0; c < 8; ++c) {
		const bool taken = leaf_opacity->Taken(c);
		if (atom(c, *direct_table) != taken || atom(c, *path_table) == taken) return false;
	}
	constexpr size_t None      = 8;
	const auto       flags     = std::optional(leaf_opacity->flags);
	const auto       off_value = leaf_opacity->off_value;
	const auto raw = [&](Value value, uint32_t component) {
		const auto* extract = Producer(Canonical(value), Op::CompositeExtractU32x4);
		return extract != nullptr && extract->Arg(0).Resolve().TryInstruction() == tri && Immediate(extract->Arg(1)) == component;
	};
	// The facing: hit & (!A | f) & (!B | !f) over the atoms hit, f, A and B, the triangle lanes set.
	for (uint32_t side = 0; side < 2; ++side) {
		LaneTable  table;
		table.exec        = facing_lanes->Arg(side).Resolve().TryInstruction();
		const auto result = table.Condition(facing_lanes->Arg(1 - side));
		if (!result || table.atoms.size() != 4) continue;
		size_t                  hit = None, f = None;
		std::array<size_t, 2>   selector {None, None};
		std::array<Value, 2>    uniform {};
		std::array<uint32_t, 2> clear_bit {};
		uint32_t                facing_dword = 0, facing_bit = 0;
		bool                    flip = false, ok = true;
		for (size_t k = 0; k < table.atoms.size() && ok; ++k) {
			const auto* a  = table.atoms[k];
			const auto  op = a->GetOpcode();
			if ((op == Op::IEqual32 || op == Op::INotEqual32) && f == None) {
				for (uint32_t at = 0; at < 2 && f == None; ++at) {
					const auto* sign = Producer(Canonical(a->Arg(at)), Op::ShiftRightLogical32);
					const auto  bit  = sign != nullptr && Immediate(sign->Arg(1)) == 31u && raw(sign->Arg(0), 0)
					                       ? RecordBit(program, s, a->Arg(1 - at))
					                       : std::nullopt;
					if (bit && bit->second >= 3u) {
						f            = k;
						facing_dword = bit->first;
						facing_bit   = bit->second;
						flip         = op == Op::INotEqual32;
					}
				}
				if (f == k) continue;
			}
			const bool test = op == Op::IEqual32 || op == Op::INotEqual32 || op == Op::FPOrdNotEqual32 ||
			                  op == Op::FPUnordNotEqual32;
			if (test && hit == None && (raw(a->Arg(0), 0) || raw(a->Arg(1), 0))) {
				hit = k;
				continue;
			}
			// u & Select((flags & bit) == 0, 1, 0).
			const auto  value = nonzero(a);
			const auto* both  = !value.IsEmpty() ? Producer(Canonical(value), Op::BitwiseAnd32) : nullptr;
			const auto  slot  = selector[0] == None ? 0u : 1u;
			bool        found = false;
			for (uint32_t at = 0; both != nullptr && at < 2 && selector[1] == None && !found; ++at) {
				const auto  u     = both->Arg(at).Resolve();
				const auto* clear = Producer(both->Arg(1 - at), Op::SelectU32);
				const auto  zero  = clear != nullptr && Immediate(clear->Arg(1)) == 1u && Immediate(clear->Arg(2)) == 0u
				                        ? Operands(clear->Arg(0), Op::IEqual32, [](Value v) { return Immediate(v) == 0u; })
				                        : std::nullopt;
				const auto  bit   = zero ? RecordField(program, s, zero->second) : std::nullopt;
				if (!DefinedBefore(program, u, w.header) || !bit || std::popcount(bit->second) != 1) continue;
				selector[slot]  = k;
				uniform[slot]   = u;
				clear_bit[slot] = static_cast<uint32_t>(std::countr_zero(bit->second)) | bit->first << 8u;
				found           = true;
			}
			ok = found;
		}
		if (!ok || hit == None || f == None || selector[1] == None || clear_bit[0] != clear_bit[1] ||
		    clear_bit[0] != ((facing_bit - 1u) | facing_dword << 8u) || facing_dword != flags->first ||
		    facing_bit != flags->second + 3u)
			continue;
		// The values u: Select(x == 0, 1, 0) and Select(x != 0, 1, 0) of one x.
		std::array<std::optional<std::pair<Value, bool>>, 2> x;
		for (uint32_t k = 0; k < 2; ++k) {
			const auto* select = Producer(uniform[k], Op::SelectU32);
			const auto* cond   = select != nullptr && Immediate(select->Arg(1)) == 1u && Immediate(select->Arg(2)) == 0u
			                         ? select->Arg(0).Resolve().TryInstruction()
			                         : nullptr;
			if (cond == nullptr || (cond->GetOpcode() != Op::IEqual32 && cond->GetOpcode() != Op::INotEqual32)) continue;
			const auto pair = Operands(Value(const_cast<Inst*>(cond)), cond->GetOpcode(), [](Value v) { return Immediate(v) == 0u; });
			if (pair) x[k] = std::pair {pair->second.Resolve(), cond->GetOpcode() == Op::IEqual32};
		}
		if (!x[0] || !x[1] || x[0]->first != x[1]->first || x[0]->second == x[1]->second) continue;
		// Which selector takes the facing f (the other the flipped one).
		uint32_t first = 2;
		for (uint32_t order = 0; order < 2; ++order) {
			bool match = true;
			for (uint32_t c = 0; c < 16 && match; ++c) {
				const bool h = atom(hit, c), fa = atom(f, c), a = atom(selector[order], c), b = atom(selector[1 - order], c);
				match        = atom(c, *result) == (h && (!a || fa) && (!b || !fa));
			}
			if (match) first = order;
		}
		if (first == 2) continue;
		const auto found = HitResults(program, w, kind_of, accept);
		using Kind       = RayQueryResultKind;
		if (!found || !std::ranges::all_of(std::array {Kind::Word, Kind::InstanceLo, Kind::InstanceHi},
		                                   [&](Kind kind) { return (*found)[static_cast<uint32_t>(kind)]; }))
			return false;
		s.layout.flags_dword   = flags->first;
		s.layout.flags_shift   = flags->second;
		s.layout.flip_facing   = flip;
		s.layout.node_count_80 = TlasNodeCount80(program, s);
		s.source.layout        = s.layout.Pack();
		w.both_facings         = false;
		w.cull                 = Value(true);
		w.any_hit              = true;
		w.alpha                = alpha;
		w.alpha_off            = off_value;
		w.flip                 = Producer(uniform[1 - first], Op::SelectU32)->Arg(0).Resolve();
		return true;
	}
	return false;
}

// PacketWave without an alpha test (see MatchPacketWaveWalk): the accept condition of a lane masked write of
// the distance, lanes && (off || (opaque && !alpha tested)) with lanes the facing (`facing`) and the t max
// (in the lanes, or next to the opacity in 01.005: lanes && flag != 0 && !(T > t max)).
template <typename KindOf, typename IsWord, typename Facing>
bool MatchOpaqueWaveLeaf(Program& program, PacketWalk& w, const BlasWalk& loop, const Inst* tri, KindOf&& kind_of,
                         IsWord&& is_word, Facing&& facing) {
	auto&      s   = w.shape;
	const auto far = [&](const Inst* compare) -> std::optional<bool> { // Whether it holds near the ray's origin.
		const auto op = compare->GetOpcode();
		if (op != ValueOpcode::FPOrdLessThanEqual32 && op != ValueOpcode::FPOrdGreaterThan32) return std::nullopt;
		const auto* t     = Canonical(compare->Arg(0)).TryInstruction();
		const auto  bound = Canonical(compare->Arg(1));
		if (t == nullptr || kind_of(t) != RayQueryResultKind::T || (bound != Canonical(s.ray[6]) && bound != Canonical(w.t_max)))
			return std::nullopt;
		return op == ValueOpcode::FPOrdLessThanEqual32;
	};
	const auto near_in = [&](Value value, const auto& self, uint32_t depth) -> bool {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr || depth > 8) return false;
		if (inst->GetOpcode() == ValueOpcode::LogicalAnd)
			return self(inst->Arg(0), self, depth + 1) || self(inst->Arg(1), self, depth + 1);
		return far(inst) == true;
	};
	for (auto at = BlockIndex(program, loop.header); at <= BlockIndex(program, loop.latch); ++at) {
		for (const auto& inst: *program.blocks[at]) {
			if (inst.GetOpcode() != ValueOpcode::SelectU32) continue;
			const auto* written = Canonical(inst.Arg(1)).TryInstruction();
			const auto* both    = Producer(inst.Arg(0), ValueOpcode::LogicalAnd);
			if (written == nullptr || kind_of(written) != RayQueryResultKind::T || both == nullptr) continue;
			const auto accept = inst.Arg(0).Resolve();
			for (uint32_t side = 0; side < 2; ++side) {
				const auto lanes = both->Arg(side).Resolve();
				if (lanes.TryInstruction() == nullptr || !facing(lanes)) continue;
				LaneTable table;
				table.exec  = lanes.TryInstruction();
				table.flags = true;
				auto taken  = table.Condition(accept);
				// (The t max next to the opacity: no hit beyond it, the table of the other atoms near it.)
				const auto at = std::ranges::find_if(table.atoms, [&](const Inst* atom) { return far(atom).has_value(); });
				if (taken && at != table.atoms.end()) {
					const auto k    = static_cast<uint32_t>(at - table.atoms.begin());
					const bool near = *far(*at);
					const auto n    = static_cast<uint32_t>(table.atoms.size());
					uint32_t   rest = 0;
					bool       none = true;
					for (uint32_t c = 0; c < (1u << n); ++c) {
						const bool bit = ((c >> k) & 1u) != 0, value = ((*taken >> c) & 1u) != 0;
						if (bit != near) {
							none &= !value;
						} else if (value) {
							const uint32_t low = c & ((1u << k) - 1u), high = (c >> (k + 1u)) << k;
							for (uint32_t r = low | high; r < 32u; r += 1u << (n - 1u)) rest |= 1u << r;
						}
					}
					if (!none) continue;
					table.atoms.erase(at);
					taken = rest;
				} else if (!near_in(lanes, near_in, 0)) {
					continue;
				}
				if (!taken) continue;
				const auto opacity = DeriveLeafOpacity(program, w, table.atoms, is_word);
				if (!opacity || opacity->flags != std::pair {s.layout.flags_dword, s.layout.flags_shift}) continue;
				bool same = true;
				for (uint32_t c = 0; c < 8; ++c)
					same &= (((*taken >> c) & 1u) != 0) == opacity->Taken(c);
				if (!same) continue;
				const auto found = HitResults(program, w, kind_of, accept);
				using Kind       = RayQueryResultKind;
				if (!found || !std::ranges::all_of(std::array {Kind::Word, Kind::InstanceLo, Kind::InstanceHi},
				                                   [&](Kind kind) { return (*found)[static_cast<uint32_t>(kind)]; }))
					continue;
				s.layout.node_count_80 = TlasNodeCount80(program, s);
				s.source.layout        = s.layout.Pack();
				w.any_hit              = true;
				w.alpha_off            = opacity->off_value;
				return true;
			}
		}
	}
	return false;
}

// PacketWave (GTA V's Fidelity, cs 0b41af19bddf132c (01.011) and a31ec6a25be5e017 (01.005)): a packet walk
// whose BLAS walk is the wave's (as ClosestWave) with leaf code that takes any hit, per lane, of the
// triangle test `tri` of the facing the instance flags select (LeafFacing), not beyond the t max, with
// flag != 0: a phi of a value the same in every lane (the flag 1 of PacketLane's opacity, off | (opaque &
// !alpha tested)) and, from the way the leaf code takes where that value is 0, of the flag of the scalar
// alpha test (DeriveLaneAlpha with scalar loads). Without an alpha test (cs 2f34bc2dfe6b04a3 (01.011) and
// c7795cb2906bd593 (01.005)) the leaf code takes the triangles of that opacity only: accept = lanes &&
// (off || (opaque && !alpha tested)), off per lane. The accept condition writes the distance, the
// primitive word and the instance record address. The host traverses before the walk as for PacketLane
// (the opacity of the instance flags, every instance opaque where off holds, the facing of the instance
// flags) with the scalar alpha test, or with no hit in the triangles that are not opaque; the walk then
// takes the shortest way to its exit and the values the code after it reads of the hit are the host's.
bool MatchPacketWaveWalk(Program& program, PacketWalk& w) {
	using Op = ValueOpcode;
	auto& s  = w.shape;
	if (!w.walked || s.wave_triangles == nullptr || !s.blases.empty()) return false;
	const Inst* tri     = s.wave_triangles;
	const auto  kind_of = [&](const Inst* value) { return WaveResultKind(program, s, tri, value); };
	const auto  of_kind = [&](RayQueryResultKind kind) {
		return [&, kind](Value value) { return SourceKind(program, w, value, kind_of) == kind; };
	};
	BlasWalk loop;
	loop.header = s.wave_header;
	loop.latch  = s.wave_latch;
	LaneAlpha alpha;
	alpha.scalar = true;
	if (WaveBlasBase(tri) == nullptr) return false;
	const auto facing = [&](Value lanes) {
		for (const bool flags: {false, true}) {
			if (LeafFacing(program, w, lanes, false, tri, Canonical(s.ray[6]), kind_of, flags) ||
			    LeafFacing(program, w, lanes, false, tri, Canonical(w.t_max), kind_of, flags))
				return true;
		}
		return false;
	};
	if (!DeriveLaneAlpha(program, w, loop, tri, s.record, of_kind(RayQueryResultKind::Node), of_kind(RayQueryResultKind::Word),
	                     alpha))
		return MatchOpaqueWaveLeaf(program, w, loop, tri, kind_of, of_kind(RayQueryResultKind::Word), facing);
	// accept = lanes && flag != 0, lanes = facing && T <= t max.
	Value      lanes;
	const auto accept = FlaggedAccept(program, loop, alpha, RayQueryResultKind::T, kind_of, &lanes);
	if (accept.IsEmpty() || !facing(lanes) || !w.cull.IsImmediate()) return false;
	// The flag's phi: the alpha test's flag from one block, the value (what the lanes write) from the
	// block that branches on it: to the alpha test where it is 0.
	const Inst* phi  = nullptr;
	const auto* both = Producer(accept, Op::LogicalAnd);
	for (uint32_t side = 0; both != nullptr && side < 2 && phi == nullptr; ++side) {
		const auto pair = Operands(both->Arg(side), Op::INotEqual32, [](Value v) { return Immediate(v) == 0u; });
		if (pair) phi = Producer(pair->second, Op::Phi);
	}
	if (phi == nullptr || phi->NumArgs() != 2) return false;
	const auto written = [&](Value value) {
		for (const Inst* select = nullptr; (select = Producer(value, Op::SelectU32)) != nullptr &&
		                                   select->Arg(0).Resolve() == lanes;)
			value = select->Arg(1).Resolve();
		return value.Resolve();
	};
	const auto alpha_arm = written(phi->Arg(0)).TryInstruction() == alpha.flag ? 0u : 1u;
	const auto value     = written(phi->Arg(1 - alpha_arm));
	auto*      split     = phi->PhiBlock(1 - alpha_arm);
	const auto* tested   = phi->PhiBlock(alpha_arm);
	if (written(phi->Arg(alpha_arm)).TryInstruction() != alpha.flag ||
	    split->terminator.kind != CFG::TerminatorKind::ConditionalBranch || split->terminator.merge_block != phi->Parent())
		return false;
	// (The alpha test's way: blocks of one predecessor from the split.)
	while (tested != nullptr && tested->ImmPredecessors().size() == 1 && tested->ImmPredecessors()[0] != split)
		tested = tested->ImmPredecessors()[0];
	const auto& t = split->terminator;
	if (tested == nullptr || tested->ImmPredecessors().size() != 1 || (tested != t.true_block && tested != t.false_block) ||
	    (t.true_block != phi->Parent() && t.false_block != phi->Parent()))
		return false;
	const auto [ref, negated] = BranchCondition(*split);
	const auto tests          = ref != nullptr ? Operands(ref->Arg(0), Op::INotEqual32, [](Value v) { return Immediate(v) == 0u; })
	                                           : std::nullopt;
	if (!tests || Canonical(tests->second) != Canonical(value) || (tested == t.true_block) != negated) return false;
	// The value: flag 1 of the opacity (the lanes are set).
	LaneTable table;
	table.exec       = lanes.TryInstruction();
	const auto taken = table.Flag(value);
	if (!taken) return false;
	const auto opacity = DeriveLeafOpacity(program, w, table.atoms, of_kind(RayQueryResultKind::Word));
	if (!opacity || opacity->flags != std::pair {s.layout.flags_dword, s.layout.flags_shift}) return false;
	for (uint32_t c = 0; c < 8; ++c) {
		if ((((*taken >> c) & 1u) != 0) != opacity->Taken(c)) return false;
	}
	const auto found = HitResults(program, w, kind_of, accept);
	using Kind       = RayQueryResultKind;
	if (!found || !std::ranges::all_of(std::array {Kind::Word, Kind::InstanceLo, Kind::InstanceHi},
	                                   [&](Kind kind) { return (*found)[static_cast<uint32_t>(kind)]; }))
		return false;
	s.layout.node_count_80 = TlasNodeCount80(program, s);
	s.source.layout        = s.layout.Pack();
	w.any_hit              = true;
	w.alpha                = alpha;
	w.alpha_off            = opacity->off_value;
	return true;
}

void RewriteHitWalk(Program& program, PacketWalk& w, uint32_t site, bool entry_alpha) {
	using Op         = ValueOpcode;
	const auto flags = [site](auto variant) {
		return RayQueryFlags(site, static_cast<uint32_t>(variant));
	};
	const auto& s = w.shape;
	// Before the walk: the alpha test's arguments (Entry's tables, or the Lane or Scalar test's values), the
	// queries (both facings; opacity from the geometry and the instance flags (Entry) or the
	// geometry (Lane), or every triangle opaque), and their traversal to the end (the alpha tested
	// candidates are the host's; no candidate is left to the guest).
	const Inserter entry {w.preheader, w.preheader->end()};
	const auto&    lane = w.alpha;
	if (lane.valid) {
		// (The scalar test takes threshold * (M0 bit 0 ? factor : 1); one whose reference is threshold * factor
		// for every material, that product and a factor of 1.)
		const auto reference =
		    lane.factor_always
		        ? entry(Op::BitCastU32F32, {entry(Op::FPMul32, {Value::F32(std::bit_cast<float>(lane.reference.U32())),
		                                                        entry(Op::BitCastF32U32, {lane.factor})})})
		        : lane.reference;
		const auto before = [&](Value value) { return DescriptorBefore(program, value, w.header, entry); };
		entry(Op::RayQueryAlpha,
		      {lane.lod, lane.spread, lane.factor.IsEmpty() || lane.factor_always ? Value(One) : lane.factor,
		       before(lane.material[0]), before(lane.material[1]), before(lane.material[2]), before(lane.material[3]),
		       before(lane.buffers[0]), before(lane.buffers[1]), before(lane.buffers[2]), before(lane.buffers[3]),
		       reference, Value(lane.lod_offset), Value(lane.fields.Pack(s.layout))},
		      flags(lane.scalar ? RayQueryAlphaScalar : RayQueryAlphaLane | (lane.factor.IsEmpty() ? 0u : RayQueryAlphaFactor)));
	}
	if (entry_alpha) {
		RayTracingAlphaFields fields;
		fields.material_dword = 0x70 / 4;
		entry(Op::RayQueryAlpha,
		      {Value(0u), Value(0u), Value(One), w.entries[0], w.entries[1], w.entries[2], w.entries[3],
		       w.buffers[0], w.buffers[1], w.buffers[2], w.buffers[3], Value(std::bit_cast<uint32_t>(0.5f)),
		       Value(std::bit_cast<uint32_t>(2.0f)), Value(fields.Pack(s.layout))},
		      flags(RayQueryAlphaEntry));
	}
	// (Any hit: the opacity of the instance flags, which `alpha_off` sets; the facing flipped where
	// `flip` holds; the guest's walk where the alpha test's level condition is 0.)
	const auto nonzero = [&](Value value, bool otherwise) {
		return value.IsEmpty() ? Value(otherwise) : entry(Op::INotEqual32, {value, Value(0u)});
	};
	const auto alpha_off = w.any_hit ? w.alpha_off : Value(entry_alpha || lane.valid ? 0u : 1u);
	const auto native =
	    entry(Op::RayQueryInit,
	          {s.ray[0], s.ray[1], s.ray[2], s.ray[3], s.ray[4], s.ray[5], w.t_max, RayMask(s), s.tlas[0], s.tlas[1], alpha_off,
	           w.lanes, w.cull, w.flip.IsEmpty() ? Value(false) : w.flip, nonzero(lane.level, true)},
	          flags((w.both_facings ? RayQueryInitBothFacings : 0u) | (entry_alpha || w.any_hit ? RayQueryInitNoLeaf : 0u) |
	                (lane.valid && !w.any_hit ? RayQueryInitGeometryOpacity : 0u) |
	                (w.any_hit && !lane.valid ? RayQueryInitOpaqueOnly : 0u)));
	const auto ballot = entry(Op::Ballot, {native});
	const auto any =
	    entry(Op::INotEqual32,
	          {entry(Op::BitwiseOr32, {entry(Op::CompositeExtractU32x4, {ballot, Value(0u)}),
	                                   entry(Op::CompositeExtractU32x4, {ballot, Value(1u)})}),
	           Value(0u)});
	entry(Op::RayQueryNext, {}, flags(0u));
	// The walk takes the way to its exit.
	for (size_t at = 0; at < w.path.size(); ++at) {
		auto* block = w.path[at];
		if (block->terminator.kind != CFG::TerminatorKind::ConditionalBranch) continue;
		const auto [ref, negated] = BranchCondition(*block);
		const auto*    next       = at + 1 < w.path.size() ? w.path[at + 1] : w.merge;
		const Inserter in {ref->Parent(), Position(*ref)};
		ref->SetArg(0, in(Op::SelectU1, {any, Value((next == block->terminator.true_block) != negated), ref->Arg(0)}));
	}
	// After it: the host's hit, when nearer than the t max.
	const Inserter after {w.merge, AfterPhis(*w.merge)};
	const auto     result = [&](RayQueryResultKind kind) { return after(Op::RayQueryResult, {}, flags(kind)); };
	const auto     has    = after(Op::INotEqual32, {result(RayQueryResultKind::Has), Value(0u)});
	// (Any hit: every hit the host finds is one.)
	const auto     commit = w.any_hit ? has
	                                  : after(Op::LogicalAnd, {has, after(Op::FPOrdLessThanEqual32,
	                                                                      {after(Op::BitCastF32U32, {result(RayQueryResultKind::T)}),
	                                                                       after(Op::BitCastF32U32, {w.t_max})})});
	for (auto& [inst, kind]: w.results) {
		const auto value  = result(kind);
		const auto select = after(Op::SelectU32, {commit, value, Value(inst)});
		const auto uses   = inst->Uses();
		for (const auto& use: uses) {
			if (use.user != select.Instruction() && !InLoop(program, use.user->Parent(), w.header, w.latch))
				use.user->SetArg(use.operand, select);
		}
	}
	after(Op::RayQueryMark, {}, flags(RayQueryMarkKind::Exit));
}

bool MatchClosestLaneWalk(Program& program, const NativeTraversalOptions& options, PacketWalk& w) {
	auto& s = w.shape;
	if (options.wave_size != 32 || w.header == s.header || !s.masked || s.blases.empty()) return false;
	const auto from = [](const Inst& phi, const Block* block) { return phi.Arg(Incoming(phi, block)); };
	// The outer loop's node: the root, then a node popped from the packet's stack (read from one of
	// its lanes).
	w.outer_node = LoopPhi(from(*s.node, s.preheader), w.header);
	if (w.outer_node == nullptr || Immediate(from(*w.outer_node, w.preheader)) != s.source.root)
		return false;
	const auto* pop = Producer(from(*w.outer_node, w.latch), ValueOpcode::Phi);
	if (pop == nullptr) return false;
	for (size_t arg = 0; arg < pop->NumArgs(); ++arg) {
		if (Producer(pop->Arg(arg), ValueOpcode::ReadLane) == nullptr) return false;
	}
	// The round's end: the inner loop's merge, whose test of the stack leaves the walk (before the
	// pop).
	const auto* exit = w.path.back();
	if (exit != s.merge || exit->terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
	    (exit->terminator.true_block != w.merge && exit->terminator.false_block != w.merge) ||
	    BlockIndex(program, pop->Parent()) <= BlockIndex(program, exit))
		return false;
	// The packet: the inner loop's lanes, those the box test hits after one; when a round starts,
	// the lanes of the trace.
	for (auto& inst: *s.header) {
		if (inst.GetOpcode() != ValueOpcode::Phi || inst.GetType() != Type::U32 || &inst == s.node)
			continue;
		std::unordered_set<const Inst*> values;
		Collect(from(inst, s.latch), s.header, values);
		if (std::ranges::none_of(values, [&](const Inst* value) {
			    return value->Parent() == s.bvh->Parent() &&
			           value->GetOpcode() == ValueOpcode::CompositeExtractU32x4 &&
			           Immediate(value->Arg(1)) == 0u && Producer(value->Arg(0), ValueOpcode::Ballot) != nullptr;
		    }))
			continue;
		if (w.packet != nullptr) return false;
		w.packet = &inst;
	}
	if (w.packet == nullptr || !DefinedBefore(program, from(*w.packet, s.preheader), w.header))
		return false;
	// The instance code: the side of a branch of the way that leads to the BLAS walk; it ends the
	// round (it reaches the round's end and not the inner loop's latch).
	const auto first = BlockIndex(program, s.header), last = BlockIndex(program, s.latch);
	const auto successors = [](const Block* block) -> std::vector<Block*> {
		const auto& term  = block->terminator;
		const auto  taken = block->condition;
		if (term.kind == CFG::TerminatorKind::ConditionalBranch && taken.IsImmediate() &&
		    taken.GetType() == Type::U1)
			return {taken.U1() ? term.true_block : term.false_block};
		const auto next = block->ImmSuccessors();
		return {next.begin(), next.end()};
	};
	// The blocks forward from `start` in the inner loop, and whether the round's end follows one.
	const auto forward = [&](Block* start, std::vector<const Block*>& seen) {
		bool                ends = false;
		std::vector<Block*> queue {start};
		seen = {start};
		for (size_t at = 0; at < queue.size(); ++at) {
			for (auto* next: successors(queue[at])) {
				const auto index = BlockIndex(program, next);
				if (next == exit) {
					ends = true;
					continue;
				}
				if (index <= BlockIndex(program, queue[at]) || index < first || index > last ||
				    std::ranges::find(seen, next) != seen.end())
					continue;
				seen.push_back(next);
				queue.push_back(next);
			}
		}
		return ends;
	};
	const auto& blas = s.blases.front();
	Block*      side = nullptr;
	for (const auto* block: w.path) {
		const auto index = BlockIndex(program, block);
		if (index < first || index > last) continue;
		for (auto* next: block->ImmSuccessors()) {
			std::vector<const Block*> seen;
			if (std::ranges::find(w.path, next) != w.path.end()) continue;
			forward(next, seen);
			if (std::ranges::find(seen, blas.header) == seen.end()) continue;
			if (side != nullptr && side != next) return false;
			side = next;
		}
	}
	std::vector<const Block*> instance;
	return side != nullptr && forward(side, instance) &&
	       std::ranges::find(instance, s.latch) == instance.end();
}

void RewriteClosestLaneWalk(Program& program, PacketWalk& w, uint32_t site) {
	using Op             = ValueOpcode;
	const auto flags     = [site](auto variant) {
		return RayQueryFlags(site, static_cast<uint32_t>(variant));
	};
	const auto lane_leaf = flags(RayQueryKeyLaneLeaf);
	auto&      s         = w.shape;
	// Before the walk: the queries (every triangle a candidate of the leaf code, both facings) and
	// the first instance.
	const Inserter entry {w.preheader, w.preheader->end()};
	const auto     native =
	    entry(Op::RayQueryInit,
	          {s.ray[0], s.ray[1], s.ray[2], s.ray[3], s.ray[4], s.ray[5], w.t_max, RayMask(s),
	           s.tlas[0], s.tlas[1], Value(0u), w.lanes, Value(true), Value(false), Value(true)},
	          flags(RayQueryInitBothFacings | RayQueryInitAllCandidates));
	const auto ballot = entry(Op::Ballot, {native});
	const auto any =
	    entry(Op::INotEqual32,
	          {entry(Op::BitwiseOr32, {entry(Op::CompositeExtractU32x4, {ballot, Value(0u)}),
	                                   entry(Op::CompositeExtractU32x4, {ballot, Value(1u)})}),
	           Value(0u)});
	const auto first = entry(Op::RayQueryNext, {}, lane_leaf);
	const auto start = Incoming(*w.outer_node, w.preheader);
	w.outer_node->SetArg(start, entry(Op::SelectU32, {any, entry(Op::CompositeExtractU32x4, {first, Value(0u)}),
	                                                 w.outer_node->Arg(start)}));
	// Each round's packet: the lanes with a leaf of its instance.
	const Inserter round {s.preheader, s.preheader->end()};
	const auto     match  = round(Op::RayQueryMatch, {}, lane_leaf);
	const auto     lanes  = round(Op::CompositeExtractU32x4, {round(Op::Ballot, {match}), Value(0u)});
	const auto     packet = Incoming(*w.packet, s.preheader);
	w.packet->SetArg(packet, round(Op::SelectU32, {any, lanes, w.packet->Arg(packet)}));
	// The round's end: the lanes of the instance are done with their leaf (closest hit: they go on
	// searching); the next instance, or the exit.
	auto*          exit           = w.path.back();
	const auto [ref, negated]     = BranchCondition(*exit);
	const Inserter at {exit, Position(*ref)};
	at(Op::RayQueryMark, {}, flags(RayQueryMarkKind::Resolve));
	const auto next     = at(Op::RayQueryNext, {}, lane_leaf);
	const auto instance = at(Op::CompositeExtractU32x4, {next, Value(0u)});
	const auto has = at(Op::INotEqual32, {at(Op::CompositeExtractU32x4, {next, Value(2u)}), Value(0u)});
	const bool leaves_on_true = exit->terminator.true_block == w.merge;
	ref->SetArg(0, at(Op::SelectU1, {any, leaves_on_true != negated ? at(Op::LogicalNot, {has}) : has,
	                                 ref->Arg(0)}));
	const Inserter back {w.latch, AfterPhis(*w.latch)};
	const auto     index = Incoming(*w.outer_node, w.latch);
	w.outer_node->SetArg(index, back(Op::SelectU32, {any, instance, w.outer_node->Arg(index)}));
	const Inserter leave {w.merge, AfterPhis(*w.merge)};
	leave(Op::RayQueryMark, {}, flags(RayQueryMarkKind::Exit));
	RewriteBlasWalks(program, s.blases, any, lane_leaf);
}

} // namespace

NativeTraversals TraceNativeTraversals(Program& program, const NativeTraversalOptions& options) {
	NativeTraversals result;
	if (program.stage != ShaderType::Compute) return result;
	std::vector<Inst*> intersections;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::BvhIntersect) intersections.push_back(&inst);
		}
	}
	// The intersections of the walks it rewrites, which keep the guest's loop.
	const auto mark = [](Inst* bvh) {
		bvh->SetFlags(bvh->Flags<uint32_t>() | BvhIntersectNative);
	};
	enum class Kind { Entry, ClosestLane, ClosestOpaque, ClosestLeaf, PacketLane, LaneMask };
	struct Walk {
		Kind         kind = Kind::LaneMask;
		PacketWalk   packet;
		LaneMaskWalk lane;
		Block*       header = nullptr;
		Block*       latch  = nullptr;

		LaneMaskWalk& Shape() { return kind == Kind::LaneMask ? lane : packet.shape; }
	};
	std::vector<Walk> walks;
	const auto        inside = [&](const Block* block) {
		return std::ranges::any_of(walks, [&](const Walk& walk) {
			return InLoop(program, block, walk.header, walk.latch);
		});
	};
	for (auto* bvh: intersections) {
		if (inside(bvh->Parent())) continue;
		Walk walk;
		bool found = false;
		if (MatchPacketWalk(program, *bvh, walk.packet)) {
			walk.header = walk.packet.header;
			walk.latch  = walk.packet.latch;
			if (MatchEntryWalk(program, options, walk.packet)) {
				walk.kind = Kind::Entry;
				found     = true;
			} else if (MatchClosestLaneWalk(program, options, walk.packet)) {
				walk.kind = Kind::ClosestLane;
				found     = true;
			} else if (MatchClosestOpaqueWalk(program, walk.packet) || MatchClosestWaveWalk(program, walk.packet)) {
				walk.kind = walk.packet.leaf ? Kind::ClosestLeaf : Kind::ClosestOpaque;
				found     = !walk.packet.leaf || MatchClosestLeafWalk(program, walk.packet);
			} else if (MatchPacketLaneWalk(program, walk.packet) || MatchPacketWaveWalk(program, walk.packet)) {
				walk.kind = Kind::PacketLane;
				found     = true;
			}
		}
		if (!found && options.wave_size == 32 && MatchLaneMaskWalk(program, *bvh, walk.lane)) {
			walk.kind   = Kind::LaneMask;
			walk.header = walk.lane.header;
			walk.latch  = walk.lane.latch;
			found       = true;
		}
		if (found) walks.push_back(std::move(walk));
	}
	if (walks.empty()) return result;
	// The walks read one TLAS, with one layout. A walk whose ray mask is a constant that selects the
	// instance mask's bits at a higher shift of the same record dword (Marvel's Wolverine, cs
	// 1a04c58a9fbc2052: 0x68000000 for the closest hit walk, 0x30000000 for the any hit walk) takes
	// the lowest shift, with the constant moved up.
	auto reference = walks.front().Shape().source;
	auto lowest    = walks.front().Shape().layout.mask_shift;
	for (auto& walk: walks) {
		if (walk.Shape().layout.mask_shift < lowest) {
			lowest    = walk.Shape().layout.mask_shift;
			reference = walk.Shape().source;
		}
	}
	const auto shares = [&](LaneMaskWalk& walk) {
		const auto& source = walk.source;
		if (source.user_sgpr != reference.user_sgpr || source.header_offset != reference.header_offset ||
		    source.header_vsharp != reference.header_vsharp || source.root != reference.root) {
			return false;
		}
		if (source.layout == reference.layout) return true;
		const auto     packed = reference.layout[2];
		const uint32_t shift  = packed >> 4u & 31u;
		auto           layout = walk.layout;
		if ((packed >> 19u & 1u) == 0 || (packed >> 25u & 31u) != 0 || layout.mask_width != 0 || !walk.mask_value.IsEmpty() ||
		    layout.mask_shift <= shift || (walk.cull_mask << (layout.mask_shift - shift)) > 0xffu) {
			return false;
		}
		const auto moved  = layout.mask_shift - shift;
		layout.mask_shift = shift;
		if (layout.Pack() != reference.layout) return false;
		walk.cull_mask <<= moved;
		walk.layout        = layout;
		walk.source.layout = reference.layout;
		return true;
	};
	std::erase_if(walks, [&](Walk& walk) { return !shares(walk.Shape()); });
	if (walks.empty()) return result;
	result       = walks.front().Shape().source;
	result.reach = {};
	// The reach covers the BVH reads only when every intersection is in a walk that traces natively.
	const auto sites   = options.rewrite ? std::min<size_t>(walks.size(), MaxRayQuerySites) : 0;
	const auto covered = sites == walks.size() && std::ranges::all_of(intersections, [&](const Inst* other) {
		                     return inside(other->Parent());
	                     });
	for (auto& walk: walks) {
		if (covered) {
			const auto& reach = walk.Shape().source.reach;
			result.reach.insert(result.reach.end(), reach.begin(), reach.end());
		}
	}
	for (size_t at = 0; at < sites; ++at) {
		auto&      walk = walks[at];
		const auto site = static_cast<uint32_t>(at);
		switch (walk.kind) {
			case Kind::Entry: RewriteHitWalk(program, walk.packet, site, true); break;
			case Kind::ClosestOpaque:
			case Kind::PacketLane: RewriteHitWalk(program, walk.packet, site, false); break;
			case Kind::ClosestLane: RewriteClosestLaneWalk(program, walk.packet, site); break;
			case Kind::ClosestLeaf: RewriteClosestLeafWalk(program, walk.packet, site); break;
			case Kind::LaneMask: RewriteLaneMaskWalk(program, walk.lane, site); break;
		}
		++result.sites;
		for (auto* bvh: intersections) {
			if (InLoop(program, bvh->Parent(), walk.header, walk.latch)) mark(bvh);
		}
	}
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
