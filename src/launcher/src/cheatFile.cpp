#include "cheatFile.h"

#include "configuration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
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
	const auto root = document.object();
	const auto doc_id = root.value(QStringLiteral("id")).toString().trimmed();
	if (!root.value(QStringLiteral("id")).isString() || doc_id.isEmpty() || !IsSupportedTitleId(doc_id) ||
	    !root.value(QStringLiteral("version")).isString() ||
	    root.value(QStringLiteral("version")).toString().trimmed().isEmpty() ||
	    !root.value(QStringLiteral("process")).isString() ||
	    root.value(QStringLiteral("process")).toString().trimmed().isEmpty()) {
		return Tr("Cheat JSON is missing required header fields (id, version, process).");
	}
	if (root.contains(QStringLiteral("master")) && root.value(QStringLiteral("master")).toBool(true)) {
		return Tr("This JSON uses master codes, which are not supported yet.");
	}
	const auto mods = root.value(QStringLiteral("mods"));
	if (!mods.isArray() || mods.toArray().isEmpty()) {
		return Tr("The cheat file contains no cheats.");
	}
	static const QRegularExpression offset_re(QStringLiteral("\\A(?:0[xX])?[0-9a-fA-F]{1,16}\\z"));
	static const QRegularExpression bytes_re(QStringLiteral("\\A(?:[0-9a-fA-F]{2})+\\z"));
	for (const auto& value: mods.toArray()) {
		if (!value.isObject()) {
			return Tr("Invalid cheat entry.");
		}
		const auto mod     = value.toObject();
		const auto name    = mod.value(QStringLiteral("name"));
		const auto enabled = mod.value(QStringLiteral("enabled"));
		const auto memory  = mod.value(QStringLiteral("memory"));
		if (!name.isString() || name.toString().trimmed().isEmpty() ||
		    (!enabled.isUndefined() && !enabled.isBool()) ||
		    !memory.isArray() || memory.toArray().isEmpty()) {
			return Tr("Invalid cheat entry.");
		}
		if (mod.contains(QStringLiteral("module_name"))) {
			const auto mod_name_val = mod.value(QStringLiteral("module_name"));
			if (!mod_name_val.isNull() && (!mod_name_val.isString() || !mod_name_val.toString().trimmed().isEmpty())) {
				return Tr("This JSON uses module-specific cheats, which are not supported yet.");
			}
		}
		for (const auto& entry_val: memory.toArray()) {
			if (!entry_val.isObject()) {
				return Tr("Invalid cheat entry.");
			}
			const auto entry   = entry_val.toObject();
			const auto off_str = entry.value(QStringLiteral("offset")).toString().trimmed();
			const auto off_val = entry.value(QStringLiteral("off")).toString().trimmed();
			const auto on_val  = entry.value(QStringLiteral("on")).toString().trimmed();
			if (!offset_re.match(off_str).hasMatch() ||
			    !bytes_re.match(off_val).hasMatch() ||
			    !bytes_re.match(on_val).hasMatch() ||
			    off_val.size() != on_val.size()) {
				return Tr("Invalid cheat entry.");
			}
			if (entry.contains(QStringLiteral("section"))) {
				const auto sec = entry.value(QStringLiteral("section"));
				if (!sec.isNull() && !sec.isUndefined()) {
					if (!sec.isDouble() && !sec.isString()) {
						return Tr("This cheat file uses memory addressing features that are not supported yet.");
					}
					bool ok = false;
					const qint64 sec_num = sec.isDouble() ? sec.toInteger()
					                                      : sec.toString().trimmed().toLongLong(&ok, 0);
					if ((sec.isDouble() && sec.toDouble() != 0.0) || (sec.isString() && (!ok || sec_num != 0))) {
						return Tr("This cheat file uses memory addressing features that are not supported yet.");
					}
				}
			}
			const auto absolute = entry.value(QStringLiteral("absolute"));
			const auto expected = entry.value(QStringLiteral("expected"));
			if ((!absolute.isUndefined() && !absolute.isNull() && (!absolute.isBool() || absolute.toBool())) ||
			    (!expected.isUndefined() && !expected.isNull() &&
			     (!expected.isString() ||
			      expected.toString().compare(off_val, Qt::CaseInsensitive) != 0)) ||
			    entry.contains(QStringLiteral("sectionName")) ||
			    entry.contains(QStringLiteral("sectionProtection")) ||
			    entry.contains(QStringLiteral("logicalOffset")) ||
			    entry.contains(QStringLiteral("sectionOffset"))) {
				return Tr("This cheat file uses memory addressing features that are not supported yet.");
			}
		}
	}
	return {};
}

// MC4 is base64 of AES-256-CBC with a fixed key and IV. The XML reader stops at the root end tag,
// before the padding.
bool Decrypt(QByteArray* data) {
	if (data == nullptr) {
		return false;
	}
	static const uint8_t key[] = "304c6528f659c766110239a51cl5dd9c";
	uint8_t              iv[]  = "u@}kzW2u[u(8DWar";
	const auto decoded         = QByteArray::fromBase64(*data);
	if (decoded.size() < 16 || (decoded.size() % 16) != 0 ||
	    (decoded.size() / 16) > 10 * 1024 * 1024) {
		return false;
	}
	auto* aes = av_aes_alloc();
	if (aes == nullptr) {
		return false;
	}
	if (av_aes_init(aes, key, 256, 1) < 0) {
		av_free(aes);
		return false;
	}
	QByteArray plaintext = decoded;
	auto* blocks = reinterpret_cast<uint8_t*>(plaintext.data());
	av_aes_crypt(aes, blocks, blocks, static_cast<int>(plaintext.size() / 16), iv, 1);
	av_free(aes);
	*data = plaintext;
	return true;
}

} // namespace

QString NormalizeVersion(const QString& version) {
	auto s = version.trimmed();
	if (s.isEmpty()) {
		return {};
	}
	if (s.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
		if (s.length() > 1 && (s.at(1).isDigit() || s.at(1) == QLatin1Char('.') ||
		                       s.at(1) == QLatin1Char('_') || s.at(1) == QLatin1Char('-') ||
		                       s.at(1).isSpace())) {
			s.remove(0, 1);
			s = s.trimmed();
		}
	}
	s.replace(QLatin1Char('_'), QLatin1Char('.'));
	s.replace(QLatin1Char('-'), QLatin1Char('.'));
	const auto parts = s.split(QLatin1Char('.'), Qt::SkipEmptyParts);
	QStringList nums;
	for (const auto& part: parts) {
		const auto token = part.trimmed().toLower();
		if (token.isEmpty()) {
			continue;
		}
		bool ok = false;
		quint64 val = token.toULongLong(&ok);
		nums.append(ok ? QString::number(val) : token);
	}
	while (nums.size() > 1 && nums.last() == QStringLiteral("0")) {
		nums.removeLast();
	}
	return nums.join(QLatin1Char('.'));
}

