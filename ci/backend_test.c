/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Contract tests for the terminal-emulator backends
 *
 * Every backend compiled in (TA_SHELL_HAVE_GHOSTTY, TA_SHELL_HAVE_VTERM)
 * is driven through the ta_shell_term_ops seam and checked against the
 * same expectations: what ta_shell and the RPCs rely on, whichever
 * emulator renders. It needs the emulator libraries but not the Test
 * Environment (ci/stubs stand in for the three TE headers the backends
 * include), so CI builds and runs it under AddressSanitizer and
 * UndefinedBehaviorSanitizer on every change.
 *
 * A known defect of an emulator (not of this module) is an expected
 * failure: it is reported as XFAIL and does not fail the run, and is
 * reported as XPASS once the emulator is fixed, so the entry can go.
 * The same defects are known issues in the tsf-shell-ts TRC database.
 *
 * Exit status: 0 when every check passed (or failed as expected).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "te_string.h"
#include "ta_shell_term.h"

/** Lines the scrollback reports, on every backend. */
#define SB_LIMIT 2000

static const ta_shell_term_ops *const backends[] = {
#ifdef TA_SHELL_HAVE_GHOSTTY
    &ta_shell_ghostty_ops,
#endif
#ifdef TA_SHELL_HAVE_VTERM
    &ta_shell_vterm_ops,
#endif
};

static const ta_shell_term_ops *ops;
static int n_checks;
static int n_failed;
static int n_xfail;
static int n_xpass;

/* Record one check; @p known names an emulator defect it may hit. */
static void
check(bool ok, const char *known, const char *what, const char *detail)
{
    n_checks++;
    if (ok && known == NULL)
        return;
    if (ok)
    {
        n_xpass++;
        printf("XPASS [%s] %s - the known defect is gone: %s\n", ops->name,
               what, known);
        return;
    }
    if (known != NULL)
    {
        n_xfail++;
        printf("XFAIL [%s] %s (%s)\n", ops->name, what, known);
        return;
    }
    n_failed++;
    printf("FAIL  [%s] %s%s%s\n", ops->name, what,
           detail != NULL ? ": " : "", detail != NULL ? detail : "");
}

static ta_shell_term *
make(int cols, int rows)
{
    ta_shell_term *t = ops->create(cols, rows);

    if (t == NULL)
    {
        printf("FAIL  [%s] cannot create a %dx%d terminal\n", ops->name, cols,
               rows);
        exit(1);
    }
    return t;
}

static void
feed(ta_shell_term *t, const char *s)
{
    ops->feed(t, s, strlen(s));
}

/* Row @p y as text, in a static buffer. */
static const char *
row(ta_shell_term *t, int y)
{
    static char buf[4096];
    te_string   s = TE_STRING_INIT;

    ops->row_text(t, y, &s);
    snprintf(buf, sizeof(buf), "%s", s.ptr != NULL ? s.ptr : "");
    te_string_free(&s);
    return buf;
}

static void
check_row(ta_shell_term *t, int y, const char *want, const char *known,
          const char *what)
{
    char detail[256];
    const char *got = row(t, y);

    /* Bounded so the message always fits: rows can be long. */
    snprintf(detail, sizeof(detail), "row %d is '%.100s', expected '%.100s'",
             y, got, want);
    check(strcmp(got, want) == 0, known, what, detail);
}

static void
check_cursor(ta_shell_term *t, int x, int y, const char *what)
{
    char detail[128];
    int  cx = -1;
    int  cy = -1;
    int  vis = -1;

    ops->cursor(t, &cx, &cy, &vis);
    snprintf(detail, sizeof(detail), "cursor (%d,%d), expected (%d,%d)", cx,
             cy, x, y);
    check(cx == x && cy == y, NULL, what, detail);
}

/* Everything visible of a terminal, for comparing two of them. */
static void
state(ta_shell_term *t, te_string *out)
{
    int cols = 0;
    int rows = 0;
    int x;
    int y;
    int vis = 0;

    ops->get_size(t, &cols, &rows);
    ops->cursor(t, &x, &y, &vis);
    te_string_append(out, "%dx%d (%d,%d) %d\n", cols, rows, x, y, vis);
    ops->text(t, out);
    te_string_append(out, "\n");
    for (y = 0; y < rows; y++)
    {
        for (x = 0; x < cols; x++)
        {
            ta_shell_cell_t c;

            if (ops->cell(t, x, y, &c) == 0)
                te_string_append(out, "%x/%d/%d/%d/%d/%x ", c.codepoint, c.fg,
                                 c.bg, c.fg_index, c.bg_index, c.flags);
        }
        te_string_append(out, "\n");
    }
    ops->scrollback(t, 0, out);
}

