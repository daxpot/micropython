#ifndef QRSCAN_RESIZE_H
#define QRSCAN_RESIZE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Box-filter downsample of a grayscale image.
// Each output pixel is the average of the source pixels covered by its
// rectangular footprint. Caller must ensure dst_w <= src_w and dst_h <= src_h.
void qrscan_downsample_gray(const uint8_t *src, int src_w, int src_h,
                            uint8_t *dst, int dst_w, int dst_h);

#ifdef __cplusplus
}
#endif

#endif
