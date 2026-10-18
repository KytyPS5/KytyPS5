#include "inputMappingDialog.h"

#include "controllerPreview.h"
#include "dualsenseWidget.h"

#include <QCheckBox>
#include <QComboBox>
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
#include <QSet>
#include <QStringList>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

constexpr auto DEFAULT_MOUSE_SENSITIVITY = 1.0;
constexpr char MOUSE_SENSITIVITY[]       = "MouseSensitivity=";
constexpr char TOUCH_PARTICLE_SKIN[]     = "TouchParticleSkin=";
constexpr char TOUCH_CURSOR_OFF[]        = "TouchCursorImage=Off";
constexpr char KEYBOARD_DISABLED[]       = "Keyboard=Disabled";
constexpr char PAD_DISABLED[]            = "Pad=Disabled";

constexpr char PAD_PREFIX[] = "Pad:";

enum Section { Face, Dpad, Shoulders, Sticks, System, LeftStick, RightStick, Shortcuts, SectionCount };

const char* const SECTION_TITLES[SectionCount] = {
    QT_TR_NOOP("Face buttons"),         QT_TR_NOOP("D-pad"),
    QT_TR_NOOP("Shoulders && triggers"), QT_TR_NOOP("Stick clicks"),
    QT_TR_NOOP("System"),               QT_TR_NOOP("Left stick"),
    QT_TR_NOOP("Right stick"),          QT_TR_NOOP("In-game shortcuts (cycle)"),
};

// pad_default is null for controls that have no gamepad equivalent.
struct PadControl {
	const char* id;
	const char* label;
	const char* keyboard_default;
	const char* pad_default;
	Section     section;
};

constexpr PadControl PAD_CONTROLS[] = {
    {"Cross", "Cross", "J", "South", Face},
    {"Circle", "Circle", "L", "East", Face},
    {"Square", "Square", "K", "West", Face},
    {"Triangle", "Triangle", "I", "North", Face},
    {"Up", "Up", "Up", "DpadUp", Dpad},
    {"Down", "Down", "Down", "DpadDown", Dpad},
    {"Left", "Left", "Left", "DpadLeft", Dpad},
    {"Right", "Right", "Right", "DpadRight", Dpad},
    {"L1", "L1", "Q", "LeftShoulder", Shoulders},
    {"R1", "R1", "E", "RightShoulder", Shoulders},
    {"L2", "L2", "Z", "LeftTrigger", Shoulders},
    {"R2", "R2", "C", "RightTrigger", Shoulders},
    {"L3", "L3 (left stick click)", "Left Shift", "LeftStick", Sticks},
    {"R3", "R3 (right stick click)", "Left Ctrl", "RightStick", Sticks},
    {"Options", "Options", "Return", "Start", System},
    {"TouchPad", "Touch pad click", "Backspace", "Touchpad", System},
    {"TouchPadRight", "Touch pad right", "Tab", nullptr, System},
    {"GyroToggle", "Gyro emulation toggle", "", "Back", System},
    {"TouchPadMode", "Emulated touch pad mode", "F6", "", System},
    {"LeftStickUp", "Up", "W", nullptr, LeftStick},
    {"LeftStickDown", "Down", "S", nullptr, LeftStick},
    {"LeftStickLeft", "Left", "A", nullptr, LeftStick},
    {"LeftStickRight", "Right", "D", nullptr, LeftStick},
    {"RightStickUp", "Up", "T", nullptr, RightStick},
    {"RightStickDown", "Down", "G", nullptr, RightStick},
    {"RightStickLeft", "Left", "F", nullptr, RightStick},
    {"RightStickRight", "Right", "H", nullptr, RightStick},
    {"SpeakerVolume", "Speaker volume", "1", nullptr, Shortcuts},
    {"VibrationIntensity", "Vibration intensity", "2", nullptr, Shortcuts},
    {"TriggerEffectIntensity", "Trigger intensity", "3", nullptr, Shortcuts},
};

