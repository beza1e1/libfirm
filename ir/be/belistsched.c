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
 * @brief       Primitive list scheduling with different node selectors.
 * @author      Sebastian Hack
 * @date        20.10.2004
 * @version     $Id$
 */
#include "config.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>

#include "benode.h"
#include "be_t.h"

#include "obst.h"
#include "list.h"
#include "iterator.h"

#include "iredges_t.h"
#include "irgwalk.h"
#include "irnode_t.h"
#include "irmode_t.h"
#include "irdump.h"
#include "irprintf_t.h"
#include "array.h"
#include "debug.h"
#include "irtools.h"

#include "bemodule.h"
#include "besched.h"
#include "beutil.h"
#include "belive_t.h"
#include "belistsched.h"
#include "bearch.h"
#include "bestat.h"
#include "beirg.h"

#include "lc_opts.h"
#include "lc_opts_enum.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL);

/**
 * Scheduling environment for the whole graph.
 */
typedef struct sched_env_t {
	unsigned *scheduled;                   /**< bitset of already scheduled nodes */
	const list_sched_selector_t *selector; /**< The node selector. */
	void *selector_env;                    /**< A pointer to give to the selector. */
} sched_env_t;

/**
 * Environment for a block scheduler.
 */
typedef struct block_sched_env_t {
	/** scheduling info per node, copied from the global scheduler object */
	unsigned                    *scheduled;
	/** the set of candidates */
	ir_nodeset_t                 cands;
	ir_node                     *block;     /**< the current block */
	sched_env_t                 *sched_env; /**< the scheduler environment */
	const list_sched_selector_t *selector;
	void                        *selector_block_env;
} block_sched_env_t;

/**
 * Returns non-zero if the node is already scheduled
 */
static bool is_already_scheduled(const sched_env_t *env, ir_node *n)
{
	unsigned idx = get_irn_idx(n);
	return rbitset_is_set(env->scheduled, idx);
}

/**
 * Mark a node as already scheduled
 */
static void set_already_scheduled(sched_env_t *env, ir_node *n)
{
	unsigned idx = get_irn_idx(n);
	rbitset_set(env->scheduled, idx);
}

static void selected(block_sched_env_t *env, ir_node *irn);
static void add_to_sched(block_sched_env_t *env, ir_node *irn);

/**
 * Put a node in the ready set, or make it available immediately if it doesn't
 * need to be scheduled
 */
static void node_ready(block_sched_env_t *env, ir_node *pred, ir_node *irn)
{
	if (is_Proj(irn)
	    || (arch_irn_get_flags(irn) & arch_irn_flags_not_scheduled)) {
		selected(env, irn);
		DB((dbg, LEVEL_3, "\tmaking immediately available: %+F\n", irn));
	} else if (be_is_Keep(irn) || be_is_CopyKeep(irn)) {
		/* Keeps must be scheduled immediately */
		add_to_sched(env, irn);
	} else {
		ir_nodeset_insert(&env->cands, irn);

		/* Notify selector about the ready node. */
		if (env->selector->node_ready)
			env->selector->node_ready(env->selector_block_env, irn, pred);

		DB((dbg, LEVEL_2, "\tmaking ready: %+F\n", irn));
	}
}

/**
 * Try to put a node in the ready set.
 * @param env   The block scheduler environment.
 * @param pred  The previous scheduled node.
 * @param irn   The node to make ready.
 * @return 1, if the node could be made ready, 0 else.
 */
static void try_make_ready(block_sched_env_t *env, ir_node *pred, ir_node *irn)
{
	int i, n;

	/* we schedule one block at a time, so no need to consider users in other
	 * blocks */
	if (is_Block(irn) || get_nodes_block(irn) != env->block)
		return;
	if (is_Phi(irn) || is_End(irn))
		return;
	/* check if all operands are already available */
	n = get_irn_ins_or_deps(irn);
	for (i = 0; i < n; ++i) {
		ir_node *op = get_irn_in_or_dep(irn, i);

		/* If the operand is local to the scheduled block and not yet
		 * scheduled, this nodes cannot be made ready, so exit. */
		if (get_nodes_block(op) == env->block
				&& !is_already_scheduled(env->sched_env, op))
			return;
	}

	node_ready(env, pred, irn);
}

