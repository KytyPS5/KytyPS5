#include "common/abi.h"
#include "common/common.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/videoDec2.h"
#include "libs/videoDec2Decoder.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs {

LIB_VERSION("Vdecsw", 1, "Vdecsw", 1, 1);

// Software video decoding. Its configuration and memory structures extend Videodec2's and it
// decodes through the same backend, but the guest drives decoding as two queues: it submits
// access units and output frame buffers, then polls each queue for completion.
namespace Vdecsw {

using namespace VideoDec2;

constexpr int32_t ERROR_INPUT_CLOSED = static_cast<int32_t>(0x81510113u);
// The guest retries a full input queue after it has consumed output.
constexpr int32_t ERROR_INPUT_QUEUE_FULL = static_cast<int32_t>(0x81510114u);
constexpr int32_t ERROR_OUTPUT_PENDING   = static_cast<int32_t>(0x81510115u);
// No submitted input awaits completion; the guest then finalizes the sequence.
constexpr int32_t ERROR_INPUT_DRAINED = static_cast<int32_t>(0x81510116u);
constexpr int32_t ERROR_STREAM_ENDED  = static_cast<int32_t>(0x81510117u);

// The configuration extends Videodec2's with flags in its reserved bytes and 8 more bytes.
constexpr size_t CONFIG_SIZE = 80;
// The backend owns its decoding memory; the guest only provides an identifying workspace.
constexpr size_t WORKSPACE_SIZE         = 256;
constexpr size_t MAX_QUEUED_INPUT_BYTES = 64ull * 1024ull * 1024ull;

struct FrameBuffer {
	size_t this_size;
	void*  frame_buffer;
	size_t frame_buffer_size;
};

struct OutputInfo {
	size_t   this_size;
	bool     is_valid;
	bool     is_end_of_stream;
	bool     is_error_frame;
	uint8_t  picture_count;
	uint32_t codec_type;
	uint32_t frame_width;
	uint32_t frame_pitch;
	uint32_t frame_height;
	void*    frame_buffer;
	size_t   frame_buffer_size;
	uint32_t frame_format;
	uint32_t frame_pitch_in_bytes;
};

struct InputCompletion {
	size_t   this_size;
	uint64_t reserved;
	uint32_t completed_count;
	uint32_t reserved1;
};

static_assert(sizeof(FrameBuffer) == 24);
static_assert(sizeof(OutputInfo) == 56 && offsetof(OutputInfo, picture_count) == 11);
static_assert(sizeof(InputCompletion) == 24 && offsetof(InputCompletion, completed_count) == 16);

// Vdecsw reports Videodec2's error codes in its own facility.
static int32_t ToVdecsw(int32_t result) {
	return result == OK
	           ? OK
	           : static_cast<int32_t>(0x81510000u | (static_cast<uint32_t>(result) & 0xffffu));
}

class SoftwareDecoder {
public:
	SoftwareDecoder(Decoder::Instance* backend, uint32_t depth)
	    : m_backend(backend), m_depth(depth) {}
	~SoftwareDecoder() { Decoder::Destroy(m_backend); }
	KYTY_CLASS_NO_COPY(SoftwareDecoder);

	int32_t SetInput(const Videodec2InputData& input) {
		std::scoped_lock lock(m_mutex);
		if (m_finishing) {
			return ERROR_INPUT_CLOSED;
		}
		// The depth bounds inputs whose completion the guest has not yet observed. Each AU is
		// copied, so its guest buffer is free at once; a byte budget bounds the copies while
		// no output buffer is available.
		if (m_unobserved_inputs >= m_depth ||
		    (!m_inputs.empty() && m_queued_bytes + input.au_size > MAX_QUEUED_INPUT_BYTES)) {
			return ERROR_INPUT_QUEUE_FULL;
		}
		const auto* bytes = static_cast<const uint8_t*>(input.au_data);
		m_inputs.push_back({input, std::vector<uint8_t>(bytes, bytes + input.au_size)});
		m_queued_bytes += input.au_size;
		m_unobserved_inputs++;
		Pump();
		return m_error;
	}

	int32_t SetOutput(const FrameBuffer& buffer) {
		std::scoped_lock lock(m_mutex);
		m_buffers.push_back(buffer);
		Pump();
		return m_error;
	}

	int32_t TrySyncInput(InputCompletion* completion) {
		std::scoped_lock lock(m_mutex);
		Pump();
		*completion = {.this_size       = sizeof(*completion),
		               .completed_count = std::exchange(m_unobserved_inputs, 0u)};
		if (m_error != OK) {
			return m_error;
		}
		return completion->completed_count != 0u ? OK : ERROR_INPUT_DRAINED;
	}

