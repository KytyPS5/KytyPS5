# Layout

This page covers how elements are sized and placed: boxes and their children, text, images and glyphs,
and how design units become pixels. The frame runs a measure pass, then an arrange pass, as
[architecture.md](architecture.md#the-frame-lifecycle) describes. Everything here is in output pixels
once it reaches the frame, and in design units everywhere a caller writes a length.

## Two sets of settings

Every element has two kinds of settings, and they answer different questions.

| Setting | Type | Answers |
|---|---|---|
| `BoxProps::layout` | `BoxLayout` | How this box arranges its own children |
| `Self::layout` | `ItemLayout` | What this element asks of the container it sits in |

`ItemLayout` is the same for every kind, so a box, text, image or glyph all take one. It is:

| Field | Meaning |
|---|---|
| `width`, `height` | An explicit size on that axis. It replaces the element's measurement, border and padding included. |
| `grow` | The element's share of the container's free main-axis space, relative to its siblings. Zero keeps its own size. |
| `min_main`, `max_main` | Limits on its length along the container's main axis. |
| `align_self` | Overrides the container's cross-axis alignment for this element. |
| `collapses` | Gives up all its main-axis length when the container is too short. See below. |

`BoxLayout` is for a box only:

| Field | Meaning |
|---|---|
| `direction` | `FlexDirection::Row` or `Column`. The main axis runs along it. |
| `justify` | `Start`, `Center`, `End` or `SpaceBetween`. Where items sit on the main axis when they don't fill it. |
| `align` | `Start`, `Center`, `End` or `Stretch`. Where items sit on the cross axis. |
| `gap` | Space between neighbouring children on the main axis. |
| `padding` | `std::optional<DpInsets>`. Space between the box's edge and its children. Unset means none. |
| `stacked` | Lays children over one another, each filling the padding box. See below. |

Overflow is not in `BoxLayout`. It is `BoxStyles::overflow`, reached as `self.styles.overflow`, because
it is part of how the box looks and clips.

`padding` is optional on purpose. A component can set its own default, as `Card` does with the theme's
card padding. A caller who sets `DpInsets {}` gets none. A caller who sets nothing keeps the default.

## Measure, then arrange

`Measure(root, max)` sizes a subtree. `max` is the room on offer. Either axis may be infinite, which means
no limit. The result is the size the root wants in output pixels, and it is not clamped to `max`, so a
caller checks it. Each element's measure hook is called with the room its parent gives it.

`Arrange(root, rect)` places the subtree inside `rect`. It uses the sizes from the measure pass and
returns false for a handle that was never measured. A parent places its children with `ArrangeChild`
from inside its own arrange hook.

```cpp
const Vec2 size = Measure(panel, {screen.x, screen.y});
if (Arrange(panel, {0.0f, 0.0f, size.x, size.y})) {
	Paint(panel, list);
}
```

Measuring twice in one pass is cheap, since a child's natural size is kept for the pass. A later pass
may offer different room, so it is measured again.

## How a box gives its children their length

A box sorts out the main axis in this order. The flex solver that does this work is internal to the box
primitive, so callers never invoke it directly.

1. **Each child's least main length.** This is its `min_main` if it sets one. Otherwise it is its
   min-content, which is the least size its content can take without spilling out. A maximum below
   that wins. This is what CSS calls `min-width: auto`.
2. **Its preferred length.** This is the explicit `width` or `height` if set. Otherwise it is the child's
   natural size.
3. **Growing.** Free space is shared among the children with a `grow` factor, in proportion to it. Free
   space goes to growing children first, and `justify` places only what is left over.
4. **Shrinking.** When the children overflow, a child with no explicit main size gives up length in
   proportion to its size, down to its least main length. A child with an explicit main size keeps it, and
   its siblings shrink instead.
5. **Capping.** `max_main` caps a child's growth.
6. **Collapsing.** A child with `collapses` gives up all its length when, after the other children take
   their minimums, the room is still short. Its rectangle is empty. It has no gap beside it, and it takes
   no share of what `justify` spreads. Its subtree is neither arranged nor painted.

Edges are snapped to whole pixels, so neighbours never overlap or leave a hairline gap.

> **Note on shrinking.** The `ItemLayout` comments say that elements never shrink. That is out of date.
> `measure.cpp` gives each child a shrink factor of 1 unless it has an explicit main size or its box
> scrolls. Shrinking is proportional, and it stops at the least main length. The code is the reference
> here. The comments should be fixed, and [contributing.md](contributing.md) lists the places to update.

A row of a fixed-width value and a growing label shows the difference:

```cpp
const AnyElement label =
    Text({.text = "Volume",
          .self = {.layout = {.grow = 1.0f},
                   .styles = {.type = theme.type.body, .color = theme.colors.text.primary}}});
const AnyElement value =
    Text({.text = "70",
          .self = {.layout = {.width = Dp(48.0f)},
                   .styles = {.type  = theme.type.body,
                              .color = theme.colors.text.secondary,
                              .align = TextAlign::End}}});
Box({.layout = {.direction = FlexDirection::Row, .align = Align::Center, .gap = theme.space.s2}},
    {label, value});
```

The label takes the room left after the value's explicit 48 units. The label uses the default
`Ellipsis` overflow, so if the row is narrower it shrinks to its ellipsis and is cut short. The value keeps
its width and does not move.

## Stacked boxes

`BoxLayout::stacked = true` lays every child over the same rectangle, the padding box, in order. On one
layer, a later child is drawn over an earlier one. The box measures as its largest child. Direction,
justify, align, gap, and each child's grow, limits, alignment and collapse are ignored. A stacked box
does not scroll.

Use a stack for the parts of one control that share a rectangle, such as a toggle's track, its knob and
its focus ring.

## Padding, gap and alignment

- `gap` is between children along the main axis only. It is not added at the ends.
- `padding` is inside the box. An explicit `width` or `height` on a box includes its padding.
- `align = Align::Stretch` makes a child as long as the cross axis. A child with an explicit cross size
  is not stretched, so a small icon in a tall row keeps its size.
- A child's `align_self` overrides its container's alignment.

## Scrolling and clipping

`BoxStyles::overflow` decides what happens to children that don't fit:

| Value | Children | Clip |
|---|---|---|
| `Overflow::Visible` | Draw past the box | None |
| `Overflow::Clip` | Drawn, but cut at the box's outer rectangle | The outer rectangle, rounded out to whole pixels |
| `Overflow::Scroll` | Keep their own length along the main axis and never shrink | The padding box, inside the border |

A clip is a rectangle, so content that reaches a rounded corner can show in the corner outside the curve.
Children inside padding of at least `1 - 1/sqrt(2)` of the radius stay inside the curve. `InsideCorners` in
`core/geometry.h` gives the rectangle for that.

A scrolling box is a box with `Overflow::Scroll`. It is found again each frame by its `key`, so give each
scrolling box a key of its own, and the rows of a loop an index:

```cpp
std::vector<AnyElement> rows;  // One per entry, built at run time.
Box({.layout = {.direction = FlexDirection::Column},
     .self   = {.styles = {.overflow = Overflow::Scroll}},
     .key    = Key("save list", slot)},
    rows);
```

Keys are frame wide, so two scrolling boxes with one key in a frame assert in a debug build. A scrolling
box without a key is numbered by build order, which holds while the unkeyed scrollers built before it
don't change in number or order. A component built on a box, such as `Card`, takes `key` in its props and
passes it on, so it can be built anywhere without its boxes scrolling together.

The wheel comes from `CurrentPointerContext()`. A column takes the vertical wheel and a row the
horizontal one. It goes to the innermost scrolling box under the pointer that can move on that axis, and
it doesn't pass on to a box around it. One notch moves three of the theme's body lines, eased over the
base duration, or straight there with reduced motion. A scrolling box never moves past either end of its
content.

## Design units and scale

Every length a caller writes is a `Dp`, a design unit:

```cpp
const Dp    gap    = theme.space.s2;  // 8 design units
const Dp    size   = Dp(16.0f);
const float pixels = ToPixels(size);  // output pixels at the current scale
```

A design unit is one pixel of the 1920 by 1080 reference. The scale turns it into output pixels:

```text
scale = clamp( min(width / 1920, height / 1080) * user_scale, 2/3, 3 )
```

`user_scale` is held between 0.8 and 1.5. Each `Dp` is converted on its own and rounded to a whole pixel.

Three helpers convert between the two:

| Function | Converts |
|---|---|
| `ToPixels(Dp)` | A design length to output pixels, rounded |
| `FromPixels(float)` | Output pixels back to design units, for a length measured in pixels such as text |
| `ToPixels(DpInsets)` | Each side of an inset |

`ScaleContext` also gives the safe area, the part of the output that keeps content clear of the edges
because a TV may crop them. `ScaleOptions::safe_area_percent` sets its inset. The rules of the theme are in
`ScaleRules` in `core/theme.h`.

Plain floats are output pixels, and the draw layer (`imgui/draw/`) takes plain floats. Each declaration
there says whether its values are design units or output pixels.

## Text

A `Text` element holds a copy of its string, so the caller's string need only last for the call. Its
measure is its layout at the room it is given, or at its own width when it sets one.

| Field | Effect |
|---|---|
| `TextStyles::type` | A `TypeStyle`: size, line height, weight, and the smallest size in pixels it may shrink to |
| `TextStyles::overflow` | `Ellipsis` (one line, cut with an ellipsis), `Clip` (one line, cut at the edge) or `Wrap` |
| `TextStyles::max_lines` | For `Wrap`, the most lines. Zero is no limit. The last shown line ends with an ellipsis. |
| `TextStyles::shrink_to_fit` | For one line, shrink the font to 85% before cutting it short |
| `TextStyles::align` | `Start`, `Center` or `End` across each line |

The text's least width, its min-content, depends on how it gives way. Wrapped text (`TextOverflow::Wrap`) can
shrink to its longest run between spaces and line breaks. Ellipsis text can shrink to the width of its
ellipsis, or to its first line if that is narrower. Clipped text has no minimum. Text with few spaces, such
as Japanese, asks for more room in a row than it needs, since ImGui also breaks inside words.

A text's layouts are kept between frames in a slot for its row in the text table. A layout is reused only
when its text and its font, size and width all match. Each slot keeps four layouts, enough for the widths a
wrapped text is measured and arranged at in one frame. A text that needs more widths than that in one frame
re-lays out every frame.

Text is drawn left to right in the order it is given. There is no shaping and no reordering. A script that
needs either must be shaped before it reaches a `Text`.

## Images

An `Image` shows a texture, or an encoded source that an open `ImageCache` decodes.

| Field | Effect |
|---|---|
| `texture` | An `ImTextureData*` the caller keeps alive until the frame is drawn |
| `source` | A `const EncodedImage*`, decoded by the open cache. Null shows nothing. Only read while the element is built. |
| `size` | The image's pixel size. It gives its proportions, and with no explicit size, its own size at the reference resolution. |
| `Self::layout` | An explicit `width` or `height` sets the size on that axis. The other follows the proportions. |
| `ImageStyles::fit` | `Fill` (stretched), `Contain` (whole, with space) or `Cover` (filling, with the excess cropped) |
| `ImageStyles::radius` | Corner radius in design units, never past half the shorter side |

An image never shrinks below its size. A null texture, or a source that can't be shown, still takes its
space and draws nothing. `ImageReady(image)` asks whether an `EncodedImage` can be shown, so a caller can show
something else in its place.

## Glyphs

A glyph is a small drawing made of shapes on a square grid, and it is one element however many shapes it
has. Its shapes are a static table, so a glyph costs no per-shape work at build time.

```cpp
inline constexpr std::array<GlyphShape, 2> ALERT_SHAPES {{
    {.x = 9.0f, .y = 4.0f, .width = 2.0f, .height = 9.0f, .fill = true, .kind = GlyphShapeKind::Rect},
    {.x = 9.0f, .y = 15.0f, .width = 2.0f, .height = 2.0f, .fill = true, .kind = GlyphShapeKind::Circle},
}};

GlyphElement alert = Glyph({.shapes = ALERT_SHAPES,
                            .grid   = 20.0f,
                            .size   = Dp(32.0f),
                            .self   = {.styles = {.color = theme.colors.status.error}}});
```

Glyph coordinates are grid units from the top left. `grid` is how many units make a side, so a glyph
scales with its `size`.

| Shape kind | Draws |
|---|---|
| `Rect` | A rectangle, optionally rounded, filled or outlined |
| `Circle` | A circle. Drawn as a path, since ImGui's own circle keeps a flat pixel on a fully rounded box. |
| `Triangle` | A triangle pointing up, with rounded corners |
| `Stroke` | A line through up to `MAX_GLYPH_STROKE_POINTS` (eight) points, with round ends and joins. One point draws a dot. |

Each shape is drawn in `GlyphInk::Color`, the glyph's colour, or `GlyphInk::Mark`, the mark colour. A mark
over a filled shape uses the mark colour, so it stays visible on a coloured fill.

A glyph is always its square. Its measure is `size` on both axes, its least size is the same, and no
layout setting changes that. `Glyph` clears the limits and collapse of its `ItemLayout` for this reason.

A rectangle lands on the same whole pixels as the same shape laid out as a box, so a glyph can replace a
stack of boxes without changing a pixel. A circle or triangle centres where its grid rectangle centres. Its
size is a pixel smaller when its side and the glyph's thickest stroke differ in parity, so a mark centres in
it exactly.

## Worked examples

### A card with a fixed-width column

```cpp
// title and detail are Texts with TextOverflow::Wrap.
Card({.layout = {.direction = FlexDirection::Column, .align = Align::Stretch, .gap = theme.space.s2},
      .self   = {.layout = {.max_main = Dp(320.0f)}}},
     {title, detail});
```

`max_main` on the card limits its width in the parent row. `Align::Stretch` makes each child as wide as the
card's inner width, so wrapped text fills it. A `Card` keeps `Align::Start` by default, so without the stretch
its children are only as wide as their own content. `Toast` sets `Align::Stretch` on its column for the same
reason.

### A row of icon and wrapped label

```cpp
Box({.layout = {.direction = FlexDirection::Row, .align = Align::Start, .gap = theme.space.s2}},
    {StatusGlyph({.status = Status::Warning, .size = Dp(24.0f)}),
     Text({.text = message,
           .self = {.layout = {.grow = 1.0f},
                    .styles = {.type = theme.type.body_small,
                               .color = theme.colors.text.secondary,
                               .overflow = TextOverflow::Wrap,
                               .max_lines = 3}}})});
```

The glyph keeps its 24-unit square. The text takes the rest of the row, wraps to three lines and cuts the
last one short. Its least width is its longest word, so a long unbroken word is the only thing that can
push the row wider than the room.

### A box that collapses its value when short

```cpp
Text({.text = value_label,
      .self = {.layout = {.min_main = Dp(40.0f), .collapses = true},
               .styles = {.type = theme.type.caption, .color = theme.colors.text.tertiary}}});
```

When the container is too short to leave its other items their minimums, this text gives up its length and
is not drawn. The container's length must be known, so it never collapses while measured with no limit. An
interactive element that is focused when it collapses loses focus, since a collapsed element is not
registered.

### A stacked toggle

```cpp
// The box draws the track through its own styles. Its children draw over it, in order.
Box({.layout = {.padding = DpInsets(theme.space.s1), .stacked = true},
     .self   = {.layout = {.width = Dp(56.0f), .height = Dp(32.0f)},
                .styles = trackStyles}},
    {knob, focus_ring});
```

Each child fills the box's padding box, so the knob and the focus ring are placed against the track's own
rectangle, not against their own measurements.
