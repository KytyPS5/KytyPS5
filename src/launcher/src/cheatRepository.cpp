#include "cheatRepository.h"

#include "cheatFile.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
	const auto lower = format.trimmed().toLower();
	return lower == QStringLiteral("json") || lower == QStringLiteral("mc4") ||
	       lower == QStringLiteral("shn");
}

QRegularExpressionMatch MatchName(const QString& name) {
	static const QRegularExpression pattern(
	    QStringLiteral(
	        "\\A([A-Za-z]{4}[0-9]{5})_([0-9]+(?:[._][0-9]+)+)(?:_[^/\\\\:*?\"<>|#]+)?\\.(json|mc4|shn)\\z"),
	    QRegularExpression::CaseInsensitiveOption);
	return pattern.match(name);
}

QUrl MakeHenCheatUrl(const QString& format, const QString& filename) {
	return QUrl(kBaseUrl + format.toLower() + QLatin1Char('/') + filename);
}

int CompareVersions(const QString& v1, const QString& v2) {
	auto to_tokens = [](QString s) {
		s = NormalizeVersion(s);
		const auto parts = s.split(QLatin1Char('.'), Qt::SkipEmptyParts);
		QList<qint64> tokens;
		for (const auto& part: parts) {
			bool   ok  = false;
			qint64 val = part.toLongLong(&ok);
			tokens.append(ok ? val : 0);
		}
		while (tokens.size() > 1 && tokens.last() == 0) {
			tokens.removeLast();
		}
		return tokens;
	};
	const auto t1    = to_tokens(v1);
	const auto t2    = to_tokens(v2);
	const int  count = qMin(t1.size(), t2.size());
	for (int i = 0; i < count; ++i) {
		if (t1[i] != t2[i]) {
			return t1[i] < t2[i] ? -1 : 1;
		}
	}
	if (t1.size() != t2.size()) {
		return t1.size() < t2.size() ? -1 : 1;
	}
	return 0;
}

} // namespace

QString ModuleTypeToString(ModuleType type) {
	switch (type) {
		case ModuleType::Patch:
			return CheatRepository::tr("Patch");
		case ModuleType::Cheat:
			return CheatRepository::tr("Cheat");
		case ModuleType::Config:
			return CheatRepository::tr("Config");
	}
	return CheatRepository::tr("Cheat");
}

QUrl RemoteFile::Url() const {
	if (url.isValid() && !url.isEmpty()) {
		return url;
	}
	return MakeHenCheatUrl(format, name);
}

CatalogResult ParseIndex(const QByteArray& data, const QString& format, const QString& title_id) {
	CatalogResult result;
	if (title_id.trimmed().isEmpty()) {
		result.error = CheatRepository::tr("Title ID cannot be empty.");
		return result;
	}
	if (!IsFormat(format) || data.size() > kIndexLimit) {
		result.error = CheatRepository::tr("Invalid cheat index.");
		return result;
	}
	if (data.isEmpty()) {
		return result;
	}

	const auto    fmt = format.trimmed().toLower();
	const auto    id  = title_id.trimmed().toUpper();
	QSet<QString> seen;
	auto          text = QString::fromUtf8(data);
	if (text.startsWith(QChar(0xfeff))) {
		text.remove(0, 1);
	}
	for (const auto& line: text.split(QLatin1Char('\n'))) {
		const auto trimmed = line.trimmed();
		if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) ||
		    trimmed.startsWith(QStringLiteral("//"))) {
			continue;
		}
		const auto separator = trimmed.indexOf(QLatin1Char('='));
		if (separator < 0) {
			continue;
		}
		const auto name  = trimmed.left(separator).trimmed();
		const auto match = MatchName(name);
		if (!match.hasMatch() || match.captured(3).compare(fmt, Qt::CaseInsensitive) != 0) {
			continue;
		}
		if (match.captured(1).compare(id, Qt::CaseInsensitive) != 0 ||
		    seen.contains(name.toLower())) {
			continue;
		}
		seen.insert(name.toLower());
		RemoteFile file;
		file.type    = ModuleType::Cheat;
		file.name    = name;
		file.title   = trimmed.mid(separator + 1).trimmed();
		if (file.title.isEmpty()) {
			file.title = name;
		}
		file.version = match.captured(2);
		file.version.replace(QLatin1Char('_'), QLatin1Char('.'));
		file.format  = fmt;
		file.source  = QStringLiteral("HEN Cheats Collection");
		file.author  = QStringLiteral("HEN Cheats Contributors");
		file.url     = MakeHenCheatUrl(fmt, name);
		result.files.append(file);
	}
	return result;
}

