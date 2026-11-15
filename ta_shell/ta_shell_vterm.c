/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Terminal-emulator backend over libvterm
 *
 * The fallback backend (built when @c TA_SHELL_HAVE_VTERM is set): the
 * VT/ANSI state machine and screen model of libvterm (@c <vterm.h>,
 * @c -lvterm), the emulator Neovim embeds. It is linked and called in
 * process; this file is the only place a libvterm symbol appears. It
 * implements the ta_shell_term_ops seam from ta_shell_term.h.
 *
 * Scrollback: libvterm hands lines that scroll off the top to the
 * @c sb_pushline callback; we render each to UTF-8 text and keep the
 * most recent TA_SHELL_VTERM_SB_MAX of them in a ring.
 */

#ifdef TA_SHELL_HAVE_VTERM

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <vterm.h>

#include "te_defs.h"
#include "te_alloc.h"
#include "te_string.h"

#include "ta_shell_term.h"

#define TA_SHELL_VTERM_SB_MAX 2000

struct ta_shell_term {
    VTerm        *vt;
    VTermScreen  *vs;
    VTermState   *state;
    int           cursor_visible;

    /* Scrollback ring of UTF-8 lines (trailing blanks already trimmed). */
    char        **sb;
    int           sb_cap;
    int           sb_len;
    int           sb_head;   /* index of the oldest line */
};

/* ---- small utilities ---------------------------------------------- */

/* Append code point @p cp to @p out as UTF-8. */
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

/* Convert a libvterm colour to packed 0xRRGGBB (or -1 default) plus a
 * palette index when the cell used one (else -1). */
static void
conv_color(VTermScreen *vs, VTermColor col, int *packed, int *index)
{
    *index = -1;

    if (VTERM_COLOR_IS_DEFAULT_FG(&col) || VTERM_COLOR_IS_DEFAULT_BG(&col))
    {
        *packed = -1;
        return;
    }
    if (VTERM_COLOR_IS_INDEXED(&col))
        *index = col.indexed.idx;

    vterm_screen_convert_color_to_rgb(vs, &col);
    *packed = ((int)col.rgb.red << 16) | ((int)col.rgb.green << 8) |
              (int)col.rgb.blue;
}

/* Render one libvterm row of cells to text, trailing blanks trimmed. */
static void
row_cells_to_text(const VTermScreenCell *cells, int cols, te_string *out)
{
    int last = -1;
    int i;

    for (i = 0; i < cols; i++)
        if (cells[i].chars[0] != 0 && cells[i].chars[0] != (uint32_t)' ')
            last = i;

    for (i = 0; i <= last; i++)
    {
        if (cells[i].width == 0)
            continue;   /* right half of a wide glyph */
        append_utf8(out, cells[i].chars[0]);
    }
}

/* ---- scrollback ring ---------------------------------------------- */

static void
sb_push(struct ta_shell_term *t, char *line)
{
    if (t->sb_cap == 0)
    {
        t->sb_cap = 256;
        t->sb = TE_ALLOC(sizeof(char *) * t->sb_cap);
    }
    if (t->sb_len < TA_SHELL_VTERM_SB_MAX && t->sb_len < t->sb_cap)
    {
        t->sb[(t->sb_head + t->sb_len) % t->sb_cap] = line;
        t->sb_len++;
        return;
    }
    if (t->sb_len >= t->sb_cap && t->sb_cap < TA_SHELL_VTERM_SB_MAX)
    {
        int oldcap = t->sb_cap;
        char **grown = TE_ALLOC(sizeof(char *) * oldcap * 2);
        int i;

        for (i = 0; i < t->sb_len; i++)
            grown[i] = t->sb[(t->sb_head + i) % oldcap];
        free(t->sb);
        t->sb = grown;
        t->sb_cap *= 2;
        t->sb_head = 0;
        t->sb[(t->sb_head + t->sb_len) % t->sb_cap] = line;
        t->sb_len++;
        return;
    }
    /* Full: drop the oldest. */
    free(t->sb[t->sb_head]);
    t->sb[t->sb_head] = line;
    t->sb_head = (t->sb_head + 1) % t->sb_cap;
}

/* ---- libvterm callbacks ------------------------------------------- */

static int
cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    struct ta_shell_term *t = user;

    if (prop == VTERM_PROP_CURSORVISIBLE)
        t->cursor_visible = val->boolean;
    return 1;
}

static int
cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    struct ta_shell_term *t = user;
    te_string             line = TE_STRING_INIT;

    row_cells_to_text(cells, cols, &line);
    sb_push(t, line.ptr != NULL ? line.ptr : strdup(""));
    /* ownership of line.ptr moved into the ring; do not free it */
    return 1;
}

static const VTermScreenCallbacks vterm_cbs = {
    .settermprop = cb_settermprop,
    .sb_pushline = cb_sb_pushline,
};

/* ---- ops ---------------------------------------------------------- */

static const char *
vt_version(void)
{
#if defined(VTERM_VERSION_MAJOR) && defined(VTERM_VERSION_MINOR)
    /* Build "major.minor" once. */
    static char buf[16];

    if (buf[0] == '\0')
        snprintf(buf, sizeof(buf), "%d.%d", VTERM_VERSION_MAJOR,
                 VTERM_VERSION_MINOR);
    return buf;
#else
    return "";
#endif
}

