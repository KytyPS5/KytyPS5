#include "configurationListWidget.h"

#include "cheatFile.h"
#include "common/archive.h"
#include "compatibilityDatabase.h"
#include "configuration.h"
#include "configurationEditDialog.h"
#include "configurationItem.h"
#include "gameContent.h"
#include "gameListTreeWidget.h"
#include "inputMappingDialog.h"
#include "mainDialog.h"
#include "patchesDialog.h"
#include "trophyViewerDialog.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QtCore>

#ifdef __linux__
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#endif

#include <memory>
#include <vector>

#include "ui_configuration_list_widget.h"

constexpr char CONF_FILE_NAME[]    = "Kyty.ini";
constexpr char CONF_ORG_NAME[]     = "Kyty";
constexpr char CONF_APP_NAME[]     = "Kyty";
constexpr char CONF_SECTION_NAME[] = "GameConfigurations";
constexpr char CONF_LAUNCHER[]     = "Launcher";
constexpr char CONF_GAME_DIR[]     = "game_dir";
constexpr char CONF_GAME_DIRS[]    = "game_dirs";
constexpr char CONF_GLOBAL[]       = "GlobalConfiguration";
constexpr char SAVE_DATA_DIR[]     = "_SaveData";

constexpr int GAME_NAME_COLUMN             = 0;
constexpr int GAME_SERIAL_COLUMN           = 1;
constexpr int GAME_VERSION_COLUMN          = 2;
constexpr int GAME_FIRMWARE_VERSION_COLUMN = 3;
constexpr int GAME_SIZE_COLUMN             = 4;
constexpr int GAME_PATH_COLUMN             = 5;
constexpr int GAME_STATUS_COLUMN           = 6;
constexpr int GAME_COMMENT_COLUMN          = 7;

static QString NormalizeGameDirectory(const QString& dir) {
	const auto trimmed = dir.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}

	return QDir::cleanPath(QDir(trimmed).absolutePath());
}

static QString PathKey(const QString& path) {
	auto normalized = NormalizeGameDirectory(path);
	if (normalized.isEmpty()) {
		normalized = QDir::cleanPath(path.trimmed());
	}
	if (normalized.isEmpty()) {
		return {};
	}

	auto canonical = QFileInfo(normalized).canonicalFilePath();
	if (canonical.isEmpty()) {
		canonical = normalized;
	}
	canonical = QDir::cleanPath(canonical);

#ifdef __linux__
	return canonical;
#else
	return canonical.toCaseFolded();
#endif
}

static QStringList NormalizeGameDirectories(const QStringList& dirs) {
	QStringList   dirs_ret;
	QSet<QString> seen;

	for (const auto& dir: dirs) {
		const auto normalized = NormalizeGameDirectory(dir);
		const auto key        = PathKey(normalized);
		if (normalized.isEmpty() || key.isEmpty() || seen.contains(key)) {
			continue;
		}

		seen.insert(key);
		dirs_ret.append(normalized);
	}

	return dirs_ret;
}

static QStringList SettingsStringList(const QVariant& value) {
	auto list = value.toStringList();
	if (!list.isEmpty()) {
		return list;
	}

	const auto text = value.toString();
	return text.isEmpty() ? QStringList() : QStringList({text});
}

static void ConfigureGameList(Ui::ConfigurationListWidget* ui) {
	ui->cfgs_list->setAlternatingRowColors(false);
	ui->cfgs_list->setAllColumnsShowFocus(true);
	ui->cfgs_list->setIndentation(0);
	ui->cfgs_list->setMouseTracking(false);
	ui->cfgs_list->setSelectionBehavior(QAbstractItemView::SelectRows);
	ui->cfgs_list->setSelectionMode(QAbstractItemView::SingleSelection);
	ui->cfgs_list->setTextElideMode(Qt::ElideMiddle);
	ui->cfgs_list->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	ui->cfgs_list->header()->setHighlightSections(false);
	ui->cfgs_list->header()->setStretchLastSection(true);
}

static void AddSaveDataDir(QStringList* dirs, QSet<QString>* seen, const QString& root,
                           const QString& title_id) {
	const auto path = QDir(root).filePath(
	    QStringLiteral("%1/%2").arg(QString::fromLatin1(SAVE_DATA_DIR), title_id));
	QDir dir(path);
	if (!dir.exists()) {
		return;
	}

	const auto absolute  = dir.absolutePath();
	auto       canonical = QFileInfo(absolute).canonicalFilePath();
	if (canonical.isEmpty()) {
		canonical = absolute;
	}
	canonical = QDir::cleanPath(canonical);

	if (!seen->contains(canonical)) {
		seen->insert(canonical);
		dirs->append(absolute);
	}
}

static QStringList GetSaveDataDirs(const Configuration& info) {
	QStringList dirs;
	if (info.title_id.trimmed().isEmpty()) {
		return dirs;
	}

	QSet<QString> seen;
	QStringList   roots({QDir::currentPath(), QCoreApplication::applicationDirPath()});

	QDir current_parent(QDir::currentPath());
	if (current_parent.cdUp()) {
		roots.append(current_parent.absolutePath());
	}

	QDir app_parent(QCoreApplication::applicationDirPath());
	if (app_parent.cdUp()) {
		roots.append(app_parent.absolutePath());
	}

	for (const auto& root: roots) {
		AddSaveDataDir(&dirs, &seen, root, info.title_id.trimmed());
	}

	return dirs;
}

