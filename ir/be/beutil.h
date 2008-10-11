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
 * @brief       Contains some useful function for the backend.
 * @author      Sebastian Hack
 * @version     $Id$
 */
#ifndef FIRM_BE_BEUTIL_H
#define FIRM_BE_BEUTIL_H

#include <stdio.h>

#include "firm_types.h"
#include "pset.h"

#include "bearch_t.h"

#ifdef _MSC_VER
typedef          __int64 long64;
typedef unsigned __int64 ulong64;

#define LL_FMT	"i64"
#define ULL_FMT	"ui64"

#else
typedef          long long long64;
typedef unsigned long long ulong64;

#define LL_FMT	"ll"
#define ULL_FMT	"llu"

#endif /* _MSC_VER */

/* iterate over a list of ir_nodes linked by link field */
#define foreach_linked_irns(head, iter) for ((iter) = (head); (iter); (iter) = get_irn_link((iter)))

/**
 * Get an empty set.
 * This function always returns the same set.
 */
pset *be_empty_set(void);

/** Undefine this to disable debugging mode. */
#define BE_DEBUG 1

/**
 * Convenient block getter.
 * Works also, if the given node is a block.
 * @param  irn The node.
 * @return The block of the node, or the node itself, if the node is a
 *         block.
 */
static INLINE ir_node *get_block(ir_node *irn)
{
	return is_Block(irn) ? irn : get_nodes_block(irn);
}

static INLINE const ir_node *get_block_const(const ir_node *irn)
{
	return is_Block(irn) ? irn : get_nodes_block(irn);
}

static INLINE int is_firm_be_mode(const ir_mode *mode)
{
	return mode_is_data(mode);
}

/**
 * Check, if a node produces or consumes a data value.
 * If it does, it is significant for scheduling and register allocation.
 * A node produces/consumes a data value, if one of its operands is of
 * mode datab, or his retuning mode is of mode datab.
 * @param irn The node to check for.
 * @return 1, if the node is a data node, 0 if not.
 */
static INLINE int is_data_node(const ir_node *irn)
{
	int i, n;

	/* If the node produces a data value, return immediately. */
	if (is_firm_be_mode(get_irn_mode(irn)))
		return 1;

	/* else check, if it takes a data value, if that is so, return */
	for(i = 0, n = get_irn_arity(irn); i < n; ++i) {
		ir_node *op = get_irn_n(irn, i);
		if (is_firm_be_mode(get_irn_mode(op)))
			return 1;
	}

	/* Else the node does not produce/consume a data value */
	return 0;
}

/**
 * Dump a vcg graph containing the controlflow graph, the schedule and
 * allocated registers.
 * @param irg The irg. Note that scheduling, register allocation must
 * have been performed.
 */
void dump_allocated_irg(ir_graph *irg, char *suffix);

void be_clear_links(ir_graph *irg);

/**
 * Dump a graph with schedule edges.
 * @param irg The graph.
 * @param suffix A suffix to its file name.
 */
void dump_ir_block_graph_sched(ir_graph *irg, const char *suffix);

/**
 * Dump a extended block graph with schedule edges.
 * @param irg The graph.
 * @param suffix A suffix to its file name.
 */
void dump_ir_extblock_graph_sched(ir_graph *irg, const char *suffix);

/**
 * Dumps a graph and numbers all dumps.
 * @param irg    The graph
 * @param suffix A suffix to its file name.
 * @param dumper The dump function
 */
void be_dump(ir_graph *irg, const char *suffix, void (*dumper)(ir_graph *, const char *));

/**
 * Returns the number of reachable nodes in an irg.
 * @param irg The irg.
 * @return The number of reachable nodes.
 */
unsigned get_num_reachable_nodes(ir_graph *irg);

/**
 * Gets the Proj with number pn from irn.
 */
ir_node *be_get_Proj_for_pn(const ir_node *irn, long pn);

/**
 * Opens a file named base.ext with the mode mode.
 */
FILE *be_ffopen(const char *base, const char *ext, const char *mode);

#endif /* FIRM_BE_BEUTIL_H */
