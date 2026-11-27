// Checks the system UI's toast queue: holds that end on their own, merging
// repeats by key, dismissal, the cap on toasts shown at once and the queue
// behind it, which toasts are newly shown, a toast's options, the game's trophy
// grades, the trophy and notification consumers, a notice's status glyph, the
// service's inbox, with fixed clock times, and the system UI instance, its
// consumers, the trophy options reaching its toasts, and the fonts and image
// cache it makes on first use.
#include "graphics/presentation/system/components/toast.h"
#include "graphics/presentation/system/core/motion.h"
#include "graphics/presentation/system/core/scale.h"
#include "graphics/presentation/system/core/theme.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/frame/frameScope.h"
#include "graphics/presentation/system/element/primitives/box/box.h"
#include "graphics/presentation/system/element/primitives/text/text.h"
#include "graphics/presentation/system/events/notification.h"
#include "graphics/presentation/system/events/publish.h"
#include "graphics/presentation/system/events/trophy.h"
#include "graphics/presentation/system/imgui/imageCache.h"
#include "graphics/presentation/system/instance.h"
#include "graphics/presentation/system/service/toast/queue.h"
#include "graphics/presentation/system/service/toast/service.h"
#include "graphics/presentation/system/views/notification/consumer.h"
#include "graphics/presentation/system/views/notification/notification.h"
#include "graphics/presentation/system/views/trophyUnlocked/consumer.h"
#include "graphics/presentation/system/views/trophyUnlocked/trophyUnlocked.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Libs::Graphics::SystemUi::ClockContext;
using Libs::Graphics::SystemUi::MAX_WAITING_TOASTS;
using Libs::Graphics::SystemUi::TimePoint;
using Libs::Graphics::SystemUi::ToastEntry;
using Libs::Graphics::SystemUi::ToastQueue;
using Libs::Graphics::SystemUi::ToastRequest;
using Libs::Graphics::SystemUi::ToastSound;

namespace {

const TimePoint BASE = TimePoint{} + std::chrono::hours(1000);

TimePoint At(float ms) {
  return BASE + std::chrono::duration_cast<ClockContext::Clock::duration>(
                    std::chrono::duration<float, std::milli>(ms));
}

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "SystemUiToastsTests: failed: %s\n", message);
    std::abort();
  }
}

// A builder that carries a label, so a test can tell toasts apart by reading it
// back from the queue. It builds nothing, as the queue never calls it.
struct Labelled {
  std::string text;
  Libs::Graphics::SystemUi::Element<Libs::Graphics::SystemUi::BoxProps>
  operator()() const {
    return {};
  }
};

// The label of the toast in `entry`, or empty when it has none.
std::string TextOf(const ToastEntry &entry) {
  const auto *labelled = entry.toast.build.target<Labelled>();
  return labelled != nullptr ? labelled->text : std::string{};
}

ToastRequest Held(std::string text, float hold_ms, std::string key = {}) {
  return {.build = Labelled{std::move(text)},
          .options = {.key = std::move(key), .hold_ms = hold_ms}};
}

// A toast is shown from when it is added until its hold ends, then leaves but
// stays listed until removed, as its fade out is played where it is drawn.
void TestHold() {
  ToastQueue toasts;
  const uint64_t id = toasts.Add(Held("hello", 100.0f), At(0));
  Check(toasts.OnScreen().size() == 1 && toasts.OnScreen()[0].shown,
        "a new toast is shown");
  toasts.Update(At(99));
  Check(toasts.OnScreen()[0].shown, "and stays shown during its hold");
  toasts.Update(At(100));
  Check(!toasts.OnScreen()[0].shown, "and leaves once its hold ends");
  Check(!toasts.Empty(), "but stays listed while it fades out");
  toasts.Remove(id, At(200));
  Check(toasts.Empty(), "until it is removed");
}

// An infinite hold stays until dismissed, and so does one too long to add to a
// time point, which would otherwise overflow it into the past. A hold that
// isn't a number doesn't stay. Its guard only removes undefined behaviour, as
// x64 happens to give the same answer without it.
void TestOddHolds() {
  ToastQueue toasts;
  const uint64_t forever =
      toasts.Add(Held("stays", std::numeric_limits<float>::infinity()), At(0));
  toasts.Add(Held("broken", std::numeric_limits<float>::quiet_NaN()), At(0));
  toasts.Add(Held("huge", 1e20f), At(0));
  toasts.Update(At(1e9f));
  Check(toasts.OnScreen()[0].shown, "an infinite hold stays");
  Check(!toasts.OnScreen()[1].shown, "a hold that isn't a number doesn't stay");
  Check(toasts.OnScreen()[2].shown, "a hold too long to add stays");
  Check(toasts.Dismiss(forever) && !toasts.OnScreen()[0].shown,
        "dismissing starts it leaving");
  Check(!toasts.Dismiss(999), "dismissing an unknown id says so");
}

// A repeat with the same key refreshes the toast on screen, with its new text
// and a fresh hold, rather than stacking another. Without the merge the list
// would hold two.
void TestMergeByKey() {
  ToastQueue toasts;
  const uint64_t first =
      toasts.Add(Held("Controller 1 connected", 100.0f, "pad.1"), At(0));
  const uint64_t again =
      toasts.Add(Held("Controller 1 reconnected", 100.0f, "pad.1"), At(80));
  Check(first == again && toasts.OnScreen().size() == 1,
        "a repeat with the same key merges");
  Check(TextOf(toasts.OnScreen()[0]) == "Controller 1 reconnected",
        "and takes the new text");
  toasts.Update(At(150));
  Check(toasts.OnScreen()[0].shown, "and its hold starts again");
  toasts.Update(At(180));
  Check(!toasts.OnScreen()[0].shown, "ending a hold after the repeat");
  toasts.Add(Held("Controller 1 connected", 100.0f, "pad.1"), At(190));
  Check(toasts.OnScreen()[0].shown,
        "a repeat brings back a toast that was leaving");
  toasts.Add(Held("no key", 100.0f), At(0));
  toasts.Add(Held("no key", 100.0f), At(0));
  Check(toasts.OnScreen().size() == 3, "toasts without a key never merge");
}

