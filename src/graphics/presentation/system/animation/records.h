#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_RECORDS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_RECORDS_H_

#include "graphics/presentation/system/animation/identity.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace Libs::Graphics::SystemUi {

/// The frame number before `frame`, in a count that skips zero when it wraps, as a frame builder's
/// begin count does, so the frame before the first is the largest.
[[nodiscard]] constexpr uint32_t PreviousFrame(uint32_t frame) {
	return frame == 1 ? std::numeric_limits<uint32_t>::max() : frame - 1;
}

/// What a tracker remembers of each animation, a `State` found by the animation's identity. The
/// tree is built again every frame, so an animation not visited in a frame is forgotten and starts
/// again when it comes back. The frame number counts up by one each frame and skips zero when it
/// wraps, as a frame builder's begin count does. It knows nothing of elements, so the frame number
/// is given to it.
template <class State>
class AnimationRecords {
public:
	/// The state of the animation with `identity`, visited in `frame`, or null when there is none,
	/// in which case the caller adds one. Visiting the same animation twice in a frame is a mistake
	/// and asserts. The pointer is good until the next Visit or Add.
	[[nodiscard]] State* Visit(AnimationIdentity identity, uint32_t frame) {
		Sweep(frame);
		for (Entry& entry: m_entries) {
			if (entry.identity == identity) {
				assert(entry.visited != frame &&
				       "the same animation was visited twice in one frame");
				entry.visited = frame;
				return &entry.state;
			}
		}
		return nullptr;
	}

	/// Remembers `state` for the animation with `identity`, visited in `frame`, after a Visit found
	/// none. The reference is good until the next Visit or Add.
	State& Add(AnimationIdentity identity, uint32_t frame, State state) {
		m_entries.push_back({identity, frame, std::move(state)});
		return m_entries.back().state;
	}

	/// The state of the animation with `identity` without visiting it, or null when there is none,
	/// for reading or updating it later in a frame that already visited it. The pointer is good
	/// until the next Visit or Add.
	[[nodiscard]] State* Find(AnimationIdentity identity) {
		for (Entry& entry: m_entries) {
			if (entry.identity == identity) {
				return &entry.state;
			}
		}
		return nullptr;
	}

	/// Calls `visit` with the identity and state of every animation remembered, as of the last
	/// sweep, so those from the frame before can be read before this frame visits them.
	template <class Visitor>
	void ForEach(Visitor&& visit) const {
		for (const Entry& entry: m_entries) {
			visit(entry.identity, entry.state);
		}
	}

private:
	/// One animation's record, found by its identity, and the frame it was last used in.
	struct Entry {
		AnimationIdentity identity;
		// The frame it was last visited in.
		uint32_t visited = 0;
		State    state;
	};

	/// Forgets the animations visited in neither `frame` nor the one before, once a frame. The
	/// frame before the first one is the largest, as the count skips zero. A record left untouched
	/// for a whole cycle of the count, about two years of frames, could be taken for a current one.
	void Sweep(uint32_t frame) {
		if (m_swept == frame) {
			return;
		}
		m_swept                 = frame;
		const uint32_t previous = PreviousFrame(frame);
		std::erase_if(m_entries, [frame, previous](const Entry& entry) {
			return entry.visited != frame && entry.visited != previous;
		});
	}

	// A vector, as there are only ever a few, so a steady frame finds them without allocating.
	std::vector<Entry> m_entries;
	// The frame the entries were last swept in. Zero is never a frame, so the first visit sweeps.
	uint32_t m_swept = 0;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_RECORDS_H_
