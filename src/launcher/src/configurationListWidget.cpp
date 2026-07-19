#include "configurationListWidget.h"

#include "common.h"
#include "compatibilityDatabase.h"
#include "configuration.h"
#include "configurationEditDialog.h"
#include "configurationItem.h"
#include "gameListTreeWidget.h"
#include "mainDialog.h"
#include "trophyViewerDialog.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QtCore>

#include <memory>

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
constexpr int GAME_FIRMWARE_VERSION_COLUMN = 2;
constexpr int GAME_PATH_COLUMN             = 3;
constexpr int GAME_STATUS_COLUMN           = 4;
constexpr int GAME_COMMENT_COLUMN          = 5;

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

static QString GetPic0Path(const Configuration& info) {
	if (info.basedir.isEmpty()) {
		return {};
	}

	const auto path = QDir(info.basedir).filePath(QStringLiteral("sce_sys/pic0.png"));
	return QFileInfo::exists(path) ? QFileInfo(path).absoluteFilePath() : QString();
}

static void ApplyGameListStyle(Ui::ConfigurationListWidget* ui) {
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

	ui->cfgs_list->setStyleSheet(QStringLiteral(
	    "QTreeWidget { background: transparent; border: 1px solid rgba(255,255,255,42); "
	    "color: #eef4ff; outline: 0; }"
	    "QTreeWidget::item { background: rgba(8,10,16,92); "
	    "border-bottom: 1px solid rgba(255,255,255,24); padding: 4px; }"
	    "QTreeWidget::item:selected { background: rgba(32,118,210,150); color: #ffffff; }"
	    "QHeaderView::section { background: rgba(22,25,31,235); color: #dce6f6; border: 0; "
	    "border-right: 1px solid rgba(255,255,255,35); border-bottom: 1px solid "
	    "rgba(255,255,255,45); "
	    "padding: 5px 6px; }"
	    "QScrollBar:vertical { background: rgba(12,14,18,150); width: 14px; margin: 0; }"
	    "QScrollBar::handle:vertical { background: rgba(220,230,245,120); min-height: 36px; }"
	    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"));

	ui->search_line_edit->setStyleSheet(QStringLiteral(
	    "QLineEdit { background: rgba(12,14,18,210); border: 1px solid rgba(255,255,255,42); "
	    "border-radius: 4px; color: #eef4ff; padding: 5px 8px; selection-background-color: "
	    "#267bd8; }"
	    "QLineEdit:focus { border-color: rgba(80,160,255,180); }"));
	ui->global_settings_button->setStyleSheet(QStringLiteral(
	    "QToolButton { background: rgba(255,255,255,18); border: 1px solid transparent; "
	    "border-radius: 5px; padding: 3px; }"
	    "QToolButton:hover { background: rgba(255,255,255,45); border-color: rgba(255,255,255,70); "
	    "}"
	    "QToolButton:disabled { background: transparent; }"));
	ui->edit_button->setStyleSheet(ui->global_settings_button->styleSheet());
	ui->delete_button->setStyleSheet(ui->global_settings_button->styleSheet());
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
	ApplyGameListStyle(m_ui);

	m_ui->global_settings_button->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
	m_ui->global_settings_button->setToolTip(tr("Edit global settings and game folders"));
	m_ui->edit_button->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
	m_ui->delete_button->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));

	m_ui->delete_button->setEnabled(false);
	m_ui->edit_button->setEnabled(false);

	m_ui->cfgs_list->setContextMenuPolicy(Qt::CustomContextMenu);
	m_ui->cfgs_list->setIconSize(QSize(48, 48));
	m_ui->cfgs_list->setRootIsDecorated(false);
	m_ui->cfgs_list->setUniformRowHeights(true);
	m_ui->cfgs_list->setSortingEnabled(true);
	m_ui->cfgs_list->setColumnWidth(GAME_NAME_COLUMN, 320);
	m_ui->cfgs_list->setColumnWidth(GAME_SERIAL_COLUMN, 110);
	m_ui->cfgs_list->setColumnWidth(GAME_FIRMWARE_VERSION_COLUMN, 150);
	m_ui->cfgs_list->setColumnWidth(GAME_PATH_COLUMN, 320);
	m_ui->cfgs_list->setColumnWidth(GAME_STATUS_COLUMN, 150);
	m_ui->cfgs_list->setColumnWidth(GAME_COMMENT_COLUMN, 240);

	connect(m_ui->global_settings_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::edit_global_settings);
	connect(m_ui->edit_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::edit_configuration);
	connect(m_ui->delete_button, &QToolButton::clicked, this,
	        &ConfigurationListWidget::delete_configuartion);
	connect(m_ui->cfgs_list, &QTreeWidget::currentItemChanged, this,
	        &ConfigurationListWidget::list_currentItemChanged);
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

void ConfigurationListWidget::WriteSettings() {
	QFile                      file = QFile(QDir(".").absoluteFilePath(CONF_FILE_NAME));
	std::unique_ptr<QSettings> s;
	if (file.exists()) {
		s = std::make_unique<QSettings>(CONF_FILE_NAME, QSettings::IniFormat);
	} else {
#ifdef __linux__
		s = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME,
		                                CONF_APP_NAME);
#else
		s = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::SystemScope, CONF_ORG_NAME,
		                                CONF_APP_NAME);
#endif
	}

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
	QFile                      file = QFile(QDir(".").absoluteFilePath(CONF_FILE_NAME));
	std::unique_ptr<QSettings> s;
	if (file.exists()) {
		s = std::make_unique<QSettings>(CONF_FILE_NAME, QSettings::IniFormat);
	} else {
#ifdef __linux__
		s = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME,
		                                CONF_APP_NAME);
