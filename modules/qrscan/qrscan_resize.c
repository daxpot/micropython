#include "qrscan_resize.h"

void qrscan_downsample_gray(const uint8_t *src, int src_w, int src_h,
                            uint8_t *dst, int dst_w, int dst_h) {
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    for (int dy = 0; dy < dst_h; dy++) {
        int y0 = dy * src_h / dst_h;
        int y1 = (dy + 1) * src_h / dst_h;
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        if (y1 > src_h) {
            y1 = src_h;
        }
        for (int dx = 0; dx < dst_w; dx++) {
            int x0 = dx * src_w / dst_w;
            int x1 = (dx + 1) * src_w / dst_w;
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            if (x1 > src_w) {
                x1 = src_w;
            }
            uint32_t sum = 0;
            uint32_t count = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint8_t *row = src + sy * src_w;
                for (int sx = x0; sx < x1; sx++) {
                    sum += row[sx];
                    count++;
                }
            }
            dst[dy * dst_w + dx] = count ? (uint8_t)(sum / count) : 0;
        }
    }
}
