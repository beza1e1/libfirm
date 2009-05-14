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
 * @brief       Copy minimization driver.
 * @author      Daniel Grund
 * @date        12.04.2005
 * @version     $Id$
 *
 * Main file for the optimization reducing the copies needed for:
 * - Phi coalescing
 * - Register-constrained nodes
 * - Two-address code instructions
 */
#include "config.h"

#include "execfreq.h"
#include "xmalloc.h"
#include "debug.h"
#include "pmap.h"
#include "raw_bitset.h"
#include "irnode.h"
#include "irgraph.h"
#include "irgwalk.h"
#include "irprog.h"
#include "irloop_t.h"
#include "iredges_t.h"
#include "phiclass.h"
#include "irbitset.h"
#include "irphase_t.h"
#include "irprintf_t.h"

#include "bemodule.h"
#include "bearch_t.h"
#include "benode_t.h"
#include "beutil.h"
#include "beifg_t.h"
#include "beintlive_t.h"
#include "becopyopt_t.h"
#include "becopystat.h"
#include "belive_t.h"
#include "beinsn_t.h"
#include "besched_t.h"
#include "bestatevent.h"
#include "beirg_t.h"
#include "error.h"

#include "lc_opts.h"
#include "lc_opts_enum.h"

#define DUMP_BEFORE 1
#define DUMP_AFTER  2
#define DUMP_APPEL  4
#define DUMP_ALL    2 * DUMP_APPEL - 1

#define COST_FUNC_FREQ     1
#define COST_FUNC_LOOP     2
#define COST_FUNC_ALL_ONE  3

static unsigned   dump_flags  = 0;
static unsigned   style_flags = 0;
static unsigned   do_stats    = 0;
static cost_fct_t cost_func   = co_get_costs_exec_freq;
static unsigned   algo        = CO_ALGO_HEUR4;
static int        improve     = 1;

static const lc_opt_enum_mask_items_t dump_items[] = {
	{ "before",  DUMP_BEFORE },
	{ "after",   DUMP_AFTER  },
	{ "appel",   DUMP_APPEL  },
	{ "all",     DUMP_ALL    },
	{ NULL,      0 }
};

static const lc_opt_enum_mask_items_t style_items[] = {
	{ "color",   CO_IFG_DUMP_COLORS },
	{ "labels",  CO_IFG_DUMP_LABELS },
	{ "constr",  CO_IFG_DUMP_CONSTR },
	{ "shape",   CO_IFG_DUMP_SHAPE  },
	{ "full",    2 * CO_IFG_DUMP_SHAPE - 1 },
	{ NULL,      0 }
};

static const lc_opt_enum_mask_items_t algo_items[] = {
	{ "none",   CO_ALGO_NONE  },
	{ "heur",   CO_ALGO_HEUR  },
	{ "heur2",  CO_ALGO_HEUR2 },
	{ "heur3",  CO_ALGO_HEUR3 },
	{ "heur4",  CO_ALGO_HEUR4 },
	{ "ilp",    CO_ALGO_ILP   },
	{ NULL,     0 }
};

typedef int (*opt_funcptr)(void);

static const lc_opt_enum_func_ptr_items_t cost_func_items[] = {
	{ "freq",   (opt_funcptr) co_get_costs_exec_freq },
	{ "loop",   (opt_funcptr) co_get_costs_loop_depth },
	{ "one",    (opt_funcptr) co_get_costs_all_one },
	{ NULL,     NULL }
};

static lc_opt_enum_mask_var_t dump_var = {
	&dump_flags, dump_items
};

static lc_opt_enum_mask_var_t style_var = {
	&style_flags, style_items
};

static lc_opt_enum_mask_var_t algo_var = {
	&algo, algo_items
};

static lc_opt_enum_func_ptr_var_t cost_func_var = {
	(opt_funcptr*) &cost_func, cost_func_items
};

static const lc_opt_table_entry_t options[] = {
	LC_OPT_ENT_ENUM_INT      ("algo",    "select copy optimization algo",                           &algo_var),
	LC_OPT_ENT_ENUM_FUNC_PTR ("cost",    "select a cost function",                                  &cost_func_var),
	LC_OPT_ENT_ENUM_MASK     ("dump",    "dump ifg before or after copy optimization",              &dump_var),
	LC_OPT_ENT_ENUM_MASK     ("style",   "dump style for ifg dumping",                              &style_var),
	LC_OPT_ENT_BOOL          ("stats",   "dump statistics after each optimization",                 &do_stats),
	LC_OPT_ENT_BOOL          ("improve", "run heur3 before if algo can exploit start solutions",    &improve),
	LC_OPT_LAST
};

/* Insert additional options registration functions here. */
extern void be_co_ilp_register_options(lc_opt_entry_t *grp);
extern void be_co2_register_options(lc_opt_entry_t *grp);
extern void be_co3_register_options(lc_opt_entry_t *grp);

void be_init_copycoal(void)
{
	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *ra_grp = lc_opt_get_grp(be_grp, "ra");
	lc_opt_entry_t *chordal_grp = lc_opt_get_grp(ra_grp, "chordal");
	lc_opt_entry_t *co_grp = lc_opt_get_grp(chordal_grp, "co");

	lc_opt_add_table(co_grp, options);
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_copycoal);

#undef QUICK_AND_DIRTY_HACK

