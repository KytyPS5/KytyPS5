#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/imeOverlay.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/windowInternal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Libs::Graphics {

uint64_t PresentReadbackEnvU64(const char* name, uint64_t fallback) {
	const char* value = std::getenv(name);
	if (value == nullptr || *value == '\0') {
		return fallback;
	}
	char*                  end    = nullptr;
	const unsigned long long parsed = std::strtoull(value, &end, 10);
	if (end == value || *end != '\0') {
		return fallback;
	}
	return static_cast<uint64_t>(parsed);
}

struct Presenter::Frame {
	VulkanImage image;
	uint64_t    present_tick = 0;
	bool        busy          = false;
	bool        reusing_last  = false;
	bool        guest_surface = false;
	Image*      guest_source  = nullptr;

	void Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format);
	void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access);
	void CopyFrom(CommandBuffer& command, Image& source);
	void Clear(CommandBuffer& command, const vk::ClearColorValue& color);
};

class FramePool final {
public:
	FramePool(WindowContext& window, CommandScheduler& scheduler)
	    : m_window(window), m_scheduler(scheduler) {}
	~FramePool() {
		m_scheduler.Wait(m_scheduler.CurrentTick() - 1);
		for (auto& frame: m_frames) {
			if (frame->image.image != nullptr) {
				m_window.graphic_ctx.DeleteImage(frame->image);
			}
		}
	}
	KYTY_CLASS_NO_COPY(FramePool);

	void Initialize(uint32_t count, vk::Format format) {
		if (count == 0 || format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires at least one frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!m_frames.empty()) {
			EXIT("prepared-frame pool was initialized twice\n");
		}
		m_format = format;
		m_frames.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	void SetFormat(vk::Format format) {
		if (format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires a presentation format\n");
		}
		Common::LockGuard lock(m_mutex);
		m_format = format;
	}

	vk::Format GetFormat() {
		Common::LockGuard lock(m_mutex);
		if (m_format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool has no presentation format\n");
		}
		return m_format;
	}

	Presenter::Frame* Acquire() {
		m_mutex.Lock();
		if (m_frames.empty()) {
			EXIT("prepared-frame pool was used before swapchain initialization\n");
		}
		while (m_free.empty()) {
			m_available.Wait(&m_mutex);
		}
		auto* frame = m_free.front();
		m_free.pop_front();
		if (frame->busy) {
			EXIT("prepared-frame pool returned an invalid frame\n");
		}
		if (m_last_frame == frame) {
			m_last_frame = nullptr;
		}
		frame->busy          = true;
		frame->reusing_last  = false;
		frame->guest_surface = false;
		frame->guest_source  = nullptr;
		m_mutex.Unlock();

		WaitForFrame(*frame);
		return frame;
	}

	Presenter::Frame* AcquireLast() {
		m_mutex.Lock();
		auto* frame = m_last_frame;
		if (frame == nullptr) {
			m_mutex.Unlock();
			return nullptr;
		}
		auto free = std::find(m_free.begin(), m_free.end(), frame);
		if (free == m_free.end() || frame->busy) {
			m_mutex.Unlock();
			EXIT("last submitted frame is not available for reuse\n");
		}
		m_free.erase(free);
		m_last_frame        = nullptr;
		frame->busy         = true;
		frame->reusing_last = true;
		m_mutex.Unlock();

		WaitForFrame(*frame);
		return frame;
	}

	void ValidateForPresent(Presenter::Frame* frame, bool reuse) {
		Common::LockGuard lock(m_mutex);
		if (frame == nullptr || !frame->busy || frame->reusing_last != reuse) {
			EXIT("prepared frame has invalid presentation ownership\n");
		}
	}

	void Release(Presenter::Frame* frame, bool make_last = false) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy         = false;
		frame->reusing_last = false;
		if (make_last) {
			m_last_frame = frame;
		}
		m_free.push_back(frame);
		m_available.Signal();
	}

private:
	void WaitForFrame(Presenter::Frame& frame) { m_scheduler.Wait(frame.present_tick); }

