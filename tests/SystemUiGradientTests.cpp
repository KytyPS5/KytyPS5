// Checks the system UI's linear gradients, where a point falls along one at
// each angle as CSS places it, boxes drawn with a gradient fill or border
// keeping their shape and smoothed edges while each point shows its colour,
// and a box's state styles blending gradients and painting them.
#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/core/gradient.h"
#include "graphics/presentation/system/core/scale.h"
#include "graphics/presentation/system/core/theme.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/frameLayout.h"
#include "graphics/presentation/system/element/frame/framePaint.h"
#include "graphics/presentation/system/element/frame/frameScope.h"
#include "graphics/presentation/system/element/primitives/box/box.h"
#include "graphics/presentation/system/element/primitives/box/interactionStates.h"
#include "graphics/presentation/system/imgui/draw/box.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>

namespace {

using namespace Libs::Graphics::SystemUi;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "SystemUiGradientTests: failed: %s\n", message);
    std::abort();
  }
}

// A rect wider than it is tall, so a diagonal gradient's corners show whether
// its length follows CSS.
constexpr Rect AREA{10.0f, 20.0f, 100.0f, 50.0f};
constexpr Color RED{255, 0, 0, 255};
constexpr Color GREEN{0, 255, 0, 255};
constexpr Color BLUE{0, 0, 255, 255};
// How far a drawn colour may be from an end's, as the vertices nearest an
// edge sit half a pixel inside it.
constexpr int NEAR = 8;
// How far a drawn colour may be from one worked out by hand, its rounding.
constexpr int ROUNDING = 2;
constexpr float MAX_CHANNEL = 255.0f;

bool Near(float value, float expected) {
  return std::abs(value - expected) < 1e-4f;
}

// Each angle runs as CSS's does, from the bottom up at 0 and turning
// clockwise, and its line ends at the corners it reaches, so on this wide rect
// a diagonal starts and ends exactly at opposite corners. A point beyond an end
// takes that end, and an empty rect puts everything at the middle.
void TestGradientPosition() {
  const Vec2 top{60.0f, 20.0f};
  const Vec2 bottom{60.0f, 70.0f};
  const Vec2 left{10.0f, 45.0f};
  const Vec2 right{110.0f, 45.0f};
  Check(Near(GradientPosition(AREA, 180.0f, top), 0.0f) &&
            Near(GradientPosition(AREA, 180.0f, bottom), 1.0f) &&
            Near(GradientPosition(AREA, 180.0f, {60.0f, 45.0f}), 0.5f),
        "at 180 it runs from the top to the bottom");
  Check(Near(GradientPosition(AREA, 0.0f, bottom), 0.0f) &&
            Near(GradientPosition(AREA, 0.0f, top), 1.0f),
        "at 0 from the bottom to the top");
  Check(Near(GradientPosition(AREA, 90.0f, left), 0.0f) &&
            Near(GradientPosition(AREA, 90.0f, right), 1.0f),
        "at 90 from the left to the right");
  Check(Near(GradientPosition(AREA, 135.0f, {10.0f, 20.0f}), 0.0f) &&
            Near(GradientPosition(AREA, 135.0f, {110.0f, 70.0f}), 1.0f),
        "a diagonal starts and ends exactly at opposite corners");
  Check(Near(GradientPosition(AREA, 180.0f, {60.0f, -100.0f}), 0.0f) &&
            Near(GradientPosition(AREA, 180.0f, {60.0f, 500.0f}), 1.0f),
        "a point beyond an end takes that end");
  Check(Near(GradientPosition(Rect{}, 180.0f, top), 0.5f),
        "an empty rect puts every point at the middle");
}

// A draw list ready for drawing outside an ImGui frame, smoothing filled
// shapes as the overlay's does. Lines are smoothed with geometry rather than
// ImGui's baked line texture, so a border's edge shows in its vertices' alpha.
// The overlay's whole-pixel borders use the texture, where every vertex is
// opaque and the shading colours each the same way.
struct TestList {
  ImDrawListSharedData shared;
  ImDrawList list{&shared};

  TestList() {
    shared.InitialFlags =
        ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;
    shared.ClipRectFullscreen = ImVec4(-8192.0f, -8192.0f, 8192.0f, 8192.0f);
    shared.SetCircleTessellationMaxError(0.30f);
    list._ResetForNewFrame();
    list.PushClipRectFullScreen();
    list.PushTexture(ImTextureRef());
  }
};

