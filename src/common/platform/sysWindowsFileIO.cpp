#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// #error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep

#include "common/platform/sysFileIO.h"
#include "common/platform/sysTimer.h"
#include "common/stringUtils.h"

#include <cstdlib>
#include <cstring>
#include <vector>

// NOLINTNEXTLINE(readability-identifier-naming)
enum sys_file_type_t {
	SYS_FILE_ERROR,       // NOLINT(readability-identifier-naming)
	SYS_FILE_MEMORY_STAT, // NOLINT(readability-identifier-naming)
	SYS_FILE_FILE,        // NOLINT(readability-identifier-naming)
	SYS_FILE_MEMORY_DYN   // NOLINT(readability-identifier-naming)
};

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_mem_buf_t {
	uint8_t* base;
	uint8_t* ptr;
	uint32_t size;
};

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_t {
	sys_file_type_t type;
	union {
		HANDLE              handle;
		sys_file_mem_buf_t* buf;
	};
};

// IWYU pragma: no_include <fileapi.h>
// IWYU pragma: no_include <handleapi.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <winbase.h>

constexpr DWORD FILE_SHARE_POSIX = static_cast<DWORD>(FILE_SHARE_READ) |
                                   static_cast<DWORD>(FILE_SHARE_WRITE) |
                                   static_cast<DWORD>(FILE_SHARE_DELETE);

/**
 * @brief Converts a filesystem path to an extended Windows path (with \\?\ or \\?\UNC\ prefix).
 *
 * This allows path lengths to exceed MAX_PATH (260 characters).
 *
 * @param path Input filesystem path.
 * @return Extended-length wide string path formatted for Win32 Unicode APIs.
 */
static std::wstring ToExtendedPath(const std::filesystem::path& path) {
	if (path.empty()) {
		return {};
	}

	std::wstring wide = path.wstring();

	for (auto& c: wide) {
		if (c == L'/') {
			c = L'\\';
		}
	}

	if (wide.rfind(L"\\\\?\\", 0) == 0 || wide.rfind(L"\\\\.\\", 0) == 0) {
		return wide;
	}

	DWORD required = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
	if (required > 0) {
		std::wstring full_path(required, L'\0');
		DWORD len = GetFullPathNameW(wide.c_str(), required, full_path.data(), nullptr);
		if (len > 0) {
			full_path.resize(len);
			for (auto& c: full_path) {
				if (c == L'/') {
					c = L'\\';
				}
			}
			if (full_path.rfind(L"\\\\?\\", 0) == 0 || full_path.rfind(L"\\\\.\\", 0) == 0) {
				return full_path;
			}
			if (full_path.size() >= 2 && full_path[0] == L'\\' && full_path[1] == L'\\') {
				return L"\\\\?\\UNC\\" + full_path.substr(2);
			}
			if (full_path.size() >= 2 &&
			    ((full_path[0] >= L'a' && full_path[0] <= L'z') ||
			     (full_path[0] >= L'A' && full_path[0] <= L'Z')) &&
			    full_path[1] == L':') {
				return L"\\\\?\\" + full_path;
			}
			return full_path;
		}
	}

	if (wide.size() >= 2 && wide[0] == L'\\' && wide[1] == L'\\') {
		return L"\\\\?\\UNC\\" + wide.substr(2);
	}
	if (wide.size() >= 2 &&
	    ((wide[0] >= L'a' && wide[0] <= L'z') || (wide[0] >= L'A' && wide[0] <= L'Z')) &&
	    wide[1] == L':') {
		return L"\\\\?\\" + wide;
	}

	return wide;
}

/**
 * @brief Opens a file or directory handle specifically for querying or updating metadata/timestamps.
 *
 * Uses FILE_FLAG_BACKUP_SEMANTICS to permit opening directory handles without exposing directory
 * handles to regular file read/write APIs.
 *
 * @param name Filesystem path to the file or directory.
 * @param desired_access Win32 access mask (e.g. FILE_READ_ATTRIBUTES or FILE_WRITE_ATTRIBUTES).
 * @return Win32 HANDLE to the opened file or directory, or INVALID_HANDLE_VALUE on failure.
 */
