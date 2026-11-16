#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# Build libghostty-vt into a prefix: fetch the pinned Zig (checksum
# verified), fetch the pinned Ghostty commit, "zig build -Demit-lib-vt".
# The versions are in ci/ghostty.env.
#
# Usage: ci/build-ghostty.sh <prefix> [<work-dir>]
#
# Linux x86_64 and aarch64 hosts only (the Zig release is chosen by the
# host architecture).

set -euo pipefail

prefix="${1:?usage: $0 <prefix> [<work-dir>]}"
work="${2:-$(mktemp -d)}"
here="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source-path=SCRIPTDIR source=ghostty.env
. "${here}/ghostty.env"

case "$(uname -m)" in
    x86_64)        zig_arch=x86_64  ; zig_sha="${ZIG_SHA256_X86_64}" ;;
    aarch64|arm64) zig_arch=aarch64 ; zig_sha="${ZIG_SHA256_AARCH64}" ;;
    *) echo "unsupported architecture $(uname -m)" >&2 ; exit 1 ;;
esac
if [ "$(uname -s)" != "Linux" ] ; then
    echo "Linux hosts only" >&2
    exit 1
fi

mkdir -p "${work}/zig"
tarball="zig-${zig_arch}-linux-${ZIG_VERSION}.tar.xz"
curl -fsSL -o "${work}/${tarball}" \
    "https://ziglang.org/download/${ZIG_VERSION}/${tarball}"
echo "${zig_sha}  ${work}/${tarball}" | sha256sum -c -
tar -xJf "${work}/${tarball}" -C "${work}/zig" --strip-components=1

git init -q "${work}/ghostty"
git -C "${work}/ghostty" remote add origin https://github.com/ghostty-org/ghostty
git -C "${work}/ghostty" fetch -q --depth 1 origin "${GHOSTTY_REF}"
git -C "${work}/ghostty" checkout -q FETCH_HEAD
test "$(git -C "${work}/ghostty" rev-parse HEAD)" = "${GHOSTTY_REF}"

# ReleaseSafe keeps the library's own safety checks on: a violated
# invariant aborts the test instead of rendering on from a corrupt state.
(cd "${work}/ghostty" && \
    "${work}/zig/zig" build -Demit-lib-vt -Doptimize=ReleaseSafe \
        --prefix "${prefix}")

test -f "${prefix}/lib/libghostty-vt.a"
test -f "${prefix}/include/ghostty/vt.h"
echo "libghostty-vt ${GHOSTTY_REF} installed in ${prefix}"
