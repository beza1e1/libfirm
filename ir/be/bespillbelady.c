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
 * @brief       Beladys spillalgorithm.
 * @author      Daniel Grund, Matthias Braun
 * @date        20.09.2005
 * @version     $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>

#include "obst.h"
#include "irprintf_t.h"
#include "irgraph.h"
#include "irnode.h"
#include "irmode.h"
#include "irgwalk.h"
#include "irloop.h"
#include "iredges_t.h"
#include "ircons_t.h"
#include "irprintf.h"
#include "irnodeset.h"
#include "xmalloc.h"

#include "beutil.h"
#include "bearch_t.h"
#include "beuses.h"
#include "besched_t.h"
#include "beirgmod.h"
#include "belive_t.h"
#include "benode_t.h"
#include "bechordal_t.h"
#include "bespilloptions.h"
#include "beloopana.h"
#include "beirg_t.h"
#include "bespill.h"
#include "bemodule.h"

#define DBG_SPILL     1
#define DBG_WSETS     2
#define DBG_FIX       4
#define DBG_DECIDE    8
#define DBG_START    16
#define DBG_SLOTS    32
#define DBG_TRACE    64
#define DBG_WORKSET 128
DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

#define TIME_UNDEFINED 6666

//#define LOOK_AT_LOOPDEPTH

/**
 * An association between a node and a point in time.
 */
typedef struct loc_t {
	ir_node          *node;
	unsigned          time;     /**< A use time (see beuses.h). */
	bool              spilled;  /**< the value was already spilled on this path */
} loc_t;

typedef struct _workset_t {
	int   len;          /**< current length */
	loc_t vals[0];      /**< inlined array of the values/distances in this working set */
} workset_t;

static struct obstack               obst;
static const arch_env_t            *arch_env;
static const arch_register_class_t *cls;
static const be_lv_t               *lv;
static be_loopana_t                *loop_ana;
static int                          n_regs;
static workset_t                   *ws;     /**< the main workset used while
	                                             processing a block. */
static be_uses_t                   *uses;   /**< env for the next-use magic */
static ir_node                     *instr;  /**< current instruction */
static unsigned                     instr_nr; /**< current instruction number
	                                               (relative to block start) */
static spill_env_t                 *senv;   /**< see bespill.h */
static ir_node                    **blocklist;

static bool                         move_spills      = true;
static bool                         respectloopdepth = true;
static bool                         improve_known_preds = true;
/* factor to weight the different costs of reloading/rematerializing a node
   (see bespill.h be_get_reload_costs_no_weight) */
static int                          remat_bonus      = 10;

static const lc_opt_table_entry_t options[] = {
	LC_OPT_ENT_BOOL   ("movespills", "try to move spills out of loops", &move_spills),
	LC_OPT_ENT_BOOL   ("respectloopdepth", "exprimental (outermost loop cutting)", &respectloopdepth),
	LC_OPT_ENT_BOOL   ("improveknownpreds", "experimental (known preds cutting)", &improve_known_preds),
	LC_OPT_ENT_INT    ("rematbonus", "give bonus to rematerialisable nodes", &remat_bonus),
	LC_OPT_LAST
};

static int loc_compare(const void *a, const void *b)
{
	const loc_t *p = a;
	const loc_t *q = b;
	return p->time - q->time;
}

void workset_print(const workset_t *w)
{
	int i;

	for(i = 0; i < w->len; ++i) {
		ir_fprintf(stderr, "%+F %d\n", w->vals[i].node, w->vals[i].time);
	}
}

/**
 * Alloc a new workset on obstack @p ob with maximum size @p max
 */
static workset_t *new_workset(void)
{
	workset_t *res;
	size_t     size = sizeof(*res) + n_regs * sizeof(res->vals[0]);

	res  = obstack_alloc(&obst, size);
	memset(res, 0, size);
	return res;
}

/**
 * Alloc a new instance on obstack and make it equal to @param workset
 */