// The vertices DrawBox gives each of `styles` over AREA, one after another in
// one list, drawn at the theme's reference size inside a frame, as a box's
// paint draws it.
ImVector<ImDrawVert> DrawnTogether(std::initializer_list<DrawBoxStyle> styles,
                                   float opacity = 1.0f) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  TestList target;
  for (const DrawBoxStyle &style : styles) {
    DrawBox(target.list, AREA, style, opacity);
  }
  return target.list.VtxBuffer;
}

// The vertices DrawBox gives `style` over AREA on its own.
ImVector<ImDrawVert> Drawn(const DrawBoxStyle &style, float opacity = 1.0f) {
  return DrawnTogether({style}, opacity);
}

// The channel of `color` at `shift`, such as its alpha.
uint8_t Channel(ImU32 color, int shift) {
  return static_cast<uint8_t>((color >> shift) & 0xFFu);
}

// Whether `vertex` is fully covered, inside the shape rather than on its
// smoothed edge.
bool Opaque(const ImDrawVert &vertex) {
  return Channel(vertex.col, IM_COL32_A_SHIFT) == 0xFF;
}

// Whether `color` is within NEAR of `expected` in every channel.
bool CloseTo(ImU32 color, Color expected) {
  const auto close = [](uint8_t value, uint8_t target) {
    return std::abs(static_cast<int>(value) - static_cast<int>(target)) <= NEAR;
  };
  return close(Channel(color, IM_COL32_R_SHIFT), expected.r) &&
         close(Channel(color, IM_COL32_G_SHIFT), expected.g) &&
         close(Channel(color, IM_COL32_B_SHIFT), expected.b) &&
         close(Channel(color, IM_COL32_A_SHIFT), expected.a);
}

// Whether every opaque vertex shows a red to blue gradient from AREA's top to
// its bottom at its height, worked out by hand, so the gradient spans the
// whole box and not only the shape drawn.
bool ShadedTopToBottom(const ImVector<ImDrawVert> &vertices) {
  int checked = 0;
  for (const ImDrawVert &vertex : vertices) {
    if (!Opaque(vertex)) {
      continue;
    }
    ++checked;
    const float along =
        std::clamp((vertex.pos.y - AREA.y) / AREA.height, 0.0f, 1.0f);
    const int red = static_cast<int>(std::lround((1.0f - along) * MAX_CHANNEL));
    const int blue = static_cast<int>(std::lround(along * MAX_CHANNEL));
    if (std::abs(Channel(vertex.col, IM_COL32_R_SHIFT) - red) > ROUNDING ||
        std::abs(Channel(vertex.col, IM_COL32_B_SHIFT) - blue) > ROUNDING) {
      return false;
    }
  }
  return checked > 0;
}

// The opaque vertex scoring lowest by `key`, such as the topmost.
const ImDrawVert &Extreme(const ImVector<ImDrawVert> &vertices,
                          const std::function<float(const ImDrawVert &)> &key) {
  const ImDrawVert *best = nullptr;
  for (const ImDrawVert &vertex : vertices) {
    if (Opaque(vertex) && (best == nullptr || key(vertex) < key(*best))) {
      best = &vertex;
    }
  }
  Check(best != nullptr, "the box has opaque vertices");
  return *best;
}

// How many vertices are fully clear, ImGui's smoothed outer edge.
int ClearCount(const ImVector<ImDrawVert> &vertices) {
  int count = 0;
  for (const ImDrawVert &vertex : vertices) {
    count += Channel(vertex.col, IM_COL32_A_SHIFT) == 0 ? 1 : 0;
  }
  return count;
}

// Whether `whole` holds `part`'s vertices from `offset` on, in the same places,
// and in the same colours too when `same_colours`.
bool Holds(const ImVector<ImDrawVert> &whole, int offset,
           const ImVector<ImDrawVert> &part, bool same_colours) {
  if (whole.Size < offset + part.Size) {
    return false;
  }
  for (int index = 0; index < part.Size; ++index) {
    const ImDrawVert &first = whole[offset + index];
    const ImDrawVert &second = part[index];
    if (first.pos.x != second.pos.x || first.pos.y != second.pos.y ||
        (same_colours && first.col != second.col)) {
      return false;
    }
  }
  return true;
}

