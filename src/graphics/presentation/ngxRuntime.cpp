#include "graphics/presentation/ngxRuntime.h"
#if defined(KYTY_HAS_DLSS)
#include "common/logging/log.h"
#include "graphics/presentation/upscaleRuntime.h"

#include <filesystem>
namespace Libs::Graphics {

template <class T>
void NgxRuntime::Load(T& entry, const char* name) {
	entry = reinterpret_cast<T>(SDL_LoadFunction(module, name));
	if (!entry) {
		LOGF("OptiScaler unavailable: missing export %s: %s\n", name, SDL_GetError());
		valid = false;
	}
}
NgxRuntime::NgxRuntime() {
	if (!external) return;
#if defined(_WIN32)
	const auto path = OptiScalerRuntimePath();
	if (!path) {
		valid = false;
		return;
	}
	const auto utf8 = path->u8string();
	module          = SDL_LoadObject(reinterpret_cast<const char*>(utf8.c_str()));
	if (!module) {
		LOGF("OptiScaler unavailable: cannot load %s: %s\n",
		     reinterpret_cast<const char*>(utf8.c_str()), SDL_GetError());
		valid = false;
		return;
	}
	Load(init, "NVSDK_NGX_VULKAN_Init");
	Load(instance_extensions, "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements");
	Load(device_extensions, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements");
	Load(capabilities, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
	Load(destroy_parameters, "NVSDK_NGX_VULKAN_DestroyParameters");
	Load(create_feature, "NVSDK_NGX_VULKAN_CreateFeature1");
	// The DLL exports EvaluateFeature; EvaluateFeature_C is an SDK wrapper.
	// Both callback ABIs are immaterial here because we always pass nullptr.
	Load(evaluate_feature, "NVSDK_NGX_VULKAN_EvaluateFeature");
	Load(release_feature, "NVSDK_NGX_VULKAN_ReleaseFeature");
	Load(shutdown, "NVSDK_NGX_VULKAN_Shutdown1");
	if (valid)
		LOGF("OptiScaler Vulkan runtime loaded: %s\n", reinterpret_cast<const char*>(utf8.c_str()));
#else
	LOGF("OptiScaler requires a Windows runtime in this build; using spatial presentation\n");
	valid = false;
#endif
}
NgxRuntime::~NgxRuntime() {
	if (module) SDL_UnloadObject(module);
}

std::shared_ptr<NgxRuntime> GetNgxRuntime() {
	static auto runtime = std::make_shared<NgxRuntime>();
	return runtime;
}

NVSDK_NGX_Result NgxRuntime::CreateDlss(VkDevice device, VkCommandBuffer command,
                                        NVSDK_NGX_Handle** feature, NVSDK_NGX_Parameter* parameters,
                                        NVSDK_NGX_DLSS_Create_Params& create) {
	if (!external)
		return NGX_VULKAN_CREATE_DLSS_EXT1(device, command, 1, 1, feature, parameters, &create);
	parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
	parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
	parameters->Set(NVSDK_NGX_Parameter_Width, create.Feature.InWidth);
	parameters->Set(NVSDK_NGX_Parameter_Height, create.Feature.InHeight);
	parameters->Set(NVSDK_NGX_Parameter_OutWidth, create.Feature.InTargetWidth);
	parameters->Set(NVSDK_NGX_Parameter_OutHeight, create.Feature.InTargetHeight);
	parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, int(create.Feature.InPerfQualityValue));
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, create.InFeatureCreateFlags);
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0u);
	return create_feature(device, command, NVSDK_NGX_Feature_SuperSampling, parameters, feature);
}

NVSDK_NGX_Result NgxRuntime::EvaluateDlss(VkCommandBuffer command, NVSDK_NGX_Handle* feature,
                                          NVSDK_NGX_Parameter*           parameters,
                                          NVSDK_NGX_VK_DLSS_Eval_Params& eval) {
	if (!external) return NGX_VULKAN_EVALUATE_DLSS_EXT(command, feature, parameters, &eval);
	parameters->Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(eval.Feature.pInColor));
	parameters->Set(NVSDK_NGX_Parameter_Output, static_cast<void*>(eval.Feature.pInOutput));
	parameters->Set(NVSDK_NGX_Parameter_Depth, static_cast<void*>(eval.pInDepth));
	parameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<void*>(eval.pInMotionVectors));
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
	                static_cast<void*>(eval.pInBiasCurrentColorMask));
	parameters->Set(NVSDK_NGX_Parameter_ExposureTexture, static_cast<void*>(nullptr));
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
	parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, eval.InJitterOffsetX);
	parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, eval.InJitterOffsetY);
	parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, eval.InMVScaleX);
	parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, eval.InMVScaleY);
	parameters->Set(NVSDK_NGX_Parameter_Reset, int(eval.InReset));
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
	                eval.InRenderSubrectDimensions.Width);
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
	                eval.InRenderSubrectDimensions.Height);
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, eval.InPreExposure);
	parameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, eval.InExposureScale);
	parameters->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, eval.InFrameTimeDeltaInMsec);
	return evaluate_feature(command, feature, parameters, nullptr);
}

} // namespace Libs::Graphics
#endif
