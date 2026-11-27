#include "graphics/presentation/system/element/primitives/image/paint.h"

#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/framePaint.h"
#include "graphics/presentation/system/element/primitives/image/image.h"
#include "graphics/presentation/system/imgui/draw/image.h"

namespace Libs::Graphics::SystemUi {

void PaintImage(const FrameBuilder& frame, AnyElement image, ImDrawList& list,
                const PaintState& state) {
	const ImageProps* const               props  = frame.FindAs<ImageProps>(image);
	const FrameView::ElementLayout* const layout = frame.LayoutOf(image);
	if (props == nullptr || layout == nullptr) {
		return;
	}
	DrawImage(list, props->texture, props->size, layout->arranged, props->self.styles,
	          state.opacity);
}

} // namespace Libs::Graphics::SystemUi
