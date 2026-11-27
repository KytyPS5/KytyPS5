#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_VULKAN_HOST_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_VULKAN_HOST_H_

// These match host_gpu/vulkanCommon.h, so Vulkan is called through function pointers loaded at
// run time and the gallery needs no Vulkan import library.
#if defined(VULKAN_H_) && !defined(VK_NO_PROTOTYPES)
#error "vulkan.h was included before VK_NO_PROTOTYPES was defined"
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#ifndef VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 1
#endif
#ifndef VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_CONSTRUCTORS
#endif
#ifndef VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_EXCEPTIONS
#endif

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <cstdint>
#include <vulkan/vulkan.hpp>

struct SDL_Window;

namespace Libs::Graphics::SystemUi::Gallery {

/// How many images the gallery may hold at once, counting ones still retiring. Each takes a slot in
/// the descriptor pool the backend makes, so the pool is sized for them beside its own textures.
inline constexpr uint32_t GALLERY_IMAGE_CAPACITY = 4;

/// How many images the gallery's image cache holds at once, such as the trophies' icons. The pool
/// is sized for it too, so the cache is made with it and the two can't drift.
inline constexpr uint32_t GALLERY_DECODED_IMAGE_CAPACITY = 6;

/// What a frame step did. Skip means this frame wasn't drawn but the next may be, as when the
/// window is minimised or the swapchain needs rebuilding. Fatal means Vulkan can not continue and
/// the caller should stop.
enum class FrameResult : uint8_t { Ok, Skip, Fatal };

/// Owns the Vulkan instance, device, queue, surface and swapchain for one SDL window, and draws
/// ImGui's output into it. Every failure is written to stderr and reported by a false return or a
/// Fatal result, as exceptions are off. The swapchain format is 8-bit UNORM and presenting is FIFO.
class VulkanHost {
public:
	/// A host with nothing brought up yet. Init brings Vulkan up.
	VulkanHost();
	/// Takes down whatever Init brought up.
	~VulkanHost();
	VulkanHost(const VulkanHost&)            = delete;
	VulkanHost& operator=(const VulkanHost&) = delete;
	VulkanHost(VulkanHost&&)                 = delete;
	VulkanHost& operator=(VulkanHost&&)      = delete;

	/// Brings Vulkan up for `window`, which must have been created with SDL_WINDOW_VULKAN, and
	/// builds its swapchain at the window's pixel size. Safe to call once.
	[[nodiscard]] bool Init(SDL_Window* window);

	/// Starts ImGui's Vulkan backend. Needs a current ImGui context and a successful Init.
	[[nodiscard]] bool InitImGuiBackend();

	/// Prepares a frame, rebuilding the swapchain first if the window was resized or the last
	/// present found it out of date. Call before ImGui::NewFrame. Returns Skip when the window
	/// has no area, as when minimised, and the frame should be skipped.
	[[nodiscard]] FrameResult BeginFrame();

	/// Draws `draw_data` over a `clear` background and presents it. `clear` holds the colour's
	/// channels from 0 to 1 as they are stored, so an sRGB-encoded colour stays as it is. Its alpha
	/// is ignored, as the window is opaque. An out of date swapchain skips the frame and is rebuilt
	/// by the next BeginFrame.
	[[nodiscard]] FrameResult Render(ImDrawData* draw_data, const ImVec4& clear);

	/// Waits for the GPU to go idle, then releases everything. The ImGui context that
	/// InitImGuiBackend used must still be current. Safe to call more than once.
	void Shutdown();

private:
	/// Creates the instance with SDL's surface extensions, and portability enumeration when
	/// offered so MoltenVK is listed.
	bool CreateInstance();
	/// Picks the GPU and its graphics queue family, preferring a discrete GPU.
	bool PickDevice();
	/// Creates the device with one graphics queue and the swapchain extension.
	bool CreateDevice();
	/// Creates the window's surface and checks the graphics queue can present to it.
	bool CreateSurface();
	/// Chooses an 8-bit UNORM format and FIFO presenting, then builds the first swapchain.
	bool CreateSwapchain(int width, int height);
	/// Builds or rebuilds the swapchain at `width` by `height` and records that size.
	bool BuildSwapchain(int width, int height);
	/// Rebuilds the swapchain for a new size once the ImGui backend is running.
	bool ResizeSwapchain(int width, int height);
	/// Hands ImGui's Vulkan backend its functions through the instance's loader.
	bool LoadImGuiFunctions();
	/// Acquires the next swapchain image and waits for its frame's fence. Skip when out of date.
	FrameResult AcquireImage(const ImGui_ImplVulkanH_Frame*& frame);
	/// Records the clear and ImGui's draw data for `frame` and submits it.
	FrameResult RecordAndSubmit(const ImGui_ImplVulkanH_Frame& frame, ImDrawData* draw_data);
	/// Presents the submitted image, unless the swapchain is already known to be stale.
	FrameResult Present();

	SDL_Window*              m_window = nullptr;
	vk::Instance             m_instance;
	vk::PhysicalDevice       m_physical_device;
	vk::Device               m_device;
	vk::Queue                m_queue;
	vk::SurfaceKHR           m_surface;
	uint32_t                 m_queue_family = 0;
	ImGui_ImplVulkanH_Window m_window_data;
	bool                     m_swapchain_built = false;
	bool                     m_imgui_backend   = false;
	int                      m_built_width     = 0;
	int                      m_built_height    = 0;
	bool                     m_rebuild         = false;
};

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_VULKAN_HOST_H_
