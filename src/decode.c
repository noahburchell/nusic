#include "decode.h"

#include "report.h"

#include <string.h>

#define MAX_ERRORS 64

static const enum spa_audio_format spa_format[] = {
	[AV_SAMPLE_FMT_S16] = SPA_AUDIO_FORMAT_S16,
	[AV_SAMPLE_FMT_S32] = SPA_AUDIO_FORMAT_S32,
	[AV_SAMPLE_FMT_FLT] = SPA_AUDIO_FORMAT_F32,
	[AV_SAMPLE_FMT_DBL] = SPA_AUDIO_FORMAT_F64,
};

static const uint8_t spa_channel[] = {
	[AV_CHAN_FRONT_LEFT] = SPA_AUDIO_CHANNEL_FL,
	[AV_CHAN_FRONT_RIGHT] = SPA_AUDIO_CHANNEL_FR,
	[AV_CHAN_FRONT_CENTER] = SPA_AUDIO_CHANNEL_FC,
	[AV_CHAN_LOW_FREQUENCY] = SPA_AUDIO_CHANNEL_LFE,
	[AV_CHAN_BACK_LEFT] = SPA_AUDIO_CHANNEL_RL,
	[AV_CHAN_BACK_RIGHT] = SPA_AUDIO_CHANNEL_RR,
	[AV_CHAN_FRONT_LEFT_OF_CENTER] = SPA_AUDIO_CHANNEL_FLC,
	[AV_CHAN_FRONT_RIGHT_OF_CENTER] = SPA_AUDIO_CHANNEL_FRC,
	[AV_CHAN_BACK_CENTER] = SPA_AUDIO_CHANNEL_RC,
	[AV_CHAN_SIDE_LEFT] = SPA_AUDIO_CHANNEL_SL,
	[AV_CHAN_SIDE_RIGHT] = SPA_AUDIO_CHANNEL_SR,
	[AV_CHAN_TOP_CENTER] = SPA_AUDIO_CHANNEL_TC,
	[AV_CHAN_TOP_FRONT_LEFT] = SPA_AUDIO_CHANNEL_TFL,
	[AV_CHAN_TOP_FRONT_CENTER] = SPA_AUDIO_CHANNEL_TFC,
	[AV_CHAN_TOP_FRONT_RIGHT] = SPA_AUDIO_CHANNEL_TFR,
	[AV_CHAN_TOP_BACK_LEFT] = SPA_AUDIO_CHANNEL_TRL,
	[AV_CHAN_TOP_BACK_CENTER] = SPA_AUDIO_CHANNEL_TRC,
	[AV_CHAN_TOP_BACK_RIGHT] = SPA_AUDIO_CHANNEL_TRR,
	[AV_CHAN_STEREO_LEFT] = SPA_AUDIO_CHANNEL_FL,
	[AV_CHAN_STEREO_RIGHT] = SPA_AUDIO_CHANNEL_FR,
	[AV_CHAN_WIDE_LEFT] = SPA_AUDIO_CHANNEL_FLW,
	[AV_CHAN_WIDE_RIGHT] = SPA_AUDIO_CHANNEL_FRW,
	[AV_CHAN_LOW_FREQUENCY_2] = SPA_AUDIO_CHANNEL_LFE2,
	[AV_CHAN_TOP_SIDE_LEFT] = SPA_AUDIO_CHANNEL_TSL,
	[AV_CHAN_TOP_SIDE_RIGHT] = SPA_AUDIO_CHANNEL_TSR,
	[AV_CHAN_BOTTOM_FRONT_CENTER] = SPA_AUDIO_CHANNEL_BC,
	[AV_CHAN_BOTTOM_FRONT_LEFT] = SPA_AUDIO_CHANNEL_BLC,
	[AV_CHAN_BOTTOM_FRONT_RIGHT] = SPA_AUDIO_CHANNEL_BRC,
};

static void diag(const struct decoder* d, const char* verb, int err) {
	report("%s %s: %s", verb, d->path, av_err2str(err));
}

// a damaged packet costs its own samples, the rest of the file still plays
static void warn(struct decoder* d, int err) {
	if(d->warned) {
		return;
	}

	d->warned = true;
	report("warning: decode %s: %s", d->path, av_err2str(err));
}