static int nodes_interfere(const be_chordal_env_t *env, const ir_node *a, const ir_node *b)
{
	if (env->ifg)
		return be_ifg_connected(env->ifg, a, b);
	else
		return values_interfere(env->birg, a, b);
}


/******************************************************************************
    _____                           _
   / ____|                         | |
  | |  __  ___ _ __   ___ _ __ __ _| |
  | | |_ |/ _ \ '_ \ / _ \ '__/ _` | |
  | |__| |  __/ | | |  __/ | | (_| | |
   \_____|\___|_| |_|\___|_|  \__,_|_|

 ******************************************************************************/

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)


copy_opt_t *new_copy_opt(be_chordal_env_t *chordal_env, cost_fct_t get_costs)
{
	const char *s1, *s2, *s3;
	int len;
	copy_opt_t *co;

	FIRM_DBG_REGISTER(dbg, "ir.be.copyopt");

	co = XMALLOCZ(copy_opt_t);
	co->cenv      = chordal_env;
	co->irg       = chordal_env->irg;
	co->cls       = chordal_env->cls;
	co->get_costs = get_costs;

	s1 = get_irp_name();
	s2 = get_entity_name(get_irg_entity(co->irg));
	s3 = chordal_env->cls->name;
	len = strlen(s1) + strlen(s2) + strlen(s3) + 5;
	co->name = XMALLOCN(char, len);
	snprintf(co->name, len, "%s__%s__%s", s1, s2, s3);

	return co;
}

void free_copy_opt(copy_opt_t *co) {
	xfree(co->name);
	free(co);
}

/**
 * Checks if a node is optimizable, viz. has something to do with coalescing
 * @param irn  The irn to check
 */
static int co_is_optimizable_root(ir_node *irn)
{
	const arch_register_req_t *req;
	const arch_register_t     *reg;

	if (arch_irn_is_ignore(irn))
		return 0;

	reg = arch_get_irn_register(irn);
	if (arch_register_type_is(reg, ignore))
		return 0;

	if (is_Reg_Phi(irn) || is_Perm_Proj(irn))
		return 1;

	req = arch_get_register_req_out(irn);
	if (is_2addr_code(req))
		return 1;

	return 0;
}

int co_get_costs_loop_depth(const copy_opt_t *co, ir_node *root, ir_node* arg, int pos) {
	int cost = 0;
	ir_loop *loop;
	ir_node *root_block = get_nodes_block(root);
	(void) co;
	(void) arg;

	if (is_Phi(root)) {
		/* for phis the copies are placed in the corresponding pred-block */
		loop = get_irn_loop(get_Block_cfgpred_block(root_block, pos));
	} else {
		/* a perm places the copy in the same block as it resides */
		loop = get_irn_loop(root_block);
	}
	if (loop) {
		int d = get_loop_depth(loop);
		cost = d*d;
	}
	return 1+cost;
}

int co_get_costs_exec_freq(const copy_opt_t *co, ir_node *root, ir_node* arg, int pos) {
	int res;
	ir_node *root_bl = get_nodes_block(root);
	ir_node *copy_bl = is_Phi(root) ? get_Block_cfgpred_block(root_bl, pos) : root_bl;
	(void) arg;
	res = get_block_execfreq_ulong(co->cenv->birg->exec_freq, copy_bl);

	/* don't allow values smaller than one. */
	return res < 1 ? 1 : res;
}


int co_get_costs_all_one(const copy_opt_t *co, ir_node *root, ir_node *arg, int pos) {
	(void) co;
	(void) root;
	(void) arg;
	(void) pos;
	return 1;
}

/******************************************************************************
   ____        _   _    _       _ _          _____ _
  / __ \      | | | |  | |     (_) |        / ____| |
 | |  | |_ __ | |_| |  | |_ __  _| |_ ___  | (___ | |_ ___  _ __ __ _  __ _  ___
 | |  | | '_ \| __| |  | | '_ \| | __/ __|  \___ \| __/ _ \| '__/ _` |/ _` |/ _ \
 | |__| | |_) | |_| |__| | | | | | |_\__ \  ____) | || (_) | | | (_| | (_| |  __/
  \____/| .__/ \__|\____/|_| |_|_|\__|___/ |_____/ \__\___/|_|  \__,_|\__, |\___|
        | |                                                            __/ |
        |_|                                                           |___/
 ******************************************************************************/

/**
 * Determines a maximum weighted independent set with respect to
 * the interference and conflict edges of all nodes in a qnode.
 */
