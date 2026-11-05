// Tests for ReadbackGuard with real page protection and no GPU. A hold's whole purpose is that a
// real guest access stops and waits, so these check host protection and real faults.
#include "common/hostException.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/cache/readbackGuard.h"
#include "kernel/memory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Libs::Graphics::GuestRange;
using Libs::Graphics::PageManager;
using Libs::Graphics::ReadbackGuard;
using Libs::Graphics::ReadbackShape;
using Libs::Graphics::TRACKER_PAGE_SIZE;
using Mode       = Common::VirtualMemory::Mode;
using Clock      = std::chrono::steady_clock;
using Limits     = ReadbackGuard::Limits;
using WaitResult = ReadbackGuard::WaitResult;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

constexpr uint64_t PAGE = TRACKER_PAGE_SIZE;
constexpr auto     LONG = seconds(60);
// Well below LONG, so a wake-up that relies on the slice running out fails this check.
constexpr auto PROMPT = seconds(30);

constexpr uint32_t STOP_AFTER = 2;
// A waiter on these can only end through a notification, since the slice and both limits outlast
// the test timeout.
constexpr Limits NOTIFY_ONLY {LONG, LONG, LONG, STOP_AFTER};
// Time out quickly without progress. The absolute limit is far away, so reaching it would show the
// no-progress limit didn't work.
constexpr Limits NO_PROGRESS_ONLY {milliseconds(2), milliseconds(30), LONG, STOP_AFTER};
// Time out on the absolute limit, for sources that always report progress.
constexpr Limits ABSOLUTE_ONLY {milliseconds(2), milliseconds(30), milliseconds(200), STOP_AFTER};

constexpr uint64_t AREA_STRIDE      = TRACKER_PAGE_SIZE * 1024;
constexpr uint32_t ALLOCATION_TRIES = 4096;

std::atomic<uint64_t>       g_protect_calls {0};
std::atomic<ReadbackGuard*> g_order_guard {nullptr};
std::atomic<bool>           g_blocked_before_pending {false};
uint64_t                    g_next_base = 0x0000000300000000ull;

/// Fails the whole run at once, because a broken guard can leave later cases hanging.
void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ReadbackGuardTests failed: %s\n", text);
		std::_Exit(1);
	}
}

/// A progress source that reports when a wait has really started. The guard calls it with its mutex
/// held after confirming an overlap, and keeps the mutex until it is waiting on its condition, so a
/// Release after this can't be missed.
struct Probe {
	std::atomic<uint32_t> entered {0};
	std::atomic<uint64_t> value {0};
	std::atomic<bool>     advancing {false};
	std::atomic<int64_t>  advances_left {-1};

	[[nodiscard]] std::function<uint64_t()> Source() {
		return [this] {
			entered++;
			if (advancing.load()) {
				value++;
			} else if (advances_left.load() > 0) {
				advances_left--;
				value++;
			}
			return value.load();
		};
	}
	void AwaitEntered(uint32_t count = 1) const {
		while (entered.load() < count) {
			std::this_thread::yield();
		}
	}
};

/// A fresh writable area, so tests can't see each other's page state.
struct Pages {
	explicit Pages(uint64_t count): address(0), size(count * PAGE) {
		for (uint32_t attempt = 0; attempt < ALLOCATION_TRIES && address == 0; attempt++) {
			const auto candidate = g_next_base;
			g_next_base += AREA_STRIDE;
			if (Common::VirtualMemory::AllocFixed(candidate, size, Mode::ReadWrite)) {
				address = candidate;
			}
		}
		Check(address != 0, "no free fixed address found");
	}
	~Pages() { Common::VirtualMemory::Free(address, size); }

	[[nodiscard]] GuestRange Range(uint64_t first, uint64_t count) const {
		return {address + first * PAGE, count * PAGE};
	}
	[[nodiscard]] uint64_t At(uint64_t page) const { return address + page * PAGE; }

	uint64_t address;
	uint64_t size;
};

void WritePage(uint64_t address, uint32_t value) {
	*reinterpret_cast<volatile uint32_t*>(address) = value;
}

