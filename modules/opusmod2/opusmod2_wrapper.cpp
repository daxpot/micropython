/*
 * opusmod2_wrapper.cpp
 *
 * C-compatible wrapper implementation for esp-opus-encoder.
 * 
 * Encoder/Decoder: Use the raw opus.h C API directly from 78/esp-opus,
 * which gives us full control (bitrate, complexity, DTX, etc.) while
 * still benefiting from the Xtensa LX7 optimizations in esp-opus.
 *
 * Resampler: Uses OpusResampler C++ class from esp-opus-encoder,
 * which wraps the internal SILK resampler.
 */

#include "opusmod2_wrapper.h"
#include "opus.h"
#include "opus_resampler.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>

/* ========== Encoder ========== */

struct opusmod2_encoder {
    OpusEncoder *opus_enc;
    int sample_rate;
    int channels;
    int duration_ms;
    int frame_size; // total int16_t samples per frame = (sample_rate/1000) * duration_ms * channels
};

extern "C" opusmod2_encoder_t *opusmod2_encoder_create(int sample_rate, int channels, int duration_ms) {
    opusmod2_encoder_t *enc = (opusmod2_encoder_t *)calloc(1, sizeof(opusmod2_encoder_t));
    if (!enc) return nullptr;

    int error;
    enc->opus_enc = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &error);
    if (enc->opus_enc == nullptr || error != OPUS_OK) {
        free(enc);
        return nullptr;
    }

    enc->sample_rate = sample_rate;
    enc->channels = channels;
    enc->duration_ms = duration_ms;
    enc->frame_size = (sample_rate / 1000) * duration_ms * channels;

    // ESP32-friendly defaults
    opus_encoder_ctl(enc->opus_enc, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(enc->opus_enc, OPUS_SET_DTX(1));

    return enc;
}

extern "C" void opusmod2_encoder_destroy(opusmod2_encoder_t *enc) {
    if (enc) {
        if (enc->opus_enc) {
            opus_encoder_destroy(enc->opus_enc);
        }
        free(enc);
    }
}

extern "C" int opusmod2_encoder_encode(opusmod2_encoder_t *enc,
                                       const int16_t *pcm, int pcm_samples,
                                       uint8_t *opus_out, int max_out_bytes) {
    if (!enc || !enc->opus_enc || !pcm || !opus_out) return -1;

    // frame_size for opus_encode is per-channel samples
    int frame_samples = pcm_samples / enc->channels;

    int nbytes = opus_encode(enc->opus_enc, pcm, frame_samples, opus_out, max_out_bytes);
    return nbytes; // negative on error, positive = bytes written
}

extern "C" void opusmod2_encoder_set_complexity(opusmod2_encoder_t *enc, int complexity) {
    if (enc && enc->opus_enc) {
        opus_encoder_ctl(enc->opus_enc, OPUS_SET_COMPLEXITY(complexity));
    }
}

extern "C" void opusmod2_encoder_set_dtx(opusmod2_encoder_t *enc, bool enable) {
    if (enc && enc->opus_enc) {
        opus_encoder_ctl(enc->opus_enc, OPUS_SET_DTX(enable ? 1 : 0));
    }
}

extern "C" void opusmod2_encoder_set_bitrate(opusmod2_encoder_t *enc, int bitrate) {
    if (enc && enc->opus_enc) {
        opus_encoder_ctl(enc->opus_enc, OPUS_SET_BITRATE(bitrate));
    }
}

extern "C" void opusmod2_encoder_reset(opusmod2_encoder_t *enc) {
    if (enc && enc->opus_enc) {
        opus_encoder_ctl(enc->opus_enc, OPUS_RESET_STATE);
    }
}

extern "C" int opusmod2_encoder_get_frame_size(opusmod2_encoder_t *enc) {
    return enc ? enc->frame_size : 0;
}

extern "C" int opusmod2_encoder_get_sample_rate(opusmod2_encoder_t *enc) {
    return enc ? enc->sample_rate : 0;
}

extern "C" int opusmod2_encoder_get_duration_ms(opusmod2_encoder_t *enc) {
    return enc ? enc->duration_ms : 0;
}

/* ========== Decoder ========== */

struct opusmod2_decoder {
    OpusDecoder *opus_dec;
    int sample_rate;
    int channels;
    int duration_ms;
    int frame_size;
};

extern "C" opusmod2_decoder_t *opusmod2_decoder_create(int sample_rate, int channels, int duration_ms) {
    opusmod2_decoder_t *dec = (opusmod2_decoder_t *)calloc(1, sizeof(opusmod2_decoder_t));
    if (!dec) return nullptr;

    int error;
    dec->opus_dec = opus_decoder_create(sample_rate, channels, &error);
    if (dec->opus_dec == nullptr || error != OPUS_OK) {
        free(dec);
        return nullptr;
    }

    dec->sample_rate = sample_rate;
    dec->channels = channels;
    dec->duration_ms = duration_ms;
    dec->frame_size = (sample_rate / 1000) * duration_ms * channels;

    return dec;
}

extern "C" void opusmod2_decoder_destroy(opusmod2_decoder_t *dec) {
    if (dec) {
        if (dec->opus_dec) {
            opus_decoder_destroy(dec->opus_dec);
        }
        free(dec);
    }
}

extern "C" int opusmod2_decoder_decode(opusmod2_decoder_t *dec,
                                       const uint8_t *opus_data, int opus_len,
                                       int16_t *pcm_out, int max_samples) {
    if (!dec || !dec->opus_dec || !opus_data || !pcm_out) return -1;

    // max_frame_size for opus_decode is per-channel samples
    int max_frame_samples = max_samples / dec->channels;

    int decoded = opus_decode(dec->opus_dec, opus_data, opus_len,
                              pcm_out, max_frame_samples, 0);
    if (decoded < 0) return decoded;

    // Return total int16_t samples (decoded * channels)
    return decoded * dec->channels;
}

extern "C" void opusmod2_decoder_reset(opusmod2_decoder_t *dec) {
    if (dec && dec->opus_dec) {
        opus_decoder_ctl(dec->opus_dec, OPUS_RESET_STATE);
    }
}

extern "C" int opusmod2_decoder_get_frame_size(opusmod2_decoder_t *dec) {
    return dec ? dec->frame_size : 0;
}

/* ========== Resampler ========== */
/* Uses the C++ OpusResampler wrapper from esp-opus-encoder */

struct opusmod2_resampler {
    OpusResampler *resampler;
    int input_sample_rate;
    int output_sample_rate;
};

extern "C" opusmod2_resampler_t *opusmod2_resampler_create(int input_sample_rate, int output_sample_rate) {
    opusmod2_resampler_t *res = (opusmod2_resampler_t *)calloc(1, sizeof(opusmod2_resampler_t));
    if (!res) return nullptr;

    res->resampler = new (std::nothrow) OpusResampler();
    if (!res->resampler) {
        free(res);
        return nullptr;
    }

    res->resampler->Configure(input_sample_rate, output_sample_rate);
    res->input_sample_rate = input_sample_rate;
    res->output_sample_rate = output_sample_rate;

    return res;
}

extern "C" void opusmod2_resampler_destroy(opusmod2_resampler_t *res) {
    if (res) {
        delete res->resampler;
        free(res);
    }
}

extern "C" int opusmod2_resampler_process(opusmod2_resampler_t *res,
                                          const int16_t *input, int input_samples,
                                          int16_t *output, int max_out_samples) {
    if (!res || !res->resampler || !input || !output) return -1;

    int expected_out = res->resampler->GetOutputSamples(input_samples);
    if (expected_out > max_out_samples) return -2; // buffer too small

    res->resampler->Process(input, input_samples, output);
    return expected_out;
}

extern "C" int opusmod2_resampler_get_output_samples(opusmod2_resampler_t *res, int input_samples) {
    if (!res || !res->resampler) return -1;
    return res->resampler->GetOutputSamples(input_samples);
}
