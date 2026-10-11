#include "libs/avPlayer.cpp"
#include "common/archive.h"
#include "ArchiveTestFixture.h"

#include <array>
#include <chrono>
#include <new>
#include <thread>

namespace {
std::filesystem::path content_root;
}

namespace Libs::LibKernel {
struct PthreadPrivate {
	std::thread thread;
};
int KYTY_SYSV_ABI PthreadCreate(Pthread* thread, const PthreadAttr*, pthread_entry_func_t entry,
                              void* arg, const char*) {
	*thread = new PthreadPrivate {std::thread([=] { entry(arg); })};
	return 0;
}
int KYTY_SYSV_ABI PthreadJoin(Pthread thread, void**) {
	thread->thread.join();
	delete thread;
	return 0;
}
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

struct VideoMemory {
	unsigned allocations        = 0;
	unsigned deallocations      = 0;
	void*    protected_buffer   = nullptr;
	bool     released_protected = false;
};
void* KYTY_SYSV_ABI Allocate(void* object, uint32_t alignment, uint32_t size) {
	Check(alignment == 0x100, "video allocation alignment");
	++static_cast<VideoMemory*>(object)->allocations;
	return ::operator new(size, std::align_val_t(alignment), std::nothrow);
}
void KYTY_SYSV_ABI Deallocate(void* object, void* data) {
	auto& memory = *static_cast<VideoMemory*>(object);
	++memory.deallocations;
	memory.released_protected |= data == memory.protected_buffer;
	::operator delete(data, std::align_val_t(0x100));
}

void CheckVideoReadAhead(const std::filesystem::path& root) {
	// A 16x16 red MPEG-4 intra frame at 30 Hz; repeat it with increasing VOP times.
	std::array<uint8_t, 50> frame {
	    0x00, 0x00, 0x01, 0xb0, 0x01, 0x00, 0x00, 0x01, 0xb5, 0x89,
	    0x13, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x20, 0x00,
	    0xc4, 0x8d, 0x88, 0x00, 0xf5, 0x00, 0x84, 0x02, 0x14, 0x63,
	    0x00, 0x00, 0x01, 0xb3, 0x00, 0x10, 0x07, 0x00, 0x00, 0x01,
	    0xb6, 0x10, 0x60, 0x51, 0x85, 0x06, 0xd8, 0x2c, 0x81, 0xe0};
	{
		Common::File output;
		Check(output.Create(root / "movie.m4v"), "create video fixture");
		for (unsigned i = 0; i < 24; ++i) {
			frame[41] = static_cast<uint8_t>(0x10 + i / 2);
			frame[42] = i % 2 == 0 ? 0x60 : 0xe0;
			output.Write(frame.data(), frame.size());
		}
	}
	VideoMemory memory;
	const AvPlayerMemAllocator allocator {&memory, Allocate, Deallocate, Allocate, Deallocate};
	Source source(allocator, {}, {}, 6, false, 0);
	Check(source.Init("/app0/movie.m4v", AvPlayerSourceFileMp4) == 0, "open video fixture");
	source.SetSync(1);
	Check(source.Start() == 0, "start video workers");
	AvPlayerFrameInfoEx info {};
	for (unsigned i = 0; i < 8; ++i) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!source.Video(&info)) {
			Check(std::chrono::steady_clock::now() < deadline, "decode steady-state video");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	// Let the producer refill, then model the game's immediate late-frame retries.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	for (unsigned i = 0; i < 5; ++i) {
		const auto previous = info.time_stamp;
		Check(source.Video(&info), "six output buffers keep five video frames ready");
		Check(info.time_stamp > previous, "video retries return successive frames");
	}
	Check(source.Pause() == 0, "pause video");
	memory.protected_buffer = info.data;
	std::array<uint8_t, 256> current {};
	std::memcpy(current.data(), info.data, current.size());
	Check(!source.Video(&info) && info.data == memory.protected_buffer,
	      "failed video get preserves output");
	Check(source.Start() == 0, "restart video with retained current output");
	Check(!memory.released_protected &&
	          std::memcmp(current.data(), memory.protected_buffer, current.size()) == 0,
	      "restart preserves output until the next successful video get");
	Check(source.Stop() == 0 && memory.allocations == memory.deallocations,
	      "stop releases all video buffers");
}
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
	content_root = root;
	CheckVideoReadAhead(root);
	std::error_code error;
	std::filesystem::remove_all(root, error);
	Check(!error, "release all archive handles");
	std::puts("AvPlayerFileTests: all cases passed");
}