// Only the limit is on screen. More wait, and each takes the room of a removed
// one, starting its hold then. A waiting toast merges by key too.
void TestQueue() {
  ToastQueue toasts(2);
  const uint64_t first = toasts.Add(Held("one", 100.0f), At(0));
  toasts.Add(Held("two", 100.0f), At(0));
  const uint64_t third = toasts.Add(Held("three", 100.0f, "third"), At(0));
  const uint64_t fourth = toasts.Add(Held("four", 100.0f), At(0));
  Check(toasts.OnScreen().size() == 2 && toasts.Waiting() == 2,
        "only the limit is on screen");
  Check(toasts.Add(Held("three again", 100.0f, "third"), At(0)) == third &&
            toasts.Waiting() == 2,
        "a waiting toast merges by key");
  toasts.Remove(first, At(500));
  Check(toasts.OnScreen().size() == 2 && toasts.OnScreen()[1].id == third,
        "the oldest waiting takes the room");
  Check(TextOf(toasts.OnScreen()[1]) == "three again",
        "with the text it merged");
  toasts.Update(At(599));
  Check(toasts.OnScreen()[1].shown, "and its hold starts when it is shown");
  Check(toasts.Dismiss(fourth) && toasts.Waiting() == 0,
        "dismissing a waiting toast forgets it");
  ToastQueue none(0);
  none.Add(Held("x", 1.0f), At(0));
  Check(none.OnScreen().size() == 1 && none.Waiting() == 0,
        "a limit of 0 still shows one");
}

// A flood past the waiting limit keeps the newest waiting and drops the oldest.
// Without the limit all of them would wait, and dropping the newest instead
// would leave the first waiting toast at the front.
void TestWaitingLimit() {
  ToastQueue toasts(1);
  toasts.Add(Held("shown", 100.0f), At(0));
  constexpr size_t FLOOD = 2 * MAX_WAITING_TOASTS;
  for (size_t index = 0; index < FLOOD; ++index) {
    toasts.Add(Held("waiting " + std::to_string(index), 100.0f), At(0));
  }
  Check(toasts.Waiting() == MAX_WAITING_TOASTS, "only the limit waits");
  toasts.Remove(toasts.OnScreen()[0].id, At(0));
  Check(TextOf(toasts.OnScreen()[0]) ==
            "waiting " + std::to_string(FLOOD - MAX_WAITING_TOASTS),
        "and the oldest are the ones dropped");
}

// A toast is reported as newly shown once, when it comes on screen, whether
// added with room or promoted when another leaves, and not while it waits or
// when a repeat refreshes it. Recording in Add rather than when shown would
// report the waiting one too early.
void TestNewlyShown() {
  ToastQueue toasts(1);
  std::vector<uint64_t> shown;
  const uint64_t first = toasts.Add(Held("first", 100.0f, "a"), At(0));
  const uint64_t second = toasts.Add(Held("second", 100.0f), At(0));
  toasts.TakeNewlyShown(shown);
  Check(shown == std::vector<uint64_t>{first},
        "only the toast on screen is newly shown");
  shown.clear();
  toasts.TakeNewlyShown(shown);
  Check(shown.empty(), "and only once");
  toasts.Add(Held("first again", 100.0f, "a"), At(10));
  toasts.TakeNewlyShown(shown);
  Check(shown.empty(), "a repeat refreshing it doesn't show it again");
  toasts.Remove(first, At(200));
  toasts.TakeNewlyShown(shown);
  Check(shown == std::vector<uint64_t>{second},
        "a waiting toast is newly shown when it takes the room");
}

// A toast's options travel with it through the queue, and a repeat by key
// brings its own. A toast has no sound unless given one.
void TestToastOptions() {
  ToastQueue toasts(1);
  const uint64_t shown = toasts.Add(Held("shown", 100.0f), At(0));
  Check(toasts.OnScreen()[0].toast.options.sound == ToastSound::None,
        "a toast has no sound by default");
  toasts.Add({.build = Labelled{"unlock"},
              .options = {.hold_ms = 250.0f, .sound = ToastSound::Unlock}},
             At(0));
  toasts.Remove(shown, At(10));
  const ToastEntry &entry = toasts.OnScreen()[0];
  Check(TextOf(entry) == "unlock" &&
            entry.toast.options.sound == ToastSound::Unlock &&
            entry.toast.options.hold_ms == 250.0f,
        "a toast's options travel with it through the queue");
  toasts.Add({.build = Labelled{"unlock again"},
              .options = {.sound = ToastSound::None}},
             At(20));
  Check(TextOf(toasts.OnScreen()[0]) == "unlock" &&
            toasts.OnScreen()[0].toast.options.sound == ToastSound::Unlock,
        "and an unkeyed toast never merges into it");
}

// The game's grades 1 to 4 are platinum to bronze, and anything else unknown.
void TestGradeOf() {
  using Libs::Graphics::SystemUi::GradeOf;
  using Libs::Graphics::SystemUi::TrophyGrade;
  Check(GradeOf(1) == TrophyGrade::Platinum &&
            GradeOf(2) == TrophyGrade::Gold &&
            GradeOf(3) == TrophyGrade::Silver &&
            GradeOf(4) == TrophyGrade::Bronze,
        "grades 1 to 4 are platinum, gold, silver and bronze");
  Check(GradeOf(0) == TrophyGrade::Unknown &&
            GradeOf(5) == TrophyGrade::Unknown &&
            GradeOf(-1) == TrophyGrade::Unknown,
        "and any other grade is unknown");
}