	int32_t TrySyncOutput(OutputInfo* output) {
		std::scoped_lock lock(m_mutex);
		*output = {.this_size = sizeof(*output)};
		Pump();
		if (m_error != OK) {
			return m_error;
		}
		if (m_outputs.empty()) {
			return m_drained ? ERROR_STREAM_ENDED : ERROR_OUTPUT_PENDING;
		}
		const auto decoded = m_outputs.front();
		m_outputs.pop_front();
		*output = {.this_size            = sizeof(*output),
		           .is_valid             = true,
		           .is_end_of_stream     = decoded.end_of_stream,
		           .is_error_frame       = decoded.error_frame,
		           .picture_count        = 1,
		           .codec_type           = decoded.codec_type,
		           .frame_width          = decoded.width,
		           .frame_pitch          = decoded.pitch,
		           .frame_height         = decoded.height,
		           .frame_buffer         = decoded.buffer,
		           .frame_buffer_size    = decoded.buffer_size,
		           .frame_pitch_in_bytes = decoded.pitch};
		return OK;
	}

	int32_t Finalize() {
		std::scoped_lock lock(m_mutex);
		m_finishing = true;
		Pump();
		return m_error;
	}

	void Reset() {
		std::scoped_lock lock(m_mutex);
		m_unobserved_inputs = 0;
		m_queued_bytes      = 0;
		m_finishing         = false;
		m_drained           = false;
		m_error             = OK;
		m_inputs.clear();
		m_buffers.clear();
		m_outputs.clear();
		Decoder::Reset(m_backend);
	}

private:
	struct QueuedInput {
		Videodec2InputData   info;
		std::vector<uint8_t> bytes;
	};

	// Decodes queued input into reserved frame buffers. A reservation stays queued across AUs
	// that produce no picture (decoder reordering delay) until a picture fills it.
	void Pump() {
		while (m_error == OK && !m_drained && !m_buffers.empty()) {
			const auto&                buffer = m_buffers.front();
			const Decoder::FrameBuffer target {buffer.frame_buffer, buffer.frame_buffer_size};
			Decoder::Output            decoded {};
			Decoder::Result            result = Decoder::Result::Ok;
			if (!m_inputs.empty()) {
				const auto input = std::move(m_inputs.front());
				m_inputs.pop_front();
				m_queued_bytes -= input.bytes.size();
				result =
				    Decoder::Decode(m_backend,
				                    {input.bytes.data(), input.bytes.size(), input.info.pts_data,
				                     input.info.dts_data, input.info.attached_data},
				                    target, &decoded);
			} else if (m_finishing) {
				result = Decoder::Drain(m_backend, target, &decoded);
				m_drained =
				    result == Decoder::Result::Ok && (!decoded.valid || decoded.end_of_stream);
			} else {
				// Collect pictures the backend completed beyond one per AU.
				result = Decoder::Flush(m_backend, target, &decoded);
				if (result == Decoder::Result::Ok && !decoded.valid) {
					break;
				}
			}
			m_error = ToVdecsw(MapDecoderResult(result));
			if (decoded.valid) {
				m_outputs.push_back(decoded);
				m_buffers.pop_front();
			}
		}
	}

	std::mutex                  m_mutex;
	Decoder::Instance*          m_backend;
	uint32_t                    m_depth;
	uint32_t                    m_unobserved_inputs = 0;
	size_t                      m_queued_bytes      = 0;
	bool                        m_finishing         = false;
	bool                        m_drained           = false;
	int32_t                     m_error             = OK;
	std::deque<QueuedInput>     m_inputs;
	std::deque<FrameBuffer>     m_buffers;
	std::deque<Decoder::Output> m_outputs;
};

static std::mutex                                                  g_decoders_mutex;
static std::unordered_map<void*, std::shared_ptr<SoftwareDecoder>> g_decoders;

static std::shared_ptr<SoftwareDecoder> FindDecoder(Videodec2Decoder decoder) {
	std::scoped_lock lock(g_decoders_mutex);
	const auto       found = g_decoders.find(decoder);
	return found != g_decoders.end() ? found->second : nullptr;
}

static int32_t ValidateConfig(const Videodec2DecoderConfigInfo* config, bool require_queue) {
	if (config->this_size != CONFIG_SIZE) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}
	// Validate the shared prefix; Vdecsw defines flags in the bytes Videodec2 reserves.
	auto common      = *config;
	common.this_size = sizeof(common);
	common.reserved0 = 0;
	common.reserved1 = 0;
	return ValidateDecoderConfig(&common, require_queue);
}

static int32_t KYTY_SYSV_ABI QueryComputeMemoryInfo(Videodec2ComputeMemoryInfo* info) {
	PRINT_NAME();

	if (info == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	}
	if (info->this_size != sizeof(*info)) {
		return ToVdecsw(VIDEODEC2_ERROR_STRUCT_SIZE);
	}
	info->cpu_gpu_memory_size = WORKSPACE_SIZE;
	info->cpu_gpu_memory      = nullptr;
	return OK;
}

