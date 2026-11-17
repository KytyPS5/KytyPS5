#ifndef CHEAT_FILE_H
#define CHEAT_FILE_H

#include <QByteArray>
#include <QJsonDocument>
#include <QList>
#include <QPair>
#include <QString>

class Configuration;

namespace Cheats {

[[nodiscard]] QString NormalizeVersion(const QString& version);
[[nodiscard]] bool    VersionsMatch(const QString& v1, const QString& v2);

[[nodiscard]] bool    IsSupportedTitleId(const QString& title_id);
[[nodiscard]] QString PlanPath(const QString& title_id);
// Replaces MC4 (encrypted) or SHN trainer XML with the equivalent cheat JSON.
[[nodiscard]] QString ConvertTrainer(QByteArray* data, bool encrypted);

// Local files retain their existing defaults; remote imports receive additional validation.
[[nodiscard]] QString       ParseDocument(const QByteArray& data, QJsonDocument* document);
[[nodiscard]] QString       ValidateImport(const QJsonDocument& document, const QString& title_id,
                                           const QString& version, const QString& process,
                                           bool allow_version_mismatch = false);
[[nodiscard]] QJsonDocument WithSelection(const QJsonDocument& document,
                                          const QList<bool>&   enabled,
                                          const QString&       target_version = {});
// Refuse to replace a file changed since it was loaded; empty data also permits creation.
[[nodiscard]] QString SaveDocument(const QString& path, const QJsonDocument& document,
                                   const QByteArray& expected_data);

struct ConfigPresetSummary {
	QString                        title;
	QString                        version;
	QString                        author;
	QString                        description;
	QList<QPair<QString, QString>> settings;
};

[[nodiscard]] ConfigPresetSummary SummarizeConfigPreset(const QJsonDocument& doc);
[[nodiscard]] bool ApplyConfigPreset(Configuration* config, const QJsonDocument& doc,
                                     QString* error);

[[nodiscard]] QJsonDocument PrepareForImport(const QJsonDocument& doc, const QString& source,
                                             const QString& author, const QString& process,
                                             const QString& version = {},
                                             const QString& title_id = {});
[[nodiscard]] QJsonDocument MergeDocuments(const QJsonDocument& base_doc,
                                           const QJsonDocument& new_doc,
                                           const QString& source, const QString& author,
                                           const QString& process,
                                           const QString& version = {},
                                           QString* error = nullptr,
                                           const QString& title_id = {},
                                           bool allow_version_mismatch = false);

} // namespace Cheats

#endif // CHEAT_FILE_H