static bool
same_state(ta_shell_term *a, ta_shell_term *b)
{
    te_string sa = TE_STRING_INIT;
    te_string sb = TE_STRING_INIT;
    bool      same;

    state(a, &sa);
    state(b, &sb);
    same = strcmp(sa.ptr, sb.ptr) == 0;
    te_string_free(&sa);
    te_string_free(&sb);
    return same;
}

/* ---- the checks --------------------------------------------------- */

static void
test_text_and_cursor(void)
{
    ta_shell_term *t = make(20, 4);
    int            cols = 0;
    int            rows = 0;

    ops->get_size(t, &cols, &rows);
    check(cols == 20 && rows == 4, NULL, "size is reported", NULL);
    feed(t, "hello\r\n  world");
    check_row(t, 0, "hello", NULL, "text reads back");
    check_row(t, 1, "  world", NULL, "leading blanks are kept");
    check_row(t, 2, "", NULL, "an empty row is empty");
    check_cursor(t, 7, 1, "cursor follows the text");
    feed(t, "\033[3;5HX");
    check_row(t, 2, "    X", NULL, "cursor positioning (CUP)");
    check(ops->cell(t, 20, 0, &(ta_shell_cell_t){ 0 }) != 0, NULL,
          "a cell outside the screen is refused", NULL);
    check(ops->cell(t, -1, 0, &(ta_shell_cell_t){ 0 }) != 0, NULL,
          "a negative column is refused", NULL);
    ops->destroy(t);
}

static void
test_wrap(void)
{
    ta_shell_term *t = make(4, 3);

    feed(t, "abcdX");
    check_row(t, 0, "abcd", NULL, "a full row stays");
    check_row(t, 1, "X", NULL, "autowrap continues on the next row");
    ops->destroy(t);

    if (ops->pending_wrap != NULL)
    {
        t = make(4, 3);
        feed(t, "abc");
        check(ops->pending_wrap(t) == 0, NULL, "no pending wrap before the "
              "last column", NULL);
        feed(t, "d");
        check(ops->pending_wrap(t) == 1, NULL, "pending wrap at the last "
              "column", NULL);
        feed(t, "\r");
        check(ops->pending_wrap(t) == 0, NULL, "CR cancels the pending wrap",
              NULL);
        ops->destroy(t);
    }

    /* CBT from the last column, then a character: it belongs at the
     * cursor, on row 0. */
    t = make(4, 3);
    feed(t, "abcd\033[ZX");
    check_row(t, 0, "Xbcd", ops->backend_bit == TA_SHELL_BACKEND_GHOSTTY ?
              "libghostty-vt: CBT keeps the pending wrap" : NULL,
              "back-tab (CBT) cancels the pending wrap");
    ops->destroy(t);
}

static void
test_attributes(void)
{
    ta_shell_term  *t = make(20, 2);
    ta_shell_cell_t c;
    char            detail[128];

    feed(t, "\033[31mR\033[0m\033[38;2;1;2;3mT\033[0m\033[1;4;7mU\033[0m"
            "\033[44mB\033[0mN");

    memset(&c, 0, sizeof(c));
    check(ops->cell(t, 0, 0, &c) == 0 && c.codepoint == 'R', NULL,
          "cell code point", NULL);
    snprintf(detail, sizeof(detail), "fg_index %d fg %#x", c.fg_index, c.fg);
    check(c.fg_index == 1 && c.fg >= 0, NULL,
          "ANSI red is palette index 1 with a resolved colour", detail);

    ops->cell(t, 1, 0, &c);
    snprintf(detail, sizeof(detail), "fg %#x fg_index %d", c.fg, c.fg_index);
    check(c.fg == 0x010203 && c.fg_index == -1, NULL,
          "24-bit colour is exact and not indexed", detail);

    ops->cell(t, 2, 0, &c);
    snprintf(detail, sizeof(detail), "flags %#x", c.flags);
    check((c.flags & TA_SHELL_ATTR_BOLD) &&
          (c.flags & TA_SHELL_ATTR_UNDERLINE) &&
          (c.flags & TA_SHELL_ATTR_REVERSE), NULL,
          "bold, underline and reverse", detail);

    ops->cell(t, 3, 0, &c);
    snprintf(detail, sizeof(detail), "bg_index %d", c.bg_index);
    check(c.bg_index == 4, NULL, "ANSI blue background", detail);

    ops->cell(t, 4, 0, &c);
    snprintf(detail, sizeof(detail), "fg %d bg %d flags %#x", c.fg, c.bg,
             c.flags);
    check(c.fg == -1 && c.bg == -1 && c.flags == 0, NULL,
          "SGR 0 restores the defaults", detail);
    ops->destroy(t);
}

