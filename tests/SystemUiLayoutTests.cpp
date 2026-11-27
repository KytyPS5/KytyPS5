// Checks the system UI puts each inset on the side it means where insets are
// built from separate values: the safe area's margins, insets turned into
// pixels, a toast's content padding and the trophy toast's padding. Each
// reads the sides field by field, so a call site that passes its values in the
// wrong order fails here even though it compiles. It also checks a box's edge
// band is drawn along its top, and that a glyph, the trophy's among them,
// draws exactly what its shapes laid out as boxes draw.
#include "graphics/presentation/system/animation/animate.h"
#include "graphics/presentation/system/components/card.h"
#include "graphics/presentation/system/components/glyphs/status.h"
#include "graphics/presentation/system/components/glyphs/trophy.h"
#include "graphics/presentation/system/components/toast.h"
#include "graphics/presentation/system/core/dp.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/core/scale.h"
#include "graphics/presentation/system/core/theme.h"
#include "graphics/presentation/system/core/units.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/frameLayout.h"
#include "graphics/presentation/system/element/frame/framePaint.h"
#include "graphics/presentation/system/element/frame/frameScope.h"
#include "graphics/presentation/system/element/primitives/box/box.h"
#include "graphics/presentation/system/element/primitives/box/interactionStates.h"
#include "graphics/presentation/system/element/primitives/glyph/glyph.h"
#include "graphics/presentation/system/imgui/draw/edge.h"
#include "graphics/presentation/system/imgui/draw/shine.h"
#include "graphics/presentation/system/views/trophyUnlocked/trophyUnlocked.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace Libs::Graphics::SystemUi;

// The full screen clip the test draw lists start under, larger than anything
// the tests place, so a clip equal to it means nothing narrowed it.
constexpr float FULLSCREEN_HALF = 8192.0f;
constexpr Rect FULLSCREEN{-FULLSCREEN_HALF, -FULLSCREEN_HALF,
                          2.0f * FULLSCREEN_HALF, 2.0f * FULLSCREEN_HALF};

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "SystemUiLayoutTests: failed: %s\n", message);
    std::abort();
  }
}

// Whether `insets` holds these pixels on these sides, read field by field.
bool HasSides(const EdgeInsets &insets, float top, float right, float bottom,
              float left) {
  return insets.top == top && insets.right == right &&
         insets.bottom == bottom && insets.left == left;
}

// Whether `insets` holds these lengths on these sides, read field by field.
bool HasSides(const DpInsets &insets, Dp top, Dp right, Dp bottom, Dp left) {
  return insets.top == top && insets.right == right &&
         insets.bottom == bottom && insets.left == left;
}

// On an ultrawide screen the safe area's side margins come from the width
// capped at the content aspect and its top and bottom margins from the height,
// so the two differ. The expected margins are worked out here from the theme's
// rules, without the insets' constructors.
void TestSafeArea() {
  const ScaleRules &rules = DEFAULT_THEME.scale;
  constexpr float WIDTH = 2560.0f;
  constexpr float HEIGHT = 1080.0f;
  constexpr float PERCENT = 10.0f;
  const ScaleContext context(rules, WIDTH, HEIGHT,
                             {.safe_area_percent = PERCENT});
  const float edge = rules.edge_min.value * context.Scale();
  const float span_x = std::min(WIDTH, HEIGHT * rules.content_aspect);
  const float sides = SnapPixel(std::max(edge, PERCENT / 100.0f * span_x));
  const float ends = SnapPixel(std::max(edge, PERCENT / 100.0f * HEIGHT));
  Check(sides != ends, "the side margins differ from the top and bottom ones");
  const Rect area = context.SafeArea();
  Check(area.x == sides && area.width == WIDTH - 2.0f * sides,
        "the safe area's side margins come from the capped width");
  Check(area.y == ends && area.height == HEIGHT - 2.0f * ends,
        "and its top and bottom margins from the height");
}

// Each side of insets in design units lands on the same side in pixels. At
// the theme's reference size a design unit is one pixel, so each side keeps
// its own distinct value.
void TestToPixels() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  DpInsets insets;
  insets.top = Dp(10.0f);
  insets.right = Dp(20.0f);
  insets.bottom = Dp(30.0f);
  insets.left = Dp(40.0f);
  Check(HasSides(ToPixels(insets), 10.0f, 20.0f, 30.0f, 40.0f),
        "each side in design units becomes the same side in pixels");
}

// The padding the box `element` puts around its children, or none when it sets
// none.
DpInsets BoxPadding(const FrameBuilder &frame, AnyElement element) {
  const BoxProps *props = frame.FindAs<BoxProps>(element);
  Check(props != nullptr, "the element is a box");
  return props->layout.padding.value_or(DpInsets{});
}

// A banded toast's content sits the theme's horizontal padding from its sides
// and its vertical padding from its bottom, and past the band as well at its
// top. A plain toast has no padding, as its content brings its own. The
// theme's band plus its vertical padding happens to equal its horizontal
// padding, so the toast here patches in a band of its own, which keeps the
// top apart from the sides.
void TestToastPadding() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const ToastTokens tokens = ToastDefaults(theme);
  const Dp vertical = tokens.padding_vertical;
  const Dp horizontal = tokens.padding_horizontal;
  const Dp band = tokens.edge_width + Dp(2.0f);
  // The left and right share one padding and every other pair of sides
  // differs, so swapping any two sides with different paddings fails below
  // rather than passing.
  Check(vertical != horizontal && band != Dp() && band + vertical != horizontal,
        "the toast's paddings differ and its band has a width");
  Check(HasSides(BoxPadding(frame, Toast({.status = Status::Info,
                                          .tokens = {.edge_width = band}})),
                 band + vertical, horizontal, vertical, horizontal),
        "a banded toast pads its content on each side, past the band at the "
        "top");
  Check(HasSides(BoxPadding(frame, Toast({})), Dp(), Dp(), Dp(), Dp()),
        "and a plain toast doesn't pad it");
}

// The clip rect a box's edge band of `band` reference pixels was drawn under,
// over `box` with rounded corners, read from the draw command holding its
// vertices.
ImVec4 EdgeBandClip(const Rect &box, float band) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  ImDrawListSharedData shared;
  shared.ClipRectFullscreen = ImVec4(FULLSCREEN.x, FULLSCREEN.y,
                                     FULLSCREEN.Right(), FULLSCREEN.Bottom());
  shared.SetCircleTessellationMaxError(0.30f);
  ImDrawList list(&shared);
  list._ResetForNewFrame();
  list.PushClipRectFullScreen();
  list.PushTexture(ImTextureRef());
  DrawEdgeBand(list, box, 8.0f, band, Color{255, 0, 0, 255});
  for (const ImDrawCmd &command : list.CmdBuffer) {
    if (command.ElemCount > 0) {
      return command.ClipRect;
    }
  }
  Check(false, "the band draws");
  return {};
}

// A box's edge band is drawn cut to a strip along its top, as wide as the box
// and as tall as the band, its bottom on a whole pixel, so it shows only along
// the top. On a box shorter than the band it is cut to the box's height. The
// box starts part way through a pixel, so the whole-pixel bottom shows.
void TestEdgeBandOnTop() {
  constexpr float BAND = 4.0f;
  const Rect box{10.5f, 20.25f, 100.0f, 50.0f};
  const ImVec4 clip = EdgeBandClip(box, BAND);
  Check(clip.x == box.x && clip.z == box.x + box.width && clip.y == box.y &&
            clip.w == std::round(box.y + BAND),
        "the band is a strip along the box's top, as wide as the box and as "
        "tall as the band");
  const Rect short_box{10.5f, 20.25f, 100.0f, 3.0f};
  Check(EdgeBandClip(short_box, BAND).w ==
            std::round(short_box.y + short_box.height),
        "and on a box shorter than the band, as tall as the box");
}

// The clip of every draw command that drew something when `root` is painted,
// in drawing order, under a full screen clip of FULLSCREEN.
std::vector<ImVec4> PaintedClips(AnyElement root) {
  ImDrawListSharedData shared;
  shared.ClipRectFullscreen = ImVec4(FULLSCREEN.x, FULLSCREEN.y,
                                     FULLSCREEN.Right(), FULLSCREEN.Bottom());
  shared.SetCircleTessellationMaxError(0.30f);
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

// Where a box with `overflow` and a filled child, followed by a filled
// sibling, is placed, and the clip of each draw command that drew something,
// in drawing order. The box itself is clear, so only the child and the sibling
// draw.
struct ClipDraw {
  Rect box;
  std::vector<ImVec4> clips;
};

ClipDraw DrawClipped(Overflow overflow) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const BoxStyles filled{.fill = Color{255, 0, 0, 255}};
  const std::array<AnyElement, 1> inside{
      Box({.self = {.layout = {.width = Dp(20.0f), .height = Dp(20.0f)},
                    .styles = filled}})};
  const BoxElement clipper =
      Box({.self = {.layout = {.width = Dp(40.0f), .height = Dp(30.0f)},
                    .styles = {.overflow = overflow}}},
          inside);
  const std::array<AnyElement, 2> parts{
      clipper,
      Box({.self = {.layout = {.width = Dp(10.0f), .height = Dp(10.0f)},
                    .styles = filled}})};
  const BoxElement root =
      Box({.layout = {.direction = FlexDirection::Column}}, parts);
  (void)Measure(root, {500.0f, 500.0f});
  Check(Arrange(root, {10.0f, 20.0f, 100.0f, 100.0f}), "the boxes are placed");
  return {frame.LayoutOf(clipper)->arranged, PaintedClips(root)};
}

// Whether `clip` is exactly `rect`.
bool ClipIs(const ImVec4 &clip, const Rect &rect) {
  return clip.x == rect.x && clip.y == rect.y && clip.z == rect.Right() &&
         clip.w == rect.Bottom();
}

// A box with Overflow::Clip draws its children clipped to its outer
// rectangle, and what is drawn after it is no longer clipped by it. The same
// boxes with Overflow::Visible draw everything under the full screen clip.
void TestOverflowClip() {
  const ClipDraw clipped = DrawClipped(Overflow::Clip);
  Check(clipped.clips.size() == 2 && ClipIs(clipped.clips[0], clipped.box),
        "a clipping box's child is drawn clipped to the box");
  Check(ClipIs(clipped.clips[1], FULLSCREEN),
        "and what is drawn after it is no longer clipped by it");
  const ClipDraw visible = DrawClipped(Overflow::Visible);
  Check(!visible.clips.empty() &&
            std::all_of(
                visible.clips.begin(), visible.clips.end(),
                [](const ImVec4 &clip) { return ClipIs(clip, FULLSCREEN); }),
        "while a box that doesn't clip draws its child under the full "
        "screen clip");
}

// A clipping box that starts and ends part way through pixels clips its
// children to the whole pixels it touches, as the renderer drops a clip's
// fractions and would otherwise cut a child short of the box's right and
// bottom edges. Flex snaps the children it places, so the box is the root,
// placed where the caller says. Its rectangle is checked to be fractional, so
// the clip differs from it only by the rounding.
void TestOverflowClipRoundsOut() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const std::array<AnyElement, 1> inside{
      Box({.self = {.layout = {.width = Dp(20.0f), .height = Dp(20.0f)},
                    .styles = {.fill = Color{255, 0, 0, 255}}}})};
  const BoxElement clipper =
      Box({.self = {.styles = {.overflow = Overflow::Clip}}}, inside);
  (void)Measure(clipper, {500.0f, 500.0f});
  Check(Arrange(clipper, {10.5f, 20.25f, 40.25f, 30.5f}), "the box is placed");
  const Rect &box = frame.LayoutOf(clipper)->arranged;
  Check(box.x != std::floor(box.x) && box.y != std::floor(box.y) &&
            box.Right() != std::ceil(box.Right()) &&
            box.Bottom() != std::ceil(box.Bottom()),
        "the clipping box starts and ends part way through pixels");
  const float left = std::floor(box.x);
  const float top = std::floor(box.y);
  const Rect whole{left, top, std::ceil(box.Right()) - left,
                   std::ceil(box.Bottom()) - top};
  const std::vector<ImVec4> clips = PaintedClips(clipper);
  Check(clips.size() == 1 && ClipIs(clips[0], whole),
        "its child is clipped to the whole pixels the box touches");
}

// A clipping box inside another clips its children to where the two overlap,
// not to its own rectangle, so a nested clip never lets a child out of an
// outer one. The inner box is wider than the outer, which children may be as
// they don't shrink, so the overlap differs from the inner box's rectangle.
void TestNestedOverflowClip() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const std::array<AnyElement, 1> content{
      Box({.self = {.layout = {.width = Dp(50.0f), .height = Dp(10.0f)},
                    .styles = {.fill = Color{255, 0, 0, 255}}}})};
  const BoxElement inner =
      Box({.self = {.layout = {.width = Dp(60.0f), .height = Dp(20.0f)},
                    .styles = {.overflow = Overflow::Clip}}},
          content);
  const std::array<AnyElement, 1> inside{inner};
  const BoxElement outer =
      Box({.self = {.layout = {.width = Dp(40.0f), .height = Dp(30.0f)},
                    .styles = {.overflow = Overflow::Clip}}},
          inside);
  (void)Measure(outer, {500.0f, 500.0f});
  Check(Arrange(outer, {10.0f, 20.0f, 40.0f, 30.0f}), "the boxes are placed");
  const Rect &outer_rect = frame.LayoutOf(outer)->arranged;
  const Rect &inner_rect = frame.LayoutOf(inner)->arranged;
  const float right = std::min(outer_rect.Right(), inner_rect.Right());
  const float bottom = std::min(outer_rect.Bottom(), inner_rect.Bottom());
  const Rect overlap{inner_rect.x, inner_rect.y, right - inner_rect.x,
                     bottom - inner_rect.y};
  Check(inner_rect.Right() > outer_rect.Right(),
        "the inner box pokes past the outer one");
  const std::vector<ImVec4> clips = PaintedClips(outer);
  Check(clips.size() == 1 && ClipIs(clips[0], overlap),
        "the inner box's child is clipped to where the two boxes overlap");
}

// The trophy toast pads its icon and text evenly, with the toast's vertical
// padding on all four sides, so the space around the icon looks the same
// beside as above it. That padding is on the box inside the toast's column, as
// the toast itself is plain. The theme's two paddings differ, so the
// horizontal one on any side fails the check. The text beside the icon is
// limited to the toast's widest less that same padding, the icon and the gap,
// so a long name fills the toast exactly and no wider.
void TestTrophyPadding() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const ToastTokens tokens = ToastDefaults(theme);
  const Dp vertical = tokens.padding_vertical;
  const Dp horizontal = tokens.padding_horizontal;
  Check(vertical != horizontal, "the theme's toast paddings differ");
  const AnyElement toast = TrophyUnlocked({.name = "Champion",
                                           .subtitle = "Trophy earned",
                                           .grade = TrophyGrade::Gold});
  Check(frame.ChildCount(toast) == 1, "the trophy toast holds one column");
  const AnyElement column = frame.Child(toast, 0);
  Check(frame.ChildCount(column) == 1, "which holds the trophy's content");
  const AnyElement content = frame.Child(column, 0);
  const DpInsets padding = BoxPadding(frame, content);
  Check(HasSides(padding, vertical, vertical, vertical, vertical),
        "the trophy's content is padded by the vertical padding on every side");
  const TrophyUnlockedTokens trophy = TrophyUnlockedDefaults(theme);
  Check(frame.ChildCount(content) == 2,
        "the content holds the icon and the text");
  const BoxProps *text = frame.FindAs<BoxProps>(frame.Child(content, 1));
  const Dp room = padding.left + padding.right + trophy.icon_size + trophy.gap;
  Check(text != nullptr &&
            text->self.layout.max_main == tokens.max_width - room,
        "the text's widest leaves room for the content's own padding, the icon "
        "and the gap");
}

