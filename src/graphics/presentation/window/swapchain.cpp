#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_hints.h"
#include "SDL_joystick.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_pixels.h"
#include "SDL_rwops.h"
#include "SDL_stdinc.h"
#include "SDL_surface.h"
#include "SDL_thread.h"
#include "SDL_touch.h"
#include "SDL_video.h"
#include "SDL_vulkan.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/guestMemoryWindow.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/windowInternal.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Libs::Graphics {

struct PreparedFrame {
	VulkanImage                    image {VulkanImageType::Unknown};
	std::unique_ptr<CommandBuffer> present_commands;
	bool                           busy = false;
};

class PreparedFramePool {
public:
	void EnsureCapacity(uint32_t count, vk::Format format) {
		if (count == 0 || format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool requires at least one frame\n");
		}
		Common::LockGuard lock(m_mutex);
		m_format = format;
		while (m_frames.size() < count) {
			auto frame = std::make_unique<PreparedFrame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	vk::Format GetFormat() {
		Common::LockGuard lock(m_mutex);
		if (m_format == vk::Format::eUndefined) {
			EXIT("prepared-frame pool has no presentation format\n");
		}
		return m_format;
	}

	PreparedFrame* Acquire() {
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
		frame->busy = true;
		m_mutex.Unlock();

		// The producer only waits here. Reset stays on the presentation thread that owns the
		// allocating Vulkan command pool.
		if (frame->present_commands != nullptr) {
			frame->present_commands->WaitForFenceOnly();
		}

		return frame;
	}

	void Release(PreparedFrame* frame) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy = false;
		m_free.push_back(frame);
		m_available.Signal();
	}

private:
	Common::Mutex                               m_mutex;
	Common::CondVar                             m_available;
	std::vector<std::unique_ptr<PreparedFrame>> m_frames;
	std::deque<PreparedFrame*>                  m_free;
	vk::Format                                  m_format = vk::Format::eUndefined;
};

static PreparedFramePool* GetPreparedFramePool() {
	static auto* pool = new PreparedFramePool;
	return pool;
}

static void ConfigurePreparedFrame(PreparedFrame& frame, vk::Extent2D extent, vk::Format format) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}

	auto&      graphics   = g_window_ctx->graphic_ctx;
	auto&      dst        = frame.image;
	const bool compatible = dst.image != nullptr && dst.extent.width == extent.width &&
	                        dst.extent.height == extent.height && dst.format == format;
	if (compatible) {
		return;
	}
	if (dst.image != nullptr) {
		EXIT_IF(!dst.view_cache.views.empty());
		graphics.DeleteImage(dst);
		dst.memory = {};
	}

	dst.extent          = extent;
	dst.format          = format;
	dst.layers          = 1;
	dst.mip_levels      = 1;
	dst.layout          = vk::ImageLayout::eUndefined;
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

static uint32_t GetPresentationTexelSize(vk::Format format) {
	switch (format) {
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA2B10G10R10UnormPack32: return 4;
		case vk::Format::eR16G16B16A16Sfloat: return 8;
		default: return 0;
	}
}

