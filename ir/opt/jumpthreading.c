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
 * @brief   Path-Sensitive Jump Threading
 * @date    10. Sep. 2006
 * @author  Christoph Mallon, Matthias Braun
 * @version $Id$
 */
#include "config.h"

#include "iroptimize.h"

#include <assert.h>
#include "array_t.h"
#include "debug.h"
#include "ircons.h"
#include "irgmod.h"
#include "irgopt.h"
#include "irgwalk.h"
#include "irnode.h"
#include "irnode_t.h"
#include "iredges.h"
#include "iredges_t.h"
#include "irtools.h"
#include "irgraph.h"
#include "tv.h"
#include "opt_confirms.h"
#include "iropt_dbg.h"
#include "irpass.h"
#include "vrp.h"

#undef AVOID_PHIB

DEBUG_ONLY(static firm_dbg_module_t *dbg);

/**
 * Add the new predecessor x to node node, which is either a Block or a Phi
 */
static void add_pred(ir_node* node, ir_node* x)
{
	ir_node** ins;
	int n;
	int i;

	assert(is_Block(node) || is_Phi(node));

	n = get_irn_arity(node);
	NEW_ARR_A(ir_node*, ins, n + 1);
	for (i = 0; i < n; i++)
		ins[i] = get_irn_n(node, i);
	ins[n] = x;
	set_irn_in(node, n + 1, ins);
}

static ir_node *ssa_second_def;
static ir_node *ssa_second_def_block;

static ir_node *search_def_and_create_phis(ir_node *block, ir_mode *mode,
                                           int first)
{
	int i;
	int n_cfgpreds;
	ir_graph *irg;
	ir_node *phi;
	ir_node **in;

	/* This is needed because we create bads sometimes */
	if (is_Bad(block))
		return new_Bad();

	/* the other defs can't be marked for cases where a user of the original
	 * value is in the same block as the alternative definition.
	 * In this case we mustn't use the alternative definition.
	 * So we keep a flag that indicated wether we walked at least 1 block
	 * away and may use the alternative definition */
	if (block == ssa_second_def_block && !first) {
		return ssa_second_def;
	}

	/* already processed this block? */
	if (irn_visited(block)) {
		ir_node *value = (ir_node*) get_irn_link(block);
		return value;
	}

	irg = get_irn_irg(block);
	assert(block != get_irg_start_block(irg));

	/* a Block with only 1 predecessor needs no Phi */
	n_cfgpreds = get_Block_n_cfgpreds(block);
	if (n_cfgpreds == 1) {
		ir_node *pred_block = get_Block_cfgpred_block(block, 0);
		ir_node *value      = search_def_and_create_phis(pred_block, mode, 0);

		set_irn_link(block, value);
		mark_irn_visited(block);
		return value;
	}

	/* create a new Phi */
	NEW_ARR_A(ir_node*, in, n_cfgpreds);
	for (i = 0; i < n_cfgpreds; ++i)
		in[i] = new_Unknown(mode);

	phi = new_r_Phi(block, n_cfgpreds, in, mode);
	set_irn_link(block, phi);
	mark_irn_visited(block);

	/* set Phi predecessors */
	for (i = 0; i < n_cfgpreds; ++i) {
		ir_node *pred_block = get_Block_cfgpred_block(block, i);
		ir_node *pred_val   = search_def_and_create_phis(pred_block, mode, 0);

		set_irn_n(phi, i, pred_val);
	}

	return phi;
}

/**
 * Given a set of values this function constructs SSA-form for the users of the
 * first value (the users are determined through the out-edges of the value).
 * Uses the irn_visited flags. Works without using the dominance tree.
 */
static void construct_ssa(ir_node *orig_block, ir_node *orig_val,
                          ir_node *second_block, ir_node *second_val)
{
	ir_graph *irg;
	ir_mode *mode;
	const ir_edge_t *edge;
	const ir_edge_t *next;

	/* no need to do anything */
	if (orig_val == second_val)
		return;

	irg = get_irn_irg(orig_val);
	inc_irg_visited(irg);

	mode = get_irn_mode(orig_val);
	set_irn_link(orig_block, orig_val);
	mark_irn_visited(orig_block);

	ssa_second_def_block = second_block;
	ssa_second_def       = second_val;

	/* Only fix the users of the first, i.e. the original node */
	foreach_out_edge_safe(orig_val, edge, next) {
		ir_node *user = get_edge_src_irn(edge);
		int j = get_edge_src_pos(edge);
		ir_node *user_block = get_nodes_block(user);
		ir_node *newval;

		/* ignore keeps */
		if (is_End(user))
			continue;

		DB((dbg, LEVEL_3, ">>> Fixing user %+F (pred %d == %+F)\n", user, j, get_irn_n(user, j)));

		if (is_Phi(user)) {
			ir_node *pred_block = get_Block_cfgpred_block(user_block, j);
			newval = search_def_and_create_phis(pred_block, mode, 1);
		} else {
			newval = search_def_and_create_phis(user_block, mode, 1);
		}

		/* don't fix newly created Phis from the SSA construction */
		if (newval != user) {
			DB((dbg, LEVEL_4, ">>>> Setting input %d of %+F to %+F\n", j, user, newval));
			set_irn_n(user, j, newval);
		}
	}
}

