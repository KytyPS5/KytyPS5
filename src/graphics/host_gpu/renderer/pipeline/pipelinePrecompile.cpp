#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/presentation/precompileNotice.h"
#include "kernel/guestPause.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace Libs::Graphics {

namespace {

// The game stops (and, with --pipeline-precompile ask, the player is asked) when what the driver
// has left to compile would take this long with the helpers. Once the player waits (or did not
// answer), or with --pipeline-precompile wait, it stops for less: it waits already.
constexpr double AskSeconds  = 30.0;
constexpr double WaitSeconds = 20.0;
// After the game goes on, the notice stays this long, and the game stops again under it for less
// (Demon's Souls maps its shaders in batches while it boots and loads; one notice then shows the
// batches that follow each other closely, instead of one popping up after another).
constexpr auto   GraceTime    = std::chrono::seconds(3);
constexpr double GraceSeconds = 5.0;
// The notice is up whenever a pipeline has taken this long to compile, whoever compiles it (the
// game's threads, background and manifest threads, helper processes), and for NoticeLinger after
// the last one that did.
constexpr double SlowSeconds   = 0.5;
constexpr auto   NoticeLinger  = GraceTime;
// A helper's pipeline before the helpers compiled a few (NVIDIA's compiler, cold: ~1 s).
constexpr double DefaultJobSeconds = 1.0;
// Compute pipelines looked up in the driver cache before the share of them it misses is believed.
constexpr uint64_t MinLookups = 32;
constexpr auto     WatchPeriod = std::chrono::milliseconds(100);
// While the game is stopped: how often threads that were in the emulator are tried again.
constexpr auto     PausePeriod = std::chrono::milliseconds(50);
constexpr auto     LogPeriod   = std::chrono::seconds(10);
// The game's pipeline stall: it counts as one (and may stop the game too) once its GPU work waited
// this long for a pipeline, and lasts while the waits follow one another closer than StallGap. The
// notice says it waits from SlowSeconds on.
constexpr auto     StallShown  = std::chrono::seconds(3);
constexpr auto     StallGap    = std::chrono::seconds(1);
// The manifest's bar (ManifestProgress): up once the manifest's threads and the helpers have had
// work for ManifestBarDelay, and until they have had none for ManifestBarLinger (Demon's Souls maps
// its shaders in batches while it boots and loads: one bar for the batches that follow each other
// closely).
constexpr auto     ManifestBarDelay  = std::chrono::seconds(1);
constexpr auto     ManifestBarLinger = std::chrono::seconds(2);

template <typename... Args>
void PrecompileLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

struct Estimate {
	// Pipelines known to be left (helper jobs and graphics pipelines whose modules are built), and
	// those finished so far.
	uint32_t pending  = 0;
	uint64_t finished = 0;
	// Shaders whose code is mapped and which are not translated yet, and those translated so far
	// (or found translated, or failed).
	uint32_t shaders  = 0;
	uint64_t shaders_finished = 0;
	// Records the manifest's threads build or create now.
	uint32_t running  = 0;
	// What the driver has left to compile, and all that is left.
	double   driver_seconds = 0.0;
	double   seconds_left   = 0.0;
	bool     drained        = false;
	// The helpers' jobs that have run SlowSeconds or more, and the longest of them.
	uint32_t helpers_slow    = 0;
	double   helpers_longest = 0.0;
};

} // namespace

PipelineCache::StallScope::StallScope(PipelineCache& cache): m_cache(cache) {
	std::scoped_lock lock(cache.m_stall_mutex);
	if (cache.m_stall.threads++ == 0) {
		cache.m_stall.since = std::chrono::steady_clock::now();
	}
}

PipelineCache::StallScope::~StallScope() {
	std::scoped_lock lock(m_cache.m_stall_mutex);
	if (--m_cache.m_stall.threads == 0) {
		m_cache.m_stall.ended = std::chrono::steady_clock::now();
	}
}

PipelineCache::PipelineStall PipelineCache::GetPipelineStall() const {
	std::scoped_lock lock(m_stall_mutex);
	return m_stall;
}

