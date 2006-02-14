/*
 * Project:     libFIRM
 * File name:   ir/tr/type_t.h
 * Purpose:     Representation of types -- private header.
 * Author:      Goetz Lindenmaier
 * Modified by: Michael Beck
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 2001-2003 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

#ifndef _TYPE_T_H_
#define _TYPE_T_H_

#include "firm_config.h"
#include "type.h"
#include "tpop_t.h"
#include "irgraph.h"

#include "array.h"

/**
 * @file type_t.h
 * This file contains the datatypes hidden in type.h.
 *
 * @author Goetz Lindenmaier
 * @see  type.h tpop_t.h tpop.h
 */

/** class attributes */
typedef struct {
  entity  **members;   /**< fields and methods of this class */
  ir_type **subtypes;  /**< direct subtypes */
  ir_type **supertypes;/**< direct supertypes */
  peculiarity peculiarity;  /**< peculiarity of this class */
  entity  *type_info;  /**< a entity representing this class, used for type info */
  unsigned final;      /**< non-zero if this is a final class */
  int dfn;             /**< number used for 'instanceof' operator */
} cls_attr;

/** struct attributes */
typedef struct {
  entity **members;    /**< fields of this struct. No method entities allowed. */
} stc_attr;

/** A (type, entity) pair */
typedef struct {
  ir_type *tp;         /**< a type */
  entity  *ent;        /**< an entity */
} tp_ent_pair;

/** method attributes */
typedef struct {
  int n_params;                   /**< number of parameters */
  tp_ent_pair *param_type;        /**< array of parameter type/value entities pairs */
  ir_type *value_params;          /**< A type whose entities represent copied value arguments. */
  int n_res;                      /**< number of results */
  tp_ent_pair *res_type;          /**< array of result type/value entity pairs */
  ir_type *value_ress;            /**< A type whose entities represent copied value results. */
  variadicity variadicity;        /**< variadicity of the method. */
  int first_variadic_param;       /**< index of the first variadic param or -1 if non-variadic .*/
  unsigned additional_properties; /**< Set of additional method properties. */
  unsigned irg_calling_conv;      /**< this is a set of calling convention flags. */
} mtd_attr;

/** union attributes */
typedef struct {
  entity **members;    /**< fields of this union. No method entities allowed. */
} uni_attr;

/** array attributes */
typedef struct {
  int   n_dimensions;     /**< Number of array dimensions.  */
  ir_node **lower_bound;  /**< Lower bounds of dimensions.  Usually all 0. */
  ir_node **upper_bound;  /**< Upper bounds or dimensions. */
  int *order;             /**< Ordering of dimensions. */
  ir_type *element_type;  /**< The type of the array elements. */
  entity *element_ent;    /**< Entity for the array elements, to be used for
                               element selection with Sel. */
} arr_attr;

/** enum attributes */
typedef struct {
  int      n_enums;    /**< Number of enumerators. */
  tarval **enumer;     /**< Contains all constants that represent a member
                          of the enum -- enumerators. */
  ident  **enum_nameid;/**< Contains the names of the enum fields as specified by
                          the source program */
} enm_attr;

/** pointer attributes */
typedef struct {
  ir_type *points_to;  /**< The type of the entity the pointer points to. */
} ptr_attr;

/*
typedef struct {   * No private attr yet! *
} pri_attr; */


/*
typedef struct {        * No private attr, must be smaller than others! *
} id_attr;
*/

/** General type attributes. */
typedef union {
  cls_attr ca;      /**< attributes of a class type */
  stc_attr sa;      /**< attributes of a struct type */
  mtd_attr ma;      /**< attributes of a method type */
  uni_attr ua;      /**< attributes of an union type */
  arr_attr aa;      /**< attributes of an array type */
  enm_attr ea;      /**< attributes of an enumeration type */
  ptr_attr pa;      /**< attributes of a pointer type */
} tp_attr;

enum type_flags {
  tf_none         = 0,  /**< No flags. */
  tf_frame_type   = 1,  /**< Set if this is a frame type. */
  tf_lowered_type = 2,  /**< Set if this is a lowered type. */
  tf_layout_fixed = 4   /**< set if the layout of a type is fixed */
};

