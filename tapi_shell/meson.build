# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# External TE library (see TE_EXT_REPO): engine-side TAPI for
# terminal-emulator sessions on agents. Built as a shared library on the
# engine platform. It needs the shell_* RPCs in rpcxdr, so add
# ../ta_shell/shell_rpc.x.m4 to the rpcxdr definitions of the engine
# platform.

if get_option('engine')
    build_lib_shared = true
endif
build_lib_static = not get_option('engine')
install_lib = install_dev

headers += files(
    'tapi_shell.h',
    'tapi_shell_rpc.h',
)

sources += files(
    'tapi_shell.c',
    'tapi_shell_rpc.c',
)

te_libs += [
    'tapi',
    'tapi_rpc',
    'rcfrpc',
    'confapi',
    'rcfapi',
    'rpcxdr',
    'logger_core',
    'tools',
]
