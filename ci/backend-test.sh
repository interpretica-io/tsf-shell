#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# Build ci/backend_test.c with both terminal-emulator backends under
# AddressSanitizer and UndefinedBehaviorSanitizer, and run it.
#
# Usage: ci/backend-test.sh <libghostty-vt prefix>
#
# libvterm is found with pkg-config (Debian/Ubuntu: libvterm-dev). The
# flags match the Test Environment platform build (-D_GNU_SOURCE) plus
# -Werror, so a warning in a backend fails here before it reaches a
# suite build.

set -euo pipefail

prefix="${1:?usage: $0 <libghostty-vt prefix>}"
top="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "${out}"' EXIT

cc="${CC:-cc}"
# shellcheck disable=SC2046 # pkg-config output is a list of flags
"${cc}" -std=c11 -D_GNU_SOURCE -O1 -g \
    -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wstrict-prototypes \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer \
    -DTA_SHELL_HAVE_GHOSTTY -DTA_SHELL_HAVE_VTERM -DGHOSTTY_STATIC \
    -I"${top}/ci/stubs" -I"${top}/ta_shell" -I"${prefix}/include" \
    $(pkg-config --cflags vterm) \
    "${top}/ci/backend_test.c" \
    "${top}/ta_shell/ta_shell_ghostty.c" \
    "${top}/ta_shell/ta_shell_vterm.c" \
    "${prefix}/lib/libghostty-vt.a" \
    $(pkg-config --libs vterm) -lpthread -lm \
    -o "${out}/backend_test"

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}" \
UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" \
    "${out}/backend_test"
