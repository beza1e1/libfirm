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
 * @brief   Tail-recursion call optimization.
 * @date    08.06.2004
 * @author  Michael Beck
 * @version $Id$
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <assert.h>

#include "debug.h"
#include "iroptimize.h"
#include "scalar_replace.h"
#include "array.h"
#include "irprog_t.h"
#include "irgwalk.h"
#include "irgmod.h"
#include "irop.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "ircons.h"
#include "irflag.h"
#include "trouts.h"
#include "irouts.h"
#include "irhooks.h"
#include "ircons_t.h"
#include "xmalloc.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg);

/**
 * the environment for collecting data
 */
typedef struct _collect_t {
	ir_node *proj_X;      /**< initial exec proj */
	ir_node *block;       /**< old first block */
	int     blk_idx;      /**< cfgpred index of the initial exec in block */
	ir_node *proj_m;      /**< memory from start proj's */
	ir_node *proj_data;   /**< linked list of all parameter access proj's */
} collect_t;

/**
 * walker for collecting data, fills a collect_t environment
 */
static void collect_data(ir_node *node, void *env) {
	collect_t *data = env;
	ir_node *pred;
	ir_op *op;

	switch (get_irn_opcode(node)) {
	case iro_Proj:
		pred = get_Proj_pred(node);

		op = get_irn_op(pred);
		if (op == op_Proj) {
			ir_node *start = get_Proj_pred(pred);

			if (get_irn_op(start) == op_Start) {
				if (get_Proj_proj(pred) == pn_Start_T_args) {
					/* found Proj(ProjT(Start)) */
					set_irn_link(node, data->proj_data);
					data->proj_data = node;
				}
			}
		} else if (op == op_Start) {
			if (get_Proj_proj(node) == pn_Start_X_initial_exec) {
				/* found ProjX(Start) */
				data->proj_X = node;
			}
		}
		break;
	case iro_Block: {
		int i, n_pred = get_Block_n_cfgpreds(node);

		/*
		 * the first block has the initial exec as cfg predecessor
		 */
		if (node != get_irg_start_block(current_ir_graph)) {
			for (i = 0; i < n_pred; ++i) {
				if (get_Block_cfgpred(node, i) == data->proj_X) {
					data->block   = node;
					data->blk_idx = i;
					break;
				}
			}
		}
		break;
	}
	default:
		break;
	}
}

typedef enum tail_rec_variants {
	TR_DIRECT,  /**< direct return value, i.e. return func(). */
	TR_ADD,     /**< additive return value, i.e. return x +/- func() */
	TR_MUL,     /**< multiplicative return value, i.e. return x * func() or return -func() */
	TR_BAD,     /**< any other transformation */
	TR_UNKNOWN  /**< during construction */
} tail_rec_variants;

typedef struct tr_env {
	int               n_tail_calls;  /**< number of tail calls found */
	int               n_ress;        /**< number of return values */
	tail_rec_variants *variants;     /**< return value variants */
	ir_node           *rets;         /**< list of returns that can be transformed */
} tr_env;


/**
 * do the graph reconstruction for tail-recursion elimination
 *
 * @param irg           the graph that will reconstructed
 * @param rets          linked list of all rets
 * @param n_tail_calls  number of tail-recursion calls
 */
