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
 * @author   Florian
 * @version  09.06.2002
 * @version  $Id$
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "rta.h"

#include <stdlib.h>

#include "irnode_t.h"
#include "irprog_t.h"
#include "irgraph_t.h"

#include "eset.h"
#include "irgwalk.h"
#include "irgmod.h"
#include "irvrfy.h"
#include "irprintf.h"

# ifndef TRUE
#  define TRUE 1
#  define FALSE 0
# endif /* not defined TRUE */

/* flags */
static int verbose     = 0;


/* base data */
static eset *_live_classes   = NULL;

/* cache computed results */
static eset *_live_graphs    = NULL;

/**
   Given a method, find the firm graph that implements that method.
*/
static ir_graph *get_implementing_graph (ir_entity *method)
{
#if 0
  ir_graph *graph = get_entity_irg ((ir_entity*) method);

  /* Search upwards in the overwrites graph. */
  /* GL: this will not work for multiple inheritance */
  if (NULL == graph) {
    int i;
    int n_over = get_entity_n_overwrites ((ir_entity*) method);

    for (i = 0; (NULL == graph) && (i < n_over); i ++) {
      ir_entity *over = get_entity_overwrites ((ir_entity*) method, i);
      graph = get_implementing_graph (over);
    }
  }

  /* GL   this is not valid in our remove irg algorithm ... which we removed by now ...  */
  assert(get_entity_peculiarity(method) == peculiarity_description
     || graph == get_entity_irg(get_SymConst_entity(get_atomic_ent_value(method))));

  /* we *must* always return a graph != NULL, *except* when we're used
     inside remove_irg or force_description */
  /* assert (graph && "no graph"); */

  return (graph);
#else
  ir_graph *graph = NULL;

  if (get_entity_peculiarity(method) != peculiarity_description)
    graph = get_entity_irg(get_SymConst_entity(get_atomic_ent_value(method)));

  return graph;
#endif
}

/**
 * Add a graph to the set of live graphs.
 *
 * @param graph  the graph to add
 * @return non-zero if the graph was added, zero
 *         if it was already in the live set
 */
static int add_graph (ir_graph *graph)
{
  if (!eset_contains (_live_graphs, graph)) {
    if (verbose > 1) {
      ir_fprintf(stdout, "RTA:        new graph of %+F\n", graph);
    }

    eset_insert (_live_graphs, graph);
    return (TRUE);
  }

  return (FALSE);
}

/**
 * Add a class to the set of live classes.
 *
 * @param clazz   the class to add
 * @return non-zero if the graph was added, zero
 *         if it was already in the live set
 */
static int add_class (ir_type *clazz)
{
  if (!eset_contains (_live_classes, clazz)) {
    if (verbose > 1) {
      ir_fprintf(stdout, "RTA:        new class: %+F\n", clazz);
    }

    eset_insert (_live_classes, clazz);
    return (TRUE);
  }

  return (FALSE);
}

/** Given an entity, add all implementing graphs that belong to live classes
 *  to _live_graphs.
 *
 *  Iff additions occurred, return TRUE, else FALSE.
*/
static int add_implementing_graphs (ir_entity *method)
{
  int i;
  int n_over = get_entity_n_overwrittenby (method);
  ir_graph *graph = get_entity_irg (method);
  int change = FALSE;

  if (NULL == graph) {
    graph = get_implementing_graph (method);
  }

  if (verbose > 1) {
    ir_fprintf(stdout, "RTA:        new call to %+F\n", method);
  }

  if (rta_is_alive_class (get_entity_owner (method))) {
    if (NULL != graph) {
      change = add_graph (graph);
    }
  }

  for (i = 0; i < n_over; i ++) {
    ir_entity *over = get_entity_overwrittenby (method, i);
    change |= add_implementing_graphs (over);
  }

  return (change);
}

/** Enter all method accesses and all class allocations into
 *  our tables.
 *
 *  Set *(int*)env to true iff (possibly) new graphs have been found.
 */
