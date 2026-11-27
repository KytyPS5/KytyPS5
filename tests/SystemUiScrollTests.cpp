// Checks scrolling in the system UI, starting with the pointer each frame is
// given, which is how wheel input reaches the boxes that scroll, and the keys
// a scrolling box keeps its position by.
#include "graphics/presentation/system/animation/easing.h"
#include "graphics/presentation/system/animation/scroll/scrollValue.h"
#include "graphics/presentation/system/components/card.h"
#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/core/key.h"
#include "graphics/presentation/system/core/pointer.h"
#include "graphics/presentation/system/core/scale.h"
#include "graphics/presentation/system/core/theme.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/frameLayout.h"
#include "graphics/presentation/system/element/frame/framePaint.h"
#include "graphics/presentation/system/element/frame/frameScope.h"
#include "graphics/presentation/system/element/primitives/box/box.h"
#include "graphics/presentation/system/element/primitives/box/scroll.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace Libs::Graphics::SystemUi;

// The full screen clip the test draw lists start under, larger than anything
// the tests place, so a clip equal to it means nothing narrowed it.
constexpr float FULLSCREEN_HALF = 8192.0f;
constexpr Rect FULLSCREEN{-FULLSCREEN_HALF, -FULLSCREEN_HALF,
                          2.0f * FULLSCREEN_HALF, 2.0f * FULLSCREEN_HALF};

// The length of a row in the scrollers below, in design units, which are
// output pixels at the reference size the tests draw at.
constexpr float ROW = 20.0f;

// The padding of the scrollers whose least length is checked.
constexpr float PAD = 5.0f;

// The room offered across a scroller and to anything measured without a
// tight limit, more than any test box needs.
constexpr float ROOM = 500.0f;

// How wide a column is, and how tall a row is, across their main axis.
constexpr float THICKNESS = 100.0f;

// Where the boxes whose clips are checked are placed, away from the origin so
// a clip that ignored the position would differ.
constexpr Vec2 ORIGIN{10.0f, 20.0f};

// How finely the test draw lists tessellate curves, as ImGui does by default.
constexpr float TESSELLATION = 0.30f;

// How wide the borders whose clips are checked are, in design units.
constexpr float BORDER = 2.0f;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "SystemUiScrollTests: failed: %s\n", message);
    std::abort();
  }
}

// A frame drawn `size` times the reference size, so a design unit is `size`
// output pixels, and by default exactly one.
struct TestFrame {
  const ThemeContext &theme = DEFAULT_THEME;
  ScaleContext scale;
  FrameBuilder frame;
  std::optional<FrameScope> scope;

  explicit TestFrame(float size = 1.0f)
      : scale(theme.scale, theme.scale.reference_width * size,
              theme.scale.reference_height * size) {
    frame.Begin();
    scope.emplace(frame, theme, scale, FontsContext{}, ClockContext{});
  }
};

// A row that is ROW long when it has room, from its padding, and may give all
// of it up, as it sets no least length, so a container that shrinks its
// children shrinks it.
AnyElement ShrinkableRow() {
  return Box({.layout = {.padding = DpInsets(Dp(ROW / 2.0f))},
              .self = {.layout = {.min_main = Dp(0.0f)}}});
}

// A row that is always ROW long, as it sets its height.
AnyElement FixedRow() {
  return Box({.self = {.layout = {.height = Dp(ROW)},
                       .styles = {.fill = Color{255, 0, 0, 255}}}});
}

// A column THICKNESS wide with `overflow` around `rows`.
BoxElement Column(Overflow overflow, std::span<const AnyElement> rows,
                  std::optional<DpInsets> padding = std::nullopt) {
  return Box(
      {.layout = {.direction = FlexDirection::Column, .padding = padding},
       .self = {.layout = {.width = Dp(THICKNESS)},
                .styles = {.overflow = overflow}}},
      rows);
}

// How long each of three shrinkable rows is in a column with `overflow` two
// rows long, and where the last one starts.
struct RowLengths {
  std::array<float, 3> heights{};
  float last_top = 0.0f;
};

// Lays out three shrinkable rows in a column with `overflow` two rows long and
// reads back their lengths.
RowLengths ShrinkableLengths(Overflow overflow) {
  TestFrame test;
  const std::array<AnyElement, 3> rows{ShrinkableRow(), ShrinkableRow(),
                                       ShrinkableRow()};
  const BoxElement column = Column(overflow, rows);
  (void)Measure(column, {ROOM, 2.0f * ROW});
  Check(Arrange(column, {0.0f, 0.0f, THICKNESS, 2.0f * ROW}),
        "the column is placed");
  RowLengths lengths;
  for (size_t index = 0; index < rows.size(); ++index) {
    lengths.heights[index] = test.frame.LayoutOf(rows[index])->arranged.height;
  }
  lengths.last_top = test.frame.LayoutOf(rows[2])->arranged.y;
  return lengths;
}

// A scroller's rows keep their own lengths when they don't fit, and those
// past its end are placed past it, where a column that doesn't scroll shrinks
// the same rows to fit.
void TestScrollKeepsLengths() {
  const RowLengths scrolled = ShrinkableLengths(Overflow::Scroll);
  Check(scrolled.heights == std::array<float, 3>{ROW, ROW, ROW},
        "a scroller's rows keep their own lengths");
  Check(scrolled.last_top == 2.0f * ROW,
        "and the row that doesn't fit starts at the scroller's end");
  const RowLengths visible = ShrinkableLengths(Overflow::Visible);
  Check(visible.heights[0] < ROW && visible.heights[2] < ROW,
        "while a column that doesn't scroll shrinks them to fit");
}

// How long a column with `overflow` around three fixed rows measures in
// `room` along its main axis.
float FixedColumnLength(Overflow overflow, float room) {
  TestFrame test;
  const std::array<AnyElement, 3> rows{FixedRow(), FixedRow(), FixedRow()};
  return Measure(Column(overflow, rows), {ROOM, room}).y;
}

// A scroller is only as long as the room it has, and with unlimited room it
// is as long as its content. A column that doesn't scroll measures as its
// rows, as they can't shrink.
void TestScrollBoxLength() {
  Check(FixedColumnLength(Overflow::Scroll, 2.0f * ROW) == 2.0f * ROW,
        "a scroller is as long as its room");
  Check(FixedColumnLength(Overflow::Scroll, FULLSCREEN.height) == 3.0f * ROW,
        "and as long as its content when the room is longer");
  Check(FixedColumnLength(Overflow::Visible, 2.0f * ROW) == 3.0f * ROW,
        "while a column that doesn't scroll is as long as its rows");
}

// A scroller needs only its padding along its main axis, as what doesn't fit
// scrolls, and the same across as a column that doesn't scroll.
void TestScrollMinContent() {
  const auto min_content = [](Overflow overflow) {
    TestFrame test;
    const std::array<AnyElement, 3> rows{FixedRow(), FixedRow(), FixedRow()};
    const BoxElement column = Column(overflow, rows, DpInsets(Dp(PAD)));
    (void)Measure(column, {ROOM, ROOM});
    return test.frame.LayoutOf(column)->min_content;
  };
  const Vec2 scrolled = min_content(Overflow::Scroll);
  const Vec2 visible = min_content(Overflow::Visible);
  Check(scrolled.y == 2.0f * PAD,
        "a scroller's least length is its padding alone");
  Check(visible.y == 3.0f * ROW + 2.0f * PAD && scrolled.x == visible.x,
        "where a column that doesn't scroll needs its rows, and both need the "
        "same width");
}

// How long a column with `overflow` around three fixed rows is placed when
// it sits under a fixed header in a container two rows and the header long.
float LengthUnderHeader(Overflow overflow) {
  TestFrame test;
  const std::array<AnyElement, 3> rows{FixedRow(), FixedRow(), FixedRow()};
  const BoxElement column = Column(overflow, rows);
  const std::array<AnyElement, 2> parts{FixedRow(), column};
  const BoxElement container =
      Box({.layout = {.direction = FlexDirection::Column}}, parts);
  (void)Measure(container, {ROOM, 3.0f * ROW});
  Check(Arrange(container, {0.0f, 0.0f, ROOM, 3.0f * ROW}),
        "the container is placed");
  return test.frame.LayoutOf(column)->arranged.height;
}

// A container shortens a scroller to the room left beside its siblings, so it
// scrolls rather than running past the container, as a column that doesn't
// scroll does.
void TestScrollerShortened() {
  Check(LengthUnderHeader(Overflow::Scroll) == 2.0f * ROW,
        "a container shortens a scroller to the room it has left");
  Check(LengthUnderHeader(Overflow::Visible) == 3.0f * ROW,
        "while a column that doesn't scroll keeps its rows' length");
}

// Whether the row that collapses, after two fixed rows in a column with
// `overflow` two rows long, is collapsed.
bool CollapsesRow(Overflow overflow) {
  TestFrame test;
  const AnyElement collapsing =
      Box({.layout = {.padding = DpInsets(Dp(ROW / 2.0f))},
           .self = {.layout = {.collapses = true}}});
  const std::array<AnyElement, 3> rows{FixedRow(), FixedRow(), collapsing};
  const BoxElement column = Column(overflow, rows);
  (void)Measure(column, {ROOM, 2.0f * ROW});
  Check(Arrange(column, {0.0f, 0.0f, THICKNESS, 2.0f * ROW}),
        "the column is placed");
  return test.frame.LayoutOf(collapsing)->collapsed;
}

// A scroller never collapses a row that would collapse in a column that is
// too short, as the scroller shows it by scrolling.
void TestScrollKeepsCollapsingRow() {
  Check(!CollapsesRow(Overflow::Scroll),
        "a scroller keeps a row that collapses");
  Check(CollapsesRow(Overflow::Visible),
        "which a column that doesn't scroll collapses");
}

// The clip of every draw command that drew something when `root` is painted,
// in drawing order.
std::vector<ImVec4> PaintedClips(AnyElement root) {
  ImDrawListSharedData shared;
  shared.ClipRectFullscreen = ImVec4(FULLSCREEN.x, FULLSCREEN.y,
                                     FULLSCREEN.Right(), FULLSCREEN.Bottom());
  shared.SetCircleTessellationMaxError(TESSELLATION);
  ImDrawList list(&shared);
  list._ResetForNewFrame();
  list.PushClipRectFullScreen();
  list.PushTexture(ImTextureRef());
  Paint(root, list);
  std::vector<ImVec4> clips;
  for (const ImDrawCmd &command : list.CmdBuffer) {
    if (command.ElemCount > 0) {
      clips.push_back(command.ClipRect);
    }
  }
  return clips;
}

// Whether `clip` is exactly `rect`.
bool ClipIs(const ImVec4 &clip, const Rect &rect) {
  return clip.x == rect.x && clip.y == rect.y && clip.z == rect.Right() &&
         clip.w == rect.Bottom();
}

