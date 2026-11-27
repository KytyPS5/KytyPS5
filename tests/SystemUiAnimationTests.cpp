// Checks the system UI's slide animation and reduced motion. A slide moves an
// element into its place along its eased entrance while the element keeps its
// space, enters once, and turns in design units scaled to the output. With
// reduced motion a slide stays in place and a fade is shortened to the theme's
// reduced_fade_max. An Animate keeps its progress by its key, or by build order
// among the unkeyed ones.
#include "graphics/presentation/system/animation/animate.h"
#include "graphics/presentation/system/animation/easing.h"
#include "graphics/presentation/system/core/clock.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/core/key.h"
#include "graphics/presentation/system/core/motion.h"
#include "graphics/presentation/system/core/scale.h"
#include "graphics/presentation/system/core/theme.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/frameLayout.h"
#include "graphics/presentation/system/element/frame/frameScope.h"
#include "graphics/presentation/system/element/primitives/box/box.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace {

using namespace Libs::Graphics::SystemUi;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "SystemUiAnimationTests: failed: %s\n", message);
    std::abort();
  }
}

bool Near(float value, float expected) {
  return std::abs(value - expected) < 1e-3f;
}

// A 4K output, where a design unit is two pixels, so a slide that forgets to
// scale its distance shows.
constexpr float WIDTH = 3840.0f;
constexpr float HEIGHT = 2160.0f;
constexpr float PIXELS_PER_DP = 2.0f;
constexpr Rect PLACE{100.0f, 200.0f, 50.0f, 20.0f};
// The distances the slides in these tests enter from, in design units.
constexpr float ACROSS_DP = 32.0f;
constexpr float DOWN_DP = 20.0f;

const TimePoint BASE = TimePoint{} + std::chrono::hours(1000);

TimePoint At(float ms) {
  return BASE + std::chrono::duration_cast<ClockContext::Clock::duration>(
                    std::chrono::duration<float, std::milli>(ms));
}

// Where an Animate and its child were placed in one frame, and whether it
// still needs frames.
struct Placed {
  Rect own;
  Rect child;
  bool needs_frames = false;
};

// Builds `props` around a plain box in `frame` at `ms` with `motion`, measures
// and places it at PLACE, and reads where it went. It is the only Animate in
// each frame, so unkeyed it is the first by build order each time and each
// frame continues the same animation.
Placed BuildAt(FrameBuilder &frame, const AnimateProps &props, float ms,
               const MotionContext &motion = {}) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, WIDTH, HEIGHT);
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, ClockContext{At(ms)}, motion);
  const BoxElement child =
      Box({.self = {.layout = {.width = Dp(25.0f), .height = Dp(10.0f)}}});
  const AnimateElement animate = Animate(props, child);
  (void)Measure(animate, {PLACE.width, PLACE.height});
  Check(Arrange(animate, PLACE), "the animation is placed");
  return {frame.LayoutOf(animate)->arranged, frame.LayoutOf(child)->arranged,
          AnimateNeedsFrames(animate)};
}

// The theme's enter curve at `t`, 1 - (1 - t)^3, worked out here.
float EnterCurve(float t) { return 1.0f - std::pow(1.0f - t, 3.0f); }

// A slide starts its whole distance from its place, scaled to the output,
// moves in along the theme's enter curve, and rests in place once in, while
// the Animate itself keeps its place throughout.
void TestSlideEntrance() {
  const ThemeContext &theme = DEFAULT_THEME;
  const float base = theme.motion.base;
  const float across = ACROSS_DP * PIXELS_PER_DP;
  FrameBuilder frame;
  const AnimateProps props{.slide = Slide{.x = Dp(ACROSS_DP)}};
  const Placed start = BuildAt(frame, props, 0.0f);
  Check(Near(start.child.x, PLACE.x + across) && Near(start.child.y, PLACE.y),
        "a slide starts its whole distance to the right, scaled to the output");
  Check(Near(start.own.x, PLACE.x) && start.needs_frames,
        "while the animation keeps its place and needs frames");
  const Placed half = BuildAt(frame, props, base / 2.0f);
  Check(Near(half.child.x, PLACE.x + across * (1.0f - EnterCurve(0.5f))),
        "halfway through, it has moved as far as the enter curve says");
  const Placed in = BuildAt(frame, props, base);
  Check(Near(in.child.x, PLACE.x) && !in.needs_frames,
        "once in, it rests in its place and needs no frames");
  const Placed later = BuildAt(frame, props, base * 4.0f);
  Check(Near(later.child.x, PLACE.x), "and it enters only once");
}

