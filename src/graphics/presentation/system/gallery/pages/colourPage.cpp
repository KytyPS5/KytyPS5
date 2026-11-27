#include "graphics/presentation/system/gallery/pages/colourPage.h"

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/contrast.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"
#include "graphics/presentation/system/gallery/pages/parts/swatch.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "Our colours establish a specified set of options that are accessible across a range of uses.";

// The contrast table's columns, in design units.
constexpr float PAIR_WIDTH   = 440.0f;
constexpr float NUMBER_WIDTH = 140.0f;

namespace P = Palette;

constexpr std::array<NamedColor, 12> PALETTE_GRAYS {
    NamedColor {"GRAY_0", P::GRAY_0},     NamedColor {"GRAY_50", P::GRAY_50},
    NamedColor {"GRAY_100", P::GRAY_100}, NamedColor {"GRAY_150", P::GRAY_150},
    NamedColor {"GRAY_200", P::GRAY_200}, NamedColor {"GRAY_300", P::GRAY_300},
    NamedColor {"GRAY_400", P::GRAY_400}, NamedColor {"GRAY_500", P::GRAY_500},
    NamedColor {"GRAY_600", P::GRAY_600}, NamedColor {"GRAY_700", P::GRAY_700},
    NamedColor {"GRAY_800", P::GRAY_800}, NamedColor {"GRAY_900", P::GRAY_900}};

constexpr std::array<NamedColor, 7> PALETTE_BLUE {
    NamedColor {"BLUE_300", P::BLUE_300}, NamedColor {"BLUE_350", P::BLUE_350},
    NamedColor {"BLUE_400", P::BLUE_400}, NamedColor {"BLUE_450", P::BLUE_450},
    NamedColor {"BLUE_500", P::BLUE_500}, NamedColor {"BLUE_600", P::BLUE_600},
    NamedColor {"BLUE_700", P::BLUE_700}};

constexpr std::array<NamedColor, 4> PALETTE_RED {
    NamedColor {"RED_400", P::RED_400}, NamedColor {"RED_500", P::RED_500},
    NamedColor {"RED_600", P::RED_600}, NamedColor {"RED_700", P::RED_700}};

constexpr std::array<NamedColor, 2> PALETTE_YELLOW {NamedColor {"YELLOW_500", P::YELLOW_500},
                                                    NamedColor {"YELLOW_600", P::YELLOW_600}};

constexpr std::array<NamedColor, 2> PALETTE_GREEN {NamedColor {"GREEN_500", P::GREEN_500},
                                                   NamedColor {"GREEN_600", P::GREEN_600}};

constexpr std::array<NamedColor, 4> PALETTE_SPECIAL {
    NamedColor {"PLATINUM", P::PLATINUM}, NamedColor {"GOLD", P::GOLD},
    NamedColor {"SILVER", P::SILVER}, NamedColor {"BRONZE", P::BRONZE}};

// The tier 1 opacities, each shown on white, the colour most tokens lay them on.
constexpr std::array<NamedColor, 15> OPACITIES {
    NamedColor {"HOVER", P::GRAY_900.WithAlpha(Opacity::HOVER)},
    NamedColor {"FOCUS", P::GRAY_900.WithAlpha(Opacity::FOCUS)},
    NamedColor {"BORDER_SUBTLE", P::GRAY_900.WithAlpha(Opacity::BORDER_SUBTLE)},
    NamedColor {"TINT", P::GRAY_900.WithAlpha(Opacity::TINT)},
    NamedColor {"PRESSED", P::GRAY_900.WithAlpha(Opacity::PRESSED)},
    NamedColor {"BORDER_STANDARD", P::GRAY_900.WithAlpha(Opacity::BORDER_STANDARD)},
    NamedColor {"INSET", P::GRAY_900.WithAlpha(Opacity::INSET)},
    NamedColor {"DISABLED", P::GRAY_900.WithAlpha(Opacity::DISABLED)},
    NamedColor {"BORDER_STRONG", P::GRAY_900.WithAlpha(Opacity::BORDER_STRONG)},
    NamedColor {"KEYLINE", P::GRAY_900.WithAlpha(Opacity::KEYLINE)},
    NamedColor {"SCRIM", P::GRAY_900.WithAlpha(Opacity::SCRIM)},
    NamedColor {"HUD", P::GRAY_900.WithAlpha(Opacity::HUD)},
    NamedColor {"HUD_STRONG", P::GRAY_900.WithAlpha(Opacity::HUD_STRONG)},
    NamedColor {"PANEL", P::GRAY_900.WithAlpha(Opacity::PANEL)},
    NamedColor {"OVERLAY", P::GRAY_900.WithAlpha(Opacity::OVERLAY)}};