bool VersionsMatch(const QString& v1, const QString& v2) {
	if (v1.trimmed().isEmpty() || v2.trimmed().isEmpty()) {
		return false;
	}
	if (v1.trimmed().compare(v2.trimmed(), Qt::CaseInsensitive) == 0) {
		return true;
	}
	const auto n1 = NormalizeVersion(v1);
	const auto n2 = NormalizeVersion(v2);
	if (n1.isEmpty() || n2.isEmpty()) {
		return false;
	}
	return n1 == n2;
}

QString ConvertTrainer(QByteArray* data, bool encrypted) {
	if (data == nullptr) {
		return Tr("Invalid cheat XML.");
	}
	QByteArray input = *data;
	if (encrypted && !Decrypt(&input)) {
		return Tr("Failed to decrypt cheat trainer.");
	}
	const auto end_tag = QByteArrayLiteral("</Trainer>");
	auto end_pos = input.lastIndexOf(end_tag);
	if (end_pos == -1) {
		end_pos = input.toLower().lastIndexOf(end_tag.toLower());
	}
	if (end_pos != -1) {
		input.truncate(end_pos + end_tag.size());
	}
	QXmlStreamReader xml(input);
	if (!xml.readNextStartElement() || xml.name().toString().compare(QLatin1String("Trainer"), Qt::CaseInsensitive) != 0) {
		return Tr("Invalid cheat XML.");
	}
	const auto trainer = xml.attributes();
	auto attr = [&](const char* name) -> QString {
		for (const auto& a: trainer) {
			if (a.name().toString().compare(QLatin1String(name), Qt::CaseInsensitive) == 0) {
				return a.value().toString().trimmed();
			}
		}
		return {};
	};

	QJsonArray mods;
	while (xml.readNextStartElement()) {
		if (xml.name().toString().compare(QLatin1String("Cheat"), Qt::CaseInsensitive) != 0) {
			xml.skipCurrentElement();
			continue;
		}
		QString cheat_name;
		for (const auto& a: xml.attributes()) {
			if (a.name().toString().compare(QLatin1String("Text"), Qt::CaseInsensitive) == 0 ||
			    a.name().toString().compare(QLatin1String("Name"), Qt::CaseInsensitive) == 0) {
				cheat_name = a.value().toString().trimmed();
				break;
			}
		}
		static const QRegularExpression ws(QStringLiteral("[\\s-]+"));
		auto clean_bytes = [&](QString s) {
			s.remove(ws);
			if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
				s.remove(0, 2);
			}
			if (s.size() == 1) {
				s.prepend(QLatin1Char('0'));
			}
			return s;
		};
		QJsonObject mod {{QStringLiteral("name"), cheat_name}, {QStringLiteral("enabled"), false}};
		QJsonArray  memory;
		QJsonObject flat_write;
		auto flush_flat_write = [&]() -> bool {
			if (flat_write.isEmpty()) {
				return true;
			}
			if (flat_write.value(QStringLiteral("offset")).toString().isEmpty() ||
			    flat_write.value(QStringLiteral("on")).toString().isEmpty() ||
			    flat_write.value(QStringLiteral("off")).toString().isEmpty()) {
				return false;
			}
			memory.append(flat_write);
			flat_write = QJsonObject();
			return true;
		};
		auto maybe_flush_for_field = [&](const QString& field_name) -> bool {
			if (flat_write.contains(field_name)) {
				return flush_flat_write();
			}
			return true;
		};
		while (xml.readNextStartElement()) {
			const auto tag = xml.name().toString();
			if (tag.compare(QLatin1String("Offset"), Qt::CaseInsensitive) == 0) {
				if (!maybe_flush_for_field(QStringLiteral("offset"))) {
					return Tr("Trainer contains an incomplete cheat write entry.");
				}
				auto offset_str = xml.readElementText().trimmed();
				if (!offset_str.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
					offset_str.prepend(QLatin1String("0x"));
				}
				flat_write.insert(QStringLiteral("offset"), offset_str);
			} else if (tag.compare(QLatin1String("ValueOn"), Qt::CaseInsensitive) == 0) {
				if (!maybe_flush_for_field(QStringLiteral("on"))) {
					return Tr("Trainer contains an incomplete cheat write entry.");
				}
				flat_write.insert(QStringLiteral("on"), clean_bytes(xml.readElementText().trimmed()));
			} else if (tag.compare(QLatin1String("ValueOff"), Qt::CaseInsensitive) == 0) {
				if (!maybe_flush_for_field(QStringLiteral("off"))) {
					return Tr("Trainer contains an incomplete cheat write entry.");
				}
				flat_write.insert(QStringLiteral("off"), clean_bytes(xml.readElementText().trimmed()));
			} else if (tag.compare(QLatin1String("Section"), Qt::CaseInsensitive) == 0) {
				if (!maybe_flush_for_field(QStringLiteral("section"))) {
					return Tr("Trainer contains an incomplete cheat write entry.");
				}
				auto value = xml.readElementText().trimmed();
				bool ok = false;
				const auto sec_num = value.toLongLong(&ok, 0);
				if (ok && sec_num != 0) {
					flat_write.insert(QStringLiteral("section"), value);
				} else if (!ok && !value.isEmpty() && value != u"0") {
					flat_write.insert(QStringLiteral("section"), value);
				}
			} else if (tag.compare(QLatin1String("Absolute"), Qt::CaseInsensitive) == 0) {
				if (!maybe_flush_for_field(QStringLiteral("absolute"))) {
					return Tr("Trainer contains an incomplete cheat write entry.");
				}
				auto value = xml.readElementText().trimmed();
				if (!value.isEmpty() && value != u"0" && value.compare(u"false", Qt::CaseInsensitive) != 0) {
					flat_write.insert(QStringLiteral("absolute"), true);
				}
			} else {
				if (!flush_flat_write()) {
					return Tr("Trainer contains an incomplete cheat write entry.");
				}
				QJsonObject write;
				while (xml.readNextStartElement()) {
					const auto field = xml.name().toString();
					if (field.compare(QLatin1String("Offset"), Qt::CaseInsensitive) == 0 ||
					    field.compare(QLatin1String("ValueOn"), Qt::CaseInsensitive) == 0 ||
					    field.compare(QLatin1String("ValueOff"), Qt::CaseInsensitive) == 0 ||
					    field.compare(QLatin1String("Section"), Qt::CaseInsensitive) == 0 ||
					    field.compare(QLatin1String("Absolute"), Qt::CaseInsensitive) == 0) {
						auto value = xml.readElementText().trimmed();
						if (field.compare(QLatin1String("Offset"), Qt::CaseInsensitive) == 0) {
							if (!value.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
								value.prepend(QLatin1String("0x"));
							}
							write.insert(QStringLiteral("offset"), value);
						} else if (field.compare(QLatin1String("ValueOn"), Qt::CaseInsensitive) == 0) {
							write.insert(QStringLiteral("on"), clean_bytes(value));
						} else if (field.compare(QLatin1String("ValueOff"), Qt::CaseInsensitive) == 0) {
							write.insert(QStringLiteral("off"), clean_bytes(value));
						} else if (field.compare(QLatin1String("Section"), Qt::CaseInsensitive) == 0) {
							bool ok = false;
							const auto sec_num = value.toLongLong(&ok, 0);
							if (ok && sec_num != 0) {
								write.insert(QStringLiteral("section"), value);
							} else if (!ok && !value.isEmpty() && value != u"0") {
								write.insert(QStringLiteral("section"), value);
							}
						} else if (field.compare(QLatin1String("Absolute"), Qt::CaseInsensitive) == 0) {
							if (!value.isEmpty() && value != u"0" && value.compare(u"false", Qt::CaseInsensitive) != 0) {
								write.insert(QStringLiteral("absolute"), true);
							}
						}
					} else {
						xml.skipCurrentElement();
					}
				}
				if (!write.isEmpty()) {
					if (write.value(QStringLiteral("offset")).toString().isEmpty() ||
					    write.value(QStringLiteral("on")).toString().isEmpty() ||
					    write.value(QStringLiteral("off")).toString().isEmpty()) {
						return Tr("Trainer contains an incomplete cheat write entry.");
					}
					memory.append(write);
				}
			}
		}
		if (!flush_flat_write()) {
			return Tr("Trainer contains an incomplete cheat write entry.");
		}
		if (!cheat_name.isEmpty() && !memory.isEmpty()) {
			mod.insert(QStringLiteral("memory"), memory);
			mods.append(mod);
		}
	}
	if (xml.hasError()) {
		return Tr("Invalid cheat XML.");
	}
	QString title_id = attr("Ppsa");
	if (title_id.isEmpty()) {
		title_id = attr("TitleId");
	}
	if (title_id.isEmpty()) {
		title_id = attr("Id");
	}
	if (title_id.isEmpty()) {
		title_id = attr("Cusa");
	}
	title_id = title_id.remove(QLatin1Char('-')).remove(QLatin1Char(' ')).trimmed().toUpper();
	if (title_id.isEmpty() || !IsSupportedTitleId(title_id)) {
		return Tr("Trainer has an invalid or unsupported Title ID (%1).").arg(title_id);
	}
	if (attr("Version").isEmpty()) {
		return Tr("Trainer is missing a Version attribute.");
	}
	if (mods.isEmpty()) {
		return Tr("Trainer contains no valid cheat entries.");
	}
	auto moder = attr("Moder");
	if (moder.isEmpty()) {
		moder = attr("Author");
	}
	if (moder.isEmpty()) {
		moder = attr("Credits");
	}
	const auto process_name = attr("Process");
	const QJsonObject root {
	    {QStringLiteral("name"), attr("Game")},
	    {QStringLiteral("id"), title_id},
	    {QStringLiteral("version"), attr("Version")},
	    {QStringLiteral("process"), process_name.isEmpty() ? QStringLiteral("eboot.bin") : process_name},
	    {QStringLiteral("credits"), moder.isEmpty() ? QJsonArray {} : QJsonArray {moder}},
	    {QStringLiteral("mods"), mods}};
	const auto doc = QJsonDocument(root);
	const auto val_err = ValidateDocument(doc);
	if (!val_err.isEmpty()) {
		return val_err;
	}
	*data = doc.toJson();
	return {};
}