// A trophy toast's icon tile glows with the theme's trophy glow in its grade's
// colour, and the toast's card doesn't, so the glow is around the icon rather
// than the whole toast. Gold and silver are checked to differ first, so the
// colour can only come from the grade.
void TestTrophyIconGlow() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  Check(!(theme.colors.trophy.gold == theme.colors.trophy.silver),
        "gold and silver differ");
  Check(theme.glow.trophy.radius != Dp() && theme.glow.trophy.peak_alpha > 0.0f,
        "the trophy glow differs from none");
  const auto glows = [&](TrophyGrade grade) {
    FrameBuilder frame;
    frame.Begin();
    const FrameScope scope(frame, theme, scale, {}, {});
    const AnyElement toast = TrophyUnlocked(
        {.name = "Champion", .subtitle = "Trophy earned", .grade = grade});
    const AnyElement content = frame.Child(frame.Child(toast, 0), 0);
    const BoxProps *card = frame.FindAs<BoxProps>(toast);
    const BoxProps *tile = frame.FindAs<BoxProps>(frame.Child(content, 0));
    Check(card != nullptr && tile != nullptr,
          "the toast and its icon tile are boxes");
    return std::pair<BoxGlow, BoxGlow>{card->self.styles.glow,
                                       tile->self.styles.glow};
  };
  const auto same = [](const BoxGlow &actual, const BoxGlow &wanted) {
    return actual.glow.radius == wanted.glow.radius &&
           actual.glow.peak_alpha == wanted.glow.peak_alpha &&
           actual.color == wanted.color;
  };
  const auto [gold_card, gold_tile] = glows(TrophyGrade::Gold);
  Check(same(gold_tile, {theme.glow.trophy, theme.colors.trophy.gold}),
        "a gold trophy's icon tile glows with the trophy glow in gold");
  Check(same(gold_card, {}), "and the toast's card doesn't glow");
  const auto [silver_card, silver_tile] = glows(TrophyGrade::Silver);
  Check(same(silver_tile, {theme.glow.trophy, theme.colors.trophy.silver}),
        "a silver trophy's tile glows in silver");
}

// Arrange sets each element's layer to the higher of the layer it asks for and
// its parent's, so a box's layer is a floor for everything inside it. A child
// asking for less sits on its parent's layer, one asking for more rises above
// it, and the parent's layer is put back for the siblings after it. An Animate
// takes its child's layer, so wrapping a box in one changes nothing.
void TestEffectiveLayers() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const auto box = [](uint8_t layer) {
    constexpr Dp SIDE = Dp(8.0f);
    return Box({.self = {.layout = {.width = SIDE, .height = SIDE},
                         .styles = {.layer = layer}}});
  };
  const BoxElement below = box(0);
  const BoxElement between = box(1);
  const BoxElement above = box(3);
  const BoxElement raised =
      Box({.self = {.styles = {.layer = 2}}}, {below, between, above});
  const BoxElement sibling = box(1);
  const BoxElement wrapped = box(2);
  const AnimateElement animate = Animate({}, wrapped);
  const BoxElement last = box(0);
  const BoxElement root =
      Box({.layout = {.direction = FlexDirection::Column}},
          {raised, sibling, animate, last});
  (void)Measure(root, {FULLSCREEN.width, FULLSCREEN.height});
  Check(Arrange(root, FULLSCREEN), "the layered tree is placed");
  const auto layer = [&frame](AnyElement element) {
    return frame.LayoutOf(element)->layer;
  };
  Check(layer(raised) == 2, "a box sits on the layer it asks for");
  Check(layer(below) == 2 && layer(between) == 2,
        "a child asking for less sits on its parent's layer");
  Check(layer(above) == 3, "and one asking for more rises above it");
  Check(layer(sibling) == 1 && layer(last) == 0,
        "the parent's layer is put back for the siblings after it");
  Check(layer(animate) == 2 && layer(wrapped) == 2,
        "an Animate takes its child's layer");
}

// How finely ImGui cuts rounded corners into segments, ImGui's default, which
// the overlay keeps.
constexpr float TESSELLATION = 0.30f;

// One draw command that drew something, how many indices it drew and the clip
// it drew them under.
struct DrawnCommand {
  unsigned int elements = 0;
  ImVec4 clip;
};

// What a draw list holds after painting, compared vertex for vertex.
struct Drawn {
  std::vector<ImDrawVert> vertices;
  std::vector<ImDrawIdx> indices;
  std::vector<DrawnCommand> commands;
};

// What painting `root` with `opacity` draws, under a full screen clip and the
// anti-aliasing the overlay draws with, which adds the edge vertices whose
// places depend on every shape's exact rectangle.
Drawn PaintedDraw(AnyElement root, float opacity) {
  ImDrawListSharedData shared;
  shared.ClipRectFullscreen = ImVec4(FULLSCREEN.x, FULLSCREEN.y,
                                     FULLSCREEN.Right(), FULLSCREEN.Bottom());
  shared.SetCircleTessellationMaxError(TESSELLATION);
  ImDrawList list(&shared);
  list._ResetForNewFrame();
  list.Flags =
      ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;
  list.PushClipRectFullScreen();
  list.PushTexture(ImTextureRef());
  Paint(root, list, {.opacity = opacity});
  Drawn drawn{.vertices = {list.VtxBuffer.begin(), list.VtxBuffer.end()},
              .indices = {list.IdxBuffer.begin(), list.IdxBuffer.end()}};
  for (const ImDrawCmd &command : list.CmdBuffer) {
    if (command.ElemCount > 0) {
      drawn.commands.push_back(
          {.elements = command.ElemCount, .clip = command.ClipRect});
    }
  }
  return drawn;
}

// Whether `a` and `b` drew the same triangles, in the same places, colours and
// texture coordinates, split the same way into draw commands under the same
// clips.
bool SameDraw(const Drawn &a, const Drawn &b) {
  const auto same_vertex = [](const ImDrawVert &left, const ImDrawVert &right) {
    return left.pos.x == right.pos.x && left.pos.y == right.pos.y &&
           left.uv.x == right.uv.x && left.uv.y == right.uv.y &&
           left.col == right.col;
  };
  const auto same_command = [](const DrawnCommand &left,
                               const DrawnCommand &right) {
    return left.elements == right.elements && left.clip.x == right.clip.x &&
           left.clip.y == right.clip.y && left.clip.z == right.clip.z &&
           left.clip.w == right.clip.w;
  };
  return std::equal(a.commands.begin(), a.commands.end(), b.commands.begin(),
                    b.commands.end(), same_command) &&
         a.indices == b.indices &&
         std::equal(a.vertices.begin(), a.vertices.end(), b.vertices.begin(),
                    b.vertices.end(), same_vertex);
}

// Whether any vertex `drawn` holds is fully `color`, as a shape's inside is.
bool DrewColor(const Drawn &drawn, Color color) {
  const ImU32 wanted = IM_COL32(color.r, color.g, color.b, color.a);
  return std::any_of(
      drawn.vertices.begin(), drawn.vertices.end(),
      [wanted](const ImDrawVert &vertex) { return vertex.col == wanted; });
}

// A test glyph on a grid of ten, with shapes that start, end and are as wide
// as half a grid unit, whose lengths each round to whole pixels on their own,
// so an edge worked out from a sum rather than length by length lands a pixel
// off, and with a filled, an outlined and a rounded shape, so each part of a
// shape's style is drawn. Snapping each edge is tested by the places part way
// through a pixel.
constexpr float TEST_GLYPH_GRID = 10.0f;
constexpr std::array<GlyphShape, 3> TEST_GLYPH_SHAPES{{
    {.x = 0.0f,
     .y = 0.5f,
     .width = 10.0f,
     .height = 4.0f,
     .fill = true,
     .radius = 1.0f},
    {.x = 2.0f,
     .y = 4.5f,
     .width = 6.0f,
     .height = 5.0f,
     .radius = 1.5f,
     .border_width = 1.2f},
    {.x = 4.5f, .y = 6.0f, .width = 1.5f, .height = 3.5f, .fill = true},
}};
constexpr Color TEST_GLYPH_COLOR{255, 200, 80, 255};

// `shapes` laid out as the trophy glyph was before glyphs were elements, a
// stack of boxes each padding one shape into its place, which flex snaps to
// whole pixels. A glyph must draw exactly what these draw.
AnyElement BoxGlyphReference(std::span<const GlyphShape> shapes, float grid,
                             Dp size, Color color) {
  const float unit = size.value / grid;
  std::vector<AnyElement> layers;
  for (const GlyphShape &shape : shapes) {
    const BoxStyles style{.fill = shape.fill ? color : Color{},
                          .border = shape.border_width > 0.0f ? color : Color{},
                          .border_width = Dp(shape.border_width * unit),
                          .radius = Dp(shape.radius * unit)};
    const std::array<AnyElement, 1> part{
        Box({.self = {.layout = {.width = Dp(shape.width * unit),
                                 .height = Dp(shape.height * unit)},
                      .styles = style}})};
    layers.push_back(
        Box({.layout = {.padding = DpInsets(Dp(shape.y * unit), Dp(), Dp(),
                                            Dp(shape.x * unit))}},
            part));
  }
  return Box({.layout = {.stacked = true},
              .self = {.layout = {.width = size, .height = size}}},
             layers);
}

// A glyph under test, how it is built, and the shapes, grid and colour the
// stack of boxes it is checked against is built from. The boxes come from the
// test's own copy of the shapes, so a mistake in the glyph's table shows.
struct GlyphUnderTest {
  AnyElement (*make)(Dp size, Color color);
  std::span<const GlyphShape> reference;
  float grid;
  Color color;
};

// The test glyph as a glyph element.
AnyElement MakeTestGlyph(Dp size, Color color) {
  return Glyph({.shapes = TEST_GLYPH_SHAPES,
                .grid = TEST_GLYPH_GRID,
                .size = size,
                .self = {.styles = {.color = color}}});
}

const GlyphUnderTest TEST_GLYPH{.make = &MakeTestGlyph,
                                .reference = TEST_GLYPH_SHAPES,
                                .grid = TEST_GLYPH_GRID,
                                .color = TEST_GLYPH_COLOR};

// The trophy glyph's parts as the boxes before it drew them, on its grid of 22
// with the cup half a unit down to centre it, each in the order it was
// stacked. Copied from the box glyph rather than read from the trophy's table.
constexpr float TROPHY_GRID = 22.0f;
constexpr std::array<GlyphShape, 6> TROPHY_REFERENCE{{
    {.x = 0.0f,
     .y = 2.5f,
     .width = 5.0f,
     .height = 6.0f,
     .radius = 2.0f,
     .border_width = 1.8f},
    {.x = 17.0f,
     .y = 2.5f,
     .width = 5.0f,
     .height = 6.0f,
     .radius = 2.0f,
     .border_width = 1.8f},
    {.x = 3.0f,
     .y = 9.5f,
     .width = 16.0f,
     .height = 4.0f,
     .fill = true,
     .radius = 2.0f},
    {.x = 3.0f, .y = 0.5f, .width = 16.0f, .height = 11.0f, .fill = true},
    {.x = 9.5f, .y = 13.5f, .width = 3.0f, .height = 5.0f, .fill = true},
    {.x = 6.0f,
     .y = 18.5f,
     .width = 10.0f,
     .height = 3.0f,
     .fill = true,
     .radius = 1.5f},
}};
constexpr Color TROPHY_TEST_COLOR{255, 207, 82, 255};

// The trophy glyph as the component builds it.
AnyElement MakeTrophyGlyph(Dp size, Color color) {
  return TrophyGlyph({.size = size, .self = {.styles = {.color = color}}});
}

const GlyphUnderTest TROPHY_GLYPH{.make = &MakeTrophyGlyph,
                                  .reference = TROPHY_REFERENCE,
                                  .grid = TROPHY_GRID,
                                  .color = TROPHY_TEST_COLOR};

// One way of drawing a glyph, its size, where its top left corner goes, the
// screen it is drawn on, and the opacity it is painted with.
struct GlyphCase {
  Dp size;
  Vec2 at;
  Vec2 screen;
  float opacity = 1.0f;
};

// What `under_test` draws as `glyph_case` says, as a glyph element or as the
// stack of boxes it replaces.
Drawn DrawGlyph(const GlyphUnderTest &under_test, const GlyphCase &glyph_case,
                bool as_boxes) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, glyph_case.screen.x,
                           glyph_case.screen.y);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const AnyElement glyph =
      as_boxes ? BoxGlyphReference(under_test.reference, under_test.grid,
                                   glyph_case.size, under_test.color)
               : under_test.make(glyph_case.size, under_test.color);
  const float side = ToPixels(glyph_case.size);
  (void)Measure(glyph, {side, side});
  Check(Arrange(glyph, {glyph_case.at.x, glyph_case.at.y, side, side}),
        "the glyph is placed");
  return PaintedDraw(glyph, glyph_case.opacity);
}

// Sizes and places where the shapes' edges snap differently, at the reference
// size, and one on a screen larger than the reference, where design units
// aren't pixels.
std::vector<GlyphCase> SnappingCases() {
  const ScaleRules &rules = DEFAULT_THEME.scale;
  const Vec2 reference{rules.reference_width, rules.reference_height};
  constexpr std::array<float, 2> SIZES{16.0f, 32.0f};
  constexpr std::array<Vec2, 2> PLACES{{{10.0f, 20.0f}, {10.5f, 20.25f}}};
  std::vector<GlyphCase> cases;
  for (const float size : SIZES) {
    for (const Vec2 place : PLACES) {
      cases.push_back({.size = Dp(size), .at = place, .screen = reference});
    }
  }
  constexpr Vec2 LARGE_SCREEN{2560.0f, 1440.0f};
  Check(ScaleContext(rules, LARGE_SCREEN.x, LARGE_SCREEN.y).Scale() != 1.0f,
        "the large screen scales design units");
  cases.push_back(
      {.size = Dp(SIZES[0]), .at = PLACES[1], .screen = LARGE_SCREEN});
  return cases;
}

// `under_test` draws exactly what its boxes draw in every snapping case. The
// boxes are checked to draw the shapes, so two that draw nothing don't pass.
void CheckMatchesBoxes(const GlyphUnderTest &under_test) {
  for (const GlyphCase &glyph_case : SnappingCases()) {
    const Drawn boxes = DrawGlyph(under_test, glyph_case, true);
    Check(!boxes.vertices.empty() && DrewColor(boxes, under_test.color),
          "the boxes draw the shapes in their colour");
    Check(SameDraw(DrawGlyph(under_test, glyph_case, false), boxes),
          "a glyph draws exactly what its shapes laid out as boxes draw");
  }
}

// `under_test` painted faded draws exactly what its boxes draw faded as much,
// and fading changes what it draws, so a glyph that ignored its fade wouldn't
// pass by matching boxes that ignored theirs.
void CheckFades(const GlyphUnderTest &under_test) {
  const ScaleRules &rules = DEFAULT_THEME.scale;
  constexpr float FADE = 0.5f;
  const GlyphCase faded{
      .size = Dp(32.0f),
      .at = {10.0f, 20.0f},
      .screen = {rules.reference_width, rules.reference_height},
      .opacity = FADE};
  const Drawn glyph = DrawGlyph(under_test, faded, false);
  Check(SameDraw(glyph, DrawGlyph(under_test, faded, true)),
        "a faded glyph draws exactly what its boxes draw faded");
  Check(!glyph.vertices.empty() && !DrewColor(glyph, under_test.color),
        "and fading it changes its colour");
}

// A glyph draws exactly what its shapes laid out as boxes draw, at sizes,
// places and scales where the shapes' edges snap differently.
void TestGlyphMatchesBoxes() { CheckMatchesBoxes(TEST_GLYPH); }

// A glyph fades exactly as its boxes do.
void TestGlyphFades() { CheckFades(TEST_GLYPH); }

// The trophy glyph draws exactly what the boxes it replaced drew, in every
// snapping case and faded, so swapping it in changes no pixel of the cup.
void TestTrophyGlyphMatchesBoxes() {
  CheckMatchesBoxes(TROPHY_GLYPH);
  CheckFades(TROPHY_GLYPH);
}

