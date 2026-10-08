#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_video_out_overlay_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/dlss.h"
#include "graphics/presentation/emulatorDlssInputs.h"
#include "graphics/presentation/frameTiming.h"
#include "graphics/presentation/preparedFrameSelection.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/presentationFrame.h"
#include "graphics/presentation/window/windowInternal.h"
#include "graphics/presentation/xessFrameGeneration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

namespace Libs::Graphics {

namespace {
vk::Format EncodedColorFormat(vk::Format format) {
	switch (format) {
		case vk::Format::eR8G8B8A8Srgb: return vk::Format::eR8G8B8A8Unorm;
		case vk::Format::eB8G8R8A8Srgb: return vk::Format::eB8G8R8A8Unorm;
		default: return format;
	}
}
} // namespace

class FramePool final {
public:
	FramePool(WindowContext& window, CommandScheduler& scheduler)
	    : m_window(window), m_scheduler(scheduler) {}
	~FramePool() {
		m_scheduler.Wait(m_scheduler.CurrentTick() - 1);
		WaitFrameGenerationInputs();
		for (auto& frame: m_frames) {
			m_window.graphic_ctx.device.destroyImageView(frame->view, nullptr);
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
		m_free.reserve(count);
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

	Presenter::Frame* Acquire(vk::Extent2D extent, vk::Format format, bool storage = false) {
		m_mutex.Lock();
		if (m_frames.empty()) {
			EXIT("prepared-frame pool was used before swapchain initialization\n");
		}
		// A synchronized flip may need more frames than the swapchain has images.
		// Queue capacity bounds growth; waiting here can prevent its master from recording.
		if (m_free.empty()) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
		auto& master = m_scheduler.GetMasterSemaphore();
		if (m_free.size() > 1) master.Refresh();
		const auto index =
		    m_free.size() == 1
		        ? 0
		        : SelectPreparedFrame(
		              std::span<Presenter::Frame* const> {m_free},
		              [&](const auto& frame) {
			              return frame.image.extent.width == extent.width &&
			                     frame.image.extent.height == extent.height &&
			                     frame.image.format == format &&
			                     (!storage || static_cast<bool>(frame.image.usage &
			                                                    vk::ImageUsageFlagBits::eStorage));
		              },
		              [&](const auto& frame) {
			              return master.IsFree(frame.present_tick) &&
			                     (!frame.fg_inputs || frame.fg_inputs->Ready(m_window.graphic_ctx));
		              });
		auto* frame   = m_free[index];
		m_free[index] = m_free.back();
		m_free.pop_back();
		if (frame->busy) {
			EXIT("prepared-frame pool returned an invalid frame\n");
		}
		frame->busy = true;
		m_mutex.Unlock();

		m_scheduler.Wait(frame->present_tick);
		if (frame->fg_inputs) frame->fg_inputs->Wait(m_window.graphic_ctx);
		return frame;
	}

	void WaitFrameGenerationInputs() {
		Common::LockGuard render_lock(m_window.render_context->GetMutex());
		Common::LockGuard lock(m_mutex);
		for (auto& frame: m_frames) {
			if (frame->fg_inputs) frame->fg_inputs->Wait(m_window.graphic_ctx);
		}
	}

	void ValidateForPresent(Presenter::Frame* frame) {
		Common::LockGuard lock(m_mutex);
		if (frame == nullptr || !frame->busy) {
			EXIT("prepared frame has invalid presentation ownership\n");
		}
	}

	void Release(Presenter::Frame* frame) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy = false;
		m_free.push_back(frame);
	}

private:
	WindowContext&                                 m_window;
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::vector<Presenter::Frame*>                 m_free;
	vk::Format                                     m_format = vk::Format::eUndefined;
};

void Presenter::Frame::Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format,
                                 bool storage) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}
	auto&      dst        = image;
	const bool compatible =
	    dst.image != nullptr && dst.extent.width == extent.width &&
	    dst.extent.height == extent.height && dst.format == format &&
	    (!storage || static_cast<bool>(dst.usage & vk::ImageUsageFlagBits::eStorage));
	if (compatible) {
		return;
	}

	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	const auto required =
	    vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	    vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferSrc |
	    vk::FormatFeatureFlagBits::eTransferDst;
	if ((features & required) != required) {
		EXIT("prepared presentation format lacks optimal blit support: format=%d features=0x%x\n",
		     static_cast<int>(format), static_cast<vk::FormatFeatureFlags::MaskType>(features));
	}

	if (dst.image != nullptr) {
		graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
		graphics.DeleteImage(dst);
	}

	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
	               vk::ImageUsageFlagBits::eSampled;
	if (storage) create.usage |= vk::ImageUsageFlagBits::eStorage;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, dst)) {
		EXIT("failed to allocate prepared presentation image, extent=%ux%u format=%d\n",
		     extent.width, extent.height, static_cast<int>(format));
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
	// Copy sRGB/UNORM pairs without decoding the guest's encoded color.
	if (EncodedColorFormat(source.backing.format) != EncodedColorFormat(image.format) ||
	    source.backing.extent != image.extent) {
		vk::ImageBlit blit {};
		blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		blit.srcOffsets[1]  = {static_cast<int32_t>(source.backing.extent.width),
		                       static_cast<int32_t>(source.backing.extent.height), 1};
		blit.dstOffsets[1]  = {static_cast<int32_t>(image.extent.width),
		                       static_cast<int32_t>(image.extent.height), 1};
		command.blitImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
		                  vk::ImageLayout::eTransferDstOptimal, 1, &blit, vk::Filter::eLinear);
		return;
	}
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, source.backing.layers};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, image.layers};
	copy.extent         = {std::min(source.backing.extent.width, image.extent.width),
	                       std::min(source.backing.extent.height, image.extent.height), 1};
	EXIT_IF(copy.srcSubresource.layerCount != copy.dstSubresource.layerCount);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
}

