#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>

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
void CompileSrtWalker(ResourcePlan& program);
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);

// One memoized evaluation session shared by the entire shader resource refresh.
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
	static ResourcePlan::EvaluationContext& AcquireContext(const ResourcePlan& program);
	static float Float32(uint64_t bits);
	bool EvaluateWide(Value value, uint64_t& result);
	bool Arg(const Inst& inst, size_t index, uint64_t& result);
	bool EvaluatePhi(const Inst& inst, uint64_t& result);
	bool EvaluateExtract(const Inst& inst, uint64_t& result);
	bool EvaluateRawRead(const Inst& inst, uint64_t& result);
	bool EvaluateInst(const Inst& inst, uint64_t& result);

	const ResourcePlan&              m_program;
	SrtRuntime                      m_runtime;
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
