/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
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
 * @file    type.c
 * @brief   Representation of types.
 * @author  Goetz Lindenmaier, Michael Beck
 * @version $Id$
 * @brief
 *
 *  Implementation of the datastructure to hold
 *  type information.
 *
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
 */
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "type_t.h"

#include "xmalloc.h"
#include "irprog_t.h"
#include "ircons.h"
#include "tpop_t.h"
#include "tv_t.h"
#include "irhooks.h"
#include "irtools.h"
#include "entity_t.h"
#include "error.h"
#include "dbginfo.h"

#include "array.h"

ir_type *firm_none_type;
ir_type *get_none_type(void)
{
	return firm_none_type;
}

ir_type *firm_code_type;
ir_type *get_code_type(void)
{
	return firm_code_type;
}

ir_type *firm_unknown_type;
ir_type *get_unknown_type(void)
{
	return firm_unknown_type;
}

/* Suffixes added to types used for pass-by-value representations. */
static ident *value_params_suffix = NULL;
static ident *value_ress_suffix = NULL;

void ir_init_type(void)
{
	value_params_suffix = new_id_from_str(VALUE_PARAMS_SUFFIX);
	value_ress_suffix   = new_id_from_str(VALUE_RESS_SUFFIX);

	/* construct none and unknown type. */
	firm_none_type = new_type(tpop_none, mode_BAD, NULL);
	set_type_size_bytes(firm_none_type, 0);
	set_type_state (firm_none_type, layout_fixed);
	remove_irp_type(firm_none_type);

	firm_code_type = new_type(tpop_code, mode_ANY, NULL);
	set_type_state(firm_code_type, layout_fixed);
	remove_irp_type(firm_code_type);

	firm_unknown_type = new_type(tpop_unknown, mode_ANY, NULL);
	set_type_size_bytes(firm_unknown_type, 0);
	set_type_state (firm_unknown_type, layout_fixed);
	remove_irp_type(firm_unknown_type);
}

void ir_finish_type(void)
{
	if (firm_none_type != NULL) {
		free_type(firm_none_type);
		firm_none_type = NULL;
	}
	if (firm_code_type != NULL) {
		free_type(firm_code_type);
		firm_code_type = NULL;
	}
	if (firm_unknown_type != NULL) {
		free_type(firm_unknown_type);
		firm_unknown_type = NULL;
	}
	value_params_suffix = NULL;
	value_ress_suffix = NULL;
}

/** the global type visited flag */
ir_visited_t firm_type_visited;

void (set_master_type_visited)(ir_visited_t val)
{
	_set_master_type_visited(val);
}

ir_visited_t (get_master_type_visited)(void)
{
	return _get_master_type_visited();
}

void (inc_master_type_visited)(void)
{
	_inc_master_type_visited();
}

ir_type *new_type(const tp_op *type_op, ir_mode *mode, type_dbg_info *db)
{
	ir_type *res;
	size_t node_size;

	node_size = offsetof(ir_type, attr) +  type_op->attr_size;
	res = (ir_type*)xmalloc(node_size);
	memset(res, 0, node_size);

	res->kind       = k_type;
	res->type_op    = type_op;
	res->mode       = mode;
	res->visibility = ir_visibility_external;
	res->flags      = tf_none;
	res->size       = 0;
	res->align      = 0;
	res->visit      = 0;
	res->link       = NULL;
	res->dbi        = db;
	res->assoc_type = NULL;
#ifdef DEBUG_libfirm
	res->nr         = get_irp_new_node_nr();
#endif /* defined DEBUG_libfirm */

	add_irp_type(res);   /* Remember the new type global. */

	return res;
}

void free_type(ir_type *tp)
{
	const tp_op *op = get_type_tpop(tp);

	if ((get_type_tpop(tp) == tpop_none) || (get_type_tpop(tp) == tpop_unknown)
			|| (get_type_tpop(tp) == tpop_code))
		return;
	/* Remove from list of all types */
	remove_irp_type(tp);
	/* Free the attributes of the type. */
	free_type_attrs(tp);
	/* Free entities automatically allocated with the ir_type */
	if (op->ops.free_auto_entities)
		op->ops.free_auto_entities(tp);
	/* And now the type itself... */
	tp->kind = k_BAD;
	free(tp);
}

void free_type_entities(ir_type *tp)
{
	const tp_op *tpop = get_type_tpop(tp);

	if (tpop->ops.free_entities)
		tpop->ops.free_entities(tp);
}

void free_type_attrs(ir_type *tp)
{
	const tp_op *tpop = get_type_tpop(tp);

	if (tpop->ops.free_attrs)
		tpop->ops.free_attrs(tp);
}

void *(get_type_link)(const ir_type *tp)
{
	return _get_type_link(tp);
}

void (set_type_link)(ir_type *tp, void *l)
{
	_set_type_link(tp, l);
}

const tp_op *(get_type_tpop)(const ir_type *tp)
{
	return _get_type_tpop(tp);
}

ident *(get_type_tpop_nameid)(const ir_type *tp)
{
	return _get_type_tpop_nameid(tp);
}

const char* get_type_tpop_name(const ir_type *tp)
{
	assert(tp && tp->kind == k_type);
	return get_id_str(tp->type_op->name);
}

tp_opcode (get_type_tpop_code)(const ir_type *tp)
{
	return _get_type_tpop_code(tp);
}

ir_mode *(get_type_mode)(const ir_type *tp)
{
	return _get_type_mode(tp);
}

void set_type_mode(ir_type *tp, ir_mode *mode)
{
	const tp_op *tpop = get_type_tpop(tp);

	if (tpop->ops.set_type_mode)
		tpop->ops.set_type_mode(tp, mode);
	else
		assert(0 && "setting a mode is NOT allowed for this type");
}

/* Outputs a unique number for this node */
long get_type_nr(const ir_type *tp)
{
	assert(tp);
#ifdef DEBUG_libfirm
	return tp->nr;
#else
	return (long)PTR_TO_INT(tp);
#endif
}

unsigned (get_type_size_bytes)(const ir_type *tp)
{
	return _get_type_size_bytes(tp);
}

ir_visibility get_type_visibility(const ir_type *tp)
{
	assert(is_type(tp));
	return tp->visibility;
}

void set_type_visibility(ir_type *tp, ir_visibility v)
{
	assert(is_type(tp));
	tp->visibility = v;
}

void set_type_size_bytes(ir_type *tp, unsigned size)
{
	const tp_op *tpop = get_type_tpop(tp);

	if (tpop->ops.set_type_size)
		tpop->ops.set_type_size(tp, size);
	else
		assert(0 && "Cannot set size for this type");
}

unsigned get_type_alignment_bytes(ir_type *tp)
{
	unsigned align = 1;

	if (tp->align > 0)
		return tp->align;

	/* alignment NOT set calculate it "on demand" */
	if (tp->mode)
		align = (get_mode_size_bits(tp->mode) + 7) >> 3;
	else if (is_Array_type(tp))
		align = get_type_alignment_bytes(get_array_element_type(tp));
	else if (is_compound_type(tp)) {
		size_t i, n = get_compound_n_members(tp);

		align = 0;
		for (i = 0; i < n; ++i) {
			ir_type  *t = get_entity_type(get_compound_member(tp, i));
			unsigned a  = get_type_alignment_bytes(t);

			if (a > align)
				align = a;
		}
	} else if (is_Method_type(tp)) {
		align = 0;
	}

	/* write back */
	tp->align = align;

	return align;
}

void set_type_alignment_bytes(ir_type *tp, unsigned align)
{
	assert(tp && tp->kind == k_type);
	/* Methods don't have an alignment. */
	if (tp->type_op != type_method) {
		tp->align = align;
	}
}

const char *get_type_state_name(ir_type_state s)
{
#define X(a)    case a: return #a;
	switch (s) {
		X(layout_undefined);
		X(layout_fixed);
	}
	return "<unknown>";
#undef X
}

ir_type_state (get_type_state)(const ir_type *tp)
{
	return _get_type_state(tp);
}

void set_type_state(ir_type *tp, ir_type_state state)
{
	assert(tp && tp->kind == k_type);

	if ((tp->type_op == type_pointer) || (tp->type_op == type_primitive) ||
		(tp->type_op == type_method))
		return;

	/* Just a correctness check: */
	if (state == layout_fixed) {
		size_t i;
		switch (get_type_tpop_code(tp)) {
		case tpo_class:
			if (tp != get_glob_type()) {
				size_t n_mem = get_class_n_members(tp);
				for (i = 0; i < n_mem; i++) {
					ir_entity *entity = get_class_member(tp, i);
					if (is_Method_type(get_entity_type(entity)))
						continue;
					assert(get_entity_offset(entity) > -1);
				}
			}
			break;
		case tpo_struct:
			for (i = 0; i < get_struct_n_members(tp); i++) {
				assert(get_entity_offset(get_struct_member(tp, i)) > -1);
			}
			break;
		case tpo_union:
			break;
		case tpo_array:
			break;
		case tpo_enumeration: {
#ifndef NDEBUG
			size_t n_enums = get_enumeration_n_enums(tp);
			assert(get_type_mode(tp) != NULL);
			for (i = 0; i < n_enums; ++i) {
				ir_enum_const *ec = get_enumeration_const(tp, i);
				ir_tarval     *tv = get_enumeration_value(ec);
				assert(tv != NULL && tv != tarval_bad);
			}
#endif
			break;
		}
		default: break;
		}
	}
	if (state == layout_fixed)
		tp->flags |= tf_layout_fixed;
	else
		tp->flags &= ~tf_layout_fixed;
}

