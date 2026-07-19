#include "graphics/host_gpu/transfer.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/window.h"

#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace Libs::Graphics::Transfer {

static Common::Mutex              g_scratch_buffer_mutex;
static std::unique_ptr<uint8_t[]> g_scratch_buffer;
static uint64_t                   g_scratch_buffer_capacity = 0;

std::vector<ImageBufferCopy> MakeLayeredImageBufferCopies(uint32_t layers, uint64_t slice_size,
                                                          uint32_t pitch, uint32_t width,
                                                          uint32_t             height,
                                                          vk::ImageAspectFlags aspect) {
	if (layers == 0 || slice_size == 0 || slice_size > UINT32_MAX / layers || pitch < width ||
	    width == 0 || height == 0) {
		EXIT("invalid layered image-buffer copy layout\n");
	}
	std::vector<ImageBufferCopy> regions(layers);
	for (uint32_t layer = 0; layer < layers; layer++) {
		auto& region     = regions[layer];
		region.offset    = static_cast<uint32_t>(slice_size * layer);
		region.pitch     = pitch;
		region.width     = width;
		region.height    = height;
		region.src_layer = layer;
		region.aspect    = aspect;
	}
	return regions;
}

ScratchBuffer::ScratchBuffer(uint64_t size) {
	EXIT_IF(size == 0);

	g_scratch_buffer_mutex.Lock();

	if (g_scratch_buffer_capacity < size) {
		g_scratch_buffer          = std::make_unique<uint8_t[]>(size);
		g_scratch_buffer_capacity = size;
	}

	m_data = g_scratch_buffer.get();
}

ScratchBuffer::~ScratchBuffer() {
	g_scratch_buffer_mutex.Unlock();
}

bool GuestBufferIsTiled(uint64_t vaddr, uint64_t size) {
	if ((size & 0x7u) == 0) {
		if (size == 0) {
			return false;
		}

		const auto* ptr     = reinterpret_cast<const uint64_t*>(vaddr);
		const auto* ptr_end = reinterpret_cast<const uint64_t*>(vaddr + size);
		const auto  element = *ptr;

		for (; ptr < ptr_end; ptr++) {
			if (element != *ptr) {
				return true;
			}
		}
		return false;
	}
	return true;
}

