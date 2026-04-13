/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _OBJTOOL_ARCH_CFI_REGS_H
#define _OBJTOOL_ARCH_CFI_REGS_H

/*
 * RISC-V CFI register numbers.
 *
 * These correspond to RISC-V integer register numbers (ABI):
 *   zero=x0, ra=x1, sp=x2, gp=x3, tp=x4, t0=x5 ... t2=x7,
 *   s0/fp=x8, s1=x9, a0=x10 ... a7=x17,
 *   s2=x18 ... s11=x27, t3=x28 ... t6=x31
 *
 * Callee-saved registers that matter for ORC unwinding:
 *   ra (x1), fp/s0 (x8), s1 (x9), s2-s11 (x18-x27)
 */
#define CFI_RA		1	/* x1 - return address */
#define CFI_SP		2	/* x2 - stack pointer */
#define CFI_FP		8	/* x8/s0 - frame pointer */
#define CFI_S1		9	/* x9 */
#define CFI_S2		18	/* x18 */
#define CFI_S3		19
#define CFI_S4		20
#define CFI_S5		21
#define CFI_S6		22
#define CFI_S7		23
#define CFI_S8		24
#define CFI_S9		25
#define CFI_S10		26
#define CFI_S11		27
#define CFI_NUM_REGS	32

#define CFI_BP		CFI_FP

#endif /* _OBJTOOL_ARCH_CFI_REGS_H */