// A box whose clip is checked, with a filled child. Its style and the state
// its state styles respond to, and the size it is drawn at, as for TestFrame.
struct ClipCase {
  BoxStyles styles;
  StateInput state = {};
  float size = 1.0f;
  bool stacked = false;
};

// Where a ClipCase's box is placed, how wide a BORDER wide border draws at its
// size, and the clip its child is drawn under.
struct BorderClip {
  Rect box;
  float border = 0.0f;
  ImVec4 child;
};

// Lays out and paints a ClipCase's box at ORIGIN and reads back its child's
// clip. The box is a column THICKNESS square.
BorderClip ChildClip(const ClipCase &given) {
  TestFrame test(given.size);
  const std::array<AnyElement, 1> rows{FixedRow()};
  const BoxElement box = Box(
      {.layout = {.direction = FlexDirection::Column, .stacked = given.stacked},
       .self = {.layout = {.width = Dp(THICKNESS), .height = Dp(THICKNESS)},
                .styles = given.styles,
                .state = given.state}},
      rows);
  const Vec2 size = Measure(box, {ROOM * given.size, ROOM * given.size});
  Check(Arrange(box, {ORIGIN.x, ORIGIN.y, size.x, size.y}),
        "the box is placed");
  const std::vector<ImVec4> clips = PaintedClips(box);
  // The child draws last, after the box's border when it has one.
  Check(!clips.empty(), "the child draws");
  return {test.frame.LayoutOf(box)->arranged, test.scale.Border(BORDER),
          clips.back()};
}

// Whether a ClipCase's child is clipped inside its box's border.
bool ClipsInsideBorder(const ClipCase &given) {
  const BorderClip drawn = ChildClip(given);
  return ClipIs(drawn.child, drawn.box.Inset(EdgeInsets(drawn.border)));
}

// Whether a ClipCase's child is clipped to its box's outer edge.
bool ClipsToEdge(const ClipCase &given) {
  const BorderClip drawn = ChildClip(given);
  return ClipIs(drawn.child, drawn.box);
}

// A style with `overflow` and a BORDER wide border in `border`.
BoxStyles Bordered(Overflow overflow, Color border) {
  return {.border = border, .border_width = Dp(BORDER), .overflow = overflow};
}

// A scroller clips its children inside the border it draws, at the width it
// draws it, so they pass under the border. It is drawn at twice the reference
// size, where that width differs from the border's design units. A box that
// clips without scrolling clips them to its outer rectangle, and a scroller
// whose border is clear doesn't draw one, so it takes no room.
void TestScrollClipsInsideBorder() {
  constexpr float DOUBLE = 2.0f;
  const Color seen{255, 255, 255, 255};
  const BorderClip doubled =
      ChildClip({.styles = Bordered(Overflow::Scroll, seen), .size = DOUBLE});
  Check(doubled.border != BORDER,
        "the border draws wider than its design units at twice the size");
  Check(ClipIs(doubled.child, doubled.box.Inset(EdgeInsets(doubled.border))),
        "a scroller clips its children inside its border");
  Check(ClipsToEdge({.styles = Bordered(Overflow::Clip, seen), .size = DOUBLE}),
        "where a box that clips without scrolling clips to its outer edge");
  Check(ClipsToEdge({.styles = Bordered(Overflow::Scroll, Color{})}),
        "and a scroller with no border drawn clips to its outer edge");
}

// A scroller with a border thinner than a pixel clips inside the one pixel the
// border still draws, as a drawn border is never thinner than that, though its
// width alone rounds to no pixels at all.
void TestScrollClipsInsideHairline() {
  constexpr float HAIRLINE = 0.25f;
  constexpr float LEAST_BORDER_PIXELS = 1.0f;
  TestFrame scale_check;
  Check(scale_check.scale.Px(HAIRLINE) == 0.0f,
        "the hairline's width alone rounds to no pixels");
  const BorderClip drawn =
      ChildClip({.styles = {.border = Color{255, 255, 255, 255},
                            .border_width = Dp(HAIRLINE),
                            .overflow = Overflow::Scroll}});
  Check(ClipIs(drawn.child, drawn.box.Inset(EdgeInsets(LEAST_BORDER_PIXELS))),
        "a scroller clips inside the one pixel a hairline border draws");
}

// A scroller whose border only a state draws clips inside that border while
// the state is on, and to its outer edge while it is off, so the clip follows
// the border the box draws.
void TestScrollClipFollowsState() {
  BoxStyles style{.overflow = Overflow::Scroll};
  style.states.focus = {.border = Color{255, 255, 255, 255},
                        .border_width = Dp(BORDER)};
  Check(ClipsInsideBorder({.styles = style, .state = {.focus = 1.0f}}),
        "a focused scroller clips inside the border focus draws");
  Check(ClipsToEdge({.styles = style, .state = {.focus = 0.0f}}),
        "and to its outer edge while it isn't focused");
}

// A stacked box with Scroll clips its children inside its border, as a
// scroller does, where the same stacked box that doesn't clip leaves them
// under the full screen clip.
void TestStackedScrollClips() {
  const Color seen{255, 255, 255, 255};
  Check(ClipsInsideBorder(
            {.styles = Bordered(Overflow::Scroll, seen), .stacked = true}),
        "a stacked scroller clips inside its border");
  const BorderClip visible =
      ChildClip({.styles = Bordered(Overflow::Visible, seen), .stacked = true});
  Check(ClipIs(visible.child, FULLSCREEN),
        "while a stacked box that doesn't clip leaves its child unclipped");
}

// How long a row with `overflow` around three fixed cells measures in `room`
// along its main axis, and its least length there.
struct RowMeasure {
  float length = 0.0f;
  float min_content = 0.0f;
};

// Measures such a row, padded by PAD.
RowMeasure MeasureRow(Overflow overflow, float room) {
  TestFrame test;
  const auto cell = [] {
    return Box({.self = {.layout = {.width = Dp(ROW)}}});
  };
  const std::array<AnyElement, 3> cells{cell(), cell(), cell()};
  const BoxElement row = Box({.layout = {.direction = FlexDirection::Row,
                                         .padding = DpInsets(Dp(PAD))},
                              .self = {.layout = {.height = Dp(THICKNESS)},
                                       .styles = {.overflow = overflow}}},
                             cells);
  const float length = Measure(row, {room, ROOM}).x;
  return {length, test.frame.LayoutOf(row)->min_content.x};
}

// A row scroller is as long as its room and needs only its padding along it,
// as a column scroller does, so scrolling follows the main axis whichever way
// it runs. A row that doesn't scroll is as long as its cells.
void TestRowScroller() {
  constexpr float SHORT = 2.0f * ROW;
  const RowMeasure scrolled = MeasureRow(Overflow::Scroll, SHORT);
  const RowMeasure visible = MeasureRow(Overflow::Visible, SHORT);
  Check(scrolled.length == SHORT, "a row scroller is as long as its room");
  Check(scrolled.min_content == 2.0f * PAD,
        "and needs only its padding along it");
  Check(visible.length == 3.0f * ROW + 2.0f * PAD &&
            visible.min_content == visible.length,
        "while a row that doesn't scroll needs its cells");
}

// How tall a row that grows is, beside a fixed row in a column with
// `overflow` four rows long, which has room for both.
float GrownLength(Overflow overflow) {
  TestFrame test;
  const AnyElement growing =
      Box({.layout = {.padding = DpInsets(Dp(ROW / 2.0f))},
           .self = {.layout = {.grow = 1.0f}}});
  const std::array<AnyElement, 2> rows{FixedRow(), growing};
  const BoxElement column = Column(overflow, rows);
  (void)Measure(column, {ROOM, 4.0f * ROW});
  Check(Arrange(column, {0.0f, 0.0f, THICKNESS, 4.0f * ROW}),
        "the column is placed");
  return test.frame.LayoutOf(growing)->arranged.height;
}

// A scroller whose rows fit still lets a row grow into the room left, as a
// column that doesn't scroll does.
void TestScrollerGrowsWhenFits() {
  const float scrolled = GrownLength(Overflow::Scroll);
  Check(scrolled == 3.0f * ROW, "a row in a scroller grows into the room left");
  Check(GrownLength(Overflow::Visible) == scrolled,
        "as much as in a column that doesn't scroll");
}

// The pointer a frame or context scope is given is the one its elements read,
// and a scope given none, nested inside one that has a pointer, has no pointer
// and no wheel, so input never leaks into a frame that wasn't given it.
void TestPointerContext() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  const PointerContext given{.position = Vec2{10.0f, 20.0f},
                             .wheel = Vec2{0.0f, -1.0f}};
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {}, {}, given);
  Check(CurrentPointerContext() == given,
        "a frame's elements read the pointer it was given");
  {
    FrameBuilder inner;
    inner.Begin();
    const FrameScope nested(inner, theme, scale, {}, {});
    Check(!CurrentPointerContext().position.has_value() &&
              CurrentPointerContext().wheel == Vec2{},
          "a frame given none has no pointer and no wheel");
  }
  // A pointer apart from the frame's, so only the context scope can make it
  // current.
  const PointerContext other{.position = Vec2{30.0f, 40.0f},
                             .wheel = Vec2{1.0f, 0.0f}};
  {
    const ContextScope context(theme, scale, {}, {}, {}, other);
    Check(CurrentPointerContext() == other,
          "a context scope makes the pointer it was given current too");
  }
  Check(CurrentPointerContext() == given,
        "and the frame's pointer comes back when it ends");
}

// How long the scroll moves below take, in milliseconds. It, DISTANCE and the
// moments the tests read are chosen so every value is exact in binary floating
// point, which is what lets the tests compare values exactly.
constexpr float SCROLL_MS = 200.0f;

// How far the scroll moves below go, in pixels, far past 1 so a value held to
// 0 to 1 as a tween's is would show.
constexpr float DISTANCE = 300.0f;

// A linear track SCROLL_MS long, so the value at any moment is easy to work
// out by hand.
constexpr Track LINEAR{SCROLL_MS, Ease::Linear};

// The moment `ms` milliseconds after a fixed start.
TimePoint At(float ms) {
  return TimePoint{} + std::chrono::duration_cast<TimePoint::duration>(
                           std::chrono::duration<float, std::milli>(ms));
}

// A scroll value rests where it starts, moves toward a target over its track
// to exactly the target, however far past 1 that is, and is moving only until
// it gets there.
void TestScrollValueMoves() {
  ScrollValue value(DISTANCE / 2.0f);
  Check(value.Value(At(0.0f)) == DISTANCE / 2.0f && !value.Moving(At(0.0f)),
        "a scroll value rests where it starts");
  value.Retarget(DISTANCE, LINEAR, At(0.0f));
  Check(value.Value(At(SCROLL_MS / 2.0f)) == DISTANCE * 3.0f / 4.0f &&
            value.Moving(At(SCROLL_MS / 2.0f)),
        "it is half way there half way through its track, and moving");
  Check(value.Value(At(SCROLL_MS)) == DISTANCE && !value.Moving(At(SCROLL_MS)),
        "and exactly at its target, past 1, at the track's end, resting");
  Check(value.Target() == DISTANCE, "it reports where it was sent");
}

