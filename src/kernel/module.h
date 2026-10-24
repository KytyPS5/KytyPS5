#ifndef EMULATOR_SRC_KERNEL_MODULE_H_
#define EMULATOR_SRC_KERNEL_MODULE_H_

#include "common/abi.h"

#include <cstddef>
#include <cstdint>

namespace Libs::LibKernel {

using KernelModule = int32_t;
struct KernelLoadModuleOpt;
struct KernelUnloadModuleOpt;

KernelModule KYTY_SYSV_ABI KernelLoadStartModule(const char* module_file_name, size_t args,
                                                 const void* argp, uint32_t flags,
                                                 const KernelLoadModuleOpt* opt, int* res);
int KYTY_SYSV_ABI KernelStopUnloadModule(KernelModule handle, size_t args, const void* argp,
                                        uint32_t flags, const KernelUnloadModuleOpt* opt,
                                        int* res);

} // namespace Libs::LibKernel

#endif // EMULATOR_SRC_KERNEL_MODULE_H_
