// Checks the gallery's token formatting and contrast maths, which its Colours
// page and the other style pages show. Each figure here was worked out apart
// from the code under test.
#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/theme.h"
#include "graphics/presentation/system/gallery/pages/parts/contrast.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

using namespace Libs::Graphics::SystemUi;
using namespace Libs::Graphics::SystemUi::Gallery;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "SystemUiGalleryTests: failed: %s\n", message);
    std::abort();
  }
}

// An opaque colour is written as #RRGGBB alone, and a translucent one adds
// its opacity as a whole percentage, which for an alpha of 245 of 255 is 96%.
// The colours are fixed here rather than read from the theme, so a palette
// change can't break the check.
void TestHexText() {
  constexpr Color OPAQUE{0xFF, 0xFF, 0xFF, 0xFF};
  constexpr Color TRANSLUCENT{0x1E, 0x23, 0x2E, 245};
  Check(HexText(OPAQUE) == "#FFFFFF",
        "an opaque colour has no opacity after it");
  Check(HexText(TRANSLUCENT) == "#1E232E 96%",
        "a translucent one gives its opacity as a whole percentage");
  Check(HexText(Color{1, 2, 3, 0}) == "#010203 0%", "and a clear one says 0%");
}

// Lengths are whole numbers when they are whole and keep one place when not,
// so a half unit doesn't read as nothing or as one.
void TestLengthText() {
  Check(LengthText(16.0f) == "16", "a whole length has no decimals");
  Check(LengthText(0.5f) == "0.5", "a half keeps its place");
  Check(MsText(180.0f) == "180 ms", "a duration says ms");
  Check(PercentText(0.38f) == "38%", "a fraction is a whole percentage");
  Check(PercentText(0.996f) == "100%",
        "rounded to the nearest, not cut short to 99%");
  Check(RatioText(4.4781f) == "4.4:1",
        "a ratio is cut down to one place, never rounded up to a pass");
}

// A style's metrics say its size and line, its weight in words and its least
// size, so Regular and Bold styles of one size tell apart.
void TestTypeMetricsText() {
  Check(TypeMetricsText(DEFAULT_THEME.type.body) ==
            "22 / 32 Regular, at least 14",
        "body is 22 on 32, Regular, at least 14");
  Check(TypeMetricsText(DEFAULT_THEME.type.label) ==
            "22 / 28 Bold, at least 14",
        "label is 22 on 28, Bold, at least 14");
}

// The ratio is the WCAG one, exactly 21 for white on black, and 4.478 for a
// grey on white as a calculator gives it. Order and alpha don't matter, and a
// colour on itself is 1. Over a surface an opaque colour hides what is under
// it, a clear one shows it, and half shows both.
void TestContrastMaths() {
  constexpr float SLACK = 0.001f;
  const Color white = Rgb(0xFFFFFF);
  const Color black = Rgb(0x000000);
  const Color grey = Rgb(0x777777);
  Check(std::fabs(ContrastRatio(white, black) - 21.0f) <= SLACK,
        "white on black is 21 to 1");
  Check(std::fabs(ContrastRatio(grey, white) - 4.478f) <= SLACK,
        "the grey is 4.478 to 1 on white");
  Check(ContrastRatio(white, grey) == ContrastRatio(grey, white),
        "in either order");
  Check(ContrastRatio(grey.WithAlpha(0.2f), white) ==
            ContrastRatio(grey, white),
        "whatever the alpha");
  Check(ContrastRatio(grey, grey) == 1.0f, "a colour on itself is 1 to 1");
  Check(Composite(grey, white) == grey, "an opaque colour hides what is under");
  Check(Composite(grey.WithAlpha(0.0f), white) == white,
        "a clear one shows it");
  Check(Composite(Color{255, 255, 255, 128}, black) == Rgb(0x808080),
        "and half shows both");
  Check(Composite(Color{255, 255, 255, 128}, black).a == 255,
        "and what results is opaque");
}