#else
		s = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::SystemScope, CONF_ORG_NAME,
		                                CONF_APP_NAME);
#endif
	}

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

static Configuration* CloneConfiguration(const Configuration& source) {
	auto* ret = new Configuration;
	ret->CopyFrom(source);
	return ret;
}

struct GameMetadata {
	QString title_name;
	QString title_id;
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

static GameMetadata GetGameMetadata(const QString& param_file, const QString& fallback) {
	GameMetadata ret;
	ret.title_name = fallback;

	QFile param(param_file);
	if (!param.open(QIODevice::ReadOnly)) {
		return ret;
	}

	QJsonParseError parse_error;
	const auto      doc = QJsonDocument::fromJson(param.readAll(), &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		return ret;
	}

	const auto root  = doc.object();
	const auto title = GetLocalizedTitleName(root);
	if (!title.isEmpty()) {
		ret.title_name = title;
	}

	ret.title_id    = GetJsonString(root, QStringLiteral("titleId"));
	ret.firmwareVer = GetFirmwareVersion(root);

	return ret;
}

static void SetGameFiles(Configuration& info, const QString& game_dir, const QString& game_path,
                         const GameMetadata& metadata) {
	QDir game(game_dir);

	info.game_path   = game_path;
	info.basedir     = game.absolutePath();
	info.name        = metadata.title_name;
	info.title_id    = metadata.title_id;
	info.firmwareVer = metadata.firmwareVer;

	if (info.name.isEmpty()) {
		info.name = game.dirName();
	}
	if (info.elf.isEmpty()) {
		info.elf = QStringLiteral("eboot.bin");
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
	m_selected_item = nullptr;
	m_ui->cfgs_list->clear();
	m_ui->cfgs_list->SetBackgroundImage({});
	m_ui->edit_button->setEnabled(false);
	m_ui->delete_button->setEnabled(false);

	const QString eboot_name = QStringLiteral("eboot.bin");
	QSet<QString> found_games;

	for (const auto& root_path: m_game_dirs) {
		QDir root(root_path);
		if (root_path.isEmpty() || !root.exists()) {
			continue;
		}

		QList<QDir> pending_dirs;
		const auto  root_subdirs =
		    root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
		for (const auto& subdir: root_subdirs) {
			pending_dirs.append(QDir(subdir.absoluteFilePath()));
		}

		while (!pending_dirs.isEmpty()) {
			QDir game_dir = pending_dirs.takeFirst();

			if (game_dir.exists(eboot_name)) {
				const QString game_path        = NormalizeGameDirectory(game_dir.absolutePath());
				const QString legacy_game_path = root.relativeFilePath(game_dir.absolutePath());
				const QString game_key         = PathKey(game_path);
				if (game_key.isEmpty() || found_games.contains(game_key)) {
					continue;
				}
				found_games.insert(game_key);

				auto metadata = GetGameMetadata(
				    game_dir.filePath(QStringLiteral("sce_sys/param.json")), game_dir.dirName());

				auto  info   = std::make_unique<Configuration>();
				auto* custom = FindCustomInfo(&m_custom_infos, game_path, legacy_game_path);
				if (custom != nullptr) {
					info->CopyFrom(*custom);
					info->custom_settings = true;
				} else {
					info->CopyEmulatorSettingsFrom(m_global_info);
					info->custom_settings = false;
				}

				SetGameFiles(*info, game_dir.absolutePath(), game_path, metadata);
				const auto* compatibility = m_compatibility->Find(info->title_id);
				if (compatibility != nullptr) {
					info->game_status  = compatibility->status;
					info->game_comment = compatibility->comment;
				}

				auto* item = new ConfigurationItem(std::move(info), m_ui->cfgs_list);
				item->SetCompatibilityEditable(m_compatibility->IsLocal() &&
				                               !item->GetInfo().title_id.trimmed().isEmpty());
				connect(item->GetStatusCombo(), &QComboBox::currentIndexChanged, this,
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
				connect(item->GetCommentEdit(), &QLineEdit::editingFinished, this, [this, item]() {
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
				continue;
			}

			const auto subdirs =
			    game_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
			for (const auto& subdir: subdirs) {
				pending_dirs.append(QDir(subdir.absoluteFilePath()));
			}
		}
	}

	m_ui->cfgs_list->sortItems(GAME_NAME_COLUMN, Qt::AscendingOrder);
	filter_configurations(m_ui->search_line_edit->text());
}

void ConfigurationListWidget::edit_configuration() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr) {
		return;
	}

	ConfigurationEditDialog dlg(item->GetInfo(), this);
	dlg.SetTitle(tr("Edit game settings"));

	if (dlg.exec() == QDialog::Accepted) {
		item->GetInfo().custom_settings = true;
		auto game_path                  = item->GetInfo().game_path;
		delete m_custom_infos.take(game_path);
		m_custom_infos.insert(game_path, CloneConfiguration(item->GetInfo()));
		WriteSettings();
		ScanGameDirectory();
	}
}

void ConfigurationListWidget::delete_configuartion() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr || !item->GetInfo().custom_settings) {
		return;
	}

	if (QMessageBox::Yes == QMessageBox::question(this, tr("Clear custom settings"),
	                                              tr("Do you want to clear custom settings?"))) {
		ClearCustomSettings(item);
		WriteSettings();
		ScanGameDirectory();
	}
}

void ConfigurationListWidget::edit_global_settings() {
	Configuration info;
	info.CopyEmulatorSettingsFrom(m_global_info);
	info.name = tr("Global settings");

	ConfigurationEditDialog dlg(info, this);
	dlg.SetTitle(tr("Global settings"));
	dlg.SetGameDirectories(m_game_dirs);

	if (dlg.exec() == QDialog::Accepted) {
		m_global_info.CopyEmulatorSettingsFrom(info);
		m_game_dirs = NormalizeGameDirectories(dlg.GetGameDirectories());
		WriteSettings();
		ScanGameDirectory();
	}
}

void ConfigurationListWidget::ClearCustomSettings(ConfigurationItem* item) {
	delete m_custom_infos.take(item->GetInfo().game_path);
}

void ConfigurationListWidget::run_configuration() {
	emit Run();
}

bool ConfigurationListWidget::CanViewSelectedTrophies() const {
	return m_selected_item != nullptr &&
	       TrophyViewerDialog::HasTrophyData(&m_selected_item->GetInfo());
}

void ConfigurationListWidget::ViewTrophies() {
	auto* current = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	auto* item    = current != nullptr ? current : m_selected_item;
	if (item == nullptr) {
		return;
	}

	TrophyViewerDialog::ShowForGame(&item->GetInfo(), this);
}

void ConfigurationListWidget::open_game_folder() {
	auto* item = static_cast<ConfigurationItem*>(m_ui->cfgs_list->currentItem());
	if (item == nullptr) {
		return;
	}

	const QDir game_dir(item->GetInfo().basedir);
	if (!game_dir.exists()) {
		QMessageBox::warning(this, tr("Open game folder"), tr("Game folder does not exist."));
		return;
	}

	if (!QDesktopServices::openUrl(QUrl::fromLocalFile(game_dir.absolutePath()))) {
		QMessageBox::warning(this, tr("Open game folder"), tr("Could not open game folder."));
	}
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
	m_ui->cfgs_list->SetBackgroundImage(GetPic0Path(item->GetInfo()));

	emit Select();
}

void ConfigurationListWidget::list_currentItemChanged(QTreeWidgetItem* current,
                                                      QTreeWidgetItem* /*previous*/) {
	SelectItem(current);
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

	QAction* action_run = menu.addAction(tr("Run"), this, SLOT(run_configuration()));
	QAction* action_open_folder =
	    menu.addAction(style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Open game folder"), this,
	                   SLOT(open_game_folder()));
	QAction* action_view_trophies = menu.addAction(
	    style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("View trophies..."));
	connect(action_view_trophies, &QAction::triggered, this,
	        &ConfigurationListWidget::ViewTrophies);
	QAction* action_remove_save_data =
	    menu.addAction(style()->standardIcon(QStyle::SP_DialogDiscardButton),
	                   tr("Remove save data..."), this, SLOT(remove_save_data()));
	menu.addSeparator();
	QAction* action_edit =
	    menu.addAction(style()->standardIcon(QStyle::SP_FileIcon), tr("Edit game settings..."),
	                   this, SLOT(edit_configuration()));
	QAction* action_delete =
	    menu.addAction(style()->standardIcon(QStyle::SP_DialogDiscardButton),
	                   tr("Clear custom settings"), this, SLOT(delete_configuartion()));

	if (item == nullptr) {
		menu.addSeparator();
		/*QAction *action_global = */ menu.addAction(
		    style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Global settings..."),
		    this, SLOT(edit_global_settings()));
	}

	if (item != nullptr) {
		action_run->setDisabled(item->IsRunning());
		action_open_folder->setDisabled(!QDir(item->GetInfo().basedir).exists());
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