static HANDLE OpenPathForMetadata(const std::filesystem::path& name, DWORD desired_access) {
	auto wide = ToExtendedPath(name);
	return CreateFileW(wide.c_str(), desired_access, FILE_SHARE_POSIX, nullptr, OPEN_EXISTING,
	                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

/**
 * @brief Converts emulator cache hint flags to Win32 file access flags.
 *
 * @param t Cache hint type.
 * @return DWORD Win32 flags for CreateFileW.
 */
static DWORD GetCacheAccessType(sys_file_cache_type_t t) {
	if (t == SYS_FILE_CACHE_RANDOM_ACCESS) {
		return FILE_FLAG_RANDOM_ACCESS;
	}

	if (t == SYS_FILE_CACHE_SEQUENTIAL_SCAN) {
		return FILE_FLAG_SEQUENTIAL_SCAN;
	}

	return FILE_ATTRIBUTE_NORMAL;
}

void SysFileRead(void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_read) {
	if (f.type == SYS_FILE_FILE) {
		DWORD w = 0;
		ReadFile(f.handle, data, size, &w, nullptr);
		if (bytes_read != nullptr) {
			*bytes_read = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT) {
		uint32_t s = size;
		if (f.buf->size != 0u) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		}
		std::memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr) {
			*bytes_read = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN) {
		uint32_t s = size;
		if (f.buf->size != 0u) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		} else {
			s = 0;
		}
		std::memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr) {
			*bytes_read = s;
		}
	}
}

