#include "kernel/fileSystem.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

namespace Libs::LibKernel::FileSystem {

LIB_NAME("libkernel", "libkernel");

constexpr int DESCRIPTOR_MIN = 3;

enum class SpecialFile {
	None,
	Random,
};

class MountPoints {
public:
	struct MountPair {
		std::filesystem::path dir;
		std::string           point;
	};

	MountPoints() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~MountPoints() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(MountPoints);

	void Mount(const std::filesystem::path& folder, const std::string& point);
	void Umount(const std::string& folder_or_point);

	[[nodiscard]] std::filesystem::path GetRealFilename(const std::string& mounted_file_name);
	[[nodiscard]] std::filesystem::path GetRealDirectory(const std::string& mounted_directory);

private:
	std::vector<MountPair> m_mount_pairs;
	Common::Mutex          m_mutex;
};

struct File {
	Common::File                        f;
	std::string                         name;
	std::filesystem::path               real_name;
	std::atomic_bool                    opened;
	std::atomic_bool                    directory;
	std::atomic_bool                    append;
	std::atomic_bool                    sync_writes;
	SpecialFile                         special;
	Common::Mutex                       mutex;
	std::vector<Common::File::DirEntry> dents;
	uint64_t                            dents_offset;
};

class FileDescriptors {
public:
	FileDescriptors() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~FileDescriptors() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(FileDescriptors);

	int   CreateDescriptor();
	void  DeleteDescriptor(int d);
	File* GetFile(int d);
	File* GetFile(const std::filesystem::path& real_name);
	void  CloseAll();

private:
	std::vector<File*> m_files;
	Common::Mutex      m_mutex;
};

static MountPoints*     g_mount_points = nullptr;
static FileDescriptors* g_files        = nullptr;

static bool IsRandomDevice(const std::string& path) {
	return path == "/dev/random" || path == "/dev/urandom";
}

static void FillRandomBuffer(void* buf, size_t nbytes) {
	auto*              out = reinterpret_cast<uint8_t*>(buf);
	std::random_device random;

	for (size_t i = 0; i < nbytes; i++) {
		out[i] = static_cast<uint8_t>(random());
	}
}

static void SecToTimespec(KernelTimespec* ts, double sec) {
	ts->tv_sec  = static_cast<int64_t>(sec);
	ts->tv_nsec = static_cast<int64_t>((sec - static_cast<double>(ts->tv_sec)) * 1000000000.0);
}

static uint64_t AlignDown(uint64_t value, uint64_t alignment) {
	return (alignment != 0 ? value & ~(alignment - 1) : value);
}

static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
	return (alignment != 0 ? (value + alignment - 1) & ~(alignment - 1) : value);
}

static int PosixToKernel(int posix_errno) {
	return KERNEL_ERROR_UNKNOWN + posix_errno;
}

int FileDescriptors::CreateDescriptor() {
	Common::LockGuard lock(m_mutex);

	auto* file        = new File {};
	file->opened      = false;
	file->directory   = false;
	file->append      = false;
	file->sync_writes = false;
	file->special     = SpecialFile::None;

	int files_num = static_cast<int>(m_files.size());
	for (int index = 0; index < files_num; index++) {
		if (m_files[index] == nullptr) {
			m_files[index] = file;
			return index + DESCRIPTOR_MIN;
		}
	}

	m_files.push_back(file);
	return static_cast<int>(m_files.size()) + DESCRIPTOR_MIN - 1;
}

void FileDescriptors::DeleteDescriptor(int d) {
	Common::LockGuard lock(m_mutex);

	auto index = static_cast<size_t>(d - DESCRIPTOR_MIN);

	EXIT_IF(index >= m_files.size());
	EXIT_IF(m_files[index] == nullptr);
	EXIT_IF(m_files[index]->opened);

	delete m_files[index];
	m_files[index] = nullptr;
}

File* FileDescriptors::GetFile(int d) {
	Common::LockGuard lock(m_mutex);

	auto index = static_cast<size_t>(d - DESCRIPTOR_MIN);

	if (index >= m_files.size()) {
		return nullptr;
	}

	return m_files[index];
}

