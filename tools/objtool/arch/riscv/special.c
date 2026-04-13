// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RISC-V special section handling for objtool.
 *
 * Handles alternative patching tables, exception tables, and
 * jump label tables for RISC-V.
 */
#include <string.h>
#include <objtool/check.h>
#include <objtool/elf.h>
#include <objtool/special.h>
#include <objtool/warn.h>
#include <arch/elf.h>
#include <arch/special.h>

/*
 * arch_handle_alternative - fix up orig_len and new_len for RISC-V.
 *
 * RISC-V's .alternative section stores alt_len (at ALT_NEW_LEN_OFFSET = 0x0a)
 * as a u16 computed by a pair of relocations:
 *   R_RISCV_ADD16  .L_end_of_new_insn  (adds target label offset)
 *   R_RISCV_SUB16  .L_start_of_new_insn (subtracts base label offset)
 * so the raw bytes in .o are 0.  We must scan the relocation table to
 * compute the actual length.
 *
 * orig_len == new_len for RISC-V because the assembler pads both sides with
 * noops to the same size using .org directives.
 */
void arch_handle_alternative(struct elf *elf, struct section *sec,
			     unsigned long entry_off,
			     struct special_alt *alt)
{
	unsigned long len_off = entry_off + ALT_NEW_LEN_OFFSET;
	struct reloc *add_reloc = NULL, *sub_reloc = NULL;
	struct reloc *r;
	unsigned long alt_len;

	if (!sec->rsec)
		return;

	/* Find the ADD16 and SUB16 relocations at the alt_len field offset. */
	for_each_reloc(sec->rsec, r) {
		if (reloc_offset(r) > len_off)
			break;
		if (reloc_offset(r) != len_off)
			continue;
		if (reloc_type(r) == R_RISCV_ADD16)
			add_reloc = r;
		else if (reloc_type(r) == R_RISCV_SUB16)
			sub_reloc = r;
	}

	if (!add_reloc || !sub_reloc)
		return;

	/*
	 * alt_len = (add_sym->offset + add_addend) - (sub_sym->offset + sub_addend)
	 * Both labels are in the same section (.text), so this is simply the
	 * distance between the end and the start of the new-instruction block.
	 */
	alt_len = (add_reloc->sym->offset + reloc_addend(add_reloc)) -
		  (sub_reloc->sym->offset + reloc_addend(sub_reloc));

	alt->orig_len = alt_len;
	alt->new_len  = alt_len;
}

bool arch_support_alt_relocation(struct special_alt *special_alt,
				 struct instruction *insn,
				 struct reloc *reloc)
{
	return false;
}

