#ifndef PATCHES_DIALOG_H
#define PATCHES_DIALOG_H

#include "cheatRepository.h"

#include <QByteArray>
#include <QDialog>
#include <QJsonDocument>
#include <QString>

class Configuration;
class QLabel;
class QListWidget;
class QPushButton;
class QTabWidget;
class QTreeWidget;

class PatchesDialog final: public QDialog {
public:
	explicit PatchesDialog(const Configuration& game, QWidget* parent = nullptr);
	~PatchesDialog() override = default;

private:
	void Load();
	void Save();
	void LoadRemote();
	void PreviewRemote();
	void ImportRemote();

	QString                   m_title_id;
	QString                   m_version;
	QString                   m_process;
	QByteArray                m_local_data;
	QJsonDocument             m_local_document;
	QJsonDocument             m_remote_document;
	bool                      m_local_readable = false;
	Cheats::CheatRepository*  m_repository     = nullptr;
	QList<Cheats::RemoteFile> m_remote_files;
	QTabWidget*               m_tabs           = nullptr;
	QTreeWidget*              m_files          = nullptr;
	QListWidget*              m_preview        = nullptr;
	QLabel*                   m_remote_status  = nullptr;
	QLabel*                   m_preview_status = nullptr;
	QPushButton*              m_refresh        = nullptr;
	QPushButton*              m_import         = nullptr;
	QListWidget*              m_patches        = nullptr;
	QLabel*                   m_status         = nullptr;
	QPushButton*              m_apply          = nullptr;
};

#endif // PATCHES_DIALOG_H
