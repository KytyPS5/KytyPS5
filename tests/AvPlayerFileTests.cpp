#include "libs/avPlayer.cpp"
#include "common/archive.h"
#include "ArchiveTestFixture.h"

#include <array>
#include <chrono>

namespace {
std::filesystem::path content_root;
}

namespace Libs::LibKernel {
int KYTY_SYSV_ABI PthreadCreate(Pthread*, const PthreadAttr*, pthread_entry_func_t, void*, const char*) {
	std::abort();
}
int KYTY_SYSV_ABI PthreadJoin(Pthread, void**) { std::abort(); }
namespace FileSystem {
std::filesystem::path GetRealFilename(const std::string& path) {
	return path.starts_with("/app0/") ? content_root / path.substr(6) : std::filesystem::path {};
}
}
}

namespace {
using namespace Libs::Audio::AvPlayer;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "AvPlayerFileTests: %s\n", message);
		std::abort();
	}
}

void CheckStream(FileStreamer& stream, const std::vector<uint8_t>& data) {
	auto* io = stream.Context();
	Check(io->seek(io->opaque, 0, AVSEEK_SIZE) == static_cast<int64_t>(data.size()), "query size");
	Check(io->seek(io->opaque, 65531, SEEK_SET | AVSEEK_FORCE) == 65531, "seek across block boundary");
	std::array<uint8_t, 32> bytes {};
	Check(io->read_packet(io->opaque, bytes.data(), bytes.size()) == bytes.size() &&
	          std::equal(bytes.begin(), bytes.end(), data.begin() + 65531), "read across block boundary");
	Check(io->seek(io->opaque, -16, SEEK_CUR) == 65547, "seek relative to cursor");
	Check(io->seek(io->opaque, INT64_MIN, SEEK_CUR) < 0 &&
	          io->seek(io->opaque, INT64_MAX, SEEK_END) < 0 &&
	          io->seek(io->opaque, 0, 12345) < 0, "reject invalid and overflowing seeks");
	Check(io->seek(io->opaque, 0, SEEK_CUR) == 65547, "rejected seeks preserve the cursor");
	Check(io->seek(io->opaque, -7, SEEK_END) == static_cast<int64_t>(data.size() - 7), "seek near end");
	Check(io->read_packet(io->opaque, bytes.data(), bytes.size()) == 7 &&
	          std::equal(bytes.begin(), bytes.begin() + 7, data.end() - 7), "short read at end");
	Check(io->read_packet(io->opaque, bytes.data(), bytes.size()) == AVERROR_EOF, "report EOF");
	Check(io->seek(io->opaque, 11, SEEK_END) == static_cast<int64_t>(data.size() + 11) &&
	          io->read_packet(io->opaque, bytes.data(), bytes.size()) == AVERROR_EOF, "read beyond end");
	Check(io->seek(io->opaque, 0, SEEK_SET) == 0, "rewind");
	std::vector<uint8_t> buffered(data.size());
	Check(avio_read(io, buffered.data(), static_cast<int>(buffered.size())) == buffered.size() &&
	          buffered == data, "FFmpeg buffered reads preserve content");
	Check(avio_seek(io, 123, SEEK_SET) == 123 &&
	          avio_read(io, bytes.data(), bytes.size()) == bytes.size() &&
	          std::equal(bytes.begin(), bytes.end(), data.begin() + 123), "FFmpeg seeks reset buffered reads");
}

struct Callbacks {
	const std::vector<uint8_t>& data;
	unsigned opens = 0;
	unsigned closes = 0;
	bool premature_eof = false;
};
int KYTY_SYSV_ABI Open(void* object, const char*) {
	++static_cast<Callbacks*>(object)->opens;
	return 0;
}
int KYTY_SYSV_ABI Close(void* object) {
	++static_cast<Callbacks*>(object)->closes;
	return 0;
}
int KYTY_SYSV_ABI Read(void* object, uint8_t* buffer, uint64_t offset, uint32_t size) {
	auto& state = *static_cast<Callbacks*>(object);
	if (state.premature_eof) {
		return 0;
	}
	const auto bytes = std::min<uint64_t>(size, state.data.size() - offset);
	std::memcpy(buffer, state.data.data() + offset, bytes);
	return static_cast<int>(bytes);
}
uint64_t KYTY_SYSV_ABI Size(void* object) { return static_cast<Callbacks*>(object)->data.size(); }
}

int main() {
	const auto root = std::filesystem::temp_directory_path() /
	    ("kyty_avio_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(std::filesystem::create_directories(root), "create fixture directory");
	std::vector<uint8_t> payload(128 * 1024 + 37);
	for (size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>(i * 37);
	}
	{
		Common::File output;
		Check(output.Create(root / "movie.bin"), "create native fixture");
		output.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	}
	Check(ArchiveTests::CreateArchive(root / "game.zar", payload), "create archive fixture");
	content_root = root;
	{
		FileStreamer native({});
		Check(native.Init("/app0/movie.bin"), "open native stream");
		CheckStream(native, payload);
	}
	content_root = Common::MakeArchivePath(root / "game.zar");
	{
		FileStreamer archive({});
		Check(archive.Init("/app0/assets/subdir/data.bin"), "open archived stream");
		CheckStream(archive, payload);
	}
	Callbacks callbacks {payload};
	const AvPlayerFileReplacement replacement {&callbacks, Open, Close, Read, Size};
	{
		FileStreamer callback(replacement);
		Check(callback.Init("callback"), "open guest callback stream");
		CheckStream(callback, payload);
		callbacks.premature_eof = true;
		auto* io = callback.Context();
		Check(io->seek(io->opaque, 0, SEEK_SET) == 0, "rewind callback");
		uint8_t byte = 0;
		Check(io->read_packet(io->opaque, &byte, 1) == AVERROR(EIO), "reject premature EOF");
	}
	Check(callbacks.opens == 1 && callbacks.closes == 1, "close callback exactly once");
	{
		FileStreamer missing({});
		Check(!missing.Init("/unmounted/movie.bin"), "reject unmapped media");
	}
	std::error_code error;
	std::filesystem::remove_all(root, error);
	Check(!error, "release all archive handles");
	std::puts("AvPlayerFileTests: all cases passed");
}
