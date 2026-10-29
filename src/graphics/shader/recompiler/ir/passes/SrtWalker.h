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
	uint64_t                  indirect_args_address = 0;
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

// A replay that read memory and failed leaves the plain walk after it to decide what the failure
// means; the walk must not try the same address again (one failing read, one attempt). The
// reads that fail while `remember` is true are remembered until SrtForgetFailedRead.
void SrtRememberFailedReads(bool remember);
void SrtForgetFailedRead();

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

	// The constant equal to `value`, or NoRef.
	[[nodiscard]] uint32_t FindConstant(Value value) const;

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
	// The control flow and the reads again, packed for RefreshFlatBuffer: block b's successors,
	// sources and reads are [begin[b], begin[b + 1]) of each list. Empty when a block names a
	// source outside the plan (the walker walks the plan's blocks then).
	bool                                 m_flow = false;
	std::vector<uint32_t>                m_successor_begin;
	std::vector<uint32_t>                m_successors;
	std::vector<uint32_t>                m_source_begin;
	std::vector<uint32_t>                m_sources;
	std::vector<uint32_t>                m_block_read_begin;
	std::vector<uint32_t>                m_block_reads;
	std::vector<uint32_t>                m_read_offsets;
	// Per descriptor source: 1 unless a block decides it.
	std::vector<uint8_t>                 m_active_template;
};

class SrtTraceSession;

class SrtWalker {
public:
	// `trace`: the walk is recorded, or replayed, in it (SrtTrace).
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          SrtWalker* clean_evaluator = nullptr, SrtTraceSession* trace = nullptr);
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);
	// Refreshes reachable scalar reads and active descriptor sources in one walk.
	bool RefreshFlatBuffer(std::vector<uint32_t>& flat);

private:
	// RefreshFlatBuffer over the packed flow of SrtNodes.
	bool RefreshFlatBufferNodes(std::vector<uint32_t>& flat);
	bool RefreshFlatBufferWalk(std::vector<uint32_t>& flat);
	bool EvaluateDescriptorWalk(uint32_t source, DescriptorValue& result);
	// Recording: the guard for the outcome `ok` of the value EvaluateRef returned last.
	bool Taken(bool ok);

public:

private:
	// `active_ref`: the mask's node reference, when the caller has it (SrtNodes::NoRef: found).
	SrtWalker(SrtWalker& parent, Value active_mask, bool clean = false,
	          SrtWalker* clean_evaluator = nullptr, uint32_t active_ref = UINT32_MAX);
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
	// Recording or replaying the walk; the role tells the trace's two walkers apart.
	SrtTraceSession*                m_trace      = nullptr;
	uint8_t                         m_trace_role = 0;
	uint32_t                        m_active_ref = SrtNodes::NoRef;
	SrtRuntime                      m_runtime;
	SrtWalker*                      m_clean_evaluator = nullptr;
	Value                           m_active_mask;
	const std::vector<uint8_t>*      m_reachable_blocks = nullptr;
	// The session's contexts, linked by next_of_session (the session's own walker keeps them).
	mutable uint32_t                m_session_contexts = UINT32_MAX;
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

	// What a run needs of the value, copied: the plan's Inst objects are cold during a draw.
	struct Step {
		uint64_t                flags     = 0;
		ValueOpcode             op        = ValueOpcode::Identity;
		StepKind                kind      = StepKind::Pure;
		uint8_t                 arg_count = 0;
		// The user data index, or the half of an extract.
		uint32_t                operand   = 0;
		uint32_t                out       = 0;
		std::array<uint32_t, 5> args {};
	};

	static constexpr uint32_t NoSlot = UINT32_MAX;

	bool Root(uint32_t slot, uint32_t& value) const;

	// In order of read depth (the raw reads on a step's longest chain of arguments): at each
	// depth the reads come first, as m_read_groups, then the steps that use them.
	std::vector<Step>                    m_steps;
	// [begin, end) of the reads of each depth: a run asks for their memory before it reads any
	// of it, so that the reads wait together and not one after another.
	std::vector<std::pair<uint32_t, uint32_t>> m_read_groups;
	// Per srt_reads entry, and per descriptor source dword (NoSlot: not a root).
	std::vector<uint32_t>                m_srt_slots;
	std::vector<std::array<uint32_t, 8>> m_descriptor_slots;
	// Constant slots are written once by Build; step slots by every run.
	mutable std::vector<uint64_t>        m_values;
	mutable std::vector<uint8_t>         m_ok;
};

// A walk over a plan's nodes as it went: the steps it took in order, with a guard at every
// decision a value made (a branch, a short circuit, a value that failed). The next walk of the
// plan replays the steps while every guard holds: the same path, with the same reads in the same
// order, without recursion or memos. A guard that fails ends the replay, and the walker walks.
// Values are kept by slot: constants are written once, when recorded, and step outputs by each
// replay. The walk answers queries (RefreshFlatBuffer, EvaluateDescriptor, Evaluate) one after
// another; each query runs its own steps, so a replay reads nothing for a query never asked.
class SrtTrace {
private:
	friend class SrtTraceSession;
	friend class SrtWalker;

