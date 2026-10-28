#ifndef ADDITIONAL_CONTENT_DIALOG_H
#define ADDITIONAL_CONTENT_DIALOG_H

#include <QDialog>
#include <QIcon>
#include <QString>
#include <QStringList>

#include <functional>

class Configuration;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class AdditionalContentDialog final: public QDialog {
public:
	// entries are the game's user add-ons, "LABEL" or "LABEL=DIR"; save receives every edit.
	AdditionalContentDialog(const Configuration& game, const QString& folder, QStringList entries,
	                        std::function<void(const QStringList&)> save,
	                        QWidget*                                parent = nullptr);

private:
	void Scan();
	void Add();
	void Remove();

	QString                                 m_title_id;
	QString                                 m_folder;
	QStringList                             m_entries;
	std::function<void(const QStringList&)> m_save;
	QIcon                                   m_game_icon;
	QLabel*                                 m_status     = nullptr;
	QListWidget*                            m_list       = nullptr;
	QPushButton*                            m_open       = nullptr;
	QPushButton*                            m_remove     = nullptr;
	QLineEdit*                              m_label_edit = nullptr;
	QLineEdit*                              m_dir_edit   = nullptr;
};

#endif // ADDITIONAL_CONTENT_DIALOG_H
