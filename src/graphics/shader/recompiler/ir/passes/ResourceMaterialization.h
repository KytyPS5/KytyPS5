#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ResourceSnapshot {
	struct IndirectImage {
		uint32_t                     resource = 0;
		uint32_t                     capacity = 0;
		std::vector<uint32_t>        keys;
		std::vector<uint32_t>        candidates;
		std::vector<DescriptorValue> descriptors;
	};

	std::vector<DescriptorValue> buffers;
	std::vector<DescriptorValue> images;
	std::vector<DescriptorValue> samplers;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint32_t>        user_data;
	std::vector<IndirectImage>   indirect_images;
};

bool ValidateResourceSnapshot(const Program& program, const ResourceSnapshot& snapshot);
bool ValidateResourceSpecialization(const Program& program, const ResourceSnapshot& snapshot);

// Resolves the immutable dense resource topology against one runtime user-data/SRT snapshot.
// On failure the destination is unchanged.
bool MaterializeResources(const Program& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot);

// Applies runtime descriptor shape/format facts to a copied dense topology before layout and
// emission. Invalid specializations are fatal.
void SpecializeResources(Program& program, ResourceSnapshot& snapshot);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
