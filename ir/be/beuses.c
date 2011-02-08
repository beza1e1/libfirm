/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
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
 * @brief       Methods to compute when a value will be used again.
 * @author      Sebastian Hack, Matthias Braun
 * @date        27.06.2005
 * @version     $Id$
 */
#include "config.h"

#include <limits.h>
#include <stdlib.h>

#include "config.h"
#include "obst.h"
#include "pmap.h"
#include "debug.h"

#include "irgwalk.h"
#include "irnode_t.h"
#include "ircons_t.h"
#include "irgraph_t.h"
#include "iredges_t.h"
#include "irdom_t.h"

#include "be_t.h"
#include "beutil.h"
#include "belive_t.h"
#include "benode.h"
#include "besched.h"
#include "beirgmod.h"
#include "bearch.h"
#include "beuses.h"

#define UNKNOWN_OUTERMOST_LOOP  ((unsigned)-1)

typedef struct be_use_t {
	const ir_node *block;
	const ir_node *node;
	unsigned outermost_loop;
	unsigned next_use;
	ir_visited_t visited;
} be_use_t;

/**
 * The "uses" environment.
 */
struct be_uses_t {
	set *uses;                          /**< cache: contains all computed uses so far. */
	ir_graph *irg;                      /**< the graph for this environment. */
	const be_lv_t *lv;                  /**< the liveness for the graph. */
	ir_visited_t visited_counter;       /**< current search counter. */
	DEBUG_ONLY(firm_dbg_module_t *dbg;  /**< debug module for debug messages. */)
};

/**
 * Set-compare two uses.
 */
static int cmp_use(const void *a, const void *b, size_t n)
{
	const be_use_t *p = (const be_use_t*)a;
	const be_use_t *q = (const be_use_t*)b;
	(void) n;

	return !(p->block == q->block && p->node == q->node);
}

static be_next_use_t get_next_use(be_uses_t *env, ir_node *from,
								  const ir_node *def, int skip_from_uses);

/**
 * Return the use for the given definition in the given block if exists,
 * else create it.
 *
 * @param env    the uses environment
 * @param block  the block we search the use in
 * @param def    the definition of the value we are searching
 */
static const be_use_t *get_or_set_use_block(be_uses_t *env,
                                            const ir_node *block,
                                            const ir_node *def)
{
	unsigned hash = HASH_COMBINE(hash_irn(block), hash_irn(def));
	be_use_t temp;
	be_use_t* result;

	temp.block = block;
	temp.node = def;
	result = (be_use_t*)set_find(env->uses, &temp, sizeof(temp), hash);

	if (result == NULL) {
		// insert templ first as we might end in a loop in the get_next_use
		// call otherwise
		temp.next_use = USES_INFINITY;
		temp.outermost_loop = UNKNOWN_OUTERMOST_LOOP;
		temp.visited = 0;
		result = (be_use_t*)set_insert(env->uses, &temp, sizeof(temp), hash);
	}

	if (result->outermost_loop == UNKNOWN_OUTERMOST_LOOP && result->visited < env->visited_counter) {
		be_next_use_t next_use;

		result->visited = env->visited_counter;
		next_use = get_next_use(env, sched_first(block), def, 0);
		if (next_use.outermost_loop != UNKNOWN_OUTERMOST_LOOP) {
			result->next_use = next_use.time;
			result->outermost_loop = next_use.outermost_loop;
			DBG((env->dbg, LEVEL_5, "Setting nextuse of %+F in block %+F to %u (outermostloop %d)\n",
				def, block, result->next_use, result->outermost_loop));
		}
	}

	return result;
}

/**
 * Check if a value of the given definition is used in the given block
 * as a Phi argument.
 *
 * @param block  the block to check
 * @param def    the definition of the value
 *
 * @return non-zero if the value is used in the given block as a Phi argument
 * in one of its successor blocks.
 */
static int be_is_phi_argument(const ir_node *block, const ir_node *def)
{
	ir_node *node;
	ir_node *succ_block = NULL;
	int arity, i;

#if 1
	if (get_irn_n_edges_kind(block, EDGE_KIND_BLOCK) < 1)
#else
	if (get_irn_n_edges_kind(block, EDGE_KIND_BLOCK) != 1)
#endif
		return 0;

	succ_block = get_first_block_succ(block);

	arity = get_Block_n_cfgpreds(succ_block);
	if (arity <= 1) {
		/* no Phis in the successor */
		return 0;
	}

	/* find the index of block in its successor */
	for (i = 0; i < arity; ++i) {
		if (get_Block_cfgpred_block(succ_block, i) == block)
			break;
	}
	assert(i < arity);

	/* iterate over the Phi nodes in the successor and check if def is
	 * one of its arguments */
	sched_foreach(succ_block, node) {
		ir_node *arg;

		if (!is_Phi(node)) {
			/* found first non-Phi node, we can stop the search here */
			break;
		}

		arg = get_irn_n(node, i);
		if (arg == def)
			return 1;
	}

	return 0;
}

/**
 * Retrieve the scheduled index (the "step") of this node in its
 * block.
 *
 * @param node  the node
 */
static inline unsigned get_step(const ir_node *node)
{
	return (unsigned)PTR_TO_INT(get_irn_link(node));
}

/**
 * Set the scheduled index (the "step") of this node in its
 * block.
 *
 * @param node  the node
 * @param step  the scheduled index of the node
 */
static inline void set_step(ir_node *node, unsigned step)
{
	set_irn_link(node, INT_TO_PTR(step));
}

/**
 * Find the next use of a value defined by def, starting at node from.
 *
 * @param env             the uses environment
 * @param from            the node at which we should start the search
 * @param def             the definition of the value
 * @param skip_from_uses  if non-zero, ignore from uses
 */