ir_visited_t (get_type_visited)(const ir_type *tp)
{
	return _get_type_visited(tp);
}

void (set_type_visited)(ir_type *tp, ir_visited_t num)
{
	_set_type_visited(tp, num);
}

void (mark_type_visited)(ir_type *tp)
{
	_mark_type_visited(tp);
}

int (type_visited)(const ir_type *tp)
{
	return _type_visited(tp);
}

int (type_not_visited)(const ir_type *tp)
{
	return _type_not_visited(tp);
}

type_dbg_info *(get_type_dbg_info)(const ir_type *tp)
{
	return _get_type_dbg_info(tp);
}

void (set_type_dbg_info)(ir_type *tp, type_dbg_info *db)
{
	_set_type_dbg_info(tp, db);
}

int (is_type)(const void *thing)
{
	return _is_type(thing);
}

/* Checks whether two types are structural equal.*/
int equal_type(ir_type *typ1, ir_type *typ2)
{
	ir_entity **m;
	ir_type **t;
	size_t i;
	size_t j;

	if (typ1 == typ2) return 1;

	if ((get_type_tpop_code(typ1) != get_type_tpop_code(typ2)) ||
	    typ1->name != typ2->name ||
	    (get_type_mode(typ1) != get_type_mode(typ2)) ||
	    (get_type_state(typ1) != get_type_state(typ2)))
		return 0;
	if ((get_type_state(typ1) == layout_fixed) &&
		(get_type_size_bytes(typ1) != get_type_size_bytes(typ2)))
		return 0;

	switch (get_type_tpop_code(typ1)) {
	case tpo_class:
		if (get_class_n_members(typ1) != get_class_n_members(typ2)) return 0;
		if (get_class_n_subtypes(typ1) != get_class_n_subtypes(typ2)) return 0;
		if (get_class_n_supertypes(typ1) != get_class_n_supertypes(typ2)) return 0;
		if (get_class_peculiarity(typ1) != get_class_peculiarity(typ2)) return 0;
		/** Compare the members **/
		m = ALLOCANZ(ir_entity*, get_class_n_members(typ1));
		/* First sort the members of typ2 */
		for (i = 0; i < get_class_n_members(typ1); i++) {
			ir_entity *e1 = get_class_member(typ1, i);
			for (j = 0; j < get_class_n_members(typ2); j++) {
				ir_entity *e2 = get_class_member(typ2, j);
				if (get_entity_name(e1) == get_entity_name(e2))
					m[i] = e2;
			}
		}
		for (i = 0; i < get_class_n_members(typ1); i++) {
			if (!m[i] || get_class_member(typ1, i) != m[i])
				return 0;
		}
		/** Compare the supertypes **/
		t = ALLOCANZ(ir_type*, get_class_n_supertypes(typ1));
		/* First sort the supertypes of typ2 */
		for (i = 0; i < get_class_n_supertypes(typ1); i++) {
			ir_type *t1 = get_class_supertype(typ1, i);
			for (j = 0; j < get_class_n_supertypes(typ2); j++) {
				ir_type *t2 = get_class_supertype(typ2, j);
				if (t2->name == t1->name)
					t[i] = t2;
			}
		}
		for (i = 0; i < get_class_n_supertypes(typ1); i++) {
			if (!t[i]  ||  /* Found no counterpart */
				get_class_supertype(typ1, i) != t[i])
				return 0;
		}
		break;

	case tpo_struct:
		if (get_struct_n_members(typ1) != get_struct_n_members(typ2)) return 0;
		m = ALLOCANZ(ir_entity*, get_struct_n_members(typ1));
		/* First sort the members of lt */
		for (i = 0; i < get_struct_n_members(typ1); i++) {
			ir_entity *e1 = get_struct_member(typ1, i);
			for (j = 0; j < get_struct_n_members(typ2); j++) {
				ir_entity *e2 = get_struct_member(typ2, j);
				if (get_entity_name(e1) == get_entity_name(e2))
					m[i] = e2;
			}
		}
		for (i = 0; i < get_struct_n_members(typ1); i++) {
			if (!m[i] || get_struct_member(typ1, i) != m[i])
				return 0;
		}
		break;

	case tpo_method: {
		size_t n_param1;
		size_t n_param2;

		if (get_method_variadicity(typ1) != get_method_variadicity(typ2)) return 0;
		if (get_method_n_ress(typ1)      != get_method_n_ress(typ2)) return 0;
		if (get_method_calling_convention(typ1) !=
		    get_method_calling_convention(typ2)) return 0;

		if (get_method_variadicity(typ1) == variadicity_non_variadic) {
			n_param1 = get_method_n_params(typ1);
			n_param2 = get_method_n_params(typ2);
		} else {
			n_param1 = get_method_first_variadic_param_index(typ1);
			n_param2 = get_method_first_variadic_param_index(typ2);
		}

		if (n_param1 != n_param2) return 0;

		for (i = 0; i < n_param1; i++) {
			if (!equal_type(get_method_param_type(typ1, i), get_method_param_type(typ2, i)))
				return 0;
		}
		for (i = 0; i < get_method_n_ress(typ1); i++) {
			if (!equal_type(get_method_res_type(typ1, i), get_method_res_type(typ2, i)))
				return 0;
		}
	} break;

	case tpo_union:
		if (get_union_n_members(typ1) != get_union_n_members(typ2)) return 0;
		m = ALLOCANZ(ir_entity*, get_union_n_members(typ1));
		/* First sort the members of lt */
		for (i = 0; i < get_union_n_members(typ1); i++) {
			ir_entity *e1 = get_union_member(typ1, i);
			for (j = 0; j < get_union_n_members(typ2); j++) {
				ir_entity *e2 = get_union_member(typ2, j);
				if (get_entity_name(e1) == get_entity_name(e2))
					m[i] = e2;
			}
		}
		for (i = 0; i < get_union_n_members(typ1); i++) {
			if (!m[i] || get_union_member(typ1, i) != m[i])
				return 0;
		}
		break;

	case tpo_array:
		if (get_array_n_dimensions(typ1) != get_array_n_dimensions(typ2))
			return 0;
		if (!equal_type(get_array_element_type(typ1), get_array_element_type(typ2)))
			return 0;
		for (i = 0; i < get_array_n_dimensions(typ1); i++) {
			if (get_array_lower_bound(typ1, i) != get_array_lower_bound(typ2, i) ||
				get_array_upper_bound(typ1, i) != get_array_upper_bound(typ2, i))
				return 0;
			if (get_array_order(typ1, i) != get_array_order(typ2, i))
				assert(0 && "type compare with different dimension orders not implemented");
		}
		break;

	case tpo_enumeration:
		assert(0 && "enumerations not implemented");
		break;

	case tpo_pointer:
		if (get_pointer_points_to_type(typ1) != get_pointer_points_to_type(typ2))
			return 0;
		break;

	case tpo_primitive:
		break;

	default: break;
	}
	return 1;
}

