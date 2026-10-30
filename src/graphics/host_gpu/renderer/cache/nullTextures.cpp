#include "graphics/host_gpu/renderer/cache/nullTextures.h"

#include "common/assert.h"
#include "common/threads.h"

#include <vector>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::DescriptorBindingKind;

struct PlaceholderShape {
	vk::ImageType          image_type;
	vk::ImageViewType      view_type;
	vk::SampleCountFlagBits samples;
	vk::ImageUsageFlags    usage;
	vk::ImageAspectFlags   aspect;
	// The first format the device has for the image wins; eUndefined ends the list.
	std::array<vk::Format, 2> formats;
};

// The kinds follow ShaderRecompiler::IR::DescriptorBindingForImage: sampled float, uint, sint
// and depth-compare, seven dimensions each; then storage float, uint, atomic uint and 64-bit
// atomic uint, five dimensions each.
PlaceholderShape ShapeOf(uint32_t index) {
	using Format = vk::Format;
	constexpr uint32_t SampledKinds = 4u * 7u;
	constexpr vk::ImageType     image_types[] = {vk::ImageType::e1D, vk::ImageType::e1D,
	                                             vk::ImageType::e2D, vk::ImageType::e2D,
	                                             vk::ImageType::e2D, vk::ImageType::e2D,
	                                             vk::ImageType::e3D};
	constexpr vk::ImageViewType view_types[]  = {
        vk::ImageViewType::e1D, vk::ImageViewType::e1DArray, vk::ImageViewType::e2D,
        vk::ImageViewType::e2DArray, vk::ImageViewType::e2D, vk::ImageViewType::e2DArray,
        vk::ImageViewType::e3D};
	constexpr vk::ImageUsageFlags transfer = vk::ImageUsageFlagBits::eTransferDst;
	if (index < SampledKinds) {
		const auto group = index / 7u;
		const auto dim   = index % 7u;
		const bool msaa  = dim == 4u || dim == 5u;
		PlaceholderShape shape {image_types[dim],
		                        view_types[dim],
		                        msaa ? vk::SampleCountFlagBits::e4 : vk::SampleCountFlagBits::e1,
		                        vk::ImageUsageFlagBits::eSampled | transfer,
		                        vk::ImageAspectFlagBits::eColor,
		                        {}};
		switch (group) {
			case 0: shape.formats = {Format::eR8G8B8A8Unorm, Format::eUndefined}; break;
			case 1: shape.formats = {Format::eR8G8B8A8Uint, Format::eUndefined}; break;
			case 2: shape.formats = {Format::eR8G8B8A8Sint, Format::eUndefined}; break;
			default:
				shape.aspect  = vk::ImageAspectFlagBits::eDepth;
				shape.formats = {Format::eD32Sfloat, Format::eUndefined};
				break;
		}
		return shape;
	}
	index -= SampledKinds;
	const auto group = index / 5u;
	const auto dim   = index % 5u;
	// Storage has no multisampled dimensions: 1D, 1D array, 2D, 2D array, 3D.
	const auto sampled_dim = dim == 4u ? 6u : dim;
	PlaceholderShape shape {image_types[sampled_dim],
	                        view_types[sampled_dim],
	                        vk::SampleCountFlagBits::e1,
	                        vk::ImageUsageFlagBits::eStorage | transfer,
	                        vk::ImageAspectFlagBits::eColor,
	                        {}};
	switch (group) {
		case 0: shape.formats = {Format::eR8G8B8A8Unorm, Format::eR32Sfloat}; break;
		case 1: shape.formats = {Format::eR8G8B8A8Uint, Format::eR32Uint}; break;
		case 2: shape.formats = {Format::eR32Uint, Format::eUndefined}; break;
		default: shape.formats = {Format::eR64Uint, Format::eUndefined}; break;
	}
	return shape;
}

bool Supported(GraphicContext& graphics, const PlaceholderShape& shape, vk::Format format) {
	vk::ImageFormatProperties properties {};
	if (graphics.physical_device.getImageFormatProperties(format, shape.image_type,
	                                                      vk::ImageTiling::eOptimal, shape.usage,
	                                                      {}, &properties) != vk::Result::eSuccess) {
		return false;
	}
	if (!(properties.sampleCounts & shape.samples)) {
		return false;
	}
	const auto features = graphics.physical_device.getFormatProperties(format).optimalTilingFeatures;
	const bool atomic   = format == vk::Format::eR32Uint || format == vk::Format::eR64Uint;
	if ((shape.usage & vk::ImageUsageFlagBits::eStorage) && atomic &&
	    !(features & vk::FormatFeatureFlagBits::eStorageImageAtomic)) {
		return false;
	}
	return true;
}

} // namespace

NullTextures::NullTextures(GraphicContext& graphics): m_graphics(graphics) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	if (!graphics.null_descriptor_enabled) {
		Create();
	}
}

NullTextures::~NullTextures() {
	for (auto& placeholder: m_placeholders) {
		if (placeholder.view != nullptr) {
			m_graphics.device.destroyImageView(placeholder.view, nullptr);
		}
		if (placeholder.image.image != nullptr) {
			m_graphics.DeleteImage(placeholder.image);
		}
	}
}

