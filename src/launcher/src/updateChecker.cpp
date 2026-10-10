#include "updateChecker.h"

#include "kytyGitVersion.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QTemporaryDir>

namespace {

constexpr qint64 MAX_METADATA_SIZE = 2 * 1024 * 1024;

QString LauncherRelativePath() {
#if defined(_WIN32)
	return QStringLiteral("launcher.exe");
#elif defined(__APPLE__)
	return QStringLiteral("KytyPS5.app/Contents/MacOS/KytyPS5");
#else
	return QStringLiteral("launcher");
#endif
}

QString InstallDirectory() {
	QDir dir(QCoreApplication::applicationDirPath());
#if defined(__APPLE__)
	// Official macOS releases are a bundle; never update a flat development build.
	if (!dir.cdUp() || !dir.cdUp() || dir.dirName() != QStringLiteral("KytyPS5.app") ||
	    !dir.cdUp()) {
		return {};
	}
#endif
	return dir.absolutePath();
}

QString HelperName() {
#if defined(_WIN32)
	return QStringLiteral("kyty_updater.exe");
#else
	return QStringLiteral("kyty_updater");
#endif
}

QNetworkRequest Request(const QUrl& url) {
	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "KytyPS5-Updater");
	request.setTransferTimeout(30000);
	return request;
}

QString TarProgram() {
#if defined(_WIN32)
	// Use the OS tool, not a tar.exe from the download or current directory.
	return QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
	    .filePath(QStringLiteral("System32/tar.exe"));
#else
	return QStringLiteral("/usr/bin/tar");
#endif
}

} // namespace

UpdateChecker::UpdateChecker(QWidget* parent): QObject(parent), m_parent(parent), m_network(this) {}

UpdateChecker::~UpdateChecker() {
	if (m_reply) {
		m_reply->disconnect(this);
		m_reply->abort();
	}
	// Extraction must stop before QTemporaryDir removes its output.
	m_prepare.disconnect(this);
	if (m_prepare.state() != QProcess::NotRunning) {
		m_prepare.kill();
		m_prepare.waitForFinished();
	}
}

bool UpdateChecker::IsSupported() {
#if defined(KYTY_OFFICIAL_BUILD) && defined(NDEBUG)
	return !QString::fromLatin1(KYTY_GIT_HASH).endsWith(QStringLiteral("-dirty")) &&
	       !UpdateRelease::PlatformSuffix().isEmpty();
#else
	return false;
#endif
}

