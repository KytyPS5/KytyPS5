#include "graphics/presentation/dlssFrameGeneration.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/presentation/upscaleRuntime.h"
#include "graphics/presentation/xessFrameGeneration.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#if defined(KYTY_HAS_DLSS)
#include "graphics/presentation/ngxRuntime.h"

#include <nvsdk_ngx_helpers_vk.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(KYTY_HAS_DLSS_FG)
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_helpers_vk.h>
#include <sl_reflex.h>
#include <sl_security.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace Libs::Graphics {
DlssFgInputs::DlssFgInputs()  = default;
DlssFgInputs::~DlssFgInputs() = default;
bool DlssFgInputs::Ready(GraphicContext& graphics) const {
	if (!pending) return true;
	// Without a completion timeline, only the existing drain can prove safety.
	if (!completion) return false;
	uint64_t counter = 0;
	RequireVulkanSuccess(graphics.device.getSemaphoreCounterValue(completion, &counter),
	                     "query Frame Generation inputs");
	return counter >= completion_value;
}
void DlssFgInputs::Wait(GraphicContext& graphics) {
	if (!pending) return;
	// Zero is a valid timeline value (no input work queued yet). Draining the
	// whole device in that case interrupts the SDK's asynchronous frame pacer.
	if (completion) {
		vk::SemaphoreWaitInfo wait {};
		wait.semaphoreCount = 1;
		wait.pSemaphores    = &completion;
		wait.pValues        = &completion_value;
		RequireVulkanSuccess(graphics.device.waitSemaphores(&wait, UINT64_MAX),
		                     "wait for Frame Generation inputs");
	} else {
		Common::LockGuard queue_lock(graphics.queue_mutex);
		Common::LockGuard present_lock(graphics.present_queue_mutex);
		RequireVulkanSuccess(graphics.device.waitIdle(), "drain Frame Generation inputs");
	}
	pending          = false;
	completion       = nullptr;
	completion_value = 0;
}

bool CaptureDlssFgInputs(GraphicContext& graphics, CommandScheduler& scheduler,
                         CommandBuffer& command, const DlssFrameInputs& inputs,
                         std::unique_ptr<DlssFgInputs>& snapshot) {
	const auto valid_image = [](const Image* image) {
		const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc;
		return image && image->backing.image && image->backing.layers == 1 &&
		       image->backing.samples == 1 && image->backing.image_type == vk::ImageType::e2D &&
		       image->backing.extent.depth == 1 && image->backing.extent.width &&
		       image->backing.extent.height && (image->backing.usage & usage) == usage;
	};
	if (!valid_image(inputs.depth) || !valid_image(inputs.motion_vectors) ||
	    inputs.depth->backing.extent != inputs.motion_vectors->backing.extent ||
	    !std::isfinite(inputs.jitter_x) || !std::isfinite(inputs.jitter_y) ||
	    !std::isfinite(inputs.motion_scale_x) || !std::isfinite(inputs.motion_scale_y) ||
	    inputs.motion_scale_x == 0 || inputs.motion_scale_y == 0)
		return false;
	const auto depth_format  = inputs.depth->backing.format;
	const auto motion_format = inputs.motion_vectors->backing.format;
	if ((depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD16Unorm &&
	     depth_format != vk::Format::eR32Sfloat && depth_format != vk::Format::eR16Unorm) ||
	    (motion_format != vk::Format::eR16G16Sfloat && motion_format != vk::Format::eR32G32Sfloat))
		return false;
	if (!snapshot) snapshot = std::make_unique<DlssFgInputs>();
	const auto copy = [&](Image& source, std::unique_ptr<Image>& destination) {
		if (!destination || destination->backing.extent != source.backing.extent ||
		    destination->backing.format != source.backing.format) {
			if (destination)
				scheduler.DeferOperation([old = std::move(destination)]() mutable { old.reset(); });
			auto info     = source.info;
			info.data     = {};
			info.stencil  = {};
			info.metadata = {};
			destination   = std::make_unique<Image>(graphics, scheduler, info);
		}
		destination->CopyImage(source);
		destination->Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
		                     vk::AccessFlagBits2::eShaderRead, {}, command.Handle());
	};
	copy(*inputs.depth, snapshot->depth);
	copy(*inputs.motion_vectors, snapshot->motion);
	snapshot->jitter_x       = inputs.jitter_x;
	snapshot->jitter_y       = inputs.jitter_y;
	snapshot->motion_scale_x = inputs.motion_scale_x;
	snapshot->motion_scale_y = inputs.motion_scale_y;
	snapshot->frame_time_ms  = inputs.frame_time_ms;
	snapshot->reset          = inputs.reset_history;
	snapshot->depth_inverted = inputs.depth_inverted;
	return true;
}

