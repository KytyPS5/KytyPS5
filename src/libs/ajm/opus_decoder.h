#pragma once

#include "libs/ajm/decoder.h"
#include "libs/ajm/ffmpeg_decoder_common.h"

#include <algorithm>
#include <cstring>

namespace Libs::Audio::Ajm {

struct AjmDecOpusInitializeParameters {
	uint32_t channel_num;
	uint32_t sample_rate;
	uint32_t mapping_family;
};

struct AjmSidebandDecOpusCodecInfo {
	uint32_t frames_per_packet;
};

static_assert(sizeof(AjmDecOpusInitializeParameters) == 12);
static_assert(sizeof(AjmSidebandDecOpusCodecInfo) == 4);

class AjmOpusDecoder final: public AjmDecoder {
public:
	AjmOpusDecoder(uint32_t max_channels, AjmSampleEncoding encoding)
	    : AjmDecoder(0, 48000, encoding), m_max_channels(max_channels) {}

	~AjmOpusDecoder() override { avcodec_free_context(&m_context); avcodec_free_context(&m_alt); }

	AjmDecodeResult Initialize(const void* parameters, size_t parameters_size) override {
		avcodec_free_context(&m_context);
		avcodec_free_context(&m_alt);
		Reset();
		auto result = MakeResult();
		// PS5 AJM Opus init parameter block is not the PS4 layout the original code assumed
		// (every init returned INVALID_PARAMETER|FATAL). Channels come from the instance flags;
		// accept any block and only honour plausible values from it.
		uint32_t channels = m_max_channels != 0 ? m_max_channels : 2;
		if (parameters != nullptr && parameters_size >= 4) {
			uint32_t first = 0;
			std::memcpy(&first, parameters, 4);
			if (first >= 1 && first <= 2) {
				channels = first;
			}
		}
		if (channels > 2) {
			channels = 2;
		}
		struct {
			uint32_t channel_num;
		} params {channels};

		const auto* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
		m_context         = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
		if (m_context != nullptr) {
			av_channel_layout_default(&m_context->ch_layout, static_cast<int>(params.channel_num));
			m_context->sample_rate = 48000;
			if (avcodec_open2(m_context, codec, nullptr) < 0) {
				avcodec_free_context(&m_context);
			}
		}
		if (m_context == nullptr) {
			result.result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return result;
		}
		SetFormat(params.channel_num, 48000, m_sample_encoding);
		return MakeResult();
	}

	void Reset() override {
		if (m_context != nullptr) {
			avcodec_flush_buffers(m_context);
			if (m_alt != nullptr) avcodec_flush_buffers(m_alt);
		}
		m_total_decoded_samples = 0;
		m_frames_per_packet     = 0;
	}

