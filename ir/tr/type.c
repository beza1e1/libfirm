/****h* libfirm/type.c
 *
 * NAME
 *   file type.c - implementation of the datastructure to hold
 *   type information.
 * COPYRIGHT
 *  (C) 2001 by Universitaet Karlsruhe
 * AUTHORS
 *  Martin Trapp, Christian Schaefer, Goetz Lindenmaier
 *
 * NOTES
 *  This module supplies a datastructure to represent all types
 *  known in the compiled program.  This includes types specified
 *  in the program as well as types defined by the language.  In the
 *  view of the intermediate representation there is no difference
 *  between these types.
 *
 *  There exist several kinds of types, arranged by the structure of
 *  the type.  A type is described by a set of attributes.  Some of
 *  these attributes are common to all types, others depend on the
 *  kind of the type.
 *
 *  Types are different from the modes defined in irmode:  Types are
 *  on the level of the programming language, modes at the level of
 *  the target processor.
 *
 * SEE ALSO
 *   type_t.h type tpop
 *****
 */

/* $Id$ */

# include <stdlib.h>
# include <stddef.h>
# include "type_t.h"
# include "tpop_t.h"
# include "typegmod_t.h"
# include "array.h"
# include "irprog.h"
# include "mangle.h"
# include "tv.h"
# include "ircons.h"

/*******************************************************************/
/** TYPE                                                          **/
/*******************************************************************/

unsigned long type_visited;

INLINE type *
new_type(tp_op *type_op, ir_mode *mode, ident* name) {
  type *res;
  int node_size ;

  assert(type_op != type_id);

  node_size = offsetof (type, attr) +  type_op->attr_size;
  res = (type *) xmalloc (node_size);
  add_irp_type(res);   /* Remember the new type global. */

  res->kind = k_type;
  res->type_op = type_op;
  res->mode = mode;
  res->name = name;
  res->state = layout_undefined;
  res->size = -1;
  res->visit = 0;
  res -> link = NULL;

  return res;
}

void free_type_attrs(type *tp) {
  switch(get_type_tpop_code(tp)) {
  case tpo_class:       { free_class_attrs(tp);       } break;
  case tpo_struct:      { free_struct_attrs(tp);      } break;
  case tpo_method:      { free_method_attrs(tp);      } break;
  case tpo_union:       { free_union_attrs(tp);       } break;
  case tpo_array:       { free_array_attrs(tp);       } break;
  case tpo_enumeration: { free_enumeration_attrs(tp); } break;
  case tpo_pointer:     { free_pointer_attrs(tp);     } break;
  case tpo_primitive:   { free_primitive_attrs(tp);   } break;
  default: break;
  }
}

/* set/get the link field */
void *get_type_link(type *tp)
{
  assert(tp && tp->kind == k_type);
  return(tp -> link);
}

void set_type_link(type *tp, void *l)
{
  assert(tp && tp->kind == k_type);
  tp -> link = l;
}

tp_op*      get_type_tpop(type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->type_op;
}

ident*      get_type_tpop_nameid(type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->type_op->name;
}

const char* get_type_tpop_name(type *tp) {
  assert(tp && tp->kind == k_type);
  return id_to_str(tp->type_op->name);
}

tp_opcode    get_type_tpop_code(type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->type_op->code;
}

ir_mode*    get_type_mode(type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->mode;
}

void        set_type_mode(type *tp, ir_mode* m) {
  assert(tp && tp->kind == k_type);

  assert(((tp->type_op != type_primitive) || mode_is_data(m)) &&
	 /* Modes of primitives must be data */
	 ((tp->type_op != type_enumeration) || mode_is_int(m)));
         /* Modes of enumerations must be integers */

  if ((tp->type_op == type_primitive) || (tp->type_op == type_enumeration)) {
    /* For pointer, primitive and enumeration size depends on the mode. */
    tp->size = get_mode_size(m);
    tp->mode = m;
  }
}

ident*      get_type_ident(type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->name;
}

void        set_type_ident(type *tp, ident* id) {
  assert(tp && tp->kind == k_type);
  tp->name = id;
}

