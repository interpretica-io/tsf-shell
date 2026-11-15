/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Terminal-emulator backend over libghostty-vt
 *
 * The preferred backend (built when @c TA_SHELL_HAVE_GHOSTTY is set):
 * the VT state machine and screen model extracted from Ghostty as the
 * zero-dependency C library libghostty-vt. It is linked and called in
 * process; this file is the only place a libghostty-vt symbol appears.
 * It implements the ta_shell_term_ops seam from ta_shell_term.h.
 *
 * -------------------------------------------------------------------
 *  BINDING SURFACE - PROVISIONAL
 *
 *  libghostty-vt's embeddable C API is still settling (see
 *  https://libghostty.tip.ghostty.org/). Every libghostty-vt symbol
 *  this backend needs is declared once, below, behind the
 *  TA_SHELL_GHOSTTY_HEADER guard, and used only through the small
 *  g_* wrappers that follow. The first build on a host that actually
 *  carries libghostty-vt should reconcile these declarations with the
 *  installed <ghostty/vt.h> - names and signatures only; the ops code
 *  underneath does not change. Define TA_SHELL_GHOSTTY_HEADER in the
 *  meson once the real header's include path and name are confirmed to
 *  use it directly instead of these forward declarations.
 * -------------------------------------------------------------------
 */

#ifdef TA_SHELL_HAVE_GHOSTTY

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "te_defs.h"
#include "te_alloc.h"
#include "te_string.h"

#include "ta_shell_term.h"

/* ---- provisional libghostty-vt declarations ----------------------- */
#ifdef TA_SHELL_GHOSTTY_HEADER
#include <ghostty/vt.h>
#else
/*
 * Forward declarations matching the documented shape of libghostty-vt's
 * terminal-state C API. Confirm against the installed header at first
 * build (this is the only block that should need touching).
 */
typedef struct ghostty_vt_s *ghostty_vt_t;

/* Attribute bits returned in ghostty_vt_cell_t.flags (provisional). */
enum {
    GHOSTTY_VT_BOLD      = 1 << 0,
    GHOSTTY_VT_UNDERLINE = 1 << 1,
    GHOSTTY_VT_REVERSE   = 1 << 2,
    GHOSTTY_VT_ITALIC    = 1 << 3,
    GHOSTTY_VT_BLINK     = 1 << 4,
    GHOSTTY_VT_STRIKE    = 1 << 5,
};

typedef struct ghostty_vt_cell_s {
    uint32_t codepoint;     /* 0 for a blank cell */
    uint32_t fg_rgb;        /* 0xRRGGBB when !fg_default */
    uint32_t bg_rgb;        /* 0xRRGGBB when !bg_default */
    int32_t  fg_index;      /* palette index 0..255, or -1 */
    int32_t  bg_index;      /* palette index 0..255, or -1 */
    uint32_t flags;         /* GHOSTTY_VT_* */
    uint8_t  width;         /* 1, or 2 for the left of a wide glyph; 0 right half */
    bool     fg_default;
    bool     bg_default;
} ghostty_vt_cell_t;

extern ghostty_vt_t ghostty_vt_new(uint16_t cols, uint16_t rows);
extern void         ghostty_vt_free(ghostty_vt_t vt);
extern void         ghostty_vt_write(ghostty_vt_t vt, const uint8_t *data,
                                     size_t len);
extern void         ghostty_vt_resize(ghostty_vt_t vt, uint16_t cols,
                                      uint16_t rows);
extern void         ghostty_vt_get_size(ghostty_vt_t vt, uint16_t *cols,
                                        uint16_t *rows);
extern void         ghostty_vt_get_cursor(ghostty_vt_t vt, uint16_t *x,
                                          uint16_t *y, bool *visible);
extern bool         ghostty_vt_get_cell(ghostty_vt_t vt, uint16_t x, uint16_t y,
                                        ghostty_vt_cell_t *out);
/* Write up to len bytes of row y's UTF-8 text; return the full length. */
extern size_t       ghostty_vt_row_text(ghostty_vt_t vt, uint16_t y, char *buf,
                                        size_t len);
/* Write up to len bytes of the last max_lines scrollback lines ('\n'
 * joined, oldest first; max_lines<=0 means all); return the full length. */
extern size_t       ghostty_vt_scrollback_text(ghostty_vt_t vt, int max_lines,
                                               char *buf, size_t len);
extern const char  *ghostty_vt_version(void);
#endif /* TA_SHELL_GHOSTTY_HEADER */

/* ---- the state this backend keeps --------------------------------- */

struct ta_shell_term {
    ghostty_vt_t vt;
};

/* ---- ops ---------------------------------------------------------- */

static const char *
g_version(void)
{
    const char *v = ghostty_vt_version();

    return v != NULL ? v : "";
}

static ta_shell_term *
g_create(int cols, int rows)
{
    struct ta_shell_term *t = TE_ALLOC(sizeof(*t));

    t->vt = ghostty_vt_new((uint16_t)cols, (uint16_t)rows);
    if (t->vt == NULL)
    {
        free(t);
        return NULL;
    }
    return t;
}

