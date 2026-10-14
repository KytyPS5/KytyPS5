#include "common/abi.h"
#include "libs/libs.h"
#include "libs/videoDec2Decoder.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <unordered_set>

namespace Libs::Vdecsw {

LIB_VERSION("Vdecsw", 1, "Vdecsw", 1, 1);

namespace Decoder = VideoDec2::Decoder;

enum class Error : uint32_t {
	ApiFail = 0x0100,
	StructSize,
	ArgumentPointer,
	DecoderInstance,
	MemorySize,
	MemoryPointer,
	FrameBufferSize,
	FrameBufferPointer,
	FrameBufferAlignment,
	AccessUnitSize = 0x010d,
	AccessUnitPointer,
	OutputInfo,
	ComputeQueue,
	InputQueueFull = 0x0113,
	OutputBufferFull,
	InputQueueEmpty = 0x0116,
	DecodePending,
	OutputBufferEmpty,
	ConfigInfo = 0x0200,
	ComputePipeId,
	ComputeQueueId,
	ResourceType,
	CodecType,
	InputQueueDepth = 0x0206,
	DpbFrameCount   = 0x0209,
	FrameWidthHeight,
	ExtraConfigInfo,
	AccessUnit = 0x0301,
	OversizeDecode,
};

constexpr int32_t Fail(Error error) {
	return static_cast<int32_t>(0x81510000u | static_cast<uint32_t>(error));
}

struct ComputeMemoryInfo {
	size_t this_size;
	size_t cpu_gpu_memory_size;
	void*  cpu_gpu_memory;
};

struct ComputeConfigInfo {
	size_t   this_size;
	uint16_t pipe_id;
	uint16_t queue_id;
	bool     check_memory_type;
	uint8_t  reserved0;
	uint16_t reserved1;
};

struct ComputeQueue {
	uint16_t pipe_id;
	uint16_t queue_id;
	uint32_t decoders = 0;
};

struct ConfigInfo {
	size_t        this_size;
	uint32_t      resource_type;
	uint32_t      codec_type;
	uint32_t      profile;
	uint32_t      max_level;
	int32_t       max_width;
	int32_t       max_height;
	int32_t       max_dpb_frames;
	uint32_t      input_queue_depth;
	ComputeQueue* compute_queue;
	uint64_t      cpu_affinity_mask;
	int32_t       cpu_thread_priority;
	bool          optimize_progressive;
	bool          check_memory_type;
	uint8_t       reserved0;
	int8_t        extra_dpb_frames;
	void*         extra_config;
	bool          disable_sync_input;
	uint32_t      max_pending_sync;
};

struct MemoryInfo {
	size_t   this_size;
	size_t   cpu_memory_size;
	void*    cpu_memory;
	size_t   gpu_memory_size;
	void*    gpu_memory;
	size_t   cpu_gpu_memory_size;
	void*    cpu_gpu_memory;
	size_t   max_frame_buffer_size;
	uint32_t frame_buffer_alignment;
	uint32_t reserved0;
};

struct InputData {
	size_t   this_size;
	void*    au_data;
	size_t   au_size;
	uint64_t pts;
	uint64_t dts;
	uint64_t attached_data;
};

struct InputResult {
	size_t   this_size;
	void*    decoded_au;
	uint32_t output_frame_count;
	uint32_t reserved0;
};

struct FrameBuffer {
	size_t this_size;
	void*  data;
	size_t size;
};

struct OutputInfo {
	size_t   this_size;
	bool     valid;
	bool     last_frame;
	bool     error_frame;
	uint8_t  picture_count;
	uint32_t codec_type;
	uint32_t width;
	uint32_t pitch;
	uint32_t height;
	bool     discarded_frame;
	void*    frame_buffer;
	size_t   frame_buffer_size;
	uint32_t format;
	uint32_t pitch_in_bytes;
};

static_assert(sizeof(ComputeMemoryInfo) == 24);
static_assert(sizeof(ComputeConfigInfo) == 16);
static_assert(sizeof(ConfigInfo) == 80);
static_assert(offsetof(ConfigInfo, extra_dpb_frames) == 63);
static_assert(offsetof(ConfigInfo, disable_sync_input) == 72);
static_assert(sizeof(MemoryInfo) == 72);
static_assert(sizeof(InputData) == 48);
static_assert(sizeof(InputResult) == 24);
static_assert(sizeof(FrameBuffer) == 24);
static_assert(sizeof(OutputInfo) == 56);
static_assert(offsetof(OutputInfo, discarded_frame) == 28);
static_assert(offsetof(OutputInfo, frame_buffer) == 32);

struct State;
static std::shared_mutex                 g_registry_mutex;
static std::unordered_set<ComputeQueue*> g_queues;
static std::unordered_set<State*>        g_decoders;

struct State {
	ConfigInfo                 config;
	Decoder::Instance*         decoder;
	std::mutex                 mutex;
	std::deque<InputResult>    completed_inputs;
	std::optional<FrameBuffer> output_buffer;

