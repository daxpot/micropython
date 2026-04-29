/*
 * vision_ml_manifest.h
 * Parse manifest.json exported by the lerobot-web ImageClassification page.
 */

#ifndef VISION_ML_MANIFEST_H
#define VISION_ML_MANIFEST_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VISION_ML_MAX_CLASSES 32
#define VISION_ML_MAX_LABEL_LEN 64

typedef struct {
    int version;
    char backbone_name[64];   /* must be "mobilenet_v1_0.25_224" */
    int input_size;           /* must be 224 */
    int feature_dim;          /* must be 256 */

    int hidden_dim;           /* fc1 out (e.g. 128) */
    int n_classes;            /* fc2 out */

    /* class labels - allocated; caller frees via vision_ml_manifest_free */
    int n_labels;
    char **labels;            /* labels[0..n_labels-1], each malloc'd UTF-8 */
} vision_ml_manifest_t;

typedef enum {
    VML_MAN_OK = 0,
    VML_MAN_ERR_PARSE = -1,
    VML_MAN_ERR_VERSION = -2,
    VML_MAN_ERR_BACKBONE = -3,
    VML_MAN_ERR_DIMS = -4,
    VML_MAN_ERR_NOMEM = -5,
} vision_ml_manifest_err_t;

/* Parse a JSON string into manifest. On success, caller must call _free. */
vision_ml_manifest_err_t vision_ml_manifest_parse(const char *json_text,
                                                    vision_ml_manifest_t *out);

void vision_ml_manifest_free(vision_ml_manifest_t *m);

#ifdef __cplusplus
}
#endif

#endif /* VISION_ML_MANIFEST_H */