bool IsSupportedTitleId(const QString& title_id) {
	static const QRegularExpression pattern(QStringLiteral("\\A(?:PPSA|CUSA)[0-9]{5}\\z"),
	                                        QRegularExpression::CaseInsensitiveOption);
	return pattern.match(title_id.trimmed()).hasMatch();
}

QString PlanPath(const QString& title_id) {
	if (!IsSupportedTitleId(title_id)) {
		return {};
	}
	return QDir(QCoreApplication::applicationDirPath())
	    .filePath(QStringLiteral("_Patches/%1.json").arg(title_id.trimmed().toUpper()));
}

QString ParseDocument(const QByteArray& data, QJsonDocument* document) {
	if (document == nullptr) {
		return Tr("Invalid JSON format.");
	}
	const auto parsed = QJsonDocument::fromJson(data);
	if (!parsed.isObject()) {
		*document = QJsonDocument();
		return Tr("Invalid JSON format.");
	}
	*document = parsed;
	return {};
}

QString ValidateImport(const QJsonDocument& document, const QString& title_id,
                       const QString& version, const QString& process,
                       bool allow_version_mismatch) {
	const auto parse_error = ValidateDocument(document);
	if (!parse_error.isEmpty()) {
		return parse_error;
	}
	const auto root = document.object();
	if (!title_id.isEmpty()) {
		if (!IsSupportedTitleId(title_id) ||
		    root.value(QStringLiteral("id"))
		            .toString()
		            .trimmed()
		            .compare(title_id.trimmed(), Qt::CaseInsensitive) != 0) {
			return Tr("The cheat file does not match this game's title ID.");
		}
	}
	const auto doc_version = root.value(QStringLiteral("version")).toString().trimmed();
	if (doc_version.isEmpty()) {
		return Tr("The cheat file has no version specified.");
	}
	if (!allow_version_mismatch && !version.isEmpty() &&
	    !VersionsMatch(doc_version, version)) {
		return Tr("The cheat file does not match this game's version (%1).").arg(version);
	}
	const auto doc_process = root.value(QStringLiteral("process")).toString().trimmed();
	const auto trimmed_process = process.trimmed();
	if (doc_process.isEmpty() && trimmed_process.isEmpty()) {
		return Tr("The cheat file does not specify an executable process.");
	}
	if (!doc_process.isEmpty() && !trimmed_process.isEmpty() &&
	    doc_process.compare(trimmed_process, Qt::CaseInsensitive) != 0) {
		return Tr("The cheat file does not match this game's executable (%1).").arg(trimmed_process);
	}
	return {};
}

