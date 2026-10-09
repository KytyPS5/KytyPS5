#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERDISKCACHE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERDISKCACHE_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>
#include <span>
#include <vector>

// Serialization of the deterministic part of a recompiler result, for the on-disk shader cache.
// Nothing here depends on user data or guest memory: the resource plan is the value graph that
// MaterializeResources evaluates at run time, and a permutation is the SPIR-V plus the binding
// metadata the renderer needs. Readers are bounds-checked and return false on any malformed input.
namespace Libs::Graphics::ShaderRecompiler::DiskCache {

// Bumped by hand when the layout written by this file changes. Changes to the recompiler itself are
// covered by the source hash that the caller mixes into the file key.
inline constexpr uint32_t kFormatVersion = 1;

// Empty result: the plan holds values this format cannot express; do not cache it.
[[nodiscard]] std::vector<uint8_t> SerializePlan(const IR::ResourcePlan& plan, bool ray_traced);
[[nodiscard]] bool DeserializePlan(std::span<const uint8_t> data, IR::ResourcePlan& plan,
                                   bool& ray_traced);

[[nodiscard]] std::vector<uint8_t> SerializePermutation(const IR::CompiledShaderInfo& info,
                                                        std::span<const uint32_t>     spirv);
[[nodiscard]] bool DeserializePermutation(std::span<const uint8_t> data,
                                          IR::CompiledShaderInfo& info,
                                          std::vector<uint32_t>&  spirv);

} // namespace Libs::Graphics::ShaderRecompiler::DiskCache

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERDISKCACHE_H_ */