static void DiagnosePreparedFrame(PreparedFrame& frame) {
	static uint32_t presented_frames = 0;
	presented_frames++;
	// Early frames pin down start-up, then keep sampling forever: "is the picture still changing?"
	// is the cheapest signal there is for whether a game is progressing or wedged, and a game can
	// sit on a splash for minutes before it moves.
	if (presented_frames > 3 && presented_frames != 30 && presented_frames != 60 &&
	    presented_frames != 120 && presented_frames != 130 && presented_frames != 140 &&
	    presented_frames != 150 && presented_frames != 180 && presented_frames != 240 &&
	    (presented_frames % 600) != 0) {
		return;
	}

	const auto texel_size = GetPresentationTexelSize(frame.image.format);
	if (texel_size == 0) {
		std::printf("Presentation source frame %u: %ux%u unsupported format=%d\n",
		            presented_frames, frame.image.extent.width, frame.image.extent.height,
		            static_cast<int>(frame.image.format));
		std::fflush(stdout);
		return;
	}

	const uint64_t pitch = static_cast<uint64_t>(frame.image.extent.width) * texel_size;
	const uint64_t size  = pitch * frame.image.extent.height;
	std::vector<uint8_t> pixels(size);
	Transfer::DownloadImage(pixels.data(), size, frame.image.extent.width, frame.image,
	                        frame.image.layout);

	uint8_t  min_value     = 0xff;
	uint8_t  max_value     = 0;
	uint64_t nonzero_bytes = 0;
	uint64_t hash          = 1469598103934665603ull;
	for (const auto value: pixels) {
		min_value = std::min(min_value, value);
		max_value = std::max(max_value, value);
		nonzero_bytes += value != 0 ? 1u : 0u;
		hash ^= value;
		hash *= 1099511628211ull;
	}

	uint16_t rgb_min[3] = {0x3ff, 0x3ff, 0x3ff};
	uint16_t rgb_max[3] = {};
	uint64_t visible_pixels = 0;
	std::vector<uint8_t> ppm_pixels;
	if (frame.image.format == vk::Format::eA2R10G10B10UnormPack32) {
		ppm_pixels.resize(static_cast<size_t>(frame.image.extent.width) *
		                  frame.image.extent.height * 3);
		for (uint64_t pixel_index = 0; pixel_index < size / 4; pixel_index++) {
			uint32_t packed = 0;
			std::memcpy(&packed, pixels.data() + pixel_index * 4, sizeof(packed));
			const uint16_t blue  = packed & 0x3ffu;
			const uint16_t green = (packed >> 10u) & 0x3ffu;
			const uint16_t red   = (packed >> 20u) & 0x3ffu;
			const uint16_t rgb[] = {red, green, blue};
			for (uint32_t channel = 0; channel < 3; channel++) {
				rgb_min[channel] = std::min(rgb_min[channel], rgb[channel]);
				rgb_max[channel] = std::max(rgb_max[channel], rgb[channel]);
			}
			visible_pixels += (red | green | blue) != 0 ? 1u : 0u;
			if (!ppm_pixels.empty()) {
				ppm_pixels[pixel_index * 3 + 0] = static_cast<uint8_t>((red * 255u + 511u) / 1023u);
				ppm_pixels[pixel_index * 3 + 1] =
				    static_cast<uint8_t>((green * 255u + 511u) / 1023u);
				ppm_pixels[pixel_index * 3 + 2] =
				    static_cast<uint8_t>((blue * 255u + 511u) / 1023u);
			}
		}
	}
	std::printf(
	    "Presentation source frame %u: %ux%u format=%d bytes=%u..%u nonzero=%llu/%llu "
	    "hash=%016llx rgb=%u..%u,%u..%u,%u..%u visible=%llu\n",
	    presented_frames, frame.image.extent.width, frame.image.extent.height,
	    static_cast<int>(frame.image.format), static_cast<unsigned>(min_value),
	    static_cast<unsigned>(max_value), static_cast<unsigned long long>(nonzero_bytes),
	    static_cast<unsigned long long>(size), static_cast<unsigned long long>(hash), rgb_min[0],
	    rgb_max[0], rgb_min[1], rgb_max[1], rgb_min[2], rgb_max[2],
	    static_cast<unsigned long long>(visible_pixels));
	if (!ppm_pixels.empty()) {
		const auto dump_path =
		    fmt::format("/tmp/kyty-presentation-source-{}.ppm", presented_frames);
		if (auto* dump = std::fopen(dump_path.c_str(), "wb"); dump != nullptr) {
			std::fprintf(dump, "P6\n%u %u\n255\n", frame.image.extent.width,
			             frame.image.extent.height);
			std::fwrite(ppm_pixels.data(), 1, ppm_pixels.size(), dump);
			std::fclose(dump);
			std::printf("Presentation source dump: %s\n", dump_path.c_str());
		}
	}
	std::fflush(stdout);
}

VulkanSwapchain::~VulkanSwapchain() = default;

[[maybe_unused]] static vk::SwapchainKHR VulkanCreateSwapchainInternal(
    vk::Device device, vk::SurfaceKHR surface, uint32_t width, uint32_t height,
    uint32_t image_count, SurfaceCapabilities& r, vk::Format& swapchain_format,
    vk::Extent2D& swapchain_extent, std::unique_ptr<vk::Image[]>& swapchain_images,
    std::unique_ptr<vk::ImageView[]>& swapchain_image_views, uint32_t& swapchain_images_count) {
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);

	EXIT_NOT_IMPLEMENTED(r.formats.empty());

	vk::Extent2D extent {};
	extent.width =
	    std::clamp(width, r.capabilities.minImageExtent.width, r.capabilities.maxImageExtent.width);
	extent.height = std::clamp(height, r.capabilities.minImageExtent.height,
	                           r.capabilities.maxImageExtent.height);

	image_count =
	    std::clamp(image_count, r.capabilities.minImageCount, r.capabilities.maxImageCount);

	vk::SwapchainCreateInfoKHR create_info {};
	create_info.sType         = vk::StructureType::eSwapchainCreateInfoKHR;
	create_info.pNext         = nullptr;
	create_info.flags         = {};
	create_info.surface       = surface;
	create_info.minImageCount = image_count;

	if (r.format_unorm_bgra32) {
		create_info.imageFormat     = vk::Format::eB8G8R8A8Unorm;
		create_info.imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
	} else if (r.format_srgb_bgra32) {
		create_info.imageFormat     = vk::Format::eB8G8R8A8Srgb;
		create_info.imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
	} else {
		const auto& format          = r.formats[0];
		create_info.imageFormat     = format.format;
		create_info.imageColorSpace = format.colorSpace;
	}

	create_info.imageExtent      = extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	create_info.imageSharingMode = vk::SharingMode::eExclusive;
	create_info.preTransform     = r.capabilities.currentTransform;
	create_info.compositeAlpha   = vk::CompositeAlphaFlagBitsKHR::eOpaque;
	create_info.presentMode      = vk::PresentModeKHR::eFifo;
	create_info.clipped          = VK_TRUE;
	create_info.oldSwapchain     = nullptr;

	swapchain_format = create_info.imageFormat;
	swapchain_extent = extent;

	vk::SwapchainKHR swapchain = nullptr;

	RequireVulkanSuccess(device.createSwapchainKHR(&create_info, nullptr, &swapchain),
	                     "vkCreateSwapchainKHR");
	EXIT_IF(swapchain == nullptr);

	auto images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* values) {
		    return device.getSwapchainImagesKHR(swapchain, count, values);
	    });
	EXIT_NOT_IMPLEMENTED(images.empty());

	swapchain_images_count = static_cast<uint32_t>(images.size());
	swapchain_images       = std::make_unique<vk::Image[]>(images.size());
	std::copy(images.begin(), images.end(), swapchain_images.get());

	swapchain_image_views = std::make_unique<vk::ImageView[]>(swapchain_images_count);
	for (uint32_t i = 0; i < swapchain_images_count; i++) {
		vk::ImageViewCreateInfo create_info {};
		create_info.sType                           = vk::StructureType::eImageViewCreateInfo;
		create_info.pNext                           = nullptr;
		create_info.flags                           = {};
		create_info.image                           = (swapchain_images)[i];
		create_info.viewType                        = vk::ImageViewType::e2D;
		create_info.format                          = swapchain_format;
		create_info.components.r                    = vk::ComponentSwizzle::eIdentity;
		create_info.components.g                    = vk::ComponentSwizzle::eIdentity;
		create_info.components.b                    = vk::ComponentSwizzle::eIdentity;
		create_info.components.a                    = vk::ComponentSwizzle::eIdentity;
		create_info.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		create_info.subresourceRange.baseArrayLayer = 0;
		create_info.subresourceRange.baseMipLevel   = 0;
		create_info.subresourceRange.layerCount     = 1;
		create_info.subresourceRange.levelCount     = 1;

		RequireVulkanSuccess(
		    device.createImageView(&create_info, nullptr, &((swapchain_image_views)[i])),
		    "vkCreateImageView");
		EXIT_IF((swapchain_image_views)[i] == nullptr);
	}

	return swapchain;
}

VulkanSwapchain* VulkanCreateSwapchain(uint32_t image_count) {
	auto& graphics = g_window_ctx->graphic_ctx;
	EXIT_IF(graphics.screen_width == 0);
	EXIT_IF(graphics.screen_height == 0);

	Common::LockGuard lock(g_window_ctx->mutex);

	auto  swapchain_owner = std::make_unique<VulkanSwapchain>();
	auto* s               = swapchain_owner.get();

	s->swapchain = VulkanCreateSwapchainInternal(
	    graphics.device, g_window_ctx->surface, graphics.screen_width, graphics.screen_height,
	    image_count, *g_window_ctx->surface_capabilities, s->swapchain_format, s->swapchain_extent,
	    s->swapchain_images, s->swapchain_image_views, s->swapchain_images_count);
	if (s->swapchain == nullptr) {
		EXIT("Could not create swapchain");
	}

	s->current_index = static_cast<uint32_t>(-1);
	s->present_frame = 0;

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	semaphore_info.pNext = nullptr;
	semaphore_info.flags = {};

	s->image_acquired_semaphores  = std::make_unique<vk::Semaphore[]>(s->swapchain_images_count);
	s->render_complete_semaphores = std::make_unique<vk::Semaphore[]>(s->swapchain_images_count);
	for (uint32_t i = 0; i < s->swapchain_images_count; i++) {
		auto result = graphics.device.createSemaphore(&semaphore_info, nullptr,
		                                              &s->image_acquired_semaphores[i]);
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

		result = graphics.device.createSemaphore(&semaphore_info, nullptr,
		                                         &s->render_complete_semaphores[i]);
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	}
	GetPreparedFramePool()->EnsureCapacity(s->swapchain_images_count, s->swapchain_format);

	return swapchain_owner.release();
}

static void VulkanDeleteSwapchain(VulkanSwapchain* s) {
	if (s == nullptr) {
		return;
	}
	auto  swapchain_owner = std::unique_ptr<VulkanSwapchain>(s);
	auto& graphics        = g_window_ctx->graphic_ctx;

	Transfer::WaitForQueueIdle();

	if (s->image_acquired_semaphores != nullptr) {
		for (uint32_t i = 0; i < s->swapchain_images_count; i++) {
			if (s->image_acquired_semaphores[i] != nullptr) {
				graphics.device.destroySemaphore(s->image_acquired_semaphores[i], nullptr);
			}
		}
	}
	if (s->render_complete_semaphores != nullptr) {
		for (uint32_t i = 0; i < s->swapchain_images_count; i++) {
			if (s->render_complete_semaphores[i] != nullptr) {
				graphics.device.destroySemaphore(s->render_complete_semaphores[i], nullptr);
			}
		}
	}
	if (s->swapchain_image_views != nullptr) {
		for (uint32_t i = 0; i < s->swapchain_images_count; i++) {
			if (s->swapchain_image_views[i] != nullptr) {
				graphics.device.destroyImageView(s->swapchain_image_views[i], nullptr);
			}
		}
	}

	if (s->swapchain != nullptr) {
		graphics.device.destroySwapchainKHR(s->swapchain, nullptr);
	}
}

static void VulkanRefreshSurfaceSize() {
	int width  = 0;
	int height = 0;
	SDL_Vulkan_GetDrawableSize(g_window_ctx->window, &width, &height);
	if (width > 0 && height > 0) {
		g_window_ctx->graphic_ctx.screen_width  = static_cast<uint32_t>(width);
		g_window_ctx->graphic_ctx.screen_height = static_cast<uint32_t>(height);
	}

	VulkanGetSurfaceCapabilities(g_window_ctx->graphic_ctx.physical_device, g_window_ctx->surface,
	                             *g_window_ctx->surface_capabilities);
}

static void VulkanRecreateSwapchain() {
	LOGF("Recreating Vulkan swapchain\n");
	VulkanRefreshSurfaceSize();
	VulkanDeleteSwapchain(g_window_ctx->swapchain);
	g_window_ctx->swapchain = VulkanCreateSwapchain(2);
}

PreparedFrame& WindowPrepareFrame(CommandBuffer& buffer, VideoOutVulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	if (image.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}

	auto*             frame = GetPreparedFramePool()->Acquire();
	Common::LockGuard render_lock(GetRenderContext().GetMutex());
	// Guest mappings change as the game streams; refresh once per presented frame so shaders
	// translating run-time addresses see the current layout.
	GetGuestMemoryWindow().RefreshRanges();
	GetRenderContext().GetTextureCache().RefreshVideoOut(image);
	ConfigurePreparedFrame(*frame, image.extent, image.format);
	ImageImageCopy copy {image};
	copy.width  = image.extent.width;
	copy.height = image.extent.height;
	const std::array copies {copy};
	Transfer::CopyImage(buffer, copies, frame->image, vk::ImageLayout::eTransferSrcOptimal);
	return *frame;
}

