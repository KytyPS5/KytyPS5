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
	explicit UpdateChecker(QWidget* parent);

	[[nodiscard]] static bool IsSupported();
	void Check(bool manual);

signals:
	void CheckingChanged(bool checking);

private:
	struct UpdateInfo;

	static UpdateInfo ParseUpdateInfo(const QByteArray& data);
	void              FetchUpdateInfo(const char* url, bool fallback, bool manual);
	void              ShowUpdateResult(const UpdateInfo& info, bool manual);

	QWidget*              m_parent           = nullptr;
	QNetworkAccessManager m_network;
	bool                  m_checking_updates = false;

	// Answer from the primary update feed, kept while the GitHub fallback feed is
	// being consulted, so it can still be shown if the fallback request fails.
	QString m_primary_tag;
	QUrl    m_primary_page_url;
};

#endif // UPDATE_CHECKER_H
