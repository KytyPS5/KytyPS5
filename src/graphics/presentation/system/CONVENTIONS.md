# System UI conventions

How the system UI framework is written, exposed and used. New code follows these, and code that
doesn't is brought in line rather than copied.

## Philosophy

- **Immediate mode.** The UI is built again every frame by calling builder functions. A builder returns
  a handle to an element in the current frame. Callers keep no element objects between frames.
- **The frame owns state.** Anything that must last between frames, such as animation, scrolling or text
  layout, lives in the frame's workspaces. A box or an animation keeps its state by its `Key`, or without
  one by its build order among unkeyed elements of its kind. The frame allocates nothing in a steady
  frame. A braced list of more than `INLINE_CHILDREN` children once its runs are spread, and runs that
  callers build in their own vectors, may.
- **Compose from primitives.** Components are composed from boxes, text and images. Glyphs are the one
  exception. A glyph is a set of shapes on a grid, held in a static table and drawn as one element.
- **Tokens from the theme.** Colours, spacing, radii, type and motion come from the theme. A literal is a
  named constant with a comment saying why it isn't a token.
- **Contracts in comments.** Every public declaration says what it needs (an open scope, a thread) and
  what it promises (lifetime, what happens when it can't be built).

## Building elements

- A primitive or component is a builder named after what it builds. It takes nothing, its props, its
  children, or its props and then its children, as `XElement X(const XProps& props, const Children&
  children = {})`, and nothing else. Anything else it needs, such as a status or an image, is a field of
  its props. `Animate` holds exactly one child, so it takes it as `AnyElement child` in place of
  `children`.
- An element whose state the frame keeps, as a box that scrolls or an animation does, finds it by a
  `Key key` in its props. A component built on such an element takes a `Key key` and passes it on, as
  `Card` does. A component that keeps no state, such as `Toast`, has no key.
- A primitive returns a typed `Element<XProps>` with an `XElement` alias. `AnimateElement` is the one
  exception, typed on `AnimateData`, which is what the frame stores for it. A component or view returns
  the element of the primitive it is built on, `BoxElement` for `Card` and `Toast` and `GlyphElement`
  for `StatusGlyph`. Every element converts to `AnyElement`, which is what containers take.
- **Props shape.** Fields about what the element holds or arranges sit at the top of `XProps`. What the
  element asks of its own container and how it looks sit in a nested `Self`:
  - `self.layout` is always an `ItemLayout` (size, grow, alignment, collapsing).
  - `self.styles` is the element's look, of type `XStyles` (`BoxStyles`, `TextStyles`, `ImageStyles`,
    `GlyphStyles`, `StatusGlyphStyles`, `TrophyGlyphStyles`). A component built on a box uses
    `BoxStyles`, as `Card` does, and one whose look is fixed has no `styles`, such as `Toast`.
  - A box's own `layout` field, of type `BoxLayout`, is how it arranges its children. It is not
    `self.layout`, which is what the box asks of its parent.
  - A glyph's `size` is its content, so it sits at the top level, and it sizes the glyph in place of
    `self.layout`.
  - `Animate` has no `Self`, as it lays out exactly as its child does.
- **Unset values.** A field that a caller may set to zero on purpose is a `std::optional`. A plain field
  where zero means unset is only for values that can never meaningfully be zero.
- **Initialisers.** Props are written with designated initialisers, which must name fields in
  declaration order. A new field may go anywhere in a struct, as initialisers don't depend on position,
  but existing fields are never reordered.
- **Children.** A builder that holds children takes them once, as `const Children&`, and gets two
  forms. A fixed set can be listed in place, as in `Box(props, {title, body})`, and a set worked out at
  run time is passed as a run, such as a `std::vector<AnyElement>`. A braced list can mix the two, as in
  `Box(props, {title, rows})`, and each run is spread in order among the single elements. A component
  passes its `children` on as they are, or as one piece of a braced list. A `Children` list is always taken
  by const reference and never copied or stored, as the braced form points into itself. Pass a list
  straight to the builder rather than binding a local reference to it, as a temporary run inside it
  ends with the statement. Callers that build a local `std::array` still work. A child left out is not
  a placeholder. Build the list to hold only what shows.

