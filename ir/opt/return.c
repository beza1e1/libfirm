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
 * @brief   Normalize returns.
 * @author  Michael Beck
 * @version $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "iroptimize.h"
#include "irgraph_t.h"
#include "ircons_t.h"
#include "irnode_t.h"
#include "irgmod.h"
#include "xmalloc.h"

#define set_bit(n)      (returns[(n) >> 3] |= 1 << ((n) & 7))
#define get_bit(n)      (returns[(n) >> 3] & (1 << ((n) & 7)))

#undef IMAX
#define IMAX(a, b)       ((a) > (b) ? (a) : (b))

/*
 * Normalize the Returns of a graph by creating a new End block
 * with One Return(Phi).
 * This is the preferred input for the if-conversion.
 *
 * In pseudocode, it means:
 *
 * if (a)
 *   return b;
 * else
 *   return c;
 *
 * is transformed into
 *
 * if (a)
 *   res = b;
 * else
 *   res = c;
 * return res;
 */
void normalize_one_return(ir_graph *irg) {
	ir_node *endbl = get_irg_end_block(irg);
	int i, j, k, n, last_idx, n_rets, n_ret_vals = -1;
	unsigned char *returns;
	ir_node **in, **retvals, **endbl_in;

	ir_node *block;

	/* look, if we have more than one return */
	n = get_Block_n_cfgpreds(endbl);
	if (n <= 0) {
		/* The end block has no predecessors, we have an endless
		   loop. In that case, no returns exists. */
		return;
	}

	returns = alloca((n + 7) >> 3);
	memset(returns, 0, (n + 7) >> 3);

	for (n_rets = i = 0; i < n; ++i) {
		ir_node *node = get_Block_cfgpred(endbl, i);

		if (is_Return(node)) {
			++n_rets;

			set_bit(i);

			if (n_ret_vals < 0)
				n_ret_vals = get_irn_arity(node);
		}
	}

	/* there should be at least one Return node in Firm */
	if (n_rets <= 1)
		return;

	in       = alloca(sizeof(*in)       * IMAX(n_rets, n_ret_vals));
	retvals  = alloca(sizeof(*retvals)  * n_rets * n_ret_vals);
	endbl_in = alloca(sizeof(*endbl_in) * n);

	last_idx = 0;
	for (j = i = 0; i < n; ++i) {
		ir_node *ret = get_Block_cfgpred(endbl, i);

		if (get_bit(i)) {
			ir_node *block = get_nodes_block(ret);

			/* create a new Jmp for every Ret and place the in in */
			in[j] = new_r_Jmp(irg, block);

			/* save the return values and shuffle them */
			for (k = 0; k < n_ret_vals; ++k)
				retvals[j + k*n_rets] = get_irn_n(ret, k);

			++j;
		} else
			endbl_in[last_idx++] = ret;
	}

	/* ok, create a new block with all created in's */
	block = new_r_Block(irg, n_rets, in);

	/* now create the Phi nodes */
	for (j = i = 0; i < n_ret_vals; ++i, j += n_rets) {
		int k;
		ir_node *first;
		/* the return values are already shuffled */

		/* Beware: normally the Phi constructor automatically replaces a Phi(a,...a) into a
		   but NOT, if a is Unknown. Here, we known that this case can be optimize also,
		   so do it here */
		first = retvals[j + 0];
		for (k = 1; k < n_rets; ++k) {
			if (retvals[j + k] != first) {
				first = NULL;
				break;
			}
		}
		if (first)
			in[i] = first;
		else
			in[i] = new_r_Phi(irg, block, n_rets, &retvals[j], get_irn_mode(retvals[j]));
	}

	endbl_in[last_idx++] = new_r_Return(irg, block, in[0], n_ret_vals-1, &in[1]);

	set_irn_in(endbl, last_idx, endbl_in);

	/* invalidate analysis information:
	 * a new Block was added, so dominator, outs and loop are inconsistent,
	 * trouts and callee-state should be still valid
	 */
	set_irg_doms_inconsistent(irg);
	set_irg_outs_inconsistent(irg);
	set_irg_extblk_inconsistent(irg);
	set_irg_loopinfo_state(irg, loopinfo_cf_inconsistent);
}

/**
 * check, whether a Ret can be moved on block upwards.
 *
 * In a block with a Return, all live nodes must be linked
 * with the Return, otherwise they are dead (because the Return leaves
 * the graph, so no more users of the other nodes can exists.
 *
 * We can move a Return, if it's predecessors are Phi nodes or
 * comes from another block. In the later case, it is always possible
 * to move the Return one block up, because the predecessor block must
 * dominate the Return block (SSA) and then it dominates the predecessor
 * block of the Return block as well.
 *
 * All predecessors of the Return block must be Jmp's of course, or we
 * cannot move it up, so we check this either.
 */