static workset_t *workset_clone(workset_t *workset)
{
	workset_t *res;
	size_t size = sizeof(*res) + n_regs * sizeof(res->vals[0]);
	res = obstack_alloc(&obst, size);
	memcpy(res, workset, size);
	return res;
}

/**
 * Copy workset @param src to @param tgt
 */
static void workset_copy(workset_t *dest, const workset_t *src)
{
	size_t size = sizeof(*src) + n_regs * sizeof(src->vals[0]);
	memcpy(dest, src, size);
}

/**
 * Overwrites the current content array of @param ws with the
 * @param count locations given at memory @param locs.
 * Set the length of @param ws to count.
 */
static void workset_bulk_fill(workset_t *workset, int count, const loc_t *locs)
{
	workset->len = count;
	memcpy(&(workset->vals[0]), locs, count * sizeof(locs[0]));
}

/**
 * Inserts the value @p val into the workset, iff it is not
 * already contained. The workset must not be full.
 */
static void workset_insert(workset_t *workset, ir_node *val, bool spilled)
{
	loc_t *loc;
	int    i;
	/* check for current regclass */
	assert(arch_irn_consider_in_reg_alloc(arch_env, cls, val));

	/* check if val is already contained */
	for (i = 0; i < workset->len; ++i) {
		loc = &workset->vals[i];
		if (loc->node == val) {
			if (spilled) {
				loc->spilled = true;
			}
			return;
		}
	}

	/* insert val */
	assert(workset->len < n_regs && "Workset already full!");
	loc           = &workset->vals[workset->len];
	loc->node     = val;
	loc->spilled  = spilled;
	loc->time     = TIME_UNDEFINED;
	workset->len++;
}

/**
 * Removes all entries from this workset
 */
static void workset_clear(workset_t *workset)
{
	workset->len = 0;
}

/**
 * Removes the value @p val from the workset if present.
 */
static INLINE void workset_remove(workset_t *workset, ir_node *val)
{
	int i;
	for(i = 0; i < workset->len; ++i) {
		if (workset->vals[i].node == val) {
			workset->vals[i] = workset->vals[--workset->len];
			return;
		}
	}
}

static INLINE const loc_t *workset_contains(const workset_t *ws,
                                            const ir_node *val)
{
	int i;

	for (i = 0; i < ws->len; ++i) {
		if (ws->vals[i].node == val)
			return &ws->vals[i];
	}

	return NULL;
}

/**
 * Iterates over all values in the working set.
 * @p ws The workset to iterate
 * @p v  A variable to put the current value in
 * @p i  An integer for internal use
 */
#define workset_foreach(ws, v, i)	for(i=0; \
										v=(i < ws->len) ? ws->vals[i].node : NULL, i < ws->len; \
										++i)

#define workset_set_time(ws, i, t) (ws)->vals[i].time=t
#define workset_get_time(ws, i) (ws)->vals[i].time
#define workset_set_length(ws, length) (ws)->len = length
#define workset_get_length(ws) ((ws)->len)
#define workset_get_val(ws, i) ((ws)->vals[i].node)
#define workset_sort(ws) qsort((ws)->vals, (ws)->len, sizeof((ws)->vals[0]), loc_compare);

typedef struct _block_info_t
{
	workset_t *start_workset;
	workset_t *end_workset;
} block_info_t;


static void *new_block_info(void)
{
	block_info_t *res = obstack_alloc(&obst, sizeof(res[0]));
	memset(res, 0, sizeof(res[0]));

	return res;
}

#define get_block_info(block)        ((block_info_t *)get_irn_link(block))
#define set_block_info(block, info)  set_irn_link(block, info)

/**
 * @return The distance to the next use or 0 if irn has dont_spill flag set
 */
