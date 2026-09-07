#include "graphics/shader/recompiler/ComputeExecution.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler {
namespace {
using O = IR::ValueOpcode;

bool IsPureUniformOperation(O op) {
	switch (op) {
		case O::BitCastU16F16: case O::BitCastF16U16: case O::BitCastU32F32: case O::BitCastF32U32:
		case O::ConvertU16U32: case O::ConvertU32U16: case O::ConvertU8U32: case O::ConvertU32U8:
		case O::ConvertF32F16: case O::ConvertF16F32: case O::ConvertS32F32: case O::ConvertU32F32:
		case O::ConvertF64S32: case O::ConvertF64U32: case O::ConvertF32F64:
		case O::CompositeConstructF64:
		case O::FPAbs64:
		case O::FPNeg64:
		case O::FPMul64:
		case O::FPFma64:
		case O::FPRecip64:
		case O::CompositeExtractF64:
		case O::ConvertF32S32: case O::ConvertF32U32: case O::CompositeConstructU64: case O::CompositeConstructU32x2:
		case O::CompositeConstructU32x3: case O::CompositeConstructF32x2: case O::CompositeConstructU32x4: case O::CompositeExtractU64:
		case O::CompositeExtractU32x2: case O::CompositeExtractU32x3: case O::CompositeExtractU32x4: case O::PackHalf2x16:
		case O::PackSnorm2x16: case O::PackUnorm2x16: case O::PackFloat2x16Rtz: case O::FPAbs32:
		case O::FPNeg32: case O::FPSaturate32: case O::BitFieldInsert: case O::BitFieldUExtract:
		case O::BitFieldSExtract: case O::WqmU64: case O::SelectU1: case O::SelectF32:
		case O::IAdd32: case O::IAdd64: case O::IAddCarry32: case O::ISub32:
		case O::ISub64: case O::IMul32: case O::IMul64: case O::UDiv32:
		case O::SMulHi: case O::UMulHi: case O::IAbs32: case O::ShiftLeftLogical32:
		case O::ShiftLeftLogical64: case O::ShiftRightLogical32: case O::ShiftRightLogical64: case O::ShiftRightArithmetic32:
		case O::ShiftRightArithmetic64: case O::BitwiseAnd32: case O::BitwiseAnd64: case O::BitwiseOr32:
		case O::BitwiseXor32: case O::BitwiseNot32: case O::BitReverse32: case O::BitCount32:
		case O::BitCount64: case O::FindUMsb32: case O::FindUMsb64: case O::FindILsb32:
		case O::SMin32: case O::UMin32: case O::SMax32: case O::UMax32:
		case O::SMinTri32: case O::UMinTri32: case O::SMaxTri32: case O::UMaxTri32:
		case O::SMedTri32: case O::UMedTri32: case O::SLessThan32: case O::SLessThan64:
		case O::ULessThan32: case O::ULessThan64: case O::IEqual32: case O::IEqual64:
		case O::SLessThanEqual32: case O::ULessThanEqual32: case O::SGreaterThan32: case O::UGreaterThan32:
		case O::UGreaterThan64: case O::INotEqual32: case O::INotEqual64: case O::SGreaterThanEqual32:
		case O::UGreaterThanEqual32: case O::LogicalOr: case O::LogicalAnd: case O::LogicalXor:
		case O::LogicalNot: case O::FPOrdEqual32: case O::FPUnordEqual32: case O::FPOrdNotEqual32:
		case O::FPUnordNotEqual32: case O::FPOrdLessThan32: case O::FPUnordLessThan32: case O::FPOrdGreaterThan32:
		case O::FPUnordGreaterThan32: case O::FPOrdLessThanEqual32: case O::FPUnordLessThanEqual32: case O::FPOrdGreaterThanEqual32:
		case O::FPUnordGreaterThanEqual32: case O::FPIsNan32: case O::FPCmpClass32: case O::FPAdd32:
		case O::FPSub32: case O::FPFma32: case O::FPMul32: case O::FPMin32:
		case O::FPMax32: case O::FPMinTri32: case O::FPMaxTri32: case O::FPMedTri32:
		case O::FPRecip32: case O::FPRecipIFlag32: case O::FPRecipSqrt32: case O::FPSqrt:
		case O::FPSin: case O::FPCos: case O::FPExp2: case O::FPLog2:
		case O::FPLdexp: case O::FPRoundEven32: case O::FPFloor32: case O::FPCeil32:
		case O::FPTrunc32: case O::FPFract32: case O::Phi: case O::Identity:
		case O::GetSrtResource: case O::GetBufferResource: case O::GetAddressResource: case O::GetScratchResource:
		case O::GetImageResource: case O::GetSamplerResource: case O::MakeImageAddress: case O::ReadConst:
		case O::ReadBoundedSrtU32:
		case O::SelectU32:
			return true;
		default: return false;
	}
}

bool IsGuestRead(O op) {
	return op == O::ReadConstBuffer || IR::BufferAccessOf(op) == IR::BufferAccess::Read ||
	       IR::AddressOpcodeInfoOf(op).access == IR::AddressAccess::Read ||
	       IR::ImageOpcodeInfoOf(op).access == IR::ImageAccess::Read;
}
bool IsGuestWrite(O op) {
	return IR::BufferAccessOf(op) == IR::BufferAccess::Write ||
	       IR::AddressOpcodeInfoOf(op).access == IR::AddressAccess::Write ||
	       IR::ImageOpcodeInfoOf(op).access == IR::ImageAccess::Write;
}
bool IsGuestAtomic(O op) {
	return IR::BufferAccessOf(op) == IR::BufferAccess::Atomic ||
	       IR::ImageOpcodeInfoOf(op).access == IR::ImageAccess::Atomic;
}

bool IsSupportedSplitOperation(O op) {
	if (IsPureUniformOperation(op) || IR::BufferAccessOf(op) != IR::BufferAccess::None ||
	    IR::AddressOpcodeInfoOf(op).access != IR::AddressAccess::None ||
	    IR::ImageOpcodeInfoOf(op).access != IR::ImageAccess::None ||
	    IR::SharedAccessOf(op) == IR::SharedAccess::Read ||
	    IR::SharedAccessOf(op) == IR::SharedAccess::Write) return true;
	switch (op) {
		case O::Void: case O::Reference: case O::ReferenceU32:
		case O::GetUserData: case O::GetShaderBase: case O::GetBuiltin:
		case O::UndefU1: case O::UndefU8: case O::UndefU16: case O::UndefU32: case O::UndefU64:
		case O::LaneId: case O::Ballot: case O::ReadLane: case O::ReadFirstLane: case O::WriteLane:
		case O::WqmMask: case O::DppMoveU32: case O::DppUpdateU32:
		case O::Dpp8MoveU32: case O::Dpp8UpdateU32: case O::Permlane16U32:
		case O::SwizzleU32: case O::BpermuteU32: case O::DataAppend:
		case O::ControlNop: case O::Waitcnt: case O::Barrier: return true;
		default: return false;
	}
}

bool HasGuestLdsAccess(const IR::Program& program) {
	for (const auto* block : program.blocks) for (const auto& inst : *block) {
		if (IR::SharedAccessOf(inst.GetOpcode()) == IR::SharedAccess::None) continue;
		const auto index = inst.Flags<IR::MemoryFlags>().index;
		if (index < program.memory_info.size() && program.memory_info[index].kind == IR::ResourceKind::Lds)
			return true;
	}
	return false;
}

bool HasWaveOperations(const IR::Program& program) {
	if (program.spirv_requirements && (program.spirv_requirements->subgroup_ballot ||
	    program.spirv_requirements->subgroup_shuffle || program.spirv_requirements->subgroup_local_invocation_id)) return true;
	for (const auto* block : program.blocks) for (const auto& inst : *block) {
		switch (inst.GetOpcode()) {
			case O::Ballot: case O::LaneId: case O::ReadLane: case O::ReadFirstLane:
			case O::WriteLane: case O::WqmMask: case O::DppMoveU32: case O::DppUpdateU32:
			case O::Dpp8MoveU32: case O::Dpp8UpdateU32: case O::Permlane16U32:
			case O::SwizzleU32: case O::BpermuteU32: case O::DataAppend: case O::DataConsume: return true;
			default: break;
		}
	}
	return false;
}

std::unordered_set<const IR::Block*> CyclicBlocks(const IR::Program& program) {
	std::unordered_set<const IR::Block*> cyclic;
	std::unordered_map<const IR::Block*, uint32_t> index, low;
	std::unordered_set<const IR::Block*> on_stack;
	std::vector<const IR::Block*> stack;
	uint32_t next = 0;
	std::function<void(const IR::Block*)> visit = [&](const IR::Block* block) {
		index[block] = low[block] = next++;
		stack.push_back(block);
		on_stack.insert(block);
		bool self_edge = false;
		for (const auto* successor : block->ImmSuccessors()) {
			self_edge |= successor == block;
			if (!index.contains(successor)) {
				visit(successor);
				low[block] = std::min(low[block], low[successor]);
			} else if (on_stack.contains(successor)) {
				low[block] = std::min(low[block], index[successor]);
			}
		}
		if (low[block] != index[block]) return;
		std::vector<const IR::Block*> component;
		for (;;) {
			const auto* member = stack.back();
			stack.pop_back();
			on_stack.erase(member);
			component.push_back(member);
			if (member == block) break;
		}
		if (component.size() > 1 || self_edge) cyclic.insert(component.begin(), component.end());
	};
	for (const auto* block : program.blocks) if (!index.contains(block)) visit(block);
	return cyclic;
}

bool HasGuestBarrier(const IR::Program& program) {
	for (const auto* block : program.blocks) for (const auto& inst : *block)
		if (inst.GetOpcode() == O::Barrier) return true;
	return false;
}

bool IsSupportedLdsIntegerAtomic(O op) {
	switch (op) {
		case O::SharedAtomicSwap32: case O::SharedAtomicIAdd32: case O::SharedAtomicISub32:
		case O::SharedAtomicSMin32: case O::SharedAtomicUMin32:
		case O::SharedAtomicSMax32: case O::SharedAtomicUMax32:
		case O::SharedAtomicAnd32: case O::SharedAtomicOr32: case O::SharedAtomicXor32:
		case O::SharedAtomicIAdd64: case O::SharedAtomicOr64:
			return true;
		default: return false;
	}
}

// Cooperative mode accepts one ordered, acyclic chain of guest workgroup
// barriers. RDNA2 permits waves to terminate before or between barriers; those
// paths need not pass every phase. The scheduler keeps their host invocations
// alive until the group ends while each barrier waits only for surviving waves.
std::string ProveCooperativeBarrierOrder(const IR::Program& program) {
	const auto count = program.blocks.size();
	if (count == 0 || count != program.block_info.size())
		return "cooperative wave64 requires complete branch metadata";
	std::unordered_map<uint32_t, size_t> ids;
	std::unordered_map<const IR::Block*, size_t> ordinals;
	for (size_t index = 0; index < count; ++index) {
		if (program.blocks[index] == nullptr ||
		    !ids.emplace(program.block_info[index].id, index).second ||
		    !ordinals.emplace(program.blocks[index], index).second)
			return "cooperative wave64 requires unique block identities";
	}
	std::vector<std::vector<size_t>> edges(count);
	for (size_t index = 0; index < count; ++index) {
		const auto& info = program.block_info[index];
		const auto add = [&](uint32_t target) {
			const auto found = ids.find(target);
			if (found == ids.end()) return false;
			if (std::ranges::find(edges[index], found->second) == edges[index].end())
				edges[index].push_back(found->second);
			return true;
		};
		switch (info.terminator.kind) {
			case CFG::TerminatorKind::Branch:
				if (!add(info.terminator.true_block))
					return "cooperative wave64 requires statically known branch targets";
				break;
			case CFG::TerminatorKind::ConditionalBranch:
				if (info.condition.IsEmpty() || !add(info.terminator.true_block) ||
				    !add(info.terminator.false_block))
					return "cooperative wave64 requires complete conditional branch metadata";
				break;
			case CFG::TerminatorKind::Return: break;
			default: return "cooperative wave64 requires statically known branch targets";
		}
		const auto successors = program.blocks[index]->ImmSuccessors();
		if (successors.size() != edges[index].size())
			return "cooperative wave64 branch metadata disagrees with CFG edges";
		for (const auto* successor : successors) {
			const auto found = ordinals.find(successor);
			if (found == ordinals.end() ||
			    std::ranges::find(edges[index], found->second) == edges[index].end())
				return "cooperative wave64 branch metadata disagrees with CFG edges";
		}
	}
	const auto reachable_without = [&](size_t excluded) {
		std::vector<bool> reached(count, false);
		std::vector<size_t> pending;
		if (excluded != 0u) pending.push_back(0u);
		while (!pending.empty()) {
			const auto index = pending.back();
			pending.pop_back();
			if (index == excluded || reached[index]) continue;
			reached[index] = true;
			for (const auto successor : edges[index]) pending.push_back(successor);
		}
		return reached;
	};
	const auto reachable = reachable_without(count);
	std::vector<size_t> exits, barrier_blocks;
	for (size_t index = 0; index < count; ++index) {
		if (!reachable[index]) continue;
		if (program.block_info[index].terminator.kind == CFG::TerminatorKind::Return)
			exits.push_back(index);
		if (std::ranges::any_of(*program.blocks[index], [](const IR::Inst& inst) {
			    return inst.GetOpcode() == O::Barrier;
		    })) barrier_blocks.push_back(index);
	}
	if (barrier_blocks.empty()) return {};
	if (exits.empty()) return "cooperative wave64 barrier order requires a reachable exit";
	const auto cyclic = CyclicBlocks(program);
	for (const auto barrier : barrier_blocks) {
		if (cyclic.contains(program.blocks[barrier]))
			return "cooperative wave64 does not support cyclic guest barriers";
	}
	// A guest barrier rendezvous is identified by its dynamic occurrence, not
	// by one static instruction address. Different waves may therefore wait at
	// incomparable acyclic barrier sites before continuing from their own site.
	return {};
}

std::string ProveSplitWaveConvergence(const IR::Program& program, bool partitions_guest_workgroup,
                                     bool cooperative,
                                     bool* synchronize_split_wave_memory) {
	if (synchronize_split_wave_memory != nullptr) *synchronize_split_wave_memory = false;
	if (program.blocks.size() != program.block_info.size())
		return "wave64 splitting requires complete branch metadata";
	// GDS support is restricted to one logical wave reserving an append range.
	// A declaration alone cannot enable unrelated GDS loads, atomics or consume.
	std::unordered_set<uint32_t> append_memory;
	std::vector<const IR::Inst*> appends;
	for (const auto* block : program.blocks) for (const auto& inst : *block) {
		if (inst.GetOpcode() != O::DataAppend) continue;
		if (cooperative) return "cooperative wave64 does not support GDS append";
		const auto index = inst.Flags<IR::MemoryFlags>().index;
		if (partitions_guest_workgroup || index >= program.memory_info.size())
			return "wave64 GDS append requires one complete guest wave and valid metadata";
		const auto& memory = program.memory_info[index];
		if (memory.kind != IR::ResourceKind::Gds || !IsSupportedWave64GdsAppendOffset(memory.offset) ||
		    memory.data_bits != 32u || memory.data_dwords != 1u)
			return "wave64 GDS append requires a DWORD counter with an aligned 16-bit byte offset";
		append_memory.insert(index);
		appends.push_back(&inst);
	}
	for (uint32_t index = 0; index < program.memory_info.size(); ++index) {
		const auto& memory = program.memory_info[index];
		if ((memory.kind == IR::ResourceKind::Lds && partitions_guest_workgroup) ||
		    (memory.kind == IR::ResourceKind::Gds && !append_memory.contains(index)) ||
		    memory.kind == IR::ResourceKind::Scratch)
			return "wave64 splitting does not support guest shared or scratch memory";
	}
	const auto cyclic = CyclicBlocks(program);
	const auto can_reach_cycle = [&](const IR::Block* origin) {
		std::vector<const IR::Block*> pending{origin};
		std::unordered_set<const IR::Block*> visited{origin};
		for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
			if (cyclic.contains(pending[cursor])) return true;
			for (const auto* successor : pending[cursor]->ImmSuccessors()) {
				if (visited.insert(successor).second) pending.push_back(successor);
			}
		}
		return false;
	};
	bool image_write_reaches_cycle = false;
	for (const auto* block : program.blocks) for (const auto& inst : *block) {
		if (IR::ImageOpcodeInfoOf(inst.GetOpcode()).access == IR::ImageAccess::Write &&
		    can_reach_cycle(block)) {
			image_write_reaches_cycle = true;
			break;
		}
	}
	std::vector<const IR::Inst*> cyclic_reads;
	std::vector<const IR::Inst*> cyclic_writes;
	std::vector<const IR::Inst*> cyclic_appends;
	bool unproved_cooperative_publication = false;
	O unproved_cooperative_operation = O::Void;
	bool unproved_cooperative_operation_is_cyclic = false;
	std::unordered_set<const IR::Inst*> instructions;
	std::function<void(IR::Value)> collect = [&](IR::Value value) {
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || !instructions.insert(inst).second) return;
		for (size_t arg = 0; arg < inst->NumArgs(); ++arg) collect(inst->Arg(arg));
	};
	for (const auto* block : program.blocks) {
		for (const auto& inst : *block) {
			collect(IR::Value(const_cast<IR::Inst*>(&inst)));
			const auto op = inst.GetOpcode();
			if (!IsSupportedSplitOperation(op) && !IsSupportedLdsIntegerAtomic(op))
				return "wave64 splitting does not support operation " + std::string(IR::ValueOpcodeName(op));
			if (IR::SharedAccessOf(op) == IR::SharedAccess::Atomic) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				const auto expected_dwords =
				    op == O::SharedAtomicIAdd64 || op == O::SharedAtomicOr64 ? 2u : 1u;
				if (index >= program.memory_info.size() ||
				    program.memory_info[index].kind != IR::ResourceKind::Lds ||
				    program.memory_info[index].data_bits != 32u ||
				    program.memory_info[index].data_dwords != expected_dwords)
					return "wave64 splitting requires matching LDS integer atomic metadata";
			}
			if (op == O::DppMoveU32) {
				const auto flags = inst.Flags<IR::DppMoveFlags>();
				const bool row_shift = (flags.control >= 0x101 && flags.control <= 0x10f) ||
				                       (flags.control >= 0x111 && flags.control <= 0x11f);
				if (flags.fetch_inactive && row_shift)
					return "wave64 splitting has not established inactive-fetch DPP row-boundary semantics";
			}
			bool planning_only = false;
			if (op == O::LoadAddressU32 || op == O::ReadConstBuffer) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				planning_only = index < program.memory_info.size() && program.memory_info[index].planning_only;
			}
			if (!planning_only) {
				const auto image_access = IR::ImageOpcodeInfoOf(op).access;
				const auto buffer_access = IR::BufferAccessOf(op);
				// The cooperative scheduler publishes SSBO writes between quanta.
				// An image payload may be outside the polling SCC, so checking only
				// cyclic image writes would miss publication through a buffer flag.
				// A terminal image output cannot feed any scheduler cycle and needs no
				// cross-quantum publication. Writes that can reach a loop retain the
				// conservative image-memory boundary.
				const bool unsupported_publication =
				    (image_access == IR::ImageAccess::Write && can_reach_cycle(block)) ||
				    image_access == IR::ImageAccess::Atomic || buffer_access == IR::BufferAccess::Atomic ||
				    IR::AddressOpcodeInfoOf(op).access != IR::AddressAccess::None;
				// Physical pointers do not inherit the SSBO's Coherent decoration.
				// Scalar reads are emitted through the cooperative wave broadcast and
				// the scheduler's AcquireRelease UniformMemory rendezvous. Read-only
				// image payloads also need no publication when every image write is
				// terminal with respect to all scheduler cycles.
				const bool read_only_image = image_access == IR::ImageAccess::Read &&
				                             !image_write_reaches_cycle;
				const bool unsupported_cyclic_read = cyclic.contains(block) && IsGuestRead(op) &&
				    buffer_access != IR::BufferAccess::Read && op != O::ReadConstBuffer &&
				    !read_only_image;
				if (unsupported_publication ||
				    (unsupported_cyclic_read && !unproved_cooperative_publication)) {
					unproved_cooperative_operation = op;
					unproved_cooperative_operation_is_cyclic = cyclic.contains(block);
				}
				unproved_cooperative_publication |= unsupported_publication || unsupported_cyclic_read;
			}
			if ((op == O::Barrier && partitions_guest_workgroup) ||
			    op == O::DataConsume ||
			    op == O::Sendmsg || op == O::TtraceData || op == O::InstPrefetch || op == O::SetAttribute)
				return "wave64 splitting does not support guest workgroup or DS operations";
			if (IR::SharedAccessOf(op) != IR::SharedAccess::None) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				const bool gds_append = op == O::DataAppend && append_memory.contains(index);
				if (partitions_guest_workgroup || index >= program.memory_info.size() ||
				    (!gds_append && program.memory_info[index].kind != IR::ResourceKind::Lds))
					return "wave64 LDS access requires one complete guest wave and valid LDS metadata";
			}
			if (cyclic.contains(block)) {
				if (!planning_only && IsGuestRead(op)) cyclic_reads.push_back(&inst);
				if (!planning_only && (IsGuestWrite(op) || IsGuestAtomic(op)))
					cyclic_writes.push_back(&inst);
				if (op == O::DataAppend) cyclic_appends.push_back(&inst);
			}
			// A single complete guest wave remains one 64-invocation host workgroup.
			// An acyclic device-buffer atomic therefore produces one lane-local old
			// value exactly where the direct emitter defines it; native subgroup
			// splitting does not duplicate or transfer that value between phases.
			// Keep cyclic, partitioned, cooperative and LDS returns behind their
			// separate ordering/publication proofs.
			const bool direct_single_wave_buffer_atomic_return =
			    !partitions_guest_workgroup && !cooperative && !cyclic.contains(block) &&
			    IR::BufferAccessOf(op) == IR::BufferAccess::Atomic;
			const bool cooperative_image_atomic_return =
			    cooperative && IR::ImageOpcodeInfoOf(op).access == IR::ImageAccess::Atomic;
			if (!direct_single_wave_buffer_atomic_return && !cooperative_image_atomic_return &&
			    (IsGuestAtomic(op) || IR::SharedAccessOf(op) == IR::SharedAccess::Atomic) && inst.HasUses())
				return "wave64 splitting does not support live atomic return values";
		}
	}
	for (const auto& block : program.block_info) {
		collect(block.condition);
		if (block.terminator.kind == CFG::TerminatorKind::IndirectBranch ||
		    block.terminator.kind == CFG::TerminatorKind::Unsupported)
			return "wave64 splitting requires statically known branch targets";
	}

	// A bounded compaction loop may reserve output slots, but a returned GDS
	// counter must not drive polling or other cross-wave progress. Follow every
	// use, including Phi edges and collective operands, to every conditional.
	if (!cyclic_appends.empty()) {
		std::unordered_set<const IR::Inst*> dependent(cyclic_appends.begin(), cyclic_appends.end());
		for (size_t cursor = 0; cursor < cyclic_appends.size(); ++cursor) {
			for (const auto& use : cyclic_appends[cursor]->Uses()) {
				if (instructions.contains(use.user) && dependent.insert(use.user).second)
					cyclic_appends.push_back(use.user);
			}
		}
		for (const auto& block : program.block_info) {
			if (block.terminator.kind == CFG::TerminatorKind::ConditionalBranch &&
			    dependent.contains(block.condition.TryInstruction()))
				return "wave64 GDS append counter cannot control a branch";
		}
	}

	if (!cyclic_reads.empty()) {
		// A complete host workgroup can fairly interleave logical waves and
		// publish coherent buffer accesses at each PC publication barrier. Keep
		// image, raw-address, scalar-cache and atomic communication unsupported
		// until their distinct visibility protocols have executable regressions.
		if (cooperative && unproved_cooperative_publication)
			return "cooperative wave64 cyclic communication requires ordinary buffer reads and writes; found " +
			       std::string(IR::ValueOpcodeName(unproved_cooperative_operation)) +
			       (unproved_cooperative_operation_is_cyclic ? " in cycle" : " outside cycle");
		if ((!cyclic_writes.empty() || !cyclic_appends.empty()) && !cooperative) {
			if (partitions_guest_workgroup || !cyclic_appends.empty())
				return "wave64 splitting cannot prove cyclic reads and writes independent of other waves";
			// The complete guest wave remains one host workgroup. Rendezvous after
			// every external memory instruction preserves guest instruction order
			// across the two native subgroup32 halves, including cyclic feedback
			// through aliased buffers, physical addresses, or storage images.
			if (synchronize_split_wave_memory != nullptr)
				*synchronize_split_wave_memory = true;
		}

		// A single complete guest wave remains one host workgroup. Only actual
		// partitioning can introduce new inter-wave progress dependencies here.
		// Convergence still applies to every mode.
		if (partitions_guest_workgroup) {
			// Memory dependence is separate from lane uniformity. Ballot and
			// ReadLane can make a polling value uniform without making it safe.
			// The monotone worklist follows every SSA use, including phi backedges,
			// source/selector operands and EXEC predicates; cycles never clear taint.
			const auto cyclic_read_roots = cyclic_reads;
			std::unordered_set<const IR::Inst*> memory_dependent(cyclic_reads.begin(), cyclic_reads.end());
			for (size_t cursor = 0; cursor < cyclic_reads.size(); ++cursor) {
				for (const auto& use : cyclic_reads[cursor]->Uses()) {
					if (instructions.contains(use.user) && memory_dependent.insert(use.user).second)
						cyclic_reads.push_back(use.user);
				}
			}
			// Check every conditional, including those outside SCCs: an acyclic
			// branch can otherwise hide memory dependence in the edge selection of
			// constant phis feeding a later loop. Indirect targets were rejected above.
			const std::unordered_set<const IR::Inst*> cyclic_read_set(cyclic_read_roots.begin(),
			                                                        cyclic_read_roots.end());
			for (const auto& block : program.block_info) {
				if (block.terminator.kind == CFG::TerminatorKind::ConditionalBranch &&
				    memory_dependent.contains(block.condition.TryInstruction())) {
					const IR::Inst* progress_read = nullptr;
					std::unordered_set<const IR::Inst*> visited;
					std::function<void(IR::Value)> find_progress_read = [&](IR::Value value) {
						const auto* inst = value.TryInstruction();
						if (inst == nullptr || !visited.insert(inst).second || progress_read != nullptr) return;
						if (cyclic_read_set.contains(inst)) { progress_read = inst; return; }
						for (size_t arg = 0; arg < inst->NumArgs(); ++arg) find_progress_read(inst->Arg(arg));
					};
					find_progress_read(block.condition);
					return "wave64 splitting cannot prove loop memory independent of branch at pc " +
					       std::to_string(block.start_pc) +
					       (progress_read == nullptr ? std::string{} :
					        "; progress read " + std::string(IR::ValueOpcodeName(progress_read->GetOpcode())));
				}
			}
		}
	}

	// Greatest fixed point: a loop-carried uniform phi stays uniform until a
	// varying source reaches it. All branches are checked below, so phi edge
	// selection is uniform too; collectives cannot sit in divergent regions.
	std::unordered_set<const IR::Inst*> varying;
	const auto uniform = [&](IR::Value value) {
		const auto* inst = value.TryInstruction();
		return inst == nullptr ? value.IsImmediate() : !varying.contains(inst);
	};
	bool changed;
	do {
		changed = false;
		for (const auto* inst : instructions) {
			if (varying.contains(inst)) continue;
			const auto op = inst->GetOpcode();
			bool is_uniform = false;
			const auto scalar_address_read = [&] {
				if (op != O::LoadAddressU32) return false;
				const auto index = inst->Flags<IR::MemoryFlags>().index;
				return index < program.memory_info.size() &&
				       program.memory_info[index].kind == IR::ResourceKind::ScalarAddress;
			}();
			if (op == O::ReadConstBuffer || scalar_address_read) {
				// These opcodes model scalar memory instructions. Cooperative lowering
				// broadcasts their result, while ordinary split lowering gives every lane
				// the same proved-uniform address recipe. Vector/raw memory kinds retain
				// the rejection boundary.
				is_uniform = true;
				if (scalar_address_read) {
					for (size_t arg = 0; arg < inst->NumArgs(); ++arg)
						is_uniform &= uniform(inst->Arg(arg));
				}
			} else if (op == O::LaneId || op == O::DppMoveU32 || op == O::Dpp8MoveU32 ||
			    op == O::Permlane16U32 || op == O::WriteLane || op == O::WqmMask ||
			    op == O::UndefU1 || op == O::UndefU8 || op == O::UndefU16 ||
			    op == O::UndefU32 || op == O::UndefU64 || IsGuestRead(op) || IsGuestAtomic(op)) {
				is_uniform = false;
			} else if (op == O::GetBuiltin) {
				is_uniform = inst->Arg(0).IsImmediate() &&
				             static_cast<IR::StageInputKind>(inst->Arg(0).U32()) == IR::StageInputKind::WorkgroupId;
			} else if (op == O::ReadLane) {
				is_uniform = uniform(inst->Arg(1));
			} else if (op == O::Ballot || op == O::ReadFirstLane || op == O::GetUserData || op == O::GetShaderBase) {
				is_uniform = true;
			} else if (IsPureUniformOperation(op)) {
				is_uniform = true;
				for (size_t arg = 0; arg < inst->NumArgs(); ++arg) is_uniform &= uniform(inst->Arg(arg));
			}
			if (!is_uniform) changed |= varying.insert(inst).second;
		}
	} while (changed);
	for (const auto* append : appends) {
		if (!uniform(append->Arg(0)))
			return "wave64 GDS append requires wave-uniform M0";
	}
	const auto varying_chain = [&](IR::Value root) {
		std::string result;
		std::unordered_set<const IR::Inst*> visited;
		std::function<void(IR::Value)> append = [&](IR::Value value) {
			const auto* inst = value.TryInstruction();
			if (inst == nullptr || !varying.contains(inst)) return;
			if (!result.empty()) result += " -> ";
			result += IR::ValueOpcodeName(inst->GetOpcode());
			if (!visited.insert(inst).second) {
				result += " (cycle)";
				return;
			}
			for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
				const auto* dependency = inst->Arg(arg).TryInstruction();
				if (dependency != nullptr && varying.contains(dependency)) {
					append(inst->Arg(arg));
					return;
				}
			}
		};
		append(root);
		return result;
	};
	for (const auto& block : program.block_info) {
		if (block.terminator.kind == CFG::TerminatorKind::ConditionalBranch && !uniform(block.condition)) {
			return "wave64 splitting cannot prove wave-uniform branch at pc " +
			       std::to_string(block.start_pc) + "; varying chain " +
			       varying_chain(block.condition);
		}
	}
	// Dispatcher lowering selects the next static block ID from these same
	// proved-uniform branch conditions. With indirect and unsupported targets
	// rejected above, all 64 host invocations therefore execute the same switch
	// case sequence and reach split-wave rendezvous together.
	return {};
}
} // namespace