// A glyph keeps its size in a row too narrow for it, as the stack of boxes it
// replaces did, so swapping one for the other moves nothing around it. It sits
// in a box of no size of its own, which can shrink, so the box takes its least
// width from what the glyph says it needs, and both are checked.
void TestGlyphKeepsItsSize() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  constexpr float SIDE = 32.0f;
  constexpr float NARROW = SIDE / 2.0f;
  const auto width_in_narrow_row = [&](bool as_boxes) {
    FrameBuilder frame;
    frame.Begin();
    const FrameScope scope(frame, theme, scale, {}, {});
    const AnyElement glyph =
        as_boxes ? BoxGlyphReference(TEST_GLYPH_SHAPES, TEST_GLYPH_GRID,
                                     Dp(SIDE), TEST_GLYPH_COLOR)
                 : AnyElement(Glyph(
                       {.shapes = TEST_GLYPH_SHAPES,
                        .grid = TEST_GLYPH_GRID,
                        .size = Dp(SIDE),
                        .self = {.styles = {.color = TEST_GLYPH_COLOR}}}));
    const std::array<AnyElement, 1> held{glyph};
    const BoxElement holder = Box({}, held);
    const std::array<AnyElement, 1> inside{holder};
    const BoxElement row =
        Box({.self = {.layout = {.width = Dp(NARROW)}}}, inside);
    (void)Measure(row, {NARROW, SIDE});
    Check(Arrange(row, {0.0f, 0.0f, NARROW, SIDE}), "the row is placed");
    return Vec2{frame.LayoutOf(glyph)->arranged.width,
                frame.LayoutOf(holder)->arranged.width};
  };
  const Vec2 boxes = width_in_narrow_row(true);
  Check(boxes.x == SIDE && boxes.y == SIDE,
        "the boxes and what holds them keep their size in a row too narrow");
  const Vec2 glyph = width_in_narrow_row(false);
  Check(glyph.x == boxes.x && glyph.y == boxes.y,
        "and so do the glyph and what holds it");
}

// What `build` draws, built in a frame for a screen of `screen` pixels, the
// reference size when it is none, and placed with its top left corner at
// `at`, as a glyph `size` on a side, painted with `opacity`.
template <class Build>
Drawn DrawBuilt(Build &&build, Dp size, Vec2 at, float opacity = 1.0f,
                Vec2 screen = {}) {
  const ThemeContext &theme = DEFAULT_THEME;
  const bool reference = screen.x <= 0.0f || screen.y <= 0.0f;
  const ScaleContext scale(theme.scale,
                           reference ? theme.scale.reference_width : screen.x,
                           reference ? theme.scale.reference_height : screen.y);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const AnyElement glyph = build();
  const float side = ToPixels(size);
  (void)Measure(glyph, {side, side});
  Check(Arrange(glyph, {at.x, at.y, side, side}), "the glyph is placed");
  return PaintedDraw(glyph, opacity);
}

// What a glyph of `shapes` on a grid of 20 draws, in the test colour with
// `mark` as its mark colour, on a screen of `screen` pixels, the reference
// size when it is none.
Drawn DrawShapes(GlyphShapes shapes, Dp size, Vec2 at, float opacity = 1.0f,
                 Color mark = {}, Vec2 screen = {}) {
  constexpr float SHAPE_TEST_GRID = 20.0f;
  return DrawBuilt(
      [&] {
        return Glyph(
            {.shapes = shapes,
             .grid = SHAPE_TEST_GRID,
             .size = size,
             .self = {.styles = {.color = TEST_GLYPH_COLOR, .mark = mark}}});
      },
      size, at, opacity, screen);
}

// What `draw` adds to a draw list set up as PaintedDraw sets one up, the
// expected drawing a test compares a glyph with.
template <class Draw> Drawn ExpectedDraw(Draw &&draw) {
  ImDrawListSharedData shared;
  shared.ClipRectFullscreen = ImVec4(FULLSCREEN.x, FULLSCREEN.y,
                                     FULLSCREEN.Right(), FULLSCREEN.Bottom());
  shared.SetCircleTessellationMaxError(TESSELLATION);
  ImDrawList list(&shared);
  list._ResetForNewFrame();
  list.Flags =
      ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;
  list.PushClipRectFullScreen();
  list.PushTexture(ImTextureRef());
  draw(list);
  Drawn drawn{.vertices = {list.VtxBuffer.begin(), list.VtxBuffer.end()},
              .indices = {list.IdxBuffer.begin(), list.IdxBuffer.end()}};
  for (const ImDrawCmd &command : list.CmdBuffer) {
    if (command.ElemCount > 0) {
      drawn.commands.push_back(
          {.elements = command.ElemCount, .clip = command.ClipRect});
    }
  }
  return drawn;
}

// The alpha of a vertex fully covered by a shape. Smoothed edges fade out.
constexpr ImU32 OPAQUE_ALPHA = 0xFFu;

// Whether a vertex is fully opaque, inside a shape rather than on the edge
// ImGui smooths.
bool Opaque(const ImDrawVert &vertex) {
  return ((vertex.col >> IM_COL32_A_SHIFT) & OPAQUE_ALPHA) == OPAQUE_ALPHA;
}

// Whether `point` lies in a triangle `drawn` drew whose corners are all fully
// opaque, so it is covered by a shape and not only by its smoothed edge.
bool Covered(const Drawn &drawn, Vec2 point) {
  const auto side = [](ImVec2 from, ImVec2 to, Vec2 at) {
    return (to.x - from.x) * (at.y - from.y) -
           (to.y - from.y) * (at.x - from.x);
  };
  for (size_t index = 0; index + 2 < drawn.indices.size(); index += 3) {
    const ImDrawVert &first = drawn.vertices[drawn.indices[index]];
    const ImDrawVert &second = drawn.vertices[drawn.indices[index + 1]];
    const ImDrawVert &third = drawn.vertices[drawn.indices[index + 2]];
    if (!Opaque(first) || !Opaque(second) || !Opaque(third)) {
      continue;
    }
    const float one = side(first.pos, second.pos, point);
    const float two = side(second.pos, third.pos, point);
    const float three = side(third.pos, first.pos, point);
    if ((one >= 0.0f && two >= 0.0f && three >= 0.0f) ||
        (one <= 0.0f && two <= 0.0f && three <= 0.0f)) {
      return true;
    }
  }
  return false;
}

// Whether every vertex `drawn` holds lies within `rect` grown by the half
// pixel ImGui smooths an edge over, so the shape stays inside `rect`.
bool StaysInside(const Drawn &drawn, const Rect &rect) {
  // Half a pixel, and a little more, as ImGui stretches the smoothed edge at a
  // curve's corners slightly past half a pixel. A stroke straddling the edge
  // reaches a whole pixel or more past it.
  constexpr float SMOOTHED = 0.6f;
  return std::all_of(drawn.vertices.begin(), drawn.vertices.end(),
                     [&](const ImDrawVert &vertex) {
                       return vertex.pos.x >= rect.x - SMOOTHED &&
                              vertex.pos.x <= rect.Right() + SMOOTHED &&
                              vertex.pos.y >= rect.y - SMOOTHED &&
                              vertex.pos.y <= rect.Bottom() + SMOOTHED;
                     });
}

// The shapes the ink test draws, a filled square in each ink.
constexpr std::array<GlyphShape, 1> COLOR_INK_SQUARE{
    {{.x = 2.0f, .y = 2.0f, .width = 12.0f, .height = 12.0f, .fill = true}}};
constexpr std::array<GlyphShape, 1> MARK_INK_SQUARE{{{.x = 2.0f,
                                                      .y = 2.0f,
                                                      .width = 12.0f,
                                                      .height = 12.0f,
                                                      .fill = true,
                                                      .ink = GlyphInk::Mark}}};

// A shape in mark ink is drawn in the glyph's mark colour and never in its own
// colour, and the same shape in colour ink the other way round, so the ink
// alone picks the colour.
void TestGlyphInk() {
  constexpr Color MARK{20, 40, 60, 255};
  const Drawn colored =
      DrawShapes(COLOR_INK_SQUARE, Dp(32.0f), {10.0f, 20.0f}, 1.0f, MARK);
  Check(DrewColor(colored, TEST_GLYPH_COLOR) && !DrewColor(colored, MARK),
        "a shape in colour ink draws the glyph's colour");
  const Drawn marked =
      DrawShapes(MARK_INK_SQUARE, Dp(32.0f), {10.0f, 20.0f}, 1.0f, MARK);
  Check(DrewColor(marked, MARK) && !DrewColor(marked, TEST_GLYPH_COLOR),
        "and one in mark ink draws the mark colour instead");
}

// Circles on a grid of 20, filled and outlined, and a fully rounded square.
constexpr std::array<GlyphShape, 1> FILLED_CIRCLE{
    {{.x = 1.0f,
      .y = 1.0f,
      .width = 18.0f,
      .height = 18.0f,
      .fill = true,
      .kind = GlyphShapeKind::Circle}}};
constexpr std::array<GlyphShape, 1> RING{{{.x = 1.0f,
                                           .y = 1.0f,
                                           .width = 18.0f,
                                           .height = 18.0f,
                                           .border_width = 1.25f,
                                           .kind = GlyphShapeKind::Circle}}};
constexpr std::array<GlyphShape, 1> ROUND_SQUARE{{{.x = 1.0f,
                                                   .y = 1.0f,
                                                   .width = 18.0f,
                                                   .height = 18.0f,
                                                   .fill = true,
                                                   .radius = 9.0f}}};

// A circle is ImGui's circle in its snapped square, and a ring's stroke lies
// just inside it, as a box's border does. At 32 on a grid of 20 a unit is 1.6
// pixels, so the square starts 2 pixels in and is 29 across, centred 14.5 in,
// and the ring is 2 pixels thick, its path half that in, plus the half pixel
// AddCircle takes off. Each is drawn in segments that stray at most a tenth of
// a pixel from its curve, by ImGui's rule of a half turn over the angle whose
// cosine is one less the allowance over the radius, rounded up to an even
// count, 28 for the circle of 14.5 and 26 for the ring's path of 13.5, where
// ImGui's own allowance of 0.3 would give the circle 16. The values are worked
// out here by hand. A fully rounded square, which ImGui leaves flat sides on,
// draws differently, so the check can tell a circle from one.
void TestGlyphCircle() {
  constexpr Vec2 AT{10.0f, 20.0f};
  constexpr ImVec2 CENTRE{26.5f, 36.5f};
  constexpr float RADIUS = 14.5f;
  constexpr float RING_PATH = 14.0f;
  constexpr float RING_WIDTH = 2.0f;
  constexpr int CIRCLE_SEGMENTS = 28;
  constexpr int RING_SEGMENTS = 26;
  const ImU32 color = IM_COL32(TEST_GLYPH_COLOR.r, TEST_GLYPH_COLOR.g,
                               TEST_GLYPH_COLOR.b, TEST_GLYPH_COLOR.a);
  const Drawn filled = DrawShapes(FILLED_CIRCLE, Dp(32.0f), AT);
  Check(SameDraw(filled, ExpectedDraw([&](ImDrawList &list) {
                   list.AddCircleFilled(CENTRE, RADIUS, color, CIRCLE_SEGMENTS);
                 })),
        "a filled circle is ImGui's circle in its snapped square");
  Check(!SameDraw(filled, DrawShapes(ROUND_SQUARE, Dp(32.0f), AT)),
        "which a fully rounded square doesn't draw");
  Check(SameDraw(DrawShapes(RING, Dp(32.0f), AT),
                 ExpectedDraw([&](ImDrawList &list) {
                   list.AddCircle(CENTRE, RING_PATH, color, RING_SEGMENTS,
                                  RING_WIDTH);
                 })),
        "and a ring's stroke lies just inside the circle");
}

// Triangles on a grid of 20, filled and outlined, with rounded corners.
constexpr std::array<GlyphShape, 1> FILLED_TRIANGLE{
    {{.x = 0.0f,
      .y = 1.0f,
      .width = 20.0f,
      .height = 18.0f,
      .fill = true,
      .radius = 2.0f,
      .kind = GlyphShapeKind::Triangle}}};
constexpr std::array<GlyphShape, 1> OUTLINED_TRIANGLE{
    {{.x = 0.0f,
      .y = 1.0f,
      .width = 20.0f,
      .height = 18.0f,
      .radius = 2.0f,
      .border_width = 1.25f,
      .kind = GlyphShapeKind::Triangle}}};

// A triangle points up from its snapped rectangle, reaching its base and its
// top, with its bottom corners rounded off and nothing outside its sloping
// sides, and an outlined one is hollow with its stroke inside the rectangle.
// At 64 on a grid of 20 a unit is 3.2 pixels, so the rectangle runs from 10 to
// 74 across and from 23 to 81 down, the corners are 6 round and the outline 4
// thick. Together the points checked tell it from a rectangle and from a
// sharp triangle.
void TestGlyphTriangle() {
  constexpr Vec2 AT{10.0f, 20.0f};
  const Rect bounds{10.0f, 23.0f, 64.0f, 58.0f};
  const Drawn filled = DrawShapes(FILLED_TRIANGLE, Dp(64.0f), AT);
  Check(Covered(filled, {42.0f, 65.0f}) && Covered(filled, {42.0f, 24.0f}) &&
            Covered(filled, {42.0f, 80.0f}),
        "a filled triangle covers its middle and reaches its top and base");
  Check(!Covered(filled, {12.0f, 80.0f}),
        "with its bottom corner rounded off where a sharp one would cover");
  Check(!Covered(filled, {20.0f, 40.0f}),
        "and nothing outside its slope, which a rectangle would cover");
  Check(StaysInside(filled, bounds), "all inside its rectangle");
  const Drawn outlined = DrawShapes(OUTLINED_TRIANGLE, Dp(64.0f), AT);
  Check(!Covered(outlined, {42.0f, 65.0f}) && Covered(outlined, {42.0f, 79.0f}),
        "an outlined triangle is hollow, its stroke along its base");
  Check(StaysInside(outlined, bounds),
        "with its stroke inside the rectangle, not straddling its edge");
  const Drawn faded = DrawShapes(FILLED_TRIANGLE, Dp(64.0f), AT, 0.5f);
  Check(!faded.vertices.empty() && !DrewColor(faded, TEST_GLYPH_COLOR),
        "and it fades with its glyph");
}

// Triangles asking for no rounding when outlined and for far more rounding
// than fits when filled.
constexpr std::array<GlyphShape, 1> SHARP_OUTLINED_TRIANGLE{
    {{.x = 0.0f,
      .y = 1.0f,
      .width = 20.0f,
      .height = 18.0f,
      .border_width = 1.25f,
      .kind = GlyphShapeKind::Triangle}}};
constexpr std::array<GlyphShape, 1> OVER_ROUND_TRIANGLE{
    {{.x = 0.0f,
      .y = 1.0f,
      .width = 20.0f,
      .height = 18.0f,
      .fill = true,
      .radius = 50.0f,
      .kind = GlyphShapeKind::Triangle}}};

// A sharp outlined triangle keeps its stroke inside its rectangle, where a
// stroke round sharp corners would be mitred past them, and a triangle asking
// for more rounding than fits is rounded only as far as fits, still inside its
// rectangle and still covering its middle. The rectangle is the one the
// triangle test works out.
void TestGlyphTriangleLimits() {
  constexpr Vec2 AT{10.0f, 20.0f};
  const Rect bounds{10.0f, 23.0f, 64.0f, 58.0f};
  const Drawn sharp = DrawShapes(SHARP_OUTLINED_TRIANGLE, Dp(64.0f), AT);
  Check(!sharp.vertices.empty() && StaysInside(sharp, bounds),
        "a sharp outlined triangle keeps its stroke inside its rectangle");
  const Drawn round = DrawShapes(OVER_ROUND_TRIANGLE, Dp(64.0f), AT);
  Check(Covered(round, {42.0f, 60.0f}) && StaysInside(round, bounds),
        "and one asking for too much rounding stays inside, covering its "
        "middle");
}

