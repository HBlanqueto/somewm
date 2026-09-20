/* Unit tests for the dominant_color() core (dominant_color.h).
 *
 * Build via meson and run under `meson test` (see meson.build). The test
 * drives the core's own static scratch; it must run single-threaded.
 */
#include <stdio.h>
#include <stdlib.h>

#define DOMINANT_COLOR_IMPLEMENTATION
#include "dominant_color.h"

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		exit(1); \
	} \
} while (0)

/* Native-endian ARGB32 builder (premultiplied, as cairo stores it). */
static uint32_t px(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
	return (a << 24) | (r << 16) | (g << 8) | b;
}

static void
test_flat_color(void)
{
	/* 4x4 flat opaque #345678 -> exact color, share 1.0. */
	uint32_t buf[4 * 4];
	struct dominant_color_opts opts = { 0 };
	struct dominant_color_result res;

	for (int i = 0; i < 4 * 4; i++)
		buf[i] = px(0xFF, 0x34, 0x56, 0x78);
	CHECK(dominant_color(buf, 4, 4, 4, &opts, &res) == 1);
	CHECK(res.r == 0x34 && res.g == 0x56 && res.b == 0x78);
	CHECK(res.share == 1.0);
	CHECK(res.voters == 16);
	CHECK(res.checked == 16);
}

static void
test_split_40_60(void)
{
	/* 10x10: 40% red, 60% green -> green wins. */
	uint32_t buf[10 * 10];
	struct dominant_color_opts opts = { 0 };
	struct dominant_color_result res;

	for (int i = 0; i < 10 * 10; i++)
		buf[i] = (i < 40) ? px(0xFF, 0xFF, 0, 0) : px(0xFF, 0, 0xFF, 0);
	CHECK(dominant_color(buf, 10, 10, 10, &opts, &res) == 1);
	CHECK(res.r == 0x00 && res.g == 0xFF && res.b == 0x00);
	CHECK(res.share > 0.59 && res.share < 0.61);
}

static void
test_background_with_glyph(void)
{
	/* 16x16 light-grey background with a few antialiased dark glyph pixels
	 * scattered in: the flat background must come back exact. */
	uint32_t buf[16 * 16];
	struct dominant_color_opts opts = { 0 };
	struct dominant_color_result res;

	for (int i = 0; i < 16 * 16; i++)
		buf[i] = px(0xFF, 0xE8, 0xE8, 0xE8);
	/* 10 dark antialiased-ish pixels (blended toward near-black). */
	buf[0] = px(0xFF, 0x20, 0x20, 0x20);
	buf[1] = px(0xFF, 0x30, 0x30, 0x30);
	buf[2] = px(0xFF, 0x40, 0x40, 0x40);
	buf[3] = px(0xFF, 0x50, 0x50, 0x50);
	buf[4] = px(0xFF, 0x18, 0x18, 0x18);
	buf[5] = px(0xFF, 0x28, 0x28, 0x28);
	buf[6] = px(0xFF, 0x38, 0x38, 0x38);
	buf[7] = px(0xFF, 0x48, 0x48, 0x48);
	buf[8] = px(0xFF, 0x58, 0x58, 0x58);
	buf[9] = px(0xFF, 0x60, 0x60, 0x60);
	CHECK(dominant_color(buf, 16, 16, 16, &opts, &res) == 1);
	CHECK(res.r == 0xE8 && res.g == 0xE8 && res.b == 0xE8);
	CHECK(res.share > 0.9);
}

static void
test_noise_around_base(void)
{
	/* 20x20 base 0x808080 with +/-2 noise on a few pixels: the result must
	 * be an actual pixel color within +/-2 of the base. */
	uint32_t buf[20 * 20];
	struct dominant_color_opts opts = { 0 };
	struct dominant_color_result res;

	for (int i = 0; i < 20 * 20; i++)
		buf[i] = px(0xFF, 0x80, 0x80, 0x80);
	buf[0] = px(0xFF, 0x7E, 0x7F, 0x81);
	buf[1] = px(0xFF, 0x82, 0x7F, 0x80);
	buf[2] = px(0xFF, 0x7F, 0x81, 0x7E);
	CHECK(dominant_color(buf, 20, 20, 20, &opts, &res) == 1);
	CHECK(abs((int)res.r - 0x80) <= 2);
	CHECK(abs((int)res.g - 0x80) <= 2);
	CHECK(abs((int)res.b - 0x80) <= 2);
	/* Share near 1.0: noise stays inside the winning bin. */
	CHECK(res.share > 0.99);
}

static void
test_unpremultiply(void)
{
	/* Straight color (51,102,153) at alpha 160 (0xA0). Premultiplied:
	 *   r = 51*160/255 = 32, g = 102*160/255 = 64, b = 153*160/255 = 96.
	 * The core must un-premultiply back to exactly (51,102,153): these
	 * values round-trip without loss, so the test asserts the exact math. */
	uint32_t buf[2 * 2];
	struct dominant_color_opts opts = { 0 };
	struct dominant_color_result res;

	for (int i = 0; i < 2 * 2; i++)
		buf[i] = px(0xA0, 32, 64, 96);
	CHECK(dominant_color(buf, 2, 2, 2, &opts, &res) == 1);
	CHECK(res.r == 51 && res.g == 102 && res.b == 153);
}

