#include "titleSound.h"

#include "gameContent.h"
#include "libs/ajm/atrac9_decoder.h"

#include <QDebug>
#include <QFile>
#include <QtConcurrentRun>

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr int   TickMs      = 20;
constexpr float FadeSeconds = 1.0f;
} // namespace

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
	// Drop NaN/out-of-range samples (they sound like high-pitched clicks).
	for (auto& s : sound.pcm) {
		s = std::isfinite(s) ? std::clamp(s, -1.0f, 1.0f) : 0.0f;
	}
	return sound;
}

TitleSound::TitleSound(QObject* parent): QObject(parent) {
	m_decode_pool.setMaxThreadCount(1);
	connect(&m_decode, &QFutureWatcher<Sound>::finished, this, [this] {
		if (m_decode.isCanceled()) {
			return;
		}
		if (m_decode.future().resultCount() == 0) {
			FadeOthers({});
			return;
		}
		auto sound = std::make_shared<Sound>(m_decode.future().takeResult());
		if (sound->pcm.empty() || sound->channels <= 0 || sound->rate <= 0) {
			qWarning() << "Cannot decode title sound:" << m_base;
			FadeOthers({});
			return;
		}
		if (m_cache.size() >= 3) {
			m_cache.clear();
		}
		m_cache.insert(m_base, sound);
		AddVoice(m_base, sound);
	});
	m_timer.setInterval(TickMs);
	connect(&m_timer, &QTimer::timeout, this, &TitleSound::Tick);
	EnsureAudio();
}

TitleSound::~TitleSound() {
	m_decode.cancel();
	m_decode.waitForFinished();
	Clear();
}

// Queues the looping track in small chunks, so only about a second of audio is copied into the
// stream at a time instead of the whole (possibly very long) decoded track.
void TitleSound::Feed(Voice& v) {
	const auto&  pcm      = v.sound->pcm;
	const size_t frame    = static_cast<size_t>(v.sound->channels);
	const size_t chunk    = std::max<size_t>(static_cast<size_t>(v.sound->rate) / 2, 1) * frame;
	const int    target_b = static_cast<int>(chunk * 2 * sizeof(float));
	while (SDL_GetAudioStreamQueued(v.stream) < target_b) {
		if (v.pos >= pcm.size()) {
			v.pos = 0;
		}
		const size_t n = std::min(chunk, pcm.size() - v.pos);
		if (!SDL_PutAudioStreamData(v.stream, pcm.data() + v.pos, static_cast<int>(n * sizeof(float)))) {
			return;
		}
		v.pos += n;
	}
}

void TitleSound::FadeOthers(const QString& keep) {
	for (auto& v : m_voices) {
		v.target = !keep.isEmpty() && v.base == keep ? 1.0f : 0.0f;
	}
}

void TitleSound::AddVoice(const QString& base, std::shared_ptr<const Sound> sound) {
	if (!EnsureAudio()) {
		FadeOthers({});
		return;
	}
	const SDL_AudioSpec spec {SDL_AUDIO_F32, sound->channels, sound->rate};
	auto* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (stream == nullptr) {
		qWarning() << "Cannot play title sound:" << SDL_GetError();
		FadeOthers({});
		return;
	}
	SDL_SetAudioStreamGain(stream, 0.0f);
	if (m_voices.empty()) {
		// The device was just opened: feed silence first so its startup glitch is not audible.
		const std::vector<float> silence(static_cast<size_t>(sound->rate * sound->channels) / 4, 0.0f);
		SDL_PutAudioStreamData(stream, silence.data(), static_cast<int>(silence.size() * sizeof(float)));
	}
	Voice voice {base, std::move(sound), stream, 0.0f, 1.0f, 0};
	Feed(voice);
	if (!SDL_ResumeAudioStreamDevice(stream)) {
		qWarning() << "Cannot play title sound:" << SDL_GetError();
		SDL_DestroyAudioStream(stream);
		FadeOthers({});
		return;
	}
	FadeOthers(base);
	m_voices.push_back(std::move(voice));
	m_timer.start();
}

// Ramps every voice toward its target gain and keeps the loops queued (the streams are never
// flushed, so queueing the track again before the queue drains loops it without a gap).
void TitleSound::Tick() {
	const float step = static_cast<float>(TickMs) / 1000.0f / FadeSeconds;
	for (auto& v : m_voices) {
		if (v.gain < v.target) {
			v.gain = std::min(v.target, v.gain + step);
		} else if (v.gain > v.target) {
			v.gain = std::max(v.target, v.gain - step);
		}
		// Squared ramp: smoother start/end than a linear one.
		SDL_SetAudioStreamGain(v.stream, v.gain * v.gain);
		Feed(v);
	}
	std::erase_if(m_voices, [](const Voice& v) {
		if (v.target <= 0.0f && v.gain <= 0.0f) {
			SDL_DestroyAudioStream(v.stream);
			return true;
		}
		return false;
	});
	if (m_voices.empty()) {
		m_timer.stop();
	}
}

