#include "graphics/presentation/directUpscaler.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/presentation/upscaleRuntime.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <string>
#include <xess/xess_vk.h>

namespace Libs::Graphics {

namespace {

// Loaded once and kept for the process: contexts are destroyed by deferred GPU callbacks.
struct Runtime {
	SDL_SharedObject* module = nullptr;
	explicit Runtime(const char* name) {
		const auto runtime = OptiScalerRuntimePath();
		if (!runtime) return;
		const auto path = (runtime->parent_path() / name).u8string();
		module          = SDL_LoadObject(reinterpret_cast<const char*>(path.c_str()));
		if (!module) LOGF("%s unavailable: %s\n", name, SDL_GetError());
	}
	template <class T>
	T Get(const char* name, bool& valid) const {
		auto* function = module ? reinterpret_cast<T>(SDL_LoadFunction(module, name)) : nullptr;
		if (!function && module) LOGF("missing runtime export %s\n", name);
		valid = valid && function;
		return function;
	}
};

struct XessApi {
	Runtime                   runtime {"libxess.dll"};
	bool                      valid = runtime.module != nullptr;
	decltype(&xessGetVersion) get_version =
	    runtime.Get<decltype(&xessGetVersion)>("xessGetVersion", valid);
	decltype(&xessVKGetRequiredInstanceExtensions) instance_extensions =
	    runtime.Get<decltype(&xessVKGetRequiredInstanceExtensions)>(
	        "xessVKGetRequiredInstanceExtensions", valid);
	decltype(&xessVKGetRequiredDeviceExtensions) device_extensions =
	    runtime.Get<decltype(&xessVKGetRequiredDeviceExtensions)>(
	        "xessVKGetRequiredDeviceExtensions", valid);
	decltype(&xessVKGetRequiredDeviceFeatures) device_features =
	    runtime.Get<decltype(&xessVKGetRequiredDeviceFeatures)>("xessVKGetRequiredDeviceFeatures",
	                                                            valid);
	decltype(&xessVKCreateContext) create =
	    runtime.Get<decltype(&xessVKCreateContext)>("xessVKCreateContext", valid);
	decltype(&xessVKBuildPipelines) build_pipelines =
	    runtime.Get<decltype(&xessVKBuildPipelines)>("xessVKBuildPipelines", valid);
	decltype(&xessVKInit)    init = runtime.Get<decltype(&xessVKInit)>("xessVKInit", valid);
	decltype(&xessVKExecute) execute =
	    runtime.Get<decltype(&xessVKExecute)>("xessVKExecute", valid);
	decltype(&xessDestroyContext) destroy =
	    runtime.Get<decltype(&xessDestroyContext)>("xessDestroyContext", valid);
	decltype(&xessGetOptimalInputResolution) optimal_input =
	    runtime.Get<decltype(&xessGetOptimalInputResolution)>("xessGetOptimalInputResolution",
	                                                          valid);
	decltype(&xessSetVelocityScale) velocity_scale =
	    runtime.Get<decltype(&xessSetVelocityScale)>("xessSetVelocityScale", valid);
	decltype(&xessSetLoggingCallback) logging =
	    runtime.Get<decltype(&xessSetLoggingCallback)>("xessSetLoggingCallback", valid);
};
const XessApi& Xess() {
	static XessApi api;
	return api;
}

struct FfxApi {
	Runtime              runtime {"amd_fidelityfx_vk.dll"};
	bool                 valid    = runtime.module != nullptr;
	PfnFfxCreateContext  create   = runtime.Get<PfnFfxCreateContext>("ffxCreateContext", valid);
	PfnFfxDestroyContext destroy  = runtime.Get<PfnFfxDestroyContext>("ffxDestroyContext", valid);
	PfnFfxQuery          query    = runtime.Get<PfnFfxQuery>("ffxQuery", valid);
	PfnFfxDispatch       dispatch = runtime.Get<PfnFfxDispatch>("ffxDispatch", valid);
};
const FfxApi& Ffx() {
	static FfxApi api;
	return api;
}

bool UsesXess() {
	return DirectUpscaler::Selected() &&
	       Config::GetOptiScalerUpscaler() == Config::OptiScalerUpscaler::XeSS;
}

bool HasExtension(const std::vector<vk::ExtensionProperties>& available, const char* name) {
	return std::any_of(available.begin(), available.end(), [&](const auto& extension) {
		return std::strcmp(extension.extensionName, name) == 0;
	});
}
void Enable(std::vector<const char*>& enabled, const char* name) {
	if (std::none_of(enabled.begin(), enabled.end(),
	                 [&](const char* item) { return std::strcmp(item, name) == 0; }))
		enabled.push_back(name);
}

// Feature structures are an sType/pNext header followed only by VkBool32 members.
template <class T>
VkBool32* Bools(T& structure) {
	return reinterpret_cast<VkBool32*>(reinterpret_cast<std::byte*>(&structure) +
	                                   sizeof(VkBaseOutStructure));
}
constexpr size_t BoolCount(size_t last_member_offset) {
	// sizeof(struct) may include trailing alignment padding, which is not a feature.
	return (last_member_offset - sizeof(VkBaseOutStructure)) / sizeof(VkBool32) + 1;
}
bool MergeBools(const VkBool32* required, const VkBool32* supported, VkBool32* enabled,
                size_t count, const char* structure) {
	for (size_t i = 0; i < count; ++i) {
		if (!required[i]) continue;
		if (!supported[i]) {
			LOGF("XeSS unavailable: unsupported device feature %s[%zu]\n", structure, i);
			return false;
		}
		enabled[i] = VK_TRUE;
	}
	return true;
}

// By scale ratio, so a mode renders at the same input size as DLSS and FSR: XeSS 1.3+
// names 1.5x UltraQuality, 1.7x Quality, 2.0x Balanced and 3.0x UltraPerformance.
xess_quality_settings_t XessQuality(Config::DlssMode mode) {
	switch (mode) {
		case Config::DlssMode::Balanced: return XESS_QUALITY_SETTING_QUALITY;
		case Config::DlssMode::Performance: return XESS_QUALITY_SETTING_BALANCED;
		case Config::DlssMode::UltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
		case Config::DlssMode::DLAA: return XESS_QUALITY_SETTING_AA;
		default: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
	}
}
float FsrRatio(Config::DlssMode mode) {
	switch (mode) {
		case Config::DlssMode::Balanced: return 1.7f;
		case Config::DlssMode::Performance: return 2.0f;
		case Config::DlssMode::UltraPerformance: return 3.0f;
		case Config::DlssMode::DLAA: return 1.0f;
		default: return 1.5f;
	}
}

void XessLog(const char* message, xess_logging_level_t level) {
	if (level >= XESS_LOGGING_LEVEL_WARNING) LOGF("XeSS: %s\n", message);
}
void FfxLog(uint32_t type, const wchar_t* message) {
	std::string text;
	for (const wchar_t* c = message; c && *c; ++c)
		text.push_back(*c < 128 ? char(*c) : '?');
	LOGF("FidelityFX %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning",
	     text.c_str());
}

xess_vk_image_view_info XessView(Image& image, vk::ImageAspectFlags aspect) {
	ImageViewInfo view {};
	view.format = image.backing.format;
	view.aspect = aspect;
	xess_vk_image_view_info info {};
	info.imageView        = image.FindView(view);
	info.image            = image.backing.image;
	info.subresourceRange = {static_cast<VkImageAspectFlags>(aspect), 0, 1, 0, 1};
	info.format           = static_cast<VkFormat>(image.backing.format);
	info.width            = image.backing.extent.width;
	info.height           = image.backing.extent.height;
	return info;
}

FfxApiResource FfxImage(VkImage image, vk::Format format, vk::Extent2D extent, bool writable) {
	FfxApiResourceDescription description {};
	description.type     = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	description.format   = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(format));
	description.width    = extent.width;
	description.height   = extent.height;
	description.depth    = 1;
	description.mipCount = 1;
	description.flags    = FFX_API_RESOURCE_FLAGS_NONE;
	description.usage    = writable ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
	return ffxApiGetResourceVK(image, description,
	                           writable ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
	                                    : FFX_API_RESOURCE_STATE_COMPUTE_READ);
}

} // namespace

