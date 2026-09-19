#include "libs/saveDataMemory.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <new>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Libs::SaveData {
namespace {
namespace fs = std::filesystem;
constexpr size_t MaxMemory = 32 * 1024 * 1024;
constexpr size_t MaxIcon = 1024 * 1024;
constexpr size_t HeaderSize = 56;
constexpr std::array<uint8_t, 8> Magic {'K', 'Y', 'T', 'Y', 'S', 'D', 'M', '1'};
static_assert(std::endian::native == std::endian::little);

#ifdef KYTY_SAVE_DATA_TESTS
MemoryTesting::FaultHook g_fault_hook = nullptr;
using Stage = MemoryTesting::Stage;
bool Fault(Stage stage) { return g_fault_hook != nullptr && g_fault_hook(stage); }
#else
enum class Stage { Read, Create, Write, Flush, Replace, DirectoryFlush };
bool Fault(Stage) { return false; }
#endif

int IoError() {
#ifdef _WIN32
	switch (GetLastError()) {
		case ERROR_DISK_FULL:
		case ERROR_HANDLE_DISK_FULL: return SAVE_DATA_ERROR_NO_SPACE_FS;
		case ERROR_ACCESS_DENIED:
		case ERROR_WRITE_PROTECT: return SAVE_DATA_ERROR_NO_PERMISSION;
		case ERROR_SHARING_VIOLATION:
		case ERROR_LOCK_VIOLATION: return SAVE_DATA_ERROR_BUSY;
		default: return SAVE_DATA_ERROR_INTERNAL;
	}
#else
	switch (errno) {
		case ENOSPC:
		case EDQUOT: return SAVE_DATA_ERROR_NO_SPACE_FS;
		case EACCES:
		case EPERM:
		case EROFS: return SAVE_DATA_ERROR_NO_PERMISSION;
		case EWOULDBLOCK: return SAVE_DATA_ERROR_BUSY;
		default: return SAVE_DATA_ERROR_INTERNAL;
	}
#endif
}

int FsError(const std::error_code& error) {
	if (error == std::errc::no_space_on_device) return SAVE_DATA_ERROR_NO_SPACE_FS;
	if (error == std::errc::permission_denied || error == std::errc::read_only_file_system)
		return SAVE_DATA_ERROR_NO_PERMISSION;
	return SAVE_DATA_ERROR_INTERNAL;
}

#ifdef _WIN32
// A virus scanner, search indexer or backup agent opens a file the moment it is closed, and while
// it holds a handle that does not permit deletion, replacing that file fails with
// ERROR_ACCESS_DENIED or ERROR_SHARING_VIOLATION. Those handles are short-lived, so an operation
// that fails that way is retried for about a second before the save is reported as failed; a
// genuine permission problem still surfaces, just after the last attempt.
bool TransientlyShared() {
	const auto error = GetLastError();
	return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
	       error == ERROR_LOCK_VIOLATION;
}

template <typename Operation>
bool RetryWhileShared(Operation operation) {
	constexpr int Attempts = 48;
	for (int attempt = 0;; attempt++) {
		if (operation()) return true;
		if (attempt + 1 >= Attempts || !TransientlyShared()) return false;
		Sleep(1u << std::min(attempt, 5)); // 1ms, doubling to 32ms, then 32ms per attempt.
	}
}
#endif

struct NativeFile {
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
	bool Valid() const { return handle != INVALID_HANDLE_VALUE; }
	bool Close() {
		if (!Valid()) return true;
		const auto old = std::exchange(handle, INVALID_HANDLE_VALUE);
		return CloseHandle(old) != 0;
	}
#else
	int handle = -1;
	bool Valid() const { return handle >= 0; }
	bool Close() {
		if (!Valid()) return true;
		return close(std::exchange(handle, -1)) == 0;
	}
#endif
	~NativeFile() { Close(); }
};

bool IsLink(const fs::path& path) {
#ifdef _WIN32
	const auto attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
	std::error_code error;
	return fs::is_symlink(fs::symlink_status(path, error));
#endif
}

int SyncDirectory(const fs::path& directory) {
#ifdef _WIN32
	(void)directory;
	return OK;
#else
	NativeFile file;
	file.handle = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (!file.Valid()) return IoError();
	int result;
	do { result = fsync(file.handle); } while (result < 0 && errno == EINTR);
	return result == 0 ? OK : IoError();
#endif
}

int MakeDirectory(const fs::path& path) {
	if (IsLink(path)) return SAVE_DATA_ERROR_NO_PERMISSION;
	std::error_code error;
	const bool created = fs::create_directory(path, error);
	if (error) return FsError(error);
	if (!fs::is_directory(path, error) || error) return SAVE_DATA_ERROR_INTERNAL;
	// Persist each newly created directory entry before publishing a save beneath it.
	return created ? SyncDirectory(path.parent_path()) : OK;
}

// The title id comes from the guest's param.sfo, so it is checked here rather than trusted: only
// characters that cannot form a path component of their own are accepted, which keeps every save
// under the configured root (CWE-22). The set deliberately excludes '.', so neither "." nor ".."
// can be spelled. It is not a title-id format check - a fixed "CUSA/PPSA + digits" rule would
// reject homebrew and system titles and leave them unable to save at all.
bool ValidTitle(std::string_view title) {
	return !title.empty() && title.size() <= 64 && std::all_of(title.begin(), title.end(), [](char c) {
		       return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		              c == '_' || c == '-';
	       });
}

int ValidateKey(std::string_view title, int32_t user, uint32_t slot) {
	if (!ValidTitle(title) || slot >= SAVE_DATA_MEMORY_SLOT_COUNT) return SAVE_DATA_ERROR_PARAMETER;
	// A negative user id is a login failure to the caller, not a malformed request.
	if (user < 0) return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
	return OK;
}

int OpenLock(const fs::path& path, NativeFile& file) {
	if (IsLink(path)) return SAVE_DATA_ERROR_NO_PERMISSION;
#ifdef _WIN32
	file.handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
	                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (!file.Valid()) return IoError();
#else
	file.handle = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (!file.Valid()) return IoError();
	if (flock(file.handle, LOCK_EX | LOCK_NB) != 0) return IoError();
#endif
	return OK;
}

int ReadFileBytes(const fs::path& path, std::vector<uint8_t>& bytes, bool& exists) {
	if (IsLink(path)) return SAVE_DATA_ERROR_NO_PERMISSION;
	NativeFile file;
#ifdef _WIN32
	// Loading a save is exposed to the same short-lived scanner handles as replacing one.
	RetryWhileShared([&] {
		file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		return file.Valid();
	});
	if (!file.Valid()) {
		if (GetLastError() == ERROR_FILE_NOT_FOUND) { exists = false; return OK; }
		return IoError();
	}
	LARGE_INTEGER length;
	if (!GetFileSizeEx(file.handle, &length)) return IoError();
	const uint64_t size = static_cast<uint64_t>(length.QuadPart);
#else
	file.handle = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (!file.Valid()) {
		if (errno == ENOENT) { exists = false; return OK; }
		return IoError();
	}
	struct stat status {};
	if (fstat(file.handle, &status) != 0) return IoError();
	if (!S_ISREG(status.st_mode)) return SAVE_DATA_ERROR_BROKEN;
	const uint64_t size = static_cast<uint64_t>(status.st_size);
#endif
	exists = true;
	if (size < HeaderSize + sizeof(SaveDataParam) || size > HeaderSize + sizeof(SaveDataParam) + MaxMemory + MaxIcon)
		return SAVE_DATA_ERROR_BROKEN;
	bytes.resize(static_cast<size_t>(size));
	if (Fault(Stage::Read)) return SAVE_DATA_ERROR_INTERNAL;
	size_t offset = 0;
	while (offset < bytes.size()) {
#ifdef _WIN32
		DWORD count = 0;
		if (!ReadFile(file.handle, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr))
			return IoError();
#else
		const auto count = read(file.handle, bytes.data() + offset, bytes.size() - offset);
		if (count < 0) { if (errno == EINTR) continue; return IoError(); }
#endif
		if (count == 0) return SAVE_DATA_ERROR_BROKEN;
		offset += static_cast<size_t>(count);
	}
	return OK;
}

uint64_t Checksum(std::span<const uint8_t> bytes) {
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < bytes.size(); i++) {
		if (i >= 48 && i < HeaderSize) continue;
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	}
	return hash;
}
void Put64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
	for (size_t i = 0; i < 8; i++) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