const char* get_type_name(type *tp) {
  assert(tp && tp->kind == k_type);
  return (id_to_str(tp->name));
}

int         get_type_size(type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->size;
}

void
set_type_size(type *tp, int size) {
  assert(tp && tp->kind == k_type);
  /* For pointer enumeration and primitive size depends on the mode.
     Methods don't have a size. */
  if ((tp->type_op != type_pointer) && (tp->type_op != type_primitive) &&
      (tp->type_op != type_enumeration) && (tp->type_op != type_method))
    tp->size = size;
}

type_state
get_type_state(type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->state;
}

void
set_type_state(type *tp, type_state state) {
  assert(tp && tp->kind == k_type);

  if ((tp->type_op == type_pointer) && (tp->type_op == type_primitive) &&
      (tp->type_op == type_method))
    return;

  /* Just a correctness check: */
  if (state == layout_fixed) {
    int i;
    switch (get_type_tpop_code(tp)) {
    case tpo_class:
      {
	assert(get_type_size(tp) > -1);
	if (tp != get_glob_type())
	  for (i = 0; i < get_class_n_member(tp); i++) {
	    assert(get_entity_offset(get_class_member(tp, i)) > -1);
	    assert(is_method_type(get_entity_type(get_class_member(tp, i))) ||
		   (get_entity_allocation(get_class_member(tp, i)) == automatic_allocated));
	    /*    @@@ lowerfirm geht nicht durch */
	  }
      } break;
    case tpo_struct:
      {
	/* assert(get_type_size(tp) > -1);    @@@ lowerfirm geht nicht durch */
	for (i = 0; i < get_struct_n_member(tp); i++) {
	  assert(get_entity_offset(get_struct_member(tp, i)) > -1);
	  assert((get_entity_allocation(get_struct_member(tp, i)) == automatic_allocated));
	}
      } break;
    case tpo_union:
      { /* ?? */
      } break;
    case tpo_array:
      { /* ??
	 Check order?
	 Assure that only innermost dimension is dynamic? */
      } break;
    case tpo_enumeration:
      {
	assert(get_type_mode != NULL);
	for (i = 0; i < get_enumeration_n_enums(tp); i++)
	  assert(get_enumeration_enum(tp, i) != NULL);
      } break;
    default: break;
    } /* switch (tp) */
  }
  tp->state = state;
}

unsigned long get_type_visited(type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->visit;
}

void        set_type_visited(type *tp, unsigned long num) {
  assert(tp && tp->kind == k_type);
  tp->visit = num;
}
/* Sets visited field in type to type_visited. */
void        mark_type_visited(type *tp) {
  assert(tp && tp->kind == k_type);
  assert(tp->visit < type_visited);
  tp->visit = type_visited;
}

int is_type            (void *thing) {
  assert(thing);
  if (get_kind(thing) == k_type)
    return 1;
  else
    return 0;
}

/*******************************************************************/
/** TYPE_CLASS                                                    **/
/*******************************************************************/

