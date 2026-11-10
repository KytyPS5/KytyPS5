#include "graphics/host_gpu/renderer/dlss.h"

#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/shaderBindings.h"

#include <SDL3/SDL_loadso.h>
#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Libs::Graphics::Dlss {
namespace {

using Result = int;

constexpr int         SDK_VERSION    = 0x15;
constexpr int         ENGINE_CUSTOM  = 0;
constexpr int         SUPER_SAMPLING = 1;
constexpr const char* PROJECT_ID     = "a10e2777-9818-4681-b625-25d71ae45d72";

constexpr int DLAA = 5; // the last quality mode; it renders at the output size

// Feature creation flags.
constexpr int IS_HDR         = 1 << 0;
constexpr int MV_LOW_RES     = 1 << 1;
constexpr int DEPTH_INVERTED = 1 << 3;
constexpr int AUTO_EXPOSURE  = 1 << 6;

// The request flags a feature fixes at creation.
constexpr uint32_t FIXED_FLAGS = Pssr::kReverseDepth | Pssr::kAutoExposure;

struct FeatureCommonInfo {
	const wchar_t* const* paths;
	uint32_t              path_count;
	void*                 internal;
	void*                 log_callback;
	int32_t               log_level;
	uint8_t               log_callback_only;
};
static_assert(sizeof(FeatureCommonInfo) == 40);
static_assert(sizeof(Resource) == 56);

struct Api {
	Result (*required_extensions)(uint32_t*, const char***, uint32_t*, const char***);
	Result (*init)(const char*, int, const char*, const wchar_t*, VkInstance, VkPhysicalDevice,
	               VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int,
	               const FeatureCommonInfo*);
	Result (*shutdown)(VkDevice, int*);
	Result (*capabilities)(Parameter**);
	Result (*allocate)(Parameter**);
	Result (*destroy)(Parameter*);
	Result (*create)(VkDevice, VkCommandBuffer, int, Parameter*, Feature**);
	Result (*release)(Feature*);
	Result (*evaluate)(VkCommandBuffer, const Feature*, Parameter*, void*);
};

SDL_SharedObject* g_module = nullptr;
Api               g_api {};
VkDevice          g_device       = nullptr;
Parameter*        g_capabilities = nullptr;
Parameter*        g_parameters   = nullptr;

bool Succeeded(Result result) {
	return (static_cast<uint32_t>(result) & 0xfff00000u) != 0xbad00000u;
}

void Warn(std::string_view text) {
	Log::WriteToConsoleAndLog(fmt::format("WARNING: DLSS: {}; PSSR scales linearly.\n", text));
}

void OpenCore() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// The driver's service install records its folder.
	wchar_t folder[MAX_PATH];
	DWORD   size = sizeof(folder);
	if (RegGetValueW(HKEY_LOCAL_MACHINE,
	                 L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore",
	                 L"NGXPath", RRF_RT_REG_SZ, nullptr, folder, &size) == ERROR_SUCCESS) {
		g_module = SDL_LoadObject(
		    Common::PathToString(std::filesystem::path(folder) / L"_nvngx.dll").c_str());
	}
#else
	g_module = SDL_LoadObject("libnvidia-ngx.so.1");
#endif
}

void CloseCore() {
	SDL_UnloadObject(g_module);
	g_module = nullptr;
}

template <typename Function>
bool Bind(Function& function, const char* name) {
	function = reinterpret_cast<Function>(SDL_LoadFunction(g_module, name));
	return function != nullptr;
}

} // namespace

bool Accepts(const Pssr::Request& request) {
	ShaderTextureResource depth {}, motion {};
	std::copy_n(request.depth, 8, depth.fields);
	std::copy_n(request.motion, 8, motion.fields);
	return (request.flags & Pssr::kSpatialOnly) == 0 && !depth.IsNull() && !motion.IsNull();
}

bool Takes(const Creation& creation, const Pssr::Request& request, const Inputs& inputs) {
	return creation.output_width == inputs.output.width &&
	       creation.output_height == inputs.output.height &&
	       creation.flags == (request.flags & FIXED_FLAGS) && request.width >= creation.min_width &&
	       request.width <= creation.max_width && request.height >= creation.min_height &&
	       request.height <= creation.max_height;
}

