#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
// #error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include "common/platform/sysFileIO.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_t {
	FILE* f;
};

// Darwin uses BSD timestamp member names.
#if defined(__APPLE__)
#define KYTY_STAT_ATIME_NS(st) ((st).st_atimespec.tv_nsec)
#define KYTY_STAT_MTIME_NS(st) ((st).st_mtimespec.tv_nsec)
#else
#define KYTY_STAT_ATIME_NS(st) ((st).st_atim.tv_nsec)
#define KYTY_STAT_MTIME_NS(st) ((st).st_mtim.tv_nsec)
#endif

static std::filesystem::path get_internal_name(const std::filesystem::path& name) {
	return name.empty() || name.is_absolute() ? name : (std::filesystem::path(".") / name);
}

static std::atomic<SysFileRecoverableDestination> g_recoverable_destination {nullptr};

void SysFileSetRecoverableDestination(SysFileRecoverableDestination probe) {
	g_recoverable_destination.store(probe, std::memory_order_release);
}

void SysFileRead(void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_read) {
	const off_t start = ftello(f.f);
	size_t      w     = fread(data, 1, size, f.f);
	if (w < size && ferror(f.f) && errno == EFAULT && start >= 0) {
		clearerr(f.f);
		thread_local std::vector<uint8_t> chunk(1u << 20u);
		if (fseeko(f.f, start + static_cast<off_t>(w), SEEK_SET) == 0) {
			size_t      remaining = size - w;
			struct stat file_stat {};
			if (fstat(fileno(f.f), &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
				const off_t at = start + static_cast<off_t>(w);
				remaining = file_stat.st_size > at
				                ? std::min(remaining, static_cast<size_t>(file_stat.st_size - at))
				                : 0;
			}
			while (remaining != 0) {
				const size_t step      = std::min(remaining, chunk.size());
				const auto recoverable = g_recoverable_destination.load(std::memory_order_acquire);
				if (recoverable == nullptr || !recoverable(static_cast<uint8_t*>(data) + w, step)) {
					break;
				}
				const size_t got = fread(chunk.data(), 1, step, f.f);
				if (got == 0) {
					break;
				}
				std::memcpy(static_cast<uint8_t*>(data) + w, chunk.data(), got);
				w += got;
				remaining -= got;
				if (got < step) {
					break;
				}
			}
			fseeko(f.f, start + static_cast<off_t>(w), SEEK_SET);
		}
	}
	if (bytes_read != nullptr) {
		*bytes_read = w;
	}
}

void SysFileWrite(const void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_written) {
	size_t w = fwrite(data, 1, size, f.f);
	if (bytes_written != nullptr) {
		*bytes_written = w;
	}
}

static sys_file_t* OpenHostFile(const std::filesystem::path& file_name, const char* mode) {
	FILE* f = fopen(get_internal_name(file_name).c_str(), mode);
	if (f == nullptr) {
		return nullptr;
	}
	// Independent handles must observe writes and truncation immediately.
	setvbuf(f, nullptr, _IONBF, 0);
	return new sys_file_t {f};
}

sys_file_t* SysFileCreate(const std::filesystem::path& file_name) {
	auto* ret = OpenHostFile(file_name, "w+");
	if (ret == nullptr) {
		printf("can't create file: %s\n", file_name.c_str());
	}
	return ret;
}

sys_file_t* SysFileOpenR(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, "r");
}

sys_file_t* SysFileOpenW(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, "r+");
}

sys_file_t* SysFileOpenRw(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, "r+");
}

void SysFileClose(sys_file_t* f) {
	fclose(f->f);
	delete f;
}

uint64_t SysFileSize(sys_file_t& f) {
	// Preserve sizes above 4 GiB.
	const off_t pos = ftello(f.f);
	if (pos < 0) {
		return 0;
	}
	if (fseeko(f.f, 0, SEEK_END) != 0) {
		return 0;
	}
	const off_t size = ftello(f.f);
	fseeko(f.f, pos, SEEK_SET);
	return (size < 0 ? 0 : static_cast<uint64_t>(size));
}

bool SysFileGetInfo(const std::filesystem::path& name, bool* is_file, uint64_t* size) {
	const auto  path = get_internal_name(name);
	struct stat info {};
	if (stat(path.c_str(), &info) != 0) {
		return false;
	}
	*is_file = !S_ISDIR(info.st_mode);
	*size    = *is_file ? static_cast<uint64_t>(info.st_size) : 0;
	return true;
}

