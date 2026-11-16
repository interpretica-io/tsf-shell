# tsf-shell

Driving an interactive TTY and reading a rendered terminal screen on a
Test Agent, packaged as an external Test Environment (TE) repository
(consumed with the `TE_EXT_REPO` builder directive). It runs a program
under a PTY on the agent, feeds the VT/ANSI bytes the program writes
into a **terminal emulator linked into the agent's RPC server process —
no Python, nothing spawned to scrape** — and reads the screen back as a
grid of cells: the text, the cursor, the colours and attributes. It
types into the program too (keys, control characters) and waits for
output (the `expect` primitive).

It is to a text screen what `tsf-cv` is to a pixel screen: the
characters, colours and cursor are known exactly, not guessed — "OCR for
a terminal that never has to recognise anything."

Three libraries:

- `ta_shell` — agent side. It owns the PTY (`forkpty`), the child, a raw
  output accumulator and a small session registry; the VT/ANSI
  emulation itself is behind a backend seam (`ta_shell_term.h`) with two
  interchangeable backends, **libghostty-vt** (`ta_shell_ghostty.c`) and
  **libvterm** (`ta_shell_vterm.c`). RPC is synchronous, so there is no
  background reader: a call drains pending output into the emulator
  first, and `expect` runs that drain in a `poll()` loop against a
  deadline. The agent and its RPC server both link it.
- `rpcs_shell` — the `shell_*` RPCs for the agent's RPC server, thin
  wrappers over `ta_shell`.
- `tapi_shell` — engine side. `tapi_shell.h` gives a test the high-level
  calls — open/attach a session, send, send a symbolic key, expect,
  snapshot the screen, read a cell, the cursor, resize, scrollback,
  wait, close; `tapi_shell_rpc.h` is the one-per-RPC layer beneath it.

TE's `tapi_job` runs a program and captures its byte streams, but cannot
give it a TTY, type a password into a `/dev/tty` prompt, render a curses
UI or report a cursor. `tsf-input` drives the kernel input layer
(evdev/uinput), which is not a TTY. That gap is what this fills.

## What it reads, and writes

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
| `tapi_shell_open` | Spawn a program under a new PTY; return a session |
| `tapi_shell_attach` | Attach the emulator to an existing byte stream (serial/pty/socket) |
| `tapi_shell_send` / `_send_bytes` / `_sendf` | Type input (string / raw bytes / formatted) |
| `tapi_shell_send_key` / `_send_ctrl` | Type a symbolic key (Enter, arrows, F-keys) or a control char |
| `tapi_shell_expect` | Wait for a substring or regex, on the output stream or the rendered screen |
| `tapi_shell_screen` | Snapshot: size, cursor, and the visible text |
| `tapi_shell_cell` | One cell: code point, fg/bg (0xRRGGBB and palette index), bold/underline/… |
| `tapi_shell_cursor` | Cursor position and visibility |
| `tapi_shell_row_text` / `_row_attrs` | A row as text, or every cell of a row as records |
| `tapi_shell_resize` | Change the screen size (raises SIGWINCH on the child) |
| `tapi_shell_scrollback` | Lines that have scrolled off the top |
| `tapi_shell_feed` / `_feed_str` | Put bytes straight into the emulator, bypassing the pty — the emulator itself as the device under test |
| `tapi_shell_snapshot_restore` | Replace the terminal by the one decoded from its own snapshot (libghostty-vt only) |
| `tapi_shell_wait` / `tapi_shell_close` | Wait for the child to exit; end the session |

Two things this makes possible beyond scripting a CLI: **testing a
terminal emulator as the DUT** — feed it a fixed escape script and
compare the rendered screen to a golden grid, a programmatic `vttest` —
and **attaching to a serial console** — a device's boot console, or a
`tsf-emu` QEMU serial exposed as `-serial pty`/`-serial unix:`.

## expect: stream or screen

`tapi_shell_expect` matches in one of two places, chosen per call:

- **stream** (the default) — the linear output since the last match, the
  classic line-oriented `expect`. The match reports a byte offset and
  the next expect resumes after it.
- **screen** — the rendered screen as it stands now, for a TUI that
  positions text rather than printing it in order. The match reports the
  cell `(x, y)` where it starts.

Either can be a plain substring or a POSIX extended regex.

## It is a linked library, not a program

`ta_shell` does not run `tmux`, `screen`, `expect` or `script` and parse
their output. It opens a PTY itself, and the terminal emulation is a
library compiled into the agent's RPC server process: the output bytes
are handed to libghostty-vt (or libvterm), and the screen comes back as
the library's own cell grid. Across the RPC a screen is text with the
rows joined by newlines, a cell is a record, and a colour is a packed
`0xRRGGBB` integer (or `-1` for the terminal default) with a separate
palette index — no float crosses the wire. Input bytes travel as a
counted `uint8_t` buffer, so a control byte or an embedded NUL is
carried faithfully.

## Backends

Two emulator backends, compiled in only when the build host has the
library; `tapi_shell_available()` reports whether any is present, and
`tapi_shell_supports()` names which, so a test skips cleanly rather than
failing when a backend is absent.

- **libghostty-vt** (preferred) — the terminal-state C library of
  Ghostty, used through its public C API (`<ghostty/vt.h>`). It is built
  from the Ghostty sources with **Zig** (`zig build -Demit-lib-vt`),
  which installs a static library and pkg-config files
  (`libghostty-vt-static.pc` is taken first). Linked statically, it lives
  inside the agent's RPC server. It is the only backend with a snapshot
  format (`tapi_shell_snapshot_restore`) and the only one that reports a
  pending wrap at the cursor (`tapi_shell_screen_t.pending_wrap`; `-1`
  on libvterm).
