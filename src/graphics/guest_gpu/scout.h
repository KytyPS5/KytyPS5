#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_SCOUT_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_SCOUT_H_

// The scout (KYTY_SCOUT): a thread that walks queue 0's graphics submissions as they are queued, ahead of
// the render thread, keeps the shader registers a table draw's evaluation reads (the stages' programs and user data)
// and evaluates the tables of each table draw whose pair it has a view of (the flat form TablePack made): the slots,
// the T#/S# words, the pixel stage's LOD statistics words and the V#s with their resolved entries' indices, as
// TableEvaluate's and TableGather's flat paths make them. The render thread takes that result as a ticket at TableDraw,
// checked against its own registers, the pair, the mapping epoch and the tables' pages (no read watchers), instead of
// evaluating. Not checked at use: the game's tables themselves, read by the scout before the render thread reaches the
// draw (a CPU write the GPU reads sits behind a wait the scout stops at; the verifier, 2, counts any difference).
// 0: off (no thread, nothing fed; the Steam Deck builds' default). 1 (the default): tickets used. 2: verifier: the
// render thread evaluates as without the scout and compares every ticket with its own result.
// The scout spins while it waits for the render thread only while the spin gate is open (spin-gate.h,
// KYTY_SPIN_GATE): while it is shut the scout stands down, leaving its submissions unwalked, and sleeps between feeds.
// A run on a page with read watchers is read from the backing instead of being refused, and its range goes into the
// ticket; at use the render thread proves each such range clean (TryReadGpuCleanBackingToHost, as its own read of the
// page would take it) and equal to the ticket's words.
// A ticket's slot words are in block order with the stages' segments (runs side by side in the block), one inline copy
// a segment (Ticket::segments; none: a stage without slots).

#include "graphics/guest_gpu/pm4.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

extern "C" {
extern volatile std::atomic_uint32_t kyty_local_scout_mode;
}

namespace Libs::Graphics {
class RenderContext;
using GpuResourceManager = RenderContext; // (chenxiao07's name for what the scout reads the mappings and watchers from)
}

