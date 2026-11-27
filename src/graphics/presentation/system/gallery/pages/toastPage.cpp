#include "graphics/presentation/system/gallery/pages/toastPage.h"

#include "graphics/presentation/system/components/toast.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/events.h"
#include "graphics/presentation/system/events/consumer.h"
#include "graphics/presentation/system/gallery/components/page.h"
#include "graphics/presentation/system/gallery/pages/parts/format.h"
#include "graphics/presentation/system/imgui/imageCache.h"
#include "graphics/presentation/system/service.h"
#include "graphics/presentation/system/views/notification/notification.h"
#include "graphics/presentation/system/views/trophyUnlocked/trophyUnlocked.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "Use a toast for a passing event message that needs no answer. It never takes focus, one shows "
    "at a time, and others wait their turn. Say what happened in words, as colour alone tells the "
    "types apart.";

// How often the live demo publishes a test event. A little faster than three toasts clear, so the
// queue behind the three on screen can be seen filling and draining.
constexpr std::chrono::milliseconds DEMO_PERIOD {1500};

// How many test events the demo's subscription holds between frames.
constexpr size_t DEMO_QUEUE = 16;

// The statuses in the order the demo cycles through them.
constexpr std::array<Status, 4> DEMO_STATUSES {Status::Info, Status::Success, Status::Warning,
                                               Status::Error};

// How often the demo publishes a trophy instead, and how many grades it cycles through.
constexpr uint32_t DEMO_TROPHY_EVERY = 4;
constexpr uint32_t DEMO_GRADES       = 4;

// Room for a demo toast's text.
constexpr size_t DEMO_TEXT_SIZE = 64;

// A grey between the bright and dark frames, as many game frames are.
constexpr Color MID_FRAME = Palette::GRAY_500;

// The demo's test event, carrying data rather than text, as real events do.
struct DemoToastEvent {
	Status   status = Status::Info;
	uint32_t number = 0;
};

// The name of `status`, for the demo's text.
std::string_view StatusName(Status status) {
	switch (status) {
		case Status::Success: return "Success";
		case Status::Warning: return "Warning";
		case Status::Error: return "Error";
		case Status::Info: break;
	}
	return "Info";
}

// Turns the demo's test events into toasts, as a real consumer turns its events.
class DemoConsumer final: public EventConsumer<DemoToastEvent> {
public:
	explicit DemoConsumer(ToastService& toasts)
	    : EventConsumer<DemoToastEvent>(DEMO_QUEUE), m_toasts(toasts) {}

protected:
	void OnEvent(DemoToastEvent& event, TimePoint now) override {
		std::array<char, DEMO_TEXT_SIZE> text {};
		std::snprintf(text.data(), text.size(), "%.*s toast %u",
		              static_cast<int>(StatusName(event.status).size()),
		              StatusName(event.status).data(), event.number);
		m_toasts.Add(
		    [props = NotificationProps {.status = event.status, .text = text.data()}] {
			    return Notification(props);
		    },
		    {}, now);
	}

private:
	ToastService& m_toasts;
};

// One notice with `status` saying `text`, built as in game.
AnyElement Specimen(Status status, std::string_view text) {
	return Notification({.status = status, .text = std::string(text)});
}

// A trophy of `grade` named `name`, with `icon` or none, built as in game.
AnyElement Trophy(TrophyGrade grade, std::string_view name, const EncodedImage& icon) {
	return TrophyUnlocked({.name     = std::string(name),
	                       .subtitle = std::string(DEMO_TROPHY_SUBTITLE),
	                       .grade    = grade,
	                       .icon     = icon});
}

// A row of `items` with the theme's gap, its children aligned to their tops.
AnyElement Row(const ThemeContext& theme, std::span<const AnyElement> items) {
	return Box({.layout = {.gap = theme.space.s4}}, items);
}

// A frame of `fill` holding `items` in a column, standing in for a game frame behind the toasts.
// Its padding is more than the shadow reaches, so the shadow stays on the frame.
AnyElement Frame(const ThemeContext& theme, Color fill, std::span<const AnyElement> items) {
	return Box({.layout = {.direction = FlexDirection::Column,
	                       .align     = Align::End,
	                       .gap       = theme.space.s3,
	                       .padding   = DpInsets(theme.space.s6)},
	            .self   = {.styles = {.fill = fill, .radius = theme.radius.md}}},
	           items);
}

