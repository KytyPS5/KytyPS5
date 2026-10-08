#ifndef KYTY_GRAPHICS_PRESENTATION_NGX_RUNTIME_H_
#define KYTY_GRAPHICS_PRESENTATION_NGX_RUNTIME_H_
#if defined(KYTY_HAS_DLSS)
#include "common/emulatorConfig.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <SDL3/SDL.h>
#include <memory>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
namespace Libs::Graphics {
// Handles and parameter maps always return to their originating runtime.
// Shared ownership keeps the DLL alive until deferred GPU cleanup finishes.
struct NgxRuntime final {

	SDL_SharedObject* module   = nullptr;
	bool              valid    = true;
	bool              external = Config::GetUpscaleBackend() == Config::UpscaleBackend::OptiScaler;
	decltype(&NVSDK_NGX_VULKAN_Init) init = &NVSDK_NGX_VULKAN_Init;
	decltype(&NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements) instance_extensions =
	    &NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements;
	decltype(&NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements) device_extensions =
	    &NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements;
	decltype(&NVSDK_NGX_VULKAN_GetCapabilityParameters) capabilities =
	    &NVSDK_NGX_VULKAN_GetCapabilityParameters;
	decltype(&NVSDK_NGX_VULKAN_DestroyParameters) destroy_parameters =
	    &NVSDK_NGX_VULKAN_DestroyParameters;
	decltype(&NVSDK_NGX_VULKAN_CreateFeature1)    create_feature = &NVSDK_NGX_VULKAN_CreateFeature1;
	decltype(&NVSDK_NGX_VULKAN_EvaluateFeature_C) evaluate_feature =
	    &NVSDK_NGX_VULKAN_EvaluateFeature_C;
	decltype(&NVSDK_NGX_VULKAN_ReleaseFeature) release_feature = &NVSDK_NGX_VULKAN_ReleaseFeature;
	decltype(&NVSDK_NGX_VULKAN_Shutdown1)      shutdown        = &NVSDK_NGX_VULKAN_Shutdown1;

	NgxRuntime();
	~NgxRuntime();
	NgxRuntime(const NgxRuntime&)                 = delete;
	NgxRuntime&      operator=(const NgxRuntime&) = delete;
	NVSDK_NGX_Result CreateDlss(VkDevice device, VkCommandBuffer command,
	                            NVSDK_NGX_Handle** feature, NVSDK_NGX_Parameter* parameters,
	                            NVSDK_NGX_DLSS_Create_Params& create);
	NVSDK_NGX_Result EvaluateDlss(VkCommandBuffer command, NVSDK_NGX_Handle* feature,
	                              NVSDK_NGX_Parameter*           parameters,
	                              NVSDK_NGX_VK_DLSS_Eval_Params& eval);

private:
	template <class T>
	void Load(T& entry, const char* name);
};
std::shared_ptr<NgxRuntime> GetNgxRuntime();
} // namespace Libs::Graphics
#endif
#endif
