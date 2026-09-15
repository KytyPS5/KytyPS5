#include "inputMappingDialog.h"

#include <QAbstractItemView>
#include <QChar>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSize>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int  BINDING_COLUMN            = 1;
constexpr auto DEFAULT_MOUSE_SENSITIVITY = 1.0;
constexpr char MOUSE_SENSITIVITY[]       = "MouseSensitivity=";

struct PadControl {
	const char* id;
	const char* label;
	const char* default_binding;
	const char* icon;
};

constexpr PadControl PAD_CONTROLS[] = {
    {"Up", "D-pad Up", "Up", "pad/dpad_up"},
    {"Down", "D-pad Down", "Down", "pad/dpad_down"},
    {"Left", "D-pad Left", "Left", "pad/dpad_left"},
    {"Right", "D-pad Right", "Right", "pad/dpad_right"},
    {"LeftStickUp", "Left stick Up", "W", "pad/lstick_up"},
    {"LeftStickDown", "Left stick Down", "S", "pad/lstick_down"},
    {"LeftStickLeft", "Left stick Left", "A", "pad/lstick_left"},
    {"LeftStickRight", "Left stick Right", "D", "pad/lstick_right"},
    {"RightStickUp", "Right stick Up", "T", "pad/rstick_up"},
    {"RightStickDown", "Right stick Down", "G", "pad/rstick_down"},
    {"RightStickLeft", "Right stick Left", "F", "pad/rstick_left"},
    {"RightStickRight", "Right stick Right", "H", "pad/rstick_right"},
    {"Triangle", "Triangle", "I", "pad/triangle"},
    {"Circle", "Circle", "L", "pad/circle"},
    {"Cross", "Cross", "J", "pad/cross"},
    {"Square", "Square", "K", "pad/square"},
    {"L1", "L1", "Q", "pad/l1"},
    {"R1", "R1", "E", "pad/r1"},
    {"L2", "L2", "", "pad/l2"},
    {"R2", "R2", "", "pad/r2"},
    {"L3", "L3", "Left Shift", "pad/l3"},
    {"R3", "R3", "Left Ctrl", "pad/r3"},
    {"Options", "Options", "Return", "pad/options"},
    {"TouchPad", "Touch pad left (SELECT)", "Backspace", "pad/touchpad"},
    {"TouchPadRight", "Touch pad right (START)", "Tab", "pad/touchpad"},
};

QIcon InputIcon(const QString& alias) {
	if (alias.isEmpty()) {
		return {};
	}
	QIcon icon(QStringLiteral(":/input/%1.png").arg(alias));
	return icon.isNull() ? QIcon() : icon;
}

