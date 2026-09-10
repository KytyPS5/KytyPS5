#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
// #error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include "common/platform/sysDbg.h"

#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libgen.h> // POSIX basename() lives here on macOS, not in <cstring>
#endif

void SysStackUsage(sys_dbg_stack_info_t& s) {
	pid_t pid = getpid();

	// printf("pid = %"I64"d\n", (int64_t)pid);

	[[maybe_unused]] int result = 0;

	memset(&s, 0, sizeof(sys_dbg_stack_info_t));

	// Record the reservation before the Linux /proc walk.
	{
		pthread_attr_t self_attr {};
#if defined(__APPLE__)
		void*        stack_top  = pthread_get_stackaddr_np(pthread_self());
		const size_t stack_size = pthread_get_stacksize_np(pthread_self());
		if (stack_top != nullptr && stack_size != 0) {
			s.reserved_addr = reinterpret_cast<uintptr_t>(stack_top) - stack_size;
			s.reserved_size = stack_size;
		}
		(void)self_attr;
#else
		if (pthread_getattr_np(pthread_self(), &self_attr) == 0) {
			void*  stack_base = nullptr;
			size_t stack_size = 0;
			if (pthread_attr_getstack(&self_attr, &stack_base, &stack_size) == 0 &&
			    stack_base != nullptr && stack_size != 0) {
				s.reserved_addr = reinterpret_cast<uintptr_t>(stack_base);
				s.reserved_size = stack_size;
			}
			pthread_attr_destroy(&self_attr);
		}
#endif
	}

	char str[1024];
	char str2[1024];
	result = sprintf(str, "/proc/%d/exe", static_cast<int>(pid));

	ssize_t buff_len = 0;
	if ((buff_len = readlink(str, str2, 1023)) == -1) {
		return;
	}
	str2[buff_len]   = '\0';
	const char* name = basename(str2);

	result = sprintf(str, "/proc/%d/maps", static_cast<int>(pid));

	FILE* f = fopen(str, "r");

	if (f == nullptr) {
		return;
	}

	// printf("&str = %"I64"x\n", (uint64_t)&str);

	uint64_t                  addr                 = 0;
	uint64_t                  endaddr              = 0;
	[[maybe_unused]] uint64_t size                 = 0;
	uint64_t                  offset               = 0;
	uint64_t                  inode                = 0;
	char                      permissions[8]       = {};
	char                      device[8]            = {};
	char                      filename[MAXPATHLEN] = {};

	auto check_addr = reinterpret_cast<uintptr_t>(&f);

	while (true) {
		if (feof(f) != 0) {
			break;
		}

		if (fgets(str, sizeof(str), f) == nullptr) {
			break;
		}

		filename[0]    = 0;
		permissions[0] = 0;
		addr           = 0;
		size           = 0;

		// printf("%s", str);

		// NOLINTNEXTLINE(cert-err34-c)
		result = sscanf(str, "%" SCNx64 "-%" SCNx64 " %s %" SCNx64 " %s %" SCNx64 " %s", &addr,
		                &endaddr, permissions, &offset, device, &inode, filename);

		size = endaddr - addr;

		bool read  = (strchr(permissions, 'r') != nullptr);
		bool write = (strchr(permissions, 'w') != nullptr);
		bool exec  = (strchr(permissions, 'x') != nullptr);

		// printf("%016"I64"x, %"I64"d, %s, %d, %d\n", addr, size, filename, read, write);

		if (read && write && !exec && strncmp(filename, "[stack", 6) == 0) {
			// printf("stack: %016"I64"x, %"I64"d\n", addr, size);

			if (check_addr >= addr && check_addr < addr + size) {
				s.addr          = addr;
				s.total_size    = size;
				s.commited_addr = addr;
				s.commited_size = size;

				if (s.code_addr != 0) {
					break;
				}
			}
		}

		if (read && !write && exec && strstr(filename, name) != nullptr) {
			s.code_addr = addr;
			s.code_size = size;

			if (s.addr != 0) {
				break;
			}
		}
	}

	result = fclose(f);

	if (s.reserved_addr == 0) {
		s.reserved_addr = s.addr;
		s.reserved_size = s.total_size;
	}
}

#endif
