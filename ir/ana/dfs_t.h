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
 * @file    dfs_t.h
 * @author  Sebastian Hack
 * @date    21.04.2007
 * @version $Id$
 * @brief
 *
 * depth first search internal stuff.
 */
#ifndef FIRM_ANA_DFS_T_H
#define FIRM_ANA_DFS_T_H

#include "hashptr.h"
#include "absgraph.h"
#include "obst.h"
#include "dfs.h"

struct _dfs_node_t {
	int visited;
	const void *node;
	const void *ancestor;
	int pre_num;
	int max_pre_num;
	int	post_num;
	int	level;
};

struct _dfs_edge_t {
	const void *src, *tgt;
	dfs_node_t *s, *t;
	dfs_edge_kind_t kind;
};

struct _dfs_t {
	void *graph;
	const absgraph_t *graph_impl;
	struct obstack obst;

	set *nodes;
	set *edges;
	dfs_node_t **pre_order;
	dfs_node_t **post_order;

	int pre_num;
	int post_num;

	unsigned edges_classified : 1;
};

static struct _dfs_node_t *_dfs_get_node(const struct _dfs_t *self, const void *node)
{
	struct _dfs_node_t templ;
	memset(&templ, 0, sizeof(templ));
	templ.node = node;
	return set_insert(self->nodes, &templ, sizeof(templ), HASH_PTR(node));
}

#define _dfs_int_is_ancestor(n, m) ((m)->pre_num >= (n)->pre_num && (m)->pre_num <= (n)->max_pre_num)

static inline int _dfs_is_ancestor(const struct _dfs_t *dfs, const void *a, const void *b)
{
	struct _dfs_node_t *n = _dfs_get_node(dfs, a);
	struct _dfs_node_t *m = _dfs_get_node(dfs, b);
	return _dfs_int_is_ancestor(n, m);
}

#define dfs_get_n_nodes(dfs)            ((dfs)->pre_num)
#define dfs_get_pre_num(dfs, node)      (_dfs_get_node((dfs), (node))->pre_num)
#define dfs_get_post_num(dfs, node)     (_dfs_get_node((dfs), (node))->post_num)
#define dfs_get_pre_num_node(dfs, num)  ((dfs)->pre_order[num]->node)
#define dfs_get_post_num_node(dfs, num) ((dfs)->post_order[num]->node)
#define dfs_is_ancestor(dfs, n, m)      _dfs_is_ancestor((n), (m))

#endif /* FIRM_ANA_DFS_T_H */