static int ou_max_ind_set_costs(unit_t *ou) {
	be_chordal_env_t *chordal_env = ou->co->cenv;
	ir_node **safe, **unsafe;
	int i, o, safe_count, safe_costs, unsafe_count, *unsafe_costs;
	bitset_t *curr;
	bitset_pos_t pos;
	int max, curr_weight, best_weight = 0;

	/* assign the nodes into two groups.
	 * safe: node has no interference, hence it is in every max stable set.
	 * unsafe: node has an interference
	 */
	safe         = ALLOCAN(ir_node*, ou->node_count - 1);
	safe_costs   = 0;
	safe_count   = 0;
	unsafe       = ALLOCAN(ir_node*, ou->node_count - 1);
	unsafe_costs = ALLOCAN(int,      ou->node_count - 1);
	unsafe_count = 0;
	for(i=1; i<ou->node_count; ++i) {
		int is_safe = 1;
		for(o=1; o<ou->node_count; ++o) {
			if (i==o)
				continue;
			if (nodes_interfere(chordal_env, ou->nodes[i], ou->nodes[o])) {
				unsafe_costs[unsafe_count] = ou->costs[i];
				unsafe[unsafe_count] = ou->nodes[i];
				++unsafe_count;
				is_safe = 0;
				break;
			}
		}
		if (is_safe) {
			safe_costs += ou->costs[i];
			safe[safe_count++] = ou->nodes[i];
		}
	}


	/* now compute the best set out of the unsafe nodes*/
	if (unsafe_count > MIS_HEUR_TRIGGER) {
		bitset_t *best = bitset_alloca(unsafe_count);
		/* Heuristik: Greedy trial and error form index 0 to unsafe_count-1 */
		for (i=0; i<unsafe_count; ++i) {
			bitset_set(best, i);
			/* check if it is a stable set */
			for (o=bitset_next_set(best, 0); o!=-1 && o<i; o=bitset_next_set(best, o+1))
				if (nodes_interfere(chordal_env, unsafe[i], unsafe[o])) {
					bitset_clear(best, i); /* clear the bit and try next one */
					break;
				}
		}
		/* compute the weight */
		bitset_foreach(best, pos)
			best_weight += unsafe_costs[pos];
	} else {
		/* Exact Algorithm: Brute force */
		curr = bitset_alloca(unsafe_count);
		bitset_set_all(curr);
		while ((max = bitset_popcnt(curr)) != 0) {
			/* check if curr is a stable set */
			for (i=bitset_next_set(curr, 0); i!=-1; i=bitset_next_set(curr, i+1))
				for (o=bitset_next_set(curr, i+1); o!=-1; o=bitset_next_set(curr, o+1)) /* !!!!! difference to qnode_max_ind_set(): NOT (curr, i) */
						if (nodes_interfere(chordal_env, unsafe[i], unsafe[o]))
							goto no_stable_set;

			/* if we arrive here, we have a stable set */
			/* compute the weigth of the stable set*/
			curr_weight = 0;
			bitset_foreach(curr, pos)
				curr_weight += unsafe_costs[pos];

			/* any better ? */
			if (curr_weight > best_weight) {
				best_weight = curr_weight;
			}

	no_stable_set:
			bitset_minus1(curr);
		}
	}

	return safe_costs+best_weight;
}

static void co_collect_units(ir_node *irn, void *env)
{
	const arch_register_req_t *req = arch_get_register_req_out(irn);
	copy_opt_t                *co  = env;
	unit_t *unit;

	if (req->cls != co->cls)
		return;
	if (!co_is_optimizable_root(irn))
		return;

	/* Init a new unit */
	unit = XMALLOCZ(unit_t);
	unit->co = co;
	unit->node_count = 1;
	INIT_LIST_HEAD(&unit->queue);

	/* Phi with some/all of its arguments */
	if (is_Reg_Phi(irn)) {
		int i, arity;

		/* init */
		arity = get_irn_arity(irn);
		unit->nodes = XMALLOCN(ir_node*, arity + 1);
		unit->costs = XMALLOCN(int,      arity + 1);
		unit->nodes[0] = irn;

		/* fill */
		for (i=0; i<arity; ++i) {
			int o, arg_pos;
			ir_node *arg = get_irn_n(irn, i);

			assert(arch_get_irn_reg_class_out(arg) == co->cls && "Argument not in same register class.");
			if (arg == irn)
				continue;
			if (nodes_interfere(co->cenv, irn, arg)) {
				unit->inevitable_costs += co->get_costs(co, irn, arg, i);
				continue;
			}

			/* Else insert the argument of the phi to the members of this ou */
			DBG((dbg, LEVEL_1, "\t   Member: %+F\n", arg));

			if (arch_irn_is_ignore(arg))
				continue;

			/* Check if arg has occurred at a prior position in the arg/list */
			arg_pos = 0;
			for (o=1; o<unit->node_count; ++o) {
				if (unit->nodes[o] == arg) {
					arg_pos = o;
					break;
				}
			}

			if (!arg_pos) { /* a new argument */
				/* insert node, set costs */
				unit->nodes[unit->node_count] = arg;
				unit->costs[unit->node_count] = co->get_costs(co, irn, arg, i);
				unit->node_count++;
			} else { /* arg has occurred before in same phi */
				/* increase costs for existing arg */
				unit->costs[arg_pos] += co->get_costs(co, irn, arg, i);
			}
		}
		unit->nodes = XREALLOC(unit->nodes, ir_node*, unit->node_count);
		unit->costs = XREALLOC(unit->costs, int,      unit->node_count);
	} else if (is_Perm_Proj(irn)) {
		/* Proj of a perm with corresponding arg */
		assert(!nodes_interfere(co->cenv, irn, get_Perm_src(irn)));
		unit->nodes = XMALLOCN(ir_node*, 2);
		unit->costs = XMALLOCN(int,      2);
		unit->node_count = 2;
		unit->nodes[0] = irn;
		unit->nodes[1] = get_Perm_src(irn);
		unit->costs[1] = co->get_costs(co, irn, unit->nodes[1], -1);
	} else {
		/* Src == Tgt of a 2-addr-code instruction */
		if (is_2addr_code(req)) {
			const unsigned other = req->other_same;
			int            count = 0;
			int            i;

			for (i = 0; (1U << i) <= other; ++i) {
				if (other & (1U << i)) {
					ir_node *o  = get_irn_n(skip_Proj(irn), i);
					if (arch_irn_is_ignore(o))
						continue;
					if (nodes_interfere(co->cenv, irn, o))
						continue;
					++count;
				}
			}

			if (count != 0) {
				int k = 0;
				++count;
				unit->nodes = XMALLOCN(ir_node*, count);
				unit->costs = XMALLOCN(int,      count);
				unit->node_count = count;
				unit->nodes[k++] = irn;

				for (i = 0; 1U << i <= other; ++i) {
					if (other & (1U << i)) {
						ir_node *o  = get_irn_n(skip_Proj(irn), i);
						if (!arch_irn_is_ignore(o) &&
								!nodes_interfere(co->cenv, irn, o)) {
							unit->nodes[k] = o;
							unit->costs[k] = co->get_costs(co, irn, o, -1);
							++k;
						}
					}
				}
			}
		} else {
			assert(0 && "This is not an optimizable node!");
		}
	}

	/* Insert the new unit at a position according to its costs */
	if (unit->node_count > 1) {
		int i;
		struct list_head *tmp;

		/* Determine the maximum costs this unit can cause: all_nodes_cost */
		for(i=1; i<unit->node_count; ++i) {
			unit->sort_key = MAX(unit->sort_key, unit->costs[i]);
			unit->all_nodes_costs += unit->costs[i];
		}

		/* Determine the minimal costs this unit will cause: min_nodes_costs */
		unit->min_nodes_costs += unit->all_nodes_costs - ou_max_ind_set_costs(unit);
		/* Insert the new ou according to its sort_key */
		tmp = &co->units;
		while (tmp->next != &co->units && list_entry_units(tmp->next)->sort_key > unit->sort_key)
			tmp = tmp->next;
		list_add(&unit->units, tmp);
	} else {
		free(unit);
	}
}

