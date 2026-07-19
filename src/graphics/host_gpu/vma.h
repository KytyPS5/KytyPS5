#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_VMA_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_VMA_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

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
void     VulkanCreateBuffer(GraphicContext* ctx, uint64_t size, VulkanBuffer* buffer);
void     VulkanDeleteBuffer(GraphicContext* ctx, VulkanBuffer* buffer);
bool     VulkanCreateImage(GraphicContext* ctx, const vk::ImageCreateInfo& image_info,
                           VulkanImage* image);
void     VulkanDeleteImage(GraphicContext* ctx, VulkanImage* image);
void     VulkanMapMemory(GraphicContext* ctx, VulkanMemory* memory, void** data);
void     VulkanUnmapMemory(GraphicContext* ctx, VulkanMemory* memory);

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_VMA_H_ */
