#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_PAINT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_PAINT_H_

#include "graphics/presentation/system/element/element.h"
#include "graphics/presentation/system/element/frame/frameHooks.h"

// The Box element's painting. Internal to the box primitive, so components don't include it.
namespace Libs::Graphics::SystemUi {

/// Draws a box where it was arranged, faded by `state`, as the box's paint hook. Its style is
/// blended with its states for its input first, then it draws its shadow, its fill and border, the
/// theme's selection, the overlays its states ask for and the theme's focus ring. The ring lies
/// over the border's band when the box draws a border, so it takes the border's place, and
/// otherwise just outside the box. The selection and ring are as strong as the states that ask for
/// them. Its children are drawn over it by the generic walk, so a child reaching the box's edge
/// draws over the ring's inner band, as it would over the border. A state's scale isn't drawn
/// yet.
void PaintBox(const FrameBuilder& frame, AnyElement box, ImDrawList& list, const PaintState& state);

/// The state a box paints with and passes to its children, as the box's inherit hook. Its states'
/// opacity, held to 0 through 1, multiplies the opacity its parent gave, so a disabled box fades
/// with everything inside it. It reads the theme current when it paints.
[[nodiscard]] PaintState InheritBox(const FrameBuilder& frame, AnyElement box,
                                    const PaintState& parent);

/// Whether a box clips its children, and to what, set into `out`, as the box's clip hook. A box
/// with Overflow::Clip clips them to its outer rectangle, one with Overflow::Scroll to the
/// rectangle inside the border it draws, and one with Overflow::Visible doesn't clip them. It
/// reads the theme current when it paints.
[[nodiscard]] bool ClipBox(const FrameBuilder& frame, AnyElement box, Rect& out);

/// Draws what goes over a box's children into `list`, as the box's paint_over hook. A box that
/// scrolls and whose content doesn't fit draws its scroll indicator, a thin pill in the theme's
/// strong border colour inside its border, clear of its corners where there is room, along the
/// right edge of a column or the bottom of a row, always shown while the content overflows. It
/// moves as the content does, so it eases and snaps with it. It reads the theme current when it
/// paints.
void PaintBoxOver(const FrameBuilder& frame, AnyElement box, ImDrawList& list,
                  const PaintState& state);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_BOX_PAINT_H_
