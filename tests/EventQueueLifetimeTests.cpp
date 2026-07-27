#include "kernel/eventQueue.h"

#include "libs/errno.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

namespace EventQueue = Libs::LibKernel::EventQueue;
using Libs::LibKernel::KERNEL_ERROR_EBADF;
using Libs::LibKernel::KERNEL_ERROR_ENOENT;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "EventQueueLifetimeTests: failed: %s\n", text);
    std::abort();
  }
}

void CheckConcurrentResult(int result, const char *text) {
  Check(result == OK || result == KERNEL_ERROR_EBADF ||
            result == KERNEL_ERROR_ENOENT,
        text);
}

void CountDeletedEvent(EventQueue::KernelEqueue,
                       EventQueue::KernelEqueueEvent *event) {
  auto *count = static_cast<std::atomic_uint32_t *>(event->filter.data);
  count->fetch_add(1, std::memory_order_relaxed);
}

struct DuplicateEventOwner {
  std::atomic_uint32_t delete_count{0};
};

void QueueDuplicateEvent(EventQueue::KernelEqueueEvent *event,
                         void *trigger_data) {
  auto next = event->event;
  next.data = reinterpret_cast<intptr_t>(trigger_data);
  if (event->triggered) {
    event->pending_events.push_back(next);
  } else {
    event->event = next;
    event->triggered = true;
  }
}

void ResetDuplicateEvent(EventQueue::KernelEqueueEvent *event) {
  event->triggered = false;
  event->event.data = 0;
}

void DeleteDuplicateEvent(EventQueue::KernelEqueue,
                          EventQueue::KernelEqueueEvent *event) {
  auto *owner = static_cast<DuplicateEventOwner *>(event->filter.data);
  owner->delete_count.fetch_add(1, std::memory_order_relaxed);
}

void PoisonDuplicateEvent(EventQueue::KernelEqueueEvent *, void *) {
  Check(false, "duplicate add replaced trigger callback");
}