int smaller_type(ir_type *st, ir_type *lt)
{
	ir_entity **m;
	size_t i;
	size_t j;
	size_t n_st_members;

	if (st == lt) return 1;

	if (get_type_tpop_code(st) != get_type_tpop_code(lt))
		return 0;

	switch (get_type_tpop_code(st)) {
	case tpo_class:
		return is_SubClass_of(st, lt);

	case tpo_struct:
		n_st_members = get_struct_n_members(st);
		if (n_st_members != get_struct_n_members(lt))
			return 0;

		m = ALLOCANZ(ir_entity*, n_st_members);
		/* First sort the members of lt */
		for (i = 0; i < n_st_members; ++i) {
			ir_entity *se = get_struct_member(st, i);
			size_t n = get_struct_n_members(lt);
			for (j = 0; j < n; ++j) {
				ir_entity *le = get_struct_member(lt, j);
				if (get_entity_name(le) == get_entity_name(se))
					m[i] = le;
			}
		}
		for (i = 0; i < n_st_members; i++) {
			if (!m[i]  ||  /* Found no counterpart */
			    !smaller_type(get_entity_type(get_struct_member(st, i)), get_entity_type(m[i])))
				return 0;
		}
		break;

	case tpo_method: {
		size_t n_param1, n_param2;

		/** FIXME: is this still 1? */
		if (get_method_variadicity(st) != get_method_variadicity(lt)) return 0;
		if (get_method_n_ress(st) != get_method_n_ress(lt)) return 0;
		if (get_method_calling_convention(st) !=
		    get_method_calling_convention(lt)) return 0;

		if (get_method_variadicity(st) == variadicity_non_variadic) {
			n_param1 = get_method_n_params(st);
			n_param2 = get_method_n_params(lt);
		} else {
			n_param1 = get_method_first_variadic_param_index(st);
			n_param2 = get_method_first_variadic_param_index(lt);
		}

		if (n_param1 != n_param2) return 0;

		for (i = 0; i < get_method_n_params(st); i++) {
			if (!smaller_type(get_method_param_type(st, i), get_method_param_type(lt, i)))
				return 0;
		}
		for (i = 0; i < get_method_n_ress(st); i++) {
			if (!smaller_type(get_method_res_type(st, i), get_method_res_type(lt, i)))
				return 0;
		}
	} break;

	case tpo_union:
		n_st_members = get_union_n_members(st);
		if (n_st_members != get_union_n_members(lt)) return 0;
		m = ALLOCANZ(ir_entity*, n_st_members);
		/* First sort the members of lt */
		for (i = 0; i < n_st_members; ++i) {
			ir_entity *se = get_union_member(st, i);
			size_t n = get_union_n_members(lt);
			for (j = 0; j < n; ++j) {
				ir_entity *le = get_union_member(lt, j);
				if (get_entity_name(le) == get_entity_name(se))
					m[i] = le;
			}
		}
		for (i = 0; i < n_st_members; ++i) {
			if (!m[i]  ||  /* Found no counterpart */
				!smaller_type(get_entity_type(get_union_member(st, i)), get_entity_type(m[i])))
				return 0;
		}
		break;

	case tpo_array: {
		ir_type *set, *let;  /* small/large elt. ir_type */
		if (get_array_n_dimensions(st) != get_array_n_dimensions(lt))
			return 0;
		set = get_array_element_type(st);
		let = get_array_element_type(lt);
		if (set != let) {
			/* If the element types are different, set must be convertible
			   to let, and they must have the same size so that address
			   computations work out.  To have a size the layout must
			   be fixed. */
			if ((get_type_state(set) != layout_fixed) ||
			    (get_type_state(let) != layout_fixed))
				return 0;
			if (!smaller_type(set, let) ||
			    get_type_size_bytes(set) != get_type_size_bytes(let))
				return 0;
		}
		for (i = 0; i < get_array_n_dimensions(st); i++) {
			if (get_array_lower_bound(lt, i))
				if (get_array_lower_bound(st, i) != get_array_lower_bound(lt, i))
					return 0;
				if (get_array_upper_bound(lt, i))
					if (get_array_upper_bound(st, i) != get_array_upper_bound(lt, i))
						return 0;
		}
	} break;

	case tpo_enumeration:
		assert(0 && "enumerations not implemented");
		break;

	case tpo_pointer:
		if (!smaller_type(get_pointer_points_to_type(st), get_pointer_points_to_type(lt)))
			return 0;
		break;

	case tpo_primitive:
		if (!smaller_mode(get_type_mode(st), get_type_mode(lt)))
			return 0;
		break;

	default: break;
	}
	return 1;
}


ir_type *new_d_type_class(ident *name, type_dbg_info *db)
{
	ir_type *res;

	res = new_type(type_class, NULL, db);
	res->name = name;

	res->attr.ca.members     = NEW_ARR_F (ir_entity *, 0);
	res->attr.ca.subtypes    = NEW_ARR_F (ir_type *, 0);
	res->attr.ca.supertypes  = NEW_ARR_F (ir_type *, 0);
	res->attr.ca.peculiarity = peculiarity_existent;
	res->attr.ca.type_info   = NULL;
	res->attr.ca.vtable_size = 0;
	res->attr.ca.clss_flags  = cf_none;
	res->attr.ca.dfn         = 0;
	hook_new_type(res);
	return res;
}

ir_type *new_type_class(ident *name)
{
	return new_d_type_class(name, NULL);
}

void free_class_entities(ir_type *clss)
{
	size_t i;
	assert(clss && (clss->type_op == type_class));
	/* we must iterate backward here */
	for (i = get_class_n_members(clss); i > 0;)
		free_entity(get_class_member(clss, --i));
	/* do NOT free the type info here. It belongs to another class */
}

void free_class_attrs(ir_type *clss)
{
	assert(clss && (clss->type_op == type_class));
	DEL_ARR_F(clss->attr.ca.members);
	DEL_ARR_F(clss->attr.ca.subtypes);
	DEL_ARR_F(clss->attr.ca.supertypes);
}

ident *get_class_ident(const ir_type *clss)
{
	assert(clss->type_op == type_class);
	return clss->name;
}

const char *get_class_name(const ir_type *clss)
{
	if (get_class_ident(clss) == NULL)
		return NULL;
	return get_id_str(get_class_ident(clss));
}

static void add_class_member(ir_type *clss, ir_entity *member)
{
	assert(clss && (clss->type_op == type_class));
	assert(clss != get_entity_type(member) && "recursive type");
	ARR_APP1 (ir_entity *, clss->attr.ca.members, member);
}

size_t (get_class_n_members)(const ir_type *clss)
{
	return _get_class_n_members(clss);
}

size_t get_class_member_index(const ir_type *clss, ir_entity *mem)
{
	size_t i, n;
	assert(clss && (clss->type_op == type_class));
	for (i = 0, n = get_class_n_members(clss); i < n; ++i) {
		if (get_class_member(clss, i) == mem)
			return i;
	}
	return INVALID_MEMBER_INDEX;
}

ir_entity *(get_class_member)(const ir_type *clss, size_t pos)
{
	return _get_class_member(clss, pos);
}

ir_entity *get_class_member_by_name(ir_type *clss, ident *name)
{
	size_t i, n_mem;
	assert(clss && (clss->type_op == type_class));
	n_mem = get_class_n_members(clss);
	for (i = 0; i < n_mem; ++i) {
		ir_entity *mem = get_class_member(clss, i);
		if (get_entity_ident(mem) == name)
			return mem;
	}
	return NULL;
}

static void remove_class_member(ir_type *clss, ir_entity *member)
{
	size_t i;
	assert(clss && (clss->type_op == type_class));
	for (i = 0; i < ARR_LEN(clss->attr.ca.members); ++i) {
		if (clss->attr.ca.members[i] == member) {
			for (; i < ARR_LEN(clss->attr.ca.members) - 1; ++i)
				clss->attr.ca.members[i] = clss->attr.ca.members[i + 1];
			ARR_SETLEN(ir_entity*, clss->attr.ca.members, ARR_LEN(clss->attr.ca.members) - 1);
			break;
		}
	}
}

void add_class_subtype(ir_type *clss, ir_type *subtype)
{
	size_t i;
	assert(clss->type_op == type_class);
	ARR_APP1 (ir_type *, clss->attr.ca.subtypes, subtype);
	for (i = 0; i < get_class_n_supertypes(subtype); i++) {
		if (get_class_supertype(subtype, i) == clss)
			/* Class already registered */
			return;
	}
	ARR_APP1(ir_type *, subtype->attr.ca.supertypes, clss);
}

size_t get_class_n_subtypes(const ir_type *clss)
{
	assert(clss->type_op == type_class);
	return ARR_LEN (clss->attr.ca.subtypes);
}

ir_type *get_class_subtype(ir_type *clss, size_t pos)
{
	assert(clss->type_op == type_class);
	assert(pos < get_class_n_subtypes(clss));
	return clss->attr.ca.subtypes[pos];
}

size_t get_class_subtype_index(ir_type *clss, const ir_type *subclass)
{
	size_t i, n_subtypes = get_class_n_subtypes(clss);
	assert(is_Class_type(subclass));
	for (i = 0; i < n_subtypes; ++i) {
		if (get_class_subtype(clss, i) == subclass)
			return i;
	}
	return (size_t)-1;
}

void set_class_subtype(ir_type *clss, ir_type *subtype, size_t pos)
{
	assert(clss->type_op == type_class);
	assert(pos < get_class_n_subtypes(clss));
	clss->attr.ca.subtypes[pos] = subtype;
}

void remove_class_subtype(ir_type *clss, ir_type *subtype)
{
	size_t i;
	assert(clss && (clss->type_op == type_class));
	for (i = 0; i < ARR_LEN(clss->attr.ca.subtypes); ++i) {
		if (clss->attr.ca.subtypes[i] == subtype) {
			for (; i < ARR_LEN(clss->attr.ca.subtypes) - 1; ++i)
				clss->attr.ca.subtypes[i] = clss->attr.ca.subtypes[i+1];
			ARR_SETLEN(ir_type*, clss->attr.ca.subtypes, ARR_LEN(clss->attr.ca.subtypes) - 1);
			break;
		}
	}
}

