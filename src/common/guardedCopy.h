#ifndef KYTY_COMMON_GUARDED_COPY_H_
#define KYTY_COMMON_GUARDED_COPY_H_

#include <cstdint>

namespace Common {

// A copy that stops at the first byte it cannot read or write. Returns the bytes not copied: 0 when all were, else
// the copy ended at byte `size` - returned (a page without access there). `destination` must be readable host memory
// (every caller passes a local buffer); the source may be a guest mapping. Windows: rep movsb, with the access
// violation resumed after the copy (hostException.cpp asks ResumeGuardedCopy before the guest-memory fault handler).
// Linux: the copy goes through /proc/self/mem, which stops at the first page without access instead of faulting.
uint64_t GuardedCopy(void* destination, const void* source, uint64_t size) noexcept;

#if defined(_WIN32)
// The vectored exception handler's check (`context`: the fault's CONTEXT): true when the access violation is
// GuardedCopy's, with the context moved past its copy.
bool ResumeGuardedCopy(void* context) noexcept;
#endif

} // namespace Common

#endif /* KYTY_COMMON_GUARDED_COPY_H_ */
