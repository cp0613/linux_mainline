// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RISC-V specific objtool support
 *
 * Instruction decoder for RISC-V, used by objtool to analyze stack
 * operations (push/pop of callee-saved registers, SP adjustments, etc.)
 * and generate ORC unwind metadata.
 *
 * Reference: RISC-V ISA Specification
 * https://riscv.org/technical/specifications/
 */
#include <string.h>
#include <objtool/check.h>
#include <objtool/disas.h>
#include <objtool/elf.h>
#include <objtool/warn.h>
#include <asm/orc_types.h>
#include <linux/objtool_types.h>
#include <arch/elf.h>

/* RISC-V instruction size */
#define RISCV_INSN_SIZE		4

/*
 * RISC-V opcode fields.
 * All RV32I/RV64I instructions are 32-bit.
 */
#define RV_INSN_OPCODE(x)	((x) & 0x7f)
#define RV_INSN_RD(x)		(((x) >> 7) & 0x1f)
#define RV_INSN_FUNCT3(x)	(((x) >> 12) & 0x7)
#define RV_INSN_RS1(x)		(((x) >> 15) & 0x1f)
#define RV_INSN_RS2(x)		(((x) >> 20) & 0x1f)
#define RV_INSN_FUNCT7(x)	(((x) >> 25) & 0x7f)

/* I-type immediate (sign-extended 12 bits) */
#define RV_INSN_I_IMM(x)	((s32)(x) >> 20)

/* S-type immediate (sign-extended 12 bits) */
#define RV_INSN_S_IMM(x)	\
	(((s32)((x) & 0xfe000000) >> 20) | (((x) >> 7) & 0x1f))

/* U-type immediate (upper 20 bits, shifted) */
#define RV_INSN_U_IMM(x)	((s32)((x) & 0xfffff000))

/* J-type immediate (for JAL) */
#define RV_INSN_J_IMM(x)	\
	(((s32)((x) & 0x80000000) >> 11) | \
	 ((x) & 0xff000) | \
	 (((x) >> 9) & 0x800) | \
	 (((x) >> 20) & 0x7fe))

/* B-type immediate (for branches) */
#define RV_INSN_B_IMM(x)	\
	(((s32)((x) & 0x80000000) >> 19) | \
	 (((x) & 0x80) << 4) | \
	 (((x) >> 20) & 0x7e0) | \
	 (((x) >> 7) & 0x1e))

/* RISC-V opcodes */
#define OPC_LOAD	0x03	/* LW, LD, etc. */
#define OPC_STORE	0x23	/* SW, SD, etc. */
#define OPC_AUIPC	0x17
#define OPC_LUI		0x37
#define OPC_OP_IMM	0x13	/* ADDI, etc. */
#define OPC_OP_IMM_32	0x1b	/* ADDIW, etc. (RV64) */
#define OPC_OP		0x33	/* ADD, SUB, etc. */
#define OPC_OP_32	0x3b	/* ADDW, etc. (RV64) */
#define OPC_JAL		0x6f
#define OPC_JALR	0x67
#define OPC_BRANCH	0x63	/* BEQ, BNE, etc. */
#define OPC_SYSTEM	0x73	/* ECALL, EBREAK, etc. */
#define OPC_MISC_MEM	0x0f	/* FENCE, etc. */

/* funct3 for LOAD */
#define F3_LD		0x3	/* LD (RV64: load doubleword) */
#define F3_LW		0x2	/* LW */

/* funct3 for STORE */
#define F3_SD		0x3	/* SD (RV64: store doubleword) */
#define F3_SW		0x2	/* SW */

/* funct3 for OP-IMM */
#define F3_ADDI		0x0

/* funct3 for JALR */
#define F3_JALR		0x0

/* funct3 for SYSTEM */
#define F3_PRIV		0x0

/* funct12 for SYSTEM/PRIV */
#define F12_EBREAK	0x001
#define F12_URET	0x002
#define F12_SRET	0x102
#define F12_MRET	0x302

