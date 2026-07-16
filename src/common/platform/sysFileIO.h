#ifndef KYTY_COMMON_PLATFORM_SYSFILEIO_H_
#define KYTY_COMMON_PLATFORM_SYSFILEIO_H_

#include "common/common.h"
#include "common/platform/sysTimer.h"

#include <filesystem>
#include <string>
#include <vector>

// The native file representation is private to the platform implementation.
struct sys_file_t; // NOLINT(readability-identifier-naming)

// NOLINTNEXTLINE(readability-identifier-naming)
enum sys_file_cache_type_t {
	SYS_FILE_CACHE_AUTO            = 0, // NOLINT(readability-identifier-naming)
	SYS_FILE_CACHE_RANDOM_ACCESS   = 1, // NOLINT(readability-identifier-naming)
	SYS_FILE_CACHE_SEQUENTIAL_SCAN = 2  // NOLINT(readability-identifier-naming)
};

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_find_t {
	std::filesystem::path path_with_name;
	SysFileTimeStruct     last_access_time;
	SysFileTimeStruct     last_write_time;
	uint64_t              size;
};

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_dir_entry_t {
	std::string name;
	bool        is_file;
};

void        SysFileRead(void* data, uint32_t size, sys_file_t& f,
                        uint32_t* bytes_read = nullptr); // NOLINT(google-runtime-references)
void        SysFileWrite(const void* data, uint32_t size, sys_file_t& f,
                         uint32_t* bytes_written = nullptr); // NOLINT(google-runtime-references)
sys_file_t* SysFileCreate(const std::filesystem::path& file_name);
sys_file_t* SysFileOpenR(const std::filesystem::path& file_name,
                         sys_file_cache_type_t        cache_type = SYS_FILE_CACHE_AUTO);
sys_file_t* SysFileOpenW(const std::filesystem::path& file_name,
                         sys_file_cache_type_t        cache_type = SYS_FILE_CACHE_AUTO);
sys_file_t* SysFileOpen(uint8_t* buf, uint32_t buf_size);
sys_file_t* SysFileCreate();
sys_file_t* SysFileOpenRw(const std::filesystem::path& file_name,
                          sys_file_cache_type_t        cache_type = SYS_FILE_CACHE_AUTO);
void        SysFileClose(sys_file_t* f);
uint64_t    SysFileSize(sys_file_t& f);                    // NOLINT(google-runtime-references)
bool        SysFileSeek(sys_file_t& f, uint64_t offset);   // NOLINT(google-runtime-references)
uint64_t    SysFileTell(sys_file_t& f);                    // NOLINT(google-runtime-references)
bool        SysFileTruncate(sys_file_t& f, uint64_t size); // NOLINT(google-runtime-references)
bool        SysFileUnlink(sys_file_t&                  f,
                          const std::filesystem::path& name); // NOLINT(google-runtime-references)
void        SysFileWrite(uint32_t n, sys_file_t& f);          // NOLINT(google-runtime-references)
uint64_t    SysFileSize(const std::filesystem::path& file_name);
bool        SysFileIsError(sys_file_t& f); // NOLINT(google-runtime-references)
bool        SysFileIsDirectoryExisting(const std::filesystem::path& path);
bool        SysFileIsFileExisting(const std::filesystem::path& name);
bool        SysFileCreateDirectory(const std::filesystem::path& path);
bool        SysFileDeleteDirectory(const std::filesystem::path& path);
bool        SysFileDeleteFile(const std::filesystem::path& name);
bool        SysFileFlush(sys_file_t& f); // NOLINT(google-runtime-references)
SysFileTimeStruct SysFileGetLastAccessTimeUtc(const std::filesystem::path& name);
SysFileTimeStruct SysFileGetLastWriteTimeUtc(const std::filesystem::path& name);
// NOLINTNEXTLINE(google-runtime-references)
void SysFileGetLastAccessAndWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w);
// NOLINTNEXTLINE(google-runtime-references)
void SysFileGetLastAccessAndWriteTimeUtc(sys_file_t& f, SysFileTimeStruct& a, SysFileTimeStruct& w);
// NOLINTNEXTLINE(google-runtime-references)
bool SysFileSetLastAccessTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& access);
// NOLINTNEXTLINE(google-runtime-references)
bool SysFileSetLastWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& write);
// NOLINTNEXTLINE(google-runtime-references)
bool SysFileSetLastAccessAndWriteTimeUtc(const std::filesystem::path& name,
                                         SysFileTimeStruct& access, SysFileTimeStruct& write);
// NOLINTNEXTLINE(google-runtime-references)
void SysFileFindFiles(const std::filesystem::path& path, std::vector<sys_file_find_t>& out);
// NOLINTNEXTLINE(google-runtime-references)
void SysFileGetDents(const std::filesystem::path& path, std::vector<sys_dir_entry_t>& out);
bool SysFileCopyFile(const std::filesystem::path& src, const std::filesystem::path& dst);
bool SysFileMoveFile(const std::filesystem::path& src, const std::filesystem::path& dst);
void SysFileRemoveReadonly(const std::filesystem::path& name);

#endif /* KYTY_COMMON_PLATFORM_SYSFILEIO_H_ */