// A ring and a triangle, each with a bar down its middle in mark ink, as the
// status glyphs are drawn.
constexpr std::array<GlyphPoint, 2> MIDDLE_BAR{{{10.0f, 6.0f}, {10.0f, 14.0f}}};
constexpr std::array<GlyphPoint, 2> LOW_BAR{{{10.0f, 9.0f}, {10.0f, 11.5f}}};
constexpr std::array<GlyphShape, 2> RING_WITH_BAR{
    {{.x = 1.0f,
      .y = 1.0f,
      .width = 18.0f,
      .height = 18.0f,
      .border_width = 1.25f,
      .kind = GlyphShapeKind::Circle},
     {.border_width = 2.0f,
      .ink = GlyphInk::Mark,
      .kind = GlyphShapeKind::Stroke,
      .points = MIDDLE_BAR}}};
constexpr std::array<GlyphShape, 2> TRIANGLE_WITH_BAR{
    {{.x = 0.0f,
      .y = 1.0f,
      .width = 20.0f,
      .height = 18.0f,
      .radius = 2.0f,
      .border_width = 1.25f,
      .kind = GlyphShapeKind::Triangle},
     {.border_width = 2.0f,
      .ink = GlyphInk::Mark,
      .kind = GlyphShapeKind::Stroke,
      .points = LOW_BAR}}};

// The middle across of the fully opaque vertices `drawn` holds in `color`.
float OpaqueMiddle(const Drawn &drawn, Color color) {
  constexpr ImU32 RGB = 0x00FFFFFFu;
  const ImU32 wanted = IM_COL32(color.r, color.g, color.b, 0) & RGB;
  float left = std::numeric_limits<float>::infinity();
  float right = -std::numeric_limits<float>::infinity();
  for (const ImDrawVert &vertex : drawn.vertices) {
    if (Opaque(vertex) && (vertex.col & RGB) == wanted) {
      left = std::min(left, vertex.pos.x);
      right = std::max(right, vertex.pos.x);
    }
  }
  return (left + right) / 2.0f;
}

// A mark centres across its ring or triangle exactly, at both status sizes, on
// screens that scale design units by 1, by a third more and by a half, placed
// on a whole pixel and part way through one. A 32 triangle is 32 pixels wide
// at the reference size and its 3 pixel bar can't centre in it, so the
// triangle gives up a pixel, and at a scale of 1.5 a ring's own snapping and
// its bar's would part by a pixel unless both centre on the same point.
void TestGlyphMarksCentre() {
  constexpr Color MARK{20, 40, 60, 255};
  constexpr float SAME = 0.01f;
  constexpr std::array<float, 2> SIZES{16.0f, 32.0f};
  constexpr std::array<Vec2, 3> SCREENS{
      {{1920.0f, 1080.0f}, {2560.0f, 1440.0f}, {2880.0f, 1620.0f}}};
  constexpr std::array<Vec2, 2> PLACES{{{10.0f, 20.0f}, {10.5f, 20.25f}}};
  for (const float size : SIZES) {
    for (const Vec2 screen : SCREENS) {
      for (const Vec2 place : PLACES) {
        for (const GlyphShapes shapes :
             {GlyphShapes(RING_WITH_BAR), GlyphShapes(TRIANGLE_WITH_BAR)}) {
          const Drawn drawn =
              DrawShapes(shapes, Dp(size), place, 1.0f, MARK, screen);
          Check(std::fabs(OpaqueMiddle(drawn, TEST_GLYPH_COLOR) -
                          OpaqueMiddle(drawn, MARK)) < SAME,
                "a mark centres across its ring or triangle exactly");
        }
      }
    }
  }
}

// Strokes on a grid of 20, a line across and a dot, 2 units thick.
constexpr std::array<GlyphPoint, 2> LINE_POINTS{
    {{5.0f, 10.0f}, {15.0f, 10.0f}}};
constexpr std::array<GlyphPoint, 1> DOT_POINT{{{10.0f, 10.0f}}};
constexpr std::array<GlyphShape, 1> LINE{{{.border_width = 2.0f,
                                           .kind = GlyphShapeKind::Stroke,
                                           .points = LINE_POINTS}}};
constexpr std::array<GlyphShape, 1> DOT{{{.border_width = 2.0f,
                                          .kind = GlyphShapeKind::Stroke,
                                          .points = DOT_POINT}}};
constexpr std::array<GlyphShape, 1> THIN_LINE{
    {{.kind = GlyphShapeKind::Stroke, .points = LINE_POINTS}}};

// The least height of the fully opaque vertices `drawn` holds, where a shape's
// top edge is, half a pixel inside its smoothed edge.
float OpaqueTop(const Drawn &drawn) {
  float top = std::numeric_limits<float>::infinity();
  for (const ImDrawVert &vertex : drawn.vertices) {
    if (Opaque(vertex)) {
      top = std::min(top, vertex.pos.y);
    }
  }
  return top;
}

// A stroke has round ends, covering past its end points as far as half its
// thickness but not the corners a square end would, and a single point is a
// round dot. Its edges land on whole pixels wherever the glyph is placed. At 64
// on a grid of 20 the stroke is 6 pixels thick, and the line runs from 26 to
// 58 across at 52 down, its top edge on pixel 49, whose smoothing leaves the
// opaque part from 49.5.
void TestGlyphStroke() {
  constexpr Vec2 AT{10.0f, 20.0f};
  constexpr Vec2 PART_PIXEL{10.5f, 20.25f};
  constexpr float SNAPPED_TOP = 49.5f;
  constexpr float SAME = 0.001f;
  const Drawn line = DrawShapes(LINE, Dp(64.0f), AT);
  Check(Covered(line, {42.0f, 52.0f}) && !Covered(line, {42.0f, 56.0f}),
        "a stroke covers its line and not beside it");
  Check(Covered(line, {60.0f, 52.0f}),
        "its end is round, reaching past its last point");
  Check(!Covered(line, {60.3f, 54.3f}),
        "without the corner a square end would cover");
  Check(std::fabs(OpaqueTop(DrawShapes(LINE, Dp(64.0f), PART_PIXEL)) -
                  SNAPPED_TOP) < SAME,
        "placed part way through a pixel, its edge still lands on one");
  const Drawn dot = DrawShapes(DOT, Dp(64.0f), AT);
  Check(Covered(dot, {42.0f, 52.0f}) && !Covered(dot, {44.6f, 54.6f}),
        "a single point is a round dot");
  Check(DrawShapes(THIN_LINE, Dp(64.0f), AT).vertices.empty(),
        "a stroke of no thickness draws nothing, though borders never go "
        "below a pixel");
  const Drawn faded = DrawShapes(LINE, Dp(64.0f), AT, 0.5f);
  Check(!faded.vertices.empty() && !DrewColor(faded, TEST_GLYPH_COLOR),
        "and it fades with its glyph");
}

// The status glyphs' shapes, copied from what they were designed as rather
// than read from their tables, on a grid of 20 with a circle 18 across or a
// triangle 20 by 18, outlined 1.25 thick, and marks 2 thick.
constexpr std::array<GlyphPoint, 1> COPY_INFO_DOT{{{10.0f, 6.25f}}};
constexpr std::array<GlyphPoint, 2> COPY_INFO_BAR{
    {{10.0f, 9.75f}, {10.0f, 13.75f}}};
constexpr std::array<GlyphPoint, 2> COPY_ERROR_BAR{
    {{10.0f, 6.0f}, {10.0f, 10.0f}}};
constexpr std::array<GlyphPoint, 1> COPY_ERROR_DOT{{{10.0f, 14.0f}}};
constexpr std::array<GlyphPoint, 2> COPY_WARNING_BAR{
    {{10.0f, 9.0f}, {10.0f, 11.5f}}};
constexpr std::array<GlyphPoint, 1> COPY_WARNING_DOT{{{10.0f, 15.25f}}};
constexpr std::array<GlyphPoint, 3> COPY_CHECK{
    {{5.75f, 10.0f}, {8.75f, 13.0f}, {14.25f, 7.0f}}};

// A status glyph's circle or triangle in the test's copy.
constexpr GlyphShape CopyBody(bool triangle, bool filled) {
  return {.x = triangle ? 0.0f : 1.0f,
          .y = 1.0f,
          .width = triangle ? 20.0f : 18.0f,
          .height = 18.0f,
          .fill = filled,
          .radius = triangle ? 2.0f : 0.0f,
          .border_width = filled ? 0.0f : 1.25f,
          .kind = triangle ? GlyphShapeKind::Triangle : GlyphShapeKind::Circle};
}

// A status glyph's mark in the test's copy.
constexpr GlyphShape CopyMark(GlyphPoints points, bool filled) {
  return {.border_width = 2.0f,
          .ink = filled ? GlyphInk::Mark : GlyphInk::Color,
          .kind = GlyphShapeKind::Stroke,
          .points = points};
}

constexpr std::array<GlyphShape, 3> COPY_INFO_OUTLINE{
    {CopyBody(false, false), CopyMark(COPY_INFO_DOT, false),
     CopyMark(COPY_INFO_BAR, false)}};
constexpr std::array<GlyphShape, 3> COPY_INFO_FILLED{
    {CopyBody(false, true), CopyMark(COPY_INFO_DOT, true),
     CopyMark(COPY_INFO_BAR, true)}};
constexpr std::array<GlyphShape, 3> COPY_WARNING_OUTLINE{
    {CopyBody(true, false), CopyMark(COPY_WARNING_BAR, false),
     CopyMark(COPY_WARNING_DOT, false)}};
constexpr std::array<GlyphShape, 3> COPY_WARNING_FILLED{
    {CopyBody(true, true), CopyMark(COPY_WARNING_BAR, true),
     CopyMark(COPY_WARNING_DOT, true)}};
constexpr std::array<GlyphShape, 3> COPY_ERROR_OUTLINE{
    {CopyBody(false, false), CopyMark(COPY_ERROR_BAR, false),
     CopyMark(COPY_ERROR_DOT, false)}};
constexpr std::array<GlyphShape, 3> COPY_ERROR_FILLED{
    {CopyBody(false, true), CopyMark(COPY_ERROR_BAR, true),
     CopyMark(COPY_ERROR_DOT, true)}};
constexpr std::array<GlyphShape, 2> COPY_SUCCESS_OUTLINE{
    {CopyBody(false, false), CopyMark(COPY_CHECK, false)}};
constexpr std::array<GlyphShape, 2> COPY_SUCCESS_FILLED{
    {CopyBody(false, true), CopyMark(COPY_CHECK, true)}};

// A status glyph, its builder and the test's copy of its two tables.
struct StatusUnderTest {
  const char *name;
  Status status;
  GlyphShapes outline;
  GlyphShapes filled;
};

// Whether every vertex `drawn` holds is `color`, however faded its smoothed
// edge, so nothing in it is drawn in another colour.
bool OnlyColor(const Drawn &drawn, Color color) {
  constexpr ImU32 RGB = 0x00FFFFFFu;
  const ImU32 wanted = IM_COL32(color.r, color.g, color.b, 0) & RGB;
  return std::all_of(drawn.vertices.begin(), drawn.vertices.end(),
                     [wanted](const ImDrawVert &vertex) {
                       return (vertex.col & RGB) == wanted;
                     });
}

// One status glyph, outlined and filled, at 32 and at 16 part way through a
// pixel. It draws exactly its copied table, outlined only in its colour and
// filled with its mark in the theme's colour for content on a coloured fill,
// which the outline never draws, so the two differ only in fill.
void CheckStatusGlyph(const StatusUnderTest &status) {
  const Color on_accent = DEFAULT_THEME.colors.text.on_accent;
  constexpr float STATUS_GRID = 20.0f;
  const std::array<std::pair<Dp, Vec2>, 2> places{
      {{Dp(32.0f), {10.0f, 20.0f}}, {Dp(16.0f), {10.5f, 20.25f}}}};
  for (const auto &[size, at] : places) {
    for (const bool fill : {false, true}) {
      const Drawn drawn = DrawBuilt(
          [&] {
            return StatusGlyph(
                {.status = status.status,
                 .size = size,
                 .fill = fill,
                 .self = {.styles = {.color = TEST_GLYPH_COLOR}}});
          },
          size, at);
      const Drawn copy = DrawBuilt(
          [&] {
            return Glyph({.shapes = fill ? status.filled : status.outline,
                          .grid = STATUS_GRID,
                          .size = size,
                          .self = {.styles = {.color = TEST_GLYPH_COLOR,
                                              .mark = on_accent}}});
          },
          size, at);
      Check(SameDraw(drawn, copy), status.name);
      Check(DrewColor(drawn, TEST_GLYPH_COLOR),
            "a status glyph draws in its colour");
      Check(fill ? DrewColor(drawn, on_accent)
                 : OnlyColor(drawn, TEST_GLYPH_COLOR),
            "its mark in on_accent when filled and only its colour when not");
    }
  }
}

// Each status glyph draws the shapes it was designed as, outlined and filled.
void TestStatusGlyphs() {
  const std::array<StatusUnderTest, 4> statuses{{
      {"the info glyph draws its shapes", Status::Info, COPY_INFO_OUTLINE,
       COPY_INFO_FILLED},
      {"the warning glyph draws its shapes", Status::Warning,
       COPY_WARNING_OUTLINE, COPY_WARNING_FILLED},
      {"the error glyph draws its shapes", Status::Error, COPY_ERROR_OUTLINE,
       COPY_ERROR_FILLED},
      {"the success glyph draws its shapes", Status::Success,
       COPY_SUCCESS_OUTLINE, COPY_SUCCESS_FILLED},
  }};
  for (const StatusUnderTest &status : statuses) {
    CheckStatusGlyph(status);
  }
}

// Where the test glyph `SIDE` on a side lands with `layout` as its own, in a
// column wider than it that aligns its children as `column_align` says.
// The side of the glyph the layout tests place in a column, and the column's.
constexpr float GLYPH_IN_COLUMN_SIDE = 32.0f;
constexpr float GLYPH_COLUMN = 100.0f;

Rect GlyphInColumn(const ItemLayout &layout,
                   Align column_align = Align::Start) {
  constexpr float SIDE = GLYPH_IN_COLUMN_SIDE;
  constexpr float COLUMN = GLYPH_COLUMN;
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const AnyElement glyph = Glyph(
      {.shapes = TEST_GLYPH_SHAPES,
       .grid = TEST_GLYPH_GRID,
       .size = Dp(SIDE),
       .self = {.layout = layout, .styles = {.color = TEST_GLYPH_COLOR}}});
  const std::array<AnyElement, 1> held{glyph};
  const BoxElement column = Box(
      {.layout = {.direction = FlexDirection::Column, .align = column_align},
       .self = {.layout = {.width = Dp(COLUMN), .height = Dp(COLUMN)}}},
      held);
  (void)Measure(column, {COLUMN, COLUMN});
  Check(Arrange(column, {0.0f, 0.0f, COLUMN, COLUMN}), "the column is placed");
  return frame.LayoutOf(glyph)->arranged;
}

// A glyph takes what its own layout asks of its container, such as its own
// alignment, which the same glyph without it doesn't get, but always keeps its
// square, whatever width, height or main-axis limits the layout asks for, and
// isn't stretched across a column that stretches what it holds.
void TestGlyphSelfLayout() {
  constexpr float SIDE = GLYPH_IN_COLUMN_SIDE;
  constexpr float COLUMN = GLYPH_COLUMN;
  const Rect start = GlyphInColumn({});
  const Rect centred = GlyphInColumn({.align_self = Align::Center});
  Check(start.x == 0.0f && centred.x == (COLUMN - SIDE) / 2.0f,
        "a glyph takes the alignment its own layout asks for");
  const Rect asked =
      GlyphInColumn({.width = Dp(2.0f * SIDE), .height = Dp(2.0f * SIDE)});
  Check(asked.width == SIDE && asked.height == SIDE,
        "and keeps its square whatever size its layout asks for");
  Check(GlyphInColumn({}, Align::Stretch).width == SIDE,
        "and isn't stretched across a column that stretches its children");
  Check(GlyphInColumn({.max_main = Dp(SIDE / 2.0f)}).height == SIDE &&
            GlyphInColumn({.min_main = Dp(2.0f * SIDE)}).height == SIDE,
        "and no limit on its main axis changes its size");
}

