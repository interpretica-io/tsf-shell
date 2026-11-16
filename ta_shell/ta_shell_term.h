/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Terminal-emulator backend seam
 *
 * ta_shell owns the PTY, the child and the stream; the actual VT/ANSI
 * emulation - turning the output byte stream into a screen of cells -
 * is behind this seam so the two backends (libghostty-vt and libvterm)
 * are interchangeable and every library-specific symbol stays in one
 * translation unit.
 *
 * A backend file defines one @ref ta_shell_term_ops with a stable name
 * and a feature bit, compiled in only when its library is present
 * (meson sets @c TA_SHELL_HAVE_GHOSTTY / @c TA_SHELL_HAVE_VTERM).
 * ta_shell.c collects the compiled-in ops into a preference-ordered
 * table (ghostty first) and picks one per session.
 *
 * This header pulls in no backend library header; it uses only plain C
 * types and te_string, so ta_shell.c and both backends share it.
 */

#ifndef __TA_SHELL_TERM_H__
#define __TA_SHELL_TERM_H__

#include <stddef.h>
#include <stdint.h>

#include "te_defs.h"
#include "te_string.h"

/** Backend bits; must equal the TARPC_SHELL_BACKEND_* wire values. */
enum {
    TA_SHELL_BACKEND_GHOSTTY = 1,
    TA_SHELL_BACKEND_VTERM   = 2,
};

/** Cell attribute bits; must equal the TARPC_SHELL_ATTR_* wire values. */
enum {
    TA_SHELL_ATTR_BOLD      = 1,
    TA_SHELL_ATTR_UNDERLINE = 2,
    TA_SHELL_ATTR_REVERSE   = 4,
    TA_SHELL_ATTR_ITALIC    = 8,
    TA_SHELL_ATTR_BLINK     = 16,
    TA_SHELL_ATTR_STRIKE    = 32,
    TA_SHELL_ATTR_WIDE      = 64,
};

/** Cursor flag bits; equal to the shell_screen.out.cursor_flags wire values. */
enum {
    TA_SHELL_CURSOR_VISIBLE      = 1, /**< The cursor is shown. */
    TA_SHELL_CURSOR_PENDING_WRAP = 2, /**< The next character wraps. */
    TA_SHELL_CURSOR_WRAP_KNOWN   = 4, /**< The backend reports bit 2. */
};

/** One rendered cell, backend-neutral. */
typedef struct ta_shell_cell_t {
    uint32_t     codepoint;   /**< Unicode code point, 0 for a blank cell. */
    int          fg;          /**< Foreground 0xRRGGBB, or -1 for default. */
    int          bg;          /**< Background 0xRRGGBB, or -1 for default. */
    int          fg_index;    /**< Palette index 0..255, -1 if not indexed. */
    int          bg_index;    /**< Palette index 0..255, -1 if not indexed. */
    unsigned int flags;       /**< TA_SHELL_ATTR_* bitmask. */
} ta_shell_cell_t;

/** Opaque per-session emulator state owned by a backend. */
typedef struct ta_shell_term ta_shell_term;

/**
 * A terminal-emulator backend: its identity and the operations
 * ta_shell drives it through. All coordinates are 0-based, @c x the
 * column and @c y the row.
 */
typedef struct ta_shell_term_ops {
    /** Stable backend name, e.g. @c "ghostty" or @c "vterm". */
    const char  *name;
    /** TA_SHELL_BACKEND_* bit this backend provides. */
    unsigned int backend_bit;
    /** A short version string of the underlying library (may be @c ""). */
    const char *(*version)(void);

    /** Create a screen of @p cols x @p rows; @c NULL on failure. */
    ta_shell_term *(*create)(int cols, int rows);
    /** Feed @p len output bytes from the stream into the emulator. */
    void (*feed)(ta_shell_term *t, const char *buf, size_t len);
    /** Current screen size. */
    void (*get_size)(ta_shell_term *t, int *cols, int *rows);
    /** One cell; @c 0 ok, non-zero when (x,y) is out of range. */
    int  (*cell)(ta_shell_term *t, int x, int y, ta_shell_cell_t *out);
    /** Cursor column, row and visibility (1/0). */
    void (*cursor)(ta_shell_term *t, int *x, int *y, int *visible);
    /** Whole screen as text: rows joined by '\n', right blanks trimmed. */
    void (*text)(ta_shell_term *t, te_string *out);
    /** One row as text (no newline), right blanks trimmed. */
    void (*row_text)(ta_shell_term *t, int y, te_string *out);
    /** Resize the screen (the child's SIGWINCH is ta_shell's job). */
    void (*resize)(ta_shell_term *t, int cols, int rows);
    /** Up to @p max_lines scrolled-off lines, oldest first, '\n'-joined. */
    void (*scrollback)(ta_shell_term *t, int max_lines, te_string *out);
    /** Destroy the emulator state. */
    void (*destroy)(ta_shell_term *t);

    /*
     * Optional operations: a backend that cannot do one leaves it NULL.
     */

    /**
     * Is the cursor in the "pending wrap" state (the last column was
     * just filled and the next character goes to the next row)?
     * @c 1 yes, @c 0 no, @c -1 when the backend cannot tell.
     */
    int  (*pending_wrap)(ta_shell_term *t);
    /**
     * Encode the terminal to the backend's snapshot format, decode that
     * snapshot into a new terminal and continue the session on the
     * decoded one. @c 0 ok; non-zero when the round trip failed, in
     * which case the live terminal is kept.
     */
    int  (*snapshot_restore)(ta_shell_term *t);
} ta_shell_term_ops;

/*
 * Each backend exposes its ops object under a fixed symbol, referenced
 * by ta_shell.c under the same compile guard that builds the backend.
 */
#ifdef TA_SHELL_HAVE_GHOSTTY
extern const ta_shell_term_ops ta_shell_ghostty_ops;
#endif
#ifdef TA_SHELL_HAVE_VTERM
extern const ta_shell_term_ops ta_shell_vterm_ops;
#endif

#endif /* !__TA_SHELL_TERM_H__ */
