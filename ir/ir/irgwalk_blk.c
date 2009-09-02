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
 * @brief   Blockwise walker implementation
 * @author  Michael Beck
 * @version $Id$
 */
#include "config.h"

#include "irnode_t.h"
#include "irgraph_t.h" /* visited flag */
#include "irgwalk.h"
#include "pset.h"
#include "irhooks.h"
#include "array.h"
#include "hashptr.h"

#define _get_walk_arity(env, node) \
	((env)->follow_deps ? get_irn_ins_or_deps((node)) : get_irn_arity((node)))
#define _get_walk_irn_n(env, node, pos) \
	((env)->follow_deps ? get_irn_in_or_dep((node), (pos)) : get_irn_n((node), (pos)))

/**
 * Metadata for block walker
 */
typedef struct _blk_collect_data_t {
	struct obstack obst;            /**< obstack to allocate objects on */
	pset           *blk_map;        /**< Hash map: Block -> List */
	ir_node        **blk_list;      /**< the Block list */
	unsigned       follow_deps : 1; /**< follow dependency edges */
} blk_collect_data_t;

/**
 * An entry for a block in the blk_map
 */
typedef struct _block_entry_t {
	ir_node *block;       /**< the block */
	ir_node **phi_list;   /**< the list of Phi instruction */
	ir_node **df_list;    /**< the list of data flow instruction */
	ir_node **cf_list;    /**< the list of control flow instructions */
	ir_node **entry_list; /**< list of all block entries */
} block_entry_t;

/**
 * compare two block_entries
 */
static int addr_cmp(const void *elt, const void *key) {
	const block_entry_t *e1 = elt;
	const block_entry_t *e2 = key;

	return e1->block != e2->block;
}

/**
 * Returns the associates block_entry_t for an block
 */
static block_entry_t *block_find_entry(ir_node *block, blk_collect_data_t *ctx)
{
	block_entry_t key;
	block_entry_t *elem;

	key.block = block;
	elem = pset_find(ctx->blk_map, &key, HASH_PTR(block));
	if (elem)
		return elem;

	elem = OALLOC(&ctx->obst, block_entry_t);

	elem->block      = block;
	elem->phi_list   = NEW_ARR_F(ir_node *, 0);
	elem->df_list    = NEW_ARR_F(ir_node *, 0);
	elem->cf_list    = NEW_ARR_F(ir_node *, 0);
	elem->entry_list = NEW_ARR_F(ir_node *, 0);

	return pset_insert(ctx->blk_map, elem, HASH_PTR(block));
}

/**
 * Traverse a block in pre order.
 */
static void traverse_block_pre(ir_node *block, block_entry_t *entry, irg_walk_func *pre, void *env) {
	int j;

	for (j = ARR_LEN(entry->cf_list) - 1; j >= 0; --j) {
		ir_node *node = entry->cf_list[j];
		pre(node, env);
	}

	for (j = ARR_LEN(entry->df_list) - 1; j >= 0; --j) {
		ir_node *node = entry->df_list[j];
		pre(node, env);
	}

	for (j = ARR_LEN(entry->phi_list) - 1; j >= 0; --j) {
		ir_node *node = entry->phi_list[j];
		pre(node, env);
	}

	pre(block, env);
}

/**
 * Traverse a block in post order.
 */
void traverse_block_post(ir_node *block, block_entry_t *entry, irg_walk_func *post, void *env)  {
	int j, n;

	post(block, env);

	for (j = 0, n = ARR_LEN(entry->phi_list); j < n; ++j) {
		ir_node *node = entry->phi_list[j];
		post(node, env);
	}

	for (j = 0, n = ARR_LEN(entry->df_list); j < n; ++j) {
		ir_node *node = entry->df_list[j];
		post(node, env);
	}

	for (j = 0, n = ARR_LEN(entry->cf_list); j < n; ++j) {
		ir_node *node = entry->cf_list[j];
		post(node, env);
	}
}

/**
 * traverse the pre order only, from End to Start
 */
static void traverse_pre(blk_collect_data_t *blks, irg_walk_func *pre, void *env) {
	int i;

	for (i = ARR_LEN(blks->blk_list) - 1; i >= 0; --i) {
		ir_node       *block = blks->blk_list[i];
		block_entry_t *entry = block_find_entry(block, blks);

		traverse_block_pre(block, entry, pre, env);

		DEL_ARR_F(entry->entry_list);
		DEL_ARR_F(entry->phi_list);
		DEL_ARR_F(entry->df_list);
		DEL_ARR_F(entry->cf_list);
	}
}

/**
 * traverse the post order only, from Start to End
 */
static void traverse_post(blk_collect_data_t *blks, irg_walk_func *post, void *env) {
	int i, k;

	for (i = 0, k = ARR_LEN(blks->blk_list); i < k; ++i) {
		ir_node       *block = blks->blk_list[i];
		block_entry_t *entry = block_find_entry(block, blks);

		traverse_block_post(block, entry, post, env);

		DEL_ARR_F(entry->entry_list);
		DEL_ARR_F(entry->phi_list);
		DEL_ARR_F(entry->df_list);
		DEL_ARR_F(entry->cf_list);
	}
}

