#include "groove.h"
#include "decode.h"

#include "libavutil/opt.h"

#include <SDL/SDL.h>

static int initialized = 0;
static int initialized_sdl = 0;

static void deinit_network() {
    avformat_network_deinit();
}

int maybe_init() {
    if (initialized)
        return 0;
    initialized = 1;


    srand(time(NULL));

    // register all codecs, demux and protocols
    avcodec_register_all();
    av_register_all();
    avformat_network_init();

    atexit(deinit_network);

    av_log_set_level(AV_LOG_QUIET);
    return 0;
}

int maybe_init_sdl() {
    if (initialized_sdl)
        return 0;
    initialized_sdl = 1;

    // TODO: can we remove SDL_INIT_TIMER ?
    int flags = SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (SDL_Init(flags)) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    atexit(SDL_Quit);
    return 0;
}

// decode one audio packet and return its uncompressed size
static int audio_decode_frame(DecodeContext *decode_ctx, GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;

    AVPacket *pkt = &f->audio_pkt;
    AVCodecContext *dec = f->audio_st->codec;

    AVPacket *pkt_temp = &decode_ctx->audio_pkt_temp;
    *pkt_temp = *pkt;

    // update the audio clock with the pts if we can
    if (pkt->pts != AV_NOPTS_VALUE) {
        f->audio_clock = av_q2d(f->audio_st->time_base)*pkt->pts;
    }

    int data_size = 0;
    int n, len1, got_frame;
    int new_packet = 1;
    AVFrame *frame = decode_ctx->frame;

    // NOTE: the audio packet can contain several frames
    while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
        avcodec_get_frame_defaults(frame);
        new_packet = 0;

        len1 = avcodec_decode_audio4(dec, frame, &got_frame, pkt_temp);
        if (len1 < 0) {
            // if error, we skip the frame
            pkt_temp->size = 0;
            return -1;
        }

        pkt_temp->data += len1;
        pkt_temp->size -= len1;

        if (!got_frame) {
            // stop sending empty packets if the decoder is finished
            if (!pkt_temp->data && dec->codec->capabilities & CODEC_CAP_DELAY)
                return 0;
            continue;
        }
        data_size = av_samples_get_buffer_size(NULL, dec->channels,
                       frame->nb_samples, frame->format, 1);

        int audio_resample = frame->format     != decode_ctx->dest_sample_fmt     ||
                         frame->channel_layout != decode_ctx->dest_channel_layout ||
                         frame->sample_rate    != decode_ctx->dest_sample_rate;

        int resample_changed = frame->format     != decode_ctx->resample_sample_fmt     ||
                           frame->channel_layout != decode_ctx->resample_channel_layout ||
                           frame->sample_rate    != decode_ctx->resample_sample_rate;

        if ((!decode_ctx->avr && audio_resample) || resample_changed) {
            int ret;
            if (decode_ctx->avr) {
                avresample_close(decode_ctx->avr);
            } else if (audio_resample) {
                decode_ctx->avr = avresample_alloc_context();
                if (!decode_ctx->avr) {
                    av_log(NULL, AV_LOG_ERROR, "error allocating AVAudioResampleContext\n");
                    return -1;
                }
            }
            if (audio_resample) {
                av_opt_set_int(decode_ctx->avr, "in_channel_layout",  frame->channel_layout, 0);
                av_opt_set_int(decode_ctx->avr, "in_sample_fmt",      frame->format,         0);
                av_opt_set_int(decode_ctx->avr, "in_sample_rate",     frame->sample_rate,    0);
                av_opt_set_int(decode_ctx->avr, "out_channel_layout", decode_ctx->dest_channel_layout,    0);
                av_opt_set_int(decode_ctx->avr, "out_sample_fmt",     decode_ctx->dest_sample_fmt,        0);
                av_opt_set_int(decode_ctx->avr, "out_sample_rate",    decode_ctx->dest_sample_rate,       0);

                if ((ret = avresample_open(decode_ctx->avr)) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "error initializing libavresample\n");
                    return -1;
                }
            }
            decode_ctx->resample_sample_fmt     = frame->format;
            decode_ctx->resample_channel_layout = frame->channel_layout;
            decode_ctx->resample_sample_rate    = frame->sample_rate;
        }

        BufferList buf_list;
        if (audio_resample) {
            int osize      = av_get_bytes_per_sample(decode_ctx->dest_sample_fmt);
            int nb_samples = frame->nb_samples;

            int out_linesize;
            buf_list.size = av_samples_get_buffer_size(&out_linesize,
                                  decode_ctx->dest_channel_count, nb_samples, decode_ctx->dest_sample_fmt, 0);
            buf_list.buffer = av_malloc(buf_list.size);
            if (!buf_list.buffer) {
                av_log(NULL, AV_LOG_ERROR, "error allocating buffer: out of memory\n");
                return -1;
            }

            int out_samples = avresample_convert(decode_ctx->avr, &buf_list.buffer,
                    out_linesize, nb_samples, frame->data,
                    frame->linesize[0], frame->nb_samples);
            if (out_samples < 0) {
                av_log(NULL, AV_LOG_ERROR, "avresample_convert() failed\n");
                break;
            }
            data_size = out_samples * osize * decode_ctx->dest_channel_count;
            buf_list.size = data_size;
        } else {
            buf_list.size = data_size;
            buf_list.buffer = av_malloc(buf_list.size);
            if (!buf_list.buffer) {
                av_log(NULL, AV_LOG_ERROR, "error allocating buffer: out of memory\n");
                return -1;
            }
            memcpy(buf_list.buffer, frame->data[0], buf_list.size);
        }
        int err = decode_ctx->buffer(decode_ctx, &buf_list);
        // if no pts, then compute it
        if (pkt->pts == AV_NOPTS_VALUE) {
            n = decode_ctx->dest_channel_count * av_get_bytes_per_sample(decode_ctx->dest_sample_fmt);
            f->audio_clock += (double)data_size / (double)(n * decode_ctx->dest_sample_rate);
        }
        return err < 0 ? err : data_size;
    }
    return data_size;
}

