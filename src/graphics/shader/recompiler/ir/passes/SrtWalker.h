#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <optional>
#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, uint32_t* value);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	// Actual guest dispatch counts before host wave partitioning. Absent for graphics
	// and offline callers that cannot prove a dispatch-dependent snapshot's bound.
	std::optional<std::array<uint32_t, 3>> compute_workgroups;
};

// A raw scalar read bounded by a loop guard or one actual dispatch axis.
// offset_scale/index + offset_bias uses U32 arithmetic BEFORE separate signed
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

// Collects reachable ReadConst values. Immediate offsets receive compact flat-buffer slots;
// dynamic offsets remain explicit and are never assigned a fake slot.
void BuildSrtPlan(Program& program);
bool ValidateRuntimeValue(const ResourcePlan& program, Value value);

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result);

// Evaluates a descriptor expression for one candidate of an already captured bounded SRT table.
// ReadBoundedSrtU32 ignores its live GPU key only within this host enumeration transaction.
bool EvaluateBoundedDescriptorSource(const ResourcePlan& program, uint32_t source,
                                     const SrtRuntime& runtime,
                                     std::span<const BoundedSrtLayout> layouts,
                                     std::span<const uint32_t> flattened_srt,
                                     uint32_t candidate, DescriptorValue& result);

// Evaluates one runtime snapshot transactionally. Scalar values and ReadConst results shared by
// several descriptors are memoized once across the batch.
bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results);

// Evaluates descriptor sources and the flattened immediate SRT with one memoized scalar walk.
// On failure neither destination is changed.
bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots);

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime,
             std::vector<uint32_t>& flat);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
