#ifndef KYTY_GRAPHICS_PRESENTATION_PREPARED_FRAME_SELECTION_H_
#define KYTY_GRAPHICS_PRESENTATION_PREPARED_FRAME_SELECTION_H_

#include <cstddef>
#include <span>

namespace Libs::Graphics {
// Caller owns the free list and performs the selected frame's lifetime waits.
template<typename Frame, typename Compatible, typename Ready>
size_t SelectPreparedFrame(std::span<Frame* const> frames, Compatible compatible, Ready ready) {
	size_t matching = frames.size(), ready_other = frames.size();
	for (size_t i = 0; i < frames.size(); ++i) {
		const bool matches = compatible(*frames[i]);
		if (matches && matching == frames.size()) matching = i;
		if (ready(*frames[i])) {
			if (matches) return i;
			if (ready_other == frames.size()) ready_other = i;
		}
	}
	// Reconfiguration of a ready frame avoids stalling the renderer on a
	// compatible allocation still being consumed by presentation or DLSS-G.
	if (ready_other != frames.size()) return ready_other;
	if (matching != frames.size()) return matching;
	return frames.empty() ? frames.size() : frames.size() - 1;
}
} // namespace Libs::Graphics
#endif
