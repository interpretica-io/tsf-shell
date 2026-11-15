# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# External TE library (see TE_EXT_REPO): agent-side terminal-emulator
# sessions. ta_shell owns the PTY, the child and the session registry;
# the VT/ANSI emulation is behind a backend seam with two optional
# backends, libghostty-vt (preferred) and libvterm (fallback). The
# library is linked and called in-process; nothing is spawned to scrape.
# The agent and its RPC server both link it.
#
# At least one backend must be found or the suite's tapi_shell_available()
# reports none and the tests skip. Each backend is compiled in only when
# its library is present, and the only backend-specific code is its own
# translation unit.

headers += files(
    'ta_shell.h',
    'ta_shell_term.h',
)

sources += files(
    'ta_shell.c',
)

te_libs += [
    'logger_core',
    'tools',
]

cc = meson.get_compiler('c')

shell_defs = []

# libghostty-vt: the zero-dependency terminal-state C library extracted
# from Ghostty (preferred). It is normally vendored and statically linked
# by the Builder (building it needs a Zig toolchain on the build host);
# detect it here by pkg-config, then by a bare library probe. The exact
# package/header name is confirmed at first build - see the binding
# surface at the top of ta_shell_ghostty.c; pass -DTA_SHELL_GHOSTTY_HEADER
# once <ghostty/vt.h> is on the include path.
ghostty_dep = dependency('ghostty-vt', required: false)
if not ghostty_dep.found()
    ghostty_dep = dependency('libghostty-vt', required: false)
endif
if not ghostty_dep.found()
    ghostty_dep = cc.find_library('ghostty-vt', required: false)
endif
if not ghostty_dep.found()
    ghostty_dep = cc.find_library('ghostty_vt', required: false)
endif
if ghostty_dep.found()
    deps += [ ghostty_dep ]
    sources += files('ta_shell_ghostty.c')
    shell_defs += '-DTA_SHELL_HAVE_GHOSTTY'
    if cc.has_header('ghostty/vt.h', dependencies: ghostty_dep)
        shell_defs += '-DTA_SHELL_GHOSTTY_HEADER'
    endif
endif

# libvterm: the emulator Neovim embeds (fallback). Ships vterm.pc on most
# distributions (Debian: libvterm-dev); fall back to a bare probe.
vterm_dep = dependency('vterm', required: false)
if not vterm_dep.found()
    vterm_dep = cc.find_library('vterm', has_headers: 'vterm.h', required: false)
endif
if vterm_dep.found()
    deps += [ vterm_dep ]
    sources += files('ta_shell_vterm.c')
    shell_defs += '-DTA_SHELL_HAVE_VTERM'
endif

if shell_defs.length() == 0
    warning('tsf-shell: neither libghostty-vt nor libvterm found; ' +
            'ta_shell will report no backend and the suite will skip.')
else
    deps += declare_dependency(compile_args: shell_defs)
endif

# forkpty() lives in libutil on Linux (glibc) and libutil on the BSDs.
util_dep = cc.find_library('util', required: false)
if util_dep.found()
    deps += [ util_dep ]
endif