// A toast asks of its container what its own layout says, through the card it
// is drawn on, so a toast given a width takes it, where the same toast without
// one hugs its content at the theme's narrowest toast.
void TestToastSelfLayout() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  constexpr float WIDTH = 400.0f;
  const float narrowest = ToastDefaults(theme).min_width.value;
  Check(narrowest != WIDTH, "the asked width differs from the narrowest");
  const auto width_of = [&](const ItemLayout &layout) {
    FrameBuilder frame;
    frame.Begin();
    const FrameScope scope(frame, theme, scale, {}, {});
    const BoxElement toast =
        Toast({.status = Status::Info, .self = {.layout = layout}});
    return Measure(toast,
                   {theme.scale.reference_width, theme.scale.reference_height})
        .x;
  };
  Check(width_of({}) == narrowest,
        "a toast with no width of its own is the theme's narrowest");
  Check(width_of({.width = Dp(WIDTH)}) == WIDTH,
        "and one given a width in its own layout takes it");
}

// Where `build`, given two fixed children of different widths, places them
// once the parent it returns is laid out in a square of BRACED_ROOM. The rects
// are default when a child wasn't placed.
constexpr float BRACED_ROOM = 400.0f;
template <class Build> std::array<Rect, 2> PlacedChildren(Build &&build) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const AnyElement first =
      Box({.self = {.layout = {.width = Dp(10.0f), .height = Dp(10.0f)}}});
  const AnyElement second =
      Box({.self = {.layout = {.width = Dp(20.0f), .height = Dp(10.0f)}}});
  const AnyElement parent = build(first, second);
  (void)Measure(parent, {BRACED_ROOM, BRACED_ROOM});
  if (!Arrange(parent, {0.0f, 0.0f, BRACED_ROOM, BRACED_ROOM})) {
    return {};
  }
  const auto placed = [&frame](AnyElement child) {
    const auto *layout = frame.LayoutOf(child);
    return layout != nullptr ? layout->arranged : Rect{};
  };
  return {placed(first), placed(second)};
}

// Box, Card and Toast given their children as a braced list place them
// exactly where the span form does. Each span form is checked to place both
// children, and the box's to place the second after the first, so a list form
// that dropped the children, or all but the first, couldn't match it. A bare
// box and one given an empty list both still build.
void TestBracedChildren() {
  const std::array<Rect, 2> box_span =
      PlacedChildren([](AnyElement first, AnyElement second) {
        const std::array<AnyElement, 2> children{first, second};
        return Box({}, children);
      });
  const std::array<Rect, 2> box_list = PlacedChildren(
      [](AnyElement first, AnyElement second) {
        return Box({}, {first, second});
      });
  Check(box_span[1].x > box_span[0].x && box_span[1].width > 0.0f,
        "the span form places the second child after the first");
  Check(box_list == box_span,
        "a box given a braced list places its children as the span form does");
  // Both span forms place both children, so a list form that dropped either
  // leaves a default rect that can't match.
  const auto both_placed = [](const std::array<Rect, 2> &rects) {
    return rects[0].width > 0.0f && rects[1].width > 0.0f;
  };
  const std::array<Rect, 2> card_span =
      PlacedChildren([](AnyElement first, AnyElement second) {
        const std::array<AnyElement, 2> children{first, second};
        return Card({}, children);
      });
  const std::array<Rect, 2> card_list =
      PlacedChildren([](AnyElement first, AnyElement second) {
        return Card({}, {first, second});
      });
  Check(both_placed(card_span) && card_list == card_span,
        "and so does a card");
  const std::array<Rect, 2> toast_span =
      PlacedChildren([](AnyElement first, AnyElement second) {
        const std::array<AnyElement, 2> children{first, second};
        return Toast({.status = Status::Info}, children);
      });
  const std::array<Rect, 2> toast_list =
      PlacedChildren([](AnyElement first, AnyElement second) {
        return Toast({.status = Status::Info}, {first, second});
      });
  Check(both_placed(toast_span) && toast_list == toast_span,
        "and so does a toast");
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  Check(!Box({}).IsNone() && !Box({}, {}).IsNone(),
        "a bare box and one given an empty list both build");
}

// Where `build`, given `count` fixed children each a different width, typed
// and untyped, places them once the parent it returns is laid out in a square
// of BRACED_ROOM. A rect is default when its child wasn't placed. The widths
// differ so children placed out of order land in other rects.
template <class Build>
std::vector<Rect> PlacedMany(size_t count, Build &&build) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  std::vector<BoxElement> typed;
  std::vector<AnyElement> any;
  for (size_t index = 0; index < count; ++index) {
    const float width = 5.0f + static_cast<float>(index);
    typed.push_back(
        Box({.self = {.layout = {.width = Dp(width), .height = Dp(10.0f)}}}));
    any.push_back(typed.back());
  }
  const AnyElement parent =
      build(std::span<const BoxElement>(typed),
            std::span<const AnyElement>(any));
  (void)Measure(parent, {BRACED_ROOM, BRACED_ROOM});
  std::vector<Rect> rects(count);
  if (Arrange(parent, {0.0f, 0.0f, BRACED_ROOM, BRACED_ROOM})) {
    for (size_t index = 0; index < count; ++index) {
      const auto *layout = frame.LayoutOf(any[index]);
      rects[index] = layout != nullptr ? layout->arranged : Rect{};
    }
  }
  return rects;
}

// Whether every child in `rects` was placed, each after the one before along
// a row, so a list that lost or reordered children can't match it.
bool PlacedInOrder(const std::vector<Rect> &rects) {
  for (size_t index = 0; index < rects.size(); ++index) {
    if (rects[index].width <= 0.0f ||
        (index > 0 && rects[index].x <= rects[index - 1].x)) {
      return false;
    }
  }
  return true;
}

// The typed half of what PlacedMany hands a builder, beside the untyped run.
using TypedRun = std::span<const BoxElement>;

// The span form of a box holding PlacedMany's children, the baseline every
// mixed list is compared with.
BoxElement SpanFormBox(TypedRun, std::span<const AnyElement> any) {
  return Box({}, any);
}

// A braced list can mix single elements, typed ones among them, with runs of
// them, and spreads every run in order among the singles, placing each child
// exactly where the span form of the same children does. An empty run adds
// nothing.
void TestMixedChildren() {
  using Typed = TypedRun;
  const auto span_form = SpanFormBox;
  const std::vector<Rect> three = PlacedMany(3, span_form);
  Check(PlacedInOrder(three), "the span form places three children in order");
  Check(PlacedMany(3,
                   [](Typed typed, std::span<const AnyElement> any) {
                     return Box({}, {typed[0], any.subspan(1)});
                   }) == three,
        "a single element then a run places them as the span form does");
  Check(PlacedMany(3,
                   [](Typed typed, std::span<const AnyElement> any) {
                     return Box({}, {any.first(2), typed[2]});
                   }) == three,
        "and so does a run then a single element");
  Check(PlacedMany(3,
                   [](Typed typed, std::span<const AnyElement>) {
                     return Box({}, {typed[0], std::span<const AnyElement>{},
                                     typed[1], typed[2]});
                   }) == three,
        "an empty run adds nothing");
}

// A mixed list is spread whole at the inline buffer's length, one past it
// where it first moves to the heap, and well past it, so an off by one at the
// switch drops or overruns a child where the span form doesn't.
void TestMixedChildrenLength() {
  for (const size_t count :
       {INLINE_CHILDREN, INLINE_CHILDREN + 1, INLINE_CHILDREN + 4}) {
    const std::vector<Rect> span = PlacedMany(count, SpanFormBox);
    Check(PlacedInOrder(span) &&
              PlacedMany(count,
                         [](TypedRun typed, std::span<const AnyElement> any) {
                           return Box({}, {typed[0], any.subspan(1)});
                         }) == span,
          "a list at or past the inline buffer is spread whole");
  }
}

// Card and Toast spread a mixed list as the span form of each does.
void TestMixedChildrenInComponents() {
  using Typed = TypedRun;
  const std::vector<Rect> card_span =
      PlacedMany(3, [](Typed, std::span<const AnyElement> any) {
        return Card({}, any);
      });
  Check(card_span[2].width > 0.0f &&
            PlacedMany(3,
                       [](Typed typed, std::span<const AnyElement> any) {
                         return Card({}, {typed[0], any.subspan(1)});
                       }) == card_span,
        "a card spreads a mixed list as the span form does");
  const std::vector<Rect> toast_span =
      PlacedMany(3, [](Typed, std::span<const AnyElement> any) {
        return Toast({.status = Status::Info}, any);
      });
  Check(toast_span[2].width > 0.0f &&
            PlacedMany(3,
                       [](Typed typed, std::span<const AnyElement> any) {
                         return Toast({.status = Status::Info},
                                      {typed[0], any.subspan(1)});
                       }) == toast_span,
        "and so does a toast");
}

// A component written with one Children parameter, as a caller would write
// one, putting its own `title` before the children it is given.
BoxElement Titled(AnyElement title, const Children &children) {
  return Box({}, {title, children});
}

// A component that takes `const Children&` once gets both forms. Children
// given to it in braces, as a span, or as a temporary vector all come out
// where the span form of the title and the same children puts them. A box
// given a temporary vector straight places it as the span form does too.
void TestChildrenForwarding() {
  const std::vector<Rect> three = PlacedMany(3, SpanFormBox);
  Check(PlacedInOrder(three), "the span form places three children in order");
  Check(PlacedMany(3,
                   [](TypedRun typed, std::span<const AnyElement>) {
                     return Titled(typed[0], {typed[1], typed[2]});
                   }) == three,
        "a component given braces passes them on as the span form does");
  Check(PlacedMany(3,
                   [](TypedRun typed, std::span<const AnyElement> any) {
                     return Titled(typed[0], any.subspan(1));
                   }) == three,
        "and so does one given a span");
  Check(PlacedMany(3,
                   [](TypedRun typed, std::span<const AnyElement> any) {
                     return Titled(typed[0], std::vector<AnyElement>(
                                                 any.begin() + 1, any.end()));
                   }) == three,
        "and one given a temporary vector");
  Check(PlacedMany(3,
                   [](TypedRun, std::span<const AnyElement> any) {
                     return Box({}, std::vector<AnyElement>(any.begin(),
                                                            any.end()));
                   }) == three,
        "and a box given a temporary vector straight");
  // A list given alone in braces is accepted, as the deleted copy is never
  // chosen, and places as the span form does.
  Check(PlacedMany(3,
                   [](TypedRun, std::span<const AnyElement> any) {
                     const Children all = any;
                     return Box({}, {all});
                   }) == three,
        "and a box given another list alone in braces");
}

// Each status has its own colour, the theme's colour of that role. The four
// are checked to differ first, so a mapping that sent every status to one
// colour couldn't pass.
void TestStatusColor() {
  const ThemeContext &theme = DEFAULT_THEME;
  const StatusColors &roles = theme.colors.status;
  Check(!(roles.info == roles.warning) && !(roles.info == roles.error) &&
            !(roles.info == roles.success) && !(roles.warning == roles.error) &&
            !(roles.warning == roles.success) &&
            !(roles.error == roles.success),
        "the theme's four status colours differ");
  Check(StatusColor(theme, Status::Info) == roles.info &&
            StatusColor(theme, Status::Warning) == roles.warning &&
            StatusColor(theme, Status::Error) == roles.error &&
            StatusColor(theme, Status::Success) == roles.success,
        "each status has the colour of its role");
}

// A status glyph given no colour is drawn in its status's colour, and not in
// another status's.
void TestStatusGlyphDefaultColor() {
  const ThemeContext &theme = DEFAULT_THEME;
  constexpr Dp SIZE = Dp(32.0f);
  constexpr Vec2 AT{10.0f, 20.0f};
  constexpr std::array<Status, 4> STATUSES{Status::Info, Status::Warning,
                                           Status::Error, Status::Success};
  for (const Status status : STATUSES) {
    const Drawn drawn = DrawBuilt(
        [status, SIZE] {
          return StatusGlyph({.status = status, .size = SIZE});
        },
        SIZE, AT);
    // TestStatusColor checks the four colours differ, so drawing only its own
    // tells this status from the others.
    const bool only_own =
        std::all_of(STATUSES.begin(), STATUSES.end(), [&](Status other) {
          return DrewColor(drawn, StatusColor(theme, other)) ==
                 (other == status);
        });
    Check(only_own,
          "a status glyph with no colour of its own has its status's colour");
  }
}

// A card's look and padding are its defaults, so what shows its defaults
// shows what it draws.
void TestCardReadsDefaults() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const CardTokens defaults = CardDefaults(theme);
  // Each default differs from an empty look, so a card that ignored them
  // couldn't match them by drawing nothing.
  Check(!(defaults.fill == Color{}) && !(defaults.border == Color{}) &&
            defaults.border_width != Dp() && defaults.radius != Dp() &&
            defaults.padding != Dp(),
        "the card's defaults differ from an empty look");
  const BoxElement card = Card({});
  const BoxProps *props = frame.FindAs<BoxProps>(card);
  Check(props != nullptr, "the card is a box");
  const BoxStyles &styles = props->self.styles;
  Check(styles.fill == defaults.fill && styles.border == defaults.border &&
            styles.border_width == defaults.border_width &&
            styles.radius == defaults.radius,
        "a card's look is its defaults");
  Check(HasSides(props->layout.padding.value_or(DpInsets{}), defaults.padding,
                 defaults.padding, defaults.padding, defaults.padding),
        "and its padding is its default padding on every side");
}

// A card casts the theme's low shadow. A box built the same way casts none, and
// the low shadow is checked to differ from none, so the card's shadow can only
// come from the card.
void TestCardCastsLowShadow() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  Check(!(theme.shadows.low == Shadow{}), "the low shadow differs from none");
  const BoxProps *card = frame.FindAs<BoxProps>(Card({}));
  const BoxProps *box = frame.FindAs<BoxProps>(Box({}));
  Check(card != nullptr && box != nullptr, "the card and the box are boxes");
  Check(box->self.styles.shadow == Shadow{}, "a plain box casts no shadow");
  Check(card->self.styles.shadow == theme.shadows.low,
        "and a card casts the theme's low shadow");
}

// How many vertices of `drawn` are fully `color`.
size_t VerticesIn(const Drawn &drawn, Color color) {
  const ImU32 wanted = IM_COL32(color.r, color.g, color.b, color.a);
  return static_cast<size_t>(std::count_if(
      drawn.vertices.begin(), drawn.vertices.end(),
      [wanted](const ImDrawVert &vertex) { return vertex.col == wanted; }));
}

// A box draws every layer of its shadow, each in its own colour. The layers are
// pure red and pure blue, which nothing else a bare box draws can be, and the
// same box with no shadow draws neither, so each colour shows its own layer.
void TestShadowDrawsEveryLayer() {
  constexpr Color RED = Rgb(0xFF0000);
  constexpr Color BLUE = Rgb(0x0000FF);
  constexpr Dp SIDE = Dp(64.0f);
  constexpr Vec2 AT{100.0f, 100.0f};
  const Shadow two = MakeShadow(
      {{.offset_y = Dp(4.0f), .blur = Dp(8.0f), .color = RED},
       {.offset_y = Dp(12.0f), .blur = Dp(16.0f), .color = BLUE}});
  const auto colours = [&](const Shadow &shadow) {
    const Drawn drawn = DrawBuilt(
        [&] { return Box({.self = {.styles = {.shadow = shadow}}}); }, SIDE,
        AT);
    return std::pair<size_t, size_t>{VerticesIn(drawn, RED),
                                     VerticesIn(drawn, BLUE)};
  };
  const auto [plain_red, plain_blue] = colours({});
  Check(plain_red == 0 && plain_blue == 0,
        "a box with no shadow draws neither layer's colour");
  const auto [red, blue] = colours(two);
  Check(red > 0, "a box draws its shadow's first layer");
  Check(blue > 0, "and its second layer too");
}

