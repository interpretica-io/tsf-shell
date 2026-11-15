/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Terminal-emulator backend over libghostty-vt
 *
 * The preferred backend (built when @c TA_SHELL_HAVE_GHOSTTY is set):
 * the VT state machine and screen model of Ghostty as the
 * zero-dependency C library libghostty-vt, linked statically and called
 * in process. This file is the only place a libghostty-vt symbol
 * appears. It implements the ta_shell_term_ops seam from
 * ta_shell_term.h on the library's public C API (<ghostty/vt.h>):
 *
 *  - the terminal:  ghostty_terminal_new/vt_write/resize/get/free;
 *  - the grid:      ghostty_terminal_grid_ref + ghostty_grid_ref_cell/
 *                   _style and ghostty_cell_get, addressed by a point in
 *                   the active area (the screen) or in the history (the
 *                   scrollback);
 *  - the snapshot:  ghostty_snapshot_encode_alloc and the snapshot
 *                   decoder, used to replace the live terminal with its
 *                   own snapshot (the seam's snapshot_restore).
 *
 * Text is assembled here from cells, the same way the libvterm backend
 * does it, so a row reads identically on both backends: blank cells are
 * spaces, the right half of a wide glyph is skipped, trailing blanks
 * are trimmed.
 */

#ifdef TA_SHELL_HAVE_GHOSTTY

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <ghostty/vt.h>

#include "te_defs.h"
#include "te_alloc.h"
#include "te_string.h"

#include "ta_shell_term.h"

/* Nominal cell size in pixels; only image protocols and size reports
 * look at it. */
#define G_CELL_W_PX 8
#define G_CELL_H_PX 16

/* ---- the state this backend keeps --------------------------------- */

struct ta_shell_term {
    GhosttyTerminal vt;
};

/* ---- small utilities ---------------------------------------------- */

/* Append code point @p cp to @p out as UTF-8; a blank cell is a space. */
static void
append_utf8(te_string *out, uint32_t cp)
{
    if (cp == 0)
    {
        te_string_append(out, " ");
    }
    else if (cp < 0x80)
    {
        te_string_append(out, "%c", (char)cp);
    }
    else if (cp < 0x800)
    {
        te_string_append(out, "%c%c",
                         (char)(0xC0 | (cp >> 6)),
                         (char)(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        te_string_append(out, "%c%c%c",
                         (char)(0xE0 | (cp >> 12)),
                         (char)(0x80 | ((cp >> 6) & 0x3F)),
                         (char)(0x80 | (cp & 0x3F)));
    }
    else
    {
        te_string_append(out, "%c%c%c%c",
                         (char)(0xF0 | (cp >> 18)),
                         (char)(0x80 | ((cp >> 12) & 0x3F)),
                         (char)(0x80 | ((cp >> 6) & 0x3F)),
                         (char)(0x80 | (cp & 0x3F)));
    }
}

/* The cell at (x, y) of the coordinate system @p tag; false when out of
 * range. */
static bool
g_ref(ta_shell_term *t, GhosttyPointTag tag, int x, uint32_t y,
      GhosttyGridRef *ref)
{
    GhosttyPoint pt;

    memset(&pt, 0, sizeof(pt));
    pt.tag = tag;
    pt.value.coordinate.x = (uint16_t)x;
    pt.value.coordinate.y = y;
    return ghostty_terminal_grid_ref(t->vt, pt, ref) == GHOSTTY_SUCCESS;
}

/* Code point and width class of the cell behind @p ref. */
static bool
g_ref_glyph(const GhosttyGridRef *ref, uint32_t *cp, GhosttyCellWide *wide)
{
    GhosttyCell cell = 0;

    *cp = 0;
    *wide = GHOSTTY_CELL_WIDE_NARROW;
    if (ghostty_grid_ref_cell(ref, &cell) != GHOSTTY_SUCCESS)
        return false;
    (void)ghostty_cell_get(cell, GHOSTTY_CELL_DATA_CODEPOINT, cp);
    (void)ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, wide);
    return true;
}

/* Convert a style colour to packed 0xRRGGBB (or -1 default) plus the
 * palette index when the cell used one (else -1). */
static void
conv_color(ta_shell_term *t, const GhosttyStyleColor *col, int *packed,
           int *index)
{
    *packed = -1;
    *index = -1;

    if (col->tag == GHOSTTY_STYLE_COLOR_RGB)
    {
        *packed = ((int)col->value.rgb.r << 16) |
                  ((int)col->value.rgb.g << 8) | (int)col->value.rgb.b;
    }
    else if (col->tag == GHOSTTY_STYLE_COLOR_PALETTE)
    {
        GhosttyColorRgb palette[256];

        *index = col->value.palette;
        memset(palette, 0, sizeof(palette));
        if (ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE,
                                 palette) == GHOSTTY_SUCCESS)
        {
            const GhosttyColorRgb *c = &palette[col->value.palette];

            *packed = ((int)c->r << 16) | ((int)c->g << 8) | (int)c->b;
        }
    }
}

/* Append row @p y of coordinate system @p tag, trailing blanks trimmed. */
static void
g_row_text_at(ta_shell_term *t, GhosttyPointTag tag, uint32_t y, int cols,
              te_string *out)
{
    int last = -1;
    int x;

    for (x = 0; x < cols; x++)
    {
        GhosttyGridRef  ref;
        uint32_t        cp;
        GhosttyCellWide wide;

        if (g_ref(t, tag, x, y, &ref) && g_ref_glyph(&ref, &cp, &wide) &&
            cp != 0 && cp != (uint32_t)' ')
            last = x;
    }
    for (x = 0; x <= last; x++)
    {
        GhosttyGridRef  ref;
        uint32_t        cp;
        GhosttyCellWide wide;

        if (!g_ref(t, tag, x, y, &ref) || !g_ref_glyph(&ref, &cp, &wide))
            continue;
        if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL)
            continue;   /* right half of a wide glyph */
        append_utf8(out, cp);
    }
}

