#ifndef CONFIGURATION_LIST_WIDGET_H
#define CONFIGURATION_LIST_WIDGET_H

#include "common.h"
#include "configuration.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

class ConfigurationItem;
class CompatibilityDatabase;
class QTreeWidgetItem;
class QPoint;
class MainDialog;

namespace Ui {
class ConfigurationListWidget;
} // namespace Ui

class ConfigurationListWidget: public QWidget {
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ConfigurationListWidget);

public:
	explicit ConfigurationListWidget(QWidget* parent = nullptr);
	~ConfigurationListWidget() override;

	void               SetRunEnabled(bool flag) { m_run_enabled = flag; }
	[[nodiscard]] bool IsRunEnabled() const { return m_run_enabled; }

	[[nodiscard]] const ConfigurationItem* GetSelectedItem() const { return m_selected_item; }
	ConfigurationItem*                     GetSelectedItem() { return m_selected_item; }

	void        SetMainDialog(MainDialog* main_dialog) { m_main_dialog = main_dialog; }
	MainDialog* GetMainDialog() { return m_main_dialog; }

	[[nodiscard]] const QString&     GetSettingsFile() const { return m_settings_file; }
	[[nodiscard]] const QStringList& GetGameDirectories() const { return m_game_dirs; }
	[[nodiscard]] bool               CanViewSelectedTrophies() const;

	bool EnsureGameDirectory();
	void ScanGameDirectory();
	void ViewTrophies();

signals:

	void Run();
	void Select();

public slots:
	void WriteSettings();
	void ReadSettings();

protected slots:

	void edit_configuration();
	void delete_configuartion();
	void edit_global_settings();
	void run_configuration();
	void list_currentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous);
	void list_itemDoubleClicked(QTreeWidgetItem* witem, int column);
	void show_context_menu(const QPoint& pos);
	void open_game_folder();
	void remove_save_data();
	void filter_configurations(const QString& text);

private:
	void               ClearCustomSettings(ConfigurationItem* item);
	void               SelectItem(QTreeWidgetItem* witem);
	void               ApplyCompatibility();
	[[nodiscard]] bool HasValidGameDirectory() const;

	ConfigurationItem*            m_selected_item = nullptr;
	bool                          m_run_enabled   = true;
	Ui::ConfigurationListWidget*  m_ui            = nullptr;
	MainDialog*                   m_main_dialog   = nullptr;
	QString                       m_settings_file;
	QStringList                   m_game_dirs;
	Configuration                 m_global_info;
	QMap<QString, Configuration*> m_custom_infos;
	CompatibilityDatabase*        m_compatibility = nullptr;
};

#endif // CONFIGURATION_LIST_WIDGET_H
