/*
 * Copyright (C) 1995-2010 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief    Peephole optimization and legalization of a sparc function
 * @author   Matthias Braun
 * @version  $Id$
 *
 * A note on sparc stackpointer (sp) behaviour:
 * The ABI expects SPARC_MIN_STACKSIZE bytes to be available at the
 * stackpointer. This space will be used to spill register windows,
 * and for spilling va_arg arguments (maybe we can optimize this away for
 * statically known not-va-arg-functions...)
 * This in effect means that we allocate that extra space at the function begin
 * which is easy. But this space isn't really fixed at the beginning of the
 * stackframe. Instead you should rather imagine the space as always being the
 * last-thing on the stack.
 * So when addressing anything stack-specific we have to account for this
 * area, while our compiler thinks the space is occupied at the beginning
 * of the stack frame. The code here among other things adjusts these offsets
 * accordingly.
 */
#include "config.h"

#include "bearch_sparc_t.h"
#include "gen_sparc_regalloc_if.h"
#include "sparc_new_nodes.h"
#include "irprog.h"
#include "irgmod.h"
#include "ircons.h"

#include "../bepeephole.h"
#include "../benode.h"
#include "../besched.h"

static void kill_unused_stacknodes(ir_node *node)
{
	if (get_irn_n_edges(node) > 0)
		return;

	if (be_is_IncSP(node)) {
		sched_remove(node);
		kill_node(node);
	} else if (is_Phi(node)) {
		int       arity = get_irn_arity(node);
		ir_node **ins   = ALLOCAN(ir_node*, arity);
		int       i;
		sched_remove(node);
		memcpy(ins, get_irn_in(node), arity*sizeof(ins[0]));
		kill_node(node);

		for (i = 0; i < arity; ++i)
			kill_unused_stacknodes(ins[i]);
	}
}

static void introduce_epilog(ir_node *ret)
{
	const arch_register_t *sp_reg     = &sparc_registers[REG_SP];
	ir_graph              *irg        = get_irn_irg(ret);
	be_stack_layout_t     *layout     = be_get_irg_stack_layout(irg);
	ir_node               *block      = get_nodes_block(ret);
	ir_type               *frame_type = get_irg_frame_type(irg);
	unsigned               frame_size = get_type_size_bytes(frame_type);
	int                    sp_idx     = be_find_return_reg_input(ret, sp_reg);
	ir_node               *sp         = get_irn_n(ret, sp_idx);

	if (!layout->sp_relative) {
		const arch_register_t *fp_reg = &sparc_registers[REG_FRAME_POINTER];
		ir_node *fp      = be_get_initial_reg_value(irg, fp_reg);
		ir_node *restore = new_bd_sparc_RestoreZero(NULL, block, fp);
		sched_add_before(ret, restore);
		arch_set_irn_register(restore, sp_reg);
		set_irn_n(ret, sp_idx, restore);

		kill_unused_stacknodes(sp);
	} else {
		ir_node *incsp  = be_new_IncSP(sp_reg, block, sp, frame_size, 0);
		set_irn_n(ret, sp_idx, incsp);
		sched_add_before(ret, incsp);
	}
}

void sparc_introduce_prolog_epilog(ir_graph *irg)
{
	const arch_register_t *sp_reg     = &sparc_registers[REG_SP];
	ir_node               *start      = get_irg_start(irg);
	be_stack_layout_t     *layout     = be_get_irg_stack_layout(irg);
	ir_node               *block      = get_nodes_block(start);
	ir_node               *initial_sp = be_get_initial_reg_value(irg, sp_reg);
	ir_node               *sp         = initial_sp;
	ir_node               *schedpoint = start;
	ir_type               *frame_type = get_irg_frame_type(irg);
	unsigned               frame_size = get_type_size_bytes(frame_type);

	/* introduce epilog for every return node */
	{
		ir_node *end_block = get_irg_end_block(irg);
		int      arity     = get_irn_arity(end_block);
		int      i;

		for (i = 0; i < arity; ++i) {
			ir_node *ret = get_irn_n(end_block, i);
			assert(be_is_Return(ret));
			introduce_epilog(ret);
		}
	}

	while (be_is_Keep(sched_next(schedpoint)))
		schedpoint = sched_next(schedpoint);

	if (!layout->sp_relative) {
		ir_node *incsp;
		ir_node *save = new_bd_sparc_Save_imm(NULL, block, sp, NULL,
		                                      -SPARC_MIN_STACKSIZE);
		arch_set_irn_register(save, sp_reg);
		sched_add_after(schedpoint, save);
		schedpoint = save;

		incsp = be_new_IncSP(sp_reg, block, save, frame_size, 0);
		edges_reroute(initial_sp, incsp);
		set_irn_n(save, n_sparc_Save_stack, initial_sp);
		sched_add_after(schedpoint, incsp);
		schedpoint = incsp;

		/* we still need the IncSP even if noone is explicitely using the
		 * value. (TODO: this isn't 100% correct yet, something at the end of
		 * the function should hold the IncSP, even if we use a restore
		 * which just overrides it instead of using the value)
		 */
		if (get_irn_n_edges(incsp) == 0) {
			ir_node *in[] = { incsp };
			ir_node *keep = be_new_Keep(block, 1, in);
			sched_add_after(schedpoint, keep);
		}
	} else {
		ir_node *incsp = be_new_IncSP(sp_reg, block, sp, frame_size, 0);
		edges_reroute(initial_sp, incsp);
		be_set_IncSP_pred(incsp, sp);
		sched_add_after(schedpoint, incsp);
	}
}

