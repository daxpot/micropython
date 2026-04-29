// PSRAM-aware allocators. quirc.c is compiled with
// -Dmalloc=qrscan_malloc -Dcalloc=qrscan_calloc -Dfree=qrscan_free
// so its working buffers (image, pixels, flood-fill vars) end up in PSRAM
// when available, falling back to internal heap otherwise.

#include "qrscan_alloc.h"

#include <string.h>
#include "esp_heap_caps.h"

void *qrscan_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    }
    return p;
}

void *qrscan_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = qrscan_malloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

void qrscan_free(void *ptr) {
    if (ptr != NULL) {
        heap_caps_free(ptr);
    }
}
