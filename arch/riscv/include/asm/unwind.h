/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RISC-V ORC-based stack unwinder interface.
 *
 * Based on the LoongArch implementation by Loongson Technology Corporation.
 * Adapted for RISC-V by the RISC-V kernel developers.
 */
#ifndef _ASM_RISCV_UNWIND_H
#define _ASM_RISCV_UNWIND_H

#include <linux/sched.h>
#include <linux/ftrace.h>

#include <asm/ptrace.h>
#include <asm/stacktrace.h>

enum unwinder_type {
	UNWINDER_GUESS,
	UNWINDER_ORC,
};

struct unwind_state {
	char type; /* UNWINDER_XXX */
	struct stack_info stack_info;
	struct task_struct *task;
	bool first, error, reset;
	int graph_idx;
	unsigned long sp, fp, pc, ra;
};

bool default_next_frame(struct unwind_state *state);

void unwind_start(struct unwind_state *state,
		  struct task_struct *task, struct pt_regs *regs);
bool unwind_next_frame(struct unwind_state *state);
unsigned long unwind_get_return_address(struct unwind_state *state);

static inline bool unwind_done(struct unwind_state *state)
{
	return state->stack_info.type == STACK_TYPE_UNKNOWN;
}

static inline bool unwind_error(struct unwind_state *state)
{
	return state->error;
}

/*
 * GRAPH_FAKE_OFFSET: offset within pt_regs where 'ra' is stored.
 * Used for ftrace graph return address unwinding.
 * RISC-V pt_regs layout: ra is the second field (after epc which is 8 bytes).
 */
#define GRAPH_FAKE_OFFSET (sizeof(struct pt_regs) - offsetof(struct pt_regs, ra))

static inline unsigned long unwind_graph_addr(struct unwind_state *state,
					      unsigned long pc,
					      unsigned long cfa)
{
	return ftrace_graph_ret_addr(state->task, &state->graph_idx,
				     pc, (unsigned long *)(cfa - GRAPH_FAKE_OFFSET));
}

/*
 * __unwind_start - initialize the unwind state.
 *
 * For RISC-V:
 *   regs->epc  = PC
 *   regs->ra   = return address (x1)
 *   regs->sp   = stack pointer (x2)
 *   regs->s0   = frame pointer (x8)
 *
 * For blocked tasks (task != current, no regs):
 *   task->thread.ra  = saved return address
 *   task->thread.sp  = saved stack pointer
 *   task->thread.s[0] = saved s0 (frame pointer)
 */
static __always_inline void __unwind_start(struct unwind_state *state,
					   struct task_struct *task,
					   struct pt_regs *regs)
{
	memset(state, 0, sizeof(*state));
	if (regs) {
		state->sp = regs->sp;
		state->pc = regs->epc;
		state->ra = regs->ra;
		state->fp = regs->s0;
	} else if (task && task != current) {
		/* blocked task: use saved thread state */
		state->sp = task->thread.sp;
		state->pc = task->thread.ra;
		state->ra = 0;
		state->fp = 0;
	} else {
		state->sp = (unsigned long)__builtin_frame_address(0);
		state->pc = (unsigned long)__builtin_return_address(0);
		state->ra = 0;
		state->fp = 0;
	}
	state->task = task;
	get_stack_info(state->sp, state->task, &state->stack_info);
	state->pc = unwind_graph_addr(state, state->pc, state->sp);
}

static __always_inline unsigned long
__unwind_get_return_address(struct unwind_state *state)
{
	if (unwind_done(state))
		return 0;

	return __kernel_text_address(state->pc) ? state->pc : 0;
}

#ifdef CONFIG_UNWINDER_ORC
void unwind_init(void);
void unwind_module_init(struct module *mod, void *orc_ip, size_t orc_ip_size,
			void *orc, size_t orc_size);
#else
static inline void unwind_init(void) {}
static inline void unwind_module_init(struct module *mod, void *orc_ip,
				      size_t orc_ip_size, void *orc,
				      size_t orc_size) {}
#endif

#endif /* _ASM_RISCV_UNWIND_H */