void TestDuplicateAddPreservesEventState() {
  EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
  Check(EventQueue::KernelCreateEqueue(&queue, "duplicate-add") == OK,
        "create duplicate add queue");

  auto original_owner = std::make_shared<DuplicateEventOwner>();
  std::weak_ptr<DuplicateEventOwner> weak_original = original_owner;
  EventQueue::KernelEqueueEvent original{};
  original.event.ident = 17;
  original.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
  original.event.udata = reinterpret_cast<void *>(0x1111);
  original.filter.data = original_owner.get();
  original.filter.owner = original_owner;
  original.filter.trigger_func = QueueDuplicateEvent;
  original.filter.reset_func = ResetDuplicateEvent;
  original.filter.delete_event_func = DeleteDuplicateEvent;
  Check(EventQueue::KernelAddEvent(queue, original) == OK,
        "add original duplicate event");
  Check(EventQueue::KernelTriggerEvent(queue, 17,
                                       EventQueue::KERNEL_EVFILT_VIDEO_OUT,
                                       reinterpret_cast<void *>(0x1234)) == OK,
        "queue first trigger");
  Check(EventQueue::KernelTriggerEvent(queue, 17,
                                       EventQueue::KERNEL_EVFILT_VIDEO_OUT,
                                       reinterpret_cast<void *>(0x5678)) == OK,
        "queue pending trigger");

  auto replacement_owner = std::make_shared<DuplicateEventOwner>();
  std::weak_ptr<DuplicateEventOwner> weak_replacement = replacement_owner;
  EventQueue::KernelEqueueEvent duplicate{};
  duplicate.triggered = false;
  duplicate.deadline_ns = 1;
  duplicate.event.ident = 17;
  duplicate.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
  duplicate.event.data = 0x7fffffff;
  duplicate.event.udata = reinterpret_cast<void *>(0x2222);
  duplicate.filter.data = replacement_owner.get();
  duplicate.filter.owner = replacement_owner;
  duplicate.filter.trigger_func = PoisonDuplicateEvent;
  Check(EventQueue::KernelAddEvent(queue, duplicate) == OK,
        "update duplicate event");

  duplicate.filter.owner.reset();
  replacement_owner.reset();
  Check(weak_replacement.expired(), "duplicate owner is not retained");
  original.filter.owner.reset();
  original_owner.reset();
  Check(!weak_original.expired(), "original event owner remains retained");

  EventQueue::KernelEvent events[2]{};
  int out = 0;
  Libs::LibKernel::KernelUseconds timeout = 0;
  Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == OK,
        "read queued duplicate triggers");
  Check(out == 2, "duplicate add preserves pending event count");
  Check(events[0].data == 0x1234 && events[1].data == 0x5678,
        "duplicate add preserves current and pending event data");
  Check(events[0].udata == reinterpret_cast<void *>(0x2222),
        "duplicate add updates current user data");
  Check(events[1].udata == reinterpret_cast<void *>(0x2222),
        "duplicate add updates pending user data");

  EventQueue::KernelEvent timer_event{};
  Check(EventQueue::KernelWaitEqueue(queue, &timer_event, 1, &out, &timeout) ==
            OK &&
            out == 1,
        "duplicate add updates deadline metadata");
  Check(timer_event.data == 0 &&
            timer_event.udata == reinterpret_cast<void *>(0x2222),
        "deadline trigger retains updated duplicate metadata");

  auto retained_owner = weak_original.lock();
  Check(retained_owner != nullptr, "original owner alive before delete");
  Check(EventQueue::KernelDeleteEvent(queue, 17,
                                      EventQueue::KERNEL_EVFILT_VIDEO_OUT) ==
            OK,
        "delete duplicate event");
  Check(retained_owner->delete_count.load(std::memory_order_relaxed) == 1,
        "duplicate add preserves delete callback");
  retained_owner.reset();
  Check(weak_original.expired(), "original owner released on delete");
  Check(EventQueue::KernelDeleteEqueue(queue) == OK,
        "delete duplicate add queue");
}

struct SimulatedVideoOutEventState;

struct SimulatedVideoOutRegistration {
  EventQueue::KernelEqueue handle = EventQueue::KERNEL_EQUEUE_INVALID;
  std::shared_ptr<SimulatedVideoOutEventState> state;
  uint64_t marker = 0x123456789abcdef0ull;
};

struct SimulatedVideoOutEventState {
  SimulatedVideoOutEventState(std::atomic_uint32_t &stage,
                              std::atomic_uint32_t &destroy_count)
      : stage(stage), destroy_count(destroy_count) {}

  ~SimulatedVideoOutEventState() {
    destroy_count.fetch_add(1, std::memory_order_relaxed);
  }

  std::mutex mutex;
  std::vector<std::shared_ptr<SimulatedVideoOutRegistration>> queues;
  std::atomic_uint32_t &stage;
  std::atomic_uint32_t &destroy_count;
  uint64_t marker = 0xfedcba9876543210ull;
};

void DetachSimulatedVideoOutEvent(EventQueue::KernelEqueue queue,
                                  EventQueue::KernelEqueueEvent *event) {
  auto *registration =
      static_cast<SimulatedVideoOutRegistration *>(event->filter.data);
  Check(registration != nullptr && registration->handle == queue,
        "simulated registration identity");
  auto state = registration->state;
  Check(state != nullptr, "simulated event owns shared state");
  state->stage.store(1, std::memory_order_release);
  while (state->stage.load(std::memory_order_acquire) != 2) {
    std::this_thread::yield();
  }

  {
    std::lock_guard lock(state->mutex);
    const auto entry =
        std::find_if(state->queues.begin(), state->queues.end(),
                     [registration](const auto &candidate) {
                       return candidate.get() == registration;
                     });
    if (entry != state->queues.end()) {
      state->queues.erase(entry);
    }
  }
  event->filter.owner.reset();
  Check(state->marker == 0xfedcba9876543210ull &&
            registration->marker == 0x123456789abcdef0ull,
        "callback state survives simulated port destruction");
}

