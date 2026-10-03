#include "gameDiscovery.h"

#include "common/archive.h"
#include "gameContent.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

QList<GameCandidate> DiscoverGames(const QStringList& roots) {
	QList<GameCandidate> result;
	QSet<QString>        seen;
	for (const auto& root_path: roots) {
		if (root_path.isEmpty()) continue;
		const QDir  root(root_path);
		QList<QDir> pending {root};
		while (!pending.isEmpty()) {
			QDir directory = pending.takeFirst();
			directory.refresh();
			if (!directory.exists()) continue;
			const auto canonical = QFileInfo(directory.absolutePath()).canonicalFilePath();
			if (seen.contains(canonical)) continue;
			seen.insert(canonical);
			const auto files = directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
			for (const auto& file: files) {
				if (Common::IsSupportedArchive(GameContent::ToPath(file.absoluteFilePath())))
					result.append({file.absoluteFilePath(), root.absolutePath(), true});
			}
			if (directory.exists(QStringLiteral("eboot.bin"))) {
				result.append({directory.absolutePath(), root.absolutePath(), false});
				// Avoid descending through every asset in an extracted game.
				continue;
			}
			for (const auto& child: directory.entryInfoList(
			         QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
				pending.append(QDir(child.absoluteFilePath()));
		}
	}
	return result;
}