#ifdef QUICK_AND_DIRTY_HACK

static int compare_ous(const void *k1, const void *k2) {
	const unit_t *u1 = *((const unit_t **) k1);
	const unit_t *u2 = *((const unit_t **) k2);
	int i, o, u1_has_constr, u2_has_constr;
	arch_register_req_t req;

	/* Units with constraints come first */
	u1_has_constr = 0;
	for (i=0; i<u1->node_count; ++i) {
		arch_get_register_req_out(&req, u1->nodes[i]);
		if (arch_register_req_is(&req, limited)) {
			u1_has_constr = 1;
			break;
		}
	}

	u2_has_constr = 0;
	for (i=0; i<u2->node_count; ++i) {
		arch_get_register_req_out(&req, u2->nodes[i]);
		if (arch_register_req_is(&req, limited)) {
			u2_has_constr = 1;
			break;
		}
	}

	if (u1_has_constr != u2_has_constr)
		return u2_has_constr - u1_has_constr;

	/* Now check, whether the two units are connected */
#if 0
	for (i=0; i<u1->node_count; ++i)
		for (o=0; o<u2->node_count; ++o)
			if (u1->nodes[i] == u2->nodes[o])
				return 0;
#endif

	/* After all, the sort key decides. Greater keys come first. */
	return u2->sort_key - u1->sort_key;

}

/**
 * Sort the ou's according to constraints and their sort_key
 */
static void co_sort_units(copy_opt_t *co) {
	int i, count = 0, costs;
	unit_t *ou, **ous;

	/* get the number of ous, remove them form the list and fill the array */
	list_for_each_entry(unit_t, ou, &co->units, units)
		count++;
	ous = ALLOCAN(unit_t, count);

	costs = co_get_max_copy_costs(co);

	i = 0;
	list_for_each_entry(unit_t, ou, &co->units, units)
		ous[i++] = ou;

	INIT_LIST_HEAD(&co->units);

	assert(count == i && list_empty(&co->units));

	for (i=0; i<count; ++i)
		ir_printf("%+F\n", ous[i]->nodes[0]);

	qsort(ous, count, sizeof(*ous), compare_ous);

	ir_printf("\n\n");
	for (i=0; i<count; ++i)
		ir_printf("%+F\n", ous[i]->nodes[0]);

	/* reinsert into list in correct order */
	for (i=0; i<count; ++i)
		list_add_tail(&ous[i]->units, &co->units);

	assert(costs == co_get_max_copy_costs(co));
}
#endif

void co_build_ou_structure(copy_opt_t *co) {
	DBG((dbg, LEVEL_1, "\tCollecting optimization units\n"));
	INIT_LIST_HEAD(&co->units);
	irg_walk_graph(co->irg, co_collect_units, NULL, co);
#ifdef QUICK_AND_DIRTY_HACK
	co_sort_units(co);
#endif
}

void co_free_ou_structure(copy_opt_t *co) {
	unit_t *curr, *tmp;
	ASSERT_OU_AVAIL(co);
	list_for_each_entry_safe(unit_t, curr, tmp, &co->units, units) {
		xfree(curr->nodes);
		xfree(curr->costs);
		xfree(curr);
	}
	co->units.next = NULL;
}

/* co_solve_heuristic() is implemented in becopyheur.c */

int co_get_max_copy_costs(const copy_opt_t *co) {
	int i, res = 0;
	unit_t *curr;

	ASSERT_OU_AVAIL(co);

	list_for_each_entry(unit_t, curr, &co->units, units) {
		res += curr->inevitable_costs;
		for (i=1; i<curr->node_count; ++i)
			res += curr->costs[i];
	}
	return res;
}

