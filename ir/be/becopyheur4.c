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
 * @brief       Simple copy minimization heuristics.
 * @author      Christian Wuerdig
 * @date        27.04.2007
 * @version     $Id$
 *
 * This is the C implementation of the mst algorithm
 * originally written in Java by Sebastian Hack.
 * (also known as "heur3" :)
 * Performs simple copy minimization.
 */
#include "config.h"

#define DISABLE_STATEV

#include <float.h>

#include "array.h"
#include "irnode_t.h"
#include "bitset.h"
#include "raw_bitset.h"
#include "irphase_t.h"
#include "pqueue.h"
#include "xmalloc.h"
#include "pdeq.h"
#include "pset.h"
#include "irprintf.h"
#include "irbitset.h"
#include "error.h"
#include "list.h"
#include "statev.h"

#include "irbitset.h"

#include "bearch.h"
#include "beifg.h"
#include "be_t.h"
#include "becopyopt_t.h"
#include "bemodule.h"


#define COL_COST_INFEASIBLE       DBL_MAX
#define AFF_NEIGHBOUR_FIX_BENEFIT 128.0
#define NEIGHBOUR_CONSTR_COSTS    64.0


#ifdef DEBUG_libfirm

#define DBG_AFF_CHUNK(env, level, chunk) do { if (firm_dbg_get_mask(dbg) & (level)) dbg_aff_chunk((env), (chunk)); } while (0)
#define DBG_COL_COST(env, level, cost)   do { if (firm_dbg_get_mask(dbg) & (level)) dbg_col_cost((env), (cost)); } while (0)

static firm_dbg_module_t *dbg = NULL;

#else

#define DBG_AFF_CHUNK(env, level, chunk)
#define DBG_COL_COST(env, level, cost)

#endif

typedef float real_t;
#define REAL(C)   (C ## f)

static unsigned last_chunk_id   = 0;
static int recolor_limit        = 7;
static real_t dislike_influence = REAL(0.1);

typedef struct _col_cost_t {
	int     col;
	real_t  cost;
} col_cost_t;

/**
 * An affinity chunk.
 */
typedef struct _aff_chunk_t {
	const ir_node    **n;                   /**< An ARR_F containing all nodes of the chunk. */
	const ir_node  **interfere;             /**< An ARR_F containing all inference. */
	int              weight;                /**< Weight of this chunk */
	unsigned         weight_consistent : 1; /**< Set if the weight is consistent. */
	unsigned         deleted           : 1; /**< For debugging: Set if the was deleted. */
	unsigned         id;                    /**< An id of this chunk. */
	unsigned         visited;
	col_cost_t       color_affinity[1];
} aff_chunk_t;

/**
 * An affinity edge.
 */
typedef struct _aff_edge_t {
	const ir_node *src;                   /**< Source node. */
	const ir_node *tgt;                   /**< Target node. */
	int           weight;                 /**< The weight of this edge. */
} aff_edge_t;

/* main coalescing environment */
typedef struct _co_mst_env_t {
	int              n_regs;         /**< number of regs in class */
	int              k;              /**< number of non-ignore registers in class */
	bitset_t         *ignore_regs;   /**< set containing all global ignore registers */
	ir_phase         ph;             /**< phase object holding data for nodes */
	pqueue_t         *chunks;        /**< priority queue for chunks */
	pset             *chunkset;      /**< set holding all chunks */
	be_ifg_t         *ifg;           /**< the interference graph */
	copy_opt_t       *co;            /**< the copy opt object */
	unsigned         chunk_visited;
	col_cost_t      **single_cols;
} co_mst_env_t;

/* stores coalescing related information for a node */
typedef struct _co_mst_irn_t {
	const ir_node    *irn;              /**< the irn this information belongs to */
	aff_chunk_t      *chunk;            /**< the chunk this irn belongs to */
	bitset_t         *adm_colors;       /**< set of admissible colors for this irn */
	ir_node          **int_neighs;      /**< array of all interfering neighbours (cached for speed reasons) */
	int              n_neighs;          /**< length of the interfering neighbours array. */
	int              int_aff_neigh;     /**< number of interfering affinity neighbours */
	int              col;               /**< color currently assigned */
	int              init_col;          /**< the initial color */
	int              tmp_col;           /**< a temporary assigned color */
	unsigned         fixed     : 1;     /**< the color is fixed */
	struct list_head list;              /**< Queue for coloring undo. */
	real_t           constr_factor;
} co_mst_irn_t;

#define get_co_mst_irn(mst_env, irn) (phase_get_or_set_irn_data(&(mst_env)->ph, (irn)))

typedef int decide_func_t(const co_mst_irn_t *node, int col);

#ifdef DEBUG_libfirm

/**
 * Write a chunk to stderr for debugging.
 */
static void dbg_aff_chunk(const co_mst_env_t *env, const aff_chunk_t *c)
{
	int i, l;
	(void) env;
	if (c->weight_consistent)
		ir_fprintf(stderr, " $%d ", c->weight);
	ir_fprintf(stderr, "{");
	for (i = 0, l = ARR_LEN(c->n); i < l; ++i) {
		const ir_node *n = c->n[i];
		ir_fprintf(stderr, " %+F,", n);
	}
	ir_fprintf(stderr, "}");
}

/**
 * Dump all admissible colors to stderr.
 */
static void dbg_admissible_colors(const co_mst_env_t *env, const co_mst_irn_t *node)
{
	unsigned idx;
	(void) env;

	if (bitset_popcount(node->adm_colors) < 1)
		fprintf(stderr, "no admissible colors?!?");
	else {
		bitset_foreach(node->adm_colors, idx) {
			fprintf(stderr, " %d", idx);
		}
	}
}

/**
 * Dump color-cost pairs to stderr.
 */
static void dbg_col_cost(const co_mst_env_t *env, const col_cost_t *cost)
{
	int i;
	for (i = 0; i < env->n_regs; ++i)
		fprintf(stderr, " (%d, %.4f)", cost[i].col, cost[i].cost);
}

#endif /* DEBUG_libfirm */

static inline int get_mst_irn_col(const co_mst_irn_t *node)
{
	return node->tmp_col >= 0 ? node->tmp_col : node->col;
}

/**
 * @return 1 if node @p node has color @p col, 0 otherwise.
 */
static int decider_has_color(const co_mst_irn_t *node, int col)
{
	return get_mst_irn_col(node) == col;
}

/**
 * @return 1 if node @p node has not color @p col, 0 otherwise.
 */
