#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_

#include "SDL_events.h"
#include "SDL_video.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace Libs::Graphics {

class Presenter;
class RenderContext;

struct SurfaceCapabilities {
	vk::SurfaceCapabilitiesKHR        capabilities {};
	std::vector<vk::SurfaceFormatKHR> formats;
	std::vector<vk::PresentModeKHR>   present_modes;
};

struct WindowLoopState {
	SDL_Event       event {};
	bool            need_exit = false;
	std::atomic_bool paused    = false;
};

struct WindowContext {
	WindowContext();
	~WindowContext();
	KYTY_CLASS_NO_COPY(WindowContext);

	[[nodiscard]] static vk::PhysicalDeviceVulkan13Features
	RequiredVulkan13Features() noexcept;
	void CreateVulkan();
	void RecreateSurface();
	void RefreshSurfaceCapabilities();
	void UpdateIcon();
	void UpdateTitle();
	void Resize(uint32_t width, uint32_t height);
	void ProcessWindowEvent(const SDL_WindowEvent& event);
	void ProcessDisplayEvent(const SDL_DisplayEvent& event);
	void ProcessEvent(double time_seconds);
	void Run();

	GraphicContext      graphic_ctx;
	SDL_Window*         window        = nullptr;
	bool                window_hidden = true;
	vk::SurfaceKHR      surface       = nullptr;
	SurfaceCapabilities surface_capabilities;
	std::unique_ptr<RenderContext> render_context;
	std::unique_ptr<Presenter> presenter;
	WindowLoopState            loop;

	char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {0};
	char processor_name[64]                            = {0};

	Common::Mutex mutex;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_
