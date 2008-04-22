/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
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
 * @brief       Implements several optimizations for IA32.
 * @author      Matthias Braun, Christian Wuerdig
 * @version     $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "irnode.h"
#include "irprog_t.h"
#include "ircons.h"
#include "irtools.h"
#include "firm_types.h"
#include "iredges.h"
#include "tv.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "height.h"
#include "irbitset.h"
#include "irprintf.h"
#include "error.h"

#include "../be_t.h"
#include "../beabi.h"
#include "../benode_t.h"
#include "../besched_t.h"
#include "../bepeephole.h"

#include "ia32_new_nodes.h"
#include "ia32_optimize.h"
#include "bearch_ia32_t.h"
#include "gen_ia32_regalloc_if.h"
#include "ia32_transform.h"
#include "ia32_dbg_stat.h"
#include "ia32_util.h"
#include "ia32_architecture.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static const arch_env_t *arch_env;
static ia32_code_gen_t  *cg;

static void peephole_IncSP_IncSP(ir_node *node);

#if 0
static void peephole_ia32_Store_IncSP_to_push(ir_node *node)
{
	ir_node  *base  = get_irn_n(node, n_ia32_Store_base);
	ir_node  *index = get_irn_n(node, n_ia32_Store_index);
	ir_node  *mem   = get_irn_n(node, n_ia32_Store_mem);
	ir_node  *incsp = base;
	ir_node  *val;
	ir_node  *noreg;
	ir_graph *irg;
	ir_node  *block;
	dbg_info *dbgi;
	ir_mode  *mode;
	ir_node  *push;
	ir_node  *proj;
	int       offset;
	int       node_offset;

	/* nomem inidicates the store doesn't alias with anything else */
	if(!is_NoMem(mem))
		return;

	/* find an IncSP in front of us, we might have to skip barriers for this */
	while(is_Proj(incsp)) {
		ir_node *proj_pred = get_Proj_pred(incsp);
		if(!be_is_Barrier(proj_pred))
			return;
		incsp = get_irn_n(proj_pred, get_Proj_proj(incsp));
	}
	if(!be_is_IncSP(incsp))
		return;

	peephole_IncSP_IncSP(incsp);

	/* must be in the same block */
	if(get_nodes_block(incsp) != get_nodes_block(node))
		return;

	if(!is_ia32_NoReg_GP(index) || get_ia32_am_sc(node) != NULL) {
		panic("Invalid storeAM found (%+F)", node);
	}

	/* we should be the store to the end of the stackspace */
	offset      = be_get_IncSP_offset(incsp);
	mode        = get_ia32_ls_mode(node);
	node_offset = get_ia32_am_offs_int(node);
	if(node_offset != offset - get_mode_size_bytes(mode))
		return;

	/* we can use a push instead of the store */
	irg   = current_ir_graph;
	block = get_nodes_block(node);
	dbgi  = get_irn_dbg_info(node);
	noreg = ia32_new_NoReg_gp(cg);
	base  = be_get_IncSP_pred(incsp);
	val   = get_irn_n(node, n_ia32_Store_val);
	push  = new_rd_ia32_Push(dbgi, irg, block, noreg, noreg, mem, val, base);

	proj  = new_r_Proj(irg, block, push, mode_M, pn_ia32_Push_M);

	be_set_IncSP_offset(incsp, offset - get_mode_size_bytes(mode));

	sched_add_before(node, push);
	sched_remove(node);

	be_peephole_before_exchange(node, proj);
	exchange(node, proj);
	be_peephole_after_exchange(proj);
}

static void peephole_ia32_Store(ir_node *node)
{
	peephole_ia32_Store_IncSP_to_push(node);
}
#endif

static int produces_zero_flag(ir_node *node, int pn)
{
	ir_node                     *count;
	const ia32_immediate_attr_t *imm_attr;

	if(!is_ia32_irn(node))
		return 0;

	if(pn >= 0) {
		if(pn != pn_ia32_res)
			return 0;
	}

	switch(get_ia32_irn_opcode(node)) {
	case iro_ia32_Add:
	case iro_ia32_Adc:
	case iro_ia32_And:
	case iro_ia32_Or:
	case iro_ia32_Xor:
	case iro_ia32_Sub:
	case iro_ia32_Sbb:
	case iro_ia32_Neg:
	case iro_ia32_Inc:
	case iro_ia32_Dec:
		return 1;

	case iro_ia32_ShlD:
	case iro_ia32_ShrD:
	case iro_ia32_Shl:
	case iro_ia32_Shr:
	case iro_ia32_Sar:
		assert(n_ia32_ShlD_count == n_ia32_ShrD_count);
		assert(n_ia32_Shl_count == n_ia32_Shr_count
				&& n_ia32_Shl_count == n_ia32_Sar_count);
		if(is_ia32_ShlD(node) || is_ia32_ShrD(node)) {
			count = get_irn_n(node, n_ia32_ShlD_count);
		} else {
			count = get_irn_n(node, n_ia32_Shl_count);
		}
		/* when shift count is zero the flags are not affected, so we can only
		 * do this for constants != 0 */
		if(!is_ia32_Immediate(count))
			return 0;

		imm_attr = get_ia32_immediate_attr_const(count);
		if(imm_attr->symconst != NULL)
			return 0;
		if((imm_attr->offset & 0x1f) == 0)
			return 0;
		return 1;

	default:
		break;
	}
	return 0;
}

