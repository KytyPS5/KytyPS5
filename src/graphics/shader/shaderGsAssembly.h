#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERGSASSEMBLY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERGSASSEMBLY_H_

#include "graphics/shader/shader.h"

namespace Libs::Graphics {

// The guest registers that decide whether a merged ES/GS (NGG) draw lowers to a mesh shader.
struct ShaderGsAssemblyRegisters {
	uint32_t gs_vgpr_component_count = 0; // SPI_SHADER_PGM_RSRC1_GS.GS_VGPR_COMP_CNT
	uint32_t es_vgpr_component_count = 0; // SPI_SHADER_PGM_RSRC2_GS.ES_VGPR_COMP_CNT
	uint32_t gs_out_prim_type        = 0; // VGT_GS_OUT_PRIM_TYPE
	uint32_t gs_max_vert_out         = 0; // VGT_GS_MAX_VERT_OUT
	uint32_t primitive_group_size    = 0; // GE_CNTL.PRIM_GRP_SIZE
	uint32_t vertex_group_size       = 0; // GE_CNTL.VERT_GRP_SIZE
	// GS_EN off with an NGG workgroup: each invocation is one input vertex, no amplification.
	bool native_ngg = false;
};

constexpr uint32_t SHADER_GS_VGPR_MAX_COMPONENTS = 4;
constexpr uint32_t SHADER_ES_VGPR_MAX_COMPONENTS = 4;

// Records the pre-loaded VGPR counts in `mesh`; returns why the draw cannot be lowered, or nullptr.
const char* ShaderBuildGsAssembly(const ShaderGsAssemblyRegisters& regs, ShaderMeshInputInfo& mesh);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERGSASSEMBLY_H_ */
