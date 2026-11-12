#include "settingsElevation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#ifdef Q_OS_WIN
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#endif

namespace SettingsElevation {

namespace {
constexpr char CONF_FILE_NAME[] = "Kyty.ini";

enum ExitCode {
	EXIT_OK                = 0,
	EXIT_BAD_ARGUMENTS     = 2,
	EXIT_SOURCE_UNREADABLE = 3,
	EXIT_WRITE_FAILED      = 4,
	EXIT_GRANT_FAILED      = 5,
};

#ifdef Q_OS_WIN
// The string SID of the user who runs this process, or empty.
QString CurrentUserSid() {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return {};
	}
	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	QByteArray buffer(static_cast<qsizetype>(size), Qt::Uninitialized);
	QString    sid;
	if (size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
		LPWSTR text = nullptr;
		if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
			sid = QString::fromWCharArray(text);
			LocalFree(text);
		}
	}
	CloseHandle(token);
	return sid;
}

// A file that an elevated process created in %ProgramData% belongs to the administrators, and the
// user who runs the launcher can only read it. Gives that user full control of `path` and clears
// its read-only attribute, so the launcher saves there without asking again.
bool GrantUser(const QString& path, const QString& sid_text) {
	const QString native     = QDir::toNativeSeparators(path);
	const auto*   wpath      = reinterpret_cast<LPCWSTR>(native.utf16());
	const DWORD   attributes = GetFileAttributesW(wpath);
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
		SetFileAttributesW(wpath, attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
	}

	PSID sid = nullptr;
	if (!ConvertStringSidToSidW(reinterpret_cast<LPCWSTR>(sid_text.utf16()), &sid)) {
		return false;
	}
	bool                 ok         = false;
	PACL                 old_dacl   = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (IsValidSid(sid) &&
	    GetNamedSecurityInfoW(wpath, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
	                          &old_dacl, nullptr, &descriptor) == ERROR_SUCCESS) {
		EXPLICIT_ACCESS_W access {};
		access.grfAccessPermissions = GENERIC_ALL;
		access.grfAccessMode        = SET_ACCESS;
		access.grfInheritance       = NO_INHERITANCE;
		access.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType  = TRUSTEE_IS_USER;
		access.Trustee.ptstrName    = static_cast<LPWSTR>(sid);
		PACL new_dacl               = nullptr;
		if (SetEntriesInAclW(1, &access, old_dacl, &new_dacl) == ERROR_SUCCESS) {
			ok = SetNamedSecurityInfoW(const_cast<LPWSTR>(wpath), SE_FILE_OBJECT,
			                           DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl,
			                           nullptr) == ERROR_SUCCESS;
			LocalFree(new_dacl);
		}
		LocalFree(descriptor);
	}
	LocalFree(sid);
	return ok;
}
#endif
} // namespace

bool IsSupported() {
#ifdef Q_OS_WIN
	return true;
#else
	return false;
#endif
}

int CopySettings(const QString& source, const QString& destination, const QString& user_sid) {
	const QFileInfo source_info(source);
	const QFileInfo destination_info(destination);
	// The elevated process must not become a way to overwrite arbitrary files.
	if (!source_info.isFile() || destination_info.fileName() != QLatin1String(CONF_FILE_NAME) ||
	    destination_info.isDir()) {
		return EXIT_BAD_ARGUMENTS;
	}

	QFile in(source_info.absoluteFilePath());
	if (!in.open(QIODevice::ReadOnly)) {
		return EXIT_SOURCE_UNREADABLE;
	}
	const QByteArray data = in.readAll();
	in.close();

	if (!QDir().mkpath(destination_info.absolutePath())) {
		return EXIT_WRITE_FAILED;
	}
	QSaveFile out(destination_info.absoluteFilePath());
	if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
		return EXIT_WRITE_FAILED;
	}
#ifdef Q_OS_WIN
	if (!user_sid.isEmpty() && !GrantUser(destination_info.absoluteFilePath(), user_sid)) {
		return EXIT_GRANT_FAILED;
	}
#else
	Q_UNUSED(user_sid);
#endif
	return EXIT_OK;
}

bool RunElevatedCopy(const QString& source, const QString& destination, QString* error) {
#ifdef Q_OS_WIN
	const auto quote = [](const QString& path) {
		return QLatin1Char('"') + QDir::toNativeSeparators(path) + QLatin1Char('"');
	};
	const QString exe        = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
	QString parameters = QString::fromLatin1(COPY_SETTINGS_ARG) + QLatin1Char(' ') + quote(source) +
	                     QLatin1Char(' ') + quote(destination);
	// The elevated process can run as another account: it gives the file to this user, not to itself.
	const QString user_sid = CurrentUserSid();
	if (!user_sid.isEmpty()) {
		parameters += QLatin1Char(' ') + quote(user_sid);
	}

	SHELLEXECUTEINFOW info {};
	info.cbSize       = sizeof(info);
	info.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
	info.lpVerb       = L"runas";
	info.lpFile       = reinterpret_cast<LPCWSTR>(exe.utf16());
	info.lpParameters = reinterpret_cast<LPCWSTR>(parameters.utf16());
	info.nShow        = SW_HIDE;
	if (!ShellExecuteExW(&info) || info.hProcess == nullptr) {
		const DWORD code = GetLastError();
		if (error != nullptr) {
			*error = code == ERROR_CANCELLED ? QCoreApplication::translate(
			                                       "SettingsElevation",
			                                       "The administrator request was cancelled.")
			                                 : QCoreApplication::translate(
			                                       "SettingsElevation",
			                                       "Could not start the elevated process (error %1).")
			                                       .arg(code);
		}
		return false;
	}

	DWORD exit_code = EXIT_WRITE_FAILED;
	if (WaitForSingleObject(info.hProcess, 30000) == WAIT_OBJECT_0) {
		GetExitCodeProcess(info.hProcess, &exit_code);
	} else {
		exit_code = STILL_ACTIVE;
	}
	CloseHandle(info.hProcess);
	if (exit_code == EXIT_GRANT_FAILED) {
		// Saved; only the permission change failed, so the next save asks again.
		if (error != nullptr) {
			*error = QCoreApplication::translate(
			    "SettingsElevation",
			    "The configuration was saved, but your user could not be given permission to change "
			    "the file, so the launcher will ask again next time.");
		}
		return true;
	}
	if (exit_code != EXIT_OK) {
		if (error != nullptr) {
			*error = QCoreApplication::translate("SettingsElevation",
			                                     "The elevated process could not copy the file (code %1).")
			             .arg(exit_code);
		}
		return false;
	}
	return true;
#else
	Q_UNUSED(source);
	Q_UNUSED(destination);
	if (error != nullptr) {
		*error = QCoreApplication::translate("SettingsElevation",
		                                     "Elevation is not available on this system.");
	}
	return false;
#endif
}

} // namespace SettingsElevation
