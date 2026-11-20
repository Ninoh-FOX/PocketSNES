/* SMOOTH scaler (256x224 -> 320x240 and 256x239 -> 320x239), bit-exact with
 * upscale_*_bilinearish() in scaler.cpp, but:
 *  - NEON: 8 RGB565 pixels per operation;
 *  - every source row is interpolated horizontally once (the original redid
 *    both horizontal weights for each blended output row);
 *  - the destination (the framebuffer, uncached for reads on the Miyoo Mini)
 *    is only written, never read;
 *  - the source pitch is a parameter (the core buffer is 512 pixels wide).
 */
#include <stdint.h>
#include <string.h>
#include "scaler_fast.h"

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#define SW 256
#define DW 320

/* --- scalar reference math (same macros as scaler.cpp) --- */
#define Half(A) (((A) >> 1) & 0x7BEF)
#define Quarter(A) (((A) >> 2) & 0x39E7)
#define RestHalf(A) ((A) & 0x0821)
#define RestQuarter(A) ((A) & 0x1863)
#define Corr1_3(A, B)     Quarter(RestQuarter(A) + (RestHalf(B) << 1) + RestQuarter(B))
#define Corr3_1(A, B)     Quarter((RestHalf(A) << 1) + RestQuarter(A) + RestQuarter(B))
#define Corr1_1(A, B)     ((A) & (B) & 0x0821)
#define Weight1_3(A, B)   (Quarter(A) + Half(B) + Quarter(B) + Corr1_3(A, B))
#define Weight3_1(A, B)   (Half(A) + Quarter(A) + Quarter(B) + Corr3_1(A, B))
#define Weight1_1(A, B)   (Half(A) + Half(B) + Corr1_1(A, B))

enum { W13, W11, W31 };

#ifdef __ARM_NEON
static inline uint16x8_t vhalf(uint16x8_t a)    { return vandq_u16(vshrq_n_u16(a, 1), vdupq_n_u16(0x7BEF)); }
static inline uint16x8_t vquarter(uint16x8_t a) { return vandq_u16(vshrq_n_u16(a, 2), vdupq_n_u16(0x39E7)); }
static inline uint16x8_t vrh(uint16x8_t a)      { return vandq_u16(a, vdupq_n_u16(0x0821)); }
static inline uint16x8_t vrq(uint16x8_t a)      { return vandq_u16(a, vdupq_n_u16(0x1863)); }

static inline uint16x8_t vw13(uint16x8_t a, uint16x8_t b)
{
	uint16x8_t corr = vquarter(vaddq_u16(vaddq_u16(vrq(a), vshlq_n_u16(vrh(b), 1)), vrq(b)));
	return vaddq_u16(vaddq_u16(vquarter(a), vhalf(b)), vaddq_u16(vquarter(b), corr));
}
static inline uint16x8_t vw31(uint16x8_t a, uint16x8_t b)
{
	uint16x8_t corr = vquarter(vaddq_u16(vaddq_u16(vshlq_n_u16(vrh(a), 1), vrq(a)), vrq(b)));
	return vaddq_u16(vaddq_u16(vhalf(a), vquarter(a)), vaddq_u16(vquarter(b), corr));
}
static inline uint16x8_t vw11(uint16x8_t a, uint16x8_t b)
{
	return vaddq_u16(vaddq_u16(vhalf(a), vhalf(b)), vandq_u16(vandq_u16(a, b), vdupq_n_u16(0x0821)));
}
#endif

/* 256 -> 320: (a b c d) -> (a, W13(a,b), W11(b,c), W31(c,d), d) */
static void hrow(uint16_t *out, const uint16_t *in)
{
	int g = 0;
#ifdef __ARM_NEON
	for (; g < SW / 4; g += 8)
	{
		uint16x8x4_t s = vld4q_u16(in + g * 4);
		uint16x8x4_t t;
		uint16_t tmp[32];
		int i;
		t.val[0] = s.val[0];
		t.val[1] = vw13(s.val[0], s.val[1]);
		t.val[2] = vw11(s.val[1], s.val[2]);
		t.val[3] = vw31(s.val[2], s.val[3]);
		vst4q_u16(tmp, t);
		for (i = 0; i < 8; i++)
		{
			uint16_t *o = out + (g + i) * 5;
			memcpy(o, tmp + i * 4, 8);
			o[4] = in[(g + i) * 4 + 3];
		}
	}
#endif
	for (; g < SW / 4; g++)
	{
		uint16_t a = in[g * 4], b = in[g * 4 + 1], c = in[g * 4 + 2], d = in[g * 4 + 3];
		uint16_t *o = out + g * 5;
		o[0] = a;
		o[1] = Weight1_3(a, b);
		o[2] = Weight1_1(b, c);
		o[3] = Weight3_1(c, d);
		o[4] = d;
	}
}

static void vmix(uint16_t *dst, const uint16_t *a, const uint16_t *b, int w)
{
	int x = 0;
#ifdef __ARM_NEON
	for (; x + 8 <= DW; x += 8)
	{
		uint16x8_t va = vld1q_u16(a + x), vb = vld1q_u16(b + x), r;
		r = w == W13 ? vw13(va, vb) : w == W11 ? vw11(va, vb) : vw31(va, vb);
		vst1q_u16(dst + x, r);
	}
#endif
	for (; x < DW; x++)
		dst[x] = w == W13 ? Weight1_3(a[x], b[x]) : w == W11 ? Weight1_1(a[x], b[x]) : Weight3_1(a[x], b[x]);
}

void scale_smooth_224(uint16_t *dst, int dpitch, const uint16_t *src, int spitch)
{
	/* 16 source rows -> 17 destination rows; H(r) kept in RAM */
	static uint16_t h[2][DW];
	int by, r;

	for (by = 0; by < 14; by++)
	{
		const uint16_t *s = src + by * 16 * spitch;
		uint16_t *d = dst + by * 17 * dpitch;
		uint16_t *prev = h[0], *cur = h[1], *t;

		for (r = 0; r < 16; r++)
		{
			hrow(cur, s + r * spitch);
			/* output row(s) produced once source row r is known */
			if (r <= 2)
				memcpy(d + r * dpitch, cur, DW * 2);
			else if (r <= 7)
				vmix(d + r * dpitch, prev, cur, W13);
			else if (r == 8)
				vmix(d + r * dpitch, prev, cur, W11);
			else if (r <= 13)
			{
				vmix(d + r * dpitch, prev, cur, W31);
				if (r == 13)
					memcpy(d + 14 * dpitch, cur, DW * 2);
			}
			else
				memcpy(d + (r + 1) * dpitch, cur, DW * 2);
			t = prev; prev = cur; cur = t;
		}
	}
}

void scale_smooth_239(uint16_t *dst, int dpitch, const uint16_t *src, int spitch)
{
	static uint16_t h[DW];
	int y;
	for (y = 0; y < 239; y++)
	{
		hrow(h, src + y * spitch);
		memcpy(dst + y * dpitch, h, DW * 2);
	}
}