void add_class_supertype(ir_type *clss, ir_type *supertype)
{
	size_t i;
	size_t n;
	assert(clss && (clss->type_op == type_class));
	assert(supertype && (supertype -> type_op == type_class));
	ARR_APP1 (ir_type *, clss->attr.ca.supertypes, supertype);
	for (i = 0, n = get_class_n_subtypes(supertype); i < n; ++i) {
		if (get_class_subtype(supertype, i) == clss)
			/* Class already registered */
			return;
	}
	ARR_APP1(ir_type *, supertype->attr.ca.subtypes, clss);
}

size_t get_class_n_supertypes(const ir_type *clss)
{
	assert(clss->type_op == type_class);
	return ARR_LEN(clss->attr.ca.supertypes);
}

size_t get_class_supertype_index(ir_type *clss, ir_type *super_clss)
{
	size_t i, n_supertypes = get_class_n_supertypes(clss);
	assert(super_clss && (super_clss->type_op == type_class));
	for (i = 0; i < n_supertypes; i++) {
		if (get_class_supertype(clss, i) == super_clss)
			return i;
	}
	return (size_t)-1;
}

ir_type *get_class_supertype(ir_type *clss, size_t pos)
{
	assert(clss->type_op == type_class);
	assert(pos < get_class_n_supertypes(clss));
	return clss->attr.ca.supertypes[pos];
}

void set_class_supertype(ir_type *clss, ir_type *supertype, size_t pos)
{
	assert(clss->type_op == type_class);
	assert(pos < get_class_n_supertypes(clss));
	clss->attr.ca.supertypes[pos] = supertype;
}

void remove_class_supertype(ir_type *clss, ir_type *supertype)
{
	size_t i;
	assert(clss && (clss->type_op == type_class));
	for (i = 0; i < ARR_LEN(clss->attr.ca.supertypes); ++i) {
		if (clss->attr.ca.supertypes[i] == supertype) {
			for (; i < ARR_LEN(clss->attr.ca.supertypes) - 1; ++i)
				clss->attr.ca.supertypes[i] = clss->attr.ca.supertypes[i+1];
			ARR_SETLEN(ir_type*, clss->attr.ca.supertypes, ARR_LEN(clss->attr.ca.supertypes) - 1);
			break;
		}
	}
}

ir_entity *get_class_type_info(const ir_type *clss)
{
	return clss->attr.ca.type_info;
}

void set_class_type_info(ir_type *clss, ir_entity *ent)
{
	clss->attr.ca.type_info = ent;
	if (ent)
		ent->repr_class = clss;
}

ir_peculiarity get_class_peculiarity(const ir_type *clss)
{
	assert(clss && (clss->type_op == type_class));
	return clss->attr.ca.peculiarity;
}

void set_class_peculiarity(ir_type *clss, ir_peculiarity pec)
{
	assert(clss && (clss->type_op == type_class));
	assert(pec != peculiarity_inherited);  /* There is no inheritance of types in libFirm. */
	clss->attr.ca.peculiarity = pec;
}

unsigned (get_class_vtable_size)(const ir_type *clss)
{
	return _get_class_vtable_size(clss);
}

void (set_class_vtable_size)(ir_type *clss, unsigned size)
{
	_set_class_vtable_size(clss, size);
}

int (is_class_final)(const ir_type *clss)
{
	return _is_class_final(clss);
}

void (set_class_final)(ir_type *clss, int flag)
{
	_set_class_final(clss, flag);
}

int (is_class_interface)(const ir_type *clss)
{
	return _is_class_interface(clss);
}

void (set_class_interface)(ir_type *clss, int flag)
{
	_set_class_interface(clss, flag);
}

int (is_class_abstract)(const ir_type *clss)
{
	 return _is_class_abstract(clss);
}

void (set_class_abstract)(ir_type *clss, int final)
{
	_set_class_abstract(clss, final);
}

void set_class_dfn(ir_type *clss, int dfn)
{
	clss->attr.ca.dfn = dfn;
}

int get_class_dfn(const ir_type *clss)
{
	return (clss->attr.ca.dfn);
}

int (is_Class_type)(const ir_type *clss)
{
	return _is_class_type(clss);
}

void set_class_mode(ir_type *tp, ir_mode *mode)
{
	/* for classes and structs we allow to set a mode if the layout is fixed AND the size matches */
	assert(get_type_state(tp) == layout_fixed &&
	       tp->size == get_mode_size_bytes(mode) && "mode don't match class layout");
	tp->mode = mode;
}

void set_class_size(ir_type *tp, unsigned size)
{
	tp->size = size;
}


ir_type *new_d_type_struct(ident *name, type_dbg_info *db)
{
	ir_type *res = new_type(type_struct, NULL, db);
	res->name = name;

	res->attr.sa.members = NEW_ARR_F(ir_entity *, 0);
	hook_new_type(res);
	return res;
}

ir_type *new_type_struct(ident *name)
{
	return new_d_type_struct (name, NULL);
}

void free_struct_entities(ir_type *strct)
{
	size_t i;
	assert(strct && (strct->type_op == type_struct));
	/* we must iterate backward here */
	for (i = get_struct_n_members(strct); i > 0;)
		free_entity(get_struct_member(strct, --i));
}

void free_struct_attrs(ir_type *strct)
{
	assert(strct && (strct->type_op == type_struct));
	DEL_ARR_F(strct->attr.sa.members);
}

ident *get_struct_ident(const ir_type *strct)
{
	assert(strct->type_op == type_struct);
	return strct->name;
}

const char *get_struct_name(const ir_type *strct)
{
	if (get_struct_ident(strct) == NULL)
		return NULL;
	return get_id_str(get_struct_ident(strct));
}

size_t get_struct_n_members(const ir_type *strct)
{
	assert(strct->type_op == type_struct);
	return ARR_LEN(strct->attr.sa.members);
}

static void add_struct_member(ir_type *strct, ir_entity *member)
{
	assert(strct && (strct->type_op == type_struct));
	assert(get_type_tpop(get_entity_type(member)) != type_method);
	assert(strct != get_entity_type(member) && "recursive type");
	ARR_APP1 (ir_entity *, strct->attr.sa.members, member);
}

ir_entity *get_struct_member(const ir_type *strct, size_t pos)
{
	assert(strct && (strct->type_op == type_struct));
	assert(pos < get_struct_n_members(strct));
	return strct->attr.sa.members[pos];
}

size_t get_struct_member_index(const ir_type *strct, ir_entity *mem)
{
	size_t i, n;
	assert(strct && (strct->type_op == type_struct));
	for (i = 0, n = get_struct_n_members(strct); i < n; ++i) {
		if (get_struct_member(strct, i) == mem)
			return i;
	}
	return (size_t)-1;
}

static void remove_struct_member(ir_type *strct, ir_entity *member)
{
	size_t i;
	assert(strct && (strct->type_op == type_struct));
	for (i = 0; i < ARR_LEN(strct->attr.sa.members); ++i) {
		if (strct->attr.sa.members[i] == member) {
			for (; i < ARR_LEN(strct->attr.sa.members) - 1; ++i)
				strct->attr.sa.members[i] = strct->attr.sa.members[i+1];
			ARR_SETLEN(ir_entity*, strct->attr.sa.members, ARR_LEN(strct->attr.sa.members) - 1);
			break;
		}
	}
}

int (is_Struct_type)(const ir_type *strct)
{
	return _is_struct_type(strct);
}

void set_struct_mode(ir_type *tp, ir_mode *mode)
{
	/* for classes and structs we allow to set a mode if the layout is fixed AND the size matches */
	assert(get_type_state(tp) == layout_fixed &&
	       tp->size == get_mode_size_bytes(mode) && "mode don't match struct layout");
	tp->mode = mode;
}

void set_struct_size(ir_type *tp, unsigned size)
{
	tp->size = size;
}


/**
 * Lazy construction of value argument / result representation.
 * Constructs a struct type and its member.  The types of the members
 * are passed in the argument list.
 *
 * @param name    name of the type constructed
 * @param len     number of fields
 * @param tps     array of field types with length len
 */
static ir_type *build_value_type(char const* name, size_t len, tp_ent_pair *tps)
{
	size_t i;
	ir_type *res = new_type_struct(new_id_from_str(name));
	res->flags |= tf_value_param_type;
	/* Remove type from type list.  Must be treated differently than other types. */
	remove_irp_type(res);
	for (i = 0; i < len; ++i) {
		ident *id = tps[i].param_name;

		/* use res as default if corresponding type is not yet set. */
		ir_type *elt_type = tps[i].tp ? tps[i].tp : res;

		/* use the parameter name if specified */
		if (id == NULL) {
			id = new_id_from_str("elt");
		}
		tps[i].ent = new_entity(res, id, elt_type);
		set_entity_allocation(tps[i].ent, allocation_parameter);
	}
	return res;
}

