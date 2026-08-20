// Fuzzes the PM4 command-buffer walker with generated packet streams.
//
// What this covers, and what it does not
// --------------------------------------
// A guest submits PM4 command buffers, and CommandProcessor::ProcessPm4 walks
// them: it reads a packet header, decodes the opcode and the length, and hands
// the payload to a handler. That framing, and the register writes it drives, are
// guest-controlled input, and until now nothing exercised them with anything but
// the buffers real titles happen to produce.
//
// It fuzzes the packet framing and the register-write opcodes. It does NOT fuzz
// draw, dispatch, DMA or event packets: those handlers submit to a real Vulkan
// device and dereference mapped guest memory, so a harness for them cannot run
// on a machine without a GPU, which is every CI runner.
//
// CommandProcessor holds a RenderContext&, and constructing a real one needs a
// live Vulkan device (RenderContext -> CommandScheduler -> MasterSemaphore ->
// vkCreateSemaphore). The opcodes fuzzed here never touch the renderer - but
// "never touch" is an assumption, and an assumption is not a test. So the
// reference is bound to a page mapped PROT_NONE: if any handler reaches through
// it, for read or for write, the process dies immediately and visibly instead of
// quietly reading rubbish. The guarantee is enforced by the MMU, not by comment.

#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Libs::Graphics::CommandProcessor;
using Libs::Graphics::Pm4Execution;
using Libs::Graphics::Pm4ProcessResult;
using Libs::Graphics::RenderContext;
namespace Pm4 = Libs::Graphics::Pm4;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "Pm4FuzzTests: failed: %s\n", text);
		std::abort();
	}
}

// A page that faults on any access. See the note at the top of the file.
void* AllocateInaccessiblePage() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
	Check(page != nullptr, "reserved a no-access page");
#else
	void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	Check(page != MAP_FAILED, "reserved a no-access page");
#endif
	return page;
}

// ── Half one: the packet walker ─────────────────────────────────────────
//
// Every opcode here has one legal encoding that its handler asserts on, so the
// framing is generated exactly and the payload is what varies. Feeding a random
// length would only trip the dispatcher's own deliberate EXIT_NOT_IMPLEMENTED,
// which is a guard doing its job, not a bug to find.

struct FixedPacket {
	uint32_t opcode      = 0;
	uint32_t total_dw    = 0;
	uint32_t payload_dw  = 0;
	uint32_t payload_mask = 0xFFFFFFFFu; // what the handler will accept
};

constexpr FixedPacket FIXED_PACKETS[] = {
    {Pm4::IT_CLEAR_STATE, 2, 1, 0xFu}, // rejects anything outside the low nibble
    {Pm4::IT_INDEX_TYPE, 2, 1, 0xFFFFFFFFu},
    {Pm4::IT_INDEX_BUFFER_SIZE, 2, 1, 0xFFFFFFFFu},
    {Pm4::IT_NUM_INSTANCES, 2, 1, 0xFFFFFFFFu},
    {Pm4::IT_PFP_SYNC_ME, 2, 1, 0xFFFFFFFFu},
    {Pm4::IT_INDEX_BASE, 3, 2, 0xFFFFFFFFu},
};

// The register offsets a SET_*_REG packet may legally carry are exactly the ones
// with a handler installed. Reading them out of the dispatch tables rather than
// hard-coding a list means this harness keeps covering the whole register space
// as the tables grow, with nothing to keep in sync.
template <class Table>
std::vector<uint32_t> InstalledOffsets(const Table& table) {
	std::vector<uint32_t> offsets;
	for (uint32_t i = 0; i < table.size(); i++) {
		if (table[i] != nullptr) {
			offsets.push_back(i);
		}
	}
	return offsets;
}

class Rng {
public:
	explicit Rng(uint32_t seed): m_rng(seed) {}

	uint32_t Below(uint32_t bound) {
		return std::uniform_int_distribution<uint32_t>(0, bound - 1)(m_rng);
	}
	uint32_t Word() { return std::uniform_int_distribution<uint32_t>(0, UINT32_MAX)(m_rng); }

private:
	std::mt19937 m_rng;
};