/* create a new class type */
type   *new_type_class (ident *name) {
  type *res;

  res = new_type(type_class, NULL, name);

  res->attr.ca.members    = NEW_ARR_F (entity *, 1);
  res->attr.ca.subtypes   = NEW_ARR_F (type *, 1);
  res->attr.ca.supertypes = NEW_ARR_F (type *, 1);
  res->attr.ca.peculiarity = existent;
  res->attr.ca.dfn        = 0;

  return res;
}
INLINE void free_class_attrs(type *clss) {
  assert(clss && (clss->type_op == type_class));
  DEL_ARR_F(clss->attr.ca.members);
  DEL_ARR_F(clss->attr.ca.subtypes);
  DEL_ARR_F(clss->attr.ca.supertypes);
}
/* manipulate private fields of class type  */
void    add_class_member   (type *clss, entity *member) {
  assert(clss && (clss->type_op == type_class));
  ARR_APP1 (entity *, clss->attr.ca.members, member);
}
int     get_class_n_member (type *clss) {
  assert(clss && (clss->type_op == type_class));
  return (ARR_LEN (clss->attr.ca.members))-1;
}
entity *get_class_member   (type *clss, int pos) {
  assert(clss && (clss->type_op == type_class));
  assert(pos >= 0 && pos < get_class_n_member(clss));
  return clss->attr.ca.members[pos+1];
}
void    set_class_member   (type *clss, entity *member, int pos) {
  assert(clss && (clss->type_op == type_class));
  assert(pos >= 0 && pos < get_class_n_member(clss));
  clss->attr.ca.members[pos+1] = member;
}
void    set_class_members  (type *clss, entity **members, int arity) {
  int i;
  assert(clss && (clss->type_op == type_class));
  DEL_ARR_F(clss->attr.ca.members);
  clss->attr.ca.members    = NEW_ARR_F (entity *, 1);
  for (i = 0; i < arity; i++) {
    set_entity_owner(members[i], clss);
    ARR_APP1 (entity *, clss->attr.ca.members, members[i]);
  }
}
void    remove_class_member(type *clss, entity *member) {
  int i;
  assert(clss && (clss->type_op == type_class));
  for (i = 1; i < (ARR_LEN (clss->attr.ca.members))-1; i++)
    if (clss->attr.ca.members[i+1] == member) {
      for(i++; i < (ARR_LEN (clss->attr.ca.members)) - 1; i++)
	clss->attr.ca.members[i] = clss->attr.ca.members[i + 1];
      ARR_SETLEN(entity*, clss->attr.ca.members, ARR_LEN(clss->attr.ca.members) - 1);
      break;
    }
}

void    add_class_subtype   (type *clss, type *subtype) {
  int i;
  assert(clss && (clss->type_op == type_class));
  ARR_APP1 (type *, clss->attr.ca.subtypes, subtype);
  for (i = 0; i < get_class_n_supertype(subtype); i++)
    if (get_class_supertype(subtype, i) == clss)
      /* Class already registered */
      return;
  ARR_APP1 (type *, subtype->attr.ca.supertypes, clss);
}
int     get_class_n_subtype (type *clss) {
  assert(clss && (clss->type_op == type_class));
  return (ARR_LEN (clss->attr.ca.subtypes))-1;
}
type   *get_class_subtype   (type *clss, int pos) {
  assert(clss && (clss->type_op == type_class));
  assert(pos >= 0 && pos < get_class_n_subtype(clss));
  return clss->attr.ca.subtypes[pos+1] = skip_tid(clss->attr.ca.subtypes[pos+1]);
}
void    set_class_subtype   (type *clss, type *subtype, int pos) {
  assert(clss && (clss->type_op == type_class));
  assert(pos >= 0 && pos < get_class_n_subtype(clss));
  clss->attr.ca.subtypes[pos+1] = subtype;
}
void    remove_class_subtype(type *clss, type *subtype) {
  int i;
  assert(clss && (clss->type_op == type_class));
  for (i = 1; i < (ARR_LEN (clss->attr.ca.subtypes))-1; i++)
    if (clss->attr.ca.subtypes[i+1] == subtype) {
      for(i++; i < (ARR_LEN (clss->attr.ca.subtypes))-1; i++)
	clss->attr.ca.subtypes[i] = clss->attr.ca.subtypes[i+1];
      ARR_SETLEN(entity*, clss->attr.ca.subtypes, ARR_LEN(clss->attr.ca.subtypes) - 1);
      break;
    }
}

