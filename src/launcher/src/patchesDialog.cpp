#include "patchesDialog.h"

#include "cheatFile.h"
#include "configuration.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
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
		const auto mod    = value.toObject();
		auto       name   = mod.value(QStringLiteral("name")).toString();
		const auto source = mod.value(QStringLiteral("source")).toString();
		const auto author = mod.value(QStringLiteral("author")).toString();
		if (checkable && (!source.isEmpty() || !author.isEmpty())) {
			QStringList tags;
			if (!source.isEmpty()) {
				tags.append(source);
			}
			if (!author.isEmpty()) {
				tags.append(author);
			}
			name = QStringLiteral("[%1] %2").arg(tags.join(QStringLiteral(" · ")), name);
		}
		auto* item = new QListWidgetItem(name, list);
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

PatchesDialog::PatchesDialog(const Configuration& game, ConfigApplyCallback config_callback,
                             QWidget* parent)
    : QDialog(parent), m_title_id(game.title_id.trimmed().toUpper()), m_game_name(game.name),
      m_version(game.gameVersion),
      m_process(QFileInfo(game.elf.isEmpty() ? QStringLiteral("eboot.bin") : game.elf).fileName()),
      m_config_callback(std::move(config_callback)) {
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("Patches, Cheats & Configs - %1").arg(game.name));
	resize(850, 680);

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
	    tr("Modules provided by <a href=\"https://github.com/KytyPS5/KytyPS5-Modules\">KytyPS5-Modules</a> "
	       "and <a href=\"https://github.com/TeeKay87/HEN-Cheats-Collection\">HEN Cheats Collection</a>."),
	    remote);
	credits->setOpenExternalLinks(true);
	credits->setWordWrap(true);
	remote_layout->addWidget(credits);

	auto* filter_layout = new QHBoxLayout;
	auto* filter_label  = new QLabel(tr("Filter:"), remote);
	m_filter_combo      = new QComboBox(remote);
	m_filter_combo->addItems(
	    {tr("All Categories"), tr("Patches only"), tr("Cheats only"), tr("Configs only")});
	filter_layout->addWidget(filter_label);
	filter_layout->addWidget(m_filter_combo);
	filter_layout->addStretch();
	remote_layout->addLayout(filter_layout);

	auto* support = StatusLabel(remote);
	support->setText(tr("Select a module below to preview details or apply:"));
	remote_layout->addWidget(support);

	auto* split = new QSplitter(Qt::Vertical, remote);
	m_files     = new QTreeWidget(split);
	m_files->setHeaderLabels(
	    {tr("Title / Module"), tr("Type"), tr("Version"), tr("Source"), tr("Author")});
	m_files->setRootIsDecorated(true);
	m_files->setSelectionMode(QAbstractItemView::SingleSelection);
	m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_files->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_files->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	m_files->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

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

	auto* selection   = new QDialogButtonBox(local);
	auto* disable_all = selection->addButton(tr("Disable all"), QDialogButtonBox::ActionRole);
	auto* enable_all  = selection->addButton(tr("Enable all"), QDialogButtonBox::ActionRole);
	local_layout->addWidget(selection);

	auto* local_buttons = new QDialogButtonBox(local);
	auto* reload        = local_buttons->addButton(tr("Reload"), QDialogButtonBox::ActionRole);
	m_apply = local_buttons->addButton(tr("Apply selection"), QDialogButtonBox::ActionRole);
	local_layout->addWidget(local_buttons);

	auto set_all = [this](Qt::CheckState state) {
		for (int i = 0; i < m_patches->count(); ++i) {
			m_patches->item(i)->setCheckState(state);
		}
	};
	connect(enable_all, &QPushButton::clicked, this, [set_all] { set_all(Qt::Checked); });
	connect(disable_all, &QPushButton::clicked, this, [set_all] { set_all(Qt::Unchecked); });

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
	connect(reload, &QPushButton::clicked, this, &PatchesDialog::Load);
	connect(m_apply, &QPushButton::clicked, this, &PatchesDialog::Save);
	connect(m_refresh, &QPushButton::clicked, this, &PatchesDialog::LoadRemote);
	connect(m_import, &QPushButton::clicked, this, &PatchesDialog::ImportRemote);
	connect(m_files, &QTreeWidget::currentItemChanged, this, &PatchesDialog::PreviewRemote);
	connect(m_filter_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
	        &PatchesDialog::FilterChanged);

	m_repository = new Cheats::CheatRepository(this);
	connect(
	    m_repository, &Cheats::CheatRepository::CatalogLoaded, this,
	    [this](QList<Cheats::RemoteFile> files, const QString& error) {
		    m_remote_files = std::move(files);
		    m_files->clear();

		    QList<qsizetype> your_indices;
		    QList<qsizetype> other_indices;

		    for (qsizetype i = 0; i < m_remote_files.size(); ++i) {
			    if (!m_version.isEmpty() &&
			        Cheats::VersionsMatch(m_remote_files[i].version, m_version)) {
				    your_indices.append(i);
			    } else {
				    other_indices.append(i);
			    }
		    }

		    auto make_item = [this](QTreeWidgetItem* parent, qsizetype index,
		                            bool is_your_version) {
			    const auto& file = m_remote_files[index];
			    auto*       item = new QTreeWidgetItem(parent);
			    item->setText(0, file.title.isEmpty() ? file.name : file.title);
			    item->setText(1, Cheats::ModuleTypeToString(file.type));
			    item->setText(2, is_your_version ? tr("%1 (installed)").arg(file.version)
			                                     : file.version);
			    item->setText(3, file.source);
			    item->setText(4, file.author.isEmpty() ? tr("Community") : file.author);
			    item->setData(0, Qt::UserRole, static_cast<int>(index));
			    const auto tip = file.description.isEmpty() ? file.title : file.description;
			    for (int col = 0; col < 5; ++col) {
				    item->setToolTip(col, tip);
			    }
			    return item;
		    };

		    m_your_version_group = new QTreeWidgetItem(m_files);
		    m_your_version_group->setText(
		        0, tr("Your version (%1):").arg(m_version.isEmpty() ? tr("Unknown") : m_version));
		    auto bold_font = m_your_version_group->font(0);
		    bold_font.setBold(true);
		    m_your_version_group->setFont(0, bold_font);
		    m_your_version_group->setFirstColumnSpanned(true);
		    m_your_version_group->setFlags(Qt::ItemIsEnabled);

		    if (your_indices.isEmpty()) {
			    auto* empty_item = new QTreeWidgetItem(m_your_version_group);
			    empty_item->setText(0,
			                        tr("(No modules found for installed version %1)")
			                            .arg(m_version.isEmpty() ? tr("Unknown") : m_version));
			    empty_item->setFirstColumnSpanned(true);
			    empty_item->setFlags(Qt::NoItemFlags);
		    } else {
			    for (auto idx: your_indices) {
				    make_item(m_your_version_group, idx, true);
			    }
		    }

		    m_other_version_group = new QTreeWidgetItem(m_files);
		    m_other_version_group->setText(0, tr("Other versions:"));
		    m_other_version_group->setFont(0, bold_font);
		    m_other_version_group->setFirstColumnSpanned(true);
		    m_other_version_group->setFlags(Qt::ItemIsEnabled);

		    if (other_indices.isEmpty()) {
			    auto* empty_item = new QTreeWidgetItem(m_other_version_group);
			    empty_item->setText(0, tr("(None)"));
			    empty_item->setFirstColumnSpanned(true);
			    empty_item->setFlags(Qt::NoItemFlags);
		    } else {
			    for (auto idx: other_indices) {
				    make_item(m_other_version_group, idx, false);
			    }
		    }

		    m_your_version_group->setExpanded(true);
		    m_other_version_group->setExpanded(true);

		    FilterChanged(m_filter_combo->currentIndex());

		    auto status =
		        m_remote_files.isEmpty() && error.isEmpty()
		            ? tr("No remote modules found for %1.").arg(m_title_id)
		            : tr("%1 remote module(s) available for %2.").arg(m_remote_files.size()).arg(m_title_id);
		    if (!error.isEmpty()) {
			    status = tr("%1\nNotice: %2\nUse Refresh to retry.").arg(status, error);
		    }
		    m_remote_status->setText(status);
		    m_filter_combo->setEnabled(true);
		    m_refresh->setEnabled(true);
	    });

	connect(
	    m_repository, &Cheats::CheatRepository::FileLoaded, this,
	    [this](const QByteArray& data, QString error) {
		    const auto* item = m_files->currentItem();
		    if (item == nullptr || !item->data(0, Qt::UserRole).isValid()) {
			    return;
		    }
		    const auto index = item->data(0, Qt::UserRole).toInt();
		    if (index < 0 || index >= m_remote_files.size()) {
			    return;
		    }
		    const auto& file = m_remote_files[index];

		    if (error.isEmpty()) {
			    error = Cheats::ParseDocument(data, &m_remote_document);
		    }
		    if (!error.isEmpty()) {
			    m_preview_status->setText(
			        tr("Could not preview this file: %1\nSelect another file or refresh to retry.")
			            .arg(error));
			    return;
		    }

		    if (file.type == Cheats::ModuleType::Config) {
			    const auto summary = Cheats::SummarizeConfigPreset(m_remote_document);
			    m_preview->clear();
			    for (const auto& setting: summary.settings) {
				    new QListWidgetItem(
				        QStringLiteral("%1: %2").arg(setting.first, setting.second), m_preview);
			    }
			    const auto author = summary.author.isEmpty() ? file.author : summary.author;
			    m_remote_files[index].author = author;
			    const_cast<QTreeWidgetItem*>(item)->setText(4, author.isEmpty() ? tr("Community") : author);

			    QString metadata =
			        tr("Configuration Preset: %1\nVersion: %2 · Source: %3 · Author: %4")
			            .arg(file.title, file.version, file.source,
			                 author.isEmpty() ? tr("Community") : author);
			    if (!summary.description.isEmpty()) {
				    metadata += QStringLiteral("\n\n") + summary.description;
			    }
			    if (!m_version.isEmpty() && !Cheats::VersionsMatch(file.version, m_version)) {
				    metadata += QStringLiteral("\n\n") +
				                tr("Notice: This preset is for game version %1 (installed: %2).")
				                    .arg(file.version, m_version);
			    }
			    metadata += QStringLiteral("\n\n") +
			                tr("Click \"Apply Config Preset\" to update this game's custom settings.");
			    m_preview_status->setText(metadata);
			    m_import->setText(tr("Apply Config Preset"));
			    m_import->setEnabled(m_config_callback != nullptr);
			    return;
		    }

		    // Patch or Cheat
		    ShowMods(m_preview, m_remote_document, false);
		    const auto root = m_remote_document.object();
		    QString    author = file.author;
		    const bool is_default_author =
		        author.isEmpty() ||
		        author == QStringLiteral("HEN Cheats Contributors") ||
		        author == tr("HEN Cheats Contributors") ||
		        author == QStringLiteral("Community") ||
		        author == tr("Community");
		    if (is_default_author) {
			    QStringList creators;
			    for (const auto& val: root.value(QStringLiteral("credits")).toArray()) {
				    if (val.isString()) {
					    creators.append(val.toString());
				    }
			    }
			    if (!creators.isEmpty()) {
				    author = creators.join(QStringLiteral(", "));
			    } else if (root.contains(QStringLiteral("author")) &&
			               root.value(QStringLiteral("author")).isString() &&
			               !root.value(QStringLiteral("author")).toString().trimmed().isEmpty()) {
				    author = root.value(QStringLiteral("author")).toString().trimmed();
			    }
		    }
		    if (author.isEmpty()) {
			    author = tr("Community");
		    }
		    m_remote_files[index].author = author;
		    const_cast<QTreeWidgetItem*>(item)->setText(4, author);

		    const auto type_str = Cheats::ModuleTypeToString(file.type);
		    QString    metadata = tr("%1: %2\nVersion: %3 · Source: %4 · Author: %5")
		                           .arg(type_str, file.title, file.version, file.source, author);
		    if (!file.description.isEmpty()) {
			    metadata += QStringLiteral("\n\n") + file.description;
		    }
		    const bool ver_match =
		        m_version.isEmpty() || Cheats::VersionsMatch(file.version, m_version);
		    if (!ver_match) {
			    metadata += QStringLiteral("\n\n") +
			                tr("Notice: This %1 is for game version %2 (installed: %3).")
			                    .arg(type_str.toLower(), file.version, m_version);
		    }

		    const auto val_error =
		        Cheats::ValidateImport(m_remote_document, m_title_id, m_version, m_process, true);
		    if (!val_error.isEmpty()) {
			    m_preview_status->setText(metadata + QStringLiteral("\n\n") + val_error);
			    m_import->setEnabled(false);
			    return;
		    }

		    metadata += QStringLiteral("\n\n") +
		                tr("Import to Local to select and enable. Changes take effect on the next game launch.");
		    m_preview_status->setText(metadata);
		    m_import->setText(tr("Import to Local"));
		    m_import->setEnabled(true);
	    });

	Load();
	LoadRemote();
}

