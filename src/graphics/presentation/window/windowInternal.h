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

#if defined(__APPLE__)
#include <functional>
#endif

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

#if defined(__APPLE__)
	// AppKit only allows window operations (show, icon, title, view/layer changes) on the
	// main thread; the present thread marshals them through the SDL main loop with these.
	// wait=true blocks until the task has run on the main thread.
	void RunOnMainThread(std::function<void()> task, bool wait);
	void DrainMainThreadTasks();
#endif

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

#if defined(__APPLE__)
	Common::Mutex                      main_task_mutex;
	Common::CondVar                    main_task_done;
	std::vector<std::function<void()>> main_tasks;        // guarded by main_task_mutex
	uint64_t                           main_tasks_queued = 0; // guarded by main_task_mutex
	uint64_t                           main_tasks_run    = 0; // guarded by main_task_mutex
#endif
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_