void    add_class_supertype   (type *clss, type *supertype) {
  int i;
  assert(clss && (clss->type_op == type_class));
  assert(supertype && (supertype -> type_op == type_class));
  ARR_APP1 (type *, clss->attr.ca.supertypes, supertype);
  for (i = 0; i < get_class_n_subtype(supertype); i++)
    if (get_class_subtype(supertype, i) == clss)
      /* Class already registered */
      return;
  ARR_APP1 (type *, supertype->attr.ca.subtypes, clss);
}
int     get_class_n_supertype (type *clss) {
  assert(clss && (clss->type_op == type_class));
  return (ARR_LEN (clss->attr.ca.supertypes))-1;
}
type   *get_class_supertype   (type *clss, int pos) {
  assert(clss && (clss->type_op == type_class));
  assert(pos >= 0 && pos < get_class_n_supertype(clss));
  return clss->attr.ca.supertypes[pos+1] = skip_tid(clss->attr.ca.supertypes[pos+1]);
}
void    set_class_supertype   (type *clss, type *supertype, int pos) {
  assert(clss && (clss->type_op == type_class));
  assert(pos >= 0 && pos < get_class_n_supertype(clss));
  clss->attr.ca.supertypes[pos+1] = supertype;
}
void    remove_class_supertype(type *clss, type *supertype) {
  int i;
  assert(clss && (clss->type_op == type_class));
  for (i = 1; i < (ARR_LEN (clss->attr.ca.supertypes))-1; i++)
    if (clss->attr.ca.supertypes[i+1] == supertype) {
      for(i++; i < (ARR_LEN (clss->attr.ca.supertypes))-1; i++)
	clss->attr.ca.supertypes[i] = clss->attr.ca.supertypes[i+1];
      ARR_SETLEN(entity*, clss->attr.ca.supertypes, ARR_LEN(clss->attr.ca.supertypes) - 1);
      break;
    }
}

INLINE peculiarity get_class_peculiarity (type *clss) {
  assert(clss && (clss->type_op == type_class));
  return clss->attr.ca.peculiarity;
}
INLINE void        set_class_peculiarity (type *clss, peculiarity pec) {
  assert(clss && (clss->type_op == type_class));
  clss->attr.ca.peculiarity = pec;
}

void set_class_dfn (type *clss, int dfn)
{
  clss->attr.ca.dfn        = dfn;
}

int get_class_dfn (type *clss)
{
  return (clss->attr.ca.dfn);
}

/* typecheck */
bool    is_class_type(type *clss) {
  assert(clss);
  if (clss->type_op == type_class) return 1; else return 0;
}

/*******************************************************************/
/** TYPE_STRUCT                                                   **/
/*******************************************************************/

/* create a new type struct */
type   *new_type_struct (ident *name) {
  type *res;
  res = new_type(type_struct, NULL, name);
  res->attr.sa.members = NEW_ARR_F (entity *, 1);
  return res;
}
INLINE void free_struct_attrs (type *strct) {
  assert(strct && (strct->type_op == type_struct));
  DEL_ARR_F(strct->attr.sa.members);
}
/* manipulate private fields of struct */
void    add_struct_member   (type *strct, entity *member) {
  assert(strct && (strct->type_op == type_struct));
  assert(get_type_tpop(get_entity_type(member)) != type_method);
    /*    @@@ lowerfirm geht nicht durch */
  ARR_APP1 (entity *, strct->attr.sa.members, member);
}
int     get_struct_n_member (type *strct) {
  assert(strct && (strct->type_op == type_struct));
  return (ARR_LEN (strct->attr.sa.members))-1;
}
entity *get_struct_member   (type *strct, int pos) {
  assert(strct && (strct->type_op == type_struct));
  assert(pos >= 0 && pos < get_struct_n_member(strct));
  return strct->attr.sa.members[pos+1];
}
void    set_struct_member   (type *strct, int pos, entity *member) {
  assert(strct && (strct->type_op == type_struct));
  assert(pos >= 0 && pos < get_struct_n_member(strct));
  assert(get_entity_type(member)->type_op != type_method);/* @@@ lowerfirm !!*/
  strct->attr.sa.members[pos+1] = member;
}
void    remove_struct_member(type *strct, entity *member) {
  int i;
  assert(strct && (strct->type_op == type_struct));
  for (i = 1; i < (ARR_LEN (strct->attr.sa.members))-1; i++)
    if (strct->attr.sa.members[i+1] == member) {
      for(i++; i < (ARR_LEN (strct->attr.sa.members))-1; i++)
	strct->attr.sa.members[i] = strct->attr.sa.members[i+1];
      ARR_SETLEN(entity*, strct->attr.sa.members, ARR_LEN(strct->attr.sa.members) - 1);
      break;
    }
}
/* typecheck */
bool    is_struct_type(type *strct) {
  assert(strct);
  if (strct->type_op == type_struct) return 1; else return 0;
}

