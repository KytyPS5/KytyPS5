#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_THEME_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_THEME_H_

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/dp.h"
#include "graphics/presentation/system/core/status.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <type_traits>

// The system UI theme. Values are in three tiers.
//  - The palette holds raw colours with no meaning. A re-theme only edits the palette.
//  - Semantic tokens give each colour a role, such as a surface level or a text level.
//  - Component tokens size and style one kind of component. They live with the component, in a
//    tokens struct in its header built from the tiers above by its XDefaults(theme) function. How
//    a caller changes them differs by kind of component, as CONVENTIONS.md says, and a glyph
//    component has none.
// Components read semantic tokens and their own tokens, never the palette.
//
// Lengths are design units, which are pixels at 1920x1080, held as Dp so they can't be mixed with
// output pixels. ScaleContext converts them for the actual output. Plain floats are values in
// output pixels, such as a type style's smallest size, the smallest targets and the reference
// resolution itself, and values with no unit, such as angles, fractions and durations.
namespace Libs::Graphics::SystemUi {

/// Tier 1. Every ramp runs dark to light, so a higher step is lighter. The neutral ramp is a pure
/// grey, which sits on most game art without tinting it. Where a value equals today's overlay, the
/// note says so, so ports keep that look.
namespace Palette {
inline constexpr Color GRAY_0   = Rgb(0x070707);
inline constexpr Color GRAY_50  = Rgb(0x111111);
inline constexpr Color GRAY_100 = Rgb(0x1C1C1C);
inline constexpr Color GRAY_150 = Rgb(0x272727);
inline constexpr Color GRAY_200 = Rgb(0x333333);
inline constexpr Color GRAY_300 = Rgb(0x4D4D4D);
inline constexpr Color GRAY_400 = Rgb(0x676767);
inline constexpr Color GRAY_500 = Rgb(0x848484);
inline constexpr Color GRAY_600 = Rgb(0xA1A1A1);
inline constexpr Color GRAY_700 = Rgb(0xBFBFBF);
inline constexpr Color GRAY_800 = Rgb(0xDFDFDF);
inline constexpr Color GRAY_900 = Rgb(0xFFFFFF);

inline constexpr Color BLUE_300 = Rgb(0x0159B3);
inline constexpr Color BLUE_350 = Rgb(0x0B64C4); // the accent fill, dark enough for a white label
inline constexpr Color BLUE_400 = Rgb(0x146FD5);
inline constexpr Color BLUE_450 = Rgb(0x3287EF);
inline constexpr Color BLUE_500 = Rgb(0x4593F7);
inline constexpr Color BLUE_600 = Rgb(0x57A0FE);
inline constexpr Color BLUE_700 = Rgb(0x85B9FE);

/// The status hues' lightest steps are for status text, bands and glyphs on the translucent
/// surfaces, where the 500s are too dark to read over a bright game frame.
inline constexpr Color GREEN_500  = Rgb(0x52CA51);
inline constexpr Color GREEN_600  = Rgb(0x65D064);
inline constexpr Color YELLOW_500 = Rgb(0xE8992E);
inline constexpr Color YELLOW_600 = Rgb(0xEDAD58);
inline constexpr Color RED_400    = Rgb(0xA21B02);
inline constexpr Color RED_500    = Rgb(0xCD3319);
inline constexpr Color RED_600    = Rgb(0xF1573D);
inline constexpr Color RED_700    = Rgb(0xF58977);
inline constexpr Color PLATINUM   = Rgb(0xA1E1F5); // today's trophy grade colours
inline constexpr Color GOLD       = Rgb(0xFFCF52);
inline constexpr Color SILVER     = Rgb(0xD3DCE8);
inline constexpr Color BRONZE     = Rgb(0xD38E5B);
} // namespace Palette

/// Tier 1 opacities. The colour tokens below take their alpha from here, so a re-theme or a
/// high-contrast variant changes each one in one place.
namespace Opacity {
inline constexpr float HOVER           = 0.06f;
inline constexpr float FOCUS           = 0.10f;
inline constexpr float BORDER_SUBTLE   = 0.11f;
inline constexpr float TINT            = 0.16f;
inline constexpr float PRESSED         = 0.20f;
inline constexpr float BORDER_STANDARD = 0.20f;
inline constexpr float INSET           = 0.35f;
inline constexpr float DISABLED        = 0.38f;
inline constexpr float BORDER_STRONG   = 0.40f;
inline constexpr float KEYLINE         = 0.45f;
inline constexpr float SCRIM           = 0.62f;
inline constexpr float HUD             = 0.80f;
inline constexpr float HUD_STRONG      = 0.84f;
inline constexpr float PANEL           = 0.92f;
inline constexpr float OVERLAY         = 0.96f;
} // namespace Opacity

/// Tier 2. Surfaces are translucent over the game, so their alpha is part of the token.
struct SurfaceColors {
	Color base    = Palette::GRAY_100.WithAlpha(Opacity::PANEL);   // full-screen menus
	Color raised  = Palette::GRAY_150.WithAlpha(Opacity::PANEL);   // cards, toasts, dialogs
	Color overlay = Palette::GRAY_200.WithAlpha(Opacity::OVERLAY); // popups above raised surfaces
	Color hud     = Palette::GRAY_50.WithAlpha(Opacity::HUD); // non-interactive heads-up display
	Color hud_strong = Palette::GRAY_50.WithAlpha(Opacity::HUD_STRONG);
	Color inset = Palette::GRAY_0.WithAlpha(Opacity::INSET); // wells, graph backgrounds, tracks
	Color scrim = Palette::GRAY_0.WithAlpha(Opacity::SCRIM); // behind modals
};

/// Text levels, from most to least prominent. The gallery's Colour page shows the contrast of each.
struct TextColors {
	Color primary   = Palette::GRAY_900;
	Color secondary = Palette::GRAY_700;
	Color tertiary  = Palette::GRAY_600;
	Color disabled  = Palette::GRAY_500;
	Color on_accent = Palette::GRAY_900;
	Color accent    = Palette::BLUE_500;
	// For words on a light surface, as every other text colour here is for a dark one.
	Color on_light           = Palette::GRAY_0;
	Color secondary_on_light = Palette::GRAY_400;
};

/// The accent, the colour of what the player can act on and what is chosen.
struct AccentColors {
	Color fill         = Palette::BLUE_350;
	Color fill_hover   = Palette::BLUE_400;
	Color fill_pressed = Palette::BLUE_300;
	Color line         = Palette::BLUE_500; // selection bars, progress fill
	Color tint         = Palette::BLUE_500.WithAlpha(Opacity::TINT); // selected row background
	// Opaque, since a glow takes its strength from its Glow token.
	Color glow = Palette::BLUE_500;
};

/// The status colours, one for each Status, and the faint tints behind them.
struct StatusColors {
	// Lighter than their tints, as these are read as text, on a tint or a raised surface.
	Color success = Palette::GREEN_600;
	Color warning = Palette::YELLOW_600;
	Color error   = Palette::RED_700;
	Color info    = Palette::BLUE_700;
	// A fill for an action that destroys something, dark enough for a white label.
	Color danger_fill = Palette::RED_400;
	// Faint backgrounds for badges and toast bars in each status colour.
	Color success_tint = Palette::GREEN_500.WithAlpha(Opacity::TINT);
	Color warning_tint = Palette::YELLOW_500.WithAlpha(Opacity::TINT);
	Color error_tint   = Palette::RED_500.WithAlpha(Opacity::TINT);
	Color info_tint    = Palette::BLUE_600.WithAlpha(Opacity::TINT);
};

/// Thresholds in graphs and the performance overlay.
struct ChartColors {
	Color good    = Palette::GREEN_500;
	Color medium  = Palette::YELLOW_500;
	Color bad     = Palette::RED_500;
	Color neutral = Palette::GRAY_600;
};

/// The focus ring is a thin dark keyline against the element, a light stroke outside it and a soft
/// glow, so it reads on light and dark game frames alike.
struct FocusColors {
	Color ring    = Palette::GRAY_900;
	Color keyline = Palette::GRAY_0.WithAlpha(Opacity::KEYLINE);
	// Opaque, since a glow takes its strength from its Glow token.
	Color glow = Palette::BLUE_500;
};

/// The hairline edges that part surfaces, from subtle to strong.
struct BorderColors {
	Color subtle   = Palette::GRAY_900.WithAlpha(Opacity::BORDER_SUBTLE);
	Color standard = Palette::GRAY_900.WithAlpha(Opacity::BORDER_STANDARD);
	Color strong   = Palette::GRAY_900.WithAlpha(Opacity::BORDER_STRONG);
};

/// Overlays drawn on top of a control to show its state.
struct StateColors {
	Color hover   = Palette::GRAY_900.WithAlpha(Opacity::HOVER);
	Color focus   = Palette::GRAY_900.WithAlpha(Opacity::FOCUS);
	Color pressed = Palette::GRAY_0.WithAlpha(Opacity::PRESSED);
	// For what sits on a light surface, where a light overlay wouldn't show. A press already
	// darkens.
	Color hover_on_light = Palette::GRAY_0.WithAlpha(Opacity::HOVER);
	Color focus_on_light = Palette::GRAY_0.WithAlpha(Opacity::FOCUS);
};

/// The colour of each trophy grade.
struct TrophyColors {
	Color platinum = Palette::PLATINUM;
	Color gold     = Palette::GOLD;
	Color silver   = Palette::SILVER;
	Color bronze   = Palette::BRONZE;
};

/// Effects take their alpha from their effect tokens. A shadow's layers carry their own colours.
struct EffectColors {
	Color shine = Palette::GRAY_900;
};

/// Switches and sliders. A toggle's track is this grey when off and the accent fill when on, under
/// a white knob. An unfilled track is too close to the surface behind it to read by its fill, so it
/// also has a border.strong outline. A disabled slider is solid grey rather than faded, so it can't
/// be taken for an enabled one on any game frame, with no blue in its fill and no white in its
/// knob.
struct ControlColors {
	Color track_off     = Palette::GRAY_400;
	Color knob          = Palette::GRAY_900;
	Color fill_disabled = Palette::GRAY_500;
	Color knob_disabled = Palette::GRAY_600;
};

/// Every colour by its role, the tier components read.
struct SemanticColors {
	SurfaceColors surface;
	TextColors    text;
	AccentColors  accent;
	StatusColors  status;
	FocusColors   focus;
	BorderColors  border;
	StateColors   state;
	TrophyColors  trophy;
	ChartColors   chart;
	EffectColors  effect;
	ControlColors control;
};

/// A spacing scale on a 4-unit grid.
struct SpaceScale {
	Dp hair = Dp(2.0f);
	Dp s1   = Dp(4.0f);
	Dp s2   = Dp(8.0f);
	Dp s3   = Dp(12.0f);
	Dp s4   = Dp(16.0f);
	Dp s5   = Dp(24.0f);
	Dp s6   = Dp(32.0f);
	Dp s7   = Dp(48.0f);
	Dp s8   = Dp(64.0f);
	Dp s9   = Dp(96.0f);
};

/// A radius that rounds a box's corners fully, whatever its size. Drawing clamps a radius to half
/// the box's shorter side, so any value past the largest box does it. It is large and finite and
/// not infinity, since blending infinite radii between states gives inf minus inf, which is NaN.
/// Blending it with a small radius keeps the corners round almost until the end of the blend, then
/// snaps to the small one.
inline constexpr Dp RADIUS_FULL = Dp(10000.0f);

/// Corner radii. A fully rounded shape uses half its shorter side instead of a token.
struct RadiusScale {
	Dp xs = Dp(4.0f);
	Dp sm = Dp(8.0f);
	Dp md = Dp(12.0f);
	Dp lg = Dp(16.0f);
	Dp xl = Dp(24.0f);
};

/// Border widths. ScaleContext never lets a scaled border drop below one pixel.
struct BorderWidths {
	Dp hair  = Dp(1.0f); // card borders and the focus keyline, so tuning one moves both
	Dp thin  = Dp(2.0f);
	Dp thick = Dp(3.0f); // focus ring
	Dp bar   = Dp(4.0f); // selection bar
};

/// Opacities that components apply on top of a colour, such as dimming a disabled control.
struct OpacityScale {
	float hover    = Opacity::HOVER;
	float focus    = Opacity::FOCUS;
	float pressed  = Opacity::PRESSED;
	float disabled = Opacity::DISABLED;
};

/// How heavy a text style's font is.
enum class FontWeight : uint8_t { Regular, Bold };

/// One text style, its size and line height in design units. `min_size` is the smallest size in
/// output pixels it may shrink to at low resolutions, so it stays a plain float.
struct TypeStyle {
	Dp         size;
	Dp         line;
	FontWeight weight   = FontWeight::Regular;
	float      min_size = 0.0f;
};

/// The type ramp. Body styles stay at 14 pixels or more, captions at 12 and dense heads-up text
/// at 11.
struct TypeRamp {
	TypeStyle display {Dp(44.0f), Dp(52.0f), FontWeight::Bold, 14.0f};
	TypeStyle title {Dp(32.0f), Dp(40.0f), FontWeight::Bold, 14.0f};
	TypeStyle heading {Dp(26.0f), Dp(34.0f), FontWeight::Bold, 14.0f};
	TypeStyle body {Dp(22.0f), Dp(32.0f), FontWeight::Regular, 14.0f};
	TypeStyle body_small {Dp(20.0f), Dp(28.0f), FontWeight::Regular, 14.0f};
	TypeStyle label {Dp(22.0f), Dp(28.0f), FontWeight::Bold, 14.0f};
	TypeStyle caption {Dp(16.0f), Dp(22.0f), FontWeight::Regular, 12.0f};
	TypeStyle dense {Dp(15.0f), Dp(20.0f), FontWeight::Regular, 11.0f};
	TypeStyle dense_small {Dp(13.0f), Dp(18.0f), FontWeight::Regular, 11.0f};
};

/// Font files, relative to the emulator's assets folder.
struct FontFiles {
	std::string_view regular = "fonts/NotoSans-Regular.ttf";
	std::string_view bold    = "fonts/NotoSans-Bold.ttf";
};

/// A soft halo around an element, `radius` design units wide. It reaches `peak_alpha` next to
/// the element and fades toward the outside.
struct Glow {
	Dp    radius;
	float peak_alpha = 0.0f;
};

/// The theme's glows, each for one kind of moment.
struct GlowScale {
	Glow focus {Dp(12.0f), 0.35f};    // around the focused control
	Glow emphasis {Dp(24.0f), 0.45f}; // a success or an arrival
	Glow trophy {Dp(24.0f), 0.30f};   // around a trophy toast's icon, in the grade colour
};

/// One layer of a shadow, as one entry of a CSS box-shadow list. It is the element's shape grown by
/// `spread`, moved by its offsets and blurred by `blur` in `color`, whose alpha is its strength.
/// The blur is drawn as CSS draws it, half strength at the shape's edge, rising to full `blur`
/// inside it and gone `blur` outside it. A layer with no blur draws nothing, unlike a hard CSS
/// shadow, as every shadow here is soft. Lengths are design units, CSS pixels at the reference
/// size.
struct ShadowLayer {
	Dp offset_x;
	Dp offset_y;
	Dp blur;
	// How far the shape grows past the element on every side before it is blurred. A negative one
	// shrinks it.
	Dp    spread;
	Color color;

