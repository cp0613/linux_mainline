/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ASM_RISCV_UNWIND_HINTS_H
#define _ASM_RISCV_UNWIND_HINTS_H

#include <linux/objtool.h>
#include "orc_types.h"

#ifdef __ASSEMBLY__

/*
 * UNWIND_HINT_END_OF_STACK - annotate the first instruction of a function that
 * is never called directly (e.g. kernel entry points defined with
 * SYM_CODE_START).  This tells objtool that it should start CFI validation from
 * this point, treating the stack as empty/unknown.
 */
.macro UNWIND_HINT_END_OF_STACK
	UNWIND_HINT type=UNWIND_HINT_TYPE_END_OF_STACK
.endm

/*
 * UNWIND_HINT_FUNC - annotate a callable non-C function (SYM_CODE_START) that
 * behaves like a normal function call from a stack-unwinding perspective.
 */
.macro UNWIND_HINT_FUNC
	UNWIND_HINT type=UNWIND_HINT_TYPE_FUNC
.endm

#endif /* __ASSEMBLY__ */

#endif /* _ASM_RISCV_UNWIND_HINTS_H */
