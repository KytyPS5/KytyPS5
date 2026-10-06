#ifndef TROPHY_VIEWER_DIALOG_H
#define TROPHY_VIEWER_DIALOG_H

#include <QDialog>

class QTabWidget;
class QWidget;

class Configuration;
class TrophyViewerDialog: public QDialog {
public:
	explicit TrophyViewerDialog(QWidget* parent = nullptr);

	static bool HasTrophyData(const Configuration* info);
	static void ShowForGame(const Configuration* info, const QString& runtime_directory,
	                        QWidget* parent);

private:
	bool LoadGame(const Configuration& info, const QString& runtime_directory, QString& error);

	QTabWidget* m_tabs = nullptr;
};

#endif // TROPHY_VIEWER_DIALOG_H