/* Register numbers */
#define REG_ZERO	0
#define REG_RA		1
#define REG_SP		2
#define REG_FP		8	/* s0/fp */

const char *arch_reg_name[CFI_NUM_REGS] = {
	"zero", "ra", "sp", "gp",
	"tp", "t0", "t1", "t2",
	"s0", "s1", "a0", "a1",
	"a2", "a3", "a4", "a5",
	"a6", "a7", "s2", "s3",
	"s4", "s5", "s6", "s7",
	"s8", "s9", "s10", "s11",
	"t3", "t4", "t5", "t6"
};

int arch_ftrace_match(const char *name)
{
	return !strcmp(name, "_mcount");
}

unsigned long arch_jump_destination(struct instruction *insn)
{
	return insn->offset + insn->immediate;
}

s64 arch_insn_adjusted_addend(struct instruction *insn, struct reloc *reloc)
{
	return reloc_addend(reloc);
}

bool arch_pc_relative_reloc(struct reloc *reloc)
{
	switch (reloc_type(reloc)) {
	case R_RISCV_32_PCREL:
		return true;
	default:
		return false;
	}
}

bool arch_callee_saved_reg(unsigned char reg)
{
	switch (reg) {
	case CFI_RA:
	case CFI_FP:
	case CFI_S1:
	case CFI_S2 ... CFI_S11:
		return true;
	default:
		return false;
	}
}

int arch_decode_hint_reg(u8 sp_reg, int *base)
{
	switch (sp_reg) {
	case ORC_REG_UNDEFINED:
		*base = CFI_UNDEFINED;
		break;
	case ORC_REG_SP:
		*base = CFI_SP;
		break;
	case ORC_REG_FP:
		*base = CFI_FP;
		break;
	default:
		return -1;
	}

	return 0;
}

static bool is_riscv(const struct elf *elf)
{
	if (elf->ehdr.e_machine == EM_RISCV)
		return true;

	ERROR("unexpected ELF machine type %d", elf->ehdr.e_machine);
	return false;
}

#define ADD_OP(op) \
	if (!(op = calloc(1, sizeof(*op)))) \
		return -1; \
	else for (*ops_list = op, ops_list = &op->next; op; op = NULL)

int arch_decode_instruction(struct objtool_file *file, const struct section *sec,
			    unsigned long offset, unsigned int maxlen,
			    struct instruction *insn)
{
	struct stack_op **ops_list = &insn->stack_ops;
	const struct elf *elf = file->elf;
	struct stack_op *op = NULL;
	u32 inst;
	unsigned int opcode, rd, rs1, rs2, funct3;
	s32 imm;

	if (!is_riscv(elf))
		return -1;

	if (maxlen < 2)
		return 0;