/*******************************************************************/
/** TYPE_METHOD                                                   **/
/*******************************************************************/

/* Create a new method type.
   N_param is the number of parameters, n_res the number of results.  */
type *new_type_method (ident *name, int n_param, int n_res) {
  type *res;
  res = new_type(type_method, mode_p, name);
  res->state = layout_fixed;
  res->size = get_mode_size(mode_p);
  res->attr.ma.n_params   = n_param;
  res->attr.ma.param_type = (type **) xmalloc (sizeof (type *) * n_param);
  res->attr.ma.n_res      = n_res;
  res->attr.ma.res_type   = (type **) xmalloc (sizeof (type *) * n_res);
  return res;
}
INLINE void free_method_attrs(type *method) {
  assert(method && (method->type_op == type_method));
  free(method->attr.ma.param_type);
  free(method->attr.ma.res_type);
}
/* manipulate private fields of method. */
int   get_method_n_params  (type *method) {
  assert(method && (method->type_op == type_method));
  return method->attr.ma.n_params;
}
type *get_method_param_type(type *method, int pos) {
  assert(method && (method->type_op == type_method));
  assert(pos >= 0 && pos < get_method_n_params(method));
  return method->attr.ma.param_type[pos] = skip_tid(method->attr.ma.param_type[pos]);
}
void  set_method_param_type(type *method, int pos, type* type) {
  assert(method && (method->type_op == type_method));
  assert(pos >= 0 && pos < get_method_n_params(method));
  method->attr.ma.param_type[pos] = type;
}

int   get_method_n_res   (type *method) {
  assert(method && (method->type_op == type_method));
  return method->attr.ma.n_res;
}
type *get_method_res_type(type *method, int pos) {
  assert(method && (method->type_op == type_method));
  assert(pos >= 0 && pos < get_method_n_res(method));
  return method->attr.ma.res_type[pos] = skip_tid(method->attr.ma.res_type[pos]);
}
void  set_method_res_type(type *method, int pos, type* type) {
  assert(method && (method->type_op == type_method));
  assert(pos >= 0 && pos < get_method_n_res(method));
  method->attr.ma.res_type[pos] = type;
}

/* typecheck */
bool  is_method_type     (type *method) {
  assert(method);
  if (method->type_op == type_method) return 1; else return 0;
}
/*****/

/*******************************************************************/
/** TYPE_UNION                                                    **/
/*******************************************************************/