static ta_shell_term *
vt_create(int cols, int rows)
{
    struct ta_shell_term *t = TE_ALLOC(sizeof(*t));

    t->cursor_visible = 1;
    t->vt = vterm_new(rows, cols);
    if (t->vt == NULL)
    {
        free(t);
        return NULL;
    }
    vterm_set_utf8(t->vt, 1);
    t->state = vterm_obtain_state(t->vt);
    t->vs = vterm_obtain_screen(t->vt);
    vterm_screen_set_callbacks(t->vs, &vterm_cbs, t);
    vterm_screen_enable_altscreen(t->vs, 1);
    vterm_screen_reset(t->vs, 1);
    return t;
}

static void
vt_feed(ta_shell_term *t, const char *buf, size_t len)
{
    vterm_input_write(t->vt, buf, len);
    vterm_screen_flush_damage(t->vs);
}

static void
vt_get_size(ta_shell_term *t, int *cols, int *rows)
{
    int r = 0, c = 0;

    vterm_get_size(t->vt, &r, &c);
    if (cols != NULL) *cols = c;
    if (rows != NULL) *rows = r;
}

static int
vt_cell(ta_shell_term *t, int x, int y, ta_shell_cell_t *out)
{
    VTermPos        pos = { .row = y, .col = x };
    VTermScreenCell c;
    int             cols = 0, rows = 0;

    vterm_get_size(t->vt, &rows, &cols);
    if (x < 0 || y < 0 || x >= cols || y >= rows)
        return 1;
    if (vterm_screen_get_cell(t->vs, pos, &c) == 0)
        return 1;

    memset(out, 0, sizeof(*out));
    out->codepoint = c.chars[0];
    conv_color(t->vs, c.fg, &out->fg, &out->fg_index);
    conv_color(t->vs, c.bg, &out->bg, &out->bg_index);
    if (c.attrs.bold)      out->flags |= TA_SHELL_ATTR_BOLD;
    if (c.attrs.underline) out->flags |= TA_SHELL_ATTR_UNDERLINE;
    if (c.attrs.italic)    out->flags |= TA_SHELL_ATTR_ITALIC;
    if (c.attrs.blink)     out->flags |= TA_SHELL_ATTR_BLINK;
    if (c.attrs.reverse)   out->flags |= TA_SHELL_ATTR_REVERSE;
    if (c.attrs.strike)    out->flags |= TA_SHELL_ATTR_STRIKE;
    if (c.width == 2)      out->flags |= TA_SHELL_ATTR_WIDE;
    return 0;
}

static void
vt_cursor(ta_shell_term *t, int *x, int *y, int *visible)
{
    VTermPos pos = { 0, 0 };

    vterm_state_get_cursorpos(t->state, &pos);
    if (x != NULL) *x = pos.col;
    if (y != NULL) *y = pos.row;
    if (visible != NULL) *visible = t->cursor_visible;
}

static void
vt_row_text(ta_shell_term *t, int y, te_string *out)
{
    int cols = 0, rows = 0;
    int x;

    vterm_get_size(t->vt, &rows, &cols);
    if (y < 0 || y >= rows)
        return;

    /* Trim trailing blanks: find the last non-blank cell first. */
    {
        int last = -1;

        for (x = 0; x < cols; x++)
        {
            VTermPos        pos = { .row = y, .col = x };
            VTermScreenCell c;

            if (vterm_screen_get_cell(t->vs, pos, &c) != 0 &&
                c.chars[0] != 0 && c.chars[0] != (uint32_t)' ')
                last = x;
        }
        for (x = 0; x <= last; x++)
        {
            VTermPos        pos = { .row = y, .col = x };
            VTermScreenCell c;

            if (vterm_screen_get_cell(t->vs, pos, &c) == 0)
                continue;
            if (c.width == 0)
                continue;
            append_utf8(out, c.chars[0]);
        }
    }
}

static void
vt_text(ta_shell_term *t, te_string *out)
{
    int cols = 0, rows = 0;
    int y;

    vterm_get_size(t->vt, &rows, &cols);
    for (y = 0; y < rows; y++)
    {
        if (y > 0)
            te_string_append(out, "\n");
        vt_row_text(t, y, out);
    }
}

static void
vt_resize(ta_shell_term *t, int cols, int rows)
{
    vterm_set_size(t->vt, rows, cols);
    vterm_screen_flush_damage(t->vs);
}

static void
vt_scrollback(ta_shell_term *t, int max_lines, te_string *out)
{
    int start = 0;
    int i;

    if (max_lines > 0 && max_lines < t->sb_len)
        start = t->sb_len - max_lines;

    for (i = start; i < t->sb_len; i++)
    {
        if (i > start)
            te_string_append(out, "\n");
        te_string_append(out, "%s", t->sb[(t->sb_head + i) % t->sb_cap]);
    }
}

static void
vt_destroy(ta_shell_term *t)
{
    int i;

    if (t == NULL)
        return;
    for (i = 0; i < t->sb_len; i++)
        free(t->sb[(t->sb_head + i) % t->sb_cap]);
    free(t->sb);
    if (t->vt != NULL)
        vterm_free(t->vt);
    free(t);
}

const ta_shell_term_ops ta_shell_vterm_ops = {
    .name        = "vterm",
    .backend_bit = TA_SHELL_BACKEND_VTERM,
    .version     = vt_version,
    .create      = vt_create,
    .feed        = vt_feed,
    .get_size    = vt_get_size,
    .cell        = vt_cell,
    .cursor      = vt_cursor,
    .text        = vt_text,
    .row_text    = vt_row_text,
    .resize      = vt_resize,
    .scrollback  = vt_scrollback,
    .destroy     = vt_destroy,
};

#endif /* TA_SHELL_HAVE_VTERM */
