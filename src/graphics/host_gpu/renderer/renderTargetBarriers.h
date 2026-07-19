#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGETBARRIERS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGETBARRIERS_H_

#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

struct VulkanImage;

void GraphicsRenderTextureBarrier(vk::CommandBuffer vk_buffer, VulkanImage* image);
void GraphicsRenderColorImageBarrier(vk::CommandBuffer vk_buffer, VulkanImage* image,
                                     vk::ImageLayout new_layout);
void GraphicsRenderDepthStencilImageBarrier(vk::CommandBuffer vk_buffer, VulkanImage* image,
                                            vk::ImageLayout new_layout);
void GraphicsRenderDepthStencilBarrier(vk::CommandBuffer vk_buffer, VulkanImage* image);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGETBARRIERS_H_