	WindowContext&                                 m_window;
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	Common::CondVar                                m_available;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::deque<Presenter::Frame*>                  m_free;
	Presenter::Frame*                              m_last_frame = nullptr;
	vk::Format                                     m_format     = vk::Format::eUndefined;
};

void Presenter::Frame::Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}
	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	const auto required =
	    vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	    vk::FormatFeatureFlagBits::eTransferSrc | vk::FormatFeatureFlagBits::eTransferDst;
	if ((features & required) != required) {
		EXIT("prepared presentation format lacks optimal blit support: format=%d features=0x%x\n",
		     static_cast<int>(format), static_cast<vk::FormatFeatureFlags::MaskType>(features));
	}

	auto&      dst        = image;
	const bool compatible = dst.image != nullptr && dst.extent.width == extent.width &&
	                        dst.extent.height == extent.height && dst.format == format;
	if (compatible) {
		return;
	}
	if (dst.image != nullptr) {
		graphics.DeleteImage(dst);
		dst.memory = {};
	}

	dst.extent     = {extent.width, extent.height, 1};
	dst.format     = format;
	dst.layers     = 1;
	dst.mip_levels = 1;
	dst.state      = {};
	dst.subresource_states.clear();
	dst.memory.property = vk::MemoryPropertyFlagBits::eDeviceLocal;

	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {dst.extent.width, dst.extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = dst.format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, dst)) {
		EXIT("failed to allocate prepared presentation image, extent=%ux%u format=%d\n",
		     dst.extent.width, dst.extent.height, static_cast<int>(dst.format));
	}
}

void Presenter::Frame::Transit(vk::CommandBuffer command, vk::ImageLayout layout,
                               vk::AccessFlags2 access) {
	const auto     stage  = access == vk::AccessFlagBits2::eTransferRead ||
	                                access == vk::AccessFlagBits2::eTransferWrite
	                            ? vk::PipelineStageFlagBits2::eTransfer
	                            : vk::PipelineStageFlagBits2::eAllCommands;
	constexpr auto writes = vk::AccessFlagBits2::eTransferWrite |
	                        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eMemoryWrite;
	if (image.state.layout == layout && image.state.access_mask == access &&
	    !static_cast<bool>(image.state.access_mask & writes)) {
		return;
	}
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = image.state.pl_stage;
	barrier.srcAccessMask                   = image.state.access_mask;
	barrier.dstStageMask                    = stage;
	barrier.dstAccessMask                   = access;
	barrier.oldLayout                       = image.state.layout;
	barrier.newLayout                       = layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state = {stage, access, layout};
	image.subresource_states.clear();
}

void Presenter::Frame::CopyFrom(CommandBuffer& command_buffer, Image& source) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, source.backing.layers};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, image.layers};
	copy.extent         = {std::min(source.backing.extent.width, image.extent.width),
	                       std::min(source.backing.extent.height, image.extent.height), 1};
	EXIT_IF(copy.srcSubresource.layerCount != copy.dstSubresource.layerCount);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
	Transit(command, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead);
}

void Presenter::Frame::Clear(CommandBuffer& command_buffer, const vk::ClearColorValue& color) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
	Transit(command, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead);
}

class Swapchain final {
public:
	enum class Status : uint8_t { Success, Recreate, SurfaceLost };

	explicit Swapchain(WindowContext& window): m_window(window) {}
	~Swapchain();
	KYTY_CLASS_NO_COPY(Swapchain);

	void                 Create();
	void                 Recreate(bool surface_lost = false);
	[[nodiscard]] Status AcquireNextImage();
	[[nodiscard]] bool   PrepareImeOverlay();
	void RecordPresentCommands(CommandBuffer& command, VulkanImage& source, bool draw_ime_overlay);
	uint64_t             Submit(CommandScheduler& scheduler);
	[[nodiscard]] Status Present();

