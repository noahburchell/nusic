#pragma once

#include <spa/param/audio/raw.h>
#include <stdint.h>

// writes the n samples of src times g to dst, a sample past full scale is clipped to it
// with g above 1 that is a soft clip, which bends the samples near full scale too
typedef void gain_fn(void* restrict dst, const void* restrict src, uint32_t n, float g);

// the function for packed samples of the format, nullptr when there is none
gain_fn* gain_for(enum spa_audio_format format);
