#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERGSASSEMBLY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERGSASSEMBLY_H_

#include "graphics/shader/shader.h"

#include <string>

namespace Libs::Graphics {

// The subset of guest register state that decides whether a merged ES/GS (NGG) draw can be
// lowered to a host mesh shader, and how its pre-loaded VGPRs are laid out.
struct ShaderGsAssemblyRegisters {
	uint32_t gs_vgpr_component_count = 0; // SPI_SHADER_PGM_RSRC1_GS.GS_VGPR_COMP_CNT
	uint32_t es_vgpr_component_count = 0; // SPI_SHADER_PGM_RSRC2_GS.ES_VGPR_COMP_CNT
	uint32_t gs_out_prim_type        = 0; // VGT_GS_OUT_PRIM_TYPE
	uint32_t gs_max_vert_out         = 0; // VGT_GS_MAX_VERT_OUT
	uint32_t gs_instance_count       = 0; // VGT_GS_INSTANCE_CNT
	uint32_t primitive_group_size    = 0; // GE_CNTL.PRIM_GRP_SIZE
	uint32_t vertex_group_size       = 0; // GE_CNTL.VERT_GRP_SIZE
	bool     es_offchip_lds          = false; // SPI_SHADER_PGM_RSRC2_GS.OC_LDS_EN
	bool     user_vgpr1_enabled      = false; // GE_USER_VGPR_EN.EN_USER_VGPR1
	bool     user_vgpr2_enabled      = false; // GE_USER_VGPR_EN.EN_USER_VGPR2
	bool     user_vgpr3_enabled      = false; // GE_USER_VGPR_EN.EN_USER_VGPR3
	// VGT_SHADER_STAGES_EN.GS_EN is off but the program still uses an NGG workgroup, so it does
	// not amplify: each invocation is one input vertex and the GS output registers do not apply.
	bool native_ngg = false;
};

// Inputs the guest stage is told to expect that this lowering cannot reproduce. They do not stop
// the draw: the affected VGPRs keep whatever the mesh lowering puts there, which is what the
// emulator did before the counts were honoured at all.
struct ShaderGsAssemblyNotes {
	bool user_vgprs_preloaded = false; // GE user VGPR data lands in a pre-loaded ES VGPR
	bool gs_instancing        = false; // VGT_GS_INSTANCE_CNT asks for more than one invocation
	bool es_offchip_lds       = false; // the ES half reads off-chip LDS (tessellation evaluation)
};

// Number of GS VGPRs the SPI pre-loads for a merged ES/GS wave, and the first VGPR of the ES half.
// See shaderGsAssembly.cpp for the layout these come from.
constexpr uint32_t SHADER_GS_VGPR_MAX_COMPONENTS = 4;
constexpr uint32_t SHADER_ES_VGPR_BASE           = 5;
constexpr uint32_t SHADER_ES_VGPR_MAX_COMPONENTS = 4;

// Derives the mesh-shader assembly of a merged ES/GS draw. On entry `mesh` carries the state that
// does not depend on the geometry assembly (input_primitive, wave_size, max_vertices,
// provoking_vertex, LDS and scratch sizes). Returns false and leaves `mesh.threads_num[0] == 0`
// when the draw cannot be lowered, so the caller can drop the draw instead of dying.
bool ShaderBuildGsAssembly(const ShaderGsAssemblyRegisters& regs, ShaderMeshInputInfo& mesh,
                           ShaderGsAssemblyNotes* notes, std::string* error);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERGSASSEMBLY_H_ */