static void selected(block_sched_env_t *env, ir_node *node)
{
	const ir_edge_t *edge;

	/* notify the selector about the finally selected node. */
	if (env->selector->node_selected)
		env->selector->node_selected(env->selector_block_env, node);

    /* Insert the node in the set of all available scheduled nodes. */
    set_already_scheduled(env->sched_env, node);

    /* check users, they might be ready now */
	foreach_out_edge(node, edge) {
		ir_node *user = get_edge_src_irn(edge);
		try_make_ready(env, node, user);
	}
	foreach_out_edge_kind(node, edge, EDGE_KIND_DEP) {
		ir_node *user = get_edge_src_irn(edge);
		try_make_ready(env, node, user);
	}
}

/**
 * Append an instruction to a schedule.
 * @param env The block scheduling environment.
 * @param irn The node to add to the schedule.
 * @return    The given node.
 */
static void add_to_sched(block_sched_env_t *env, ir_node *irn)
{
	assert(! (arch_irn_get_flags(irn) & arch_irn_flags_not_scheduled));

	sched_add_before(env->block, irn);

	DB((dbg, LEVEL_2, "\tschedule %+F\n", irn));

	/* Remove the node from the ready set */
	ir_nodeset_remove(&env->cands, irn);

	selected(env, irn);
}

/**
 * Perform list scheduling on a block.
 *
 * Note, that the caller must compute a linked list of nodes in the block
 * using the link field before calling this function.
 *
 * Also the outs must have been computed.
 *
 * @param block The block node.
 * @param env Scheduling environment.
 */
static void list_sched_block(ir_node *block, void *env_ptr)
{
	sched_env_t *env                      = (sched_env_t*)env_ptr;
	const list_sched_selector_t *selector = env->selector;

	block_sched_env_t be;
	const ir_edge_t *edge;
	ir_nodeset_t *cands = &be.cands;

	/* Initialize the block's list head that will hold the schedule. */
	sched_init_block(block);

	/* Initialize the block scheduling environment */
	be.block     = block;
	be.selector  = selector;
	be.sched_env = env;
	ir_nodeset_init_size(cands, get_irn_n_edges(block));

	DB((dbg, LEVEL_1, "scheduling %+F\n", block));

	if (selector->init_block)
		be.selector_block_env = selector->init_block(env->selector_env, block);

	/* Then one can add all nodes are ready to the set. */
	foreach_out_edge(block, edge) {
		ir_node *irn = get_edge_src_irn(edge);

		if (is_Phi(irn)) {
			/* Phi functions are scheduled immediately, since they only
			 * transfer data flow from the predecessors to this block. */
			add_to_sched(&be, irn);
		} else if (be_is_Start(irn)) {
			/* The start block will be scheduled as the first node */
			add_to_sched(&be, irn);
		} else {
			try_make_ready(&be, NULL, irn);
		}
	}

	/* Iterate over all remaining nodes */
	while (ir_nodeset_size(cands) > 0) {
		ir_node *irn = be.selector->select(be.selector_block_env, cands);
		DB((dbg, LEVEL_2, "\tpicked node %+F\n", irn));

		/* remove the scheduled node from the ready list. */
		ir_nodeset_remove(cands, irn);
		/* Add the node to the schedule. */
		add_to_sched(&be, irn);
	}

	if (selector->finish_block)
		selector->finish_block(be.selector_block_env);
}

/* List schedule a graph. */
void be_list_sched_graph(ir_graph *irg, const list_sched_selector_t *selector)
{
	int num_nodes;
	sched_env_t env;

	/* Matze: This is very slow, we should avoid it to improve backend speed,
	 * we just have to make sure that we have no dangling out-edges at this
	 * point...
	 */
	edges_deactivate(irg);
	edges_activate(irg);

	num_nodes = get_irg_last_idx(irg);

	/* initialize environment for list scheduler */
	memset(&env, 0, sizeof(env));
	env.selector  = selector;
	env.scheduled = rbitset_malloc(num_nodes);

	if (selector->init_graph != NULL)
		env.selector_env = selector->init_graph(irg);

	/* Schedule each single block. */
	irg_block_walk_graph(irg, list_sched_block, NULL, &env);

	if (selector->finish_graph != NULL)
		selector->finish_graph(env.selector_env);

	free(env.scheduled);
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_listsched)
void be_init_listsched(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.sched");
}
