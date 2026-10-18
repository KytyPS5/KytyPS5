#ifndef UPDATE_RELEASE_H
#define UPDATE_RELEASE_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

namespace UpdateRelease {

inline const QString LATEST_URL =
    QStringLiteral("https://api.github.com/repos/KytyPS5/KytyPS5/releases/latest");
inline const QString TAG_URL =
    QStringLiteral("https://api.github.com/repos/KytyPS5/KytyPS5/releases/tags/%1");
inline const QString PAGE_URL =
    QStringLiteral("https://github.com/KytyPS5/KytyPS5/releases/tag/%1");
inline const QString DOWNLOAD_URL =
    QStringLiteral("https://github.com/KytyPS5/KytyPS5/releases/download/%1/%2");

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
