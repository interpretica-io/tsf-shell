/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Agent-side terminal-emulator sessions: PTY, registry, expect
 *
 * ta_shell owns everything around the emulator: the PTY (or an attached
 * byte stream), the child process, a raw output accumulator and the
 * session registry. The VT/ANSI emulation itself is behind the backend
 * seam (ta_shell_term.h); this unit never includes a backend library
 * header. RPC is synchronous, so there is no background reader: each
 * call drains pending output into the emulator first, and expect runs
 * that drain in a poll loop against a deadline.
 */

#define TE_LGR_USER     "TA SHELL"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <regex.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>       /* forkpty() on the BSDs/macOS */
#endif

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "logger_api.h"
#include "agentlib.h"

#include "ta_shell.h"
#include "ta_shell_term.h"

/* ---- backend table ------------------------------------------------ */

/*
 * The compiled-in backends, in preference order (ghostty first). Each
 * is referenced under the same guard that builds its translation unit,
 * so a missing library simply drops out of the table.
 */
static const ta_shell_term_ops *const backends[] = {
#ifdef TA_SHELL_HAVE_GHOSTTY
    &ta_shell_ghostty_ops,
#endif
#ifdef TA_SHELL_HAVE_VTERM
    &ta_shell_vterm_ops,
#endif
};
#define N_BACKENDS ((int)(sizeof(backends) / sizeof(backends[0])))

/* The preferred backend, or the one whose bit is in @p want (0 = any). */
static const ta_shell_term_ops *
pick_backend(unsigned int want)
{
    int i;

    for (i = 0; i < N_BACKENDS; i++)
    {
        if (want == 0 || (want & backends[i]->backend_bit) != 0)
            return backends[i];
    }
    return NULL;
}

/* ---- a growable byte buffer (the output stream) ------------------- */

#define TA_SHELL_STREAM_MAX (1u << 20)  /* cap the kept stream at 1 MiB */

typedef struct bytebuf {
    char   *p;
    size_t  len;
    size_t  cap;
} bytebuf;

