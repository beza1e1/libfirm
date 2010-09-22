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
 * @brief   restarting SSA construction for values.
 * @author  Michael Beck
 * @version $Id$
 */
#include "config.h"

#include "ircons_t.h"
#include "irgraph_t.h"
#include "irnode_t.h"
#include "irgwalk.h"

/**
 * Post-walker: prepare the graph nodes for new SSA construction cycle by
 * allocation new arrays.
 */
static void prepare_nodes(ir_node *irn, void *env)
{
	(void)env;

	if (is_Block(irn)) {
		unsigned        n_loc = current_ir_graph->n_loc;
		struct obstack *obst  = current_ir_graph->obst;
		/* reset mature flag */
		irn->attr.block.is_matured = 0;
		irn->attr.block.graph_arr  = NEW_ARR_D(ir_node *, obst, n_loc);
		memset(irn->attr.block.graph_arr, 0, sizeof(ir_node*) * n_loc);
		irn->attr.block.phis       = NULL;
	}
}

/*
 * Restarts SSA construction on the given graph with n_loc
 * new values.
 *
 * @param irg    the graph on which the SSA construction is restarted
 * @param n_loc  number of new variables
 *
 * After this function is complete, the graph is in phase_building
 * again and set_value()/get_value() and mature_block() can be used
 * to construct new values.
 */
void ssa_cons_start(ir_graph *irg, int n_loc)
{
	/* for now we support only phase_high graphs */
	assert(irg->phase_state == phase_high);

	/* reset the phase to phase building: some optimization might depend on it */
	set_irg_phase_state(irg, phase_building);

	irg_set_nloc(irg, n_loc);

	/*
	 * Note: we could try to reuse existing frag arrays, but it does not
	 * seems worth to do this.  First, we have to check if they really exists and
	 * then clear them.  We do not expect SSA construction is used often.
	 */
	irg_walk_graph(irg, NULL, prepare_nodes, NULL);
}

/**
 * mature all immature Blocks.
 */
static void finish_block(ir_node *block, void *env)
{
	(void)env;

	if (!get_Block_matured(block))
		mature_immBlock(block);
}

/*
 * Finalize the (restarted) SSA construction. Matures all blocks that are
 * not matured yet and reset the graph state to phase_high.
 */
void ssa_cons_finish(ir_graph *irg)
{
	irg_block_walk_graph(irg, NULL, finish_block, NULL);
	irg_finalize_cons(irg);
}
