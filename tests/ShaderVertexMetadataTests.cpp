#include "graphics/shader/shaderVertexMetadata.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderVertexMetadataTests: failed: %s\n", text);
		std::abort();
	}
}

struct Fixture {
	std::array<uint16_t, static_cast<size_t>(DirectResourceType::Last) + 1>    offsets {};
	ShaderUserData                                                             user_data {};
	ShaderSemantic                                                             semantic {};
	ShaderMappedData                                                           mapped {};

	Fixture() {
		offsets.fill(ILLEGAL_DIRECT_OFFSET);
		offsets[static_cast<size_t>(DirectResourceType::PtrVertexBufferTable)]     = 2;
		offsets[static_cast<size_t>(DirectResourceType::PtrVertexAttribDescTable)] = 4;
		user_data.direct_resource_offset = offsets.data();
		user_data.direct_resource_count  = static_cast<uint16_t>(offsets.size());
		mapped.user_data                 = &user_data;
		mapped.input_semantics           = &semantic;
		mapped.num_input_semantics       = 1;
	}
};

void CheckRejected(const ShaderMappedData& data, const char* text) {
	ShaderVertexMetadata output;
	std::array<ShaderSemantic, 7> prior_semantics {};
	output.vertex_buffer_reg = 37;
	output.vertex_attrib_reg = 41;
	output.input_semantics   = prior_semantics;
	Check(ShaderReadVertexMetadata(data, 64, output) != nullptr, text);
	Check(output.vertex_buffer_reg == 37 && output.vertex_attrib_reg == 41 &&
	          output.input_semantics.data() == prior_semantics.data() &&
	          output.input_semantics.size() == prior_semantics.size(),
	      "metadata rejection changed the prior output");
}

void TestValidAndInvalidMetadata() {
	Fixture              fixture;
	ShaderVertexMetadata output;
	Check(ShaderReadVertexMetadata(fixture.mapped, 64, output) == nullptr,
	      "valid vertex metadata was rejected");
	Check(output.vertex_buffer_reg == 2 && output.vertex_attrib_reg == 4 &&
	          output.input_semantics.size() == 1,
	      "valid vertex metadata was decoded incorrectly");

	Fixture buffer_only;
	buffer_only.offsets[static_cast<size_t>(DirectResourceType::PtrVertexAttribDescTable)] =
	    ILLEGAL_DIRECT_OFFSET;
	buffer_only.mapped.input_semantics     = nullptr;
	buffer_only.mapped.num_input_semantics = 0;
	Check(ShaderReadVertexMetadata(buffer_only.mapped, 64, output) == nullptr,
	      "vertex-buffer-only metadata was rejected");
	Check(output.vertex_buffer_reg == 2 && output.vertex_attrib_reg == -1 &&
	          output.input_semantics.empty(),
	      "vertex-buffer-only metadata was decoded incorrectly");

	Fixture attrib_only;
	attrib_only.offsets[static_cast<size_t>(DirectResourceType::PtrVertexBufferTable)] =
	    ILLEGAL_DIRECT_OFFSET;
	CheckRejected(attrib_only.mapped, "vertex-attribute-only metadata was accepted");

	auto missing_header      = fixture.mapped;
	missing_header.user_data = nullptr;
	CheckRejected(missing_header, "missing user-data header was accepted");

	Fixture missing_offsets;
	missing_offsets.user_data.direct_resource_offset = nullptr;
	CheckRejected(missing_offsets.mapped, "missing direct-resource offsets were accepted");

	Fixture excessive_resources;
	excessive_resources.user_data.direct_resource_count =
	    static_cast<uint16_t>(excessive_resources.offsets.size() + 1);
	CheckRejected(excessive_resources.mapped, "excessive direct-resource count was accepted");

	Fixture excessive_semantics;
	excessive_semantics.mapped.num_input_semantics = ShaderVertexInputInfo::RES_MAX + 1;
	CheckRejected(excessive_semantics.mapped, "excessive vertex semantic count was accepted");

	Fixture excessive_register;
	excessive_register.offsets[static_cast<size_t>(DirectResourceType::PtrVertexBufferTable)] = 63;
	CheckRejected(excessive_register.mapped, "out-of-domain vertex table SGPR was accepted");

	Fixture excessive_attrib_register;
	excessive_attrib_register
	    .offsets[static_cast<size_t>(DirectResourceType::PtrVertexAttribDescTable)] = 63;
	CheckRejected(excessive_attrib_register.mapped,
	              "out-of-domain vertex attribute table SGPR was accepted");

	Fixture missing_semantics;
	missing_semantics.mapped.input_semantics = nullptr;
	CheckRejected(missing_semantics.mapped, "missing vertex semantic array was accepted");
}

} // namespace

int main() {
	TestValidAndInvalidMetadata();
	std::puts("ShaderVertexMetadataTests: all cases passed");
	return 0;
}