	/* Peek at the lower 2 bits to detect compressed (16-bit) instructions */
	if ((*(u16 *)(sec->data->d_buf + offset) & 0x3) != 0x3) {
		u16 c_inst = *(u16 *)(sec->data->d_buf + offset);
		unsigned int c_op    = c_inst & 0x3;        /* bits[1:0]  */
		unsigned int c_funct3 = (c_inst >> 13) & 0x7; /* bits[15:13] */
		unsigned int c_rs1   = (c_inst >> 7) & 0x1f;  /* bits[11:7]  */
		unsigned int c_rs2   = (c_inst >> 2) & 0x1f;  /* bits[6:2]   */
		unsigned int c_bit12 = (c_inst >> 12) & 0x1;  /* bit[12]     */

		insn->len = 2;
		insn->type = INSN_OTHER;

		/*
		 * 0x0000 is the canonical illegal instruction encoding and
		 * is used as alignment padding between functions. Treat it
		 * as a NOP so that unreachable-instruction analysis does not
		 * flag these harmless pad bytes.
		 */
		if (c_inst == 0x0000) {
			insn->type = INSN_NOP;
			return 0;
		}

		if (c_op == 0x2 && c_funct3 == 0x4) {
			/*
			 * Quadrant 2, funct3=100: CR-format jumps/calls
			 *   c.jr   rs1     (bit12=0, rs1!=0, rs2=0) -> JUMP_DYNAMIC
			 *   c.ret          (bit12=0, rs1=ra, rs2=0) -> RETURN
			 *   c.jalr rs1     (bit12=1, rs1!=0, rs2=0) -> CALL_DYNAMIC
			 *     Note: c.jalr is treated as INSN_OTHER because objtool
			 *     cannot resolve the callee from a 16-bit indirect call
			 *     instruction alone (no reloc attached to it).
			 *   c.ebreak       (bit12=1, rs1=0,  rs2=0) -> BUG
			 */
			if (c_rs2 == 0) {
				if (!c_bit12 && c_rs1 != 0) {
					/* c.jr or c.ret */
					if (c_rs1 == REG_RA)
						insn->type = INSN_RETURN;
					else
						insn->type = INSN_JUMP_DYNAMIC;
				} else if (c_bit12 && c_rs1 == 0) {
					/* c.ebreak */
					insn->type = INSN_BUG;
				}
				/* c.jalr: leave as INSN_OTHER */
			}
		} else if (c_op == 0x1) {
			if (c_funct3 == 0x0) {
				/*
				 * Quadrant 1, funct3=000: CI-format
				 *   c.nop:    c.addi rd=0, imm
				 *   c.addi:   c.addi rd, imm  (rd != x0)
				 *
				 * CI-format immediate:
				 *   imm[5]   = inst[12]
				 *   imm[4:0] = inst[6:2]
				 */
				if (c_rs1 == 0) {
					/* c.nop */
					insn->type = INSN_NOP;
				} else if (c_rs1 == REG_SP) {
					/* c.addi sp, sp, imm -- SP adjust */
					s32 ci_imm = (s32)(
						(((u32)c_inst >> 12) & 0x1) << 5 |
						(((u32)c_inst >> 2) & 0x1f)
					);
					/* sign-extend from bit 5 */
					if (ci_imm & (1 << 5))
						ci_imm |= (s32)0xffffffc0;

					insn->immediate = ci_imm;
					ADD_OP(op) {
						op->src.type = OP_SRC_ADD;
						op->src.reg = CFI_SP;
						op->src.offset = ci_imm;
						op->dest.type = OP_DEST_REG;
						op->dest.reg = CFI_SP;
					}
				}
			} else if (c_funct3 == 0x3) {
					/*
					 * Quadrant 1, funct3=011: CI-format
					 *   c.addi rd, imm      (rd != x0, rd != sp)
					 *   c.addi16sp sp, imm   (rd == sp)
					 *
					 * Only care about the sp-modifying variant
					 * (C.ADDI16SP) for CFI tracking.
					 *
					 * C.ADDI16SP encoding (rd=sp):
					 *   nzimm[9]   = inst[12]
					 *   nzimm[4]   = inst[6]
					 *   nzimm[6]   = inst[5]
					 *   nzimm[7:8] = inst[3:4]  (note: swapped)
					 *   nzimm[5]   = inst[2]
					 *
					 * C.ADDI (rd != sp, rd != x0) encoding:
					 *   imm[5]   = inst[12]
					 *   imm[4:0] = inst[6:2]
					 */
					if (c_rs1 == REG_SP) {
						/* C.ADDI16SP: addi sp, sp, nzimm */
						s32 nzimm = (s32)(
							(((u32)c_inst >> 12) & 0x1) << 9  | /* nzimm[9]  from inst[12] */
							(((u32)c_inst >> 4) & 0x1)  << 8  | /* nzimm[8]  from inst[4]  */
							(((u32)c_inst >> 3) & 0x1)  << 7  | /* nzimm[7]  from inst[3]  */
							(((u32)c_inst >> 5) & 0x1)  << 6  | /* nzimm[6]  from inst[5]  */
							(((u32)c_inst >> 2) & 0x1)  << 5  | /* nzimm[5]  from inst[2]  */
							(((u32)c_inst >> 6) & 0x1)  << 4    /* nzimm[4]  from inst[6]  */
						);
						/* sign-extend from bit 9 */
						if (nzimm & (1 << 9))
							nzimm |= (s32)0xfffffc00;
					
						insn->immediate = nzimm;
						ADD_OP(op) {
							op->src.type = OP_SRC_ADD;
							op->src.reg = CFI_SP;
							op->src.offset = nzimm;
							op->dest.type = OP_DEST_REG;
							op->dest.reg = CFI_SP;
						}
					}
				} else if (c_funct3 == 0x5) {
				/*
				 * c.j: CJ-format, quadrant 1, funct3=101
				 * 11-bit signed byte offset, encoded as:
				 *   imm[11]=inst[12], imm[4]=inst[11],
				 *   imm[9:8]=inst[10:9], imm[10]=inst[8],
				 *   imm[6]=inst[7], imm[7]=inst[6],
				 *   imm[3:1]=inst[5:3], imm[5]=inst[2]
				 */
				s32 cj_imm = (s32)(
					(((u32)c_inst & 0x1000) >> 1)  | /* imm[11] */
					(((u32)c_inst & 0x0800) >> 7)  | /* imm[4]  */
					(((u32)c_inst & 0x0600) >> 1)  | /* imm[9:8]*/
					(((u32)c_inst & 0x0100) << 2)  | /* imm[10] */
					(((u32)c_inst & 0x0080) >> 1)  | /* imm[6]  */
					(((u32)c_inst & 0x0040) << 1)  | /* imm[7]  */
					(((u32)c_inst & 0x0038) >> 2)  | /* imm[3:1]*/
					(((u32)c_inst & 0x0004) << 3)    /* imm[5]  */
				);
				/* sign-extend from bit 11 */
				if (cj_imm & (1 << 11))
					cj_imm |= (s32)0xfffff800;
				insn->immediate = cj_imm;
				insn->type = INSN_JUMP_UNCONDITIONAL;
			} else if (c_funct3 == 0x6 || c_funct3 == 0x7) {
				/*
				 * c.beqz / c.bnez: CB-format, quadrant 1
				 * funct3=110 / funct3=111
				 * 8-bit signed byte offset, encoded as:
				 *   imm[8]=inst[12], imm[4:3]=inst[11:10],
				 *   imm[7:6]=inst[6:5], imm[2:1]=inst[4:3],
				 *   imm[5]=inst[2]
				 */
				s32 cb_imm = (s32)(
					(((u32)c_inst & 0x1000) >> 4)  | /* imm[8]  */
					(((u32)c_inst & 0x0c00) >> 7)  | /* imm[4:3]*/
					(((u32)c_inst & 0x0060) << 1)  | /* imm[7:6]*/
					(((u32)c_inst & 0x0018) >> 2)  | /* imm[2:1]*/
					(((u32)c_inst & 0x0004) << 3)    /* imm[5]  */
				);
				/* sign-extend from bit 8 */
				if (cb_imm & (1 << 8))
					cb_imm |= (s32)0xffffff00;
				insn->immediate = cb_imm;
				insn->type = INSN_JUMP_CONDITIONAL;
			}
			/* c.jal (RV32 only, funct3=001): leave as INSN_OTHER */
		}

		return 0;
	}

	if (maxlen < RISCV_INSN_SIZE)
		return 0;

	insn->len = RISCV_INSN_SIZE;
	insn->type = INSN_OTHER;
	insn->immediate = 0;

	inst = *(u32 *)(sec->data->d_buf + offset);

	opcode = RV_INSN_OPCODE(inst);
	rd     = RV_INSN_RD(inst);
	funct3 = RV_INSN_FUNCT3(inst);
	rs1    = RV_INSN_RS1(inst);
	rs2    = RV_INSN_RS2(inst);

	switch (opcode) {
	case OPC_LOAD:
		/*
		 * LD rd, imm(rs1) -- load doubleword
		 * If rs1 == SP or FP, this is a stack restore op.
		 */
		if (funct3 == F3_LD) {
			imm = RV_INSN_I_IMM(inst);
			if (rs1 == REG_SP) {
				insn->immediate = imm;
				ADD_OP(op) {
					op->src.type = OP_SRC_REG_INDIRECT;
					op->src.reg = CFI_SP;
					op->src.offset = imm;
					op->dest.type = OP_DEST_REG;
					op->dest.reg = rd;
				}
			} else if (rs1 == REG_FP) {
				insn->immediate = imm;
				ADD_OP(op) {
					op->src.type = OP_SRC_REG_INDIRECT;
					op->src.reg = CFI_FP;
					op->src.offset = imm;
					op->dest.type = OP_DEST_REG;
					op->dest.reg = rd;
				}
			}
		}
		break;

	case OPC_STORE:
		/*
		 * SD rs2, imm(rs1) -- store doubleword
		 * If rs1 == SP, this is a callee-saved register save.
		 */
		if (funct3 == F3_SD) {
			imm = RV_INSN_S_IMM(inst);
			if (rs1 == REG_SP) {
				insn->immediate = imm;
				ADD_OP(op) {
					op->src.type = OP_SRC_REG;
					op->src.reg = rs2;
					op->dest.type = OP_DEST_REG_INDIRECT;
					op->dest.reg = CFI_SP;
					op->dest.offset = imm;
				}
			} else if (rs1 == REG_FP) {
				insn->immediate = imm;
				ADD_OP(op) {
					op->src.type = OP_SRC_REG;
					op->src.reg = rs2;
					op->dest.type = OP_DEST_REG_INDIRECT;
					op->dest.reg = CFI_FP;
					op->dest.offset = imm;
				}
			}
		}
		break;

	case OPC_OP_IMM:
		/*
		 * ADDI rd, rs1, imm
		 * Key cases:
		 *   addi sp, sp, -imm  -> stack frame allocation
		 *   addi sp, sp, +imm  -> stack frame deallocation
		 *   addi s0, sp, imm   -> frame pointer setup (s0 = sp + imm)
		 *   addi sp, s0, -imm  -> frame pointer restore
		 */
		if (funct3 == F3_ADDI) {
			imm = RV_INSN_I_IMM(inst);
			if (rd == REG_SP && rs1 == REG_SP) {
				/* addi sp, sp, imm -- SP adjust */
				insn->immediate = imm;
				ADD_OP(op) {
					op->src.type = OP_SRC_ADD;
					op->src.reg = CFI_SP;
					op->src.offset = imm;
					op->dest.type = OP_DEST_REG;
					op->dest.reg = CFI_SP;
				}
			} else if (rd == REG_FP && rs1 == REG_SP) {
				/* addi s0, sp, imm -- FP setup from SP */
				insn->immediate = imm;
				ADD_OP(op) {
					op->src.type = OP_SRC_ADD;
					op->src.reg = CFI_SP;
					op->src.offset = imm;
					op->dest.type = OP_DEST_REG;
					op->dest.reg = CFI_FP;
				}
			} else if (rd == REG_SP && rs1 == REG_FP) {
				/* addi sp, s0, imm -- SP restore from FP */
				struct symbol *func = find_func_containing(insn->sec, insn->offset);

				if (func)
					func->frame_pointer = true;

				insn->immediate = imm;
				ADD_OP(op) {
					op->src.type = OP_SRC_ADD;
					op->src.reg = CFI_FP;
					op->src.offset = imm;
					op->dest.type = OP_DEST_REG;
					op->dest.reg = CFI_SP;
				}
			} else if (inst == 0x00000013) {
				/* addi zero, zero, 0 -- canonical NOP */
				insn->type = INSN_NOP;
			}
		}
		break;

	case OPC_JAL:
		/*
		 * JAL rd, offset
		 *   JAL x0, offset  -> unconditional jump (no link)
		 *   JAL ra, offset  -> call
		 *   JAL rd, offset  -> intra-function jump (rd != x0 && rd != ra)
		 *
		 * On RISC-V, using a non-ra register as the link register
		 * for JAL typically indicates an intra-function subroutine
		 * call pattern (e.g. jal t0, sub / jr t0 to return).  Objtool
		 * cannot track the return through jr t0, so treat these as
		 * unconditional jumps rather than calls to avoid false
		 * unreachable-instruction warnings on the fall-through path.
		 */
		imm = RV_INSN_J_IMM(inst);
		insn->immediate = imm;
		if (rd == REG_ZERO) {
			insn->type = INSN_JUMP_UNCONDITIONAL;
		} else if (rd == REG_RA) {
			insn->type = INSN_CALL;
		} else {
			insn->type = INSN_CALL;
		}
		break;

	case OPC_JALR:
		/*
		 * JALR rd, rs1, imm
		 *   JALR x0, ra, 0  -> ret (return)
		 *   JALR ra, rs1, 0 -> indirect call
		 *   JALR x0, rs1, 0 -> indirect jump (tail call)
		 */
		if (funct3 == F3_JALR) {
			imm = RV_INSN_I_IMM(inst);
			if (rd == REG_ZERO && rs1 == REG_RA && imm == 0) {
				/* ret -- jalr x0, ra, 0 */
				insn->type = INSN_RETURN;
			} else if (rd == REG_RA) {
				/* jalr ra, rs1, imm -- indirect call */
				insn->type = INSN_CALL_DYNAMIC;
			} else if (rd == REG_ZERO && imm == 0) {
				/* jalr x0, rs1, 0 -- indirect jump */
				insn->type = INSN_JUMP_DYNAMIC;
			} else {
				insn->type = INSN_JUMP_DYNAMIC;
			}
		}
		break;

	case OPC_BRANCH:
		/*
		 * BEQ, BNE, BLT, BGE, BLTU, BGEU -- conditional branch
		 */
		imm = RV_INSN_B_IMM(inst);
		insn->immediate = imm;
		insn->type = INSN_JUMP_CONDITIONAL;
		break;

	case OPC_SYSTEM:
		if (funct3 == F3_PRIV) {
			u32 funct12 = inst >> 20;

			if (funct12 == F12_EBREAK) {
				insn->type = INSN_BUG;
			} else if (funct12 == F12_SRET || funct12 == F12_MRET) {
				/* sret/mret -- return from trap */
				insn->type = INSN_RETURN;
			}
		}
		break;

	default:
		break;
	}

