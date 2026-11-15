/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Shell RPC server library
 *
 * The shell_* RPCs (see shell_rpc.x.m4) on top of ta_shell.
 * TARPC_FUNC_STATIC() binds an RPC to the function of the same name, so
 * each RPC has a plain C function first and the generated wrapper after
 * it. The functions move the te_string results (screen text, matched
 * text, row records) into the RPC's string fields; scalars go straight
 * into the out struct. The sessions themselves live in ta_shell, keyed
 * by the integer handle these calls pass through.
 */

#define TE_LGR_USER     "RPC SHELL"

#include "te_config.h"

#include <string.h>
#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "rpc_server.h"

#include "ta_shell.h"

/* Hand a te_string result over to an RPC string field (never NULL). */
static char *
take(te_string *str)
{
    return str->ptr != NULL ? str->ptr : TE_STRDUP("");
}

/* ---- available ---------------------------------------------------- */

static te_errno
shell_available(unsigned int *features, char **version)
{
    te_string version_s = TE_STRING_INIT;
    te_bool   ok = ta_shell_available(features, &version_s);

    *version = take(&version_s);

    return ok ? 0 : TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);
}

TARPC_FUNC_STATIC(shell_available, {},
{
    unsigned int features = 0;

    MAKE_CALL(out->retval = func(&features, &out->versions));
    out->features = features;
    out->common.errno_changed = false;
})

/* ---- open --------------------------------------------------------- */

static te_errno
shell_open(const char *argv, int cols, int rows, const char *term,
           const char *env, unsigned int backend, int *handle)
{
    return ta_shell_open(argv, cols, rows, term, env, backend, handle);
}

TARPC_FUNC_STATIC(shell_open, {},
{
    int handle = -1;

    MAKE_CALL(out->retval = func(in->argv, in->cols, in->rows, in->term,
                                 in->env, in->backend, &handle));
    out->handle = handle;
})

/* ---- attach ------------------------------------------------------- */

static te_errno
shell_attach(const char *path, int kind, int cols, int rows,
             unsigned int backend, int *handle)
{
    return ta_shell_attach(path, kind, cols, rows, backend, handle);
}

TARPC_FUNC_STATIC(shell_attach, {},
{
    int handle = -1;

    MAKE_CALL(out->retval = func(in->path, in->kind, in->cols, in->rows,
                                 in->backend, &handle));
    out->handle = handle;
})

/* ---- send --------------------------------------------------------- */

static te_errno
shell_send(int handle, const void *data, size_t len, size_t *written)
{
    return ta_shell_send(handle, data, len, written);
}

TARPC_FUNC_STATIC(shell_send, {},
{
    size_t written = 0;

    MAKE_CALL(out->retval = func(in->handle, in->data.data_val,
                                 in->data.data_len, &written));
    out->written = (tarpc_uint)written;
})

/* ---- expect ------------------------------------------------------- */

static te_errno
shell_expect(int handle, const char *pattern, unsigned int flags,
             int timeout_ms, char **match, int *mx, int *my, int *offset)
{
    te_string match_s = TE_STRING_INIT;
    te_errno  rc = ta_shell_expect(handle, pattern, flags, timeout_ms,
                                   &match_s, mx, my, offset);

    *match = take(&match_s);

    return rc;
}

TARPC_FUNC_STATIC(shell_expect, {},
{
    int mx = -1;
    int my = -1;
    int offset = -1;

    MAKE_CALL(out->retval = func(in->handle, in->pattern, in->flags,
                                 in->timeout_ms, &out->match, &mx, &my,
                                 &offset));
    out->mx = mx;
    out->my = my;
    out->offset = offset;
})

/* ---- screen ------------------------------------------------------- */

static te_errno
shell_screen(int handle, int *cols, int *rows, int *cursor_x, int *cursor_y,
             unsigned int *cursor_flags, char **text)
{
    te_string text_s = TE_STRING_INIT;
    te_errno  rc = ta_shell_screen(handle, cols, rows, cursor_x, cursor_y,
                                   cursor_flags, &text_s);

    *text = take(&text_s);

    return rc;
}

