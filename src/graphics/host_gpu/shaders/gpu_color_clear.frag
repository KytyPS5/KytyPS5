#version 450

struct Record {
	uint vertex_count;
	uint instance_count;
	uint first_vertex;
	uint first_instance;
	uvec4 color;
};

layout(std430, binding = 0) readonly buffer record_buf {
	Record records[];
};

layout(push_constant) uniform Params {
	uint record;
} params;

layout(location = 0) out vec4 color;

void main() {
	color = uintBitsToFloat(records[params.record].color);
}
