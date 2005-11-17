/*
 * Project:     libFIRM
 * File name:   ir/tr/trvrfy.c
 * Purpose:     Check types and entities for correctness.
 * Author:      Michael Beck, Goetz Lindenmaier
 * Modified by:
 * Created:     29.1.2003
 * CVS-ID:      $Id$
 * Copyright:   (c) 2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "trvrfy.h"
#include "irgraph_t.h"  /* for checking whether constant code is allocated
               on proper obstack */

/**
 * Check a class
 */
static int check_class(type *tp) {
  int i, j, k;
  int found;

  /*printf("\n"); DDMT(tp);*/

  for (i = 0; i < get_class_n_members(tp); i++) {

    entity *mem = get_class_member(tp, i);
    assert(mem && "NULL members not allowed");
    /*printf(" %d, %d", get_entity_n_overwrites(mem), get_class_n_supertypes(tp)); DDME(mem);*/
    if (!mem) return error_null_mem;

    if (get_entity_n_overwrites(mem) > get_class_n_supertypes(tp)) {
      DDMT(tp); DDME(mem);
      assert(get_entity_n_overwrites(mem) <= get_class_n_supertypes(tp));
    }
    for (j = 0; j < get_entity_n_overwrites(mem); j++) {
      entity *ovw = get_entity_overwrites(mem, j);
      /*printf(" overwrites: "); DDME(ovw);*/
      /* Check whether ovw is member of one of tp's supertypes. If so,
         the representation is correct. */
      found = 0;
      for (k = 0; k < get_class_n_supertypes(tp); k++) {
        if (get_class_member_index(get_class_supertype(tp, k), ovw) >= 0) {
          found = 0;
          break;
        }
      }
      if (!found) {
        DDMT(tp); DDME(mem);
        assert(found && "overwrites an entity not contained in direct supertype");
        return error_ent_not_cont;
      }
    }

  }
  return 0;
}

/**
 * Check an array.
 */
static int check_array(type *tp) {
  int i, n_dim = get_array_n_dimensions(tp);
  for (i = 0; i < n_dim; ++i)
    assert(has_array_lower_bound(tp, i) || has_array_upper_bound(tp, i));

  return 0;
}


/**
 * Check a primitive.
 */
static int check_primitive(type *tp) {
  assert(is_mode(get_type_mode(tp)));

  return 0;
}


/**
 * Checks a type.
 *
 * Currently checks class types only.
 */
static int check_type(type *tp) {
  switch (get_type_tpop_code(tp)) {
  case tpo_class:
    return check_class(tp);
  case tpo_array:
    return check_array(tp);
  case tpo_primitive:
    return check_primitive(tp);
  default: break;
  }
  return 0;
}

/**
 * helper environment struct for constant_on_wrong_obstack()
 */
struct myenv {
  int res;
  ir_graph *irg;
};

/**
 * called by the walker
 */
static void on_irg_storage(ir_node *n, void *env) {
  struct myenv * myenv = env;

  myenv->res = node_is_in_irgs_storage(myenv->irg, n);

  /* We also test whether the setting of the visited flag is legal. */
  assert(get_irn_visited(n) <= get_irg_visited(myenv->irg) &&
     "Visited flag of node is larger than that of corresponding irg.");
}

/**
 * checks whether a given constant IR node is NOT on the
 * constant IR graph.
 */
static int constant_on_wrong_irg(ir_node *n) {
  struct myenv env;

  env.res = 1;  /* on right obstack */
  env.irg = get_const_code_irg();

  irg_walk(n, on_irg_storage, NULL, (void *)&env);
  return ! env.res;
}

/*
 * Check if constants node are NOT on the constant IR graph.
 */
static int constants_on_wrong_irg(entity *ent) {
  if (get_entity_variability(ent) == variability_uninitialized) return 0;

  if (is_compound_entity(ent)) {
    int i;
    for (i = 0; i < get_compound_ent_n_values(ent); i++) {
      if (constant_on_wrong_irg(get_compound_ent_value(ent, i)))
        return 1;
    }
  } else {
    /* Might not be set if entity belongs to a description or is external allocated. */
    if (get_atomic_ent_value(ent))
      return constant_on_wrong_irg(get_atomic_ent_value(ent));
    else if (get_entity_visibility(ent) != visibility_external_allocated)
      assert((is_Class_type(get_entity_owner(ent)) &&
          get_class_peculiarity(get_entity_owner(ent)) == peculiarity_description) &&
         "Value in constant atomic entity not set.");
  }
  return 0;
}

/*
 * Check an entity. Currently, we check only if initialized constants
 * are build on the const irg graph.
 *
 * @return
 *  0   if no error encountered
 *  != 0    else
 */
static int check_entity(entity *ent) {
  int rem_vpi;
  type *tp = get_entity_type(ent);
  type *owner = get_entity_owner(ent);

  current_ir_graph =  get_const_code_irg();
  if (constants_on_wrong_irg(ent)) {
    assert(0 && "Contants placed on wrong IRG");
    return error_const_on_wrong_irg;
  }

  rem_vpi = get_visit_pseudo_irgs();
  set_visit_pseudo_irgs(1);
  if ((get_entity_peculiarity(ent) == peculiarity_existent) &&
      (get_entity_visibility(ent) != visibility_external_allocated) &&
      (is_Method_type(get_entity_type(ent)))                &&
      (!get_entity_irg(ent) || !(is_ir_graph(get_entity_irg(ent))))) {
    assert(0 && "Method ents with pec_exist must have an irg");
    return error_existent_entity_without_irg;
  }
  set_visit_pseudo_irgs(rem_vpi);

  /* Originally, this test assumed, that only method entities have
     pec_inh.  As I changed this, I have to test for method type before
     doing the test. */
  if (get_entity_peculiarity(ent) == peculiarity_inherited) {
    if (is_Method_type(get_entity_type(ent))) {
      entity *impl = get_SymConst_entity(get_atomic_ent_value(ent));
      assert(get_entity_peculiarity(impl) == peculiarity_existent &&
	     "inherited method entities must have constant pointing to existent entity.");
    }
  }

  /* Entities in global type are not dynamic or automatic allocated. */
  if (owner == get_glob_type()) {
    assert(get_entity_allocation(ent) != allocation_dynamic &&
	   get_entity_allocation(ent) != allocation_automatic);
  }

  if (get_entity_variability(ent) != variability_uninitialized) {
    if (is_atomic_type(tp)) {
      ir_node *val = get_atomic_ent_value(ent);
      if (val)
        assert(get_irn_mode(val) == get_type_mode(tp) &&
	       "Mode of constant in entity must match type.");
    }
  }

  return no_error;
}

/*
 * check types and entities
 */
static void check_tore(type_or_ent *tore, void *env) {
  int *res = env;
  assert(tore);
  if (is_type(tore)) {
    *res = check_type((type *)tore);
  } else {
    assert(is_entity(tore));
    *res = check_entity((entity *)tore);
  }
}

/*
 * Verify types and entities.
 */
int tr_vrfy(void) {
  int res;

  type_walk(check_tore, NULL, &res);
  return res;
}
