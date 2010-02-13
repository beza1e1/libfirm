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
 * @brief       Random node selector.
 * @author      Matthias Braun
 * @date        29.08.2006
 * @version     $Id$
 */
#include "config.h"

#include <stdlib.h>

#include "besched.h"
#include "belistsched.h"

/**
 * The random selector:
 * Just assure that branches are executed last, otherwise select a random node
 */
static ir_node *random_select(void *block_env, ir_nodeset_t *ready_set,
                              ir_nodeset_t *live_set)
{
	ir_nodeset_iterator_t iter;
	ir_node          *irn      = NULL;
	int only_branches_left = 1;
	(void)block_env;
	(void)live_set;

	/* assure that branches and constants are executed last */
	ir_nodeset_iterator_init(&iter, ready_set);
	while ( (irn = ir_nodeset_iterator_next(&iter)) != NULL) {
		if (!is_cfop(irn)) {
			only_branches_left = 0;
			break;
		}
	}

	if (only_branches_left) {
		/* at last: schedule branches */
		ir_nodeset_iterator_init(&iter, ready_set);
		irn = ir_nodeset_iterator_next(&iter);
	} else {
		do {
			/* take 1 random node */
			int n = rand() % ir_nodeset_size(ready_set);
			int i = 0;
			ir_nodeset_iterator_init(&iter, ready_set);
			while ((irn = ir_nodeset_iterator_next(&iter)) != NULL) {
				if (i == n) {
					break;
				}
				++i;
			}
		} while (is_cfop(irn));
	}

	return irn;
}

static void *random_init_graph(const list_sched_selector_t *vtab, const be_irg_t *birg)
{
	(void)vtab;
	(void)birg;
	/* Using time(NULL) as a seed here gives really random results,
	   but is NOT deterministic which makes debugging impossible.
	   Moreover no-one want non-deterministic compilers ... */
	srand(0x4711);
	return NULL;
}

static void *random_init_block(void *graph_env, ir_node *block)
{
	(void)graph_env;
	(void)block;
	return NULL;
}

const list_sched_selector_t random_selector = {
	random_init_graph,
	random_init_block,
	random_select,
	NULL,                /* to_appear_in_schedule */
	NULL,                /* node_ready */
	NULL,                /* node_selected */
	NULL,                /* exectime */
	NULL,                /* latency */
	NULL,                /* finish_block */
	NULL                 /* finish_graph */
};
