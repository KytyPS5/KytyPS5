# System UI framework

The system UI framework is the immediate-mode UI library behind the emulator's system overlay:
toasts, notices and trophy unlocks drawn over the game. It is also the library behind the system UI
gallery, a window that shows the design system without a game running.

The code lives in `src/graphics/presentation/system/`. It draws through Dear ImGui's draw lists. It
does not use ImGui widgets, so nothing in it is keyed by ImGui ids.

These docs explain how the pieces fit and how to use them. The rules for writing the code are in
[CONVENTIONS.md](CONVENTIONS.md), which these docs link to rather than repeat.

## What it is

The framework is immediate mode: builder calls return handles to the current frame, state lives in the
frame, colours and sizes come from a theme, and nothing throws. The rules and their reasons are in
[CONVENTIONS.md](CONVENTIONS.md#philosophy). What matters first is that a build that can't happen
returns a none handle, and a none child refuses its whole container.

## A 60-second example

This builds a small card with two lines of text and draws it. It is the whole cycle: a builder
function, then a frame that measures, arranges and paints.

```cpp
#include "graphics/presentation/system/components/card.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"

#include "imgui.h"

#include <array>

using namespace Libs::Graphics::SystemUi;

// Builds the panel. Runs with a FrameScope open, and returns a handle, never throws.
AnyElement BuildSavedPanel() {
	const ThemeContext& theme = CurrentThemeContext();
	return Card({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s2}},
	            {Text({.text = "Saved",
	                   .self = {.styles = {.type  = theme.type.heading,
	                                       .color = theme.colors.text.primary}}}),
	             Text({.text = "Your settings are stored.",
	                   .self = {.styles = {.type  = theme.type.body_small,
	                                       .color = theme.colors.text.secondary}}})});
}

// Draws one frame of the panel into `list`, in a window of `screen` pixels.
void DrawSavedPanel(FrameBuilder& frame, ImDrawList& list, const FontsContext& fonts, Vec2 screen) {
	frame.Begin();
	const ScaleContext scale(DEFAULT_THEME.scale, screen.x, screen.y);
	const FrameScope   scope(frame, DEFAULT_THEME, scale, fonts, ClockContext::Now());
	const AnyElement   panel = BuildSavedPanel();
	const Vec2         size  = Measure(panel, screen);
	if (Arrange(panel, {0.0f, 0.0f, size.x, size.y})) {
		Paint(panel, list);
	}
}
```

What each step does:

1. `frame.Begin()` clears the last frame's elements and interned text. Everything built before it is stale.
2. `FrameScope` makes the frame, theme, scale, fonts, clock, motion and pointer current for the builders.
3. The builders return handles. Nothing is measured yet.
4. `Measure` sizes the tree in output pixels, offering `screen` as the room.
5. `Arrange` places it. It returns false if the tree was never measured.
6. `Paint` writes the draw commands into `list`.

The owner of the frame (here, the caller) includes `element/frame/frameBuilder.h`. Code that only
builds primitives includes the umbrella `element.h`, which sits at the root of `system/`. Components are
not in the umbrella: `Card` comes from `components/card.h`, and each component has its own header.

## Terms

| Term | Meaning |
|---|---|
| Frame | One pass of build, measure, arrange and paint. The frame owns the tree and its state. |
| Element | One node of the tree, built by a builder. Its kind is its props type. |
| Handle | What a builder returns: `AnyElement` or a typed `Element<Props>`. It is valid only in the frame that made it. |
| None handle | A handle that refers to nothing. A builder returns one when it refuses. Passing one as a child refuses the whole container, so the container draws nothing. |
| Scope | A thread-local object that makes a context current for the builders, such as `FrameScope`. Scopes end in reverse order. |
| Workspace | A value a frame keeps across frames. A scroll or animation record is found by its element's key, or without one by its build order among unkeyed elements of its kind. Text layouts are kept by their row in the text table. |
| Design units | `Dp` lengths, which are reference pixels at 1920 by 1080. The scale turns them into output pixels. |
| Self | The nested struct in each props type that holds what the element asks of its container and how it looks. |
| Token | A named value from the theme, such as `theme.space.s2`, read by a component rather than hard-coded. |

## Folder map

Each folder is one layer. A layer may include the layers before it and never one after it. The numbers
match [CONVENTIONS.md](CONVENTIONS.md#layers).

| Folder | Layer | What it holds |
|---|---|---|
| `core/` | 1 | Theme tokens, `Dp` lengths, `ScaleContext`, the context scopes, `Status`, geometry, `Patch`, the clock |
| `common/props/` | 2 | Props shared by primitives: `ItemLayout`, `Align`, `Justify`, `FlexDirection`, text and image styles |
| `imgui/` | 3 | Draw helpers on `ImDrawList`, text layout, fonts, `ImageStore` and `ImageCache` |
| `element/` | 4 | The node tree, `FrameBuilder`, layout, paint, refusals, and the primitives `Box`, `Text`, `Image`, `Glyph` |
| `animation/` | 4 and 5 | Timelines, animation records, fade, slide, tween and scroll values, easing. `animate.*` (5) builds `Animate` elements |
| `events/` | 6 | Event channels, `Publish` and `Subscribe`, consumers, `NotificationEvent`, `TrophyEvent` |
| `components/` | 7 | Reusable parts built from primitives: `Card`, `Toast`, `StatusGlyph`, `TrophyGlyph` |
| `service/` | 8 | `ToastService`, which holds, times and draws toasts; `ToastQueue`; `ToastOptions` |
| `views/` | 9 | Whole toasts and their consumers: `Notification`, `TrophyUnlocked` |
| `instance.*` | 10 | `Instance`, which sets the system UI up for its owner |
| `gallery/` | 10 | The standalone gallery window. It uses only the framework |
| `docs/` | | The guides listed in the reading guide |

The umbrella headers serve one audience each:

| Header | For |
|---|---|
| `core.h` | Code that styles and sizes: colours, lengths, geometry, the theme, the `Current*Context` accessors and the clock, keys, statuses, gradients |
| `element.h` | Code that builds elements: every primitive, `FrameScope`, `Measure`, `Arrange`, `Paint`, refusals |
| `animation.h` | Code that animates: `Animate`, the value types, timelines, easing |
| `events.h` | Publishers outside the system UI: `Publish` and every event type |
| `service.h` | Code that owns or draws toasts: `ToastService` |
| `instance.h` | The owner that sets the system UI up |

## Reading guide

1. [architecture.md](docs/architecture.md) covers the frame lifecycle, elements and handles, state across
   frames, scopes, animation, events, the toast service, `Instance`, and threading.
2. [layout.md](docs/layout.md) covers how boxes, text, images and glyphs are sized and placed, with worked
   examples.
3. [usage.md](docs/usage.md) covers the primitives, styles, components, themes and tokens, and how to add a
   toast or publish an event from the emulator.
4. [contributing.md](docs/contributing.md) covers building, running the tests and the gallery, adding a
   primitive, component or gallery page, testing expectations, and commits.
5. [CONVENTIONS.md](CONVENTIONS.md) holds the rules. Read it before you write code.

## What is not here yet

- **No input routing.** A caller sets `StateInput` (hover, focus, pressed, disabled, selected) on each
  box. The gallery maps the mouse to it. The frame reads the pointer only for scrolling boxes, which
  use its position and wheel.
- **No text shaping.** Text is drawn left to right in the order given. Scripts that need shaping must
  be shaped before they reach the framework.
- **State scale is not drawn.** `StateStyle` has a `scale` field, but nothing in `element/` reads it
  yet. [usage.md](docs/usage.md#styles) and
  [contributing.md](docs/contributing.md#known-gaps-in-the-code-and-comments) say the same.