// a layout without channel names gets the default one for its channel count
static void positions(struct spa_audio_info_raw* fmt, const AVChannelLayout* layout) {
	AVChannelLayout guess = { 0 };
	const AVChannelLayout* l = layout;
	uint32_t i;

	if(layout->order == AV_CHANNEL_ORDER_UNSPEC) {
		av_channel_layout_default(&guess, layout->nb_channels);
		l = &guess;
	}

	for(i = 0; i < fmt->channels; i++) {
		unsigned c = (unsigned)av_channel_layout_channel_from_index(l, i);
		uint32_t p = c < sizeof(spa_channel) / sizeof(*spa_channel) ? spa_channel[c] : 0;

		fmt->position[i] = p ? p : SPA_AUDIO_CHANNEL_START_Aux + i;
	}

	if(fmt->channels == 1 && fmt->position[0] == SPA_AUDIO_CHANNEL_FC) {
		fmt->position[0] = SPA_AUDIO_CHANNEL_MONO;
	}
}

static bool configure(struct decoder* d) {
	const AVFrame* f = d->frame;
	enum AVSampleFormat in = f->format;
	enum AVSampleFormat out = av_get_packed_sample_fmt(in);
	int planar = av_sample_fmt_is_planar(in);
	int ch = f->ch_layout.nb_channels;
	int r;

	// pipewire has no 64 bit integer format, and it drains a stream with zero bytes,
	// which an unsigned format plays as a full scale pulse, so u8 widens to s16
	if(out == AV_SAMPLE_FMT_S64) {
		out = AV_SAMPLE_FMT_S32;
	} else if(out == AV_SAMPLE_FMT_U8) {
		out = AV_SAMPLE_FMT_S16;
	}

	if((unsigned)out >= sizeof(spa_format) / sizeof(*spa_format) || !spa_format[out]) {
		report("%s: unsupported sample format", d->path);
		return false;
	}

	if(ch < 1 || ch > (int)SPA_AUDIO_MAX_CHANNELS) {
		report("%s: has %d channels, the maximum is %u", d->path, ch, SPA_AUDIO_MAX_CHANNELS);
		return false;
	}

	if(f->sample_rate < 1) {
		report("%s: invalid sample rate %d Hz", d->path, f->sample_rate);
		return false;
	}

	swr_free(&d->swr);
	if(out != in) {
		r = swr_alloc_set_opts2(&d->swr, &f->ch_layout, out, f->sample_rate, &f->ch_layout, in, f->sample_rate, 0, nullptr);
		if(r >= 0) {
			r = swr_init(d->swr);
		}

		if(r < 0) {
			diag(d, "convert", r);
			return false;
		}
	}

	r = av_channel_layout_copy(&d->in_layout, &f->ch_layout);
	if(r < 0) {
		diag(d, "decode", r);
		return false;
	}

	d->in_fmt = in;
	d->out_fmt = out;
	d->in_rate = f->sample_rate;
	d->planes = planar ? (uint32_t)ch : 1;
	d->step = (uint32_t)av_get_bytes_per_sample(in) * (planar ? 1 : (uint32_t)ch);
	d->stride = (uint32_t)av_get_bytes_per_sample(out) * (uint32_t)ch;
	d->format = (struct spa_audio_info_raw){
		.format = spa_format[out],
		.rate = (uint32_t)f->sample_rate,
		.channels = (uint32_t)ch,
	};
	positions(&d->format, &f->ch_layout);

	return true;
}

// 1 when the frame is current, 0 when it lies before a seek target, -1 when it cannot be played
static int accept(struct decoder* d) {
	const AVFrame* f = d->frame;
	const AVStream* st = d->demux->streams[d->stream];
	int64_t at = d->skip_to;

	if(f->format != d->in_fmt || f->sample_rate != d->in_rate || av_channel_layout_compare(&f->ch_layout, &d->in_layout)) {
		if(!configure(d)) {
			d->failed = true;
			return -1;
		}
	}

	d->left = (uint32_t)f->nb_samples;
	d->errors = 0;
	if(d->skip_to < 0) {
		return 1;
	}

	// a seek lands on the packet before the target, the frames ahead of it are cut
	if(f->pts != AV_NOPTS_VALUE) {
		at = av_rescale_q(f->pts - (st->start_time != AV_NOPTS_VALUE ? st->start_time : 0), st->time_base,
		                  (AVRational){ 1, f->sample_rate });
	}

	if(at < d->skip_to) {
		if(d->skip_to - at >= f->nb_samples) {
			d->left = 0;
			return 0;
		}

		d->left -= (uint32_t)(d->skip_to - at);
		at = d->skip_to;
	}

	d->pos = at;
	d->skip_to = -1;

	return 1;
}

