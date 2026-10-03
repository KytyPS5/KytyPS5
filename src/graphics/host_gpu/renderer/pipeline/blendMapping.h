#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BLENDMAPPING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BLENDMAPPING_H_

#include <cstdint>

namespace Libs::Graphics {

namespace HW {
struct BlendControl;
}
namespace Prospero {
struct ColorComponentMapping;
}

enum class BlendMappingSupport {
	Direct,
	SourceAlpha, // Requires logical alpha in the second blend source.
	// The mapping moves alpha out of the alpha slot, so Vulkan's SrcAlpha would read the wrong
	// component. Legal only when the mapping is a plain permutation of all four components, which
	// leaves the shader's own alpha holding a known component, so the caller can put it back.
	PermutedSourceAlpha,
	Unsupported,
};

bool                BlendFactorIsDualSource(uint8_t factor);
BlendMappingSupport ClassifyBlendMapping(const HW::BlendControl&                blend,
                                         const Prospero::ColorComponentMapping& mapping);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BLENDMAPPING_H_