// The trophy consumer turns each unlock into one toast on the toast service,
// held for the given hold and greeted with the unlock sound. An unlock
// published before it subscribes is not seen. What the toast shows is built by
// its builder, which the gallery draws.
void TestTrophyConsumer() {
  using namespace Libs::Graphics::SystemUi;
  Publish(TrophyEvent{.name = "Too early", .grade = 2});
  ToastService toasts;
  TrophyEventConsumer consumer(toasts, "Trophy earned", 7000.0f);
  Check(!consumer.HasPending(),
        "an unlock before the consumer exists is not seen");
  const auto icon =
      std::make_shared<const std::vector<std::byte>>(4, std::byte{2});
  Publish(TrophyEvent{.name = "Champion", .grade = 1, .icon_png = icon});
  Check(consumer.HasPending(), "an unlock after it is waiting");
  Check(toasts.OnScreen().empty() && !toasts.NeedsFrames(),
        "the service starts with no toasts and needs no frames");
  consumer.Consume(At(0));
  // Asked before anything reads the queue, so the toast is still in the inbox.
  Check(toasts.NeedsFrames(),
        "an add not yet drawn makes the service need frames");
  const std::vector<ToastEntry> on_screen = toasts.OnScreen();
  Check(!consumer.HasPending() && on_screen.size() == 1,
        "and becomes one toast");
  Check(on_screen[0].toast.build != nullptr &&
            on_screen[0].toast.options.hold_ms == 7000.0f &&
            on_screen[0].toast.options.sound == ToastSound::Unlock &&
            on_screen[0].toast.options.key.empty() &&
            on_screen[0].toast.options.shine,
        "built by its view, held for the hold it was given, with the unlock "
        "sound, no key and a shine");
}