void TestCallbackStateOutlivesPort() {
  EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
  Check(EventQueue::KernelCreateEqueue(&queue, "shared-port-state") == OK,
        "create shared port state queue");

  std::atomic_uint32_t stage{0};
  std::atomic_uint32_t destroy_count{0};
  auto port_state =
      std::make_shared<SimulatedVideoOutEventState>(stage, destroy_count);
  std::weak_ptr<SimulatedVideoOutEventState> weak_state = port_state;
  auto registration = std::make_shared<SimulatedVideoOutRegistration>();
  std::weak_ptr<SimulatedVideoOutRegistration> weak_registration =
      registration;
  registration->handle = queue;
  registration->state = port_state;
  port_state->queues.push_back(registration);

  EventQueue::KernelEqueueEvent event{};
  event.event.ident = 8;
  event.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
  event.filter.data = registration.get();
  event.filter.owner = registration;
  event.filter.delete_event_func = DetachSimulatedVideoOutEvent;
  Check(EventQueue::KernelAddEvent(queue, event) == OK,
        "add shared port state event");
  event.filter.owner.reset();

  std::jthread close([&] {
    Check(EventQueue::KernelDeleteEqueue(queue) == OK,
          "delete shared port state queue");
  });
  while (stage.load(std::memory_order_acquire) != 1) {
    std::this_thread::yield();
  }

  std::vector<std::shared_ptr<SimulatedVideoOutRegistration>> detached;
  {
    std::lock_guard lock(port_state->mutex);
    detached = std::move(port_state->queues);
  }
  registration.reset();
  detached.clear();
  port_state.reset();
  Check(!weak_state.expired(),
        "callback state outlives simulated port object");
  Check(!weak_registration.expired(),
        "registration outlives simulated port object");

  stage.store(2, std::memory_order_release);
  close.join();
  Check(weak_registration.expired(), "detached registration is released");
  Check(weak_state.expired(), "detached event state is released");
  Check(destroy_count.load(std::memory_order_relaxed) == 1,
        "shared event state is destroyed exactly once");
}

struct OwnedCallbackPayload {
  OwnedCallbackPayload(std::atomic_uint32_t &stage,
                       std::atomic_uint32_t &delete_count,
                       std::atomic_uint32_t &destroy_count)
      : stage(stage), delete_count(delete_count), destroy_count(destroy_count) {}

  std::atomic_uint32_t &stage;
  std::atomic_uint32_t &delete_count;
  std::atomic_uint32_t &destroy_count;
  uint64_t marker = 0xc0dec0dec0dec0deull;

  ~OwnedCallbackPayload() {
    destroy_count.fetch_add(1, std::memory_order_relaxed);
  }
};

void DeleteOwnedEvent(EventQueue::KernelEqueue queue,
                      EventQueue::KernelEqueueEvent *event) {
  auto *payload = static_cast<OwnedCallbackPayload *>(event->filter.data);
  Check(payload != nullptr, "owned callback payload");
  Check(!EventQueue::KernelPinEqueue(queue),
        "owned callback runs after registry removal");
  payload->delete_count.fetch_add(1, std::memory_order_relaxed);
  event->filter.owner.reset();
  payload->stage.store(1, std::memory_order_release);
  while (payload->stage.load(std::memory_order_acquire) != 2) {
    std::this_thread::yield();
  }
  Check(payload->marker == 0xc0dec0dec0dec0deull,
        "owned callback payload remains valid");
}