void WaitForGraphicsIdle(GraphicContext* ctx) {
	Common::Mutex* locked[GraphicContext::QUEUES_NUM] {};
	int            locked_num = 0;

	for (int id = 0; id < GraphicContext::QUEUES_NUM; id++) {
		auto* mutex = ctx->queues[id].mutex;
		if (mutex == nullptr || ctx->queues[id].vk_queue == nullptr) {
			continue;
		}

		bool already_locked = false;
		for (int i = 0; i < locked_num; i++) {
			if (locked[i] == mutex) {
				already_locked = true;
				break;
			}
		}

		if (!already_locked) {
			mutex->Lock();
			locked[locked_num++] = mutex;
		}
	}

	auto result = ctx->device.waitIdle();

	for (int i = locked_num - 1; i >= 0; i--) {
		locked[i]->Unlock();
	}

	if (result != vk::Result::eSuccess) {
		LOGF("vkDeviceWaitIdle failed: %s (%d)\n", VulkanToString(result).c_str(),
		     static_cast<int>(result));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

static void SetImageLayout(vk::CommandBuffer buffer, VulkanImage* dst_image, uint32_t base_level,
                           uint32_t levels, vk::ImageAspectFlags aspect_mask,
                           vk::ImageLayout old_image_layout, vk::ImageLayout new_image_layout);

template <typename Recorder>
static void ExecuteImmediateCommands(const Recorder& recorder) {
	// Keep synchronous utility submission behind one boundary so adopting a central scheduler does
	// not touch every caller.
	CommandBuffer command(GraphicContext::QUEUE_UTIL);
	command.Begin();
	recorder(&command, command.Handle());
	command.End();
	command.Execute();
	command.WaitForFence();
}

[[nodiscard]] static bool IsSingleImageAspect(vk::ImageAspectFlags aspect) {
	return aspect == vk::ImageAspectFlagBits::eColor || aspect == vk::ImageAspectFlagBits::eDepth ||
	       aspect == vk::ImageAspectFlagBits::eStencil;
}

[[nodiscard]] static vk::BufferImageCopy
MakeBufferImageCopy(vk::DeviceSize buffer_offset, uint32_t buffer_pitch,
                    vk::ImageAspectFlags aspect, uint32_t mip_level, uint32_t array_layer,
                    vk::Offset3D image_offset, vk::Extent3D image_extent) {
	vk::BufferImageCopy copy {};
	copy.bufferOffset                    = buffer_offset;
	copy.bufferRowLength                 = buffer_pitch != image_extent.width ? buffer_pitch : 0;
	copy.bufferImageHeight               = 0;
	copy.imageSubresource.aspectMask     = aspect;
	copy.imageSubresource.mipLevel       = mip_level;
	copy.imageSubresource.baseArrayLayer = array_layer;
	copy.imageSubresource.layerCount     = 1;
	copy.imageOffset                     = image_offset;
	copy.imageExtent                     = image_extent;
	return copy;
}

static void SetBufferMemoryBarrier(vk::CommandBuffer command, vk::Buffer buffer,
                                   vk::DeviceSize offset, vk::DeviceSize size,
                                   vk::AccessFlags src_access, vk::AccessFlags dst_access,
                                   vk::PipelineStageFlags src_stage,
                                   vk::PipelineStageFlags dst_stage) {
	vk::BufferMemoryBarrier barrier {};
	barrier.sType               = vk::StructureType::eBufferMemoryBarrier;
	barrier.srcAccessMask       = src_access;
	barrier.dstAccessMask       = dst_access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer;
	barrier.offset              = offset;
	barrier.size                = size;
	command.pipelineBarrier(src_stage, dst_stage, vk::DependencyFlags {}, 0, nullptr, 1, &barrier,
	                        0, nullptr);
}

[[nodiscard]] static vk::ImageAspectFlags GetTransferAspects(const VulkanImage&   image,
                                                             vk::ImageAspectFlags copy_aspect) {
	if (copy_aspect == vk::ImageAspectFlagBits::eColor) {
		return vk::ImageAspectFlagBits::eColor;
	}
	if (copy_aspect == vk::ImageAspectFlagBits::eDepth ||
	    copy_aspect == vk::ImageAspectFlagBits::eStencil) {
		const auto aspects = ImageViewOps::DepthAspectMask(image.format);
		if (!(copy_aspect & aspects)) {
			EXIT("image transfer aspect is unavailable: format=%d copy=0x%x available=0x%x\n",
			     static_cast<int>(image.format),
			     static_cast<vk::ImageAspectFlags::MaskType>(copy_aspect),
			     static_cast<vk::ImageAspectFlags::MaskType>(aspects));
		}
		return aspects;
	}
	EXIT("unsupported image transfer aspect: 0x%x\n",
	     static_cast<vk::ImageAspectFlags::MaskType>(copy_aspect));
}

static void RecordBufferToImageCopy(CommandBuffer& command, VulkanBuffer& src_buffer,
                                    VulkanImage&                         dst_image,
                                    std::span<const vk::BufferImageCopy> regions,
                                    vk::ImageAspectFlags                 transition_aspects,
                                    vk::ImageLayout initial_layout, vk::ImageLayout final_layout) {
	auto vk_command = command.Handle();
	SetBufferMemoryBarrier(vk_command, src_buffer.buffer, 0, VK_WHOLE_SIZE,
	                       vk::AccessFlagBits::eMemoryWrite, vk::AccessFlagBits::eTransferRead,
	                       vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer);
	SetImageLayout(vk_command, &dst_image, 0, VK_REMAINING_MIP_LEVELS, transition_aspects,
	               initial_layout, vk::ImageLayout::eTransferDstOptimal);
	vk_command.copyBufferToImage(src_buffer.buffer, dst_image.image,
	                             vk::ImageLayout::eTransferDstOptimal,
	                             static_cast<uint32_t>(regions.size()), regions.data());
	SetImageLayout(vk_command, &dst_image, 0, VK_REMAINING_MIP_LEVELS, transition_aspects,
	               vk::ImageLayout::eTransferDstOptimal, final_layout);
}

[[nodiscard]] static std::span<const vk::BufferImageCopy>
ConvertBufferImageCopies(std::span<const BufferImageCopy> regions) {
	static thread_local std::vector<vk::BufferImageCopy> vk_regions;
	vk_regions.resize(regions.size());

	for (size_t i = 0; i < regions.size(); i++) {
		const auto& r = regions[i];
		vk_regions[i] = MakeBufferImageCopy(
		    r.offset, r.pitch, r.aspect, r.dst_level, r.dst_layer, {r.dst_x, r.dst_y, r.dst_z},
		    {r.width, (r.copy_height != 0 ? r.copy_height : r.height), 1});
	}
	return vk_regions;
}

[[nodiscard]] static std::span<const vk::BufferImageCopy>
ConvertImageBufferCopies(std::span<const ImageBufferCopy> regions) {
	static thread_local std::vector<vk::BufferImageCopy> vk_regions;
	vk_regions.resize(regions.size());
	for (size_t i = 0; i < regions.size(); i++) {
		const auto& r = regions[i];
		vk_regions[i] = MakeBufferImageCopy(
		    r.offset, r.pitch, r.aspect, r.src_level, r.src_layer, {r.src_x, r.src_y, r.src_z},
		    {r.width, r.copy_height != 0 ? r.copy_height : r.height, 1});
	}
	return vk_regions;
}

static void RecordImageToBuffer(CommandBuffer& command, VulkanImage& src_image,
                                VulkanBuffer& dst_buffer, std::span<const ImageBufferCopy> regions,
                                vk::ImageLayout final_layout) {
	auto                 vk_command = command.Handle();
	vk::ImageAspectFlags aspects    = {};
	for (const auto& region: regions) {
		aspects |= GetTransferAspects(src_image, region.aspect);
	}
	SetImageLayout(vk_command, &src_image, 0, VK_REMAINING_MIP_LEVELS, aspects, src_image.layout,
	               vk::ImageLayout::eTransferSrcOptimal);
	SetBufferMemoryBarrier(vk_command, dst_buffer.buffer, 0, VK_WHOLE_SIZE,
	                       vk::AccessFlagBits::eMemoryRead, vk::AccessFlagBits::eTransferWrite,
	                       vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer);
	const auto copies = ConvertImageBufferCopies(regions);
	vk_command.copyImageToBuffer(src_image.image, vk::ImageLayout::eTransferSrcOptimal,
	                             dst_buffer.buffer, static_cast<uint32_t>(copies.size()),
	                             copies.data());
	SetBufferMemoryBarrier(vk_command, dst_buffer.buffer, 0, VK_WHOLE_SIZE,
	                       vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
	                       vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost);
	SetImageLayout(vk_command, &src_image, 0, VK_REMAINING_MIP_LEVELS, aspects,
	               vk::ImageLayout::eTransferSrcOptimal, final_layout);
}

static void RecordImageToBuffer(CommandBuffer& command, VulkanImage& src_image,
                                VulkanBuffer& dst_buffer, uint32_t dst_pitch,
                                vk::ImageLayout final_layout, vk::ImageAspectFlags aspect) {
	const ImageBufferCopy region {
	    0, dst_pitch, 0, src_image.extent.width, src_image.extent.height, 0, 0, 0, 0, 0, aspect};
	RecordImageToBuffer(command, src_image, dst_buffer,
	                    std::span<const ImageBufferCopy>(&region, 1), final_layout);
}

class ReusableStagingBuffer {
public:
	ReusableStagingBuffer()  = default;
	~ReusableStagingBuffer() = default;
	KYTY_CLASS_NO_COPY(ReusableStagingBuffer);

	void UploadToBuffer(GraphicContext* ctx, VulkanBuffer* dst_buffer, const void* src_data,
	                    uint64_t size, uint64_t dst_offset) {
		RecordUpload<true>(ctx, src_data, size, [&](CommandBuffer*, vk::CommandBuffer vk_command) {
			SetBufferMemoryBarrier(
			    vk_command, m_buffer.buffer, 0, size, vk::AccessFlagBits::eMemoryWrite,
			    vk::AccessFlagBits::eTransferRead, vk::PipelineStageFlagBits::eAllCommands,
			    vk::PipelineStageFlagBits::eTransfer);
			SetBufferMemoryBarrier(
			    vk_command, dst_buffer->buffer, dst_offset, size,
			    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
			    vk::AccessFlagBits::eTransferWrite, vk::PipelineStageFlagBits::eAllCommands,
			    vk::PipelineStageFlagBits::eTransfer);
			const vk::BufferCopy copy_region {
			    .srcOffset = 0, .dstOffset = dst_offset, .size = size};
			vk_command.copyBuffer(m_buffer.buffer, dst_buffer->buffer, 1, &copy_region);
			SetBufferMemoryBarrier(
			    vk_command, dst_buffer->buffer, dst_offset, size,
			    vk::AccessFlagBits::eTransferWrite,
			    vk::AccessFlagBits::eVertexAttributeRead | vk::AccessFlagBits::eIndexRead |
			        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
			    vk::PipelineStageFlagBits::eTransfer,
			    vk::PipelineStageFlagBits::eVertexInput | vk::PipelineStageFlagBits::eVertexShader |
			        vk::PipelineStageFlagBits::eFragmentShader |
			        vk::PipelineStageFlagBits::eComputeShader);
		});
	}

	void UploadToImage(GraphicContext* ctx, VulkanImage* image, const void* src_data, uint64_t size,
	                   uint32_t src_pitch, vk::ImageAspectFlags copy_aspect,
	                   vk::ImageLayout initial_layout, vk::ImageLayout final_layout) {
		const auto transition_aspects = GetTransferAspects(*image, copy_aspect);
		RecordUpload<false>(ctx, src_data, size, [&](CommandBuffer* command, vk::CommandBuffer) {
			const auto region = MakeBufferImageCopy(0, src_pitch, copy_aspect, 0, 0, {0, 0, 0},
			                                        {image->extent.width, image->extent.height, 1});
			RecordBufferToImageCopy(*command, m_buffer, *image,
			                        std::span<const vk::BufferImageCopy>(&region, 1),
			                        transition_aspects, initial_layout, final_layout);
		});
	}

	void UploadToImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data,
	                   uint64_t size, std::span<const BufferImageCopy> regions,
	                   vk::ImageLayout dst_layout) {
		RecordUpload<false>(ctx, src_data, size, [&](CommandBuffer* command, vk::CommandBuffer) {
			vk::ImageAspectFlags transition_aspects = {};
			for (const auto& region: regions) {
				transition_aspects |= GetTransferAspects(*dst_image, region.aspect);
			}
			RecordBufferToImageCopy(*command, m_buffer, *dst_image,
			                        ConvertBufferImageCopies(regions), transition_aspects,
			                        dst_image->layout, dst_layout);
		});
	}

	void DownloadFromImage(GraphicContext* ctx, void* dst_data, uint64_t size, uint32_t dst_pitch,
	                       VulkanImage* src_image, vk::ImageLayout src_layout,
	                       vk::ImageAspectFlags aspect) {
		Common::LockGuard lock(m_mutex);

		EnsureBuffer(ctx, size, vk::BufferUsageFlagBits::eTransferDst);

		ExecuteImmediateCommands([&](CommandBuffer* command, vk::CommandBuffer) {
			RecordImageToBuffer(*command, *src_image, m_buffer, dst_pitch, src_layout, aspect);
		});

		std::memcpy(dst_data, m_mapped_data, size);
	}

	void DownloadFromImage(GraphicContext* ctx, void* dst_data, uint64_t size,
	                       std::span<const ImageBufferCopy> regions, VulkanImage* src_image,
	                       vk::ImageLayout src_layout) {
		Common::LockGuard lock(m_mutex);
		EnsureBuffer(ctx, size, vk::BufferUsageFlagBits::eTransferDst);
		ExecuteImmediateCommands([&](CommandBuffer* command, vk::CommandBuffer) {
			RecordImageToBuffer(*command, *src_image, m_buffer, regions, src_layout);
		});
		std::memcpy(dst_data, m_mapped_data, size);
	}

	void Release(GraphicContext* ctx) {
		Common::LockGuard lock(m_mutex);
		if (m_buffer.buffer != nullptr) {
			EXIT_IF(ctx == nullptr || m_mapped_data == nullptr);
			VulkanUnmapMemory(ctx, &m_buffer.memory);
			VulkanDeleteBuffer(ctx, &m_buffer);
		}
		m_capacity    = 0;
		m_mapped_data = nullptr;
	}

private:
	template <bool WaitIdle, typename Recorder>
	void RecordUpload(GraphicContext* ctx, const void* src_data, uint64_t size,
	                  const Recorder& recorder) {
		Common::LockGuard lock(m_mutex);
		CopyFromHost(ctx, src_data, size, vk::BufferUsageFlagBits::eTransferSrc);
		if constexpr (WaitIdle) {
			WaitForGraphicsIdle(ctx);
		}
		ExecuteImmediateCommands(recorder);
	}

	void CopyFromHost(GraphicContext* ctx, const void* src_data, uint64_t size,
	                  vk::BufferUsageFlags usage) {
		EnsureBuffer(ctx, size, usage);

		std::memcpy(m_mapped_data, src_data, size);
	}

	void EnsureBuffer(GraphicContext* ctx, uint64_t size, vk::BufferUsageFlags usage) {
		if (m_capacity < size || m_buffer.usage != usage) {
			if (m_buffer.buffer != nullptr) {
				VulkanUnmapMemory(ctx, &m_buffer.memory);
				m_mapped_data = nullptr;
				VulkanDeleteBuffer(ctx, &m_buffer);
			}

			m_buffer.usage           = usage;
			m_buffer.memory.property = vk::MemoryPropertyFlagBits::eHostVisible |
			                           vk::MemoryPropertyFlagBits::eHostCoherent;
			VulkanCreateBuffer(ctx, size, &m_buffer);
			m_capacity = size;

			VulkanMapMemory(ctx, &m_buffer.memory, &m_mapped_data);
		}
	}

	Common::Mutex m_mutex;
	VulkanBuffer  m_buffer;
	uint64_t      m_capacity    = 0;
	void*         m_mapped_data = nullptr;
};

// Do not replace with one
static ReusableStagingBuffer g_texture_staging_buffer;
static ReusableStagingBuffer g_vertex_staging_buffer;
static ReusableStagingBuffer g_readback_staging_buffer;

bool IsBlockCompressedFormat(vk::Format format) {
	return format == vk::Format::eBc1RgbUnormBlock || format == vk::Format::eBc1RgbSrgbBlock ||
	       format == vk::Format::eBc1RgbaUnormBlock || format == vk::Format::eBc1RgbaSrgbBlock ||
	       format == vk::Format::eBc2UnormBlock || format == vk::Format::eBc2SrgbBlock ||
	       format == vk::Format::eBc3UnormBlock || format == vk::Format::eBc3SrgbBlock ||
	       format == vk::Format::eBc4UnormBlock || format == vk::Format::eBc4SnormBlock ||
	       format == vk::Format::eBc5UnormBlock || format == vk::Format::eBc5SnormBlock ||
	       format == vk::Format::eBc6HUfloatBlock || format == vk::Format::eBc6HSfloatBlock ||
	       format == vk::Format::eBc7UnormBlock || format == vk::Format::eBc7SrgbBlock;
}

uint32_t BlockCompressedBytesPerBlock(vk::Format format) {
	switch (format) {
		case vk::Format::eBc1RgbUnormBlock:
		case vk::Format::eBc1RgbSrgbBlock:
		case vk::Format::eBc1RgbaUnormBlock:
		case vk::Format::eBc1RgbaSrgbBlock:
		case vk::Format::eBc4UnormBlock:
		case vk::Format::eBc4SnormBlock: return 8;
		case vk::Format::eBc2UnormBlock:
		case vk::Format::eBc2SrgbBlock:
		case vk::Format::eBc3UnormBlock:
		case vk::Format::eBc3SrgbBlock:
		case vk::Format::eBc5UnormBlock:
		case vk::Format::eBc5SnormBlock:
		case vk::Format::eBc6HUfloatBlock:
		case vk::Format::eBc6HSfloatBlock:
		case vk::Format::eBc7UnormBlock:
		case vk::Format::eBc7SrgbBlock: return 16;
		default: return 0;
	}
}

static uint32_t GetFormatTexelSize(vk::Format format) {
	switch (format) {
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR32G32Uint: return 8;
		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eR32G32B32A32Uint: return 16;
		default: return 0;
	}
}

static ReusableStagingBuffer* GetStagingBuffer(StagingBufferType type) {
	switch (type) {
		case StagingBufferType::Texture: return &g_texture_staging_buffer;
		case StagingBufferType::Vertex: return &g_vertex_staging_buffer;
		case StagingBufferType::ReadBack: return &g_readback_staging_buffer;
		default: EXIT("unknown staging buffer type\n");
	}

	return nullptr;
}

void ReleaseCachedResources(GraphicContext* ctx) {
	EXIT_IF(ctx == nullptr);
	g_texture_staging_buffer.Release(ctx);
	g_vertex_staging_buffer.Release(ctx);
	g_readback_staging_buffer.Release(ctx);
}

static void SetImageLayout(vk::CommandBuffer buffer, VulkanImage* dst_image, uint32_t base_level,
                           uint32_t levels, vk::ImageAspectFlags aspect_mask,
                           vk::ImageLayout old_image_layout, vk::ImageLayout new_image_layout) {
	if ((old_image_layout == vk::ImageLayout::eTransferDstOptimal &&
	     new_image_layout == vk::ImageLayout::eTransferSrcOptimal) ||
	    (old_image_layout == vk::ImageLayout::eTransferDstOptimal &&
	     new_image_layout == vk::ImageLayout::eGeneral) ||
	    (old_image_layout == vk::ImageLayout::eTransferDstOptimal &&
	     new_image_layout == vk::ImageLayout::ePresentSrcKHR)) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
			LOGF("set_image_layout: image=%p type=%d tracked=%d requested=%d->%d base=%u "
			     "levels=%u\n",
			     VulkanHandleToPointer(dst_image->image), static_cast<int>(dst_image->type),
			     static_cast<int>(dst_image->layout), static_cast<int>(old_image_layout),
			     static_cast<int>(new_image_layout), base_level, levels);
		}
	}

	if (old_image_layout == new_image_layout) {
		dst_image->layout = new_image_layout;
		return;
	}

	vk::ImageMemoryBarrier image_memory_barrier {};
	image_memory_barrier.sType                           = vk::StructureType::eImageMemoryBarrier;
	image_memory_barrier.pNext                           = nullptr;
	image_memory_barrier.srcAccessMask                   = {};
	image_memory_barrier.dstAccessMask                   = {};
	image_memory_barrier.oldLayout                       = old_image_layout;
	image_memory_barrier.newLayout                       = new_image_layout;
	image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.image                           = dst_image->image;
	image_memory_barrier.subresourceRange.aspectMask     = aspect_mask;
	image_memory_barrier.subresourceRange.baseMipLevel   = base_level;
	image_memory_barrier.subresourceRange.levelCount     = levels;
	image_memory_barrier.subresourceRange.baseArrayLayer = 0;
	image_memory_barrier.subresourceRange.layerCount     = dst_image->layers;

	switch (old_image_layout) {
		case vk::ImageLayout::eColorAttachmentOptimal:
			image_memory_barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
			break;
		case vk::ImageLayout::eDepthStencilAttachmentOptimal:
			image_memory_barrier.srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead |
			                                     vk::AccessFlagBits::eDepthStencilAttachmentWrite;
			break;
		case vk::ImageLayout::eGeneral:
			image_memory_barrier.srcAccessMask =
			    vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite |
			    vk::AccessFlagBits::eColorAttachmentRead |
			    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eTransferRead |
			    vk::AccessFlagBits::eTransferWrite;
			break;
		case vk::ImageLayout::eTransferDstOptimal:
			image_memory_barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
			break;
		case vk::ImageLayout::eTransferSrcOptimal:
			image_memory_barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
			break;
		case vk::ImageLayout::ePreinitialized:
			image_memory_barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
			break;
		default: break;
	}

	switch (new_image_layout) {
		case vk::ImageLayout::eTransferDstOptimal:
			image_memory_barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
			break;
		case vk::ImageLayout::eTransferSrcOptimal:
			image_memory_barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
			break;
		case vk::ImageLayout::eShaderReadOnlyOptimal:
			image_memory_barrier.srcAccessMask |=
			    vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eHostWrite;
			image_memory_barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
			break;
		case vk::ImageLayout::eGeneral:
			image_memory_barrier.dstAccessMask =
			    vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
			break;
		case vk::ImageLayout::eColorAttachmentOptimal:
			image_memory_barrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
			break;
		case vk::ImageLayout::eDepthStencilAttachmentOptimal:
			image_memory_barrier.dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead |
			                                     vk::AccessFlagBits::eDepthStencilAttachmentWrite;
			break;
		default: break;
	}

	auto src_stages = static_cast<vk::PipelineStageFlags>(
	    vk::PipelineStageFlagBits::eTopOfPipe | vk::PipelineStageFlagBits::eTransfer |
	    vk::PipelineStageFlagBits::eColorAttachmentOutput |
	    vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eEarlyFragmentTests |
	    vk::PipelineStageFlagBits::eLateFragmentTests | vk::PipelineStageFlagBits::eHost);
	auto dest_stages = static_cast<vk::PipelineStageFlags>(
	    vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eVertexShader |
	    vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader |
	    vk::PipelineStageFlagBits::eColorAttachmentOutput |
	    vk::PipelineStageFlagBits::eEarlyFragmentTests |
	    vk::PipelineStageFlagBits::eLateFragmentTests);

	buffer.pipelineBarrier(src_stages, dest_stages, vk::DependencyFlags {}, 0, nullptr, 0, nullptr,
	                       1, &image_memory_barrier);

	dst_image->layout = new_image_layout;
}