void UpdateChecker::ShowPreviousResult(QWidget* parent) {
	const auto directory = InstallDirectory();
	QFile      result(QDir(directory).filePath(QStringLiteral(".kyty-update-result")));
	if (directory.isEmpty() || !result.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto status  = result.readLine().trimmed();
	auto       message = QString::fromUtf8(result.read(64 * 1024)).trimmed();
	result.close();
	result.remove();
	if (status != "success" && status != "failure") {
		return;
	}
	const bool success = status == "success";
	if (message.isEmpty()) {
		message = success ? tr("The update was installed successfully.")
		                  : tr("The update could not be installed.");
	}
	QMessageBox(success ? QMessageBox::Information : QMessageBox::Warning, tr("KytyPS5 Update"),
	            message, QMessageBox::Ok, parent)
	    .exec();
}

void UpdateChecker::FetchRelease(const QUrl& url, std::function<void(UpdateRelease::Info)> done) {
	auto request = Request(url);
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
	request.setTransferTimeout(15000);
	auto* reply = m_network.get(request);
	m_reply     = reply;
	reply->setReadBufferSize(MAX_METADATA_SIZE + 1);
	connect(reply, &QNetworkReply::readyRead, this, [reply]() {
		if (reply->bytesAvailable() > MAX_METADATA_SIZE) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, done = std::move(done)]() {
		UpdateRelease::Info info;
		if (reply->bytesAvailable() > MAX_METADATA_SIZE) {
			info.error = tr("The update response is too large.");
		} else if (reply->error() != QNetworkReply::NoError) {
			info.error = reply->errorString();
		} else {
			info = UpdateRelease::Parse(reply->readAll(), UpdateRelease::PlatformSuffix());
		}
		m_reply = nullptr;
		reply->deleteLater();
		done(info);
	});
}

void UpdateChecker::Check(bool manual) {
	if (!IsSupported() || m_busy) {
		return;
	}
	m_busy = true;
	emit CheckingChanged(true);
	FetchRelease(QUrl(UpdateRelease::LATEST_URL), [this, manual](UpdateRelease::Info latest) {
		m_release = std::move(latest);
		if (!m_release.published_at.isValid()) {
			Finish(m_release.error, manual);
		} else if (m_release.tag == QString::fromLatin1(KYTY_RELEASE_TAG)) {
			CheckCurrent(m_release, manual);
		} else {
			// Date/hash tags cannot order releases published on the same day.
			const auto tag = QUrl::toPercentEncoding(QString::fromLatin1(KYTY_RELEASE_TAG));
			FetchRelease(QUrl(UpdateRelease::TAG_URL.arg(QString::fromLatin1(tag))),
			             [this, manual](const UpdateRelease::Info& current) {
				             CheckCurrent(current, manual);
			             });
		}
	});
}

void UpdateChecker::CheckCurrent(const UpdateRelease::Info& current, bool manual) {
	if (current.tag != QString::fromLatin1(KYTY_RELEASE_TAG) || !current.published_at.isValid()) {
		Finish(tr("Could not verify the currently installed release. Please try again later."),
		       manual);
		return;
	}
	const auto relation = UpdateRelease::Compare(m_release, current);
	if (relation == UpdateRelease::VersionRelation::Newer) {
		if (m_release.error.isEmpty()) {
			OfferUpdate();
		} else {
			Finish(m_release.error, manual);
		}
		return;
	}
	if (relation == UpdateRelease::VersionRelation::Unknown) {
		Finish(tr("Could not determine whether the available release is newer."), manual);
		return;
	}
	Finish();
	if (manual) {
		QMessageBox::information(m_parent, tr("Update Check"),
		                         tr("No newer release is available."));
	}
}

void UpdateChecker::OfferUpdate() {
	QMessageBox question(QMessageBox::Information, tr("KytyPS5 Update"),
	                     tr("Version %1 is available.\n\nCurrent: %2\n\n"
	                        "Download and install it now? The launcher will restart. Your settings "
	                        "and games are kept.")
	                         .arg(m_release.tag, QString::fromLatin1(KYTY_RELEASE_TAG)),
	                     QMessageBox::Yes | QMessageBox::Open | QMessageBox::Cancel, m_parent);
	question.button(QMessageBox::Yes)->setText(tr("Install Update"));
	question.button(QMessageBox::Open)->setText(tr("Release Notes"));
	question.setDefaultButton(QMessageBox::Yes);
	const auto answer = question.exec();
	if (answer == QMessageBox::Yes) {
		Download();
		return;
	}
	if (answer == QMessageBox::Open) {
		QDesktopServices::openUrl(m_release.page_url);
	}
	Finish();
}

void UpdateChecker::Download() {
	if (m_game_running) {
		Finish(tr("Close the running game before installing an update."));
		return;
	}
	const auto directory = InstallDirectory();
	const auto helper    = QDir(QCoreApplication::applicationDirPath()).filePath(HelperName());
	if (directory.isEmpty() || !QFileInfo::exists(helper) || !QFileInfo::exists(TarProgram())) {
		Finish(tr("This installation cannot update itself. Download the release from GitHub and "
		          "extract it manually."));
		return;
	}
	m_workspace = std::make_unique<QTemporaryDir>(
	    QDir(directory).filePath(QStringLiteral(".kyty-update-XXXXXX")));
	if (!m_workspace->isValid()) {
		Finish(tr("The installation folder is not writable. Move KytyPS5 to a writable folder and "
		          "try again."));
		return;
	}
	if (!QFile::copy(helper, m_workspace->filePath(HelperName())) ||
	    !QDir(m_workspace->path()).mkdir(QStringLiteral("payload"))) {
		Finish(tr("Could not prepare the update folder."));
		return;
	}
	m_download.setFileName(
	    m_workspace->filePath(QStringLiteral("package") + UpdateRelease::PlatformSuffix()));
	if (!m_download.open(QIODevice::WriteOnly)) {
		Finish(m_download.errorString());
		return;
	}
	m_hash.reset();
	m_download_error.clear();
	m_canceled   = false;
	m_installing = true;
	emit InstallingChanged(true);
	m_progress = new QProgressDialog(tr("Downloading %1…").arg(m_release.tag), tr("Cancel"), 0, 100,
	                                 m_parent);
	m_progress->setWindowTitle(tr("KytyPS5 Update"));
	m_progress->setWindowModality(Qt::ApplicationModal);
	m_progress->setAutoClose(false);
	m_progress->setAutoReset(false);
	m_progress->setMinimumDuration(0);
	connect(m_progress, &QProgressDialog::canceled, this, [this]() {
		m_canceled = true;
		if (m_reply) {
			m_reply->abort();
		} else if (m_prepare.state() != QProcess::NotRunning) {
			m_prepare.kill();
		}
	});
	auto* reply = m_network.get(Request(m_release.download_url));
	m_reply     = reply;
	reply->setReadBufferSize(1024 * 1024);
	connect(reply, &QNetworkReply::readyRead, this, [this, reply]() { ReadDownload(reply); });
	connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
		if (m_progress) {
			m_progress->setValue(
			    static_cast<int>(qMin<qint64>(99, received * 100 / m_release.size)));
		}
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		ReadDownload(reply);
		const auto error      = reply->error();
		const auto error_text = reply->errorString();
		m_reply               = nullptr;
		reply->deleteLater();
		const auto size    = m_download.size();
		const bool flushed = m_download.flush();
		m_download.close();
		if (m_canceled) {
			Finish();
		} else if (!m_download_error.isEmpty()) {
			Finish(m_download_error);
		} else if (error != QNetworkReply::NoError) {
			Finish(error_text);
		} else if (!flushed || size != m_release.size || m_hash.result() != m_release.sha256) {
			Finish(tr("The update failed its size or SHA-256 verification. Please try again."));
		} else {
			PreparePackage();
		}
	});
}