	[[nodiscard]] uint32_t ImageCount() const noexcept {
		return static_cast<uint32_t>(m_images.size());
	}
	[[nodiscard]] vk::Format Format() const noexcept { return m_format; }

private:
	void Destroy();

	WindowContext&              m_window;
	vk::SwapchainKHR            m_handle = nullptr;
	vk::Format                  m_format = vk::Format::eUndefined;
	vk::Extent2D                m_extent {};
	std::vector<vk::Image>      m_images;
	std::vector<vk::ImageView>  m_image_views;
	std::vector<vk::Semaphore>  m_image_acquired;
	std::vector<vk::Semaphore>  m_render_complete;
	std::unique_ptr<ImeOverlay> m_ime_overlay;
	uint32_t                    m_image_index = static_cast<uint32_t>(-1);
	uint32_t                    m_frame_index = 0;
};

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner),
	      present_scheduler(renderer, owner.graphic_ctx), frames(owner, present_scheduler) {
		EXIT_IF(owner.render_context == nullptr);
		swapchain.Create();
		frames.Initialize(swapchain.ImageCount(), swapchain.Format());
	}

	void RecoverSwapchain(Swapchain::Status status) {
		LOGF("Recovering Vulkan swapchain%s\n",
		     status == Swapchain::Status::SurfaceLost ? " and surface" : "");
		swapchain.Recreate(status == Swapchain::Status::SurfaceLost);
		frames.SetFormat(swapchain.Format());
	}

	void CapturePreparedFrame(Presenter::Frame& frame) {
		const char* path = std::getenv("KYTY_PRESENT_READBACK_PATH");
		const bool capture_source = std::getenv("KYTY_PRESENT_READBACK_SOURCE") != nullptr;
		const auto* source_image =
		    capture_source && frame.guest_source != nullptr ? frame.guest_source : nullptr;
		const auto capture_format =
		    source_image != nullptr ? source_image->backing.format : frame.image.format;
		const auto capture_extent = source_image != nullptr
		                                ? source_image->backing.extent
		                                : vk::Extent3D {frame.image.extent.width,
		                                                frame.image.extent.height, 1};
		const auto capture_handle =
		    source_image != nullptr ? source_image->backing.image : frame.image.image;
		static uint32_t trace_count = 0;
		if (path != nullptr && *path != '\0' && trace_count < 16) {
			std::printf("PresentReadback: guest=%d format=%d extent=%ux%u count=%" PRIu64 "\n",
			            frame.guest_surface ? 1 : 0, static_cast<int>(capture_format),
			            capture_extent.width, capture_extent.height, present_readback_count);
			std::fflush(stdout);
			trace_count++;
		}
		if (!frame.guest_surface || path == nullptr || *path == '\0') {
			return;
		}
		const auto frame_index = present_readback_frame++;
		const auto frame_start = PresentReadbackEnvU64("KYTY_PRESENT_READBACK_START", 0);
		const auto frame_limit = PresentReadbackEnvU64("KYTY_PRESENT_READBACK_LIMIT", 8);
		if (frame_index < frame_start || present_readback_count >= frame_limit) {
			return;
		}
		if (capture_format != vk::Format::eA2B10G10R10UnormPack32 &&
		    capture_format != vk::Format::eR8G8B8A8Unorm &&
		    capture_format != vk::Format::eB8G8R8A8Unorm &&
		    capture_format != vk::Format::eR8G8B8A8Srgb &&
		    capture_format != vk::Format::eB8G8R8A8Srgb) {
			return;
		}
		const uint64_t size = uint64_t {capture_extent.width} * capture_extent.height * 4u;
		Buffer download(window.graphic_ctx, present_scheduler, MemoryUsage::Download, 0,
		                vk::BufferUsageFlagBits::eTransferDst, size);
		auto& command = present_scheduler.BeginCommand();
		vk::BufferImageCopy copy {};
		copy.bufferRowLength = capture_extent.width;
		copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		copy.imageExtent = capture_extent;
		command.Handle().copyImageToBuffer(capture_handle, vk::ImageLayout::eTransferSrcOptimal,
		                                   download.Handle(), 1, &copy);
		vk::BufferMemoryBarrier2 barrier {};
		barrier.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
		barrier.srcAccessMask       = vk::AccessFlagBits2::eTransferWrite;
		barrier.dstStageMask        = vk::PipelineStageFlagBits2::eHost;
		barrier.dstAccessMask       = vk::AccessFlagBits2::eHostRead;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer              = download.Handle();
		barrier.offset              = 0;
		barrier.size                = size;
		vk::DependencyInfo dependency {};
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &barrier;
		command.Handle().pipelineBarrier2(dependency);
		const auto tick = present_scheduler.Submit();
		present_scheduler.Wait(tick);
		download.Invalidate(0, size);

		std::array<uint32_t, 4> minimum {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
		std::array<uint32_t, 4> maximum {0, 0, 0, 0};
		std::array<uint64_t, 4> nonzero {};
		uint64_t colored_pixels = 0;
		const auto bytes = download.Mapped();
		for (uint64_t offset = 0; offset < size; offset += 4) {
			std::array<uint32_t, 4> values {};
			if (capture_format == vk::Format::eA2B10G10R10UnormPack32) {
				uint32_t packed = 0;
				std::memcpy(&packed, bytes.data() + static_cast<size_t>(offset), sizeof(packed));
				values = {packed & 0x3ffu, (packed >> 10u) & 0x3ffu,
				          (packed >> 20u) & 0x3ffu, packed >> 30u};
			} else {
				for (uint32_t channel = 0; channel < 4; channel++) {
					values[channel] = bytes[static_cast<size_t>(offset + channel)];
				}
			}
			bool colored = false;
			for (uint32_t channel = 0; channel < 4; channel++) {
				const auto value = values[channel];
				minimum[channel] = std::min(minimum[channel], value);
				maximum[channel] = std::max(maximum[channel], value);
				nonzero[channel] += value != 0 ? 1u : 0u;
				if (channel < 3 && value != 0) colored = true;
			}
			colored_pixels += colored ? 1u : 0u;
		}
		if (auto* output = std::fopen(path, "ab"); output != nullptr) {
			std::fprintf(output,
			             "frame=%" PRIu64 " width=%u height=%u format=%d colored=%" PRIu64
			             " nonzero=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
			             " min=%u,%u,%u,%u max=%u,%u,%u,%u\n",
			             frame_index, capture_extent.width, capture_extent.height,
			             static_cast<int>(capture_format), colored_pixels, nonzero[0], nonzero[1],
			             nonzero[2], nonzero[3], minimum[0], minimum[1], minimum[2], minimum[3],
			             maximum[0], maximum[1], maximum[2], maximum[3]);
			std::fclose(output);
		}
		present_readback_count++;
	}

	Image& ResolveSurface(const ImageInfo& info) {
		TextureCache::ImageDesc desc {};
		desc.info                  = info;
		desc.view_info.format      = info.pixel_format;
		desc.view_info.type        = vk::ImageViewType::e2D;
		desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		desc.view_info.base_level  = 0;
		desc.view_info.level_count = 1;
		desc.view_info.base_layer  = 0;
		desc.view_info.layer_count = 1;
		desc.view_info.usage       = vk::ImageUsageFlagBits::eTransferSrc;
		desc.type                  = TextureCache::BindingType::VideoOut;

		auto&      cache      = renderer.GetTextureCache();
		const auto image_id   = cache.FindImage(desc);
		auto&      image      = cache.GetImage(image_id);
		image.usage.video_out = true;
		cache.UpdateImage(image_id);
		const char* readback_path = std::getenv("KYTY_PRESENT_READBACK_PATH");
		if (readback_path != nullptr && *readback_path != '\0' && present_source_trace_count < 8) {
			std::printf(
			    "PresentSource: addr=0x%016" PRIx64 " size=0x%016" PRIx64
			    " metadata=0x%016" PRIx64 " compression=%u usage=t%d/s%d/r%d/v%d "
			    "dirty=g%d/b%d/c%d backing=%d %ux%u\n",
			    image.info.data.address, image.info.data.size, image.info.metadata.range.address,
			    static_cast<uint32_t>(image.info.metadata.compression), image.usage.texture ? 1 : 0,
			    image.usage.storage ? 1 : 0, image.usage.render_target ? 1 : 0,
			    image.usage.video_out ? 1 : 0, image.IsGpuModified() ? 1 : 0,
			    image.IsBufferModified() ? 1 : 0, image.IsCpuDirty() ? 1 : 0,
			    static_cast<int>(image.backing.format), image.backing.extent.width,
			    image.backing.extent.height);
			std::fflush(stdout);
			present_source_trace_count++;
		}
		return image;
	}

	RenderContext&        renderer;
	WindowContext&        window;
	Swapchain             swapchain;
	CommandScheduler      present_scheduler;
	FramePool             frames;
	std::atomic<uint64_t> presented_ime_revision {0};
	uint64_t              present_readback_frame = 0;
	uint64_t              present_readback_count = 0;
	uint32_t              present_source_trace_count = 0;
};