struct DlssFrameGeneration::Impl {
	bool              initialized = false, logged = false;
	std::atomic<bool> available {false}, enabled {false};
	bool        device_extensions_ready = false;
	uint32_t    presented               = 0;
	uint64_t    total_presented         = 0;
	bool        external                = false;
	uint32_t    external_submitted      = 0;
	SDL_Window* external_window         = nullptr;
	bool        xefg                    = false;
	bool        logged_frames           = false;
	std::unique_ptr<XessFgBridge> bridge;
#if defined(KYTY_HAS_DLSS)
	SDL_SharedObject*                             external_module   = nullptr;
	GraphicContext*                               external_graphics = nullptr;
	decltype(&NVSDK_NGX_VULKAN_CreateFeature1)    external_create   = nullptr;
	decltype(&NVSDK_NGX_VULKAN_EvaluateFeature_C) external_evaluate = nullptr;
	decltype(&NVSDK_NGX_VULKAN_ReleaseFeature)    external_release  = nullptr;
	struct ExternalContext {
		std::shared_ptr<NgxRuntime>                parameters_runtime;
		NVSDK_NGX_Handle*                          feature    = nullptr;
		NVSDK_NGX_Parameter*                       parameters = nullptr;
		decltype(&NVSDK_NGX_VULKAN_ReleaseFeature) release    = nullptr;
		std::unique_ptr<Image>                     color, interpolated;
		bool                                       reset = true;
		~ExternalContext() {
			if (feature) release(feature);
			if (parameters) parameters_runtime->destroy_parameters(parameters);
		}
	};
	std::shared_ptr<ExternalContext>                                   external_context;
	std::vector<std::pair<uint64_t, std::shared_ptr<ExternalContext>>> external_retired;
#endif
#if defined(KYTY_HAS_DLSS_FG)
	HMODULE                    module         = nullptr;
	HWND                       window         = nullptr;
	PFN_vkGetInstanceProcAddr  proxy          = nullptr;
	PFN_vkGetInstanceProcAddr  native         = nullptr;
	GraphicContext*            graphics       = nullptr;
	sl::FrameToken*            token          = nullptr;
	uint32_t                   frame_index    = 0;
	PFun_slInit*               init           = nullptr;
	PFun_slShutdown*           shutdown       = nullptr;
	PFun_slIsFeatureSupported* supported      = nullptr;
	PFun_slGetFeatureFunction* get_function   = nullptr;
	PFun_slGetNewFrameToken*   get_token      = nullptr;
	PFun_slSetConstants*       constants      = nullptr;
	PFun_slSetTagForFrame*     tag            = nullptr;
	PFun_slDLSSGSetOptions*    options        = nullptr;
	PFun_slDLSSGGetState*      state          = nullptr;
	PFun_slReflexSetOptions*   reflex_options = nullptr;
	PFun_slReflexSleep*        sleep          = nullptr;
	PFun_slPCLSetMarker*       marker         = nullptr;
	std::wstring               directory;