static void
test_wide(void)
{
    ta_shell_term  *t = make(10, 2);
    ta_shell_cell_t c;

    feed(t, "ab\xe4\xb8\xad" "x");
    check_row(t, 0, "ab\xe4\xb8\xad" "x", NULL,
              "a wide glyph reads back once");
    ops->cell(t, 2, 0, &c);
    check(c.codepoint == 0x4e2d && (c.flags & TA_SHELL_ATTR_WIDE), NULL,
          "the left half carries the glyph and the wide flag", NULL);
    ops->cell(t, 3, 0, &c);
    check(c.codepoint == 0, NULL, "the right half is blank", NULL);
    ops->cell(t, 4, 0, &c);
    check(c.codepoint == 'x', NULL, "the next glyph follows the wide one",
          NULL);
    ops->destroy(t);
}

/* Print @p n numbered lines on a @p rows-high screen; check scrollback. */
static void
scroll_case(int cols, int rows, int n)
{
    ta_shell_term *t = make(cols, rows);
    te_string      sb = TE_STRING_INIT;
    char           what[96];
    char           detail[160];
    int            scrolled = n - (rows - 1);
    int            expect = scrolled < SB_LIMIT ? scrolled : SB_LIMIT;
    int            first = scrolled - expect + 1;
    int            got = 0;
    bool           ok = true;
    const char    *p;
    int            i;

    for (i = 1; i <= n; i++)
    {
        char line[32];

        snprintf(line, sizeof(line), "L%05d\r\n", i);
        feed(t, line);
    }
    ops->scrollback(t, 0, &sb);

    p = sb.ptr != NULL ? sb.ptr : "";
    while (*p != '\0' && ok)
    {
        size_t len = strcspn(p, "\n");
        char   want[16];

        snprintf(want, sizeof(want), "L%05d", first + got);
        if (len != strlen(want) || strncmp(p, want, len) != 0)
        {
            snprintf(detail, sizeof(detail), "line %d is '%.*s', expected "
                     "'%s'", got, (int)(len > 64 ? 64 : len), p, want);
            ok = false;
            break;
        }
        got++;
        p += len + (p[len] == '\n');
    }
    if (ok && got != expect)
    {
        snprintf(detail, sizeof(detail), "%d lines, expected %d", got,
                 expect);
        ok = false;
    }
    snprintf(what, sizeof(what), "scrollback after %d lines on %dx%d", n,
             cols, rows);
    check(ok, NULL, what, detail);

    /* max_lines picks the newest. */
    te_string_reset(&sb);
    ops->scrollback(t, 3, &sb);
    snprintf(detail, sizeof(detail), "L%05d\nL%05d\nL%05d", scrolled - 2,
             scrolled - 1, scrolled);
    snprintf(what, sizeof(what), "max_lines=3 after %d lines on %dx%d", n,
             cols, rows);
    check(scrolled >= 3 && sb.ptr != NULL && strcmp(sb.ptr, detail) == 0,
          NULL, what, sb.ptr);

    te_string_free(&sb);
    ops->destroy(t);
}

static void
test_scrollback(void)
{
    static const int counts[] = { 30, 2000, 2024, 2025, 5000 };
    static const int widths[] = { 8, 80, 300 };   /* a line is 6 + CR LF */
    size_t           i;
    size_t           j;

    for (j = 0; j < TE_ARRAY_LEN(widths); j++)
        for (i = 0; i < TE_ARRAY_LEN(counts); i++)
            scroll_case(widths[j], 24, counts[i]);
}

static void
test_resize(void)
{
    ta_shell_term *t = make(10, 3);
    int            cols = 0;
    int            rows = 0;

    feed(t, "abc");
    ops->resize(t, 30, 8);
    ops->get_size(t, &cols, &rows);
    check(cols == 30 && rows == 8, NULL, "resize to a larger screen", NULL);
    check_row(t, 0, "abc", NULL, "text survives growing the screen");
    ops->resize(t, 2, 2);
    ops->get_size(t, &cols, &rows);
    check(cols == 2 && rows == 2, NULL, "resize to a tiny screen", NULL);
    feed(t, "0123456789");
    ops->resize(t, 80, 24);
    ops->get_size(t, &cols, &rows);
    check(cols == 80 && rows == 24, NULL, "resize back", NULL);
    ops->destroy(t);
}

/* The same bytes, cut at @p cut and fed in two writes or one byte at a
 * time, render the same. */
