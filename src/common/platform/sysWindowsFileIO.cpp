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

static DWORD GetCacheAccessType(sys_file_cache_type_t t) {
	DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
	if (t == SYS_FILE_CACHE_RANDOM_ACCESS) {
		return flags | FILE_FLAG_RANDOM_ACCESS;
	}

	if (t == SYS_FILE_CACHE_SEQUENTIAL_SCAN) {
		return flags | FILE_FLAG_SEQUENTIAL_SCAN;
	}

	return flags | FILE_ATTRIBUTE_NORMAL;
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

bool SysFileIsDirectoryExisting(const std::filesystem::path& path) {
	auto  wide = ToExtendedPath(path);
	DWORD a    = GetFileAttributesW(wide.c_str());
	return a != INVALID_FILE_ATTRIBUTES &&
	       ((a & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) != 0u);
}

bool SysFileIsFileExisting(const std::filesystem::path& name) {
	auto  wide = ToExtendedPath(name);
	DWORD a    = GetFileAttributesW(wide.c_str());
	return a != INVALID_FILE_ATTRIBUTES &&
	       ((a & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) == 0u);
}

bool SysFileCreateDirectory(const std::filesystem::path& path) {
	auto wide = ToExtendedPath(path);
	return CreateDirectoryW(wide.c_str(), nullptr) != 0;
}

bool SysFileDeleteDirectory(const std::filesystem::path& path) {
	auto wide = ToExtendedPath(path);
	return RemoveDirectoryW(wide.c_str()) != 0;
}

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

SysFileTimeStruct SysFileGetLastAccessTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};
	sys_file_t*       f = SysFileOpenR(name);
	r.is_invalid =
	    (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, &r.time, nullptr) == 0));
	SysFileClose(f);
	return r;
}

SysFileTimeStruct SysFileGetLastWriteTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};
	sys_file_t*       f = SysFileOpenR(name);
	r.is_invalid =
	    (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, nullptr, &r.time) == 0));
	SysFileClose(f);
	return r;
}

void SysFileGetLastAccessAndWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	sys_file_t* f = SysFileOpenR(name);
	a.is_invalid  = w.is_invalid =
	    (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, &a.time, &w.time) == 0));
	SysFileClose(f);
}

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

bool SysFileSetLastAccessTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& access) {
	if (access.is_invalid) {
		return false;
	}

	bool ok = true;

	sys_file_t* f = SysFileOpenW(name);

	ok = !(f->type == SYS_FILE_ERROR ||
	       (SetFileTime(f->handle, nullptr, &access.time, nullptr) == 0));

	SysFileClose(f);

	return ok;
}

bool SysFileSetLastWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& write) {
	if (write.is_invalid) {
		return false;
	}

	bool ok = true;

	sys_file_t* f = SysFileOpenW(name);

	ok = !(f->type == SYS_FILE_ERROR ||
	       (SetFileTime(f->handle, nullptr, nullptr, &write.time) == 0));

	SysFileClose(f);

	return ok;
}

bool SysFileSetLastAccessAndWriteTimeUtc(const std::filesystem::path& name,
                                         SysFileTimeStruct& access, SysFileTimeStruct& write) {
	if (access.is_invalid || write.is_invalid) {
		return false;
	}

	bool ok = true;

	sys_file_t* f = SysFileOpenW(name);

	ok = !(f->type == SYS_FILE_ERROR ||
	       (SetFileTime(f->handle, nullptr, &access.time, &write.time) == 0));

	SysFileClose(f);

	return ok;
}

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

bool SysFileCopyFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = ToExtendedPath(src);
	auto dst_wide = ToExtendedPath(dst);
	return CopyFileW(src_wide.c_str(), dst_wide.c_str(), FALSE) != 0;
}

bool SysFileRenameFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = ToExtendedPath(src);
	auto dst_wide = ToExtendedPath(dst);
	return MoveFileW(src_wide.c_str(), dst_wide.c_str()) != 0;
}

void SysFileRemoveReadonly(const std::filesystem::path& name) {
	auto  wide  = ToExtendedPath(name);
	DWORD attrs = GetFileAttributesW(wide.c_str());
	if (attrs != INVALID_FILE_ATTRIBUTES) {
		SetFileAttributesW(wide.c_str(), attrs & (~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)));
	}
}

#endif