namespace Libs::Graphics::Scout {

[[nodiscard]] inline uint32_t Mode() {
	return kyty_local_scout_mode.load(std::memory_order_relaxed);
}
// The thread runs (Start without a stop since).
[[nodiscard]] bool Running() noexcept;

// GPU thread: after the performance switches (nothing without KYTY_SCOUT), and before the thread returns.
void Start(RenderContext& resources);
void Stop();

// GuestGpu::Enqueue (the queue mutex held): a graphics submission of queue 0 with its sequence number, whether it has
// constant engine commands, and whether the command processor is reset before it (its own or a flip preparation's).
void Feed(uint64_t sequence, std::span<const uint32_t> commands, bool constant, bool reset);

// The packets that are draws (the ones whose completion ProcessPm4 counts): both sides number a submission's draws by
// them.
[[nodiscard]] constexpr bool IsDraw(uint32_t opcode) {
	return opcode == Pm4::IT_DRAW_INDEX_2 || opcode == Pm4::IT_DRAW_INDEX_OFFSET_2 || opcode == Pm4::IT_DRAW_INDEX_AUTO ||
	       opcode == Pm4::IT_DRAW_INDIRECT || opcode == Pm4::IT_DRAW_INDEX_INDIRECT ||
	       opcode == Pm4::IT_DRAW_INDIRECT_MULTI || opcode == Pm4::IT_DRAW_INDEX_INDIRECT_MULTI;
}

// Render thread, queue 0's command processor: a graphics submission's first slice; each draw packet (predicated ones
// too, in the order the stream has them); the packets after the first of a run one call took.
void BeginSubmission(uint64_t sequence);
void NoteDraw(const uint32_t* packet);
void SkipDraws(uint32_t count);
// The same for a pipelined command processor, whose draws execute on another thread than the one that numbers them: the
// execution thread, at each draw operation, names the draw it is at by its submission and its number in it (the draw
// packets the command processor passed in the submission before it, predicated ones too).
void NoteDrawAt(const uint32_t* packet, uint64_t sequence, uint32_t ordinal);
// A command processor write to guest memory (WriteGuestMemory): a wait on one of these words is the stream's own.
// `values`: the written bytes, when guest memory does not show them yet (a guarded write: KYTY_WRITE_GUARD); null: read
// at `address`.
void NoteWrite(const void* address, uint32_t size, const void* values = nullptr);
// KYTY_SCOUT=2, the verifier: a met WAIT_REG_MEM of queue 0 on `size` bytes at `address`. When the
// scout's rule passes it (every word noted by NoteWrite), any word that holds another value than the command
// processor's last is counted and reported: another writer (the game's CPU) changed a word the scout doesn't wait on.
void CheckWait(const void* address, uint32_t size);
// After the emulator wrote guest memory itself, on any thread, as GPU work earlier in the stream would on the console
// (labels, the game's linear copies, buffer fills, constant RAM dumps; `write_back`: GPU results copied back, by
// readbacks, downloads and image write-backs): the write goes into a ring of recent writes with the next
// value of the write clock. A ticket read before such a write over one of its pages is not used (WrittenSince).
void NoteHostWrite(uint64_t address, uint64_t size, bool write_back = false);
// NoteHostWrite where the scout may be off (one relaxed load then).
inline void NoteHostWriteIfOn(uint64_t address, uint64_t size, bool write_back = false) {
	if (Mode() != 0) NoteHostWrite(address, size, write_back);
}

// A pair's flat form as TablePair::PackedRun has it, and where its slots' words are among the stage's in block order.
struct Run {
	std::array<uint16_t, 3>   address {}; // low, high, offset
	uint16_t                  records = NotBounded;
	int32_t                   immediate = 0;
	uint16_t                  first = 0, count = 0;
	uint16_t                  at = 0;
	static constexpr uint16_t NotBounded = 0xffff;
};
// Runs side by side in a stage's block, in block order: a ticket's slot words are its segments' in turn.
struct Segment {
	uint16_t first = 0, count = 0;
};

// An immutable copy of what a table pair's flat evaluation reads (TablePack publishes it): the scout never touches a
// TablePair, a program or a plan. Replaced when the pair is packed again, retired when it is reset; freed once the
// scout passed a point at which it held none (the retire stamp at or below its quiescent stamp).
struct View {
	std::array<uint64_t, 3> key {};     // vs, ps, start instance SGPR (TableTry's pair key)
	uint64_t                version = 0; // TablePair::scout_version
	uint32_t                stages  = 0;
	struct Stage {
		std::vector<Run>      runs;
		std::vector<Segment>  segments;
		std::vector<uint16_t> level_ends, image_words, buffer_words;
		std::vector<uint8_t>  image_counts;
		std::vector<uint32_t> constants;
		uint32_t              user_words = 0, block_dwords = 0, lod_stats_count = 0, buffers = 0, slot_words = 0;
	};
	std::array<Stage, 2> stage;
	uint64_t             retired = 0;
};
[[nodiscard]] uint64_t NextVersion();
// Render thread: the view replaces any other in its directory entry (by the key's hash).
void Publish(View* view, uint64_t key_hash);
// Render thread: the pair's view leaves the directory (its pair was reset).
void Retire(uint64_t key_hash, uint64_t version);

// TableXpr's resolved entries (TableXpr::ResolvedEntries is this): a ticket's entry index is below it, and the apply
// takes &table.resolved[index] without a check.
inline constexpr uint32_t ResolvedEntries = 65536;
static_assert(std::has_single_bit(ResolvedEntries));

// A draw's evaluation (the render thread's data: TableXpr's blocks, words, lod_stats and entries).
struct alignas(64) Ticket {
	uint64_t                                sequence = 0;
	uint32_t                                ordinal  = 0;
	uint32_t                                used     = 0; // data words
	uint64_t                                packet   = 0;
	uint64_t                                version  = 0;
	std::array<uint64_t, 3>                 key {};
	uint64_t                                mapping_epoch = 0;
	uint64_t                                written       = 0; // the write clock before the scout read its tables
	uint64_t                                first_page = 0, last_page = 0; // its pages' bounds (4 KiB pages)
	std::array<std::array<uint32_t, 32>, 2> user_data {};
	std::array<uint32_t, 2>                 user_words {};
	std::array<uint16_t, 2>                 slot_words {}, buffers {};
	uint16_t                                image_words = 0, lod_stats = 0, pages = 0, stages = 0;
	uint16_t                                backing = 0; // ranges read from the backing
	std::array<uint16_t, 2>                 segments {};
	// Slots of stage 0's runs then stage 1's (by segment, in block order), each segment (first slot |
	// word count << 16, stage 0's then stage 1's), the T#/S# words, the LOD statistics words, each buffer's four V#
	// words and its entry's index (stage 0's, then stage 1's), each 4 KiB page read once, in order (low, high;
	// PageBacking in high when every range on it was read from the backing), then each range read from the backing
	// (address low, high, its slots' offset in data | word count << 16).
	static constexpr uint32_t Capacity = 928;
	static constexpr uint32_t PageBacking = 0x80000000u;
	uint32_t                  data[Capacity];
};
static_assert(sizeof(Ticket) <= 4096);
// The ticket's pages (page number low, high each), after the rest of its data.
[[nodiscard]] inline const uint32_t* Pages(const Ticket& ticket) {
	return ticket.data + ticket.slot_words[0] + ticket.slot_words[1] + ticket.segments[0] + ticket.segments[1] +
	       ticket.image_words + ticket.lod_stats + 5u * (ticket.buffers[0] + ticket.buffers[1]);
}
// Page `i` of `pages` (Pages), without its PageBacking bit.
[[nodiscard]] inline uint64_t PageAt(const uint32_t* pages, uint32_t i) {
	return pages[2 * i] | uint64_t {pages[2 * i + 1] & ~Ticket::PageBacking} << 32u;
}
// Render thread: a host write since the ticket's clock covers one of its pages, or one isn't known yet (still being
// noted), or the ring of writes wrapped since. `write_back`: the write that covers a page was a write-back.
[[nodiscard]] bool WrittenSince(const Ticket& ticket, bool& write_back);

// Render thread, at TableDraw: the ticket of its current draw (tickets of earlier draws dropped), null when the scout
// made none; and the draw's packet (the ticket's identity).
[[nodiscard]] const Ticket* Current();
[[nodiscard]] uint64_t      CurrentPacket();
// KYTY_SCOUT=3, this tree's pipelined GPU: no scout thread and no walk of the stream. The resolve thread, which takes
// every operation in order with the registers it executes with, evaluates a table draw's ticket from the draw's own
// shader registers (`key`: TableTry's pair key, `key_hash` its hash; the stages' user data) into `ticket`, a slot of
// the pipeline's own ring that the operation carries to the execution thread. The host write clock and the views'
// directory are the scout's. False: no view of the pair, or an evaluation the flat path does not make.
[[nodiscard]] bool EvaluateAhead(const std::array<uint64_t, 3>& key, uint64_t key_hash, std::span<const uint32_t> gs_user,
                                 std::span<const uint32_t> ps_user, Ticket& ticket);

// Why the scout made no ticket for a draw (it notes one per draw packet it reached).
enum class Reason : uint8_t {
	Unwalked,     // none noted: the scout didn't reach the draw (a submission it left), or another draw's note replaced it
	Ticket,       // it made one
	NotNative,    // a packet the native paths don't take
	Continuation, // a run's packet after its first
	NotTable,     // tessellation or no vertex program
	Constant,     // a submission with constant engine commands may be unfinished
	Behind,       // the render thread reached it first
	NoViewEmpty,  // no view in the pair's directory entry
	NoViewOther,  // another pair's view there
	Refused,      // unmapped, or out of a bounded run's range
	Short,        // a guarded copy stopped short
	TooBig,       // the ticket would not fit
	Fenced,       // a submission before it was not walked whole (State::fence)
	Count
};
[[nodiscard]] Reason CurrentReason();

} // namespace Libs::Graphics::Scout

#endif /* EMULATOR_SRC_GRAPHICS_GUEST_GPU_SCOUT_H_ */
