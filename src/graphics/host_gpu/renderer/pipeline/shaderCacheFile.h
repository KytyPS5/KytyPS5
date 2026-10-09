#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERCACHEFILE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERCACHEFILE_H_

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
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
	// Bytes of the stage input info (vertex, pixel or compute) without its runtime pointers.
	std::vector<uint8_t>                         input;
	ShaderRecompiler::IR::ResourceSpecialization specialization;

	bool operator==(const ShaderCacheRecipe&) const = default;
};

using ShaderCacheRecipes = std::vector<std::shared_ptr<const ShaderCacheRecipe>>;

static_assert(std::is_trivially_copyable_v<ShaderVertexInputInfo>);
static_assert(std::is_trivially_copyable_v<ShaderPixelInputInfo>);
static_assert(std::is_trivially_copyable_v<ShaderComputeInputInfo>);
static_assert(std::is_trivially_copyable_v<ShaderRecompiler::IR::ResourceSpecialization::Buffer>);
static_assert(std::is_trivially_copyable_v<ShaderRecompiler::IR::ResourceSpecialization::Image>);

// Size of the input info a stage records, or 0 for a stage the cache does not hold.
size_t ShaderCacheInputSize(ShaderType stage);

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