void Presenter::Frame::Clear(CommandBuffer& command_buffer, const vk::ClearColorValue& color) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
}

class Swapchain final {
public:
	enum class Status : uint8_t { Success, Recreate, SurfaceLost };

	explicit Swapchain(WindowContext& window): m_window(window) {}
	~Swapchain();
	KYTY_CLASS_NO_COPY(Swapchain);

	void                 Create();
	void                 Recreate(bool surface_lost = false);
	[[nodiscard]] bool   NeedsResize() const;
	[[nodiscard]] Status AcquireNextImage();
	[[nodiscard]] bool   PrepareSystemOverlay();
	void                 RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
	                                           const Presenter::Layer& overlay, bool draw_system_overlay,
	                                           Image* interpolated = nullptr);
	uint64_t             Submit(CommandScheduler& scheduler, uint64_t producer_tick);
	[[nodiscard]] Status Present();

	[[nodiscard]] uint32_t ImageCount() const noexcept {
		return static_cast<uint32_t>(m_images.size());
	}
	[[nodiscard]] vk::Format Format() const noexcept { return m_format; }
	[[nodiscard]] vk::Extent2D Extent() const noexcept { return m_extent; }
	[[nodiscard]] vk::Image    CurrentImage() const { return m_images[m_image_index]; }

private:
	void Destroy();
	void CreateViews();
	void DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer);

	WindowContext&   m_window;
	vk::SwapchainKHR m_handle = nullptr;
	// XeSS Frame Generation: images shared with its D3D12 swap chain replace the Vulkan one.
	XessFgBridge*    m_bridge = nullptr;
	vk::Format       m_format = vk::Format::eUndefined;
	vk::Extent2D     m_extent {};
	// Drawable pixel size observed when this swapchain was created.
	vk::Extent2D                   m_window_extent {};
	std::vector<vk::Image>         m_images;
	std::vector<vk::ImageView>     m_image_views;
	std::vector<vk::Semaphore>     m_image_acquired;
	std::vector<vk::Semaphore>     m_render_complete;
	std::unique_ptr<SystemOverlay> m_system_overlay;
	vk::DescriptorSetLayout        m_overlay_descriptors = nullptr;
	vk::PipelineLayout             m_overlay_layout      = nullptr;
	vk::Pipeline                   m_overlay_pipeline    = nullptr;
	vk::Sampler                    m_overlay_sampler     = nullptr;
	uint32_t                       m_image_index         = static_cast<uint32_t>(-1);
	uint32_t                       m_frame_index         = 0;
};

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner),
	      present_scheduler(renderer, owner.graphic_ctx, true), frames(owner, present_scheduler),
	      dlss(owner.graphic_ctx, renderer.GetCommandScheduler()),
	      emulator_inputs(owner.graphic_ctx, renderer.GetCommandScheduler()) {
		EXIT_IF(owner.render_context == nullptr);
		swapchain.Create();
		frames.Initialize(swapchain.ImageCount(), swapchain.Format());
	}

	~Impl() {
		// Prepared DLSS outputs can still be referenced by the producer stream
		// even if they were never presented. Drain it before FramePool destruction.
		Common::LockGuard render_lock(renderer.GetMutex());
		auto&             scheduler = renderer.GetCommandScheduler();
		if (scheduler.Active()) scheduler.Finish();
		if (window.frame_generation && window.frame_generation->Hooked()) {
			// Normal teardown has stopped producers. Retire SDK work before
			// destroying Vulkan resources and the standalone NGX backend.
			present_scheduler.Wait(present_scheduler.CurrentTick() - 1);
			frames.WaitFrameGenerationInputs();
			window.frame_generation->SetEnabled(false);
			RequireVulkanSuccess(window.graphic_ctx.device.waitIdle(),
			                     "drain FG before NGX shutdown");
		}
	}

	void RecoverSwapchain(Swapchain::Status status) {
		frames.WaitFrameGenerationInputs();
		LOGF("Recovering Vulkan swapchain%s\n",
		     status == Swapchain::Status::SurfaceLost ? " and surface" : "");
		swapchain.Recreate(status == Swapchain::Status::SurfaceLost);
		frames.SetFormat(swapchain.Format());
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
		return image;
	}
	// Scale relative to output; 100% preserves native guest rasterization.
	bool UpdateRasterScale(vk::Extent2D output, vk::Extent2D display) {
		const auto percent = Config::GetRenderScalePercent();
		const auto key =
		    std::tuple {output.width, output.height, display.width, display.height, percent};
		if (key != raster_scale_key) {
			raster_scale_key = key;
			raster_scale     = {1.f, 1.f};
			if (percent < 100 && display.width && display.height) {
				raster_scale = {std::min(1.f, float(std::max(1u, output.width * percent / 100)) /
				                                  float(display.width)),
				                std::min(1.f, float(std::max(1u, output.height * percent / 100)) /
				                                  float(display.height))};
			}
			renderer.SetRasterScale(raster_scale.first, raster_scale.second);
		}
		return raster_scale.first < 1.f || raster_scale.second < 1.f;
	}
	void Present(bool new_frame = true);
	void PresentLayers(bool new_frame, bool generate_frame, uint64_t timing_begin,
	                   const SystemOverlayVisualState& overlay_visual);
	void PresentDeferred() {
		if (deferred_deadline.exchange(0) == 0) return;
		const auto begin =
		    timing.Enabled() ? Common::Timer::QueryPerformanceCounter() - deferred_work : 0;
		PresentLayers(true, true, begin, GetSystemOverlayVisualState());
	}

	RenderContext&        renderer;
	WindowContext&        window;
	Swapchain             swapchain;
	CommandScheduler      present_scheduler;
	FramePool             frames;
	Common::Mutex         present_mutex;
	std::array<Layer, 2>  layers {};
	std::atomic<uint64_t> presented_overlay_revision {0};
	DlssProcessor                                                dlss;
	EmulatorDlssInputs                                           emulator_inputs;
	vk::Extent2D                                                 dlss_target_size {};
	Config::DlssMode                                             dlss_mode = Config::DlssMode::Off;
	vk::Extent2D                                                 dlss_source_size {};
	uint32_t                                                     dlss_render_scale   = 100;
	bool                                                         dlss_reduced_source = false;
	bool                                                         dlss_bypassed       = false;
	std::optional<vk::Extent2D>                                  dlss_input_size;
	bool                                                         emulator_dlss_logged = false;
	std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> raster_scale_key {};
	std::pair<float, float>                                      raster_scale {1.f, 1.f};
	FrameTimingRecorder                                          timing;
	// External FG shows the real frame at this QPC time, after the guest flip has
	// completed with the interpolated frame. Zero when nothing is deferred.
	std::atomic<uint64_t> deferred_deadline {0};
	uint64_t              deferred_work = 0; // Interpolation CPU time, for timing only.
};

