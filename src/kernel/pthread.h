#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/subsystems.h"

// IWYU pragma: no_include <pthread.h>

extern "C" {
struct sched_param;
}

namespace Loader {
struct Program;
} // namespace Loader

namespace Libs {

namespace LibKernel {

KYTY_SUBSYSTEM_DEFINE(Pthread);

struct PthreadAttrPrivate;
struct PthreadPrivate;
struct PthreadMutexPrivate;
struct PthreadMutexattrPrivate;
struct PthreadRwlockPrivate;
struct PthreadRwlockattrPrivate;
struct PthreadCondattrPrivate;
struct PthreadCondPrivate;

struct KernelTimespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct KernelTimeval {
	int64_t tv_sec;
	int64_t tv_usec;
};

struct KernelTimezone {
	int32_t tz_minuteswest;
	int32_t tz_dsttime;
};

struct KernelTimesec {
	int64_t  t;
	uint32_t west_sec;
	uint32_t dst_sec;
};

using PthreadAttr       = PthreadAttrPrivate*;
using Pthread           = PthreadPrivate*;
using KernelCpumask     = uint64_t;
using PthreadMutex      = PthreadMutexPrivate*;
using PthreadMutexattr  = PthreadMutexattrPrivate*;
using KernelSchedParam  = struct sched_param;
using PthreadRwlock     = PthreadRwlockPrivate*;
using PthreadRwlockattr = PthreadRwlockattrPrivate*;
using KernelUseconds    = unsigned int;
using PthreadCondattr   = PthreadCondattrPrivate*;
using PthreadCond       = PthreadCondPrivate*;
using KernelClockid     = int32_t;
using PthreadKey        = int;

using pthread_entry_func_t          = KYTY_SYSV_ABI void* (*)(void*);
using thread_dtors_func_t           = KYTY_SYSV_ABI void (*)();
using pthread_key_destructor_func_t = KYTY_SYSV_ABI void (*)(void*);

void  PthreadInitSelfForMainThread();
void* PthreadCreateMainGuestStack();
void  PthreadDeleteStaticObjects(Loader::Program* program);

int KYTY_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* attr, int type);
int KYTY_SYSV_ABI PthreadMutexattrSetprotocol(PthreadMutexattr* attr, int protocol);
int KYTY_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex);

Pthread KYTY_SYSV_ABI PthreadSelf();
Pthread               PthreadSelfOrNull();
Pthread               PthreadSwapSelfForSignal(Pthread thread);
int KYTY_SYSV_ABI     PthreadCreate(Pthread* thread, const PthreadAttr* attr,
                                    pthread_entry_func_t entry, void* arg, const char* name);
int KYTY_SYSV_ABI     PthreadDetach(Pthread thread);
int KYTY_SYSV_ABI     PthreadJoin(Pthread thread, void** value);
int KYTY_SYSV_ABI     PthreadCancel(Pthread thread);
int KYTY_SYSV_ABI     PthreadSetcancelstate(int state, int* old_state);
int KYTY_SYSV_ABI     PthreadSetcanceltype(int type, int* old_type);
int KYTY_SYSV_ABI     PthreadGetprio(Pthread thread, int* prio);
int KYTY_SYSV_ABI     PthreadSetprio(Pthread thread, int prio);
void KYTY_SYSV_ABI    PthreadTestcancel();
int KYTY_SYSV_ABI     PthreadSetaffinity(Pthread thread, KernelCpumask mask);
void KYTY_SYSV_ABI    PthreadExit(void* value);
int KYTY_SYSV_ABI     PthreadEqual(Pthread thread1, Pthread thread2);
int KYTY_SYSV_ABI     PthreadGetname(Pthread thread, char* name);
int KYTY_SYSV_ABI     PthreadRename(Pthread thread, const char* name);
void KYTY_SYSV_ABI    PthreadYield();
int KYTY_SYSV_ABI     PthreadGetthreadid();
int KYTY_SYSV_ABI     PthreadGetaffinity(Pthread thread, KernelCpumask* mask);
int                   PthreadGetUniqueId(Pthread thread);
uint64_t              PthreadGetHostThreadId(Pthread thread);
void                  PthreadWakeForSignal(Pthread thread);
void                  PthreadQueuePendingSignal(Pthread thread, int signum);
bool                  PthreadHasPendingSignal(Pthread thread, int signum);
bool                  PthreadTakePendingSignal(Pthread thread, int signum);
bool PthreadGetGuestStack(Pthread thread, uint64_t* stack_addr, uint64_t* stack_size);
int  PthreadGetPriorityForKernel(Pthread thread);
int  PthreadGetCurrentPriorityForKernel();

