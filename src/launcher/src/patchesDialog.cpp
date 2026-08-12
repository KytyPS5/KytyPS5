#include "patchesDialog.h"

#include "configuration.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>

PatchesDialog::PatchesDialog(const Configuration& game, QWidget* parent)
    : QDialog(parent), m_title_id(game.title_id.trimmed().toUpper()) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("Patches (Experimental) - %1").arg(game.name));
	resize(640, 480);

	auto* layout = new QVBoxLayout(this);
	m_patches    = new QListWidget(this);
	m_status     = new QLabel(this);
	m_apply      = new QPushButton(tr("Apply selection"), this);
	auto* close  = new QPushButton(tr("Close"), this);

	m_status->setWordWrap(true);
	layout->addWidget(m_patches);
	layout->addWidget(m_status);

	auto* buttons = new QDialogButtonBox(this);
	buttons->addButton(m_apply, QDialogButtonBox::ActionRole);
	buttons->addButton(close, QDialogButtonBox::RejectRole);
	layout->addWidget(buttons);

	connect(m_apply, &QPushButton::clicked, this, &PatchesDialog::Save);
	connect(close, &QPushButton::clicked, this, &QDialog::close);

	Load();
}

bool PatchesDialog::IsSupportedTitleId(const QString& title_id) {
	return title_id.trimmed().toUpper().startsWith(QStringLiteral("PPSA"));
}

QString PatchesDialog::PatchPlanPath(const QString& title_id) {
	return QDir(QCoreApplication::applicationDirPath())
	    .filePath(QStringLiteral("_Patches/%1.json").arg(title_id.trimmed().toUpper()));
}

void PatchesDialog::Load() {
	QFile file(PatchPlanPath(m_title_id));
	if (!file.open(QIODevice::ReadOnly)) {
		m_status->setText(tr("No local patch file: %1").arg(file.fileName()));
		m_apply->setEnabled(false);
		return;
	}

	const auto document = QJsonDocument::fromJson(file.readAll());
	if (document.isNull() || !document.isObject()) {
		m_status->setText(tr("Invalid patch JSON in %1.").arg(file.fileName()));
		m_apply->setEnabled(false);
		return;
	}

	const auto root    = document.object();
	const auto patches = root.value(QStringLiteral("patches")).toArray();
	for (const auto& value: patches) {
		const auto patch = value.toObject();
		auto* item = new QListWidgetItem(patch.value(QStringLiteral("name")).toString(), m_patches);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(patch.value(QStringLiteral("enabled")).toBool(true) ? Qt::Checked
		                                                                        : Qt::Unchecked);
	}

	m_apply->setEnabled(!patches.isEmpty());
	if (patches.isEmpty()) {
		// shadPS4/ETAHen cheat files use "mods"; Kyty only reads a "patches" array.
		if (root.contains(QStringLiteral("mods"))) {
			m_status->setText(
			    tr("This file uses a \"mods\" list. Kyty expects a top-level \"patches\" "
			       "array in %1.")
			        .arg(file.fileName()));
		} else if (!root.contains(QStringLiteral("patches"))) {
			m_status->setText(
			    tr("No \"patches\" array in %1.").arg(file.fileName()));
		} else {
			m_status->setText(tr("Loaded 0 patch(es) from %1.").arg(file.fileName()));
		}
		return;
	}

	m_status->setText(tr("Loaded %1 patch(es) from %2.").arg(patches.size()).arg(file.fileName()));
}

void PatchesDialog::Save() {
	const auto path = PatchPlanPath(m_title_id);
	QFile      input(path);
	if (!input.open(QIODevice::ReadOnly)) {
		return;
	}

	auto document = QJsonDocument::fromJson(input.readAll());
	input.close();
	auto root    = document.object();
	auto patches = root.value(QStringLiteral("patches")).toArray();
	for (int index = 0; index < patches.size(); index++) {
		auto patch = patches[index].toObject();
		patch.insert(QStringLiteral("enabled"),
		             m_patches->item(index)->checkState() == Qt::Checked);
		patches[index] = patch;
	}
	root.insert(QStringLiteral("patches"), patches);
	document.setObject(root);

	QSaveFile output(path);
	if (output.open(QIODevice::WriteOnly) && output.write(document.toJson()) >= 0 &&
	    output.commit()) {
		m_status->setText(tr("Patch selection saved."));
	}
}
