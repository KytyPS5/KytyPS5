// Converts SMPTE ST 2084 PQ (or linear nits) Rec.2020 to sRGB-encoded Rec.709. The 250-nit SDR
// white scales to 1.0, then an extended Reinhard curve rolls off luminance above 0.75 so that
// 10,000 nits reaches 1.0.
vec3 Hdr10ToSdr(vec3 rgb, bool source_linear) {
	vec3 nits = clamp(rgb, 0.0, 10000.0);
	if (!source_linear) {
		vec3 powered = pow(rgb, vec3(32.0 / 2523.0));
		nits = 10000.0 * pow(max(powered - 3424.0 / 4096.0, 0.0) /
		                         (2413.0 / 128.0 - (2392.0 / 128.0) * powered),
		                     vec3(16384.0 / 2610.0));
	}
	const mat3 to_rec709 = (1.0 / 250.0) *
	    mat3(1.660491, -0.124550, -0.018151,
	         -0.587641, 1.132900, -0.100579,
	         -0.072850, -0.008349, 1.118730);
	vec3 linear = max(to_rec709 * nits, 0.0);
	const float knee = 0.75;
	const float span = 1.0 - knee;
	const float white = (10000.0 / 250.0 - knee) / span;
	float luma = dot(linear, vec3(0.2126, 0.7152, 0.0722));
	if (luma > knee) {
		float x = (luma - knee) / span;
		linear *= (knee + span * x * (1.0 + x / (white * white)) / (1.0 + x)) / luma;
	}
	linear = clamp(linear, 0.0, 1.0);
	return mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055,
	           greaterThan(linear, vec3(0.0031308)));
}
