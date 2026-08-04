#ifndef LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_
#define LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_

#include <QDialog>
#include <QStringList>

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

class InputMappingDialog final: public QDialog {
public:
	explicit InputMappingDialog(const QStringList& mapping, QWidget* parent = nullptr);

	[[nodiscard]] QStringList Mapping() const;

private:
	void ChangeBinding();
	void ClearBinding();
	void RestoreDefaults();
	void SetBinding(QTreeWidgetItem* item, const QString& binding);
	void UpdateButtons();

	QTreeWidget* m_bindings      = nullptr;
	QPushButton* m_change_button = nullptr;
	QPushButton* m_clear_button  = nullptr;
	bool         m_customized    = false;
};

#endif /* LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_ */
