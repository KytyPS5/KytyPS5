#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERCACHEFILE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERCACHEFILE_H_

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Libs::Graphics {

// The inputs of one compiled shader permutation: compiling them again gives the same program.
struct ShaderCacheRecipe {
	ShaderType            stage           = ShaderType::Unknown;
	uint32_t              user_data_count = 0;
	uint32_t              push_data_start = 0;
	uint64_t              hash            = 0;
	std::vector<uint32_t> code;
	std::vector<uint32_t> back_code;
	// The stage input info (vertex, pixel or compute) from EncodeShaderCacheInput.
	std::vector<uint8_t>                         input;
	ShaderRecompiler::IR::ResourceSpecialization specialization;

	bool operator==(const ShaderCacheRecipe&) const = default;
};

using ShaderCacheRecipes = std::vector<std::shared_ptr<const ShaderCacheRecipe>>;

// Size of the encoded input info a stage records, or 0 for a stage the cache does not hold.
size_t ShaderCacheInputSize(ShaderType stage);

// Stores the input info field by field, without padding bytes and runtime pointers, so that equal
// fields always give equal bytes. Decoding fails on a size mismatch or an invalid bool.
std::vector<uint8_t> EncodeShaderCacheInput(const ShaderVertexInputInfo& info);
std::vector<uint8_t> EncodeShaderCacheInput(const ShaderPixelInputInfo& info);
std::vector<uint8_t> EncodeShaderCacheInput(const ShaderComputeInputInfo& info);
bool DecodeShaderCacheInput(std::span<const uint8_t> data, ShaderVertexInputInfo& info);
bool DecodeShaderCacheInput(std::span<const uint8_t> data, ShaderPixelInputInfo& info);
bool DecodeShaderCacheInput(std::span<const uint8_t> data, ShaderComputeInputInfo& info);

std::vector<uint8_t> SerializeShaderCache(std::string_view          signature,
                                          const ShaderCacheRecipes& recipes);
// Appends the recipes of `data` when it starts with `signature`. Parsing stops at the first damaged
// record, and `parsed_bytes` receives the size of the valid prefix. Returns false when the
// signature does not match.
bool ParseShaderCache(std::span<const uint8_t> data, std::string_view signature,
                      ShaderCacheRecipes& recipes, size_t* parsed_bytes = nullptr);

bool ReadShaderCacheFile(const std::filesystem::path& path, std::vector<uint8_t>& data);
// Replaces `path` atomically through a uniquely named temporary file in the same directory.
bool WriteShaderCacheFile(const std::filesystem::path& path, std::span<const uint8_t> data);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERCACHEFILE_H_