static void finish_sparc_Save(ir_node *node)
{
	sparc_attr_t *attr = get_sparc_attr(node);
	int offset = attr->immediate_value;
	ir_node  *schedpoint = node;
	dbg_info *dbgi;
	ir_node  *block;
	ir_node  *new_save;
	ir_node  *stack;
	ir_entity *entity;

	if (sparc_is_value_imm_encodeable(offset))
		return;

	/* uhh only works for the imm variant yet */
	assert(get_irn_arity(node) == 1);

	block = get_nodes_block(node);
	dbgi = get_irn_dbg_info(node);
	stack = get_irn_n(node, n_sparc_Save_stack);
	entity = attr->immediate_value_entity;
	new_save = new_bd_sparc_Save_imm(dbgi, block, stack, entity, 0);
	arch_set_irn_register(new_save, &sparc_registers[REG_SP]);
	stack = new_save;

	sched_add_after(node, new_save);
	schedpoint = new_save;
	while (offset > SPARC_IMMEDIATE_MAX || offset < SPARC_IMMEDIATE_MIN) {
		if (offset > 0) {
			stack = be_new_IncSP(&sparc_registers[REG_SP], block, stack,
			                     SPARC_IMMEDIATE_MIN, 0);
			offset -= -SPARC_IMMEDIATE_MIN;
		} else {
			stack = be_new_IncSP(&sparc_registers[REG_SP], block, stack,
			                     -SPARC_IMMEDIATE_MIN, 0);
			offset -= SPARC_IMMEDIATE_MIN;
		}
		sched_add_after(schedpoint, stack);
		schedpoint = stack;
	}
	attr = get_sparc_attr(new_save);
	attr->immediate_value = offset;
	be_peephole_exchange(node, stack);
}

/**
 * sparc immediates are limited. Split IncSP with bigger immediates if
 * necessary.
 */
static void finish_be_IncSP(ir_node *node)
{
	int      sign   = 1;
	int      offset = be_get_IncSP_offset(node);
	ir_node *sp     = be_get_IncSP_pred(node);
	ir_node *block;

	/* we might have to break the IncSP apart if the constant has become too
	 * big */
	if (offset < 0) {
		offset = -offset;
		sign   = -1;
	}

	if (sparc_is_value_imm_encodeable(-offset))
		return;

	/* split incsp into multiple instructions */
	block = get_nodes_block(node);
	while (offset > -SPARC_IMMEDIATE_MIN) {
		sp = be_new_IncSP(&sparc_registers[REG_SP], block, sp,
		                  sign * -SPARC_IMMEDIATE_MIN, 0);
		sched_add_before(node, sp);
		offset -= -SPARC_IMMEDIATE_MIN;
	}

	be_set_IncSP_pred(node, sp);
	be_set_IncSP_offset(node, sign*offset);
}

/**
 * adjust sp-relative offsets. Split into multiple instructions if offset
 * exceeds sparc immediate range.
 */
static void finish_sparc_FrameAddr(ir_node *node)
{
	/* adapt to sparc stack magic */
	sparc_attr_t *attr   = get_sparc_attr(node);
	int           offset = attr->immediate_value;
	ir_node      *base   = get_irn_n(node, n_sparc_FrameAddr_base);
	dbg_info     *dbgi   = get_irn_dbg_info(node);
	ir_node      *block  = get_nodes_block(node);
	int           sign   = 1;
	bool          sp_relative
		= arch_get_irn_register(base) == &sparc_registers[REG_SP];
	if (sp_relative) {
		offset += SPARC_MIN_STACKSIZE;
	}

	if (offset < 0) {
		sign   = -1;
		offset = -offset;
	}

	if (offset > -SPARC_IMMEDIATE_MIN) {
		ir_entity *entity = attr->immediate_value_entity;
		ir_node   *new_frameaddr
			= new_bd_sparc_FrameAddr(dbgi, block, base, entity, 0);
		ir_node   *schedpoint = node;
		const arch_register_t *reg = arch_get_irn_register(node);

		sched_add_after(schedpoint, new_frameaddr);
		schedpoint = new_frameaddr;
		arch_set_irn_register(new_frameaddr, reg);
		base = new_frameaddr;

		while (offset > -SPARC_IMMEDIATE_MIN) {
			if (sign > 0) {
				base = new_bd_sparc_Sub_imm(dbgi, block, base, NULL,
											SPARC_IMMEDIATE_MIN);
			} else {
				base = new_bd_sparc_Add_imm(dbgi, block, base, NULL,
											SPARC_IMMEDIATE_MIN);
			}
			arch_set_irn_register(base, reg);
			sched_add_after(schedpoint, base);
			schedpoint = base;

			offset -= -SPARC_IMMEDIATE_MIN;
		}

		be_peephole_exchange(node, base);
		attr = get_sparc_attr(new_frameaddr);
	}
	attr->immediate_value = sign*offset;
}