static void
bytebuf_append(bytebuf *b, const char *data, size_t n, size_t *dropped)
{
    size_t want = b->len + n;

    *dropped = 0;

    if (want > b->cap)
    {
        size_t ncap = b->cap == 0 ? 4096 : b->cap;

        while (ncap < want)
            ncap *= 2;
        b->p = realloc(b->p, ncap);
        b->cap = ncap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;

    /* Keep the tail when we exceed the cap; report what we dropped so
     * the caller can slide its stream search base along with it. */
    if (b->len > TA_SHELL_STREAM_MAX)
    {
        size_t drop = b->len - TA_SHELL_STREAM_MAX;

        memmove(b->p, b->p + drop, b->len - drop);
        b->len -= drop;
        *dropped = drop;
    }
}

/* ---- session registry --------------------------------------------- */

#define TA_SHELL_MAX_SESSIONS 64

typedef struct ta_shell_session {
    te_bool                   in_use;
    int                       master_fd;   /* pty master or attached fd */
    pid_t                     child;       /* -1 for an attached session */
    te_bool                   is_pty;      /* a pty (TIOCSWINSZ applies) */
    const ta_shell_term_ops  *ops;
    ta_shell_term            *term;
    bytebuf                   stream;      /* raw output accumulator */
    size_t                    search_base; /* STREAM expect resumes here */
    te_bool                   reaped;      /* child status already collected */
    int                       exit_code;   /* valid when reaped, else -1 */
    int                       exit_signal; /* valid when reaped, else 0 */
} ta_shell_session;

static ta_shell_session sessions[TA_SHELL_MAX_SESSIONS];

static ta_shell_session *
session_get(int handle)
{
    if (handle < 0 || handle >= TA_SHELL_MAX_SESSIONS ||
        !sessions[handle].in_use)
        return NULL;
    return &sessions[handle];
}

static int
session_alloc(void)
{
    int i;

    for (i = 0; i < TA_SHELL_MAX_SESSIONS; i++)
    {
        if (!sessions[i].in_use)
        {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            sessions[i].in_use = true;
            sessions[i].master_fd = -1;
            sessions[i].child = -1;
            sessions[i].exit_code = -1;
            return i;
        }
    }
    return -1;
}

/* ---- small helpers ------------------------------------------------ */

static int64_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Split a newline-separated string into a NULL-terminated argv. The
 * returned vector and its first element share one allocation with a
 * writable copy of @p text; free(*out_storage) frees all of it.
 */
static char **
split_lines(const char *text, char **out_storage)
{
    char  *copy = TE_STRDUP(text != NULL ? text : "");
    int    n = 0;
    char **vec;
    char  *p;

    for (p = copy; *p != '\0'; p++)
        if (*p == '\n')
            n++;
    n += 2;     /* last line without a trailing newline, plus the NULL */

    vec = TE_ALLOC(sizeof(char *) * n);

    n = 0;
    p = copy;
    if (*p != '\0')
        vec[n++] = p;
    for (; *p != '\0'; p++)
    {
        if (*p == '\n')
        {
            *p = '\0';
            if (p[1] != '\0')
                vec[n++] = p + 1;
        }
    }
    vec[n] = NULL;

    *out_storage = copy;
    return vec;
}

/* Drain whatever is readable within @p timeout_ms into the emulator. */
static void
drain_once(ta_shell_session *s, int timeout_ms)
{
    struct pollfd pfd = { .fd = s->master_fd, .events = POLLIN };
    int           pr;
    char          buf[8192];

    if (s->master_fd < 0)
        return;

    pr = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (pr <= 0)
        return;

    for (;;)
    {
        ssize_t r = read(s->master_fd, buf, sizeof(buf));

        if (r > 0)
        {
            size_t dropped = 0;

            s->ops->feed(s->term, buf, (size_t)r);
            bytebuf_append(&s->stream, buf, (size_t)r, &dropped);
            if (dropped > 0)
                s->search_base = s->search_base > dropped
                                 ? s->search_base - dropped : 0;
            continue;
        }
        if (r == 0)
        {
            /* EOF: the child closed the pty. Nothing more will come. */
            break;
        }
        if (errno == EINTR)
            continue;
        /* EAGAIN (nothing left) or EIO (slave gone) - stop this pass. */
        break;
    }
}

/*
 * Find @p pattern in [buf, buf+len). On a match, set *ms/*me to the
 * match start/end offsets and return true. Regex works on a
 * NUL-terminated copy (terminal text is NUL-free in practice).
 */
static te_bool
find_match(const char *buf, size_t len, const char *pattern, te_bool regex,
           size_t *ms, size_t *me)
{
    if (pattern == NULL || *pattern == '\0')
        return false;

    if (!regex)
    {
        void *hit = memmem(buf, len, pattern, strlen(pattern));

        if (hit == NULL)
            return false;
        *ms = (size_t)((char *)hit - buf);
        *me = *ms + strlen(pattern);
        return true;
    }
    else
    {
        regex_t    re;
        regmatch_t m;
        char      *copy = TE_STRNDUP(buf, len);
        te_bool    ok = false;

        if (regcomp(&re, pattern, REG_EXTENDED | REG_NEWLINE) == 0)
        {
            if (regexec(&re, copy, 1, &m, 0) == 0 && m.rm_so >= 0)
            {
                *ms = (size_t)m.rm_so;
                *me = (size_t)m.rm_eo;
                ok = true;
            }
            regfree(&re);
        }
        free(copy);
        return ok;
    }
}

/* ---- available ---------------------------------------------------- */

/* See description in ta_shell.h */
te_bool
ta_shell_available(unsigned int *features, te_string *version)
{
    unsigned int feats = 0;
    int          i;

    for (i = 0; i < N_BACKENDS; i++)
    {
        feats |= backends[i]->backend_bit;
        if (version != NULL)
            te_string_append(version, "%s%s %s", i == 0 ? "" : "; ",
                             backends[i]->name, backends[i]->version());
    }

    if (features != NULL)
        *features = feats;

    return N_BACKENDS > 0;
}

/* ---- open --------------------------------------------------------- */

/* See description in ta_shell.h */
te_errno
ta_shell_open(const char *argv_nl, int cols, int rows, const char *term,
              const char *env, unsigned int backend, int *handle)
{
    const ta_shell_term_ops *ops = pick_backend(backend);
    struct winsize           ws;
    char                    *argv_store = NULL;
    char                    *env_store = NULL;
    char                   **argv;
    char                   **envv;
    int                      master = -1;
    pid_t                    pid;
    int                      slot;
    ta_shell_session        *s;

    if (ops == NULL)
        return TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);

    if (cols <= 0)
        cols = 80;
    if (rows <= 0)
        rows = 24;

    argv = split_lines(argv_nl, &argv_store);
    if (argv[0] == NULL)
    {
        free(argv); free(argv_store);
        return TE_RC(TE_TA_UNIX, TE_EINVAL);
    }
    envv = split_lines(env, &env_store);

    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0)
    {
        free(argv); free(argv_store); free(envv); free(env_store);
        return TE_OS_RC(TE_TA_UNIX, errno);
    }

    if (pid == 0)
    {
        /* Child: forkpty has already made the pty our controlling tty
         * and dup'd it onto 0/1/2. Apply env, TERM, then exec. */
        int i;

        for (i = 0; envv[i] != NULL; i++)
        {
            char *eq = strchr(envv[i], '=');

            if (eq != NULL)
            {
                *eq = '\0';
                setenv(envv[i], eq + 1, 1);
            }
        }
        setenv("TERM", (term != NULL && *term != '\0') ? term
                        : "xterm-256color", 1);

        execvp(argv[0], argv);
        _exit(127);
    }

    /* Parent. */
    free(argv); free(argv_store); free(envv); free(env_store);
    (void)fcntl(master, F_SETFL, O_NONBLOCK);

    slot = session_alloc();
    if (slot < 0)
    {
        close(master);
        kill(pid, SIGKILL);
        (void)ta_waitpid(pid, NULL, 0);
        return TE_RC(TE_TA_UNIX, TE_ENOMEM);
    }
    s = &sessions[slot];
    s->master_fd = master;
    s->child = pid;
    s->is_pty = true;
    s->ops = ops;
    s->term = ops->create(cols, rows);
    if (s->term == NULL)
    {
        ta_shell_close(slot);
        return TE_RC(TE_TA_UNIX, TE_EFAIL);
    }

    *handle = slot;
    return 0;
}

