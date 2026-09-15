#include "graphics/shader/shaderGsAssembly.h"
#include "graphics/shader/shaderVertexMetadata.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderVertexMetadataTests: failed: %s\n", text);
		std::abort();
	}
}

struct Fixture {
	std::array<uint16_t, static_cast<size_t>(AgcDirectResourceType::Last) + 1> offsets {};
	ShaderUserData                                                             user_data {};
	ShaderSemantic                                                             semantic {};
	ShaderMappedData                                                           mapped {};

	Fixture() {
		offsets.fill(AGC_ILLEGAL_DIRECT_OFFSET);
		offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexBufferTable)]     = 2;
		offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexAttribDescTable)] = 4;
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
	std::string error;
	Check(!ShaderReadVertexMetadata(data, 64, output, &error), text);
	Check(!error.empty(), "metadata rejection omitted its diagnostic");
	Check(output.vertex_buffer_reg == 37 && output.vertex_attrib_reg == 41 &&
	          output.input_semantics.data() == prior_semantics.data() &&
	          output.input_semantics.size() == prior_semantics.size(),
	      "metadata rejection changed the prior output");
}

void TestValidAndInvalidMetadata() {
	Fixture              fixture;
	ShaderVertexMetadata output;
	std::string          error;
	Check(ShaderReadVertexMetadata(fixture.mapped, 64, output, &error),
	      "valid AGC vertex metadata was rejected");
	Check(output.vertex_buffer_reg == 2 && output.vertex_attrib_reg == 4 &&
	          output.input_semantics.size() == 1,
	      "valid AGC vertex metadata was decoded incorrectly");

	Fixture buffer_only;
	buffer_only.offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexAttribDescTable)] =
	    AGC_ILLEGAL_DIRECT_OFFSET;
	buffer_only.mapped.input_semantics     = nullptr;
	buffer_only.mapped.num_input_semantics = 0;
	Check(ShaderReadVertexMetadata(buffer_only.mapped, 64, output, &error),
	      "vertex-buffer-only AGC metadata was rejected");
	Check(output.vertex_buffer_reg == 2 && output.vertex_attrib_reg == -1 &&
	          output.input_semantics.empty(),
	      "vertex-buffer-only AGC metadata was decoded incorrectly");

	Fixture attrib_only;
	attrib_only.offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexBufferTable)] =
	    AGC_ILLEGAL_DIRECT_OFFSET;
	CheckRejected(attrib_only.mapped, "vertex-attribute-only AGC metadata was accepted");

	auto missing_header      = fixture.mapped;
	missing_header.user_data = nullptr;
	CheckRejected(missing_header, "missing AGC user-data header was accepted");

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
	excessive_register.offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexBufferTable)] =
	    63;
	CheckRejected(excessive_register.mapped, "out-of-domain vertex table SGPR was accepted");

	Fixture excessive_attrib_register;
	excessive_attrib_register
	    .offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexAttribDescTable)] = 63;
	CheckRejected(excessive_attrib_register.mapped,
	              "out-of-domain vertex attribute table SGPR was accepted");

	Fixture missing_semantics;
	missing_semantics.mapped.input_semantics = nullptr;
	CheckRejected(missing_semantics.mapped, "missing vertex semantic array was accepted");
}

void CheckAccepted(bool accepted, const char* text, const std::string& error) {
	if (!accepted) {
		std::fprintf(stderr, "ShaderVertexMetadataTests: failed: %s: %s\n", text,
		             error.c_str());
		std::abort();
	}
}

// A merged ES/GS draw that the mesh lowering supports: a triangle list feeding a GS that emits one
// triangle per input primitive into 32-lane waves.
ShaderGsAssemblyRegisters ValidGsRegisters() {
	return {
	    .gs_vgpr_component_count = 3,
	    .es_vgpr_component_count = 3,
	    .gs_out_prim_type        = 2,
	    .gs_max_vert_out         = 3,
	    .gs_instance_count       = 0,
	    .primitive_group_size    = 16,
	    .vertex_group_size       = 48,
	};
}

ShaderMeshInputInfo ValidGsMesh() {
	ShaderMeshInputInfo mesh;
	mesh.input_primitive  = static_cast<uint32_t>(Prospero::PrimitiveType::kTriList);
	mesh.wave_size        = 32;
	mesh.max_vertices     = 96;
	mesh.provoking_vertex = 0;
	return mesh;
}

void CheckGsRejected(const ShaderGsAssemblyRegisters& regs, const ShaderMeshInputInfo& input,
                     const char* text) {
	auto                  mesh = input;
	ShaderGsAssemblyNotes notes;
	std::string           error;
	Check(!ShaderBuildGsAssembly(regs, mesh, &notes, &error), text);
	Check(!error.empty(), "GS assembly rejection omitted its diagnostic");
	Check(error.find("gs_vgpr_comp_cnt=") != std::string::npos &&
	          error.find("es_vgpr_comp_cnt=") != std::string::npos,
	      "GS assembly rejection did not name the reported VGPR component counts");
	Check(mesh.threads_num[0] == 0, "a rejected GS assembly still claimed a mesh thread count");
}

