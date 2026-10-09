#include "inputMappingDialog.h"

#include "controllerPreview.h"
#include "dualsenseWidget.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>

namespace {

constexpr auto DEFAULT_MOUSE_SENSITIVITY = 1.0;
constexpr char MOUSE_SENSITIVITY[]       = "MouseSensitivity=";

enum Group { Dpad, LeftStick, RightStick, Face, Shoulders, Center, Shortcuts, GroupCount };

struct PadControl {
	const char* id;
	const char* label;
	const char* default_binding;
	Group       group;
	int         row;
	int         column;
};

constexpr PadControl PAD_CONTROLS[] = {
    {"Up", "Up", "Up", Dpad, 0, 1},
    {"Down", "Down", "Down", Dpad, 2, 1},
    {"Left", "Left", "Left", Dpad, 1, 0},
    {"Right", "Right", "Right", Dpad, 1, 2},
    {"LeftStickUp", "Up", "W", LeftStick, 0, 1},
    {"LeftStickDown", "Down", "S", LeftStick, 2, 1},
    {"LeftStickLeft", "Left", "A", LeftStick, 1, 0},
    {"LeftStickRight", "Right", "D", LeftStick, 1, 2},
    {"RightStickUp", "Up", "T", RightStick, 0, 1},
    {"RightStickDown", "Down", "G", RightStick, 2, 1},
    {"RightStickLeft", "Left", "F", RightStick, 1, 0},
    {"RightStickRight", "Right", "H", RightStick, 1, 2},
    {"Triangle", "Triangle", "I", Face, 0, 1},
    {"Circle", "Circle", "L", Face, 1, 2},
    {"Cross", "Cross", "J", Face, 2, 1},
    {"Square", "Square", "K", Face, 1, 0},
    {"L1", "L1", "Q", Shoulders, 0, 1},
    {"R1", "R1", "E", Shoulders, 0, 2},
    {"L2", "L2", "Z", Shoulders, 0, 0},
    {"R2", "R2", "C", Shoulders, 0, 3},
    {"L3", "Press / L3", "Left Shift", LeftStick, 1, 1},
    {"R3", "Press / R3", "Left Ctrl", RightStick, 1, 1},
    {"Options", "Options", "Return", Center, 0, 1},
    {"TouchPad", "Touch pad left", "Backspace", Center, 0, 0},
    {"TouchPadRight", "Touch pad right", "Tab", Center, 0, 2},
    {"SpeakerVolume", "Speaker volume", "1", Shortcuts, 0, 0},
    {"VibrationIntensity", "Vibration intensity", "2", Shortcuts, 0, 1},
    {"TriggerEffectIntensity", "Trigger intensity", "3", Shortcuts, 0, 2},
};

QString KeypadName(int key) {
	if (key >= Qt::Key_0 && key <= Qt::Key_9) {
		return QStringLiteral("Keypad %1").arg(key - Qt::Key_0);
	}

	switch (key) {
		case Qt::Key_Return:
		case Qt::Key_Enter: return QStringLiteral("Keypad Enter");
		case Qt::Key_Slash: return QStringLiteral("Keypad /");
		case Qt::Key_Asterisk: return QStringLiteral("Keypad *");
		case Qt::Key_Minus: return QStringLiteral("Keypad -");
		case Qt::Key_Plus: return QStringLiteral("Keypad +");
		case Qt::Key_Period: return QStringLiteral("Keypad .");
		case Qt::Key_Equal: return QStringLiteral("Keypad =");
		case Qt::Key_Comma: return QStringLiteral("Keypad ,");
		default: return {};
	}
}

QString KeyName(const QKeyEvent& event) {
	const int key = event.key();
	if (event.modifiers().testFlag(Qt::KeypadModifier)) {
		return KeypadName(key);
	}
	if (key >= Qt::Key_A && key <= Qt::Key_Z) {
		return QChar(key);
	}
	if (key >= Qt::Key_0 && key <= Qt::Key_9) {
		return QChar(key);
	}
	if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
		return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
	}