/**
 * traverse both
 */
static void traverse_both(blk_collect_data_t *blks, irg_walk_func *pre, irg_walk_func *post, void *env)
{
	int i;

	for (i = ARR_LEN(blks->blk_list) - 1; i >= 0; --i) {
		ir_node       *block = blks->blk_list[i];
		block_entry_t *entry = block_find_entry(block, blks);

		traverse_block_pre(block, entry, pre, env);
	}

	/* second step */
	traverse_post(blks, post, env);
}

/**
 * Do the traversal.
 */
static void traverse_blocks(blk_collect_data_t *blks, irg_walk_func *pre, irg_walk_func *post, void *env) {
	if      (!post) traverse_pre (blks, pre, env);
	else if (!pre)  traverse_post(blks, post, env);
	else            traverse_both(blks, pre, post, env);
}

typedef struct dom_traversal_t {
	blk_collect_data_t *blks;
	irg_walk_func      *pre;
	irg_walk_func      *post;
	void               *env;
} dom_traversal_t;

/**
 * Dom block walker. Visit all nodes in pre oder.
 */
static void dom_block_visit_pre(ir_node *block, void *env) {
	dom_traversal_t *ctx   = env;
	block_entry_t   *entry = block_find_entry(block, ctx->blks);

	traverse_block_pre(block, entry, ctx->pre, ctx->env);
}

/**
 * Dom block walker. Visit all nodes in post oder.
 */
static void dom_block_visit_post(ir_node *block, void *env) {
	dom_traversal_t *ctx   = env;
	block_entry_t   *entry = block_find_entry(block, ctx->blks);

	traverse_block_post(block, entry, ctx->post, ctx->env);
}

/**
 * Dom block walker. Visit all nodes in pre oder, than in post order.
 */
static void dom_block_visit_both(ir_node *block, void *env) {
	dom_traversal_t *ctx   = env;
	block_entry_t   *entry = block_find_entry(block, ctx->blks);

	traverse_block_pre(block, entry, ctx->pre, ctx->env);
	traverse_block_post(block, entry, ctx->post, ctx->env);
}

/**
 * Do the traversal in the dominator tree in top-down order.
 */
static void traverse_dom_blocks_top_down(blk_collect_data_t* blks, irg_walk_func *pre, irg_walk_func *post, void *env) {
	dom_traversal_t ctx;

	ctx.blks = blks;
	ctx.pre  = pre;
	ctx.post = post;
	ctx.env  = env;

	if (pre != NULL && post != NULL)
		dom_tree_walk_irg(current_ir_graph,	dom_block_visit_both, NULL, &ctx);
	else if (pre != NULL)
		dom_tree_walk_irg(current_ir_graph,	dom_block_visit_pre, NULL, &ctx);
	else if (post != NULL)
		dom_tree_walk_irg(current_ir_graph,	dom_block_visit_post, NULL, &ctx);
}

/**
 * walks over the graph and collects all blocks and all block entries
 */
static void collect_walk(ir_node *node, blk_collect_data_t *env)
{
	int           i, is_phi;
	block_entry_t *entry;
	ir_node       *block;

	mark_irn_visited(node);

	if (node->op == op_Block) {
		/* predecessors of a block are control flow nodes */
		for (i = _get_walk_arity(env, node) - 1; i >= 0; --i) {
			ir_node *pred = _get_walk_irn_n(env, node, i);
			ir_node *blk  = get_nodes_block(pred);

			if (!irn_visited(pred)) {
				collect_walk(pred, env);

				/* control flow predecessors are always block inputs */
				entry = block_find_entry(blk, env);
				ARR_APP1(ir_node *, entry->entry_list, pred);
			}
		}

		/* it's a block, put it into the block list */
		if (node == get_irg_end_block(current_ir_graph)) {
			/* Put the end block always last. If we don't do it here,
			 * it might be placed elsewhere if the graph contains
			 * endless loops.
			 */
		} else {
			ARR_APP1(ir_node *, env->blk_list, node);
		}
	}
	else {
		block = get_nodes_block(node);

		if (!irn_visited(block))
			collect_walk(block, env);

		is_phi = is_Phi(node);
		for (i = _get_walk_arity(env, node) - 1; i >= 0; --i) {
			ir_node *pred = _get_walk_irn_n(env, node, i);

			if (!irn_visited(pred)) {
				collect_walk(pred, env);

				/* BEWARE: predecessors of End nodes might be blocks */
				if (is_no_Block(pred)) {
					ir_node *blk  = get_nodes_block(pred);

					/*
					 * Note that Phi predecessors are always block entries
					 * because Phi edges are always "outside" a block
					 */
					if (block != blk || is_phi) {
						entry = block_find_entry(blk, env);
						ARR_APP1(ir_node *, entry->entry_list, pred);
					}
				}
			}
		}
	}
}

