// Opt-in integration test: evaluates the production DLSS backend on a real RTX
// device with controlled scene inputs and the emulator-wide presentation path.
#include "common/subsystems.h"
#include "common/logging/log.h"
#include "graphics/presentation/dlss.h"
#include "graphics/presentation/emulatorDlssInputs.h"
#include "graphics/presentation/window/presentationFrame.h"
#include "graphics/presentation/window/windowInternal.h"
#include "graphics/presentation/window.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "kernel/memory.h"
#include "kernel/fileSystem.h"
#include "kernel/pthread.h"
#include "loader/timer.h"
#include "common/profiler.h"
#include "libs/network.h"
#include "libs/controller.h"
#include "libs/audio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <chrono>
#include <thread>
#include <cmath>

using namespace Libs::Graphics;
namespace {
std::atomic<int> validation_errors = 0;
bool interpolation_succeeded = true;
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "DlssGpuTests: %s\n", message);
		std::fflush(stderr);
		std::_Exit(1);
	}
}
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
    vk::DebugUtilsMessageTypeFlagsEXT, const vk::DebugUtilsMessengerCallbackDataEXT* message, void*) {
	if (severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
		++validation_errors;
		std::fprintf(stderr, "Vulkan validation: %s\n", message->pMessage);
	}
	return VK_FALSE;
}

ImageInfo Description(vk::Format format, vk::Extent2D size) {
	ImageInfo info {};
	info.pixel_format = format;
	info.extent = {size.width, size.height, 1};
	info.pitch = size.width;
	info.bytes_per_block = format == vk::Format::eR16G16B16A16Sfloat ? 8 : 4;
	info.mip_layout[0] = {0, uint64_t(size.width) * size.height * info.bytes_per_block, size.width, size.height};
	return info;
}

std::vector<uint8_t> Readback(CommandScheduler& scheduler, VulkanImage& image, uint32_t bytes_per_pixel);

void RasterScaleCase(GraphicContext& graphics, CommandScheduler& scheduler) {
	for (const auto percent : {25u, 50u, 67u, 100u}) {
		auto& command = scheduler.Current();
		Image color(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {256, 128}));
		Image depth(graphics, scheduler, Description(vk::Format::eD32Sfloat, {256, 128}));
		RenderState state {};
		state.width = 256;
		state.height = 128;
		state.num_color_attachments = 1;
		state.raster_scale_percent = percent;
		auto& attachment = state.color_attachments[0];
		attachment.image = &color;
		attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
		attachment.is_clear = true;
		attachment.clear_value = {std::bit_cast<uint32_t>(.25f), std::bit_cast<uint32_t>(.5f), 0, std::bit_cast<uint32_t>(1.f)};
		ImageViewInfo view {};
		view.format = color.backing.format;
		view.usage = vk::ImageUsageFlagBits::eColorAttachment;
		attachment.image_view = color.FindView(view);
		auto& db = state.depth_stencil_attachment;
		db.image = &depth;
		db.image_layout = vk::ImageLayout::eDepthAttachmentOptimal;
		db.has_depth = db.depth_clear = true;
		db.clear_value[0] = std::bit_cast<uint32_t>(.75f);
		view.format = depth.backing.format;
		view.aspect = vk::ImageAspectFlagBits::eDepth;
		view.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
		db.image_view = depth.FindView(view);
		color.Transit(attachment.image_layout, vk::AccessFlagBits2::eColorAttachmentWrite, {}, command.Handle());
		depth.Transit(db.image_layout, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {}, command.Handle());
		command.BeginRendering(state);
		const auto& effective = command.EffectiveRenderState();
		Check(effective.width == 256 * percent / 100 && effective.height == 128 * percent / 100,
		      "render scale did not reduce actual raster attachments");
		vk::ClearAttachment clear {};
		clear.aspectMask = vk::ImageAspectFlagBits::eColor;
		clear.colorAttachment = 0;
		clear.clearValue.color.float32 = std::array<float, 4> {1.f, 0.f, 0.f, 1.f};
		vk::ClearRect rect {};
		rect.rect.extent = {effective.width / 2, effective.height};
		rect.layerCount = 1;
		command.Handle().clearAttachments(1, &clear, 1, &rect);
		// This consumer computes barriers before EndRendering. It must still see
		// the upscaled render result, with tracked layouts matching the real GPU.
		color.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {}, command.Handle());
		depth.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {}, command.Handle());
		const auto pixels = Readback(scheduler, color.backing, 4);
		Check(pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0, "scaled color not stored into guest image");
		const auto right = (256 * 64 + 240) * 4;
		Check(std::abs(int(pixels[right]) - 64) <= 1 && std::abs(int(pixels[right + 1]) - 128) <= 1,
		      "scaled load clear did not preserve unmodified pixels");
		// Exercise image retirement on a subsequent pass with a different size.
		scheduler.FlushAndWait();
	}
	std::puts("Raster scale GPU cases passed");
}

void Clear(CommandBuffer& command, Image& image, std::array<float, 4> value) {
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {}, command.Handle());
	vk::ClearColorValue clear {};
	clear.float32 = value;
	vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal, &clear, 1, &range);
}