static void split_critical_edge(ir_node *block, int pos)
{
	ir_graph *irg = get_irn_irg(block);
	ir_node *in[1];
	ir_node *new_block;
	ir_node *new_jmp;

	in[0] = get_Block_cfgpred(block, pos);
	new_block = new_r_Block(irg, 1, in);
	new_jmp = new_r_Jmp(new_block);
	set_Block_cfgpred(block, pos, new_jmp);
}

typedef struct jumpthreading_env_t {
	ir_node       *true_block;
	ir_node       *cmp;        /**< The Compare node that might be partial evaluated */
	pn_Cmp         pnc;        /**< The Compare mode of the Compare node. */
	ir_node       *cnst;
	tarval        *tv;
	ir_visited_t   visited_nr;

	ir_node       *cnst_pred;   /**< the block before the constant */
	int            cnst_pos;    /**< the pos to the constant block (needed to
	                                  kill that edge later) */
} jumpthreading_env_t;

static ir_node *copy_and_fix_node(const jumpthreading_env_t *env,
                                  ir_node *block, ir_node *copy_block, int j,
                                  ir_node *node)
{
	int      i, arity;
	ir_node *copy;

	/* we can evaluate Phis right now, all other nodes get copied */
	if (is_Phi(node)) {
		copy = get_Phi_pred(node, j);
		/* we might have to evaluate a Phi-cascade */
		if (get_irn_visited(copy) >= env->visited_nr) {
			copy = get_irn_link(copy);
		}
	} else {
		copy = exact_copy(node);
		set_nodes_block(copy, copy_block);

		assert(get_irn_mode(copy) != mode_X);

		arity = get_irn_arity(copy);
		for (i = 0; i < arity; ++i) {
			ir_node *pred     = get_irn_n(copy, i);
			ir_node *new_pred;

			if (get_nodes_block(pred) != block)
				continue;

			if (get_irn_visited(pred) >= env->visited_nr) {
				new_pred = get_irn_link(pred);
			} else {
				new_pred = copy_and_fix_node(env, block, copy_block, j, pred);
			}
			DB((dbg, LEVEL_2, ">> Set Pred of %+F to %+F\n", copy, new_pred));
			set_irn_n(copy, i, new_pred);
		}
	}

	set_irn_link(node, copy);
	set_irn_visited(node, env->visited_nr);

	return copy;
}

static void copy_and_fix(const jumpthreading_env_t *env, ir_node *block,
                         ir_node *copy_block, int j)
{
	const ir_edge_t *edge;

	/* Look at all nodes in the cond_block and copy them into pred */
	foreach_out_edge(block, edge) {
		ir_node *node = get_edge_src_irn(edge);
		ir_node *copy;
		ir_mode *mode;

		/* ignore control flow */
		mode = get_irn_mode(node);
		if (mode == mode_X || is_Cond(node))
			continue;
#ifdef AVOID_PHIB
		/* we may not copy mode_b nodes, because this could produce Phi with
		 * mode_bs which can't be handled in all backends. Instead we duplicate
		 * the node and move it to its users */
		if (mode == mode_b) {
			const ir_edge_t *edge, *next;
			ir_node *pred;
			int      pn;

			assert(is_Proj(node));

			pred = get_Proj_pred(node);
			pn   = get_Proj_proj(node);

			foreach_out_edge_safe(node, edge, next) {
				ir_node *cmp_copy;
				ir_node *user       = get_edge_src_irn(edge);
				int pos             = get_edge_src_pos(edge);
				ir_node *user_block = get_nodes_block(user);

				if (user_block == block)
					continue;

				cmp_copy = exact_copy(pred);
				set_nodes_block(cmp_copy, user_block);
				copy = new_r_Proj(current_ir_graph, user_block, cmp_copy, mode_b, pn);
				set_irn_n(user, pos, copy);
			}
			continue;
		}
#endif

		copy = copy_and_fix_node(env, block, copy_block, j, node);

		/* we might hit values in blocks that have already been processed by a
		 * recursive find_phi_with_const() call */
		assert(get_irn_visited(copy) <= env->visited_nr);
		if (get_irn_visited(copy) >= env->visited_nr) {
			ir_node *prev_copy = get_irn_link(copy);
			if (prev_copy != NULL)
				set_irn_link(node, prev_copy);
		}
	}

	/* fix data-flow (and reconstruct SSA if needed) */
	foreach_out_edge(block, edge) {
		ir_node *node = get_edge_src_irn(edge);
		ir_node *copy_node;
		ir_mode *mode;

		mode = get_irn_mode(node);
		if (mode == mode_X || is_Cond(node))
			continue;
#ifdef AVOID_PHIB
		if (mode == mode_b)
			continue;
#endif

		DB((dbg, LEVEL_2, ">> Fixing users of %+F\n", node));

		copy_node = get_irn_link(node);
		construct_ssa(block, node, copy_block, copy_node);
	}
}

