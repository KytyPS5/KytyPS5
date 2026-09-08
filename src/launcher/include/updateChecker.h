#ifndef UPDATE_CHECKER_H
#define UPDATE_CHECKER_H

#include <QNetworkAccessManager>
#include <QObject>

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
};

#endif // UPDATE_CHECKER_H
