#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

[[noreturn]] void Fail(const IR::Program& program, const char* reason) {
	EXIT("SPIR-V validation failed: hash=0x%016" PRIx64 " stage=%u reason=%s\n",
	     program.shader_hash, static_cast<unsigned>(program.stage), reason);
	std::abort();
}

bool IsWaveUniformAluOpcode(IR::ValueOpcode op) {
	using Op = IR::ValueOpcode;
	switch (op) {
		case Op::BitCastU16F16:
		case Op::BitCastF16U16:
		case Op::BitCastU32F32:
		case Op::BitCastF32U32:
		case Op::BitCastU64F64:
		case Op::BitCastF64U64:
		case Op::ConvertU16U32:
		case Op::ConvertU32U16:
		case Op::ConvertU8U32:
		case Op::ConvertU32U8:
		case Op::ConvertF32F16:
		case Op::ConvertF16F32:
		case Op::ConvertS32F32:
		case Op::ConvertU32F32:
		case Op::ConvertF32S32:
		case Op::ConvertF64S32:
		case Op::ConvertF32F64:
		case Op::ConvertF64F32:
		case Op::ConvertF32U32:
		case Op::ConvertF64U32:
		case Op::CompositeConstructU64:
		case Op::CompositeConstructU32x2:
		case Op::CompositeConstructU32x3:
		case Op::CompositeConstructF32x2:
		case Op::CompositeConstructU32x4:
		case Op::CompositeExtractU64:
		case Op::CompositeExtractU32x2:
		case Op::CompositeExtractU32x3:
		case Op::CompositeExtractU32x4:
		case Op::FPAbs32:
		case Op::FPNeg32:
		case Op::FPSaturate32:
		case Op::BitFieldInsert:
		case Op::BitFieldUExtract:
		case Op::BitFieldSExtract:
		case Op::SelectU1:
		case Op::SelectF32:
		case Op::SelectF64:
		case Op::SelectU32:
		case Op::IAdd32:
		case Op::IAdd64:
		case Op::ISub32:
		case Op::ISub64:
		case Op::IMul32:
		case Op::UDiv32:
		case Op::SMulHi:
		case Op::UMulHi:
		case Op::IAbs32:
		case Op::ShiftLeftLogical32:
		case Op::ShiftLeftLogical64:
		case Op::ShiftRightLogical32:
		case Op::ShiftRightLogical64:
		case Op::ShiftRightArithmetic32:
		case Op::ShiftRightArithmetic64:
		case Op::BitwiseAnd32:
		case Op::BitwiseAnd64:
		case Op::BitwiseOr32:
		case Op::BitwiseXor32:
		case Op::BitwiseNot32:
		case Op::BitReverse32:
		case Op::BitCount32:
		case Op::FindUMsb32:
		case Op::FindILsb32:
		case Op::SMin32:
		case Op::UMin32:
		case Op::SMax32:
		case Op::UMax32:
		case Op::SLessThan32:
		case Op::SLessThan64:
		case Op::SLessThanEqual64:
		case Op::ULessThanEqual64:
		case Op::UGreaterThanEqual64:
		case Op::ULessThan32:
		case Op::ULessThan64:
		case Op::IEqual32:
		case Op::IEqual64:
		case Op::SLessThanEqual32:
		case Op::ULessThanEqual32:
		case Op::SGreaterThan32:
		case Op::UGreaterThan32:
		case Op::UGreaterThan64:
		case Op::INotEqual32:
		case Op::INotEqual64:
		case Op::SGreaterThanEqual32:
		case Op::UGreaterThanEqual32:
		case Op::LogicalOr:
		case Op::LogicalAnd:
		case Op::LogicalXor:
		case Op::LogicalNot: return true;
		default: return false;
	}
}

bool IsWaveUniformValue(IR::Value value, std::unordered_set<const IR::Inst*>& visited) {
	value = value.Resolve();
	if (value.IsImmediate()) {
		return true;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return false;
	}
	if (!visited.insert(inst).second) {
		return true;
	}
	using Op      = IR::ValueOpcode;
	const auto op = inst->GetOpcode();
	if (op == Op::GetUserData || op == Op::ReadConst || op == Op::ReadConstBuffer ||
	    op == Op::MeshDrawParameter || op == Op::GetDispatchThreadExtent ||
	    op == Op::GetShaderBase || op == Op::GetSrtResource || op == Op::GetBufferResource ||
	    op == Op::GetAddressResource || op == Op::GetScratchResource ||
	    op == Op::GetImageResource || op == Op::GetSamplerResource) {
		return true;
	}
	if (op == Op::GetBuiltin) {
		return inst->Arg(0).IsImmediate() &&
		       (static_cast<IR::StageInputKind>(inst->Arg(0).U32()) ==
		            IR::StageInputKind::WorkgroupId ||
		        static_cast<IR::StageInputKind>(inst->Arg(0).U32()) ==
		            IR::StageInputKind::NumWorkgroups);
	}
	if (op == Op::GetBufferResourceIndex || op == Op::Phi || IsWaveUniformAluOpcode(op)) {
		for (size_t i = 0; i < inst->NumArgs(); ++i) {
			if (!IsWaveUniformValue(inst->Arg(i), visited)) {
				return false;
			}
		}
		return true;
	}
	return false;
}

