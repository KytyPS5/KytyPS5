#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

[[noreturn]] void Fail(const IR::Program& program, const char* reason) {
	EXIT("SPIR-V validation failed: hash=0x%016" PRIx64 " stage=%u reason=%s\n",
	     program.shader_hash, static_cast<unsigned>(program.stage), reason);
	std::abort();
}

// The host stage that runs a program, as a VkShaderStageFlagBits value.
uint32_t HostStageBit(ShaderType stage, bool mesh_emulated = false) {
	if (stage == ShaderType::Mesh && mesh_emulated) {
		return 0x20u; // a compute shader
	}
	switch (stage) {
		case ShaderType::Vertex:
		case ShaderType::Local: return 0x1u;
		case ShaderType::TessellationControl: return 0x2u;
		case ShaderType::TessellationEvaluation: return 0x4u;
		case ShaderType::Pixel: return 0x10u;
		case ShaderType::Compute: return 0x20u;
		case ShaderType::Mesh: return 0x80u;
		default: return 0u;
	}
}

// The host has subgroup operations in the stage. The vertex support of
// DriverFaults::vertex_subgroups_unreported is measured for vertex functions only, not for the
// vertex stage of a tessellation pipeline.
bool StageHasSubgroups(ShaderType stage, const HostGpu& host, bool mesh_emulated = false) {
	if (stage == ShaderType::Local && host.faults.vertex_subgroups_unreported) {
		return false;
	}
	return (host.capabilities.subgroup_supported_stages & HostStageBit(stage, mesh_emulated)) != 0u;
}

} // namespace

bool UsesSingleLaneModel(ShaderType stage, const HostGpu& host) {
	const bool one_lane_per_invocation = stage == ShaderType::Vertex ||
	                                     stage == ShaderType::Local ||
	                                     stage == ShaderType::TessellationEvaluation;
	return one_lane_per_invocation && !StageHasSubgroups(stage, host);
}

bool LaneIndexFromScan(ShaderType stage, const HostGpu& host) {
	const bool vertex_stage = stage == ShaderType::Vertex || stage == ShaderType::Local ||
	                          stage == ShaderType::TessellationEvaluation;
	return host.faults.vertex_subgroups_unreported && vertex_stage &&
	       !UsesSingleLaneModel(stage, host);
}