struct PadSourceName {
	const char* id;
	const char* label;
};

constexpr PadSourceName PAD_SOURCES[] = {
    {"South", "A / Cross"},
    {"East", "B / Circle"},
    {"West", "X / Square"},
    {"North", "Y / Triangle"},
    {"Back", "View / Back / Share"},
    {"Guide", "Guide / Home"},
    {"Start", "Menu / Start"},
    {"LeftStick", "Left stick click"},
    {"RightStick", "Right stick click"},
    {"LeftShoulder", "LB / L1"},
    {"RightShoulder", "RB / R1"},
    {"DpadUp", "D-pad Up"},
    {"DpadDown", "D-pad Down"},
    {"DpadLeft", "D-pad Left"},
    {"DpadRight", "D-pad Right"},
    {"Misc1", "Share / Misc"},
    {"RightPaddle1", "Right paddle 1"},
    {"LeftPaddle1", "Left paddle 1"},
    {"RightPaddle2", "Right paddle 2"},
    {"LeftPaddle2", "Left paddle 2"},
    {"Touchpad", "Touch pad click"},
    {"LeftTrigger", "LT / L2"},
    {"RightTrigger", "RT / R2"},
};

QString PadSourceLabel(const QString& id) {
	for (const auto& source: PAD_SOURCES) {
		if (id == QLatin1String(source.id)) {
			return QObject::tr(source.label);
		}
	}
	return id;
}

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
		    tr("Press a key or mouse button.\nF1, F2, F7, F8, and F11 are reserved; Esc cancels."),
		    this);
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
		if (event->key() == Qt::Key_F1 || event->key() == Qt::Key_F2 ||
		    event->key() == Qt::Key_F7 || event->key() == Qt::Key_F8 ||
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

class PadCaptureDialog final: public QDialog {
public:
	PadCaptureDialog(ControllerPreview* preview, QWidget* parent): QDialog(parent) {
		setWindowTitle(tr("Set Binding"));
		setModal(true);
		setMinimumWidth(360);

		auto* layout = new QVBoxLayout(this);
		m_label      = new QLabel(this);
		m_label->setAlignment(Qt::AlignCenter);
		m_label->setWordWrap(true);
		layout->addWidget(m_label);

		if (preview == nullptr) {
			return;
		}
		// Inputs already held when the dialog opens must be released before they count.
		m_held = preview->RawPressed();
		connect(preview, &ControllerPreview::StateChanged, this, [this, preview] {
			const auto& pressed = preview->RawPressed();
			m_held.intersect(pressed);
			for (const auto& name: pressed) {
				if (!m_held.contains(name)) {
					m_binding = name;
					accept();
					return;
				}
			}
			UpdateText(preview);
		});
		UpdateText(preview);
	}

	[[nodiscard]] const QString& Binding() const { return m_binding; }

private:
	void UpdateText(const ControllerPreview* preview) {
		m_label->setText(preview->DeviceName().isEmpty()
		                     ? tr("No controller connected.\nConnect one, or press Esc to cancel.")
		                     : tr("Press a button or trigger on %1.\nEsc cancels.")
		                           .arg(preview->DeviceName()));
	}

	QLabel*       m_label = nullptr;
	QString       m_binding;
	QSet<QString> m_held;
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
		if (entry == QLatin1String(KEYBOARD_DISABLED) || entry == QLatin1String(PAD_DISABLED) ||
		    entry.startsWith(QLatin1String(MOUSE_SENSITIVITY)) ||
		    entry.startsWith(QLatin1String(TOUCH_PARTICLE_SKIN)) ||
		    entry.startsWith(QLatin1String("TouchCursorImage=")) ||
		    entry.startsWith(QLatin1String(PAD_PREFIX))) {
			continue;
		}
		const auto separator = entry.indexOf(QLatin1Char('='));
		if (separator > 0 && separator + 1 < entry.size()) {
			AssignBinding(result, entry.left(separator), entry.mid(separator + 1));
		}
	}
	return result;
}