static uint64_t CompressedImageCopyBufferSize(const ImageImageCopy& r,
                                              const VulkanImage&    dst_image) {
	if (r.src_aspect != vk::ImageAspectFlagBits::eColor ||
	    r.dst_aspect != vk::ImageAspectFlagBits::eColor ||
	    !IsBlockCompressedFormat(dst_image.format)) {
		return 0;
	}

	auto src_texel_size = GetFormatTexelSize(r.src_image->format);
	auto dst_block_size = BlockCompressedBytesPerBlock(dst_image.format);
	if (src_texel_size == 0 || src_texel_size != dst_block_size) {
		return 0;
	}

	auto block_width  = (r.width + 3u) / 4u;
	auto block_height = (r.height + 3u) / 4u;
	if (block_width > r.src_image->extent.width || block_height > r.src_image->extent.height) {
		return 0;
	}
	return static_cast<uint64_t>(block_width) * block_height * dst_block_size;
}

static void CopyBlockImageToCompressedImage(vk::CommandBuffer vk_buffer, const ImageImageCopy& r,
                                            VulkanImage* dst_image, VulkanBuffer* copy_buffer) {
	const auto copy_size = CompressedImageCopyBufferSize(r, *dst_image);
	EXIT_IF(copy_size == 0 || copy_buffer == nullptr || copy_buffer->buffer_size < copy_size);

	const auto block_width  = (r.width + 3u) / 4u;
	const auto block_height = (r.height + 3u) / 4u;

	const auto src_region =
	    MakeBufferImageCopy(0, 0, vk::ImageAspectFlagBits::eColor, r.src_level, r.src_layer,
	                        {r.src_x, r.src_y, 0}, {block_width, block_height, 1});

	auto src_layout = r.src_image->layout;
	SetImageLayout(vk_buffer, r.src_image, r.src_level, 1, vk::ImageAspectFlagBits::eColor,
	               src_layout, vk::ImageLayout::eTransferSrcOptimal);

	SetBufferMemoryBarrier(vk_buffer, copy_buffer->buffer, 0, VK_WHOLE_SIZE,
	                       vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eTransferWrite,
	                       vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eTransfer);
	vk_buffer.copyImageToBuffer(r.src_image->image, vk::ImageLayout::eTransferSrcOptimal,
	                            copy_buffer->buffer, 1, &src_region);
	SetBufferMemoryBarrier(vk_buffer, copy_buffer->buffer, 0, VK_WHOLE_SIZE,
	                       vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
	                       vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eTransfer);

	const auto dst_region =
	    MakeBufferImageCopy(0, 0, vk::ImageAspectFlagBits::eColor, r.dst_level, r.dst_layer,
	                        {r.dst_x, r.dst_y, 0}, {r.width, r.height, 1});

	vk_buffer.copyBufferToImage(copy_buffer->buffer, dst_image->image,
	                            vk::ImageLayout::eTransferDstOptimal, 1, &dst_region);

	SetImageLayout(vk_buffer, r.src_image, r.src_level, 1, vk::ImageAspectFlagBits::eColor,
	               vk::ImageLayout::eTransferSrcOptimal, src_layout);
}