uint64_t Get64(const std::vector<uint8_t>& bytes, size_t offset) {
	uint64_t value = 0;
	for (size_t i = 0; i < 8; i++) value |= uint64_t {bytes[offset + i]} << (8 * i);
	return value;
}

struct Snapshot {
	std::vector<uint8_t> data;
	std::vector<uint8_t> icon;
	SaveDataParam param {};
	size_t icon_capacity = 0;
};

int Decode(const std::vector<uint8_t>& bytes, Snapshot& save) {
	if (!std::equal(Magic.begin(), Magic.end(), bytes.begin()) || Get64(bytes, 8) != 1 ||
	    Get64(bytes, 40) != sizeof(SaveDataParam) || Get64(bytes, 48) != Checksum(bytes))
		return SAVE_DATA_ERROR_BROKEN;
	const auto size = Get64(bytes, 16), capacity = Get64(bytes, 24), icon_size = Get64(bytes, 32);
	if (size == 0 || size > MaxMemory || capacity > MaxIcon || icon_size > capacity ||
	    bytes.size() != HeaderSize + sizeof(SaveDataParam) + size + icon_size)
		return SAVE_DATA_ERROR_BROKEN;
	std::memcpy(&save.param, bytes.data() + HeaderSize, sizeof(save.param));
	const auto start = bytes.begin() + HeaderSize + sizeof(SaveDataParam);
	save.data.assign(start, start + size);
	save.icon.assign(start + size, bytes.end());
	save.icon_capacity = static_cast<size_t>(capacity);
	return OK;
}

