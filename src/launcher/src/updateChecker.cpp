#include "updateChecker.h"

#include "kytyGitVersion.h"

#include <QByteArray>
#include <QDate>
#include <QDebug>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <QWidget>

namespace {

constexpr char DEFAULT_FEED_URL[]  = "https://kytyps5.github.io/data/updates.json";
constexpr char FALLBACK_FEED_URL[] =
    "https://api.github.com/repos/KytyPS5/KytyPS5/releases/latest";

QDate ExtractTagDate(const QString& tag) {
	static const QRegularExpression date_re(QStringLiteral("(\\d{4}-\\d{2}-\\d{2})"));
	const auto match = date_re.match(tag);
	if (match.hasMatch()) {
		return QDate::fromString(match.captured(1), QStringLiteral("yyyy-MM-dd"));
	}
	return {};
}

} // namespace

UpdateChecker::UpdateChecker(QWidget* parent): QObject(parent), m_parent(parent), m_network(this) {}

bool UpdateChecker::IsSupported() {
#if defined(KYTY_OFFICIAL_BUILD) && defined(NDEBUG)
	return !QString::fromLatin1(KYTY_GIT_HASH).endsWith(QStringLiteral("-dirty"));
#else
	return false;
#endif
}

bool UpdateChecker::IsNewerRelease(const QString& latest_tag, const QString& current_tag) {
	if (latest_tag.isEmpty() || latest_tag == current_tag) {
		return false;
	}

	const QDate remote_date  = ExtractTagDate(latest_tag);
	const QDate current_date = ExtractTagDate(current_tag);
	if (remote_date.isValid() && current_date.isValid() && remote_date < current_date) {
		return false;
	}

	return true;
}

UpdateChecker::UpdateInfo UpdateChecker::ParseUpdateInfo(const QByteArray& data) {
	const auto document = QJsonDocument::fromJson(data);
	if (!document.isObject()) {
		return {{}, {}, tr("Invalid update feed")};
	}

	const auto root = document.object();
	UpdateInfo info {root.value(QStringLiteral("tag")).toString(),
	                 QUrl(root.value(QStringLiteral("html_url")).toString()), {}};
	if (info.tag.isEmpty()) {
		info.tag = root.value(QStringLiteral("tag_name")).toString();
	}
	if (info.tag.isEmpty() || !info.page_url.isValid() ||
	    info.page_url.scheme() != QStringLiteral("https")) {
		info.error = tr("Incomplete update feed");
	}
	return info;
}

void UpdateChecker::Check(bool manual) {
	if (!IsSupported() || m_checking_updates) {
		return;
	}
	m_checking_updates = true;
	m_cached_feed_info = {};
	emit CheckingChanged(true);
	FetchUpdateInfo(DEFAULT_FEED_URL, false, manual);
}

void UpdateChecker::FetchUpdateInfo(const char* url, bool fallback, bool manual) {
	QNetworkRequest request(QUrl(QString::fromLatin1(url)));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "Kyty-Launcher");
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setTransferTimeout(15000);

	auto* reply = m_network.get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, fallback, manual]() {
		UpdateInfo info;
		if (reply->error() == QNetworkReply::NoError) {
			info = ParseUpdateInfo(reply->readAll());
		} else {
			info.error = reply->errorString();
		}
		reply->deleteLater();

		if (!fallback) {
			if (!info.error.isEmpty()) {
				// Website feed failed, fall back to GitHub API
				FetchUpdateInfo(FALLBACK_FEED_URL, true, manual);
				return;
			}

			// If the website feed thinks the current version is not the latest,
			// verify with GitHub Releases directly to ensure the website feed
			// isn't lagging behind a freshly downloaded release.
			const QString current_tag = QString::fromLatin1(KYTY_RELEASE_TAG);
			if (IsNewerRelease(info.tag, current_tag)) {
				m_cached_feed_info = info;
				FetchUpdateInfo(FALLBACK_FEED_URL, true, manual);
				return;
			}
		}

		if (fallback && !info.error.isEmpty() && !m_cached_feed_info.tag.isEmpty()) {
			// GitHub API verification was unreachable or rate-limited; fall back to cached website feed
			info = m_cached_feed_info;
		}

		m_checking_updates = false;
		emit CheckingChanged(false);
		ShowUpdateResult(info, manual);
	});
}

void UpdateChecker::ShowUpdateResult(const UpdateInfo& info, bool manual) {
	if (!info.error.isEmpty()) {
		qWarning() << "Could not check for updates:" << info.error;
		if (manual) {
			QMessageBox::warning(m_parent, tr("Update Check"),
			                     tr("Could not check for updates:\n%1").arg(info.error));
		}
		return;
	}

	const QString current_tag = QString::fromLatin1(KYTY_RELEASE_TAG);
	if (!IsNewerRelease(info.tag, current_tag)) {
		if (manual) {
			QMessageBox::information(m_parent, tr("Update Check"),
			                         tr("You are using the latest version (%1).").arg(current_tag));
		}
		return;
	}

	const auto message =
	    tr("An update is available.\n\nCurrent: %1\nLatest: %2\n\n"
	       "Open the release page?")
	        .arg(current_tag, info.tag);
	if (QMessageBox::question(m_parent, tr("KytyPS5 Update"), message,
	                          QMessageBox::Open | QMessageBox::Cancel,
	                          QMessageBox::Open) == QMessageBox::Open) {
		QDesktopServices::openUrl(info.page_url);
	}
}