	constexpr bool operator==(const ShadowLayer&) const = default;
};

/// The most layers a shadow holds, the two a theme elevation uses and one more, such as a hairline
/// ring. Kept small, as every box style holds a shadow for itself and each of its states.
inline constexpr size_t MAX_SHADOW_LAYERS = 3;

/// A soft shadow under an element, a stack of layers in CSS order, the first drawn on top, so one
/// close tight layer and one far soft one can read as a single lift. One with no layers draws
/// nothing, which is a box's default. Build one with MakeShadow.
struct Shadow {
	std::array<ShadowLayer, MAX_SHADOW_LAYERS> layers {};
	uint8_t                                    count = 0;

	/// How many layers it holds, never more than fit, so a count set by hand can't read past them.
	[[nodiscard]] constexpr size_t Count() const {
		return std::min(static_cast<size_t>(count), MAX_SHADOW_LAYERS);
	}

	/// Whether two shadows hold the same layers, whatever is left in the slots past them, so two
	/// that draw the same are equal however they were built.
	constexpr bool operator==(const Shadow& other) const {
		if (Count() != other.Count()) {
			return false;
		}
		for (size_t index = 0; index < Count(); ++index) {
			if (!(layers[index] == other.layers[index])) {
				return false;
			}
		}
		return true;
	}
};

/// A shadow of `layers`, as written in a CSS box-shadow list. Layers past MAX_SHADOW_LAYERS are
/// dropped.
[[nodiscard]] constexpr Shadow MakeShadow(std::initializer_list<ShadowLayer> layers) {
	Shadow shadow;
	for (const ShadowLayer& layer: layers) {
		if (shadow.count < MAX_SHADOW_LAYERS) {
			shadow.layers[shadow.count++] = layer;
		}
	}
	return shadow;
}

/// The colour every theme shadow is cast in, a near black with no tint, so a shadow darkens what
/// is under it without colouring it.
inline constexpr Color SHADOW_COLOR = Palette::GRAY_0;

/// The theme's shadows, from a surface just off what is under it to one floating well above it.
/// Each is a close layer that keeps the edge crisp and a far one that gives the lift.
struct Shadows {
	// A surface raised a little, such as a card or a toast over the game.
	Shadow low = MakeShadow(
	    {{.offset_y = Dp(1.0f),
	      .blur     = Dp(4.0f),
	      .spread   = Dp(1.0f),
	      .color    = SHADOW_COLOR.WithAlpha(0.10f)},
	     {.offset_y = Dp(5.0f), .blur = Dp(10.0f), .color = SHADOW_COLOR.WithAlpha(0.08f)}});
	// A surface lifted clear of what is under it, such as a menu.
	Shadow medium = MakeShadow({{.offset_y = Dp(8.0f),
	                             .blur     = Dp(15.0f),
	                             .spread   = Dp(1.0f),
	                             .color    = SHADOW_COLOR.WithAlpha(0.10f)},
	                            {.offset_y = Dp(18.0f),
	                             .blur     = Dp(24.0f),
	                             .spread   = Dp(1.0f),
	                             .color    = SHADOW_COLOR.WithAlpha(0.12f)}});
	// A surface floating well above, such as a dialog.
	Shadow high = MakeShadow({{.offset_y = Dp(10.0f),
	                           .blur     = Dp(20.0f),
	                           .spread   = Dp(1.0f),
	                           .color    = SHADOW_COLOR.WithAlpha(0.10f)},
	                          {.offset_y = Dp(24.0f),
	                           .blur     = Dp(36.0f),
	                           .spread   = Dp(1.0f),
	                           .color    = SHADOW_COLOR.WithAlpha(0.18f)}});
};

/// A soft band of light that sweeps across a surface once, leaning right.
struct ShineTokens {
	float angle_degrees    = 20.0f; // from vertical
	float width_fraction   = 0.3f;  // of the element's width
	Dp    min_width        = Dp(80.0f);
	float peak_alpha       = 0.14f;
	float plateau_fraction = 0.2f; // of the band, at full brightness in the middle
};

/// The curves the theme moves things along.
enum class Ease : uint8_t {
	Linear,   // spinners, indeterminate progress
	Enter,    // things arriving, 1 - (1 - t)^3
	Exit,     // things leaving, t^3
	Standard, // moves, resizes, scrolling and the shine sweep, cubic in and out
	Emphasis, // a success pop or a toggle knob, slightly overshooting
};

/// Durations in milliseconds. Exits take about 65% of the matching enter.
struct MotionDurations {
	float instant  = 0.0f;
	float fast     = 100.0f;
	float base     = 180.0f;
	float slow     = 280.0f;
	float slower   = 420.0f;
	float emphasis = 600.0f;
	float shine    = 700.0f;
	// How long a toast's shine waits once the toast starts arriving, so it crosses a toast that has
	// come to rest rather than one still sliding in. The slower step.
	float shine_delay = slower;
	// A quick fade out, for a toast leaving once its time is up.
	float exit_fade = 120.0f;
	float pulse     = 1600.0f;
	// The longest fade when the player asks for reduced motion.
	float reduced_fade_max = 120.0f;
};

/// How design units map to the output. The scale is
/// clamp(min(width / reference_width, height / reference_height) * user scale, min, max).
struct ScaleRules {
	float reference_width  = 1920.0f;
	float reference_height = 1080.0f;
	float min_scale        = 2.0f / 3.0f; // 720p
	float max_scale        = 3.0f;        // 8K
	float min_user_scale   = 0.8f;
	float max_user_scale   = 1.5f;
	// The safe area keeps content away from the screen edges, as TVs may crop them. Its inset is
	// a percentage of the screen, but never less than `edge_min`.
	float default_safe_area_percent = 3.5f;
	float max_safe_area_percent     = 10.0f;
	Dp    edge_min                  = Dp(32.0f);
	// Menus and dialogs sit in a frame no wider than this aspect, centred in the safe area, so
	// they don't stretch across ultrawide screens.
	float content_aspect = 16.0f / 9.0f;

	/// True when the rules can produce a finite scale. A theme must pass this before use.
	[[nodiscard]] constexpr bool IsValid() const {
		return reference_width > 0.0f && reference_height > 0.0f && min_scale > 0.0f &&
		       min_scale <= max_scale && min_user_scale > 0.0f &&
		       min_user_scale <= max_user_scale && max_safe_area_percent >= 0.0f &&
		       edge_min.value >= 0.0f && content_aspect > 0.0f;
	}
};

/// How controls respond to focus, pointer hover and presses, and how a held direction repeats.
/// Focus arrives a little slower than it leaves, so moving along a row reads as one highlight
/// travelling.
struct InteractionTokens {
	float focus_in_ms  = MotionDurations {}.fast;
	float focus_out_ms = 60.0f;
	float hover_ms     = MotionDurations {}.fast;
	float press_ms     = MotionDurations {}.fast;
	// A pressed control shrinks slightly about its centre.
	float press_scale = 0.98f;
	// A toggle's knob, and a tab bar's indicator moving to the chosen tab.
	float knob_ms      = MotionDurations {}.base;
	float indicator_ms = MotionDurations {}.slow;
	// A held direction waits this long, then repeats at this interval.
	float repeat_delay_ms = 400.0f;
	float repeat_rate_ms  = 80.0f;
	// The smallest targets, in output pixels after scaling, not design units. Gamepad
	// targets need more room than pointer targets.
	float min_gamepad_target = 44.0f;
	float min_pointer_target = 32.0f;
};

/// Everything a component needs to look consistent. DEFAULT_THEME is the only theme for now.
struct ThemeContext {
	SemanticColors    colors;
	SpaceScale        space;
	RadiusScale       radius;
	BorderWidths      border;
	OpacityScale      opacity;
	TypeRamp          type;
	FontFiles         fonts;
	GlowScale         glow;
	ShineTokens       shine;
	MotionDurations   motion;
	ScaleRules        scale;
	InteractionTokens interaction;
	Shadows           shadows;
};

/// The theme the system UI draws with, every token at its value above.
inline constexpr ThemeContext DEFAULT_THEME {};

// A runtime theme is built by copying DEFAULT_THEME and changing fields, so the tree must stay a
// plain aggregate. A component's own sizes live with the component, in a tokens struct built from
// the theme by its XDefaults function, so changing a scale on a copy changes the components that
// use it.
static_assert(std::is_aggregate_v<ThemeContext> && std::is_trivially_copyable_v<ThemeContext>);
static_assert(DEFAULT_THEME.scale.IsValid());

/// The colour of `status` in `theme`, its role among the theme's status colours.
[[nodiscard]] constexpr Color StatusColor(const ThemeContext& theme, Status status) {
	switch (status) {
		case Status::Warning: return theme.colors.status.warning;
		case Status::Error: return theme.colors.status.error;
		case Status::Success: return theme.colors.status.success;
		case Status::Info: break;
	}
	return theme.colors.status.info;
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_CORE_THEME_H_