// Whether two draws put their vertices in the same places.
bool SameShape(const ImVector<ImDrawVert> &first,
               const ImVector<ImDrawVert> &second) {
  return first.Size == second.Size && Holds(first, 0, second, false);
}

const auto TOP_FIRST = [](const ImDrawVert &vertex) { return vertex.pos.y; };
const auto BOTTOM_FIRST = [](const ImDrawVert &vertex) {
  return -vertex.pos.y;
};
const auto LEFT_FIRST = [](const ImDrawVert &vertex) { return vertex.pos.x; };
const auto RIGHT_FIRST = [](const ImDrawVert &vertex) { return -vertex.pos.x; };

// A gradient fill draws the same rounded shape as a plain fill, with the same
// smoothed outer edge, each point its colour at its height, and at 90 its left
// shows the start colour and its right the end.
void TestFillGradient() {
  const ImVector<ImDrawVert> plain = Drawn({.fill = RED, .radius = 8.0f});
  const ImVector<ImDrawVert> shaded =
      Drawn({.radius = 8.0f, .fill_gradient = LinearGradient{RED, BLUE}});
  Check(SameShape(plain, shaded), "a gradient fill keeps the box's shape");
  Check(ClearCount(plain) > 0 && ClearCount(shaded) == ClearCount(plain),
        "and its smoothed outer edge");
  Check(ShadedTopToBottom(shaded),
        "each point shows the colour at its height, from the start colour at "
        "the top to the end at the bottom");
  const ImVector<ImDrawVert> across = Drawn(
      {.radius = 8.0f, .fill_gradient = LinearGradient{RED, BLUE, 90.0f}});
  Check(CloseTo(Extreme(across, LEFT_FIRST).col, RED) &&
            CloseTo(Extreme(across, RIGHT_FIRST).col, BLUE),
        "and at 90 its left shows the start and its right the end");
}

// A gradient border draws the same stroke as a plain border, with no fill,
// and shades it across the whole box, so a point on the stroke has the colour
// the box's height gives it rather than the stroke's own.
void TestBorderGradient() {
  const ImVector<ImDrawVert> plain =
      Drawn({.radius = 8.0f, .border = RED, .border_width = 4.0f});
  const ImVector<ImDrawVert> shaded =
      Drawn({.radius = 8.0f,
             .border_width = 4.0f,
             .border_gradient = LinearGradient{RED, BLUE}});
  Check(SameShape(plain, shaded),
        "a gradient border draws the same stroke and no fill");
  Check(CloseTo(Extreme(shaded, TOP_FIRST).col, RED) &&
            CloseTo(Extreme(shaded, BOTTOM_FIRST).col, BLUE),
        "its top shows the start colour and its bottom the end");
  Check(ShadedTopToBottom(shaded),
        "and each point the colour the whole box's height gives it");
}

// Shading recolours only what its own draw added. A box drawn earlier into
// the list, and a box's gradient fill drawn before its gradient border, keep
// their colours.
void TestEarlierVerticesKept() {
  const ImVector<ImDrawVert> earlier = Drawn({.fill = GREEN});
  const ImVector<ImDrawVert> fill_alone =
      Drawn({.radius = 8.0f, .fill_gradient = LinearGradient{RED, BLUE}});
  const ImVector<ImDrawVert> together =
      DrawnTogether({{.fill = GREEN},
                     {.radius = 8.0f,
                      .border_width = 4.0f,
                      .fill_gradient = LinearGradient{RED, BLUE},
                      .border_gradient = LinearGradient{GREEN, BLUE}}});
  Check(together.Size > earlier.Size + fill_alone.Size,
        "the border adds vertices after both fills");
  Check(Holds(together, 0, earlier, true),
        "a box drawn earlier into the list keeps its colours");
  Check(Holds(together, earlier.Size, fill_alone, true),
        "and so does a gradient fill drawn before a gradient border");
}