ConfigurationListWidget::ConfigurationListWidget(QWidget* parent)
    : QWidget(parent), m_ui(new Ui::ConfigurationListWidget) {
	m_compatibility = new CompatibilityDatabase(
	    QCoreApplication::arguments().contains(QStringLiteral("--local")), this);
	m_ui->setupUi(this);
	ConfigureGameList(m_ui);

	m_ui->refresh_action->setShortcuts(QKeySequence::Refresh);
	m_ui->refresh_button->setDefaultAction(m_ui->refresh_action);
	addAction(m_ui->refresh_action);

	UpdateToolbarIcons();
	m_ui->global_settings_button->setToolTip(tr("Edit global settings and game folders"));
	m_ui->input_mapping_button->setToolTip(tr("Edit global input mapping"));

	m_ui->delete_button->setEnabled(false);
	m_ui->edit_button->setEnabled(false);

	m_ui->cfgs_list->setContextMenuPolicy(Qt::CustomContextMenu);
	m_ui->cfgs_list->setIconSize(QSize(48, 48));
	m_ui->cfgs_list->setRootIsDecorated(false);
	m_ui->cfgs_list->setUniformRowHeights(true);
	m_ui->cfgs_list->setSortingEnabled(true);
	m_ui->cfgs_list->setColumnWidth(GAME_NAME_COLUMN, 320);
	m_ui->cfgs_list->setColumnWidth(GAME_SERIAL_COLUMN, 110);
	m_ui->cfgs_list->setColumnWidth(GAME_VERSION_COLUMN, 120);
	m_ui->cfgs_list->setColumnWidth(GAME_FIRMWARE_VERSION_COLUMN, 150);
	m_ui->cfgs_list->setColumnWidth(GAME_SIZE_COLUMN, 100);
	m_ui->cfgs_list->setColumnWidth(GAME_PATH_COLUMN, 320);
	m_ui->cfgs_list->setColumnWidth(GAME_STATUS_COLUMN, 150);
	m_ui->cfgs_list->setColumnWidth(GAME_COMMENT_COLUMN, 240);
	m_ui->cfgs_list->sortItems(GAME_NAME_COLUMN, Qt::AscendingOrder);

	connect(m_ui->refresh_action, &QAction::triggered, this,
	        &ConfigurationListWidget::ScanGameDirectory);
	connect(m_ui->global_settings_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::edit_global_settings);
	connect(m_ui->input_mapping_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::edit_input_mapping);
	connect(m_ui->edit_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::edit_configuration);
	connect(m_ui->delete_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::delete_configuartion);
	connect(m_ui->trophy_overview_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::ViewTrophyOverview);
	connect(m_ui->cfgs_list, &QTreeWidget::currentItemChanged, this,
	        &ConfigurationListWidget::SelectItem);
	connect(m_ui->cfgs_list, &QTreeWidget::itemDoubleClicked, this,
	        &ConfigurationListWidget::list_itemDoubleClicked);
	connect(m_ui->cfgs_list, &QTreeWidget::customContextMenuRequested, this,
	        &ConfigurationListWidget::show_context_menu);
	connect(m_ui->search_line_edit, &QLineEdit::textChanged, this,
	        &ConfigurationListWidget::filter_configurations);
	connect(m_compatibility, &CompatibilityDatabase::Updated, this,
	        &ConfigurationListWidget::ApplyCompatibility);

	m_ui->cfgs_list->setDragDropMode(QAbstractItemView::NoDragDrop);

	ReadSettings();
	if (m_compatibility->IsLocal()) {
		m_compatibility->Load();
	}
	ScanGameDirectory();
	if (!m_compatibility->IsLocal()) {
		m_compatibility->Load();
	}
}

ConfigurationListWidget::~ConfigurationListWidget() {
	qDeleteAll(m_custom_infos);
	delete m_ui;
}

void ConfigurationListWidget::changeEvent(QEvent* event) {
	QWidget::changeEvent(event);
	if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange) {
		UpdateToolbarIcons();
	}
}

void ConfigurationListWidget::UpdateToolbarIcons() {
	const auto color = palette().color(QPalette::Window).lightness() < 128 ? QColor(Qt::white)
	                                                                      : QColor(Qt::black);
	const auto set_icon = [&color](QToolButton* button, const QString& resource) {
		auto pixmap = QIcon(resource).pixmap(button->iconSize(), button->devicePixelRatioF());
		QPainter painter(&pixmap);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(pixmap.rect(), color);
		button->setIcon(QIcon(pixmap));
	};

	set_icon(m_ui->refresh_button, QStringLiteral(":/icons/refresh.svg"));
	m_ui->refresh_action->setIcon(m_ui->refresh_button->icon());
	set_icon(m_ui->global_settings_button, QStringLiteral(":/icons/global-settings.svg"));
	set_icon(m_ui->input_mapping_button, QStringLiteral(":/icons/input-mapping.svg"));
	set_icon(m_ui->edit_button, QStringLiteral(":/icons/edit-configuration.svg"));
	set_icon(m_ui->delete_button, QStringLiteral(":/icons/remove-configuration.svg"));
	set_icon(m_ui->trophy_overview_button, QStringLiteral(":/icons/trophy.svg"));
}

static std::unique_ptr<QSettings> CreateSettings() {
	const QDir app_dir(QCoreApplication::applicationDirPath());
	if (QFileInfo(app_dir.filePath("portable.txt")).isFile()) {
		return std::make_unique<QSettings>(app_dir.filePath(CONF_FILE_NAME), QSettings::IniFormat);
	}
	if (QFile::exists(CONF_FILE_NAME)) {
		return std::make_unique<QSettings>(CONF_FILE_NAME, QSettings::IniFormat);
	}
#ifdef __linux__
	return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME,
	                                   CONF_APP_NAME);
