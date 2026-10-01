#include "common/archive.h"

#include "common/stringUtils.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <zarchive/zarchivereader.h>

// POSIX hosts and MinGW's pthread-backed libstdc++ have cancellable condition waits.
// MSVC/ClangCL and MinGW's native Win32 threading backend do not use pthread waits.
#if !defined(_WIN32) || defined(_GLIBCXX_GCC_GTHR_POSIX_H)
#include <pthread.h>
#define KYTY_ARCHIVE_PTHREAD_CANCELLATION
#endif

namespace Common {

// Backends expose immutable metadata and offset-based reads. Cursor management, path routing,
// reader lifetime and the host-stack worker are shared by every archive format.
class ArchiveReader {
public:
	struct Entry {
		uint64_t id;
		uint64_t size;
		bool     is_file;
	};

	virtual ~ArchiveReader()                                                       = default;
	virtual std::optional<Entry>        Find(std::string_view member)              = 0;
	virtual std::vector<File::DirEntry> List(std::string_view member)              = 0;
	virtual uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) = 0;
};

namespace {

class ZArchiveReaderBackend final: public ArchiveReader {
public:
	explicit ZArchiveReaderBackend(std::unique_ptr<ZArchiveReader> reader)
	    : m_reader(std::move(reader)) {}

	std::optional<Entry> Find(std::string_view member) override {
		const auto node = m_reader->LookUp(member);
		if (node == ZARCHIVE_INVALID_NODE) {
			return {};
		}
		return Entry {node, m_reader->GetFileSize(node), m_reader->IsFile(node)};
	}

	std::vector<File::DirEntry> List(std::string_view member) override {
		const auto node = m_reader->LookUp(member);
		if (!m_reader->IsDirectory(node)) {
			return {};
		}
		const auto count = m_reader->GetDirEntryCount(node);
		if (count > (1u << 20u)) {
			return {};
		}
		std::vector<File::DirEntry> result;
		result.reserve(count);
		for (uint32_t index = 0; index < count; ++index) {
			ZArchiveReader::DirEntry entry {};
			if (m_reader->GetDirEntry(node, index, entry)) {
				result.push_back({std::string(entry.name), entry.isFile});
			}
		}
		return result;
	}

	uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) override {
		// ZArchiveReader already serializes access to its stream and decompression cache.
		return m_reader->ReadFromFile(static_cast<ZArchiveNodeHandle>(id), offset, size, data);
	}

private:
	std::unique_ptr<ZArchiveReader> m_reader;
};

std::shared_ptr<ArchiveReader> OpenZArchive(const std::filesystem::path& path) {
	auto reader = std::unique_ptr<ZArchiveReader>(ZArchiveReader::OpenFromFile(path));
	return reader ? std::make_shared<ZArchiveReaderBackend>(std::move(reader)) : nullptr;
}

struct ArchiveFormat {
	std::string_view extension;
	std::shared_ptr<ArchiveReader> (*open)(const std::filesystem::path&);
};

constexpr ArchiveFormat Formats[] = {{".zar", OpenZArchive}};
using NativeView                  = std::basic_string_view<std::filesystem::path::value_type>;

const ArchiveFormat* FindFormat(NativeView path) {
	for (const auto& format: Formats) {
		if (path.size() < format.extension.size()) {
			continue;
		}
		const auto extension = path.substr(path.size() - format.extension.size());
		if (std::equal(
		        extension.begin(), extension.end(), format.extension.begin(),
		        [](auto a, char b) { return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b; })) {
			return &format;
		}
	}
	return nullptr;
}

size_t FindMarker(NativeView path) {
	// Native files use this check on every operation. The usual path has no marker and allocates
	// nothing.
	for (auto marker = path.find('!'); marker != NativeView::npos;
	     marker      = path.find('!', marker + 1)) {
		if ((marker + 1 == path.size() || path[marker + 1] == '/' || path[marker + 1] == '\\') &&
		    FindFormat(path.substr(0, marker)) != nullptr) {
			return marker;
		}
	}
	return NativeView::npos;
}

struct ParsedPath {
	std::filesystem::path archive;
	std::string           member;
};