int KYTY_SYSV_ABI          KernelUsleep(KernelUseconds microseconds);
unsigned int KYTY_SYSV_ABI KernelSleep(unsigned int seconds);
int KYTY_SYSV_ABI          KernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp);

int KYTY_SYSV_ABI   PthreadKeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor);
int KYTY_SYSV_ABI   PthreadKeyDelete(PthreadKey key);
int KYTY_SYSV_ABI   PthreadSetspecific(PthreadKey key, void* value);
void* KYTY_SYSV_ABI PthreadGetspecific(PthreadKey key);

void KYTY_SYSV_ABI KernelSetThreadDtors(thread_dtors_func_t dtors);

int KYTY_SYSV_ABI PthreadAttrInit(PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrDestroy(PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrGet(Pthread thread, PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask);
int KYTY_SYSV_ABI PthreadAttrGetdetachstate(const PthreadAttr* attr, int* state);
int KYTY_SYSV_ABI PthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guard_size);
int KYTY_SYSV_ABI PthreadAttrGetinheritsched(const PthreadAttr* attr, int* inherit_sched);
int KYTY_SYSV_ABI PthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param);
int KYTY_SYSV_ABI PthreadAttrGetschedpolicy(const PthreadAttr* attr, int* policy);
int KYTY_SYSV_ABI PthreadAttrGetsolosched(const PthreadAttr* attr, int* solosched);
int KYTY_SYSV_ABI PthreadAttrGetstack(const PthreadAttr* __restrict attr,
                                      void** __restrict stack_addr, size_t* __restrict stack_size);
int KYTY_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* attr, void** stack_addr);
int KYTY_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size);
int KYTY_SYSV_ABI PthreadAttrSetaffinity(PthreadAttr* attr, KernelCpumask mask);
int KYTY_SYSV_ABI PthreadAttrSetdetachstate(PthreadAttr* attr, int state);
int KYTY_SYSV_ABI PthreadAttrSetguardsize(PthreadAttr* attr, size_t guard_size);
int KYTY_SYSV_ABI PthreadAttrSetinheritsched(PthreadAttr* attr, int inherit_sched);
int KYTY_SYSV_ABI PthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param);
int KYTY_SYSV_ABI PthreadAttrSetschedpolicy(PthreadAttr* attr, int policy);
int KYTY_SYSV_ABI PthreadAttrSetsolosched(PthreadAttr* attr, int solosched);
int KYTY_SYSV_ABI PthreadAttrSetstack(PthreadAttr* attr, void* addr, size_t size);
int KYTY_SYSV_ABI PthreadAttrSetstackaddr(PthreadAttr* attr, void* addr);
int KYTY_SYSV_ABI PthreadAttrSetstacksize(PthreadAttr* attr, size_t stack_size);

