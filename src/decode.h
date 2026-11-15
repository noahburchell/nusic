#pragma once

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <spa/param/audio/raw.h>

// format and stride describe the current frame as dec_read delivers it
// packed samples in the sample type of the decoder, planar input is interleaved
struct decoder {
	const char*               path;
	AVFormatContext*          demux;
	AVCodecContext*           codec;
	SwrContext*               swr;
	AVPacket*                 pkt;
	AVFrame*                  frame;
	struct spa_audio_info_raw format;
	AVChannelLayout           in_layout;
	enum AVSampleFormat       in_fmt;
	enum AVSampleFormat       out_fmt;
	int                       in_rate;
	int                       stream;
	uint32_t                  stride;
	uint32_t                  step;
	uint32_t                  planes;
	uint32_t                  left;
	uint32_t                  errors;
	bool                      flushing;
	bool                      warned;
	bool                      failed;
};

// decodes the first frame, so format is valid once this returns true
bool dec_open(struct decoder* d, const char* path);
void dec_close(struct decoder* d);
// true while a frame is current, decodes the next one when the last is used up
bool dec_next(struct decoder* d);
// writes at most max frames of the current frame to dst, returns the count
uint32_t dec_read(struct decoder* d, uint8_t* dst, uint32_t max);
