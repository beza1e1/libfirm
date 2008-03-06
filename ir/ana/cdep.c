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
 * @brief   Implementation of cdep
 * @author  Christoph Mallon
 * @version $Id$
 */
#include <assert.h>
#include <stdlib.h>
#include "irdom.h"
#include "irgraph.h"
#include "irgwalk.h"
#include "irnode.h"
#include "pmap.h"
#include "obst.h"
#include "xmalloc.h"
#include "cdep.h"
#include "irprintf.h"
#include "irdump.h"

typedef struct cdep_info {
	pmap   *cdep_map;     /**< A map to find the list of all control dependence nodes for a block. */
	struct obstack obst;  /**< An obstack where all cdep data lives on. */
} cdep_info;

static cdep_info *cdep_data;

/* Return a list of all control dependences of a block. */
ir_cdep *find_cdep(const ir_node *block) {
	return pmap_get(cdep_data->cdep_map, (void *)block);
}

/* Replace the control dependence info of old by the info of nw. */
void exchange_cdep(ir_node *old, const ir_node *nw) {
	ir_cdep *cdep = find_cdep(nw);
	pmap_insert(cdep_data->cdep_map, old, cdep);
}

/**
 * Adds a control dependence from node to dep_on.
 */
static void add_cdep(ir_node *node, ir_node *dep_on) {
	ir_cdep *dep = find_cdep(node);
#if 0
	ir_fprintf(stderr, "Adding cdep of %+F on %+F\n", node, dep_on);
#endif

	if (dep == NULL) {
		ir_cdep *newdep = obstack_alloc(&cdep_data->obst, sizeof(*newdep));

		newdep->node = dep_on;
		newdep->next = NULL;
		pmap_insert(cdep_data->cdep_map, node, newdep);
	} else {
		ir_cdep *newdep;

		for (;;) {
			if (dep->node == dep_on) return;
			if (dep->next == NULL) break;
			dep = dep->next;
		}
		newdep = obstack_alloc(&cdep_data->obst, sizeof(*newdep));
		newdep->node = dep_on;
		newdep->next = NULL;
		dep->next = newdep;
	}
}

typedef struct cdep_env {
	ir_node *start_block;
	ir_node *end_block;
} cdep_env;

/**
 * Pre-block-walker: calculate the control dependence
 */
static void cdep_pre(ir_node *node, void *ctx) {
	cdep_env *env = ctx;
	unsigned int n;
	unsigned int i;

	/* special case:
	 * start and end block have no control dependency
	 */
	if (node == env->start_block) return;
	if (node == env->end_block) return;

	n = get_Block_n_cfgpreds(node);
	for (i = 0; i < n; i++) {
		ir_node *pred = get_Block_cfgpred_block(node, i);
		ir_node *pdom;
		ir_node *dependee;

		if (is_Bad(pred)) continue;

		pdom = get_Block_ipostdom(pred);
		for (dependee = node; dependee != pdom; dependee = get_Block_ipostdom(dependee)) {
			assert(!is_Bad(pdom));
			add_cdep(dependee, pred);
		}
	}
}


/**
 * A block edge hook: add all cdep edges of block.
 */
static int cdep_edge_hook(FILE *F, ir_node *block)
{
	ir_cdep *cd;

#if 0
	ir_node *pdom = get_Block_ipostdom(block);
	if (pdom != NULL) {
		fprintf(
			F,
			"edge:{sourcename:\"n%ld\" targetname:\"n%ld\" color:gold}\n",
			get_irn_node_nr(pdom), get_irn_node_nr(block)
		);
	}
#endif

	for (cd = find_cdep(block); cd != NULL; cd = cd->next) {
		fprintf(
			F,
			"edge:{sourcename:\"n%ld\" targetname:\"n%ld\" "
			"linestyle:dashed color:gold}\n",
			get_irn_node_nr(block), get_irn_node_nr(cd->node)
		);
	}

	return 0;
}

/* Compute the control dependence graph for a graph. */
void compute_cdep(ir_graph *irg) {
	ir_node *start_block, *rem;
	cdep_env env;

	free_cdep(irg);
	cdep_data = xmalloc(sizeof(*cdep_data));
	obstack_init(&cdep_data->obst);

	cdep_data->cdep_map = pmap_create();

	assure_postdoms(irg);

	/* we must temporary change the post dominator relation:
	   the ipdom of the startblock is the end block.
	   Firm does NOT add the phantom edge from Start to End.
	 */
	start_block = get_irg_start_block(irg);
	rem = get_Block_ipostdom(start_block);
	set_Block_ipostdom(start_block, get_irg_end_block(irg));

	env.start_block = get_irg_start_block(irg);
	env.end_block   = get_irg_end_block(irg);
	irg_block_walk_graph(irg, cdep_pre, NULL, &env);

#if 0
	set_dump_block_edge_hook(cdep_edge_hook);
	dump_ir_block_graph(irg, "_cdep");
	set_dump_block_edge_hook(NULL);
#else
	(void) cdep_edge_hook;
#endif

	/* restore the post dominator relation */
	set_Block_ipostdom(start_block, rem);
}

/* Free the control dependence info. */
void free_cdep(ir_graph *irg) {
	(void) irg;
	if (cdep_data != NULL) {
		pmap_destroy(cdep_data->cdep_map);
		obstack_free(&cdep_data->obst, NULL);
		xfree(cdep_data);
		cdep_data = NULL;
	}
}

/* Check whether dependee is (directly) control dependent on candidate. */
int is_cdep_on(const ir_node *dependee, const ir_node *candidate) {
	const ir_cdep *dep;

	for (dep = find_cdep(dependee); dep != NULL; dep = dep->next) {
		if (dep->node == candidate) return 1;
	}
	return 0;
}

/* Check whether dependee is (possible iterated) control dependent on candidate. */
int is_iterated_cdep_on(ir_node *dependee, ir_node *candidate) {
	const ir_cdep *dep;

	while ((dep = find_cdep(dependee)) != NULL) {
		if (dep->next != NULL) return 0;
		if (dep->node == candidate) return 1;
		dependee = dep->node;
	}
	return 0;
}

/* If block is control dependent on exactly one node, return this node, else NULL. */
ir_node *get_unique_cdep(const ir_node *block) {
	ir_cdep *cdep = find_cdep(block);

	return cdep != NULL && cdep->next == NULL ? cdep->node : NULL;
}

/* Check if the given block is control dependent of more than one node. */
int has_multiple_cdep(const ir_node *block) {
	ir_cdep *cdep = find_cdep(block);

	return cdep != NULL && cdep->next != NULL;
}
