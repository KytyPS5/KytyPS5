#include "trophyViewerDialog.h"

#include "common/archive.h"
#include "configuration.h"
#include "gameContent.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QByteArray>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QObject>
#include <QPixmap>
#include <QRegularExpression>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <QtEndian>

#include <utility>

namespace {

constexpr quint32 UCP_MAGIC              = 0xb228c60a;
constexpr quint32 UCP_VERSION            = 1;
constexpr int     UCP_HEADER_LEN         = 0x40;
constexpr int     UCP_TOC_SKIP           = 0x20;
constexpr int     UCP_ENTRY_LEN          = 0x40;
constexpr int     UCP_NAME_LEN           = 0x20;
constexpr quint32 UCP_MAX_FILES          = 4096;
constexpr quint64 UCP_MAX_EXTRACTED_SIZE = quint64 {64} << 20u;

struct UcpEntry {
	QString name;
	quint64 offset = 0;
	quint64 size   = 0;
};

struct TrophyDefinition {
	QString id;
	QString grade;
	bool    hidden     = false;
	bool    has_reward = false;
};

struct TrophyText {
	QString name;
	QString detail;
	QString reward;
};

struct TrophyRow {
	QString id;
	QString name;
	QString detail;
	QString grade;
	QString reward;
	bool    hidden     = false;
	bool    has_reward = false;
	QPixmap icon;
};

struct TrophySet {
	QString          tab_title;
	QList<TrophyRow> trophies;
};

static bool CanRead(const QByteArray& data, qsizetype offset, qsizetype size) {
	return offset >= 0 && size >= 0 && offset <= data.size() && size <= data.size() - offset;
}

static QString ReadFixedString(const QByteArray& data, qsizetype offset, qsizetype max_size) {
	if (!CanRead(data, offset, max_size)) {
		return {};
	}

	qsizetype len = 0;
	while (len < max_size && data.at(offset + len) != '\0') {
		len++;
	}

	return QString::fromLatin1(data.constData() + offset, len);
}

static bool IsUsedUcpEntry(const QString& name) {
	const auto lower = name.toCaseFolded();
	return lower == QStringLiteral("tropconf.json") || lower == QStringLiteral("tropmeta.json") ||
	       (lower.startsWith(QStringLiteral("tropmeta_")) &&
	        lower.endsWith(QStringLiteral(".json"))) ||
	       (lower.startsWith(QStringLiteral("trop")) && lower.endsWith(QStringLiteral(".png")));
}

static bool ReadUcp(const QString& file_name, QMap<QString, QByteArray>& files, QString& error) {
	const QByteArray data =
	    GameContent::ReadPath(GameContent::ToPath(file_name), GameContent::MaxTrophyPackageSize);
	if (data.isEmpty()) {
		error = QObject::tr("Could not open %1").arg(QDir::toNativeSeparators(file_name));
		return false;
	}

	if (data.size() < UCP_HEADER_LEN) {
		error = QObject::tr("%1 is too small to be a trophy package.")
		            .arg(QFileInfo(file_name).fileName());
		return false;
	}

	const auto magic = qFromBigEndian<quint32>(data.constData() + 0x00);
	if (magic != UCP_MAGIC) {
		error = QObject::tr("%1 has an invalid trophy package magic.")
		            .arg(QFileInfo(file_name).fileName());
		return false;
	}

	const auto version = qFromBigEndian<quint32>(data.constData() + 0x04);
	if (version != UCP_VERSION) {
		error = QObject::tr("%1 uses unsupported trophy package version %2.")
		            .arg(QFileInfo(file_name).fileName(), QString::number(version));
		return false;
	}

	const auto declared_size = qFromBigEndian<quint64>(data.constData() + 0x08);
	if (declared_size < UCP_HEADER_LEN || declared_size > static_cast<quint64>(data.size())) {
		error = QObject::tr("%1 is truncated.").arg(QFileInfo(file_name).fileName());
		return false;
	}

	const auto file_count = qFromBigEndian<quint32>(data.constData() + 0x10);
	if (file_count > UCP_MAX_FILES) {
		error = QObject::tr("%1 contains too many files.").arg(QFileInfo(file_name).fileName());
		return false;
	}
	const auto toc_offset = static_cast<quint64>(qFromBigEndian<quint32>(data.constData() + 0x14));
	const auto data_size  = declared_size;

	const quint64 table_size = UCP_TOC_SKIP + static_cast<quint64>(file_count) * UCP_ENTRY_LEN;
	if (toc_offset > data_size || table_size > data_size - toc_offset ||
	    !std::in_range<qsizetype>(toc_offset + table_size)) {
		error = QObject::tr("%1 has an invalid table of contents.")
		            .arg(QFileInfo(file_name).fileName());
		return false;
	}

	quint64 extracted_size = 0;
	for (quint32 i = 0; i < file_count; i++) {
		const auto entry_offset = static_cast<qsizetype>(toc_offset + UCP_TOC_SKIP +
		                                                 static_cast<quint64>(i) * UCP_ENTRY_LEN);
		UcpEntry   entry;
		entry.name   = ReadFixedString(data, entry_offset, UCP_NAME_LEN).trimmed();
		entry.offset = qFromBigEndian<quint64>(data.constData() + entry_offset + 0x20);
		entry.size   = qFromBigEndian<quint64>(data.constData() + entry_offset + 0x28);

		if (entry.name.isEmpty()) {
			continue;
		}
		if (entry.offset > data_size || entry.size > data_size - entry.offset ||
		    !std::in_range<qsizetype>(entry.offset) || !std::in_range<qsizetype>(entry.size)) {
			error = QObject::tr("%1 has an invalid entry for %2.")
			            .arg(QFileInfo(file_name).fileName(), entry.name);
			return false;
		}
		if (!IsUsedUcpEntry(entry.name)) {
			continue;
		}

		const auto entry_limit = entry.name.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)
		                             ? GameContent::MaxImageSize
		                             : GameContent::MaxMetadataSize;
		if (entry.size > entry_limit || entry.size > UCP_MAX_EXTRACTED_SIZE - extracted_size) {
			error = QObject::tr("%1 contains an oversized entry for %2.")
			            .arg(QFileInfo(file_name).fileName(), entry.name);
			return false;
		}

		files.insert(entry.name.toCaseFolded(), data.mid(static_cast<qsizetype>(entry.offset),
		                                                 static_cast<qsizetype>(entry.size)));
		extracted_size += entry.size;
	}

