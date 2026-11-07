#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_QUEUECOMMITS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_QUEUECOMMITS_H_

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// The command buffers committed to one queue, by every submitter.
//
// MoltenVK keeps every live resource in one residency set attached to the queue, so each command
// buffer committed while a resource is alive holds it until that command buffer completes,
// whether its commands use the resource or not. Destroying the resource earlier makes Metal
// report an invalid resource and lose the device. A resource may therefore be destroyed only when
// every command buffer committed to the queue so far has completed: those of each command
// scheduler, which signal their own timeline, and those of a present, which signal none.
//
// The queue runs its command buffers in commit order. So a present is known to be complete once
// a timeline submission committed after it has completed.
//
// Every call is made under the queue's lock, so that nothing is committed between a check and
// what the caller does with its result.
class QueueCommits final {
public:
	// Whether the timeline has completed `tick`.
	using IsRetired = std::function<bool(uint64_t tick)>;

	// A submitter with its own timeline. Returns its id.
	uint32_t AddTimeline(IsRetired is_retired) {
		m_timelines.push_back({std::move(is_retired), 0, true});
		return static_cast<uint32_t>(m_timelines.size() - 1);
	}
	// The submitter is gone: everything it committed has completed.
	void RemoveTimeline(uint32_t timeline) { m_timelines[timeline].active = false; }

	// The timeline committed the command buffer that signals `tick`.
	void NoteSubmit(uint32_t timeline, uint64_t tick) {
		m_timelines[timeline].last_tick = tick;
		m_present_pending               = false;
	}
	// A command buffer that signals no timeline was committed (a present).
	void NotePresent() { m_present_pending = true; }
	// The queue was idle (vkQueueWaitIdle): every command buffer committed so far has completed.
	void NoteIdle() { m_present_pending = false; }

	// Every command buffer committed so far has completed.
	[[nodiscard]] bool AllDone() const {
		if (m_present_pending) {
			return false;
		}
		for (const auto& timeline: m_timelines) {
			if (timeline.active && timeline.last_tick != 0 &&
			    !timeline.is_retired(timeline.last_tick)) {
				return false;
			}
		}
		return true;
	}

private:
	struct Timeline {
		IsRetired is_retired;
		uint64_t  last_tick = 0;
		bool      active    = true;
	};

	std::vector<Timeline> m_timelines;
	// A present was committed after the last timeline submission.
	bool m_present_pending = false;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_QUEUECOMMITS_H_ */