int co_get_inevit_copy_costs(const copy_opt_t *co) {
	int res = 0;
	unit_t *curr;

	ASSERT_OU_AVAIL(co);

	list_for_each_entry(unit_t, curr, &co->units, units)
		res += curr->inevitable_costs;
	return res;
}

int co_get_copy_costs(const copy_opt_t *co) {
	int i, res = 0;
	unit_t *curr;

	ASSERT_OU_AVAIL(co);

	list_for_each_entry(unit_t, curr, &co->units, units) {
		int root_col = get_irn_col(curr->nodes[0]);
		DBG((dbg, LEVEL_1, "  %3d costs for root %+F color %d\n", curr->inevitable_costs, curr->nodes[0], root_col));
		res += curr->inevitable_costs;
		for (i=1; i<curr->node_count; ++i) {
			int arg_col = get_irn_col(curr->nodes[i]);
			if (root_col != arg_col) {
				DBG((dbg, LEVEL_1, "  %3d for arg %+F color %d\n", curr->costs[i], curr->nodes[i], arg_col));
				res += curr->costs[i];
			}
		}
	}
	return res;
}

int co_get_lower_bound(const copy_opt_t *co) {
	int res = 0;
	unit_t *curr;

	ASSERT_OU_AVAIL(co);

	list_for_each_entry(unit_t, curr, &co->units, units)
		res += curr->inevitable_costs + curr->min_nodes_costs;
	return res;
}

void co_complete_stats(const copy_opt_t *co, co_complete_stats_t *stat)
{
	bitset_t *seen = bitset_irg_malloc(co->irg);
	affinity_node_t *an;

	memset(stat, 0, sizeof(stat[0]));

	/* count affinity edges. */
	co_gs_foreach_aff_node(co, an) {
		neighb_t *neigh;
		stat->aff_nodes += 1;
		bitset_add_irn(seen, an->irn);
		co_gs_foreach_neighb(an, neigh) {
			if(!bitset_contains_irn(seen, neigh->irn)) {
				stat->aff_edges += 1;
				stat->max_costs += neigh->costs;

				if (get_irn_col(an->irn) != get_irn_col(neigh->irn)) {
					stat->costs += neigh->costs;
					stat->unsatisfied_edges += 1;
				}

				if(nodes_interfere(co->cenv, an->irn, neigh->irn)) {
					stat->aff_int += 1;
					stat->inevit_costs += neigh->costs;
				}

			}
		}
	}

	bitset_free(seen);
}

/******************************************************************************
   _____                 _        _____ _
  / ____|               | |      / ____| |
 | |  __ _ __ __ _ _ __ | |__   | (___ | |_ ___  _ __ __ _  __ _  ___
 | | |_ | '__/ _` | '_ \| '_ \   \___ \| __/ _ \| '__/ _` |/ _` |/ _ \
 | |__| | | | (_| | |_) | | | |  ____) | || (_) | | | (_| | (_| |  __/
  \_____|_|  \__,_| .__/|_| |_| |_____/ \__\___/|_|  \__,_|\__, |\___|
                  | |                                       __/ |
                  |_|                                      |___/
 ******************************************************************************/

static int compare_affinity_node_t(const void *k1, const void *k2, size_t size) {
	const affinity_node_t *n1 = k1;
	const affinity_node_t *n2 = k2;
	(void) size;

	return (n1->irn != n2->irn);
}

static void add_edge(copy_opt_t *co, ir_node *n1, ir_node *n2, int costs) {
	affinity_node_t new_node, *node;
	neighb_t        *nbr;
	int             allocnew = 1;

	new_node.irn        = n1;
	new_node.degree     = 0;
	new_node.neighbours = NULL;
	node = set_insert(co->nodes, &new_node, sizeof(new_node), hash_irn(new_node.irn));

	for (nbr = node->neighbours; nbr; nbr = nbr->next)
		if (nbr->irn == n2) {
			allocnew = 0;
			break;
		}

	/* if we did not find n2 in n1's neighbourhood insert it */
	if (allocnew) {
		nbr        = obstack_alloc(&co->obst, sizeof(*nbr));
		nbr->irn   = n2;
		nbr->costs = 0;
		nbr->next  = node->neighbours;

		node->neighbours = nbr;
		node->degree++;
	}

	/* now nbr points to n1's neighbour-entry of n2 */
	nbr->costs += costs;
}

static inline void add_edges(copy_opt_t *co, ir_node *n1, ir_node *n2, int costs) {
	if (! be_ifg_connected(co->cenv->ifg, n1, n2)) {
		add_edge(co, n1, n2, costs);
		add_edge(co, n2, n1, costs);
	}
}

