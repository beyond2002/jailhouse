/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2013
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#define ARRAY_SIZE(array)	sizeof(array) / sizeof((array)[0])

#define BYTE_MASK(size)		(0xFFFFFFFFFFFFFFFFULL >> ((8 - size) * 8))