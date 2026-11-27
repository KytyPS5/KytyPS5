#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_HOOKS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_HOOKS_H_

#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/element/element.h"

#include <cstdint>

/// Only named, so the frame's headers don't pull in ImGui.
struct ImDrawList;

namespace Libs::Graphics::SystemUi {

class FrameBuilder;
struct PaintState;

/// What measuring one element found, its size and the least size its content can take on each
/// axis.
struct Measured {
	Vec2 size;
	Vec2 min_content;
};

/// What one kind of element does in a frame, how it is measured, placed and painted. Each
/// primitive gives its frame these for its own props type, and generic layout and paint call them
/// through the element's kind, so neither the frame nor layout nor paint needs a list of kinds.
/// Every frame needs them set before its elements are laid out, so a primitive sets them on the
/// frame it builds in. A hook left unset does nothing for that kind.
struct FrameHooks {
	// Measures the element in the space `max`, which may be infinite on either axis. Without it
	// the element measures to nothing.
	Measured (*measure)(FrameBuilder& frame, AnyElement element, Vec2 max) = nullptr;
	// Places what is below the element inside `rect`, where the element itself has been put.
	// Returns false when something below it can't be placed. Without it there is nothing below the
	// element to place.
	bool (*arrange)(FrameBuilder& frame, AnyElement element, const Rect& rect) = nullptr;
	// Draws the element itself, where it was arranged, into `list` with `state`, the state its
	// inherit hook gave, or else its parent's. Its children are painted after it by the generic
	// walk, so a hook draws only its own element. Without it the element draws nothing, though
	// its children still do. Painting only reads what layout left, so the hook is given a frame it
	// can't change.
	void (*paint)(const FrameBuilder& frame, AnyElement element, ImDrawList& list,
	              const PaintState& state) = nullptr;
	// The state the element paints with and passes to its children, from the state its parent
	// gave it, such as its own opacity multiplied in so it fades with everything inside it. The
	// walk calls it before the paint hook. Without it the element and its children paint with
	// what the parent gave.
	PaintState (*inherit)(const FrameBuilder& frame, AnyElement element,
	                      const PaintState& parent) = nullptr;
	// Whether the element's children are clipped, and if so to what, set into `out`. The walk
	// clips them to that rectangle rounded out to whole pixels, within whatever clips the element
	// already, while it paints them. The element itself isn't clipped by it. Without the hook the
	// children are clipped as the element is.
	bool (*clip)(const FrameBuilder& frame, AnyElement element, Rect& out) = nullptr;
	// Draws what goes over the element's children, such as a scroll indicator, into `list` with
	// the same state as its paint hook. The walk calls it after the children, once their clip is
	// gone, so it is clipped as the element itself is. Without it nothing is drawn over the
	// children.
	void (*paint_over)(const FrameBuilder& frame, AnyElement element, ImDrawList& list,
	                   const PaintState& state) = nullptr;
	/// The layer the element asks to sit on, 0 the lowest. Arrange raises it to its parent's, so an
	/// element never sits below the one holding it, and Paint draws higher layers over lower ones.
	/// Without it the element asks for layer 0 and so sits on its parent's.
	uint8_t (*layer)(const FrameBuilder& frame, AnyElement element) = nullptr;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_FRAME_FRAME_HOOKS_H_
