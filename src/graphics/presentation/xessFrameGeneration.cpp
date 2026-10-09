#include "graphics/presentation/xessFrameGeneration.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/presentation/directUpscaler.h"
#include "graphics/presentation/upscaleRuntime.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
// windows.h first.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan_win32.h>
#include <wrl/client.h>
#include <xell/xell_d3d12.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#endif

namespace Libs::Graphics {

bool XessFgBridge::Selected() {
#if defined(_WIN32)
	return Config::DlssFrameGenerationEnabled() &&
	       Config::GetUpscaleBackend() == Config::UpscaleBackend::OptiScaler &&
	       Config::GetOptiScalerFrameGeneration() == Config::OptiScalerFrameGeneration::XeSS;
#else
	return false;
#endif
}

void XessFgBridge::AppendDeviceExtensions(std::vector<const char*>&                   enabled,
                                          const std::vector<vk::ExtensionProperties>& available) {
#if defined(_WIN32)
	if (!Selected()) return;
	for (const char* name: {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
	                        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME}) {
		const bool present =
		    std::any_of(available.begin(), available.end(), [&](const auto& extension) {
			    return std::strcmp(extension.extensionName, name) == 0;
		    });
		const bool listed = std::any_of(enabled.begin(), enabled.end(), [&](const char* item) {
			return std::strcmp(item, name) == 0;
		});
		if (present && !listed) enabled.push_back(name);
	}
#else
	(void)enabled;
	(void)available;
#endif
}

#if defined(_WIN32)
namespace {
using Microsoft::WRL::ComPtr;

struct FgApi {
	HMODULE                                            xefg = nullptr, xell = nullptr;
	bool                                               valid        = false;
	decltype(&xefgSwapChainD3D12CreateContext)         create       = nullptr;
	decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) init         = nullptr;
	decltype(&xefgSwapChainD3D12GetProperties)         properties   = nullptr;
	decltype(&xefgSwapChainSetNumInterpolatedFrames)   interpolated = nullptr;
	decltype(&xefgSwapChainD3D12GetSwapChainPtr)       swapchain    = nullptr;
	decltype(&xefgSwapChainD3D12TagFrameResource)      tag          = nullptr;
	decltype(&xefgSwapChainTagFrameConstants)          constants    = nullptr;
	decltype(&xefgSwapChainSetPresentId)               present_id   = nullptr;
	decltype(&xefgSwapChainSetEnabled)                 enable       = nullptr;
	decltype(&xefgSwapChainGetLastPresentStatus)       status       = nullptr;
	decltype(&xefgSwapChainSetLatencyReduction)        latency      = nullptr;
	decltype(&xefgSwapChainSetLoggingCallback)         logging      = nullptr;
	decltype(&xefgSwapChainDestroy)                    destroy      = nullptr;
	decltype(&xefgSwapChainGetVersion)                 version      = nullptr;
	decltype(&xellD3D12CreateContext)                  xell_create  = nullptr;
	decltype(&xellDestroyContext)                      xell_destroy = nullptr;
	decltype(&xellAddMarkerData)                       marker       = nullptr;
	decltype(&xellSetSleepMode)                        sleep_mode   = nullptr;
	decltype(&xellSleep)                               sleep        = nullptr;

