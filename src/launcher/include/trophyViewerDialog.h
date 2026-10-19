#ifndef TROPHY_VIEWER_DIALOG_H
#define TROPHY_VIEWER_DIALOG_H

#include <QDialog>

#include <vector>

class QTabWidget;
class QWidget;

class Configuration;
class TrophyViewerDialog: public QDialog {
public:
	explicit TrophyViewerDialog(QWidget* parent = nullptr);

	static bool HasTrophyData(const Configuration* info);
	static bool GetProgress(const Configuration* info, const QString& runtime_directory,
	                        unsigned& earned, unsigned& total, unsigned& percent);
	static void ShowForGame(const Configuration* info, const QString& runtime_directory,
	                        QWidget* parent);
	static void ShowOverview(const std::vector<const Configuration*>& games,
	                         const QString& runtime_directory, QWidget* parent);

private:
	bool LoadGame(const Configuration& info, const QString& runtime_directory, QString& error);

	QTabWidget* m_tabs = nullptr;
};

#endif // TROPHY_VIEWER_DIALOG_H
