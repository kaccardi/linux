// SPDX-License-Identifier: GPL-2.0
/*
 * utils.c
 *
 * This contains various libraries that are needed for fgkaslr
 */
#define __DISABLE_EXPORTS
#define _LINUX_KPROBES_H
#define NOKPROBE_SYMBOL(fname)
#include "../../../../lib/sort.c"
#include "../../../../lib/bsearch.c"
