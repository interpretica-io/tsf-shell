# tsf-shell

tsf-shell lets a Test Environment (TE) test drive an interactive program
through a terminal and read what the terminal shows. It is an external
TE repository, consumed with the `TE_EXT_REPO` builder directive.

On the Test Agent it starts a program under a PTY and feeds everything
the program writes into a terminal emulator that is linked into the
agent's RPC server. The test then reads the screen as a grid of cells
(text, cursor, colours, attributes), types into the program and waits
for output, as `expect` does.

TE's `tapi_job` can run a program and capture its output, but it cannot
give the program a TTY, answer a password prompt on `/dev/tty`, render a
curses UI or report a cursor position. tsf-shell does those.

## Libraries

- `ta_shell` (agent side) owns the PTY (`forkpty`), the child process,
  an output buffer and a registry of sessions. The emulation is behind a
  backend interface, `ta_shell_term.h`, with two implementations:
  libghostty-vt (`ta_shell_ghostty.c`) and libvterm (`ta_shell_vterm.c`).
  RPC calls are synchronous and there is no background reader: each call
  first moves pending output into the emulator, and `expect` repeats
  that in a `poll()` loop until a deadline.
- `rpcs_shell` holds the `shell_*` RPCs, thin wrappers over `ta_shell`.
- `tapi_shell` (engine side): `tapi_shell.h` is the API tests use, and
  `tapi_shell_rpc.h` is the layer with one function per RPC.

## Example

```c
tapi_shell_session sh;
const char *argv[] = { "sh", "-c", "read x; echo got:$x", NULL };

if (!tapi_shell_available(pco))
    TEST_SKIP("The agent has no terminal-emulator backend");

CHECK_RC(tapi_shell_open(pco, argv, 80, 24, NULL, &sh));
tapi_shell_send(&sh, "hello");
tapi_shell_send_key(&sh, TAPI_SHELL_KEY_ENTER, 0);
if (tapi_shell_expect_str(&sh, "got:hello", 2000) != 0)
    TEST_VERDICT("the shell did not echo the line");
tapi_shell_close(&sh);
```

## Operations

| Call | What it does |
|---|---|
| `tapi_shell_open` | Start a program under a new PTY and return a session |
| `tapi_shell_attach` | Attach the emulator to an existing serial device, pty or socket |
| `tapi_shell_send` / `_send_bytes` / `_sendf` | Type input: a string, raw bytes, or formatted text |
| `tapi_shell_send_key` / `_send_ctrl` | Type a key (Enter, arrows, F-keys) or a control character |
| `tapi_shell_expect` | Wait for a substring or regex in the output stream or on the screen |
| `tapi_shell_screen` | Screen size, cursor and visible text |
| `tapi_shell_cell` | One cell: code point, colours (0xRRGGBB and palette index), attributes |
| `tapi_shell_cursor` | Cursor position and visibility |
| `tapi_shell_row_text` / `_row_attrs` | One row as text, or each cell of a row as a record |
| `tapi_shell_resize` | Change the screen size; the child gets SIGWINCH |
| `tapi_shell_scrollback` | Lines that scrolled off the top |
| `tapi_shell_feed` / `_feed_str` | Write bytes directly into the emulator, without the pty |
| `tapi_shell_snapshot_restore` | Replace the terminal with the one decoded from its own snapshot (libghostty-vt only) |
| `tapi_shell_wait` / `tapi_shell_close` | Wait for the child to exit; end the session |

With `tapi_shell_feed` the emulator itself is the thing under test: feed
it a fixed escape sequence and compare the screen with the expected
one. With `tapi_shell_attach` a test can read a device's serial console,
or a `tsf-emu` QEMU guest started with `-serial pty` or `-serial unix:`.

### expect: stream or screen

Each `tapi_shell_expect` call matches in one of two places:

- the stream (default): the output since the previous match, as in
  classic `expect`. The result is a byte offset, and the next call
  continues after it.
- the screen: the rendered screen at the moment of the call, for
  programs that place text by cursor movement. The result is the cell
  `(x, y)` where the match starts.

The pattern is a substring or a POSIX extended regex.

### Data on the wire

A screen is text with rows joined by newlines. A cell is a record. A
colour is a packed `0xRRGGBB` integer, or `-1` for the terminal default,
plus a separate palette index. Input travels as a counted byte buffer,
so control bytes and NUL arrive unchanged.

## Backends

A backend is compiled in only when the build host has its library.
`tapi_shell_available()` tells whether any backend is present, and
`tapi_shell_supports()` tells which, so a test can skip instead of
failing.

- libghostty-vt (preferred) is the terminal-state library of Ghostty,
  used through its public C API, `<ghostty/vt.h>`. It is built from the
  Ghostty sources with Zig (`zig build -Demit-lib-vt`), which installs a
  static library and pkg-config files; the static one
  (`libghostty-vt-static.pc`) is used first. Only this backend supports
  snapshots and reports a pending wrap at the cursor
  (`tapi_shell_screen_t.pending_wrap` is `-1` on libvterm).
- libvterm (fallback) is the emulator Neovim uses. Most distributions
  ship it (Debian/Ubuntu: `libvterm-dev`), so it works on hosts without
  Zig.

Sessions use the preferred backend unless a test asks for one with
`tapi_shell_open_ex` or the attach call, for example to compare how the
two render the same input.

The two backends behave the same in these respects:

- Text. A blank cell reads as a space, the right half of a wide
  character is skipped (`tapi_shell_cell` reports it as blank), and
  trailing blanks are removed.
- Scrollback. It holds the newest 2000 lines that scrolled off the top,
  oldest first. libghostty-vt removes history one page at a time, and on
  a narrow screen one page holds thousands of rows, so a line limit would
  sometimes leave almost no history. Its history is therefore limited by
  size instead (64 MiB per session, enough for more than 2000 rows of
  any content up to 1000 columns), and the newest 2000 rows are
  returned.
- Unfinished input. An escape sequence or UTF-8 character split between
  two reads from the pty is completed by the next read. libghostty-vt
  also stores the unfinished part in a snapshot, so a snapshot can be
  taken at any point.

## Requirements

- Build host: a TE build environment and at least one backend that
  pkg-config can find, `libghostty-vt-static` or `vterm`. The Docker
  image of `tsf-shell-ts` has both.
- Agent host: a Unix system with PTYs (`forkpty` from `libutil`) and the
  TE RPC server (`ta_rpcprovider`). libghostty-vt is linked statically
  and needs nothing on the agent; libvterm needs its shared library.

## Usage

Add the repository to the suite's catalog of external repositories
(`conf/external.yml`, or `dispatcher.sh --external=<catalog.yml>`):

```yaml
repositories:
  - name: tsf_shell
    url: https://github.com/interpretica-io/tsf-shell.git
    ref: main
    libs:
      - ta_shell
      - rpcs_shell
      - tapi_shell
```

The Builder writes the commit it built to `conf/builder.conf.lock`;
commit that file to pin the version. In `builder.conf`, declare the
libraries, add the RPC definitions, and link the agent libraries and the
backends into the RPC server. `tsf-shell-ts` uses exactly this:

```
TE_EXT_REPO_USE([tsf_shell], [], [ta_shell rpcs_shell tapi_shell])

# The shell_* RPCs. With one platform for engine and agent, this one
# declaration covers rpcxdr (engine) and rpcxdrta (agent).
TE_LIB_PARMS([rpcxdr], [], [],
             [--with-rpcdefs=tarpc_job.x.m4,../ta_shell/shell_rpc.x.m4])

# The backends the build host has, by pkg-config name.
TS_SHELL_DEPS="libpcre2-8"
for dep in libghostty-vt-static vterm ; do
    pkg-config --exists "$dep" && TS_SHELL_DEPS="$TS_SHELL_DEPS $dep"
done

TE_TA_APP([ta_rpcprovider], [], [linux], [ta_rpcprovider], [], [],
          [ta_shell rpcs_shell ... rpcserver agentlib rpcxdrta ...],
          [\${EXT_SOURCES}/build.sh --extra-deps='${TS_SHELL_DEPS}'],
          [ta_rpcs], [])
```

Put `--extra-deps` in single quotes. The value is a list, and with
double quotes it is split into separate options, which fails with
"Unknown option: libghostty-vt-static".

Then add `tapi_shell` to `te_libs` in the suite's `meson.build`. TE must
support `TE_EXT_REPO`. The RPC program number is 44, because 20 to 43
are used by other tsf modules; change it in `shell_rpc.x.m4` if it
collides.

## Testing

- `ci/lint.sh` checks layout (spaces, no trailing blanks, at most 80
  columns in C), licence headers, and runs cppcheck, shellcheck and
  actionlint.
- `ci/backend_test.c`, run by `ci/backend-test.sh`, builds both backends
  against the real libraries and tests them through the backend
  interface under AddressSanitizer and UBSan: text, cursor, wrap,
  colours and attributes, wide characters, scrollback near the 2000-line
  limit on narrow and wide screens, resize, input split between writes,
  and snapshots. It does not need TE; `ci/stubs` replace the three TE
  headers that the backends include.
- [tsf-shell-ts](https://github.com/interpretica-io/tsf-shell-ts) builds
  the module with TE and tests every call on a real agent, once per
  backend.

GitHub Actions runs the first two on every push and pull request
(`.github/workflows/ci.yml`). To run them on a Linux machine:

```bash
ci/lint.sh
ci/build-ghostty.sh ~/ghostty-vt     # once; versions are in ci/ghostty.env
ci/backend-test.sh ~/ghostty-vt
```

### Known defects in the emulators

The tests expect these to fail: XFAIL in the backend tests, "failed, as
expected" in the suite. They are still checked, and a fixed defect shows
up as an unexpected pass.

| Emulator | Defect |
|---|---|
| libghostty-vt | Cursor backward tab (CBT, `CSI Z`) from the last column moves the cursor but keeps the pending wrap, so the next character goes to the row below the cursor. CUB, CR, CHA and BS clear the pending wrap. A snapshot drops the flag, because it is valid only in the last column, so a restored terminal puts that character somewhere else. |
| libvterm (0.1.4, 0.3.3) | A UTF-8 character split between two writes after other text becomes U+FFFD. The same bytes written one at a time decode correctly. |

## Limitations

- tsf-shell does not install a backend. The build uses what the build
  host has, and `tapi_shell_available()` and `tapi_shell_supports()` let
  tests skip when a backend is missing.
- Reading the screen, a cell, the cursor or the scrollback changes
  nothing. Opening a session starts a process, and `tapi_shell_send*`
  writes to it.
- `tapi_shell_attach` takes a path: a character device, a pty or a Unix
  socket on the agent. Attaching to the output of a `tapi_job` is not
  supported yet, because the job keeps its file descriptors.
