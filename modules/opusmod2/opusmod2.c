/*
 * opusmod2.c
 *
 * MicroPython user C module: opusmod2
 * Wraps 78/esp-opus-encoder via C bridge for Opus encode/decode/resample.
 *
 * Python API:
 *   import opusmod2
 *
 *   # Encoder
 *   enc = opusmod2.Encoder(sample_rate=16000, channels=1, duration_ms=60,
 *                          complexity=0, dtx=True, bitrate=32000)
 *   opus_bytes = enc.encode(pcm_bytes)
 *   enc.set_complexity(3)
 *   enc.set_dtx(False)
 *   enc.reset()
 *   frame_size = enc.frame_size
 *
 *   # Decoder
 *   dec = opusmod2.Decoder(sample_rate=16000, channels=1, duration_ms=60)
 *   pcm_bytes, samples = dec.decode(opus_bytes)
 *   dec.reset()
 *
 *   # Resampler
 *   res = opusmod2.Resampler(input_rate=48000, output_rate=16000)
 *   out_bytes = res.process(pcm_bytes)
 */

#include "mphalport.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/binary.h"
#include "py/objstr.h"
#include "opusmod2_wrapper.h"

/* ================================================================
 * OpusEncoder2 class
 * ================================================================ */

typedef struct _opusmod2_encoder_obj_t {
    mp_obj_base_t base;
    opusmod2_encoder_t *enc;
    int sample_rate;
    int channels;
    int duration_ms;
    int frame_size; // expected int16_t samples per frame
} opusmod2_encoder_obj_t;

static mp_obj_t opusmod2_encoder_make_new(const mp_obj_type_t *type,
                                          size_t n_args, size_t n_kw,
                                          const mp_obj_t *args) {
    enum {
        ARG_sample_rate, ARG_channels, ARG_duration_ms,
        ARG_complexity, ARG_dtx, ARG_bitrate
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample_rate,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 16000} },
        { MP_QSTR_channels,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_duration_ms,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 60} },
        { MP_QSTR_complexity,   MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_dtx,          MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
        { MP_QSTR_bitrate,      MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args,
                              MP_ARRAY_SIZE(allowed_args), allowed_args, vals);

    int sample_rate  = vals[ARG_sample_rate].u_int;
    int channels     = vals[ARG_channels].u_int;
    int duration_ms  = vals[ARG_duration_ms].u_int;
    int complexity   = vals[ARG_complexity].u_int;
    bool dtx         = vals[ARG_dtx].u_bool;
    int bitrate      = vals[ARG_bitrate].u_int;

    opusmod2_encoder_obj_t *self = mp_obj_malloc(opusmod2_encoder_obj_t, type);
    self->sample_rate = sample_rate;
    self->channels = channels;
    self->duration_ms = duration_ms;

    self->enc = opusmod2_encoder_create(sample_rate, channels, duration_ms);
    if (!self->enc) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("Failed to create Opus encoder"));
    }

    self->frame_size = opusmod2_encoder_get_frame_size(self->enc);

    // Apply initial settings
    opusmod2_encoder_set_complexity(self->enc, complexity);
    opusmod2_encoder_set_dtx(self->enc, dtx);
    if (bitrate > 0) {
        opusmod2_encoder_set_bitrate(self->enc, bitrate);
    }

    return MP_OBJ_FROM_PTR(self);
}

// Destructor - prevent memory leak
static mp_obj_t opusmod2_encoder_del(mp_obj_t self_in) {
    opusmod2_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->enc) {
        opusmod2_encoder_destroy(self->enc);
        self->enc = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(opusmod2_encoder_del_obj, opusmod2_encoder_del);

// encode(pcm_bytes) -> bytes
// pcm_bytes must contain exactly frame_size int16_t samples
static mp_obj_t mp_opusmod2_encoder_encode(mp_obj_t self_in, mp_obj_t pcm_in) {
    opusmod2_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->enc) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Encoder destroyed"));
    }

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(pcm_in, &bufinfo, MP_BUFFER_READ);

    int pcm_samples = bufinfo.len / sizeof(int16_t);

    // Validate frame size
    if (pcm_samples != self->frame_size) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("PCM must be %d samples, got %d"),
            self->frame_size, pcm_samples);
    }

    // Output buffer - 1500 bytes is more than enough for any Opus frame
    uint8_t opus_buf[1500];

    int nbytes = opusmod2_encoder_encode(self->enc,
                                         (const int16_t *)bufinfo.buf, pcm_samples,
                                         opus_buf, sizeof(opus_buf));
    if (nbytes < 0) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("Opus encode failed: %d"), nbytes);
    }

    return mp_obj_new_bytes(opus_buf, nbytes);
}
static MP_DEFINE_CONST_FUN_OBJ_2(opusmod2_encoder_encode_obj, mp_opusmod2_encoder_encode);