void SysFileWrite(const void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_written) {
	if (f.type == SYS_FILE_FILE) {
		DWORD w = 0;
		WriteFile(f.handle, data, size, &w, nullptr);
		if (bytes_written != nullptr) {
			*bytes_written = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT) {
		uint32_t s = size;
		if (f.buf->size != 0u) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		}
		std::memcpy(f.buf->ptr, data, s);
		f.buf->ptr += s;
		if (bytes_written != nullptr) {
			*bytes_written = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN) {
		uint32_t pos = f.buf->ptr - f.buf->base;
		if (f.buf->size < pos + size) {
			f.buf->base = static_cast<uint8_t*>(std::realloc(f.buf->base, pos + size));
			f.buf->ptr  = f.buf->base + pos;
			f.buf->size = pos + size;
		}
		std::memcpy(f.buf->ptr, data, size);
		f.buf->ptr += size;
		if (bytes_written != nullptr) {
			*bytes_written = size;
		}
	}
}

/**
 * @brief Creates a new file or truncates an existing file.
 *
 * @param file_name Path to the file.
 * @return sys_file_t* Pointer to the allocated file descriptor.
 */
sys_file_t* SysFileCreate(const std::filesystem::path& file_name) {
	auto* ret = new sys_file_t;

	auto   wide   = ToExtendedPath(file_name);
	HANDLE h_file = nullptr;
	h_file = CreateFileW(wide.c_str(),
	                     static_cast<DWORD>(GENERIC_READ) | static_cast<DWORD>(GENERIC_WRITE) |
	                         static_cast<DWORD>(DELETE),
	                     FILE_SHARE_POSIX, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;

	return ret;
}

/**
 * @brief Opens an existing file for reading.
 *
 * @param file_name Path to the file.
 * @param cache_type Cache hint for file buffering.
 * @return sys_file_t* Pointer to the allocated file descriptor.
 */
sys_file_t* SysFileOpenR(const std::filesystem::path& file_name, sys_file_cache_type_t cache_type) {
	auto* ret = new sys_file_t;

	auto   wide   = ToExtendedPath(file_name);
	HANDLE h_file = nullptr;
	h_file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_POSIX, nullptr, OPEN_EXISTING,
	                     GetCacheAccessType(cache_type), nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;

	return ret;
}

sys_file_t* SysFileOpen(uint8_t* buf, uint32_t buf_size) {
	auto* ret = new sys_file_t;

	ret->type      = SYS_FILE_MEMORY_STAT;
	ret->buf       = new sys_file_mem_buf_t;
	ret->buf->base = buf;
	ret->buf->ptr  = buf;
	ret->buf->size = buf_size;

	return ret;
}

sys_file_t* SysFileCreate() {
	auto* ret = new sys_file_t;

	ret->type      = SYS_FILE_MEMORY_DYN;
	ret->buf       = new sys_file_mem_buf_t;
	ret->buf->base = nullptr;
	ret->buf->ptr  = nullptr;
	ret->buf->size = 0;

	return ret;
}

/**
 * @brief Opens an existing file for writing.
 *
 * @param file_name Path to the file.
 * @param cache_type Cache hint for file buffering.
 * @return sys_file_t* Pointer to the allocated file descriptor.
 */
sys_file_t* SysFileOpenW(const std::filesystem::path& file_name, sys_file_cache_type_t cache_type) {
	auto* ret = new sys_file_t;

	auto   wide   = ToExtendedPath(file_name);
	HANDLE h_file = nullptr;
	h_file        = CreateFileW(
	    wide.c_str(), static_cast<DWORD>(GENERIC_WRITE) | static_cast<DWORD>(DELETE),
	    FILE_SHARE_POSIX, nullptr, OPEN_EXISTING, GetCacheAccessType(cache_type), nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;

	return ret;
}

/**
 * @brief Opens an existing file for reading and writing.
 *
 * @param file_name Path to the file.
 * @param cache_type Cache hint for file buffering.
 * @return sys_file_t* Pointer to the allocated file descriptor.
 */
sys_file_t* SysFileOpenRw(const std::filesystem::path& file_name,
                          sys_file_cache_type_t        cache_type) {
	auto* ret = new sys_file_t;

	auto   wide   = ToExtendedPath(file_name);
	HANDLE h_file = nullptr;
	h_file = CreateFileW(wide.c_str(),
	                     static_cast<DWORD>(GENERIC_READ) | static_cast<DWORD>(GENERIC_WRITE) |
	                         static_cast<DWORD>(DELETE),
	                     FILE_SHARE_POSIX, nullptr, OPEN_EXISTING, GetCacheAccessType(cache_type),
	                     nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;

	return ret;
}

void SysFileClose(sys_file_t* f) {
	if (f->type == SYS_FILE_FILE) {
		CloseHandle(f->handle);
	} else if (f->type == SYS_FILE_MEMORY_STAT) {
		delete f->buf;
	} else if (f->type == SYS_FILE_MEMORY_DYN) {
		std::free(f->buf->base);
		delete f->buf;
	}

	// f.type = SYS_FILE_ERROR;
	delete f;
}

uint64_t SysFileSize(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s;
		GetFileSizeEx(f.handle, &s);
		return s.QuadPart;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		return f.buf->size;
	}

	return 0;
}

/**
 * @brief Retrieves the size of a file by path in bytes.
 *
 * @param file_name Path to the file.
 * @return File size in bytes, or 0 if retrieval fails.
 */
uint64_t SysFileSize(const std::filesystem::path& file_name) {
	LARGE_INTEGER             s;
	WIN32_FILE_ATTRIBUTE_DATA a;

	auto wide = ToExtendedPath(file_name);
	if (GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &a) == 0) {
		return 0;
	}

	s.HighPart = static_cast<LONG>(a.nFileSizeHigh);
	s.LowPart  = a.nFileSizeLow;

	return s.QuadPart;
}

bool SysFileTruncate(sys_file_t& f, uint64_t size) {
	bool ok = false;
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s {};
		LARGE_INTEGER r {};
		s.QuadPart = 0;
		SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
		s.QuadPart = static_cast<LONGLONG>(size);
		ok         = (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0 &&
		              SetEndOfFile(f.handle) != 0);
		SetFilePointerEx(f.handle, r, nullptr, FILE_BEGIN);
	}

	return ok;
}

bool SysFileUnlink(sys_file_t& f, const std::filesystem::path& name) {
	if (f.type == SYS_FILE_FILE) {
		FILE_DISPOSITION_INFO info {};
		info.DeleteFile = TRUE;
		if (SetFileInformationByHandle(f.handle, FileDispositionInfo, &info, sizeof(info)) != 0) {
			return true;
		}
	}

	return SysFileDeleteFile(name);
}

bool SysFileSeek(sys_file_t& f, uint64_t offset) {
	bool ok = true;
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s;
		s.QuadPart = static_cast<LONGLONG>(offset);
		ok         = (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0);
		// printf("seek: %u\n", offset);
	} else if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		f.buf->ptr = f.buf->base + offset;
	}

	return ok;
}

uint64_t SysFileTell(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s {};
		LARGE_INTEGER r {};
		s.QuadPart = 0;
		SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
		// printf("tell: %u\n", r.QuadPart);
		return r.QuadPart;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		return f.buf->ptr - f.buf->base;
	}

	return 0;
}