/// Runs a guard wait on another thread once it has really started waiting.
class Waiter {
public:
	Waiter(ReadbackGuard& guard, Probe& probe, uint64_t address)
	    : m_thread([this, &guard, address] {
		      m_result = guard.WaitAt(address);
		      m_finish = Clock::now();
	      }) {
		probe.AwaitEntered();
	}
	/// Returns the result, checking the wait ended soon after the event that should wake it.
	WaitResult Finish(Clock::time_point event) {
		m_thread.join();
		Check(m_finish - event < PROMPT, "waiter was not woken promptly");
		return m_result;
	}

private:
	WaitResult        m_result = WaitResult::NotPending;
	Clock::time_point m_finish {};
	std::thread       m_thread;
};

std::atomic<ReadbackGuard*> g_fault_guard {nullptr};
std::atomic<uint64_t>       g_fault_begin {0};
std::atomic<uint64_t>       g_fault_end {0};

/// Sends faults in the test's pages to the guard, then retries them. A fault can arrive just as its
/// hold is released, so a retry is right even when nothing is pending anymore.
bool HandleFault(const Common::HostException::ExceptionInfo& info) {
	auto*      guard = g_fault_guard.load();
	const auto vaddr = info.access_violation_vaddr;
	if (guard == nullptr || info.type != Common::HostException::ExceptionType::AccessViolation ||
	    vaddr < g_fault_begin.load() || vaddr >= g_fault_end.load()) {
		return false;
	}
	guard->WaitAt(vaddr);
	return true;
}

/// Reads a page on another thread. Faults go to the guard through the installed handler, so a
/// blocked read is really waiting in the fault path.
class Reader {
public:
	explicit Reader(uint64_t address)
	    : m_thread([this, address] {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		      Check(Common::HostException::InitializeThreadSignalStack(), "signal stack failed");
#endif
		      m_value.store(*reinterpret_cast<volatile uint32_t*>(address));
		      m_done.store(true);
	      }) {
	}
	~Reader() { Join(); }

	[[nodiscard]] bool     Done() const { return m_done.load(); }
	[[nodiscard]] uint32_t Value() const { return m_value.load(); }
	void                   Join() {
		if (m_thread.joinable()) {
			m_thread.join();
		}
	}

private:
	std::atomic<bool>     m_done {false};
	std::atomic<uint32_t> m_value {0};
	std::thread           m_thread;
};

// The gate is what keeps a game's many unread readbacks cheap. Each row differs from the qualifying
// shape in one way, so a false result can't come from a failed setup.
void TestHoldRangeGate() {
	constexpr uint64_t  kena_address = 0x202d810000ull;
	constexpr uint64_t  kena_size    = 0x40000;
	constexpr uint64_t  cap          = 1024ull * 1024ull;
	const ReadbackShape kena {
	    .data = {kena_address, kena_size}, .levels = 1, .layers = 1, .depth = 1};
	Check((ReadbackGuard::HoldRange(kena) == GuestRange {kena_address, kena_size}),
	      "Kena's result image should qualify in full");

	// The one linear image size small enough to qualify in the Astro Bot log, which holds only its
	// whole pages.
	constexpr uint64_t astro_size = 276480;
	auto               changed    = kena;
	changed.data.size             = astro_size;
	Check(ReadbackGuard::HoldRange(changed).size == (astro_size & ~(PAGE - 1)),
	      "Astro Bot's small linear image should hold its whole pages");

	changed       = kena;
	changed.tiled = true;
	Check(ReadbackGuard::HoldRange(changed).Empty(), "tiled image qualified");
	changed        = kena;
	changed.levels = 2;
	Check(ReadbackGuard::HoldRange(changed).Empty(), "mipped image qualified");
	changed        = kena;
	changed.layers = 2;
	Check(ReadbackGuard::HoldRange(changed).Empty(), "array image qualified");
	changed       = kena;
	changed.depth = 2;
	Check(ReadbackGuard::HoldRange(changed).Empty(), "volume image qualified");
	changed           = kena;
	changed.data.size = cap;
	Check(ReadbackGuard::HoldRange(changed).size == cap, "image at the size cap should qualify");
	changed.data.size = cap + 1;
	Check(ReadbackGuard::HoldRange(changed).Empty(), "image over the size cap qualified");

	// A 1x1 result, and a page-sized result straddling two pages, hold no whole page.
	changed           = kena;
	changed.data.size = 4;
	Check(ReadbackGuard::HoldRange(changed).Empty(), "sub-page image qualified");
	changed.data = {kena_address + PAGE / 2, PAGE};
	Check(ReadbackGuard::HoldRange(changed).Empty(),
	      "straddling image with no whole page qualified");

	// Only the whole pages in the middle may be held, so unrelated data on the edges never waits.
	changed.data = {kena_address + PAGE / 2, PAGE * 3};
	Check((ReadbackGuard::HoldRange(changed) == GuestRange {kena_address + PAGE, PAGE * 2}),
	      "unaligned image should hold only its whole pages");
}