	template <class T>
	bool Load(T*& target, const char* name) {
		target = reinterpret_cast<T*>(GetProcAddress(module, name));
		return target != nullptr;
	}
	template <class T>
	bool Feature(T*& target, sl::Feature feature, const char* name) {
		void*      address = nullptr;
		const auto result  = get_function(feature, name, address);
		target             = reinterpret_cast<T*>(address);
		return result == sl::Result::eOk && target;
	}
	void Mark(sl::PCLMarker event) {
		if (token && marker) {
			const auto result = marker(event, *token);
			if (result != sl::Result::eOk)
				LOGF("FG marker %u failed: %u\n", uint32_t(event), uint32_t(result));
		}
	}
	sl::Resource Resource(Image& image, vk::CommandBuffer command) {
		ImageViewInfo view {};
		view.format = image.backing.format;
		view.aspect = image.info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
		                                   : vk::ImageAspectFlagBits::eColor;
		view.usage  = vk::ImageUsageFlagBits::eSampled;
		image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {},
		              command);
		VmaAllocationInfo allocation {};
		vmaGetAllocationInfo(graphics->allocator, image.backing.allocation, &allocation);
		sl::Resource resource(
		    sl::ResourceType::eTex2d,
		    reinterpret_cast<void*>(static_cast<VkImage>(image.backing.image)),
		    reinterpret_cast<void*>(allocation.deviceMemory),
		    reinterpret_cast<void*>(static_cast<VkImageView>(image.FindView(view))),
		    uint32_t(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		resource.width        = image.backing.extent.width;
		resource.height       = image.backing.extent.height;
		resource.nativeFormat = uint32_t(image.backing.format);
		resource.mipLevels    = 1;
		resource.arrayLayers  = 1;
		resource.flags        = uint32_t(image.backing.flags);
		resource.usage        = uint32_t(image.backing.usage);
		return resource;
	}
#endif
};

DlssFrameGeneration::DlssFrameGeneration(): m_impl(std::make_unique<Impl>()) {}
DlssFrameGeneration::~DlssFrameGeneration() {
	Shutdown();
}

PFN_vkGetInstanceProcAddr DlssFrameGeneration::Initialize(PFN_vkGetInstanceProcAddr native) {
	if (!Config::DlssFrameGenerationEnabled()) return native;
	if (XessFgBridge::Selected()) {
		// XeFG presents through D3D12; the Vulkan instance and device stay native.
		m_impl->xefg = true;
		return native;
	}
	if (Config::GetUpscaleBackend() == Config::UpscaleBackend::OptiScaler) {
		// The Vulkan FSR generator must not share Streamline's presentation hooks.
		m_impl->external = true;
		return native;
	}
	if (Config::VulkanValidationEnabled() || Config::SpirvDebugPrintfEnabled() ||
	    Config::GpuAssistedValidationEnabled()) {
		// NVIDIA's Vulkan FG runtime currently produces internal layout errors
		// (Streamline issue #84). Preserve validation for the emulator and use
		// ordinary presentation instead of aborting inside the SDK pacer.
		LOGF("DLSS Frame Generation unavailable with Vulkan validation; using ordinary "
		     "presentation\n");
		return native;
	}
#if defined(KYTY_HAS_DLSS_FG)
	auto& impl       = *m_impl;
	impl.native      = native;
	const char* base = SDL_GetBasePath();
	if (base == nullptr) {
		LOGF("DLSS Frame Generation unavailable: %s\n", SDL_GetError());
		return native;
	}
	impl.directory  = std::filesystem::u8path(base).wstring();
	const auto path = (std::filesystem::path(impl.directory) / "sl.interposer.dll").wstring();
	if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
		LOGF("DLSS Frame Generation unavailable: invalid or missing signed Streamline runtime\n");
		return native;
	}
	impl.module = LoadLibraryExW(
	    path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!impl.module || !impl.Load(impl.init, "slInit") ||
	    !impl.Load(impl.shutdown, "slShutdown") ||
	    !impl.Load(impl.supported, "slIsFeatureSupported") ||
	    !impl.Load(impl.get_function, "slGetFeatureFunction") ||
	    !impl.Load(impl.get_token, "slGetNewFrameToken") ||
	    !impl.Load(impl.constants, "slSetConstants") || !impl.Load(impl.tag, "slSetTagForFrame")) {
		LOGF("DLSS Frame Generation unavailable: Streamline exports missing\n");
		return native;
	}
	const std::array features {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
	const wchar_t*   plugins = impl.directory.c_str();
	sl::Preferences  pref {};
	pref.pathsToPlugins    = &plugins;
	pref.numPathsToPlugins = 1;
	pref.featuresToLoad    = features.data();
	pref.numFeaturesToLoad = uint32_t(features.size());
	// Manual Vulkan integration supplies resource states explicitly. Keep the
	// SDK's default disabled command-list tracking: its tracking hooks are not
	// available in this integration.
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
	             sl::PreferenceFlags::eUseManualHooking |
	             sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.renderAPI     = sl::RenderAPI::eVulkan;
	pref.engine        = sl::EngineType::eCustom;
	pref.engineVersion = KYTY_VERSION;
	pref.projectId     = "84943a21-981d-49ef-a275-18acdd14b479";
	if (std::getenv("KYTY_DLSS_FG_DEBUG")) pref.logLevel = sl::LogLevel::eVerbose;
	pref.logMessageCallback = [](sl::LogType type, const char* message) {
		LOGF("Streamline[%u]: %s\n", uint32_t(type), message);
	};
	const auto result = impl.init(pref, sl::kSDKVersion);
	if (result != sl::Result::eOk) {
		LOGF("DLSS Frame Generation Streamline initialization failed: %u\n", uint32_t(result));
		return native;
	}
	impl.initialized                = true;
	PFN_vkGetInstanceProcAddr proxy = nullptr;
	if (!impl.Load(proxy, "vkGetInstanceProcAddr")) {
		Shutdown();
		return native;
	}
	impl.proxy = proxy;
	return proxy;
#else
	LOGF("DLSS Frame Generation requested but this build has no Streamline SDK\n");
	return native;
#endif
}

bool DlssFrameGeneration::CreateSurface(SDL_Window* window, vk::Instance instance,
                                        vk::SurfaceKHR& surface) {
	m_impl->external_window = window;
	VkSurfaceKHR native     = VK_NULL_HANDLE;
#if defined(KYTY_HAS_DLSS_FG)
	if (m_impl->initialized) {
		const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
		    m_impl->proxy(instance, "vkCreateWin32SurfaceKHR"));
		VkWin32SurfaceCreateInfoKHR info {};
		info.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		info.hinstance = GetModuleHandleW(nullptr);
		info.hwnd      = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
		m_impl->window = info.hwnd;
		if (!create || !info.hwnd || create(instance, &info, nullptr, &native) != VK_SUCCESS)
			return false;
		surface = native;
		return true;
	}
#endif
	if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &native)) return false;
	surface = native;
	return true;
}

vk::Result DlssFrameGeneration::CreateInstance(const vk::InstanceCreateInfo& info,
                                               vk::Instance&                 instance) {
#if defined(KYTY_HAS_DLSS_FG)
	auto& impl = *m_impl;
	if (impl.initialized) {
		// Streamline checks only global extensions. Ordinary validation comes
		// from its layer and does not need VK_EXT_validation_features.
		std::vector<const char*> extensions;
		for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
			if (std::strcmp(info.ppEnabledExtensionNames[i],
			                VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) != 0) {
				extensions.push_back(info.ppEnabledExtensionNames[i]);
			}
		}
		auto create_info                    = info;
		create_info.ppEnabledExtensionNames = extensions.data();
		create_info.enabledExtensionCount   = uint32_t(extensions.size());
		const auto create =
		    reinterpret_cast<PFN_vkCreateInstance>(impl.proxy(nullptr, "vkCreateInstance"));
		VkInstance native_instance = nullptr;
		const auto result = create(reinterpret_cast<const VkInstanceCreateInfo*>(&create_info),
		                           nullptr, &native_instance);
		instance          = native_instance;
		return vk::Result(result);
	}
