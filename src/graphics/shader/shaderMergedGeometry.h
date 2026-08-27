#ifndef EMULATOR_SRC_GRAPHICS_SHADER_SHADERMERGEDGEOMETRY_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_SHADERMERGEDGEOMETRY_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics {

struct MergedGeometryProgram {
	std::vector<uint32_t> code;
	uint32_t gs_word_offset = 0;
};

bool ShaderAssembleMergedGeometry(std::span<const uint32_t> es_code,
                                  std::span<const uint32_t> gs_code,
                                  MergedGeometryProgram& out, std::string* error);

struct MergedGeometryLaunchState {
	static constexpr uint32_t ScalarCount = 32;
	static constexpr uint32_t VectorCount = 16;

	bool scalar[ScalarCount] = {};
	bool vector[VectorCount] = {};
	bool reads_m0            = false;
	bool scalar_out_of_range = false;
	bool vector_out_of_range = false;

	[[nodiscard]] std::string Describe() const;
};

bool ShaderAnalyzeMergedLaunchState(std::span<const uint32_t> code,
                                    MergedGeometryLaunchState& out, std::string* error);

inline constexpr uint32_t MeshWaveLanes = 64;

enum class MeshInputTopology { TriangleList, TriangleStrip };

[[nodiscard]] inline uint32_t MeshPrimitiveCount(MeshInputTopology topology, uint32_t index_count) {
	switch (topology) {
		case MeshInputTopology::TriangleList: return index_count / 3u;
		case MeshInputTopology::TriangleStrip: return index_count >= 3u ? index_count - 2u : 0u;
	}
	return 0;
}

struct MeshDispatch {
	uint32_t workgroup_count = 0;
	uint32_t vertices_per_workgroup   = 0;
	uint32_t primitives_per_workgroup = 0;
	uint32_t last_vertices   = 0;
	uint32_t last_primitives = 0;
	uint32_t output_vertices_per_workgroup   = 0;
	uint32_t output_primitives_per_workgroup = 0;
};

[[nodiscard]] inline uint32_t MeshOutputPrimitivesPerPrimitive(uint32_t output_vertices) {
	return output_vertices >= 3u ? output_vertices - 2u : 1u;
}

// SPI_SHADER_PGM_RSRC2_GS.LDS_SIZE counts 128-dword granules, the same unit compute uses.
inline constexpr uint32_t MeshLdsGranuleDwords = 128;

[[nodiscard]] inline uint32_t MeshLdsDwords(uint32_t rsrc2_lds_size) {
	return rsrc2_lds_size * MeshLdsGranuleDwords;
}

bool ShaderValidateMeshLds(uint32_t lds_dwords, uint32_t max_bytes, std::string* error);

bool ShaderComputeMeshDispatch(uint32_t total_primitives, uint32_t vertices_per_primitive,
                               uint32_t output_vertices_per_primitive, uint32_t vertex_group_size,
                               uint32_t primitive_group_size, uint32_t max_output_per_subgroup,
                               MeshDispatch& out, std::string* error);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_SHADERMERGEDGEOMETRY_H_
