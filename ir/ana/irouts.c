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
 * @brief    Compute and access out edges (also called def-use edges).
 * @author   Goetz Lindenmaier, Michael Beck
 * @date     1.2002
 * @version  $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "xmalloc.h"
#include "irouts.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irprog_t.h"
#include "irgwalk.h"
#include "irtools.h"

#ifdef DEBUG_libfirm
/* Note:  ir_node.out_valid and ir_graph.n_outs are only present when DEBUG_libfirm is defined */
/* Accesses to out_valid and n_outs are fenced out to avoid breakage
   when compiling with neither DEBUG_libfirm or NDEBUG defined */
#endif /* defined DEBUG_libfirm */

/*--------------------------------------------------------------------*/
/** Accessing the out datastructures                                 **/
/*--------------------------------------------------------------------*/

#ifdef DEBUG_libfirm
/** Clear the outs of a node */
static void reset_outs(ir_node *node, void *unused) {
	(void) unused;
	node->out       = NULL;
	node->out_valid = 0;
}
#endif

int get_irn_outs_computed(const ir_node *node)
{
	return node->out != NULL;
}

/* returns the number of successors of the node: */
int get_irn_n_outs(ir_node *node) {
	assert(node && node->kind == k_ir_node);
#ifdef DEBUG_libfirm
	/* assert(node->out_valid); */
#endif /* defined DEBUG_libfirm */
	/* we misuse the first for the size info of the out array */
	return node->out[0].pos;
}

/* Access successor n */
ir_node *get_irn_out(ir_node *def, int pos) {
	assert(pos >= 0 && pos < get_irn_n_outs(def));
#ifdef DEBUG_libfirm
	/* assert(def->out_valid); */
#endif /* defined DEBUG_libfirm */
	return def->out[pos+1].use;
}

/* Access successor n */
ir_node *get_irn_out_ex(ir_node *def, int pos, int *in_pos) {
	assert(pos >= 0 && pos < get_irn_n_outs(def));
#ifdef DEBUG_libfirm
	/* assert(def->out_valid); */
#endif /* defined DEBUG_libfirm */
	*in_pos = def->out[pos+1].pos;
	return def->out[pos+1].use;
}

void set_irn_out(ir_node *def, int pos, ir_node *use, int in_pos) {
	assert(def && use);
	assert(pos >= 0 && pos < get_irn_n_outs(def));
#ifdef DEBUG_libfirm
	def->out_valid = 1;          /* assume that this function is used correctly */
#endif /* defined DEBUG_libfirm */
	def->out[pos+1].use = use;
	def->out[pos+1].pos = in_pos;
}

/* Return the number of control flow successors, ignore keep-alives. */
int get_Block_n_cfg_outs(ir_node *bl) {
	int i, n_cfg_outs = 0;
	assert(bl && is_Block(bl));
#ifdef DEBUG_libfirm
	assert(bl->out_valid);
#endif /* defined DEBUG_libfirm */
	for (i = 1; i <= bl->out[0].pos; ++i) {
		ir_node *succ = bl->out[i].use;
		if (get_irn_mode(succ) == mode_X && !is_End(succ))
			n_cfg_outs += succ->out[0].pos;
	}
	return n_cfg_outs;
}

/* Return the number of control flow successors, honor keep-alives. */
int get_Block_n_cfg_outs_ka(ir_node *bl) {
	int i, n_cfg_outs = 0;
	assert(bl && is_Block(bl));
#ifdef DEBUG_libfirm
	assert(bl->out_valid);
#endif /* defined DEBUG_libfirm */
	for (i = 1; i <= bl->out[0].pos; ++i) {
		ir_node *succ = bl->out[i].use;
		if (get_irn_mode(succ) == mode_X) {

			if (is_End(succ)) {
				/* ignore End if we are in the Endblock */
				if (get_nodes_block(succ) == bl)
					continue;
				else /* count Keep-alive as one */
					n_cfg_outs += 1;
			} else
				n_cfg_outs += succ->out[0].pos;
		}
	}
	return n_cfg_outs;
}

