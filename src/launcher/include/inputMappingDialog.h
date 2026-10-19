#ifndef LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_
#define LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QStringList>

class ControllerPreview;
class DualSenseWidget;
class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QTabWidget;
class QWidget;

class InputMappingDialog final: public QDialog {
public:
	explicit InputMappingDialog(const QStringList& mapping, ControllerPreview* preview,
	                            QWidget* parent = nullptr);
	~InputMappingDialog() override;

	[[nodiscard]] QStringList Mapping() const;

private:
	QWidget* BuildTab(bool pad);
	void     ChangeKeyboardBinding(const QString& id);
	void     ChangePadBinding(const QString& id);
	void     ClearBinding(bool pad, const QString& id);
	void     RestoreDefaults();
	void     UpdateButtons();
	void     UpdatePreview();
	void     UpdateHighlights();

	ControllerPreview*           m_preview;
	DualSenseWidget*             m_controller = nullptr;
	QTabWidget*                  m_tabs       = nullptr;
	QHash<QString, QString>      m_bindings;
	QHash<QString, QString>      m_pad_bindings;
	QHash<QString, QPushButton*> m_buttons;
	QHash<QString, QPushButton*> m_pad_buttons;
	QSet<QString>                m_pad_pressed;
	QString                      m_selected;
	QLabel*                      m_status          = nullptr;
	QDoubleSpinBox*              m_sensitivity     = nullptr;
	QComboBox*                   m_particle_skin   = nullptr;
	QCheckBox*                   m_touch_cursor    = nullptr;
	bool                         m_custom_bindings = false;
	bool                         m_custom_pad      = false;
};

#endif /* LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_ */
