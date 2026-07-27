#version 450

layout(location = 0) out vec2 uv;

void main() {
	vec2 position = vec2(float((gl_VertexIndex & 1u) << 2u), float((gl_VertexIndex & 2u) << 1u));
	gl_Position   = vec4(position - vec2(1.0), 0.0, 1.0);
	uv            = position * 0.5;
}
