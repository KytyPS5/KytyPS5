#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_SCOPE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_SCOPE_H_

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/element/frame/frameView.h"

namespace Libs::Graphics::SystemUi {

/// Only named here, so a component that opens or reads a scope never sees the builder's members.
class FrameBuilder;

/// Makes a frame current on this thread along with the context it draws in, for as long as the
/// scope lives, and puts back what it replaced when it ends. The frame lives in the element layer,
/// so it can't sit in core's ContextContainer, and the scope holds a ContextScope instead of
/// widening one. Scopes nest and must end in the reverse order they were opened, as local variables
/// do. A ContextScope opened inside one changes the context and keeps the frame. The scope doesn't
/// call Begin, which stays the caller's to do once per frame, so a nested scope over the same
/// frame never clears what is being built. Its body is in frameScope.cpp, where the builder is
/// complete, so this header needn't include it.
class FrameScope {
public:
	/// Makes `frame` current with a context of `theme`, `scale`, `fonts`, the frame's `clock`, the
	/// player's `motion` and the frame's `pointer`. `frame` and `theme` must outlive the scope. A
	/// scope that isn't named ends on the same line, so the compiler warns about one.
	[[nodiscard]] FrameScope(FrameBuilder& frame, const ThemeContext& theme,
	                         const ScaleContext& scale, const FontsContext& fonts,
	                         const ClockContext& clock, const MotionContext& motion = {},
	                         const PointerContext& pointer = {});

	// A temporary theme would be gone while the context still refers to it.
	FrameScope(FrameBuilder& frame, const ThemeContext&& theme, const ScaleContext& scale,
	           const FontsContext& fonts, const ClockContext& clock,
	           const MotionContext& motion = {}, const PointerContext& pointer = {}) = delete;

	/// Puts back the frame that was current before this scope, and then its context ends.
	~FrameScope();

	FrameScope(const FrameScope&)            = delete;
	FrameScope& operator=(const FrameScope&) = delete;
	FrameScope(FrameScope&&)                 = delete;
	FrameScope& operator=(FrameScope&&)      = delete;

private:
	// The frame is made current in the constructor's body and put back in the destructor's, so the
	// context is open the whole time the frame is current, wherever this member is declared.
	ContextScope m_context;
	// The frame that was current before this scope, put back when it ends.
	FrameBuilder* m_previous;
	// The scope that was open before this one. Ending is checked against the scope rather than the
	// frame, so two scopes over one frame, which nesting allows, still can't end out of order
	// unnoticed.
	const FrameScope* m_previous_scope;
};

/// The frame of the innermost open FrameScope on this thread, as components see it. Calling it with
/// no scope open is a mistake and asserts. A release build then gets a frame of its own that was
/// never begun, so any element built on it is refused and the tree draws nothing. Text and bytes
/// interned in it are still stored, and as nothing begins it they are never cleared.
[[nodiscard]] FrameView& CurrentFrame();

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_SCOPE_H_