	return true;
}

static const QByteArray* FindFile(const QMap<QString, QByteArray>& files, const QString& name) {
	const auto it = files.constFind(name.toCaseFolded());
	return it == files.constEnd() ? nullptr : &it.value();
}

static const QByteArray* FindFirstTrophyMetadataFile(const QMap<QString, QByteArray>& files) {
	for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
		if (it.key().startsWith(QStringLiteral("tropmeta_")) &&
		    it.key().endsWith(QStringLiteral(".json"))) {
			return &it.value();
		}
	}

	return FindFile(files, QStringLiteral("tropmeta.json"));
}

static const QByteArray* FindMetadataForLanguage(const QMap<QString, QByteArray>& files,
                                                 const QString& locale) {
	const auto normalize_locale = [](QString value) {
		value = value.trimmed().toCaseFolded();
		value.replace(QLatin1Char('_'), QLatin1Char('-'));
		return value;
	};
	const auto desired_locale = normalize_locale(locale);
	const auto separator = desired_locale.indexOf(QLatin1Char('-'));
	const auto language = separator < 0 ? desired_locale : desired_locale.left(separator);
	const QByteArray* language_match = nullptr;
	for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
		if (!it.key().startsWith(QStringLiteral("tropmeta_")) ||
		    !it.key().endsWith(QStringLiteral(".json"))) {
			continue;
		}
		const auto file_locale =
		    normalize_locale(it.key().mid(9, it.key().size() - 14));
		if (file_locale == desired_locale) {
			return &it.value();
		}
		const auto file_separator = file_locale.indexOf(QLatin1Char('-'));
		const auto file_language =
		    file_separator < 0 ? file_locale : file_locale.left(file_separator);
		if (language_match == nullptr && file_language == language) {
			language_match = &it.value();
		}
	}
	return language_match;
}

static QString CanonicalTrophyId(const QString& id) {
	bool        valid = false;
	const auto numeric_id = id.trimmed().toULongLong(&valid);
	return valid ? QString::number(numeric_id) : id.trimmed();
}