#else
	return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::SystemScope, CONF_ORG_NAME,
	                                   CONF_APP_NAME);
#endif
}

void ConfigurationListWidget::WriteSettings() {
	auto s = CreateSettings();

	MainDialog::WriteSettings(*s);
	ConfigurationEditDialog::WriteSettings(*s);

	s->beginGroup(CONF_LAUNCHER);
	m_game_dirs = NormalizeGameDirectories(m_game_dirs);
	s->setValue(CONF_GAME_DIRS, m_game_dirs);
	s->remove(CONF_GAME_DIR);
	s->endGroup();

	s->remove(CONF_GLOBAL);
	s->beginGroup(CONF_GLOBAL);
	m_global_info.WriteSettings(s.get());
	m_global_info.controller.WriteSettings(s.get());
	s->endGroup();

	s->remove(CONF_SECTION_NAME);
	s->beginWriteArray(CONF_SECTION_NAME);
	int i = 0;
	for (auto it = m_custom_infos.constBegin(); it != m_custom_infos.constEnd(); ++it) {
		s->setArrayIndex(i++);
		it.value()->WriteSettings(s.get());
	}
	s->endArray();
}

void ConfigurationListWidget::ReadSettings() {
	auto s = CreateSettings();

	m_settings_file = s->fileName();

	MainDialog::ReadSettings(*s);
	ConfigurationEditDialog::ReadSettings(*s);

	s->beginGroup(CONF_LAUNCHER);
	m_game_dirs = NormalizeGameDirectories(SettingsStringList(s->value(CONF_GAME_DIRS)));
	if (m_game_dirs.isEmpty()) {
		m_game_dirs = NormalizeGameDirectories(SettingsStringList(s->value(CONF_GAME_DIR)));
	}
	s->endGroup();

	s->beginGroup(CONF_GLOBAL);
	if (!s->childKeys().isEmpty()) {
		m_global_info.ReadSettings(s.get());
	}
	m_global_info.controller.ReadSettings(s.get());
	s->endGroup();

	qDeleteAll(m_custom_infos);
	m_custom_infos.clear();

	int size = s->beginReadArray(CONF_SECTION_NAME);

	for (int i = 0; i < size; i++) {
		s->setArrayIndex(i);
		auto* info = new Configuration;
		info->ReadSettings(s.get());
		info->custom_settings = true;
		if (!info->game_path.isEmpty()) {
			m_custom_infos.insert(info->game_path, info);
		} else {
			delete info;
		}
	}
	s->endArray();
}

void ConfigurationListWidget::ApplyCompatibility() {
	const bool sorting_enabled = m_ui->cfgs_list->isSortingEnabled();
	const int  sort_column     = m_ui->cfgs_list->sortColumn();
	const auto sort_order      = m_ui->cfgs_list->header()->sortIndicatorOrder();
	m_ui->cfgs_list->setSortingEnabled(false);

	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); index++) {
		auto*       item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		const auto& title_id = item->GetInfo().title_id;
		const auto* entry    = m_compatibility->Find(title_id);
		item->GetInfo().game_status =
		    entry != nullptr ? entry->status : Configuration::GameStatus::Unknown;
		item->GetInfo().game_comment = entry != nullptr ? entry->comment : QString();

		const QSignalBlocker blocker(item->GetStatusCombo());
		item->Update();
		item->SetCompatibilityEditable(m_compatibility->IsLocal() && !title_id.trimmed().isEmpty());
	}

	m_ui->cfgs_list->setSortingEnabled(sorting_enabled);
	if (sorting_enabled) {
		m_ui->cfgs_list->sortItems(sort_column, sort_order);
	}
}

std::unique_ptr<Configuration>
ConfigurationListWidget::CreateConfiguration(const ConfigurationItem& item) const {
	auto        info   = std::make_unique<Configuration>();
	const auto* custom = m_custom_infos.value(item.GetInfo().game_path);
	info->CopyGameInfoFrom(item.GetInfo());
	info->CopyEmulatorSettingsFrom(custom != nullptr ? *custom : m_global_info);
	info->controller = m_global_info.controller;
	if (custom != nullptr && !custom->elf.isEmpty()) {
		info->elf = custom->elf;
	}
	info->host_input_mapping = m_global_info.host_input_mapping;
	return info;
}

struct GameMetadata {
	QString title_name;
	QString title_id;
	QString gameVersion;
	QString firmwareVer;
};

static QString GetJsonString(const QJsonObject& obj, const QString& key) {
	return obj.value(key).toString().trimmed();
}

static QString GetLocalizedTitleName(const QJsonObject& root) {
	const auto localized = root.value(QStringLiteral("localizedParameters")).toObject();
	if (localized.isEmpty()) {
		return {};
	}

	const auto default_language = GetJsonString(localized, QStringLiteral("defaultLanguage"));
	if (!default_language.isEmpty()) {
		const auto title = GetJsonString(localized.value(default_language).toObject(),
		                                 QStringLiteral("titleName"));
		if (!title.isEmpty()) {
			return title;
		}
	}

	const auto english_title = GetJsonString(localized.value(QStringLiteral("en-US")).toObject(),
	                                         QStringLiteral("titleName"));
	if (!english_title.isEmpty()) {
		return english_title;
	}

	for (auto it = localized.constBegin(); it != localized.constEnd(); ++it) {
		const auto title = GetJsonString(it.value().toObject(), QStringLiteral("titleName"));
		if (!title.isEmpty()) {
			return title;
		}
	}

	return {};
}