static INLINE unsigned get_distance(ir_node *from, unsigned from_step,
                                    const ir_node *def, int skip_from_uses)
{
	be_next_use_t use;
	int           flags = arch_irn_get_flags(arch_env, def);
	unsigned      costs;
	unsigned      time;

	assert(! (flags & arch_irn_flags_ignore));

	use  = be_get_next_use(uses, from, from_step, def, skip_from_uses);
	time = use.time;
	if (USES_IS_INFINITE(time))
		return USES_INFINITY;

	/* We have to keep nonspillable nodes in the workingset */
	if (flags & arch_irn_flags_dont_spill)
		return 0;

	/* give some bonus to rematerialisable nodes */
	if (remat_bonus > 0) {
		costs = be_get_reload_costs_no_weight(senv, def, use.before);
		assert(costs * remat_bonus < 1000);
		time  += 1000 - (costs * remat_bonus);
	}

	return time;
}

/**
 * Performs the actions necessary to grant the request that:
 * - new_vals can be held in registers
 * - as few as possible other values are disposed
 * - the worst values get disposed
 *
 * @p is_usage indicates that the values in new_vals are used (not defined)
 * In this case reloads must be performed
 */
static void displace(workset_t *new_vals, int is_usage)
{
	ir_node **to_insert = alloca(n_regs * sizeof(to_insert[0]));
	bool     *spilled   = alloca(n_regs * sizeof(spilled[0]));
	ir_node  *val;
	int       i;
	int       len;
	int       spills_needed;
	int       demand;
	int       iter;

	/* 1. Identify the number of needed slots and the values to reload */
	demand = 0;
	workset_foreach(new_vals, val, iter) {
		bool reloaded = false;

		if (! workset_contains(ws, val)) {
			DB((dbg, DBG_DECIDE, "    insert %+F\n", val));
			if (is_usage) {
				DB((dbg, DBG_SPILL, "Reload %+F before %+F\n", val, instr));
				be_add_reload(senv, val, instr, cls, 1);
				reloaded = true;
			}
		} else {
			DB((dbg, DBG_DECIDE, "    %+F already in workset\n", val));
			assert(is_usage);
			/* remove the value from the current workset so it is not accidently
			 * spilled */
			workset_remove(ws, val);
		}
		spilled[demand]   = reloaded;
		to_insert[demand] = val;
		++demand;
	}

	/* 2. Make room for at least 'demand' slots */
	len           = workset_get_length(ws);
	spills_needed = len + demand - n_regs;
	assert(spills_needed <= len);

	/* Only make more free room if we do not have enough */
	if (spills_needed > 0) {
		ir_node   *curr_bb  = NULL;
		workset_t *ws_start = NULL;

		if (move_spills) {
			curr_bb  = get_nodes_block(instr);
			ws_start = get_block_info(curr_bb)->start_workset;
		}

		DB((dbg, DBG_DECIDE, "    disposing %d values\n", spills_needed));

		/* calculate current next-use distance for live values */
		for (i = 0; i < len; ++i) {
			ir_node  *val  = workset_get_val(ws, i);
			unsigned  dist = get_distance(instr, instr_nr, val, !is_usage);
			workset_set_time(ws, i, dist);
		}

		/* sort entries by increasing nextuse-distance*/
		workset_sort(ws);

		for (i = len - spills_needed; i < len; ++i) {
			ir_node *val = ws->vals[i].node;

			DB((dbg, DBG_DECIDE, "    disposing node %+F (%u)\n", val,
			     workset_get_time(ws, i)));

			if (move_spills) {
				if (!USES_IS_INFINITE(ws->vals[i].time)
						&& !ws->vals[i].spilled) {
					ir_node *after_pos = sched_prev(instr);
					DB((dbg, DBG_DECIDE, "Spill %+F after node %+F\n", val,
						after_pos));
					be_add_spill(senv, val, after_pos);
				}
			}
		}

		/* kill the last 'demand' entries in the array */
		workset_set_length(ws, len - spills_needed);
	}

	/* 3. Insert the new values into the workset */
	for (i = 0; i < demand; ++i) {
		ir_node *val = to_insert[i];

		workset_insert(ws, val, spilled[i]);
	}
}

enum {
	AVAILABLE_EVERYWHERE,
	AVAILABLE_NOWHERE,
	AVAILABLE_PARTLY,
	AVAILABLE_UNKNOWN
};

