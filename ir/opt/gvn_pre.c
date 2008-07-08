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
 * @brief   Global Value Numbering Partial Redundancy Elimination
 *          (VanDrunen Hosking 2004)
 * @author  Michael Beck
 * @version $Id$
 * @summary
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "irflag.h"
#include "irdom.h"
#include "irouts.h"
#include "irgopt.h"
#include "irgwalk.h"
#include "ircons.h"
#include "irgmod.h"
#include "valueset.h"
#include "irnodemap.h"
#include "irnodeset.h"
#include "debug.h"

#include "irgraph_t.h"
#include "irnode_t.h"
#include "iropt_t.h"

/** Additional info we need for every block. */
typedef struct block_info {
	ir_valueset_t     *exp_gen;   /**< The set of expression per block. */
	ir_valueset_t     *avail_out; /**< The Avail_out set for a block. */
	ir_valueset_t     *antic_in;  /**< The Antic_in set for a block. */
	ir_valueset_t     *new_set;   /**< The set of all new values for a block. */
	ir_node           *avail;     /**< The get_map(avail, block) result. */
	int               not_found;  /**< Non-zero, if avail was not found in this block. */
	struct block_info *next;      /**< Links all entries, so we can recover the sets easily. */
} block_info;

/**
 * A pair of nodes that must be exchanged.
 * We must defer the exchange because our hash-sets cannot
 * find an already replace node else.
 */
typedef struct elim_pair {
	ir_node *old_node;      /**< The old node that will be replaced. */
	ir_node *new_node;      /**< The new node. */
	struct elim_pair *next; /**< Links all entries in a list. */
} elim_pair;

/** The environment for the GVN-PRE algorithm */
typedef struct pre_env {
	struct obstack *obst;   /**< The obstack to allocate on. */
	ir_node *start_block;   /**< The start block of the current graph. */
	ir_node *end_block;     /**< The end block of the current graph */
	block_info *list;       /**< Links all block info entires for easier recovery. */
	elim_pair *pairs;       /**< A list of node pairs that must be eliminated. */
	char changes;           /**< Non-zero, if calculation of Antic_in has changed. */
	char first_iter;        /**< non-zero for first iteration */
} pre_env;

static pset         *value_table;
static ir_nodemap_t value_map;

/** The debug module handle. */
DEBUG_ONLY(static firm_dbg_module_t *dbg;)

/* ----------  Functions for Value sets ---------- */

/** computes dst = dst \/ src for value sets */
static void value_union(ir_valueset_t *dst, ir_valueset_t *src)
{
	ir_valueset_iterator_t iter;
	ir_node *value, *expr;

	foreach_valueset(src, value, expr, iter) {
		ir_valueset_insert(dst, value, expr);
	}
}


/* ----------  Functions for Values ---------- */

/**
 * Add a node node representing the value value to the set.
 */
static ir_node *add(ir_node *e, ir_node *v)
{
	v = identify_remember(value_table, v);
	ir_nodemap_insert(&value_map, e, v);
	return v;
}

/**
 * Lookup a value in a value set.
 */
static ir_node *lookup(ir_node *e)
{
	ir_node *value = ir_nodemap_get(&value_map, e);
	if (value != NULL)
		return identify_remember(value_table, value);
	return NULL;
}

/**
 * Add or replace a value in a set by an node computing the same
 * value in a dominator block.
 */
static ir_node *lookup_or_add(ir_node *e)
{
	ir_node *x = lookup(e);

	if (x == NULL) {
		x = add(e, e);
	}
	return x;
}


/**
 * Return the block info of a block
 */
static block_info *get_block_info(ir_node *block) {
	return get_irn_link(block);
}

/**
 * allocate a block info
 */
static void alloc_blk_info(ir_node *block, pre_env *env) {
	block_info *info = obstack_alloc(env->obst, sizeof(*info));

	set_irn_link(block, info);
	info->exp_gen   = ir_valueset_new(16);
	info->avail_out = ir_valueset_new(16);
	info->antic_in  = ir_valueset_new(16);
	info->new_set   = NULL;
	info->avail     = NULL;
	info->not_found = 0;
	info->next      = env->list;
	env->list       = info;
}

/**
 * Returns non-zero if a node is movable and a possible candidate for PRE.
 */