static QString GetFirmwareVersion(const QJsonObject& root) {
	const auto encoded = GetJsonString(root, QStringLiteral("requiredSystemSoftwareVersion"));
	static const QRegularExpression version_pattern(
	    QStringLiteral("^0[xX]([0-9]{6})[0-9A-Fa-f]{10}$"));
	const auto match = version_pattern.match(encoded);
	if (!match.hasMatch()) {
		return {};
	}

	const auto digits = match.captured(1);
	bool       valid  = false;
	const int  major  = digits.left(2).toInt(&valid, 10);
	if (!valid) {
		return {};
	}

	QString    version = QStringLiteral("%1.%2").arg(major).arg(digits.mid(2, 2));
	const auto patch   = digits.mid(4, 2);
	if (patch != QStringLiteral("00")) {
		version += QStringLiteral(".") + patch;
	}

	return version;
}

static GameMetadata GetGameMetadata(const QByteArray& param_data, const QString& fallback) {
	GameMetadata ret;
	ret.title_name = fallback;

	if (param_data.isEmpty()) {
		return ret;
	}

	QJsonParseError parse_error;
	const auto      doc = QJsonDocument::fromJson(param_data, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		return ret;
	}

	const auto root  = doc.object();
	const auto title = GetLocalizedTitleName(root);
	if (!title.isEmpty()) {
		ret.title_name = title;
	}

	ret.title_id    = GetJsonString(root, QStringLiteral("titleId"));
	ret.gameVersion = GetJsonString(root, QStringLiteral("appVersion"));
	if (ret.gameVersion.isEmpty()) {
		ret.gameVersion = GetJsonString(root, QStringLiteral("contentVersion"));
	}
	ret.firmwareVer = GetFirmwareVersion(root);

	return ret;
}

static void SetGameFiles(Configuration& info, const QString& game_dir, const QString& game_path,
                         const GameMetadata& metadata, bool archive) {
	const QFileInfo game(game_dir);

	info.game_path   = game_path;
	info.basedir     = archive ? game.absoluteFilePath() : QDir(game_dir).absolutePath();
	info.name        = metadata.title_name;
	info.title_id    = metadata.title_id;
	info.gameVersion = metadata.gameVersion;
	info.firmwareVer = metadata.firmwareVer;

	if (info.name.isEmpty()) {
		info.name = archive ? game.completeBaseName() : QDir(game_dir).dirName();
	}
}

static Configuration* FindCustomInfo(QMap<QString, Configuration*>* custom_infos,
                                     const QString& game_path, const QString& legacy_game_path) {
	auto custom = custom_infos->find(game_path);
	if (custom != custom_infos->end()) {
		return custom.value();
	}

	custom = custom_infos->find(legacy_game_path);
	if (custom == custom_infos->end()) {
		return nullptr;
	}

	auto* info = custom.value();
	custom_infos->erase(custom);
	info->game_path = game_path;
	custom_infos->insert(game_path, info);

	return info;
}

bool ConfigurationListWidget::EnsureGameDirectory() {
	if (HasValidGameDirectory()) {
		return true;
	}

	QMessageBox::information(this, tr("Game folders"),
	                         tr("Add at least one game folder in global settings."));
	edit_global_settings();

	return HasValidGameDirectory();
}

bool ConfigurationListWidget::HasValidGameDirectory() const {
	for (const auto& dir: m_game_dirs) {
		if (!dir.isEmpty() && QDir(dir).exists()) {
			return true;
		}
	}

	return false;
}

