#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_PARAMETERKEYS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_PARAMETERKEYS_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Retains, in plan, each parameter component a vertex stage exports as a function the host can
// evaluate: integer work on user data, scalar memory and buffer records the vertex index selects.
// An export the host cannot evaluate leaves an empty value for its parameter.
void BuildParameterKeys(const Program& program, ResourcePlan& plan);

// The uniform values parameter's keys depend on, besides the record buffers EvaluateParameterKeys
// reports: equal signatures over unchanged buffers give equal keys. False when keys are unknown.
bool ParameterKeySignature(const ResourcePlan& plan, const SrtRuntime& runtime, uint32_t parameter,
                           std::vector<uint32_t>& signature);

// Every value parameter can take: the function over each record of the buffers it loads by
// vertex index, and with every such load out of range. Sorted and unique. reads receives the
// record buffer ranges read. False when the values are not known exactly.
bool EvaluateParameterKeys(const ResourcePlan& plan, const SrtRuntime& runtime, uint32_t parameter,
                           std::vector<uint32_t>&                      keys,
                           std::vector<std::pair<uint64_t, uint64_t>>& reads);

// The opcodes parameter key functions may contain; exposed for tests.
bool IsParameterKeyOpcode(ValueOpcode opcode);
// Evaluates an integer or logical opcode of a key function on operand bits; exposed for tests.
bool EvaluateParameterKeyOp(ValueOpcode opcode, std::span<const uint32_t> args, uint32_t& result);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_PARAMETERKEYS_H_ */
