#include "graphics/shader/shaderGsAssembly.h"

#include <algorithm>

namespace Libs::Graphics {

// NGG VGPRs: GS v0-v3 = offsets 0/1, offsets 2/3, primitive ID, instance ID; ES v5 = vertex index.
const char* ShaderBuildGsAssembly(const ShaderGsAssemblyRegisters& regs,
                                  ShaderMeshInputInfo&             mesh) {
	mesh.threads_num[0] = mesh.threads_num[1] = mesh.threads_num[2] = 0;

	// Both fields are two bits wide, so every reported value is a value the hardware can produce.
	mesh.gs_vgpr_component_count =
	    std::min(regs.gs_vgpr_component_count, SHADER_GS_VGPR_MAX_COMPONENTS - 1u);
	mesh.es_vgpr_component_count =
	    std::min(regs.es_vgpr_component_count, SHADER_ES_VGPR_MAX_COMPONENTS - 1u);

	const auto primitive = static_cast<Prospero::PrimitiveType>(mesh.input_primitive);
	if (primitive != Prospero::PrimitiveType::kPointList &&
	    primitive != Prospero::PrimitiveType::kLineList &&
	    primitive != Prospero::PrimitiveType::kTriStrip &&
	    primitive != Prospero::PrimitiveType::kTriFan &&
	    primitive != Prospero::PrimitiveType::kTriList) {
		return "unsupported GS input primitive";
	}
	if (regs.gs_out_prim_type != 2u) {
		return "unsupported GS output primitive";
	}
	if (!regs.native_ngg && regs.gs_max_vert_out < 3u) {
		return "GS emits fewer than one triangle";
	}
	if (regs.vertex_group_size < mesh.InputPrimitiveSize() || mesh.max_vertices == 0u) {
		return "GS subgroup holds no complete input primitive";
	}
	return nullptr;
}

} // namespace Libs::Graphics
