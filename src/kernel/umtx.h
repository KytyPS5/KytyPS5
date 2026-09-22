#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_UMTX_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_UMTX_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::LibKernel::Umtx {

// FreeBSD _umtx_op operation numbers, as the guest's libthr knows them.
constexpr int UMTX_OP_RESERVED0         = 0;
constexpr int UMTX_OP_RESERVED1         = 1;
constexpr int UMTX_OP_WAIT              = 2;
constexpr int UMTX_OP_WAKE              = 3;
constexpr int UMTX_OP_MUTEX_TRYLOCK     = 4;
constexpr int UMTX_OP_MUTEX_LOCK        = 5;
constexpr int UMTX_OP_MUTEX_UNLOCK      = 6;
constexpr int UMTX_OP_SET_CEILING       = 7;
constexpr int UMTX_OP_CV_WAIT           = 8;
constexpr int UMTX_OP_CV_SIGNAL         = 9;
constexpr int UMTX_OP_CV_BROADCAST      = 10;
constexpr int UMTX_OP_WAIT_UINT         = 11;
constexpr int UMTX_OP_RW_RDLOCK         = 12;
constexpr int UMTX_OP_RW_WRLOCK         = 13;
constexpr int UMTX_OP_RW_UNLOCK         = 14;
constexpr int UMTX_OP_WAIT_UINT_PRIVATE = 15;
constexpr int UMTX_OP_WAKE_PRIVATE      = 16;
constexpr int UMTX_OP_MUTEX_WAIT        = 17;
constexpr int UMTX_OP_MUTEX_WAKE        = 18;
constexpr int UMTX_OP_SEM_WAIT          = 19;
constexpr int UMTX_OP_SEM_WAKE          = 20;
constexpr int UMTX_OP_NWAKE_PRIVATE     = 21;
constexpr int UMTX_OP_MUTEX_WAKE2       = 22;
constexpr int UMTX_OP_SEM2_WAIT         = 23;
constexpr int UMTX_OP_SEM2_WAKE         = 24;
constexpr int UMTX_OP_SHM               = 25;
constexpr int UMTX_OP_ROBUST_LISTS      = 26;

// struct umutex owner-word encoding.
constexpr uint32_t UMUTEX_UNOWNED       = 0x00000000u;
constexpr uint32_t UMUTEX_CONTESTED     = 0x80000000u;
constexpr uint32_t UMUTEX_RB_OWNERDEAD  = UMUTEX_CONTESTED | 0x10u;
constexpr uint32_t UMUTEX_RB_NOTRECOV   = UMUTEX_CONTESTED | 0x11u;

// struct umutex / ucond / urwlock / _usem2 flag words.
constexpr uint32_t USYNC_PROCESS_SHARED = 0x0001;
constexpr uint32_t UMUTEX_PRIO_INHERIT  = 0x0004;
constexpr uint32_t UMUTEX_PRIO_PROTECT  = 0x0008;
constexpr uint32_t UMUTEX_ROBUST        = 0x0010;
constexpr uint32_t UMUTEX_NONCONSISTENT = 0x0020;

constexpr uint32_t URWLOCK_PREFER_READER = 0x0002;
constexpr uint32_t URWLOCK_READ_OWNER    = 0x80000000u;
constexpr uint32_t URWLOCK_WRITE_OWNER   = 0x40000000u;
constexpr uint32_t URWLOCK_WRITE_WAITERS = 0x20000000u;
constexpr uint32_t URWLOCK_READ_WAITERS  = 0x10000000u;
constexpr uint32_t URWLOCK_MAX_READERS   = 0x0fffffffu;

constexpr uint32_t USEM_HAS_WAITERS = 0x80000000u;
constexpr uint32_t USEM_MAX_COUNT   = 0x7fffffffu;

// _umtx_time flags, and the CV_WAIT wflags passed in `val`.
constexpr uint32_t UMTX_ABSTIME          = 0x01;
constexpr uint64_t CVWAIT_CHECK_UNPARKING = 0x01;
constexpr uint64_t CVWAIT_ABSTIME         = 0x02;
constexpr uint64_t CVWAIT_CLOCKID         = 0x04;

// UMTX_OP_SHM sub-operations.
constexpr uint64_t UMTX_SHM_CREAT   = 0x0001;
constexpr uint64_t UMTX_SHM_LOOKUP  = 0x0002;
constexpr uint64_t UMTX_SHM_DESTROY = 0x0004;
constexpr uint64_t UMTX_SHM_ALIVE   = 0x0008;

#pragma pack(push, 4)

struct GuestTimespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct GuestUmtxTime {
	GuestTimespec timeout;
	uint32_t      flags;
	uint32_t      clockid;
};

struct GuestUmutex {
	uint32_t m_owner;
	uint32_t m_flags;
	uint32_t m_ceilings[2];
	uint64_t m_rb_lnk;
	uint32_t m_spare[2];
};

struct GuestUcond {
	uint32_t c_has_waiters;
	uint32_t c_flags;
	uint32_t c_clockid;
	uint32_t c_spare[1];
};

struct GuestUrwlock {
	uint32_t rw_state;
	uint32_t rw_flags;
	uint32_t rw_blocked_readers;
	uint32_t rw_blocked_writers;
	uint32_t rw_spare[4];
};

struct GuestUsem2 {
	uint32_t count;
	uint32_t flags;
};

struct GuestUsem {
	uint32_t has_waiters;
	uint32_t count;
	uint32_t flags;
};

struct GuestRobustListsParams {
	uint64_t robust_list_offset;
	uint64_t robust_priv_list_offset;
	uint64_t robust_inact_offset;
};

#pragma pack(pop)

static_assert(sizeof(GuestTimespec) == 16);
static_assert(sizeof(GuestUmtxTime) == 24);
static_assert(sizeof(GuestUmutex) == 32);
static_assert(sizeof(GuestUcond) == 16);
static_assert(sizeof(GuestUrwlock) == 32);
static_assert(sizeof(GuestUsem2) == 8);

using tid_func_t         = uint32_t (*)();
using signal_poll_func_t = void (*)();
// Fills seconds/nanoseconds for a FreeBSD clock id. Returns false for an id it cannot serve.
using clock_gettime_func_t = bool (*)(int clock_id, int64_t* sec, int64_t* nsec);

// Installs the emulator-side hooks. Any argument may be null to keep the built-in default.
void SetHooks(tid_func_t tid, signal_poll_func_t signal_poll, clock_gettime_func_t clock_gettime);

// The thread id this layer writes into a umutex owner word.
uint32_t CurrentTid();

// Runs one _umtx_op. Returns 0 on success or a positive POSIX errno (the values in
// `libs/errno.h`, namespace Libs::Posix) on failure. Never aborts the emulator.
int Op(void* obj, int operation, uint64_t val, void* uaddr1, void* uaddr2);

// Test seam: drops every queue. Only safe when no thread is waiting.
void ResetForTests();

// Test seam: how many threads are parked on `addr` for `operation`'s queue type.
int WaiterCountForTests(const void* addr, int operation);

} // namespace Libs::LibKernel::Umtx

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_UMTX_H_ */