- **libvterm** (fallback) — the emulator Neovim embeds, an ordinary C
  library on most distributions (Debian/Ubuntu: `libvterm-dev`), for
  hosts without a Zig toolchain.

A session uses the preferred backend by default; a test may force one
(for example to compare two renderings of the same escape script) by
passing a backend to `tapi_shell_open_ex` / the attach call.

Both backends give the same answers where the module defines them:

- **Text** is assembled from cells the same way: a blank cell is a
  space, the right half of a wide glyph is skipped (and reads as a blank
  cell with `tapi_shell_cell`), trailing blanks are trimmed.
- **Scrollback** is the newest **2000** lines that scrolled off the top,
  oldest first. libghostty-vt drops history a whole page at a time and a
  page holds up to thousands of rows on a narrow screen, so its history
  is bounded in bytes (64 MiB per session, which holds well over 2000
  rows of any content up to 1000 columns) and the newest 2000 rows are
  reported — not bounded by its line limit, which would let the history
  fall far below the limit.
- **Unfinished input** — an escape sequence or a UTF-8 character cut
  between two reads of the pty — is completed by the next read. On
  libghostty-vt it is also carried by a snapshot, so a snapshot can be
  taken at any point.

## Build and agent host requirements

- **Build host**: a TE build environment and at least one backend where
  pkg-config finds it — `libghostty-vt-static` (from a Zig build of
  Ghostty) and/or `vterm` (`libvterm-dev`). `tsf-shell-ts` carries a
  Docker image that has both (libghostty-vt built from a pinned Ghostty
  commit with a checksum-verified Zig).
- **Agent host**: a Unix agent with a PTY (`forkpty` — `libutil`) and an
  **RPC server** (`ta_rpcprovider`). libghostty-vt is statically linked
  and needs nothing installed there; libvterm needs its shared library.
  The agent runs the program the test drives.

## Usage

Declare the repository in the suite's external libraries catalog
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

The Builder records the commit it built in `conf/builder.conf.lock`;
commit that file to pin it. In `builder.conf`, declare the libraries,
add the RPC definitions, and link the agent libraries and the backends
into the RPC server (this is the wiring `tsf-shell-ts` builds and runs):

```
TE_EXT_REPO_USE([tsf_shell], [], [ta_shell rpcs_shell tapi_shell])

# The shell_* RPCs. With one platform for engine and agent, one
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

Quote `--extra-deps` with **single** quotes: the value is a list, and
double quotes let it be split into separate options ("Unknown option:
libghostty-vt-static").

Then add `tapi_shell` to `te_libs` in the suite's `meson.build`. Requires
TE with `TE_EXT_REPO` support. The RPC program number is **44** (20–43
are taken by the other tsf agent RPCs); change it in `shell_rpc.x.m4` if
it ever collides.

## Verification

Three levels, each run on every change:

- **Static checks** (`ci/lint.sh`): layout (spaces, no trailing blanks,
  80 columns in C), licence and copyright headers, cppcheck, shellcheck
  and actionlint.
- **Backend contract tests** (`ci/backend_test.c`, run by
  `ci/backend-test.sh`): both backends, built against the real libraries,
  driven through the backend seam under AddressSanitizer and UBSan —
  text, cursor, wrap, colours and attributes, wide glyphs, scrollback at
  and around the 2000-line limit on narrow and wide screens, resize,
  input cut between writes, and snapshots. They need no TE tree:
  `ci/stubs` stand in for the three TE headers the backends include.
- **The suite** — [tsf-shell-ts](https://github.com/interpretica-io/tsf-shell-ts)
  builds the module with TE and runs every call on a real agent, once per
  backend.

GitHub Actions runs the first two (`.github/workflows/ci.yml`). To run
them locally on Linux:

```bash
ci/lint.sh
ci/build-ghostty.sh ~/ghostty-vt     # once; versions in ci/ghostty.env
ci/backend-test.sh ~/ghostty-vt
```

### Known defects of the emulators

The tests treat these as expected failures (XFAIL in the backend tests,
"failed, as expected" in the suite's TRC): they keep being checked, and
show up as an unexpected pass once the emulator is fixed.

| Emulator | Defect |
|---|---|
| libghostty-vt | Cursor backward tab (CBT, `CSI Z`) from the last column moves the cursor but keeps the pending wrap, so the next character lands one row below the cursor; CUB, CR, CHA and BS clear it. A snapshot drops the flag (it is valid only at the last column), so a restored terminal renders that character in a different place. |
| libvterm 0.1.x | A UTF-8 character cut between two writes after other text decodes as U+FFFD; the same bytes one at a time decode correctly. |

## Scope

- **A backend must be in the build.** `tsf-shell` does not install one:
  the build links what the build host has, `tapi_shell_available()`
  skips cleanly when nothing was linked in, and `tapi_shell_supports()`
  gates on a specific backend.
- **Reading is reading; typing is a write.** Snapshotting the screen, a
  cell, the cursor and the scrollback only read. Opening a session spawns
  a process and `tapi_shell_send*` types into it — a test does that on
  purpose, never as a side effect of a read.
- **Attach is to a path, for now.** `tapi_shell_attach` opens an existing
  char device, pty or unix socket on the agent — enough for a serial
  console or a `tsf-emu` serial. Attaching directly to a `tapi_job`
  output stream is a later extension (a job owns its fds).
```
