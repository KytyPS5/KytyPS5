#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {
struct ShaderTessellationInputInfo;
}

namespace Libs::Graphics::ShaderRecompiler {

struct CompileOptions;
namespace IR {
struct Program;
}

struct MergedHullHalves {
	std::vector<uint32_t> local;
	std::vector<uint32_t> control;
};

MergedHullHalves SplitMergedHullProgram(std::span<const uint32_t> code);
void AnalyzeTessellationPrograms(std::span<const uint32_t> local, std::span<const uint32_t> control,
                                 ShaderTessellationInputInfo& info);
void LowerTessellationMemory(IR::Program& program, const CompileOptions& options);

} // namespace Libs::Graphics::ShaderRecompiler
