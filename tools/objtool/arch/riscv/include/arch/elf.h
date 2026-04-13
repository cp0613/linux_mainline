/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _OBJTOOL_ARCH_ELF_H
#define _OBJTOOL_ARCH_ELF_H

/*
 * RISC-V ELF relocation types.
 * See: https://github.com/riscv-non-isa/riscv-elf-psabi-doc
 */
#ifndef R_RISCV_NONE
#define R_RISCV_NONE		0
#endif
#ifndef R_RISCV_32
#define R_RISCV_32		1
#endif
#ifndef R_RISCV_64
#define R_RISCV_64		2
#endif
#ifndef R_RISCV_CALL_PLT
#define R_RISCV_CALL_PLT	19
#endif
#ifndef R_RISCV_PCREL_HI20
#define R_RISCV_PCREL_HI20	23
#endif
#ifndef R_RISCV_ADD16
#define R_RISCV_ADD16		34
#endif
#ifndef R_RISCV_SUB16
#define R_RISCV_SUB16		38
#endif
#ifndef R_RISCV_ADD32
#define R_RISCV_ADD32		35
#endif
#ifndef R_RISCV_SUB32
#define R_RISCV_SUB32		39
#endif
#ifndef R_RISCV_32_PCREL
#define R_RISCV_32_PCREL	57
#endif

#ifndef EM_RISCV
#define EM_RISCV		243
#endif

#define R_NONE			R_RISCV_NONE
#define R_ABS32			R_RISCV_32
#define R_ABS64			R_RISCV_64
#define R_DATA32		R_RISCV_32_PCREL
#define R_DATA64		R_RISCV_32_PCREL
#define R_TEXT32		R_RISCV_32_PCREL
#define R_TEXT64		R_RISCV_32_PCREL

#endif /* _OBJTOOL_ARCH_ELF_H */
