#include "patchesDialog.h"

#include "cheatFile.h"
#include "configuration.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

void ShowMods(QListWidget* list, const QJsonDocument& document, bool checkable) {
	list->clear();
	for (const auto& value: document.object().value(QStringLiteral("mods")).toArray()) {
		const auto mod  = value.toObject();
		auto*      item = new QListWidgetItem(mod.value(QStringLiteral("name")).toString(), list);
		if (checkable) {
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(mod.value(QStringLiteral("enabled")).toBool(true) ? Qt::Checked
			                                                                      : Qt::Unchecked);
		}
	}
}

QLabel* StatusLabel(QWidget* parent) {
	auto* label = new QLabel(parent);
	label->setWordWrap(true);
	label->setTextFormat(Qt::PlainText);
	return label;
}

} // namespace

PatchesDialog::PatchesDialog(const Configuration& game, QWidget* parent)
    : QDialog(parent), m_title_id(game.title_id.trimmed().toUpper()), m_version(game.gameVersion),
      m_process(QFileInfo(game.elf.isEmpty() ? QStringLiteral("eboot.bin") : game.elf).fileName()) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("Cheats (experimental) - %1").arg(game.name));
	resize(800, 640);

	auto* layout     = new QVBoxLayout(this);
	auto* game_label = StatusLabel(this);
	game_label->setText(tr("%1 · Installed version: %2")
	                        .arg(m_title_id, m_version.isEmpty() ? tr("Unknown") : m_version));
	layout->addWidget(game_label);
	m_tabs = new QTabWidget(this);
	layout->addWidget(m_tabs);

	auto* remote        = new QWidget(m_tabs);
	auto* remote_layout = new QVBoxLayout(remote);
	m_tabs->addTab(remote, tr("Remote"));
	auto* credits = new QLabel(
	    tr("Cheats by <a href=\"https://github.com/TeeKay87/HEN-Cheats-Collection\">TeeKay87 / HEN "
	       "Cheats Collection</a> and its contributors."),
	    remote);
	credits->setOpenExternalLinks(true);
	credits->setWordWrap(true);
	remote_layout->addWidget(credits);
	auto* support = StatusLabel(remote);
	support->setText(tr("Only JSON cheats are supported. MC4 and SHN files are shown disabled. "
	                    "Select a JSON file to preview its cheats."));
	remote_layout->addWidget(support);
	auto* split = new QSplitter(Qt::Vertical, remote);
	m_files     = new QTreeWidget(split);
	m_files->setHeaderLabels({tr("Version"), tr("Format"), tr("File")});
	m_files->setRootIsDecorated(false);
	m_files->setSelectionMode(QAbstractItemView::SingleSelection);
	m_files->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_files->header()->setSectionResizeMode(2, QHeaderView::Stretch);
	m_preview = new QListWidget(split);
	remote_layout->addWidget(split, 1);
	m_remote_status  = StatusLabel(remote);
	m_preview_status = StatusLabel(remote);
	remote_layout->addWidget(m_remote_status);
	remote_layout->addWidget(m_preview_status);
	auto* remote_buttons = new QDialogButtonBox(remote);
	m_refresh            = remote_buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
	m_import = remote_buttons->addButton(tr("Import to Local"), QDialogButtonBox::ActionRole);
	m_import->setEnabled(false);
	remote_layout->addWidget(remote_buttons);

	auto* local        = new QWidget(m_tabs);
	auto* local_layout = new QVBoxLayout(local);
	m_tabs->addTab(local, tr("Local"));
	m_patches = new QListWidget(local);
	m_status  = StatusLabel(local);
	local_layout->addWidget(m_patches, 1);
	local_layout->addWidget(m_status);
	auto* local_buttons = new QDialogButtonBox(local);
	auto* reload        = local_buttons->addButton(tr("Reload"), QDialogButtonBox::ActionRole);
	m_apply = local_buttons->addButton(tr("Apply selection"), QDialogButtonBox::ActionRole);
	local_layout->addWidget(local_buttons);
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
	connect(reload, &QPushButton::clicked, this, &PatchesDialog::Load);
	connect(m_apply, &QPushButton::clicked, this, &PatchesDialog::Save);
	connect(m_refresh, &QPushButton::clicked, this, &PatchesDialog::LoadRemote);
	connect(m_import, &QPushButton::clicked, this, &PatchesDialog::ImportRemote);
	connect(m_files, &QTreeWidget::currentItemChanged, this, &PatchesDialog::PreviewRemote);

	m_repository = new Cheats::CheatRepository(this);
	connect(
	    m_repository, &Cheats::CheatRepository::CatalogLoaded, this,
	    [this](QList<Cheats::RemoteFile> files, const QString& error) {
		    std::stable_sort(files.begin(), files.end(), [this](const auto& a, const auto& b) {
			    return (a.version == m_version) > (b.version == m_version);
		    });
		    m_remote_files = std::move(files);
		    for (qsizetype index = 0; index < m_remote_files.size(); ++index) {
			    const auto& file = m_remote_files[index];
			    auto*       item = new QTreeWidgetItem(
			        m_files, {file.version == m_version ? tr("%1 (installed)").arg(file.version)
			                                            : file.version,
			                  file.format.toUpper(), file.name});
			    item->setData(0, Qt::UserRole, index);
			    for (int column = 0; column < 3; ++column) {
				    item->setToolTip(column, file.IsSupported()
				                                 ? file.title
				                                 : tr("%1 — %2 is not supported.")
				                                       .arg(file.title, file.format.toUpper()));
			    }
			    if (!file.IsSupported()) {
				    item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
			    }
		    }
		    auto status =
		        m_remote_files.isEmpty() && error.isEmpty()
		            ? tr("No remote cheats found for %1.").arg(m_title_id)
		            : tr("%1 remote file(s) for %2.").arg(m_remote_files.size()).arg(m_title_id);
		    if (!error.isEmpty()) {
			    status = tr("%1\nCould not load the complete collection: %2\nUse Refresh to retry.")
			                 .arg(status, error);
		    }
		    m_remote_status->setText(status);
		    m_refresh->setEnabled(true);
	    });
	connect(
	    m_repository, &Cheats::CheatRepository::FileLoaded, this,
	    [this](const QByteArray& data, QString error) {
		    if (error.isEmpty()) {
			    error = Cheats::ParseDocument(data, &m_remote_document);
		    }
		    if (!error.isEmpty()) {
			    m_preview_status->setText(
			        tr("Could not preview this file: %1\nSelect another file or refresh to retry.")
			            .arg(error));
			    return;
		    }
		    ShowMods(m_preview, m_remote_document, false);
		    error = Cheats::ValidateImport(m_remote_document, m_title_id, m_version, m_process);
		    const auto  root = m_remote_document.object();
		    QStringList creators;
		    for (const auto& value: root.value(QStringLiteral("credits")).toArray()) {
			    if (value.isString()) {
				    creators.append(value.toString());
			    }
		    }
		    const auto metadata =
		        tr("%1\nCredits: %2 · Version: %3")
		            .arg(root.value(QStringLiteral("name")).toString(),
		                 creators.isEmpty() ? root.value(QStringLiteral("author")).toString()
		                                    : creators.join(QStringLiteral(", ")),
		                 root.value(QStringLiteral("version")).toString());
		    m_preview_status->setText(
		        metadata + QStringLiteral("\n\n") +
		        (error.isEmpty() ? tr("Import to Local, then select the cheats to enable. Changes "
		                              "take effect on the next game launch.")
		                         : error));
		    m_import->setEnabled(error.isEmpty());
	    });
	Load();
	LoadRemote();
}

