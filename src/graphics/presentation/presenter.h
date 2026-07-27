#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_

#include "common/common.h"

#include <memory>

namespace Libs::Graphics {

class CommandBuffer;
class RenderContext;
struct ImageInfo;
struct WindowContext;

class Presenter final {
public:
	struct Frame;

	explicit Presenter(WindowContext& window);
	~Presenter();
	KYTY_CLASS_NO_COPY(Presenter);

	[[nodiscard]] Frame& PrepareFrame(CommandBuffer& command, const ImageInfo& info);
	[[nodiscard]] Frame& PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
	                                       CommandBuffer* producer = nullptr);
	[[nodiscard]] Frame* PrepareLastFrame();
	[[nodiscard]] bool   IsGuestPaused() const noexcept;
	[[nodiscard]] RenderContext& Renderer() const noexcept;
	void                 Present(Frame& frame, bool reuse = false);
	void                 Discard(Frame& frame);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
