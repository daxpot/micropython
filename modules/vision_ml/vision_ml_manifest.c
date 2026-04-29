/*
 * vision_ml_manifest.c
 * Parse manifest.json with cJSON (built into ESP-IDF).
 *
 * Validates:
 *   version == 1
 *   backbone.name == "mobilenet_v1_0.25_224"
 *   backbone.input_size == 224
 *   backbone.feature_dim == 256
 *   head.layers contains fc1 (in=256), and fc2 (out matches classes length)
 */

#include "vision_ml_manifest.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "vision_ml_engine.h"   /* for VISION_ML_FEATURE_DIM, VISION_ML_INPUT_SIZE */

#define EXPECTED_BACKBONE "mobilenet_v1_0.25_224"

void vision_ml_manifest_free(vision_ml_manifest_t *m) {
    if (!m) return;
    if (m->labels) {
        for (int i = 0; i < m->n_labels; i++) {
            free(m->labels[i]);
        }
        free(m->labels);
        m->labels = NULL;
    }
    m->n_labels = 0;
}

vision_ml_manifest_err_t vision_ml_manifest_parse(const char *json_text,
                                                    vision_ml_manifest_t *out) {
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json_text);
    if (!root) return VML_MAN_ERR_PARSE;

    vision_ml_manifest_err_t rc = VML_MAN_ERR_PARSE;

    cJSON *jv = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsNumber(jv)) goto done;
    out->version = jv->valueint;
    if (out->version != 1) { rc = VML_MAN_ERR_VERSION; goto done; }

    cJSON *jbb = cJSON_GetObjectItem(root, "backbone");
    if (!cJSON_IsObject(jbb)) goto done;
    cJSON *jbb_name = cJSON_GetObjectItem(jbb, "name");
    cJSON *jbb_in = cJSON_GetObjectItem(jbb, "input_size");
    cJSON *jbb_fd = cJSON_GetObjectItem(jbb, "feature_dim");
    if (!cJSON_IsString(jbb_name) || !cJSON_IsNumber(jbb_in) || !cJSON_IsNumber(jbb_fd)) goto done;

    strncpy(out->backbone_name, jbb_name->valuestring, sizeof(out->backbone_name) - 1);
    out->input_size = jbb_in->valueint;
    out->feature_dim = jbb_fd->valueint;

    if (strcmp(out->backbone_name, EXPECTED_BACKBONE) != 0) {
        rc = VML_MAN_ERR_BACKBONE; goto done;
    }
    if (out->input_size != VISION_ML_INPUT_SIZE ||
        out->feature_dim != VISION_ML_FEATURE_DIM) {
        rc = VML_MAN_ERR_DIMS; goto done;
    }

    /* head.layers: scan for fc1 (in==feature_dim) and fc2 */
    cJSON *jhead = cJSON_GetObjectItem(root, "head");
    if (!cJSON_IsObject(jhead)) goto done;
    cJSON *jlayers = cJSON_GetObjectItem(jhead, "layers");
    if (!cJSON_IsArray(jlayers)) goto done;

    int fc1_in = -1, fc1_out = -1, fc2_in = -1, fc2_out = -1;
    cJSON *layer = NULL;
    cJSON_ArrayForEach(layer, jlayers) {
        cJSON *jname = cJSON_GetObjectItem(layer, "name");
        cJSON *jin = cJSON_GetObjectItem(layer, "in");
        cJSON *jout = cJSON_GetObjectItem(layer, "out");
        if (!cJSON_IsString(jname) || !cJSON_IsNumber(jin) || !cJSON_IsNumber(jout)) continue;
        if (strcmp(jname->valuestring, "fc1") == 0) {
            fc1_in = jin->valueint; fc1_out = jout->valueint;
        } else if (strcmp(jname->valuestring, "fc2") == 0) {
            fc2_in = jin->valueint; fc2_out = jout->valueint;
        }
    }
    if (fc1_in != out->feature_dim || fc1_out <= 0 || fc2_in != fc1_out || fc2_out <= 0) {
        rc = VML_MAN_ERR_DIMS; goto done;
    }
    out->hidden_dim = fc1_out;
    out->n_classes = fc2_out;

    /* classes array */
    cJSON *jclasses = cJSON_GetObjectItem(root, "classes");
    if (!cJSON_IsArray(jclasses)) goto done;
    int n = cJSON_GetArraySize(jclasses);
    if (n != out->n_classes) { rc = VML_MAN_ERR_DIMS; goto done; }
    if (n > VISION_ML_MAX_CLASSES) { rc = VML_MAN_ERR_DIMS; goto done; }

    out->labels = (char **)calloc((size_t)n, sizeof(char *));
    if (!out->labels) { rc = VML_MAN_ERR_NOMEM; goto done; }
    out->n_labels = n;

    for (int i = 0; i < n; i++) {
        cJSON *jl = cJSON_GetArrayItem(jclasses, i);
        if (!cJSON_IsString(jl)) { rc = VML_MAN_ERR_PARSE; goto fail_labels; }
        size_t len = strlen(jl->valuestring);
        if (len >= VISION_ML_MAX_LABEL_LEN) len = VISION_ML_MAX_LABEL_LEN - 1;
        char *s = (char *)malloc(len + 1);
        if (!s) { rc = VML_MAN_ERR_NOMEM; goto fail_labels; }
        memcpy(s, jl->valuestring, len);
        s[len] = '\0';
        out->labels[i] = s;
    }

    rc = VML_MAN_OK;
    goto done;

fail_labels:
    vision_ml_manifest_free(out);

done:
    cJSON_Delete(root);
    return rc;
}