void Swapchain::Create() {
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	EXIT_IF(m_window.surface == nullptr);

	// Capture the size before querying surface capabilities. If the window changes
	// during creation, the next presentation will detect the newer size.
	{
		Common::LockGuard lock(m_window.mutex);
		m_window_extent = {graphics.screen_width, graphics.screen_height};
	}
	EXIT_IF(m_window_extent.width == 0);
	EXIT_IF(m_window_extent.height == 0);
	m_window.RefreshSurfaceCapabilities();
	const auto& surface = m_window.surface_capabilities;
	EXIT_NOT_IMPLEMENTED(surface.formats.empty());

	m_extent = surface.capabilities.currentExtent;
	if (m_extent.width == std::numeric_limits<uint32_t>::max()) {
		m_extent.width =
		    std::clamp(m_window_extent.width, surface.capabilities.minImageExtent.width,
		               surface.capabilities.maxImageExtent.width);
		m_extent.height =
		    std::clamp(m_window_extent.height, surface.capabilities.minImageExtent.height,
		               surface.capabilities.maxImageExtent.height);
	}
	if (auto* fg = m_window.frame_generation.get(); fg && fg->Bridge()) {
		if (fg->Bridge()->CreateSwapchain(fg->NativeWindow(), m_extent,
		                                  Config::GetPresentMode() == Config::PresentMode::Fifo)) {
			m_bridge = fg->Bridge();
			m_format = m_bridge->Format();
			m_images = m_bridge->Images();
			CreateViews();
			m_image_index = static_cast<uint32_t>(-1);
			m_frame_index = 0;
			return;
		}
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
	// Vulkan DLSS-G does not support VSync. The backend paces generated frames.
	if (m_window.frame_generation && m_window.frame_generation->Enabled()) {
		if (std::ranges::find(surface.present_modes, vk::PresentModeKHR::eImmediate) !=
		    surface.present_modes.end()) {
			create_info.presentMode = vk::PresentModeKHR::eImmediate;
		}
	}
	create_info.clipped = VK_TRUE;
	RequireVulkanSuccess(graphics.device.createSwapchainKHR(&create_info, nullptr, &m_handle),
	                     "vkCreateSwapchainKHR");
	EXIT_IF(m_handle == nullptr);

	m_images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* images) {
		    return graphics.device.getSwapchainImagesKHR(m_handle, count, images);
	    });
	EXIT_NOT_IMPLEMENTED(m_images.empty());
	CreateViews();

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

void Swapchain::CreateViews() {
	auto& graphics = m_window.graphic_ctx;
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
		Common::LockGuard present_lock(graphics.present_queue_mutex);
		if (m_window.frame_generation && m_window.frame_generation->Hooked()) {
			// Streamline presents on its own queues and pacer thread. Its device
			// idle proxy flushes those pending jobs as well as the host queue.
			RequireVulkanSuccess(graphics.device.waitIdle(),
			                     "wait for Frame Generation presentation");
		} else {
			RequireVulkanSuccess(
			    (graphics.present_queue ? graphics.present_queue : graphics.queue).waitIdle(),
			    "wait for swapchain queue");
		}
	}
	if (m_system_overlay != nullptr) {
		m_system_overlay->ReleaseVulkan();
	}
	graphics.device.destroyPipeline(m_overlay_pipeline, nullptr);
	graphics.device.destroyPipelineLayout(m_overlay_layout, nullptr);
	graphics.device.destroyDescriptorSetLayout(m_overlay_descriptors, nullptr);
	graphics.device.destroySampler(m_overlay_sampler, nullptr);
	m_overlay_pipeline    = nullptr;
	m_overlay_layout      = nullptr;
	m_overlay_descriptors = nullptr;
	m_overlay_sampler     = nullptr;

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
	if (m_bridge != nullptr) {
		m_bridge->DestroySwapchain();
		m_bridge = nullptr;
	}

	m_handle        = nullptr;
	m_format        = vk::Format::eUndefined;
	m_extent        = {};
	m_window_extent = {};
	m_image_index   = static_cast<uint32_t>(-1);
	m_frame_index   = 0;
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
		EXIT_IF(!SDL_RunOnMainThread(
		    [](void* window) { static_cast<WindowContext*>(window)->RecreateSurface(); }, &m_window,
		    true));
#else
		m_window.RecreateSurface();
#endif
	}
	Create();
}

bool Swapchain::NeedsResize() const {
	Common::LockGuard lock(m_window.mutex);
	return m_window_extent.width != m_window.graphic_ctx.screen_width ||
	       m_window_extent.height != m_window.graphic_ctx.screen_height;
}