static ir_node *turn_into_mode_t(ir_node *node)
{
	ir_node               *block;
	ir_node               *res_proj;
	ir_node               *new_node;
	const arch_register_t *reg;

	if(get_irn_mode(node) == mode_T)
		return node;

	assert(get_irn_mode(node) == mode_Iu);

	new_node = exact_copy(node);
	set_irn_mode(new_node, mode_T);

	block    = get_nodes_block(new_node);
	res_proj = new_r_Proj(current_ir_graph, block, new_node, mode_Iu,
	                      pn_ia32_res);

	reg = arch_get_irn_register(arch_env, node);
	arch_set_irn_register(arch_env, res_proj, reg);

	be_peephole_before_exchange(node, res_proj);
	sched_add_before(node, new_node);
	sched_remove(node);
	exchange(node, res_proj);
	be_peephole_after_exchange(res_proj);

	return new_node;
}

static void peephole_ia32_Test(ir_node *node)
{
	ir_node *left  = get_irn_n(node, n_ia32_Test_left);
	ir_node *right = get_irn_n(node, n_ia32_Test_right);
	ir_node *flags_proj;
	ir_node *block;
	ir_mode *flags_mode;
	int      pn    = -1;
	ir_node *schedpoint;
	const ir_edge_t       *edge;

	assert(n_ia32_Test_left == n_ia32_Test8Bit_left
			&& n_ia32_Test_right == n_ia32_Test8Bit_right);

	/* we need a test for 0 */
	if(left != right)
		return;

	block = get_nodes_block(node);
	if(get_nodes_block(left) != block)
		return;

	if(is_Proj(left)) {
		pn   = get_Proj_proj(left);
		left = get_Proj_pred(left);
	}

	/* happens rarely, but if it does code will panic' */
	if (is_ia32_Unknown_GP(left))
		return;

	/* walk schedule up and abort when we find left or some other node destroys
	   the flags */
	schedpoint = sched_prev(node);
	while(schedpoint != left) {
		schedpoint = sched_prev(schedpoint);
		if(arch_irn_is(arch_env, schedpoint, modify_flags))
			return;
		if(schedpoint == block)
			panic("couldn't find left");
	}

	/* make sure only Lg/Eq tests are used */
	foreach_out_edge(node, edge) {
		ir_node *user = get_edge_src_irn(edge);
		int      pnc  = get_ia32_condcode(user);

		if(pnc != pn_Cmp_Eq && pnc != pn_Cmp_Lg) {
			return;
		}
	}

	if(!produces_zero_flag(left, pn))
		return;

	left = turn_into_mode_t(left);

	flags_mode = ia32_reg_classes[CLASS_ia32_flags].mode;
	flags_proj = new_r_Proj(current_ir_graph, block, left, flags_mode,
	                        pn_ia32_flags);
	arch_set_irn_register(arch_env, flags_proj, &ia32_flags_regs[REG_EFLAGS]);

	assert(get_irn_mode(node) != mode_T);

	be_peephole_before_exchange(node, flags_proj);
	exchange(node, flags_proj);
	sched_remove(node);
	be_peephole_after_exchange(flags_proj);
}

/**
 * AMD Athlon works faster when RET is not destination of
 * conditional jump or directly preceded by other jump instruction.
 * Can be avoided by placing a Rep prefix before the return.
 */
static void peephole_ia32_Return(ir_node *node) {
	ir_node *block, *irn, *rep;

	if (!ia32_cg_config.use_pad_return)
		return;

	block = get_nodes_block(node);

	if (get_Block_n_cfgpreds(block) == 1) {
		ir_node *pred = get_Block_cfgpred(block, 0);

		if (is_Jmp(pred)) {
			/* The block of the return has only one predecessor,
			   which jumps directly to this block.
			   This jump will be encoded as a fall through, so we
			   ignore it here.
			   However, the predecessor might be empty, so it must be
			   ensured that empty blocks are gone away ... */
			return;
		}
	}

	/* check if this return is the first on the block */
	sched_foreach_reverse_from(node, irn) {
		switch (be_get_irn_opcode(irn)) {
		case beo_Return:
			/* the return node itself, ignore */
			continue;
		case beo_Barrier:
			/* ignore the barrier, no code generated */
			continue;
		case beo_IncSP:
			/* arg, IncSP 0 nodes might occur, ignore these */
			if (be_get_IncSP_offset(irn) == 0)
				continue;
			return;
		default:
			if (is_Phi(irn))
				continue;
			return;
		}
	}
	/* yep, return is the first real instruction in this block */
#if 0
	/* add an rep prefix to the return */
	rep = new_rd_ia32_RepPrefix(get_irn_dbg_info(node), current_ir_graph, block);
	keep_alive(rep);
	sched_add_before(node, rep);
#else
	/* ensure, that the 3 byte return is generated */
	be_Return_set_emit_pop(node, 1);
#endif
}

