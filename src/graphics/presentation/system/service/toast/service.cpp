#include "graphics/presentation/system/service/toast/service.h"

#include "graphics/presentation/system/animation.h"
#include "graphics/presentation/system/components/toast.h"
#include "graphics/presentation/system/core/scale.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/imgui/imageCache.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Libs::Graphics::SystemUi {

// Everything a service holds. Adds and dismisses go into the inbox, under a lock of its own held
// only briefly, so a thread adding a toast never waits for a frame of toasts to be built. The
// queue is held while the inbox is applied and the toasts drawn. The consumers have a lock of
// their own too, as a consumer adds toasts while they are held.
struct ToastState {
	// An add or a dismiss waiting in the inbox, applied in the order made.
	struct InboxEntry {
		bool         dismiss = false;
		uint64_t     id      = 0;
		ToastRequest toast;
		TimePoint    now {};
	};

	mutable std::mutex                              consumers_mutex;
	std::vector<std::unique_ptr<EventConsumerBase>> consumers;

	std::mutex              inbox_mutex;
	std::vector<InboxEntry> inbox;
	std::atomic<uint64_t>   last_id {0};

	std::mutex queue_mutex;
	ToastQueue queue;
	// The ids of adds that merged into a toast already there by key, each to the id of that
	// toast, so either id dismisses it. Held with the queue.
	std::unordered_map<uint64_t, uint64_t> merged;
	std::vector<InboxEntry>                applying;

	// Only the drawing thread touches these.
	FrameBuilder                frame;
	std::vector<AnimateElement> animates;
	std::vector<AnyElement>     built;
	std::vector<uint64_t>       finished;
	std::vector<uint64_t>       newly_shown;
	std::vector<ToastEntry>     appeared;

	// Whether a toast is on screen or waiting, kept for NeedsFrames, which other threads ask.
	std::atomic<bool> showing {false};
};

namespace {

using InboxEntry = ToastState::InboxEntry;

// The name every toast's animation is keyed under, with the toast's id.
constexpr std::string_view TOAST_KEY = "toast";

// Lets each consumer handle its waiting events at `now`. The queue isn't held, so a consumer can
// add toasts.
void ConsumeEvents(ToastState& state, TimePoint now) {
	std::scoped_lock lock(state.consumers_mutex);
	for (const std::unique_ptr<EventConsumerBase>& consumer: state.consumers) {
		consumer->Consume(now);
	}
}

// Applies `entry` to the queue, noting an add that merged by key so its id still dismisses.
void ApplyEntry(ToastState& state, InboxEntry& entry) {
	if (entry.dismiss) {
		const auto alias = state.merged.find(entry.id);
		state.queue.Dismiss(alias != state.merged.end() ? alias->second : entry.id);
		return;
	}
	const uint64_t kept = state.queue.Add(std::move(entry.toast), entry.now, entry.id);
	if (kept != entry.id) {
		state.merged[entry.id] = kept;
	}
}

// Moves the inbox into the queue in the order it was filled. Needs the queue held. The inbox is
// held only while it is swapped out, so adds made meanwhile wait for the next frame.
void ApplyInbox(ToastState& state) {
	state.applying.clear();
	{
		std::scoped_lock lock(state.inbox_mutex);
		state.applying.swap(state.inbox);
	}
	for (InboxEntry& entry: state.applying) {
		ApplyEntry(state, entry);
	}
	state.applying.clear();
	// Forgets the aliases of toasts that have gone, so the map stays as small as the queue.
	std::erase_if(state.merged,
	              [&state](const auto& alias) { return !state.queue.Contains(alias.second); });
}

// Says whether anything is on screen or waiting, the inbox included. Needs the queue held. The
// inbox is held while the flag is set, so an add made at the same time can't have its flag lost.
void StoreShowing(ToastState& state) {
	std::scoped_lock lock(state.inbox_mutex);
	state.showing.store(!state.queue.Empty() || !state.inbox.empty(), std::memory_order_release);
}

// Notes the toasts that came on screen since the last frame, which Draw returns.
void NoteAppeared(ToastState& state) {
	state.newly_shown.clear();
	state.queue.TakeNewlyShown(state.newly_shown);
	state.appeared.clear();
	for (const ToastEntry& entry: state.queue.OnScreen()) {
		if (std::find(state.newly_shown.begin(), state.newly_shown.end(), entry.id) !=
		    state.newly_shown.end()) {
			state.appeared.push_back(entry);
		}
	}
}

// A toast's sweep, the theme's, waiting the theme's shine delay first, so it crosses a toast that
// has come to rest rather than one still sliding in.
Shine ToastShine(const ThemeContext& theme) {
	Timeline timeline = ThemeShine(theme);
	timeline.delay_ms = theme.motion.shine_delay;
	return {.timeline = timeline};
}

// Builds the stack of toasts in the open frame scope, each from its builder, keeping the handle of
// each toast's animation in the order of the queue.
AnyElement BuildStack(ToastState& state, const ThemeContext& theme) {
	state.animates.clear();
	state.built.clear();
	// Oldest first, so the newest comes in at the bottom and the toasts above it stay put.
	const Slide arrival {.x = ToastDefaults(theme).slide_distance};
	for (const ToastEntry& entry: state.queue.OnScreen()) {
		// Every toast is keyed by its id, so each keeps its own animation. Shown fades it in as it
		// slides in from the right, and its hold ending fades it out in place, with the theme's
		// fade and no hold of its own. One that asks to shine sweeps once it has come on screen,
		// timed from its fade, as the Animate's first build is when the toast is shown.
		const AnimateElement animate =
		    Animate({.fade  = Fade {.shown = entry.shown},
		             .slide = arrival,
		             .shine = entry.toast.options.shine ? std::optional<Shine>(ToastShine(theme))
		                                                : std::nullopt,
		             .key   = Key(TOAST_KEY, entry.id)},
		            entry.toast.build ? entry.toast.build() : BoxElement {});
		state.animates.push_back(animate);
		state.built.push_back(animate);
	}
	return Box({.layout = {.direction = FlexDirection::Column,
	                       .align     = Align::End,
	                       .gap       = theme.space.s3}},
	           state.built);
}

// Forgets the toasts whose fade out has finished, read from the handles BuildStack kept, at `now`.
// Needs the frame scope the stack was built in.
void ForgetFinished(ToastState& state, TimePoint now) {
	state.finished.clear();
	const std::vector<ToastEntry>& on_screen = state.queue.OnScreen();
	for (size_t index = 0; index < on_screen.size() && index < state.animates.size(); ++index) {
		if (!on_screen[index].shown && FadePose(state.animates[index]).phase == Phase::Gone) {
			state.finished.push_back(on_screen[index].id);
		}
	}
	for (const uint64_t id: state.finished) {
		state.queue.Remove(id, now);
	}
}

// Builds, places and paints the stack as `frame` says, in a frame of its own, then forgets the
// toasts that have faded out.
void PaintStack(ToastState& state, const ToastFrame& frame) {
	const ThemeContext& theme = frame.theme;
	const ToastArea&    area  = frame.area;
	state.frame.Begin();
	const ScaleContext scale(theme.scale, area.width, area.height);
	const FrameScope   scope(state.frame, theme, scale, frame.fonts, frame.clock, frame.motion);
	const AnyElement   stack  = BuildStack(state, theme);
	const float        margin = ToPixels(theme.space.s6);
	// A gap below whatever sits above, and never nearer the top than the margin.
	const float top  = std::max(margin, area.top + ToPixels(theme.space.s3));
	const Vec2  size = Measure(stack, {area.width - 2.0f * margin, area.height - top - margin});
	// Against the right edge, under the performance panel.
	if (Arrange(stack, {area.width - margin - size.x, top, size.x, size.y})) {
		Paint(stack, frame.list);
	}
	ForgetFinished(state, frame.clock.time);
}

} // namespace