static void
chunk_case(const char *name, const char *bytes, size_t cut,
           const char *known)
{
    ta_shell_term *whole = make(10, 3);
    ta_shell_term *split = make(10, 3);
    size_t         len = strlen(bytes);
    size_t         i;
    char           what[96];

    ops->feed(whole, bytes, len);
    ops->feed(split, bytes, cut);
    ops->feed(split, bytes + cut, len - cut);
    snprintf(what, sizeof(what), "%s: two writes render like one", name);
    check(same_state(whole, split), known, what, NULL);
    ops->destroy(split);

    split = make(10, 3);
    for (i = 0; i < len; i++)
        ops->feed(split, bytes + i, 1);
    snprintf(what, sizeof(what), "%s: byte-wise input renders like one write",
             name);
    check(same_state(whole, split), NULL, what, NULL);
    ops->destroy(split);
    ops->destroy(whole);
}

static void
test_chunking(void)
{
    chunk_case("cut escape sequence", "ab\033[1;3HX", 4, NULL);
    chunk_case("cut UTF-8", "ab\xe4\xb8\xad" "c", 4,
               ops->backend_bit == TA_SHELL_BACKEND_VTERM ?
               "libvterm 0.1.x: UTF-8 cut between writes becomes U+FFFD" :
               NULL);
}

/* Bring two terminals to the same state; restore one from its own
 * snapshot; both must render @p after the same. */
static void
snapshot_case(const char *name, int cols, int rows, const char *before,
              const char *after, const char *known)
{
    ta_shell_term *live = make(cols, rows);
    ta_shell_term *restored = make(cols, rows);
    char           what[96];
    int            rc;

    feed(live, before);
    feed(restored, before);
    rc = ops->snapshot_restore(restored);
    snprintf(what, sizeof(what), "%s: snapshot round trip", name);
    check(rc == 0, NULL, what, NULL);
    feed(live, after);
    feed(restored, after);
    snprintf(what, sizeof(what), "%s: restored renders like live", name);
    check(same_state(live, restored), known, what, NULL);
    ops->destroy(live);
    ops->destroy(restored);
}

static void
test_snapshot(void)
{
    ta_shell_term *t;

    if (ops->snapshot_restore == NULL)
        return;

    snapshot_case("plain text", 10, 3, "hello", " world", NULL);
    snapshot_case("styles", 10, 3, "\033[1;31mab", "cd\033[0me", NULL);
    snapshot_case("cut escape sequence", 10, 3, "ab\033[1;", "3HX", NULL);
    snapshot_case("cut UTF-8", 10, 3, "ab\xe4\xb8", "\xad" "c", NULL);
    snapshot_case("pending wrap", 4, 3, "abcd", "X", NULL);
    snapshot_case("scroll region", 10, 5, "\033[2;4r\033[4;1Hone\r\ntwo",
                  "\r\nthree\r\nfour", NULL);
    snapshot_case("alternate screen", 10, 3, "main\033[?1049halt",
                  "\033[?1049lX", NULL);
    snapshot_case("scrollback", 6, 2, "one\r\ntwo\r\nthree", "\r\nfour",
                  NULL);
    snapshot_case("back-tab from the last column", 4, 3, "abcd\033[Z", "X",
                  "libghostty-vt: CBT keeps the pending wrap, the snapshot "
                  "drops it");

    /* Restoring twice in a row, mid-sequence, still finishes it. */
    t = make(10, 3);
    feed(t, "ab\033[1;");
    check(ops->snapshot_restore(t) == 0, NULL, "snapshot mid-sequence",
          NULL);
    check(ops->snapshot_restore(t) == 0, NULL,
          "a restored terminal can be snapshotted again", NULL);
    feed(t, "3HX");
    check_row(t, 0, "abX", NULL, "the cut sequence completes after two "
              "restores");
    ops->destroy(t);
}

int
main(void)
{
    size_t i;

    if (TE_ARRAY_LEN(backends) == 0)
    {
        printf("no backend compiled in\n");
        return 1;
    }

    for (i = 0; i < TE_ARRAY_LEN(backends); i++)
    {
        int before = n_failed;

        ops = backends[i];
        printf("== %s %s\n", ops->name, ops->version());
        test_text_and_cursor();
        test_wrap();
        test_attributes();
        test_wide();
        test_scrollback();
        test_resize();
        test_chunking();
        test_snapshot();
        printf("== %s: %s\n", ops->name,
               n_failed == before ? "ok" : "FAILED");
    }

    printf("%d checks: %d failed, %d expected failures, %d unexpected "
           "passes\n", n_checks, n_failed, n_xfail, n_xpass);
    return n_failed == 0 ? 0 : 1;
}
