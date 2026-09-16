#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>
#include <string_view>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, uint32_t* value);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
};

enum class RuntimeValueType { Any, Integer };

// Why a value cannot be re-executed on the host. Reported next to the rejected descriptor dword
// so the log names the instruction that stopped the walk instead of only the dword index.
enum class RuntimeValueReject {
	None,
	// The chain reached an opcode the host evaluator has no rule for.
	UnsupportedOpcode,
	// A leaf operand the host cannot hold: a register handle or an opaque type.
	UnsupportedOperand,
	// Host floating point does not model the shader rounding and denormal modes.
	FloatInIntegerChain,
	// Wrong arity, a non-immediate index, or a handle of the wrong kind.
	MalformedInstruction,
	// An undefined or void value.
	UndefinedValue,
	// A definition cycle whose phi carries a different value each iteration, so no single
	// descriptor stands for it.
	CyclicValue,
	// A loop-carried phi web no operand enters from outside, so the loop is never entered with a
	// value the host could stand in for. Only a GPU-side descriptor can serve this.
	CyclicValueNoEntry,
	// A loop-carried phi web two different values enter from outside: a merge rather than a loop.
	// A single host binding would have to stand for both.
	CyclicValueMerge,
	// Not a 32-bit scalar, so it cannot be a descriptor dword at all.
	NonScalarType,
};

// Trivially copyable and default constructed by the caller, so recording a reason allocates
// nothing and costs nothing on the accepting path.
struct RuntimeValueFailure {
	RuntimeValueReject reason     = RuntimeValueReject::None;
	ValueOpcode        opcode     = ValueOpcode::Void;
	bool               has_opcode = false;
	// On CyclicValueMerge, the two operands that enter the phi web and disagree. Naming them is
	// what separates "two spellings of one descriptor" from "two genuinely different buffers".
	// Void stands for a leaf with no instruction behind it, which for a descriptor dword is an
	// immediate.
	ValueOpcode        entry_opcode      = ValueOpcode::Void;
	ValueOpcode        other_opcode      = ValueOpcode::Void;
	bool               has_entry_opcodes = false;
};

[[nodiscard]] std::string_view RuntimeValueRejectName(RuntimeValueReject reason);

// Collects reachable ReadConst values. Immediate offsets receive compact flat-buffer slots;
// dynamic offsets remain explicit and are never assigned a fake slot.
void BuildSrtPlan(Program& program);
// Optionally reports why the first rejected instruction could not be re-executed. Pass a sink
// only where that reason is logged: it is written at most once, and only when validation fails.
bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType     type    = RuntimeValueType::Any,
                          RuntimeValueFailure* failure = nullptr);
bool EvaluateUniformValues(const ResourcePlan& program, std::span<const Value> values,
                            const SrtRuntime& runtime, std::span<uint32_t> results);

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result);

// Evaluates one runtime snapshot transactionally. Scalar values and ReadConst results shared by
// several descriptors are memoized once across the batch.
bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results);

// Evaluates potentially reachable descriptor sources and the flattened immediate SRT with one
// memoized scalar walk. Inactive descriptors are zero; on failure no destination is changed.
bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots,
                            std::vector<uint8_t>& active_sources);

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime,
             std::vector<uint32_t>& flat);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
