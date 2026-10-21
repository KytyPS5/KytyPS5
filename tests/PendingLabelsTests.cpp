// Tests for the end-of-pipe label writes the host GPU has not completed yet.

#include "graphics/guest_gpu/pendingLabels.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using Libs::Graphics::PendingLabels;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

uint64_t Address(const void* pointer) {
	return reinterpret_cast<uint64_t>(pointer);
}

// The command processor sees a pending label; guest memory does not change until it is written.
void TestPendingLabelIsSeenButNotWritten() {
	PendingLabels labels;
	alignas(8) uint32_t memory = 5;
	labels.Add({.address = Address(&memory), .value = 9, .bytes = 4});
	Check(labels.Read(&memory) == 9, "a pending label is not seen by a wait");
	Check(memory == 5, "a pending label was written before the host GPU completed it");
}

// A completed label is written by the thread that takes it, and then reads come from memory.
void TestCompletedLabelIsWrittenInOrder() {
	PendingLabels labels;
	alignas(8) std::array<uint32_t, 2> memory {0, 0};
	const auto first  = labels.Add({.address = Address(&memory[0]), .value = 1, .bytes = 4});
	const auto second = labels.Add({.address = Address(&memory[1]), .value = 2, .bytes = 4,
	                                .interrupt = true, .interrupt_context = 7});
	labels.Complete(second);
	std::vector<PendingLabels::Label> taken;
	labels.TakeCompleted(taken);
	Check(taken.empty(), "a label was taken before an earlier one completed");
	labels.Complete(first);
	labels.TakeCompleted(taken);
	Check(taken.size() == 2 && taken[0].value == 1 && taken[1].value == 2,
	      "completed labels are not taken in the order they were recorded");
	Check(taken.size() == 2 && taken[1].interrupt && taken[1].interrupt_context == 7,
	      "a label lost its interrupt");
	for (const auto& label: taken) {
		std::memcpy(reinterpret_cast<void*>(label.address), &label.value, label.bytes);
		labels.Retire(label);
	}
	Check(labels.Empty(), "retired labels are still pending");
	memory[0] = 3;
	Check(labels.Read(&memory[0]) == 3, "a retired label still hides guest memory");
}

// The newest write of an address wins, also against an older one that completes later.
void TestNewestWriteWins() {
	PendingLabels labels;
	alignas(8) uint32_t memory = 0;
	const auto older = labels.Add({.address = Address(&memory), .value = 1, .bytes = 4});
	labels.Add({.address = Address(&memory), .value = 2, .bytes = 4});
	Check(labels.Read(&memory) == 2, "an older label hid a newer one");
	labels.Complete(older);
	std::vector<PendingLabels::Label> taken;
	labels.TakeCompleted(taken);
	Check(taken.size() == 1, "the older label was not taken");
	memory = 1;
	labels.Retire(taken[0]);
	Check(labels.Read(&memory) == 2, "retiring the older label dropped the newer one");
}

// Labels of 32 and 64 bits overlap byte by byte, newest last.
void TestMixedWidths() {
	PendingLabels labels;
	alignas(8) uint64_t memory = 0;
	labels.Add({.address = Address(&memory), .value = 0x1111111122222222ull, .bytes = 8});
	auto* halves = reinterpret_cast<uint32_t*>(&memory);
	labels.Add({.address = Address(&halves[1]), .value = 0x33333333u, .bytes = 4});
	Check(labels.Read(&memory) == 0x3333333322222222ull, "a 32-bit label over a 64-bit one");
	Check(labels.Read(&halves[0]) == 0x22222222u, "a 32-bit read of a 64-bit label");
	Check(labels.Read(&halves[1]) == 0x33333333u, "the newer half");
}

// An interrupt with no write keeps its place among the labels.
void TestInterruptOnlyKeepsOrder() {
	PendingLabels labels;
	alignas(8) uint32_t memory = 0;
	const auto label = labels.Add({.address = Address(&memory), .value = 4, .bytes = 4});
	const auto event = labels.Add({.bytes = 0, .interrupt = true, .interrupt_context = 1});
	labels.Complete(event);
	std::vector<PendingLabels::Label> taken;
	labels.TakeCompleted(taken);
	Check(taken.empty(), "an interrupt overtook an earlier label");
	labels.Complete(label);
	labels.TakeCompleted(taken);
	Check(taken.size() == 2 && taken[0].bytes == 4 && taken[1].bytes == 0 && taken[1].interrupt,
	      "the label and the interrupt are out of order");
}

