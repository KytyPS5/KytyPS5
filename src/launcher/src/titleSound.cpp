#include "titleSound.h"

#include "gameContent.h"
#include "libs/ajm/atrac9_decoder.h"

#include <QDebug>
#include <QtConcurrentRun>

#include <SDL3/SDL.h>
#include <array>

TitleSound::Sound TitleSound::Decode(const QByteArray& data, const QPromise<Sound>& promise) {
	using namespace Libs::Audio::Ajm;
	AjmAt9Decoder                decoder(0, 0, AjmSampleEncoding::Float,
	                                     AJM_INSTANCE_FLAG_DEC_AT9_PARSE_RIFF_HEADER);
	AjmGaplessState              gapless;
	std::array<float, 16 * 1024> batch;
	Sound                        sound;
	size_t                       offset = 0;
	while (offset < static_cast<size_t>(data.size()) && !gapless.IsEnd()) {
		if (promise.isCanceled()) {
			return {};
		}
		const auto result = decoder.Decode(data.constData() + offset, data.size() - offset,
		                                   batch.data(), sizeof(batch), true, &gapless);
		if ((result.result != 0 && result.result != AJM_RESULT_NOT_ENOUGH_ROOM) ||
		    result.input_consumed == 0) {
			return {};
		}
		const auto       samples    = result.output_written / sizeof(float);
		constexpr size_t MaxSamples = (128u << 20u) / sizeof(float);
		if (samples > MaxSamples - sound.pcm.size()) {
			return {};
		}
		sound.pcm.insert(sound.pcm.end(), batch.begin(), batch.begin() + samples);
		offset += result.input_consumed;
	}
	if (promise.isCanceled()) {
		return {};
	}
	const auto format = decoder.GetFormat();
	sound.channels    = static_cast<int>(format.channel_num);
	sound.rate        = static_cast<int>(format.sampling_frequency);
	return sound;
}

TitleSound::TitleSound(QObject* parent): QObject(parent) {
	m_decode_pool.setMaxThreadCount(1);
	connect(&m_decode, &QFutureWatcher<Sound>::finished, this, [this] {
		if (m_decode.isCanceled() || m_decode.future().resultCount() == 0) {
			return;
		}
		auto sound = m_decode.future().takeResult();
		if (sound.pcm.empty()) {
			qWarning() << "Cannot decode title sound:" << m_base;
			return;
		}
		m_audio_initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
		if (m_audio_initialized) {
			const SDL_AudioSpec spec {SDL_AUDIO_F32, sound.channels, sound.rate};
			m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr,
			                                     nullptr);
		}
		if (m_stream == nullptr ||
		    !SDL_PutAudioStreamData(m_stream, sound.pcm.data(),
		                            static_cast<int>(sound.pcm.size() * sizeof(float))) ||
		    !SDL_FlushAudioStream(m_stream) || !SDL_ResumeAudioStreamDevice(m_stream)) {
			qWarning() << "Cannot play title sound:" << SDL_GetError();
			Stop();
		}
	});
}

TitleSound::~TitleSound() {
	m_decode.cancel();
	Stop();
}

void TitleSound::SetTitle(const QString& base) {
	if (base == m_base) {
		return;
	}
	m_decode.cancel();
	Stop();
	m_base = base;
	if (!base.isEmpty()) {
		m_decode.setFuture(QtConcurrent::run(&m_decode_pool, [base](QPromise<Sound>& promise) {
			const auto data =
			    GameContent::ReadFile(base, QStringLiteral("sce_sys/snd0.at9"), 32u << 20u);
			if (!data.isEmpty() && !promise.isCanceled()) {
				promise.addResult(Decode(data, promise));
			}
		}));
	}
}

void TitleSound::Stop() {
	if (m_stream != nullptr) {
		SDL_DestroyAudioStream(m_stream);
		m_stream = nullptr;
	}
	if (m_audio_initialized) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		m_audio_initialized = false;
	}
}