// set_complexity(n) - 0~10
static mp_obj_t opusmod2_encoder_set_complexity_method(mp_obj_t self_in, mp_obj_t val_in) {
    opusmod2_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->enc) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Encoder destroyed"));
    }
    int complexity = mp_obj_get_int(val_in);
    if (complexity < 0 || complexity > 10) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("complexity must be 0-10"));
    }
    opusmod2_encoder_set_complexity(self->enc, complexity);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(opusmod2_encoder_set_complexity_method_obj,
                                 opusmod2_encoder_set_complexity_method);

// set_dtx(enable)
static mp_obj_t opusmod2_encoder_set_dtx_method(mp_obj_t self_in, mp_obj_t val_in) {
    opusmod2_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->enc) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Encoder destroyed"));
    }
    opusmod2_encoder_set_dtx(self->enc, mp_obj_is_true(val_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(opusmod2_encoder_set_dtx_method_obj,
                                 opusmod2_encoder_set_dtx_method);

// reset()
static mp_obj_t opusmod2_encoder_reset_method(mp_obj_t self_in) {
    opusmod2_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->enc) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Encoder destroyed"));
    }
    opusmod2_encoder_reset(self->enc);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(opusmod2_encoder_reset_method_obj,
                                 opusmod2_encoder_reset_method);

// Properties via methods
static void opusmod2_encoder_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    opusmod2_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        // Load attribute
        if (attr == MP_QSTR_frame_size) {
            dest[0] = mp_obj_new_int(self->frame_size);
        } else if (attr == MP_QSTR_sample_rate) {
            dest[0] = mp_obj_new_int(self->sample_rate);
        } else if (attr == MP_QSTR_channels) {
            dest[0] = mp_obj_new_int(self->channels);
        } else if (attr == MP_QSTR_duration_ms) {
            dest[0] = mp_obj_new_int(self->duration_ms);
        } else {
            // Delegate to locals dict for methods
            dest[1] = MP_OBJ_SENTINEL;
        }
    }
}

static const mp_rom_map_elem_t opusmod2_encoder_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__),        MP_ROM_PTR(&opusmod2_encoder_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_encode),          MP_ROM_PTR(&opusmod2_encoder_encode_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_complexity),   MP_ROM_PTR(&opusmod2_encoder_set_complexity_method_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_dtx),          MP_ROM_PTR(&opusmod2_encoder_set_dtx_method_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset),            MP_ROM_PTR(&opusmod2_encoder_reset_method_obj) },
};
static MP_DEFINE_CONST_DICT(opusmod2_encoder_locals_dict, opusmod2_encoder_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    opusmod2_encoder_type,
    MP_QSTR_Encoder,
    MP_TYPE_FLAG_NONE,
    make_new, opusmod2_encoder_make_new,
    attr, opusmod2_encoder_attr,
    locals_dict, &opusmod2_encoder_locals_dict
);

/* ================================================================
 * OpusDecoder2 class
 * ================================================================ */

typedef struct _opusmod2_decoder_obj_t {
    mp_obj_base_t base;
    opusmod2_decoder_t *dec;
    int sample_rate;
    int channels;
    int duration_ms;
    int frame_size;
} opusmod2_decoder_obj_t;

