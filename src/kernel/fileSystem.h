#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "kernel/pthread.h"

#include <filesystem>

namespace Libs::LibKernel::FileSystem {

struct FileStat {
	uint32_t       st_dev;
	uint32_t       st_ino;
	uint16_t       st_mode;
	uint16_t       st_nlink;
	uint32_t       st_uid;
	uint32_t       st_gid;
	uint32_t       st_rdev;
	KernelTimespec st_atim;
	KernelTimespec st_mtim;
	KernelTimespec st_ctim;
	int64_t        st_size;
	int64_t        st_blocks;
	uint32_t       st_blksize;
	uint32_t       st_flags;
	uint32_t       st_gen;
	int32_t        st_lspare;
	KernelTimespec st_birthtim;
	unsigned int: (8 / 2) * (16 - static_cast<int>(sizeof(KernelTimespec)));
	unsigned int: (8 / 2) * (16 - static_cast<int>(sizeof(KernelTimespec)));
};

KYTY_SUBSYSTEM_DEFINE(FileSystem);

void                  Mount(const std::filesystem::path& folder, const std::string& point);
void                  Umount(const std::string& folder_or_point);
std::filesystem::path GetRealFilename(const std::string& mounted_file_name);

int KYTY_SYSV_ABI     KernelOpen(const char* path, int flags, uint16_t mode);
int KYTY_SYSV_ABI     KernelClose(int d);
int64_t KYTY_SYSV_ABI KernelRead(int d, void* buf, size_t nbytes);
int64_t KYTY_SYSV_ABI KernelPread(int d, void* buf, size_t nbytes, int64_t offset);
int64_t KYTY_SYSV_ABI KernelWrite(int d, const void* buf, size_t nbytes);
int64_t KYTY_SYSV_ABI KernelPwrite(int d, const void* buf, size_t nbytes, int64_t offset);
int64_t KYTY_SYSV_ABI KernelLseek(int d, int64_t offset, int whence);
int KYTY_SYSV_ABI     KernelStat(const char* path, FileStat* sb);
int KYTY_SYSV_ABI     KernelFstat(int d, FileStat* sb);
int KYTY_SYSV_ABI     KernelUnlink(const char* path);
int KYTY_SYSV_ABI     KernelRename(const char* from, const char* to);
int KYTY_SYSV_ABI     KernelGetdirentries(int fd, char* buf, int nbytes, int64_t* basep);
int KYTY_SYSV_ABI     KernelGetdents(int fd, char* buf, int nbytes);
int KYTY_SYSV_ABI     KernelMkdir(const char* path, uint16_t mode);
int KYTY_SYSV_ABI     KernelRmdir(const char* path);
int KYTY_SYSV_ABI     KernelCheckReachability(const char* path);

} // namespace Libs::LibKernel::FileSystem

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_ */
