#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 color;

vec3 SrgbToLinear(vec3 rgb) {
	return mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
	           greaterThan(rgb, vec3(0.04045)));
}

vec3 LinearToPq(vec3 normalized) {
	vec3 powered = pow(clamp(normalized, 0.0, 1.0), vec3(2610.0 / 16384.0));
	return pow((3424.0 / 4096.0 + (2413.0 / 128.0) * powered) /
	               (1.0 + (2392.0 / 128.0) * powered), vec3(2523.0 / 32.0));
}

vec3 SdrToHdr10(vec3 rgb, bool source_linear) {
	rgb = clamp(rgb, 0.0, 1.0);
	vec3 linear = source_linear ? rgb : SrgbToLinear(rgb);
	// Convert primaries and scale 250-nit reference white relative to 10,000 nits.
	const mat3 to_rec2020 = (250.0 / 10000.0) *
	    mat3(0.627404, 0.069097, 0.016391,
	         0.329283, 0.919540, 0.088013,
	         0.043313, 0.011362, 0.895595);
	return LinearToPq(to_rec2020 * linear);
}

#ifdef IMGUI_HDR
layout(set = 0, binding = 0) uniform texture2D image;
layout(set = 1, binding = 0) uniform sampler image_sampler;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
	vec4 sampled = In.Color * texture(sampler2D(image, image_sampler), In.UV);
	color = vec4(SdrToHdr10(sampled.rgb, false), sampled.a);
}
#else
#include "gpu_video_out_hdr10_to_sdr.inc"

layout(location = 0) in vec2 uv;
layout(binding = 0) uniform sampler2D source;
layout(push_constant) uniform Layer {
	uint flags; // bit 0: premultiplied, 1: HDR target, 2: PQ source, 3: linear source, 4: opaque
	// The layer's size on the target.
	int width;
	int height;
};

const uint HDR_TARGET = 2u;
const uint PQ_SOURCE = 4u;

// A source pixel as the layer shows it.
vec4 Shown(vec4 sampled) {
	if ((flags & 16u) != 0u) {
		sampled.a = 1.0;
	}
	bool premultiplied = (flags & 1u) != 0u;
	bool source_linear = (flags & 8u) != 0u;
	uint conversion = flags & (HDR_TARGET | PQ_SOURCE);
	bool convert = conversion == HDR_TARGET || conversion == PQ_SOURCE || source_linear;
	if (convert && premultiplied) {
		sampled.rgb = sampled.a > 0.0 ? sampled.rgb / sampled.a : vec3(0.0);
	}
	if (conversion == HDR_TARGET) {
		sampled.rgb = SdrToHdr10(sampled.rgb, source_linear);
	} else if (conversion == PQ_SOURCE) {
		sampled.rgb = Hdr10ToSdr(sampled.rgb, source_linear);
	} else if (source_linear) {
		sampled.rgb = conversion == 0u ? LinearToSrgb(sampled.rgb)
		                               : LinearToPq(sampled.rgb * (1.0 / 10000.0));
	}
	if (!premultiplied || convert) {
		sampled.rgb *= sampled.a;
	}
	return sampled;
}

// The source texels of a target pixel along one axis: those it covers, or a linear filter's two.
void Footprint(int pixel, int size, int shown, out int first, out int last) {
	if (shown < size) {
		first = pixel * size / shown;
		last  = ((pixel + 1) * size - 1) / shown;
	} else {
		first = ((2 * pixel + 1) * size + shown) / (2 * shown) - 1;
		last  = first + 1;
	}
}

// The share of one of them.
float Weight(int texel, int pixel, int size, int shown) {
	if (shown < size) {
		int low  = max(texel * shown, pixel * size);
		int high = min((texel + 1) * shown, (pixel + 1) * size);
		return float(high - low) / float(size);
	}
	int   centre = (2 * pixel + 1) * size + shown;
	float second = float(centre % (2 * shown)) / float(2 * shown);
	return texel == centre / (2 * shown) - 1 ? 1.0 - second : second;
}

void main() {
	ivec2 size  = textureSize(source, 0);
	ivec2 shown = ivec2(width, height);
	if (all(greaterThanEqual(shown, size))) {
		color = Shown(texture(source, uv));
		return;
	}
	// A linear filter reads two source pixels per axis. A layer shown smaller takes the mean of
	// those each pixel covers: of their light when opaque, else of the values the target blends.
	ivec2 pixel  = ivec2(uv * vec2(shown));
	bool  opaque = (flags & 16u) != 0u;
	bool  pq     = (flags & HDR_TARGET) != 0u;
	ivec2 first;
	ivec2 last;
	Footprint(pixel.x, size.x, shown.x, first.x, last.x);
	Footprint(pixel.y, size.y, shown.y, first.y, last.y);
	vec4 mean = vec4(0.0);
	for (int y = first.y; y <= last.y; y++) {
		float row = Weight(y, pixel.y, size.y, shown.y);
		for (int x = first.x; x <= last.x; x++) {
			vec4 texel = Shown(texelFetch(source, clamp(ivec2(x, y), ivec2(0), size - 1), 0));
			if (opaque) {
				texel.rgb = pq ? PqToNits(texel.rgb) : SrgbToLinear(texel.rgb);
			}
			mean += row * Weight(x, pixel.x, size.x, shown.x) * texel;
		}
	}
	if (opaque) {
		mean = vec4(pq ? LinearToPq(mean.rgb / 10000.0) : LinearToSrgb(mean.rgb), 1.0);
	}
	color = mean;
}
#endif