QString ParseTouchParticleSkin(const QStringList& mapping) {
	for (const auto& entry: mapping) {
		if (entry.startsWith(QLatin1String(TOUCH_PARTICLE_SKIN))) {
			return entry.mid(sizeof(TOUCH_PARTICLE_SKIN) - 1);
		}
	}
	return QStringLiteral("Classic");
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
	resize(QSize(1360, 780).boundedTo(screen()->availableGeometry().size()));

	m_bindings        = ParseMapping(mapping);
	m_custom_bindings = !m_bindings.isEmpty() || mapping.contains(QLatin1String(KEYBOARD_DISABLED));
	m_custom_pad      = mapping.contains(QLatin1String(PAD_DISABLED));
	for (const auto& entry: mapping) {
		if (!entry.startsWith(QLatin1String(PAD_PREFIX))) {
			continue;
		}
		const auto body      = entry.mid(static_cast<int>(sizeof(PAD_PREFIX)) - 1);
		const auto separator = body.indexOf(QLatin1Char('='));
		if (separator > 0 && separator + 1 < body.size()) {
			AssignBinding(m_pad_bindings, body.left(separator), body.mid(separator + 1));
			m_custom_pad = true;
		}
	}
	for (const auto& control: PAD_CONTROLS) {
		const auto id = QString::fromLatin1(control.id);
		if (!m_custom_bindings && control.keyboard_default[0] != '\0') {
			m_bindings.insert(id, QString::fromLatin1(control.keyboard_default));
		}
		if (!m_custom_pad && control.pad_default != nullptr) {
			m_pad_bindings.insert(id, QString::fromLatin1(control.pad_default));
		}
	}

	auto* layout = new QVBoxLayout(this);
	layout->setSpacing(10);

	m_tabs = new QTabWidget(this);
	m_tabs->setObjectName(QStringLiteral("inputTabs"));
	m_tabs->setDocumentMode(true);
	m_tabs->addTab(BuildTab(true), tr("Gamepad"));
	m_tabs->addTab(BuildTab(false), tr("Keyboard && Mouse"));
	layout->addWidget(m_tabs, 1);
	m_sensitivity->setValue(ParseMouseSensitivity(mapping));

	auto* touch_group  = new QGroupBox(tr("Emulated touch pad (shared by both tabs)"), this);
	auto* touch_layout = new QHBoxLayout(touch_group);
	touch_layout->addWidget(new QLabel(tr("Particle skin"), touch_group));
	m_particle_skin = new QComboBox(touch_group);
	m_particle_skin->setObjectName(QStringLiteral("touchParticleSkin"));
	m_particle_skin->addItem(tr("Classic"), QStringLiteral("Classic"));
	m_particle_skin->addItem(tr("Blue neon"), QStringLiteral("BlueNeon"));
	m_particle_skin->addItem(tr("Monochrome"), QStringLiteral("Monochrome"));
	m_particle_skin->addItem(tr("Crimson"), QStringLiteral("Crimson"));
	m_particle_skin->addItem(tr("Red/Blue"), QStringLiteral("RedBlue"));
	touch_layout->addWidget(m_particle_skin);
	m_touch_cursor = new QCheckBox(tr("Show hand cursor and trail"), touch_group);
	m_touch_cursor->setObjectName(QStringLiteral("touchCursorImage"));
	m_touch_cursor->setToolTip(tr("When off, the emulated touch pad still works but the hand "
	                              "image and drawing trail are not shown."));
	m_touch_cursor->setChecked(!mapping.contains(QLatin1String(TOUCH_CURSOR_OFF)));
	touch_layout->addWidget(m_touch_cursor);
	touch_layout->addStretch();
	touch_layout->addWidget(new QLabel(
	    tr("Toggle with the touch pad mode key (F6 by default); draw with left mouse / Cross."),
	    touch_group));
	layout->addWidget(touch_group);
	const auto skin_index = m_particle_skin->findData(ParseTouchParticleSkin(mapping));
	m_particle_skin->setCurrentIndex(skin_index >= 0 ? skin_index : 0);

	auto* buttons  = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults |
	                                          QDialogButtonBox::Save | QDialogButtonBox::Cancel,
	                                      this);
	auto* defaults = buttons->button(QDialogButtonBox::RestoreDefaults);
	defaults->setObjectName(QStringLiteral("defaults"));
	defaults->setAutoDefault(false);
	defaults->setToolTip(tr("Restore the defaults of the current tab."));
	auto* tip = new QLabel(tr("Tip: right-click a binding to unmap it."), this);
	auto* footer = new QHBoxLayout;
	footer->addWidget(tip);
	footer->addStretch();
	footer->addWidget(buttons);
	layout->addLayout(footer);

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