void CopyImage(CommandBuffer* buffer, std::span<const ImageImageCopy> regions,
               VulkanImage* dst_image, vk::ImageLayout dst_layout) {
	auto vk_buffer = buffer->Handle();

	bool                 same_image_copy        = false;
	vk::ImageAspectFlags dst_transition_aspects = {};
	for (const auto& r: regions) {
		dst_transition_aspects |= GetTransferAspects(*dst_image, r.dst_aspect);
		if (r.src_image == dst_image) {
			same_image_copy = true;
			break;
		}
	}

	if (same_image_copy) {
		SetImageLayout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, dst_transition_aspects,
		               dst_image->layout, vk::ImageLayout::eGeneral);

		for (const auto& r: regions) {
			vk::ImageCopy region;

			region.srcSubresource.aspectMask     = r.src_aspect;
			region.srcSubresource.mipLevel       = r.src_level;
			region.srcSubresource.baseArrayLayer = r.src_layer;
			region.srcSubresource.layerCount     = 1;
			region.srcOffset                     = {r.src_x, r.src_y, r.src_z};
			region.dstSubresource.aspectMask     = r.dst_aspect;
			region.dstSubresource.mipLevel       = r.dst_level;
			region.dstSubresource.baseArrayLayer = r.dst_layer;
			region.dstSubresource.layerCount     = 1;
			region.dstOffset                     = {r.dst_x, r.dst_y, r.dst_z};
			region.extent                        = {r.width, r.height, 1};

			auto src_layout     = vk::ImageLayout::eGeneral;
			auto restore_layout = vk::ImageLayout::eGeneral;
			if (r.src_image != dst_image) {
				restore_layout = r.src_image->layout;
				SetImageLayout(vk_buffer, r.src_image, r.src_level, 1,
				               GetTransferAspects(*r.src_image, r.src_aspect), restore_layout,
				               vk::ImageLayout::eTransferSrcOptimal);
				src_layout = vk::ImageLayout::eTransferSrcOptimal;
			}

			vk_buffer.copyImage(r.src_image->image, src_layout, dst_image->image,
			                    vk::ImageLayout::eGeneral, 1, &region);

			if (r.src_image != dst_image) {
				SetImageLayout(vk_buffer, r.src_image, r.src_level, 1,
				               GetTransferAspects(*r.src_image, r.src_aspect),
				               vk::ImageLayout::eTransferSrcOptimal, restore_layout);
			}
		}

		SetImageLayout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, dst_transition_aspects,
		               vk::ImageLayout::eGeneral, dst_layout);
		return;
	}

	SetImageLayout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, dst_transition_aspects,
	               dst_image->layout, vk::ImageLayout::eTransferDstOptimal);
	uint64_t compressed_copy_size = 0;
	for (const auto& r: regions) {
		compressed_copy_size =
		    std::max(compressed_copy_size, CompressedImageCopyBufferSize(r, *dst_image));
	}
	VulkanBuffer* compressed_copy_buffer = nullptr;
	if (compressed_copy_size != 0) {
		compressed_copy_buffer = new VulkanBuffer;
		compressed_copy_buffer->usage =
		    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
		compressed_copy_buffer->memory.property = {};
		VulkanCreateBuffer(WindowGetGraphicContext(), compressed_copy_size, compressed_copy_buffer);
		buffer->DeleteAfterFence(compressed_copy_buffer);
	}

	for (const auto& r: regions) {
		vk::ImageCopy region;

		auto src_layout = r.src_image->layout;

		if (CompressedImageCopyBufferSize(r, *dst_image) != 0) {
			CopyBlockImageToCompressedImage(vk_buffer, r, dst_image, compressed_copy_buffer);
			continue;
		}

		region.srcSubresource.aspectMask     = r.src_aspect;
		region.srcSubresource.mipLevel       = r.src_level;
		region.srcSubresource.baseArrayLayer = r.src_layer;
		region.srcSubresource.layerCount     = 1;
		region.srcOffset                     = {r.src_x, r.src_y, r.src_z};
		region.dstSubresource.aspectMask     = r.dst_aspect;
		region.dstSubresource.mipLevel       = r.dst_level;
		region.dstSubresource.baseArrayLayer = r.dst_layer;
		region.dstSubresource.layerCount     = 1;
		region.dstOffset                     = {r.dst_x, r.dst_y, r.dst_z};
		region.extent                        = {r.width, r.height, 1};

		SetImageLayout(vk_buffer, r.src_image, r.src_level, 1,
		               GetTransferAspects(*r.src_image, r.src_aspect), src_layout,
		               vk::ImageLayout::eTransferSrcOptimal);

		vk_buffer.copyImage(r.src_image->image, vk::ImageLayout::eTransferSrcOptimal,
		                    dst_image->image, vk::ImageLayout::eTransferDstOptimal, 1, &region);

		SetImageLayout(vk_buffer, r.src_image, r.src_level, 1,
		               GetTransferAspects(*r.src_image, r.src_aspect),
		               vk::ImageLayout::eTransferSrcOptimal, src_layout);
	}

	SetImageLayout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, dst_transition_aspects,
	               vk::ImageLayout::eTransferDstOptimal, dst_layout);
}

