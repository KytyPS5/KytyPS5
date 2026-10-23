#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Why a guest shader could not be tracked: the caller drops the draw instead of terminating.
struct ResourceTrackingStatus {
	bool        ok = true;
	uint32_t    pc = 0;
	std::string reason;
};

// Resolves native descriptor sources, plans their scalar reads, and assigns dense resource bindings.
ResourceTrackingStatus TrackResources(Program& program, const Decoder::Program& decoded,
                                      const CFG::Graph& native_cfg, ShaderStageInputInfo input);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_ */
