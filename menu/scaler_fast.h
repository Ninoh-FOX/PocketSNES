#ifndef SCALER_FAST_H
#define SCALER_FAST_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* pitches in pixels; destination is only written */
void scale_smooth_224(uint16_t *dst, int dpitch, const uint16_t *src, int spitch); /* 256x224 -> 320x240 */
void scale_smooth_239(uint16_t *dst, int dpitch, const uint16_t *src, int spitch); /* 256x239 -> 320x239 */
#ifdef __cplusplus
}
#endif
#endif