/* create a new type uni */
type  *new_type_uni (ident *name) {
  type *res;
  res = new_type(type_union, NULL, name);
  /*res->attr.ua.unioned_type = (type **)  xmalloc (sizeof (type *)  * n_types);
    res->attr.ua.delim_names  = (ident **) xmalloc (sizeof (ident *) * n_types); */
  res->attr.ua.members = NEW_ARR_F (entity *, 1);
  return res;
}
INLINE void free_union_attrs (type *uni) {
  assert(uni && (uni->type_op == type_union));
  DEL_ARR_F(uni->attr.ua.members);
}
/* manipulate private fields of union */
#if 0
int    get_union_n_types      (type *uni) {
  assert(uni && (uni->type_op == type_union));
  return uni->attr.ua.n_types;
}
type  *get_union_unioned_type (type *uni, int pos) {
  assert(uni && (uni->type_op == type_union));
  assert(pos >= 0 && pos < get_union_n_types(uni));
  return uni->attr.ua.unioned_type[pos] = skip_tid(uni->attr.ua.unioned_type[pos]);
}
void   set_union_unioned_type (type *uni, int pos, type *type) {
  assert(uni && (uni->type_op == type_union));
  assert(pos >= 0 && pos < get_union_n_types(uni));
  uni->attr.ua.unioned_type[pos] = type;
}
ident *get_union_delim_nameid (type *uni, int pos) {
  assert(uni && (uni->type_op == type_union));
  assert(pos >= 0 && pos < get_union_n_types(uni));
  return uni->attr.ua.delim_names[pos];
}
const char *get_union_delim_name (type *uni, int pos) {
  assert(uni && (uni->type_op == type_union));
  assert(pos >= 0 && pos < get_union_n_types(uni));
  return id_to_str(uni->attr.ua.delim_names[pos]);
}
void   set_union_delim_nameid (type *uni, int pos, ident *id) {
  assert(uni && (uni->type_op == type_union));
  assert(pos >= 0 && pos < get_union_n_types(uni));
  uni->attr.ua.delim_names[pos] = id;
}
#endif
int    get_union_n_members      (type *uni) {
  assert(uni && (uni->type_op == type_union));
  return (ARR_LEN (uni->attr.ua.members))-1;
}
void    add_union_member   (type *uni, entity *member) {
  assert(uni && (uni->type_op == type_union));
  ARR_APP1 (entity *, uni->attr.ua.members, member);
}
entity  *get_union_member (type *uni, int pos) {
  assert(uni && (uni->type_op == type_union));
  assert(pos >= 0 && pos < get_union_n_members(uni));
  return uni->attr.ua.members[pos+1];
}
void   set_union_member (type *uni, int pos, entity *member) {
  assert(uni && (uni->type_op == type_union));
  assert(pos >= 0 && pos < get_union_n_members(uni));
  uni->attr.ua.members[pos+1] = member;
}
void   remove_union_member(type *uni, entity *member) {
  int i;
  assert(uni && (uni->type_op == type_union));
  for (i = 1; i < (ARR_LEN (uni->attr.ua.members))-1; i++)
    if (uni->attr.ua.members[i+1] == member) {
      for(i++; i < (ARR_LEN (uni->attr.ua.members))-1; i++)
	uni->attr.ua.members[i] = uni->attr.ua.members[i+1];
      ARR_SETLEN(entity*, uni->attr.ua.members, ARR_LEN(uni->attr.ua.members) - 1);
      break;
    }
}

/* typecheck */
bool   is_union_type         (type *uni) {
  assert(uni);
  if (uni->type_op == type_union) return 1; else return 0;
}

/*******************************************************************/
/** TYPE_ARRAY                                                    **/
/*******************************************************************/


/* create a new type array -- set dimension sizes independently */
type *new_type_array         (ident *name, int n_dimensions,
							  type *element_type) {
  type *res;
  int i;
  assert((element_type->type_op != type_method));
  assert(get_type_tpop(element_type) != type_method);
  res = new_type(type_array, NULL, name);
  res->attr.aa.n_dimensions = n_dimensions;
  res->attr.aa.lower_bound  = (ir_node **) xmalloc (sizeof (ir_node *) * n_dimensions);
  res->attr.aa.upper_bound  = (ir_node **) xmalloc (sizeof (ir_node *) * n_dimensions);
  res->attr.aa.order  = (int *) xmalloc (sizeof (int) * n_dimensions);

  for (i = 0; i < n_dimensions; i++) {
    res->attr.aa.lower_bound[i]  = NULL;
    res->attr.aa.upper_bound[i]  = NULL;
    res->attr.aa.order[i] = i;
  }
  res->attr.aa.element_type = element_type;
  new_entity(res, mangle_u(name, id_from_str("elem_ent", 8)), element_type);
  return res;
}

INLINE void free_array_attrs (type *array) {
  assert(array && (array->type_op == type_array));
  free(array->attr.aa.lower_bound);
  free(array->attr.aa.upper_bound);
}

/* manipulate private fields of array type */
int   get_array_n_dimensions (type *array) {
  assert(array && (array->type_op == type_array));
  return array->attr.aa.n_dimensions;
}
void  set_array_bounds_int (type *array, int dimension, int lower_bound,
			    int upper_bound) {
  ir_graph *rem;
  assert(array && (array->type_op == type_array));
  rem = current_ir_graph;
  current_ir_graph = get_const_code_irg();
  array->attr.aa.lower_bound[dimension] =
    new_Const(mode_I, tarval_from_long (mode_I, lower_bound));
  array->attr.aa.upper_bound[dimension] =
    new_Const(mode_I, tarval_from_long (mode_I, upper_bound));
  current_ir_graph = rem;
}