// A fault can arrive the moment a page is blocked, so the fast check has to report pending by then
// or the fault would skip the guard. The protect stub checks this at the moment of blocking.
void TestFaultDuringHoldSeesPending() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(1);
	g_blocked_before_pending = false;
	g_order_guard            = &guard;
	Check(guard.Hold(pages.Range(0, 1)), "hold failed");
	g_order_guard = nullptr;
	Check(!g_blocked_before_pending.load(), "page was blocked before the hold was visible");
	guard.Release(pages.Range(0, 1));
}

// Readbacks can re-queue while one is pending, so only the first hold may protect, and the page
// must stay blocked until the last one lands.
void TestRepeatHoldProtectsOnce() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(2);
	const auto    range = pages.Range(0, 1);

	g_protect_calls = 0;
	Check(guard.Hold(range) && guard.HasPending(), "first hold failed");
	Check(g_protect_calls == 1, "first hold should protect once");
	Check(guard.Hold(range), "repeat hold failed");
	Check(g_protect_calls == 1, "repeat hold should not protect again");

	guard.Release(range);
	Check(g_protect_calls == 1 && guard.HasPending(), "first release lifted a held range");
	guard.Release(range);
	Check(g_protect_calls == 2 && !guard.HasPending(), "last release did not lift the range");
	Check(guard.TrackedRanges() == 0, "landed range still tracked");
	Check(guard.GetCounters().holds == 2, "holds not counted");
}

// A read of a held page has to block and then see data written after the read began. Reading a
// page that isn't held is the baseline showing the blocking comes from the hold.
void TestRealFaultWaitsForLanding() {
	constexpr uint32_t old_value    = 0x1111;
	constexpr uint32_t landed_value = 0x2222;
	Probe              probe;
	PageManager        manager;
	ReadbackGuard      guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages              pages(2);
	WritePage(pages.At(0), old_value);
	const auto range = pages.Range(0, 1);

	g_fault_begin = pages.At(0);
	g_fault_end   = pages.At(2);
	g_fault_guard = &guard;
	Check(guard.Hold(range), "hold failed");
	{
		Reader unheld(pages.At(1));
		unheld.Join();
		Check(unheld.Done() && probe.entered == 0, "read of an unheld page waited");
	}
	Reader held(pages.At(0));
	probe.AwaitEntered();
	Check(!held.Done(), "read of a held page did not wait");

	// The landing writes through another mapping, which these protect calls stand in for.
	Check(Common::VirtualMemory::Protect(pages.At(0), PAGE, Mode::ReadWrite), "protect failed");
	WritePage(pages.At(0), landed_value);
	Check(Common::VirtualMemory::Protect(pages.At(0), PAGE, Mode::NoAccess), "protect failed");
	guard.Release(range);
	held.Join();
	g_fault_guard = nullptr;
	Check(held.Done() && held.Value() == landed_value, "read did not see the landed data");
	Check(guard.GetCounters().waits == 1, "wait not counted");
}

// Threads that can't block need to know whether an address is held without waiting.
void TestCovers() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(3);
	const auto    range = pages.Range(1, 1);
	Check(!guard.Covers(pages.At(1)), "covered with nothing pending");
	Check(guard.Hold(range), "hold failed");
	Check(guard.Covers(pages.At(1)) && guard.Covers(pages.At(2) - 1), "held page not covered");
	Check(!guard.Covers(pages.At(0)) && !guard.Covers(pages.At(2)), "neighbour reported covered");
	Check(guard.LiftAt(pages.At(1)) && !guard.Covers(pages.At(1)), "lifted page still covered");
	guard.Release(range);
}

