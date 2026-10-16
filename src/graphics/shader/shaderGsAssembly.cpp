#include "graphics/shader/shaderGsAssembly.h"

#include <algorithm>
#include <cstdio>

namespace Libs::Graphics {

// A merged ES/GS (NGG) wave pre-loads a fixed set of VGPRs. The slots do not move: the two
// component counts only say how many of them the SPI actually writes, so a stage compiled with a
// lower count simply never reads the tail. The GS half owns VGPR0..VGPR3
//   VGPR0 = ES vertex offsets 0 and 1, VGPR1 = offsets 2 and 3,
//   VGPR2 = primitive ID,             VGPR3 = GS invocation (instance) ID
// and GS_VGPR_COMP_CNT is the index of the last one written. The ES half starts at VGPR5
//   VGPR5 = vertex index, VGPR6 = GE user VGPR1, VGPR7 = GE user VGPR2,
//   VGPR8 = GE user VGPR3 or the instance index
// and ES_VGPR_COMP_CNT is the index of the last one written, relative to VGPR5.
//
// So every value of either count is legal and none of them relocates an input: the lowering only
// has to make sure the registers a given count claims are initialised.

namespace {

bool Reject(std::string* error, const ShaderGsAssemblyRegisters& regs,
            const ShaderMeshInputInfo& mesh, const char* reason) {
	if (error != nullptr) {
		char message[512];
		std::snprintf(message, sizeof(message),
		              "%s: input=%u output=%u vertices=%u GE=%u/%u max_output=%u "
		              "gs_vgpr_comp_cnt=%u es_vgpr_comp_cnt=%u",
		              reason, mesh.input_primitive, regs.gs_out_prim_type, regs.gs_max_vert_out,
		              regs.primitive_group_size, regs.vertex_group_size, mesh.max_vertices,
		              regs.gs_vgpr_component_count, regs.es_vgpr_component_count);
		error->assign(message);
	}
	return false;
}

} // namespace

bool ShaderBuildGsAssembly(const ShaderGsAssemblyRegisters& regs, ShaderMeshInputInfo& mesh,
                           ShaderGsAssemblyNotes* notes, std::string* error) {
	mesh.threads_num[0] = mesh.threads_num[1] = mesh.threads_num[2] = 0;

	// Both fields are two bits wide, so every reported value is a value the hardware can produce.
	mesh.gs_vgpr_component_count =
	    std::min(regs.gs_vgpr_component_count, SHADER_GS_VGPR_MAX_COMPONENTS - 1u);
	mesh.es_vgpr_component_count =
	    std::min(regs.es_vgpr_component_count, SHADER_ES_VGPR_MAX_COMPONENTS - 1u);

	if (notes != nullptr) {
		*notes = {};
		// GE_USER_VGPR1..3 land in ES VGPR components 1..3. Their data registers are not tracked,
		// so a stage that is told to expect them reads whatever the lowering leaves behind.
		notes->user_vgprs_preloaded =
		    (regs.user_vgpr1_enabled && mesh.es_vgpr_component_count >= 1u) ||
		    (regs.user_vgpr2_enabled && mesh.es_vgpr_component_count >= 2u) ||
		    (regs.user_vgpr3_enabled && mesh.es_vgpr_component_count >= 3u);
		// VGPR3 carries the GS invocation ID; the lowering runs one invocation per primitive.
		notes->gs_instancing  = regs.gs_instance_count != 0;
		notes->es_offchip_lds = regs.es_offchip_lds;
	}

	const auto primitive = static_cast<Prospero::PrimitiveType>(mesh.input_primitive);
	if (primitive != Prospero::PrimitiveType::kPointList &&
	    primitive != Prospero::PrimitiveType::kLineList &&
	    primitive != Prospero::PrimitiveType::kTriStrip &&
	    primitive != Prospero::PrimitiveType::kTriFan &&
	    primitive != Prospero::PrimitiveType::kTriList) {
		return Reject(error, regs, mesh, "unsupported GS input primitive");
	}
	if (regs.gs_out_prim_type != 2u) {
		return Reject(error, regs, mesh, "unsupported GS output primitive");
	}
	if (!regs.native_ngg && regs.gs_max_vert_out < 3u) {
		return Reject(error, regs, mesh, "GS emits fewer than one triangle");
	}
	if (regs.vertex_group_size < mesh.InputPrimitiveSize() || mesh.max_vertices == 0u) {
		return Reject(error, regs, mesh, "GS subgroup holds no complete input primitive");
	}

	// A GS-mode subgroup exports at most one primitive per output vertex slot, not only a strip's
	// worth: Nanite's raster exports 32 triangles from 32 vertices.
	mesh.max_primitives = mesh.fast_launch ? mesh.max_vertices
	                      : regs.native_ngg
	                          ? mesh.InputPrimitiveCount(mesh.max_vertices)
	                          : std::max(regs.primitive_group_size * (regs.gs_max_vert_out - 2u),
	                                     std::min(regs.primitive_group_size * regs.gs_max_vert_out,
	                                              mesh.max_vertices));
	mesh.primitives_per_group = std::min(
	    {regs.primitive_group_size, mesh.InputPrimitiveCount(regs.vertex_group_size),
	     regs.native_ngg ? mesh.max_primitives : mesh.max_vertices / regs.gs_max_vert_out});
	if (mesh.primitives_per_group == 0u) {
		return Reject(error, regs, mesh, "GS subgroup holds no complete output primitive");
	}
	mesh.vertices_per_group = mesh.InputVertexCount(mesh.primitives_per_group);
	const auto threads =
	    regs.native_ngg && !mesh.fast_launch ? mesh.vertices_per_group : mesh.max_vertices;
	mesh.threads_num[0] = ((threads + mesh.wave_size - 1u) / mesh.wave_size) * mesh.wave_size;
	mesh.threads_num[1] = mesh.threads_num[2] = 1u;
	return true;
}

} // namespace Libs::Graphics