/* only optimize up to 48 stores behind IncSPs */
#define MAXPUSH_OPTIMIZE	48

/**
 * Tries to create pushs from IncSP,Store combinations.
 * The Stores are replaced by Push's, the IncSP is modified
 * (possibly into IncSP 0, but not removed).
 */
static void peephole_IncSP_Store_to_push(ir_node *irn)
{
	int i;
	int offset;
	ir_node *node;
	ir_node *stores[MAXPUSH_OPTIMIZE];
	ir_node *block = get_nodes_block(irn);
	ir_graph *irg = cg->irg;
	ir_node *curr_sp;
	ir_mode *spmode = get_irn_mode(irn);

	memset(stores, 0, sizeof(stores));

	assert(be_is_IncSP(irn));

	offset = be_get_IncSP_offset(irn);
	if (offset < 4)
		return;

	/*
	 * We first walk the schedule after the IncSP node as long as we find
	 * suitable stores that could be transformed to a push.
	 * We save them into the stores array which is sorted by the frame offset/4
	 * attached to the node
	 */
	for(node = sched_next(irn); !sched_is_end(node); node = sched_next(node)) {
		ir_node *mem;
		int offset;
		int storeslot;

		// it has to be a store
		if(!is_ia32_Store(node))
			break;

		// it has to use our sp value
		if(get_irn_n(node, n_ia32_base) != irn)
			continue;
		// store has to be attached to NoMem
		mem = get_irn_n(node, n_ia32_mem);
		if(!is_NoMem(mem)) {
			continue;
		}

		/* unfortunately we can't support the full AMs possible for push at the
		 * moment. TODO: fix this */
		if(get_ia32_am_scale(node) > 0 || !is_ia32_NoReg_GP(get_irn_n(node, n_ia32_index)))
			break;

		offset = get_ia32_am_offs_int(node);

		storeslot = offset / 4;
		if(storeslot >= MAXPUSH_OPTIMIZE)
			continue;

		// storing into the same slot twice is bad (and shouldn't happen...)
		if(stores[storeslot] != NULL)
			break;

		// storing at half-slots is bad
		if(offset % 4 != 0)
			break;

		stores[storeslot] = node;
	}

	curr_sp = be_get_IncSP_pred(irn);

	// walk the stores in inverse order and create pushs for them
	i = (offset / 4) - 1;
	if(i >= MAXPUSH_OPTIMIZE) {
		i = MAXPUSH_OPTIMIZE - 1;
	}

	for( ; i >= 0; --i) {
		const arch_register_t *spreg;
		ir_node *push;
		ir_node *val, *mem, *mem_proj;
		ir_node *store = stores[i];
		ir_node *noreg = ia32_new_NoReg_gp(cg);

		if(store == NULL || is_Bad(store))
			break;

		val = get_irn_n(store, n_ia32_unary_op);
		mem = get_irn_n(store, n_ia32_mem);
		spreg = arch_get_irn_register(cg->arch_env, curr_sp);

		push = new_rd_ia32_Push(get_irn_dbg_info(store), irg, block, noreg, noreg, mem, val, curr_sp);

		sched_add_before(irn, push);

		// create stackpointer proj
		curr_sp = new_r_Proj(irg, block, push, spmode, pn_ia32_Push_stack);
		arch_set_irn_register(cg->arch_env, curr_sp, spreg);

		// create memory proj
		mem_proj = new_r_Proj(irg, block, push, mode_M, pn_ia32_Push_M);

		// use the memproj now
		exchange(store, mem_proj);

		// we can remove the store now
		sched_remove(store);

		offset -= 4;
	}

	be_set_IncSP_offset(irn, offset);
	be_set_IncSP_pred(irn, curr_sp);
}

/**
 * Tries to optimize two following IncSP.
 */