void PatchesDialog::Load() {
	m_patches->clear();
	m_local_data.clear();
	m_local_document = {};
	m_local_readable = false;
	m_apply->setEnabled(false);
	const auto path = Cheats::PlanPath(m_title_id);
	if (path.isEmpty()) {
		m_status->setText(tr("Unsupported title ID."));
		return;
	}
	QFile file(path);
	if (!file.exists()) {
		m_local_readable = true;
		m_status->setText(tr("No local cheat file: %1").arg(path));
		return;
	}
	if (!file.open(QIODevice::ReadOnly)) {
		m_status->setText(tr("Could not read cheat file: %1").arg(file.errorString()));
		return;
	}
	m_local_data = file.readAll();
	if (file.error() != QFile::NoError) {
		m_status->setText(tr("Could not read cheat file: %1").arg(file.errorString()));
		return;
	}
	m_local_readable = true;
	const auto error = Cheats::ParseDocument(m_local_data, &m_local_document);
	if (!error.isEmpty()) {
		m_status->setText(tr("%1\n%2").arg(error, path));
		return;
	}
	ShowMods(m_patches, m_local_document, true);
	m_apply->setEnabled(m_patches->count() > 0);
	m_status->setText(tr("Loaded %1 cheat(s) from %2.").arg(m_patches->count()).arg(path));
}

