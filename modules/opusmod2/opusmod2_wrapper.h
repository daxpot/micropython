/*
 * opusmod2_wrapper.h
 *
 * C-compatible wrapper for esp-opus-encoder C++ classes.
 * This allows MicroPython's C binding layer to call C++ OpusEncoderWrapper,
 * OpusDecoderWrapper, and OpusResampler without needing C++ compilation.
 */

#ifndef OPUSMOD2_WRAPPER_H
#define OPUSMOD2_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Encoder ========== */

typedef struct opusmod2_encoder opusmod2_encoder_t;

/**
 * Create an Opus encoder.
 * @param sample_rate  Input sample rate (8000/16000/24000/48000)
 * @param channels     Number of channels (1 or 2)
 * @param duration_ms  Frame duration in ms (default 60)
 * @return Encoder handle, or NULL on failure
 */
opusmod2_encoder_t *opusmod2_encoder_create(int sample_rate, int channels, int duration_ms);

/**
 * Destroy an Opus encoder and free resources.
 */
void opusmod2_encoder_destroy(opusmod2_encoder_t *enc);

/**
 * Encode a PCM frame directly.
 * Input pcm must contain exactly frame_size samples (sample_rate/1000 * duration_ms * channels).
 *
 * @param enc          Encoder handle
 * @param pcm          Input PCM samples (int16_t)
 * @param pcm_samples  Number of int16_t samples in pcm
 * @param opus_out     Output buffer for encoded data
 * @param max_out_bytes Size of opus_out buffer
 * @return Number of bytes written to opus_out, or negative on error
 */
int opusmod2_encoder_encode(opusmod2_encoder_t *enc,
                            const int16_t *pcm, int pcm_samples,
                            uint8_t *opus_out, int max_out_bytes);

/**
 * Set encoder complexity (0-10, default 0 for ESP32).
 */
void opusmod2_encoder_set_complexity(opusmod2_encoder_t *enc, int complexity);

/**
 * Enable/disable DTX (Discontinuous Transmission).
 */
void opusmod2_encoder_set_dtx(opusmod2_encoder_t *enc, bool enable);

/**
 * Set encoder bitrate in bits per second.
 */
void opusmod2_encoder_set_bitrate(opusmod2_encoder_t *enc, int bitrate);

/**
 * Reset encoder state.
 */
void opusmod2_encoder_reset(opusmod2_encoder_t *enc);

/**
 * Get the expected frame size (number of int16_t samples per frame).
 */
int opusmod2_encoder_get_frame_size(opusmod2_encoder_t *enc);

/**
 * Get the sample rate.
 */
int opusmod2_encoder_get_sample_rate(opusmod2_encoder_t *enc);

/**
 * Get the duration in ms.
 */
int opusmod2_encoder_get_duration_ms(opusmod2_encoder_t *enc);

/* ========== Decoder ========== */

typedef struct opusmod2_decoder opusmod2_decoder_t;

/**
 * Create an Opus decoder.
 * @param sample_rate  Output sample rate
 * @param channels     Number of channels (1 or 2)
 * @param duration_ms  Expected frame duration in ms (default 60)
 * @return Decoder handle, or NULL on failure
 */
opusmod2_decoder_t *opusmod2_decoder_create(int sample_rate, int channels, int duration_ms);

/**
 * Destroy an Opus decoder and free resources.
 */
void opusmod2_decoder_destroy(opusmod2_decoder_t *dec);

/**
 * Decode an Opus packet.
 * @param dec          Decoder handle
 * @param opus_data    Input Opus packet
 * @param opus_len     Length of opus_data in bytes
 * @param pcm_out      Output buffer for decoded PCM (int16_t)
 * @param max_samples  Maximum number of int16_t samples pcm_out can hold
 * @return Number of decoded samples, or negative on error
 */
int opusmod2_decoder_decode(opusmod2_decoder_t *dec,
                            const uint8_t *opus_data, int opus_len,
                            int16_t *pcm_out, int max_samples);

/**
 * Reset decoder state.
 */
void opusmod2_decoder_reset(opusmod2_decoder_t *dec);

/**
 * Get the expected frame size (samples per frame).
 */
int opusmod2_decoder_get_frame_size(opusmod2_decoder_t *dec);

/* ========== Resampler ========== */

typedef struct opusmod2_resampler opusmod2_resampler_t;

/**
 * Create a resampler (uses Opus internal SILK resampler).
 * @param input_sample_rate   Input sample rate
 * @param output_sample_rate  Output sample rate
 * @return Resampler handle, or NULL on failure
 */
opusmod2_resampler_t *opusmod2_resampler_create(int input_sample_rate, int output_sample_rate);

/**
 * Destroy a resampler.
 */
void opusmod2_resampler_destroy(opusmod2_resampler_t *res);

/**
 * Process (resample) audio samples.
 * @param res             Resampler handle
 * @param input           Input PCM samples (int16_t)
 * @param input_samples   Number of input samples
 * @param output          Output buffer for resampled PCM
 * @param max_out_samples Maximum output samples output can hold
 * @return Number of output samples written
 */
int opusmod2_resampler_process(opusmod2_resampler_t *res,
                               const int16_t *input, int input_samples,
                               int16_t *output, int max_out_samples);

/**
 * Get the expected number of output samples for a given number of input samples.
 */
int opusmod2_resampler_get_output_samples(opusmod2_resampler_t *res, int input_samples);

#ifdef __cplusplus
}
#endif

#endif /* OPUSMOD2_WRAPPER_H */
