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
	setWindowTitle(tr("Cheats (Experimental) - %1").arg(game.name));
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
		m_status->setText(tr("No local cheat file: %1").arg(file.fileName()));
		m_apply->setEnabled(false);
		return;
	}

	const auto document = QJsonDocument::fromJson(file.readAll());
	if (document.isNull() || !document.isObject()) {
		m_status->setText(tr("Invalid cheat JSON in %1.").arg(file.fileName()));
		m_apply->setEnabled(false);
		return;
	}

	const auto root = document.object();
	if (!root.value(QStringLiteral("mods")).isArray()) {
		m_status->setText(tr("Cheat JSON has no \"mods\" array in %1.").arg(file.fileName()));
		m_apply->setEnabled(false);
		return;
	}

	const auto mods = root.value(QStringLiteral("mods")).toArray();
	for (const auto& value: mods) {
		if (!value.isObject()) {
			m_status->setText(tr("Invalid cheat entry in %1.").arg(file.fileName()));
			m_apply->setEnabled(false);
			m_patches->clear();
			return;
		}
		const auto mod     = value.toObject();
		const auto name    = mod.value(QStringLiteral("name"));
		const auto enabled = mod.value(QStringLiteral("enabled"));
		if (!name.isString() || name.toString().isEmpty() ||
		    (!enabled.isUndefined() && !enabled.isBool())) {
			m_status->setText(tr("Invalid cheat entry in %1.").arg(file.fileName()));
			m_apply->setEnabled(false);
			m_patches->clear();
			return;
		}
		auto* item = new QListWidgetItem(name.toString(), m_patches);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(enabled.toBool(true) ? Qt::Checked : Qt::Unchecked);
	}

	m_apply->setEnabled(!mods.isEmpty());
	m_status->setText(tr("Loaded %1 cheat(s) from %2.").arg(mods.size()).arg(file.fileName()));
}

void PatchesDialog::Save() {
	const auto path = PatchPlanPath(m_title_id);
	QFile      input(path);
	if (!input.open(QIODevice::ReadOnly)) {
		return;
	}

	auto document = QJsonDocument::fromJson(input.readAll());
	input.close();
	const auto changed_message = tr("Cheat file changed; reload the dialog.");
	if (!document.isObject()) {
		m_status->setText(changed_message);
		return;
	}
	auto root = document.object();
	auto mods = root.value(QStringLiteral("mods")).toArray();
	if (mods.size() != m_patches->count()) {
		m_status->setText(changed_message);
		return;
	}
	for (int index = 0; index < mods.size(); index++) {
		auto mod = mods[index].toObject();
		mod.insert(QStringLiteral("enabled"), m_patches->item(index)->checkState() == Qt::Checked);
		mods[index] = mod;
	}
	root.insert(QStringLiteral("mods"), mods);
	document.setObject(root);

	QSaveFile output(path);
	if (output.open(QIODevice::WriteOnly) && output.write(document.toJson()) >= 0 &&
	    output.commit()) {
		m_status->setText(tr("Cheat selection saved."));
	}
}
