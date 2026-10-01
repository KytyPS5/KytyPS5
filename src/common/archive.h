#ifndef KYTY_COMMON_ARCHIVE_H_
#define KYTY_COMMON_ARCHIVE_H_

#include "common/file.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Common {

class ArchiveReader;

class ArchiveFile {
public:
	~ArchiveFile();

	[[nodiscard]] uint64_t Size() const;
	[[nodiscard]] uint64_t Tell() const;
	bool                   Seek(uint64_t offset);
	void                   Read(void* data, uint32_t size, uint32_t* bytes_read);

	KYTY_CLASS_NO_COPY(ArchiveFile);

private:
	struct Private;
	explicit ArchiveFile(std::unique_ptr<Private> p);
	std::unique_ptr<Private> m_p;

	friend std::unique_ptr<ArchiveFile> OpenArchiveFile(const std::filesystem::path& path);
};

// Virtual paths use "archive.ext!/member" and retain native host-path encoding.
[[nodiscard]] std::filesystem::path       MakeArchivePath(const std::filesystem::path& archive,
                                                          const std::filesystem::path& member = {});
[[nodiscard]] bool                        IsSupportedArchive(const std::filesystem::path& path);
[[nodiscard]] bool                        IsArchivePath(const std::filesystem::path& path);
[[nodiscard]] std::optional<File::Info>   GetArchiveInfo(const std::filesystem::path& path);
[[nodiscard]] std::vector<File::DirEntry> GetArchiveDirEntries(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path       GetArchiveHostPath(const std::filesystem::path& path);
[[nodiscard]] std::unique_ptr<ArchiveFile> OpenArchiveFile(const std::filesystem::path& path);

// Retain this handle while a mount or metadata scan is active to reuse the index and block cache.
// Accepts either a host archive filename or a virtual path inside the archive.
[[nodiscard]] std::shared_ptr<ArchiveReader> OpenArchive(const std::filesystem::path& path);

} // namespace Common

#endif /* KYTY_COMMON_ARCHIVE_H_ */
