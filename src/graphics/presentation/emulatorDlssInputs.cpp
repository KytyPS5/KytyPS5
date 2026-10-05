#include "graphics/presentation/emulatorDlssInputs.h"

#include "common/logging/log.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "gpu_blit_shaders/emulator_dlss_reduce_spv.h"
#include "gpu_blit_shaders/emulator_dlss_flow_spv.h"
#include "gpu_blit_shaders/emulator_dlss_inputs_spv.h"

#include <algorithm>
#include <array>
#include <span>

namespace Libs::Graphics {
namespace {
constexpr size_t LevelCount = 4;
struct Push {
	float jitter_x = 0, jitter_y = 0;
	int history = 0, coarse = 0;
};
static_assert(sizeof(Push) == 16);

ImageInfo Description(vk::Format format, vk::Extent2D size) {
	ImageInfo info {};
	info.pixel_format = format;
	info.extent = {size.width, size.height, 1};
	info.pitch = size.width;
	info.bytes_per_block = format == vk::Format::eR16G16B16A16Sfloat ? 8 : 4;
	info.mip_layout[0] = {0, uint64_t(size.width) * size.height * info.bytes_per_block,
	                      size.width, size.height};
	return info;
}

vk::ImageView View(Image& image, bool storage) {
	ImageViewInfo view {};
	view.format = image.backing.format;
	// Final-frame presentation preserves the guest's already encoded color.
	if (view.format == vk::Format::eR8G8B8A8Srgb) view.format = vk::Format::eR8G8B8A8Unorm;
	if (view.format == vk::Format::eB8G8R8A8Srgb) view.format = vk::Format::eB8G8R8A8Unorm;
	view.aspect = vk::ImageAspectFlagBits::eColor;
	view.usage = storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
	return image.FindView(view);
}

float Halton(uint64_t index, uint32_t base) {
	float value = 0, weight = 1;
	while (index != 0) {
		weight /= static_cast<float>(base);
		value += weight * static_cast<float>(index % base);
		index /= base;
	}
	return value - .5f;
}

bool SupportedColor(GraphicContext& graphics, vk::Format format) {
	switch (format) {
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32G32B32A32Sfloat: break;
		default: return false;
	}
	const auto required = vk::FormatFeatureFlagBits::eSampledImage |
	                      vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
	return (graphics.GetFormatProperties(format).optimalTilingFeatures & required) == required;
}
}

struct EmulatorDlssInputs::Impl {
	GraphicContext& graphics;
	CommandScheduler& scheduler;
	vk::DescriptorSetLayout descriptors = nullptr;
	vk::PipelineLayout layout = nullptr;
	vk::Sampler sampler = nullptr;
	std::array<vk::Pipeline, 3> pipelines {};
	vk::Extent2D input_extent {}, source_extent {};
	vk::Format source_format = vk::Format::eUndefined;
	uint64_t frames = 0, previous_time = 0;
	bool reset = true;
	struct Resources {
		std::array<std::unique_ptr<Image>, LevelCount> current, previous, flow;
		std::unique_ptr<Image> color, depth, motion, bias;
	};
	std::unique_ptr<Resources> images;

	Impl(GraphicContext& owner, CommandScheduler& commands): graphics(owner), scheduler(commands) {}

	void RetireImages() {
		if (images == nullptr) return;
		// Image destructors destroy immediately: retain old resources through the
		// tick that last referenced them, including resize/mode changes in flight.
		scheduler.DeferOperation([old = std::move(images)]() mutable { old.reset(); });
	}
	~Impl() {
		auto cleanup = [owner = &graphics, old = std::move(images), handles = pipelines,
		                descriptor_layout = descriptors, pipeline_layout = layout,
		                sampler_handle = sampler]() mutable {
			old.reset();
			for (auto pipeline : handles) if (pipeline) owner->device.destroyPipeline(pipeline, nullptr);
			if (pipeline_layout) owner->device.destroyPipelineLayout(pipeline_layout, nullptr);
			if (descriptor_layout) owner->device.destroyDescriptorSetLayout(descriptor_layout, nullptr);
			if (sampler_handle) owner->device.destroySampler(sampler_handle, nullptr);
		};
		if (scheduler.Active()) scheduler.DeferOperation(std::move(cleanup));
		else cleanup();
	}

