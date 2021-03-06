/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2013
 * Copyright (c) Valentine Sinitsyn, 2014
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *  Valentine Sinitsyn <valentine.sinitsyn@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/mmio.h>
#include <jailhouse/paging.h>
#include <jailhouse/printk.h>
#include <asm/ioapic.h>
#include <asm/iommu.h>
#include <asm/vcpu.h>

#define X86_MAX_INST_LEN	15

/*
 * There are a few instructions that can have 8-byte immediate values
 * on 64-bit mode, but they are not supported/expected here, so we are
 * safe.
 */
#define IMMEDIATE_SIZE          4

union opcode {
	u8 raw;
	struct { /* REX */
		u8 b:1, x:1, r:1, w:1;
		u8 code:4;
	} __attribute__((packed)) rex;
	struct {
		u8 rm:3;
		u8 reg:3;
		u8 mod:2;
	} __attribute__((packed)) modrm;
	struct {
		u8 base:3;
		u8 index:3;
		u8 ss:2;
	} __attribute__((packed)) sib;
};

struct parse_context {
	unsigned int remaining;
	unsigned int count;
	unsigned int size;
	const u8 *inst;
};

static bool ctx_update(struct parse_context *ctx,
		       unsigned long *pc, unsigned int advance,
		       const struct guest_paging_structures *pg)
{
	ctx->inst += advance;
	ctx->count += advance;
	if (ctx->size > advance) {
		ctx->size -= advance;
	} else {
		ctx->size = ctx->remaining;
		ctx->inst = vcpu_get_inst_bytes(pg, *pc, &ctx->size);
		if (!ctx->inst)
			return false;
		ctx->remaining -= ctx->size;
		*pc += ctx->size;
	}
	return true;
}

struct mmio_instruction x86_mmio_parse(unsigned long pc,
	const struct guest_paging_structures *pg_structs, bool is_write)
{
	struct parse_context ctx = { .remaining = X86_MAX_INST_LEN,
				     .count = 1 };
	union registers *guest_regs = &this_cpu_data()->guest_regs;
	struct mmio_instruction inst = { .inst_len = 0 };
	bool has_immediate = false;
	union opcode op[4] = { };
	bool does_write = false;
	bool has_rex_w = false;
	bool has_rex_r = false;
	unsigned int n;

	if (!ctx_update(&ctx, &pc, 0, pg_structs))
		goto error_noinst;

restart:
	op[0].raw = *ctx.inst;
	if (op[0].rex.code == X86_REX_CODE) {
		if (op[0].rex.w)
			has_rex_w = true;
		if (op[0].rex.r)
			has_rex_r = true;
		if (op[0].rex.x)
			goto error_unsupported;

		if (!ctx_update(&ctx, &pc, 1, pg_structs))
			goto error_noinst;
		goto restart;
	}
	switch (op[0].raw) {
	case X86_OP_MOVZX_OPC1:
		if (!ctx_update(&ctx, &pc, 1, pg_structs))
			goto error_noinst;
		op[1].raw = *ctx.inst;
		if (op[1].raw == X86_OP_MOVZX_OPC2_B)
			inst.access_size = 1;
		else if (op[1].raw == X86_OP_MOVZX_OPC2_W)
			inst.access_size = 2;
		else
			goto error_unsupported;
		break;
	case X86_OP_MOVB_TO_MEM:
		inst.access_size = 1;
		does_write = true;
		break;
	case X86_OP_MOV_TO_MEM:
		inst.access_size = has_rex_w ? 8 : 4;
		does_write = true;
		break;
	case X86_OP_MOV_FROM_MEM:
		inst.access_size = has_rex_w ? 8 : 4;
		break;
	case X86_OP_MOV_IMMEDIATE_TO_MEM:
		inst.access_size = has_rex_w ? 8 : 4;
		has_immediate = true;
		does_write = true;
		break;
	case X86_OP_MOV_MEM_TO_AX:
		inst.inst_len = ctx.count + 4;
		inst.access_size = has_rex_w ? 8 : 4;
		inst.in_reg_num = 15;
		goto final;
	case X86_OP_MOV_AX_TO_MEM:
		inst.inst_len = ctx.count + 4;
		inst.access_size = has_rex_w ? 8 : 4;
		inst.out_val = guest_regs->by_index[15];
		does_write = true;
		goto final;
	default:
		goto error_unsupported;
	}

	if (!ctx_update(&ctx, &pc, 1, pg_structs))
		goto error_noinst;

	op[2].raw = *ctx.inst;
	switch (op[2].modrm.mod) {
	case 0:
		if (op[2].modrm.rm == 5) { /* 32-bit displacement */
			inst.inst_len += 4;
			/* walk displacement bytes, to point to immediate */
			if (has_immediate &&
			    !ctx_update(&ctx, &pc, 4, pg_structs))
				goto error_noinst;
			break;
		} else if (op[2].modrm.rm != 4) { /* no SIB */
			break;
		}


		if (!ctx_update(&ctx, &pc, 1, pg_structs))
			goto error_noinst;

		op[3].raw = *ctx.inst;
		if (op[3].sib.base == 5)
			inst.inst_len += 4;
		break;
	case 1:
	case 2:
		if (op[2].modrm.rm == 4) /* SIB */
			goto error_unsupported;
		inst.inst_len += op[2].modrm.mod == 1 ? 1 : 4;
		break;
	default:
		goto error_unsupported;
	}

	inst.inst_len += ctx.count;
	if (has_rex_r)
		inst.in_reg_num = 7 - op[2].modrm.reg;
	else if (op[2].modrm.reg == 4)
		goto error_unsupported;
	else
		inst.in_reg_num = 15 - op[2].modrm.reg;

	if (has_immediate)
		for (n = 0; n < IMMEDIATE_SIZE; n++) {
			if (!ctx_update(&ctx, &pc, 1, pg_structs))
				goto error_noinst;
			inst.out_val |= (unsigned long)*ctx.inst << (n * 8);
		}
	else if (does_write)
		inst.out_val = guest_regs->by_index[inst.in_reg_num];

final:
	if (does_write != is_write)
		goto error_inconsitent;

	return inst;

error_noinst:
	panic_printk("FATAL: unable to get MMIO instruction\n");
	goto error;

error_unsupported:
	panic_printk("FATAL: unsupported instruction "
		     "(0x%02x [0x%02x] 0x%02x 0x%02x)\n",
		     op[0].raw, op[1].raw, op[2].raw, op[3].raw);
	goto error;

error_inconsitent:
	panic_printk("FATAL: inconsistent access, expected %s instruction\n",
		     is_write ? "write" : "read");
error:
	inst.inst_len = 0;
	return inst;
}

unsigned int arch_mmio_count_regions(struct cell *cell)
{
	return ioapic_mmio_count_regions(cell) + iommu_mmio_count_regions(cell);
}
