#pragma once

#include "common/logging/log.h"
#include "libs/ajm/ffmpeg_decoder_common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Libs::Audio::Ajm {

struct AjmSidebandDecMp3CodecInfo {
	uint32_t header;
	uint8_t  crc;
	uint8_t  mode;
	uint8_t  mode_extension;
	uint8_t  copyright;
	uint8_t  original;
	uint8_t  emphasis;
	uint16_t reserved[3];
};

static_assert(sizeof(AjmSidebandDecMp3CodecInfo) == 16);

struct AjmMp3FrameHeader {
	uint32_t header              = 0;
	uint32_t frame_size          = 0;
	uint32_t num_channels        = 0;
	uint32_t samples_per_channel = 0;
	uint32_t bitrate             = 0;
	uint32_t sample_rate         = 0;
	uint8_t  crc                 = 0;
	uint8_t  mode                = 0;
	uint8_t  mode_extension      = 0;
	uint8_t  copyright           = 0;
	uint8_t  original            = 0;
	uint8_t  emphasis            = 0;
};

static uint32_t AjmReadBe32(const uint8_t* data) {
	return (static_cast<uint32_t>(data[0]) << 24u) | (static_cast<uint32_t>(data[1]) << 16u) |
	       (static_cast<uint32_t>(data[2]) << 8u) | static_cast<uint32_t>(data[3]);
}

static bool AjmParseMp3Header(const uint8_t* data, size_t size, AjmMp3FrameHeader* out) {
	if (data == nullptr || size < 4 || out == nullptr) {
		return false;
	}

	const uint32_t header = AjmReadBe32(data);
	if ((header & 0xffe00000u) != 0xffe00000u) {
		return false;
	}

	const auto version_id     = (header >> 19u) & 0x3u;
	const auto layer_id       = (header >> 17u) & 0x3u;
	const auto protection_bit = (header >> 16u) & 0x1u;
	const auto bitrate_index  = (header >> 12u) & 0xfu;
	const auto sample_index   = (header >> 10u) & 0x3u;
	const auto padding        = (header >> 9u) & 0x1u;
	const auto channel_mode   = (header >> 6u) & 0x3u;
	const auto mode_extension = (header >> 4u) & 0x3u;
	const auto copyright      = (header >> 3u) & 0x1u;
	const auto original       = (header >> 2u) & 0x1u;
	const auto emphasis       = header & 0x3u;

	if (version_id == 1 || layer_id != 1 || sample_index == 3 || bitrate_index == 0 ||
	    bitrate_index == 15) {
		return false;
	}

	static constexpr uint32_t SAMPLE_RATES[4][3] = {
	    {11025, 12000, 8000},
	    {0, 0, 0},
	    {22050, 24000, 16000},
	    {44100, 48000, 32000},
	};
	static constexpr uint32_t BITRATES_MPEG1[16] = {
	    0,      32000,  40000,  48000,  56000,  64000,  80000,  96000,
	    112000, 128000, 160000, 192000, 224000, 256000, 320000, 0,
	};
	static constexpr uint32_t BITRATES_MPEG2[16] = {
	    0,     8000,  16000, 24000,  32000,  40000,  48000,  56000,
	    64000, 80000, 96000, 112000, 128000, 144000, 160000, 0,
	};

	const auto sample_rate = SAMPLE_RATES[version_id][sample_index];
	const auto bitrate =
	    (version_id == 3 ? BITRATES_MPEG1[bitrate_index] : BITRATES_MPEG2[bitrate_index]);
	if (sample_rate == 0 || bitrate == 0) {
		return false;
	}

	const bool mpeg1 = (version_id == 3);

	out->header = header;
	out->frame_size =
	    (mpeg1 ? (144u * bitrate) / sample_rate : (72u * bitrate) / sample_rate) + padding;
	out->num_channels        = (channel_mode == 3 ? 1u : 2u);
	out->samples_per_channel = (mpeg1 ? 1152u : 576u);
	out->bitrate             = bitrate;
	out->sample_rate         = sample_rate;
	out->crc                 = static_cast<uint8_t>(protection_bit);
	out->mode                = static_cast<uint8_t>(channel_mode);
	out->mode_extension      = static_cast<uint8_t>(mode_extension);
	out->copyright           = static_cast<uint8_t>(copyright);
	out->original            = static_cast<uint8_t>(original);
	out->emphasis            = static_cast<uint8_t>(emphasis);
	return true;
}

