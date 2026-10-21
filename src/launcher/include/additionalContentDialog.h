#ifndef ADDITIONAL_CONTENT_DIALOG_H
#define ADDITIONAL_CONTENT_DIALOG_H

#include <QDialog>
#include <QIcon>
#include <QString>

class Configuration;
class QLabel;
class QListWidget;
class QPushButton;

class AdditionalContentDialog final: public QDialog {
public:
	AdditionalContentDialog(const Configuration& game, const QString& folder,
	                        QWidget* parent = nullptr);

private:
	void Scan();

	QString      m_title_id;
	QString      m_folder;
	QIcon        m_game_icon;
	QLabel*      m_status = nullptr;
	QListWidget* m_list   = nullptr;
	QPushButton* m_open   = nullptr;
};

#endif // ADDITIONAL_CONTENT_DIALOG_H
