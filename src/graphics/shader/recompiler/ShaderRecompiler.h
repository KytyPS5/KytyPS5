#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/shader.h"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {

struct RtFeedbackSite;

struct CompileOptions {
	ShaderType                  stage           = ShaderType::Compute;
	uint32_t                    wave_size       = 64;
	uint32_t                    user_data_base  = 0;
	uint64_t                    shader_hash     = 0;
	bool                        dump_ir                    = true;
	bool                        early_dump                 = false;
	bool                        emit_debug_info            = false;
	bool                        maintenance8_enabled       = false;
	const char*                 dump_label                 = nullptr;
	std::span<const uint32_t>   user_data;
	std::span<const uint32_t>   back_code;
	ShaderStageInputInfo        input_info;
	// When a program of the cache is translated again for another specialization, its ray tracing call sites
	// must inline exactly what the cached program did: the feedback state may have moved on since.
	const std::vector<RtFeedbackSite>* fixed_sites = nullptr;
};

struct CallInputRead {
	uint64_t address = 0;
	bool relative_to_srt = false;
	// When set, `address` is a byte offset into the call table whose V# sits at this SRT offset; the table
	// moves between frames, its base is read again every time.
	uint32_t table_vsharp_srt_offset = UINT32_MAX;
	std::vector<uint32_t> words;
};

// A call site whose table is inlined by feedback (RtFeedback.h): the usage slot its kernel logs to and the
// records that were inlined, ascending.
struct RtFeedbackSite {
	uint32_t              slot = 0;
	std::vector<uint32_t> records;
	// Shared-body sites inline one copy per function. The functions (low address dwords, ascending), where the
	// table's V# sits in the SRT, and the key the learned function set is stored under.
	bool                  shared = false;
	uint64_t              site_hash = 0;
	uint32_t              pc = 0;
	uint32_t              table_srt_offset = 0;
	std::vector<uint32_t> functions;
	// Group mode: only the functions of this group are inlined (group_count == 0 when not split).
	uint32_t              group_pass  = 0;
	uint32_t              group_count = 0;
};

// The group a call-table function belongs to when the table is split into `groups` groups: stable under
// growth of the learned function set, which is why it is a hash of the address and not a position.
inline uint32_t CallGroupOf(uint32_t function_low, uint32_t groups) {
	return groups <= 1u ? 0u : ((function_low * 0x9e3779b1u) >> 12u) % groups;
}

struct TranslateResult {
	IR::Program program;
	// Guest call records whose contents were embedded in the translated program.
	std::vector<CallInputRead> call_inputs;
	// Feedback call sites of this program; the cache retranslates it when the GPU logs another record.
	std::vector<RtFeedbackSite> rt_sites;
	// The shader selects descriptors by its workgroup z index, so it has to be compiled once per
	// z slice (CompileOptions input_info.compute->fixed_group_z). Not a failure.
	bool        needs_group_z = false;
	// The shader calls through a table of functions that is split into this many groups (--raytracing); the
	// host requests one program per group (input_info.compute->call_pass) and dispatches them one after the other.
	uint32_t    needs_call_passes = 0;
	// The shader calls through S_SWAPPC_B64 with a return address (ray tracing) in a shape that is not expanded.
	// Function calls are not translated, so the program is not produced.
	bool        uses_function_calls = false;
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
		std::array<IR::Value, 2> target;
	};
	struct Linked {
		std::vector<uint32_t> code;
		Decoder::Program decoded;
		std::vector<uint32_t> observed_function;
	};
	std::vector<uint32_t> code;
	Decoder::Program decoded;
	IR::ResourcePlan call_targets;
	std::optional<Call> call;
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
