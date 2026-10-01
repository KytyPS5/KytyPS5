#include "common/inlineFunction.h"
#include "common/spscQueue.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/hostCopyQueue.h"
#include "graphics/host_gpu/renderer/occlusionQueries.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace {

using Libs::Graphics::CommandChunkQueue;
using Libs::Graphics::CommandStream;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "GpuPipelineTests: failed: %s\n", text);
		std::abort();
	}
}

std::vector<uint64_t> g_log;

// Busy-waits longer than the spin budget, so the other side falls back to its kernel wait.
void Stall(std::chrono::nanoseconds duration) {
	const auto end = std::chrono::steady_clock::now() + duration;
	while (std::chrono::steady_clock::now() < end) {
		Common::SpinPause();
	}
}

struct SubmitPayload {
	uint64_t tick = 0;
};

vk::CommandBuffer NoCommandBuffer() {
	return nullptr;
}

void TestCommandStreamOrder() {
	CommandStream stream;
	for (uint64_t i = 0; i < 10; i++) {
		stream.Record([i](vk::CommandBuffer) { g_log.push_back(i); });
	}
	stream.RecordSpecial<SubmitPayload>(CommandStream::RecordKind::Submit)->tick = 77;
	stream.Record([](vk::CommandBuffer) { g_log.push_back(100); });
	Check(stream.CommandCount() == 11, "command count");

	auto chunks = stream.TakeChunks();
	Check(chunks.size() == 1, "small stream fits one chunk");

	g_log.clear();
	uint64_t submits = 0;
	for (const auto& chunk: chunks) {
		CommandStream::Replay(*chunk, NoCommandBuffer, [&](const CommandStream::Entry& entry) {
			Check(entry.kind == CommandStream::RecordKind::Submit, "special kind");
			const auto* payload = static_cast<const SubmitPayload*>(entry.Payload());
			Check(payload->tick == 77, "submit payload");
			g_log.push_back(1000 + payload->tick);
			submits++;
		});
	}
	Check(submits == 1, "one submit");
	// The submit was recorded after command 9.
	const std::vector<uint64_t> expected_order {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 1077, 100};
	Check(g_log == expected_order, "replay preserves record order");
}

void TestCommandStreamArraysStayInChunk() {
	CommandStream stream;
	// Fill most of a chunk, then record commands whose arrays would not fit: the stream must
	// move the arrays and the command to a new chunk together.
	std::vector<uint32_t> big(CommandStream::ChunkCapacity / sizeof(uint32_t) / 2 + 100, 7u);
	uint64_t              sum = 0;
	for (int round = 0; round < 8; round++) {
		const auto count = static_cast<uint32_t>(big.size() - round);
		stream.Reserve(CommandStream::RoundUp(sizeof(uint32_t) * count) + 128);
		const auto* copy = stream.Copy(big.data(), count);
		stream.Record([copy, count, &sum](vk::CommandBuffer) {
			for (uint32_t i = 0; i < count; i++) {
				sum += copy[i];
			}
		});
	}
	auto chunks = stream.TakeChunks();
	Check(chunks.size() >= 4, "large arrays roll over to new chunks");
	for (auto& chunk: chunks) {
		CommandStream::Replay(*chunk, NoCommandBuffer, [](const CommandStream::Entry&) {
			Check(false, "no special records");
		});
		stream.Recycle(std::move(chunk));
	}
	uint64_t expected = 0;
	for (int round = 0; round < 8; round++) {
		expected += uint64_t {7} * (big.size() - round);
	}
	Check(sum == expected, "arrays are intact after rollover");

	// Oversized single commands get a chunk of their own.
	std::vector<uint32_t> huge(CommandStream::ChunkCapacity / sizeof(uint32_t) * 2, 3u);
	stream.Reserve(CommandStream::RoundUp(sizeof(uint32_t) * huge.size()) + 128);
	const auto* copy  = stream.Copy(huge.data(), huge.size());
	const auto  count = huge.size();
	uint64_t    total = 0;
	stream.Record([copy, count, &total](vk::CommandBuffer) {
		for (size_t i = 0; i < count; i++) {
			total += copy[i];
		}
	});
	chunks = stream.TakeChunks();
	Check(chunks.size() == 1 && chunks[0]->capacity > CommandStream::ChunkCapacity,
	      "oversized chunk");
	CommandStream::Replay(*chunks[0], NoCommandBuffer, [](const CommandStream::Entry&) {});
	Check(total == uint64_t {3} * huge.size(), "oversized command replays");
}