static int can_move_ret(ir_node *ret) {
	ir_node *retbl = get_nodes_block(ret);
	int i, n = get_irn_arity(ret);

	for (i = 0; i < n; ++i) {
		ir_node *pred = get_irn_n(ret, i);

		if (! is_Phi(pred) && retbl == get_nodes_block(pred)) {
			/* first condition failed, found a non-Phi predecessor
			 * then is in the Return block */
			return 0;
		}
	}

	/* check, that predecessors are Jmps */
	n = get_Block_n_cfgpreds(retbl);
	for (i = 0; i < n; ++i)
		if (get_irn_op(get_Block_cfgpred(retbl, i)) != op_Jmp)
			return 0;

	/* if we have 0 control flow predecessors, we cannot move :-) */
	return n > 0;
}

/*
 * Normalize the Returns of a graph by moving
 * the Returns upwards as much as possible.
 * This might be preferred for code generation.
 *
 * In pseudocode, it means:
 *
 * if (a)
 *   res = b;
 * else
 *   res = c;
 * return res;
 *
 * is transformed into
 *
 * if (a)
 *   return b;
 * else
 *   return c;
 */
void normalize_n_returns(ir_graph *irg) {
	int i, j, n, n_rets, n_finals, n_ret_vals;
	ir_node *list  = NULL;
	ir_node *final = NULL;
	ir_node **in;
	ir_node *endbl = get_irg_end_block(irg);
	ir_node *end;

	/*
	 * First, link all returns:
	 * These must be predecessors of the endblock.
	 * Place Returns that can be moved on list, all others
	 * on final.
	 */
	n = get_Block_n_cfgpreds(endbl);
	for (n_finals = n_rets = i = 0; i < n; ++i) {
		ir_node *ret = get_Block_cfgpred(endbl, i);

		if (is_Return(ret) && can_move_ret(ret)) {
			/*
			* Ok, all conditions met, we can move this Return, put it
			* on our work list.
			*/
			set_irn_link(ret, list);
			list = ret;
			++n_rets;
		} else {
			/* Put all nodes that are not changed on the final list. */
			set_irn_link(ret, final);
			final = ret;
			++n_finals;
		}
	}

	if (n_rets <= 0)
		return;

	/*
	 * Now move the Returns upwards. We move always one block up (and create n
	 * new Returns), than we check if a newly created Return can be moved even further.
	 * If yes, we simply add it to our work list, else to the final list.
	 */
	end        = get_irg_end(irg);
	n_ret_vals = get_irn_arity(list);
	in         = alloca(sizeof(*in) * n_ret_vals);
	while (list) {
		ir_node *ret   = list;
		ir_node *block = get_nodes_block(ret);
		ir_node *phiM;

		list = get_irn_link(ret);
		--n_rets;

		n = get_Block_n_cfgpreds(block);
		for (i = 0; i < n; ++i) {
			ir_node *jmp = get_Block_cfgpred(block, i);
			ir_node *new_bl, *new_ret;

			if (get_irn_op(jmp) != op_Jmp)
				continue;

			new_bl = get_nodes_block(jmp);

			/* create the in-array for the new Ret */
			for (j = 0; j < n_ret_vals; ++j) {
				ir_node *pred = get_irn_n(ret, j);

				in[j] = (is_Phi(pred) && get_nodes_block(pred) == block) ? get_Phi_pred(pred, i) : pred;
			}

			new_ret = new_r_Return(irg, new_bl, in[0], n_ret_vals - 1, &in[1]);

			if (! is_Bad(new_ret)) {
				/*
				 * The newly created node might be bad, if we
				 * create it in a block with only Bad predecessors.
				 * In that case ignore this block.
				 *
				 * We could even kill the jmp then ...
				 */
				if (can_move_ret(new_ret)) {
					set_irn_link(new_ret, list);
					list = new_ret;
					++n_rets;
				}
				else {
					set_irn_link(new_ret, final);
					final = new_ret;
					++n_finals;
				}
			}

			/* remove the Jmp, we have placed a Return here */
			exchange(jmp, new_r_Bad(irg));
		}

		/*
		 * if the memory of the old Return is a PhiM, remove it
		 * from the keep-alives, or it will keep the block which
		 * will crash the dominator algorithm.
		 */
		phiM = get_Return_mem(ret);
		if (is_Phi(phiM)) {
			n = get_End_n_keepalives(end);
			for (i = 0; i < n; ++i) {
				if (get_End_keepalive(end, i) == phiM) {
					set_End_keepalive(end, i, new_r_Bad(irg));
					break;
				}
			}
		}
	}

	/*
	 * Last step: Create a new endblock, with all nodes on the final
	 * list as predecessors.
	 */
	in = alloca(sizeof(*in) * n_finals);

	for (i = 0; final; ++i, final = get_irn_link(final))
		in[i] = final;

	exchange(endbl, new_r_Block(irg, n_finals, in));

	/* the end block is not automatically skipped, so do it here */
	set_irg_end_block(irg, skip_Id(get_irg_end_block(irg)));

	/* Invalidate analysis information:
	 * Blocks become dead and new Returns were deleted, so dominator, outs and loop are inconsistent,
	 * trouts and callee-state should be still valid
	 */
	set_irg_doms_inconsistent(irg);
	set_irg_extblk_inconsistent(irg);  /* may not be needed */
	set_irg_outs_inconsistent(irg);
	set_irg_loopinfo_state(current_ir_graph, loopinfo_cf_inconsistent);
}
