#ifndef COMPATIBILITY_DATABASE_H
#define COMPATIBILITY_DATABASE_H

#include "common.h"
#include "configuration.h"

#include <QMap>
#include <QObject>
#include <QString>

struct CompatibilityEntry {
	Configuration::GameStatus status = Configuration::GameStatus::Unknown;
	QString                   comment;
};

class CompatibilityDatabase: public QObject {
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(CompatibilityDatabase);

public:
	explicit CompatibilityDatabase(bool local, QObject* parent = nullptr);

	[[nodiscard]] bool                      IsLocal() const { return m_local; }
	[[nodiscard]] const CompatibilityEntry* Find(const QString& title_id) const;

	void Load();
	void SetStatus(const QString& title_id, Configuration::GameStatus status);
	void SetComment(const QString& title_id, const QString& comment);

signals:
	void Updated();

private:
	void Save() const;

	QMap<QString, CompatibilityEntry> m_entries;
	bool                              m_local = false;
};

#endif // COMPATIBILITY_DATABASE_H
