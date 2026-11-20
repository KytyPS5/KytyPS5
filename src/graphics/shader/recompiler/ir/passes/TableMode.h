#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_TABLEMODE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_TABLEMODE_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Table mode (Program::table_mode, TablePlan) for a program it can serve: its SRT reads come from slots whose addresses
// the plan derives from user data and lower slots, its descriptors' words are such slots, and it reads no memory the
// renderer cannot give it through the block. Ported from chenxiao07's KytyPS5 fork (BindingLayout.cpp
// EnterTableMode). False, with the reason, for a program it cannot serve (the program is left as it was).
bool EnterTableMode(Program& program, std::string& reason);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_TABLEMODE_H_ */