void Swapchain::Create() {
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	EXIT_IF(m_window.surface == nullptr);

	Common::LockGuard lock(m_window.mutex);
	EXIT_IF(graphics.screen_width == 0);
	EXIT_IF(graphics.screen_height == 0);
	const auto&       surface = m_window.surface_capabilities;
	EXIT_NOT_IMPLEMENTED(surface.formats.empty());

	m_extent = surface.capabilities.currentExtent;
	if (m_extent.width == std::numeric_limits<uint32_t>::max()) {
		m_extent.width =
		    std::clamp(graphics.screen_width, surface.capabilities.minImageExtent.width,
		               surface.capabilities.maxImageExtent.width);
		m_extent.height =
		    std::clamp(graphics.screen_height, surface.capabilities.minImageExtent.height,
		               surface.capabilities.maxImageExtent.height);
	}
	uint32_t image_count = surface.capabilities.minImageCount + 1;
	if (surface.capabilities.maxImageCount != 0) {
		image_count = std::min(image_count, surface.capabilities.maxImageCount);
	}
	const auto transform =
	    surface.capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
	        ? vk::SurfaceTransformFlagBitsKHR::eIdentity
	        : surface.capabilities.currentTransform;
	const auto composite =
	    surface.capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
	        ? vk::CompositeAlphaFlagBitsKHR::eOpaque
	        : vk::CompositeAlphaFlagBitsKHR::eInherit;

	vk::SurfaceFormatKHR format {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
	if (surface.formats.size() != 1 || surface.formats.front().format != vk::Format::eUndefined) {
		const auto it = std::find_if(surface.formats.begin(), surface.formats.end(),
		                             [](const vk::SurfaceFormatKHR& candidate) {
			                             return candidate.format == vk::Format::eB8G8R8A8Unorm ||
			                                    candidate.format == vk::Format::eR8G8B8A8Unorm;
		                             });
		if (it == surface.formats.end()) {
			EXIT("no supported UNORM swapchain format\n");
		}
		format = *it;
	}
	m_format                      = format.format;
	const auto swapchain_features = graphics.GetFormatProperties(m_format).optimalTilingFeatures;
	if (!static_cast<bool>(swapchain_features & vk::FormatFeatureFlagBits::eBlitDst)) {
		EXIT("swapchain format cannot be a blit destination: format=%d\n",
		     static_cast<int>(m_format));
	}

	vk::SwapchainCreateInfoKHR create_info {};
	create_info.sType            = vk::StructureType::eSwapchainCreateInfoKHR;
	create_info.surface          = m_window.surface;
	create_info.minImageCount    = image_count;
	create_info.imageFormat      = format.format;
	create_info.imageColorSpace  = format.colorSpace;
	create_info.imageExtent      = m_extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	create_info.imageSharingMode = vk::SharingMode::eExclusive;
	create_info.preTransform     = transform;
	create_info.compositeAlpha   = composite;
	switch (Config::GetPresentMode()) {
		case Config::PresentMode::Mailbox:
			create_info.presentMode = vk::PresentModeKHR::eMailbox;
			break;
		case Config::PresentMode::Immediate:
			create_info.presentMode = vk::PresentModeKHR::eImmediate;
			break;
		case Config::PresentMode::Fifo:
		default: create_info.presentMode = vk::PresentModeKHR::eFifo; break;
	}
	if (std::find(surface.present_modes.begin(), surface.present_modes.end(),
	              create_info.presentMode) == surface.present_modes.end()) {
		LOGF("warning: requested present mode is unavailable; falling back to Fifo\n");
		create_info.presentMode = vk::PresentModeKHR::eFifo;
	}
	create_info.clipped          = VK_TRUE;
	RequireVulkanSuccess(graphics.device.createSwapchainKHR(&create_info, nullptr, &m_handle),
	                     "vkCreateSwapchainKHR");
	EXIT_IF(m_handle == nullptr);

	m_images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* images) {
		    return graphics.device.getSwapchainImagesKHR(m_handle, count, images);
	    });
	EXIT_NOT_IMPLEMENTED(m_images.empty());

	m_image_views.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		vk::ImageViewCreateInfo view {};
		view.sType                           = vk::StructureType::eImageViewCreateInfo;
		view.image                           = m_images[i];
		view.viewType                        = vk::ImageViewType::e2D;
		view.format                          = m_format;
		view.components                      = {};
		view.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.baseMipLevel   = 0;
		view.subresourceRange.layerCount     = 1;
		view.subresourceRange.levelCount     = 1;
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &m_image_views[i]),
		                     "vkCreateImageView");
		EXIT_IF(m_image_views[i] == nullptr);
	}

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	m_image_acquired.resize(m_images.size());
	m_render_complete.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_image_acquired[i]),
		    "create swapchain image-acquired semaphore");
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_render_complete[i]),
		    "create swapchain render-complete semaphore");
	}
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
}