// A producer records commands with arrays and submits into a queue-backed stream while a
// consumer replays and recycles the chunks, as the Vulkan recording thread does.
void TestCommandChunkQueueThreads() {
	constexpr uint64_t Commands = 200000;
	CommandChunkQueue  queue;
	uint64_t           replayed_sum     = 0;
	uint64_t           replayed_count   = 0;
	std::atomic_uint64_t submits {0};
	uint64_t           last_submit_tick = 0;
	bool               ordered          = true;
	uint64_t           next_expected    = 0;

	std::jthread consumer([&] {
		for (;;) {
			auto chunk = queue.Pop();
			if (chunk == nullptr) {
				return;
			}
			// The callables write through pointers into consumer-owned state.
			CommandStream::Replay(*chunk, NoCommandBuffer, [&](const CommandStream::Entry& entry) {
				const auto* payload = static_cast<const SubmitPayload*>(entry.Payload());
				ordered &= payload->tick == last_submit_tick + 1;
				last_submit_tick = payload->tick;
				submits.fetch_add(1, std::memory_order_release);
			});
			queue.Recycle(std::move(chunk));
		}
	});

	struct Sink {
		uint64_t* sum;
		uint64_t* count;
		uint64_t* next;
		bool*     ordered;
	};
	const Sink sink {&replayed_sum, &replayed_count, &next_expected, &ordered};

	uint64_t expected_sum = 0;
	uint64_t tick         = 0;
	{
		CommandStream stream(&queue);
		for (uint64_t i = 0; i < Commands; i++) {
			const auto words = static_cast<uint32_t>(1 + i % 61);
			uint32_t   data[61];
			for (uint32_t w = 0; w < words; w++) {
				data[w] = static_cast<uint32_t>(i + w);
				expected_sum += data[w];
			}
			stream.Reserve(CommandStream::RoundUp(sizeof(uint32_t) * words) + 128);
			const auto* copy = stream.Copy(data, words);
			stream.Record([sink, copy, words, i](vk::CommandBuffer) {
				*sink.ordered &= *sink.next == i;
				*sink.next = i + 1;
				for (uint32_t w = 0; w < words; w++) {
					*sink.sum += copy[w];
				}
				(*sink.count)++;
			});
			if (i % 997 == 996) {
				stream.RecordSpecial<SubmitPayload>(CommandStream::RecordKind::Submit)->tick =
				    ++tick;
				stream.Publish();
			}
		}
		stream.RecordSpecial<SubmitPayload>(CommandStream::RecordKind::Submit)->tick = ++tick;
		stream.Publish();
	}
	// The last record is a submit: once it is replayed, so is everything before it.
	while (submits.load(std::memory_order_acquire) != tick) {
		Common::SpinPause();
	}
	queue.Stop();
	consumer.join();

	Check(ordered, "the consumer replays commands and submits in record order");
	Check(replayed_count == Commands, "every command is replayed once");
	Check(replayed_sum == expected_sum, "array contents survive the handoff");
	Check(submits == tick && last_submit_tick == tick, "every submit is replayed once");
}

