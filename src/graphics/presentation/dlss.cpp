#include "graphics/presentation/dlss.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <SDL3/SDL.h>

#if defined(KYTY_HAS_DLSS)
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif

namespace Libs::Graphics {
namespace {
#if defined(KYTY_HAS_DLSS)
// NGX APIs are not thread safe, including releases in deferred GPU callbacks.
std::mutex ngx_mutex;
constexpr const char* project_id = "84943a21-981d-49ef-a275-18acdd14b479";

NVSDK_NGX_FeatureDiscoveryInfo DiscoveryInfo() {
	NVSDK_NGX_FeatureDiscoveryInfo info {};
	info.SDKVersion = NVSDK_NGX_Version_API;
	info.FeatureID  = NVSDK_NGX_Feature_SuperSampling;
	info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
	info.Identifier.v.ProjectDesc.ProjectId = project_id;
	info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
	info.Identifier.v.ProjectDesc.EngineVersion = KYTY_VERSION;
	info.ApplicationDataPath = L".";
	return info;
}

bool AppendExtensions(std::vector<const char*>& enabled,
                      const std::vector<vk::ExtensionProperties>& available,
                      uint32_t count, VkExtensionProperties* required) {
	for (uint32_t i = 0; i < count; ++i) {
		if (std::none_of(available.begin(), available.end(), [&](const auto& extension) {
			    return std::strcmp(extension.extensionName, required[i].extensionName) == 0;
		    })) {
			LOGF("DLSS unavailable: missing Vulkan extension %s\n", required[i].extensionName);
			return false;
		}
	}
	// SDK owns the returned extension names for the lifetime of the process.
	for (uint32_t i = 0; i < count; ++i) {
		if (std::none_of(enabled.begin(), enabled.end(), [&](const char* name) {
			    return std::strcmp(name, required[i].extensionName) == 0;
		    })) {
			enabled.push_back(required[i].extensionName);
		}
	}
	return true;
}

NVSDK_NGX_PerfQuality_Value Quality(Config::DlssMode mode) {
	switch (mode) {
		case Config::DlssMode::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
		case Config::DlssMode::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		case Config::DlssMode::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		case Config::DlssMode::DLAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
		default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}
}

NVSDK_NGX_Resource_VK Resource(Image& image, vk::ImageAspectFlags aspect,
                               vk::CommandBuffer command) {
	ImageViewInfo view {};
	view.format = image.backing.format;
	view.aspect = aspect;
	auto handle = image.FindView(view);
	image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {}, command);
	VkImageSubresourceRange range {static_cast<VkImageAspectFlags>(aspect), 0, 1, 0, 1};
	return NVSDK_NGX_Create_ImageView_Resource_VK(handle, image.backing.image, range,
	    static_cast<VkFormat>(view.format), image.backing.extent.width, image.backing.extent.height, false);
}
#endif
} // namespace

bool AppendDlssInstanceExtensions(std::vector<const char*>& enabled,
                                  const std::vector<vk::ExtensionProperties>& available) {
#if defined(KYTY_HAS_DLSS)
	if (Config::GetDlssMode() == Config::DlssMode::Off) return false;
	std::scoped_lock lock(ngx_mutex);
	auto info = DiscoveryInfo();
	uint32_t count = 0;
	VkExtensionProperties* required = nullptr;
	auto result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &required);
	if (NVSDK_NGX_FAILED(result)) {
		LOGF("DLSS instance extension query failed: 0x%x; using normal presentation\n", static_cast<unsigned>(result));
		return false;
	}
	return AppendExtensions(enabled, available, count, required);
#else
	(void)enabled;
	(void)available;
	return false;
#endif
}

bool AppendDlssDeviceExtensions(GraphicContext& graphics, std::vector<const char*>& enabled,
                                const std::vector<vk::ExtensionProperties>& available) {
#if defined(KYTY_HAS_DLSS)
	if (Config::GetDlssMode() == Config::DlssMode::Off ||
	    graphics.physical_device_properties.vendorID != 0x10de) return false;
	std::scoped_lock lock(ngx_mutex);
	auto info = DiscoveryInfo();
	uint32_t count = 0;
	VkExtensionProperties* required = nullptr;
	auto result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
	    graphics.instance, graphics.physical_device, &info, &count, &required);
	if (NVSDK_NGX_FAILED(result)) {
		LOGF("DLSS device extension query failed: 0x%x; using normal presentation\n", static_cast<unsigned>(result));
		return false;
	}
	return AppendExtensions(enabled, available, count, required);
#else
	(void)graphics;
	(void)enabled;
	(void)available;
	return false;
#endif
}