// Waits only concern the held pages. Page 1 is held here, so its neighbours on both sides show the
// range check works in each direction.
void TestWaitNotPending() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(3);
	Check(guard.WaitAt(pages.At(1)) == WaitResult::NotPending, "wait with nothing pending");
	const auto range = pages.Range(1, 1);
	Check(guard.Hold(range), "hold failed");
	Check(guard.WaitAt(pages.At(0)) == WaitResult::NotPending, "page below a hold waited");
	Check(guard.WaitAt(pages.At(2)) == WaitResult::NotPending, "page above a hold waited");
	Check(guard.WaitRange(pages.At(2), PAGE) == WaitResult::NotPending,
	      "range above a hold waited");
	Check(guard.WaitRange(pages.At(0), PAGE) == WaitResult::NotPending,
	      "range below a hold waited");
	Check(guard.WaitRange(pages.At(1), 0) == WaitResult::NotPending, "empty range waited");
	guard.Release(range);
}

// A huge range wraps past the top of the address space if overlap is computed by addition, which
// would miss the hold and skip the wait.
void TestHugeRangeDoesNotOverflow() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(1);
	const auto    range = pages.Range(0, 1);
	Check(guard.Hold(range), "hold failed");
	WaitResult  result = WaitResult::NotPending;
	std::thread waiter([&] { result = guard.WaitRange(pages.At(0), UINT64_MAX); });
	probe.AwaitEntered();
	guard.Release(range);
	waiter.join();
	Check(result == WaitResult::Landed, "huge range missed a hold");
}

// With no GPU progress the hold must lift, or the game would hang. The later landing must still be
// safe, since a timeout doesn't cancel it.
void TestTimeoutLiftsAndLandingStaysSafe() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NO_PROGRESS_ONLY);
	Pages         pages(1);
	const auto    range = pages.Range(0, 1);
	Check(guard.Hold(range), "hold failed");
	const auto start = Clock::now();
	Check(guard.WaitAt(pages.At(0)) == WaitResult::TimedOut, "wait should time out");
	Check(Clock::now() - start < PROMPT, "no-progress limit did not end the wait");
	Check(!guard.HasPending(), "timeout did not lift the hold");
	WritePage(pages.At(0), 1);
	const auto counters = guard.GetCounters();
	Check(counters.timeouts == 1 && counters.lifts == 1, "timeout not counted");
	Check(counters.wait_us >= static_cast<uint64_t>(milliseconds(30).count()) * 1000 &&
	          counters.wait_max_us == counters.wait_us,
	      "wait time not recorded");
	Check(guard.TrackedRanges() == 1, "lifted range should still await its landing");
	guard.Release(range);
	Check(guard.TrackedRanges() == 0, "landing left the range tracked");
	Check(guard.WaitAt(pages.At(0)) == WaitResult::NotPending, "landing left something pending");
}

// Progress must keep a wait alive past the no-progress limit, since long GPU stalls exist. The
// timeout test above is the baseline with the same limits and no progress.
void TestProgressExtendsWait() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NO_PROGRESS_ONLY);
	Pages         pages(1);
	const auto    range = pages.Range(0, 1);
	probe.advancing     = true;
	Check(guard.Hold(range), "hold failed");
	std::thread lander([&] {
		probe.AwaitEntered();
		std::this_thread::sleep_for(NO_PROGRESS_ONLY.no_progress_timeout * 3);
		guard.Release(range);
	});
	const auto result = guard.WaitAt(pages.At(0));
	lander.join();
	Check(result == WaitResult::Landed, "progress did not keep the wait alive");
}