// Move-only elements pass through a small queue in order while both sides sleep and wake.
void TestSpscQueue() {
	constexpr uint64_t              Count = 1000000;
	Common::SpscQueue<std::unique_ptr<uint64_t>> queue(8);
	Check(queue.Capacity() == 8, "capacity is a power of two");
	uint64_t     received = 0;
	bool         ordered  = true;
	std::jthread consumer([&] {
		for (;;) {
			auto value = queue.Pop();
			if (!value) {
				return;
			}
			ordered &= **value == received;
			received++;
			// Let the producer fill the queue now and then.
			if (received % 100000 == 0) {
				Stall(std::chrono::milliseconds(3));
			}
		}
	});
	for (uint64_t i = 0; i < Count; i++) {
		queue.Push(std::make_unique<uint64_t>(i));
		// Let the consumer sleep on an empty queue now and then.
		if (i % 250000 == 0) {
			Stall(std::chrono::milliseconds(3));
		}
	}
	queue.Stop();
	consumer.join();
	Check(ordered && received == Count, "every element arrives once, in order");

	// Elements left in a stopped queue are still popped, then destroyed with the queue.
	Common::SpscQueue<std::unique_ptr<uint64_t>> leftover(4);
	leftover.Push(std::make_unique<uint64_t>(1));
	leftover.Push(std::make_unique<uint64_t>(2));
	leftover.Stop();
	auto first = leftover.Pop();
	Check(first && **first == 1, "a stopped queue drains before it reports the stop");
}

// A waiter sees the writes made before the counter reached its target.
void TestProgressCounter() {
	constexpr uint64_t     Rounds = 200000;
	Common::ProgressCounter counter;
	std::vector<uint64_t>  data(Rounds + 1, 0);
	std::jthread           advancer([&] {
		for (uint64_t i = 1; i <= Rounds; i++) {
			data[i] = i * 3;
			counter.Advance(i);
			if (i % 50000 == 0) {
				Stall(std::chrono::milliseconds(3));
			}
		}
	});
	bool visible = true;
	for (uint64_t target = 1; target <= Rounds; target += 7) {
		counter.WaitFor(target);
		visible &= data[target] == target * 3;
	}
	counter.WaitFor(Rounds);
	advancer.join();
	Check(visible, "writes before Advance() are visible after WaitFor()");
	Check(counter.Load() == Rounds, "final value");

	// Published values wake a waiter at the next Notify(), as the execution thread does.
	Common::ProgressCounter lazy;
	std::jthread            publisher([&] {
		for (uint64_t i = 1; i <= Rounds; i++) {
			lazy.Publish(i);
			if (i % 16 == 0) {
				lazy.Notify();
			}
			if (i % 40000 == 0) {
				Stall(std::chrono::milliseconds(3));
			}
		}
		lazy.Notify();
	});
	for (uint64_t target = 1; target <= Rounds; target += 997) {
		lazy.WaitFor(target);
	}
	lazy.WaitFor(Rounds);
	publisher.join();
	Check(lazy.Load() == Rounds, "published final value");

	// A waiter that wakes and finds its target reached leaves it registered until the next
	// Notify(); its next wait finds that target and does not register its own. A Notify() at the
	// value the waiter read (the execution thread notifies again at one value when an urgent
	// command interrupts its wait) takes the registration out: the waiter must still wake when
	// its own target is reached.
	Common::ProgressCounter stale;
	std::atomic_bool        reached {false};
	stale.Advance(1);
	std::jthread waiter([&] {
		stale.WaitFor(2);
		stale.WaitFor(3);
		reached = true;
	});
	Stall(std::chrono::milliseconds(50));
	stale.Advance(2);
	Stall(std::chrono::milliseconds(50));
	stale.Notify();
	Stall(std::chrono::milliseconds(50));
	stale.Advance(3);
	for (int i = 0; i < 200 && !reached; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	Check(reached, "a Notify() at the value a waiter read does not lose its wake-up");
}

// Small callables live in place, large ones on the heap; both move and destroy once.
void TestInlineFunction() {
	static int destroyed = 0;
	struct Probe {
		int* calls;
		Probe(int* c): calls(c) {}
		Probe(Probe&& other) noexcept: calls(other.calls) { other.calls = nullptr; }
		~Probe() {
			if (calls != nullptr) {
				destroyed++;
			}
		}
		void operator()() const { (*calls)++; }
	};
	int calls = 0;
	{
		Common::InlineFunction<32> small(Probe {&calls});
		auto moved = std::move(small);
		Check(!small && moved, "moving empties the source");
		moved();
		std::array<uint64_t, 16> big {};
		big[15] = 7;
		Common::InlineFunction<32> large([big, &calls] { calls += static_cast<int>(big[15]); });
		Common::InlineFunction<32> large_moved;
		large_moved = std::move(large);
		large_moved();
	}
	Check(calls == 8, "calls reach the callables");
	Check(destroyed == 1, "the small callable is destroyed once");
}

// Copies complete in order, in full and partial batches; WaitFor() and Drain() see their bytes,
// also after the copy thread slept on an empty queue, and a waiter on another thread (the
// recording thread) sees the copies counted before its target.
void TestHostCopyQueue() {
	constexpr size_t           Copies = 20000;
	constexpr size_t           Bytes  = 300;
	std::vector<uint8_t>       sources(Copies * Bytes);
	std::vector<uint8_t>       targets(Copies * Bytes, 0);
	for (size_t i = 0; i < sources.size(); i++) {
		sources[i] = static_cast<uint8_t>(i * 7 + i / 251);
	}
	Libs::Graphics::HostCopyQueue queue;
	std::atomic_uint64_t          published {0};
	std::atomic_bool              stop {false};
	bool                          seen = true;
	std::jthread                  waiter([&] {
		// Like the recording thread: waits for the copies counted before a submission.
		while (!stop.load(std::memory_order_acquire)) {
			const auto target = published.load(std::memory_order_acquire);
			queue.WaitFor(target);
			for (uint64_t i = target >= 8 ? target - 8 : 0; i < target; i++) {
				seen &= targets[i * Bytes + Bytes - 1] == sources[i * Bytes + Bytes - 1];
			}
		}
	});
	bool drained = true;
	for (size_t i = 0; i < Copies; i++) {
		queue.Copy(targets.data() + i * Bytes, sources.data() + i * Bytes, Bytes);
		// Flushes every few copies, as the execution thread does after each draw; Copy() hands
		// over full batches itself.
		if (i % 3 == 0) {
			published.store(queue.Flush(), std::memory_order_release);
		}
		if (i % 997 == 0) {
			queue.Drain();
			drained &= std::memcmp(targets.data(), sources.data(), (i + 1) * Bytes) == 0;
		}
		if (i % 5000 == 0) {
			Stall(std::chrono::milliseconds(3));
		}
	}
	queue.Drain();
	stop.store(true, std::memory_order_release);
	waiter.join();
	Check(queue.Flush() == Copies, "every copy is counted");
	Check(drained, "Drain() waits for every queued copy");
	Check(seen, "WaitFor() on another thread sees the copies before its target");
	Check(targets == sources, "every copy lands");
}

} // namespace


