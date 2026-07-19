#include "graphics/host_gpu/renderer/shaderResourceBarrier.h"

#include "common/assert.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderBindings.h"

#include <cstring>

namespace Libs::Graphics {

vk::PipelineStageFlags ShaderPipelineStages(vk::ShaderStageFlags stages) {
	vk::PipelineStageFlags result = {};
	if (stages & vk::ShaderStageFlagBits::eVertex) {
		result |= vk::PipelineStageFlagBits::eVertexShader;
	}
	if (stages & vk::ShaderStageFlagBits::eFragment) {
		result |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (stages & vk::ShaderStageFlagBits::eCompute) {
		result |= vk::PipelineStageFlagBits::eComputeShader;
	}
	EXIT_IF(!result);
	return result;
}

VulkanMemoryBarrier MakeShaderWriteDependency() {
	VulkanMemoryBarrier barrier {};
	barrier.sType         = vk::StructureType::eMemoryBarrier;
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite |
	                        vk::AccessFlagBits::eVertexAttributeRead |
	                        vk::AccessFlagBits::eIndexRead | vk::AccessFlagBits::eUniformRead |
	                        vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eColorAttachmentRead |
	                        vk::AccessFlagBits::eColorAttachmentWrite;
	return barrier;
}

vk::ImageMemoryBarrier MakeStorageImageDependency(const VulkanImage& image, bool read,
                                                  bool written) {
	EXIT_IF(image.image == nullptr || image.type == VulkanImageType::DepthStencil);

	vk::ImageMemoryBarrier barrier {};
	barrier.sType         = vk::StructureType::eImageMemoryBarrier;
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
	barrier.dstAccessMask = {};
	if (read) {
		barrier.dstAccessMask |= vk::AccessFlagBits::eShaderRead;
	}
	if (written) {
		barrier.dstAccessMask |= vk::AccessFlagBits::eShaderWrite;
	}
	barrier.oldLayout                       = image.layout;
	barrier.newLayout                       = vk::ImageLayout::eGeneral;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = image.layers;
	EXIT_IF(!barrier.dstAccessMask || barrier.subresourceRange.layerCount == 0);
	return barrier;
}

vk::BufferMemoryBarrier MakeGdsDependency(const VulkanBuffer& buffer) {
	EXIT_IF(buffer.buffer == nullptr);

	vk::BufferMemoryBarrier barrier {};
	barrier.sType         = vk::StructureType::eBufferMemoryBarrier;
	barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
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

bool HasShaderBufferWrites(const ShaderStageRuntime& runtime) {
	EXIT_IF(!runtime);
	return !CollectShaderBufferWrites(*runtime.program, *runtime.resources).empty();
}

void ShaderWriteBarrier(vk::CommandBuffer vk_buffer, vk::PipelineStageFlags source_stages) {
	EXIT_IF(vk_buffer == nullptr || !source_stages);
	const auto barrier = MakeShaderWriteDependency();
	vk_buffer.pipelineBarrier(
	    source_stages,
	    vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eVertexInput |
	        vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader |
	        vk::PipelineStageFlagBits::eTransfer |
	        vk::PipelineStageFlagBits::eColorAttachmentOutput,
	    vk::DependencyFlags {}, 1, &barrier, 0, nullptr, 0, nullptr);
}

} // namespace Libs::Graphics
