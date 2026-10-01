#include "gameContent.h"

#include "common/archive.h"
#include "common/file.h"
#include "common/stringUtils.h"

#include <QFileInfo>

#include <limits>

namespace GameContent {

std::filesystem::path ToPath(const QString& path) {
#if defined(_WIN32)
	return std::filesystem::path(path.toStdWString());
#else
	return std::filesystem::path(path.toStdString());
#endif
}

QString FromPath(const std::filesystem::path& path) {
#if defined(_WIN32)
	return QString::fromStdWString(path.wstring());
#else
	return QString::fromStdString(Common::PathToString(path));
#endif
}

bool IsArchive(const QString& base) {
	const QFileInfo info(base);
	return info.isFile() && Common::IsSupportedArchive(ToPath(base));
}

std::filesystem::path Resolve(const QString& base, const QString& relative) {
	const auto root = ToPath(base);
	return IsArchive(base) ? Common::MakeArchivePath(root, ToPath(relative))
	                       : root / ToPath(relative);
}

bool FileExists(const QString& base, const QString& relative) {
	return Common::File::IsFileExisting(Resolve(base, relative));
}

QByteArray ReadFile(const QString& base, const QString& relative, uint64_t max_size) {
	return ReadPath(Resolve(base, relative), max_size);
}

QByteArray ReadPath(const std::filesystem::path& path, uint64_t max_size) {
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return {};
	}

	const auto size = file.Size();
	if (size > max_size || size > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
		return {};
	}

	QByteArray data(static_cast<int>(size), Qt::Uninitialized);
	uint32_t   bytes_read = 0;
	file.Read(data.data(), static_cast<uint32_t>(size), &bytes_read);
	return bytes_read == size ? data : QByteArray {};
}

QStringList ListFiles(const QString& base, const QString& relative) {
	const auto  directory = Resolve(base, relative);
	const bool  archive   = Common::IsArchivePath(directory);
	QStringList result;
	for (const auto& entry: Common::File::GetDirEntries(directory)) {
		if (entry.is_file) {
			const auto path = FromPath(directory / Common::PathFromUtf8(entry.name));
			if (archive || !QFileInfo(path).isSymLink()) {
				result.append(path);
			}
		}
	}
	return result;
}

} // namespace GameContent