struct DlssProcessor::Impl {
	GraphicContext& graphics;
	CommandScheduler& scheduler;
	bool available = false;
	bool reset = true;
	bool missing_inputs_logged = false;
#if defined(KYTY_HAS_DLSS)
	bool initialized = false;
	NVSDK_NGX_Parameter* parameters = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
	vk::Extent2D input_extent {};
	vk::Extent2D output_extent {};
	int flags = 0;
	Config::DlssMode mode = Config::DlssMode::Off;
#endif

	Impl(GraphicContext& owner, CommandScheduler& commands) : graphics(owner), scheduler(commands) {
		if (Config::GetDlssMode() == Config::DlssMode::Off) return;
#if defined(KYTY_HAS_DLSS)
		if (!graphics.dlss_extensions_enabled) {
			LOGF("DLSS unavailable on this Vulkan device; using normal presentation\n");
			return;
		}
		char* path = SDL_GetPrefPath("Kyty", "KytyPS5");
		if (path == nullptr) {
			LOGF("DLSS unavailable: cannot create NGX data directory: %s\n", SDL_GetError());
			return;
		}
		auto data_path = std::filesystem::u8path(path).wstring();
		SDL_free(path);
		std::scoped_lock lock(ngx_mutex);
		auto result = NVSDK_NGX_VULKAN_Init_with_ProjectID(project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
		    KYTY_VERSION, data_path.c_str(), graphics.instance, graphics.physical_device,
		    graphics.device, VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
		    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
		if (NVSDK_NGX_FAILED(result)) {
			LOGF("DLSS NGX initialization failed: 0x%x; using normal presentation\n", static_cast<unsigned>(result));
			return;
		}
		initialized = true;
		result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters);
		int supported = 0;
		if (NVSDK_NGX_SUCCEED(result) && parameters != nullptr) {
			result = parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
		}
		available = NVSDK_NGX_SUCCEED(result) && supported != 0;
		LOGF("DLSS Super Resolution: %s\n", available ? "available; emulator reconstruction enabled" : "unsupported; using normal presentation");
#else
		LOGF("DLSS requested but this build has no NGX SDK; using normal presentation\n");
#endif
	}

	~Impl() {
#if defined(KYTY_HAS_DLSS)
		if (initialized) {
			// Presenter is destroyed before the renderer. Its scheduler drains these
			// callbacks before destroying the Vulkan device, including pending commands.
			auto cleanup = [handle = feature, params = parameters, device = graphics.device] {
				std::scoped_lock lock(ngx_mutex);
				if (handle != nullptr) NVSDK_NGX_VULKAN_ReleaseFeature(handle);
				if (params != nullptr) NVSDK_NGX_VULKAN_DestroyParameters(params);
				NVSDK_NGX_VULKAN_Shutdown1(device);
			};
			if (scheduler.Active()) {
				scheduler.DeferOperation(std::move(cleanup));
			} else {
				// NGX can initialize before any guest command stream has begun.
				cleanup();
			}
		}
#endif
	}
};