Swapchain::~Swapchain() {
	Destroy();
}

void Swapchain::Destroy() {
	if (m_handle == nullptr && m_image_acquired.empty() && m_render_complete.empty() &&
	    m_image_views.empty()) {
		return;
	}
	auto& graphics = m_window.graphic_ctx;

	{
		Common::LockGuard queue_lock(graphics.queue_mutex);
		RequireVulkanSuccess(graphics.queue.waitIdle(), "wait for swapchain queue");
	}
	if (m_ime_overlay != nullptr) {
		m_ime_overlay->ReleaseVulkan();
	}

	for (const auto semaphore: m_image_acquired) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto semaphore: m_render_complete) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto view: m_image_views) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
		}
	}
	if (m_handle != nullptr) {
		graphics.device.destroySwapchainKHR(m_handle, nullptr);
	}

	m_handle      = nullptr;
	m_format      = vk::Format::eUndefined;
	m_extent      = {};
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
	m_images.clear();
	m_image_views.clear();
	m_image_acquired.clear();
	m_render_complete.clear();
}

void Swapchain::Recreate(bool surface_lost) {
	Destroy();
	if (surface_lost) {
#if defined(__APPLE__)
		// Surface recreation goes through SDL_Vulkan_CreateSurface, which touches the
		// window's view/layer and must run on the main thread on macOS.
		m_window.RunOnMainThread([this] { m_window.RecreateSurface(); });
#else
		m_window.RecreateSurface();
#endif
	}
	m_window.RefreshSurfaceCapabilities();
	Create();
}