// A scroll value follows its track's ease. A quarter of the way through a
// Standard track it has moved less than a quarter of the way, where a linear
// track moves exactly a quarter.
void TestScrollValueEases() {
  const float quarter = SCROLL_MS / 4.0f;
  ScrollValue linear;
  linear.Retarget(DISTANCE, LINEAR, At(0.0f));
  ScrollValue standard;
  standard.Retarget(DISTANCE, {SCROLL_MS, Ease::Standard}, At(0.0f));
  Check(linear.Value(At(quarter)) == DISTANCE / 4.0f,
        "a linear scroll is a quarter of the way after a quarter of its track");
  Check(standard.Value(At(quarter)) ==
            DISTANCE * ApplyEase(Ease::Standard, 0.25f),
        "a Standard scroll follows the Standard ease");
  Check(standard.Value(At(quarter)) < linear.Value(At(quarter)),
        "which starts slower than linear");
}

// Sent somewhere new while moving, a scroll value carries on from where it is,
// with no jump, and takes the whole track again from there, as a notch of the
// wheel mid-move does.
void TestScrollValueRetargetsFromCurrent() {
  ScrollValue value;
  value.Retarget(DISTANCE, LINEAR, At(0.0f));
  const float halfway = value.Value(At(SCROLL_MS / 2.0f));
  value.Retarget(0.0f, LINEAR, At(SCROLL_MS / 2.0f));
  // The old move is at the same place at this moment, so this check alone shows
  // only that nothing jumps. The next one shows the new target took over.
  Check(value.Value(At(SCROLL_MS / 2.0f)) == halfway,
        "a new target doesn't make the value jump");
  Check(value.Value(At(SCROLL_MS)) == halfway / 2.0f,
        "and takes the whole track from there, half way back after half of it");
  Check(value.Moving(At(SCROLL_MS)) && !value.Moving(At(1.5f * SCROLL_MS)),
        "so it moves for the whole track after the new target");
}

// Asking again for the target a scroll value already has doesn't restart it,
// so a caller can ask every frame.
void TestScrollValueSameTarget() {
  ScrollValue value;
  value.Retarget(DISTANCE, LINEAR, At(0.0f));
  value.Retarget(DISTANCE, LINEAR, At(SCROLL_MS / 2.0f));
  Check(value.Value(At(SCROLL_MS)) == DISTANCE && !value.Moving(At(SCROLL_MS)),
        "asking for the same target again doesn't restart the move");
}

// Snapping moves a scroll value straight there and rests, even mid-move, and a
// track with no usable duration moves it straight to its target too, as
// reduced motion asks. A target or snap that isn't a number is ignored.
void TestScrollValueSnaps() {
  ScrollValue value;
  value.Retarget(DISTANCE, LINEAR, At(0.0f));
  value.Snap(DISTANCE / 3.0f);
  Check(value.Value(At(SCROLL_MS / 2.0f)) == DISTANCE / 3.0f &&
            !value.Moving(At(SCROLL_MS / 2.0f)),
        "a snap moves straight there mid-move and rests");
  value.Retarget(DISTANCE, {0.0f, Ease::Standard}, At(SCROLL_MS));
  Check(value.Value(At(SCROLL_MS)) == DISTANCE && !value.Moving(At(SCROLL_MS)),
        "a track with no duration moves straight to its target");
  value.Retarget(0.0f, {std::numeric_limits<float>::infinity(), Ease::Linear},
                 At(SCROLL_MS));
  Check(value.Value(At(SCROLL_MS)) == 0.0f,
        "and so does one that lasts for ever, which would freeze it");
  value.Retarget(std::numeric_limits<float>::quiet_NaN(), LINEAR,
                 At(SCROLL_MS));
  value.Snap(std::numeric_limits<float>::quiet_NaN());
  Check(value.Value(At(2.0f * SCROLL_MS)) == 0.0f && value.Target() == 0.0f,
        "a target or snap that isn't a number is ignored");
  const ScrollValue unset(std::numeric_limits<float>::quiet_NaN());
  Check(unset.Value(At(0.0f)) == 0.0f,
        "and one that starts at no number starts at 0");
}

// Infinite targets, snaps and starts are ignored as non-numbers are, where a
// finite one is taken, so an endless position never reaches the layout.
void TestScrollValueIgnoresInfinity() {
  constexpr float INFINITE = std::numeric_limits<float>::infinity();
  ScrollValue value(DISTANCE);
  value.Retarget(INFINITE, LINEAR, At(0.0f));
  Check(value.Target() == DISTANCE, "an infinite target is ignored");
  value.Snap(-INFINITE);
  Check(value.Value(At(0.0f)) == DISTANCE, "and so is an infinite snap");
  value.Snap(0.0f);
  Check(value.Value(At(0.0f)) == 0.0f, "where a finite snap is taken");
  const ScrollValue endless(INFINITE);
  Check(endless.Value(At(0.0f)) == 0.0f, "an infinite start starts at 0");
}

// A track longer than a minute takes a minute, so a broken token can't keep a
// scroll moving for ever. It still moves until then, so it was shortened, not
// dropped as a track with no duration is.
void TestScrollValueLongestTrack() {
  constexpr float MINUTE_MS = 60000.0f;
  constexpr float LONG_TRACK_MS = 1.0e9f;
  ScrollValue capped;
  capped.Retarget(DISTANCE, {LONG_TRACK_MS, Ease::Linear}, At(0.0f));
  Check(capped.Moving(At(MINUTE_MS - 1.0f)),
        "a track longer than a minute still moves just before a minute");
  Check(!capped.Moving(At(MINUTE_MS + 1.0f)) &&
            capped.Value(At(MINUTE_MS + 1.0f)) == DISTANCE,
        "and is over after a minute");
}

// How many rows the lone scroller below holds, and how many it shows, so it
// can scroll (SCROLL_ROWS - VIEW_ROWS) rows.
constexpr size_t SCROLL_ROWS = 20;
constexpr size_t VIEW_ROWS = 4;

// How far the lone scroller can scroll, in pixels at the reference size.
constexpr float MAX_SCROLL = static_cast<float>(SCROLL_ROWS - VIEW_ROWS) * ROW;

// How far one notch of the wheel scrolls, three of the theme's body lines, in
// pixels at the reference size.
const float NOTCH = 3.0f * DEFAULT_THEME.type.body.line.value;

// How long a scroll eases, the theme's base duration.
const float EASE_MS = DEFAULT_THEME.motion.base;

// One notch of the wheel toward the end of a column, toward its start, and
// toward the end of a row.
constexpr Vec2 DOWN{0.0f, -1.0f};
constexpr Vec2 UP{0.0f, 1.0f};
constexpr Vec2 ALONG_ROW{-1.0f, 0.0f};

// What a frame of the tracker tests is built with.
struct FrameInput {
  float ms = 0.0f;
  std::optional<Vec2> pointer;
  Vec2 wheel;
  bool reduced = false;
};

// A frame builder kept from frame to frame, as a UI keeps one, so the tracker
// in its workspace carries scroll positions from one frame to the next.
struct ScrollHarness {
  const ThemeContext &theme = DEFAULT_THEME;
  ScaleContext scale{theme.scale, theme.scale.reference_width,
                     theme.scale.reference_height};
  FrameBuilder frame;

  // Begins a frame built with `input` and returns what `build` returns, which
  // builds the tree, lays it out and reads it.
  template <class Build> auto Run(const FrameInput &input, Build &&build) {
    frame.Begin();
    const FrameScope scope(frame, theme, scale, FontsContext{},
                           ClockContext{At(input.ms)},
                           MotionContext{input.reduced},
                           PointerContext{input.pointer, input.wheel});
    return build();
  }
};

// Measures `root` for `rect`'s size and arranges it there.
void Lay(AnyElement root, const Rect &rect) {
  (void)Measure(root, rect.Size());
  Check(Arrange(root, rect), "the tree is placed");
}

// A column scroller THICKNESS wide and `height` tall around `rows` fixed rows,
// keeping its position by `key`, or by its build order without one.
BoxElement ScrollColumn(size_t rows, float height, Key key = {},
                        std::optional<DpInsets> padding = std::nullopt,
                        std::span<const AnyElement> extra = {}) {
  std::vector<AnyElement> children;
  for (size_t index = 0; index < rows; ++index) {
    children.push_back(FixedRow());
  }
  children.insert(children.end(), extra.begin(), extra.end());
  return Box({.layout = {.direction = FlexDirection::Column, .padding = padding},
              .self = {.layout = {.width = Dp(THICKNESS), .height = Dp(height)},
                       .styles = {.overflow = Overflow::Scroll}},
              .key = key},
             children);
}

// A row scroller THICKNESS wide and two rows tall around ten cells ROW wide,
// so it can scroll its cells past its width.
BoxElement ScrollRow() {
  constexpr size_t CELLS = 10;
  std::vector<AnyElement> cells;
  for (size_t index = 0; index < CELLS; ++index) {
    cells.push_back(Box({.self = {.layout = {.width = Dp(ROW)}}}));
  }
  return Box(
      {.layout = {.direction = FlexDirection::Row},
       .self = {.layout = {.width = Dp(THICKNESS), .height = Dp(2.0f * ROW)},
                .styles = {.overflow = Overflow::Scroll}}},
      cells);
}

// How far `scroller` has scrolled along `direction`, how far its first child
// has moved back from its start. The scrollers here have no padding.
float OffsetOf(const FrameBuilder &frame, AnyElement scroller,
               FlexDirection direction = FlexDirection::Column) {
  const Rect &box = frame.LayoutOf(scroller)->arranged;
  const Rect &first = frame.LayoutOf(frame.Child(scroller, 0))->arranged;
  return direction == FlexDirection::Row ? box.x - first.x : box.y - first.y;
}

// How far the lone scroller had scrolled in a frame, and whether it still
// needed frames.
struct ScrollRead {
  float offset = 0.0f;
  bool needs_frames = false;
};

// How the lone scroller is built in a frame. Laid out as normal, built but not
// laid out, as inside a collapsed box, or refused, as it is given a child that
// already has a parent.
enum class ColumnBuild : uint8_t {
  LaidOut,
  NotLaidOut,
  Refused,
};

