#include "compatibilityDatabase.h"

#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtConcurrentRun>

#include <utility>

namespace {

constexpr char FILE_NAME[] = "compatibility_db.json";
constexpr char URL[] =
    "https://github.com/Nmzik/KytyPS5/releases/download/compat-db/compatibility_db.json";
constexpr int RETRY_COUNT    = 3;
constexpr int RETRY_DELAY_MS = 750;

struct LoadResult {
	QMap<QString, CompatibilityEntry> entries;
	QString                           error;
};

QString TitleKey(const QString& title_id) {
	return title_id.trimmed().toUpper();
}

Configuration::GameStatus StatusFromText(const QString& text) {
	const auto value = text.trimmed();
	if (value == QStringLiteral("InGame") || value == QStringLiteral("In game")) {
		return Configuration::GameStatus::InGame;
	}
	if (value == QStringLiteral("MainMenu") || value == QStringLiteral("Main menu")) {
		return Configuration::GameStatus::MainMenu;
	}
	if (value == QStringLiteral("Logo")) {
		return Configuration::GameStatus::Logo;
	}
	if (value == QStringLiteral("DoesntBoot") || value == QStringLiteral("Doesn't boot")) {
		return Configuration::GameStatus::DoesntBoot;
	}
	return Configuration::GameStatus::Unknown;
}

QString StatusToText(Configuration::GameStatus status) {
	switch (status) {
		case Configuration::GameStatus::InGame: return QStringLiteral("InGame");
		case Configuration::GameStatus::MainMenu: return QStringLiteral("MainMenu");
		case Configuration::GameStatus::Logo: return QStringLiteral("Logo");
		case Configuration::GameStatus::DoesntBoot: return QStringLiteral("DoesntBoot");
		case Configuration::GameStatus::Unknown: return QStringLiteral("Unknown");
	}
	return QStringLiteral("Unknown");
}

LoadResult Parse(const QByteArray& data) {
	LoadResult      ret;
	QJsonParseError parse_error;
	const auto      doc = QJsonDocument::fromJson(data, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		ret.error = QStringLiteral("Invalid compatibility JSON: %1").arg(parse_error.errorString());
		return ret;
	}

	const auto root = doc.object();
	for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
		const auto title_id = TitleKey(it.key());
		const auto object   = it.value().toObject();
		if (title_id.isEmpty() || object.isEmpty()) {
			continue;
		}
		ret.entries.insert(title_id,
		                   {StatusFromText(object.value(QStringLiteral("status")).toString()),
		                    object.value(QStringLiteral("comment")).toString()});
	}
	return ret;
}

LoadResult DownloadOnce() {
	QNetworkAccessManager manager;
	QNetworkRequest       request(QUrl(QString::fromLatin1(URL)));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "Kyty-Launcher");

	auto*      reply = manager.get(request);
	QEventLoop loop;
	QTimer     timeout;
	timeout.setSingleShot(true);
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
	timeout.start(15000);
	loop.exec();

	if (!timeout.isActive()) {
		return {{}, QStringLiteral("Compatibility download timed out")};
	}
	if (reply->error() != QNetworkReply::NoError) {
		return {{}, reply->errorString()};
	}
	return Parse(reply->readAll());
}

LoadResult Download() {
	auto result = DownloadOnce();
	for (int retry = 1; retry <= RETRY_COUNT && !result.error.isEmpty(); retry++) {
		QThread::msleep(static_cast<unsigned long>(RETRY_DELAY_MS * retry));
		result = DownloadOnce();
	}
	return result;
}

} // namespace

CompatibilityDatabase::CompatibilityDatabase(bool local, QObject* parent)
    : QObject(parent), m_local(local) {}

const CompatibilityEntry* CompatibilityDatabase::Find(const QString& title_id) const {
	const auto entry = m_entries.constFind(TitleKey(title_id));
	return entry != m_entries.constEnd() ? &entry.value() : nullptr;
}

void CompatibilityDatabase::Load() {
	if (m_local) {
		QFile file(QDir(".").absoluteFilePath(FILE_NAME));
		if (!file.exists()) {
			emit Updated();
			return;
		}
		if (!file.open(QIODevice::ReadOnly)) {
			qWarning() << "Could not open compatibility database:" << file.errorString();
			return;
		}
		auto result = Parse(file.readAll());
		if (!result.error.isEmpty()) {
			qWarning() << result.error;
			return;
		}
		m_entries = std::move(result.entries);
		emit Updated();
		return;
	}

	auto* watcher = new QFutureWatcher<LoadResult>(this);
	connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher]() {
		const auto result = watcher->result();
		watcher->deleteLater();
		if (!result.error.isEmpty()) {
			qWarning() << "Could not refresh compatibility database:" << result.error;
			return;
		}
		m_entries = result.entries;
		emit Updated();
	});
	watcher->setFuture(QtConcurrent::run(Download));
}

void CompatibilityDatabase::SetStatus(const QString& title_id, Configuration::GameStatus status) {
	const auto key = TitleKey(title_id);
	if (!m_local || key.isEmpty()) {
		return;
	}
	m_entries[key].status = status;
	Save();
}

void CompatibilityDatabase::SetComment(const QString& title_id, const QString& comment) {
	const auto key = TitleKey(title_id);
	if (!m_local || key.isEmpty()) {
		return;
	}
	m_entries[key].comment = comment;
	Save();
}

void CompatibilityDatabase::Save() const {
	QJsonObject root;
	for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
		root.insert(it.key(),
		            QJsonObject {{QStringLiteral("status"), StatusToText(it.value().status)},
		                         {QStringLiteral("comment"), it.value().comment}});
	}

	QSaveFile  file(QDir(".").absoluteFilePath(FILE_NAME));
	const auto data = QJsonDocument(root).toJson(QJsonDocument::Indented);
	if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
		qWarning() << "Could not save compatibility database:" << file.errorString();
	}
}
