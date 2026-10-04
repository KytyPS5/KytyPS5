#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVDRIVERSIMPLIFY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVDRIVERSIMPLIFY_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

struct DriverSimplifyStats {
	uint32_t speculated_branches = 0;
	uint32_t clamped_loads       = 0;
	uint32_t forwarded_operands  = 0;
	uint32_t removed             = 0;
	// Values replaced by an equal value: a local identity (KYTY_NO_SPV_PEEPHOLE) or a dominating
	// duplicate (KYTY_NO_SPV_CSE).
	uint32_t folded       = 0;
	uint32_t deduplicated = 0;
	// Blocks whose instructions were moved next to their users (KYTY_NO_SPV_SINK).
	uint32_t sunk_blocks = 0;
};

bool SimplifyForDriver(std::vector<uint32_t>& words, DriverSimplifyStats* stats = nullptr);

uint32_t DriverSimplifyMinWords();
void     SetDriverSimplifyMinWords(uint32_t words);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVDRIVERSIMPLIFY_H_ */
