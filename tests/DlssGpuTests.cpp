// Opt-in integration test: evaluates the production DLSS backend on a real RTX
// device with controlled scene inputs and the emulator-wide presentation path.
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/subsystems.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/dlss.h"
#include "graphics/presentation/emulatorDlssInputs.h"
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/presentationFrame.h"
#include "graphics/presentation/window/windowInternal.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/audio.h"
#include "libs/controller.h"
#include "libs/network.h"
#include "loader/timer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <spirv-tools/libspirv.hpp>
#include <thread>

using namespace Libs::Graphics;
namespace {
std::atomic<int> validation_errors       = 0;
bool             interpolation_succeeded = true;
void             Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "DlssGpuTests: %s\n", message);
        std::fflush(stderr);
        std::_Exit(1);
    }
}
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
                                             vk::DebugUtilsMessageTypeFlagsEXT,
                                             const vk::DebugUtilsMessengerCallbackDataEXT* message,
                                             void*) {
	if (severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
		++validation_errors;
		std::fprintf(stderr, "Vulkan validation: %s\n", message->pMessage);
	}
	return VK_FALSE;
}

ImageInfo Description(vk::Format format, vk::Extent2D size) {
	ImageInfo info {};
	info.pixel_format    = format;
	info.extent          = {size.width, size.height, 1};
	info.pitch           = size.width;
	info.bytes_per_block = format == vk::Format::eR16G16B16A16Sfloat ? 8 : 4;
	info.mip_layout[0] = {0, uint64_t(size.width) * size.height * info.bytes_per_block, size.width,
	                      size.height};
	return info;
}

std::vector<uint8_t> Readback(CommandScheduler& scheduler, VulkanImage& image,
                              uint32_t bytes_per_pixel);
void CheckEncodedResample(const std::vector<uint8_t>& source, vk::Extent2D source_size,
                          const std::vector<uint8_t>& output, vk::Extent2D output_size, bool bgra);

void FrameGenerationCompletionCase(GraphicContext& graphics) {
	DlssFgInputs inputs;
	Check(inputs.Ready(graphics), "unused FG inputs are not ready");
	inputs.pending = true;
	Check(!inputs.Ready(graphics), "missing FG completion timeline was treated as safe");
	vk::SemaphoreTypeCreateInfo timeline {};
	timeline.semaphoreType = vk::SemaphoreType::eTimeline;
	vk::SemaphoreCreateInfo create {};
	create.pNext = &timeline;
	vk::Semaphore completion;
	RequireVulkanSuccess(graphics.device.createSemaphore(&create, nullptr, &completion),
	                     "create FG completion test timeline");
	inputs.completion       = completion;
	inputs.completion_value = 1;
	Check(!inputs.Ready(graphics), "pending FG work was reused");
	inputs.completion_value = 0;
	Check(inputs.Ready(graphics), "zero FG completion value was not ready");
	inputs.completion_value = 1;
	vk::SemaphoreSignalInfo signal {};
	signal.semaphore = completion;
	signal.value     = 1;
	RequireVulkanSuccess(graphics.device.signalSemaphore(&signal),
	                     "signal FG completion test timeline");
	Check(inputs.Ready(graphics) && inputs.pending, "FG readiness changed lifetime ownership");
	inputs.Wait(graphics);
	Check(!inputs.pending && !inputs.completion && inputs.completion_value == 0,
	      "FG reuse did not retire completion state");
	graphics.device.destroySemaphore(completion, nullptr);
	std::puts("Frame Generation completion timeline cases passed");
}

void PresentationQueueCase(GraphicContext& graphics, RenderContext& renderer) {
	if (graphics.present_queue == graphics.queue) {
		std::puts("SKIP: device exposes only one universal queue");
		return;
	}
	auto&            producer = renderer.GetCommandScheduler();
	CommandScheduler presentation(renderer, graphics, true);
	HW::Context      registers {};
	HW::UserConfig   user {};
	HW::Shader       shaders {};
	presentation.Begin(registers, user, shaders);
	Presenter::Frame frame;
	frame.Configure(graphics, {32, 16}, vk::Format::eR8G8B8A8Unorm);
	vk::ClearColorValue color {};
	color.float32 = std::array<float, 4> {1, 0, 0, 1};
	frame.Clear(producer.Current(), color);
	vk::SemaphoreTypeCreateInfo type {};
	type.semaphoreType = vk::SemaphoreType::eTimeline;
	vk::SemaphoreCreateInfo create {};
	create.pNext = &type;
	vk::Semaphore gate;
	RequireVulkanSuccess(graphics.device.createSemaphore(&create, nullptr, &gate),
	                     "create cross-queue test gate");
	SubmitInfo blocked;
	blocked.AddWait(gate, 1);
	const auto producer_tick = producer.Submit(blocked);
	frame.Transit(presentation.Current().Handle(), vk::ImageLayout::eTransferSrcOptimal,
	              vk::AccessFlagBits2::eTransferRead);
	SubmitInfo dependency;
	dependency.AddWait(producer.GetMasterSemaphore().Handle(), producer_tick);
	const auto            consumer_tick = presentation.Submit(dependency);
	const auto            consumer_done = presentation.GetMasterSemaphore().Handle();
	vk::SemaphoreWaitInfo wait {};
	wait.semaphoreCount = 1;
	wait.pSemaphores    = &consumer_done;
	wait.pValues        = &consumer_tick;
	Check(graphics.device.waitSemaphores(&wait, 1000000) == vk::Result::eTimeout,
	      "presentation finished before its producer dependency");
	vk::SemaphoreSignalInfo signal {};
	signal.semaphore = gate;
	signal.value     = 1;
	RequireVulkanSuccess(graphics.device.signalSemaphore(&signal), "release cross-queue producer");
	(void)producer.BeginCommand();
	(void)presentation.BeginCommand();
	const auto pixels = Readback(presentation, frame.image, 4);
	for (size_t i = 0; i < pixels.size(); i += 4) {
		Check(pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 0 && pixels[i + 3] == 255,
		      "separate presentation queue read incomplete producer pixels");
	}
	graphics.DeleteImage(frame.image);
	graphics.device.destroySemaphore(gate, nullptr);
	std::puts("Separate presentation queue and producer timeline dependency passed");
}

void RasterScaleCase(GraphicContext& graphics, CommandScheduler& scheduler) {
	for (const auto percent: {25u, 50u, 67u, 100u}) {
		auto& command = scheduler.Current();
		Image color(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {256, 128}));
		Check(command.RasterColorSource(color) == nullptr,
		      "a new image inherited a previous raster surface");
		Image       depth(graphics, scheduler, Description(vk::Format::eD32Sfloat, {256, 128}));
		RenderState state {};
		state.width                 = 256;
		state.height                = 128;
		state.num_color_attachments = 1;
		state.raster_scale_x = state.raster_scale_y = float(percent) / 100.f;
		auto& attachment                            = state.color_attachments[0];
		attachment.image                            = &color;
		attachment.image_layout                     = vk::ImageLayout::eColorAttachmentOptimal;
		attachment.is_clear                         = true;
		attachment.clear_value = {std::bit_cast<uint32_t>(.25f), std::bit_cast<uint32_t>(.5f), 0,
		                          std::bit_cast<uint32_t>(1.f)};
		ImageViewInfo view {};
		view.format           = color.backing.format;
		view.usage            = vk::ImageUsageFlagBits::eColorAttachment;
		attachment.image_view = color.FindView(view);
		auto& db              = state.depth_stencil_attachment;
		db.image              = &depth;
		db.image_layout       = vk::ImageLayout::eDepthAttachmentOptimal;
		db.has_depth = db.depth_clear = true;
		db.clear_value[0]             = std::bit_cast<uint32_t>(.75f);
		view.format                   = depth.backing.format;
		view.aspect                   = vk::ImageAspectFlagBits::eDepth;
		view.usage                    = vk::ImageUsageFlagBits::eDepthStencilAttachment;
		db.image_view                 = depth.FindView(view);
		color.Transit(attachment.image_layout, vk::AccessFlagBits2::eColorAttachmentWrite, {},
		              command.Handle());
		depth.Transit(db.image_layout, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {},
		              command.Handle());
		command.BeginRendering(state);
		const auto& effective = command.EffectiveRenderState();
		Check(effective.width == uint32_t(std::lround(256 * percent / 100.0)) &&
		          effective.height == uint32_t(std::lround(128 * percent / 100.0)),
		      "render scale did not reduce actual raster attachments");
		vk::ClearAttachment clear {};
		clear.aspectMask               = vk::ImageAspectFlagBits::eColor;
		clear.colorAttachment          = 0;
		clear.clearValue.color.float32 = std::array<float, 4> {1.f, 0.f, 0.f, 1.f};
		vk::ClearRect rect {};
		rect.rect.extent = {effective.width / 2, effective.height};
		rect.layerCount  = 1;
		command.Handle().clearAttachments(1, &clear, 1, &rect);
		// This consumer computes barriers before EndRendering. It must still see
		// the upscaled render result, with tracked layouts matching the real GPU.
		color.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
		              command.Handle());
		depth.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
		              command.Handle());
		const auto pixels = Readback(scheduler, color.backing, 4);
		Check(pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0,
		      "scaled color not stored into guest image");
		const auto right = (256 * 64 + 240) * 4;
		Check(std::abs(int(pixels[right]) - 64) <= 1 && std::abs(int(pixels[right + 1]) - 128) <= 1,
		      "scaled load clear did not preserve unmodified pixels");
		if (percent == 50) {
			// A native-sized nearest copy followed by DLSS input resampling must
			// not replace one interpolation of the actual reduced raster color.
			std::vector<uint8_t> reference(128 * 64 * 4);
			// Retain the known two-color geometry, using the readback clear
			// values to account for the device's permitted UNORM rounding.
			for (uint32_t y = 0; y < 64; ++y)
				for (uint32_t x = 0; x < 128; ++x) {
					const auto pixel     = (y * 128 + x) * 4;
					reference[pixel]     = x < 64 ? 255 : pixels[right];
					reference[pixel + 1] = x < 64 ? 0 : pixels[right + 1];
					reference[pixel + 2] = 0;
					reference[pixel + 3] = 255;
				}
			EmulatorDlssInputs generator(graphics, scheduler);
			auto*              reduced = command.RasterColorSource(color);
			Check(reduced != nullptr && reduced->backing.extent.width == 128 &&
			          reduced->backing.extent.height == 64,
			      "final color lost its actual reduced raster source");
			auto inputs = generator.Prepare(scheduler.Current(), *reduced, {200, 100});
			Check(inputs.has_value(), "reduced raster temporal inputs");
			CheckEncodedResample(reference, {128, 64},
			                     Readback(scheduler, inputs->color->backing, 8), {200, 100}, false);
			color.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
			              {}, scheduler.Current().Handle());
			vk::ClearColorValue replacement {};
			replacement.float32 = std::array<float, 4> {0, 1, 0, 1};
			const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
			scheduler.Current().Handle().clearColorImage(
			    color.backing.image, vk::ImageLayout::eTransferDstOptimal, &replacement, 1, &range);
			Check(command.RasterColorSource(color) == nullptr,
			      "a later GPU write retained stale raster pixels");
			// LOAD must still retain prior color/depth; only CLEAR copies may be skipped.
			state.color_attachments[0].is_clear        = false;
			state.depth_stencil_attachment.depth_clear = false;
			color.Transit(attachment.image_layout, vk::AccessFlagBits2::eColorAttachmentWrite, {},
			              command.Handle());
			depth.Transit(db.image_layout, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {},
			              command.Handle());
			command.BeginRendering(state);
			command.Handle().clearAttachments(1, &clear, 1, &rect);
			color.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
			              {}, command.Handle());
			const auto loaded = Readback(scheduler, color.backing, 4);
			Check(loaded[0] == 255 && loaded[1] == 0 && loaded[right] == 0 &&
			          loaded[right + 1] == 255,
			      "scaled LOAD discarded existing pixels");
			Check(command.RasterColorSource(color) != nullptr,
			      "a read-only consumer invalidated raster color");
			// A destination barrier may be computed before it ends the pass. It
			// must invalidate the source even though EndRendering then stores it.
			color.Transit(attachment.image_layout, vk::AccessFlagBits2::eColorAttachmentWrite, {},
			              command.Handle());
			depth.Transit(db.image_layout, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {},
			              command.Handle());
			command.BeginRendering(state);
			color.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
			              {}, command.Handle());
			Check(command.RasterColorSource(color) == nullptr,
			      "a write scheduled before pass completion retained stale raster color");
			command.Handle().clearColorImage(
			    color.backing.image, vk::ImageLayout::eTransferDstOptimal, &replacement, 1, &range);
		}
		// Exercise image retirement on a subsequent pass with a different size.
		scheduler.FlushAndWait();
	}
	std::puts("Raster scale GPU cases passed");
}

void Clear(CommandBuffer& command, Image& image, std::array<float, 4> value) {
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	vk::ClearColorValue clear {};
	clear.float32 = value;
	vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
	                                 &clear, 1, &range);
}

void RasterTargetReuseCase(GraphicContext& graphics, CommandScheduler& scheduler) {
	Image      first(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {256, 128}));
	Image      second(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {128, 64}));
	const auto pass = [&](Image& target, bool clear = true, bool leave_open = false) {
		RenderState state {};
		state.width                 = target.backing.extent.width;
		state.height                = target.backing.extent.height;
		state.num_color_attachments = 1;
		state.raster_scale_x = state.raster_scale_y = .5f;
		auto& attachment                            = state.color_attachments[0];
		attachment.image                            = &target;
		attachment.image_layout                     = vk::ImageLayout::eColorAttachmentOptimal;
		attachment.is_clear                         = clear;
		attachment.clear_value = {std::bit_cast<uint32_t>(.25f), std::bit_cast<uint32_t>(.5f), 0,
		                          std::bit_cast<uint32_t>(1.f)};
		ImageViewInfo view {};
		view.format           = target.backing.format;
		view.aspect           = vk::ImageAspectFlagBits::eColor;
		view.usage            = vk::ImageUsageFlagBits::eColorAttachment;
		attachment.image_view = target.FindView(view);
		auto& command         = scheduler.Current();
		target.Transit(attachment.image_layout, vk::AccessFlagBits2::eColorAttachmentWrite, {},
		               command.Handle());
		command.BeginRendering(state);
		if (leave_open) return vk::Image {};
		command.EndRendering();
		auto* reduced = command.RasterColorSource(target);
		Check(reduced, "alternating target lost reduced output");
		return reduced->backing.image;
	};
	const auto original = pass(first);
	pass(second);
	Check(pass(first, false) == original,
	      "alternating render targets recreate small-render images");
	const auto pixels = Readback(scheduler, first.backing, 4);
	Check(std::abs(int(pixels[0]) - 64) <= 1 && std::abs(int(pixels[1]) - 128) <= 1 &&
	          pixels[3] == 255,
	      "alternating target LOAD did not retain original color");
	pass(first, true, true);
	auto& command = scheduler.Current();
	// Image::Transit plans this write before it asks the active pass to end.
	first.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	const vk::ClearColorValue       green(std::array<float, 4> {0, 1, 0, 1});
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.Handle().clearColorImage(first.backing.image, vk::ImageLayout::eTransferDstOptimal,
	                                 &green, 1, &range);
	pass(second);
	pass(first, false);
	const auto modified = Readback(scheduler, first.backing, 4);
	Check(modified[0] == 0 && modified[1] == 255 && modified[3] == 255,
	      "cached raster LOAD ignored a write planned before EndRendering");
	Image       depth(graphics, scheduler, Description(vk::Format::eD32Sfloat, {128, 64}));
	RenderState depth_state {};
	depth_state.width          = 128;
	depth_state.height         = 64;
	depth_state.raster_scale_x = depth_state.raster_scale_y = .5f;
	auto& attachment                                        = depth_state.depth_stencil_attachment;
	attachment.image                                        = &depth;
	attachment.has_depth = attachment.depth_clear = true;
	attachment.clear_value[0]                     = std::bit_cast<uint32_t>(.75f);
	attachment.image_layout                       = vk::ImageLayout::eDepthAttachmentOptimal;
	ImageViewInfo depth_view {};
	depth_view.format     = depth.backing.format;
	depth_view.aspect     = vk::ImageAspectFlagBits::eDepth;
	depth_view.usage      = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	attachment.image_view = depth.FindView(depth_view);
	depth.Transit(attachment.image_layout, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {},
	              command.Handle());
	command.BeginRendering(depth_state);
	command.EndRendering();
	depth.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	const vk::ClearDepthStencilValue changed_depth {.2f, 0};
	const vk::ImageSubresourceRange  depth_range {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
	command.Handle().clearDepthStencilImage(
	    depth.backing.image, vk::ImageLayout::eTransferDstOptimal, &changed_depth, 1, &depth_range);
	attachment.depth_clear  = false;
	attachment.image_layout = vk::ImageLayout::eDepthReadOnlyOptimal;
	depth.Transit(attachment.image_layout, vk::AccessFlagBits2::eDepthStencilAttachmentRead, {},
	              command.Handle());
	command.BeginRendering(depth_state);
	command.EndRendering();
	const auto depth_pixels = Readback(scheduler, depth.backing, 4);
	float      depth_value  = 0;
	std::memcpy(&depth_value, depth_pixels.data(), sizeof(depth_value));
	Check(std::abs(depth_value - .2f) < .001f,
	      "read-only raster LOAD ignored a native depth write");
	scheduler.FlushAndWait();
	std::puts("Raster target reuse passed");
}

std::vector<uint8_t> Readback(CommandScheduler& scheduler, VulkanImage& image,
                              uint32_t bytes_per_pixel) {
	auto&      graphics = scheduler.Graphics();
	const auto aspect   = image.format == vk::Format::eD32Sfloat ? vk::ImageAspectFlagBits::eDepth
	                                                             : vk::ImageAspectFlagBits::eColor;
	VkBufferCreateInfo create {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	create.size  = uint64_t(image.extent.width) * image.extent.height * bytes_per_pixel;
	create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo alloc {};
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer          buffer     = VK_NULL_HANDLE;
	VmaAllocation     allocation = nullptr;
	VmaAllocationInfo mapped {};
	Check(vmaCreateBuffer(graphics.allocator, &create, &alloc, &buffer, &allocation, &mapped) ==
	          VK_SUCCESS,
	      "emulator readback allocation");
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = image.state.pl_stage;
	barrier.srcAccessMask       = image.state.access_mask;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferRead;
	barrier.oldLayout           = image.state.layout;
	barrier.newLayout           = vk::ImageLayout::eTransferSrcOptimal;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                                             = image.image;
	barrier.subresourceRange                                  = {aspect, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	auto& command                      = scheduler.Current();
	command.EndRendering();
	command.Handle().pipelineBarrier2(dependency);
	image.state = {barrier.dstStageMask, barrier.dstAccessMask, barrier.newLayout};
	image.subresource_states.clear();
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {aspect, 0, 0, 1};
	copy.imageExtent      = image.extent;
	command.Handle().copyImageToBuffer(image.image, barrier.newLayout, buffer, 1, &copy);
	scheduler.Finish();
	Check(vmaInvalidateAllocation(graphics.allocator, allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
	      "readback invalidation");
	std::vector<uint8_t> result(create.size);
	std::memcpy(result.data(), mapped.pMappedData, result.size());
	vmaDestroyBuffer(graphics.allocator, buffer, allocation);
	return result;
}

float Half(uint16_t bits) {
	const int   exponent = (bits >> 10) & 31;
	const int   mantissa = bits & 1023;
	const float value    = exponent == 0    ? std::ldexp(float(mantissa), -24)
	                       : exponent == 31 ? std::numeric_limits<float>::infinity()
	                                        : std::ldexp(float(1024 + mantissa), exponent - 25);
	return bits & 0x8000 ? -value : value;
}

void CheckEncodedResample(const std::vector<uint8_t>& source, vk::Extent2D source_size,
                          const std::vector<uint8_t>& output, vk::Extent2D output_size,
                          bool bgra = false) {
	for (const auto& point: std::array<std::array<uint32_t, 2>, 3> {
	         {{0, 0},
	          {output_size.width / 2, output_size.height / 2},
	          {output_size.width - 1, output_size.height - 1}}}) {
		const float x = std::clamp((point[0] + .5f) * source_size.width / output_size.width - .5f,
		                           0.f, float(source_size.width - 1));
		const float y = std::clamp((point[1] + .5f) * source_size.height / output_size.height - .5f,
		                           0.f, float(source_size.height - 1));
		const auto  x0 = uint32_t(x), y0 = uint32_t(y);
		const auto  x1 = std::min(x0 + 1, source_size.width - 1),
		           y1  = std::min(y0 + 1, source_size.height - 1);
		for (size_t channel = 0; channel < 4; ++channel) {
			const auto source_channel = bgra && channel < 3 ? 2 - channel : channel;
			const auto sample         = [&](uint32_t sx, uint32_t sy) {
                return source[(uint64_t(sy) * source_size.width + sx) * 4 + source_channel] / 255.f;
			};
			const float expected =
			    std::lerp(std::lerp(sample(x0, y0), sample(x1, y0), x - x0),
			              std::lerp(sample(x0, y1), sample(x1, y1), x - x0), y - y0);
			uint16_t actual;
			std::memcpy(&actual,
			            output.data() + (uint64_t(point[1]) * output_size.width + point[0]) * 8 +
			                channel * 2,
			            2);
			if (std::abs(Half(actual) - expected) >= .003f) {
				std::fprintf(
				    stderr,
				    "Color resampling mismatch at (%u, %u), channel %zu: expected %.6f, got %.6f\n",
				    point[0], point[1], channel, expected, Half(actual));
				Check(false, "color resampling changed encoded color or channel order");
			}
		}
	}
}

void UploadPattern(CommandScheduler& scheduler, Image& image, int shift_x, int shift_y,
                   bool detail = false) {
	auto&                graphics = scheduler.Graphics();
	const auto           width = image.backing.extent.width, height = image.backing.extent.height;
	std::vector<uint8_t> pixels(uint64_t(width) * height * 4);
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x) {
			const float px = float(x) - shift_x, py = float(y) - shift_y;
			const auto  index = (uint64_t(y) * width + x) * 4;
			pixels[index]     = uint8_t(128 + 55 * std::sin(px * .043f + py * .027f));
			pixels[index + 1] = uint8_t(128 + 55 * std::sin(px * .021f - py * .051f));
			pixels[index + 2] = uint8_t(128 + 55 * std::cos(px * .037f + py * .013f));
			pixels[index + 3] = 255;
			if (detail) {
				const int     sx = int(x) - shift_x, sy = int(y) - shift_y;
				const uint8_t value = ((sx / 2 + sy / 2) & 1) ? 220 : 32;
				pixels[index] = pixels[index + 1] = pixels[index + 2] = value;
			}
		}
	VkBufferCreateInfo create {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	create.size  = pixels.size();
	create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo alloc {};
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	alloc.flags =
	    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer          buffer     = VK_NULL_HANDLE;
	VmaAllocation     allocation = nullptr;
	VmaAllocationInfo mapped {};
	Check(vmaCreateBuffer(graphics.allocator, &create, &alloc, &buffer, &allocation, &mapped) ==
	          VK_SUCCESS,
	      "pattern upload allocation");
	std::memcpy(mapped.pMappedData, pixels.data(), pixels.size());
	Check(vmaFlushAllocation(graphics.allocator, allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
	      "pattern upload flush");
	auto& command = scheduler.Current();
	command.EndRendering();
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent      = image.backing.extent;
	command.Handle().copyBufferToImage(buffer, image.backing.image,
	                                   vk::ImageLayout::eTransferDstOptimal, 1, &copy);
	scheduler.Finish();
	vmaDestroyBuffer(graphics.allocator, buffer, allocation);
}

void OptiScalerFramePixelsCase(GraphicContext& graphics, CommandScheduler& scheduler) {
	DlssFrameGeneration fg;
	fg.Initialize(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr);
	fg.OnDevice(graphics);
	Check(fg.External() && fg.Available(), "external Vulkan FG runtime unavailable");
	fg.SetEnabled(true);
	Image        source(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {320, 180}));
	Image        expected(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {320, 180}));
	DlssFgInputs inputs;
	inputs.depth           = std::make_unique<Image>(graphics, scheduler,
	                                                 Description(vk::Format::eR32Sfloat, {320, 180}));
	inputs.motion          = std::make_unique<Image>(graphics, scheduler,
	                                                 Description(vk::Format::eR16G16Sfloat, {320, 180}));
	double generated_error = 0, current_error = 0;
	for (int frame = 0; frame < 8; ++frame) {
		UploadPattern(scheduler, source, frame * 4, 0);
		const auto current = Readback(scheduler, source.backing, 4);
		Clear(scheduler.Current(), *inputs.depth, {.5f, 0, 0, 0});
		Clear(scheduler.Current(), *inputs.motion, {-4.f, 0, 0, 0});
		inputs.reset = frame == 0;
		fg.BeginFrame();
		source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
		               scheduler.Current().Handle());
		auto* output =
		    fg.Interpolate(scheduler, scheduler.Current(), inputs, source.backing, {320, 180});
		if (frame == 0) {
			Check(!output, "FG presents reset history as an interpolated frame");
			scheduler.FlushAndWait();
			continue;
		}
		Check(output, "external FG did not produce an image");
		const auto generated = Readback(scheduler, output->backing, 4);
		UploadPattern(scheduler, expected, frame * 4 - 2, 0);
		const auto midpoint = Readback(scheduler, expected.backing, 4);
		// Ignore borders where translating content is newly exposed.
		for (uint32_t y = 16; y < 164; ++y)
			for (uint32_t x = 16; x < 304; ++x) {
				const auto pixel = (y * 320 + x) * 4;
				for (uint32_t channel = 0; channel < 3; ++channel) {
					generated_error +=
					    std::abs(int(generated[pixel + channel]) - int(midpoint[pixel + channel]));
					current_error +=
					    std::abs(int(current[pixel + channel]) - int(midpoint[pixel + channel]));
				}
			}
	}
	std::printf("OptiScaler FG midpoint error: generated %.0f / current %.0f\n", generated_error,
	            current_error);
	Check(generated_error < current_error * .9,
	      "generated pixels do not reconstruct intermediate motion");
	scheduler.FlushAndWait();
	fg.Shutdown();
}

void EmulatorMotionCase(GraphicContext& graphics, CommandScheduler& scheduler) {
	EmulatorDlssInputs generator(graphics, scheduler);
	Image source(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {1280, 720}));
	UploadPattern(scheduler, source, 0, 0);
	auto first = generator.Prepare(scheduler.Current(), source, {640, 360});
	Check(first && first->reset_history && first->jitter_x == 0 && first->jitter_y == 0,
	      "first emulator frame/history");
	const auto zero_motion = Readback(scheduler, first->motion_vectors->backing, 4);
	Check(std::all_of(zero_motion.begin(), zero_motion.end(), [](uint8_t v) { return v == 0; }),
	      "first frame has stale motion");
	// Warm the compute shaders before measuring consecutive-frame motion.
	Check(generator.Prepare(scheduler.Current(), source, {640, 360}).has_value(), "motion warmup");
	scheduler.Finish();
	auto stationary = generator.Prepare(scheduler.Current(), source, {640, 360});
	Check(stationary && !stationary->reset_history, "stationary history unexpectedly resets");
	const auto static_motion = Readback(scheduler, stationary->motion_vectors->backing, 4);
	Check(std::all_of(static_motion.begin(), static_motion.end(), [](uint8_t v) { return v == 0; }),
	      "static scene has nonzero motion");
	UploadPattern(scheduler, source, 16, 8);
	auto moving = generator.Prepare(scheduler.Current(), source, {640, 360});
	Check(moving && !moving->reset_history, "moving frame lacks previous history");
	const auto         motion = Readback(scheduler, moving->motion_vectors->backing, 4);
	std::vector<float> xs, ys;
	for (uint32_t y = 70; y < 290; y += 7)
		for (uint32_t x = 90; x < 550; x += 7) {
			uint16_t values[2];
			std::memcpy(values, motion.data() + (uint64_t(y) * 640 + x) * 4, 4);
			xs.push_back(Half(values[0]));
			ys.push_back(Half(values[1]));
		}
	std::sort(xs.begin(), xs.end());
	std::sort(ys.begin(), ys.end());
	const auto mx = xs[xs.size() / 2], my = ys[ys.size() / 2];
	std::printf("Emulator motion median: %.3f, %.3f (expected -8, -4 render pixels)\n", mx, my);
	Check(std::abs(mx + 8) < 1.5f && std::abs(my + 4) < 1.5f,
	      "GPU motion direction or render-pixel scale");
	UploadPattern(scheduler, source, 32, 16);
	auto fg_only = generator.Prepare(scheduler.Current(), source, {640, 360}, false);
	Check(fg_only && !fg_only->color && !fg_only->bias_current_color && fg_only->jitter_x == 0 &&
	          fg_only->jitter_y == 0 && !fg_only->reset_history,
	      "FG-only preparation generated unused color or jitter");
	const auto fg_motion = Readback(scheduler, fg_only->motion_vectors->backing, 4);
	uint16_t   fg_center[2];
	std::memcpy(fg_center, fg_motion.data() + (180 * 640 + 320) * 4, 4);
	Check(std::abs(Half(fg_center[0]) + 8) < 1.5f && std::abs(Half(fg_center[1]) + 4) < 1.5f,
	      "FG-only preparation lost translated motion");
	Clear(scheduler.Current(), source, {0, 0, 0, 1});
	auto cut = generator.Prepare(scheduler.Current(), source, {640, 360});
	Check(cut && cut->bias_current_color, "scene-cut rejection mask missing");
	const auto mask = Readback(scheduler, cut->bias_current_color->backing, 4);
	float      center_bias;
	std::memcpy(&center_bias, mask.data() + (180 * 640 + 320) * 4, 4);
	Check(center_bias > .9f, "scene cut reuses mismatched history");
	generator.Reset();
	Check(generator.Prepare(scheduler.Current(), source, {640, 360})->reset_history,
	      "explicit history reset");
	Check(generator.Prepare(scheduler.Current(), source, {320, 180})->reset_history,
	      "resize history reset");
	const auto original_format = source.backing.format;
	source.backing.format      = vk::Format::eR8G8B8A8Uint;
	Check(!generator.Prepare(scheduler.Current(), source, {320, 180}),
	      "integer color sampler accepted");
	source.backing.format = original_format;
	Check(!generator.Prepare(scheduler.Current(), source, {0, 180}), "zero input extent accepted");
	scheduler.Finish();
}

void GeometryMotionCase(GraphicContext& graphics, RenderContext& renderer) {
	using namespace ShaderRecompiler;
	auto&                  scheduler = renderer.GetCommandScheduler();
	auto&                  motion    = renderer.GetGeometryMotion();
	constexpr vk::Extent2D extent {64, 64};
	Image      color(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, extent));
	Image      reference(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, extent));
	Image      depth_target(graphics, scheduler, Description(vk::Format::eD32Sfloat, extent));
	const auto vop1 = [](uint32_t op, uint32_t dst, uint32_t src) {
		return 0x7e000000u | (dst << 17) | (op << 9) | src;
	};
	const auto vop2 = [](uint32_t op, uint32_t dst, uint32_t src, uint32_t vgpr) {
		return (op << 25) | (dst << 17) | (vgpr << 9) | src;
	};
	std::vector<uint32_t> vertex_code;
	// A full-screen triangle from the guest vertex ID. s0 moves its geometry;
	// its uniform color gives image matching no signal for this movement.
	vertex_code.push_back(vop2(0x1b, 1, 129, 5));
	vertex_code.push_back(vop2(0x16, 2, 129, 5));
	for (uint32_t reg: {1u, 2u}) {
		vertex_code.push_back(vop1(6, reg, 256 + reg));
		vertex_code.push_back(vop2(8, reg, 246, reg)); // inline 4.0
		vertex_code.push_back(vop2(3, reg, 243, reg)); // inline -1.0
	}
	vertex_code.push_back(vop2(3, 1, 0, 1));
	vertex_code.insert(vertex_code.end(), {vop1(1, 3, 1), vop1(1, 4, 242)});
	vertex_code.insert(vertex_code.end(), {0xf80008cfu, 0x04030201u, 0xbf810000u});
	std::vector<uint32_t> pixel_code;
	for (uint32_t i = 0; i < 4; ++i) {
		pixel_code.push_back(vop1(1, i, 255));
		pixel_code.push_back(std::bit_cast<uint32_t>(i == 3 ? 1.f : .25f * (i + 1)));
	}
	pixel_code.insert(pixel_code.end(), {0xf800180fu, 0x03020100u, 0xbf810000u});
	std::array<uint32_t, 64>        user_data {};
	std::array<CompileResult, 4>    compiled;
	std::array<vk::ShaderModule, 4> modules {};
	for (uint32_t i = 0; i < 4; ++i) {
		ShaderVertexInputInfo vertex {};
		ShaderPixelInputInfo  pixel {};
		if (i < 2) vertex.geometry_motion_dword = pixel.geometry_motion_dword = 0;
		CompileOptions options;
		options.stage     = i % 2 ? ShaderType::Pixel : ShaderType::Vertex;
		options.user_data = user_data;
		if (i % 2)
			options.input_info.pixel = &pixel;
		else
			options.input_info.vertex = &vertex;
		compiled[i] = CompileProgram(TranslateProgram(i % 2 ? pixel_code : vertex_code, options),
		                             options, {}, 14);
		spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
		validator.SetMessageConsumer(
		    [](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
			    std::fprintf(stderr, "geometry SPIR-V: %s\n", message);
		    });
		Check(validator.Validate(compiled[i].spirv),
		      "translated geometry shader SPIR-V validation");
		vk::ShaderModuleCreateInfo create {};
		create.codeSize = compiled[i].spirv.size() * 4;
		create.pCode    = compiled[i].spirv.data();
		RequireVulkanSuccess(graphics.device.createShaderModule(&create, nullptr, &modules[i]),
		                     "geometry shader module");
	}
	vk::PushConstantRange range {};
	range.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
	range.size       = 128;
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &range;
	vk::PipelineLayout layout;
	RequireVulkanSuccess(graphics.device.createPipelineLayout(&layout_info, nullptr, &layout),
	                     "geometry test layout");
	std::array<vk::Pipeline, 2> pipelines {};
	for (uint32_t i = 0; i < 2; ++i) {
		std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
		for (uint32_t stage = 0; stage < 2; ++stage) {
			stages[stage].stage =
			    stage ? vk::ShaderStageFlagBits::eFragment : vk::ShaderStageFlagBits::eVertex;
			stages[stage].module = modules[i * 2 + stage];
			stages[stage].pName  = "main";
		}
		std::array<vk::Format, 8> formats {};
		formats[0] = color.backing.format;
		if (i == 0) formats[7] = vk::Format::eR16G16B16A16Sfloat;
		vk::PipelineRenderingCreateInfo rendering {};
		rendering.colorAttachmentCount    = i == 0 ? 8 : 1;
		rendering.pColorAttachmentFormats = formats.data();
		rendering.depthAttachmentFormat   = vk::Format::eD32Sfloat;
		vk::PipelineVertexInputStateCreateInfo   input {};
		vk::PipelineInputAssemblyStateCreateInfo assembly {};
		assembly.topology = vk::PrimitiveTopology::eTriangleList;
		vk::PipelineViewportStateCreateInfo viewport {};
		viewport.viewportCount = viewport.scissorCount = 1;
		vk::PipelineRasterizationStateCreateInfo raster {};
		raster.polygonMode = vk::PolygonMode::eFill;
		raster.lineWidth   = 1;
		vk::PipelineMultisampleStateCreateInfo samples {};
		samples.rasterizationSamples = vk::SampleCountFlagBits::e1;
		vk::PipelineDepthStencilStateCreateInfo depth {};
		depth.depthTestEnable = depth.depthWriteEnable = true;
		depth.depthCompareOp                           = vk::CompareOp::eLess;
		std::array<vk::PipelineColorBlendAttachmentState, 8> attachments {};
		attachments[0].colorWriteMask = attachments[7].colorWriteMask = vk::ColorComponentFlags(15);
		vk::PipelineColorBlendStateCreateInfo blend {};
		blend.attachmentCount = rendering.colorAttachmentCount;
		blend.pAttachments    = attachments.data();
		const vk::DynamicState             dynamic_states[] {vk::DynamicState::eViewport,
		                                                     vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic {};
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates    = dynamic_states;
		vk::GraphicsPipelineCreateInfo create {};
		create.pNext               = &rendering;
		create.stageCount          = 2;
		create.pStages             = stages.data();
		create.layout              = layout;
		create.pVertexInputState   = &input;
		create.pInputAssemblyState = &assembly;
		create.pViewportState      = &viewport;
		create.pRasterizationState = &raster;
		create.pMultisampleState   = &samples;
		create.pColorBlendState    = &blend;
		create.pDynamicState       = &dynamic;
		create.pDepthStencilState  = &depth;
		RequireVulkanSuccess(
		    graphics.device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipelines[i]),
		    "geometry test graphics pipeline");
	}
	EmulatorDlssInputs generator(graphics, scheduler);
	const uint64_t     key[] {1, 2, 3};
	auto render = [&](uint32_t pipeline, float shift, bool negative_height, uint32_t scale,
	                  bool indexed, uint32_t capacity = 3, bool inverted_depth = false,
	                  float z = .25f, bool preserve = false) {
		auto& command = scheduler.Current();
		command.EndRendering();
		auto push = pipeline == 0 ? motion.PrepareDraw(command, key, capacity, 4, 2)
		                          : std::array<uint32_t, 14> {};
		if (inverted_depth) push[7] |= 2;
		auto& target = pipeline == 0 ? color : reference;
		target.Transit(vk::ImageLayout::eColorAttachmentOptimal,
		               vk::AccessFlagBits2::eColorAttachmentWrite, {}, command.Handle());
		ImageViewInfo view {};
		view.format = target.backing.format;
		view.aspect = vk::ImageAspectFlagBits::eColor;
		view.usage  = vk::ImageUsageFlagBits::eColorAttachment;
		RenderState state {};
		state.width = state.height  = 64;
		state.num_color_attachments = 1;
		state.raster_scale_x = state.raster_scale_y = float(scale) / 100.f;
		state.color_attachments[0].image            = &target;
		state.color_attachments[0].image_view       = target.FindView(view);
		state.color_attachments[0].is_clear         = true;
		state.color_attachments[0].image_layout     = vk::ImageLayout::eColorAttachmentOptimal;
		depth_target.Transit(vk::ImageLayout::eDepthAttachmentOptimal,
		                     vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {},
		                     command.Handle());
		ImageViewInfo depth_view {};
		depth_view.format                         = depth_target.backing.format;
		depth_view.aspect                         = vk::ImageAspectFlagBits::eDepth;
		depth_view.usage                          = vk::ImageUsageFlagBits::eDepthStencilAttachment;
		state.depth_stencil_attachment.image      = &depth_target;
		state.depth_stencil_attachment.image_view = depth_target.FindView(depth_view);
		state.depth_stencil_attachment.image_layout = vk::ImageLayout::eDepthAttachmentOptimal;
		state.depth_stencil_attachment.has_depth    = state.depth_stencil_attachment.depth_clear =
		    true;
		state.depth_stencil_attachment.clear_value[0] = std::bit_cast<uint32_t>(.5f);
		if (pipeline == 0) Check(motion.Attach(command, state), "geometry test sidecar attachment");
		Buffer         indices(graphics, scheduler, MemoryUsage::Upload, 0,
		                       vk::BufferUsageFlagBits::eIndexBuffer, 24);
		const uint32_t index_data[] {0, 1, 2, 0, 1, 2};
		std::memcpy(indices.Mapped().data(), index_data, sizeof(index_data));
		indices.Flush(0, sizeof(index_data));
		command.BeginRendering(state, preserve);
		if (pipeline == 0) motion.BeginPass(command);
		const auto& effective = command.EffectiveRenderState();
		const float viewport_data[] {0,
		                             negative_height ? 1.f : 0.f,
		                             1,
		                             negative_height ? -1.f : 1.f,
		                             1.f / effective.width,
		                             1.f / effective.height};
		std::memcpy(push.data() + 8, viewport_data, sizeof(viewport_data));
		std::array<uint32_t, 32> constants {};
		std::copy(push.begin(), push.end(), constants.begin());
		const auto& bindings = compiled[pipeline * 2].program.bindings;
		for (size_t i = 0; i < bindings.user_data_registers.size(); ++i) {
			constants[bindings.push_data_start_dword + i] =
			    std::bit_cast<uint32_t>(bindings.user_data_registers[i] == 0 ? shift : z);
		}
		command.Handle().pushConstants(layout, range.stageFlags, 0, sizeof(constants),
		                               constants.data());
		vk::Viewport viewport {};
		viewport.width  = float(effective.width);
		viewport.height = float(effective.height);
		if (negative_height) {
			viewport.y      = viewport.height;
			viewport.height = -viewport.height;
		}
		viewport.maxDepth = 1;
		vk::Rect2D scissor {};
		scissor.extent = {effective.width, effective.height};
		command.Handle().setViewport(0, 1, &viewport);
		command.Handle().setScissor(0, 1, &scissor);
		command.Handle().bindPipeline(vk::PipelineBindPoint::eGraphics, pipelines[pipeline]);
		if (indexed) {
			command.Handle().bindIndexBuffer(indices.Handle(), 0, vk::IndexType::eUint32);
			command.Handle().drawIndexed(6, 2, 0, 0, 4);
		} else
			command.Handle().draw(3, 2, 0, 4);
		command.EndRendering();
		scheduler.Finish();
	};
	Check(!motion.SupportsSurface(color, {UINT32_MAX, UINT32_MAX}),
	      "unbounded guide allocation accepted");
	Check(motion.PrepareDraw(scheduler.Current(), key, UINT32_MAX, 0, UINT32_MAX)[4] == 0,
	      "overflowing vertex history allocation accepted");
	{
		GeometryMotion bounded(graphics, scheduler);
		constexpr uint32_t capacity = 64 * 1024 * 1024 / 2 / 20;
		const std::array<uint64_t, 1> first {1}, second {2}, rejected {3};
		Check(bounded.PrepareDraw(scheduler.Current(), first, capacity, 0, 1)[4] == capacity &&
		          bounded.PrepareDraw(scheduler.Current(), second, capacity, 0, 1)[4] == capacity,
		      "history budget fixture did not allocate its resident draws");
		Check(bounded.PrepareDraw(scheduler.Current(), rejected, 2, 0, 1)[4] == 0,
		      "draw history exceeded its GPU budget");
		Check(bounded.PrepareDraw(scheduler.Current(), first, 1, 0, 1)[4] == 1 &&
		          bounded.PrepareDraw(scheduler.Current(), rejected, 2, 0, 1)[4] == 2,
		      "refused draw retained metadata and became ambiguous without a guest flip");
		std::vector<uint64_t> oversized_key(4 * 1024 * 1024 / sizeof(uint64_t) + 1);
		Check(bounded.PrepareDraw(scheduler.Current(), oversized_key, 1, 0, 1)[4] == 0,
		      "unbounded draw identity was retained");
		std::vector<uint64_t> large_key(4 * 1024 * 1024 / 2 / sizeof(uint64_t) + 1);
		Check(bounded.PrepareDraw(scheduler.Current(), large_key, 1, 0, 1)[4] == 1,
		      "bounded draw identity was refused");
		large_key.back() = 1;
		Check(bounded.PrepareDraw(scheduler.Current(), large_key, 1, 0, 1)[4] == 0,
		      "draw identities exceeded their aggregate budget without a guest flip");
		for (int i = 0; i < 3; ++i) bounded.AdvanceFrame();
		Check(bounded.PrepareDraw(scheduler.Current(), large_key, 1, 0, 1)[4] == 1,
		      "retired identities did not release their metadata budget");
		scheduler.Finish();
	}
	for (bool inverted_depth: {false, true})
		for (bool negative_height: {false, true})
			for (uint32_t scale: {100u, 50u})
				for (bool indexed: {false, true}) {
					generator.Reset();
					motion.AdvanceFrame();
					motion.AdvanceFrame(); // disjoint fixture history
					render(0, 0, negative_height, scale, indexed, 3, inverted_depth);
					auto* guide = motion.Source(color);
					Check(guide != nullptr, "matching native color provenance");
					auto first =
					    generator.Prepare(scheduler.Current(), color, {32, 32}, true, guide);
					Check(first.has_value(), "native first frame preparation");
					const auto first_depth = Readback(scheduler, first->depth->backing, 4);
					float      depth;
					std::memcpy(&depth, first_depth.data() + (16 * 32 + 16) * 4, 4);
					Check(
					    std::abs(depth - (inverted_depth ? .75f : .25f)) < .001f,
					    "first frame discarded actual geometry depth or inverted-depth convention");
					const auto first_motion =
					    Readback(scheduler, first->motion_vectors->backing, 4);
					Check(std::all_of(first_motion.begin(), first_motion.end(),
					                  [](uint8_t v) { return v == 0; }),
					      "first geometry frame fabricated movement");
					motion.AdvanceFrame();
					render(0, .25f, negative_height, scale, indexed, 3, inverted_depth);
					guide = motion.Source(color);
					Check(guide != nullptr, "second frame lost native guide");
					auto second =
					    generator.Prepare(scheduler.Current(), color, {32, 32}, false, guide);
					Check(second.has_value(), "native second frame preparation");
					const auto vectors = Readback(scheduler, second->motion_vectors->backing, 4);
					uint16_t   center[2];
					std::memcpy(center, vectors.data() + (16 * 32 + 16) * 4, 4);
					Check(std::abs(Half(center[0]) + 4) < .02f && std::abs(Half(center[1])) < .02f,
					      "guest geometry motion did not survive viewport, raster scale, "
					      "instancing/indexing or DLSS pixel scaling");
					render(1, .25f, negative_height, scale, indexed);
					Check(Readback(scheduler, color.backing, 4) ==
					          Readback(scheduler, reference.backing, 4),
					      "geometry instrumentation changed guest color");
					color.Transit(vk::ImageLayout::eTransferDstOptimal,
					              vk::AccessFlagBits2::eTransferWrite, {},
					              scheduler.Current().Handle());
					Check(motion.Source(color) == nullptr,
					      "native guide survived unrelated color write");
				}
	motion.AdvanceFrame();
	render(0, .25f, false, 100, false, 0);
	auto fallback =
	    generator.Prepare(scheduler.Current(), color, {32, 32}, false, motion.Source(color));
	const auto fallback_depth = Readback(scheduler, fallback->depth->backing, 4);
	float      neutral;
	std::memcpy(&neutral, fallback_depth.data() + (16 * 32 + 16) * 4, 4);
	Check(neutral == .5f, "unsupported draw supplied fabricated geometry depth");
	motion.AdvanceFrame();
	render(0, 0, false, 100, false, 3, false, .75f);
	const auto occluded = Readback(scheduler, motion.Source(color)->backing, 8);
	Check(std::all_of(occluded.begin(), occluded.end(), [](uint8_t v) { return v == 0; }),
	      "depth-rejected fragments overwrote the motion guide");
	// The same geometry identity twice in one frame is ambiguous; the entire
	// previous guide must stop being available and next frame must reset it.
	Check(motion.PrepareDraw(scheduler.Current(), key, 3, 4, 2)[4] == 0 &&
	          motion.Source(color) == nullptr,
	      "ambiguous duplicate draw reused geometry history");
	motion.AdvanceFrame();
	Check((motion.PrepareDraw(scheduler.Current(), key, 3, 4, 2)[7] & 1) == 0,
	      "ambiguous previous frame remained valid");
	for (uint32_t scale: {100u, 50u}) {
		render(1, 0, false, scale, false);
		render(1, 0, false, scale, false, 3, false, .4f, true);
		const auto preserved = Readback(scheduler, depth_target.backing, 4);
		float      visible;
		std::memcpy(&visible, preserved.data() + (32 * 64 + 32) * 4, 4);
		Check(std::abs(visible - .25f) < .001f, "resumed capture pass repeated guest depth clear");
	}
	// Exercise the production shader cache too: instrumentation must be a
	// distinct permutation, and occupied guest interfaces must remain untouched.
	ShaderInit();
	ShaderUserData vertex_metadata {};
	const auto     vertex_address = reinterpret_cast<uint64_t>(vertex_code.data());
	const auto     pixel_address  = reinterpret_cast<uint64_t>(pixel_code.data());
	ShaderMapUserData(vertex_address, {.type            = Prospero::ShaderBinaryType::kGs,
	                                   .user_data       = &vertex_metadata,
	                                   .code_size_bytes = uint32_t(vertex_code.size() * 4)});
	ShaderMapUserData(pixel_address, {.type            = Prospero::ShaderBinaryType::kPs,
	                                  .code_size_bytes = uint32_t(pixel_code.size() * 4)});
	HW::VertexShaderInfo vertex_regs {};
	vertex_regs.es_regs.data_addr       = vertex_address;
	vertex_regs.gs_regs.rsrc2.user_sgpr = 2;
	vertex_regs.gs_user_sgpr.value[1]   = std::bit_cast<uint32_t>(.25f);
	HW::PixelShaderInfo pixel_regs {};
	pixel_regs.ps_regs.data_addr = pixel_address;
	HW::Context    registers {};
	HW::UserConfig user {};
	user.SetPrimitiveType(Prospero::PrimitiveType::kTriList);
	std::array<Prospero::ColorComponentMapping, 8> mapping {};
	std::array<ShaderVertexInputInfo, 3>           vertex_inputs {};
	ShaderPixelInputInfo                           pixel_inputs {};
	auto                                           programs = [&](bool enable) {
        return renderer.GetPipelineCache().GetGraphicsPrograms(
            vertex_regs, pixel_regs, registers.GetShaderRegisters(), registers, user, mapping, true,
            vertex_inputs, pixel_inputs, enable);
	};
	const auto native = programs(true);
	Check(vertex_inputs[0].geometry_motion_dword == 0 && pixel_inputs.geometry_motion_dword == 0,
	      "production shader cache did not enable geometry capture");
	const auto ordinary = programs(false);
	Check(vertex_inputs[0].geometry_motion_dword == UINT32_MAX &&
	          pixel_inputs.geometry_motion_dword == UINT32_MAX &&
	          ordinary.vertex[0].id != native.vertex[0].id && ordinary.pixel.id != native.pixel.id,
	      "instrumented shader permutation leaked into ordinary guest draws");
	const auto default_programs = renderer.GetPipelineCache().GetGraphicsPrograms(
	    vertex_regs, pixel_regs, registers.GetShaderRegisters(), registers, user, mapping, true,
	    vertex_inputs, pixel_inputs);
	Check(vertex_inputs[0].geometry_motion_dword == UINT32_MAX &&
	          pixel_inputs.geometry_motion_dword == UINT32_MAX &&
	          default_programs.vertex[0].id == ordinary.vertex[0].id &&
	          default_programs.pixel.id == ordinary.pixel.id,
	      "default shader lookup unexpectedly instrumented geometry motion");
	Check(programs(true).vertex[0].id == native.vertex[0].id,
	      "geometry shader cache failed to reuse its native permutation");
	auto occupied_code = vertex_code;
	occupied_code.insert(occupied_code.end() - 1, {0xf8000bafu, 0x04030201u}); // guest parameter 26
	vertex_regs.es_regs.data_addr = reinterpret_cast<uint64_t>(occupied_code.data());
	ShaderMapUserData(vertex_regs.es_regs.data_addr,
	                  {.type            = Prospero::ShaderBinaryType::kGs,
	                   .user_data       = &vertex_metadata,
	                   .code_size_bytes = uint32_t(occupied_code.size() * 4)});
	(void)programs(true);
	Check(vertex_inputs[0].geometry_motion_dword == UINT32_MAX &&
	          pixel_inputs.geometry_motion_dword == UINT32_MAX,
	      "geometry capture overwrote an occupied guest varying slot");
	motion.AdvanceFrame();
	scheduler.Finish();
	for (auto pipeline: pipelines)
		graphics.device.destroyPipeline(pipeline, nullptr);
	graphics.device.destroyPipelineLayout(layout, nullptr);
	for (auto module: modules)
		graphics.device.destroyShaderModule(module, nullptr);
	std::puts(
	    "Guest shader geometry motion/depth, indexed instancing, viewport/raster scale, reversed "
	    "depth, occlusion, color preservation, pass continuation and provenance cases passed");
}

void GeometryInputCase(GraphicContext& graphics, CommandScheduler& scheduler,
                       Config::ConfigOptions options) {
	options.upscale_motion = Config::UpscaleMotion::Geometry;
	Config::Load(options);
	EmulatorDlssInputs generator(graphics, scheduler);
	Image source(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {128, 64}));
	Image guide(graphics, scheduler, Description(vk::Format::eR16G16B16A16Sfloat, {128, 64}));
	Clear(scheduler.Current(), source, {.25f, .5f, .75f, 1});
	Clear(scheduler.Current(), guide, {-.125f, .0625f, .25f, 1});
	const vk::Extent2D extent {64, 32};
	auto               first = generator.Prepare(scheduler.Current(), source, extent, true, &guide);
	Check(first && first->reset_history, "geometry-only first frame did not reset");
	auto next = generator.Prepare(scheduler.Current(), source, extent, true, &guide);
	Check(next && !next->reset_history, "geometry-only history did not advance");
	const auto vectors = Readback(scheduler, next->motion_vectors->backing, 4);
	for (size_t i = 0; i < vectors.size(); i += 4) {
		uint16_t xy[2];
		std::memcpy(xy, vectors.data() + i, 4);
		Check(Half(xy[0]) == -8 && Half(xy[1]) == 2,
		      "geometry-only motion has incorrect sign or render-pixel scale");
	}
	Clear(scheduler.Current(), guide, {0, 0, 0, 0});
	auto uncovered = generator.Prepare(scheduler.Current(), source, extent, true, &guide);
	Check(uncovered.has_value(), "uncovered geometry-only frame failed");
	const auto bias = Readback(scheduler, uncovered->bias_current_color->backing, 4);
	for (size_t i = 0; i < bias.size(); i += 4) {
		float value;
		std::memcpy(&value, bias.data() + i, 4);
		Check(value == 1, "uncovered geometry-only pixels retain unvalidated history");
	}
	auto fg_only = generator.Prepare(scheduler.Current(), source, extent, false, &guide);
	Check(fg_only && !fg_only->reset_history && !fg_only->color && !fg_only->bias_current_color &&
	          fg_only->jitter_x == 0 && fg_only->jitter_y == 0,
	      "FG-only transition retained reconstruction inputs or discarded motion history");
	const auto resumed = generator.Prepare(scheduler.Current(), source, extent, true, &guide);
	Check(resumed && !resumed->reset_history && resumed->color && resumed->bias_current_color,
	      "SR re-enable discarded shared motion history or failed to allocate reconstruction inputs");
	options.upscale_motion = Config::UpscaleMotion::Hybrid;
	Config::Load(options);
	const auto changed = generator.Prepare(scheduler.Current(), source, extent);
	Check(changed && changed->reset_history, "motion-mode change reused incompatible history");
	scheduler.Finish(); // Retire commands before the fixture's source/guide images die.
	std::puts("Geometry-only input motion, rejection and mode-change cases passed");
}

void InputCostCase(GraphicContext& graphics, CommandScheduler& scheduler) {
	vk::QueryPoolCreateInfo create {};
	create.queryType  = vk::QueryType::eTimestamp;
	create.queryCount = 2;
	vk::QueryPool queries;
	RequireVulkanSuccess(graphics.device.createQueryPool(&create, nullptr, &queries),
	                     "input timing queries");
	for (const auto extent:
	     {vk::Extent2D {640, 360}, vk::Extent2D {1707, 960}, vk::Extent2D {2560, 1440}}) {
		for (const bool reconstruct: {true, false}) {
			for (const auto motion:
			     {Config::UpscaleMotion::Hybrid, Config::UpscaleMotion::Geometry}) {
				Config::ConfigOptions options;
				options.dlss_mode      = Config::DlssMode::Quality;
				options.upscale_motion = motion;
				Config::Load(options);
				EmulatorDlssInputs generator(graphics, scheduler);
				Image              source(graphics, scheduler,
				                          Description(vk::Format::eR8G8B8A8Unorm, {1920, 1080}));
				UploadPattern(scheduler, source, 0, 0);
				std::vector<double> samples;
				for (int frame = 0; frame < 10; ++frame) {
					auto handle = scheduler.Current().Handle();
					handle.resetQueryPool(queries, 0, 2);
					handle.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, queries, 0);
					Check(generator.Prepare(scheduler.Current(), source, extent, reconstruct)
					          .has_value(),
					      "input cost preparation");
					handle.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, queries, 1);
					scheduler.Finish();
					uint64_t ticks[2] {};
					RequireVulkanSuccess(
					    graphics.device.getQueryPoolResults(
					        queries, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t),
					        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait),
					    "input GPU timestamps");
					if (frame >= 3)
						samples.push_back(
						    (ticks[1] - ticks[0]) *
						    graphics.physical_device_properties.limits.timestampPeriod / 1e6);
				}
				std::sort(samples.begin(), samples.end());
				std::printf("Temporal input GPU median %ux%u (%s, %s): %.3f ms\n", extent.width,
				            extent.height, reconstruct ? "SR" : "FG only",
				            motion == Config::UpscaleMotion::Hybrid ? "Hybrid" : "Geometry",
				            samples[samples.size() / 2]);
			}
			std::fflush(stdout);
		}
	}
	graphics.device.destroyQueryPool(queries, nullptr);
}

void FinalFrameDetailCase(GraphicContext& graphics, CommandScheduler& scheduler,
                          DlssProcessor& dlss) {
	const vk::Extent2D extent {640, 360};
	Image              source(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, extent));
	Image         output(graphics, scheduler, Description(vk::Format::eR16G16B16A16Sfloat, extent));
	ImageViewInfo view {};
	view.format                    = output.backing.format;
	view.usage                     = vk::ImageUsageFlagBits::eStorage;
	const auto         output_view = output.FindView(view);
	EmulatorDlssInputs generator(graphics, scheduler);
	UploadPattern(scheduler, source, 0, 0, true);
	const auto reference     = Readback(scheduler, source.backing, 4);
	const auto input_extent  = dlss.OptimalInputExtent(extent, extent);
	double     squared_error = 0;
	for (int frame = 0; frame < 32; ++frame) {
		if (input_extent) {
			auto inputs = generator.Prepare(scheduler.Current(), source, *input_extent);
			Check(inputs &&
			          dlss.Evaluate(scheduler.Current(), *inputs, output.backing, output_view),
			      "detail NGX evaluation");
		} else {
			Check(generator.ResampleColor(scheduler.Current(), source, output.backing, output_view),
			      "detail native presentation");
		}
		const auto pixels = Readback(scheduler, output.backing, 8);
		if (frame >= 24)
			for (size_t pixel = 0; pixel < reference.size() / 4; ++pixel) {
				uint16_t red;
				std::memcpy(&red, pixels.data() + pixel * 8, 2);
				const double error = Half(red) - reference[pixel * 4] / 255.0;
				squared_error += error * error;
			}
	}
	const double rmse = std::sqrt(squared_error / (8 * extent.width * extent.height));
	std::printf("Headless final-frame detail RMSE: %.6f\n", rmse);
	Check(rmse < .08, "final-frame DLSS discarded already rendered thin detail");
	// Actual upscaling retains existing render samples, within NGX's legal
	// dynamic-resolution range; do not manufacture a smaller render first.
	const auto upscale = dlss.OptimalInputExtent({960, 540}, extent);
	Check(upscale.has_value(), "actual upscale was disabled");
	const auto recommended = dlss.OptimalInputExtent({960, 540});
	Check(recommended.has_value(), "SDK settings missing");
	const auto larger_source = dlss.OptimalInputExtent({960, 540}, {800, 450});
	Check(larger_source && larger_source->width >= recommended->width &&
	          larger_source->height >= recommended->height,
	      "higher-resolution rendered source lost samples before reconstruction");
}

