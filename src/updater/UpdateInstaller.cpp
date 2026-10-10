#include "UpdateInstaller.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace Kyty::Updater {
namespace {
namespace fs = std::filesystem;
struct Entry {
	fs::path      relative;
	fs::file_type type;
};
struct Plan {
	fs::path           source;
	fs::path           target;
	std::vector<Entry> entries;
};

bool IsWithin(const fs::path& path, const fs::path& root) {
	auto part = path.begin();
	for (const auto& component: root) {
		if (part == path.end() || *part++ != component) {
			return false;
		}
	}
	return true;
}

bool IsRelative(const fs::path& path) {
	if (path.empty() || path.is_absolute() || path.has_root_name()) {
		return false;
	}
	for (const auto& component: path) {
		if (component == ".." || component == "." || component.empty()) {
			return false;
		}
	}
	return true;
}

fs::file_type Type(const fs::path& path) {
	std::error_code error;
	const auto      status = fs::symlink_status(path, error);
	if (error && error != std::errc::no_such_file_or_directory) {
		throw fs::filesystem_error("Cannot inspect update path", path, error);
	}
	return status.type();
}

void CheckLink(const fs::path& path, const fs::path& root) {
	const auto link = fs::read_symlink(path);
	if (link.is_absolute() || !IsWithin(fs::canonical(path), root)) {
		throw std::runtime_error("Update contains a link outside its installation: " +
		                         path.string());
	}
}

void CheckDestination(const fs::path& target, const Entry& entry) {
	const auto destination = target / entry.relative;
	const auto type        = Type(destination);
	if (type == fs::file_type::not_found) {
		return;
	}
	if ((entry.type == fs::file_type::directory) != (type == fs::file_type::directory) ||
	    (type != fs::file_type::regular && type != fs::file_type::directory &&
	     type != fs::file_type::symlink)) {
		throw std::runtime_error("Update would replace a different file type: " +
		                         destination.string());
	}
	if (type == fs::file_type::symlink) {
		CheckLink(destination, target);
	}
}

Plan MakePlan(const fs::path& source, const fs::path& target, const fs::path& launcher) {
	if (!IsRelative(launcher) || Type(source) != fs::file_type::directory ||
	    Type(target) != fs::file_type::directory) {
		throw std::runtime_error(
		    "Update source, installation directory, or launcher path is invalid.");
	}
	Plan plan {fs::canonical(source), fs::canonical(target), {}};
	if (IsWithin(plan.target, plan.source)) {
		throw std::runtime_error("Update source must not contain the installation directory.");
	}
	// macOS releases also have flat runtime files; install only the signed bundle.
	fs::path scope;
	if (launcher.begin()->extension() == ".app") {
		scope = *launcher.begin();
	}
	const auto source_root = scope.empty() ? plan.source : plan.source / scope;
	if (Type(source_root) != fs::file_type::directory) {
		throw std::runtime_error("The release does not contain the application bundle.");
	}
	const auto binary_directory = launcher.parent_path();
#ifdef _WIN32
	const fs::path suffix = ".exe";
#else
	const fs::path suffix;
#endif
	for (const auto& required: {launcher, binary_directory / ("kyty_emulator" + suffix.string()),
	                            binary_directory / ("kyty_updater" + suffix.string())}) {
		if (Type(plan.source / required) != fs::file_type::regular) {
			throw std::runtime_error("Release is missing an executable: " + required.string());
		}
#ifndef _WIN32
		const auto executable =
		    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
		if ((fs::status(plan.source / required).permissions() & executable) == fs::perms::none) {
			throw std::runtime_error("Release executable lacks execute permissions: " +
			                         required.string());
		}
#endif
	}
	if (!scope.empty()) {
		plan.entries.push_back({scope, fs::file_type::directory});
	}
	for (const auto& item: fs::recursive_directory_iterator(source_root)) {
		const auto relative = item.path().lexically_relative(plan.source);
		const auto type     = item.symlink_status().type();
		if (relative.begin()->string().starts_with(".kyty-update")) {
			throw std::runtime_error("Release uses a reserved updater filename.");
		}
		if (type == fs::file_type::symlink) {
			CheckLink(item.path(), source_root);
		} else if (type != fs::file_type::directory && type != fs::file_type::regular) {
			throw std::runtime_error("Release contains an unsupported file: " + relative.string());
		}
		plan.entries.push_back({relative, type});
	}
	// Checking directories before their children prevents destination symlink traversal.
	std::sort(plan.entries.begin(), plan.entries.end(),
	          [](const Entry& left, const Entry& right) { return left.relative < right.relative; });
	for (const auto& entry: plan.entries) {
		CheckDestination(plan.target, entry);
	}
	return plan;
}

struct InstallLock {
	fs::path path;
	void     Acquire(const fs::path& target) {
		auto candidate = target / ".kyty-update-lock";
		if (!fs::create_directory(candidate)) {
			throw std::runtime_error(
			    "Another update is running, or a previous update was interrupted. "
			    "Close all updater processes, inspect retained backups, then remove " +
			    candidate.string());
		}
		path = std::move(candidate);
	}
	~InstallLock() {
		if (!path.empty()) {
			std::error_code ignored;
			// An incomplete rollback leaves backups here and keeps the lock for recovery.
			fs::remove(path, ignored);
		}
	}
};

struct Change {
	fs::path relative;
	bool     old_moved = false;
	bool     installed = false;
};
} // namespace

