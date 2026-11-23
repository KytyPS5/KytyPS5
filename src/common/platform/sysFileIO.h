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
struct sys_dir_entry_t {
	std::string name;
	bool        is_file;
};

void        SysFileRead(void* data, uint32_t size, sys_file_t& f,
                        uint32_t* bytes_read = nullptr); // NOLINT(google-runtime-references)
void        SysFileWrite(const void* data, uint32_t size, sys_file_t& f,
                         uint32_t* bytes_written = nullptr); // NOLINT(google-runtime-references)
// The open functions return nullptr on failure.
sys_file_t* SysFileCreate(const std::filesystem::path& file_name);
sys_file_t* SysFileOpenR(const std::filesystem::path& file_name);
sys_file_t* SysFileOpenW(const std::filesystem::path& file_name);
sys_file_t* SysFileOpenRw(const std::filesystem::path& file_name);
void        SysFileClose(sys_file_t* f);
uint64_t    SysFileSize(sys_file_t& f);                    // NOLINT(google-runtime-references)
bool        SysFileSeek(sys_file_t& f, uint64_t offset);   // NOLINT(google-runtime-references)
uint64_t    SysFileTell(sys_file_t& f);                    // NOLINT(google-runtime-references)
bool        SysFileTruncate(sys_file_t& f, uint64_t size); // NOLINT(google-runtime-references)
bool        SysFileUnlink(sys_file_t&                  f,
                          const std::filesystem::path& name); // NOLINT(google-runtime-references)
bool        SysFileGetInfo(const std::filesystem::path& name, bool* is_file, uint64_t* size);
bool        SysFileCreateDirectory(const std::filesystem::path& path);
bool        SysFileDeleteFile(const std::filesystem::path& name);
bool        SysFileFlush(sys_file_t& f); // NOLINT(google-runtime-references)
SysFileTimeStruct SysFileGetLastWriteTimeUtc(const std::filesystem::path& name);
// NOLINTNEXTLINE(google-runtime-references)
void SysFileGetLastAccessAndWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w);
// NOLINTNEXTLINE(google-runtime-references)
void SysFileGetLastAccessAndWriteTimeUtc(sys_file_t& f, SysFileTimeStruct& a, SysFileTimeStruct& w);
// NOLINTNEXTLINE(google-runtime-references)
void SysFileGetDents(const std::filesystem::path& path, std::vector<sys_dir_entry_t>& out);
bool SysFileRenameFile(const std::filesystem::path& src, const std::filesystem::path& dst);

#endif /* KYTY_COMMON_PLATFORM_SYSFILEIO_H_ */
