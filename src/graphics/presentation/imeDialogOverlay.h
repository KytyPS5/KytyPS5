#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_IMEDIALOGOVERLAY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_IMEDIALOGOVERLAY_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <memory>

union SDL_Event;

namespace Libs::Graphics {

struct GraphicContext;

void InitializeImeDialogInput();
void ShutdownImeDialogInput();
bool ProcessImeDialogInput(const SDL_Event& event);

class ImeDialogOverlay final {
public:
	explicit ImeDialogOverlay(GraphicContext& graphics);
	~ImeDialogOverlay();
	KYTY_CLASS_NO_COPY(ImeDialogOverlay);

	[[nodiscard]] bool PrepareFrame(vk::Extent2D extent, vk::Format format, uint32_t image_count);
	void               Record(vk::CommandBuffer command, vk::ImageView target);
	void               ReleaseVulkan();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_IMEDIALOGOVERLAY_H_