// Builds one frame of a lone column scroller at the origin, VIEW_ROWS rows
// tall around `rows` rows with `padding`, built as `build` says, and reads it.
// Not laid out or refused, it reads nothing.
ScrollRead LoneColumn(ScrollHarness &harness, const FrameInput &input,
                      size_t rows = SCROLL_ROWS,
                      ColumnBuild build = ColumnBuild::LaidOut,
                      std::optional<DpInsets> padding = std::nullopt) {
  return harness.Run(input, [&] {
    std::vector<AnyElement> taken;
    if (build == ColumnBuild::Refused) {
      taken.push_back(FixedRow());
      const std::array<AnyElement, 1> owned{taken.back()};
      (void)Box({}, owned);
    }
    const BoxElement column = ScrollColumn(
        rows, static_cast<float>(VIEW_ROWS) * ROW, {}, padding, taken);
    // Not laid out or refused, there is nothing to read.
    if (build != ColumnBuild::LaidOut) {
      return ScrollRead{};
    }
    Lay(column, {0.0f, 0.0f, THICKNESS, static_cast<float>(VIEW_ROWS) * ROW});
    return ScrollRead{OffsetOf(harness.frame, column),
                      ScrollNeedsFrames(column)};
  });
}

// A point over the lone scroller, and one beside it.
constexpr Vec2 INSIDE{THICKNESS / 2.0f, ROW};
constexpr Vec2 OUTSIDE{2.0f * THICKNESS, ROW};

// A notch of the wheel over a column scroller eases it a notch toward the end
// of its content with the Enter ease, which starts fast, and a notch the other
// way brings it back. Until it gets there it needs frames.
void TestWheelScrollsColumn() {
  ScrollHarness harness;
  (void)LoneColumn(harness, {});
  const ScrollRead started =
      LoneColumn(harness, {.pointer = INSIDE, .wheel = DOWN});
  Check(started.offset == 0.0f && started.needs_frames,
        "a notch starts the scroller easing from where it was");
  const ScrollRead quarter = LoneColumn(harness, {.ms = EASE_MS / 4.0f});
  Check(quarter.offset == SnapPixel(NOTCH * ApplyEase(Ease::Enter, 0.25f)) &&
            quarter.offset > NOTCH / 4.0f,
        "it eases with Enter, further than linear a quarter of the way in");
  const ScrollRead done = LoneColumn(harness, {.ms = EASE_MS});
  Check(done.offset == NOTCH && !done.needs_frames,
        "and rests a notch along once the ease is over");
  (void)LoneColumn(harness, {.ms = EASE_MS, .pointer = INSIDE, .wheel = UP});
  Check(LoneColumn(harness, {.ms = 2.0f * EASE_MS}).offset == 0.0f,
        "a notch the other way brings it back");
}

// The wheel with the pointer beside a scroller, or with no pointer, leaves it
// where it is, where the same notch over it scrolls it.
void TestWheelElsewhereIgnored() {
  const auto offset_after = [](std::optional<Vec2> pointer) {
    ScrollHarness harness;
    (void)LoneColumn(harness, {});
    (void)LoneColumn(harness, {.pointer = pointer, .wheel = DOWN});
    return LoneColumn(harness, {.ms = EASE_MS}).offset;
  };
  Check(offset_after(OUTSIDE) == 0.0f,
        "a notch beside a scroller leaves it alone");
  Check(offset_after(std::nullopt) == 0.0f, "and so does one with no pointer");
  Check(offset_after(INSIDE) == NOTCH, "where one over it scrolls it");
}

// A notch while a scroller is easing adds to where it was going, not to where
// it is, so quick notches each count in full.
void TestNotchAddsToTarget() {
  ScrollHarness harness;
  (void)LoneColumn(harness, {});
  (void)LoneColumn(harness, {.pointer = INSIDE, .wheel = DOWN});
  (void)LoneColumn(harness,
                   {.ms = EASE_MS / 2.0f, .pointer = INSIDE, .wheel = DOWN});
  Check(LoneColumn(harness, {.ms = 1.5f * EASE_MS}).offset == 2.0f * NOTCH,
        "a second notch mid-ease takes the scroller two notches along");
}

// A scroller never scrolls past either end of its content. Turned past its
// start it stays there, so the next notch the other way moves it a whole
// notch, and turned past its end it stops there. When its content shrinks
// under it, it moves straight to the new end.
void TestScrollClamps() {
  constexpr float PAST = 2.0f;
  ScrollHarness harness;
  (void)LoneColumn(harness, {});
  (void)LoneColumn(harness, {.pointer = INSIDE, .wheel = Vec2{0.0f, PAST}});
  (void)LoneColumn(harness, {.ms = EASE_MS, .pointer = INSIDE, .wheel = DOWN});
  Check(LoneColumn(harness, {.ms = 2.0f * EASE_MS}).offset == NOTCH,
        "turned past its start it stays there, so a notch back moves a notch");
  const float notches_past_end = MAX_SCROLL / NOTCH + PAST;
  (void)LoneColumn(harness, {.ms = 2.0f * EASE_MS,
                             .pointer = INSIDE,
                             .wheel = Vec2{0.0f, -notches_past_end}});
  Check(LoneColumn(harness, {.ms = 3.0f * EASE_MS}).offset == MAX_SCROLL,
        "turned past its end it stops at the end");
  constexpr size_t SHRUNK_ROWS = VIEW_ROWS + 2;
  const ScrollRead shrunk =
      LoneColumn(harness, {.ms = 3.0f * EASE_MS}, SHRUNK_ROWS);
  Check(shrunk.offset == static_cast<float>(SHRUNK_ROWS - VIEW_ROWS) * ROW &&
            !shrunk.needs_frames,
        "and when its content shrinks it moves straight to the new end");
}

// With reduced motion a notch moves a scroller straight there in the frame it
// turns, needing no more frames, where it would otherwise start easing.
void TestReducedMotionSnaps() {
  ScrollHarness harness;
  (void)LoneColumn(harness, {.reduced = true});
  const ScrollRead moved =
      LoneColumn(harness, {.pointer = INSIDE, .wheel = DOWN, .reduced = true});
  Check(moved.offset == NOTCH && !moved.needs_frames,
        "with reduced motion a notch moves the scroller straight there");
}

// How far an inner and an outer scroller have scrolled.
struct NestedRead {
  float inner = 0.0f;
  float outer = 0.0f;
};

// Builds one frame of an outer column scroller four rows tall at the origin
// holding an inner one two rows tall around `inner_rows` rows, then ten rows
// of its own, and reads both.
NestedRead NestedColumns(ScrollHarness &harness, const FrameInput &input,
                         size_t inner_rows) {
  constexpr size_t OUTER_ROWS = 10;
  return harness.Run(input, [&] {
    const BoxElement inner = ScrollColumn(inner_rows, 2.0f * ROW);
    std::vector<AnyElement> children{inner};
    for (size_t index = 0; index < OUTER_ROWS; ++index) {
      children.push_back(FixedRow());
    }
    const BoxElement outer = Box(
        {.layout = {.direction = FlexDirection::Column},
         .self = {.layout = {.width = Dp(THICKNESS),
                             .height = Dp(static_cast<float>(VIEW_ROWS) * ROW)},
                  .styles = {.overflow = Overflow::Scroll}}},
        children);
    Lay(outer, {0.0f, 0.0f, THICKNESS, static_cast<float>(VIEW_ROWS) * ROW});
    return NestedRead{OffsetOf(harness.frame, inner),
                      OffsetOf(harness.frame, outer)};
  });
}

// A point over the inner scroller, and one over the outer one only.
constexpr Vec2 OVER_INNER{THICKNESS / 2.0f, ROW};
constexpr Vec2 OVER_OUTER{THICKNESS / 2.0f, 3.0f * ROW};

// Turns the wheel by `wheel` over `pointer` in a frame of the nested
// scrollers at `ms` and reads them once the ease is over.
NestedRead TurnNested(ScrollHarness &harness, float ms, Vec2 pointer,
                      Vec2 wheel, size_t inner_rows) {
  (void)NestedColumns(harness, {.ms = ms, .pointer = pointer, .wheel = wheel},
                      inner_rows);
  return NestedColumns(harness, {.ms = ms + EASE_MS}, inner_rows);
}

// Where scrollers nest, the innermost under the pointer takes the wheel, and
// the wheel doesn't pass on to the outer one even once the inner one is at its
// end. Over the outer one alone, the outer one takes it.
void TestInnermostTakesWheel() {
  constexpr size_t INNER_ROWS = 10;
  ScrollHarness harness;
  (void)NestedColumns(harness, {}, INNER_ROWS);
  const NestedRead inner_turned =
      TurnNested(harness, 0.0f, OVER_INNER, DOWN, INNER_ROWS);
  Check(inner_turned.inner == NOTCH && inner_turned.outer == 0.0f,
        "the inner scroller under the pointer takes the wheel");
  const float past_end = static_cast<float>(INNER_ROWS);
  const NestedRead inner_end = TurnNested(harness, EASE_MS, OVER_INNER,
                                          Vec2{0.0f, -past_end}, INNER_ROWS);
  Check(inner_end.outer == 0.0f && inner_end.inner > NOTCH,
        "and the wheel doesn't pass on as it reaches its end");
  const NestedRead at_end =
      TurnNested(harness, 2.0f * EASE_MS, OVER_INNER, DOWN, INNER_ROWS);
  Check(at_end.outer == 0.0f && at_end.inner == inner_end.inner,
        "nor once it is already at its end");
  const NestedRead outer_turned =
      TurnNested(harness, 3.0f * EASE_MS, OVER_OUTER, DOWN, INNER_ROWS);
  Check(outer_turned.outer == NOTCH,
        "where over the outer scroller alone, the outer one takes it");
}

// How far two overlapping scrollers have scrolled, the one built first and the
// one built second.
struct OverlapRead {
  float first = 0.0f;
  float second = 0.0f;
};

// Builds one frame of two column scrollers over each other at the origin, each
// in a box of its own, the first box on `first_layer`, and reads both.
OverlapRead OverlappingColumns(ScrollHarness &harness, const FrameInput &input,
                               uint8_t first_layer) {
  const float height = static_cast<float>(VIEW_ROWS) * ROW;
  return harness.Run(input, [&] {
    const BoxElement first = ScrollColumn(SCROLL_ROWS, height);
    const BoxElement second = ScrollColumn(SCROLL_ROWS, height);
    const auto holder = [&](BoxElement scroller, uint8_t layer) {
      return Box({.self = {.layout = {.width = Dp(THICKNESS),
                                      .height = Dp(height)},
                           .styles = {.layer = layer}}},
                 {scroller});
    };
    const BoxElement root = Box(
        {.layout = {.stacked = true},
         .self = {.layout = {.width = Dp(THICKNESS), .height = Dp(height)}}},
        {holder(first, first_layer), holder(second, 0)});
    Lay(root, {0.0f, 0.0f, THICKNESS, height});
    return OverlapRead{OffsetOf(harness.frame, first),
                       OffsetOf(harness.frame, second)};
  });
}

