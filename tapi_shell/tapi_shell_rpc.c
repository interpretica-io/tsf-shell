/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Shell TAPI: RPC client wrappers
 *
 * The rcf_rpc_call() boilerplate behind tapi_shell. The RPCs return
 * te_errno; an RPC transport failure is mapped to TE_ECORRUPTED.
 */

#define TE_LGR_USER     "TAPI SHELL RPC"

#include "te_config.h"

#include <string.h>
#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_time.h"
#include "te_string.h"
#include "logger_api.h"
#include "tapi_rpc_internal.h"
#include "tarpc.h"

#include "tapi_shell_rpc.h"

#define CHECK_RPC_ERRNO_UNCHANGED(_func, _var) \
    CHECK_RETVAL_VAR_ERR_COND(_func, _var, false,                    \
                              TE_RC(TE_TAPI, TE_ECORRUPTED), false)

/* Append an RPC string result, when there is one. */
static void
take_string(te_string *dst, const char *src)
{
    if (dst != NULL && src != NULL)
        te_string_append(dst, "%s", src);
}

/* Store a scalar through a possibly-NULL pointer. */
static void
put(int *p, int v)
{
    if (p != NULL)
        *p = v;
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_available(rcf_rpc_server *rpcs, unsigned int *features,
                    te_string *version)
{
    tarpc_shell_available_in  in;
    tarpc_shell_available_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    rcf_rpc_call(rpcs, "shell_available", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_available, out.retval);
    TAPI_RPC_LOG(rpcs, shell_available, "", "%r feats=0x%x", out.retval,
                 out.features);

    if (out.retval == 0 && features != NULL)
        *features = out.features;
    take_string(version, out.version);

    RETVAL_TE_ERRNO(shell_available, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_open(rcf_rpc_server *rpcs, const char *argv, int cols, int rows,
               const char *term, const char *env, unsigned int backend,
               int *handle)
{
    tarpc_shell_open_in  in;
    tarpc_shell_open_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.argv = (char *)(argv != NULL ? argv : "");
    in.cols = cols;
    in.rows = rows;
    in.term = (char *)(term != NULL ? term : "");
    in.env = (char *)(env != NULL ? env : "");
    in.backend = backend;

    rcf_rpc_call(rpcs, "shell_open", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_open, out.retval);
    TAPI_RPC_LOG(rpcs, shell_open, "%s %dx%d", "%r handle=%d", argv, cols, rows,
                 out.retval, out.handle);

    if (out.retval == 0)
        put(handle, out.handle);

    RETVAL_TE_ERRNO(shell_open, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_attach(rcf_rpc_server *rpcs, const char *path, int kind, int cols,
                 int rows, unsigned int backend, int *handle)
{
    tarpc_shell_attach_in  in;
    tarpc_shell_attach_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.path = (char *)(path != NULL ? path : "");
    in.kind = kind;
    in.cols = cols;
    in.rows = rows;
    in.backend = backend;

    rcf_rpc_call(rpcs, "shell_attach", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_attach, out.retval);
    TAPI_RPC_LOG(rpcs, shell_attach, "%s kind=%d", "%r handle=%d", path, kind,
                 out.retval, out.handle);

    if (out.retval == 0)
        put(handle, out.handle);

    RETVAL_TE_ERRNO(shell_attach, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_send(rcf_rpc_server *rpcs, int handle, const void *data, size_t len,
               size_t *written)
{
    tarpc_shell_send_in  in;
    tarpc_shell_send_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.data.data_val = (uint8_t *)(uintptr_t)data;
    in.data.data_len = (unsigned int)len;

    rcf_rpc_call(rpcs, "shell_send", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_send, out.retval);
    TAPI_RPC_LOG(rpcs, shell_send, "handle=%d len=%u", "%r wrote=%u", handle,
                 (unsigned)len, out.retval, out.written);

    if (out.retval == 0 && written != NULL)
        *written = out.written;

    RETVAL_TE_ERRNO(shell_send, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_expect(rcf_rpc_server *rpcs, int handle, const char *pattern,
                 unsigned int flags, int timeout_ms, te_string *match, int *mx,
                 int *my, int *offset)
{
    tarpc_shell_expect_in  in;
    tarpc_shell_expect_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.pattern = (char *)(pattern != NULL ? pattern : "");
    in.flags = flags;
    in.timeout_ms = timeout_ms;

    /* The call blocks on the agent up to timeout_ms; let it. */
    rpcs->timeout = TE_SEC2MS(10) + (timeout_ms > 0 ? timeout_ms : 0);

    rcf_rpc_call(rpcs, "shell_expect", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_expect, out.retval);
    TAPI_RPC_LOG(rpcs, shell_expect, "handle=%d '%s' flags=0x%x to=%d",
                 "%r mx=%d my=%d off=%d", handle, pattern, flags, timeout_ms,
                 out.retval, out.mx, out.my, out.offset);

    take_string(match, out.match);
    put(mx, out.mx);
    put(my, out.my);
    put(offset, out.offset);

    RETVAL_TE_ERRNO(shell_expect, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_screen(rcf_rpc_server *rpcs, int handle, int *cols, int *rows,
                 int *cursor_x, int *cursor_y, unsigned int *cursor_flags,
                 te_string *text)
{
    tarpc_shell_screen_in  in;
    tarpc_shell_screen_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;

    rcf_rpc_call(rpcs, "shell_screen", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_screen, out.retval);
    TAPI_RPC_LOG(rpcs, shell_screen, "handle=%d", "%r %dx%d cur=%d,%d", handle,
                 out.retval, out.cols, out.rows, out.cursor_x, out.cursor_y);

    if (out.retval == 0)
    {
        put(cols, out.cols);
        put(rows, out.rows);
        put(cursor_x, out.cursor_x);
        put(cursor_y, out.cursor_y);
        if (cursor_flags != NULL)
            *cursor_flags = out.cursor_flags;
        take_string(text, out.text);
    }

    RETVAL_TE_ERRNO(shell_screen, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_cell(rcf_rpc_server *rpcs, int handle, int x, int y,
               uint32_t *codepoint, int *fg, int *bg, int *fg_index,
               int *bg_index, unsigned int *flags)
{
    tarpc_shell_cell_in  in;
    tarpc_shell_cell_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.x = x;
    in.y = y;

    rcf_rpc_call(rpcs, "shell_cell", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_cell, out.retval);
    TAPI_RPC_LOG(rpcs, shell_cell, "handle=%d %d,%d", "%r cp=U+%04X", handle,
                 x, y, out.retval, out.codepoint);

    if (out.retval == 0)
    {
        if (codepoint != NULL)
            *codepoint = out.codepoint;
        put(fg, out.fg);
        put(bg, out.bg);
        put(fg_index, out.fg_index);
        put(bg_index, out.bg_index);
        if (flags != NULL)
            *flags = out.flags;
    }

    RETVAL_TE_ERRNO(shell_cell, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_row_attrs(rcf_rpc_server *rpcs, int handle, int y, te_string *cells)
{
    tarpc_shell_row_attrs_in  in;
    tarpc_shell_row_attrs_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.y = y;

    rcf_rpc_call(rpcs, "shell_row_attrs", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_row_attrs, out.retval);
    TAPI_RPC_LOG(rpcs, shell_row_attrs, "handle=%d y=%d", "%r", handle, y,
                 out.retval);

    take_string(cells, out.cells);

    RETVAL_TE_ERRNO(shell_row_attrs, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_resize(rcf_rpc_server *rpcs, int handle, int cols, int rows)
{
    tarpc_shell_resize_in  in;
    tarpc_shell_resize_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.cols = cols;
    in.rows = rows;

    rcf_rpc_call(rpcs, "shell_resize", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_resize, out.retval);
    TAPI_RPC_LOG(rpcs, shell_resize, "handle=%d %dx%d", "%r", handle, cols,
                 rows, out.retval);

    RETVAL_TE_ERRNO(shell_resize, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_scrollback(rcf_rpc_server *rpcs, int handle, int max_lines,
                     te_string *text)
{
    tarpc_shell_scrollback_in  in;
    tarpc_shell_scrollback_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.max_lines = max_lines;

    rcf_rpc_call(rpcs, "shell_scrollback", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_scrollback, out.retval);
    TAPI_RPC_LOG(rpcs, shell_scrollback, "handle=%d max=%d", "%r", handle,
                 max_lines, out.retval);

    take_string(text, out.text);

    RETVAL_TE_ERRNO(shell_scrollback, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_wait(rcf_rpc_server *rpcs, int handle, int timeout_ms, int *exited,
               int *exit_code, int *exit_signal)
{
    tarpc_shell_wait_in  in;
    tarpc_shell_wait_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.timeout_ms = timeout_ms;

    rpcs->timeout = TE_SEC2MS(10) + (timeout_ms > 0 ? timeout_ms : 0);

    rcf_rpc_call(rpcs, "shell_wait", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_wait, out.retval);
    TAPI_RPC_LOG(rpcs, shell_wait, "handle=%d to=%d",
                 "%r exited=%d code=%d sig=%d", handle, timeout_ms, out.retval,
                 out.exited, out.exit_code, out.exit_signal);

    put(exited, out.exited);
    put(exit_code, out.exit_code);
    put(exit_signal, out.exit_signal);

    RETVAL_TE_ERRNO(shell_wait, out.retval);
}

/* See description in tapi_shell_rpc.h */
te_errno
rpc_shell_close(rcf_rpc_server *rpcs, int handle)
{
    tarpc_shell_close_in  in;
    tarpc_shell_close_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;

    rcf_rpc_call(rpcs, "shell_close", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(shell_close, out.retval);
    TAPI_RPC_LOG(rpcs, shell_close, "handle=%d", "%r", handle, out.retval);

    RETVAL_TE_ERRNO(shell_close, out.retval);
}
