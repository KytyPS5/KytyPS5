#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_IMEOVERLAY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_IMEOVERLAY_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <memory>

union SDL_Event;

namespace Libs::Graphics {

struct GraphicContext;

struct ImeVisualState {
	bool     active;
	uint64_t revision;
};

void           InitializeImeInput();
void           ShutdownImeInput();
bool           ProcessImeInput(const SDL_Event& event);
ImeVisualState GetImeVisualState() noexcept;

class ImeOverlay final {
public:
	explicit ImeOverlay(GraphicContext& graphics);
	~ImeOverlay();
	KYTY_CLASS_NO_COPY(ImeOverlay);

	[[nodiscard]] bool PrepareFrame(vk::Extent2D extent, vk::Format format, uint32_t image_count);
	void               Record(vk::CommandBuffer command, vk::ImageView target);
	void               ReleaseVulkan();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_IMEOVERLAY_H_
