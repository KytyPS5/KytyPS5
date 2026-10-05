// ColorTransform values match imageInfo.h. Preserve the floating-point bit fields exactly.
uvec2 transform_color(uvec2 value, uint transform, bool to_host) {
	switch (transform) {
		case 1u:
			return uvec2((value.y & 0xffffu) | (value.x & 0xffff0000u),
			             (value.x & 0xffffu) | (value.y & 0xffff0000u));
		case 2u: {
			uint packed = value.x;
			if (to_host) {
				value.x = (packed >> 21u) | (((packed >> 10u) & 0x7ffu) << 11u) |
				          ((packed & 0x3ffu) << 22u);
			} else {
				value.x = (packed >> 22u) | (((packed >> 11u) & 0x7ffu) << 10u) |
				          ((packed & 0x7ffu) << 21u);
			}
			break;
		}
	}
	return value;
}