// Whether `value` is `expected` to well under a pixel, as ring points are
// worked out from corner arcs in floats.
bool NearPixel(float value, float expected) {
  constexpr float TOLERANCE = 0.01f;
  return std::abs(value - expected) < TOLERANCE;
}

// Where a layer's full-strength ring lies across, its leftmost and rightmost x.
// The ring is the shape grown by the spread, pulled in by the blur as CSS
// centres a blur on the edge, and moved by the offset.
std::pair<float, float> FullRingAcross(Dp spread, Dp offset_x) {
  constexpr Color RED = Rgb(0xFF0000);
  constexpr Dp SIDE = Dp(64.0f);
  constexpr Vec2 AT{100.0f, 100.0f};
  const Shadow shadow = MakeShadow({{.offset_x = offset_x,
                                     .blur = Dp(8.0f),
                                     .spread = spread,
                                     .color = RED}});
  const Drawn drawn = DrawBuilt(
      [&] { return Box({.self = {.styles = {.shadow = shadow}}}); }, SIDE, AT);
  const ImU32 wanted = IM_COL32(RED.r, RED.g, RED.b, RED.a);
  float left = std::numeric_limits<float>::infinity();
  float right = -std::numeric_limits<float>::infinity();
  for (const ImDrawVert &vertex : drawn.vertices) {
    if (vertex.col == wanted) {
      left = std::min(left, vertex.pos.x);
      right = std::max(right, vertex.pos.x);
    }
  }
  return {left, right};
}

// A layer's spread grows its shape, its offset moves it across, and its blur
// is centred on the edge, so the full-strength ring of a 64 wide box at 100
// with a blur of 8 starts 8 in from the edge with no spread or offset, and a
// spread of 4 and an offset of 6 move each side by the spread and the offset.
void TestShadowLayerGeometry() {
  constexpr float LEFT = 100.0f;
  constexpr float RIGHT = 164.0f;
  constexpr float BLUR = 8.0f;
  const auto [plain_left, plain_right] = FullRingAcross(Dp(0.0f), Dp(0.0f));
  Check(NearPixel(plain_left, LEFT + BLUR) && NearPixel(plain_right, RIGHT - BLUR),
        "a layer's full ring starts its blur in from the edge");
  constexpr float SPREAD = 4.0f;
  constexpr float ACROSS = 6.0f;
  const auto [moved_left, moved_right] =
      FullRingAcross(Dp(SPREAD), Dp(ACROSS));
  Check(NearPixel(moved_left, LEFT + BLUR - SPREAD + ACROSS) &&
            NearPixel(moved_right, RIGHT - BLUR + SPREAD + ACROSS),
        "and its spread grows it and its offset moves it across");
}

// A layer whose spread isn't a number is left out rather than drawn at NaN,
// while the layer beside it still draws. Without the guard its vertices would
// all be NaN, which the check finds, and the good layer shows the box still
// draws its shadow.
void TestShadowSkipsBrokenLayer() {
  constexpr Color RED = Rgb(0xFF0000);
  constexpr Color BLUE = Rgb(0x0000FF);
  const Shadow shadow = MakeShadow(
      {{.blur = Dp(8.0f),
        .spread = Dp(std::numeric_limits<float>::quiet_NaN()),
        .color = RED},
       {.blur = Dp(8.0f), .color = BLUE}});
  const Drawn drawn =
      DrawBuilt([&] { return Box({.self = {.styles = {.shadow = shadow}}}); },
                Dp(64.0f), Vec2{100.0f, 100.0f});
  const bool any_nan = std::any_of(
      drawn.vertices.begin(), drawn.vertices.end(),
      [](const ImDrawVert &vertex) {
        return std::isnan(vertex.pos.x) || std::isnan(vertex.pos.y);
      });
  Check(!any_nan && VerticesIn(drawn, RED) == 0,
        "a layer with a NaN spread draws nothing");
  Check(VerticesIn(drawn, BLUE) > 0, "and the layer beside it still draws");
}

// A shadow blends layer by layer with its state. Halfway to a hover shadow
// with one more layer, the shared layer is halfway between the two, and the
// extra layer is where the hover's is and half as strong, as it fades in from
// clear. At no hover and full hover it is each shadow exactly, so the halfway
// values can only come from the blend.
void TestShadowBlendsLayerByLayer() {
  const ThemeContext &theme = DEFAULT_THEME;
  constexpr Color DARK = Rgb(0x070707);
  BoxStyles styles{
      .shadow = MakeShadow(
          {{.offset_y = Dp(2.0f), .blur = Dp(4.0f), .color = DARK.WithAlpha(0.2f)}})};
  styles.states.hover.shadow = MakeShadow(
      {{.offset_y = Dp(10.0f), .blur = Dp(12.0f), .color = DARK.WithAlpha(0.4f)},
       {.offset_y = Dp(20.0f), .blur = Dp(30.0f), .color = DARK.WithAlpha(0.6f)}});
  Check(ResolveStyle(theme, styles, {}).shadow == styles.shadow,
        "with no hover the shadow is the resting one");
  Check(ResolveStyle(theme, styles, {.hover = 1.0f}).shadow ==
            *styles.states.hover.shadow,
        "and at full hover it is the hover one");
  const Shadow halfway = ResolveStyle(theme, styles, {.hover = 0.5f}).shadow;
  const ShadowLayer &hover_extra = styles.states.hover.shadow->layers[1];
  const auto near = [](uint8_t value, float expected) {
    return std::abs(static_cast<float>(value) - expected) <= 1.0f;
  };
  constexpr float MAX_CHANNEL = 255.0f;
  Check(halfway.count == 2, "halfway the shadow has both layers");
  Check(halfway.layers[0].offset_y == Dp(6.0f) &&
            halfway.layers[0].blur == Dp(8.0f) &&
            near(halfway.layers[0].color.a, 0.3f * MAX_CHANNEL),
        "the shared layer is halfway between the two");
  Check(halfway.layers[1].offset_y == hover_extra.offset_y &&
            halfway.layers[1].blur == hover_extra.blur &&
            near(halfway.layers[1].color.a, 0.3f * MAX_CHANNEL),
        "and the extra layer is in its place at half its strength");
}

// The glow tests draw a 64 unit box at 100, 100 with a glow in pure red, which
// nothing else a bare box draws can be.
constexpr Color GLOW_RED = Rgb(0xFF0000);
constexpr Dp GLOW_SIDE = Dp(64.0f);
constexpr Vec2 GLOW_AT{100.0f, 100.0f};

// Whether `vertex` is the glow's red at any strength.
bool IsGlowRed(const ImDrawVert &vertex) {
  constexpr ImU32 RGB_MASK = IM_COL32(255, 255, 255, 0);
  return (vertex.col & RGB_MASK) == IM_COL32(255, 0, 0, 0);
}

// The strength of `vertex`, its alpha from 0 to 255.
int AlphaOf(const ImDrawVert &vertex) {
  return static_cast<int>((vertex.col >> IM_COL32_A_SHIFT) & 0xFF);
}

// What a box drawn the glow tests' way with `glow` and `fill` draws, painted
// with `opacity`.
Drawn DrawGlowBox(const BoxGlow &glow, Color fill = {}, float opacity = 1.0f) {
  return DrawBuilt(
      [&] { return Box({.self = {.styles = {.fill = fill, .glow = glow}}}); },
      GLOW_SIDE, GLOW_AT, opacity);
}

// The strongest red the glow draws, 0 when it draws none.
int StrongestGlow(const Drawn &drawn) {
  int strongest = 0;
  for (const ImDrawVert &vertex : drawn.vertices) {
    if (IsGlowRed(vertex)) {
      strongest = std::max(strongest, AlphaOf(vertex));
    }
  }
  return strongest;
}

// A box's glow is a hollow halo of its own reach around it. It runs from the
// box's edge at full strength out to its reach, and draws nothing over the box,
// so a translucent fill isn't tinted. The same box with no glow draws no red,
// so every red vertex is the glow's.
void TestGlowDrawsHalo() {
  constexpr Dp REACH = Dp(16.0f);
  const float left = GLOW_AT.x;
  const float right = GLOW_AT.x + GLOW_SIDE.value;
  const float top = GLOW_AT.y;
  const float bottom = GLOW_AT.y + GLOW_SIDE.value;
  Check(StrongestGlow(DrawGlowBox({})) == 0, "a box with no glow draws no red");
  const Drawn drawn = DrawGlowBox({.glow = {REACH, 1.0f}, .color = GLOW_RED});
  float outer_left = std::numeric_limits<float>::infinity();
  float outer_right = -std::numeric_limits<float>::infinity();
  bool full_on_edge = true;
  size_t full = 0;
  for (const ImDrawVert &vertex : drawn.vertices) {
    if (!IsGlowRed(vertex)) {
      continue;
    }
    outer_left = std::min(outer_left, vertex.pos.x);
    outer_right = std::max(outer_right, vertex.pos.x);
    if (AlphaOf(vertex) == 255) {
      ++full;
      const bool on_edge =
          NearPixel(vertex.pos.x, left) || NearPixel(vertex.pos.x, right) ||
          NearPixel(vertex.pos.y, top) || NearPixel(vertex.pos.y, bottom);
      full_on_edge = full_on_edge && on_edge;
    }
  }
  Check(full > 0 && full_on_edge,
        "the glow is full strength on the box's edge");
  Check(NearPixel(outer_left, left - REACH.value) &&
            NearPixel(outer_right, right + REACH.value),
        "and reaches its own reach past the box on either side");
  // A filled halo would add triangles of red across the box, though no new
  // vertices, so the triangles are checked rather than the vertices.
  bool covers_box = false;
  for (size_t index = 0; index + 2 < drawn.indices.size(); index += 3) {
    const ImDrawVert &a = drawn.vertices[drawn.indices[index]];
    const ImDrawVert &b = drawn.vertices[drawn.indices[index + 1]];
    const ImDrawVert &c = drawn.vertices[drawn.indices[index + 2]];
    if (!IsGlowRed(a) || !IsGlowRed(b) || !IsGlowRed(c)) {
      continue;
    }
    const float x = (a.pos.x + b.pos.x + c.pos.x) / 3.0f;
    const float y = (a.pos.y + b.pos.y + c.pos.y) / 3.0f;
    covers_box = covers_box || (x > left && x < right && y > top && y < bottom);
  }
  Check(!covers_box, "and leaves the box itself uncovered");
}

// The glow's strength at the edge is its peak times the colour's alpha, and it
// fades with the box's opacity like everything else the box draws.
void TestGlowStrengthAndFade() {
  constexpr int FULL = 255;
  constexpr int HALF = 128;
  const BoxGlow full{.glow = {Dp(16.0f), 1.0f}, .color = GLOW_RED};
  const BoxGlow half{.glow = {Dp(16.0f), 0.5f}, .color = GLOW_RED};
  Check(StrongestGlow(DrawGlowBox(full)) == FULL,
        "a full glow is full strength at the edge");
  Check(std::abs(StrongestGlow(DrawGlowBox(half)) - HALF) <= 1,
        "a glow of half strength is half as strong");
  Check(std::abs(StrongestGlow(DrawGlowBox(full, {}, 0.5f)) - HALF) <= 1,
        "and a full glow on a box at half opacity is half as strong too");
}

// The glow goes under the box's body, so the fill covers its inner edge rather
// than the glow lying over the fill. The body is pure blue, which the glow
// can't be. The order is read from the triangles, which are drawn in the order
// they are listed, rather than from the vertices.
void TestGlowUnderBody() {
  constexpr Color BLUE = Rgb(0x0000FF);
  const Drawn drawn =
      DrawGlowBox({.glow = {Dp(16.0f), 1.0f}, .color = GLOW_RED}, BLUE);
  const ImU32 blue = IM_COL32(BLUE.r, BLUE.g, BLUE.b, BLUE.a);
  const auto first_index = [&drawn](auto &&wanted) {
    return std::find_if(drawn.indices.begin(), drawn.indices.end(),
                        [&](ImDrawIdx index) {
                          return wanted(drawn.vertices[index]);
                        });
  };
  const auto first_red = first_index(IsGlowRed);
  const auto first_blue = first_index(
      [blue](const ImDrawVert &vertex) { return vertex.col == blue; });
  Check(first_red != drawn.indices.end() && first_blue != drawn.indices.end(),
        "the box draws its glow and its fill");
  Check(first_red < first_blue, "and the glow before the fill");
}

// A glow with no reach, a reach that isn't a number, no strength or a clear
// colour draws exactly what no glow does, so a broken glow can't put NaN
// vertices on the screen.
void TestGlowSkipsBroken() {
  const Drawn none = DrawGlowBox({});
  const float nan = std::numeric_limits<float>::quiet_NaN();
  Check(SameDraw(DrawGlowBox({.glow = {Dp(0.0f), 1.0f}, .color = GLOW_RED}),
                 none),
        "a glow with no reach draws nothing");
  Check(SameDraw(DrawGlowBox({.glow = {Dp(nan), 1.0f}, .color = GLOW_RED}),
                 none),
        "a glow whose reach isn't a number draws nothing");
  Check(SameDraw(DrawGlowBox({.glow = {Dp(16.0f), 0.0f}, .color = GLOW_RED}),
                 none),
        "a glow with no strength draws nothing");
  Check(SameDraw(DrawGlowBox({.glow = {Dp(16.0f), 1.0f},
                              .color = GLOW_RED.WithAlpha(0.0f)}),
                 none),
        "a clear glow draws nothing");
}

// The shine tests draw a pure blue box with a held shine over it. The shine is
// the theme's, white, which a blue box never draws, so every white vertex is
// the shine's.
constexpr Color SHINE_BOX_BLUE = Rgb(0x0000FF);
// Degrees in half a turn, for turning the band's angle into radians.
constexpr float DEGREES_PER_HALF_TURN = 180.0f;

// How far the theme's band leans from vertical, in radians.
float ShineAngle() {
  return DEFAULT_THEME.shine.angle_degrees * std::numbers::pi_v<float> /
         DEGREES_PER_HALF_TURN;
}

// Whether `vertex` is the shine's white at any strength.
bool IsShine(const ImDrawVert &vertex) {
  constexpr ImU32 RGB_MASK = IM_COL32(255, 255, 255, 0);
  return (vertex.col & RGB_MASK) == IM_COL32(255, 255, 255, 0);
}

// Where `point` lies across the band's direction, the direction its lines are
// measured along.
float AlongShine(ImVec2 point) {
  const float angle = ShineAngle();
  return point.x * std::cos(angle) + point.y * std::sin(angle);
}

// What a blue box at `place` with corners of `radius` draws with a shine held
// at `position` over it, or with no shine when there is none, painted with
// `opacity` in `theme` at its reference size, where a design unit is a pixel.
Drawn DrawShineBox(std::optional<float> position, const Rect &place,
                   Dp radius = Dp(), float opacity = 1.0f,
                   const ThemeContext &theme = DEFAULT_THEME) {
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const BoxElement box = Box(
      {.self = {.styles = {.fill = SHINE_BOX_BLUE, .radius = radius}}});
  const AnyElement root =
      position
          ? AnyElement(Animate({.shine = Shine{.position = position}}, box))
          : AnyElement(box);
  (void)Measure(root, {place.width, place.height});
  Check(Arrange(root, place), "the shone box is placed");
  return PaintedDraw(root, opacity);
}