// Where two scrollers overlap, the one drawn on top takes the wheel. On one
// layer that is the one built last. With the one built first raised to a
// higher layer, it is drawn on top, so it takes the wheel instead.
void TestHigherLayerTakesWheel() {
  const auto turned = [](uint8_t first_layer) {
    ScrollHarness harness;
    (void)OverlappingColumns(harness, {}, first_layer);
    (void)OverlappingColumns(harness, {.pointer = INSIDE, .wheel = DOWN},
                             first_layer);
    return OverlappingColumns(harness, {.ms = EASE_MS}, first_layer);
  };
  const OverlapRead flat = turned(0);
  Check(flat.first == 0.0f && flat.second == NOTCH,
        "on one layer the scroller built last takes the wheel");
  const OverlapRead raised = turned(1);
  Check(raised.first == NOTCH && raised.second == 0.0f,
        "and one raised to a higher layer takes it though built first");
}

// An inner scroller whose content fits can't scroll, so the wheel over it
// goes to the outer one, where one that overflows takes it itself.
void TestFittingScrollerPassesWheel() {
  constexpr size_t FITTING_ROWS = 2;
  ScrollHarness harness;
  (void)NestedColumns(harness, {}, FITTING_ROWS);
  const NestedRead turned =
      TurnNested(harness, 0.0f, OVER_INNER, DOWN, FITTING_ROWS);
  Check(turned.outer == NOTCH && turned.inner == 0.0f,
        "the wheel over an inner scroller that fits scrolls the outer one");
}

// How far a column scroller and a row scroller at its top have scrolled.
struct AxisRead {
  float column = 0.0f;
  float row = 0.0f;
};

// Builds one frame of a column scroller at the origin with a row scroller as
// its first child, then ten rows, and reads both.
AxisRead RowInColumn(ScrollHarness &harness, const FrameInput &input) {
  constexpr size_t OUTER_ROWS = 10;
  return harness.Run(input, [&] {
    const BoxElement row = ScrollRow();
    std::vector<AnyElement> children{row};
    for (size_t index = 0; index < OUTER_ROWS; ++index) {
      children.push_back(FixedRow());
    }
    const BoxElement column = Box(
        {.layout = {.direction = FlexDirection::Column},
         .self = {.layout = {.width = Dp(THICKNESS),
                             .height = Dp(static_cast<float>(VIEW_ROWS) * ROW)},
                  .styles = {.overflow = Overflow::Scroll}}},
        children);
    Lay(column, {0.0f, 0.0f, THICKNESS, static_cast<float>(VIEW_ROWS) * ROW});
    return AxisRead{OffsetOf(harness.frame, column),
                    OffsetOf(harness.frame, row, FlexDirection::Row)};
  });
}

// A row scroller takes only the horizontal wheel, so the vertical wheel over a
// row inside a column scrolls the column, and the horizontal wheel there
// scrolls the row.
void TestWheelFollowsAxis() {
  ScrollHarness harness;
  (void)RowInColumn(harness, {});
  (void)RowInColumn(harness, {.pointer = OVER_INNER, .wheel = ALONG_ROW});
  const AxisRead horizontal = RowInColumn(harness, {.ms = EASE_MS});
  Check(horizontal.row == NOTCH && horizontal.column == 0.0f,
        "the horizontal wheel over a row scrolls the row");
  // The row is still where it was, as the column hasn't moved, so the pointer
  // is still over it.
  (void)RowInColumn(harness,
                    {.ms = EASE_MS, .pointer = OVER_INNER, .wheel = DOWN});
  const AxisRead vertical = RowInColumn(harness, {.ms = 2.0f * EASE_MS});
  Check(vertical.column == NOTCH && vertical.row == NOTCH,
        "and the vertical wheel over it scrolls the column around it");
}

// Builds one frame of a box two rows tall with `overflow` holding a spacer
// three rows tall and then the lone scroller, so the scroller sits below the
// box's edge, and reads how far the scroller has scrolled.
float ScrollerBelowEdge(ScrollHarness &harness, const FrameInput &input,
                        Overflow overflow) {
  return harness.Run(input, [&] {
    const BoxElement scroller =
        ScrollColumn(SCROLL_ROWS, static_cast<float>(VIEW_ROWS) * ROW);
    const std::array<AnyElement, 2> children{
        Box({.self = {.layout = {.height = Dp(3.0f * ROW)}}}), scroller};
    const BoxElement holder = Box(
        {.layout = {.direction = FlexDirection::Column},
         .self = {.layout = {.width = Dp(THICKNESS), .height = Dp(2.0f * ROW)},
                  .styles = {.overflow = overflow}}},
        children);
    Lay(holder, {0.0f, 0.0f, THICKNESS, 2.0f * ROW});
    return OffsetOf(harness.frame, scroller);
  });
}

// A scroller hidden by a box that clips doesn't take the wheel where it can't
// be seen, where the same scroller under a box that doesn't clip does.
void TestClippedScrollerIgnored() {
  const Vec2 below_edge{THICKNESS / 2.0f, 4.0f * ROW};
  const auto offset_after = [&below_edge](Overflow overflow) {
    ScrollHarness harness;
    (void)ScrollerBelowEdge(harness, {}, overflow);
    (void)ScrollerBelowEdge(harness, {.pointer = below_edge, .wheel = DOWN},
                            overflow);
    return ScrollerBelowEdge(harness, {.ms = EASE_MS}, overflow);
  };
  Check(offset_after(Overflow::Clip) == 0.0f,
        "a scroller clipped out of sight doesn't take the wheel");
  Check(offset_after(Overflow::Scroll) == 0.0f,
        "nor one scrolled out of sight by a scroller around it");
  Check(offset_after(Overflow::Visible) == NOTCH, "where one in sight does");
}

// Builds one frame of a box two rows tall with `overflow` around a box that
// clips, tall enough to show the lone scroller below a spacer three rows tall,
// and reads how far the scroller has scrolled.
float ScrollerInNestedClips(ScrollHarness &harness, const FrameInput &input,
                            Overflow overflow) {
  constexpr float INNER_ROWS_TALL = 7.0f;
  return harness.Run(input, [&] {
    const BoxElement scroller =
        ScrollColumn(SCROLL_ROWS, static_cast<float>(VIEW_ROWS) * ROW);
    const std::array<AnyElement, 2> children{
        Box({.self = {.layout = {.height = Dp(3.0f * ROW)}}}), scroller};
    const std::array<AnyElement, 1> inner{
        Box({.layout = {.direction = FlexDirection::Column},
             .self = {.layout = {.width = Dp(THICKNESS),
                                 .height = Dp(INNER_ROWS_TALL * ROW)},
                      .styles = {.overflow = Overflow::Clip}}},
            children)};
    const BoxElement outer = Box(
        {.layout = {.direction = FlexDirection::Column},
         .self = {.layout = {.width = Dp(THICKNESS), .height = Dp(2.0f * ROW)},
                  .styles = {.overflow = overflow}}},
        inner);
    Lay(outer, {0.0f, 0.0f, THICKNESS, 2.0f * ROW});
    return OffsetOf(harness.frame, scroller);
  });
}

// A box that clips inside one that clips less still hides what the outer one
// hides, so a scroller the inner box shows but the outer one hides doesn't
// take the wheel, where it does when the outer box doesn't clip.
void TestNestedClipsHideScroller() {
  const Vec2 below_edge{THICKNESS / 2.0f, 4.0f * ROW};
  const auto offset_after = [&below_edge](Overflow overflow) {
    ScrollHarness harness;
    (void)ScrollerInNestedClips(harness, {}, overflow);
    (void)ScrollerInNestedClips(harness, {.pointer = below_edge, .wheel = DOWN},
                                overflow);
    return ScrollerInNestedClips(harness, {.ms = EASE_MS}, overflow);
  };
  Check(offset_after(Overflow::Clip) == 0.0f,
        "a scroller hidden by an outer clip doesn't take the wheel");
  Check(offset_after(Overflow::Visible) == NOTCH,
        "where one only the inner clip would show does when the outer box "
        "doesn't clip");
}

// Builds three frames of a row holding the two scrollers `build` builds side
// by side, turning the wheel over the first in the second frame, and reads
// how far each has scrolled once the ease is over.
template <class Build> std::array<float, 2> ScrollFirstOfPair(Build &&build) {
  ScrollHarness harness;
  const auto frame = [&](const FrameInput &input) {
    return harness.Run(input, [&] {
      const std::array<BoxElement, 2> pair = build();
      const std::array<AnyElement, 2> children{pair[0], pair[1]};
      const BoxElement row = Box({}, children);
      Lay(row,
          {0.0f, 0.0f, 2.0f * THICKNESS, static_cast<float>(VIEW_ROWS) * ROW});
      return std::array<float, 2>{OffsetOf(harness.frame, pair[0]),
                                  OffsetOf(harness.frame, pair[1])};
    });
  };
  (void)frame({});
  (void)frame({.pointer = INSIDE, .wheel = DOWN});
  return frame({.ms = EASE_MS});
}

// Unkeyed scrollers keep their own positions by their build order, and keys
// given to scrollers built in one place, as rows of a loop are, keep them
// apart too. TestKeyOutlastsOrder shows the keys, not the order, find them.
void TestScrollersKeptApart() {
  const float height = static_cast<float>(VIEW_ROWS) * ROW;
  const std::array<float, 2> sites = ScrollFirstOfPair([height] {
    return std::array<BoxElement, 2>{ScrollColumn(SCROLL_ROWS, height),
                                     ScrollColumn(SCROLL_ROWS, height)};
  });
  Check(sites == std::array<float, 2>{NOTCH, 0.0f},
        "unkeyed scrollers scroll apart");
  const std::array<float, 2> keys = ScrollFirstOfPair([height] {
    std::array<BoxElement, 2> pair;
    for (uint64_t index = 0; index < pair.size(); ++index) {
      pair[index] = ScrollColumn(SCROLL_ROWS, height, Key("row", index));
    }
    return pair;
  });
  Check(keys == std::array<float, 2>{NOTCH, 0.0f},
        "and keys don't merge scrollers built in one place");
}

// Builds three frames of a row holding the two scrollers `build` builds side
// by side, turning the wheel over the first in the second frame, and then
// building them in the other order in the third, once the ease is over. Reads
// how far the scroller built first and the one built second have scrolled in
// that third frame.
template <class Build>
std::array<float, 2> ScrollFirstThenSwap(Build &&build) {
  ScrollHarness harness;
  const auto frame = [&](const FrameInput &input, bool swapped) {
    return harness.Run(input, [&] {
      const std::array<BoxElement, 2> pair = build(swapped);
      const std::array<AnyElement, 2> children{pair[0], pair[1]};
      const BoxElement row = Box({}, children);
      Lay(row,
          {0.0f, 0.0f, 2.0f * THICKNESS, static_cast<float>(VIEW_ROWS) * ROW});
      return std::array<float, 2>{OffsetOf(harness.frame, pair[0]),
                                  OffsetOf(harness.frame, pair[1])};
    });
  };
  (void)frame({}, false);
  (void)frame({.pointer = INSIDE, .wheel = DOWN}, false);
  return frame({.ms = EASE_MS}, true);
}