static bool ReadJsonObject(const QByteArray& data, const QString& file_name, QJsonObject& object,
                           QString& error) {
	QJsonParseError parse_error;
	const auto      doc = QJsonDocument::fromJson(data, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		error = QObject::tr("Could not read %1: %2").arg(file_name, parse_error.errorString());
		return false;
	}

	object = doc.object();
	return true;
}

static QString JsonString(const QJsonValue& value) {
	if (value.isString()) {
		return value.toString();
	}
	if (value.isDouble()) {
		const double number = value.toDouble();
		if (number == static_cast<int>(number)) {
			return QString::number(static_cast<int>(number));
		}
		return QString::number(number);
	}
	if (value.isBool()) {
		return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
	}

	return {};
}

static QList<TrophyDefinition> ReadDefinitions(const QJsonObject& tropconf) {
	QList<TrophyDefinition> ret;

	const auto trophies = tropconf.value(QStringLiteral("trophies")).toArray();
	for (const auto& value: trophies) {
		const auto       obj = value.toObject();
		TrophyDefinition def;
		def.id         = JsonString(obj.value(QStringLiteral("id"))).trimmed();
		def.grade      = JsonString(obj.value(QStringLiteral("grade"))).trimmed();
		def.hidden     = obj.value(QStringLiteral("hidden")).toBool(false);
		def.has_reward = obj.value(QStringLiteral("hasReward")).toBool(false);

		if (!def.id.isEmpty()) {
			ret.append(def);
		}
	}

	return ret;
}

static QMap<QString, TrophyText> ReadMetadata(const QJsonObject& tropmeta) {
	QMap<QString, TrophyText> ret;

	const auto metadata = tropmeta.value(QStringLiteral("metadata")).toObject();
	const auto trophies = metadata.value(QStringLiteral("trophyMetadata")).toArray();
	for (const auto& value: trophies) {
		const auto obj = value.toObject();
		const auto id  = JsonString(obj.value(QStringLiteral("id"))).trimmed();
		if (id.isEmpty()) {
			continue;
		}

		TrophyText text;
		text.name   = JsonString(obj.value(QStringLiteral("name"))).trimmed();
		text.detail = JsonString(obj.value(QStringLiteral("detail"))).trimmed();
		text.reward = JsonString(obj.value(QStringLiteral("reward"))).trimmed();
		ret.insert(id, text);
	}

	return ret;
}

static QString GradeToText(const QString& grade) {
	if (grade == QStringLiteral("P")) {
		return QObject::tr("Platinum");
	}
	if (grade == QStringLiteral("G")) {
		return QObject::tr("Gold");
	}
	if (grade == QStringLiteral("S")) {
		return QObject::tr("Silver");
	}
	if (grade == QStringLiteral("B")) {
		return QObject::tr("Bronze");
	}

	return grade;
}

static QPixmap LoadTrophyIcon(const QMap<QString, QByteArray>& files, const QString& id) {
	QStringList names;
	names.append(QStringLiteral("trop%1.png").arg(id));

	bool      id_is_number = false;
	const int id_num       = id.toInt(&id_is_number);
	if (id_is_number) {
		names.append(QStringLiteral("trop%1.png").arg(id_num, 4, 10, QLatin1Char('0')));
	}

	for (const auto& name: names) {
		const auto* data = FindFile(files, name);
		if (data == nullptr) {
			continue;
		}

		QPixmap pixmap;
		if (pixmap.loadFromData(*data)) {
			return pixmap;
		}
	}

	return {};
}

static QString TrophyTabTitle(const QString& file_name) {
	static const QRegularExpression trophy_file_re(QStringLiteral("^trophy(\\d+)\\.ucp$"),
	                                               QRegularExpression::CaseInsensitiveOption);

	const auto name  = QFileInfo(file_name).fileName();
	const auto match = trophy_file_re.match(name);
	if (match.hasMatch()) {
		return QObject::tr("Trophy %1").arg(match.captured(1));
	}

	return QFileInfo(file_name).completeBaseName();
}