static void rta_act (ir_node *node, void *env)
{
  int *change = (int*) env;
  ir_opcode op = get_irn_opcode (node);

  if (iro_Call == op) {         /* CALL */
    ir_entity *ent = NULL;

    ir_node *ptr = get_Call_ptr (node);

    /* CALL SEL */
    if (iro_Sel == get_irn_opcode (ptr)) {
      ent = get_Sel_entity (ptr);
      *change |= add_implementing_graphs (ent);

      /* CALL SYMCONST */
    } else if (iro_SymConst == get_irn_opcode (ptr)) {
      if (get_SymConst_kind(ptr) == symconst_addr_ent) {
        ir_graph *graph;

        ent = get_SymConst_entity (ptr);
        graph = get_entity_irg (ent);
        if (graph) {
          *change |= add_graph (graph);
        } else {
          /* it's an external allocated thing. */
        }
      } else if (get_SymConst_kind(ptr) == symconst_addr_name) {
	    /* Entities of kind addr_name may not be allocated in this compilation unit.
	       If so, the frontend built faulty Firm.  So just ignore. */
	    /* if (get_SymConst_name(ptr) != new_id_from_str("iro_Catch"))
        assert (ent && "couldn't determine entity of call to SymConst of kind addr_name."); */
      } else {
        /* other symconst. */
        assert(0 && "This SymConst can not be an address for a method call.");
      }

      /* STRANGE */
    } else {
      assert(0 && "Unexpected address expression: can not analyse, therefore can not do correct rta!");
    }

  } else if (iro_Alloc == op) { /* ALLOC */
    ir_type *type = get_Alloc_type (node);

    *change |= add_class (type);
  }
}

/**
   Traverse the given graph to collect method accesses and
   object allocations.
*/
static int rta_fill_graph (ir_graph* graph)
{
  int change = FALSE;
  irg_walk_graph (graph, rta_act, NULL, &change);
  return change;
}

/** Traverse all graphs to collect method accesses and object allocations.
 */
static int rta_fill_incremental (void)
{
  int i;
  int n_runs = 0;
  int rerun  = TRUE;
#ifdef INTERPROCEDURAL_VIEW
  int old_ip_view = get_interprocedural_view();

  set_interprocedural_view(0);     /* save this for later */
#endif

  /* init_tables has added main_irg to _live_graphs */

  /* Need to take care of graphs that are externally
     visible or sticky. Pretend that they are called: */

  for (i = 0; i < get_irp_n_irgs(); i++) {
    ir_graph *graph = get_irp_irg (i);
    ir_entity *ent = get_irg_entity (graph);

    if ((visibility_external_visible == get_entity_visibility (ent)) ||
        (stickyness_sticky == get_entity_stickyness (ent))) {
      eset_insert (_live_graphs, graph);
      // printf("external visible: "); DDMG(graph);
    }
  }

  while (rerun) {
    ir_graph *graph;

    /* start off new */
    eset *live_graphs = _live_graphs;
    _live_graphs = eset_create ();

    if (verbose > 1) {
      fprintf(stdout, "RTA: RUN %i\n", n_runs);
    }

    /* collect what we have found previously */
    eset_insert_all (_live_graphs, live_graphs);

    rerun = FALSE;
    for (graph = eset_first (live_graphs);
         graph;
         graph = eset_next (live_graphs)) {

      if (verbose > 1) {
        ir_fprintf(stdout, "RTA: RUN %i: considering graph of %+F\n", n_runs,
		        graph);
      }

      rerun |= rta_fill_graph (graph);
    }

    eset_destroy (live_graphs);

    n_runs ++;
  }

#ifdef INTERPROCEDURAL_VIEW
  set_interprocedural_view(old_ip_view); /* cover up our traces */
#endif

  return (n_runs);
}

/**
 * Count the number of graphs that we have found to be live.
 */