class AjmMp3Decoder final: public AjmDecoder {
public:
	AjmMp3Decoder(uint32_t channels, uint32_t sample_rate, AjmSampleEncoding encoding,
	              uint64_t flags)
	    : AjmDecoder(channels, sample_rate, encoding) {
		(void)flags;

		m_codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
		if (m_codec == nullptr) {
			LOGF("AJM MP3: avcodec_find_decoder failed\n");
			return;
		}

		m_codec_context = avcodec_alloc_context3(m_codec);
		m_parser        = av_parser_init(m_codec->id);
		if (m_codec_context == nullptr || m_parser == nullptr ||
		    avcodec_open2(m_codec_context, m_codec, nullptr) < 0) {
			LOGF("AJM MP3: decoder initialization failed\n");
			Release();
		}
	}

	~AjmMp3Decoder() override { Release(); }

	void Reset() override {
		if (m_codec_context != nullptr) {
			avcodec_flush_buffers(m_codec_context);
		}
		if (m_parser != nullptr) {
			av_parser_close(m_parser);
			m_parser = (m_codec != nullptr ? av_parser_init(m_codec->id) : nullptr);
		}
		m_total_decoded_samples = 0;
		m_have_header           = false;
		m_bitrate               = 0;
	}

	AjmDecodeResult Decode(const void* input, size_t input_size, void* output, size_t output_size,
	                       bool multiple_frames, AjmGaplessState* gapless) override {
		auto result = MakeResult();

		if (m_codec_context == nullptr || m_parser == nullptr) {
			result.result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return result;
		}
		if (input == nullptr || input_size == 0) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
			return result;
		}
		if (input_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
			result.result = AJM_RESULT_INVALID_PARAMETER;
			return result;
		}

		const auto* input_bytes   = static_cast<const uint8_t*>(input);
		size_t      input_offset  = 0;
		size_t      output_offset = 0;
		bool        decoded       = false;

		while (input_offset < input_size) {
			AjmMp3FrameHeader header {};
			if (AjmParseMp3Header(input_bytes + input_offset, input_size - input_offset, &header)) {
				m_last_header = header;
				m_have_header = true;
			}

			uint8_t*  packet_data = nullptr;
			int       packet_size = 0;
			const int consumed    = av_parser_parse2(
			    m_parser, m_codec_context, &packet_data, &packet_size, input_bytes + input_offset,
			    static_cast<int>(input_size - input_offset), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
			if (consumed < 0) {
				result.result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				break;
			}

			input_offset += static_cast<size_t>(consumed);
			if (packet_size > 0) {
				if (!DecodePacket(packet_data, packet_size, output, output_size, &output_offset,
				                  gapless, &result)) {
					break;
				}
				decoded = true;
				if (!multiple_frames) {
					break;
				}
			}

			if (consumed == 0) {
				break;
			}
		}

		if (!decoded && result.result == OK) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
		}

		result.input_consumed        = input_offset;
		result.output_written        = output_offset;
		result.total_decoded_samples = m_total_decoded_samples;
		result.format                = GetFormat();
		return result;
	}

	void WriteCodecInfo(void* output, size_t output_size,
	                    const AjmDecodeResult& result) const override {
		(void)result;
		if (output == nullptr || output_size < sizeof(AjmSidebandDecMp3CodecInfo) ||
		    !m_have_header) {
			return;
		}

		auto* info           = static_cast<AjmSidebandDecMp3CodecInfo*>(output);
		info->header         = m_last_header.header;
		info->crc            = m_last_header.crc;
		info->mode           = m_last_header.mode;
		info->mode_extension = m_last_header.mode_extension;
		info->copyright      = m_last_header.copyright;
		info->original       = m_last_header.original;
		info->emphasis       = m_last_header.emphasis;
	}

	[[nodiscard]] size_t CodecInfoSize() const override {
		return sizeof(AjmSidebandDecMp3CodecInfo);
	}

	[[nodiscard]] AjmSidebandFormat GetFormat() const override {
		auto format    = AjmMakeFormat(m_channels, m_sample_rate, m_sample_encoding);
		format.bitrate = m_bitrate;
		return format;
	}