Swapchain::Status Swapchain::AcquireNextImage() {
	EXIT_IF(m_handle == nullptr || m_frame_index >= m_image_acquired.size());
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", VulkanToString(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
}

bool Swapchain::PrepareImeOverlay() {
	if (m_ime_overlay == nullptr) {
		m_ime_overlay = std::make_unique<ImeOverlay>(m_window.graphic_ctx);
	}
	return m_ime_overlay->PrepareFrame(m_extent, m_format, ImageCount());
}

void Swapchain::RecordPresentCommands(CommandBuffer& command, VulkanImage& source,
                                      bool draw_ime_overlay) {
	if (source.state.layout != vk::ImageLayout::eTransferSrcOptimal) {
		EXIT("invalid prepared presentation image, vk_image=%p layout=%d\n",
		     static_cast<void*>(source.image), static_cast<int>(source.state.layout));
	}
	EXIT_IF(m_image_index >= m_images.size());
	auto vk_command = command.Handle();

	vk::ImageMemoryBarrier to_transfer {};
	to_transfer.sType                           = vk::StructureType::eImageMemoryBarrier;
	to_transfer.dstAccessMask                   = vk::AccessFlagBits::eTransferWrite;
	to_transfer.oldLayout                       = vk::ImageLayout::eUndefined;
	to_transfer.newLayout                       = vk::ImageLayout::eTransferDstOptimal;
	to_transfer.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.image                           = m_images[m_image_index];
	to_transfer.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_transfer.subresourceRange.baseMipLevel   = 0;
	to_transfer.subresourceRange.levelCount     = 1;
	to_transfer.subresourceRange.baseArrayLayer = 0;
	to_transfer.subresourceRange.layerCount     = 1;
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
	                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 0,
	                           nullptr, 0, nullptr, 1, &to_transfer);

	vk::ImageBlit region {};
	region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
	region.srcSubresource.mipLevel       = 0;
	region.srcSubresource.baseArrayLayer = 0;
	region.srcSubresource.layerCount     = 1;
	region.srcOffsets[1].x               = static_cast<int>(source.extent.width);
	region.srcOffsets[1].y               = static_cast<int>(source.extent.height);
	region.srcOffsets[1].z               = 1;
	region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
	region.dstSubresource.mipLevel       = 0;
	region.dstSubresource.baseArrayLayer = 0;
	region.dstSubresource.layerCount     = 1;
	region.dstOffsets[1].x               = static_cast<int>(m_extent.width);
	region.dstOffsets[1].y               = static_cast<int>(m_extent.height);
	region.dstOffsets[1].z               = 1;
	vk_command.blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal,
	                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1, &region,
	                     vk::Filter::eLinear);

	vk::ImageMemoryBarrier to_present {};
	to_present.sType         = vk::StructureType::eImageMemoryBarrier;
	to_present.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	to_present.dstAccessMask = draw_ime_overlay ? vk::AccessFlagBits::eColorAttachmentRead |
	                                                  vk::AccessFlagBits::eColorAttachmentWrite
	                                            : vk::AccessFlagBits::eMemoryRead;
	to_present.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
	to_present.newLayout     = draw_ime_overlay ? vk::ImageLayout::eColorAttachmentOptimal
	                                            : vk::ImageLayout::ePresentSrcKHR;
	to_present.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_present.image                           = m_images[m_image_index];
	to_present.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_present.subresourceRange.baseMipLevel   = 0;
	to_present.subresourceRange.levelCount     = 1;
	to_present.subresourceRange.baseArrayLayer = 0;
	to_present.subresourceRange.layerCount     = 1;
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           draw_ime_overlay ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                            : vk::PipelineStageFlagBits::eAllCommands,
	                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
	                           &to_present);
	if (draw_ime_overlay) {
		m_ime_overlay->Record(vk_command, m_image_views[m_image_index]);
		to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
		to_present.newLayout     = vk::ImageLayout::ePresentSrcKHR;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
		                           &to_present);
	}
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	EXIT_IF(m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	return scheduler.Submit(submit);
}

