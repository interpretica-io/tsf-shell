/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Shell TAPI: RPC client wrappers
 *
 * Client wrappers of the shell_* RPCs, see shell_rpc.x.m4. Tests use
 * tapi_shell.h; these are the calls behind it, one per RPC. Scalars
 * come back by pointer, text (screen, matched text, row records) in a
 * te_string the engine side parses. The session is identified by the
 * integer @c handle shell_open/shell_attach returned.
 */

#ifndef __TAPI_SHELL_RPC_H__
#define __TAPI_SHELL_RPC_H__

#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "rcf_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** shell_available(): backends bitmask and version; 0 when any present. */
extern te_errno rpc_shell_available(rcf_rpc_server *rpcs,
                                    unsigned int *features,
                                    te_string *version);

/** shell_open(): spawn a program under a PTY; handle >= 0 on success. */
extern te_errno rpc_shell_open(rcf_rpc_server *rpcs, const char *argv,
                               int cols, int rows, const char *term,
                               const char *env, unsigned int backend,
                               int *handle);

/** shell_attach(): attach to an existing byte-stream path on the agent. */
extern te_errno rpc_shell_attach(rcf_rpc_server *rpcs, const char *path,
                                 int kind, int cols, int rows,
                                 unsigned int backend, int *handle);

/** shell_send(): write raw input bytes; *written set to the count. */
extern te_errno rpc_shell_send(rcf_rpc_server *rpcs, int handle,
                               const void *data, size_t len, size_t *written);

/** shell_expect(): wait for @p pattern; 0 found, TE_ETIMEDOUT otherwise. */
extern te_errno rpc_shell_expect(rcf_rpc_server *rpcs, int handle,
                                 const char *pattern, unsigned int flags,
                                 int timeout_ms, te_string *match, int *mx,
                                 int *my, int *offset);

/** shell_screen(): size, cursor and the visible text. */
extern te_errno rpc_shell_screen(rcf_rpc_server *rpcs, int handle, int *cols,
                                 int *rows, int *cursor_x, int *cursor_y,
                                 unsigned int *cursor_flags, te_string *text);

/** shell_cell(): one cell's code point, colours, indices and attr flags. */
extern te_errno rpc_shell_cell(rcf_rpc_server *rpcs, int handle, int x, int y,
                               uint32_t *codepoint, int *fg, int *bg,
                               int *fg_index, int *bg_index,
                               unsigned int *flags);

/** shell_row_attrs(): row @p y as newline records (see shell_rpc.x.m4). */
extern te_errno rpc_shell_row_attrs(rcf_rpc_server *rpcs, int handle, int y,
                                    te_string *cells);

/** shell_resize(): change the screen size (SIGWINCH on the child). */
extern te_errno rpc_shell_resize(rcf_rpc_server *rpcs, int handle, int cols,
                                 int rows);

/** shell_feed(): feed raw bytes straight into the emulator. */
extern te_errno rpc_shell_feed(rcf_rpc_server *rpcs, int handle,
                               const void *data, size_t len);

/** shell_snapshot_restore(): continue on the terminal's own snapshot. */
extern te_errno rpc_shell_snapshot_restore(rcf_rpc_server *rpcs, int handle);

/** shell_scrollback(): up to @p max_lines scrolled-off lines. */
extern te_errno rpc_shell_scrollback(rcf_rpc_server *rpcs, int handle,
                                     int max_lines, te_string *text);

/** shell_wait(): wait for the child to exit; TE_ETIMEDOUT if still up. */
extern te_errno rpc_shell_wait(rcf_rpc_server *rpcs, int handle, int timeout_ms,
                               int *exited, int *exit_code, int *exit_signal);

/** shell_close(): end the session and free the handle. */
extern te_errno rpc_shell_close(rcf_rpc_server *rpcs, int handle);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_SHELL_RPC_H__ */