void PatchesDialog::FilterChanged(int index) {
	auto apply_filter = [this, index](QTreeWidgetItem* group) {
		if (group == nullptr) {
			return;
		}
		int visible_count = 0;
		for (int i = 0; i < group->childCount(); ++i) {
			auto* child = group->child(i);
			if (!child->data(0, Qt::UserRole).isValid()) {
				continue;
			}
			const auto file_idx = child->data(0, Qt::UserRole).toInt();
			if (file_idx < 0 || file_idx >= m_remote_files.size()) {
				continue;
			}
			const auto type = m_remote_files[file_idx].type;
			bool       visible = true;
			if (index == 1) {
				visible = (type == Cheats::ModuleType::Patch);
			} else if (index == 2) {
				visible = (type == Cheats::ModuleType::Cheat);
			} else if (index == 3) {
				visible = (type == Cheats::ModuleType::Config);
			}
			child->setHidden(!visible);
			if (visible) {
				++visible_count;
			}
		}
		if (group->childCount() > 0 && group->child(0)->data(0, Qt::UserRole).isValid()) {
			group->setHidden(visible_count == 0);
		}
	};
	apply_filter(m_your_version_group);
	apply_filter(m_other_version_group);

	if (m_files->currentItem() != nullptr && m_files->currentItem()->isHidden()) {
		m_files->setCurrentItem(nullptr);
		m_preview->clear();
		m_remote_document = {};
		m_preview_status->clear();
		m_import->setEnabled(false);
	}
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
		m_status->setText(tr("No local cheats or patches: %1").arg(path));
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
	m_status->setText(tr("Loaded %1 cheat(s) and patch(es) from %2.").arg(m_patches->count()).arg(path));
}