std::optional<ParsedPath> ParsePath(const std::filesystem::path& path) {
	const auto& native = path.native();
	const auto  marker = FindMarker(native);
	if (marker == NativeView::npos) {
		return {};
	}
	ParsedPath parsed {.archive = std::filesystem::path(native.substr(0, marker))};
	auto       member = PathToGenericString(std::filesystem::path(native.substr(marker + 1)));
	std::replace(member.begin(), member.end(), '\\', '/');
	std::string_view remaining = member;
	while (!remaining.empty()) {
		const auto separator = remaining.find('/');
		const auto component = remaining.substr(0, separator);
		remaining.remove_prefix(separator == std::string_view::npos ? remaining.size() : separator + 1);
		if (component.empty() || component == ".") {
			continue;
		}
		if (component == "..") {
			if (parsed.member.empty()) {
				return {};
			}
			const auto separator = parsed.member.rfind('/');
			parsed.member.resize(separator == std::string::npos ? 0 : separator);
		} else {
			if (!parsed.member.empty()) {
				parsed.member += '/';
			}
			parsed.member += component;
		}
	}
	return parsed;
}

std::mutex                                                              g_readers_mutex;
std::unordered_map<std::filesystem::path, std::weak_ptr<ArchiveReader>> g_readers;

// Guest stacks can be only 64 KiB. Decompress on an ordinary host stack, then copy into guest
// memory on the caller so GPU-tracked writes use its fault handler. A single worker bounds host
// threads and avoids duplicating the archive's own block cache. Requests live on waiting callers.
class ArchiveIoThread {
public:
	static ArchiveIoThread& Instance() {
		// Guest exit can run static destructors while other guest threads are still reading.
		// Keep the worker alive until process termination, as with the guest threads themselves.
		static auto* instance = new ArchiveIoThread;
		return *instance;
	}

	uint64_t Read(ArchiveReader& reader, uint64_t id, uint64_t offset, uint32_t size, void* data) {
		Request          request {reader, id, offset, size, data};
		std::unique_lock lock(m_mutex);
		if (m_last != nullptr) {
			m_last->next = &request;
		} else {
			m_first = &request;
		}
		m_last = &request;
		m_wake.notify_one();
		request.ready.wait(lock, [&] { return request.done; });
		return request.result;
	}

	KYTY_CLASS_NO_COPY(ArchiveIoThread);

private:
	struct Request {
		ArchiveReader&          reader;
		uint64_t                id;
		uint64_t                offset;
		uint32_t                size;
		void*                   data;
		Request*                next   = nullptr;
		uint64_t                result = 0;
		bool                    done   = false;
		std::condition_variable ready {};
	};

	ArchiveIoThread() {
		std::thread([this] { Loop(); }).detach();
	}

	void Loop() {
		for (;;) {
			Request* request;
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait(lock, [this] { return m_first != nullptr; });
				request = m_first;
				m_first = request->next;
				if (m_first == nullptr) {
					m_last = nullptr;
				}
			}
			const auto result =
			    request->reader.Read(request->id, request->offset, request->size, request->data);
			{
				std::lock_guard lock(m_mutex);
				request->result = result;
				request->done   = true;
				// Notify before releasing the mutex: the waiting caller owns the condition
				// variable.
				request->ready.notify_one();
			}
		}
	}

	std::mutex              m_mutex;
	std::condition_variable m_wake;
	Request*                m_first = nullptr;
	Request*                m_last  = nullptr;
};

} // namespace

struct ArchiveFile::Private {
	std::shared_ptr<ArchiveReader> reader;
	ArchiveReader::Entry           entry;
	uint64_t                       position = 0;
};

ArchiveFile::ArchiveFile(std::unique_ptr<Private> p): m_p(std::move(p)) {}
ArchiveFile::~ArchiveFile() = default;

uint64_t ArchiveFile::Size() const {
	return m_p->entry.size;
}
uint64_t ArchiveFile::Tell() const {
	return m_p->position;
}
bool ArchiveFile::Seek(uint64_t offset) {
	m_p->position = offset;
	return true;
}

void ArchiveFile::Read(void* data, uint32_t size, uint32_t* bytes_read) {
#if defined(KYTY_ARCHIVE_PTHREAD_CANCELLATION)
	// A pending cancellation must not destroy the waiting request or its TLS buffer while the
	// worker still uses them. Restore the caller's state only after the copy is complete.
	int cancellation_state = PTHREAD_CANCEL_ENABLE;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancellation_state);
