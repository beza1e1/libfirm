/*
 * Copyright (C) 2011 Karlsruhe Insitutute of Technology.  All right reserved.
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

/*
 * @brief   Perform local opts on all nodes until fix point is reached
 * @author  Andreas Zwinkau
 * @version $Id$
 */
#include "config.h"

#include <assert.h>
#include <stdbool.h>

#include "iroptimize.h"
#include "irnode_t.h"
#include "irgmod.h"
#include "iropt_t.h"
#include "irgwalk.h"
#include "irtools.h"
#include "opt_manage.h"
#include "irdump.h"
#include "debug.h"
#include "adt/pdeq.h"

/**
 * Enqueue all users of a node to a queue.
 * Skips mode_T nodes.
 */
static void enqueue_users(ir_node *n, pdeq *todo)
{
	const ir_edge_t *edge;

	foreach_out_edge(n, edge) {
		ir_node *user = get_edge_src_irn(edge);

		/* enqueue user into todo queue */
		if (get_irn_link(user) == todo)
			return;
		pdeq_putr(todo, user);
		set_irn_link(user, todo);

		if (get_irn_mode(user) == mode_T) {
		/* A mode_T node has Proj's. Because most optimizations
			run on the Proj's we have to enqueue them also. */
			enqueue_users(user, todo);
		}
	}
}


/** Perform local opts on node. */
static void localopt_walker(ir_node *irn, void *env)
{
	pdeq *todo = (pdeq*) env;
	ir_node *optimized = optimize_in_place_2(irn);

	if (optimized != irn) {
		/* Since the irn is optimized, its users might be optimized further,
		 * hence we remember the users in the todo queue. */
		enqueue_users(irn, todo);
		exchange(irn, optimized);
	}
}

/** perform local opts until fix point is reached */
static ir_graph_state_t do_local_opts(ir_graph *irg)
{
	bool changed;
	pdeq *todo = new_pdeq();

	/* exploit irn links to store, whether a node is already in the todo queue */
	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);

	/* perform local opts on every node and fill todo queue with users of changed nodes */
	irg_walk_graph(irg, localopt_walker, NULL, todo);

	changed = !pdeq_empty(todo);

	/* process the todo queue */
	while (!pdeq_empty(todo)) {
		ir_node *n = (ir_node*)pdeq_getl(todo);
		localopt_walker(n, todo);
		set_irn_link(n, NULL);
	}

	/* clean up */
	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);
	del_pdeq(todo);

	if (changed)
		return 0;

	return IR_GRAPH_STATE_NO_UNREACHABLE_BLOCKS | IR_GRAPH_STATE_NO_BAD_BLOCKS | IR_GRAPH_STATE_CONSISTENT_OUT_EDGES;
}

optdesc_t opt_locals = {
	"locals",
	IR_GRAPH_STATE_NO_UNREACHABLE_BLOCKS | IR_GRAPH_STATE_NO_BAD_BLOCKS | IR_GRAPH_STATE_CONSISTENT_OUT_EDGES,
	do_local_opts,
};