Swapchain::Status Swapchain::Present() {
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", VulkanToString(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	return Status::Success;
}

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() = default;

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	auto*             frame = m_impl->frames.Acquire();
	frame->guest_surface = true;
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	auto&             image = m_impl->ResolveSurface(info);
	frame->guest_source = &image;
	if (image.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}

	auto frame_format = info.pixel_format;
	switch (frame_format) {
		case vk::Format::eR8G8B8A8Srgb: frame_format = vk::Format::eR8G8B8A8Unorm; break;
		case vk::Format::eB8G8R8A8Srgb: frame_format = vk::Format::eB8G8R8A8Unorm; break;
		default: break;
	}
	frame->Configure(m_impl->window.graphic_ctx,
	                 {image.backing.extent.width, image.backing.extent.height}, frame_format);
	frame->CopyFrom(buffer, image);
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
                                               CommandBuffer* producer) {
	KYTY_PROFILER_FUNCTION();
	auto              format = m_impl->frames.GetFormat();
	auto*             frame  = m_impl->frames.Acquire();
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	frame->Configure(m_impl->window.graphic_ctx, {width, height}, format);
	vk::ClearColorValue clear {};
	clear.float32[3] = opaque ? 1.0f : 0.0f;
	if (producer != nullptr) {
		EXIT_IF(producer->IsInvalid());
		frame->Clear(*producer, clear);
	} else {
		auto& command = m_impl->present_scheduler.BeginCommand();
		frame->Clear(command, clear);
		frame->present_tick = m_impl->present_scheduler.Submit();
	}
	return *frame;
}

