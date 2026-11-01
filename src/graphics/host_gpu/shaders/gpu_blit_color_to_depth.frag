#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

// Writes the floats a 32-bit color image holds into the depth aspect of a depth attachment.
layout(binding = 0) uniform utexture2D color;

void main() {
	gl_FragDepth = uintBitsToFloat(texelFetch(color, ivec2(gl_FragCoord.xy), 0).r);
}