// A semantic colour of `Group` and its full name in the code. Only the name is written here, and
// the colour is read from the live theme, so the page always shows what the theme holds.
template <class Group>
struct Role {
	std::string_view name;
	Color Group::* color;
};

constexpr std::array<Role<SurfaceColors>, 7> SURFACE_ROLES {
    Role<SurfaceColors> {"surface.base", &SurfaceColors::base},
    Role<SurfaceColors> {"surface.raised", &SurfaceColors::raised},
    Role<SurfaceColors> {"surface.overlay", &SurfaceColors::overlay},
    Role<SurfaceColors> {"surface.hud", &SurfaceColors::hud},
    Role<SurfaceColors> {"surface.hud_strong", &SurfaceColors::hud_strong},
    Role<SurfaceColors> {"surface.inset", &SurfaceColors::inset},
    Role<SurfaceColors> {"surface.scrim", &SurfaceColors::scrim}};

constexpr std::array<Role<TextColors>, 8> TEXT_ROLES {
    Role<TextColors> {"text.primary", &TextColors::primary},
    Role<TextColors> {"text.secondary", &TextColors::secondary},
    Role<TextColors> {"text.tertiary", &TextColors::tertiary},
    Role<TextColors> {"text.disabled", &TextColors::disabled},
    Role<TextColors> {"text.on_accent", &TextColors::on_accent},
    Role<TextColors> {"text.accent", &TextColors::accent},
    Role<TextColors> {"text.on_light", &TextColors::on_light},
    Role<TextColors> {"text.secondary_on_light", &TextColors::secondary_on_light}};

constexpr std::array<Role<AccentColors>, 6> ACCENT_ROLES {
    Role<AccentColors> {"accent.fill", &AccentColors::fill},
    Role<AccentColors> {"accent.fill_hover", &AccentColors::fill_hover},
    Role<AccentColors> {"accent.fill_pressed", &AccentColors::fill_pressed},
    Role<AccentColors> {"accent.line", &AccentColors::line},
    Role<AccentColors> {"accent.tint", &AccentColors::tint},
    Role<AccentColors> {"accent.glow", &AccentColors::glow}};

constexpr std::array<Role<FocusColors>, 3> FOCUS_ROLES {
    Role<FocusColors> {"focus.ring", &FocusColors::ring},
    Role<FocusColors> {"focus.keyline", &FocusColors::keyline},
    Role<FocusColors> {"focus.glow", &FocusColors::glow}};

constexpr std::array<Role<StatusColors>, 9> STATUS_ROLES {
    Role<StatusColors> {"status.success", &StatusColors::success},
    Role<StatusColors> {"status.warning", &StatusColors::warning},
    Role<StatusColors> {"status.error", &StatusColors::error},
    Role<StatusColors> {"status.info", &StatusColors::info},
    Role<StatusColors> {"status.danger_fill", &StatusColors::danger_fill},
    Role<StatusColors> {"status.success_tint", &StatusColors::success_tint},
    Role<StatusColors> {"status.warning_tint", &StatusColors::warning_tint},
    Role<StatusColors> {"status.error_tint", &StatusColors::error_tint},
    Role<StatusColors> {"status.info_tint", &StatusColors::info_tint}};

constexpr std::array<Role<ChartColors>, 4> CHART_ROLES {
    Role<ChartColors> {"chart.good", &ChartColors::good},
    Role<ChartColors> {"chart.medium", &ChartColors::medium},
    Role<ChartColors> {"chart.bad", &ChartColors::bad},
    Role<ChartColors> {"chart.neutral", &ChartColors::neutral}};

constexpr std::array<Role<BorderColors>, 3> BORDER_ROLES {
    Role<BorderColors> {"border.subtle", &BorderColors::subtle},
    Role<BorderColors> {"border.standard", &BorderColors::standard},
    Role<BorderColors> {"border.strong", &BorderColors::strong}};

constexpr std::array<Role<StateColors>, 5> STATE_ROLES {
    Role<StateColors> {"state.hover", &StateColors::hover},
    Role<StateColors> {"state.focus", &StateColors::focus},
    Role<StateColors> {"state.pressed", &StateColors::pressed},
    Role<StateColors> {"state.hover_on_light", &StateColors::hover_on_light},
    Role<StateColors> {"state.focus_on_light", &StateColors::focus_on_light}};

constexpr std::array<Role<TrophyColors>, 4> TROPHY_ROLES {
    Role<TrophyColors> {"trophy.platinum", &TrophyColors::platinum},
    Role<TrophyColors> {"trophy.gold", &TrophyColors::gold},
    Role<TrophyColors> {"trophy.silver", &TrophyColors::silver},
    Role<TrophyColors> {"trophy.bronze", &TrophyColors::bronze}};

constexpr std::array<Role<EffectColors>, 1> EFFECT_ROLES {
    Role<EffectColors> {"effect.shine", &EffectColors::shine}};

constexpr std::array<Role<ControlColors>, 4> CONTROL_ROLES {
    Role<ControlColors> {"control.track_off", &ControlColors::track_off},
    Role<ControlColors> {"control.knob", &ControlColors::knob},
    Role<ControlColors> {"control.fill_disabled", &ControlColors::fill_disabled},
    Role<ControlColors> {"control.knob_disabled", &ControlColors::knob_disabled}};

// Every group holds colours and nothing else, so a colour added to a group and not to its table
// here fails to build rather than going missing, though a table naming one colour twice could
// still hide another.
template <class Group, size_t Count>
constexpr bool Covers(const std::array<Role<Group>, Count>&) {
	return sizeof(Group) == Count * sizeof(Color);
}
static_assert(Covers(SURFACE_ROLES) && Covers(TEXT_ROLES) && Covers(ACCENT_ROLES) &&
              Covers(FOCUS_ROLES) && Covers(STATUS_ROLES) && Covers(CHART_ROLES) &&
              Covers(BORDER_ROLES) && Covers(STATE_ROLES) && Covers(TROPHY_ROLES) &&
              Covers(EFFECT_ROLES) && Covers(CONTROL_ROLES));
static_assert(sizeof(SemanticColors) ==
                  (SURFACE_ROLES.size() + TEXT_ROLES.size() + ACCENT_ROLES.size() +
                   FOCUS_ROLES.size() + STATUS_ROLES.size() + CHART_ROLES.size() +
                   BORDER_ROLES.size() + STATE_ROLES.size() + TROPHY_ROLES.size() +
                   EFFECT_ROLES.size() + CONTROL_ROLES.size()) *
                      sizeof(Color),
              "a colour group was added to SemanticColors without a table here");

// Adds `group`'s colours to `named`, under the names `roles` gives them.
template <class Group, size_t Count>
void AddRoles(const Group& group, const std::array<Role<Group>, Count>& roles,
              std::vector<NamedColor>& named) {
	for (const Role<Group>& role: roles) {
		named.push_back({role.name, group.*role.color});
	}
}

// The semantic colours in sections of related groups, each a grid of swatches.
std::array<AnyElement, 5> RoleSections(const ThemeContext& theme) {
	const SemanticColors&   colors = theme.colors;
	std::vector<NamedColor> surfaces;
	AddRoles(colors.surface, SURFACE_ROLES, surfaces);
	std::vector<NamedColor> text;
	AddRoles(colors.text, TEXT_ROLES, text);
	std::vector<NamedColor> accent;
	AddRoles(colors.accent, ACCENT_ROLES, accent);
	AddRoles(colors.focus, FOCUS_ROLES, accent);
	std::vector<NamedColor> status;
	AddRoles(colors.status, STATUS_ROLES, status);
	AddRoles(colors.chart, CHART_ROLES, status);
	std::vector<NamedColor> others;
	AddRoles(colors.border, BORDER_ROLES, others);
	AddRoles(colors.state, STATE_ROLES, others);
	AddRoles(colors.trophy, TROPHY_ROLES, others);
	AddRoles(colors.effect, EFFECT_ROLES, others);
	AddRoles(colors.control, CONTROL_ROLES, others);
	return {Section(theme, "Surfaces", SwatchGrid(theme, surfaces)),
	        Section(theme, "Text", SwatchGrid(theme, text)),
	        Section(theme, "Accent and focus", SwatchGrid(theme, accent)),
	        Section(theme, "Status and charts", SwatchGrid(theme, status)),
	        Section(theme, "Borders, state, trophies, effects and controls",
	                SwatchGrid(theme, others))};
}

// The table's header row, naming the columns.
AnyElement HeaderRow(const ThemeContext& theme) {
	const Color                     quiet = theme.colors.text.tertiary;
	const std::array<AnyElement, 5> cells {CaptionCell(theme, "Pair", PAIR_WIDTH, quiet),
	                                       CaptionCell(theme, "Over black", NUMBER_WIDTH, quiet),
	                                       CaptionCell(theme, "Over white", NUMBER_WIDTH, quiet),
	                                       CaptionCell(theme, "Target", NUMBER_WIDTH, quiet),
	                                       CaptionCell(theme, "Result", NUMBER_WIDTH, quiet)};
	return Box({.layout = {.padding = DpInsets(theme.space.s1, theme.space.s2)}}, cells);
}

// One pair of the table. A failing pair says so in words and is tinted, and the ratio that falls
// short is in the error colour, so the failure never rests on colour alone.
AnyElement ContrastLine(const ThemeContext& theme, const ContrastRow& row) {
	const SemanticColors& colors      = theme.colors;
	const float           black       = ContrastOver(row, CONTRAST_BLACK_FRAME);
	const float           white       = ContrastOver(row, CONTRAST_WHITE_FRAME);
	const bool            passes      = ContrastPasses(row);
	const auto            ratio_color = [&](float ratio) {
        return ratio < row.target ? colors.status.error : colors.text.primary;
	};
	const std::array<AnyElement, 5> cells {
	    CaptionCell(theme, row.name, PAIR_WIDTH, colors.text.secondary),
	    CaptionCell(theme, RatioText(black), NUMBER_WIDTH, ratio_color(black)),
	    CaptionCell(theme, RatioText(white), NUMBER_WIDTH, ratio_color(white)),
	    CaptionCell(theme, RatioText(row.target), NUMBER_WIDTH, colors.text.tertiary),
	    CaptionCell(theme, passes ? "Pass" : "Fail", NUMBER_WIDTH,
	                passes ? colors.text.primary : colors.status.error)};
	return Box({.layout = {.padding = DpInsets(theme.space.s1, theme.space.s2)},
	            .self   = {.styles = {.fill   = passes ? Color {} : colors.status.error_tint,
	                                  .radius = theme.radius.xs}}},
	           cells);
}

// The contrast table, a header then one line per pair the theme is judged on.
AnyElement ContrastTable(const ThemeContext& theme) {
	std::vector<AnyElement> lines {HeaderRow(theme)};
	for (const ContrastRow& row: ContrastRows(theme)) {
		lines.push_back(ContrastLine(theme, row));
	}
	return Box({.layout = {.direction = FlexDirection::Column}}, lines);
}

} // namespace

AnyElement BuildColourPage() {
	const ThemeContext& theme = CurrentThemeContext();
	// The short ramps share a row, one empty swatch column between them, so each starts on the
	// grid's columns above.
	const Dp         ramp_gap = Dp(SWATCH_WIDTH + 2.0f * theme.space.s3.value);
	const AnyElement ramps    = Box({.layout = {.gap = ramp_gap}},
	                                {Section(theme, "Red", SwatchGrid(theme, PALETTE_RED)),
	                                 Section(theme, "Yellow", SwatchGrid(theme, PALETTE_YELLOW)),
	                                 Section(theme, "Green", SwatchGrid(theme, PALETTE_GREEN))});
	// Every semantic role by its name in the code, after the palette they are made from.
	const std::array<AnyElement, 5> roles = RoleSections(theme);
	return Page({.title = "Colours", .subtitle = RULE},
	            {Section(theme, "Neutrals", SwatchGrid(theme, PALETTE_GRAYS)),
	             Section(theme, "Blue", SwatchGrid(theme, PALETTE_BLUE)), ramps,
	             Section(theme, "Special", SwatchGrid(theme, PALETTE_SPECIAL)),
	             Section(theme, "Opacities, on white", SwatchGrid(theme, OPACITIES)), roles,
	             Section(theme, "Contrast, the worse of over black and over white decides",
	                     ContrastTable(theme))});
}

} // namespace Libs::Graphics::SystemUi::Gallery
