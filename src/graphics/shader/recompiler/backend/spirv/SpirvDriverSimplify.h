#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVDRIVERSIMPLIFY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVDRIVERSIMPLIFY_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

bool SimplifyForDriver(std::vector<uint32_t>& words);

uint32_t DriverSimplifyMinWords();
void     SetDriverSimplifyMinWords(uint32_t words);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVDRIVERSIMPLIFY_H_ */
