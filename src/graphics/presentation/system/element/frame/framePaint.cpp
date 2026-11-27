#include "graphics/presentation/system/element/frame/framePaint.h"

#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "imgui.h"

#include <array>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace Libs::Graphics::SystemUi {

namespace {

// Whether `element`'s children are clipped, and to what, from its clip hook. Flex snaps what it
// places, but a rectangle the caller places need not be whole, so the clip is rounded out to whole
// pixels. The renderer cuts a clip to whole pixels by dropping the fractions, which would
// otherwise lose up to a pixel of a child that reaches the right or bottom edge while the
// element's own anti-aliased fill still covers it.
bool ClipsChildren(const FrameBuilder& frame, AnyElement element, const FrameHooks* hooks,
                   Rect& clip) {
	Rect hooked;
	if (hooks == nullptr || hooks->clip == nullptr || !hooks->clip(frame, element, hooked)) {
		return false;
	}
	const float left = std::floor(hooked.x);
	const float top  = std::floor(hooked.y);
	clip = {left, top, std::ceil(hooked.Right()) - left, std::ceil(hooked.Bottom()) - top};
	return true;
}

// How many layers an element can sit on, one for every value its layer can hold.
constexpr size_t LAYER_COUNT = 256;

// The channels a paint pass draws its layers into, kept in the frame so a steady frame reuses the
// splitter's buffers. Each layer drawn on has a channel, in order from the lowest, so merging the
// channels puts higher layers over lower ones.
struct PaintLayers {
	ImDrawListSplitter               splitter;
	std::array<uint8_t, LAYER_COUNT> channel {};
	// Set while a pass draws through the splitter, which can't be split again until it merges.
	bool painting = false;
};

// Where a paint pass sends each element's drawing, none when every element is on one layer and
// everything is drawn straight into the list in tree order.
struct LayerRoute {
	ImDrawListSplitter*                     splitter = nullptr;
	const std::array<uint8_t, LAYER_COUNT>* channel  = nullptr;
};

// Marks the layers of `element` and its subtree in `used`, skipping what the walk doesn't draw, so
// a layer is given a channel only when something is drawn on it.
void MarkLayers(const FrameBuilder& frame, AnyElement element, std::bitset<LAYER_COUNT>& used) {
	const FrameView::ElementLayout* const layout = frame.LayoutOf(element);
	if (layout == nullptr || layout->collapsed) {
		return;
	}
	used.set(layout->layer);
	for (size_t index = 0; index < frame.ChildCount(element); ++index) {
		MarkLayers(frame, frame.Child(element, index), used);
	}
}

// Sends what is drawn next into `layer`'s channel, when the pass is split into layers.
void DrawOnLayer(const LayerRoute& route, ImDrawList& list, uint8_t layer) {
	if (route.splitter != nullptr) {
		route.splitter->SetCurrentChannel(&list, (*route.channel)[layer]);
	}
}

// Draws `element` and its subtree within a paint pass. Private to the walk, since a paint hook
// draws only its own element and the walk paints the children after it, then what goes over them.
// The walk pushes a clip for the children and pops it after them, so the two always match. Each
// element is drawn into its layer's channel, while the clip and the state still come down the tree
// in tree order, so an element on a higher layer keeps its parent's clip and opacity.
void PaintChild(const FrameBuilder& frame, AnyElement element, ImDrawList& list,
                const PaintState& state, const LayerRoute& route) {
	const FrameView::ElementLayout* const layout = frame.LayoutOf(element);
	if (layout == nullptr || layout->collapsed) {
		// Absent, so neither it nor anything below it is drawn.
		return;
	}
	const FrameHooks* hooks = frame.FrameHooksOf(element);
	// The element's own state, which its children inherit, so a fading element fades its subtree.
	const PaintState own = hooks != nullptr && hooks->inherit != nullptr
	                           ? hooks->inherit(frame, element, state)
	                           : state;
	DrawOnLayer(route, list, layout->layer);
	if (hooks != nullptr && hooks->paint != nullptr) {
		hooks->paint(frame, element, list, own);
	}
	Rect       clip;
	const bool clipped = ClipsChildren(frame, element, hooks, clip);
	if (clipped) {
		list.PushClipRect({clip.x, clip.y}, {clip.Right(), clip.Bottom()}, true);
	}
	for (size_t index = 0; index < frame.ChildCount(element); ++index) {
		PaintChild(frame, frame.Child(element, index), list, own, route);
	}
	if (clipped) {
		list.PopClipRect();
	}
	// Over the children and outside their clip, so what an element draws over its content isn't
	// cut by the clip the content is. It is drawn on the element's own layer, over its content on
	// that layer and under any child on a higher one.
	if (hooks != nullptr && hooks->paint_over != nullptr) {
		DrawOnLayer(route, list, layout->layer);
		hooks->paint_over(frame, element, list, own);
	}
}

// Gives each layer marked in `used` a channel, in order from the lowest, and returns how many.
// Every layer gets one, an unmarked layer the channel of the nearest marked layer below it, so an
// element the marking walk missed still draws into a channel that exists rather than past the
// last.
int AssignChannels(const std::bitset<LAYER_COUNT>&   used,
                   std::array<uint8_t, LAYER_COUNT>& channel) {
	int count = 0;
	for (size_t layer = 0; layer < LAYER_COUNT; ++layer) {
		count += used.test(layer) ? 1 : 0;
		channel[layer] = static_cast<uint8_t>(count > 0 ? count - 1 : 0);
	}
	return count;
}

} // namespace

void Paint(AnyElement root, ImDrawList& list, const PaintState& state) {
	FrameBuilder&            frame = CurrentBuilder();
	std::bitset<LAYER_COUNT> used;
	MarkLayers(frame, root, used);
	// Everything on one layer is drawn straight into the list, as tree order is already the order
	// to draw in. An absent root marks no layer and draws nothing, and a split into no channels
	// isn't allowed, so it takes this path too.
	if (used.count() < 2) {
		PaintChild(frame, root, list, state, {});
		return;
	}
	PaintLayers& layers = frame.Workspace<PaintLayers>();
	assert(!layers.painting && "Paint is never called again while it paints");
	if (layers.painting) {
		// A Paint inside another can't split the list again before the outer one merges, so it
		// draws flat into the outer pass's current channel rather than lose what the outer drew.
		PaintChild(frame, root, list, state, {});
		return;
	}
	layers.painting = true;
	layers.splitter.Split(&list, AssignChannels(used, layers.channel));
	PaintChild(frame, root, list, state,
	           {.splitter = &layers.splitter, .channel = &layers.channel});
	layers.splitter.Merge(&list);
	layers.painting = false;
}

} // namespace Libs::Graphics::SystemUi