/* ---- attach ------------------------------------------------------- */

/* See description in ta_shell.h */
te_errno
ta_shell_attach(const char *path, int kind, int cols, int rows,
                unsigned int backend, int *handle)
{
    const ta_shell_term_ops *ops = pick_backend(backend);
    int                      fd = -1;
    int                      slot;
    ta_shell_session        *s;

    if (ops == NULL)
        return TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);
    if (path == NULL || *path == '\0')
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    if (cols <= 0)
        cols = 80;
    if (rows <= 0)
        rows = 24;

    if (kind == TA_SHELL_ATTACH_SOCKET)
    {
        struct sockaddr_un sa;

        if (strlen(path) >= sizeof(sa.sun_path))
            return TE_RC(TE_TA_UNIX, TE_ENAMETOOLONG);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return TE_OS_RC(TE_TA_UNIX, errno);
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        te_strlcpy(sa.sun_path, path, sizeof(sa.sun_path));
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
        {
            int e = errno;

            close(fd);
            return TE_OS_RC(TE_TA_UNIX, e);
        }
    }
    else /* PTY / SERIAL: open the char device both ways. */
    {
        fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0)
            return TE_OS_RC(TE_TA_UNIX, errno);
    }

    (void)fcntl(fd, F_SETFL, O_NONBLOCK);

    slot = session_alloc();
    if (slot < 0)
    {
        close(fd);
        return TE_RC(TE_TA_UNIX, TE_ENOMEM);
    }
    s = &sessions[slot];
    s->master_fd = fd;
    s->child = -1;
    s->is_pty = (kind != TA_SHELL_ATTACH_SOCKET);
    s->ops = ops;
    s->term = ops->create(cols, rows);
    if (s->term == NULL)
    {
        ta_shell_close(slot);
        return TE_RC(TE_TA_UNIX, TE_EFAIL);
    }

    *handle = slot;
    return 0;
}

