/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ASM_RISCV_ORC_HEADER_H
#define _ASM_RISCV_ORC_HEADER_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/orc_hash.h>

/*
 * The header is currently a 20-byte hash of the ORC entry definition; see
 * scripts/orc_hash.sh.
 */
#define ORC_HEADER					\
	__used __section(".orc_header") __aligned(4)	\
	static const u8 orc_header[] = { ORC_HASH }

#endif /* _ASM_RISCV_ORC_HEADER_H */
