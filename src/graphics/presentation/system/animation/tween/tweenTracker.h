#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TWEEN_TWEEN_TRACKER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TWEEN_TWEEN_TRACKER_H_

#include "graphics/presentation/system/animation/identity.h"
#include "graphics/presentation/system/animation/records.h"
#include "graphics/presentation/system/animation/tween/tween.h"
#include "graphics/presentation/system/animation/tween/tweenValue.h"

#include <cstdint>

// The tweens' values from frame to frame. The tree is built again every frame, so whoever builds
// tweens keeps one tracker for as long as the UI lives, and it keeps a value for each tween.
namespace Libs::Graphics::SystemUi {

/// Where a tween was when it was tracked.
struct TweenPose {
	float value = 1.0f;
	// Whether it will change without new input, so the UI must keep drawing frames.
	bool moving = false;
};

/// Remembers every tween built recently, found by its identity, so each carries on from frame to
/// frame. It knows nothing of elements, so the frame number is given to it.
class TweenTracker {
public:
	/// The pose at `now` of the tween with `identity`, which plays `tween` on `tracks` in `frame`.
	/// A tween tracked for the first time rests at its target, and a change of target moves it
	/// from wherever it is. A tween not tracked in a frame is forgotten, so it rests at its target
	/// again when it is next tracked. `frame` counts up by one each frame and skips zero when it
	/// wraps, as a frame builder's begin count does. Tracking the same tween twice in a frame is a
	/// mistake and asserts.
	[[nodiscard]] TweenPose PoseAt(AnimationIdentity identity, const Tween& tween,
	                               const TweenTracks& tracks, TimePoint now, uint32_t frame);

private:
	AnimationRecords<TweenValue> m_records;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_TWEEN_TWEEN_TRACKER_H_
