#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_WATERFALLDESCRIPTOR_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_WATERFALLDESCRIPTOR_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct WaterfallDescriptor {
	const Inst* mask_phi = nullptr;
	const Inst* index    = nullptr;
	Value       entry;
	Value       key;
	const Inst* compare  = nullptr;
	const Inst* scaled   = nullptr;
	size_t      latch    = 0;
	const Inst* handle   = nullptr;
	Value       heap;
	uint32_t    table_offset = 0;
	uint32_t    stride_shift = 0;

	bool operator==(const WaterfallDescriptor& other) const = default;
};

std::vector<WaterfallDescriptor> FindWaterfallDescriptors(const Program& program);

uint32_t RewriteWaterfallDescriptors(Program& program);

}

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_WATERFALLDESCRIPTOR_H_ */