void ConfigurationListWidget::ScanGameDirectory() {
	// Commit an active inline editor before destroying its row or replacing its data.
	if (auto* focused = QApplication::focusWidget();
	    focused != nullptr && m_ui->cfgs_list->isAncestorOf(focused)) {
		m_ui->cfgs_list->setFocus();
	}

	const auto selected_path =
	    m_selected_item != nullptr ? m_selected_item->GetInfo().game_path : QString();
	const bool           sorting_enabled = m_ui->cfgs_list->isSortingEnabled();
	const int            sort_column     = m_ui->cfgs_list->sortColumn();
	const auto           sort_order      = m_ui->cfgs_list->header()->sortIndicatorOrder();
	const QSignalBlocker blocker(m_ui->cfgs_list);
	m_ui->cfgs_list->setSortingEnabled(false);
	m_selected_item = nullptr;

	// MainDialog tracks the running item, so keep its identity even if its
	// directory is no longer present. Other rows can be rebuilt from disk.
	QMap<QString, ConfigurationItem*> running_items;
	for (int index = m_ui->cfgs_list->topLevelItemCount() - 1; index >= 0; index--) {
		auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		if (item->IsRunning()) {
			running_items.insert(PathKey(item->GetInfo().game_path), item);
		} else {
			delete item;
		}
	}

	const QString eboot_name = QStringLiteral("eboot.bin");
	QSet<QString> found_games;
	const auto add_game = [this, &found_games, &eboot_name,
	                       &running_items](const QString& base, const QString& game_path,
	                                       const QString& legacy_game_path, const QString& fallback,
	                                       bool archive) {
		const QString game_key = PathKey(game_path);
		if (game_key.isEmpty() || found_games.contains(game_key)) {
			return;
		}
		// Keep the index and decompression cache alive through validation, metadata and icon
		// reads, after rejecting duplicate candidates from overlapping game folders.
		const auto reader = archive ? Common::OpenArchive(GameContent::ToPath(base)) : nullptr;
		if (archive && (reader == nullptr || !GameContent::FileExists(base, eboot_name))) {
			return;
		}
		found_games.insert(game_key);

		const auto metadata =
		    GetGameMetadata(GameContent::ReadFile(base, QStringLiteral("sce_sys/param.json"),
		                                          GameContent::MaxMetadataSize),
		                    fallback);
		auto info = std::make_unique<Configuration>();
		info->custom_settings =
		    FindCustomInfo(&m_custom_infos, game_path, legacy_game_path) != nullptr;

		SetGameFiles(*info, base, game_path, metadata, archive);
		const auto* compatibility = m_compatibility->Find(info->title_id);
		if (compatibility != nullptr) {
			info->game_status  = compatibility->status;
			info->game_comment = compatibility->comment;
		}

		if (auto* item = running_items.value(game_key); item != nullptr) {
			// Refresh metadata without losing the running row's identity.
			const QSignalBlocker status_blocker(item->GetStatusCombo());
			item->GetInfo().CopyGameInfoFrom(*info);
			item->Update(true);
			item->SetCompatibilityEditable(m_compatibility->IsLocal() &&
			                               !item->GetInfo().title_id.trimmed().isEmpty());
			return;
		}

		auto* item = new ConfigurationItem(std::move(info), m_ui->cfgs_list);
		item->SetCompatibilityEditable(m_compatibility->IsLocal() &&
		                               !item->GetInfo().title_id.trimmed().isEmpty());
		connect(item->GetStatusCombo(), &QComboBox::currentIndexChanged, item,
		        [this, item](int /*index*/) {
			        if (!m_compatibility->IsLocal()) {
				        return;
			        }

			        const auto& title_id = item->GetInfo().title_id;
			        if (title_id.trimmed().isEmpty()) {
				        return;
			        }
			        item->GetInfo().game_status = static_cast<Configuration::GameStatus>(
			            item->GetStatusCombo()->currentData().toInt());
			        item->Update();
			        m_compatibility->SetStatus(title_id, item->GetInfo().game_status);
			        m_ui->cfgs_list->setCurrentItem(item);
			        SelectItem(item);
		        });
		connect(item->GetCommentEdit(), &QLineEdit::editingFinished, item, [this, item]() {
			if (!m_compatibility->IsLocal()) {
				return;
			}

			const auto& title_id = item->GetInfo().title_id;
			if (title_id.trimmed().isEmpty()) {
				return;
			}
			item->GetInfo().game_comment = item->GetCommentEdit()->text();
			item->Update();
			m_compatibility->SetComment(title_id, item->GetInfo().game_comment);
			m_ui->cfgs_list->setCurrentItem(item);
			SelectItem(item);
		});
	};
	const auto scan_archives = [&add_game](const QDir& directory, const QDir& root) {
		const auto entries = directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
		for (const auto& archive: entries) {
			const auto base = archive.absoluteFilePath();
			if (!Common::IsSupportedArchive(GameContent::ToPath(base))) {
				continue;
			}
			add_game(base, QDir::cleanPath(base), root.relativeFilePath(base),
			         archive.completeBaseName(), true);
		}
	};

	for (const auto& root_path: m_game_dirs) {
		QDir root(root_path);
		if (root_path.isEmpty() || !root.exists()) {
			continue;
		}

		scan_archives(root, root);
		QList<QDir> pending_dirs;
		const auto  root_subdirs =
		    root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
		for (const auto& subdir: root_subdirs) {
			pending_dirs.append(QDir(subdir.absoluteFilePath()));
		}

		while (!pending_dirs.isEmpty()) {
			QDir game_dir = pending_dirs.takeFirst();
			scan_archives(game_dir, root);

			if (game_dir.exists(eboot_name)) {
				const QString game_path        = NormalizeGameDirectory(game_dir.absolutePath());
				const QString legacy_game_path = root.relativeFilePath(game_dir.absolutePath());
				add_game(game_dir.absolutePath(), game_path, legacy_game_path, game_dir.dirName(),
				         false);
				continue;
			}

			const auto subdirs =
			    game_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
			for (const auto& subdir: subdirs) {
				pending_dirs.append(QDir(subdir.absoluteFilePath()));
			}
		}
	}

	m_ui->cfgs_list->setSortingEnabled(sorting_enabled);
	if (sorting_enabled) {
		m_ui->cfgs_list->sortItems(sort_column, sort_order);
	}
	filter_configurations(m_ui->search_line_edit->text());

	ConfigurationItem* selected_item = nullptr;
	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); index++) {
		auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		if (!item->isHidden() && item->GetInfo().game_path == selected_path) {
			selected_item = item;
			break;
		}
	}
	m_ui->cfgs_list->setCurrentItem(selected_item);
	SelectItem(selected_item);
}

void ConfigurationListWidget::edit_configuration() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr) {
		return;
	}

	auto                    info = CreateConfiguration(*item);
	ConfigurationEditDialog dlg(*info, this);
	dlg.setWindowTitle(tr("Edit game config"));
	connect(&dlg, &ConfigurationEditDialog::PreviewControllerColor, this,
	        &ConfigurationListWidget::PreviewControllerColor);

	if (dlg.exec() == QDialog::Accepted) {
		info->custom_settings           = true;
		item->GetInfo().custom_settings = true;
		auto game_path                  = info->game_path;
		delete m_custom_infos.take(game_path);
		m_custom_infos.insert(game_path, info.release());
		WriteSettings();
		item->Update();
		SelectItem(item);
	} else {
		emit Select();
	}
}

