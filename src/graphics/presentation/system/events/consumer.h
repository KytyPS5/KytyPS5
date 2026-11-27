#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_CONSUMER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_CONSUMER_H_

#include "graphics/presentation/system/core/clock.h"
#include "graphics/presentation/system/events/subscribe.h"

#include <cstddef>
#include <vector>

namespace Libs::Graphics::SystemUi {

/// Something that takes waiting events once a frame, whatever their type, so one list can hold the
/// consumers of every event type. Each consumer decides what its events do, such as adding a toast,
/// so publishers send data and never text.
class EventConsumerBase {
public:
	EventConsumerBase()                                    = default;
	EventConsumerBase(const EventConsumerBase&)            = delete;
	EventConsumerBase& operator=(const EventConsumerBase&) = delete;
	EventConsumerBase(EventConsumerBase&&)                 = delete;
	EventConsumerBase& operator=(EventConsumerBase&&)      = delete;
	virtual ~EventConsumerBase()                           = default;

	/// Whether events are waiting, cheap enough to ask every frame from any thread.
	[[nodiscard]] virtual bool HasPending() const = 0;

	/// Takes the waiting events and handles each, at `now`. Called on the thread that draws.
	virtual void Consume(TimePoint now) = 0;
};

/// A consumer of the events of type `TEnvelope`. It subscribes when made, so events published
/// before then are not seen, drains its own subscription each frame and hands each event to
/// OnEvent in the order published.
template <class TEnvelope>
class EventConsumer: public EventConsumerBase {
public:
	/// Subscribes to `TEnvelope`, holding up to `capacity` events between frames.
	explicit EventConsumer(size_t capacity): m_subscription(Subscribe<TEnvelope>(capacity)) {}

	/// Whether events are waiting, cheap enough to ask every frame from any thread.
	[[nodiscard]] bool HasPending() const override { return m_subscription.HasPending(); }

	/// Takes the waiting events and hands each to OnEvent, at `now`.
	void Consume(TimePoint now) override {
		m_events.clear();
		m_subscription.DrainInto(m_events);
		for (TEnvelope& event: m_events) {
			OnEvent(event, now);
		}
		// Cleared once handled too, so an event's data isn't kept alive until the next frame,
		// which may not come once nothing is showing.
		m_events.clear();
	}

protected:
	/// Handles one event at `now`. It may move from `event`, which is not used again.
	virtual void OnEvent(TEnvelope& event, TimePoint now) = 0;

private:
	EventSubscription<TEnvelope> m_subscription;
	// Kept between frames, so draining doesn't allocate once it has grown.
	std::vector<TEnvelope> m_events;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_CONSUMER_H_