	enum class Op : uint8_t {
		UserData,     // aux: user data index
		ShaderBase,
		Pure,         // args[0, aux): ApplyPure(op, flags)
		ExtractHalf,  // args[0], aux: component
		AddCarryHalf, // args[0], args[1], aux: component
		RawRead,      // args: low, high, offset, records, word3; flags: memory index;
		              // aux: ReadVector | ReadClean (the reader); expect: the address outcome
		GuardOk,      // args[0] evaluated (expect 1) or failed (0)
		GuardTruth,   // args[0] is nonzero (expect 1) or zero (0)
		GuardDecided, // DecidedByArgument(op, aux, args[0]) == expect
		GuardEqual,   // args[0] == args[1] is expect
	};
	// RawRead address outcomes.
	static constexpr uint8_t ReadFails = 0;
	static constexpr uint8_t ReadZero  = 1;
	static constexpr uint8_t ReadWord  = 2;
	// RawRead reader bits in aux.
	static constexpr uint32_t ReaderVector = 1u;
	static constexpr uint32_t ReaderClean  = 2u;

	struct Step {
		uint64_t                flags  = 0;
		ValueOpcode             op     = ValueOpcode::Identity;
		Op                      kind   = Op::GuardOk;
		uint8_t                 expect = 0;
		uint32_t                aux    = 0;
		uint32_t                out    = 0;
		std::array<uint32_t, 5> args {};
	};

	enum class QueryKind : uint8_t { FlatBuffer, Descriptor, Value };
	struct Query {
		QueryKind kind   = QueryKind::Value;
		uint8_t   role   = 0;
		bool      result = false;
		// The descriptor source, or the evaluated value's node reference.
		uint64_t key           = 0;
		uint32_t steps_end     = 0;
		uint32_t outputs_begin = 0;
		uint32_t outputs_end   = 0;
		// FlatBuffer: the buffer's size and the active sources (m_actives) after the walk.
		uint32_t flat_size = 0;
		uint32_t active    = 0;
	};

	std::vector<Step>                          m_steps;
	std::vector<Query>                         m_queries;
	// (flat buffer offset, descriptor dword, or 0 for a value; slot) per query output.
	std::vector<std::pair<uint32_t, uint32_t>> m_outputs;
	std::vector<std::vector<uint8_t>>          m_actives;
	std::vector<uint64_t>                      m_values;
	std::vector<uint8_t>                       m_ok;
	std::vector<uint8_t>                       m_constant;
	uint32_t                                   m_fail_slot = 0;
};

// One MaterializeResources walk with a trace: it replays the plan's trace, or records one.
class SrtTraceSession {
public:
	// SetEnabled(false): every walk walks (tests of the walker's own reads). KYTY_SRT_TRACE=0
	// turns traces off from the start.
	static void SetEnabled(bool enabled);
	[[nodiscard]] static bool Enabled();

	// Replays `trace`, or records when it is null.
	SrtTraceSession(const ResourcePlan& program, std::shared_ptr<SrtTrace> trace);
	~SrtTraceSession();
	SrtTraceSession(const SrtTraceSession&)            = delete;
	SrtTraceSession& operator=(const SrtTraceSession&) = delete;

	// The runtime the walkers read through, once the walk has made it.
	void Attach(const SrtRuntime& runtime);
	[[nodiscard]] bool Replaying() const noexcept { return m_replay != nullptr; }
	// A guard failed or a query differed: the walk's answers are void, and the walker walks.
	[[nodiscard]] bool Aborted() const noexcept { return m_aborted; }
	// The recorded trace; null when the walk took a path the trace cannot hold.
	[[nodiscard]] std::shared_ptr<SrtTrace> Finish();

private:
	friend class SrtWalker;
	struct Recording;

	// Recording.
	[[nodiscard]] uint32_t Constant(uint64_t bits);
	[[nodiscard]] uint32_t Fail() const;
	uint32_t               Step(SrtTrace::Step step);
	void                   Guard(SrtTrace::Op kind, uint32_t slot, bool expect, uint32_t aux = 0,
	                             ValueOpcode op = ValueOpcode::Identity, uint32_t other = 0);
	void                   Bind(const void* context, uint32_t ref, uint32_t slot);
	[[nodiscard]] uint32_t Bound(const void* context, uint32_t ref);
	void                   Unbind(const void* context, uint32_t ref);
	void                   Unsupported() noexcept { m_supported = false; }
	void BeginQuery(SrtTrace::QueryKind kind, uint8_t role, uint64_t key);
	void Output(uint32_t index, uint32_t slot);
	void EndQuery(bool result, uint32_t flat_size = 0, const std::vector<uint8_t>* active = nullptr);
	[[nodiscard]] uint32_t Reader(const SrtRuntime& runtime, bool vector);

	// Replay: the next query, if it is this one, with its steps run. Null: aborted.
	const SrtTrace::Query* Replay(SrtTrace::QueryKind kind, uint8_t role, uint64_t key);
	bool                   ReplayFlatBuffer(uint8_t role, std::vector<uint32_t>& flat,
	                                        std::vector<uint8_t>& active);
	bool                   ReplayDescriptor(uint8_t role, uint32_t source, uint32_t dword_count,
	                                        DescriptorValue& result);
	bool                   ReplayValue(uint8_t role, uint64_t key, uint64_t& result);
	[[nodiscard]] uint64_t Value(uint32_t slot) const { return m_replay->m_values[slot]; }

	const ResourcePlan&        m_program;
	SrtRuntime                 m_runtime;
	SrtRuntime                 m_clean_runtime;
	std::shared_ptr<SrtTrace>  m_replay;
	std::unique_ptr<Recording> m_recording;
	// Top-level walkers made so far: a walker's role is its place in that order.
	uint8_t                    m_roles = 0;
	uint32_t                   m_query = 0;
	uint32_t                   m_step  = 0;
	bool                       m_aborted   = false;
	bool                       m_supported = true;
	// Recording: the slot of the value the last EvaluateRef returned.
	uint32_t last = 0;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