static void peephole_IncSP_IncSP(ir_node *node)
{
	int      pred_offs;
	int      curr_offs;
	int      offs;
	ir_node *pred = be_get_IncSP_pred(node);
	ir_node *predpred;

	if(!be_is_IncSP(pred))
		return;

	if(get_irn_n_edges(pred) > 1)
		return;

	pred_offs = be_get_IncSP_offset(pred);
	curr_offs = be_get_IncSP_offset(node);

	if(pred_offs == BE_STACK_FRAME_SIZE_EXPAND) {
		if(curr_offs != BE_STACK_FRAME_SIZE_SHRINK) {
			return;
		}
		offs = 0;
	} else if(pred_offs == BE_STACK_FRAME_SIZE_SHRINK) {
		if(curr_offs != BE_STACK_FRAME_SIZE_EXPAND) {
			return;
		}
		offs = 0;
	} else if(curr_offs == BE_STACK_FRAME_SIZE_EXPAND
			|| curr_offs == BE_STACK_FRAME_SIZE_SHRINK) {
		return;
	} else {
		offs = curr_offs + pred_offs;
	}

	/* add pred offset to ours and remove pred IncSP */
	be_set_IncSP_offset(node, offs);

	predpred = be_get_IncSP_pred(pred);
	be_peephole_before_exchange(pred, predpred);

	/* rewire dependency edges */
	edges_reroute_kind(pred, predpred, EDGE_KIND_DEP, current_ir_graph);
	be_set_IncSP_pred(node, predpred);
	sched_remove(pred);
	be_kill_node(pred);

	be_peephole_after_exchange(predpred);
}

/**
 * Find a free GP register if possible, else return NULL.
 */
static const arch_register_t *get_free_gp_reg(void)
{
	int i;

	for(i = 0; i < N_ia32_gp_REGS; ++i) {
		const arch_register_t *reg = &ia32_gp_regs[i];
		if(arch_register_type_is(reg, ignore))
			continue;

		if(be_peephole_get_value(CLASS_ia32_gp, i) == NULL)
			return &ia32_gp_regs[i];
	}

	return NULL;
}

static void peephole_be_IncSP(ir_node *node)
{
	const arch_register_t *esp = &ia32_gp_regs[REG_ESP];
	const arch_register_t *reg;
	ir_graph              *irg;
	dbg_info              *dbgi;
	ir_node               *block;
	ir_node               *keep;
	ir_node               *val;
	ir_node               *pop, *pop2;
	ir_node               *stack;
	int                    offset;

	/* first optimize incsp->incsp combinations */
	peephole_IncSP_IncSP(node);

	/* transform IncSP->Store combinations to Push where possible */
	peephole_IncSP_Store_to_push(node);

	if (arch_get_irn_register(arch_env, node) != esp)
		return;

	/* replace IncSP -4 by Pop freereg when possible */
	offset = be_get_IncSP_offset(node);
	if (!(offset == -4 && !ia32_cg_config.use_add_esp_4) &&
	    !(offset == -8 && !ia32_cg_config.use_add_esp_8) &&
	    !(offset == +4 && !ia32_cg_config.use_sub_esp_4) &&
	    !(offset == +8 && !ia32_cg_config.use_sub_esp_8))
		return;

	if (offset < 0) {
		/* we need a free register for pop */
		reg = get_free_gp_reg();
		if(reg == NULL)
			return;

		irg   = current_ir_graph;
		dbgi  = get_irn_dbg_info(node);
		block = get_nodes_block(node);
		stack = be_get_IncSP_pred(node);
		pop   = new_rd_ia32_Pop(dbgi, irg, block, new_NoMem(), stack);

		stack = new_r_Proj(irg, block, pop, mode_Iu, pn_ia32_Pop_stack);
		arch_set_irn_register(arch_env, stack, esp);
		val   = new_r_Proj(irg, block, pop, mode_Iu, pn_ia32_Pop_res);
		arch_set_irn_register(arch_env, val, reg);

		sched_add_before(node, pop);

		keep = sched_next(node);
		if (!be_is_Keep(keep)) {
			ir_node *in[1];
			in[0] = val;
			keep = be_new_Keep(&ia32_reg_classes[CLASS_ia32_gp], irg, block, 1, in);
			sched_add_before(node, keep);
		} else {
			be_Keep_add_node(keep, &ia32_reg_classes[CLASS_ia32_gp], val);
		}

		if (offset == -8) {
			pop2  = new_rd_ia32_Pop(dbgi, irg, block, new_NoMem(), stack);

			stack = new_r_Proj(irg, block, pop2, mode_Iu, pn_ia32_Pop_stack);
			arch_set_irn_register(arch_env, stack, esp);
			val   = new_r_Proj(irg, block, pop2, mode_Iu, pn_ia32_Pop_res);
			arch_set_irn_register(arch_env, val, reg);

			sched_add_after(pop, pop2);
			be_Keep_add_node(keep, &ia32_reg_classes[CLASS_ia32_gp], val);
		}
	} else {
		/* NIY */
		return;
	}

	be_peephole_before_exchange(node, stack);
	sched_remove(node);
	exchange(node, stack);
	be_peephole_after_exchange(stack);
}

/**
 * Peephole optimisation for ia32_Const's
 */
