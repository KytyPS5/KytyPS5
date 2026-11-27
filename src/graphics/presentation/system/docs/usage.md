# Usage

This page shows how to build UI with the framework: the primitives and their styles, the components
built from them, the theme's tokens, toasts and events, and the gallery. The layout rules are in
[layout.md](layout.md). The frame and the scopes are in [architecture.md](architecture.md).

Every example assumes a `FrameScope` is open, as the builders read the theme and the frame from it.
Every example also uses the `Libs::Graphics::SystemUi` namespace.

## Primitives

There are four primitives. Each builder takes its props as one struct, and returns a typed element.

| Builder | Props | Returns | Children | Keeps state by |
|---|---|---|---|---|
| `Box(props, children)` | `BoxProps` | `BoxElement` | Any | `props.key`, for its scroll position |
| `Text(props)` | `TextProps` | `TextElement` | None | Nothing |
| `Image(props)` | `ImageProps` | `ImageElement` | None | Nothing |
| `Glyph(props)` | `GlyphProps` | `GlyphElement` | None | Nothing |

`Animate(props, child)` is also an element, keeping its progress by `props.key`, covered in [architecture.md](architecture.md#animation).

A `Box` is a container, a `Text` is words, an `Image` is a picture, and a `Glyph` is a drawing made of
shapes. What each element asks of its container and how it looks sit in `self`, as
[CONVENTIONS.md](../CONVENTIONS.md#building-elements) describes.

```cpp
// A box that lays two items out in a row, with the gap and padding a theme gives.
Box({.layout = {.direction = FlexDirection::Row,
                .align     = Align::Center,
                .gap       = theme.space.s2,
                .padding   = DpInsets(theme.space.s3, theme.space.s4)}},
    {first, second});
```

```cpp
// A line of text in the theme's body style and primary colour.
Text({.text = "Resolution",
      .self = {.styles = {.type = theme.type.body, .color = theme.colors.text.primary}}});
```

```cpp
// An image fitted to a square, with its corners rounded by a theme radius.
Image({.texture = icon_texture,
       .size    = {512.0f, 512.0f},
       .self    = {.layout = {.width = Dp(64.0f), .height = Dp(64.0f)},
                   .styles = {.fit = ImageFit::Cover, .radius = theme.radius.sm}}});
```

```cpp
// A glyph drawn from a static table of shapes, in the status colour.
Glyph({.shapes = CHECK_SHAPES,
       .grid   = 20.0f,
       .size   = Dp(24.0f),
       .self   = {.styles = {.color = theme.colors.status.success}}});
```

`Text`, `Image` and `Glyph` have no children. Use a `Box` to group things.

## Passing children

`Box`, `Card` and `Toast` take their children as one `const Children&` parameter, which accepts two forms.

- **A braced list**, for a set you know as you write it. Typed elements go straight in.

  ```cpp
  Box(props, {title});
  Box(props, {icon, label, value});
  ```

- **A run**, such as a `std::vector<AnyElement>` or a `std::span<const AnyElement>`, for a list built at run
  time, such as one entry per row.

  ```cpp
  std::vector<AnyElement> rows;
  for (const Entry& entry: entries) {
  	rows.push_back(Row(entry));
  }
  Box(props, rows);
  ```

A braced list can also mix the two. Each run is spread in order among the single elements. A component
that takes `const Children&` gets both forms with no extra code, and can put its own element in front
of the caller's children without copying them or nesting them in another box:

```cpp
// A titled section, its title first and the caller's children after it, all in one column. Callers
// can write Section(props, {a, b}) or Section(props, rows).
BoxElement Section(const SectionProps& props, const Children& children = {}) {
	return Box({.layout = {.direction = FlexDirection::Column}}, {props.title, children});
}
```

A list in braces is copied into the `Children` list, on the stack up to `INLINE_CHILDREN` (16) handles and on
the heap past that. A run is pointed at with no copy, so it must last for the call, which a temporary
vector does. Always take a `Children` list by const reference. It can't be copied or stored, as the braced
form points into itself. A run of typed elements, such as a `std::vector<TextElement>`, doesn't convert,
so gather runs as `AnyElement`.

Every child must be a live element. A none handle refuses the whole container, as
[architecture.md](architecture.md#elements-and-handles) explains, so leave out what doesn't show rather
than passing a placeholder.

## Styles

Each primitive takes its look in a styles struct.

### BoxStyles

| Field | Meaning |
|---|---|
| `fill`, `border`, `border_width` | The fill colour, the outline colour and its width in design units. A transparent border or zero width draws none. |
| `radius` | The corner radius in design units |
| `layer` | The layer the box and everything inside it are drawn on, 0 the lowest. Within one `Paint`, a higher layer is drawn over every lower one wherever it sits in the tree, keeping its clip and opacity, and a child asking for a lower layer than its box is drawn on its box's. On one layer, tree order. The scroll wheel goes to the scroller on the highest layer, across separate roots too, so overlapping roots that must share it go under one parent. States don't change it. |
| `overflow` | `Overflow::Visible`, `Clip` or `Scroll`, as [layout.md](layout.md#scrolling-and-clipping) describes |
| `states` | A `StateStyles` with `hover`, `focus`, `pressed`, `disabled` and `selected` |
| `shadow` | A `Shadow`, up to three `ShadowLayer`s in CSS box-shadow order, each with `offset_x`, `offset_y`, `blur`, `spread` and a `color` whose alpha is its strength, built with `MakeShadow({...})`. The blur is drawn as CSS draws it, and a layer with no blur draws nothing. Use the theme's `shadows.low`, `medium` or `high`. None by default. A state's `shadow` blends with it layer by layer. |
| `edge` | An `EdgeBand` with a `color` and `width`, drawn along the top edge. Used for a toast's status band. |
| `glow` | A `BoxGlow`, one of the theme's `Glow`s and a `color`, drawn as a soft halo outside the box over its shadow and under its body. It leaves the box itself uncovered, so a translucent fill isn't tinted. None by default. States don't change it. Used around a trophy's icon. |
| `fill_gradient`, `border_gradient` | A `LinearGradient` with `from`, `to` and `angle_degrees`. Used in place of the colour when set. |

A state changes the box while the control it stands for is in that state. `StateInput` gives each
state's weight from 0 to 1, so a highlight can fade in:

```cpp
BoxStyles style {.fill = theme.colors.surface.raised, .radius = theme.radius.md};
style.states.hover.overlay   = theme.colors.state.hover;
style.states.pressed.overlay = theme.colors.state.pressed;
style.states.selected.fill   = theme.colors.accent.tint;
style.states.selected.selection = true;

Box({.self = {.styles = style, .state = {.hover = hover_weight, .selected = is_selected}}}, children);
```

`StateInput` has `hover`, `focus`, `pressed` (each 0 to 1), and `disabled` and `selected` (on or off).
The caller sets it, since the framework doesn't read the mouse for hover or focus. The gallery's sidebar
does this itself. A state with a value moves the box toward it by the state's weight.

> **Not drawn yet.** `StateStyle::scale` is declared and documented, but nothing in `element/` reads it, and
> no test covers it. The header comment that says it shrinks the box describes intended behaviour, not current
> behaviour. See [contributing.md](contributing.md#known-gaps-in-the-code-and-comments).

### TextStyles

| Field | Meaning |
|---|---|
| `type` | A `TypeStyle` from the theme's ramp: `size`, `line`, `weight` and `min_size` |
| `color` | The text colour |
| `align` | `TextAlign::Start`, `Center` or `End` |
| `overflow` | `TextOverflow::Ellipsis`, `Clip` or `Wrap` |
| `max_lines` | For `Wrap`, the most lines. Zero is no limit. |
| `shrink_to_fit` | For one line, shrink before cutting short |

The theme's ramp is in `TypeRamp` (`display`, `title`, `heading`, `body`, `body_small`, `label`,
`caption`, `dense`, `dense_small`). Its styles' weights are `FontWeight::Regular` or `Bold`.

### ImageStyles and GlyphStyles

`ImageStyles` has `fit` (`ImageFit::Fill`, `Contain` or `Cover`) and `radius`.

`GlyphStyles` has `color`, the colour of shapes drawn in colour ink, and `mark`, the colour of shapes
drawn in mark ink.

## Components

A component is a function that builds a fixed arrangement of primitives. Components read the theme,
so their look follows it. Three kinds differ on purpose, as [CONVENTIONS.md](../CONVENTIONS.md#components)
explains.

| Component | Builder | Props | Tokens |
|---|---|---|---|
| Card | `Card(props, children)` | `CardProps` | `CardTokens`, from `CardDefaults(theme)` |
| Toast | `Toast(props, children)` | `ToastProps` | `ToastTokens`, from `ToastDefaults(theme)` |
| Status glyph | `StatusGlyph(props)` | `StatusGlyphProps` | None, the colour is in props |
| Trophy glyph | `TrophyGlyph(props)` | `TrophyGlyphProps` | None, the colour is in props |

### Card

`Card` is a box with the theme's surface: a raised fill, a hairline border, large corners and the theme's `shadows.low`. It holds
its children in the theme's card padding, unless its own layout sets one.

```cpp
Card({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s2}}, {heading, detail});
```

To change one card, patch its styles. The fields you name are laid over `CardDefaults`, and the rest
keep the theme's look:

```cpp
// A card with a smaller radius than the theme's, and the theme's fill and border.
Card({.self = {.styles = {.radius = theme.radius.sm}}}, {heading, detail});
```

`CardTokens` holds `fill`, `border`, `border_width`, `radius`, `padding` and `shadow`. Read them from
`CardDefaults(theme)`, so a page that shows them can't drift from what the card draws.

### Toast

`Toast` is a card with a stronger border and smaller corners, keeping the card's `shadows.low`. It is the surface for
a passing notice over the game. Its width hugs short content between `min_width` and `max_width`, and
its children stretch across that width, so text wraps there.

```cpp
Toast({.status = Status::Info},
      {Text({.text = "Controller 1 connected",
             .self = {.styles = {.type = theme.type.body_small,
                                 .color = theme.colors.text.primary}}})});
```

`ToastProps` has three fields:

| Field | Meaning |
|---|---|
| `status` | `std::optional<Status>`. With a status, the toast has a band of that status's colour along its top edge, and padding below it. Without one it is plain, with no band and no padding. |
| `tokens` | A `ToastTokens` of the fields to change. Zero keeps the theme's value. |
| `self` | `self.layout`, what the toast asks of the stack it sits in |

`ToastTokens` has `min_width`, `max_width`, `padding_horizontal`, `padding_vertical`, `edge_width` and
`slide_distance`. `slide_distance` belongs to the service. It is the distance the service slides a toast
in from, and patching it on one toast changes nothing.

```cpp
// A wider error toast, for a message that needs the room.
Toast({.status = Status::Error, .tokens = {.max_width = Dp(360.0f)}}, {title, detail});
```

### Status glyphs and trophy glyphs

`StatusGlyph` draws the mark for a `Status`: an i in a circle for `Info`, an exclamation mark in a
triangle for `Warning`, an exclamation mark in a circle for `Error`, and a check mark in a circle for
`Success`. Its props are a `size`, a `fill` flag and `self`. Its colour is the status colour unless
`self.styles.color` is set.

```cpp
StatusGlyph({.status = Status::Success, .size = Dp(20.0f), .fill = true});
```

`TrophyGlyph` draws a trophy cup with the same kind of shape table. Its props are `size` and `self`,
and its colour is `self.styles.color`, which the caller sets. A grade's colour is `TrophyGradeColor(theme, grade)`,
which takes a `TrophyGrade`, so a raw game grade goes through `GradeOf(raw)` first. It lives in
`views/trophyUnlocked/trophyUnlocked.h`, a view, so a component can't call it. A component that needs a grade
colour takes it in its props.

```cpp
TrophyGlyph({.size = Dp(32.0f), .self = {.styles = {.color = theme.colors.trophy.gold}}});
```

### Views

Views are whole toasts that other code builds. Each takes a plain props struct.

- `Notification(NotificationProps {status, text})` is a notice: text in the body-small style, on a
  toast with its status, wrapped to three lines.
- `TrophyUnlocked(TrophyUnlockedProps {name, subtitle, grade, icon})` is a trophy toast: the icon beside
  the name, with the subtitle after a cup in the grade's colour. With no icon it shows a tile with a larger
  cup.

Both read the current theme. Building a view with no `FrameScope` open is a mistake that asserts in a debug
build. A release build gets a none handle.

## Themes and tokens

The look is set by three tiers. Each tier is used only from above it, as
[`core/theme.h`](../core/theme.h) describes.

| Tier | Where | Holds | Who reads it |
|---|---|---|---|
| Palette | `Palette::` constants | Raw colours with no meaning | Only the semantic tier |
| Semantic | `ThemeContext::colors`, `space`, `radius`, `border`, `opacity`, `type`, `glow`, `shadows`, `motion`, `scale`, `interaction`, `fonts`, `shine` | Each colour with a role, and the sizes and motion | Components and views |
| Component | `XTokens` from `XDefaults(theme)` | One component's sizes and look | That component, and any page that shows it |

Components read the semantic tier and their own tokens. They never read the palette.

`DEFAULT_THEME` is the only theme. A runtime theme is a copy of it with some fields changed:

```cpp
ThemeContext theme = DEFAULT_THEME;
theme.space.s4 = Dp(20.0f);           // a looser grid step
theme.motion.base = 220.0f;           // a slower base motion, in milliseconds
```

Some facts about the theme:

- Lengths are `Dp`. `theme.space` is a `SpaceScale`, from `hair` to `s9`. `theme.radius` is a
  `RadiusScale` from `xs` to `xl`.
- Colours are per role: `colors.surface.raised`, `colors.text.secondary`, `colors.accent.fill`,
  `colors.status.error`, `colors.border.standard`, `colors.state.hover`, `colors.trophy.gold`, and more.
  `colors.surface` and the other groups alpha-blend over the game, so their alpha is part of the token.
- `motion` holds durations in float milliseconds, such as `base` and `exit_fade`.
- `ThemeContext` is an aggregate and trivially copyable, so a copy is cheap. The theme given to a
  `ContextScope` or `FrameScope` is held by reference, so it must outlive that scope.

### Patch

`Patch(defaults, overrides)` lays the fields you set over the defaults, so a caller changes one field of a
component's look. A plain field set to its empty value keeps the default, so a field that must reach zero
must be `std::optional`. A card can't yet be patched to no border, square corners or a clear fill, and
a banded toast's `edge_width` and paddings can't be patched to zero. The full rules, and the limit of
`MAX_PATCH_FIELDS` fields per struct, are in [`core/patch.h`](../core/patch.h).

## Status

`Status` is `Info`, `Warning`, `Error` or `Success`, in `core/status.h`. That header uses only the standard
library, so the emulator's libraries can use it. `StatusColor(theme, status)` gives its colour, and
`StatusGlyph({.status = status, ...})` its glyph. Colour alone tells statuses apart, so a toast's text must say
what happened too.

## Adding a toast from code

A toast is added with a builder and `ToastOptions`. The builder is called every frame the toast is on
screen, so it must hold plain data, and it must not call the service.

```cpp
#include "graphics/presentation/system/components/toast.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/service.h"

#include <array>
#include <string>
#include <utility>

// Shows a short notice that a pad connected. Holds for four seconds, and merges with a repeat.
uint64_t ShowPadConnected(ToastService& toasts, TimePoint now, std::string name) {
	return toasts.Add(
	    [name = std::move(name)] {
	        const ThemeContext& theme = CurrentThemeContext();
	        return Toast({.status = Status::Info},
	                     {Text({.text = name,
	                            .self = {.styles = {.type  = theme.type.body_small,
	                                                .color = theme.colors.text.primary}}})});
	    },
	    {.key = "pad.connected", .hold_ms = 4000.0f}, now);
}
```

The options are:

| Field | Meaning |
|---|---|
| `key` | A repeat with the same key does not stack. If the toast is on screen, the repeat swaps in its new builder and options, restarts its hold, fades it back in if it was leaving, and keeps its id and place. If it is waiting, the repeat swaps in its builder and options and keeps its place in the queue. The repeat replays no sound. An empty key never merges. |
| `hold_ms` | How long it stays once shown. Infinity stays until dismissed. |
| `sound` | `ToastSound::Unlock` marks it for the trophy sound when it appears |
| `shine` | Sweeps a band of light across it once it has appeared, after the theme's `motion.shine_delay`. Off by default. A trophy turns it on. |

`Add` returns an id. Pass it to `Dismiss(id)` to start the toast leaving. A dismissal of a toast that has
already left does nothing. The id also dismisses a toast that merged into another by key.

The toasts are shown by the instance's service, through `Instance::Toasts()`. The overlay draws them each
frame, so the toast is shown from the next frame drawn after the `Add`.

## Publishing from the emulator

Code outside the system UI publishes events and doesn't need to know about the framework's drawing. It
includes only the event headers.

### A notice

```cpp
#include "graphics/presentation/system/events/notification.h"

PublishNotification("Controller 1 disconnected", Status::Warning);
```

`PublishNotification` takes the text, which must already be in the player's language, and a status. It
can be called from any thread. The notice is dropped if nothing is subscribed, such as while the game
boots or with system notices off. `src/libs/controller.cpp` does this for pad connections and disconnections.

### A trophy

```cpp
#include "graphics/presentation/system/events.h"

// `icon_png` is the game's encoded icon as a span, which may be empty.
void PublishTrophy(std::string_view name, int32_t grade, std::span<const std::byte> icon_png) {
	const auto icon = icon_png.empty() ? nullptr
	                                   : std::make_shared<const std::vector<std::byte>>(
	                                         icon_png.begin(), icon_png.end());
	Libs::Graphics::SystemUi::Publish(Libs::Graphics::SystemUi::TrophyEvent {
	    .name = std::string(name), .grade = grade, .icon_png = icon});
}
```

`grade` is the game's number, 1 for platinum to 4 for bronze. `icon_png` is the encoded image, or null
when the game gave none. The image is shared, so each subscriber's copy doesn't copy the bytes. The
overlay's `NotifyTrophyUnlocked` does this, and it only publishes when system notices are on. Otherwise it
uses its older trophy popup.

### A new kind of event

Adding an event type takes these steps.

1. Declare the struct in `events/`. Keep it standard library only if an emulator library publishes it.
2. Include its header from `events.h`, the umbrella that publishers include, so every publisher gets the type
   the same way.
3. Give it a publish helper, as `PublishNotification` does, if the caller should not name the type.
4. Write a consumer in `views/` that derives from `EventConsumer<YourEvent>`. Its `OnEvent` adds toasts or
   does whatever the event needs.
5. Create the consumer in `Instance`'s constructor, next to the trophy and notification consumers. This step
   edits `Instance`, so it is not a change that stays out of shared code.

Each event type has its own channel, found by its type, so the new channel doesn't change how the others
deliver.

## Using the gallery

The gallery is a window that shows the design system without a game. It is the place to check a component
or a token visually before it reaches the overlay.

Build and run it as [contributing.md](contributing.md#the-gallery) describes. The sidebar groups its pages as below.
The Card page reads its token values from `CardDefaults`, so the numbers it shows are the ones the card draws.
A new page should do the same, as [contributing.md](contributing.md#adding-a-gallery-page) explains.

| Group | Pages |
|---|---|
| Foundations | Colours, Typography, Elevations, Spacing, Theme |
| Motion | Animations, Speed, Timing |
| Primitives | Box, Image |
| Components | Card, Toast, Glyphs |

The Toast page is the one live page. While it shows, the gallery publishes test events on a steady rhythm,
and every fourth is a trophy with an icon. Its toasts sit at the top right, as over a game, and the sidebar's
reduced-motion switch applies to them.
