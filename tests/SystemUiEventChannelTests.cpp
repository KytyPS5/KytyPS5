// Checks the system UI's typed event channel: delivery to every subscriber, the
// bounded queue and what it drops, subscriptions coming and going, channels of
// different types kept apart, and a publisher and a subscriber on two threads
// losing nothing they don't count.
#include "graphics/presentation/system/events/channel.h"
#include "graphics/presentation/system/events/publish.h"
#include "graphics/presentation/system/events/subscribe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Libs::Graphics::SystemUi::Channel;
using Libs::Graphics::SystemUi::EventChannel;
using Libs::Graphics::SystemUi::EventSubscription;

namespace {

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "SystemUiEventChannelTests: failed: %s\n", message);
    std::abort();
  }
}

struct Number {
  int value = 0;
};

struct Word {
  std::string text;
};

std::vector<int> Values(EventSubscription<Number> &subscription) {
  std::vector<Number> taken;
  subscription.DrainInto(taken);
  std::vector<int> values;
  for (const Number &number : taken) {
    values.push_back(number.value);
  }
  return values;
}

// Both subscribers get every event, in order. Delivering to the first alone
// fails the second.
void TestFanOut() {
  EventChannel<Number> channel;
  auto first = channel.Subscribe(8);
  auto second = channel.Subscribe(8);
  channel.Publish({1});
  channel.Publish({2});
  Check(Values(first) == std::vector<int>{1, 2},
        "the first subscriber gets every event in order");
  Check(Values(second) == std::vector<int>{1, 2}, "and so does the second");
  Check(Values(first).empty(), "taking empties the queue");
}

// A queue of 3 given 6 keeps the newest 3 in order and counts 3 dropped.
// Dropping the newest instead would keep 1, 2, 3, and no cap would keep all 6.
void TestCapacity() {
  EventChannel<Number> channel;
  auto subscription = channel.Subscribe(3);
  for (int value = 1; value <= 6; ++value) {
    channel.Publish({value});
  }
  Check(Values(subscription) == std::vector<int>{4, 5, 6},
        "a full queue drops its oldest");
  Check(subscription.Dropped() == 3, "and counts what it dropped");
  auto tiny = channel.Subscribe(0);
  channel.Publish({7});
  channel.Publish({8});
  Check(Values(tiny) == std::vector<int>{8}, "a capacity of 0 still holds one");
}

// Destroying one subscription neither breaks publishing nor stops the others
// receiving, and a moved-from subscription holds nothing. What a destroyed one
// would have received can't be seen, as its queue is gone with it.
void TestUnsubscribe() {
  EventChannel<Number> channel;
  auto kept = channel.Subscribe(8);
  {
    auto gone = channel.Subscribe(8);
    channel.Publish({1});
  }
  channel.Publish({2});
  Check(Values(kept) == std::vector<int>{1, 2},
        "a subscriber outlives one destroyed beside it");
  EventSubscription<Number> moved = std::move(kept);
  channel.Publish({3});
  Check(!kept.Subscribed() && Values(kept).empty(),
        "a moved-from subscription holds nothing");
  Check(Values(moved) == std::vector<int>{3},
        "and the one it moved to receives");
}

// Events published before anyone subscribes are dropped, not saved for the
// first subscriber.
void TestPublishBeforeSubscribe() {
  EventChannel<Number> channel;
  channel.Publish({1});
  auto late = channel.Subscribe(8);
  channel.Publish({2});
  Check(Values(late) == std::vector<int>{2},
        "a subscriber gets only what is published after it");
}

// HasPending goes on with an event and off once taken. Never setting it fails
// the second check, and never clearing it the third.
void TestHasPending() {
  EventChannel<Number> channel;
  auto subscription = channel.Subscribe(8);
  Check(!subscription.HasPending(), "a new subscription has nothing pending");
  channel.Publish({1});
  Check(subscription.HasPending(), "an event makes it pending");
  (void)Values(subscription);
  Check(!subscription.HasPending(), "and taking it clears that");
  Check(!EventSubscription<Number>{}.HasPending(),
        "a default-made subscription is never pending");
}

// The global channels are one per type, so events of one type never reach
// another's subscribers, and asking twice gives the same channel.
void TestGlobalChannelsByType() {
  auto numbers = Libs::Graphics::SystemUi::Subscribe<Number>(8);
  auto words = Libs::Graphics::SystemUi::Subscribe<Word>(8);
  Libs::Graphics::SystemUi::Publish(Number{5});
  Libs::Graphics::SystemUi::Publish(Word{"hello"});
  std::vector<Word> taken;
  words.DrainInto(taken);
  Check(Values(numbers) == std::vector<int>{5},
        "a number reaches only number subscribers");
  Check(taken.size() == 1 && taken[0].text == "hello",
        "and a word only word subscribers");
  Check(&Channel<Number>() == &Channel<Number>(),
        "each type has one global channel");
}

// A publisher and a taker on two threads. Everything published is either taken,
// in order, or counted as dropped.
void TestThreads() {
  constexpr int COUNT = 20000;
  EventChannel<Number> channel;
  auto subscription = channel.Subscribe(64);
  std::atomic<bool> done{false};
  std::thread publisher([&channel, &done] {
    for (int value = 0; value < COUNT; ++value) {
      channel.Publish({value});
    }
    done.store(true, std::memory_order_release);
  });
  // Taken while the publisher runs, then once more after it ends, so the loop
  // ends whatever the channel drops.
  std::vector<Number> taken;
  while (!done.load(std::memory_order_acquire)) {
    subscription.DrainInto(taken);
  }
  publisher.join();
  subscription.DrainInto(taken);
  bool ordered = true;
  for (size_t index = 1; index < taken.size(); ++index) {
    ordered = ordered && taken[index - 1].value < taken[index].value;
  }
  Check(ordered, "events taken across threads keep their order");
  Check(taken.size() + subscription.Dropped() == COUNT,
        "and none is lost without being counted");
}

} // namespace

int main() {
  TestFanOut();
  TestCapacity();
  TestUnsubscribe();
  TestPublishBeforeSubscribe();
  TestHasPending();
  TestGlobalChannelsByType();
  TestThreads();
  std::printf("SystemUiEventChannelTests: passed\n");
  return 0;
}
