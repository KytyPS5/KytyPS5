#ifndef UPDATE_CHECKER_H
#define UPDATE_CHECKER_H

#include "updateRelease.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QProcess>

#include <functional>
#include <memory>

class QNetworkReply;
class QProgressDialog;
class QTemporaryDir;
class QWidget;

class UpdateChecker final: public QObject {
	Q_OBJECT

public:
	explicit UpdateChecker(QWidget* parent);
	~UpdateChecker() override;

	[[nodiscard]] static bool IsSupported();
	static void               ShowPreviousResult(QWidget* parent);
	void                      Check(bool manual);
	void                      SetGameRunning(bool running) { m_game_running = running; }
	[[nodiscard]] bool        IsInstalling() const { return m_installing; }

signals:
	void CheckingChanged(bool checking);
	void InstallingChanged(bool installing);

private:
	void FetchRelease(const QUrl& url, std::function<void(UpdateRelease::Info)> done);
	void CheckCurrent(const UpdateRelease::Info& current, bool manual);
	void OfferUpdate();
	void Download();
	void ReadDownload(QNetworkReply* reply);
	void PreparePackage();
	void RunPreparation(const QString& program, const QStringList& arguments, bool validate);
	void Restart();
	void Finish(QString error = {}, bool report = true);

	QWidget*                       m_parent;
	QNetworkAccessManager          m_network;
	QPointer<QNetworkReply>        m_reply;
	QPointer<QProgressDialog>      m_progress;
	QProcess                       m_prepare;
	std::unique_ptr<QTemporaryDir> m_workspace;
	QFile                          m_download;
	QCryptographicHash             m_hash {QCryptographicHash::Sha256};
	UpdateRelease::Info            m_release;
	QString                        m_download_error;
	bool                           m_busy         = false;
	bool                           m_installing   = false;
	bool                           m_game_running = false;
	bool                           m_canceled     = false;
};

#endif // UPDATE_CHECKER_H