static int32_t KYTY_SYSV_ABI AllocateComputeQueue(const Videodec2ComputeConfigInfo* config,
                                                  const Videodec2ComputeMemoryInfo* memory,
                                                  Videodec2ComputeQueue*            queue) {
	PRINT_NAME();

	if (config == nullptr || memory == nullptr || queue == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	}
	if (config->this_size != sizeof(*config) || memory->this_size != sizeof(*memory)) {
		return ToVdecsw(VIDEODEC2_ERROR_STRUCT_SIZE);
	}
	if (const auto result = ValidateComputeQueueConfig(config); result != OK) {
		return ToVdecsw(result);
	}
	if (memory->cpu_gpu_memory_size < WORKSPACE_SIZE) {
		return ToVdecsw(VIDEODEC2_ERROR_MEMORY_SIZE);
	}
	if (memory->cpu_gpu_memory == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_MEMORY_POINTER);
	}
	*queue = memory->cpu_gpu_memory;
	return OK;
}

static int32_t KYTY_SYSV_ABI ReleaseComputeQueue(Videodec2ComputeQueue queue) {
	PRINT_NAME();

	return queue != nullptr ? OK : ToVdecsw(VIDEODEC2_ERROR_COMPUTE_QUEUE);
}

static int32_t KYTY_SYSV_ABI QueryDecoderMemoryInfo(const Videodec2DecoderConfigInfo* config,
                                                    Videodec2DecoderMemoryInfo*       memory) {
	PRINT_NAME();

	if (config == nullptr || memory == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	}
	if (memory->this_size != sizeof(*memory)) {
		return ToVdecsw(VIDEODEC2_ERROR_STRUCT_SIZE);
	}
	if (const auto result = ValidateConfig(config, false); result != OK) {
		return ToVdecsw(result);
	}
	// Output pictures are NV12 with a 256-byte pitch.
	const uint64_t width  = config->max_frame_width > 0 ? config->max_frame_width : 4096u;
	const uint64_t height = config->max_frame_height > 0 ? config->max_frame_height : 2160u;
	const auto     pitch  = (width + 255u) & ~uint64_t {255};
	*memory               = {.this_size              = sizeof(*memory),
	                         .cpu_memory_size        = WORKSPACE_SIZE,
	                         .max_frame_buffer_size  = pitch * (height + (height + 1u) / 2u),
	                         .frame_buffer_alignment = 256};
	return OK;
}

static int32_t KYTY_SYSV_ABI CreateDecoder(const Videodec2DecoderConfigInfo* config,
                                           const Videodec2DecoderMemoryInfo* memory,
                                           Videodec2Decoder*                 decoder) {
	PRINT_NAME();

	if (config == nullptr || memory == nullptr || decoder == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	}
	if (memory->this_size != sizeof(*memory)) {
		return ToVdecsw(VIDEODEC2_ERROR_STRUCT_SIZE);
	}
	if (const auto result = ValidateConfig(config, true); result != OK) {
		return ToVdecsw(result);
	}
	if (memory->cpu_memory_size < WORKSPACE_SIZE) {
		return ToVdecsw(VIDEODEC2_ERROR_MEMORY_SIZE);
	}
	if (memory->cpu_memory == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_MEMORY_POINTER);
	}
	auto* backend =
	    Decoder::Create({config->codec_type, config->max_frame_width, config->max_frame_height});
	if (backend == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_API_FAIL);
	}
	auto state = std::make_shared<SoftwareDecoder>(backend, config->decode_input_queue_depth);
	{
		std::scoped_lock lock(g_decoders_mutex);
		g_decoders.emplace(state.get(), state);
	}
	*decoder = state.get();
	return OK;
}

static int32_t KYTY_SYSV_ABI DeleteDecoder(Videodec2Decoder decoder) {
	PRINT_NAME();

	std::scoped_lock lock(g_decoders_mutex);
	// A call still running on another thread keeps the decoder alive until it returns.
	return g_decoders.erase(decoder) != 0 ? OK : ToVdecsw(VIDEODEC2_ERROR_DECODER_INSTANCE);
}

static int32_t KYTY_SYSV_ABI ResetDecoder(Videodec2Decoder decoder) {
	PRINT_NAME();

	const auto state = FindDecoder(decoder);
	if (state == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_DECODER_INSTANCE);
	}
	state->Reset();
	return OK;
}

