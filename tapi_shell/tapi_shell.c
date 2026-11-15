/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Terminal-emulator sessions from a test
 *
 * The engine-neutral layer over the shell_* RPCs: it builds the newline
 * argv/env the agent wants, turns symbolic keys into the byte sequences
 * an xterm-style terminal sends, and shapes the RPC results into the
 * public structures. The emulation itself runs on the agent.
 */

#define TE_LGR_USER     "TAPI SHELL"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "logger_api.h"

#include "tapi_shell.h"
#include "tapi_shell_rpc.h"

/* ---- helpers ------------------------------------------------------ */

/* Join a NULL-terminated string array into one '\n'-separated string. */
static char *
join_lines(const char *const *items)
{
    te_string s = TE_STRING_INIT;
    int       i;

    if (items != NULL)
    {
        for (i = 0; items[i] != NULL; i++)
            te_string_append(&s, "%s%s", i == 0 ? "" : "\n", items[i]);
    }
    return s.ptr != NULL ? s.ptr : TE_STRDUP("");
}

/* ---- available / supports ----------------------------------------- */

/* See description in tapi_shell.h */
bool
tapi_shell_available(rcf_rpc_server *rpcs)
{
    return rpc_shell_available(rpcs, NULL, NULL) == 0;
}

/* See description in tapi_shell.h */
bool
tapi_shell_supports(rcf_rpc_server *rpcs, tapi_shell_backend backend)
{
    unsigned int features = 0;

    if (rpc_shell_available(rpcs, &features, NULL) != 0)
        return false;

    return (features & (unsigned int)backend) != 0;
}

/* ---- open / attach ------------------------------------------------ */