## Components

- A component's look comes from its tokens. Each component other than a glyph component declares an
  `XTokens` struct and `XTokens XDefaults(const ThemeContext& theme)` in its header, built from the
  theme's semantic tokens.
- Unless it is one of the kinds below, a caller changes a component's look through `XProps::tokens`, a
  struct of the fields to change, which the builder lays over `XDefaults` with `Patch`. `Patch` handles
  at most `MAX_PATCH_FIELDS` fields and counts a nested struct as one, so tokens with more fields than
  that are grouped into nested structs.
- Three kinds of component differ, on purpose. A card is a box with a look, so a caller patches its
  `self.styles`, laid over the styles built from `CardDefaults`, and it has no `tokens`. A toast is a
  fixed variant of the card, so its surface comes from the card and the theme, and only its sizes are
  tokens. A glyph component takes its colour in its props and has no tokens, as its shapes are its whole
  look.
- `Patch` keeps the default wherever a plain field is left at zero, so a plain field can't be patched
  back to zero. A card can't be patched to no border, square corners or a clear fill, and a banded
  toast's `edge_width` and paddings can't be patched to zero. A field that must reach zero becomes a
  `std::optional` when something needs it.
- A view takes no `tokens`. A view that has tokens, such as the trophy toast, declares `XTokens` and
  `XDefaults` named after its builder so the gallery can show its sizes.
- Anything shown elsewhere about a component, such as a gallery page listing its sizes, reads them from
  `XDefaults`, so the two can't drift.
- Public builders read the theme from the open scope with `CurrentThemeContext()`. Pure functions that
  compute from the theme, such as `XDefaults` or `StatusColor`, take `const ThemeContext&`, and so may
  private helpers in a `.cpp` that build part of one element.
- Status is one enum, `Status {Info, Warning, Error, Success}`, in `core/status.h`. Its colour comes from
  `StatusColor(theme, status)` and its glyph from `StatusGlyph({.status = status, ...})`. A toast takes
  `std::optional<Status>`, and one without a status has the plain look, with no band.

## Layers

Each layer may include the layers before it and never one after it.

1. `core/` holds values, the theme, scale, contexts, status, keys, gradients, `Patch` and encoded
   images.
2. `common/` holds props shared by primitives.
3. `imgui/` holds drawing helpers, conversions to ImGui types, fonts, and the image cache and store.
4. `element/` holds the frame and the primitives. Everything in `animation/` except `animate.*` sits
   beside it, as scrolling uses those pieces and none of them builds elements.
5. `animation/animate.*` holds `Animate`, the one animation piece that builds elements.
6. `events/` holds the channels and every event type.
7. `components/` holds reusable parts built from primitives.
8. `service/` holds the toast service, which draws components.
9. `views/` holds whole toasts and screens, and the consumers that turn events into them.
10. `instance` sets the system UI up, and `gallery/` only uses the framework.

## Gallery

The gallery is a tool for looking at the framework, not part of it. Its helpers may take the theme and
positional arguments, return `AnyElement` and be named for what they show, such as `BuildBoxPage`. It
still follows the layers, the umbrella headers, naming and the documentation rules. A length it hands
to a primitive is a `constexpr Dp`, and one its own helpers take as a float, such as a column width,
stays a plain float in design units.

## Context, time and dependencies