static unsigned available_in_all_preds(workset_t* const* pred_worksets,
                                       size_t n_pred_worksets,
                                       const ir_node *value, bool is_local_phi)
{
	size_t i;
	bool   avail_everywhere = true;
	bool   avail_nowhere    = true;

	assert(n_pred_worksets > 0);

	/* value available in all preds? */
	for (i = 0; i < n_pred_worksets; ++i) {
		bool             found     = false;
		const workset_t *p_workset = pred_worksets[i];
		int              p_len     = workset_get_length(p_workset);
		int              p_i;
		const ir_node   *l_value;

		if (is_local_phi) {
			assert(is_Phi(value));
			l_value = get_irn_n(value, i);
		} else {
			l_value = value;
		}

		for (p_i = 0; p_i < p_len; ++p_i) {
			const loc_t *p_l = &p_workset->vals[p_i];
			if (p_l->node != l_value)
				continue;

			found = true;
			break;
		}

		if (found) {
			avail_nowhere = false;
		} else {
			avail_everywhere = false;
		}
	}

	if (avail_everywhere) {
		assert(!avail_nowhere);
		return AVAILABLE_EVERYWHERE;
	} else if (avail_nowhere) {
		return AVAILABLE_NOWHERE;
	} else {
		return AVAILABLE_PARTLY;
	}
}

/** Decides whether a specific node should be in the start workset or not
 *
 * @param env      belady environment
 * @param first
 * @param node     the node to test
 * @param loop     the loop of the node
 */
static loc_t to_take_or_not_to_take(ir_node* first, ir_node *node,
                                    ir_loop *loop, unsigned available)
{
	be_next_use_t next_use;
	loc_t         loc;

	loc.time    = USES_INFINITY;
	loc.node    = node;
	loc.spilled = false;

	if (!arch_irn_consider_in_reg_alloc(arch_env, cls, node)) {
		loc.time = USES_INFINITY;
		return loc;
	}

	/* We have to keep nonspillable nodes in the workingset */
	if (arch_irn_get_flags(arch_env, node) & arch_irn_flags_dont_spill) {
		loc.time = 0;
		DB((dbg, DBG_START, "    %+F taken (dontspill node)\n", node, loc.time));
		return loc;
	}

	next_use = be_get_next_use(uses, first, 0, node, 0);
	if (USES_IS_INFINITE(next_use.time)) {
		// the nodes marked as live in shouldn't be dead, so it must be a phi
		assert(is_Phi(node));
		loc.time = USES_INFINITY;
		DB((dbg, DBG_START, "    %+F not taken (dead)\n", node));
		return loc;
	}

	loc.time = next_use.time;

	if (improve_known_preds) {
		if (available == AVAILABLE_EVERYWHERE) {
			DB((dbg, DBG_START, "    %+F taken (%u, live in all preds)\n",
			    node, loc.time));
			return loc;
		} else if(available == AVAILABLE_NOWHERE) {
			DB((dbg, DBG_START, "    %+F not taken (%u, live in no pred)\n",
			    node, loc.time));
			loc.time = USES_INFINITY;
			return loc;
		}
	}

	if (!respectloopdepth || next_use.outermost_loop >= get_loop_depth(loop)) {
		DB((dbg, DBG_START, "    %+F taken (%u, loop %d)\n", node, loc.time,
		    next_use.outermost_loop));
	} else {
		loc.time = USES_PENDING;
		DB((dbg, DBG_START, "    %+F delayed (outerdepth %d < loopdepth %d)\n",
		    node, next_use.outermost_loop, get_loop_depth(loop)));
	}

	return loc;
}

/**
 * Computes the start-workset for a block with multiple predecessors. We assume
 * that at least 1 of the predeccesors is a back-edge which means we're at the
 * beginning of a loop. We try to reload as much values as possible now so they
 * don't get reloaded inside the loop.
 */
