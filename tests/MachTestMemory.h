#pragma once

#if defined(__APPLE__)
#include "graphics/host_gpu/regionDefinitions.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace TestMemory {

inline void* Allocate(uint64_t preferred, uint64_t size, vm_prot_t protection) {
  using namespace Libs::Graphics;
  if (!GuestRange{preferred, size}.Valid()) return nullptr;
  mach_vm_address_t raw = preferred;
  if (preferred >= LOWER_ADDRESS_SIZE) {
    if (mach_vm_allocate(mach_task_self(), &raw, size, VM_FLAGS_FIXED) != KERN_SUCCESS) return nullptr;
    if (mach_vm_protect(mach_task_self(), raw, size, false, protection) != KERN_SUCCESS) {
      mach_vm_deallocate(mach_task_self(), raw, size);
      return nullptr;
    }
    return reinterpret_cast<void*>(raw);
  }
  const auto offset = preferred % TRACKER_REGION_SIZE;
  raw -= offset;
  if (mach_vm_map(mach_task_self(), &raw, size + offset, TRACKER_REGION_SIZE - 1, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, false, protection, VM_PROT_ALL, VM_INHERIT_NONE) != KERN_SUCCESS) return nullptr;
  const auto address = raw + offset;
  if (address >= LOWER_ADDRESS_SIZE || !GuestRange{address, size}.Valid() || raw % TRACKER_REGION_SIZE != 0) {
    mach_vm_deallocate(mach_task_self(), raw, size + offset);
    return nullptr;
  }
  if (offset != 0 && mach_vm_deallocate(mach_task_self(), raw, offset) != KERN_SUCCESS) {
    mach_vm_deallocate(mach_task_self(), raw, size + offset);
    return nullptr;
  }
  return reinterpret_cast<void*>(address);
}

inline bool CheckOccupiedPreferredAddress() {
  using namespace Libs::Graphics;
  const uint64_t page = vm_page_size;
  const uint64_t offset = 0x10000;
  mach_vm_address_t guard = 0x200000000ull;
  if (mach_vm_map(mach_task_self(), &guard, offset + page, TRACKER_REGION_SIZE - 1, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, false, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE) != KERN_SUCCESS) return false;
  auto* occupied = reinterpret_cast<volatile uint8_t*>(guard + offset);
  *occupied = 0xa5;
  auto* memory = Allocate(guard + offset, page, VM_PROT_READ | VM_PROT_WRITE);
  const auto address = reinterpret_cast<uint64_t>(memory);
  const bool valid = memory != nullptr && address != guard + offset && address % TRACKER_REGION_SIZE == offset && address < LOWER_ADDRESS_SIZE && GuestRange{address, page}.Valid() && *occupied == 0xa5;
  if (memory != nullptr) mach_vm_deallocate(mach_task_self(), address, page);
  mach_vm_deallocate(mach_task_self(), guard, offset + page);
  return valid;
}

} // namespace TestMemory
#endif
