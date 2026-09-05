#ifndef UPDATE_CHECKER_H
#define UPDATE_CHECKER_H

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

class QByteArray;
class QWidget;

class UpdateChecker final: public QObject {
	Q_OBJECT

public:
	struct UpdateInfo {
		QString tag;
		QUrl    page_url;
		QString error;
	};

	explicit UpdateChecker(QWidget* parent);

	[[nodiscard]] static bool       IsSupported();
	[[nodiscard]] static bool       IsNewerRelease(const QString& latest_tag, const QString& current_tag);
	[[nodiscard]] static UpdateInfo ParseUpdateInfo(const QByteArray& data);
	void                            Check(bool manual);

signals:
	void CheckingChanged(bool checking);

private:
	void FetchUpdateInfo(const char* url, bool fallback, bool manual);
	void ShowUpdateResult(const UpdateInfo& info, bool manual);

	QWidget*              m_parent           = nullptr;
	QNetworkAccessManager m_network;
	bool                  m_checking_updates = false;
	UpdateInfo            m_cached_feed_info;
};

#endif // UPDATE_CHECKER_H