static QString ConsoleLanguageLocale(int language) {
	static const QStringList locales = {
	    QStringLiteral("ja-JP"), QStringLiteral("en-US"), QStringLiteral("fr-FR"),
	    QStringLiteral("es-ES"), QStringLiteral("de-DE"), QStringLiteral("it-IT"),
	    QStringLiteral("nl-NL"), QStringLiteral("pt-PT"), QStringLiteral("ru-RU"),
	    QStringLiteral("ko-KR"), QStringLiteral("zh-Hant"), QStringLiteral("zh-Hans"),
	    QStringLiteral("fi-FI"), QStringLiteral("sv-SE"), QStringLiteral("da-DK"),
	    QStringLiteral("no-NO"), QStringLiteral("pl-PL"), QStringLiteral("pt-BR"),
	    QStringLiteral("en-GB"), QStringLiteral("tr-TR"), QStringLiteral("es-419"),
	    QStringLiteral("ar-AE"), QStringLiteral("fr-CA"), QStringLiteral("cs-CZ"),
	    QStringLiteral("hu-HU"), QStringLiteral("el-GR"), QStringLiteral("ro-RO"),
	    QStringLiteral("th-TH"), QStringLiteral("vi-VN"), QStringLiteral("id-ID"),
	};
	return language >= 0 && language < locales.size() ? locales.at(language)
	                                                  : QStringLiteral("en-US");
}

static bool BuildTrophySet(const QString& ucp_file, int console_language, TrophySet& set,
                           QString& error) {
	QMap<QString, QByteArray> files;
	if (!ReadUcp(ucp_file, files, error)) {
		return false;
	}

	const auto* conf_data = FindFile(files, QStringLiteral("tropconf.json"));
	if (conf_data == nullptr) {
		error =
		    QObject::tr("%1 does not contain tropconf.json.").arg(QFileInfo(ucp_file).fileName());
		return false;
	}

	QJsonObject tropconf;
	if (!ReadJsonObject(*conf_data, QStringLiteral("tropconf.json"), tropconf, error)) {
		return false;
	}

	const auto default_language =
	    JsonString(tropconf.value(QStringLiteral("defaultLanguage"))).trimmed();
	const auto locale = ConsoleLanguageLocale(console_language);
	const QByteArray* meta_data = FindMetadataForLanguage(files, locale);
	if (!default_language.isEmpty()) {
		if (meta_data == nullptr) {
			meta_data = FindMetadataForLanguage(files, default_language);
		}
	}
	if (meta_data == nullptr) {
		meta_data = FindMetadataForLanguage(files, QStringLiteral("en-US"));
	}
	if (meta_data == nullptr) {
		meta_data = FindFirstTrophyMetadataFile(files);
	}
	if (meta_data == nullptr) {
		error = QObject::tr("%1 does not contain readable trophy metadata.")
		            .arg(QFileInfo(ucp_file).fileName());
		return false;
	}

	QJsonObject tropmeta;
	if (!ReadJsonObject(*meta_data, QStringLiteral("tropmeta.json"), tropmeta, error)) {
		return false;
	}

	const auto definitions = ReadDefinitions(tropconf);
	if (definitions.isEmpty()) {
		error = QObject::tr("%1 does not define any trophies.").arg(QFileInfo(ucp_file).fileName());
		return false;
	}

	const auto texts = ReadMetadata(tropmeta);

	set.tab_title = TrophyTabTitle(ucp_file);
	for (const auto& def: definitions) {
		const auto text = texts.value(def.id);

		TrophyRow row;
		row.id         = def.id;
		row.grade      = def.grade;
		row.hidden     = def.hidden;
		row.has_reward = def.has_reward;
		row.reward     = text.reward;
		row.name       = text.name;
		row.detail     = text.detail;
		row.icon       = LoadTrophyIcon(files, def.id);

		if (row.name.isEmpty()) {
			row.name =
			    row.hidden ? QObject::tr("Hidden Trophy") : QObject::tr("Trophy %1").arg(def.id);
		}
		if (row.detail.isEmpty() && row.hidden) {
			row.detail = QObject::tr("This trophy is hidden.");
		}

		set.trophies.append(row);
	}

	return true;
}

