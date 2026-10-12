#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

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
