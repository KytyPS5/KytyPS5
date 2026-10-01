#ifndef KYTY_TESTS_MACOS_MEMORY_TEST_ALLOCATION_H_
#define KYTY_TESTS_MACOS_MEMORY_TEST_ALLOCATION_H_

#include "graphics/host_gpu/regionDefinitions.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace MacosMemoryTest {

inline void *Allocate(mach_vm_address_t preferred_address, mach_vm_size_t size,
                      vm_prot_t protection) {
  using Libs::Graphics::GuestRange;
  using Libs::Graphics::LOWER_ADDRESS_SIZE;
  constexpr auto mask = Libs::Graphics::TRACKER_REGION_SIZE - 1;
  const auto offset = preferred_address & mask;
  if (!GuestRange{preferred_address, size}.Valid() ||
      preferred_address % Libs::Graphics::TRACKER_PAGE_SIZE != 0 ||
      size % Libs::Graphics::TRACKER_PAGE_SIZE != 0) {
    return nullptr;
  }
  mach_vm_address_t allocation = preferred_address - offset;
  const auto allocation_size = size + offset;
  if (mach_vm_map(mach_task_self(), &allocation, allocation_size, mask,
                  VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, false, protection,
                  VM_PROT_ALL, VM_INHERIT_COPY) != KERN_SUCCESS) {
    return nullptr;
  }
  const auto address = allocation + offset;
  if (!GuestRange{address, size}.Valid() ||
      (address < LOWER_ADDRESS_SIZE) !=
          (preferred_address < LOWER_ADDRESS_SIZE) ||
      (offset != 0 && mach_vm_deallocate(mach_task_self(), allocation,
                                         offset) != KERN_SUCCESS)) {
    mach_vm_deallocate(mach_task_self(), allocation, allocation_size);
    return nullptr;
  }
  return reinterpret_cast<void *>(address);
}

} // namespace MacosMemoryTest

#endif