static QStringList FindTrophyFiles(const Configuration* info) {
	if (info == nullptr || info->basedir.isEmpty()) {
		return {};
	}

	QStringList files;
	for (const auto& file:
	     GameContent::ListFiles(info->basedir, QStringLiteral("sce_sys/trophy2"))) {
		const auto name = QFileInfo(file).fileName();
		if (name.startsWith(QStringLiteral("trophy"), Qt::CaseInsensitive) &&
		    name.endsWith(QStringLiteral(".ucp"), Qt::CaseInsensitive)) {
			files.append(file);
		}
	}
	files.sort(Qt::CaseInsensitive);

	QStringList   trophy_files;
	QSet<QString> seen;
	for (const auto& file: files) {
		auto key = QFileInfo(file).canonicalFilePath();
		if (key.isEmpty()) {
			key = file;
		}
		key = QDir::cleanPath(key).toCaseFolded();

		if (!seen.contains(key)) {
			seen.insert(key);
			trophy_files.append(file);
		}
	}
	return trophy_files;
}

static QSet<QString> LoadUnlockedTrophies(const Configuration& info) {
	QSet<QString> unlocked;
	const auto title_id = info.title_id.trimmed();
	if (title_id.isEmpty() ||
	    !std::all_of(title_id.begin(), title_id.end(), [](QChar c) {
		    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-');
	    })) {
		return unlocked;
	}

	QStringList roots {QDir::currentPath(), QCoreApplication::applicationDirPath()};
	QDir current_parent(QDir::currentPath());
	if (current_parent.cdUp()) {
		roots.append(current_parent.absolutePath());
	}
	QDir application_parent(QCoreApplication::applicationDirPath());
	if (application_parent.cdUp()) {
		roots.append(application_parent.absolutePath());
	}

	for (const auto& root: roots) {
		const auto path = QDir(root).filePath(
		    QStringLiteral("_SaveData/%1/trophies_%2.json").arg(title_id).arg(info.user_id));
		const auto bytes = GameContent::ReadPath(GameContent::ToPath(path), uint64_t {1} << 20u);
		if (bytes.isEmpty()) {
			continue;
		}
		QJsonParseError parse_error;
		const auto document = QJsonDocument::fromJson(bytes, &parse_error);
		if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
			qWarning("Could not parse saved trophy unlocks from %s: %s",
			         QDir::toNativeSeparators(path).toUtf8().constData(),
			         parse_error.errorString().toUtf8().constData());
			continue;
		}
		const auto entries = document.object().value(QStringLiteral("unlockedTrophies")).toArray();
		for (const auto& entry: entries) {
			const auto id = entry.isString()
			                    ? entry.toString().trimmed()
			                    : (entry.isDouble() ? QString::number(entry.toInt()) : QString {});
			if (!id.isEmpty()) {
				unlocked.insert(CanonicalTrophyId(id));
			}
		}
		return unlocked;
	}
	return unlocked;
}

static void PrepareTable(QTableWidget* table) {
	table->setObjectName(QStringLiteral("trophy_table"));
	table->setColumnCount(4);
	table->setHorizontalHeaderLabels({QObject::tr("Unlocked"), QObject::tr("Trophy"),
	                                  QObject::tr("Name"), QObject::tr("Description")});
	table->setAlternatingRowColors(false);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setMouseTracking(false);
	table->viewport()->setMouseTracking(false);
	table->setAttribute(Qt::WA_Hover, false);
	table->viewport()->setAttribute(Qt::WA_Hover, false);
	table->setIconSize(QSize(96, 96));
	table->setShowGrid(false);
	table->setWordWrap(true);
	table->setStyleSheet(QStringLiteral(
	    "QTableWidget#trophy_table::item:selected {"
	    " background-color: #0878d1; color: #ffffff;"
	    " border-top: 1px solid #8bd5ff; border-bottom: 1px solid #8bd5ff;"
	    "}"
	    "QTableWidget#trophy_table::item:focus {"
	    " border: 2px solid #ffd34e;"
	    "}"));
	table->verticalHeader()->setVisible(false);
	table->verticalHeader()->setDefaultSectionSize(112);
	table->horizontalHeader()->setStretchLastSection(true);
	table->horizontalHeader()->setHighlightSections(false);
	table->setColumnWidth(0, 100);
	table->setColumnWidth(1, 132);
	table->setColumnWidth(2, 260);
	table->setColumnWidth(3, 480);
}

static QTableWidgetItem* CreateItem(const QString& text) {
	auto* item = new QTableWidgetItem(text);
	item->setFlags(item->flags() & ~Qt::ItemIsEditable);
	return item;
}