static void decide_start_workset(const ir_node *block)
{
	ir_loop    *loop = get_irn_loop(block);
	ir_node    *first;
	ir_node    *node;
	loc_t       loc;
	loc_t      *starters;
	loc_t      *delayed;
	int         i, len, ws_count;
	int	        free_slots, free_pressure_slots;
	unsigned    pressure;
	int         arity;
	workset_t **pred_worksets;
	bool        all_preds_known;

	/* check predecessors */
	arity           = get_irn_arity(block);
	pred_worksets   = alloca(sizeof(pred_worksets[0]) * arity);
	all_preds_known = true;
	for(i = 0; i < arity; ++i) {
		ir_node      *pred_block = get_Block_cfgpred_block(block, i);
		block_info_t *pred_info  = get_block_info(pred_block);

		if (pred_info == NULL) {
			pred_worksets[i] = NULL;
			all_preds_known  = false;
		} else {
			pred_worksets[i] = pred_info->end_workset;
		}
	}

	/* Collect all values living at start of block */
	starters = NEW_ARR_F(loc_t, 0);
	delayed  = NEW_ARR_F(loc_t, 0);

	DB((dbg, DBG_START, "Living at start of %+F:\n", block));
	first = sched_first(block);

	/* check all Phis first */
	sched_foreach(block, node) {
		unsigned available;

		if (! is_Phi(node))
			break;
		if (!arch_irn_consider_in_reg_alloc(arch_env, cls, node))
			continue;

		if (all_preds_known) {
			available = available_in_all_preds(pred_worksets, arity, node, true);
		} else {
			available = AVAILABLE_UNKNOWN;
		}

		loc = to_take_or_not_to_take(first, node, loop, available);

		if (! USES_IS_INFINITE(loc.time)) {
			if (USES_IS_PENDING(loc.time))
				ARR_APP1(loc_t, delayed, loc);
			else
				ARR_APP1(loc_t, starters, loc);
		} else {
			be_spill_phi(senv, node);
		}
	}

	/* check all Live-Ins */
	be_lv_foreach(lv, block, be_lv_state_in, i) {
		ir_node *node = be_lv_get_irn(lv, block, i);
		unsigned available;

		if (all_preds_known) {
			available = available_in_all_preds(pred_worksets, arity, node, false);
		} else {
			available = AVAILABLE_UNKNOWN;
		}

		loc = to_take_or_not_to_take(first, node, loop, available);

		if (! USES_IS_INFINITE(loc.time)) {
			if (USES_IS_PENDING(loc.time))
				ARR_APP1(loc_t, delayed, loc);
			else
				ARR_APP1(loc_t, starters, loc);
		}
	}

	pressure            = be_get_loop_pressure(loop_ana, cls, loop);
	assert(ARR_LEN(delayed) <= (signed)pressure);
	free_slots          = n_regs - ARR_LEN(starters);
	free_pressure_slots = n_regs - (pressure - ARR_LEN(delayed));
	free_slots          = MIN(free_slots, free_pressure_slots);

	/* so far we only put nodes into the starters list that are used inside
	 * the loop. If register pressure in the loop is low then we can take some
	 * values and let them live through the loop */
	DB((dbg, DBG_START, "Loop pressure %d, taking %d delayed vals\n",
	    pressure, free_slots));
	if (free_slots > 0) {
		qsort(delayed, ARR_LEN(delayed), sizeof(delayed[0]), loc_compare);

		for (i = 0; i < ARR_LEN(delayed) && free_slots > 0; ++i) {
			int    p, arity;
			loc_t *loc = & delayed[i];

			/* don't use values which are dead in a known predecessors
			 * to not induce unnecessary reloads */
			arity = get_irn_arity(block);
			for (p = 0; p < arity; ++p) {
				ir_node      *pred_block = get_Block_cfgpred_block(block, p);
				block_info_t *pred_info  = get_block_info(pred_block);

				if (pred_info == NULL)
					continue;

				if (!workset_contains(pred_info->end_workset, loc->node)) {
					DB((dbg, DBG_START,
					    "    delayed %+F not live at pred %+F\n", loc->node,
					    pred_block));
					goto skip_delayed;
				}
			}

			DB((dbg, DBG_START, "    delayed %+F taken\n", loc->node));
			ARR_APP1(loc_t, starters, *loc);
			loc->node = NULL;
			--free_slots;
		skip_delayed:
			;
		}
	}

	/* spill phis (the actual phis not just their values) that are in this block
	 * but not in the start workset */
	for (i = ARR_LEN(delayed) - 1; i >= 0; --i) {
		ir_node *node = delayed[i].node;
		if (node == NULL || !is_Phi(node) || get_nodes_block(node) != block)
			continue;

		DB((dbg, DBG_START, "    spilling delayed phi %+F\n", node));
		be_spill_phi(senv, node);
	}
	DEL_ARR_F(delayed);

	/* Sort start values by first use */
	qsort(starters, ARR_LEN(starters), sizeof(starters[0]), loc_compare);

	/* Copy the best ones from starters to start workset */
	ws_count = MIN(ARR_LEN(starters), n_regs);
	workset_clear(ws);
	workset_bulk_fill(ws, ws_count, starters);

	/* spill phis (the actual phis not just their values) that are in this block
	 * but not in the start workset */
	len = ARR_LEN(starters);
	for (i = ws_count; i < len; ++i) {
		ir_node *node = starters[i].node;
		if (! is_Phi(node) || get_nodes_block(node) != block)
			continue;

		DB((dbg, DBG_START, "    spilling phi %+F\n", node));
		be_spill_phi(senv, node);
	}

	DEL_ARR_F(starters);

	/* determine spill status of the values: If there's 1 pred block (which
	 * is no backedge) where the value is spilled then we must set it to
	 * spilled here. */
	for(i = 0; i < ws_count; ++i) {
		loc_t   *loc     = &ws->vals[i];
		ir_node *value   = loc->node;
		bool     spilled;
		int      n;

		/* phis from this block aren't spilled */
		if (get_nodes_block(value) == block) {
			assert(is_Phi(value));
			loc->spilled = false;
			continue;
		}

		/* determine if value was spilled on any predecessor */
		spilled = false;
		for(n = 0; n < arity; ++n) {
			workset_t *pred_workset = pred_worksets[n];
			int        p_len;
			int        p;

			if (pred_workset == NULL)
				continue;

			p_len = workset_get_length(pred_workset);
			for(p = 0; p < p_len; ++p) {
				loc_t *l = &pred_workset->vals[p];

				if (l->node != value)
					continue;

				if (l->spilled) {
					spilled = true;
				}
				break;
			}
		}

		loc->spilled = spilled;
	}
}

