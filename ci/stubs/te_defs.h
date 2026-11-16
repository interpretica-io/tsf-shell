/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief CI stand-in for the Test Environment's te_defs.h
 *
 * The backend units (ta_shell_ghostty.c, ta_shell_vterm.c) use nothing
 * of TE but a growable string and a checked allocation. These stubs give
 * exactly that, so ci/backend_test.c can build and run the backends
 * against the real emulator libraries without a TE build tree.
 */
#ifndef CI_STUB_TE_DEFS_H
#define CI_STUB_TE_DEFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TE_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif /* !CI_STUB_TE_DEFS_H */