	switch (key) {
		case Qt::Key_Space: return QStringLiteral("Space");
		case Qt::Key_Return:
		case Qt::Key_Enter: return QStringLiteral("Return");
		case Qt::Key_Backspace: return QStringLiteral("Backspace");
		case Qt::Key_Tab: return QStringLiteral("Tab");
		case Qt::Key_Shift: return QStringLiteral("Left Shift");
		case Qt::Key_Control: return QStringLiteral("Left Ctrl");
		case Qt::Key_Alt: return QStringLiteral("Left Alt");
		case Qt::Key_Meta: return QStringLiteral("Left GUI");
		case Qt::Key_Insert: return QStringLiteral("Insert");
		case Qt::Key_Delete: return QStringLiteral("Delete");
		case Qt::Key_Home: return QStringLiteral("Home");
		case Qt::Key_End: return QStringLiteral("End");
		case Qt::Key_PageUp: return QStringLiteral("PageUp");
		case Qt::Key_PageDown: return QStringLiteral("PageDown");
		case Qt::Key_Left: return QStringLiteral("Left");
		case Qt::Key_Right: return QStringLiteral("Right");
		case Qt::Key_Up: return QStringLiteral("Up");
		case Qt::Key_Down: return QStringLiteral("Down");
		case Qt::Key_CapsLock: return QStringLiteral("CapsLock");
		case Qt::Key_NumLock: return QStringLiteral("Numlock");
		case Qt::Key_ScrollLock: return QStringLiteral("ScrollLock");
		case Qt::Key_Pause: return QStringLiteral("Pause");
		case Qt::Key_Print: return QStringLiteral("PrintScreen");
		default: break;
	}

	if (event.modifiers() != Qt::NoModifier) {
		return {};
	}
	const auto name = QKeySequence(key).toString(QKeySequence::PortableText);
	return name.size() == 1 ? name : QString();
}

class InputCaptureDialog final: public QDialog {
public:
	explicit InputCaptureDialog(QWidget* parent): QDialog(parent) {
		setWindowTitle(tr("Set Binding"));
		setModal(true);
		setMinimumWidth(360);

		auto* layout = new QVBoxLayout(this);
		m_label      = new QLabel(
		    tr("Press a key or mouse button.\nF1, F7, and F11 are reserved; Esc cancels."), this);
		m_label->setAlignment(Qt::AlignCenter);
		layout->addWidget(m_label);
	}

	[[nodiscard]] const QString& Binding() const { return m_binding; }

protected:
	void keyPressEvent(QKeyEvent* event) override {
		if (event->isAutoRepeat()) {
			return;
		}
		if (event->key() == Qt::Key_Escape) {
			reject();
			return;
		}
		if (event->key() == Qt::Key_F1 || event->key() == Qt::Key_F7 ||
		    event->key() == Qt::Key_F11) {
			m_label->setText(tr("That key is reserved by the emulator."));
			return;
		}

		m_binding = KeyName(*event);
		if (!m_binding.isEmpty()) {
			accept();
		} else {
			m_label->setText(tr("That key is not supported."));
		}
	}

	void mousePressEvent(QMouseEvent* event) override {
		switch (event->button()) {
			case Qt::LeftButton: m_binding = QStringLiteral("Mouse:Left"); break;
			case Qt::RightButton: m_binding = QStringLiteral("Mouse:Right"); break;
			case Qt::MiddleButton: m_binding = QStringLiteral("Mouse:Middle"); break;
			case Qt::BackButton: m_binding = QStringLiteral("Mouse:X1"); break;
			case Qt::ForwardButton: m_binding = QStringLiteral("Mouse:X2"); break;
			default: return;
		}
		accept();
	}

private:
	QLabel* m_label = nullptr;
	QString m_binding;
};

void AssignBinding(QHash<QString, QString>& bindings, const QString& id, const QString& binding) {
	for (auto item = bindings.begin(); item != bindings.end();) {
		if (item.value().compare(binding, Qt::CaseInsensitive) == 0) {
			item = bindings.erase(item);
		} else {
			++item;
		}
	}
	bindings.insert(id, binding);
}

QHash<QString, QString> ParseMapping(const QStringList& mapping) {
	QHash<QString, QString> result;
	for (const auto& entry: mapping) {
		if (entry.startsWith(QLatin1String(MOUSE_SENSITIVITY))) {
			continue;
		}
		const auto separator = entry.indexOf(QLatin1Char('='));
		if (separator > 0 && separator + 1 < entry.size()) {
			AssignBinding(result, entry.left(separator), entry.mid(separator + 1));
		}
	}
	return result;
}

double ParseMouseSensitivity(const QStringList& mapping) {
	for (const auto& entry: mapping) {
		if (entry.startsWith(QLatin1String(MOUSE_SENSITIVITY))) {
			return entry.mid(sizeof(MOUSE_SENSITIVITY) - 1).toDouble();
		}
	}
	return DEFAULT_MOUSE_SENSITIVITY;
}

} // namespace

