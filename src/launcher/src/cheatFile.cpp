#include "cheatFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QXmlStreamReader>

extern "C" {
#include <libavutil/aes.h>
#include <libavutil/mem.h>
}

namespace Cheats {

namespace {

QString Tr(const char* text) {
	return QCoreApplication::translate("Cheats", text);
}

QString ValidateDocument(const QJsonDocument& document) {
	if (!document.isObject()) {
		return Tr("Invalid cheat JSON.");
	}
	const auto mods = document.object().value(QStringLiteral("mods"));
	if (!mods.isArray()) {
		return Tr("Cheat JSON has no \"mods\" array.");
	}
	for (const auto& value: mods.toArray()) {
		const auto mod     = value.toObject();
		const auto name    = mod.value(QStringLiteral("name"));
		const auto enabled = mod.value(QStringLiteral("enabled"));
		if (!value.isObject() || !name.isString() || name.toString().isEmpty() ||
		    (!enabled.isUndefined() && !enabled.isBool())) {
			return Tr("Invalid cheat entry.");
		}
	}
	return {};
}

// MC4 is base64 of AES-256-CBC with a fixed key and IV. The XML reader stops at the root end tag,
// before the padding.
void Decrypt(QByteArray* data) {
	static const uint8_t key[] = "304c6528f659c766110239a51cl5dd9c";
	uint8_t              iv[]  = "u@}kzW2u[u(8DWar";
	*data                      = QByteArray::fromBase64(*data);
	auto* aes                  = av_aes_alloc();
	auto* blocks               = reinterpret_cast<uint8_t*>(data->data());
	av_aes_init(aes, key, 256, 1);
	av_aes_crypt(aes, blocks, blocks, static_cast<int>(data->size() / 16), iv, 1);
	av_free(aes);
}

} // namespace

QString ConvertTrainer(QByteArray* data, bool encrypted) {
	if (encrypted) {
		Decrypt(data);
	}
	QXmlStreamReader xml(*data);
	if (!xml.readNextStartElement() || xml.name() != u"Trainer") {
		return Tr("Invalid cheat XML.");
	}
	const auto trainer = xml.attributes();
	QJsonArray mods;
	while (xml.readNextStartElement()) {
		if (xml.name() != u"Cheat") {
			xml.skipCurrentElement();
			continue;
		}
		QJsonObject mod {{QStringLiteral("name"), xml.attributes().value(u"Text").toString()}};
		QJsonArray  memory;
		while (xml.readNextStartElement()) {
			QJsonObject write;
			while (xml.readNextStartElement()) {
				const auto field = xml.name().toString();
				auto       value = xml.readElementText().trimmed();
				if (field == u"Offset") {
					write.insert(QStringLiteral("offset"), value);
				} else if (field == u"ValueOn") {
					write.insert(QStringLiteral("on"), value.remove(QLatin1Char('-')));
				} else if (field == u"ValueOff") {
					write.insert(QStringLiteral("off"), value.remove(QLatin1Char('-')));
				} else if (field == u"Section" && value != u"0") {
					// Section-relative and absolute writes are rejected on import.
					write.insert(QStringLiteral("section"), value);
				} else if (field == u"Absolute") {
					write.insert(QStringLiteral("absolute"), true);
				}
			}
			memory.append(write);
		}
		mod.insert(QStringLiteral("memory"), memory);
		mods.append(mod);
	}
	if (xml.hasError()) {
		return Tr("Invalid cheat XML.");
	}
	const QJsonObject root {
	    {QStringLiteral("name"), trainer.value(u"Game").toString()},
	    {QStringLiteral("id"), trainer.value(u"Cusa").toString()},
	    {QStringLiteral("version"), trainer.value(u"Version").toString()},
	    {QStringLiteral("process"), trainer.value(u"Process").toString()},
	    {QStringLiteral("credits"), QJsonArray {trainer.value(u"Moder").toString()}},
	    {QStringLiteral("mods"), mods}};
	*data = QJsonDocument(root).toJson();
	return {};
}

bool IsSupportedTitleId(const QString& title_id) {
	static const QRegularExpression pattern(QStringLiteral("\\APPSA[0-9]{5}\\z"));
	return pattern.match(title_id.trimmed().toUpper()).hasMatch();
}

QString PlanPath(const QString& title_id) {
	if (!IsSupportedTitleId(title_id)) {
		return {};
	}
	return QDir(QCoreApplication::applicationDirPath())
	    .filePath(QStringLiteral("_Patches/%1.json").arg(title_id.trimmed().toUpper()));
}

QString ParseDocument(const QByteArray& data, QJsonDocument* document) {
	const auto parsed = QJsonDocument::fromJson(data);
	const auto error  = ValidateDocument(parsed);
	*document         = error.isEmpty() ? parsed : QJsonDocument();
	return error;
}

QString ValidateImport(const QJsonDocument& document, const QString& title_id,
                       const QString& version, const QString& process) {
	const auto parse_error = ValidateDocument(document);
	if (!parse_error.isEmpty()) {
		return parse_error;
	}
	const auto root = document.object();
	if (!IsSupportedTitleId(title_id) ||
	    root.value(QStringLiteral("id"))
	            .toString()
	            .compare(title_id.trimmed(), Qt::CaseInsensitive) != 0) {
		return Tr("The cheat file does not match this game's title ID.");
	}
	if (version.isEmpty() || root.value(QStringLiteral("version")).toString() != version) {
		return Tr("The cheat file does not match this game's version (%1).").arg(version);
	}
	if (process.isEmpty() ||
	    root.value(QStringLiteral("process")).toString().compare(process, Qt::CaseInsensitive) !=
	        0) {
		return Tr("The cheat file does not match this game's executable (%1).").arg(process);
	}
	static const QRegularExpression offset_pattern(
	    QStringLiteral("\\A(?:0[xX])?[0-9a-fA-F]{1,16}\\z"));
	static const QRegularExpression bytes_pattern(QStringLiteral("\\A(?:[0-9a-fA-F]{2})+\\z"));
	const auto                      mods = root.value(QStringLiteral("mods")).toArray();
	if (mods.isEmpty()) {
		return Tr("The cheat file contains no cheats.");
	}
	if (root.contains(QStringLiteral("master"))) {
		return Tr("This JSON uses master codes, which are not supported yet.");
	}
	for (const auto& value: mods) {
		const auto mod    = value.toObject();
		const auto memory = mod.value(QStringLiteral("memory"));
		if (mod.contains(QStringLiteral("module_name"))) {
			return Tr("This JSON uses module-specific cheats, which are not supported yet.");
		}
		if (!memory.isArray() || memory.toArray().isEmpty()) {
			return Tr("Unsupported memory entries in cheat \"%1\".")
			    .arg(mod.value(QStringLiteral("name")).toString());
		}
		for (const auto& entry: memory.toArray()) {
			const auto write    = entry.toObject();
			const auto offset   = write.value(QStringLiteral("offset")).toString();
			const auto off      = write.value(QStringLiteral("off")).toString();
			const auto on       = write.value(QStringLiteral("on")).toString();
			const auto absolute = write.value(QStringLiteral("absolute"));
			const auto expected = write.value(QStringLiteral("expected"));
			if ((!absolute.isUndefined() && (!absolute.isBool() || absolute.toBool())) ||
			    (!expected.isUndefined() &&
			     (!expected.isString() ||
			      expected.toString().compare(off, Qt::CaseInsensitive) != 0)) ||
			    write.contains(QStringLiteral("section")) ||
			    write.contains(QStringLiteral("sectionName")) ||
			    write.contains(QStringLiteral("sectionProtection")) ||
			    write.contains(QStringLiteral("logicalOffset")) ||
			    write.contains(QStringLiteral("sectionOffset"))) {
				return Tr(
				    "This cheat file uses memory addressing features that are not supported yet.");
			}
			if (!entry.isObject() || !offset_pattern.match(offset).hasMatch() ||
			    !bytes_pattern.match(off).hasMatch() || !bytes_pattern.match(on).hasMatch() ||
			    off.size() != on.size()) {
				return Tr("Unsupported memory entries in cheat \"%1\".")
				    .arg(mod.value(QStringLiteral("name")).toString());
			}
		}
	}
	return {};
}

QJsonDocument WithSelection(const QJsonDocument& document, const QList<bool>& enabled) {
	auto root = document.object();
	auto mods = root.value(QStringLiteral("mods")).toArray();
	if (mods.size() != enabled.size()) {
		return {};
	}
	for (qsizetype index = 0; index < mods.size(); ++index) {
		auto mod = mods[index].toObject();
		mod.insert(QStringLiteral("enabled"), enabled[index]);
		mods[index] = mod;
	}
	root.insert(QStringLiteral("mods"), mods);
	return QJsonDocument(root);
}

QString SaveDocument(const QString& path, const QJsonDocument& document,
                     const QByteArray& expected_data) {
	if (path.isEmpty() || !document.isObject()) {
		return Tr("Invalid cheat file.");
	}
	QFile input(path);
	if (input.exists()) {
		if (!input.open(QIODevice::ReadOnly)) {
			return Tr("Could not read cheat file: %1").arg(input.errorString());
		}
		const auto current = input.readAll();
		if (input.error() != QFile::NoError) {
			return Tr("Could not read cheat file: %1").arg(input.errorString());
		}
		if (current != expected_data) {
			return Tr("Cheat file changed; reload the dialog.");
		}
	} else if (!expected_data.isEmpty()) {
		return Tr("Cheat file changed; reload the dialog.");
	}
	input.close();
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return Tr("Could not create the local cheats directory.");
	}
	QSaveFile  output(path);
	const auto data = document.toJson();
	if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() ||
	    !output.commit()) {
		return Tr("Could not save cheat file: %1").arg(output.errorString());
	}
	return {};
}

} // namespace Cheats