Swapchain::Status Swapchain::AcquireNextImage() {
	if (m_bridge != nullptr) {
		m_image_index = m_bridge->Acquire();
		return Status::Success;
	}
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
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
}

bool Swapchain::PrepareSystemOverlay() {
	if (m_system_overlay == nullptr) {
		m_system_overlay = std::make_unique<SystemOverlay>(m_window.graphic_ctx);
	}
	return m_system_overlay->PrepareFrame(m_extent, m_format, ImageCount());
}

void Swapchain::DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer) {
	auto device = m_window.graphic_ctx.device;
	if (m_overlay_pipeline == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_overlay_sampler),
		                     "create video-out overlay sampler");
		const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler,
		                                              1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo    descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = 1;
		descriptors.pBindings    = &binding;
		RequireVulkanSuccess(
		    device.createDescriptorSetLayout(&descriptors, nullptr, &m_overlay_descriptors),
		    "create video-out overlay descriptor layout");
		const vk::PushConstantRange alpha {vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_overlay_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &alpha;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_overlay_layout),
		                     "create video-out overlay pipeline layout");
		const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
		const auto fragment = CompileSPV(GPU_VIDEO_OUT_OVERLAY_SPV, device);
		std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
		stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
		stages[0].module = vertex;
		stages[0].pName  = "main";
		stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
		stages[1].module = fragment;
		stages[1].pName  = "main";
		vk::PipelineVertexInputStateCreateInfo   vertex_input {};
		vk::PipelineInputAssemblyStateCreateInfo assembly {};
		assembly.topology = vk::PrimitiveTopology::eTriangleList;
		vk::PipelineViewportStateCreateInfo viewport {};
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;
		vk::PipelineRasterizationStateCreateInfo rasterization {};
		rasterization.lineWidth = 1.0f;
		vk::PipelineMultisampleStateCreateInfo multisample {};
		multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
		vk::PipelineColorBlendAttachmentState attachment {};
		attachment.blendEnable         = VK_TRUE;
		attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
		attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.colorWriteMask = vk::ColorComponentFlagBits::eR |
		                            vk::ColorComponentFlagBits::eG |
		                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
		vk::PipelineColorBlendStateCreateInfo blend {};
		blend.attachmentCount = 1;
		blend.pAttachments    = &attachment;
		const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic {};
		dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
		dynamic.pDynamicStates    = dynamic_states.data();
		vk::PipelineRenderingCreateInfo rendering {};
		rendering.colorAttachmentCount    = 1;
		rendering.pColorAttachmentFormats = &m_format;
		vk::GraphicsPipelineCreateInfo create {};
		create.pNext               = &rendering;
		create.stageCount          = static_cast<uint32_t>(stages.size());
		create.pStages             = stages.data();
		create.pVertexInputState   = &vertex_input;
		create.pInputAssemblyState = &assembly;
		create.pViewportState      = &viewport;
		create.pRasterizationState = &rasterization;
		create.pMultisampleState   = &multisample;
		create.pColorBlendState    = &blend;
		create.pDynamicState       = &dynamic;
		create.layout              = m_overlay_layout;
		RequireVulkanSuccess(
		    device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &m_overlay_pipeline),
		    "create video-out overlay pipeline");
		device.destroyShaderModule(fragment, nullptr);
		device.destroyShaderModule(vertex, nullptr);
	}
	auto& frame = *layer.frame;
	if (frame.view == nullptr) {
		vk::ImageViewCreateInfo view {};
		view.image            = frame.image.image;
		view.viewType         = vk::ImageViewType::e2D;
		view.format           = frame.image.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(device.createImageView(&view, nullptr, &frame.view),
		                     "create video-out overlay image view");
	}
	const vk::DescriptorImageInfo image {m_overlay_sampler, frame.view,
	                                     vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet        write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo      = &image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_overlay_layout, 0, 1, &write);
	const uint32_t premultiplied = layer.premultiplied_alpha;
	command.pushConstants(m_overlay_layout, vk::ShaderStageFlagBits::eFragment, 0,
	                      sizeof(premultiplied), &premultiplied);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_overlay_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = m_image_views[m_image_index];
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = m_extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {
	    0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height),
	    0.0f, 1.0f};
	const vk::Rect2D scissor {{0, 0}, m_extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void Swapchain::RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
                                      const Presenter::Layer& overlay, bool draw_system_overlay,
                                      Image* interpolated) {
	EXIT_IF(m_image_index >= m_images.size());
	auto       vk_command      = command.Handle();
	const bool draw_overlay    = overlay.frame != nullptr;
	// Shared D3D12 images are read through the GENERAL layout, not presented by Vulkan.
	const auto final_layout =
	    m_bridge ? vk::ImageLayout::eGeneral : vk::ImageLayout::ePresentSrcKHR;
	const bool draw_attachment = draw_overlay || draw_system_overlay;
	if (interpolated) {
		interpolated->Transit(vk::ImageLayout::eTransferSrcOptimal,
		                      vk::AccessFlagBits2::eTransferRead, {}, vk_command);
	} else if (source != nullptr) {
		source->Transit(vk_command, vk::ImageLayout::eTransferSrcOptimal,
		                vk::AccessFlagBits2::eTransferRead);
	}
	if (draw_overlay) {
		overlay.frame->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                       vk::AccessFlagBits2::eShaderRead);
	}

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
	// Match the acquire wait stage so the layout transition cannot precede acquisition.
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 0,
	                           nullptr, 0, nullptr, 1, &to_transfer);

	if (source != nullptr || interpolated) {
		const auto&   image = interpolated ? interpolated->backing : source->image;
		vk::ImageBlit region {};
		region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.srcSubresource.mipLevel       = 0;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount     = 1;
		region.srcOffsets[1].x               = static_cast<int>(image.extent.width);
		region.srcOffsets[1].y               = static_cast<int>(image.extent.height);
		region.srcOffsets[1].z               = 1;
		region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.dstSubresource.mipLevel       = 0;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount     = 1;
		region.dstOffsets[1].x               = static_cast<int>(m_extent.width);
		region.dstOffsets[1].y               = static_cast<int>(m_extent.height);
		region.dstOffsets[1].z               = 1;
		vk_command.blitImage(image.image, vk::ImageLayout::eTransferSrcOptimal,
		                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1,
		                     &region, vk::Filter::eLinear);
	} else {
		const vk::ClearColorValue black {};
		vk_command.clearColorImage(m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal,
		                           &black, 1, &to_transfer.subresourceRange);
	}

	vk::ImageMemoryBarrier to_present {};
	to_present.sType               = vk::StructureType::eImageMemoryBarrier;
	to_present.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	to_present.dstAccessMask       = draw_attachment ? vk::AccessFlagBits::eColorAttachmentRead |
	                                                       vk::AccessFlagBits::eColorAttachmentWrite
	                                                 : vk::AccessFlagBits::eMemoryRead;
	to_present.oldLayout           = vk::ImageLayout::eTransferDstOptimal;
	to_present.newLayout =
	    draw_attachment ? vk::ImageLayout::eColorAttachmentOptimal : final_layout;
	to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.image               = m_images[m_image_index];
	to_present.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_present.subresourceRange.baseMipLevel   = 0;
	to_present.subresourceRange.levelCount     = 1;
	to_present.subresourceRange.baseArrayLayer = 0;
	to_present.subresourceRange.layerCount     = 1;
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           draw_attachment ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                           : vk::PipelineStageFlagBits::eAllCommands,
	                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
	                           &to_present);
	if (draw_attachment) {
		if (draw_overlay) {
			DrawOverlay(vk_command, overlay);
			if (draw_system_overlay) {
				to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
				to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
				vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0,
				                           nullptr, 1, &to_present);
			}
		}
		if (draw_system_overlay) {
			m_system_overlay->Record(vk_command, m_image_views[m_image_index]);
		}
		to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
		to_present.newLayout     = final_layout;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
		                           &to_present);
	}
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler, uint64_t producer_tick) {
	if (m_bridge != nullptr) {
		SubmitInfo submit;
		if (producer_tick) {
			auto& producer = m_window.render_context->GetCommandScheduler();
			submit.AddWait(producer.GetMasterSemaphore().Handle(), producer_tick);
		}
		m_bridge->AddSubmitSync(submit, m_image_index);
		return scheduler.Submit(submit);
	}
	EXIT_IF(m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	if (producer_tick) {
		auto& producer = m_window.render_context->GetCommandScheduler();
		submit.AddWait(producer.GetMasterSemaphore().Handle(), producer_tick);
	}
	submit.AddSignal(m_render_complete[m_image_index]);
	return scheduler.Submit(submit);
}

Swapchain::Status Swapchain::Present() {
	if (m_bridge != nullptr) {
		if (!m_bridge->Present(m_image_index)) return Status::Recreate;
		m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
		return Status::Success;
	}
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
		auto&             graphics = m_window.graphic_ctx;
		const auto        queue = graphics.present_queue ? graphics.present_queue : graphics.queue;
		Common::LockGuard lock(queue != graphics.queue ? graphics.present_queue_mutex
		                                               : graphics.queue_mutex);
		result = queue.presentKHR(&present);
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
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	return Status::Success;
}

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() = default;

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info,
                                          const DlssFrameInputs* dlss_inputs, bool process_dlss) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	const auto stamp = [&] {
		return m_impl->timing.Enabled() ? Common::Timer::QueryPerformanceCounter() : uint64_t(0);
	};
	const auto         prepare_begin = stamp();
	const auto         frame_format  = EncodedColorFormat(info.pixel_format);
	const vk::Extent2D output_size {Config::GetScreenWidth(), Config::GetScreenHeight()};
	const vk::Extent2D source_hint {info.extent.width, info.extent.height};
	const bool raster_scaled = process_dlss && m_impl->UpdateRasterScale(output_size, source_hint);
	const bool reconstruct_hint =
	    process_dlss && m_impl->dlss.Available() &&
	    (dlss_inputs || Config::GetDlssMode() == Config::DlssMode::DLAA || raster_scaled ||
	     source_hint.width < output_size.width || source_hint.height < output_size.height);
	// Wait before taking the renderer lock; resolved inputs determine the final format.
	auto* frame = m_impl->frames.Acquire(
	    reconstruct_hint ? output_size : source_hint,
	    reconstruct_hint ? vk::Format::eR16G16B16A16Sfloat : frame_format, reconstruct_hint);
	const auto        acquired = stamp();
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	const auto        locked = stamp();
	auto&             image  = m_impl->ResolveSurface(info);
	if (image.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}
	const auto                     resolved = stamp();
	std::optional<DlssFrameInputs> generated_inputs;
	const bool                     fg_available = Config::DlssFrameGenerationEnabled() &&
	                          m_impl->window.frame_generation &&
	                          m_impl->window.frame_generation->Available() &&
	                          m_impl->window.frame_generation->Foreground();
	Image* temporal_source = &image;
	if (process_dlss) {
		if (auto* reduced = buffer.RasterColorSource(image)) temporal_source = reduced;
	}
	if (process_dlss && (m_impl->dlss.Available() || fg_available)) {
		const vk::Extent2D output_size {Config::GetScreenWidth(), Config::GetScreenHeight()};
		if (dlss_inputs == nullptr) {
			const bool         reduced_source = temporal_source != &image;
			const vk::Extent2D source_size {temporal_source->backing.extent.width,
			                                temporal_source->backing.extent.height};
			if (m_impl->dlss_target_size != output_size ||
			    m_impl->dlss_source_size != source_size ||
			    m_impl->dlss_mode != Config::GetDlssMode() ||
			    m_impl->dlss_render_scale != Config::GetRenderScalePercent() ||
			    m_impl->dlss_reduced_source != reduced_source) {
				// Only a proven reduced raster can bypass recommended input sizing.
				const auto hint         = reduced_source || Config::GetRenderScalePercent() == 100
				                              ? source_size
				                              : vk::Extent2D {};
				m_impl->dlss_input_size = m_impl->dlss.OptimalInputExtent(output_size, hint);
				m_impl->dlss_bypassed = !m_impl->dlss_input_size && m_impl->dlss.Available() &&
				                        Config::GetDlssMode() != Config::DlssMode::DLAA &&
				                        hint.width >= output_size.width &&
				                        hint.height >= output_size.height;
				if (m_impl->dlss_bypassed) {
					Log::WriteToConsoleAndLog(fmt::format(
					    "DLSS Super Resolution not needed: source {}x{} already covers output "
					    "{}x{}; presenting the native image (lower Render scale to reconstruct)\n",
					    source_size.width, source_size.height, output_size.width,
					    output_size.height));
				}
				m_impl->dlss_target_size    = output_size;
				m_impl->dlss_source_size    = source_size;
				m_impl->dlss_render_scale   = Config::GetRenderScalePercent();
				m_impl->dlss_reduced_source = reduced_source;
				m_impl->dlss_mode           = Config::GetDlssMode();
				m_impl->emulator_inputs.Reset();
			}
			if (m_impl->dlss_input_size || fg_available) {
				// FG inputs need not exceed the presented backbuffer.
				const vk::Extent2D fg_size {std::min(source_size.width, output_size.width),
				                            std::min(source_size.height, output_size.height)};
				generated_inputs = m_impl->emulator_inputs.Prepare(
				    buffer, *temporal_source, m_impl->dlss_input_size.value_or(fg_size),
				    m_impl->dlss_input_size.has_value(),
				    m_impl->renderer.GetGeometryMotion().Source(image));
				if (generated_inputs) dlss_inputs = &*generated_inputs;
			}
		} else {
			m_impl->emulator_inputs.Reset();
		}
	}
	// Instrument guest draws only while SR or FG consumes their motion/depth.
	if (process_dlss) {
		m_impl->renderer.SetTemporalInputsNeeded(
		    fg_available || (m_impl->dlss.Available() && m_impl->dlss_input_size.has_value()));
	}
	const auto inputs_ready = stamp();
	auto&      graphics     = m_impl->window.graphic_ctx;
	const auto required =
	    vk::FormatFeatureFlagBits::eStorageImage | vk::FormatFeatureFlagBits::eBlitSrc |
	    vk::FormatFeatureFlagBits::eBlitDst | vk::FormatFeatureFlagBits::eSampledImage |
	    vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
	const auto features =
	    graphics.GetFormatProperties(vk::Format::eR16G16B16A16Sfloat).optimalTilingFeatures;
	const bool reconstruct =
	    process_dlss && dlss_inputs != nullptr && m_impl->dlss.Available() &&
	    (!generated_inputs || m_impl->dlss_input_size.has_value()) &&
	    (features & required) == required && output_size.width > 0 && output_size.height > 0 &&
	    output_size.width <= graphics.physical_device_properties.limits.maxImageDimension2D &&
	    output_size.height <= graphics.physical_device_properties.limits.maxImageDimension2D;
	frame->dlss_evaluated = false;
	frame->dlss_bypassed  = process_dlss && m_impl->dlss_bypassed;
	frame->guest_frame    = true;
	frame->producer_tick  = m_impl->renderer.GetCommandScheduler().CurrentTick();
	const bool fg_capture = process_dlss && fg_available && dlss_inputs;
	const bool fg_captured =
	    fg_capture && CaptureDlssFgInputs(graphics, m_impl->renderer.GetCommandScheduler(), buffer,
	                                      *dlss_inputs, frame->fg_inputs);
	if (!fg_captured && frame->fg_inputs) {
		m_impl->renderer.GetCommandScheduler().DeferOperation(
		    [old = std::move(frame->fg_inputs)]() mutable { old.reset(); });
	}
	const auto captured           = stamp();
	const auto finish_preparation = [&] {
		frame->preparation = {acquired - prepare_begin, locked - acquired,
		                      resolved - locked,        inputs_ready - resolved,
		                      captured - inputs_ready,  stamp() - captured};
	};
	if (reconstruct) {
		frame->Configure(graphics, output_size, vk::Format::eR16G16B16A16Sfloat, true);
		if (frame->view == nullptr) {
			vk::ImageViewCreateInfo view {};
			view.image            = frame->image.image;
			view.viewType         = vk::ImageViewType::e2D;
			view.format           = frame->image.format;
			view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
			RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &frame->view),
			                     "create DLSS output view");
		}
		frame->dlss_evaluated =
		    m_impl->dlss.Evaluate(buffer, *dlss_inputs, frame->image, frame->view);
		if (!frame->dlss_evaluated) {
			if (frame->fg_inputs) frame->fg_inputs->jitter_x = frame->fg_inputs->jitter_y = 0;
			m_impl->emulator_inputs.Reset();
			// An UNORM view preserves sRGB color when resizing into RGBA16F.
			if (!m_impl->emulator_inputs.ResampleColor(buffer, *temporal_source, frame->image,
			                                           frame->view)) {
				frame->Configure(
				    graphics,
				    {temporal_source->backing.extent.width, temporal_source->backing.extent.height},
				    frame_format);
				frame->CopyFrom(buffer, *temporal_source);
			}
		} else if (generated_inputs && !m_impl->emulator_dlss_logged) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "{} emulator reconstruction active: {}x{} -> {}x{} -> {}x{}; {} motion, "
			    "final-frame mode\n",
			    Config::GetUpscaleBackend() == Config::UpscaleBackend::OptiScaler ? "OptiScaler"
			                                                                      : "DLSS",
			    temporal_source->backing.extent.width, temporal_source->backing.extent.height,
			    m_impl->dlss_input_size->width, m_impl->dlss_input_size->height, output_size.width,
			    output_size.height,
			    Config::GetUpscaleMotion() == Config::UpscaleMotion::Hybrid ? "hybrid"
			                                                                : "geometry"));
			m_impl->emulator_dlss_logged = true;
		}
		finish_preparation();
		return *frame;
	}
	if (process_dlss) {
		// Off presents the unjittered guest surface rather than the jittered
		// reconstruction input. FG constants must describe that actual color.
		if (frame->fg_inputs) frame->fg_inputs->jitter_x = frame->fg_inputs->jitter_y = 0;
		m_impl->dlss.SkipFrame();
		if (!fg_available) m_impl->emulator_inputs.Reset();
	}
	// Reuse the reduced raster; presentation handles spatial scaling.
	frame->Configure(
	    m_impl->window.graphic_ctx,
	    {temporal_source->backing.extent.width, temporal_source->backing.extent.height},
	    frame_format);
	frame->CopyFrom(buffer, *temporal_source);
	finish_preparation();
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
                                               CommandBuffer* producer) {
	KYTY_PROFILER_FUNCTION();
	auto              format = m_impl->frames.GetFormat();
	auto*             frame  = m_impl->frames.Acquire({width, height}, format);
	frame->dlss_evaluated    = false;
	frame->dlss_bypassed     = false;
	frame->guest_frame       = false;
	frame->preparation       = {};
	frame->producer_tick     = producer ? m_impl->renderer.GetCommandScheduler().CurrentTick() : 0;
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

bool Presenter::PresentLastFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (m_impl->layers[0].frame == nullptr && m_impl->layers[1].frame == nullptr) {
		return false;
	}
	m_impl->Present(false);
	return true;
}

