#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"

#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;

struct ShaderBufferWriteRange {
	uint64_t address = 0;
	uint64_t size    = 0;

	bool operator==(const ShaderBufferWriteRange& other) const = default;
};

vk::PipelineStageFlags  ShaderPipelineStages(vk::ShaderStageFlags stages);
VulkanMemoryBarrier     MakeShaderWriteDependency();
vk::ImageMemoryBarrier  MakeStorageImageDependency(const VulkanImage& image, bool read,
                                                   bool written);
vk::BufferMemoryBarrier MakeGdsDependency(const VulkanBuffer& buffer);
std::vector<ShaderBufferWriteRange>
CollectShaderBufferWrites(const ShaderRecompiler::IR::Program&          program,
                          const ShaderRecompiler::IR::ResourceSnapshot& resources);
bool HasShaderBufferWrites(const ShaderStageRuntime& runtime);
void ShaderWriteBarrier(vk::CommandBuffer vk_buffer, vk::PipelineStageFlags source_stages);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