void  set_array_bounds (type *array, int dimension, ir_node * lower_bound,
			ir_node * upper_bound) {
  assert(array && (array->type_op == type_array));
  array->attr.aa.lower_bound[dimension] = lower_bound;
  array->attr.aa.upper_bound[dimension] = upper_bound;
}
void  set_array_lower_bound_int (type *array, int dimension, int lower_bound) {
  ir_graph *rem;
  assert(array && (array->type_op == type_array));
  rem = current_ir_graph;
  current_ir_graph = get_const_code_irg();
  array->attr.aa.lower_bound[dimension] =
    new_Const(mode_I, tarval_from_long (mode_I, lower_bound));
  current_ir_graph = rem;
}
void  set_array_lower_bound  (type *array, int dimension, ir_node * lower_bound) {
  assert(array && (array->type_op == type_array));
  array->attr.aa.lower_bound[dimension] = lower_bound;
}
void  set_array_upper_bound  (type *array, int dimension, ir_node * upper_bound) {
  assert(array && (array->type_op == type_array));
  array->attr.aa.upper_bound[dimension] = upper_bound;
}
ir_node * get_array_lower_bound  (type *array, int dimension) {
  assert(array && (array->type_op == type_array));
  return array->attr.aa.lower_bound[dimension];
}
ir_node * get_array_upper_bound  (type *array, int dimension) {
  assert(array && (array->type_op == type_array));
  return array->attr.aa.upper_bound[dimension];
}
void set_array_order (type *array, int dimension, int order) {
  assert(array && (array->type_op == type_array));
  array->attr.aa.order[dimension] = order;
}
int  get_array_order (type *array, int dimension) {
  assert(array && (array->type_op == type_array));
  return array->attr.aa.order[dimension];
}
void  set_array_element_type (type *array, type *type) {
  assert(array && (array->type_op == type_array));
  array->attr.aa.element_type = type;
}
type *get_array_element_type (type *array) {
  assert(array && (array->type_op == type_array));
  return array->attr.aa.element_type = skip_tid(array->attr.aa.element_type);
}
void  set_array_element_entity (type *array, entity *ent) {
  assert(array && (array->type_op == type_array));
  assert((get_entity_type(ent)->type_op != type_method));
  array->attr.aa.element_ent = ent;
  array->attr.aa.element_type = get_entity_type(ent);
}
entity *get_array_element_entity (type *array) {
  assert(array && (array->type_op == type_array));
  return array->attr.aa.element_ent;
}

/* typecheck */
bool   is_array_type         (type *array) {
  assert(array);
  if (array->type_op == type_array) return 1; else return 0;
}

/*******************************************************************/
/** TYPE_ENUMERATION                                              **/
/*******************************************************************/

/* create a new type enumeration -- set the enumerators independently */
type   *new_type_enumeration    (ident *name, int n_enums) {
  type *res;
  int i;
  res = new_type(type_enumeration, NULL, name);
  res->attr.ea.n_enums     = n_enums;
  res->attr.ea.enumer      = (tarval **) xmalloc (sizeof (tarval *) * n_enums);
  res->attr.ea.enum_nameid = (ident  **) xmalloc (sizeof (ident  *) * n_enums);
  for (i = 0; i < n_enums; i++) {
    res->attr.ea.enumer[i] = NULL;
    res->attr.ea.enum_nameid  = NULL;
  }
  return res;
}

INLINE void free_enumeration_attrs(type *enumeration) {
  assert(enumeration && (enumeration->type_op == type_enumeration));
  free(enumeration->attr.ea.enumer);
  free(enumeration->attr.ea.enum_nameid);
}