static void build_graph_walker(ir_node *irn, void *env) {
	const arch_register_req_t *req = arch_get_register_req_out(irn);
	copy_opt_t                *co  = env;
	int pos, max;
	const arch_register_t *reg;

	if (req->cls != co->cls || arch_irn_is_ignore(irn))
		return;

	reg = arch_get_irn_register(irn);
	if (arch_register_type_is(reg, ignore))
		return;

	if (is_Reg_Phi(irn)) { /* Phis */
		for (pos=0, max=get_irn_arity(irn); pos<max; ++pos) {
			ir_node *arg = get_irn_n(irn, pos);
			add_edges(co, irn, arg, co->get_costs(co, irn, arg, pos));
		}
	} else if (is_Perm_Proj(irn)) { /* Perms */
		ir_node *arg = get_Perm_src(irn);
		add_edges(co, irn, arg, co->get_costs(co, irn, arg, 0));
	} else { /* 2-address code */
		if (is_2addr_code(req)) {
			const unsigned other = req->other_same;
			int i;

			for (i = 0; 1U << i <= other; ++i) {
				if (other & (1U << i)) {
					ir_node *other = get_irn_n(skip_Proj(irn), i);
					if (!arch_irn_is_ignore(other))
						add_edges(co, irn, other, co->get_costs(co, irn, other, 0));
				}
			}
		}
	}
}

void co_build_graph_structure(copy_opt_t *co) {
	obstack_init(&co->obst);
	co->nodes = new_set(compare_affinity_node_t, 32);

	irg_walk_graph(co->irg, build_graph_walker, NULL, co);
}

void co_free_graph_structure(copy_opt_t *co) {
	ASSERT_GS_AVAIL(co);

	del_set(co->nodes);
	obstack_free(&co->obst, NULL);
	co->nodes = NULL;
}

/* co_solve_ilp1() co_solve_ilp2() are implemented in becopyilpX.c */

int co_gs_is_optimizable(copy_opt_t *co, ir_node *irn) {
	affinity_node_t new_node, *n;

	ASSERT_GS_AVAIL(co);

	new_node.irn = irn;
	n = set_find(co->nodes, &new_node, sizeof(new_node), hash_irn(new_node.irn));
	if (n) {
		return (n->degree > 0);
	} else
		return 0;
}

static int co_dump_appel_disjoint_constraints(const copy_opt_t *co, ir_node *a, ir_node *b)
{
	ir_node *nodes[]  = { a, b };
	bitset_t *constr[] = { NULL, NULL };
	int j;

	constr[0] = bitset_alloca(co->cls->n_regs);
	constr[1] = bitset_alloca(co->cls->n_regs);

	for (j = 0; j < 2; ++j) {
		const arch_register_req_t *req = arch_get_register_req_out(nodes[j]);
		if(arch_register_req_is(req, limited))
			rbitset_copy_to_bitset(req->limited, constr[j]);
		else
			bitset_set_all(constr[j]);

	}

	return !bitset_intersect(constr[0], constr[1]);
}

void co_dump_appel_graph(const copy_opt_t *co, FILE *f)
{
	be_ifg_t *ifg       = co->cenv->ifg;
	int      *color_map = ALLOCAN(int, co->cls->n_regs);
	int      *node_map  = XMALLOCN(int, get_irg_last_idx(co->irg) + 1);

	ir_node *irn;
	void *it, *nit;
	int n, n_regs;
	unsigned i;

	n_regs = 0;
	for(i = 0; i < co->cls->n_regs; ++i) {
		const arch_register_t *reg = &co->cls->regs[i];
		color_map[i] = arch_register_type_is(reg, ignore) ? -1 : n_regs++;
	}

	/*
	 * n contains the first node number.
	 * the values below n are the pre-colored register nodes
	 */

	it  = be_ifg_nodes_iter_alloca(ifg);
	nit = be_ifg_neighbours_iter_alloca(ifg);

	n = n_regs;
	be_ifg_foreach_node(ifg, it, irn) {
		if (arch_irn_is_ignore(irn))
			continue;
		node_map[get_irn_idx(irn)] = n++;
	}

	fprintf(f, "%d %d\n", n, n_regs);

	be_ifg_foreach_node(ifg, it, irn) {
		if (!arch_irn_is_ignore(irn)) {
			int idx                        = node_map[get_irn_idx(irn)];
			affinity_node_t           *a   = get_affinity_info(co, irn);
			const arch_register_req_t *req = arch_get_register_req_out(irn);
			ir_node                   *adj;

			if(arch_register_req_is(req, limited)) {
				for(i = 0; i < co->cls->n_regs; ++i) {
					if(!rbitset_is_set(req->limited, i) && color_map[i] >= 0)
						fprintf(f, "%d %d -1\n", color_map[i], idx);
				}
			}

			be_ifg_foreach_neighbour(ifg, nit, irn, adj) {
				if (!arch_irn_is_ignore(adj) &&
						!co_dump_appel_disjoint_constraints(co, irn, adj)) {
					int adj_idx = node_map[get_irn_idx(adj)];
					if(idx < adj_idx)
						fprintf(f, "%d %d -1\n", idx, adj_idx);
				}
			}

			if(a) {
				neighb_t *n;

				co_gs_foreach_neighb(a, n) {
					if (!arch_irn_is_ignore(n->irn)) {
						int n_idx = node_map[get_irn_idx(n->irn)];
						if(idx < n_idx)
							fprintf(f, "%d %d %d\n", idx, n_idx, (int) n->costs);
					}
				}
			}
		}
	}

	xfree(node_map);
}

/*
	 ___ _____ ____   ____   ___ _____   ____                        _
	|_ _|  ___/ ___| |  _ \ / _ \_   _| |  _ \ _   _ _ __ ___  _ __ (_)_ __   __ _
	 | || |_ | |  _  | | | | | | || |   | | | | | | | '_ ` _ \| '_ \| | '_ \ / _` |
	 | ||  _|| |_| | | |_| | |_| || |   | |_| | |_| | | | | | | |_) | | | | | (_| |
	|___|_|   \____| |____/ \___/ |_|   |____/ \__,_|_| |_| |_| .__/|_|_| |_|\__, |
	                                                          |_|            |___/
*/