InputMappingDialog::InputMappingDialog(const QStringList& mapping, ControllerPreview* preview,
                                       QWidget* parent)
    : QDialog(parent), m_preview(preview) {
	setWindowTitle(tr("Controls"));
	resize(QSize(1100, 820).boundedTo(screen()->availableGeometry().size()));

	auto* layout = new QVBoxLayout(this);
	layout->setSpacing(10);
	auto* title = new QLabel(tr("Controller setup"), this);
	auto  font  = title->font();
	font.setPointSize(font.pointSize() + 7);
	font.setBold(true);
	title->setFont(font);
	layout->addWidget(title);
	auto* hint = new QLabel(
	    tr("Select a button to remap. Press controller buttons to test."), this);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	m_status = new QLabel(this);
	m_status->setObjectName(QStringLiteral("controllerStatus"));
	m_status->setWordWrap(true);
	layout->addWidget(m_status);

	auto* scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto* content = new QWidget(scroll);
	auto* grid    = new QGridLayout(content);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setSpacing(12);
	QGroupBox*   groups[GroupCount];
	QGridLayout* cells[GroupCount];
	const char*  titles[] = {"D-Pad",
	                         "Left stick",
	                         "Right stick",
	                         "Face buttons",
	                         "Shoulders and triggers",
	                         "Touch pad and options",
	                         "In-game shortcuts (cycle)"};
	for (int index = 0; index < GroupCount; ++index) {
		groups[index] = new QGroupBox(tr(titles[index]), content);
		cells[index]  = new QGridLayout(groups[index]);
		cells[index]->setSpacing(5);
	}

	m_bindings        = ParseMapping(mapping);
	m_custom_bindings = !m_bindings.isEmpty();
	for (const auto& control: PAD_CONTROLS) {
		const auto id          = QString::fromLatin1(control.id);
		auto*      cell        = new QGroupBox(tr(control.label), groups[control.group]);
		auto*      cell_layout = new QVBoxLayout(cell);
		cell_layout->setContentsMargins(3, 5, 3, 3);
		auto* button = new QPushButton(cell);
		button->setObjectName(QStringLiteral("binding_") + id);
		button->setAutoDefault(false);
		button->setMinimumWidth(64);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		button->setAccessibleName(tr(titles[control.group]) + QStringLiteral(" / ") +
		                          tr(control.label));
		cell_layout->addWidget(button);
		cells[control.group]->addWidget(cell, control.row, control.column);
		m_buttons.insert(id, button);
		if (!m_custom_bindings) {
			m_bindings.insert(id, QString::fromLatin1(control.default_binding));
		}
		connect(button, &QPushButton::clicked, this, [this, id] { ChangeBinding(id); });
	}

	auto* left = new QVBoxLayout;
	left->addWidget(groups[Dpad]);
	left->addWidget(groups[LeftStick]);
	left->addStretch();
	grid->addLayout(left, 0, 0);
	auto* center = new QVBoxLayout;
	center->addWidget(groups[Shoulders]);
	m_controller = new DualSenseWidget(content);
	m_controller->setObjectName(QStringLiteral("controllerDiagram"));
	center->addWidget(m_controller, 1);
	center->addWidget(groups[Center]);
	grid->addLayout(center, 0, 1);
	auto* right = new QVBoxLayout;
	right->addWidget(groups[Face]);
	right->addWidget(groups[RightStick]);
	right->addStretch();
	grid->addLayout(right, 0, 2);
	grid->setColumnStretch(1, 1);
	grid->addWidget(groups[Shortcuts], 1, 0, 1, 3);
	scroll->setWidget(content);
	layout->addWidget(scroll, 1);

	auto* sensitivity_layout = new QHBoxLayout;
	sensitivity_layout->addWidget(new QLabel(tr("Mouse sensitivity"), this));
	m_sensitivity = new QDoubleSpinBox(this);
	m_sensitivity->setObjectName(QStringLiteral("mouseSensitivity"));
	m_sensitivity->setRange(0.1, 5.0);
	m_sensitivity->setSingleStep(0.1);
	m_sensitivity->setDecimals(1);
	m_sensitivity->setSuffix(QStringLiteral("×"));
	m_sensitivity->setValue(ParseMouseSensitivity(mapping));
	sensitivity_layout->addWidget(m_sensitivity);
	sensitivity_layout->addWidget(
	    new QLabel(tr("F7 in-game toggles mouse movement on the right stick."), this));
	sensitivity_layout->addStretch();
	layout->addLayout(sensitivity_layout);

	auto* buttons  = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults |
	                                          QDialogButtonBox::Save | QDialogButtonBox::Cancel,
	                                      this);
	auto* defaults = buttons->button(QDialogButtonBox::RestoreDefaults);
	defaults->setObjectName(QStringLiteral("defaults"));
	defaults->setAutoDefault(false);
	layout->addWidget(buttons);
	connect(m_controller, &DualSenseWidget::ControlClicked, this,
	        &InputMappingDialog::ChangeBinding);
	connect(defaults, &QPushButton::clicked, this, &InputMappingDialog::RestoreDefaults);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	if (m_preview != nullptr) {
		connect(m_preview, &ControllerPreview::StateChanged, this,
		        &InputMappingDialog::UpdatePreview);
		m_preview->SetPreviewEnabled(true);
	}
	UpdateButtons();
	UpdatePreview();
}