Presenter::Frame* Presenter::PrepareLastFrame() {
	return m_impl->frames.AcquireLast();
}

bool Presenter::IsGuestPaused() const noexcept {
	return m_impl->window.loop.paused.load(std::memory_order_acquire);
}

bool Presenter::NeedsImeRefresh() const noexcept {
	const auto visual = GetImeVisualState();
	return visual.active ||
	       visual.revision != m_impl->presented_ime_revision.load(std::memory_order_acquire);
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

void Presenter::Present(Frame& frame, bool reuse) {
	KYTY_PROFILER_FUNCTION();
	m_impl->frames.ValidateForPresent(&frame, reuse);

	const auto ime_visual = GetImeVisualState();
	{
		Common::LockGuard render_lock(m_impl->renderer.GetMutex());
		m_impl->CapturePreparedFrame(frame);
	}
	auto& swapchain = m_impl->swapchain;
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		auto status = swapchain.AcquireNextImage();
		if (status != Swapchain::Status::Success) {
			m_impl->RecoverSwapchain(status);
			continue;
		}
		{
			Common::LockGuard render_lock(m_impl->renderer.GetMutex());
			auto&             command          = m_impl->present_scheduler.BeginCommand();
			const bool        draw_ime_overlay = ime_visual.active && swapchain.PrepareImeOverlay();
			swapchain.RecordPresentCommands(command, frame.image, draw_ime_overlay);
			frame.present_tick = swapchain.Submit(m_impl->present_scheduler);
		}
		status = swapchain.Present();
		if (status != Swapchain::Status::Success) {
			m_impl->RecoverSwapchain(status);
			continue;
		}

		m_impl->presented_ime_revision.store(ime_visual.revision, std::memory_order_release);
		m_impl->window.UpdateTitle();
		m_impl->frames.Release(&frame, true);
		return;
	}
	LOGF("Vulkan presentation retry exhausted; dropping frame\n");
	m_impl->frames.Release(&frame, reuse);
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

WindowContext::WindowContext() = default;

} // namespace Libs::Graphics
