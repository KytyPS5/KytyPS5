#include "graphics/shader/shader.h"

#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ShaderIR.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderStageRuntimeTests: failed: %s\n", text);
		std::abort();
	}
}

std::shared_ptr<const Libs::Graphics::ShaderRecompiler::IR::Program> SrtProgram(uint64_t address) {
	using namespace Libs::Graphics::ShaderRecompiler::IR;
	Program program;
	program.stage                      = Libs::Graphics::ShaderType::Compute;
	program.srt_plan_complete          = true;
	program.srt_patching_complete      = true;
	program.resource_tracking_complete = true;
	program.provenance.values.resize(6);
	program.provenance.values[0].op  = ScalarValueOp::Undefined;
	program.provenance.values[1].op  = ScalarValueOp::Unknown;
	program.provenance.values[2].op  = ScalarValueOp::Constant;
	program.provenance.values[2].imm = static_cast<uint32_t>(address);
	program.provenance.values[3].op  = ScalarValueOp::Constant;
	program.provenance.values[3].imm = static_cast<uint32_t>(address >> 32u);
	program.provenance.values[4].op   = ScalarValueOp::Constant;
	program.provenance.values[5].op   = ScalarValueOp::ReadConst;
	program.provenance.values[5].args = {2, 3, 4};
	program.srt.reads.push_back({5, 0, 0x40});
	return std::make_shared<const Program>(std::move(program));
}

std::shared_ptr<const Libs::Graphics::ShaderRecompiler::IR::Program> UnbasedFlatProgram() {
	using namespace Libs::Graphics::ShaderRecompiler::IR;
	Program program;
	program.stage                      = Libs::Graphics::ShaderType::Compute;
	program.srt_plan_complete          = true;
	program.srt_patching_complete      = true;
	program.resource_tracking_complete = true;
	AddressResource address;
	address.source       = ScalarProvenance::Unknown;
	address.first_use_pc = 0x80;
	address.kind         = ResourceKind::Flat;
	program.info.addresses.push_back(address);
	return std::make_shared<const Program>(std::move(program));
}

void TestFailurePreservesPriorStage() {
	using namespace Libs::Graphics;
	auto cached_program  = SrtProgram(0x1001);
	auto prior_program   = std::make_shared<const ShaderRecompiler::IR::Program>();
	auto prior_resources = std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>();
	ShaderStageRuntime stage {prior_program, prior_resources};
	const std::array<uint32_t, 1> user_data = {};
	std::string error;
	Check(!ShaderMaterializeStageRuntime(cached_program, user_data, 0, &stage, &error) &&
	          error.find("failed at 0x0000000000001000") != std::string::npos,
	      "unmapped cache ReadConst did not fail through the checked production reader");
	Check(stage.program == prior_program && stage.resources == prior_resources,
	      "failed cache rematerialization replaced the prior stage snapshot");
}

void TestMappedSrtUsesCheckedReaderByDefault() {
	using namespace Libs::Graphics;
	const uint32_t dword = 0x12345678;
	auto cached_program  = SrtProgram(reinterpret_cast<uint64_t>(&dword));
	ShaderStageRuntime stage;
	std::string error;
	Check(ShaderMaterializeStageRuntime(cached_program, {}, 0, &stage, &error), error.c_str());
	Check(stage.program == cached_program && stage.resources != nullptr,
	      "cache rematerialization did not publish the mapped stage");
	Check(stage.resources->flattened_srt.size() == 1 &&
	          stage.resources->flattened_srt[0] == dword,
	      "cache rematerialization did not use the checked reader by default");
}

void TestUnbasedFlatCacheHitFailsClosed() {
	using namespace Libs::Graphics;
	auto cached_program  = UnbasedFlatProgram();
	auto prior_program   = std::make_shared<const ShaderRecompiler::IR::Program>();
	auto prior_resources = std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>();
	ShaderStageRuntime stage {prior_program, prior_resources};
	std::string error;
	Check(!ShaderMaterializeStageRuntime(cached_program, {}, 0, &stage, &error) &&
	          error.find("requires runtime guest-address translation") != std::string::npos,
	      "unbased FLAT cache hit did not fail without a runtime translator");
	Check(stage.program == prior_program && stage.resources == prior_resources,
	      "unbased FLAT cache rejection replaced the prior stage snapshot");
}

} // namespace

int main() {
	TestFailurePreservesPriorStage();
	TestMappedSrtUsesCheckedReaderByDefault();
	TestUnbasedFlatCacheHitFailsClosed();
	std::puts("ShaderStageRuntimeTests: all cases passed");
	return 0;
}
