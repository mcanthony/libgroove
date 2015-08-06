/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "groove/groove.h"
#include "config.h"
#include "ffmpeg.hpp"
#include "util.hpp"

#include <pthread.h>

static int should_deinit_network = 0;

const char *groove_strerror(int error) {
    switch ((enum GrooveError)error) {
        case GrooveErrorNone: return "(no error)";
        case GrooveErrorNoMem: return "out of memory";
    }
    return "(invalid error)";
}

static int my_lockmgr_cb(void **mutex, enum AVLockOp op) {
    if (mutex == NULL)
        return -1;
    pthread_mutex_t *pmutex;
    switch (op) {
        case AV_LOCK_CREATE:
            pmutex = allocate<pthread_mutex_t>(1);
            *mutex = pmutex;
            return pthread_mutex_init(pmutex, NULL);
        case AV_LOCK_OBTAIN:
            pmutex = (pthread_mutex_t *) *mutex;
            return pthread_mutex_lock(pmutex);
        case AV_LOCK_RELEASE:
            pmutex = (pthread_mutex_t *) *mutex;
            return pthread_mutex_unlock(pmutex);
        case AV_LOCK_DESTROY:
            pmutex = (pthread_mutex_t *) *mutex;
            int err = pthread_mutex_destroy(pmutex);
            deallocate(pmutex);
            *mutex = NULL;
            return err;
    }
    return 0;
}

int groove_init(void) {
    av_lockmgr_register(&my_lockmgr_cb);

    srand(time(NULL));

    // register all codecs, demux and protocols
    avcodec_register_all();
    av_register_all();
    avformat_network_init();
    avfilter_register_all();

    should_deinit_network = 1;

    av_log_set_level(AV_LOG_QUIET);
    return 0;
}

void groove_finish(void) {
    if (should_deinit_network) {
        avformat_network_deinit();
        should_deinit_network = 0;
    }
}

void groove_set_logging(int level) {
    av_log_set_level(level);
}

int groove_channel_layout_count(uint64_t channel_layout) {
    return av_get_channel_layout_nb_channels(channel_layout);
}

uint64_t groove_channel_layout_default(int count) {
    return av_get_default_channel_layout(count);
}

int groove_sample_format_bytes_per_sample(enum GrooveSampleFormat format) {
    return av_get_bytes_per_sample((enum AVSampleFormat)format);
}

int groove_audio_formats_equal(const struct GrooveAudioFormat *a, const struct GrooveAudioFormat *b) {
    return (a->sample_rate    == b->sample_rate &&
            a->channel_layout == b->channel_layout &&
            a->sample_fmt     == b->sample_fmt);
}

const char *groove_version(void) {
    return GROOVE_VERSION_STRING;
}

int groove_version_major(void) {
    return GROOVE_VERSION_MAJOR;
}

int groove_version_minor(void) {
    return GROOVE_VERSION_MINOR;
}

int groove_version_patch(void) {
    return GROOVE_VERSION_PATCH;
}

static void get_rand_str(char *dest, int len) {
   static const char *alphanumeric = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
   static const int alphanumeric_len = 64;

   for(int i = 0; i < len; i += 1) {
       int index = rand() % alphanumeric_len;
       dest[i] = alphanumeric[index];
   }
}

static const char *get_file_extension(int *out_ext_len, const char *file, int file_len) {
    for (int i = file_len - 1; i >= 0; i -= 1) {
        if (file[i] == '.') {
            if (i == 0 || file[i-1] == '/') {
                *out_ext_len = 0;
                return NULL;
            } else {
                *out_ext_len = file_len - i;
                return &file[i];
            }
        } else if (file[i] == '/') {
            *out_ext_len = 0;
            return NULL;
        }
    }
    *out_ext_len = 0;
    return NULL;
}

static const char *find_str_chr_rev(const char *str, int str_len, char c) {
    for (int i = str_len - 1; i >= 0; i -= 1) {
        if (str[i] == c)
            return &str[i];
    }
    return NULL;
}

char *groove_create_rand_name(int *out_len, const char *file, int file_len) {
    static const int random_len = 16;
    static const int max_ext_len = 16;
    static const char *prefix = ".tmp";
    static const int prefix_len = 4;
    int ext_len;
    const char *ext = get_file_extension(&ext_len, file, file_len);
    if (ext_len > max_ext_len)
        ext_len = 0;

    const char *slash = find_str_chr_rev(file, file_len, '/');

    char *result;
    char *basename;
    if (!slash) {
        *out_len = prefix_len + random_len + ext_len;
        result = allocate<char>(*out_len + 1);
        if (!result)
            return NULL;
        basename = result;
    } else {
        int basename_start = slash - file + 1;
        *out_len = basename_start + prefix_len + random_len + ext_len;
        result = allocate<char>(*out_len + 1);
        if (!result)
            return NULL;
        basename = result + basename_start;
        memcpy(result, file, basename_start);
    }
    strcpy(basename, prefix);
    get_rand_str(&basename[prefix_len], random_len);
    memcpy(&basename[prefix_len+random_len], ext, ext_len);
    return result;
}
