// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2008 ARM Limited
 * Copyright (C) 2014 Regents of the University of California
 */

#include <linux/export.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task_stack.h>
#include <linux/stacktrace.h>
#include <linux/ftrace.h>

#include <asm/stacktrace.h>
#include <asm/unwind.h>

#ifdef CONFIG_FRAME_POINTER

/*
 * This disables KASAN checking when reading a value from another task's stack,
 * since the other task could be running on another CPU and could have poisoned
 * the stack in the meantime.
 */
#define READ_ONCE_TASK_STACK(task, x)			\
({							\
	unsigned long val;				\
	unsigned long addr = x;				\
	if ((task) == current)				\
		val = READ_ONCE(addr);			\
	else						\
		val = READ_ONCE_NOCHECK(addr);		\
	val;						\
})

extern asmlinkage void handle_exception(void);
extern unsigned long ret_from_exception_end;

static inline int fp_is_valid(unsigned long fp, unsigned long sp)
{
	unsigned long low, high;

	low = sp + sizeof(struct stackframe);
	high = ALIGN(sp, THREAD_SIZE);

	return !(fp < low || fp > high || fp & 0x07);
}

void notrace walk_stackframe(struct task_struct *task, struct pt_regs *regs,
			     bool (*fn)(void *, unsigned long), void *arg)
{
	unsigned long fp, sp, pc;
	int graph_idx = 0;
	int level = 0;

	if (regs) {
		fp = frame_pointer(regs);
		sp = user_stack_pointer(regs);
		pc = instruction_pointer(regs);
	} else if (task == NULL || task == current) {
		fp = (unsigned long)__builtin_frame_address(0);
		sp = current_stack_pointer;
		pc = (unsigned long)walk_stackframe;
		level = -1;
	} else {
		/* task blocked in __switch_to */
		fp = task->thread.s[0];
		sp = task->thread.sp;
		pc = task->thread.ra;
	}

	for (;;) {
		struct stackframe *frame;

		if (unlikely(!__kernel_text_address(pc) || (level++ >= 0 && !fn(arg, pc))))
			break;

		if (unlikely(!fp_is_valid(fp, sp)))
			break;

		/* Unwind stack frame */
		frame = (struct stackframe *)fp - 1;
		sp = fp;
		if (regs && (regs->epc == pc) && fp_is_valid(frame->ra, sp)) {
			/* We hit function where ra is not saved on the stack */
			fp = frame->ra;
			pc = regs->ra;
		} else {
			fp = READ_ONCE_TASK_STACK(task, frame->fp);
			pc = READ_ONCE_TASK_STACK(task, frame->ra);
			pc = ftrace_graph_ret_addr(current, &graph_idx, pc,
						   &frame->ra);
			if (pc >= (unsigned long)handle_exception &&
			    pc < (unsigned long)&ret_from_exception_end) {
				if (unlikely(!fn(arg, pc)))
					break;

				pc = ((struct pt_regs *)sp)->epc;
				fp = ((struct pt_regs *)sp)->s0;
			}
		}

	}
}

#else /* !CONFIG_FRAME_POINTER */

void notrace walk_stackframe(struct task_struct *task,
	struct pt_regs *regs, bool (*fn)(void *, unsigned long), void *arg)
{
	unsigned long sp, pc;
	unsigned long *ksp;

	if (regs) {
		sp = user_stack_pointer(regs);
		pc = instruction_pointer(regs);
	} else if (task == NULL || task == current) {
		sp = current_stack_pointer;
		pc = (unsigned long)walk_stackframe;
	} else {
		/* task blocked in __switch_to */
		sp = task->thread.sp;
		pc = task->thread.ra;
	}

	if (unlikely(sp & 0x7))
		return;

	ksp = (unsigned long *)sp;
	while (!kstack_end(ksp)) {
		if (__kernel_text_address(pc) && unlikely(!fn(arg, pc)))
			break;
		pc = READ_ONCE_NOCHECK(*ksp++) - 0x4;
	}
}

#endif /* CONFIG_FRAME_POINTER */

static bool print_trace_address(void *arg, unsigned long pc)
{
	const char *loglvl = arg;

	print_ip_sym(loglvl, pc);
	return true;
}

noinline void dump_backtrace(struct pt_regs *regs, struct task_struct *task,
		    const char *loglvl)
{
	walk_stackframe(task, regs, print_trace_address, (void *)loglvl);
}

void show_stack(struct task_struct *task, unsigned long *sp, const char *loglvl)
{
	pr_cont("%sCall Trace:\n", loglvl);
	dump_backtrace(NULL, task, loglvl);
}

static bool save_wchan(void *arg, unsigned long pc)
{
	if (!in_sched_functions(pc)) {
		unsigned long *p = arg;
		*p = pc;
		return false;
	}
	return true;
}