/* ---- ops ---------------------------------------------------------- */

static const char *
g_version(void)
{
    const char *v = ghostty_version();

    return v != NULL ? v : "";
}

static ta_shell_term *
g_create(int cols, int rows)
{
    struct ta_shell_term *t = TE_ALLOC(sizeof(*t));

    if (ghostty_terminal_new(NULL, &t->vt, (uint16_t)cols,
                             (uint16_t)rows) != GHOSTTY_SUCCESS ||
        t->vt == NULL)
    {
        free(t);
        return NULL;
    }
    return t;
}

static void
g_feed(ta_shell_term *t, const char *buf, size_t len)
{
    ghostty_terminal_vt_write(t->vt, (const uint8_t *)buf, len);
}

static void
g_get_size(ta_shell_term *t, int *cols, int *rows)
{
    uint16_t c = 0;
    uint16_t r = 0;

    (void)ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_COLS, &c);
    (void)ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_ROWS, &r);
    if (cols != NULL) *cols = c;
    if (rows != NULL) *rows = r;
}

static int
g_cell(ta_shell_term *t, int x, int y, ta_shell_cell_t *out)
{
    GhosttyGridRef  ref;
    GhosttyStyle    style;
    GhosttyCellWide wide;
    uint32_t        cp;
    int             cols = 0;
    int             rows = 0;

    g_get_size(t, &cols, &rows);
    if (x < 0 || y < 0 || x >= cols || y >= rows)
        return 1;
    if (!g_ref(t, GHOSTTY_POINT_TAG_ACTIVE, x, (uint32_t)y, &ref) ||
        !g_ref_glyph(&ref, &cp, &wide))
        return 1;

    memset(out, 0, sizeof(*out));
    out->codepoint = cp;
    out->fg = -1;
    out->bg = -1;
    out->fg_index = -1;
    out->bg_index = -1;

    memset(&style, 0, sizeof(style));
    if (ghostty_grid_ref_style(&ref, &style) == GHOSTTY_SUCCESS)
    {
        conv_color(t, &style.fg_color, &out->fg, &out->fg_index);
        conv_color(t, &style.bg_color, &out->bg, &out->bg_index);
        if (style.flags & GHOSTTY_STYLE_FLAG_BOLD)
            out->flags |= TA_SHELL_ATTR_BOLD;
        if (GHOSTTY_STYLE_UNDERLINE(style) != 0)
            out->flags |= TA_SHELL_ATTR_UNDERLINE;
        if (style.flags & GHOSTTY_STYLE_FLAG_INVERSE)
            out->flags |= TA_SHELL_ATTR_REVERSE;
        if (style.flags & GHOSTTY_STYLE_FLAG_ITALIC)
            out->flags |= TA_SHELL_ATTR_ITALIC;
        if (style.flags & GHOSTTY_STYLE_FLAG_BLINK)
            out->flags |= TA_SHELL_ATTR_BLINK;
        if (style.flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH)
            out->flags |= TA_SHELL_ATTR_STRIKE;
    }
    if (wide == GHOSTTY_CELL_WIDE_WIDE)
        out->flags |= TA_SHELL_ATTR_WIDE;
    return 0;
}

