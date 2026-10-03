#pragma once

#include <QList>
#include <QStringList>

struct GameCandidate {
	QString path;
	QString root;
	bool    archive = false;
};

// Read the filesystem anew on every scan. Invalid archives remain candidates so
// a later Refresh can retry them after a copy finishes or a helper is installed.
QList<GameCandidate> DiscoverGames(const QStringList& roots);
