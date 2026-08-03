#include "common/hleTrace.h"

#ifdef KYTY_HLE_TRACE

#include "common/threads.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Common::HleTrace {

namespace {

constexpr int      MAX_SLOTS      = 512;
constexpr int      MAX_NAME       = 48;
constexpr int      NOTE_COUNT     = 128;
constexpr int      NOTE_LENGTH    = 192;
constexpr uint32_t DEFAULT_PERIOD = 5000;

// A thread's last call identifies where it is parked, but not what it loops over. HISTOGRAM
// entries record the distinct functions a thread calls, so a poll loop shows its whole body
// rather than whichever call the watchdog happened to catch last.
constexpr int HISTOGRAM = 12;

struct HistogramEntry {
	std::atomic<const char*> module {nullptr};
	std::atomic<const char*> func {nullptr};
	std::atomic<uint64_t>    calls {0};
};

// Aligned to a cache line: every field below is written by the owning thread on every
// instrumented call, and with ~60 guest threads adjacent slots would otherwise false-share and
// cost far more than the measurement is worth.
struct alignas(64) Slot {
	std::atomic<const char*>   module {nullptr};
	std::atomic<const char*>   func {nullptr};
	std::atomic<uint32_t>      depth {0};
	std::atomic<uint64_t>      calls {0};
	std::atomic<int>           unique_id {-1};
	std::atomic<bool>          live {false};
	std::array<char, MAX_NAME> name {};
	std::array<HistogramEntry, HISTOGRAM> histogram {};
};

// The per-thread histogram is capped, so on a thread that calls hundreds of distinct functions
// the rare ones are dropped -- which is precisely where a once-per-frame check would hide. This
// table is global and keyed by the function-name pointer, so it records every function the guest
// calls no matter which thread makes the call or how rarely.
constexpr int      GLOBAL_SLOTS = 8192;
constexpr uintptr_t GLOBAL_MASK = GLOBAL_SLOTS - 1;
constexpr int      GLOBAL_PROBE = 32;

struct GlobalEntry {
	std::atomic<const char*> func {nullptr};
	std::atomic<const char*> module {nullptr};
	std::atomic<uint64_t>    calls {0};
};

std::array<GlobalEntry, GLOBAL_SLOTS> g_global; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void RecordGlobal(const char* module, const char* func) {
	// String literals are unique per function, so the pointer itself is the key. Shift off the
	// low bits that are always zero from alignment before masking.
	const auto hash = reinterpret_cast<uintptr_t>(func) >> 4;
	for (int probe = 0; probe < GLOBAL_PROBE; probe++) {
		auto&       entry = g_global.at((hash + probe) & GLOBAL_MASK);
		const char* known = entry.func.load(std::memory_order_acquire);
		if (known == nullptr) {
			const char* expected = nullptr;
			if (entry.func.compare_exchange_strong(expected, func, std::memory_order_acq_rel)) {
				entry.module.store(module, std::memory_order_relaxed);
				known = func;
			} else {
				known = expected; // another thread claimed this slot first
			}
		}
		if (known == func) {
			entry.calls.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
	// Table saturated at this probe depth. Dropping is preferable to unbounded probing; the
	// watchdog reports the occupancy so saturation is visible rather than silent.
}

// Slots are claimed by index and never released, so the watchdog can read them without
// synchronising against thread exit.
std::array<Slot, MAX_SLOTS> g_slots;      // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int>            g_slot_count {0};

// Ordered call log. The per-thread breadcrumb says where a thread is parked and the histogram says
// what it loops over, but neither gives a *sequence* -- and some questions are only answerable in
// order ("what did this thread call between creating an audio port and destroying it?").
//
// Append-only and bounded: recording stops when the buffer is full, so what it captures is the
// first N calls of the run, which is exactly the startup window. Off unless
// $KYTY_HLE_TRACE_ORDER is set, because it costs an atomic increment on every HLE call.
constexpr int ORDER_CAPACITY = 1 << 21;

struct OrderEntry {
	const char* module;
	const char* func;
	int32_t     unique_id;
};

// Intentionally heap-allocated on first use rather than a 48 MB BSS array in every trace build.
OrderEntry*           g_order = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int>      g_order_count {0};
std::atomic<bool>     g_order_enabled {false};
std::atomic<bool>     g_order_dumped {false};

void RecordOrder(const char* module, const char* func, int32_t unique_id) {
	if (!g_order_enabled.load(std::memory_order_relaxed) || g_order == nullptr) {
		return;
	}
	const int index = g_order_count.fetch_add(1, std::memory_order_relaxed);
	if (index >= ORDER_CAPACITY) {
		return;
	}
	g_order[index] = OrderEntry {module, func, unique_id};
}

void DumpOrder() {
	if (!g_order_enabled.load(std::memory_order_relaxed) || g_order == nullptr) {
		return;
	}
	bool expected = false;
	if (!g_order_dumped.compare_exchange_strong(expected, true)) {
		return;
	}
	const char* path = ::getenv("KYTY_HLE_TRACE_ORDER_FILE");
	if (path == nullptr) {
		path = "kyty_hle_order.log";
	}
	auto* file = ::fopen(path, "w"); // NOLINT(cppcoreguidelines-owning-memory)
	if (file == nullptr) {
		return;
	}
	const int total = std::min(g_order_count.load(std::memory_order_relaxed), ORDER_CAPACITY);
	::fprintf(file, "ordered HLE calls: %d entries%s\n", total,
	          total >= ORDER_CAPACITY ? " (buffer full, truncated)" : "");
	for (int i = 0; i < total; i++) {
		const auto& entry = g_order[i];
		::fprintf(file, "%d\t#%d\t%s::%s\n", i, entry.unique_id,
		          entry.module != nullptr ? entry.module : "-",
		          entry.func != nullptr ? entry.func : "-");
	}
	::fclose(file);
}

std::array<std::array<char, NOTE_LENGTH>, NOTE_COUNT> g_notes; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<uint64_t>                                 g_note_seq {0};

std::atomic<bool> g_started {false};

thread_local Slot* g_slot = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

Slot* Self() {
	if (g_slot != nullptr) {
		return g_slot;
	}
	const int index = g_slot_count.fetch_add(1, std::memory_order_relaxed);
	if (index >= MAX_SLOTS) {
		return nullptr;
	}
	auto& slot = g_slots.at(index);
	slot.unique_id.store(Common::Thread::GetThreadIdUnique(), std::memory_order_relaxed);
	slot.live.store(true, std::memory_order_release);
	g_slot = &slot;
	return g_slot;
}

const char* OrDash(const char* value) {
	return value != nullptr ? value : "-";
}

void WatchdogFunc(void* /*arg*/) {
	const char* path = ::getenv("KYTY_HLE_TRACE_FILE"); // NOLINT(concurrency-mt-unsafe)
	if (path == nullptr) {
		path = "kyty_hle_trace.log";
	}
	uint32_t    period      = DEFAULT_PERIOD;
	const char* period_text = ::getenv("KYTY_HLE_TRACE_PERIOD_MS"); // NOLINT(concurrency-mt-unsafe)
	if (period_text != nullptr) {
		const auto parsed = ::strtoul(period_text, nullptr, 10);
		if (parsed >= 100 && parsed <= 600000) {
			period = static_cast<uint32_t>(parsed);
		}
	}

	auto* file = ::fopen(path, "w"); // NOLINT(cppcoreguidelines-owning-memory)
	if (file == nullptr) {
		return;
	}
	::fprintf(file, "kyty HLE trace, snapshot every %u ms\n", period);
	::fflush(file);

	const auto                      start = std::chrono::steady_clock::now();
	static std::array<uint64_t, MAX_SLOTS> previous {};
	static std::array<std::array<uint64_t, HISTOGRAM>, MAX_SLOTS> previous_histogram {};
	static std::array<uint64_t, GLOBAL_SLOTS>                     previous_global {};
	std::array<int, MAX_SLOTS>      order {};
	uint64_t                        notes_seen = 0;

	for (;;) {
		Common::Thread::Sleep(period);

		const auto elapsed =
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		const int count = std::min(g_slot_count.load(std::memory_order_relaxed), MAX_SLOTS);

		// The ordered log covers the startup window; dump it once it can no longer be growing
		// usefully, either because the buffer filled or because startup is well past.
		if (elapsed >= 15.0 || g_order_count.load(std::memory_order_relaxed) >= ORDER_CAPACITY) {
			DumpOrder();
		}

		int live = 0;
		for (int i = 0; i < count; i++) {
			if (g_slots.at(i).live.load(std::memory_order_acquire)) {
				order.at(live++) = i;
			}
		}

		// Busiest first: a spinning thread is the one worth seeing at the top.
		std::sort(order.begin(), order.begin() + live, [](int a, int b) {
			const auto da = g_slots.at(a).calls.load(std::memory_order_relaxed) - previous.at(a);
			const auto db = g_slots.at(b).calls.load(std::memory_order_relaxed) - previous.at(b);
			return da > db;
		});

		::fprintf(file, "\n===== t=+%.1fs  threads=%d =====\n", elapsed, live);
		for (int i = 0; i < live; i++) {
			auto&      slot  = g_slots.at(order.at(i));
			const auto calls = slot.calls.load(std::memory_order_relaxed);
			const auto delta = calls - previous.at(order.at(i));
			previous.at(order.at(i)) = calls;
			const bool inside        = slot.depth.load(std::memory_order_relaxed) > 0;
			::fprintf(file, "  #%-4d %-28s %s %s::%s  calls=+%llu total=%llu\n",
			          slot.unique_id.load(std::memory_order_relaxed), slot.name.data(),
			          inside ? "IN  " : "last", OrDash(slot.module.load(std::memory_order_relaxed)),
			          OrDash(slot.func.load(std::memory_order_relaxed)),
			          static_cast<unsigned long long>(delta),
			          static_cast<unsigned long long>(calls));

			// The loop body of an active thread is the interesting part; parked threads have
			// nothing to show.
			if (delta == 0) {
				continue;
			}
			auto& seen = previous_histogram.at(order.at(i));
			std::array<int, HISTOGRAM> ranked {};
			int                        ranked_count = 0;
			for (int h = 0; h < HISTOGRAM; h++) {
				if (slot.histogram.at(h).func.load(std::memory_order_acquire) != nullptr) {
					ranked.at(ranked_count++) = h;
				}
			}
			std::sort(ranked.begin(), ranked.begin() + ranked_count, [&slot, &seen](int a, int b) {
				const auto da = slot.histogram.at(a).calls.load(std::memory_order_relaxed) - seen.at(a);
				const auto db = slot.histogram.at(b).calls.load(std::memory_order_relaxed) - seen.at(b);
				return da > db;
			});
			for (int h = 0; h < std::min(ranked_count, 5); h++) {
				auto&      entry = slot.histogram.at(ranked.at(h));
				const auto total = entry.calls.load(std::memory_order_relaxed);
				const auto step  = total - seen.at(ranked.at(h));
				seen.at(ranked.at(h)) = total;
				if (step == 0) {
					break;
				}
				::fprintf(file, "        %s::%s  +%llu\n",
				          OrDash(entry.module.load(std::memory_order_relaxed)),
				          OrDash(entry.func.load(std::memory_order_relaxed)),
				          static_cast<unsigned long long>(step));
			}
			for (int h = 0; h < ranked_count; h++) {
				seen.at(ranked.at(h)) =
				    slot.histogram.at(ranked.at(h)).calls.load(std::memory_order_relaxed);
			}
		}

		// Complete inventory of what the guest called this interval, across all threads. Rare
		// calls survive here even when a busy thread's own histogram has dropped them.
		{
			std::array<int, GLOBAL_SLOTS> hot {};
			int                           hot_count = 0;
			int                           occupied  = 0;
			for (int g = 0; g < GLOBAL_SLOTS; g++) {
				if (g_global.at(g).func.load(std::memory_order_acquire) == nullptr) {
					continue;
				}
				occupied++;
				if (g_global.at(g).calls.load(std::memory_order_relaxed) != previous_global.at(g)) {
					hot.at(hot_count++) = g;
				}
			}
			std::sort(hot.begin(), hot.begin() + hot_count, [](int a, int b) {
				const auto da =
				    g_global.at(a).calls.load(std::memory_order_relaxed) - previous_global.at(a);
				const auto db =
				    g_global.at(b).calls.load(std::memory_order_relaxed) - previous_global.at(b);
				return da > db;
			});
			::fprintf(file, "  -- all calls this interval: %d distinct (%d known total) --\n",
			          hot_count, occupied);
			for (int h = 0; h < hot_count; h++) {
				auto&      entry = g_global.at(hot.at(h));
				const auto total = entry.calls.load(std::memory_order_relaxed);
				::fprintf(file, "     %s::%s  +%llu\n",
				          OrDash(entry.module.load(std::memory_order_relaxed)),
				          OrDash(entry.func.load(std::memory_order_relaxed)),
				          static_cast<unsigned long long>(total - previous_global.at(hot.at(h))));
				previous_global.at(hot.at(h)) = total;
			}
		}

		const auto seq = g_note_seq.load(std::memory_order_acquire);
		if (seq > notes_seen) {
			const auto first = std::max(notes_seen, seq >= NOTE_COUNT ? seq - NOTE_COUNT : 0);
			::fprintf(file, "  -- events %llu..%llu --\n",
			          static_cast<unsigned long long>(first),
			          static_cast<unsigned long long>(seq - 1));
			for (auto n = first; n < seq; n++) {
				::fprintf(file, "     %s\n", g_notes.at(n % NOTE_COUNT).data());
			}
			notes_seen = seq;
		}

		::fflush(file);
	}
}

} // namespace

void Enter(const char* module, const char* func) {
	auto* slot = Self();
	if (slot == nullptr) {
		return;
	}
	// depth and calls have exactly one writer -- the owning thread -- so a plain load/store pair
	// is correct and avoids a locked read-modify-write on every HLE call. The watchdog is the
	// only reader and tolerates a torn view.
	const auto depth = slot->depth.load(std::memory_order_relaxed);
	if (depth == 0) {
		slot->module.store(module, std::memory_order_relaxed);
		slot->func.store(func, std::memory_order_relaxed);
	}
	slot->depth.store(depth + 1, std::memory_order_relaxed);
	slot->calls.store(slot->calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

	RecordGlobal(module, func);
	RecordOrder(module, func, slot->unique_id.load(std::memory_order_relaxed));

	// Only the owning thread writes here, so a linear scan over a dozen pointer comparisons is
	// both correct and cheaper than anything cleverer. Functions past the twelfth distinct one
	// are dropped rather than evicted; a poll loop has far fewer than that.
	for (auto& entry: slot->histogram) {
		const auto* known = entry.func.load(std::memory_order_relaxed);
		if (known == func) {
			entry.calls.store(entry.calls.load(std::memory_order_relaxed) + 1,
			                  std::memory_order_relaxed);
			return;
		}
		if (known == nullptr) {
			entry.module.store(module, std::memory_order_relaxed);
			entry.calls.store(1, std::memory_order_relaxed);
			entry.func.store(func, std::memory_order_release);
			return;
		}
	}
}

void Leave() {
	// Deliberately does not clear module/func: once depth drops to zero they become the "last
	// call" breadcrumb, which is what distinguishes an idle thread from a blocked one.
	if (g_slot != nullptr) {
		g_slot->depth.fetch_sub(1, std::memory_order_relaxed);
	}
}

void Note(const char* category, const char* text) {
	const auto index = g_note_seq.fetch_add(1, std::memory_order_relaxed);
	auto&      entry = g_notes.at(index % NOTE_COUNT);
	::snprintf(entry.data(), NOTE_LENGTH, "[%s] %s", category, text != nullptr ? text : "");
	// Publish after the text is written so the watchdog never reads a half-filled entry.
	g_note_seq.store(index + 1, std::memory_order_release);
}

void NameCurrentThread(const char* name) {
	auto* slot = Self();
	if (slot == nullptr || name == nullptr) {
		return;
	}
	::snprintf(slot->name.data(), MAX_NAME, "%s", name);
}

void SetThreadName(int unique_id, const char* name) {
	if (name == nullptr) {
		return;
	}
	const int count = std::min(g_slot_count.load(std::memory_order_relaxed), MAX_SLOTS);
	for (int i = 0; i < count; i++) {
		auto& slot = g_slots.at(i);
		if (slot.live.load(std::memory_order_acquire) &&
		    slot.unique_id.load(std::memory_order_relaxed) == unique_id) {
			::snprintf(slot.name.data(), MAX_NAME, "%s", name);
			return;
		}
	}
}

void Start() {
	bool expected = false;
	if (!g_started.compare_exchange_strong(expected, true)) {
		return;
	}
	if (::getenv("KYTY_HLE_TRACE_ORDER") != nullptr) {
		g_order = new OrderEntry[ORDER_CAPACITY](); // NOLINT(cppcoreguidelines-owning-memory)
		g_order_enabled.store(true, std::memory_order_release);
	}
	// Intentionally leaked: the emulator exits with std::quick_exit(), so there is nothing to
	// join and no destructor to run.
	auto* watchdog = new Common::Thread(WatchdogFunc, nullptr); // NOLINT
	watchdog->Detach();
}

} // namespace Common::HleTrace

#endif // KYTY_HLE_TRACE
