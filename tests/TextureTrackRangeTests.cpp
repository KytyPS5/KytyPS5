#include "graphics/host_gpu/renderer/cache/textureTrackRange.h"

#include <cstdint>
#include <cstdio>

using Libs::Graphics::ReleaseTrackedHead;
using Libs::Graphics::ReleaseTrackedTail;
using Libs::Graphics::TrackedBytes;
using Libs::Graphics::kTrackPage;

static int g_fails = 0;

static void Check(bool ok, const char* what) {
	if (!ok) {
		std::fprintf(stderr, "texture_track_range: %s\n", what);
		g_fails++;
	}
}

// A release the page manager can accept: empty, or a non-wrapping guest range.
static bool ReleaseOk(uint64_t address, uint64_t size) {
	constexpr uint64_t kSpace = 1ull << 40;
	if (size == 0) return true;
	return address != 0 && address < kSpace && size <= kSpace - address && size <= 16ull * kTrackPage;
}

int main() {
	// The crash subtraction. end < begin must be an empty range, not a wrapped size.
	Check(TrackedBytes(0x571b2dc00ull, 0x571b2d000ull) == 0, "user tail bytes empty");
	Check(TrackedBytes(0x571b30000ull, 0x571b2fc00ull) == 0, "s501 head bytes empty");
	Check(TrackedBytes(0x571b2dc00ull, 0x571b2e000ull) == 0x400, "ordered partial page");

	// User crash: image starts 0xC00 into the page and ends in that same page.
	// The old tail stored the page base as track_end, and the next untrack wrapped.
	{
		constexpr uint64_t begin = 0x571b2dc00ull;
		constexpr uint64_t end   = 0x571b2df00ull;
		const auto         tail  = ReleaseTrackedTail(end, begin, end);
		Check(tail.track_begin <= tail.track_end, "user tail stays ordered");
		Check(tail.track_begin == tail.track_end, "user tail closes the interval");
		Check(tail.address == begin && tail.size == end - begin, "user tail releases the image");
		Check(ReleaseOk(tail.address, tail.size), "user tail release is a real range");
		Check(TrackedBytes(tail.track_begin, tail.track_end) == 0, "user tail leaves nothing");
	}

	// S501 crash: image ends 0x400 before the next page. The old head advanced
	// track_begin to that page, past track_end, and the next untrack wrapped by 0x400.
	{
		constexpr uint64_t begin = 0x571b2f800ull;
		constexpr uint64_t end   = 0x571b2fc00ull;
		const auto         head  = ReleaseTrackedHead(begin, begin, end);
		Check(head.track_begin <= head.track_end, "s501 head stays ordered");
		Check(head.track_begin == head.track_end, "s501 head closes the interval");
		Check(head.address == begin && head.size == end - begin, "s501 head releases the image");
		Check(ReleaseOk(head.address, head.size), "s501 head release is a real range");
		Check(TrackedBytes(head.track_begin, head.track_end) == 0, "s501 head leaves nothing");
	}

	// An image that crosses pages still peels one partial edge and keeps the full pages.
	{
		constexpr uint64_t begin = 0x571b2dc00ull;
		constexpr uint64_t end   = 0x571b31200ull;
		const auto         head  = ReleaseTrackedHead(begin, begin, end);
		Check(head.address == begin && head.size == 0x400, "multipage head peels 0x400");
		Check(head.track_begin == 0x571b2e000ull && head.track_end == end, "multipage head keeps the rest");
		const auto tail = ReleaseTrackedTail(end, begin, end);
		Check(tail.address == 0x571b31000ull && tail.size == 0x200, "multipage tail peels 0x200");
		Check(tail.track_begin == begin && tail.track_end == 0x571b31000ull, "multipage tail keeps the rest");
		Check(ReleaseOk(head.address, head.size) && ReleaseOk(tail.address, tail.size),
		      "multipage releases are real ranges");
	}

	// A page-aligned end has no partial tail.
	{
		constexpr uint64_t begin = 0x1000;
		constexpr uint64_t end   = 0x3000;
		const auto         tail  = ReleaseTrackedTail(end, begin, end);
		Check(tail.size == 0 && tail.track_end == end && tail.track_begin == begin, "aligned tail is a no-op");
	}

	return g_fails == 0 ? 0 : 1;
}
