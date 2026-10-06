#ifndef KYTY_GRAPHICS_PRESENTATION_EMULATOR_DLSS_INPUTS_H_
#define KYTY_GRAPHICS_PRESENTATION_EMULATOR_DLSS_INPUTS_H_

#include "graphics/presentation/dlss.h"

namespace Libs::Graphics {
// Uses captured guest geometry/depth where validated coverage is available,
// with image-space motion and a neutral depth plane elsewhere.
class EmulatorDlssInputs {
public:
	EmulatorDlssInputs(GraphicContext& graphics, CommandScheduler& scheduler);
	~EmulatorDlssInputs();
	KYTY_CLASS_NO_COPY(EmulatorDlssInputs);
	[[nodiscard]] std::optional<DlssFrameInputs> Prepare(CommandBuffer& command, Image& source,
	                                                   vk::Extent2D input_extent, bool reconstruct_color = true,
	                                                   Image* geometry_motion_depth = nullptr);
	// Ordinary color resampling for a rejected DLSS frame. No jitter/history update.
	[[nodiscard]] bool ResampleColor(CommandBuffer& command, Image& source,
	                                 VulkanImage& output, vk::ImageView output_view);
	void Reset();
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics
#endif
