#include "libs/spatialRenderer.h"

#include "libs/audio_internal.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>

namespace Libs::Audio {

bool AudioOut2::AudioOut2Position::IsFinite() const {
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

double AudioOut2::AudioOut2Position::Length() const {
	return std::hypot(double(x), double(y), double(z));
}

AudioOut2::AudioOut2Position AudioOut2::AudioOut2Position::operator/(double divisor) const {
	return {float(x / divisor), float(y / divisor), float(z / divisor)};
}

bool SpatialAttributes::SetPosition(const void* value, size_t size) {
	if (!value || size != sizeof(position)) return false;
	std::memcpy(&position, value, size);
	return position.IsFinite();
}

bool SpatialAttributes::SetSpread(const void* value, size_t size) {
	float angle;
	if (!value || size != sizeof(angle)) return false;
	std::memcpy(&angle, value, sizeof(angle));
	if (!std::isfinite(angle) || angle < 0) return false;
	spread = std::min(angle, 2 * std::numbers::pi_v<float>);
	return true;
}

bool SpatialAttributes::SetAmbisonics(const void* value, size_t size, bool allow_acn) {
	if (!value || size != sizeof(ambisonics)) return false;
	std::memcpy(&ambisonics, value, size);
	return ambisonics == UINT32_MAX || ambisonics < 16 ||
	       (allow_acn && ambisonics >= 64 && ambisonics < 100);
}

struct SpatialRenderer::Impl {
	struct Voice {
		ALuint source = 0, buffer = 0;
		int    order = 0;
	};
	ALCdevice*                        device  = nullptr;
	ALCcontext*                       context = nullptr;
	uint32_t                          frames = 0, channels = 0;
	std::vector<Voice>                voices, retiring;
	std::array<Voice, 2>              bformat;
	std::array<std::vector<float>, 2> beds;
	std::vector<float>                mono, silence;
	bool                              draining = false;

	void Release(Voice& voice) {
		if (voice.source) alDeleteSourcesDirect(context, 1, &voice.source);
		if (voice.buffer) alDeleteBuffersDirect(context, 1, &voice.buffer);
		voice = {};
	}

	void Update(Voice& voice, std::span<const float> samples, int order, bool acn,
	            SpatialAttributes attributes = {}) {
		if (voice.source && (samples.empty() || order != voice.order)) {
			// OpenAL fades a stopped voice. Give it silence instead of another turn
			// of the last grain, while letting its convolution history decay.
			silence.resize(size_t(frames) * (voice.order + 1) * (voice.order + 1));
			alBufferSubDataDirectSOFT(
			    context, voice.buffer,
			    voice.order ? AL_FORMAT_BFORMAT3D_FLOAT32 : AL_FORMAT_MONO_FLOAT32, silence.data(),
			    0, static_cast<ALsizei>(silence.size() * sizeof(float)));
			retiring.push_back(std::exchange(voice, {}));
		}
		if (samples.empty()) return;
		const auto format = order ? AL_FORMAT_BFORMAT3D_FLOAT32 : AL_FORMAT_MONO_FLOAT32;
		const auto bytes  = static_cast<ALsizei>(samples.size_bytes());
		const bool start  = !voice.source;
		if (start) {
			alGenSourcesDirect(context, 1, &voice.source);
			if (!voice.source) return;
			alGenBuffersDirect(context, 1, &voice.buffer);
			if (!voice.buffer) {
				Release(voice);
				return;
			}
			if (order) {
				alBufferiDirect(context, voice.buffer, AL_AMBISONIC_LAYOUT_SOFT,
				                acn ? AL_ACN_SOFT : AL_FUMA_SOFT);
				alBufferiDirect(context, voice.buffer, AL_AMBISONIC_SCALING_SOFT,
				                acn ? AL_SN3D_SOFT : AL_FUMA_SOFT);
				alBufferiDirect(context, voice.buffer, AL_UNPACK_AMBISONIC_ORDER_SOFT, order);
			}
			alBufferDataDirect(context, voice.buffer, format, samples.data(), bytes, 48000);
			alSourceiDirect(context, voice.source, AL_BUFFER, voice.buffer);
			alSourceiDirect(context, voice.source, AL_LOOPING, AL_TRUE);
			voice.order = order;
		} else {
			// Only alcRenderSamplesSOFT advances the mixer. Exactly one turn of this
			// buffer has elapsed, so replace it in place and retain the voice's filters.
			alBufferSubDataDirectSOFT(context, voice.buffer, format, samples.data(), 0, bytes);
		}
		if (!order) {
			auto dir = attributes.position;
			if (const double length = dir.Length(); length > 0) dir = dir / length;
			// Invert OpenAL Soft's apparent angle of a sphere at unit distance.
			constexpr float pi    = std::numbers::pi_v<float>;
			const float     angle = attributes.spread;
			const float     radius =
			    angle <= pi ? std::sin(angle * 0.5f) : pi / std::max(2 * pi - angle, 1e-6f);
			alSource3fDirect(context, voice.source, AL_POSITION, dir.x, dir.y, -dir.z);
			alSourcefDirect(context, voice.source, AL_SOURCE_RADIUS, radius);
		}
		if (start) alSourcePlayDirect(context, voice.source);
	}
};

SpatialRenderer::SpatialRenderer(): m(std::make_unique<Impl>()) {}

SpatialRenderer::~SpatialRenderer() {
	if (m_handle) AudioInternal::AudioOutClose(m_handle);
	if (m->context) {
		for (auto& voice: m->voices)
			m->Release(voice);
		for (auto& voice: m->bformat)
			m->Release(voice);
		alcDestroyContext(m->context);
	}
	if (m->device) alcCloseDevice(m->device);
}

std::unique_ptr<SpatialRenderer> SpatialRenderer::Open(uint32_t frames, uint32_t max_objects) {
	if (!frames || frames > std::numeric_limits<ALsizei>::max() / (36 * sizeof(float))) return {};
	auto  renderer = std::unique_ptr<SpatialRenderer>(new SpatialRenderer);
	auto& m        = *renderer->m;
	m.device       = alcLoopbackOpenDeviceSOFT(nullptr);
	if (!m.device) return {};
	m.channels    = AudioInternal::GetMainOutput().channels;
	ALCint layout = ALC_STEREO_SOFT;
	auto   format = AudioInternal::Format::FloatStereo;
	switch (m.channels) {
		case 6:
			layout = ALC_5POINT1_SOFT;
			format = AudioInternal::Format::Float6Ch;
			break;
		case 8:
			layout = ALC_7POINT1_SOFT;
			format = AudioInternal::Format::Float8ChStd;
			break;
	}
	const ALCint attrs[] = {ALC_FORMAT_CHANNELS_SOFT,
	                        layout,
	                        ALC_FORMAT_TYPE_SOFT,
	                        ALC_FLOAT_SOFT,
	                        ALC_FREQUENCY,
	                        48000,
	                        ALC_MONO_SOURCES,
	                        static_cast<int>(max_objects),
	                        ALC_STEREO_SOURCES,
	                        4, // Two B-format conventions, with a retiring buffer on order changes.
	                        ALC_OUTPUT_MODE_SOFT,
	                        Config::GetAudioOutput() == Config::AudioOutput::Headphones
	                            ? ALC_STEREO_HRTF_SOFT
	                            : ALC_STEREO_BASIC_SOFT,
	                        ALC_OUTPUT_LIMITER_SOFT,
	                        ALC_FALSE,
	                        0};
	m.context            = alcCreateContext(m.device, attrs);
	if (!m.context) return {};
	alDistanceModelDirect(m.context, AL_NONE);
	m.frames = frames;
	m.voices.resize(max_objects);
	renderer->m_handle =
	    AudioInternal::AudioOutOpen(AUDIO_OUT_PORT_TYPE_MAIN, frames, 48000, format);
	if (!renderer->m_handle) return {};
	return renderer;
}

bool SpatialRenderer::Render(std::span<const SpatialObject> objects, std::vector<float>& output,
                             bool include_silence) {
	std::array<int, 2> orders {};
	bool               fed = false;
	for (const auto& obj: objects) {
		if (obj.pcm.empty()) continue;
		fed           = true;
		const auto ch = obj.attributes.ambisonics;
		if (ch != UINT32_MAX) {
			const bool acn   = ch >= 64;
			const int  order = static_cast<int>(std::sqrt(acn ? ch - 64 : ch));
			orders[acn]      = std::max({orders[acn], order, 1});
		}
	}
	output.clear();
	if (!include_silence && !fed && !m->draining) return true;
	for (size_t i = 0; i < orders.size(); ++i) {
		const auto n = orders[i] + 1;
		m->beds[i].assign(orders[i] ? size_t(m->frames) * n * n : 0, 0);
	}
	for (size_t i = 0; i < objects.size(); ++i) {
		const auto& obj   = objects[i];
		const auto  ch    = obj.attributes.ambisonics;
		const bool  point = ch == UINT32_MAX && !obj.pcm.empty();
		if (!point) m->Update(m->voices[i], {}, 0, false);
		if (obj.pcm.empty()) continue;
		const auto* f32 = reinterpret_cast<const float*>(obj.pcm.data());
		if (point && obj.is_float && obj.gain == 1) {
			m->Update(m->voices[i], {f32, m->frames}, 0, false, obj.attributes);
			continue;
		}
		const bool acn = ch >= 64;
		if (point) m->mono.assign(m->frames, 0);
		auto&        dst    = point ? m->mono : m->beds[acn];
		const size_t stride = dst.size() / m->frames;
		const auto   offset = point ? 0u : (acn ? ch - 64 : ch);
		const auto*  s16    = reinterpret_cast<const int16_t*>(obj.pcm.data());
		for (uint32_t j = 0; j < m->frames; ++j) {
			const float sample = (obj.is_float ? f32[j] : s16[j] / 32768.0f) * obj.gain;
			dst[j * stride + offset] += sample;
		}
		if (point) m->Update(m->voices[i], dst, 0, false, obj.attributes);
	}
	for (size_t i = 0; i < orders.size(); ++i) {
		m->Update(m->bformat[i], m->beds[i], orders[i], i != 0);
	}
	output.resize(size_t(m->frames) * m->channels);
	alcRenderSamplesSOFT(m->device, output.data(), m->frames);
	for (auto& voice: m->retiring)
		m->Release(voice);
	m->retiring.clear();
	m->draining = fed || std::any_of(output.begin(), output.end(), [](float s) { return s != 0; });
	if (!include_silence && !m->draining) output.clear();
	const auto error = alGetErrorDirect(m->context);
	return alcGetError(m->device) == ALC_NO_ERROR && error == AL_NO_ERROR;
}
} // namespace Libs::Audio
