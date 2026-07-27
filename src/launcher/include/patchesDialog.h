#ifndef PATCHES_DIALOG_H
#define PATCHES_DIALOG_H

#include "common.h"

#include <QDialog>
#include <QString>

class Configuration;
class QLabel;
class QListWidget;
class QPushButton;

class PatchesDialog final: public QDialog {
public:
	explicit PatchesDialog(const Configuration& game, QWidget* parent = nullptr);
	~PatchesDialog() override = default;

	KYTY_QT_CLASS_NO_COPY(PatchesDialog);

	[[nodiscard]] static bool    IsSupportedTitleId(const QString& title_id);
	[[nodiscard]] static QString PatchPlanPath(const QString& title_id);

private:
	void Load();
	void Save();

	QString      m_title_id;
	QListWidget* m_patches = nullptr;
	QLabel*      m_status  = nullptr;
	QPushButton* m_apply   = nullptr;
};

#endif // PATCHES_DIALOG_H
