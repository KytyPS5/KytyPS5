#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Libs::GuestStdio {

// The legacy LibcInternal stdio imports point at guest FILE objects.  The fields used by
// libc.prx's stream writer are part of the guest ABI and must not alias the host's FILE.
struct File {
	uint16_t                    flags            = 0;
	uint16_t                    reserved_02      = 0;
	int32_t                     descriptor       = -1;
	uint8_t*                    buffer_base      = nullptr;
	uint8_t*                    buffer_limit     = nullptr;
	uint8_t*                    write_pointer    = nullptr;
	uint8_t*                    buffer_base_copy = nullptr;
	uint8_t*                    write_limit      = nullptr;
	std::array<std::byte, 0x20> reserved_30 {};
	uint8_t*                    auxiliary_base_50 = nullptr;
	uint8_t*                    auxiliary_base_58 = nullptr;
	std::array<std::byte, 0x1e> reserved_60 {};
	uint8_t                     inline_buffer_7e = 0;
	uint8_t                     reserved_7f      = 0;
	uintptr_t                   mutex            = 0;
};

static_assert(offsetof(File, flags) == 0x00);
static_assert(offsetof(File, descriptor) == 0x04);
static_assert(offsetof(File, buffer_base) == 0x08);
static_assert(offsetof(File, buffer_limit) == 0x10);
static_assert(offsetof(File, write_pointer) == 0x18);
static_assert(offsetof(File, buffer_base_copy) == 0x20);
static_assert(offsetof(File, write_limit) == 0x28);
static_assert(offsetof(File, auxiliary_base_50) == 0x50);
static_assert(offsetof(File, auxiliary_base_58) == 0x58);
static_assert(offsetof(File, inline_buffer_7e) == 0x7e);
static_assert(offsetof(File, mutex) == 0x80);
static_assert(sizeof(File) == 0x88);

inline constexpr uint16_t kWritableUnbufferedFlags = 0x080a;
inline constexpr size_t   kBufferSize              = 0x10000;

struct Stream {
	File                             file {};
	std::array<uint8_t, kBufferSize> buffer {};

	explicit Stream(int32_t descriptor) {
		file.flags             = kWritableUnbufferedFlags;
		file.descriptor        = descriptor;
		file.buffer_base       = buffer.data();
		file.buffer_limit      = buffer.data() + buffer.size();
		file.write_pointer     = buffer.data();
		file.buffer_base_copy  = buffer.data();
		file.write_limit       = buffer.data() + buffer.size();
		file.auxiliary_base_50 = buffer.data();
		file.auxiliary_base_58 = buffer.data();
	}
};

// Flush a guest stream buffer into a host stream without passing the guest FILE layout to libc.
int Flush(File* file, std::FILE* host_stream) noexcept;

} // namespace Libs::GuestStdio