bool DirectUpscaler::Selected() {
#if defined(_WIN32)
	return Config::GetUpscaleBackend() == Config::UpscaleBackend::OptiScaler &&
	       Config::GetOptiScalerUpscaler() != Config::OptiScalerUpscaler::Auto &&
	       Config::GetDlssMode() != Config::DlssMode::Off;
#else
	return false;
#endif
}

bool DirectUpscaler::AppendInstanceExtensions(
    std::vector<const char*>& enabled, const std::vector<vk::ExtensionProperties>& available) {
	if (!Selected()) return false;
	if (!UsesXess()) return Ffx().valid;
	const auto& api = Xess();
	if (!api.valid) return false;
	uint32_t           count = 0, version = 0;
	const char* const* names = nullptr;
	if (api.instance_extensions(&count, &names, &version) != XESS_RESULT_SUCCESS) return false;
	for (uint32_t i = 0; i < count; ++i) {
		if (!HasExtension(available, names[i])) {
			LOGF("XeSS unavailable: missing Vulkan instance extension %s\n", names[i]);
			return false;
		}
		Enable(enabled, names[i]);
	}
	return true;
}

bool DirectUpscaler::AppendDeviceExtensions(GraphicContext&                             graphics,
                                            std::vector<const char*>&                   enabled,
                                            const std::vector<vk::ExtensionProperties>& available) {
	if (!Selected()) return false;
	// The FidelityFX Vulkan backend resolves vkGetBufferMemoryRequirements2KHR by its
	// extension alias, which is null unless the (core 1.1) extension is enabled.
	for (const char* name:
	     {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME}) {
		if (!HasExtension(available, name)) return false;
		Enable(enabled, name);
	}
	if (!UsesXess()) return Ffx().valid;
	const auto& api = Xess();
	if (!api.valid) return false;
	uint32_t           count = 0;
	const char* const* names = nullptr;
	if (api.device_extensions(graphics.instance, graphics.physical_device, &count, &names) !=
	    XESS_RESULT_SUCCESS)
		return false;
	for (uint32_t i = 0; i < count; ++i) {
		if (!HasExtension(available, names[i])) {
			LOGF("XeSS unavailable: missing Vulkan device extension %s\n", names[i]);
			return false;
		}
	}
	for (uint32_t i = 0; i < count; ++i)
		Enable(enabled, names[i]);
	return true;
}