/**
 * walks over the nodes of a block
 * and collects them into the right list
 */
static void collect_blks_lists(ir_node *node, ir_node *block,
                               block_entry_t *entry, blk_collect_data_t *env)
{
	int i;

	mark_irn_visited(node);

	/*
	 * Do not descent into Phi predecessors, these are always
	 * outside the current block because Phi edges are always
	 * "outside".
	 */
	if (! is_Phi(node)) {
		for (i = _get_walk_arity(env, node) - 1; i >= 0; --i) {
			ir_node *pred = _get_walk_irn_n(env, node, i);

			/* BEWARE: predecessors of End nodes might be blocks */
			if (is_no_Block(pred)) {
				ir_node *blk  = get_nodes_block(pred);

				if (!irn_visited(pred)) {
					if (block != blk)
						continue;
					collect_blks_lists(pred, block, entry, env);
				}
			}
		}
	} else {
		ARR_APP1(ir_node *, entry->phi_list, node);
		return;
	}

	if (get_irn_mode(node) == mode_X) {
		ARR_APP1(ir_node *, entry->cf_list, node);
	} else {
		ARR_APP1(ir_node *, entry->df_list, node);
	}
}

/**
 * walk over the graph and collect all lists
 */
static void collect_lists(blk_collect_data_t *env)
{
	int             i, j;
	ir_node         *block, *node;
	block_entry_t   *entry;

	inc_irg_visited(current_ir_graph);

	for (i = ARR_LEN(env->blk_list) - 1; i >= 0; --i) {
		block = env->blk_list[i];
		entry = block_find_entry(block, env);

		for (j = ARR_LEN(entry->entry_list) - 1; j >= 0; --j) {
			node = entry->entry_list[j];

			/* a entry might already be visited due to Phi loops */
			if (node->visited < current_ir_graph->visited)
				collect_blks_lists(node, block, entry, env);
		}
	}
}

/**
 * Intraprozedural graph walker over blocks.
 */
static void
do_irg_walk_blk(ir_graph *irg, irg_walk_func *pre, irg_walk_func *post, void *env, unsigned follow_deps,
                void (*traverse)(blk_collect_data_t* blks, irg_walk_func *pre, irg_walk_func *post, void *env))
{
	ir_node            *end_node = get_irg_end(irg);
	ir_node            *end_blk = get_irg_end_block(irg);
	blk_collect_data_t blks;
	block_entry_t      *entry;

#ifdef INTERPROCEDURAL_VIEW
	/* switch off interprocedural view */
	int old_view       = get_interprocedural_view();
	set_interprocedural_view(0);
#endif

	obstack_init(&blks.obst);
	blks.blk_map     = new_pset(addr_cmp, 1);
	blks.blk_list    = NEW_ARR_F(ir_node *, 0);
	blks.follow_deps = follow_deps != 0;

	/* first step: traverse the graph and fill the lists */
	ir_reserve_resources(irg, IR_RESOURCE_IRN_VISITED);
	inc_irg_visited(irg);
	collect_walk(end_node, &blks);

	/* add the end block */
	ARR_APP1(ir_node *, blks.blk_list, end_blk);

	/* and the end node */
	entry = block_find_entry(end_blk, &blks);
	ARR_APP1(ir_node *, entry->entry_list, end_node);

	collect_lists(&blks);

	/* second step: traverse the list */
	traverse(&blks, pre, post, env);

	DEL_ARR_F(blks.blk_list);
	del_pset(blks.blk_map);
	obstack_free(&blks.obst, NULL);

#ifdef INTERPROCEDURAL_VIEW
	set_interprocedural_view(old_view);
#endif
	ir_free_resources(irg, IR_RESOURCE_IRN_VISITED);
}

void irg_walk_blkwise_graph(ir_graph *irg, irg_walk_func *pre, irg_walk_func *post, void *env) {
	ir_graph * rem = current_ir_graph;

	hook_irg_walk_blkwise(irg, (generic_func *)pre, (generic_func *)post);
	current_ir_graph = irg;
	do_irg_walk_blk(irg, pre, post, env, 0, traverse_blocks);
	current_ir_graph = rem;
}

void irg_walk_in_or_dep_blkwise_graph(ir_graph *irg, irg_walk_func *pre, irg_walk_func *post, void *env) {
	ir_graph * rem = current_ir_graph;

	hook_irg_walk_blkwise(irg, (generic_func *)pre, (generic_func *)post);
	current_ir_graph = irg;
	do_irg_walk_blk(irg, pre, post, env, 1, traverse_blocks);
	current_ir_graph = rem;
}

void irg_walk_blkwise_dom_top_down(ir_graph *irg, irg_walk_func *pre, irg_walk_func *post, void *env) {
	ir_graph * rem = current_ir_graph;

	hook_irg_walk_blkwise(irg, (generic_func *)pre, (generic_func *)post);
	current_ir_graph = irg;
	do_irg_walk_blk(irg, pre, post, env, 0, traverse_dom_blocks_top_down);
	current_ir_graph = rem;
}