bool IsWaveUniformOrExecMaskedCondition(
    IR::Value value, const std::unordered_set<const IR::Inst*>& exec_predicates,
    bool& has_uniform_leaf, uint32_t depth = 0) {
	if (depth > 16u) {
		return false;
	}
	value            = value.Resolve();
	const auto* inst = value.TryInstruction();
	if (inst != nullptr && exec_predicates.contains(inst)) {
		return true;
	}
	if (inst != nullptr && inst->GetOpcode() == IR::ValueOpcode::LogicalAnd) {
		return IsWaveUniformOrExecMaskedCondition(inst->Arg(0), exec_predicates, has_uniform_leaf,
		                                          depth + 1u) &&
		       IsWaveUniformOrExecMaskedCondition(inst->Arg(1), exec_predicates, has_uniform_leaf,
		                                          depth + 1u);
	}
	std::unordered_set<const IR::Inst*> visited;
	if (IsWaveUniformValue(value, visited)) {
		has_uniform_leaf = true;
		return true;
	}
	return false;
}

// True when a Wave64 compute shader has no cross-lane communication, shared-memory accesses, or
// workgroup barriers, and all wave branch votes are structured EXEC/scalar tests, allowing native
// 1-lane-per-thread execution across two 32-lane host subgroups per 64-lane guest wave.
bool CanUseSingleLaneWave64Compute(const IR::Program&              program,
                                   const ShaderWorkgroupInputInfo& workgroup) {
	if (program.stage != ShaderType::Compute || program.dispatcher_fallback ||
	    program.wave_size != 64u || workgroup.host_subgroup_size != 32u ||
	    program.blocks.size() <= 1u) {
		return false;
	}
	const uint32_t total_threads = std::max(workgroup.threads_num[0], 1u) *
	                               std::max(workgroup.threads_num[1], 1u) *
	                               std::max(workgroup.threads_num[2], 1u);
	if (total_threads % 64u != 0u || total_threads > 1024u) {
		return false;
	}
	const auto& info = program.info;
	if (!info.subgroup_vote || program.blocks.size() <= 2u || info.subgroup_ballot ||
	    info.subgroup_shuffle || info.subgroup_arithmetic || info.subgroup_barrier ||
	    info.subgroup_local_invocation_id || info.uses_lds || info.uses_gds ||
	    info.uses_swizzle || info.bvh || info.bvh8 || info.bvh_stack || info.ray_query ||
	    info.compute_derivatives) {
		return false;
	}
	std::unordered_set<const IR::Inst*> exec_predicates;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == IR::ValueOpcode::ConditionRef &&
			    inst.Flags<CFG::BranchCondition>() == CFG::BranchCondition::ExecNonZero) {
				if (const auto* pred = inst.Arg(0).Resolve().TryInstruction()) {
					exec_predicates.insert(pred);
				}
			}
		}
	}
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::Barrier:
				case IR::ValueOpcode::StoreCompletion:
				case IR::ValueOpcode::DataCacheInvalidate:
				case IR::ValueOpcode::DataAppend:
				case IR::ValueOpcode::DataConsume:
				case IR::ValueOpcode::LaneId:
				case IR::ValueOpcode::Ballot:
				case IR::ValueOpcode::ReadFirstLane:
				case IR::ValueOpcode::ReadLane:
				case IR::ValueOpcode::WriteLane:
				case IR::ValueOpcode::SubgroupReduceU32:
				case IR::ValueOpcode::DppMoveU32:
				case IR::ValueOpcode::DppUpdateU32:
				case IR::ValueOpcode::SwizzleU32: return false;
				case IR::ValueOpcode::ConditionRef: {
					const auto kind = inst.Flags<CFG::BranchCondition>();
					if (kind == CFG::BranchCondition::ExecNonZero ||
					    kind == CFG::BranchCondition::ScalarInstruction) {
						break;
					}
					if (kind == CFG::BranchCondition::SccNonZero ||
					    kind == CFG::BranchCondition::SccZero ||
					    kind == CFG::BranchCondition::VccNonZero ||
					    kind == CFG::BranchCondition::VccZero) {
						bool has_uniform_leaf = false;
						if (IsWaveUniformOrExecMaskedCondition(inst.Arg(0), exec_predicates,
						                                       has_uniform_leaf) &&
						    has_uniform_leaf) {
							break;
						}
					}
					return false;
				}
				default: break;
			}
		}
	}
	return true;
}

} // namespace

std::vector<uint32_t> EmitProgram(IR::Program& program, ShaderStageInputInfo input_info,
                                  IR::PushData::Window push_data, const Profile& profile,
                                  bool emit_debug_info) {
	using namespace Emitter;

	if (!program.srt_plan_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete) {
		Fail(program, "SPIR-V emitter requires a fully planned native shader program");
	}
	IR::ValidateProgram(program);
	program.bindings = {};
	EmitterState state(program, input_info, profile);
	state.push_data = push_data;
	if (emit_debug_info) {
		char name[64];
		std::snprintf(name, sizeof(name), "rdna2-%016" PRIx64 "-block-byte-pc-plus-one",
		              program.shader_hash);
		state.debug_source = state.builder.AddString(name);
		state.builder.EnableNames();
	}
	const auto* workgroup = ShaderWorkgroupInput(program.stage, input_info);
	state.lane_count =
	    workgroup != nullptr && program.wave_size == 64u && workgroup->host_subgroup_size == 32u &&
	            !CanUseSingleLaneWave64Compute(program, *workgroup)
	        ? 2u
	        : 1u;
	DefineModule(state);
	EmitProgram(state);
	state.builder.AddEntryPoint(ExecutionModelForStage(state.program.stage), state.main_func,
	                            "main", state.interface_variables);

	return state.builder.Build();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
