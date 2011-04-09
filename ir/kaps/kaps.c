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
 * @brief   Partitioned Boolean Quadratic Problem (PBQP) solver.
 * @date    02.10.2008
 * @author  Sebastian Buchwald
 * @version $Id$
 */
#include "config.h"

#include "adt/array.h"
#include "adt/xmalloc.h"

#include "kaps.h"
#include "matrix.h"
#include "pbqp_edge.h"
#include "pbqp_edge_t.h"
#include "pbqp_node.h"
#include "pbqp_node_t.h"
#include "vector.h"

pbqp_node_t *get_node(pbqp_t *pbqp, unsigned index)
{
	return pbqp->nodes[index];
}

pbqp_edge_t *get_edge(pbqp_t *pbqp, unsigned src_index, unsigned tgt_index)
{
	int i;
	int len;
	pbqp_node_t *src_node;
	pbqp_node_t *tgt_node;

	if (tgt_index < src_index) {
		unsigned tmp = src_index;
		src_index    = tgt_index;
		tgt_index    = tmp;
	}

	src_node = get_node(pbqp, src_index);
	tgt_node = get_node(pbqp, tgt_index);
	assert(tgt_node);

	len = ARR_LEN(src_node->edges);

	for (i = 0; i < len; ++i) {
		pbqp_edge_t *cur_edge = src_node->edges[i];
		if (cur_edge->tgt == tgt_node) {
			return cur_edge;
		}
	}

	return NULL;
}

pbqp_t *alloc_pbqp(unsigned number_nodes)
{
	pbqp_t *pbqp = XMALLOC(pbqp_t);

	obstack_init(&pbqp->obstack);

	pbqp->solution = 0;
	pbqp->num_nodes = number_nodes;
#if	KAPS_DUMP
	pbqp->dump_file = NULL;
#endif
	pbqp->nodes = OALLOCNZ(&pbqp->obstack, pbqp_node_t*, number_nodes);
#if KAPS_STATISTIC
	pbqp->num_bf = 0;
	pbqp->num_edges = 0;
	pbqp->num_r0 = 0;
	pbqp->num_r1 = 0;
	pbqp->num_r2 = 0;
	pbqp->num_rm = 0;
	pbqp->num_rn = 0;
#endif

	return pbqp;
}

void free_pbqp(pbqp_t *pbqp)
{
	obstack_free(&pbqp->obstack, NULL);
	xfree(pbqp);
}

void add_node_costs(pbqp_t *pbqp, unsigned node_index, vector_t *costs)
{
	pbqp_node_t *node = get_node(pbqp, node_index);

	if (node == NULL) {
		node = alloc_node(pbqp, node_index, costs);
		pbqp->nodes[node_index] = node;
	} else {
		vector_add(node->costs, costs);
	}
}

void add_edge_costs(pbqp_t *pbqp, unsigned src_index, unsigned tgt_index,
                    pbqp_matrix_t *costs)
{
	pbqp_edge_t *edge = get_edge(pbqp, src_index, tgt_index);

	if (tgt_index < src_index) {
		pbqp_matrix_transpose(pbqp, costs);
		add_edge_costs(pbqp, tgt_index, src_index, costs);
		return;
	}

	if (edge == NULL) {
		edge = alloc_edge(pbqp, src_index, tgt_index, costs);
	} else {
		pbqp_matrix_add(edge->costs, costs);
	}
}

num get_node_solution(pbqp_t *pbqp, unsigned node_index)
{
	pbqp_node_t *node = get_node(pbqp, node_index);

	return node->solution;
}

num get_solution(pbqp_t *pbqp)
{
	return pbqp->solution;
}

#if	KAPS_DUMP
void set_dumpfile(pbqp *pbqp, FILE *f)
{
	pbqp->dump_file = f;
}
#endif
