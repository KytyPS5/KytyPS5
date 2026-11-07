#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <csetjmp>
#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Tracking ends the emulator when it meets a shader it cannot follow. A caller that can do without
// that shader sets a recovery point on its thread first: the failure then stores its message here
// and jumps back to `point`, as this build has no exceptions. The frames in between are dropped
// without being unwound, which leaks what they held; the caller must not hold a lock across them.
struct TrackingRecovery {
	std::jmp_buf point;
	std::string  message;
};
void SetTrackingRecovery(TrackingRecovery* recovery) noexcept;

// Resolves native descriptor sources, plans their scalar reads, and assigns dense resource bindings.
void TrackResources(Program& program, const Decoder::Program& decoded, const CFG::Graph& native_cfg,
                    ShaderStageInputInfo input);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_ */