static int decider_hasnot_color(const co_mst_irn_t *node, int col)
{
	return get_mst_irn_col(node) != col;
}

/**
 * Always returns true.
 */
static int decider_always_yes(const co_mst_irn_t *node, int col)
{
	(void) node;
	(void) col;
	return 1;
}

/** compares two affinity edges by its weight */
static int cmp_aff_edge(const void *a, const void *b)
{
	const aff_edge_t *e1 = a;
	const aff_edge_t *e2 = b;

	if (e2->weight == e1->weight) {
		if (e2->src->node_idx == e1->src->node_idx)
			return QSORT_CMP(e2->tgt->node_idx, e1->tgt->node_idx);
		else
			return QSORT_CMP(e2->src->node_idx, e1->src->node_idx);
	}
	/* sort in descending order */
	return QSORT_CMP(e2->weight, e1->weight);
}

/** compares to color-cost pairs */
static __attribute__((unused)) int cmp_col_cost_lt(const void *a, const void *b)
{
	const col_cost_t *c1 = a;
	const col_cost_t *c2 = b;
	real_t diff = c1->cost - c2->cost;
	return (diff > 0) - (diff < 0);
}

static int cmp_col_cost_gt(const void *a, const void *b)
{
	const col_cost_t *c1 = a;
	const col_cost_t *c2 = b;
	real_t diff = c2->cost - c1->cost;
	return (diff > 0) - (diff < 0);
}

/**
 * Creates a new affinity chunk
 */
static inline aff_chunk_t *new_aff_chunk(co_mst_env_t *env)
{
	aff_chunk_t *c = XMALLOCF(aff_chunk_t, color_affinity, env->n_regs);
	c->n                 = NEW_ARR_F(const ir_node *, 0);
	c->interfere         = NEW_ARR_F(const ir_node *, 0);
	c->weight            = -1;
	c->weight_consistent = 0;
	c->deleted           = 0;
	c->id                = ++last_chunk_id;
	c->visited           = 0;
	pset_insert(env->chunkset, c, c->id);
	return c;
}

/**
 * Frees all memory allocated by an affinity chunk.
 */
static inline void delete_aff_chunk(co_mst_env_t *env, aff_chunk_t *c)
{
	pset_remove(env->chunkset, c, c->id);
	DEL_ARR_F(c->interfere);
	DEL_ARR_F(c->n);
	c->deleted = 1;
	free(c);
}

/**
 * binary search of sorted nodes.
 *
 * @return the position where n is found in the array arr or ~pos
 * if the nodes is not here.
 */
static inline int nodes_bsearch(const ir_node **arr, const ir_node *n)
{
	int hi = ARR_LEN(arr);
	int lo = 0;

	while (lo < hi) {
		int md = lo + ((hi - lo) >> 1);

		if (arr[md] == n)
			return md;
		if (arr[md] < n)
			lo = md + 1;
		else
			hi = md;
	}

	return ~lo;
}

/** Check if a node n can be found inside arr. */
static int node_contains(const ir_node **arr, const ir_node *n)
{
	int i = nodes_bsearch(arr, n);
	return i >= 0;
}

/**
 * Insert a node into the sorted nodes list.
 *
 * @return 1 if the node was inserted, 0 else
 */
static int nodes_insert(const ir_node ***arr, const ir_node *irn)
{
	int idx = nodes_bsearch(*arr, irn);

	if (idx < 0) {
		int i, n = ARR_LEN(*arr);
		const ir_node **l;

		ARR_APP1(const ir_node *, *arr, irn);

		/* move it */
		idx = ~idx;
		l = *arr;
		for (i = n - 1; i >= idx; --i)
			l[i + 1] = l[i];
		l[idx] = irn;
		return 1;
	}
	return 0;
}

/**
 * Adds a node to an affinity chunk
 */
static inline void aff_chunk_add_node(aff_chunk_t *c, co_mst_irn_t *node)
{
	int i;

	if (! nodes_insert(&c->n, node->irn))
		return;

	c->weight_consistent = 0;
	node->chunk          = c;

	for (i = node->n_neighs - 1; i >= 0; --i) {
		ir_node *neigh = node->int_neighs[i];
		nodes_insert(&c->interfere, neigh);
	}
}

/**
 * In case there is no phase information for irn, initialize it.
 */
static void *co_mst_irn_init(ir_phase *ph, const ir_node *irn, void *old)
{
	co_mst_irn_t *res = old ? old : phase_alloc(ph, sizeof(res[0]));
	co_mst_env_t *env = ph->priv;

	if (!old) {
		const arch_register_req_t *req;
		neighbours_iter_t nodes_it;
		ir_node  *neigh;
		unsigned len;

		res->irn           = irn;
		res->chunk         = NULL;
		res->fixed         = 0;
		res->tmp_col       = -1;
		res->int_neighs    = NULL;
		res->int_aff_neigh = 0;
		res->col           = arch_register_get_index(arch_get_irn_register(irn));
		res->init_col      = res->col;
		INIT_LIST_HEAD(&res->list);

		DB((dbg, LEVEL_4, "Creating phase info for %+F\n", irn));

		/* set admissible registers */
		res->adm_colors = bitset_obstack_alloc(phase_obst(ph), env->n_regs);

		/* Exclude colors not assignable to the irn */
		req = arch_get_register_req_out(irn);
		if (arch_register_req_is(req, limited))
			rbitset_copy_to_bitset(req->limited, res->adm_colors);
		else
			bitset_set_all(res->adm_colors);

		/* exclude global ignore registers as well */
		bitset_andnot(res->adm_colors, env->ignore_regs);

		/* compute the constraint factor */
		res->constr_factor = (real_t) (1 + env->n_regs - bitset_popcount(res->adm_colors)) / env->n_regs;

		/* set the number of interfering affinity neighbours to -1, they are calculated later */
		res->int_aff_neigh = -1;

		/* build list of interfering neighbours */
		len = 0;
		be_ifg_foreach_neighbour(env->ifg, &nodes_it, irn, neigh) {
			if (!arch_irn_is_ignore(neigh)) {
				obstack_ptr_grow(phase_obst(ph), neigh);
				++len;
			}
		}
		res->int_neighs = obstack_finish(phase_obst(ph));
		res->n_neighs   = len;
	}
	return res;
}

/**
 * Check if affinity chunk @p chunk interferes with node @p irn.
 */
static inline int aff_chunk_interferes(const aff_chunk_t *chunk, const ir_node *irn)
{
	return node_contains(chunk->interfere, irn);
}