void EvaluateCase(GraphicContext& graphics, RenderContext& renderer, DlssProcessor& dlss,
                  Config::DlssMode mode, vk::Extent2D output_size) {
	Config::ConfigOptions options;
	options.dlss_mode       = mode;
	options.upscale_backend = Config::GetUpscaleBackend();
	options.optiscaler_path = Config::GetOptiScalerPath();
	Config::Load(options);
	auto size = dlss.OptimalInputExtent(output_size);
	Check(size.has_value(), "SDK optimal resolution query");
	auto&       scheduler = renderer.GetCommandScheduler();
	Image       color(graphics, scheduler, Description(vk::Format::eR16G16B16A16Sfloat, *size));
	Image       depth(graphics, scheduler, Description(vk::Format::eR32Sfloat, *size));
	Image       motion(graphics, scheduler, Description(vk::Format::eR16G16Sfloat, *size));
	VulkanImage output;
	vk::ImageCreateInfo create {};
	create.imageType   = vk::ImageType::e2D;
	create.format      = vk::Format::eR16G16B16A16Sfloat;
	create.extent      = {output_size.width, output_size.height, 1};
	create.mipLevels   = 1;
	create.arrayLayers = 1;
	create.samples     = vk::SampleCountFlagBits::e1;
	create.tiling      = vk::ImageTiling::eOptimal;
	create.usage       = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
	               vk::ImageUsageFlagBits::eTransferDst;
	Check(graphics.CreateImage(create, output), "DLSS output allocation");
	vk::ImageViewCreateInfo view_create {};
	view_create.image            = output.image;
	view_create.viewType         = vk::ImageViewType::e2D;
	view_create.format           = output.format;
	view_create.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::ImageView view;
	RequireVulkanSuccess(graphics.device.createImageView(&view_create, nullptr, &view),
	                     "DLSS test view");
	DlssFrameInputs inputs {&color, &depth, &motion};
	for (int frame = 0; frame < 3; ++frame) {
		auto& command = scheduler.Current();
		Clear(command, color, {0.25f, 0.5f, 0.75f, 1.0f});
		Clear(command, depth, {0.5f, 0, 0, 0});
		Clear(command, motion, {0, 0, 0, 0});
		inputs.jitter_x      = frame == 1 ? 0.25f : -0.25f;
		inputs.jitter_y      = frame == 1 ? -0.25f : 0.25f;
		inputs.reset_history = frame == 2;
		Check(dlss.Evaluate(command, inputs, output, view), "NGX production evaluation");
		scheduler.Finish();
	}
	// Invalid inputs must preserve availability and never evaluate NGX.
	auto& command = scheduler.Current();
	auto  invalid = inputs;
	invalid.depth = nullptr;
	Check(!dlss.Evaluate(command, invalid, output, view), "missing depth accepted");
	invalid          = inputs;
	invalid.jitter_x = std::numeric_limits<float>::quiet_NaN();
	Check(!dlss.Evaluate(command, invalid, output, view), "NaN jitter accepted");
	invalid                = inputs;
	invalid.motion_vectors = nullptr;
	Check(!dlss.Evaluate(command, invalid, output, view), "missing motion vectors accepted");
	const auto motion_width     = motion.backing.extent.width;
	motion.backing.extent.width = motion_width + 1;
	Check(!dlss.Evaluate(command, inputs, output, view), "mismatched motion dimensions accepted");
	motion.backing.extent.width = motion_width;
	const auto target_handle    = output.image;
	output.image                = color.backing.image;
	Check(!dlss.Evaluate(command, inputs, output, view), "aliased input/output accepted");
	output.image = target_handle;
	Check(dlss.Available(), "invalid frame disabled device support");
	// Exercise the same production backend with emulator-generated inputs only.
	EmulatorDlssInputs generator(graphics, scheduler);
	Image final_color(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {1280, 720}));
	for (int frame = 0; frame < 3; ++frame) {
		Clear(scheduler.Current(), final_color, {.3f, .6f, .9f, 1});
		auto generated = generator.Prepare(scheduler.Current(), final_color, *size);
		Check(generated.has_value(), "emulator-generated temporal inputs");
		Check(dlss.Evaluate(scheduler.Current(), *generated, output, view),
		      "NGX emulator-frame evaluation");
		scheduler.Finish();
	}

	VkBufferCreateInfo buffer_create {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer_create.size  = uint64_t(output_size.width) * output_size.height * 8;
	buffer_create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo allocation_create {};
	allocation_create.usage = VMA_MEMORY_USAGE_AUTO;
	allocation_create.flags =
	    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer          buffer     = VK_NULL_HANDLE;
	VmaAllocation     allocation = nullptr;
	VmaAllocationInfo allocation_info {};
	Check(vmaCreateBuffer(graphics.allocator, &buffer_create, &allocation_create, &buffer,
	                      &allocation, &allocation_info) == VK_SUCCESS,
	      "readback buffer allocation");
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eComputeShader;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eShaderWrite;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferRead;
	barrier.oldLayout           = vk::ImageLayout::eGeneral;
	barrier.newLayout           = vk::ImageLayout::eTransferSrcOptimal;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = output.image;
	barrier.subresourceRange    = view_create.subresourceRange;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.Handle().pipelineBarrier2(dependency);
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent      = output.extent;
	command.Handle().copyImageToBuffer(output.image, vk::ImageLayout::eTransferSrcOptimal, buffer,
	                                   1, &copy);
	scheduler.Finish();
	Check(vmaInvalidateAllocation(graphics.allocator, allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
	      "readback invalidation");
	const auto* pixels = static_cast<const uint16_t*>(allocation_info.pMappedData);
	const auto  center =
	    (uint64_t(output_size.height / 2) * output_size.width + output_size.width / 2) * 4;
	// Positive finite half floats, with brighter G/B than R in the source scene.
	Check(pixels[center] > 0 && pixels[center] < 0x7c00 && pixels[center + 1] > pixels[center] &&
	          pixels[center + 2] > pixels[center + 1] && pixels[center + 2] < 0x7c00,
	      "DLSS output is blank or invalid");
	vmaDestroyBuffer(graphics.allocator, buffer, allocation);
	graphics.device.destroyImageView(view, nullptr);
	graphics.DeleteImage(output);
	std::printf("DLSS mode %d: %ux%u -> %ux%u, native + emulator GPU output verified\n",
	            static_cast<int>(mode), size->width, size->height, output_size.width,
	            output_size.height);
}

void EmulatorPresentationCase(Presenter& presenter, Config::ConfigOptions config,
                              const char* regression = nullptr) {
	auto&                                renderer  = presenter.Renderer();
	auto&                                scheduler = renderer.GetCommandScheduler();
	auto&                                graphics  = scheduler.Graphics();
	vk::DebugUtilsMessengerCreateInfoEXT debug {};
	debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
	debug.messageType     = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
	                    vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
	debug.pfnUserCallback = DebugCallback;
	vk::DebugUtilsMessengerEXT messenger;
	if (config.vulkan_validation_enabled) {
		RequireVulkanSuccess(
		    graphics.instance.createDebugUtilsMessengerEXT(&debug, nullptr, &messenger),
		    "presentation validation messenger");
	}
	HW::Context    registers {};
	HW::UserConfig user {};
	HW::Shader     shaders {};
	scheduler.Begin(registers, user, shaders);
	auto info               = Description(vk::Format::eR8G8B8A8Srgb, {640, 360});
	info.guest_format       = Prospero::BufferFormat::k8_8_8_8Srgb;
	const auto mapping_size = (info.mip_layout[0].size + 16383) & ~uint64_t(16383);
	const auto memory       = Libs::LibKernel::Memory::AllocateRuntimeMemory(
        0x200200000ull, mapping_size, Common::VirtualMemory::Mode::ReadWrite,
        "DLSS presentation fixture");
	Check(memory != 0, "presentation guest image allocation");
	info.data = {memory, info.mip_layout[0].size};
	TextureCache::ImageDesc desc {};
	desc.info             = info;
	desc.type             = TextureCache::BindingType::VideoOut;
	desc.view_info.format = info.pixel_format;
	desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
	desc.view_info.usage  = vk::ImageUsageFlagBits::eTransferSrc;
	auto&      cache      = renderer.GetTextureCache();
	const auto id         = cache.FindImage(desc);
	auto&      source     = cache.GetImage(id);
	UploadPattern(scheduler, source, 0, 0);
	cache.MarkGpuWritten(id);
	if (config.dlss_frame_generation) {
		// The FSR path returns Vulkan images; XeSS generates inside its DXGI swapchain.
		if (config.upscale_backend == Config::UpscaleBackend::OptiScaler &&
		    config.optiscaler_frame_generation == Config::OptiScalerFrameGeneration::Fsr)
			OptiScalerFramePixelsCase(graphics, scheduler);
		Check(presenter.FrameGeneration().Available(),
		      "Frame Generation unavailable on test device");
		const auto windows = SDL_GetWindows(nullptr);
		Check(windows && windows[0], "Frame Generation test window");
		auto*      test_window = windows[0];
		const auto hwnd        = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(test_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
		Check(hwnd != nullptr, "Frame Generation HWND");
		SDL_free(windows);
		const auto focus_test_window = [&] {
			if (GetForegroundWindow() == hwnd) return;
			const auto current    = GetCurrentThreadId();
			const auto foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
			const auto target     = GetWindowThreadProcessId(hwnd, nullptr);
			const bool attached_foreground =
			    foreground && foreground != current && AttachThreadInput(current, foreground, TRUE);
			const bool attached_target = target && target != current && target != foreground &&
			                             AttachThreadInput(current, target, TRUE);
			ShowWindow(hwnd, SW_RESTORE);
			BringWindowToTop(hwnd);
			SetForegroundWindow(hwnd);
			SetActiveWindow(hwnd);
			SetFocus(hwnd);
			if (attached_target) AttachThreadInput(current, target, FALSE);
			if (attached_foreground) AttachThreadInput(current, foreground, FALSE);
		};
		const auto report_focus = [&] {
			std::fprintf(stderr, "FG test focus: expected %p, foreground %p, available %d\n",
			             static_cast<void*>(hwnd), static_cast<void*>(GetForegroundWindow()),
			             presenter.FrameGeneration().Available());
		};
		const auto prepare_focused_frame = [&]() -> Presenter::Frame& {
			// Preparation can wait for GPU ownership after restoring focus. The
			// API runner may steal focus during that wait; retire and retry that
			// uncaptured fixture without counting it as a submitted guest frame.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (std::chrono::steady_clock::now() < deadline) {
				focus_test_window();
				SDL_PumpEvents();
				if (!presenter.FrameGeneration().Foreground()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				auto& frame = presenter.PrepareFrame(scheduler.Current(), info);
				if (frame.fg_inputs || !presenter.FrameGeneration().Available()) return frame;
				scheduler.FlushAndWait();
				presenter.Discard(frame);
				SDL_PumpEvents();
			}
			report_focus();
			Check(false, "could not capture FG inputs in the focused test window");
			std::abort();
		};
		const auto check_cached = [&] {
			// External FG defers the real frame; the present thread would show it first.
			presenter.PresentDeferred();
			const auto submitted = presenter.FrameGeneration().TotalSubmittedFrames();
			const auto presented = presenter.FrameGeneration().TotalPresentedFrames();
			Check(presenter.PresentLastFrame(), "cached guest presentation failed");
			Check(presenter.FrameGeneration().Enabled(),
			      "cached guest frame disabled Frame Generation");
			Check(presenter.FrameGeneration().Available(),
			      "cached guest frame invalidated Frame Generation");
			Check(presenter.FrameGeneration().TotalSubmittedFrames() == submitted,
			      "cached presentation advanced Frame Generation history");
			Check(presenter.FrameGeneration().TotalPresentedFrames() == presented,
			      "cached presentation inflated generated-frame statistics");
		};
		constexpr int count           = 180;
		int           deferred_frames = 0;
		for (int index = 0; index < count; ++index) {
			// Input snapshots are only prepared for a foreground game. Restore
			// focus before preparation as well as before SDK presentation.
			focus_test_window();
			UploadPattern(scheduler, source, index, 0);
			cache.MarkGpuWritten(id);
			auto& frame = prepare_focused_frame();
			Check(frame.fg_inputs && frame.fg_inputs->motion && frame.fg_inputs->depth,
			      "Frame Generation inputs were not captured per frame");
			if (regression && std::strcmp(regression, "fg-controlled") == 0) {
				// This fixture translates one source pixel per frame. Isolate the
				// runtime from optical-flow estimation using its exact backward motion.
				Clear(scheduler.Current(), *frame.fg_inputs->depth, {.5f, 0, 0, 0});
				Clear(scheduler.Current(), *frame.fg_inputs->motion,
				      {-float(frame.fg_inputs->motion->backing.extent.width) / 640.f, 0, 0, 0});
				frame.fg_inputs->jitter_x = frame.fg_inputs->jitter_y = 0;
				frame.fg_inputs->reset                                = index == 0;
			}
			scheduler.FlushAndWait();
			// DLSS-G deliberately bypasses interpolation for unfocused windows.
			// The API runner can take focus while reporting build output. Restore
			// only this test window before measuring the actual SDK presents.
			focus_test_window();
			presenter.Present(frame);
			deferred_frames += presenter.DeferredPresentTime() != 0;
			if (!presenter.FrameGeneration().Enabled()) report_focus();
			Check(presenter.FrameGeneration().Enabled(), "new guest frame lost Frame Generation");
			if (index == 60)
				check_cached(); // The next guest flip must keep the same generation mode.
			SDL_Event event {};
			while (SDL_PollEvent(&event)) {
			}
			// Leave room for an intermediate scanout even on a 60 Hz display.
			std::this_thread::sleep_for(std::chrono::milliseconds(33));
		}
		RequireVulkanSuccess(graphics.device.waitIdle(), "wait for generated frames");
		const auto presented = presenter.FrameGeneration().TotalPresentedFrames();
		const auto submitted = presenter.FrameGeneration().TotalSubmittedFrames();
		std::printf(
		    "Frame Generation: %llu display frames / %u actual submissions (%d guest frames)\n",
		    static_cast<unsigned long long>(presented), submitted, count);
		std::fflush(stdout);
		interpolation_succeeded = presented > submitted;
		// The guest flip completes with the generated frame; the real one follows later.
		Check(presenter.FrameGeneration().External() == (deferred_frames > 0),
		      "only external Frame Generation defers the real frame");
		for (int repeat = 0; repeat < 3; ++repeat) {
			check_cached();
		}
		// Off resumes ordinary presentation; re-enable prepares a fresh history.
		config.dlss_frame_generation = false;
		Config::Load(config);
		Check(presenter.PresentLastFrame(), "Frame Generation disable broke cached presentation");
		Check(!presenter.FrameGeneration().Enabled(),
		      "Frame Generation remained enabled after Off");
		config.dlss_frame_generation = true;
		Config::Load(config);
		const auto display_before     = presenter.FrameGeneration().TotalPresentedFrames();
		const auto submissions_before = presenter.FrameGeneration().TotalSubmittedFrames();
		// Re-enable and change output size while previously tagged resources exist.
		SDL_SetWindowSize(test_window, 1360, 768);
		SDL_RaiseWindow(test_window);
		for (int index = count; index < count + 45; ++index) {
			UploadPattern(scheduler, source, index, 0);
			cache.MarkGpuWritten(id);
			auto& next = prepare_focused_frame();
			Check(next.fg_inputs != nullptr, "re-enabled Frame Generation lost inputs");
			if (config.dlss_mode == Config::DlssMode::Off) {
				Check(next.fg_inputs->jitter_x == 0 && next.fg_inputs->jitter_y == 0,
				      "unjittered Off color carries reconstruction jitter");
			}
			scheduler.FlushAndWait();
			focus_test_window();
			presenter.Present(next);
			SDL_Event event {};
			while (SDL_PollEvent(&event)) {
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(33));
		}
		RequireVulkanSuccess(graphics.device.waitIdle(), "wait for re-enabled generation");
		const auto new_display =
		    presenter.FrameGeneration().TotalPresentedFrames() - display_before;
		const auto new_submissions =
		    presenter.FrameGeneration().TotalSubmittedFrames() - submissions_before;
		std::printf(
		    "Frame Generation re-enable/resize: %llu display frames / %u actual submissions\n",
		    static_cast<unsigned long long>(new_display), new_submissions);
		interpolation_succeeded = interpolation_succeeded && new_display > new_submissions;
		Check(presenter.FrameGeneration().Enabled(), "Frame Generation did not re-enable");
		auto& blank = presenter.PrepareBlankFrame(1360, 768, true);
		presenter.Present(blank);
		Check(!presenter.FrameGeneration().Enabled(), "blank frame did not disable generation");
		Check(presenter.PresentLastFrame(), "cached blank presentation failed");
		Check(!presenter.FrameGeneration().Enabled(), "cached blank frame re-enabled generation");
		auto& resumed = prepare_focused_frame();
		scheduler.FlushAndWait();
		presenter.Present(resumed);
		Check(presenter.FrameGeneration().Enabled(),
		      "guest frame did not resume generation after blank");
		// A missing native depth input must retire a reused snapshot and present normally.
		DlssFrameInputs missing {};
		for (int index = 0; index < 6; ++index) {
			auto& invalid = presenter.PrepareFrame(scheduler.Current(), info, &missing);
			Check(!invalid.fg_inputs, "invalid input reused stale Frame Generation snapshot");
			scheduler.FlushAndWait();
			presenter.Present(invalid);
		}
		Check(!presenter.FrameGeneration().Enabled(), "invalid inputs did not disable generation");
		graphics.instance.destroyDebugUtilsMessengerEXT(messenger, nullptr);
		Check(validation_errors == 0, "Frame Generation Vulkan validation errors");
		return;
	}
	if (regression != nullptr) {
		const auto original = Readback(scheduler, source.backing, 4);
		if (std::strcmp(regression, "spatial") == 0) {
			if (config.upscale_backend == Config::UpscaleBackend::Native)
				config.dlss_mode = Config::DlssMode::Off;
			config.render_scale_percent = 50;
			Config::Load(config);
			RenderState state {};
			state.width                 = 640;
			state.height                = 360;
			state.num_color_attachments = 1;
			state.raster_scale_x = state.raster_scale_y = .5f;
			auto& attachment                            = state.color_attachments[0];
			attachment.image                            = &source;
			attachment.image_layout                     = vk::ImageLayout::eColorAttachmentOptimal;
			attachment.is_clear                         = true;
			attachment.clear_value = {std::bit_cast<uint32_t>(.25f), std::bit_cast<uint32_t>(.5f),
			                          0, std::bit_cast<uint32_t>(1.f)};
			ImageViewInfo view {};
			view.format           = source.backing.format;
			view.usage            = vk::ImageUsageFlagBits::eColorAttachment;
			attachment.image_view = source.FindView(view);
			source.Transit(attachment.image_layout, vk::AccessFlagBits2::eColorAttachmentWrite, {},
			               scheduler.Current().Handle());
			cache.MarkGpuWritten(id);
			scheduler.Current().BeginRendering(state);
			auto& frame = presenter.PrepareFrame(scheduler.Current(), info);
			// 50% of the 960x540 output is 480x270, from a 640x360 guest surface.
			const auto [scale_x, scale_y] = presenter.Renderer().GetRasterScale();
			Check(scale_x == .75f && scale_y == .75f, "render scale is not a share of the output");
			Check(!frame.dlss_evaluated && frame.image.extent.width == 320 &&
			          frame.image.extent.height == 180,
			      "spatial presentation did not copy the actual reduced "
			      "raster directly");
			const auto copied = Readback(scheduler, frame.image, 4);
			Check(copied[0] > 120 && copied[1] > copied[0] && copied[2] == 0 && copied[3] == 255,
			      "direct reduced spatial copy lost encoded color");
			presenter.Discard(frame);
			auto& overlay = presenter.PrepareFrame(scheduler.Current(), info, nullptr, false);
			Check(overlay.image.extent.width == 640 && overlay.image.extent.height == 360,
			      "a separate overlay inherited the scene render scale");
			presenter.Discard(overlay);
		} else if (std::strcmp(regression, "detail") == 0) {
			config.screen_width  = 640;
			config.screen_height = 360;
			Config::Load(config);
			UploadPattern(scheduler, source, 0, 0, true);
			cache.MarkGpuWritten(id);
			const auto reference     = Readback(scheduler, source.backing, 4);
			double     squared_error = 0;
			for (int index = 0; index < 32; ++index) {
				auto&      frame  = presenter.PrepareFrame(scheduler.Current(), info);
				const auto output = Readback(scheduler, frame.image, frame.dlss_evaluated ? 8 : 4);
				if (index >= 24) {
					for (size_t pixel = 0; pixel < reference.size() / 4; ++pixel) {
						float actual;
						if (frame.dlss_evaluated) {
							uint16_t red;
							std::memcpy(&red, output.data() + pixel * 8, 2);
							actual = Half(red);
						} else
							actual = output[pixel * 4] / 255.f;
						const double error = actual - reference[pixel * 4] / 255.0;
						squared_error += error * error;
					}
				}
				presenter.Discard(frame);
			}
			const double rmse = std::sqrt(squared_error / (8 * 640 * 360));
			std::printf("Final-frame detail RMSE (Quality, native "
			            "output): %.6f\n",
			            rmse);
			std::fflush(stdout);
			Check(rmse < .08, "final-frame DLSS discarded already "
			                  "rendered thin detail");
		} else if (std::strcmp(regression, "srgb") == 0) {
			config.dlss_mode = Config::DlssMode::Off;
			Config::Load(config);
			for (bool main_bus: {true, false}) {
				auto& copied = presenter.PrepareFrame(scheduler.Current(), info, nullptr, main_bus);
				Check(!copied.dlss_evaluated, "Off/overlay unexpectedly evaluated DLSS");
				Check(Readback(scheduler, copied.image, 4) == original,
				      "sRGB Off/overlay presentation changed encoded pixel "
				      "bytes");
				presenter.Discard(copied);
			}
			Image bgra(graphics, scheduler, Description(vk::Format::eB8G8R8A8Srgb, {37, 19}));
			UploadPattern(scheduler, bgra, 0, 0);
			const auto       bgra_bytes = Readback(scheduler, bgra.backing, 4);
			Presenter::Frame copied;
			copied.Configure(graphics, {37, 19}, vk::Format::eB8G8R8A8Unorm);
			copied.CopyFrom(scheduler.Current(), bgra);
			Check(Readback(scheduler, copied.image, 4) == bgra_bytes,
			      "BGRA sRGB copy changed encoded pixel bytes");
			graphics.DeleteImage(copied.image);
		} else if (std::strcmp(regression, "fallback") == 0) {
			DlssFrameInputs invalid {};
			auto&           fallback = presenter.PrepareFrame(scheduler.Current(), info, &invalid);
			Check(!fallback.dlss_evaluated, "invalid inputs did not fall back");
			Check(fallback.image.format == vk::Format::eR16G16B16A16Sfloat,
			      "fallback did not retain the reconstruction output");
			const auto output = Readback(scheduler, fallback.image, 8);
			CheckEncodedResample(original, {640, 360}, output, {960, 540});
			Image bgra(graphics, scheduler, Description(vk::Format::eB8G8R8A8Srgb, {37, 19}));
			UploadPattern(scheduler, bgra, 0, 0);
			const auto         bgra_bytes = Readback(scheduler, bgra.backing, 4);
			EmulatorDlssInputs generator(graphics, scheduler);
			Check(generator.ResampleColor(scheduler.Current(), bgra, fallback.image, fallback.view),
			      "BGRA sRGB fallback rejected");
			CheckEncodedResample(bgra_bytes, {37, 19}, Readback(scheduler, fallback.image, 8),
			                     {960, 540}, true);
			presenter.Discard(fallback);
		} else if (std::strcmp(regression, "pool") == 0) {
			auto& blank   = presenter.PrepareBlankFrame(960, 540, true);
			auto& main    = presenter.PrepareFrame(scheduler.Current(), info);
			auto& overlay = presenter.PrepareFrame(scheduler.Current(), info, nullptr, false);
			scheduler.Finish();
			const auto main_image    = main.image.image;
			const auto overlay_image = overlay.image.image;
			presenter.Discard(overlay);
			presenter.Discard(main);
			for (int repeat = 0; repeat < 4; ++repeat) {
				auto& next_main = presenter.PrepareFrame(scheduler.Current(), info);
				Check(&next_main == &main && next_main.image.image == main_image,
				      "MAIN DLSS frame stole/reallocated an overlay image");
				auto& next_overlay =
				    presenter.PrepareFrame(scheduler.Current(), info, nullptr, false);
				Check(&next_overlay == &overlay && next_overlay.image.image == overlay_image,
				      "overlay frame stole/reallocated a DLSS image");
				scheduler.Finish();
				presenter.Discard(next_overlay);
				presenter.Discard(next_main);
			}
			presenter.Discard(blank);
		} else {
			Check(false, "unknown presentation regression case");
		}
		scheduler.Finish();
		graphics.instance.destroyDebugUtilsMessengerEXT(messenger, nullptr);
		Check(validation_errors == 0, "regression Vulkan validation errors");
		std::printf("Presentation regression %s passed\n", regression);
		return;
	}
	// No scene adapter, depth or motion is passed to PrepareFrame.
	auto& frame = presenter.PrepareFrame(scheduler.Current(), info);
	Check(frame.dlss_evaluated, "ordinary VideoOut color does not activate emulator DLSS");
	Check(frame.image.format == vk::Format::eR16G16B16A16Sfloat &&
	          frame.image.extent.width == 960 && frame.image.extent.height == 540,
	      "reconstructed frame format/output resolution");
	const auto pixels  = Readback(scheduler, frame.image, 8);
	float      minimum = 1, maximum = 0;
	for (size_t i = 0; i < pixels.size(); i += 8) {
		uint16_t red;
		std::memcpy(&red, pixels.data() + i, 2);
		const float value = Half(red);
		Check(std::isfinite(value), "non-finite reconstructed presentation pixel");
		minimum = std::min(minimum, value);
		maximum = std::max(maximum, value);
	}
	Check(maximum - minimum > .2f, "prepared DLSS output lost source texture");
	presenter.Present(frame);
	Check(presenter.PresentLastFrame(), "cached reconstructed frame cannot be presented");
	auto& overlay = presenter.PrepareFrame(scheduler.Current(), info, nullptr, false);
	Check(!overlay.dlss_evaluated && overlay.image.format == vk::Format::eR8G8B8A8Unorm &&
	          overlay.image.extent.width == 640,
	      "separate overlay incorrectly reconstructed");
	scheduler.Finish();
	presenter.Discard(overlay);
	// Resize/mode changes while producer work is queued must retire resources safely.
	for (auto mode: {Config::DlssMode::Performance, Config::DlssMode::DLAA, Config::DlssMode::Off,
	                 Config::DlssMode::Quality}) {
		config.dlss_mode     = mode;
		config.screen_width  = 1280;
		config.screen_height = 720;
		Config::Load(config);
		auto& next = presenter.PrepareFrame(scheduler.Current(), info);
		Check(next.dlss_evaluated == (mode != Config::DlssMode::Off),
		      "mode switching/fallback presentation");
		if (next.dlss_evaluated)
			Check(next.image.extent.width == 1280 && next.image.extent.height == 720,
			      "output resize");
		scheduler.Finish();
		presenter.Present(next);
	}
	Clear(scheduler.Current(), source, {.1f, .7f, .3f, 1});
	cache.MarkGpuWritten(id);
	auto& changed = presenter.PrepareFrame(scheduler.Current(), info);
	Check(changed.dlss_evaluated, "changed source lost DLSS");
	const auto new_pixels = Readback(scheduler, changed.image, 8);
	uint16_t   center[4];
	std::memcpy(center, new_pixels.data() + (360 * 1280 + 640) * 8, 8);
	Check(Half(center[1]) > Half(center[0]) + .3f,
	      "source scene cut not reflected in reconstructed pixels");
	presenter.Present(changed);
	presenter.ClearLayer(0);
	scheduler.Finish();
	graphics.instance.destroyDebugUtilsMessengerEXT(messenger, nullptr);
	Check(validation_errors == 0, "presentation Vulkan validation errors");
	std::puts("Emulator presentation: no adapter, actual GPU pixels, overlay, cached frame, modes "
	          "and resize passed");
}
void FrameGenerationExtensionFallbackCase(bool missing_private_data = false) {
	static vk::detail::DynamicLoader loader;
	const auto native = loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(native);
	DlssFrameGeneration fg;
	VULKAN_HPP_DEFAULT_DISPATCHER.init(fg.Initialize(native));
	Check(fg.Hooked(), "extension fallback test did not initialize Streamline");
	vk::ApplicationInfo app {};
	app.pApplicationName = "FrameGenerationExtensionFallback";
	app.apiVersion       = VULKAN_TARGET_API_VERSION;
	vk::InstanceCreateInfo instance_info {};
	instance_info.pApplicationInfo = &app;
	vk::Instance instance;
	RequireVulkanSuccess(fg.CreateInstance(instance_info, instance),
	                     "Streamline fallback instance");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
	const auto devices = EnumerateVulkan<vk::PhysicalDevice>(
	    "fallback physical devices", [&](uint32_t* n, vk::PhysicalDevice* p) {
		    return instance.enumeratePhysicalDevices(n, p);
	    });
	Check(!devices.empty(), "extension fallback test needs a Vulkan device");
	const auto physical  = devices.front();
	auto       available = EnumerateVulkan<vk::ExtensionProperties>(
        "fallback device extensions", [&](uint32_t* n, vk::ExtensionProperties* p) {
            return physical.enumerateDeviceExtensionProperties(nullptr, n, p);
        });
	std::vector<const char*> enabled;
	if (std::any_of(available.begin(), available.end(), [](const auto& extension) {
		    return std::strcmp(extension.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
	    })) {
		Check(fg.ConfigureDeviceExtensions(available, enabled, true), "supported present_id rejected");
		Check(fg.ConfigureDeviceExtensions(available, enabled, true) && enabled.size() == 1,
		      "optional present_id was appended more than once");
	}
	// Mask the optional capability on the selected physical device. Keep the
	// real interposer-created instance to exercise native dispatcher restoration.
	if (!missing_private_data) {
		std::erase_if(available, [](const auto& extension) {
			return std::strcmp(extension.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
		});
	}
	enabled.clear();
	Check(!fg.ConfigureDeviceExtensions(available, enabled, !missing_private_data),
	      "missing optional FG capability did not fall back");
	Check(!fg.Hooked() && !fg.Available() && !fg.Enabled() && enabled.empty(),
	      "unsupported FG left Streamline requirements enabled");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(native);
	VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
	const auto queues = physical.getQueueFamilyProperties();
	uint32_t   family = UINT32_MAX;
	for (uint32_t i = 0; i < queues.size(); ++i) {
		if (queues[i].queueFlags & vk::QueueFlagBits::eGraphics) {
			family = i;
			break;
		}
	}
	Check(family != UINT32_MAX, "fallback graphics queue");
	const float               priority = 1;
	vk::DeviceQueueCreateInfo queue {};
	queue.queueFamilyIndex = family;
	queue.queueCount       = 1;
	queue.pQueuePriorities = &priority;
	vk::DeviceCreateInfo device_info {};
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos    = &queue;
	vk::Device device;
	RequireVulkanSuccess(physical.createDevice(&device_info, nullptr, &device),
	                     "native device after missing FG extension");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
	vk::Queue graphics_queue;
	device.getQueue(family, 0, &graphics_queue);
	Check(graphics_queue != nullptr, "native fallback queue creation");
	device.destroy(nullptr);
	instance.destroy(nullptr);
	std::puts("Frame Generation optional-extension fallback created a native Vulkan device");
}
} // namespace

int main(int argc, char** argv) {
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions config;
	config.dlss_mode = Config::DlssMode::Quality;
	const bool optiscaler_unavailable =
	    argc == 2 && std::strcmp(argv[1], "--optiscaler-unavailable") == 0;
	if (optiscaler_unavailable) {
		config.upscale_backend = Config::UpscaleBackend::OptiScaler;
		config.optiscaler_path = "__kyty_missing_runtime__/OptiScaler.dll";
	}
	const bool optiscaler_fg_gpu =
	    argc == 2 && std::strcmp(argv[1], "--optiscaler-frame-generation-gpu") == 0;
	const bool optiscaler_presentation =
	    argc == 2 && std::strcmp(argv[1], "--optiscaler-presentation") == 0;
	const bool optiscaler = optiscaler_fg_gpu || optiscaler_presentation ||
	                        (argc == 2 && std::strcmp(argv[1], "--optiscaler-gpu") == 0);
	if (optiscaler) {
		const auto* runtime_path = std::getenv("KYTY_OPTISCALER_TEST_DLL");
		if (!runtime_path || !*runtime_path) {
			std::puts("SKIP: set KYTY_OPTISCALER_TEST_DLL to the installed "
			          "OptiScaler DLL");
			return 77;
		}
		config.upscale_backend  = Config::UpscaleBackend::OptiScaler;
		config.printf_direction = Config::LogDirection::Console;
		config.optiscaler_path  = runtime_path;
		if (const auto* gpu_index = std::getenv("KYTY_OPTISCALER_TEST_GPU")) {
			char*      end   = nullptr;
			const long index = std::strtol(gpu_index, &end, 10);
			Check(end != gpu_index && *end == '\0' && index >= 0 && index <= INT32_MAX,
			      "invalid KYTY_OPTISCALER_TEST_GPU index");
			config.gpu_index = static_cast<int32_t>(index);
		}
	}

	const bool presentation_regression =
	    argc == 3 && std::strcmp(argv[1], "--presentation-regression") == 0;
	const bool fg_controlled =
	    argc == 2 && (std::strcmp(argv[1], "--frame-generation-controlled") == 0 ||
	                  std::strcmp(argv[1], "--frame-generation-controlled-off") == 0);
	const bool frame_generation_off =
	    argc == 2 && (std::strcmp(argv[1], "--frame-generation-off") == 0 ||
	                  std::strcmp(argv[1], "--frame-generation-controlled-off") == 0);
	const bool optiscaler_fg =
	    argc == 2 && std::strcmp(argv[1], "--optiscaler-frame-generation") == 0;
	if (optiscaler_fg || optiscaler_fg_gpu) {
		const auto* path = std::getenv("KYTY_OPTISCALER_TEST_DLL");
		if (!path) return 77;
		config.upscale_backend = Config::UpscaleBackend::OptiScaler;
		config.optiscaler_path = path;
		config.dlss_mode       = Config::DlssMode::Off;
	}
	const bool frame_generation = optiscaler_fg || fg_controlled || frame_generation_off ||
	                              (argc == 2 && std::strcmp(argv[1], "--frame-generation") == 0);
	const bool fg_validation_fallback =
	    argc == 2 && std::strcmp(argv[1], "--frame-generation-validation-fallback") == 0;
	const bool fg_extension_fallback =
	    argc == 2 && std::strcmp(argv[1], "--frame-generation-extension-fallback") == 0;
	config.dlss_frame_generation =
	    frame_generation || optiscaler_fg_gpu || fg_validation_fallback || fg_extension_fallback;
	if (frame_generation_off) config.dlss_mode = Config::DlssMode::Off;
	const bool emulator_presentation =
	    optiscaler_unavailable || optiscaler_presentation || presentation_regression ||
	    frame_generation || (argc == 2 && std::strcmp(argv[1], "--emulator-presentation") == 0);
	const bool window_device = emulator_presentation || fg_validation_fallback ||
	                           (argc == 2 && (std::strcmp(argv[1], "--window-device") == 0 ||
	                                          std::strcmp(argv[1], "--window-device-off") == 0));
	if (window_device) {
		config.printf_direction = Config::LogDirection::Console;
		config.vulkan_validation_enabled =
		    (emulator_presentation && !frame_generation) || fg_validation_fallback;
		if (emulator_presentation) {
			config.screen_width  = 960;
			config.screen_height = 540;
		}
		if (frame_generation) {
			config.screen_width  = 1280;
			config.screen_height = 720;
		}
		if (std::strcmp(argv[1], "--window-device-off") == 0)
		config.dlss_mode = Config::DlssMode::Off;
	}
	if (optiscaler || optiscaler_fg) {
		if (const auto* algorithm = std::getenv("KYTY_OPTISCALER_TEST_UPSCALER")) {
			if (std::strcmp(algorithm, "Fsr") == 0)
				config.optiscaler_upscaler = Config::OptiScalerUpscaler::Fsr;
			else if (std::strcmp(algorithm, "XeSS") == 0)
				config.optiscaler_upscaler = Config::OptiScalerUpscaler::XeSS;
			else
				Check(std::strcmp(algorithm, "Auto") == 0, "invalid test upscaler");
		}
		if (const auto* generator = std::getenv("KYTY_OPTISCALER_TEST_FRAME_GENERATION")) {
			if (std::strcmp(generator, "XeSS") == 0)
				config.optiscaler_frame_generation = Config::OptiScalerFrameGeneration::XeSS;
			else
				Check(std::strcmp(generator, "Fsr") == 0, "invalid test frame generator");
		}
	}
	Config::Load(config);
	subsystems.Initialize<Log::Lifecycle>();
	if (fg_extension_fallback) {
		FrameGenerationExtensionFallbackCase();
		FrameGenerationExtensionFallbackCase(true);
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--window-title-timing") == 0) {
		Check(SDL_InitSubSystem(SDL_INIT_VIDEO), "SDL video initialization");
		WindowContext window;
		window.window = SDL_CreateWindow("title timing", 64, 64, SDL_WINDOW_HIDDEN);
		Check(window.window != nullptr, "hidden title-test window");
		window.UpdateTitle();
		const std::string first_title = SDL_GetWindowTitle(window.window);
		std::atomic<bool> finished    = false;
		std::thread       producer([&] {
            for (int i = 0; i < 120; ++i)
                window.UpdateTitle();
            finished.store(true, std::memory_order_release);
        });
		// Reproduce the presentation worker while the UI thread is busy.
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const bool finished_without_ui = finished.load(std::memory_order_acquire);
		while (!finished.load(std::memory_order_acquire)) {
			SDL_Event event {};
			(void)SDL_WaitEventTimeout(&event, 10);
		}
		producer.join();
		SDL_Event event {};
		while (SDL_PollEvent(&event)) {
		}
		Check(finished_without_ui, "frame title updates block on the UI thread");
		Check(first_title == SDL_GetWindowTitle(window.window), "title is changed on every frame");
		for (int i = 0; i < 20; ++i)
			window.UpdateTitle(false, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		finished.store(false, std::memory_order_release);
		std::thread due_update([&] {
			window.UpdateTitle();
			finished.store(true, std::memory_order_release);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const bool due_finished_without_ui = finished.load(std::memory_order_acquire);
		while (!finished.load(std::memory_order_acquire))
			(void)SDL_WaitEventTimeout(&event, 10);
		due_update.join();
		while (SDL_PollEvent(&event)) {
		}
		Check(due_finished_without_ui, "periodic FPS refresh still waits for the UI thread");
		Check(first_title != SDL_GetWindowTitle(window.window),
		      "title is not refreshed after FPS interval");
		Check(std::strstr(SDL_GetWindowTitle(window.window), "frame: 122,") != nullptr,
		      "cached/host frames inflate the gameplay FPS counter");
		Check(std::strstr(SDL_GetWindowTitle(window.window), "DLSS: inactive") != nullptr,
		      "a requested DLSS mode is incorrectly shown as evaluated");
		std::puts("Window title timing tests passed");
		return 0;
	}
	if (window_device) {
		subsystems.Initialize<Loader::Timer::Lifecycle>();
		subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();
		subsystems.Initialize<Profiler::Lifecycle>();
		subsystems.Initialize<Libs::Network::Lifecycle>();
	}
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	if (window_device) {
		subsystems.Initialize<Libs::LibKernel::FileSystem::Lifecycle>();
		subsystems.Initialize<Libs::Controller::Lifecycle>();
		subsystems.Initialize<Libs::Audio::Lifecycle>();
		auto& presenter = WindowInit(frame_generation ? 1280 : 640, frame_generation ? 720 : 360);
		if (optiscaler_fg)
			Check(!presenter.FrameGeneration().Hooked(),
			      "OptiScaler FG still uses NVIDIA Streamline proxy");
		if (fg_validation_fallback)
			Check(!presenter.FrameGeneration().Available(),
			      "Frame Generation must retain validated Vulkan fallback");
		if (emulator_presentation)
			EmulatorPresentationCase(presenter, config,
			                         fg_controlled
			                             ? "fg-controlled"
			                             : (optiscaler_unavailable
			                                    ? "spatial"
			                                    : (presentation_regression ? argv[2] : nullptr)));
		std::puts("Production window Vulkan device created successfully");
		WindowShutdown();
		Check(interpolation_succeeded, "no intermediate DLSS frames were presented");
		return 0;
	}
	static vk::detail::DynamicLoader loader;
	VULKAN_HPP_DEFAULT_DISPATCHER.init(
	    loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
	GraphicContext graphics;
	auto           extensions = EnumerateVulkan<vk::ExtensionProperties>(
        "instance extensions", [](uint32_t* n, vk::ExtensionProperties* p) {
            return vk::enumerateInstanceExtensionProperties(nullptr, n, p);
        });
	std::vector<const char*> enabled;
	if (!optiscaler_fg_gpu)
		Check(AppendDlssInstanceExtensions(enabled, extensions), "DLSS instance extensions");
	enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	auto layers = EnumerateVulkan<vk::LayerProperties>(
	    "instance layers", [](uint32_t* n, vk::LayerProperties* p) {
		    return vk::enumerateInstanceLayerProperties(n, p);
	    });
	const char* validation     = "VK_LAYER_KHRONOS_validation";
	bool        has_validation = std::any_of(layers.begin(), layers.end(), [&](const auto& layer) {
        return std::strcmp(layer.layerName, validation) == 0;
    });
	vk::ApplicationInfo app {};
	app.pApplicationName = "DlssGpuTests";
	app.apiVersion       = VULKAN_TARGET_API_VERSION;
	vk::InstanceCreateInfo instance_create {};
	instance_create.pApplicationInfo        = &app;
	instance_create.enabledExtensionCount   = static_cast<uint32_t>(enabled.size());
	instance_create.ppEnabledExtensionNames = enabled.data();
	instance_create.enabledLayerCount       = has_validation ? 1 : 0;
	instance_create.ppEnabledLayerNames     = &validation;
	RequireVulkanSuccess(vk::createInstance(&instance_create, nullptr, &graphics.instance),
	                     "test Vulkan instance");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(graphics.instance);
	vk::DebugUtilsMessengerCreateInfoEXT debug {};
	debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
	debug.messageType     = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
	                    vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
	debug.pfnUserCallback = DebugCallback;
	RequireVulkanSuccess(
	    graphics.instance.createDebugUtilsMessengerEXT(&debug, nullptr, &graphics.debug_messenger),
	    "test validation messenger");
	const auto devices = EnumerateVulkan<vk::PhysicalDevice>(
	    "physical devices", [&](uint32_t* n, vk::PhysicalDevice* p) {
		    return graphics.instance.enumeratePhysicalDevices(n, p);
	    });
	for (size_t index = 0; index < devices.size(); ++index) {
		const auto device = devices[index];
		std::printf("GPU %zu: %s\n", index, device.getProperties().deviceName.data());
		if (!graphics.physical_device &&
		    (config.gpu_index >= 0 ? size_t(config.gpu_index) == index
		                           : optiscaler || device.getProperties().vendorID == 0x10de))
			graphics.physical_device = device;
	}
	if (!graphics.physical_device) {
		std::puts("SKIP: no requested GPU");
		return 77;
	}
	graphics.physical_device.getProperties(&graphics.physical_device_properties);
	vk::PhysicalDevicePushDescriptorPropertiesKHR push_properties {};
	vk::PhysicalDeviceProperties2                 properties {};
	properties.pNext = &push_properties;
	graphics.physical_device.getProperties2(&properties);
	graphics.max_push_descriptors = push_properties.maxPushDescriptors;
	graphics.physical_device.getMemoryProperties(&graphics.physical_device_memory_properties);
	const auto queues = graphics.physical_device.getQueueFamilyProperties();
	for (uint32_t i = 0; i < queues.size(); ++i) {
		const auto flags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
		if ((queues[i].queueFlags & flags) == flags) {
			graphics.queue_family = i;
			break;
		}
	}
	Check(graphics.queue_family != UINT32_MAX, "graphics/compute queue");
	const auto device_extensions = EnumerateVulkan<vk::ExtensionProperties>(
	    "device extensions", [&](uint32_t* n, vk::ExtensionProperties* p) {
		    return graphics.physical_device.enumerateDeviceExtensionProperties(nullptr, n, p);
	    });
	const bool has_barycentric =
	    std::any_of(device_extensions.begin(), device_extensions.end(), [](const auto& extension) {
		    return std::strcmp(extension.extensionName,
		                       VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0;
	    });
	enabled = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
	if (has_barycentric) enabled.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
	if (!optiscaler_fg_gpu) {
		graphics.dlss_extensions_enabled =
		    AppendDlssDeviceExtensions(graphics, enabled, device_extensions);
		Check(graphics.dlss_extensions_enabled, "DLSS device extensions");
	}
	const std::array priorities {1.0f, 1.0f};
	graphics.present_queue_index = queues[graphics.queue_family].queueCount > 1 ? 1u : 0u;
	vk::DeviceQueueCreateInfo queue {};
	queue.queueFamilyIndex = graphics.queue_family;
	queue.queueCount       = graphics.present_queue_index + 1;
	queue.pQueuePriorities = priorities.data();
	auto features11        = WindowContext::RequiredVulkan11Features();
	auto features12        = WindowContext::RequiredVulkan12Features();
	if (optiscaler) {
		vk::PhysicalDeviceVulkan12Features supported12 {};
		vk::PhysicalDeviceFeatures2        supported {};
		supported.pNext = &supported12;
		graphics.physical_device.getFeatures2(&supported);
		features12.shaderFloat16 = supported12.shaderFloat16;
	}
	auto features13  = WindowContext::RequiredVulkan13Features();
	features13.pNext = &features12;
	features12.pNext = &features11;
	vk::PhysicalDeviceFeatures features {};
	features.shaderInt64                    = VK_TRUE;
	features.shaderInt16                    = VK_TRUE;
	features.vertexPipelineStoresAndAtomics = VK_TRUE;
	features.independentBlend               = VK_TRUE;
	features.shaderClipDistance             = VK_TRUE;
	features.shaderCullDistance             = VK_TRUE;
	features.sampleRateShading              = VK_TRUE;
	graphics.sample_rate_shading_enabled    = true;
	vk::DeviceCreateInfo device_create {};
	device_create.pNext                   = &features13;
	device_create.pEnabledFeatures        = &features;
	device_create.queueCreateInfoCount    = 1;
	device_create.pQueueCreateInfos       = &queue;
	device_create.enabledExtensionCount   = static_cast<uint32_t>(enabled.size());
	device_create.ppEnabledExtensionNames = enabled.data();
	RequireVulkanSuccess(
	    graphics.physical_device.createDevice(&device_create, nullptr, &graphics.device),
	    "test Vulkan device");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(graphics.device);
	graphics.device.getQueue(graphics.queue_family, 0, &graphics.queue);
	graphics.device.getQueue(graphics.queue_family, graphics.present_queue_index,
	                         &graphics.present_queue);
	Check(graphics.CreateAllocator(), "VMA initialization");
	{
		RenderContext renderer(graphics);
		if (optiscaler_fg_gpu) {
			HW::Context    registers {};
			HW::UserConfig user {};
			HW::Shader     shaders {};
			auto&          scheduler = renderer.GetCommandScheduler();
			scheduler.Begin(registers, user, shaders);
			OptiScalerFramePixelsCase(graphics, scheduler);
			scheduler.Finish();
		} else {
			{
				DlssProcessor startup(graphics, renderer.GetCommandScheduler());
				Check(startup.Available(), "NGX initialization without a guest stream");
			}
			HW::Context    registers {};
			HW::UserConfig user {};
			HW::Shader     shaders {};
			auto&          scheduler = renderer.GetCommandScheduler();
			scheduler.Begin(registers, user, shaders);
			PresentationQueueCase(graphics, renderer);
			FrameGenerationCompletionCase(graphics);
			RasterScaleCase(graphics, scheduler);
			RasterTargetReuseCase(graphics, scheduler);
			EmulatorMotionCase(graphics, scheduler);
			GeometryInputCase(graphics, scheduler, config);
			if (has_barycentric)
				GeometryMotionCase(graphics, renderer);
			else
				std::puts("SKIP: guest shader fixture needs barycentric interpolation; temporal "
				          "output is still checked");
			{
				DlssProcessor dlss(graphics, scheduler);
				Check(dlss.Available(), "NGX capability check");
				if (argc == 2 && std::strcmp(argv[1], "--quality-cost") == 0) {
					FinalFrameDetailCase(graphics, scheduler, dlss);
					InputCostCase(graphics, scheduler);
				} else if (argc == 2 && std::strcmp(argv[1], "--input-cost") == 0) {
					InputCostCase(graphics, scheduler);
				} else {
					for (auto mode: {Config::DlssMode::Quality, Config::DlssMode::Balanced,
					                 Config::DlssMode::Performance,
					                 Config::DlssMode::UltraPerformance, Config::DlssMode::DLAA}) {
						EvaluateCase(graphics, renderer, dlss, mode, {1920, 1080});
					}
					EvaluateCase(graphics, renderer, dlss, Config::DlssMode::Quality, {1280, 720});
				}
			}
			scheduler.Finish(); // releases features and shuts down NGX after GPU completion
		}
	}
	RequireVulkanSuccess(graphics.device.waitIdle(), "test device idle");
	graphics.DestroyAllocator();
	graphics.device.destroy(nullptr);
	graphics.instance.destroyDebugUtilsMessengerEXT(graphics.debug_messenger, nullptr);
	graphics.instance.destroy(nullptr);
	Check(validation_errors == 0, "Vulkan validation errors");
	std::printf("DLSS GPU tests passed (validation layer: %s)\n",
	            has_validation ? "enabled" : "unavailable");
}
