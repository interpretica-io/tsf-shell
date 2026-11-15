/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief RPC for terminal-emulator sessions
 *
 * The RPCs of rpcs_shell, a thin layer over ta_shell, which runs a
 * terminal emulator in the agent's RPC server process: it drives a
 * program under a PTY (or attaches to an existing byte stream), feeds
 * the VT/ANSI output into an in-process terminal emulator
 * (libghostty-vt, or libvterm as a fallback), and reads the rendered
 * screen back as structured data. Add this file to the rpcxdr
 * definitions of the engine platform and of the agent platform:
 *
 *   TE_LIB_PARMS([rpcxdr], [<platform>], [],
 *                [--with-rpcdefs=tarpc_job.x.m4,../ta_shell/shell_rpc.x.m4])
 *
 * Unlike tsf-cv (one call = one image, no state), a shell session is
 * STATEFUL: shell_open/shell_attach return an integer @c handle and the
 * session - the PTY master, the child, the emulator screen and a stream
 * accumulator - lives on the agent between calls, keyed by that handle.
 *
 * There is no portable float on the wire and nothing here needs one:
 * sizes, coordinates and colours are plain integers (a colour is a
 * packed @c 0xRRGGBB, or @c -1 for "terminal default", with a separate
 * palette index when the cell used one). Results that are lists - the
 * per-cell attributes of a row - come back as newline-separated
 * records, one cell per line, the same shape tsf-cv and tsf-smt use,
 * and the engine side parses them. Raw input bytes (shell_send) travel
 * as a counted @c uint8_t buffer, so a control byte or an embedded NUL
 * is carried faithfully.
 */

/*
 * Wire constants (documentary; te_rpcgen takes no anonymous enums, so
 * these live as named C constants in the hand-written headers that use
 * them - ta_shell.h / ta_shell_term.h on the agent, tapi_shell.h on the
 * engine - and are kept in step with the values here):
 *
 *   shell_available.out.features / backend to force:
 *     1 = libghostty-vt (preferred), 2 = libvterm (fallback)
 *
 *   shell_attach.in.kind:
 *     1 = pty/char device (open() O_RDWR O_NOCTTY), 2 = serial char
 *     device (same open path), 3 = unix-domain stream socket (connect())
 *
 *   shell_expect.in.flags (bitmask):
 *     1 = pattern is a POSIX extended regex,
 *     2 = match the rendered screen rather than the output stream
 *
 *   shell_cell.out.flags / the shell_row_attrs records (bitmask):
 *     1 bold, 2 underline, 4 reverse, 8 italic, 16 blink, 32 strike,
 *     64 = left cell of a double-width glyph.
 *   shell_screen.out.cursor_flags: bit 0 = cursor visible.
 */

/*
 * shell_available(): is a terminal-emulator backend compiled into this
 * agent? retval is 0 when at least one is. features is a bitmask of the
 * backends present; version names them (e.g. "ghostty 1.0; vterm 0.3").
 */
struct tarpc_shell_available_in {
    struct tarpc_in_arg common;
};

struct tarpc_shell_available_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_uint      features;
    string          versions<>;   /* "version" is a reserved RPC keyword */
};

/*
 * shell_open(): spawn a program under a fresh PTY and attach an emulator.
 *
 *   argv        the command, one argument per line (argv[0] first)
 *   cols,rows   initial screen size (<=0 means 80x24)
 *   term        TERM for the child, or "" for "xterm-256color"
 *   env         extra environment, one "KEY=VALUE" per line, or ""
 *   backend     a TARPC_SHELL_BACKEND_* to force, or 0 for the preferred
 *   handle      the session handle, >= 0 on success
 */
struct tarpc_shell_open_in {
    struct tarpc_in_arg common;

    string          argv<>;
    tarpc_int       cols;
    tarpc_int       rows;
    string          term<>;
    string          env<>;
    tarpc_uint      backend;
};

struct tarpc_shell_open_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       handle;
};

/*
 * shell_attach(): attach an emulator to an existing byte stream on the
 * agent (a pty/serial char device, or a unix socket). No child is
 * spawned; shell_wait does not apply.
 */
struct tarpc_shell_attach_in {
    struct tarpc_in_arg common;

    string          path<>;
    tarpc_int       kind;       /* TARPC_SHELL_ATTACH_* */
    tarpc_int       cols;
    tarpc_int       rows;
    tarpc_uint      backend;
};

struct tarpc_shell_attach_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       handle;
};

/*
 * shell_send(): write raw bytes to the session as input. Symbolic keys
 * (Enter, Ctrl-C, arrows, F-keys) are turned into their byte sequences
 * by the engine-side TAPI, so this call is a plain counted write.
 *   written     bytes actually written
 */