	bool Initialize() {
		if (layout != nullptr) return true;
		if (graphics.max_push_descriptors < 7) return false;
		for (const auto format : {vk::Format::eR16G16B16A16Sfloat,
		                         vk::Format::eR16G16Sfloat, vk::Format::eR32Sfloat}) {
			const auto required = vk::FormatFeatureFlagBits::eStorageImage |
			                      vk::FormatFeatureFlagBits::eSampledImage |
			                      vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
			if ((graphics.GetFormatProperties(format).optimalTilingFeatures & required) != required) return false;
		}
		std::array<vk::DescriptorSetLayoutBinding, 7> bindings {};
		for (uint32_t i = 0; i < bindings.size(); ++i) {
			bindings[i] = {i, i < 3 ? vk::DescriptorType::eCombinedImageSampler : vk::DescriptorType::eStorageImage,
			               1, vk::ShaderStageFlagBits::eCompute};
		}
		vk::DescriptorSetLayoutCreateInfo descriptor_info {};
		descriptor_info.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
		descriptor_info.pBindings = bindings.data();
		RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr, &descriptors),
		                     "create emulator DLSS descriptor layout");
		const vk::PushConstantRange range {vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
		vk::PipelineLayoutCreateInfo layout_info {};
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &descriptors;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = &range;
		RequireVulkanSuccess(graphics.device.createPipelineLayout(&layout_info, nullptr, &layout),
		                     "create emulator DLSS pipeline layout");
		vk::SamplerCreateInfo sampler_info {};
		sampler_info.magFilter = sampler_info.minFilter = vk::Filter::eLinear;
		sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(graphics.device.createSampler(&sampler_info, nullptr, &sampler), "create emulator DLSS sampler");
		const std::array<std::span<const uint32_t>, 3> shaders {
		    EMULATOR_DLSS_REDUCE_SPV, EMULATOR_DLSS_FLOW_SPV, EMULATOR_DLSS_INPUTS_SPV};
		for (size_t i = 0; i < shaders.size(); ++i) {
			vk::ShaderModuleCreateInfo module_info {};
			module_info.codeSize = shaders[i].size_bytes();
			module_info.pCode = shaders[i].data();
			vk::ShaderModule module;
			RequireVulkanSuccess(graphics.device.createShaderModule(&module_info, nullptr, &module), "create emulator DLSS shader");
			vk::ComputePipelineCreateInfo create {};
			create.stage.stage = vk::ShaderStageFlagBits::eCompute;
			create.stage.module = module;
			create.stage.pName = "main";
			create.layout = layout;
			const auto result = graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &pipelines[i]);
			graphics.device.destroyShaderModule(module, nullptr);
			RequireVulkanSuccess(result, "create emulator DLSS compute pipeline");
		}
		LOGF("DLSS emulator path: GPU image-space motion, resampled jitter, neutral depth; final frame includes guest HUD\n");
		return true;
	}

	void Allocate(vk::Extent2D extent) {
		RetireImages();
		images = std::make_unique<Resources>();
		input_extent = extent;
		const auto make = [&](vk::Format format, vk::Extent2D size) {
			return std::make_unique<Image>(graphics, scheduler, Description(format, size));
		};
		images->color = make(vk::Format::eR16G16B16A16Sfloat, extent);
		images->depth = make(vk::Format::eR32Sfloat, extent);
		images->motion = make(vk::Format::eR16G16Sfloat, extent);
		images->bias = make(vk::Format::eR32Sfloat, extent);
		const uint32_t width = std::min(extent.width, 320u);
		vk::Extent2D size {width, std::max(1u, uint32_t(uint64_t(extent.height) * width / extent.width))};
		for (size_t level = 0; level < LevelCount; ++level) {
			images->current[level] = make(vk::Format::eR16G16B16A16Sfloat, size);
			images->previous[level] = make(vk::Format::eR16G16B16A16Sfloat, size);
			images->flow[level] = make(vk::Format::eR16G16B16A16Sfloat, size);
			size = {std::max(1u, size.width / 2), std::max(1u, size.height / 2)};
		}
		reset = true;
		frames = 0;
	}

	void Dispatch(CommandBuffer& command, size_t pipeline, std::array<Image*, 3> inputs,
	              std::array<Image*, 4> outputs, Push push) {
		command.EndRendering();
		const auto handle = command.Handle();
		std::array<vk::DescriptorImageInfo, 7> infos {};
		std::array<vk::WriteDescriptorSet, 7> writes {};
		for (uint32_t i = 0; i < infos.size(); ++i) {
			auto& image = i < 3 ? *inputs[i] : *outputs[i - 3];
			const auto destination = i < 3 ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eGeneral;
			image.Transit(destination, i < 3 ? vk::AccessFlagBits2::eShaderRead : vk::AccessFlagBits2::eShaderWrite,
			              {}, handle);
			infos[i] = {i < 3 ? sampler : nullptr, View(image, i >= 3), destination};
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = i < 3 ? vk::DescriptorType::eCombinedImageSampler : vk::DescriptorType::eStorageImage;
			writes[i].pImageInfo = &infos[i];
		}
		handle.bindPipeline(vk::PipelineBindPoint::eCompute, pipelines[pipeline]);
		handle.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0,
		                            static_cast<uint32_t>(writes.size()), writes.data());
		handle.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
		const auto extent = outputs[0]->backing.extent;
		handle.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
	}
};