private:
	void Release() {
		if (m_parser != nullptr) {
			av_parser_close(m_parser);
			m_parser = nullptr;
		}
		if (m_codec_context != nullptr) {
			avcodec_free_context(&m_codec_context);
		}
	}

	bool WriteFrame(const AVFrame* frame, void* output, size_t output_size, size_t* output_offset,
	                AjmGaplessState* gapless, AjmDecodeResult* result) {
		if (frame == nullptr || output_offset == nullptr || result == nullptr) {
			return false;
		}

		const auto channels = static_cast<uint32_t>(std::max(frame->ch_layout.nb_channels, 1));
		const auto bpf      = channels * AjmBytesPerSample(m_sample_encoding);
		if (bpf == 0) {
			result->result = AJM_RESULT_INVALID_PARAMETER;
			return false;
		}

		SetFormat(channels, static_cast<uint32_t>(std::max(frame->sample_rate, 1)),
		          m_sample_encoding);
		if (m_codec_context != nullptr && m_codec_context->bit_rate > 0) {
			m_bitrate = static_cast<uint32_t>(m_codec_context->bit_rate);
		} else if (m_have_header) {
			m_bitrate = m_last_header.bitrate;
		}

		uint32_t skip_samples = 0;
		if (gapless != nullptr && gapless->current.skip_samples > 0) {
			skip_samples = std::min<uint32_t>(frame->nb_samples, gapless->current.skip_samples);
			gapless->current.skip_samples =
			    static_cast<uint16_t>(gapless->current.skip_samples - skip_samples);
		}

		uint32_t samples_to_write = static_cast<uint32_t>(frame->nb_samples) - skip_samples;
		if (gapless != nullptr && gapless->HasSampleLimit()) {
			samples_to_write = std::min(samples_to_write, gapless->current.total_samples);
		}

		const auto requested_bytes = static_cast<size_t>(samples_to_write) * bpf;
		auto       writable_bytes  = requested_bytes;
		if (output == nullptr || *output_offset >= output_size) {
			writable_bytes = 0;
		} else {
			writable_bytes = std::min(writable_bytes, output_size - *output_offset);
			writable_bytes -= writable_bytes % bpf;
		}

		if (writable_bytes != 0) {
			auto*       dst = static_cast<uint8_t*>(output) + *output_offset;
			const auto* src = frame->data[0] + static_cast<size_t>(skip_samples) * bpf;
			std::memcpy(dst, src, writable_bytes);
			*output_offset += writable_bytes;
		}

		const auto samples_written = static_cast<uint32_t>(writable_bytes / bpf);
		if (gapless != nullptr && gapless->HasSampleLimit()) {
			gapless->current.total_samples -=
			    std::min(gapless->current.total_samples, samples_written);
		}
		if (gapless != nullptr) {
			const auto skipped_total =
			    std::min<uint32_t>(std::numeric_limits<uint16_t>::max(),
			                       static_cast<uint32_t>(gapless->current.skipped_samples) +
			                           skip_samples + (samples_to_write - samples_written));
			gapless->current.skipped_samples = static_cast<uint16_t>(skipped_total);
		}

		m_total_decoded_samples += samples_written;
		result->frames++;
		result->total_decoded_samples = m_total_decoded_samples;
		result->format                = GetFormat();

		if (writable_bytes != requested_bytes) {
			result->result |= AJM_RESULT_NOT_ENOUGH_ROOM;
			return false;
		}
		return true;
	}

	bool DecodePacket(const uint8_t* packet_data, int packet_size, void* output, size_t output_size,
	                  size_t* output_offset, AjmGaplessState* gapless, AjmDecodeResult* result) {
		if (packet_data == nullptr || packet_size <= 0 || result == nullptr) {
			return true;
		}

		AVPacket* packet = av_packet_alloc();
		if (packet == nullptr || av_new_packet(packet, packet_size) < 0) {
			av_packet_free(&packet);
			result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return false;
		}
		std::memcpy(packet->data, packet_data, static_cast<size_t>(packet_size));

		int rc = avcodec_send_packet(m_codec_context, packet);
		av_packet_free(&packet);
		if (rc < 0) {
			result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
			return false;
		}

		for (;;) {
			AVFrame* frame = av_frame_alloc();
			if (frame == nullptr) {
				result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
				return false;
			}

			rc = avcodec_receive_frame(m_codec_context, frame);
			if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
				av_frame_free(&frame);
				return true;
			}
			if (rc < 0) {
				av_frame_free(&frame);
				result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				return false;
			}

			AVFrame* converted = AjmConvertFfmpegFrame(frame, m_sample_encoding, result);
			av_frame_free(&frame);
			if (converted == nullptr) {
				return false;
			}

			const bool ok =
			    WriteFrame(converted, output, output_size, output_offset, gapless, result);
			av_frame_free(&converted);
			if (!ok) {
				return false;
			}
		}
	}

	const AVCodec*        m_codec         = nullptr;
	AVCodecContext*       m_codec_context = nullptr;
	AVCodecParserContext* m_parser        = nullptr;
	bool                  m_have_header   = false;
	uint32_t              m_bitrate       = 0;
	AjmMp3FrameHeader     m_last_header;
};

} // namespace Libs::Audio::Ajm