static void peephole_ia32_Const(ir_node *node)
{
	const ia32_immediate_attr_t *attr = get_ia32_immediate_attr_const(node);
	const arch_register_t       *reg;
	ir_graph                    *irg = current_ir_graph;
	ir_node                     *block;
	dbg_info                    *dbgi;
	ir_node                     *produceval;
	ir_node                     *xor;
	ir_node                     *noreg;

	/* try to transform a mov 0, reg to xor reg reg */
	if (attr->offset != 0 || attr->symconst != NULL)
		return;
	if (ia32_cg_config.use_mov_0)
		return;
	/* xor destroys the flags, so no-one must be using them */
	if (be_peephole_get_value(CLASS_ia32_flags, REG_EFLAGS) != NULL)
		return;

	reg = arch_get_irn_register(arch_env, node);
	assert(be_peephole_get_reg_value(reg) == NULL);

	/* create xor(produceval, produceval) */
	block      = get_nodes_block(node);
	dbgi       = get_irn_dbg_info(node);
	produceval = new_rd_ia32_ProduceVal(dbgi, irg, block);
	arch_set_irn_register(arch_env, produceval, reg);

	noreg = ia32_new_NoReg_gp(cg);
	xor   = new_rd_ia32_Xor(dbgi, irg, block, noreg, noreg, new_NoMem(),
	                        produceval, produceval);
	arch_set_irn_register(arch_env, xor, reg);

	sched_add_before(node, produceval);
	sched_add_before(node, xor);

	be_peephole_before_exchange(node, xor);
	exchange(node, xor);
	sched_remove(node);
	be_peephole_after_exchange(xor);
}

static INLINE int is_noreg(ia32_code_gen_t *cg, const ir_node *node)
{
	return node == cg->noreg_gp;
}

static ir_node *create_immediate_from_int(ia32_code_gen_t *cg, int val)
{
	ir_graph *irg         = current_ir_graph;
	ir_node  *start_block = get_irg_start_block(irg);
	ir_node  *immediate   = new_rd_ia32_Immediate(NULL, irg, start_block, NULL,
	                                              0, val);
	arch_set_irn_register(cg->arch_env, immediate, &ia32_gp_regs[REG_GP_NOREG]);

	return immediate;
}

static ir_node *create_immediate_from_am(ia32_code_gen_t *cg,
                                         const ir_node *node)
{
	ir_graph  *irg     = get_irn_irg(node);
	ir_node   *block   = get_nodes_block(node);
	int        offset  = get_ia32_am_offs_int(node);
	int        sc_sign = is_ia32_am_sc_sign(node);
	ir_entity *entity  = get_ia32_am_sc(node);
	ir_node   *res;

	res = new_rd_ia32_Immediate(NULL, irg, block, entity, sc_sign, offset);
	arch_set_irn_register(cg->arch_env, res, &ia32_gp_regs[REG_GP_NOREG]);
	return res;
}

static int is_am_one(const ir_node *node)
{
	int        offset  = get_ia32_am_offs_int(node);
	ir_entity *entity  = get_ia32_am_sc(node);

	return offset == 1 && entity == NULL;
}

static int is_am_minus_one(const ir_node *node)
{
	int        offset  = get_ia32_am_offs_int(node);
	ir_entity *entity  = get_ia32_am_sc(node);

	return offset == -1 && entity == NULL;
}

/**
 * Transforms a LEA into an Add or SHL if possible.
 */
