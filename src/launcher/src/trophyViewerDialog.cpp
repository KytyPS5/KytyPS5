#include "trophyViewerDialog.h"

#include "common/archive.h"
#include "common/trophies.h"
#include "configuration.h"
#include "gameContent.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProgressBar>
#include <QRegularExpression>
#include <QSize>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
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

namespace {

using Common::Trophies::Progress;

Common::Trophies::UnlockData LoadUnlocks(const Configuration& info,
                                         const QString& runtime_directory, const QString& file) {
	const auto label = TrophyFilePattern.match(QFileInfo(file).fileName()).captured(1).toUInt();
	return Common::Trophies::LoadUnlockData(Common::Trophies::UnlocksPath(
	    GameContent::ToPath(runtime_directory), info.title_id.toStdString(), info.user_id, label));
}

Progress GetGameProgress(const Configuration* info, const QString& runtime_directory) {
	Progress result;
	if (info == nullptr) {
		return result;
	}
	const auto reader = Common::OpenArchive(GameContent::ToPath(info->basedir));
	for (const auto& file: FindTrophyFiles(info)) {
		const auto package =
		    Common::Trophies::LoadPackage(GameContent::ToPath(file), info->console_language);
		const auto progress =
		    Common::Trophies::GetProgress(package, LoadUnlocks(*info, runtime_directory, file));
		result.total += progress.total;
		result.earned += progress.earned;
		for (size_t grade = 1; grade < result.total_grade.size(); ++grade) {
			result.total_grade[grade] += progress.total_grade[grade];
			result.earned_grade[grade] += progress.earned_grade[grade];
		}
	}
	return result;
}

QPixmap MakeCupIcon(const QColor& color, int size) {
	QPixmap pixmap(size, size);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	const double u = size / 20.0;
	painter.setPen(Qt::NoPen);
	painter.setBrush(color);
	QPainterPath cup;
	cup.moveTo(5 * u, 2 * u);
	cup.lineTo(15 * u, 2 * u);
	cup.lineTo(14 * u, 9 * u);
	cup.quadTo(10 * u, 13 * u, 6 * u, 9 * u);
	cup.closeSubpath();
	painter.drawPath(cup);
	painter.setPen(QPen(color, 1.6 * u));
	painter.setBrush(Qt::NoBrush);
	painter.drawArc(QRectF(1.5 * u, 3 * u, 5 * u, 5 * u), 90 * 16, 180 * 16);
	painter.drawArc(QRectF(13.5 * u, 3 * u, 5 * u, 5 * u), 90 * 16, -180 * 16);
	painter.drawLine(QPointF(10 * u, 12 * u), QPointF(10 * u, 16 * u));
	painter.setPen(Qt::NoPen);
	painter.setBrush(color);
	painter.drawRoundedRect(QRectF(6 * u, 16 * u, 8 * u, 2.5 * u), u, u);
	return pixmap;
}

QColor GradeColor(int grade) {
	switch (grade) {
		case 1: return QColor(0x7fc8ff);
		case 2: return QColor(0xf5c542);
		case 3: return QColor(0xc4cad2);
		default: return QColor(0xcd7f4f);
	}
}

QWidget* MakeGradeCount(QWidget* parent, int grade, int value, int icon_size, int font_size) {
	auto* box = new QWidget(parent);
	auto* row = new QHBoxLayout(box);
	row->setContentsMargins(0, 0, 0, 0);
	auto* icon = new QLabel(box);
	icon->setPixmap(MakeCupIcon(GradeColor(grade), icon_size));
	auto* text = new QLabel(QString::number(value), box);
	text->setStyleSheet(QStringLiteral("font-size: %1px;").arg(font_size));
	row->addWidget(icon);
	row->addWidget(text);
	return box;
}

QPixmap LoadGameArt(const Configuration* game, int height) {
	QPixmap    art;
	const auto data = GameContent::ReadFile(game->basedir, QStringLiteral("sce_sys/icon0.png"),
	                                        GameContent::MaxImageSize);
	if (!data.isEmpty() && art.loadFromData(data)) {
		return art.scaledToHeight(height, Qt::SmoothTransformation);
	}
	return {};
}

} // namespace

