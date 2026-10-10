#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>

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
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);

// One memoized evaluation session shared by the entire shader resource refresh. It evaluates the
// plan's walker nodes (ResourcePlan::WalkerNode), compiled on first use.
class SrtWalker {
public:
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          SrtWalker* clean_evaluator = nullptr, Value active_mask = {});
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);
	// Refreshes reachable scalar reads and active descriptor sources in one walk.
	bool RefreshFlatBuffer(std::vector<uint32_t>& flat);

private:
	using Ref  = uint32_t;
	using Node = ResourcePlan::WalkerNode;

	// Node references are below ImmediateRef. Immediates index walker_immediates, except U32 values
	// below 2^30 - 1, held in the reference itself above InlineRef. NoRef is never an operand.
	static constexpr Ref ImmediateRef = 0x80000000u;
	static constexpr Ref InlineRef    = 0xc0000000u;
	static constexpr Ref NoRef        = UINT32_MAX;

	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime, SrtWalker* clean_evaluator,
	          Ref active_mask);
	static ResourcePlan::EvaluationContext& AcquireContext(const ResourcePlan& program);
	static float Float32(uint64_t bits);
	// Sizes the memos of this walker and its clean evaluator to the compiled nodes.
	void Refresh();
	// Value::operator==, without its type switch for the common instruction roots.
	static bool SameRoot(Value left, Value right) {
		const auto* inst = left.TryInstruction();
		return inst != nullptr ? inst == right.TryInstruction() : left == right;
	}
	// A root is a plain plan field (SRT read, descriptor DWORD): a new value gets a new reference.
	Ref RootRef(std::vector<ResourcePlan::WalkerRoot>& roots, size_t index, Value value) {
		const auto& root = roots[index];
		if (root.ref != NoRef && SameRoot(root.value, value)) {
			return root.ref;
		}
		return CompileRoot(roots[index], value);
	}
	Ref  CompileRoot(ResourcePlan::WalkerRoot& root, Value value);
	bool EvaluateRef(Ref ref, uint64_t& result) {
		if (ref >= ImmediateRef) {
			if (ref >= InlineRef) {
				result = ref & ~InlineRef;
				return true;
			}
			EXIT_IF(m_layout != m_program.walker_layout);
			const auto& immediate = m_immediates[ref & ~ImmediateRef];
			result                = immediate.payload;
			return immediate.valid;
		}
		// Outside an EXEC context, a memo hit is the whole evaluation.
		if (m_active_mask == NoRef && m_values[ref].generation == m_generation) {
			result = m_values[ref].value;
			return true;
		}
		return EvaluateNodeRef(ref, result);
	}
	bool Arg(const Node& node, size_t index, uint64_t& result) {
		return EvaluateRef(node.args[index], result);
	}
	bool EvaluateNodeRef(Ref ref, uint64_t& result);
	bool EvaluateNode(const Node& node, uint64_t& result);
	bool EvaluateExtract(const Node& node, uint64_t& result);
	bool EvaluateRawRead(const Node& node, uint64_t& result);

	const ResourcePlan&                     m_program;
	SrtRuntime                              m_runtime;
	SrtWalker*                              m_clean_evaluator = nullptr;
	Ref                                     m_active_mask;
	ResourcePlan::EvaluationContext&        m_context;
	uint64_t                                m_generation = 0;
	const Node*                             m_nodes      = nullptr;
	const ResourcePlan::WalkerImmediate*    m_immediates = nullptr;
	ResourcePlan::EvaluationContext::Entry* m_values     = nullptr;
	// The plan's walker_layout when the pointers above were taken. Every public entry
	// refreshes them, as a nested walker may compile nodes in between.
	uint32_t m_layout = 0;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
