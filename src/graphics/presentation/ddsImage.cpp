#include "graphics/presentation/ddsImage.h"

#include "common/assert.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "imgui_impl_vulkan.h"

#include <array>

namespace Libs::Graphics {

DdsImage::~DdsImage() {
	if (m_descriptor != VK_NULL_HANDLE) {
		ImGui_ImplVulkan_RemoveTexture(m_descriptor);
	}
	m_graphics.device.destroyImageView(m_view, nullptr);
	if (m_image.image != nullptr) {
		m_graphics.DeleteImage(m_image);
	}
	if (m_staging != VK_NULL_HANDLE) {
		vmaDestroyBuffer(m_graphics.allocator, m_staging, m_staging_allocation);
	}
}

bool DdsImage::Load(const std::filesystem::path& path) {
	EXIT_IF(m_image.image != nullptr || m_staging != VK_NULL_HANDLE);
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return false;
	}

	std::array<uint32_t, 37> header {};
	uint32_t                 read = 0;
	file.Read(header.data(), sizeof(header), &read);
	const auto width  = header[4];
	const auto height = header[3];
	if (read != sizeof(header) || header[0] != 0x20534444 || header[1] != 124 || header[19] != 32 ||
	    header[20] != 4 || header[21] != 0x30315844 || header[32] != 98 || header[33] != 3 ||
	    header[34] != 0 || header[35] != 1 || header[6] != 0 || header[7] != 1 || header[28] != 0 ||
	    width == 0 || height == 0 || width > 3840 || height > 2160) {
		return false;
	}
	const uint32_t size = ((width + 3) / 4) * ((height + 3) / 4) * 16;
	if (file.Size() != sizeof(header) + size) {
		return false;
	}

	constexpr auto format   = vk::Format::eBc7UnormBlock;
	constexpr auto features = vk::FormatFeatureFlagBits::eSampledImage |
	                          vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	                          vk::FormatFeatureFlagBits::eTransferDst;
	if ((m_graphics.GetFormatProperties(format).optimalTilingFeatures & features) != features) {
		LOGF("[Splash] host does not support BC7 textures\n");
		return false;
	}

	vk::BufferCreateInfo buffer {};
	buffer.size  = size;
	buffer.usage = vk::BufferUsageFlagBits::eTransferSrc;
	VmaAllocationCreateInfo allocation {};
	allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	allocation.flags =
	    VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	VmaAllocationInfo mapped {};
	if (vmaCreateBuffer(m_graphics.allocator, static_cast<const VkBufferCreateInfo*>(buffer),
	                    &allocation, &m_staging, &m_staging_allocation, &mapped) != VK_SUCCESS) {
		LOGF("[Splash] could not allocate DDS upload buffer\n");
		return false;
	}
	file.Read(mapped.pMappedData, size, &read);
	if (read != size) {
		return false;
	}
	RequireVulkanSuccess(static_cast<vk::Result>(vmaFlushAllocation(m_graphics.allocator,
	                                                                m_staging_allocation, 0, size)),
	                     "flush splash image upload");

	vk::ImageCreateInfo image {};
	image.imageType   = vk::ImageType::e2D;
	image.extent      = {width, height, 1};
	image.mipLevels   = 1;
	image.arrayLayers = 1;
	image.format      = format;
	image.tiling      = vk::ImageTiling::eOptimal;
	image.usage       = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
	image.samples     = vk::SampleCountFlagBits::e1;
	if (!m_graphics.CreateImage(image, m_image)) {
		LOGF("[Splash] could not allocate DDS image\n");
		return false;
	}
	vk::ImageViewCreateInfo view {};
	view.image            = m_image.image;
	view.viewType         = vk::ImageViewType::e2D;
	view.format           = format;
	view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	RequireVulkanSuccess(m_graphics.device.createImageView(&view, nullptr, &m_view),
	                     "create splash image view");
	m_descriptor = ImGui_ImplVulkan_AddTexture(static_cast<VkImageView>(m_view),
	                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return true;
}

void DdsImage::Upload(vk::CommandBuffer command) {
	if (m_image.state.layout == vk::ImageLayout::eShaderReadOnlyOptimal) {
		return;
	}
	vk::ImageMemoryBarrier barrier {};
	barrier.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
	barrier.oldLayout           = vk::ImageLayout::eUndefined;
	barrier.newLayout           = vk::ImageLayout::eTransferDstOptimal;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = m_image.image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr, 1,
	                        &barrier);
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent      = m_image.extent;
	command.copyBufferToImage(m_staging, m_image.image, vk::ImageLayout::eTransferDstOptimal, 1,
	                          &copy);
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barrier.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
	barrier.newLayout     = vk::ImageLayout::eShaderReadOnlyOptimal;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eFragmentShader, {}, 0, nullptr, 0, nullptr,
	                        1, &barrier);
	m_image.state.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

ImTextureRef DdsImage::Texture() const {
	return ImTextureRef {static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(m_descriptor))};
}

} // namespace Libs::Graphics