/**
 * For the given block @p block, decide for each values
 * whether it is used from a register or is reloaded
 * before the use.
 */
static void process_block(ir_node *block)
{
	workset_t       *new_vals;
	ir_node         *irn;
	int              iter;
	block_info_t    *block_info;
	int              arity;

	/* no need to process a block twice */
	assert(get_block_info(block) == NULL);

	/* construct start workset */
	arity = get_Block_n_cfgpreds(block);
	if (arity == 0) {
		/* no predecessor -> empty set */
		workset_clear(ws);
	} else if (arity == 1) {
		/* one predecessor, copy it's end workset */
		ir_node      *pred_block = get_Block_cfgpred_block(block, 0);
		block_info_t *pred_info  = get_block_info(pred_block);

		assert(pred_info != NULL);
		workset_copy(ws, pred_info->end_workset);
	} else {
		/* multiple predecessors, do more advanced magic :) */
		decide_start_workset(block);
	}

	DB((dbg, DBG_DECIDE, "\n"));
	DB((dbg, DBG_DECIDE, "Decide for %+F\n", block));

	block_info = new_block_info();
	set_block_info(block, block_info);

	DB((dbg, DBG_WSETS, "Start workset for %+F:\n", block));
	workset_foreach(ws, irn, iter) {
		DB((dbg, DBG_WSETS, "  %+F (%u)\n", irn,
		     workset_get_time(ws, iter)));
	}

	block_info->start_workset = workset_clone(ws);

	/* process the block from start to end */
	DB((dbg, DBG_WSETS, "Processing...\n"));
	instr_nr = 0;
	/* TODO: this leaks (into the obstack)... */
	new_vals = new_workset();

	sched_foreach(block, irn) {
		int i, arity;
		assert(workset_get_length(ws) <= n_regs);

		/* Phis are no real instr (see insert_starters()) */
		if (is_Phi(irn)) {
			continue;
		}
		DB((dbg, DBG_DECIDE, "  ...%+F\n", irn));

		/* set instruction in the workset */
		instr = irn;

		/* allocate all values _used_ by this instruction */
		workset_clear(new_vals);
		for(i = 0, arity = get_irn_arity(irn); i < arity; ++i) {
			ir_node *in = get_irn_n(irn, i);
			if (!arch_irn_consider_in_reg_alloc(arch_env, cls, in))
				continue;

			/* (note that "spilled" is irrelevant here) */
			workset_insert(new_vals, in, false);
		}
		displace(new_vals, 1);

		/* allocate all values _defined_ by this instruction */
		workset_clear(new_vals);
		if (get_irn_mode(irn) == mode_T) {
			const ir_edge_t *edge;

			foreach_out_edge(irn, edge) {
				ir_node *proj = get_edge_src_irn(edge);
				if (!arch_irn_consider_in_reg_alloc(arch_env, cls, proj))
					continue;
				workset_insert(new_vals, proj, false);
			}
		} else {
			if (!arch_irn_consider_in_reg_alloc(arch_env, cls, irn))
				continue;
			workset_insert(new_vals, irn, false);
		}
		displace(new_vals, 0);

		instr_nr++;
	}

	/* Remember end-workset for this block */
	block_info->end_workset = workset_clone(ws);
	DB((dbg, DBG_WSETS, "End workset for %+F:\n", block));
	workset_foreach(ws, irn, iter)
		DB((dbg, DBG_WSETS, "  %+F (%u)\n", irn,
		     workset_get_time(ws, iter)));
}