std::optional<Creation> Choose(Parameter& capabilities, const Pssr::Request& request,
                               const Inputs& inputs) {
	void* callback = nullptr;
	if (!Succeeded(capabilities.Get("DLSSOptimalSettingsCallback", &callback)) ||
	    callback == nullptr) {
		return std::nullopt;
	}
	const auto settings = reinterpret_cast<Result (*)(Parameter*)>(callback);
	const auto read     = [&](const char* name, uint32_t& value) {
		uint32_t found = 0;
		if (Succeeded(capabilities.Get(name, &found)) && found != 0) {
			value = found;
		}
	};
	std::optional<Creation> best;
	uint32_t                best_distance = 0;
	const uint32_t          flags         = request.flags & FIXED_FLAGS;
	for (int mode = 0; mode <= DLAA; ++mode) {
		capabilities.Set("Width", inputs.output.width);
		capabilities.Set("Height", inputs.output.height);
		capabilities.Set("PerfQualityValue", mode);
		capabilities.Set("RTXValue", 0);
		uint32_t optimal_width  = 0;
		uint32_t optimal_height = 0;
		if (!Succeeded(settings(&capabilities))) {
			continue;
		}
		read("OutWidth", optimal_width);
		read("OutHeight", optimal_height);
		if (optimal_width == 0 || optimal_height == 0) {
			continue; // the model lacks the mode
		}
		Creation creation {
		    inputs.output.width, inputs.output.height, flags,         mode,
		    optimal_width,       optimal_height,       optimal_width, optimal_height};
		if (mode == DLAA) {
			creation.min_width = creation.max_width = inputs.output.width;
			creation.min_height = creation.max_height = inputs.output.height;
		} else {
			// Without a dynamic range the mode takes its optimal size only.
			read("DLSS.Get.Dynamic.Min.Render.Width", creation.min_width);
			read("DLSS.Get.Dynamic.Min.Render.Height", creation.min_height);
			read("DLSS.Get.Dynamic.Max.Render.Width", creation.max_width);
			read("DLSS.Get.Dynamic.Max.Render.Height", creation.max_height);
		}
		const auto distance =
		    std::max(optimal_width, request.width) - std::min(optimal_width, request.width);
		if (Takes(creation, request, inputs) && (!best || distance < best_distance)) {
			best          = creation;
			best_distance = distance;
		}
	}
	return best;
}

void SetCreation(Parameter& parameters, const Creation& creation, uint32_t preset) {
	int flags = IS_HDR | MV_LOW_RES;
	if ((creation.flags & Pssr::kReverseDepth) != 0) {
		flags |= DEPTH_INVERTED;
	}
	if ((creation.flags & Pssr::kAutoExposure) != 0) {
		flags |= AUTO_EXPOSURE;
	}
	parameters.Set("CreationNodeMask", 1u);
	parameters.Set("VisibilityNodeMask", 1u);
	parameters.Set("Width", creation.max_width);
	parameters.Set("Height", creation.max_height);
	parameters.Set("OutWidth", creation.output_width);
	parameters.Set("OutHeight", creation.output_height);
	parameters.Set("PerfQualityValue", creation.mode);
	parameters.Set("DLSS.Feature.Create.Flags", flags);
	for (const char* hint:
	     {"DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
	      "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
	      "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"}) {
		parameters.Set(hint, preset);
	}
}

void SetEvaluation(Parameter& parameters, const Pssr::Request& request, Inputs& inputs,
                   bool reset) {
	parameters.Set("Color", static_cast<void*>(&inputs.color));
	parameters.Set("Depth", static_cast<void*>(&inputs.depth));
	parameters.Set("MotionVectors", static_cast<void*>(&inputs.motion));
	parameters.Set("ExposureTexture", static_cast<void*>(inputs.exposure));
	parameters.Set("Output", static_cast<void*>(&inputs.output));
	parameters.Set("Jitter.Offset.X", request.jitter[0]);
	parameters.Set("Jitter.Offset.Y", request.jitter[1]);
	parameters.Set("MV.Scale.X", request.motion_scale[0]);
	parameters.Set("MV.Scale.Y", request.motion_scale[1]);
	parameters.Set("DLSS.Pre.Exposure", request.pre_exposure);
	parameters.Set("Reset", reset ? 1 : 0);
	parameters.Set("DLSS.Render.Subrect.Dimensions.Width", request.width);
	parameters.Set("DLSS.Render.Subrect.Dimensions.Height", request.height);
	parameters.Set("DLSS.Input.Color.Subrect.Base.X", request.x);
	parameters.Set("DLSS.Input.Color.Subrect.Base.Y", request.y);
	parameters.Set("DLSS.Input.Depth.Subrect.Base.X", request.x);
	parameters.Set("DLSS.Input.Depth.Subrect.Base.Y", request.y);
	parameters.Set("DLSS.Input.MV.Subrect.Base.X", request.x);
	parameters.Set("DLSS.Input.MV.Subrect.Base.Y", request.y);
}

