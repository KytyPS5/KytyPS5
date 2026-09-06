#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

namespace Libs::Graphics {

struct ShaderStageRuntime;

vk::PipelineStageFlags  ShaderPipelineStages(vk::ShaderStageFlags stages);
VulkanMemoryBarrier     MakeShaderAccessDependency();
VulkanMemoryBarrier     MakeShaderWriteHazardDependency();
VulkanMemoryBarrier     MakeShaderWriteDependency();
vk::BufferMemoryBarrier MakeGdsDependency(vk::Buffer buffer);
bool HasShaderBufferWrites(const ShaderStageRuntime& runtime);
void ShaderAccessBarrier(vk::CommandBuffer vk_buffer, vk::PipelineStageFlags source_stages);
void ShaderWriteHazardBarrier(vk::CommandBuffer      vk_buffer,
                              vk::PipelineStageFlags destination_stages);
void ShaderWriteBarrier(vk::CommandBuffer vk_buffer, vk::PipelineStageFlags source_stages);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