ir_type *new_d_type_method(size_t n_param, size_t n_res, type_dbg_info *db)
{
	ir_type *res;

	assert((get_mode_size_bits(mode_P_code) % 8 == 0) && "unorthodox modes not implemented");
	res = new_type(type_method, mode_P_code, db);
	res->flags                       |= tf_layout_fixed;
	res->size                         = get_mode_size_bytes(mode_P_code);
	res->attr.ma.n_params             = n_param;
	res->attr.ma.params               = XMALLOCNZ(tp_ent_pair, n_param);
	res->attr.ma.value_params         = NULL;
	res->attr.ma.n_res                = n_res;
	res->attr.ma.res_type             = XMALLOCNZ(tp_ent_pair, n_res);
	res->attr.ma.value_ress           = NULL;
	res->attr.ma.variadicity          = variadicity_non_variadic;
	res->attr.ma.first_variadic_param = -1;
	res->attr.ma.additional_properties = mtp_no_property;
	hook_new_type(res);
	return res;
}

ir_type *new_type_method(size_t n_param, size_t n_res)
{
	return new_d_type_method(n_param, n_res, NULL);
}

ir_type *clone_type_method(ir_type *tp)
{
	ir_type  *res;
	ir_mode  *mode;
	size_t    n_params;
	size_t    n_res;
	type_dbg_info *db;

	assert(is_Method_type(tp));

	mode     = tp->mode;
	n_params = tp->attr.ma.n_params;
	n_res    = tp->attr.ma.n_res;
	db       = tp->dbi;

	res = new_type(type_method, mode, db);

	res->flags                         = tp->flags;
	res->assoc_type                    = tp->assoc_type;
	res->size                          = tp->size;
	res->attr.ma.n_params              = n_params;
	res->attr.ma.params                = XMALLOCN(tp_ent_pair, n_params);
	memcpy(res->attr.ma.params, tp->attr.ma.params, n_params * sizeof(res->attr.ma.params[0]));
	res->attr.ma.value_params          = tp->attr.ma.value_params;
	res->attr.ma.n_res                 = n_res;
	res->attr.ma.res_type              = XMALLOCN(tp_ent_pair, n_res);
	memcpy(res->attr.ma.res_type, tp->attr.ma.res_type, n_res * sizeof(res->attr.ma.res_type[0]));
	res->attr.ma.value_ress            = tp->attr.ma.value_ress;
	res->attr.ma.variadicity           = tp->attr.ma.variadicity;
	res->attr.ma.first_variadic_param  = tp->attr.ma.first_variadic_param;
	res->attr.ma.additional_properties = tp->attr.ma.additional_properties;
	res->attr.ma.irg_calling_conv      = tp->attr.ma.irg_calling_conv;
	hook_new_type(res);
	return res;
}

void free_method_entities(ir_type *method)
{
	(void) method;
	assert(method && (method->type_op == type_method));
}

void free_method_attrs(ir_type *method)
{
	assert(method && (method->type_op == type_method));
	free(method->attr.ma.params);
	free(method->attr.ma.res_type);
	/* cannot free it yet, type could be cloned ...
	if (method->attr.ma.value_params) {
		free_type_entities(method->attr.ma.value_params);
		free_type(method->attr.ma.value_params);
	}
	*/
	if (method->attr.ma.value_ress) {
		free_type_entities(method->attr.ma.value_ress);
		free_type(method->attr.ma.value_ress);
	}
}

size_t (get_method_n_params)(const ir_type *method)
{
	return _get_method_n_params(method);
}

ir_type *get_method_param_type(ir_type *method, size_t pos)
{
	ir_type *res;
	assert(method->type_op == type_method);
	assert(pos < get_method_n_params(method));
	res = method->attr.ma.params[pos].tp;
	assert(res != NULL && "empty method param type");
	return res;
}

void set_method_param_type(ir_type *method, size_t pos, ir_type *tp)
{
	assert(method->type_op == type_method);
	assert(pos < get_method_n_params(method));
	method->attr.ma.params[pos].tp = tp;
	/* If information constructed set pass-by-value representation. */
	if (method->attr.ma.value_params) {
		assert(get_method_n_params(method) == get_struct_n_members(method->attr.ma.value_params));
		set_entity_type(get_struct_member(method->attr.ma.value_params, pos), tp);
	}
}

ident *get_method_param_ident(ir_type *method, size_t pos)
{
	assert(method->type_op == type_method);
	assert(pos < get_method_n_params(method));
	return method->attr.ma.params[pos].param_name;
}

const char *get_method_param_name(ir_type *method, size_t pos)
{
	ident *id = get_method_param_ident(method, pos);
	return id ? get_id_str(id) : NULL;
}

void set_method_param_ident(ir_type *method, size_t pos, ident *id)
{
	assert(method->type_op == type_method);
	assert(pos < get_method_n_params(method));
	method->attr.ma.params[pos].param_name = id;
}

ir_entity *get_method_value_param_ent(ir_type *method, size_t pos)
{
	assert(method && (method->type_op == type_method));
	assert(pos < get_method_n_params(method));

	if (!method->attr.ma.value_params) {
		/* parameter value type not created yet, build */
		method->attr.ma.value_params = build_value_type("<value param>",
			get_method_n_params(method), method->attr.ma.params);
	}
	/*
	 * build_value_type() sets the method->attr.ma.value_params type as default if
	 * no type is set!
	 */
	assert((get_entity_type(method->attr.ma.params[pos].ent) != method->attr.ma.value_params)
	       && "param type not yet set");
	return method->attr.ma.params[pos].ent;
}

void set_method_value_param_type(ir_type *method, ir_type *tp)
{
	size_t i;
	size_t n;

	assert(method && (method->type_op == type_method));
	assert(is_value_param_type(tp));
	assert(get_method_n_params(method) == get_struct_n_members(tp));

	method->attr.ma.value_params = tp;

	n = get_struct_n_members(tp);
	for (i = 0; i < n; i++) {
		ir_entity *ent = get_struct_member(tp, i);
		method->attr.ma.params[i].ent = ent;
	}
}

ir_type *get_method_value_param_type(const ir_type *method)
{
	assert(method && (method->type_op == type_method));
	return method->attr.ma.value_params;
}

size_t (get_method_n_ress)(const ir_type *method)
{
	return _get_method_n_ress(method);
}

ir_type *get_method_res_type(ir_type *method, size_t pos)
{
	ir_type *res;
	assert(method->type_op == type_method);
	assert(pos < get_method_n_ress(method));
	res = method->attr.ma.res_type[pos].tp;
	assert(res != NULL && "empty method return type");
	return res;
}

void set_method_res_type(ir_type *method, size_t pos, ir_type *tp)
{
	assert(method->type_op == type_method);
	assert(pos < get_method_n_ress(method));
	/* set the result ir_type */
	method->attr.ma.res_type[pos].tp = tp;
	/* If information constructed set pass-by-value representation. */
	if (method->attr.ma.value_ress) {
		assert(get_method_n_ress(method) == get_struct_n_members(method->attr.ma.value_ress));
		set_entity_type(get_struct_member(method->attr.ma.value_ress, pos), tp);
	}
}

ir_entity *get_method_value_res_ent(ir_type *method, size_t pos)
{
	assert(method->type_op == type_method);
	assert(pos < get_method_n_ress(method));

	if (!method->attr.ma.value_ress) {
		/* result value type not created yet, build */
		method->attr.ma.value_ress = build_value_type("<value result>",
			get_method_n_ress(method), method->attr.ma.res_type);
	}
	/*
	 * build_value_type() sets the method->attr.ma.value_ress type as default if
	 * no type is set!
	 */
	assert((get_entity_type(method->attr.ma.res_type[pos].ent) != method->attr.ma.value_ress)
	       && "result type not yet set");

	return method->attr.ma.res_type[pos].ent;
}

ir_type *get_method_value_res_type(const ir_type *method)
{
	assert(method->type_op == type_method);
	return method->attr.ma.value_ress;
}

const char *get_variadicity_name(ir_variadicity vari)
{
#define X(a)    case a: return #a
	switch (vari) {
	X(variadicity_non_variadic);
	X(variadicity_variadic);
	default:
		return "BAD VALUE";
	}
#undef X
}

ir_variadicity get_method_variadicity(const ir_type *method)
{
	assert(method && (method->type_op == type_method));
	return method->attr.ma.variadicity;
}

void set_method_variadicity(ir_type *method, ir_variadicity vari)
{
	assert(method && (method->type_op == type_method));
	method->attr.ma.variadicity = vari;
}

size_t get_method_first_variadic_param_index(const ir_type *method)
{
	assert(method->type_op == type_method);

	if (method->attr.ma.variadicity == variadicity_non_variadic)
		return (size_t)-1;

	if (method->attr.ma.first_variadic_param == (size_t)-1)
		return get_method_n_params(method);
	return method->attr.ma.first_variadic_param;
}

void set_method_first_variadic_param_index(ir_type *method, size_t index)
{
	assert(method->type_op == type_method);
	assert(index <= get_method_n_params(method));

	method->attr.ma.first_variadic_param = index;
}

mtp_additional_properties (get_method_additional_properties)(const ir_type *method)
{
	return _get_method_additional_properties(method);
}

