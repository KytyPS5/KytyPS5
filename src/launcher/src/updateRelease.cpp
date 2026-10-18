#include "updateRelease.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRegularExpression>

namespace UpdateRelease {

QString PlatformSuffix() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	return QStringLiteral("-Windows-x64.zip");
#elif defined(__linux__) && defined(__x86_64__)
	return QStringLiteral("-Linux-x86_64.tar.gz");
#elif defined(__APPLE__) && defined(__x86_64__)
	return QStringLiteral("-macOS-x86_64.zip");
#else
	return {};
#endif
}

Info Parse(const QByteArray& data, const QString& platform_suffix) {
	Info       info;
	const auto document = QJsonDocument::fromJson(data);
	if (!document.isObject()) {
		info.error = QObject::tr("Invalid GitHub release response.");
		return info;
	}

	const auto root = document.object();
	info.tag        = root.value(QStringLiteral("tag_name")).toString();
	const auto published_at =
	    QDateTime::fromString(root.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
	if (info.tag.isEmpty() || !published_at.isValid() ||
	    root.value(QStringLiteral("draft")).toBool() ||
	    root.value(QStringLiteral("prerelease")).toBool()) {
		info.error = QObject::tr("The release is not a published stable release.");
		return info;
	}
	info.published_at      = published_at;
	const auto encoded_tag = QString::fromLatin1(QUrl::toPercentEncoding(info.tag));
	info.page_url          = QUrl(PAGE_URL.arg(encoded_tag));

	if (platform_suffix.isEmpty()) {
		info.error = QObject::tr("Built-in updates are unavailable for this platform.");
		return info;
	}

	const auto asset_name = info.tag + platform_suffix;
	for (const auto& value: root.value(QStringLiteral("assets")).toArray()) {
		const auto asset = value.toObject();
		if (asset.value(QStringLiteral("name")).toString() != asset_name) {
			continue;
		}
		const auto                      digest = asset.value(QStringLiteral("digest")).toString();
		const auto                      size   = asset.value(QStringLiteral("size")).toInteger();
		static const QRegularExpression digest_pattern(
		    QStringLiteral("\\Asha256:[A-Fa-f0-9]{64}\\z"));
		if (!digest_pattern.match(digest).hasMatch() || size <= 0) {
			info.error = QObject::tr("The update package size or SHA-256 checksum is invalid.");
			return info;
		}
		const auto encoded_asset = QString::fromLatin1(QUrl::toPercentEncoding(asset_name));
		info.download_url        = QUrl(DOWNLOAD_URL.arg(encoded_tag, encoded_asset));
		info.sha256              = QByteArray::fromHex(digest.mid(7).toLatin1());
		info.size                = size;
		return info;
	}
	info.error = QObject::tr("This release has no update package for your platform.");
	return info;
}

VersionRelation Compare(const Info& latest, const Info& current) {
	if (latest.tag.isEmpty() || current.tag.isEmpty()) {
		return VersionRelation::Unknown;
	}
	if (latest.tag == current.tag) {
		return VersionRelation::Current;
	}
	if (!latest.published_at.isValid() || !current.published_at.isValid() ||
	    latest.published_at == current.published_at) {
		return VersionRelation::Unknown;
	}
	return latest.published_at > current.published_at ? VersionRelation::Newer
	                                                  : VersionRelation::Older;
}

} // namespace UpdateRelease
