#include "trophyViewerDialog.h"

#include "configuration.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QByteArray>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
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

#include <limits>

namespace {

constexpr quint32 UCP_MAGIC      = 0xb228c60a;
constexpr quint32 UCP_VERSION    = 1;
constexpr int     UCP_HEADER_LEN = 0x40;
constexpr int     UCP_TOC_SKIP   = 0x20;
constexpr int     UCP_ENTRY_LEN  = 0x40;
constexpr int     UCP_NAME_LEN   = 0x20;

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

static quint32 ReadBe32(const QByteArray& data, qsizetype offset) {
	const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
	return (static_cast<quint32>(p[0]) << 24u) | (static_cast<quint32>(p[1]) << 16u) |
	       (static_cast<quint32>(p[2]) << 8u) | static_cast<quint32>(p[3]);
}

static quint64 ReadBe64(const QByteArray& data, qsizetype offset) {
	return (static_cast<quint64>(ReadBe32(data, offset)) << 32u) |
	       static_cast<quint64>(ReadBe32(data, offset + 4));
}

static bool FitsQSizeType(quint64 value) {
	return value <= static_cast<quint64>(std::numeric_limits<qsizetype>::max());
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

static bool ReadUcp(const QString& file_name, QMap<QString, QByteArray>& files, QString& error) {
	QFile file(file_name);
	if (!file.open(QIODevice::ReadOnly)) {
		error = QObject::tr("Could not open %1").arg(QDir::toNativeSeparators(file_name));
		return false;
	}

	const QByteArray data = file.readAll();
	if (data.size() < UCP_HEADER_LEN) {
		error = QObject::tr("%1 is too small to be a trophy package.")
		            .arg(QFileInfo(file_name).fileName());
		return false;
	}

	const auto magic = ReadBe32(data, 0x00);
	if (magic != UCP_MAGIC) {
		error = QObject::tr("%1 has an invalid trophy package magic.")
		            .arg(QFileInfo(file_name).fileName());
		return false;
	}

	const auto version = ReadBe32(data, 0x04);
	if (version != UCP_VERSION) {
		error = QObject::tr("%1 uses unsupported trophy package version %2.")
		            .arg(QFileInfo(file_name).fileName(), QString::number(version));
		return false;
	}

	const auto declared_size = ReadBe64(data, 0x08);
	if (declared_size > static_cast<quint64>(data.size())) {
		error = QObject::tr("%1 is truncated.").arg(QFileInfo(file_name).fileName());
		return false;
	}

	const auto file_count = ReadBe32(data, 0x10);
	const auto toc_offset = static_cast<quint64>(ReadBe32(data, 0x14));
	const auto data_size  = static_cast<quint64>(data.size());

	const quint64 table_size = UCP_TOC_SKIP + static_cast<quint64>(file_count) * UCP_ENTRY_LEN;
	if (toc_offset > data_size || table_size > data_size - toc_offset ||
	    !FitsQSizeType(toc_offset + table_size)) {
		error = QObject::tr("%1 has an invalid table of contents.")
		            .arg(QFileInfo(file_name).fileName());
		return false;
	}

	for (quint32 i = 0; i < file_count; i++) {
		const auto entry_offset = static_cast<qsizetype>(toc_offset + UCP_TOC_SKIP +
		                                                 static_cast<quint64>(i) * UCP_ENTRY_LEN);
		UcpEntry   entry;
		entry.name   = ReadFixedString(data, entry_offset, UCP_NAME_LEN).trimmed();
		entry.offset = ReadBe64(data, entry_offset + 0x20);
		entry.size   = ReadBe64(data, entry_offset + 0x28);

		if (entry.name.isEmpty()) {
			continue;
		}
		if (entry.offset > data_size || entry.size > data_size - entry.offset ||
		    !FitsQSizeType(entry.offset) || !FitsQSizeType(entry.size)) {
			error = QObject::tr("%1 has an invalid entry for %2.")
			            .arg(QFileInfo(file_name).fileName(), entry.name);
			return false;
		}

		files.insert(entry.name.toCaseFolded(), data.mid(static_cast<qsizetype>(entry.offset),
		                                                 static_cast<qsizetype>(entry.size)));
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

static bool BuildTrophySet(const QString& ucp_file, TrophySet& set, QString& error) {
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
	const QByteArray* meta_data = nullptr;
	if (!default_language.isEmpty()) {
		meta_data = FindFile(files, QStringLiteral("tropmeta_%1.json").arg(default_language));
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

	const QDir trophy_dir(QDir(info->basedir).filePath(QStringLiteral("sce_sys/trophy2")));
	if (!trophy_dir.exists()) {
		return {};
	}

	const auto files =
	    trophy_dir.entryInfoList({QStringLiteral("Trophy*.ucp"), QStringLiteral("trophy*.ucp")},
	                             QDir::Files | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase);

	QStringList   trophy_files;
	QSet<QString> seen;
	for (const auto& file: files) {
		auto key = file.canonicalFilePath();
		if (key.isEmpty()) {
			key = file.absoluteFilePath();
		}
		key = QDir::cleanPath(key).toCaseFolded();

		if (!seen.contains(key)) {
			seen.insert(key);
			trophy_files.append(file.absoluteFilePath());
		}
	}

	return trophy_files;
}

static void PrepareTable(QTableWidget* table) {
	table->setColumnCount(4);
	table->setHorizontalHeaderLabels({QObject::tr("Unlocked"), QObject::tr("Trophy"),
	                                  QObject::tr("Name"), QObject::tr("Description")});
	table->setAlternatingRowColors(true);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
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
	const auto trophy_files = FindTrophyFiles(&info);
	if (trophy_files.isEmpty()) {
		error = tr("No trophy package found in sce_sys/trophy2.");
		return false;
	}

	QStringList errors;
	for (const auto& file: trophy_files) {
		TrophySet set;
		QString   set_error;
		if (!BuildTrophySet(file, set, set_error)) {
			errors.append(set_error);
			continue;
		}

		auto* table = new QTableWidget(set.trophies.size(), 4, m_tabs);
		PrepareTable(table);

		for (int row_index = 0; row_index < set.trophies.size(); row_index++) {
			const auto& row = set.trophies.at(row_index);
			table->setRowHeight(row_index, 112);

			auto* status = CreateItem(tr("locked"));
			status->setTextAlignment(Qt::AlignCenter);
			if (row.hidden) {
				status->setForeground(QBrush(Qt::gray));
			}
			table->setItem(row_index, 0, status);

			auto* icon_label = new QLabel(table);
			icon_label->setAlignment(Qt::AlignCenter);
			icon_label->setMinimumSize(QSize(108, 108));
			if (!row.icon.isNull()) {
				icon_label->setPixmap(
				    row.icon.scaled(QSize(96, 96), Qt::KeepAspectRatio, Qt::SmoothTransformation));
			}
			table->setCellWidget(row_index, 1, icon_label);

			auto* name = CreateItem(row.name);
			auto  font = name->font();
			font.setBold(true);
			name->setFont(font);
			name->setToolTip(TrophyTooltip(row));
			table->setItem(row_index, 2, name);

			auto* detail = CreateItem(row.detail);
			detail->setToolTip(TrophyTooltip(row));
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