namespace Occlusion = Libs::Graphics::OcclusionCounters;

// Plays the role of OcclusionQueries over a fake GPU: each allocated slot counts `samples`, and
// dumps publish in order once "completed".
struct OcclusionModel {
	explicit OcclusionModel(uint32_t capacity): segments(capacity) {}

	Occlusion::Segments                   segments;
	std::map<uint64_t, uint64_t>          memory;
	std::map<uint32_t, uint64_t>          slot_samples;
	std::vector<Occlusion::Segments::Dump> pending;
	uint64_t                              total = 0;

	// Draws that pass `samples` samples, recorded inside one render pass instance.
	void Draw(uint64_t samples) {
		if (!segments.Counting()) {
			return;
		}
		uint32_t slot = 0;
		if (segments.AllocateSlot(slot)) {
			slot_samples[slot] = samples;
		}
	}
	void Dump(uint64_t address) { pending.push_back(segments.OnDump(address)); }
	void Complete() {
		for (const auto& dump: pending) {
			for (uint64_t index = dump.first_slot; index < dump.end_slot; index++) {
				total += slot_samples[static_cast<uint32_t>(index % segments.Capacity())];
			}
			if (dump.uncounted) {
				total += Occlusion::UncountedSamples;
			}
			segments.Release(dump.end_slot);
			for (uint32_t db = 0; db < Occlusion::DbCount; db++) {
				memory[dump.address + db * Occlusion::DbStride] = Occlusion::DbValue(db, total);
			}
		}
		pending.clear();
	}
	// What SET_PREDICATION and the guest compute: the sum of end - begin over the DBs, if ready.
	bool Read(uint64_t address, uint64_t& samples) {
		samples = 0;
		for (uint32_t db = 0; db < Occlusion::DbCount; db++) {
			const auto begin = memory[address + db * Occlusion::DbStride];
			const auto end   = memory[address + db * Occlusion::DbStride + 8u];
			if ((begin & end & Occlusion::ReadyBit) == 0) {
				return false;
			}
			samples += end - begin;
		}
		return true;
	}
};

