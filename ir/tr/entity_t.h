/*
 * Project:     libFIRM
 * File name:   ir/tr/entity_t.h
 * Purpose:     Representation of all program known entities -- private header.
 * Author:      Martin Trapp, Christian Schaefer
 * Modified by: Goetz Lindenmaier, Michael Beck
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1998-2006 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

/**
 * @file entity_t.h
 *
 * entity.h:  entities represent all program known objects.
 *
 * @author Martin Trapp, Christian Schaefer, Goetz Lindenmaier
 *
 *  An entity is the representation of program known objects in Firm.
 *  The primary concept of entities is to represent members of complex
 *  types, i.e., fields and methods of classes.  As not all programming
 *  language model all variables and methods as members of some class,
 *  the concept of entities is extended to cover also local and global
 *  variables, and arbitrary procedures.
 *
 *  An entity always specifies the type of the object it represents and
 *  the type of the object it is a part of, the owner of the entity.
 *  Originally this is the type of the class of which the entity is a
 *  member.
 *  The owner of local variables is the procedure they are defined in.
 *  The owner of global variables and procedures visible in the whole
 *  program is a universally defined class type "GlobalType".  The owner
 *  of procedures defined in the scope of an other procedure is the
 *  enclosing procedure.
 */

#ifndef _FIRM_TR_ENTITY_T_H_
#define _FIRM_TR_ENTITY_T_H_

#include "firm_common_t.h"
#include "firm_config.h"

#include "type_t.h"
#include "entity.h"
#include "typegmod.h"
#include "mangle.h"
#include "pseudo_irg.h"

/** A path in a compound graph. */
struct compound_graph_path {
  firm_kind kind;       /**< dynamic type tag for compound graph path. */
  ir_type *tp;          /**< The type this path belongs to. */
  int len;              /**< length of the path */
  struct tuple {
    int    index;       /**< Array index.  To compute position of array elements */
    entity *node;       /**< entity */
  } list[1];            /**< List of entity/index tuple of length len to express the
                             access path. */
};

/** The attributes for atomic entities. */
typedef struct atomic_ent_attr {
  ir_node *value;            /**< value if entity is not of variability uninitialized.
                               Only for atomic entities. */
} atomic_ent_attr;

/** The attributes for compound entities. */
typedef struct compound_ent_attr {
  ir_node **values;     /**< constant values of compound entities. Only available if
                             variability not uninitialized.  Must be set for variability constant. */
  compound_graph_path **val_paths; /**< paths corresponding to constant values. Only available if
                                        variability not uninitialized.  Must be set for variability constant. */
} compound_ent_attr;

/** A reserved value for "not yet set". */
#define VTABLE_NUM_NOT_SET ((unsigned)(-1))

/** The attributes for methods. */
typedef struct method_ent_attr {
  ir_graph *irg;                 /**< The corresponding irg if known.
                                      The ir_graph constructor automatically sets this field. */
  unsigned irg_add_properties;   /**< Additional graph properties can be
                                      stored in a entity if no irg is available. */

  unsigned vtable_number;        /**< For a dynamically called method, the number assigned
                                      in the virtual function table. */

  ptr_access_kind *param_access; /**< the parameter access */
  float *param_weight;           /**< The weight of method's parameters. Parameters
                                      with a high weight are good for procedure cloning. */
  ir_img_section section;        /**< The code section where this method should be placed */
} method_ent_attr;


/** The type of an entity. */
struct entity {
  firm_kind kind;       /**< The dynamic type tag for entity. */
  ident *name;          /**< The name of this entity. */
  ident *ld_name;       /**< Unique name of this entity, i.e., the mangled
                             name.  If the field is read before written a default
                             mangling is applies.  The name of the owner is prepended
                             to the name of the entity, separated by a underscore.
                             E.g.,  for a class `A' with field `a' this
                             is the ident for `A_a'. */
  ir_type *type;        /**< The type of this entity, e.g., a method type, a
                             basic type of the language or a class itself. */
  ir_type *owner;                /**< The compound type (e.g. class type) this entity belongs to. */
  ir_allocation allocation:3;    /**< Distinguishes static and dynamically allocated
                                    entities and some further cases. */
  ir_visibility visibility:3;    /**< Specifies visibility to external program
                                      fragments. */
  ir_variability variability:3;  /**< Specifies variability of entities content. */
  ir_volatility volatility:2;    /**< Specifies volatility of entities content. */
  ir_stickyness stickyness:2;    /**< Specifies whether this entity is sticky.  */
  ir_peculiarity peculiarity:3;  /**< The peculiarity of this entity. */
  unsigned final:1;              /**< If set, this entity cannot be overridden. */
  unsigned compiler_gen:1;       /**< If set, this entity was compiler generated. */
  int offset;                    /**< Offset in bits for this entity.  Fixed when layout
                                      of owner is determined. */
  unsigned long visit;           /**< visited counter for walks of the type information. */
  struct dbg_info *dbi;          /**< A pointer to information for debug support. */
  void *link;                    /**< To store some intermediate information. */
  ir_type *repr_class;           /**< If this entity represents a class info, the associated class. */