void PatchesDialog::Save() {
	QList<bool> enabled;
	for (int index = 0; index < m_patches->count(); ++index) {
		enabled.append(m_patches->item(index)->checkState() == Qt::Checked);
	}
	const auto document = Cheats::WithSelection(m_local_document, enabled, m_version);
	const auto error = Cheats::SaveDocument(Cheats::PlanPath(m_title_id), document, m_local_data);
	if (!error.isEmpty()) {
		m_status->setText(error);
		return;
	}
	m_local_document = document;
	m_local_data     = document.toJson();
	m_status->setText(tr("Selection saved. Changes take effect on the next game launch."));
}

void PatchesDialog::LoadRemote() {
	m_repository->CancelFetch();
	m_files->clear();
	m_your_version_group  = nullptr;
	m_other_version_group = nullptr;
	m_filter_combo->setEnabled(false);
	m_remote_files.clear();
	m_preview->clear();
	m_remote_document = {};
	m_import->setEnabled(false);
	m_refresh->setEnabled(false);
	m_preview_status->clear();
	m_remote_status->setText(tr("Loading remote modules from KytyPS5-Modules and HEN Cheats…"));
	m_repository->Load(m_title_id);
}

void PatchesDialog::PreviewRemote() {
	m_repository->CancelFetch();
	m_preview->clear();
	m_remote_document = {};
	m_import->setEnabled(false);
	m_preview_status->clear();
	const auto* item = m_files->currentItem();
	if (item == nullptr || !item->data(0, Qt::UserRole).isValid()) {
		return;
	}
	const auto index = item->data(0, Qt::UserRole).toInt();
	if (index < 0 || index >= m_remote_files.size()) {
		return;
	}
	const auto& file = m_remote_files[index];
	m_preview_status->setText(tr("Loading preview for %1…").arg(file.title));
	m_repository->Fetch(file);
}