static void finish_sparc_LdSt(ir_node *node)
{
	sparc_load_store_attr_t *attr = get_sparc_load_store_attr(node);
	if (attr->is_frame_entity) {
		ir_node *base;
		bool     sp_relative;
		if (is_sparc_Ld(node) || is_sparc_Ldf(node)) {
			base = get_irn_n(node, n_sparc_Ld_ptr);
		} else {
			assert(is_sparc_St(node) || is_sparc_Stf(node));
			base = get_irn_n(node, n_sparc_St_ptr);
		}
		sp_relative = arch_get_irn_register(base) == &sparc_registers[REG_SP];
		if (sp_relative)
			attr->base.immediate_value += SPARC_MIN_STACKSIZE;
	}
}

static void peephole_be_IncSP(ir_node *node)
{
	ir_node *pred;
	node = be_peephole_IncSP_IncSP(node);
	if (!be_is_IncSP(node))
		return;

	pred = be_get_IncSP_pred(node);
	if (is_sparc_Save(pred) && be_has_only_one_user(pred)) {
		int offset = -be_get_IncSP_offset(node);
		sparc_attr_t *attr = get_sparc_attr(pred);
		attr->immediate_value += offset;
		be_peephole_exchange(node, pred);
	}
}

static void peephole_sparc_FrameAddr(ir_node *node)
{
	/* the peephole code currently doesn't allow this since it changes
	 * the register. Find out why and how to workaround this... */
#if 0
	const sparc_attr_t *attr = get_sparc_attr_const(node);
	if (attr->immediate_value == 0) {
		ir_node *base = get_irn_n(node, n_sparc_FrameAddr_base);
		be_peephole_exchange(node, base);
	}
#endif
	(void) node;
}

static void finish_be_Return(ir_node *node)
{
	ir_node *schedpoint = node;
	ir_node *restore;
	/* see that there is no code between Return and restore, if there is move
	 * it in front of the restore */
	while (true) {
		if (!sched_has_prev(schedpoint))
			return;
		schedpoint = sched_prev(schedpoint);
		if (is_sparc_Restore(schedpoint) || is_sparc_RestoreZero(schedpoint))
			break;
	}
	restore = schedpoint;
	schedpoint = sched_prev(node);
	/* move all code between return and restore up */
	while (schedpoint != restore) {
		ir_node *next_schedpoint = sched_prev(schedpoint);
		sched_remove(schedpoint);
		sched_add_before(restore, schedpoint);
		schedpoint = next_schedpoint;
	}
}

static void register_peephole_optimisation(ir_op *op, peephole_opt_func func)
{
	assert(op->ops.generic == NULL);
	op->ops.generic = (op_func) func;
}

void sparc_finish(ir_graph *irg)
{
	/* perform peephole optimizations */
	clear_irp_opcodes_generic_func();
	register_peephole_optimisation(op_be_IncSP,        peephole_be_IncSP);
	register_peephole_optimisation(op_sparc_FrameAddr, peephole_sparc_FrameAddr);
	be_peephole_opt(irg);

	/* perform legalizations (mostly fix nodes with too big immediates) */
	clear_irp_opcodes_generic_func();
	register_peephole_optimisation(op_be_IncSP,        finish_be_IncSP);
	register_peephole_optimisation(op_be_Return,       finish_be_Return);
	register_peephole_optimisation(op_sparc_FrameAddr, finish_sparc_FrameAddr);
	register_peephole_optimisation(op_sparc_Ld,        finish_sparc_LdSt);
	register_peephole_optimisation(op_sparc_Ldf,       finish_sparc_LdSt);
	register_peephole_optimisation(op_sparc_Save,      finish_sparc_Save);
	register_peephole_optimisation(op_sparc_St,        finish_sparc_LdSt);
	register_peephole_optimisation(op_sparc_Stf,       finish_sparc_LdSt);
	be_peephole_opt(irg);
}