/**
 * Check if there are interference edges from c1 to c2.
 * @param c1    A chunk
 * @param c2    Another chunk
 * @return 1 if there are interferences between nodes of c1 and c2, 0 otherwise.
 */
static inline int aff_chunks_interfere(const aff_chunk_t *c1, const aff_chunk_t *c2)
{
	int i;

	if (c1 == c2)
		return 0;

	/* check if there is a node in c2 having an interfering neighbor in c1 */
	for (i = ARR_LEN(c2->n) - 1; i >= 0; --i) {
		const ir_node *irn = c2->n[i];

		if (node_contains(c1->interfere, irn))
			return 1;
	}
	return 0;
}

/**
 * Returns the affinity chunk of @p irn or creates a new
 * one with @p irn as element if there is none assigned.
 */
static inline aff_chunk_t *get_aff_chunk(co_mst_env_t *env, const ir_node *irn)
{
	co_mst_irn_t *node = get_co_mst_irn(env, irn);
	return node->chunk;
}

/**
 * Let chunk(src) absorb the nodes of chunk(tgt) (only possible when there
 * are no interference edges from chunk(src) to chunk(tgt)).
 * @return 1 if successful, 0 if not possible
 */
static int aff_chunk_absorb(co_mst_env_t *env, const ir_node *src, const ir_node *tgt)
{
	aff_chunk_t *c1 = get_aff_chunk(env, src);
	aff_chunk_t *c2 = get_aff_chunk(env, tgt);

#ifdef DEBUG_libfirm
		DB((dbg, LEVEL_4, "Attempt to let c1 (id %u): ", c1 ? c1->id : 0));
		if (c1) {
			DBG_AFF_CHUNK(env, LEVEL_4, c1);
		} else {
			DB((dbg, LEVEL_4, "{%+F}", src));
		}
		DB((dbg, LEVEL_4, "\n\tabsorb c2 (id %u): ", c2 ? c2->id : 0));
		if (c2) {
			DBG_AFF_CHUNK(env, LEVEL_4, c2);
		} else {
			DB((dbg, LEVEL_4, "{%+F}", tgt));
		}
		DB((dbg, LEVEL_4, "\n"));
#endif

	if (c1 == NULL) {
		if (c2 == NULL) {
			/* no chunk exists */
			co_mst_irn_t *mirn = get_co_mst_irn(env, src);
			int i;

			for (i = mirn->n_neighs - 1; i >= 0; --i) {
				if (mirn->int_neighs[i] == tgt)
					break;
			}
			if (i < 0) {
				/* create one containing both nodes */
				c1 = new_aff_chunk(env);
				aff_chunk_add_node(c1, get_co_mst_irn(env, src));
				aff_chunk_add_node(c1, get_co_mst_irn(env, tgt));
				goto absorbed;
			}
		} else {
			/* c2 already exists */
			if (! aff_chunk_interferes(c2, src)) {
				aff_chunk_add_node(c2, get_co_mst_irn(env, src));
				goto absorbed;
			}
		}
	} else if (c2 == NULL) {
		/* c1 already exists */
		if (! aff_chunk_interferes(c1, tgt)) {
			aff_chunk_add_node(c1, get_co_mst_irn(env, tgt));
			goto absorbed;
		}
	} else if (c1 != c2 && ! aff_chunks_interfere(c1, c2)) {
		int idx, len;

		for (idx = 0, len = ARR_LEN(c2->n); idx < len; ++idx)
			aff_chunk_add_node(c1, get_co_mst_irn(env, c2->n[idx]));

		for (idx = 0, len = ARR_LEN(c2->interfere); idx < len; ++idx) {
			const ir_node *irn = c2->interfere[idx];
			nodes_insert(&c1->interfere, irn);
		}

		c1->weight_consistent = 0;

		delete_aff_chunk(env, c2);
		goto absorbed;
	}
	DB((dbg, LEVEL_4, " ... c1 interferes with c2, skipped\n"));
	return 0;

absorbed:
	DB((dbg, LEVEL_4, " ... absorbed\n"));
	return 1;
}

/**
 * Assures that the weight of the given chunk is consistent.
 */
static void aff_chunk_assure_weight(co_mst_env_t *env, aff_chunk_t *c)
{
	if (! c->weight_consistent) {
		int w = 0;
		int idx, len, i;

		for (i = 0; i < env->n_regs; ++i) {
			c->color_affinity[i].col = i;
			c->color_affinity[i].cost = REAL(0.0);
		}

		for (idx = 0, len = ARR_LEN(c->n); idx < len; ++idx) {
			const ir_node         *n       = c->n[idx];
			const affinity_node_t *an      = get_affinity_info(env->co, n);
			co_mst_irn_t          *node    = get_co_mst_irn(env, n);

			node->chunk = c;
			if (node->constr_factor > REAL(0.0)) {
				unsigned col;
				bitset_foreach (node->adm_colors, col)
					c->color_affinity[col].cost += node->constr_factor;
			}

			if (an != NULL) {
				neighb_t *neigh;
				co_gs_foreach_neighb(an, neigh) {
					const ir_node *m = neigh->irn;

					if (arch_irn_is_ignore(m))
						continue;

					w += node_contains(c->n, m) ? neigh->costs : 0;
				}
			}
		}

		for (i = 0; i < env->n_regs; ++i)
			c->color_affinity[i].cost *= (REAL(1.0) / ARR_LEN(c->n));

		c->weight            = w;
		// c->weight            = bitset_popcount(c->nodes);
		c->weight_consistent = 1;
	}
}

/**
 * Count the number of interfering affinity neighbours
 */
static int count_interfering_aff_neighs(co_mst_env_t *env, const affinity_node_t *an)
{
	const neighb_t     *neigh;
	const ir_node      *irn  = an->irn;
	const co_mst_irn_t *node = get_co_mst_irn(env, irn);
	int                res   = 0;

	co_gs_foreach_neighb(an, neigh) {
		const ir_node *n = neigh->irn;
		int           i;

		if (arch_irn_is_ignore(n))
			continue;

		/* check if the affinity neighbour interfere */
		for (i = 0; i < node->n_neighs; ++i) {
			if (node->int_neighs[i] == n) {
				++res;
				break;
			}
		}
	}
	return res;
}


/**
 * Build chunks of nodes connected by affinity edges.
 * We start at the heaviest affinity edge.
 * The chunks of the two edge-defining nodes will be
 * merged if there are no interference edges from one
 * chunk to the other.
 */