InputMappingDialog::~InputMappingDialog() {
	if (m_preview != nullptr) {
		m_preview->SetPreviewEnabled(false);
	}
}

QStringList InputMappingDialog::Mapping() const {
	QStringList result;
	if (m_custom_bindings) {
		for (const auto& control: PAD_CONTROLS) {
			const auto id      = QString::fromLatin1(control.id);
			const auto binding = m_bindings.value(id);
			if (!binding.isEmpty()) {
				result.append(id + QLatin1Char('=') + binding);
			}
		}
	}
	if (m_sensitivity->value() != DEFAULT_MOUSE_SENSITIVITY) {
		result.append(QLatin1String(MOUSE_SENSITIVITY) +
		              QString::number(m_sensitivity->value(), 'f', 1));
	}
	return result;
}

void InputMappingDialog::ChangeBinding(const QString& id) {
	if (!m_buttons.contains(id)) {
		return;
	}
	m_selected = id;
	UpdateHighlights();
	InputCaptureDialog dialog(this);
	dialog.setWindowTitle(tr("Set binding — %1").arg(m_buttons.value(id)->accessibleName()));
	if (dialog.exec() == QDialog::Accepted) {
		AssignBinding(m_bindings, id, dialog.Binding());
		m_custom_bindings = true;
	}
	m_selected.clear();
	UpdateButtons();
}

void InputMappingDialog::RestoreDefaults() {
	m_bindings.clear();
	for (const auto& control: PAD_CONTROLS) {
		m_bindings.insert(QString::fromLatin1(control.id),
		                  QString::fromLatin1(control.default_binding));
	}
	m_sensitivity->setValue(DEFAULT_MOUSE_SENSITIVITY);
	m_custom_bindings = false;
	UpdateButtons();
}

void InputMappingDialog::UpdateButtons() {
	for (auto item = m_buttons.begin(); item != m_buttons.end(); ++item) {
		auto*      button  = item.value();
		const auto binding = m_bindings.value(item.key());
		const auto label   = binding.isEmpty() ? tr("None") : binding;
		button->setText(QString(label).replace(QLatin1Char('&'), QLatin1String("&&")));
		button->setToolTip(tr("%1: %2. Click to change.").arg(button->accessibleName(), label));
	}
	UpdateHighlights();
}

void InputMappingDialog::UpdatePreview() {
	const auto pressed = m_preview != nullptr ? m_preview->Pressed() : QSet<QString>();
	m_controller->SetPressed(pressed);
	m_controller->SetSticks(m_preview != nullptr ? m_preview->LeftStick() : QPointF(),
	                        m_preview != nullptr ? m_preview->RightStick() : QPointF());
	UpdateHighlights();
	if (m_preview != nullptr && !m_preview->Error().isEmpty()) {
		m_status->setText(tr("Controller preview unavailable: %1").arg(m_preview->Error()));
	} else if (m_preview != nullptr && !m_preview->DeviceName().isEmpty()) {
		m_status->setText(tr("Connected: %1").arg(m_preview->DeviceName()));
	} else {
		m_status->setText(tr("No controller connected"));
	}
}

void InputMappingDialog::UpdateHighlights() {
	m_controller->SetSelected(m_selected);
	for (auto item = m_buttons.begin(); item != m_buttons.end(); ++item) {
		const bool down   = m_preview != nullptr && m_preview->Pressed().contains(item.key());
		auto       colors = palette();
		if (down) {
			colors.setColor(QPalette::Button, colors.color(QPalette::Highlight));
			colors.setColor(QPalette::ButtonText, colors.color(QPalette::HighlightedText));
		} else if (item.key() == m_selected) {
			colors.setColor(QPalette::ButtonText, colors.color(QPalette::Highlight));
		}
		auto label_font = font();
		label_font.setBold(down);
		item.value()->setPalette(colors);
		item.value()->setFont(label_font);
	}
}