void PatchesDialog::ImportRemote() {
	const auto* item = m_files->currentItem();
	if (item == nullptr || item->isHidden() || !item->data(0, Qt::UserRole).isValid()) {
		return;
	}
	const auto index = item->data(0, Qt::UserRole).toInt();
	if (index < 0 || index >= m_remote_files.size()) {
		return;
	}
	const auto& file = m_remote_files[index];

	if (file.type == Cheats::ModuleType::Config) {
		if (m_config_callback == nullptr) {
			m_preview_status->setText(tr("Cannot apply config: configuration callback not available."));
			return;
		}
		if (QMessageBox::question(
		        this, tr("Apply configuration preset?"),
		        tr("Apply preset \"%1\" (Source: %2, Author: %3) to %4?\n\nThis will update the game's custom configuration settings.")
		            .arg(file.title, file.source, file.author, m_game_name),
		        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) {
			return;
		}
		QString error;
		if (!m_config_callback(m_remote_document, &error)) {
			m_preview_status->setText(tr("Failed to apply configuration preset: %1").arg(error));
			return;
		}
		QMessageBox::information(
		    this, tr("Config preset applied"),
		    tr("Configuration preset \"%1\" applied successfully. Custom settings are now enabled for this game.")
		        .arg(file.title));
		return;
	}

	// Patch or Cheat
	const auto error = Cheats::ValidateImport(m_remote_document, m_title_id, m_version, m_process, true);
	if (!error.isEmpty()) {
		m_preview_status->setText(error);
		return;
	}
	if (!m_local_readable) {
		m_preview_status->setText(
		    tr("Could not read the local cheat file. Open Local and reload it before importing."));
		return;
	}

	const auto doc_ver = m_remote_document.object().value(QStringLiteral("version")).toString().trimmed();
	const auto effective_ver = !doc_ver.isEmpty() ? doc_ver : file.version;
	const bool ver_match = m_version.isEmpty() || Cheats::VersionsMatch(effective_ver, m_version);
	if (!ver_match) {
		if (QMessageBox::question(
		        this, tr("Version mismatch"),
		        tr("This %1 was created for version %2, but your installed game version is %3.\n\nDo you want to import it anyway?")
		            .arg(Cheats::ModuleTypeToString(file.type).toLower(), effective_ver, m_version),
		        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
			return;
		}
	}

	const auto path = Cheats::PlanPath(m_title_id);
	bool       merge = false;
	if (QFile::exists(path) && m_local_document.isObject() &&
	    m_local_document.object().value(QStringLiteral("mods")).toArray().size() > 0) {
		QMessageBox msg(this);
		msg.setWindowTitle(tr("Import to Local"));
		msg.setText(tr("Existing local cheats/patches found for %1.").arg(m_title_id));
		msg.setInformativeText(tr("Do you want to add these new mods to your existing list, or replace the local file entirely?"));
		auto* add_btn     = msg.addButton(tr("Add / Merge"), QMessageBox::AcceptRole);
		auto* replace_btn = msg.addButton(tr("Replace"), QMessageBox::DestructiveRole);
		auto* cancel_btn  = msg.addButton(QMessageBox::Cancel);
		msg.setDefaultButton(add_btn);
		msg.exec();
		if (msg.clickedButton() != add_btn && msg.clickedButton() != replace_btn) {
			return;
		}
		merge = (msg.clickedButton() == add_btn);
	}

	QJsonDocument document;
	if (merge) {
		QString merge_error;
		document = Cheats::MergeDocuments(m_local_document, m_remote_document, file.source,
		                                  file.author, m_process, m_version, &merge_error, m_title_id,
		                                  !ver_match);
		if (!document.isObject()) {
			m_preview_status->setText(merge_error.isEmpty() ? tr("Failed to merge cheat files.") : merge_error);
			return;
		}
	} else {
		document = Cheats::PrepareForImport(m_remote_document, file.source, file.author, m_process, m_version, m_title_id);
	}

	const auto save_error = Cheats::SaveDocument(path, document, m_local_data);
	if (!save_error.isEmpty()) {
		m_preview_status->setText(save_error);
		return;
	}
	Load();
	m_status->setText(
	    tr("Imported mods from %1. Check the patches/cheats to enable and click Apply selection.")
	        .arg(file.title));
	m_tabs->setCurrentIndex(1);
}
