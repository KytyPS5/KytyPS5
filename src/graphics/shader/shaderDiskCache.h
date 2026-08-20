#ifndef EMULATOR_SRC_GRAPHICS_SHADER_SHADERDISKCACHE_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_SHADERDISKCACHE_H_

#include "common/emulatorConfig.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Libs::Graphics {

// One cached translation: the SPIR-V a guest shader was recompiled into, plus
// the resource plan needed to decide whether it may be reused.
//
// The plan is NOT the whole IR. Everything downstream of a cache hit reads
// program.info, program.bindings, program.shader_hash, program.wave_size and
// program.spirv_requirements; nothing reads program.blocks or program.values.
// So those are not written, and a loaded program has them empty.
struct ShaderDiskCacheEntry {
	ShaderType                                           stage = ShaderType::Unknown;
	ShaderLaneMaskMode                 lane_mask_mode = ShaderLaneMaskMode::NativeWave;
	uint64_t                                             shader_hash = 0;
	ShaderId                                             program_id;
	Config::ShaderOptimizationType optimization_type = Config::ShaderOptimizationType::None;
	std::shared_ptr<const ShaderRecompiler::IR::Program> program;
	std::vector<uint32_t>                                spirv;
};

// Reads the cache. A missing file, a header from another build, a truncated
// file or a corrupt entry all mean the same thing: fewer entries, no error.
// Whatever comes back still has to pass the same runtime specialization check
// an in-memory permutation passes, so a bad entry costs a recompile, not a
// wrong frame.
[[nodiscard]] std::vector<ShaderDiskCacheEntry> ShaderDiskCacheLoad(
    const std::filesystem::path& path);

// Writes the cache via a temporary file and a rename, so an interrupted exit
// cannot leave a half-written file for the next run to read.
bool ShaderDiskCacheSave(const std::filesystem::path&             path,
                         const std::vector<ShaderDiskCacheEntry>& entries);

// Identifies the recompiler this build has. It mixes the git version with the
// sizes and shapes of every IR struct the plan serializes, so a cache written
// before a recompiler change is rejected rather than misread. Exposed for tests.
[[nodiscard]] uint64_t ShaderDiskCacheBuildId();

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_SHADERDISKCACHE_H_ */
