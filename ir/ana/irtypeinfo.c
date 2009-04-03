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
 * @brief     Data structure to hold type information for nodes.
 * @author    Goetz Lindenmaier
 * @date      28.8.2003
 * @version   $Id$
 * @summary
 *  Data structure to hold type information for nodes.
 *
 *  This module defines a field "type" of type "type *" for each ir node.
 *  It defines a flag for irgraphs to mark whether the type info of the
 *  graph is valid.  Further it defines an auxiliary type "initial_type".
 *
 *  The module defines a map that contains pairs (irnode, type).  If an irnode
 *  is not in the map it is assumed to be initialized, i.e., the initialization
 *  requires no compute time.  As firm nodes can not be freed and reallocated
 *  pointers for nodes are unique (until a call of dead_node_elimination).
 */
#include "config.h"

#include "irtypeinfo.h"

#include <stddef.h>

#include "irgraph_t.h"   /* for setting the state flag. */
#include "irprog_t.h"
#include "irnode_t.h"
#include "pmap.h"

/* ------------ The map. ---------------------------------------------- */


static pmap *type_node_map = NULL;


/* ------------ Auxiliary type. --------------------------------------- */

/*  This auxiliary type expresses that a field is uninitialized.  The
 *  variable is set by init_irtypeinfo.  The type is freed by
 *  free_irtypeinfo.
 */
ir_type *initial_type = NULL;

/* ------------ Initializing this module. ----------------------------- */

/*  Initializes the type information module.
 *  Generates a type "initial_type" and sets the type of all nodes to this type.
 *  Calling set/get_irn_type is invalid before calling init. Requires memory
 *  in the order of MIN(<calls to set_irn_type>, #irnodes).
 */
void init_irtypeinfo(void) {
  int i, n;

  if (!initial_type)
    initial_type = new_type_class(new_id_from_str("initial_type"));

  /* We need a new, empty map. */
  if (type_node_map) pmap_destroy(type_node_map);
  type_node_map = pmap_create();

  n = get_irp_n_irgs();
  for (i = 0; i < n; ++i)
    set_irg_typeinfo_state(get_irp_irg(i), ir_typeinfo_none);
}

void free_irtypeinfo(void) {
  int i, n;

  if (initial_type) {
    free_type(initial_type);
    initial_type = NULL;
  }
  //else assert(0 && "call init_type_info before freeing");

  if (type_node_map) {
    pmap_destroy(type_node_map);
    type_node_map = NULL;
  }
  //else assert(0 && "call init_type_info before freeing");

  n = get_irp_n_irgs();
  for (i = 0; i < n; ++i)
    set_irg_typeinfo_state(get_irp_irg(i), ir_typeinfo_none);
}


/* ------------ Irgraph state handling. ------------------------------- */

void set_irg_typeinfo_state(ir_graph *irg, ir_typeinfo_state s) {
  assert(is_ir_graph(irg));
  irg->typeinfo_state = s;
  if ((irg->typeinfo_state == ir_typeinfo_consistent) &&
      (irp->typeinfo_state == ir_typeinfo_consistent) &&
      (s                   != ir_typeinfo_consistent)   )
    irp->typeinfo_state = ir_typeinfo_inconsistent;
}

ir_typeinfo_state get_irg_typeinfo_state(const ir_graph *irg) {
  assert(is_ir_graph(irg));
  return irg->typeinfo_state;
}


/* Returns accumulated type information state information.
 *
 * Returns ir_typeinfo_consistent if the type information of all irgs is
 * consistent.  Returns ir_typeinfo_inconsistent if at least one irg has inconsistent
 * or no type information.  Returns ir_typeinfo_none if no irg contains type information.
 */
ir_typeinfo_state get_irp_typeinfo_state(void) {
  return irp->typeinfo_state;
}
void set_irp_typeinfo_state(ir_typeinfo_state s) {
  irp->typeinfo_state = s;
}
/* If typeinfo is consistent, sets it to inconsistent. */
void set_irp_typeinfo_inconsistent(void) {
  if (irp->typeinfo_state == ir_typeinfo_consistent)
    irp->typeinfo_state = ir_typeinfo_inconsistent;
}


/* ------------ Irnode type information. ------------------------------ */

/* These routines only work properly if the ir_graph is in state
 * ir_typeinfo_consistent or ir_typeinfo_inconsistent.  They
 * assume current_ir_graph set properly.
 */
ir_type *get_irn_typeinfo_type(const ir_node *n) {
  ir_type *res = initial_type;
  pmap_entry *entry;
  assert(get_irg_typeinfo_state(get_irn_irg(n)) == ir_typeinfo_consistent  ||
	 get_irg_typeinfo_state(get_irn_irg(n)) == ir_typeinfo_inconsistent  );

  entry = pmap_find(type_node_map, n);
  if (entry)
    res = entry->value;

  return res;
}

void set_irn_typeinfo_type(ir_node *n, ir_type *tp) {
  assert(get_irg_typeinfo_state(current_ir_graph) == ir_typeinfo_consistent  ||
  	 get_irg_typeinfo_state(current_ir_graph) == ir_typeinfo_inconsistent  );

  pmap_insert(type_node_map, (void *)n, (void *)tp);
}