// Completion wakes the thread that writes the labels, until it is shut down.
void TestCompletionWakes() {
	PendingLabels labels;
	int wakes = 0;
	labels.SetWake([&wakes] { wakes++; });
	labels.Complete(labels.Add({.bytes = 0, .interrupt = true}));
	Check(wakes == 1, "a completion did not wake the writer");
	labels.SetWake(nullptr);
	labels.Complete(labels.Add({.bytes = 0, .interrupt = true}));
	Check(wakes == 1, "a completion woke a writer that was shut down");
}

// A wait on a pending label does not read guest memory: the page can be the GPU's, and reading
// it would wait for the host GPU.
void TestCoveredReadSkipsMemory() {
	PendingLabels labels;
	// An address that faults if read: the read must come from the pending label alone.
	const auto* unreadable = reinterpret_cast<const uint32_t*>(uintptr_t {0x10});
	labels.Add({.address = Address(unreadable), .value = 6, .bytes = 4});
	Check(labels.Read(unreadable) == 6, "a covered read did not come from the label");
	const auto* unreadable64 = reinterpret_cast<const uint64_t*>(uintptr_t {0x20});
	labels.Add({.address = Address(unreadable64), .value = 0x11, .bytes = 4});
	labels.Add({.address = Address(unreadable64) + 4, .value = 0x22, .bytes = 4});
	Check(labels.Read(unreadable64) == 0x0000002200000011ull,
	      "two labels that cover a 64-bit read were not combined");
}

// GPU commands recorded after a label read its value too (FC 25 resets an append counter with a
// label and loads it into GDS in the next submission), until a later GPU write replaces it.
void TestGpuReadersSeePendingLabels() {
	PendingLabels labels;
	labels.Add({.address = 0x1000, .value = 0, .bytes = 8});
	labels.Add({.address = 0x3000, .value = 5, .bytes = 4});
	auto seen = labels.PendingForGpu(0x1000, 0x1004);
	Check(seen.size() == 1 && seen[0].address == 0x1000 && seen[0].bytes == 8,
	      "a GPU read did not see the pending label over its bytes");
	Check(labels.PendingForGpu(0x2000, 0x3000).empty(), "a GPU read saw a label outside it");
	labels.WrittenByGpu(0x1000, 0x1004);
	Check(labels.PendingForGpu(0x1000, 0x1008).empty(),
	      "a label hid a GPU write recorded after it");
	Check(labels.Read(reinterpret_cast<const uint32_t*>(uintptr_t {0x3000})) == 5,
	      "a GPU write elsewhere changed what the command processor sees");
	labels.Add({.address = 0x1000, .value = 7, .bytes = 4});
	seen = labels.PendingForGpu(0x1000, 0x1008);
	Check(seen.size() == 1 && seen[0].value == 7, "a label after the GPU write was not seen");
	std::vector<PendingLabels::Label> taken;
	for (uint64_t serial = 1; serial <= 3; serial++) labels.Complete(serial);
	labels.TakeCompleted(taken);
	for (const auto& label: taken) labels.Retire(label);
	Check(labels.PendingForGpu(0x0, 0x4000).empty(), "a retired label is still pending for the GPU");
}

} // namespace

int main() {
	TestGpuReadersSeePendingLabels();
	TestPendingLabelIsSeenButNotWritten();
	TestCompletedLabelIsWrittenInOrder();
	TestNewestWriteWins();
	TestMixedWidths();
	TestInterruptOnlyKeepsOrder();
	TestCompletionWakes();
	TestCoveredReadSkipsMemory();
	if (g_failures != 0) {
		std::cerr << g_failures << " pending label check(s) failed\n";
		return 1;
	}
	std::cout << "pending label tests passed\n";
	return 0;
}