static void peephole_ia32_Lea(ir_node *node)
{
	const arch_env_t      *arch_env = cg->arch_env;
	ir_graph              *irg      = current_ir_graph;
	ir_node               *base;
	ir_node               *index;
	const arch_register_t *base_reg;
	const arch_register_t *index_reg;
	const arch_register_t *out_reg;
	int                    scale;
	int                    has_immediates;
	ir_node               *op1;
	ir_node               *op2;
	dbg_info              *dbgi;
	ir_node               *block;
	ir_node               *res;
	ir_node               *noreg;
	ir_node               *nomem;

	assert(is_ia32_Lea(node));

	/* we can only do this if are allowed to globber the flags */
	if(be_peephole_get_value(CLASS_ia32_flags, REG_EFLAGS) != NULL)
		return;

	base  = get_irn_n(node, n_ia32_Lea_base);
	index = get_irn_n(node, n_ia32_Lea_index);

	if(is_noreg(cg, base)) {
		base     = NULL;
		base_reg = NULL;
	} else {
		base_reg = arch_get_irn_register(arch_env, base);
	}
	if(is_noreg(cg, index)) {
		index     = NULL;
		index_reg = NULL;
	} else {
		index_reg = arch_get_irn_register(arch_env, index);
	}

	if(base == NULL && index == NULL) {
		/* we shouldn't construct these in the first place... */
#ifdef DEBUG_libfirm
		ir_fprintf(stderr, "Optimisation warning: found immediate only lea\n");
#endif
		return;
	}

	out_reg = arch_get_irn_register(arch_env, node);
	scale   = get_ia32_am_scale(node);
	assert(!is_ia32_need_stackent(node) || get_ia32_frame_ent(node) != NULL);
	/* check if we have immediates values (frame entities should already be
	 * expressed in the offsets) */
	if(get_ia32_am_offs_int(node) != 0 || get_ia32_am_sc(node) != NULL) {
		has_immediates = 1;
	} else {
		has_immediates = 0;
	}

	/* we can transform leas where the out register is the same as either the
	 * base or index register back to an Add or Shl */
	if(out_reg == base_reg) {
		if(index == NULL) {
#ifdef DEBUG_libfirm
			if(!has_immediates) {
				ir_fprintf(stderr, "Optimisation warning: found lea which is "
				           "just a copy\n");
			}
#endif
			op1 = base;
			goto make_add_immediate;
		}
		if(scale == 0 && !has_immediates) {
			op1 = base;
			op2 = index;
			goto make_add;
		}
		/* can't create an add */
		return;
	} else if(out_reg == index_reg) {
		if(base == NULL) {
			if(has_immediates && scale == 0) {
				op1 = index;
				goto make_add_immediate;
			} else if(!has_immediates && scale > 0) {
				op1 = index;
				op2 = create_immediate_from_int(cg, scale);
				goto make_shl;
			} else if(!has_immediates) {
#ifdef DEBUG_libfirm
				ir_fprintf(stderr, "Optimisation warning: found lea which is "
				           "just a copy\n");
#endif
			}
		} else if(scale == 0 && !has_immediates) {
			op1 = index;
			op2 = base;
			goto make_add;
		}
		/* can't create an add */
		return;
	} else {
		/* can't create an add */
		return;
	}

make_add_immediate:
	if(ia32_cg_config.use_incdec) {
		if(is_am_one(node)) {
			dbgi  = get_irn_dbg_info(node);
			block = get_nodes_block(node);
			res   = new_rd_ia32_Inc(dbgi, irg, block, op1);
			arch_set_irn_register(arch_env, res, out_reg);
			goto exchange;
		}
		if(is_am_minus_one(node)) {
			dbgi  = get_irn_dbg_info(node);
			block = get_nodes_block(node);
			res   = new_rd_ia32_Dec(dbgi, irg, block, op1);
			arch_set_irn_register(arch_env, res, out_reg);
			goto exchange;
		}
	}
	op2 = create_immediate_from_am(cg, node);

make_add:
	dbgi  = get_irn_dbg_info(node);
	block = get_nodes_block(node);
	noreg = ia32_new_NoReg_gp(cg);
	nomem = new_NoMem();
	res   = new_rd_ia32_Add(dbgi, irg, block, noreg, noreg, nomem, op1, op2);
	arch_set_irn_register(arch_env, res, out_reg);
	set_ia32_commutative(res);
	goto exchange;

make_shl:
	dbgi  = get_irn_dbg_info(node);
	block = get_nodes_block(node);
	noreg = ia32_new_NoReg_gp(cg);
	nomem = new_NoMem();
	res   = new_rd_ia32_Shl(dbgi, irg, block, op1, op2);
	arch_set_irn_register(arch_env, res, out_reg);
	goto exchange;

exchange:
	SET_IA32_ORIG_NODE(res, ia32_get_old_node_name(cg, node));

	/* add new ADD/SHL to schedule */
	DBG_OPT_LEA2ADD(node, res);

	/* exchange the Add and the LEA */
	be_peephole_before_exchange(node, res);
	sched_add_before(node, res);
	sched_remove(node);
	exchange(node, res);
	be_peephole_after_exchange(res);
}

/**
 * Split a Imul mem, imm into a Load mem and Imul reg, imm if possible.
 */
static void peephole_ia32_Imul_split(ir_node *imul) {
	const ir_node         *right = get_irn_n(imul, n_ia32_IMul_right);
	const arch_register_t *reg;
	ir_node               *load, *block, *base, *index, *mem, *res, *noreg;
	dbg_info              *dbgi;
	ir_graph              *irg;

	if (! is_ia32_Immediate(right) || get_ia32_op_type(imul) != ia32_AddrModeS) {
		/* no memory, imm form ignore */
		return;
	}
	/* we need a free register */
	reg = get_free_gp_reg();
	if (reg == NULL)
		return;

	/* fine, we can rebuild it */
	dbgi  = get_irn_dbg_info(imul);
	block = get_nodes_block(imul);
	irg   = current_ir_graph;
	base  = get_irn_n(imul, n_ia32_IMul_base);
	index = get_irn_n(imul, n_ia32_IMul_index);
	mem   = get_irn_n(imul, n_ia32_IMul_mem);
	load = new_rd_ia32_Load(dbgi, irg, block, base, index, mem);

	/* copy all attributes */
	set_irn_pinned(load, get_irn_pinned(imul));
	set_ia32_op_type(load, ia32_AddrModeS);
	set_ia32_ls_mode(load, get_ia32_ls_mode(imul));

	set_ia32_am_scale(load, get_ia32_am_scale(imul));
	set_ia32_am_sc(load, get_ia32_am_sc(imul));
	set_ia32_am_offs_int(load, get_ia32_am_offs_int(imul));
	if (is_ia32_am_sc_sign(imul))
		set_ia32_am_sc_sign(load);
	if (is_ia32_use_frame(imul))
		set_ia32_use_frame(load);
	set_ia32_frame_ent(load, get_ia32_frame_ent(imul));

	sched_add_before(imul, load);

	mem = new_rd_Proj(dbgi, irg, block, load, mode_M, pn_ia32_Load_M);
	res = new_rd_Proj(dbgi, irg, block, load, mode_Iu, pn_ia32_Load_res);

	arch_set_irn_register(arch_env, res, reg);
	be_peephole_after_exchange(res);

	set_irn_n(imul, n_ia32_IMul_mem, mem);
	noreg = get_irn_n(imul, n_ia32_IMul_left);
	set_irn_n(imul, n_ia32_IMul_left, res);
	set_ia32_op_type(imul, ia32_Normal);
}