void Load() {
	if (!Config::Ps5ProEnabled() || Config::GetDlssFolder().empty()) {
		return;
	}
	OpenCore();
	if (g_module == nullptr) {
		Warn("the NVIDIA driver has no NGX library");
		return;
	}
	if (!Bind(g_api.required_extensions, "NVSDK_NGX_VULKAN_RequiredExtensions") ||
	    !Bind(g_api.init, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext") ||
	    !Bind(g_api.shutdown, "NVSDK_NGX_VULKAN_Shutdown1") ||
	    !Bind(g_api.capabilities, "NVSDK_NGX_VULKAN_GetCapabilityParameters") ||
	    !Bind(g_api.allocate, "NVSDK_NGX_VULKAN_AllocateParameters") ||
	    !Bind(g_api.destroy, "NVSDK_NGX_VULKAN_DestroyParameters") ||
	    !Bind(g_api.create, "NVSDK_NGX_VULKAN_CreateFeature1") ||
	    !Bind(g_api.release, "NVSDK_NGX_VULKAN_ReleaseFeature") ||
	    !Bind(g_api.evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature")) {
		Warn("the NVIDIA driver's NGX library lacks an entry point");
		CloseCore();
	}
}

void AppendExtensions(std::vector<const char*>&                   out,
                      const std::vector<vk::ExtensionProperties>& available, bool device) {
	std::array<uint32_t, 2>     counts {};
	std::array<const char**, 2> names {};
	if (g_module == nullptr ||
	    !Succeeded(g_api.required_extensions(&counts[0], &names[0], &counts[1], &names[1]))) {
		return;
	}
	for (const char* name: std::span(names[device], counts[device])) {
		const auto same = [&](std::string_view other) { return other == name; };
		if (std::ranges::any_of(available, same,
		                        [](const auto& properties) {
			                        return std::string_view(properties.extensionName.data());
		                        }) &&
		    std::ranges::none_of(out, same)) {
			out.push_back(name);
		}
	}
}

void Init(const GraphicContext& graphics, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
          PFN_vkGetDeviceProcAddr get_device_proc_addr) {
	if (g_module == nullptr) {
		return;
	}
	// Static, since NGX may keep these pointers.
	static const std::wstring      path = Config::GetDlssFolder().wstring();
	static const wchar_t* const    paths[] {path.c_str()};
	static const FeatureCommonInfo info {.paths = paths, .path_count = 1};
	const auto                     folder = Common::PathToString(Config::GetDlssFolder());
	if (const auto result = g_api.init(
	        PROJECT_ID, ENGINE_CUSTOM, "1.0", paths[0], static_cast<VkInstance>(graphics.instance),
	        static_cast<VkPhysicalDevice>(graphics.physical_device),
	        static_cast<VkDevice>(graphics.device), get_instance_proc_addr, get_device_proc_addr,
	        SDK_VERSION, &info);
	    !Succeeded(result)) {
		Warn(fmt::format("NGX did not start (0x{:08x})", static_cast<uint32_t>(result)));
		CloseCore();
		return;
	}
	g_device      = static_cast<VkDevice>(graphics.device);
	int available = 0;
	int status    = 0;
	if (!Succeeded(g_api.capabilities(&g_capabilities)) ||
	    !Succeeded(g_capabilities->Get("SuperSampling.Available", &available)) || available == 0) {
		if (g_capabilities != nullptr) {
			(void)g_capabilities->Get("SuperSampling.FeatureInitResult", &status);
		}
		Warn(fmt::format("super resolution is unavailable (0x{:08x}); check the model in {}",
		                 static_cast<uint32_t>(status), folder));
		Shutdown();
		return;
	}
	if (!Succeeded(g_api.allocate(&g_parameters))) {
		Warn("NGX parameters could not be allocated");
		Shutdown();
		return;
	}
	Log::WriteToConsoleAndLog(fmt::format("DLSS: upscaling PSSR with the model in {}\n", folder));
}

void Shutdown() {
	if (g_parameters != nullptr) {
		g_api.destroy(g_parameters);
		g_parameters = nullptr;
	}
	if (g_capabilities != nullptr) {
		g_api.destroy(g_capabilities);
		g_capabilities = nullptr;
	}
	if (g_device != nullptr) {
		int in_use = 0;
		g_api.shutdown(g_device, &in_use);
		g_device = nullptr;
	}
	CloseCore();
}

Parameter* Capabilities() {
	return g_capabilities;
}

Feature* CreateFeature(vk::CommandBuffer command, const Creation& creation) {
	SetCreation(*g_parameters, creation, Config::GetDlssPreset());
	Feature* feature = nullptr;
	if (const auto result = g_api.create(g_device, static_cast<VkCommandBuffer>(command),
	                                     SUPER_SAMPLING, g_parameters, &feature);
	    !Succeeded(result)) {
		Warn(fmt::format("creating a {}x{} to {}x{} feature failed (0x{:08x})", creation.max_width,
		                 creation.max_height, creation.output_width, creation.output_height,
		                 static_cast<uint32_t>(result)));
		return nullptr;
	}
	return feature;
}

bool Evaluate(vk::CommandBuffer command, const Feature* feature, const Pssr::Request& request,
              Inputs& inputs, bool reset) {
	SetEvaluation(*g_parameters, request, inputs, reset);
	if (const auto result =
	        g_api.evaluate(static_cast<VkCommandBuffer>(command), feature, g_parameters, nullptr);
	    !Succeeded(result)) {
		Warn(fmt::format("evaluation failed (0x{:08x})", static_cast<uint32_t>(result)));
		return false;
	}
	return true;
}

void ReleaseFeature(Feature* feature) {
	g_api.release(feature);
}

} // namespace Libs::Graphics::Dlss
