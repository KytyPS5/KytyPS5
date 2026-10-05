#ifndef KYTY_GRAPHICS_PRESENTATION_EMULATOR_DLSS_INPUTS_H_
#define KYTY_GRAPHICS_PRESENTATION_EMULATOR_DLSS_INPUTS_H_

#include "graphics/presentation/dlss.h"

namespace Libs::Graphics {
// Generic final-frame reconstruction: image-space motion, resampling jitter,
// and a neutral depth plane. These are not guest camera/geometry buffers.
class EmulatorDlssInputs {
public:
	EmulatorDlssInputs(GraphicContext& graphics, CommandScheduler& scheduler);
	~EmulatorDlssInputs();
	KYTY_CLASS_NO_COPY(EmulatorDlssInputs);
	[[nodiscard]] std::optional<DlssFrameInputs> Prepare(CommandBuffer& command, Image& source,
	                                                    vk::Extent2D input_extent);
	void Reset();
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics
#endif
