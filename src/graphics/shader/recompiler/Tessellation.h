#pragma once

#include <cstdint>
#include <span>

namespace Libs::Graphics {
struct ShaderTessellationInputInfo;
}

namespace Libs::Graphics::ShaderRecompiler {

struct CompileOptions;
namespace IR {
struct Program;
}

// False when the interface cannot be reflected; the strides are then left at zero and the
// tessellated draw is skipped.
[[nodiscard]] bool AnalyzeTessellationPrograms(std::span<const uint32_t> local,
                                               std::span<const uint32_t> control,
                                               ShaderTessellationInputInfo& info);
// False when the program uses tessellation memory in a way this lowering cannot express.
[[nodiscard]] bool LowerTessellationMemory(IR::Program& program, const CompileOptions& options);

} // namespace Libs::Graphics::ShaderRecompiler