/* manipulate fields of enumeration type. */
int     get_enumeration_n_enums (type *enumeration) {
  assert(enumeration && (enumeration->type_op == type_enumeration));
  return enumeration->attr.ea.n_enums;
}
void    set_enumeration_enum    (type *enumeration, int pos, tarval *con) {
  assert(enumeration && (enumeration->type_op == type_enumeration));
  assert(pos >= 0 && pos < get_enumeration_n_enums(enumeration));
  enumeration->attr.ea.enumer[pos] = con;
}
tarval *get_enumeration_enum    (type *enumeration, int pos) {
  assert(enumeration && (enumeration->type_op == type_enumeration));
  assert(pos >= 0 && pos < get_enumeration_n_enums(enumeration));
  return enumeration->attr.ea.enumer[pos];
}
void    set_enumeration_nameid  (type *enumeration, int pos, ident *id) {
  assert(enumeration && (enumeration->type_op == type_enumeration));
  assert(pos >= 0 && pos < get_enumeration_n_enums(enumeration));
  enumeration->attr.ea.enum_nameid[pos] = id;
}
ident  *get_enumeration_nameid  (type *enumeration, int pos) {
  assert(enumeration && (enumeration->type_op == type_enumeration));
  assert(pos >= 0 && pos < get_enumeration_n_enums(enumeration));
  return enumeration->attr.ea.enum_nameid[pos];
}
const char *get_enumeration_name(type *enumeration, int pos) {
  assert(enumeration && (enumeration->type_op == type_enumeration));
  assert(pos >= 0 && pos < get_enumeration_n_enums(enumeration));
  return id_to_str(enumeration->attr.ea.enum_nameid[pos]);
}

/* typecheck */
bool    is_enumeration_type     (type *enumeration) {
  assert(enumeration);
  if (enumeration->type_op == type_enumeration) return 1; else return 0;
}

/*******************************************************************/
/** TYPE_POINTER                                                  **/
/*******************************************************************/

/* Create a new type pointer */
type *new_type_pointer           (ident *name, type *points_to) {
  type *res;
  res = new_type(type_pointer, mode_p, name);
  res->attr.pa.points_to = points_to;
  res->size = get_mode_size(res->mode);
  res->state = layout_fixed;
  return res;
}
INLINE void free_pointer_attrs (type *pointer) {
  assert(pointer && (pointer->type_op == type_pointer));
}
/* manipulate fields of type_pointer */
void  set_pointer_points_to_type (type *pointer, type *type) {
  assert(pointer && (pointer->type_op == type_pointer));
  pointer->attr.pa.points_to = type;
}
type *get_pointer_points_to_type (type *pointer) {
  assert(pointer && (pointer->type_op == type_pointer));
  return pointer->attr.pa.points_to = skip_tid(pointer->attr.pa.points_to);
}

/* typecheck */
bool  is_pointer_type            (type *pointer) {
  assert(pointer);
  if (pointer->type_op == type_pointer) return 1; else return 0;
}


/*******************************************************************/
/** TYPE_PRIMITIVE                                                **/
/*******************************************************************/

/* create a new type primitive */
type *new_type_primitive (ident *name, ir_mode *mode) {
  type *res;
  /* @@@ assert( mode_is_data(mode) && (!mode == mode_p)); */
  res = new_type(type_primitive, mode, name);
  res->size = get_mode_size(mode);
  res->state = layout_fixed;
  return res;
}
INLINE void free_primitive_attrs (type *primitive) {
  assert(primitive && (primitive->type_op == type_primitive));
}

/* typecheck */
bool  is_primitive_type  (type *primitive) {
  assert(primitive && primitive->kind == k_type);
  if (primitive->type_op == type_primitive) return 1; else return 0;
}

/*******************************************************************/
/** common functionality                                          **/
/*******************************************************************/


INLINE int is_atomic_type(type *tp) {
  assert(tp && tp->kind == k_type);
  return (is_primitive_type(tp) || is_pointer_type(tp) ||
	  is_enumeration_type(tp));
}
INLINE int is_compound_type(type *tp) {
  assert(tp && tp->kind == k_type);
  return (is_class_type(tp) || is_struct_type(tp) ||
	  is_array_type(tp) || is_union_type(tp));
}