/**
 * returns whether the cmp evaluates to true or false, or can't be evaluated!
 * 1: true, 0: false, -1: can't evaluate
 *
 * @param pnc       the compare mode of the Compare
 * @param tv_left   the left tarval
 * @param tv_right  the right tarval
 */
static int eval_cmp_tv(pn_Cmp pnc, tarval *tv_left, tarval *tv_right)
{
	pn_Cmp cmp_result = tarval_cmp(tv_left, tv_right);

	/* does the compare evaluate to true? */
	if (cmp_result == pn_Cmp_False)
		return -1;
	if ((cmp_result & pnc) != cmp_result)
		return 0;

	return 1;
}

/**
 * returns whether the cmp evaluates to true or false according to vrp
 * information , or can't be evaluated!
 * 1: true, 0: false, -1: can't evaluate
 *
 * @param pnc       the compare mode of the Compare
 * @param left   the left node
 * @param right  the right node
 */
static int eval_cmp_vrp(pn_Cmp pnc, ir_node *left, ir_node *right)
{
	pn_Cmp cmp_result = vrp_cmp(left, right);
	/* does the compare evaluate to true? */
	if (cmp_result == pn_Cmp_False) {
		return -1;
	}
	if ((cmp_result & pnc) != cmp_result) {
		if ((cmp_result & pnc) != 0) {
			return -1;
		}
		return 0;
	}
	return 1;
}
/**
 * returns whether the cmp evaluates to true or false, or can't be evaluated!
 * 1: true, 0: false, -1: can't evaluate
 *
 * @param env      the environment
 * @param cand     the candidate node, either a Const or a Confirm
 */
static int eval_cmp(jumpthreading_env_t *env, ir_node *cand)
{
	if (is_Const(cand)) {
		tarval *tv_cand   = get_Const_tarval(cand);
		tarval *tv_cmp    = get_Const_tarval(env->cnst);

		return eval_cmp_tv(env->pnc, tv_cand, tv_cmp);
	} else { /* a Confirm */
		tarval *res = computed_value_Cmp_Confirm(env->cmp, cand, env->cnst, env->pnc);

		if (res == tarval_bad)
			return -1;
		return res == tarval_b_true;
	}
}

/**
 * Check for Const or Confirm with Const.
 */
static int is_Const_or_Confirm(const ir_node *node)
{
	if (is_Confirm(node))
		node = get_Confirm_bound(node);
	return is_Const(node);
}

/**
 * get the tarval of a Const or Confirm with
 */
static tarval *get_Const_or_Confirm_tarval(const ir_node *node)
{
	if (is_Confirm(node)) {
		if (get_Confirm_bound(node))
			node = get_Confirm_bound(node);
	}
	return get_Const_tarval(node);
}