// The theme with the colours the contrast figures below were worked out for
// pinned, so a change to the palette can't break the checks, which are of the
// table's maths and not of the palette.
ThemeContext PinnedTheme() {
  // The colours and opacities the committed theme had when the figures were
  // worked out, as literals, not the Opacity constants, which may change.
  ThemeContext theme = DEFAULT_THEME;
  theme.colors.text.primary = Rgb(0xFFFFFF);
  theme.colors.text.secondary = Rgb(0xB0B4C0);
  theme.colors.text.tertiary = Rgb(0x8A93A6);
  theme.colors.surface.raised = Rgb(0x181A20).WithAlpha(0.92f);
  theme.colors.surface.hud = Rgb(0x0E1117).WithAlpha(0.72f);
  theme.colors.status.error = Rgb(0xFF7A72);
  theme.colors.status.error_tint = Rgb(0xEB5046).WithAlpha(0.16f);
  return theme;
}

// The row of the pinned theme's table called `name`.
ContrastRow FindRow(std::string_view name) {
  const std::vector<ContrastRow> rows = ContrastRows(PinnedTheme());
  const auto found =
      std::find_if(rows.begin(), rows.end(),
                   [name](const ContrastRow &row) { return row.name == name; });
  Check(found != rows.end(), "the table has the row");
  return *found;
}

// The table's figures, for the pinned colours, against ones worked out apart
// from this code. A surface is translucent, so a pair differs over black and
// white. A tint is judged on the surface under it, which the same row without
// that surface doesn't reach. A pair fine over black but not over white fails,
// as the worse decides, and the same pair passes a lower target.
void TestContrastRows() {
  constexpr float SLACK = 0.01f;
  const Color black = Rgb(0x000000);
  const Color white = Rgb(0xFFFFFF);
  const ContrastRow primary = FindRow("Primary text on raised");
  Check(std::fabs(ContrastOver(primary, black) - 17.759f) <= SLACK &&
            std::fabs(ContrastOver(primary, white) - 13.971f) <= SLACK,
        "primary on raised is as worked out");
  const ContrastRow tertiary = FindRow("Tertiary text on raised");
  Check(std::fabs(ContrastOver(tertiary, white) - 4.527f) <= SLACK &&
            ContrastPasses(tertiary),
        "tertiary on raised only just passes, over white");
  const ContrastRow hud = FindRow("Secondary text on HUD");
  Check(std::fabs(ContrastOver(hud, black) - 9.441f) <= SLACK &&
            std::fabs(ContrastOver(hud, white) - 3.715f) <= SLACK,
        "secondary on the HUD is as worked out");
  Check(!ContrastPasses(hud),
        "and fails, as the worse of the two is under its target");
  ContrastRow lower = hud;
  lower.target = CONTRAST_LARGE;
  Check(ContrastPasses(lower), "the same pair passes a lower target");
  const ContrastRow tinted = FindRow("Error text on its tint, on raised");
  Check(std::fabs(ContrastOver(tinted, black) - 5.867f) <= SLACK &&
            std::fabs(ContrastOver(tinted, white) - 4.627f) <= SLACK,
        "a tint is judged on the surface under it");
  ContrastRow bare = tinted;
  bare.under = {};
  Check(std::fabs(ContrastOver(bare, white) - 2.079f) <= SLACK,
        "which the same tint with nothing under it doesn't reach");
  Check(FindRow("Accent line on raised, not text").target == CONTRAST_LARGE &&
            FindRow("Focus ring on raised, not text").target ==
                CONTRAST_LARGE &&
            FindRow("Strong border on raised, not text").target ==
                CONTRAST_LARGE &&
            primary.target == CONTRAST_TEXT,
        "parts that aren't text have the lower target, and text the higher");
}

} // namespace

int main() {
  TestHexText();
  TestLengthText();
  TestTypeMetricsText();
  TestContrastMaths();
  TestContrastRows();
  std::printf("SystemUiGalleryTests: passed\n");
  return 0;
}
