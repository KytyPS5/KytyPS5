#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VMA_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VMA_H_

#include "common/common.h"

namespace Libs::Graphics {

struct GraphicContext;
struct VulkanBuffer;
struct VulkanImage;
struct VulkanMemory;

bool     VulkanCreateAllocator(GraphicContext* ctx);
void     VulkanDestroyAllocator(GraphicContext* ctx);
uint64_t VulkanNextMemoryUniqueId();
void     VulkanTrackAllocation(const VulkanMemory* memory);
void     VulkanUntrackAllocation(const VulkanMemory* memory);
void     VulkanLogMemoryBudget(GraphicContext* ctx);
bool     VulkanAllocate(GraphicContext* ctx, VulkanMemory* memory);
void     VulkanFree(GraphicContext* ctx, VulkanMemory* memory);
void     VulkanMapMemory(GraphicContext* ctx, VulkanMemory* memory, void** data);
void     VulkanUnmapMemory(GraphicContext* ctx, VulkanMemory* memory);
void     VulkanBindImageMemory(GraphicContext* ctx, VulkanImage* image, VulkanMemory* memory);
void     VulkanBindBufferMemory(GraphicContext* ctx, VulkanBuffer* buffer, VulkanMemory* memory);

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_VMA_H_ */