static ir_node *find_const_or_confirm(jumpthreading_env_t *env, ir_node *jump,
                                      ir_node *value)
{
	ir_node *block = get_nodes_block(jump);

	if (irn_visited_else_mark(value))
		return NULL;

	if (is_Const_or_Confirm(value)) {
		if (eval_cmp(env, value) <= 0) {
			return NULL;
		}

		DB((
			dbg, LEVEL_1,
			"> Found jump threading candidate %+F->%+F\n",
			env->true_block, block
		));

		/* adjust true_block to point directly towards our jump */
		add_pred(env->true_block, jump);

		split_critical_edge(env->true_block, 0);

		/* we need a bigger visited nr when going back */
		env->visited_nr++;

		return block;
	}

	if (is_Phi(value)) {
		int i, arity;

		/* the Phi has to be in the same Block as the Jmp */
		if (get_nodes_block(value) != block) {
			return NULL;
		}

		arity = get_irn_arity(value);
		for (i = 0; i < arity; ++i) {
			ir_node *copy_block;
			ir_node *phi_pred = get_Phi_pred(value, i);
			ir_node *cfgpred  = get_Block_cfgpred(block, i);

			copy_block = find_const_or_confirm(env, cfgpred, phi_pred);
			if (copy_block == NULL)
				continue;

			/* copy duplicated nodes in copy_block and fix SSA */
			copy_and_fix(env, block, copy_block, i);

			if (copy_block == get_nodes_block(cfgpred)) {
				env->cnst_pred = block;
				env->cnst_pos  = i;
			}

			/* return now as we can't process more possibilities in 1 run */
			return copy_block;
		}
	}

	return NULL;
}

static ir_node *find_candidate(jumpthreading_env_t *env, ir_node *jump,
                               ir_node *value)
{
	ir_node *block = get_nodes_block(jump);

	if (irn_visited_else_mark(value)) {
		return NULL;
	}

	if (is_Const_or_Confirm(value)) {
		tarval *tv = get_Const_or_Confirm_tarval(value);

		if (tv != env->tv)
			return NULL;

		DB((
			dbg, LEVEL_1,
			"> Found jump threading candidate %+F->%+F\n",
			env->true_block, block
		));

		/* adjust true_block to point directly towards our jump */
		add_pred(env->true_block, jump);

		split_critical_edge(env->true_block, 0);

		/* we need a bigger visited nr when going back */
		env->visited_nr++;

		return block;
	}
	if (is_Phi(value)) {
		int i, arity;

		/* the Phi has to be in the same Block as the Jmp */
		if (get_nodes_block(value) != block)
			return NULL;

		arity = get_irn_arity(value);
		for (i = 0; i < arity; ++i) {
			ir_node *copy_block;
			ir_node *phi_pred = get_Phi_pred(value, i);
			ir_node *cfgpred  = get_Block_cfgpred(block, i);

			copy_block = find_candidate(env, cfgpred, phi_pred);
			if (copy_block == NULL)
				continue;

			/* copy duplicated nodes in copy_block and fix SSA */
			copy_and_fix(env, block, copy_block, i);

			if (copy_block == get_nodes_block(cfgpred)) {
				env->cnst_pred = block;
				env->cnst_pos  = i;
			}

			/* return now as we can't process more possibilities in 1 run */
			return copy_block;
		}
	}
	if (is_Proj(value)) {
		ir_node *left;
		ir_node *right;
		int      pnc;
		ir_node *cmp = get_Proj_pred(value);
		if (!is_Cmp(cmp))
			return NULL;

		left  = get_Cmp_left(cmp);
		right = get_Cmp_right(cmp);
		pnc   = get_Proj_proj(value);

		/* we assume that the constant is on the right side, swap left/right
		 * if needed */
		if (is_Const(left)) {
			ir_node *t = left;
			left       = right;
			right      = t;

			pnc        = get_inversed_pnc(pnc);
		}

		if (!is_Const(right))
			return 0;

		if (get_nodes_block(left) != block) {
			return 0;
		}

		/* negate condition when we're looking for the false block */
		if (env->tv == tarval_b_false) {
			pnc = get_negated_pnc(pnc, get_irn_mode(right));
		}

		/* (recursively) look if a pred of a Phi is a constant or a Confirm */
		env->cmp  = cmp;
		env->pnc  = pnc;
		env->cnst = right;

		return find_const_or_confirm(env, jump, left);
	}

	return NULL;
}

/**
 * Block-walker: searches for the following construct
 *
 *  Const or Phi with constants
 *           |
 *          Cmp
 *           |
 *         Cond
 *          /
 *       ProjX
 *        /
 *     Block
 */
