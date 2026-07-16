#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"

#include <vector>

namespace Libs::Graphics {

struct ShaderBufferWriteRange {
	uint64_t address = 0;
	uint64_t size    = 0;

	bool operator==(const ShaderBufferWriteRange& other) const = default;
};

VkPipelineStageFlags  ShaderPipelineStages(VkShaderStageFlags stages);
VkMemoryBarrier       MakeShaderWriteDependency();
VkImageMemoryBarrier  MakeStorageImageDependency(const VulkanImage& image, bool read, bool written);
VkBufferMemoryBarrier MakeGdsDependency(const VulkanBuffer& buffer);
std::vector<ShaderBufferWriteRange>
CollectShaderBufferWrites(const ShaderRecompiler::IR::Program&          program,
                          const ShaderRecompiler::IR::ResourceSnapshot& resources);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
