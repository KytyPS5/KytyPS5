#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/shader.h"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {

struct CompileOptions {
	ShaderType                  stage           = ShaderType::Compute;
	uint32_t                    wave_size       = 64;
	// See IR::ResourcePlan::host_subgroup_size.
	uint32_t                    host_subgroup_size = 64;
	// VK_EXT_robustness2 on the device: buffer accesses then need no range test of their own.
	bool                        robust_buffer_access2      = false;
	uint32_t                    user_data_base  = 0;
	uint64_t                    shader_hash     = 0;
	bool                        dump_ir                    = true;
	bool                        early_dump                 = false;
	const char*                 dump_label                 = nullptr;
	std::span<const uint32_t>   user_data;
	std::span<const uint32_t>   back_code;
	ShaderStageInputInfo        input_info;
	// `first_use_pc` of every resource a previous materialization of this shader proved the host
	// cannot re-evaluate. Empty on the first translate of a shader; the caller grows it when a
	// draw reports a degrade and re-translates once. See ResourcePlan::unfoldable_pcs.
	std::span<const uint32_t>   unfoldable_pcs;
	// See Program::refuse_indirect_buffer_table. Read by resource tracking only, and dropped
	// before TranslateProgram returns, so it may capture state that lives only for this call.
	std::function<bool(const IR::Program& program, std::span<const IR::DescriptorSource> sources,
	                   uint32_t source)>
	    refuse_indirect_buffer_table;
};

// Why a guest shader could not be recompiled: a CFG-build or resource-tracking rejection that
// describes the guest program, not a broken emulator invariant. The emulator is built without
// exceptions, so a rejection travels back as a value and the caller drops the draw.
struct RecompileStatus {
	bool        ok = true;
	uint32_t    pc = 0;
	std::string reason;
};

struct TranslateResult {
	IR::Program program;
	std::string decoded_dump;
	std::string cfg_dump;
	// Not ok when the guest shader could not be recompiled. The program is then unusable - either
	// empty, or left half-rewritten by the rejected pass - and the caller skips the draw.
	RecompileStatus status;
};

struct CompileResult {
	std::vector<uint32_t>  spirv;
	std::string            decoded_dump;
	std::string            ir_dump;
	IR::Program            program;
	// Not ok when the backend cannot express the guest program. The shader is then dropped, the
	// same way a rejected translation is, rather than ending the session.
	RecompileStatus        status;
};

// What the structurized-CFG cache has done this session. The cache is meant to be invisible -
// a hit and a build produce the same graph - so this is the only way a run or a test can tell the
// two apart, and it is the number that says what the cache is worth.
struct CfgCacheStats {
	uint64_t hits    = 0;
	uint64_t misses  = 0;
	uint64_t evicted = 0;
	uint64_t refused = 0;
	uint64_t entries = 0;
	uint64_t bytes   = 0;
	uint64_t loaded   = 0;
	uint64_t stored   = 0;
	uint64_t rejected = 0;
	uint64_t hits_from_file    = 0;
	uint64_t avoided_file_us   = 0;
	uint64_t avoided_memory_us = 0;
};

[[nodiscard]] CfgCacheStats CfgCacheStatistics();

// Backs the structurized-CFG cache with a file. `stamp` must change whenever CFG::BuildGraph or
// CFG::Structurize changes behaviour, or CFG::Graph's layout changes; 0 disables the file.
void OpenCfgCacheFile(const std::string& path, uint64_t stamp);

// Write the file out. Safe to call repeatedly; does nothing when no file is configured.
void FlushCfgCacheFile();

// Empties the in-memory cache and forgets any file. Tests only.
void ResetCfgCacheForTest();
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
// Links the SWAPPC leaf when it can and returns the program to translate. A call it cannot link
// returns `source.decoded` unchanged, whose S_SWAPPC_B64 the translator lowers to the any-hit
// stub; callers translate the returned program rather than `source.linked`.
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