QJsonDocument WithSelection(const QJsonDocument& document, const QList<bool>& enabled,
                            const QString& target_version) {
	if (!document.isObject()) {
		return {};
	}
	auto root = document.object();
	const auto mods_val = root.value(QStringLiteral("mods"));
	if (!mods_val.isArray()) {
		return {};
	}
	auto mods = mods_val.toArray();
	if (mods.size() != enabled.size()) {
		return {};
	}
	for (qsizetype index = 0; index < mods.size(); ++index) {
		if (!mods[index].isObject()) {
			return {};
		}
		auto mod = mods[index].toObject();
		mod.insert(QStringLiteral("enabled"), enabled[index]);
		mods[index] = mod;
	}
	root.insert(QStringLiteral("mods"), mods);
	if (!target_version.isEmpty() &&
	    VersionsMatch(root.value(QStringLiteral("version")).toString(), target_version)) {
		root.insert(QStringLiteral("version"), target_version);
	}
	if (!root.contains(QStringLiteral("process")) ||
	    root.value(QStringLiteral("process")).toString().trimmed().isEmpty()) {
		root.insert(QStringLiteral("process"), QStringLiteral("eboot.bin"));
	}
	return QJsonDocument(root);
}

QString SaveDocument(const QString& path, const QJsonDocument& document,
                     const QByteArray& expected_data) {
	if (path.isEmpty() || !document.isObject()) {
		return Tr("Invalid cheat file.");
	}
	const auto validate_error = ValidateDocument(document);
	if (!validate_error.isEmpty()) {
		return validate_error;
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

ConfigPresetSummary SummarizeConfigPreset(const QJsonDocument& doc) {
	ConfigPresetSummary summary;
	if (!doc.isObject()) {
		return summary;
	}
	const auto root     = doc.object();
	summary.title       = root.value(QStringLiteral("name")).toString().trimmed();
	if (summary.title.isEmpty()) {
		summary.title = root.value(QStringLiteral("title")).toString().trimmed();
	}
	const auto ver_val  = root.value(QStringLiteral("version"));
	summary.version     = ver_val.isString() ? ver_val.toString().trimmed() : ver_val.toVariant().toString().trimmed();
	summary.author      = root.value(QStringLiteral("author")).toString();
	summary.description = root.value(QStringLiteral("description")).toString();

	const auto cfg      = root.value(QStringLiteral("config")).toObject();
	const auto graphics = cfg.value(QStringLiteral("graphics")).toObject();
	const auto cpu      = cfg.value(QStringLiteral("cpu")).toObject();
	const auto audio    = cfg.value(QStringLiteral("audio")).toObject();

	const auto fps_val = graphics.value(QStringLiteral("targetFps"));
	if (fps_val.isDouble() || (fps_val.isString() && !fps_val.toString().trimmed().isEmpty())) {
		bool ok = false;
		const int fps = fps_val.toVariant().toInt(&ok);
		if (ok) {
			summary.settings.append({QStringLiteral("Target FPS (Vblank)"),
			                         QString::number(fps) + QStringLiteral(" Hz")});
		}
	}
	const auto vsync_val = graphics.value(QStringLiteral("vsync"));
	if (vsync_val.isBool() || (vsync_val.isString() && !vsync_val.toString().trimmed().isEmpty())) {
		const bool vsync = vsync_val.isBool() ? vsync_val.toBool()
		                                      : (vsync_val.toString().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 ||
		                                         vsync_val.toString().toInt() != 0);
		summary.settings.append({QStringLiteral("VSync (Present Mode)"),
		                         vsync ? QStringLiteral("Enabled (Fifo)") : QStringLiteral("Disabled (Immediate)")});
	}
	const auto scale_val = graphics.value(QStringLiteral("resolutionScale"));
	if (scale_val.isDouble() || (scale_val.isString() && !scale_val.toString().trimmed().isEmpty())) {
		bool ok = false;
		const double scale = scale_val.toVariant().toDouble(&ok);
		if (ok) {
			summary.settings.append({QStringLiteral("Resolution Scale"),
			                         QString::number(scale, 'f', 2) + QStringLiteral("x")});
		}
	}
	if (graphics.value(QStringLiteral("screen_resolution")).isString()) {
		summary.settings.append({QStringLiteral("Screen Resolution"),
		                         graphics.value(QStringLiteral("screen_resolution")).toString()});
	}

	auto format_setting = [](const QString& key, const QJsonValue& val) -> QString {
		if (val.isBool()) {
			return val.toBool() ? QStringLiteral("Enabled") : QStringLiteral("Disabled");
		}
		if (key == QStringLiteral("present_mode")) {
			bool is_num = false;
			int num = -1;
			if (val.isDouble()) {
				num = val.toInt();
				is_num = true;
			} else if (val.isString()) {
				num = val.toString().trimmed().toInt(&is_num);
			}
			if (is_num) {
				switch (num) {
				case 0: return QStringLiteral("Fifo");
				case 1: return QStringLiteral("Mailbox");
				case 2: return QStringLiteral("Immediate");
				default: break;
				}
			}
			const auto str = val.toString().trimmed();
			if (str.compare(QStringLiteral("fifo"), Qt::CaseInsensitive) == 0 ||
			    str.compare(QStringLiteral("relaxedfifo"), Qt::CaseInsensitive) == 0) return QStringLiteral("Fifo");
			if (str.compare(QStringLiteral("mailbox"), Qt::CaseInsensitive) == 0) return QStringLiteral("Mailbox");
			if (str.compare(QStringLiteral("immediate"), Qt::CaseInsensitive) == 0) return QStringLiteral("Immediate");
		}
		if (key == QStringLiteral("pssr_upscaler")) {
			bool is_num = false;
			int num = -1;
			if (val.isDouble()) {
				num = val.toInt();
				is_num = true;
			} else if (val.isString()) {
				num = val.toString().trimmed().toInt(&is_num);
			}
			if (is_num) {
				switch (num) {
				case 0: return QStringLiteral("Linear");
				case 1: return QStringLiteral("DLSS");
				case 2: return QStringLiteral("FSR");
				default: break;
				}
			}
			const auto str = val.toString().trimmed();
			if (str.compare(QStringLiteral("linear"), Qt::CaseInsensitive) == 0) return QStringLiteral("Linear");
			if (str.compare(QStringLiteral("dlss"), Qt::CaseInsensitive) == 0) return QStringLiteral("DLSS");
			if (str.compare(QStringLiteral("fsr"), Qt::CaseInsensitive) == 0) return QStringLiteral("FSR");
		}
		if (key == QStringLiteral("shader_log_direction") || key == QStringLiteral("printf_direction")) {
			bool is_num = false;
			int num = -1;
			if (val.isDouble()) {
				num = val.toInt();
				is_num = true;
			} else if (val.isString()) {
				num = val.toString().trimmed().toInt(&is_num);
			}
			if (is_num) {
				switch (num) {
				case 0: return QStringLiteral("Silent");
				case 1: return QStringLiteral("Console");
				case 2: return QStringLiteral("File");
				default: break;
				}
			}
			const auto str = val.toString().trimmed();
			if (str.compare(QStringLiteral("silent"), Qt::CaseInsensitive) == 0) return QStringLiteral("Silent");
			if (str.compare(QStringLiteral("console"), Qt::CaseInsensitive) == 0) return QStringLiteral("Console");
			if (str.compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) return QStringLiteral("File");
		}
		if (val.isDouble()) {
			return val.toDouble() == static_cast<double>(val.toInteger())
			           ? QString::number(val.toInteger())
			           : QString::number(val.toDouble());
		}
		return val.toString();
	};

	static const auto valid_keys = Configuration().GameSettings();
	QSet<QString>     seen_keys;
	for (auto it = graphics.constBegin(); it != graphics.constEnd(); ++it) {
		if (valid_keys.contains(it.key()) &&
		    it.key() != QStringLiteral("targetFps") && it.key() != QStringLiteral("vsync") &&
		    it.key() != QStringLiteral("resolutionScale") && it.key() != QStringLiteral("screen_resolution") &&
		    !it.value().isObject() && !it.value().isArray()) {
			if (!seen_keys.contains(it.key())) {
				seen_keys.insert(it.key());
				summary.settings.append({it.key(), format_setting(it.key(), it.value())});
			}
		}
	}
	for (auto it = cpu.constBegin(); it != cpu.constEnd(); ++it) {
		if (valid_keys.contains(it.key()) && !it.value().isObject() && !it.value().isArray()) {
			if (!seen_keys.contains(it.key())) {
				seen_keys.insert(it.key());
				summary.settings.append({it.key(), format_setting(it.key(), it.value())});
			}
		}
	}
	for (auto it = audio.constBegin(); it != audio.constEnd(); ++it) {
		if (valid_keys.contains(it.key()) && !it.value().isObject() && !it.value().isArray()) {
			if (!seen_keys.contains(it.key())) {
				seen_keys.insert(it.key());
				summary.settings.append({it.key(), format_setting(it.key(), it.value())});
			}
		}
	}
	for (auto it = cfg.constBegin(); it != cfg.constEnd(); ++it) {
		if (valid_keys.contains(it.key()) &&
		    it.key() != QStringLiteral("graphics") && it.key() != QStringLiteral("cpu") &&
		    it.key() != QStringLiteral("audio") && !it.value().isObject() && !it.value().isArray()) {
			if (!seen_keys.contains(it.key())) {
				seen_keys.insert(it.key());
				summary.settings.append({it.key(), format_setting(it.key(), it.value())});
			}
		}
	}
	return summary;
}

bool ApplyConfigPreset(Configuration* config, const QJsonDocument& doc, QString* error) {
	if (config == nullptr || !doc.isObject()) {
		if (error != nullptr) {
			*error = Tr("Invalid configuration object.");
		}
		return false;
	}
	const auto root = doc.object();
	if (!root.contains(QStringLiteral("config")) || !root.value(QStringLiteral("config")).isObject()) {
		if (error != nullptr) {
			*error = Tr("Preset does not contain a valid \"config\" object.");
		}
		return false;
	}
	const auto cfg      = root.value(QStringLiteral("config")).toObject();
	const auto graphics = cfg.value(QStringLiteral("graphics")).toObject();
	const auto cpu      = cfg.value(QStringLiteral("cpu")).toObject();
	const auto audio    = cfg.value(QStringLiteral("audio")).toObject();

	Configuration temp;
	temp.CopyEmulatorSettingsFrom(*config);
	temp.CopyGameInfoFrom(*config);
	temp.elf = config->elf;
	temp.custom_settings = true;

	const auto fps_val = graphics.value(QStringLiteral("targetFps"));
	if (fps_val.isDouble() || (fps_val.isString() && !fps_val.toString().trimmed().isEmpty())) {
		bool ok = false;
		const int fps = fps_val.toVariant().toInt(&ok);
		if (ok) {
			if (fps < 30 || fps > 360) {
				if (error != nullptr) {
					*error = Tr("Target FPS must be between 30 and 360.");
				}
				return false;
			}
			temp.vblank_frequency = fps;
		}
	}
	const auto vsync_val = graphics.value(QStringLiteral("vsync"));
	if (vsync_val.isBool() || (vsync_val.isString() && !vsync_val.toString().trimmed().isEmpty())) {
		const bool vsync = vsync_val.isBool() ? vsync_val.toBool()
		                                      : (vsync_val.toString().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 ||
		                                         vsync_val.toString().toInt() != 0);
		temp.present_mode = vsync ? Configuration::PresentMode::Fifo : Configuration::PresentMode::Immediate;
	}
	const auto scale_val = graphics.value(QStringLiteral("resolutionScale"));
	if (scale_val.isDouble() || (scale_val.isString() && !scale_val.toString().trimmed().isEmpty())) {
		bool ok = false;
		const double scale = scale_val.toVariant().toDouble(&ok);
		if (ok) {
			if (scale >= 1.9) {
				temp.screen_resolution = QStringLiteral("3840x2160");
			} else if (scale >= 1.3) {
				temp.screen_resolution = QStringLiteral("2560x1440");
			} else if (scale >= 0.95) {
				temp.screen_resolution = QStringLiteral("1920x1080");
			} else {
				temp.screen_resolution = QStringLiteral("1280x720");
			}
		}
	}
	if (graphics.value(QStringLiteral("screen_resolution")).isString()) {
		const auto res = Configuration::NormalizeResolution(graphics.value(QStringLiteral("screen_resolution")).toString());
		if (res.isEmpty()) {
			if (error != nullptr) {
				*error = Tr("Invalid screen resolution.");
			}
			return false;
		}
		temp.screen_resolution = res;
	}

	const auto valid_keys = temp.GameSettings();
	QJsonObject direct_settings;

	auto add_direct = [&](const QString& key, const QJsonValue& val) {
		if (!valid_keys.contains(key)) {
			return;
		}
		const auto default_val = valid_keys.value(key).toString();
		const bool is_bool_setting = (default_val == QStringLiteral("true") || default_val == QStringLiteral("false"));
		if (is_bool_setting) {
			if (val.isBool()) {
				direct_settings.insert(key, val.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
			} else if (val.isDouble()) {
				direct_settings.insert(key, val.toInt() != 0 ? QStringLiteral("true") : QStringLiteral("false"));
			} else if (val.isString()) {
				const auto s = val.toString().trimmed();
				if (s.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
				    s.compare(QStringLiteral("enabled"), Qt::CaseInsensitive) == 0 ||
				    s == QStringLiteral("1")) {
					direct_settings.insert(key, QStringLiteral("true"));
				} else if (s.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0 ||
				           s.compare(QStringLiteral("disabled"), Qt::CaseInsensitive) == 0 ||
				           s == QStringLiteral("0")) {
					direct_settings.insert(key, QStringLiteral("false"));
				}
			}
			return;
		}
		if (key == QStringLiteral("present_mode")) {
			bool is_num = false;
			int num = -1;
			if (val.isDouble()) {
				num = val.toInt();
				is_num = true;
			} else if (val.isString()) {
				num = val.toString().trimmed().toInt(&is_num);
			}
			if (is_num) {
				switch (num) {
				case 0: direct_settings.insert(key, QStringLiteral("Fifo")); break;
				case 1: direct_settings.insert(key, QStringLiteral("Mailbox")); break;
				case 2: direct_settings.insert(key, QStringLiteral("Immediate")); break;
				default: break;
				}
			} else if (val.isString()) {
				const auto str = val.toString().trimmed();
				if (str.compare(QStringLiteral("fifo"), Qt::CaseInsensitive) == 0 ||
				    str.compare(QStringLiteral("relaxedfifo"), Qt::CaseInsensitive) == 0) {
					direct_settings.insert(key, QStringLiteral("Fifo"));
				} else if (str.compare(QStringLiteral("mailbox"), Qt::CaseInsensitive) == 0) {
					direct_settings.insert(key, QStringLiteral("Mailbox"));
				} else if (str.compare(QStringLiteral("immediate"), Qt::CaseInsensitive) == 0) {
					direct_settings.insert(key, QStringLiteral("Immediate"));
				}
			}
			if (!direct_settings.contains(key)) {
				direct_settings.insert(key, val.toVariant().toString());
			}
			return;
		}
		if (key == QStringLiteral("pssr_upscaler")) {
			bool is_num = false;
			int num = -1;
			if (val.isDouble()) {
				num = val.toInt();
				is_num = true;
			} else if (val.isString()) {
				num = val.toString().trimmed().toInt(&is_num);
			}
			if (is_num) {
				switch (num) {
				case 0: direct_settings.insert(key, QStringLiteral("Linear")); break;
				case 1: direct_settings.insert(key, QStringLiteral("DLSS")); break;
				case 2: direct_settings.insert(key, QStringLiteral("FSR")); break;
				default: break;
				}
			} else if (val.isString()) {
				const auto str = val.toString().trimmed();
				if (str.compare(QStringLiteral("linear"), Qt::CaseInsensitive) == 0) {
					direct_settings.insert(key, QStringLiteral("Linear"));
				} else if (str.compare(QStringLiteral("dlss"), Qt::CaseInsensitive) == 0) {
					direct_settings.insert(key, QStringLiteral("DLSS"));
				} else if (str.compare(QStringLiteral("fsr"), Qt::CaseInsensitive) == 0) {
					direct_settings.insert(key, QStringLiteral("FSR"));
				}
			}
			if (!direct_settings.contains(key)) {
				direct_settings.insert(key, val.toVariant().toString());
			}
			return;
		}
		if (key == QStringLiteral("shader_log_direction") || key == QStringLiteral("printf_direction")) {
			bool is_num = false;
			int num = -1;
			if (val.isDouble()) {
				num = val.toInt();
				is_num = true;
			} else if (val.isString()) {
				num = val.toString().trimmed().toInt(&is_num);
			}
			if (is_num) {
				switch (num) {
				case 0: direct_settings.insert(key, QStringLiteral("Silent")); break;
				case 1: direct_settings.insert(key, QStringLiteral("Console")); break;
				case 2: direct_settings.insert(key, QStringLiteral("File")); break;
				default: break;
				}
			} else if (val.isString()) {
				const auto str = val.toString().trimmed();
				if (str.compare(QStringLiteral("silent"), Qt::CaseInsensitive) == 0) {
					direct_settings.insert(key, QStringLiteral("Silent"));
				} else if (str.compare(QStringLiteral("console"), Qt::CaseInsensitive) == 0) {
					direct_settings.insert(key, QStringLiteral("Console"));
				} else if (str.compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
					direct_settings.insert(key, QStringLiteral("File"));
				}
			}
			if (!direct_settings.contains(key)) {
				direct_settings.insert(key, val.toVariant().toString());
			}
			return;
		}
		if (val.isString()) {
			direct_settings.insert(key, val.toString());
		} else if (val.isBool()) {
			direct_settings.insert(key, val.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
		} else if (val.isDouble()) {
			if (val.toDouble() == static_cast<double>(val.toInteger())) {
				direct_settings.insert(key, QString::number(val.toInteger()));
			} else {
				direct_settings.insert(key, QString::number(val.toDouble()));
			}
		}
	};

	for (auto it = cfg.constBegin(); it != cfg.constEnd(); ++it) {
		if (it.key() != QStringLiteral("graphics") && it.key() != QStringLiteral("cpu") &&
		    it.key() != QStringLiteral("audio")) {
			add_direct(it.key(), it.value());
		}
	}
	for (auto it = graphics.constBegin(); it != graphics.constEnd(); ++it) {
		if (it.key() != QStringLiteral("screen_resolution") && it.key() != QStringLiteral("targetFps") &&
		    it.key() != QStringLiteral("vsync") && it.key() != QStringLiteral("resolutionScale")) {
			add_direct(it.key(), it.value());
		}
	}
	for (auto it = cpu.constBegin(); it != cpu.constEnd(); ++it) {
		add_direct(it.key(), it.value());
	}
	for (auto it = audio.constBegin(); it != audio.constEnd(); ++it) {
		add_direct(it.key(), it.value());
	}

	if (!direct_settings.isEmpty()) {
		QString set_err;
		if (!temp.SetGameSettings(direct_settings, set_err)) {
			if (error != nullptr) {
				*error = set_err;
			}
			return false;
		}
	}
	config->CopyEmulatorSettingsFrom(temp);
	config->CopyGameInfoFrom(temp);
	config->elf = temp.elf;
	config->custom_settings = true;
	return true;
}

static bool SanitizeModEntry(const QJsonObject& in_mod, const QString& fallback_source,
                             const QString& fallback_author, QJsonObject* out_mod) {
	if (out_mod == nullptr) {
		return false;
	}
	const auto name = in_mod.value(QStringLiteral("name")).toString().trimmed();
	if (name.isEmpty()) {
		return false;
	}
	if (in_mod.contains(QStringLiteral("module_name"))) {
		const auto mod_val = in_mod.value(QStringLiteral("module_name"));
		if (!mod_val.isNull() && (!mod_val.isString() || !mod_val.toString().trimmed().isEmpty())) {
			return false;
		}
	}
	const auto memory = in_mod.value(QStringLiteral("memory"));
	if (!memory.isArray() || memory.toArray().isEmpty()) {
		return false;
	}
	static const QRegularExpression offset_re(QStringLiteral("\\A(?:0[xX])?[0-9a-fA-F]{1,16}\\z"));
	static const QRegularExpression bytes_re(QStringLiteral("\\A(?:[0-9a-fA-F]{2})+\\z"));
	QJsonArray clean_memory;
	for (const auto& entry_val: memory.toArray()) {
		if (!entry_val.isObject()) {
			return false;
		}
		const auto entry = entry_val.toObject();
		if (entry.contains(QStringLiteral("section"))) {
			const auto sec = entry.value(QStringLiteral("section"));
			if (!sec.isNull() && !sec.isUndefined()) {
				if (!sec.isDouble() && !sec.isString()) {
					return false;
				}
				bool ok = false;
				const qint64 sec_num = sec.isDouble() ? sec.toInteger()
				                                      : sec.toString().trimmed().toLongLong(&ok, 0);
				if ((sec.isDouble() && sec.toDouble() != 0.0) || (sec.isString() && (!ok || sec_num != 0))) {
					return false;
				}
			}
		}
		const auto absolute = entry.value(QStringLiteral("absolute"));
		const auto expected = entry.value(QStringLiteral("expected"));
		const auto off_str  = entry.value(QStringLiteral("offset")).toString().trimmed();
		const auto off_val  = entry.value(QStringLiteral("off")).toString().trimmed();
		const auto on_val   = entry.value(QStringLiteral("on")).toString().trimmed();
		if ((!absolute.isUndefined() && !absolute.isNull() && (!absolute.isBool() || absolute.toBool())) ||
		    (!expected.isUndefined() && !expected.isNull() &&
		     (!expected.isString() || expected.toString().compare(off_val, Qt::CaseInsensitive) != 0)) ||
		    entry.contains(QStringLiteral("sectionName")) ||
		    entry.contains(QStringLiteral("sectionProtection")) ||
		    entry.contains(QStringLiteral("logicalOffset")) ||
		    entry.contains(QStringLiteral("sectionOffset")) ||
		    !offset_re.match(off_str).hasMatch() ||
		    !bytes_re.match(off_val).hasMatch() ||
		    !bytes_re.match(on_val).hasMatch() ||
		    off_val.size() != on_val.size()) {
			return false;
		}
		QString normalized_offset = off_str;
		if (!normalized_offset.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
			normalized_offset.prepend(QLatin1String("0x"));
		}
		QJsonObject clean_entry {
			{QStringLiteral("offset"), normalized_offset},
			{QStringLiteral("off"), off_val},
			{QStringLiteral("on"), on_val}
		};
		clean_memory.append(clean_entry);
	}
	if (clean_memory.isEmpty()) {
		return false;
	}
	QJsonObject clean_mod {
		{QStringLiteral("name"), name},
		{QStringLiteral("enabled"), false},
		{QStringLiteral("memory"), clean_memory}
	};
	const auto mod_source = in_mod.value(QStringLiteral("source")).toString().trimmed();
	if (!mod_source.isEmpty()) {
		clean_mod.insert(QStringLiteral("source"), mod_source);
	} else if (!fallback_source.isEmpty()) {
		clean_mod.insert(QStringLiteral("source"), fallback_source);
	}
	const auto mod_author = in_mod.value(QStringLiteral("author")).toString().trimmed();
	if (!mod_author.isEmpty()) {
		clean_mod.insert(QStringLiteral("author"), mod_author);
	} else if (!fallback_author.isEmpty()) {
		clean_mod.insert(QStringLiteral("author"), fallback_author);
	}
	*out_mod = clean_mod;
	return true;
}

QJsonDocument PrepareForImport(const QJsonDocument& doc, const QString& source,
                               const QString& author, const QString& process,
                               const QString& version, const QString& title_id) {
	if (!doc.isObject()) {
		return {};
	}
	auto root = doc.object();
	if (!title_id.isEmpty()) {
		if (!IsSupportedTitleId(title_id)) {
			return {};
		}
		const auto doc_id = root.value(QStringLiteral("id")).toString().trimmed();
		if (!doc_id.isEmpty() && doc_id.compare(title_id.trimmed(), Qt::CaseInsensitive) != 0) {
			return {};
		}
		root.insert(QStringLiteral("id"), title_id.trimmed().toUpper());
	}
	const auto trimmed_proc = process.trimmed();
	const auto doc_proc = root.value(QStringLiteral("process")).toString().trimmed();
	if (!trimmed_proc.isEmpty() && !doc_proc.isEmpty() &&
	    doc_proc.compare(trimmed_proc, Qt::CaseInsensitive) != 0) {
		return {};
	}
	if (!version.isEmpty()) {
		root.insert(QStringLiteral("version"), version);
	}
	if (!root.contains(QStringLiteral("process")) ||
	    root.value(QStringLiteral("process")).toString().trimmed().isEmpty()) {
		root.insert(QStringLiteral("process"),
		            trimmed_proc.isEmpty() ? QStringLiteral("eboot.bin") : trimmed_proc);
	}
	auto mods = root.value(QStringLiteral("mods")).toArray();
	QJsonArray clean_mods;
	QSet<QString> seen_names;
	for (qsizetype i = 0; i < mods.size(); ++i) {
		if (!mods[i].isObject()) {
			continue;
		}
		const auto mod = mods[i].toObject();
		const auto name = mod.value(QStringLiteral("name")).toString().trimmed();
		if (name.isEmpty() || seen_names.contains(name.toLower())) {
			continue;
		}
		QJsonObject clean_mod;
		if (!SanitizeModEntry(mod, source, author, &clean_mod)) {
			continue;
		}
		seen_names.insert(name.toLower());
		clean_mods.append(clean_mod);
	}
	if (clean_mods.isEmpty()) {
		return {};
	}
	root.insert(QStringLiteral("mods"), clean_mods);
	return QJsonDocument(root);
}

QJsonDocument MergeDocuments(const QJsonDocument& base_doc, const QJsonDocument& new_doc,
                             const QString& source, const QString& author,
                             const QString& process, const QString& version,
                             QString* error, const QString& title_id,
                             bool allow_version_mismatch) {
	if (!new_doc.isObject()) {
		if (error != nullptr) {
			*error = Tr("Invalid document to merge.");
		}
		return {};
	}
	const auto import_error = ValidateImport(new_doc, title_id, version, process, allow_version_mismatch);
	if (!import_error.isEmpty()) {
		if (error != nullptr) {
			*error = import_error;
		}
		return {};
	}
	const auto new_id = new_doc.object().value(QStringLiteral("id")).toString().trimmed();
	if (!title_id.isEmpty()) {
		if (!IsSupportedTitleId(title_id)) {
			if (error != nullptr) {
				*error = Tr("Cannot merge cheats: Unsupported Title ID (%1).").arg(title_id);
			}
			return {};
		}
		if (!new_id.isEmpty() && new_id.compare(title_id.trimmed(), Qt::CaseInsensitive) != 0) {
			if (error != nullptr) {
				*error = Tr("Cannot merge cheats: Title ID mismatch (%1 vs %2).").arg(new_id, title_id);
			}
			return {};
		}
	}
	const auto trimmed_process = process.trimmed();
	const auto new_proc = new_doc.object().value(QStringLiteral("process")).toString().trimmed();
	if (!trimmed_process.isEmpty() && !new_proc.isEmpty() &&
	    new_proc.compare(trimmed_process, Qt::CaseInsensitive) != 0) {
		if (error != nullptr) {
			*error = Tr("Cannot merge cheats: Executable mismatch (%1 vs %2).").arg(new_proc, trimmed_process);
		}
		return {};
	}
	const auto new_ver = new_doc.object().value(QStringLiteral("version")).toString().trimmed();
	const auto trimmed_ver = version.trimmed();
	if (!allow_version_mismatch && !trimmed_ver.isEmpty() && !new_ver.isEmpty() &&
	    !VersionsMatch(new_ver, trimmed_ver)) {
		if (error != nullptr) {
			*error = Tr("Cannot merge cheats: Version mismatch (%1 vs %2).").arg(new_ver, trimmed_ver);
		}
		return {};
	}
	if (!base_doc.isObject() || !base_doc.object().value(QStringLiteral("mods")).isArray()) {
		auto imported = PrepareForImport(new_doc, source, author, process, version, title_id);
		if (imported.isEmpty() && error != nullptr && error->isEmpty()) {
			*error = Tr("Failed to import cheat document.");
		}
		return imported;
	}
	const auto base_id = base_doc.object().value(QStringLiteral("id")).toString().trimmed();
	if (!base_id.isEmpty() && !new_id.isEmpty() &&
	    base_id.compare(new_id, Qt::CaseInsensitive) != 0) {
		if (error != nullptr) {
			*error = Tr("Cannot merge cheats: Title ID mismatch (%1 vs %2).").arg(base_id, new_id);
		}
		return {};
	}
	if (!title_id.isEmpty() && !base_id.isEmpty() &&
	    base_id.compare(title_id.trimmed(), Qt::CaseInsensitive) != 0) {
		if (error != nullptr) {
			*error = Tr("Cannot merge cheats: Title ID mismatch (%1 vs %2).").arg(base_id, title_id);
		}
		return {};
	}
	const auto base_ver = base_doc.object().value(QStringLiteral("version")).toString().trimmed();
	if (!allow_version_mismatch) {
		if (!base_ver.isEmpty() && !new_ver.isEmpty() &&
		    !VersionsMatch(base_ver, new_ver)) {
			if (error != nullptr) {
				*error = Tr("Cannot merge cheats: Version mismatch (%1 vs %2).").arg(base_ver, new_ver);
			}
			return {};
		}
		if (!trimmed_ver.isEmpty() && !base_ver.isEmpty() &&
		    !VersionsMatch(base_ver, trimmed_ver)) {
			if (error != nullptr) {
				*error = Tr("Cannot merge cheats: Version mismatch (%1 vs %2).").arg(base_ver, trimmed_ver);
			}
			return {};
		}
	}
	const auto base_proc = base_doc.object().value(QStringLiteral("process")).toString().trimmed();
	if (!base_proc.isEmpty() && !new_proc.isEmpty() &&
	    base_proc.compare(new_proc, Qt::CaseInsensitive) != 0) {
		if (error != nullptr) {
			*error = Tr("Cannot merge cheats: Executable mismatch (%1 vs %2).").arg(base_proc, new_proc);
		}
		return {};
	}
	if (!trimmed_process.isEmpty() && !base_proc.isEmpty() &&
	    base_proc.compare(trimmed_process, Qt::CaseInsensitive) != 0) {
		if (error != nullptr) {
			*error = Tr("Cannot merge cheats: Executable mismatch (%1 vs %2).").arg(base_proc, trimmed_process);
		}
		return {};
	}
	auto root = base_doc.object();
	if (!title_id.isEmpty()) {
		root.insert(QStringLiteral("id"), title_id.trimmed().toUpper());
	} else if (base_id.isEmpty() && !new_id.isEmpty()) {
		root.insert(QStringLiteral("id"), new_id.trimmed().toUpper());
	}
	const QString target_ver = !version.isEmpty() ? version : (!base_ver.isEmpty() ? base_ver : new_ver);
	if (!target_ver.isEmpty()) {
		root.insert(QStringLiteral("version"), target_ver);
	}
	const bool base_version_changed = !base_ver.isEmpty() && !target_ver.isEmpty() &&
	                                  !VersionsMatch(base_ver, target_ver);
	if (!root.contains(QStringLiteral("process")) ||
	    root.value(QStringLiteral("process")).toString().trimmed().isEmpty()) {
		const auto target_proc = !trimmed_process.isEmpty() ? trimmed_process
		                       : !new_proc.isEmpty()         ? new_proc
		                                                     : QStringLiteral("eboot.bin");
		root.insert(QStringLiteral("process"), target_proc);
	}
	if (!root.contains(QStringLiteral("name")) && new_doc.object().contains(QStringLiteral("name"))) {
		root.insert(QStringLiteral("name"), new_doc.object().value(QStringLiteral("name")));
	}
	if (!root.contains(QStringLiteral("credits")) && new_doc.object().contains(QStringLiteral("credits"))) {
		root.insert(QStringLiteral("credits"), new_doc.object().value(QStringLiteral("credits")));
	}
	auto base_mods = root.value(QStringLiteral("mods")).toArray();
	if (base_version_changed) {
		QJsonArray disabled_base_mods;
		for (const auto& val: base_mods) {
			if (val.isObject()) {
				auto mod_obj = val.toObject();
				mod_obj.insert(QStringLiteral("enabled"), false);
				disabled_base_mods.append(mod_obj);
			}
		}
		base_mods = disabled_base_mods;
		root.insert(QStringLiteral("mods"), base_mods);
	}
	QSet<QString> existing_names;
	for (const auto& val: base_mods) {
		if (val.isObject()) {
			existing_names.insert(val.toObject().value(QStringLiteral("name")).toString().trimmed().toLower());
		}
	}
	const auto new_mods = new_doc.object().value(QStringLiteral("mods")).toArray();
	for (const auto& val: new_mods) {
		if (!val.isObject()) {
			continue;
		}
		const auto mod = val.toObject();
		const auto name = mod.value(QStringLiteral("name")).toString().trimmed();
		if (name.isEmpty() || existing_names.contains(name.toLower())) {
			continue;
		}
		QJsonObject clean_mod;
		if (!SanitizeModEntry(mod, source, author, &clean_mod)) {
			continue;
		}
		existing_names.insert(name.toLower());
		base_mods.append(clean_mod);
	}
	if (base_mods.isEmpty()) {
		if (error != nullptr) {
			*error = Tr("No valid cheats found to merge.");
		}
		return {};
	}
	root.insert(QStringLiteral("mods"), base_mods);
	return QJsonDocument(root);
}

} // namespace Cheats