void ConfigurationListWidget::delete_configuartion() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr || !item->GetInfo().custom_settings) {
		return;
	}

	if (QMessageBox::Yes == QMessageBox::question(this, tr("Clear game config"),
	                                              tr("Clear this game's config and use global settings?"))) {
		delete m_custom_infos.take(item->GetInfo().game_path);
		item->GetInfo().custom_settings = false;
		WriteSettings();
		item->Update();
		SelectItem(item);
	}
}

void ConfigurationListWidget::edit_global_settings() {
	Configuration info;
	info.CopyEmulatorSettingsFrom(m_global_info);
	info.controller = m_global_info.controller;
	info.name = tr("Global settings");

	ConfigurationEditDialog dlg(info, this);
	dlg.setWindowTitle(tr("Global settings"));
	dlg.SetGlobalSettings(m_game_dirs);
	connect(&dlg, &ConfigurationEditDialog::PreviewControllerColor, this,
	        &ConfigurationListWidget::PreviewControllerColor);
	connect(&dlg, &ConfigurationEditDialog::ImportGameSettings, this,
	        [this, &dlg]() { ImportGameSettings(&dlg); });
	connect(&dlg, &ConfigurationEditDialog::ExportGameSettings, this,
	        [this, &dlg]() { ExportGameSettings(&dlg); });

	if (dlg.exec() == QDialog::Accepted) {
		m_global_info.CopyEmulatorSettingsFrom(info);
		m_global_info.controller     = info.controller;
		const auto game_dirs         = NormalizeGameDirectories(dlg.GetGameDirectories());
		const bool game_dirs_changed = game_dirs != m_game_dirs;
		m_game_dirs                  = game_dirs;
		WriteSettings();
		if (game_dirs_changed) {
			ScanGameDirectory();
		}
	}
	SelectItem(m_ui->cfgs_list->currentItem());
}

static bool IsGameSettingsTitleId(const QString& title_id) {
	static const QRegularExpression pattern(QStringLiteral("\\APPSA[0-9]{5}\\z"));
	return pattern.match(title_id).hasMatch();
}

void ConfigurationListWidget::ImportGameSettings(QWidget* parent) {
	const auto path = QFileDialog::getOpenFileName(parent, tr("Import game configs"), {},
	                                               tr("JSON files (*.json)"));
	if (path.isEmpty()) {
		return;
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, tr("Import failed"), file.errorString());
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject() || document.object().isEmpty()) {
		QMessageBox::warning(
		    parent, tr("Import failed"),
		    tr("Expected a JSON object containing game configs keyed by PPSA code."));
		return;
	}
	const auto settings = document.object();
	for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
		Configuration info;
		QString       error;
		if (!IsGameSettingsTitleId(it.key()) || !it.value().isObject() ||
		    it.value().toObject().isEmpty() ||
		    !info.SetGameSettings(it.value().toObject(), error)) {
			QMessageBox::warning(parent, tr("Import failed"),
			                     tr("Invalid game config for %1. %2").arg(it.key(), error));
			return;
		}
	}
	struct ImportedGame {
		ConfigurationItem*             item;
		std::unique_ptr<Configuration> info;
	};
	std::vector<ImportedGame> imported;
	auto                      unmatched = settings.keys();
	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); ++index) {
		auto*      item     = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		const auto title_id = item->GetInfo().title_id.trimmed().toUpper();
		if (!settings.contains(title_id)) {
			continue;
		}
		auto    info = CreateConfiguration(*item);
		QString error;
		if (!info->SetGameSettings(settings.value(title_id).toObject(), error)) {
			QMessageBox::warning(parent, tr("Import failed"),
			                     tr("Invalid game config for %1. %2").arg(title_id, error));
			return;
		}
		info->custom_settings = true;
		imported.push_back({item, std::move(info)});
		unmatched.removeAll(title_id);
	}
	auto message = imported.empty() ? tr("No matching games found in the library.")
	                                : tr("Configs will be updated for the following games:\n");
	for (const auto& game: imported) {
		message += tr("\n%1 — %2").arg(game.info->title_id.trimmed().toUpper(), game.info->name);
	}
	if (!unmatched.isEmpty()) {
		message += tr("\nGames not found: %1").arg(unmatched.join(QStringLiteral(", ")));
	}
	if (imported.empty()) {
		QMessageBox::information(parent, tr("Import game configs"), message);
		return;
	}
	QMessageBox confirmation(QMessageBox::Question, tr("Import game configs"), message,
	                         QMessageBox::Apply | QMessageBox::Cancel, parent);
	confirmation.setTextFormat(Qt::PlainText);
	confirmation.setDefaultButton(QMessageBox::Cancel);
	if (confirmation.exec() != QMessageBox::Apply) {
		return;
	}
	for (auto& [item, info]: imported) {
		const auto game_path = info->game_path;
		delete m_custom_infos.take(game_path);
		m_custom_infos.insert(game_path, info.release());
		item->GetInfo().custom_settings = true;
		item->Update();
	}
	WriteSettings();
}

