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
 * @brief    Interprocedural analysis to improve the call graph estimate.
 * @author   Florian Liekweg
 * @version  09.06.2002
 * @version  $Id$
 */
#include "config.h"

#include "rta.h"

#include <stdlib.h>
#include <stdbool.h>

#include "irnode_t.h"
#include "irprog_t.h"
#include "irgraph_t.h"

#include "pset_new.h"
#include "irgwalk.h"
#include "irgmod.h"
#include "irverify.h"
#include "irprintf.h"
#include "debug.h"
#include "error.h"

/** The debug handle. */
DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

/* base data */
static pset_new_t *_live_classes = NULL;

/* cache computed results */
static pset_new_t *_live_graphs  = NULL;

/**
 * Add a graph to the set of live graphs.
 *
 * @param graph  the graph to add
 * @return non-zero if the graph was added, zero
 *         if it was already in the live set
 */
static bool add_graph(ir_graph *graph)
{
	if (!pset_new_contains(_live_graphs, graph)) {
		DB((dbg, LEVEL_2, "RTA:        new graph of %+F\n", graph));

		pset_new_insert(_live_graphs, graph);
		return true;
	}

	return false;
}

/**
 * Add a class to the set of live classes.
 *
 * @param clazz   the class to add
 * @return non-zero if the graph was added, zero
 *         if it was already in the live set
 */
static bool add_class(ir_type *clazz)
{
	if (!pset_new_contains(_live_classes, clazz)) {
		DB((dbg, LEVEL_2, "RTA:        new class: %+F\n", clazz));

		pset_new_insert(_live_classes, clazz);
		return true;
	}

	return false;
}

/** Given an entity, add all implementing graphs that belong to live classes
 *  to _live_graphs.
 *
 *  Iff additions occurred, return true, else false.
*/
static bool add_implementing_graphs(ir_entity *method)
{
	int i;
	int n_over = get_entity_n_overwrittenby(method);
	ir_graph *graph = get_entity_irg(method);
	bool change = false;

	if (NULL == graph)
		graph = get_entity_irg(method);

	DB((dbg, LEVEL_2, "RTA:        new call to %+F\n", method));

	if (rta_is_alive_class(get_entity_owner(method))) {
		if (NULL != graph)
			change = add_graph(graph);
	}

	for (i = 0; i < n_over; ++i) {
		ir_entity *over = get_entity_overwrittenby(method, i);
		change |= add_implementing_graphs(over);
	}

	return change;
}

/**
 * Walker: Enter all method accesses and all class allocations into
 * our tables.
 *
 * Set *(int*)env to true iff (possibly) new graphs have been found.
 */
static void rta_act(ir_node *node, void *env)
{
	bool *change = (bool*)env;
	ir_opcode op = get_irn_opcode(node);

	if (iro_Call == op) {         /* CALL */
		ir_entity *ent = NULL;

		ir_node *ptr = get_Call_ptr(node);

		/* CALL SEL */
		if (iro_Sel == get_irn_opcode(ptr)) {
			ent = get_Sel_entity(ptr);
			*change |= add_implementing_graphs(ent);

			/* CALL SYMCONST */
		} else if (iro_SymConst == get_irn_opcode(ptr)) {
			if (get_SymConst_kind(ptr) == symconst_addr_ent) {
				ir_graph *graph;

				ent = get_SymConst_entity(ptr);
				graph = get_entity_irg(ent);
				if (graph) {
					*change |= add_graph(graph);
				} else {
					/* it's an external allocated thing. */
				}
			} else {
				/* other symconst. */
				panic("This SymConst can not be an address for a method call.");
			}

			/* STRANGE */
		} else {
			panic("Unexpected address expression: can not analyse, therefore can not do correct rta!");
		}
	} else if (iro_Alloc == op) { /* ALLOC */
		ir_type *type = get_Alloc_type(node);

		*change |= add_class(type);
	}
}