// The element's opacity fades a gradient as it does a colour, and a gradient
// clear at both ends draws nothing.
void TestGradientOpacity() {
  const ImVector<ImDrawVert> faded =
      Drawn({.fill_gradient = LinearGradient{RED, RED}}, 0.5f);
  int strongest = 0;
  for (const ImDrawVert &vertex : faded) {
    strongest = std::max(
        strongest, static_cast<int>(Channel(vertex.col, IM_COL32_A_SHIFT)));
  }
  Check(std::abs(strongest - 128) <= 1,
        "half opacity draws an opaque gradient half clear");
  const ImVector<ImDrawVert> clear =
      Drawn({.fill_gradient = LinearGradient{Color{}, Color{}}});
  Check(clear.Size == 0, "and a gradient clear at both ends draws nothing");
}

// Halfway between red and green, and between blue and green, either way, as
// Lerp rounds them.
constexpr Color RED_GREEN{128, 128, 0, 255};
constexpr Color BLUE_GREEN{0, 128, 128, 255};

// A box's fill stays a plain colour unless the style or a state with weight
// gives it a gradient. A gradient at rest is the style's own, and a state's
// plain colour moves both its ends, halfway at half weight. A plain fill
// meeting a state's gradient becomes a gradient at that state's angle.
void TestResolvedGradientFill() {
  const ThemeContext &theme = DEFAULT_THEME;
  const LinearGradient base{RED, BLUE};
  const BoxStyles plain{.fill = RED, .states = {.hover = {.fill = BLUE}}};
  Check(!ResolveStyle(theme, plain, {.hover = 1.0f}).fill_gradient &&
            ResolveStyle(theme, plain, {.hover = 1.0f}).fill == BLUE,
        "a box with no gradient anywhere stays a plain colour");
  const BoxStyles tinted{.states = {.hover = {.fill = GREEN}},
                         .fill_gradient = base};
  Check(ResolveStyle(theme, tinted, {}).fill_gradient == base,
        "a gradient fill at rest is the style's own");
  Check(ResolveStyle(theme, tinted, {.hover = 1.0f}).fill_gradient ==
            LinearGradient{GREEN, GREEN},
        "a state's plain colour moves both ends to it at full weight");
  Check(ResolveStyle(theme, tinted, {.hover = 0.5f}).fill_gradient ==
            LinearGradient{RED_GREEN, BLUE_GREEN},
        "and halfway at half weight");
  const BoxStyles growing{
      .fill = GREEN,
      .states = {.hover = {.fill_gradient = LinearGradient{RED, BLUE, 90.0f}}}};
  Check(!ResolveStyle(theme, growing, {}).fill_gradient,
        "a state's gradient at rest leaves a plain fill plain");
  Check(ResolveStyle(theme, growing, {.hover = 0.5f}).fill_gradient ==
            LinearGradient{RED_GREEN, BLUE_GREEN, 90.0f},
        "and halfway it is a gradient at that state's angle, its ends halfway "
        "from the plain colour");
}

// Two gradients blend with their angle turning the short way round, hover and
// focus setting the same gradient approach it once at the larger weight, a
// state's gradient with an angle that isn't a number is ignored, and borders
// blend as fills do.
void TestResolvedGradientBlending() {
  const ThemeContext &theme = DEFAULT_THEME;
  const BoxStyles turning{
      .states = {.hover = {.fill_gradient = LinearGradient{RED, BLUE, 10.0f}}},
      .fill_gradient = LinearGradient{RED, BLUE, 350.0f}};
  const float angle = ResolveStyle(theme, turning, {.hover = 0.5f})
                          .fill_gradient->angle_degrees;
  Check(std::abs(std::remainder(angle, 360.0f)) < 1e-3f,
        "an angle turns the short way round, through 0 and not 180");
  const LinearGradient shared{GREEN, GREEN};
  const BoxStyles both{.states = {.hover = {.fill_gradient = shared},
                                  .focus = {.fill_gradient = shared}},
                       .fill_gradient = LinearGradient{RED, BLUE}};
  Check(
      ResolveStyle(theme, both, {.hover = 0.5f, .focus = 0.5f}).fill_gradient ==
          LinearGradient{RED_GREEN, BLUE_GREEN},
      "hover and focus with the same gradient approach it once");
  const BoxStyles broken{
      .states = {.hover = {.fill_gradient = LinearGradient{GREEN, GREEN, NAN}}},
      .fill_gradient = LinearGradient{RED, BLUE}};
  Check(ResolveStyle(theme, broken, {.hover = 1.0f}).fill_gradient ==
            LinearGradient{RED, BLUE},
        "a state's gradient with an angle that isn't a number is ignored");
  // On a plain fill too, where the state's plain colour then applies instead.
  const ResolvedStyle fallback = ResolveStyle(
      theme,
      {.fill = RED,
       .states = {.hover = {.fill = GREEN,
                            .fill_gradient =
                                LinearGradient{GREEN, GREEN, NAN}}}},
      {.hover = 1.0f});
  Check(!fallback.fill_gradient && fallback.fill == GREEN,
        "and on a plain fill the state's plain colour applies instead");
  // The angle tells the two targets apart, as a plain colour's would stay at
  // the base's 180.
  const BoxStyles both_set{
      .states = {.hover = {.fill = GREEN,
                           .fill_gradient =
                               LinearGradient{GREEN, GREEN, 90.0f}}},
      .fill_gradient = LinearGradient{RED, BLUE}};
  Check(ResolveStyle(theme, both_set, {.hover = 1.0f}).fill_gradient ==
            LinearGradient{GREEN, GREEN, 90.0f},
        "a state that sets a colour and a gradient moves toward its gradient");
  const BoxStyles edged{.states = {.hover = {.border_gradient = shared}},
                        .border_gradient = LinearGradient{RED, BLUE}};
  Check(ResolveStyle(theme, edged, {}).border_gradient ==
                LinearGradient{RED, BLUE} &&
            ResolveStyle(theme, edged, {.hover = 1.0f}).border_gradient ==
                shared,
        "a border's gradient blends as a fill's does");
}

