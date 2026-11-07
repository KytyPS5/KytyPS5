// Tests for the threads that copy an upload pass into the staging buffer.

#include "graphics/host_gpu/copyWorkers.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using Libs::Graphics::CopyWorkers;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
	std::vector<uint8_t> bytes(size);
	for (size_t i = 0; i < size; i++) {
		bytes[i] = static_cast<uint8_t>(i * 31 + seed);
	}
	return bytes;
}

// Copies of every size land at their own place and touch no byte outside it.
void TestEveryCopyLandsInPlace(uint32_t worker_count) {
	CopyWorkers workers(worker_count);
	const auto  source = Pattern(8 << 20, 7);
	std::vector<uint8_t>           target(source.size() + 64, 0xEE);
	std::vector<CopyWorkers::Copy> copies;
	// Sizes below, at, and above a piece, and an empty copy.
	const size_t sizes[] = {1, 100, CopyWorkers::PieceSize, CopyWorkers::PieceSize + 1, 0,
	                        3 * CopyWorkers::PieceSize + 17, 1 << 20};
	size_t       offset  = 0;
	for (const auto size: sizes) {
		copies.push_back({target.data() + offset, source.data() + offset, size});
		offset += size + 8; // a gap the copies must not touch
	}
	workers.Run(copies);
	size_t at = 0;
	for (const auto size: sizes) {
		for (size_t i = 0; i < size; i++) {
			if (target[at + i] != source[at + i]) {
				Check(false, "a copied byte was wrong");
				return;
			}
		}
		for (size_t i = 0; i < 8; i++) {
			Check(target[at + size + i] == 0xEE, "a byte between two copies was written");
		}
		at += size + 8;
	}
}

// Many runs back to back, as in an upload pass: each one is complete when Run returns.
void TestRunsBackToBack() {
	CopyWorkers                    workers(3);
	std::vector<uint8_t>           target(3 << 20);
	std::vector<CopyWorkers::Copy> copies(1);
	for (int run = 0; run < 500; run++) {
		const auto source = Pattern(target.size() - static_cast<size_t>(run) * 1000,
		                            static_cast<uint8_t>(run));
		copies[0]         = {target.data(), source.data(), source.size()};
		workers.Run(copies);
		if (target[0] != source[0] || target[source.size() / 2] != source[source.size() / 2] ||
		    target[source.size() - 1] != source[source.size() - 1]) {
			Check(false, "a run returned before its copy was complete");
			return;
		}
	}
}

// The copy function of the workers copies every byte, in pieces, on the calling thread and on the
// workers: a reader of guest memory replaces memcpy this way.
std::atomic<size_t> g_function_bytes {0};

void CountingCopy(void* destination, const void* source, size_t size) {
	std::memcpy(destination, source, size);
	g_function_bytes.fetch_add(size, std::memory_order_relaxed);
}

void TestTheCopyFunctionCopiesEveryByte() {
	for (const size_t size: {size_t {100}, size_t {3} << 20}) {
		CopyWorkers workers(3, CountingCopy);
		const auto  source = Pattern(size, 5);
		std::vector<uint8_t>           target(size, 0);
		std::vector<CopyWorkers::Copy> copies {{target.data(), source.data(), size}};
		g_function_bytes = 0;
		workers.Run(copies);
		Check(target == source, "the copy function left a byte wrong");
		Check(g_function_bytes == size, "a byte did not go through the copy function");
	}
}

// A run with no copies returns at once.
void TestEmptyRun() {
	CopyWorkers workers(2);
	workers.Run({});
}

} // namespace

int main() {
	TestEveryCopyLandsInPlace(0);
	TestEveryCopyLandsInPlace(1);
	TestEveryCopyLandsInPlace(3);
	TestRunsBackToBack();
	TestTheCopyFunctionCopiesEveryByte();
	TestEmptyRun();
	if (g_failures != 0) {
		return 1;
	}
	std::cout << "CopyWorkersTests: all cases passed\n";
	return 0;
}
