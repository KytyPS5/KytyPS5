#ifndef KYTY_GRAPHICS_HOST_GPU_RENDERER_CACHE_TEXTURETRACKRANGE_H_
#define KYTY_GRAPHICS_HOST_GPU_RENDERER_CACHE_TEXTURETRACKRANGE_H_

// Bytes of a tracked image interval to release when a CPU write peels one edge.
// The tracked interval is [track_begin, track_end). A peel must leave that interval
// ordered. The old peel moved one end past the other when the image occupied only the
// partial page, and the next full untrack subtracted the ends (PageManager fatal,
// size = end - begin wrapped, e.g. vaddr 0x571b2dc00 size 0xfffffffffffff400).

#include "common/alignment.h"

#include <cstdint>

namespace Libs::Graphics {

inline constexpr uint64_t kTrackPage = 4096;

struct TrackedEdge {
	uint64_t address     = 0; // first byte passed to the page manager
	uint64_t size        = 0; // 0: nothing to unwatch
	uint64_t track_begin = 0; // interval after the peel
	uint64_t track_end   = 0;
};

// Bytes of [begin, end). An inverted interval is empty: there is no such range.
inline uint64_t TrackedBytes(uint64_t begin, uint64_t end) {
#if defined(KYTY_TRACKEDGE_MUTANT_NO_CLAMP)
	return end - begin;
#else
	return end > begin ? end - begin : 0;
#endif
}

// Drop the partial first page. An image that ends before the next page boundary has no
// full page left, so the whole tracked interval is released and the ends meet.
inline TrackedEdge ReleaseTrackedHead(uint64_t image_begin, uint64_t track_begin,
                                      uint64_t track_end) {
	const uint64_t boundary = Common::AlignDown(image_begin + kTrackPage, kTrackPage);
#if defined(KYTY_TRACKEDGE_MUTANT_NO_CLAMP)
	return {image_begin, boundary - image_begin, boundary, track_end};
#else
	if (track_end <= track_begin) {
		return {0, 0, track_begin, track_begin};
	}
	const uint64_t release_end = boundary < track_end ? boundary : track_end;
	return {track_begin, TrackedBytes(track_begin, release_end), release_end, track_end};
#endif
}

// Drop the partial last page [AlignDown(image_end), image_end). When that page starts
// before the tracked interval, the image has no full page left: release what is still
// tracked and close the interval, instead of storing the page base as the new end.
inline TrackedEdge ReleaseTrackedTail(uint64_t image_end, uint64_t track_begin,
                                      uint64_t track_end) {
	const uint64_t boundary = Common::AlignDown(image_end, kTrackPage);
#if defined(KYTY_TRACKEDGE_MUTANT_NO_CLAMP)
	return {boundary, image_end - boundary, track_begin, boundary};
#else
	if (track_end <= track_begin) {
		return {0, 0, track_begin, track_begin};
	}
	if (boundary < track_begin) {
		return {track_begin, TrackedBytes(track_begin, track_end), track_begin, track_begin};
	}
	return {boundary, TrackedBytes(boundary, image_end), track_begin, boundary};
#endif
}

} // namespace Libs::Graphics

#endif