// The specimens that compare the statuses, the text lengths and the narrowest toast.
std::array<AnyElement, 4> TypeAndTextRows(const ThemeContext& theme) {
	const std::array<AnyElement, 4> types {
	    Specimen(Status::Info, "Controller 1 connected"),
	    Specimen(Status::Success, "Game saved"),
	    Specimen(Status::Warning, "Shader cache is full"),
	    Specimen(Status::Error, "Save failed"),
	};
	const std::array<AnyElement, 3> lengths {
	    Specimen(Status::Info, "Hi"),
	    Specimen(Status::Info,
	             "The controller was disconnected. Reconnect it, or press the PS button on "
	             "another controller to continue playing."),
	    Specimen(Status::Warning,
	             "This line is long enough to need more than the three lines a notice allows, so "
	             "the last line shown ends with an ellipsis rather than the toast growing "
	             "taller and taller to cover more of the game behind it."),
	};
	return {SectionTitle(theme, "Notifications of each type"), Row(theme, types),
	        SectionTitle(theme, "Narrowest, wrapped and cut short"), Row(theme, lengths)};
}

// The trophy specimens, each grade with the icon, then a long name and one with no icon.
std::array<AnyElement, 3> TrophyRows(const ThemeContext& theme, const EncodedImage& icon) {
	const std::array<AnyElement, 4> grades {
	    Trophy(TrophyGrade::Platinum, "Master of All", icon),
	    Trophy(TrophyGrade::Gold, "Champion", icon),
	    Trophy(TrophyGrade::Silver, "Runner Up", icon),
	    Trophy(TrophyGrade::Bronze, "First Steps", icon),
	};
	const std::array<AnyElement, 2> edges {
	    Trophy(TrophyGrade::Gold,
	           "A trophy whose name is long enough to need more than the two lines a trophy toast "
	           "allows, so it is cut short",
	           icon),
	    Trophy(TrophyGrade::Gold, "No icon from the game", {}),
	};
	return {SectionTitle(theme, "Trophies"), Row(theme, grades), Row(theme, edges)};
}

// The stack and the frames behind it, bright and mid grey.
std::array<AnyElement, 2> StackAndFrameRows(const ThemeContext& theme, const EncodedImage& icon) {
	const std::array<AnyElement, 3> stack {
	    Trophy(TrophyGrade::Gold, "Champion", icon),
	    Specimen(Status::Info, "Controller 2 connected"),
	    Specimen(Status::Error, "Save failed"),
	};
	const std::array<AnyElement, 2> bright {
	    Specimen(Status::Info, "Controller 1 connected"),
	    Specimen(Status::Error, "Save failed"),
	};
	const std::array<AnyElement, 2> mid {
	    Specimen(Status::Success, "Game saved"),
	    Specimen(Status::Warning, "Shader cache is full"),
	};
	// The dark frame holds the stack, the bright and mid frames a pair each, side by side. Each
	// frame takes a palette colour, as it stands for a game frame and not a surface of the UI.
	const std::array<AnyElement, 3> frames {
	    Frame(theme, Palette::GRAY_0, stack),
	    Frame(theme, Palette::GRAY_900, bright),
	    Frame(theme, MID_FRAME, mid),
	};
	return {SectionTitle(theme,
	                     "A trophy and two notices stacked, then on a bright and a mid grey frame"),
	        Row(theme, frames)};
}

// A toast status, or none for a plain toast, and what a toast with it might say.
struct ToastSample {
	// None for a plain toast.
	std::optional<Status> status;
	std::string_view      text;
};

constexpr std::array<ToastSample, 5> TOAST_SAMPLES {
    ToastSample {std::nullopt, "Plain, with no band"},
    ToastSample {Status::Info, "Info, Controller 1 connected"},
    ToastSample {Status::Success, "Success, Game saved"},
    ToastSample {Status::Warning, "Warning, Shader cache is full"},
    ToastSample {Status::Error, "Error, Save failed"}};