/**
 * Register a peephole optimisation function.
 */
static void register_peephole_optimisation(ir_op *op, peephole_opt_func func) {
	assert(op->ops.generic == NULL);
	op->ops.generic = (op_func)func;
}

/* Perform peephole-optimizations. */
void ia32_peephole_optimization(ia32_code_gen_t *new_cg)
{
	cg       = new_cg;
	arch_env = cg->arch_env;

	/* register peephole optimisations */
	clear_irp_opcodes_generic_func();
	register_peephole_optimisation(op_ia32_Const, peephole_ia32_Const);
	//register_peephole_optimisation(op_ia32_Store, peephole_ia32_Store);
	register_peephole_optimisation(op_be_IncSP, peephole_be_IncSP);
	register_peephole_optimisation(op_ia32_Lea, peephole_ia32_Lea);
	register_peephole_optimisation(op_ia32_Test, peephole_ia32_Test);
	register_peephole_optimisation(op_ia32_Test8Bit, peephole_ia32_Test);
	register_peephole_optimisation(op_be_Return, peephole_ia32_Return);
	if (! ia32_cg_config.use_imul_mem_imm32)
		register_peephole_optimisation(op_ia32_IMul, peephole_ia32_Imul_split);

	be_peephole_opt(cg->birg);
}

/**
 * Removes node from schedule if it is not used anymore. If irn is a mode_T node
 * all it's Projs are removed as well.
 * @param irn  The irn to be removed from schedule
 */
static INLINE void try_kill(ir_node *node)
{
	if(get_irn_mode(node) == mode_T) {
		const ir_edge_t *edge, *next;
		foreach_out_edge_safe(node, edge, next) {
			ir_node *proj = get_edge_src_irn(edge);
			try_kill(proj);
		}
	}

	if(get_irn_n_edges(node) != 0)
		return;

	if (sched_is_scheduled(node)) {
		sched_remove(node);
	}

	be_kill_node(node);
}

static void optimize_conv_store(ir_node *node)
{
	ir_node *pred;
	ir_node *pred_proj;
	ir_mode *conv_mode;
	ir_mode *store_mode;

	if(!is_ia32_Store(node) && !is_ia32_Store8Bit(node))
		return;

	assert(n_ia32_Store_val == n_ia32_Store8Bit_val);
	pred_proj = get_irn_n(node, n_ia32_Store_val);
	if(is_Proj(pred_proj)) {
		pred = get_Proj_pred(pred_proj);
	} else {
		pred = pred_proj;
	}
	if(!is_ia32_Conv_I2I(pred) && !is_ia32_Conv_I2I8Bit(pred))
		return;
	if(get_ia32_op_type(pred) != ia32_Normal)
		return;

	/* the store only stores the lower bits, so we only need the conv
	 * it it shrinks the mode */
	conv_mode  = get_ia32_ls_mode(pred);
	store_mode = get_ia32_ls_mode(node);
	if(get_mode_size_bits(conv_mode) < get_mode_size_bits(store_mode))
		return;

	set_irn_n(node, n_ia32_Store_val, get_irn_n(pred, n_ia32_Conv_I2I_val));
	if(get_irn_n_edges(pred_proj) == 0) {
		be_kill_node(pred_proj);
		if(pred != pred_proj)
			be_kill_node(pred);
	}
}