void (set_method_additional_properties)(ir_type *method, mtp_additional_properties mask)
{
	_set_method_additional_properties(method, mask);
}

void (add_method_additional_properties)(ir_type *method,
                                        mtp_additional_properties flag)
{
	_add_method_additional_properties(method, flag);
}

unsigned (get_method_calling_convention)(const ir_type *method)
{
	return _get_method_calling_convention(method);
}

void (set_method_calling_convention)(ir_type *method, unsigned cc_mask)
{
	_set_method_calling_convention(method, cc_mask);
}

unsigned get_method_n_regparams(ir_type *method)
{
	unsigned cc = get_method_calling_convention(method);
	assert(IS_FASTCALL(cc));

	return cc & ~cc_bits;
}

void set_method_n_regparams(ir_type *method, unsigned n_regs)
{
	unsigned cc = get_method_calling_convention(method);
	assert(IS_FASTCALL(cc));

	set_method_calling_convention(method, (cc & cc_bits) | (n_regs & ~cc_bits));
}

int (is_Method_type)(const ir_type *method)
{
	return _is_method_type(method);
}


ir_type *new_d_type_union(ident *name, type_dbg_info *db)
{
	ir_type *res = new_type(type_union, NULL, db);
	res->name = name;

	res->attr.ua.members = NEW_ARR_F(ir_entity *, 0);
	hook_new_type(res);
	return res;
}

ir_type *new_type_union(ident *name)
{
	return new_d_type_union(name, NULL);
}

void free_union_entities(ir_type *uni)
{
	size_t i;
	assert(uni && (uni->type_op == type_union));
	/* we must iterate backward here */
	for (i = get_union_n_members(uni); i > 0;)
		free_entity(get_union_member(uni, --i));
}

void free_union_attrs(ir_type *uni)
{
	assert(uni && (uni->type_op == type_union));
	DEL_ARR_F(uni->attr.ua.members);
}

ident *get_union_ident(const ir_type *uni)
{
	assert(uni->type_op == type_union);
	return uni->name;
}

const char *get_union_name(const ir_type *uni)
{
	if (get_union_ident(uni) == NULL)
		return NULL;
	return get_id_str(get_union_ident(uni));
}

size_t get_union_n_members(const ir_type *uni)
{
	assert(uni->type_op == type_union);
	return ARR_LEN(uni->attr.ua.members);
}

static void add_union_member(ir_type *uni, ir_entity *member)
{
	assert(uni->type_op == type_union);
	assert(uni != get_entity_type(member) && "recursive type");
	ARR_APP1(ir_entity *, uni->attr.ua.members, member);
}

ir_entity *get_union_member(const ir_type *uni, size_t pos)
{
	assert(uni->type_op == type_union);
	assert(pos < get_union_n_members(uni));
	return uni->attr.ua.members[pos];
}

size_t get_union_member_index(const ir_type *uni, ir_entity *mem)
{
	size_t i, n;
	assert(uni && (uni->type_op == type_union));
	for (i = 0, n = get_union_n_members(uni); i < n; ++i) {
		if (get_union_member(uni, i) == mem)
			return i;
	}
	return (size_t)-1;
}

static void remove_union_member(ir_type *uni, ir_entity *member)
{
	size_t i;
	assert(uni && (uni->type_op == type_union));
	for (i = 0; i < ARR_LEN(uni->attr.ua.members); ++i) {
		if (uni->attr.ua.members[i] == member) {
			for (; i < ARR_LEN(uni->attr.ua.members) - 1; i++)
				uni->attr.ua.members[i] = uni->attr.ua.members[i+1];
			ARR_SETLEN(ir_entity*, uni->attr.ua.members, ARR_LEN(uni->attr.ua.members) - 1);
			break;
		}
	}
}

int (is_Union_type)(const ir_type *uni)
{
	return _is_union_type(uni);
}

void set_union_size(ir_type *tp, unsigned size)
{
	tp->size = size;
}



ir_type *new_d_type_array(int n_dimensions, ir_type *element_type,
                          type_dbg_info *db)
{
	ir_type *res;
	int i;
	ir_node *unk;
	ir_graph *irg = get_const_code_irg();

	assert(!is_Method_type(element_type));

	res = new_type(type_array, NULL, db);
	res->attr.aa.n_dimensions = n_dimensions;
	res->attr.aa.lower_bound  = XMALLOCNZ(ir_node*, n_dimensions);
	res->attr.aa.upper_bound  = XMALLOCNZ(ir_node*, n_dimensions);
	res->attr.aa.order        = XMALLOCNZ(int,      n_dimensions);

	unk = new_r_Unknown(irg, mode_Iu);
	for (i = 0; i < n_dimensions; i++) {
		res->attr.aa.lower_bound[i] =
		res->attr.aa.upper_bound[i] = unk;
		res->attr.aa.order[i]       = i;
	}

	res->attr.aa.element_type = element_type;
	res->attr.aa.element_ent
		= new_entity(NULL, new_id_from_chars("elem_ent", 8), element_type);
	res->attr.aa.element_ent->owner = res;

	hook_new_type(res);
	return res;
}

ir_type *new_type_array(int n_dimensions, ir_type *element_type)
{
	return new_d_type_array(n_dimensions, element_type, NULL);
}

void free_array_automatic_entities(ir_type *array)
{
	assert(array && (array->type_op == type_array));
	free_entity(get_array_element_entity(array));
}

void free_array_entities(ir_type *array)
{
	(void) array;
	assert(array->type_op == type_array);
}

void free_array_attrs(ir_type *array)
{
	assert(array->type_op == type_array);
	free(array->attr.aa.lower_bound);
	free(array->attr.aa.upper_bound);
	free(array->attr.aa.order);
}

/* manipulate private fields of array ir_type */
size_t get_array_n_dimensions(const ir_type *array)
{
	assert(array->type_op == type_array);
	return array->attr.aa.n_dimensions;
}

void set_array_bounds(ir_type *array, size_t dimension, ir_node *lower_bound,
                      ir_node *upper_bound)
{
	assert(array && (array->type_op == type_array));
	assert(lower_bound && "lower_bound node may not be NULL.");
	assert(upper_bound && "upper_bound node may not be NULL.");
	assert(dimension < array->attr.aa.n_dimensions);
	array->attr.aa.lower_bound[dimension] = lower_bound;
	array->attr.aa.upper_bound[dimension] = upper_bound;
}

void set_array_bounds_int(ir_type *array, size_t dimension, int lower_bound,
                          int upper_bound)
{
	ir_graph *irg = get_const_code_irg();
	set_array_bounds(array, dimension,
	          new_r_Const_long(irg, mode_Iu, lower_bound),
	          new_r_Const_long(irg, mode_Iu, upper_bound));
}

void set_array_lower_bound(ir_type *array, size_t dimension,
                           ir_node *lower_bound)
{
	assert(array && (array->type_op == type_array));
	assert(lower_bound && "lower_bound node may not be NULL.");
	array->attr.aa.lower_bound[dimension] = lower_bound;
}

void set_array_lower_bound_int(ir_type *array, size_t dimension, int lower_bound)
{
	ir_graph *irg = get_const_code_irg();
	set_array_lower_bound(array, dimension,
	     new_r_Const_long(irg, mode_Iu, lower_bound));
}

void set_array_upper_bound(ir_type *array, size_t dimension, ir_node *upper_bound)
{
  assert(array && (array->type_op == type_array));
  assert(upper_bound && "upper_bound node may not be NULL.");
  array->attr.aa.upper_bound[dimension] = upper_bound;
}

void set_array_upper_bound_int(ir_type *array, size_t dimension, int upper_bound)
{
	ir_graph *irg = get_const_code_irg();
	set_array_upper_bound(array, dimension,
	                      new_r_Const_long(irg, mode_Iu, upper_bound));
}

int has_array_lower_bound(const ir_type *array, size_t dimension)
{
	assert(array && (array->type_op == type_array));
	return !is_Unknown(array->attr.aa.lower_bound[dimension]);
}

ir_node *get_array_lower_bound(const ir_type *array, size_t dimension)
{
	assert(array && (array->type_op == type_array));
	return array->attr.aa.lower_bound[dimension];
}

long get_array_lower_bound_int(const ir_type *array, size_t dimension)
{
	ir_node *node;
	assert(array && (array->type_op == type_array));
	node = array->attr.aa.lower_bound[dimension];
	assert(is_Const(node));
	return get_tarval_long(get_Const_tarval(node));
}

int has_array_upper_bound(const ir_type *array, size_t dimension)
{
	assert(array && (array->type_op == type_array));
	return !is_Unknown(array->attr.aa.upper_bound[dimension]);
}

ir_node *get_array_upper_bound(const ir_type *array, size_t dimension)
{
	assert(array && (array->type_op == type_array));
	return array->attr.aa.upper_bound[dimension];
}