static int is_nice_value(ir_node *n) {
	ir_mode *mode;

	while (is_Proj(n))
		n = get_Proj_pred(n);
	mode = get_irn_mode(n);
	/*
	 * FIXME: For now, we cannot handle Div/even if it's movable.
	 * That should be fixed.
	 */
	if (!mode_is_data(mode))
		return 0;
	return (get_irn_pinned(n) != op_pin_state_pinned);
}  /* is_nice_value */

#ifdef DEBUG_libfirm
/**
 * Dump a ir_nodeset_t set.
 */
static void dump_node_set(ir_nodeset_t *set, char *txt, ir_node *block)
{
	ir_nodeset_iterator_t iter;
	ir_node *n;
	int i;

	DB((dbg, LEVEL_2, "%s(%+F) = {\n", txt, block));
	i = 0;
	foreach_ir_nodeset(set, n, iter) {
		if ((i & 3) == 3)
			DB((dbg, LEVEL_2, "\n"));
		DB((dbg, LEVEL_2, " %+F,", n));
		++i;
	}
	DB((dbg, LEVEL_2, "\n}\n"));
}  /* dump_node_set */

/**
 * Dump a value set.
 */
static void dump_value_set(ir_valueset_t *set, char *txt, ir_node *block) {
	ir_valueset_iterator_t iter;
	ir_node *value, *expr;
	int i;

	DB((dbg, LEVEL_2, "%s(%+F) = {\n", txt, block));
	i = 0;
	foreach_valueset(set, value, expr, iter) {
		if ((i & 3) == 3)
			DB((dbg, LEVEL_2, "\n"));
		if (value != expr)
			DB((dbg, LEVEL_2, " %+F(%+F),", expr, value));
		else
			DB((dbg, LEVEL_2, " %+F,", expr));
		++i;
	}
	DB((dbg, LEVEL_2, "\n}\n"));
}  /* dump_value_set */


#else
#define dump_node_set(set, txt, block)
#define dump_value_set(set, txt, block)
#endif /* DEBUG_libfirm */

/**
 * Topological walker. Allocates block info for every block and place nodes in topological
 * order into the nodes set.
 */
static void topo_walker(ir_node *irn, void *ctx) {
	pre_env    *env = ctx;
	ir_node    *block;
	block_info *info;
	ir_node    *value;

	if (is_Block(irn)) {
		/* the topological walker ensures that blocks are visited before anything else */
		alloc_blk_info(irn, env);
		return;
	}
	/* GVN step: remember the value */
	value = add(irn, irn);

	/* no need to put constants into the sets: they are always redundant */
	if (! is_nice_value(irn) || is_irn_constlike(irn))
		return;

	/* place this node into the set of possible nodes of its block */
	block = get_nodes_block(irn);
	info  = get_block_info(block);

	ir_valueset_insert(info->exp_gen, value, irn);
}  /* topo_walker */

/**
 * Computes Avail_out(block):
 *
 * Avail_in(block)  = Avail_out(dom(block))
 * Avail_out(block) = Avail_in(block) \/ Nodes(block)
 *
 * Precondition:
 *  This function must be called in the top-down dominance order:
 *  Then, it computes Leader(Nodes(block)) instead of Nodes(block) !
 */
static void compute_avail_top_down(ir_node *block, void *ctx)
{
	pre_env    *env = ctx;
	block_info *dom_info;
	block_info *info = get_block_info(block);
	ir_node    *dom_blk;

	/* we don't need the end block Avail */
	if (block == env->end_block)
		return;

	/*
	 * First add all nodes from the dominator.
	 * This must be done to ensure that Antic_out contains the leader
	 * for every node. The root has no dominator.
	 */
	if (block != env->start_block) {
		dom_blk = get_Block_idom(block);
		assert(is_Block(dom_blk));

		dom_info = get_block_info(dom_blk);
		assert(dom_info);

		value_union(info->avail_out, dom_info->avail_out);
	}
	value_union(info->avail_out, info->exp_gen);

	dump_value_set(info->avail_out, "Avail_out", block);
}

/**
 * check if a node n is clean in block block.
 */
static int _is_clean(ir_node *n, ir_node *block)
{
	int i;

	if (get_nodes_block(n) != block)
		return 1;
	if (is_Phi(n))
		return 1;

	if (irn_visited(n))
		return 0;

	if (! is_nice_value(n))
		goto bad;
	for (i = get_irn_arity(n) - 1; i >= 0; --i) {
		ir_node *pred = get_irn_n(n, i);
		if (! _is_clean(pred, block))
			goto bad;
	}
	return 1;
bad:
	mark_irn_visited(n);
	return 0;
}