	return 0;
}

const char *arch_nop_insn(int len)
{
	static u32 nop = 0x00000013; /* addi zero, zero, 0 */

	if (len != RISCV_INSN_SIZE) {
		ERROR("invalid NOP size: %d\n", len);
		return NULL;
	}

	return (const char *)&nop;
}

const char *arch_ret_insn(int len)
{
	/* jalr x0, ra, 0 */
	static u32 ret = 0x00008067;

	if (len != RISCV_INSN_SIZE) {
		ERROR("invalid RET size: %d\n", len);
		return NULL;
	}

	return (const char *)&ret;
}

void arch_initial_func_cfi_state(struct cfi_init_state *state)
{
	int i;

	for (i = 0; i < CFI_NUM_REGS; i++) {
		state->regs[i].base = CFI_UNDEFINED;
		state->regs[i].offset = 0;
	}

	/* Initial CFA: SP + 0 */
	state->cfa.base = CFI_SP;
	state->cfa.offset = 0;
}

unsigned int arch_reloc_size(struct reloc *reloc)
{
	switch (reloc_type(reloc)) {
	case R_RISCV_32:
	case R_RISCV_32_PCREL:
	case R_RISCV_ADD32:
		/* For ADD32+SUB32 switch-table pairs, the ADD32 is the primary
		 * reloc carrying the target symbol.  Its size is 4 (one table
		 * entry) so the consecutive-entry check advances correctly to
		 * the next slot after the ADD32+SUB32 pair is consumed.
		 */
		return 4;
	case R_RISCV_SUB32:
		/* SUB32 shares the same offset as its paired ADD32; report
		 * size 0 so the consecutive-entry check does not advance the
		 * expected offset before the next ADD32 is seen.
		 */
		return 0;
	default:
		return 8;
	}
}

