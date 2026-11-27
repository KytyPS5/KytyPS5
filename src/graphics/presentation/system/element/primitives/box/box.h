#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_BOX_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_BOX_H_

#include "graphics/presentation/system/common/props/direction.h"
#include "graphics/presentation/system/common/props/itemLayout.h"
#include "graphics/presentation/system/common/props/justify.h"
#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/gradient.h"
#include "graphics/presentation/system/core/key.h"
#include "graphics/presentation/system/core/theme.h"
#include "graphics/presentation/system/core/units.h"
#include "graphics/presentation/system/element/children.h"
#include "graphics/presentation/system/element/element.h"

#include <cstdint>
#include <optional>

namespace Libs::Graphics::SystemUi {

struct BoxProps;

/// A box's element, what Box and the components built on it return.
using BoxElement = Element<BoxProps>;

/// How a box lays out its children. What the box itself asks of its container is in BoxProps::self.
struct BoxLayout {
	FlexDirection direction = FlexDirection::Row;
	Justify       justify   = Justify::Start;
	Align         align     = Align::Start;
	// Space between neighbouring children on the main axis.
	Dp gap;
	// Space between the box's edge and its children. A box with none set has none, and a component
	// built on a box can give its own default instead, as a card gives the theme's, so a caller who
	// sets one, even zero, always gets it. Unset is not zero, so `{}` leaves it unset and
	// `DpInsets {}` asks for none.
	std::optional<DpInsets> padding;
	// Lays the children over one another instead of along an axis, each filling the box inside its
	// padding whatever length it asks for, and in order, so on one layer a later child is drawn
	// over an earlier one. The box measures as its largest child. Direction, justify, align and gap
	// are ignored, and so are a child's grow, limits, alignment and collapses, so a stack never
	// collapses a child. It suits parts of one control that share a rectangle, such as a toggle's
	// track, shade, knob and ring. A stacked box is not a list and does not scroll, so AsList is
	// not supported on one, Overflow::Scroll only clips it, and it is not laid out by the flex
	// solver.
	bool stacked = false;
};

/// How a box differs while it is in a state. Fields left unset leave the box as it is. A state set
/// to a value moves the box that far toward it as the state comes on, and the colours blend by the
/// state's weight. The overlay is drawn over the box at the state's weight, the scale shrinks the
/// box about its centre, and the focus ring and the selection show at the state's weight. A
/// state doesn't change the box's layer, which stays where its styles put it. A shadow blends layer
/// by layer, each layer's offsets, blur, spread and colour by the weight, a layer only one side has
/// fading in or out from clear, so a hover can lift a box gradually. Lengths are design units.
struct StateStyle {
	std::optional<Color>  fill;
	std::optional<Color>  border;
	std::optional<Dp>     border_width;
	std::optional<Dp>     radius;
	Color                 overlay;
	std::optional<float>  opacity;
	std::optional<float>  scale;
	bool                  focus_ring = false;
	bool                  selection  = false;
	std::optional<Shadow> shadow;
	// Gradients the box's fill and border move toward, end by end, with the angle turning the
	// short way round. A state that sets no gradient of its own moves a gradient's ends toward
	// its plain fill or border colour instead.
	std::optional<LinearGradient> fill_gradient;
	std::optional<LinearGradient> border_gradient;
};

/// The states a box responds to, applied in the order selected, hover, focus, pressed, disabled. A
/// state added here also goes in BOX_STATE_COUNT and InOrder, in interactionStates.
struct StateStyles {
	StateStyle hover;
	StateStyle focus;
	StateStyle pressed;
	StateStyle disabled;
	StateStyle selected;
};

/// What the player is doing to the control a box stands for, which its state styles respond to. The
/// highlights are how far each has come, from 0 to 1, so a highlight can fade in, and disabled and
/// selected are on or off. The caller sets it until interaction fills it in.
struct StateInput {
	float hover    = 0.0f;
	float focus    = 0.0f;
	float pressed  = 0.0f;
	bool  disabled = false;
	bool  selected = false;
};

/// Whether a box's children may draw outside it. Clip keeps them inside the box's outer rectangle,
/// so children may draw over the border band. That matches the toast today and not CSS, which clips
/// to the padding box. The clip is a rectangle, so it doesn't follow rounded corners. Children
/// inside padding of at least 1 - 1/sqrt(2), about 0.3, of the radius stay inside the curve, as
/// InsideCorners works out. Children that overflow the padding show in the corners. The clip is
/// rounded out to whole pixels, so it never cuts a child short of the box's edge. The box's own
/// effects are not clipped by it. Text keeps its size when a pose scales the box down, so text in a
/// scaled-down clipping box can be cut at its edges, as it can in CSS.
///
/// Scroll lays the children out along the main axis at their own lengths, never shrinking or
/// collapsing them, and the box is no longer on that axis than the room it is given, so children
/// past its end overflow it. Its least length on that axis is its padding, as in CSS, so a
/// container can shorten it. It clips its children inside its border, the padding box as in CSS,
/// so they never draw over the border. The clip is a rectangle, so content that scrolls past a
/// rounded corner shows in the corner outside the curve, and content scrolls over an edge band, as
/// the band lies under the border. A column scrolls with the vertical wheel and a row with the
/// horizontal one, while the pointer is over it and its content doesn't fit. Where scrolling boxes
/// nest, the innermost under the pointer that can scroll along the wheel's axis takes it, and the
/// wheel doesn't pass to the one around it. It eases there, or moves straight there with reduced
/// motion, and never scrolls past either end of its content. A stacked box with Scroll doesn't
/// scroll, as it lays out as a stack, and clips as Scroll does.
enum class Overflow : uint8_t {
	Visible,
	Clip,
	Scroll,
};

/// A band of colour along a box's top edge, following its rounded corners, such as the bar that
/// tells a toast's statuses apart. It is drawn over the fill and under the border, so the border
/// still draws the outline. A clear colour or zero width draws none.
struct EdgeBand {
	Color color;
	Dp    width;
};

/// A soft glow of colour around a box, following its rounded corners, such as the light around a
/// trophy's icon. It is drawn outside the box and leaves the box itself uncovered, so a translucent
/// fill isn't tinted. A clear colour, no reach or no strength draws none.
struct BoxGlow {
	// How far it reaches and how strong it is at the box's edge, one of the theme's glows.
	Glow  glow;
	Color color;
};

/// How a box looks. A transparent border or a zero border width draws no border. Lengths are
/// design units, converted when the box is drawn.
struct BoxStyles {
	Color fill;
	Color border;
	Dp    border_width;
	Dp    radius;
	// The layer the box and everything inside it are drawn on, 0 the lowest. Within one Paint, a
	// box on a higher layer is drawn over everything on lower ones, wherever it sits in the tree,
	// and a child asking for a lower layer than its box is drawn on its box's. On one layer, boxes
	// are drawn in tree order. The layer alone casts no shadow. The shadow below gives one.
	uint8_t layer = 0;
	// Whether the box's children may draw outside it.
	Overflow overflow = Overflow::Visible;
	// How the box changes with the state of the control it stands for.
	StateStyles states;
	// The soft shadow under the box, its layers in their own colours. None by default. It is
	// filled under the box too, so a translucent fill shows it through. A box draws it when it
	// paints, so it covers what is drawn before it on its layer and everything on lower layers, as
	// the blur spreads on every side.
	Shadow shadow = {};
	// A band along the top edge. None by default. States don't change it.
	EdgeBand edge = {};
	// A glow around the box, drawn over its shadow and under its body. None by default. States
	// don't change it.
	BoxGlow glow = {};
	// Gradients drawn in place of the fill and border colours when set, each spanning the whole
	// box.
	std::optional<LinearGradient> fill_gradient;
	std::optional<LinearGradient> border_gradient;
};

/// Everything that shapes a box apart from its children, so a box takes one argument. The layout is
/// how it arranges its children, and the self is what it asks of its container and how it looks.
/// They stay separate types, so a helper can build either one whole.
struct BoxProps {
	/// What a box asks of its container and how it looks. Each element kind keeps what it is apart
	/// from its content in a nested Self, whose layout goes to AddNode.
	struct Self {
		ItemLayout layout;
		BoxStyles  styles;
		// What the player is doing to the control the box stands for, which picks how far each of
		// the style's states shows. At rest unless the caller says otherwise.
		StateInput state = {};
	};

	BoxLayout layout;
	Self      self;
	// Tells the box apart from every other in the frame, so the frame can keep its state, such as
	// a scroll position, from frame to frame. Keys are frame wide, so two boxes that keep state
	// need two keys, as Key("row", index) gives the rows of a loop. Two scrolling boxes sharing one
	// in a frame are a mistake that asserts in a debug build, and otherwise share one position,
	// which the wheel moves once for each of them. A scrolling box without one is told apart by
	// how many unkeyed scrolling boxes were built before it in the frame, which holds while that
	// number and order don't change. Only a box with Overflow::Scroll reads it yet.
	Key key;
};

/// A box with `children`, laid out and drawn as `props` says, built in the current frame. The
/// children are listed in braces, as in Box(props, {title, rows}), or given as a run such as a
/// vector, as Children describes. Each child can have one parent only. A refused box is a none
/// handle, and a RefusalScope open around the call says why. The box keeps its props whole as its
/// data, so its element is a BoxElement, and only the element layer can read them back. A box that
/// scrolls keeps its position by its key, so it can be built anywhere, including inside a function
/// that builds boxes for its callers.
[[nodiscard]] BoxElement Box(const BoxProps& props, const Children& children = {});

/// True while `box`, built in the current frame, is still easing to where the wheel sent it, so
/// the UI must keep drawing frames. A box that doesn't scroll never is.
[[nodiscard]] bool ScrollNeedsFrames(BoxElement box);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_BOX_H_