/**
   Traverse the given graph to collect method accesses and
   object allocations.
*/
static bool rta_fill_graph(ir_graph* graph)
{
	bool change = false;
	irg_walk_graph(graph, rta_act, NULL, &change);
	return change;
}

/**
 * Traverse all graphs to collect method accesses and object allocations.
 */
static int rta_fill_incremental(void)
{
	int  i;
	int  n_runs = 0;
	bool rerun  = true;

	/* init_tables has added main_irg to _live_graphs */

	/* Need to take care of graphs that are externally
	   visible or sticky. Pretend that they are called: */
	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		ir_graph *graph = get_irp_irg(i);
		ir_entity *ent = get_irg_entity(graph);
		ir_linkage linkage = get_entity_linkage(ent);

		if (entity_is_externally_visible(ent)
				|| (linkage & IR_LINKAGE_HIDDEN_USER)) {
			pset_new_insert(_live_graphs, graph);
		}
	}

	while (rerun) {
		ir_graph *graph;
		pset_new_iterator_t iter;

		/* start off new */
		pset_new_t *live_graphs = _live_graphs;
		_live_graphs = XMALLOC(pset_new_t);
		pset_new_init(_live_graphs);

		DB((dbg, LEVEL_2, "RTA: RUN %i\n", n_runs));

		/* collect what we have found previously */
		foreach_pset_new(live_graphs, graph, iter) {
			pset_new_insert(_live_graphs, graph);
		}

		rerun = false;
		foreach_pset_new(live_graphs, graph, iter) {
			DB((dbg, LEVEL_2, "RTA: RUN %i: considering graph of %+F\n", n_runs, graph));
			rerun |= rta_fill_graph(graph);
		}
		pset_new_destroy(live_graphs);
		free(live_graphs);
		++n_runs;
	}

	return n_runs;
}

#ifdef DEBUG_libfirm
/**
 * Count the number of graphs that we have found to be live.
 */
static int stats(void)
{
	int i;
	int n_live_graphs = 0;

	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		ir_graph *graph = get_irp_irg(i);

		if (rta_is_alive_graph(graph))
			++n_live_graphs;
	}

	return n_live_graphs;
}
#endif

/* remove a graph, part I */
/*
   We removed the first graph to implement the entity, so we're
   abstract now.  Pretend that it wasn't there at all, and every
   entity that used to inherit this entity's graph is now abstract.
*/

/**
   Initialize the static data structures.
*/
static void init_tables(void)
{
	ir_type  *tp;
	int      i, n;
	ir_graph *irg;

	_live_classes = XMALLOC(pset_new_t);
	pset_new_init(_live_classes);
	_live_graphs  = XMALLOC(pset_new_t);
	pset_new_init(_live_graphs);

	irg = get_irp_main_irg();
	if (irg != NULL) {
		/* add the main irg to the live set if one is specified */
		pset_new_insert(_live_graphs, irg);
	}

	/* Find static allocated classes */
	tp = get_glob_type();
	n  = get_class_n_members(tp);
	for (i = 0; i < n; ++i) {
		ir_type *member_type = get_entity_type(get_class_member(tp, i));
		if (is_Class_type(member_type))
			pset_new_insert(_live_classes, member_type);
	}

	tp = get_tls_type();
	n  = get_struct_n_members(tp);
	for (i = 0; i < n; ++i) {
		ir_type *member_type = get_entity_type(get_struct_member(tp, i));
		if (is_Class_type(member_type))
			pset_new_insert(_live_classes, member_type);
	}

	/** @FIXME: add constructors/destructors */
}

/*
 * Initialize the RTA data structures, and perform RTA.
 */
void rta_init(void)
{
	int n_runs = 0;

	FIRM_DBG_REGISTER(dbg, "firm.ana.rta");

# ifdef DEBUG_libfirm
	{
		int i;
		for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
			irg_verify(get_irp_irg(i), 0);
		}
		tr_verify();
	}