static int stats (void)
{
  int i;
  int n_live_graphs = 0;
  int n_graphs = get_irp_n_irgs();

  for (i = 0; i < n_graphs; i++) {
    ir_graph *graph = get_irp_irg(i);

    if (rta_is_alive_graph (graph)) {
      n_live_graphs ++;
    }
  }

  return (n_live_graphs);
}

/* remove a graph, part I */
/*
   We removed the first graph to implement the entity, so we're
   abstract now.  Pretend that it wasn't there at all, and every
   entity that used to inherit this entity's graph is now abstract.
*/
/* Since we *know* that this entity will not be called, this is OK. */
static void force_description (ir_entity *ent, ir_entity *addr)
{
  int i, n_over = get_entity_n_overwrittenby (ent);

  set_entity_peculiarity (ent, peculiarity_description);

  for (i = 0; i < n_over; i ++) {
    ir_entity *over = get_entity_overwrittenby (ent, i);

    if (peculiarity_inherited == get_entity_peculiarity (over)) {
      /* We rely on the fact that cse is performed on the const_code_irg. */
      ir_entity *my_addr = get_SymConst_entity(get_atomic_ent_value(over));

      if (addr == my_addr) {
        force_description (over, addr);
      }
    } else if (peculiarity_existent == get_entity_peculiarity (over)) {
      /* check whether 'over' forces 'inheritance' of *our* graph: */
      ir_node *f_addr = get_atomic_ent_value (over);
      ir_entity *impl_ent = get_SymConst_entity (f_addr);

      assert(is_SymConst(f_addr) && "can't do complex addrs");
      if (impl_ent == addr) {
        assert (0 && "gibt's denn sowas");
        force_description (over, addr);
      }
    }
  }
}

/**
   Initialize the static data structures.
*/
static void init_tables (void)
{
  ir_type *tp;
  int i, n;

  _live_classes = eset_create ();
  _live_graphs  = eset_create ();

  if (get_irp_main_irg ()) {
    eset_insert (_live_graphs, get_irp_main_irg ());
  }

  /* Find static allocated classes */
  tp = get_glob_type();
  n = get_class_n_members(tp);
  for (i = 0; i < n; ++i) {
    ir_type *member_type = get_entity_type(get_class_member(tp, i));
    if (is_Class_type(member_type))
      eset_insert(_live_classes, member_type);
  }

  tp = get_tls_type();
  n = get_struct_n_members(tp);
  for (i = 0; i < n; ++i) {
    ir_type *member_type = get_entity_type(get_struct_member(tp, i));
    if (is_Class_type(member_type))
      eset_insert(_live_classes, member_type);
  }
}

/*
 * Initialize the RTA data structures, and perform RTA.
 * do_verbose If == 1, print statistics, if > 1, chatter about every detail
 */
void rta_init (int do_verbose)
{
  int n_runs = 0;

  int rem_vpi = get_visit_pseudo_irgs();
  set_visit_pseudo_irgs(1);

# ifdef DEBUG_libfirm
  {
    int i, n;
    n = get_irp_n_irgs();
    for (i = 0; i < n; i++) {
      irg_vrfy (get_irp_irg(i));
	}
    tr_vrfy ();
  }
# endif /* defined DEBUG_libfirm */

  verbose = do_verbose;

  init_tables ();

  n_runs = rta_fill_incremental ();

  if (verbose) {
    int n_live_graphs = stats ();

    printf ("RTA: n_graphs      = %i\n", get_irp_n_irgs ());
    printf ("RTA: n_live_graphs = %i\n", n_live_graphs);
    printf ("RTA: n_runs        = %i\n", n_runs);
  }

# ifdef DEBUG_libfirm
  {
    int i, n;
    n = get_irp_n_irgs();
    for (i = 0; i < n; i++) {
      irg_vrfy (get_irp_irg(i));
	}
    tr_vrfy ();
  }
# endif /* defined DEBUG_libfirm */

  set_visit_pseudo_irgs(rem_vpi);
}