struct tarpc_shell_send_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    uint8_t         data<>;
};

struct tarpc_shell_send_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_uint      written;
};

/*
 * shell_expect(): drain the session output (feeding the emulator) until
 * @c pattern appears or @c timeout_ms elapses.
 *
 *   flags       TARPC_SHELL_EXPECT_* (regex; match screen vs stream)
 *   timeout_ms  deadline; <=0 means "whatever is already buffered"
 *   retval      0 found, TE_ETIMEDOUT not found in time
 *   match       the matched text
 *   mx,my       for a SCREEN match, the cell where it starts, else -1
 *   offset      for a STREAM match, the byte offset of the match end
 *               (the next expect on this session resumes from there)
 */
struct tarpc_shell_expect_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    string          pattern<>;
    tarpc_uint      flags;
    tarpc_int       timeout_ms;
};

struct tarpc_shell_expect_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    string          match<>;
    tarpc_int       mx;
    tarpc_int       my;
    tarpc_int       offset;
};

/*
 * shell_screen(): a snapshot of the rendered screen.
 *
 *   text          the visible rows, joined by '\n', trailing blanks
 *                 on each row trimmed
 *   cursor_flags  bit 0 = cursor visible
 */
struct tarpc_shell_screen_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
};

struct tarpc_shell_screen_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       cols;
    tarpc_int       rows;
    tarpc_int       cursor_x;
    tarpc_int       cursor_y;
    tarpc_uint      cursor_flags;
    string          text<>;
};

/*
 * shell_cell(): the full attributes of one cell.
 *   codepoint   the Unicode code point (0 for an empty cell)
 *   fg,bg       packed 0xRRGGBB, or -1 for the terminal default
 *   fg_index,bg_index  palette index 0..255 when the cell used one, else -1
 *   flags       TARPC_SHELL_ATTR_*
 */
struct tarpc_shell_cell_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    tarpc_int       x;
    tarpc_int       y;
};

struct tarpc_shell_cell_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_uint      codepoint;
    tarpc_int       fg;
    tarpc_int       bg;
    tarpc_int       fg_index;
    tarpc_int       bg_index;
    tarpc_uint      flags;
};

/*
 * shell_row_attrs(): every cell of row @c y as records, one per line:
 *   "x \t codepoint \t fg \t bg \t fg_index \t bg_index \t flags"
 * (fg/bg are 0xRRGGBB or -1; indices -1 when none).
 */
struct tarpc_shell_row_attrs_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    tarpc_int       y;
};

struct tarpc_shell_row_attrs_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    string          cells<>;
};

/*
 * shell_resize(): change the screen size and raise SIGWINCH on the child
 * (by setting the pty window size).
 */
struct tarpc_shell_resize_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    tarpc_int       cols;
    tarpc_int       rows;
};

struct tarpc_shell_resize_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
};

/*
 * shell_scrollback(): up to @c max_lines lines that have scrolled off
 * the top, oldest first, joined by '\n' (<=0 means all kept).
 */
struct tarpc_shell_scrollback_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    tarpc_int       max_lines;
};

struct tarpc_shell_scrollback_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    string          text<>;
};

/*
 * shell_wait(): wait up to @c timeout_ms for the child to exit.
 *   exited      1 when the child has exited (0 means still running)
 *   exit_code   exit status when exited normally, else -1
 *   exit_signal terminating signal when killed, else 0
 * retval is TE_ETIMEDOUT if still running at the deadline, TE_EINVAL for
 * an attached session (no child).
 */
struct tarpc_shell_wait_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    tarpc_int       timeout_ms;
};

struct tarpc_shell_wait_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       exited;
    tarpc_int       exit_code;
    tarpc_int       exit_signal;
};

/*
 * shell_close(): end the session - kill the child (SIGHUP, then SIGKILL),
 * close the fd, free the emulator and the handle.
 */
struct tarpc_shell_close_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
};

struct tarpc_shell_close_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
};

program shell
{
    version ver0
    {
        RPC_DEF(shell_available)
        RPC_DEF(shell_open)
        RPC_DEF(shell_attach)
        RPC_DEF(shell_send)
        RPC_DEF(shell_expect)
        RPC_DEF(shell_screen)
        RPC_DEF(shell_cell)
        RPC_DEF(shell_row_attrs)
        RPC_DEF(shell_resize)
        RPC_DEF(shell_scrollback)
        RPC_DEF(shell_wait)
        RPC_DEF(shell_close)
    } = 1;
} = 44;