static QString TrophyTooltip(const TrophyRow& row) {
	QStringList lines;
	lines.append(row.name);
	if (!row.detail.isEmpty()) {
		lines.append(row.detail);
	}

	const auto grade = GradeToText(row.grade);
	if (!grade.isEmpty()) {
		lines.append(QObject::tr("Grade: %1").arg(grade));
	}
	if (row.hidden) {
		lines.append(QObject::tr("Hidden trophy"));
	}
	if (row.has_reward && !row.reward.isEmpty()) {
		lines.append(QObject::tr("Reward: %1").arg(row.reward));
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

void TrophyViewerDialog::ShowForGame(const Configuration* info, QWidget* parent) {
	if (info == nullptr) {
		return;
	}

	TrophyViewerDialog dlg(parent);
	if (!info->name.isEmpty()) {
		dlg.setWindowTitle(tr("Trophy Viewer - %1").arg(info->name));
	}

	QString error;
	if (!dlg.LoadGame(*info, error)) {
		QMessageBox::warning(parent, tr("Trophy Viewer"), error);
		return;
	}

	dlg.exec();
}

bool TrophyViewerDialog::LoadGame(const Configuration& info, QString& error) {
	const auto reader = Common::OpenArchive(GameContent::ToPath(info.basedir));
	const auto trophy_files = FindTrophyFiles(&info);
	if (trophy_files.isEmpty()) {
		error = tr("No trophy package found in sce_sys/trophy2.");
		return false;
	}

	const auto unlocked_trophies = LoadUnlockedTrophies(info);
	QStringList errors;
	for (const auto& file: trophy_files) {
		TrophySet set;
		QString   set_error;
		if (!BuildTrophySet(file, info.console_language, set, set_error)) {
			errors.append(set_error);
			continue;
		}

		auto* table = new QTableWidget(set.trophies.size(), 4, m_tabs);
		PrepareTable(table);

		for (int row_index = 0; row_index < set.trophies.size(); row_index++) {
			const auto& row = set.trophies.at(row_index);
			table->setRowHeight(row_index, 112);

			const bool trophy_unlocked =
			    unlocked_trophies.contains(CanonicalTrophyId(row.id));
			const QString state_label =
			    trophy_unlocked ? tr("?  UNLOCKED") : tr("?  LOCKED");
			auto* status = CreateItem(state_label);
			status->setTextAlignment(Qt::AlignCenter);
			auto status_font = status->font();
			status_font.setBold(true);
			status->setFont(status_font);
			if (trophy_unlocked) {
				status->setForeground(QBrush(QColor(115, 255, 175)));
				status->setBackground(QBrush(QColor(20, 83, 55)));
			} else if (row.hidden) {
				status->setForeground(QBrush(QColor(185, 190, 200)));
				status->setBackground(QBrush(QColor(56, 58, 64)));
			} else {
				status->setForeground(QBrush(QColor(205, 210, 220)));
				status->setBackground(QBrush(QColor(50, 53, 60)));
			}
			table->setItem(row_index, 0, status);

			auto* icon_item = CreateItem({});
			icon_item->setTextAlignment(Qt::AlignCenter);
			if (!row.icon.isNull()) {
				auto trophy_icon = row.icon;
				if (!trophy_unlocked) {
					trophy_icon = QPixmap::fromImage(
					    trophy_icon.toImage().convertToFormat(QImage::Format_Grayscale8));
				}
				icon_item->setIcon(QIcon(trophy_icon));
			}
			table->setItem(row_index, 1, icon_item);

			auto* name = CreateItem(row.name);
			auto  font = name->font();
			font.setBold(true);
			name->setFont(font);
			name->setToolTip(TrophyTooltip(row));
			auto* detail = CreateItem(row.detail);
			detail->setToolTip(TrophyTooltip(row));

			const QBrush row_background(trophy_unlocked ? QColor(27, 54, 45)
			                                            : QColor(37, 39, 44));
			for (auto* item: {icon_item, name, detail}) {
				item->setBackground(row_background);
			}
			table->setItem(row_index, 2, name);
			table->setItem(row_index, 3, detail);
		}

		m_tabs->addTab(table, set.tab_title);
	}

	if (m_tabs->count() == 0) {
		error = errors.isEmpty() ? tr("No readable trophy data found.")
		                         : errors.join(QLatin1Char('\n'));
		return false;
	}

	return true;
}