static void build_affinity_chunks(co_mst_env_t *env)
{
	nodes_iter_t nodes_it;
	aff_edge_t  *edges    = NEW_ARR_F(aff_edge_t, 0);
	ir_node     *n;
	int         i, len;
	aff_chunk_t *curr_chunk;

	/* at first we create the affinity edge objects */
	be_ifg_foreach_node(env->ifg, &nodes_it, n) {
		int             n_idx = get_irn_idx(n);
		co_mst_irn_t    *n1;
		affinity_node_t *an;

		if (arch_irn_is_ignore(n))
			continue;

		n1 = get_co_mst_irn(env, n);
		an = get_affinity_info(env->co, n);

		if (an != NULL) {
			neighb_t *neigh;

			if (n1->int_aff_neigh < 0)
				n1->int_aff_neigh = count_interfering_aff_neighs(env, an);

			/* build the affinity edges */
			co_gs_foreach_neighb(an, neigh) {
				const ir_node *m     = neigh->irn;
				int            m_idx = get_irn_idx(m);

				/* record the edge in only one direction */
				if (n_idx < m_idx) {
					co_mst_irn_t *n2;
					aff_edge_t   edge;

					/* skip ignore nodes */
					if (arch_irn_is_ignore(m))
						continue;

					edge.src = n;
					edge.tgt = m;

					n2 = get_co_mst_irn(env, m);
					if (n2->int_aff_neigh < 0) {
						affinity_node_t *am = get_affinity_info(env->co, m);
						n2->int_aff_neigh = count_interfering_aff_neighs(env, am);
					}
					/*
					 * these weights are pure hackery ;-).
					 * It's not chriswue's fault but mine.
					 */
					edge.weight = neigh->costs;
					ARR_APP1(aff_edge_t, edges, edge);
				}
			}
		}
	}

	/* now: sort edges and build the affinity chunks */
	len = ARR_LEN(edges);
	qsort(edges, len, sizeof(edges[0]), cmp_aff_edge);
	for (i = 0; i < len; ++i) {
		DBG((dbg, LEVEL_1, "edge (%u,%u) %f\n", edges[i].src->node_idx, edges[i].tgt->node_idx, edges[i].weight));

		(void)aff_chunk_absorb(env, edges[i].src, edges[i].tgt);
	}

	/* now insert all chunks into a priority queue */
	foreach_pset(env->chunkset, curr_chunk) {
		aff_chunk_assure_weight(env, curr_chunk);

		DBG((dbg, LEVEL_1, "entry #%u", curr_chunk->id));
		DBG_AFF_CHUNK(env, LEVEL_1, curr_chunk);
		DBG((dbg, LEVEL_1, "\n"));

		pqueue_put(env->chunks, curr_chunk, curr_chunk->weight);
	}

	foreach_phase_irn(&env->ph, n) {
		co_mst_irn_t *mirn = get_co_mst_irn(env, n);

		if (mirn->chunk == NULL) {
			/* no chunk is allocated so far, do it now */
			aff_chunk_t *curr_chunk = new_aff_chunk(env);
			aff_chunk_add_node(curr_chunk, mirn);

			aff_chunk_assure_weight(env, curr_chunk);

			DBG((dbg, LEVEL_1, "entry #%u", curr_chunk->id));
			DBG_AFF_CHUNK(env, LEVEL_1, curr_chunk);
			DBG((dbg, LEVEL_1, "\n"));

			pqueue_put(env->chunks, curr_chunk, curr_chunk->weight);
		}
	}

	DEL_ARR_F(edges);
}

static __attribute__((unused)) void chunk_order_nodes(co_mst_env_t *env, aff_chunk_t *chunk)
{
	pqueue_t *grow = new_pqueue();
	const ir_node *max_node = NULL;
	int max_weight = 0;
	int i;

	for (i = ARR_LEN(chunk->n) - 1; i >= 0; i--) {
		const ir_node   *irn = chunk->n[i];
		affinity_node_t *an  = get_affinity_info(env->co, irn);
		int w = 0;
		neighb_t *neigh;

		if (arch_irn_is_ignore(irn))
			continue;

		if (an) {
			co_gs_foreach_neighb(an, neigh)
				w += neigh->costs;

			if (w > max_weight) {
				max_weight = w;
				max_node   = irn;
			}
		}
	}

	if (max_node) {
		bitset_t *visited = bitset_irg_malloc(env->co->irg);

		for (i = ARR_LEN(chunk->n) - 1; i >= 0; --i)
			bitset_add_irn(visited, chunk->n[i]);

		pqueue_put(grow, (void *) max_node, max_weight);
		bitset_remv_irn(visited, max_node);
		i = 0;
		while (!pqueue_empty(grow)) {
			ir_node *irn = pqueue_pop_front(grow);
			affinity_node_t *an = get_affinity_info(env->co, irn);
			neighb_t        *neigh;

			if (arch_irn_is_ignore(irn))
				continue;

			assert(i <= ARR_LEN(chunk->n));
			chunk->n[i++] = irn;

			assert(an);

			/* build the affinity edges */
			co_gs_foreach_neighb(an, neigh) {
				co_mst_irn_t *node = get_co_mst_irn(env, neigh->irn);

				if (bitset_contains_irn(visited, node->irn)) {
					pqueue_put(grow, (void *) neigh->irn, neigh->costs);
					bitset_remv_irn(visited, node->irn);
				}
			}
		}

		del_pqueue(grow);
		bitset_free(visited);
	}
}

/**
 * Greedy collect affinity neighbours into thew new chunk @p chunk starting at node @p node.
 */
