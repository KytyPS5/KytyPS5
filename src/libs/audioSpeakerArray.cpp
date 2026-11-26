#include "libs/audio.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <numbers>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Audio::AudioOut2 {
namespace {
using Position                   = AudioOut2Position;
constexpr int      INVALID_PARAM = -2144960511;
constexpr double   PI            = std::numbers::pi;
constexpr float    EPS           = 1e-5f;
constexpr unsigned MAX_SPEAKERS  = 32;
using Gains                      = std::array<float, MAX_SPEAKERS>;

float Dot(Position a, Position b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
Position Cross(Position a, Position b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

struct Triangle {
	std::array<unsigned, 3> speakers;
	std::array<Position, 3> inverse;
};

struct SpeakerArray {
	struct Azimuth {
		double   angle;
		unsigned speaker;
	};
	std::vector<Position> speakers;
	std::vector<Azimuth>  ring;
	std::vector<Triangle> hull;
	float                 correction    = 0;
	uint32_t              first_channel = 0;
	std::vector<Gains>    decoder;

	bool SetLayout(const AudioOut2SpeakerArrayParam& params);
	void BuildHull();
	void Pan(Position direction, float diffuse, bool height_aware, std::span<float> gains) const;
	void BuildDecoder(const AudioOut2AmbisonicsDecodeParam& params);
};

bool SpeakerArray::SetLayout(const AudioOut2SpeakerArrayParam& params) {
	if (params.count < (params.is_3d ? 4u : 2u) || params.count > MAX_SPEAKERS ||
	    !params.positions || params.correction_type > 1)
		return false;
	if (params.correction_type == 1) {
		if (!std::isfinite(params.correction_gain) || params.correction_gain < 0) return false;
		correction = params.correction_gain;
	}
	const float max_dot = params.count == 2 ? float(std::cos(5 * PI / 180)) : 1 - EPS;
	for (auto p: std::span(params.positions, params.count)) {
		if (!p.IsFinite()) return false;
		if (!params.is_3d) p.y = 0;
		const double length = p.Length();
		if (length == 0) return false;
		p = p / length;
		if (std::ranges::any_of(speakers, [&](Position s) { return Dot(p, s) > max_dot; }))
			return false;
		speakers.push_back(p);
	}
	if (params.is_3d) {
		BuildHull();
		if (hull.empty()) return false;
	} else {
		const auto [left, right] = std::ranges::minmax(speakers, {}, &Position::x);
		if (left.x > 0 || right.x < 0) return false;
	}
	// The lowest tier also handles directions below an open 3D array.
	const auto bottom = std::ranges::min(speakers, {}, &Position::y).y;
	for (unsigned i = 0; i < speakers.size(); ++i) {
		const auto p = speakers[i];
		if (p.y <= bottom + EPS) ring.push_back({std::atan2(p.x, p.z), i});
	}
	std::ranges::sort(ring, {}, &Azimuth::angle);
	return true;
}

void SpeakerArray::BuildHull() {
	// A supporting face has a plane n.p = 1 with every speaker on or behind it.
	// Triangulate flat polygonal faces as a fan about their first vertex.
	for (unsigned i = 0; i < speakers.size(); ++i) {
		for (unsigned j = i + 1; j < speakers.size(); ++j) {
			for (unsigned k = j + 1; k < speakers.size(); ++k) {
				const auto  a = speakers[i], b = speakers[j], c = speakers[k];
				const auto  bc  = Cross(b, c);
				const float det = Dot(a, bc);
				if (std::abs(det) < EPS) continue;
				const Triangle face {{i, j, k}, {bc / det, Cross(c, a) / det, Cross(a, b) / det}};
				bool           behind = false, valid = true;
				for (unsigned v = 0; v < speakers.size(); ++v) {
					if (v == i || v == j || v == k) continue;
					const auto  p    = speakers[v];
					const float w    = Dot(face.inverse[0], p);
					const float side = w + Dot(face.inverse[1], p) + Dot(face.inverse[2], p) - 1;
					if (side > EPS || (side >= -EPS && (v < i || w < -EPS))) {
						valid = false;
						break;
					}
					behind |= side < -EPS;
				}
				if (valid && behind) hull.push_back(face);
			}
		}
	}
}

void SpeakerArray::Pan(Position dir, float diffuse, bool height_aware,
                       std::span<float> gains) const {
	std::ranges::fill(gains, 0);
	bool in_hull = false;
	for (const auto& face: hull) {
		const std::array w {Dot(dir, face.inverse[0]), Dot(dir, face.inverse[1]),
		                    Dot(dir, face.inverse[2])};
		if (std::ranges::min(w) < -EPS) continue;
		for (size_t j = 0; j < 3; ++j)
			gains[face.speakers[j]] = std::max(0.f, w[j]);
		in_hull = true;
		break;
	}
	if (!in_hull) {
		const auto angle = std::atan2(double(dir.x), double(dir.z));
		auto       right = std::ranges::upper_bound(ring, angle, {}, &Azimuth::angle);
		auto       left  = right == ring.begin() ? ring.end() - 1 : right - 1;
		if (right == ring.end()) right = ring.begin();
		const auto gap    = right->angle - left->angle + (right <= left ? 2 * PI : 0);
		const auto offset = angle - left->angle + (angle < left->angle ? 2 * PI : 0);
		// Across a gap of half a circle or more the two vectors have no positive
		// basis. Interpolate at constant power along that uncovered arc.
		if (gap >= PI - EPS) {
			gains[left->speaker] = float(std::cos(PI / 2 * offset / gap));
			gains[right->speaker] += float(std::sin(PI / 2 * offset / gap));
		} else {
			gains[left->speaker]  = float(std::sin(gap - offset));
			gains[right->speaker] = float(std::sin(offset));
		}
		if (height_aware && correction > 0 && !hull.empty()) {
			// Fold the virtual bottom speaker onto the lowest tier.
			const float horizontal = std::hypot(dir.x, dir.z);
			const float bottom = correction * std::max(0.f, -dir.y) / std::sqrt(float(ring.size()));
			for (const auto& s: ring)
				gains[s.speaker] = horizontal * gains[s.speaker] + bottom;
		}
	}
	double power = 0;
	for (float g: gains)
		power += double(g) * g;
	const double direct  = power > 0 ? (1 - diffuse) / power : 0;
	const float  ambient = diffuse / float(gains.size());
	for (auto& gain: gains)
		gain = static_cast<float>(std::sqrt(direct * gain * gain + ambient));
}

// Real SN3D spherical harmonics, without the Condon-Shortley phase. The factor
// 2n+1 converts the encoding basis to its dual for spherical integration.
std::array<double, 36> DecodeBasis(double azimuth, double y) {
	std::array<double, 36> basis {};
	double                 diagonal = 1;
	for (int m = 0; m <= 5; ++m) {
		double prev = 0, p = diagonal;
		for (int n = m; n <= 5; ++n) {
			const double scale  = (2 * n + 1) * std::sqrt((m ? 2. : 1.) * std::tgamma(n - m + 1) /
			                                              std::tgamma(n + m + 1));
			const int    center = n * (n + 1);
			basis[center + m]   = scale * p * std::cos(m * azimuth);
			if (m) basis[center - m] = scale * p * std::sin(m * azimuth);
			const double next = ((2 * n + 1) * y * p - (n + m) * prev) / (n - m + 1);
			prev              = p;
			p                 = next;
		}
		diagonal *= (2 * m + 1) * std::sqrt(1 - y * y);
	}
	return basis;
}

void SpeakerArray::BuildDecoder(const AudioOut2AmbisonicsDecodeParam& params) {
	// Project this layout's VBAP gains onto the harmonics once at creation.
	// Sampling uniformly in sin(elevation) gives every cell equal solid angle.
	constexpr int rings = 24, slices = 48;
	const bool    fuma = params.ordering == 1;
	first_channel      = fuma ? 0 : 64;
	decoder.resize(fuma ? 16 : 36);
	constexpr int fuma_to_acn[] = {0, 3, 1, 2, 6, 7, 5, 8, 4, 12, 13, 11, 14, 10, 15, 9};
	// MaxN to SN3D scale for W and the second/third-order FuMa components.
	const double fuma_scale[] = {std::sqrt(2.),
	                             1,
	                             1,
	                             1,
	                             1,
	                             std::sqrt(.75),
	                             std::sqrt(.75),
	                             std::sqrt(.75),
	                             std::sqrt(.75),
	                             1,
	                             std::sqrt(32. / 45),
	                             std::sqrt(32. / 45),
	                             std::sqrt(5.) / 3,
	                             std::sqrt(5.) / 3,
	                             std::sqrt(5. / 8),
	                             std::sqrt(5. / 8)};
	for (int r = 0; r < rings; ++r) {
		const double y = (2 * r + 1.) / rings - 1;
		const double h = std::sqrt(1 - y * y);
		for (int s = 0; s < slices; ++s) {
			const double az    = (s + .5) * 2 * PI / slices;
			const auto   basis = DecodeBasis(az, y);
			Gains        gains {};
			Pan({float(-h * std::sin(az)), float(y), float(h * std::cos(az))}, 0,
			    params.height_aware, std::span(gains).first(speakers.size()));
			for (size_t ch = 0; ch < decoder.size(); ++ch) {
				const double weight = fuma ? basis[fuma_to_acn[ch]] * fuma_scale[ch] : basis[ch];
				for (size_t i = 0; i < speakers.size(); ++i)
					decoder[ch][i] += float(weight / (rings * slices) * gains[i]);
			}
		}
	}
}

std::mutex                                                    arrays_mutex;
std::unordered_map<AudioOut2SpeakerArrayHandle, SpeakerArray> arrays;
} // namespace

size_t KYTY_SYSV_ABI AudioOut2GetSpeakerArrayMemorySize(uint32_t count, uint8_t is_3d,
                                                        uint8_t is_ambisonics) {
	return 0x400 + std::clamp(count, 1u, MAX_SPEAKERS) * (is_ambisonics ? 0x100 : 0x40) +
	       (is_3d ? 0x200 : 0);
}

int KYTY_SYSV_ABI AudioOut2SpeakerArrayCreate(AudioOut2SpeakerArrayHandle*          handle,
                                              const AudioOut2SpeakerArrayParam*     p,
                                              const AudioOut2AmbisonicsDecodeParam* ambi) {
	if (!handle || !p || !p->buffer || (ambi && ambi->ordering > 1) ||
	    p->size < AudioOut2GetSpeakerArrayMemorySize(p->count, p->is_3d, ambi != nullptr))
		return INVALID_PARAM;
	SpeakerArray array;
	if (!array.SetLayout(*p)) return INVALID_PARAM;
	if (ambi) array.BuildDecoder(*ambi);
	std::lock_guard lock(arrays_mutex);
	if (!arrays.try_emplace(p->buffer, std::move(array)).second) return INVALID_PARAM;
	*handle = p->buffer;
	return OK;
}

int KYTY_SYSV_ABI AudioOut2SpeakerArrayDestroy(AudioOut2SpeakerArrayHandle handle) {
	std::lock_guard lock(arrays_mutex);
	return arrays.erase(handle) ? OK : INVALID_PARAM;
}

int KYTY_SYSV_ABI AudioOut2GetSpeakerArrayCoefficients(AudioOut2SpeakerArrayHandle handle,
                                                       Position pos, float spread, float* gains,
                                                       uint32_t count, uint8_t height_aware,
                                                       float radius) {
	std::lock_guard lock(arrays_mutex);
	const auto      it = arrays.find(handle);
	if (it == arrays.end() || !gains || count != it->second.speakers.size() || !pos.IsFinite() ||
	    !std::isfinite(spread) || spread < 0 || !(radius >= .1f && radius <= 2))
		return INVALID_PARAM;
	const double distance = pos.Length();
	const float  diffuse =
	    std::clamp(std::max(spread / float(2 * PI), float(1 - distance / radius)), 0.f, 1.f);
	it->second.Pan(distance ? pos / distance : Position {}, diffuse, height_aware, {gains, count});
	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSpeakerArrayAmbisonicsCoefficients(AudioOut2SpeakerArrayHandle handle,
                                                                 uint32_t channel, float* gains,
                                                                 uint32_t count) {
	std::lock_guard lock(arrays_mutex);
	const auto      it = arrays.find(handle);
	if (it == arrays.end() || !gains || count != it->second.speakers.size()) return INVALID_PARAM;
	const auto row = channel - it->second.first_channel;
	if (row >= it->second.decoder.size()) return INVALID_PARAM;
	std::copy_n(it->second.decoder[row].begin(), count, gains);
	return OK;
}
} // namespace Libs::Audio::AudioOut2