bool DirectUpscaler::EnableDeviceFeatures(
    GraphicContext& graphics, vk::PhysicalDeviceFeatures& features,
    vk::PhysicalDeviceVulkan11Features& features11, vk::PhysicalDeviceVulkan12Features& features12,
    vk::PhysicalDeviceVulkan13Features&                 features13,
    vk::PhysicalDeviceMutableDescriptorTypeFeaturesEXT& mutable_descriptor, const void*& next) {
	if (!UsesXess()) return true;
	const auto& api = Xess();
	if (!api.valid) return false;
	VkPhysicalDeviceFeatures2 head {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	void*                     chain = &head;
	if (api.device_features(graphics.instance, graphics.physical_device, &chain) !=
	    XESS_RESULT_SUCCESS)
		return false;
	vk::PhysicalDeviceMutableDescriptorTypeFeaturesEXT supported_mutable {};
	vk::PhysicalDeviceVulkan13Features                 supported13 {};
	supported13.pNext = &supported_mutable;
	vk::PhysicalDeviceVulkan12Features supported12 {};
	supported12.pNext = &supported13;
	vk::PhysicalDeviceVulkan11Features supported11 {};
	supported11.pNext = &supported12;
	vk::PhysicalDeviceFeatures2 supported {};
	supported.pNext = &supported11;
	graphics.physical_device.getFeatures2(&supported);
	for (auto* node = static_cast<VkBaseOutStructure*>(chain); node != nullptr;
	     node       = node->pNext) {
		bool ok = true;
		switch (node->sType) {
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
				ok = MergeBools(reinterpret_cast<const VkBool32*>(
				                    &reinterpret_cast<VkPhysicalDeviceFeatures2*>(node)->features),
				                reinterpret_cast<const VkBool32*>(&supported.features),
				                reinterpret_cast<VkBool32*>(&features),
				                sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32),
				                "VkPhysicalDeviceFeatures");
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
				ok = MergeBools(
				    Bools(*node), Bools(supported11), Bools(features11),
				    BoolCount(offsetof(VkPhysicalDeviceVulkan11Features, shaderDrawParameters)),
				    "Vulkan11Features");
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
				ok = MergeBools(Bools(*node), Bools(supported12), Bools(features12),
				                BoolCount(offsetof(VkPhysicalDeviceVulkan12Features,
				                                   subgroupBroadcastDynamicId)),
				                "Vulkan12Features");
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
				ok = MergeBools(Bools(*node), Bools(supported13), Bools(features13),
				                BoolCount(offsetof(VkPhysicalDeviceVulkan13Features, maintenance4)),
				                "Vulkan13Features");
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:
				ok = supported_mutable.mutableDescriptorType == VK_TRUE;
				if (ok && !mutable_descriptor.mutableDescriptorType) {
					mutable_descriptor.mutableDescriptorType = VK_TRUE;
					mutable_descriptor.pNext                 = const_cast<void*>(next);
					next                                     = &mutable_descriptor;
				}
				break;
			default:
				LOGF("XeSS unavailable: unhandled device feature structure %d\n", int(node->sType));
				ok = false;
		}
		if (!ok) return false;
	}
	return true;
}