/** the structure of a type */
struct ir_type {
  firm_kind kind;          /**< the firm kind, must be k_type */
  const tp_op *type_op;    /**< the type operation of the type */
  ident *name;             /**< The name of the type */
  visibility visibility;   /**< Visibility of entities of this type. */
  unsigned flags;          /**< Type flags, a bitmask of enum type_flags. */
  int size;                /**< Size of an entity of this type. This is determined
                                when fixing the layout of this class.  Size must be
                                given in bits. */
  int align;               /**< Alignment of an entity of this type. This should be
                                set according to the source language needs. If not set it's
                                calculated automatically by get_type_alignment().
                                Alignment must be given in bits. */
  ir_mode *mode;           /**< The mode for atomic types */
  unsigned long visit;     /**< visited counter for walks of the type information */
  void *link;              /**< holds temporary data - like in irnode_t.h */
  struct dbg_info *dbi;    /**< A pointer to information for debug support. */
  ir_type *assoc_type;     /**< The associated lowered/unlowered type */

  /* ------------- fields for analyses ---------------*/

#ifdef DEBUG_libfirm
  int nr;             /**< a unique node number for each node to make output
                           readable. */
#endif
  tp_attr attr;            /* type kind specific fields. This must be the last
                  entry in this struct!  Varying size! */
};

/**
 *   Creates a new type representation:
 *
 *   @param type_op  the kind of this type.  May not be type_id.
 *   @param mode     the mode to be used for this type, may be NULL
 *   @param name     an ident for the name of this type.
 *   @param db       debug info
 *
 *   @return A new type of the given type.  The remaining private attributes are not
 *           initialized.  The type is in state layout_undefined.
 */
ir_type *
new_type(tp_op *type_op, ir_mode *mode, ident *name, dbg_info *db);
void free_type_attrs       (ir_type *tp);

void free_class_entities      (ir_type *clss);
void free_struct_entities     (ir_type *strct);
void free_method_entities     (ir_type *method);
void free_union_entities      (ir_type *uni);
void free_array_entities      (ir_type *array);
void free_enumeration_entities(ir_type *enumeration);
void free_pointer_entities    (ir_type *pointer);

void free_array_automatic_entities(ir_type *array);

void free_class_attrs      (ir_type *clss);
void free_struct_attrs     (ir_type *strct);
void free_method_attrs     (ir_type *method);
void free_union_attrs      (ir_type *uni);
void free_array_attrs      (ir_type *array);
void free_enumeration_attrs(ir_type *enumeration);
void free_pointer_attrs    (ir_type *pointer);

void set_class_mode(ir_type *tp, ir_mode *mode);
void set_struct_mode(ir_type *tp, ir_mode *mode);
void set_pointer_mode(ir_type *tp, ir_mode *mode);
void set_primitive_mode(ir_type *tp, ir_mode *mode);
void set_enumeration_mode(ir_type *tp, ir_mode *mode);

void set_class_size_bits(ir_type *tp, int bits);
void set_struct_size_bits(ir_type *tp, int bits);
void set_union_size_bits(ir_type *tp, int bits);
void set_array_size_bits(ir_type *tp, int size);
void set_default_size_bits(ir_type *tp, int size);

/**
 * Initialize the type module.
 *
 * @param builtin_db       debug info for built-in objects
 * @param default_cc_mask  default calling conventions for methods
 */
void firm_init_type(dbg_info *builtin_db, unsigned default_cc_mask);


/* ------------------- *
 *  inline functions   *
 * ------------------- */

extern unsigned long firm_type_visited;

static INLINE void _set_master_type_visited(unsigned long val) { firm_type_visited = val; }
static INLINE unsigned long _get_master_type_visited(void)     { return firm_type_visited; }
static INLINE void _inc_master_type_visited(void)              { ++firm_type_visited; }

static INLINE void *
_get_type_link(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return(tp -> link);
}

static INLINE void
_set_type_link(ir_type *tp, void *l) {
  assert(tp && tp->kind == k_type);
  tp -> link = l;
}