// A keyed scroller keeps its position when built in another order, as the
// key and not the build order finds it. The same scrollers unkeyed hand their
// positions over by build order, so the position follows the key alone.
void TestKeyOutlastsOrder() {
  const float height = static_cast<float>(VIEW_ROWS) * ROW;
  const std::array<float, 2> keyed = ScrollFirstThenSwap([height](bool swap) {
    const Key first = swap ? Key("b") : Key("a");
    const Key second = swap ? Key("a") : Key("b");
    return std::array<BoxElement, 2>{ScrollColumn(SCROLL_ROWS, height, first),
                                     ScrollColumn(SCROLL_ROWS, height, second)};
  });
  Check(keyed == std::array<float, 2>{0.0f, NOTCH},
        "a keyed scroller keeps its position built in another order");
  const std::array<float, 2> unkeyed = ScrollFirstThenSwap([height](bool) {
    return std::array<BoxElement, 2>{ScrollColumn(SCROLL_ROWS, height),
                                     ScrollColumn(SCROLL_ROWS, height)};
  });
  Check(unkeyed == std::array<float, 2>{NOTCH, 0.0f},
        "where unkeyed ones keep theirs by build order");
}

// Builds three frames of an unkeyed scroller on the left, built after a
// scroller with `first` on its right, turning the wheel over the left one in
// the second frame, and leaving the right one out of the third. Reads how far
// the left one has scrolled in that third frame, once the ease is over.
float KeptAfterFirstLeaves(Key first) {
  ScrollHarness harness;
  const float height = static_cast<float>(VIEW_ROWS) * ROW;
  const auto frame = [&](const FrameInput &input, bool with_first) {
    return harness.Run(input, [&] {
      const BoxElement leaves =
          with_first ? ScrollColumn(SCROLL_ROWS, height, first) : BoxElement{};
      const BoxElement kept = ScrollColumn(SCROLL_ROWS, height);
      std::vector<AnyElement> children{kept};
      if (with_first) {
        children.push_back(leaves);
      }
      const BoxElement row = Box({}, children);
      Lay(row, {0.0f, 0.0f, 2.0f * THICKNESS, height});
      return OffsetOf(harness.frame, kept);
    });
  };
  (void)frame({}, true);
  (void)frame({.pointer = INSIDE, .wheel = DOWN}, true);
  return frame({.ms = EASE_MS}, false);
}

// An unkeyed scroller keeps its position when a keyed one built before it
// leaves, as a keyed scroller takes no number from the unkeyed ones. The same
// with the one that leaves unkeyed renumbers the kept one, which then finds
// the other's unscrolled position, so the key alone keeps the number.
void TestKeyedTakesNoNumber() {
  Check(KeptAfterFirstLeaves(Key("leaves")) == NOTCH,
        "an unkeyed scroller keeps its place when a keyed one before it leaves");
  Check(KeptAfterFirstLeaves(Key()) == 0.0f,
        "where an unkeyed one before it leaving renumbers it");
}

// A scrolling card, keeping its position by `key`.
BoxElement ScrollCard(Key key = {}) {
  std::vector<AnyElement> rows;
  for (size_t index = 0; index < SCROLL_ROWS; ++index) {
    rows.push_back(FixedRow());
  }
  return Card(
      {.layout = {.direction = FlexDirection::Column,
                  .padding = DpInsets(Dp(0.0f))},
       .self = {.layout = {.width = Dp(THICKNESS),
                           .height = Dp(static_cast<float>(VIEW_ROWS) * ROW)},
                .styles = {.overflow = Overflow::Scroll}},
       .key = key},
      rows);
}

// Unkeyed cards scroll apart, and a keyed card keeps its position when built
// in another order, so a card passes its key to its box rather than scrolling
// by build order alone.
void TestCardsKeptApart() {
  const std::array<float, 2> cards = ScrollFirstOfPair(
      [] { return std::array<BoxElement, 2>{ScrollCard(), ScrollCard()}; });
  Check(cards == std::array<float, 2>{NOTCH, 0.0f},
        "unkeyed cards scroll apart");
  const std::array<float, 2> keyed = ScrollFirstThenSwap([](bool swap) {
    return std::array<BoxElement, 2>{ScrollCard(swap ? Key("b") : Key("a")),
                                     ScrollCard(swap ? Key("a") : Key("b"))};
  });
  Check(keyed == std::array<float, 2>{0.0f, NOTCH},
        "and a keyed card keeps its position built in another order");
}

// A key is the same for the same name and differs for another name, index or
// part, and a named key is never none, so keys tell elements apart.
void TestKeys() {
  Check(Key().IsNone(), "a default key is none");
  Check(!Key("pane").IsNone() && Key("pane") == Key("pane"),
        "a named key is some key, the same for the same name");
  Check(Key("pane") != Key("list"), "and differs for another name");
  Check(Key("row", 1) != Key("row", 2) && Key("row", 1) != Key("row"),
        "an index tells keys of one name apart");
  Check(Key("pane").With("scroll") != Key("pane") &&
            Key("pane").With("scroll") == Key("pane").With("scroll"),
        "and a part makes a key of its own, the same each time");
  // A name given in a box's props, as callers write it, so that spelling keeps
  // compiling.
  const BoxProps named{.key = "pane"};
  Check(named.key == Key("pane"), "a key named in props is that name's key");
  static_assert(Key("pane") == Key(std::string_view("pane")),
                "a key is made at compile time, the same from either spelling");
}

// A column of SCROLL_ROWS rows that doesn't scroll itself, the one child the
// braced scrollers below hold, so they have more than they show.
AnyElement TallColumn() {
  std::vector<AnyElement> rows;
  for (size_t index = 0; index < SCROLL_ROWS; ++index) {
    rows.push_back(FixedRow());
  }
  return Box({.layout = {.direction = FlexDirection::Column},
              .self = {.layout = {.width = Dp(THICKNESS)}}},
             rows);
}

// Unkeyed scrollers and cards given their children as a braced list scroll
// apart by their build order, so the list form numbers each box as it is
// built. A list form that gave them one identity would turn the wheel over the
// first and scroll the second too.
void TestBracedScrollersKeptApart() {
  const float height = static_cast<float>(VIEW_ROWS) * ROW;
  const BoxProps scroller{
      .layout = {.direction = FlexDirection::Column},
      .self = {.layout = {.width = Dp(THICKNESS), .height = Dp(height)},
               .styles = {.overflow = Overflow::Scroll}}};
  const std::array<float, 2> boxes = ScrollFirstOfPair([&] {
    const BoxElement first = Box(scroller, {TallColumn()});
    const BoxElement second = Box(scroller, {TallColumn()});
    return std::array<BoxElement, 2>{first, second};
  });
  Check(boxes == std::array<float, 2>{NOTCH, 0.0f},
        "unkeyed boxes given braced children scroll apart");
  const CardProps card{
      .layout = {.direction = FlexDirection::Column,
                 .padding = DpInsets(Dp(0.0f))},
      .self = {.layout = {.width = Dp(THICKNESS), .height = Dp(height)},
               .styles = {.overflow = Overflow::Scroll}}};
  const std::array<float, 2> cards = ScrollFirstOfPair([&] {
    const BoxElement first = Card(card, {TallColumn()});
    const BoxElement second = Card(card, {TallColumn()});
    return std::array<BoxElement, 2>{first, second};
  });
  Check(cards == std::array<float, 2>{NOTCH, 0.0f},
        "and so do cards given braced children");
}

// A notch at the end of a scroller resting there leaves it at the end, at
// rest, needing no frames, where a notch back from the end starts it easing.
void TestNotchAtEndStaysAtRest() {
  ScrollHarness harness;
  (void)LoneColumn(harness, {});
  const float past_end = static_cast<float>(SCROLL_ROWS);
  (void)LoneColumn(harness,
                   {.pointer = INSIDE, .wheel = Vec2{0.0f, -past_end}});
  (void)LoneColumn(harness, {.ms = EASE_MS});
  const ScrollRead at_end =
      LoneColumn(harness, {.ms = EASE_MS, .pointer = INSIDE, .wheel = DOWN});
  Check(at_end.offset == MAX_SCROLL && !at_end.needs_frames,
        "a notch at the end leaves the scroller resting at the end");
  const ScrollRead back =
      LoneColumn(harness, {.ms = EASE_MS, .pointer = INSIDE, .wheel = UP});
  Check(back.needs_frames, "where a notch back from the end starts it easing");
}

// A notch in the frame the content grows counts in full against the grown
// content, so a scroller resting at the end of short content moves a whole
// notch on when rows are added that frame.
void TestNotchCountsWhenContentGrows() {
  constexpr size_t SHORT_ROWS = VIEW_ROWS + 2;
  // Two rows, less than a notch, so one notch takes the short content to its
  // end. The first check fails loudly if a theme makes a notch shorter.
  const float short_max = static_cast<float>(SHORT_ROWS - VIEW_ROWS) * ROW;
  ScrollHarness harness;
  (void)LoneColumn(harness, {}, SHORT_ROWS);
  (void)LoneColumn(harness, {.pointer = INSIDE, .wheel = DOWN}, SHORT_ROWS);
  Check(LoneColumn(harness, {.ms = EASE_MS}, SHORT_ROWS).offset == short_max,
        "the scroller rests at the end of its short content");
  (void)LoneColumn(harness, {.ms = EASE_MS, .pointer = INSIDE, .wheel = DOWN},
                   SCROLL_ROWS);
  Check(LoneColumn(harness, {.ms = 2.0f * EASE_MS}).offset == short_max + NOTCH,
        "and a notch as rows are added moves it a whole notch on");
}

// When content shrinks under a scroller still easing toward a target past the
// new end, the target comes back to the end too, so when the content grows
// again the scroller stays at that end rather than jumping on to the old
// target.
void TestShrinkBringsTargetBack() {
  constexpr size_t SHRUNK_ROWS = VIEW_ROWS + 3;
  const float shrunk_max = static_cast<float>(SHRUNK_ROWS - VIEW_ROWS) * ROW;
  ScrollHarness harness;
  (void)LoneColumn(harness, {});
  (void)LoneColumn(harness, {.pointer = INSIDE, .wheel = DOWN});
  const ScrollRead shrunk =
      LoneColumn(harness, {.ms = EASE_MS / 4.0f}, SHRUNK_ROWS);
  Check(
      shrunk.offset < shrunk_max && shrunk_max < NOTCH,
      "the scroller is short of the shrunk end, which is short of its target");
  Check(LoneColumn(harness, {.ms = 2.0f * EASE_MS}).offset == shrunk_max,
        "and stays at the shrunk end when the content grows again");
}

