#include "graphics/presentation/system/views/trophyUnlocked/consumer.h"

#include "graphics/presentation/system/service.h"
#include "graphics/presentation/system/views/trophyUnlocked/trophyUnlocked.h"

#include <utility>

namespace Libs::Graphics::SystemUi {

TrophyEventConsumer::TrophyEventConsumer(ToastService& toasts, std::string subtitle, float hold_ms,
                                         size_t capacity)
    : EventConsumer<TrophyEvent>(capacity), m_toasts(toasts), m_subtitle(std::move(subtitle)),
      m_hold_ms(hold_ms) {}

void TrophyEventConsumer::OnEvent(TrophyEvent& event, TimePoint now) {
	// The props are held by the builder, so the toast keeps them for every frame it is drawn. It
	// has no key, as two unlocks are never one.
	m_toasts.Add(
	    [props = TrophyUnlockedProps {.name     = std::move(event.name),
	                                  .subtitle = m_subtitle,
	                                  .grade    = GradeOf(event.grade),
	                                  .icon     = std::move(event.icon_png)}] {
		    return TrophyUnlocked(props);
	    },
	    {.hold_ms = m_hold_ms, .sound = ToastSound::Unlock, .shine = true}, now);
}

} // namespace Libs::Graphics::SystemUi
