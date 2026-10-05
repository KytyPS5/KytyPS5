#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_

#include <SDL3/SDL.h>

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
class DlssFrameGeneration;

struct SurfaceCapabilities {
	vk::SurfaceCapabilitiesKHR        capabilities {};
	std::vector<vk::SurfaceFormatKHR> formats;
	std::vector<vk::PresentModeKHR>   present_modes;
};

struct WindowLoopState {
	SDL_Event        event {};
	bool             need_exit = false;
	std::atomic_bool paused    = false;
};

struct WindowContext {
	WindowContext();
	~WindowContext();
	KYTY_CLASS_NO_COPY(WindowContext);

	[[nodiscard]] static vk::PhysicalDeviceVulkan11Features RequiredVulkan11Features() noexcept;
	[[nodiscard]] static vk::PhysicalDeviceVulkan12Features RequiredVulkan12Features() noexcept;
	[[nodiscard]] static vk::PhysicalDeviceVulkan13Features RequiredVulkan13Features() noexcept;
	[[nodiscard]] static uint32_t InitialWindowFlags(bool fullscreen) noexcept;
	void                                                    CreateVulkan();
	void                                                    RecreateSurface();
	void                                                    RefreshSurfaceCapabilities();
	void                                                    UpdateIcon();
	void UpdateTitle(bool dlss_active = false, bool new_guest_frame = true);
	void                                                    Resize(int width, int height);
	void ProcessWindowEvent(const SDL_WindowEvent& event);
	void ProcessDisplayEvent(const SDL_DisplayEvent& event);
	void ProcessEvent(double time_seconds);
	void Run();

	GraphicContext                 graphic_ctx;
	SDL_Window*                    window        = nullptr;
	vk::SurfaceKHR                 surface       = nullptr;
	SurfaceCapabilities            surface_capabilities;
	std::unique_ptr<RenderContext> render_context;
	std::unique_ptr<Presenter>     presenter;
	std::unique_ptr<DlssFrameGeneration> frame_generation;
	WindowLoopState                loop;
	uint64_t title_fps_start = 0, title_frame_number = 0, title_fps_frames = 0;
	bool title_initialized = false;

	Common::Mutex mutex;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_
