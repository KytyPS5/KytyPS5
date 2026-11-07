#ifndef UPDATE_RELEASE_H
#define UPDATE_RELEASE_H

#include "cmake_config.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

namespace UpdateRelease {

// Releases come from the repository that built this binary
inline const QString LATEST_URL =
    QStringLiteral("https://api.github.com/repos/" KYTY_BUILD_REPOSITORY "/releases/latest");
inline const QString TAG_URL =
    QStringLiteral("https://api.github.com/repos/" KYTY_BUILD_REPOSITORY "/releases/tags/%1");
inline const QString PAGE_URL =
    QStringLiteral("https://github.com/" KYTY_BUILD_REPOSITORY "/releases/tag/%1");
inline const QString DOWNLOAD_URL =
    QStringLiteral("https://github.com/" KYTY_BUILD_REPOSITORY "/releases/download/%1/%2");

struct Info {
	QString    tag;
	QUrl       page_url;
	QUrl       download_url;
	QByteArray sha256;
	qint64     size = 0;
	QDateTime  published_at;
	QString    error;
};

enum class VersionRelation { Current, Newer, Older, Unknown };

[[nodiscard]] QString         PlatformSuffix();
[[nodiscard]] Info            Parse(const QByteArray& data, const QString& platform_suffix);
[[nodiscard]] VersionRelation Compare(const Info& latest, const Info& current);

} // namespace UpdateRelease

#endif // UPDATE_RELEASE_H