static INLINE const tp_op*
_get_type_tpop(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->type_op;
}

static INLINE ident*
_get_type_tpop_nameid(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return get_tpop_ident(tp->type_op);
}

static INLINE tp_opcode
_get_type_tpop_code(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return get_tpop_code(tp->type_op);
}

static INLINE ir_mode *
_get_type_mode(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->mode;
}

static INLINE ident *
_get_type_ident(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->name;
}

static INLINE void
_set_type_ident(ir_type *tp, ident* id) {
  assert(tp && tp->kind == k_type);
  tp->name = id;
}

static INLINE int
_get_type_size_bits(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->size;
}

static INLINE int
_get_type_size_bytes(const ir_type *tp) {
  int size = _get_type_size_bits(tp);
  if (size < 0)
    return -1;
  if ((size & 7) != 0) {
    assert(0 && "cannot take byte size of this type");
    return -1;
  }
  return size >> 3;
}

static INLINE type_state
_get_type_state(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->flags & tf_layout_fixed ? layout_fixed : layout_undefined;
}

static INLINE unsigned long
_get_type_visited(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->visit;
}

static INLINE void
_set_type_visited(ir_type *tp, unsigned long num) {
  assert(tp && tp->kind == k_type);
  tp->visit = num;
}

static INLINE void
_mark_type_visited(ir_type *tp) {
  assert(tp && tp->kind == k_type);
  assert(tp->visit < firm_type_visited);
  tp->visit = firm_type_visited;
}

static INLINE int
_type_visited(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->visit >= firm_type_visited;
}

static INLINE int
_type_not_visited(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return tp->visit  < firm_type_visited;
}

static INLINE int
_is_type(const void *thing) {
  return (get_kind(thing) == k_type);
}

static INLINE int
_is_class_type(const ir_type *clss) {
  assert(clss);
  return (clss->type_op == type_class);
}

static INLINE int
_get_class_n_members (const ir_type *clss) {
  assert(clss && (clss->type_op == type_class));
  return (ARR_LEN (clss->attr.ca.members));
}

static INLINE entity *
_get_class_member   (const ir_type *clss, int pos) {
  assert(clss && (clss->type_op == type_class));
  assert(pos >= 0 && pos < _get_class_n_members(clss));
  return clss->attr.ca.members[pos];
}

static INLINE int
_is_class_final   (const ir_type *clss) {
  assert(clss && (clss->type_op == type_class));
  return clss->attr.ca.final;
}

static INLINE void
_set_class_final   (ir_type *clss, int final) {
  assert(clss && (clss->type_op == type_class));
  clss->attr.ca.final = final;
}

static INLINE int
_is_struct_type(const ir_type *strct) {
  assert(strct);
  return (strct->type_op == type_struct);
}

static INLINE int
_is_method_type(const ir_type *method) {
  assert(method);
  return (method->type_op == type_method);
}

static INLINE int
_is_union_type(const ir_type *uni) {
  assert(uni);
  return (uni->type_op == type_union);
}

static INLINE int
_is_array_type(const ir_type *array) {
  assert(array);
  return (array->type_op == type_array);
}

static INLINE int
_is_enumeration_type(const ir_type *enumeration) {
  assert(enumeration);
  return (enumeration->type_op == type_enumeration);
}

static INLINE int
_is_pointer_type(const ir_type *pointer) {
  assert(pointer);
  return (pointer->type_op == type_pointer);
}

/** Returns true if a type is a primitive type. */
static INLINE int
_is_primitive_type(const ir_type *primitive) {
  assert(primitive && primitive->kind == k_type);
  return (primitive->type_op == type_primitive);
}

static INLINE int
_is_atomic_type(const ir_type *tp) {
  assert(tp && tp->kind == k_type);
  return (_is_primitive_type(tp) || _is_pointer_type(tp) ||
      _is_enumeration_type(tp));
}

static INLINE int
_get_method_n_params(const ir_type *method) {
  assert(method && (method->type_op == type_method));
  return method->attr.ma.n_params;
}

