#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_DISPLAYBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_DISPLAYBUFFER_H_

#include "common/common.h"

namespace Libs::Graphics {
struct VideoOutVulkanImage;
} // namespace Libs::Graphics

namespace Libs::Presentation {

struct DisplayBufferImage {
	Graphics::VideoOutVulkanImage* image = nullptr;
	uint32_t                       index = static_cast<uint32_t>(-1);
	uint64_t                       size  = 0;
	uint64_t                       pitch = 0;
};

DisplayBufferImage DisplayBufferFind(uint64_t addr, bool render_target = false);
int  DisplayBufferSubmitFlipFromGpu(int handle, int index, int flip_mode, int64_t flip_arg);
void DisplayBufferCompleteFlipFromGpu(int handle, int index);

} // namespace Libs::Presentation

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_DISPLAYBUFFER_H_
