#include "updateChecker.h"

#include "kytyGitVersion.h"

#include <QByteArray>
#include <QDebug>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>
#include <QWidget>

namespace {

constexpr char DEFAULT_FEED_URL[]  = "https://kytyps5.github.io/data/updates.json";
constexpr char FALLBACK_FEED_URL[] =
    "https://api.github.com/repos/KytyPS5/KytyPS5/releases/latest";

} // namespace

struct UpdateChecker::UpdateInfo {
	QString tag;
	QUrl    page_url;
	QString error;
};

UpdateChecker::UpdateChecker(QWidget* parent): QObject(parent), m_parent(parent), m_network(this) {}

bool UpdateChecker::IsSupported() {
#if defined(KYTY_OFFICIAL_BUILD) && defined(NDEBUG)
	return !QString::fromLatin1(KYTY_GIT_HASH).endsWith(QStringLiteral("-dirty"));
#else
	return false;
#endif
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

		if (!info.error.isEmpty() && !fallback) {
			FetchUpdateInfo(FALLBACK_FEED_URL, true, manual);
			return;
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

	const bool current = info.tag == QString::fromLatin1(KYTY_RELEASE_TAG);
	if (current) {
		if (manual) {
			QMessageBox::information(m_parent, tr("Update Check"),
			                         tr("You are using the latest version (%1).").arg(info.tag));
		}
		return;
	}
	const auto message =
	    tr("An update is available.\n\nCurrent: %1\nLatest: %2\n\n"
	       "Open the release page?")
	        .arg(QString::fromLatin1(KYTY_RELEASE_TAG), info.tag);
	if (QMessageBox::question(m_parent, tr("KytyPS5 Update"), message,
	                          QMessageBox::Open | QMessageBox::Cancel,
	                          QMessageBox::Open) == QMessageBox::Open) {
		QDesktopServices::openUrl(info.page_url);
	}
}
