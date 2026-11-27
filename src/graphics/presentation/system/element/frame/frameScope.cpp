#include "graphics/presentation/system/element/frame/frameScope.h"

#include "graphics/presentation/system/element/frame/frameBuilder.h"

#include <cassert>

namespace Libs::Graphics::SystemUi {

namespace {

// The frame of the innermost open scope on this thread, or null when none is open. Per thread, so
// a scope opened on the render thread is never seen by another. It holds the builder, so both
// CurrentFrame and CurrentBuilder read it without a cast.
thread_local FrameBuilder* t_current_frame = nullptr;
// The innermost open scope on this thread, so each scope can check it is the one ending.
thread_local const FrameScope* t_current_scope = nullptr;

// The frame a release build hands out when no scope is open. Per thread, as a frame is not thread
// safe, and never begun, so anything built on it is refused.
FrameBuilder& FallbackFrame() {
	static thread_local FrameBuilder t_fallback;
	return t_fallback;
}

} // namespace

FrameScope::FrameScope(FrameBuilder& frame, const ThemeContext& theme, const ScaleContext& scale,
                       const FontsContext& fonts, const ClockContext& clock,
                       const MotionContext& motion, const PointerContext& pointer)
    : m_context(theme, scale, fonts, clock, motion, pointer), m_previous(t_current_frame),
      m_previous_scope(t_current_scope) {
	t_current_frame = &frame;
	t_current_scope = this;
}

FrameScope::~FrameScope() {
	assert(t_current_scope == this && "FrameScopes must end in reverse order");
	t_current_frame = m_previous;
	t_current_scope = m_previous_scope;
}

FrameView& CurrentFrame() {
	return CurrentBuilder();
}

FrameBuilder& CurrentBuilder() {
	FrameBuilder* const current = t_current_frame;
	assert(current != nullptr && "CurrentFrame and CurrentBuilder need an open FrameScope");
	return current != nullptr ? *current : FallbackFrame();
}

} // namespace Libs::Graphics::SystemUi