bool SysFileIsError(sys_file_t& f) {
	return f.type == SYS_FILE_ERROR ||
	       (f.type == SYS_FILE_FILE && f.handle == INVALID_HANDLE_VALUE);
}

/**
 * @brief Checks if a directory exists at the given path.
 *
 * @param path Path to check.
 * @return true if the directory exists, false otherwise.
 */
bool SysFileIsDirectoryExisting(const std::filesystem::path& path) {
	auto  wide = ToExtendedPath(path);
	DWORD a    = GetFileAttributesW(wide.c_str());
	return a != INVALID_FILE_ATTRIBUTES &&
	       ((a & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) != 0u);
}

/**
 * @brief Checks if a regular file exists at the given path.
 *
 * @param name Path to check.
 * @return true if the file exists, false otherwise.
 */
bool SysFileIsFileExisting(const std::filesystem::path& name) {
	auto  wide = ToExtendedPath(name);
	DWORD a    = GetFileAttributesW(wide.c_str());
	return a != INVALID_FILE_ATTRIBUTES &&
	       ((a & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) == 0u);
}

/**
 * @brief Creates a directory on the filesystem.
 *
 * @param path Directory path to create.
 * @return true on success, false on failure.
 */
bool SysFileCreateDirectory(const std::filesystem::path& path) {
	auto wide = ToExtendedPath(path);
	return CreateDirectoryW(wide.c_str(), nullptr) != 0;
}

/**
 * @brief Deletes a directory from the filesystem.
 *
 * @param path Directory path to remove.
 * @return true on success, false on failure.
 */
bool SysFileDeleteDirectory(const std::filesystem::path& path) {
	auto wide = ToExtendedPath(path);
	return RemoveDirectoryW(wide.c_str()) != 0;
}

/**
 * @brief Deletes a regular file from the filesystem.
 *
 * @param name Path to the file to delete.
 * @return true on success, false on failure.
 */
bool SysFileDeleteFile(const std::filesystem::path& name) {
	auto wide = ToExtendedPath(name);
	return DeleteFileW(wide.c_str()) != 0;
}

bool SysFileFlush(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE && f.handle != INVALID_HANDLE_VALUE) {
		return (FlushFileBuffers(f.handle) != 0);
	}

	return false;
}

/**
 * @brief Retrieves the last access time in UTC for a file or directory.
 *
 * @param name Path to the file or directory.
 * @return SysFileTimeStruct with timestamp or marked invalid.
 */
SysFileTimeStruct SysFileGetLastAccessTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};
	HANDLE            h = OpenPathForMetadata(name, FILE_READ_ATTRIBUTES);
	r.is_invalid =
	    (h == INVALID_HANDLE_VALUE || (GetFileTime(h, nullptr, &r.time, nullptr) == 0));
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
	return r;
}

/**
 * @brief Retrieves the last write time in UTC for a file or directory.
 *
 * @param name Path to the file or directory.
 * @return SysFileTimeStruct with timestamp or marked invalid.
 */
SysFileTimeStruct SysFileGetLastWriteTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};
	HANDLE            h = OpenPathForMetadata(name, FILE_READ_ATTRIBUTES);
	r.is_invalid =
	    (h == INVALID_HANDLE_VALUE || (GetFileTime(h, nullptr, nullptr, &r.time) == 0));
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
	return r;
}

/**
 * @brief Retrieves both last access and last write times in UTC for a file or directory.
 *
 * @param name Path to the file or directory.
 * @param a Output struct for access time.
 * @param w Output struct for write time.
 */
void SysFileGetLastAccessAndWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	HANDLE h = OpenPathForMetadata(name, FILE_READ_ATTRIBUTES);
	a.is_invalid = w.is_invalid =
	    (h == INVALID_HANDLE_VALUE || (GetFileTime(h, nullptr, &a.time, &w.time) == 0));
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
}

/**
 * @brief Retrieves last access and last write times from an already open file descriptor.
 *
 * @param f Open file descriptor.
 * @param a Output struct for access time.
 * @param w Output struct for write time.
 */
