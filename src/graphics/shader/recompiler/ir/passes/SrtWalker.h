#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	std::span<const uint32_t> workgroup_counts;
};

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

// Retained reads no longer need the guest instruction PC. A clean read evaluates
// its address and bounds through the strict reader as well as the final DWORD.
struct SrtReadFlags {
	uint32_t index = 0;
	uint32_t clean = 0;
};

enum class RuntimeValueType { Any, Integer, ImmutableInteger };

bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
// Uniform across a compute wave, including values derived from its workgroup ID.
// This does not imply that the host can evaluate the value.
bool ValidateWaveUniformValue(const ResourcePlan& program, Value value);
void CompileSrtWalker(ResourcePlan& program);
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);

// Nonnegative affine coefficients: element zero is constant, remaining elements are
// caller-selected leaves. Runtime range proofs must additionally exclude U32 wrap.
template <size_t Size>
std::optional<std::array<uint64_t, Size>> AffineIndex(
    Value value, const auto& resolve, const auto& leaf, uint32_t depth = 0) {
	value = resolve(value);
	if (depth > 32u || value.GetType() != Type::U32) return {};
	std::array<uint64_t, Size> result {};
	if (value.IsImmediate()) {
		result[0] = value.U32();
		return result;
	}
	if (const auto index = leaf(value)) {
		result[*index] = 1u;
		return result;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return {};
	const auto op = inst->GetOpcode();
	if (op != ValueOpcode::IAdd32 && op != ValueOpcode::IMul32 &&
	    op != ValueOpcode::ShiftLeftLogical32) return {};
	auto left = AffineIndex<Size>(inst->Arg(0), resolve, leaf, depth + 1u);
	auto right = AffineIndex<Size>(inst->Arg(1), resolve, leaf, depth + 1u);
	if (!left || !right) return {};
	const auto constant = [](const auto& coefficients) {
		return std::ranges::all_of(std::span(coefficients).subspan(1u),
		                          [](uint64_t coefficient) { return coefficient == 0u; });
	};
	if (op == ValueOpcode::IMul32 && !constant(*right)) std::swap(left, right);
	if (op != ValueOpcode::IAdd32 && !constant(*right)) return {};
	if (op == ValueOpcode::ShiftLeftLogical32) {
		if ((*right)[0] >= 32u) return {};
		(*right)[0] = uint64_t {1} << (*right)[0];
	}
	for (size_t index = 0; index < Size; ++index) {
		(*left)[index] = op == ValueOpcode::IAdd32
		    ? (*left)[index] + (*right)[index] : (*left)[index] * (*right)[0];
		if ((*left)[index] > UINT32_MAX) return {};
	}
	return left;
}

// One memoized evaluation session shared by the entire shader resource refresh.
// The values of a plan as small nodes in one array, for the walker. The walker touches a few
// bytes of each value it evaluates; the plan's Inst objects are large and scattered, so a walk
// over them is bound by cache misses. Node i is the value with evaluation index i (Build numbers
// every value of the plan). Only the walker reads it; the plan's values stay the reference.
class SrtNodes {
public:
	// Null: the plan has values the nodes cannot express, and the walker walks the values.
	static std::shared_ptr<const SrtNodes> Build(const ResourcePlan& program);

	// SetEnabled(false): walkers ignore the nodes (A/B and differential tests).
	static void SetEnabled(bool enabled);
	// For this thread only: a differential check runs the same walk without the nodes.
	static void SetThreadDisabled(bool disabled);
	[[nodiscard]] static bool Enabled();
	// KYTY_SRT_VERIFY: MaterializeResources checks each walk over the nodes against a walk over
	// the values, and stops the emulator when they differ.
	static void SetVerify(bool verify);
	[[nodiscard]] static bool Verify();

private:
	friend class SrtWalker;
	friend class SrtNodesBuilder;

	// A reference to a value: a node index, or (with ConstantBit) a constant index.
	static constexpr uint32_t ConstantBit = 0x80000000u;
	static constexpr uint32_t NoRef       = UINT32_MAX;

	// What SrtWalker::EvaluateInst does for the value.
	enum class Kind : uint8_t {
		Fail,            // never evaluates
		Exit,            // the walker stops the emulator (a malformed value)
		Pure,            // args[0..arity) through ApplyPure
		UserData,        // aux: user data index
		ShaderBase,
		Phi,             // extra[aux .. aux + 2 * count): predecessor block, value
		InvariantPhi,    // args[0]: the value every operand resolves to
		ReadFirstLane,   // args[0]: value, args[1]: EXEC mask
		ExtractU64,      // args[0]: packed, aux: component
		ExtractConstruct,// args[0]: the chosen part
		ExtractAddCarry, // args[0], args[1]: addends, aux: component
		RawRead,         // args: low, high, offset, guard; extra[aux]: records, word3
		Select,          // args[0]: condition, args[1], args[2]
		LogicalAnd,
		LogicalOr,
	};

	// RawRead bits in Node::arity.
	static constexpr uint8_t ReadClean        = 1u;
	static constexpr uint8_t ReadVector       = 2u;
	static constexpr uint8_t ReadGuardImm     = 4u;
	static constexpr uint8_t ReadBounded      = 8u;
	static constexpr uint8_t ReadHandleBounds = 16u;

	struct Node {
		ValueOpcode             op    = ValueOpcode::Identity;
		Kind                    kind  = Kind::Fail;
		uint8_t                 arity = 0;
		uint32_t                aux   = 0;
		uint64_t                flags = 0;
		std::array<uint32_t, 4> args {NoRef, NoRef, NoRef, NoRef};
	};
	static_assert(sizeof(Node) == 32);

	struct Constant {
		uint64_t bits = 0;
		bool     ok   = false;
	};

	std::vector<Node>        m_nodes;
	std::vector<uint32_t>    m_extra;
	std::vector<Constant>    m_constants;
	// For walkers that need a Value again (a nested EXEC mask).
	std::vector<Value>       m_constant_values;
	std::vector<const Inst*> m_insts;
	// The roots, per srt_reads entry, per descriptor source dword, per control block.
	std::vector<uint32_t>                m_read_refs;
	std::vector<uint8_t>                 m_read_clean;
	std::vector<std::array<uint32_t, 8>> m_descriptor_refs;
	std::vector<uint32_t>                m_condition_refs;
};

class SrtWalker {
public:
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          SrtWalker* clean_evaluator = nullptr);
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);
	// Refreshes reachable scalar reads and active descriptor sources in one walk.
	bool RefreshFlatBuffer(std::vector<uint32_t>& flat);