// return < 0 if error
static int init_decode(GrooveFile *file) {
    GrooveFilePrivate *f = file->internals;

    // set all streams to discard. in a few lines here we will find the audio
    // stream and cancel discarding it
    for (int i = 0; i < f->ic->nb_streams; i++)
        f->ic->streams[i]->discard = AVDISCARD_ALL;

    f->audio_stream_index = av_find_best_stream(f->ic, AVMEDIA_TYPE_AUDIO, -1, -1, &f->decoder, 0);

    if (f->audio_stream_index < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: no audio stream found\n", f->ic->filename);
        return -1;
    }

    if (!f->decoder) {
        av_log(NULL, AV_LOG_ERROR, "%s: no decoder found\n", f->ic->filename);
        return -1;
    }

    AVCodecContext *avctx = f->ic->streams[f->audio_stream_index]->codec;

    if (avcodec_open2(avctx, f->decoder, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to open decoder\n");
        return -1;
    }

    // prepare audio output
    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);
    if (!avctx->channel_layout) {
        av_log(NULL, AV_LOG_ERROR, "unable to guess channel layout\n");
        return -1;
    }

    f->audio_st = f->ic->streams[f->audio_stream_index];
    f->audio_st->discard = AVDISCARD_DEFAULT;

    memset(&f->audio_pkt, 0, sizeof(f->audio_pkt));

    return 0;
}

int decode(DecodeContext *decode_ctx, GrooveFile *file) {
    GrooveFilePrivate * f = file->internals;
    AVPacket *pkt = &f->audio_pkt;

    // if the file has not been initialized for decoding
    if (f->audio_stream_index < 0) {
        int err = init_decode(file);
        if (err < 0)
            return err;
    }
    if (f->abort_request)
        return -1;
    if (decode_ctx->paused != decode_ctx->last_paused) {
        decode_ctx->last_paused = decode_ctx->paused;
        if (decode_ctx->paused) {
            av_read_pause(f->ic);
        } else {
            av_read_play(f->ic);
        }
    }
    if (f->seek_req) {
        AVCodecContext *dec = f->audio_st->codec;
        int64_t seek_target = f->seek_pos;
        int64_t seek_min    = f->seek_rel > 0 ? seek_target - f->seek_rel + 2: INT64_MIN;
        int64_t seek_max    = f->seek_rel < 0 ? seek_target - f->seek_rel - 2: INT64_MAX;
        // FIXME the +-2 is due to rounding being not done in the correct
        // direction in generation of the seek_pos/seek_rel variables
        int err = avformat_seek_file(f->ic, -1, seek_min, seek_target, seek_max,
                f->seek_flags);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", f->ic->filename);
        } else {
            if (decode_ctx->flush)
                decode_ctx->flush(decode_ctx);
            avcodec_flush_buffers(dec);
        }
        f->seek_req = 0;
        f->eof = 0;
    }
    if (f->eof) {
        if (f->audio_st->codec->codec->capabilities & CODEC_CAP_DELAY) {
            av_init_packet(pkt);
            pkt->data = NULL;
            pkt->size = 0;
            pkt->stream_index = f->audio_stream_index;
            if (audio_decode_frame(decode_ctx, file) > 0) {
                // keep flushing
                return 0;
            }
        }
        // this file is complete. move on
        return -1;
    }
    int err = av_read_frame(f->ic, pkt);
    if (err < 0) {
        // treat all errors as EOF, but log non-EOF errors.
        if (err != AVERROR_EOF) {
            av_log(NULL, AV_LOG_WARNING, "error reading frames\n");
        }
        f->eof = 1;
        return 0;
    }
    if (pkt->stream_index != f->audio_stream_index) {
        // we're only interested in the One True Audio Stream
        av_free_packet(pkt);
        return 0;
    }
    audio_decode_frame(decode_ctx, file);
    av_free_packet(pkt);
    return 0;
}

void cleanup_decode_ctx(DecodeContext *decode_ctx) {
    if (decode_ctx->avr)
        avresample_free(&decode_ctx->avr);

    avcodec_free_frame(&decode_ctx->frame);
}