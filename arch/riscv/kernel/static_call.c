// SPDX-License-Identifier: GPL-2.0
/*
 * RISC-V static_call() support
 *
 * Static calls use code patching to reduce the overhead of indirect function
 * calls.  On RISC-V each static_call trampoline contains an embedded 8-byte
 * data slot at a fixed offset (RISCV_SCT_DATA) from the trampoline start.
 * The trampoline uses auipc+ld+jr to load and jump through that slot.
 *
 * arch_static_call_transform() updates the data slot to point at the new
 * function.  Because .static_call.text is mapped read-only under
 * CONFIG_STRICT_KERNEL_RWX, the write must go through the fixmap alias
 * via patch_insn_write() rather than a direct store.
 */

#include <linux/static_call.h>
#include <linux/memory.h>
#include <asm/static_call.h>
#include <asm/text-patching.h>

void arch_static_call_transform(void *site, void *tramp, void *func, bool tail)
{
	void **data;

	if (!func)
		func = __static_call_return0;

	/*
	 * We only need to update the trampoline's data slot.
	 * Inline call-site patching (HAVE_STATIC_CALL_INLINE) is not
	 * supported yet, so @site is always NULL.
	 */
	if (!tramp)
		return;

	/*
	 * The data slot is at a fixed offset from the trampoline start.
	 * See the layout description in asm/static_call.h.
	 */
	data = (void **)((u8 *)tramp + RISCV_SCT_DATA);

	/*
	 * The data slot lives in .static_call.text which is mapped read-only
	 * (CONFIG_STRICT_KERNEL_RWX).  Use patch_insn_write() so the write
	 * goes through the fixmap alias, avoiding a write-to-RO page fault.
	 */
	mutex_lock(&text_mutex);
	patch_insn_write(data, &func, sizeof(func));
	mutex_unlock(&text_mutex);
}
EXPORT_SYMBOL_GPL(arch_static_call_transform);