/**
 * 'decide' is block-local and makes assumptions
 * about the set of live-ins. Thus we must adapt the
 * live-outs to the live-ins at each block-border.
 */
static void fix_block_borders(ir_node *block, void *data)
{
	workset_t    *start_workset;
	int           arity;
	int           i;
	int           iter;
	(void) data;

	DB((dbg, DBG_FIX, "\n"));
	DB((dbg, DBG_FIX, "Fixing %+F\n", block));

	start_workset = get_block_info(block)->start_workset;

	/* process all pred blocks */
	arity = get_irn_arity(block);
	for (i = 0; i < arity; ++i) {
		ir_node   *pred = get_Block_cfgpred_block(block, i);
		workset_t *pred_end_workset = get_block_info(pred)->end_workset;
		ir_node   *node;

		DB((dbg, DBG_FIX, "  Pred %+F\n", pred));

		/* spill all values not used anymore */
		workset_foreach(pred_end_workset, node, iter) {
			ir_node *n2;
			int      iter2;
			bool     found = false;
			workset_foreach(start_workset, n2, iter2) {
				if (n2 == node) {
					found = true;
					break;
				}
				/* note that we do not look at phi inputs, becuase the values
				 * will be either live-end and need no spill or
				 * they have other users in which must be somewhere else in the
				 * workset */
			}

			if (found)
				continue;

			if (move_spills && be_is_live_in(lv, block, node)
					&& !pred_end_workset->vals[iter].spilled) {
				ir_node *insert_point;
				if (arity > 1) {
					insert_point = be_get_end_of_block_insertion_point(pred);
					insert_point = sched_prev(insert_point);
				} else {
					insert_point = block;
				}
				DB((dbg, DBG_SPILL, "Spill %+F after %+F\n", node,
				     insert_point));
				be_add_spill(senv, node, insert_point);
			}
		}

		/* reload missing values in predecessors, add missing spills */
		workset_foreach(start_workset, node, iter) {
			const loc_t *l    = &start_workset->vals[iter];
			const loc_t *pred_loc;

			/* if node is a phi of the current block we reload
			 * the corresponding argument, else node itself */
			if (is_Phi(node) && get_nodes_block(node) == block) {
				node = get_irn_n(node, i);
				assert(!l->spilled);

				/* we might have unknowns as argument for the phi */
				if (!arch_irn_consider_in_reg_alloc(arch_env, cls, node))
					continue;
			}

			/* check if node is in a register at end of pred */
			pred_loc = workset_contains(pred_end_workset, node);
			if (pred_loc != NULL) {
				/* we might have to spill value on this path */
				if (move_spills && !pred_loc->spilled && l->spilled) {
					ir_node *insert_point
						= be_get_end_of_block_insertion_point(pred);
					insert_point = sched_prev(insert_point);
					DB((dbg, DBG_SPILL, "Spill %+F after %+F\n", node,
					    insert_point));
					be_add_spill(senv, node, insert_point);
				}
			} else {
				/* node is not in register at the end of pred -> reload it */
				DB((dbg, DBG_FIX, "    reload %+F\n", node));
				DB((dbg, DBG_SPILL, "Reload %+F before %+F,%d\n", node, block, i));
				be_add_reload_on_edge(senv, node, block, i, cls, 1);
			}
		}
	}
}