// The shine vertices of `drawn` that are at its strongest, the band's plateau
// and the edges where its fades reach full brightness.
std::vector<ImVec2> BrightestShine(const Drawn &drawn) {
  int strongest = 0;
  for (const ImDrawVert &vertex : drawn.vertices) {
    if (IsShine(vertex)) {
      strongest = std::max(strongest, AlphaOf(vertex));
    }
  }
  std::vector<ImVec2> brightest;
  for (const ImDrawVert &vertex : drawn.vertices) {
    if (IsShine(vertex) && strongest > 0 && AlphaOf(vertex) == strongest) {
      brightest.push_back(vertex.pos);
    }
  }
  return brightest;
}

// A shine stays inside the box's rounded shape, its corners included, so no
// light shows past a curved corner. The band at the middle is wider than the
// box, so it reaches every corner. Only lit vertices are checked, as the
// smoothed edge's outer vertices are clear. A box with no shine draws no
// white, so the white is the shine's.
void TestShineInsideShape() {
  constexpr Rect PLACE{100.0f, 100.0f, 64.0f, 64.0f};
  constexpr float RADIUS = 16.0f;
  // ImGui cuts a curve into straight pieces, which sit a little inside it.
  constexpr float TOLERANCE = 0.5f;
  const auto count = [](const Drawn &drawn) {
    return std::count_if(drawn.vertices.begin(), drawn.vertices.end(), IsShine);
  };
  Check(count(DrawShineBox(std::nullopt, PLACE, Dp(RADIUS))) == 0,
        "a box with no shine draws no white");
  const Drawn drawn = DrawShineBox(0.5f, PLACE, Dp(RADIUS));
  Check(count(drawn) > 0, "a shone box draws its shine");
  bool inside = true;
  for (const ImDrawVert &vertex : drawn.vertices) {
    if (!IsShine(vertex) || AlphaOf(vertex) == 0) {
      continue;
    }
    // The nearest point of the shape's inner rectangle, whose corners are the
    // centres of the rounded corners.
    const float x = std::clamp(vertex.pos.x, PLACE.x + RADIUS,
                               PLACE.Right() - RADIUS);
    const float y = std::clamp(vertex.pos.y, PLACE.y + RADIUS,
                               PLACE.Bottom() - RADIUS);
    inside = inside && std::hypot(vertex.pos.x - x, vertex.pos.y - y) <=
                           RADIUS + TOLERANCE;
  }
  Check(inside, "and every lit part of it lies inside the rounded shape");
}

// When a strip edge lies exactly along one of the box's straight sides, the
// strip just outside the box clips to nothing but points on that side, some of
// them repeated. Drawn, those repeated points gave ImGui's smoothed fill edges
// of no length, and it threw vertices far outside the box. Such a strip is
// left out. The band's sizes are set here rather than taken from the theme,
// and chosen so a vertical band halfway across has its plateau exactly as wide
// as the box, so the plateau's edges, which are strip edges however many
// strips each fade is cut into, lie along both sides. The test checks that
// first, so neither a change to the theme nor to how DrawShine cuts its fades
// can make it pass without testing anything. Every lit vertex is checked
// against the box.
void TestShineCutAlongSide() {
  constexpr Rect PLACE{100.0f, 100.0f, 64.0f, 64.0f};
  constexpr float RADIUS = 16.0f;
  constexpr float BAND = 80.0f;
  constexpr float PLATEAU = 0.8f;
  // The smoothed edge's lit vertices sit inside the outline, so any lit
  // vertex further out than this is thrown.
  constexpr float TOLERANCE = 0.5f;
  ThemeContext upright = DEFAULT_THEME;
  upright.shine.angle_degrees = 0.0f;
  upright.shine.width_fraction = 0.0f;
  upright.shine.min_width = Dp(BAND);
  upright.shine.plateau_fraction = PLATEAU;
  // An upright band's edges are x positions. Halfway, it is centred on the
  // box, and its plateau starts one fade in from its left edge.
  const float start = PLACE.x - BAND + (PLACE.width + BAND) * 0.5f;
  const float plateau_left = start + BAND * (1.0f - PLATEAU) / 2.0f;
  Check(NearPixel(plateau_left, PLACE.x) &&
            NearPixel(plateau_left + BAND * PLATEAU, PLACE.Right()),
        "the plateau's edges lie along the box's sides");
  const Drawn drawn =
      DrawShineBox(0.5f, PLACE, Dp(RADIUS), 1.0f, upright);
  bool inside = true;
  size_t lit = 0;
  for (const ImDrawVert &vertex : drawn.vertices) {
    if (!IsShine(vertex) || AlphaOf(vertex) == 0) {
      continue;
    }
    ++lit;
    inside = inside && vertex.pos.x >= PLACE.x - TOLERANCE &&
             vertex.pos.x <= PLACE.Right() + TOLERANCE &&
             vertex.pos.y >= PLACE.y - TOLERANCE &&
             vertex.pos.y <= PLACE.Bottom() + TOLERANCE;
  }
  Check(lit > 0 && inside,
        "a strip edge along a side throws no vertex outside the box");
}

// A shine with no strength, on a box at no opacity, in a clear colour or at a
// position that isn't a number draws exactly what no shine does. So does one
// drawn across a rectangle whose height isn't a number, which would otherwise
// put NaN vertices on the screen, as the clip keeps the corners it can't place.
void TestShineSkipsBroken() {
  constexpr Rect PLACE{100.0f, 100.0f, 600.0f, 64.0f};
  const Drawn none = DrawShineBox(std::nullopt, PLACE);
  ThemeContext faint = DEFAULT_THEME;
  faint.shine.peak_alpha = 0.0f;
  ThemeContext clear = DEFAULT_THEME;
  clear.colors.effect.shine = clear.colors.effect.shine.WithAlpha(0.0f);
  Check(SameDraw(DrawShineBox(0.5f, PLACE, Dp(), 1.0f, faint), none),
        "a shine with no strength draws nothing");
  Check(SameDraw(DrawShineBox(0.5f, PLACE, Dp(), 0.0f),
                 DrawShineBox(std::nullopt, PLACE, Dp(), 0.0f)),
        "a shine on a box at no opacity draws nothing");
  Check(SameDraw(DrawShineBox(0.5f, PLACE, Dp(), 1.0f, clear), none),
        "a clear shine draws nothing");
  Check(SameDraw(DrawShineBox(std::numeric_limits<float>::quiet_NaN(), PLACE),
                 none),
        "a shine whose position isn't a number draws nothing");
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const ShineStyle style{.color = theme.colors.effect.shine,
                         .angle_degrees = theme.shine.angle_degrees,
                         .width_fraction = theme.shine.width_fraction,
                         .min_width = theme.shine.min_width.value,
                         .peak_alpha = theme.shine.peak_alpha,
                         .plateau_fraction = theme.shine.plateau_fraction};
  const Rect broken{PLACE.x, PLACE.y, PLACE.width,
                    std::numeric_limits<float>::quiet_NaN()};
  Check(ExpectedDraw([&](ImDrawList &list) {
          DrawShine(list, broken, 0.0f, style, 0.5f);
        }).vertices.empty(),
        "and one across a rectangle whose height isn't a number draws nothing");
}

// The band's middle follows its position, at the box's middle halfway and
// further left a quarter of the way.
void TestShineFollowsPosition() {
  constexpr Rect PLACE{100.0f, 100.0f, 600.0f, 64.0f};
  const auto middle = [&](float position) {
    const std::vector<ImVec2> brightest =
        BrightestShine(DrawShineBox(position, PLACE));
    float total = 0.0f;
    for (const ImVec2 &point : brightest) {
      total += AlongShine(point);
    }
    return brightest.empty() ? 0.0f : total / brightest.size();
  };
  const float centre =
      AlongShine({PLACE.x + PLACE.width / 2.0f, PLACE.y + PLACE.height / 2.0f});
  const float half = middle(0.5f);
  Check(std::abs(half - centre) < 1.0f,
        "halfway, the band's middle is the box's middle");
  Check(middle(0.25f) < half - 1.0f,
        "and a quarter of the way it is further left");
}

// At 0 and 1 the band lies wholly off the box, so a sweep starts and ends
// showing nothing. Each draws exactly what no shine does. Just past 0 only the
// band's leading edge has reached the box, as it travels from wholly off one
// side to wholly off the other, so the lit part reaches no further into the
// box than that small step of its travel.
void TestShineAtRest() {
  constexpr Rect PLACE{100.0f, 100.0f, 600.0f, 64.0f};
  constexpr float STEP = 0.02f;
  // The smoothed edge reaches a little past the band.
  constexpr float TOLERANCE = 2.0f;
  const Drawn none = DrawShineBox(std::nullopt, PLACE);
  Check(SameDraw(DrawShineBox(0.0f, PLACE), none),
        "at 0 the band is wholly off the box's left");
  Check(SameDraw(DrawShineBox(1.0f, PLACE), none),
        "and at 1 wholly off its right");
  const float first = AlongShine({PLACE.x, PLACE.y});
  const float last = AlongShine({PLACE.Right(), PLACE.Bottom()});
  const float band = DEFAULT_THEME.shine.width_fraction * PLACE.width;
  float furthest = -std::numeric_limits<float>::infinity();
  for (const ImDrawVert &vertex : DrawShineBox(STEP, PLACE).vertices) {
    if (IsShine(vertex)) {
      furthest = std::max(furthest, AlongShine(vertex.pos));
    }
  }
  Check(std::isfinite(furthest) &&
            furthest - first <= (last - first + band) * STEP + TOLERANCE,
        "just past 0 only the band's leading edge has reached the box");
}

// The band leans right at the top by the theme's angle, so its bright edge is
// further right along the box's top than along its bottom by the box's height
// times the angle's tangent.
void TestShineSlant() {
  constexpr Rect PLACE{100.0f, 100.0f, 600.0f, 200.0f};
  constexpr float TOLERANCE = 0.5f;
  // ImGui's smoothed edge pulls a shape's lit vertices half a pixel inside its
  // outline, so those along the box's top and bottom are found within a pixel.
  constexpr float EDGE = 1.0f;
  const std::vector<ImVec2> brightest =
      BrightestShine(DrawShineBox(0.5f, PLACE));
  float top = std::numeric_limits<float>::infinity();
  float bottom = std::numeric_limits<float>::infinity();
  for (const ImVec2 &point : brightest) {
    if (std::abs(point.y - PLACE.y) < EDGE) {
      top = std::min(top, point.x);
    }
    if (std::abs(point.y - PLACE.Bottom()) < EDGE) {
      bottom = std::min(bottom, point.x);
    }
  }
  Check(std::isfinite(top) && std::isfinite(bottom),
        "the band's bright part reaches the box's top and bottom");
  const float angle = ShineAngle();
  Check(std::abs((top - bottom) - PLACE.height * std::tan(angle)) < TOLERANCE,
        "the band leans right at the top by the theme's angle");
}

// The band is the theme's strength at its brightest and fades with the box's
// opacity. It is the theme's share of the box's width, but never narrower
// than the theme's minimum, so a narrow box still gets a broad band.
void TestShineStrengthAndWidth() {
  const ShineTokens &shine = DEFAULT_THEME.shine;
  constexpr Rect WIDE{100.0f, 100.0f, 600.0f, 64.0f};
  constexpr Rect NARROW{100.0f, 100.0f, 200.0f, 64.0f};
  // The smoothed edge reaches a little past each end of the band.
  constexpr float TOLERANCE = 2.0f;
  const auto strongest = [&](float opacity) {
    int most = 0;
    for (const ImDrawVert &vertex :
         DrawShineBox(0.5f, WIDE, Dp(), opacity).vertices) {
      if (IsShine(vertex)) {
        most = std::max(most, AlphaOf(vertex));
      }
    }
    return most;
  };
  const int peak = static_cast<int>(std::lround(shine.peak_alpha * 255.0f));
  Check(std::abs(strongest(1.0f) - peak) <= 1,
        "the band is the theme's strength at its brightest");
  Check(std::abs(strongest(0.5f) - peak / 2) <= 1,
        "and half that on a box at half opacity");
  const auto band_width = [](const Rect &place) {
    float first = std::numeric_limits<float>::infinity();
    float last = -std::numeric_limits<float>::infinity();
    for (const ImDrawVert &vertex : DrawShineBox(0.5f, place).vertices) {
      if (IsShine(vertex)) {
        first = std::min(first, AlongShine(vertex.pos));
        last = std::max(last, AlongShine(vertex.pos));
      }
    }
    return last - first;
  };
  Check(shine.width_fraction * NARROW.width < shine.min_width.value &&
            shine.width_fraction * WIDE.width > shine.min_width.value,
        "the narrow box's share is under the minimum and the wide box's over");
  Check(std::abs(band_width(WIDE) - shine.width_fraction * WIDE.width) <
            TOLERANCE,
        "on a wide box the band is the theme's share of its width");
  Check(std::abs(band_width(NARROW) - shine.min_width.value) < TOLERANCE,
        "and on a narrow box it is the theme's minimum");
}

// The shine is drawn over the box rather than under it, so it lies over the
// box's fill and content alike. The order is read from the triangles, which
// are drawn in the order they are listed.
void TestShineOverChild() {
  constexpr Rect PLACE{100.0f, 100.0f, 600.0f, 64.0f};
  const Drawn drawn = DrawShineBox(0.5f, PLACE);
  const ImU32 blue = IM_COL32(SHINE_BOX_BLUE.r, SHINE_BOX_BLUE.g,
                              SHINE_BOX_BLUE.b, SHINE_BOX_BLUE.a);
  const auto first_shine = std::find_if(
      drawn.indices.begin(), drawn.indices.end(),
      [&](ImDrawIdx index) { return IsShine(drawn.vertices[index]); });
  const auto first_blue = std::find_if(
      drawn.indices.begin(), drawn.indices.end(),
      [&](ImDrawIdx index) { return drawn.vertices[index].col == blue; });
  Check(first_shine != drawn.indices.end() &&
            first_blue != drawn.indices.end(),
        "the box draws its fill and its shine");
  Check(first_blue < first_shine, "and the shine after the fill");
}

// One corner of a triangle as the GPU draws it, its vertex and the clip it is
// drawn under.
struct DrawnPoint {
  ImDrawVert vertex;
  ImVec4 clip;
};

// The corners of every triangle in `drawn`, in the order they are drawn. A
// layered paint keeps its vertices in tree order and only reorders the
// triangles, so draws are compared in this order rather than vertex by vertex.
std::vector<DrawnPoint> InDrawnOrder(const Drawn &drawn) {
  std::vector<DrawnPoint> points;
  size_t offset = 0;
  for (const DrawnCommand &command : drawn.commands) {
    for (unsigned int index = 0; index < command.elements; ++index) {
      points.push_back(
          {drawn.vertices[drawn.indices[offset + index]], command.clip});
    }
    offset += command.elements;
  }
  return points;
}

// Whether `a` and `b` draw the same triangles in the same order, each in the
// same place, colour and texture under the same clip, however the commands
// are split.
bool SameDrawnOrder(const Drawn &a, const Drawn &b) {
  const std::vector<DrawnPoint> left = InDrawnOrder(a);
  const std::vector<DrawnPoint> right = InDrawnOrder(b);
  return std::equal(
      left.begin(), left.end(), right.begin(), right.end(),
      [](const DrawnPoint &l, const DrawnPoint &r) {
        return l.vertex.pos.x == r.vertex.pos.x &&
               l.vertex.pos.y == r.vertex.pos.y &&
               l.vertex.uv.x == r.vertex.uv.x &&
               l.vertex.uv.y == r.vertex.uv.y && l.vertex.col == r.vertex.col &&
               l.clip.x == r.clip.x && l.clip.y == r.clip.y &&
               l.clip.z == r.clip.z && l.clip.w == r.clip.w;
      });
}

// The layer tests draw coloured squares on layers in a stacked box, so they
// all overlap and only the order they are drawn in tells them apart.
constexpr Dp LAYER_SIDE = Dp(40.0f);
constexpr Color LAYER_RED = Rgb(0xFF0000);
constexpr Color LAYER_BLUE = Rgb(0x0000FF);
constexpr Color LAYER_GREEN = Rgb(0x00FF00);
constexpr Color LAYER_YELLOW = Rgb(0xFFFF00);

