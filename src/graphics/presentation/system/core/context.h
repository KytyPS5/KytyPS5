#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_CONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_CONTEXT_H_

#include "graphics/presentation/system/core/clock.h"
#include "graphics/presentation/system/core/fonts.h"
#include "graphics/presentation/system/core/motion.h"
#include "graphics/presentation/system/core/pointer.h"
#include "graphics/presentation/system/core/scale.h"
#include "graphics/presentation/system/core/theme.h"

#include <cassert>

namespace Libs::Graphics::SystemUi {

/// Everything a component needs to draw. A ContextScope makes one current and CurrentContext()
/// reads it, so functions don't take it as a parameter. Components read the theme from here rather
/// than DEFAULT_THEME, so a theme loaded at run time reaches all of them. The theme must outlive
/// the context, so build the context from a named theme, never a temporary. `clock` holds the
/// frame's moment, read once per frame, so everything drawn in a frame animates from the same
/// moment, `motion` how much the player lets it move, and `pointer` the mouse as this frame read
/// it.
struct ContextContainer {
	const ThemeContext& theme;
	ScaleContext        scale;
	FontsContext        fonts;
	ClockContext        clock;
	MotionContext       motion;
	PointerContext      pointer;
};

[[nodiscard]] inline const ContextContainer& CurrentContext();

/// Makes a ContextContainer current on this thread for as long as the scope lives, and puts back
/// the one it replaced when it ends. Scopes nest, so a preview drawn at another size opens its own
/// scope inside the page's and the page's context returns after it. Scopes must end in the reverse
/// order they were opened, as local variables do.
class ContextScope {
public:
	/// Makes a context of `theme`, `scale`, `fonts`, the frame's `clock`, the player's `motion`
	/// and the frame's `pointer` current. `theme` must outlive the scope. A scope that isn't named
	/// ends on the same line, so the compiler warns about one.
	[[nodiscard]] ContextScope(const ThemeContext& theme, const ScaleContext& scale,
	                           const FontsContext& fonts, const ClockContext& clock,
	                           const MotionContext& motion = {}, const PointerContext& pointer = {})
	    : m_context {theme, scale, fonts, clock, motion, pointer}, m_previous(t_current_context) {
		t_current_context = &m_context;
	}

	// A temporary theme would be gone while the context still refers to it.
	ContextScope(const ThemeContext&& theme, const ScaleContext& scale, const FontsContext& fonts,
	             const ClockContext& clock, const MotionContext& motion = {},
	             const PointerContext& pointer = {}) = delete;

	/// Puts back the context that was current before this scope.
	~ContextScope() {
		assert(t_current_context == &m_context && "ContextScopes must end in reverse order");
		t_current_context = m_previous;
	}

	ContextScope(const ContextScope&)            = delete;
	ContextScope& operator=(const ContextScope&) = delete;
	ContextScope(ContextScope&&)                 = delete;
	ContextScope& operator=(ContextScope&&)      = delete;

private:
	/// Reads the current context, which nothing else may touch.
	friend const ContextContainer& CurrentContext();

	/// The context of the innermost open scope on this thread, or null when none is open. It is per
	/// thread, so a scope opened on the render thread is never seen by another thread. As an inline
	/// variable it is one variable for the whole program, not one per file that includes this.
	static inline thread_local const ContextContainer* t_current_context = nullptr;

	ContextContainer        m_context;
	const ContextContainer* m_previous;
};

/// The context of the innermost open ContextScope on this thread. Calling it with no scope open
/// is a mistake and asserts. A release build then gets the default theme at its reference size with
/// no fonts, at the clock's starting moment, so nothing reads a context that isn't there.
[[nodiscard]] inline const ContextContainer& CurrentContext() {
	const ContextContainer* const current = ContextScope::t_current_context;
	assert(current != nullptr && "CurrentContext needs an open ContextScope");
	if (current != nullptr) {
		return *current;
	}
	static const ContextContainer FALLBACK {DEFAULT_THEME,
	                                        ScaleContext(DEFAULT_THEME.scale,
	                                                     DEFAULT_THEME.scale.reference_width,
	                                                     DEFAULT_THEME.scale.reference_height),
	                                        {},
	                                        {},
	                                        {},
	                                        {}};
	return FALLBACK;
}

/// The theme of the current context.
[[nodiscard]] inline const ThemeContext& CurrentThemeContext() {
	return CurrentContext().theme;
}

/// The output scale of the current context.
[[nodiscard]] inline const ScaleContext& CurrentScaleContext() {
	return CurrentContext().scale;
}

/// The fonts of the current context.
[[nodiscard]] inline const FontsContext& CurrentFontsContext() {
	return CurrentContext().fonts;
}

/// The clock of the current context, holding the moment everything in this frame animates from.
[[nodiscard]] inline const ClockContext& CurrentClockContext() {
	return CurrentContext().clock;
}

/// The pointer as the current frame read it, with no position when there is none.
[[nodiscard]] inline const PointerContext& CurrentPointerContext() {
	return CurrentContext().pointer;
}

/// How much the player lets the system UI move, in the current context.
[[nodiscard]] inline const MotionContext& CurrentMotionContext() {
	return CurrentContext().motion;
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_CONTEXT_H_