struct DirectUpscaler::Impl {
	GraphicContext&            graphics;
	CommandScheduler&          scheduler;
	Config::OptiScalerUpscaler kind;
	bool                       available = false, reset = true, logged = false;
	// XeSS
	xess_context_handle_t   xess         = nullptr;
	bool                    xess_ready   = false;
	uint32_t                xess_flags   = 0;
	xess_quality_settings_t xess_quality = XESS_QUALITY_SETTING_QUALITY;
	vk::Extent2D            xess_output {};
	float                   velocity_x = 0, velocity_y = 0;
	// FidelityFX
	ffxContext   ffx       = nullptr;
	uint32_t     ffx_flags = 0;
	vk::Extent2D ffx_output {};

	Impl(GraphicContext& owner, CommandScheduler& commands)
	    : graphics(owner), scheduler(commands), kind(Config::GetOptiScalerUpscaler()) {
		if (kind == Config::OptiScalerUpscaler::XeSS) {
			const auto& api = Xess();
			if (!api.valid || !graphics.dlss_extensions_enabled) return;
			xess_version_t version {};
			api.get_version(&version);
			xess_context_handle_t context = nullptr;
			const auto            result =
			    api.create(graphics.instance, graphics.physical_device, graphics.device, &context);
			if (result < XESS_RESULT_SUCCESS) {
				LOGF("XeSS %u.%u.%u context creation failed: %d; using normal presentation\n",
				     version.major, version.minor, version.patch, int(result));
				return;
			}
			api.logging(context, XESS_LOGGING_LEVEL_WARNING, XessLog);
			// Compile in the background; xessVKInit waits for it.
			api.build_pipelines(context, VK_NULL_HANDLE, false,
			                    XESS_INIT_FLAG_LDR_INPUT_COLOR |
			                        XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK);
			xess      = context;
			available = true;
			LOGF("XeSS %u.%u.%u Super Resolution (direct): available\n", version.major,
			     version.minor, version.patch);
		} else {
			available = Ffx().valid && graphics.dlss_extensions_enabled;
			LOGF("FidelityFX Super Resolution (direct): %s\n",
			     available ? "available" : "unavailable");
		}
	}
	~Impl() {
		RetireXess();
		RetireFfx();
	}
	void RetireXess() {
		if (!xess) return;
		auto cleanup = [context = xess] { Xess().destroy(context); };
		xess         = nullptr;
		xess_ready   = false;
		if (scheduler.Active())
			scheduler.DeferOperation(std::move(cleanup));
		else
			cleanup();
	}
	void RetireFfx() {
		if (!ffx) return;
		auto cleanup = [context = ffx]() mutable { Ffx().destroy(&context, nullptr); };
		ffx          = nullptr;
		if (scheduler.Active())
			scheduler.DeferOperation(std::move(cleanup));
		else
			cleanup();
	}

