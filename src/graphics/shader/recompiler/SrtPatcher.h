#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTPATCHER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTPATCHER_H_

#include "graphics/shader/recompiler/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Replaces each immediate ReadConst producer selected by the SRT plan with a dense flat-buffer
// dword load. Dynamic-offset reads remain explicit. On failure no instruction is changed.
bool PatchSrtReads(Program* program, std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTPATCHER_H_ */