QWidget* InputMappingDialog::BuildTab(bool pad) {
	auto* page   = new QWidget(this);
	auto* layout = new QVBoxLayout(page);
	layout->setSpacing(10);

	auto* hint = new QLabel(
	    pad ? tr("Choose which button on your gamepad triggers each PS5 control. Click a "
	             "binding, then press the button or trigger you want to use.")
	        : tr("Choose which keyboard key or mouse button triggers each PS5 control. Click a "
	             "binding, then press the key or mouse button."),
	    page);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	if (pad) {
		m_status = new QLabel(page);
		m_status->setObjectName(QStringLiteral("controllerStatus"));
		m_status->setWordWrap(true);
		layout->addWidget(m_status);
	}

	auto* body = new QHBoxLayout;
	body->setSpacing(14);
	layout->addLayout(body, 1);

	auto* scroll = new QScrollArea(page);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto* content = new QWidget(scroll);
	auto* columns = new QGridLayout(content);
	columns->setContentsMargins(0, 0, 8, 0);
	columns->setSpacing(12);
	columns->setAlignment(Qt::AlignTop);

	// Section order and the column each one lives in.
	static constexpr Section ORDER[] = {Face, Dpad, Shoulders, Sticks, System, LeftStick,
	                                    RightStick, Shortcuts};
	int                      column_rows[2] = {0, 0};
	for (const auto section: ORDER) {
		QGroupBox*   group = nullptr;
		QGridLayout* rows  = nullptr;
		int          row   = 0;
		for (const auto& control: PAD_CONTROLS) {
			if (control.section != section || (pad && control.pad_default == nullptr)) {
				continue;
			}
			if (group == nullptr) {
				group = new QGroupBox(tr(SECTION_TITLES[section]), content);
				rows  = new QGridLayout(group);
				rows->setHorizontalSpacing(12);
				rows->setVerticalSpacing(6);
				rows->setColumnStretch(0, 1);
				rows->setColumnMinimumWidth(1, 190);
			}
			const auto id    = QString::fromLatin1(control.id);
			const auto label = !pad && id == QLatin1String("TouchPad") ? tr("Touch pad left")
			                                                           : tr(control.label);
			auto* name = new QLabel(label, group);
			auto* button = new QPushButton(group);
			button->setObjectName((pad ? QStringLiteral("padBinding_") : QStringLiteral("binding_")) +
			                      id);
			button->setAutoDefault(false);
			button->setMinimumWidth(190);
			button->setContextMenuPolicy(Qt::CustomContextMenu);
			button->setAccessibleName(tr(SECTION_TITLES[section]) + QStringLiteral(" / ") + label);
			rows->addWidget(name, row, 0);
			rows->addWidget(button, row, 1);
			++row;
			(pad ? m_pad_buttons : m_buttons).insert(id, button);
			connect(button, &QPushButton::clicked, this, [this, pad, id] {
				pad ? ChangePadBinding(id) : ChangeKeyboardBinding(id);
			});
			connect(button, &QPushButton::customContextMenuRequested, this,
			        [this, pad, id] { ClearBinding(pad, id); });
		}
		if (group != nullptr) {
			const int target = column_rows[0] <= column_rows[1] ? 0 : 1;
			columns->addWidget(group, column_rows[target]++, target);
		}
	}
	columns->setColumnStretch(0, 1);
	columns->setColumnStretch(1, 1);
	scroll->setWidget(content);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setMinimumWidth(pad ? 640 : 700);
	body->addWidget(scroll, 3);

	if (pad) {
		m_controller = new DualSenseWidget(page);
		m_controller->setObjectName(QStringLiteral("controllerDiagram"));
		m_controller->setMinimumWidth(320);
		body->addWidget(m_controller, 2);
		connect(m_controller, &DualSenseWidget::ControlClicked, this,
		        [this](const QString& id) {
			        ChangePadBinding(id == QLatin1String("TouchPadRight") ? QStringLiteral("TouchPad")
			                                                               : id);
		        });
		auto* note = new QLabel(tr("The gyro emulation toggle activates tilt mode on gamepads "
		                           "without a gyroscope: the right stick then controls pitch and "
		                           "roll. It defaults to the View/Back button."),
		                        page);
		note->setWordWrap(true);
		layout->addWidget(note);
	} else {
		auto* sensitivity_layout = new QHBoxLayout;
		sensitivity_layout->addWidget(new QLabel(tr("Mouse sensitivity"), page));
		m_sensitivity = new QDoubleSpinBox(page);
		m_sensitivity->setObjectName(QStringLiteral("mouseSensitivity"));
		m_sensitivity->setRange(0.1, 5.0);
		m_sensitivity->setSingleStep(0.1);
		m_sensitivity->setDecimals(1);
		m_sensitivity->setSuffix(QStringLiteral("×"));
		m_sensitivity->setValue(DEFAULT_MOUSE_SENSITIVITY);
		sensitivity_layout->addWidget(m_sensitivity);
		sensitivity_layout->addWidget(
		    new QLabel(tr("F7 in-game toggles mouse movement on the right stick."), page));
		sensitivity_layout->addStretch();
		layout->addLayout(sensitivity_layout);

	}
	return page;
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
		if (result.isEmpty()) {
			result.append(QLatin1String(KEYBOARD_DISABLED));
		}
	}
	if (m_custom_pad) {
		const auto start = result.size();
		for (const auto& control: PAD_CONTROLS) {
			const auto id     = QString::fromLatin1(control.id);
			const auto source = m_pad_bindings.value(id);
			if (!source.isEmpty()) {
				result.append(QLatin1String(PAD_PREFIX) + id + QLatin1Char('=') + source);
			}
		}
		if (result.size() == start) {
			result.append(QLatin1String(PAD_DISABLED));
		}
	}
	if (m_sensitivity->value() != DEFAULT_MOUSE_SENSITIVITY) {
		result.append(QLatin1String(MOUSE_SENSITIVITY) +
		              QString::number(m_sensitivity->value(), 'f', 1));
	}
	const auto skin = m_particle_skin->currentData().toString();
	if (skin != QLatin1String("Classic")) {
		result.append(QLatin1String(TOUCH_PARTICLE_SKIN) + skin);
	}
	if (!m_touch_cursor->isChecked()) {
		result.append(QLatin1String(TOUCH_CURSOR_OFF));
	}
	return result;
}