Result Validate(const fs::path& source, const fs::path& target, const fs::path& launcher_relative) {
	try {
		MakePlan(source, target, launcher_relative);
		// Check actual write access, including ACLs, and interrupted/concurrent updates.
		InstallLock lock;
		lock.Acquire(target);
		return {true, {}};
	} catch (const std::exception& error) {
		return {false, error.what()};
	}
}

Result Install(const fs::path& source, const fs::path& target, const fs::path& launcher_relative) {
	InstallLock           lock;
	fs::path              backup;
	std::vector<Change>   changes;
	std::vector<fs::path> created_directories;
	try {
		const auto plan = MakePlan(source, target, launcher_relative);
		lock.Acquire(plan.target);
		backup = lock.path / "backup";
		changes.reserve(plan.entries.size());
		created_directories.reserve(plan.entries.size());
		// The launcher extracts beside the installation, so files can move directly
		// from that staging directory without another full copy of the release.
		for (const auto& entry: plan.entries) {
			CheckDestination(plan.target, entry);
			const auto destination = plan.target / entry.relative;
			if (entry.type == fs::file_type::directory) {
				if (fs::create_directory(destination)) {
					created_directories.push_back(destination);
				}
				continue;
			}
			changes.push_back({entry.relative});
			auto& change = changes.back();
			if (Type(destination) != fs::file_type::not_found) {
				fs::create_directories((backup / entry.relative).parent_path());
				fs::rename(destination, backup / entry.relative);
				change.old_moved = true;
			}
			fs::rename(plan.source / entry.relative, destination);
			change.installed = true;
		}
		std::error_code cleanup_error;
		fs::remove_all(backup, cleanup_error);
		return {true, cleanup_error ? "Update installed; backup cleanup failed: " + backup.string()
		                            : std::string {}};
	} catch (const std::exception& error) {
		std::string message  = error.what();
		bool        restored = true;
		for (auto change = changes.rbegin(); change != changes.rend(); ++change) {
			try {
				const auto destination = target / change->relative;
				if (change->installed) {
					fs::rename(destination, source / change->relative);
				}
				if (change->old_moved) {
					fs::rename(backup / change->relative, destination);
				}
			} catch (const std::exception& rollback_error) {
				restored = false;
				message += "\nRollback failed: " + std::string(rollback_error.what());
			}
		}
		for (auto directory = created_directories.rbegin(); directory != created_directories.rend();
		     ++directory) {
			std::error_code ignored;
			fs::remove(*directory, ignored);
		}
		if (!backup.empty()) {
			if (restored) {
				std::error_code ignored;
				fs::remove_all(backup, ignored);
			} else {
				message += "\nBackup retained at: " + backup.string();
			}
		}
		return {false, message};
	}
}
} // namespace Kyty::Updater
