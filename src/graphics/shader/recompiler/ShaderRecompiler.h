#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/shader.h"

#include <array>
#include <bitset>
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
	bool                        early_dump                 = false;
	bool                        emit_debug_info            = false;
	// Experimental: link S_SWAPPC calls whose target is read from a function table at an entry
	// selected by GPU data. Off by default because such callees may read descriptors through
	// pointers that are also selected by GPU data, which resource tracking cannot plan yet.
	// Set from Config::FunctionTableCallsEnabled() (--function-table-calls / launcher option).
	bool                        function_table_calls       = false;
	const Spirv::Profile*       profile                    = &Spirv::DefaultProfile;
	const char*                 dump_label                 = nullptr;
	std::span<const uint32_t>   user_data;
	std::span<const uint32_t>   back_code;
	ShaderStageInputInfo        input_info;
};

struct TranslateResult {
	IR::Program program;
	std::string decoded_dump;
	std::string cfg_dump;
};

struct CompileResult {
	std::vector<uint32_t>  spirv;
	std::string            decoded_dump;
	std::string            ir_dump;
	IR::Program            program;
};

// Decoded source and typed scalar call queries are retained once per cache source.
struct ShaderSource {
	struct Call {
		uint32_t instruction;
		// One candidate per definition reaching the call; all must evaluate to the same address.
		std::array<std::vector<IR::Value>, 2> target;
		std::bitset<128> preserved_registers;
		uint64_t address = 0;
		// A call through a function table in a GPU buffer: the target pair is read at a
		// lane-selected entry (e.g. a waterfall loop over a per-thread index). Every
		// distinct entry is linked, and a compare chain on the target pair picks the callee.
		struct Table {
			std::array<IR::Value, 4> descriptor;     // V# dwords of the table buffer
			uint32_t                 stride     = 0; // bytes between entries
			uint32_t                 low_offset = 0; // byte offset of the target's low word
		};
		std::optional<Table> table;
		std::vector<uint64_t> table_targets; // distinct raw (unaligned) targets, last refresh
	};
	struct Linked {
		struct Function { uint32_t offset; uint32_t size; };
		std::vector<uint32_t> code;
		Decoder::Program decoded;
		std::vector<Function> functions;      // one per entry of callees
		std::vector<uint64_t> callees;        // aligned addresses in link order
		std::vector<std::vector<uint64_t>> tables; // table_targets per call site at link time
		std::vector<uint32_t> observed_function;
	};
	std::vector<uint32_t> code;
	Decoder::Program decoded;
	IR::ResourcePlan call_targets;
	std::vector<Call> calls;
	uint32_t unsupported_call_pc = UINT32_MAX;
	std::optional<Linked> linked;
	uint64_t revision = 0;
	std::vector<std::pair<uint64_t, uint64_t>> reads;
};

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