int KYTY_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockRdlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockTimedrdlock(PthreadRwlock* rwlock, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadRwlockTimedwrlock(PthreadRwlock* rwlock, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadRwlockTryrdlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockTrywrlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockUnlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockWrlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockattrDestroy(PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockattrInit(PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockattrGettype(PthreadRwlockattr* attr, int* type);
int KYTY_SYSV_ABI PthreadRwlockattrSettype(PthreadRwlockattr* attr, int type);

int KYTY_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr);
int KYTY_SYSV_ABI PthreadCondattrInit(PthreadCondattr* attr);
int KYTY_SYSV_ABI PthreadCondattrSetclock(PthreadCondattr* attr, KernelClockid clock_id);
int KYTY_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondDestroy(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr);
int KYTY_SYSV_ABI PthreadCondSignal(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondSignalto(PthreadCond* cond, Pthread thread);
int KYTY_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadCondTimedwaitAbs(PthreadCond* cond, PthreadMutex* mutex,
                                          const KernelTimespec* abstime);
int KYTY_SYSV_ABI PthreadCondWait(PthreadCond* cond, PthreadMutex* mutex);

int KYTY_SYSV_ABI      KernelClockGetres(KernelClockid clock_id, KernelTimespec* tp);
int KYTY_SYSV_ABI      KernelClockGettime(KernelClockid clock_id, KernelTimespec* tp);
int KYTY_SYSV_ABI      KernelGettimeofday(KernelTimeval* tp);
int KYTY_SYSV_ABI      KernelGettimezone(KernelTimezone* tz);
int KYTY_SYSV_ABI      KernelConvertLocaltimeToUtc(int64_t local_time, int64_t reserved,
                                                   int64_t* utc_time, KernelTimezone* timezone,
                                                   int32_t* dst_seconds);
int KYTY_SYSV_ABI      KernelConvertUtcToLocaltime(int64_t utc_time, int64_t* local_time,
                                                   KernelTimesec* st, uint64_t* dst_sec);
uint64_t KYTY_SYSV_ABI KernelGetTscFrequency();
uint64_t KYTY_SYSV_ABI KernelReadTsc();
uint64_t KYTY_SYSV_ABI KernelGetProcessTime();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounter();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounterFrequency();

} // namespace LibKernel

namespace Posix {

int KYTY_SYSV_ABI  pthread_create(LibKernel::Pthread* thread, const LibKernel::PthreadAttr* attr,
                                  LibKernel::pthread_entry_func_t entry, void* arg);
int KYTY_SYSV_ABI  pthread_create_name_np(LibKernel::Pthread*             thread,
                                          const LibKernel::PthreadAttr*   attr,
                                          LibKernel::pthread_entry_func_t entry, void* arg,
                                          const char* name);
int KYTY_SYSV_ABI  pthread_detach(LibKernel::Pthread thread);
void KYTY_SYSV_ABI pthread_exit(void* value);
LibKernel::Pthread KYTY_SYSV_ABI pthread_self();
int KYTY_SYSV_ABI                pthread_rename_np(LibKernel::Pthread thread, const char* name);
int KYTY_SYSV_ABI                pthread_setcancelstate(int state, int* old_state);
int KYTY_SYSV_ABI                pthread_setprio(LibKernel::Pthread thread, int prio);
int KYTY_SYSV_ABI                pthread_getschedparam(LibKernel::Pthread thread, int* policy,
                                                       LibKernel::KernelSchedParam* param);
int KYTY_SYSV_ABI                pthread_setschedparam(LibKernel::Pthread thread, int policy,
                                                       const LibKernel::KernelSchedParam* param);
void KYTY_SYSV_ABI               pthread_yield();
int KYTY_SYSV_ABI                sched_get_priority_max(int policy);
int KYTY_SYSV_ABI                sched_get_priority_min(int policy);
int KYTY_SYSV_ABI                pthread_join(LibKernel::Pthread thread, void** value);
int KYTY_SYSV_ABI                pthread_attr_init(LibKernel::PthreadAttr* attr);
int KYTY_SYSV_ABI                pthread_attr_destroy(LibKernel::PthreadAttr* attr);
int KYTY_SYSV_ABI pthread_attr_get_np(LibKernel::Pthread thread, LibKernel::PthreadAttr* attr);
int KYTY_SYSV_ABI pthread_attr_getdetachstate(const LibKernel::PthreadAttr* attr, int* state);
int KYTY_SYSV_ABI pthread_attr_getguardsize(const LibKernel::PthreadAttr* attr, size_t* guard_size);
int KYTY_SYSV_ABI pthread_attr_getinheritsched(const LibKernel::PthreadAttr* attr,
                                               int*                          inherit_sched);
int KYTY_SYSV_ABI pthread_attr_getschedparam(const LibKernel::PthreadAttr* attr,
                                             LibKernel::KernelSchedParam*  param);
int KYTY_SYSV_ABI pthread_attr_getschedpolicy(const LibKernel::PthreadAttr* attr, int* policy);
int KYTY_SYSV_ABI pthread_attr_getstack(const LibKernel::PthreadAttr* __restrict attr,
                                        void** __restrict stack_addr,
                                        size_t* __restrict stack_size);
int KYTY_SYSV_ABI pthread_attr_getstacksize(const LibKernel::PthreadAttr* attr, size_t* stack_size);
int KYTY_SYSV_ABI pthread_attr_setdetachstate(LibKernel::PthreadAttr* attr, int state);
int KYTY_SYSV_ABI pthread_attr_setguardsize(LibKernel::PthreadAttr* attr, size_t guard_size);
int KYTY_SYSV_ABI pthread_attr_setinheritsched(LibKernel::PthreadAttr* attr, int inherit_sched);
int KYTY_SYSV_ABI pthread_attr_setschedparam(LibKernel::PthreadAttr*            attr,
                                             const LibKernel::KernelSchedParam* param);
int KYTY_SYSV_ABI pthread_attr_setschedpolicy(LibKernel::PthreadAttr* attr, int policy);
int KYTY_SYSV_ABI pthread_attr_setstacksize(LibKernel::PthreadAttr* attr, size_t stack_size);
int KYTY_SYSV_ABI pthread_cond_broadcast(LibKernel::PthreadCond* cond);
int KYTY_SYSV_ABI pthread_cond_signal(LibKernel::PthreadCond* cond);
int KYTY_SYSV_ABI pthread_cond_init(LibKernel::PthreadCond*           cond,
                                    const LibKernel::PthreadCondattr* attr);
int KYTY_SYSV_ABI pthread_condattr_init(LibKernel::PthreadCondattr* attr);
int KYTY_SYSV_ABI pthread_condattr_destroy(LibKernel::PthreadCondattr* attr);
int KYTY_SYSV_ABI pthread_condattr_setclock(LibKernel::PthreadCondattr* attr,
                                            LibKernel::KernelClockid    clock_id);
int KYTY_SYSV_ABI pthread_cond_wait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI pthread_cond_timedwait(LibKernel::PthreadCond*          cond,
                                         LibKernel::PthreadMutex*         mutex,
                                         const LibKernel::KernelTimespec* abstime);
int KYTY_SYSV_ABI pthread_once(void* once_control, void(KYTY_SYSV_ABI* init_routine)());
int KYTY_SYSV_ABI pthread_mutex_lock(LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI pthread_mutex_trylock(LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI pthread_mutex_timedlock(LibKernel::PthreadMutex*         mutex,
                                          const LibKernel::KernelTimespec* abstime);
int KYTY_SYSV_ABI pthread_mutex_unlock(LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI pthread_rwlock_rdlock(LibKernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI pthread_rwlock_unlock(LibKernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI pthread_rwlock_wrlock(LibKernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI pthread_rwlock_destroy(LibKernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI pthread_key_create(LibKernel::PthreadKey*                   key,
                                     LibKernel::pthread_key_destructor_func_t destructor);
int KYTY_SYSV_ABI pthread_key_delete(LibKernel::PthreadKey key);
int KYTY_SYSV_ABI pthread_setspecific(LibKernel::PthreadKey key, void* value);
void* KYTY_SYSV_ABI pthread_getspecific(LibKernel::PthreadKey key);
int KYTY_SYSV_ABI   pthread_mutex_destroy(LibKernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI   pthread_mutex_init(LibKernel::PthreadMutex*           mutex,
                                       const LibKernel::PthreadMutexattr* attr);
int KYTY_SYSV_ABI   pthread_mutexattr_init(LibKernel::PthreadMutexattr* attr);
int KYTY_SYSV_ABI   pthread_mutexattr_settype(LibKernel::PthreadMutexattr* attr, int type);
int KYTY_SYSV_ABI   pthread_mutexattr_setprotocol(LibKernel::PthreadMutexattr* attr, int protocol);
int KYTY_SYSV_ABI   pthread_mutexattr_destroy(LibKernel::PthreadMutexattr* attr);
int KYTY_SYSV_ABI   pthread_getstack(const LibKernel::PthreadAttr* __restrict attr,
                                     void** __restrict stack_addr, size_t* __restrict stack_size);
int64_t KYTY_SYSV_ABI lseek(int d, int64_t offset, int whence);

} // namespace Posix

} // namespace Libs

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_ */