uint64_t Presenter::DeferredPresentTime() const noexcept {
	return m_impl->deferred_deadline.load(std::memory_order_acquire);
}

void Presenter::PresentDeferred() {
	Common::LockGuard lock(m_impl->present_mutex);
	m_impl->PresentDeferred();
}

bool Presenter::IsGuestPaused() const noexcept {
	return m_impl->window.loop.paused.load(std::memory_order_acquire);
}

bool Presenter::NeedsSystemOverlayRefresh() const noexcept {
	const auto visual = GetSystemOverlayVisualState();
	return visual.active ||
	       visual.revision != m_impl->presented_overlay_revision.load(std::memory_order_acquire);
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

DlssFrameGeneration& Presenter::FrameGeneration() const noexcept {
	return *m_impl->window.frame_generation;
}

void Presenter::Present(Frame& frame) {
	const Layer layer {&frame, 0, false};
	Present(std::span(&layer, 1));
}

void Presenter::Present(std::span<const Layer> layers) {
	Common::LockGuard lock(m_impl->present_mutex);
	// The previous real frame is shown before its layers are released.
	m_impl->PresentDeferred();
	for (const auto& layer: layers) {
		EXIT_IF(layer.bus < 0 || layer.bus >= static_cast<int>(m_impl->layers.size()));
		m_impl->frames.ValidateForPresent(layer.frame);
		auto& previous = m_impl->layers[layer.bus];
		if (previous.frame != nullptr) {
			m_impl->frames.Release(previous.frame);
		}
		previous = layer;
	}
	m_impl->Present();
}

void Presenter::ClearLayer(int bus) {
	if (static_cast<size_t>(bus) >= m_impl->layers.size()) {
		return;
	}
	Common::LockGuard lock(m_impl->present_mutex);
	m_impl->PresentDeferred();
	auto& layer = m_impl->layers[bus];
	if (layer.frame != nullptr) {
		m_impl->frames.Release(layer.frame);
		layer = {};
	}
}

void Presenter::Impl::Present(bool new_frame) {
	KYTY_PROFILER_FUNCTION();
	PresentDeferred();
	const auto timing_begin = timing.Enabled() ? Common::Timer::QueryPerformanceCounter() : 0;

	const auto overlay_visual = GetSystemOverlayVisualState();
	auto*      fg             = window.frame_generation.get();
	const bool foreground     = fg && fg->Foreground();
	const bool generate_frame = new_frame && foreground && !window.loop.paused.load() &&
	                            layers[0].frame && layers[0].frame->guest_frame &&
	                            layers[0].frame->fg_inputs;
	const bool keep_frame_generation = !new_frame && foreground && !window.loop.paused.load() &&
	                                   fg && fg->Enabled() &&
	                                   Config::DlssFrameGenerationEnabled() && layers[0].frame &&
	                                   layers[0].frame->guest_frame && layers[0].frame->fg_inputs;
	const bool frame_generation_enabled = generate_frame || keep_frame_generation;
	if (fg && fg->Enabled() &&
	    (!frame_generation_enabled || !Config::DlssFrameGenerationEnabled())) {
		frames.WaitFrameGenerationInputs();
	}
	if (fg && fg->SetEnabled(frame_generation_enabled)) {
		RecoverSwapchain(Swapchain::Status::Recreate);
	}
	// Some window systems keep presenting an old swapchain after a resize.
	if (swapchain.NeedsResize()) {
		RecoverSwapchain(Swapchain::Status::Recreate);
	}
	const bool external_frame = fg && fg->External() && fg->Enabled() && generate_frame;
	if (external_frame && fg->BeginFrame()) {
		Image*         output                = nullptr;
		CommandBuffer* interpolation_command = nullptr;
		auto&          frame                 = *layers[0].frame;
		{
			Common::LockGuard render_lock(renderer.GetMutex());
			interpolation_command = &present_scheduler.BeginCommand();
			frame.Transit(interpolation_command->Handle(), vk::ImageLayout::eTransferSrcOptimal,
			              vk::AccessFlagBits2::eTransferRead);
			output = fg->Interpolate(present_scheduler, *interpolation_command, *frame.fg_inputs,
			                         frame.image, swapchain.Extent());
		}
		const auto acquired = output ? swapchain.AcquireNextImage() : Swapchain::Status::Success;
		{
			Common::LockGuard render_lock(renderer.GetMutex());
			uint64_t          producer_tick = 0;
			for (const auto& layer: layers)
				if (layer.frame)
					producer_tick = std::max(producer_tick, layer.frame->producer_tick);
			uint64_t tick = 0;
			if (output && acquired == Swapchain::Status::Success) {
				const bool overlay = overlay_visual.active && swapchain.PrepareSystemOverlay();
				swapchain.RecordPresentCommands(*interpolation_command, &frame, layers[1], overlay,
				                                output);
				tick = swapchain.Submit(present_scheduler, producer_tick);
			} else {
				// Reset and failed evaluations never present a duplicate real frame.
				SubmitInfo submit;
				if (producer_tick)
					submit.AddWait(renderer.GetCommandScheduler().GetMasterSemaphore().Handle(),
					               producer_tick);
				tick = present_scheduler.Submit(submit);
			}
			for (const auto& layer: layers)
				if (layer.frame) layer.frame->present_tick = tick;
			frame.fg_inputs->pending          = true;
			frame.fg_inputs->completion       = present_scheduler.GetMasterSemaphore().Handle();
			frame.fg_inputs->completion_value = tick;
		}
		if (output && acquired == Swapchain::Status::Success) {
			const auto status = swapchain.Present();
			if (status == Swapchain::Status::Success) {
				fg->InterpolatedPresented();
				// Let the guest flip complete now. The present thread shows the real
				// frame half a guest frame later, so spacing never delays rendering.
				const auto now = Common::Timer::QueryPerformanceCounter();
				const auto half_frame =
				    std::fmin(std::fmax(frame.fg_inputs->frame_time_ms, 1.f), 100.f) / 2000.f;
				deferred_work = timing_begin != 0 ? now - timing_begin : 0;
				deferred_deadline.store(
				    now + uint64_t(half_frame * Common::Timer::QueryPerformanceFrequency()),
				    std::memory_order_release);
				return;
			}
			RecoverSwapchain(status);
		} else if (acquired != Swapchain::Status::Success)
			RecoverSwapchain(acquired);
	}
	PresentLayers(new_frame, generate_frame, timing_begin, overlay_visual);
}

void Presenter::Impl::PresentLayers(bool new_frame, bool generate_frame, uint64_t timing_begin,
                                    const SystemOverlayVisualState& overlay_visual) {
	auto* fg = window.frame_generation.get();
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		// Sleep before recording/locking; producers can keep submitting guest work.
		bool fg_tag_failed =
		    fg && !fg->External() && fg->Enabled() && generate_frame && !fg->BeginFrame();
		auto status = swapchain.AcquireNextImage();
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}
		{
			Common::LockGuard render_lock(renderer.GetMutex());
			auto&             command = present_scheduler.BeginCommand();
			const bool        draw_system_overlay =
			    overlay_visual.active && swapchain.PrepareSystemOverlay();
			swapchain.RecordPresentCommands(command, layers[0].frame, layers[1],
			                                draw_system_overlay);
			if (fg && !fg->External() && fg->Enabled() && generate_frame && !fg_tag_failed) {
				fg_tag_failed =
				    !fg->TagFrame(command, *layers[0].frame->fg_inputs, swapchain.Extent());
			}
			uint64_t producer_tick = 0;
			for (const auto& layer: layers) {
				if (layer.frame)
					producer_tick = std::max(producer_tick, layer.frame->producer_tick);
			}
			const auto tick = swapchain.Submit(present_scheduler, producer_tick);
			for (const auto& layer: layers) {
				if (layer.frame != nullptr) {
					layer.frame->present_tick = tick;
				}
			}
		}
		bool recreate_after_present = false;
		if (fg_tag_failed) {
			frames.WaitFrameGenerationInputs();
			recreate_after_present = fg->SetEnabled(false);
		}
		if (fg && generate_frame) fg->PresentStart();
		status = swapchain.Present();
		if (fg)
			fg->PresentEnd(layers[0].frame ? layers[0].frame->fg_inputs.get() : nullptr,
			               generate_frame);
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}
		if (recreate_after_present) RecoverSwapchain(Swapchain::Status::Recreate);

		presented_overlay_revision.store(overlay_visual.revision, std::memory_order_release);
		const bool new_guest_frame =
		    new_frame && std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
			    return layer.frame != nullptr && layer.frame->guest_frame;
		    });
		window.UpdateTitle(layers[0].frame != nullptr && layers[0].frame->dlss_evaluated,
		                   new_guest_frame,
		                   layers[0].frame != nullptr && layers[0].frame->dlss_bypassed);
		const auto* main_frame  = layers[0].frame;
		const auto  preparation = new_guest_frame && main_frame && main_frame->guest_frame
		                              ? main_frame->preparation
		                              : FramePreparationTiming {};
		const auto  display_frames =
            new_guest_frame ? (fg && fg->Enabled() ? fg->PresentedFrames() : 1u) : 0u;
		timing.Record(timing_begin, new_guest_frame,
		              main_frame != nullptr && main_frame->dlss_evaluated, preparation,
		              display_frames);
		return;
	}
	LOGF("Vulkan presentation retry exhausted; dropping frame\n");
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

WindowContext::WindowContext() = default;

} // namespace Libs::Graphics