void CopyImageViaBuffer(CommandBuffer* buffer, GraphicContext* ctx, VulkanImage* src_image,
                        vk::ImageAspectFlags src_aspect, VulkanImage* dst_image,
                        vk::ImageAspectFlags dst_aspect, uint32_t bytes_per_element,
                        vk::ImageLayout dst_layout) {
	if (!IsSingleImageAspect(src_aspect) || !IsSingleImageAspect(dst_aspect) ||
	    (bytes_per_element != 2 && bytes_per_element != 4) || src_image->layers != 1 ||
	    dst_image->layers != 1 || src_image->mip_levels != 1 || dst_image->mip_levels != 1 ||
	    src_image->extent.width == 0 || src_image->extent.height == 0 ||
	    src_image->extent.width != dst_image->extent.width ||
	    src_image->extent.height != dst_image->extent.height) {
		EXIT("unsupported image-buffer-image copy, src_aspect=0x%x dst_aspect=0x%x bpe=%u "
		     "src=%ux%u/%u/%u dst=%ux%u/%u/%u\n",
		     static_cast<vk::ImageAspectFlags::MaskType>(src_aspect),
		     static_cast<vk::ImageAspectFlags::MaskType>(dst_aspect), bytes_per_element,
		     src_image->extent.width, src_image->extent.height, src_image->layers,
		     src_image->mip_levels, dst_image->extent.width, dst_image->extent.height,
		     dst_image->layers, dst_image->mip_levels);
	}

	const uint64_t row_bytes = static_cast<uint64_t>(src_image->extent.width) * bytes_per_element;
	constexpr uint64_t MAX_COPY_BUFFER_SIZE = 16ull * 1024ull * 1024ull;
	if (row_bytes == 0 || row_bytes > MAX_COPY_BUFFER_SIZE) {
		EXIT("unsupported image-buffer-image row size: 0x%016" PRIx64 "\n", row_bytes);
	}
	const auto rows_per_chunk = static_cast<uint32_t>(
	    std::min<uint64_t>(src_image->extent.height, MAX_COPY_BUFFER_SIZE / row_bytes));
	const auto copy_buffer_size = row_bytes * rows_per_chunk;

	// Kyty does not yet have a cross-queue scheduler. Drain submitted work once before
	// recording the recreate copy; commands already recorded on this buffer remain ordered by the
	// barriers below. The preservation policy stays isolated here so a future scheduler can replace
	// this synchronization without changing texture-cache ownership logic.
	WaitForGraphicsIdle(ctx);
	auto* copy_buffer = new VulkanBuffer;
	copy_buffer->usage =
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
	copy_buffer->memory.property = vk::MemoryPropertyFlagBits::eDeviceLocal;
	VulkanCreateBuffer(ctx, copy_buffer_size, copy_buffer);
	buffer->DeleteAfterFence(copy_buffer);

	auto       vk_buffer    = buffer->Handle();
	const auto src_layout   = src_image->layout;
	const auto final_layout = dst_layout;
	SetImageLayout(vk_buffer, src_image, 0, 1, src_aspect, src_layout,
	               vk::ImageLayout::eTransferSrcOptimal);
	SetImageLayout(vk_buffer, dst_image, 0, 1, dst_aspect, dst_image->layout,
	               vk::ImageLayout::eTransferDstOptimal);

	for (uint32_t row = 0; row < src_image->extent.height; row += rows_per_chunk) {
		const auto rows = std::min(rows_per_chunk, src_image->extent.height - row);
		auto copy = MakeBufferImageCopy(0, 0, src_aspect, 0, 0, {0, static_cast<int32_t>(row), 0},
		                                {src_image->extent.width, rows, 1});
		vk_buffer.copyImageToBuffer(src_image->image, vk::ImageLayout::eTransferSrcOptimal,
		                            copy_buffer->buffer, 1, &copy);

		SetBufferMemoryBarrier(
		    vk_buffer, copy_buffer->buffer, 0, copy_buffer_size, vk::AccessFlagBits::eTransferWrite,
		    vk::AccessFlagBits::eTransferRead, vk::PipelineStageFlagBits::eTransfer,
		    vk::PipelineStageFlagBits::eTransfer);

		copy.imageSubresource.aspectMask = dst_aspect;
		vk_buffer.copyBufferToImage(copy_buffer->buffer, dst_image->image,
		                            vk::ImageLayout::eTransferDstOptimal, 1, &copy);
		if (row + rows < src_image->extent.height) {
			SetBufferMemoryBarrier(
			    vk_buffer, copy_buffer->buffer, 0, copy_buffer_size,
			    vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eTransferWrite,
			    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer);
		}
	}

	SetImageLayout(vk_buffer, src_image, 0, 1, src_aspect, vk::ImageLayout::eTransferSrcOptimal,
	               src_layout);
	SetImageLayout(vk_buffer, dst_image, 0, 1, dst_aspect, vk::ImageLayout::eTransferDstOptimal,
	               final_layout);
}