static void do_opt_tail_rec(ir_graph *irg, tr_env *env) {
	ir_node *end_block = get_irg_end_block(irg);
	ir_node *block, *jmp, *call, *calls;
	ir_node **in;
	ir_node **phis;
	ir_node ***call_params;
	ir_node *p, *n;
	int i, j, n_params, n_locs;
	collect_t data;
	int rem            = get_optimize();
	ir_entity *ent     = get_irg_entity(irg);
	ir_type *method_tp = get_entity_type(ent);
	ir_graph *old      = current_ir_graph;

	current_ir_graph = irg;

	assert(env->n_tail_calls > 0);

	/* we add new nodes, so the outs are inconsistent */
	set_irg_outs_inconsistent(irg);

	/* we add new blocks and change the control flow */
	set_irg_doms_inconsistent(irg);
	set_irg_extblk_inconsistent(irg);

	/* we add a new loop */
	set_irg_loopinfo_inconsistent(irg);

	/* calls are removed */
	set_trouts_inconsistent();

	/* we must build some new nodes WITHOUT CSE */
	set_optimize(0);

	/* collect needed data */
	data.proj_X    = NULL;
	data.block     = NULL;
	data.blk_idx   = -1;
	data.proj_m    = get_irg_initial_mem(irg);
	data.proj_data = NULL;
	irg_walk_graph(irg, NULL, collect_data, &data);

	/* check number of arguments */
	call = get_irn_link(end_block);
	n_params = get_Call_n_params(call);

	assert(data.proj_X && "Could not find initial exec from Start");
	assert(data.block  && "Could not find first block");
	assert(data.proj_m && "Could not find initial memory");
	assert((data.proj_data || n_params == 0) && "Could not find Proj(ProjT(Start)) of non-void function");

	/* allocate in's for phi and block construction */
	NEW_ARR_A(ir_node *, in, env->n_tail_calls + 1);

	in[0] = data.proj_X;

	/* turn Return's into Jmp's */
	for (i = 1, p = env->rets; p; p = n) {
		ir_node *block = get_nodes_block(p);

		n = get_irn_link(p);
		in[i++] = new_r_Jmp(irg, block);

		// exchange(p, new_r_Bad(irg));

		/* we might generate an endless loop, so add
		 * the block to the keep-alive list */
		add_End_keepalive(get_irg_end(irg), block);
	}

	/* create a new block at start */
	block = new_r_Block(irg, env->n_tail_calls + 1, in);
	jmp   = new_r_Jmp(irg, block);

	/* the old first block is now the second one */
	set_Block_cfgpred(data.block, data.blk_idx, jmp);

	/* allocate phi's, position 0 contains the memory phi */
	NEW_ARR_A(ir_node *, phis, n_params + 1);

	/* build the memory phi */
	i = 0;
	in[i] = new_r_Proj(irg, get_irg_start_block(irg), get_irg_start(irg), mode_M, pn_Start_M);
	set_irg_initial_mem(irg, in[i]);
	++i;

	for (calls = call; calls; calls = get_irn_link(calls)) {
		in[i] = get_Call_mem(calls);
		++i;
	}
	assert(i == env->n_tail_calls + 1);

	phis[0] = new_r_Phi(irg, block, env->n_tail_calls + 1, in, mode_M);

	/* build the data Phi's */
	if (n_params > 0) {
		ir_node *calls;
		ir_node *args;
		ir_node *args_bl;

		NEW_ARR_A(ir_node **, call_params, env->n_tail_calls);

		/* collect all parameters */
		for (i = 0, calls = call; calls; calls = get_irn_link(calls)) {
			call_params[i] = get_Call_param_arr(calls);
			++i;
		}

		/* build new Proj's and Phi's */
		args    = get_irg_args(irg);
		args_bl = get_nodes_block(args);
		for (i = 0; i < n_params; ++i) {
			ir_mode *mode = get_type_mode(get_method_param_type(method_tp, i));

			in[0] = new_r_Proj(irg, args_bl, args, mode, i);
			for (j = 0; j < env->n_tail_calls; ++j)
				in[j + 1] = call_params[j][i];

			phis[i + 1] = new_r_Phi(irg, block, env->n_tail_calls + 1, in, mode);
		}
	}

	/*
	 * ok, we are here, so we have build and collected all needed Phi's
	 * now exchange all Projs into links to Phi
	 */
	for (p = data.proj_m; p; p = n) {
		n = get_irn_link(p);
		exchange(p, phis[0]);
	}
	for (p = data.proj_data; p; p = n) {
		long proj = get_Proj_proj(p);

		assert(0 <= proj && proj < n_params);
		n = get_irn_link(p);
		exchange(p, phis[proj + 1]);
	}

	/* tail recursion was done, all info is invalid */
	set_irg_doms_inconsistent(irg);
	set_irg_outs_inconsistent(irg);
	set_irg_extblk_inconsistent(irg);
	set_irg_loopinfo_state(current_ir_graph, loopinfo_cf_inconsistent);
	set_trouts_inconsistent();
	set_irg_callee_info_state(irg, irg_callee_info_inconsistent);

	set_optimize(rem);

	/* check if we need new values */
	n_locs = 0;
	for (i = 0; i < env->n_ress; ++i) {
		if (env->variants[i] != TR_DIRECT)
			++n_locs;
	}

	if (n_locs > 0) {
		ir_node *bad, *start_block;
		ir_node **in;
		ir_mode **modes;

		NEW_ARR_A(ir_node *, in, n_locs);
		NEW_ARR_A(ir_mode *, modes, n_locs);
		ssa_cons_start(irg, n_locs);

		start_block = get_irg_start_block(irg);
		set_cur_block(start_block);

		for (i = 0; i < env->n_ress; ++i) {
			ir_type *tp = get_method_res_type(method_tp, i);
			ir_mode *mode = get_type_mode(tp);

			modes[i] = mode;
			if (env->variants[i] == TR_ADD) {
				set_value(i, new_Const(mode, get_mode_null(mode)));
			} else if (env->variants[i] == TR_MUL) {
				set_value(i, new_Const(mode, get_mode_one(mode)));
			}
		}
		mature_immBlock(start_block);

		/* no: we can kill all returns */
		bad = get_irg_bad(irg);

		for (p = env->rets; p; p = n) {
			ir_node *block = get_nodes_block(p);
			ir_node *call, *mem, *jmp, *tuple;

			set_cur_block(block);
			n = get_irn_link(p);

			call = skip_Proj(get_Return_mem(p));
			assert(is_Call(call));

			mem = get_Call_mem(call);

			/* create a new jump, free of CSE */
			set_optimize(0);
			jmp = new_Jmp();
			set_optimize(rem);

			for (i = 0; i < env->n_ress; ++i) {
				if (env->variants[i] != TR_DIRECT) {
					in[i] = get_value(i, modes[i]);
				} else {
					in[i] = bad;
				}
			}
			/* create a new tuple for the return values */
			tuple = new_Tuple(env->n_ress, in);

			turn_into_tuple(call, pn_Call_max);
			set_Tuple_pred(call, pn_Call_M,                mem);
			set_Tuple_pred(call, pn_Call_X_regular,        jmp);
			set_Tuple_pred(call, pn_Call_X_except,         bad);
			set_Tuple_pred(call, pn_Call_T_result,         tuple);
			set_Tuple_pred(call, pn_Call_M_except,         mem);
			set_Tuple_pred(call, pn_Call_P_value_res_base, bad);

			for (i = 0; i < env->n_ress; ++i) {
				ir_node *res = get_Return_res(p, i);
				if (env->variants[i] != TR_DIRECT) {
					set_value(i, res);
				}
			}

			exchange(p, bad);
		}

		/* finally fix all other returns */
		end_block = get_irg_end_block(irg);
		for (i = get_Block_n_cfgpreds(end_block) - 1; i >= 0; --i) {
			ir_node *ret = get_Block_cfgpred(end_block, i);

			/* search all Returns of a block */
			if (! is_Return(ret))
				continue;

			set_cur_block(get_nodes_block(ret));
			for (j = 0; j < env->n_ress; ++j) {
				ir_node *pred = get_Return_res(ret, j);
				ir_node *n;

				switch (env->variants[j]) {
				case TR_DIRECT:
					continue;

				case TR_ADD:
					n = get_value(j, modes[j]);
					n = new_Add(n, pred, modes[j]);
					set_Return_res(ret, j, n);
					break;

				case TR_MUL:
					n = get_value(j, modes[j]);
					n = new_Mul(n, pred, modes[j]);
					set_Return_res(ret, j, n);
					break;

				default:
					assert(!"unexpected tail recursion variant");
				}
			}
		}
		ssa_cons_finish(irg);
	} else {
		ir_node *bad = get_irg_bad(irg);

		/* no: we can kill all returns */
		for (p = env->rets; p; p = n) {
			n = get_irn_link(p);
			exchange(p, bad);
		}
	}
	current_ir_graph = old;
}

