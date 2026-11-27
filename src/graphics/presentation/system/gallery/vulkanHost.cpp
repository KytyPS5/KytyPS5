#include "graphics/presentation/system/gallery/vulkanHost.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

// Exactly one translation unit in the gallery holds the dispatcher's storage. The gallery does
// not link host_gpu/vulkanCommon.cpp, which holds the emulator's.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Libs::Graphics::SystemUi::Gallery {
namespace {

constexpr uint32_t API_VERSION     = VK_API_VERSION_1_1;
constexpr uint32_t MIN_IMAGE_COUNT = 2;
constexpr uint32_t NO_QUEUE_FAMILY = UINT32_MAX;
constexpr float    QUEUE_PRIORITY  = 1.0f;
constexpr uint64_t NO_TIMEOUT      = UINT64_MAX;
constexpr float    OPAQUE_ALPHA    = 1.0f;
// The swapchain images are only drawn into and presented, so they need no usage beyond ImGui's.
constexpr VkImageUsageFlags NO_EXTRA_IMAGE_USAGE    = 0;
constexpr const char*       PORTABILITY_ENUMERATION = "VK_KHR_portability_enumeration";
constexpr const char*       PORTABILITY_SUBSET      = "VK_KHR_portability_subset";

// 8-bit UNORM formats, so the theme's sRGB-encoded colours reach the display without a second
// conversion by the hardware.
constexpr std::array<VkFormat, 2> SWAPCHAIN_FORMATS = {VK_FORMAT_B8G8R8A8_UNORM,
                                                       VK_FORMAT_R8G8B8A8_UNORM};

// Writes `what` to stderr when `result` isn't success, and returns whether it was.
bool Succeeded(vk::Result result, const char* what) {
	if (result == vk::Result::eSuccess) {
		return true;
	}
	std::fprintf(stderr, "system_ui_gallery: could not %s (%s)\n", what,
	             vk::to_string(result).c_str());
	return false;
}

// ImGui's backend reports its own Vulkan failures through this.
void ReportBackendResult(VkResult result) {
	if (result != VK_SUCCESS) {
		std::fprintf(stderr, "system_ui_gallery: ImGui Vulkan backend error %d\n",
		             static_cast<int>(result));
	}
}

// Whether `name` is in `properties`.
bool HasExtension(const std::vector<vk::ExtensionProperties>& properties, const char* name) {
	return std::any_of(properties.begin(), properties.end(), [name](const auto& property) {
		return std::string_view(property.extensionName) == name;
	});
}

// Hands the backend its Vulkan functions through the instance loader. `user_data` is the
// VkInstance.
PFN_vkVoidFunction LoadBackendFunction(const char* name, void* user_data) {
	const auto instance = *static_cast<VkInstance*>(user_data);
	return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(instance, name);
}

} // namespace

VulkanHost::VulkanHost() = default;

VulkanHost::~VulkanHost() {
	Shutdown();
}