bool SysFileTruncate(sys_file_t& f, uint64_t size) {
	// Flush before resizing and restore the caller's position.
	const auto position = ftell(f.f);
	fflush(f.f);
	const bool ok = (ftruncate(fileno(f.f), static_cast<off_t>(size)) == 0);
	if (position >= 0) {
		fseek(f.f, position, SEEK_SET);
	}
	return ok;
}

bool SysFileUnlink(sys_file_t& /*f*/, const std::filesystem::path& name) {
	return SysFileDeleteFile(name);
}

bool SysFileSeek(sys_file_t& f, uint64_t offset) {
	return fseek(f.f, static_cast<int64_t>(offset), SEEK_SET) == 0;
}

uint64_t SysFileTell(sys_file_t& f) {
	return ftell(f.f);
}

bool SysFileCreateDirectory(const std::filesystem::path& path) {
	auto real_name     = get_internal_name(path);
	auto real_name_str = real_name.string();

	mode_t m = S_IRWXU | S_IRWXG | S_IRWXO; // NOLINT

	int r = mkdir(real_name_str.c_str(), m);

	if (r != 0) {
		int e = errno;
		if (e == EEXIST) {
			printf("mkdir(%s, %" PRIx32 ") failed: The named file exists\n", real_name_str.c_str(),
			       static_cast<uint32_t>(m));
		} else {
			printf("mkdir(%s, %" PRIx32 ") failed: %d\n", real_name_str.c_str(),
			       static_cast<uint32_t>(m), e);
			return false;
		}
	}

	return true;
}

bool SysFileDeleteFile(const std::filesystem::path& name) {
	auto real_name     = get_internal_name(name);
	auto real_name_str = real_name.string();

	return 0 == unlink(real_name_str.c_str());
}

bool SysFileFlush(sys_file_t& f) {
	return fflush(f.f) == 0 && fsync(fileno(f.f)) == 0;
}

SysFileTimeStruct SysFileGetLastWriteTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};

	auto real_name     = get_internal_name(name);
	auto real_name_str = real_name.string();

	struct stat s {};

	if (0 != stat(real_name_str.c_str(), &s)) {
		r.is_invalid = true;
	} else {
		r.is_invalid = false;
		r.time       = s.st_mtime;
		r.nanos      = KYTY_STAT_MTIME_NS(s);
	}

	return r;
}

void SysFileGetLastAccessAndWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	auto real_name     = get_internal_name(name);
	auto real_name_str = real_name.string();

	struct stat s {};

	if (0 != stat(real_name_str.c_str(), &s)) {
		a.is_invalid = true;
		w.is_invalid = true;
	} else {
		a.is_invalid = false;
		w.is_invalid = false;
		a.time       = s.st_atime;
		a.nanos      = KYTY_STAT_ATIME_NS(s);
		w.time       = s.st_mtime;
		w.nanos      = KYTY_STAT_MTIME_NS(s);
	}
}

void SysFileGetLastAccessAndWriteTimeUtc(sys_file_t& f, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	struct stat s {};

	const bool ok = (0 == fstat(fileno(f.f), &s));

	a.is_invalid = w.is_invalid = !ok;

	if (ok) {
		a.time  = s.st_atime;
		a.nanos = KYTY_STAT_ATIME_NS(s);
		w.time  = s.st_mtime;
		w.nanos = KYTY_STAT_MTIME_NS(s);
	}
}

// Keep "." and ".." to match FindFirstFileW.
void SysFileGetDents(const std::filesystem::path& path, std::vector<sys_dir_entry_t>& out) {
	auto real_path = get_internal_name(path);

	DIR* dir = opendir(real_path.string().c_str());
	if (dir == nullptr) {
		return;
	}

	for (const dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
		sys_dir_entry_t r {};

		r.name = entry->d_name;

		if (entry->d_type == DT_UNKNOWN) {
			// Some filesystems do not populate d_type.
			struct stat s {};
			r.is_file = 0 == lstat((real_path / r.name).string().c_str(), &s) && S_ISREG(s.st_mode);
		} else {
			r.is_file = entry->d_type != DT_DIR;
		}

		out.push_back(r);
	}

	closedir(dir);
}

bool SysFileRenameFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto real_src = get_internal_name(src);
	auto real_dst = get_internal_name(dst);

	// Match MoveFileW: fail when the destination exists.
	std::error_code error;
	if (std::filesystem::exists(real_dst, error)) {
		return false;
	}

	return 0 == rename(real_src.string().c_str(), real_dst.string().c_str());
}

#endif