// Progress that stops must start the limit again from that moment, or a GPU that advanced once and
// then hung would wait for the absolute limit.
void TestProgressThatStopsTimesOut() {
	constexpr int64_t advances = 3;
	Probe             probe;
	PageManager       manager;
	ReadbackGuard     guard(manager, probe.Source(), NO_PROGRESS_ONLY);
	Pages             pages(1);
	const auto        range = pages.Range(0, 1);
	probe.advances_left     = advances;
	Check(guard.Hold(range), "hold failed");
	const auto start = Clock::now();
	Check(guard.WaitAt(pages.At(0)) == WaitResult::TimedOut, "stopped progress should time out");
	Check(Clock::now() - start < PROMPT, "stopped progress waited for the absolute limit");
	guard.Release(range);
}

// Progress alone can't keep a guest waiting forever.
void TestAbsoluteTimeout() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), ABSOLUTE_ONLY);
	Pages         pages(1);
	const auto    range = pages.Range(0, 1);
	probe.advancing     = true;
	Check(guard.Hold(range), "hold failed");
	const auto start = Clock::now();
	Check(guard.WaitAt(pages.At(0)) == WaitResult::TimedOut, "absolute limit did not end the wait");
	Check(Clock::now() - start >= ABSOLUTE_ONLY.absolute_timeout,
	      "wait ended before the absolute limit");
	guard.Release(range);
}

// A timeout has to lift everything overlapping, or the guest's retry would fault straight back in.
void TestTimeoutLiftsEveryOverlap() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NO_PROGRESS_ONLY);
	Pages         pages(2);
	const auto    first  = pages.Range(0, 1);
	const auto    second = pages.Range(0, 2);
	Check(guard.Hold(first) && guard.Hold(second), "hold failed");
	Check(guard.WaitAt(pages.At(0)) == WaitResult::TimedOut, "wait should time out");
	Check(!guard.HasPending(), "timeout left an overlap protected");
	Check(guard.GetCounters().lifts == 2, "both lifts should count");
	guard.Release(first);
	guard.Release(second);
}

// Repeated timeouts must stop holds, or a stuck GPU becomes a near-freeze. Holds already taken must
// lift too, and they still get their landing. A landed wait between timeouts clears the streak, or
// normal play would trip this eventually.
void TestRepeatedTimeoutsStopHolds() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NO_PROGRESS_ONLY);
	Pages         pages(3);
	const auto    range = pages.Range(0, 1);
	const auto    other = pages.Range(2, 1);

	const auto time_out = [&] {
		Check(guard.Hold(range), "hold refused early");
		Check(guard.WaitAt(pages.At(0)) == WaitResult::TimedOut, "wait should time out");
		guard.Release(range);
	};
	time_out();

	// Progress keeps this wait alive whatever the scheduling, so it can only end by landing.
	Check(guard.Hold(range), "hold refused after one timeout");
	probe.advancing = true;
	probe.entered   = 0;
	std::thread lander([&] {
		probe.AwaitEntered();
		guard.Release(range);
	});
	Check(guard.WaitAt(pages.At(0)) == WaitResult::Landed, "wait should land");
	lander.join();
	probe.advancing = false;

	time_out();
	Check(!guard.GetCounters().stopped, "a landed wait did not clear the streak");

	Check(guard.Hold(other), "hold refused before stopping");
	time_out();
	const auto counters = guard.GetCounters();
	Check(counters.stopped, "repeated timeouts did not stop holds");
	Check(!guard.HasPending(), "stopping left an unrelated hold in place");
	WritePage(other.address, 1);

	// A refused hold must not add a reference, or the pending landing would no longer clear it.
	Check(!guard.Hold(other), "hold allowed after stopping");
	Check(guard.GetCounters().refused == 1, "refused hold not counted");
	guard.Release(other);
	Check(guard.TrackedRanges() == 0, "refused hold left a reference behind");
	Check(!guard.Hold(range) && !guard.HasPending(), "new range held after stopping");
}