/**
 * check if a node n is clean.
 */
static int is_clean(ir_node *n) {
	int res = _is_clean(n, get_nodes_block(n));
	return res;
}

/**
 * Implements phi_translate.
 */
static ir_node *phi_translate(ir_node *node, ir_node *block, int pos, pre_env *env)
{
	ir_node *nn, *res;
	int i, arity;
	struct obstack *old;

	if (is_Phi(node)) {
		if (get_nodes_block(node) == block) {
			/* a Phi inside our block */
			return get_Phi_pred(node, pos);
		}
		/* already outside */
		return node;
	}

	arity = get_irn_intra_arity(node);

	/* check if the node has at least one Phi predecessor */
	for (i = 0; i < arity; ++i) {
		ir_node *pred    = get_irn_intra_n(node, i);
		ir_node *pred_bl = get_nodes_block(pred);
		ir_node *leader  = lookup(pred);

		leader = leader != NULL ? leader : pred;
		if (is_Phi(leader) && get_nodes_block(pred) == block)
			break;
	}
	if (i >= arity) {
		/* no Phi in the predecessors */
		return node;
	}

	/* Create a copy of the node in the pos'th predecessor block.
	   Use our environmental obstack, as these nodes are always
	   temporary. */
	old = current_ir_graph->obst;
	current_ir_graph->obst = env->obst;
	nn = new_ir_node(
		get_irn_dbg_info(node),
		current_ir_graph,
		NULL,
		get_irn_op(node),
		get_irn_mode(node),
		arity,
		get_irn_in(node));
	/* We need the attribute copy here, because the Hash value of a
	   node might depend on that. */
	copy_node_attr(node, nn);

	set_nodes_block(nn, get_nodes_block(node));
	for (i = 0; i < arity; ++i) {
		ir_node *pred    = get_irn_intra_n(node, i);
		ir_node *pred_bl = get_irn_intra_n(pred, -1);
		ir_node *leader  = lookup(pred);

		leader = leader != NULL ? leader : pred;
		if (is_Phi(leader) && get_irn_intra_n(pred, -1) == block)
			set_irn_n(nn, i, get_Phi_pred(leader, pos));
		else
			set_irn_n(nn, i, leader);
	}
	current_ir_graph->obst = old;
	return nn;
}  /* phi_translate */

/**
 * computes Antic_in(block):
 */
static void compute_antic(ir_node *block, void *ctx) {
	pre_env    *env = ctx;
	block_info *succ_info;
	block_info *info = get_block_info(block);
	ir_node    *succ, *value, *expr;
	size_t     size;
	ir_valueset_iterator_t  iter;

	/* no need for computations in start block */
	if (block == env->start_block)
		return;

	size = ir_valueset_size(info->antic_in);

	/* the end block has no successor */
	if (block != env->end_block) {
		int n_succ = get_Block_n_cfg_outs(block);

		if (n_succ == 1) {
			int i, pos = -1;

			/* find blocks position in succ's block predecessors */
			succ = get_Block_cfg_out(block, 0);
			for (i = get_Block_n_cfgpreds(succ) - 1; i >= 0; --i) {
				if (get_Block_cfgpred_block(succ, i) == block) {
					pos = i;
					break;
				}
			}
			assert(pos >= 0);

			succ_info = get_block_info(succ);
			/* translate into list: we cannot insert into a set we iterate
			 * and succ might be equal to block for endless loops */
			foreach_valueset(succ_info->antic_in, value, expr, iter) {
				ir_node *trans = phi_translate(expr, succ, pos, env);

				if (is_clean(trans))
					ir_valueset_insert(info->antic_in, value, trans);
			}
		} else { /* n_succ > 1 */
			ir_node    *succ0;
			block_info *succ0_info;
			int        i;

			assert(n_succ > 1);

			/*
			 * This step puts all generated expression from the current
			 * current block into Antic_in.
			 * It is enough to do this in the first iteration only, because
			 * the set info->exp_gen is not changed anymore.
			 */
			if (env->first_iter) {
				foreach_valueset(info->exp_gen, value, expr, iter) {
					ir_valueset_insert(info->antic_in, value, expr);
				}
			}

			/* Select a successor to compute the disjoint of all Nodes
			   sets, it might be useful to select the block with the
			   smallest number of nodes.  For simplicity we choose the
			   first one. */
			succ0      = get_Block_cfg_out(block, 0);
			succ0_info = get_block_info(succ0);
			foreach_valueset(succ0_info->antic_in, value, expr, iter) {
				/* we need the disjoint */
				for (i = 1; i < n_succ; ++i) {
					ir_node *succ = get_Block_cfg_out(block, i);
					block_info *succ_info = get_block_info(succ);
					if (ir_valueset_lookup(succ_info->antic_in, value) == NULL)
						break;
				}
				if (i >= n_succ) {
					/* we found a value that is common in all Antic_in(succ(b)),
					    put it in Antic_in(b) if the value is NOT already represented. */
					ir_valueset_insert(info->antic_in, value, expr);
				}
			}
		}
	}

	/* we do not need a clean here, because we ensure that only cleaned nodes are in exp_gen
	 * and all other sets */

	dump_value_set(info->antic_in, "Antic_in", block);
	if (size != ir_valueset_size(info->antic_in)) {
		/* the Antic_in set has changed */
		env->changes |= 1;
	}
}  /* compute_antic */