/* ---- send --------------------------------------------------------- */

/* See description in ta_shell.h */
te_errno
ta_shell_send(int handle, const void *data, size_t len, size_t *written)
{
    ta_shell_session *s = session_get(handle);
    const char       *p = data;
    size_t            off = 0;

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    while (off < len)
    {
        ssize_t w = write(s->master_fd, p + off, len - off);

        if (w > 0)
        {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            /* Let the reader side catch up, then retry. */
            struct pollfd pfd = { .fd = s->master_fd, .events = POLLOUT };

            if (poll(&pfd, 1, 1000) > 0)
                continue;
        }
        break;
    }

    if (written != NULL)
        *written = off;
    return off == len ? 0 : TE_OS_RC(TE_TA_UNIX, errno);
}

/* ---- expect ------------------------------------------------------- */

/* Map a byte offset in the stream to a screen-independent cell: used
 * only for SCREEN matches, where we search the rendered text instead. */
static void
offset_to_cell(const char *text, size_t off, int *x, int *y)
{
    size_t i;

    *x = 0;
    *y = 0;
    for (i = 0; i < off && text[i] != '\0'; i++)
    {
        if (text[i] == '\n')
        {
            (*y)++;
            *x = 0;
        }
        else
        {
            (*x)++;
        }
    }
}

/* See description in ta_shell.h */
te_errno
ta_shell_expect(int handle, const char *pattern, unsigned int flags,
                int timeout_ms, te_string *match, int *mx, int *my,
                int *offset)
{
    ta_shell_session *s = session_get(handle);
    te_bool           regex = (flags & TA_SHELL_EXPECT_REGEX) != 0;
    te_bool           on_screen = (flags & TA_SHELL_EXPECT_SCREEN) != 0;
    int64_t           deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    if (mx != NULL) *mx = -1;
    if (my != NULL) *my = -1;
    if (offset != NULL) *offset = -1;

    for (;;)
    {
        size_t ms = 0, me = 0;
        te_bool found;

        /* Catch up on output first, then test. */
        drain_once(s, 0);

        if (on_screen)
        {
            te_string screen = TE_STRING_INIT;

            s->ops->text(s->term, &screen);
            found = find_match(screen.ptr != NULL ? screen.ptr : "",
                               screen.len, pattern, regex, &ms, &me);
            if (found)
            {
                int x, y;

                offset_to_cell(screen.ptr, ms, &x, &y);
                if (mx != NULL) *mx = x;
                if (my != NULL) *my = y;
                if (match != NULL)
                    te_string_append(match, "%.*s", (int)(me - ms),
                                     screen.ptr + ms);
            }
            te_string_free(&screen);
        }
        else
        {
            const char *base = (s->stream.p != NULL ? s->stream.p : "") +
                               s->search_base;
            size_t      avail = s->stream.len - s->search_base;

            found = find_match(base, avail, pattern, regex, &ms, &me);
            if (found)
            {
                if (match != NULL)
                    te_string_append(match, "%.*s", (int)(me - ms), base + ms);
                s->search_base += me;       /* resume after the match */
                if (offset != NULL)
                    *offset = (int)s->search_base;
            }
        }

        if (found)
            return 0;

        if (now_ms() >= deadline)
            return TE_RC(TE_TA_UNIX, TE_ETIMEDOUT);

        /* Block a short slice for more output, bounded by the deadline. */
        {
            int64_t left = deadline - now_ms();

            drain_once(s, (int)(left < 50 ? left : 50));
        }
    }
}