long get_array_upper_bound_int(const ir_type *array, size_t dimension)
{
	ir_node *node;
	assert(array && (array->type_op == type_array));
	node = array->attr.aa.upper_bound[dimension];
	assert(is_Const(node));
	return get_tarval_long(get_Const_tarval(node));
}

void set_array_order(ir_type *array, size_t dimension, int order)
{
	assert(array && (array->type_op == type_array));
	array->attr.aa.order[dimension] = order;
}

int get_array_order(const ir_type *array, size_t dimension)
{
	assert(array && (array->type_op == type_array));
	return array->attr.aa.order[dimension];
}

size_t find_array_dimension(const ir_type *array, int order)
{
	size_t dim;

	assert(array->type_op == type_array);

	for (dim = 0; dim < array->attr.aa.n_dimensions; ++dim) {
		if (array->attr.aa.order[dim] == order)
			return dim;
	}
	return (size_t)-1;
}

void set_array_element_type(ir_type *array, ir_type *tp)
{
	assert(array && (array->type_op == type_array));
	assert(!is_Method_type(tp));
	array->attr.aa.element_type = tp;
}

ir_type *get_array_element_type(const ir_type *array)
{
	assert(array && (array->type_op == type_array));
	return array->attr.aa.element_type;
}

void set_array_element_entity(ir_type *array, ir_entity *ent)
{
	assert(array && (array->type_op == type_array));
	assert((get_entity_type(ent)->type_op != type_method));
	array->attr.aa.element_ent = ent;
	array->attr.aa.element_type = get_entity_type(ent);
}

ir_entity *get_array_element_entity(const ir_type *array)
{
	assert(array && (array->type_op == type_array));
	return array->attr.aa.element_ent;
}

int (is_Array_type)(const ir_type *array)
{
	return _is_array_type(array);
}

void set_array_size(ir_type *tp, unsigned size)
{
	/* FIXME: Here we should make some checks with the element type size */
	tp->size = size;
}


ir_type *new_d_type_enumeration(ident *name, size_t n_enums, type_dbg_info *db)
{
	ir_type *res;

	res = new_type(type_enumeration, NULL, db);
	res->name = name;
	res->attr.ea.enumer = NEW_ARR_F(ir_enum_const, n_enums);
	hook_new_type(res);
	return res;
}

ir_type *new_type_enumeration(ident *name, size_t n_enums)
{
	return new_d_type_enumeration(name, n_enums, NULL);
}

void free_enumeration_entities(ir_type *enumeration)
{
	(void) enumeration;
	assert(enumeration->type_op == type_enumeration);
}

void free_enumeration_attrs(ir_type *enumeration)
{
	assert(enumeration->type_op == type_enumeration);
	DEL_ARR_F(enumeration->attr.ea.enumer);
}

ident *get_enumeration_ident(const ir_type *enumeration)
{
	assert(enumeration->type_op == type_enumeration);
	return enumeration->name;
}

const char *get_enumeration_name(const ir_type *enumeration)
{
	if (get_enumeration_ident(enumeration) == NULL)
		return NULL;
	return get_id_str(get_enumeration_ident(enumeration));
}

size_t get_enumeration_n_enums(const ir_type *enumeration)
{
	assert(enumeration->type_op == type_enumeration);
	return ARR_LEN(enumeration->attr.ea.enumer);
}

void set_enumeration_const(ir_type *enumeration, size_t pos, ident *nameid,
                           ir_tarval *con)
{
	assert(pos < ARR_LEN(enumeration->attr.ea.enumer));
	enumeration->attr.ea.enumer[pos].nameid = nameid;
	enumeration->attr.ea.enumer[pos].value  = con;
	enumeration->attr.ea.enumer[pos].owner  = enumeration;
}

ir_enum_const *get_enumeration_const(const ir_type *enumeration, size_t pos)
{
	assert(enumeration->type_op == type_enumeration);
	assert(pos < get_enumeration_n_enums(enumeration));
	return &enumeration->attr.ea.enumer[pos];
}

ir_type *get_enumeration_owner(const ir_enum_const *enum_cnst)
{
	return enum_cnst->owner;
}

void set_enumeration_value(ir_enum_const *enum_cnst, ir_tarval *con)
{
	enum_cnst->value = con;
}

ir_tarval *get_enumeration_value(const ir_enum_const *enum_cnst)
{
	return enum_cnst->value;
}

void set_enumeration_nameid(ir_enum_const *enum_cnst, ident *id)
{
	enum_cnst->nameid = id;
}

ident *get_enumeration_const_nameid(const ir_enum_const *enum_cnst)
{
	return enum_cnst->nameid;
}

const char *get_enumeration_const_name(const ir_enum_const *enum_cnst)
{
	return get_id_str(enum_cnst->nameid);
}

int (is_Enumeration_type)(const ir_type *enumeration)
{
	return _is_enumeration_type(enumeration);
}

void set_enumeration_mode(ir_type *tp, ir_mode *mode)
{
	assert(mode_is_int(mode) && "Modes of enumerations must be integers");
	/* For pointer and enumeration size depends on the mode, but only byte size allowed. */
	assert((get_mode_size_bits(mode) % 8) == 0 && "unorthodox modes not implemented");

	tp->size = get_mode_size_bytes(mode);
	tp->mode = mode;
}



ir_type *new_d_type_pointer(ir_type *points_to, type_dbg_info *db)
{
	ir_type *res;
	ir_mode *mode;

	if (is_Method_type(points_to) || is_code_type(points_to)) {
		mode = mode_P_code;
	} else {
		mode = mode_P_data;
	}

	res = new_type(type_pointer, mode, db);
	res->attr.pa.points_to = points_to;
	assert((get_mode_size_bits(res->mode) % 8 == 0) && "unorthodox modes not implemented");
	res->size = get_mode_size_bytes(res->mode);
	res->flags |= tf_layout_fixed;
	hook_new_type(res);
	return res;
}

ir_type *new_type_pointer(ir_type *points_to)
{
	return new_d_type_pointer(points_to, NULL);
}

void free_pointer_entities(ir_type *pointer)
{
	(void) pointer;
	assert(pointer && (pointer->type_op == type_pointer));
}

void free_pointer_attrs(ir_type *pointer)
{
	(void) pointer;
	assert(pointer && (pointer->type_op == type_pointer));
}

void set_pointer_points_to_type(ir_type *pointer, ir_type *tp)
{
	assert(pointer && (pointer->type_op == type_pointer));
	pointer->attr.pa.points_to = tp;
}

ir_type *get_pointer_points_to_type(const ir_type *pointer)
{
	assert(pointer && (pointer->type_op == type_pointer));
	return pointer->attr.pa.points_to;
}

int (is_Pointer_type)(const ir_type *pointer)
{
	return _is_pointer_type(pointer);
}

void set_pointer_mode(ir_type *tp, ir_mode *mode)
{
	assert(mode_is_reference(mode) && "Modes of pointers must be references");
	/* For pointer and enumeration size depends on the mode, but only byte size allowed. */
	assert((get_mode_size_bits(mode) & 7) == 0 && "unorthodox modes not implemented");

	tp->size = get_mode_size_bytes(mode);
	tp->mode = mode;
}

ir_type *find_pointer_type_to_type(ir_type *tp)
{
	size_t i, n = get_irp_n_types();
	for (i = 0; i < n; ++i) {
		ir_type *found = get_irp_type(i);
		if (is_Pointer_type(found) && get_pointer_points_to_type(found) == tp)
			return (found);
	}
	return firm_unknown_type;
}



ir_type *new_d_type_primitive(ir_mode *mode, type_dbg_info *db)
{
	ir_type *res = new_type(type_primitive, mode, db);
	res->size  = get_mode_size_bytes(mode);
	res->flags |= tf_layout_fixed;
	res->attr.ba.base_type = NULL;
	hook_new_type(res);
	return res;
}

ir_type *new_type_primitive(ir_mode *mode)
{
	return new_d_type_primitive(mode, NULL);
}

int (is_Primitive_type)(const ir_type *primitive)
{
	return _is_primitive_type(primitive);
}

void set_primitive_mode(ir_type *tp, ir_mode *mode)
{
	/* Modes of primitives must be data */
	assert(mode_is_data(mode));

	/* For primitive size depends on the mode. */
	tp->size = get_mode_size_bytes(mode);
	tp->mode = mode;
}

ir_type *get_primitive_base_type(const ir_type *tp)
{
	assert(is_Primitive_type(tp));
	return tp->attr.ba.base_type;
}

void set_primitive_base_type(ir_type *tp, ir_type *base_tp)
{
	assert(is_Primitive_type(tp));
	tp->attr.ba.base_type = base_tp;
}



int (is_atomic_type)(const ir_type *tp)
{
	return _is_atomic_type(tp);
}

size_t get_compound_n_members(const ir_type *tp)
{
	const tp_op *op  = get_type_tpop(tp);
	return op->ops.get_n_members(tp);
}

ir_entity *get_compound_member(const ir_type *tp, size_t pos)
{
	const tp_op *op = get_type_tpop(tp);
	return op->ops.get_member(tp, pos);
}

