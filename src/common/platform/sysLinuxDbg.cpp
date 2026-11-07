#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
// #error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include "common/platform/sysDbg.h"

#include <pthread.h>

sys_dbg_stack_info_t SysStackUsage() {
	sys_dbg_stack_info_t s {};
#if defined(__APPLE__)
	pthread_t self  = pthread_self();
	s.reserved_addr = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self)) -
	                  pthread_get_stacksize_np(self);
#else
	pthread_attr_t attr {};
	if (pthread_getattr_np(pthread_self(), &attr) == 0) {
		void*  stack_base = nullptr;
		size_t stack_size = 0;
		if (pthread_attr_getstack(&attr, &stack_base, &stack_size) == 0) {
			s.reserved_addr = reinterpret_cast<uintptr_t>(stack_base);
		}
		pthread_attr_destroy(&attr);
	}
#endif
	return s;
}

#endif
