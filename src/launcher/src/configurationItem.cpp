#include "configurationItem.h"

#include "configuration.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QSize>
#include <QStringList>
#include <QStyle>
#include <QTreeWidget>
#include <QVersionNumber>
#include <QWheelEvent>
#include <QWidget>

namespace {

enum Column {
	NameColumn,
	SerialColumn,
	FirmwareVersionColumn,
	PathColumn,
	StatusColumn,
	CommentsColumn,
};

class NoWheelComboBox: public QComboBox {
public:
	explicit NoWheelComboBox(QWidget* parent = nullptr): QComboBox(parent) {}

protected:
	void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

QString GetStatusText(Configuration::GameStatus status) {
	switch (status) {
		case Configuration::GameStatus::Unknown: return QStringLiteral("Unknown");
		case Configuration::GameStatus::MainMenu: return QStringLiteral("Main menu");
		case Configuration::GameStatus::InGame: return QStringLiteral("In game");
		case Configuration::GameStatus::Logo: return QStringLiteral("Logo");
		case Configuration::GameStatus::DoesntBoot: return QStringLiteral("Doesn't boot");
	}

	return QStringLiteral("Unknown");
}

QString GetStatusColor(Configuration::GameStatus status) {
	switch (status) {
		case Configuration::GameStatus::InGame: return QStringLiteral("#2fb344");
		case Configuration::GameStatus::MainMenu: return QStringLiteral("#2f80ed");
		case Configuration::GameStatus::Logo: return QStringLiteral("#f2c94c");
		case Configuration::GameStatus::DoesntBoot: return QStringLiteral("#e55353");
		case Configuration::GameStatus::Unknown: return QStringLiteral("#8a8a8a");
	}

	return QStringLiteral("#8a8a8a");
}

void AddStatus(QComboBox* combo, Configuration::GameStatus status) {
	combo->addItem(GetStatusText(status), static_cast<int>(status));
}

void SetStatus(QComboBox* combo, Configuration::GameStatus status) {
	const int index = combo->findData(static_cast<int>(status));
	combo->setCurrentIndex(index >= 0 ? index : 0);
}

QIcon StandardIcon(QStyle::StandardPixmap icon) {
	return QApplication::style()->standardIcon(icon);
}

QString GetPathText(const Configuration& info) {
	return !info.game_path.isEmpty() ? info.game_path : info.basedir;
}

QString GetDisplayText(const Configuration& info) {
	QStringList lines({info.name});

	if (!info.title_id.isEmpty()) {
		lines.append(QStringLiteral("Serial ID: %1").arg(info.title_id));
	}
	if (!info.firmwareVer.isEmpty()) {
		lines.append(QStringLiteral("Firmware version: %1").arg(info.firmwareVer));
	}

	const auto path = GetPathText(info);
	if (!path.isEmpty()) {
		lines.append(QStringLiteral("Path: %1").arg(path));
	}
	if (!info.game_comment.isEmpty()) {
		lines.append(QStringLiteral("Comment: %1").arg(info.game_comment));
	}

	return lines.join(QLatin1Char('\n'));
}

QString GetSortText(const Configuration& info) {
	return QStringLiteral("%1\n%2").arg(info.name.toCaseFolded(), info.game_path.toCaseFolded());
}

void MakeTransparent(QWidget* widget) {
	widget->setAutoFillBackground(false);
	widget->setAttribute(Qt::WA_NoSystemBackground);
	widget->setAttribute(Qt::WA_TranslucentBackground);
}

} // namespace

ConfigurationItem::ConfigurationItem(std::unique_ptr<Configuration> info, QTreeWidget* parent)
    : QTreeWidgetItem(parent), m_info(std::move(info)) {
	setSizeHint(NameColumn, QSize(0, 72));

	m_status_widget = new QWidget(parent);
	MakeTransparent(m_status_widget);
	auto* layout = new QHBoxLayout(m_status_widget);
	layout->setContentsMargins(4, 0, 4, 0);
	layout->setSpacing(6);

	m_status_indicator = new QLabel(m_status_widget);
	m_status_indicator->setFixedSize(QSize(12, 12));
	layout->addWidget(m_status_indicator);

	m_status_combo = new NoWheelComboBox(m_status_widget);
	MakeTransparent(m_status_combo);
	AddStatus(m_status_combo, Configuration::GameStatus::Unknown);
	AddStatus(m_status_combo, Configuration::GameStatus::MainMenu);
	AddStatus(m_status_combo, Configuration::GameStatus::InGame);
	AddStatus(m_status_combo, Configuration::GameStatus::Logo);
	AddStatus(m_status_combo, Configuration::GameStatus::DoesntBoot);
	m_status_combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	m_status_combo->setFixedWidth(125);
	m_status_combo->setStyleSheet(QStringLiteral(
	    "QComboBox { background: transparent; border: 0; color: #eef4ff; padding-left: 2px; }"
	    "QComboBox:focus { background: rgba(255,255,255,35); border-radius: 3px; }"
	    "QComboBox::drop-down { border: 0; }"
	    "QComboBox QAbstractItemView { background: #181c24; color: #eef4ff; "
	    "selection-background-color: #267bd8; }"));
	layout->addWidget(m_status_combo);
	layout->addStretch(1);

	parent->setItemWidget(this, StatusColumn, m_status_widget);

	m_comment_edit = new QLineEdit(parent);
	MakeTransparent(m_comment_edit);
	m_comment_edit->setClearButtonEnabled(true);
	m_comment_edit->setFrame(false);
	m_comment_edit->setStyleSheet(QStringLiteral(
	    "QLineEdit { background: transparent; border: 0; color: #eef4ff; padding-left: 2px; "
	    "selection-background-color: #267bd8; }"
	    "QLineEdit:focus { background: rgba(255,255,255,35); "
	    "border: 1px solid rgba(255,255,255,90); border-radius: 3px; }"));
	parent->setItemWidget(this, CommentsColumn, m_comment_edit);

	Update();
	SetRunning(false);
}

ConfigurationItem::~ConfigurationItem() = default;

void ConfigurationItem::Update() {
	const auto display_text = GetDisplayText(*m_info);
	const auto path         = GetPathText(*m_info);

	setText(NameColumn, m_info->name);
	setText(SerialColumn, m_info->title_id);
	setText(FirmwareVersionColumn,
	        m_info->firmwareVer.isEmpty() ? QStringLiteral("\u2014") : m_info->firmwareVer);
	setText(PathColumn, path);
	setText(StatusColumn, {});
	setText(CommentsColumn, {});
	for (int column = NameColumn; column <= CommentsColumn; column++) {
		setToolTip(column, display_text);
	}
	SetStatus(m_status_combo, m_info->game_status);
	if (m_comment_edit->text() != m_info->game_comment) {
		m_comment_edit->setText(m_info->game_comment);
	}

	UpdateIcon();
	UpdateStatusIndicator();
}

bool ConfigurationItem::operator<(const QTreeWidgetItem& other) const {
	const auto* other_item = dynamic_cast<const ConfigurationItem*>(&other);
	if (other_item == nullptr) {
		return QTreeWidgetItem::operator<(other);
	}

	const int column = treeWidget() != nullptr ? treeWidget()->sortColumn() : NameColumn;
	switch (column) {
		case NameColumn: return GetSortText(*m_info) < GetSortText(*other_item->m_info);
		case StatusColumn:
			return GetStatusText(m_info->game_status) <
			       GetStatusText(other_item->m_info->game_status);
		case FirmwareVersionColumn: {
			const auto& version       = m_info->firmwareVer;
			const auto& other_version = other_item->m_info->firmwareVer;
			if (version.isEmpty() || other_version.isEmpty()) {
				return version.isEmpty() && !other_version.isEmpty();
			}
			return QVersionNumber::compare(QVersionNumber::fromString(version),
			                               QVersionNumber::fromString(other_version)) < 0;
		}
		case CommentsColumn:
			return m_info->game_comment.toCaseFolded() <
			       other_item->m_info->game_comment.toCaseFolded();
		default: return text(column).toCaseFolded() < other.text(column).toCaseFolded();
	}
}

void ConfigurationItem::SetRunning(bool state) {
	m_running = state;

	QFont f = font(NameColumn);
	f.setBold(state);
	for (int column = NameColumn; column <= CommentsColumn; column++) {
		setFont(column, f);
	}

	UpdateIcon();
}

void ConfigurationItem::SetCompatibilityEditable(bool editable) {
	m_status_combo->setEnabled(editable);
	m_comment_edit->setReadOnly(!editable);
	m_comment_edit->setClearButtonEnabled(editable);
}

void ConfigurationItem::UpdateIcon() {
	const QString icon_file = QDir(m_info->basedir).filePath(QStringLiteral("sce_sys/icon0.png"));
	if (QFileInfo::exists(icon_file)) {
		setIcon(NameColumn, QIcon(icon_file));
		return;
	}

	if (m_running) {
		setIcon(NameColumn, StandardIcon(QStyle::SP_MediaPlay));
	} else if (m_info->custom_settings) {
		setIcon(NameColumn, StandardIcon(QStyle::SP_FileIcon));
	} else {
		setIcon(NameColumn, StandardIcon(QStyle::SP_ComputerIcon));
	}
}

void ConfigurationItem::UpdateStatusIndicator() {
	m_status_indicator->setStyleSheet(
	    QStringLiteral(
	        "background-color: %1; border: 1px solid rgba(255, 255, 255, 90); border-radius: 6px;")
	        .arg(GetStatusColor(m_info->game_status)));
	m_status_indicator->setToolTip(GetStatusText(m_info->game_status));
}
