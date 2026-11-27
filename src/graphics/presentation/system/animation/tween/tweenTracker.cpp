#include "graphics/presentation/system/animation/tween/tweenTracker.h"

namespace Libs::Graphics::SystemUi {

TweenPose TweenTracker::PoseAt(AnimationIdentity identity, const Tween& tween,
                               const TweenTracks& tracks, TimePoint now, uint32_t frame) {
	TweenValue* value = m_records.Visit(identity, frame);
	if (value == nullptr) {
		// Resting at its target, so what starts part way shows part way from its first frame and
		// entrances stay a fade's job.
		value = &m_records.Add(identity, frame, TweenValue(tween.target));
	} else {
		value->Retarget(tween.target, tracks, now, tween.whole_track);
	}
	return {value->Value(now), value->Moving(now)};
}

} // namespace Libs::Graphics::SystemUi