std::vector<uint8_t> Encode(const Snapshot& save) {
	std::vector<uint8_t> bytes(HeaderSize + sizeof(SaveDataParam) + save.data.size() + save.icon.size());
	std::copy(Magic.begin(), Magic.end(), bytes.begin());
	Put64(bytes, 8, 1);
	Put64(bytes, 16, save.data.size());
	Put64(bytes, 24, save.icon_capacity);
	Put64(bytes, 32, save.icon.size());
	Put64(bytes, 40, sizeof(SaveDataParam));
	std::memcpy(bytes.data() + HeaderSize, &save.param, sizeof(save.param));
	std::copy(save.data.begin(), save.data.end(), bytes.begin() + HeaderSize + sizeof(SaveDataParam));
	std::copy(save.icon.begin(), save.icon.end(), bytes.end() - save.icon.size());
	Put64(bytes, 48, Checksum(bytes));
	return bytes;
}

int WriteSnapshot(const fs::path& path, const Snapshot& save, bool& published) {
	published = false;
	const auto bytes = Encode(save);
	auto temporary = path;
	temporary += ".tmp";
	if (IsLink(path) || IsLink(temporary)) return SAVE_DATA_ERROR_NO_PERMISSION;
	if (Fault(Stage::Create)) return SAVE_DATA_ERROR_INTERNAL;
	std::error_code cleanup_error;
	fs::remove(temporary, cleanup_error);
	if (cleanup_error) return FsError(cleanup_error);
	NativeFile file;
#ifdef _WIN32
	// A temporary left open elsewhere, or one whose deletion has not completed yet, is the same
	// transient condition as a replacement that cannot delete its destination.
	RetryWhileShared([&] {
		file.handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		return file.Valid();
	});
#else
	file.handle = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
#endif
	if (!file.Valid()) return IoError();
	size_t offset = 0;
	while (offset < bytes.size()) {
		// Bounded chunks permit short-write and interruption tests at a real I/O boundary.
		const auto amount = std::min<size_t>(4096, bytes.size() - offset);
		if (Fault(Stage::Write)) return SAVE_DATA_ERROR_INTERNAL;
#ifdef _WIN32
		DWORD count = 0;
		if (!WriteFile(file.handle, bytes.data() + offset, static_cast<DWORD>(amount), &count, nullptr))
			return IoError();
#else
		const auto count = write(file.handle, bytes.data() + offset, amount);
		if (count < 0) { if (errno == EINTR) continue; return IoError(); }
#endif
		if (count == 0) return SAVE_DATA_ERROR_INTERNAL;
		offset += static_cast<size_t>(count);
	}
	if (Fault(Stage::Flush)) return SAVE_DATA_ERROR_INTERNAL;
#ifdef _WIN32
	if (!FlushFileBuffers(file.handle)) return IoError();
#else
	int flushed;
#ifdef __APPLE__
	do { flushed = fcntl(file.handle, F_FULLFSYNC); } while (flushed < 0 && errno == EINTR);
#else
	do { flushed = fsync(file.handle); } while (flushed < 0 && errno == EINTR);
#endif
	if (flushed != 0) return IoError();
#endif
	if (!file.Close()) return IoError();
	if (Fault(Stage::Replace)) return SAVE_DATA_ERROR_INTERNAL;
#ifdef _WIN32
	if (!RetryWhileShared([&] {
		    return MoveFileExW(temporary.c_str(), path.c_str(),
		                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
	    }))
		return IoError();
#else
	if (rename(temporary.c_str(), path.c_str()) != 0) return IoError();
#endif
	published = true;
	if (Fault(Stage::DirectoryFlush)) return SAVE_DATA_ERROR_INTERNAL;
	return SyncDirectory(path.parent_path());
}

bool ValidRange(const SaveDataMemoryData& data, size_t size) {
	return data.offset >= 0 && static_cast<uint64_t>(data.offset) <= size &&
	       data.buf_size <= size - static_cast<size_t>(data.offset) &&
	       (data.buf != nullptr || data.buf_size == 0);
}
bool ValidIcon(const SaveDataIcon* icon, size_t capacity) {
	return icon == nullptr || (icon->buf_size <= capacity && (icon->buf != nullptr || icon->buf_size == 0));
}
void SetParam(Snapshot& save, const SaveDataParam* param) {
	if (param == nullptr) return;
	save.param = *param;
	save.param.pad = 0;
	std::memset(save.param.reserved, 0, sizeof(save.param.reserved));
}
void SetIcon(Snapshot& save, const SaveDataIcon* icon) {
	if (icon == nullptr) return;
	save.icon.resize(icon->buf_size);
	if (!save.icon.empty()) std::memcpy(save.icon.data(), icon->buf, save.icon.size());
}
} // namespace

