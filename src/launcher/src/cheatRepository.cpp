#include "cheatRepository.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace Cheats {

namespace {

constexpr qint64 kIndexLimit = 2 * 1024 * 1024;
constexpr qint64 kFileLimit  = 8 * 1024 * 1024;

const QString kBaseUrl = QStringLiteral(
    "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/");

bool IsFormat(const QString& format) {
	return format == QStringLiteral("json") || format == QStringLiteral("mc4") ||
	       format == QStringLiteral("shn");
}

QRegularExpressionMatch MatchName(const QString& name) {
	static const QRegularExpression pattern(QStringLiteral(
	    "\\A([A-Z]{4}[0-9]{5})_([0-9]+(?:\\.[0-9]+)+)_[A-Za-z0-9_.-]+\\.(json|mc4|shn)\\z"));
	return pattern.match(name);
}

} // namespace

bool RemoteFile::IsSupported() const {
	return format == QStringLiteral("json") && !Url().isEmpty();
}

QUrl RemoteFile::Url() const {
	const auto match = MatchName(name);
	if (!IsFormat(format) || !match.hasMatch() || match.captured(3) != format) {
		return {};
	}
	return QUrl(kBaseUrl + format + QLatin1Char('/') + name);
}

CatalogResult ParseIndex(const QByteArray& data, const QString& format, const QString& title_id) {
	CatalogResult result;
	if (!IsFormat(format) || data.isEmpty() || data.size() > kIndexLimit) {
		result.error = CheatRepository::tr("Invalid cheat index.");
		return result;
	}

	const auto    id = title_id.trimmed().toUpper();
	QSet<QString> seen;
	int           invalid = 0;
	auto          text    = QString::fromUtf8(data);
	if (text.startsWith(QChar(0xfeff))) {
		text.remove(0, 1);
	}
	for (const auto& line: text.split(QLatin1Char('\n'))) {
		const auto trimmed = line.trimmed();
		if (trimmed.isEmpty()) {
			continue;
		}
		const auto separator = trimmed.indexOf(QLatin1Char('='));
		const auto name      = trimmed.left(separator);
		const auto match     = MatchName(name);
		if (separator < 0 || !match.hasMatch() || match.captured(3) != format) {
			invalid++;
			continue;
		}
		if (match.captured(1) != id || seen.contains(name)) {
			continue;
		}
		seen.insert(name);
		result.files.append(
		    {name, trimmed.mid(separator + 1).trimmed(), match.captured(2), format});
	}
	if (invalid != 0) {
		result.error = CheatRepository::tr("Skipped %1 invalid cheat index entries.").arg(invalid);
	}
	return result;
}

CheatRepository::CheatRepository(QObject* parent): QObject(parent), m_network(this) {}

QNetworkReply* CheatRepository::Request(const QUrl& url, qint64 limit,
                                        std::function<void(QByteArray, QString)> complete) {
	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::SameOriginRedirectPolicy);
	request.setRawHeader("User-Agent", "Kyty-Launcher");
	request.setTransferTimeout(15000);
	auto* reply = m_network.get(request);
	reply->setReadBufferSize(limit + 1);
	connect(reply, &QNetworkReply::readyRead, this, [reply, limit]() {
		if (reply->bytesAvailable() > limit) {
			reply->setProperty("too_large", true);
			reply->abort();
		}
	});
	QTimer::singleShot(30000, reply, [reply]() {
		if (!reply->isFinished()) {
			reply->setProperty("timed_out", true);
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this,
	        [this, reply, limit, complete = std::move(complete)]() {
		        QString error;
		        if (reply->property("too_large").toBool() || reply->bytesAvailable() > limit) {
			        error = tr("Cheat download exceeds the size limit.");
		        } else if (reply->property("timed_out").toBool()) {
			        error = tr("Cheat download timed out.");
		        } else if (reply->error() != QNetworkReply::NoError) {
			        error = reply->errorString();
		        } else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
		                   200) {
			        error = tr("Unexpected response from the cheat repository.");
		        }
		        const auto data = error.isEmpty() ? reply->readAll() : QByteArray();
		        m_catalog_replies.removeAll(reply);
		        if (m_file_reply == reply) {
			        m_file_reply.clear();
		        }
		        reply->deleteLater();
		        complete(data, error);
	        });
	return reply;
}

void CheatRepository::Load(const QString& title_id) {
	const auto generation = ++m_catalog_generation;
	for (auto* reply: std::exchange(m_catalog_replies, {})) {
		reply->abort();
	}
	CancelFetch();
	m_files.clear();
	m_errors.clear();
	m_pending = 3;
	for (const auto& format:
	     {QStringLiteral("json"), QStringLiteral("mc4"), QStringLiteral("shn")}) {
		auto* reply =
		    Request(QUrl(kBaseUrl + format + QStringLiteral(".txt")), kIndexLimit,
		            [this, generation, format, title_id](const QByteArray& data, QString error) {
			            if (generation != m_catalog_generation) {
				            return;
			            }
			            if (error.isEmpty()) {
				            auto result = ParseIndex(data, format, title_id);
				            m_files.append(result.files);
				            error = result.error;
			            }
			            if (!error.isEmpty()) {
				            m_errors.append(format.toUpper() + QStringLiteral(": ") + error);
			            }
			            if (--m_pending == 0) {
				            std::sort(m_files.begin(), m_files.end(),
				                      [](const RemoteFile& left, const RemoteFile& right) {
					                      if (left.version != right.version) {
						                      return left.version > right.version;
					                      }
					                      if (left.format != right.format) {
						                      return left.format < right.format;
					                      }
					                      return left.name < right.name;
				                      });
				            emit CatalogLoaded(m_files, m_errors.join(QLatin1Char('\n')));
			            }
		            });
		m_catalog_replies.append(reply);
	}
}

void CheatRepository::Fetch(const RemoteFile& file) {
	CancelFetch();
	if (!file.IsSupported()) {
		emit FileLoaded({}, tr("Only JSON cheat files are supported."));
		return;
	}
	const auto generation = m_fetch_generation;
	m_file_reply = Request(file.Url(), kFileLimit,
	                       [this, generation](const QByteArray& data, const QString& error) {
		                       if (generation == m_fetch_generation) {
			                       emit FileLoaded(data, error);
		                       }
	                       });
}

void CheatRepository::CancelFetch() {
	m_fetch_generation++;
	if (m_file_reply != nullptr) {
		auto* reply = m_file_reply.data();
		m_file_reply.clear();
		reply->abort();
	}
}

} // namespace Cheats