/**
 * Perform insertion of partially redundant values.
 * For every Block node, do the following:
 * 1.  Propagate the NEW_SETS of the dominator into the current block.
 * If the block has multiple predecessors,
 *     2a. Iterate over the ANTIC expressions for the block to see if
 *         any of them are partially redundant.
 *     2b. If so, insert them into the necessary predecessors to make
 *         the expression fully redundant.
 *     2c. Insert a new Phi merging the values of the predecessors.
 *     2d. Insert the new Phi, and the new expressions, into the
 *         NEW_SETS set.
 */
static void insert_nodes(ir_node *block, void *ctx)
{
	pre_env    *env = ctx;
	ir_node    *value, *expr, *idom, *first_s, *worklist;
	block_info *curr_info, *idom_info;
	int        pos, arity = get_irn_intra_arity(block);
	int        all_same, by_some, updated;
	ir_valueset_iterator_t iter;

	/* ensure that even the start block has a new_set */
	curr_info = get_block_info(block);
	if (curr_info->new_set)
		ir_valueset_del(curr_info->new_set);
	curr_info->new_set = ir_valueset_new(16);

	if (block == env->start_block)
		return;

	idom      = get_Block_idom(block);
	idom_info = get_block_info(idom);

	/* update the new_sets */
	updated = 0;
	dump_value_set(idom_info->new_set, "[New Set]", idom);
	foreach_valueset(idom_info->new_set, value, expr, iter) {
		ir_valueset_insert(curr_info->new_set, value, expr);
		updated |= ir_valueset_replace(curr_info->avail_out, value, expr);
	}
	if (updated) {
		dump_value_set(curr_info->avail_out, "Updated [Avail_out]", block);
	}

	if (arity <= 1)
		return;

	/* convert the set into a list. This allows the removal of
	 * elements from the set */
	worklist = NULL;
	foreach_valueset(curr_info->antic_in, value, expr, iter) {
		ir_mode *mode;

		/* If the value was already computed in the dominator, then
		   it is totally redundant.  Hence we have nothing to insert. */
		if (ir_valueset_lookup(idom_info->avail_out, value)) {
			//      DB((dbg, LEVEL_2, "Found %+F from block %+F avail in dom %+F\n", v, block, idom));
			continue;
		}

		by_some  = 0;
		all_same = 1;
		first_s  = NULL;
		mode     = NULL;

		/* for all predecessor blocks */
		for (pos = 0; pos < arity; ++pos) {
			block_info *pred_info;
			ir_node *pred_blk = get_Block_cfgpred_block(block, pos);
			ir_node *e_prime, *v_prime, *e_dprime;

			/* ignore bad blocks. */
			if (is_Bad(pred_blk))
				continue;

			e_prime = phi_translate(expr, block, pos, env);
			v_prime = lookup(e_prime);
			if (v_prime == NULL)
				v_prime = value;

			pred_info = get_block_info(pred_blk);
			e_dprime  = ir_valueset_lookup(pred_info->avail_out, v_prime);

			if (e_dprime == NULL) {
				pred_info->avail     = e_prime;
				pred_info->not_found = 1;
				all_same = 0;
			} else {
				pred_info->avail     = e_dprime;
				pred_info->not_found = 0;
				mode     = get_irn_mode(e_dprime);
				e_dprime = e_dprime;
				by_some  = 1;
				if (first_s == NULL)
					first_s = e_dprime;
				else if (first_s != e_dprime)
					all_same = 0;

				DB((dbg, LEVEL_2, "Found %+F from block %+F as %+F in pred %+F\n", expr, block, e_dprime, pred_blk));
			}  /* if */
		}  /* for */

		/* If it's not the same value already existing along every predecessor, and
		   it's defined by some predecessor, it is partially redundant. */
		if (! all_same && by_some) {
			ir_node *phi, **in;

			DB((dbg, LEVEL_1, "Partial redundant %+F from block %+F found\n", expr, block));

			in = xmalloc(arity * sizeof(*in));
			/* for all predecessor blocks */
			for (pos = 0; pos < arity; ++pos) {
				ir_node *pred_blk = get_Block_cfgpred_block(block, pos);
				block_info *pred_info = get_block_info(pred_blk);

				/* ignore bad blocks. */
				if (is_Bad(pred_blk)) {
					in[pos] = new_Bad();
					continue;
				}

				/* ignore blocks that already have the expression */
				if (pred_info->not_found) {
					ir_node *e_prime = pred_info->avail;
					ir_node *nn;
					if (!is_Phi(e_prime)) {
						mode = get_irn_mode(e_prime);
						nn = new_ir_node(
							get_irn_dbg_info(e_prime),
							current_ir_graph, pred_blk,
							get_irn_op(e_prime),
							mode,
							get_irn_arity(e_prime),
							get_irn_in(e_prime) + 1);
						copy_node_attr(e_prime, nn);

						DB((dbg, LEVEL_2, "New node %+F in block %+F created\n", nn, pred_blk));
						ir_valueset_insert(pred_info->avail_out, add(nn, lookup(expr)), nn);
						pred_info->avail = nn;
					}
				}
				in[pos] = pred_info->avail;
			}  /* for */
			phi = new_r_Phi(current_ir_graph, block, arity, in, mode);
			value = add(phi, lookup(expr));
			ir_valueset_replace(curr_info->avail_out, value, phi);
			ir_valueset_insert(curr_info->new_set, value, phi);
			free(in);
			DB((dbg, LEVEL_2, "New %+F for redundant %+F created\n", phi, expr));
			env->changes |= 1;
		}  /* if */
  }  /* node_set_foreach */
}  /* insert_nodes */