- **Scopes for builders.** Code that builds elements reads its context from thread-local scopes, each
  in effect for as long as it lives.
  - `ContextScope` gives `CurrentContext()` and `CurrentThemeContext()`, `CurrentScaleContext()`,
    `CurrentFontsContext()`, `CurrentClockContext()`, `CurrentMotionContext()` and
    `CurrentPointerContext()`.
  - `FrameScope` gives `CurrentFrame()` and `CurrentBuilder()`, and opens a `ContextScope` of its own.
  - `ImageCacheScope` gives `CurrentImageCache()`.
  - `RefusalScope` records the first reason `RefuseBuild` sends while it is open.
  - `ArrangeClipScope` is not thread-local. The box primitive opens one around arranging a box that
    clips, and it pushes that clip onto the frame's stack of arrange clips, so the wheel finds a
    scrolling box only where it shows.
- **Hooks.** Frame hooks use the `FrameBuilder&` they are given, never `CurrentBuilder()`, and may read
  the `ContextScope` values. Functions that only compute take what they need as parameters.
- **Services.** A service, such as `ToastService`, is handed everything it uses, when it is made or each
  frame in a plain struct of contexts and pointers, and opens the scopes it needs itself. It never relies
  on a scope its caller opened.
- **Events are global by design.** Code outside the system UI publishes to a typed channel without
  holding a reference to it.
- **Time.** Builders read the ambient clock with `CurrentClockContext()`. Value types and services take
  a `TimePoint now`, and a service's per-frame struct carries a `ClockContext` for the scope it opens.
  `TimePoint` lives in `core/clock.h` and isn't redeclared in each class. Animation always works from
  absolute times, never from accumulated frame deltas.
- **Moments.** An effect that plays once as an element arrives, such as a shine, is an `Animate`, timed
  from the Animate's first build. A toast waiting in its queue is first built when it is shown, so its
  moment starts then, and nothing has to be handed the time it appeared.

## Errors

The framework never throws. This is a deliberate exception to the project's rule of explicit
exceptions, as UI drawn every frame must never take down the game.

- An element that can't be built is a none handle, an empty handle that every builder and container
  accepts and draws nothing for. The reason is logged once per run, except `ElementError::None`, and
  recorded in the innermost open `RefusalScope`, if there is one.
- A programmer error, such as scopes ending out of order, is an `assert`.
- Bad data from outside gives a null, an empty `std::optional`, a clamped value or a fallback. A failure
  that happens once, such as an image that won't decode or a font that won't load, is logged. Inputs
  read every frame, such as window sizes and settings, and enum values from the game, such as a trophy
  grade, are clamped or mapped silently, so the log isn't flooded.

## Lifetimes and threads

- What the frame keeps but doesn't own, such as glyph shapes and stroke points, must have static
  storage, or be kept alive past the frame by its owner, as `ImageStore` keeps image textures. Types
  that point at static data take it only through a `consteval` constructor, so pointing at the stack
  doesn't compile.
- Scopes end in the reverse order they began, and every scope asserts it.
- An owner that others hold a reference back to (`Instance`, `ToastService`, consumers, caches) is
  neither copyable nor movable, and says so with all four deleted operations, or inherits them from a
  base that does, as the event consumers do.
- Every declaration says which thread may call it when that isn't only the drawing thread.

## Units

- Public lengths are `Dp`, design units, which equal reference pixels at the theme's reference size.
  The two names mean the same thing, and the docs say "design units". Type sizes and line heights are
  lengths too. A length is written `Dp(16.0f)`, the one spelling used everywhere, and a named length is
  a `constexpr Dp`.
- Values in output pixels stay plain floats and say so, such as the theme's smallest type size and
  smallest targets, its reference size and the toast area. So do glyph grid units.
- The draw layer (`imgui/draw/`) takes plain floats, apart from the text and image styles it is handed
  whole. Each declaration says whether its floats are design units or output pixels.
- Durations are float milliseconds. In a struct of durations the fields need no suffix. A duration among
  other kinds of values ends in `_ms`.

## Naming

