#ifndef KYTY_GRAPHICS_SHADER_RECOMPILER_SPIRV_OPTIMIZER_H_
#define KYTY_GRAPHICS_SHADER_RECOMPILER_SPIRV_OPTIMIZER_H_

#include "common/emulatorConfig.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// None is byte-preserving. Failed optimization leaves the original module intact.
[[nodiscard]] bool Optimize(std::vector<uint32_t>& code, Config::ShaderOptimizationType mode,
                            std::string& diagnostics);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif // KYTY_GRAPHICS_SHADER_RECOMPILER_SPIRV_OPTIMIZER_H_
