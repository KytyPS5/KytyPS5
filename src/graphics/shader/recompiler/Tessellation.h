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

void AnalyzeTessellationPrograms(std::span<const uint32_t> local, std::span<const uint32_t> control,
                                 ShaderTessellationInputInfo& info);
void LowerTessellationMemory(IR::Program& program, const CompileOptions& options);

} // namespace Libs::Graphics::ShaderRecompiler