/* Access predecessor n, ignore keep-alives. */
ir_node *get_Block_cfg_out(ir_node *bl, int pos) {
	int i;
	assert(bl && is_Block(bl));
#ifdef DEBUG_libfirm
	assert(bl->out_valid);
#endif /* defined DEBUG_libfirm */
	for (i = 1; i <= bl->out[0].pos; ++i) {
		ir_node *succ = bl->out[i].use;
		if (get_irn_mode(succ) == mode_X && !is_End(succ)) {
			int n_outs = succ->out[0].pos;
			if (pos < n_outs)
				return succ->out[pos + 1].use;
			else
				pos -= n_outs;
		}
	}
	return NULL;
}

/* Access predecessor n, honor keep-alives. */
ir_node *get_Block_cfg_out_ka(ir_node *bl, int pos) {
	int i, n_outs;
	assert(bl && is_Block(bl));
#ifdef DEBUG_libfirm
	assert (bl->out_valid);
#endif /* defined DEBUG_libfirm */
	for (i = 1; i <= bl->out[0].pos; ++i) {
		ir_node *succ = bl->out[i].use;
		if (get_irn_mode(succ) == mode_X) {
			if (is_End(succ)) {
				ir_node *end_bl = get_nodes_block(succ);
				if (end_bl == bl) {
					/* ignore End if we are in the Endblock */
					continue;
				}
				if (pos == 0) {
					/* handle keep-alive here: return the Endblock instead of the End node */
					return end_bl;
				} else
					--pos;
			} else {
				n_outs = succ->out[0].pos;
				if (pos < n_outs)
					return succ->out[pos + 1].use;
				else
					pos -= n_outs;
			}
		}
	}
	return NULL;
}

static void irg_out_walk_2(ir_node *node, irg_walk_func *pre,
            irg_walk_func *post, void *env) {
	int     i, n;
	ir_node *succ;

	assert(node);
	assert(get_irn_visited(node) < get_irg_visited(current_ir_graph));

	set_irn_visited(node, get_irg_visited(current_ir_graph));

	if (pre) pre(node, env);

	for (i = 0, n = get_irn_n_outs(node); i < n; ++i) {
		succ = get_irn_out(node, i);
		if (get_irn_visited(succ) < get_irg_visited(current_ir_graph))
			irg_out_walk_2(succ, pre, post, env);
	}

	if (post) post(node, env);

	return;
}

void irg_out_walk(ir_node *node,
                  irg_walk_func *pre, irg_walk_func *post,
                  void *env) {
	assert(node);
	if (get_irg_outs_state(current_ir_graph) != outs_none) {
		inc_irg_visited (current_ir_graph);
		irg_out_walk_2(node, pre, post, env);
	}
	return;
}

static void irg_out_block_walk2(ir_node *bl,
                                irg_walk_func *pre, irg_walk_func *post,
                                void *env) {
	int i, n;

	if (Block_not_block_visited(bl)) {
		mark_Block_block_visited(bl);

		if (pre)
			pre(bl, env);

		for (i = 0, n =  get_Block_n_cfg_outs(bl); i < n; ++i) {
			/* find the corresponding predecessor block. */
			ir_node *pred = get_Block_cfg_out(bl, i);
			/* recursion */
			irg_out_block_walk2(pred, pre, post, env);
		}

		if (post)
			post(bl, env);
	}
}

/* Walks only over Block nodes in the graph.  Has it's own visited
   flag, so that it can be interleaved with the other walker.         */
void irg_out_block_walk(ir_node *node,
                        irg_walk_func *pre, irg_walk_func *post,
                        void *env) {

	assert(is_Block(node) || (get_irn_mode(node) == mode_X));

	inc_irg_block_visited(current_ir_graph);

	if (get_irn_mode(node) == mode_X) {
		int i, n;

		for (i = 0, n = get_irn_n_outs(node); i < n; ++i) {
			ir_node *succ = get_irn_out(node, i);
			if (get_irn_visited(succ) < get_irg_visited(current_ir_graph))
				irg_out_walk_2(succ, pre, post, env);
		}
	}
	else {
		irg_out_block_walk2(node, pre, post, env);
	}
}