# endif /* defined DEBUG_libfirm */

	init_tables();

	n_runs = rta_fill_incremental();

	DB((dbg, LEVEL_1, "RTA: n_graphs      = %i\n", get_irp_n_irgs()));
	DB((dbg, LEVEL_1, "RTA: n_live_graphs = %i\n", stats()));
	DB((dbg, LEVEL_1, "RTA: n_runs        = %i\n", n_runs));

# ifdef DEBUG_libfirm
	{
		int i;

		for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
			irg_verify(get_irp_irg(i), 0);
		}
		tr_verify();
	}
# endif /* defined DEBUG_libfirm */
}

/**
 * walker for all types and entities
 *
 * Changes the peculiarity of entities that represents
 * dead graphs to peculiarity_description.
 */
static void make_entity_to_description(type_or_ent tore, void *env)
{
	(void) env;
	if (is_entity(tore.ent)) {
		ir_entity *ent = tore.ent;

		if ((is_Method_type(get_entity_type(ent))) &&
			!entity_is_externally_visible(ent)) {
			ir_graph *irg = get_entity_irg(ent);
			if (irg != NULL && ! pset_new_contains(_live_graphs, irg)) {
				set_entity_peculiarity(ent, peculiarity_description);
				set_entity_irg(ent, NULL);
			}
		}
	}
}

/* Delete all graphs that we have found to be dead from the program
   If verbose == 1, print statistics, if > 1, chatter about every detail
*/
void rta_delete_dead_graphs(void)
{
	int      i, n_dead_irgs, n_graphs = get_irp_n_irgs();
	ir_graph *irg, *next_irg, *dead_irgs;

	irp_reserve_resources(irp, IR_RESOURCE_IRG_LINK);

	n_dead_irgs = 0;
	dead_irgs = NULL;
	for (i = n_graphs - 1; i >= 0; --i) {
		irg = get_irp_irg(i);

		if (! rta_is_alive_graph(irg)) {
			set_irg_link(irg, dead_irgs);
			dead_irgs = irg;
			++n_dead_irgs;
		}
	}

	/* Hmm, probably we need to run this only if dead_irgs is non-NULL */
	type_walk(make_entity_to_description, NULL, NULL);
	for (irg = dead_irgs; irg != NULL; irg = next_irg) {
		next_irg = get_irg_link(irg);
		remove_irp_irg(irg);
	}

	DB((dbg, LEVEL_1, "RTA: dead methods = %i\n", n_dead_irgs));

	irp_free_resources(irp, IR_RESOURCE_IRG_LINK);
}

/* Clean up the RTA data structures.  Call this after calling rta_init */
void rta_cleanup(void)
{
# ifdef DEBUG_libfirm
	int i;
	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		irg_verify(get_irp_irg(i), 0);
	}
	tr_verify();
# endif /* defined DEBUG_libfirm */

	if (_live_classes != NULL) {
		pset_new_destroy(_live_classes);
		free(_live_classes);
		_live_classes = NULL;
	}

	if (_live_graphs != NULL) {
		pset_new_destroy(_live_graphs);
		free(_live_graphs);
		_live_graphs = NULL;
	}
}

/* Say whether this class might be instantiated at any point in the program: */
int rta_is_alive_class(ir_type *clazz)
{
	return pset_new_contains(_live_classes, clazz);
}

/* Say whether this graph might be run at any time in the program: */
int rta_is_alive_graph(ir_graph *graph)
{
	return pset_new_contains(_live_graphs, graph);
}

/* dump our opinion */
void rta_report(void)
{
	int i, n;

	n = get_irp_n_types();
	for (i = 0; i < n; ++i) {
		ir_type *tp = get_irp_type(i);
		if (is_Class_type(tp) && rta_is_alive_class(tp)) {
			ir_printf("RTA: considered allocated: %+F\n", tp);
		}
	}

	n = get_irp_n_irgs();
	for (i = 0; i < n; i++) {
		ir_graph *irg = get_irp_irg(i);
		if (rta_is_alive_graph(irg)) {
			ir_printf("RTA: considered called: graph of %+F\n", irg);
		}
	}
}