static void expand_chunk_from(co_mst_env_t *env, co_mst_irn_t *node, bitset_t *visited,
	aff_chunk_t *chunk, aff_chunk_t *orig_chunk, decide_func_t *decider, int col)
{
	waitq *nodes = new_waitq();

	DBG((dbg, LEVEL_1, "\n\tExpanding new chunk (#%u) from %+F, color %d:", chunk->id, node->irn, col));

	/* init queue and chunk */
	waitq_put(nodes, node);
	bitset_set(visited, get_irn_idx(node->irn));
	aff_chunk_add_node(chunk, node);
	DB((dbg, LEVEL_1, " %+F", node->irn));

	/* as long as there are nodes in the queue */
	while (! waitq_empty(nodes)) {
		co_mst_irn_t    *n  = waitq_get(nodes);
		affinity_node_t *an = get_affinity_info(env->co, n->irn);

		/* check all affinity neighbors */
		if (an != NULL) {
			neighb_t *neigh;
			co_gs_foreach_neighb(an, neigh) {
				const ir_node *m    = neigh->irn;
				int            m_idx = get_irn_idx(m);
				co_mst_irn_t *n2;

				if (arch_irn_is_ignore(m))
					continue;

				n2 = get_co_mst_irn(env, m);

				if (! bitset_is_set(visited, m_idx)  &&
					decider(n2, col)                 &&
					! n2->fixed                      &&
					! aff_chunk_interferes(chunk, m) &&
					node_contains(orig_chunk->n, m))
				{
					/*
						following conditions are met:
						- neighbour is not visited
						- neighbour likes the color
						- neighbour has not yet a fixed color
						- the new chunk doesn't interfere with the neighbour
						- neighbour belongs or belonged once to the original chunk
					*/
					bitset_set(visited, m_idx);
					aff_chunk_add_node(chunk, n2);
					DB((dbg, LEVEL_1, " %+F", n2->irn));
					/* enqueue for further search */
					waitq_put(nodes, n2);
				}
			}
		}
	}

	DB((dbg, LEVEL_1, "\n"));

	del_waitq(nodes);
}

/**
 * Fragment the given chunk into chunks having given color and not having given color.
 */
static aff_chunk_t *fragment_chunk(co_mst_env_t *env, int col, aff_chunk_t *c, waitq *tmp)
{
	bitset_t    *visited = bitset_irg_malloc(env->co->irg);
	int         idx, len;
	aff_chunk_t *best = NULL;

	for (idx = 0, len = ARR_LEN(c->n); idx < len; ++idx) {
		const ir_node *irn;
		co_mst_irn_t  *node;
		aff_chunk_t   *tmp_chunk;
		decide_func_t *decider;
		int           check_for_best;

		irn = c->n[idx];
		if (bitset_is_set(visited, get_irn_idx(irn)))
			continue;

		node = get_co_mst_irn(env, irn);

		if (get_mst_irn_col(node) == col) {
			decider        = decider_has_color;
			check_for_best = 1;
			DBG((dbg, LEVEL_4, "\tcolor %d wanted\n", col));
		}
		else {
			decider        = decider_hasnot_color;
			check_for_best = 0;
			DBG((dbg, LEVEL_4, "\tcolor %d forbidden\n", col));
		}

		/* create a new chunk starting at current node */
		tmp_chunk = new_aff_chunk(env);
		waitq_put(tmp, tmp_chunk);
		expand_chunk_from(env, node, visited, tmp_chunk, c, decider, col);
		assert(ARR_LEN(tmp_chunk->n) > 0 && "No nodes added to chunk");

		/* remember the local best */
		aff_chunk_assure_weight(env, tmp_chunk);
		if (check_for_best && (! best || best->weight < tmp_chunk->weight))
			best = tmp_chunk;
	}

	assert(best && "No chunk found?");
	bitset_free(visited);
	return best;
}

/**
 * Resets the temporary fixed color of all nodes within wait queue @p nodes.
 * ATTENTION: the queue is empty after calling this function!
 */
static inline void reject_coloring(struct list_head *nodes)
{
	co_mst_irn_t *n, *temp;
	DB((dbg, LEVEL_4, "\treject coloring for"));
	list_for_each_entry_safe(co_mst_irn_t, n, temp, nodes, list) {
		DB((dbg, LEVEL_4, " %+F", n->irn));
		assert(n->tmp_col >= 0);
		n->tmp_col = -1;
		list_del_init(&n->list);
	}
	DB((dbg, LEVEL_4, "\n"));
}

static inline void materialize_coloring(struct list_head *nodes)
{
	co_mst_irn_t *n, *temp;
	list_for_each_entry_safe(co_mst_irn_t, n, temp, nodes, list) {
		assert(n->tmp_col >= 0);
		n->col     = n->tmp_col;
		n->tmp_col = -1;
		list_del_init(&n->list);
	}
}

static inline void set_temp_color(co_mst_irn_t *node, int col, struct list_head *changed)
{
	assert(col >= 0);
	assert(!node->fixed);
	assert(node->tmp_col < 0);
	assert(node->list.next == &node->list && node->list.prev == &node->list);
	assert(bitset_is_set(node->adm_colors, col));

	list_add_tail(&node->list, changed);
	node->tmp_col = col;
}

static inline int is_loose(co_mst_irn_t *node)
{
	return !node->fixed && node->tmp_col < 0;
}

/**
 * Determines the costs for each color if it would be assigned to node @p node.
 */
static void determine_color_costs(co_mst_env_t *env, co_mst_irn_t *node, col_cost_t *costs)
{
	int   *neigh_cols = ALLOCAN(int, env->n_regs);
	int    n_loose    = 0;
	real_t coeff;
	int    i;

	for (i = 0; i < env->n_regs; ++i) {
		neigh_cols[i] = 0;
		costs[i].col = i;
		costs[i].cost = bitset_is_set(node->adm_colors, i) ? node->constr_factor : REAL(0.0);
	}

	for (i = 0; i < node->n_neighs; ++i) {
		co_mst_irn_t *n = get_co_mst_irn(env, node->int_neighs[i]);
		int col = get_mst_irn_col(n);
		if (is_loose(n)) {
			++n_loose;
			++neigh_cols[col];
		} else
			costs[col].cost = REAL(0.0);
	}

	if (n_loose > 0) {
		coeff = REAL(1.0) / n_loose;
		for (i = 0; i < env->n_regs; ++i)
			costs[i].cost *= REAL(1.0) - coeff * neigh_cols[i];
	}
}

/* need forward declaration due to recursive call */
static int recolor_nodes(co_mst_env_t *env, co_mst_irn_t *node, col_cost_t *costs, struct list_head *changed_ones, int depth, int *max_depth, int *trip);

/**
 * Tries to change node to a color but @p explude_col.
 * @return 1 if succeeded, 0 otherwise.
 */
static int change_node_color_excluded(co_mst_env_t *env, co_mst_irn_t *node, int exclude_col, struct list_head *changed, int depth, int *max_depth, int *trip)
{
	int col = get_mst_irn_col(node);
	int res = 0;

	/* neighbours has already a different color -> good, temporary fix it */
	if (col != exclude_col) {
		if (is_loose(node))
			set_temp_color(node, col, changed);
		return 1;
	}

	/* The node has the color it should not have _and_ has not been visited yet. */
	if (is_loose(node)) {
		col_cost_t *costs = ALLOCAN(col_cost_t, env->n_regs);

		/* Get the costs for giving the node a specific color. */
		determine_color_costs(env, node, costs);

		/* Since the node must not have the not_col, set the costs for that color to "infinity" */
		costs[exclude_col].cost = REAL(0.0);

		/* sort the colors according costs, cheapest first. */
		qsort(costs, env->n_regs, sizeof(costs[0]), cmp_col_cost_gt);

		/* Try recoloring the node using the color list. */
		res = recolor_nodes(env, node, costs, changed, depth + 1, max_depth, trip);
	}

	return res;
}