// Every value of GS_VGPR_COMP_CNT and ES_VGPR_COMP_CNT is legal: the counts say how many VGPRs the
// SPI pre-loads, they never move an input, so none of them may change the derived assembly.
void TestGsVgprComponentCounts() {
	ShaderMeshInputInfo   reference = ValidGsMesh();
	ShaderGsAssemblyNotes notes;
	std::string           error;
	CheckAccepted(ShaderBuildGsAssembly(ValidGsRegisters(), reference, &notes, &error),
	      "the reference GS assembly was rejected", error);
	Check(reference.max_primitives == 16 && reference.primitives_per_group == 16 &&
	          reference.vertices_per_group == 48 && reference.threads_num[0] == 96 &&
	          reference.threads_num[1] == 1 && reference.threads_num[2] == 1,
	      "the reference GS assembly was derived incorrectly");

	for (uint32_t gs_count = 0; gs_count < SHADER_GS_VGPR_MAX_COMPONENTS; gs_count++) {
		for (uint32_t es_count = 0; es_count < SHADER_ES_VGPR_MAX_COMPONENTS; es_count++) {
			auto regs                    = ValidGsRegisters();
			regs.gs_vgpr_component_count = gs_count;
			regs.es_vgpr_component_count = es_count;
			auto mesh                    = ValidGsMesh();
			CheckAccepted(ShaderBuildGsAssembly(regs, mesh, &notes, &error),
			      "a legal GS/ES VGPR component count was rejected", error);
			Check(mesh.gs_vgpr_component_count == gs_count &&
			          mesh.es_vgpr_component_count == es_count,
			      "the GS/ES VGPR component counts were not carried into the mesh assembly");
			Check(mesh.max_primitives == reference.max_primitives &&
			          mesh.primitives_per_group == reference.primitives_per_group &&
			          mesh.vertices_per_group == reference.vertices_per_group &&
			          mesh.threads_num[0] == reference.threads_num[0],
			      "a GS/ES VGPR component count changed the derived mesh assembly");
			Check(!notes.user_vgprs_preloaded && !notes.gs_instancing && !notes.es_offchip_lds,
			      "a plain GS assembly reported an unsupported input");
		}
	}
}

// Inputs the lowering cannot reproduce are reported but never stop the draw.
void TestGsUnsupportedInputsAreReportedNotFatal() {
	for (uint32_t es_count = 0; es_count < SHADER_ES_VGPR_MAX_COMPONENTS; es_count++) {
		auto regs                    = ValidGsRegisters();
		regs.es_vgpr_component_count = es_count;
		regs.user_vgpr1_enabled      = true;
		auto                  mesh   = ValidGsMesh();
		ShaderGsAssemblyNotes notes;
		std::string           error;
		CheckAccepted(ShaderBuildGsAssembly(regs, mesh, &notes, &error),
		      "an enabled GE user VGPR stopped a GS draw", error);
		Check(notes.user_vgprs_preloaded == (es_count >= 1),
		      "a pre-loaded GE user VGPR was reported against the wrong component count");
	}

	auto                  instanced = ValidGsRegisters();
	instanced.gs_instance_count     = 0x00000005;
	auto                  mesh      = ValidGsMesh();
	ShaderGsAssemblyNotes notes;
	std::string           error;
	CheckAccepted(ShaderBuildGsAssembly(instanced, mesh, &notes, &error),
	      "GS instancing stopped a GS draw", error);
	Check(notes.gs_instancing, "GS instancing was not reported");

	auto offchip           = ValidGsRegisters();
	offchip.es_offchip_lds = true;
	mesh                   = ValidGsMesh();
	CheckAccepted(ShaderBuildGsAssembly(offchip, mesh, &notes, &error),
	      "an off-chip LDS ES half stopped a GS draw", error);
	Check(notes.es_offchip_lds, "an off-chip LDS ES half was not reported");
}

// Assemblies the mesh lowering has no shape for are dropped, with every register in the message.
void TestGsUnsupportedAssemblyIsRejected() {
	auto adjacency = ValidGsMesh();
	adjacency.input_primitive =
	    static_cast<uint32_t>(Prospero::PrimitiveType::kTriListAdjacency);
	CheckGsRejected(ValidGsRegisters(), adjacency, "an adjacency input primitive was accepted");

	auto lines             = ValidGsRegisters();
	lines.gs_out_prim_type = 1;
	CheckGsRejected(lines, ValidGsMesh(), "a non-triangle GS output primitive was accepted");

	auto short_output          = ValidGsRegisters();
	short_output.gs_max_vert_out = 2;
	CheckGsRejected(short_output, ValidGsMesh(), "a GS emitting no triangle was accepted");

	auto small_group             = ValidGsRegisters();
	small_group.vertex_group_size = 2;
	CheckGsRejected(small_group, ValidGsMesh(), "a GE vertex group below one primitive was accepted");

	auto no_output        = ValidGsMesh();
	no_output.max_vertices = 0;
	CheckGsRejected(ValidGsRegisters(), no_output, "an empty GS subgroup was accepted");

	auto starved         = ValidGsMesh();
	starved.max_vertices = 2;
	CheckGsRejected(ValidGsRegisters(), starved,
	                "a GS subgroup holding no output primitive was accepted");
}

} // namespace

int main() {
	TestValidAndInvalidMetadata();
	TestGsVgprComponentCounts();
	TestGsUnsupportedInputsAreReportedNotFatal();
	TestGsUnsupportedAssemblyIsRejected();
	std::puts("ShaderVertexMetadataTests: all cases passed");
	return 0;
}