	~State() {
		Decoder::Destroy(decoder);
		--config.compute_queue->decoders;
	}
};

// The shared registry lock keeps guest-backed state alive until each API call returns.
// Deletion takes the exclusive lock before releasing that memory to the guest.
struct StateAccess {
	std::shared_lock<std::shared_mutex> lock {g_registry_mutex};
	State*                              value;
	explicit StateAccess(State* handle): value(g_decoders.contains(handle) ? handle : nullptr) {}
	explicit operator bool() const { return value != nullptr; }
	State*   operator->() const { return value; }
};

static int32_t Result(Decoder::Result result) {
	switch (result) {
		case Decoder::Result::Ok: return 0;
		case Decoder::Result::ApiFail: return Fail(Error::ApiFail);
		case Decoder::Result::AccessUnit: return Fail(Error::AccessUnit);
		case Decoder::Result::FrameBufferSize: return Fail(Error::FrameBufferSize);
		case Decoder::Result::OversizeDecode: return Fail(Error::OversizeDecode);
		case Decoder::Result::InputFull: return Fail(Error::OutputBufferFull);
		case Decoder::Result::NoFrame: return Fail(Error::DecodePending);
	}
	return Fail(Error::ApiFail);
}

static int32_t ValidateConfig(const ConfigInfo* config) {
	if (config == nullptr) return Fail(Error::ArgumentPointer);
	if (config->this_size != sizeof(ConfigInfo)) return Fail(Error::StructSize);
	if (config->resource_type != 1) return Fail(Error::ResourceType);
	if (config->codec_type != 1) return Fail(Error::CodecType);
	if (config->reserved0 != 0) return Fail(Error::ConfigInfo);
	if (config->extra_config != nullptr) return Fail(Error::ExtraConfigInfo);
	if (config->input_queue_depth < 1 || config->input_queue_depth > 8)
		return Fail(Error::InputQueueDepth);
	if (config->max_dpb_frames < 1 || config->extra_dpb_frames < 0)
		return Fail(Error::DpbFrameCount);
	if (config->max_width <= 0 || config->max_height <= 0) return Fail(Error::FrameWidthHeight);
	return 0;
}

static size_t FrameSize(const ConfigInfo& config) {
	// AVC output: 256-byte pitch, 16-line maximum height, and 4096 bytes of metadata.
	const auto pitch  = (static_cast<size_t>(config.max_width) + 255u) & ~size_t {255};
	const auto height = (static_cast<size_t>(config.max_height) + 15u) & ~size_t {15};
	return pitch * height * 3 / 2 + 4096;
}

static int32_t KYTY_SYSV_ABI QueryComputeMemoryInfo(ComputeMemoryInfo* memory) {
	PRINT_NAME();
	if (memory == nullptr) return Fail(Error::ArgumentPointer);
	if (memory->this_size != sizeof(ComputeMemoryInfo)) return Fail(Error::StructSize);
	memory->cpu_gpu_memory_size = sizeof(ComputeQueue);
	return 0;
}

static int32_t KYTY_SYSV_ABI AllocateComputeQueue(const ComputeConfigInfo* config,
                                                  const ComputeMemoryInfo* memory,
                                                  ComputeQueue**           output) {
	PRINT_NAME();
	if (config == nullptr || memory == nullptr || output == nullptr)
		return Fail(Error::ArgumentPointer);
	if (config->this_size != sizeof(ComputeConfigInfo) ||
	    memory->this_size != sizeof(ComputeMemoryInfo))
		return Fail(Error::StructSize);
	if (config->reserved0 != 0 || config->reserved1 != 0) return Fail(Error::ConfigInfo);
	if (config->pipe_id > 4) return Fail(Error::ComputePipeId);
	if (config->queue_id > 7) return Fail(Error::ComputeQueueId);
	if (memory->cpu_gpu_memory_size < sizeof(ComputeQueue)) return Fail(Error::MemorySize);
	if (memory->cpu_gpu_memory == nullptr ||
	    reinterpret_cast<uintptr_t>(memory->cpu_gpu_memory) % alignof(ComputeQueue) != 0)
		return Fail(Error::MemoryPointer);
	std::scoped_lock lock(g_registry_mutex);
	for (const auto* queue: g_queues)
		if ((queue->pipe_id == config->pipe_id && queue->queue_id == config->queue_id) ||
		    queue == memory->cpu_gpu_memory)
			return Fail(Error::ComputeQueue);
	auto* queue = new (memory->cpu_gpu_memory) ComputeQueue {config->pipe_id, config->queue_id};
	g_queues.insert(queue);
	*output = queue;
	return 0;
}

static int32_t KYTY_SYSV_ABI ReleaseComputeQueue(ComputeQueue* queue) {
	PRINT_NAME();
	std::scoped_lock lock(g_registry_mutex);
	if (!g_queues.contains(queue) || queue->decoders != 0) return Fail(Error::ComputeQueue);
	g_queues.erase(queue);
	queue->~ComputeQueue();
	return 0;
}

static int32_t KYTY_SYSV_ABI QueryDecoderMemoryInfo(const ConfigInfo* config, MemoryInfo* memory) {
	PRINT_NAME();
	if (const auto error = ValidateConfig(config); error != 0) return error;
	if (memory == nullptr) return Fail(Error::ArgumentPointer);
	if (memory->this_size != sizeof(MemoryInfo)) return Fail(Error::StructSize);
	memory->cpu_memory_size = sizeof(State);
	// The FFmpeg backend needs no guest GPU workspace.
	memory->gpu_memory_size        = 0;
	memory->cpu_gpu_memory_size    = 0;
	memory->max_frame_buffer_size  = FrameSize(*config);
	memory->frame_buffer_alignment = 256;
	memory->reserved0              = 0;
	return 0;
}

static int32_t KYTY_SYSV_ABI CreateDecoder(const ConfigInfo* config, const MemoryInfo* memory,
                                           State** output) {
	PRINT_NAME();
	if (const auto error = ValidateConfig(config); error != 0) return error;
	if (memory == nullptr || output == nullptr) return Fail(Error::ArgumentPointer);
	if (memory->this_size != sizeof(MemoryInfo)) return Fail(Error::StructSize);
	if (memory->cpu_memory_size < sizeof(State)) return Fail(Error::MemorySize);
	if (memory->cpu_memory == nullptr ||
	    reinterpret_cast<uintptr_t>(memory->cpu_memory) % alignof(State) != 0)
		return Fail(Error::MemoryPointer);
	std::scoped_lock lock(g_registry_mutex);
	if (!g_queues.contains(config->compute_queue)) return Fail(Error::ComputeQueue);
	if (g_decoders.contains(static_cast<State*>(memory->cpu_memory)))
		return Fail(Error::MemoryPointer);
	auto* decoder = Decoder::Create(
	    {config->codec_type, config->max_width, config->max_height,
	     std::max(2u, static_cast<uint32_t>(config->max_dpb_frames) + config->extra_dpb_frames)});
	if (decoder == nullptr) return Fail(Error::ApiFail);
	auto* state = new (memory->cpu_memory) State {*config, decoder};
	++config->compute_queue->decoders;
	g_decoders.insert(state);
	*output = state;
	return 0;
}

static int32_t KYTY_SYSV_ABI DeleteDecoder(State* handle) {
	PRINT_NAME();
	std::scoped_lock lock(g_registry_mutex);
	if (g_decoders.erase(handle) == 0) return Fail(Error::DecoderInstance);
	handle->~State();
	return 0;
}

static int32_t KYTY_SYSV_ABI SetDecodeInput(State* handle, const InputData* input) {
	PRINT_NAME();
	const StateAccess state(handle);
	if (!state) return Fail(Error::DecoderInstance);
	if (input == nullptr) return Fail(Error::ArgumentPointer);
	if (input->this_size != sizeof(InputData)) return Fail(Error::StructSize);
	if (input->au_data == nullptr) return Fail(Error::AccessUnitPointer);
	if (input->au_size == 0) return Fail(Error::AccessUnitSize);
	std::scoped_lock lock(state->mutex);
	const auto&      config = state->config;
	if (!config.disable_sync_input &&
	    state->completed_inputs.size() >=
	        uint64_t {config.input_queue_depth} + config.max_pending_sync)
		return Fail(Error::InputQueueFull);
	uint32_t   produced = 0;
	const auto result   = Decoder::Submit(
	    state->decoder,
	    {input->au_data, input->au_size, input->pts, input->dts, input->attached_data}, &produced);
	if (result != Decoder::Result::Ok) return Result(result);
	if (!config.disable_sync_input)
		state->completed_inputs.push_back({sizeof(InputResult), input->au_data, produced, 0});
	return 0;
}

static int32_t KYTY_SYSV_ABI TrySyncDecodeInput(State* handle, InputResult* output) {
	PRINT_NAME();
	const StateAccess state(handle);
	if (!state) return Fail(Error::DecoderInstance);
	if (output == nullptr) return Fail(Error::ArgumentPointer);
	if (output->this_size != sizeof(InputResult)) return Fail(Error::StructSize);
	std::scoped_lock lock(state->mutex);
	if (state->config.disable_sync_input) return Fail(Error::ApiFail);
	if (state->completed_inputs.empty()) return Fail(Error::InputQueueEmpty);
	*output = state->completed_inputs.front();
	state->completed_inputs.pop_front();
	return 0;
}

static int32_t KYTY_SYSV_ABI SetDecodeOutput(State* handle, FrameBuffer* buffer) {
	PRINT_NAME();
	const StateAccess state(handle);
	if (!state) return Fail(Error::DecoderInstance);
	if (buffer == nullptr) return Fail(Error::ArgumentPointer);
	if (buffer->this_size != sizeof(FrameBuffer)) return Fail(Error::StructSize);
	if (buffer->data == nullptr) return Fail(Error::FrameBufferPointer);
	if (reinterpret_cast<uintptr_t>(buffer->data) % 256 != 0)
		return Fail(Error::FrameBufferAlignment);
	if (buffer->size < FrameSize(state->config)) return Fail(Error::FrameBufferSize);
	std::scoped_lock lock(state->mutex);
	if (state->output_buffer) return Fail(Error::OutputBufferFull);
	state->output_buffer = *buffer;
	return 0;
}

static int32_t KYTY_SYSV_ABI TrySyncDecodeOutput(State* handle, OutputInfo* output) {
	PRINT_NAME();
	const StateAccess state(handle);
	if (!state) return Fail(Error::DecoderInstance);
	if (output == nullptr) return Fail(Error::ArgumentPointer);
	if (output->this_size != sizeof(OutputInfo)) return Fail(Error::StructSize);
	std::scoped_lock lock(state->mutex);
	if (!state->output_buffer) return Fail(Error::OutputBufferEmpty);
	const auto&     buffer = *state->output_buffer;
	Decoder::Output decoded {};
	const auto      result = Decoder::Receive(state->decoder, {buffer.data, buffer.size}, &decoded);
	if (result != Decoder::Result::Ok) return Result(result);
	*output = {sizeof(OutputInfo),
	           true,
	           decoded.last_frame,
	           decoded.error_frame,
	           1,
	           decoded.codec_type,
	           decoded.width,
	           decoded.pitch,
	           decoded.height,
	           false,
	           decoded.buffer,
	           decoded.buffer_size,
	           0,
	           decoded.pitch};
	state->output_buffer.reset();
	return 0;
}

static int32_t KYTY_SYSV_ABI FinalizeDecodeSequence(State* handle) {
	PRINT_NAME();
	const StateAccess state(handle);
	if (!state) return Fail(Error::DecoderInstance);
	std::scoped_lock lock(state->mutex);
	return Result(Decoder::Finalize(state->decoder));
}

static int32_t KYTY_SYSV_ABI ResetDecoder(State* handle) {
	PRINT_NAME();
	const StateAccess state(handle);
	if (!state) return Fail(Error::DecoderInstance);
	std::scoped_lock lock(state->mutex);
	Decoder::Reset(state->decoder);
	state->completed_inputs.clear();
	state->output_buffer.reset();
	return 0;
}

static int32_t KYTY_SYSV_ABI GetAvcPictureInfo(const OutputInfo*        output,
                                               Decoder::AvcPictureInfo* first,
                                               Decoder::AvcPictureInfo* second) {
	PRINT_NAME();
	if (output == nullptr || first == nullptr) return Fail(Error::ArgumentPointer);
	if (output->this_size != sizeof(OutputInfo) || first->this_size != sizeof(*first) ||
	    (second != nullptr && second->this_size != sizeof(*second)))
		return Fail(Error::StructSize);
	if (!output->valid || output->codec_type != 1 || output->picture_count == 0)
		return Fail(Error::OutputInfo);
	Decoder::PictureInfo decoded {};
	if (!Decoder::GetPictureInfo(output->frame_buffer, &decoded) || decoded.codec_type != 1)
		return Fail(Error::OutputInfo);
	*first = Decoder::MakeAvcPictureInfo(decoded, sizeof(*first));
	if (second != nullptr) *second = {.this_size = sizeof(*second)};
	return 0;
}

LIB_DEFINE(InitVdecsw_1) {
	PRINT_NAME_ENABLE(true);
	LIB_FUNC("0moTubWCsTM", QueryComputeMemoryInfo);
	LIB_FUNC("hIgrg5h4V6s", AllocateComputeQueue);
	LIB_FUNC("A+2M7EivuOU", QueryDecoderMemoryInfo);
	LIB_FUNC("+L5ArV1tPGA", CreateDecoder);
	LIB_FUNC("ecUtPX+dBYk", DeleteDecoder);
	LIB_FUNC("fX-zOOefbbs", ReleaseComputeQueue);
	LIB_FUNC("aqMiF0AgUYI", SetDecodeInput);
	LIB_FUNC("veb-YBrOqo0", ResetDecoder);
	LIB_FUNC("kMBw37oH8nI", TrySyncDecodeOutput);
	LIB_FUNC("rgtMCOpyBSc", SetDecodeOutput);
	LIB_FUNC("ihNT-uuEAr4", GetAvcPictureInfo);
	LIB_FUNC("5Y6nZqIZvBg", FinalizeDecodeSequence);
	LIB_FUNC("l4sQYy5wPkc", TrySyncDecodeInput);
}

} // namespace Libs::Vdecsw
