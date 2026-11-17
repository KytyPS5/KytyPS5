#ifndef CHEAT_REPOSITORY_H
#define CHEAT_REPOSITORY_H

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QNetworkReply;

namespace Cheats {

enum class ModuleType {
	Patch,
	Cheat,
	Config,
};

[[nodiscard]] QString ModuleTypeToString(ModuleType type);

struct RemoteFile {
	ModuleType type = ModuleType::Cheat;
	QString    name;
	QString    title;
	QString    version;
	QString    format;
	QString    source;
	QString    author;
	QString    description;
	QString    sha256;
	QUrl       url;

	[[nodiscard]] QUrl Url() const;
};

struct CatalogResult {
	QList<RemoteFile> files;
	QString           error;
};

[[nodiscard]] CatalogResult ParseIndex(const QByteArray& data, const QString& format,
                                       const QString& title_id);
[[nodiscard]] CatalogResult ParseModulesCatalog(const QByteArray& data,
                                                const QString& title_id);

class CheatRepository final: public QObject {
	Q_OBJECT
	Q_DISABLE_COPY_MOVE(CheatRepository)

public:
	explicit CheatRepository(QObject* parent = nullptr);
	~CheatRepository() override;

	void Load(const QString& title_id);
	void Fetch(const RemoteFile& file);
	void CancelFetch();

signals:
	void CatalogLoaded(QList<Cheats::RemoteFile> files, QString error);
	void FileLoaded(QByteArray data, QString error);

private:
	QNetworkReply* Request(const QUrl& url, qint64 limit, bool is_catalog,
	                       std::function<void(QByteArray, QString)> complete);
	void           CheckCatalogComplete();

	QNetworkAccessManager          m_network;
	QList<QPointer<QNetworkReply>> m_catalog_replies;
	QPointer<QNetworkReply>        m_file_reply;
	QList<RemoteFile>              m_files;
	QStringList                    m_errors;
	int                            m_catalog_generation = 0;
	int                            m_fetch_generation   = 0;
	int                            m_pending            = 0;
};

} // namespace Cheats

Q_DECLARE_METATYPE(Cheats::RemoteFile)
Q_DECLARE_METATYPE(QList<Cheats::RemoteFile>)

#endif // CHEAT_REPOSITORY_H
