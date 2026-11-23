#ifndef KYTY_COMMON_FILE_H_
#define KYTY_COMMON_FILE_H_

#include "common/common.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#endif

namespace Common {

// SUBSYSTEM_DEFINE(File);

class File {
public:
	enum class Mode { Read, Write, ReadWrite };

	struct DirEntry {
		std::string name;
		bool        is_file;
	};
	struct Info {
		bool     is_file;
		uint64_t size;
	};
	// Nanoseconds since the Unix epoch.
	struct Times {
		int64_t access_ns;
		int64_t write_ns;
	};

	File();
	explicit File(const std::filesystem::path& name);
	File(const std::filesystem::path& name, Mode mode);
	virtual ~File();

	bool Create(const std::filesystem::path& name);
	bool Open(const std::filesystem::path& name, Mode mode);

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

	[[nodiscard]] std::optional<Times> GetTimes() const;

	void Read(void* data, uint32_t size, uint32_t* bytes_read = nullptr);
	void Write(const void* data, uint32_t size, uint32_t* bytes_written = nullptr);

	void Printf(const char* format, ...) KYTY_FORMAT_PRINTF(2, 3);

	std::vector<std::byte> ReadWholeBuffer();

	static uint64_t Size(const std::filesystem::path& name);
	static std::optional<Info> GetInfo(const std::filesystem::path& name);
	// Archive members report the host archive's times.
	static std::optional<Times> GetTimes(const std::filesystem::path& name);

	static bool IsDirectoryExisting(const std::filesystem::path& path);
	static bool IsFileExisting(const std::filesystem::path& name);
	static bool CreateDirectory(const std::filesystem::path& path);
	static bool CreateDirectories(const std::filesystem::path& path);
	static bool DeleteFile(const std::filesystem::path& name);

	static std::vector<DirEntry> GetDirEntries(const std::filesystem::path& path);
	static bool RenameFile(const std::filesystem::path& src, const std::filesystem::path& dst);

	KYTY_CLASS_NO_COPY(File);

private:
	struct FilePrivate;

	std::filesystem::path        m_file_name;
	std::unique_ptr<FilePrivate> m_p;
};

} // namespace Common

#endif /* KYTY_COMMON_FILE_H_ */
