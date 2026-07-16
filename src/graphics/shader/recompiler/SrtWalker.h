#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ShaderIR.h"

#include <optional>
#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, uint32_t* value);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base = 0;
	SrtMemoryReader           read_memory = nullptr;
	void*                     userdata    = nullptr;
	std::optional<uint64_t>   flat_memory_base;
};

struct DescriptorSourceRequest {
	uint32_t source = 0;
	uint32_t use_pc = 0;
};

bool FoldScalarConstant(const ScalarProvenance& provenance, uint32_t value, uint32_t* result);

// Collects reachable ReadConst values. Immediate offsets receive compact flat-buffer slots;
// dynamic offsets remain explicit and are never assigned a fake slot.
bool BuildSrtPlan(Program* program, std::string* error);

bool EvaluateDescriptorSource(const Program& program, uint32_t source, uint32_t use_pc,
                              const SrtRuntime& runtime, DescriptorValue* result,
                              std::string* error);

// Evaluates one runtime snapshot transactionally. Scalar values and ReadConst results shared by
// several descriptors are memoized once across the batch.
bool EvaluateDescriptorSources(const Program&                           program,
                               std::span<const DescriptorSourceRequest> requests,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>* results,
                               std::string* error);

// Evaluates descriptor sources and the flattened immediate SRT with one memoized scalar walk.
// On failure neither destination is changed.
bool EvaluateRuntimeSources(const Program&                           program,
                            std::span<const DescriptorSourceRequest> requests,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>* results,
                            std::vector<uint32_t>* flat, std::string* error);

bool WalkSrt(const Program& program, const SrtRuntime& runtime, std::vector<uint32_t>* flat,
             std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
