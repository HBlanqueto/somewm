/* dominant_color.h - dependency-free dominant-color histogram core.
 *
 * stb-style single header: define DOMINANT_COLOR_IMPLEMENTATION in exactly one
 * translation unit before including to emit the implementation. The declaration
 * part uses only C standard headers (stdint.h, stdbool.h, stddef.h, string.h);
 * no wlroots, cairo, pixman, glib or Lua.
 *
 * INPUT CONTRACT
 *   The image is cairo ARGB32: one native-endian uint32 per pixel,
 *   premultiplied alpha, arbitrary row stride measured in uint32s. Pixels are
 *   read as uint32, never as bytes. alpha = (px >> 24), r = (px >> 16), etc.
 *
 * ALGORITHM
 *   Pass 1 quantizes each un-premultiplied pixel into a `bits`-per-channel
 *   histogram (bits 3..5) and finds the winning bin (most votes; ties go to
 *   the lowest bin index). Pass 2 returns the most frequent EXACT color among
 *   the winning bin's pixels, so a flat background with antialiased text comes
 *   back exact; an open-addressing table holds the exact-color tallies and the
 *   core falls back to the winning-bin mean when the table fills.
 *
 *   Pixels whose alpha is below `min_alpha` do not vote, matching the old
 *   Lua/GdkPixbuf tally (a fully transparent snapshot must not read as black).
 *
 * SCRATCH
 *   No heap allocation per call. The histogram is sized for bits = 5
 *   (2^15 bins, 128 KiB) and the exact-color table is fixed (32 KiB), both as
 *   static buffers. This is safe on the compositor's single main thread; the
 *   core must never be called from more than one thread at a time.
 *
 * License: MIT.
 */
#ifndef DOMINANT_COLOR_H
#define DOMINANT_COLOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct dominant_color_opts {
	int bits;      /* histogram bits per channel: 3..5; 0 = default 4 */
	int step_x;    /* horizontal sample step, >= 1; 0 = default 1 */
	int step_y;    /* vertical sample step, >= 1; 0 = default 1 */
	int min_alpha; /* alpha floor, 0..255; <= 0 = default 32 */
};

struct dominant_color_result {
	uint8_t r, g, b; /* straight (un-premultiplied) color */
	double share;    /* winning-bin votes / voters, 0..1 */
	uint32_t voters; /* pixels that voted (alpha >= min_alpha) */
	uint32_t checked; /* pixels examined (after step_x/step_y) */
};

/* Computes the dominant color of an ARGB32 (premultiplied) image.
 * `width`/`height` bound the region; `stride_px` is the row stride in
 * uint32s. Returns 1 and fills *out on success, 0 when nothing voted. */
int dominant_color(const uint32_t *pixels, int width, int height, int stride_px,
                   const struct dominant_color_opts *opts,
                   struct dominant_color_result *out);

#ifdef DOMINANT_COLOR_IMPLEMENTATION

#define DC_MAX_BINS (1u << 15) /* bits = 5: 2^(3*5) bins */
#define DC_HASH_SIZE 8192u

static uint32_t dc_hist[DC_MAX_BINS];
static uint32_t dc_exact_color[DC_HASH_SIZE];
static uint32_t dc_exact_count[DC_HASH_SIZE];

static uint32_t
dc_hash(uint32_t c)
{
	return (uint32_t)((uint64_t)c * 2654435761u) & (DC_HASH_SIZE - 1u);
}

/* Un-premultiply one premultiplied ARGB pixel; caller guarantees a > 0. */
static void
dc_unpremultiply(uint32_t px, uint32_t *r, uint32_t *g, uint32_t *b)
{
	uint32_t a = (px >> 24) & 0xffu;
	uint32_t pr = (px >> 16) & 0xffu;
	uint32_t pg = (px >> 8) & 0xffu;
	uint32_t pb = px & 0xffu;
	*r = (pr * 255u + a / 2u) / a;
	*g = (pg * 255u + a / 2u) / a;
	*b = (pb * 255u + a / 2u) / a;
	if (*r > 255u) *r = 255u;
	if (*g > 255u) *g = 255u;
	if (*b > 255u) *b = 255u;
}

int
dominant_color(const uint32_t *pixels, int width, int height, int stride_px,
               const struct dominant_color_opts *opts,
               struct dominant_color_result *out)
{
	int bits = opts && opts->bits != 0 ? opts->bits : 4;
	int step_x = opts && opts->step_x != 0 ? opts->step_x : 1;
	int step_y = opts && opts->step_y != 0 ? opts->step_y : 1;
	int min_alpha = opts && opts->min_alpha > 0 ? opts->min_alpha : 32;
	uint32_t nbins, shift2, shift1;
	uint32_t win, voters, checked;
	uint32_t i;
	bool table_full;
	uint32_t table_used;