unsigned long arch_jump_table_sym_offset(struct reloc *reloc, struct reloc *table)
{
	switch (reloc_type(reloc)) {
	case R_RISCV_32_PCREL:
		return reloc->sym->offset + reloc_addend(reloc) -
		       (reloc_offset(reloc) - reloc_offset(table));
	case R_RISCV_ADD32:
		/*
		 * For PC-relative switch tables the ADD32 reloc carries the
		 * target label directly as its symbol+addend.  The base offset
		 * is subtracted by the paired SUB32, but objtool only needs the
		 * target symbol's section-relative offset, which the ADD32 sym
		 * already provides.
		 */
		return reloc->sym->offset + reloc_addend(reloc);
	default:
		return reloc->sym->offset + reloc_addend(reloc);
	}
}

/*
 * RISC-V PC-relative switch tables store each entry as an ADD32+SUB32 pair
 * at the same .rodata offset.  add_jump_table() processes one reloc at a
 * time; the ADD32 carries the target symbol so we handle it normally, while
 * the SUB32 only encodes the table-base symbol and should be skipped.
 */
bool arch_jump_table_reloc_skip(struct reloc *reloc, struct reloc *table)
{
	return reloc_type(reloc) == R_RISCV_SUB32;
}

#ifdef DISAS

int arch_disas_info_init(struct disassemble_info *dinfo)
{
	return disas_info_init(dinfo, bfd_arch_riscv,
			       bfd_mach_riscv32, bfd_mach_riscv64,
			       NULL);
}