// A toast with `sample`'s status holding its text. A plain toast adds no padding, as what it holds
// brings its own, so its text is padded here with the toast's own padding, as real content is.
AnyElement ToastOf(const ThemeContext& theme, const ToastTokens& toast, const ToastSample& sample) {
	const AnyElement text = Text(
	    {.text = sample.text,
	     .self = {.styles = {.type = theme.type.body_small, .color = theme.colors.text.primary}}});
	if (sample.status) {
		return Toast({.status = sample.status}, std::array<AnyElement, 1> {text});
	}
	const std::array<AnyElement, 1> padded {
	    Box({.layout = {.padding = DpInsets(toast.padding_vertical, toast.padding_horizontal)}},
	        std::array<AnyElement, 1> {text}),
	};
	return Toast({}, padded);
}

// The bare toast component with each status, and plain, holding plain text, beside its layout
// tokens. Every notice below is built on it.
AnyElement ToastComponent(const ThemeContext& theme) {
	const ToastTokens       toast = ToastDefaults(theme);
	std::vector<AnyElement> toasts;
	for (const ToastSample& sample: TOAST_SAMPLES) {
		toasts.push_back(ToastOf(theme, toast, sample));
	}
	const std::array<AnyElement, 5> tokens {
	    TokenLine(theme, "min_width to max_width",
	              LengthText(toast.min_width.value) + " to " + LengthText(toast.max_width.value)),
	    TokenLine(theme, "padding_horizontal", LengthText(toast.padding_horizontal.value)),
	    TokenLine(theme, "padding_vertical", LengthText(toast.padding_vertical.value)),
	    TokenLine(theme, "edge_width, the status band", LengthText(toast.edge_width.value)),
	    TokenLine(theme, "slide_distance", LengthText(toast.slide_distance.value)),
	};
	return BesideTokens(
	    theme, Box({.layout = {.direction = FlexDirection::Column, .gap = theme.space.s3}}, toasts),
	    Lines(theme, tokens));
}

} // namespace

AnyElement BuildToastPage(const ToastPageProps& props) {
	const EncodedImage&             icon     = props.icon;
	const ThemeContext&             theme    = CurrentThemeContext();
	const std::array<AnyElement, 4> types    = TypeAndTextRows(theme);
	const std::array<AnyElement, 3> trophies = TrophyRows(theme, icon);
	const std::array<AnyElement, 2> stacks   = StackAndFrameRows(theme, icon);
	return Page({.title = "Toast", .subtitle = RULE},
	            {
	                Section(theme, "The toast component, in each type", ToastComponent(theme)),
	                types,
	                trophies,
	                stacks,
	            });
}

ToastDemo::ToastDemo(ToastService& toasts, EncodedImage icon_png)
    : m_icon_png(std::move(icon_png)), m_toasts(toasts) {
	m_toasts.AddConsumer(std::make_unique<DemoConsumer>(m_toasts));
}

void ToastDemo::Draw(ImDrawList& list, const ToastArea& area, const ThemeContext& theme,
                     const FontsContext& fonts, ClockContext clock, MotionContext motion) {
	if (clock.time >= m_next) {
		++m_published;
		// Every fourth a trophy, through the same event the emulator publishes, cycling the grades.
		if (m_published % DEMO_TROPHY_EVERY == 0) {
			const int32_t grade =
			    static_cast<int32_t>(m_published / DEMO_TROPHY_EVERY % DEMO_GRADES) + 1;
			Publish(TrophyEvent {.name = "Demo trophy", .grade = grade, .icon_png = m_icon_png});
		} else {
			// A count of its own, so every status comes round, Info too, between the trophies.
			const Status status = DEMO_STATUSES[m_notices++ % DEMO_STATUSES.size()];
			Publish(DemoToastEvent {.status = status, .number = m_published});
		}
		m_next = clock.time + DEMO_PERIOD;
	}
	// The toasts decode their icons in the cache the caller has open, handed over explicitly, as
	// the service never relies on a scope its caller opened.
	m_toasts.Draw({.list   = list,
	               .area   = area,
	               .theme  = theme,
	               .fonts  = fonts,
	               .images = CurrentImageCache(),
	               .clock  = clock,
	               .motion = motion});
}

} // namespace Libs::Graphics::SystemUi::Gallery