File* FileDescriptors::GetFile(const std::filesystem::path& real_name) {
	Common::LockGuard lock(m_mutex);

	for (auto* f: m_files) {
		if (f != nullptr && f->real_name == real_name) {
			return f;
		}
	}

	return nullptr;
}

void FileDescriptors::CloseAll() {
	Common::LockGuard lock(m_mutex);

	for (auto& f: m_files) {
		if (f != nullptr && f->opened) {
			f->f.Close();
			delete f;
			f = nullptr;
		}
	}
}

void MountPoints::Mount(const std::filesystem::path& folder, const std::string& point) {
	Common::LockGuard lock(m_mutex);

	auto folder_str = Common::FixDirectorySlash(Common::PathToGenericString(folder));
	auto point_str  = Common::FixDirectorySlash(point);

	Umount(point_str);

	MountPair p;
	p.dir   = folder_str;
	p.point = point_str;

	m_mount_pairs.push_back(p);
}

void MountPoints::Umount(const std::string& folder_or_point) {
	Common::LockGuard lock(m_mutex);

	auto folder_or_point_str = Common::FixDirectorySlash(folder_or_point);

	const auto it = std::find_if(
	    m_mount_pairs.begin(), m_mount_pairs.end(), [&folder_or_point_str](const MountPair& p) {
		    return Common::PathToGenericString(p.dir) == folder_or_point_str ||
		           p.point == folder_or_point_str;
	    });
	if (it != m_mount_pairs.end()) {
		m_mount_pairs.erase(it);
	}
}

std::filesystem::path MountPoints::GetRealFilename(const std::string& mounted_file_name) {
	Common::LockGuard lock(m_mutex);

	auto mounted_path =
	    Common::DirectoryWithoutFilename(Common::FixFilenameSlash(mounted_file_name));

	const auto it = std::find_if(
	    m_mount_pairs.begin(), m_mount_pairs.end(),
	    [&mounted_path](const MountPair& p) { return Common::StartsWith(mounted_path, p.point); });
	if (it != m_mount_pairs.end()) {
		const auto& p = *it;
		auto        rel_path =
		    Common::RemoveFirst(Common::FixFilenameSlash(mounted_file_name), p.point.size());
		while (Common::StartsWith(rel_path, '/')) {
			rel_path = Common::RemoveFirst(rel_path, 1);
		}
		return p.dir / rel_path;
	}

	return mounted_file_name;
}

std::filesystem::path MountPoints::GetRealDirectory(const std::string& mounted_directory) {
	Common::LockGuard lock(m_mutex);

	auto mounted_path = Common::FixDirectorySlash(mounted_directory);

	const auto it = std::find_if(
	    m_mount_pairs.begin(), m_mount_pairs.end(),
	    [&mounted_path](const MountPair& p) { return Common::StartsWith(mounted_path, p.point); });
	if (it != m_mount_pairs.end()) {
		const auto& p = *it;
		auto        rel_path =
		    Common::RemoveFirst(Common::FixDirectorySlash(mounted_directory), p.point.size());
		while (Common::StartsWith(rel_path, '/')) {
			rel_path = Common::RemoveFirst(rel_path, 1);
		}
		return p.dir / rel_path;
	}

	return mounted_directory;
}

