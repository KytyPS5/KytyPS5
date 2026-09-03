#ifndef CONFIGURATION_EDIT_DIALOG_H
#define CONFIGURATION_EDIT_DIALOG_H

#include "common.h"

#include <QDialog>
#include <QString>
#include <QStringList>
class QByteArray;
class QGroupBox;
class QListWidget;
class QMoveEvent;
class QToolButton;
class QSettings;
class QWidget;

class Configuration;
namespace Ui {
class ConfigurationEditDialog;
} // namespace Ui

class ConfigurationEditDialog: public QDialog {
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ConfigurationEditDialog);

public:
	explicit ConfigurationEditDialog(Configuration& info, QWidget* parent = nullptr);
	~ConfigurationEditDialog() override;

	static void WriteSettings(QSettings& s);
	static void ReadSettings(QSettings& s);

	void                      SetTitle(const QString& str);
	void                      SetGameDirectories(const QStringList& dirs);
	[[nodiscard]] QStringList GetGameDirectories() const;

private:
	Ui::ConfigurationEditDialog* m_ui = nullptr;
	Configuration&               m_info;
	QGroupBox*                   m_game_dirs_group        = nullptr;
	QListWidget*                 m_game_dirs_list         = nullptr;
	QToolButton*                 m_remove_game_dir_button = nullptr;
	bool                         m_show_game_dirs         = false;

protected:
	void Init(const Configuration& info);
	void InitGameDirectories();
	void AddGameDirectoryItem(const QString& dir);

	void moveEvent(QMoveEvent* event) override;

	static QByteArray g_last_geometry;

	/*slots:*/

	void update_info();
	void adjust_size();
	void save();
	void clear();
	void add_game_directory();
	void remove_selected_game_directories();
	void update_game_directory_buttons();
};

#endif // CONFIGURATION_EDIT_DIALOG_H
