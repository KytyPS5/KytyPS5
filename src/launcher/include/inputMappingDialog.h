#ifndef LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_
#define LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_

#include <QDialog>
#include <QHash>
#include <QStringList>

class ControllerPreview;
class DualSenseWidget;
class QLabel;
class QPushButton;
class QDoubleSpinBox;

class InputMappingDialog final: public QDialog {
public:
	explicit InputMappingDialog(const QStringList& mapping, ControllerPreview* preview,
	                            QWidget* parent = nullptr);
	~InputMappingDialog() override;

	[[nodiscard]] QStringList Mapping() const;

private:
	void ChangeBinding(const QString& id);
	void RestoreDefaults();
	void UpdateButtons();
	void UpdatePreview();
	void UpdateHighlights();

	ControllerPreview*           m_preview;
	DualSenseWidget*             m_controller = nullptr;
	QHash<QString, QString>      m_bindings;
	QHash<QString, QPushButton*> m_buttons;
	QString                      m_selected;
	QLabel*                      m_status          = nullptr;
	QDoubleSpinBox*              m_sensitivity     = nullptr;
	bool                         m_custom_bindings = false;
};

#endif /* LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_ */