// A square in `color` on `layer`, with a shadow when asked, so a test can show
// the shadow moves with its box.
BoxElement LayerSquare(Color color, uint8_t layer, bool shadow = false) {
  const Shadow cast =
      shadow ? MakeShadow({{.blur = Dp(8.0f), .color = LAYER_RED}}) : Shadow{};
  return Box({.self = {.layout = {.width = LAYER_SIDE, .height = LAYER_SIDE},
                       .styles = {.fill = color,
                                  .layer = layer,
                                  .shadow = cast}}});
}

// A stacked box holding `children`, so each lies over the last in tree order.
BoxElement LayerStack(const Children &children, uint8_t layer = 0) {
  return Box({.layout = {.stacked = true},
              .self = {.layout = {.width = LAYER_SIDE, .height = LAYER_SIDE},
                       .styles = {.layer = layer}}},
             children);
}

// Boxes on a higher layer are drawn over those on lower ones across the whole
// tree, not only among siblings. A red square on layer 2, shadow included,
// inside the first group, is drawn over a blue one on layer 0 in the second,
// exactly as the same tree on layer 0 draws with the groups swapped. The same
// tree on layer 0 in its own order draws differently, so the comparison can
// tell the two orders apart.
void TestLayersDrawAcrossParents() {
  const auto draw = [](uint8_t red_layer, bool swapped) {
    return DrawBuilt(
        [&] {
          const BoxElement red =
              LayerStack({LayerSquare(LAYER_RED, red_layer, true)});
          const BoxElement blue = LayerStack({LayerSquare(LAYER_BLUE, 0)});
          return swapped ? LayerStack({blue, red}) : LayerStack({red, blue});
        },
        LAYER_SIDE, {100.0f, 100.0f});
  };
  const Drawn layered = draw(2, false);
  Check(!SameDrawnOrder(draw(0, false), draw(0, true)),
        "the two orders draw differently");
  Check(SameDrawnOrder(layered, draw(0, true)),
        "a higher layer in an earlier group is drawn over a lower one in a "
        "later group, its shadow with it");
}

// Elements on the same layer keep tree order. Five squares on layers 1, 0, 1,
// 0, 1 are drawn as the two on layer 0 then the three on layer 1, each pair in
// the order it was built.
void TestEqualLayersKeepTreeOrder() {
  constexpr std::array<Color, 5> COLOURS{LAYER_RED, LAYER_BLUE, LAYER_GREEN,
                                         LAYER_YELLOW, Rgb(0xFF00FF)};
  constexpr std::array<uint8_t, 5> LAYERS{1, 0, 1, 0, 1};
  constexpr std::array<size_t, 5> DRAWN{1, 3, 0, 2, 4};
  const Drawn layered = DrawBuilt(
      [&] {
        std::vector<AnyElement> squares;
        for (size_t index = 0; index < COLOURS.size(); ++index) {
          squares.push_back(LayerSquare(COLOURS[index], LAYERS[index]));
        }
        return LayerStack(squares);
      },
      LAYER_SIDE, {100.0f, 100.0f});
  const Drawn expected = DrawBuilt(
      [&] {
        std::vector<AnyElement> squares;
        for (const size_t index : DRAWN) {
          squares.push_back(LayerSquare(COLOURS[index], 0));
        }
        return LayerStack(squares);
      },
      LAYER_SIDE, {100.0f, 100.0f});
  Check(SameDrawnOrder(layered, expected),
        "elements on one layer are drawn in the order they were built");
}

// A child asking for a lower layer than its parent's is drawn on its parent's,
// so it isn't sunk below the box holding it. A green square on layer 0 inside
// a box on layer 2 is drawn over a yellow square on layer 1 built after it.
void TestChildDrawnOnParentsLayer() {
  const Drawn layered = DrawBuilt(
      [] {
        return LayerStack({LayerStack({LayerSquare(LAYER_GREEN, 0)}, 2),
                           LayerSquare(LAYER_YELLOW, 1)});
      },
      LAYER_SIDE, {100.0f, 100.0f});
  const Drawn expected = DrawBuilt(
      [] {
        return LayerStack({LayerSquare(LAYER_YELLOW, 0),
                           LayerStack({LayerSquare(LAYER_GREEN, 0)})});
      },
      LAYER_SIDE, {100.0f, 100.0f});
  Check(SameDrawnOrder(layered, expected),
        "a child asking for less is drawn on its parent's layer");
}

// A box drawn on a higher layer keeps its parent's clip and fades with its
// parent. A red square on layer 2 inside a clipping box at half opacity is
// drawn over a blue square on layer 0 outside it, exactly as the same tree on
// layer 0 draws with the two swapped, clip and colour alike.
void TestLayerKeepsClipAndOpacity() {
  const auto clipping = [](uint8_t red_layer) {
    BoxStyles styles{.overflow = Overflow::Clip};
    styles.states.hover.opacity = 0.5f;
    return Box({.layout = {.stacked = true},
                .self = {.layout = {.width = LAYER_SIDE, .height = LAYER_SIDE},
                         .styles = styles,
                         .state = {.hover = 1.0f}}},
               {LayerSquare(LAYER_RED, red_layer)});
  };
  const Drawn layered = DrawBuilt(
      [&] { return LayerStack({clipping(2), LayerSquare(LAYER_BLUE, 0)}); },
      LAYER_SIDE, {100.0f, 100.0f});
  const Drawn expected = DrawBuilt(
      [&] { return LayerStack({LayerSquare(LAYER_BLUE, 0), clipping(0)}); },
      LAYER_SIDE, {100.0f, 100.0f});
  Check(SameDrawnOrder(layered, expected),
        "a box on a higher layer keeps its parent's clip and opacity");
  // Checked against the values too, so a clip or a fade missing from both
  // sides can't pass. The red is cut to the clipping box and half as strong.
  constexpr ImU32 RGB_MASK = IM_COL32(255, 255, 255, 0);
  constexpr int HALF = 128;
  const ImVec4 box{100.0f, 100.0f, 100.0f + LAYER_SIDE.value,
                   100.0f + LAYER_SIDE.value};
  bool clipped = true;
  int strongest = 0;
  for (const DrawnPoint &point : InDrawnOrder(layered)) {
    if ((point.vertex.col & RGB_MASK) != IM_COL32(255, 0, 0, 0)) {
      continue;
    }
    clipped = clipped && point.clip.x == box.x && point.clip.y == box.y &&
              point.clip.z == box.z && point.clip.w == box.w;
    strongest = std::max(strongest, AlphaOf(point.vertex));
  }
  Check(strongest > 0 && clipped, "the red is cut to the clipping box");
  Check(std::abs(strongest - HALF) <= 1, "and drawn at half strength");
}

// An Animate is drawn on its child's layer, so its shine, drawn over the
// child, stays over the child on a raised layer rather than falling under what
// the child covers. A red square on layer 2 with a held shine is drawn, shine
// and all, over a blue square on layer 1 built after it.
void TestShineOnChildsLayer() {
  const auto shone = [](uint8_t layer) {
    return Animate({.shine = Shine{.position = 0.5f}},
                   LayerSquare(LAYER_RED, layer));
  };
  const Drawn layered = DrawBuilt(
      [&] { return LayerStack({shone(2), LayerSquare(LAYER_BLUE, 1)}); },
      LAYER_SIDE, {100.0f, 100.0f});
  const Drawn expected = DrawBuilt(
      [&] { return LayerStack({LayerSquare(LAYER_BLUE, 0), shone(0)}); },
      LAYER_SIDE, {100.0f, 100.0f});
  Check(SameDrawnOrder(layered, expected),
        "a shine is drawn on its child's layer, over what the child covers");
}

// Whether `point` is drawn in `color`'s red, green and blue at any strength.
bool DrawnIn(const DrawnPoint &point, Color color) {
  constexpr ImU32 RGB_MASK = IM_COL32(255, 255, 255, 0);
  return (point.vertex.col & RGB_MASK) ==
         IM_COL32(color.r, color.g, color.b, 0);
}

// What an element draws over its children is drawn on its own layer, so a
// child on a higher layer still covers it. A shine over a box on layer 0 is
// drawn under the red square on layer 2 inside that box, where on one layer it
// would be drawn over the square. The shine is the theme's white, which the
// squares are not.
void TestPaintOverOnOwnLayer() {
  const auto order = [](uint8_t inner_layer) {
    const Drawn drawn = DrawBuilt(
        [&] {
          return Animate({.shine = Shine{.position = 0.5f}},
                         LayerStack({LayerSquare(LAYER_RED, inner_layer)}));
        },
        LAYER_SIDE, {100.0f, 100.0f});
    const std::vector<DrawnPoint> points = InDrawnOrder(drawn);
    size_t last_shine = 0;
    size_t first_red = points.size();
    for (size_t index = 0; index < points.size(); ++index) {
      last_shine = DrawnIn(points[index], Rgb(0xFFFFFF)) ? index : last_shine;
      if (DrawnIn(points[index], LAYER_RED) && first_red == points.size()) {
        first_red = index;
      }
    }
    return std::pair<size_t, size_t>{last_shine, first_red};
  };
  const auto [flat_shine, flat_red] = order(0);
  Check(flat_red < flat_shine, "on one layer the shine goes over the square");
  const auto [layered_shine, layered_red] = order(2);
  Check(layered_shine < layered_red,
        "and on a lower layer than the square, under it");
}

// A layered Paint leaves the list as it found it, merged back into one, so
// what the caller draws after it goes over it. A caller that has split the
// list into channels of its own can still paint layers into one of them, and
// its own merge puts them where it asked, in their layered order.
void TestLayeredPaintLeavesListAsFound() {
  constexpr Color MARKER = Rgb(0xFF00FF);
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const BoxElement root = LayerStack(
      {LayerSquare(LAYER_RED, 2), LayerSquare(LAYER_BLUE, 0)});
  // The same squares on one layer, built in the order the layers draw them.
  const BoxElement flat = LayerStack(
      {LayerSquare(LAYER_BLUE, 0), LayerSquare(LAYER_RED, 0)});
  const Rect place{100.0f, 100.0f, LAYER_SIDE.value, LAYER_SIDE.value};
  for (const BoxElement placed : {root, flat}) {
    (void)Measure(placed, {place.width, place.height});
    Check(Arrange(placed, place), "the stack is placed");
  }
  const auto marker = [&](ImDrawList &list) {
    list.AddRectFilled({0.0f, 0.0f}, {10.0f, 10.0f},
                       IM_COL32(MARKER.r, MARKER.g, MARKER.b, MARKER.a));
  };
  const Drawn layered =
      ExpectedDraw([&](ImDrawList &list) { Paint(root, list); });
  const Drawn in_order =
      ExpectedDraw([&](ImDrawList &list) { Paint(flat, list); });
  Check(SameDrawnOrder(layered, in_order),
        "a layered Paint draws its layers in order and merges them whole");
  const std::vector<DrawnPoint> painted = InDrawnOrder(layered);
  const std::vector<DrawnPoint> after =
      InDrawnOrder(ExpectedDraw([&](ImDrawList &list) {
        Paint(root, list);
        marker(list);
      }));
  Check(!painted.empty() && after.size() > painted.size() &&
            DrawnIn(after.back(), MARKER) &&
            !DrawnIn(after[painted.size() - 1], MARKER),
        "what is drawn after a layered Paint goes over it");
  const std::vector<DrawnPoint> split =
      InDrawnOrder(ExpectedDraw([&](ImDrawList &list) {
        list.ChannelsSplit(2);
        list.ChannelsSetCurrent(1);
        Paint(root, list);
        list.ChannelsSetCurrent(0);
        marker(list);
        list.ChannelsMerge();
      }));
  const size_t marked = split.size() - painted.size();
  bool layered_after = split.size() > painted.size();
  for (size_t index = 0; layered_after && index < painted.size(); ++index) {
    layered_after = split[marked + index].vertex.col ==
                        painted[index].vertex.col &&
                    split[marked + index].vertex.pos.x ==
                        painted[index].vertex.pos.x &&
                    split[marked + index].vertex.pos.y ==
                        painted[index].vertex.pos.y;
  }
  Check(DrawnIn(split.front(), MARKER) && layered_after,
        "and painting layers into a caller's own channel keeps their order "
        "where the caller merges it");
}

// Layers order what one Paint draws. A second Paint into the same list draws
// over all of the first, whatever their layers. Each root holds two layers, so
// both calls split the list into layers and the second reuses the first's
// splitter. The first root draws its green square on layer 0 then its red one
// on layer 2. The second draws its blue square on layer 0 then its yellow one
// on layer 1, all of it after all of the first, as two flat roots drawn in
// that order would.
void TestSeparatePaintsDrawInOrder() {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const BoxElement first = LayerStack(
      {LayerSquare(LAYER_RED, 2), LayerSquare(LAYER_GREEN, 0)});
  const BoxElement second = LayerStack(
      {LayerSquare(LAYER_YELLOW, 1), LayerSquare(LAYER_BLUE, 0)});
  const BoxElement flat = LayerStack(
      {LayerSquare(LAYER_GREEN, 0), LayerSquare(LAYER_RED, 0),
       LayerSquare(LAYER_BLUE, 0), LayerSquare(LAYER_YELLOW, 0)});
  const Rect place{100.0f, 100.0f, LAYER_SIDE.value, LAYER_SIDE.value};
  for (const BoxElement root : {first, second, flat}) {
    (void)Measure(root, {place.width, place.height});
    Check(Arrange(root, place), "each root is placed");
  }
  const Drawn twice = ExpectedDraw([&](ImDrawList &list) {
    Paint(first, list);
    Paint(second, list);
  });
  const Drawn once =
      ExpectedDraw([&](ImDrawList &list) { Paint(flat, list); });
  Check(SameDrawnOrder(twice, once),
        "a later Paint draws over an earlier one whatever their layers");
}

} // namespace

int main() {
  TestSafeArea();
  TestToPixels();
  TestToastPadding();
  TestEdgeBandOnTop();
  TestOverflowClip();
  TestOverflowClipRoundsOut();
  TestNestedOverflowClip();
  TestTrophyPadding();
  TestTrophyIconGlow();
  TestEffectiveLayers();
  TestLayersDrawAcrossParents();
  TestEqualLayersKeepTreeOrder();
  TestChildDrawnOnParentsLayer();
  TestLayerKeepsClipAndOpacity();
  TestShineOnChildsLayer();
  TestLayeredPaintLeavesListAsFound();
  TestSeparatePaintsDrawInOrder();
  TestPaintOverOnOwnLayer();
  TestGlyphMatchesBoxes();
  TestGlyphFades();
  TestGlyphKeepsItsSize();
  TestTrophyGlyphMatchesBoxes();
  TestGlyphInk();
  TestGlyphCircle();
  TestGlyphTriangle();
  TestGlyphTriangleLimits();
  TestGlyphMarksCentre();
  TestGlyphStroke();
  TestStatusGlyphs();
  TestGlyphSelfLayout();
  TestToastSelfLayout();
  TestBracedChildren();
  TestMixedChildren();
  TestMixedChildrenLength();
  TestMixedChildrenInComponents();
  TestChildrenForwarding();
  TestStatusColor();
  TestStatusGlyphDefaultColor();
  TestCardReadsDefaults();
  TestCardCastsLowShadow();
  TestShadowDrawsEveryLayer();
  TestShadowLayerGeometry();
  TestShadowSkipsBrokenLayer();
  TestShadowBlendsLayerByLayer();
  TestGlowDrawsHalo();
  TestGlowStrengthAndFade();
  TestGlowUnderBody();
  TestGlowSkipsBroken();
  TestShineInsideShape();
  TestShineCutAlongSide();
  TestShineSkipsBroken();
  TestShineFollowsPosition();
  TestShineAtRest();
  TestShineSlant();
  TestShineStrengthAndWidth();
  TestShineOverChild();
  std::printf("SystemUiLayoutTests: passed\n");
  return 0;
}