static void thread_jumps(ir_node* block, void* data)
{
	jumpthreading_env_t env;
	int *changed = data;
	ir_node *selector;
	ir_node *projx;
	ir_node *cond;
	ir_node *copy_block;
	int      selector_evaluated;
	const ir_edge_t *edge, *next;
	ir_node *bad;
	size_t   cnst_pos;

	if (get_Block_n_cfgpreds(block) != 1)
		return;

	projx = get_Block_cfgpred(block, 0);
	if (!is_Proj(projx))
		return;
	assert(get_irn_mode(projx) == mode_X);

	cond = get_Proj_pred(projx);
	if (!is_Cond(cond))
		return;

	selector = get_Cond_selector(cond);
	/* TODO handle switch Conds */
	if (get_irn_mode(selector) != mode_b)
		return;

	/* handle cases that can be immediately evaluated */
	selector_evaluated = -1;
	if (is_Proj(selector)) {
		ir_node *cmp = get_Proj_pred(selector);
		if (is_Cmp(cmp)) {
			ir_node *left  = get_Cmp_left(cmp);
			ir_node *right = get_Cmp_right(cmp);
			if (is_Const(left) && is_Const(right)) {
				int     pnc      = get_Proj_proj(selector);
				tarval *tv_left  = get_Const_tarval(left);
				tarval *tv_right = get_Const_tarval(right);

				selector_evaluated = eval_cmp_tv(pnc, tv_left, tv_right);
			}
			if (selector_evaluated < 0) {
				/* This is only the case if the predecessor nodes are not
				 * constant or the comparison could not be evaluated.
				 * Try with VRP information now.
				 */
				int pnc = get_Proj_proj(selector);

				selector_evaluated = eval_cmp_vrp(pnc, left, right);
			}
		}
	} else if (is_Const_or_Confirm(selector)) {
		tarval *tv = get_Const_or_Confirm_tarval(selector);
		if (tv == tarval_b_true) {
			selector_evaluated = 1;
		} else {
			assert(tv == tarval_b_false);
			selector_evaluated = 0;
		}
	}

	env.cnst_pred = NULL;
	if (get_Proj_proj(projx) == pn_Cond_false) {
		env.tv = tarval_b_false;
		if (selector_evaluated >= 0)
			selector_evaluated = !selector_evaluated;
	} else {
		env.tv = tarval_b_true;
	}

	if (selector_evaluated == 0) {
		bad = new_Bad();
		exchange(projx, bad);
		*changed = 1;
		return;
	} else if (selector_evaluated == 1) {
		dbg_info *dbgi = get_irn_dbg_info(selector);
		ir_node  *jmp  = new_rd_Jmp(dbgi, get_nodes_block(projx));
		DBG_OPT_JUMPTHREADING(projx, jmp);
		exchange(projx, jmp);
		*changed = 1;
		return;
	}

	/* (recursively) look if a pred of a Phi is a constant or a Confirm */
	env.true_block = block;
	inc_irg_visited(current_ir_graph);
	env.visited_nr = get_irg_visited(current_ir_graph);

	copy_block = find_candidate(&env, projx, selector);
	if (copy_block == NULL)
		return;

	/* we have to remove the edge towards the pred as the pred now
	 * jumps into the true_block. We also have to shorten Phis
	 * in our block because of this */
	bad      = new_Bad();
	cnst_pos = env.cnst_pos;

	/* shorten Phis */
	foreach_out_edge_safe(env.cnst_pred, edge, next) {
		ir_node *node = get_edge_src_irn(edge);

		if (is_Phi(node))
			set_Phi_pred(node, cnst_pos, bad);
	}

	set_Block_cfgpred(env.cnst_pred, cnst_pos, bad);

	/* the graph is changed now */
	*changed = 1;
}

void opt_jumpthreading(ir_graph* irg)
{
	int changed, rerun;

	FIRM_DBG_REGISTER(dbg, "firm.opt.jumpthreading");

	DB((dbg, LEVEL_1, "===> Performing jumpthreading on %+F\n", irg));

	remove_critical_cf_edges(irg);

	edges_assure(irg);
	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK | IR_RESOURCE_IRN_VISITED);

	changed = 0;
	do {
		rerun = 0;
		irg_block_walk_graph(irg, thread_jumps, NULL, &rerun);
		changed |= rerun;
	} while (rerun);

	ir_free_resources(irg, IR_RESOURCE_IRN_LINK | IR_RESOURCE_IRN_VISITED);

	if (changed) {
		/* control flow changed, some blocks may become dead */
		set_irg_outs_inconsistent(irg);
		set_irg_doms_inconsistent(irg);
		set_irg_extblk_inconsistent(irg);
		set_irg_loopinfo_inconsistent(irg);
		set_irg_entity_usage_state(irg, ir_entity_usage_not_computed);

		/* Dead code might be created. Optimize it away as it is dangerous
		 * to call optimize_df() an dead code. */
		optimize_cf(irg);
	}
}

/* Creates an ir_graph pass for opt_jumpthreading. */
ir_graph_pass_t *opt_jumpthreading_pass(const char *name)
{
	return def_graph_pass(name ? name : "jumpthreading", opt_jumpthreading);
}  /* opt_jumpthreading_pass */