/*--------------------------------------------------------------------*/
/** Building and Removing the out datastructure                      **/
/**                                                                  **/
/** The outs of a graph are allocated in a single, large array.      **/
/** This allows to allocate and deallocate the memory for the outs   **/
/** on demand.  The large array is separated into many small ones    **/
/** for each node.  Only a single field to reference the out array   **/
/** is stored in each node and a field referencing the large out     **/
/** array in irgraph.  The 0 field of each out array contains the    **/
/** size of this array.  This saves memory in the irnodes themselves.**/
/** The construction does two passes over the graph.  The first pass **/
/** counts the overall number of outs and the outs of each node.  It **/
/** stores the outs of each node in the out reference of the node.   **/
/** Then the large array is allocated.  The second iteration chops   **/
/** the large array into smaller parts, sets the out edges and       **/
/** recounts the out edges.                                          **/
/** Removes Tuple nodes!                                             **/
/*--------------------------------------------------------------------*/


/** Returns the amount of out edges for not yet visited successors. */
static int _count_outs(ir_node *n) {
	int start, i, res, irn_arity;

	mark_irn_visited(n);
	n->out = INT_TO_PTR(1);     /* Space for array size. */

	start = is_Block(n) ? 0 : -1;
	irn_arity = get_irn_arity(n);
	res = irn_arity - start + 1;  /* --1 or --0; 1 for array size. */

	for (i = start; i < irn_arity; ++i) {
		/* Optimize Tuples.  They annoy if walking the cfg. */
		ir_node *pred         = get_irn_n(n, i);
		ir_node *skipped_pred = skip_Tuple(pred);

		if (skipped_pred != pred) {
			set_irn_n(n, i, skipped_pred);
		}

		/* count Def-Use edges for predecessors */
		if (irn_not_visited(skipped_pred))
			res += _count_outs(skipped_pred);

		/*count my Def-Use edges */
		skipped_pred->out = INT_TO_PTR(PTR_TO_INT(skipped_pred->out) + 1);
	}
	return res;
}


/** Returns the amount of out edges for not yet visited successors.
 *  This version handles some special nodes like irg_frame, irg_args etc.
 */
static int count_outs(ir_graph *irg) {
	ir_node *n;
	int     i, res;

	inc_irg_visited(irg);
	res = _count_outs(get_irg_end(irg));

	/* Now handle anchored nodes. We need the out count of those
	   even if they are not visible. */
	for (i = anchor_last - 1; i >= 0; --i) {
		n = get_irg_anchor(irg, i);
		if (irn_not_visited(n)) {
			mark_irn_visited(n);

			n->out = INT_TO_PTR(1);
			++res;
		}
	}
	return res;
}

/**
 * Enter memory for the outs to a node.
 *
 * @param use    current node
 * @param free   current free address in the chunk allocated for the outs
 *
 * @return The next free address
 */
static ir_def_use_edge *_set_out_edges(ir_node *use, ir_def_use_edge *free) {
	int     n_outs, start, i, irn_arity, pos;

	mark_irn_visited(use);

	/* Allocate my array */
	n_outs = PTR_TO_INT(use->out);
	use->out = free;
#ifdef DEBUG_libfirm
	use->out_valid = 1;
#endif /* defined DEBUG_libfirm */
	free += n_outs;
	/* We count the successors again, the space will be sufficient.
	   We use this counter to remember the position for the next back
	   edge. */
	use->out[0].pos = 0;

	start = is_Block(use) ? 0 : -1;
	irn_arity = get_irn_arity(use);

	for (i = start; i < irn_arity; ++i) {
		ir_node *def = get_irn_n(use, i);

		/* Recursion */
		if (irn_not_visited(def))
			free = _set_out_edges(def, free);

		/* Remember this Def-Use edge */
		pos = def->out[0].pos + 1;
		def->out[pos].use = use;
		def->out[pos].pos = i;

		/* increase the number of Def-Use edges so far */
		def->out[0].pos = pos;
	}
	return free;
}

/**
 * Enter memory for the outs to a node. Handles special nodes
 *
 * @param irg    the graph
 * @param free   current free address in the chunk allocated for the outs
 *
 * @return The next free address
 */