CatalogResult ParseModulesCatalog(const QByteArray& data, const QString& title_id) {
	CatalogResult result;
	if (title_id.trimmed().isEmpty()) {
		result.error = CheatRepository::tr("Title ID cannot be empty.");
		return result;
	}
	if (data.size() > kIndexLimit) {
		result.error = CheatRepository::tr("Invalid KytyPS5-Modules catalog.");
		return result;
	}
	if (data.isEmpty()) {
		result.error = CheatRepository::tr("KytyPS5-Modules catalog response was empty.");
		return result;
	}
	const auto doc = QJsonDocument::fromJson(data);
	if (!doc.isObject()) {
		result.error = CheatRepository::tr("KytyPS5-Modules catalog is not a valid JSON object.");
		return result;
	}
	const auto root = doc.object();
	const auto id   = title_id.trimmed().toUpper();

	const struct CategoryDef {
		const char* name;
		ModuleType  type;
	} categories[] = {
	    {"patches", ModuleType::Patch},
	    {"cheats",  ModuleType::Cheat},
	    {"configs", ModuleType::Config},
	};

	static const QUrl base_url(QStringLiteral("https://kytyps5.github.io/KytyPS5-Modules/"));
	QSet<QString>     seen_paths;

	for (const auto& cat: categories) {
		const auto cat_obj = root.value(QLatin1String(cat.name)).toObject();
		QJsonArray entries;
		for (auto it = cat_obj.constBegin(); it != cat_obj.constEnd(); ++it) {
			if (it.key().trimmed().compare(id, Qt::CaseInsensitive) == 0 && it.value().isArray()) {
				for (const auto& entry: it.value().toArray()) {
					entries.append(entry);
				}
			}
		}
		for (const auto& val: entries) {
			if (!val.isObject()) {
				continue;
			}
			const auto item = val.toObject();
			auto       path = item.value(QStringLiteral("path")).toString().trimmed();
			path.replace(QLatin1Char('\\'), QLatin1Char('/'));

			// Prevent directory traversal, local file access, or scheme hijacking
			if (path.isEmpty() || !path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) ||
			    path.contains(QStringLiteral("://")) || path.contains(QStringLiteral("..")) ||
			    path.contains(QLatin1Char('#')) || path.contains(QLatin1Char('?')) ||
			    path.startsWith(QLatin1Char('/')) || seen_paths.contains(path.toLower())) {
				continue;
			}

			const auto resolved = base_url.resolved(QUrl(path));
			if (resolved.scheme() != QStringLiteral("https") ||
			    resolved.host().compare(base_url.host(), Qt::CaseInsensitive) != 0) {
				continue;
			}
			seen_paths.insert(path.toLower());

			RemoteFile file;
			file.type        = cat.type;
			file.name        = QFileInfo(path).fileName();
			file.title       = item.value(QStringLiteral("title")).toString().trimmed();
			if (file.title.isEmpty()) {
				file.title = item.value(QStringLiteral("name")).toString().trimmed();
			}
			if (file.title.isEmpty()) {
				file.title = item.value(QStringLiteral("gameName")).toString().trimmed();
			}
			if (file.title.isEmpty()) {
				file.title = file.name;
			}
			const auto ver_val = item.value(QStringLiteral("version"));
			file.version       = ver_val.isString() ? ver_val.toString().trimmed()
			                                        : ver_val.toVariant().toString().trimmed();
			file.version.replace(QLatin1Char('_'), QLatin1Char('.'));
			file.format      = QStringLiteral("json");
			file.source      = QStringLiteral("KytyPS5-Modules");
			file.author      = item.value(QStringLiteral("author")).toString().trimmed();
			if (file.author.isEmpty()) {
				file.author = QStringLiteral("Community");
			}
			file.description = item.value(QStringLiteral("description")).toString().trimmed();
			file.sha256      = item.value(QStringLiteral("sha256")).toString().trimmed();
			file.url         = resolved;
			result.files.append(file);
		}
	}
	return result;
}

CheatRepository::CheatRepository(QObject* parent): QObject(parent), m_network(this) {}

CheatRepository::~CheatRepository() {
	for (auto& reply: m_catalog_replies) {
		if (reply != nullptr) {
			reply->disconnect(this);
			reply->abort();
			reply->deleteLater();
		}
	}
	if (m_file_reply != nullptr) {
		m_file_reply->disconnect(this);
		m_file_reply->abort();
		m_file_reply->deleteLater();
	}
}

