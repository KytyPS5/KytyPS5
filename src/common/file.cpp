#include "common/file.h"

#include "common/archive.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/platform/sysFileIO.h"
#include "common/platform/sysTimer.h"
#include "common/stringUtils.h"

#include <cstdarg>
#include <cstdio>
#include <utility>
#include <vector>

// IWYU pragma: no_include <fileapi.h>
// IWYU pragma: no_include <windows.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#endif

namespace Common {

struct File::FilePrivate {
	sys_file_t*                  f;
	std::unique_ptr<ArchiveFile> archive;
};

File::File(): m_p(std::make_unique<FilePrivate>()) {
	m_p->f = nullptr;
}

File::~File() {
	Close();
}

File::File(const std::filesystem::path& name): m_p(std::make_unique<FilePrivate>()) {
	m_p->f = nullptr;

	Create(name);
}

File::File(const std::filesystem::path& name, Mode mode): m_p(std::make_unique<FilePrivate>()) {
	m_p->f = nullptr;

	Open(name, mode);
}

bool File::IsInvalid() const {
	return m_p->f == nullptr && m_p->archive == nullptr;
}

bool File::Create(const std::filesystem::path& name) {
	EXIT_IF(!IsInvalid());

	if (IsArchivePath(name)) {
		return false;
	}

	m_file_name = name;

	m_p->f = SysFileCreate(name);
	return m_p->f != nullptr;
}

bool File::Open(const std::filesystem::path& name, Mode mode) {
	EXIT_IF(!IsInvalid());

	m_file_name = name;
	if (IsArchivePath(name)) {
		if (mode != Mode::Read) {
			return false;
		}
		m_p->archive = OpenArchiveFile(name);
		return m_p->archive != nullptr;
	}

	switch (mode) {
		case Mode::Read: m_p->f = SysFileOpenR(name); break;
		case Mode::Write: m_p->f = SysFileOpenW(name); break;
		case Mode::ReadWrite: m_p->f = SysFileOpenRw(name); break;
	}
	return m_p->f != nullptr;
}

void File::Close() {
	m_p->archive.reset();
	if (m_p->f != nullptr) {
		SysFileClose(m_p->f);
		m_p->f = nullptr;
	}
}

uint64_t File::Size() const {
	EXIT_IF(IsInvalid());

	if (m_p->archive != nullptr) {
		return m_p->archive->Size();
	}

	if (m_p->f == nullptr) {
		return 0;
	}

	return SysFileSize(*m_p->f);
}

uint64_t File::Remaining() const {
	EXIT_IF(IsInvalid());

	const auto size     = Size();
	const auto position = Tell();
	return position < size ? size - position : 0;
}

uint64_t File::Size(const std::filesystem::path& name) {
	const auto info = GetInfo(name);
	return info ? info->size : 0;
}

std::optional<File::Info> File::GetInfo(const std::filesystem::path& name) {
	if (IsArchivePath(name)) {
		return GetArchiveInfo(name);
	}
	Info info {};
	return SysFileGetInfo(name, &info.is_file, &info.size) ? std::optional(info) : std::nullopt;
}

static std::optional<File::Times> QueryTimes(auto& target) {
	SysFileTimeStruct access {};
	SysFileTimeStruct write {};
	SysFileGetLastAccessAndWriteTimeUtc(target, access, write);
	if (access.is_invalid || write.is_invalid) {
		return std::nullopt;
	}
	return File::Times {SysFileTimeToNs(access), SysFileTimeToNs(write)};
}

std::optional<File::Times> File::GetTimes(const std::filesystem::path& name) {
	if (IsArchivePath(name)) {
		return GetTimes(GetArchiveHostPath(name));
	}
	return QueryTimes(name);
}

std::optional<File::Times> File::GetTimes() const {
	EXIT_IF(IsInvalid());

	if (m_p->archive != nullptr) {
		return GetTimes(m_file_name);
	}
	return QueryTimes(*m_p->f);
}

bool File::Seek(uint64_t offset) {
	EXIT_IF(IsInvalid());

	if (m_p->archive != nullptr) {
		return m_p->archive->Seek(offset);
	}

	return SysFileSeek(*m_p->f, offset);
}

bool File::Truncate(uint64_t size) {
	EXIT_IF(IsInvalid());

	if (m_p->archive != nullptr) {
		return false;
	}

	return SysFileTruncate(*m_p->f, size);
}

bool File::Unlink() {
	EXIT_IF(IsInvalid());

	if (m_p->archive != nullptr) {
		return false;
	}

	return SysFileUnlink(*m_p->f, m_file_name);
}

uint64_t File::Tell() const {
	EXIT_IF(IsInvalid());

	if (m_p->archive != nullptr) {
		return m_p->archive->Tell();
	}

	return SysFileTell(*m_p->f);
}

void File::Read(void* data, uint32_t size, uint32_t* bytes_read) {
	EXIT_IF(IsInvalid());

	if (m_p->archive != nullptr) {
		m_p->archive->Read(data, size, bytes_read);
		return;
	}

	if (m_p->f != nullptr) {
		SysFileRead(data, size, *m_p->f, bytes_read);
	}
}

void File::Write(const void* data, uint32_t size, uint32_t* bytes_written) {
	EXIT_IF(IsInvalid());

	if (m_p->archive != nullptr) {
		if (bytes_written != nullptr) {
			*bytes_written = 0;
		}
		return;
	}

	SysFileWrite(data, size, *m_p->f, bytes_written);
}

static std::string FileVformat(const char* format, va_list args) {
	va_list copy {};
	va_copy(copy, args);
	const int size = std::vsnprintf(nullptr, 0, format, copy);
	va_end(copy);

	if (size <= 0) {
		return {};
	}

	std::vector<char> buffer(static_cast<size_t>(size) + 1u);
	va_list           copy2 {};
	va_copy(copy2, args);
	std::vsnprintf(buffer.data(), buffer.size(), format, copy2);
	va_end(copy2);

	return {buffer.data(), static_cast<size_t>(size)};
}

void File::Printf(const char* format, ...) {
	va_list args {};
	va_start(args, format);

	auto s = FileVformat(format, args);

	va_end(args);

	s = Common::ReplaceStr(s, "\n", "\r\n");
	Write(s.data(), static_cast<uint32_t>(s.size()));
}

static std::filesystem::path WithoutTrailingSeparator(
	const std::filesystem::path& path) {
		if (!path.empty() && !path.has_filename() &&
			path != path.root_path()) {
			return path.parent_path();
		}

		return path;
}


bool File::IsDirectoryExisting(const std::filesystem::path& path) {
	const auto info = GetInfo(path);
	return info && !info->is_file;
}

bool File::IsFileExisting(const std::filesystem::path& name) {
	const auto info = GetInfo(name);
	return info && info->is_file;
}

bool File::CreateDirectory(const std::filesystem::path& path) {
	if (IsArchivePath(path)) {
		return false;
	}
    return SysFileCreateDirectory(
        WithoutTrailingSeparator(path));
}

bool File::DeleteDirectory(const std::filesystem::path& path) {
	if (IsArchivePath(path)) {
		return false;
	}
    return SysFileDeleteDirectory(
        WithoutTrailingSeparator(path));
}

bool File::CreateDirectories(const std::filesystem::path& path) {
	if (IsArchivePath(path)) {
		return false;
	}
    const auto normalized_path =
        WithoutTrailingSeparator(path);

    auto current_path = normalized_path.root_path();

    for (const auto& component:
         normalized_path.relative_path()) {
        current_path /= component;

        if (IsDirectoryExisting(current_path)) {
            continue;
        }

        if (!CreateDirectory(current_path)) {
            return false;
        }
    }

    return true;
}

bool File::DeleteFile(
    const std::filesystem::path& name) // @suppress("Member declaration not found")
{
	if (IsArchivePath(name)) {
		return false;
	}
	return SysFileDeleteFile(name);
}

bool File::Flush() {
	EXIT_IF(IsInvalid());

	if (m_p->archive != nullptr) {
		return true;
	}

	return SysFileFlush(*m_p->f);
}

std::vector<std::byte> File::ReadWholeBuffer() {
	EXIT_IF(IsInvalid());
	EXIT_IF(Tell() != 0);

	uint64_t s = Size();

	EXIT_IF((s >> 32u) != 0);

	const auto            read_size = static_cast<uint32_t>(s);
	std::vector<std::byte> buf(read_size);

	Read(buf.data(), read_size);

	return buf;
}

std::vector<File::DirEntry> File::GetDirEntries(const std::filesystem::path& path) {
	if (IsArchivePath(path)) {
		return GetArchiveDirEntries(path);
	}
	std::vector<sys_dir_entry_t> files;

	SysFileGetDents(path, files);

	std::vector<File::DirEntry> ret;
	ret.reserve(files.size());

	for (auto& f: files) {
		ret.emplace_back(std::move(f.name), f.is_file);
	}

	return ret;
}

bool File::RenameFile(const std::filesystem::path& src,
                      const std::filesystem::path& dst) // @suppress("Member declaration not found")
{
	if (IsArchivePath(src) || IsArchivePath(dst)) {
		return false;
	}
	if (IsFileExisting(dst)) {
		DeleteFile(dst); // @suppress("Invalid arguments")
	}

	return SysFileRenameFile(src, dst);
}

} // namespace Common