void TrophyViewerDialog::ShowOverview(const std::vector<const Configuration*>& games,
                                      const QString& runtime_directory, QWidget* parent) {
	QDialog dialog(parent);
	dialog.setWindowTitle(QObject::tr("Trophies"));
	dialog.resize(820, 640);
	dialog.setStyleSheet(QStringLiteral(
	    "QDialog { background: palette(window); }"
	    "QLabel { color: palette(text); }"
	    "QListWidget { background: transparent; border: none; outline: none; }"
	    "QListWidget::item { border-bottom: 1px solid palette(mid); padding: 4px; }"
	    "QListWidget::item:selected, QListWidget::item:hover { background: palette(midlight); "
	    "border: 1px solid palette(highlight); }"
	    "QProgressBar { background: palette(mid); border: none; height: 4px; }"
	    "QProgressBar::chunk { background: palette(highlight); }"));
	auto* layout = new QVBoxLayout(&dialog);

	struct GameProgress {
		const Configuration* game;
		Progress             progress;
	};
	std::vector<GameProgress> trophy_games;
	Progress                  totals;
	for (const auto* game: games) {
		const auto progress = GetGameProgress(game, runtime_directory);
		if (progress.total == 0) {
			continue;
		}
		trophy_games.push_back({game, progress});
		totals.earned += progress.earned;
		for (size_t grade = 1; grade < totals.earned_grade.size(); ++grade) {
			totals.earned_grade[grade] += progress.earned_grade[grade];
		}
	}

	auto* header = new QHBoxLayout;
	auto* title  = new QLabel(QObject::tr("Trophies"), &dialog);
	title->setStyleSheet(QStringLiteral("font-size: 30px;"));
	header->addWidget(title);
	header->addStretch(1);
	auto* total = new QLabel(QObject::tr("Total %1").arg(totals.earned), &dialog);
	total->setAlignment(Qt::AlignCenter);
	total->setStyleSheet(QStringLiteral("font-size: 16px;"));
	header->addWidget(total);
	for (int grade = 1; grade <= 4; ++grade) {
		header->addWidget(MakeGradeCount(&dialog, grade, totals.earned_grade[grade], 32, 18));
	}
	layout->addLayout(header);

	auto* list = new QListWidget(&dialog);
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	layout->addWidget(list, 1);

	for (size_t index = 0; index < trophy_games.size(); ++index) {
		const auto& [game, progress] = trophy_games[index];
		auto* item                   = new QListWidgetItem(list);
		item->setSizeHint(QSize(760, 112));
		item->setData(Qt::UserRole, static_cast<int>(index));

		auto* row_widget = new QWidget(list);
		row_widget->setAttribute(Qt::WA_TransparentForMouseEvents);
		auto* row = new QHBoxLayout(row_widget);
		auto* art = new QLabel(row_widget);
		art->setFixedSize(80, 80);
		art->setAlignment(Qt::AlignCenter);
		const auto pixmap = LoadGameArt(game, 80);
		if (!pixmap.isNull()) {
			art->setPixmap(pixmap);
		} else {
			art->setStyleSheet(QStringLiteral("background: palette(mid);"));
		}
		row->addWidget(art);
		const auto name = !game->name.isEmpty() ? game->name : game->title_id;
		auto* text = new QLabel(QStringLiteral("%1\n%2").arg(name, game->title_id), row_widget);
		text->setStyleSheet(QStringLiteral("font-size: 18px;"));
		row->addWidget(text, 1);

		auto* percent_box = new QVBoxLayout;
		percent_box->setSpacing(2);
		percent_box->setContentsMargins(0, 0, 0, 0);
		auto* percent = new QLabel(QStringLiteral("%1%").arg(progress.Percentage()), row_widget);
		percent->setAlignment(Qt::AlignRight);
		percent->setStyleSheet(QStringLiteral("font-size: 24px;"));
		percent->setMinimumHeight(34);
		auto* earned = new QLabel(
		    QObject::tr("Earned %1/%2").arg(progress.earned).arg(progress.total), row_widget);
		earned->setAlignment(Qt::AlignRight);
		earned->setMinimumHeight(18);
		auto* bar = new QProgressBar(row_widget);
		bar->setRange(0, 100);
		bar->setValue(progress.Percentage());
		bar->setTextVisible(false);
		bar->setFixedWidth(130);
		percent_box->addWidget(percent);
		percent_box->addWidget(earned);
		percent_box->addWidget(bar);
		row->addLayout(percent_box);
		for (int grade = 1; grade <= 4; ++grade) {
			if (grade == 1 && progress.total_grade[1] == 0) {
				continue;
			}
			row->addWidget(MakeGradeCount(row_widget, grade, progress.earned_grade[grade], 24, 18));
		}
		list->setItemWidget(item, row_widget);
	}
	if (trophy_games.empty()) {
		auto* empty =
		    new QLabel(QObject::tr("No games with trophies found in the game folders."), &dialog);
		empty->setAlignment(Qt::AlignCenter);
		layout->insertWidget(1, empty);
		list->hide();
	}

	connect(list, &QListWidget::itemClicked, &dialog,
	        [&dialog, &trophy_games, &runtime_directory](QListWidgetItem* item) {
		        const auto index = item->data(Qt::UserRole).toInt();
		        if (index >= 0 && static_cast<size_t>(index) < trophy_games.size()) {
			        ShowForGame(trophy_games[static_cast<size_t>(index)].game, runtime_directory,
			                    &dialog);
		        }
	        });
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	dialog.exec();
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
		const auto unlocks     = LoadUnlocks(info, runtime_directory, file);
		const auto counts      = Common::Trophies::GetProgress(package, unlocks);
		auto*      page        = new QWidget(m_tabs);
		auto*      page_layout = new QVBoxLayout(page);

		auto* header = new QHBoxLayout;
		auto* art    = new QLabel(page);
		if (const auto pixmap = LoadGameArt(&info, 56); !pixmap.isNull()) {
			art->setPixmap(pixmap);
			header->addWidget(art);
		}
		auto* title = new QLabel(QString::fromStdString(package.title), page);
		title->setStyleSheet(QStringLiteral("font-size: 28px;"));
		header->addWidget(title, 1);
		const auto make_stat = [page](const QString& label, const QString& value) {
			auto* box    = new QVBoxLayout;
			auto* name   = new QLabel(label, page);
			auto* number = new QLabel(value, page);
			name->setStyleSheet(QStringLiteral("font-size: 14px;"));
			number->setStyleSheet(QStringLiteral("font-size: 22px;"));
			box->addWidget(name);
			box->addWidget(number);
			return box;
		};
		header->addLayout(
		    make_stat(tr("Progress"), QStringLiteral("%1%").arg(counts.Percentage())));
		header->addLayout(
		    make_stat(tr("Earned"), QStringLiteral("%1/%2").arg(counts.earned).arg(counts.total)));
		for (int grade = 1; grade <= 4; ++grade) {
			if (counts.total_grade[grade] > 0) {
				header->addWidget(MakeGradeCount(page, grade, counts.earned_grade[grade], 30, 20));
			}
		}
		page_layout->addLayout(header);
		auto* progress = new QProgressBar(page);
		progress->setRange(0, 100);
		progress->setValue(counts.Percentage());
		progress->setTextVisible(false);
		page_layout->addWidget(progress);
		auto* count_label = new QLabel(tr("All trophies: %1").arg(counts.total), page);
		page_layout->addWidget(count_label);

		auto* list = new QListWidget(page);
		list->setSelectionMode(QAbstractItemView::NoSelection);
		list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
		list->setSpacing(4);
		page_layout->addWidget(list, 1);
		for (const auto& [id, trophy]: package.trophies) {
			const bool trophy_unlocked = unlocks.unlocked.contains(id);
			const auto hidden_locked   = trophy.hidden && !trophy_unlocked;
			auto*      item            = new QListWidgetItem(list);
			item->setSizeHint(QSize(900, 96));

			auto* card = new QWidget(list);
			card->setStyleSheet(
			    QStringLiteral("QWidget#card { background: palette(base); border-radius: 12px; }"));
			card->setObjectName(QStringLiteral("card"));
			auto* row  = new QHBoxLayout(card);
			auto* icon = new QLabel(card);
			icon->setFixedSize(76, 76);
			icon->setAlignment(Qt::AlignCenter);
			QPixmap pixmap;
			if (hidden_locked) {
				pixmap = QPixmap(QStringLiteral(":/icons/hidden-trophy.png"));
			} else if (pixmap.loadFromData(reinterpret_cast<const uchar*>(trophy.icon_png.data()),
			                               static_cast<uint>(trophy.icon_png.size())) &&
			           !trophy_unlocked) {
				pixmap =
				    QPixmap::fromImage(pixmap.toImage().convertToFormat(QImage::Format_Grayscale8));
			}
			if (!pixmap.isNull()) {
				icon->setPixmap(
				    pixmap.scaled(76, 76, Qt::KeepAspectRatio, Qt::SmoothTransformation));
			}
			row->addWidget(icon);

			auto*      left      = new QVBoxLayout;
			const auto name_text = hidden_locked         ? tr("Hidden trophy")
			                       : trophy.name.empty() ? tr("Trophy %1").arg(id)
			                                             : QString::fromStdString(trophy.name);
			auto*      name      = new QLabel(name_text, card);
			name->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: bold;"));
			left->addWidget(name);
			if (!hidden_locked) {
				auto* grade_row = new QHBoxLayout;
				auto* cup       = new QLabel(card);
				cup->setPixmap(MakeCupIcon(GradeColor(trophy.grade), 18));
				auto* grade_text = new QLabel(GradeToText(trophy.grade), card);
				grade_text->setStyleSheet(QStringLiteral("font-size: 15px;"));
				grade_row->addWidget(cup);
				grade_row->addWidget(grade_text);
				grade_row->addStretch(1);
				left->addLayout(grade_row);
			}

			row->addLayout(left, 2);

			auto*   right = new QVBoxLayout;
			QString date_text;
			if (const auto found = unlocks.timestamps.find(id); found != unlocks.timestamps.end()) {
				date_text = QDateTime::fromMSecsSinceEpoch(
				                static_cast<qint64>(
				                    (found->second - Common::Trophies::UnixEpochTick) / 1000))
				                .toLocalTime()
				                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
			} else if (trophy_unlocked) {
				date_text = tr("Earned");
			}
			auto* date = new QLabel(date_text, card);
			date->setAlignment(Qt::AlignRight);
			date->setStyleSheet(QStringLiteral("font-size: 15px;"));
			auto* detail = new QLabel(
			    hidden_locked ? QString {} : QString::fromStdString(trophy.description), card);
			detail->setWordWrap(true);
			detail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
			detail->setStyleSheet(QStringLiteral("font-size: 17px;"));
			right->addWidget(date);
			right->addWidget(detail, 1);
			row->addLayout(right, 3);
			card->setToolTip(hidden_locked ? name_text : TrophyTooltip(trophy));
			list->setItemWidget(item, card);
		}
		m_tabs->addTab(page, QString::fromStdString(package.title));
	}
	m_tabs->tabBar()->setVisible(m_tabs->count() > 1);
	if (m_tabs->count() == 0) {
		error = errors.join(QLatin1Char('\n'));
		return false;
	}
	return true;
}
