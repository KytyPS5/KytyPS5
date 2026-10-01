#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source;
layout(push_constant) uniform AlphaControl {
	uint premultiplied;
};

void main() {
	color = texture(source, uv);
	if (premultiplied == 0u) {
		color.rgb *= color.a;
	}
}
