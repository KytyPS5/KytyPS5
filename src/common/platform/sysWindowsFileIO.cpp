#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// #error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep

#include "common/platform/sysFileIO.h"
#include "common/stringUtils.h"


// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_t {
	HANDLE handle;
};

// IWYU pragma: no_include <fileapi.h>
// IWYU pragma: no_include <handleapi.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <winbase.h>

constexpr DWORD FILE_SHARE_POSIX = static_cast<DWORD>(FILE_SHARE_READ) |
                                   static_cast<DWORD>(FILE_SHARE_WRITE) |
                                   static_cast<DWORD>(FILE_SHARE_DELETE);
constexpr DWORD READ_WRITE_DELETE = static_cast<DWORD>(GENERIC_READ) |
                                    static_cast<DWORD>(GENERIC_WRITE) | static_cast<DWORD>(DELETE);

void SysFileRead(void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_read) {
	DWORD r = 0;
	ReadFile(f.handle, data, size, &r, nullptr);
	if (bytes_read != nullptr) {
		*bytes_read = r;
	}
}

void SysFileWrite(const void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_written) {
	DWORD w = 0;
	WriteFile(f.handle, data, size, &w, nullptr);
	if (bytes_written != nullptr) {
		*bytes_written = w;
	}
}

static sys_file_t* OpenHostFile(const std::filesystem::path& file_name, DWORD access,
                                DWORD disposition) {
	HANDLE handle = CreateFileW(file_name.c_str(), access, FILE_SHARE_POSIX, nullptr, disposition,
	                            FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return nullptr;
	}
	return new sys_file_t {handle};
}

sys_file_t* SysFileCreate(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, READ_WRITE_DELETE, CREATE_ALWAYS);
}

sys_file_t* SysFileOpenR(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, GENERIC_READ, OPEN_EXISTING);
}

sys_file_t* SysFileOpenW(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, static_cast<DWORD>(GENERIC_WRITE) | static_cast<DWORD>(DELETE),
	                    OPEN_EXISTING);
}

sys_file_t* SysFileOpenRw(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, READ_WRITE_DELETE, OPEN_EXISTING);
}

void SysFileClose(sys_file_t* f) {
	CloseHandle(f->handle);
	delete f;
}

uint64_t SysFileSize(sys_file_t& f) {
	LARGE_INTEGER s;
	GetFileSizeEx(f.handle, &s);
	return s.QuadPart;
}

bool SysFileGetInfo(const std::filesystem::path& name, bool* is_file, uint64_t* size) {
	const bool directory_path = !name.empty() && !name.has_filename() && name != name.root_path();
	WIN32_FILE_ATTRIBUTE_DATA info {};
	if (GetFileAttributesExW(directory_path ? name.parent_path().c_str() : name.c_str(),
	                         GetFileExInfoStandard, &info) == 0) {
		return false;
	}
	const bool file = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	if (directory_path && file) {
		return false;
	}
	*is_file = file;
	*size = *is_file ? ((static_cast<uint64_t>(info.nFileSizeHigh) << 32u) | info.nFileSizeLow) : 0;
	return true;
}

bool SysFileTruncate(sys_file_t& f, uint64_t size) {
	LARGE_INTEGER s {};
	LARGE_INTEGER r {};
	s.QuadPart = 0;
	SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
	s.QuadPart = static_cast<LONGLONG>(size);
	const bool ok =
	    (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0 && SetEndOfFile(f.handle) != 0);
	SetFilePointerEx(f.handle, r, nullptr, FILE_BEGIN);
	return ok;
}

bool SysFileUnlink(sys_file_t& f, const std::filesystem::path& name) {
	FILE_DISPOSITION_INFO info {};
	info.DeleteFile = TRUE;
	if (SetFileInformationByHandle(f.handle, FileDispositionInfo, &info, sizeof(info)) != 0) {
		return true;
	}

	return SysFileDeleteFile(name);
}

bool SysFileSeek(sys_file_t& f, uint64_t offset) {
	LARGE_INTEGER s;
	s.QuadPart = static_cast<LONGLONG>(offset);
	return SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0;
}

uint64_t SysFileTell(sys_file_t& f) {
	LARGE_INTEGER s {};
	LARGE_INTEGER r {};
	s.QuadPart = 0;
	SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
	return r.QuadPart;
}

bool SysFileCreateDirectory(const std::filesystem::path& path) {
	auto wide = path.wstring();
	return CreateDirectoryW(wide.c_str(), nullptr) != 0;
}

bool SysFileDeleteFile(const std::filesystem::path& name) {
	auto wide = name.wstring();
	return DeleteFileW(wide.c_str()) != 0;
}

bool SysFileFlush(sys_file_t& f) {
	return FlushFileBuffers(f.handle) != 0;
}

SysFileTimeStruct SysFileGetLastWriteTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct a {};
	SysFileTimeStruct w {};
	SysFileGetLastAccessAndWriteTimeUtc(name, a, w);
	return w;
}

void SysFileGetLastAccessAndWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	sys_file_t* f = SysFileOpenR(name);
	if (f == nullptr) {
		a.is_invalid = w.is_invalid = true;
		return;
	}
	SysFileGetLastAccessAndWriteTimeUtc(*f, a, w);
	SysFileClose(f);
}

void SysFileGetLastAccessAndWriteTimeUtc(sys_file_t& f, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	a.is_invalid = w.is_invalid = (GetFileTime(f.handle, nullptr, &a.time, &w.time) == 0);
}

void SysFileGetDents(const std::filesystem::path& path, std::vector<sys_dir_entry_t>& out) {
	const auto pattern = path / L"*";

	WIN32_FIND_DATAW data {};
	HANDLE           h = FindFirstFileW(pattern.c_str(), &data);

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

bool SysFileRenameFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = src.wstring();
	auto dst_wide = dst.wstring();
	return MoveFileW(src_wide.c_str(), dst_wide.c_str()) != 0;
}

#endif
