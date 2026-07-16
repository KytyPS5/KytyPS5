#include "graphics/host_gpu/renderer/shaderResourceBarrier.h"

#include "common/assert.h"
#include "graphics/shader/shaderBindings.h"

#include <cstring>

namespace Libs::Graphics {

VkPipelineStageFlags ShaderPipelineStages(VkShaderStageFlags stages) {
	VkPipelineStageFlags result = 0;
	if ((stages & VK_SHADER_STAGE_VERTEX_BIT) != 0) {
		result |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
	}
	if ((stages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0) {
		result |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	if ((stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0) {
		result |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	}
	EXIT_IF(result == 0);
	return result;
}

VkMemoryBarrier MakeShaderWriteDependency() {
	VkMemoryBarrier barrier {};
	barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
	                        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
	                        VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
	                        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
	                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	return barrier;
}

VkImageMemoryBarrier MakeStorageImageDependency(const VulkanImage& image, bool read, bool written) {
	EXIT_IF(image.image == nullptr || image.type == VulkanImageType::DepthStencil);

	VkImageMemoryBarrier barrier {};
	barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask =
	    (read ? VK_ACCESS_SHADER_READ_BIT : 0) | (written ? VK_ACCESS_SHADER_WRITE_BIT : 0);
	barrier.oldLayout                       = image.layout;
	barrier.newLayout                       = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = image.layers;
	EXIT_IF(barrier.dstAccessMask == 0 || barrier.subresourceRange.layerCount == 0);
	return barrier;
}

VkBufferMemoryBarrier MakeGdsDependency(const VulkanBuffer& buffer) {
	EXIT_IF(buffer.buffer == nullptr);

	VkBufferMemoryBarrier barrier {};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask =
	    VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer.buffer;
	barrier.offset              = 0;
	barrier.size                = VK_WHOLE_SIZE;
	return barrier;
}

std::vector<ShaderBufferWriteRange>
CollectShaderBufferWrites(const ShaderRecompiler::IR::Program&          program,
                          const ShaderRecompiler::IR::ResourceSnapshot& resources) {
	EXIT_IF(resources.buffers.size() != program.info.buffers.size());
	std::vector<ShaderBufferWriteRange> writes;
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		if (!program.info.buffers[i].written) {
			continue;
		}
		const auto& value = resources.buffers[i];
		EXIT_IF(value.dword_count < 4);
		ShaderBufferResource descriptor;
		std::memcpy(descriptor.fields, value.dwords.data(), sizeof(descriptor.fields));
		const auto address = descriptor.Base48();
		const auto records = static_cast<uint64_t>(descriptor.NumRecords());
		const auto stride  = static_cast<uint64_t>(descriptor.Stride());
		if (stride != 0 && records > UINT64_MAX / stride) {
			EXIT("shader resource barrier buffer footprint overflow\n");
		}
		const auto size = stride == 0 ? records : stride * records;
		if (address != 0 && size != 0) {
			writes.push_back({address, size});
		}
	}
	return writes;
}

} // namespace Libs::Graphics
