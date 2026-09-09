#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VMA_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VMA_H_

#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics {

uint64_t VulkanNextMemoryUniqueId();
void     VulkanTrackAllocation(const VulkanMemory& memory);
void     VulkanUntrackAllocation(const VulkanMemory& memory);

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_VMA_H_ */