#endif
	return vk::createInstance(&info, nullptr, &instance);
}

bool DlssFrameGeneration::ConfigureDeviceExtensions(
    std::span<const vk::ExtensionProperties> available, std::vector<const char*>& enabled,
    bool private_data) {
	auto& impl = *m_impl;
	if (!impl.initialized) return false;
	if (!private_data) {
		LOGF("DLSS Frame Generation unavailable: privateData is unsupported; using native Vulkan\n");
		Shutdown();
		return false;
	}
	const auto present_id = [](const auto& extension) {
		return std::strcmp(extension.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
	};
	if (!std::any_of(available.begin(), available.end(), present_id)) {
		LOGF("DLSS Frame Generation unavailable: VK_KHR_present_id is unsupported; using native "
		     "Vulkan\n");
		Shutdown();
		return false;
	}
	if (std::none_of(enabled.begin(), enabled.end(), [](const char* extension) {
		    return std::strcmp(extension, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
	    })) {
		// Streamline Reflex's VK_NV_low_latency2 depends on present_id.
		enabled.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
	}
	impl.device_extensions_ready = true;
	return true;
}

void DlssFrameGeneration::OnDevice(GraphicContext& graphics) {
	if (m_impl->xefg) {
		m_impl->bridge    = std::make_unique<XessFgBridge>(graphics);
		m_impl->available = m_impl->bridge->Available();
		if (!m_impl->available) m_impl->bridge.reset();
		return;
	}
#if defined(KYTY_HAS_DLSS)
	if (m_impl->external) {
		auto& impl             = *m_impl;
		impl.external_graphics = &graphics;
		const auto path        = OptiScalerRuntimePath();
		if (!path) return;
		const auto library   = (path->parent_path() / "dlssg_to_fsr3_amd_is_better.dll").u8string();
		impl.external_module = SDL_LoadObject(reinterpret_cast<const char*>(library.c_str()));
		if (!impl.external_module) {
			LOGF("OptiScaler Frame Generation unavailable: %s\n", SDL_GetError());
			return;
		}
		const auto load = [&](const char* name) {
			return SDL_LoadFunction(impl.external_module, name);
		};
		impl.external_create = reinterpret_cast<decltype(impl.external_create)>(
		    load("NVSDK_NGX_VULKAN_CreateFeature1"));
		impl.external_evaluate = reinterpret_cast<decltype(impl.external_evaluate)>(
		    load("NVSDK_NGX_VULKAN_EvaluateFeature"));
		impl.external_release = reinterpret_cast<decltype(impl.external_release)>(
		    load("NVSDK_NGX_VULKAN_ReleaseFeature"));
		const auto init =
		    reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_Init)>(load("NVSDK_NGX_VULKAN_Init"));
		impl.available = init && impl.external_create && impl.external_evaluate &&
		                 impl.external_release &&
		                 NVSDK_NGX_SUCCEED(init(0, L".", graphics.instance,
		                                        graphics.physical_device, graphics.device, nullptr,
		                                        nullptr, nullptr, NVSDK_NGX_Version_API));
		LOGF("OptiScaler Frame Generation Vulkan (Nukem FSR): %s\n",
		     impl.available ? "available" : "unavailable");
		return;
	}
#endif
#if defined(KYTY_HAS_DLSS_FG)
	auto& impl = *m_impl;
	if (!impl.initialized || !impl.device_extensions_ready) return;
	impl.graphics = &graphics;
	sl::AdapterInfo adapter {};
	adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
	const auto support       = impl.supported(sl::kFeatureDLSS_G, adapter);
	impl.available           = support == sl::Result::eOk &&
	                 impl.Feature(impl.options, sl::kFeatureDLSS_G, "slDLSSGSetOptions") &&
	                 impl.Feature(impl.state, sl::kFeatureDLSS_G, "slDLSSGGetState") &&
	                 impl.Feature(impl.reflex_options, sl::kFeatureReflex, "slReflexSetOptions") &&
	                 impl.Feature(impl.sleep, sl::kFeatureReflex, "slReflexSleep") &&
	                 impl.Feature(impl.marker, sl::kFeaturePCL, "slPCLSetMarker");
	if (impl.available) {
		sl::ReflexOptions reflex {};
		reflex.mode       = sl::ReflexMode::eLowLatency;
		const auto result = impl.reflex_options(reflex);
		if (result != sl::Result::eOk) {
			LOGF("DLSS Frame Generation Reflex initialization failed: %u\n", uint32_t(result));
			impl.available = false;
		}
	}
	LOGF("DLSS Frame Generation: %s (support result %u)\n",
	     impl.available ? "available" : "unavailable", uint32_t(support));
#else
	(void)graphics;
#endif
}
bool DlssFrameGeneration::Available() const {
	return m_impl->available && (!m_impl->bridge || m_impl->bridge->Available());
}
bool DlssFrameGeneration::Hooked() const {
	return m_impl->initialized;
}
bool DlssFrameGeneration::External() const {
	return m_impl->external;
}
bool DlssFrameGeneration::Enabled() const {
	return m_impl->enabled;
}
bool DlssFrameGeneration::Foreground() const {
	if (m_impl->xefg) return true; // XeFG's swap chain paces unfocused windows too.
	if (m_impl->external) {
#if defined(_WIN32)
		const auto window = m_impl->external_window;
		const auto hwnd   = window
		                        ? SDL_GetPointerProperty(SDL_GetWindowProperties(window),
		                                                 SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
		                        : nullptr;
		return hwnd && GetForegroundWindow() == hwnd;
#else
		return m_impl->external_window &&
		       (SDL_GetWindowFlags(m_impl->external_window) & SDL_WINDOW_INPUT_FOCUS);
#endif
	}
#if defined(KYTY_HAS_DLSS_FG)
	return m_impl->window && GetForegroundWindow() == m_impl->window;
#else
	return false;
#endif
}

bool DlssFrameGeneration::SetEnabled(bool enabled) {
	if (m_impl->xefg) {
		m_impl->enabled = enabled && m_impl->available && Config::DlssFrameGenerationEnabled();
		return false; // Generation is toggled per present; the swap chain stays.
	}
	if (m_impl->external) {
		enabled = enabled && m_impl->available && Config::DlssFrameGenerationEnabled();
		if (enabled && !m_impl->logged_frames && Config::GetFrameGenerationFrames() > 1) {
			LOGF(
			    "OptiScaler FSR Frame Generation generates 1 frame per real frame (%u requested)\n",
			    Config::GetFrameGenerationFrames());
			m_impl->logged_frames = true;
		}
		if (enabled != m_impl->enabled) {
#if defined(KYTY_HAS_DLSS)
			if (m_impl->external_context) m_impl->external_context->reset = true;
#endif
			m_impl->enabled = enabled;
		}
		return false; // The ordinary Vulkan swapchain remains valid.
	}
#if defined(KYTY_HAS_DLSS_FG)
	auto& impl = *m_impl;
	enabled    = enabled && impl.available && Config::DlssFrameGenerationEnabled();
	if (enabled == impl.enabled) return false;
	sl::DLSSGOptions options {};
	options.mode = enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
	// Multi Frame Generation where the GPU supports it (numFramesToGenerateMax).
	sl::DLSSGState limits {};
	const uint32_t maximum = impl.state(sl::ViewportHandle(0), limits, nullptr) == sl::Result::eOk
	                             ? std::max(limits.numFramesToGenerateMax, 1u)
	                             : 1u;
	options.numFramesToGenerate = std::min(Config::GetFrameGenerationFrames(), maximum);
	if (enabled)
		LOGF("DLSS Frame Generation: %u generated frames per real frame (%u requested, maximum "
		     "%u)\n",
		     options.numFramesToGenerate, Config::GetFrameGenerationFrames(), maximum);
	// Snapshots are owned by prepared frames and waited on before reuse.
	// Allow guest submissions to run while the SDK processes interpolation.
	options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockNoClientQueues;
	const auto result            = impl.options(sl::ViewportHandle(0), options);
	if (result != sl::Result::eOk) {
		LOGF("DLSS Frame Generation mode change failed: %u\n", uint32_t(result));
		impl.available = false;
		return false;
	}
	impl.enabled = enabled;
	return true;
#else
	(void)enabled;
	return false;
#endif
}

bool DlssFrameGeneration::BeginFrame() {
	if (m_impl->xefg) {
		if (!m_impl->enabled || !m_impl->bridge) return false;
		m_impl->bridge->MarkSimulation();
		return true;
	}
	if (m_impl->external) {
		m_impl->presented = 0;
		return m_impl->enabled;
	}
#if defined(KYTY_HAS_DLSS_FG)
	auto& impl = *m_impl;
	if (!impl.enabled) return false;
	impl.token        = nullptr;
	const auto result = impl.get_token(impl.token, nullptr);
	if (result != sl::Result::eOk || !impl.token) {
		LOGF("DLSS Frame Generation frame token failed: %u\n", uint32_t(result));
		impl.available = false;
		return false;
	}
	const auto sleep_result = impl.sleep(*impl.token);
	if (sleep_result != sl::Result::eOk) {
		LOGF("DLSS Frame Generation Reflex sleep failed: %u\n", uint32_t(sleep_result));
		impl.available = false;
		return false;
	}
	impl.Mark(sl::PCLMarker::eSimulationStart);
	impl.Mark(sl::PCLMarker::eSimulationEnd);
	return true;
#else
	return false;
#endif
}

Image* DlssFrameGeneration::Interpolate(CommandScheduler& scheduler, CommandBuffer& command,
                                        DlssFgInputs& inputs, const VulkanImage& source,
                                        vk::Extent2D output) {
#if defined(KYTY_HAS_DLSS)
	auto& impl = *m_impl;
	if (!impl.external || !impl.enabled || !inputs.depth || !inputs.motion) return nullptr;
	const auto fail = [&](const char* operation, NVSDK_NGX_Result result) -> Image* {
		LOGF("OptiScaler Frame Generation %s failed: 0x%x; using ordinary presentation\n",
		     operation, unsigned(result));
		impl.available = false;
		return nullptr;
	};
	command.EndRendering();
	const vk::Extent3D extent {output.width, output.height, 1};
	if (impl.external_context && (impl.external_context->color->backing.extent != extent ||
	                              impl.external_context->color->backing.format != source.format)) {
		impl.external_retired.emplace_back(scheduler.CurrentTick() - 1,
		                                   std::move(impl.external_context));
	}
	std::erase_if(impl.external_retired,
	              [&](const auto& retired) { return scheduler.IsFree(retired.first); });
	if (!impl.external_context) {
		auto context                = std::make_shared<Impl::ExternalContext>();
		context->release            = impl.external_release;
		context->parameters_runtime = GetNgxRuntime();
		const auto allocate =
		    context->parameters_runtime->valid
		        ? reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_AllocateParameters)>(SDL_LoadFunction(
		              context->parameters_runtime->module, "NVSDK_NGX_VULKAN_AllocateParameters"))
		        : nullptr;
		const auto allocated =
		    allocate ? allocate(&context->parameters) : NVSDK_NGX_Result_FAIL_PlatformError;
		if (NVSDK_NGX_FAILED(allocated)) return fail("parameters", allocated);
		ImageInfo info {};
		info.pixel_format     = source.format;
		info.extent           = extent;
		info.pitch            = output.width;
		info.bytes_per_block  = source.format == vk::Format::eR16G16B16A16Sfloat ? 8 : 4;
		info.mip_layout[0]    = {0, uint64_t(output.width) * output.height * info.bytes_per_block,
		                         output.width, output.height};
		context->color        = std::make_unique<Image>(*impl.external_graphics, scheduler, info);
		context->interpolated = std::make_unique<Image>(*impl.external_graphics, scheduler, info);
		context->parameters->Set("Width", output.width);
		context->parameters->Set("Height", output.height);
		context->parameters->Set("DLSSG.BackbufferFormat", unsigned(source.format));
		const auto created = impl.external_create(impl.external_graphics->device, command.Handle(),
		                                          NVSDK_NGX_Feature_FrameGeneration,
		                                          context->parameters, &context->feature);
		if (NVSDK_NGX_FAILED(created)) return fail("creation", created);
		impl.external_context = std::move(context);
	}
	auto& context = *impl.external_context;
	context.color->Transit(vk::ImageLayout::eTransferDstOptimal,
	                       vk::AccessFlagBits2::eTransferWrite, {}, command.Handle());
	vk::ImageBlit blit {};
	blit.srcSubresource = blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	blit.srcOffsets[1] = vk::Offset3D(int(source.extent.width), int(source.extent.height), 1);
	blit.dstOffsets[1] = vk::Offset3D(int(output.width), int(output.height), 1);
	command.Handle().blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal,
	                           context.color->backing.image, vk::ImageLayout::eTransferDstOptimal,
	                           1, &blit, vk::Filter::eLinear);
	// These are the states expected by Nukem's Vulkan FFX adapter; it restores
	// external resources to these states before returning from dispatch.
	context.color->Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
	                       vk::AccessFlagBits2::eShaderRead, {}, command.Handle());
	inputs.depth->Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
	                      {}, command.Handle());
	inputs.motion->Transit(vk::ImageLayout::eTransferDstOptimal,
	                       vk::AccessFlagBits2::eTransferWrite, {}, command.Handle());
	context.interpolated->Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderWrite, {},
	                              command.Handle());
	const auto resource = [](Image& image, bool writable) {
		ImageViewInfo view {};
		view.format = image.backing.format;
		view.aspect = image.info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
		                                   : vk::ImageAspectFlagBits::eColor;
		view.usage = writable ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
		return NVSDK_NGX_Create_ImageView_Resource_VK(
		    image.FindView(view), image.backing.image,
		    VkImageSubresourceRange {VkImageAspectFlags(view.aspect), 0, 1, 0, 1},
		    VkFormat(view.format), image.backing.extent.width, image.backing.extent.height,
		    writable);
	};
	auto color = resource(*context.color, false), depth = resource(*inputs.depth, false),
	     motion       = resource(*inputs.motion, false),
	     interpolated = resource(*context.interpolated, true);
	auto* parameters  = context.parameters;
	parameters->Set("DLSSG.Backbuffer", static_cast<void*>(&color));
	parameters->Set("DLSSG.Depth", static_cast<void*>(&depth));
	parameters->Set("DLSSG.MVecs", static_cast<void*>(&motion));
	parameters->Set("DLSSG.OutputInterpolated", static_cast<void*>(&interpolated));
	parameters->Set("DLSSG.EnableInterp", 1u);
	parameters->Set("DLSSG.IsRecording", 1u);
	parameters->Set("DLSSG.MultiFrameCount", 1u);
	parameters->Set("DLSSG.MultiFrameIndex", 1u);
	const bool reset = context.reset || inputs.reset;
	parameters->Set("DLSSG.Reset", unsigned(reset));
	parameters->Set("DLSSG.DepthInverted", unsigned(inputs.depth_inverted));
	parameters->Set("DLSSG.MvecScaleX", inputs.motion_scale_x);
	parameters->Set("DLSSG.MvecScaleY", inputs.motion_scale_y);
	parameters->Set("DLSSG.JitterOffsetX", inputs.jitter_x);
	parameters->Set("DLSSG.JitterOffsetY", inputs.jitter_y);
	parameters->Set("DLSSG.CameraNear", .1f);
	parameters->Set("DLSSG.CameraFar", 10000.f);
	parameters->Set("DLSSG.CameraFOV", 1.5707963f);
	const auto evaluated =
	    impl.external_evaluate(command.Handle(), context.feature, parameters, nullptr);
	++impl.external_submitted;
	if (NVSDK_NGX_FAILED(evaluated)) return fail("evaluation", evaluated);
	context.reset = false;
	return reset ? nullptr : context.interpolated.get();