/**
 * Tries to bring node @p node to cheapest color and color all interfering neighbours with other colors.
 * ATTENTION: Expect @p costs already sorted by increasing costs.
 * @return 1 if coloring could be applied, 0 otherwise.
 */
static int recolor_nodes(co_mst_env_t *env, co_mst_irn_t *node, col_cost_t *costs, struct list_head *changed, int depth, int *max_depth, int *trip)
{
	int   i;
	struct list_head local_changed;

	++*trip;
	if (depth > *max_depth)
		*max_depth = depth;

	DBG((dbg, LEVEL_4, "\tRecoloring %+F with color-costs", node->irn));
	DBG_COL_COST(env, LEVEL_4, costs);
	DB((dbg, LEVEL_4, "\n"));

	if (depth >= recolor_limit) {
		DBG((dbg, LEVEL_4, "\tHit recolor limit\n"));
		return 0;
	}

	for (i = 0; i < env->n_regs; ++i) {
		int tgt_col  = costs[i].col;
		int neigh_ok = 1;
		int j;

		/* If the costs for that color (and all successive) are infinite, bail out we won't make it anyway. */
		if (costs[i].cost == REAL(0.0)) {
			DBG((dbg, LEVEL_4, "\tAll further colors forbidden\n"));
			return 0;
		}

		/* Set the new color of the node and mark the node as temporarily fixed. */
		assert(node->tmp_col < 0 && "Node must not have been temporary fixed.");
		INIT_LIST_HEAD(&local_changed);
		set_temp_color(node, tgt_col, &local_changed);
		DBG((dbg, LEVEL_4, "\tTemporary setting %+F to color %d\n", node->irn, tgt_col));

		/* try to color all interfering neighbours with current color forbidden */
		for (j = 0; j < node->n_neighs; ++j) {
			co_mst_irn_t *nn;
			ir_node      *neigh;

			neigh = node->int_neighs[j];

			if (arch_irn_is_ignore(neigh))
				continue;

			nn = get_co_mst_irn(env, neigh);
			DB((dbg, LEVEL_4, "\tHandling neighbour %+F, at position %d (fixed: %d, tmp_col: %d, col: %d)\n",
				neigh, j, nn->fixed, nn->tmp_col, nn->col));

			/*
				Try to change the color of the neighbor and record all nodes which
				get changed in the tmp list. Add this list to the "changed" list for
				that color. If we did not succeed to change the color of the neighbor,
				we bail out and try the next color.
			*/
			if (get_mst_irn_col(nn) == tgt_col) {
				/* try to color neighbour with tgt_col forbidden */
				neigh_ok = change_node_color_excluded(env, nn, tgt_col, &local_changed, depth + 1, max_depth, trip);

				if (!neigh_ok)
					break;
			}
		}

		/*
			We managed to assign the target color to all neighbors, so from the perspective
			of the current node, every thing was ok and we can return safely.
		*/
		if (neigh_ok) {
			/* append the local_changed ones to global ones */
			list_splice(&local_changed, changed);
			return 1;
		}
		else {
			/* coloring of neighbours failed, so we try next color */
			reject_coloring(&local_changed);
		}
	}

	DBG((dbg, LEVEL_4, "\tAll colors failed\n"));
	return 0;
}

/**
 * Tries to bring node @p node and all it's neighbours to color @p tgt_col.
 * @return 1 if color @p col could be applied, 0 otherwise
 */
static int change_node_color(co_mst_env_t *env, co_mst_irn_t *node, int tgt_col, struct list_head *changed)
{
	int col = get_mst_irn_col(node);

	/* if node already has the target color -> good, temporary fix it */
	if (col == tgt_col) {
		DBG((dbg, LEVEL_4, "\t\tCNC: %+F has already color %d, fix temporary\n", node->irn, tgt_col));
		if (is_loose(node))
			set_temp_color(node, tgt_col, changed);
		return 1;
	}

	/*
		Node has not yet a fixed color and target color is admissible
		-> try to recolor node and it's affinity neighbours
	*/
	if (is_loose(node) && bitset_is_set(node->adm_colors, tgt_col)) {
		col_cost_t *costs = env->single_cols[tgt_col];
		int res, max_depth, trip;

		max_depth = 0;
		trip      = 0;

		DBG((dbg, LEVEL_4, "\t\tCNC: Attempt to recolor %+F ===>>\n", node->irn));
		res = recolor_nodes(env, node, costs, changed, 0, &max_depth, &trip);
		DBG((dbg, LEVEL_4, "\t\tCNC: <<=== Recoloring of %+F %s\n", node->irn, res ? "succeeded" : "failed"));
		stat_ev_int("heur4_recolor_depth_max", max_depth);
		stat_ev_int("heur4_recolor_trip", trip);


		return res;
	}

#ifdef DEBUG_libfirm
		if (firm_dbg_get_mask(dbg) & LEVEL_4) {
			if (!is_loose(node))
				DB((dbg, LEVEL_4, "\t\tCNC: %+F has already fixed color %d\n", node->irn, col));
			else {
				DB((dbg, LEVEL_4, "\t\tCNC: color %d not admissible for %+F (", tgt_col, node->irn));
				dbg_admissible_colors(env, node);
				DB((dbg, LEVEL_4, ")\n"));
			}
		}
#endif

	return 0;
}

/**
 * Tries to color an affinity chunk (or at least a part of it).
 * Inserts uncolored parts of the chunk as a new chunk into the priority queue.
 */