static be_next_use_t get_next_use(be_uses_t *env, ir_node *from,
								  const ir_node *def, int skip_from_uses)
{
	unsigned  step;
	ir_node  *block = get_nodes_block(from);
	ir_node  *next_use;
	ir_node  *node;
	unsigned  timestep;
	unsigned  next_use_step;
	const ir_edge_t *edge;

	assert(skip_from_uses == 0 || skip_from_uses == 1);
	if (skip_from_uses) {
		from = sched_next(from);
	}

	next_use      = NULL;
	next_use_step = INT_MAX;
	timestep      = get_step(from);
	foreach_out_edge(def, edge) {
		ir_node  *node = get_edge_src_irn(edge);
		unsigned  node_step;

		if (is_Anchor(node))
			continue;
		if (get_nodes_block(node) != block)
			continue;
		if (is_Phi(node))
			continue;

		node_step = get_step(node);
		if (node_step < timestep)
			continue;
		if (node_step < next_use_step) {
			next_use      = node;
			next_use_step = node_step;
		}
	}

	if (next_use != NULL) {
		be_next_use_t result;
		result.time           = next_use_step - timestep + skip_from_uses;
		result.outermost_loop = get_loop_depth(get_irn_loop(block));
		result.before         = next_use;
		return result;
	}

	node = sched_last(block);
	step = get_step(node) + 1 + timestep + skip_from_uses;

	if (be_is_phi_argument(block, def)) {
		// TODO we really should continue searching the uses of the phi,
		// as a phi isn't a real use that implies a reload (because we could
		// easily spill the whole phi)

		be_next_use_t result;
		result.time           = step;
		result.outermost_loop = get_loop_depth(get_irn_loop(block));
		result.before         = block;
		return result;
	}

	{
	unsigned  next_use   = USES_INFINITY;
	unsigned  outermost_loop;
	be_next_use_t result;
	ir_loop  *loop          = get_irn_loop(block);
	unsigned  loopdepth     = get_loop_depth(loop);
	int       found_visited = 0;
	int       found_use     = 0;
	ir_graph *irg           = get_irn_irg(block);
	ir_node  *startblock    = get_irg_start_block(irg);

	result.before  = NULL;
	outermost_loop = loopdepth;
	foreach_block_succ(block, edge) {
		const be_use_t *use;
		const ir_node *succ_block = get_edge_src_irn(edge);
		ir_loop *succ_loop;
		unsigned use_dist;

		if (succ_block == startblock)
			continue;

		DBG((env->dbg, LEVEL_5, "Checking succ of block %+F: %+F (for use of %+F)\n", block, succ_block, def));
		if (!be_is_live_in(env->lv, succ_block, def)) {
			//next_use = USES_INFINITY;
			DBG((env->dbg, LEVEL_5, "   not live in\n"));
			continue;
		}

		use = get_or_set_use_block(env, succ_block, def);
		DBG((env->dbg, LEVEL_5, "Found %u (loopdepth %u) (we're in block %+F)\n", use->next_use,
					use->outermost_loop, block));
		if (USES_IS_INFINITE(use->next_use)) {
			if (use->outermost_loop == UNKNOWN_OUTERMOST_LOOP) {
				found_visited = 1;
			}
			continue;
		}

		found_use = 1;
		use_dist = use->next_use;

		succ_loop = get_irn_loop(succ_block);
		if (get_loop_depth(succ_loop) < loopdepth) {
			unsigned factor = (loopdepth - get_loop_depth(succ_loop)) * 5000;
			DBG((env->dbg, LEVEL_5, "Increase usestep because of loop out edge %d -> %d (%u)\n", factor));
			// TODO we should use the number of nodes in the loop or so...
			use_dist += factor;
		}

		if (use_dist < next_use) {
			next_use       = use_dist;
			outermost_loop = use->outermost_loop;
			result.before  = use->node;
		}
	}

	if (loopdepth < outermost_loop)
		outermost_loop = loopdepth;

	result.time           = next_use + step;
	result.outermost_loop = outermost_loop;

	if (!found_use && found_visited) {
		// the current result is correct for the current search, but isn't
		// generally correct, so mark it
		result.outermost_loop = UNKNOWN_OUTERMOST_LOOP;
	}
	DBG((env->dbg, LEVEL_5, "Result: %d (outerloop: %u)\n", result.time, result.outermost_loop));
	return result;
	}
}

be_next_use_t be_get_next_use(be_uses_t *env, ir_node *from,
                         const ir_node *def, int skip_from_uses)
{
	++env->visited_counter;
	return get_next_use(env, from, def, skip_from_uses);
}

/**
 * Pre-block walker, set the step number for every scheduled node
 * in increasing order.
 *
 * After this, two scheduled nodes can be easily compared for the
 * "scheduled earlier in block" property.
 */
static void set_sched_step_walker(ir_node *block, void *data)
{
	ir_node  *node;
	unsigned step = 0;
	(void) data;

	sched_foreach(block, node) {
		set_step(node, step);
		if (is_Phi(node))
			continue;
		++step;
	}
}

be_uses_t *be_begin_uses(ir_graph *irg, const be_lv_t *lv)
{
	be_uses_t *env = XMALLOC(be_uses_t);

	edges_assure(irg);

	//set_using_irn_link(irg);

	/* precalculate sched steps */
	irg_block_walk_graph(irg, set_sched_step_walker, NULL, NULL);

	env->uses = new_set(cmp_use, 512);
	env->irg = irg;
	env->lv = lv;
	env->visited_counter = 0;
	FIRM_DBG_REGISTER(env->dbg, "firm.be.uses");

	return env;
}

void be_end_uses(be_uses_t *env)
{
	//clear_using_irn_link(env->irg);
	del_set(env->uses);
	free(env);
}
