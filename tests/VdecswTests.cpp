#include "libs/libVdecsw.cpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
std::vector<Loader::SymbolResolve> registrations;
}
namespace Loader {
void SymbolDatabase::Add(const SymbolResolve& symbol, uint64_t, const std::string&) {
	registrations.push_back(symbol);
}
}

namespace {
using namespace Libs::Vdecsw;

#define CHECK(condition) do { if (!(condition)) { \
	std::fprintf(stderr, "VdecswTests:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

// Six 16x16 YUV420 frames with Y=40,60,80,100,120,140, encoded by libx264
// (QP18, bframes=2, b-adapt=0, keyint=12, aud=1; SEI removed). Coding order
// is I0,P3,B1,B2,P5,B4. Both delayed output and packet metadata are observable.
constexpr uint8_t Avc[] = {
	0x00,0x00,0x00,0x01,0x09,0x10,0x00,0x00,0x00,0x01,0x67,0x64,0x00,0x0a,0xac,0xd9,
	0x5e,0x84,0x00,0x00,0x03,0x00,0x04,0x00,0x00,0x03,0x00,0x30,0x3c,0x48,0x96,0x58,
	0x00,0x00,0x00,0x01,0x68,0xeb,0xe0,0x8c,0xb2,0x2c,0x00,0x00,0x01,0x65,0x88,0x84,
	0x00,0xeb,0xfe,0xf5,0x27,0xf8,0x14,0xd0,0x8c,0x2d,0x00,0x00,0x00,0x01,0x09,0x30,
	0x00,0x00,0x01,0x41,0x9a,0x23,0x6c,0x7f,0xa0,0xc3,0x2f,0x81,0xec,0xfb,0x1c,0x00,
	0x00,0x00,0x01,0x09,0x50,0x00,0x00,0x01,0x41,0x9e,0x41,0x78,0xaf,0xd6,0x81,0x00,
	0x00,0x00,0x01,0x09,0x50,0x00,0x00,0x01,0x01,0x9e,0x62,0x6a,0x49,0xff,0xd8,0x80,
	0x00,0x00,0x00,0x01,0x09,0x30,0x00,0x00,0x01,0x41,0x9a,0x65,0x49,0xa8,0x41,0x68,
	0x99,0x4c,0x14,0xff,0xa0,0xc3,0x2f,0x81,0xdd,0x0c,0xc1,0x00,0x00,0x00,0x01,0x09,
	0x50,0x00,0x00,0x01,0x01,0x9e,0x84,0x6a,0x49,0xff,0xd8,0x81,
};
constexpr std::array<size_t, 7> PacketOffsets{0, 58, 79, 95, 112, 139, sizeof(Avc)};
constexpr std::array<uint32_t, 6> PresentationOrder{0, 3, 1, 2, 5, 4};
// One independent, immediately displayable IDR (same pixels, QP18, no B frames).
constexpr uint8_t Intra[] = {
	0x00,0x00,0x00,0x01,0x09,0x10,0x00,0x00,0x00,0x01,0x67,0x64,0x10,0x0a,0xac,0xbb,
	0xd0,0x80,0x00,0x00,0x03,0x00,0x80,0x00,0x00,0x03,0x01,0x02,0x00,0x00,0x00,0x01,
	0x68,0xee,0x02,0x32,0xc8,0xb0,0x00,0x00,0x01,0x65,0x88,0x84,0x3a,0xfe,0xf5,0x27,
	0xf8,0x14,0xd0,0x8c,0x2d,
};

InputData Input(uint32_t packet) {
	return {sizeof(InputData), const_cast<uint8_t*>(Avc + PacketOffsets[packet]),
	        PacketOffsets[packet + 1] - PacketOffsets[packet],
	        PresentationOrder[packet] * 1000u, packet * 1000u,
	        0x12340000u + PresentationOrder[packet]};
}

struct GuardedMemory {
	uint8_t* allocation;
	uint8_t* data;
	size_t size;
	explicit GuardedMemory(size_t bytes): size(bytes) {
		allocation = static_cast<uint8_t*>(std::aligned_alloc(256, (bytes + 767u) & ~size_t{255}));
		CHECK(allocation != nullptr);
		data = allocation + 256;
		std::memset(allocation, 0xa7, bytes + 512);
	}
	~GuardedMemory() { Check(); std::free(allocation); }
	void Check() const {
		for (size_t i = 0; i < 256; ++i)
			CHECK(allocation[i] == 0xa7 && data[size + i] == 0xa7);
	}
};

ConfigInfo Config() {
	ConfigInfo config{};
	config.this_size = sizeof(config);
	config.resource_type = config.codec_type = 1;
	config.profile = 100;
	config.max_level = 10;
	config.max_width = config.max_height = 16;
	config.max_dpb_frames = 4;
	config.extra_dpb_frames = 1;
	config.input_queue_depth = 3;
	config.cpu_thread_priority = -1;
	config.optimize_progressive = true;
	return config;
}

struct Session {
	GuardedMemory queue_memory{sizeof(ComputeQueue)};
	GuardedMemory state_memory{sizeof(State)};
	ConfigInfo config = Config();
	MemoryInfo memory{sizeof(MemoryInfo)};
	ComputeQueue* queue = nullptr;
	State* decoder = nullptr;

	explicit Session(bool disable_sync = false) {
		ComputeMemoryInfo compute{sizeof(compute), 0, queue_memory.data};
		CHECK(QueryComputeMemoryInfo(&compute) == 0);
		CHECK(compute.cpu_gpu_memory_size == queue_memory.size);
		CHECK(compute.cpu_gpu_memory == queue_memory.data);
		ComputeConfigInfo queue_config{sizeof(queue_config), 3, 3, true, 0, 0};
		CHECK(AllocateComputeQueue(&queue_config, &compute, &queue) == 0);
		config.compute_queue = queue;
		config.disable_sync_input = disable_sync;
		CHECK(QueryDecoderMemoryInfo(&config, &memory) == 0);
		CHECK(memory.cpu_memory_size == state_memory.size);
		CHECK(memory.gpu_memory_size == 0 && memory.cpu_gpu_memory_size == 0);
		CHECK(memory.max_frame_buffer_size == 10240 && memory.frame_buffer_alignment == 256);
		memory.cpu_memory = state_memory.data;
		CHECK(CreateDecoder(&config, &memory, &decoder) == 0);
		CHECK(ReleaseComputeQueue(queue) == Fail(Error::ComputeQueue));
	}
	~Session() {
		CHECK(DeleteDecoder(decoder) == 0);
		CHECK(DeleteDecoder(decoder) == Fail(Error::DecoderInstance));
		CHECK(ReleaseComputeQueue(queue) == 0);
		CHECK(ReleaseComputeQueue(queue) == Fail(Error::ComputeQueue));
	}
	void Submit(uint32_t packet) {
		auto input = Input(packet);
		CHECK(SetDecodeInput(decoder, &input) == 0);
		InputResult result{sizeof(result)};
		CHECK(TrySyncDecodeInput(decoder, &result) == 0);
		CHECK(result.decoded_au == input.au_data && result.reserved0 == 0);
	}
};

void TestRegistrationAndErrors() {
	Loader::SymbolDatabase symbols;
	InitVdecsw_1(&symbols);
	g_print_name = false;
	constexpr std::array nids{"0moTubWCsTM", "hIgrg5h4V6s", "A+2M7EivuOU", "+L5ArV1tPGA",
	    "ecUtPX+dBYk", "fX-zOOefbbs", "aqMiF0AgUYI", "veb-YBrOqo0", "kMBw37oH8nI",
	    "rgtMCOpyBSc", "ihNT-uuEAr4", "5Y6nZqIZvBg", "l4sQYy5wPkc"};
	CHECK(registrations.size() == nids.size());
	for (size_t i = 0; i < nids.size(); ++i) {
		const auto& symbol = registrations[i];
		CHECK(symbol.name == nids[i] && symbol.library == "Vdecsw" && symbol.module == "Vdecsw");
		CHECK(symbol.library_version == 1 && symbol.module_version_major == 1 && symbol.module_version_minor == 1);
	}
	CHECK(uint32_t(Fail(Error::StructSize)) == 0x81510101u);
	CHECK(QueryComputeMemoryInfo(nullptr) == Fail(Error::ArgumentPointer));
	ComputeMemoryInfo compute{};
	CHECK(QueryComputeMemoryInfo(&compute) == Fail(Error::StructSize));
	auto config = Config();
	MemoryInfo memory{sizeof(memory)};
	config.max_width = 3840; config.max_height = 2160; config.max_level = 52;
	CHECK(QueryDecoderMemoryInfo(&config, &memory) == 0 && memory.max_frame_buffer_size == 12445696u);
	config.this_size = 72;
	CHECK(QueryDecoderMemoryInfo(&config, &memory) == Fail(Error::StructSize));
	Session session;
	GuardedMemory other_queue(sizeof(ComputeQueue));
	ComputeMemoryInfo compute_duplicate{sizeof(compute_duplicate), other_queue.size, other_queue.data};
	ComputeConfigInfo same_queue{sizeof(same_queue), 3, 3, false, 0, 0};
	ComputeQueue* duplicate = nullptr;
	CHECK(AllocateComputeQueue(&same_queue, &compute_duplicate, &duplicate) == Fail(Error::ComputeQueue));
	CHECK(duplicate == nullptr);
	compute_duplicate.cpu_gpu_memory = session.queue_memory.data;
	same_queue.queue_id = 2;
	CHECK(AllocateComputeQueue(&same_queue, &compute_duplicate, &duplicate) == Fail(Error::ComputeQueue));
	State* duplicate_decoder = nullptr;
	CHECK(CreateDecoder(&session.config, &session.memory, &duplicate_decoder) == Fail(Error::MemoryPointer));
	CHECK(duplicate_decoder == nullptr);
	CHECK(ResetDecoder(nullptr) == Fail(Error::DecoderInstance));
	InputResult completion{sizeof(completion)};
	CHECK(TrySyncDecodeInput(session.decoder, &completion) == Fail(Error::InputQueueEmpty));
	OutputInfo output{sizeof(output)};
	CHECK(TrySyncDecodeOutput(session.decoder, &output) == Fail(Error::OutputBufferEmpty));
	GuardedMemory pixels(session.memory.max_frame_buffer_size);
	FrameBuffer frame{sizeof(frame), pixels.data + 1, pixels.size};
	CHECK(SetDecodeOutput(session.decoder, &frame) == Fail(Error::FrameBufferAlignment));
	frame.data = pixels.data; frame.size--;
	CHECK(SetDecodeOutput(session.decoder, &frame) == Fail(Error::FrameBufferSize));
	frame.size++;
	CHECK(SetDecodeOutput(session.decoder, &frame) == 0);
	CHECK(SetDecodeOutput(session.decoder, &frame) == Fail(Error::OutputBufferFull));
	CHECK(TrySyncDecodeOutput(session.decoder, &output) == Fail(Error::DecodePending));
	CHECK(ResetDecoder(session.decoder) == 0);
	CHECK(TrySyncDecodeOutput(session.decoder, &output) == Fail(Error::OutputBufferEmpty));
}

void CheckPicture(const OutputInfo& output, uint32_t frame, GuardedMemory& pixels) {
	CHECK(output.valid && output.picture_count == 1 && !output.error_frame && !output.discarded_frame);
	CHECK(output.width == 16 && output.height == 16 && output.pitch == 256 && output.pitch_in_bytes == 256);
	CHECK(output.frame_buffer == pixels.data && output.frame_buffer_size == pixels.size);
	for (uint32_t y = 0; y < 16; ++y)
		for (uint32_t x = 0; x < 16; ++x) CHECK(pixels.data[y * 256 + x] == 40 + frame * 20);
	Decoder::AvcPictureInfo picture{sizeof(picture)};
	CHECK(GetAvcPictureInfo(&output, &picture, nullptr) == 0);
	CHECK(picture.is_valid && picture.pts_data == frame * 1000u);
	CHECK(picture.attached_data == 0x12340000u + frame);
	CHECK(picture.profile_idc == 100 && picture.level_idc == 10);
	CHECK(picture.pic_width_in_mbs_minus1 == 0 && picture.pic_height_in_map_units_minus1 == 0);
	pixels.Check();
}

void TestBFramesAndFinalization() {
	Session session;
	GuardedMemory pixels(session.memory.max_frame_buffer_size);
	FrameBuffer buffer{sizeof(buffer), pixels.data, pixels.size};
	CHECK(SetDecodeOutput(session.decoder, &buffer) == 0);
	uint32_t frames = 0;
	bool registered = true;
	OutputInfo output{sizeof(output)};
	const auto receive = [&] {
		if (!registered) { CHECK(SetDecodeOutput(session.decoder, &buffer) == 0); registered = true; }
		const auto result = TrySyncDecodeOutput(session.decoder, &output);
		if (result == Fail(Error::DecodePending)) return false;
		CHECK(result == 0);
		CheckPicture(output, frames++, pixels);
		registered = false;
		return true;
	};
	CHECK(!receive()); // Output may be supplied before the first AU.
	for (uint32_t packet = 0; packet < 6; ++packet) {
		session.Submit(packet);
		while (receive()) CHECK(!output.last_frame);
	}
	CHECK(frames < 6); // B pictures remain in the decoder until end of sequence.
	CHECK(FinalizeDecodeSequence(session.decoder) == 0);
	while (frames < 6) { CHECK(receive()); CHECK(output.last_frame == (frames == 6)); }
	CHECK(FinalizeDecodeSequence(session.decoder) == 0);
	auto input = Input(0);
	CHECK(SetDecodeInput(session.decoder, &input) == Fail(Error::ApiFail));
	CHECK(ResetDecoder(session.decoder) == 0);
	Decoder::AvcPictureInfo picture{sizeof(picture)};
	CHECK(GetAvcPictureInfo(&output, &picture, nullptr) == Fail(Error::OutputInfo));
	CHECK(TrySyncDecodeOutput(session.decoder, &output) == Fail(Error::OutputBufferEmpty));
	InputResult completion{sizeof(completion)};
	CHECK(TrySyncDecodeInput(session.decoder, &completion) == Fail(Error::InputQueueEmpty));
	session.Submit(0); // Reset accepts a new sequence.
}

void TestImmediateIntra() {
	Session session;
	InputData input{sizeof(input), const_cast<uint8_t*>(Intra), sizeof(Intra), 0, 0, 0x12340000};
	CHECK(SetDecodeInput(session.decoder, &input) == 0);
	InputResult completion{sizeof(completion)};
	CHECK(TrySyncDecodeInput(session.decoder, &completion) == 0);
	CHECK(completion.output_frame_count == 1 && completion.decoded_au == Intra);
	GuardedMemory pixels(session.memory.max_frame_buffer_size);
	FrameBuffer buffer{sizeof(buffer), pixels.data, pixels.size};
	CHECK(SetDecodeOutput(session.decoder, &buffer) == 0);
	OutputInfo output{sizeof(output)};
	CHECK(TrySyncDecodeOutput(session.decoder, &output) == 0);
	CheckPicture(output, 0, pixels);
	CHECK(!output.last_frame); // Output is available without another AU or finalization.
}

void TestCompletionOrderAndBackpressure() {
	Session session;
	for (uint32_t packet = 0; packet < 3; ++packet) {
		auto input = Input(packet);
		CHECK(SetDecodeInput(session.decoder, &input) == 0);
	}
	auto retry = Input(3);
	CHECK(SetDecodeInput(session.decoder, &retry) == Fail(Error::InputQueueFull));
	for (uint32_t packet = 0; packet < 3; ++packet) {
		InputResult output{sizeof(output)};
		CHECK(TrySyncDecodeInput(session.decoder, &output) == 0);
		CHECK(output.decoded_au == Input(packet).au_data);
	}
	session.Submit(3); // A rejected AU was not consumed.
	CHECK(ResetDecoder(session.decoder) == 0);
}

Decoder::Input BackendInput(uint32_t packet) {
	const auto input = Input(packet);
	return {input.au_data, input.au_size, input.pts, input.dts, input.attached_data};
}

void CheckBackendPicture(const Decoder::Output& output, uint32_t frame, GuardedMemory& pixels) {
	CHECK(output.valid && output.buffer == pixels.data && output.width == 16 && output.height == 16);
	for (uint32_t y = 0; y < 16; ++y)
		for (uint32_t x = 0; x < 16; ++x) CHECK(pixels.data[y * 256 + x] == 40 + frame * 20);
	Decoder::PictureInfo picture{};
	CHECK(Decoder::GetPictureInfo(pixels.data, &picture));
	CHECK(picture.pts == frame * 1000u && picture.attached_data == 0x12340000u + frame);
	pixels.Check();
}

void TestFailedCopyRetention() {
	auto* decoder = Decoder::Create({1, 16, 16, 8});
	CHECK(decoder != nullptr);
	for (uint32_t packet = 0; packet < 6; ++packet) {
		uint32_t produced = 0;
		CHECK(Decoder::Submit(decoder, BackendInput(packet), &produced) == Decoder::Result::Ok);
	}
	CHECK(Decoder::Finalize(decoder) == Decoder::Result::Ok);
	GuardedMemory pixels(10240);
	Decoder::Output output{};
	CHECK(Decoder::Receive(decoder, {pixels.data, 1}, &output) == Decoder::Result::FrameBufferSize);
	CHECK(!output.valid);
	for (uint32_t frame = 0; frame < 6; ++frame) {
		CHECK(Decoder::Receive(decoder, {pixels.data, pixels.size}, &output) == Decoder::Result::Ok);
		CheckBackendPicture(output, frame, pixels);
		CHECK(output.last_frame == (frame == 5));
	}
	CHECK(Decoder::Receive(decoder, {pixels.data, pixels.size}, &output) == Decoder::Result::NoFrame);
	Decoder::PictureInfo picture{};
	Decoder::Destroy(decoder);
	CHECK(!Decoder::GetPictureInfo(pixels.data, &picture));
}

void TestBoundedQueueFinalization() {
	auto* decoder = Decoder::Create({1, 16, 16, 2});
	CHECK(decoder != nullptr);
	GuardedMemory pixels(10240);
	Decoder::Output output{};
	uint32_t frames = 0;
	uint32_t rejected = 0;
	for (uint32_t packet = 0; packet < 6;) {
		uint32_t produced = 0;
		const auto result = Decoder::Submit(decoder, BackendInput(packet), &produced);
		if (result == Decoder::Result::InputFull) {
			CHECK(produced == 0);
			++rejected;
			CHECK(Decoder::Receive(decoder, {pixels.data, pixels.size}, &output) == Decoder::Result::Ok);
			CheckBackendPicture(output, frames++, pixels);
			CHECK(!output.last_frame);
		} else { CHECK(result == Decoder::Result::Ok); ++packet; }
	}
	CHECK(rejected > 0 && frames < 6);
	// The retained queue is full, and FFmpeg still holds delayed B pictures.
	CHECK(Decoder::Finalize(decoder) == Decoder::Result::Ok);
	while (frames < 6) {
		CHECK(Decoder::Receive(decoder, {pixels.data, pixels.size}, &output) == Decoder::Result::Ok);
		CheckBackendPicture(output, frames++, pixels);
		CHECK(output.last_frame == (frames == 6));
	}
	CHECK(Decoder::Receive(decoder, {pixels.data, pixels.size}, &output) == Decoder::Result::NoFrame);
	Decoder::Destroy(decoder);
}

void TestCombinedDecode() {
	auto* decoder = Decoder::Create({1, 16, 16});
	CHECK(decoder != nullptr);
	GuardedMemory pixels(10240);
	Decoder::Output output{};
	uint32_t frames = 0;
	for (uint32_t packet = 0; packet < 6; ++packet) {
		CHECK(Decoder::Decode(decoder, BackendInput(packet), {pixels.data, pixels.size}, &output) == Decoder::Result::Ok);
		if (output.valid) CheckBackendPicture(output, frames++, pixels);
		CHECK(Decoder::Flush(decoder, {pixels.data, pixels.size}, &output) == Decoder::Result::Ok);
		CHECK(!output.valid); // Legacy Flush is receive-only; the next AU remains acceptable.
	}
	CHECK(frames == 4);
	CHECK(Decoder::Finalize(decoder) == Decoder::Result::Ok);
	while (frames < 6) {
		CHECK(Decoder::Flush(decoder, {pixels.data, pixels.size}, &output) == Decoder::Result::Ok);
		CheckBackendPicture(output, frames++, pixels);
	}
	Decoder::Destroy(decoder);
}

void TestDisabledInputSync() {
	Session session(true);
	auto input = Input(0);
	CHECK(SetDecodeInput(session.decoder, &input) == 0);
	InputResult completion{sizeof(completion)};
	CHECK(TrySyncDecodeInput(session.decoder, &completion) == Fail(Error::ApiFail));
}
}

int main() {
	TestRegistrationAndErrors();
	TestBFramesAndFinalization();
	TestImmediateIntra();
	TestCompletionOrderAndBackpressure();
	TestFailedCopyRetention();
	TestBoundedQueueFinalization();
	TestCombinedDecode();
	TestDisabledInputSync();
	std::puts("Vdecsw tests passed");
}