static ir_def_use_edge *set_out_edges(ir_graph *irg, ir_def_use_edge *free) {
	ir_node *n;
	int     i, n_outs;

	inc_irg_visited(irg);
	free = _set_out_edges(get_irg_end(irg), free);

	/* handle anchored nodes */
	for (i = anchor_last - 1; i >= 0; --i) {
		n = get_irg_anchor(irg, i);
		if (irn_not_visited(n)) {
			mark_irn_visited(n);

			n_outs = PTR_TO_INT(n->out);
			n->out = free;
#ifdef DEBUG_libfirm
			n->out_valid = 1;
#endif /* defined DEBUG_libfirm */
			free += n_outs;
		}
	}

	return free;
}


/**
 * We want that the out of ProjX from Start contains the next block at
 * position 1, the Start block at position 2.  This is necessary for
 * the out block walker.
 */
static INLINE void fix_start_proj(ir_graph *irg) {
	ir_node *proj    = NULL;
	ir_node *irn;
	ir_node *startbl = get_irg_start_block(irg);
	int     i, block_pos, other_pos;

	if (get_Block_n_cfg_outs(startbl)) {
		for (i = get_irn_n_outs(startbl) - 1; i >= 0; --i)
			if (get_irn_mode(get_irn_out(startbl, i)) == mode_X) {
				proj = get_irn_out(startbl, i);
				break;
			}

		if (get_irn_out_ex(proj, 0, &block_pos) == startbl) {
			assert(get_irn_n_outs(proj) == 2);
			irn = get_irn_out_ex(proj, 1, &other_pos);
			set_irn_out(proj, 0, irn, other_pos);
			set_irn_out(proj, 1, startbl, block_pos);
		}
	}
}

/* compute the outs for a given graph */
void compute_irg_outs(ir_graph *irg) {
	ir_graph        *rem = current_ir_graph;
	int             n_out_edges = 0;
	ir_def_use_edge *end = NULL;         /* Only for debugging */

	current_ir_graph = irg;

	/* Update graph state */
	assert(get_irg_phase_state(current_ir_graph) != phase_building);

	if (current_ir_graph->outs_state != outs_none)
		free_irg_outs(current_ir_graph);

	/* This first iteration counts the overall number of out edges and the
	   number of out edges for each node. */
	n_out_edges = count_outs(irg);

	/* allocate memory for all out edges. */
	irg->outs = xcalloc(n_out_edges, sizeof(irg->outs[0]));
#ifdef DEBUG_libfirm
	irg->n_outs = n_out_edges;
#endif /* defined DEBUG_libfirm */

	/* The second iteration splits the irg->outs array into smaller arrays
	   for each node and writes the back edges into this array. */
	end = set_out_edges(irg, irg->outs);

	/* Check how much memory we have used */
	assert (end == (irg->outs + n_out_edges));

	/* We want that the out of ProjX from Start contains the next block at
	   position 1, the Start block at position 2.  This is necessary for
	   the out block walker. */
	fix_start_proj(irg);

	current_ir_graph->outs_state = outs_consistent;
	current_ir_graph = rem;
}

void assure_irg_outs(ir_graph *irg) {
	if (get_irg_outs_state(irg) != outs_consistent)
		compute_irg_outs(irg);
}

void compute_irp_outs(void) {
	int i;
	for (i = get_irp_n_irgs() -1; i >= 0; --i)
		compute_irg_outs(get_irp_irg(i));
}

void free_irp_outs(void) {
	int i;
	for (i = get_irp_n_irgs() -1; i >= 0; --i)
		free_irg_outs(get_irp_irg(i));
}


/*------------------------------------------------------------*
 *  This computes the outedges for in interprocedural graph.  *
 *  There is one quirk:                                       *
 *  The number of the outedges for each node is saved in      *
 *  the first member of the ir_node** array. Maybe we should  *
 *  change this to make it more portable...                   *
 *------------------------------------------------------------*/


#ifdef INTERPROCEDURAL_VIEW
/**
 * Inits the number of outedges for each node
 * before counting.
 */
static void init_count(ir_node * node, void *env) {
	(void) env;
	node->out = (ir_node **) 1; /* 1 for the array size */
}