ToastService::ToastService(): m_state(std::make_unique<ToastState>()) {}

ToastService::~ToastService() = default;

uint64_t ToastService::Add(ToastBuilder build, ToastOptions options, TimePoint now) {
	ToastState&      state = *m_state;
	const uint64_t   id    = state.last_id.fetch_add(1, std::memory_order_relaxed) + 1;
	std::scoped_lock lock(state.inbox_mutex);
	state.inbox.push_back({.id    = id,
	                       .toast = {.build = std::move(build), .options = std::move(options)},
	                       .now   = now});
	// Said as soon as it is added, so NeedsFrames never reads false between a consumer clearing its
	// pending events and the toasts they made being drawn.
	state.showing.store(true, std::memory_order_release);
	return id;
}

void ToastService::Dismiss(uint64_t id) {
	ToastState&      state = *m_state;
	std::scoped_lock lock(state.inbox_mutex);
	state.inbox.push_back({.dismiss = true, .id = id});
}

void ToastService::AddConsumer(std::unique_ptr<EventConsumerBase> consumer) {
	ToastState&      state = *m_state;
	std::scoped_lock lock(state.consumers_mutex);
	state.consumers.push_back(std::move(consumer));
}

bool ToastService::NeedsFrames() const {
	const ToastState& state = *m_state;
	if (state.showing.load(std::memory_order_acquire)) {
		return true;
	}
	std::scoped_lock lock(state.consumers_mutex);
	// Asked again once the consumers are held, as a consumer may have turned its events into a
	// toast between the first look and taking the lock, leaving neither events nor a false flag.
	return std::any_of(state.consumers.begin(), state.consumers.end(),
	                   [](const std::unique_ptr<EventConsumerBase>& consumer) {
		                   return consumer->HasPending();
	                   }) ||
	       state.showing.load(std::memory_order_acquire);
}

const std::vector<ToastEntry>& ToastService::Draw(const ToastFrame& frame) {
	ToastState& state = *m_state;
	// Open for the whole frame, so the consumers and the builders see the frame's cache, or none,
	// and never one the caller left open. It is always engaged, as the optional only picks the
	// constructor.
	std::optional<ImageCacheScope> images;
	if (frame.images != nullptr) {
		images.emplace(*frame.images);
	} else {
		images.emplace(nullptr);
	}
	ConsumeEvents(state, frame.clock.time);
	std::scoped_lock lock(state.queue_mutex);
	ApplyInbox(state);
	state.queue.Update(frame.clock.time);
	NoteAppeared(state);
	if (!state.queue.OnScreen().empty()) {
		PaintStack(state, frame);
	}
	StoreShowing(state);
	return state.appeared;
}

std::vector<ToastEntry> ToastService::OnScreen() {
	ToastState&      state = *m_state;
	std::scoped_lock lock(state.queue_mutex);
	ApplyInbox(state);
	return state.queue.OnScreen();
}

size_t ToastService::Waiting() {
	ToastState&      state = *m_state;
	std::scoped_lock lock(state.queue_mutex);
	ApplyInbox(state);
	return state.queue.Waiting();
}

} // namespace Libs::Graphics::SystemUi
