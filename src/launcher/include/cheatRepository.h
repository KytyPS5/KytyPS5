#ifndef CHEAT_REPOSITORY_H
#define CHEAT_REPOSITORY_H

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QNetworkReply;

namespace Cheats {

struct RemoteFile {
	QString name;
	QString title;
	QString version;
	QString format;

	[[nodiscard]] bool IsSupported() const;
	[[nodiscard]] QUrl Url() const;
};

struct CatalogResult {
	QList<RemoteFile> files;
	QString           error;
};

[[nodiscard]] CatalogResult ParseIndex(const QByteArray& data, const QString& format,
                                       const QString& title_id);

class CheatRepository final: public QObject {
	Q_OBJECT

public:
	explicit CheatRepository(QObject* parent = nullptr);
	void Load(const QString& title_id);
	void Fetch(const RemoteFile& file);
	void CancelFetch();

signals:
	void CatalogLoaded(QList<Cheats::RemoteFile> files, QString error);
	void FileLoaded(QByteArray data, QString error);

private:
	QNetworkReply* Request(const QUrl& url, qint64 limit,
	                       std::function<void(QByteArray, QString)> complete);

	QNetworkAccessManager   m_network;
	QList<QNetworkReply*>   m_catalog_replies;
	QPointer<QNetworkReply> m_file_reply;
	QList<RemoteFile>       m_files;
	QStringList             m_errors;
	int                     m_catalog_generation = 0;
	int                     m_fetch_generation   = 0;
	int                     m_pending            = 0;
};

} // namespace Cheats

#endif // CHEAT_REPOSITORY_H