void UpdateChecker::ReadDownload(QNetworkReply* reply) {
	const auto data = reply->readAll();
	if (data.isEmpty() || m_canceled || !m_download_error.isEmpty()) {
		return;
	}
	if (m_download.size() + data.size() > m_release.size) {
		m_download_error = tr("The downloaded file is larger than the release asset.");
	} else if (m_download.write(data) != data.size()) {
		m_download_error = m_download.errorString();
	} else {
		m_hash.addData(data);
	}
	if (!m_download_error.isEmpty() && !reply->isFinished()) {
		reply->abort();
	}
}

void UpdateChecker::PreparePackage() {
	m_progress->setLabelText(tr("Preparing the update…"));
	m_progress->setRange(0, 0);
	RunPreparation(TarProgram(),
	               {QStringLiteral("-xf"), m_download.fileName(), QStringLiteral("-C"),
	                m_workspace->filePath(QStringLiteral("payload")),
	                QStringLiteral("--no-same-owner"), QStringLiteral("--no-same-permissions")},
	               false);
}

void UpdateChecker::RunPreparation(const QString& program, const QStringList& arguments,
                                   bool validate) {
	m_prepare.disconnect(this);
	m_prepare.setProgram(program);
	m_prepare.setArguments(arguments);
	m_prepare.setWorkingDirectory(m_workspace->path());
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.remove(QStringLiteral("TAR_OPTIONS"));
	m_prepare.setProcessEnvironment(environment);
	connect(&m_prepare, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			Finish(m_prepare.errorString());
		}
	});
	connect(&m_prepare, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
	        [this, validate](int exit_code, QProcess::ExitStatus status) {
		        if (m_canceled) {
			        Finish();
		        } else if (exit_code != 0 || status != QProcess::NormalExit) {
			        Finish(
			            tr("Could not prepare the update:\n%1")
			                .arg(QString::fromUtf8(m_prepare.readAllStandardError()).left(4096)));
		        } else if (!validate) {
			        RunPreparation(m_workspace->filePath(HelperName()),
			                       {QStringLiteral("--validate"),
			                        m_workspace->filePath(QStringLiteral("payload")),
			                        InstallDirectory(), LauncherRelativePath()},
			                       true);
		        } else {
			        Restart();
		        }
	        });
	m_prepare.start();
}

void UpdateChecker::Restart() {
	if (m_game_running || m_canceled) {
		Finish(tr("The update was canceled. Close any running game before trying again."));
		return;
	}
	m_progress->setLabelText(tr("Restarting to install the update…"));
	m_progress->setCancelButton(nullptr);
	QProcess helper;
	helper.setProgram(m_workspace->filePath(HelperName()));
	helper.setArguments({QStringLiteral("--install"),
	                     m_workspace->filePath(QStringLiteral("payload")), InstallDirectory(),
	                     LauncherRelativePath(),
	                     QString::number(QCoreApplication::applicationPid()), QDir::currentPath()});
	helper.setWorkingDirectory(m_workspace->path());
	helper.setStandardOutputFile(m_workspace->filePath(QStringLiteral("helper.log")));
	helper.setStandardErrorFile(m_workspace->filePath(QStringLiteral("helper.log")),
	                            QIODevice::Append);
	if (!helper.startDetached()) {
		Finish(tr("Could not start the update installer."));
		return;
	}
	m_workspace->setAutoRemove(false);
	QApplication::quit();
}

void UpdateChecker::Finish(QString error, bool report) {
	const auto release_page = m_release.page_url;
	m_release               = {};
	m_download.close();
	m_workspace.reset();
	if (m_progress) {
		m_progress->disconnect(this);
		m_progress->hide();
		m_progress->deleteLater();
		m_progress = nullptr;
	}
	m_busy       = false;
	m_installing = false;
	emit CheckingChanged(false);
	emit InstallingChanged(false);
	if (error.isEmpty()) {
		return;
	}
	qWarning() << "KytyPS5 updater:" << error;
	if (!report) {
		return;
	}
	QMessageBox message(QMessageBox::Warning, tr("KytyPS5 Update"), error, QMessageBox::Close,
	                    m_parent);
	if (!release_page.isEmpty()) {
		message.addButton(QMessageBox::Open);
		message.button(QMessageBox::Open)->setText(tr("Open Release Page"));
	}
	if (message.exec() == QMessageBox::Open) {
		QDesktopServices::openUrl(release_page);
	}
}