static void color_aff_chunk(co_mst_env_t *env, aff_chunk_t *c)
{
	aff_chunk_t *best_chunk   = NULL;
	int         n_nodes       = ARR_LEN(c->n);
	int         best_color    = -1;
	int         n_int_chunks  = 0;
	waitq       *tmp_chunks   = new_waitq();
	waitq       *best_starts  = NULL;
	col_cost_t  *order        = ALLOCANZ(col_cost_t, env->n_regs);
	bitset_t    *visited;
	int         idx, len, i, nidx, pos;
	struct list_head changed;

	DB((dbg, LEVEL_2, "fragmentizing chunk #%u", c->id));
	DBG_AFF_CHUNK(env, LEVEL_2, c);
	DB((dbg, LEVEL_2, "\n"));

	stat_ev_ctx_push_fmt("heur4_color_chunk", "%u", c->id);

	++env->chunk_visited;

	/* compute color preference */
	for (pos = 0, len = ARR_LEN(c->interfere); pos < len; ++pos) {
		const ir_node *n = c->interfere[pos];
		co_mst_irn_t *node = get_co_mst_irn(env, n);
		aff_chunk_t *chunk = node->chunk;

		if (is_loose(node) && chunk && chunk->visited < env->chunk_visited) {
			assert(!chunk->deleted);
			chunk->visited = env->chunk_visited;
			++n_int_chunks;

			aff_chunk_assure_weight(env, chunk);
			for (i = 0; i < env->n_regs; ++i)
				order[i].cost += chunk->color_affinity[i].cost;
		}
	}

	for (i = 0; i < env->n_regs; ++i) {
		real_t dislike = n_int_chunks > 0 ? REAL(1.0) - order[i].cost / n_int_chunks : REAL(0.0);
		order[i].col  = i;
		order[i].cost = (REAL(1.0) - dislike_influence) * c->color_affinity[i].cost + dislike_influence * dislike;
	}

	qsort(order, env->n_regs, sizeof(order[0]), cmp_col_cost_gt);

	DBG_COL_COST(env, LEVEL_2, order);
	DB((dbg, LEVEL_2, "\n"));

	/* check which color is the "best" for the given chunk.
	 * if we found a color which was ok for all nodes, we take it
	 * and do not look further. (see did_all flag usage below.)
	 * If we have many colors which fit all nodes it is hard to decide
	 * which one to take anyway.
	 * TODO Sebastian: Perhaps we should at all nodes and figure out
	 * a suitable color using costs as done above (determine_color_costs).
	 */
	for (i = 0; i < env->k; ++i) {
		int         col = order[i].col;
		waitq       *good_starts = new_waitq();
		aff_chunk_t *local_best;
		int          n_succeeded;

		/* skip ignore colors */
		if (bitset_is_set(env->ignore_regs, col))
			continue;

		DB((dbg, LEVEL_2, "\ttrying color %d\n", col));

		n_succeeded = 0;

		/* try to bring all nodes of given chunk to the current color. */
		for (idx = 0, len = ARR_LEN(c->n); idx < len; ++idx) {
			const ir_node   *irn  = c->n[idx];
			co_mst_irn_t    *node = get_co_mst_irn(env, irn);
			int              good;

			assert(! node->fixed && "Node must not have a fixed color.");
			DB((dbg, LEVEL_4, "\t\tBringing %+F from color %d to color %d ...\n", irn, node->col, col));

			/*
				The order of the colored nodes is important, so we record the successfully
				colored ones in the order they appeared.
			*/
			INIT_LIST_HEAD(&changed);
			stat_ev_tim_push();
			good = change_node_color(env, node, col, &changed);
			stat_ev_tim_pop("heur4_recolor");
			if (good) {
				waitq_put(good_starts, node);
				materialize_coloring(&changed);
				node->fixed = 1;
			}

			else
				reject_coloring(&changed);

			n_succeeded += good;
			DB((dbg, LEVEL_4, "\t\t... %+F attempt from %d to %d %s\n", irn, node->col, col, good ? "succeeded" : "failed"));
		}

		/* unfix all nodes */
		for (idx = 0, len = ARR_LEN(c->n); idx < len; ++idx) {
			co_mst_irn_t *node = get_co_mst_irn(env, c->n[idx]);
			node->fixed = 0;
		}

		/* try next color when failed */
		if (n_succeeded == 0)
			continue;

		/* fragment the chunk according to the coloring */
		local_best = fragment_chunk(env, col, c, tmp_chunks);

		/* search the best of the good list
		   and make it the new best if it is better than the current */
		if (local_best) {
			aff_chunk_assure_weight(env, local_best);

			DB((dbg, LEVEL_3, "\t\tlocal best chunk (id %u) for color %d: ", local_best->id, col));
			DBG_AFF_CHUNK(env, LEVEL_3, local_best);

			if (! best_chunk || best_chunk->weight < local_best->weight) {
				best_chunk = local_best;
				best_color = col;
				if (best_starts)
					del_waitq(best_starts);
				best_starts = good_starts;
				DB((dbg, LEVEL_3, "\n\t\t... setting global best chunk (id %u), color %d\n", best_chunk->id, best_color));
			} else {
				DB((dbg, LEVEL_3, "\n\t\t... omitting, global best is better\n"));
				del_waitq(good_starts);
			}
		}
		else {
			del_waitq(good_starts);
		}

		/* if all nodes were recolored, bail out */
		if (n_succeeded == n_nodes)
			break;
	}

	stat_ev_int("heur4_colors_tried", i);

	/* free all intermediate created chunks except best one */
	while (! waitq_empty(tmp_chunks)) {
		aff_chunk_t *tmp = waitq_get(tmp_chunks);
		if (tmp != best_chunk)
			delete_aff_chunk(env, tmp);
	}
	del_waitq(tmp_chunks);

	/* return if coloring failed */
	if (! best_chunk) {
		if (best_starts)
			del_waitq(best_starts);
		return;
	}

	DB((dbg, LEVEL_2, "\tbest chunk #%u ", best_chunk->id));
	DBG_AFF_CHUNK(env, LEVEL_2, best_chunk);
	DB((dbg, LEVEL_2, "using color %d\n", best_color));

	for (idx = 0, len = ARR_LEN(best_chunk->n); idx < len; ++idx) {
		const ir_node *irn  = best_chunk->n[idx];
		co_mst_irn_t  *node = get_co_mst_irn(env, irn);
		int res;

		/* bring the node to the color. */
		DB((dbg, LEVEL_4, "\tManifesting color %d for %+F, chunk #%u\n", best_color, node->irn, best_chunk->id));
		INIT_LIST_HEAD(&changed);
		stat_ev_tim_push();
		res = change_node_color(env, node, best_color, &changed);
		stat_ev_tim_pop("heur4_recolor");
		if (res) {
			materialize_coloring(&changed);
			node->fixed = 1;
		}
		assert(list_empty(&changed));
	}

	/* remove the nodes in best chunk from original chunk */
	len = ARR_LEN(best_chunk->n);
	for (idx = 0; idx < len; ++idx) {
		const ir_node *irn = best_chunk->n[idx];
		int pos = nodes_bsearch(c->n, irn);

		if (pos > 0)
			c->n[pos] = NULL;
	}
	len = ARR_LEN(c->n);
	for (idx = nidx = 0; idx < len; ++idx) {
		const ir_node *irn = c->n[idx];

		if (irn != NULL) {
			c->n[nidx++] = irn;
		}
	}
	ARR_SHRINKLEN(c->n, nidx);


	/* we have to get the nodes back into the original chunk because they are scattered over temporary chunks */
	for (idx = 0, len = ARR_LEN(c->n); idx < len; ++idx) {
		const ir_node *n  = c->n[idx];
		co_mst_irn_t  *nn = get_co_mst_irn(env, n);
		nn->chunk = c;
	}

	/* fragment the remaining chunk */
	visited = bitset_irg_malloc(env->co->irg);
	for (idx = 0, len = ARR_LEN(best_chunk->n); idx < len; ++idx)
		bitset_set(visited, get_irn_idx(best_chunk->n[idx]));

	for (idx = 0, len = ARR_LEN(c->n); idx < len; ++idx) {
		const ir_node *irn = c->n[idx];
		if (! bitset_is_set(visited, get_irn_idx(irn))) {
			aff_chunk_t  *new_chunk = new_aff_chunk(env);
			co_mst_irn_t *node      = get_co_mst_irn(env, irn);

			expand_chunk_from(env, node, visited, new_chunk, c, decider_always_yes, 0);
			aff_chunk_assure_weight(env, new_chunk);
			pqueue_put(env->chunks, new_chunk, new_chunk->weight);
		}
	}

	for (idx = 0, len = ARR_LEN(best_chunk->n); idx < len; ++idx) {
		const ir_node *n  = best_chunk->n[idx];
		co_mst_irn_t  *nn = get_co_mst_irn(env, n);
		nn->chunk = NULL;
	}

	/* clear obsolete chunks and free some memory */
	delete_aff_chunk(env, best_chunk);
	bitset_free(visited);
	if (best_starts)
		del_waitq(best_starts);

	stat_ev_ctx_pop("heur4_color_chunk");
}

