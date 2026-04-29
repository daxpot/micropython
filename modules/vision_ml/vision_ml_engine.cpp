/*
 * vision_ml_engine.cpp
 *
 * TFLite Micro engine for the MobileNetV1 0.25_224 backbone, truncated at
 * conv_pw_13_relu. Output [1,7,7,256] is global-average-pooled to a
 * 256-dim float32 feature vector, which the head then classifies.
 *
 * The backbone .tflite blob lives in the `ml_model` raw partition
 * (subtype 0x40 @ 0xF00000, 1MB). We mmap it directly so the flatbuffer
 * stays in flash and only the tensor arena occupies PSRAM.
 */

#include "vision_ml_engine.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "vision_ml";

#define ML_MODEL_PARTITION_NAME    "ml_model"
#define ML_MODEL_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x40)
#define TENSOR_ARENA_SIZE          (1024 * 1024)   /* 1MB in PSRAM */
#define ML_OP_COUNT                12

static const tflite::Model *s_model = nullptr;
static tflite::MicroInterpreter *s_interpreter = nullptr;
static uint8_t *s_arena = nullptr;
static esp_partition_mmap_handle_t s_mmap_handle = 0;
static const esp_partition_t *s_partition = nullptr;
static bool s_initialized = false;

/* The interpreter and resolver have non-trivial ctors and we don't want to
 * use placement-new in C++ here; declare statically with std::aligned_storage
 * pattern. Simpler: use static-storage objects, constructed lazily via
 * placement-new on first init, destroyed on deinit. */
alignas(tflite::MicroMutableOpResolver<ML_OP_COUNT>)
static uint8_t s_resolver_storage[sizeof(tflite::MicroMutableOpResolver<ML_OP_COUNT>)];
alignas(tflite::MicroInterpreter)
static uint8_t s_interp_storage[sizeof(tflite::MicroInterpreter)];

static void destroy_interpreter(void) {
    if (s_interpreter) {
        s_interpreter->~MicroInterpreter();
        s_interpreter = nullptr;
    }
}

extern "C" int vision_ml_engine_init(void) {
    if (s_initialized) {
        return 0;
    }

    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                            ML_MODEL_PARTITION_SUBTYPE,
                                            ML_MODEL_PARTITION_NAME);
    if (!s_partition) {
        ESP_LOGE(TAG, "partition '%s' (subtype 0x40) not found", ML_MODEL_PARTITION_NAME);
        return -1;
    }

    const void *mapped = nullptr;
    esp_err_t err = esp_partition_mmap(s_partition, 0, s_partition->size,
                                        ESP_PARTITION_MMAP_DATA, &mapped, &s_mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %d", err);
        return -2;
    }

    s_model = tflite::GetModel(mapped);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema version %lu != %d",
                 (unsigned long)s_model->version(), TFLITE_SCHEMA_VERSION);
        esp_partition_munmap(s_mmap_handle);
        s_mmap_handle = 0;
        return -3;
    }

    if (!s_arena) {
        s_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_SIZE,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_arena) {
            ESP_LOGE(TAG, "tensor arena PSRAM alloc (%d bytes) failed", TENSOR_ARENA_SIZE);
            esp_partition_munmap(s_mmap_handle);
            s_mmap_handle = 0;
            return -4;
        }
    }

    auto *resolver = new (s_resolver_storage)
        tflite::MicroMutableOpResolver<ML_OP_COUNT>();
    resolver->AddConv2D();
    resolver->AddDepthwiseConv2D();
    resolver->AddPad();
    resolver->AddPadV2();
    resolver->AddReshape();
    resolver->AddRelu();
    resolver->AddRelu6();
    resolver->AddAdd();
    resolver->AddMean();
    resolver->AddQuantize();
    resolver->AddDequantize();
    resolver->AddSoftmax();

    s_interpreter = new (s_interp_storage)
        tflite::MicroInterpreter(s_model, *resolver, s_arena, TENSOR_ARENA_SIZE);

    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors() failed");
        destroy_interpreter();
        return -5;
    }

    TfLiteTensor *in = s_interpreter->input(0);
    TfLiteTensor *out = s_interpreter->output(0);
    if (in->dims->size != 4 ||
        in->dims->data[1] != VISION_ML_INPUT_SIZE ||
        in->dims->data[2] != VISION_ML_INPUT_SIZE ||
        in->dims->data[3] != 3) {
        ESP_LOGE(TAG, "unexpected input shape");
        destroy_interpreter();
        return -6;
    }
    if (out->dims->size != 4 ||
        out->dims->data[1] != 7 || out->dims->data[2] != 7 ||
        out->dims->data[3] != VISION_ML_FEATURE_DIM) {
        ESP_LOGE(TAG, "unexpected output shape: [%d,%d,%d,%d], expected [1,7,7,%d]",
                 out->dims->data[0], out->dims->data[1],
                 out->dims->data[2], out->dims->data[3],
                 VISION_ML_FEATURE_DIM);
        destroy_interpreter();
        return -6;
    }

    ESP_LOGI(TAG, "engine init OK; arena_used=%u/%u",
             (unsigned)s_interpreter->arena_used_bytes(),
             (unsigned)TENSOR_ARENA_SIZE);

    s_initialized = true;
    return 0;
}