DlssProcessor::DlssProcessor(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_impl(std::make_unique<Impl>(graphics, scheduler)) {}
DlssProcessor::~DlssProcessor() = default;
bool DlssProcessor::Available() const {
	return m_impl->available && Config::GetDlssMode() != Config::DlssMode::Off;
}

std::optional<vk::Extent2D> DlssProcessor::OptimalInputExtent(vk::Extent2D output, vk::Extent2D source) const {
#if defined(KYTY_HAS_DLSS)
	if (!Available() || output.width == 0 || output.height == 0) return std::nullopt;
	if (Config::GetDlssMode() != Config::DlssMode::DLAA &&
	    source.width >= output.width && source.height >= output.height) return std::nullopt;
	std::scoped_lock lock(ngx_mutex);
	unsigned width = 0, height = 0, max_width = 0, max_height = 0, min_width = 0, min_height = 0;
	float sharpness = 0;
	// Final color has already been rendered. Preserve its detail rather than
	// shrinking it to a resolution intended for a game's pre-render settings.
	auto result = NGX_DLSS_GET_OPTIMAL_SETTINGS(m_impl->parameters, output.width, output.height,
	    Quality(Config::GetDlssMode()), &width, &height, &max_width, &max_height,
	    &min_width, &min_height, &sharpness);
	if (NVSDK_NGX_FAILED(result) || width == 0 || height == 0) return std::nullopt;
	if (source.width && source.height && Config::GetDlssMode() != Config::DlssMode::DLAA) {
		width = std::clamp(source.width, min_width, max_width);
		height = std::clamp(source.height, min_height, max_height);
	}
	return vk::Extent2D {width, height};
#else
	(void)output;
	(void)source;
	return std::nullopt;
#endif
}

void DlssProcessor::SkipFrame() {
	m_impl->reset = true;
	if (Available() && !m_impl->missing_inputs_logged) {
		LOGF("DLSS inactive: no compatible temporal inputs or presentation surface; using normal presentation\n");
		m_impl->missing_inputs_logged = true;
	}
}

bool DlssProcessor::Evaluate(CommandBuffer& command, const DlssFrameInputs& inputs,
                              VulkanImage& output, vk::ImageView output_view) {
#if defined(KYTY_HAS_DLSS)
	auto& state = *m_impl;
	if (!Available() || command.IsInvalid() || &command.GetGraphics() != &state.graphics) {
		state.reset = true;
		return false;
	}
	const auto valid_image = [](const Image* image) {
		return image != nullptr && image->backing.image != nullptr && image->backing.layers == 1 &&
		       image->backing.samples == 1 && image->backing.image_type == vk::ImageType::e2D &&
		       static_cast<bool>(image->backing.usage & vk::ImageUsageFlagBits::eSampled);
	};
	if (!valid_image(inputs.color) || !valid_image(inputs.depth) || !valid_image(inputs.motion_vectors) ||
	    !std::isfinite(inputs.jitter_x) || !std::isfinite(inputs.jitter_y) ||
	    !std::isfinite(inputs.motion_scale_x) || !std::isfinite(inputs.motion_scale_y) ||
	    inputs.motion_scale_x == 0 || inputs.motion_scale_y == 0 || output_view == nullptr ||
	    output.image == nullptr || output.format != vk::Format::eR16G16B16A16Sfloat ||
	    output.samples != 1 || output.layers != 1 ||
	    (output.usage & (vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst)) !=
	        (vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst)) {
		SkipFrame();
		return false;
	}
	const vk::Extent2D input_size {inputs.color->backing.extent.width, inputs.color->backing.extent.height};
	const vk::Extent2D output_size {output.extent.width, output.extent.height};
	for (const auto* image : {inputs.depth, inputs.motion_vectors}) {
		if (image->backing.extent.width != input_size.width || image->backing.extent.height != input_size.height) {
			SkipFrame();
			return false;
		}
	}
	if (inputs.bias_current_color != nullptr &&
	    (!valid_image(inputs.bias_current_color) ||
	     inputs.bias_current_color->backing.extent.width != input_size.width ||
	     inputs.bias_current_color->backing.extent.height != input_size.height ||
	     inputs.bias_current_color->backing.format != vk::Format::eR32Sfloat ||
	     inputs.bias_current_color->backing.image == output.image)) {
		SkipFrame();
		return false;
	}
	if (input_size.width == 0 || input_size.height == 0 ||
	    output_size.width < input_size.width || output_size.height < input_size.height ||
	    (Config::GetDlssMode() == Config::DlssMode::DLAA && input_size != output_size)) {
		SkipFrame();
		return false;
	}
	const auto depth_format = inputs.depth->backing.format;
	const bool native_depth = depth_format == vk::Format::eD32Sfloat || depth_format == vk::Format::eD16Unorm;
	const auto motion_format = inputs.motion_vectors->backing.format;
	const auto color_format = inputs.color->backing.format;
	const bool valid_color = color_format == vk::Format::eR8G8B8A8Unorm ||
	                         color_format == vk::Format::eB8G8R8A8Unorm ||
	                         color_format == vk::Format::eR16G16B16A16Sfloat ||
	                         color_format == vk::Format::eR32G32B32A32Sfloat;
	if ((!native_depth && depth_format != vk::Format::eR32Sfloat && depth_format != vk::Format::eR16Unorm) ||
	    (motion_format != vk::Format::eR16G16Sfloat && motion_format != vk::Format::eR32G32Sfloat) ||
	    !valid_color || output.image == inputs.color->backing.image ||
	    output.image == inputs.depth->backing.image || output.image == inputs.motion_vectors->backing.image) {
		SkipFrame();
		return false;
	}
	std::scoped_lock lock(ngx_mutex);
	int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
	if (inputs.depth_inverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
	if (inputs.hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
	if (state.feature == nullptr || state.input_extent != input_size || state.output_extent != output_size ||
	    state.flags != flags || state.mode != Config::GetDlssMode()) {
		unsigned optimal_width = 0, optimal_height = 0, max_width = 0, max_height = 0;
		unsigned min_width = 0, min_height = 0;
		float sharpness = 0;
		const auto settings = NGX_DLSS_GET_OPTIMAL_SETTINGS(state.parameters, output_size.width,
		    output_size.height, Quality(Config::GetDlssMode()), &optimal_width, &optimal_height,
		    &max_width, &max_height, &min_width, &min_height, &sharpness);
		if (NVSDK_NGX_FAILED(settings) || input_size.width < min_width || input_size.width > max_width ||
		    input_size.height < min_height || input_size.height > max_height) {
			if (!state.missing_inputs_logged) {
				LOGF("DLSS inactive: input %ux%u outside SDK range %ux%u..%ux%u for output %ux%u\n",
				    input_size.width, input_size.height, min_width, min_height, max_width, max_height,
				    output_size.width, output_size.height);
			}
			SkipFrame();
			return false;
		}
		if (state.feature != nullptr) {
			state.scheduler.DeferOperation([handle = state.feature] {
				std::scoped_lock release_lock(ngx_mutex);
				NVSDK_NGX_VULKAN_ReleaseFeature(handle);
			});
			state.feature = nullptr;
		}
		NVSDK_NGX_DLSS_Create_Params create {};
		create.Feature.InWidth = input_size.width;
		create.Feature.InHeight = input_size.height;
		create.Feature.InTargetWidth = output_size.width;
		create.Feature.InTargetHeight = output_size.height;
		create.Feature.InPerfQualityValue = Quality(Config::GetDlssMode());
		create.InFeatureCreateFlags = flags;
		command.EndRendering();
		auto result = NGX_VULKAN_CREATE_DLSS_EXT1(state.graphics.device, command.Handle(), 1, 1,
		    &state.feature, state.parameters, &create);
		if (NVSDK_NGX_FAILED(result)) {
			LOGF("DLSS feature creation failed: 0x%x; using normal presentation\n", static_cast<unsigned>(result));
			state.available = false;
			return false;
		}
		state.input_extent = input_size;
		state.output_extent = output_size;
		state.flags = flags;
		state.mode = Config::GetDlssMode();
		state.reset = true;
	}
	command.EndRendering();
	auto color = Resource(*inputs.color, vk::ImageAspectFlagBits::eColor, command.Handle());
	auto depth = Resource(*inputs.depth, native_depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, command.Handle());
	auto motion = Resource(*inputs.motion_vectors, vk::ImageAspectFlagBits::eColor, command.Handle());
	NVSDK_NGX_Resource_VK bias {};
	if (inputs.bias_current_color != nullptr) {
		bias = Resource(*inputs.bias_current_color, vk::ImageAspectFlagBits::eColor, command.Handle());
	}
	VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	auto target = NVSDK_NGX_Create_ImageView_Resource_VK(output_view, output.image, range,
	    static_cast<VkFormat>(output.format), output_size.width, output_size.height, true);
	// Input transitions use Image's tracked state; output uses the prepared-frame state.
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask = output.state.pl_stage;
	barrier.srcAccessMask = output.state.access_mask;
	barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	barrier.dstAccessMask = vk::AccessFlagBits2::eShaderWrite;
	barrier.oldLayout = output.state.layout;
	barrier.newLayout = vk::ImageLayout::eGeneral;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = output.image;
	barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers = &barrier;
	command.Handle().pipelineBarrier2(dependency);
	output.state = {vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral};
	output.subresource_states.clear();
	NVSDK_NGX_VK_DLSS_Eval_Params eval {};
	eval.Feature.pInColor = &color;
	eval.Feature.pInOutput = &target;
	eval.pInDepth = &depth;
	eval.pInMotionVectors = &motion;
	eval.pInBiasCurrentColorMask = inputs.bias_current_color != nullptr ? &bias : nullptr;
	eval.InJitterOffsetX = inputs.jitter_x;
	eval.InJitterOffsetY = inputs.jitter_y;
	eval.InMVScaleX = inputs.motion_scale_x;
	eval.InMVScaleY = inputs.motion_scale_y;
	eval.InRenderSubrectDimensions = {input_size.width, input_size.height};
	eval.InPreExposure = 1.0f;
	eval.InExposureScale = 1.0f;
	eval.InReset = state.reset || inputs.reset_history;
	auto result = NGX_VULKAN_EVALUATE_DLSS_EXT(command.Handle(), state.feature, state.parameters, &eval);
	if (NVSDK_NGX_FAILED(result)) {
		LOGF("DLSS evaluation failed: 0x%x; using normal presentation\n", static_cast<unsigned>(result));
		state.available = false;
		state.reset = true;
		return false;
	}
	state.reset = false;
	return true;
#else
	(void)command;
	(void)inputs;
	(void)output;
	(void)output_view;
	return false;
#endif
}
} // namespace Libs::Graphics
