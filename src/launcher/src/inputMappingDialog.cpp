#include "inputMappingDialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {

constexpr int  PRIMARY_COLUMN            = 1;
constexpr int  FALLBACK_COLUMN           = 2;
constexpr auto DEFAULT_MOUSE_SENSITIVITY = 1.0;
constexpr char MOUSE_SENSITIVITY[]       = "MouseSensitivity=";

struct PadControl {
	const char* id;
	const char* label;
	const char* primary_binding;
	const char* fallback_binding;
};

constexpr PadControl PAD_CONTROLS[] = {
    {"Up", "D-pad Up", "Up", "1"},
    {"Left", "D-pad Left", "Left", "2"},
    {"Down", "D-pad Down", "Down", "3"},
    {"Right", "D-pad Right", "Right", "4"},
    {"LeftStickUp", "Left stick Up", "W", ""},
    {"LeftStickDown", "Left stick Down", "S", ""},
    {"LeftStickLeft", "Left stick Left", "A", ""},
    {"LeftStickRight", "Left stick Right", "D", ""},
    {"RightStickUp", "Right stick Up", "", "Keypad 8"},
    {"RightStickDown", "Right stick Down", "", "Keypad 2"},
    {"RightStickLeft", "Right stick Left", "", "Keypad 4"},
    {"RightStickRight", "Right stick Right", "", "Keypad 6"},
    {"Triangle", "Triangle", "I", "C"},
    {"Circle", "Circle", "L", "X"},
    {"Cross", "Cross", "K", "Left Shift"},
    {"Square", "Square", "J", "Z"},
    {"L1", "L1", "Q", ""},
    {"R1", "R1", "E", ""},
    {"L2", "L2", "Mouse:Right", "R"},
    {"R2", "R2", "Mouse:Left", "F"},
    {"L3", "L3", "T", ""},
    {"R3", "R3", "V", ""},
    {"Options", "Options", "Return", ""},
    {"TouchPad", "Touch pad press", "Backspace", ""},
    {"TouchPadRight", "Touch pad secondary", "", ""},
    {"AnalogModifier", "Analog modifier", "Tab", ""},
    {"AnalogStepDown", "Analog step 25%", "Minus", ""},
    {"AnalogStepMiddle", "Analog step 50%", "Equals", ""},
    {"AnalogStepUp", "Analog step 75%", "Plus", ""},
    {"AnalogLock", "Analog adjustment lock", "CapsLock", ""},
    {"Gyro", "Gyroscope", "7", "/"},
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
		case Qt::Key_Return:
		case Qt::Key_Enter: return QStringLiteral("Return");
		case Qt::Key_Backspace: return QStringLiteral("Backspace");
		case Qt::Key_Tab: return QStringLiteral("Tab");
		case Qt::Key_Minus: return QStringLiteral("Minus");
		case Qt::Key_Equal:
		case Qt::Key_Plus: return QStringLiteral("Equals");
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
		case Qt::Key_Slash: return QStringLiteral("/");
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
		m_label = new QLabel(tr("Press a key, mouse button, or wheel direction.\nSpace, F1, F7, "
		                        "and F11 are reserved; Esc cancels."),
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

	void wheelEvent(QWheelEvent* event) override {
		const int delta = event->angleDelta().y();
		if (delta == 0) {
			m_label->setText(tr("That wheel event is not available."));
			return;
		}
		m_binding = delta > 0 ? QStringLiteral("Mouse:WheelUp") : QStringLiteral("Mouse:WheelDown");
		accept();
	}

private:
	QLabel* m_label = nullptr;
	QString m_binding;
};

QHash<QString, QStringList> ParseMapping(const QStringList& mapping) {
	QHash<QString, QStringList> result;
	for (const auto& entry: mapping) {
		if (entry.startsWith(QLatin1String(MOUSE_SENSITIVITY))) {
			continue;
		}
		const auto separator = entry.indexOf(QLatin1Char('='));
		if (separator > 0 && separator + 1 < entry.size()) {
			const auto binding = entry.mid(separator + 1);
			result[entry.left(separator)].append(binding);
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

	auto* explanation = new QLabel(
	    tr("Analog adjustment: hold the input and press Tab with Minus for 25%, Equals for 50%, "
	       "or Plus for 75%. Release Tab while holding the input for 100%. "
	       "You can also use Tab plus mouse wheel down/up for continuous adjustment.\n\n"
	       "Caps Lock is the analog adjustment lock. Press it once to keep the current trigger, "
	       "microphone, or gyro value after releasing the adjustment keys; press it again to reset "
	       "all locked analog values. Caps Lock is captured by its physical scancode and can be "
	       "changed in the binding table."),
	    this);
	explanation->setWordWrap(true);
	explanation->setTextFormat(Qt::PlainText);
	layout->addWidget(explanation);

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
	m_bindings->setColumnCount(3);
	m_bindings->setHeaderLabels(
	    {tr("DualSense Control"), tr("Host Input"), tr("Host Input (Fallback)")});
	m_bindings->setRootIsDecorated(false);
	m_bindings->setSelectionMode(QAbstractItemView::SingleSelection);
	m_bindings->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_bindings->header()->setSectionResizeMode(PRIMARY_COLUMN, QHeaderView::Stretch);
	m_bindings->header()->setSectionResizeMode(FALLBACK_COLUMN, QHeaderView::Stretch);
	layout->addWidget(m_bindings);

	for (const auto& control: PAD_CONTROLS) {
		QStringList values = parsed.value(QString::fromLatin1(control.id));
		if (values.isEmpty()) {
			values << QString::fromLatin1(control.primary_binding)
			       << QString::fromLatin1(control.fallback_binding);
		} else if (values.size() == 1) {
			values << QString::fromLatin1(control.fallback_binding);
		}
		auto* item = new QTreeWidgetItem(m_bindings);
		item->setText(0, tr(control.label));
		item->setData(0, Qt::UserRole, QString::fromLatin1(control.id));
		SetBinding(item, PRIMARY_COLUMN, values.value(0));
		SetBinding(item, FALLBACK_COLUMN, values.value(1));
		if (values.value(0).isEmpty() && QString::fromLatin1(control.id).startsWith("RightStick")) {
			item->setText(PRIMARY_COLUMN, tr("Mouse movement (F7)"));
		}
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
	for (int index = 0; index < m_bindings->topLevelItemCount(); index++) {
		const auto* item = m_bindings->topLevelItem(index);
		for (const int column: {PRIMARY_COLUMN, FALLBACK_COLUMN}) {
			const auto binding = item->data(column, Qt::UserRole).toString();
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
		for (const int column: {PRIMARY_COLUMN, FALLBACK_COLUMN}) {
			if (other != item && other->data(column, Qt::UserRole)
			                             .toString()
			                             .compare(dialog.Binding(), Qt::CaseInsensitive) == 0) {
				SetBinding(other, column, {});
			}
		}
	}
	SetBinding(item, m_bindings->currentColumn(), dialog.Binding());
	m_custom_bindings = true;
}

void InputMappingDialog::ClearBinding() {
	SetBinding(m_bindings->currentItem(), m_bindings->currentColumn(), {});
	m_custom_bindings = true;
}

void InputMappingDialog::RestoreDefaults() {
	for (int index = 0; index < m_bindings->topLevelItemCount(); index++) {
		const auto& control = PAD_CONTROLS[index];
		SetBinding(m_bindings->topLevelItem(index), PRIMARY_COLUMN,
		           QString::fromLatin1(control.primary_binding));
		SetBinding(m_bindings->topLevelItem(index), FALLBACK_COLUMN,
		           QString::fromLatin1(control.fallback_binding));
	}
	m_sensitivity->setValue(DEFAULT_MOUSE_SENSITIVITY);
	m_custom_bindings = false;
}

void InputMappingDialog::SetBinding(QTreeWidgetItem* item, int column, const QString& binding) {
	if (item == nullptr) {
		return;
	}
	item->setData(column, Qt::UserRole, binding);
	item->setText(column, binding.isEmpty() ? tr("None") : binding);
	UpdateButtons();
}

void InputMappingDialog::UpdateButtons() {
	if (m_change_button == nullptr) {
		return;
	}
	const auto* item = m_bindings->currentItem();
	m_change_button->setEnabled(item != nullptr && m_bindings->currentColumn() > 0);
	m_clear_button->setEnabled(
	    item != nullptr && m_bindings->currentColumn() > 0 &&
	    !item->data(m_bindings->currentColumn(), Qt::UserRole).toString().isEmpty());
}
