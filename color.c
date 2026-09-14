/*
 * color.c - color parsing and manipulation
 *
 * Adapted from AwesomeWM's color.c for Wayland/somewm
 * Original copyright:
 *   Copyright © 2008-2009 Julien Danjou <julien@danjou.info>
 *   Copyright © 2009 Uli Schlachter <psychon@znc.in>
 *
 * X11-specific code removed:
 *   - xcb_alloc_color() color allocation (no colormaps in Wayland)
 *   - xcb_visualtype_t visual handling (no X11 visuals in Wayland)
 *   - globalconf dependencies (no X11 connection needed)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "color.h"
#include "common/util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <lauxlib.h>

/* Scale a single RGBA channel to a byte.
 *
 * Integer values (e.g. "255") are treated as the CSS 0-255 scale; values with
 * a fractional part (e.g. "0.5") are treated as the 0.0-1.0 scale.
 */
static uint8_t
color_channel_to_u8(double v, bool is_int)
{
    if (is_int) {
        if (v < 0)
            v = 0;
        if (v > 255)
            v = 255;
        return (uint8_t)(v + 0.5);
    }
    if (v < 0)
        v = 0;
    if (v > 1)
        v = 1;
    return (uint8_t)(v * 255.0 + 0.5);
}

/* Parse one rgba()/rgb() channel: skip whitespace, read a number. */
static bool
color_parse_channel(const char **sp, double *out, bool *is_int)
{
    const char *s = *sp;
    char *end;
    double v;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0' || *s == ')')
        return false;

    v = strtod(s, &end);
    if (end == s)
        return false;

    *is_int = (strspn(s, "0123456789") == (size_t)(end - s));
    *out = v;
    *sp = end;
    return true;
}

/** Parse the functional color form rgba(R,G,B,A) / rgb(R,G,B).
 *
 * R,G,B accept either 0-255 integers ("255") or 0.0-1.0 fractions ("0.5").
 * A (alpha) is always a 0.0-1.0 fraction, like CSS; "1" means fully opaque.
 *
 * \param colstr The color string (starts with "rgb")
 * \param len The color string length
 * \param red Pointer to store red component (0-255)
 * \param green Pointer to store green component (0-255)
 * \param blue Pointer to store blue component (0-255)
 * \param alpha Pointer to store alpha component (0-255)
 * \return true if parsing succeeded, false on error
 */
static bool
color_parse_rgba(const char *colstr, ssize_t len,
                 uint8_t *red, uint8_t *green, uint8_t *blue, uint8_t *alpha)
{
    const char *s = colstr + 3;
    bool has_alpha;
    double comp[4];
    bool is_int[4];
    int n = 0;

    has_alpha = (*s == 'a');
    s = strchr(s, '(');
    if (!s)
        return false;
    s++;

    while (*s != ')' && *s != '\0') {
        if (!color_parse_channel(&s, &comp[n], &is_int[n]))
            return false;
        n++;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == ',')
            s++;
    }
    if (*s != ')')
        return false;

    /* rgb() needs exactly 3 channels; rgba() takes 3 or 4 (alpha optional). */
    if (has_alpha) {
        if (n != 4 && n != 3)
            return false;
    } else if (n != 3) {
        return false;
    }

    *red   = color_channel_to_u8(comp[0], is_int[0]);
    *green = color_channel_to_u8(comp[1], is_int[1]);
    *blue  = color_channel_to_u8(comp[2], is_int[2]);
    if (n == 4)
        *alpha = color_channel_to_u8(comp[3], false);
    else
        *alpha = 0xff;

    return true;
}

/** Parse a hexadecimal color string to its RGBA components
 *
 * Accepts "#RGB", "#RRGGBB" and "#RRGGBBAA".
 *
 * \param colstr The color string (must start with #)
 * \param len The color string length
 * \param red Pointer to store red component (0-255)
 * \param green Pointer to store green component (0-255)
 * \param blue Pointer to store blue component (0-255)
 * \param alpha Pointer to store alpha component (0-255)
 * \return true if parsing succeeded, false on error
 */
static bool
color_parse_hex(const char *colstr, ssize_t len,
                uint8_t *red, uint8_t *green, uint8_t *blue, uint8_t *alpha)
{
    unsigned long colnum;
    char *p;
    int i;

    *alpha = 0xff;  /* Default to fully opaque */

    if (colstr[0] != '#')
        return false;

    if (len == 4) {
        /* Short form #RGB: each digit doubled (#f0a == #ff00aa) */
        char expanded[8];
        expanded[0] = '#';
        for (i = 0; i < 3; i++) {
            expanded[1 + 2 * i] = colstr[1 + i];
            expanded[2 + 2 * i] = colstr[1 + i];
        }
        expanded[7] = '\0';
        return color_parse_hex(expanded, 7, red, green, blue, alpha);
    }

    colnum = strtoul(colstr + 1, &p, 16);
    if (len == 9 && (p - colstr) == 9) {
        /* Format: #RRGGBBAA */
        *alpha = colnum & 0xff;
        colnum >>= 8;
        len -= 2;
        p -= 2;
    }
    if (len != 7 || (p - colstr) != 7)
        return false;

    *red   = (colnum >> 16) & 0xff;
    *green = (colnum >> 8) & 0xff;
    *blue  = colnum & 0xff;

    return true;
}

/** Parse a color string to its RGBA components
 *
 * Supports hex ("#RGB", "#RRGGBB", "#RRGGBBAA") and the functional form
 * ("rgb(...)", "rgba(r,g,b,a)").
 *
 * \param colstr The color string
 * \param len The color string length
 * \param red Pointer to store red component (0-255)
 * \param green Pointer to store green component (0-255)
 * \param blue Pointer to store blue component (0-255)
 * \param alpha Pointer to store alpha component (0-255)
 * \return true if parsing succeeded, false on error
 */