#endif /* DISAS */

/*
 * Resolve the call destination for a RISC-V auipc+jalr call pair.
 *
 * RISC-V position-independent calls are encoded as a two-instruction
 * sequence:
 *   auipc  ra, %pcrel_hi(sym)   <- R_RISCV_CALL_PLT reloc here
 *   jalr   ra, %pcrel_lo(.)(ra) <- no reloc; decoded as INSN_CALL_DYNAMIC
 *
 * objtool sees the jalr and marks it INSN_CALL_DYNAMIC, but the relocation
 * (and therefore the callee symbol) lives on the preceding auipc.  This
 * function bridges the gap: given the jalr instruction, it looks back 4
 * bytes for an auipc ra instruction carrying an R_RISCV_CALL_PLT reloc and
 * returns the target symbol so the caller can check whether it is noreturn.
 */
struct symbol *arch_dynamic_call_dest(struct objtool_file *file,
				      struct instruction *insn)
{
	struct reloc *reloc;
	u32 prev_inst;

	/* Need at least 4 bytes before jalr for the auipc */
	if (insn->offset < RISCV_INSN_SIZE)
		return NULL;

	/* Peek at the preceding 32-bit instruction */
	prev_inst = *(u32 *)(insn->sec->data->d_buf + insn->offset - RISCV_INSN_SIZE);

	/* auipc ra: opcode=0x17, rd=ra(1).  Lower 12 bits encode rd and opcode. */
	if ((prev_inst & 0xfff) != ((REG_RA << 7) | OPC_AUIPC))
		return NULL;

	/*
	 * Look for a reloc at the auipc offset.  Thanks to the
	 * find_reloc_by_dest_range() fix that prefers named-symbol relocs over
	 * R_RISCV_RELAX (sym=0), this will return the R_RISCV_CALL_PLT reloc
	 * when both are present at the same offset.
	 */
	reloc = find_reloc_by_dest(file->elf, (struct section *)insn->sec,
				   insn->offset - RISCV_INSN_SIZE);
	if (!reloc || reloc_type(reloc) != R_RISCV_CALL_PLT)
		return NULL;

	return reloc->sym;
}