void BlitToSwapchain(CommandBuffer* buffer, VulkanImage* src_image,
                     VulkanSwapchain* dst_swapchain) {
	auto vk_buffer = buffer->Handle();
	if (src_image->layout != vk::ImageLayout::eTransferSrcOptimal) {
		EXIT("invalid prepared presentation image, image=%p vk_image=%p layout=%d\n",
		     static_cast<const void*>(src_image),
		     src_image != nullptr ? static_cast<void*>(src_image->image) : nullptr,
		     src_image != nullptr ? static_cast<int>(src_image->layout) : -1);
	}

	VulkanImage swapchain_image(VulkanImageType::Unknown);

	swapchain_image.image  = dst_swapchain->swapchain_images[dst_swapchain->current_index];
	swapchain_image.layout = vk::ImageLayout::eUndefined;

	SetImageLayout(vk_buffer, &swapchain_image, 0, 1, vk::ImageAspectFlagBits::eColor,
	               vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

	vk::ImageBlit region {};
	region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
	region.srcSubresource.mipLevel       = 0;
	region.srcSubresource.baseArrayLayer = 0;
	region.srcSubresource.layerCount     = 1;
	region.srcOffsets[0].x               = 0;
	region.srcOffsets[0].y               = 0;
	region.srcOffsets[0].z               = 0;
	region.srcOffsets[1].x               = static_cast<int>(src_image->extent.width);
	region.srcOffsets[1].y               = static_cast<int>(src_image->extent.height);
	region.srcOffsets[1].z               = 1;
	region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
	region.dstSubresource.mipLevel       = 0;
	region.dstSubresource.baseArrayLayer = 0;
	region.dstSubresource.layerCount     = 1;
	region.dstOffsets[0].x               = 0;
	region.dstOffsets[0].y               = 0;
	region.dstOffsets[0].z               = 0;
	region.dstOffsets[1].x               = static_cast<int>(dst_swapchain->swapchain_extent.width);
	region.dstOffsets[1].y               = static_cast<int>(dst_swapchain->swapchain_extent.height);
	region.dstOffsets[1].z               = 1;

	vk_buffer.blitImage(src_image->image, vk::ImageLayout::eTransferSrcOptimal,
	                    swapchain_image.image, vk::ImageLayout::eTransferDstOptimal, 1, &region,
	                    vk::Filter::eLinear);
}

void ClearColorImage(CommandBuffer* buffer, VulkanImage* image, const vk::ClearColorValue& color) {
	auto vk_buffer = buffer->Handle();
	SetImageLayout(vk_buffer, image, 0, 1, vk::ImageAspectFlagBits::eColor, image->layout,
	               vk::ImageLayout::eTransferDstOptimal);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk_buffer.clearColorImage(image->image, vk::ImageLayout::eTransferDstOptimal, &color, 1,
	                          &range);
	SetImageLayout(vk_buffer, image, 0, 1, vk::ImageAspectFlagBits::eColor,
	               vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal);
}

void UploadImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size,
                 uint32_t src_pitch, vk::ImageLayout dst_layout) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(size == 0);

	GetStagingBuffer(StagingBufferType::Texture)
	    ->UploadToImage(ctx, dst_image, src_data, size, src_pitch, vk::ImageAspectFlagBits::eColor,
	                    vk::ImageLayout::eUndefined, dst_layout);
}