void SysFileGetLastAccessAndWriteTimeUtc(sys_file_t& f, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	if (f.type == SYS_FILE_FILE) {
		a.is_invalid = w.is_invalid = (GetFileTime(f.handle, nullptr, &a.time, &w.time) == 0);
	} else if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		SysTimeStruct t {};
		SysGetSystemTimeUtc(t);
		SysSystemToFileTimeUtc(t, a);
		SysSystemToFileTimeUtc(t, w);
	} else {
		a.is_invalid = w.is_invalid = true;
	}
}

/**
 * @brief Sets the last access time in UTC for a file or directory.
 *
 * @param name Path to the file or directory.
 * @param access New access timestamp.
 * @return true on success, false on failure.
 */
bool SysFileSetLastAccessTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& access) {
	if (access.is_invalid) {
		return false;
	}

	HANDLE h = OpenPathForMetadata(name, FILE_WRITE_ATTRIBUTES);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}

	bool ok = (SetFileTime(h, nullptr, &access.time, nullptr) != 0);
	CloseHandle(h);
	return ok;
}

/**
 * @brief Sets the last write time in UTC for a file or directory.
 *
 * @param name Path to the file or directory.
 * @param write New write timestamp.
 * @return true on success, false on failure.
 */
bool SysFileSetLastWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& write) {
	if (write.is_invalid) {
		return false;
	}

	HANDLE h = OpenPathForMetadata(name, FILE_WRITE_ATTRIBUTES);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}

	bool ok = (SetFileTime(h, nullptr, nullptr, &write.time) != 0);
	CloseHandle(h);
	return ok;
}

/**
 * @brief Sets both last access and last write times in UTC for a file or directory.
 *
 * @param name Path to the file or directory.
 * @param access New access timestamp.
 * @param write New write timestamp.
 * @return true on success, false on failure.
 */
bool SysFileSetLastAccessAndWriteTimeUtc(const std::filesystem::path& name,
                                         SysFileTimeStruct& access, SysFileTimeStruct& write) {
	if (access.is_invalid || write.is_invalid) {
		return false;
	}

	HANDLE h = OpenPathForMetadata(name, FILE_WRITE_ATTRIBUTES);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}

	bool ok = (SetFileTime(h, nullptr, &access.time, &write.time) != 0);
	CloseHandle(h);
	return ok;
}

/**
 * @brief Enumerates directory entries for a given path.
 *
 * @param path Directory path to enumerate.
 * @param out Vector populated with discovered directory entries.
 */
void SysFileGetDents(const std::filesystem::path& path, std::vector<sys_dir_entry_t>& out) {
	auto extended = ToExtendedPath(path);
	if (!extended.empty() && extended.back() != L'\\') {
		extended += L'\\';
	}
	extended += L'*';

	WIN32_FIND_DATAW data {};
	HANDLE h = FindFirstFileW(extended.c_str(), &data);

	if (h == INVALID_HANDLE_VALUE) {
		return;
	}

	do {
		std::filesystem::path file_name(data.cFileName);

		sys_dir_entry_t r {};

		r.is_file = ((data.dwFileAttributes & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) == 0u);
		r.name    = Common::PathToString(file_name);

		out.push_back(std::move(r));

	} while (FindNextFileW(h, &data) != 0);

	FindClose(h);
}

/**
 * @brief Copies a file from source path to destination path.
 *
 * @param src Source file path.
 * @param dst Destination file path.
 * @return true on success, false on failure.
 */
bool SysFileCopyFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = ToExtendedPath(src);
	auto dst_wide = ToExtendedPath(dst);
	return CopyFileW(src_wide.c_str(), dst_wide.c_str(), FALSE) != 0;
}

/**
 * @brief Renames or moves a file or directory.
 *
 * @param src Source path.
 * @param dst Destination path.
 * @return true on success, false on failure.
 */
bool SysFileRenameFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = ToExtendedPath(src);
	auto dst_wide = ToExtendedPath(dst);
	return MoveFileW(src_wide.c_str(), dst_wide.c_str()) != 0;
}

/**
 * @brief Clears the read-only attribute from a file or directory.
 *
 * @param name Path to the file or directory.
 */
void SysFileRemoveReadonly(const std::filesystem::path& name) {
	auto  wide  = ToExtendedPath(name);
	DWORD attrs = GetFileAttributesW(wide.c_str());
	if (attrs != INVALID_FILE_ATTRIBUTES) {
		SetFileAttributesW(wide.c_str(), attrs & (~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)));
	}
}

#endif