struct SaveDataMemoryStore::Impl {
	struct Record {
		NativeFile lock;
		fs::path path;
		Snapshot save;
	};
	using Key = std::tuple<std::string, int32_t, uint32_t>;
	fs::path root;
	std::mutex mutex;
	std::map<Key, std::unique_ptr<Record>> records;

	Record* Find(std::string_view title, int32_t user, uint32_t slot) {
		const auto it = records.find(Key {std::string(title), user, slot});
		return it == records.end() ? nullptr : it->second.get();
	}
	int PreparePath(std::string_view title, int32_t user, uint32_t slot, Record& record) {
		std::error_code error;
		root = fs::absolute(root, error);
		if (error) return FsError(error);
		// The configured root's parent must already exist; descendants never come from guest paths.
		auto directory = root;
		int result = MakeDirectory(directory);
		if (result != OK) return result;
		for (const auto& component : {std::string(title), MemoryDirectoryName(slot), std::to_string(user)}) {
			directory /= component;
			result = MakeDirectory(directory);
			if (result != OK) return result;
		}
		// Data, parameters and icon share one file so that a save is replaced in a single rename.
		// The name is deliberately not "memory.dat": that is the raw-guest-bytes file written by
		// other implementations, and handing this container's header to a title expecting raw
		// bytes would corrupt its save rather than read as absent.
		record.path = directory / "memory.sdm";
		auto lock_path = record.path;
		lock_path += ".lock";
		return OpenLock(lock_path, record.lock);
	}
};

