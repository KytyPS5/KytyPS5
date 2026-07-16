#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_

#include "SDL_video.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

struct WindowGame;

struct SurfaceCapabilities {
	VkSurfaceCapabilitiesKHR        capabilities {};
	std::vector<VkSurfaceFormatKHR> formats;
	std::vector<VkPresentModeKHR>   present_modes;
	bool                            format_srgb_bgra32  = false;
	bool                            format_unorm_bgra32 = false;
};

struct WindowContext {
	GraphicContext       graphic_ctx;
	VulkanSwapchain*     swapchain            = nullptr;
	SDL_Window*          window               = nullptr;
	bool                 window_hidden        = true;
	VkSurfaceKHR         surface              = nullptr;
	SurfaceCapabilities* surface_capabilities = nullptr;
	WindowGame*          game                 = nullptr;

	char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {0};
	char processor_name[64]                            = {0};

	Common::Mutex   mutex;
	bool            graphic_initialized = false;
	Common::CondVar graphic_initialized_condvar;
};

extern WindowContext* g_window_ctx;

void VulkanGetSurfaceCapabilities(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                  SurfaceCapabilities* capabilities);
VulkanSwapchain* VulkanCreateSwapchain(GraphicContext* ctx, uint32_t image_count);
void             VulkanCreate(WindowContext* ctx);

void WindowUpdateIcon();
void WindowUpdateTitle();

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_
