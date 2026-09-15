#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>

#ifdef KYTY_SRT_TEST_HOOKS
#include <memory_resource>
#endif

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

// The functions below resolve SRT slots, clean-slot flags and descriptor sources against the
// ResourcePlan they are given, so a Value that plan does not own only evaluates meaningfully when
// it needs none of those - but it is always memoized correctly: an instruction that holds none of
// this plan's dense memo slots, including one another plan cloned, is memoized by pointer.

// Collects reachable ReadConst values. Immediate offsets receive compact flat-buffer slots;
// dynamic offsets remain explicit and are never assigned a fake slot.
void BuildSrtPlan(Program& program);
bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any);
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

#ifdef KYTY_SRT_TEST_HOOKS
namespace SrtTestHooks {
// Test builds only: evaluators created on the calling thread take their memo storage from
// `resource` instead of the thread's pool; nullptr restores the pool.
void SetMemoResource(std::pmr::memory_resource* resource);
// Test builds only: when disabled, evaluators on the calling thread memoize every instruction
// through the pointer-keyed map instead of the plan's dense slots. The two must agree.
void SetDenseMemo(bool enabled);
// Test builds only: returns the calling thread's memo arenas to a pristine state (every slot
// invalid, counter at zero), which is the state a fresh thread starts in.
void ResetMemoArenas();
// Test builds only: moves the calling thread's memo counter WITHOUT invalidating any slot, so the
// wrap is reachable. Only a value the counter could legitimately reach next is meaningful;
// production never reuses a generation, which is precisely what the wrap must preserve.
void SetMemoGeneration(uint32_t generation);
} // namespace SrtTestHooks
#endif

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
