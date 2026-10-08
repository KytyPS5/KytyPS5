#include "graphics/shader/shaderVertexMetadata.h"

#include <array>
#include <cstring>

namespace Libs::Graphics {

bool ShaderReadVertexMetadata(const ShaderMappedData& data, uint32_t max_user_sgprs,
                              ShaderVertexMetadata& metadata, std::string* error) {
	if (data.user_data == nullptr) {
		return ShaderError::Fail(error, "missing AGC user-data header");
	}

	ShaderUserData user_data {};
	std::memcpy(&user_data, data.user_data, sizeof(user_data));

	constexpr uint32_t DirectResourceCount =
	    static_cast<uint32_t>(AgcDirectResourceType::Last) + 1u;
	if (user_data.direct_resource_count > DirectResourceCount) {
		return ShaderError::Fail(error,
		                         "AGC direct-resource count exceeds the known resource domain");
	}

	std::array<uint16_t, DirectResourceCount> direct_offsets {};
	const auto                                direct_size =
	    static_cast<uint64_t>(user_data.direct_resource_count) * sizeof(uint16_t);
	if (direct_size != 0) {
		if (user_data.direct_resource_offset == nullptr) {
			return ShaderError::Fail(error, "missing AGC direct-resource offsets");
		}
		std::memcpy(direct_offsets.data(), user_data.direct_resource_offset, direct_size);
	}

	ShaderVertexMetadata next;
	for (uint32_t type = 0; type < user_data.direct_resource_count; type++) {
		const auto reg = direct_offsets[type];
		if (reg == AGC_ILLEGAL_DIRECT_OFFSET) {
			continue;
		}
		switch (static_cast<AgcDirectResourceType>(type)) {
			case AgcDirectResourceType::PtrVertexBufferTable: next.vertex_buffer_reg = reg; break;
			case AgcDirectResourceType::PtrVertexAttribDescTable:
				next.vertex_attrib_reg = reg;
				break;
			default: break;
		}
	}

	if (next.vertex_attrib_reg >= 0 && next.vertex_buffer_reg < 0) {
		return ShaderError::Fail(error, "vertex attribute table requires a vertex buffer table");
	}
	if (next.vertex_buffer_reg < 0) {
		metadata = next;
		return true;
	}
	if (static_cast<uint32_t>(next.vertex_buffer_reg) + 1u >= max_user_sgprs) {
		return ShaderError::Fail(error, "vertex table pointer exceeds the user-SGPR domain");
	}
	if (next.vertex_attrib_reg < 0) {
		metadata = next;
		return true;
	}
	if (static_cast<uint32_t>(next.vertex_attrib_reg) + 1u >= max_user_sgprs) {
		return ShaderError::Fail(error, "vertex table pointer exceeds the user-SGPR domain");
	}
	if (data.num_input_semantics == 0 ||
	    data.num_input_semantics > ShaderVertexInputInfo::RES_MAX) {
		return ShaderError::Fail(error, "vertex semantic count is outside the supported domain");
	}

	const auto semantic_size =
	    static_cast<uint64_t>(data.num_input_semantics) * sizeof(ShaderSemantic);
	if (data.input_semantics == nullptr) {
		return ShaderError::Fail(error, "missing vertex input semantics");
	}
	std::memcpy(next.input_semantics.data(), data.input_semantics, semantic_size);
	next.input_semantics_count = data.num_input_semantics;
	metadata                   = next;
	return true;
}

} // namespace Libs::Graphics