/**
 * Walker, change nodes by its value if different.
 *
 * We cannot do the changes right here, as this would change
 * the hash values of the nodes in the avail_out set!
 */
static void eliminate(ir_node *irn, void *ctx) {
	pre_env *env = ctx;

	if (is_no_Block(irn)) {
		ir_node *block = get_nodes_block(irn);
		block_info *bl = get_block_info(block);
		ir_node *value = lookup(irn);

		if (value != NULL) {
			ir_node *expr = ir_valueset_lookup(bl->avail_out, value);

			if (expr != NULL && expr != irn) {
				elim_pair *p = obstack_alloc(env->obst, sizeof(*p));

				p->old_node = irn;
				p->new_node = expr;
				p->next     = env->pairs;
				env->pairs  = p;
			}
		}
	}
}  /* eliminate */

/**
 * Do all the recorded changes and optimize
 * newly created Phi's.
 */
static void eliminate_nodes(elim_pair *pairs) {
	elim_pair *p;

	for (p = pairs; p != NULL; p = p->next) {
		DB((dbg, LEVEL_2, "Replacing %+F by %+F\n", p->old_node, p->new_node));
		/*
		 * PRE tends to create Phi(self, self, ... , x, self, self, ...)
		 * which we can optimize here
		 */
		if (is_Phi(p->new_node)) {
			int i;
			ir_node *res = NULL;

			for (i = get_irn_intra_arity(p->new_node) - 1; i >= 0; --i) {
				ir_node *pred = get_irn_n(p->new_node, i);

				if (pred != p->old_node) {
					if (res) {
						res = NULL;
						break;
					}
					res = pred;
				}
			}
			if (res)
				p->new_node = res;
		}
		exchange(p->old_node, p->new_node);
	}
}  /* eliminate_nodes */

