#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {

struct CompileOptions {
	ShaderType                    stage           = ShaderType::Compute;
	ShaderLaneMaskMode            lane_mask_mode  = ShaderLaneMaskMode::NativeWave;
	uint32_t                      wave_size       = 64;
	uint32_t                      user_data_base  = 0;
	uint32_t                      user_data_count = 64;
	uint64_t                      shader_hash     = 0;
	uint64_t                      shader_base     = 0;
	std::optional<uint64_t>       flat_memory_base;
	uint32_t                      descriptor_set       = 0;
	uint32_t                      push_constant_offset = 0;
	bool                          dump_ir              = true;
	bool                          early_dump           = false;
	const char*                   dump_label           = nullptr;
	const uint32_t*               user_data            = nullptr;
	IR::SrtMemoryReader           read_memory          = nullptr;
	void*                         read_memory_data     = nullptr;
	const IR::ResourceSnapshot*   resource_snapshot    = nullptr;
	const ShaderVertexInputInfo*  vertex_input_info    = nullptr;
	const ShaderPixelInputInfo*   pixel_input_info     = nullptr;
	const ShaderComputeInputInfo* compute_input_info   = nullptr;
};

struct CompileResult {
	std::vector<uint32_t> spirv;
	std::string           decoded_dump;
	std::string           ir_dump;
	IR::Program           program;
	IR::ResourceSnapshot  resources;
};

bool TryRecompile(std::span<const uint32_t> code, const CompileOptions& options,
                  CompileResult* result, std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERRECOMPILER_H_ */