std::string MemoryDirectoryName(uint32_t slot_id) {
	return "sce_sdmemory" + (slot_id == 0 ? std::string() : std::to_string(slot_id));
}

SaveDataMemoryStore::SaveDataMemoryStore(fs::path root) : m_impl(std::make_unique<Impl>()) {
	m_impl->root = std::move(root);
}
SaveDataMemoryStore::~SaveDataMemoryStore() = default;

int SaveDataMemoryStore::Setup(std::string_view title, const SaveDataMemorySetup2* request,
                              SaveDataMemorySetupResult* result) try {
	if (request == nullptr || request->memory_size == 0 || request->memory_size > MaxMemory ||
	    request->icon_memory_size > MaxIcon ||
	    (request->option & ~(SAVE_DATA_MEMORY_SET_PARAM | SAVE_DATA_MEMORY_DOUBLE_BUFFER)) != 0)
		return SAVE_DATA_ERROR_PARAMETER;
	if (const int error = ValidateKey(title, request->user_id, request->slot_id); error != OK) return error;
	// DOUBLE_BUFFER only doubles the size the system reserves; a snapshot is published atomically
	// here either way, so it is accepted and has no effect on what is stored.
	const bool init = (request->option & SAVE_DATA_MEMORY_SET_PARAM) != 0;
	if (init && !ValidIcon(request->init_icon, request->icon_memory_size)) return SAVE_DATA_ERROR_PARAMETER;
	std::lock_guard lock(m_impl->mutex);
	auto* record = m_impl->Find(title, request->user_id, request->slot_id);
	std::unique_ptr<Impl::Record> fresh;
	bool exists = record != nullptr;
	if (record == nullptr) {
		fresh = std::make_unique<Impl::Record>();
		record = fresh.get();
		int error = m_impl->PreparePath(title, request->user_id, request->slot_id, *record);
		if (error != OK) return error;
		std::vector<uint8_t> bytes;
		error = ReadFileBytes(record->path, bytes, exists);
		if (error != OK) return error;
		if (exists && (error = Decode(bytes, record->save)) != OK) return error;
	}
	const size_t existed_size = exists ? record->save.data.size() : 0;
	Snapshot candidate = record->save;
	candidate.data.resize(std::max(request->memory_size, candidate.data.size()));
	candidate.icon_capacity = std::max(request->icon_memory_size, candidate.icon_capacity);
	if (!exists && init) {
		SetParam(candidate, request->init_param);
		SetIcon(candidate, request->init_icon);
		// Initialization data is not an update: nothing has been saved yet.
		candidate.param.mtime = 0;
	}
	// Setup never truncates existing progress or replaces existing metadata with initialization data.
	bool published = false;
	const int error = WriteSnapshot(record->path, candidate, published);
	if (published) record->save = std::move(candidate);
	if (error != OK) return error;
	if (fresh) m_impl->records.emplace(Impl::Key {std::string(title), request->user_id, request->slot_id}, std::move(fresh));
	if (result != nullptr) { *result = {}; result->existed_memory_size = existed_size; }
	return OK;
} catch (const std::bad_alloc&) { return SAVE_DATA_ERROR_OUT_OF_MEMORY; }
catch (const fs::filesystem_error&) { return SAVE_DATA_ERROR_INTERNAL; }

int SaveDataMemoryStore::Get(std::string_view title, SaveDataMemoryGet2* request) try {
	if (request == nullptr) return SAVE_DATA_ERROR_PARAMETER;
	if (const int error = ValidateKey(title, request->user_id, request->slot_id); error != OK) return error;
	std::lock_guard lock(m_impl->mutex);
	const auto* record = m_impl->Find(title, request->user_id, request->slot_id);
	if (record == nullptr) return SAVE_DATA_ERROR_MEMORY_NOT_READY;
	const auto& save = record->save;
	if (request->data != nullptr && !ValidRange(*request->data, save.data.size())) return SAVE_DATA_ERROR_PARAMETER;
	if (request->icon != nullptr && request->icon->buf == nullptr && request->icon->buf_size != 0) return SAVE_DATA_ERROR_PARAMETER;
	if (request->data != nullptr && request->data->buf_size != 0)
		std::memcpy(request->data->buf, save.data.data() + request->data->offset, request->data->buf_size);
	if (request->param != nullptr) *request->param = save.param;
	if (request->icon != nullptr) {
		const auto count = std::min(request->icon->buf_size, save.icon.size());
		if (count != 0) std::memcpy(request->icon->buf, save.icon.data(), count);
		request->icon->data_size = save.icon.size();
	}
	return OK;
} catch (const std::bad_alloc&) { return SAVE_DATA_ERROR_OUT_OF_MEMORY; }