void InputMappingDialog::ChangeKeyboardBinding(const QString& id) {
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

void InputMappingDialog::ChangePadBinding(const QString& id) {
	if (!m_pad_buttons.contains(id)) {
		return;
	}
	m_selected = id;
	UpdateHighlights();
	PadCaptureDialog dialog(m_preview, this);
	dialog.setWindowTitle(tr("Set binding — %1").arg(m_pad_buttons.value(id)->accessibleName()));
	if (dialog.exec() == QDialog::Accepted) {
		AssignBinding(m_pad_bindings, id, dialog.Binding());
		m_custom_pad = true;
	}
	m_selected.clear();
	UpdateButtons();
}

void InputMappingDialog::ClearBinding(bool pad, const QString& id) {
	if (pad) {
		m_pad_bindings.remove(id);
		m_custom_pad = true;
	} else {
		m_bindings.remove(id);
		m_custom_bindings = true;
	}
	UpdateButtons();
}

void InputMappingDialog::RestoreDefaults() {
	if (m_tabs->currentIndex() == 0) {
		m_pad_bindings.clear();
		for (const auto& control: PAD_CONTROLS) {
			if (control.pad_default != nullptr) {
				m_pad_bindings.insert(QString::fromLatin1(control.id),
				                      QString::fromLatin1(control.pad_default));
			}
		}
		m_custom_pad = false;
	} else {
		m_bindings.clear();
		for (const auto& control: PAD_CONTROLS) {
			if (control.keyboard_default[0] != '\0') {
				m_bindings.insert(QString::fromLatin1(control.id),
				                  QString::fromLatin1(control.keyboard_default));
			}
		}
		m_sensitivity->setValue(DEFAULT_MOUSE_SENSITIVITY);
		m_custom_bindings = false;
	}
	UpdateButtons();
}

void InputMappingDialog::UpdateButtons() {
	for (auto item = m_buttons.begin(); item != m_buttons.end(); ++item) {
		auto*      button  = item.value();
		const auto binding = m_bindings.value(item.key());
		const auto label   = binding.isEmpty() ? tr("None") : binding;
		button->setText(QString(label).replace(QLatin1Char('&'), QLatin1String("&&")));
		button->setToolTip(
		    tr("%1: %2. Click to change, right-click to clear.").arg(button->accessibleName(), label));
	}
	for (auto item = m_pad_buttons.begin(); item != m_pad_buttons.end(); ++item) {
		auto*      button  = item.value();
		const auto source  = m_pad_bindings.value(item.key());
		const auto label   = source.isEmpty() ? tr("None") : PadSourceLabel(source);
		button->setText(QString(label).replace(QLatin1Char('&'), QLatin1String("&&")));
		button->setToolTip(
		    tr("%1: %2. Click to change, right-click to clear.").arg(button->accessibleName(), label));
	}
	UpdateHighlights();
}

void InputMappingDialog::UpdatePreview() {
	m_pad_pressed.clear();
	if (m_preview != nullptr) {
		const auto& raw = m_preview->RawPressed();
		for (auto item = m_pad_bindings.begin(); item != m_pad_bindings.end(); ++item) {
			if (raw.contains(item.value())) {
				m_pad_pressed.insert(item.key());
			}
		}
		if (m_pad_pressed.contains(QStringLiteral("TouchPad"))) {
			m_pad_pressed.insert(QStringLiteral("TouchPadRight"));
		}
		for (const auto& name: m_preview->Pressed()) {
			// Stick directions and touch pad fingers are shown as the pad reports them.
			if (name.startsWith(QLatin1String("LeftStick")) ||
			    name.startsWith(QLatin1String("RightStick")) ||
			    name.startsWith(QLatin1String("TouchPad"))) {
				m_pad_pressed.insert(name);
			}
		}
	}
	m_controller->SetPressed(m_pad_pressed);
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
	for (auto item = m_pad_buttons.begin(); item != m_pad_buttons.end(); ++item) {
		const bool down   = m_pad_pressed.contains(item.key());
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
	for (auto item = m_buttons.begin(); item != m_buttons.end(); ++item) {
		auto colors = palette();
		if (item.key() == m_selected) {
			colors.setColor(QPalette::ButtonText, colors.color(QPalette::Highlight));
		}
		item.value()->setPalette(colors);
	}
}
