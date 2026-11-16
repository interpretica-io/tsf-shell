/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Agent-side terminal-emulator sessions
 *
 * A terminal emulator running in the agent's own RPC server process:
 * spawn a program under a PTY (or attach to an existing byte stream),
 * feed its VT/ANSI output into an in-process emulator (libghostty-vt,
 * or libvterm; see ta_shell_term.h) and read the rendered screen back.
 * The library is linked and called, nothing is spawned to scrape. The
 * agent and its RPC server both link this; the RPCs (see shell_rpc.x.m4)
 * are thin wrappers over these functions.
 *
 * Sessions are stateful. ta_shell keeps a small fixed registry of them,
 * keyed by a non-negative integer @c handle that shell_open/shell_attach
 * return; every other call takes that handle. RPC is synchronous and
 * there is no background reader: a call that needs fresh state drains
 * the session's output (non-blocking) first, and ta_shell_expect() runs
 * that drain in a poll loop until its pattern matches or time runs out.
 *
 * No float crosses the seam; colours are packed 0xRRGGBB (or -1 for the
 * terminal default) with a separate palette index. Row attribute dumps
 * come back as newline records, one cell per line, the tsf-cv shape.
 */

#ifndef __TA_SHELL_H__
#define __TA_SHELL_H__

#include <stddef.h>
#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"

/** Backends, reported by ta_shell_available(). Wire values. */
enum {
    TA_SHELL_FEAT_GHOSTTY = 1, /**< libghostty-vt (preferred). */
    TA_SHELL_FEAT_VTERM   = 2, /**< libvterm (fallback). */
};

/** shell_attach kind. Wire values. */
enum {
    TA_SHELL_ATTACH_PTY    = 1, /**< pty/char device, open() O_RDWR O_NOCTTY. */
    TA_SHELL_ATTACH_SERIAL = 2, /**< serial char device, same open path. */
    TA_SHELL_ATTACH_SOCKET = 3, /**< unix stream socket, connect(). */
};

/** ta_shell_expect() flags. Wire values. */
enum {
    TA_SHELL_EXPECT_REGEX  = 1, /**< pattern is a POSIX extended regex. */
    TA_SHELL_EXPECT_SCREEN = 2, /**< match the rendered screen, not stream. */
};

/**
 * Which backends are compiled into this agent, and their versions.
 *
 * @param[out] features  Bitmask of @c TA_SHELL_FEAT_*.
 * @param[out] version   Human string naming the backends and versions.
 *
 * @return @c true when at least one backend is present.
 */
extern te_bool ta_shell_available(unsigned int *features, te_string *version);

/**
 * Spawn @p argv under a new PTY and attach an emulator.
 *
 * @param[in]  argv     Command, one argument per line (argv[0] first).
 * @param[in]  cols     Initial columns (<=0 -> 80).
 * @param[in]  rows     Initial rows (<=0 -> 24).
 * @param[in]  term     TERM for the child, or "" for "xterm-256color".
 * @param[in]  env      Extra env, one "KEY=VALUE" per line, or "".
 * @param[in]  backend  A TA_SHELL_FEAT_* to force, or 0 for preferred.
 * @param[out] handle   Session handle, >= 0 on success.
 */
extern te_errno ta_shell_open(const char *argv, int cols, int rows,
                              const char *term, const char *env,
                              unsigned int backend, int *handle);

/**
 * Attach an emulator to an existing byte-stream @p path on the agent.
 *
 * @param[in]  path     Device/socket path on the agent.
 * @param[in]  kind     One of @c TA_SHELL_ATTACH_*.
 * @param[in]  cols     Columns (<=0 -> 80).
 * @param[in]  rows     Rows (<=0 -> 24).
 * @param[in]  backend  A TA_SHELL_FEAT_* to force, or 0 for preferred.
 * @param[out] handle   Session handle, >= 0 on success.
 */
extern te_errno ta_shell_attach(const char *path, int kind, int cols, int rows,
                                unsigned int backend, int *handle);

/** Write @p len input bytes to the session; *written set to the count. */
extern te_errno ta_shell_send(int handle, const void *data, size_t len,
                              size_t *written);

/**
 * Drain the session output into the emulator until @p pattern matches or
 * @p timeout_ms elapses.
 *
 * @param[in]  handle      Session.
 * @param[in]  pattern     Substring, or a POSIX ERE with @c _REGEX.
 * @param[in]  flags       @c TA_SHELL_EXPECT_* bits.
 * @param[in]  timeout_ms  Deadline; <=0 means only what is buffered now.
 * @param[out] match       The matched text.
 * @param[out] mx,my       SCREEN match start cell, else -1.
 * @param[out] offset      STREAM match end byte offset, else -1.
 *
 * @return 0 on a match, @c TE_ETIMEDOUT if none in time.
 */
extern te_errno ta_shell_expect(int handle, const char *pattern,
                                unsigned int flags, int timeout_ms,
                                te_string *match, int *mx, int *my,
                                int *offset);

/** A screen snapshot: size, cursor and the visible text. */
extern te_errno ta_shell_screen(int handle, int *cols, int *rows, int *cursor_x,
                                int *cursor_y, unsigned int *cursor_flags,
                                te_string *text);

/** One cell's code point, colours (0xRRGGBB/-1), indices and attr flags. */
extern te_errno ta_shell_cell(int handle, int x, int y, uint32_t *codepoint,
                              int *fg, int *bg, int *fg_index, int *bg_index,
                              unsigned int *flags);

/** Row @p y as newline records, one cell per line (see shell_rpc.x.m4). */
extern te_errno ta_shell_row_attrs(int handle, int y, te_string *cells);

/** Resize the screen and raise SIGWINCH on the child via the pty. */
extern te_errno ta_shell_resize(int handle, int cols, int rows);

/** Up to @p max_lines scrolled-off lines, oldest first, '\n'-joined. */
extern te_errno ta_shell_scrollback(int handle, int max_lines, te_string *text);

/**
 * Wait up to @p timeout_ms for the child to exit.
 *
 * @param[out] exited       1 when the child has exited, else 0.
 * @param[out] exit_code    Exit status when exited normally, else -1.
 * @param[out] exit_signal  Terminating signal when killed, else 0.
 *
 * @return 0 on exit, @c TE_ETIMEDOUT while still running, @c TE_EINVAL
 *         for an attached session (no child).
 */
extern te_errno ta_shell_wait(int handle, int timeout_ms, int *exited,
                              int *exit_code, int *exit_signal);

/**
 * Feed @p len bytes straight into the session's emulator, as if the
 * program had written them. Nothing goes through the pty, so the bytes
 * arrive exactly as given and at once - the way to drive the emulator
 * itself as the device under test.
 */
extern te_errno ta_shell_feed(int handle, const void *data, size_t len);

/**
 * Take a snapshot of the session's terminal, decode it into a new
 * terminal and continue the session on the decoded one.
 *
 * @return Status code; TE_EOPNOTSUPP when the backend has no snapshot
 *         format, TE_EFAIL when the round trip failed.
 */
extern te_errno ta_shell_snapshot_restore(int handle);

/** End the session: kill the child, close the fd, free everything. */
extern te_errno ta_shell_close(int handle);

#endif /* !__TA_SHELL_H__ */