- Files are camelCase. Header guards are `EMULATOR_SRC_<PATH>_<FILE>_H_` in upper snake case, with a
  camelCase file name split at its capitals, so `core/encodedImage.h` is guarded by
  `EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_ENCODED_IMAGE_H_`. Everything is in
  `namespace Libs::Graphics::SystemUi`, or a namespace nested in it such as `Gallery`. The umbrella
  headers, the gallery's `main` and forward declarations of ImGui types sit outside it.
- Types, functions and methods are PascalCase. The one exception is `clear()` and `empty()` on frame
  workspaces, which the frame calls by the names the standard containers use. Constants are
  UPPER_SNAKE_CASE, with qualifiers first (`DEFAULT_`, `MAX_`, `MIN_`).
- Public fields of structs are snake_case, the frame's internal structs included, so
  `Measured::minContent` is `min_content`. Private members of classes are `m_snake_case`. Thread-local
  statics are `t_snake_case`, and other statics at namespace scope are `g_snake_case`. Other statics
  inside a function are plain snake_case.
- A method never shares the name of a type.
- A predicate answering "must the caller keep drawing frames" is `NeedsFrames()` or `XNeedsFrames(...)`.
  Words like `Moving` and `HasPending` keep their literal meanings.
- `XContext` is a plain bundle of values, built once and then only read. Owners of cached data are
  `XCache`, owners of loaded data `XStore` and owners that run a feature `XService`. Records an element
  keeps across frames are held by an `XTracker`. Scopes are `XScope`. Other owners are named for what
  they are, such as `FrameBuilder` or `ToastQueue`. Setup structs are `XOptions`.

## Headers

- A primitive's public header declares its props, its element alias and its builder.
- An element kind's hooks are named `MeasureX`, `ArrangeX`, `PaintX`, `PaintXOver`, `InheritX`,
  `ClipX` and `LayerX`, gathered in a constexpr `X_FRAME_HOOKS` that its builder sets on every call.
  A paint hook draws only into the list it is given and never leaves it split or changes its
  channel, as Paint draws layers through channels of its own and switches between them around
  each hook.
- The hooks are declared in the primitive's `layout.h` and `paint.h`, whose comment says they are
  internal to the primitive, and defined in `layout.cpp` and `paint.cpp`. A primitive with large hooks
  splits them further by phase, as the box does in `measure.cpp` and `arrange.cpp`. A primitive whose
  hooks are short may keep them in its `.cpp` instead, as the glyph does.
- Helpers a header must show but callers shouldn't use go in an `XDetail` namespace. Helpers in a
  `.cpp` go in an anonymous namespace.
- Each umbrella header serves one audience and includes all of it:
  - `core.h` for code that styles and sizes, with the value types, the theme, the contexts and the
    clock, keys, statuses and gradients.
  - `element.h` for code that builds elements, with every primitive, `FrameScope`, `Measure`,
    `Arrange`, `Paint` and `RefusalScope`.
  - `animation.h` for code that animates, with `Animate`, the value types, timelines and easing.
  - `events.h` for publishers outside the system UI, with publishing and every event type.
  - `service.h` for code that owns or draws the toasts.
  - `instance.h` for the owner that sets the system UI up. It declares `Instance` and brings what its
    options name.
- Components, views, the gallery and the toast service include umbrella headers rather than primitive
  or core headers. Core headers outside `core.h` are included explicitly where they are used, even
  when `core.h` brings them in. Subscribers inside the system UI include `events/consumer.h` and the
  event headers they consume, as no umbrella serves them.

## Documentation

- Every declaration in a header has a `///` comment, types included. A group of related constants may
  share one `///` comment above them. Deleted operations and overrides of a documented virtual need
  none. Fields and code in `.cpp` files use `//`, function-pointer fields included.
- Comments are full sentences that say why, avoiding colons and semicolons.
- `[[nodiscard]]` goes on every function whose result must be used, such as builders, queries, handles
  and anything that hands over ownership. Ids that callers may ignore don't need it.
