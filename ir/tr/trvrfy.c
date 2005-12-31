/*
 * Project:     libFIRM
 * File name:   ir/tr/trvrfy.c
 * Purpose:     Check types and entities for correctness.
 * Author:      Michael Beck, Goetz Lindenmaier
 * Modified by:
 * Created:     29.1.2003
 * CVS-ID:      $Id$
 * Copyright:   (c) 2003 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "trvrfy.h"
#include "irgraph_t.h"  /* for checking whether constant code is allocated
               on proper obstack */
#include "irflag_t.h"
#include "irprintf.h"
#include "irgwalk.h"
#include "typewalk.h"

static const char *firm_vrfy_failure_msg;

#ifdef NDEBUG
/*
 * in RELEASE mode, returns ret if the expression expr evaluates to zero
 * in ASSERT mode, asserts the expression expr (and the string string).
 */
#define ASSERT_AND_RET(expr, string, ret)       if (!(expr)) return (ret)

/*
 * in RELEASE mode, returns ret if the expression expr evaluates to zero
 * in ASSERT mode, executes blk if the expression expr evaluates to zero and asserts expr
 */
#define ASSERT_AND_RET_DBG(expr, string, ret, blk)      if (!(expr)) return (ret)
#else
#define ASSERT_AND_RET(expr, string, ret) \
do { \
  if (opt_do_node_verification == FIRM_VERIFICATION_ON) {\
    assert((expr) && string); } \
  if (!(expr)) { \
    if (opt_do_node_verification == FIRM_VERIFICATION_REPORT) \
      fprintf(stderr, #expr " : " string "\n"); \
    firm_vrfy_failure_msg = #expr " && " string; \
    return (ret); \
  } \
} while(0)

#define ASSERT_AND_RET_DBG(expr, string, ret, blk) \
do { \
  if (!(expr)) { \
    firm_vrfy_failure_msg = #expr " && " string; \
    if (opt_do_node_verification != FIRM_VERIFICATION_ERROR_ONLY) { blk; } \
    if (opt_do_node_verification == FIRM_VERIFICATION_REPORT) \
      fprintf(stderr, #expr " : " string "\n"); \
    else if (opt_do_node_verification == FIRM_VERIFICATION_ON) { \
      assert((expr) && string); \
    } \
    return (ret); \
  } \
} while(0)

#endif /* NDEBUG */

/**
 * Check a class
 */
static int check_class(ir_type *tp) {
  int i, j, k;
  int found;

  /*printf("\n"); DDMT(tp);*/

  for (i = 0; i < get_class_n_members(tp); i++) {

    entity *mem = get_class_member(tp, i);
    assert(mem && "NULL members not allowed");
    /*printf(" %d, %d", get_entity_n_overwrites(mem), get_class_n_supertypes(tp)); DDME(mem);*/
    if (!mem) return error_null_mem;

    if (get_entity_n_overwrites(mem) > get_class_n_supertypes(tp)) {
      ASSERT_AND_RET_DBG(
        get_entity_n_overwrites(mem) <= get_class_n_supertypes(tp),
        "wrong number of entity overwrites",
        error_wrong_ent_overwrites,
        ir_fprintf(stderr, "%+F %+F\n", tp, mem)
      );
    }
    for (j = 0; j < get_entity_n_overwrites(mem); j++) {
      entity *ovw = get_entity_overwrites(mem, j);
      /*printf(" overwrites: "); DDME(ovw);*/
      /* Check whether ovw is member of one of tp's supertypes. If so,
         the representation is correct. */
      found = 0;
      for (k = 0; k < get_class_n_supertypes(tp); k++) {
        if (get_class_member_index(get_class_supertype(tp, k), ovw) >= 0) {
          found = 1;
          break;
        }
      }
      if (!found) {
        ASSERT_AND_RET_DBG(
          found,
          "overwrites an entity not contained in direct supertype",
          error_ent_not_cont,
          ir_fprintf(stderr, "%+F %+F\n", tp, mem)
        );
      }
    }
  }
  return 0;
}

/**
 * Check an array.
 */
static int check_array(ir_type *tp) {
  int i, n_dim = get_array_n_dimensions(tp);
  for (i = 0; i < n_dim; ++i) {
    ASSERT_AND_RET_DBG(
      has_array_lower_bound(tp, i) || has_array_upper_bound(tp, i),
      "array bound missing",
      1,
      ir_fprintf(stderr, "%+F in dimension %d\n", tp, i)
    );
  }
  return 0;
}


/**
 * Check a primitive.
 */
static int check_primitive(ir_type *tp) {
  ASSERT_AND_RET_DBG(
    is_mode(get_type_mode(tp)),
    "Primitive type without mode",
    1,
    ir_fprintf(stderr, "%+F\n", tp)
  );
  return 0;
}


/*
 * Checks a type.
 *
 * return
 *  0   if no error encountered
 */
int check_type(ir_type *tp) {
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
 * checks the visited flag
 */
static int check_visited_flag(ir_graph *irg, ir_node *n) {
  ASSERT_AND_RET_DBG(
    get_irn_visited(n) <= get_irg_visited(irg),
    "Visited flag of node is larger than that of corresponding irg.",
    0,
    ir_fprintf(stderr, "%+F in %+F\n", n, irg)
  );
  return 1;
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
  struct myenv *myenv = env;

  /* We also test whether the setting of the visited flag is legal. */
  myenv->res = node_is_in_irgs_storage(myenv->irg, n) &&
               check_visited_flag(myenv->irg, n);
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
    else if (get_entity_visibility(ent) != visibility_external_allocated) {
      ASSERT_AND_RET_DBG(
        is_Class_type(get_entity_owner(ent)) &&
        get_class_peculiarity(get_entity_owner(ent)) == peculiarity_description,
        "Value in constant atomic entity not set.",
        0,
        ir_fprintf(stderr, "%+F, owner %+F\n", ent, get_entity_owner(ent))
      );
    }
  }
  return 0;
}

/*
 * Check an entity. Currently, we check only if initialized constants
 * are build on the const irg graph.
 *
 * @return
 *  0   if no error encountered
 *  != 0    a trvrfy_error_codes code
 */
int check_entity(entity *ent) {
  int rem_vpi;
  ir_type *tp = get_entity_type(ent);
  ir_type *owner = get_entity_owner(ent);

  current_ir_graph =  get_const_code_irg();
  ASSERT_AND_RET(constants_on_wrong_irg(ent) == 0, "Contants placed on wrong IRG", error_const_on_wrong_irg);

  rem_vpi = get_visit_pseudo_irgs();
  set_visit_pseudo_irgs(1);
  if ((get_entity_peculiarity(ent) == peculiarity_existent) &&
      (get_entity_visibility(ent) != visibility_external_allocated) &&
      (is_Method_type(get_entity_type(ent)))                &&
      (!get_entity_irg(ent) || !(is_ir_graph(get_entity_irg(ent))))) {
    ASSERT_AND_RET_DBG(
      0,
      "Method ents with pec_exist must have an irg",
      error_existent_entity_without_irg,
      ir_fprintf(stderr, "%+F\n", ent)
    );
  }
  set_visit_pseudo_irgs(rem_vpi);

  /* Originally, this test assumed, that only method entities have
     pecularity_inherited.  As I changed this, I have to test for method type before
     doing the test. */
  if (get_entity_peculiarity(ent) == peculiarity_inherited) {
    if (is_Method_type(get_entity_type(ent))) {
      entity *impl = get_SymConst_entity(get_atomic_ent_value(ent));
      ASSERT_AND_RET_DBG(
        get_entity_peculiarity(impl) == peculiarity_existent,
	     "inherited method entities must have constant pointing to existent entity.",
       error_inherited_ent_without_const,
       ir_fprintf(stderr, "%+F points to %+F\n", ent, impl)
      );
    }
  }

  /* Entities in global type are not dynamic or automatic allocated. */
  if (owner == get_glob_type()) {
    ASSERT_AND_RET_DBG(
      get_entity_allocation(ent) != allocation_dynamic &&
	    get_entity_allocation(ent) != allocation_automatic,
      "Entities in global type are not allowed to by dynamic or automatic allocated",
      error_glob_ent_allocation,
      ir_fprintf(stderr, "%+F\n", ent)
    );
  }

  if (get_entity_variability(ent) != variability_uninitialized) {
    if (is_atomic_type(tp)) {
      ir_node *val = get_atomic_ent_value(ent);
      if (val)
        ASSERT_AND_RET_DBG(
          get_irn_mode(val) == get_type_mode(tp),
	        "Mode of constant in entity must match type.",
          error_ent_const_mode,
          ir_fprintf(stderr, "%+F const %+F, type %+F(%+F)\n",
            ent, val, tp, get_type_mode(tp))
        );
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
    *res = check_type((ir_type *)tore);
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
