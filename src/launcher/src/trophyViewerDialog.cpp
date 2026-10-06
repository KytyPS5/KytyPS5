#include "trophyViewerDialog.h"

#include "common/archive.h"
#include "common/trophies.h"
#include "configuration.h"
#include "gameContent.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QMessageBox>
#include <QPixmap>
#include <QRegularExpression>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

static const QRegularExpression TrophyFilePattern(QStringLiteral("^trophy(\\d+)\\.ucp$"));

static QStringList FindTrophyFiles(const Configuration* info) {
	if (info == nullptr || info->basedir.isEmpty()) {
		return {};
	}
	QStringList files;
	for (const auto& file: GameContent::ListFiles(
	         info->basedir, QString::fromLatin1(Common::Trophies::PackageDirectory))) {
		if (TrophyFilePattern.match(QFileInfo(file).fileName()).hasMatch()) {
			files.append(file);
		}
	}
	files.sort();
	return files;
}

static QString GradeToText(int grade) {
	switch (grade) {
		case 1: return QObject::tr("Platinum");
		case 2: return QObject::tr("Gold");
		case 3: return QObject::tr("Silver");
		case 4: return QObject::tr("Bronze");
		default: return {};
	}
}

static void PrepareTable(QTableWidget* table) {
	table->setHorizontalHeaderLabels({QObject::tr("Unlocked"), QObject::tr("Trophy"),
	                                  QObject::tr("Name"), QObject::tr("Description")});
	table->setAlternatingRowColors(true);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setIconSize(QSize(96, 96));
	table->setShowGrid(false);
	table->setWordWrap(true);
	table->verticalHeader()->setVisible(false);
	table->verticalHeader()->setDefaultSectionSize(112);
	table->horizontalHeader()->setStretchLastSection(true);
	table->horizontalHeader()->setHighlightSections(false);
	table->setColumnWidth(0, 100);
	table->setColumnWidth(1, 132);
	table->setColumnWidth(2, 260);
	table->setColumnWidth(3, 480);
}

static QString TrophyTooltip(const Common::Trophies::Trophy& trophy) {
	QStringList lines {QString::fromStdString(trophy.name),
	                   QString::fromStdString(trophy.description),
	                   QObject::tr("Grade: %1").arg(GradeToText(trophy.grade))};
	if (trophy.hidden) {
		lines.append(QObject::tr("Hidden trophy"));
	}
	if (trophy.has_reward && !trophy.reward.empty()) {
		lines.append(QObject::tr("Reward: %1").arg(QString::fromStdString(trophy.reward)));
	}
	return lines.join(QLatin1Char('\n'));
}

} // namespace

TrophyViewerDialog::TrophyViewerDialog(QWidget* parent): QDialog(parent) {
	setWindowTitle(tr("Trophy Viewer"));
	resize(1000, 640);

	auto* layout = new QVBoxLayout(this);

	m_tabs = new QTabWidget(this);
	layout->addWidget(m_tabs, 1);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
}

bool TrophyViewerDialog::HasTrophyData(const Configuration* info) {
	return !FindTrophyFiles(info).isEmpty();
}

void TrophyViewerDialog::ShowForGame(const Configuration* info, const QString& runtime_directory,
                                     QWidget* parent) {
	if (info == nullptr) {
		return;
	}

	TrophyViewerDialog dlg(parent);
	if (!info->name.isEmpty()) {
		dlg.setWindowTitle(tr("Trophy Viewer - %1").arg(info->name));
	}

	QString error;
	if (!dlg.LoadGame(*info, runtime_directory, error)) {
		QMessageBox::warning(parent, tr("Trophy Viewer"), error);
		return;
	}

	dlg.exec();
}

bool TrophyViewerDialog::LoadGame(const Configuration& info, const QString& runtime_directory,
                                  QString& error) {
	const auto reader       = Common::OpenArchive(GameContent::ToPath(info.basedir));
	const auto trophy_files = FindTrophyFiles(&info);
	if (trophy_files.isEmpty()) {
		error = tr("No trophy package found in %1.")
		            .arg(QString::fromLatin1(Common::Trophies::PackageDirectory));
		return false;
	}
	QStringList errors;
	for (const auto& file: trophy_files) {
		const auto package =
		    Common::Trophies::LoadPackage(GameContent::ToPath(file), info.console_language);
		if (package.trophies.empty()) {
			errors.append(tr("Could not read trophy package %1.").arg(QFileInfo(file).fileName()));
			continue;
		}
		const auto label = TrophyFilePattern.match(QFileInfo(file).fileName()).captured(1).toUInt();
		const auto unlocked = Common::Trophies::LoadUnlocks(
		    Common::Trophies::UnlocksPath(GameContent::ToPath(runtime_directory),
		                                  info.title_id.toStdString(), info.user_id, label));
		auto* table = new QTableWidget(static_cast<int>(package.trophies.size()), 4, m_tabs);
		PrepareTable(table);
		int row_index = 0;
		for (const auto& [id, trophy]: package.trophies) {
			const bool trophy_unlocked = unlocked.contains(id);
			auto* status = new QTableWidgetItem(trophy_unlocked ? tr("UNLOCKED") : tr("LOCKED"));
			status->setTextAlignment(Qt::AlignCenter);
			auto status_font = status->font();
			status_font.setBold(true);
			status->setFont(status_font);
			table->setItem(row_index, 0, status);

			auto* icon_item = new QTableWidgetItem;
			icon_item->setTextAlignment(Qt::AlignCenter);
			QPixmap icon;
			if (icon.loadFromData(reinterpret_cast<const uchar*>(trophy.icon_png.data()),
			                      static_cast<uint>(trophy.icon_png.size()))) {
				if (!trophy_unlocked) {
					icon = QPixmap::fromImage(
					    icon.toImage().convertToFormat(QImage::Format_Grayscale8));
				}
				icon_item->setIcon(QIcon(icon));
			}
			table->setItem(row_index, 1, icon_item);

			const auto name_text = trophy.name.empty() ? (trophy.hidden ? tr("Hidden Trophy")
			                                                            : tr("Trophy %1").arg(id))
			                                           : QString::fromStdString(trophy.name);
			auto*      name      = new QTableWidgetItem(name_text);
			auto       font      = name->font();
			font.setBold(true);
			name->setFont(font);
			name->setToolTip(TrophyTooltip(trophy));
			auto* detail = new QTableWidgetItem(QString::fromStdString(trophy.description));
			detail->setToolTip(name->toolTip());
			table->setItem(row_index, 2, name);
			table->setItem(row_index, 3, detail);
			++row_index;
		}
		m_tabs->addTab(table, tr("Trophy %1").arg(label));
	}
	if (m_tabs->count() == 0) {
		error = errors.join(QLatin1Char('\n'));
		return false;
	}
	return true;
}