// A slide given a timeline of its own waits out its delay at its whole
// distance, and stays in its place past that timeline's hold and exit, as a
// slide only ever enters.
void TestSlideOwnTimeline() {
  constexpr float DELAY = 50.0f;
  constexpr float ENTER = 100.0f;
  constexpr float HOLD = 50.0f;
  constexpr float EXIT = 100.0f;
  const Timeline own{.delay_ms = DELAY,
                     .enter = {.duration_ms = ENTER, .ease = Ease::Enter},
                     .hold_ms = HOLD,
                     .exit = {.duration_ms = EXIT}};
  FrameBuilder frame;
  const AnimateProps props{.slide = Slide{.x = Dp(ACROSS_DP), .timeline = own}};
  (void)BuildAt(frame, props, 0.0f);
  Check(Near(BuildAt(frame, props, DELAY / 2.0f).child.x,
             PLACE.x + ACROSS_DP * PIXELS_PER_DP),
        "a slide waits out its delay at its whole distance");
  (void)BuildAt(frame, props, DELAY + ENTER);
  Check(Near(BuildAt(frame, props, DELAY + ENTER + HOLD + EXIT * 2.0f).child.x,
             PLACE.x),
        "and stays in its place past its timeline's hold and exit");
}

// A slide can enter from below as well, and a fade beside it leaving doesn't
// move it, as only the fade takes it away.
void TestSlideFromBelowStaysWhileFading() {
  const ThemeContext &theme = DEFAULT_THEME;
  FrameBuilder frame;
  const AnimateProps arriving{.fade = Fade{.shown = true},
                              .slide = Slide{.y = Dp(DOWN_DP)}};
  Check(Near(BuildAt(frame, arriving, 0.0f).child.y,
             PLACE.y + DOWN_DP * PIXELS_PER_DP),
        "a slide from below starts its distance under its place");
  (void)BuildAt(frame, arriving, theme.motion.base);
  const AnimateProps leaving{.fade = Fade{.shown = false},
                             .slide = Slide{.y = Dp(DOWN_DP)}};
  const Placed fading = BuildAt(frame, leaving, theme.motion.base + 1.0f);
  Check(Near(fading.child.y, PLACE.y) && Near(fading.child.x, PLACE.x),
        "and stays in place while its fade takes it away");
}

// With reduced motion a slide stays in its place from the start and needs no
// frames for it, and a fade's entrance and exit are no longer than the theme's
// reduced_fade_max, where without it they run their whole length.
void TestReducedMotion() {
  const ThemeContext &theme = DEFAULT_THEME;
  const MotionContext reduced{.reduced = true};
  FrameBuilder still;
  const Placed placed =
      BuildAt(still, {.slide = Slide{.x = Dp(ACROSS_DP)}}, 0.0f, reduced);
  Check(Near(placed.child.x, PLACE.x) && !placed.needs_frames,
        "with reduced motion a slide stays in place and needs no frames");
  const float longest = theme.motion.reduced_fade_max;
  const Timeline slow{.enter = {.duration_ms = longest * 3.0f},
                      .exit = {.duration_ms = longest * 3.0f}};
  const AnimateProps shown{.fade = Fade{.shown = true, .timeline = slow}};
  FrameBuilder quick;
  FrameBuilder full;
  (void)BuildAt(quick, shown, 0.0f, reduced);
  (void)BuildAt(full, shown, 0.0f);
  Check(!BuildAt(quick, shown, longest, reduced).needs_frames &&
            BuildAt(full, shown, longest).needs_frames,
        "a fade's entrance is done by the reduced length, where a full one "
        "goes on");
  const AnimateProps hidden{.fade = Fade{.shown = false, .timeline = slow}};
  (void)BuildAt(quick, hidden, longest * 2.0f, reduced);
  (void)BuildAt(full, hidden, longest * 2.0f);
  Check(!BuildAt(quick, hidden, longest * 3.0f, reduced).needs_frames &&
            BuildAt(full, hidden, longest * 3.0f).needs_frames,
        "and so is its exit");
}

// One fade a frame builds, with the key it is found by and where it is going.
struct BuiltFade {
  Key key;
  bool shown = false;
};

// Builds a frame at `ms` holding an Animate around a plain box for each of
// `fades`, in their order, and reads how opaque each one's fade is.
template <size_t COUNT>
std::array<float, COUNT> FadeAlphas(FrameBuilder &frame, float ms,
                                    const std::array<BuiltFade, COUNT> &fades) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, WIDTH, HEIGHT);
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, ClockContext{At(ms)});
  std::array<float, COUNT> alphas{};
  for (size_t index = 0; index < COUNT; ++index) {
    const AnimateElement animate =
        Animate({.fade = Fade{.shown = fades[index].shown},
                 .key = fades[index].key},
                Box({}));
    alphas[index] = FadePose(animate).alpha;
  }
  return alphas;
}