void TestOcclusionCounters() {
	Check(Occlusion::DbValue(0, 5) == (Occlusion::ReadyBit | 5u), "DB 0 holds the count");
	Check(Occlusion::DbValue(3, 5) == Occlusion::ReadyBit, "other DBs stay zero");
	Check(Occlusion::DumpSize == 15u * 16u + 8u, "dump size");

	// Draws outside queries are not counted; a query reads the samples between its dumps.
	OcclusionModel model(64);
	model.Draw(1000);
	Check(!model.segments.Counting(), "no query open");
	model.Dump(0x1000);
	Check(model.segments.Counting(), "begin opens a query");
	model.Draw(7);
	model.Draw(3);
	model.Dump(0x1008);
	Check(!model.segments.Counting(), "end closes it");
	uint64_t samples = 0;
	Check(!model.Read(0x1000, samples), "not ready before the GPU finished");
	model.Complete();
	Check(model.Read(0x1000, samples) && samples == 10, "query counts its draws");

	// An occluded query reads zero.
	model.Dump(0x2000);
	model.Draw(0);
	model.Dump(0x2008);
	model.Complete();
	Check(model.Read(0x2000, samples) && samples == 0, "occluded query");

	// Nested and interleaved queries share the running counter.
	model.Dump(0x3000);
	model.Draw(5);
	model.Dump(0x4000);
	model.Draw(11);
	model.Dump(0x3008);
	model.Draw(13);
	model.Dump(0x4008);
	model.Complete();
	Check(model.Read(0x3000, samples) && samples == 16, "outer query");
	Check(model.Read(0x4000, samples) && samples == 24, "interleaved query");
	Check(!model.segments.Counting(), "all queries closed");

	// A full slot ring makes the segment read visible instead of occluded.
	OcclusionModel small(2);
	small.Dump(0x5000);
	small.Draw(0);
	small.Draw(0);
	small.Draw(0);
	small.Dump(0x5008);
	small.Complete();
	Check(small.Read(0x5000, samples) && samples >= Occlusion::UncountedSamples,
	      "uncounted samples read visible");
	// Released slots are allocated again.
	small.Dump(0x6000);
	small.Draw(4);
	small.Draw(2);
	small.Dump(0x6008);
	small.Complete();
	Check(small.Read(0x6000, samples) && samples == 6, "slots reused after release");

	// A begin without its end stops keeping the counters running after a few frames.
	Occlusion::Segments ageing(16, 2);
	(void)ageing.OnDump(0x7000);
	Check(ageing.Counting(), "open");
	ageing.OnFrame();
	ageing.OnFrame();
	Check(ageing.Counting(), "still open within the limit");
	ageing.OnFrame();
	Check(!ageing.Counting(), "dropped after the limit");
}

int main() {
	TestCommandStreamOrder();
	TestCommandStreamArraysStayInChunk();
	TestCommandChunkQueueThreads();
	TestSpscQueue();
	TestProgressCounter();
	TestInlineFunction();
	TestHostCopyQueue();
	TestOcclusionCounters();
	std::printf("GpuPipelineTests: all tests passed\n");
	return 0;
}
