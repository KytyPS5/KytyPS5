#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderFunctions.h"
#include "graphics/shader/shader.h"

#include <array>
#include <bitset>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {

struct CompileOptions {
	ShaderType                  stage           = ShaderType::Compute;
	uint32_t                    wave_size       = 64;
	uint32_t                    user_data_base  = 0;
	uint64_t                    shader_hash     = 0;
	bool                        dump_ir                    = true;
	bool                        emit_debug_info            = false;
	const Spirv::Profile*       profile                    = &Spirv::DefaultProfile;
	std::span<const uint32_t>   user_data;
	std::span<const uint32_t>   back_code;
	ShaderStageInputInfo        input_info;
	// Also emit CompileResult::spirv_plain for a pixel shader instrumented for GET_LOD_STATS.
	bool                        plain_mip_stats_variant    = false;
};

struct TranslateResult {
	IR::Program program;
	std::string decoded_dump;
	std::string cfg_dump;
};

struct CompileResult {
	std::vector<uint32_t>  spirv;
	// The same program without the GET_LOD_STATS recording (Spirv::UsesMipStats): for draws in
	// which no image has a mip-statistics counter. Empty unless requested and instrumented.
	std::vector<uint32_t>  spirv_plain;
	std::string            decoded_dump;
	std::string            ir_dump;
	IR::Program            program;
};

// Decoded source and typed scalar call queries are retained once per cache source.
struct ShaderSource {
	struct Call {
		uint32_t instruction;
		std::array<IR::Value, 2> target;
		std::bitset<128> preserved_registers;
		uint64_t address = 0;
	};
	struct Linked {
		struct Function { uint32_t offset; uint32_t size; };
		std::vector<uint32_t> code;
		Decoder::Program decoded;
		std::vector<Function> functions;
		std::vector<uint32_t> observed_function;
	};
	std::vector<uint32_t> code;
	Decoder::Program decoded;
	IR::ResourcePlan call_targets;
	std::vector<Call> calls;
	uint32_t unsupported_call_pc = UINT32_MAX;
	// Calls whose targets the call queries cannot evaluate (a function each lane picks from a
	// table, or a target written differently on different paths) are expanded from the guest's
	// call tables when the source is refreshed: `linked` then holds the expanded program, and
	// `refresh_failed` reports a refresh whose expansion is not possible (with its reason).
	bool expand_calls = false;
	uint32_t wave_size = 64;
	std::shared_ptr<Decoder::ShaderFunctionExpander> expander;
	bool refresh_failed = false;
	std::string refresh_failure;
	std::optional<Linked> linked;
	// The addresses the expanded program reads at run time (Decoder::RuntimeLiteral), current
	// after each refresh: they change without changing the program.
	std::vector<uint32_t> call_words;
	uint64_t revision = 0;
	std::vector<std::pair<uint64_t, uint64_t>> reads;

	[[nodiscard]] bool HasCalls() const { return !calls.empty() || expand_calls; }
};

[[nodiscard]] const char* StageName(ShaderType stage);
[[nodiscard]] ShaderSource PrepareShaderSource(std::span<const uint32_t> code,
	                                           const CompileOptions& options);
const Decoder::Program& RefreshShaderSource(ShaderSource& source, const IR::SrtRuntime& runtime);
[[nodiscard]] TranslateResult TranslateProgram(const Decoder::Program& decoded,
	                                           const CompileOptions& options);

[[nodiscard]] TranslateResult TranslateProgram(std::span<const uint32_t> code,
                                               const CompileOptions& options);
[[nodiscard]] CompileResult CompileProgram(TranslateResult translated,
                                           const CompileOptions& options,
                                           const IR::ResourceSpecialization& specialization,
	                                       uint32_t push_data_start_dword = 0);

} // namespace Libs::Graphics::ShaderRecompiler

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_ */