	if (bits < 3) bits = 3;
	if (bits > 5) bits = 5;
	if (step_x < 1) step_x = 1;
	if (step_y < 1) step_y = 1;
	if (min_alpha < 0) min_alpha = 0;
	if (min_alpha > 255) min_alpha = 255;

	if (!pixels || width <= 0 || height <= 0 || !out)
		return 0;

	nbins = 1u << (3 * bits);
	shift2 = 2 * bits;
	shift1 = bits;

	/* Only zero the bins this call can touch, so the common bits=4 case
	 * zeroes 16 KiB rather than the full 128 KiB bits=5 scratch. */
	memset(dc_hist, 0, nbins * sizeof(dc_hist[0]));
	memset(dc_exact_count, 0, sizeof(dc_exact_count));

	/* Pass 1: quantized histogram. */
	voters = 0;
	checked = 0;
	for (int y = 0; y < height; y += step_y) {
		const uint32_t *row = pixels + (size_t)y * stride_px;
		for (int x = 0; x < width; x += step_x) {
			uint32_t px = row[x];
			uint32_t a, r, g, b, idx;
			checked++;
			a = (px >> 24) & 0xffu;
			/* Fully transparent pixels carry no visible color regardless of
			 * min_alpha; skip them before the un-premultiply division. */
			if (a == 0u || a < (uint32_t)min_alpha)
				continue;
			dc_unpremultiply(px, &r, &g, &b);
			idx = ((r >> (8 - bits)) << shift2) |
			      ((g >> (8 - bits)) << shift1) |
			      ((b >> (8 - bits)));
			dc_hist[idx]++;
			voters++;
		}
	}
	if (voters == 0)
		return 0;

	/* Winning bin; strictly-greater keeps the lowest index on ties. */
	win = 0;
	for (i = 1; i < nbins; i++)
		if (dc_hist[i] > dc_hist[win])
			win = i;

	/* Pass 2: most frequent exact color inside the winning bin. */
	{
		uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
		uint32_t in_bin = 0;
		table_full = false;
		table_used = 0;
		for (int y = 0; y < height; y += step_y) {
			const uint32_t *row = pixels + (size_t)y * stride_px;
			for (int x = 0; x < width; x += step_x) {
				uint32_t px = row[x];
				uint32_t a, r, g, b, idx, c, slot;
				a = (px >> 24) & 0xffu;
				if (a == 0u || a < (uint32_t)min_alpha)
					continue;
				dc_unpremultiply(px, &r, &g, &b);
				idx = ((r >> (8 - bits)) << shift2) |
				      ((g >> (8 - bits)) << shift1) |
				      ((b >> (8 - bits)));
				if (idx != win)
					continue;
				sum_r += r;
				sum_g += g;
				sum_b += b;
				in_bin++;
				if (table_full)
					continue;
				c = (r << 16) | (g << 8) | b;
				slot = dc_hash(c);
				while (dc_exact_count[slot] != 0u && dc_exact_color[slot] != c)
					slot = (slot + 1u) & (DC_HASH_SIZE - 1u);
				if (dc_exact_count[slot] == 0u) {
					if (++table_used >= DC_HASH_SIZE) {
						table_full = true;
						continue;
					}
					dc_exact_color[slot] = c;
					dc_exact_count[slot] = 1u;
				} else {
					dc_exact_count[slot]++;
				}
			}
		}

		if (!table_full) {
			uint32_t best_color = 0, best_count = 0;
			for (i = 0; i < DC_HASH_SIZE; i++) {
				if (dc_exact_count[i] != 0u && dc_exact_count[i] > best_count) {
					best_count = dc_exact_count[i];
					best_color = dc_exact_color[i];
				}
			}
			if (best_count > 0) {
				out->r = (best_color >> 16) & 0xffu;
				out->g = (best_color >> 8) & 0xffu;
				out->b = best_color & 0xffu;
			} else {
				out->r = (uint8_t)((sum_r + in_bin / 2u) / in_bin);
				out->g = (uint8_t)((sum_g + in_bin / 2u) / in_bin);
				out->b = (uint8_t)((sum_b + in_bin / 2u) / in_bin);
			}
		} else {
			/* Table filled: fall back to the winning-bin mean. */
			out->r = (uint8_t)((sum_r + in_bin / 2u) / in_bin);
			out->g = (uint8_t)((sum_g + in_bin / 2u) / in_bin);
			out->b = (uint8_t)((sum_b + in_bin / 2u) / in_bin);
		}
	}

	out->share = (double)dc_hist[win] / (double)voters;
	out->voters = voters;
	out->checked = checked;
	return 1;
}

#endif /* DOMINANT_COLOR_IMPLEMENTATION */

#endif /* DOMINANT_COLOR_H */