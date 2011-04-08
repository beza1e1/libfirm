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
 * @brief   PBQP edges.
 * @date    02.10.2008
 * @author  Sebastian Buchwald
 * @version $Id$
 */
#include "config.h"

#include "adt/array.h"
#include "assert.h"

#include "kaps.h"
#include "matrix.h"
#include "optimal.h"
#include "pbqp_edge.h"
#include "pbqp_edge_t.h"
#include "pbqp_node.h"
#include "pbqp_node_t.h"
#include "pbqp_t.h"

pbqp_edge_t *alloc_edge(pbqp_t *pbqp, int src_index, int tgt_index,
                        pbqp_matrix_t *costs)
{
	int transpose = 0;

	if (tgt_index < src_index) {
		int tmp = src_index;
		src_index = tgt_index;
		tgt_index = tmp;

		transpose = 1;
	}

	pbqp_edge_t *edge = OALLOC(&pbqp->obstack, pbqp_edge_t);

	pbqp_node_t *src_node = get_node(pbqp, src_index);

	pbqp_node_t *tgt_node = get_node(pbqp, tgt_index);

	if (transpose) {
		edge->costs = pbqp_matrix_copy_and_transpose(pbqp, costs);
	} else {
		edge->costs = pbqp_matrix_copy(pbqp, costs);
	}

	/*
	 * Connect edge with incident nodes. Since the edge is allocated, we know
	 * that it don't appear in the edge lists of the nodes.
	 */
	ARR_APP1(pbqp_edge_t *, src_node->edges, edge);
	edge->src = src_node;
	ARR_APP1(pbqp_edge_t *, tgt_node->edges, edge);
	edge->tgt = tgt_node;
	edge->bucket_index = UINT_MAX;

	return edge;
}

void delete_edge(pbqp_edge_t *edge)
{
	pbqp_node_t  *src_node;
	pbqp_node_t  *tgt_node;

	src_node = edge->src;
	tgt_node = edge->tgt;

	disconnect_edge(src_node, edge);
	disconnect_edge(tgt_node, edge);

	edge->src = NULL;
	edge->tgt = NULL;

	reorder_node_after_edge_deletion(src_node);
	reorder_node_after_edge_deletion(tgt_node);
}

unsigned is_deleted(pbqp_edge_t *edge)
{
	unsigned deleted;

	deleted = (edge->src == NULL) && (edge-> tgt == NULL);

	return deleted;
}

pbqp_edge_t *pbqp_edge_deep_copy(pbqp_t *pbqp, pbqp_edge_t *edge,
                                 pbqp_node_t *src_node, pbqp_node_t *tgt_node)
{
	pbqp_edge_t *copy = OALLOC(&pbqp->obstack, pbqp_edge_t);
	assert(src_node);
	assert(tgt_node);

	copy->costs = pbqp_matrix_copy(pbqp, edge->costs);

	/* Connect edge with incident nodes. */
	copy->src = src_node;
	copy->tgt = tgt_node;
	copy->bucket_index = UINT_MAX;

	return copy;
}
