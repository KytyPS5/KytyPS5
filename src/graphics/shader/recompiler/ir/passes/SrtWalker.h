#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <optional>
#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, std::span<uint32_t> values);
using SrtMemoryRangeClamper = uint64_t (*)(void* userdata, uint64_t address, uint64_t size);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	// Actual guest dispatch counts before host wave partitioning. Absent for graphics
	// and offline callers that cannot prove a dispatch-dependent snapshot's bound.
	std::optional<std::array<uint32_t, 3>> compute_workgroups;
	// Optional renderer address-space query. A zero result means the requested
	// base cannot be bound; a nonzero result is the contiguous mapped prefix.
	SrtMemoryRangeClamper clamp_memory_range = nullptr;
};

// A raw scalar read bounded by a loop guard or one actual dispatch axis.
// offset_scale/index + offset_bias uses U32 arithmetic before separate signed
// memory_offset addition/alignment, matching raw SMEM address evaluation.
struct BoundedSrtReadProof {
	Value index;
	Value count;
	Value address_low;
	Value address_high;
	Value descriptor_word2;
	Value descriptor_word3;
	uint32_t source_dwords = 2;
	uint32_t offset_scale = 0;
	uint32_t offset_bias = 0;
	uint32_t memory_offset = 0;
	uint32_t workgroup_axis = UINT32_MAX;
	bool count_signed = false;
};

std::optional<BoundedSrtReadProof> ProveBoundedSrtRead(const Program& program,
                                                       const Inst& read);

enum class RuntimeValueType { Any, Integer };

// Collects reachable ReadConst values. Immediate offsets receive compact flat-buffer slots;
// dynamic offsets remain explicit and are never assigned a fake slot.
void BuildSrtPlan(Program& program);
bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
// Uses the strict reader for values that affect shader specialization.
SrtRuntime CleanRuntime(SrtRuntime runtime);
bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result);
bool EvaluateBoundedDescriptorSource(const ResourcePlan& program, uint32_t source,
                                     const SrtRuntime& runtime,
                                     std::span<const BoundedSrtLayout> layouts,
                                     std::span<const uint32_t> flattened_srt,
                                     uint32_t candidate, DescriptorValue& result);
bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat,
                            std::span<const uint8_t> clean_flat_slots);

// One memoized evaluation session shared by the entire shader resource refresh.
class SrtWalker {
public:
	SrtWalker(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::span<const uint8_t> clean_flat_slots = {}, SrtWalker* clean_evaluator = nullptr,
	          Value active_mask = {}, std::span<const BoundedSrtLayout> bounded_layouts = {},
	          std::span<const uint32_t> bounded_flat = {},
	          std::optional<uint32_t> bounded_candidate = {});
	~SrtWalker();
	SrtWalker(const SrtWalker&)            = delete;
	SrtWalker& operator=(const SrtWalker&) = delete;

	bool Evaluate(Value value, uint32_t& result);
	bool EvaluateDescriptor(uint32_t source, DescriptorValue& result);
	// An empty span means that all sources are active.
	std::span<const uint8_t> FindActiveSources();
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
	std::span<const uint8_t>         m_clean_flat_slots;
	SrtWalker*                      m_clean_evaluator = nullptr;
	Value                           m_active_mask;
	std::span<const BoundedSrtLayout> m_bounded_layouts;
	std::span<const uint32_t>       m_bounded_flat;
	std::optional<uint32_t>         m_bounded_candidate;
	ResourcePlan::EvaluationContext& m_context;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
