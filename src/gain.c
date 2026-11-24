#include "gain.h"

#include <math.h>

// the loops round half away from zero without a call into libm
// gcc vectorises them only with -fno-trapping-math, which configure sets

// the soft clip is straight up to KNEE of full scale, from there a parabola bends it into full scale
// the parabola ends flat where the straight line would be at 2 - KNEE of full scale
#define KNEE          0.8
#define KNEE_AT(fs)   (KNEE * (fs))
#define KNEE_END(fs)  ((2.0 - KNEE) * (fs))
#define KNEE_BEND(fs) (1.0 / (4.0 * (1.0 - KNEE) * (fs)))

static void soft_s16(void* restrict dst, const void* restrict src, uint32_t n, float g) {
	const int16_t* s = src;
	int16_t* d = dst;
	float at = (float)KNEE_AT(32767.0);
	float end = (float)KNEE_END(32767.0);
	float bend = (float)KNEE_BEND(32767.0);
	uint32_t i;

	for(i = 0; i < n; i++) {
		float v = (float)s[i] * g;
		float a = fabsf(v);
		float e;

		a = a > end ? end : a;
		e = a - at;
		e = e < 0.0f ? 0.0f : e;
		a -= e * e * bend;
		d[i] = (int16_t)copysignf(a + 0.5f, v);
	}
}

static void soft_s32(void* restrict dst, const void* restrict src, uint32_t n, float g) {
	const int32_t* s = src;
	int32_t* d = dst;
	double at = KNEE_AT(2147483647.0);
	double end = KNEE_END(2147483647.0);
	double bend = KNEE_BEND(2147483647.0);
	double k = (double)g;
	uint32_t i;

	for(i = 0; i < n; i++) {
		double v = (double)s[i] * k;
		double a = fabs(v);
		double e;

		a = a > end ? end : a;
		e = a - at;
		e = e < 0.0 ? 0.0 : e;
		a -= e * e * bend;
		d[i] = (int32_t)copysign(a + 0.5, v);
	}
}

static void soft_f32(void* restrict dst, const void* restrict src, uint32_t n, float g) {
	const float* s = src;
	float* d = dst;
	float at = (float)KNEE_AT(1.0);
	float end = (float)KNEE_END(1.0);
	float bend = (float)KNEE_BEND(1.0);
	uint32_t i;

	for(i = 0; i < n; i++) {
		float v = s[i] * g;
		float a = fabsf(v);
		float e;

		a = a > end ? end : a;
		e = a - at;
		e = e < 0.0f ? 0.0f : e;
		a -= e * e * bend;
		d[i] = copysignf(a, v);
	}
}

static void soft_f64(void* restrict dst, const void* restrict src, uint32_t n, float g) {
	const double* s = src;
	double* d = dst;
	double at = KNEE_AT(1.0);
	double end = KNEE_END(1.0);
	double bend = KNEE_BEND(1.0);
	double k = (double)g;
	uint32_t i;

	for(i = 0; i < n; i++) {
		double v = s[i] * k;
		double a = fabs(v);
		double e;

		a = a > end ? end : a;
		e = a - at;
		e = e < 0.0 ? 0.0 : e;
		a -= e * e * bend;
		d[i] = copysign(a, v);
	}
}

// a gain of 1 or less cannot push a sample of the file past full scale, so the soft clip starts above it
static void scale_s16(void* restrict dst, const void* restrict src, uint32_t n, float g) {
	const int16_t* s = src;
	int16_t* d = dst;
	uint32_t i;

	if(g > 1.0f) {
		soft_s16(dst, src, n, g);
		return;
	}

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

	if(g > 1.0f) {
		soft_s32(dst, src, n, g);
		return;
	}

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

	if(g > 1.0f) {
		soft_f32(dst, src, n, g);
		return;
	}

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

	if(g > 1.0f) {
		soft_f64(dst, src, n, g);
		return;
	}

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
