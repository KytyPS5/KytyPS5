#include "libs/audio3d.h"

#include "libs/audio_internal.h"
#include "libs/errno.h"
#include "libs/spatialRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace Libs::Audio::Audio3d {

namespace {
constexpr int INVALID_PORT      = -2132148222;
constexpr int INVALID_OBJECT    = -2132148221;
constexpr int INVALID_PARAMETER = -2132148220;
constexpr int OUT_OF_RESOURCES  = -2132148218;
constexpr int NOT_READY         = -2132148217;
enum Attribute : uint32_t {
	Pcm        = 1,
	Position   = 3,
	Spread     = 4,
	Gain       = 5,
	Reset      = 7,
	Ambisonics = 9
};

struct Port {
	std::mutex                       mutex;
	std::condition_variable          changed;
	std::unique_ptr<SpatialRenderer> renderer;
	uint32_t                         frames = 0;
	std::vector<SpatialObject>       objects;
	std::vector<uint32_t>            ids;
	uint32_t                         serial = 0;
	std::vector<std::vector<float>>  grains;
	size_t                           read = 0, queued = 0, playable = 0;
	bool                             closed = false;
	std::thread                      worker;

	void Play() {
		std::unique_lock lock(mutex);
		while (true) {
			changed.wait(lock, [&] { return closed || playable; });
			if (closed) return;
			AudioInternal::OutputParam output {renderer->OutputHandle(), grains[read].data()};
			lock.unlock();
			AudioInternal::AudioOutOutputs(&output, 1);
			lock.lock();
			read = (read + 1) % grains.size();
			--queued;
			--playable;
			changed.notify_all();
		}
	}

	~Port() {
		if (worker.joinable()) Stop();
	}

	void Stop() {
		{
			std::lock_guard lock(mutex);
			closed = true;
		}
		changed.notify_all();
		worker.join();
	}

	SpatialObject* FindObject(uint32_t id) {
		const auto slot = id & 511u;
		return id != UINT32_MAX && slot < ids.size() && ids[slot] == id ? &objects[slot] : nullptr;
	}
};

std::array<std::shared_ptr<Port>, 4> ports;
std::mutex                           ports_mutex;

std::shared_ptr<Port> FindPort(uint32_t id) {
	std::lock_guard lock(ports_mutex);
	return id < ports.size() ? ports[id] : nullptr;
}
} // namespace

void Shutdown() {
	for (uint32_t i = 0; i < ports.size(); ++i)
		Audio3dPortClose(i);
}

int KYTY_SYSV_ABI Audio3dInitialize(int64_t reserved) {
	return reserved == 0 ? OK : INVALID_PARAMETER;
}

void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p) {
	static_assert(sizeof(*p) == 0x20);
	if (p) *p = {};
}

int KYTY_SYSV_ABI Audio3dPortOpen(int user_id, const Audio3dOpenParameters* p, uint32_t* id) {
	if (!id) return INVALID_PARAMETER;
	*id = UINT32_MAX;
	if (!p || (p->size != 0x20 && p->size != 0x28) || (user_id != 255 && user_id != 1) ||
	    p->rate != 0 || p->buffer_mode != 2 || !p->max_objects || p->max_objects > 512 ||
	    !p->granularity || p->granularity % 256 || !p->queue_depth)
		return INVALID_PARAMETER;
	const uint32_t max_depth = p->granularity == 256   ? 64
	                           : p->granularity == 512 ? 31
	                           : p->granularity == 768 ? 20
	                                                   : 15;
	if (p->queue_depth > max_depth) return INVALID_PARAMETER;
	std::lock_guard lock(ports_mutex);
	const auto      slot = std::find(ports.begin(), ports.end(), nullptr);
	if (slot == ports.end()) return OUT_OF_RESOURCES;
	auto port      = std::make_shared<Port>();
	port->renderer = SpatialRenderer::Open(p->granularity, p->max_objects);
	if (!port->renderer) return OUT_OF_RESOURCES;
	port->frames = p->granularity;
	port->objects.resize(p->max_objects);
	port->ids.resize(p->max_objects, UINT32_MAX);
	port->grains.resize(p->queue_depth);
	port->worker = std::thread(&Port::Play, port.get());
	*slot        = std::move(port);
	*id          = static_cast<uint32_t>(slot - ports.begin());
	return OK;
}

int KYTY_SYSV_ABI Audio3dPortClose(uint32_t id) {
	std::shared_ptr<Port> port;
	{
		std::lock_guard lock(ports_mutex);
		if (id >= ports.size() || !ports[id]) return INVALID_PORT;
		port = std::move(ports[id]);
	}
	port->Stop();
	return OK;
}

