#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_TROPHY_UNLOCKED_CONSUMER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_TROPHY_UNLOCKED_CONSUMER_H_

#include "graphics/presentation/system/events/consumer.h"
#include "graphics/presentation/system/events/trophy.h"

#include <cstddef>
#include <string>

namespace Libs::Graphics::SystemUi {

/// How many unlocks wait between frames, more than a game unlocks at once. Once taken, trophies
/// share the toast queue's limit with every other toast, so a burst past it drops the oldest
/// waiting, trophies included, unlike the old trophy toast, which kept every one.
inline constexpr size_t TROPHY_EVENT_QUEUE = 12;

class ToastService;

/// Turns each trophy unlock into a trophy toast on the toast service it was given, named for the
/// trophy, with `subtitle` under the name, its icon and grade, held for `hold_ms`, greeted with the
/// unlock sound. The subtitle and hold are given, so the consumer reads no settings and no
/// localisation, and is plain logic. It subscribes when made, so unlocks published before then are
/// not seen.
class TrophyEventConsumer final: public EventConsumer<TrophyEvent> {
public:
	/// A consumer of trophy unlocks adding to `toasts`, holding up to `capacity` between frames.
	/// The service must outlive it, as it does when the consumer is one of its own.
	TrophyEventConsumer(ToastService& toasts, std::string subtitle, float hold_ms,
	                    size_t capacity = TROPHY_EVENT_QUEUE);

protected:
	/// Adds the trophy toast for `event` at `now`.
	void OnEvent(TrophyEvent& event, TimePoint now) override;

private:
	ToastService& m_toasts;
	std::string   m_subtitle;
	float         m_hold_ms;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_TROPHY_UNLOCKED_CONSUMER_H_
