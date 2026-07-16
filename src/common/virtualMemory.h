#ifndef KYTY_COMMON_VIRTUALMEMORY_H_
#define KYTY_COMMON_VIRTUALMEMORY_H_

#include "common/common.h"

namespace Common {

namespace VirtualMemory {

enum class Mode : uint32_t {
	NoAccess         = 0,
	Read             = 1,
	Write            = 2,
	ReadWrite        = Read | Write,
	Execute          = 4,
	ExecuteRead      = Execute | Read,
	ExecuteWrite     = Execute | Write,
	ExecuteReadWrite = Execute | Read | Write,
};

inline bool IsExecute(Mode mode) {
	return (mode == Mode::Execute || mode == Mode::ExecuteRead || mode == Mode::ExecuteWrite ||
	        mode == Mode::ExecuteReadWrite);
}

void Init();

uint64_t Alloc(uint64_t address, uint64_t size, Mode mode);
uint64_t AllocAligned(uint64_t address, uint64_t size, Mode mode, uint64_t alignment);
bool     AllocFixed(uint64_t address, uint64_t size, Mode mode);
bool     Commit(uint64_t address, uint64_t size, Mode mode);
uint64_t Reserve(uint64_t address, uint64_t size);
uint64_t ReserveAligned(uint64_t address, uint64_t size, uint64_t alignment);
bool     ReserveFixed(uint64_t address, uint64_t size);
bool     Decommit(uint64_t address, uint64_t size);
bool     Free(uint64_t address);
bool     FreeRange(uint64_t address, uint64_t size);
bool     Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode = nullptr);
bool     FlushInstructionCache(uint64_t address, uint64_t size);
bool     PatchReplace(uint64_t vaddr, uint64_t value);

} // namespace VirtualMemory

} // namespace Common

#endif /* KYTY_COMMON_VIRTUALMEMORY_H_ */