extern "C" int vision_ml_engine_extract_features(const uint8_t *rgb888,
                                                  int rgb_len,
                                                  float *features_out_256) {
    if (!s_initialized) return -1;
    if (rgb_len != VISION_ML_INPUT_BYTES) return -2;

    TfLiteTensor *input = s_interpreter->input(0);
    if (input->type != kTfLiteInt8) return -3;

    const float in_scale = input->params.scale;
    const int in_zp = input->params.zero_point;
    int8_t *qbuf = input->data.int8;

    /* (pixel/127.5 - 1) -> quantize to int8: q = round(x/scale) + zp */
    const float inv_scale = 1.0f / in_scale;
    for (int i = 0; i < rgb_len; i++) {
        float x = (float)rgb888[i] * (1.0f / 127.5f) - 1.0f;
        int v = (int)lroundf(x * inv_scale) + in_zp;
        if (v < -128) v = -128;
        else if (v > 127) v = 127;
        qbuf[i] = (int8_t)v;
    }

    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke() failed");
        return -4;
    }

    TfLiteTensor *output = s_interpreter->output(0);
    if (output->type != kTfLiteInt8) return -5;

    const float out_scale = output->params.scale;
    const int out_zp = output->params.zero_point;
    const int8_t *od = output->data.int8;

    /* Layout NHWC: 1*7*7*256, GAP across spatial dims */
    for (int c = 0; c < VISION_ML_FEATURE_DIM; c++) {
        features_out_256[c] = 0.0f;
    }
    for (int hw = 0; hw < 49; hw++) {
        const int8_t *row = od + hw * VISION_ML_FEATURE_DIM;
        for (int c = 0; c < VISION_ML_FEATURE_DIM; c++) {
            features_out_256[c] += (float)row[c];
        }
    }
    const float gap_scale = out_scale / 49.0f;
    const float zp_offset = (float)out_zp * out_scale;
    for (int c = 0; c < VISION_ML_FEATURE_DIM; c++) {
        features_out_256[c] = features_out_256[c] * gap_scale - zp_offset;
    }

    return 0;
}

extern "C" void vision_ml_engine_deinit(void) {
    if (!s_initialized) return;
    destroy_interpreter();
    if (s_arena) {
        heap_caps_free(s_arena);
        s_arena = nullptr;
    }
    if (s_mmap_handle) {
        esp_partition_munmap(s_mmap_handle);
        s_mmap_handle = 0;
    }
    s_model = nullptr;
    s_partition = nullptr;
    s_initialized = false;
}

extern "C" bool vision_ml_engine_initialized(void) { return s_initialized; }

extern "C" size_t vision_ml_engine_arena_used(void) {
    return s_interpreter ? s_interpreter->arena_used_bytes() : 0;
}

extern "C" size_t vision_ml_engine_arena_size(void) {
    return s_initialized ? TENSOR_ARENA_SIZE : 0;
}