void ConfigurationListWidget::ExportGameSettings(QWidget* parent) const {
	QMap<QString, const ConfigurationItem*> games;
	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); ++index) {
		const auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		const auto& game = item->GetInfo();
		const auto  title_id = game.title_id.trimmed().toUpper();
		if (m_custom_infos.contains(game.game_path) && IsGameSettingsTitleId(title_id)) {
			games.insert(tr("%1 — %2 (%3)").arg(title_id, game.name, game.game_path), item);
		}
	}
	if (games.isEmpty()) {
		QMessageBox::information(parent, tr("Export game config"),
		                         tr("No game configs with a PPSA code were found."));
		return;
	}
	bool       accepted = false;
	const auto selected = QInputDialog::getItem(parent, tr("Export game config"), tr("Game:"),
	                                            games.keys(), 0, false, &accepted);
	if (!accepted) {
		return;
	}
	const auto& game     = games.value(selected)->GetInfo();
	const auto  title_id = game.title_id.trimmed().toUpper();
	const auto  path =
	    QFileDialog::getSaveFileName(parent, tr("Export game config"),
	                                 title_id + QStringLiteral(".json"), tr("JSON files (*.json)"));
	if (path.isEmpty()) {
		return;
	}
	const auto settings =
	    QJsonObject::fromVariantMap(m_custom_infos.value(game.game_path)->GameSettings());
	const auto data =
	    QJsonDocument(QJsonObject {{title_id, settings}}).toJson(QJsonDocument::Indented);
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
		QMessageBox::warning(parent, tr("Export failed"), file.errorString());
	}
}

void ConfigurationListWidget::edit_input_mapping() {
	InputMappingDialog dialog(m_global_info.host_input_mapping, this);
	if (dialog.exec() == QDialog::Accepted) {
		m_global_info.host_input_mapping = dialog.Mapping();
		WriteSettings();
	}
}

void ConfigurationListWidget::ViewTrophies() {
	auto* current = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	auto* item    = current != nullptr ? current : m_selected_item;
	if (item == nullptr) {
		return;
	}

	const auto config = CreateConfiguration(*item);
	TrophyViewerDialog::ShowForGame(config.get(), m_runtime_directory, this);
}

void ConfigurationListWidget::ViewTrophyOverview() {
	std::vector<std::unique_ptr<Configuration>> configurations;
	std::vector<const Configuration*>            games;
	configurations.reserve(static_cast<size_t>(m_ui->cfgs_list->topLevelItemCount()));
	games.reserve(static_cast<size_t>(m_ui->cfgs_list->topLevelItemCount()));
	for (int index = 0; index < m_ui->cfgs_list->topLevelItemCount(); ++index) {
		const auto* item =
		    static_cast<const ConfigurationItem*>(m_ui->cfgs_list->topLevelItem(index));
		configurations.push_back(CreateConfiguration(*item));
		games.push_back(configurations.back().get());
	}
	TrophyViewerDialog::ShowOverview(games, m_runtime_directory, this);
}

void ConfigurationListWidget::open_game_folder() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr) {
		return;
	}

	const auto base       = item->GetInfo().basedir;
	const bool is_archive = GameContent::IsArchive(base);
	const QDir game_dir(is_archive ? QFileInfo(base).absolutePath() : base);
	if (!game_dir.exists()) {
		QMessageBox::warning(this, tr("Open game folder"), tr("Game folder does not exist."));
		return;
	}

	const auto open_directory = [this, game_dir] {
		if (!QDesktopServices::openUrl(QUrl::fromLocalFile(game_dir.absolutePath()))) {
			QMessageBox::warning(this, tr("Open game folder"), tr("Could not open game folder."));
		}
	};
	if (is_archive) {
		const auto path = QFileInfo(base).absoluteFilePath();
#if defined(_WIN32)
		QProcess explorer;
		explorer.setProgram("explorer.exe");
		explorer.setNativeArguments(
		    QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(path)));
		if (explorer.startDetached()) {
			return;
		}
#elif defined(__APPLE__)
		if (QProcess::startDetached("/usr/bin/open", {"-R", path})) {
			return;
		}
#elif defined(__linux__)
		auto request = QDBusMessage::createMethodCall("org.freedesktop.FileManager1",
		                                              "/org/freedesktop/FileManager1",
		                                              "org.freedesktop.FileManager1", "ShowItems");
		request << QStringList {QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)}
		        << QString();
		auto* watcher = new QDBusPendingCallWatcher(
		    QDBusConnection::sessionBus().asyncCall(request, 5000), this);
		connect(watcher, &QDBusPendingCallWatcher::finished, this,
		        [open_directory](QDBusPendingCallWatcher* call) {
			        call->deleteLater();
			        if (call->isError()) {
				        open_directory();
			        }
		        });
		return;
#endif
	}
	open_directory();
}

void ConfigurationListWidget::remove_save_data() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr) {
		return;
	}

	const auto save_data_dirs = GetSaveDataDirs(item->GetInfo());
	if (save_data_dirs.isEmpty()) {
		QMessageBox::information(this, tr("Remove save data"),
		                         tr("No save data folder found for this game."));
		return;
	}

	const auto title =
	    !item->GetInfo().name.isEmpty() ? item->GetInfo().name : item->GetInfo().title_id;
	const auto text =
	    tr("Remove save data for \"%1\"?\n\nThis will delete:\n%2\n\nThis cannot be undone.")
	        .arg(title, save_data_dirs.join(QLatin1Char('\n')));

	if (QMessageBox::Yes != QMessageBox::question(this, tr("Remove save data"), text)) {
		return;
	}

	QStringList failed_dirs;
	for (const auto& path: save_data_dirs) {
		QDir dir(path);
		if (dir.exists() && !dir.removeRecursively()) {
			failed_dirs.append(path);
		}
	}

	if (!failed_dirs.isEmpty()) {
		QMessageBox::warning(this, tr("Remove save data"),
		                     tr("Could not remove:\n%1").arg(failed_dirs.join(QLatin1Char('\n'))));
	}
}