/* See description in tapi_shell.h */
te_errno
tapi_shell_open_ex(rcf_rpc_server *rpcs, const char *const argv[], int cols,
                   int rows, const tapi_shell_open_opts *opts,
                   tapi_shell_session *session)
{
    tapi_shell_open_opts defaults = { 0 };
    char                *argv_s;
    char                *env_s;
    int                  handle = -1;
    te_errno             rc;

    if (opts == NULL)
        opts = &defaults;

    argv_s = join_lines(argv);
    env_s = join_lines(opts->env);

    rc = rpc_shell_open(rpcs, argv_s, cols, rows, opts->term, env_s,
                        (unsigned int)opts->backend, &handle);

    free(argv_s);
    free(env_s);

    if (rc == 0)
    {
        session->rpcs = rpcs;
        session->handle = handle;
    }
    return rc;
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_open(rcf_rpc_server *rpcs, const char *const argv[], int cols,
                int rows, const char *term, tapi_shell_session *session)
{
    tapi_shell_open_opts opts = { .term = term };

    return tapi_shell_open_ex(rpcs, argv, cols, rows, &opts, session);
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_attach(rcf_rpc_server *rpcs, const char *path,
                  tapi_shell_attach_kind kind, int cols, int rows,
                  tapi_shell_session *session)
{
    int      handle = -1;
    te_errno rc;

    rc = rpc_shell_attach(rpcs, path, (int)kind, cols, rows, 0, &handle);
    if (rc == 0)
    {
        session->rpcs = rpcs;
        session->handle = handle;
    }
    return rc;
}

/* ---- send --------------------------------------------------------- */

/* See description in tapi_shell.h */
te_errno
tapi_shell_send_bytes(tapi_shell_session *session, const void *data, size_t len)
{
    return rpc_shell_send(session->rpcs, session->handle, data, len, NULL);
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_send(tapi_shell_session *session, const char *str)
{
    return tapi_shell_send_bytes(session, str, strlen(str));
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_sendf(tapi_shell_session *session, const char *fmt, ...)
{
    te_string s = TE_STRING_INIT;
    va_list   ap;
    te_errno  rc;

    va_start(ap, fmt);
    te_string_append_va(&s, fmt, ap);
    va_end(ap);

    rc = tapi_shell_send_bytes(session, s.ptr != NULL ? s.ptr : "", s.len);
    te_string_free(&s);
    return rc;
}

/* The byte sequence an xterm-style terminal sends for a symbolic key. */
static const char *
key_sequence(tapi_shell_key key)
{
    switch (key)
    {
        case TAPI_SHELL_KEY_ENTER:     return "\r";
        case TAPI_SHELL_KEY_TAB:       return "\t";
        case TAPI_SHELL_KEY_ESC:       return "\x1b";
        case TAPI_SHELL_KEY_BACKSPACE: return "\x7f";
        case TAPI_SHELL_KEY_SPACE:     return " ";
        case TAPI_SHELL_KEY_UP:        return "\x1b[A";
        case TAPI_SHELL_KEY_DOWN:      return "\x1b[B";
        case TAPI_SHELL_KEY_RIGHT:     return "\x1b[C";
        case TAPI_SHELL_KEY_LEFT:      return "\x1b[D";
        case TAPI_SHELL_KEY_HOME:      return "\x1b[H";
        case TAPI_SHELL_KEY_END:       return "\x1b[F";
        case TAPI_SHELL_KEY_INSERT:    return "\x1b[2~";
        case TAPI_SHELL_KEY_DELETE:    return "\x1b[3~";
        case TAPI_SHELL_KEY_PGUP:      return "\x1b[5~";
        case TAPI_SHELL_KEY_PGDN:      return "\x1b[6~";
        case TAPI_SHELL_KEY_F1:        return "\x1bOP";
        case TAPI_SHELL_KEY_F2:        return "\x1bOQ";
        case TAPI_SHELL_KEY_F3:        return "\x1bOR";
        case TAPI_SHELL_KEY_F4:        return "\x1bOS";
        case TAPI_SHELL_KEY_F5:        return "\x1b[15~";
        case TAPI_SHELL_KEY_F6:        return "\x1b[17~";
        case TAPI_SHELL_KEY_F7:        return "\x1b[18~";
        case TAPI_SHELL_KEY_F8:        return "\x1b[19~";
        case TAPI_SHELL_KEY_F9:        return "\x1b[20~";
        case TAPI_SHELL_KEY_F10:       return "\x1b[21~";
        case TAPI_SHELL_KEY_F11:       return "\x1b[23~";
        case TAPI_SHELL_KEY_F12:       return "\x1b[24~";
        default:                       return NULL;
    }
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_send_key(tapi_shell_session *session, tapi_shell_key key,
                    unsigned int mods)
{
    const char *seq = key_sequence(key);

    if (seq == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);

    if (mods & TAPI_SHELL_MOD_ALT)
    {
        te_string s = TE_STRING_INIT;
        te_errno  rc;

        te_string_append(&s, "\x1b%s", seq);
        rc = tapi_shell_send_bytes(session, s.ptr, s.len);
        te_string_free(&s);
        return rc;
    }
    return tapi_shell_send(session, seq);
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_send_ctrl(tapi_shell_session *session, char letter)
{
    char b = (char)(letter & 0x1f);

    return tapi_shell_send_bytes(session, &b, 1);
}

/* ---- expect ------------------------------------------------------- */

/* See description in tapi_shell.h */
te_errno
tapi_shell_expect(tapi_shell_session *session, const char *pattern,
                  const tapi_shell_expect_opts *opts, tapi_shell_match *match)
{
    tapi_shell_expect_opts defaults = { 0 };
    unsigned int           flags = 0;
    te_string              m = TE_STRING_INIT;
    int                    mx = -1, my = -1, offset = -1;
    te_errno               rc;

    if (opts == NULL)
        opts = &defaults;
    if (opts->regex)
        flags |= 1;                      /* TARPC_SHELL_EXPECT_REGEX */
    if (opts->where == TAPI_SHELL_WHERE_SCREEN)
        flags |= 2;                      /* TARPC_SHELL_EXPECT_SCREEN */

    rc = rpc_shell_expect(session->rpcs, session->handle, pattern, flags,
                          opts->timeout_ms, &m, &mx, &my, &offset);

    if (match != NULL)
    {
        match->text = m.ptr != NULL ? TE_STRDUP(m.ptr) : NULL;
        match->x = mx;
        match->y = my;
        match->offset = offset;
    }
    te_string_free(&m);
    return rc;
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_expect_str(tapi_shell_session *session, const char *sub,
                      int timeout_ms)
{
    tapi_shell_expect_opts opts = {
        .where = TAPI_SHELL_WHERE_STREAM,
        .timeout_ms = timeout_ms,
    };

    return tapi_shell_expect(session, sub, &opts, NULL);
}

/* See description in tapi_shell.h */
void
tapi_shell_match_free(tapi_shell_match *match)
{
    if (match == NULL)
        return;
    free(match->text);
    match->text = NULL;
}

/* ---- screen / cell / cursor --------------------------------------- */

/* See description in tapi_shell.h */
te_errno
tapi_shell_screen(tapi_shell_session *session, tapi_shell_screen_t *screen)
{
    te_string    text = TE_STRING_INIT;
    unsigned int cflags = 0;
    te_errno     rc;

    memset(screen, 0, sizeof(*screen));
    rc = rpc_shell_screen(session->rpcs, session->handle, &screen->cols,
                          &screen->rows, &screen->cursor_x, &screen->cursor_y,
                          &cflags, &text);
    if (rc == 0)
    {
        screen->cursor_visible = (cflags & 1) != 0;
        screen->text = text.ptr != NULL ? TE_STRDUP(text.ptr) : TE_STRDUP("");
    }
    te_string_free(&text);
    return rc;
}

/* See description in tapi_shell.h */
void
tapi_shell_screen_free(tapi_shell_screen_t *screen)
{
    if (screen == NULL)
        return;
    free(screen->text);
    screen->text = NULL;
}

/* See description in tapi_shell.h */
bool
tapi_shell_screen_contains(tapi_shell_session *session, const char *sub)
{
    tapi_shell_screen_t screen;
    bool                found = false;

    if (tapi_shell_screen(session, &screen) == 0 && screen.text != NULL)
        found = strstr(screen.text, sub) != NULL;
    tapi_shell_screen_free(&screen);
    return found;
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_row_text(tapi_shell_session *session, int y, te_string *out)
{
    tapi_shell_screen_t screen;
    te_errno            rc = tapi_shell_screen(session, &screen);
    const char         *p;
    int                 row = 0;

    if (rc != 0)
        return rc;

    /* The screen text is rows joined by '\n'; walk to row y. */
    for (p = screen.text; p != NULL && row < y; row++)
    {
        p = strchr(p, '\n');
        if (p != NULL)
            p++;
    }
    if (p != NULL)
    {
        const char *nl = strchr(p, '\n');
        size_t      len = nl != NULL ? (size_t)(nl - p) : strlen(p);

        te_string_append(out, "%.*s", (int)len, p);
    }
    tapi_shell_screen_free(&screen);
    return 0;
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_cell(tapi_shell_session *session, int x, int y,
                tapi_shell_cell_t *cell)
{
    memset(cell, 0, sizeof(*cell));
    cell->fg = -1;
    cell->bg = -1;
    cell->fg_index = -1;
    cell->bg_index = -1;
    return rpc_shell_cell(session->rpcs, session->handle, x, y,
                          &cell->codepoint, &cell->fg, &cell->bg,
                          &cell->fg_index, &cell->bg_index, &cell->flags);
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_cursor(tapi_shell_session *session, int *x, int *y, bool *visible)
{
    tapi_shell_screen_t screen;
    te_errno            rc = tapi_shell_screen(session, &screen);

    if (rc == 0)
    {
        if (x != NULL) *x = screen.cursor_x;
        if (y != NULL) *y = screen.cursor_y;
        if (visible != NULL) *visible = screen.cursor_visible;
    }
    tapi_shell_screen_free(&screen);
    return rc;
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_row_attrs(tapi_shell_session *session, int y, te_string *out)
{
    return rpc_shell_row_attrs(session->rpcs, session->handle, y, out);
}

/* ---- resize / scrollback / wait / close --------------------------- */

/* See description in tapi_shell.h */
te_errno
tapi_shell_resize(tapi_shell_session *session, int cols, int rows)
{
    return rpc_shell_resize(session->rpcs, session->handle, cols, rows);
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_scrollback(tapi_shell_session *session, int max_lines,
                      te_string *out)
{
    return rpc_shell_scrollback(session->rpcs, session->handle, max_lines, out);
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_wait(tapi_shell_session *session, int timeout_ms,
                tapi_shell_status *status)
{
    int      exited = 0, code = -1, sig = 0;
    te_errno rc = rpc_shell_wait(session->rpcs, session->handle, timeout_ms,
                                 &exited, &code, &sig);

    if (status != NULL)
    {
        status->exited = exited != 0;
        status->exit_code = code;
        status->exit_signal = sig;
    }
    return rc;
}

/* See description in tapi_shell.h */
te_errno
tapi_shell_close(tapi_shell_session *session)
{
    te_errno rc = rpc_shell_close(session->rpcs, session->handle);

    session->handle = -1;
    return rc;
}