#endif
	constexpr uint32_t                chunk_size = 1u << 20u;
	thread_local std::vector<uint8_t> chunk;
	uint32_t                          read = 0;
	if (data != nullptr && size != 0 && m_p->position < Size()) {
		const auto requested =
		    static_cast<uint32_t>(std::min<uint64_t>(size, Size() - m_p->position));
		if (chunk.size() < std::min(chunk_size, requested)) {
			chunk.resize(std::min(chunk_size, requested));
		}
		while (read < requested) {
			const auto step = std::min(chunk_size, requested - read);
			const auto got  = ArchiveIoThread::Instance().Read(*m_p->reader, m_p->entry.id,
			                                                   m_p->position, step, chunk.data());
			if (got == 0 || got > step) {
				break;
			}
			std::memcpy(static_cast<uint8_t*>(data) + read, chunk.data(), got);
			read += static_cast<uint32_t>(got);
			m_p->position += got;
			if (got < step) {
				break;
			}
		}
	}
	if (bytes_read != nullptr) {
		*bytes_read = read;
	}
#if defined(KYTY_ARCHIVE_PTHREAD_CANCELLATION)
	pthread_setcancelstate(cancellation_state, nullptr);
#endif
}

std::filesystem::path MakeArchivePath(const std::filesystem::path& archive,
                                      const std::filesystem::path& member) {
	auto root = archive;
	root += "!";
	return member.empty() ? root : root / member.relative_path();
}

bool IsSupportedArchive(const std::filesystem::path& path) {
	return FindFormat(path.native()) != nullptr;
}

bool IsArchivePath(const std::filesystem::path& path) {
	return FindMarker(path.native()) != NativeView::npos;
}

std::filesystem::path GetArchiveHostPath(const std::filesystem::path& path) {
	const auto marker = FindMarker(path.native());
	return marker == NativeView::npos ? std::filesystem::path {}
	                                  : std::filesystem::path(path.native().substr(0, marker));
}

std::shared_ptr<ArchiveReader> OpenArchive(const std::filesystem::path& path) {
	const auto  archive = IsArchivePath(path) ? GetArchiveHostPath(path) : path;
	const auto* format  = FindFormat(archive.native());
	if (format == nullptr) {
		return {};
	}
	std::error_code error;
	auto            key = std::filesystem::absolute(archive, error);
	if (error) {
		return {};
	}
	{
		std::lock_guard lock(g_readers_mutex);
		if (auto it = g_readers.find(key); it != g_readers.end()) {
			if (auto reader = it->second.lock()) {
				return reader;
			}
		}
	}
	// Do not hold the cache mutex while opening an unrelated archive from disk.
	auto reader = format->open(archive);
	if (reader == nullptr) {
		return {};
	}
	std::lock_guard lock(g_readers_mutex);
	std::erase_if(g_readers, [](const auto& entry) { return entry.second.expired(); });
	auto& cached = g_readers[key];
	if (auto existing = cached.lock()) {
		return existing;
	}
	cached = reader;
	return reader;
}

std::optional<File::Info> GetArchiveInfo(const std::filesystem::path& path) {
	const auto parsed = ParsePath(path);
	if (parsed) {
		if (auto reader = OpenArchive(parsed->archive)) {
			if (auto entry = reader->Find(parsed->member)) {
				return File::Info {entry->is_file, entry->size};
			}
		}
	}
	return {};
}

std::vector<File::DirEntry> GetArchiveDirEntries(const std::filesystem::path& path) {
	const auto parsed = ParsePath(path);
	if (parsed) {
		if (auto reader = OpenArchive(parsed->archive)) {
			return reader->List(parsed->member);
		}
	}
	return {};
}

std::unique_ptr<ArchiveFile> OpenArchiveFile(const std::filesystem::path& path) {
	auto parsed = ParsePath(path);
	if (!parsed) {
		return {};
	}
	auto       reader = OpenArchive(parsed->archive);
	const auto entry  = reader ? reader->Find(parsed->member) : std::nullopt;
	if (!entry || !entry->is_file) {
		return {};
	}
	auto p     = std::make_unique<ArchiveFile::Private>();
	p->reader  = std::move(reader);
	p->entry   = *entry;
	return std::unique_ptr<ArchiveFile>(new ArchiveFile(std::move(p)));
}

} // namespace Common