TARPC_FUNC_STATIC(shell_screen, {},
{
    int          cols = 0;
    int          rows = 0;
    int          cx = 0;
    int          cy = 0;
    unsigned int cf = 0;

    MAKE_CALL(out->retval = func(in->handle, &cols, &rows, &cx, &cy, &cf,
                                 &out->text));
    out->cols = cols;
    out->rows = rows;
    out->cursor_x = cx;
    out->cursor_y = cy;
    out->cursor_flags = cf;
})

/* ---- cell --------------------------------------------------------- */

static te_errno
shell_cell(int handle, int x, int y, uint32_t *codepoint, int *fg, int *bg,
           int *fg_index, int *bg_index, unsigned int *flags)
{
    return ta_shell_cell(handle, x, y, codepoint, fg, bg, fg_index, bg_index,
                         flags);
}

TARPC_FUNC_STATIC(shell_cell, {},
{
    uint32_t     codepoint = 0;
    int          fg = -1;
    int          bg = -1;
    int          fg_index = -1;
    int          bg_index = -1;
    unsigned int flags = 0;

    MAKE_CALL(out->retval = func(in->handle, in->x, in->y, &codepoint, &fg,
                                 &bg, &fg_index, &bg_index, &flags));
    out->codepoint = codepoint;
    out->fg = fg;
    out->bg = bg;
    out->fg_index = fg_index;
    out->bg_index = bg_index;
    out->flags = flags;
})

/* ---- row_attrs ---------------------------------------------------- */

static te_errno
shell_row_attrs(int handle, int y, char **cells)
{
    te_string cells_s = TE_STRING_INIT;
    te_errno  rc = ta_shell_row_attrs(handle, y, &cells_s);

    *cells = take(&cells_s);

    return rc;
}

TARPC_FUNC_STATIC(shell_row_attrs, {},
{
    MAKE_CALL(out->retval = func(in->handle, in->y, &out->cells));
})

/* ---- resize ------------------------------------------------------- */

static te_errno
shell_resize(int handle, int cols, int rows)
{
    return ta_shell_resize(handle, cols, rows);
}

TARPC_FUNC_STATIC(shell_resize, {},
{
    MAKE_CALL(out->retval = func(in->handle, in->cols, in->rows));
})

/* ---- feed / snapshot ---------------------------------------------- */

static te_errno
shell_feed(int handle, const void *data, size_t len)
{
    return ta_shell_feed(handle, data, len);
}

TARPC_FUNC_STATIC(shell_feed, {},
{
    MAKE_CALL(out->retval = func(in->handle, in->data.data_val,
                                 in->data.data_len));
})

static te_errno
shell_snapshot_restore(int handle)
{
    return ta_shell_snapshot_restore(handle);
}

TARPC_FUNC_STATIC(shell_snapshot_restore, {},
{
    MAKE_CALL(out->retval = func(in->handle));
})

/* ---- scrollback --------------------------------------------------- */

static te_errno
shell_scrollback(int handle, int max_lines, char **text)
{
    te_string text_s = TE_STRING_INIT;
    te_errno  rc = ta_shell_scrollback(handle, max_lines, &text_s);

    *text = take(&text_s);

    return rc;
}

TARPC_FUNC_STATIC(shell_scrollback, {},
{
    MAKE_CALL(out->retval = func(in->handle, in->max_lines, &out->text));
})

/* ---- wait --------------------------------------------------------- */

static te_errno
shell_wait(int handle, int timeout_ms, int *exited, int *exit_code,
           int *exit_signal)
{
    return ta_shell_wait(handle, timeout_ms, exited, exit_code, exit_signal);
}

TARPC_FUNC_STATIC(shell_wait, {},
{
    int exited = 0;
    int exit_code = -1;
    int exit_signal = 0;

    MAKE_CALL(out->retval = func(in->handle, in->timeout_ms, &exited,
                                 &exit_code, &exit_signal));
    out->exited = exited;
    out->exit_code = exit_code;
    out->exit_signal = exit_signal;
})

/* ---- close -------------------------------------------------------- */

static te_errno
shell_close(int handle)
{
    return ta_shell_close(handle);
}

TARPC_FUNC_STATIC(shell_close, {},
{
    MAKE_CALL(out->retval = func(in->handle));
})
