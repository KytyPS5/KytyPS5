#include "additionalContentDialog.h"

#include "common/additionalContent.h"
#include "configuration.h"
#include "gameContent.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

AdditionalContentDialog::AdditionalContentDialog(const Configuration& game, const QString& folder,
                                                 QWidget* parent)
    : QDialog(parent), m_title_id(game.title_id.trimmed().toUpper()), m_folder(folder) {
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

	m_status = new QLabel(this);
	m_status->setTextFormat(Qt::PlainText);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	auto* refresh = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
	m_open        = buttons->addButton(tr("Open folder"), QDialogButtonBox::ActionRole);
	layout->addWidget(buttons);
	connect(refresh, &QPushButton::clicked, this, &AdditionalContentDialog::Scan);
	connect(m_open, &QPushButton::clicked, this,
	        [this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(m_folder)); });
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

	if (m_folder.isEmpty()) {
		m_status->setText(tr("No add-ons folder is set. Open Global settings and set the "
		                     "Add-ons folder to the location of your extracted add-on packages."));
	} else if (!folder_exists) {
		m_status->setText(tr("The add-ons folder does not exist:\n%1\nCheck the path in Global "
		                     "settings.")
		                      .arg(QDir::toNativeSeparators(m_folder)));
	} else if (m_list->count() == 0) {
		m_status->setText(
		    tr("No add-ons found for this game in:\n%1").arg(QDir::toNativeSeparators(m_folder)));
	} else {
		m_status->setText(tr("%n installed add-on(s)", nullptr, m_list->count()));
	}
}