// A scroller built last frame but not laid out, as inside a collapsed box,
// wasn't drawn, so it doesn't take the wheel where it was drawn before.
void TestUnarrangedScrollerIgnored() {
  const auto offset_after = [](ColumnBuild build) {
    ScrollHarness harness;
    (void)LoneColumn(harness, {});
    (void)LoneColumn(harness, {}, SCROLL_ROWS, build);
    (void)LoneColumn(harness, {.pointer = INSIDE, .wheel = DOWN});
    return LoneColumn(harness, {.ms = EASE_MS}).offset;
  };
  Check(offset_after(ColumnBuild::NotLaidOut) == 0.0f,
        "a scroller not laid out last frame doesn't take the wheel");
  Check(offset_after(ColumnBuild::LaidOut) == NOTCH, "where one laid out does");
}

// A scroller refused for a frame isn't built, so it keeps no position and
// starts again at the start of its content when next built, where one built
// in that frame keeps its place.
void TestRefusedScrollerForgotten() {
  const auto offset_after = [](ColumnBuild build) {
    ScrollHarness harness;
    (void)LoneColumn(harness, {});
    (void)LoneColumn(harness, {.pointer = INSIDE, .wheel = DOWN});
    (void)LoneColumn(harness, {.ms = EASE_MS});
    (void)LoneColumn(harness, {.ms = EASE_MS}, SCROLL_ROWS, build);
    return LoneColumn(harness, {.ms = EASE_MS}).offset;
  };
  Check(offset_after(ColumnBuild::Refused) == 0.0f,
        "a scroller refused for a frame starts again at the start");
  Check(offset_after(ColumnBuild::NotLaidOut) == NOTCH,
        "where one built but not laid out keeps its place");
}

// A scroller's padding stays clear at both ends, so it scrolls as far as its
// rows run past the space inside its padding, and its first row starts inside
// the padding.
void TestPaddedScrollerRange() {
  ScrollHarness harness;
  const DpInsets padding{Dp{PAD}};
  const float height = static_cast<float>(VIEW_ROWS) * ROW;
  (void)LoneColumn(harness, {}, SCROLL_ROWS, ColumnBuild::LaidOut, padding);
  const float past_end = static_cast<float>(SCROLL_ROWS);
  (void)LoneColumn(harness, {.pointer = INSIDE, .wheel = Vec2{0.0f, -past_end}},
                   SCROLL_ROWS, ColumnBuild::LaidOut, padding);
  const ScrollRead read = LoneColumn(harness, {.ms = EASE_MS}, SCROLL_ROWS,
                                     ColumnBuild::LaidOut, padding);
  // OffsetOf measures from the box's edge, so the padding is part of it.
  const float range =
      static_cast<float>(SCROLL_ROWS) * ROW - (height - 2.0f * PAD);
  Check(read.offset == range - PAD,
        "a padded scroller scrolls as far as its rows run past its padding");
}

// A box that doesn't scroll never needs frames for scrolling.
void TestStillBoxNeedsNoFrames() {
  ScrollHarness harness;
  const bool needs = harness.Run({}, [] {
    const BoxElement still = Box({});
    Lay(still, {0.0f, 0.0f, THICKNESS, THICKNESS});
    return ScrollNeedsFrames(still);
  });
  Check(!needs, "a box that doesn't scroll needs no frames for scrolling");
}

// The indicator sizes the geometry tests use, with no corner to keep clear of.
constexpr IndicatorSizes PLAIN_SIZES{
    .thickness = 4.0f, .inset = 2.0f, .min_length = 32.0f, .corner = 0.0f};

// The rectangle inside the border the geometry tests use, and content four
// times its length scrolled half way.
constexpr Rect INDICATOR_BOX{0.0f, 0.0f, 200.0f, 40.0f};
constexpr float INDICATOR_CONTENT = 800.0f;
constexpr float INDICATOR_VIEWPORT = 200.0f;
constexpr float INDICATOR_HALF_WAY = 300.0f;

// Whether `rect` is `expected` exactly.
bool RectIs(const std::optional<Rect> &rect, const Rect &expected) {
  return rect.has_value() && *rect == expected;
}

// A row's indicator runs along its bottom edge, inside the inset, as long as
// the share of the content in view, and half way along its track when the row
// is scrolled half way. At the start and end of the content it sits at the
// ends of its track, so it moves with the scroll.
void TestRowIndicatorGeometry() {
  const auto at = [](float offset) {
    return ScrollIndicatorRect(INDICATOR_BOX,
                               {.direction = FlexDirection::Row,
                                .content = INDICATOR_CONTENT,
                                .viewport = INDICATOR_VIEWPORT,
                                .offset = offset},
                               PLAIN_SIZES);
  };
  // The track is the width less an inset at each end, and the thumb a quarter
  // of it, as a quarter of the content is in view.
  const float track = INDICATOR_BOX.width - 2.0f * PLAIN_SIZES.inset;
  const float length = track * INDICATOR_VIEWPORT / INDICATOR_CONTENT;
  const float bottom =
      INDICATOR_BOX.Bottom() - PLAIN_SIZES.inset - PLAIN_SIZES.thickness;
  Check(length > PLAIN_SIZES.min_length,
        "the share in view is longer than the least length");
  Check(RectIs(at(INDICATOR_HALF_WAY),
               {PLAIN_SIZES.inset + (track - length) / 2.0f, bottom, length,
                PLAIN_SIZES.thickness}),
        "a row's indicator runs along its bottom, half way when scrolled half "
        "way");
  Check(RectIs(at(0.0f),
               {PLAIN_SIZES.inset, bottom, length, PLAIN_SIZES.thickness}),
        "at the start of its track at the start of the content");
  Check(RectIs(at(INDICATOR_CONTENT - INDICATOR_VIEWPORT),
               {PLAIN_SIZES.inset + track - length, bottom, length,
                PLAIN_SIZES.thickness}),
        "and at the end of its track at the end of the content");
}

// A column's indicator runs along its right edge, and none is drawn when the
// content fits or there is no room for a track.
void TestColumnIndicatorGeometry() {
  const Rect tall{0.0f, 0.0f, INDICATOR_BOX.height, INDICATOR_BOX.width};
  const ScrollMetrics column{.direction = FlexDirection::Column,
                             .content = INDICATOR_CONTENT,
                             .viewport = INDICATOR_VIEWPORT,
                             .offset = 0.0f};
  const std::optional<Rect> right =
      ScrollIndicatorRect(tall, column, PLAIN_SIZES);
  Check(right.has_value() &&
            right->x ==
                tall.Right() - PLAIN_SIZES.inset - PLAIN_SIZES.thickness &&
            right->y == PLAIN_SIZES.inset &&
            right->width == PLAIN_SIZES.thickness,
        "a column's indicator runs down its right edge");
  ScrollMetrics fits = column;
  fits.content = INDICATOR_VIEWPORT;
  Check(!ScrollIndicatorRect(tall, fits, PLAIN_SIZES).has_value(),
        "content that fits has no indicator");
  const Rect flat{0.0f, 0.0f, INDICATOR_BOX.width, 2.0f * PLAIN_SIZES.inset};
  Check(!ScrollIndicatorRect(flat, column, PLAIN_SIZES).has_value(),
        "and neither does a box with no room for a track");
}

// What one draw command that drew something drew, its clip, the bounds of
// its vertices and the most opaque of them.
struct DrawnCommand {
  ImVec4 clip;
  Rect bounds;
  uint32_t alpha = 0;
};

// Every draw command `list` drew something with, in order.
std::vector<DrawnCommand> DrawnCommands(const ImDrawList &list) {
  std::vector<DrawnCommand> drawn;
  for (const ImDrawCmd &command : list.CmdBuffer) {
    if (command.ElemCount == 0) {
      continue;
    }
    float left = FULLSCREEN.Right();
    float top = FULLSCREEN.Bottom();
    float right = FULLSCREEN.x;
    float bottom = FULLSCREEN.y;
    uint32_t alpha = 0;
    for (unsigned int index = 0; index < command.ElemCount; ++index) {
      const ImDrawVert &vertex = list.VtxBuffer[static_cast<int>(
          command.VtxOffset +
          list.IdxBuffer[static_cast<int>(command.IdxOffset + index)])];
      left = std::min(left, vertex.pos.x);
      top = std::min(top, vertex.pos.y);
      right = std::max(right, vertex.pos.x);
      bottom = std::max(bottom, vertex.pos.y);
      alpha = std::max(alpha, (vertex.col >> IM_COL32_A_SHIFT) & 0xFFu);
    }
    drawn.push_back(
        {command.ClipRect, {left, top, right - left, bottom - top}, alpha});
  }
  return drawn;
}

// Whether `bounds`, drawn with anti-aliasing, covers `rect`, as the fringe
// reaches up to a pixel past each edge.
bool CoversWithFringe(const Rect &bounds, const Rect &rect) {
  constexpr float FRINGE = 1.0f;
  return std::abs(bounds.x - rect.x) <= FRINGE &&
         std::abs(bounds.y - rect.y) <= FRINGE &&
         std::abs(bounds.Right() - rect.Right()) <= FRINGE &&
         std::abs(bounds.Bottom() - rect.Bottom()) <= FRINGE;
}

// The scroller the paint tests draw, 100 by 80 at the origin with a border two
// units wide and corners rounded twelve, so its indicator keeps inside the
// border and clear of the corners.
constexpr float PAINTED_HEIGHT = 80.0f;
constexpr float PAINTED_BORDER = 2.0f;
constexpr float PAINTED_RADIUS = 12.0f;

// Builds one frame of the painted scroller around `rows` rows, paints it with
// `opacity` and returns what each draw command drew.
std::vector<DrawnCommand> PaintedScroller(ScrollHarness &harness,
                                          const FrameInput &input, size_t rows,
                                          float opacity = 1.0f,
                                          float own_opacity = 1.0f) {
  return harness.Run(input, [&] {
    std::vector<AnyElement> children;
    for (size_t index = 0; index < rows; ++index) {
      children.push_back(FixedRow());
    }
    BoxStyles style{.border = Color{255, 255, 255, 255},
                    .border_width = Dp(PAINTED_BORDER),
                    .radius = Dp(PAINTED_RADIUS),
                    .overflow = Overflow::Scroll};
    // The scroller's own opacity comes from a state, held on while it is below
    // one, as a disabled control fades.
    style.states.disabled = {.opacity = own_opacity};
    const BoxElement scroller =
        Box({.layout = {.direction = FlexDirection::Column},
             .self = {.layout = {.width = Dp(THICKNESS),
                                 .height = Dp(PAINTED_HEIGHT)},
                      .styles = style,
                      .state = {.disabled = own_opacity < 1.0f}}},
            children);
    Lay(scroller, {0.0f, 0.0f, THICKNESS, PAINTED_HEIGHT});
    ImDrawListSharedData shared;
    shared.ClipRectFullscreen = ImVec4(FULLSCREEN.x, FULLSCREEN.y,
                                       FULLSCREEN.Right(), FULLSCREEN.Bottom());
    shared.SetCircleTessellationMaxError(TESSELLATION);
    ImDrawList list(&shared);
    list._ResetForNewFrame();
    list.PushClipRectFullScreen();
    list.PushTexture(ImTextureRef());
    Paint(scroller, list, {.opacity = opacity});
    return DrawnCommands(list);
  });
}

