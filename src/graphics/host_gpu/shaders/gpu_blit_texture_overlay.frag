#version 450 core

layout(binding = 0, set = 0) uniform sampler2D overlay_source;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
	out_color = texture(overlay_source, uv);
}
