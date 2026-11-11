#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

layout(push_constant) uniform Push {
	uint anchor_select;
	uint stencil_bit;
}
push;

#if defined(EQAA_SELECT)
#define FETCH(source) texelFetch(source, pixel, int((push.anchor_select >> (4u * quad)) & 0xfu))
#else
#define FETCH(source) texelFetch(source, pixel, 0)
#endif

#if defined(EQAA_COLOR) && defined(EQAA_FLOAT)
// Formats with no integer view round-trip exactly through fp32.
#if defined(EQAA_SELECT)
layout(binding = 0, set = 0) uniform texture2DMS source;
#else
layout(binding = 0, set = 0) uniform texture2D source;
#endif
layout(location = 0) out vec4 color;
#elif defined(EQAA_COLOR)
#if defined(EQAA_SELECT)
layout(binding = 0, set = 0) uniform utexture2DMS source;
#else
layout(binding = 0, set = 0) uniform utexture2D source;
#endif
layout(location = 0) out uvec4 color;
#elif defined(EQAA_DEPTH)
#if defined(EQAA_SELECT)
layout(binding = 0, set = 0) uniform texture2DMS source;
#else
layout(binding = 0, set = 0) uniform texture2D source;
#endif
#else
#if defined(EQAA_SELECT)
layout(binding = 0, set = 0) uniform utexture2DMS source;
#else
layout(binding = 0, set = 0) uniform utexture2D source;
#endif
#endif

void main() {
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	uint  quad  = (uint(pixel.x) & 1u) | ((uint(pixel.y) & 1u) << 1u);
#if defined(EQAA_COLOR)
	color = FETCH(source);
#elif defined(EQAA_DEPTH)
	gl_FragDepth = FETCH(source).r;
#else
	if (((FETCH(source).r >> push.stencil_bit) & 1u) == 0u) {
		discard;
	}
#endif
}