// The device is opened once (with a silent stream) and kept open, so starting a track never
// has to wake it up.
bool TitleSound::EnsureAudio() {
	if (!m_audio_initialized) {
		m_audio_initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
		if (!m_audio_initialized) {
			qWarning() << "Cannot init audio:" << SDL_GetError();
			return false;
		}
	}
	if (m_keepalive == nullptr) {
		const SDL_AudioSpec spec {SDL_AUDIO_F32, 2, 48000};
		m_keepalive = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (m_keepalive != nullptr) {
			SDL_ResumeAudioStreamDevice(m_keepalive);
		}
	}
	return true;
}

void TitleSound::SetTitle(const QString& base) {
	if (base == m_base) {
		return;
	}
	m_decode.cancel();
	m_base = base;
	const bool have_voice =
	    std::any_of(m_voices.begin(), m_voices.end(), [&](const Voice& v) { return v.base == base; });
	// Old voices keep playing until the replacement is ready, unless there is nothing to play.
	if (base.isEmpty() || have_voice || m_cache.contains(base)) {
		FadeOthers(base);
	}
	if (base.isEmpty() || have_voice) {
		return;
	}
	if (const auto cached = m_cache.value(base)) {
		AddVoice(base, cached);
		return;
	}
	m_decode.setFuture(QtConcurrent::run(&m_decode_pool, [base](QPromise<Sound>& promise) {
		const auto data =
		    GameContent::ReadFile(base, QStringLiteral("sce_sys/snd0.at9"), 32u << 20u);
		if (!data.isEmpty() && !promise.isCanceled()) {
			promise.addResult(Decode(data, promise));
		}
	}));
}

void TitleSound::PlayNavigation() {
	if (!EnsureAudio()) {
		return;
	}
	if (!m_nav_loaded) {
		m_nav_loaded = true;
		QFile file(QStringLiteral(":/sounds/navigation.wav"));
		if (file.open(QIODevice::ReadOnly)) {
			const auto          wav = file.readAll();
			SDL_AudioSpec       spec {};
			Uint8*              buffer = nullptr;
			Uint32              length = 0;
			if (SDL_LoadWAV_IO(SDL_IOFromConstMem(wav.constData(), static_cast<size_t>(wav.size())),
			                   true, &spec, &buffer, &length)) {
				// Converted once to float so every playback uses the same format.
				Uint8* converted     = nullptr;
				int    converted_len = 0;
				const SDL_AudioSpec dst {SDL_AUDIO_F32, spec.channels, spec.freq};
				if (SDL_ConvertAudioSamples(&spec, buffer, static_cast<int>(length), &dst, &converted,
				                            &converted_len)) {
					m_nav_pcm      = QByteArray(reinterpret_cast<const char*>(converted), converted_len);
					m_nav_channels = spec.channels;
					m_nav_rate     = spec.freq;
					SDL_free(converted);
				}
				SDL_free(buffer);
			}
		}
	}
	if (m_nav_pcm.isEmpty()) {
		return;
	}
	for (auto it = m_nav_streams.begin(); it != m_nav_streams.end();) {
		if (SDL_GetAudioStreamQueued(*it) == 0) {
			SDL_DestroyAudioStream(*it);
			it = m_nav_streams.erase(it);
		} else {
			++it;
		}
	}
	const SDL_AudioSpec spec {SDL_AUDIO_F32, m_nav_channels, m_nav_rate};
	auto* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (stream == nullptr) {
		return;
	}
	if (SDL_PutAudioStreamData(stream, m_nav_pcm.constData(), m_nav_pcm.size()) &&
	    SDL_ResumeAudioStreamDevice(stream)) {
		m_nav_streams.push_back(stream);
	} else {
		SDL_DestroyAudioStream(stream);
	}
}

void TitleSound::Clear() {
	m_timer.stop();
	for (auto* s : m_nav_streams) {
		SDL_DestroyAudioStream(s);
	}
	m_nav_streams.clear();
	for (auto& v : m_voices) {
		SDL_DestroyAudioStream(v.stream);
	}
	m_voices.clear();
	if (m_keepalive != nullptr) {
		SDL_DestroyAudioStream(m_keepalive);
		m_keepalive = nullptr;
	}
	if (m_audio_initialized) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		m_audio_initialized = false;
	}
}