private:
	SrtWalker(SrtWalker& parent, Value active_mask, bool clean = false,
	          SrtWalker* clean_evaluator = nullptr);
	ResourcePlan::EvaluationContext& AcquireContext();
	void ResetUnknowns();
	bool EvaluateWide(Value value, uint64_t& result);
	bool Arg(const Inst& inst, size_t index, uint64_t& result);
	bool EvaluatePhi(const Inst& inst, uint64_t& result);
	bool EvaluateExtract(const Inst& inst, uint64_t& result);
	bool EvaluateRawRead(const Inst& inst, uint64_t& result);
	bool EvaluateInst(const Inst& inst, uint64_t& result);
	// The same evaluation over SrtNodes.
	bool EvaluateRef(uint32_t ref, uint64_t& result);
	bool EvaluateNode(uint32_t index, uint64_t& result);
	bool EvaluateNodeRawRead(const SrtNodes::Node& node, uint64_t& result);
	[[nodiscard]] uint32_t RefOf(Value value) const;
	[[nodiscard]] Value    ValueOf(uint32_t ref) const;

	const ResourcePlan&              m_program;
	const SrtNodes*                 m_nodes = nullptr;
	uint32_t                        m_active_ref = SrtNodes::NoRef;
	SrtRuntime                      m_runtime;
	SrtWalker*                      m_clean_evaluator = nullptr;
	Value                           m_active_mask;
	const std::vector<uint8_t>*      m_reachable_blocks = nullptr;
	const SrtWalker*                m_session;
	uint8_t                         m_read_policy;
	ResourcePlan::EvaluationContext& m_context;
};

// The walk of MaterializeResources as a flat list of steps in dependency order, built once per
// plan. A run gives the walker's values and makes the walker's reads, without recursion or memos.
// A plan has no tape when its walk depends on runtime values or needs the strict reader:
// branches, captured reads, specialization memory, indirect descriptors, a uniform fill, or a
// select, short-circuit operation, phi or read-first-lane.
class SrtTape {
public:
	static std::shared_ptr<const SrtTape> Build(const ResourcePlan& program);

	// Runs every step. A failed step fails the steps and roots that use it, not the run. Unlike
	// the walker, the run also reads for roots after a failed one; a failed call stops the stage.
	void Run(const ResourcePlan& program, const SrtRuntime& runtime) const;
	// The roots of the last run, as RefreshFlatBuffer and EvaluateDescriptor give them.
	bool FlatBuffer(const ResourcePlan& program, std::vector<uint32_t>& flat) const;
	bool Descriptor(const ResourcePlan& program, uint32_t source, DescriptorValue& result) const;

private:
	friend class SrtTapeBuilder;

	enum class StepKind : uint8_t { UserData, ShaderBase, Pure, ExtractHalf, AddCarryHalf, RawRead };

	struct Step {
		const Inst*             inst      = nullptr;
		StepKind                kind      = StepKind::Pure;
		uint8_t                 arg_count = 0;
		// The user data index, or the half of an extract.
		uint32_t                operand   = 0;
		uint32_t                out       = 0;
		std::array<uint32_t, 5> args {};
	};

	static constexpr uint32_t NoSlot = UINT32_MAX;

	bool Root(uint32_t slot, uint32_t& value) const;

	std::vector<Step>                    m_steps;
	// Per srt_reads entry, and per descriptor source dword (NoSlot: not a root).
	std::vector<uint32_t>                m_srt_slots;
	std::vector<std::array<uint32_t, 8>> m_descriptor_slots;
	// Constant slots are written once by Build; step slots by every run.
	mutable std::vector<uint64_t>        m_values;
	mutable std::vector<uint8_t>         m_ok;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