int SaveDataMemoryStore::Set(std::string_view title, const SaveDataMemorySet2* request) try {
	if (request == nullptr || request->data_num > SAVE_DATA_MEMORY_DATA_NUM_MAX) return SAVE_DATA_ERROR_PARAMETER;
	if (const int error = ValidateKey(title, request->user_id, request->slot_id); error != OK) return error;
	std::lock_guard lock(m_impl->mutex);
	auto* record = m_impl->Find(title, request->user_id, request->slot_id);
	if (record == nullptr) return SAVE_DATA_ERROR_MEMORY_NOT_READY;
	// Nothing to update: publishing an identical snapshot would cost a write and a flush.
	if (request->data == nullptr && request->param == nullptr && request->icon == nullptr) return OK;
	if (!ValidIcon(request->icon, record->save.icon_capacity)) return SAVE_DATA_ERROR_PARAMETER;
	// data_num == 0 next to a non-null data pointer means one entry, not none.
	const auto count = std::max(1u, request->data_num);
	if (request->data != nullptr) {
		for (uint32_t i = 0; i < count; i++)
			if (!ValidRange(request->data[i], record->save.data.size())) return SAVE_DATA_ERROR_PARAMETER;
	}
	Snapshot candidate = record->save;
	if (request->data != nullptr) {
		for (uint32_t i = 0; i < count; i++) {
			const auto& data = request->data[i];
			if (data.buf_size != 0) std::memcpy(candidate.data.data() + data.offset, data.buf, data.buf_size);
		}
	}
	SetParam(candidate, request->param);
	SetIcon(candidate, request->icon);
	// The system owns the modification time, not the title: it stamps every update, and save
	// browsers sort by it.
	candidate.param.mtime = static_cast<int64_t>(std::time(nullptr));
	bool published = false;
	const int error = WriteSnapshot(record->path, candidate, published);
	// A directory flush can fail after replacement; keep reads coherent with the visible file.
	if (published) record->save = std::move(candidate);
	return error;
} catch (const std::bad_alloc&) { return SAVE_DATA_ERROR_OUT_OF_MEMORY; }
catch (const fs::filesystem_error&) { return SAVE_DATA_ERROR_INTERNAL; }

int SaveDataMemoryStore::Sync(std::string_view title, const SaveDataMemorySync* request) try {
	if (request == nullptr || request->option > 1) return SAVE_DATA_ERROR_PARAMETER;
	if (const int error = ValidateKey(title, request->user_id, request->slot_id); error != OK) return error;
	std::lock_guard lock(m_impl->mutex);
	const auto* record = m_impl->Find(title, request->user_id, request->slot_id);
	if (record == nullptr) return SAVE_DATA_ERROR_MEMORY_NOT_READY;
	bool published = false;
	return WriteSnapshot(record->path, record->save, published);
} catch (const std::bad_alloc&) { return SAVE_DATA_ERROR_OUT_OF_MEMORY; }
catch (const fs::filesystem_error&) { return SAVE_DATA_ERROR_INTERNAL; }

void SaveDataMemoryStore::Clear() {
	std::lock_guard lock(m_impl->mutex);
	m_impl->records.clear();
}

#ifdef KYTY_SAVE_DATA_TESTS
void MemoryTesting::SetFaultHook(FaultHook hook) { g_fault_hook = hook; }
#endif
} // namespace Libs::SaveData