static INLINE int
_get_method_n_ress(const ir_type *method) {
  assert(method && (method->type_op == type_method));
  return method->attr.ma.n_res;
}

static INLINE unsigned
_get_method_additional_properties(const ir_type *method) {
  assert(method && (method->type_op == type_method));
  return method->attr.ma.additional_properties;
}

static INLINE void
_set_method_additional_properties(ir_type *method, unsigned mask) {
  assert(method && (method->type_op == type_method));

  /* do not allow to set the mtp_property_inherited flag or
   * the automatic inheritance of flags will not work */
  method->attr.ma.additional_properties = mask & ~mtp_property_inherited;
}

static INLINE void
_set_method_additional_property(ir_type *method, mtp_additional_property flag) {
  assert(method && (method->type_op == type_method));

  /* do not allow to set the mtp_property_inherited flag or
   * the automatic inheritance of flags will not work */
  method->attr.ma.additional_properties |= flag & ~mtp_property_inherited;
}

static INLINE unsigned
_get_method_calling_convention(const ir_type *method) {
  assert(method && (method->type_op == type_method));
  return method->attr.ma.irg_calling_conv;
}

static INLINE void
_set_method_calling_convention(ir_type *method, unsigned cc_mask) {
  assert(method && (method->type_op == type_method));
  method->attr.ma.irg_calling_conv = cc_mask;
}

#define set_master_type_visited(val)      _set_master_type_visited(val)
#define get_master_type_visited()         _get_master_type_visited()
#define inc_master_type_visited()         _inc_master_type_visited()
#define get_type_link(tp)                 _get_type_link(tp)
#define set_type_link(tp, l)              _set_type_link(tp, l)
#define get_type_tpop(tp)                 _get_type_tpop(tp)
#define get_type_tpop_nameid(tp)          _get_type_tpop_nameid(tp)
#define get_type_tpop_code(tp)            _get_type_tpop_code(tp)
#define get_type_mode(tp)                 _get_type_mode(tp)
#define get_type_ident(tp)                _get_type_ident(tp)
#define set_type_ident(tp, id)            _set_type_ident(tp, id)
#define get_type_size_bits(tp)            _get_type_size_bits(tp)
#define get_type_size_bytes(tp)           _get_type_size_bytes(tp)
#define get_type_state(tp)                _get_type_state(tp)
#define get_type_visited(tp)              _get_type_visited(tp)
#define set_type_visited(tp, num)         _set_type_visited(tp, num)
#define mark_type_visited(tp)             _mark_type_visited(tp)
#define type_visited(tp)                  _type_visited(tp)
#define type_not_visited(tp)              _type_not_visited(tp)
#define is_type(thing)                    _is_type(thing)
#define is_Class_type(clss)               _is_class_type(clss)
#define get_class_n_members(clss)         _get_class_n_members(clss)
#define get_class_member(clss, pos)       _get_class_member(clss, pos)
#define is_class_final(clss)              _is_class_final(clss)
#define set_class_final(clss, final)      _set_class_final(clss, final)
#define is_Struct_type(strct)             _is_struct_type(strct)
#define is_Method_type(method)            _is_method_type(method)
#define is_Union_type(uni)                _is_union_type(uni)
#define is_Array_type(array)              _is_array_type(array)
#define is_Enumeration_type(enumeration)  _is_enumeration_type(enumeration)
#define is_Pointer_type(pointer)          _is_pointer_type(pointer)
#define is_Primitive_type(primitive)      _is_primitive_type(primitive)
#define is_atomic_type(tp)                _is_atomic_type(tp)
#define get_method_n_params(method)       _get_method_n_params(method)
#define get_method_n_ress(method)         _get_method_n_ress(method)
#define get_method_additional_properties(method)        _get_method_additional_properties(method)
#define set_method_additional_properties(method, mask)  _set_method_additional_properties(method, mask)
#define set_method_additional_property(method, flag)    _set_method_additional_property(method, flag)
#define get_method_calling_convention(method)           _get_method_calling_convention(method)
#define set_method_calling_convention(method, cc_mask)  _set_method_calling_convention(method, cc_mask)

#endif /* _TYPE_T_H_ */
