/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_STATIC_CALL_H
#define _ASM_RISCV_STATIC_CALL_H

/*
 * RISC-V static_call trampoline layout (RV64):
 *
 * Each trampoline is placed in .static_call.text and contains an embedded
 * 8-byte data slot holding the target function pointer.  The trampoline
 * loads that pointer via a PC-relative auipc+ld pair and jumps to it:
 *
 *   offset  0: auipc  t0, %pcrel_hi(1f)     ; t0 = PC + upper(data_slot)
 *   offset  4: ld     t0, %pcrel_lo(0b)(t0) ; t0 = *data_slot
 *   offset  8: jr     t0                    ; jump to target
 *   offset 12: (4 bytes padding for 8-byte alignment)
 *   offset 16: .quad  <target>              ; 8-byte data slot  <-- 1:
 *
 * %pcrel_lo must reference the label of the corresponding %pcrel_hi
 * instruction (label 0:, i.e., the auipc), not an expression like .-4.
 *
 * RISCV_SCT_DATA (16) is the byte offset from trampoline start to the
 * data slot.  arch_static_call_transform() uses this constant to locate
 * and update the slot without parsing any instructions.
 *
 * Note: HAVE_STATIC_CALL is only selected when CFI is enabled (matching
 * arm64 practice), since the indirect-jump trampoline requires CFI metadata.
 */

#define RISCV_SCT_DATA	16

#define __ARCH_DEFINE_STATIC_CALL_TRAMP(name, target)			\
	asm(".pushsection .static_call.text, \"ax\"\n"			\
	    ".align 2\n"						\
	    ".globl " name "\n"						\
	    name ":\n"							\
	    "0:\tauipc\tt0, %pcrel_hi(1f)\n"				\
	    "\tld\tt0, %pcrel_lo(0b)(t0)\n"				\
	    "\tjr\tt0\n"						\
	    ".align 3\n"						\
	    "1:\t.quad\t" target "\n"					\
	    ".type " name ", @function\n"				\
	    ".size " name ", 1b + 8 - " name "\n"			\
	    ".popsection\n")

#define ARCH_DEFINE_STATIC_CALL_TRAMP(name, func)			\
	__ARCH_DEFINE_STATIC_CALL_TRAMP(STATIC_CALL_TRAMP_STR(name), #func)

#define ARCH_DEFINE_STATIC_CALL_NULL_TRAMP(name)			\
	ARCH_DEFINE_STATIC_CALL_TRAMP(name, __static_call_return0)

#define ARCH_DEFINE_STATIC_CALL_RET0_TRAMP(name)			\
	ARCH_DEFINE_STATIC_CALL_TRAMP(name, __static_call_return0)

#endif /* _ASM_RISCV_STATIC_CALL_H */