/*
 * Argh: Endless loops cause problems, because the
 * insert algorithm did not terminate. We get translated nodes that
 * references the origin. These nodes are translated again and again...
 *
 * The current fix is to use post-dominance. This simple ignores
 * endless loops, ie we cannot optimize them.
 */
void do_gvn_pre(ir_graph *irg)
{
	struct obstack obst;
	pre_env a_env;
	optimization_state_t state;
	block_info *bl_info;
	unsigned antic_iter, insert_iter;

	/* register a debug mask */
	FIRM_DBG_REGISTER(dbg, "firm.opt.gvn_pre");
	firm_dbg_set_mask(dbg, SET_LEVEL_2);

	/* edges will crash if enabled due to our allocate on other obstack trick */
	edges_deactivate(irg);

	value_table = new_identities();
	ir_nodemap_init(&value_map);

	obstack_init(&obst);
	a_env.obst        = &obst;
	a_env.list        = NULL;
	a_env.start_block = get_irg_start_block(irg);
	a_env.end_block   = get_irg_end_block(irg);
	a_env.pairs       = NULL;

	/* Move Proj's into the same block as their args,
	   else we would assign the result to wrong blocks */
	normalize_proj_nodes(irg);

	/* critical edges MUST be removed */
	remove_critical_cf_edges(irg);

	/* we need dominator for Antic_out calculation */
	assure_doms(irg);
	assure_postdoms(irg);
	/* we get all nodes of a block by following outs */
	assure_irg_outs(irg);

	/*
	 * Switch on GCSE. We need it to correctly compute
	 * the leader of a node by hashing.
	 */
	save_optimization_state(&state);
	set_opt_global_cse(1);

	DB((dbg, LEVEL_1, "Doing GVN-PRE for %e\n", get_irg_entity(irg)));

	/* allocate block info for all blocks */
	irg_walk_blkwise_graph(irg, NULL, topo_walker, &a_env);

	/* compute the available value sets for all blocks */
	dom_tree_walk_irg(irg, compute_avail_top_down, NULL, &a_env);

	/* compute the anticipated value sets for all blocks */
	antic_iter = 0;
	a_env.first_iter = 1;

	/* we use the visited flag to mark non-clean nodes */
	inc_irg_visited(irg);
	do {
		DB((dbg, LEVEL_1, "Antic_in Iteration %d starts ...\n", ++antic_iter));
		a_env.changes = 0;
		//irg_block_walk_graph(irg, compute_antic, NULL, &a_env);
		postdom_tree_walk_irg(irg, compute_antic, NULL, &a_env);
		a_env.first_iter = 0;
		DB((dbg, LEVEL_1, "------------------------\n"));
	} while (a_env.changes != 0);

	/* compute redundant expressions */
	insert_iter = 0;
	do {
		DB((dbg, LEVEL_1, "Insert Iteration %d starts ...\n", ++insert_iter));
		a_env.changes = 0;
		dom_tree_walk_irg(irg, insert_nodes, NULL, &a_env);
		DB((dbg, LEVEL_1, "------------------------\n"));
	} while (a_env.changes != 0);

	/* last step: eliminate nodes */
	irg_walk_graph(irg, NULL, eliminate, &a_env);
	eliminate_nodes(a_env.pairs);

	/* clean up: delete all sets */
	for (bl_info = a_env.list; bl_info != NULL; bl_info = bl_info->next) {
		ir_valueset_del(bl_info->exp_gen);
		ir_valueset_del(bl_info->avail_out);
		ir_valueset_del(bl_info->antic_in);
		if (bl_info->new_set)
			ir_valueset_del(bl_info->new_set);
	}
	del_identities(value_table);
	ir_nodemap_destroy(&value_map);
	obstack_free(&obst, NULL);
	value_table = NULL;

	/* pin the graph again: This is needed due to the use of set_opt_global_cse(1) */
	set_irg_pinned(irg, op_pin_state_pinned);
	restore_optimization_state(&state);

	if (a_env.pairs) {
		set_irg_outs_inconsistent(irg);
		set_irg_loopinfo_inconsistent(irg);

	}
	dump_ir_block_graph(irg, "-gvn");
}  /* do_gvn_pre */
