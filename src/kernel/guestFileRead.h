#ifndef KYTY_KERNEL_GUEST_FILE_READ_H_
#define KYTY_KERNEL_GUEST_FILE_READ_H_

#include "common/file.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Libs::LibKernel::FileSystem {

// A predicate on the guest range [dst, dst + size).
using GuestRangeFunc = bool (*)(const void* dst, uint64_t size);

// Reads up to want bytes into a guest buffer from the file, which is positioned at pos. Returns 0,
// EFAULT or EIO; *got holds the bytes delivered in every case. A short read at end of file
// returns 0.
//
// A guest buffer may be write-protected by the GPU resource tracker. The host files are
// unbuffered, so the host read then fails with EFAULT (ERROR_NOACCESS on Windows) instead of
// faulting into the guest handler. The rest of the read goes through a host bounce buffer: the
// copy into the guest takes the fault and the tracker unprotects the page. A store the handler
// does not recover would end the emulator, so the copy needs a destination recovers_fault accepts;
// any other destination fails the read with EFAULT, and the file offset then covers only the
// delivered bytes.
//
// The first host read stores straight into the buffer, so a destination known_unwritable reports
// (reserved, or mapped without write access) fails the read with EFAULT before it: it delivers 0
// bytes and leaves the offset unchanged. That check rejects nothing it does not know about: a
// buffer outside the guest memory map, such as a host buffer handed to a guest callback, is read
// into as before.
inline int ReadIntoGuest(Common::File& f, void* buf, uint32_t want, uint64_t pos, uint32_t* got,
                         GuestRangeFunc known_unwritable, GuestRangeFunc recovers_fault) {
	int err = 0;
	*got    = 0;
	if (want != 0 && known_unwritable(buf, want)) {
		return EFAULT;
	}
	f.ReadEx(buf, want, got, &err);
	if (*got >= want || err == 0) {
		return 0;
	}
	if (err != EFAULT) {
		return EIO;
	}

	constexpr uint32_t   Chunk = 1u << 20u;
	std::vector<uint8_t> bounce;
	uint32_t             done   = *got;
	int                  result = 0;
	if (!f.Seek(pos + done)) {
		return EIO;
	}
	while (done < want) {
		const uint32_t count = std::min<uint32_t>(want - done, Chunk);
		bounce.resize(count);
		uint32_t r  = 0;
		int      e2 = 0;
		f.ReadEx(bounce.data(), count, &r, &e2);
		if (e2 != 0) {
			result = EIO;
			break;
		}
		auto* dst = static_cast<uint8_t*>(buf) + done;
		if (r != 0) {
			if (!recovers_fault(dst, r)) {
				result = EFAULT;
				break;
			}
			std::memcpy(dst, bounce.data(), r);
		}
		done += r;
		if (r < count) {
			break;
		}
	}
	*got = done;
	if (result != 0) {
		(void)f.Seek(pos + done);
	}
	return result;
}

} // namespace Libs::LibKernel::FileSystem

#endif /* KYTY_KERNEL_GUEST_FILE_READ_H_ */