/**
 * Check the lifetime of locals in the given graph.
 * Tail recursion can only be done, if we can prove that
 * the lifetime of locals end with the recursive call.
 * We do this by checking that no address of a local variable is
 * stored or transmitted as an argument to a call.
 *
 * @return non-zero if it's ok to do tail recursion
 */
static int check_lifetime_of_locals(ir_graph *irg) {
	ir_node *irg_frame, *irg_val_param_base;
	int i;

	irg_frame = get_irg_frame(irg);
	for (i = get_irn_n_outs(irg_frame) - 1; i >= 0; --i) {
		ir_node *succ = get_irn_out(irg_frame, i);

		if (is_Sel(succ) && is_address_taken(succ))
			return 0;
	}

	/* Check if we have compound arguments.
	   For now, we cannot handle them, */
	irg_val_param_base = get_irg_value_param_base(irg);
	if (get_irn_n_outs(irg_val_param_base) > 0)
		return 0;

	return 1;
}

/**
 * Examine irn and detect the recursion variant.
 */
static tail_rec_variants find_variant(ir_node *irn, ir_node *call) {
	ir_node           *a, *b;
	tail_rec_variants va, vb, res;

	if (skip_Proj(skip_Proj(irn)) == call) {
		/* found it */
		return TR_DIRECT;
	}
	switch (get_irn_opcode(irn)) {
	case iro_Add:
		/* try additive */
		a = get_Add_left(irn);
		if (get_irn_MacroBlock(a) != get_irn_MacroBlock(call)) {
			/* we are outside, ignore */
			va = TR_UNKNOWN;
		} else {
			va = find_variant(a, call);
			if (va == TR_BAD)
				return TR_BAD;
		}
		b = get_Add_right(irn);
		if (get_irn_MacroBlock(b) != get_irn_MacroBlock(call)) {
			/* we are outside, ignore */
			vb = TR_UNKNOWN;
		} else {
			vb = find_variant(b, call);
			if (vb == TR_BAD)
				return TR_BAD;
		}
		if (va == vb) {
			res = va;
		}
		else if (va == TR_UNKNOWN)
			res = vb;
		else if (vb == TR_UNKNOWN)
			res = va;
		else {
			/* they are different but none is TR_UNKNOWN -> incompatible */
			return TR_BAD;
		}
		if (res == TR_DIRECT || res == TR_ADD)
			return TR_ADD;
		/* not compatible */
		return TR_BAD;

	case iro_Sub:
		/* try additive, but return value mut be left */
		a = get_Sub_left(irn);
		if (get_irn_MacroBlock(a) != get_irn_MacroBlock(call)) {
			/* we are outside, ignore */
			va = TR_UNKNOWN;
		} else {
			va = find_variant(a, call);
			if (va == TR_BAD)
				return TR_BAD;
		}
		b = get_Sub_right(irn);
		if (get_irn_MacroBlock(b) != get_irn_MacroBlock(call)) {
			/* we are outside, ignore */
			vb = TR_UNKNOWN;
		} else {
			vb = find_variant(b, call);
			if (vb != TR_UNKNOWN)
				return TR_BAD;
		}
		res = va;
		if (res == TR_DIRECT || res == TR_ADD)
			return res;
		/* not compatible */
		return TR_BAD;

	case iro_Mul:
		/* try multiplicative */
		a = get_Mul_left(irn);
		if (get_irn_MacroBlock(a) != get_irn_MacroBlock(call)) {
			/* we are outside, ignore */
			va = TR_UNKNOWN;
		} else {
			va = find_variant(a, call);
			if (va == TR_BAD)
				return TR_BAD;
		}
		b = get_Mul_right(irn);
		if (get_irn_MacroBlock(b) != get_irn_MacroBlock(call)) {
			/* we are outside, ignore */
			vb = TR_UNKNOWN;
		} else {
			vb = find_variant(b, call);
			if (vb == TR_BAD)
				return TR_BAD;
		}
		if (va == vb) {
			res = va;
		}
		else if (va == TR_UNKNOWN)
			res = vb;
		else if (vb == TR_UNKNOWN)
			res = va;
		else {
			/* they are different but none is TR_UNKNOWN -> incompatible */
			return TR_BAD;
		}
		if (res == TR_DIRECT || res == TR_MUL)
			return TR_MUL;
		/* not compatible */
		return TR_BAD;

	case iro_Minus:
		/* try multiplicative */
		a = get_Minus_op(irn);
		res =  find_variant(a, call);
		if (res == TR_DIRECT)
			return TR_MUL;
		if (res == TR_MUL || res == TR_UNKNOWN)
			return res;
		/* not compatible */
		return TR_BAD;

	default:
		return TR_UNKNOWN;
	}
}


