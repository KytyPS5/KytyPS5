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
	Unsupported,
};

bool                BlendFactorIsDualSource(uint8_t factor);
BlendMappingSupport ClassifyBlendMapping(const HW::BlendControl&                blend,
                                         const Prospero::ColorComponentMapping& mapping);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BLENDMAPPING_H_
