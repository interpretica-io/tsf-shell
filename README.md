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

Two emulator backends, compiled in only when the agent has the library;
`tapi_shell_available()` reports whether any is present, and
`tapi_shell_supports()` names which, so a test skips cleanly rather than
failing when a backend is absent.

- **libghostty-vt** (preferred) — the terminal-state C library of
  Ghostty, used through its public C API (`<ghostty/vt.h>`). It is built
  from the Ghostty sources with **Zig** (`zig build -Demit-lib-vt`),
  which installs a static library and pkg-config files; linked
  statically, it lives inside the agent and the agent host needs nothing
  installed. It is the only backend with a snapshot format
  (`tapi_shell_snapshot_restore`) and the only one that reports a
  pending wrap at the cursor.
- **libvterm** (fallback) — the emulator Neovim embeds. It is an
  ordinary C library with a stable API and ships on most distributions
  (Debian: `libvterm-dev`), so it is the pragmatic backend where Zig is
  not available.

A session uses the preferred backend by default; a test may force one
(for example to compare two renderings of the same escape script) by
passing a backend to `tapi_shell_open_ex` / the attach call.

## Agent host requirements

- **A terminal-emulator backend**: libghostty-vt (preferred; built with
  a Zig toolchain, usually vendored and statically linked), **or**
  libvterm with its development headers (Debian: `libvterm-dev`). At
  least one must be present or the suite skips.
- A Unix agent with a PTY (`forkpty`, `openpty` — `libutil`) and an
  **RPC server** (`ta_rpcprovider`). Injection is the whole point here,
  so the agent runs the program the test drives.

## Usage

Declare the repository in an external libraries catalog and pass it to
`dispatcher.sh --external=<catalog.yml>`:

```yaml
repositories:
  - name: tsf_shell
    url: https://github.com/interpretica-io/tsf-shell.git
    ref: <tag>
    libs:
      - ta_shell
      - rpcs_shell
      - tapi_shell
```

In `builder.conf`, bind `tapi_shell` to the engine, list `ta_shell` and
`rpcs_shell` among the RPC server's libraries, and add the RPC
definitions to both platforms:

```
TE_EXT_REPO_USE([tsf_shell], [ta_shell rpcs_shell], [tapi_shell])

# The shell_* RPCs, on the engine and on the agent platform:
TE_LIB_PARMS([rpcxdr], [${TE_HOST}], [],
             [--with-rpcdefs=tarpc_job.x.m4,../ta_shell/shell_rpc.x.m4])
TE_LIB_PARMS([rpcxdr], [${TE_TA_TYPE}], [],
             [--with-rpcdefs=tarpc_job.x.m4,../ta_shell/shell_rpc.x.m4])

# ta_shell and rpcs_shell in the RPC server application:
TE_TA_APP([ta_rpcprovider], ..., [ta_shell rpcs_shell ...], ...)
```

Then add `tapi_shell` to `te_libs` in the suite's `meson.build`. Requires
TE with `TE_EXT_REPO` support and an agent with an **RPC server**
(`ta_rpcprovider`). The RPC program number is **44** (20–43 are taken by
the other tsf agent RPCs); change it in `shell_rpc.x.m4` if it ever
collides.

## What was verified, and what was not

**Nothing was compiled here.** This environment had no TE toolchain and
no libghostty-vt/libvterm development headers, so `ta_shell.c`, the two
backends, `rpcs_shell.c`, `tapi_shell*.c` and the TE integration (the
three `meson.build`s, `shell_rpc.x.m4`, the `TE_EXT_REPO` wiring) were
written to the `tsf-cv` template but **not built or run**. The companion
suite `tsf-shell-ts` drives every call against deterministic,
agent-local terminal output and is the first thing to build it. The
first real build should expect the ordinary first-build fixes, and in
particular:

- **libvterm**: the colour-classification macros
  (`VTERM_COLOR_IS_DEFAULT_FG`/`_INDEXED`) and
  `vterm_screen_convert_color_to_rgb` are present on modern libvterm
  (≥ 0.1.4) and may need adjusting on an older one.
- PTY controlling-terminal setup is left to `forkpty`; confirm `TERM`
  and the initial `winsize` reach the child, and that `TIOCSWINSZ`
  raises `SIGWINCH`.
- A couple of TE helpers are used on trust: `te_string_append_va` and
  `TE_SEC2MS`.

## Scope

- **A backend must be on the agent.** `tsf-shell` does not install one;
  `tapi_shell_available()` skips cleanly when none is linked in, and
  `tapi_shell_supports()` gates on a specific backend. (`tsf-package`
  can install a dependency as a test's setup.)
- **Reading is reading; typing is a write.** Snapshotting the screen, a
  cell, the cursor and the scrollback only read. Opening a session spawns
  a process and `tapi_shell_send*` types into it — a test does that on
  purpose, never as a side effect of a read.
- **Attach is to a path, for now.** `tapi_shell_attach` opens an existing
  char device, pty or unix socket on the agent — enough for a serial
  console or a `tsf-emu` serial. Attaching directly to a `tapi_job`
  output stream is a later extension (a job owns its fds).
```
