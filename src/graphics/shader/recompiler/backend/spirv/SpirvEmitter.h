#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/capturedVertexLayout.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <optional>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// A vertex, local or tessellation evaluation stage runs one guest lane per host invocation. On a
// host with no subgroup operations in that stage, its lane operations are done on that one lane
// (the IR pass LowerLaneOpsToSingleLane and the emitter both follow this answer).
[[nodiscard]] bool UsesSingleLaneModel(ShaderType stage, const HostGpu& host);

// A vertex stage that uses subgroup operations on a device without subgroup built-ins there
// (DriverFaults::vertex_subgroups_unreported): the lane index comes from a scan, an arithmetic
// subgroup operation.
[[nodiscard]] bool LaneIndexFromScan(ShaderType stage, const HostGpu& host);

// How many clip distances the shader of this stage with these outputs writes. This counts the one
// the emitter adds to a vertex shader to cut away vertices with an invalid position.
uint32_t VertexClipDistanceCount(ShaderType stage, const IR::ShaderInfo& info);

// Something a program needs that the host does not offer.
struct MissingCapability {
	const char* need = nullptr; // what the shader does, in plain words
	const char* name = nullptr; // the Vulkan feature or construct
};

// The first thing `program` needs that `host` does not offer, if any. EmitProgram stops the
// emulator on such a program; a caller that must not stop asks here first.
[[nodiscard]] std::optional<MissingCapability> FindMissingCapability(const IR::Program& program,
                                                                     const HostGpu&     host);

// Resets and writes backend IDs and descriptor indices; do not emit the same program concurrently.
std::vector<uint32_t> EmitProgram(IR::Program& program, ShaderStageInputInfo input_info,
                                  uint32_t push_data_start_dword = 0,
                                  const HostGpu& host = HostGpu::Full());

// The record of a mesh program's outputs on a host without mesh shaders: the program runs as a
// compute shader (EmitProgram with an emulated mesh program), which stores one record per
// workgroup.
[[nodiscard]] CapturedVertexLayout MeshCaptureLayout(const IR::Program&         program,
                                                     const ShaderMeshInputInfo& mesh);

// The vertex shader that draws what the compute shader stored: one triangle per primitive of
// every record. A primitive that is absent, that the program culled, or that a cull distance
// rejects gets one position for its three corners, so it covers no sample. The pixel shader is
// unchanged.
struct MeshEmulationVertexProgram {
	std::vector<uint32_t> spirv;
	uint32_t              slot_words = 0;
};
[[nodiscard]] MeshEmulationVertexProgram
EmitMeshEmulationVertexProgram(const IR::Program& program, ShaderStageInputInfo input_info);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_ */