void ConfigurationListWidget::filter_configurations(const QString& text) {
	const auto query              = text.trimmed();
	const bool has_query          = !query.isEmpty();
	bool       selection_is_shown = false;

	for (int item_index = 0; item_index < m_ui->cfgs_list->topLevelItemCount(); item_index++) {
		auto* item = m_ui->cfgs_list->topLevelItem(item_index);
		if (item == nullptr) {
			continue;
		}

		const bool match = !has_query ||
		                   item->text(GAME_NAME_COLUMN).contains(query, Qt::CaseInsensitive) ||
		                   item->text(GAME_SERIAL_COLUMN).contains(query, Qt::CaseInsensitive);
		item->setHidden(!match);

		if (match && item == m_selected_item) {
			selection_is_shown = true;
		}
	}

	if (m_selected_item != nullptr && !selection_is_shown) {
		m_ui->cfgs_list->clearSelection();
		m_ui->cfgs_list->setCurrentItem(nullptr);
		SelectItem(nullptr);
	}
}

void ConfigurationListWidget::SelectItem(QTreeWidgetItem* witem) {
	auto* item = static_cast<ConfigurationItem*>(witem);
	if (item == nullptr) {
		m_selected_item = nullptr;
		m_ui->cfgs_list->SetBackgroundImage({});
		m_ui->edit_button->setEnabled(false);
		m_ui->delete_button->setEnabled(false);
		emit Select();
		return;
	}

	m_ui->delete_button->setEnabled(!item->IsRunning() && item->GetInfo().custom_settings);
	m_ui->edit_button->setEnabled(!item->IsRunning());

	m_selected_item = item;
	m_ui->cfgs_list->SetBackgroundImage(item->GetInfo().basedir);

	emit Select();
}

void ConfigurationListWidget::list_itemDoubleClicked(QTreeWidgetItem* witem, int /*column*/) {
	SelectItem(witem);
	if (m_run_enabled) {
		emit Run();
	}
}

void ConfigurationListWidget::show_context_menu(const QPoint& pos) {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->itemAt(pos));

	if (item != nullptr) {
		m_ui->cfgs_list->setCurrentItem(item);
		SelectItem(item);
	}

	QMenu      menu;
	const auto save_data_dirs = item != nullptr ? GetSaveDataDirs(item->GetInfo()) : QStringList();
	const bool has_trophy_data =
	    item != nullptr && TrophyViewerDialog::HasTrophyData(&item->GetInfo());

	QAction* action_run = menu.addAction(tr("Run"), this, SIGNAL(Run()));
	QAction* action_open_folder =
	    menu.addAction(style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Open game folder"), this,
	                   SLOT(open_game_folder()));
	QAction* action_view_trophies = menu.addAction(
	    style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("View trophies..."));
	connect(action_view_trophies, &QAction::triggered, this,
	        &ConfigurationListWidget::ViewTrophies);
	QAction* action_patches = menu.addAction(tr("Cheats (experimental)..."));
	connect(action_patches, &QAction::triggered, this,
	        [this, item = QPointer<ConfigurationItem>(item)]() {
		        if (item != nullptr) {
			        auto* dialog = new PatchesDialog(item->GetInfo(), this);
			        dialog->show();
		        }
	        });
	action_patches->setVisible(item != nullptr &&
	                           Cheats::IsSupportedTitleId(item->GetInfo().title_id));
	QAction* action_remove_save_data =
	    menu.addAction(style()->standardIcon(QStyle::SP_DialogDiscardButton),
	                   tr("Remove save data..."), this, SLOT(remove_save_data()));
	menu.addSeparator();
	QAction* action_edit =
	    menu.addAction(style()->standardIcon(QStyle::SP_FileIcon), tr("Edit game config..."),
	                   this, SLOT(edit_configuration()));
	QAction* action_delete =
	    menu.addAction(style()->standardIcon(QStyle::SP_DialogDiscardButton),
	                   tr("Clear game config"), this, SLOT(delete_configuartion()));

	if (item == nullptr) {
		menu.addSeparator();
		/*QAction *action_global = */ menu.addAction(
		    style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Global settings..."),
		    this, SLOT(edit_global_settings()));
	}

	if (item != nullptr) {
		action_run->setDisabled(item->IsRunning());
		const auto& base = item->GetInfo().basedir;
		action_open_folder->setDisabled(!QDir(base).exists() && !GameContent::IsArchive(base));
		action_view_trophies->setDisabled(!has_trophy_data);
		action_remove_save_data->setDisabled(item->IsRunning() || save_data_dirs.isEmpty());
		action_edit->setDisabled(item->IsRunning());
		action_delete->setDisabled(item->IsRunning() || !item->GetInfo().custom_settings);
	} else {
		action_run->setDisabled(true);
		action_open_folder->setDisabled(true);
		action_view_trophies->setDisabled(true);
		action_remove_save_data->setDisabled(true);
		action_edit->setDisabled(true);
		action_delete->setDisabled(true);
	}

	if (!m_run_enabled) {
		action_run->setDisabled(true);
	}

	menu.exec(QCursor::pos());
}