	template <class T>
	void Load(HMODULE module, T& target, const char* name) {
		target = reinterpret_cast<T>(GetProcAddress(module, name));
		if (!target) LOGF("XeSS-FG unavailable: missing export %s\n", name);
		valid = valid && target;
	}
	FgApi() {
		const auto runtime = OptiScalerRuntimePath();
		if (!runtime) return;
		const auto directory = runtime->parent_path();
		// libxess_fg.dll resolves libxell.dll from its own directory.
		xell = LoadLibraryExW((directory / L"libxell.dll").c_str(), nullptr,
		                      LOAD_WITH_ALTERED_SEARCH_PATH);
		xefg = LoadLibraryExW((directory / L"libxess_fg.dll").c_str(), nullptr,
		                      LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!xefg || !xell) {
			LOGF("XeSS-FG unavailable: libxess_fg.dll and libxell.dll not found in %s\n",
			     directory.string().c_str());
			return;
		}
		valid = true;
		Load(xefg, create, "xefgSwapChainD3D12CreateContext");
		Load(xefg, init, "xefgSwapChainD3D12InitFromSwapChainDesc");
		Load(xefg, properties, "xefgSwapChainD3D12GetProperties");
		Load(xefg, interpolated, "xefgSwapChainSetNumInterpolatedFrames");
		Load(xefg, swapchain, "xefgSwapChainD3D12GetSwapChainPtr");
		Load(xefg, tag, "xefgSwapChainD3D12TagFrameResource");
		Load(xefg, constants, "xefgSwapChainTagFrameConstants");
		Load(xefg, present_id, "xefgSwapChainSetPresentId");
		Load(xefg, enable, "xefgSwapChainSetEnabled");
		Load(xefg, status, "xefgSwapChainGetLastPresentStatus");
		Load(xefg, latency, "xefgSwapChainSetLatencyReduction");
		Load(xefg, logging, "xefgSwapChainSetLoggingCallback");
		Load(xefg, destroy, "xefgSwapChainDestroy");
		Load(xefg, version, "xefgSwapChainGetVersion");
		Load(xell, xell_create, "xellD3D12CreateContext");
		Load(xell, xell_destroy, "xellDestroyContext");
		Load(xell, marker, "xellAddMarkerData");
		Load(xell, sleep_mode, "xellSetSleepMode");
		Load(xell, sleep, "xellSleep");
	}
};
const FgApi& Api() {
	static FgApi api;
	return api;
}

DXGI_FORMAT Dxgi(vk::Format format) {
	switch (format) {
		case vk::Format::eR8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case vk::Format::eB8G8R8A8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case vk::Format::eR32Sfloat: return DXGI_FORMAT_R32_FLOAT;
		case vk::Format::eR16Unorm: return DXGI_FORMAT_R16_UNORM;
		case vk::Format::eR16G16Sfloat: return DXGI_FORMAT_R16G16_FLOAT;
		case vk::Format::eR32G32Sfloat: return DXGI_FORMAT_R32G32_FLOAT;
		default: return DXGI_FORMAT_UNKNOWN;
	}
}

void FgLog(const char* message, xefg_swapchain_logging_level_t level, void*) {
	if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING) LOGF("XeSS-FG: %s\n", message);
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER barrier {};
	barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource   = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter  = after;
	return barrier;
}
} // namespace

struct XessFgBridge::Impl {
	struct Shared {
		ComPtr<ID3D12Resource> resource;
		vk::Image              image  = nullptr;
		vk::DeviceMemory       memory = nullptr;
		vk::Format             format = vk::Format::eUndefined;
		vk::Extent2D           extent {};
	};
	struct Slot {
		Shared                               color, depth, motion;
		ComPtr<ID3D12CommandAllocator>       allocator;
		ComPtr<ID3D12GraphicsCommandList>    list;
		uint64_t                             rendered = 0, released = 0;
		bool                                 has_inputs = false;
		xefg_swapchain_frame_constant_data_t constants {};
	};

	GraphicContext&                         graphics;
	std::atomic<bool>                       available         {false};
	PFN_vkGetMemoryWin32HandlePropertiesKHR memory_properties = nullptr;
	PFN_vkImportSemaphoreWin32HandleKHR     import_semaphore  = nullptr;
	ComPtr<IDXGIFactory4>                   factory;
	ComPtr<ID3D12Device>                    device;
	ComPtr<ID3D12CommandQueue>              queue;
	// Vulkan signals `rendered` per composed image; D3D12 signals `released` after reading it.
	ComPtr<ID3D12Fence>     rendered, released;
	vk::Semaphore           vk_rendered = nullptr, vk_released = nullptr;
	uint64_t                render_value = 0, release_value = 0;
	xell_context_handle_t   xell    = nullptr;
	xefg_swapchain_handle_t context = nullptr;
	ComPtr<IDXGISwapChain3> swapchain;
	bool                    tearing = false, vsync = false, generating = false;
	std::vector<Slot>       slots;
	std::vector<vk::Image>  images;
	uint32_t                current = 0, next = 0, present_id = 0, presented = 0;
	bool                    status_logged = false;