#else
	(void)scheduler;
	(void)command;
	(void)inputs;
	(void)source;
	(void)output;
	return nullptr;
#endif
}

void DlssFrameGeneration::InterpolatedPresented() {
	if (!m_impl->external || !m_impl->enabled) return;
	++m_impl->presented;
	++m_impl->total_presented;
}

bool DlssFrameGeneration::TagFrame(CommandBuffer& command, DlssFgInputs& inputs,
                                   vk::Extent2D output) {
	if (m_impl->xefg) {
		if (!m_impl->enabled || !m_impl->bridge || !inputs.depth || !inputs.motion) return false;
		m_impl->bridge->MarkRenderSubmit(true);
		const bool recorded = m_impl->bridge->RecordInputs(
		    command.Handle(), *inputs.depth, *inputs.motion, inputs.jitter_x, inputs.jitter_y,
		    inputs.motion_scale_x, inputs.motion_scale_y, inputs.reset, inputs.frame_time_ms);
		m_impl->bridge->MarkRenderSubmit(false);
		if (recorded) ++m_impl->external_submitted;
		return recorded;
	}
#if defined(KYTY_HAS_DLSS_FG)
	auto& impl = *m_impl;
	if (!impl.enabled || !impl.token || !inputs.depth || !inputs.motion) return false;
	const auto accept = [&](sl::Result result, const char* operation) {
		if (result == sl::Result::eOk) return true;
		LOGF("DLSS Frame Generation %s failed: %u\n", operation, uint32_t(result));
		impl.available = false;
		return false;
	};
	++impl.frame_index;
	impl.Mark(sl::PCLMarker::eRenderSubmitStart);
	sl::Constants      constants {};
	const sl::float4x4 identity {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
	constants.cameraViewToClip = constants.clipToCameraView = identity;
	constants.clipToLensClip                                = identity;
	constants.clipToPrevClip = constants.prevClipToClip = identity;
	constants.jitterOffset                              = {inputs.jitter_x, inputs.jitter_y};
	constants.cameraPinholeOffset                       = {0, 0};
	constants.mvecScale              = {inputs.motion_scale_x / inputs.motion->backing.extent.width,
	                                    inputs.motion_scale_y / inputs.motion->backing.extent.height};
	constants.cameraPos              = {0, 0, 0};
	constants.cameraUp               = {0, 1, 0};
	constants.cameraRight            = {1, 0, 0};
	constants.cameraFwd              = {0, 0, 1};
	constants.cameraNear             = .1f;
	constants.cameraFar              = 1000.f;
	constants.cameraFOV              = 1.5707963f;
	constants.cameraAspectRatio      = float(output.width) / output.height;
	constants.depthInverted          = inputs.depth_inverted ? sl::eTrue : sl::eFalse;
	constants.cameraMotionIncluded   = sl::eTrue;
	constants.motionVectors3D        = sl::eFalse;
	constants.reset                  = inputs.reset ? sl::eTrue : sl::eFalse;
	constants.orthographicProjection = sl::eTrue;
	if (!accept(impl.constants(constants, *impl.token, sl::ViewportHandle(0)), "constants"))
		return false;
	const auto       vk_command = command.Handle();
	auto             depth      = impl.Resource(*inputs.depth, vk_command);
	auto             motion     = impl.Resource(*inputs.motion, vk_command);
	const sl::Extent backbuffer_extent {0, 0, output.width, output.height};
	// Streamline already owns the intercepted swapchain backbuffer and its state.
	// Supplying only its extent avoids replacing SDK metadata with a partial resource.
	const std::array tags {
	    sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent),
	    sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent),
	    sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::eValidUntilPresent,
	                    &backbuffer_extent)};
	const auto result =
	    impl.tag(*impl.token, sl::ViewportHandle(0), tags.data(), uint32_t(tags.size()),
	             reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(vk_command)));
	impl.Mark(sl::PCLMarker::eRenderSubmitEnd);
	return accept(result, "tagging");