// The toast in `entries` with `id`, or null.
const ToastEntry *Find(const std::vector<ToastEntry> &entries, uint64_t id) {
  for (const ToastEntry &entry : entries) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

// The service's adds and dismisses go through its inbox in order. A repeat by
// key keeps the toast already there, and its own id still dismisses it, which
// would dismiss nothing without the alias. A dismiss right behind its add, both
// still in the inbox, applies after it.
void TestServiceInbox() {
  using Libs::Graphics::SystemUi::ToastService;
  ToastService toasts;
  const uint64_t first = toasts.Add(
      Labelled{"pad"}, {.key = "pad.inbox", .hold_ms = 100000.0f}, At(0));
  const uint64_t again =
      toasts.Add(Labelled{"pad again"},
                 {.key = "pad.inbox", .hold_ms = 100000.0f}, At(10));
  std::vector<ToastEntry> entries = toasts.OnScreen();
  const ToastEntry *pad = Find(entries, first);
  Check(again != first && pad != nullptr && Find(entries, again) == nullptr &&
            TextOf(*pad) == "pad again" && pad->shown,
        "a repeat by key merges into the toast there, keeping its id");
  toasts.Dismiss(again);
  entries = toasts.OnScreen();
  pad = Find(entries, first);
  Check(pad != nullptr && !pad->shown, "and the repeat's id dismisses it");
  const uint64_t quick =
      toasts.Add(Labelled{"quick"}, {.hold_ms = 100000.0f}, At(20));
  toasts.Dismiss(quick);
  entries = toasts.OnScreen();
  const ToastEntry *dismissed = Find(entries, quick);
  Check(dismissed != nullptr && !dismissed->shown,
        "a dismiss right behind its add applies after it");
}

// The notification consumer turns each published notice into one toast on the
// service it was given, and sees none published before it subscribes.
void TestNotificationConsumer() {
  using namespace Libs::Graphics::SystemUi;
  PublishNotification("Too early", Status::Error);
  ToastService toasts;
  NotificationEventConsumer consumer(toasts);
  Check(!consumer.HasPending(),
        "a notice before the consumer exists is not seen");
  Check(toasts.OnScreen().empty() && toasts.Waiting() == 0,
        "the service starts with no toasts");
  PublishNotification("Controller 1 connected");
  Check(consumer.HasPending(), "a notice after it is waiting");
  consumer.Consume(At(0));
  Check(!consumer.HasPending() && toasts.OnScreen().size() == 1 &&
            toasts.Waiting() == 0,
        "and becomes one toast on the service it was given");
  Check(!toasts.OnScreen()[0].toast.options.shine,
        "and a notice doesn't shine, as it marks no moment");
}

// An instance gives its toasts a consumer of notifications and one
// of trophies. A consumer with events waiting is what makes the toasts need
// frames before anything is drawn, so each instance starts needing none and
// needs them only once an event of the kind under test is published. Each kind
// has an instance of its own, set up after the other kind was published, so
// neither can be made to need frames by the other's event. Making one needs no
// ImGui context.
void TestInstanceConsumers() {
  using namespace Libs::Graphics::SystemUi;
  Instance notices(DEFAULT_THEME, {.trophy = {.subtitle = "Trophy earned"}});
  Check(!notices.Toasts().NeedsFrames(),
        "an instance just set up has nothing waiting");
  PublishNotification("Controller 1 connected");
  Check(notices.Toasts().NeedsFrames(),
        "a notice waits on the instance's toasts");
  Instance trophies(DEFAULT_THEME, {.trophy = {.subtitle = "Trophy earned"}});
  Check(!trophies.Toasts().NeedsFrames(),
        "a second instance sees no earlier notice");
  Publish(TrophyEvent{.name = "Champion", .grade = 1});
  Check(trophies.Toasts().NeedsFrames(),
        "an unlock waits on the instance's toasts");
}

// A 1x1 transparent PNG, the smallest image the cache decodes.
constexpr std::array<uint8_t, 67> ONE_PIXEL_PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
    0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

// A fresh copy of the one-pixel PNG. The cache tells images apart by their
// bytes' address, so each copy is another image.
Libs::Graphics::SystemUi::EncodedImage OnePixel() {
  const auto bytes = std::as_bytes(std::span(ONE_PIXEL_PNG));
  return std::make_shared<const std::vector<std::byte>>(bytes.begin(),
                                                        bytes.end());
}

// The folder the downloaded font files are in, ending in a separator.
std::string FontFolder() { return std::string(KYTY_NOTO_SANS_DIR) + "/"; }

// The theme with its fonts pointing at the downloaded font files, which sit in
// a folder of their own rather than under fonts/.
Libs::Graphics::SystemUi::ThemeContext NotoTheme() {
  Libs::Graphics::SystemUi::ThemeContext theme =
      Libs::Graphics::SystemUi::DEFAULT_THEME;
  theme.fonts.regular = "NotoSans-Regular.ttf";
  theme.fonts.bold = "NotoSans-Bold.ttf";
  return theme;
}

// Makes an ImGui context the instance's fonts and images can be made in.
// Textures are registered only with a renderer that takes them, as the
// overlay's Vulkan backend does.
void CreateImGuiContext() {
  ImGui::CreateContext();
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
}

// Whether an instance whose cache has room for `capacity` images gives a
// second image a texture in the same frame.
bool SecondImageFits(const Libs::Graphics::SystemUi::ThemeContext &theme,
                     size_t capacity) {
  using namespace Libs::Graphics::SystemUi;
  Instance system_ui(theme, {.fonts = {.assets_folder = FontFolder()},
                             .images = {.capacity = capacity}});
  ImageCache &images = system_ui.Images();
  images.BeginFrame();
  const EncodedImage first = OnePixel();
  const EncodedImage second = OnePixel();
  Check(images.Use(first).texture != nullptr, "a first image always fits");
  const bool fits = images.Use(second).texture != nullptr;
  system_ui.ReleaseRendering();
  return fits;
}

// Whether `element` or anything inside it is a text saying `text`.
bool HasText(const Libs::Graphics::SystemUi::FrameBuilder &frame,
             Libs::Graphics::SystemUi::AnyElement element,
             std::string_view text) {
  using namespace Libs::Graphics::SystemUi;
  const TextProps *props = frame.FindAs<TextProps>(element);
  if (props != nullptr && props->text == text) {
    return true;
  }
  for (size_t index = 0; index < frame.ChildCount(element); ++index) {
    if (HasText(frame, frame.Child(element, index), text)) {
      return true;
    }
  }
  return false;
}

// The trophy toasts an instance shows say the subtitle and stay for the hold
// it was set up with. Its consumers handle their events only when its toasts
// are drawn, so one frame is drawn into a bare draw list, then the trophy
// toast that came on screen is built again to read its text. Both values
// differ from the options' defaults, an empty subtitle and the default hold.
void TestInstanceTrophyOptions() {
  using namespace Libs::Graphics::SystemUi;
  constexpr std::string_view SUBTITLE = "Well played";
  constexpr float HOLD_MS = 4321.0f;
  constexpr float WIDTH = 1920.0f;
  constexpr float HEIGHT = 1080.0f;
  Check(HOLD_MS != DEFAULT_TOAST_HOLD_MS, "the hold differs from the default");
  CreateImGuiContext();
  const ThemeContext theme = NotoTheme();
  {
    Instance system_ui(theme, {.fonts = {.assets_folder = FontFolder()},
                               .trophy = {.subtitle = std::string(SUBTITLE),
                                          .hold_ms = HOLD_MS}});
    Publish(TrophyEvent{.name = "Champion", .grade = 1});
    ImDrawListSharedData shared;
    ImDrawList list(&shared);
    list._ResetForNewFrame();
    list.PushClipRectFullScreen();
    list.PushTexture(ImTextureRef());
    ImageCache &images = system_ui.Images();
    images.BeginFrame();
    const std::vector<ToastEntry> appeared =
        system_ui.Toasts().Draw({.list = list,
                                 .area = {.width = WIDTH, .height = HEIGHT},
                                 .theme = theme,
                                 .fonts = system_ui.Fonts(),
                                 .images = &images,
                                 .clock = ClockContext{At(0)}});
    Check(appeared.size() == 1 &&
              appeared[0].toast.options.sound == ToastSound::Unlock,
          "the unlock comes on screen as a trophy toast");
    Check(appeared[0].toast.options.hold_ms == HOLD_MS,
          "held for the hold the instance was set up with");
    FrameBuilder frame;
    frame.Begin();
    const ScaleContext scale(theme.scale, WIDTH, HEIGHT);
    const FrameScope scope(frame, theme, scale, system_ui.Fonts(), {});
    Check(HasText(frame, appeared[0].toast.build(), SUBTITLE),
          "and saying the subtitle it was set up with");
    system_ui.ReleaseRendering();
  }
  ImGui::DestroyContext();
}

// An instance loads its fonts and makes its image cache only when they are
// first asked for, as the drawing thread may have no ImGui context when it is
// made, and only once however often they are asked for. Each weight comes from
// the file the theme names in the assets folder, with the fallbacks merged
// right after it, and the cache holds as many images as it was given room
// for.
void TestInstanceRendering() {
  using namespace Libs::Graphics::SystemUi;
  CreateImGuiContext();
  const ThemeContext theme = NotoTheme();
  const ImVector<ImFont *> &added = ImGui::GetIO().Fonts->Fonts;
  // The font each merge followed, or nothing for a merge before any font.
  // Merged fonts join the last font added, so each weight's fallbacks must
  // come right after it.
  std::vector<std::string> merged_after;
  {
    Instance system_ui(
        theme,
        {.fonts = {.assets_folder = FontFolder(),
                   .merge_fallbacks = [&merged_after, &added] {
                     merged_after.emplace_back(
                         added.empty() ? "" : added.back()->GetDebugName());
                   }}});
    Check(added.empty(), "making an instance loads no fonts");
    const FontsContext &fonts = system_ui.Fonts();
    // ImGui names a font after the file it came from.
    Check(fonts.regular != nullptr && fonts.bold != nullptr &&
              std::string_view(fonts.regular->GetDebugName()) ==
                  "NotoSans-Regular.ttf" &&
              std::string_view(fonts.bold->GetDebugName()) ==
                  "NotoSans-Bold.ttf",
          "asking for its fonts loads each weight from the file the theme "
          "names in the folder");
    Check(merged_after == std::vector<std::string>{"NotoSans-Regular.ttf",
                                                   "NotoSans-Bold.ttf"},
          "with the fallbacks merged right after each");
    (void)system_ui.Images();
    (void)system_ui.Fonts();
    Check(added.Size == 2, "and asking again loads nothing more");
    system_ui.ReleaseRendering();
  }
  Check(!SecondImageFits(theme, 1), "a cache of one has no room for a second");
  Check(SecondImageFits(theme, 2), "and one of two does");
  ImGui::DestroyContext();
}

// Releasing an instance's rendering drops its image cache, so the cache asked
// for afterwards is a fresh one holding nothing.
void TestInstanceReleaseRendering() {
  using namespace Libs::Graphics::SystemUi;
  CreateImGuiContext();
  const ThemeContext theme = NotoTheme();
  {
    Instance system_ui(theme, {.fonts = {.assets_folder = FontFolder()}});
    system_ui.Images().BeginFrame();
    const EncodedImage shown = OnePixel();
    Check(system_ui.Images().Use(shown).texture != nullptr &&
              system_ui.Images().NeedsFrames(),
          "an image shown is held by the cache");
    system_ui.ReleaseRendering();
    Check(!system_ui.Images().NeedsFrames(),
          "and released with the rendering, so the cache asked for next "
          "holds nothing");
    system_ui.ReleaseRendering();
  }
  ImGui::DestroyContext();
}

// An instance reads the theme's font paths when it is made, as they are views
// that may not last until the fonts load. Here they view strings that change
// before then. Each keeps its length, so it keeps its buffer and the views see
// the new text, which names no file, if they were read late.
void TestInstanceReadsThemeOnce() {
  using namespace Libs::Graphics::SystemUi;
  CreateImGuiContext();
  std::string regular = "NotoSans-Regular.ttf";
  std::string bold = "NotoSans-Bold.ttf";
  ThemeContext theme = DEFAULT_THEME;
  theme.fonts.regular = regular;
  theme.fonts.bold = bold;
  {
    Instance system_ui(theme, {.fonts = {.assets_folder = FontFolder()}});
    regular.assign(regular.size(), 'x');
    bold.assign(bold.size(), 'x');
    const FontsContext &fonts = system_ui.Fonts();
    Check(fonts.regular != nullptr && fonts.bold != nullptr &&
              std::string_view(fonts.regular->GetDebugName()) ==
                  "NotoSans-Regular.ttf" &&
              std::string_view(fonts.bold->GetDebugName()) ==
                  "NotoSans-Bold.ttf",
          "the fonts load from the paths the theme named when the instance "
          "was made");
    system_ui.ReleaseRendering();
  }
  ImGui::DestroyContext();
}

// The vertices `toasts` draws in a frame at `ms` with `motion`, at the
// theme's reference size.
ImVector<ImDrawVert>
DrawToastsAt(Libs::Graphics::SystemUi::ToastService &toasts, float ms,
             const Libs::Graphics::SystemUi::MotionContext &motion) {
  using namespace Libs::Graphics::SystemUi;
  const ThemeContext &theme = DEFAULT_THEME;
  const FontsContext fonts{};
  ImDrawListSharedData shared;
  ImDrawList list(&shared);
  list._ResetForNewFrame();
  list.PushClipRectFullScreen();
  list.PushTexture(ImTextureRef());
  (void)toasts.Draw({.list = list,
                     .area = {.width = theme.scale.reference_width,
                              .height = theme.scale.reference_height},
                     .theme = theme,
                     .fonts = fonts,
                     .clock = ClockContext{At(ms)},
                     .motion = motion});
  return list.VtxBuffer;
}

// The leftmost point of `vertices`, the drawn toast's left edge.
float LeftEdge(const ImVector<ImDrawVert> &vertices) {
  Check(vertices.Size > 0, "the toast draws");
  float left = std::numeric_limits<float>::infinity();
  for (const ImDrawVert &vertex : vertices) {
    left = std::min(left, vertex.pos.x);
  }
  return left;
}

// How far right of its place a toast drawn `ms` after it arrived sits, with
// `motion`. Its first frame starts its animations while it is still clear and
// draws nothing, so the frame `ms` later is compared with one long after it is
// in.
float ToastShiftAt(float ms,
                   const Libs::Graphics::SystemUi::MotionContext &motion) {
  using namespace Libs::Graphics::SystemUi;
  const ThemeContext &theme = DEFAULT_THEME;
  ToastService toasts;
  (void)toasts.Add(
      [] {
        return Box(
            {.self = {.layout = {.width = Dp(200.0f), .height = Dp(40.0f)},
                      .styles = {.fill = Color{255, 0, 0, 255}}}});
      },
      {.hold_ms = std::numeric_limits<float>::infinity()}, At(0));
  (void)DrawToastsAt(toasts, 0.0f, motion);
  const float arriving = LeftEdge(DrawToastsAt(toasts, ms, motion));
  const float settled =
      LeftEdge(DrawToastsAt(toasts, theme.motion.base * 4.0f, motion));
  return arriving - settled;
}

// A toast slides in from the right by the toast slide distance along the
// theme's enter curve, worked out here, and with reduced motion it appears in
// its place.
void TestToastSlidesIn() {
  using namespace Libs::Graphics::SystemUi;
  const ThemeContext &theme = DEFAULT_THEME;
  const float distance = ToastDefaults(theme).slide_distance.value;
  const float early = theme.motion.base / 6.0f;
  const float along = 1.0f - std::pow(1.0f - early / theme.motion.base, 3.0f);
  CreateImGuiContext();
  Check(std::abs(ToastShiftAt(early, {}) - distance * (1.0f - along)) < 0.01f,
        "a toast arriving sits right of its place by what remains of its "
        "slide");
  Check(std::abs(ToastShiftAt(early, {.reduced = true})) < 0.01f,
        "and with reduced motion it appears in its place");
  ImGui::DestroyContext();
}

// What a toast service holding one toast draws once it is fully in, with the
// fonts and images given, at the theme's reference size. The first frame
// starts its animations while it is still clear, so the frame long after it is
// the one returned.
struct DrawnToast {
  std::vector<ImDrawVert> vertices;
  // Whether any command draws with a texture. Text draws from the font atlas,
  // so only a toast without text shows by this whether it drew an image.
  bool textured = false;
  // The draw commands and the indices they read, so a check can tell which
  // clip a vertex was drawn under.
  std::vector<ImDrawCmd> commands;
  std::vector<ImDrawIdx> indices;
};

DrawnToast DrawSettledToast(Libs::Graphics::SystemUi::ToastService &toasts,
                            const Libs::Graphics::SystemUi::ThemeContext &theme,
                            const Libs::Graphics::SystemUi::FontsContext &fonts,
                            Libs::Graphics::SystemUi::ImageCache *images) {
  using namespace Libs::Graphics::SystemUi;
  constexpr float SETTLED_MS = 2000.0f;
  DrawnToast drawn;
  for (const float ms : {0.0f, SETTLED_MS}) {
    ImDrawListSharedData shared;
    // The overlay's tessellation, as rounded corners need segment counts.
    constexpr float TESSELLATION = 0.30f;
    shared.SetCircleTessellationMaxError(TESSELLATION);
    // The screen as a frame sets it, so a clip pushed inside it, such as the
    // band's strip, is its own rectangle rather than cut to nothing.
    shared.ClipRectFullscreen = ImVec4(0.0f, 0.0f, theme.scale.reference_width,
                                       theme.scale.reference_height);
    ImDrawList list(&shared);
    list._ResetForNewFrame();
    list.PushClipRectFullScreen();
    list.PushTexture(ImTextureRef());
    if (images != nullptr) {
      images->BeginFrame();
    }
    (void)toasts.Draw({.list = list,
                       .area = {.width = theme.scale.reference_width,
                                .height = theme.scale.reference_height},
                       .theme = theme,
                       .fonts = fonts,
                       .images = images,
                       .clock = ClockContext{At(ms)}});
    drawn.vertices.assign(list.VtxBuffer.begin(), list.VtxBuffer.end());
    drawn.commands.assign(list.CmdBuffer.begin(), list.CmdBuffer.end());
    drawn.indices.assign(list.IdxBuffer.begin(), list.IdxBuffer.end());
    drawn.textured = std::any_of(list.CmdBuffer.begin(), list.CmdBuffer.end(),
                                 [](const ImDrawCmd &command) {
                                   return command.ElemCount > 0 &&
                                          command.TexRef._TexData != nullptr;
                                 });
  }
  return drawn;
}

// Whether any vertex drawn fully in `color` was drawn under a clip no taller
// than `strip_height`, as the band's strip is. The band is the toast's whole
// rounded fill cut to that strip, so its vertices span the toast and only its
// clip tells it from the status glyph drawn in the same colour.
bool StripHasColor(const DrawnToast &drawn,
                   Libs::Graphics::SystemUi::Color color, float strip_height) {
  const ImU32 wanted = IM_COL32(color.r, color.g, color.b, color.a);
  for (const ImDrawCmd &command : drawn.commands) {
    // An empty clip shows nothing, so what is drawn under it isn't the band.
    const float clip_height = command.ClipRect.w - command.ClipRect.y;
    if (clip_height <= 0.0f || clip_height > strip_height) {
      continue;
    }
    for (unsigned int index = 0; index < command.ElemCount; ++index) {
      const ImDrawIdx vertex = drawn.indices[command.IdxOffset + index];
      if (drawn.vertices[command.VtxOffset + vertex].col == wanted) {
        return true;
      }
    }
  }
  return false;
}

// A notice published with a status is drawn with that status's colour in its
// band, through the instance's consumer, and a notice with another status is
// not. The status colours are checked to differ first, so the check can't pass
// by every status sharing one colour. The notice's glyph is in the status
// colour too, so only what is drawn under the band's strip clip counts.
void TestNoticeStatusReachesBand() {
  using namespace Libs::Graphics::SystemUi;
  CreateImGuiContext();
  const ThemeContext theme = NotoTheme();
  const Color error = StatusColor(theme, Status::Error);
  const Color info = StatusColor(theme, Status::Info);
  Check(!(error == info), "the error and info colours differ");
  // The band's strip at the reference size, with a pixel's room for the clip's
  // bottom being rounded to a whole pixel.
  const float strip_height = ToastDefaults(theme).edge_width.value + 1.0f;
  const auto band_colors = [&](Status status) {
    Instance system_ui(theme, {.fonts = {.assets_folder = FontFolder()}});
    PublishNotification("Save failed", status);
    const DrawnToast drawn = DrawSettledToast(
        system_ui.Toasts(), theme, system_ui.Fonts(), &system_ui.Images());
    const std::pair<bool, bool> found{
        StripHasColor(drawn, error, strip_height),
        StripHasColor(drawn, info, strip_height)};
    system_ui.ReleaseRendering();
    return found;
  };
  const auto [error_in_error, info_in_error] = band_colors(Status::Error);
  Check(error_in_error && !info_in_error,
        "an error notice has the error colour in its band");
  const auto [error_in_info, info_in_info] = band_colors(Status::Info);
  Check(info_in_info && !error_in_info,
        "and an info notice has the info colour instead");
  ImGui::DestroyContext();
}

// How many vertices in `vertices` are fully `color`.
size_t ColorCount(const std::vector<ImDrawVert> &vertices,
                  Libs::Graphics::SystemUi::Color color) {
  const ImU32 wanted = IM_COL32(color.r, color.g, color.b, color.a);
  return static_cast<size_t>(std::count_if(
      vertices.begin(), vertices.end(),
      [wanted](const ImDrawVert &vertex) { return vertex.col == wanted; }));
}

// A notice draws its status glyph in its status colour. The band is in that
// colour too, so the check compares the notice with the same text on a toast
// built as notices were before the glyph, the same band and text and no glyph.
// Only the glyph differs, so the notice drawing more vertices in the status
// colour shows the glyph is drawn.
void TestNoticeShowsStatusGlyph() {
  using namespace Libs::Graphics::SystemUi;
  CreateImGuiContext();
  const ThemeContext theme = NotoTheme();
  const Color info = StatusColor(theme, Status::Info);
  // The lines a notice's text wraps to, so the baseline's text is styled as the
  // notice's.
  constexpr int NOTICE_LINES = 3;
  const auto info_count = [&](auto build) {
    Instance system_ui(theme, {.fonts = {.assets_folder = FontFolder()}});
    ToastService toasts;
    (void)toasts.Add(build, {.hold_ms = std::numeric_limits<float>::infinity()},
                     At(0));
    const DrawnToast drawn =
        DrawSettledToast(toasts, theme, system_ui.Fonts(), &system_ui.Images());
    system_ui.ReleaseRendering();
    return ColorCount(drawn.vertices, info);
  };
  const size_t without_glyph = info_count([&] {
    const std::array<AnyElement, 1> text{
        Text({.text = "Game saved",
              .self = {.styles = {.type = theme.type.body_small,
                                  .color = theme.colors.text.primary,
                                  .overflow = TextOverflow::Wrap,
                                  .max_lines = NOTICE_LINES}}})};
    return Toast({.status = Status::Info}, text);
  });
  const size_t with_glyph = info_count([] {
    return Notification({.status = Status::Info, .text = "Game saved"});
  });
  Check(without_glyph > 0, "a toast with a band draws the status colour");
  Check(with_glyph > without_glyph,
        "and a notice draws more of it, in its glyph");
  ImGui::DestroyContext();
}

// A toast service draws the images in its toasts from the cache its frame
// names, and with none named, draws them from no cache at all, even inside a
// scope the caller left open, as the service never relies on its caller's
// scope. The same toast drawn with the cache named shows its image, so the
// check can tell an image from none.
void TestToastsOwnImageScope() {
  using namespace Libs::Graphics::SystemUi;
  CreateImGuiContext();
  const ThemeContext theme = NotoTheme();
  {
    Instance system_ui(theme, {.fonts = {.assets_folder = FontFolder()}});
    static const EncodedImage icon = OnePixel();
    constexpr float ICON_SIDE = 32.0f;
    // A toast is a box, so the image sits in one.
    const auto image_toast = [] {
      const std::array<AnyElement, 1> shown{
          Image({.self = {.layout = {.width = Dp(ICON_SIDE),
                                     .height = Dp(ICON_SIDE)}},
                 .source = &icon})};
      return Box({}, shown);
    };
    ImageCache &images = system_ui.Images();
    ToastService named;
    (void)named.Add(image_toast,
                    {.hold_ms = std::numeric_limits<float>::infinity()}, At(0));
    Check(DrawSettledToast(named, theme, system_ui.Fonts(), &images).textured,
          "a toast drawn with its frame's cache shows its image");
    ToastService unnamed;
    (void)unnamed.Add(image_toast,
                      {.hold_ms = std::numeric_limits<float>::infinity()},
                      At(0));
    {
      images.BeginFrame();
      const ImageCacheScope callers(images);
      Check(!DrawSettledToast(unnamed, theme, system_ui.Fonts(), nullptr)
                 .textured,
            "and one drawn with no cache named shows none, though its caller "
            "has one open");
    }
    system_ui.ReleaseRendering();
  }
  ImGui::DestroyContext();
}

// Draws `toasts` at each of `times` in turn, in the default theme at its
// reference size, and returns the vertices the last frame drew.
std::vector<ImDrawVert>
DrawToastFrames(Libs::Graphics::SystemUi::ToastService &toasts,
                std::initializer_list<float> times) {
  using namespace Libs::Graphics::SystemUi;
  const ThemeContext &theme = DEFAULT_THEME;
  std::vector<ImDrawVert> vertices;
  for (const float ms : times) {
    ImDrawListSharedData shared;
    constexpr float TESSELLATION = 0.30f;
    shared.SetCircleTessellationMaxError(TESSELLATION);
    shared.ClipRectFullscreen = ImVec4(0.0f, 0.0f, theme.scale.reference_width,
                                       theme.scale.reference_height);
    ImDrawList list(&shared);
    list._ResetForNewFrame();
    list.PushClipRectFullScreen();
    list.PushTexture(ImTextureRef());
    (void)toasts.Draw({.list = list,
                       .area = {.width = theme.scale.reference_width,
                                .height = theme.scale.reference_height},
                       .theme = theme,
                       .fonts = {},
                       .images = nullptr,
                       .clock = ClockContext{At(ms)}});
    vertices.assign(list.VtxBuffer.begin(), list.VtxBuffer.end());
  }
  return vertices;
}

// How many of `vertices` are lit white, the theme's shine colour. The toast's
// own border may be a faint white too, so a count is only compared with the
// same toast drawn without a shine.
size_t LitWhite(const std::vector<ImDrawVert> &vertices) {
  constexpr ImU32 RGB_MASK = IM_COL32(255, 255, 255, 0);
  return static_cast<size_t>(std::count_if(
      vertices.begin(), vertices.end(), [](const ImDrawVert &vertex) {
        return (vertex.col & RGB_MASK) == IM_COL32(255, 255, 255, 0) &&
               (vertex.col >> IM_COL32_A_SHIFT) > 0;
      }));
}

// A toast with no text, so the font isn't needed and nothing else it draws is
// the shine's white apart from its border, which the comparisons cancel out.
Libs::Graphics::SystemUi::BoxElement PlainToast() {
  using namespace Libs::Graphics::SystemUi;
  constexpr float SIDE = 64.0f;
  const std::array<AnyElement, 1> content{
      Box({.self = {.layout = {.width = Dp(SIDE), .height = Dp(SIDE)}}})};
  return Toast({}, content);
}

// A toast asked to shine waits the theme's shine delay as it comes on screen,
// drawing as one not asked meanwhile, so the band crosses a toast that has come
// to rest. Then it sweeps a band across itself, where the same toast not asked
// doesn't, and once the sweep is over the two draw alike again.
void TestServiceShinesOptedToast() {
  using namespace Libs::Graphics::SystemUi;
  CreateImGuiContext();
  const float delay = DEFAULT_THEME.motion.shine_delay;
  const float sweep = DEFAULT_THEME.motion.shine;
  constexpr float SETTLED_MS = 2000.0f;
  Check(delay >= DEFAULT_THEME.motion.base && delay + sweep < SETTLED_MS,
        "the shine waits until the toast has arrived and is over by the time "
        "checked last");
  const auto lit_at = [&](bool shine, float ms) {
    ToastService toasts;
    (void)toasts.Add(
        PlainToast,
        {.hold_ms = std::numeric_limits<float>::infinity(), .shine = shine},
        At(0));
    return LitWhite(DrawToastFrames(toasts, {0.0f, ms}));
  };
  Check(lit_at(true, delay / 2.0f) == lit_at(false, delay / 2.0f),
        "a toast asked to shine shows no band while it waits");
  const float halfway = delay + sweep / 2.0f;
  Check(lit_at(true, halfway) > lit_at(false, halfway),
        "then sweeps a band across itself");
  Check(lit_at(true, SETTLED_MS) == lit_at(false, SETTLED_MS),
        "and once the sweep is over it draws as one not asked");
  ImGui::DestroyContext();
}

// A shining toast that waits for room sweeps once it comes on screen, not once
// it was added. The screen is full of toasts that leave well after the shine's
// delay and sweep have passed since the add, so a sweep timed from the add
// would be over by the time the toast is drawn, and the check halfway through
// its sweep from when it was shown would find no band.
void TestWaitingToastShinesWhenShown() {
  using namespace Libs::Graphics::SystemUi;
  CreateImGuiContext();
  const float delay = DEFAULT_THEME.motion.shine_delay;
  const float sweep = DEFAULT_THEME.motion.shine;
  constexpr float HOLD_MS = 1200.0f;
  constexpr float FRAME_MS = 16.0f;
  constexpr float GIVE_UP_MS = 5000.0f;
  const auto lit_halfway = [&](bool shine) {
    ToastService toasts;
    for (size_t index = 0; index < DEFAULT_VISIBLE_TOASTS; ++index) {
      (void)toasts.Add(PlainToast, {.hold_ms = HOLD_MS}, At(0));
    }
    const uint64_t waiting = toasts.Add(
        PlainToast,
        {.hold_ms = std::numeric_limits<float>::infinity(), .shine = shine},
        At(0));
    // Frames are drawn until the waiting toast comes on screen.
    float shown = 0.0f;
    for (float ms = 0.0f; ms < GIVE_UP_MS && shown == 0.0f; ms += FRAME_MS) {
      (void)DrawToastFrames(toasts, {ms});
      for (const ToastEntry &entry : toasts.OnScreen()) {
        shown = entry.id == waiting ? ms : shown;
      }
    }
    Check(shown > delay + sweep,
          "the waiting toast comes on screen after the shine's delay and "
          "sweep have passed since it was added");
    return LitWhite(DrawToastFrames(
        toasts, {shown + FRAME_MS, shown + delay + sweep / 2.0f}));
  };
  Check(lit_halfway(true) > lit_halfway(false),
        "and halfway through its sweep from then it shows the band");
  ImGui::DestroyContext();
}

// A trophy toast whose icon can be shown glows around the icon as the tile
// does without one, in the grade's colour, and rounds the glow's corners as
// the icon's. The icon branch is told apart from the tile by its two layers,
// the image and its outline, where the tile holds one cup.
void TestTrophyIconGlows() {
  using namespace Libs::Graphics::SystemUi;
  CreateImGuiContext();
  const ThemeContext theme = NotoTheme();
  {
    Instance system_ui(theme, {.fonts = {.assets_folder = FontFolder()}});
    const EncodedImage icon = OnePixel();
    ImageCache &images = system_ui.Images();
    images.BeginFrame();
    {
      const ScaleContext scale(theme.scale, theme.scale.reference_width,
                               theme.scale.reference_height);
      FrameBuilder frame;
      frame.Begin();
      const FrameScope scope(frame, theme, scale, system_ui.Fonts(), {});
      const ImageCacheScope cache(images);
      const AnyElement toast = TrophyUnlocked({.name = "Champion",
                                               .subtitle = "Trophy earned",
                                               .grade = TrophyGrade::Gold,
                                               .icon = icon});
      const AnyElement slot =
          frame.Child(frame.Child(frame.Child(toast, 0), 0), 0);
      const BoxProps *stack = frame.FindAs<BoxProps>(slot);
      const TrophyUnlockedTokens trophy = TrophyUnlockedDefaults(theme);
      Check(stack != nullptr && frame.ChildCount(slot) == 2,
            "a trophy with an icon shows it in a stack of the image and its "
            "outline");
      const BoxGlow &glow = stack->self.styles.glow;
      Check(glow.glow.radius == theme.glow.trophy.radius &&
                glow.glow.peak_alpha == theme.glow.trophy.peak_alpha &&
                glow.color == theme.colors.trophy.gold,
            "and the stack glows with the trophy glow in the grade's colour");
      Check(stack->self.styles.radius == trophy.icon_radius,
            "rounded as the icon is");
    }
    system_ui.ReleaseRendering();
  }
  ImGui::DestroyContext();
}

} // namespace

int main() {
  TestHold();
  TestOddHolds();
  TestMergeByKey();
  TestQueue();
  TestWaitingLimit();
  TestNewlyShown();
  TestToastOptions();
  TestGradeOf();
  TestTrophyConsumer();
  TestServiceInbox();
  TestNotificationConsumer();
  TestInstanceConsumers();
  TestInstanceTrophyOptions();
  TestInstanceRendering();
  TestInstanceReleaseRendering();
  TestInstanceReadsThemeOnce();
  TestToastSlidesIn();
  TestNoticeStatusReachesBand();
  TestNoticeShowsStatusGlyph();
  TestToastsOwnImageScope();
  TestTrophyIconGlows();
  TestServiceShinesOptedToast();
  TestWaitingToastShinesWhenShown();
  std::printf("SystemUiToastsTests: passed\n");
  return 0;
}