EmulatorDlssInputs::EmulatorDlssInputs(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_impl(std::make_unique<Impl>(graphics, scheduler)) {}
EmulatorDlssInputs::~EmulatorDlssInputs() = default;
void EmulatorDlssInputs::Reset() { m_impl->reset = true; }

std::optional<DlssFrameInputs> EmulatorDlssInputs::Prepare(CommandBuffer& command, Image& source,
                                                         vk::Extent2D extent) {
	auto& state = *m_impl;
	const auto limit = state.graphics.physical_device_properties.limits.maxImageDimension2D;
	if (command.IsInvalid() || &command.GetGraphics() != &state.graphics ||
	    extent.width == 0 || extent.height == 0 || extent.width > limit || extent.height > limit ||
	    source.backing.image == nullptr || source.backing.layers != 1 || source.backing.samples != 1 ||
	    source.backing.image_type != vk::ImageType::e2D ||
	    source.backing.extent.depth != 1 || source.backing.extent.width == 0 || source.backing.extent.height == 0 ||
	    !(source.backing.usage & vk::ImageUsageFlagBits::eSampled) ||
	    !SupportedColor(state.graphics, source.backing.format) || !state.Initialize()) {
		Reset();
		return std::nullopt;
	}
	if (state.images == nullptr || extent != state.input_extent) state.Allocate(extent);
	const vk::Extent2D source_extent {source.backing.extent.width, source.backing.extent.height};
	const auto now = Common::Timer::QueryPerformanceCounter();
	if (state.source_extent != source_extent || state.source_format != source.backing.format ||
	    (state.previous_time != 0 && now - state.previous_time > Common::Timer::QueryPerformanceFrequency() / 4)) {
		state.reset = true;
	}
	state.source_extent = source_extent;
	state.source_format = source.backing.format;
	state.previous_time = now;
	Push push {};
	push.history = !state.reset;
	// The first frame samples at the pixel center; temporal sampling starts
	// only once a corresponding previous image exists.
	if (!state.reset) {
		push.jitter_x = Halton(state.frames % 32 + 1, 2);
		push.jitter_y = Halton(state.frames % 32 + 1, 3);
	}
	auto& images = *state.images;
	for (size_t level = 0; level < LevelCount; ++level) {
		auto* input = level == 0 ? &source : images.current[level - 1].get();
		auto* target = images.current[level].get();
		state.Dispatch(command, 0, {input, input, input}, {target, target, target, target}, push);
	}
	for (size_t level = LevelCount; level-- > 0;) {
		auto* coarse = level + 1 < LevelCount ? images.flow[level + 1].get() : images.previous[level].get();
		push.coarse = level + 1 < LevelCount;
		auto* target = images.flow[level].get();
		state.Dispatch(command, 1, {images.current[level].get(), images.previous[level].get(), coarse},
		               {target, target, target, target}, push);
	}
	state.Dispatch(command, 2, {&source, &source, images.flow[0].get()},
	               {images.color.get(), images.depth.get(), images.motion.get(), images.bias.get()}, push);
	DlssFrameInputs result {images.color.get(), images.depth.get(), images.motion.get()};
	result.jitter_x = push.jitter_x;
	result.jitter_y = push.jitter_y;
	result.reset_history = state.reset;
	result.bias_current_color = images.bias.get();
	state.reset = false;
	++state.frames;
	std::swap(images.current, images.previous);
	return result;
}
} // namespace Libs::Graphics