  /* ------------- fields for entities owned by a class type ---------------*/

  entity **overwrites;     /**< A list of entities this entity overwrites. */
  entity **overwrittenby;  /**< A list of entities that overwrite this entity.  */

  /* ------------- fields for atomic entities  --------------- */
  ir_node *value;          /**< value if entity is not of variability uninitialized.
                                Only for atomic entities. */
  union {
    /* ------------- fields for compound entities -------------- */
    compound_ent_attr cmpd_attr;
    /* ------------- fields for method entities ---------------- */
    method_ent_attr   mtd_attr;
  } attr; /**< type specific attributes */

  /* ------------- fields for analyses ---------------*/

#ifdef DEBUG_libfirm
  long nr;             /**< A unique node number for each node to make output
                            readable. */
# endif /* DEBUG_libfirm */
};

/** Initialize the entity module. */
void firm_init_entity(void);


/* ----------------------- inline functions ------------------------ */
static INLINE int
_is_entity(const void *thing) {
  return get_kind(thing) == k_entity;
}

static INLINE const char *
_get_entity_name(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return get_id_str(get_entity_ident(ent));
}

static INLINE ident *
_get_entity_ident(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->name;
}

static INLINE void
_set_entity_ident(entity *ent, ident *id) {
  assert(ent && ent->kind == k_entity);
  ent->name = id;
}

static INLINE ir_type *
_get_entity_owner(entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->owner = skip_tid(ent->owner);
}

static INLINE ident *
_get_entity_ld_ident(entity *ent)
{
  assert(ent && ent->kind == k_entity);
  if (ent->ld_name == NULL)
    ent->ld_name = mangle_entity(ent);
  return ent->ld_name;
}

static INLINE void
_set_entity_ld_ident(entity *ent, ident *ld_ident) {
  assert(ent && ent->kind == k_entity);
  ent->ld_name = ld_ident;
}

static INLINE const char *
_get_entity_ld_name(entity *ent) {
  assert(ent && ent->kind == k_entity);
  return get_id_str(get_entity_ld_ident(ent));
}

static INLINE ir_type *
_get_entity_type(entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->type = skip_tid(ent->type);
}

static INLINE void
_set_entity_type(entity *ent, ir_type *type) {
  assert(ent && ent->kind == k_entity);
  ent->type = type;
}

static INLINE ir_allocation
_get_entity_allocation(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->allocation;
}

static INLINE void
_set_entity_allocation(entity *ent, ir_allocation al) {
  assert(ent && ent->kind == k_entity);
  ent->allocation = al;
}

static INLINE ir_visibility
_get_entity_visibility(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->visibility;
}

static INLINE ir_variability
_get_entity_variability(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->variability;
}

static INLINE ir_volatility
_get_entity_volatility(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->volatility;
}

static INLINE void
_set_entity_volatility(entity *ent, ir_volatility vol) {
  assert(ent && ent->kind == k_entity);
  ent->volatility = vol;
}

static INLINE ir_peculiarity
_get_entity_peculiarity(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->peculiarity;
}

/**
 * @todo Why peculiarity only for methods?
 *       Good question.  Originally, there were only description and
 *       existent.  The thought was, what sense does it make to
 *       describe a field?  With inherited the situation changed.  So
 *       I removed the assertion.  GL, 28.2.05
 */
static INLINE void
_set_entity_peculiarity(entity *ent, ir_peculiarity pec) {
  assert(ent && ent->kind == k_entity);
  /* @@@ why peculiarity only for methods? */
  //assert(is_Method_type(ent->type));

  ent->peculiarity = pec;
}

static INLINE ir_stickyness
_get_entity_stickyness(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->stickyness;
}

static INLINE void
_set_entity_stickyness(entity *ent, ir_stickyness stickyness) {
  assert(ent && ent->kind == k_entity);
  ent->stickyness = stickyness;
}

static INLINE int
_get_entity_final(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return (int)ent->final;
}

static INLINE void
_set_entity_final(entity *ent, int final) {
  assert(ent && ent->kind == k_entity);
  ent->final = final ? 1 : 0;
}

static INLINE int
_get_entity_offset_bits(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->offset;
}