std::vector<uint8_t> Readback(CommandScheduler& scheduler, VulkanImage& image, uint32_t bytes_per_pixel) {
	auto& graphics = scheduler.Graphics();
	VkBufferCreateInfo create {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	create.size = uint64_t(image.extent.width) * image.extent.height * bytes_per_pixel;
	create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo alloc {};
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	VmaAllocationInfo mapped {};
	Check(vmaCreateBuffer(graphics.allocator, &create, &alloc, &buffer, &allocation, &mapped) == VK_SUCCESS,
	      "emulator readback allocation");
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask = image.state.pl_stage;
	barrier.srcAccessMask = image.state.access_mask;
	barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
	barrier.oldLayout = image.state.layout;
	barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image.image;
	barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers = &barrier;
	auto& command = scheduler.Current();
	command.EndRendering();
	command.Handle().pipelineBarrier2(dependency);
	image.state = {barrier.dstStageMask, barrier.dstAccessMask, barrier.newLayout};
	image.subresource_states.clear();
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent = image.extent;
	command.Handle().copyImageToBuffer(image.image, barrier.newLayout, buffer, 1, &copy);
	scheduler.Finish();
	Check(vmaInvalidateAllocation(graphics.allocator, allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS, "readback invalidation");
	std::vector<uint8_t> result(create.size);
	std::memcpy(result.data(), mapped.pMappedData, result.size());
	vmaDestroyBuffer(graphics.allocator, buffer, allocation);
	return result;
}

float Half(uint16_t bits) {
	const int exponent = (bits >> 10) & 31;
	const int mantissa = bits & 1023;
	const float value = exponent == 0 ? std::ldexp(float(mantissa), -24) :
	                    exponent == 31 ? std::numeric_limits<float>::infinity() :
	                    std::ldexp(float(1024 + mantissa), exponent - 25);
	return bits & 0x8000 ? -value : value;
}

void CheckEncodedResample(const std::vector<uint8_t>& source, vk::Extent2D source_size,
                          const std::vector<uint8_t>& output, vk::Extent2D output_size,
                          bool bgra = false) {
	for (const auto& point : std::array<std::array<uint32_t, 2>, 3> {{{0, 0},
	         {output_size.width / 2, output_size.height / 2}, {output_size.width - 1, output_size.height - 1}}}) {
		const float x = std::clamp((point[0] + .5f) * source_size.width / output_size.width - .5f,
		                           0.f, float(source_size.width - 1));
		const float y = std::clamp((point[1] + .5f) * source_size.height / output_size.height - .5f,
		                           0.f, float(source_size.height - 1));
		const auto x0 = uint32_t(x), y0 = uint32_t(y);
		const auto x1 = std::min(x0 + 1, source_size.width - 1), y1 = std::min(y0 + 1, source_size.height - 1);
		for (size_t channel = 0; channel < 4; ++channel) {
			const auto source_channel = bgra && channel < 3 ? 2 - channel : channel;
			const auto sample = [&](uint32_t sx, uint32_t sy) {
				return source[(uint64_t(sy) * source_size.width + sx) * 4 + source_channel] / 255.f;
			};
			const float expected = std::lerp(std::lerp(sample(x0, y0), sample(x1, y0), x - x0),
			                                 std::lerp(sample(x0, y1), sample(x1, y1), x - x0), y - y0);
			uint16_t actual;
			std::memcpy(&actual, output.data() + (uint64_t(point[1]) * output_size.width + point[0]) * 8 + channel * 2, 2);
			Check(std::abs(Half(actual) - expected) < .003f,
			      "sRGB DLSS fallback changed encoded color or channel order");
		}
	}
}

void UploadPattern(CommandScheduler& scheduler, Image& image, int shift_x, int shift_y) {
	auto& graphics = scheduler.Graphics();
	const auto width = image.backing.extent.width, height = image.backing.extent.height;
	std::vector<uint8_t> pixels(uint64_t(width) * height * 4);
	for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
		const float px = float(x) - shift_x, py = float(y) - shift_y;
		const auto index = (uint64_t(y) * width + x) * 4;
		pixels[index] = uint8_t(128 + 55 * std::sin(px * .043f + py * .027f));
		pixels[index + 1] = uint8_t(128 + 55 * std::sin(px * .021f - py * .051f));
		pixels[index + 2] = uint8_t(128 + 55 * std::cos(px * .037f + py * .013f));
		pixels[index + 3] = 255;
	}
	VkBufferCreateInfo create {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	create.size = pixels.size();
	create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo alloc {};
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	VmaAllocationInfo mapped {};
	Check(vmaCreateBuffer(graphics.allocator, &create, &alloc, &buffer, &allocation, &mapped) == VK_SUCCESS,
	      "pattern upload allocation");
	std::memcpy(mapped.pMappedData, pixels.data(), pixels.size());
	Check(vmaFlushAllocation(graphics.allocator, allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS, "pattern upload flush");
	auto& command = scheduler.Current();
	command.EndRendering();
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {}, command.Handle());
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent = image.backing.extent;
	command.Handle().copyBufferToImage(buffer, image.backing.image, vk::ImageLayout::eTransferDstOptimal, 1, &copy);
	scheduler.Finish();
	vmaDestroyBuffer(graphics.allocator, buffer, allocation);
}

void EmulatorMotionCase(GraphicContext& graphics, CommandScheduler& scheduler) {
	EmulatorDlssInputs generator(graphics, scheduler);
	Image source(graphics, scheduler, Description(vk::Format::eR8G8B8A8Unorm, {1280, 720}));
	UploadPattern(scheduler, source, 0, 0);
	auto first = generator.Prepare(scheduler.Current(), source, {640, 360});
	Check(first && first->reset_history && first->jitter_x == 0 && first->jitter_y == 0, "first emulator frame/history");
	const auto zero_motion = Readback(scheduler, first->motion_vectors->backing, 4);
	Check(std::all_of(zero_motion.begin(), zero_motion.end(), [](uint8_t v) { return v == 0; }), "first frame has stale motion");
	// Warm the compute shaders before measuring consecutive-frame motion.
	Check(generator.Prepare(scheduler.Current(), source, {640, 360}).has_value(), "motion warmup");
	scheduler.Finish();
	auto stationary = generator.Prepare(scheduler.Current(), source, {640, 360});
	Check(stationary && !stationary->reset_history, "stationary history unexpectedly resets");
	const auto static_motion = Readback(scheduler, stationary->motion_vectors->backing, 4);
	Check(std::all_of(static_motion.begin(), static_motion.end(), [](uint8_t v) { return v == 0; }), "static scene has nonzero motion");
	UploadPattern(scheduler, source, 16, 8);
	auto moving = generator.Prepare(scheduler.Current(), source, {640, 360});
	Check(moving && !moving->reset_history, "moving frame lacks previous history");
	const auto motion = Readback(scheduler, moving->motion_vectors->backing, 4);
	std::vector<float> xs, ys;
	for (uint32_t y = 70; y < 290; y += 7) for (uint32_t x = 90; x < 550; x += 7) {
		uint16_t values[2];
		std::memcpy(values, motion.data() + (uint64_t(y) * 640 + x) * 4, 4);
		xs.push_back(Half(values[0])); ys.push_back(Half(values[1]));
	}
	std::sort(xs.begin(), xs.end()); std::sort(ys.begin(), ys.end());
	const auto mx = xs[xs.size() / 2], my = ys[ys.size() / 2];
	std::printf("Emulator motion median: %.3f, %.3f (expected -8, -4 render pixels)\n", mx, my);
	Check(std::abs(mx + 8) < 1.5f && std::abs(my + 4) < 1.5f, "GPU motion direction or render-pixel scale");
	Clear(scheduler.Current(), source, {0, 0, 0, 1});
	auto cut = generator.Prepare(scheduler.Current(), source, {640, 360});
	Check(cut && cut->bias_current_color, "scene-cut rejection mask missing");
	const auto mask = Readback(scheduler, cut->bias_current_color->backing, 4);
	float center_bias;
	std::memcpy(&center_bias, mask.data() + (180 * 640 + 320) * 4, 4);
	Check(center_bias > .9f, "scene cut reuses mismatched history");
	generator.Reset();
	Check(generator.Prepare(scheduler.Current(), source, {640, 360})->reset_history, "explicit history reset");
	Check(generator.Prepare(scheduler.Current(), source, {320, 180})->reset_history, "resize history reset");
	const auto original_format = source.backing.format;
	source.backing.format = vk::Format::eR8G8B8A8Uint;
	Check(!generator.Prepare(scheduler.Current(), source, {320, 180}), "integer color sampler accepted");
	source.backing.format = original_format;
	Check(!generator.Prepare(scheduler.Current(), source, {0, 180}), "zero input extent accepted");
	scheduler.Finish();
}

void EvaluateCase(GraphicContext& graphics, RenderContext& renderer, DlssProcessor& dlss,
                  Config::DlssMode mode, vk::Extent2D output_size) {
	Config::ConfigOptions options;
	options.dlss_mode = mode;
	Config::Load(options);
	auto size = dlss.OptimalInputExtent(output_size);
	Check(size.has_value(), "SDK optimal resolution query");
	auto& scheduler = renderer.GetCommandScheduler();
	Image color(graphics, scheduler, Description(vk::Format::eR16G16B16A16Sfloat, *size));
	Image depth(graphics, scheduler, Description(vk::Format::eR32Sfloat, *size));
	Image motion(graphics, scheduler, Description(vk::Format::eR16G16Sfloat, *size));
	VulkanImage output;
	vk::ImageCreateInfo create {};
	create.imageType = vk::ImageType::e2D;
	create.format = vk::Format::eR16G16B16A16Sfloat;
	create.extent = {output_size.width, output_size.height, 1};
	create.mipLevels = 1;
	create.arrayLayers = 1;
	create.samples = vk::SampleCountFlagBits::e1;
	create.tiling = vk::ImageTiling::eOptimal;
	create.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
	               vk::ImageUsageFlagBits::eTransferDst;
	Check(graphics.CreateImage(create, output), "DLSS output allocation");
	vk::ImageViewCreateInfo view_create {};
	view_create.image = output.image;
	view_create.viewType = vk::ImageViewType::e2D;
	view_create.format = output.format;
	view_create.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::ImageView view;
	RequireVulkanSuccess(graphics.device.createImageView(&view_create, nullptr, &view), "DLSS test view");
	DlssFrameInputs inputs {&color, &depth, &motion};
	for (int frame = 0; frame < 3; ++frame) {
		auto& command = scheduler.Current();
		Clear(command, color, {0.25f, 0.5f, 0.75f, 1.0f});
		Clear(command, depth, {0.5f, 0, 0, 0});
		Clear(command, motion, {0, 0, 0, 0});
		inputs.jitter_x = frame == 1 ? 0.25f : -0.25f;
		inputs.jitter_y = frame == 1 ? -0.25f : 0.25f;
		inputs.reset_history = frame == 2;
		Check(dlss.Evaluate(command, inputs, output, view), "NGX production evaluation");
		scheduler.Finish();
	}
	// Invalid inputs must preserve availability and never evaluate NGX.
	auto& command = scheduler.Current();
	auto invalid = inputs;
	invalid.depth = nullptr;
	Check(!dlss.Evaluate(command, invalid, output, view), "missing depth accepted");
	invalid = inputs;
	invalid.jitter_x = std::numeric_limits<float>::quiet_NaN();
	Check(!dlss.Evaluate(command, invalid, output, view), "NaN jitter accepted");
	invalid = inputs;
	invalid.motion_vectors = nullptr;
	Check(!dlss.Evaluate(command, invalid, output, view), "missing motion vectors accepted");
	const auto motion_width = motion.backing.extent.width;
	motion.backing.extent.width = motion_width + 1;
	Check(!dlss.Evaluate(command, inputs, output, view), "mismatched motion dimensions accepted");
	motion.backing.extent.width = motion_width;
	const auto target_handle = output.image;
	output.image = color.backing.image;
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
		Check(dlss.Evaluate(scheduler.Current(), *generated, output, view), "NGX emulator-frame evaluation");
		scheduler.Finish();
	}

	VkBufferCreateInfo buffer_create {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer_create.size = uint64_t(output_size.width) * output_size.height * 8;
	buffer_create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo allocation_create {};
	allocation_create.usage = VMA_MEMORY_USAGE_AUTO;
	allocation_create.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	VmaAllocationInfo allocation_info {};
	Check(vmaCreateBuffer(graphics.allocator, &buffer_create, &allocation_create, &buffer, &allocation, &allocation_info) == VK_SUCCESS, "readback buffer allocation");
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
	barrier.oldLayout = vk::ImageLayout::eGeneral;
	barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = output.image;
	barrier.subresourceRange = view_create.subresourceRange;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers = &barrier;
	command.Handle().pipelineBarrier2(dependency);
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.imageExtent = output.extent;
	command.Handle().copyImageToBuffer(output.image, vk::ImageLayout::eTransferSrcOptimal, buffer, 1, &copy);
	scheduler.Finish();
	Check(vmaInvalidateAllocation(graphics.allocator, allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS, "readback invalidation");
	const auto* pixels = static_cast<const uint16_t*>(allocation_info.pMappedData);
	const auto center = (uint64_t(output_size.height / 2) * output_size.width + output_size.width / 2) * 4;
	// Positive finite half floats, with brighter G/B than R in the source scene.
	Check(pixels[center] > 0 && pixels[center] < 0x7c00 &&
	      pixels[center + 1] > pixels[center] && pixels[center + 2] > pixels[center + 1] &&
	      pixels[center + 2] < 0x7c00, "DLSS output is blank or invalid");
	vmaDestroyBuffer(graphics.allocator, buffer, allocation);
	graphics.device.destroyImageView(view, nullptr);
	graphics.DeleteImage(output);
	std::printf("DLSS mode %d: %ux%u -> %ux%u, native + emulator GPU output verified\n", static_cast<int>(mode), size->width, size->height, output_size.width, output_size.height);
}

void EmulatorPresentationCase(Presenter& presenter, Config::ConfigOptions config,
                              const char* regression = nullptr) {
	auto& renderer = presenter.Renderer();
	auto& scheduler = renderer.GetCommandScheduler();
	auto& graphics = scheduler.Graphics();
	vk::DebugUtilsMessengerCreateInfoEXT debug {};
	debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
	debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
	debug.pfnUserCallback = DebugCallback;
	vk::DebugUtilsMessengerEXT messenger;
	if (config.vulkan_validation_enabled) {
		RequireVulkanSuccess(graphics.instance.createDebugUtilsMessengerEXT(&debug, nullptr, &messenger), "presentation validation messenger");
	}
	HW::Context registers {};
	HW::UserConfig user {};
	HW::Shader shaders {};
	scheduler.Begin(registers, user, shaders);
	auto info = Description(vk::Format::eR8G8B8A8Srgb, {640, 360});
	info.guest_format = Prospero::BufferFormat::k8_8_8_8Srgb;
	const auto mapping_size = (info.mip_layout[0].size + 16383) & ~uint64_t(16383);
	const auto memory = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0x200200000ull, mapping_size, Common::VirtualMemory::Mode::ReadWrite, "DLSS presentation fixture");
	Check(memory != 0, "presentation guest image allocation");
	info.data = {memory, info.mip_layout[0].size};
	TextureCache::ImageDesc desc {};
	desc.info = info;
	desc.type = TextureCache::BindingType::VideoOut;
	desc.view_info.format = info.pixel_format;
	desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
	desc.view_info.usage = vk::ImageUsageFlagBits::eTransferSrc;
	auto& cache = renderer.GetTextureCache();
	const auto id = cache.FindImage(desc);
	auto& source = cache.GetImage(id);
	UploadPattern(scheduler, source, 0, 0);
	cache.MarkGpuWritten(id);
	if (config.dlss_frame_generation) {
		Check(presenter.FrameGeneration().Available(), "Frame Generation unavailable on test device");
		const auto windows = SDL_GetWindows(nullptr);
		Check(windows && windows[0], "Frame Generation test window");
		auto* test_window = windows[0];
		const auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(test_window),
		    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
		Check(hwnd != nullptr, "Frame Generation HWND");
		SDL_free(windows);
		const auto check_cached = [&] {
			const auto submitted = presenter.FrameGeneration().TotalSubmittedFrames();
			const auto presented = presenter.FrameGeneration().TotalPresentedFrames();
			Check(presenter.PresentLastFrame(), "cached guest presentation failed");
			Check(presenter.FrameGeneration().Enabled(), "cached guest frame disabled Frame Generation");
			Check(presenter.FrameGeneration().Available(), "cached guest frame invalidated Frame Generation");
			Check(presenter.FrameGeneration().TotalSubmittedFrames() == submitted,
			      "cached presentation advanced Frame Generation history");
			Check(presenter.FrameGeneration().TotalPresentedFrames() == presented,
			      "cached presentation inflated generated-frame statistics");
		};
		constexpr int count = 180;
		for (int index = 0; index < count; ++index) {
			UploadPattern(scheduler, source, index, 0);
			cache.MarkGpuWritten(id);
			auto& frame = presenter.PrepareFrame(scheduler.Current(), info);
			Check(frame.fg_inputs && frame.fg_inputs->motion && frame.fg_inputs->depth,
			      "Frame Generation inputs were not captured per frame");
			if (regression && std::strcmp(regression, "fg-controlled") == 0) {
				// This fixture translates one source pixel per frame. Isolate the
				// runtime from optical-flow estimation using its exact backward motion.
				Clear(scheduler.Current(), *frame.fg_inputs->depth, {.5f, 0, 0, 0});
				Clear(scheduler.Current(), *frame.fg_inputs->motion,
				      {-float(frame.fg_inputs->motion->backing.extent.width) / 640.f, 0, 0, 0});
				frame.fg_inputs->jitter_x = frame.fg_inputs->jitter_y = 0;
				frame.fg_inputs->reset = index == 0;
			}
			scheduler.FlushAndWait();
			// DLSS-G deliberately bypasses interpolation for unfocused windows.
			// The API runner can take focus while reporting build output. Restore
			// only this test window before measuring the actual SDK presents.
			if (GetForegroundWindow() != hwnd) {
				const auto foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
				const auto this_thread = GetCurrentThreadId();
				const bool attached = foreground_thread && foreground_thread != this_thread &&
				    AttachThreadInput(this_thread, foreground_thread, TRUE);
				ShowWindow(hwnd, SW_RESTORE);
				SetForegroundWindow(hwnd);
				if (attached) AttachThreadInput(this_thread, foreground_thread, FALSE);
			}
			presenter.Present(frame);
			Check(presenter.FrameGeneration().Enabled(), "new guest frame lost Frame Generation");
			if (index == 60) check_cached(); // The next guest flip must keep the same generation mode.
			SDL_Event event {};
			while (SDL_PollEvent(&event)) {}
			// Leave room for an intermediate scanout even on a 60 Hz display.
			std::this_thread::sleep_for(std::chrono::milliseconds(33));
		}
		RequireVulkanSuccess(graphics.device.waitIdle(), "wait for generated frames");
		const auto presented = presenter.FrameGeneration().TotalPresentedFrames();
		const auto submitted = presenter.FrameGeneration().TotalSubmittedFrames();
		std::printf("Frame Generation: %llu display frames / %u actual submissions (%d guest frames)\n",
		    static_cast<unsigned long long>(presented), submitted, count);
		std::fflush(stdout);
		interpolation_succeeded = presented > submitted;
		for (int repeat = 0; repeat < 3; ++repeat) {
			check_cached();
		}
		// Off resumes ordinary presentation; re-enable prepares a fresh history.
		config.dlss_frame_generation = false;
		Config::Load(config);
		Check(presenter.PresentLastFrame(), "Frame Generation disable broke cached presentation");
		Check(!presenter.FrameGeneration().Enabled(), "Frame Generation remained enabled after Off");
		config.dlss_frame_generation = true;
		Config::Load(config);
		const auto display_before = presenter.FrameGeneration().TotalPresentedFrames();
		const auto submissions_before = presenter.FrameGeneration().TotalSubmittedFrames();
		// Re-enable and change output size while previously tagged resources exist.
		SDL_SetWindowSize(test_window, 1360, 768);
		SDL_RaiseWindow(test_window);
		for (int index = count; index < count + 45; ++index) {
			UploadPattern(scheduler, source, index, 0);
			cache.MarkGpuWritten(id);
			auto& next = presenter.PrepareFrame(scheduler.Current(), info);
			Check(next.fg_inputs != nullptr, "re-enabled Frame Generation lost inputs");
			if (config.dlss_mode == Config::DlssMode::Off) {
				Check(next.fg_inputs->jitter_x == 0 && next.fg_inputs->jitter_y == 0,
				      "unjittered Off color carries reconstruction jitter");
			}
			scheduler.FlushAndWait();
			SetForegroundWindow(hwnd);
			presenter.Present(next);
			SDL_Event event {};
			while (SDL_PollEvent(&event)) {}
			std::this_thread::sleep_for(std::chrono::milliseconds(33));
		}
		RequireVulkanSuccess(graphics.device.waitIdle(), "wait for re-enabled generation");
		const auto new_display = presenter.FrameGeneration().TotalPresentedFrames() - display_before;
		const auto new_submissions = presenter.FrameGeneration().TotalSubmittedFrames() - submissions_before;
		std::printf("Frame Generation re-enable/resize: %llu display frames / %u actual submissions\n",
		    static_cast<unsigned long long>(new_display), new_submissions);
		interpolation_succeeded = interpolation_succeeded && new_display > new_submissions;
		Check(presenter.FrameGeneration().Enabled(), "Frame Generation did not re-enable");
		auto& blank = presenter.PrepareBlankFrame(1360, 768, true);
		presenter.Present(blank);
		Check(!presenter.FrameGeneration().Enabled(), "blank frame did not disable generation");
		Check(presenter.PresentLastFrame(), "cached blank presentation failed");
		Check(!presenter.FrameGeneration().Enabled(), "cached blank frame re-enabled generation");
		auto& resumed = presenter.PrepareFrame(scheduler.Current(), info);
		scheduler.FlushAndWait();
		presenter.Present(resumed);
		Check(presenter.FrameGeneration().Enabled(), "guest frame did not resume generation after blank");
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
		if (std::strcmp(regression, "srgb") == 0) {
			config.dlss_mode = Config::DlssMode::Off;
			Config::Load(config);
			for (bool main_bus : {true, false}) {
				auto& copied = presenter.PrepareFrame(scheduler.Current(), info, nullptr, main_bus);
				Check(!copied.dlss_evaluated, "Off/overlay unexpectedly evaluated DLSS");
				Check(Readback(scheduler, copied.image, 4) == original,
				      "sRGB Off/overlay presentation changed encoded pixel bytes");
				presenter.Discard(copied);
			}
			Image bgra(graphics, scheduler, Description(vk::Format::eB8G8R8A8Srgb, {37, 19}));
			UploadPattern(scheduler, bgra, 0, 0);
			const auto bgra_bytes = Readback(scheduler, bgra.backing, 4);
			Presenter::Frame copied;
			copied.Configure(graphics, {37, 19}, vk::Format::eB8G8R8A8Unorm);
			copied.CopyFrom(scheduler.Current(), bgra);
			Check(Readback(scheduler, copied.image, 4) == bgra_bytes,
			      "BGRA sRGB copy changed encoded pixel bytes");
			graphics.DeleteImage(copied.image);
		} else if (std::strcmp(regression, "fallback") == 0) {
			DlssFrameInputs invalid {};
			auto& fallback = presenter.PrepareFrame(scheduler.Current(), info, &invalid);
			Check(!fallback.dlss_evaluated, "invalid inputs did not fall back");
			Check(fallback.image.format == vk::Format::eR16G16B16A16Sfloat,
			      "fallback did not retain the reconstruction output");
			const auto output = Readback(scheduler, fallback.image, 8);
			CheckEncodedResample(original, {640, 360}, output, {960, 540});
			Image bgra(graphics, scheduler, Description(vk::Format::eB8G8R8A8Srgb, {37, 19}));
			UploadPattern(scheduler, bgra, 0, 0);
			const auto bgra_bytes = Readback(scheduler, bgra.backing, 4);
			EmulatorDlssInputs generator(graphics, scheduler);
			Check(generator.ResampleColor(scheduler.Current(), bgra, fallback.image, fallback.view),
			      "BGRA sRGB fallback rejected");
			CheckEncodedResample(bgra_bytes, {37, 19}, Readback(scheduler, fallback.image, 8), {960, 540}, true);
			presenter.Discard(fallback);
		} else if (std::strcmp(regression, "pool") == 0) {
			auto& blank = presenter.PrepareBlankFrame(960, 540, true);
			auto& main = presenter.PrepareFrame(scheduler.Current(), info);
			auto& overlay = presenter.PrepareFrame(scheduler.Current(), info, nullptr, false);
			scheduler.Finish();
			const auto main_image = main.image.image;
			const auto overlay_image = overlay.image.image;
			presenter.Discard(overlay);
			presenter.Discard(main);
			for (int repeat = 0; repeat < 4; ++repeat) {
				auto& next_main = presenter.PrepareFrame(scheduler.Current(), info);
				Check(&next_main == &main && next_main.image.image == main_image,
				      "MAIN DLSS frame stole/reallocated an overlay image");
				auto& next_overlay = presenter.PrepareFrame(scheduler.Current(), info, nullptr, false);
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
	Check(frame.image.format == vk::Format::eR16G16B16A16Sfloat && frame.image.extent.width == 960 &&
	      frame.image.extent.height == 540, "reconstructed frame format/output resolution");
	const auto pixels = Readback(scheduler, frame.image, 8);
	float minimum = 1, maximum = 0;
	for (size_t i = 0; i < pixels.size(); i += 8) {
		uint16_t red;
		std::memcpy(&red, pixels.data() + i, 2);
		const float value = Half(red);
		Check(std::isfinite(value), "non-finite reconstructed presentation pixel");
		minimum = std::min(minimum, value); maximum = std::max(maximum, value);
	}
	Check(maximum - minimum > .2f, "prepared DLSS output lost source texture");
	presenter.Present(frame);
	Check(presenter.PresentLastFrame(), "cached reconstructed frame cannot be presented");
	auto& overlay = presenter.PrepareFrame(scheduler.Current(), info, nullptr, false);
	Check(!overlay.dlss_evaluated && overlay.image.format == vk::Format::eR8G8B8A8Unorm && overlay.image.extent.width == 640,
	      "separate overlay incorrectly reconstructed");
	scheduler.Finish();
	presenter.Discard(overlay);
	// Resize/mode changes while producer work is queued must retire resources safely.
	for (auto mode : {Config::DlssMode::Performance, Config::DlssMode::DLAA, Config::DlssMode::Off, Config::DlssMode::Quality}) {
		config.dlss_mode = mode;
		config.screen_width = 1280;
		config.screen_height = 720;
		Config::Load(config);
		auto& next = presenter.PrepareFrame(scheduler.Current(), info);
		Check(next.dlss_evaluated == (mode != Config::DlssMode::Off), "mode switching/fallback presentation");
		if (next.dlss_evaluated) Check(next.image.extent.width == 1280 && next.image.extent.height == 720, "output resize");
		scheduler.Finish();
		presenter.Present(next);
	}
	Clear(scheduler.Current(), source, {.1f, .7f, .3f, 1});
	cache.MarkGpuWritten(id);
	auto& changed = presenter.PrepareFrame(scheduler.Current(), info);
	Check(changed.dlss_evaluated, "changed source lost DLSS");
	const auto new_pixels = Readback(scheduler, changed.image, 8);
	uint16_t center[4];
	std::memcpy(center, new_pixels.data() + (360 * 1280 + 640) * 8, 8);
	Check(Half(center[1]) > Half(center[0]) + .3f, "source scene cut not reflected in reconstructed pixels");
	presenter.Present(changed);
	presenter.ClearLayer(0);
	scheduler.Finish();
	graphics.instance.destroyDebugUtilsMessengerEXT(messenger, nullptr);
	Check(validation_errors == 0, "presentation Vulkan validation errors");
	std::puts("Emulator presentation: no adapter, actual GPU pixels, overlay, cached frame, modes and resize passed");
}
void FrameGenerationExtensionFallbackCase() {
	static vk::detail::DynamicLoader loader;
	const auto native = loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(native);
	DlssFrameGeneration fg;
	VULKAN_HPP_DEFAULT_DISPATCHER.init(fg.Initialize(native));
	Check(fg.Hooked(), "extension fallback test did not initialize Streamline");
	vk::ApplicationInfo app {};
	app.pApplicationName = "FrameGenerationExtensionFallback";
	app.apiVersion = VULKAN_TARGET_API_VERSION;
	vk::InstanceCreateInfo instance_info {};
	instance_info.pApplicationInfo = &app;
	vk::Instance instance;
	RequireVulkanSuccess(fg.CreateInstance(instance_info, instance), "Streamline fallback instance");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
	const auto devices = EnumerateVulkan<vk::PhysicalDevice>("fallback physical devices",
	    [&](uint32_t* n, vk::PhysicalDevice* p) { return instance.enumeratePhysicalDevices(n, p); });
	Check(!devices.empty(), "extension fallback test needs a Vulkan device");
	const auto physical = devices.front();
	auto available = EnumerateVulkan<vk::ExtensionProperties>("fallback device extensions",
	    [&](uint32_t* n, vk::ExtensionProperties* p) { return physical.enumerateDeviceExtensionProperties(nullptr, n, p); });
	std::vector<const char*> enabled;
	if (std::any_of(available.begin(), available.end(), [](const auto& extension) {
		return std::strcmp(extension.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
	})) {
		Check(fg.ConfigureDeviceExtensions(available, enabled), "supported present_id rejected");
		Check(fg.ConfigureDeviceExtensions(available, enabled) && enabled.size() == 1,
		      "optional present_id was appended more than once");
	}
	// Mask the optional capability on the selected physical device. Keep the
	// real interposer-created instance to exercise native dispatcher restoration.
	std::erase_if(available, [](const auto& extension) {
		return std::strcmp(extension.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
	});
	enabled.clear();
	Check(!fg.ConfigureDeviceExtensions(available, enabled), "missing present_id did not fall back");
	Check(!fg.Hooked() && !fg.Available() && !fg.Enabled() && enabled.empty(),
	      "unsupported FG left Streamline requirements enabled");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(native);
	VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
	const auto queues = physical.getQueueFamilyProperties();
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < queues.size(); ++i) {
		if (queues[i].queueFlags & vk::QueueFlagBits::eGraphics) { family = i; break; }
	}
	Check(family != UINT32_MAX, "fallback graphics queue");
	const float priority = 1;
	vk::DeviceQueueCreateInfo queue {};
	queue.queueFamilyIndex = family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	vk::DeviceCreateInfo device_info {};
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue;
	vk::Device device;
	RequireVulkanSuccess(physical.createDevice(&device_info, nullptr, &device), "native device after missing FG extension");
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
	const bool presentation_regression = argc == 3 && std::strcmp(argv[1], "--presentation-regression") == 0;
	const bool fg_controlled = argc == 2 && (std::strcmp(argv[1], "--frame-generation-controlled") == 0 ||
	    std::strcmp(argv[1], "--frame-generation-controlled-off") == 0);
	const bool frame_generation_off = argc == 2 && (std::strcmp(argv[1], "--frame-generation-off") == 0 ||
	    std::strcmp(argv[1], "--frame-generation-controlled-off") == 0);
	const bool frame_generation = fg_controlled || frame_generation_off || (argc == 2 && std::strcmp(argv[1], "--frame-generation") == 0);
	const bool fg_validation_fallback = argc == 2 && std::strcmp(argv[1], "--frame-generation-validation-fallback") == 0;
	const bool fg_extension_fallback = argc == 2 && std::strcmp(argv[1], "--frame-generation-extension-fallback") == 0;
	config.dlss_frame_generation = frame_generation || fg_validation_fallback || fg_extension_fallback;
	if (frame_generation_off) config.dlss_mode = Config::DlssMode::Off;
	const bool emulator_presentation = presentation_regression || frame_generation ||
	    (argc == 2 && std::strcmp(argv[1], "--emulator-presentation") == 0);
	const bool window_device = emulator_presentation || fg_validation_fallback || (argc == 2 &&
	    (std::strcmp(argv[1], "--window-device") == 0 ||
	     std::strcmp(argv[1], "--window-device-off") == 0));
	if (window_device) {
		config.printf_direction = Config::LogDirection::Console;
		config.vulkan_validation_enabled = (emulator_presentation && !frame_generation) || fg_validation_fallback;
		if (emulator_presentation) { config.screen_width = 960; config.screen_height = 540; }
		if (frame_generation) { config.screen_width = 1280; config.screen_height = 720; }
		if (std::strcmp(argv[1], "--window-device-off") == 0) config.dlss_mode = Config::DlssMode::Off;
	}
	Config::Load(config);
	subsystems.Initialize<Log::Lifecycle>();
	if (fg_extension_fallback) {
		FrameGenerationExtensionFallbackCase();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--window-title-timing") == 0) {
		Check(SDL_InitSubSystem(SDL_INIT_VIDEO), "SDL video initialization");
		WindowContext window;
		window.window = SDL_CreateWindow("title timing", 64, 64, SDL_WINDOW_HIDDEN);
		Check(window.window != nullptr, "hidden title-test window");
		window.UpdateTitle();
		const std::string first_title = SDL_GetWindowTitle(window.window);
		std::atomic<bool> finished = false;
		std::thread producer([&] {
			for (int i = 0; i < 120; ++i) window.UpdateTitle();
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
		while (SDL_PollEvent(&event)) {}
		Check(finished_without_ui, "frame title updates block on the UI thread");
		Check(first_title == SDL_GetWindowTitle(window.window), "title is changed on every frame");
		for (int i = 0; i < 20; ++i) window.UpdateTitle(false, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		finished.store(false, std::memory_order_release);
		std::thread due_update([&] {
			window.UpdateTitle();
			finished.store(true, std::memory_order_release);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const bool due_finished_without_ui = finished.load(std::memory_order_acquire);
		while (!finished.load(std::memory_order_acquire)) (void)SDL_WaitEventTimeout(&event, 10);
		due_update.join();
		while (SDL_PollEvent(&event)) {}
		Check(due_finished_without_ui, "periodic FPS refresh still waits for the UI thread");
		Check(first_title != SDL_GetWindowTitle(window.window), "title is not refreshed after FPS interval");
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
		if (fg_validation_fallback) Check(!presenter.FrameGeneration().Available(), "Frame Generation must retain validated Vulkan fallback");
		if (emulator_presentation) EmulatorPresentationCase(presenter, config,
		                                                     fg_controlled ? "fg-controlled" : (presentation_regression ? argv[2] : nullptr));
		std::puts("Production window Vulkan device created successfully");
		WindowShutdown();
		Check(interpolation_succeeded, "no intermediate DLSS frames were presented");
		return 0;
	}
	static vk::detail::DynamicLoader loader;
	VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
	GraphicContext graphics;
	auto extensions = EnumerateVulkan<vk::ExtensionProperties>("instance extensions", [](uint32_t* n, vk::ExtensionProperties* p) { return vk::enumerateInstanceExtensionProperties(nullptr, n, p); });
	std::vector<const char*> enabled;
	Check(AppendDlssInstanceExtensions(enabled, extensions), "DLSS instance extensions");
	enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	auto layers = EnumerateVulkan<vk::LayerProperties>("instance layers", [](uint32_t* n, vk::LayerProperties* p) { return vk::enumerateInstanceLayerProperties(n, p); });
	const char* validation = "VK_LAYER_KHRONOS_validation";
	bool has_validation = std::any_of(layers.begin(), layers.end(), [&](const auto& layer) { return std::strcmp(layer.layerName, validation) == 0; });
	vk::ApplicationInfo app {};
	app.pApplicationName = "DlssGpuTests";
	app.apiVersion = VULKAN_TARGET_API_VERSION;
	vk::InstanceCreateInfo instance_create {};
	instance_create.pApplicationInfo = &app;
	instance_create.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
	instance_create.ppEnabledExtensionNames = enabled.data();
	instance_create.enabledLayerCount = has_validation ? 1 : 0;
	instance_create.ppEnabledLayerNames = &validation;
	RequireVulkanSuccess(vk::createInstance(&instance_create, nullptr, &graphics.instance), "test Vulkan instance");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(graphics.instance);
	vk::DebugUtilsMessengerCreateInfoEXT debug {};
	debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
	debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
	debug.pfnUserCallback = DebugCallback;
	RequireVulkanSuccess(graphics.instance.createDebugUtilsMessengerEXT(&debug, nullptr, &graphics.debug_messenger), "test validation messenger");
	const auto devices = EnumerateVulkan<vk::PhysicalDevice>("physical devices", [&](uint32_t* n, vk::PhysicalDevice* p) { return graphics.instance.enumeratePhysicalDevices(n, p); });
	for (auto device : devices) {
		if (device.getProperties().vendorID == 0x10de) { graphics.physical_device = device; break; }
	}
	if (!graphics.physical_device) { std::puts("SKIP: no NVIDIA GPU"); return 77; }
	graphics.physical_device.getProperties(&graphics.physical_device_properties);
	vk::PhysicalDevicePushDescriptorPropertiesKHR push_properties {};
	vk::PhysicalDeviceProperties2 properties {};
	properties.pNext = &push_properties;
	graphics.physical_device.getProperties2(&properties);
	graphics.max_push_descriptors = push_properties.maxPushDescriptors;
	graphics.physical_device.getMemoryProperties(&graphics.physical_device_memory_properties);
	const auto queues = graphics.physical_device.getQueueFamilyProperties();
	for (uint32_t i = 0; i < queues.size(); ++i) {
		const auto flags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
		if ((queues[i].queueFlags & flags) == flags) { graphics.queue_family = i; break; }
	}
	Check(graphics.queue_family != UINT32_MAX, "graphics/compute queue");
	const auto device_extensions = EnumerateVulkan<vk::ExtensionProperties>("device extensions", [&](uint32_t* n, vk::ExtensionProperties* p) { return graphics.physical_device.enumerateDeviceExtensionProperties(nullptr, n, p); });
	enabled = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME};
	graphics.dlss_extensions_enabled = AppendDlssDeviceExtensions(graphics, enabled, device_extensions);
	Check(graphics.dlss_extensions_enabled, "DLSS device extensions");
	float priority = 1;
	vk::DeviceQueueCreateInfo queue {};
	queue.queueFamilyIndex = graphics.queue_family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	auto features11 = WindowContext::RequiredVulkan11Features();
	auto features12 = WindowContext::RequiredVulkan12Features();
	auto features13 = WindowContext::RequiredVulkan13Features();
	features13.pNext = &features12;
	features12.pNext = &features11;
	vk::PhysicalDeviceFeatures features {};
	features.shaderInt64 = VK_TRUE;
	features.shaderInt16 = VK_TRUE;
	features.sampleRateShading = VK_TRUE;
	graphics.sample_rate_shading_enabled = true;
	vk::DeviceCreateInfo device_create {};
	device_create.pNext = &features13;
	device_create.pEnabledFeatures = &features;
	device_create.queueCreateInfoCount = 1;
	device_create.pQueueCreateInfos = &queue;
	device_create.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
	device_create.ppEnabledExtensionNames = enabled.data();
	RequireVulkanSuccess(graphics.physical_device.createDevice(&device_create, nullptr, &graphics.device), "test Vulkan device");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(graphics.device);
	graphics.device.getQueue(graphics.queue_family, 0, &graphics.queue);
	Check(graphics.CreateAllocator(), "VMA initialization");
	{
		RenderContext renderer(graphics);
		{
			DlssProcessor startup(graphics, renderer.GetCommandScheduler());
			Check(startup.Available(), "NGX initialization without a guest stream");
		}
		HW::Context registers {};
		HW::UserConfig user {};
		HW::Shader shaders {};
		auto& scheduler = renderer.GetCommandScheduler();
		scheduler.Begin(registers, user, shaders);
		RasterScaleCase(graphics, scheduler);
		EmulatorMotionCase(graphics, scheduler);
		{
			DlssProcessor dlss(graphics, scheduler);
			Check(dlss.Available(), "NGX capability check");
			for (auto mode : {Config::DlssMode::Quality, Config::DlssMode::Balanced, Config::DlssMode::Performance, Config::DlssMode::UltraPerformance, Config::DlssMode::DLAA}) {
				EvaluateCase(graphics, renderer, dlss, mode, {1920, 1080});
			}
			EvaluateCase(graphics, renderer, dlss, Config::DlssMode::Quality, {1280, 720});
		}
		scheduler.Finish(); // releases features and shuts down NGX after GPU completion
	}
	RequireVulkanSuccess(graphics.device.waitIdle(), "test device idle");
	graphics.DestroyAllocator();
	graphics.device.destroy(nullptr);
	graphics.instance.destroyDebugUtilsMessengerEXT(graphics.debug_messenger, nullptr);
	graphics.instance.destroy(nullptr);
	Check(validation_errors == 0, "Vulkan validation errors");
	std::printf("DLSS GPU tests passed (validation layer: %s)\n", has_validation ? "enabled" : "unavailable");
}