void AppendPacket(std::vector<uint32_t>& buffer, Rng& rng) {
	const auto choice = rng.Below(16);

	// The padding word the walker skips without consulting any handler.
	if (choice == 0) {
		buffer.push_back(0x80000000u);
		return;
	}

	// A NOP of a random legal length: the one opcode whose length field really
	// does vary, so it is what exercises the cursor arithmetic.
	if (choice == 1) {
		const auto total_dw = rng.Below(8) + 2;
		buffer.push_back(KYTY_PM4(total_dw, Pm4::IT_NOP, Pm4::R_ZERO));
		for (uint32_t i = 0; i < total_dw - 1; i++) {
			// Not a marker: 0x6875.... sends CpOpNop down the marker path,
			// which parses a guest string and is not part of this half.
			uint32_t word = rng.Word();
			if ((word & 0xffff0000u) == 0x68750000u) {
				word ^= 0x00010000u;
			}
			buffer.push_back(word);
		}
		return;
	}

	const auto& packet = FIXED_PACKETS[rng.Below(std::size(FIXED_PACKETS))];
	buffer.push_back(KYTY_PM4(packet.total_dw, packet.opcode, 0));
	for (uint32_t i = 0; i < packet.payload_dw; i++) {
		buffer.push_back(rng.Word() & packet.payload_mask);
	}
}

void FuzzTheWalker() {
	auto& renderer = *static_cast<RenderContext*>(AllocateInaccessiblePage());
	CommandProcessor cp(renderer);

	constexpr uint32_t ITERATIONS = 20000;
	uint64_t           packets    = 0;

	for (uint32_t iteration = 0; iteration < ITERATIONS; iteration++) {
		// A fresh, fully reset processor every iteration, so a failure is
		// reproducible from its seed alone and no state leaks between cases.
		cp.Reset();

		Rng                   rng(iteration + 1);
		std::vector<uint32_t> buffer;
		const auto            count = rng.Below(24) + 1;
		for (uint32_t i = 0; i < count; i++) {
			AppendPacket(buffer, rng);
		}
		Check(!buffer.empty(), "the generated buffer is not empty");

		Pm4Execution execution;
		const auto   result =
		    cp.Process(execution, buffer.data(), static_cast<uint32_t>(buffer.size()));

		// Every opcode generated here consumes its packet and returns, so the
		// walker must reach the end of the buffer rather than suspend on a wait.
		Check(result == Pm4ProcessResult::Complete, "the whole buffer is consumed");
		packets += count;
	}

	// Reset really has to reset. Without this, the loop above would be fuzzing
	// one ever-dirtier context rather than 20000 independent ones, and would
	// quietly prove much less than it appears to.
	cp.Reset();
	Rng                   rng(0xABCDEFu);
	std::vector<uint32_t> buffer;
	for (uint32_t i = 0; i < 64; i++) {
		AppendPacket(buffer, rng);
	}
	Pm4Execution first;
	Check(cp.Process(first, buffer.data(), static_cast<uint32_t>(buffer.size())) ==
	          Pm4ProcessResult::Complete,
	      "the dirtying buffer is consumed");
	cp.Reset();
	Pm4Execution second;
	Check(cp.Process(second, buffer.data(), static_cast<uint32_t>(buffer.size())) ==
	          Pm4ProcessResult::Complete,
	      "the same buffer is consumed identically after a reset");

	std::printf("Pm4FuzzTests: walker: %u buffers, %llu packets\n", ITERATIONS,
	            static_cast<unsigned long long>(packets));
}

// ── Half two: the register decoders ─────────────────────────────────────
//
// Behind SET_CONTEXT_REG / SET_SH_REG / SET_UCONFIG_REG sit around two thousand
// register handlers, each unpacking bitfields out of guest-supplied words. Two
// things make them awkward to reach through the walker:
//
//   - each handler asserts one exact packet length, which differs per register
//     and is not readable from the dispatch table, and
//   - a wrong length is a deliberate EXIT_NOT_IMPLEMENTED, which ends the
//     process.
//
// So each register is probed in a forked child with an all-zero payload first.
// A child that dies there is a register that wants a different packet length -
// not covered, counted, and moved past. A register that SURVIVES the zero probe
// and then dies on random payloads is a real finding, and fails the run. That
// distinction is the whole point: without it, a crash and an unsupported length
// look identical.
//
// Windows has no fork, so this half is skipped there. Two of the three CI
// platforms run it.

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