static void
g_feed(ta_shell_term *t, const char *buf, size_t len)
{
    ghostty_vt_write(t->vt, (const uint8_t *)buf, len);
}

static void
g_get_size(ta_shell_term *t, int *cols, int *rows)
{
    uint16_t c = 0, r = 0;

    ghostty_vt_get_size(t->vt, &c, &r);
    if (cols != NULL) *cols = c;
    if (rows != NULL) *rows = r;
}

static int
g_cell(ta_shell_term *t, int x, int y, ta_shell_cell_t *out)
{
    ghostty_vt_cell_t c;

    if (x < 0 || y < 0)
        return 1;
    if (!ghostty_vt_get_cell(t->vt, (uint16_t)x, (uint16_t)y, &c))
        return 1;

    memset(out, 0, sizeof(*out));
    out->codepoint = c.codepoint;
    out->fg = c.fg_default ? -1 : (int)(c.fg_rgb & 0xFFFFFF);
    out->bg = c.bg_default ? -1 : (int)(c.bg_rgb & 0xFFFFFF);
    out->fg_index = c.fg_index;
    out->bg_index = c.bg_index;
    if (c.flags & GHOSTTY_VT_BOLD)      out->flags |= TA_SHELL_ATTR_BOLD;
    if (c.flags & GHOSTTY_VT_UNDERLINE) out->flags |= TA_SHELL_ATTR_UNDERLINE;
    if (c.flags & GHOSTTY_VT_REVERSE)   out->flags |= TA_SHELL_ATTR_REVERSE;
    if (c.flags & GHOSTTY_VT_ITALIC)    out->flags |= TA_SHELL_ATTR_ITALIC;
    if (c.flags & GHOSTTY_VT_BLINK)     out->flags |= TA_SHELL_ATTR_BLINK;
    if (c.flags & GHOSTTY_VT_STRIKE)    out->flags |= TA_SHELL_ATTR_STRIKE;
    if (c.width == 2)                   out->flags |= TA_SHELL_ATTR_WIDE;
    return 0;
}

static void
g_cursor(ta_shell_term *t, int *x, int *y, int *visible)
{
    uint16_t cx = 0, cy = 0;
    bool     vis = true;

    ghostty_vt_get_cursor(t->vt, &cx, &cy, &vis);
    if (x != NULL) *x = cx;
    if (y != NULL) *y = cy;
    if (visible != NULL) *visible = vis ? 1 : 0;
}

/* Append the full text of row y via the library's own renderer. */
static void
g_row_text(ta_shell_term *t, int y, te_string *out)
{
    char    stackbuf[1024];
    size_t  need;

    if (y < 0)
        return;

    need = ghostty_vt_row_text(t->vt, (uint16_t)y, stackbuf, sizeof(stackbuf));
    if (need < sizeof(stackbuf))
    {
        te_string_append(out, "%.*s", (int)need, stackbuf);
    }
    else
    {
        char *big = TE_ALLOC(need + 1);

        ghostty_vt_row_text(t->vt, (uint16_t)y, big, need + 1);
        te_string_append(out, "%.*s", (int)need, big);
        free(big);
    }
}

static void
g_text(ta_shell_term *t, te_string *out)
{
    int cols = 0, rows = 0;
    int y;

    g_get_size(t, &cols, &rows);
    for (y = 0; y < rows; y++)
    {
        if (y > 0)
            te_string_append(out, "\n");
        g_row_text(t, y, out);
    }
}

static void
g_resize(ta_shell_term *t, int cols, int rows)
{
    ghostty_vt_resize(t->vt, (uint16_t)cols, (uint16_t)rows);
}

static void
g_scrollback(ta_shell_term *t, int max_lines, te_string *out)
{
    char   stackbuf[4096];
    size_t need;

    need = ghostty_vt_scrollback_text(t->vt, max_lines, stackbuf,
                                      sizeof(stackbuf));
    if (need < sizeof(stackbuf))
    {
        te_string_append(out, "%.*s", (int)need, stackbuf);
    }
    else
    {
        char *big = TE_ALLOC(need + 1);

        ghostty_vt_scrollback_text(t->vt, max_lines, big, need + 1);
        te_string_append(out, "%.*s", (int)need, big);
        free(big);
    }
}

static void
g_destroy(ta_shell_term *t)
{
    if (t == NULL)
        return;
    if (t->vt != NULL)
        ghostty_vt_free(t->vt);
    free(t);
}

const ta_shell_term_ops ta_shell_ghostty_ops = {
    .name        = "ghostty",
    .backend_bit = TA_SHELL_BACKEND_GHOSTTY,
    .version     = g_version,
    .create      = g_create,
    .feed        = g_feed,
    .get_size    = g_get_size,
    .cell        = g_cell,
    .cursor      = g_cursor,
    .text        = g_text,
    .row_text    = g_row_text,
    .resize      = g_resize,
    .scrollback  = g_scrollback,
    .destroy     = g_destroy,
};

#endif /* TA_SHELL_HAVE_GHOSTTY */
