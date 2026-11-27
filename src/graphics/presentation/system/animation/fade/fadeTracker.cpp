#include "graphics/presentation/system/animation/fade/fadeTracker.h"

namespace Libs::Graphics::SystemUi {

// Hiding dismisses the fade where it stands. Showing it again starts the entrance over from however
// opaque it is now, skipping the delay, so it neither jumps nor waits.
void FadeTracker::Update(Record& record, const Timeline& timeline, bool shown, TimePoint now) {
	if (shown == record.asked_shown) {
		return;
	}
	record.asked_shown = shown;
	if (!shown) {
		// Showing always clears a dismissal, so there is never an earlier one to keep.
		record.dismissed = now;
		return;
	}
	const float alpha = Evaluate(timeline, record.start, record.dismissed, now).alpha;
	record.start      = EntranceStartFor(timeline, alpha, now);
	record.dismissed.reset();
}

Pose FadeTracker::PoseAt(AnimationIdentity identity, const Timeline& timeline, bool shown,
                         TimePoint now, uint32_t frame) {
	Record* record = m_records.Visit(identity, frame);
	if (record == nullptr) {
		const std::optional<TimePoint> dismissed = shown ? std::nullopt : std::optional(now);
		record = &m_records.Add(identity, frame, {now, dismissed, shown});
	} else {
		Update(*record, timeline, shown, now);
	}
	return Evaluate(timeline, record->start, record->dismissed, now);
}

} // namespace Libs::Graphics::SystemUi