// Two fades, one coming in and one held out, built in one order and then the
// other, part way through the entrance. Reads how opaque the one coming in and
// the one held out are after the swap, keyed by `in` and `out`.
std::array<float, 2> AfterSwap(Key in, Key out) {
  FrameBuilder frame;
  const float halfway = DEFAULT_THEME.motion.base / 2.0f;
  (void)FadeAlphas<2>(frame, 0.0f, {{{in, true}, {out, false}}});
  const std::array<float, 2> swapped =
      FadeAlphas<2>(frame, halfway, {{{out, false}, {in, true}}});
  return {swapped[1], swapped[0]};
}

// A keyed Animate carries on from where it was when built in another order,
// as its key and not its build order finds it. Unkeyed, the same two hand
// their progress over by build order, so the progress follows the key alone.
void TestAnimateKeyOutlastsOrder() {
  const std::array<float, 2> keyed = AfterSwap(Key("in"), Key("out"));
  Check(keyed[0] > 0.0f && keyed[1] == 0.0f,
        "a keyed fade carries on built in another order");
  const std::array<float, 2> unkeyed = AfterSwap({}, {});
  Check(unkeyed[0] == 0.0f && unkeyed[1] > 0.0f,
        "where unkeyed ones hand their progress over by build order");
}

// How opaque an unkeyed fade coming in is part way through its entrance, built
// after a fade held out with `first` in the first frame, which is left out of
// the next.
float KeptAfterFirstLeaves(Key first) {
  FrameBuilder frame;
  const float halfway = DEFAULT_THEME.motion.base / 2.0f;
  (void)FadeAlphas<2>(frame, 0.0f, {{{first, false}, {Key(), true}}});
  return FadeAlphas<1>(frame, halfway, {{{Key(), true}}})[0];
}

// An unkeyed Animate carries on when a keyed one built before it leaves, as a
// keyed Animate takes no number from the unkeyed ones. With the one that
// leaves unkeyed, the kept one is renumbered onto the other's held out fade,
// so the key alone keeps its number.
void TestKeyedAnimateTakesNoNumber() {
  Check(KeptAfterFirstLeaves(Key("leaves")) > 0.0f,
        "an unkeyed fade carries on when a keyed one before it leaves");
  Check(KeptAfterFirstLeaves(Key()) == 0.0f,
        "where an unkeyed one before it leaving renumbers it");
}

// How opaque an unkeyed fade coming in is part way through its entrance, built
// after an unkeyed Animate held out in the first frame, refused when
// `refuse_first` as its child already has a parent, and built alone in the
// next frame.
float KeptAfterFirstRefusedOrBuilt(bool refuse_first) {
  FrameBuilder frame;
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, WIDTH, HEIGHT);
  {
    frame.Begin();
    const FrameScope scope(frame, theme, scale, {}, ClockContext{At(0.0f)});
    const BoxElement child = Box({});
    if (refuse_first) {
      (void)Box({}, {child});
    }
    const AnimateElement first = Animate({.fade = Fade{.shown = false}}, child);
    Check(first.IsNone() == refuse_first,
          "the first Animate is refused exactly when its child is taken");
    (void)Animate({.fade = Fade{.shown = true}}, Box({}));
  }
  return FadeAlphas<1>(frame, theme.motion.base / 2.0f, {{{Key(), true}}})[0];
}

// A refused Animate takes no number from the unkeyed ones, as it isn't built,
// so the unkeyed fade after it keeps its number when the refused one is gone
// the next frame. With the first one really built instead, the kept fade is
// renumbered onto its held out fade, so only the refusal keeps the number.
void TestRefusedAnimateTakesNoNumber() {
  Check(KeptAfterFirstRefusedOrBuilt(true) > 0.0f,
        "an unkeyed fade after a refused Animate carries on");
  Check(KeptAfterFirstRefusedOrBuilt(false) == 0.0f,
        "where one after a built Animate that leaves is renumbered");
}

// Where a shine stood in one frame, and whether it still needs frames.
struct Shone {
  std::optional<float> position;
  bool needs_frames = false;
};

// Builds `props` around a plain box in `frame` at `ms` with `motion` and reads
// where its shine stood. Like BuildAt, it is the only Animate in each frame.
Shone ShineAt(FrameBuilder &frame, const AnimateProps &props, float ms,
              const MotionContext &motion = {}) {
  const ThemeContext &theme = DEFAULT_THEME;
  const ScaleContext scale(theme.scale, WIDTH, HEIGHT);
  frame.Begin();
  const FrameScope scope(frame, theme, scale, {}, ClockContext{At(ms)}, motion);
  const AnimateElement animate = Animate(
      props,
      Box({.self = {.layout = {.width = Dp(25.0f), .height = Dp(10.0f)}}}));
  return {ShinePosition(animate), AnimateNeedsFrames(animate)};
}