static void add_block(ir_node *block, void *data)
{
	(void) data;
	ARR_APP1(ir_node*, blocklist, block);
}

static void be_spill_belady(be_irg_t *birg, const arch_register_class_t *rcls)
{
	int i;
	ir_graph *irg = be_get_birg_irg(birg);

	be_liveness_assure_sets(be_assure_liveness(birg));

	stat_ev_tim_push();
	/* construct control flow loop tree */
	if (! (get_irg_loopinfo_state(irg) & loopinfo_cf_consistent)) {
		construct_cf_backedges(irg);
	}
	stat_ev_tim_pop("belady_time_backedges");

	stat_ev_tim_push();
	be_clear_links(irg);
	stat_ev_tim_pop("belady_time_clear_links");

	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);

	/* init belady env */
	stat_ev_tim_push();
	obstack_init(&obst);
	arch_env  = birg->main_env->arch_env;
	cls       = rcls;
	lv        = be_get_birg_liveness(birg);
	n_regs    = cls->n_regs - be_put_ignore_regs(birg, cls, NULL);
	ws        = new_workset();
	uses      = be_begin_uses(irg, lv);
	loop_ana  = be_new_loop_pressure(birg, cls);
	senv      = be_new_spill_env(birg);
	blocklist = NEW_ARR_F(ir_node*, 0);
	irg_block_edges_walk(get_irg_start_block(irg), NULL, add_block, NULL);
	stat_ev_tim_pop("belady_time_init");

	stat_ev_tim_push();
	/* walk blocks in reverse postorder */
	for (i = ARR_LEN(blocklist) - 1; i >= 0; --i) {
		process_block(blocklist[i]);
	}
	DEL_ARR_F(blocklist);
	stat_ev_tim_pop("belady_time_belady");

	stat_ev_tim_push();
	/* belady was block-local, fix the global flow by adding reloads on the
	 * edges */
	irg_block_walk_graph(irg, fix_block_borders, NULL, NULL);
	stat_ev_tim_pop("belady_time_fix_borders");

	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);

	/* Insert spill/reload nodes into the graph and fix usages */
	be_insert_spills_reloads(senv);

	/* clean up */
	be_delete_spill_env(senv);
	be_end_uses(uses);
	be_free_loop_pressure(loop_ana);
	obstack_free(&obst, NULL);
}

void be_init_spillbelady(void)
{
	static be_spiller_t belady_spiller = {
		be_spill_belady
	};
	lc_opt_entry_t *be_grp       = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *belady_group = lc_opt_get_grp(be_grp, "belady");
	lc_opt_add_table(belady_group, options);

	be_register_spiller("belady", &belady_spiller);
	FIRM_DBG_REGISTER(dbg, "firm.be.spill.belady");
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_spillbelady);