	explicit Impl(GraphicContext& owner): graphics(owner) {
		const auto& api = Api();
		if (!api.valid) return;
		const auto proc = [&](const char* name) {
			return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(graphics.device, name);
		};
		memory_properties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
		    proc("vkGetMemoryWin32HandlePropertiesKHR"));
		import_semaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
		    proc("vkImportSemaphoreWin32HandleKHR"));
		if (!memory_properties || !import_semaphore) {
			LOGF("XeSS-FG unavailable: Vulkan Win32 external memory or semaphores are not "
			     "enabled\n");
			return;
		}
		vk::PhysicalDeviceIDProperties id {};
		vk::PhysicalDeviceProperties2  properties {};
		properties.pNext = &id;
		graphics.physical_device.getProperties2(&properties);
		LUID luid {};
		std::memcpy(&luid, id.deviceLUID.data(), sizeof(luid));
		ComPtr<IDXGIAdapter1> adapter;
		if (!id.deviceLUIDValid || FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
		    FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
		    FAILED(
		        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
			LOGF("XeSS-FG unavailable: no D3D12 device for the Vulkan GPU\n");
			return;
		}
		D3D12_COMMAND_QUEUE_DESC queue_desc {};
		queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
		    !SharedFence(rendered, vk_rendered) || !SharedFence(released, vk_released)) {
			LOGF("XeSS-FG unavailable: cannot share fences between Vulkan and D3D12\n");
			return;
		}
		// XeSS-FG generates frames only with latency reduction on.
		xell_sleep_params_t sleep {};
		sleep.bLowLatencyMode = 1;
		if (api.xell_create(device.Get(), &xell) != XELL_RESULT_SUCCESS ||
		    api.sleep_mode(xell, &sleep) != XELL_RESULT_SUCCESS) {
			LOGF("XeSS-FG: Xe Low Latency unavailable on this GPU\n");
			if (xell) api.xell_destroy(xell);
			xell = nullptr;
		}
		ComPtr<IDXGIFactory5> factory5;
		BOOL                  allow = FALSE;
		if (SUCCEEDED(factory.As(&factory5)))
			factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
			                              sizeof(allow));
		tearing = allow;
		xefg_swapchain_version_t version {};
		api.version(&version);
		available = true;
		LOGF("XeSS Frame Generation %u.%u.%u (D3D12 presentation bridge): available\n",
		     version.major, version.minor, version.patch);
	}
	~Impl() {
		DestroySwapchain();
		if (xell) Api().xell_destroy(xell);
		for (auto semaphore: {vk_rendered, vk_released}) {
			if (semaphore) graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}

	bool SharedFence(ComPtr<ID3D12Fence>& fence, vk::Semaphore& semaphore) {
		HANDLE handle = nullptr;
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
		    FAILED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
			return false;
		vk::SemaphoreTypeCreateInfo type {};
		type.semaphoreType = vk::SemaphoreType::eTimeline;
		vk::SemaphoreCreateInfo info {};
		info.pNext = &type;
		bool ok =
		    graphics.device.createSemaphore(&info, nullptr, &semaphore) == vk::Result::eSuccess;
		if (ok) {
			VkImportSemaphoreWin32HandleInfoKHR import {
			    VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
			import.semaphore  = semaphore;
			import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
			import.handle     = handle;
			ok                = import_semaphore(graphics.device, &import) == VK_SUCCESS;
		}
		CloseHandle(handle);
		return ok;
	}

	bool CreateShared(Shared& shared, vk::Format format, vk::Extent2D extent,
	                  vk::ImageUsageFlags usage, bool render_target) {
		D3D12_RESOURCE_DESC desc {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width            = extent.width;
		desc.Height           = extent.height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels        = 1;
		desc.Format           = Dxgi(format);
		desc.SampleDesc.Count = 1;
		desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags =
		    render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
		D3D12_HEAP_PROPERTIES heap {};
		heap.Type     = D3D12_HEAP_TYPE_DEFAULT;
		HANDLE handle = nullptr;
		if (desc.Format == DXGI_FORMAT_UNKNOWN ||
		    FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
		                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
		                                           IID_PPV_ARGS(&shared.resource))) ||
		    FAILED(device->CreateSharedHandle(shared.resource.Get(), nullptr, GENERIC_ALL, nullptr,
		                                      &handle)))
			return false;
		vk::ExternalMemoryImageCreateInfo external {};
		external.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource;
		vk::ImageCreateInfo info {};
		info.pNext         = &external;
		info.imageType     = vk::ImageType::e2D;
		info.format        = format;
		info.extent        = vk::Extent3D {extent.width, extent.height, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = 1;
		info.samples       = vk::SampleCountFlagBits::e1;
		info.tiling        = vk::ImageTiling::eOptimal;
		info.usage         = usage;
		info.sharingMode   = vk::SharingMode::eExclusive;
		info.initialLayout = vk::ImageLayout::eUndefined;
		bool ok =
		    graphics.device.createImage(&info, nullptr, &shared.image) == vk::Result::eSuccess;
		if (ok) {
			vk::MemoryRequirements requirements {};
			graphics.device.getImageMemoryRequirements(shared.image, &requirements);
			VkMemoryWin32HandlePropertiesKHR properties {
			    VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
			memory_properties(graphics.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
			                  handle, &properties);
			uint32_t bits = requirements.memoryTypeBits &
			                (properties.memoryTypeBits ? properties.memoryTypeBits : ~0u);
			uint32_t    type   = UINT32_MAX;
			const auto& memory = graphics.physical_device_memory_properties;
			for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
				if (!(bits & (1u << i))) continue;
				if (type == UINT32_MAX || (memory.memoryTypes[i].propertyFlags &
				                           vk::MemoryPropertyFlagBits::eDeviceLocal))
					type = i;
				if (memory.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)
					break;
			}
			VkMemoryDedicatedAllocateInfo dedicated {
			    VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
			dedicated.image = shared.image;
			VkImportMemoryWin32HandleInfoKHR import {
			    VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
			import.pNext      = &dedicated;
			import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
			import.handle     = handle;
			VkMemoryAllocateInfo allocate {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			allocate.pNext           = &import;
			allocate.allocationSize  = requirements.size;
			allocate.memoryTypeIndex = type;
			ok                       = type != UINT32_MAX &&
			     graphics.device.allocateMemory(
			         reinterpret_cast<const vk::MemoryAllocateInfo*>(&allocate), nullptr,
			         &shared.memory) == vk::Result::eSuccess &&
			     graphics.device.bindImageMemory(shared.image, shared.memory, 0) ==
			         vk::Result::eSuccess;
		}
		CloseHandle(handle);
		shared.format = format;
		shared.extent = extent;
		if (!ok) Release(shared);
		return ok;
	}
	void Release(Shared& shared) {
		if (shared.image) graphics.device.destroyImage(shared.image, nullptr);
		if (shared.memory) graphics.device.freeMemory(shared.memory, nullptr);
		shared = {};
	}

	// D3D12 has finished every submitted copy and present.
	void WaitD3d12() {
		if (!queue || !released) return;
		queue->Signal(released.Get(), ++release_value);
		if (released->GetCompletedValue() < release_value)
			released->SetEventOnCompletion(release_value, nullptr);
	}

	void DestroySwapchain() {
		WaitD3d12();
		for (auto& slot: slots) {
			Release(slot.color);
			Release(slot.depth);
			Release(slot.motion);
		}
		slots.clear();
		images.clear();
		swapchain.Reset();
		if (context) Api().destroy(context);
		context    = nullptr;
		generating = false;
	}
};

XessFgBridge::XessFgBridge(GraphicContext& graphics): m_impl(std::make_unique<Impl>(graphics)) {}
XessFgBridge::~XessFgBridge() = default;
bool XessFgBridge::Available() const {
	return m_impl->available;
}
vk::Format XessFgBridge::Format() const {
	return vk::Format::eR8G8B8A8Unorm;
}
const std::vector<vk::Image>& XessFgBridge::Images() const {
	return m_impl->images;
}

bool XessFgBridge::CreateSwapchain(void* hwnd, vk::Extent2D extent, bool vsync) {
	auto&       state = *m_impl;
	const auto& api   = Api();
	if (!state.available) return false;
	state.DestroySwapchain();
	if (api.create(state.device.Get(), &state.context) != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
		LOGF("XeSS-FG context creation failed; presenting without Frame Generation\n");
		state.available = false;
		return false;
	}
	api.logging(state.context, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, FgLog, nullptr);
	if (state.xell) api.latency(state.context, state.xell);
	DXGI_SWAP_CHAIN_DESC1 desc {};
	desc.Width            = extent.width;
	desc.Height           = extent.height;
	desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount      = 3;
	desc.Scaling          = DXGI_SCALING_STRETCH;
	desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags            = state.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	xefg_swapchain_d3d12_init_params_t params {};
	params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
	xefg_swapchain_properties_t properties {};
	const uint32_t              maximum =
        api.properties(state.context, &params, extent.width, extent.height,
	                                DXGI_FORMAT_R8G8B8A8_UNORM, &properties) == XEFG_SWAPCHAIN_RESULT_SUCCESS
	                     ? std::max(properties.maxSupportedInterpolations, 1u)
	                     : 1u;
	const uint32_t frames        = std::min(Config::GetFrameGenerationFrames(), maximum);
	params.maxInterpolatedFrames = frames;
	params.creationNodeMask      = 1;
	params.visibleNodeMask       = 1;
	params.uiMode                = XEFG_SWAPCHAIN_UI_MODE_AUTO;
	ComPtr<IDXGIFactory2> factory2;
	state.factory.As(&factory2);
	const auto result = api.init(state.context, static_cast<HWND>(hwnd), &desc, nullptr,
	                             state.queue.Get(), factory2.Get(), &params);
	if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS ||
	    api.swapchain(state.context, IID_PPV_ARGS(&state.swapchain)) !=
	        XEFG_SWAPCHAIN_RESULT_SUCCESS) {
		LOGF("XeSS-FG swap chain creation failed: %d; presenting without Frame Generation\n",
		     int(result));
		state.DestroySwapchain();
		state.available = false;
		return false;
	}
	state.vsync = vsync;
	state.slots.resize(3);
	for (auto& slot: state.slots) {
		const auto usage = vk::ImageUsageFlagBits::eColorAttachment |
		                   vk::ImageUsageFlagBits::eTransferDst |
		                   vk::ImageUsageFlagBits::eTransferSrc;
		if (!state.CreateShared(slot.color, Format(), extent, usage, true) ||
		    FAILED(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		                                                IID_PPV_ARGS(&slot.allocator))) ||
		    FAILED(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		                                           slot.allocator.Get(), nullptr,
		                                           IID_PPV_ARGS(&slot.list)))) {
			LOGF("XeSS-FG shared presentation images unavailable; presenting without Frame "
			     "Generation\n");
			state.DestroySwapchain();
			state.available = false;
			return false;
		}
		slot.list->Close();
		state.images.push_back(slot.color.image);
	}
	api.interpolated(state.context, frames);
	api.enable(state.context, 0);
	LOGF("XeSS-FG presentation: %ux%u through D3D12%s; %u generated frames per real frame (%u "
	     "requested, "
	     "maximum %u)\n",
	     extent.width, extent.height, state.tearing && !vsync ? ", tearing allowed" : "", frames,
	     Config::GetFrameGenerationFrames(), maximum);
	return true;
}

void XessFgBridge::DestroySwapchain() {
	m_impl->DestroySwapchain();
}

uint32_t XessFgBridge::Acquire() {
	auto& state                           = *m_impl;
	state.current                         = state.next;
	state.next                            = (state.next + 1) % uint32_t(state.slots.size());
	state.slots[state.current].has_inputs = false;
	return state.current;
}

void XessFgBridge::AddSubmitSync(SubmitInfo& submit, uint32_t image) {
	auto& state = *m_impl;
	auto& slot  = state.slots[image];
	// The previous D3D12 copy out of this image must finish before it is overwritten.
	if (slot.released) submit.AddWait(state.vk_released, slot.released);
	slot.rendered = ++state.render_value;
	submit.AddSignal(state.vk_rendered, slot.rendered);
}

bool XessFgBridge::RecordInputs(vk::CommandBuffer command, Image& depth, Image& motion,
                                float jitter_x, float jitter_y, float motion_scale_x,
                                float motion_scale_y, bool reset, float frame_time_ms) {
	auto&              state = *m_impl;
	auto&              slot  = state.slots[state.current];
	const vk::Extent2D size {depth.backing.extent.width, depth.backing.extent.height};
	if (motion.backing.extent != depth.backing.extent ||
	    Dxgi(depth.backing.format) == DXGI_FORMAT_UNKNOWN ||
	    Dxgi(motion.backing.format) == DXGI_FORMAT_UNKNOWN)
		return false;
	const auto usage = vk::ImageUsageFlagBits::eTransferDst;
	for (auto [shared, source]:
	     {std::pair {&slot.depth, &depth}, std::pair {&slot.motion, &motion}}) {
		if (shared->image && shared->extent == size && shared->format == source->backing.format)
			continue;
		// Rare (resolution change): the old image may still be read by either API.
		state.WaitD3d12();
		{
			Common::LockGuard queue_lock(state.graphics.queue_mutex);
			Common::LockGuard present_lock(state.graphics.present_queue_mutex);
			RequireVulkanSuccess(state.graphics.device.waitIdle(), "retire XeSS-FG inputs");
		}
		state.Release(*shared);
		if (!state.CreateShared(*shared, source->backing.format, size, usage, false)) return false;
	}
	for (auto [shared, source]:
	     {std::pair {&slot.depth, &depth}, std::pair {&slot.motion, &motion}}) {
		source->Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
		                {}, command);
		vk::ImageMemoryBarrier barrier {};
		barrier.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		barrier.oldLayout           = vk::ImageLayout::eUndefined;
		barrier.newLayout           = vk::ImageLayout::eTransferDstOptimal;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = shared->image;
		barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
		                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr, 1,
		                        &barrier);
		vk::ImageCopy copy {};
		copy.srcSubresource = copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		copy.extent                               = vk::Extent3D {size.width, size.height, 1};
		command.copyImage(source->backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                  shared->image, vk::ImageLayout::eTransferDstOptimal, 1, &copy);
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		barrier.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
		barrier.newLayout     = vk::ImageLayout::eGeneral;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                        vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 0, nullptr,
		                        1, &barrier);
	}
	auto& constants = slot.constants;
	constants       = {};
	// No camera is known for the final frame: identity matrices, motion from the inputs.
	for (int i = 0; i < 4; ++i)
		constants.viewMatrix[i * 5] = constants.projectionMatrix[i * 5] = 1.0f;
	constants.jitterOffsetX      = jitter_x;
	constants.jitterOffsetY      = jitter_y;
	constants.motionVectorScaleX = motion_scale_x;
	constants.motionVectorScaleY = motion_scale_y;
	constants.resetHistory       = reset ? 1u : 0u;
	constants.frameRenderTime    = frame_time_ms;
	slot.has_inputs              = true;
	return true;
}