// A shine sweeps once across on the theme's standard curve over the theme's
// shine time, worked out from the clock rather than counted frame by frame, so
// a stalled frame lands where the clock says. Once across it draws no band and
// needs no frames. An Animate with no shine has no band and needs no frames,
// so the band comes from the shine.
void TestShineSweep() {
  const float sweep = DEFAULT_THEME.motion.shine;
  {
    FrameBuilder frame;
    const Shone none = ShineAt(frame, {}, sweep / 4.0f);
    Check(!none.position && !none.needs_frames,
          "an Animate with no shine has no band and needs no frames");
  }
  FrameBuilder frame;
  const AnimateProps props{.shine = Shine{}};
  (void)ShineAt(frame, props, 0.0f);
  const Shone quarter = ShineAt(frame, props, sweep / 4.0f);
  Check(quarter.position &&
            Near(*quarter.position, ApplyEase(Ease::Standard, 0.25f)) &&
            quarter.needs_frames,
        "a quarter of the way through, the band is where the standard curve "
        "puts it, and it needs frames");
  const Shone stalled = ShineAt(frame, props, sweep * 3.0f / 4.0f);
  Check(stalled.position &&
            Near(*stalled.position, ApplyEase(Ease::Standard, 0.75f)),
        "a frame after a stall lands where the clock says");
  const Shone over = ShineAt(frame, props, sweep);
  Check(!over.position && !over.needs_frames,
        "once across, it draws no band and needs no frames");
  Check(!ShineAt(frame, props, sweep * 4.0f).position,
        "and it sweeps only once");
}

// With reduced motion a shine doesn't sweep, where the same shine at the same
// moment without it is partway across.
void TestShineReducedMotion() {
  const float quarter = DEFAULT_THEME.motion.shine / 4.0f;
  const AnimateProps props{.shine = Shine{}};
  FrameBuilder moving;
  (void)ShineAt(moving, props, 0.0f);
  Check(ShineAt(moving, props, quarter).position.has_value(),
        "without reduced motion the shine is partway across");
  FrameBuilder reduced;
  (void)ShineAt(reduced, props, 0.0f, {.reduced = true});
  const Shone still = ShineAt(reduced, props, quarter, {.reduced = true});
  Check(!still.position && !still.needs_frames,
        "with reduced motion it doesn't sweep and needs no frames");
}

// A held shine stands where it is held, at any moment and with reduced motion
// too, and needs no frames, as it never moves.
void TestShineHeldPosition() {
  constexpr float HELD = 0.4f;
  const AnimateProps props{.shine = Shine{.position = HELD}};
  FrameBuilder frame;
  const Shone first = ShineAt(frame, props, 0.0f);
  Check(first.position && *first.position == HELD && !first.needs_frames,
        "a held shine stands where it is held and needs no frames");
  FrameBuilder reduced;
  const Shone still = ShineAt(reduced, props, 0.0f, {.reduced = true});
  Check(still.position && *still.position == HELD,
        "and it shows with reduced motion too");
}

// A shine given a timeline of its own waits out its delay with no band, then
// crosses on its own curve and time. Its timeline's own hold and exit are
// ignored, as a sweep never comes back, so once across it rests with no band
// and needs no frames, where a timeline left to its hold would still be
// counting down to its exit.
void TestShineOwnTimeline() {
  constexpr float DELAY = 50.0f;
  constexpr float ENTER = 100.0f;
  constexpr float HOLD = 50.0f;
  constexpr float EXIT = 100.0f;
  const Timeline own{.delay_ms = DELAY,
                     .enter = {.duration_ms = ENTER, .ease = Ease::Linear},
                     .hold_ms = HOLD,
                     .exit = {.duration_ms = EXIT}};
  const AnimateProps props{.shine = Shine{.timeline = own}};
  FrameBuilder frame;
  (void)ShineAt(frame, props, 0.0f);
  Check(!ShineAt(frame, props, DELAY / 2.0f).position,
        "a shine draws no band while it waits out its delay");
  const Shone half = ShineAt(frame, props, DELAY + ENTER / 2.0f);
  Check(half.position && Near(*half.position, 0.5f),
        "then crosses on its own curve and time");
  const Shone across = ShineAt(frame, props, DELAY + ENTER + HOLD / 2.0f);
  Check(!across.position && !across.needs_frames,
        "and once across it rests with no band, ignoring its own hold");
}

} // namespace

int main() {
  TestSlideEntrance();
  TestSlideOwnTimeline();
  TestSlideFromBelowStaysWhileFading();
  TestReducedMotion();
  TestAnimateKeyOutlastsOrder();
  TestKeyedAnimateTakesNoNumber();
  TestRefusedAnimateTakesNoNumber();
  TestShineSweep();
  TestShineReducedMotion();
  TestShineHeldPosition();
  TestShineOwnTimeline();
  std::printf("SystemUiAnimationTests: passed\n");
  return 0;
}