static const char *get_dot_color_name(size_t col)
{
	static const char *names[] = {
		"blue",
		"red",
		"green",
		"yellow",
		"cyan",
		"magenta",
		"orange",
		"chocolate",
		"beige",
		"navy",
		"darkgreen",
		"darkred",
		"lightPink",
		"chartreuse",
		"lightskyblue",
		"linen",
		"pink",
		"lightslateblue",
		"mintcream",
		"red",
		"darkolivegreen",
		"mediumblue",
		"mistyrose",
		"salmon",
		"darkseagreen",
		"mediumslateblue"
		"moccasin",
		"tomato",
		"forestgreen",
		"darkturquoise",
		"palevioletred"
	};

	return col < sizeof(names)/sizeof(names[0]) ? names[col] : "white";
}

typedef struct _co_ifg_dump_t {
	const copy_opt_t *co;
	unsigned flags;
} co_ifg_dump_t;

static void ifg_dump_graph_attr(FILE *f, void *self)
{
	(void) self;
	fprintf(f, "overlap=scale");
}

static int ifg_is_dump_node(void *self, ir_node *irn)
{
	(void)self;
	return !arch_irn_is_ignore(irn);
}

static void ifg_dump_node_attr(FILE *f, void *self, ir_node *irn)
{
	co_ifg_dump_t             *env     = self;
	const arch_register_t     *reg     = arch_get_irn_register(irn);
	const arch_register_req_t *req     = arch_get_register_req_out(irn);
	int                        limited = arch_register_req_is(req, limited);

	if(env->flags & CO_IFG_DUMP_LABELS) {
		ir_fprintf(f, "label=\"%+F", irn);

		if((env->flags & CO_IFG_DUMP_CONSTR) && limited) {
			bitset_t *bs = bitset_alloca(env->co->cls->n_regs);
			rbitset_copy_to_bitset(req->limited, bs);
			ir_fprintf(f, "\\n%B", bs);
		}
		ir_fprintf(f, "\" ");
	} else {
		fprintf(f, "label=\"\" shape=point " );
	}

	if(env->flags & CO_IFG_DUMP_SHAPE)
		fprintf(f, "shape=%s ", limited ? "diamond" : "ellipse");

	if(env->flags & CO_IFG_DUMP_COLORS)
		fprintf(f, "style=filled color=%s ", get_dot_color_name(reg->index));
}

static void ifg_dump_at_end(FILE *file, void *self)
{
	co_ifg_dump_t *env = self;
	affinity_node_t *a;

	co_gs_foreach_aff_node(env->co, a) {
		const arch_register_t *ar = arch_get_irn_register(a->irn);
		unsigned aidx = get_irn_idx(a->irn);
		neighb_t *n;

		co_gs_foreach_neighb(a, n) {
			const arch_register_t *nr = arch_get_irn_register(n->irn);
			unsigned nidx = get_irn_idx(n->irn);

			if(aidx < nidx) {
				const char *color = nr == ar ? "blue" : "red";
				fprintf(file, "\tn%d -- n%d [weight=0.01 ", aidx, nidx);
				if(env->flags & CO_IFG_DUMP_LABELS)
					fprintf(file, "label=\"%d\" ", n->costs);
				if(env->flags & CO_IFG_DUMP_COLORS)
					fprintf(file, "color=%s ", color);
				else
					fprintf(file, "style=dotted");
				fprintf(file, "];\n");
			}
		}
	}
}


static be_ifg_dump_dot_cb_t ifg_dot_cb = {
	ifg_is_dump_node,
	ifg_dump_graph_attr,
	ifg_dump_node_attr,
	NULL,
	NULL,
	ifg_dump_at_end
};



void co_dump_ifg_dot(const copy_opt_t *co, FILE *f, unsigned flags)
{
	co_ifg_dump_t cod;

	cod.co    = co;
	cod.flags = flags;
	be_ifg_dump_dot(co->cenv->ifg, co->irg, f, &ifg_dot_cb, &cod);
}


void co_solve_park_moon(copy_opt_t *opt)
{
	(void) opt;
}

static int void_algo(copy_opt_t *co)
{
	(void) co;
	return 0;
}

/*
		_    _                  _ _   _
	   / \  | | __ _  ___  _ __(_) |_| |__  _ __ ___  ___
	  / _ \ | |/ _` |/ _ \| '__| | __| '_ \| '_ ` _ \/ __|
	 / ___ \| | (_| | (_) | |  | | |_| | | | | | | | \__ \
	/_/   \_\_|\__, |\___/|_|  |_|\__|_| |_|_| |_| |_|___/
			   |___/
*/

typedef struct {
	co_algo_t  *algo;
	const char *name;
	int        can_improve_existing;
} co_algo_info_t;

static co_algo_info_t algos[] = {
	{ void_algo,               "none",  0 },
	{ co_solve_heuristic,      "heur1", 0 },
	{ co_solve_heuristic_new,  "heur2", 0 },
#ifdef WITH_JVM
	{ co_solve_heuristic_java, "heur3", 0 },
#else
	{ NULL,                    "heur3", 0 },
#endif
	{ co_solve_heuristic_mst,  "heur4", 0 },
#ifdef WITH_ILP
	{ co_solve_ilp2,           "ilp",   1 },
#else
	{ NULL,                    "ilp",   1 },
#endif
	{ NULL,                    "",      0 }
};

