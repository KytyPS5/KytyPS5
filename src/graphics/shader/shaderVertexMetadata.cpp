#include "graphics/shader/shaderVertexMetadata.h"

#include "graphics/host_gpu/hostMemory.h"

#include <array>
#include <cstring>

namespace Libs::Graphics {

namespace {

bool Fail(std::string* error, const char* message) {
	if (error != nullptr) {
		*error = message;
	}
	return false;
}

bool Readable(const void* data, uint64_t size) {
	return data != nullptr && HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(data), size);
}

} // namespace

bool ShaderReadVertexMetadata(const ShaderMappedData& data, uint32_t max_user_sgprs,
                              ShaderVertexMetadata* metadata, std::string* error) {
	if (metadata == nullptr) {
		return Fail(error, "missing vertex metadata output");
	}
	if (!Readable(data.user_data, sizeof(ShaderUserData))) {
		return Fail(error, "unreadable AGC user-data header");
	}

	ShaderUserData user_data {};
	std::memcpy(&user_data, data.user_data, sizeof(user_data));

	constexpr uint32_t DirectResourceCount =
	    static_cast<uint32_t>(AgcDirectResourceType::Last) + 1u;
	if (user_data.direct_resource_count > DirectResourceCount) {
		return Fail(error, "AGC direct-resource count exceeds the known resource domain");
	}

	std::array<uint16_t, DirectResourceCount> direct_offsets {};
	const auto                                direct_size =
	    static_cast<uint64_t>(user_data.direct_resource_count) * sizeof(uint16_t);
	if (direct_size != 0) {
		if (!Readable(user_data.direct_resource_offset, direct_size)) {
			return Fail(error, "unreadable AGC direct-resource offsets");
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

	const bool embedded = next.vertex_buffer_reg >= 0 || next.vertex_attrib_reg >= 0;
	if (!embedded) {
		*metadata = next;
		return true;
	}
	if (next.vertex_buffer_reg < 0 || next.vertex_attrib_reg < 0) {
		return Fail(error, "vertex buffer and attribute tables must be supplied together");
	}
	if (static_cast<uint32_t>(next.vertex_buffer_reg) + 1u >= max_user_sgprs ||
	    static_cast<uint32_t>(next.vertex_attrib_reg) + 1u >= max_user_sgprs) {
		return Fail(error, "vertex table pointer exceeds the user-SGPR domain");
	}
	if (data.num_input_semantics == 0 ||
	    data.num_input_semantics > ShaderVertexInputInfo::RES_MAX) {
		return Fail(error, "vertex semantic count is outside the supported domain");
	}

	const auto semantic_size =
	    static_cast<uint64_t>(data.num_input_semantics) * sizeof(ShaderSemantic);
	if (!Readable(data.input_semantics, semantic_size)) {
		return Fail(error, "unreadable vertex input semantics");
	}
	std::memcpy(next.input_semantics.data(), data.input_semantics, semantic_size);
	next.input_semantics_count = data.num_input_semantics;
	*metadata                  = next;
	return true;
}

} // namespace Libs::Graphics