/**
 * Adjusts the out edge count for its predecessors
 * and adds the current arity to the overall count,
 * which is saved in "env"
 */
static void node_arity_count(ir_node * node, void * env) {
	int *anz = (int *) env, arity, n_outs, i, start;
	ir_node *succ;

	arity = get_irn_arity(node);
	start = (is_Block(node)) ? 0 : -1;

	n_outs = 1 + arity + (-start);  // ((is_Block(node)) ? 0 : 1);   // Why + 1??
	*anz += n_outs;

	for(i = start; i < arity; i++) {
		succ = get_irn_n(node, i);
		succ->out = (ir_node **)INT_TO_PTR(PTR_TO_INT(succ->out) + 1);
	}
}

/*
 * Inits all nodes for setting the outedges
 * Returns the overall count of edges
 */
int count_ip_outs(void) {
	int res = 0;

	cg_walk(init_count, node_arity_count, &res);

	return(res);
}

static int dummy_count = 0, global_count; /* Only for debugging */

/**
 * For each node: Sets the pointer to array
 * in which the outedges are written later.
 * The current array start is transported in env
 */
static void set_array_pointer(ir_node *node, void *env) {
	int n_outs;
	ir_node ***free = (ir_node ***) env;

	/* Allocate my array */
	n_outs = PTR_TO_INT(node->out);  /* We wrote the count here in count_ip_outs */
	dummy_count += n_outs;
	assert(dummy_count <= global_count && "More outedges than initially counted!");
	node -> out = *free;
	*free = &((*free)[n_outs]);
	/* We count the successors again, the space will be sufficient.
	   We use this counter to remember the position for the next back
	   edge. */
	node -> out[0] = (ir_node *) 0;
}


/**
 * Adds an outedge from the predecessor to the
 * current node.
 */
static void set_out_pointer(ir_node * node, void *env) {
	int i, arity = get_irn_arity(node);
	ir_node *succ;
	int start = (!is_Block(node)) ? -1 : 0;
	(void) env;

	for (i = start; i < arity; ++i) {
		succ = get_irn_n(node, i);
		succ->out[get_irn_n_outs(succ)+1] = node;
		succ->out[0] = INT_TO_PTR(get_irn_n_outs(succ) + 1);
	}
}


/*
 * Sets the outedges for all nodes.
 */
void set_ip_outs(void) {
	ir_node **outedge_array = get_irp_ip_outedges();
	cg_walk(set_array_pointer, set_out_pointer, (void *) &outedge_array);
}



/*
 * Counts the outedges, allocates memory to save the
 * outedges and fills this outedge array in interprocedural
 * view!
 */
void compute_ip_outs(void) {
	int n_out_edges;
	ir_node **out_edges;

	assert(get_irp_ip_view_state() == ip_view_valid &&
	 "Cannot construct outs for invalid ip view.");

	if (irp->outs_state != outs_none) {
		free_ip_outs();
	}

	global_count = n_out_edges = count_ip_outs();
	out_edges = xcalloc(n_out_edges, sizeof(out_edges[0]));
	set_irp_ip_outedges(out_edges);
	set_ip_outs();
}

void free_ip_outs(void) {
	ir_node **out_edges = get_irp_ip_outedges();
	if (out_edges != NULL) {
		free(out_edges);
		set_irp_ip_outedges(NULL);
	}
	irp->outs_state = outs_none;
}
#endif


void free_irg_outs(ir_graph *irg) {
	/*   current_ir_graph->outs_state = outs_none; */
	irg->outs_state = outs_none;

	if (irg->outs) {
#ifdef DEBUG_libfirm
		memset(irg->outs, 0, irg->n_outs);
#endif /* defined DEBUG_libfirm */
		free(irg->outs);
		irg->outs = NULL;
#ifdef DEBUG_libfirm
		irg->n_outs = 0;
#endif /* defined DEBUG_libfirm */
	}

#ifdef DEBUG_libfirm
	/* when debugging, *always* reset all nodes' outs!  irg->outs might
	   have been lying to us */
	irg_walk_graph (irg, reset_outs, NULL, NULL);
#endif /* defined DEBUG_libfirm */
}