/* ---- screen / cell / row ------------------------------------------ */

/* See description in ta_shell.h */
te_errno
ta_shell_screen(int handle, int *cols, int *rows, int *cursor_x, int *cursor_y,
                unsigned int *cursor_flags, te_string *text)
{
    ta_shell_session *s = session_get(handle);
    int               vis = 1;
    int               cx = 0, cy = 0;

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    drain_once(s, 0);
    s->ops->get_size(s->term, cols, rows);
    s->ops->cursor(s->term, &cx, &cy, &vis);
    if (cursor_x != NULL) *cursor_x = cx;
    if (cursor_y != NULL) *cursor_y = cy;
    if (cursor_flags != NULL)
    {
        int pending = s->ops->pending_wrap != NULL ?
                      s->ops->pending_wrap(s->term) : -1;

        *cursor_flags = vis ? TA_SHELL_CURSOR_VISIBLE : 0u;
        if (pending >= 0)
            *cursor_flags |= TA_SHELL_CURSOR_WRAP_KNOWN;
        if (pending > 0)
            *cursor_flags |= TA_SHELL_CURSOR_PENDING_WRAP;
    }
    if (text != NULL)
        s->ops->text(s->term, text);

    return 0;
}

/* See description in ta_shell.h */
te_errno
ta_shell_cell(int handle, int x, int y, uint32_t *codepoint, int *fg, int *bg,
              int *fg_index, int *bg_index, unsigned int *flags)
{
    ta_shell_session *s = session_get(handle);
    ta_shell_cell_t     c;

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    drain_once(s, 0);
    if (s->ops->cell(s->term, x, y, &c) != 0)
        return TE_RC(TE_TA_UNIX, TE_ESRCH);

    if (codepoint != NULL) *codepoint = c.codepoint;
    if (fg != NULL) *fg = c.fg;
    if (bg != NULL) *bg = c.bg;
    if (fg_index != NULL) *fg_index = c.fg_index;
    if (bg_index != NULL) *bg_index = c.bg_index;
    if (flags != NULL) *flags = c.flags;

    return 0;
}

/* See description in ta_shell.h */
te_errno
ta_shell_row_attrs(int handle, int y, te_string *cells)
{
    ta_shell_session *s = session_get(handle);
    int               cols = 0, rows = 0;
    int               x;

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    drain_once(s, 0);
    s->ops->get_size(s->term, &cols, &rows);
    if (y < 0 || y >= rows)
        return TE_RC(TE_TA_UNIX, TE_ESRCH);

    for (x = 0; x < cols; x++)
    {
        ta_shell_cell_t c;

        if (s->ops->cell(s->term, x, y, &c) != 0)
            continue;
        te_string_append(cells, "%d\t%u\t%d\t%d\t%d\t%d\t%u\n",
                         x, c.codepoint, c.fg, c.bg, c.fg_index, c.bg_index,
                         c.flags);
    }
    return 0;
}

/* ---- resize ------------------------------------------------------- */

/* See description in ta_shell.h */
te_errno
ta_shell_resize(int handle, int cols, int rows)
{
    ta_shell_session *s = session_get(handle);

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);
    if (cols <= 0 || rows <= 0)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    if (s->is_pty)
    {
        struct winsize ws;

        memset(&ws, 0, sizeof(ws));
        ws.ws_col = (unsigned short)cols;
        ws.ws_row = (unsigned short)rows;
        /* Setting the window size is what raises SIGWINCH on the child. */
        (void)ioctl(s->master_fd, TIOCSWINSZ, &ws);
    }
    s->ops->resize(s->term, cols, rows);

    return 0;
}

