#pragma once
#include "common/abi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Loader {
struct Program;
namespace DemonsSoulsCopy {
void Install(Program* program);
void Clear();
using PrepareWrite = bool (*)(uint64_t, uint64_t);
inline void* Move(void* destination, const void* source, size_t size, PrepareWrite prepare) {
	if (size == 0 || destination == source) return destination;
	// Preserve the normal mapping/fault path after preparing a complete large write.
	if (size >= 256 * 1024 && prepare) (void)prepare(reinterpret_cast<uint64_t>(destination), size);
	return std::memmove(destination, source, size);
}
inline std::array<uint8_t, 13> Tailcall(uint64_t function) {
	std::array<uint8_t, 13> code {0x5d, 0x48, 0xb8}; // pop rbp; mov rax, function; jmp rax
	std::memcpy(code.data() + 3, &function, sizeof(function));
	code[11] = 0xff;
	code[12] = 0xe0;
	return code;
}
} // namespace DemonsSoulsCopy
} // namespace Loader
