#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_NOTIFICATION_CONSUMER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_NOTIFICATION_CONSUMER_H_

#include "graphics/presentation/system/events/consumer.h"
#include "graphics/presentation/system/events/notification.h"

#include <cstddef>

namespace Libs::Graphics::SystemUi {

/// How many notices wait between frames, more than the emulator sends at once.
inline constexpr size_t NOTIFICATION_EVENT_QUEUE = 16;

class ToastService;

/// Turns each notification into a notice on the toast service it was given, saying its text with
/// the band of its status, held for the default hold. It subscribes when made, so notices
/// published before then are not seen.
class NotificationEventConsumer final: public EventConsumer<NotificationEvent> {
public:
	/// A consumer of notifications adding to `toasts`, holding up to `capacity` between frames.
	/// The service must outlive it, as it does when the consumer is one of its own.
	explicit NotificationEventConsumer(ToastService& toasts,
	                                   size_t        capacity = NOTIFICATION_EVENT_QUEUE);

protected:
	/// Adds the notice for `event` at `now`.
	void OnEvent(NotificationEvent& event, TimePoint now) override;

private:
	ToastService& m_toasts;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_NOTIFICATION_CONSUMER_H_
