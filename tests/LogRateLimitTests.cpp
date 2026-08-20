#include "common/emulatorConfig.h"
#include "common/logging/log.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "LogRateLimitTests: failed: %s\n", text);
		std::abort();
	}
}

uint64_t CountWrites(Log::Site& site, uint64_t calls) {
	uint64_t written = 0;
	for (uint64_t i = 0; i < calls; i++) {
		if (Log::ShouldWrite(site)) {
			written++;
		}
	}
	return written;
}

// The reported problem: a line inside a per-draw loop fires millions of times
// and turns the log into something nobody can upload (issue #200).
void TestARunawaySiteIsBounded() {
	Log::Site site {"runaway.cpp", 1};

	constexpr uint64_t CALLS   = 10'000'000;
	const auto         written = CountWrites(site, CALLS);

	Check(written < CALLS / 1000, "a runaway site writes a tiny fraction of its calls");
	Check(site.count.load() == CALLS, "every call is still counted");

	// The whole point is that the volume stops growing linearly. Ten million
	// messages must not cost anything like ten million lines.
	std::printf("LogRateLimitTests: %llu calls -> %llu messages\n",
	            static_cast<unsigned long long>(CALLS),
	            static_cast<unsigned long long>(written));
}

// A line that fires rarely must be completely unaffected: this must not lose
// information, only repetition.
void TestQuietSitesAreUntouched() {
	Log::Site site {"quiet.cpp", 2};
	Check(CountWrites(site, 32) == 32, "a site below the limit writes every message");
}

void TestSitesAreIndependent() {
	Log::Site loud {"loud.cpp", 3};
	Log::Site quiet {"quiet.cpp", 4};

	CountWrites(loud, 1'000'000);
	Check(CountWrites(quiet, 16) == 16, "one noisy site does not silence another");
}

// The first messages from a site are what a bug report needs: they show the
// problem starting. Losing those in favour of later ones would defeat it.
void TestTheFirstMessagesAlwaysGetThrough() {
	Log::Site site {"early.cpp", 5};

	uint64_t first_run = 0;
	for (uint64_t i = 0; i < 64; i++) {
		if (Log::ShouldWrite(site)) {
			first_run++;
		} else {
			break;
		}
	}
	Check(first_run == 64, "the opening messages are never dropped");
}

void TestCountingIsThreadSafe() {
	Log::Site site {"threads.cpp", 6};

	constexpr uint64_t PER_THREAD = 100'000;
	constexpr int      THREADS    = 8;

	std::vector<std::thread> threads;
	threads.reserve(THREADS);
	for (int i = 0; i < THREADS; i++) {
		threads.emplace_back([&site] { CountWrites(site, PER_THREAD); });
	}
	for (auto& thread: threads) {
		thread.join();
	}

	Check(site.count.load() == PER_THREAD * THREADS, "no counts are lost across threads");
}

void TestDisabledLimitWritesEverything() {
	// A limit of zero is the escape hatch for anyone who really does want the
	// old behaviour; verify it is a true bypass rather than a large limit.
	Config::ConfigOptions options {};
	options.log_repeat_limit = 0;
	Config::Load(options);
	Log::Initialize();

	Log::Site site {"unlimited.cpp", 7};
	Check(CountWrites(site, 100'000) == 100'000, "a zero limit writes every message");
}

} // namespace

int main() {
	Config::Initialize();

	Config::ConfigOptions options {};
	options.printf_direction = Config::OutputDirection::Silent;
	Config::Load(options);
	Log::Initialize();

	TestARunawaySiteIsBounded();
	TestQuietSitesAreUntouched();
	TestSitesAreIndependent();
	TestTheFirstMessagesAlwaysGetThrough();
	TestCountingIsThreadSafe();
	TestDisabledLimitWritesEverything();

	std::printf("LogRateLimitTests: all passed\n");
	return 0;
}
