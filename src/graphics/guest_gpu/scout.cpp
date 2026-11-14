#include "graphics/guest_gpu/scout.h"

#include "common/guardedCopy.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/lodStatsReport.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/shaderBindings.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <x86intrin.h>
#include <xxhash.h>

namespace {
// KYTY_SCOUT: 0 off, 1 the scout thread's tickets used, 2 its verifier, 3 (the default here) the tickets are made by the
// resolve thread (EvaluateAhead), which sees every operation with its exact registers.
uint32_t ScoutModeFromEnvironment() {
	const char* value = std::getenv("KYTY_SCOUT");
	return value != nullptr && value[0] >= '0' && value[0] <= '3' && value[1] == '\0' ? static_cast<uint32_t>(value[0] - '0') : 3u;
}
} // namespace

extern "C" {
[[gnu::used]] volatile std::atomic_uint32_t kyty_local_scout_mode {ScoutModeFromEnvironment()};
}

namespace Libs::Graphics::Scout {

namespace {

// The scout's counters (chenxiao07's LiveCounters::Scout*, kept here: the render side's are the table path's own).
// Reported on Stop.
namespace LiveCounters {
enum Id : uint32_t {
	ScoutAbandoned, ScoutStoodDown, ScoutWriteBacks, ScoutWaitsStopped, ScoutWaitsPassed, ScoutWaitOther, ScoutWaitChecks,
	ScoutWaitChanged, ScoutTooBig, ScoutTicketWords, ScoutTickets, ScoutSubmissions, ScoutShort, ScoutRewinds, ScoutRefused,
	ScoutNoViewOther, ScoutNoView, ScoutNotTable, ScoutLeadStalls, ScoutFenced, ScoutFeedFull, ScoutDrawPackets, ScoutConstant,
	ScoutBehind, ScoutBackingTickets, ScoutBackingRanges, Count
};
constexpr const char* Names[Count] = {
	"abandoned", "stood down", "write-backs", "waits stopped", "waits passed", "waits on other words", "waits checked",
	"waits changed", "too big", "ticket words", "tickets", "submissions", "short reads", "rewinds", "refused", "no view (other pair)",
	"no view", "not table", "lead stalls", "fenced", "feed full", "draw packets", "constant", "behind", "backing tickets",
	"backing ranges"};
std::atomic<uint64_t> g_counts[Count];
inline void           Add(Id id, uint64_t n = 1) noexcept { g_counts[id].fetch_add(n, std::memory_order_relaxed); }
void Report() {
	std::string line;
	for (uint32_t i = 0; i < Count; ++i)
		if (const auto n = g_counts[i].load(std::memory_order_relaxed); n != 0)
			line += std::string(" ") + Names[i] + " " + std::to_string(n) + ";";
	std::printf("Scout:%s\n", line.c_str());
	std::fflush(stdout);
}
} // namespace LiveCounters

// Whether the scout may spin for the render thread (chenxiao07's SpinGate: the hand-off workers spin only while the
// process keeps CPUs idle). This tree has no recording worker to measure it: always open.
namespace SpinGate {
inline bool Open(bool) { return true; }
} // namespace SpinGate

// The words the render thread and the scout share, grouped by the thread that writes them and how often, each group on
// cache lines no other variable shares (alignas(128): a struct's size is then a multiple of 128, the pair of 64-byte
// lines the cores' adjacent-line prefetcher fetches together). Packed by declaration they shared two lines: the scout's
// store at every draw it evaluated and every pause of its idle spin (Quiesce) and its ring tail took them from the
// render thread's core, whose read of g_running at every command packet and loads of the ring head and the write clock
// at every table draw then missed (1-1, 120 Hz cap: ~1.2% of the render thread's samples on those loads).
struct alignas(128) RenderPosition {
	std::atomic<uint64_t> render {0};    // the render thread's position (g_render), at every draw
	std::atomic<uint64_t> ring_head {0}; // the tickets it has passed, at every draw
};
struct alignas(128) RenderFeed {
	std::atomic<uint64_t> feed_tail {0}; // the submissions fed, at every submission (the idle scout's spin reads it)
	std::atomic<uint32_t> wake {0};      // moves with each feed and the stop (the idle scout waits on it)
};
struct alignas(128) ScoutWritten {
	std::atomic<uint64_t> ring_tail {0}; // the tickets made, at every ticket
	std::atomic<uint64_t> feed_head {0}; // the submissions taken
	std::atomic<uint64_t> quiescent {0}; // the retire clock at the scout's last quiescent point (Quiesce)
};
struct alignas(128) ReadMostly {
	std::atomic<bool>     running {false}, stop {false};
	std::atomic<uint64_t> retire_clock {0}; // moves when the render thread retires a view
};
struct alignas(128) RenderOwn {
	uint64_t        render_sequence = 0;
	uint32_t        render_count    = 0;
	const uint32_t* render_packet   = nullptr;
	uint64_t        known_tail      = 0;
	uint64_t        version         = 0;
};
struct alignas(128) AnyWriter {
	std::atomic<uint64_t> write_clock {0}; // NoteHostWrite's, from any thread
};
RenderPosition g_render_position;
RenderFeed     g_render_feed;
ScoutWritten   g_scout_written;
ReadMostly     g_read_mostly;
RenderOwn      g_render_own;
AnyWriter      g_any_writer;

// Submissions fed and not yet walked.
struct FeedEntry {
	uint64_t        sequence = 0;
	const uint32_t* commands = nullptr;
	uint32_t        size     = 0;
	bool            constant = false, reset = false;
};
constexpr uint64_t                 FeedEntries = 1024;
std::array<FeedEntry, FeedEntries> g_feed;
constexpr auto&                    g_feed_tail = g_render_feed.feed_tail;
constexpr auto&                    g_feed_head = g_scout_written.feed_head;
constexpr auto&                    g_wake      = g_render_feed.wake;

// Tickets, in stream order; the scout stays at most Lead draws ahead of the render thread.
constexpr uint64_t                RingSlots = 128;
constexpr int64_t                 Lead      = 64;
std::array<Ticket, RingSlots>     g_ring;
constexpr auto&                   g_ring_tail = g_scout_written.ring_tail;
constexpr auto&                   g_ring_head = g_render_position.ring_head;

// The render thread's position: its submission's sequence number << 24 | the draws it has reached in it. Its own
// copies, and the packet of its current draw.
constexpr auto& g_render          = g_render_position.render;
constexpr auto& g_render_sequence = g_render_own.render_sequence;
constexpr auto& g_render_count    = g_render_own.render_count;
constexpr auto& g_render_packet   = g_render_own.render_packet;

// Words the command processor wrote (direct-mapped by address, across frames). At KYTY_SCOUT=2 also the value each
// held after its write (the GPU thread's only: CheckWait).
constexpr size_t                                LabelEntries = 16384;
std::array<std::atomic<uint64_t>, LabelEntries> g_labels {};
std::array<uint32_t, LabelEntries>              g_label_values {};

// The emulator's own writes to guest memory (NoteHostWrite), from any thread: the last ones, by their clock value
// (their pages), and the clock the scout reads. (A 4 KiB page-hashed table of stamps cost the render thread a missed
// load a page at every use: 0.2-0.3 ms a profiled frame.) A writer takes its clock value,
// claims the entry (Busy, only from an older value), writes its pages and publishes the value: an entry whose clock
// is not the one looked for, before and after its pages are read, is not known.
struct HostWrite {
	std::atomic<uint64_t>     clock {0};
	std::atomic<uint64_t>     first {0}, last {0}; // pages; last's top bit: a write-back
	static constexpr uint64_t Busy = 1ull << 63u, WriteBack = 1ull << 63u;
};
constexpr uint64_t                  WriteEntries = 4096;
std::array<HostWrite, WriteEntries> g_writes;
constexpr auto&                     g_write_clock = g_any_writer.write_clock;
// The ring's tail as the render thread last read it (the line the scout writes at every ticket).
constexpr auto&                     g_known_tail = g_render_own.known_tail;

// The scout's reason for each draw it reached (Reason): submission << 40 | draw << 8 | reason, by the draw's number.
constexpr size_t                              ReasonEntries = 4096;
std::array<std::atomic<uint64_t>, ReasonEntries> g_reasons {};

// Pair views by the pair key's hash, and the ones retired (render thread) until the scout passed a quiescent point.
// (4096 entries lost 8-14% of the evaluated draws to other pairs' views, by launch: the shaders' addresses differ.)
constexpr size_t                                 DirectoryEntries = 65536;
std::array<std::atomic<View*>, DirectoryEntries> g_directory {};
constexpr auto&                                  g_retire_clock = g_read_mostly.retire_clock;
constexpr auto&                                  g_quiescent    = g_scout_written.quiescent;
std::vector<View*>                               g_retired;
constexpr auto&                                  g_version = g_render_own.version;

constexpr auto&     g_running = g_read_mostly.running;
constexpr auto&     g_stop    = g_read_mostly.stop;
std::thread*        g_thread = nullptr; // (on the heap: a game-initiated exit() must not find a static one joinable; Stop() deletes it)
RenderContext* g_resources = nullptr;

constexpr std::array<uint32_t, 8> LodStatsMask {0, 0, 0, 0, 0, (0xfffu << 8u) | (1u << 25u), 0xffu, 0};

uint64_t LabelSlot(uint64_t address) {
	return ((address >> 2u) * 0x9e3779b97f4a7c15ull) >> 50u;
}

void Note(uint64_t sequence, uint32_t ordinal, Reason reason) {
	g_reasons[ordinal & (ReasonEntries - 1)].store((sequence & 0xffffffu) << 40u | uint64_t {ordinal} << 8u |
	                                                   static_cast<uint8_t>(reason),
	                                               std::memory_order_relaxed);
}

bool Read(void* out, uint64_t address, uint64_t bytes) {
	if (address == 0) return false;
#if !defined(_WIN32)
	// Linux: GuardedCopy is a pread of /proc/self/mem, one system call per read. Direct memory goes through the
	// backing view first, as the watched ranges already do; what the view cannot serve still takes the guarded copy.
	if (Libs::LibKernel::Memory::TryReadBackingToHost(address, out, bytes)) return true;
#endif
	return Common::GuardedCopy(out, reinterpret_cast<const void*>(address), bytes) == 0;
}

// The GS user SGPR at an SH register location, -1 for another register (graphicsRun.cpp's GsUserSgprAt).
int32_t GsUserSgprAt(uint32_t location) {
	const auto id = location - Pm4::SPI_SHADER_USER_DATA_GS_0;
	return id < 32u ? static_cast<int32_t>(id) : -1;
}

bool TestValue(uint64_t value, uint64_t ref, uint64_t mask, uint32_t func) {
	switch (func) {
		case 0: return true;
		case 1: return (value & mask) < ref;
		case 2: return (value & mask) <= ref;
		case 3: return (value & mask) == ref;
		case 4: return (value & mask) != ref;
		case 5: return (value & mask) >= ref;
		case 6: return (value & mask) > ref;
		default: return false;
	}
}

void Quiesce() {
	// (Stored only when the clock moved: with no view retired since the last value published, the scout holds none the
	// render thread could free, and the same value again frees nothing more. It was a store at every draw evaluated and
	// every pause of the idle spin.)
	const auto clock = g_retire_clock.load(std::memory_order_seq_cst);
	if (g_quiescent.load(std::memory_order_relaxed) != clock) g_quiescent.store(clock, std::memory_order_seq_cst);
}

void FreeRetired() {
	const auto quiescent = g_quiescent.load(std::memory_order_seq_cst);
	std::erase_if(g_retired, [&](View* view) {
		if (view->retired > quiescent) return false;
		delete view;
		return true;
	});
}

void RetireView(View* view) {
	view->retired = g_retire_clock.fetch_add(1, std::memory_order_seq_cst) + 1;
	g_retired.push_back(view);
}

// The registers a table draw's evaluation and its pair key read, as the command processor's (SET_SH_REG and
// SET_SH_REG_INDIRECT through pm4Handlers.cpp's tables; a reset clears them).
struct Registers {
	uint64_t                 es = 0, ls = 0, hs = 0, ps = 0;
	std::array<uint32_t, 32> gs_user {}, ps_user {};
	int32_t                  start_instance = -1;