bool XessFgBridge::Present(uint32_t image) {
	auto&       state = *m_impl;
	const auto& api   = Api();
	auto&       slot  = state.slots[image];
	state.presented   = 0;
	// Its allocator last executed three presents ago.
	if (state.released->GetCompletedValue() < slot.released)
		state.released->SetEventOnCompletion(slot.released, nullptr);
	slot.allocator->Reset();
	slot.list->Reset(slot.allocator.Get(), nullptr);
	state.queue->Wait(state.rendered.Get(), slot.rendered);
	const uint32_t         id = ++state.present_id;
	ComPtr<ID3D12Resource> back;
	state.swapchain->GetBuffer(state.swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
	const D3D12_RESOURCE_BARRIER before[] {
	    Transition(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
	    Transition(slot.color.resource.Get(), D3D12_RESOURCE_STATE_COMMON,
	               D3D12_RESOURCE_STATE_COPY_SOURCE)};
	slot.list->ResourceBarrier(2, before);
	slot.list->CopyResource(back.Get(), slot.color.resource.Get());
	const D3D12_RESOURCE_BARRIER after[] {
	    Transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
	    Transition(slot.color.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
	               D3D12_RESOURCE_STATE_COMMON)};
	slot.list->ResourceBarrier(2, after);
	const bool generate = slot.has_inputs;
	if (generate) {
		for (auto [shared, type]: {std::pair {&slot.depth, XEFG_SWAPCHAIN_RES_DEPTH},
		                           std::pair {&slot.motion, XEFG_SWAPCHAIN_RES_MOTION_VECTOR}}) {
			xefg_swapchain_d3d12_resource_data_t data {};
			data.type          = type;
			data.validity      = XEFG_SWAPCHAIN_RV_ONLY_NOW;
			data.resourceSize  = {shared->extent.width, shared->extent.height};
			data.pResource     = shared->resource.Get();
			data.incomingState = D3D12_RESOURCE_STATE_COMMON;
			api.tag(state.context, slot.list.Get(), id, &data);
		}
		api.constants(state.context, id, &slot.constants);
	}
	slot.list->Close();
	ID3D12CommandList* lists[] {slot.list.Get()};
	state.queue->ExecuteCommandLists(1, lists);
	slot.released = ++state.release_value;
	state.queue->Signal(state.released.Get(), slot.released);
	if (generate != state.generating) {
		api.enable(state.context, generate ? 1u : 0u);
		state.generating = generate;
	}
	api.present_id(state.context, id);
	if (state.xell) api.marker(state.xell, id, XELL_PRESENT_START);
	const auto result = state.swapchain->Present(
	    state.vsync ? 1 : 0, !state.vsync && state.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
	if (state.xell) api.marker(state.xell, id, XELL_PRESENT_END);
	if (result == DXGI_STATUS_OCCLUDED) return true;
	if (FAILED(result)) {
		LOGF("XeSS-FG present failed: 0x%08x\n", unsigned(result));
		state.available = false;
		return false;
	}
	xefg_swapchain_present_status_t status {};
	state.presented = api.status(state.context, &status) == XEFG_SWAPCHAIN_RESULT_SUCCESS
	                      ? status.framesPresented
	                      : 1;
	if (generate && !state.status_logged && id > 60 && status.framesPresented < 2) {
		LOGF("XeSS-FG did not generate: result %d, enabled %u, frames %u\n",
		     int(status.frameGenResult), status.isFrameGenEnabled, status.framesPresented);
		state.status_logged = true;
	}
	return true;
}

uint32_t XessFgBridge::PresentedFrames() const {
	return m_impl->presented;
}

void XessFgBridge::MarkSimulation() {
	auto& state = *m_impl;
	if (!state.xell) return;
	// The frame starts on the presentation thread: the guest has already simulated it.
	Api().sleep(state.xell, state.present_id + 1);
	Api().marker(state.xell, state.present_id + 1, XELL_SIMULATION_START);
	Api().marker(state.xell, state.present_id + 1, XELL_SIMULATION_END);
}

void XessFgBridge::MarkRenderSubmit(bool start) {
	auto& state = *m_impl;
	if (state.xell)
		Api().marker(state.xell, state.present_id + 1,
		             start ? XELL_RENDERSUBMIT_START : XELL_RENDERSUBMIT_END);
}

#else

struct XessFgBridge::Impl {};
XessFgBridge::XessFgBridge(GraphicContext&) {}
XessFgBridge::~XessFgBridge() = default;
bool XessFgBridge::Available() const {
	return false;
}
bool XessFgBridge::CreateSwapchain(void*, vk::Extent2D, bool) {
	return false;
}
void       XessFgBridge::DestroySwapchain() {}
vk::Format XessFgBridge::Format() const {
	return vk::Format::eUndefined;
}
const std::vector<vk::Image>& XessFgBridge::Images() const {
	static const std::vector<vk::Image> none;
	return none;
}
uint32_t XessFgBridge::Acquire() {
	return 0;
}
void XessFgBridge::AddSubmitSync(SubmitInfo&, uint32_t) {}
bool XessFgBridge::RecordInputs(vk::CommandBuffer, Image&, Image&, float, float, float, float, bool,
                                float) {
	return false;
}
bool XessFgBridge::Present(uint32_t) {
	return false;
}
uint32_t XessFgBridge::PresentedFrames() const {
	return 1;
}
void XessFgBridge::MarkSimulation() {}
void XessFgBridge::MarkRenderSubmit(bool) {}

#endif

} // namespace Libs::Graphics