	AjmDecodeResult Decode(const void* input, size_t input_size, void* output, size_t output_size,
	                       bool multiple_frames, AjmGaplessState* gapless) override {
		auto result = MakeResult();
		if (m_context == nullptr) {
			result.result = AJM_RESULT_NOT_INITIALIZED;
			return result;
		}
		if (input == nullptr || input_size == 0) {
			result.result = AJM_RESULT_PARTIAL_INPUT;
			return result;
		}

		const Framing fmt = DetectFraming(static_cast<const uint8_t*>(input), input_size);
		while (gapless == nullptr || !gapless->IsEnd()) {
			const auto* data      = static_cast<const uint8_t*>(input) + result.input_consumed;
			const auto  remaining = input_size - result.input_consumed;
			if (result.input_consumed != 0 && remaining == 0) {
				break;
			}
			if (remaining < 2 && fmt != Framing::Raw) {
				result.result = AJM_RESULT_PARTIAL_INPUT;
				break;
			}
			uint32_t packet_size = 0;
			uint32_t header      = 0;
			if (!ParseHeader(fmt, data, remaining, &header, &packet_size)) {
				if (fmt == Framing::Raw || result.input_consumed != 0) {
					result.result = AJM_RESULT_PARTIAL_INPUT;
					break;
				}
				header      = 0;
				packet_size = static_cast<uint32_t>(remaining);
			}
			data += header;
			if (packet_size == 0 || ((data[0] & 3u) == 3 && packet_size < 2)) {
				result.result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				result.internal_result = 0x1043;
				break;
			}
			const uint32_t config        = data[0] >> 3u;
			const uint32_t frames        = (data[0] & 3u) == 0   ? 1
			                               : (data[0] & 3u) == 3 ? data[1] & 0x3fu
			                                                     : 2;
			const uint32_t frame_samples = config >= 16         ? 120u << (config & 3u)
			                               : config >= 12       ? 480u << (config & 1u)
			                               : (config & 3u) == 3 ? 2880u
			                                                    : 480u << (config & 3u);
			const uint32_t samples       = frame_samples * frames;
			if (frames == 0 || frames > 6 || frame_samples > 960) {
				result.result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				result.internal_result = static_cast<int32_t>(0xffff8800u);
				break;
			}
			const auto skip  = gapless != nullptr
			                       ? std::min(samples, uint32_t {gapless->current.skip_samples})
			                       : 0;
			const auto write = gapless != nullptr && gapless->HasSampleLimit()
			                       ? std::min(samples - skip, gapless->current.total_samples)
			                       : samples - skip;
			const auto sample_bytes = m_channels * AjmBytesPerSample(m_sample_encoding);
			const auto output_bytes = write * sample_bytes;
			if (output_bytes > output_size - result.output_written ||
			    (output_bytes != 0 && output == nullptr)) {
				result.result = AJM_RESULT_NOT_ENOUGH_ROOM;
				break;
			}

			AVFrame* frame = DecodePacket(data, packet_size, &result);
			if (frame == nullptr) {
				break;
			}
			if (frame->nb_samples < static_cast<int>(samples) ||
			    frame->ch_layout.nb_channels > 2) {
				av_frame_free(&frame);
				result.result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
				result.internal_result = 0x1043;
				break;
			}
			if (output_bytes != 0) {
				const uint32_t pc  = frame->ch_layout.nb_channels;
				const size_t   bps = AjmBytesPerSample(m_sample_encoding);
				auto*          dst = static_cast<uint8_t*>(output) + result.output_written;
				const uint8_t* src = frame->data[0] + static_cast<size_t>(skip) * pc * bps;
				for (uint32_t i = 0; i < write; i++) {
					const uint8_t* s = src + static_cast<size_t>(i) * pc * bps;
					uint8_t*       d = dst + static_cast<size_t>(i) * m_channels * bps;
					if (pc == m_channels) {
						std::memcpy(d, s, m_channels * bps);
					} else if (pc == 1) {
						for (uint32_t c = 0; c < m_channels; c++) std::memcpy(d + c * bps, s, bps);
					} else {
						// stereo packet into a mono instance: average
						if (m_sample_encoding == AjmSampleEncoding::Float) {
							float a, b; std::memcpy(&a, s, 4); std::memcpy(&b, s + 4, 4);
							a = (a + b) * 0.5f; std::memcpy(d, &a, 4);
						} else if (m_sample_encoding == AjmSampleEncoding::S32) {
							int32_t a, b; std::memcpy(&a, s, 4); std::memcpy(&b, s + 4, 4);
							a = static_cast<int32_t>((static_cast<int64_t>(a) + b) / 2); std::memcpy(d, &a, 4);
						} else {
							int16_t a, b; std::memcpy(&a, s, 2); std::memcpy(&b, s + 2, 2);
							a = static_cast<int16_t>((a + b) / 2); std::memcpy(d, &a, 2);
						}
					}
				}
			}
			av_frame_free(&frame);
			if (gapless != nullptr) {
				gapless->current.skip_samples -= static_cast<uint16_t>(skip);
				gapless->current.skipped_samples = static_cast<uint16_t>(
				    std::min<uint32_t>(std::numeric_limits<uint16_t>::max(),
				                       gapless->current.skipped_samples + samples - write));
				if (gapless->HasSampleLimit()) {
					gapless->current.total_samples -= write;
				}
			}
			result.input_consumed += packet_size + header;
			result.output_written += output_bytes;
			result.frames += frames;
			m_frames_per_packet = frames;
			m_total_decoded_samples += write;
			if (!multiple_frames) {
				break;
			}
		}
		result.frames_per_packet     = m_frames_per_packet;
		result.total_decoded_samples = m_total_decoded_samples;
		return result;
	}

	void WriteCodecInfo(void* output, size_t size, const AjmDecodeResult&) const override {
		if (output != nullptr && size >= sizeof(AjmSidebandDecOpusCodecInfo)) {
			static_cast<AjmSidebandDecOpusCodecInfo*>(output)->frames_per_packet =
			    m_frames_per_packet;
		}
	}