static mp_obj_t opusmod2_decoder_make_new(const mp_obj_type_t *type,
                                          size_t n_args, size_t n_kw,
                                          const mp_obj_t *args) {
    enum { ARG_sample_rate, ARG_channels, ARG_duration_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample_rate,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 16000} },
        { MP_QSTR_channels,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_duration_ms,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 60} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args,
                              MP_ARRAY_SIZE(allowed_args), allowed_args, vals);

    int sample_rate = vals[ARG_sample_rate].u_int;
    int channels    = vals[ARG_channels].u_int;
    int duration_ms = vals[ARG_duration_ms].u_int;

    opusmod2_decoder_obj_t *self = mp_obj_malloc(opusmod2_decoder_obj_t, type);
    self->sample_rate = sample_rate;
    self->channels = channels;
    self->duration_ms = duration_ms;

    self->dec = opusmod2_decoder_create(sample_rate, channels, duration_ms);
    if (!self->dec) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("Failed to create Opus decoder"));
    }

    self->frame_size = opusmod2_decoder_get_frame_size(self->dec);

    return MP_OBJ_FROM_PTR(self);
}

// Destructor
static mp_obj_t opusmod2_decoder_del(mp_obj_t self_in) {
    opusmod2_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->dec) {
        opusmod2_decoder_destroy(self->dec);
        self->dec = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(opusmod2_decoder_del_obj, opusmod2_decoder_del);

// decode(opus_bytes) -> (pcm_bytes, samples)
static mp_obj_t mp_opusmod2_decoder_decode(mp_obj_t self_in, mp_obj_t data_in) {
    opusmod2_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->dec) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Decoder destroyed"));
    }

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);

    // Allocate output buffer: max 120ms @ 48kHz stereo = 5760*2 samples
    int max_samples = 5760 * self->channels;
    int16_t *pcm = m_new(int16_t, max_samples);

    int samples = opusmod2_decoder_decode(self->dec,
                                          (const uint8_t *)bufinfo.buf, bufinfo.len,
                                          pcm, max_samples);
    if (samples < 0) {
        m_del(int16_t, pcm, max_samples);
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("Opus decode failed: %d"), samples);
    }

    size_t out_bytes = samples * sizeof(int16_t);
    mp_obj_t tuple[2];
    tuple[0] = mp_obj_new_bytes((const byte *)pcm, out_bytes);
    tuple[1] = mp_obj_new_int(samples / self->channels); // samples per channel

    m_del(int16_t, pcm, max_samples);
    return mp_obj_new_tuple(2, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_2(opusmod2_decoder_decode_obj, mp_opusmod2_decoder_decode);

// reset()
static mp_obj_t opusmod2_decoder_reset_method(mp_obj_t self_in) {
    opusmod2_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->dec) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Decoder destroyed"));
    }
    opusmod2_decoder_reset(self->dec);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(opusmod2_decoder_reset_method_obj,
                                 opusmod2_decoder_reset_method);

// Properties
static void opusmod2_decoder_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    opusmod2_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        if (attr == MP_QSTR_frame_size) {
            dest[0] = mp_obj_new_int(self->frame_size);
        } else if (attr == MP_QSTR_sample_rate) {
            dest[0] = mp_obj_new_int(self->sample_rate);
        } else if (attr == MP_QSTR_channels) {
            dest[0] = mp_obj_new_int(self->channels);
        } else if (attr == MP_QSTR_duration_ms) {
            dest[0] = mp_obj_new_int(self->duration_ms);
        } else {
            dest[1] = MP_OBJ_SENTINEL;
        }
    }
}

static const mp_rom_map_elem_t opusmod2_decoder_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&opusmod2_decoder_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_decode),  MP_ROM_PTR(&opusmod2_decoder_decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset),   MP_ROM_PTR(&opusmod2_decoder_reset_method_obj) },
};
static MP_DEFINE_CONST_DICT(opusmod2_decoder_locals_dict, opusmod2_decoder_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    opusmod2_decoder_type,
    MP_QSTR_Decoder,
    MP_TYPE_FLAG_NONE,
    make_new, opusmod2_decoder_make_new,
    attr, opusmod2_decoder_attr,
    locals_dict, &opusmod2_decoder_locals_dict
);

/* ================================================================
 * OpusResampler class
 * ================================================================ */

typedef struct _opusmod2_resampler_obj_t {
    mp_obj_base_t base;
    opusmod2_resampler_t *res;
    int input_rate;
    int output_rate;
} opusmod2_resampler_obj_t;