unsigned long __get_wchan(struct task_struct *task)
{
	unsigned long pc = 0;

	if (!try_get_task_stack(task))
		return 0;
	walk_stackframe(task, NULL, save_wchan, &pc);
	put_task_stack(task);
	return pc;
}

noinline noinstr void arch_stack_walk(stack_trace_consume_fn consume_entry, void *cookie,
		     struct task_struct *task, struct pt_regs *regs)
{
	walk_stackframe(task, regs, consume_entry, cookie);
}

/*
 * Get the return address for a single stackframe and return a pointer to the
 * next frame tail.
 */
static unsigned long unwind_user_frame(stack_trace_consume_fn consume_entry,
				       void *cookie, unsigned long fp,
				       unsigned long reg_ra)
{
	struct stackframe buftail;
	unsigned long ra = 0;
	unsigned long __user *user_frame_tail =
		(unsigned long __user *)(fp - sizeof(struct stackframe));

	/* Check accessibility of one struct frame_tail beyond */
	if (!access_ok(user_frame_tail, sizeof(buftail)))
		return 0;
	if (__copy_from_user_inatomic(&buftail, user_frame_tail,
				      sizeof(buftail)))
		return 0;

	ra = reg_ra ? : buftail.ra;

	fp = buftail.fp;
	if (!ra || !consume_entry(cookie, ra))
		return 0;

	return fp;
}

void arch_stack_walk_user(stack_trace_consume_fn consume_entry, void *cookie,
			  const struct pt_regs *regs)
{
	unsigned long fp = 0;

	fp = regs->s0;
	if (!consume_entry(cookie, regs->epc))
		return;

	fp = unwind_user_frame(consume_entry, cookie, fp, regs->ra);
	while (fp && !(fp & 0x7))
		fp = unwind_user_frame(consume_entry, cookie, fp, 0);
}

/*
 * Stack info helpers for the ORC unwinder.
 * These functions identify what kind of stack a given SP belongs to.
 */
bool in_task_stack(unsigned long stack, struct task_struct *task,
		   struct stack_info *info)
{
	unsigned long begin = (unsigned long)task_stack_page(task);
	unsigned long end   = begin + THREAD_SIZE;

	if (stack < begin || stack >= end)
		return false;

	info->type	= STACK_TYPE_TASK;
	info->begin	= begin;
	info->end	= end;
	info->next_sp	= 0;

	return true;
}

bool in_irq_stack(unsigned long stack, struct stack_info *info)
{
#ifdef CONFIG_VMAP_STACK
	/*
	 * RISC-V doesn't have a separate per-CPU IRQ stack in the
	 * traditional sense; overflow_stack is for stack overflow handling.
	 * Extend this when a separate IRQ stack is implemented.
	 */
#endif
	return false;
}

int get_stack_info(unsigned long stack, struct task_struct *task,
		   struct stack_info *info)
{
	if (!task)
		task = current;

	if (in_task_stack(stack, task, info))
		return 0;

	if (in_irq_stack(stack, info))
		return 0;

	info->type = STACK_TYPE_UNKNOWN;
	return -EINVAL;
}

#ifdef CONFIG_UNWINDER_ORC
/*
 * arch_stack_walk_reliable - reliable stack trace for livepatch.
 *
 * This function walks the call stack using the ORC unwinder.  It is
 * "reliable" in the sense that it returns -EINVAL whenever it encounters
 * anything unexpected (missing ORC data, corrupt frame, etc.) rather than
 * silently producing incorrect output.
 *
 * This is required by the livepatch consistency model to determine whether
 * a task is safe to patch (i.e., not currently executing a function that is
 * being patched).
 */
int arch_stack_walk_reliable(stack_trace_consume_fn consume_entry,
			     void *cookie, struct task_struct *task)
{
	unsigned long addr;
	struct pt_regs dummyregs;
	struct pt_regs *regs = &dummyregs;
	struct unwind_state state;

	if (task == current) {
		regs->sp  = (unsigned long)__builtin_frame_address(0);
		regs->epc = (unsigned long)__builtin_return_address(0);
		regs->s0  = 0;
	} else {
		regs->sp  = task->thread.sp;
		regs->epc = task->thread.ra;
		regs->s0  = task->thread.s[0];
	}
	regs->ra = 0;

	for (unwind_start(&state, task, regs);
	     !unwind_done(&state) && !unwind_error(&state);
	     unwind_next_frame(&state)) {
		addr = unwind_get_return_address(&state);

		/*
		 * A NULL or invalid return address probably means there's some
		 * generated code which __kernel_text_address() doesn't know about.
		 */
		if (!addr)
			return -EINVAL;

		if (!consume_entry(cookie, addr))
			return -EINVAL;
	}

	/* Check for stack corruption */
	if (unwind_error(&state))
		return -EINVAL;

	return 0;
}
#endif /* CONFIG_UNWINDER_ORC */
