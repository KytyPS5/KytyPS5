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
	    IR::ImageOpcodeInfoOf(op).access != IR::ImageAccess::None) return true;
	switch (op) {
		case O::Void: case O::Reference: case O::ReferenceU32:
		case O::GetUserData: case O::GetShaderBase: case O::GetBuiltin:
		case O::UndefU1: case O::UndefU8: case O::UndefU16: case O::UndefU32: case O::UndefU64:
		case O::LaneId: case O::Ballot: case O::ReadLane: case O::ReadFirstLane: case O::WriteLane:
		case O::WqmMask: case O::DppMoveU32: case O::DppUpdateU32:
		case O::Dpp8MoveU32: case O::Dpp8UpdateU32: case O::Permlane16U32:
		case O::SwizzleU32: case O::BpermuteU32:
		case O::ControlNop: case O::Waitcnt: return true;
		default: return false;
	}
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

std::string ProveSplitWaveConvergence(const IR::Program& program) {
	if (program.dispatcher_fallback) return "wave64 splitting requires structured control flow";
	if (program.blocks.size() != program.block_info.size())
		return "wave64 splitting requires complete branch metadata";
	for (const auto& memory : program.memory_info) {
		if (memory.kind == IR::ResourceKind::Lds || memory.kind == IR::ResourceKind::Gds ||
		    memory.kind == IR::ResourceKind::Scratch)
			return "wave64 splitting does not support guest shared or scratch memory";
	}
	const auto cyclic = CyclicBlocks(program);
	std::vector<const IR::Inst*> cyclic_reads;
	bool has_cyclic_write = false;
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
			if (!IsSupportedSplitOperation(op))
				return "wave64 splitting does not support operation " + std::string(IR::ValueOpcodeName(op));
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
			if (op == O::Barrier || IR::SharedAccessOf(op) != IR::SharedAccess::None ||
			    op == O::DataAppend || op == O::DataConsume ||
			    op == O::Sendmsg || op == O::TtraceData || op == O::InstPrefetch || op == O::SetAttribute)
				return "wave64 splitting does not support guest workgroup or DS operations";
			if (cyclic.contains(block)) {
				if (!planning_only && IsGuestRead(op)) cyclic_reads.push_back(&inst);
				has_cyclic_write |= IsGuestWrite(op) || IsGuestAtomic(op);
			}
			if (IsGuestAtomic(op) && inst.HasUses())
				return "wave64 splitting does not support live atomic return values";
		}
	}
	for (const auto& block : program.block_info) {
		collect(block.condition);
		if (block.terminator.kind == CFG::TerminatorKind::IndirectBranch ||
		    block.terminator.kind == CFG::TerminatorKind::Unsupported)
			return "wave64 splitting requires statically known branch targets";
	}

	if (!cyclic_reads.empty()) {
		// Without an alias/progress proof, a write in any loop may communicate
		// with a read in another loop or guest wave. Write-only shaders keep
		// their existing path; post-loop output stores remain permitted.
		if (has_cyclic_write)
			return "wave64 splitting cannot prove cyclic reads and writes independent of other waves";

		// Memory dependence is separate from lane uniformity. Ballot and
		// ReadLane can make a polling value uniform without making it safe.
		// The monotone worklist follows every SSA use, including phi backedges,
		// source/selector operands and EXEC predicates; cycles never clear taint.
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
		for (const auto& block : program.block_info) {
			if (block.terminator.kind == CFG::TerminatorKind::ConditionalBranch &&
			    memory_dependent.contains(block.condition.TryInstruction()))
				return "wave64 splitting cannot prove loop memory independent of branch at pc " +
				       std::to_string(block.start_pc);
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
			if (op == O::LaneId || op == O::DppMoveU32 || op == O::Dpp8MoveU32 ||
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
	for (const auto& block : program.block_info) {
		if (block.terminator.kind == CFG::TerminatorKind::ConditionalBranch && !uniform(block.condition))
			return "wave64 splitting cannot prove wave-uniform branch at pc " + std::to_string(block.start_pc);
	}
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
	// Storage reservations alone do not couple guest waves. The convergence
	// proof below rejects actual LDS/GDS/scratch accesses and all barriers.
	if (derivatives) {
		plan.error = "wave64 splitting does not support compute derivatives";
		return plan;
	}
	const auto host_layout = PlanComputeWorkgroup({64, 1, 1}, limits);
	if (!host_layout) { plan.error = "device cannot fit a complete wave64 workgroup"; return plan; }
	plan.error = ProveSplitWaveConvergence(program);
	if (!plan.error.empty()) return plan;
	plan.layout.host_size = host_layout->host_size;
	plan.wave_partition_factor = count / 64;
	plan.split_wave64 = true;
	return plan;
}
} // namespace Libs::Graphics::ShaderRecompiler