void TestCallbackOwnsPayload() {
  EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
  Check(EventQueue::KernelCreateEqueue(&queue, "owned-callback") == OK,
        "create owned callback queue");

  std::atomic_uint32_t stage{0};
  std::atomic_uint32_t delete_count{0};
  std::atomic_uint32_t destroy_count{0};
  auto registration =
      std::make_shared<OwnedCallbackPayload>(stage, delete_count, destroy_count);
  std::weak_ptr<OwnedCallbackPayload> weak_registration = registration;
  std::vector<std::shared_ptr<OwnedCallbackPayload>> port_registrations{
      registration};
  {
    EventQueue::KernelEqueueEvent event{};
    event.event.ident = 2;
    event.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
    event.filter.data = registration.get();
    event.filter.owner = registration;
    event.filter.delete_event_func = DeleteOwnedEvent;
    Check(EventQueue::KernelAddEvent(queue, event) == OK,
          "add owned callback event");
  }

  std::jthread close([&] {
    Check(EventQueue::KernelDeleteEqueue(queue) == OK,
          "delete owned callback queue");
  });
  while (stage.load(std::memory_order_acquire) != 1) {
    std::this_thread::yield();
  }

  Check(!EventQueue::KernelPinEqueue(queue),
        "owned callback queue removed while callback blocked");
  port_registrations.clear();
  registration.reset();
  Check(!weak_registration.expired(),
        "delete callback retains detached payload");

  stage.store(2, std::memory_order_release);
  close.join();
  Check(delete_count.load(std::memory_order_relaxed) == 1,
        "owned callback runs exactly once");
  Check(weak_registration.expired(),
        "owned callback payload released with event");
  Check(destroy_count.load(std::memory_order_relaxed) == 1,
        "owned callback payload destroyed exactly once");
}

void TestPinnedClose() {
  EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
  Check(EventQueue::KernelCreateEqueue(&queue, "pinned-close") == OK,
        "create pinned queue");

  std::atomic_uint32_t delete_count{0};
  EventQueue::KernelEqueueEvent event{};
  event.event.ident = 1;
  event.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
  event.filter.data = &delete_count;
  event.filter.delete_event_func = CountDeletedEvent;
  Check(EventQueue::KernelAddEvent(queue, event) == OK, "add callback event");

  auto owner = EventQueue::KernelPinEqueue(queue);
  Check(owner != nullptr, "pin live queue");
  Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete pinned queue");
  Check(delete_count.load(std::memory_order_relaxed) == 1,
        "close invokes callback once");
  Check(!EventQueue::KernelPinEqueue(queue), "deleted queue leaves registry");
  Check(EventQueue::KernelTriggerEvent(queue, 1,
                                       EventQueue::KERNEL_EVFILT_VIDEO_OUT,
                                       nullptr) == KERNEL_ERROR_EBADF,
        "stale trigger rejected");
  Check(EventQueue::KernelDeleteEqueue(queue) == KERNEL_ERROR_EBADF,
        "second queue delete rejected");

  owner.reset();
  Check(delete_count.load(std::memory_order_relaxed) == 1,
        "deferred destruction does not repeat callback");
}

void TestStaleHandleNeverAliasesNewQueue() {
  EventQueue::KernelEqueue stale = EventQueue::KERNEL_EQUEUE_INVALID;
  Check(EventQueue::KernelCreateEqueue(&stale, "stale-handle") == OK,
        "create stale queue");
  Check(EventQueue::KernelDeleteEqueue(stale) == OK, "delete stale queue");

  EventQueue::KernelEqueue replacement = EventQueue::KERNEL_EQUEUE_INVALID;
  Check(EventQueue::KernelCreateEqueue(&replacement, "replacement") == OK,
        "create replacement queue");
  Check(stale != replacement, "queue handles are never recycled");
  Check(!EventQueue::KernelPinEqueue(stale), "stale handle does not pin");
  Check(EventQueue::KernelAddUserEvent(stale, 11) == KERNEL_ERROR_EBADF,
        "stale handle cannot mutate replacement");
  Check(EventQueue::KernelAddUserEvent(replacement, 11) == OK,
        "replacement handle remains valid");
  Check(EventQueue::KernelTriggerUserEvent(stale, 11, nullptr) ==
            KERNEL_ERROR_EBADF,
        "stale handle cannot trigger replacement");
  Check(EventQueue::KernelTriggerUserEvent(replacement, 11, nullptr) == OK,
        "replacement event triggers");
  Check(EventQueue::KernelDeleteEqueue(replacement) == OK,
        "delete replacement queue");
}

