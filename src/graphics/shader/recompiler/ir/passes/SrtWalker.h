#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);

// Bytes readable from address, capped at size. A guest descriptor may declare an arena far larger
// than the pages behind it, and enumerating the declared size probes memory that cannot answer.
using SrtExtentQuery = uint64_t (*)(void* userdata, uint64_t address, uint64_t size);

// Names why a read refused, for the refusal message only. Optional: the walk never consults it.
using SrtReadRefusalDescriber = const char* (*)(void* userdata, uint64_t address);

// Reads a contiguous block. Every single-word read re-checks GPU ownership of its four bytes, which
// costs a cache lock and a region search; enumerating a table pays that tens of thousands of times.
using SrtBlockReader = bool (*)(void* userdata, uint64_t address, void* data, uint64_t size);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	SrtExtentQuery            readable_extent            = nullptr;
	SrtReadRefusalDescriber   describe_read_refusal      = nullptr;
	SrtBlockReader            read_specialization_block  = nullptr;
	std::span<const uint32_t> workgroup_counts;
};

// Records every guest range a walk reads, so a caller can revalidate them later.
class SrtReadCapture {
public:
	SrtReadCapture(SrtRuntime source, std::vector<std::pair<uint64_t, uint64_t>>& ranges):
	    m_source(source), m_ranges(ranges) {}
	SrtRuntime ObservedRuntime();

private:
	static bool ReadStrict(void* userdata, uint64_t address, std::span<uint32_t> values);
	static bool ReadOrdinary(void* userdata, uint64_t address, std::span<uint32_t> values);

	SrtRuntime                                  m_source;
	std::vector<std::pair<uint64_t, uint64_t>>& m_ranges;
};

enum class RuntimeValueType { Any, Integer, ImmutableInteger };

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

// Optionally reports why the first rejected instruction could not be re-executed. Pass a sink
// only where that reason is logged: it is written at most once, and only when validation fails.
bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType     type    = RuntimeValueType::Any,
                          RuntimeValueFailure* failure = nullptr);
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);


// One pass of the per-lane sweep a readfirstlane runs. `dependent` stays clear for an operand
// that never asks for the lane, which is every shader that does not go through the mask model,
// so those pay one walk exactly as before.
struct LaneScope {
	uint32_t lane      = 0;
	bool     dependent = false;
};

// One memoized evaluation session shared by the entire shader resource refresh.
class SrtWalker {
public:
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::span<const uint8_t> clean_flat_slots = {}, SrtWalker* clean_evaluator = nullptr,
	          Value active_mask = {});
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);
	// Refreshes reachable scalar reads and active descriptor sources in one walk.
	bool RefreshFlatBuffer(std::vector<uint32_t>& flat);

private:
	static ResourcePlan::EvaluationContext& AcquireContext(const ResourcePlan& program);
	static float Float32(uint64_t bits);
	bool EvaluateWide(Value value, uint64_t& result);
	bool Arg(const Inst& inst, size_t index, uint64_t& result);
	bool EvaluatePhi(const Inst& inst, uint64_t& result);
	bool EvaluateExtract(const Inst& inst, uint64_t& result);
	bool EvaluateExtractU32x4(const Inst& inst, uint32_t component, uint64_t& result);
	bool EvaluateRawRead(const Inst& inst, uint64_t& result, uint32_t component_bytes = 0u);
	bool EvaluateInst(const Inst& inst, uint64_t& result);

	const ResourcePlan&              m_program;
	SrtRuntime                      m_runtime;
	std::span<const uint8_t>         m_clean_flat_slots;
	SrtWalker*                      m_clean_evaluator = nullptr;
	Value                           m_active_mask;
	ResourcePlan::EvaluationContext& m_context;
	// Loop-carried phi values taken on trust, inherited by any trial this walk starts.
	std::unordered_map<const Inst*, uint64_t> m_assumed;
	std::unordered_set<const Inst*>           m_barred;
	// Non-null only inside the per-lane sweep a readfirstlane runs, which is the one place a
	// lane index has a value. Owned by the sweep, shared with any walk it starts.
	LaneScope*                                m_lane = nullptr;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