static void
test_min_alpha_and_transparent(void)
{
	struct dominant_color_opts opts = { 0 };
	struct dominant_color_result res;

	/* Fully transparent: nothing votes. */
	uint32_t transparent[2 * 2] = { 0, 0, 0, 0 };
	CHECK(dominant_color(transparent, 2, 2, 2, &opts, &res) == 0);

	/* Below min_alpha (default 32): ignored. */
	uint32_t dim[2 * 2];
	for (int i = 0; i < 2 * 2; i++)
		dim[i] = px(0x10, 0xFF, 0xFF, 0xFF); /* alpha 16 < 32 */
	CHECK(dominant_color(dim, 2, 2, 2, &opts, &res) == 0);

	/* Mixed: bright pixels vote, dim ones do not. */
	uint32_t mixed[2 * 2] = {
		px(0xFF, 0xFF, 0x00, 0x00),
		px(0x10, 0xFF, 0xFF, 0xFF),
		px(0xFF, 0xFF, 0x00, 0x00),
		px(0xFF, 0xFF, 0x00, 0x00),
	};
	CHECK(dominant_color(mixed, 2, 2, 2, &opts, &res) == 1);
	CHECK(res.r == 0xFF && res.g == 0x00 && res.b == 0x00);
	CHECK(res.voters == 3);

	/* Explicit min_alpha=200: only the opaque pixels count. */
	opts.min_alpha = 200;
	uint32_t alpha_mix[4] = {
		px(0xFF, 0x00, 0x00, 0xFF),
		px(0x80, 0xFF, 0x00, 0x00),
		px(0xFF, 0x00, 0x00, 0xFF),
		px(0x80, 0xFF, 0x00, 0x00),
	};
	CHECK(dominant_color(alpha_mix, 2, 2, 2, &opts, &res) == 1);
	CHECK(res.r == 0x00 && res.g == 0x00 && res.b == 0xFF);
	CHECK(res.voters == 2);
}

static void
test_step_and_stride_and_ties(void)
{
	struct dominant_color_result res;
	uint32_t buf[6 * 6];

	/* step_x=step_y=2: only the 9 even grid points are examined. Paint the
	 * examined points red and everything else green. */
	for (int y = 0; y < 6; y++)
		for (int x = 0; x < 6; x++)
			buf[y * 6 + x] = px(0xFF, 0, 0xFF, 0);
	for (int y = 0; y < 6; y += 2)
		for (int x = 0; x < 6; x += 2)
			buf[y * 6 + x] = px(0xFF, 0xFF, 0, 0);
	struct dominant_color_opts step_opts = { .step_x = 2, .step_y = 2 };
	CHECK(dominant_color(buf, 6, 6, 6, &step_opts, &res) == 1);
	CHECK(res.r == 0xFF && res.g == 0x00 && res.b == 0x00);
	CHECK(res.checked == 9);
	CHECK(res.voters == 9);

	/* Stride with padding: width 3 but stride 6 (3 unused uint32s per row).
	 * The core must read only the first 3 columns. */
	{
		uint32_t padded[6 * 4];
		for (int i = 0; i < 6 * 4; i++)
			padded[i] = px(0xFF, 0x11, 0x22, 0x33); /* padding + col 0..2 */
		padded[1] = px(0xFF, 0xAA, 0xAA, 0xAA);
		padded[2] = px(0xFF, 0xBB, 0xBB, 0xBB);
		padded[7] = px(0xFF, 0xAA, 0xAA, 0xAA);
		padded[8] = px(0xFF, 0xBB, 0xBB, 0xBB);
		padded[13] = px(0xFF, 0xAA, 0xAA, 0xAA);
		padded[14] = px(0xFF, 0xBB, 0xBB, 0xBB);
		padded[19] = px(0xFF, 0xAA, 0xAA, 0xAA);
		padded[20] = px(0xFF, 0xBB, 0xBB, 0xBB);
		/* 4 rows, 6-wide stride, only first 3 columns are the image. */
		CHECK(dominant_color(padded, 3, 4, 6, &(struct dominant_color_opts){ 0 },
		                     &res) == 1);
		CHECK(res.r == 0x11 && res.g == 0x22 && res.b == 0x33);
		CHECK(res.checked == 12);
	}

	/* Tie: exactly 50/50 red vs green. Red bins before green? Quantization
	 * bins are (r,g,b) index = r<<(2b) | g<<b | b with b=4. Red
	 * (0xFF,0,0) -> 15<<8 = 3840; green (0,0xFF,0) -> 15<<4 = 240. The
	 * lower index (green) must win the tie. */
	{
		uint32_t tie[8];
		for (int i = 0; i < 8; i++)
			tie[i] = (i < 4) ? px(0xFF, 0xFF, 0, 0) : px(0xFF, 0, 0xFF, 0);
		CHECK(dominant_color(tie, 4, 2, 4, &(struct dominant_color_opts){ 0 },
		                     &res) == 1);
		CHECK(res.r == 0x00 && res.g == 0xFF && res.b == 0x00);
		CHECK(res.share == 0.5);
	}
}

int
main(void)
{
	test_flat_color();
	test_split_40_60();
	test_background_with_glyph();
	test_noise_around_base();
	test_unpremultiply();
	test_min_alpha_and_transparent();
	test_step_and_stride_and_ties();
	printf("dominant_color: all tests passed\n");
	return 0;
}