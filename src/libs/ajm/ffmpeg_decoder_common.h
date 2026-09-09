#pragma once

#include "common/assert.h"

#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace Libs::Audio::Ajm {

static AVSampleFormat AjmSampleEncodingToAvFormat(AjmSampleEncoding encoding) {
	switch (encoding) {
		case AjmSampleEncoding::S16: return AV_SAMPLE_FMT_S16;
		case AjmSampleEncoding::S32: return AV_SAMPLE_FMT_S32;
		case AjmSampleEncoding::Float: return AV_SAMPLE_FMT_FLT;
		default: EXIT("unsupported AJM PCM sample encoding %u\n", static_cast<uint32_t>(encoding));
	}
	return AV_SAMPLE_FMT_S16;
}

static AVFrame* AjmConvertFfmpegFrame(const AVFrame* frame, AjmSampleEncoding encoding,
                                      AjmDecodeResult* result) {
	if (frame == nullptr || result == nullptr) {
		return nullptr;
	}

	AVFrame* converted = av_frame_alloc();
	if (converted == nullptr) {
		result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
		return nullptr;
	}

	converted->format      = AjmSampleEncodingToAvFormat(encoding);
	converted->sample_rate = frame->sample_rate;
	converted->nb_samples  = frame->nb_samples;
	if (av_channel_layout_copy(&converted->ch_layout, &frame->ch_layout) < 0 ||
	    av_frame_get_buffer(converted, 0) < 0) {
		av_frame_free(&converted);
		result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_FATAL;
		return nullptr;
	}

	SwrContext* swr        = nullptr;
	int         swr_result = swr_alloc_set_opts2(
	    &swr, &converted->ch_layout, static_cast<AVSampleFormat>(converted->format),
	    converted->sample_rate, &frame->ch_layout, static_cast<AVSampleFormat>(frame->format),
	    frame->sample_rate, 0, nullptr);
	if (swr_result < 0 || swr_init(swr) < 0 || swr_convert_frame(swr, converted, frame) < 0) {
		swr_free(&swr);
		av_frame_free(&converted);
		result->result = AJM_RESULT_CODEC_ERROR | AJM_RESULT_INVALID_DATA;
		return nullptr;
	}
	swr_free(&swr);

	return converted;
}

} // namespace Libs::Audio::Ajm