void PatchesDialog::Save() {
	QList<bool> enabled;
	for (int index = 0; index < m_patches->count(); ++index) {
		enabled.append(m_patches->item(index)->checkState() == Qt::Checked);
	}
	const auto document = Cheats::WithSelection(m_local_document, enabled);
	const auto error = Cheats::SaveDocument(Cheats::PlanPath(m_title_id), document, m_local_data);
	if (!error.isEmpty()) {
		m_status->setText(error);
		return;
	}
	m_local_document = document;
	m_local_data     = document.toJson();
	m_status->setText(tr("Cheat selection saved. Changes take effect on the next game launch."));
}

void PatchesDialog::LoadRemote() {
	m_repository->CancelFetch();
	m_files->clear();
	m_remote_files.clear();
	m_preview->clear();
	m_remote_document = {};
	m_import->setEnabled(false);
	m_refresh->setEnabled(false);
	m_preview_status->clear();
	m_remote_status->setText(tr("Loading remote cheats…"));
	m_repository->Load(m_title_id);
}

void PatchesDialog::PreviewRemote() {
	m_repository->CancelFetch();
	m_preview->clear();
	m_remote_document = {};
	m_import->setEnabled(false);
	m_preview_status->clear();
	const auto* item = m_files->currentItem();
	if (item == nullptr) {
		return;
	}
	const auto index = item->data(0, Qt::UserRole).toInt();
	if (index < 0 || index >= m_remote_files.size() || !m_remote_files[index].IsSupported()) {
		return;
	}
	m_preview_status->setText(tr("Loading cheat preview…"));
	m_repository->Fetch(m_remote_files[index]);
}

void PatchesDialog::ImportRemote() {
	const auto error = Cheats::ValidateImport(m_remote_document, m_title_id, m_version, m_process);
	if (!error.isEmpty()) {
		m_preview_status->setText(error);
		return;
	}
	if (!m_local_readable) {
		m_preview_status->setText(
		    tr("Could not read the local cheat file. Open Local and reload it before importing."));
		return;
	}
	const auto path = Cheats::PlanPath(m_title_id);
	if (QFile::exists(path) &&
	    QMessageBox::question(this, tr("Replace local cheats?"),
	                          tr("Replace the local cheat file and selections for %1? Imported "
	                             "cheats will start unchecked.")
	                              .arg(m_title_id),
	                          QMessageBox::Yes | QMessageBox::No,
	                          QMessageBox::No) != QMessageBox::Yes) {
		return;
	}
	const auto mods     = m_remote_document.object().value(QStringLiteral("mods")).toArray();
	const auto document = Cheats::WithSelection(m_remote_document, QList<bool>(mods.size(), false));
	const auto save_error = Cheats::SaveDocument(path, document, m_local_data);
	if (!save_error.isEmpty()) {
		m_preview_status->setText(save_error);
		return;
	}
	Load();
	m_status->setText(
	    tr("Imported %1 cheat(s). Select cheats and apply your selection for the next game launch.")
	        .arg(mods.size()));
	m_tabs->setCurrentIndex(1);
}
