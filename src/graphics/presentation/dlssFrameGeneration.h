#ifndef KYTY_DLSS_FRAME_GENERATION_H_
#define KYTY_DLSS_FRAME_GENERATION_H_
#include "graphics/presentation/dlss.h"

#include <SDL3/SDL.h>
#include <span>
#include <vector>

namespace Libs::Graphics {
class XessFgBridge;
// Frame-owned snapshots. Producer inputs may be reused before presentation.
struct DlssFgInputs {
	std::unique_ptr<Image> depth, motion;
	float                  jitter_x = 0, jitter_y = 0;
	float                  motion_scale_x = 1, motion_scale_y = 1;
	float                  frame_time_ms = 16.666667f;
	bool                   reset = true, depth_inverted = false;
	vk::Semaphore          completion;
	uint64_t               completion_value = 0;
	bool                   pending          = false;
	DlssFgInputs();
	~DlssFgInputs();
	[[nodiscard]] bool Ready(GraphicContext& graphics) const;
	void               Wait(GraphicContext& graphics);
};
bool CaptureDlssFgInputs(GraphicContext& graphics, CommandScheduler& scheduler,
                         CommandBuffer& command, const DlssFrameInputs& inputs,
                         std::unique_ptr<DlssFgInputs>& snapshot);

class DlssFrameGeneration final {
public:
	DlssFrameGeneration();
	~DlssFrameGeneration();
	KYTY_CLASS_NO_COPY(DlssFrameGeneration);
	// Initialize before Vulkan; returned loader supplies every required SL proxy.
	PFN_vkGetInstanceProcAddr Initialize(PFN_vkGetInstanceProcAddr native);
	bool       CreateSurface(SDL_Window* window, vk::Instance instance, vk::SurfaceKHR& surface);
	vk::Result CreateInstance(const vk::InstanceCreateInfo& info, vk::Instance& instance);
	// Negotiate optional FG requirements after selecting the physical device.
	// False retires Streamline; restore the native dispatcher before device creation.
	bool               ConfigureDeviceExtensions(std::span<const vk::ExtensionProperties> available,
	                                             std::vector<const char*>&                enabled);
	void               OnDevice(GraphicContext& graphics);
	[[nodiscard]] bool Available() const;
	[[nodiscard]] bool Hooked() const;
	[[nodiscard]] bool External() const;
	[[nodiscard]] bool Enabled() const;
	[[nodiscard]] bool Foreground() const;
	// Returns true when switching mode requires swapchain recreation.
	bool SetEnabled(bool enabled);
	// Reflex may sleep: call before acquiring the renderer mutex.
	bool BeginFrame();
	// Source must be in TransferSrc. External FG uses the presentation queue;
	// the returned image is retained until its submitted commands complete.
	Image*   Interpolate(CommandScheduler& scheduler, CommandBuffer& command, DlssFgInputs& inputs,
	                     const VulkanImage& source, vk::Extent2D output);
	void     InterpolatedPresented();
	bool     TagFrame(CommandBuffer& command, DlssFgInputs& inputs, vk::Extent2D output);
	void     PresentStart();
	void     PresentEnd(DlssFgInputs* inputs = nullptr, bool new_frame = true);
	uint32_t PresentedFrames() const;
	uint64_t TotalPresentedFrames() const;
	uint32_t TotalSubmittedFrames() const;
	// XeSS Frame Generation presents through D3D12 instead of a Vulkan swap chain.
	[[nodiscard]] XessFgBridge* Bridge() const;
	[[nodiscard]] void*         NativeWindow() const;
	void                        Shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics
#endif