void DownloadImage(GraphicContext* ctx, void* dst_data, uint64_t size, uint32_t dst_pitch,
                   VulkanImage* src_image, vk::ImageLayout src_layout,
                   vk::ImageAspectFlags aspect) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(size == 0);

	GetStagingBuffer(StagingBufferType::ReadBack)
	    ->DownloadFromImage(ctx, dst_data, size, dst_pitch, src_image, src_layout, aspect);
}

void DownloadImage(GraphicContext* ctx, void* dst_data, uint64_t size,
                   std::span<const ImageBufferCopy> regions, VulkanImage* src_image,
                   vk::ImageLayout src_layout) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(size == 0 || regions.empty());
	GetStagingBuffer(StagingBufferType::ReadBack)
	    ->DownloadFromImage(ctx, dst_data, size, regions, src_image, src_layout);
}

void UploadImage(GraphicContext* ctx, VulkanImage* image, const void* src_data, uint64_t size,
                 std::span<const BufferImageCopy> regions, vk::ImageLayout dst_layout) {
	EXIT_IF(size == 0 || regions.empty());

	GetStagingBuffer(StagingBufferType::Texture)
	    ->UploadToImage(ctx, image, src_data, size, regions, dst_layout);
}

void CopyImageImmediate(GraphicContext* ctx, std::span<const ImageImageCopy> regions,
                        VulkanImage* dst_image, vk::ImageLayout dst_layout) {
	ExecuteImmediateCommands([&](CommandBuffer* command, vk::CommandBuffer) {
		CopyImage(command, regions, dst_image, dst_layout);
	});
}

