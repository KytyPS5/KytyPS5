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
	bool                        emit_debug_info            = false;
	const char*                 dump_label                 = nullptr;
	std::span<const uint32_t>   user_data;
	std::span<const uint32_t>   back_code;
	ShaderStageInputInfo        input_info;
	// The renderer passes what its device enabled. The default is for tests and offline use.
	HostGpu host = HostGpu::Full();
};

struct TranslateResult {
	IR::Program program;
	std::string decoded_dump;
	std::string cfg_dump;
};

struct CompileResult {
	std::vector<uint32_t>  spirv;
	// An emulated mesh program also needs a vertex shader and the size of its records.
	std::vector<uint32_t>  mesh_vertex_spirv;
	uint32_t               mesh_slot_words = 0;
	std::string            decoded_dump;
	std::string            ir_dump;
	IR::Program            program;
};

// Decoded source and typed scalar call queries are retained once per cache source.
struct ShaderSource {
	struct Call {
		// A call through a table of functions: the target is two dwords of a scalar buffer load
		// whose offset, the entry, changes per call (a waterfall over the lanes' entries).
		struct Table {
			uint32_t load   = 0; // The load's instruction.
			uint32_t dword  = 0; // The dword of an entry that holds the target's low half.
			uint32_t stride = 0; // Bytes from one entry to the next.
			int32_t  offset = 0; // The load's immediate offset.
			std::array<IR::Value, 4> descriptor;
		};
		uint32_t instruction;
		std::array<IR::Value, 2> target;
		std::optional<Table> table;
		std::bitset<128> preserved_registers;
		uint64_t address = 0;
	};
	struct Linked {
		struct Function { uint32_t offset; uint32_t size; };
		std::vector<uint32_t> code;
		Decoder::Program decoded;
		std::vector<Function> functions;
		std::vector<uint32_t> observed_function;
		// A table call is named by the targets of its entries and the code of every function it
		// links (observed_function holds both, in this order); the entries' other words, such as
		// the data each function gets, change nothing. observed_ranges are the functions' code.
		std::vector<std::pair<uint32_t, uint32_t>> table_targets;
		std::vector<std::pair<uint64_t, uint32_t>> observed_ranges;
		// Names this linking for as long as it lives, parked or not.
		uint64_t id = 0;
	};
	std::vector<uint32_t> code;
	Decoder::Program decoded;
	IR::ResourcePlan call_targets;
	std::vector<Call> calls;
	uint32_t unsupported_call_pc = UINT32_MAX;
	uint32_t wave_size = 64; // Of the shader: a vector compare writes one scalar register in wave32.
	std::optional<Linked> linked;
	// A table call links what one dispatch's table holds; dispatches with other tables take turns.
	// The linkings they left are kept, so that the next dispatch of a known table links nothing.
	std::vector<Linked> parked_links;
	uint64_t link_count = 0;
	uint64_t revision = 0;
	std::vector<std::pair<uint64_t, uint64_t>> reads;
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
