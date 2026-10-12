#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "spirv-tools/libspirv.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Libs::Graphics;
namespace Recompiler = ShaderRecompiler;

static void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "MacosNggVertexTests: %s\n", message);
		std::abort();
	}
}

static uint32_t Sop2(uint32_t opcode, uint32_t dst, uint32_t src0, uint32_t src1) {
	return 0x80000000u | (opcode << 23u) | (dst << 16u) | (src1 << 8u) | src0;
}

static void CheckVertex(uint32_t wave_size, bool export_count = false,
                        bool different_mask = false) {
	std::vector<uint32_t> shader {
	    Sop2(0x27, 1, 3, 255),
	    0x00080008u,
	    Sop2(0x03, 2, 192, 1),
	    Sop2(0x21, 126, 193, 2),
	    0xf8000941u,
	    0u,
	    Sop2(0x0e, 0, 3, 255),
	    0xffu,
	    Sop2(0x03, 2, 192, 0),
	    Sop2(0x21, 126, 193, 2),
	    0x7e0002f2u,
	    0xf80008cfu,
	    0u,
	    0xbf810000u,
	};
	if (different_mask) shader[7] = 0x3fu;
	if (export_count) {
		shader.insert(shader.begin() + 11, 0x7e020203u);
		shader[13] = 0x01010101u;
	}
	ShaderVertexInputInfo      input {};
	Recompiler::CompileOptions options;
	options.stage                               = ShaderType::Vertex;
	options.wave_size                           = wave_size;
	options.input_info.vertex                   = &input;
	options.dump_ir                             = false;
	auto                             translated = Recompiler::TranslateProgram(shader, options);
	auto                             plan = Recompiler::IR::ExtractResourcePlan(translated.program);
	Recompiler::IR::ResourceSnapshot snapshot;
	Recompiler::IR::ResourceSpecialization specialization;
	Check(Recompiler::IR::MaterializeResources(plan, {}, snapshot, specialization),
	      "resource materialization failed");
	auto result = Recompiler::CompileProgram(std::move(translated), options, specialization);
	Check(result.program.info.subgroup_ballot == (export_count || different_mask) &&
	          result.program.info.subgroup_local_invocation_id == different_mask,
	      "vertex validity lowering removed a count/mask consumer or retained its prologue");
	bool position = false;
	for (const auto& output: result.program.info.outputs) {
		position |= output.kind == Recompiler::IR::StageOutputKind::Position;
	}
	Check(position, "vertex position export disappeared");
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
	Check(validator.Validate(result.spirv), "invalid SPIR-V after vertex lowering");
}

int main() {
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Console;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	for (uint32_t wave_size: {32u, 64u}) {
		CheckVertex(wave_size);
		CheckVertex(wave_size, true);
		CheckVertex(wave_size, false, true);
	}
	std::puts("MacosNggVertexTests: passed");
}
