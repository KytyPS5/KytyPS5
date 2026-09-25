#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERPRECOMPILE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERPRECOMPILE_H_

#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderCompiler.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace Libs::Graphics::ShaderPrecompile {

// Records everything needed to recreate one shader permutation without the guest running.
//
// The replay side (stage B) rebuilds CompileOptions from these fields and calls
// TranslateProgram + CompileProgram directly, injecting the stored specialization. That works
// because ResourceSpecialization holds only enums and integers: unlike ResourceSnapshot it
// carries no guest addresses, so it survives a round trip through a file. MaterializeResources,
// which does need live guest memory, is never called on the replay path.

// append=false rewrites the file with a fresh header; true keeps the existing set and adds
// to it, so an interrupted run can never destroy what earlier runs recorded.
void Open(const std::filesystem::path& path, const std::string& recompiler_key, bool append);
void Close();

// No-ops when Open() was not called or the file could not be created.
void Record(const ShaderParams& params, const ShaderRecompiler::CompileOptions& options,
            const ShaderRecompiler::IR::ResourceSpecialization& specialization,
            uint32_t push_data_start_dword, const ShaderVertexInputInfo& info);
void Record(const ShaderParams& params, const ShaderRecompiler::CompileOptions& options,
            const ShaderRecompiler::IR::ResourceSpecialization& specialization,
            uint32_t push_data_start_dword, const ShaderPixelInputInfo& info);
void Record(const ShaderParams& params, const ShaderRecompiler::CompileOptions& options,
            const ShaderRecompiler::IR::ResourceSpecialization& specialization,
            uint32_t push_data_start_dword, const ShaderComputeInputInfo& info);

uint64_t RecordedCount();

// One decoded permutation. `info` carries the stage's InputInfo with its runtime `stage` member
// left default-constructed: the replay fills that in itself after compiling.
struct PermutationRecord {
	ShaderType                                   stage                 = ShaderType::Unknown;
	uint64_t                                     hash                  = 0;
	uint32_t                                     push_data_start_dword = 0;
	uint32_t                                     user_data_base        = 0;
	uint32_t                                     wave_size             = 64;
	std::vector<uint32_t>                        code;
	std::vector<uint32_t>                        back_code;
	std::vector<uint32_t>                        user_data;
	ShaderRecompiler::IR::ResourceSpecialization specialization;
	std::variant<ShaderVertexInputInfo, ShaderPixelInputInfo, ShaderComputeInputInfo> info;
};

// Empty when the file is absent, truncated, or written by a different recompiler build.
std::vector<PermutationRecord> Load(const std::filesystem::path& path,
                                    const std::string&           recompiler_key);

} // namespace Libs::Graphics::ShaderPrecompile

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERPRECOMPILE_H_