struct reloc *arch_find_switch_table(struct objtool_file *file,
				     struct instruction *insn,
				     unsigned long *table_size)
{
	struct reloc *text_reloc, *rodata_reloc;
	struct section *table_sec;
	unsigned long table_offset;

	/*
	 * Look for a PCREL_HI20 relocation on this instruction, which is what
	 * GCC emits for the auipc that loads the base address of a PC-relative
	 * switch jump table stored in .rodata.  The table entries use an
	 * R_RISCV_ADD32 + R_RISCV_SUB32 pair at each slot to store a
	 * PC-relative (table-base-relative) 32-bit offset to the target label.
	 */
	text_reloc = find_reloc_by_dest_range(file->elf, insn->sec,
					      insn->offset, insn->len);
	if (!text_reloc || reloc_type(text_reloc) != R_RISCV_PCREL_HI20 ||
	    !text_reloc->sym->sec->rodata)
		goto func_scan;

	table_offset = text_reloc->sym->offset + reloc_addend(text_reloc);
	table_sec = text_reloc->sym->sec;

	/*
	 * Make sure the .rodata address isn't associated with a named symbol
	 * (GCC switch tables are anonymous data).  Also allow the special
	 * C_JUMP_TABLE_SECTION which uses the same format.
	 */
	if (find_symbol_containing(table_sec, table_offset) &&
	    strcmp(table_sec->name, C_JUMP_TABLE_SECTION))
		goto func_scan;

	/*
	 * Scan .rela.rodata for the first R_RISCV_ADD32 at table_offset.
	 * RISC-V PC-relative switch tables use ADD32+SUB32 pairs; the ADD32
	 * carries the target symbol.  We iterate rather than relying on
	 * find_reloc_by_dest() which may return either the ADD32 or the SUB32
	 * depending on hash ordering.
	 */
	rodata_reloc = NULL;
	if (table_sec->rsec) {
		struct reloc *r;

		for_each_reloc(table_sec->rsec, r) {
			if (reloc_offset(r) > table_offset)
				break;
			if (reloc_offset(r) == table_offset &&
			    reloc_type(r) == R_RISCV_ADD32) {
				rodata_reloc = r;
				break;
			}
		}
	}
	if (!rodata_reloc)
		goto func_scan;

	*table_size = 0;
	return rodata_reloc;

func_scan:
	/*
	 * GCC may load the switch table base address into a callee-saved
	 * register via auipc early in the function, far from the jr
	 * dispatch instruction.  When the backward search in
	 * find_jump_table() calls us with instructions other than the
	 * original jr, or when the auipc is not directly reachable via
	 * first_jump_src links, scan the function's .rela.text section
	 * for any R_RISCV_PCREL_HI20 relocation that targets anonymous
	 * .rodata (a switch table) and whose entries point back to code
	 * in the same function.
	 */
	{
		struct symbol *func = insn_func(insn);
		struct reloc *r;
		struct section *rsec = insn->sec->rsec;

		if (!func || !rsec)
			goto c_jump_table;

		for_each_reloc(rsec, r) {
			if (reloc_type(r) != R_RISCV_PCREL_HI20)
				continue;

			/* Must be within the same function */
			if (reloc_offset(r) < func->offset ||
			    reloc_offset(r) >= func->offset + func->len)
				continue;

			if (!r->sym->sec->rodata)
				continue;

			table_offset = r->sym->offset + reloc_addend(r);
			table_sec = r->sym->sec;

			/* Skip named symbols (not anonymous switch tables) */
			if (find_symbol_containing(table_sec, table_offset) &&
			    strcmp(table_sec->name, C_JUMP_TABLE_SECTION))
				continue;

			/* Find the first R_RISCV_ADD32 at table_offset */
			rodata_reloc = NULL;
			if (table_sec->rsec) {
				struct reloc *rr;

				for_each_reloc(table_sec->rsec, rr) {
					if (reloc_offset(rr) > table_offset)
						break;
					if (reloc_offset(rr) == table_offset &&
					    reloc_type(rr) == R_RISCV_ADD32) {
						rodata_reloc = rr;
						break;
					}
				}
			}
			if (!rodata_reloc)
				continue;

			/*
			 * Verify: the first table entry should point to code
			 * in the same function.
			 */
			{
				unsigned long target_off =
					rodata_reloc->sym->offset + reloc_addend(rodata_reloc);
				struct instruction *dest =
					find_insn(file, rodata_reloc->sym->sec, target_off);
				if (!dest || !insn_func(dest) ||
				    insn_func(dest)->pfunc != func->pfunc)
					continue;
			}

			*table_size = 0;
			return rodata_reloc;
		}
	}

c_jump_table:
	/*
	 * Fall back to C jump table detection for .data.rel.ro.c_jump_table.
	 */
	{
		struct section *rsec = insn->sec->rsec;
		struct reloc *reloc;

		if (!rsec)
			return NULL;

		for_each_reloc(rsec, reloc) {
			if (reloc_offset(reloc) > insn->offset)
				break;

			if (!strcmp(reloc->sym->sec->name, C_JUMP_TABLE_SECTION)) {
				*table_size = 0;

				table_sec = reloc->sym->sec;
				table_offset = reloc->sym->offset + reloc_addend(reloc);

				rodata_reloc = find_reloc_by_dest(file->elf, table_sec,
								  table_offset);
				if (!rodata_reloc)
					return NULL;

				return rodata_reloc;
			}
		}
	}

	return NULL;
}

const char *arch_cpu_feature_name(int feature_number)
{
	return NULL;
}
