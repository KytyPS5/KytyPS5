#include "additionalContentDialog.h"

#include "common/additionalContent.h"
#include "configuration.h"
#include "gameContent.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

AdditionalContentDialog::AdditionalContentDialog(const Configuration& game, const QString& folder,
                                                 QStringList                             entries,
                                                 std::function<void(const QStringList&)> save,
                                                 QWidget*                                parent)
    : QDialog(parent), m_title_id(game.title_id.trimmed().toUpper()), m_folder(folder),
      m_entries(std::move(entries)), m_save(std::move(save)) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("Additional Content - %1").arg(game.name));
	resize(720, 480);

	QPixmap icon;
	icon.loadFromData(GameContent::ReadFile(game.basedir, QStringLiteral("sce_sys/icon0.png"),
	                                        GameContent::MaxImageSize));
	m_game_icon = icon.isNull() ? style()->standardIcon(QStyle::SP_FileIcon) : QIcon(icon);

	auto* layout = new QVBoxLayout(this);
	auto* name   = new QLabel(tr("%1 · %2").arg(game.name, m_title_id), this);
	name->setTextFormat(Qt::PlainText);
	name->setWordWrap(true);
	layout->addWidget(name);

	m_list = new QListWidget(this);
	m_list->setIconSize(QSize(64, 64));
	m_list->setSpacing(4);
	m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	layout->addWidget(m_list, 1);

	auto* add_layout = new QHBoxLayout;
	m_label_edit     = new QLineEdit(this);
	m_label_edit->setPlaceholderText(tr("Label or content ID"));
	m_dir_edit = new QLineEdit(this);
	m_dir_edit->setPlaceholderText(tr("Add-on data folder (empty for a license only)"));
	auto* browse = new QPushButton(tr("Browse..."), this);
	auto* add    = new QPushButton(tr("Add"), this);
	add->setDefault(true);
	add_layout->addWidget(m_label_edit, 1);
	add_layout->addWidget(m_dir_edit, 2);
	add_layout->addWidget(browse);
	add_layout->addWidget(add);
	layout->addLayout(add_layout);
	connect(browse, &QPushButton::clicked, this, [this]() {
		const auto dir =
		    QFileDialog::getExistingDirectory(this, tr("Add-on data folder"), m_dir_edit->text());
		if (!dir.isEmpty()) {
			m_dir_edit->setText(QDir::toNativeSeparators(dir));
		}
	});
	connect(add, &QPushButton::clicked, this, &AdditionalContentDialog::Add);

	m_status = new QLabel(this);
	m_status->setTextFormat(Qt::PlainText);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	auto* refresh = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
	m_open        = buttons->addButton(tr("Open folder"), QDialogButtonBox::ActionRole);
	m_remove      = buttons->addButton(tr("Remove"), QDialogButtonBox::ActionRole);
	layout->addWidget(buttons);
	connect(refresh, &QPushButton::clicked, this, &AdditionalContentDialog::Scan);
	connect(m_open, &QPushButton::clicked, this,
	        [this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(m_folder)); });
	connect(m_remove, &QPushButton::clicked, this, &AdditionalContentDialog::Remove);
	connect(m_list, &QListWidget::currentItemChanged, this, [this](const QListWidgetItem* item) {
		m_remove->setEnabled(item != nullptr && item->data(Qt::UserRole).isValid());
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

	Scan();
}

void AdditionalContentDialog::Scan() {
	m_list->clear();
	const bool folder_exists = !m_folder.isEmpty() && QDir(m_folder).exists();
	m_open->setEnabled(folder_exists);

	for (const auto& entry:
	     Common::FindAddContent(GameContent::ToPath(m_folder), m_title_id.toStdString())) {
		QPixmap icon;
		icon.loadFromData(GameContent::ReadFile(GameContent::FromPath(entry.mount_dir),
		                                        QStringLiteral("sce_sys/icon0.png"),
		                                        GameContent::MaxImageSize));
		const auto title =
		    QString::fromStdString(entry.title.empty() ? entry.entitlement_label : entry.title);
		auto* item = new QListWidgetItem(icon.isNull() ? m_game_icon : QIcon(icon),
		                                 tr("%1\nInstalled").arg(title), m_list);
		item->setToolTip(QString::fromStdString(entry.content_id));
	}
	for (const auto& entry: m_entries) {
		const auto label = entry.section('=', 0, 0);
		const auto dir   = entry.section('=', 1);
		auto       text  = tr("%1\nLicense only").arg(label);
		if (!dir.isEmpty()) {
			text = (QDir(dir).exists() ? tr("%1\nData: %2") : tr("%1\nData folder not found: %2"))
			           .arg(label, QDir::toNativeSeparators(dir));
		}
		auto* item = new QListWidgetItem(m_game_icon, text, m_list);
		item->setData(Qt::UserRole, entry);
	}
	m_remove->setEnabled(false);

	if (m_list->count() != 0) {
		m_status->setText(tr("%n add-on(s)", nullptr, m_list->count()));
	} else if (m_folder.isEmpty()) {
		m_status->setText(tr("No add-ons folder is set. Open Global settings and set the "
		                     "Add-ons folder to the location of your extracted add-on packages."));
	} else if (!folder_exists) {
		m_status->setText(tr("The add-ons folder does not exist:\n%1\nCheck the path in Global "
		                     "settings.")
		                      .arg(QDir::toNativeSeparators(m_folder)));
	} else {
		m_status->setText(
		    tr("No add-ons found for this game in:\n%1").arg(QDir::toNativeSeparators(m_folder)));
	}
}

void AdditionalContentDialog::Add() {
	// A content ID ends with its entitlement label.
	const auto label = m_label_edit->text().trimmed().section('-', -1);
	const auto dir   = m_dir_edit->text().trimmed();
	const auto entry = dir.isEmpty() ? label : label + '=' + QDir(dir).absolutePath();
	if (!Common::ParseAddContent(entry.toStdString())) {
		m_status->setText(tr("Enter a 16-character entitlement label (A-Z, 0-9) and an existing "
		                     "data folder, or no folder for a license only."));
		return;
	}
	m_entries.removeIf([&](const QString& e) { return e.section('=', 0, 0) == label; });
	m_entries.append(entry);
	m_save(m_entries);
	m_label_edit->clear();
	m_dir_edit->clear();
	Scan();
}

void AdditionalContentDialog::Remove() {
	m_entries.removeOne(m_list->currentItem()->data(Qt::UserRole).toString());
	m_save(m_entries);
	Scan();
}
