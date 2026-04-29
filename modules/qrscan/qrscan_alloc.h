// PSRAM-aware allocators for quirc (used via -Dmalloc/-Dcalloc/-Dfree).
#ifndef QRSCAN_ALLOC_H
#define QRSCAN_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *qrscan_malloc(size_t size);
void *qrscan_calloc(size_t nmemb, size_t size);
void  qrscan_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
