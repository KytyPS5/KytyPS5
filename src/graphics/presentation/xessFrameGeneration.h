#ifndef KYTY_GRAPHICS_PRESENTATION_XESS_FRAME_GENERATION_H_
#define KYTY_GRAPHICS_PRESENTATION_XESS_FRAME_GENERATION_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <memory>
#include <vector>

namespace Libs::Graphics {
class Image;
struct GraphicContext;
struct SubmitInfo;

// XeSS Frame Generation for the Vulkan renderer. XeFG exists only for D3D12, so while it
// is selected the window presents through XeFG's DXGI swap chain on a D3D12 device of
// the same GPU: Vulkan composes each frame into an image shared with D3D12, together
// with the frame's depth and motion, and D3D12 copies it to the back buffer and presents.
// XeFG then inserts and paces the generated frames. Windows only.
class XessFgBridge final {
public:
	// OptiScaler backend with Frame Generation set to XeSS.
	[[nodiscard]] static bool Selected();
	static void               AppendDeviceExtensions(std::vector<const char*>&                   enabled,
	                                                 const std::vector<vk::ExtensionProperties>& available);

	explicit XessFgBridge(GraphicContext& graphics);
	~XessFgBridge();
	KYTY_CLASS_NO_COPY(XessFgBridge);

	[[nodiscard]] bool Available() const;

	// Presentation. The images replace the Vulkan swap chain images; they end each frame
	// in VK_IMAGE_LAYOUT_GENERAL.
	bool                     CreateSwapchain(void* hwnd, vk::Extent2D extent, bool vsync);
	void                     DestroySwapchain();
	[[nodiscard]] vk::Format Format() const;
	[[nodiscard]] const std::vector<vk::Image>& Images() const;
	[[nodiscard]] uint32_t                      Acquire();
	// The frame's Vulkan submission waits for D3D12 to release the image and signals it.
	void AddSubmitSync(SubmitInfo& submit, uint32_t image);
	// Copies the frame's generation inputs into the acquired image's shared inputs.
	bool RecordInputs(vk::CommandBuffer command, Image& depth, Image& motion, float jitter_x,
	                  float jitter_y, float motion_scale_x, float motion_scale_y, bool reset,
	                  float frame_time_ms);
	// Presents through XeFG; generates a frame when this image has inputs.
	bool                   Present(uint32_t image);
	[[nodiscard]] uint32_t PresentedFrames() const;

	// XeLL markers for the next present.
	void MarkSimulation();
	void MarkRenderSubmit(bool start);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif
