#ifndef TROPHY_VIEWER_DIALOG_H
#define TROPHY_VIEWER_DIALOG_H

#include "common.h"

#include <QDialog>

class QTabWidget;
class QWidget;

class Configuration;
class TrophyViewerDialog: public QDialog {
	KYTY_QT_CLASS_NO_COPY(TrophyViewerDialog);

public:
	explicit TrophyViewerDialog(QWidget* parent = nullptr);

	static bool HasTrophyData(const Configuration* info);
	static void ShowForGame(const Configuration* info, QWidget* parent);

private:
	bool LoadGame(const Configuration& info, QString& error);

	QTabWidget* m_tabs = nullptr;
};

#endif // TROPHY_VIEWER_DIALOG_H
