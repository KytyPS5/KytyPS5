#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
// #error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include "common/assert.h"
#include "common/platform/sysFileIO.h"
#include "common/platform/sysTimer.h"
#include "common/stringUtils.h"

#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

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
		FILE*               f;
		sys_file_mem_buf_t* buf;
	};
};

static std::filesystem::path get_internal_name(const std::filesystem::path& name) {
	return name.is_absolute() ? name : (std::filesystem::path(".") / name);
}

void SysFileRead(void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_read) {
	if (f.type == SYS_FILE_FILE) {
		size_t w = fread(data, 1, size, f.f);
		if (bytes_read != nullptr) {
			*bytes_read = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT) {
		uint32_t s = size;
		if (f.buf->size != 0) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		}
		memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr) {
			*bytes_read = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN) {
		uint32_t s = size;
		if (f.buf->size != 0) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		} else {
			s = 0;
		}
		memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr) {
			*bytes_read = s;
		}
	}
}

void SysFileWrite(const void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_written) {
	if (f.type == SYS_FILE_FILE) {
		size_t w = fwrite(data, 1, size, f.f);
		if (bytes_written != nullptr) {
			*bytes_written = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT) {
		uint32_t s = size;
		if (f.buf->size != 0) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		}
		memcpy(f.buf->ptr, data, s);
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
		memcpy(f.buf->ptr, data, size);
		f.buf->ptr += size;
		if (bytes_written != nullptr) {
			*bytes_written = size;
		}
	}
}

void SysFileWrite(uint32_t n, sys_file_t& f) {
	SysFileWrite(&n, 4, f);
}

sys_file_t* SysFileCreate(const std::filesystem::path& file_name) {
	auto* ret = new sys_file_t;

	auto real_name     = get_internal_name(file_name);
	auto real_name_str = real_name.string();

	ret->f = fopen(real_name_str.c_str(), "w+");

	if (ret->f == nullptr) {
		printf("can't create file: %s\n", real_name_str.c_str());
	}

	ret->type = SYS_FILE_FILE;

	return ret;
}

sys_file_t* SysFileOpenR(const std::filesystem::path& file_name,
                         sys_file_cache_type_t /*cache_type*/) {
	auto* ret = new sys_file_t;

	ret->type = SYS_FILE_FILE;

	auto internal_name     = get_internal_name(file_name);
	auto internal_name_str = internal_name.string();

	FILE* f = fopen(internal_name_str.c_str(), "r");

	if (f == nullptr) {
		ret->type = SYS_FILE_ERROR;
	}

	ret->f = f;

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

sys_file_t* SysFileOpenW(const std::filesystem::path& file_name,
                         sys_file_cache_type_t /*cache_type*/) {
	auto* ret = new sys_file_t;

	auto real_name     = get_internal_name(file_name);
	auto real_name_str = real_name.string();

	FILE* f = fopen(real_name_str.c_str(), "r+");

	if (f == nullptr) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	ret->f = f;

	return ret;
}

sys_file_t* SysFileOpenRw(const std::filesystem::path& file_name,
                          sys_file_cache_type_t /*cache_type*/) {
	auto* ret = new sys_file_t;

	auto real_name     = get_internal_name(file_name);
	auto real_name_str = real_name.string();

	FILE* f = fopen(real_name_str.c_str(), "r+");

	if (f == nullptr) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	ret->f = f;

	return ret;
}

void SysFileClose(sys_file_t* f) {
	[[maybe_unused]] int result = 0;

	if (f->type == SYS_FILE_FILE && f->f != nullptr) {
		result = fclose(f->f);
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
	[[maybe_unused]] int result = 0;

	if (f.type == SYS_FILE_FILE) {
		uint32_t pos  = ftell(f.f);
		result        = fseek(f.f, 0, SEEK_END);
		uint32_t size = ftell(f.f);
		result        = fseek(f.f, pos, SEEK_SET);
		return size;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		return f.buf->size;
	}

	return 0;
}

uint64_t SysFileSize(const std::filesystem::path& file_name) {
	sys_file_t* f    = SysFileOpenR(file_name);
	uint64_t    size = SysFileSize(*f);
	SysFileClose(f);
	return size;
}

bool SysFileTruncate(sys_file_t& /*f*/, uint64_t /*size*/) {
	return false;
}

bool SysFileUnlink(sys_file_t& /*f*/, const std::filesystem::path& name) {
	return SysFileDeleteFile(name);
}

bool SysFileSeek(sys_file_t& f, uint64_t offset) {
	bool ok = true;
	if (f.type == SYS_FILE_FILE) {
		ok = (fseek(f.f, static_cast<int64_t>(offset), SEEK_SET) == 0);
		//		LARGE_INTEGER s;
		//		s.QuadPart = offset;
		//		SetFilePointerEx(f.handle, s, 0, FILE_BEGIN);
		// printf("seek: %u\n", offset);
	} else if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		f.buf->ptr = f.buf->base + offset;
	}
	return ok;
}

uint64_t SysFileTell(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE) {
		return ftell(f.f);
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		return f.buf->ptr - f.buf->base;
	}

	return 0;
}

