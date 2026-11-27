#include "graphics/presentation/system/element/primitives/text/text.h"

#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/primitives/text/layout.h"
#include "graphics/presentation/system/element/primitives/text/paint.h"

namespace Libs::Graphics::SystemUi {

namespace {

// How texts are measured, placed and painted, set on every frame a text is built in.
constexpr FrameHooks TEXT_FRAME_HOOKS {
    .measure = &MeasureText, .arrange = &ArrangeText, .paint = &PaintText};

} // namespace

TextElement Text(const TextProps& props) {
	FrameBuilder& frame = CurrentBuilder();
	// Set on every call, as Box does, so any frame a text is built in can lay it out and paint it.
	frame.SetFrameHooks<TextProps>(TEXT_FRAME_HOOKS);
	// The caller's string may not outlive the call, so the frame keeps its own copy.
	TextProps kept = props;
	kept.text      = frame.Intern(props.text);
	return frame.AddNode(kept, {}, props.self.layout);
}

} // namespace Libs::Graphics::SystemUi