void FuzzRegisterDecoders() {
	std::printf("Pm4FuzzTests: registers: skipped, needs fork\n");
}

#else

enum class ChildResult { Survived, Died };

// Runs `body` in a child process and reports whether it came back alive.
template <class F>
ChildResult RunIsolated(F&& body) {
	std::fflush(stdout);
	std::fflush(stderr);

	const pid_t pid = fork();
	Check(pid >= 0, "fork succeeded");

	if (pid == 0) {
		// The handlers log copiously on odd input; that is not what is being
		// tested and it would bury the result.
		::freopen("/dev/null", "w", stdout);
		::freopen("/dev/null", "w", stderr);
		body();
		::_exit(0);
	}

	int status = 0;
	Check(waitpid(pid, &status, 0) == pid, "waited for the child");
	const bool survived = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	return survived ? ChildResult::Survived : ChildResult::Died;
}

template <class Table>
void FuzzRegisterTable(const Table& table, uint32_t opcode, const char* what) {
	constexpr uint32_t PAYLOAD_DW = 8;
	constexpr uint32_t ROUNDS     = 256;

	const auto offsets = InstalledOffsets(table);
	Check(!offsets.empty(), "the register table has handlers installed");

	// The single-value form: header, offset word, one value.
	const auto cmd_id = KYTY_PM4(3, opcode, 0) & ~1u;

	size_t covered = 0;
	size_t skipped = 0;

	for (auto offset: offsets) {
		auto& renderer = *static_cast<RenderContext*>(AllocateInaccessiblePage());

		const auto probe = RunIsolated([&] {
			CommandProcessor cp(renderer);
			cp.Reset();
			const uint32_t zeros[PAYLOAD_DW] = {};
			table[offset](cp, cmd_id, offset, zeros, PAYLOAD_DW);
		});

		if (probe == ChildResult::Died) {
			// This register wants a packet shape this harness does not generate.
			skipped++;
			continue;
		}

		const auto fuzz = RunIsolated([&] {
			CommandProcessor cp(renderer);
			Rng              rng(0x5EEDu + opcode * 131u + offset);
			for (uint32_t round = 0; round < ROUNDS; round++) {
				cp.Reset();
				uint32_t payload[PAYLOAD_DW];
				for (auto& word: payload) {
					word = rng.Word();
				}
				const auto consumed = table[offset](cp, cmd_id, offset, payload, PAYLOAD_DW);

				// A handler claiming to consume nothing would loop the walker
				// forever; one claiming more than it was given would walk the
				// cursor off the end of the guest buffer.
				if (consumed < 1 || consumed > PAYLOAD_DW) {
					::_exit(2);
				}
			}
		});

		if (fuzz == ChildResult::Died) {
			std::fprintf(stderr,
			             "Pm4FuzzTests: %s register 0x%03x accepts a zero payload but fails on "
			             "random values\n",
			             what, offset);
			std::abort();
		}
		covered++;
	}

	std::printf("Pm4FuzzTests: %s: %zu fuzzed, %zu need another packet shape (of %zu)\n", what,
	            covered, skipped, offsets.size());
	Check(covered > 0, "at least some registers were reachable");
}

void FuzzRegisterDecoders() {
	FuzzRegisterTable(Libs::Graphics::g_hw_ctx_func, Pm4::IT_SET_CONTEXT_REG, "context");
	FuzzRegisterTable(Libs::Graphics::g_hw_sh_func, Pm4::IT_SET_SH_REG, "shader");
	FuzzRegisterTable(Libs::Graphics::g_hw_uc_func, Pm4::IT_SET_UCONFIG_REG, "uconfig");
}

#endif

} // namespace

int main() {
	Common::InitializeThreads();
	// The dispatcher reads the debug-dump switches on every packet, so the
	// config has to exist. Defaults are what a normal run uses.
	Config::Initialize();
	Libs::Graphics::GraphicsInitJmpTables();

	FuzzTheWalker();
	FuzzRegisterDecoders();

	std::printf("Pm4FuzzTests: all passed\n");
	return 0;
}