static void optimize_load_conv(ir_node *node)
{
	ir_node *pred, *predpred;
	ir_mode *load_mode;
	ir_mode *conv_mode;

	if (!is_ia32_Conv_I2I(node) && !is_ia32_Conv_I2I8Bit(node))
		return;

	assert(n_ia32_Conv_I2I_val == n_ia32_Conv_I2I8Bit_val);
	pred = get_irn_n(node, n_ia32_Conv_I2I_val);
	if(!is_Proj(pred))
		return;

	predpred = get_Proj_pred(pred);
	if(!is_ia32_Load(predpred))
		return;

	/* the load is sign extending the upper bits, so we only need the conv
	 * if it shrinks the mode */
	load_mode = get_ia32_ls_mode(predpred);
	conv_mode = get_ia32_ls_mode(node);
	if(get_mode_size_bits(conv_mode) < get_mode_size_bits(load_mode))
		return;

	if(get_mode_sign(conv_mode) != get_mode_sign(load_mode)) {
		/* change the load if it has only 1 user */
		if(get_irn_n_edges(pred) == 1) {
			ir_mode *newmode;
			if(get_mode_sign(conv_mode)) {
				newmode = find_signed_mode(load_mode);
			} else {
				newmode = find_unsigned_mode(load_mode);
			}
			assert(newmode != NULL);
			set_ia32_ls_mode(predpred, newmode);
		} else {
			/* otherwise we have to keep the conv */
			return;
		}
	}

	/* kill the conv */
	exchange(node, pred);
}

static void optimize_conv_conv(ir_node *node)
{
	ir_node *pred_proj, *pred, *result_conv;
	ir_mode *pred_mode, *conv_mode;
	int      conv_mode_bits;
	int      pred_mode_bits;

	if (!is_ia32_Conv_I2I(node) && !is_ia32_Conv_I2I8Bit(node))
		return;

	assert(n_ia32_Conv_I2I_val == n_ia32_Conv_I2I8Bit_val);
	pred_proj = get_irn_n(node, n_ia32_Conv_I2I_val);
	if(is_Proj(pred_proj))
		pred = get_Proj_pred(pred_proj);
	else
		pred = pred_proj;

	if(!is_ia32_Conv_I2I(pred) && !is_ia32_Conv_I2I8Bit(pred))
		return;

	/* we know that after a conv, the upper bits are sign extended
	 * so we only need the 2nd conv if it shrinks the mode */
	conv_mode      = get_ia32_ls_mode(node);
	conv_mode_bits = get_mode_size_bits(conv_mode);
	pred_mode      = get_ia32_ls_mode(pred);
	pred_mode_bits = get_mode_size_bits(pred_mode);

	if(conv_mode_bits == pred_mode_bits
			&& get_mode_sign(conv_mode) == get_mode_sign(pred_mode)) {
		result_conv = pred_proj;
	} else if(conv_mode_bits <= pred_mode_bits) {
		/* if 2nd conv is smaller then first conv, then we can always take the
		 * 2nd conv */
		if(get_irn_n_edges(pred_proj) == 1) {
			result_conv = pred_proj;
			set_ia32_ls_mode(pred, conv_mode);

			/* Argh:We must change the opcode to 8bit AND copy the register constraints */
			if (get_mode_size_bits(conv_mode) == 8) {
				set_irn_op(pred, op_ia32_Conv_I2I8Bit);
				set_ia32_in_req_all(pred, get_ia32_in_req_all(node));
			}
		} else {
			/* we don't want to end up with 2 loads, so we better do nothing */
			if(get_irn_mode(pred) == mode_T) {
				return;
			}

			result_conv = exact_copy(pred);
			set_ia32_ls_mode(result_conv, conv_mode);

			/* Argh:We must change the opcode to 8bit AND copy the register constraints */
			if (get_mode_size_bits(conv_mode) == 8) {
				set_irn_op(result_conv, op_ia32_Conv_I2I8Bit);
				set_ia32_in_req_all(result_conv, get_ia32_in_req_all(node));
			}
		}
	} else {
		/* if both convs have the same sign, then we can take the smaller one */
		if(get_mode_sign(conv_mode) == get_mode_sign(pred_mode)) {
			result_conv = pred_proj;
		} else {
			/* no optimisation possible if smaller conv is sign-extend */
			if(mode_is_signed(pred_mode)) {
				return;
			}
			/* we can take the smaller conv if it is unsigned */
			result_conv = pred_proj;
		}
	}

	/* kill the conv */
	exchange(node, result_conv);

	if(get_irn_n_edges(pred_proj) == 0) {
		be_kill_node(pred_proj);
		if(pred != pred_proj)
			be_kill_node(pred);
	}
	optimize_conv_conv(result_conv);
}

static void optimize_node(ir_node *node, void *env)
{
	(void) env;

	optimize_load_conv(node);
	optimize_conv_store(node);
	optimize_conv_conv(node);
}

/**
 * Performs conv and address mode optimization.
 */
void ia32_optimize_graph(ia32_code_gen_t *cg)
{
	irg_walk_blkwise_graph(cg->irg, NULL, optimize_node, cg);

	if (cg->dump)
		be_dump(cg->irg, "-opt", dump_ir_block_graph_sched);
}

void ia32_init_optimize(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.ia32.optimize");
}