bool SysFileIsError(sys_file_t& f) {
	return f.type == SYS_FILE_ERROR || (f.type == SYS_FILE_FILE && f.f == nullptr);
}

bool SysFileIsDirectoryExisting(const std::filesystem::path& path) {
	auto real_name     = get_internal_name(path);
	auto real_name_str = real_name.string();

	struct stat s {};

	if (0 != stat(real_name_str.c_str(), &s)) {
		return false;
	}

	return S_ISDIR(s.st_mode); // NOLINT
}

bool SysFileIsFileExisting(const std::filesystem::path& name) {
	auto real_name     = get_internal_name(name);
	auto real_name_str = real_name.string();

	struct stat s {};

	if (0 != stat(real_name_str.c_str(), &s)) {
		return false;
	}

	return !S_ISDIR(s.st_mode); // NOLINT
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

bool SysFileDeleteDirectory(const std::filesystem::path& path) {
	auto real_name     = get_internal_name(path);
	auto real_name_str = real_name.string();

	return 0 == remove(real_name_str.c_str());
}

bool SysFileDeleteFile(const std::filesystem::path& name) {
	auto real_name     = get_internal_name(name);
	auto real_name_str = real_name.string();

	return 0 == unlink(real_name_str.c_str());
}

bool SysFileFlush(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE && f.f != nullptr) {
		return (fflush(f.f) == 0);
	}

	return false;
}

SysFileTimeStruct SysFileGetLastAccessTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};

	auto real_name     = get_internal_name(name);
	auto real_name_str = real_name.string();

	struct stat s {};

	if (0 != stat(real_name_str.c_str(), &s)) {
		r.is_invalid = true;
	} else {
		r.is_invalid = false;
		r.time       = s.st_atime;
	}

	return r;
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
		w.time       = s.st_mtime;
	}
}

void SysFileGetLastAccessAndWriteTimeUtc(sys_file_t& /*f*/, SysFileTimeStruct& /*a*/,
                                         SysFileTimeStruct& /*w*/) {
	EXIT("not implemented\n");
}

bool SysFileSetLastAccessTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& access) {
	if (access.is_invalid) {
		return false;
	}

	auto real_name     = get_internal_name(name);
	auto real_name_str = real_name.string();

	struct stat s {};

	if (0 != stat(real_name_str.c_str(), &s)) {
		return false;
	}

	utimbuf times {};
	times.actime  = access.time;
	times.modtime = s.st_mtime;

	return !(0 != utime(real_name_str.c_str(), &times));

	//	if (0 != stat(n.data(), &s))
	//	{
	//		return false;
	//	}
	//
	//	times.actime += times.actime - s.st_atime;
	//	times.modtime += times.modtime - s.st_mtime;
	//
	//	if (0 != utime(n.data(), &times))
	//	{
	//		return false;
	//	}
}

bool SysFileSetLastWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& write) {
	if (write.is_invalid) {
		return false;
	}

	auto real_name     = get_internal_name(name);
	auto real_name_str = real_name.string();

	struct stat s {};

	if (0 != stat(real_name_str.c_str(), &s)) {
		return false;
	}

	utimbuf times {};
	times.actime  = s.st_atime;
	times.modtime = write.time;

	return !(0 != utime(real_name_str.c_str(), &times));

	//	if (0 != stat(n.data(), &s))
	//	{
	//		return false;
	//	}
	//
	//	times.actime += times.actime - s.st_atime;
	//	times.modtime += times.modtime - s.st_mtime;
	//
	//	if (0 != utime(n.data(), &times))
	//	{
	//		return false;
	//	}
}

bool SysFileSetLastAccessAndWriteTimeUtc(const std::filesystem::path& name,
                                         SysFileTimeStruct& access, SysFileTimeStruct& write) {
	if (access.is_invalid || write.is_invalid) {
		return false;
	}

	auto real_name     = get_internal_name(name);
	auto real_name_str = real_name.string();

	struct stat s {};

	if (0 != stat(real_name_str.c_str(), &s)) {
		return false;
	}

	utimbuf times {};
	times.actime  = access.time;
	times.modtime = write.time;

	return !(0 != utime(real_name_str.c_str(), &times));

	//	if (0 != stat(n.data(), &s))
	//	{
	//		return false;
	//	}
	//
	//	times.actime += times.actime - s.st_atime;
	//	times.modtime += times.modtime - s.st_mtime;
	//
	//	if (0 != utime(n.data(), &times))
	//	{
	//		return false;
	//	}
}

void SysFileFindFiles(const std::filesystem::path& /*path*/,
                      std::vector<sys_file_find_t>& /*out*/) {
	EXIT("not implemented\n");
}

void SysFileGetDents(const std::filesystem::path& /*path*/, std::vector<sys_dir_entry_t>& /*out*/) {
	EXIT("not implemented\n");
}

bool SysFileCopyFile(const std::filesystem::path& /*src*/, const std::filesystem::path& /*dst*/) {
	EXIT("not implemented\n");
	return false;
}

bool SysFileMoveFile(const std::filesystem::path& /*src*/, const std::filesystem::path& /*dst*/) {
	EXIT("not implemented\n");
	return false;
}

void SysFileRemoveReadonly(const std::filesystem::path& /*name*/) {
	EXIT("not implemented\n");
}

#endif