QNetworkReply* CheatRepository::Request(const QUrl& url, qint64 limit, bool is_catalog,
                                        std::function<void(QByteArray, QString)> complete) {
	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "Kyty-Launcher");
	request.setTransferTimeout(15000);
	auto* reply = m_network.get(request);

	if (is_catalog) {
		m_catalog_replies.append(reply);
	} else {
		m_file_reply = reply;
	}

	reply->setReadBufferSize(limit + 1);

	connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, limit]() {
		const auto length = reply->header(QNetworkRequest::ContentLengthHeader);
		if (length.isValid() && length.toLongLong() > limit) {
			reply->setProperty("too_large", true);
			reply->abort();
		}
	});

	connect(reply, &QNetworkReply::readyRead, reply, [reply, limit]() {
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
	        [this, reply, limit, is_catalog, complete = std::move(complete)]() {
		        QString error;
		        if (reply->property("too_large").toBool() || reply->bytesAvailable() > limit) {
			        error = tr("Download exceeds the size limit.");
		        } else if (reply->property("timed_out").toBool()) {
			        error = tr("Download timed out.");
		        } else if (reply->error() != QNetworkReply::NoError) {
			        error = reply->errorString();
		        } else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
		                   200) {
			        error = tr("Unexpected response from repository: HTTP %1")
			                    .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
		        }
		        const auto data = error.isEmpty() ? reply->readAll() : QByteArray();
		        if (is_catalog) {
			        m_catalog_replies.removeAll(reply);
		        }
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
	for (auto& reply: std::exchange(m_catalog_replies, {})) {
		if (reply != nullptr) {
			reply->abort();
		}
	}
	CancelFetch();
	m_files.clear();
	m_errors.clear();
	m_pending = 0;

	if (!IsSupportedTitleId(title_id)) {
		QTimer::singleShot(0, this, [this, generation]() {
			if (generation == m_catalog_generation) {
				emit CatalogLoaded({}, tr("Unsupported or empty title ID."));
			}
		});
		return;
	}

	m_pending = 4; // 1 for KytyPS5-Modules catalog + 3 for HEN Cheats (json, mc4, shn)

	// Fetch from KytyPS5-Modules API
	Request(
	    QUrl(QStringLiteral("https://kytyps5.github.io/KytyPS5-Modules/index.min.json")),
	    kIndexLimit, true,
	    [this, generation, title_id](const QByteArray& data, QString error) {
		    if (generation != m_catalog_generation) {
			    return;
		    }
		    if (error.isEmpty()) {
			    auto result = ParseModulesCatalog(data, title_id);
			    m_files.append(result.files);
			    error = result.error;
		    }
		    if (!error.isEmpty()) {
			    m_errors.append(QStringLiteral("KytyPS5-Modules: ") + error);
		    }
		    CheckCatalogComplete();
	    });

	// Fetch from HEN Cheats Collection
	for (const auto& format:
	     {QStringLiteral("json"), QStringLiteral("mc4"), QStringLiteral("shn")}) {
		Request(QUrl(kBaseUrl + format + QStringLiteral(".txt")), kIndexLimit, true,
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
			        CheckCatalogComplete();
		        });
	}
}

void CheatRepository::CheckCatalogComplete() {
	if (--m_pending == 0) {
		std::sort(m_files.begin(), m_files.end(),
		          [](const RemoteFile& left, const RemoteFile& right) {
			          if (left.type != right.type) {
				          return static_cast<int>(left.type) < static_cast<int>(right.type);
			          }
			          const int ver_cmp = CompareVersions(left.version, right.version);
			          if (ver_cmp != 0) {
				          return ver_cmp > 0; // newer versions first
			          }
			          if (left.source != right.source) {
				          return left.source < right.source;
			          }
			          return left.name < right.name;
		          });
		emit CatalogLoaded(m_files, m_errors.join(QLatin1Char('\n')));
	}
}

void CheatRepository::Fetch(const RemoteFile& file) {
	CancelFetch();
	const auto url = file.Url();
	const auto generation = m_fetch_generation;
	if (!url.isValid() || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
		QTimer::singleShot(0, this, [this, generation]() {
			if (generation == m_fetch_generation) {
				emit FileLoaded({}, tr("Invalid or insecure download URL."));
			}
		});
		return;
	}

	Request(url, kFileLimit, false,
	        [this, generation, file](QByteArray data, QString error) {
		        if (generation != m_fetch_generation) {
			        return;
		        }
		        if (error.isEmpty() && !file.sha256.isEmpty()) {
			        const auto actual =
			            QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
			        if (actual.compare(file.sha256.toLatin1(), Qt::CaseInsensitive) != 0) {
				        error = tr("Checksum verification failed: file is corrupted or modified.");
				        data.clear();
			        }
		        }
		        if (error.isEmpty() &&
		            file.format.compare(QStringLiteral("json"), Qt::CaseInsensitive) != 0) {
			        error = ConvertTrainer(
			            &data, file.format.compare(QStringLiteral("mc4"), Qt::CaseInsensitive) == 0);
			        if (!error.isEmpty()) {
				        data.clear();
			        }
		        }
		        emit FileLoaded(data, error);
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