	bool EvaluateXess(CommandBuffer& command, const DlssFrameInputs& inputs, VulkanImage& output,
	                  vk::ImageView output_view, vk::Extent2D input, vk::Extent2D output_size,
	                  bool native_depth) {
		const auto& api   = Xess();
		uint32_t    flags = inputs.hdr ? uint32_t(XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE)
		                               : uint32_t(XESS_INIT_FLAG_LDR_INPUT_COLOR);
		if (inputs.depth_inverted) flags |= XESS_INIT_FLAG_INVERTED_DEPTH;
		if (inputs.bias_current_color) flags |= XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
		const auto quality = XessQuality(Config::GetDlssMode());
		if (!xess_ready || xess_output != output_size || xess_flags != flags ||
		    xess_quality != quality) {
			if (xess_ready) {
				// Initialization replaces resources the GPU may still read: use a new context.
				RetireXess();
				const auto created =
				    api.create(graphics.instance, graphics.physical_device, graphics.device, &xess);
				if (created < XESS_RESULT_SUCCESS) return Fail("context creation", int(created));
				api.logging(xess, XESS_LOGGING_LEVEL_WARNING, XessLog);
			}
			xess_vk_init_params_t init {};
			init.outputResolution = {output_size.width, output_size.height};
			init.qualitySetting   = quality;
			init.initFlags        = flags;
			init.creationNodeMask = init.visibleNodeMask = 1;
			// Positive results are warnings (an older driver, for example).
			const auto result = api.init(xess, &init);
			if (result < XESS_RESULT_SUCCESS) return Fail("initialization", int(result));
			xess_ready   = true;
			xess_output  = output_size;
			xess_flags   = flags;
			xess_quality = quality;
			velocity_x = velocity_y = 0;
			reset                   = true;
		}
		if (velocity_x != inputs.motion_scale_x || velocity_y != inputs.motion_scale_y) {
			api.velocity_scale(xess, inputs.motion_scale_x, inputs.motion_scale_y);
			velocity_x = inputs.motion_scale_x;
			velocity_y = inputs.motion_scale_y;
		}
		const auto               handle = command.Handle();
		xess_vk_execute_params_t params {};
		params.colorTexture    = XessView(*inputs.color, vk::ImageAspectFlagBits::eColor);
		params.velocityTexture = XessView(*inputs.motion_vectors, vk::ImageAspectFlagBits::eColor);
		params.depthTexture =
		    XessView(*inputs.depth, native_depth ? vk::ImageAspectFlagBits::eDepth
		                                         : vk::ImageAspectFlagBits::eColor);
		for (auto* image:
		     {inputs.color, inputs.motion_vectors, inputs.depth, inputs.bias_current_color}) {
			if (image)
				image->Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
				               vk::AccessFlagBits2::eShaderRead, {}, handle);
		}
		if (inputs.bias_current_color)
			params.responsivePixelMaskTexture =
			    XessView(*inputs.bias_current_color, vk::ImageAspectFlagBits::eColor);
		params.outputTexture.imageView        = output_view;
		params.outputTexture.image            = output.image;
		params.outputTexture.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		params.outputTexture.format           = static_cast<VkFormat>(output.format);
		params.outputTexture.width            = output_size.width;
		params.outputTexture.height           = output_size.height;
		params.jitterOffsetX                  = inputs.jitter_x;
		params.jitterOffsetY                  = inputs.jitter_y;
		params.exposureScale                  = 1.0f;
		params.resetHistory                   = reset || inputs.reset_history;
		params.inputWidth                     = input.width;
		params.inputHeight                    = input.height;
		PrepareOutput(handle, output);
		const auto result = api.execute(xess, handle, &params);
		if (result < XESS_RESULT_SUCCESS) return Fail("execution", int(result));
		return true;
	}

	bool EvaluateFfx(CommandBuffer& command, const DlssFrameInputs& inputs, VulkanImage& output,
	                 vk::Extent2D input, vk::Extent2D output_size) {
		const auto& api   = Ffx();
		uint32_t    flags = 0;
		if (inputs.depth_inverted) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
		if (inputs.hdr)
			flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
		if (!ffx || ffx_output != output_size || ffx_flags != flags) {
			RetireFfx();
			ffxCreateBackendVKDesc backend {};
			backend.header.type      = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
			backend.vkDevice         = graphics.device;
			backend.vkPhysicalDevice = graphics.physical_device;
			backend.vkDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
			ffxCreateContextDescUpscale create {};
			create.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
			create.header.pNext = &backend.header;
			create.flags        = flags;
			// Any render size up to the output: the guest's own resolution is reconstructed.
			create.maxRenderSize  = {output_size.width, output_size.height};
			create.maxUpscaleSize = {output_size.width, output_size.height};
			create.fpMessage      = FfxLog;
			const auto result     = api.create(&ffx, &create.header, nullptr);
			if (result != FFX_API_RETURN_OK) {
				ffx = nullptr;
				return Fail("context creation", int(result));
			}
			ffx_output = output_size;
			ffx_flags  = flags;
			reset      = true;
			ffxQueryGetProviderVersion version {};
			version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
			if (api.query(&ffx, &version.header) == FFX_API_RETURN_OK && version.versionName)
				LOGF("FidelityFX upscaler provider: %s\n", version.versionName);
		}
		const auto handle = command.Handle();
		for (auto* image:
		     {inputs.color, inputs.motion_vectors, inputs.depth, inputs.bias_current_color}) {
			if (image)
				image->Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
				               vk::AccessFlagBits2::eShaderRead, {}, handle);
		}
		const auto resource = [&](Image* image) {
			return image ? FfxImage(image->backing.image, image->backing.format,
			                        {image->backing.extent.width, image->backing.extent.height},
			                        false)
			             : FfxApiResource {};
		};
		ffxDispatchDescUpscale dispatch {};
		dispatch.header.type       = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		dispatch.commandList       = static_cast<VkCommandBuffer>(handle);
		dispatch.color             = resource(inputs.color);
		dispatch.depth             = resource(inputs.depth);
		dispatch.motionVectors     = resource(inputs.motion_vectors);
		dispatch.reactive          = resource(inputs.bias_current_color);
		dispatch.output            = FfxImage(output.image, output.format, output_size, true);
		dispatch.jitterOffset      = {inputs.jitter_x, inputs.jitter_y};
		dispatch.motionVectorScale = {inputs.motion_scale_x, inputs.motion_scale_y};
		dispatch.renderSize        = {input.width, input.height};
		dispatch.upscaleSize       = {output_size.width, output_size.height};
		dispatch.frameTimeDelta    = inputs.frame_time_ms;
		dispatch.preExposure       = 1.0f;
		dispatch.reset             = reset || inputs.reset_history;
		// The emulator's final-frame depth has no camera: OptiScaler's FSR defaults.
		dispatch.cameraNear              = inputs.depth_inverted ? 10000.0f : 0.1f;
		dispatch.cameraFar               = inputs.depth_inverted ? 0.1f : 10000.0f;
		dispatch.cameraFovAngleVertical  = 1.0471976f;
		dispatch.viewSpaceToMetersFactor = 1.0f;
		PrepareOutput(handle, output);
		const auto result = api.dispatch(&ffx, &dispatch.header);
		if (result != FFX_API_RETURN_OK) return Fail("dispatch", int(result));
		return true;
	}

	static void PrepareOutput(vk::CommandBuffer handle, VulkanImage& output) {
		vk::ImageMemoryBarrier2 barrier {};
		barrier.srcStageMask  = output.state.pl_stage;
		barrier.srcAccessMask = output.state.access_mask;
		barrier.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
		barrier.dstAccessMask =
		    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead;
		barrier.oldLayout           = output.state.layout;
		barrier.newLayout           = vk::ImageLayout::eGeneral;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = output.image;
		barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		vk::DependencyInfo dependency {};
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers    = &barrier;
		handle.pipelineBarrier2(dependency);
		output.state = {vk::PipelineStageFlagBits2::eComputeShader,
		                vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral};
		output.subresource_states.clear();
	}

	bool Fail(const char* operation, int result) {
		LOGF("%s %s failed: %d; using normal presentation\n",
		     kind == Config::OptiScalerUpscaler::XeSS ? "XeSS" : "FidelityFX", operation, result);
		available = false;
		reset     = true;
		return false;
	}
};