std::optional<MissingCapability> FindMissingCapability(const IR::Program& program,
                                                       const HostGpu&     host) {
	const auto& requirements = program.info;
	const auto& caps         = host.capabilities;
	// Compute shared memory that is kept in a device buffer is a buffer for this purpose. The
	// emitter sets the descriptor counts before it asks.
	const bool shared_in_buffer =
	    program.bindings
	        .descriptor_counts[static_cast<size_t>(IR::DescriptorBindingKind::SharedMemory)] != 0;
	if (!caps.buffer_int64_atomics && (requirements.buffer_int64_atomics ||
	                                   (requirements.shared_int64_atomics && shared_in_buffer))) {
		return MissingCapability {"64-bit buffer atomics", "shaderBufferInt64Atomics"};
	}
	if (!caps.shared_int64_atomics && requirements.shared_int64_atomics && !shared_in_buffer) {
		return MissingCapability {"64-bit atomics on compute shared memory",
		                          "shaderSharedInt64Atomics with workgroupMemoryExplicitLayout"};
	}
	if (!caps.image_int64_atomics &&
	    std::ranges::any_of(program.info.images,
	                        [](const IR::ImageResource& image) { return image.atomic64; })) {
		return MissingCapability {"64-bit image atomics", "shaderImageInt64Atomics"};
	}
	if (!caps.compute_derivatives && requirements.compute_derivatives &&
	    program.stage == ShaderType::Compute) {
		return MissingCapability {"derivatives in a compute shader", "computeDerivativeGroupQuads"};
	}
	const bool subgroups = requirements.subgroup_ballot || requirements.subgroup_barrier ||
	                       requirements.subgroup_vote || requirements.subgroup_shuffle ||
	                       requirements.subgroup_arithmetic ||
	                       requirements.subgroup_local_invocation_id;
	// A wave barrier has no single-lane form.
	const bool single_lane =
	    UsesSingleLaneModel(program.stage, host) && !requirements.subgroup_barrier;
	if (subgroups && !single_lane &&
	    !StageHasSubgroups(program.stage, host, program.mesh_emulated)) {
		return MissingCapability {"subgroup operations in this stage", "subgroupSupportedStages"};
	}
	// One invocation is not a wave: DS_ORDERED_COUNT would add once per invocation.
	if (program.uses_ordered_count && UsesSingleLaneModel(program.stage, host)) {
		return MissingCapability {"DS_ORDERED_COUNT without subgroup operations in this stage",
		                          "subgroupSupportedStages"};
	}
	if (subgroups && !single_lane) {
		// VkSubgroupFeatureFlagBits: basic 0x1, vote 0x2, ballot 0x8, shuffle 0x10.
		const uint32_t operations =
		    (requirements.subgroup_local_invocation_id || requirements.subgroup_barrier ? 0x1u
		                                                                                : 0u) |
		    (requirements.subgroup_vote ? 0x2u : 0u) | (requirements.subgroup_ballot ? 0x8u : 0u) |
		    (requirements.subgroup_shuffle ? 0x10u : 0u) |
		    (requirements.subgroup_arithmetic ? 0x4u : 0u) |
		    // arithmetic 0x4, for a lane index from a scan
		    (requirements.subgroup_local_invocation_id && LaneIndexFromScan(program.stage, host)
		         ? 0x4u
		         : 0u);
		if ((caps.subgroup_supported_operations & operations) != operations) {
			return MissingCapability {
			    "a subgroup operation (lane id, vote, ballot, shuffle or scan)",
			    "subgroupSupportedOperations"};
		}
	}
	// An emulated mesh program writes its cull distances to the record buffer, and the replay
	// shader rejects the primitives itself: no host cull distances.
	const bool replays_cull = program.stage == ShaderType::Mesh && program.mesh_emulated;
	if (!caps.cull_distance && !replays_cull &&
	    std::ranges::any_of(program.info.outputs, [](const IR::StageOutput& output) {
		    return output.kind == IR::StageOutputKind::CullDistance;
	    })) {
		return MissingCapability {"cull distance outputs", "shaderCullDistance"};
	}
	const auto reads = [&](auto predicate) {
		return std::ranges::any_of(program.info.inputs, predicate);
	};
	const bool per_vertex  = reads([](const IR::StageInput& input) { return input.per_vertex; });
	const bool centroid    = reads([](const IR::StageInput& input) {
		return input.kind == IR::StageInputKind::BaryCoordSmoothCentroid;
	});
	const bool barycentric = centroid || reads([](const IR::StageInput& input) {
		                         return input.kind == IR::StageInputKind::BaryCoordSmooth ||
		                                input.kind == IR::StageInputKind::BaryCoordNoPerspective;
	                         });
	// Without per-vertex inputs the raw vertex values come through flat inputs (see
	// triangleVertexValueShader.h), and the shader has the barycentric builtin only when it reads
	// the barycentrics as values.
	if (host.faults.no_per_vertex_inputs) {
		if (!caps.fragment_barycentric && requirements.barycentric) {
			return MissingCapability {"barycentrics", "fragmentShaderBarycentric"};
		}
		return std::nullopt;
	}
	if (!caps.fragment_barycentric && (per_vertex || barycentric)) {
		return MissingCapability {"barycentrics or raw vertex values", "fragmentShaderBarycentric"};
	}
	return std::nullopt;
}

namespace {

// A capability the host does not offer must not reach SPIR-V: the driver would reject the module
// later with a message that does not say which guest shader needed it.
void ValidateHost(const IR::Program& program, const HostGpu& host) {
	if (const auto missing = FindMissingCapability(program, host)) {
		EXIT("shader needs %s (%s), which the host GPU does not offer: hash=0x%016" PRIx64
		     " stage=%u\n",
		     missing->need, missing->name, program.shader_hash,
		     static_cast<unsigned>(program.stage));
	}
}

} // namespace

std::vector<uint32_t> EmitProgram(IR::Program& program, ShaderStageInputInfo input_info,
                                  uint32_t push_data_start_dword, const HostGpu& host,
                                  bool emit_debug_info) {
	using namespace Emitter;

	if (!program.srt_plan_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete) {
		Fail(program, "SPIR-V emitter requires a fully planned native shader program");
	}
	IR::ValidateProgram(program);
	program.bindings = {.push_data_start_dword = push_data_start_dword};
	EmitterState state(program, input_info, host);
	state.single_lane = UsesSingleLaneModel(program.stage, host);
	if (emit_debug_info) {
		char name[64];
		std::snprintf(name, sizeof(name), "rdna2-%016" PRIx64 "-block-byte-pc-plus-one",
		              program.shader_hash);
		state.debug_source = state.builder.AddString(name);
		state.builder.EnableNames();
	}
	const auto* workgroup = ShaderWorkgroupInput(program.stage, input_info);
	state.lane_count =
	    workgroup != nullptr && program.wave_size == 64u && workgroup->host_subgroup_size == 32u
	        ? 2u
	        : 1u;
	DefineModule(state);
	ValidateHost(program, host);
	EmitProgram(state);
	state.builder.AddEntryPoint(MeshEmulated(state) ? spv::ExecutionModelGLCompute
	                                                : ExecutionModelForStage(state.program.stage),
	                            state.main_func, "main", state.interface_variables);

	return state.builder.Build();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