void PipelineCache::StartPrecompile() {
	m_precompile = std::jthread([this](const std::stop_token& stop) { Precompile(stop); });
}

// Cold (a first session, or a new driver), the driver compiles the pipelines the manifest lists as
// the game maps their shaders, while the game runs and competes with it for the processors, and
// the game stalls on those it needs first. When what is left would take long, this thread stops
// the game's threads (LibKernel::GuestPause) until it is compiled: the helpers and the manifest's
// threads then have the processors to themselves. With --pipeline-precompile ask (the default)
// the first notice asks the player whether to wait for them or to play at once, once a session:
// playing at once lets the game go on as without this thread, and the rest compiles in the
// background; waiting, or no answer, stops the game again for the pipelines it maps later, with
// the progress alone.
//
// Whatever the mode, the notice is up too whenever any pipeline has taken SlowSeconds to compile,
// whoever compiles it (the game's GPU work waiting for it, which says so; background and manifest
// threads; the helper processes), and for NoticeLinger after the last one that did.
void PipelineCache::Precompile(const std::stop_token& stop) {
	KYTY_PROFILER_THREAD("Thread_PipelinePrecompile");
	using Clock = std::chrono::steady_clock;
	// Played: the game is not stopped for pipelines any more (the player plays at once, or this
	// thread may not stop it); only the notice of its waits is left.
	enum class Phase { Watch, Ask, Wait, Grace, Played };

	const auto estimate = [this] {
		const auto stats   = m_helpers != nullptr ? m_helpers->GetStats() : PipelineHelpers::Stats {};
		const auto backlog = GetManifestBacklog();
		Estimate   e;
		// The compute modules not translated yet need pipelines the driver cache misses as often
		// as it missed those looked up so far.
		const uint64_t lookups = stats.queued + m_helper_found.load(std::memory_order_relaxed);
		const double   misses  = lookups >= MinLookups
		                             ? static_cast<double>(stats.queued) / static_cast<double>(lookups)
		                             : 0.0;
		const uint32_t jobs    = stats.waiting + stats.in_flight;
		const double   job     = stats.job_seconds > 0.0 ? stats.job_seconds : DefaultJobSeconds;
		// A job that has run long is not about to end: helpers' times have a long tail (some of
		// Wolverine's and GTA V's take minutes, against ~1 s usually). One that has run t is taken
		// to need as long again, and at least what is left of a usual one; the helpers run their
		// jobs side by side, so the longest of them is the least it all takes.
		double running = 0.0;
		double longest = 0.0;
		for (size_t i = 0; i < stats.running.size(); i++) {
			const double ran = stats.running[i];
			e.helpers_slow += ran >= SlowSeconds ? 1u : 0u;
			e.helpers_longest = std::max(e.helpers_longest, ran);
			// A large one is known to take long before it has run long.
			const double left = std::max(std::max(job, stats.running_expected[i]) - ran, ran);
			running += left;
			longest = std::max(longest, left);
		}
		const double   queued  = std::max(stats.waiting * job, stats.waiting_seconds) +
		                         backlog.compute_modules * misses * job;
		const double   helpers = std::max(longest, (running + queued) /
		                                               static_cast<double>(std::max(stats.processes, 1u)));
		const double   threads  = static_cast<double>(std::max(backlog.threads, 1u));
		const double   graphics = backlog.pipelines * backlog.pipeline_seconds / threads;
		const double   modules  = backlog.modules * backlog.module_seconds / threads;
		e.pending        = jobs + backlog.pipelines;
		e.finished       = stats.done + stats.failed + stats.withdrawn + backlog.pipelines_done;
		e.shaders        = backlog.modules;
		e.shaders_finished = backlog.modules_done;
		e.running        = backlog.running;
		e.driver_seconds = helpers + graphics;
		e.seconds_left   = std::max(helpers, graphics + modules);
		e.drained = jobs == 0 && backlog.modules == 0 && backlog.pipelines == 0 && backlog.running == 0 &&
		            !backlog.scanning;
		return e;
	};

	// While the game is stopped, more helpers: the processors are theirs.
	const uint32_t helpers_running = m_helpers != nullptr ? m_helpers->GetStats().processes : 0;
	const uint32_t helpers_alone   = PipelineHelperProcessesAlone(helpers_running);
	const auto     set_helpers     = [this](uint32_t processes) {
		if (m_helpers != nullptr) {
			m_helpers->SetProcesses(processes);
		}
	};
	const bool can_stop = m_helpers != nullptr && m_manifest_warm &&
	                      Config::GetPipelinePrecompile() != Config::PipelinePrecompile::Play &&
	                      LibKernel::GuestPause::Supported();
	bool                                   wait  = Config::GetPipelinePrecompile() == Config::PipelinePrecompile::Wait;
	auto                                   phase = can_stop ? Phase::Watch : Phase::Played;
	std::unique_ptr<LibKernel::GuestPause> pause;
	Clock::time_point                      shown_at;  // the game first stopped under the notice
	Clock::time_point                      paused_at; // the game, this time
	Clock::time_point                      logged_at;
	Clock::time_point                      grace_end;
	// The notice stays until then: SlowSeconds of a compilation, plus NoticeLinger.
	Clock::time_point                      slow_until;
	uint64_t                               base       = 0; // finished when the game first stopped
	uint64_t                               stop_base  = 0; // and when the game stopped this time
	uint32_t                               stops      = 0;
	// The game's waits for pipelines, once one lasted StallShown.
	bool                                   stalled    = false;
	Clock::time_point                      stall_since;
	Clock::time_point                      stall_logged;
	uint64_t                               stall_base = 0;
	// The notice on screen, whatever brought it (this thread stopping the game, the game waiting
	// for pipelines, or one after the other): its progress counts from when it appeared, and it
	// stays until neither keeps it.
	bool                                   shown       = false;
	uint64_t                               notice_base = 0;
	Clock::time_point                      notice_shown_at;
	// The manifest's bar: its work counts shaders translated and pipelines created alike, from when
	// the work began.
	bool                                   bar_shown = false;
	std::optional<Clock::time_point>       bar_work_since;
	Clock::time_point                      bar_last_work;
	uint64_t                               bar_base = 0;
	const auto seconds = [](Clock::duration duration) { return std::chrono::duration<double>(duration).count(); };

	std::mutex                  sleep_mutex;
	std::condition_variable_any sleeper;
	const auto                  sleep = [&](Clock::duration period) {
		std::unique_lock lock(sleep_mutex);
		sleeper.wait_for(lock, stop, period, [] { return false; });
	};
	const auto play = [&] {
		pause.reset();
		set_helpers(helpers_running);
		phase = Phase::Played;
	};

	while (!stop.stop_requested()) {
		const bool paused = phase == Phase::Ask || phase == Phase::Wait;
		sleep(paused ? Clock::duration(PausePeriod) : Clock::duration(WatchPeriod));
		const auto e     = estimate();
		const auto now   = Clock::now();
		const auto stall = GetPipelineStall();
		// A pipeline the game waits for may be in no queue: one its GPU thread creates itself.
		const auto left  = std::max(e.pending, stall.threads);
		// Any compilation that takes long: the driver's on any thread of this process, the helper
		// processes', and the waits of the game for a pipeline whoever creates it.
		const auto creations = GetDriverCreations(SlowSeconds);
		const bool waits_slow = stall.threads != 0 && now - stall.since >= std::chrono::duration<double>(SlowSeconds);
		const uint32_t slow = creations.slow + e.helpers_slow;
		if (slow != 0 || waits_slow) {
			slow_until = now + NoticeLinger;
		}
		if (!stalled && stall.threads != 0 && now - stall.since >= StallShown) {
			stalled      = true;
			stall_since  = stall.since;
			stall_logged = now;
			stall_base   = e.finished;
			PrecompileLog("Pipeline stall: the game has waited {:.1f} s for the pipelines the driver compiles "
			              "({} left)",
			              seconds(now - stall_since), left);
		} else if (stalled && stall.threads == 0 && now - stall.ended >= StallGap) {
			stalled = false;
			PrecompileLog("Pipeline stall: the game goes on after {:.1f} s ({} pipelines compiled meanwhile)",
			              seconds(stall.ended - stall_since), e.finished - stall_base);
		} else if (stalled && now - stall_logged >= LogPeriod) {
			stall_logged = now;
			PrecompileLog("Pipeline stall: {} pipelines left at {:.0f} s", left, seconds(now - stall_since));
		}
		if (phase == Phase::Watch || phase == Phase::Grace) {
			const bool grace = phase == Phase::Grace;
			// While the game waits for pipelines it is stopped anyway: stopping its threads too costs
			// it nothing and gives the helpers the processors. So that notice comes for less, and
			// first, instead of after one that says the game waits.
			const double threshold = grace || stalled ? GraceSeconds : wait ? WaitSeconds : AskSeconds;
			if (grace && (slow != 0 || waits_slow)) {
				// Another slow compilation while the notice is up: it stays for it.
				grace_end = std::max(grace_end, slow_until);
			}
			if (grace && TakePrecompileAnswer() == PrecompileAnswer::Play) {
				PrecompileLog("Pipeline precompile: the player plays at once ({} pipelines left, compiled in the "
				              "background)",
				              e.pending);
				play();
			} else if (e.driver_seconds < threshold || !LibKernel::GuestPause::GuestStarted()) {
				// (With the code of earlier sessions or of the game's files, the pipelines are known
				// before the game is loaded: it is stopped once it starts.)
				if (grace && now >= grace_end) {
					PrecompileLog("Pipeline precompile: notice closed after {:.1f} s: {} pipelines compiled while "
					              "the game was stopped {} times",
					              seconds(now - shown_at), e.finished - base, stops);
					set_helpers(helpers_running);
					phase = Phase::Watch;
				}
			} else {
				if (!grace) {
					// An answer given to an earlier notice does not answer this one.
					(void)TakePrecompileAnswer();
					shown_at = now;
					base     = e.finished;
					stops    = 0;
				}
				phase     = wait ? Phase::Wait : Phase::Ask;
				pause     = std::make_unique<LibKernel::GuestPause>();
				set_helpers(helpers_alone);
				paused_at = now;
				logged_at = now;
				stop_base = e.finished;
				stops++;
				PrecompileLog("Pipeline precompile: about {:.0f} s of pipelines for the driver to compile ({} "
				              "pipelines, {} shaders to translate): the game waits for them{}",
				              e.driver_seconds, e.pending, e.shaders,
				              phase == Phase::Ask ? ", or for the player to play at once"
				              : grace             ? " (the same notice)"
				                                  : "");
			}
		}
		if (phase == Phase::Ask || phase == Phase::Wait) {
			pause->Poll();
			const auto answer = TakePrecompileAnswer();
			if (answer == PrecompileAnswer::Play) {
				PrecompileLog("Pipeline precompile: the player plays at once after {:.1f} s ({} pipelines left, "
				              "compiled in the background)",
				              seconds(now - shown_at), e.pending);
				play();
			} else {
				if (answer == PrecompileAnswer::Wait && phase == Phase::Ask) {
					PrecompileLog("Pipeline precompile: the player waits");
					phase = Phase::Wait;
					wait  = true;
				}
				if (e.drained) {
					PrecompileLog("Pipeline precompile: {} pipelines compiled in {:.1f} s; the game goes on",
					              e.finished - stop_base, seconds(now - paused_at));
					// The helpers keep the processors until the notice goes: the game mostly stops again.
					pause.reset();
					// The answer holds for the session, and no answer is to wait: the game stops again
					// for the pipelines it maps next, without asking.
					wait      = true;
					phase     = Phase::Grace;
					grace_end = now + GraceTime;
				} else if (now - logged_at >= LogPeriod) {
					logged_at = now;
					PrecompileLog("Pipeline precompile: {} of {} pipelines, {} shaders to translate, about {:.0f} s "
					              "left at {:.0f} s; {} guest threads stopped",
					              e.finished - base, e.finished - base + e.pending, e.shaders, e.seconds_left,
					              seconds(now - paused_at), pause->Suspended());
				}
			}
		}

		// The manifest's bar, while the game runs: stopped, it has the notice's progress.
		if (m_manifest_warm) {
			const uint64_t finished = e.finished + e.shaders_finished;
			const uint32_t left     = e.pending + e.running + e.shaders;
			if (left != 0) {
				bar_last_work = now;
				if (!bar_shown && !bar_work_since) {
					bar_work_since = now;
					bar_base       = finished;
				}
			} else if (!bar_shown) {
				bar_work_since.reset();
			}
			if (!bar_shown && bar_work_since && now - *bar_work_since >= ManifestBarDelay) {
				bar_shown = true;
				PrecompileLog("Pipeline manifest bar: shown ({} pipelines left, {} shaders to translate)",
				              e.pending + e.running, e.shaders);
			} else if (bar_shown && now - bar_last_work >= ManifestBarLinger) {
				bar_shown = false;
				PrecompileLog("Pipeline manifest bar: hidden after {:.1f} s ({} shaders and pipelines done)",
				              seconds(now - *bar_work_since), finished - std::min(bar_base, finished));
				bar_work_since.reset();
			}
			if (bar_shown && phase != Phase::Ask && phase != Phase::Wait) {
				const auto done = static_cast<uint32_t>(finished - std::min(bar_base, finished));
				ShowManifestProgress({.done      = done,
				                      .total     = done + left,
				                      .pipelines = e.pending + e.running,
				                      .shaders   = e.shaders});
			} else {
				HideManifestProgress();
			}
		}

		// One notice: what the game does now decides its text, whatever brought the notice.
		std::optional<PrecompileNoticeState> state;
		if (phase == Phase::Ask) {
			state = PrecompileNoticeState::Question;
		} else if (phase == Phase::Wait) {
			state = PrecompileNoticeState::Paused;
		} else if ((stalled || waits_slow) && (stall.threads != 0 || phase != Phase::Grace)) {
			// Not once the game goes on after a stop: that notice stays anyway, and saying it waits
			// for the StallGap would come between "paused" and "ready".
			state = PrecompileNoticeState::Waiting;
		} else if (phase == Phase::Grace) {
			state = PrecompileNoticeState::Ready;
		} else if (now < slow_until) {
			// The game runs, and a pipeline takes long (or did a moment ago).
			state = PrecompileNoticeState::Compiling;
		}
		if (!state) {
			if (shown) {
				shown = false;
				HidePrecompileNotice();
				PrecompileLog("Pipeline notice: hidden after {:.1f} s, {:.1f} s after the last compilation over {:.1f} s",
				              seconds(now - notice_shown_at), seconds(now - (slow_until - NoticeLinger)), SlowSeconds);
			}
			continue;
		}
		if (!shown) {
			shown           = true;
			notice_base     = e.finished;
			notice_shown_at = now;
			constexpr const char* Names[] = {"question", "paused", "waiting", "ready", "compiling"};
			PrecompileLog("Pipeline notice: shown, state {} ({} driver creations for {:.1f} s, {} helper jobs "
			              "for {:.1f} s, {} game threads waiting)",
			              Names[static_cast<uint32_t>(*state)], creations.running, creations.longest,
			              e.pending, e.helpers_longest, stall.threads);
		}
		const auto done = static_cast<uint32_t>(e.finished - notice_base);
		ShowPrecompileNotice({.visible          = true,
		                      .state            = *state,
		                      .done             = done,
		                      .total            = done + left,
		                      .shaders          = *state == PrecompileNoticeState::Waiting ? 0u : e.shaders,
		                      .waited_pipelines = *state == PrecompileNoticeState::Compiling ? slow : stall.threads,
		                      .seconds_waited   = *state == PrecompileNoticeState::Compiling
		                                              ? static_cast<uint32_t>(std::max(creations.longest, e.helpers_longest))
		                                              : static_cast<uint32_t>(seconds(now - (stall.threads != 0 ? stall.since : stall_since)))});
	}
	pause.reset();
	set_helpers(helpers_running);
	HidePrecompileNotice();
	HideManifestProgress();
}

} // namespace Libs::Graphics