// Where the painted scroller's indicator goes, worked out by hand from the
// theme's indicator tokens at the reference size. Inside the border it has a
// 96 by 76 rectangle. Each end of the track keeps the inset and the corner
// clear, the radius less the border and the inset, so the track is 76 less
// twice 10. A fifth of the content is in view, shorter than the least length,
// so the thumb takes the least length. It moves along what is left of the
// track as `share` of the scroll is done.
Rect PaintedIndicator(float share) {
  const SpaceScale &space = DEFAULT_THEME.space;
  const float inset = space.hair.value;
  const float corner = PAINTED_RADIUS - PAINTED_BORDER - inset;
  const float inside = PAINTED_HEIGHT - 2.0f * PAINTED_BORDER;
  const float track = inside - 2.0f * (inset + corner);
  const float length = space.s6.value;
  return {THICKNESS - PAINTED_BORDER - inset - space.s1.value,
          PAINTED_BORDER + inset + corner + (track - length) * share,
          space.s1.value, length};
}

// A scroller whose content overflows draws its indicator last, over its rows
// and outside their clip, inside its border and clear of its rounded corners,
// at the least length. One whose content fits draws none, so its rows are the
// last thing drawn.
void TestIndicatorPainted() {
  ScrollHarness harness;
  const std::vector<DrawnCommand> overflowing =
      PaintedScroller(harness, {}, SCROLL_ROWS);
  Check(!overflowing.empty() && ClipIs(overflowing.back().clip, FULLSCREEN) &&
            CoversWithFringe(overflowing.back().bounds, PaintedIndicator(0.0f)),
        "an overflowing scroller draws its indicator last, outside its rows' "
        "clip, inside its border and clear of its corners");
  const std::vector<DrawnCommand> fitting =
      PaintedScroller(harness, {}, VIEW_ROWS - 1);
  Check(!fitting.empty() && !ClipIs(fitting.back().clip, FULLSCREEN),
        "one whose content fits draws none, so its rows are drawn last");
}

// A scroller arranged twice in a frame draws its indicator from how it was
// last arranged, not first. The second arrange is twice as tall, so twice as
// much is in view and the thumb is longer than the least length, where the
// first arrange's share would give the least length in the taller track.
void TestIndicatorFollowsLastArrange() {
  constexpr float MOVED_X = 2.0f * THICKNESS;
  constexpr float TALL = 2.0f * PAINTED_HEIGHT;
  const float inset = DEFAULT_THEME.space.hair.value;
  const float track = TALL - 2.0f * inset;
  const float content = static_cast<float>(SCROLL_ROWS) * ROW;
  const float tall_length = track * TALL / content;
  Check(tall_length > DEFAULT_THEME.space.s6.value &&
            track * PAINTED_HEIGHT / content < DEFAULT_THEME.space.s6.value,
        "the taller share is past the least length and the shorter one isn't");
  ScrollHarness harness;
  const Rect bounds = harness.Run({}, [] {
    const BoxElement scroller = ScrollColumn(SCROLL_ROWS, PAINTED_HEIGHT);
    Lay(scroller, {0.0f, 0.0f, THICKNESS, PAINTED_HEIGHT});
    Check(Arrange(scroller, {MOVED_X, 0.0f, THICKNESS, TALL}),
          "the scroller is placed again");
    ImDrawListSharedData shared;
    shared.ClipRectFullscreen = ImVec4(FULLSCREEN.x, FULLSCREEN.y,
                                       FULLSCREEN.Right(), FULLSCREEN.Bottom());
    shared.SetCircleTessellationMaxError(TESSELLATION);
    ImDrawList list(&shared);
    list._ResetForNewFrame();
    list.PushClipRectFullScreen();
    list.PushTexture(ImTextureRef());
    Paint(scroller, list);
    return DrawnCommands(list).back().bounds;
  });
  Check(bounds.x > MOVED_X &&
            CoversWithFringe(bounds,
                             {bounds.x, inset, bounds.width, tall_length}),
        "a scroller arranged twice draws its indicator from its last arrange");
}

// The indicator fades with the scroller, so it is drawn at the opacity the
// scroller paints with.
void TestIndicatorFades() {
  constexpr float HALF = 0.5f;
  ScrollHarness harness;
  const uint32_t full = PaintedScroller(harness, {}, SCROLL_ROWS).back().alpha;
  const uint32_t faded =
      PaintedScroller(harness, {}, SCROLL_ROWS, HALF).back().alpha;
  Check(full > 0 && faded < full &&
            std::abs(static_cast<float>(faded) - HALF * full) <= 1.0f,
        "the indicator is drawn at the opacity its scroller paints with");
  const uint32_t own =
      PaintedScroller(harness, {}, SCROLL_ROWS, 1.0f, HALF).back().alpha;
  Check(std::abs(static_cast<float>(own) - HALF * full) <= 1.0f,
        "including the scroller's own opacity");
}

// Edge cases of where the indicator goes. A track shorter than the least
// length holds the thumb to the track. An offset rounded a little past the end
// leaves it at the end of its track. Corners too big to leave a track of the
// least length are kept clear only as far as still leaves one, so it still
// shows, where smaller ones are kept clear in full.
// Content of no length with a negative viewport, from padding larger than the
// box, has none.
void TestIndicatorEdgeCases() {
  const ScrollMetrics column{.direction = FlexDirection::Column,
                             .content = INDICATOR_CONTENT,
                             .viewport = INDICATOR_VIEWPORT,
                             .offset = 0.0f};
  constexpr float SHORT_EDGE = 30.0f;
  const Rect short_box{0.0f, 0.0f, INDICATOR_BOX.height, SHORT_EDGE};
  const float short_track = SHORT_EDGE - 2.0f * PLAIN_SIZES.inset;
  Check(
      short_track < PLAIN_SIZES.min_length &&
          RectIs(ScrollIndicatorRect(short_box, column, PLAIN_SIZES),
                 {short_box.Right() - PLAIN_SIZES.inset - PLAIN_SIZES.thickness,
                  PLAIN_SIZES.inset, PLAIN_SIZES.thickness, short_track}),
      "a track shorter than the least length holds the thumb to the track");
  const float max = INDICATOR_CONTENT - INDICATOR_VIEWPORT;
  ScrollMetrics row = column;
  row.direction = FlexDirection::Row;
  row.offset = max;
  ScrollMetrics past = row;
  past.offset = max + 0.5f;
  const std::optional<Rect> at_end =
      ScrollIndicatorRect(INDICATOR_BOX, row, PLAIN_SIZES);
  Check(at_end.has_value() &&
            RectIs(ScrollIndicatorRect(INDICATOR_BOX, past, PLAIN_SIZES),
                   *at_end),
        "an offset a little past the end leaves the thumb at its end");
  constexpr Rect WIDE{0.0f, 0.0f, 280.0f, 158.0f};
  IndicatorSizes rounded = PLAIN_SIZES;
  rounded.corner = 77.0f;
  const std::optional<Rect> big = ScrollIndicatorRect(WIDE, column, rounded);
  // As much clearance as leaves a track of exactly the least length.
  const float most_clear =
      (WIDE.height - 2.0f * PLAIN_SIZES.inset - PLAIN_SIZES.min_length) / 2.0f;
  Check(most_clear < rounded.corner && big.has_value() &&
            big->y == PLAIN_SIZES.inset + most_clear &&
            big->height == PLAIN_SIZES.min_length,
        "corners too big for a track of the least length are kept clear only "
        "as far as leaves one");
  rounded.corner = 20.0f;
  const std::optional<Rect> small = ScrollIndicatorRect(WIDE, column, rounded);
  Check(small.has_value() && small->y == PLAIN_SIZES.inset + rounded.corner,
        "where smaller ones are kept clear in full");
  const ScrollMetrics empty{.direction = FlexDirection::Column,
                            .content = 0.0f,
                            .viewport = -10.0f,
                            .offset = 0.0f};
  Check(!ScrollIndicatorRect(INDICATOR_BOX, empty, PLAIN_SIZES).has_value(),
        "content of no length has no indicator");
}

// The indicator follows the scroll, at the end of its track once the scroller
// has scrolled to the end of its content.
void TestIndicatorFollowsScroll() {
  ScrollHarness harness;
  (void)PaintedScroller(harness, {}, SCROLL_ROWS);
  const float past_end = static_cast<float>(SCROLL_ROWS);
  (void)PaintedScroller(
      harness,
      {.pointer = Vec2{THICKNESS / 2.0f, ROW}, .wheel = Vec2{0.0f, -past_end}},
      SCROLL_ROWS);
  const std::vector<DrawnCommand> scrolled =
      PaintedScroller(harness, {.ms = EASE_MS}, SCROLL_ROWS);
  Check(CoversWithFringe(scrolled.back().bounds, PaintedIndicator(1.0f)),
        "the indicator is at the end of its track at the end of the content");
}

} // namespace

int main() {
  TestPointerContext();
  TestScrollValueMoves();
  TestScrollValueEases();
  TestScrollValueRetargetsFromCurrent();
  TestScrollValueSameTarget();
  TestScrollValueSnaps();
  TestScrollValueIgnoresInfinity();
  TestScrollValueLongestTrack();
  TestWheelScrollsColumn();
  TestWheelElsewhereIgnored();
  TestNotchAddsToTarget();
  TestScrollClamps();
  TestReducedMotionSnaps();
  TestInnermostTakesWheel();
  TestHigherLayerTakesWheel();
  TestFittingScrollerPassesWheel();
  TestWheelFollowsAxis();
  TestClippedScrollerIgnored();
  TestKeys();
  TestScrollersKeptApart();
  TestKeyOutlastsOrder();
  TestKeyedTakesNoNumber();
  TestCardsKeptApart();
  TestBracedScrollersKeptApart();
  TestNotchAtEndStaysAtRest();
  TestNotchCountsWhenContentGrows();
  TestShrinkBringsTargetBack();
  TestUnarrangedScrollerIgnored();
  TestRefusedScrollerForgotten();
  TestPaddedScrollerRange();
  TestNestedClipsHideScroller();
  TestStillBoxNeedsNoFrames();
  TestRowIndicatorGeometry();
  TestColumnIndicatorGeometry();
  TestIndicatorPainted();
  TestIndicatorFollowsLastArrange();
  TestIndicatorFades();
  TestIndicatorFollowsScroll();
  TestIndicatorEdgeCases();
  TestScrollKeepsLengths();
  TestScrollBoxLength();
  TestScrollMinContent();
  TestScrollerShortened();
  TestScrollKeepsCollapsingRow();
  TestScrollClipsInsideBorder();
  TestScrollClipsInsideHairline();
  TestScrollClipFollowsState();
  TestStackedScrollClips();
  TestRowScroller();
  TestScrollerGrowsWhenFits();
  std::printf("SystemUiScrollTests: passed\n");
  return 0;
}