static void
g_cursor(ta_shell_term *t, int *x, int *y, int *visible)
{
    uint16_t cx = 0;
    uint16_t cy = 0;
    bool     vis = true;

    (void)ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_CURSOR_X, &cx);
    (void)ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &cy);
    (void)ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE,
                               &vis);
    if (x != NULL) *x = cx;
    if (y != NULL) *y = cy;
    if (visible != NULL) *visible = vis ? 1 : 0;
}

static int
g_pending_wrap(ta_shell_term *t)
{
    bool pending = false;

    if (ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_CURSOR_PENDING_WRAP,
                             &pending) != GHOSTTY_SUCCESS)
        return -1;
    return pending ? 1 : 0;
}

static void
g_row_text(ta_shell_term *t, int y, te_string *out)
{
    int cols = 0;
    int rows = 0;

    g_get_size(t, &cols, &rows);
    if (y < 0 || y >= rows)
        return;
    g_row_text_at(t, GHOSTTY_POINT_TAG_ACTIVE, (uint32_t)y, cols, out);
}

static void
g_text(ta_shell_term *t, te_string *out)
{
    int cols = 0;
    int rows = 0;
    int y;

    g_get_size(t, &cols, &rows);
    for (y = 0; y < rows; y++)
    {
        if (y > 0)
            te_string_append(out, "\n");
        g_row_text_at(t, GHOSTTY_POINT_TAG_ACTIVE, (uint32_t)y, cols, out);
    }
}

static void
g_resize(ta_shell_term *t, int cols, int rows)
{
    (void)ghostty_terminal_resize(t->vt, (uint16_t)cols, (uint16_t)rows,
                                  G_CELL_W_PX, G_CELL_H_PX);
}

static void
g_scrollback(ta_shell_term *t, int max_lines, te_string *out)
{
    size_t total = 0;
    size_t start = 0;
    size_t i;
    int    cols = 0;
    int    rows = 0;

    g_get_size(t, &cols, &rows);
    if (ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS,
                             &total) != GHOSTTY_SUCCESS)
        return;
    if (max_lines > 0 && (size_t)max_lines < total)
        start = total - (size_t)max_lines;

    for (i = start; i < total; i++)
    {
        if (i > start)
            te_string_append(out, "\n");
        g_row_text_at(t, GHOSTTY_POINT_TAG_HISTORY, (uint32_t)i, cols, out);
    }
}

/*
 * Replace the live terminal with the one decoded from its own snapshot.
 * Returns 0 on success; on failure the live terminal is kept.
 */
static int
g_snapshot_restore(ta_shell_term *t)
{
    GhosttySnapshotDecoder dec = NULL;
    GhosttyTerminal        restored = NULL;
    uint8_t               *buf = NULL;
    size_t                 len = 0;
    int                    rc = 1;

    if (ghostty_snapshot_encode_alloc(t->vt, NULL, &buf,
                                      &len) != GHOSTTY_SUCCESS)
        return 1;

    if (ghostty_snapshot_decoder_new_buf(NULL, &dec, buf,
                                         len) == GHOSTTY_SUCCESS &&
        ghostty_snapshot_decoder_decode(dec, &restored) == GHOSTTY_SUCCESS &&
        restored != NULL)
    {
        ghostty_snapshot_decoder_free(dec);
        dec = NULL;
        ghostty_terminal_free(t->vt);
        t->vt = restored;
        rc = 0;
    }

    if (dec != NULL)
        ghostty_snapshot_decoder_free(dec);
    ghostty_free(NULL, buf, len);
    return rc;
}

static void
g_destroy(ta_shell_term *t)
{
    if (t == NULL)
        return;
    if (t->vt != NULL)
        ghostty_terminal_free(t->vt);
    free(t);
}

const ta_shell_term_ops ta_shell_ghostty_ops = {
    .name             = "ghostty",
    .backend_bit      = TA_SHELL_BACKEND_GHOSTTY,
    .version          = g_version,
    .create           = g_create,
    .feed             = g_feed,
    .get_size         = g_get_size,
    .cell             = g_cell,
    .cursor           = g_cursor,
    .text             = g_text,
    .row_text         = g_row_text,
    .resize           = g_resize,
    .scrollback       = g_scrollback,
    .destroy          = g_destroy,
    .pending_wrap     = g_pending_wrap,
    .snapshot_restore = g_snapshot_restore,
};

#endif /* TA_SHELL_HAVE_GHOSTTY */