	[[nodiscard]] size_t CodecInfoSize() const override {
		return sizeof(AjmSidebandDecOpusCodecInfo);
	}

private:
	AVFrame* DecodePacket(const uint8_t* data, uint32_t size, AjmDecodeResult* result) {
		AVPacket* packet = av_packet_alloc();
		AVFrame*  frame  = av_frame_alloc();
		if (packet == nullptr || frame == nullptr ||
		    av_new_packet(packet, static_cast<int>(size)) < 0) {
			av_packet_free(&packet);
			av_frame_free(&frame);
			result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
			return nullptr;
		}
		std::memcpy(packet->data, data, size);
		AVCodecContext* ctx = ContextFor((data[0] & 4u) != 0 ? 2u : 1u);
		int rc = ctx != nullptr ? avcodec_send_packet(ctx, packet) : -1;
		av_packet_free(&packet);
		if (rc >= 0) {
			rc = avcodec_receive_frame(ctx, frame);
		}
		if (rc < 0) {
			av_frame_free(&frame);
			result->result          = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
			result->internal_result = 0x1043;
			return nullptr;
		}
		AVFrame* converted = AjmConvertFfmpegFrame(frame, m_sample_encoding, result);
		av_frame_free(&frame);
		return converted;
	}

	AVCodecContext* m_context = nullptr;
	// ffmpeg's Opus decoder mangles mono-coded packets fed to a stereo context (blocky noise);
	// libopus upmixes them. Keep one context per packet channel count and remap on output.
	AVCodecContext* ContextFor(uint32_t channels) {
		if (channels == m_channels) {
			return m_context;
		}
		if (m_alt == nullptr) {
			const auto* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
			m_alt             = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
			if (m_alt != nullptr) {
				av_channel_layout_default(&m_alt->ch_layout, static_cast<int>(channels));
				m_alt->sample_rate = 48000;
				if (avcodec_open2(m_alt, codec, nullptr) < 0) {
					avcodec_free_context(&m_alt);
				}
			}
		}
		return m_alt;
	}

	AVCodecContext* m_alt = nullptr;
	enum class Framing { Raw, Be2, Le2, Be4, Hdr8 };

	// Parse the per-packet header for a framing. Hdr8 = {u32 BE size, u32 BE final_range}.
	static bool ParseHeader(Framing fmt, const uint8_t* p, size_t rem, uint32_t* header,
	                        uint32_t* size) {
		auto be32 = [](const uint8_t* q) {
			return (uint32_t {q[0]} << 24u) | (uint32_t {q[1]} << 16u) | (uint32_t {q[2]} << 8u) |
			       uint32_t {q[3]};
		};
		uint32_t h = 0;
		uint32_t s = 0;
		switch (fmt) {
			case Framing::Raw: h = 0; s = static_cast<uint32_t>(rem); break;
			case Framing::Be2: if (rem < 2) return false; h = 2; s = (uint32_t {p[0]} << 8u) | p[1]; break;
			case Framing::Le2: if (rem < 2) return false; h = 2; s = (uint32_t {p[1]} << 8u) | p[0]; break;
			case Framing::Be4: if (rem < 4) return false; h = 4; s = be32(p); break;
			case Framing::Hdr8: if (rem < 8) return false; h = 8; s = be32(p); break;
		}
		if (s == 0 || s > rem - h) {
			return false;
		}
		*header = h;
		*size   = s;
		return true;
	}

	// A framing is accepted if the packet chain covers the whole input (trailing zeros allowed).
	static bool ChainFits(Framing fmt, const uint8_t* p, size_t total) {
		size_t off = 0;
		while (off < total) {
			uint32_t h = 0;
			uint32_t s = 0;
			if (!ParseHeader(fmt, p + off, total - off, &h, &s)) {
				for (size_t i = off; i < total; i++) {
					if (p[i] != 0) return false;
				}
				return off != 0;
			}
			off += h + s;
		}
		return true;
	}

	Framing DetectFraming(const uint8_t* p, size_t total) {
		static const Framing order[] = {Framing::Hdr8, Framing::Be4, Framing::Be2, Framing::Le2};
		if (m_framing != Framing::Raw && ChainFits(m_framing, p, total)) {
			return m_framing;
		}
		for (auto f: order) {
			if (ChainFits(f, p, total)) {
				m_framing = f;
				return f;
			}
		}
		return Framing::Raw;
	}

	Framing         m_framing = Framing::Raw;
	const uint32_t  m_max_channels;
	uint32_t        m_frames_per_packet = 0;
};

}