int KYTY_SYSV_ABI Audio3dObjectReserve(uint32_t id, uint32_t* object) {
	if (!object) return INVALID_PARAMETER;
	*object = UINT32_MAX;
	auto p  = FindPort(id);
	if (!p) return INVALID_PORT;
	std::lock_guard lock(p->mutex);
	for (size_t i = 0; i < p->ids.size(); ++i) {
		// An unreserved object may still have one final grain to play.
		if (p->ids[i] != UINT32_MAX || !p->objects[i].pcm.empty()) continue;
		do {
			*object = (++p->serial << 9) | static_cast<uint32_t>(i);
		} while (*object == UINT32_MAX);
		p->ids[i]          = *object;
		p->objects[i]      = {};
		p->objects[i].gain = 0;
		return OK;
	}
	return OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI Audio3dObjectUnreserve(uint32_t id, uint32_t object) {
	auto p = FindPort(id);
	if (!p) return INVALID_PORT;
	std::lock_guard lock(p->mutex);
	if (!p->FindObject(object)) return INVALID_OBJECT;
	p->ids[object & 511u] = UINT32_MAX;
	return OK;
}

int KYTY_SYSV_ABI Audio3dObjectSetAttribute(uint32_t id, uint32_t object, uint32_t attribute,
                                            const void* value, size_t size) {
	const Audio3dAttribute a {attribute, 0, value, size};
	return Audio3dObjectSetAttributes(id, object, 1, &a);
}

int KYTY_SYSV_ABI Audio3dObjectSetAttributes(uint32_t id, uint32_t object, size_t count,
                                             const Audio3dAttribute* attributes) {
	auto p = FindPort(id);
	if (!p) return INVALID_PORT;
	std::lock_guard lock(p->mutex);
	auto*           obj = p->FindObject(object);
	if (!obj) return INVALID_OBJECT;
	if (count && !attributes) return INVALID_PARAMETER;
	if (p->queued == p->grains.size()) return NOT_READY;

	// Apply RESET before other attributes and PCM last, regardless of guest order.
	uint32_t seen = 0;
	for (const auto& a: std::span(attributes, count)) {
		if (a.id > 11) continue;
		if (seen & (1u << a.id)) return INVALID_PARAMETER;
		seen |= 1u << a.id;
	}
	const bool reset = (seen & (1u << Reset)) != 0;
	if (reset && !obj->pcm.empty()) return NOT_READY;
	auto       spatial = reset ? SpatialAttributes {} : obj->attributes;
	float      gain    = reset ? 0 : obj->gain;
	Audio3dPcm pcm {};
	for (const auto& a: std::span(attributes, count)) {
		switch (a.id) {
			case Pcm:
				if (!a.value || a.size != sizeof(pcm)) return INVALID_PARAMETER;
				std::memcpy(&pcm, a.value, sizeof(pcm));
				if (pcm.format > 1 || (pcm.count && !pcm.samples) ||
				    reinterpret_cast<uintptr_t>(pcm.samples) %
				        (pcm.format ? sizeof(float) : sizeof(int16_t)))
					return INVALID_PARAMETER;
				break;
			case Position:
				if (!spatial.SetPosition(a.value, a.size)) return INVALID_PARAMETER;
				break;
			case Spread:
				if (!spatial.SetSpread(a.value, a.size)) return INVALID_PARAMETER;
				break;
			case Gain:
				if (!a.value || a.size != sizeof(gain)) return INVALID_PARAMETER;
				std::memcpy(&gain, a.value, a.size);
				if (!std::isfinite(gain) || gain < 0) return INVALID_PARAMETER;
				break;
			case Ambisonics:
				if (!spatial.SetAmbisonics(a.value, a.size, false)) return INVALID_PARAMETER;
				break;
		}
	}
	obj->attributes = spatial;
	obj->gain       = gain;
	if (seen & (1u << Pcm)) {
		obj->is_float      = pcm.format == 1;
		const size_t width = obj->is_float ? sizeof(float) : sizeof(int16_t);
		obj->pcm.assign(p->frames * width, 0);
		if (pcm.count)
			std::memcpy(obj->pcm.data(), pcm.samples, std::min(pcm.count, p->frames) * width);
	}
	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetAttributesSupported(uint32_t id, uint32_t* caps, uint32_t* count) {
	if (!FindPort(id)) return INVALID_PORT;
	if (!count) return INVALID_PARAMETER;
	static constexpr uint32_t supported[] = {Pcm, Position, Spread, Gain, Reset, Ambisonics};
	if (caps) {
		*count = std::min(*count, static_cast<uint32_t>(std::size(supported)));
		std::copy_n(supported, *count, caps);
	} else {
		*count = std::size(supported);
	}
	return OK;
}

int KYTY_SYSV_ABI Audio3dPortSetAttribute(uint32_t id, uint32_t attribute, const void* value,
                                          size_t size) {
	if (!FindPort(id)) return INVALID_PORT;
	// The legacy reverb and downmix port controls are accepted by the ABI.
	if (!value || size != 4 || attribute < 0x10001 || attribute > 0x10003) return INVALID_PARAMETER;
	return OK;
}

int KYTY_SYSV_ABI Audio3dPortAdvance(uint32_t id) {
	auto p = FindPort(id);
	if (!p) return INVALID_PORT;
	std::lock_guard lock(p->mutex);
	if (p->queued == p->grains.size()) return NOT_READY;
	auto& grain = p->grains[(p->read + p->queued) % p->grains.size()];
	if (!p->renderer->Render(p->objects, grain, true)) return OUT_OF_RESOURCES;
	for (auto& obj: p->objects)
		obj.pcm.clear();
	++p->queued;
	return OK;
}

int KYTY_SYSV_ABI Audio3dPortPush(uint32_t id, uint32_t blocking) {
	auto p = FindPort(id);
	if (!p) return INVALID_PORT;
	if (blocking > 1) return INVALID_PARAMETER;
	std::unique_lock lock(p->mutex);
	p->playable = p->queued;
	p->changed.notify_all();
	if (blocking) p->changed.wait(lock, [&] { return p->closed || p->queued < p->grains.size(); });
	return p->closed ? INVALID_PORT : OK;
}

int KYTY_SYSV_ABI Audio3dPortGetQueueLevel(uint32_t id, uint32_t* level, uint32_t* available) {
	auto p = FindPort(id);
	if (!p) return INVALID_PORT;
	if (!level && !available) return INVALID_PARAMETER;
	std::lock_guard lock(p->mutex);
	if (level) *level = static_cast<uint32_t>(p->queued);
	if (available) *available = static_cast<uint32_t>(p->grains.size() - p->queued);
	return OK;
}

} // namespace Libs::Audio::Audio3d