PreparedFrame& WindowPrepareBlankFrame(CommandBuffer& buffer, uint32_t width, uint32_t height,
                                       bool opaque) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	auto*             pool   = GetPreparedFramePool();
	auto              format = pool->GetFormat();
	auto*             frame  = pool->Acquire();
	Common::LockGuard render_lock(GetRenderContext().GetMutex());
	ConfigurePreparedFrame(*frame, {width, height}, format);
	vk::ClearColorValue clear {};
	clear.float32[3] = opaque ? 1.0f : 0.0f;
	Transfer::ClearColorImage(buffer, frame->image, clear);
	return *frame;
}

void WindowPresentFrame(PreparedFrame& frame) {
	KYTY_PROFILER_FUNCTION();

	struct ReleaseScope {
		PreparedFrame* frame;
		~ReleaseScope() { GetPreparedFramePool()->Release(frame); }
	};
	ReleaseScope release {&frame};

	DiagnosePreparedFrame(frame);

	if (g_window_ctx->window_hidden) {
		WindowUpdateIcon();

		SDL_ShowWindow(g_window_ctx->window);

		g_window_ctx->window_hidden = false;
		VulkanRecreateSwapchain();
	}

	auto* swapchain = g_window_ctx->swapchain;

	const auto present_frame = swapchain->present_frame;
	EXIT_IF(present_frame >= swapchain->swapchain_images_count);

	swapchain->current_index = static_cast<uint32_t>(-1);

	auto result = g_window_ctx->graphic_ctx.device.acquireNextImageKHR(
	    swapchain->swapchain, UINT64_MAX, swapchain->image_acquired_semaphores[present_frame],
	    nullptr, &swapchain->current_index);

	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			break;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			VulkanRecreateSwapchain();
			return;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", VulkanToString(result).c_str());
	}
	EXIT_NOT_IMPLEMENTED(swapchain->current_index == static_cast<uint32_t>(-1));
	if (frame.present_commands == nullptr) {
		frame.present_commands = std::make_unique<CommandBuffer>();
	}
	frame.present_commands->WaitForFenceAndReset();
	auto& buffer = *frame.present_commands;

	auto vk_buffer = buffer.Handle();

	buffer.Begin();

	Transfer::BlitToSwapchain(buffer, frame.image, *swapchain);

	vk::ImageMemoryBarrier pre_present_barrier {};
	pre_present_barrier.sType                           = vk::StructureType::eImageMemoryBarrier;
	pre_present_barrier.pNext                           = nullptr;
	pre_present_barrier.srcAccessMask                   = vk::AccessFlagBits::eTransferWrite;
	pre_present_barrier.dstAccessMask                   = vk::AccessFlagBits::eMemoryRead;
	pre_present_barrier.oldLayout                       = vk::ImageLayout::eTransferDstOptimal;
	pre_present_barrier.newLayout                       = vk::ImageLayout::ePresentSrcKHR;
	pre_present_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	pre_present_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	pre_present_barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	pre_present_barrier.subresourceRange.baseMipLevel   = 0;
	pre_present_barrier.subresourceRange.levelCount     = 1;
	pre_present_barrier.subresourceRange.baseArrayLayer = 0;
	pre_present_barrier.subresourceRange.layerCount     = 1;
	pre_present_barrier.image = swapchain->swapchain_images[swapchain->current_index];
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                          vk::PipelineStageFlagBits::eBottomOfPipe, vk::DependencyFlags {}, 0,
	                          nullptr, 0, nullptr, 1, &pre_present_barrier);

	buffer.End();

	auto render_complete_semaphore =
	    swapchain->render_complete_semaphores[swapchain->current_index];
	const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
	buffer.ExecuteWithSemaphore(swapchain->image_acquired_semaphores[present_frame], wait_stage,
	                            render_complete_semaphore);

	vk::PresentInfoKHR present;
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.pNext              = nullptr;
	present.swapchainCount     = 1;
	present.pSwapchains        = &swapchain->swapchain;
	present.pImageIndices      = &swapchain->current_index;
	present.pWaitSemaphores    = &render_complete_semaphore;
	present.waitSemaphoreCount = 1;
	present.pResults           = nullptr;

	auto& graphics = g_window_ctx->graphic_ctx;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		result = graphics.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			break;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			VulkanRecreateSwapchain();
			return;
		default: EXIT("vkQueuePresentKHR failed: %s\n", VulkanToString(result).c_str());
	}

	swapchain->present_frame = (present_frame + 1u) % swapchain->swapchain_images_count;

	RenderDocOnPresent();
	WindowUpdateTitle();
}

} // namespace Libs::Graphics
