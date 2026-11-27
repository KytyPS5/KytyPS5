#include "graphics/presentation/system/element/primitives/text/paint.h"

#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/framePaint.h"
#include "graphics/presentation/system/element/primitives/text/layout.h"

#include <algorithm>

namespace Libs::Graphics::SystemUi {

void PaintText(const FrameBuilder& frame, AnyElement text, ImDrawList& list,
               const PaintState& state) {
	const TextProps* const                props    = frame.FindAs<TextProps>(text);
	const FrameView::ElementLayout* const layout   = frame.LayoutOf(text);
	const TextLayout* const               laid_out = FindLaidOut(frame, text);
	if (props == nullptr || layout == nullptr || laid_out == nullptr) {
		return;
	}
	// Centred in the spare height, but never above the rectangle, so a first line is never lost.
	const Rect& arranged = layout->arranged;
	const float spare    = std::max(0.0f, arranged.height - laid_out->size.y);
	const Rect  drawn {arranged.x, arranged.y + spare / 2.0f, arranged.width, laid_out->size.y};
	DrawText(list, *laid_out, ToDrawTextProps(*props), drawn, state.opacity);
}

} // namespace Libs::Graphics::SystemUi
