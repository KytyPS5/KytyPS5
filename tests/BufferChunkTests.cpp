// Tests for the guest range that a new buffer of the buffer cache covers.

#include "graphics/host_gpu/bufferChunk.h"

#include <iostream>

namespace {

using Libs::Graphics::BUFFER_CHUNK_SIZE;
using Libs::Graphics::ChunkRange;
using Libs::Graphics::GuestSpan;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

constexpr uint64_t Page  = 0x4000;
constexpr uint64_t Chunk = BUFFER_CHUNK_SIZE;
constexpr uint64_t Base  = 0x2'0000'0000;

void TestARequestGrowsToItsChunk() {
	const GuestSpan mapped {Base, Base + 8 * Chunk};
	const auto      span = ChunkRange({Base + Chunk + Page, Base + Chunk + 3 * Page}, mapped);
	Check(span.begin == Base + Chunk && span.end == Base + 2 * Chunk,
	      "a small request inside a mapping covers its whole chunk");
}

void TestARequestOverAChunkBorderCoversBothChunks() {
	const GuestSpan mapped {Base, Base + 8 * Chunk};
	const auto      span = ChunkRange({Base + Chunk - Page, Base + Chunk + Page}, mapped);
	Check(span.begin == Base && span.end == Base + 2 * Chunk,
	      "a request across a chunk border covers both chunks");
}

void TestTheChunkStaysInsideTheMapping() {
	const GuestSpan mapped {Base + Page, Base + 5 * Page};
	const auto      span = ChunkRange({Base + 2 * Page, Base + 3 * Page}, mapped);
	Check(span.begin == Base + Page && span.end == Base + 5 * Page,
	      "a chunk is cut at the ends of the mapping that holds the request");
}

void TestARequestOutsideTheMappingIsKept() {
	const GuestSpan mapped {Base + 2 * Page, Base + 4 * Page};
	const auto      span = ChunkRange({Base, Base + 3 * Page}, mapped);
	Check(span.begin == Base && span.end == Base + 3 * Page,
	      "a request that starts outside the mapping is not grown");
	const auto none = ChunkRange({Base, Base + Page}, GuestSpan {});
	Check(none.begin == Base && none.end == Base + Page,
	      "without a mapping the request is not grown");
}

void TestARequestPastTheMappingEndKeepsItsEnd() {
	const GuestSpan mapped {Base, Base + 4 * Page};
	const auto      span = ChunkRange({Base + 2 * Page, Base + 6 * Page}, mapped);
	Check(span.begin == Base && span.end == Base + 6 * Page,
	      "a request that leaves the mapping keeps its own end");
}

void TestAChunkOfOnePageKeepsTheRequest() {
	const GuestSpan mapped {Base, Base + 8 * Chunk};
	const auto      span = ChunkRange({Base + Page, Base + 3 * Page}, mapped, Page);
	Check(span.begin == Base + Page && span.end == Base + 3 * Page,
	      "with chunks of one page a buffer covers only its first use");
}

} // namespace

int main() {
	TestARequestGrowsToItsChunk();
	TestARequestOverAChunkBorderCoversBothChunks();
	TestTheChunkStaysInsideTheMapping();
	TestARequestOutsideTheMappingIsKept();
	TestARequestPastTheMappingEndKeepsItsEnd();
	TestAChunkOfOnePageKeepsTheRequest();
	if (g_failures != 0) {
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "buffer chunk tests passed\n";
	return 0;
}