void TestConcurrentCloseCallback() {
  for (uint32_t iteration = 0; iteration < 64; iteration++) {
    EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
    Check(EventQueue::KernelCreateEqueue(&queue, "callback-race") == OK,
          "create callback race queue");

    std::atomic_uint32_t delete_count{0};
    EventQueue::KernelEqueueEvent callback_event{};
    callback_event.event.ident = 9;
    callback_event.event.filter = EventQueue::KERNEL_EVFILT_GRAPHICS;
    callback_event.filter.data = &delete_count;
    callback_event.filter.delete_event_func = CountDeletedEvent;
    Check(EventQueue::KernelAddEvent(queue, callback_event) == OK,
          "add callback race event");

    std::atomic_bool start{false};
    std::jthread trigger([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (uint32_t i = 0; i < 256; i++) {
        CheckConcurrentResult(
            EventQueue::KernelTriggerEvent(
                queue, 9, EventQueue::KERNEL_EVFILT_GRAPHICS, nullptr),
            "callback race trigger result");
      }
    });

    start.store(true, std::memory_order_release);
    Check(EventQueue::KernelDeleteEqueue(queue) == OK,
          "callback race queue delete");
    trigger.join();
    Check(delete_count.load(std::memory_order_relaxed) == 1,
          "concurrent close invokes callback exactly once");
  }
}

void TestConcurrentDelete() {
  for (uint32_t iteration = 0; iteration < 64; iteration++) {
    EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
    Check(EventQueue::KernelCreateEqueue(&queue, "concurrent-delete") == OK,
          "create concurrent queue");
    EventQueue::KernelEqueueEvent event{};
    event.event.ident = 7;
    event.event.filter = EventQueue::KERNEL_EVFILT_USER;
    Check(EventQueue::KernelAddEvent(queue, event) == OK,
          "add concurrent event");

    std::atomic_bool start{false};
    std::jthread mutate([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (uint32_t i = 0; i < 64; i++) {
        CheckConcurrentResult(EventQueue::KernelAddEvent(queue, event),
                              "concurrent add result");
        CheckConcurrentResult(
            EventQueue::KernelTriggerEvent(
                queue, 7, EventQueue::KERNEL_EVFILT_USER, nullptr),
            "concurrent trigger result");
        CheckConcurrentResult(EventQueue::KernelDeleteEvent(
                                  queue, 7, EventQueue::KERNEL_EVFILT_USER),
                              "concurrent event delete result");
      }
    });
    std::jthread trigger([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (uint32_t i = 0; i < 128; i++) {
        CheckConcurrentResult(
            EventQueue::KernelTriggerEvent(
                queue, 7, EventQueue::KERNEL_EVFILT_USER, nullptr),
            "parallel trigger result");
      }
    });

    start.store(true, std::memory_order_release);
    Check(EventQueue::KernelDeleteEqueue(queue) == OK,
          "concurrent queue delete");
    mutate.join();
    trigger.join();
    Check(!EventQueue::KernelPinEqueue(queue),
          "concurrent queue removed from registry");
  }
}

} // namespace

int main() {
  TestDuplicateAddPreservesEventState();
  TestCallbackStateOutlivesPort();
  TestCallbackOwnsPayload();
  TestPinnedClose();
  TestStaleHandleNeverAliasesNewQueue();
  TestConcurrentCloseCallback();
  TestConcurrentDelete();
  std::printf("EventQueueLifetimeTests: all cases passed\n");
  return 0;
}
