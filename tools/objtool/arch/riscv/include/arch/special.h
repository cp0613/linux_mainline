/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _OBJTOOL_ARCH_SPECIAL_H
#define _OBJTOOL_ARCH_SPECIAL_H

/*
 * See more info about struct exception_table_entry
 * in arch/riscv/include/asm/extable.h
 * struct exception_table_entry { int insn, fixup; short type, data; }
 * = 4 + 4 + 2 + 2 = 12 bytes
 */
#define EX_ENTRY_SIZE		12
#define EX_ORIG_OFFSET		0
#define EX_NEW_OFFSET		4

/*
 * See more info about struct jump_entry
 * in include/linux/jump_label.h
 */
#define JUMP_ENTRY_SIZE		16
#define JUMP_ORIG_OFFSET	0
#define JUMP_NEW_OFFSET		4
#define JUMP_KEY_OFFSET		8

/*
 * RISC-V uses ".alternative" (not ".altinstructions") for its patching
 * table.  See arch/riscv/include/asm/alternative.h for the struct layout:
 *   struct alt_entry {
 *     s32 old_offset;  <- PC-relative offset to orig insn (4 bytes)
 *     s32 alt_offset;  <- PC-relative offset to new insn  (4 bytes)
 *     u16 vendor_id;                                      (2 bytes)
 *     u16 alt_len;     <- length of new (and orig) insns  (2 bytes)
 *     u32 patch_id;                                       (4 bytes)
 *   } = 16 bytes total
 *
 * There is no separate orig_len field; orig and new are forced to the
 * same length by the .org directives in ALT_NEW_CONTENT, so we read
 * alt_len (at offset 10) for both.
 */
#define ALT_SECTION_NAME	".alternative"
#define ALT_ENTRY_SIZE		16
#define ALT_ORIG_OFFSET		0
#define ALT_NEW_OFFSET		4
#define ALT_FEATURE_OFFSET	8
#define ALT_ORIG_LEN_OFFSET	10	/* same field as ALT_NEW_LEN_OFFSET */
#define ALT_NEW_LEN_OFFSET	10	/* alt_len (u16 little-endian, low byte) */

#endif /* _OBJTOOL_ARCH_SPECIAL_H */