static INLINE int
_get_entity_offset_bytes(const entity *ent) {
  int bits = _get_entity_offset_bits(ent);

  if (bits & 7) return -1;
  return bits >> 3;
}

static INLINE void
_set_entity_offset_bits(entity *ent, int offset) {
  assert(ent && ent->kind == k_entity);
  ent->offset = offset;
}

static INLINE void
_set_entity_offset_bytes(entity *ent, int offset) {
  _set_entity_offset_bits(ent, offset * 8);
}

static INLINE void *
_get_entity_link(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->link;
}

static INLINE void
_set_entity_link(entity *ent, void *l) {
  assert(ent && ent->kind == k_entity);
  ent->link = l;
}

static INLINE ir_graph *
_get_entity_irg(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  assert(ent == unknown_entity || is_Method_type(ent->type));
  if (!get_visit_pseudo_irgs() && ent->attr.mtd_attr.irg
      && is_pseudo_ir_graph(ent->attr.mtd_attr.irg))
    return NULL;
  return ent->attr.mtd_attr.irg;
}

static INLINE unsigned long
_get_entity_visited(entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->visit;
}

static INLINE void
_set_entity_visited(entity *ent, unsigned long num) {
  assert(ent && ent->kind == k_entity);
  ent->visit = num;
}

static INLINE void
_mark_entity_visited(entity *ent) {
  assert(ent && ent->kind == k_entity);
  ent->visit = firm_type_visited;
}

static INLINE int
_entity_visited(entity *ent) {
  return _get_entity_visited(ent) >= firm_type_visited;
}

static INLINE int
_entity_not_visited(entity *ent) {
  return _get_entity_visited(ent) < firm_type_visited;
}

static INLINE ir_type *
_get_entity_repr_class(const entity *ent) {
  assert(ent && ent->kind == k_entity);
  return ent->repr_class;
}

#define is_entity(thing)                         _is_entity(thing)
#define get_entity_name(ent)                     _get_entity_name(ent)
#define get_entity_ident(ent)                    _get_entity_ident(ent)
#define set_entity_ident(ent, id)                _set_entity_ident(ent, id)
#define get_entity_owner(ent)                    _get_entity_owner(ent)
#define get_entity_ld_ident(ent)                 _get_entity_ld_ident(ent)
#define set_entity_ld_ident(ent, ld_ident)       _set_entity_ld_ident(ent, ld_ident)
#define get_entity_ld_name(ent)                  _get_entity_ld_name(ent)
#define get_entity_type(ent)                     _get_entity_type(ent)
#define set_entity_type(ent, type)               _set_entity_type(ent, type)
#define get_entity_allocation(ent)               _get_entity_allocation(ent)
#define set_entity_allocation(ent, al)           _set_entity_allocation(ent, al)
#define get_entity_visibility(ent)               _get_entity_visibility(ent)
#define get_entity_variability(ent)              _get_entity_variability(ent)
#define get_entity_volatility(ent)               _get_entity_volatility(ent)
#define set_entity_volatility(ent, vol)          _set_entity_volatility(ent, vol)
#define get_entity_peculiarity(ent)              _get_entity_peculiarity(ent)
#define set_entity_peculiarity(ent, pec)         _set_entity_peculiarity(ent, pec)
#define get_entity_stickyness(ent)               _get_entity_stickyness(ent)
#define set_entity_stickyness(ent, stickyness)   _set_entity_stickyness(ent, stickyness)
#define get_entity_final(ent)                    _get_entity_final(ent)
#define set_entity_final(ent, final)             _set_entity_final(ent, final)
#define get_entity_offset_bits(ent)              _get_entity_offset_bits(ent)
#define get_entity_offset_bytes(ent)             _get_entity_offset_bytes(ent)
#define set_entity_offset_bits(ent, offset)      _set_entity_offset_bits(ent, offset)
#define set_entity_offset_bytes(ent, offset)     _set_entity_offset_bytes(ent, offset)
#define get_entity_link(ent)                     _get_entity_link(ent)
#define set_entity_link(ent, l)                  _set_entity_link(ent, l)
#define get_entity_irg(ent)                      _get_entity_irg(ent)
#define get_entity_visited(ent)                  _get_entity_visited(ent)
#define set_entity_visited(ent, num)             _set_entity_visited(ent, num)
#define mark_entity_visited(ent)                 _mark_entity_visited(ent)
#define entity_visited(ent)                      _entity_visited(ent)
#define entity_not_visited(ent)                  _entity_not_visited(ent)
#define get_entity_repr_class(ent)               _get_entity_repr_class(ent)


#endif /* _FIRM_TR_ENTITY_T_H_ */