// A hold lifted by someone else leaves the guest reading old data. That is no landing, so it must
// be reported as a lift and must not clear the timeout streak.
void TestLiftDoesNotCountAsLanding() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(1);
	const auto    range = pages.Range(0, 1);
	Check(guard.Hold(range), "hold failed");
	Waiter     waiter(guard, probe, pages.At(0));
	const auto lifted_at = Clock::now();
	guard.LiftRange(range.address, range.size);
	Check(waiter.Finish(lifted_at) == WaitResult::Lifted, "lift reported as a landing");
	guard.Release(range);

	Probe         timeout_probe;
	PageManager   timeout_manager;
	ReadbackGuard timeout_guard(timeout_manager, timeout_probe.Source(), NO_PROGRESS_ONLY);
	Pages         timeout_pages(1);
	const auto    timeout_range = timeout_pages.Range(0, 1);
	const auto    time_out      = [&] {
		Check(timeout_guard.Hold(timeout_range), "hold refused early");
		Check(timeout_guard.WaitAt(timeout_pages.At(0)) == WaitResult::TimedOut,
		      "wait should time out");
	};
	time_out();
	// This wait starts on a lifted range held again, and ends with another lift rather than
	// landing.
	Check(timeout_guard.Hold(timeout_range), "hold refused after one timeout");
	timeout_probe.advancing   = true;
	timeout_probe.entered     = 0;
	WaitResult  lifted_result = WaitResult::NotPending;
	std::thread lifted_waiter([&] { lifted_result = timeout_guard.WaitAt(timeout_pages.At(0)); });
	timeout_probe.AwaitEntered();
	timeout_guard.LiftRange(timeout_range.address, timeout_range.size);
	lifted_waiter.join();
	timeout_probe.advancing = false;
	Check(lifted_result == WaitResult::Lifted, "second lift reported as a landing");
	Check(!timeout_guard.GetCounters().stopped, "stopped too early");
	time_out();
	Check(timeout_guard.GetCounters().stopped, "a lift cleared the timeout streak");
	for (uint32_t landing = 0; landing < 3; landing++) {
		timeout_guard.Release(timeout_range);
	}
}

// Unmap needs held pages released even though their landing hasn't arrived, without touching holds
// elsewhere. A waiter on a lifted range must wake rather than wait out its slice.
void TestLiftRangeWakesAndKeepsOthers() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(4);
	const auto    lifted = pages.Range(0, 1);
	const auto    kept   = pages.Range(2, 1);
	Check(guard.Hold(lifted) && guard.Hold(kept), "hold failed");

	Waiter waiter(guard, probe, lifted.address);
	Check(guard.ActiveWaiters() == 1, "waiter not counted while waiting");
	const auto lifted_at = Clock::now();
	guard.LiftRange(lifted.address, lifted.size);
	Check(waiter.Finish(lifted_at) == WaitResult::Lifted, "lifted range did not wake its waiter");
	Check(guard.ActiveWaiters() == 0, "waiter still counted after finishing");
	Check(guard.HasPending(), "lift touched a range outside it");
	WritePage(lifted.address, 1);
	Check(guard.GetCounters().lifts == 1, "range lift not counted");

	guard.LiftAll();
	Check(!guard.HasPending(), "LiftAll left a hold");
	WritePage(kept.address, 1);
	Check(guard.GetCounters().lifts == 2, "LiftAll not counted");
	guard.Release(lifted);
	guard.Release(kept);
}

// Shutdown lifts everything, so a guest still waiting then must wake too.
void TestLiftAllWakes() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(1);
	const auto    range = pages.Range(0, 1);
	Check(guard.Hold(range), "hold failed");
	Waiter     waiter(guard, probe, range.address);
	const auto lifted_at = Clock::now();
	guard.LiftAll();
	Check(waiter.Finish(lifted_at) == WaitResult::Lifted, "LiftAll did not wake its waiter");
	guard.Release(range);
}

