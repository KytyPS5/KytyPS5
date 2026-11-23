#include "graphics/host_gpu/renderer/upscaler/fsr.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/graphicContext.h"

#include <SDL3/SDL_loadso.h>
#include <algorithm>
#include <string_view>
#include <vulkan/vulkan_format_traits.hpp>

// AMD's headers declare the DLL exports for Windows only.
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#define __declspec(attribute)
#endif
#include <api/include/vk/ffx_api_vk.h>
#include <upscalers/include/ffx_upscale.h>
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#undef __declspec
#endif

namespace Libs::Graphics::Fsr {

using Dlss::Creation;
using Dlss::Inputs;

namespace {

struct Api {
	PfnFfxCreateContext  create;
	PfnFfxDestroyContext destroy;
	PfnFfxQuery          query;
	PfnFfxDispatch       dispatch;
};

SDL_SharedObject*      g_module = nullptr;
Api                    g_api {};
ffxCreateBackendVKDesc g_backend {};

void Warn(std::string_view text) {
	Log::WriteToConsoleAndLog(fmt::format("WARNING: FSR: {}; PSSR scales linearly.\n", text));
}

template <typename Function>
bool Bind(Function& function, const char* name) {
	function = reinterpret_cast<Function>(SDL_LoadFunction(g_module, name));
	return function != nullptr;
}

// Images stay in GENERAL, FidelityFX's unordered access state.
FfxApiResource Describe(const Dlss::Resource* resource) {
	if (resource == nullptr) {
		return {};
	}
	FfxApiResourceDescription description {};
	description.type     = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	description.format   = ffxApiGetSurfaceFormatVK(resource->format);
	description.width    = resource->width;
	description.height   = resource->height;
	description.depth    = 1;
	description.mipCount = 1;
	description.usage    = resource->read_write != 0 ? FFX_API_RESOURCE_USAGE_UAV : 0;
	if (ffxApiIsDepthFormat(resource->format)) {
		description.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
	}
	if (ffxApiIsStencilFormat(resource->format)) {
		description.usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;
	}
	return ffxApiGetResourceVK(resource->image, description,
	                           FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

} // namespace

// FidelityFX views images from their origin, in the formats it knows, sampled as floats.
bool Reads(const Pssr::Request& request, const Inputs& inputs) {
	const auto known = [](const Dlss::Resource* resource) {
		if (resource == nullptr) {
			return true;
		}
		const std::string_view numeric =
		    vk::componentNumericFormat(static_cast<vk::Format>(resource->format), 0);
		return ffxApiGetSurfaceFormatVK(resource->format) != FFX_API_SURFACE_FORMAT_UNKNOWN &&
		       numeric != "UINT" && numeric != "SINT";
	};
	return request.x == 0 && request.y == 0 && known(&inputs.color) && known(&inputs.depth) &&
	       known(&inputs.motion) && known(inputs.exposure) && known(&inputs.output);
}

std::optional<Creation> Choose(const Pssr::Request& request, const Inputs& inputs) {
	const Creation creation {.output_width  = inputs.output.width,
	                         .output_height = inputs.output.height,
	                         .flags         = request.flags & Dlss::FIXED_FLAGS,
	                         .min_width     = 1,
	                         .min_height    = 1,
	                         .max_width     = std::min(inputs.color.width, inputs.output.width),
	                         .max_height    = std::min(inputs.color.height, inputs.output.height)};
	if (!Dlss::Takes(creation, request, inputs)) {
		return std::nullopt;
	}
	return creation;
}

void SetCreation(ffxCreateContextDescUpscale& desc, const Creation& creation) {
	desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
	if ((creation.flags & Pssr::kReverseDepth) != 0) {
		desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
	}
	if ((creation.flags & Pssr::kAutoExposure) != 0) {
		desc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
	}
	desc.maxRenderSize  = {creation.max_width, creation.max_height};
	desc.maxUpscaleSize = {creation.output_width, creation.output_height};
}

void SetDispatch(ffxDispatchDescUpscale& desc, const Pssr::Request& request, const Inputs& inputs,
                 bool reset, float frame_time) {
	desc.color             = Describe(&inputs.color);
	desc.depth             = Describe(&inputs.depth);
	desc.motionVectors     = Describe(&inputs.motion);
	desc.exposure          = Describe(inputs.exposure);
	desc.output            = Describe(&inputs.output);
	desc.jitterOffset      = {request.jitter[0], request.jitter[1]};
	desc.motionVectorScale = {request.motion_scale[0], request.motion_scale[1]};
	desc.renderSize        = {request.width, request.height};
	desc.upscaleSize       = {inputs.output.width, inputs.output.height};
	desc.frameTimeDelta    = frame_time;
	desc.preExposure       = request.pre_exposure > 0.0f ? request.pre_exposure : 1.0f;
	desc.reset             = reset;
	// PSSR has no camera; these pass FidelityFX's checks.
	desc.cameraNear              = 0.1f;
	desc.cameraFar               = 1000.0f;
	desc.cameraFovAngleVertical  = 1.0f;
	desc.viewSpaceToMetersFactor = 1.0f;
}

void Load() {
	if (!Config::Ps5ProEnabled() || Config::GetPssrUpscaler() != Config::PssrUpscaler::FSR) {
		return;
	}
	const auto& folder = Config::GetFsrFolder();
	if (folder.empty()) {
		Warn("no folder is set");
		return;
	}
	g_module =
	    SDL_LoadObject(Common::PathToString(folder / "amd_fidelityfx_loader_vk.dll").c_str());
	if (g_module == nullptr) {
		Warn(fmt::format("{} has no amd_fidelityfx_loader_vk.dll for this system",
		                 Common::PathToString(folder)));
		return;
	}
	if (!Bind(g_api.create, "ffxCreateContext") || !Bind(g_api.destroy, "ffxDestroyContext") ||
	    !Bind(g_api.query, "ffxQuery") || !Bind(g_api.dispatch, "ffxDispatch")) {
		Warn("the FidelityFX loader lacks an entry point");
		Shutdown();
	}
}

void Init(const GraphicContext& graphics, PFN_vkGetDeviceProcAddr get_device_proc_addr) {
	if (g_module == nullptr) {
		return;
	}
	g_backend = {{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, nullptr},
	             {static_cast<VkDevice>(graphics.device),
	              static_cast<VkPhysicalDevice>(graphics.physical_device), get_device_proc_addr}};
	// The first version is the one contexts get.
	uint64_t                count   = 1;
	const char*             version = nullptr;
	ffxQueryDescGetVersions versions {{FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, nullptr},
	                                  FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
	                                  &g_backend.deviceContext,
	                                  &count,
	                                  nullptr,
	                                  &version};
	if (g_api.query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0 ||
	    version == nullptr) {
		Warn("amd_fidelityfx_upscaler_vk.dll is missing or has no upscaler for this GPU");
		Shutdown();
		return;
	}
	Log::WriteToConsoleAndLog(fmt::format("FSR: upscaling PSSR with FSR {} from the DLLs in {}\n",
	                                      version, Common::PathToString(Config::GetFsrFolder())));
}

void Shutdown() {
	SDL_UnloadObject(g_module);
	g_module  = nullptr;
	g_backend = {};
}

bool Enabled() {
	return g_module != nullptr;
}

Context* CreateContext(const Creation& creation) {
	ffxCreateContextDescUpscaleVersion version {
	    {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &g_backend.header},
	    FFX_UPSCALER_VERSION};
	ffxCreateContextDescUpscale desc {{FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &version.header}};
	SetCreation(desc, creation);
	ffxContext context = nullptr;
	if (const auto result = g_api.create(&context, &desc.header, nullptr);
	    result != FFX_API_RETURN_OK) {
		Warn(fmt::format("creating a {}x{} to {}x{} context failed ({})", creation.max_width,
		                 creation.max_height, creation.output_width, creation.output_height,
		                 result));
		return nullptr;
	}
	return static_cast<Context*>(context);
}

bool Evaluate(vk::CommandBuffer command, Context* context, const Pssr::Request& request,
              const Inputs& inputs, bool reset) {
	// PSSR has no frame time; the vblank period stands in. FidelityFX reuses a dispatch's views
	// three dispatches later, which one PSSR dispatch a frame keeps clear of the GPU.
	const float            frame_time = 1000.0f / static_cast<float>(Config::GetVblankFrequency());
	ffxDispatchDescUpscale desc {{FFX_API_DISPATCH_DESC_TYPE_UPSCALE, nullptr},
	                             static_cast<VkCommandBuffer>(command)};
	SetDispatch(desc, request, inputs, reset, frame_time);
	ffxContext ffx = context;
	if (const auto result = g_api.dispatch(&ffx, &desc.header); result != FFX_API_RETURN_OK) {
		Warn(fmt::format("dispatch failed ({})", result));
		return false;
	}
	return true;
}

void ReleaseContext(Context* context) {
	ffxContext ffx = context;
	g_api.destroy(&ffx, nullptr);
}

} // namespace Libs::Graphics::Fsr
