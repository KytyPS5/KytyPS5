#include "common/virtualMemory.h"

#include "common/platform/sysVirtual.h"

namespace Common {

namespace VirtualMemory {

void Init() {
	SysVirtualInit();
}

uint64_t Alloc(uint64_t address, uint64_t size, Mode mode) {
	return SysVirtualAlloc(address, size, mode);
}

uint64_t AllocAligned(uint64_t address, uint64_t size, Mode mode, uint64_t alignment) {
	return SysVirtualAllocAligned(address, size, mode, alignment);
}

bool AllocFixed(uint64_t address, uint64_t size, Mode mode) {
	return SysVirtualAllocFixed(address, size, mode);
}

bool Commit(uint64_t address, uint64_t size, Mode mode) {
	return SysVirtualCommit(address, size, mode);
}

uint64_t Reserve(uint64_t address, uint64_t size) {
	return SysVirtualReserve(address, size);
}

uint64_t ReserveAligned(uint64_t address, uint64_t size, uint64_t alignment) {
	return SysVirtualReserveAligned(address, size, alignment);
}

bool ReserveFixed(uint64_t address, uint64_t size) {
	return SysVirtualReserveFixed(address, size);
}

bool Decommit(uint64_t address, uint64_t size) {
	return SysVirtualDecommit(address, size);
}

bool Free(uint64_t address) {
	return SysVirtualFree(address);
}

bool FreeRange(uint64_t address, uint64_t size) {
	return SysVirtualFreeRange(address, size);
}

bool Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode) {
	return SysVirtualProtect(address, size, mode, old_mode);
}

bool FlushInstructionCache(uint64_t address, uint64_t size) {
	return SysVirtualFlushInstructionCache(address, size);
}

bool PatchReplace(uint64_t vaddr, uint64_t value) {
	Mode old_mode {};
	Protect(vaddr, 8, Mode::ReadWrite, &old_mode);

	auto* ptr = reinterpret_cast<uint64_t*>(vaddr);

	bool ret = (*ptr != value);

	*ptr = value;

	Protect(vaddr, 8, old_mode);

	if (IsExecute(old_mode)) {
		FlushInstructionCache(vaddr, 8);
	}

	return ret;
}

} // namespace VirtualMemory

} // namespace Common
