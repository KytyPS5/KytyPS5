# Architecture

This page explains how the framework works at runtime: what happens in a frame, what an element is,
where state lives between frames, and how events reach toasts on screen. The rules behind these
choices are in [CONVENTIONS.md](../CONVENTIONS.md). This page links to them rather than repeating them.

## The frame lifecycle

A frame is one pass that builds, sizes, places and paints a tree. Every stage works on the same
`FrameBuilder`, which holds the tree of the current frame.

```text
  frame.Begin()                        clears elements, interned text, per-frame workspaces
      |
  FrameScope (frame, theme, scale, fonts, clock, motion, pointer)
      |
  builders: Box, Text, Image, Glyph, Animate, Card, Toast ...
      |                                  returns AnyElement handles, nothing is sized yet
  Measure(root, max)                   sizes go back up from the leaves; each kind's MeasureX hook
      |                                  Vec2 in output pixels, max may be infinite on an axis
  Arrange(root, rect)                  places go down from the root; each kind's ArrangeX hook
      |                                  sets each layer from LayerX, at least its parent's
      |                                  returns false for a handle that was never measured
  Paint(root, ImDrawList&)             pre-order: InheritX, PaintX, clip, children, PaintXOver
                                         each drawn into its layer's channel, merged lowest first
```

Three rules follow from this order.

1. Handles are only valid for the frame that made them. After `Begin`, an old handle is stale, and
   every builder and query treats it as none.
2. `Arrange` needs a `Measure` in the same frame. The root is laid out whatever its parent decided,
   since only a parent collapses a child.
3. `Paint` needs an arrange. It draws what layout left, and it never changes the tree.

`Measure` starts a new measure pass. A size measured in one pass is reused only within it, since a
later pass may offer different space.

## Elements and handles

An element is a node of the frame's tree plus a row of data in a table for its kind. A kind is the
props type of a primitive: `BoxProps`, `TextProps`, `ImageProps`, `GlyphProps`, and `AnimateData` for
`Animate`. Components are built from these and add no kinds of their own.

The public handles are in `element/element.h`:

| Type | Meaning |
|---|---|
| `AnyElement` | An element of any kind. Lists of children hold these. |
| `Element<Props>` | An element whose kind is known, returned by a builder. Converts to `AnyElement`. |
| `BoxElement`, `TextElement`, `ImageElement`, `GlyphElement` | Aliases for `Element<XProps>`. Components and views return the alias of the primitive they are built on. |
| `AnimateElement` | `Element<AnimateData>`, typed on what the frame stores for an `Animate`. |
| `ElementList<N>` | A `std::array` of `N` `AnyElement`s, for a list of children whose length is fixed where it is built. |

