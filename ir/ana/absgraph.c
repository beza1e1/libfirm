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
 * @file    absgraph.c
 * @author  Sebastian Hack
 * @date    20.04.2007
 * @version $Id$
 * @brief
 *
 * Abstract graph implementations for the CFG of a ir_graph.
 */
#include <config.h>

#include "irgraph_t.h"
#include "iredges_t.h"
#include "absgraph.h"

static void *irg_cfg_succ_get_root(void *self)
{
	ir_graph *irg = (ir_graph*) self;
	edges_assure_kind(irg, EDGE_KIND_BLOCK);
	return get_irg_start_block(irg);
}

static void *irg_cfg_succ_get_end(void *self)
{
	ir_graph *irg = (ir_graph*) self;
	return get_irg_end_block(irg);
}

static void irg_cfg_succ_grow_succs(void *self, void *node, struct obstack *obst)
{
	ir_node *bl = (ir_node*) node;
	const ir_edge_t *edge;

	(void) self;
	foreach_block_succ(bl, edge) {
		obstack_ptr_grow(obst, get_edge_src_irn(edge));
	}
}

const absgraph_t absgraph_irg_cfg_succ = {
	irg_cfg_succ_get_root,
	irg_cfg_succ_grow_succs,
	irg_cfg_succ_get_end
};

static void *irg_cfg_pred_get_root(void *self)
{
	return get_irg_end_block((ir_graph*) self);
}

static void *irg_cfg_pred_get_end(void *self)
{
	return get_irg_start_block((ir_graph*) self);
}

static void irg_cfg_pred_grow_succs(void *self, void *node, struct obstack *obst)
{
	int i, n;

	(void) self;
	for (i = 0, n = get_irn_arity((ir_node*) node); i < n; ++i) {
		obstack_ptr_grow(obst, get_irn_n((ir_node*) node, i));
	}
}

const absgraph_t absgraph_irg_cfg_pred = {
	irg_cfg_pred_get_root,
	irg_cfg_pred_grow_succs,
	irg_cfg_pred_get_end
};