static bool
color_parse(const char *colstr, ssize_t len,
            uint8_t *red, uint8_t *green, uint8_t *blue, uint8_t *alpha)
{
    bool ok;

    *alpha = 0xff;  /* Default to fully opaque */

    if (len >= 4 && colstr[0] == 'r' && colstr[1] == 'g' && colstr[2] == 'b'
            && (colstr[3] == '(' || (colstr[3] == 'a' && len >= 5 && colstr[4] == '('))
            && colstr[len - 1] == ')') {
        ok = color_parse_rgba(colstr, len, red, green, blue, alpha);
        if (!ok)
            fprintf(stderr, "somewm: error, invalid color '%s'\n", colstr);
        return ok;
    }

    ok = color_parse_hex(colstr, len, red, green, blue, alpha);
    if (!ok)
        fprintf(stderr, "somewm: error, invalid color '%s'\n", colstr);
    return ok;
}

/** Parse a color string and initialize a color_t structure
 *
 * This is the Wayland-simplified version. Unlike AwesomeWM's color_init_unchecked()
 * which allocates X11 colormaps and handles visuals, we just parse the string and
 * store RGBA values directly.
 *
 * \param color Pointer to color_t to initialize
 * \param colstr Color string (e.g., "#ff0000" or "#ff0000aa")
 * \return true if successful, false on error
 */
bool
color_init_from_string(color_t *color, const char *colstr)
{
    ssize_t len;

    if (!color || !colstr || colstr[0] == '\0') {
        return false;
    }

    len = strlen(colstr);

    if (!color_parse(colstr, len, &color->red, &color->green,
                     &color->blue, &color->alpha)) {
        return false;
    }

    color->initialized = true;
    return true;
}

/** Convert color_t to Cairo color format
 *
 * Cairo uses doubles in the range 0.0-1.0 for color components.
 *
 * \param color Source color
 * \param r Pointer to store red (0.0-1.0)
 * \param g Pointer to store green (0.0-1.0)
 * \param b Pointer to store blue (0.0-1.0)
 * \param a Pointer to store alpha (0.0-1.0)
 */
void
color_to_cairo(const color_t *color, double *r, double *g, double *b, double *a)
{
    if (!color) return;

    if (r) *r = color->red / 255.0;
    if (g) *g = color->green / 255.0;
    if (b) *b = color->blue / 255.0;
    if (a) *a = color->alpha / 255.0;
}

/** Convert color_t to float array for wlroots
 *
 * wlr_scene_rect_set_color() expects float[4] in RGBA format with 0.0-1.0 range.
 *
 * \param color Source color
 * \param floats Pointer to float[4] array to fill (RGBA order)
 */
void
color_to_floats(const color_t *color, float floats[static 4])
{
    if (!color) return;

    floats[0] = color->red / 255.0f;
    floats[1] = color->green / 255.0f;
    floats[2] = color->blue / 255.0f;
    floats[3] = color->alpha / 255.0f;
}

/** Convert color_t to uint32_t in ARGB format
 *
 * Format: 0xAARRGGBB
 * This is the format wlroots typically uses.
 *
 * \param color Source color
 * \return uint32_t in ARGB format
 */
uint32_t
color_to_uint32(const color_t *color)
{
    if (!color) return 0;

    return ((uint32_t)color->alpha << 24) |
           ((uint32_t)color->red   << 16) |
           ((uint32_t)color->green << 8)  |
           ((uint32_t)color->blue);
}

/** Convert color_t to uint32_t in RGBA format
 *
 * Format: 0xRRGGBBAA
 * Alternative format for systems that prefer RGBA ordering.
 *
 * \param color Source color
 * \return uint32_t in RGBA format
 */
uint32_t
color_to_uint32_rgba(const color_t *color)
{
    if (!color) return 0;

    return ((uint32_t)color->red   << 24) |
           ((uint32_t)color->green << 16) |
           ((uint32_t)color->blue  << 8)  |
           ((uint32_t)color->alpha);
}

/** Push a color as a hex string onto the Lua stack
 *
 * Adapted from AwesomeWM's luaA_pushcolor() (lines 202-223) with the
 * X11 color struct dependency removed. We use our simple color_t instead.
 *
 * Format: "#RRGGBB" if alpha is 0xff, "#RRGGBBAA" otherwise
 *
 * \param L Lua state
 * \param color Color to push
 * \return Number of elements pushed on stack (always 1)
 */
int
luaA_pushcolor(lua_State *L, const color_t *color)
{
    char s[10];  /* "#RRGGBBAA\0" = 10 chars max */
    int len;

    if (!color || !color->initialized) {
        lua_pushnil(L);
        return 1;
    }

    if (color->alpha >= 0xff) {
        /* Full opacity - use short format */
        len = snprintf(s, sizeof(s), "#%02x%02x%02x",
                      color->red, color->green, color->blue);
    } else {
        /* Transparent - include alpha */
        len = snprintf(s, sizeof(s), "#%02x%02x%02x%02x",
                      color->red, color->green, color->blue, color->alpha);
    }

    lua_pushlstring(L, s, len);
    return 1;
}

/** Parse a color from Lua stack
 *
 * New function for Wayland - reads a color string from Lua and parses it.
 *
 * \param L Lua state
 * \param idx Stack index of color string
 * \param color Pointer to color_t to fill
 * \return true if parsing succeeded, false on error
 */
bool
luaA_tocolor(lua_State *L, int idx, color_t *color)
{
    const char *colstr;

    if (!color) return false;

    colstr = lua_tostring(L, idx);
    if (!colstr) {
        return false;
    }

    return color_init_from_string(color, colstr);
}
