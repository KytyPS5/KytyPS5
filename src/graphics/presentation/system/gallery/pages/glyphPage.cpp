#include "graphics/presentation/system/gallery/pages/glyphPage.h"

#include "graphics/presentation/system/components/glyphs/status.h"
#include "graphics/presentation/system/components/glyphs/trophy.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"
#include "graphics/presentation/system/views/trophyUnlocked/trophyUnlocked.h"

#include <array>
#include <string_view>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "Draw symbols from shapes, never an icon font, so they need no image or font to show. Pair "
    "every symbol with a word.";

// A size larger than the toast draws the cup at, so its shape shows in detail.
constexpr Dp LARGE_GLYPH = Dp(80.0f);

// A grade, its name and its colour's name in the code.
struct NamedGrade {
	std::string_view name;
	std::string_view token;
	TrophyGrade      grade;
};

constexpr std::array<NamedGrade, 4> GRADES {
    NamedGrade {"Platinum", "trophy.platinum", TrophyGrade::Platinum},
    NamedGrade {"Gold", "trophy.gold", TrophyGrade::Gold},
    NamedGrade {"Silver", "trophy.silver", TrophyGrade::Silver},
    NamedGrade {"Bronze", "trophy.bronze", TrophyGrade::Bronze}};

// The sizes every glyph on the page is shown at, the two the trophy toast draws its cup at, before
// its subtitle and on the tile in place of a missing icon, and one larger. They are read from the
// toast so they never drift, and the status glyphs share them so the two sets compare size for
// size.
std::array<Dp, 3> GlyphSizes(const ThemeContext& theme) {
	const TrophyUnlockedTokens toast = TrophyUnlockedDefaults(theme);
	return {toast.cup_size, toast.tile_cup_size, LARGE_GLYPH};
}

// The glyph in one grade at each size, captioned with the grade.
AnyElement GlyphColumn(const ThemeContext& theme, const NamedGrade& named) {
	const Color             color = TrophyGradeColor(theme, named.grade);
	std::vector<AnyElement> glyphs;
	for (const Dp size: GlyphSizes(theme)) {
		glyphs.push_back(TrophyGlyph({.size = size, .self = {.styles = {.color = color}}}));
	}
	return Captioned(theme, Box({.layout = {.align = Align::End, .gap = theme.space.s3}}, glyphs),
	                 named.name);
}

// The glyph in every grade and size, beside the grade colours by their names in the code.
AnyElement Grades(const ThemeContext& theme) {
	std::vector<AnyElement> columns;
	std::vector<AnyElement> tokens;
	for (const NamedGrade& named: GRADES) {
		columns.push_back(GlyphColumn(theme, named));
		tokens.push_back(
		    TokenLine(theme, named.token, HexText(TrophyGradeColor(theme, named.grade))));
	}
	return BesideTokens(theme, Box({.layout = {.gap = theme.space.s7}}, columns),
	                    Lines(theme, tokens));
}

// A status, its name and its colour's name in the code.
struct NamedStatus {
	std::string_view name;
	std::string_view token;
	Status           status;
};

constexpr std::array<NamedStatus, 4> STATUSES {
    NamedStatus {"Info", "status.info", Status::Info},
    NamedStatus {"Warning", "status.warning", Status::Warning},
    NamedStatus {"Error", "status.error", Status::Error},
    NamedStatus {"Success", "status.success", Status::Success}};

// One status glyph outlined over filled, each at every size, in its status colour, captioned with
// its name.
AnyElement StatusColumn(const ThemeContext& theme, const NamedStatus& named) {
	std::vector<AnyElement> rows;
	for (const bool fill: {false, true}) {
		std::vector<AnyElement> glyphs;
		for (const Dp size: GlyphSizes(theme)) {
			glyphs.push_back(StatusGlyph({.status = named.status, .size = size, .fill = fill}));
		}
		rows.push_back(Box({.layout = {.align = Align::End, .gap = theme.space.s3}}, glyphs));
	}
	return Captioned(
	    theme, Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s4}}, rows),
	    named.name);
}

// Every status glyph, outlined and filled at every size, beside the status colours by their names
// in the code. Filled, the mark is drawn in text.on_accent.
AnyElement Statuses(const ThemeContext& theme) {
	std::vector<AnyElement> columns;
	std::vector<AnyElement> tokens;
	for (const NamedStatus& named: STATUSES) {
		columns.push_back(StatusColumn(theme, named));
		tokens.push_back(TokenLine(theme, named.token, HexText(StatusColor(theme, named.status))));
	}
	tokens.push_back(
	    TokenLine(theme, "filled mark text.on_accent", HexText(theme.colors.text.on_accent)));
	return BesideTokens(theme, Box({.layout = {.gap = theme.space.s7}}, columns),
	                    Lines(theme, tokens));
}

// The cup at the size the toast draws it before its subtitle, beside a word, as it must always be
// in use.
AnyElement InUse(const ThemeContext& theme) {
	const std::array<AnyElement, 2> paired {
	    TrophyGlyph({.size = TrophyUnlockedDefaults(theme).cup_size,
	                 .self = {.styles = {.color = theme.colors.trophy.gold}}}),
	    Text({.text = "Gold trophy earned",
	          .self = {.styles = {.type = theme.type.body, .color = theme.colors.text.primary}}}),
	};
	return Box({.layout = {.align = Align::Center, .gap = theme.space.s2}}, paired);
}

} // namespace

AnyElement BuildGlyphPage() {
	const ThemeContext& theme = CurrentThemeContext();
	return Page(
	    {.title = "Glyphs", .subtitle = RULE},
	    {
	        Section(theme, "Grades, at the toast's two sizes and one larger", Grades(theme)),
	        Section(theme, "Status glyphs, outlined over filled, at the same sizes",
	                Statuses(theme)),
	        Section(theme, "In use, always with a word", InUse(theme)),
	    });
}

} // namespace Libs::Graphics::SystemUi::Gallery