bool VulkanHost::Init(SDL_Window* window) {
	m_window = window;
	// SDL loads the Vulkan library itself when a window is made with SDL_WINDOW_VULKAN. Going
	// through its loader means the gallery needs no link-time Vulkan import library.
	const auto get_instance_proc_addr =
	    reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
	if (get_instance_proc_addr == nullptr) {
		std::fprintf(stderr, "system_ui_gallery: could not load Vulkan (%s)\n", SDL_GetError());
		return false;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

	int width  = 0;
	int height = 0;
	SDL_GetWindowSizeInPixels(window, &width, &height);
	return CreateInstance() && LoadImGuiFunctions() && PickDevice() && CreateDevice() &&
	       CreateSurface() && CreateSwapchain(width, height);
}

bool VulkanHost::CreateInstance() {
	uint32_t           sdl_count      = 0;
	const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
	if (sdl_extensions == nullptr) {
		std::fprintf(stderr, "system_ui_gallery: SDL has no Vulkan extensions (%s)\n",
		             SDL_GetError());
		return false;
	}
	std::vector<const char*> extensions(sdl_extensions, sdl_extensions + sdl_count);

	uint32_t available_count = 0;
	(void)vk::enumerateInstanceExtensionProperties(nullptr, &available_count, nullptr);
	std::vector<vk::ExtensionProperties> available(available_count);
	(void)vk::enumerateInstanceExtensionProperties(nullptr, &available_count, available.data());
	available.resize(available_count);

	// MoltenVK is only listed as a device when portability enumeration is on.
	vk::InstanceCreateFlags flags;
	if (HasExtension(available, PORTABILITY_ENUMERATION)) {
		extensions.push_back(PORTABILITY_ENUMERATION);
		flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
	}

	vk::ApplicationInfo app_info {};
	app_info.pApplicationName = "System UI gallery";
	app_info.apiVersion       = API_VERSION;
	vk::InstanceCreateInfo info {};
	info.flags                   = flags;
	info.pApplicationInfo        = &app_info;
	info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();
	if (!Succeeded(vk::createInstance(&info, nullptr, &m_instance), "create the Vulkan instance")) {
		return false;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(m_instance);
	return true;
}

bool VulkanHost::LoadImGuiFunctions() {
	VkInstance raw_instance = m_instance;
	if (!ImGui_ImplVulkan_LoadFunctions(API_VERSION, LoadBackendFunction, &raw_instance)) {
		std::fprintf(stderr, "system_ui_gallery: could not load the ImGui Vulkan functions\n");
		return false;
	}
	return true;
}

bool VulkanHost::PickDevice() {
	// ImGui's helper prefers a discrete GPU, which is what a design review wants.
	m_physical_device = ImGui_ImplVulkanH_SelectPhysicalDevice(m_instance);
	if (!m_physical_device) {
		std::fprintf(stderr, "system_ui_gallery: no Vulkan device found\n");
		return false;
	}
	m_queue_family = ImGui_ImplVulkanH_SelectQueueFamilyIndex(m_physical_device);
	if (m_queue_family == NO_QUEUE_FAMILY) {
		std::fprintf(stderr, "system_ui_gallery: the Vulkan device has no graphics queue\n");
		return false;
	}
	return true;
}

bool VulkanHost::CreateDevice() {
	uint32_t count = 0;
	(void)m_physical_device.enumerateDeviceExtensionProperties(nullptr, &count, nullptr);
	std::vector<vk::ExtensionProperties> available(count);
	(void)m_physical_device.enumerateDeviceExtensionProperties(nullptr, &count, available.data());
	available.resize(count);

	std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	// A portability device must be told that its subset is accepted.
	if (HasExtension(available, PORTABILITY_SUBSET)) {
		extensions.push_back(PORTABILITY_SUBSET);
	}

	const float               queue_priority = QUEUE_PRIORITY;
	vk::DeviceQueueCreateInfo queue_info {};
	queue_info.queueFamilyIndex = m_queue_family;
	queue_info.queueCount       = 1;
	queue_info.pQueuePriorities = &queue_priority;
	vk::DeviceCreateInfo info {};
	info.queueCreateInfoCount    = 1;
	info.pQueueCreateInfos       = &queue_info;
	info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();
	if (!Succeeded(m_physical_device.createDevice(&info, nullptr, &m_device),
	               "create the Vulkan device")) {
		return false;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device);
	m_device.getQueue(m_queue_family, 0, &m_queue);
	return true;
}

bool VulkanHost::CreateSurface() {
	VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
	if (!SDL_Vulkan_CreateSurface(m_window, m_instance, nullptr, &raw_surface)) {
		std::fprintf(stderr, "system_ui_gallery: could not create the window surface (%s)\n",
		             SDL_GetError());
		return false;
	}
	m_surface            = raw_surface;
	vk::Bool32 supported = VK_FALSE;
	if (!Succeeded(m_physical_device.getSurfaceSupportKHR(m_queue_family, m_surface, &supported),
	               "query surface support") ||
	    supported != VK_TRUE) {
		std::fprintf(stderr, "system_ui_gallery: the graphics queue can't present to the window\n");
		return false;
	}
	return true;
}

bool VulkanHost::CreateSwapchain(int width, int height) {
	m_window_data.Surface       = m_surface;
	m_window_data.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
	    m_physical_device, m_surface, SWAPCHAIN_FORMATS.data(),
	    static_cast<int>(SWAPCHAIN_FORMATS.size()), VK_COLORSPACE_SRGB_NONLINEAR_KHR);
	// The helper falls back to whatever the surface lists first, which could be an sRGB or HDR
	// format. Colours would then differ from the emulator's, so refuse it.
	if (std::find(SWAPCHAIN_FORMATS.begin(), SWAPCHAIN_FORMATS.end(),
	              m_window_data.SurfaceFormat.format) == SWAPCHAIN_FORMATS.end()) {
		std::fprintf(stderr, "system_ui_gallery: the surface has no 8-bit UNORM format\n");
		return false;
	}
	// FIFO is the one present mode every driver must offer, and it paces the gallery to the
	// display.
	constexpr VkPresentModeKHR FIFO = VK_PRESENT_MODE_FIFO_KHR;
	m_window_data.PresentMode =
	    ImGui_ImplVulkanH_SelectPresentMode(m_physical_device, m_surface, &FIFO, 1);
	return BuildSwapchain(width, height);
}

bool VulkanHost::BuildSwapchain(int width, int height) {
	ImGui_ImplVulkanH_CreateOrResizeWindow(m_instance, m_physical_device, m_device, &m_window_data,
	                                       m_queue_family, nullptr, width, height, MIN_IMAGE_COUNT,
	                                       NO_EXTRA_IMAGE_USAGE);
	m_swapchain_built        = true;
	m_built_width            = width;
	m_built_height           = height;
	m_window_data.FrameIndex = 0;
	m_rebuild                = false;
	// Before the ImGui backend starts, the helper's error reporting returns early, so a failed
	// vkCreateSwapchainKHR would otherwise go unseen and leave a null swapchain.
	if (m_window_data.Swapchain == VK_NULL_HANDLE || m_window_data.ImageCount < MIN_IMAGE_COUNT) {
		std::fprintf(stderr, "system_ui_gallery: could not create the swapchain\n");
		return false;
	}
	return true;
}

bool VulkanHost::ResizeSwapchain(int width, int height) {
	ImGui_ImplVulkan_SetMinImageCount(MIN_IMAGE_COUNT);
	return BuildSwapchain(width, height);
}

bool VulkanHost::InitImGuiBackend() {
	ImGui_ImplVulkan_InitInfo info = {};
	info.ApiVersion                = API_VERSION;
	info.Instance                  = m_instance;
	info.PhysicalDevice            = m_physical_device;
	info.Device                    = m_device;
	info.QueueFamily               = m_queue_family;
	info.Queue                     = m_queue;
	// The backend makes the pool, sized for its own textures plus the gallery's images and those
	// its image cache decodes.
	info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE +
	                          GALLERY_IMAGE_CAPACITY + GALLERY_DECODED_IMAGE_CAPACITY;
	info.MinImageCount                = MIN_IMAGE_COUNT;
	info.ImageCount                   = m_window_data.ImageCount;
	info.PipelineInfoMain.RenderPass  = m_window_data.RenderPass;
	info.PipelineInfoMain.Subpass     = 0;
	info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	info.CheckVkResultFn              = ReportBackendResult;
	m_imgui_backend                   = ImGui_ImplVulkan_Init(&info);
	if (!m_imgui_backend) {
		std::fprintf(stderr, "system_ui_gallery: could not start the ImGui Vulkan backend\n");
	}
	return m_imgui_backend;
}

FrameResult VulkanHost::BeginFrame() {
	if ((SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0) {
		return FrameResult::Skip;
	}
	int width  = 0;
	int height = 0;
	SDL_GetWindowSizeInPixels(m_window, &width, &height);
	if (width <= 0 || height <= 0) {
		return FrameResult::Skip;
	}
	// Compared with the size last built for, not the surface's own extent, which some compositors
	// report differently and would force a rebuild every frame.
	if ((m_rebuild || m_built_width != width || m_built_height != height) &&
	    !ResizeSwapchain(width, height)) {
		return FrameResult::Fatal;
	}
	ImGui_ImplVulkan_NewFrame();
	return FrameResult::Ok;
}

FrameResult VulkanHost::AcquireImage(const ImGui_ImplVulkanH_Frame*& frame) {
	const auto&      semaphores = m_window_data.FrameSemaphores[m_window_data.SemaphoreIndex];
	const vk::Result result     = m_device.acquireNextImageKHR(m_window_data.Swapchain, NO_TIMEOUT,
	                                                           semaphores.ImageAcquiredSemaphore,
	                                                           nullptr, &m_window_data.FrameIndex);
	if (result == vk::Result::eErrorOutOfDateKHR) {
		m_rebuild = true;
		return FrameResult::Skip;
	}
	// A suboptimal swapchain still draws. It is rebuilt on the next frame.
	if (result == vk::Result::eSuboptimalKHR) {
		m_rebuild = true;
	} else if (!Succeeded(result, "acquire a swapchain image")) {
		return FrameResult::Fatal;
	}
	frame                 = &m_window_data.Frames[m_window_data.FrameIndex];
	const vk::Fence fence = frame->Fence;
	// Waiting keeps the command pool from being reset while the GPU still reads it. The fence is
	// reset only at submit, so a frame that fails before then can not leave it unsignalled.
	return Succeeded(m_device.waitForFences(1, &fence, VK_TRUE, NO_TIMEOUT), "wait for a frame")
	           ? FrameResult::Ok
	           : FrameResult::Fatal;
}

FrameResult VulkanHost::RecordAndSubmit(const ImGui_ImplVulkanH_Frame& frame,
                                        ImDrawData*                    draw_data) {
	const vk::CommandBuffer commands = frame.CommandBuffer;
	if (!Succeeded(m_device.resetCommandPool(frame.CommandPool, {}), "reset a command pool")) {
		return FrameResult::Fatal;
	}
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	if (!Succeeded(commands.begin(&begin), "begin a command buffer")) {
		return FrameResult::Fatal;
	}
	vk::RenderPassBeginInfo pass {};
	pass.renderPass        = m_window_data.RenderPass;
	pass.framebuffer       = frame.Framebuffer;
	pass.renderArea.extent = vk::Extent2D {static_cast<uint32_t>(m_window_data.Width),
	                                       static_cast<uint32_t>(m_window_data.Height)};
	pass.clearValueCount   = 1;
	pass.pClearValues      = reinterpret_cast<const vk::ClearValue*>(&m_window_data.ClearValue);
	commands.beginRenderPass(&pass, vk::SubpassContents::eInline);
	ImGui_ImplVulkan_RenderDrawData(draw_data, frame.CommandBuffer);
	commands.endRenderPass();

	const auto&         semaphores = m_window_data.FrameSemaphores[m_window_data.SemaphoreIndex];
	const vk::Semaphore wait_semaphore      = semaphores.ImageAcquiredSemaphore;
	const vk::Semaphore signal_semaphore    = semaphores.RenderCompleteSemaphore;
	const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
	vk::SubmitInfo               submit {};
	submit.waitSemaphoreCount   = 1;
	submit.pWaitSemaphores      = &wait_semaphore;
	submit.pWaitDstStageMask    = &wait_stage;
	submit.commandBufferCount   = 1;
	submit.pCommandBuffers      = &commands;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores    = &signal_semaphore;
	const vk::Fence fence       = frame.Fence;
	const bool      submitted   = Succeeded(commands.end(), "end a command buffer") &&
	                       Succeeded(m_device.resetFences(1, &fence), "reset a frame fence") &&
	                       Succeeded(m_queue.submit(1, &submit, fence), "submit a frame");
	return submitted ? FrameResult::Ok : FrameResult::Fatal;
}

FrameResult VulkanHost::Present() {
	// A frame already known to be stale isn't shown. The semaphores are recreated with the
	// swapchain.
	if (m_rebuild) {
		return FrameResult::Ok;
	}
	const vk::Semaphore wait_semaphore =
	    m_window_data.FrameSemaphores[m_window_data.SemaphoreIndex].RenderCompleteSemaphore;
	const vk::SwapchainKHR swapchain = m_window_data.Swapchain;
	vk::PresentInfoKHR     info {};
	info.waitSemaphoreCount = 1;
	info.pWaitSemaphores    = &wait_semaphore;
	info.swapchainCount     = 1;
	info.pSwapchains        = &swapchain;
	info.pImageIndices      = &m_window_data.FrameIndex;
	const vk::Result result = m_queue.presentKHR(&info);
	if (result == vk::Result::eErrorOutOfDateKHR) {
		m_rebuild = true;
		return FrameResult::Skip;
	}
	if (result == vk::Result::eSuboptimalKHR) {
		m_rebuild = true;
	} else if (!Succeeded(result, "present a frame")) {
		return FrameResult::Fatal;
	}
	// One more semaphore set than frames in flight, so the next acquire never reuses one in use.
	m_window_data.SemaphoreIndex =
	    (m_window_data.SemaphoreIndex + 1) % m_window_data.SemaphoreCount;
	return FrameResult::Ok;
}

FrameResult VulkanHost::Render(ImDrawData* draw_data, const ImVec4& clear) {
	if (!m_swapchain_built || draw_data == nullptr || draw_data->DisplaySize.x <= 0.0f ||
	    draw_data->DisplaySize.y <= 0.0f) {
		return FrameResult::Skip;
	}
	auto& value = m_window_data.ClearValue.color.float32;
	value[0]    = clear.x;
	value[1]    = clear.y;
	value[2]    = clear.z;
	value[3]    = OPAQUE_ALPHA;

	const ImGui_ImplVulkanH_Frame* frame  = nullptr;
	FrameResult                    result = AcquireImage(frame);
	if (result == FrameResult::Ok) {
		result = RecordAndSubmit(*frame, draw_data);
	}
	return result == FrameResult::Ok ? Present() : result;
}

void VulkanHost::Shutdown() {
	if (m_device) {
		(void)m_device.waitIdle();
	}
	if (m_imgui_backend) {
		ImGui_ImplVulkan_Shutdown();
		m_imgui_backend = false;
	}
	if (m_swapchain_built) {
		ImGui_ImplVulkanH_DestroyWindow(m_instance, m_device, &m_window_data, nullptr);
		m_swapchain_built = false;
	}
	if (m_surface) {
		m_instance.destroySurfaceKHR(m_surface);
		m_surface = nullptr;
	}
	if (m_device) {
		m_device.destroy();
		m_device = nullptr;
	}
	if (m_instance) {
		m_instance.destroy();
		m_instance = nullptr;
	}
}

} // namespace Libs::Graphics::SystemUi::Gallery
