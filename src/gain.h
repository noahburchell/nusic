#pragma once

#include <spa/param/audio/raw.h>
#include <stdint.h>

// writes the n samples of src times g to dst, a sample past full scale is clipped to it
// with the soft clip on and g above 1, the samples near full scale are bent into it instead
typedef void gain_fn(void* restrict dst, const void* restrict src, uint32_t n, float g);

// the function for packed samples of the format, nullptr when there is none
gain_fn* gain_for(enum spa_audio_format format);
// the soft clip leaves the straight line at knee of full scale, from 0 to 1, where 1 is a hard clip
// set before the first stream opens, the realtime thread reads it without a lock
void gain_soft(bool on, double knee);