void UploadImage(GraphicContext* ctx, DepthStencilVulkanImage* image, const void* src_data,
                 uint64_t size, uint32_t src_pitch, vk::ImageAspectFlags aspect) {
	EXIT_IF(size == 0);
	GetStagingBuffer(StagingBufferType::Texture)
	    ->UploadToImage(ctx, image, src_data, size, src_pitch, aspect, image->layout,
	                    vk::ImageLayout::eDepthStencilAttachmentOptimal);
}

void UploadBuffer(GraphicContext* ctx, StagingBufferType type, VulkanBuffer* dst_buffer,
                  uint64_t dst_offset, const void* src_data, uint64_t size) {
	EXIT_IF(size == 0);
	EXIT_IF(dst_offset > dst_buffer->buffer_size || size > dst_buffer->buffer_size - dst_offset);
	GetStagingBuffer(type)->UploadToBuffer(ctx, dst_buffer, src_data, size, dst_offset);
}

void CopyBuffer(VulkanBuffer* src_buffer, VulkanBuffer* dst_buffer, uint64_t size) {
	EXIT_IF(size == 0 || size > src_buffer->buffer_size || size > dst_buffer->buffer_size);

	ExecuteImmediateCommands([&](CommandBuffer*, vk::CommandBuffer vk_command) {
		SetBufferMemoryBarrier(vk_command, src_buffer->buffer, 0, size,
		                       vk::AccessFlagBits::eMemoryWrite, vk::AccessFlagBits::eTransferRead,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer);
		SetBufferMemoryBarrier(vk_command, dst_buffer->buffer, 0, size,
		                       vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
		                       vk::AccessFlagBits::eTransferWrite,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer);
		const vk::BufferCopy copy_region {.srcOffset = 0, .dstOffset = 0, .size = size};
		vk_command.copyBuffer(src_buffer->buffer, dst_buffer->buffer, 1, &copy_region);
		SetBufferMemoryBarrier(
		    vk_command, dst_buffer->buffer, 0, size, vk::AccessFlagBits::eTransferWrite,
		    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
		    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands);
	});
}

void DownloadBuffer(GraphicContext* ctx, VulkanBuffer* src_buffer, uint64_t src_offset,
                    void* dst_data, uint64_t size) {
	EXIT_IF(size == 0);
	EXIT_IF(src_offset > src_buffer->buffer_size || size > src_buffer->buffer_size - src_offset);
	const auto family = ctx->queues[GraphicContext::QUEUE_UTIL].family;
	EXIT_IF(family == static_cast<uint32_t>(-1));
	for (int i = GraphicContext::QUEUE_COMPUTE_START;
	     i < GraphicContext::QUEUE_COMPUTE_START + GraphicContext::QUEUE_COMPUTE_NUM; i++) {
		EXIT_IF(ctx->queues[i].family != family);
	}
	EXIT_IF(ctx->queues[GraphicContext::QUEUE_GFX].family != family);

	if (src_buffer->memory.property & vk::MemoryPropertyFlagBits::eHostVisible) {
		void* mapped = nullptr;
		VulkanMapMemory(ctx, &src_buffer->memory, &mapped);
		std::memcpy(dst_data, static_cast<const uint8_t*>(mapped) + src_offset, size);
		VulkanUnmapMemory(ctx, &src_buffer->memory);
		return;
	}

	VulkanBuffer readback {};
	readback.usage           = vk::BufferUsageFlagBits::eTransferDst;
	readback.memory.property = vk::MemoryPropertyFlagBits::eHostVisible |
	                           vk::MemoryPropertyFlagBits::eHostCoherent |
	                           vk::MemoryPropertyFlagBits::eHostCached;
	VulkanCreateBuffer(ctx, size, &readback);

	ExecuteImmediateCommands([&](CommandBuffer*, vk::CommandBuffer vk_command) {
		SetBufferMemoryBarrier(vk_command, src_buffer->buffer, src_offset, size,
		                       vk::AccessFlagBits::eMemoryWrite, vk::AccessFlagBits::eTransferRead,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer);
		const vk::BufferCopy copy {.srcOffset = src_offset, .dstOffset = 0, .size = size};
		vk_command.copyBuffer(src_buffer->buffer, readback.buffer, 1, &copy);
		SetBufferMemoryBarrier(vk_command, readback.buffer, 0, size,
		                       vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eHost);
	});

	void* mapped = nullptr;
	VulkanMapMemory(ctx, &readback.memory, &mapped);
	std::memcpy(dst_data, mapped, size);
	VulkanUnmapMemory(ctx, &readback.memory);
	VulkanDeleteBuffer(ctx, &readback);
}

} // namespace Libs::Graphics::Transfer
