#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_FADE_FADE_TRACKER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_FADE_FADE_TRACKER_H_

#include "graphics/presentation/system/animation/identity.h"
#include "graphics/presentation/system/animation/records.h"
#include "graphics/presentation/system/animation/timeline.h"
#include "graphics/presentation/system/core/clock.h"

#include <cstdint>
#include <optional>

// The fades' progress from frame to frame. The tree is built again every frame, so whoever builds
// fades keeps one tracker for as long as the UI lives, and it keeps a small record for each fade.
namespace Libs::Graphics::SystemUi {

/// Remembers every fade built recently, found by its identity, so each carries on from frame to
/// frame. It knows nothing of elements, so the frame number is given to it.
class FadeTracker {
public:
	/// The pose at `now` of the fade with `identity`, which plays `timeline` in `frame`. A fade
	/// tracked for the first time starts now. Asking for it hidden dismisses it, so it fades out
	/// from where it is, and asking for it shown again starts its entrance again from its current
	/// opacity without the delay. A fade that left on its own after its hold stays gone while it is
	/// asked for. A fade not tracked in a frame is forgotten, so it starts again when it is next
	/// tracked. `frame` counts up by one each frame and skips zero when it wraps, as a frame
	/// builder's begin count does. Tracking the same fade twice in a frame is a mistake and
	/// asserts.
	[[nodiscard]] Pose PoseAt(AnimationIdentity identity, const Timeline& timeline, bool shown,
	                          TimePoint now, uint32_t frame);

private:
	/// What the tracker remembers of one fade.
	struct Record {
		TimePoint                start;
		std::optional<TimePoint> dismissed;
		// Whether it was last asked for shown, so a change of mind can be told from asking again.
		bool asked_shown = true;
	};

	/// Takes in a change in whether the fade of `record` is asked for shown.
	static void Update(Record& record, const Timeline& timeline, bool shown, TimePoint now);

	AnimationRecords<Record> m_records;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_FADE_FADE_TRACKER_H_
