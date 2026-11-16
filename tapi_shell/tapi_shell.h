/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Terminal-emulator sessions from a test
 *
 * @defgroup tapi_shell Terminal emulator (tapi_shell)
 * @{
 *
 * Driving an interactive TTY and reading a rendered terminal screen on
 * a Test Agent. A session runs a program under a PTY (or attaches to an
 * existing byte stream - a serial console, a tsf-emu QEMU serial, a
 * socket), feeds its VT/ANSI output into a terminal emulator running in
 * the agent's RPC server process (libghostty-vt, or libvterm), and lets
 * a test type into it (@ref tapi_shell_send, @ref tapi_shell_send_key),
 * wait for output (@ref tapi_shell_expect - the @c expect primitive) and
 * assert on the screen cell by cell (@ref tapi_shell_screen,
 * @ref tapi_shell_cell, @ref tapi_shell_cursor).
 *
 * This is to a text screen what tsf-cv is to a pixel screen: the
 * characters, colours and cursor are known exactly, not guessed. TE's
 * tapi_job runs a program and captures its byte streams, but cannot give
 * it a TTY, type a password into a /dev/tty prompt, render a curses UI
 * or report a cursor - that gap is what this fills.
 *
 * The emulator runs where the program does, in process, linked not
 * spawned; nothing is scraped from another program's output. A session
 * is stateful: @ref tapi_shell_open / @ref tapi_shell_attach return a
 * #tapi_shell_session that the other calls take, and
 * @ref tapi_shell_close ends it.
 *
 * @code
 * tapi_shell_session sh;
 * const char *argv[] = { "sh", "-c", "read x; echo got:$x", NULL };
 *
 * if (!tapi_shell_available(pco))
 *     TEST_SKIP("The agent has no terminal-emulator backend");
 *
 * CHECK_RC(tapi_shell_open(pco, argv, 80, 24, NULL, &sh));
 * tapi_shell_send(&sh, "hello");
 * tapi_shell_send_key(&sh, TAPI_SHELL_KEY_ENTER, 0);
 * if (tapi_shell_expect_str(&sh, "got:hello", 2000) != 0)
 *     TEST_VERDICT("the shell did not echo the line");
 * tapi_shell_close(&sh);
 * @endcode
 */

#ifndef __TAPI_SHELL_H__
#define __TAPI_SHELL_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "rcf_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Emulator backends, as a bitmask. The values are the wire values. */
typedef enum tapi_shell_backend {
    TAPI_SHELL_BACKEND_GHOSTTY = 1, /**< libghostty-vt (preferred). */
    TAPI_SHELL_BACKEND_VTERM   = 2, /**< libvterm (fallback). */
} tapi_shell_backend;

/** How @ref tapi_shell_attach opens the path. The values are wire values. */
typedef enum tapi_shell_attach_kind {
    TAPI_SHELL_ATTACH_PTY    = 1, /**< A pty/char device. */
    TAPI_SHELL_ATTACH_SERIAL = 2, /**< A serial char device. */
    TAPI_SHELL_ATTACH_SOCKET = 3, /**< A unix-domain stream socket. */
} tapi_shell_attach_kind;

/** Where @ref tapi_shell_expect matches. */
typedef enum tapi_shell_where {
    TAPI_SHELL_WHERE_STREAM = 0, /**< The output stream (classic expect). */
    TAPI_SHELL_WHERE_SCREEN = 1, /**< The rendered screen (for TUIs). */
} tapi_shell_where;

/** Cell attribute bits, as reported in #tapi_shell_cell_t.flags. */
typedef enum tapi_shell_attr {
    TAPI_SHELL_ATTR_BOLD      = 1,
    TAPI_SHELL_ATTR_UNDERLINE = 2,
    TAPI_SHELL_ATTR_REVERSE   = 4,
    TAPI_SHELL_ATTR_ITALIC    = 8,
    TAPI_SHELL_ATTR_BLINK     = 16,
    TAPI_SHELL_ATTR_STRIKE    = 32,
    TAPI_SHELL_ATTR_WIDE      = 64, /**< Left cell of a double-width glyph. */
} tapi_shell_attr;

/** Key modifiers for @ref tapi_shell_send_key, as a bitmask. */
typedef enum tapi_shell_mod {
    TAPI_SHELL_MOD_NONE = 0,
    TAPI_SHELL_MOD_ALT  = 1, /**< Prefix the key with ESC (Meta). */
} tapi_shell_mod;

/**
 * A symbolic key. The engine side turns it into the byte sequence an
 * xterm-style terminal sends (cursor keys in normal/DECCKM-off mode),
 * so a test does not hand-write escape sequences. For a plain character
 * use @ref tapi_shell_send; for a control character use
 * @ref tapi_shell_send_ctrl.
 */
typedef enum tapi_shell_key {
    TAPI_SHELL_KEY_ENTER = 1,
    TAPI_SHELL_KEY_TAB,
    TAPI_SHELL_KEY_ESC,
    TAPI_SHELL_KEY_BACKSPACE,
    TAPI_SHELL_KEY_SPACE,
    TAPI_SHELL_KEY_UP,
    TAPI_SHELL_KEY_DOWN,
    TAPI_SHELL_KEY_RIGHT,
    TAPI_SHELL_KEY_LEFT,
    TAPI_SHELL_KEY_HOME,
    TAPI_SHELL_KEY_END,
    TAPI_SHELL_KEY_INSERT,
    TAPI_SHELL_KEY_DELETE,
    TAPI_SHELL_KEY_PGUP,
    TAPI_SHELL_KEY_PGDN,
    TAPI_SHELL_KEY_F1,
    TAPI_SHELL_KEY_F2,
    TAPI_SHELL_KEY_F3,
    TAPI_SHELL_KEY_F4,
    TAPI_SHELL_KEY_F5,
    TAPI_SHELL_KEY_F6,
    TAPI_SHELL_KEY_F7,
    TAPI_SHELL_KEY_F8,
    TAPI_SHELL_KEY_F9,
    TAPI_SHELL_KEY_F10,
    TAPI_SHELL_KEY_F11,
    TAPI_SHELL_KEY_F12,
} tapi_shell_key;

/** A live session. Treat the fields as opaque. */
typedef struct tapi_shell_session {
    rcf_rpc_server *rpcs;   /**< The agent's RPC server. */
    int             handle; /**< The agent-side session handle. */
} tapi_shell_session;

/** Extra options for @ref tapi_shell_open_ex. Zeroed means defaults. */
typedef struct tapi_shell_open_opts {
    const char         *term;    /**< TERM, or @c NULL for xterm-256color. */
    const char *const  *env;     /**< NULL-terminated "KEY=VALUE" or @c NULL. */
    tapi_shell_backend  backend; /**< Force a backend, or 0 for preferred. */
} tapi_shell_open_opts;

/** Options for @ref tapi_shell_expect; zeroed: a substring on the stream. */
typedef struct tapi_shell_expect_opts {
    bool             regex;      /**< @p pattern is a POSIX extended regex. */
    tapi_shell_where where;      /**< Stream (default) or screen. */
    int              timeout_ms; /**< Deadline; <=0 means what is buffered. */
} tapi_shell_expect_opts;

/** The result of @ref tapi_shell_expect. */
typedef struct tapi_shell_match {
    char *text;   /**< The matched text (heap; free with the result). */
    int   x;      /**< SCREEN match start column, else -1. */
    int   y;      /**< SCREEN match start row, else -1. */
    int   offset; /**< STREAM match end byte offset, else -1. */
} tapi_shell_match;

/** A screen snapshot from @ref tapi_shell_screen. */
typedef struct tapi_shell_screen_t {
    int   cols;           /**< Columns. */
    int   rows;           /**< Rows. */
    int   cursor_x;       /**< Cursor column. */
    int   cursor_y;       /**< Cursor row. */
    bool  cursor_visible; /**< Whether the cursor is shown. */
    int   pending_wrap;   /**< 1 when the next character wraps to the next
                               row, 0 when not, -1 when the backend does
                               not report it. */
    char *text;           /**< Rows joined by '\n' (heap; free with result). */
} tapi_shell_screen_t;

/** One cell from @ref tapi_shell_cell. */
typedef struct tapi_shell_cell_t {
    uint32_t     codepoint; /**< Unicode code point, 0 for a blank cell. */
    int          fg;        /**< Foreground 0xRRGGBB, or -1 for default. */
    int          bg;        /**< Background 0xRRGGBB, or -1 for default. */
    int          fg_index;  /**< Palette index 0..255, or -1. */
    int          bg_index;  /**< Palette index 0..255, or -1. */
    unsigned int flags;     /**< #tapi_shell_attr bitmask. */
} tapi_shell_cell_t;

/** The result of @ref tapi_shell_wait. */
typedef struct tapi_shell_status {
    bool exited;      /**< The child has exited. */
    int  exit_code;   /**< Exit status when exited normally, else -1. */
    int  exit_signal; /**< Terminating signal when killed, else 0. */
} tapi_shell_status;

/**
 * Is a terminal-emulator backend usable on the agent?
 *
 * @param rpcs      RPC server on the agent.
 *
 * @return @c true when at least one backend is linked in.
 */
extern bool tapi_shell_available(rcf_rpc_server *rpcs);

/**
 * Does the agent have a specific backend?
 *
 * @param rpcs      RPC server on the agent.
 * @param backend   The backend to ask about.
 *
 * @return @c true when that backend is compiled into the agent.
 */
extern bool tapi_shell_supports(rcf_rpc_server *rpcs,
                                tapi_shell_backend backend);

/**
 * Spawn @p argv under a new PTY and open a session.
 *
 * @param[in]  rpcs     RPC server on the agent.
 * @param[in]  argv     NULL-terminated command (argv[0] first).
 * @param[in]  cols     Columns (<=0 -> 80).
 * @param[in]  rows     Rows (<=0 -> 24).
 * @param[in]  term     TERM for the child, or @c NULL for xterm-256color.
 * @param[out] session  The opened session on success.
 */
extern te_errno tapi_shell_open(rcf_rpc_server *rpcs, const char *const argv[],
                                int cols, int rows, const char *term,
                                tapi_shell_session *session);

/** Like @ref tapi_shell_open with environment and backend options. */
extern te_errno tapi_shell_open_ex(rcf_rpc_server *rpcs,
                                   const char *const argv[], int cols, int rows,
                                   const tapi_shell_open_opts *opts,
                                   tapi_shell_session *session);

/**
 * Attach a session to an existing byte stream on the agent.
 *
 * @param[in]  rpcs     RPC server on the agent.
 * @param[in]  path     Device/socket path on the agent.
 * @param[in]  kind     How to open it.
 * @param[in]  cols     Columns (<=0 -> 80).
 * @param[in]  rows     Rows (<=0 -> 24).
 * @param[out] session  The opened session on success.
 */
extern te_errno tapi_shell_attach(rcf_rpc_server *rpcs, const char *path,
                                  tapi_shell_attach_kind kind, int cols,
                                  int rows, tapi_shell_session *session);

/** Send a NUL-terminated string as input. */
extern te_errno tapi_shell_send(tapi_shell_session *session, const char *str);

/** Send raw bytes as input (control bytes, embedded NULs and all). */
extern te_errno tapi_shell_send_bytes(tapi_shell_session *session,
                                      const void *data, size_t len);

/** Send a printf-formatted string as input. */
extern te_errno tapi_shell_sendf(tapi_shell_session *session, const char *fmt,
                                 ...) TE_LIKE_PRINTF(2, 3);

/** Send a symbolic key (optionally with modifiers). */
extern te_errno tapi_shell_send_key(tapi_shell_session *session,
                                    tapi_shell_key key, unsigned int mods);

/** Send a control character: @p letter 'c' sends Ctrl-C (0x03), etc. */
extern te_errno tapi_shell_send_ctrl(tapi_shell_session *session, char letter);

/**
 * Wait for @p pattern to appear.
 *
 * @param[in]  session  The session.
 * @param[in]  pattern  Substring, or a POSIX ERE when @c opts->regex.
 * @param[in]  opts     Options, or @c NULL for a substring on the stream.
 * @param[out] match    The match (may be @c NULL); free with
 *                      @ref tapi_shell_match_free.
 *
 * @return 0 on a match, @c TE_ETIMEDOUT if none in time.
 */
extern te_errno tapi_shell_expect(tapi_shell_session *session,
                                  const char *pattern,
                                  const tapi_shell_expect_opts *opts,
                                  tapi_shell_match *match);

/** Convenience: wait @p timeout_ms for substring @p sub on the stream. */
extern te_errno tapi_shell_expect_str(tapi_shell_session *session,
                                      const char *sub, int timeout_ms);

/** Free the heap owned by a #tapi_shell_match. */
extern void tapi_shell_match_free(tapi_shell_match *match);

/** Take a screen snapshot; free with @ref tapi_shell_screen_free. */
extern te_errno tapi_shell_screen(tapi_shell_session *session,
                                  tapi_shell_screen_t *screen);

/** Free the heap owned by a #tapi_shell_screen_t. */
extern void tapi_shell_screen_free(tapi_shell_screen_t *screen);

/** Does the current screen contain @p sub anywhere? */
extern bool tapi_shell_screen_contains(tapi_shell_session *session,
                                       const char *sub);

/** Append the text of row @p y (trailing blanks trimmed) to @p out. */
extern te_errno tapi_shell_row_text(tapi_shell_session *session, int y,
                                    te_string *out);

/** Read one cell's code point, colours and attributes. */
extern te_errno tapi_shell_cell(tapi_shell_session *session, int x, int y,
                                tapi_shell_cell_t *cell);

/** Read the cursor position and visibility. */
extern te_errno tapi_shell_cursor(tapi_shell_session *session, int *x, int *y,
                                  bool *visible);

/**
 * Feed @p len bytes straight into the session's emulator, as if the
 * program had written them.
 *
 * The pty is not involved, so the bytes arrive exactly as given and at
 * once. This is how a test drives the emulator itself as the device
 * under test: open a session on an idle program and feed it the escape
 * sequences to render.
 */
extern te_errno tapi_shell_feed(tapi_shell_session *session, const void *data,
                                size_t len);

/** Like @ref tapi_shell_feed for a NUL-terminated string. */
extern te_errno tapi_shell_feed_str(tapi_shell_session *session,
                                    const char *str);

/**
 * Take a snapshot of the session's terminal, decode it into a new
 * terminal and continue the session on the decoded one.
 *
 * A snapshot has to carry everything that decides how later output is
 * rendered, so a session must behave the same with and without this
 * call. Only libghostty-vt has a snapshot format.
 *
 * @return Status code; TE_EOPNOTSUPP when the backend has no snapshots.
 */
extern te_errno tapi_shell_snapshot_restore(tapi_shell_session *session);

/** Append row @p y as newline cell-records (see shell_rpc.x.m4) to @p out. */
extern te_errno tapi_shell_row_attrs(tapi_shell_session *session, int y,
                                     te_string *out);

/** Resize the screen (raises SIGWINCH on the child). */
extern te_errno tapi_shell_resize(tapi_shell_session *session, int cols,
                                  int rows);

/** Append up to @p max_lines scrolled-off lines to @p out (<=0 means all). */
extern te_errno tapi_shell_scrollback(tapi_shell_session *session,
                                      int max_lines, te_string *out);

/** Wait up to @p timeout_ms for the child to exit. */
extern te_errno tapi_shell_wait(tapi_shell_session *session, int timeout_ms,
                                tapi_shell_status *status);

/** End the session and free its agent-side state. */
extern te_errno tapi_shell_close(tapi_shell_session *session);

/**@}*/

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_SHELL_H__ */