ComputeExecutionPlan PlanComputeExecution(const IR::Program& program,
                                          ShaderStageInputInfo input_info,
                                          const ComputeWorkgroupLimits& limits) {
	ComputeExecutionPlan plan;
	if (program.stage != ShaderType::Compute || input_info.compute == nullptr) {
		plan.error = "compute execution planning requires compute stage information";
		return plan;
	}
	const auto* cs = input_info.compute;
	const bool derivatives = program.spirv_requirements && program.spirv_requirements->compute_derivatives;
	const auto xy_default = derivatives ? 2u : 1u;
	plan.layout.guest_size = {cs->threads_num[0] ? cs->threads_num[0] : xy_default,
	                          cs->threads_num[1] ? cs->threads_num[1] : xy_default,
	                          cs->threads_num[2] ? cs->threads_num[2] : 1u};
	const bool split = program.wave_size == 64 && HasWaveOperations(program) && limits.native_subgroup_size != 0 &&
	                   limits.native_subgroup_size != 64 && !limits.can_require_subgroup_size_64;
	if (!split) {
		const auto layout = PlanComputeWorkgroup(plan.layout.guest_size, limits);
		if (!layout) plan.error = "compute workgroup cannot fit device";
		else if (layout->IsReshaped() && derivatives) plan.error = "compute derivative quad topology cannot be reshaped";
		else plan.layout = *layout;
		return plan;
	}
	if (limits.native_subgroup_size != 32) {
		plan.error = "wave64 emulation currently requires a native32 host";
		return plan;
	}
	uint32_t count = 1;
	for (const auto size : plan.layout.guest_size) {
		if (count > UINT32_MAX / size) { plan.error = "guest invocation count overflows"; return plan; }
		count *= size;
	}
	// A single partial guest wave with at most 32 real invocations has no upper
	// half work items. Preserve the existing native active-prefix execution.
	if (count <= 32) {
		const auto native = PlanComputeWorkgroup(plan.layout.guest_size, limits);
		if (!native) plan.error = "compute workgroup cannot fit device";
		else if (native->IsReshaped() && derivatives) plan.error = "compute derivative quad topology cannot be reshaped";
		else plan.layout = *native;
		return plan;
	}
	if (count % 64 != 0) { plan.error = "wave64 splitting requires complete guest waves"; return plan; }
	// Multi-wave shared storage and guest barriers require a cooperative host
	// group. Independent waves retain the smaller partitioned execution path.
	// If that path cannot prove cross-wave progress, a later bounded fallback
	// may keep the complete guest workgroup together. Reservations without live
	// accesses do not allocate the lazy guest array.
	const bool uses_lds = HasGuestLdsAccess(program);
	const bool requires_cooperative = count > 64u && (uses_lds || HasGuestBarrier(program));
	if (uses_lds && cs->lds_size_dwords == 0) {
		plan.error = "wave64 LDS access requires a nonzero guest LDS allocation";
		return plan;
	}
	if (derivatives) {
		plan.error = "wave64 splitting does not support compute derivatives";
		return plan;
	}

	const auto try_mode = [&](bool cooperative) {
		auto candidate = plan;
		const uint64_t collective_dwords = cooperative ? uint64_t{count} : 64ull;
		const uint64_t shared_bytes = collective_dwords * sizeof(uint32_t) +
		                              (uses_lds ? uint64_t{cs->lds_size_dwords} * sizeof(uint32_t) : 0);
		if (shared_bytes > limits.max_shared_memory_bytes) {
			candidate.error = "wave64 guest LDS and collective scratch exceed device shared memory limit";
			return candidate;
		}
		const auto host_shape = cooperative ? candidate.layout.guest_size :
		                                      std::array<uint32_t,3>{64,1,1};
		const auto host_layout = PlanComputeWorkgroup(host_shape, limits);
		if (!host_layout) {
			candidate.error = cooperative ? "device cannot fit a complete cooperative guest workgroup" :
			                                "device cannot fit a complete wave64 workgroup";
			return candidate;
		}
		if (cooperative) {
			candidate.error = ProveCooperativeBarrierOrder(program);
			if (!candidate.error.empty()) return candidate;
		}
		candidate.error = ProveSplitWaveConvergence(program, count > 64 && !cooperative, cooperative,
		                                                &candidate.synchronize_split_wave_memory);
		if (!candidate.error.empty()) return candidate;
		candidate.layout.host_size = host_layout->host_size;
		candidate.wave_partition_factor = cooperative ? 1u : count / 64;
		candidate.split_wave64 = true;
		candidate.cooperative_wave64 = cooperative;
		return candidate;
	};

	auto preferred = try_mode(requires_cooperative);
	if (preferred.error.empty() || requires_cooperative || count <= 64u) return preferred;

	// The independent proof deliberately rejects loops whose ordinary SSBO
	// feedback may require another guest wave to make progress. Retry with the
	// already validated cooperative scheduler, which publishes those writes at
	// every scheduling quantum. All unsupported image, atomic, raw-address,
	// scalar-cache and divergent-control cases remain rejected by the same
	// convergence proof, and the complete host workgroup must fit the device.
	auto promoted = try_mode(true);
	if (promoted.error.empty()) return promoted;
	preferred.error += "; cooperative fallback rejected: " + promoted.error;
	return preferred;
}
} // namespace Libs::Graphics::ShaderRecompiler