// The vertices a box of AREA's size painted at AREA draws with `style` and
// `state`, at the theme's reference size.
ImVector<ImDrawVert> Painted(const BoxStyles &style,
                             const StateInput &state = {}) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, theme.scale.reference_width,
                           theme.scale.reference_height);
  FrameBuilder frame;
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, {});
  const BoxElement box = Box(
      {.self = {.layout = {.width = Dp(AREA.width), .height = Dp(AREA.height)},
                .styles = style,
                .state = state}});
  (void)Measure(box, {AREA.width, AREA.height});
  Check(Arrange(box, AREA), "the box is arranged");
  TestList target;
  Paint(box, target.list);
  return target.list.VtxBuffer;
}

// A painted box draws its gradients, with an edge band between its fill and
// its border too, and its focus ring takes a gradient border's place as it
// takes a plain border's.
void TestPaintedGradients() {
  const LinearGradient gradient{RED, BLUE};
  Check(ShadedTopToBottom(
            Painted({.radius = Dp(8.0f), .fill_gradient = gradient})),
        "a painted box shows its gradient fill");
  const ImVector<ImDrawVert> fill =
      Drawn({.radius = 8.0f, .fill_gradient = gradient});
  const ImVector<ImDrawVert> border = Drawn(
      {.radius = 8.0f, .border_width = 4.0f, .border_gradient = gradient});
  const ImVector<ImDrawVert> banded =
      Painted({.border_width = Dp(4.0f),
               .radius = Dp(8.0f),
               .edge = {.color = GREEN, .width = Dp(4.0f)},
               .fill_gradient = gradient,
               .border_gradient = gradient});
  Check(Holds(banded, 0, fill, true) &&
            Holds(banded, banded.Size - border.Size, border, true),
        "with an edge band, its fill and border still show their gradients");
  const BoxStyles ringed{.border_width = Dp(4.0f),
                         .states = {.focus = {.focus_ring = true}},
                         .border_gradient = gradient};
  BoxStyles plain_ringed = ringed;
  plain_ringed.border = RED;
  plain_ringed.border_gradient.reset();
  Check(SameShape(Painted(ringed, {.focus = 1.0f}),
                  Painted(plain_ringed, {.focus = 1.0f})),
        "its focus ring covers a gradient border as it does a plain one");
}

} // namespace

int main() {
  ImGui::CreateContext();
  TestGradientPosition();
  TestFillGradient();
  TestBorderGradient();
  TestEarlierVerticesKept();
  TestGradientOpacity();
  TestResolvedGradientFill();
  TestResolvedGradientBlending();
  TestPaintedGradients();
  ImGui::DestroyContext();
  std::printf("SystemUiGradientTests: passed\n");
  return 0;
}