void NullTextures::Create() {
	auto& device = m_graphics.device;
	std::vector<vk::ImageMemoryBarrier> to_general;
	std::vector<vk::ImageMemoryBarrier> to_shader;
	for (uint32_t index = 0; index < m_placeholders.size(); index++) {
		const auto shape  = ShapeOf(index);
		auto       format = vk::Format::eUndefined;
		for (const auto candidate: shape.formats) {
			if (candidate != vk::Format::eUndefined && Supported(m_graphics, shape, candidate)) {
				format = candidate;
				break;
			}
		}
		if (format == vk::Format::eUndefined) {
			continue; // A null texture of this kind stops the emulator (see ImageInfo).
		}
		auto&               placeholder = m_placeholders[index];
		vk::ImageCreateInfo image_info {};
		image_info.imageType     = shape.image_type;
		image_info.format        = format;
		image_info.extent        = vk::Extent3D {1, 1, 1};
		image_info.mipLevels     = 1;
		image_info.arrayLayers   = 1;
		image_info.samples       = shape.samples;
		image_info.tiling        = vk::ImageTiling::eOptimal;
		image_info.usage         = shape.usage;
		image_info.sharingMode   = vk::SharingMode::eExclusive;
		image_info.initialLayout = vk::ImageLayout::eUndefined;
		EXIT_IF(!m_graphics.CreateImage(image_info, placeholder.image));

		const vk::ImageSubresourceRange range {shape.aspect, 0, 1, 0, 1};
		vk::ImageViewCreateInfo         view_info {};
		view_info.image            = placeholder.image.image;
		view_info.viewType         = shape.view_type;
		view_info.format           = format;
		view_info.subresourceRange = range;
		EXIT_IF(device.createImageView(&view_info, nullptr, &placeholder.view) !=
		        vk::Result::eSuccess);

		vk::ImageMemoryBarrier barrier {};
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = placeholder.image.image;
		barrier.subresourceRange    = range;
		barrier.oldLayout           = vk::ImageLayout::eUndefined;
		barrier.newLayout           = vk::ImageLayout::eGeneral;
		barrier.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_general.push_back(barrier);
		barrier.oldLayout     = vk::ImageLayout::eGeneral;
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		to_shader.push_back(barrier);
		placeholder.image.state.layout = vk::ImageLayout::eGeneral;
	}

	// Clear every placeholder to zero once, on a command buffer of its own.
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.queueFamilyIndex = m_graphics.queue_family;
	vk::CommandPool pool       = nullptr;
	vk::Fence       fence      = nullptr;
	const vk::FenceCreateInfo fence_info {};
	EXIT_IF(device.createCommandPool(&pool_info, nullptr, &pool) != vk::Result::eSuccess ||
	        device.createFence(&fence_info, nullptr, &fence) != vk::Result::eSuccess);
	vk::CommandBufferAllocateInfo command_info {};
	command_info.commandPool        = pool;
	command_info.level              = vk::CommandBufferLevel::ePrimary;
	command_info.commandBufferCount = 1;
	vk::CommandBuffer          command = nullptr;
	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	EXIT_IF(device.allocateCommandBuffers(&command_info, &command) != vk::Result::eSuccess ||
	        command.begin(&begin_info) != vk::Result::eSuccess);
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr,
	                        static_cast<uint32_t>(to_general.size()), to_general.data());
	for (const auto& barrier: to_general) {
		const auto& range = barrier.subresourceRange;
		if (range.aspectMask & vk::ImageAspectFlagBits::eDepth) {
			const vk::ClearDepthStencilValue zero {0.0f, 0};
			command.clearDepthStencilImage(barrier.image, vk::ImageLayout::eGeneral, &zero, 1, &range);
		} else {
			vk::ClearColorValue zero {};
			zero.uint32 = std::array<uint32_t, 4> {0, 0, 0, 0};
			command.clearColorImage(barrier.image, vk::ImageLayout::eGeneral, &zero, 1, &range);
		}
	}
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 0, nullptr,
	                        static_cast<uint32_t>(to_shader.size()), to_shader.data());
	EXIT_IF(command.end() != vk::Result::eSuccess);
	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		EXIT_IF(m_graphics.queue.submit(1, &submit, fence) != vk::Result::eSuccess);
	}
	EXIT_IF(device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX) != vk::Result::eSuccess);
	device.destroyFence(fence, nullptr);
	device.destroyCommandPool(pool, nullptr);
}

vk::DescriptorImageInfo NullTextures::ImageInfo(DescriptorBindingKind kind) const {
	const auto index = ShaderRecompiler::IR::ImageBindingIndex(kind);
	EXIT_IF(index >= m_placeholders.size());
	const auto view = m_placeholders[index].view;
	if (view == nullptr) {
		EXIT("a null texture of image binding %u has no placeholder: the device has no null "
		     "descriptor (VK_EXT_robustness2's nullDescriptor) and no image of that kind\n",
		     static_cast<uint32_t>(kind));
	}
	return {nullptr, view, vk::ImageLayout::eGeneral};
}

void NullTextures::Reset(DescriptorBindingKind kind, vk::CommandBuffer command) const {
	const auto index = ShaderRecompiler::IR::ImageBindingIndex(kind);
	EXIT_IF(index >= m_placeholders.size());
	const auto& placeholder = m_placeholders[index];
	if (placeholder.view == nullptr) {
		return; // ImageInfo stops the emulator.
	}
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::ImageMemoryBarrier          barrier {};
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = placeholder.image.image;
	barrier.subresourceRange    = range;
	barrier.oldLayout           = vk::ImageLayout::eGeneral;
	barrier.newLayout           = vk::ImageLayout::eGeneral;
	barrier.srcAccessMask       = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr, 1,
	                        &barrier);
	vk::ClearColorValue zero {};
	zero.uint32 = std::array<uint32_t, 4> {0, 0, 0, 0};
	command.clearColorImage(placeholder.image.image, vk::ImageLayout::eGeneral, &zero, 1, &range);
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 0, nullptr, 1,
	                        &barrier);
}

} // namespace Libs::Graphics