KYTY_SUBSYSTEM_INIT(FileSystem) {
	g_mount_points = new MountPoints;
	g_files        = new FileDescriptors;
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(FileSystem) {
	if (g_files != nullptr) {
		g_files->CloseAll();
	}
}

KYTY_SUBSYSTEM_DESTROY(FileSystem) {
	if (g_files != nullptr) {
		g_files->CloseAll();
	}
}

void Mount(const std::filesystem::path& folder, const std::string& point) {

	g_mount_points->Mount(folder, point);
}

void Umount(const std::string& folder_or_point) {

	g_mount_points->Umount(folder_or_point);
}

std::filesystem::path GetRealFilename(const std::string& mounted_file_name) {

	return g_mount_points->GetRealFilename(mounted_file_name);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int KYTY_SYSV_ABI KernelOpen(const char* path, int flags, uint16_t mode) {
	PRINT_NAME();

	if (path == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	auto flags_u = static_cast<uint32_t>(flags);

	LOGF("\t path = %s\n"
	     "\t flags = %08" PRIx32 "\n"
	     "\t mode = %04" PRIx16 "\n",
	     path, flags_u, mode);

	bool append    = (flags_u & 0x0008u) != 0;
	bool fsync     = (flags_u & 0x0080u) != 0;
	bool sync      = (flags_u & 0x0080u) != 0;
	bool creat     = (flags_u & 0x0200u) != 0;
	bool trunc     = (flags_u & 0x0400u) != 0;
	bool excl      = (flags_u & 0x0800u) != 0;
	bool dsync     = (flags_u & 0x1000u) != 0;
	bool direct    = (flags_u & 0x00010000u) != 0;
	bool directory = (flags_u & 0x00020000u) != 0;

	if (direct) {
		LOGF("\t O_DIRECT ignored on host file backend\n");
	}

	flags_u &= 0x3u;

	Common::File::Mode rw_mode = Common::File::Mode::Read;

	switch (flags_u) {
		case 0: rw_mode = Common::File::Mode::Read; break;
		case 1: rw_mode = Common::File::Mode::Write; break;
		case 2: rw_mode = Common::File::Mode::ReadWrite; break;
		default: EXIT("invalid flag_u: %u\n", flags_u);
	}

	EXIT_NOT_IMPLEMENTED(directory && rw_mode != Common::File::Mode::Read);
	EXIT_NOT_IMPLEMENTED(directory && (trunc || creat));

	int   descriptor = g_files->CreateDescriptor();
	auto* file       = g_files->GetFile(descriptor);

	EXIT_IF(file == nullptr || file->opened || file->directory);

	file->name        = path;
	file->append      = append;
	file->sync_writes = fsync || sync || dsync;

	if (!directory && IsRandomDevice(file->name)) {
		file->real_name = file->name;
		file->special   = SpecialFile::Random;
		file->opened    = true;

		LOGF_COLOR(Log::Color::Green, "\tOpen device: %s, [ok]\n",
		           Common::PathToString(file->real_name).c_str());

		return descriptor;
	}

	file->real_name = (directory ? g_mount_points->GetRealDirectory(file->name)
	                             : g_mount_points->GetRealFilename(file->name));

	if (trunc && rw_mode == Common::File::Mode::Read) {
		return KERNEL_ERROR_EACCES;
	}

	bool dir_exist  = Common::File::IsDirectoryExisting(file->real_name);
	bool file_exist = Common::File::IsFileExisting(file->real_name);

	if (directory || dir_exist) {
		if (!dir_exist) {
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_ENOTDIR;
		}

		EXIT_NOT_IMPLEMENTED(!directory && rw_mode != Common::File::Mode::Read);
		EXIT_NOT_IMPLEMENTED(!directory && (trunc || creat));

		file->dents        = Common::File::GetDirEntries(file->real_name);
		file->dents_offset = 0;
		file->directory    = true;

		LOGF_COLOR(Log::Color::Green, "\tOpen dir: %s, entries = %" PRIu32 ", [ok]\n",
		           Common::PathToString(file->real_name).c_str(),
		           static_cast<uint32_t>(file->dents.size()));

		for (const auto& f: file->dents) {
			LOGF("\t\t%s %s\n", f.is_file ? "[file]" : "[dir ]", f.name.c_str());
		}
	} else {
		bool result = false;

		if (excl && creat && file_exist) {
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_EEXIST;
		}

		if (creat && (!file_exist || trunc)) {
			Common::File::CreateDirectories(file->real_name.parent_path());
			result = file->f.Create(file->real_name);

			LOGF_COLOR(result ? Log::Color::Green : Log::Color::Red, "\tCreate: %s, %s\n",
			           Common::PathToString(file->real_name).c_str(), result ? "[ok]" : "[fail]");
		} else {
			result = file->f.Open(file->real_name, rw_mode);

			LOGF_COLOR(result ? Log::Color::Green : Log::Color::Red, "\tOpen: %s, %s\n",
			           Common::PathToString(file->real_name).c_str(), result ? "[ok]" : "[fail]");
		}

		if (result && trunc) {
			result = file->f.Truncate(0);
		}

		if (result && append) {
			file->f.Seek(file->f.Size());
		}

		if (!result || file->f.IsInvalid()) {
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_EACCES;
		}
	}

	file->opened = true;
	return descriptor;
}

int KYTY_SYSV_ABI KernelClose(int d) {
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EPERM;
	}

	if (::Libs::Network::Net::IsSocket(d)) {
		const auto result = ::Libs::Network::Net::SocketClose(d);
		return (result == OK ? OK : PosixToKernel(::Libs::Network::NetToPosix(result)));
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_IF(!file->opened);

	if (!file->directory && file->special == SpecialFile::None) {
		file->f.Close();
	}

	file->opened = false;

	LOGF("\tClose: %s\n", Common::PathToString(file->real_name).c_str());

	g_files->DeleteDescriptor(d);

	return OK;
}

int64_t KYTY_SYSV_ABI KernelRead(int d, void* buf, size_t nbytes) {
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (::Libs::Network::Net::IsSocket(d)) {
		const auto result = ::Libs::Network::Net::Recv(d, buf, nbytes, 0);
		return (result >= 0 ? result : PosixToKernel(*Posix::GetErrorAddr()));
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_NOT_IMPLEMENTED(file->directory);

	EXIT_IF(!file->opened);

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

	if (file->special == SpecialFile::Random) {
		FillRandomBuffer(buf, nbytes);

		LOGF("\tRead %" PRIu64 " random bytes from: %s\n", static_cast<uint64_t>(nbytes),
		     Common::PathToString(file->real_name).c_str());

		return static_cast<int64_t>(nbytes);
	}

	file->mutex.Lock();

	bool     is_invalid = file->f.IsInvalid();
	uint32_t bytes_read = 0;
	file->f.Read(buf, static_cast<uint32_t>(nbytes), &bytes_read);

	file->mutex.Unlock();

	if (is_invalid) {
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tRead %u bytes from: %s\n", bytes_read, Common::PathToString(file->real_name).c_str());

	return bytes_read;
}

int64_t KYTY_SYSV_ABI KernelWrite(int d, const void* buf, size_t nbytes) {
	PRINT_NAME();

	if (buf == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (d == 1 || d == 2) {
		auto*      out      = (d == 1 ? stdout : stderr);
		const auto written  = std::fwrite(buf, 1, nbytes, out);
		const auto flush_ok = std::fflush(out) == 0;

		if (written != nbytes || !flush_ok) {
			return KERNEL_ERROR_EIO;
		}

		return static_cast<int64_t>(written);
	}

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}

	if (::Libs::Network::Net::IsSocket(d)) {
		const auto result = ::Libs::Network::Net::Send(d, buf, nbytes, 0);
		return (result >= 0 ? result : PosixToKernel(*Posix::GetErrorAddr()));
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_NOT_IMPLEMENTED(file->directory);
	EXIT_NOT_IMPLEMENTED(file->special != SpecialFile::None);

	EXIT_IF(!file->opened);

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

	file->mutex.Lock();

	bool     is_invalid    = file->f.IsInvalid();
	uint32_t bytes_written = 0;
	if (file->append) {
		file->f.Seek(file->f.Size());
	}
	file->f.Write(buf, static_cast<uint32_t>(nbytes), &bytes_written);
	if (file->sync_writes) {
		file->f.Flush();
	}

	file->mutex.Unlock();

	if (is_invalid) {
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tWrite %u bytes to: %s\n", bytes_written, Common::PathToString(file->real_name).c_str());

	return bytes_written;
}

int64_t KYTY_SYSV_ABI KernelPread(int d, void* buf, size_t nbytes, int64_t offset) {
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (offset < 0) {
		return KERNEL_ERROR_EINVAL;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_NOT_IMPLEMENTED(file->directory);

	EXIT_IF(!file->opened);

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

	if (file->special == SpecialFile::Random) {
		FillRandomBuffer(buf, nbytes);

		LOGF("\tRead %" PRIu64 " random bytes (pos = %" PRId64 ") from: %s\n",
		     static_cast<uint64_t>(nbytes), offset, Common::PathToString(file->real_name).c_str());

		return static_cast<int64_t>(nbytes);
	}

	file->mutex.Lock();

	bool     is_invalid = file->f.IsInvalid();
	auto     pos        = file->f.Tell();
	uint32_t bytes_read = 0;
	file->f.Seek(offset);
	file->f.Read(buf, static_cast<uint32_t>(nbytes), &bytes_read);
	file->f.Seek(pos);

	file->mutex.Unlock();

	if (is_invalid) {
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tRead %u bytes (pos = %" PRId64 ") from: %s\n", bytes_read, offset,
	     Common::PathToString(file->real_name).c_str());

	return bytes_read;
}

int64_t KYTY_SYSV_ABI KernelPwrite(int d, const void* buf, size_t nbytes, int64_t offset) {
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (offset < 0) {
		return KERNEL_ERROR_EINVAL;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_NOT_IMPLEMENTED(file->directory);
	EXIT_NOT_IMPLEMENTED(file->special != SpecialFile::None);

	EXIT_IF(!file->opened);

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

	file->mutex.Lock();

	bool     is_invalid    = file->f.IsInvalid();
	auto     pos           = file->f.Tell();
	uint32_t bytes_written = 0;
	file->f.Seek(file->append ? file->f.Size() : static_cast<uint64_t>(offset));
	file->f.Write(buf, static_cast<uint32_t>(nbytes), &bytes_written);
	if (file->sync_writes) {
		file->f.Flush();
	}
	file->f.Seek(pos);

	file->mutex.Unlock();

	if (is_invalid) {
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tWrite %u bytes (pos = %" PRId64 ") to: %s\n", bytes_written, offset,
	     Common::PathToString(file->real_name).c_str());

	return bytes_written;
}

int64_t KYTY_SYSV_ABI KernelLseek(int d, int64_t offset, int whence) {
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EPERM;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_NOT_IMPLEMENTED(file->directory);

	EXIT_IF(!file->opened);

	if (file->special != SpecialFile::None) {
		return KERNEL_ERROR_ESPIPE;
	}

	file->mutex.Lock();

	bool is_invalid = file->f.IsInvalid();

	if (whence == 1) {
		offset = static_cast<int64_t>(file->f.Tell()) + offset;
		whence = 0;
	}

	if (whence == 2) {
		offset = static_cast<int64_t>(file->f.Size()) + offset;
		whence = 0;
	}

	EXIT_NOT_IMPLEMENTED(whence != 0);

	if (offset < 0) {
		return KERNEL_ERROR_EINVAL;
	}

	file->f.Seek(offset);
	auto pos = static_cast<int64_t>(file->f.Tell());

	EXIT_IF(pos != offset);

	file->mutex.Unlock();

	if (is_invalid) {
		LOGF("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tLseek (pos = %" PRId64 ") to: %s\n", offset,
	     Common::PathToString(file->real_name).c_str());

	return pos;
}

int KYTY_SYSV_ABI KernelStat(const char* path, FileStat* sb) {
	PRINT_NAME();

	if (path == nullptr || sb == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	LOGF("\t KernelStat: %s\n", path);

	std::string path_s         = std::string(path);
	auto        real_file_name = g_mount_points->GetRealFilename(path_s);
	auto        real_directory = g_mount_points->GetRealDirectory(path_s);

	bool is_dir  = Common::File::IsDirectoryExisting(real_file_name) ||
	               Common::File::IsDirectoryExisting(real_directory);
	bool is_file = Common::File::IsFileExisting(real_file_name);

	if (!is_dir && !is_file) {
		LOGF("\t file not found\n");
		return KERNEL_ERROR_ENOENT;
	}

	EXIT_NOT_IMPLEMENTED(is_dir && is_file);

	FileStat stat {};
	stat.st_mode = 0000777u | (is_dir ? 0040000u : 0100000u);

	auto at = Common::DateTime::FromSystemUTC();
	auto wt = at;

	if (is_dir) {
		stat.st_size    = 0;
		stat.st_blksize = 512;
		stat.st_blocks  = 0;
	} else {
		stat.st_size    = static_cast<int64_t>(Common::File::Size(real_file_name));
		stat.st_blksize = 512;
		stat.st_blocks  = (stat.st_size + 511) / 512;

		Common::File::GetLastAccessAndWriteTimeUTC(real_file_name, &at, &wt);
	}

	SecToTimespec(&stat.st_atim, at.ToUnix());
	SecToTimespec(&stat.st_mtim, wt.ToUnix());
	stat.st_ctim     = stat.st_atim;
	stat.st_birthtim = stat.st_mtim;
	*sb              = stat;

	return OK;
}

int KYTY_SYSV_ABI KernelFstat(int d, FileStat* sb) {
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EPERM;
	}

	if (sb == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	EXIT_IF(!file->opened);

	LOGF("\tKernelFstat: %s\n", Common::PathToString(file->real_name).c_str());

	FileStat stat {};
	stat.st_mode = 0000777u | (file->directory ? 0040000u : 0100000u);

	auto at = Common::DateTime::FromSystemUTC();
	auto wt = at;

	if (file->special != SpecialFile::None) {
		stat.st_size    = 0;
		stat.st_blksize = 512;
		stat.st_blocks  = 0;
		SecToTimespec(&stat.st_atim, at.ToUnix());
		SecToTimespec(&stat.st_mtim, wt.ToUnix());
		stat.st_ctim     = stat.st_atim;
		stat.st_birthtim = stat.st_mtim;
		*sb              = stat;

		return OK;
	}

	if (!file->directory) {
		file->mutex.Lock();

		bool is_invalid = file->f.IsInvalid();
		auto size       = file->f.Size();
		file->f.GetLastAccessAndWriteTimeUTC(&at, &wt);

		file->mutex.Unlock();

		if (is_invalid) {
			LOGF("\tfile is invalid\n");
			return KERNEL_ERROR_EIO;
		}

		stat.st_size    = static_cast<int64_t>(size);
		stat.st_blksize = 512;
		stat.st_blocks  = (stat.st_size + 511) / 512;
	} else {
		stat.st_size    = 0;
		stat.st_blksize = 512;
		stat.st_blocks  = 0;
	}

	SecToTimespec(&stat.st_atim, at.ToUnix());
	SecToTimespec(&stat.st_mtim, wt.ToUnix());
	stat.st_ctim     = stat.st_atim;
	stat.st_birthtim = stat.st_mtim;
	*sb              = stat;

	return OK;
}

int KYTY_SYSV_ABI KernelUnlink(const char* path) {
	PRINT_NAME();

	if (path == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	auto path_s         = std::string(path);
	auto real_file_name = g_mount_points->GetRealFilename(path_s);
	auto real_directory = g_mount_points->GetRealDirectory(path_s);

	bool is_dir  = Common::File::IsDirectoryExisting(real_file_name) ||
	               Common::File::IsDirectoryExisting(real_directory);
	bool is_file = Common::File::IsFileExisting(real_file_name);

	if (is_dir) {
		return KERNEL_ERROR_EPERM;
	}

	if (!is_file) {
		return KERNEL_ERROR_ENOENT;
	}

	auto* open_file = g_files->GetFile(real_file_name);
	bool  ok        = (open_file != nullptr && open_file->opened && !open_file->directory
	                       ? open_file->f.Unlink()
	                       : Common::File::DeleteFile(real_file_name));

	if (!ok) {
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tKernelUnlink: %s\n", path);

	return OK;
}

int KYTY_SYSV_ABI KernelRename(const char* from, const char* to) {
	PRINT_NAME();

	if (from == nullptr || to == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	auto from_path = std::string(from);
	auto to_path   = std::string(to);
	auto real_from = g_mount_points->GetRealFilename(from_path);
	auto real_to   = g_mount_points->GetRealFilename(to_path);

	EXIT_NOT_IMPLEMENTED(g_files->GetFile(real_from) != nullptr);
	EXIT_NOT_IMPLEMENTED(g_files->GetFile(real_to) != nullptr);

	if (!Common::File::IsFileExisting(real_from)) {
		return KERNEL_ERROR_ENOENT;
	}

	Common::File::CreateDirectories(real_to.parent_path());

	if (Common::File::IsFileExisting(real_to)) {
		Common::File::DeleteFile(real_to);
	}

	if (!Common::File::MoveFile(real_from, real_to)) {
		return KERNEL_ERROR_EIO;
	}

	LOGF("\tKernelRename: %s -> %s\n", from, to);

	return OK;
}

int KYTY_SYSV_ABI KernelGetdirentries(int fd, char* buf, int nbytes, int64_t* basep) {
	PRINT_NAME();

	if (fd < DESCRIPTOR_MIN) {
		return KERNEL_ERROR_EBADF;
	}

	if (buf == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(fd);

	if (file == nullptr) {
		return KERNEL_ERROR_EBADF;
	}

	constexpr uint64_t DIR_BLOCK_SIZE   = 512;
	constexpr uint64_t DIRENT_META_SIZE = 8;

	if (!file->directory || nbytes < static_cast<int>(DIR_BLOCK_SIZE)) {
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_IF(!file->opened);

	LOGF("\t dir    = %s\n"
	     "\t nbytes = %d\n"
	     "\t offset = %" PRIu64 "\n",
	     Common::PathToString(file->real_name).c_str(), nbytes, file->dents_offset);

	if (basep != nullptr) {
		*basep = static_cast<int64_t>(file->dents_offset);
	}

	std::vector<uint8_t> dirents;
	dirents.reserve(
	    std::max<size_t>(static_cast<size_t>(DIR_BLOCK_SIZE), file->dents.size() * 64u));

	uint64_t next_ceiling       = 0;
	uint64_t dirent_offset      = 0;
	int64_t  last_reclen_offset = -1;

	for (const auto& entry: file->dents) {
		const auto& str      = entry.name;
		auto        str_size = str.size();
		EXIT_NOT_IMPLEMENTED(str_size > 255);

		uint64_t reclen = AlignUp(DIRENT_META_SIZE + str_size + 1, 4);
		if (dirent_offset + reclen > next_ceiling) {
			if (last_reclen_offset >= 0) {
				auto* last_reclen =
				    reinterpret_cast<uint16_t*>(dirents.data() + last_reclen_offset);
				*last_reclen = static_cast<uint16_t>(*last_reclen + (next_ceiling - dirent_offset));
			}

			dirent_offset = next_ceiling;
			next_ceiling += DIR_BLOCK_SIZE;
			dirents.resize(next_ceiling);
			std::fill(dirents.begin() + static_cast<std::ptrdiff_t>(dirent_offset), dirents.end(),
			          0);
		}

		*reinterpret_cast<uint32_t*>(dirents.data() + dirent_offset + 0) =
		    Common::hash(str.data(), static_cast<uint32_t>(str.size()));
		*reinterpret_cast<uint16_t*>(dirents.data() + dirent_offset + 4) =
		    static_cast<uint16_t>(reclen);
		*reinterpret_cast<uint8_t*>(dirents.data() + dirent_offset + 6) = (entry.is_file ? 8 : 4);
		*reinterpret_cast<uint8_t*>(dirents.data() + dirent_offset + 7) =
		    static_cast<uint8_t>(str_size);
		memcpy(dirents.data() + dirent_offset + 8, str.data(), str_size);
		dirents[dirent_offset + 8 + str_size] = '\0';

		last_reclen_offset = static_cast<int64_t>(dirent_offset + 4);
		dirent_offset += reclen;

		LOGF("\t name  = %s\n", str.data());
	}

	if (last_reclen_offset >= 0) {
		auto* last_reclen = reinterpret_cast<uint16_t*>(dirents.data() + last_reclen_offset);
		*last_reclen      = static_cast<uint16_t>(*last_reclen + (next_ceiling - dirent_offset));
	}

	const uint64_t directory_size = next_ceiling;
	if (file->dents_offset >= directory_size) {
		return 0;
	}

	uint64_t bytes_written        = 0;
	uint64_t working_offset       = file->dents_offset;
	uint64_t dirent_buffer_offset = 0;
	uint64_t aligned_count        = AlignDown(static_cast<uint64_t>(nbytes), DIR_BLOCK_SIZE);

	while (dirent_buffer_offset < dirents.size()) {
		const auto* normal_dirent = dirents.data() + dirent_buffer_offset;
		uint16_t    reclen        = *reinterpret_cast<const uint16_t*>(normal_dirent + 4);
		uint8_t     namlen        = *(normal_dirent + 7);

		if (namlen == 0 || reclen == 0) {
			break;
		}

		if (working_offset >= reclen) {
			dirent_buffer_offset += reclen;
			working_offset -= reclen;
			continue;
		}

		if (bytes_written + reclen > aligned_count) {
			break;
		}

		const auto bytes_to_copy = reclen - working_offset;
		memcpy(buf + bytes_written, normal_dirent + working_offset, bytes_to_copy);
		bytes_written += bytes_to_copy;
		dirent_buffer_offset += reclen;
		working_offset = 0;
	}

	file->dents_offset += bytes_written;
	return static_cast<int64_t>(bytes_written);
}

int KYTY_SYSV_ABI KernelGetdents(int fd, char* buf, int nbytes) {
	PRINT_NAME();

	return KernelGetdirentries(fd, buf, nbytes, nullptr);
}

int KYTY_SYSV_ABI KernelMkdir(const char* path, uint16_t mode) {
	PRINT_NAME();

	if (path == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	if (std::strlen(path) > 255) {
		return KERNEL_ERROR_ENAMETOOLONG;
	}

	LOGF("\t path = %s\n"
	     "\t mode = %04" PRIx16 "\n",
	     path, mode);

	auto real_name = g_mount_points->GetRealDirectory(std::string(path));

	if (Common::File::IsDirectoryExisting(real_name)) {
		return KERNEL_ERROR_EEXIST;
	}

	if (!Common::File::CreateDirectory(real_name)) {
		return KERNEL_ERROR_EIO;
	}

	if (!Common::File::IsDirectoryExisting(real_name)) {
		return KERNEL_ERROR_ENOENT;
	}

	return OK;
}

int KYTY_SYSV_ABI KernelRmdir(const char* path) {
	PRINT_NAME();

	if (path == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	LOGF("\t path = %s\n", path);

	auto real_name = g_mount_points->GetRealDirectory(std::string(path));

	if (!Common::File::IsDirectoryExisting(real_name)) {
		return KERNEL_ERROR_ENOENT;
	}

	if (!Common::File::DeleteDirectory(real_name)) {
		return KERNEL_ERROR_EIO;
	}

	return OK;
}

int KYTY_SYSV_ABI KernelCheckReachability(const char* path) {
	PRINT_NAME();

	if (path == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	if (std::strlen(path) > 255) {
		return KERNEL_ERROR_ENAMETOOLONG;
	}

	LOGF("\t KernelCheckReachability: %s\n", path);

	std::string mounted_path = std::string(path);

	if (IsRandomDevice(mounted_path)) {
		return OK;
	}

	auto real_name = g_mount_points->GetRealFilename(mounted_path);

	if (Common::File::IsFileExisting(real_name) || Common::File::IsDirectoryExisting(real_name)) {
		return OK;
	}

	return KERNEL_ERROR_ENOENT;
}

} // namespace Libs::LibKernel::FileSystem