/*
 * convert simple tail-calls into loops
 */
int opt_tail_rec_irg(ir_graph *irg) {
	tr_env            env;
	ir_node           *end_block;
	int               i, n_ress, n_tail_calls = 0;
	ir_node           *rets = NULL;
	ir_type           *mtd_type, *call_type;
	ir_entity         *ent;

	assure_irg_outs(irg);

	if (! check_lifetime_of_locals(irg))
		return 0;


	ent      = get_irg_entity(irg);
	mtd_type = get_entity_type(ent);
    n_ress   = get_method_n_ress(mtd_type);

	env.variants = NULL;
	env.n_ress   = n_ress;

	if (n_ress > 0) {
		NEW_ARR_A(tail_rec_variants, env.variants, n_ress);

		for (i = 0; i < n_ress; ++i)
			env.variants[i] = TR_DIRECT;
	}

	/*
	 * This tail recursion optimization works best
	 * if the Returns are normalized.
	 */
	normalize_n_returns(irg);

	end_block = get_irg_end_block(irg);
	set_irn_link(end_block, NULL);

	for (i = get_Block_n_cfgpreds(end_block) - 1; i >= 0; --i) {
		ir_node *ret = get_Block_cfgpred(end_block, i);
		ir_node *call, *call_ptr;
		int j;
		ir_node **ress;

		/* search all Returns of a block */
		if (! is_Return(ret))
			continue;

		/* check, if it's a Return self() */
		call = skip_Proj(get_Return_mem(ret));
		if (! is_Call(call))
			continue;

		/* the call must be in the same block as the return */
		if (get_nodes_block(call) != get_nodes_block(ret))
			continue;

		/* check if it's a recursive call */
		call_ptr = get_Call_ptr(call);

		if (! is_SymConst_addr_ent(call_ptr))
			continue;

		ent = get_SymConst_entity(call_ptr);
		if (!ent || get_entity_irg(ent) != irg)
			continue;

		/*
		 * Check, that the types match. At least in C
		 * this might fail.
		 */
		mtd_type  = get_entity_type(ent);
		call_type = get_Call_type(call);

		if (mtd_type != call_type) {
			/*
			 * Hmm, the types did not match, bad.
			 * This can happen in C when no prototype is given
			 * or K&R style is used.
			 */
#if 0
			printf("Warning: Tail recursion fails because of different method and call types:\n");
			dump_type(mtd_type);
			dump_type(call_type);
#endif
			continue;
		}

		/* ok, mem is routed to a recursive call, check return args */
		ress = get_Return_res_arr(ret);
		for (j = get_Return_n_ress(ret) - 1; j >= 0; --j) {
			tail_rec_variants var = find_variant(ress[j], call);

			if (var >= TR_BAD) {
				/* cannot be transformed */
				break;
			}
			if (var == TR_DIRECT)
				var = env.variants[j];
			else if (env.variants[j] == TR_DIRECT)
				env.variants[j] = var;
			if (env.variants[j] != var) {
				/* not compatible */
				break;
			}
		}
		if (j >= 0)
			continue;

		/* here, we have found a call */
		set_irn_link(call, get_irn_link(end_block));
		set_irn_link(end_block, call);
		++n_tail_calls;

		/* link all returns, we will need this */
		set_irn_link(ret, rets);
		rets = ret;
	}

	/* now, end_block->link contains the list of all tail calls */
	if (n_tail_calls <= 0)
		return 0;

	DB((dbg, LEVEL_2, "  Performing tail recursion for graph %s and %d Calls\n",
	    get_entity_ld_name(get_irg_entity(irg)), n_tail_calls));

	hook_tail_rec(irg, n_tail_calls);

	env.n_tail_calls = n_tail_calls;
	env.rets         = rets;
	do_opt_tail_rec(irg, &env);

	return n_tail_calls;
}

/*
 * optimize tail recursion away
 */
void opt_tail_recursion(void) {
	int i;
	int n_opt_applications = 0;
	ir_graph *irg;

	FIRM_DBG_REGISTER(dbg, "firm.opt.tailrec");

	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		irg = get_irp_irg(i);

		current_ir_graph = irg;

		if (opt_tail_rec_irg(irg))
			++n_opt_applications;
	}

	DB((dbg, LEVEL_1, "Performed tail recursion for %d of %d graphs\n",
	    n_opt_applications, get_irp_n_irgs()));
}