bool dec_next(struct decoder* d) {
	int r;

	if(d->left) {
		return true;
	}

	for(;;) {
		r = avcodec_receive_frame(d->codec, d->frame);
		if(r >= 0) {
			if(d->frame->nb_samples < 1) {
				continue;
			}

			r = accept(d);
			if(!r) {
				av_frame_unref(d->frame);
				continue;
			}

			return r > 0;
		}

		if(r == AVERROR_EOF) {
			return false;
		}

		if(r != AVERROR(EAGAIN)) {
			warn(d, r);
			if(++d->errors > MAX_ERRORS) {
				d->failed = true;
				return false;
			}

			continue;
		}

		if(d->flushing) {
			return false;
		}

		r = av_read_frame(d->demux, d->pkt);
		if(r < 0) {
			if(r != AVERROR_EOF) {
				diag(d, "read", r);
				d->failed = true;
			}

			d->flushing = true;
			avcodec_send_packet(d->codec, nullptr);
			continue;
		}

		if(d->pkt->stream_index == d->stream) {
			r = avcodec_send_packet(d->codec, d->pkt);
			if(r < 0) {
				warn(d, r);
			}
		}

		av_packet_unref(d->pkt);
	}
}

uint32_t dec_read(struct decoder* d, uint8_t* dst, uint32_t max) {
	const uint8_t* in[SPA_AUDIO_MAX_CHANNELS];
	uint32_t n = d->left < max ? d->left : max;
	size_t off = (size_t)((uint32_t)d->frame->nb_samples - d->left) * d->step;
	uint32_t i;
	int r;

	if(!d->swr) {
		memcpy(dst, d->frame->extended_data[0] + off, (size_t)n * d->stride);
	} else {
		for(i = 0; i < d->planes; i++) {
			in[i] = d->frame->extended_data[i] + off;
		}

		// the counts match and the rate is unchanged, so swr buffers nothing
		r = swr_convert(d->swr, &dst, (int)n, in, (int)n);
		if(r < 0) {
			warn(d, r);
			d->left = 0;
			av_frame_unref(d->frame);
			return 0;
		}

		n = (uint32_t)r;
	}

	d->left -= n;
	d->pos += n;
	if(!d->left) {
		av_frame_unref(d->frame);
	}

	return n;
}

int dec_seek(struct decoder* d, int64_t pos) {
	const AVStream* st = d->demux->streams[d->stream];
	int64_t ts = av_rescale_q(pos, (AVRational){ 1, d->in_rate }, st->time_base);
	int r;

	if(st->start_time != AV_NOPTS_VALUE) {
		ts += st->start_time;
	}

	r = av_seek_frame(d->demux, d->stream, ts, AVSEEK_FLAG_BACKWARD);
	if(r < 0) {
		diag(d, "seek", r);
		return -1;
	}

	avcodec_flush_buffers(d->codec);
	av_frame_unref(d->frame);
	d->left = 0;
	d->flushing = false;
	d->skip_to = pos;
	d->pos = pos;

	return dec_next(d);
}

bool dec_open(struct decoder* d, const char* path) {
	const AVCodec* codec;
	unsigned i;
	int r;

	*d = (struct decoder){ .path = path, .in_fmt = AV_SAMPLE_FMT_NONE, .skip_to = -1 };
	r = avformat_open_input(&d->demux, path, nullptr, nullptr);
	if(r < 0) {
		diag(d, "open", r);
		return false;
	}

	r = avformat_find_stream_info(d->demux, nullptr);
	if(r < 0) {
		diag(d, "probe", r);
		goto fail;
	}

	r = av_find_best_stream(d->demux, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
	if(r < 0) {
		report("%s: %s", path, r == AVERROR_DECODER_NOT_FOUND ? "no decoder for the audio stream" : "no audio stream");
		goto fail;
	}

	// cover art and other streams are never read
	d->stream = r;
	for(i = 0; i < d->demux->nb_streams; i++) {
		if((int)i != d->stream) {
			d->demux->streams[i]->discard = AVDISCARD_ALL;
		}
	}

	d->codec = avcodec_alloc_context3(codec);
	d->pkt = av_packet_alloc();
	d->frame = av_frame_alloc();
	if(!d->codec || !d->pkt || !d->frame) {
		diag(d, "decode", AVERROR(ENOMEM));
		goto fail;
	}

	r = avcodec_parameters_to_context(d->codec, d->demux->streams[d->stream]->codecpar);
	if(r >= 0) {
		// the decoder trims encoder delay and padding with this, which makes albums gapless
		d->codec->pkt_timebase = d->demux->streams[d->stream]->time_base;
		r = avcodec_open2(d->codec, codec, nullptr);
	}

	if(r < 0) {
		diag(d, "decode", r);
		goto fail;
	}

	if(dec_next(d)) {
		return true;
	}

	if(!d->failed) {
		report("%s: no audio data", path);
	}

fail:
	dec_close(d);

	return false;
}

void dec_close(struct decoder* d) {
	swr_free(&d->swr);
	av_frame_free(&d->frame);
	av_packet_free(&d->pkt);
	avcodec_free_context(&d->codec);
	avformat_close_input(&d->demux);
	av_channel_layout_uninit(&d->in_layout);
}
