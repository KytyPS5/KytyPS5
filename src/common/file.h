#ifndef KYTY_COMMON_FILE_H_
#define KYTY_COMMON_FILE_H_

#include "common/byteBuffer.h"
#include "common/common.h"
#include "common/dateTime.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#ifdef CopyFile
#undef CopyFile
#endif
#ifdef MoveFile
#undef MoveFile
#endif
#endif

namespace Common {

// SUBSYSTEM_DEFINE(File);

class File {
public:
	enum class Mode { Read, Write, ReadWrite, WriteRead };

	struct FindInfo {
		std::filesystem::path path_with_name;
		std::filesystem::path rel_path_with_name;
		DateTime              last_access_time;
		DateTime              last_write_time;
		uint64_t              size;
	};

	struct DirEntry {
		std::string name;
		bool        is_file;
	};

	File();
	explicit File(const std::filesystem::path& name);
	File(const std::filesystem::path& name, Mode mode);
	virtual ~File();

	bool Create(const std::filesystem::path& name);
	bool Open(const std::filesystem::path& name, Mode mode);
	bool OpenInMem(void* buf, uint32_t buf_size);
	bool OpenInMem(ByteBuffer& buf); // NOLINT(google-runtime-references)
	bool CreateInMem();

	void Close();

	bool Flush();

	[[nodiscard]] uint64_t Size() const;
	[[nodiscard]] uint64_t Remaining() const;

	bool                   Seek(uint64_t offset);
	[[nodiscard]] uint64_t Tell() const;

	bool Truncate(uint64_t size);
	bool Unlink();

	[[nodiscard]] bool IsInvalid() const;

	[[nodiscard]] bool IsEOF() const { return Tell() >= Size(); }

	void GetLastAccessAndWriteTimeUTC(DateTime* access, DateTime* write);

	void       Read(void* data, uint32_t size, uint32_t* bytes_read = nullptr);
	ByteBuffer Read(uint32_t size);
	void       Write(const void* data, uint32_t size, uint32_t* bytes_written = nullptr);
	void       Write(const ByteBuffer& buf, uint32_t* bytes_written = nullptr);

	void Printf(const char* format, ...) KYTY_FORMAT_PRINTF(2, 3);

	ByteBuffer ReadWholeBuffer();

	static uint64_t Size(const std::filesystem::path& name);

	static bool IsDirectoryExisting(const std::filesystem::path& path);
	static bool IsFileExisting(const std::filesystem::path& name);
	static bool CreateDirectory(const std::filesystem::path& path);
	static bool CreateDirectories(const std::filesystem::path& path);
	static bool DeleteDirectory(const std::filesystem::path& path);
	static bool DeleteDirectories(const std::filesystem::path& path);
	static bool DeleteFile(const std::filesystem::path& name);

	static DateTime GetLastAccessTimeUTC(const std::filesystem::path& name);
	static DateTime GetLastWriteTimeUTC(const std::filesystem::path& name);
	static void GetLastAccessAndWriteTimeUTC(const std::filesystem::path& name, DateTime* access,
	                                         DateTime* write);
	static bool SetLastAccessTimeUTC(const std::filesystem::path& name, const DateTime& dt);
	static bool SetLastWriteTimeUTC(const std::filesystem::path& name, const DateTime& dt);
	static bool SetLastAccessAndWriteTimeUTC(const std::filesystem::path& name,
	                                         const DateTime& access, const DateTime& write);

	static std::vector<FindInfo> FindFiles(const std::filesystem::path& path);
	static std::vector<DirEntry> GetDirEntries(const std::filesystem::path& path);
	static bool CopyFile(const std::filesystem::path& src, const std::filesystem::path& dst);
	static bool MoveFile(const std::filesystem::path& src, const std::filesystem::path& dst);
	static void RemoveReadonly(const std::filesystem::path& name);

	KYTY_CLASS_NO_COPY(File);

private:
	struct FilePrivate;

	std::filesystem::path        m_file_name;
	std::unique_ptr<FilePrivate> m_p;
};

} // namespace Common

#endif /* KYTY_COMMON_FILE_H_ */