static int32_t KYTY_SYSV_ABI SetDecodeInput(Videodec2Decoder          decoder,
                                            const Videodec2InputData* input) {
	PRINT_NAME();

	if (input == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	}
	if (input->this_size != sizeof(*input)) {
		return ToVdecsw(VIDEODEC2_ERROR_STRUCT_SIZE);
	}
	if (input->au_data == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ACCESS_UNIT_POINTER);
	}
	if (input->au_size == 0 || input->au_size > INT32_MAX) {
		return ToVdecsw(VIDEODEC2_ERROR_ACCESS_UNIT_SIZE);
	}
	const auto state = FindDecoder(decoder);
	return state != nullptr ? state->SetInput(*input) : ToVdecsw(VIDEODEC2_ERROR_DECODER_INSTANCE);
}

static int32_t KYTY_SYSV_ABI SetDecodeOutput(Videodec2Decoder decoder, const FrameBuffer* buffer) {
	PRINT_NAME();

	if (buffer == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	}
	if (buffer->this_size != sizeof(*buffer)) {
		return ToVdecsw(VIDEODEC2_ERROR_STRUCT_SIZE);
	}
	if (buffer->frame_buffer == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_FRAME_BUFFER_POINTER);
	}
	if (buffer->frame_buffer_size == 0) {
		return ToVdecsw(VIDEODEC2_ERROR_FRAME_BUFFER_SIZE);
	}
	const auto state = FindDecoder(decoder);
	return state != nullptr ? state->SetOutput(*buffer)
	                        : ToVdecsw(VIDEODEC2_ERROR_DECODER_INSTANCE);
}

static int32_t KYTY_SYSV_ABI TrySyncDecodeInput(Videodec2Decoder decoder,
                                                InputCompletion* completion) {
	PRINT_NAME();

	if (completion == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	}
	if (completion->this_size != sizeof(*completion)) {
		return ToVdecsw(VIDEODEC2_ERROR_STRUCT_SIZE);
	}
	const auto state = FindDecoder(decoder);
	return state != nullptr ? state->TrySyncInput(completion)
	                        : ToVdecsw(VIDEODEC2_ERROR_DECODER_INSTANCE);
}

static int32_t KYTY_SYSV_ABI TrySyncDecodeOutput(Videodec2Decoder decoder, OutputInfo* output) {
	PRINT_NAME();

	if (output == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	}
	if (output->this_size != sizeof(*output)) {
		return ToVdecsw(VIDEODEC2_ERROR_STRUCT_SIZE);
	}
	const auto state = FindDecoder(decoder);
	return state != nullptr ? state->TrySyncOutput(output)
	                        : ToVdecsw(VIDEODEC2_ERROR_DECODER_INSTANCE);
}

static int32_t KYTY_SYSV_ABI FinalizeDecodeSequence(Videodec2Decoder decoder) {
	PRINT_NAME();

	const auto state = FindDecoder(decoder);
	return state != nullptr ? state->Finalize() : ToVdecsw(VIDEODEC2_ERROR_DECODER_INSTANCE);
}

static int32_t KYTY_SYSV_ABI GetAvcPictureInfo(const OutputInfo* output, void* first_picture_info,
                                               void* second_picture_info) {
	PRINT_NAME();

	if (output == nullptr) {
		return ToVdecsw(VIDEODEC2_ERROR_ARGUMENT_POINTER);
	}
	if (output->this_size != sizeof(*output)) {
		return ToVdecsw(VIDEODEC2_ERROR_STRUCT_SIZE);
	}
	if (!output->is_valid || output->picture_count == 0) {
		return ToVdecsw(VIDEODEC2_ERROR_OUTPUT_INFO);
	}
	return ToVdecsw(WritePictureInfo(output->frame_buffer, output->codec_type, first_picture_info,
	                                 second_picture_info));
}

LIB_DEFINE(InitVdecsw_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("0moTubWCsTM", QueryComputeMemoryInfo);
	LIB_FUNC("hIgrg5h4V6s", AllocateComputeQueue);
	LIB_FUNC("fX-zOOefbbs", ReleaseComputeQueue);
	LIB_FUNC("A+2M7EivuOU", QueryDecoderMemoryInfo);
	LIB_FUNC("+L5ArV1tPGA", CreateDecoder);
	LIB_FUNC("ecUtPX+dBYk", DeleteDecoder);
	LIB_FUNC("veb-YBrOqo0", ResetDecoder);
	LIB_FUNC("aqMiF0AgUYI", SetDecodeInput);
	LIB_FUNC("rgtMCOpyBSc", SetDecodeOutput);
	LIB_FUNC("l4sQYy5wPkc", TrySyncDecodeInput);
	LIB_FUNC("kMBw37oH8nI", TrySyncDecodeOutput);
	LIB_FUNC("5Y6nZqIZvBg", FinalizeDecodeSequence);
	LIB_FUNC("ihNT-uuEAr4", GetAvcPictureInfo);
}

} // namespace Vdecsw

} // namespace Libs