#else
	(void)command;
	(void)inputs;
	(void)output;
	return false;
#endif
}
void DlssFrameGeneration::PresentStart() {
	if (m_impl->external) return;
#if defined(KYTY_HAS_DLSS_FG)
	if (m_impl->enabled) m_impl->Mark(sl::PCLMarker::ePresentStart);
#endif
}
void DlssFrameGeneration::PresentEnd(DlssFgInputs* inputs, bool new_frame) {
	if (m_impl->xefg) {
		// Inputs were copied into D3D12-shared images by the present commands.
		if (new_frame && m_impl->enabled && m_impl->bridge) {
			m_impl->presented = m_impl->bridge->PresentedFrames();
			m_impl->total_presented += m_impl->presented;
			if (!m_impl->logged && m_impl->external_submitted > 20 &&
			    m_impl->total_presented > m_impl->external_submitted) {
				Log::WriteToConsoleAndLog(
				    fmt::format("XeSS Frame Generation active: {} display frames "
				                "for {} submitted guest frames\n",
				                m_impl->total_presented, m_impl->external_submitted));
				m_impl->logged = true;
			}
		}
		return;
	}
	if (m_impl->external) {
		if (new_frame && m_impl->enabled) {
			++m_impl->presented;
			++m_impl->total_presented;
		}
		return;
	}
#if defined(KYTY_HAS_DLSS_FG)
	auto& impl = *m_impl;
	if (!impl.enabled) return;
	if (new_frame) impl.Mark(sl::PCLMarker::ePresentEnd);
	if (inputs) inputs->pending = true;
	sl::DLSSGState state {};
	const auto     result = impl.state(sl::ViewportHandle(0), state, nullptr);
	if (result == sl::Result::eOk) {
		if (inputs) {
			inputs->completion =
			    reinterpret_cast<VkSemaphore>(state.inputsProcessingCompletionFence);
			inputs->completion_value = state.lastPresentInputsProcessingCompletionFenceValue;
		}
		// Cached overlay presents retain the latest input fence, but do not
		// advance guest history or count the same SDK frame a second time.
		if (new_frame) {
			impl.presented = state.numFramesActuallyPresented;
			impl.total_presented += impl.presented;
		}
		if (!impl.logged && impl.frame_index > 20 && impl.total_presented > impl.frame_index) {
			Log::WriteToConsoleAndLog(fmt::format("DLSS Frame Generation active: {} display frames "
			                                      "reported for {} submitted guest frames\n",
			                                      impl.total_presented, impl.frame_index));
			impl.logged = true;
		}
		if (state.status != sl::DLSSGStatus::eOk) {
			LOGF("DLSS Frame Generation runtime status: 0x%x\n", uint32_t(state.status));
			// The next presentation drains inputs, turns the feature off and
			// recreates an ordinary swapchain. Do not keep feeding a failed runtime.
			impl.available = false;
		}
	} else {
		LOGF("DLSS Frame Generation state query failed: %u\n", uint32_t(result));
		impl.available = false;
	}
#endif
	(void)inputs;
	(void)new_frame;
}
uint32_t DlssFrameGeneration::PresentedFrames() const {
	return m_impl->presented;
}
uint64_t DlssFrameGeneration::TotalPresentedFrames() const {
	return m_impl->total_presented;
}
uint32_t DlssFrameGeneration::TotalSubmittedFrames() const {
	if (m_impl->external || m_impl->xefg) return m_impl->external_submitted;
#if defined(KYTY_HAS_DLSS_FG)
	return m_impl->frame_index;
#else
	return 0;
#endif
}
XessFgBridge* DlssFrameGeneration::Bridge() const {
	return m_impl->xefg && m_impl->available ? m_impl->bridge.get() : nullptr;
}
void* DlssFrameGeneration::NativeWindow() const {
#if defined(_WIN32)
	return m_impl->external_window
	           ? SDL_GetPointerProperty(SDL_GetWindowProperties(m_impl->external_window),
	                                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
	           : nullptr;
#else
	return nullptr;
#endif
}
void DlssFrameGeneration::Shutdown() {
	// The swap chain released its shared images; D3D12 objects go before the Vulkan device.
	m_impl->bridge.reset();
	if (m_impl->xefg) {
		m_impl->available = false;
		m_impl->enabled   = false;
	}
#if defined(KYTY_HAS_DLSS)
	m_impl->external_context.reset();
	m_impl->external_retired.clear();
	// Keep DLL code loaded through deferred releases and Vulkan teardown.
#endif
	if (m_impl->external) {
		m_impl->available = false;
		m_impl->enabled   = false;
	}
#if defined(KYTY_HAS_DLSS_FG)
	auto& impl = *m_impl;
	if (impl.initialized) impl.shutdown();
	impl.initialized = false;
	impl.available   = false;
	impl.enabled     = false;
	impl.device_extensions_ready                     = false;
	// The global Vulkan dispatcher retains the proxy function addresses until
	// instance/device destruction completes. Keep the module loaded for that.
#endif
}
} // namespace Libs::Graphics