// Threads that can't block use LiftAt, and a new hold on a lifted range must protect again while
// its first landing is still due.
void TestLiftAtAndHoldAgain() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(2);
	const auto    range = pages.Range(0, 1);
	Check(!guard.LiftAt(pages.At(0)), "lift with nothing pending");
	Check(guard.Hold(range), "hold failed");
	Check(!guard.LiftAt(pages.At(1)), "lift beside a hold");
	Check(guard.HasPending(), "lift beside a hold removed it");
	Check(guard.LiftAt(pages.At(1) - 1), "lift inside a hold failed");
	Check(!guard.HasPending(), "lift did not remove the hold");
	Check(guard.GetCounters().lifts == 1, "address lift not counted");

	g_protect_calls = 0;
	Check(guard.Hold(range), "second hold failed");
	Check(g_protect_calls == 1 && guard.HasPending(), "hold after a lift did not protect again");

	// A thread that can't block lifts the page, so a guest waiting on it must wake too.
	Waiter     waiter(guard, probe, pages.At(0));
	const auto lifted_at = Clock::now();
	Check(guard.LiftAt(pages.At(0)), "lift for a waiter failed");
	Check(waiter.Finish(lifted_at) == WaitResult::Lifted, "lift did not wake its waiter");

	g_protect_calls = 0;
	Check(guard.Hold(range), "third hold failed");
	Check(g_protect_calls == 1, "third hold did not protect");
	guard.Release(range);
	Check(guard.HasPending(), "first landing released a later hold");
	guard.Release(range);
	Check(guard.HasPending(), "second landing released a later hold");
	guard.Release(range);
	Check(!guard.HasPending() && g_protect_calls == 2, "last landing did not lift");
}

// Overlapping results keep the shared page blocked until both land, which requires each range to
// release exactly what it took. A real read shows the page itself stays blocked.
void TestOverlappingRanges() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(3);
	const auto    first  = pages.Range(0, 2);
	const auto    second = pages.Range(1, 2);
	Check(guard.Hold(first) && guard.Hold(second), "hold failed");
	guard.Release(first);
	WritePage(pages.At(0), 1);
	Check(guard.HasPending(), "other range lost its hold");

	g_fault_begin = pages.At(0);
	g_fault_end   = pages.At(3);
	g_fault_guard = &guard;
	Reader shared(pages.At(1));
	probe.AwaitEntered();
	Check(!shared.Done(), "shared page was not blocked");
	guard.Release(second);
	shared.Join();
	g_fault_guard = nullptr;
	Check(shared.Done(), "read of the shared page did not finish");
	WritePage(pages.At(1), 1);
}

// Ranges with one start and different sizes are separate holds. Merging them would release pages
// that are still pending, or never release them.
void TestSameStartDifferentSizes() {
	Probe         probe;
	PageManager   manager;
	ReadbackGuard guard(manager, probe.Source(), NOTIFY_ONLY);
	Pages         pages(2);
	const auto    small = pages.Range(0, 1);
	const auto    large = pages.Range(0, 2);
	Check(guard.Hold(small) && guard.Hold(large), "hold failed");
	Check(guard.TrackedRanges() == 2, "ranges with one start were merged");
	guard.Release(large);
	WritePage(pages.At(1), 1);
	Check(guard.HasPending(), "small range lost its hold");
	guard.Release(small);
	WritePage(pages.At(0), 1);
	Check(!guard.HasPending() && guard.TrackedRanges() == 0, "landing left state behind");
}

} // namespace

namespace Libs::LibKernel::Memory {

/// Counts protect calls, and records whether a page was blocked before its hold was visible.
bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode) {
	g_protect_calls++;
	if (auto* guard = g_order_guard.load();
	    guard != nullptr && mode == Mode::NoAccess && !guard->HasPending()) {
		g_blocked_before_pending = true;
	}
	return Common::VirtualMemory::Protect(vaddr, size, mode);
}

} // namespace Libs::LibKernel::Memory

int main() {
	Check(Common::HostException::InstallHandler(HandleFault), "InstallHandler failed");
	TestHoldRangeGate();
	TestFaultDuringHoldSeesPending();
	TestRepeatHoldProtectsOnce();
	TestRealFaultWaitsForLanding();
	TestCovers();
	TestWaitNotPending();
	TestHugeRangeDoesNotOverflow();
	TestTimeoutLiftsAndLandingStaysSafe();
	TestProgressExtendsWait();
	TestProgressThatStopsTimesOut();
	TestAbsoluteTimeout();
	TestTimeoutLiftsEveryOverlap();
	TestRepeatedTimeoutsStopHolds();
	TestLiftDoesNotCountAsLanding();
	TestLiftRangeWakesAndKeepsOthers();
	TestLiftAllWakes();
	TestLiftAtAndHoldAgain();
	TestOverlappingRanges();
	TestSameStartDifferentSizes();
	std::puts("ReadbackGuardTests: all cases passed");
	return 0;
}