	void Set(uint32_t offset, uint32_t value) {
		const auto low  = [&](uint64_t& base) { base = (base & 0xFFFFFF00000000FFull) | uint64_t {value} << 8u; };
		const auto high = [&](uint64_t& base) { base = (base & 0xFFFF00FFFFFFFFFFull) | (uint64_t {value} & 0xffu) << 40u; };
		if (offset - Pm4::SPI_SHADER_USER_DATA_GS_0 < 32u) {
			gs_user[offset - Pm4::SPI_SHADER_USER_DATA_GS_0] = value;
			return;
		}
		if (offset - Pm4::SPI_SHADER_USER_DATA_PS_0 < 32u) {
			ps_user[offset - Pm4::SPI_SHADER_USER_DATA_PS_0] = value;
			return;
		}
		switch (offset) {
			case Pm4::SPI_SHADER_PGM_LO_ES: low(es); break;
			case Pm4::SPI_SHADER_PGM_HI_ES: high(es); break;
			case Pm4::SPI_SHADER_PGM_LO_LS: low(ls); break;
			case Pm4::SPI_SHADER_PGM_HI_LS: high(ls); break;
			case Pm4::SPI_SHADER_PGM_LO_HS: low(hs); break;
			case Pm4::SPI_SHADER_PGM_HI_HS: high(hs); break;
			case Pm4::SPI_SHADER_PGM_LO_PS: low(ps); break;
			case Pm4::SPI_SHADER_PGM_HI_PS: high(ps); break;
			default: break;
		}
	}
};

// The scout's own: its registers, its mapped-page cache, a ticket's scratch and each submission's draw count.
struct State {
	Registers                 registers;
	std::array<uint64_t, 256> mapped {};
	uint64_t                  mapping_epoch = UINT64_MAX;
	uint64_t                  constant      = 0; // the last submission with constant engine commands
	// The last submission the scout did not walk whole while the render thread was not past it (left at a wait, abandoned,
	// skipped while standing down, or never fed): a wait in its unwalked part, on a word the game's CPU writes, was never
	// seen, so no ticket is made for a later draw until the render thread is past it (PR #288).
	uint64_t                  fence         = 0;
	std::array<std::pair<uint64_t, uint32_t>, 256> draws {};     // (sequence, draw packets) of the ones walked whole
	std::array<std::vector<uint32_t>, 2>           blocks;
	std::vector<uint32_t>                          words, lod_stats;
	std::vector<std::array<uint64_t, 3>>           ranges; // address, bytes, read from the backing
	std::vector<uint64_t>                          pages;  // page << 1 | a range on it read as a guest page
	std::array<std::vector<std::pair<std::array<uint32_t, 4>, uint32_t>>, 2> entries;
};

// Draws between the render thread's position and draw `ordinal` of submission `sequence` (negative: the render thread
// reached it).
int64_t LeadOf(const State& state, uint64_t sequence, uint32_t ordinal) {
	const auto render = g_render.load(std::memory_order_acquire);
	const auto at     = render >> 24u;
	const auto count  = static_cast<uint32_t>(render & 0xffffffu);
	if (at > sequence) return -1;
	if (at == sequence) return int64_t {ordinal} - count;
	if (sequence - at > 64) return Lead + 1;
	int64_t lead = ordinal;
	for (auto s = at; s < sequence; ++s) {
		const auto& [walked, draws] = state.draws[s & 255u];
		if (walked != s) continue;
		lead += s == at ? int64_t {draws} - count : int64_t {draws};
	}
	return lead;
}

// The render thread passed the packets before draw `ordinal` of submission `sequence` (it reached that draw, or a later
// submission).
bool Passed(uint64_t sequence, uint32_t ordinal) {
	const auto render = g_render.load(std::memory_order_acquire);
	return (render >> 24u) > sequence || ((render >> 24u) == sequence && (render & 0xffffffu) > ordinal);
}

// A wait for the render thread: pause, then yield. False while the spin gate is shut (spin-gate.h: fewer than three of
// the process's CPUs idle): the scout stands down instead of holding a core the game's threads want.
bool Pause(uint32_t& spin) {
	if (!SpinGate::Open(false)) return false;
	if (++spin < 64) _mm_pause();
	else std::this_thread::yield();
	return true;
}

enum class Loss { None, Refused, Short, TooBig };

// TableEvaluate's and TableGather's flat paths on a view and the scout's registers, into a ticket.
Loss Evaluate(State& state, const View& view, Ticket& ticket) {
	auto& resources = *g_resources;
	const auto epoch = resources.MappingEpoch();
	if (epoch != state.mapping_epoch) {
		state.mapped.fill(UINT64_MAX);
		state.mapping_epoch = epoch;
	}
	ticket.mapping_epoch = epoch;
	// (Before any read: a host write after it stamps its pages later than this.)
	ticket.written = g_write_clock.load(std::memory_order_acquire);
	ticket.version       = view.version;
	ticket.key           = view.key;
	ticket.stages        = static_cast<uint16_t>(view.stages);
	ticket.user_data     = {state.registers.gs_user, state.registers.ps_user};
	ticket.user_words    = {view.stage[0].user_words, view.stage[1].user_words};
	ticket.slot_words    = {};
	ticket.buffers       = {};
	state.words.clear();
	state.lod_stats.clear();
	state.ranges.clear();
	const auto* granules = resources.ReadGranules();
	uint32_t    backed   = 0;
	const auto  locate   = [&](uint64_t low, uint64_t high, uint32_t offset, int32_t immediate, uint32_t count,
                            uint64_t& out, bool bounded, uint32_t records, bool& watched) {
        if (bounded) {
            const auto stride = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
            const auto size   = stride == 0 ? uint64_t {records} : uint64_t {stride} * records;
            if (immediate < 0 || static_cast<uint64_t>(immediate) + count * 4ull > size) return Loss::Refused;
        }
        const auto address = ((low | high << 32u) & 0x0000fffffffffffcull) + (offset & ~3u) +
                             static_cast<uint64_t>(int64_t {immediate});
        const auto end = address + count * 4ull;
        for (auto page = address >> 14u; page <= (end - 1) >> 14u; ++page) {
            auto& seen = state.mapped[page & 255u];
            if (seen == page) continue;
            if (!Libs::LibKernel::Memory::IsFullyMapped(page << 14u, 16384)) return Loss::Refused;
            seen = page;
        }
        // (NativeXprReader's test: a page with read watchers is the GPU thread's to read. Such a range is read
        // from the backing instead, for the render thread to prove at use.)
        watched = false;
        for (auto page = address >> 12u; page <= (end - 1) >> 12u; ++page) {
            const auto granule = page >> (PageManager::READ_GRANULE_BITS - 12u);
            if ((page << 12u) < TRACKER_ADDRESS_SIZE &&
                ((granules[granule / 64u].load(std::memory_order_acquire) >> (granule % 64u)) & 1u) == 0)
                continue;
            if (resources.HasReadWatchers(page << 12u, 4096)) {
                watched = true;
                break;
            }
        }
        out = address;
        return Loss::None;
	};
	for (uint32_t st = 0; st < view.stages; ++st) {
		const auto& stage = view.stage[st];
		auto&       slots = state.blocks[st];
		slots.resize(stage.block_dwords);
		const std::array<const uint32_t*, 3> bases {st == 0 ? state.registers.gs_user.data() : state.registers.ps_user.data(),
		                                            slots.data(), stage.constants.data()};
		const auto get = [&](uint16_t operand) { return bases[operand >> 14u][operand & 0x3fffu]; };
		uint32_t   begin = 0;
		for (const auto end: stage.level_ends) {
			const auto first_range = state.ranges.size();
			for (auto i = begin; i < end; ++i) {
				const auto& run     = stage.runs[i];
				const bool  bounded = run.records != Run::NotBounded;
				uint64_t    address = 0;
				bool        watched = false;
				if (const auto loss = locate(get(run.address[0]), get(run.address[1]), get(run.address[2]), run.immediate,
				                             run.count, address, bounded, bounded ? get(run.records) : 0, watched);
				    loss != Loss::None)
					return loss;
				state.ranges.push_back({address, run.count * 4ull, watched ? 1ull : 0ull});
				backed += watched ? 1u : 0u;
			}
			for (auto i = begin; i < end; ++i) {
				const auto& run   = stage.runs[i];
				const auto& range = state.ranges[first_range + (i - begin)];
				if (range[2] != 0 ? !Libs::LibKernel::Memory::TryReadBackingToHost(range[0], slots.data() + run.first, range[1])
				                  : !Read(slots.data() + run.first, range[0], range[1]))
					return Loss::Short;
			}
			begin = end;
		}
		const auto first = state.words.size();
		state.words.resize(first + stage.image_words.size());
		auto*  out = state.words.data() + first;
		size_t at  = 0;
		for (size_t i = 0; i < stage.image_counts.size(); ++i) {
			std::array<uint32_t, 8> image {};
			const uint32_t          count = stage.image_counts[i];
			for (uint32_t w = 0; w < count; ++w) {
				image[w]    = get(stage.image_words[at + w]);
				out[at + w] = image[w] & ~LodStatsMask[w];
			}
			at += count;
			if (i < stage.lod_stats_count) {
				state.lod_stats.push_back(count >= 8u ? LodStatsReport::ImageField(image.data()) : LodStatsReport::NoCounterFlag);
			}
		}
		for (; at < stage.image_words.size(); ++at) out[at] = get(stage.image_words[at]);
		auto& entries = state.entries[st];
		entries.resize(stage.buffers);
		for (uint32_t i = 0; i < stage.buffers; ++i) {
			auto& [words4, index] = entries[i];
			for (uint32_t w = 0; w < 4; ++w) words4[w] = get(stage.buffer_words[4 * i + w]);
			index = static_cast<uint32_t>(XXH3_64bits(words4.data(), sizeof(words4)) & (ResolvedEntries - 1));
		}
	}
	// The pages read, each once, in order (a page is the backing's when no range on it was read as a guest page).
	state.pages.clear();
	for (const auto& [address, bytes, watched]: state.ranges)
		for (auto page = address >> 12u; page <= (address + bytes - 1) >> 12u; ++page)
			state.pages.push_back(page << 1u | (watched != 0 ? 0u : 1u));
	std::sort(state.pages.begin(), state.pages.end());
	size_t kept = 0;
	for (const auto entry: state.pages) {
		if (kept != 0 && (state.pages[kept - 1] >> 1u) == (entry >> 1u)) state.pages[kept - 1] |= entry & 1u;
		else state.pages[kept++] = entry;
	}
	state.pages.resize(kept);
	// The ticket's data (Ticket::data's order), its slots in block order by segment.
	uint32_t need = static_cast<uint32_t>(state.words.size() + state.lod_stats.size() + 2 * state.pages.size() + 3 * backed);
	for (uint32_t st = 0; st < view.stages; ++st)
		need += view.stage[st].slot_words + 5 * view.stage[st].buffers + static_cast<uint32_t>(view.stage[st].segments.size());
	if (need > Ticket::Capacity || state.pages.empty()) return Loss::TooBig;
	auto* out = ticket.data;
	for (uint32_t st = 0; st < view.stages; ++st) {
		for (const auto& segment: view.stage[st].segments) {
			std::memcpy(out, state.blocks[st].data() + segment.first, segment.count * 4ull);
			out += segment.count;
		}
		ticket.slot_words[st] = static_cast<uint16_t>(view.stage[st].slot_words);
	}
	ticket.segments = {};
	for (uint32_t st = 0; st < view.stages; ++st) {
		for (const auto& segment: view.stage[st].segments) *out++ = segment.first | uint32_t {segment.count} << 16u;
		ticket.segments[st] = static_cast<uint16_t>(view.stage[st].segments.size());
	}
	out = std::copy(state.words.begin(), state.words.end(), out);
	out = std::copy(state.lod_stats.begin(), state.lod_stats.end(), out);
	for (uint32_t st = 0; st < view.stages; ++st) {
		for (const auto& [words4, index]: state.entries[st]) {
			out    = std::copy(words4.begin(), words4.end(), out);
			*out++ = index;
		}
		ticket.buffers[st] = static_cast<uint16_t>(view.stage[st].buffers);
	}
	for (const auto entry: state.pages) {
		const auto page = entry >> 1u;
		*out++          = static_cast<uint32_t>(page);
		*out++          = static_cast<uint32_t>(page >> 32u) | ((entry & 1u) != 0 ? 0u : Ticket::PageBacking);
	}
	// (A stage's ranges are its runs', in run order; their slots' words are at the runs' places in block order.)
	uint32_t base  = 0;
	size_t   range = 0;
	for (uint32_t st = 0; st < view.stages; ++st) {
		for (const auto& run: view.stage[st].runs) {
			const auto& [address, bytes, watched] = state.ranges[range++];
			if (watched != 0) {
				*out++ = static_cast<uint32_t>(address);
				*out++ = static_cast<uint32_t>(address >> 32u);
				*out++ = (base + run.at) | static_cast<uint32_t>(bytes / 4u) << 16u;
			}
		}
		base += view.stage[st].slot_words;
	}
	if (backed != 0) {
		LiveCounters::Add(LiveCounters::ScoutBackingRanges, backed);
		LiveCounters::Add(LiveCounters::ScoutBackingTickets);
	}
	ticket.backing     = static_cast<uint16_t>(backed);
	ticket.first_page  = state.pages.front() >> 1u;
	ticket.last_page   = state.pages.back() >> 1u;
	ticket.image_words = static_cast<uint16_t>(state.words.size());
	ticket.lod_stats   = static_cast<uint16_t>(state.lod_stats.size());
	ticket.pages       = static_cast<uint16_t>(state.pages.size());
	ticket.used        = static_cast<uint32_t>(out - ticket.data);
	return Loss::None;
}

// A submission's packets as ProcessPm4 takes them (indirect buffers followed in order, conditional execution and
// branches by the memory they test), its registers kept and a ticket made for each draw that may reach TableDraw.
// False: stopped.
bool Walk(State& state, const FeedEntry& entry) {
	const auto sequence = entry.sequence;
	if (entry.reset) state.registers = {};
	if (entry.constant) state.constant = sequence;
	LiveCounters::Add(LiveCounters::ScoutSubmissions);
	struct Level {
		uint64_t base = 0;
		uint32_t size = 0, offset = 0;
	};
	std::array<Level, 8> stack {};
	uint32_t             depth = 0;
	stack[depth++]             = {reinterpret_cast<uint64_t>(entry.commands), entry.size, 0};
	uint32_t                draws = 0;
	// The run of DRAW_INDEX_INDIRECT the last one belongs to (TryNativeXprDraws' rule): its length, its first packet's
	// words 2-4, and where the next packet of the run would be.
	uint32_t                run = 0;
	std::array<uint32_t, 3> run_words {};
	uint64_t                run_next = 0;
	const auto abandon = [&] {
		LiveCounters::Add(LiveCounters::ScoutAbandoned);
		state.fence = std::max(state.fence, sequence);
		return true;
	};
	// (The render thread past the submission: nothing of it is left to see.)
	const auto render_past = [&] {
		LiveCounters::Add(LiveCounters::ScoutAbandoned);
		return true;
	};
	// A wait it didn't finish: stopped (false), or the spin gate shut (the submission left, its later draws without
	// tickets: true).
	const auto leave = [&] {
		if (g_stop.load(std::memory_order_relaxed)) return false;
		LiveCounters::Add(LiveCounters::ScoutStoodDown);
		state.fence = std::max(state.fence, sequence);
		return true;
	};
	std::array<uint32_t, 64> packet {};
	while (depth != 0) {
		if (g_stop.load(std::memory_order_relaxed)) return false;
		auto& level = stack[depth - 1];
		if (level.offset >= level.size) {
			--depth;
			continue;
		}
		// (Its memory may be the game's again once the render thread is past it.)
		if ((g_render.load(std::memory_order_acquire) >> 24u) > sequence) return render_past();
		const auto at = level.base + uint64_t {level.offset} * 4u;
		if (!Read(packet.data(), at, 4)) return abandon();
		const auto header = packet[0];
		if (header == 0x80000000u) {
			++level.offset;
			continue;
		}
		if ((header >> 30u) != 3u) return abandon();
		const auto length = KYTY_PM4_LEN(header);
		if (length > level.size - level.offset) return abandon();
		const auto opcode = (header >> 8u) & 0xffu;
		level.offset += length;
		const auto body = [&](uint32_t dwords) { return dwords <= packet.size() && dwords <= length && Read(packet.data(), at, dwords * 4ull); };
		// Waits on words the stream wrote are passed (the GPU's queues syncing); at any other, and at a rewind (the game
		// may still be writing the commands after it), the scout waits until the render thread is past it (false: it
		// didn't).
		const auto stop_here = [&] {
			LiveCounters::Add(LiveCounters::ScoutWaitsStopped);
			for (uint32_t spin = 0; !Passed(sequence, draws);)
				if (g_stop.load(std::memory_order_relaxed) || !Pause(spin)) return false;
			return true;
		};
		if (opcode == Pm4::IT_SET_SH_REG) {
			if (length < 3) continue;
			uint32_t offset = 0;
			if (!Read(&offset, at + 4, 4)) return abandon();
			if (offset == Pm4::SH_NOP || offset >= Pm4::SH_NUM) continue;
			// (The user data parsers write the registers of their stage only: UserSgprWriteNum.)
			auto count = length - 2;
			if (offset - Pm4::SPI_SHADER_USER_DATA_GS_0 < 32u) count = std::min(count, 32u - (offset - Pm4::SPI_SHADER_USER_DATA_GS_0));
			if (offset - Pm4::SPI_SHADER_USER_DATA_PS_0 < 32u) count = std::min(count, 32u - (offset - Pm4::SPI_SHADER_USER_DATA_PS_0));
			for (uint32_t i = 0; i < count; i += packet.size()) {
				const auto n = std::min<uint32_t>(packet.size(), count - i);
				if (!Read(packet.data(), at + 8 + i * 4ull, n * 4ull)) return abandon();
				for (uint32_t j = 0; j < n; ++j) state.registers.Set(offset + i + j, packet[j]);
			}
		} else if (opcode == Pm4::IT_SET_SH_REG_INDIRECT) {
			if (length != 5 || !body(5)) return abandon();
			const auto table = (uint64_t {packet[1]} & 0xfffffffcu) | uint64_t {packet[2]} << 32u;
			const auto count = packet[4] & 0x3fffu;
			for (uint32_t i = 0; i < count; i += 32) {
				const auto n = std::min<uint32_t>(32, count - i);
				if (!Read(packet.data(), table + i * 8ull, n * 8ull)) break; // (its registers then differ: no ticket uses them)
				for (uint32_t j = 0; j < n; ++j) {
					const auto raw = packet[2 * j];
					if (raw == 0xffffffffu) continue;
					const auto offset = raw & ~0x70000000u;
					if (offset != Pm4::SH_NOP && offset < Pm4::SH_NUM) state.registers.Set(offset, packet[2 * j + 1]);
				}
			}
		} else if (opcode == Pm4::IT_INDIRECT_BUFFER) {
			if (length == 4) {
				if (!body(4)) return abandon();
				const auto address = uint64_t {packet[1]} | uint64_t {packet[2]} << 32u;
				const auto size    = packet[3] & 0xfffffu;
				if (size == 0) continue;
				if (address == 0 || depth == stack.size()) return abandon();
				stack[depth++] = {address, size, 0};
			} else if (length == 14) {
				if (!body(14)) return abandon();
				const auto compare = (uint64_t {packet[2]} & 0xfffffff8u) | uint64_t {packet[3]} << 32u;
				uint64_t   value   = 0;
				if (g_resources->HasReadWatchers(compare, 8) || !Read(&value, compare, 8)) return abandon();
				const auto mask = packet[4] | uint64_t {packet[5]} << 32u, reference = packet[6] | uint64_t {packet[7]} << 32u;
				const auto mode = packet[1] & 0x3u;
				const auto then_address = (uint64_t {packet[8]} & 0xfffffffcu) | uint64_t {packet[9]} << 32u;
				const auto else_address = (uint64_t {packet[11]} & 0xfffffffcu) | uint64_t {packet[12]} << 32u;
				const auto then_size = packet[10] & 0xfffffu, else_size = packet[13] & 0xfffffu;
				if (depth == stack.size()) return abandon();
				if (TestValue(value, reference, mask, (packet[1] >> 8u) & 0x7u)) stack[depth++] = {then_address, then_size, 0};
				else if (mode == 2 && else_size != 0) stack[depth++] = {else_address, else_size, 0};
			} else {
				return abandon();
			}
		} else if (opcode == Pm4::IT_COND_EXEC) {
			if (!body(5)) return abandon();
			const auto address = uint64_t {packet[2]} << 32u | (packet[1] & 0xfffffffcu);
			uint32_t   value   = 0;
			if (g_resources->HasReadWatchers(address, 4) || !Read(&value, address, 4)) return abandon();
			if (value == 0) level.offset = std::min(level.size, level.offset + (packet[4] & 0x3fffu));
		} else if (opcode == Pm4::IT_WAIT_REG_MEM || opcode == Pm4::IT_WAIT_REG_MEM_64) {
			if (!body(4)) return abandon();
			const bool wide    = opcode == Pm4::IT_WAIT_REG_MEM_64;
			const auto address = (packet[2] & ~(wide ? 0x7u : 0x3u)) | (uint64_t {packet[3]} & 0x3ffffu) << 32u;
			bool       written = g_labels[LabelSlot(address)].load(std::memory_order_relaxed) == address;
			if (wide) written = written && g_labels[LabelSlot(address + 4)].load(std::memory_order_relaxed) == address + 4;
			if (written) {
				LiveCounters::Add(LiveCounters::ScoutWaitsPassed);
			} else if (!stop_here()) {
				return leave();
			}
		} else if (opcode == Pm4::IT_REWIND) {
			LiveCounters::Add(LiveCounters::ScoutRewinds);
			if (!stop_here()) return leave();
		} else if (opcode == Pm4::IT_NOP && header == 0xC0001024u) {
			state.registers = {}; // (DISPATCH_RESET: the command processor's Reset)
		} else if (IsDraw(opcode)) {
			const auto ordinal = draws++;
			LiveCounters::Add(LiveCounters::ScoutDrawPackets);
			const bool indirect = opcode == Pm4::IT_DRAW_INDIRECT || opcode == Pm4::IT_DRAW_INDEX_INDIRECT ||
			                      opcode == Pm4::IT_DRAW_INDIRECT_MULTI || opcode == Pm4::IT_DRAW_INDEX_INDIRECT_MULTI;
			if (!body(std::min<uint32_t>(length, 10))) return abandon();
			state.registers.start_instance = indirect && length >= 4 ? GsUserSgprAt(packet[3] & 0xffffu) : -1;
			// What may reach TableDraw: the native draws' packets, a run's first only.
			if (header == 0xc0032500u) {
				const std::array<uint32_t, 3> words {packet[2], packet[3], packet[4]};
				if (run != 0 && run < 64 && at == run_next && words == run_words) {
					++run;
					run_next = at + 20;
					Note(sequence, ordinal, Reason::Continuation);
					continue;
				}
				run       = 1;
				run_words = words;
				run_next  = at + 20;
			} else {
				run = 0;
				if (header != 0xc0083800u && header != 0xc0042700u && header != 0xc0033500u && header != 0xc0012d00u) {
					Note(sequence, ordinal, Reason::NotNative);
					continue;
				}
			}
			const auto& registers = state.registers;
			if (registers.es == 0 || registers.ls != 0 || registers.hs != 0) {
				LiveCounters::Add(LiveCounters::ScoutNotTable);
				Note(sequence, ordinal, Reason::NotTable);
				continue;
			}
			// (No ticket while a submission with constant engine commands may be unfinished: its constant RAM dumps write
			// memory the stream's draws may read, without a label.)
			if (state.constant != 0 && !((g_render.load(std::memory_order_acquire) >> 24u) > state.constant)) {
				LiveCounters::Add(LiveCounters::ScoutConstant);
				Note(sequence, ordinal, Reason::Constant);
				continue;
			}
			// (Nor while a submission before it was not walked whole: State::fence.)
			if (state.fence != 0 && state.fence < sequence && !((g_render.load(std::memory_order_acquire) >> 24u) > state.fence)) {
				LiveCounters::Add(LiveCounters::ScoutFenced);
				Note(sequence, ordinal, Reason::Fenced);
				continue;
			}
			// At most Lead draws ahead; none for a draw the render thread reached.
			bool stalled = false;
			for (uint32_t spin = 0;;) {
				const auto lead = LeadOf(state, sequence, ordinal);
				if (lead < 0) break;
				if (lead <= Lead && g_ring_tail.load(std::memory_order_relaxed) - g_ring_head.load(std::memory_order_acquire) < RingSlots)
					break;
				stalled = true;
				if (g_stop.load(std::memory_order_relaxed) || !Pause(spin)) return leave();
			}
			if (stalled) LiveCounters::Add(LiveCounters::ScoutLeadStalls);
			if (LeadOf(state, sequence, ordinal) < 0) {
				LiveCounters::Add(LiveCounters::ScoutBehind);
				Note(sequence, ordinal, Reason::Behind);
				continue;
			}
			const std::array<uint64_t, 3> key {registers.es, registers.ps, static_cast<uint32_t>(registers.start_instance)};
			const auto* view = g_directory[XXH3_64bits(key.data(), sizeof(key)) & (DirectoryEntries - 1)].load(std::memory_order_acquire);
			if (view == nullptr || view->key != key) {
				LiveCounters::Add(view == nullptr ? LiveCounters::ScoutNoView : LiveCounters::ScoutNoViewOther);
				Note(sequence, ordinal, view == nullptr ? Reason::NoViewEmpty : Reason::NoViewOther);
				Quiesce();
				continue;
			}
			const auto tail   = g_ring_tail.load(std::memory_order_relaxed);
			auto&      ticket = g_ring[tail % RingSlots];
			ticket.sequence   = sequence;
			ticket.ordinal    = ordinal;
			ticket.packet     = at;
			const auto loss   = Evaluate(state, *view, ticket);
			Quiesce();
			switch (loss) {
				case Loss::None:
					g_ring_tail.store(tail + 1, std::memory_order_release);
					LiveCounters::Add(LiveCounters::ScoutTickets);
					LiveCounters::Add(LiveCounters::ScoutTicketWords, ticket.used);
					Note(sequence, ordinal, Reason::Ticket);
					break;
				case Loss::Refused: LiveCounters::Add(LiveCounters::ScoutRefused), Note(sequence, ordinal, Reason::Refused); break;
				case Loss::Short: LiveCounters::Add(LiveCounters::ScoutShort), Note(sequence, ordinal, Reason::Short); break;
				case Loss::TooBig: LiveCounters::Add(LiveCounters::ScoutTooBig), Note(sequence, ordinal, Reason::TooBig); break;
			}
		}
	}
	state.draws[sequence & 255u] = {sequence, draws};
	return true;
}

void Main() {
	KYTY_PROFILER_THREAD("Thread_Scout");
	auto     state = std::make_unique<State>();
	uint64_t last  = 0; // the last submission fed (a gap: Feed dropped one on a full ring)
	auto     report = std::chrono::steady_clock::now();
	for (uint32_t spin = 0;;) {
		Quiesce();
		// (The counters every 10 s: the emulator is usually killed, not stopped.)
		if (spin == 0 && std::chrono::steady_clock::now() - report > std::chrono::seconds(10)) {
			report = std::chrono::steady_clock::now();
			LiveCounters::Report();
		}
		const auto wake = g_wake.load(std::memory_order_acquire);
		if (g_stop.load(std::memory_order_acquire)) return;
		const auto head = g_feed_head.load(std::memory_order_relaxed);
		if (head == g_feed_tail.load(std::memory_order_acquire)) {
			if (++spin < 2048 && Pause(spin)) continue;
			g_wake.wait(wake, std::memory_order_acquire);
			spin = 0;
			continue;
		}
		spin             = 0;
		const auto entry = g_feed[head % FeedEntries];
		g_feed_head.store(head + 1, std::memory_order_release);
		if (last != 0 && entry.sequence > last + 1) state->fence = std::max(state->fence, entry.sequence - 1);
		last = entry.sequence;
		// (A submission the render thread is past: its commands may be the game's again; its registers are lost. While
		// the spin gate is shut, every submission: the scout stands down, its thread asleep between feeds.)
		const bool shut = !SpinGate::Open(false);
		if (shut || (g_render.load(std::memory_order_acquire) >> 24u) > entry.sequence) {
			if (!((g_render.load(std::memory_order_acquire) >> 24u) > entry.sequence)) state->fence = std::max(state->fence, entry.sequence);
			if (entry.reset) state->registers = {};
			if (entry.constant) state->constant = entry.sequence;
			LiveCounters::Add(shut ? LiveCounters::ScoutStoodDown : LiveCounters::ScoutAbandoned);
			continue;
		}
		if (!Walk(*state, entry)) return;
	}
}

} // namespace

bool Running() noexcept {
	return g_running.load(std::memory_order_relaxed);
}

void Start(RenderContext& resources) {
	if (Mode() == 0 || Running()) return;
	g_resources = &resources;
	g_stop.store(false);
	g_running.store(true);
	if (Mode() == 3) {
		// (The resolve thread evaluates: EvaluateAhead. Running: the host writes are noted and the views published.)
		return;
	}
	g_thread = new std::thread(Main);
	std::printf("Scout: on (KYTY_SCOUT=%u)\n", Mode());
}

void Stop() {
	if (!Running()) return;
	g_stop.store(true, std::memory_order_seq_cst);
	g_wake.fetch_add(1, std::memory_order_seq_cst);
	g_wake.notify_all();
	if (g_thread != nullptr && g_thread->joinable()) g_thread->join();
	delete g_thread;
	g_thread = nullptr;
	g_running.store(false);
	for (uint32_t i = 0; i < LiveCounters::Count; ++i)
		if (const auto n = LiveCounters::g_counts[i].load(std::memory_order_relaxed); n != 0)
			std::printf("Scout: %s %llu\n", LiveCounters::Names[i], static_cast<unsigned long long>(n));
	for (auto& entry: g_directory) delete entry.exchange(nullptr);
	for (auto* view: g_retired) delete view;
	g_retired.clear();
}

bool EvaluateAhead(const std::array<uint64_t, 3>& key, uint64_t key_hash, std::span<const uint32_t> gs_user,
                   std::span<const uint32_t> ps_user, Ticket& ticket) {
	static auto* const state = new State {}; // (the resolve thread's)
	bool               made  = false;
	const auto*        view  = g_directory[key_hash & (DirectoryEntries - 1)].load(std::memory_order_seq_cst);
	if (view != nullptr && view->key == key) {
		auto& registers   = state->registers;
		registers.gs_user = {};
		registers.ps_user = {};
		std::copy_n(gs_user.begin(), std::min<size_t>(gs_user.size(), registers.gs_user.size()), registers.gs_user.begin());
		std::copy_n(ps_user.begin(), std::min<size_t>(ps_user.size(), registers.ps_user.size()), registers.ps_user.begin());
		ticket.sequence = 0;
		ticket.ordinal  = 0;
		ticket.packet   = 0;
		made            = Evaluate(*state, *view, ticket) == Loss::None;
	}
	// (No view is held past this point: the ones retired meanwhile may go.)
	Quiesce();
	return made;
}

void Feed(uint64_t sequence, std::span<const uint32_t> commands, bool constant, bool reset) {
	if (!Running() || Mode() == 3) return;
	const auto tail = g_feed_tail.load(std::memory_order_relaxed);
	if (tail - g_feed_head.load(std::memory_order_acquire) >= FeedEntries) {
		LiveCounters::Add(LiveCounters::ScoutFeedFull);
		return;
	}
	g_feed[tail % FeedEntries] = {sequence, commands.data(), static_cast<uint32_t>(commands.size()), constant, reset};
	g_feed_tail.store(tail + 1, std::memory_order_release);
	g_wake.fetch_add(1, std::memory_order_release);
	g_wake.notify_one();
}

void BeginSubmission(uint64_t sequence) {
	g_render_sequence = sequence;
	g_render_count    = 0;
	g_render_packet   = nullptr;
	g_render.store(sequence << 24u, std::memory_order_release);
	FreeRetired();
}

void NoteDraw(const uint32_t* packet) {
	g_render_packet = packet;
	g_render.store(g_render_sequence << 24u | ++g_render_count, std::memory_order_release);
}

void NoteDrawAt(const uint32_t* packet, uint64_t sequence, uint32_t ordinal) {
	if (sequence != g_render_sequence) {
		g_render_sequence = sequence;
		FreeRetired();
	}
	g_render_count  = ordinal + 1;
	g_render_packet = packet;
	g_render.store(sequence << 24u | g_render_count, std::memory_order_release);
}

void SkipDraws(uint32_t count) {
	g_render_count += count;
	g_render.store(g_render_sequence << 24u | g_render_count, std::memory_order_release);
}

void NoteWrite(const void* address, uint32_t size, const void* values) {
	const uint64_t first = reinterpret_cast<uint64_t>(address) & ~uint64_t {3};
	const auto     end   = std::min(reinterpret_cast<uint64_t>(address) + size, first + uint64_t {64 * 4});
	const bool keep  = Mode() == 2;
	for (auto word = first; word < end; word += 4) {
		g_labels[LabelSlot(word)].store(word, std::memory_order_relaxed);
		if (keep) {
			uint32_t value = 0;
			const auto  begin  = reinterpret_cast<uint64_t>(address);
			const bool  inside = values != nullptr && word >= begin && word + 4 <= begin + size;
			const auto* from   = inside ? static_cast<const uint8_t*>(values) + (word - begin)
			                            : reinterpret_cast<const uint8_t*>(word);
			std::memcpy(&value, from, sizeof(value));
			g_label_values[LabelSlot(word)] = value;
		}
	}
	NoteHostWrite(reinterpret_cast<uint64_t>(address), size);
}

void CheckWait(const void* address, uint32_t size) {
	const auto first = reinterpret_cast<uint64_t>(address);
	for (auto word = first; word < first + size; word += 4)
		if (g_labels[LabelSlot(word)].load(std::memory_order_relaxed) != word) {
			LiveCounters::Add(LiveCounters::ScoutWaitOther);
			return;
		}
	LiveCounters::Add(LiveCounters::ScoutWaitChecks);
	for (auto word = first; word < first + size; word += 4) {
		const auto held = *reinterpret_cast<const uint32_t*>(word), wrote = g_label_values[LabelSlot(word)];
		if (held == wrote) continue;
		LiveCounters::Add(LiveCounters::ScoutWaitChanged);
		static uint32_t reports = 0;
		if (reports++ < 64)
			std::printf("Scout: wait 0x%llx (%u bytes): word 0x%llx holds 0x%08x, the command processor wrote 0x%08x; "
			            "submission %llu draw %u\n",
			            static_cast<unsigned long long>(first), size, static_cast<unsigned long long>(word), held, wrote,
			            static_cast<unsigned long long>(g_render_sequence), g_render_count);
		return;
	}
}

void NoteHostWrite(uint64_t address, uint64_t size, bool write_back) {
	if (size == 0 || !Running()) return;
	if (write_back) LiveCounters::Add(LiveCounters::ScoutWriteBacks);
	// (After the write: a scout that reads the clock at or past this value reads the bytes written.)
	const auto clock = g_write_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
	auto&      entry = g_writes[clock % WriteEntries];
	// (An entry a newer or unfinished write holds is left to it: a ticket that would look for this one finds another
	// clock there, and is refused.)
	for (auto old = entry.clock.load(std::memory_order_relaxed);;) {
		if ((old & HostWrite::Busy) != 0 || old >= clock) return;
		if (entry.clock.compare_exchange_weak(old, clock | HostWrite::Busy, std::memory_order_acquire,
		                                      std::memory_order_relaxed))
			break;
	}
	std::atomic_thread_fence(std::memory_order_release);
	entry.first.store(address >> 12u, std::memory_order_relaxed);
	entry.last.store((address + size - 1) >> 12u | (write_back ? HostWrite::WriteBack : 0), std::memory_order_relaxed);
	entry.clock.store(clock, std::memory_order_release);
}

bool WrittenSince(const Ticket& ticket, bool& write_back) {
	const auto now = g_write_clock.load(std::memory_order_acquire);
	if (now == ticket.written) return false;
	if (now - ticket.written >= WriteEntries) return true;
	const auto* pages = Pages(ticket);
	for (auto clock = ticket.written + 1; clock <= now; ++clock) {
		const auto& entry = g_writes[clock % WriteEntries];
		if (entry.clock.load(std::memory_order_acquire) != clock) return true;
		const auto first = entry.first.load(std::memory_order_relaxed);
		const auto last  = entry.last.load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		if (entry.clock.load(std::memory_order_relaxed) != clock) return true;
		const auto last_page = last & ~HostWrite::WriteBack;
		if (last_page < ticket.first_page || first > ticket.last_page) continue;
		for (uint32_t i = 0; i < ticket.pages; ++i) {
			const auto page = PageAt(pages, i);
			if (page > last_page) break; // (in order)
			if (page >= first) {
				write_back = (last & HostWrite::WriteBack) != 0;
				return true;
			}
		}
	}
	return false;
}

Reason CurrentReason() {
	const auto ordinal = g_render_count - 1;
	const auto noted   = g_reasons[ordinal & (ReasonEntries - 1)].load(std::memory_order_relaxed);
	if ((noted >> 40u) != (g_render_sequence & 0xffffffu) || ((noted >> 8u) & 0xffffffffu) != ordinal)
		return Reason::Unwalked;
	return static_cast<Reason>(noted & 0xffu);
}

uint64_t NextVersion() {
	return ++g_version;
}

void Publish(View* view, uint64_t key_hash) {
	if (auto* old = g_directory[key_hash & (DirectoryEntries - 1)].exchange(view, std::memory_order_seq_cst)) RetireView(old);
	FreeRetired();
}

void Retire(uint64_t key_hash, uint64_t version) {
	auto& entry = g_directory[key_hash & (DirectoryEntries - 1)];
	auto* view  = entry.load(std::memory_order_acquire);
	if (view != nullptr && view->version == version && entry.compare_exchange_strong(view, nullptr)) RetireView(view);
}

const Ticket* Current() {
	const auto    sequence = g_render_sequence;
	const auto    ordinal  = g_render_count - 1;
	auto          head     = g_ring_head.load(std::memory_order_relaxed);
	const Ticket* found    = nullptr;
	bool          later    = false; // (a ticket of a later draw: no need to ask for the tail again)
	// (The tail only when the tickets known are used up.)
	for (uint32_t pass = 0; pass < 2 && !later; ++pass) {
		if (pass == 1) g_known_tail = g_ring_tail.load(std::memory_order_acquire);
		for (; head != g_known_tail; ++head) {
			const auto& ticket = g_ring[head % RingSlots];
			if (ticket.sequence > sequence || (ticket.sequence == sequence && ticket.ordinal >= ordinal)) {
				if (ticket.sequence == sequence && ticket.ordinal == ordinal) found = &ticket;
				later = true;
				break;
			}
		}
	}
	g_ring_head.store(head, std::memory_order_release);
	// The next ticket's lines (the scout's core wrote them) load during the draws until it is used.
	const auto* next = reinterpret_cast<const char*>(&g_ring[(head + 1) % RingSlots]);
	for (uint32_t line = 0; line < 12; ++line) __builtin_prefetch(next + 64 * line);
	return found;
}

uint64_t CurrentPacket() {
	return reinterpret_cast<uint64_t>(g_render_packet);
}

} // namespace Libs::Graphics::Scout