/*
    __  __       _         ____       _
   |  \/  | __ _(_)_ __   |  _ \ _ __(_)_   _____ _ __
   | |\/| |/ _` | | '_ \  | | | | '__| \ \ / / _ \ '__|
   | |  | | (_| | | | | | | |_| | |  | |\ V /  __/ |
   |_|  |_|\__,_|_|_| |_| |____/|_|  |_| \_/ \___|_|

*/

static FILE *my_open(const be_chordal_env_t *env, const char *prefix, const char *suffix)
{
	FILE *result;
	char buf[1024];
	size_t i, n;
	char *tu_name;

	n = strlen(env->birg->main_env->cup_name);
	tu_name = XMALLOCN(char, n + 1);
	strcpy(tu_name, env->birg->main_env->cup_name);
	for (i = 0; i < n; ++i)
		if (tu_name[i] == '.')
			tu_name[i] = '_';


	ir_snprintf(buf, sizeof(buf), "%s%s_%F_%s%s", prefix, tu_name, env->irg, env->cls->name, suffix);
	xfree(tu_name);
	result = fopen(buf, "wt");
	if(result == NULL) {
		panic("Couldn't open '%s' for writing.", buf);
	}

	return result;
}

void co_driver(be_chordal_env_t *cenv)
{
	ir_timer_t          *timer = ir_timer_register("firm.be.copyopt", "runtime");
	co_complete_stats_t before, after;
	copy_opt_t          *co;
	co_algo_t           *algo_func;
	int                 was_optimal = 0;

	if (algo >= CO_ALGO_LAST)
		return;

	be_liveness_assure_chk(be_get_birg_liveness(cenv->birg));

	co = new_copy_opt(cenv, cost_func);
	co_build_ou_structure(co);
	co_build_graph_structure(co);

	co_complete_stats(co, &before);

	be_stat_ev_ull("co_aff_nodes",    before.aff_nodes);
	be_stat_ev_ull("co_aff_edges",    before.aff_edges);
	be_stat_ev_ull("co_max_costs",    before.max_costs);
	be_stat_ev_ull("co_inevit_costs", before.inevit_costs);
	be_stat_ev_ull("co_aff_int",      before.aff_int);

	be_stat_ev_ull("co_init_costs",   before.costs);
	be_stat_ev_ull("co_init_unsat",   before.unsatisfied_edges);

	if (dump_flags & DUMP_BEFORE) {
		FILE *f = my_open(cenv, "", "-before.dot");
		co_dump_ifg_dot(co, f, style_flags);
		fclose(f);
	}

	/* if the algo can improve results, provide an initial solution with heur3 */
	if (improve && algos[algo].can_improve_existing) {
		co_complete_stats_t stats;

		/* produce a heuristic solution */
#ifdef WITH_JVM
		co_solve_heuristic_java(co);
#else
		co_solve_heuristic(co);
#endif /* WITH_JVM */

		/* do the stats and provide the current costs */
		co_complete_stats(co, &stats);
		be_stat_ev_ull("co_prepare_costs", stats.costs);
	}

#ifdef WITH_JVM
	/* start the JVM here so that it does not tamper the timing. */
	if (algo == CO_ALGO_HEUR3)
		be_java_coal_start_jvm();
#endif /* WITH_JVM */

	algo_func = algos[algo].algo;

	/* perform actual copy minimization */
	ir_timer_reset_and_start(timer);
	was_optimal = algo_func(co);
	ir_timer_stop(timer);

	be_stat_ev("co_time", ir_timer_elapsed_msec(timer));
	be_stat_ev_ull("co_optimal", was_optimal);

	if (dump_flags & DUMP_AFTER) {
		FILE *f = my_open(cenv, "", "-after.dot");
		co_dump_ifg_dot(co, f, style_flags);
		fclose(f);
	}

	co_complete_stats(co, &after);

	if (do_stats) {
		ulong64 optimizable_costs = after.max_costs - after.inevit_costs;
		ulong64 evitable          = after.costs     - after.inevit_costs;

		ir_printf("%30F ", cenv->irg);
		printf("%10s %10" ULL_FMT "%10" ULL_FMT "%10" ULL_FMT, cenv->cls->name, after.max_costs, before.costs, after.inevit_costs);

		if(optimizable_costs > 0)
			printf("%10" ULL_FMT " %5.2f\n", after.costs, (evitable * 100.0) / optimizable_costs);
		else
			printf("%10" ULL_FMT " %5s\n", after.costs, "-");
	}

	/* Dump the interference graph in Appel's format. */
	if (dump_flags & DUMP_APPEL) {
		FILE *f = my_open(cenv, "", ".apl");
		fprintf(f, "# %lld %lld\n", after.costs, after.unsatisfied_edges);
		co_dump_appel_graph(co, f);
		fclose(f);
	}

	be_stat_ev_ull("co_after_costs", after.costs);
	be_stat_ev_ull("co_after_unsat", after.unsatisfied_edges);

	co_free_graph_structure(co);
	co_free_ou_structure(co);
	free_copy_opt(co);
}