/**
 * walker for all types and entities
 *
 * Changes the peculiarity of entities that represents
 * dead graphs to peculiarity_description.
 */
static void make_entity_to_description(type_or_ent tore, void *env) {
  (void) env;
  if (is_entity(tore.ent)) {
    ir_entity *ent = tore.ent;

    if ((is_Method_type(get_entity_type(ent)))                        &&
        (get_entity_peculiarity(ent) != peculiarity_description)      &&
        (get_entity_visibility(ent)  != visibility_external_allocated)   ) {
      ir_graph *irg = get_entity_irg(get_SymConst_entity(get_atomic_ent_value(ent)));
      if (!eset_contains (_live_graphs, irg)) {
        set_entity_peculiarity(ent, peculiarity_description);
        set_entity_irg(ent, NULL);
      }
    }
  }
}

/* Delete all graphs that we have found to be dead from the program
   If verbose == 1, print statistics, if > 1, chatter about every detail
*/
void rta_delete_dead_graphs (void)
{
  int i;
  int n_graphs = get_irp_n_irgs ();
  ir_graph *graph = NULL;
  int n_dead_graphs = 0;
  ir_graph **dead_graphs;

  int rem_vpi = get_visit_pseudo_irgs();
  set_visit_pseudo_irgs(1);

  dead_graphs = XMALLOCN(ir_graph*, get_irp_n_irgs());

  for (i = 0; i < n_graphs; i++) {
    graph = get_irp_irg(i);

    if (rta_is_alive_graph (graph)) {
      /* do nothing (except some debugging fprintf`s :-) */
    } else {
#ifndef NDEBUG
      ir_entity *ent = get_irg_entity (graph);
      assert (visibility_external_visible != get_entity_visibility (ent));
#endif

      dead_graphs[n_dead_graphs] = graph;
      n_dead_graphs ++;
    }
  }

  type_walk(make_entity_to_description, NULL, NULL);
  for (i = 0; i < n_dead_graphs; ++i) {
    remove_irp_irg (dead_graphs[i]);
  }

  if (verbose) {
    printf ("RTA: n_dead_graphs = %i\n", n_dead_graphs);
  }

  set_visit_pseudo_irgs(rem_vpi);

  free(dead_graphs);
}

/* Clean up the RTA data structures.  Call this after calling rta_init */
void rta_cleanup (void)
{
# ifdef DEBUG_libfirm
  int i;
    for (i = 0; i < get_irp_n_irgs(); i++) {
      irg_vrfy (get_irp_irg(i));
    }
    tr_vrfy ();
# endif /* defined DEBUG_libfirm */

  if (_live_classes) {
    eset_destroy (_live_classes);
    _live_classes = NULL;
  }

  if (_live_graphs) {
    eset_destroy (_live_graphs);
    _live_graphs = NULL;
  }
}

/* Say whether this class might be instantiated at any point in the program: */
int  rta_is_alive_class  (ir_type   *clazz)
{
  return (eset_contains (_live_classes, clazz));
}

/* Say whether this graph might be run at any time in the program: */
int  rta_is_alive_graph (ir_graph *graph)
{
  return eset_contains (_live_graphs, graph);
}

/* dump our opinion */
void rta_report (void)
{
  int i;

  for (i = 0; i < get_irp_n_types(); ++i) {
    ir_type *tp = get_irp_type(i);
    if (is_Class_type(tp) && rta_is_alive_class(tp)) {
      ir_fprintf(stdout, "RTA: considered allocated: %+F\n", tp);
    }
  }

  for (i = 0; i < get_irp_n_irgs(); i++) {
    if (rta_is_alive_graph (get_irp_irg(i))) {
      ir_fprintf(stdout, "RTA: considered called: graph of %+F\n", get_irp_irg(i));
    }
  }
}