A handle is a node index plus the serial of the tree that made it. A default-constructed handle is
none: `IsNone()` is true. A none handle passed as a child is not skipped. It makes the container's whole
child claim fail, so the container is refused as `None` and draws nothing. Build the child list to hold
only what shows, as [usage.md](usage.md#passing-children) describes.

A builder returns none, and does not throw, when the frame can't build the element. The cases are
listed in `element/refusal.h` as `ElementError`:

| Reason | When |
|---|---|
| `None` | A none child was passed. The container is refused for this reason. Not logged. |
| `Stale` | The handle was made in an earlier frame |
| `OutOfRange` | The handle names a node the frame doesn't have |
| `AlreadyParented` | A child that already has a parent, or is listed twice |
| `NotBegun` | The frame hasn't had `Begin` called |
| `DuplicateKey` | Reserved. See the note on keys below |

A refusal is logged once per run for each reason, so a refused tree that repeats every frame doesn't
flood the log. To learn why something came back as none, open a `RefusalScope` around the builder call:

```cpp
const RefusalScope refusals;
const BoxElement box = Box(props, children);
if (box.IsNone()) {
	// refusals.First() says the first reason, which is the cause.
}
```

`RefusalScope::First()` returns the first refusal and not the last. A refused element becomes none, and
every parent built on it is then refused as `None`, which says nothing about the cause.

### Kinds and their frame hooks

Each kind gives the frame a `FrameHooks` struct. Generic layout and paint call the hooks through the
element's kind, so neither needs a list of kinds. A primitive sets its hooks on every call to its
builder, so the frame it is built in always has them.

| Kind | Builder | Children | Hooks it sets |
|---|---|---|---|
| `BoxProps` | `Box` | any number | measure, arrange, paint, inherit, clip, paint_over, layer |
| `TextProps` | `Text` | none | measure, arrange, paint |
| `ImageProps` | `Image` | none | measure, paint |
| `GlyphProps` | `Glyph` | none | measure, paint |
| `AnimateData` | `Animate` | exactly one | measure, arrange, inherit, paint_over, layer |

Hooks are named after their stage: `MeasureX`, `ArrangeX`, `PaintX`, `PaintXOver`, `InheritX`,
`ClipX` and `LayerX`. `inherit` gives the element its own paint state, such as an opacity, which its
children inherit. `clip` says whether children are cut to a rectangle. `paint_over` draws after the
children, outside their clip, as a scroll indicator does. `layer` is the layer the element asks for.
Arrange raises it to its parent's and keeps it in the element's layout, and Paint draws each element
into a channel for its layer, merging the channels from the lowest, so a higher layer is drawn over a
lower one across the tree while the clip and opacity still come down it in tree order. An Animate asks
for its child's layer.

Layout records for each element are kept beside the tree in `FrameView::ElementLayout`. These
include the size and rectangle, the least size the content can take (`min_content`), and whether the
element was collapsed. Only element kinds' hooks read them, including `Animate`'s. Components see the results
only through the elements they build. The gallery's sidebar is the one exception, which reads the rows' and
toggle's rectangles to hit-test the pointer (see [contributing.md](contributing.md#the-gallery)).

### Keys

`FrameView::ElementMetadata` has key fields for naming siblings. Nothing in the framework sets them
yet, so `has_key` is always false and the `DuplicateKey` refusal can't be reached. Don't build on keys
until a builder sets them. Use the place a builder is called from, and an animation or scroll `key`,
for identity instead (see below).

## Workspaces and state across frames

The frame is rebuilt every frame, so anything that must last is held by the frame in a workspace. A
workspace is one value of a type, made the first time it is asked for. The builder owns it, so it
lives as long as the `FrameBuilder`.

| Facility | Kept? | Use |
|---|---|---|
| `Workspace<T>()` | Yes, for the life of the builder | Per-element state found by key: `FadeTracker`, `SlideTracker`, `ShineTracker`, `TweenTracker`, `ScrollTracker`, the box layout's working storage `BoxLayoutWorkspace`, and `PaintLayers`, the splitter and channel table a layered Paint draws through. Also `TextCache`, which is keyed by text row, not key (see the note below). |
| `FrameWorkspace<T>()` | Emptied by every `Begin` | Per-frame records: `ScrollFrame` for the scrolling boxes of this frame, `AnimateFrame` for numbering the unkeyed Animates of this frame, `ArrangeLayer` for the layer being arranged, `TextLaidOutRows` for the text layouts used this frame |
| `Table<T>()` | Emptied by `Begin`, capacity kept | The data rows of one kind of element |
| `ElementStack()` | Emptied by `Begin` | Runs of children, built without a vector per frame |
| `Intern`, `InternCString`, `StoreBytes` | Cleared by `Begin` | Text and bytes a builder must keep until the frame is drawn |

A kept workspace whose type owns memory is expected to keep its capacity, so the frame's own storage
allocates nothing in a steady frame. Use `ElementStack()` as a stack: note its size, push, use indices
and resize back.

`TextCache` is different from the animation and scroll records. Its slots are indexed by the text's row in
the frame's text table, which follows build order. `Text` has no key, so a text's layout is
found again by that row and then matched on its text, font, size and width. Layouts shift when the tree
changes shape, which costs a re-layout but never a wrong one. [CONVENTIONS.md](../CONVENTIONS.md) says
state is keyed by key or build order, which overstates this (see the Known gaps in contributing.md).

### Identity: keys

Animations and scrolling boxes need to be found again next frame. Each finds its record by an
identity made from a `Key key` in its props, from `core/key.h`.

- A `Key` is hashed as it is made, from a name (`"pane"`), a name and an index (`Key("row", i)`), or
  another key and a part (`key.With("scroll")`). Keys are frame wide, like ids on a web page, so two
  elements that keep state need two keys.
- **A scrolling box**'s identity is `key.With("scroll")`, and **an Animate**'s is `key.With("animate")`,
  which leaves the key free for any other state kept for it later.
- An element of either kind without a key is numbered by build order, the nth unkeyed scroller, or the
  nth unkeyed Animate, built this frame. That holds while the number and order of unkeyed ones before it
  stay the same, so key anything that comes and goes, as each toast is keyed by its id.
- A component built on a box, such as `Card`, takes a `key` in its props and passes it on. No builder
  takes the place in the source it was called from.

Both kinds of record are kept in an `AnimationRecords`, one for each. It keeps a record while it is
visited in the current frame or the one before. A record that goes a whole frame unvisited is forgotten,
so an animation that disappears and comes back starts again. It asserts if the same animation is visited
twice in one frame.

## Scopes and contexts

Builders don't take the theme, scale or frame as parameters. They read the innermost open scope on the
thread. Each scope ends in the reverse order it began, and asserts that it does.

| Scope | Gives | Opened by |
|---|---|---|
| `ContextScope` | `CurrentContext()`, and `CurrentThemeContext()`, `CurrentScaleContext()`, `CurrentFontsContext()`, `CurrentClockContext()`, `CurrentMotionContext()`, `CurrentPointerContext()` | The owner, or a `FrameScope` |
| `FrameScope` | `CurrentFrame()` (for components) and `CurrentBuilder()` (for the element layer) | The owner, once per frame |
| `ImageCacheScope` | `CurrentImageCache()` | The owner, around anything that shows images |
| `RefusalScope` | Records the first refusal made on the thread | Whoever wants to know why a build failed |

`FrameScope` also opens a `ContextScope`, so one scope sets up everything a frame needs. Nested
scopes are how a preview at another size works: it opens its own `ContextScope` inside the page's,
and the page's context comes back afterwards.

The theme is held by reference, so it must outlive the scope. The scale, fonts, clock, motion and pointer
are copied in. The `ContextScope` constructor rejects a temporary theme at compile time.

## Layers

Each layer depends only on the layers before it. `core/` is the base, and `element/` builds and draws the
tree without knowing about toasts or events. In `animation/`, only `animate.*` builds elements. `events/`
uses only the standard library, so the emulator's libraries can publish without linking the drawing code.
`components/`, `service/` and `views/` come last. The full order is in
[CONVENTIONS.md](../CONVENTIONS.md#layers).

## Animation

An animation is a timeline plus a start time. Every pose is worked out from absolute times, from the
clock the frame was given, never by adding up frame deltas. A stall of several seconds during a shader
compile therefore never freezes or skips an animation. It resumes at the right point on the next frame.

The clock is read once per frame into a `ClockContext`, so everything drawn in one frame animates from
the same moment. Value types and services take a `TimePoint` directly.

### The pieces

| Type | What it does |
|---|---|
| `Timeline` | A `delay_ms`, an `enter` `Track`, a `hold_ms` (infinite by default) and an `exit` `Track`. A `Track` is a `duration_ms` and an `Ease`. |
| `Evaluate(timeline, start, dismissed, now)` | The `Pose` at `now`: a `Phase` (`Waiting`, `Entering`, `Shown`, `Exiting`, `Gone`) and an `alpha` from 0 to 1. |
| `Fade` | Shown or hidden, with an optional `timeline`. Without one it uses `ThemeFade`. |
| `Slide` | Enters `x` and `y` design units away from its place, then stays. Uses `ThemeSlide` without a timeline. |
| `Tween` | Eases toward a `target` between 0 and 1. Uses `ThemeTween` without `tracks`. |
| `Shine` | Sweeps a band of light across once from the first build, or stands at a held `position`. Uses `ThemeShine` without a `timeline`. |
| `TweenValue`, `ScrollValue` | Value types that move toward a target and can turn around mid-move without jumping. |
| `ApplyEase(Ease, t)` | The theme's easing curve, from `Linear`, `Enter`, `Exit`, `Standard` and `Emphasis`. |

### Animate

`Animate(props, child)` wraps one child, its children. It lays out as the child would, asking its container
for what the child asks, and keeps that space while the child fades or slides. The slide moves the
child's rectangle, not the Animate's, so the content around it doesn't move.

`AnimateProps` holds four optional animations and a key. Each is independent, so one `Animate` can
carry any mix. A `Shine` sweeps a band of light across the child once, from the Animate's first build,
and the Animate draws it over the child once the child has painted, rounded as the child box is, or
square when the child isn't a box. A shine with a held `position` draws its band standing still
instead.

```cpp
const AnimateElement fading = Animate({.fade = Fade {.shown = is_open}, .key = Key("row", row_index)}, row);
if (AnimateNeedsFrames(fading)) {
	// The UI must keep drawing frames while the fade plays.
}
```

Four queries read the result of the current frame. Each needs the `FrameScope` the element was built
in.

- `FadePose(animate)` tells when a fade has finished, so a caller can stop building a fading element.
- `TweenAmount(animate)` is the tween's value before clamping, so an overshoot shows.
- `ShinePosition(animate)` is where the shine's band stands across the child, or none when it draws no
  band, before or after its sweep.
- `AnimateNeedsFrames(animate)` tells the owner to keep drawing frames.

### Reduced motion

`MotionContext::reduced` comes from the player's settings. With it set, a fade is no longer than the
theme's `reduced_fade_max`, a slide has no timeline, so the element appears in place, and a shine
doesn't sweep, though a held one still shows. A tween's easing is unchanged, since it moves nothing the
player sees as motion.

## Events

Events carry data from the emulator to whoever shows it. The emulator publishes from any thread, and
the system UI handles each event on the thread that draws.

```text
  publisher (any thread)           channel per type           consumer (drawing thread)
  Publish(TrophyEvent)  --->  EventChannel<TrophyEvent>  --->  EventConsumer::Consume(now)
                              each subscriber has a         drains its own mailbox, calls
                              bounded mailbox, drops        OnEvent for each event, in order
                              the oldest when full
```

- `Channel<T>()` is the program's one channel for each event type, made the first time it is asked for.
- `Publish<T>(event)` copies the event into each subscriber's mailbox and returns. It never calls a
  subscriber's code. Publishing with no subscriber is a lock and nothing else.
- `Subscribe<T>(capacity)` returns an `EventSubscription`. Destroying it stops delivery. Its
  `Dropped()` count says how many events it lost to a full mailbox.
- `EventConsumer<E>` subscribes in its constructor, so it only sees events published after it is made.
  Each frame, `Consume` drains its subscription and calls `OnEvent` for each event in order.

Two headers keep this light. `events/publish.h` is all a publisher needs. `events/notification.h` and
`events/trophy.h` use only the standard library, so the emulator's libraries can publish without
depending on ImGui.

The event types are:

| Type | Fields | Published by |
|---|---|---|
| `NotificationEvent` | `text`, `status` | `PublishNotification(text, status)`, from `src/libs/controller.cpp` for pad connections |
| `TrophyEvent` | `name`, `grade` (1 platinum to 4 bronze), `icon_png` | `NotifyTrophyUnlocked` in `src/graphics/presentation/systemOverlay.cpp` |

## The toast service

`ToastService` holds the toasts on screen and those waiting for room. It is the only thing that draws
them. It never relies on a scope its caller opened. Each frame it does this, in order:

1. Asks each consumer to handle its waiting events at the frame's time. Consumers may add toasts.
2. Applies the inbox. Adds and dismissals made from any thread since the last frame are applied in the
   order made.
3. Ends the holds that are over. A toast whose hold ends starts leaving.
4. Notes the toasts that came on screen, which `Draw` returns, so the caller can play a sound once.
5. Builds every toast on screen from its builder, stacked at the top right below whatever sits above.
   Each toast is an `Animate` with a fade and a slide, so it fades in and slides in from the right. A toast
   whose builder is missing, or whose builder refuses, gives a none child. That refuses its `Animate` and then
   the whole stack, so one bad toast hides every toast that frame. See the Known gaps in contributing.md.
6. Forgets each toast whose fade out has finished.

Adding a toast takes a builder and `ToastOptions`:

```cpp
const uint64_t id = toasts.Add(build, {.key = "pad.1", .hold_ms = 4000.0f}, now);
toasts.Dismiss(id);
```

| Option | Meaning |
|---|---|
| `key` | A repeat with the same key does not stack. If the toast is on screen, the repeat swaps in its new builder and options, restarts its hold, fades it back in if it was leaving, and keeps its id and place. If it is waiting, the repeat swaps in its builder and options and keeps its place in the queue. The repeat replays no sound, since the toast doesn't come on screen again. An empty key never merges. |
| `hold_ms` | How long the toast stays once shown. An infinite hold stays until dismissed. |
| `sound` | `ToastSound::Unlock` marks the toast for the trophy sound when it comes on screen. |
| `shine` | Sweeps a band of light across the toast once it has come on screen, after the theme's `motion.shine_delay`, so it crosses a toast at rest, for a moment worth marking. Off by default, and the trophy consumer turns it on. |

`ToastQueue` holds the state. It shows at most `DEFAULT_VISIBLE_TOASTS` (three) toasts, and queues
the rest. It keeps at most `MAX_WAITING_TOASTS` (sixteen) waiting, dropping the oldest when a burst
goes over, since stale toasts would otherwise queue for minutes.

The builder is a `std::function<BoxElement()>`. The service calls it every frame a toast is drawn, so
the builder must hold plain data, not a reference to anything that may be gone. It runs with the
service's lock held, so it must not call into the service at all.

## Instance and the overlay

`Instance` sets the system UI up once for its owner. It owns the `ToastService`, the two consumers that
turn events into toasts, the fonts and the image cache.

```cpp
// Shaped like the options InitializeSystemUiOnce builds. The values are the overlay's own.
const InstanceOptions options {
    .fonts  = {.assets_folder = assets_folder, .merge_fallbacks = merge_fallbacks},
    .images = {.capacity = TOAST_IMAGE_CAPACITY},
    .trophy = {.subtitle = trophy_subtitle, .hold_ms = trophy_hold_ms},
};
Instance system_ui(DEFAULT_THEME, options);
```

The construction call comes from `InitializeSystemUiOnce` in `systemOverlay.cpp`. It runs at the first
vblank after the game's first frame, with no splash showing, and only when system notices are on. Its
consumers therefore see only what is published after that, so an unlock during boot is never shown.

| Member | Thread | Notes |
|---|---|---|
| `Toasts()` | Any for adding; drawing thread for `Draw` | Returns the `ToastService&` |
| `Fonts()` | Drawing thread, with an ImGui context current | Loads the theme's fonts into the atlas the first time |
| `Images()` | Drawing thread, with an ImGui context current | The first call makes the `ImageCache` and also loads the fonts, so it needs the context |
| `ReleaseRendering()` | Drawing thread, after the renderer shuts down | Drops fonts and images so the context can go |

`Fonts()` and `Images()` are made lazily because the fonts need an ImGui context, which may not exist
when the instance is made. `ReleaseRendering` must run after the renderer backend shuts down and
before the ImGui context is destroyed. `systemOverlay.cpp` does exactly that in its shutdown path.

The overlay draws each frame from `DrawToasts`:

```cpp
images.BeginFrame();
const auto& appeared = system_ui->Toasts().Draw(
    {.list   = *ImGui::GetForegroundDrawList(),
     .area   = {.width = io.DisplaySize.x, .height = io.DisplaySize.y, .top = top},
     .theme  = DEFAULT_THEME,
     .fonts  = system_ui->Fonts(),
     .images = &images,
     .clock  = ClockContext {now},
     .motion = {.reduced = Config::ReducedMotionEnabled()}});
```

It also asks `NeedsFrames()` of the service and the image cache, so the overlay keeps drawing while a
toast or an icon is still alive.

## Threading and lifetime

Each declaration says which thread may call it when that isn't only the drawing thread. The rules as a
whole:

| Call | Thread |
|---|---|
| `Publish<T>`, `PublishNotification` | Any |
| `ToastService::AddConsumer`, `NeedsFrames` | Any |
| `ToastService::Add`, `Dismiss`, `OnScreen`, `Waiting` | Any, but not from a builder |
| `ToastService::Draw`, and every builder, measure, arrange and paint | The drawing thread |
| `Instance::Fonts`, `Images`, `ReleaseRendering`, `ImageStore`, `ImageCache` | The drawing thread |

The frame, its scopes and its workspaces belong to one thread. The `ContextScope` and `FrameScope` pointers
are `thread_local`, so a scope opened on one thread is never seen by another. The lifetime and thread rules
are in [CONVENTIONS.md](../CONVENTIONS.md#lifetimes-and-threads). Two of them matter most here: glyph tables
must be static, and text interned in a frame lasts only until the next `Begin`.

## Errors

The framework never throws. A refused build is a none handle. Bad outside data, such as an image that
won't decode, is logged and gives a null or a clamped value. A programmer error is an `assert`. This is
a deliberate exception to the emulator's rule on exceptions, as [CONVENTIONS.md](../CONVENTIONS.md#errors)
explains.