size_t get_compound_member_index(const ir_type *tp, ir_entity *member)
{
	const tp_op *op = get_type_tpop(tp);
	return op->ops.get_member_index(tp, member);
}

int is_compound_type(const ir_type *tp)
{
	assert(tp->kind == k_type);
	return tp->type_op->flags & TP_OP_FLAG_COMPOUND;
}

ident *get_compound_ident(const ir_type *tp)
{
	assert(is_compound_type(tp));
	return tp->name;
}

const char *get_compound_name(const ir_type *tp)
{
	if (get_compound_ident(tp) == NULL)
		return NULL;
	return get_id_str(get_compound_ident(tp));
}

void remove_compound_member(ir_type *compound, ir_entity *entity)
{
	switch (get_type_tpop_code(compound)) {
	case tpo_class:  remove_class_member(compound, entity);  break;
	case tpo_struct: remove_struct_member(compound, entity); break;
	case tpo_union:  remove_union_member(compound, entity);  break;
	default:
		panic("argument for remove_compound_member not a compound type");
	}
}

void add_compound_member(ir_type *compound, ir_entity *entity)
{
	switch (get_type_tpop_code(compound)) {
	case tpo_class:  add_class_member(compound, entity);  break;
	case tpo_struct: add_struct_member(compound, entity); break;
	case tpo_union:  add_union_member(compound, entity);  break;
	default:
		panic("argument for add_compound_member not a compound type");
	}
}



int is_code_type(const ir_type *tp)
{
	assert(tp && tp->kind == k_type);
	return tp->type_op == tpop_code;
}

int is_frame_type(const ir_type *tp)
{
	return tp->flags & tf_frame_type;
}

int is_value_param_type(const ir_type *tp)
{
	return tp->flags & tf_value_param_type;
}

int is_lowered_type(const ir_type *tp)
{
	return tp->flags & tf_lowered_type;
}

ir_type *new_type_value(void)
{
	ir_type *res = new_type_struct(new_id_from_str("<value_type>"));

	res->flags |= tf_value_param_type;

	/* Remove type from type list.  Must be treated differently than other types. */
	remove_irp_type(res);

	return res;
}

ir_type *new_type_frame(void)
{
	ir_type *res = new_type_class(new_id_from_str("<frame_type>"));

	res->flags |= tf_frame_type;

	/* Remove type from type list.  Must be treated differently than other types. */
	remove_irp_type(res);

	/* It is not possible to derive from the frame type. Set the final flag. */
	set_class_final(res, 1);

	return res;
}

ir_type *clone_frame_type(ir_type *type)
{
	ir_type *res;
	size_t  i, n;

	assert(is_frame_type(type));
	/* the entity link resource should be allocated if this function is called */
	assert(irp_resources_reserved(irp) & IR_RESOURCE_ENTITY_LINK);

	res = new_type_frame();
	for (i = 0, n = get_class_n_members(type); i < n; ++i) {
		ir_entity *ent  = get_class_member(type, i);
		ir_entity *nent = copy_entity_own(ent, res);
		set_entity_link(ent, nent);
		set_entity_link(nent, ent);
	}
	return res;
}

void set_lowered_type(ir_type *tp, ir_type *lowered_type)
{
	assert(is_type(tp) && is_type(lowered_type));
	lowered_type->flags |= tf_lowered_type;
	tp->assoc_type = lowered_type;
	lowered_type->assoc_type = tp;
}

ir_type *get_associated_type(const ir_type *tp)
{
	return tp->assoc_type;
}

void set_default_size(ir_type *tp, unsigned size)
{
	tp->size = size;
}

void default_layout_compound_type(ir_type *type)
{
	size_t i;
	size_t n = get_compound_n_members(type);
	int size = 0;
	unsigned align_all = 1;

	for (i = 0; i < n; ++i) {
		ir_entity *entity      = get_compound_member(type, i);
		ir_type   *entity_type = get_entity_type(entity);
		unsigned   align;
		unsigned   misalign;

		if (is_Method_type(entity_type))
			continue;

		assert(get_type_state(entity_type) == layout_fixed);
		align     = get_type_alignment_bytes(entity_type);
		align_all = align > align_all ? align : align_all;
		misalign  = (align ? size % align : 0);
		size     += (misalign ? align - misalign : 0);

		set_entity_offset(entity, size);
		if (!is_Union_type(type)) {
			size += get_type_size_bytes(entity_type);
		}
	}
	if (align_all > 0 && size % align_all) {
		size += align_all - (size % align_all);
	}
	if (align_all > get_type_alignment_bytes(type)) {
		set_type_alignment_bytes(type, align_all);
	}
	set_type_size_bytes(type, size);
	set_type_state(type, layout_fixed);
}

ir_entity *frame_alloc_area(ir_type *frame_type, int size, unsigned alignment,
                            int at_start)
{
	ir_entity *area;
	ir_type *tp;
	ident *name;
	char buf[32];
	unsigned frame_align;
	int offset, frame_size;
	static unsigned area_cnt = 0;
	static ir_type *a_byte = NULL;

	assert(is_frame_type(frame_type));
	assert(get_type_state(frame_type) == layout_fixed);
	assert(get_type_alignment_bytes(frame_type) > 0);
	set_type_state(frame_type, layout_undefined);

	if (! a_byte)
		a_byte = new_type_primitive(mode_Bu);

	snprintf(buf, sizeof(buf), "area%u", area_cnt++);
	name = new_id_from_str(buf);

	/* align the size */
	frame_align = get_type_alignment_bytes(frame_type);
	size = (size + frame_align - 1) & ~(frame_align - 1);

	tp = new_type_array(1, a_byte);
	set_array_bounds_int(tp, 0, 0, size);
	set_type_alignment_bytes(tp, alignment);

	frame_size = get_type_size_bytes(frame_type);
	if (at_start) {
		size_t i, n;
		/* fix all offsets so far */
		for (i = 0, n = get_class_n_members(frame_type); i < n; ++i) {
			ir_entity *ent = get_class_member(frame_type, i);

			set_entity_offset(ent, get_entity_offset(ent) + size);
		}
		/* calculate offset and new type size */
		offset = 0;
		frame_size += size;

		/* increase size to match alignment... */
		if (alignment > frame_align) {
			frame_align = alignment;
			set_type_alignment_bytes(frame_type, frame_align);
			frame_size  = (frame_size + frame_align - 1) & ~(frame_align - 1);
		}
	} else {
		/* calculate offset and new type size */
		offset = (frame_size + alignment - 1) & ~(alignment - 1);
		frame_size = offset + size;
	}

	area = new_entity(frame_type, name, tp);
	set_entity_offset(area, offset);
	set_type_size_bytes(frame_type, frame_size);

	/* mark this entity as compiler generated */
	set_entity_compiler_generated(area, 1);

	set_type_state(frame_type, layout_fixed);
	return area;
}

void ir_print_type(char *buffer, size_t buffer_size, const ir_type *type)
{
	ident *id;
	int p;
	type_dbg_info *tdbgi = get_type_dbg_info(type);
	if (tdbgi != NULL) {
		ir_retrieve_type_dbg_info(buffer, buffer_size, tdbgi);
		return;
	}

	/* we have to construct some name... */
	switch (get_type_tpop_code(type)) {
	case tpo_uninitialized:
		break;
	case tpo_code:
		snprintf(buffer, buffer_size, "code");
		return;

	case tpo_class:
		id = get_class_ident(type);
		snprintf(buffer, buffer_size, "class '%s'", get_id_str(id));
		return;

	case tpo_struct:
		id = get_struct_ident(type);
		snprintf(buffer, buffer_size, "struct '%s'", get_id_str(id));
		return;

	case tpo_union:
		id = get_union_ident(type);
		snprintf(buffer, buffer_size, "union '%s'", get_id_str(id));
		return;

	case tpo_enumeration:
		id = get_enumeration_ident(type);
		snprintf(buffer, buffer_size, "enumeration '%s'", get_id_str(id));
		return;

	case tpo_unknown:
		snprintf(buffer, buffer_size, "unknown type");
		return;

	case tpo_pointer:
		p = snprintf(buffer, buffer_size, "pointer to ");
		buffer      += p;
		buffer_size -= p;
		ir_print_type(buffer, buffer_size, get_pointer_points_to_type(type));
		return;

	case tpo_array:
		p = snprintf(buffer, buffer_size, "array of ");
		buffer      += p;
		buffer_size -= p;
		ir_print_type(buffer, buffer_size, get_array_element_type(type));
		return;

	case tpo_primitive:
		id = get_mode_ident(get_type_mode(type));
		snprintf(buffer, buffer_size, "%s", get_id_str(id));
		return;

	case tpo_none:
		snprintf(buffer, buffer_size, "none");
		return;
	case tpo_method:
		/* TODO: we should print argument and return types here... */
		snprintf(buffer, buffer_size, "method type");
		return;
	}
	snprintf(buffer, buffer_size, "invalid type");
}
