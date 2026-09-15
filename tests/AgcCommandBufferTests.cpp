#include "libs/agcCommandBuffer.h"

#include <array>
#include <cstdio>
#include <cstdlib>

using Libs::Graphics::Gen5::CommandBufferAvailableDW;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "AgcCommandBufferTests: %s\n", message);
		std::abort();
	}
}

int main() {
	std::array<uint32_t, 16> storage {};
	auto* b = storage.data();
	Check(CommandBufferAvailableDW(b, b + 6, b, nullptr, 0) == 6, "24-byte null-boundary buffer");
	Check(CommandBufferAvailableDW(b, b + 6, b + 6, nullptr, 0) == 0, "exact-fit exhaustion");
	Check(CommandBufferAvailableDW(b, b + 6, b + 1, nullptr, 0) < 6, "insufficient space");
	Check(CommandBufferAvailableDW(b, b + 6, b, nullptr, 2) == 4, "reserved words");
	Check(CommandBufferAvailableDW(b, b + 6, b, nullptr, 6) == 0, "all space reserved");
	Check(CommandBufferAvailableDW(b, b + 6, b, nullptr, 7) == 0, "reservation underflow");
	Check(CommandBufferAvailableDW(b, b + 16, b + 2, b + 10, 2) == 6, "two-ended buffer");
	Check(CommandBufferAvailableDW(b, b + 16, b + 10, b + 10, 0) == 0, "meeting cursors");
	Check(CommandBufferAvailableDW(b, b + 16, b + 11, b + 10, 0) == 0, "crossed cursors");
	Check(CommandBufferAvailableDW(b, b + 6, b + 7, nullptr, 0) == 0, "cursor beyond top");
	Check(CommandBufferAvailableDW(b + 1, b + 6, b, nullptr, 0) == 0, "cursor below bottom");
	Check(CommandBufferAvailableDW(b, b + 6, b, b + 7, 0) == 0, "down cursor beyond top");
	Check(CommandBufferAvailableDW(nullptr, b + 6, b, nullptr, 0) == 0, "null bottom");
	Check(CommandBufferAvailableDW(b, nullptr, b, nullptr, 0) == 0, "null top");
	Check(CommandBufferAvailableDW(b, b + 6, nullptr, nullptr, 0) == 0, "null cursor");
	std::puts("AgcCommandBufferTests: 15 checks passed");
}
