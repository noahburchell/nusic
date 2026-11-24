#include "gain.h"

// the loops round half away from zero without a call into libm
// gcc vectorises them only with -fno-trapping-math, which configure sets

static void scale_s16(void* restrict dst, const void* restrict src, uint32_t n, float g) {
	const int16_t* s = src;
	int16_t* d = dst;
	uint32_t i;

	for(i = 0; i < n; i++) {
		float v = (float)s[i] * g;

		v = v > 32767.0f ? 32767.0f : v;
		v = v < -32768.0f ? -32768.0f : v;
		d[i] = (int16_t)(v + (v < 0.0f ? -0.5f : 0.5f));
	}
}

// a float holds 24 bits, so 32 bit samples go through a double
static void scale_s32(void* restrict dst, const void* restrict src, uint32_t n, float g) {
	const int32_t* s = src;
	int32_t* d = dst;
	double k = (double)g;
	uint32_t i;

	for(i = 0; i < n; i++) {
		double v = (double)s[i] * k;

		v = v > 2147483647.0 ? 2147483647.0 : v;
		v = v < -2147483648.0 ? -2147483648.0 : v;
		d[i] = (int32_t)(v + (v < 0.0 ? -0.5 : 0.5));
	}
}

static void scale_f32(void* restrict dst, const void* restrict src, uint32_t n, float g) {
	const float* s = src;
	float* d = dst;
	uint32_t i;

	for(i = 0; i < n; i++) {
		float v = s[i] * g;

		v = v > 1.0f ? 1.0f : v;
		d[i] = v < -1.0f ? -1.0f : v;
	}
}

static void scale_f64(void* restrict dst, const void* restrict src, uint32_t n, float g) {
	const double* s = src;
	double* d = dst;
	double k = (double)g;
	uint32_t i;

	for(i = 0; i < n; i++) {
		double v = s[i] * k;

		v = v > 1.0 ? 1.0 : v;
		d[i] = v < -1.0 ? -1.0 : v;
	}
}

gain_fn* gain_for(enum spa_audio_format format) {
	if(format == SPA_AUDIO_FORMAT_S16) {
		return scale_s16;
	}

	if(format == SPA_AUDIO_FORMAT_S32) {
		return scale_s32;
	}

	if(format == SPA_AUDIO_FORMAT_F32) {
		return scale_f32;
	}

	return format == SPA_AUDIO_FORMAT_F64 ? scale_f64 : nullptr;
}