static mp_obj_t opusmod2_resampler_make_new(const mp_obj_type_t *type,
                                            size_t n_args, size_t n_kw,
                                            const mp_obj_t *args) {
    enum { ARG_input_rate, ARG_output_rate };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_input_rate,   MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_output_rate,  MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args,
                              MP_ARRAY_SIZE(allowed_args), allowed_args, vals);

    int input_rate  = vals[ARG_input_rate].u_int;
    int output_rate = vals[ARG_output_rate].u_int;

    opusmod2_resampler_obj_t *self = mp_obj_malloc(opusmod2_resampler_obj_t, type);
    self->input_rate = input_rate;
    self->output_rate = output_rate;

    self->res = opusmod2_resampler_create(input_rate, output_rate);
    if (!self->res) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("Failed to create resampler"));
    }

    return MP_OBJ_FROM_PTR(self);
}

// Destructor
static mp_obj_t opusmod2_resampler_del(mp_obj_t self_in) {
    opusmod2_resampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->res) {
        opusmod2_resampler_destroy(self->res);
        self->res = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(opusmod2_resampler_del_obj, opusmod2_resampler_del);

// process(pcm_bytes) -> bytes
static mp_obj_t mp_opusmod2_resampler_process(mp_obj_t self_in, mp_obj_t data_in) {
    opusmod2_resampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->res) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Resampler destroyed"));
    }

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);

    int input_samples = bufinfo.len / sizeof(int16_t);
    int output_samples = opusmod2_resampler_get_output_samples(self->res, input_samples);
    if (output_samples < 0) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("Resampler get output samples failed"));
    }

    int16_t *out = m_new(int16_t, output_samples);

    int result = opusmod2_resampler_process(self->res,
                                            (const int16_t *)bufinfo.buf, input_samples,
                                            out, output_samples);
    if (result < 0) {
        m_del(int16_t, out, output_samples);
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("Resample failed: %d"), result);
    }

    mp_obj_t ret = mp_obj_new_bytes((const byte *)out, result * sizeof(int16_t));
    m_del(int16_t, out, output_samples);
    return ret;
}
static MP_DEFINE_CONST_FUN_OBJ_2(opusmod2_resampler_process_obj, mp_opusmod2_resampler_process);

// Properties
static void opusmod2_resampler_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    opusmod2_resampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        if (attr == MP_QSTR_input_rate) {
            dest[0] = mp_obj_new_int(self->input_rate);
        } else if (attr == MP_QSTR_output_rate) {
            dest[0] = mp_obj_new_int(self->output_rate);
        } else {
            dest[1] = MP_OBJ_SENTINEL;
        }
    }
}

static const mp_rom_map_elem_t opusmod2_resampler_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__),  MP_ROM_PTR(&opusmod2_resampler_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_process),  MP_ROM_PTR(&opusmod2_resampler_process_obj) },
};
static MP_DEFINE_CONST_DICT(opusmod2_resampler_locals_dict, opusmod2_resampler_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    opusmod2_resampler_type,
    MP_QSTR_Resampler,
    MP_TYPE_FLAG_NONE,
    make_new, opusmod2_resampler_make_new,
    attr, opusmod2_resampler_attr,
    locals_dict, &opusmod2_resampler_locals_dict
);

/* ================================================================
 * Module definition
 * ================================================================ */

static const mp_rom_map_elem_t opusmod2_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),   MP_ROM_QSTR(MP_QSTR_opusmod2) },
    { MP_ROM_QSTR(MP_QSTR_Encoder),    MP_ROM_PTR(&opusmod2_encoder_type) },
    { MP_ROM_QSTR(MP_QSTR_Decoder),    MP_ROM_PTR(&opusmod2_decoder_type) },
    { MP_ROM_QSTR(MP_QSTR_Resampler),  MP_ROM_PTR(&opusmod2_resampler_type) },
};
static MP_DEFINE_CONST_DICT(opusmod2_module_globals, opusmod2_module_globals_table);

const mp_obj_module_t opusmod2_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&opusmod2_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_opusmod2, opusmod2_user_cmodule);