DirectUpscaler::DirectUpscaler(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_impl(std::make_unique<Impl>(graphics, scheduler)) {}
DirectUpscaler::~DirectUpscaler() = default;

bool DirectUpscaler::Available() const {
	return m_impl->available;
}
const char* DirectUpscaler::Name() const {
	return m_impl->kind == Config::OptiScalerUpscaler::XeSS ? "XeSS" : "FSR";
}

std::optional<vk::Extent2D> DirectUpscaler::OptimalInputExtent(vk::Extent2D output,
                                                               vk::Extent2D source) const {
	auto& state = *m_impl;
	if (!state.available || output.width == 0 || output.height == 0) return std::nullopt;
	const auto mode = Config::GetDlssMode();
	if (mode == Config::DlssMode::DLAA) return output;
	vk::Extent2D optimal {}, minimum {}, maximum = output;
	if (state.kind == Config::OptiScalerUpscaler::XeSS) {
		const xess_2d_t target {output.width, output.height};
		xess_2d_t       best {}, low {}, high {};
		if (Xess().optimal_input(state.xess, &target, XessQuality(mode), &best, &low, &high) <
		    XESS_RESULT_SUCCESS)
			return std::nullopt;
		optimal = {best.x, best.y};
		minimum = {low.x, low.y};
		maximum = {high.x, high.y};
	} else {
		const float ratio = FsrRatio(mode);
		optimal           = {uint32_t(std::ceil(float(output.width) / ratio)),
		                     uint32_t(std::ceil(float(output.height) / ratio))};
		minimum           = {(output.width + 2) / 3, (output.height + 2) / 3};
	}
	if (source.width == 0 || source.height == 0) return optimal;
	// Reconstruct the guest's own resolution when the runtime accepts it.
	return vk::Extent2D {std::clamp(source.width, minimum.width, maximum.width),
	                     std::clamp(source.height, minimum.height, maximum.height)};
}

bool DirectUpscaler::Evaluate(CommandBuffer& command, const DlssFrameInputs& inputs,
                              VulkanImage& output, vk::ImageView output_view) {
	auto& state = *m_impl;
	if (!state.available) return false;
	const vk::Extent2D input {inputs.color->backing.extent.width,
	                          inputs.color->backing.extent.height};
	const vk::Extent2D output_size {output.extent.width, output.extent.height};
	const auto         depth_format = inputs.depth->backing.format;
	const bool         native_depth =
	    depth_format == vk::Format::eD32Sfloat || depth_format == vk::Format::eD16Unorm;
	command.EndRendering();
	const bool evaluated = state.kind == Config::OptiScalerUpscaler::XeSS
	                           ? state.EvaluateXess(command, inputs, output, output_view, input,
	                                                output_size, native_depth)
	                           : state.EvaluateFfx(command, inputs, output, input, output_size);
	if (evaluated) {
		state.reset = false;
		if (!state.logged) {
			LOGF("%s Super Resolution (direct) active: %ux%u -> %ux%u\n", Name(), input.width,
			     input.height, output_size.width, output_size.height);
			state.logged = true;
		}
	}
	return evaluated;
}

void DirectUpscaler::SkipFrame() {
	m_impl->reset = true;
}

} // namespace Libs::Graphics
