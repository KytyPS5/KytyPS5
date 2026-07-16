#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_SEMAPHORE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_SEMAPHORE_H_

#include "common/abi.h"
#include "common/common.h"
#include "kernel/pthread.h"

namespace Libs::LibKernel::Semaphore {

class KernelSemaPrivate;

using KernelSema = KernelSemaPrivate*;

int KYTY_SYSV_ABI KernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init,
                                   int max, void* opt);
int KYTY_SYSV_ABI KernelDeleteSema(KernelSema sem);
int KYTY_SYSV_ABI KernelWaitSema(KernelSema sem, int need, KernelUseconds* time);
int KYTY_SYSV_ABI KernelPollSema(KernelSema sem, int need);
int KYTY_SYSV_ABI KernelSignalSema(KernelSema sem, int count);
int KYTY_SYSV_ABI KernelCancelSema(KernelSema sem, int count, int* threads);
int KYTY_SYSV_ABI PthreadSemInit(void* sem, int flag, unsigned int value, const char* name);
int KYTY_SYSV_ABI PthreadSemDestroy(void* sem);
int KYTY_SYSV_ABI PthreadSemWait(void* sem);
int KYTY_SYSV_ABI PthreadSemTrywait(void* sem);
int KYTY_SYSV_ABI PthreadSemTimedwait(void* sem, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadSemPost(void* sem);
int KYTY_SYSV_ABI PthreadSemGetvalue(void* sem, int* value);

} // namespace Libs::LibKernel::Semaphore

namespace Libs::Posix {

int KYTY_SYSV_ABI sem_init(void* sem, int pshared, unsigned int value);
int KYTY_SYSV_ABI sem_destroy(void* sem);
int KYTY_SYSV_ABI sem_wait(void* sem);
int KYTY_SYSV_ABI sem_trywait(void* sem);
int KYTY_SYSV_ABI sem_reltimedwait_np(void* sem, uint32_t usec);
int KYTY_SYSV_ABI sem_timedwait(void* sem, const LibKernel::KernelTimespec* abstime);
int KYTY_SYSV_ABI sem_post(void* sem);
int KYTY_SYSV_ABI sem_getvalue(void* sem, int* value);

} // namespace Libs::Posix

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_SEMAPHORE_H_ */
