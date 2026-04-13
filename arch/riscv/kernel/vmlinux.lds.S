/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 */

#define RO_EXCEPTION_TABLE_ALIGN	4
#define RUNTIME_DISCARD_EXIT

#ifdef CONFIG_XIP_KERNEL
#include "vmlinux-xip.lds.S"
#else

#include <asm/pgtable.h>
#define LOAD_OFFSET KERNEL_LINK_ADDR

#include <asm/vmlinux.lds.h>
#include <asm/page.h>
#include <asm/cache.h>
#include <asm/thread_info.h>
#include <asm/set_memory.h>
#ifdef CONFIG_UNWINDER_ORC
#include <asm/orc_lookup.h>
#endif
#include "image-vars.h"

#include <linux/sizes.h>
OUTPUT_ARCH(riscv)
ENTRY(_start)

jiffies = jiffies_64;

PHDRS {
	text PT_LOAD FLAGS(5);		/* R_E */
	data PT_LOAD FLAGS(6);		/* RW_ */
}

SECTIONS
{
	/* Beginning of code and text segment */
	. = LOAD_OFFSET;
	_start = .;
	HEAD_TEXT_SECTION
	. = ALIGN(PAGE_SIZE);

	.text : {
		_text = .;
		_stext = .;
		TEXT_TEXT
		SCHED_TEXT
		LOCK_TEXT
		KPROBES_TEXT
		ENTRY_TEXT
		IRQENTRY_TEXT
		SOFTIRQENTRY_TEXT
		_etext = .;
	} :text

	.orc_header : AT(ADDR(.orc_header) - LOAD_OFFSET) {
		BOUNDED_SECTION_BY(.orc_header, _orc_header)
	} . = ALIGN(4);
	.orc_unwind_ip : AT(ADDR(.orc_unwind_ip) - LOAD_OFFSET) {
		BOUNDED_SECTION_BY(.orc_unwind_ip, _orc_unwind_ip)
	} . = ALIGN(2);
	.orc_unwind : AT(ADDR(.orc_unwind) - LOAD_OFFSET) {
		BOUNDED_SECTION_BY(.orc_unwind, _orc_unwind)
	} . = ALIGN(4);
	text_size = _etext - _stext;

	. = ALIGN(SECTION_ALIGN);
	__init_begin = .;
	__init_text_begin = .;
	.init.text : AT(ADDR(.init.text) - LOAD_OFFSET) ALIGN(SECTION_ALIGN) { \
		_sinittext = .;						\
		INIT_TEXT						\
		_einittext = .;						\
	}

	. = ALIGN(8);
	__soc_early_init_table : {
		__soc_early_init_table_start = .;
		KEEP(*(__soc_early_init_table))
		__soc_early_init_table_end = .;
	}
	__soc_builtin_dtb_table : {
		__soc_builtin_dtb_table_start = .;
		KEEP(*(__soc_builtin_dtb_table))
		__soc_builtin_dtb_table_end = .;
	}
	/* we have to discard exit text and such at runtime, not link time */
	__exittext_begin = .;
	.exit.text :
	{
		EXIT_TEXT
	}
	__exittext_end = .;

	__init_text_end = .;
	. = ALIGN(SECTION_ALIGN);
#ifdef CONFIG_EFI
	. = ALIGN(PECOFF_SECTION_ALIGNMENT);
	__pecoff_text_end = .;
#endif
	/* Start of init data section */
	__init_data_begin = .;
	INIT_DATA_SECTION(16)

	.init.pi : {
		KEEP(*(.init.pi*))
	}

	.init.bss : {
		KEEP(*(.init.bss*))	/* from the EFI stub */
	}
	.exit.data :
	{
		EXIT_DATA
	}

	RUNTIME_CONST_VARIABLES

	PERCPU_SECTION(L1_CACHE_BYTES)

	.rel.dyn : {
		*(.rel.dyn*)
	}

	.rela.dyn : ALIGN(8) {
		__rela_dyn_start = .;
		*(.rela .rela*)
		__rela_dyn_end = .;
	}

	__init_data_end = .;

	. = ALIGN(8);
	.alternative : {
		__alt_start = .;
		KEEP(*(.alternative))
		__alt_end = .;
	}
	__init_end = .;

	/* Start of data section */
	_sdata = .;
	RO_DATA(SECTION_ALIGN)
	.srodata : {
		*(.srodata*)
	}

	. = ALIGN(SECTION_ALIGN);
	_data = .;

	. = ALIGN(PAGE_SIZE);
	.data : AT(ADDR(.data) - LOAD_OFFSET) {
		INIT_TASK_DATA(THREAD_ALIGN)
		NOSAVE_DATA
		PAGE_ALIGNED_DATA(PAGE_SIZE)
		CACHE_HOT_DATA(L1_CACHE_BYTES)
		CACHELINE_ALIGNED_DATA(L1_CACHE_BYTES)
		READ_MOSTLY_DATA(L1_CACHE_BYTES)
		DATA_DATA
		CONSTRUCTORS
	} :data
	BUG_TABLE
	.sdata : {
		__global_pointer$ = . + 0x800;
		*(.sdata*)
	}

	.got : { *(.got*) }

#ifdef CONFIG_RELOCATABLE
	.data.rel : { *(.data.rel*) }
	.plt : { *(.plt) }
	.dynamic : { *(.dynamic) }
	.dynsym : { *(.dynsym) }
	.dynstr : { *(.dynstr) }
	.hash : { *(.hash) }
	.gnu.hash : { *(.gnu.hash) }
#endif

#ifdef CONFIG_EFI
	.pecoff_edata_padding : { BYTE(0); . = ALIGN(PECOFF_FILE_ALIGNMENT); }
	__pecoff_data_raw_size = ABSOLUTE(. - __pecoff_text_end);
	__pecoff_data_raw_end = ABSOLUTE(.);
#endif

	/*
	 * The .orc_lookup section is written to by unwind_init() during
	 * early boot, so it must reside in a writable data segment.
	 * Place it here in the data segment, separate from the read-only
	 * .orc_header/.orc_unwind_ip/.orc_unwind sections which stay in
	 * the text segment.
	 */
	. = ALIGN(4);
	.orc_lookup : AT(ADDR(.orc_lookup) - LOAD_OFFSET) {
		orc_lookup = .;
		. += (((text_size + LOOKUP_BLOCK_SIZE - 1) /
			LOOKUP_BLOCK_SIZE) + 1) * 4;
		orc_lookup_end = .;
	}

	/* End of data section */
	_edata = .;

	BSS_SECTION(PAGE_SIZE, PAGE_SIZE, 0)

#ifdef CONFIG_EFI
	. = ALIGN(PECOFF_SECTION_ALIGNMENT);
	__pecoff_data_virt_size = ABSOLUTE(. - __pecoff_text_end);
	__pecoff_data_virt_end = ABSOLUTE(.);
#endif
	_end = .;

	STABS_DEBUG
	DWARF_DEBUG
	MODINFO
	ELF_DETAILS
	.riscv.attributes 0 : { *(.riscv.attributes) }

	DISCARDS
}
#endif /* CONFIG_XIP_KERNEL */