/* ---- feed / snapshot ---------------------------------------------- */

/* See description in ta_shell.h */
te_errno
ta_shell_feed(int handle, const void *data, size_t len)
{
    ta_shell_session *s = session_get(handle);

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    /* Keep the order: what the program already wrote comes first. */
    drain_once(s, 0);
    if (len > 0)
        s->ops->feed(s->term, data, len);

    return 0;
}

/* See description in ta_shell.h */
te_errno
ta_shell_snapshot_restore(int handle)
{
    ta_shell_session *s = session_get(handle);

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);
    if (s->ops->snapshot_restore == NULL)
        return TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);

    drain_once(s, 0);
    if (s->ops->snapshot_restore(s->term) != 0)
        return TE_RC(TE_TA_UNIX, TE_EFAIL);

    return 0;
}

/* ---- scrollback --------------------------------------------------- */

/* See description in ta_shell.h */
te_errno
ta_shell_scrollback(int handle, int max_lines, te_string *text)
{
    ta_shell_session *s = session_get(handle);

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    drain_once(s, 0);
    if (text != NULL)
        s->ops->scrollback(s->term, max_lines, text);

    return 0;
}

/* ---- wait --------------------------------------------------------- */

static void
store_status(ta_shell_session *s, int status)
{
    s->reaped = true;
    if (WIFEXITED(status))
    {
        s->exit_code = WEXITSTATUS(status);
        s->exit_signal = 0;
    }
    else if (WIFSIGNALED(status))
    {
        s->exit_code = -1;
        s->exit_signal = WTERMSIG(status);
    }
}

/* See description in ta_shell.h */
te_errno
ta_shell_wait(int handle, int timeout_ms, int *exited, int *exit_code,
              int *exit_signal)
{
    ta_shell_session *s = session_get(handle);
    int64_t           deadline;

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);
    if (s->child < 0)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);   /* an attached session */

    if (exited != NULL) *exited = 0;
    if (exit_code != NULL) *exit_code = -1;
    if (exit_signal != NULL) *exit_signal = 0;

    deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);

    for (;;)
    {
        if (!s->reaped)
        {
            int   status;
            pid_t r = ta_waitpid(s->child, &status, WNOHANG);

            if (r == s->child)
                store_status(s, status);
            else if (r < 0 && errno != EINTR)
                return TE_OS_RC(TE_TA_UNIX, errno);
        }

        if (s->reaped)
        {
            if (exited != NULL) *exited = 1;
            if (exit_code != NULL) *exit_code = s->exit_code;
            if (exit_signal != NULL) *exit_signal = s->exit_signal;
            return 0;
        }

        if (now_ms() >= deadline)
            return TE_RC(TE_TA_UNIX, TE_ETIMEDOUT);

        /* Keep draining while we wait, so output is not lost; this also
         * paces the loop (it blocks up to the slice on the pty). */
        drain_once(s, 20);
    }
}

/* ---- close -------------------------------------------------------- */

/* See description in ta_shell.h */
te_errno
ta_shell_close(int handle)
{
    ta_shell_session *s = session_get(handle);

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);

    if (s->child > 0 && !s->reaped)
    {
        int   i;
        int   status;

        kill(s->child, SIGHUP);
        for (i = 0; i < 20; i++)      /* ~200 ms grace */
        {
            if (ta_waitpid(s->child, &status, WNOHANG) == s->child)
            {
                s->reaped = true;
                break;
            }
            {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };

                nanosleep(&ts, NULL);
            }
        }
        if (!s->reaped)
        {
            kill(s->child, SIGKILL);
            (void)ta_waitpid(s->child, &status, 0);
        }
    }

    if (s->term != NULL && s->ops != NULL)
        s->ops->destroy(s->term);
    if (s->master_fd >= 0)
        close(s->master_fd);
    free(s->stream.p);

    memset(s, 0, sizeof(*s));
    return 0;
}