/**
 * Main driver for mst safe coalescing algorithm.
 */
static int co_solve_heuristic_mst(copy_opt_t *co)
{
	unsigned     n_regs       = co->cls->n_regs;
	bitset_t     *ignore_regs = bitset_alloca(n_regs);
	unsigned     i, j, k;
	ir_node      *irn;
	co_mst_env_t mst_env;

	last_chunk_id = 0;

	stat_ev_tim_push();

	/* init phase */
	phase_init(&mst_env.ph, co->irg, co_mst_irn_init);
	phase_set_private(&mst_env.ph, &mst_env);

	k = be_put_ignore_regs(co->cenv->irg, co->cls, ignore_regs);
	k = n_regs - k;

	mst_env.n_regs        = n_regs;
	mst_env.k             = k;
	mst_env.chunks        = new_pqueue();
	mst_env.co            = co;
	mst_env.ignore_regs   = ignore_regs;
	mst_env.ifg           = co->cenv->ifg;
	mst_env.chunkset      = pset_new_ptr(512);
	mst_env.chunk_visited = 0;
	mst_env.single_cols   = phase_alloc(&mst_env.ph, sizeof(*mst_env.single_cols) * n_regs);

	for (i = 0; i < n_regs; ++i) {
		col_cost_t *vec = phase_alloc(&mst_env.ph, sizeof(*vec) * n_regs);

		mst_env.single_cols[i] = vec;
		for (j = 0; j < n_regs; ++j) {
			vec[j].col  = j;
			vec[j].cost = REAL(0.0);
		}
		vec[i].col  = 0;
		vec[0].col  = i;
		vec[0].cost = REAL(1.0);
	}

	DBG((dbg, LEVEL_1, "==== Coloring %+F, class %s ====\n", co->irg, co->cls->name));

	/* build affinity chunks */
	stat_ev_tim_push();
	build_affinity_chunks(&mst_env);
	stat_ev_tim_pop("heur4_initial_chunk");

	/* color chunks as long as there are some */
	while (! pqueue_empty(mst_env.chunks)) {
		aff_chunk_t *chunk = pqueue_pop_front(mst_env.chunks);

		color_aff_chunk(&mst_env, chunk);
		DB((dbg, LEVEL_4, "<<<====== Coloring chunk (%u) done\n", chunk->id));
		delete_aff_chunk(&mst_env, chunk);
	}

	/* apply coloring */
	foreach_phase_irn(&mst_env.ph, irn) {
		co_mst_irn_t          *mirn;
		const arch_register_t *reg;

		if (arch_irn_is_ignore(irn))
			continue;

		mirn = get_co_mst_irn(&mst_env, irn);
		// assert(mirn->fixed && "Node should have fixed color");

		/* skip nodes where color hasn't changed */
		if (mirn->init_col == mirn->col)
			continue;

		reg = arch_register_for_index(co->cls, mirn->col);
		arch_set_irn_register(irn, reg);
		DB((dbg, LEVEL_1, "%+F set color from %d to %d\n", irn, mirn->init_col, mirn->col));
	}

	/* free allocated memory */
	del_pqueue(mst_env.chunks);
	phase_deinit(&mst_env.ph);
	del_pset(mst_env.chunkset);

	stat_ev_tim_pop("heur4_total");

	return 0;
}

static const lc_opt_table_entry_t options[] = {
	LC_OPT_ENT_INT      ("limit", "limit recoloring",  &recolor_limit),
	LC_OPT_ENT_DBL      ("di",    "dislike influence", &dislike_influence),
	LC_OPT_LAST
};

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_copyheur4);
void be_init_copyheur4(void)
{
	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *ra_grp = lc_opt_get_grp(be_grp, "ra");
	lc_opt_entry_t *chordal_grp = lc_opt_get_grp(ra_grp, "chordal");
	lc_opt_entry_t *co_grp = lc_opt_get_grp(chordal_grp, "co");
	lc_opt_entry_t *heur4_grp = lc_opt_get_grp(co_grp, "heur4");

	static co_algo_info copyheur = {
		co_solve_heuristic_mst, 0
	};

	lc_opt_add_table(heur4_grp, options);
	be_register_copyopt("heur4", &copyheur);

	FIRM_DBG_REGISTER(dbg, "firm.be.co.heur4");
}