// Maps a captured host-input name (see KeyName / mousePressEvent) to a bundled
// key-prompt icon. Returns a null icon when nothing fits, leaving the text label.
QIcon HostInputIcon(const QString& binding) {
	if (binding.isEmpty()) {
		return {};
	}

	static const QHash<QString, QString> named = {
	    {QStringLiteral("Return"), QStringLiteral("key/return")},
	    {QStringLiteral("Backspace"), QStringLiteral("key/backspace")},
	    {QStringLiteral("Tab"), QStringLiteral("key/tab")},
	    {QStringLiteral("Space"), QStringLiteral("key/space")},
	    {QStringLiteral("Left Shift"), QStringLiteral("key/shift")},
	    {QStringLiteral("Left Ctrl"), QStringLiteral("key/ctrl")},
	    {QStringLiteral("Left Alt"), QStringLiteral("key/alt")},
	    {QStringLiteral("Insert"), QStringLiteral("key/insert")},
	    {QStringLiteral("Delete"), QStringLiteral("key/delete")},
	    {QStringLiteral("Home"), QStringLiteral("key/home")},
	    {QStringLiteral("End"), QStringLiteral("key/end")},
	    {QStringLiteral("PageUp"), QStringLiteral("key/pageup")},
	    {QStringLiteral("PageDown"), QStringLiteral("key/pagedown")},
	    {QStringLiteral("CapsLock"), QStringLiteral("key/capslock")},
	    {QStringLiteral("Numlock"), QStringLiteral("key/numlock")},
	    {QStringLiteral("PrintScreen"), QStringLiteral("key/printscreen")},
	    {QStringLiteral("Left"), QStringLiteral("key/arrow_left")},
	    {QStringLiteral("Right"), QStringLiteral("key/arrow_right")},
	    {QStringLiteral("Up"), QStringLiteral("key/arrow_up")},
	    {QStringLiteral("Down"), QStringLiteral("key/arrow_down")},
	    {QStringLiteral("Mouse:Left"), QStringLiteral("mouse/left")},
	    {QStringLiteral("Mouse:Right"), QStringLiteral("mouse/right")},
	    {QStringLiteral("Mouse:Middle"), QStringLiteral("mouse/middle")},
	    {QStringLiteral("Mouse:X1"), QStringLiteral("mouse/x1")},
	    {QStringLiteral("Mouse:X2"), QStringLiteral("mouse/x2")},
	};

	QString alias = named.value(binding);
	if (alias.isEmpty() && binding.size() == 1) {
		const QChar ch = binding.at(0).toLower();
		if (ch.isLetter() || ch.isDigit()) {
			alias = QStringLiteral("key/%1").arg(ch);
		}
	}
	if (alias.isEmpty() && binding.size() >= 2 && binding.size() <= 3 &&
	    (binding.at(0) == QLatin1Char('F') || binding.at(0) == QLatin1Char('f'))) {
		bool      ok = false;
		const int n  = binding.mid(1).toInt(&ok);
		if (ok && n >= 1 && n <= 12) {
			alias = QStringLiteral("key/f%1").arg(n);
		}
	}
	return InputIcon(alias);
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
		    tr("Press a key or mouse button.\nSpace, F1, F7, and F11 are reserved; Esc cancels."),
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
		if (event->key() == Qt::Key_Space || event->key() == Qt::Key_F1 ||
		    event->key() == Qt::Key_F7 || event->key() == Qt::Key_F11) {
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

QHash<QString, QString> ParseMapping(const QStringList& mapping) {
	QHash<QString, QString> result;
	for (const auto& entry: mapping) {
		if (entry.startsWith(QLatin1String(MOUSE_SENSITIVITY))) {
			continue;
		}
		const auto separator = entry.indexOf(QLatin1Char('='));
		if (separator > 0 && separator + 1 < entry.size()) {
			const auto binding = entry.mid(separator + 1);
			for (auto item = result.begin(); item != result.end();) {
				if (item.value().compare(binding, Qt::CaseInsensitive) == 0) {
					item = result.erase(item);
				} else {
					++item;
				}
			}
			result.insert(entry.left(separator), binding);
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

InputMappingDialog::InputMappingDialog(const QStringList& mapping, QWidget* parent)
    : QDialog(parent) {
	setWindowTitle(tr("Input Mapping"));
	resize(460, 650);

	auto* layout = new QVBoxLayout(this);
	layout->addWidget(
	    new QLabel(tr("Map keyboard or mouse buttons to DualSense controls.\n"
	                  "Press F7 in-game to toggle mouse movement on the right stick."),
	               this));

	const auto parsed = ParseMapping(mapping);
	m_custom_bindings = !parsed.isEmpty();

	auto* sensitivity_layout = new QHBoxLayout;
	sensitivity_layout->addWidget(new QLabel(tr("Mouse sensitivity"), this));
	m_sensitivity = new QDoubleSpinBox(this);
	m_sensitivity->setRange(0.1, 5.0);
	m_sensitivity->setSingleStep(0.1);
	m_sensitivity->setDecimals(1);
	m_sensitivity->setSuffix(QStringLiteral("x"));
	m_sensitivity->setValue(ParseMouseSensitivity(mapping));
	sensitivity_layout->addWidget(m_sensitivity);
	sensitivity_layout->addStretch();
	layout->addLayout(sensitivity_layout);

	m_bindings = new QTreeWidget(this);
	m_bindings->setColumnCount(2);
	m_bindings->setHeaderLabels({tr("DualSense control"), tr("Host input")});
	m_bindings->setRootIsDecorated(false);
	m_bindings->setSelectionMode(QAbstractItemView::SingleSelection);
	m_bindings->setIconSize(QSize(24, 24));
	m_bindings->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_bindings->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	layout->addWidget(m_bindings);

	for (const auto& control: PAD_CONTROLS) {
		auto* item = new QTreeWidgetItem(m_bindings);
		item->setText(0, tr(control.label));
		item->setIcon(0, InputIcon(QString::fromLatin1(control.icon)));
		item->setData(0, Qt::UserRole, QString::fromLatin1(control.id));
		SetBinding(item, m_custom_bindings ? parsed.value(QString::fromLatin1(control.id))
		                                   : QString::fromLatin1(control.default_binding));
	}
	m_bindings->setCurrentItem(m_bindings->topLevelItem(0));

	auto* controls  = new QHBoxLayout;
	m_change_button = new QPushButton(tr("Change..."), this);
	m_clear_button  = new QPushButton(tr("Clear"), this);
	auto* defaults  = new QPushButton(tr("Defaults"), this);
	controls->addWidget(m_change_button);
	controls->addWidget(m_clear_button);
	controls->addWidget(defaults);
	controls->addStretch();
	layout->addLayout(controls);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);

	connect(m_bindings, &QTreeWidget::itemDoubleClicked, this,
	        [this](QTreeWidgetItem*, int) { ChangeBinding(); });
	connect(m_bindings, &QTreeWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
	connect(m_change_button, &QPushButton::clicked, this, [this]() { ChangeBinding(); });
	connect(m_clear_button, &QPushButton::clicked, this, [this]() { ClearBinding(); });
	connect(defaults, &QPushButton::clicked, this, [this]() { RestoreDefaults(); });
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	UpdateButtons();
}

QStringList InputMappingDialog::Mapping() const {
	QStringList result;
	if (m_custom_bindings) {
		for (int index = 0; index < m_bindings->topLevelItemCount(); index++) {
			const auto* item    = m_bindings->topLevelItem(index);
			const auto  binding = item->data(BINDING_COLUMN, Qt::UserRole).toString();
			if (!binding.isEmpty()) {
				result.append(item->data(0, Qt::UserRole).toString() + QLatin1Char('=') + binding);
			}
		}
	}
	if (m_sensitivity->value() != DEFAULT_MOUSE_SENSITIVITY) {
		result.append(QLatin1String(MOUSE_SENSITIVITY) +
		              QString::number(m_sensitivity->value(), 'f', 1));
	}
	return result;
}

void InputMappingDialog::ChangeBinding() {
	auto* item = m_bindings->currentItem();
	if (item == nullptr) {
		return;
	}

	InputCaptureDialog dialog(this);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	for (int index = 0; index < m_bindings->topLevelItemCount(); index++) {
		auto* other = m_bindings->topLevelItem(index);
		if (other != item && other->data(BINDING_COLUMN, Qt::UserRole)
		                             .toString()
		                             .compare(dialog.Binding(), Qt::CaseInsensitive) == 0) {
			SetBinding(other, {});
		}
	}
	SetBinding(item, dialog.Binding());
	m_custom_bindings = true;
}

void InputMappingDialog::ClearBinding() {
	SetBinding(m_bindings->currentItem(), {});
	m_custom_bindings = true;
}

void InputMappingDialog::RestoreDefaults() {
	for (int index = 0; index < m_bindings->topLevelItemCount(); index++) {
		SetBinding(m_bindings->topLevelItem(index),
		           QString::fromLatin1(PAD_CONTROLS[index].default_binding));
	}
	m_sensitivity->setValue(DEFAULT_MOUSE_SENSITIVITY);
	m_custom_bindings = false;
}

void InputMappingDialog::SetBinding(QTreeWidgetItem* item, const QString& binding) {
	if (item == nullptr) {
		return;
	}
	item->setData(BINDING_COLUMN, Qt::UserRole, binding);
	item->setText(BINDING_COLUMN, binding.isEmpty() ? tr("None") : binding);
	item->setIcon(BINDING_COLUMN, HostInputIcon(binding));
	UpdateButtons();
}

void InputMappingDialog::UpdateButtons() {
	if (m_change_button == nullptr) {
		return;
	}
	const auto* item = m_bindings->currentItem();
	m_change_button->setEnabled(item != nullptr);
	m_clear_button->setEnabled(item != nullptr &&
	                           !item->data(BINDING_COLUMN, Qt::UserRole).toString().isEmpty());
}
