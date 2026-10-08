#ifndef KYTY_GRAPHICS_PRESENTATION_DLSS_H_
#define KYTY_GRAPHICS_PRESENTATION_DLSS_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <memory>
#include <optional>

namespace Libs::Graphics {
class CommandBuffer;
class CommandScheduler;
class Image;
struct GraphicContext;
struct VulkanImage;

// The emulator supplies reconstructed final-frame inputs by default. Native
// scene adapters can supply actual scene resources before HUD/overlays instead.
// Images must be updated in the texture cache and produced on this scheduler's
// queue. Keep them alive until the recording command's GPU tick has completed.
struct DlssFrameInputs {
	Image* color              = nullptr;
	Image* depth              = nullptr;
	Image* motion_vectors     = nullptr;
	float  jitter_x           = 0.0f; // render pixels; jitter used to render this frame
	float  jitter_y           = 0.0f;
	float  motion_scale_x     = 1.0f; // converts motion vectors to render pixels
	float  motion_scale_y     = 1.0f;
	bool   depth_inverted     = false;
	bool   hdr                = false;
	bool   reset_history      = false; // scene cut, pause/resume, discontinuity
	float  frame_time_ms      = 16.667f;
	Image* bias_current_color = nullptr; // optional per-pixel history rejection
};

// Query extensions before creating Vulkan objects. A failure disables only DLSS.
bool AppendDlssInstanceExtensions(std::vector<const char*>&                   enabled,
                                  const std::vector<vk::ExtensionProperties>& available);
bool AppendDlssDeviceExtensions(GraphicContext& graphics, std::vector<const char*>& enabled,
                                const std::vector<vk::ExtensionProperties>& available);

class DlssProcessor final {
public:
	DlssProcessor(GraphicContext& graphics, CommandScheduler& scheduler);
	~DlssProcessor();
	KYTY_CLASS_NO_COPY(DlssProcessor);
	[[nodiscard]] bool                        Available() const;
	[[nodiscard]] std::optional<vk::Extent2D> OptimalInputExtent(vk::Extent2D output,
	                                                             vk::Extent2D source = {}) const;
	// Call under the renderer mutex, only once per new guest frame, outside
	// dynamic rendering. Output is a single-layer RGBA16F storage image.
	[[nodiscard]] bool Evaluate(CommandBuffer& command, const DlssFrameInputs& inputs,
	                            VulkanImage& output, vk::ImageView output_view);
	void               SkipFrame();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics
#endif
