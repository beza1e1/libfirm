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
 * @brief      Intraprozedural analyses to estimate the call graph.
 * @author     Hubert Schmid
 * @date       09.06.2002
 * @version    $Id$
 * @summary
 *  Interprocedural analysis to estimate the calling relation.
 *
 *  This analysis computes all entities representing methods that
 *  can be called at a Call node.  Further it computes a set of
 *  methods that are 'free', i.e., their adress is handled by
 *  the program directly, or they are visible external.
 */
#include "config.h"

#ifdef HAVE_STRING_H
# include <string.h>
#endif

#include "cgana.h"
#include "rta.h"

#include "xmalloc.h"
#include "irnode_t.h"
#include "irmode_t.h"
#include "irprog_t.h"
#include "irgwalk.h"
#include "ircons.h"
#include "irgmod.h"
#include "iropt.h"
#include "irtools.h"

#include "irflag_t.h"
#include "dbginfo_t.h"
#include "iropt_dbg.h"

#include "eset.h"
#include "pmap.h"
#include "array.h"
#include "error.h"

#include "irdump.h"

/* unambiguous address used as a mark. */
static void *MARK = &MARK;

static eset *entities = NULL;

/*--------------------------------------------------------------------------*/
/* The analysis                                                             */
/*--------------------------------------------------------------------------*/


/*--------------------------------------------------------------------------*/
/* Initialize datastructures, remove unwanted constructs, optimize          */
/* call target computations.                                                */
/*--------------------------------------------------------------------------*/

/** Returns the entity that contains the implementation of the inherited
 *  entity if available, else returns the entity passed. */
static ir_entity *get_inherited_methods_implementation(ir_entity *inh_meth) {
	ir_node *value = get_atomic_ent_value(inh_meth);
	assert(value && "constant entity without value");
	assert(is_SymConst_addr_ent(value) &&
	       "Complex constant values not supported -- address of method should be straight constant!");

	return get_SymConst_entity(value);
}

/** Collect the entity representing the implementation of this
 *  method (not the same if inherited) and all entities for overwriting
 *  implementations in "set".
 *  If the implementation of the method is not included in the
 *  compilation unit "open" is set to true.
 *  A recursive descend in the overwritten relation.
 *  Cycle-free, therefore must terminate.
 *
 * @param method
 * @param set      A set of entities.
 * @param size     Number of entities in set.
 * @param open
 */
static void collect_impls(ir_entity *method, eset *set, int *size, int *open) {
	int i;
	ir_entity *impl;

	/* Add the implementation to the set if it contains an irg, else
	   remember that there are more methods called. */
	impl = method;
	if (get_entity_peculiarity(method) == peculiarity_inherited)
		impl = get_inherited_methods_implementation(method);

	if (get_entity_peculiarity(method) != peculiarity_description) {
		eset_insert(set, impl);
		++(*size);
	}

	/*- recursive descent -*/
	for (i = get_entity_n_overwrittenby(method) - 1; i >= 0; --i)
		collect_impls(get_entity_overwrittenby(method, i), set, size, open);
}

/** Alle Methoden bestimmen, die die �bergebene Methode �berschreiben
 *  (und implementieren). In der zur�ckgegebenen Reihung kommt jede
 *  Methode nur einmal vor. Der Wert 'NULL' steht f�r unbekannte
 *  (externe) Methoden. Die zur�ckgegebene Reihung mu� vom Aufrufer
 *  wieder freigegeben werden (siehe "DEL_ARR_F"). Gibt es �berhaupt
 *  keine Methoden, die "method" �berschreiben, so gibt die Methode
 *  "NULL" zur�ck.
 *
 *  @param method
 */
static ir_entity ** get_impl_methods(ir_entity * method) {
	eset * set = eset_create();
	int size = 0;
	ir_entity ** arr;
	int open = 0;

	/* Collect all method entities that can be called here */
	collect_impls(method, set, &size, &open);

	/* Vorgaenger einfuegen. */
	if (size == 0 && !open) {
		/* keine implementierte �berschriebene Methode */
		arr = NULL;
	} else if (open) {
		ir_entity * ent;
		arr = NEW_ARR_F(ir_entity *, size + 1);
		arr[0] = NULL;  /* Represents open method */
		for (ent = eset_first(set); size > 0; ent = eset_next(set), --size)
			arr[size] = ent;
	} else {
		ir_entity * ent;
		arr = NEW_ARR_F(ir_entity *, size);
		for (size -= 1, ent = eset_first(set); size >= 0; ent = eset_next(set), --size)
			arr[size] = ent;
	}
	eset_destroy(set);
	return arr;
}

/** Analyze address computations.
 *
 *  Compute for all Sel nodes the set of methods that can be selected.
 *  For each entity we store the set of subentities in the link field.
 *
 *  Further do some optimizations:
 *  - Call standard optimizations for Sel nodes: this removes polymorphic
 *    calls.
 *  - If the node is a SymConst(name) replace it by SymConst(ent) if possible.
 *    For this we precomputed a map name->entity.  Nowadays, we no more support
 *    this and assert.
 *  - If the node is a Sel:
 *    If we found only a single method that can be called, replace the Sel
 *    by a SymConst.  This is more powerful than the analysis in opt_polymorphy,
 *    as here we walk the type graph.  In opt_polymorphy we only apply a local
 *    pattern.
 *
 *  @param node  The node to analyze
 *  @param env   A map that maps names of entities to the entities.
 */
static void sel_methods_walker(ir_node *node, void *env) {
	pmap *ldname_map = env;
	ir_entity **arr;

	/* Call standard optimizations */
	if (is_Sel(node)) {
		ir_node *new_node = optimize_in_place(node);
		if (node != new_node) {
			exchange(node, new_node);
			node = new_node;
		}
	}

	/* replace SymConst(name)-operations by SymConst(ent) */
	if (is_SymConst(node)) {
		if (get_SymConst_kind(node) == symconst_addr_name) {
			pmap_entry *entry = pmap_find(ldname_map, get_SymConst_name(node));
			if (entry != NULL) { /* Method is declared in the compiled code */
				assert(!"There should not be a SymConst[addr_name] addressing a method with an implementation"
					"in this compilation unit.  Use a SymConst[addr_ent].");
			}
		}
	} else if (is_Sel(node) && is_Method_type(get_entity_type(get_Sel_entity(node)))) {
		ir_entity *ent = get_SymConst_entity(get_atomic_ent_value(get_Sel_entity(node)));
		assert(get_entity_peculiarity(ent) != peculiarity_inherited);

		if (!eset_contains(entities, ent)) {
			/* Entity not yet handled. Find all (internal or external)
			 * implemented methods that overwrites this entity.
			 * This set is stored in the entity link. */
			set_entity_link(ent, get_impl_methods(ent));
			eset_insert(entities, ent);
		}

		/* -- As an add on we get an optimization that removes polymorphic calls.
		This optimization is more powerful than that in transform_node_Sel().  -- */
		arr = get_entity_link(ent);
		if (arr == NULL) {
			/*
			 * The Sel node never returns a pointer to a usable method.
			 * We could not call it, but it may be description:
			 * We call a method in a dead part of the program.
			 */
			assert(get_entity_peculiarity(ent) == peculiarity_description);
		}
		else if (get_opt_closed_world() && get_opt_dyn_meth_dispatch() &&
			(ARR_LEN(arr) == 1 && arr[0] != NULL)) {
			ir_node *new_node;

			/*
			 * The Sel node returns only one possible method.
			 * So we could replace the Sel node by a SymConst.
			 * This method must exists.
			 */
			set_irg_current_block(current_ir_graph, get_nodes_block(node));
			assert(get_entity_peculiarity(get_SymConst_entity(get_atomic_ent_value(arr[0]))) ==
				peculiarity_existent);
			new_node = copy_const_value(get_irn_dbg_info(node), get_atomic_ent_value(arr[0]));
			DBG_OPT_POLY(node, new_node);
			exchange(node, new_node);
		}
	}
}

/**
 * Initialize auxiliary data structures.
 *
 * Computes a set of entities that overwrite an entity and contain
 * an implementation. The set is stored in the entity's link field.
 *
 * Further replaces Sel nodes where this set contains exactly one
 * method by SymConst nodes.
 * Finally asserts if there is a SymConst(name) if there could be a
 * SymConst(ent).
 */
static void sel_methods_init(void) {
	int i;
	pmap *ldname_map = pmap_create();   /* Map entity names to entities: to replace
	                                       SymConst(name) by SymConst(ent). */
	assert(entities == NULL);
	entities = eset_create();
	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		ir_entity * ent = get_irg_entity(get_irp_irg(i));
		/* only external visible methods are allowed to call by a SymConst_ptr_name */
		if (get_entity_visibility(ent) != visibility_local) {
			pmap_insert(ldname_map, (void *)get_entity_ld_ident(ent), ent);
		}
	}

	all_irg_walk(sel_methods_walker, NULL, ldname_map);
	pmap_destroy(ldname_map);
}

/*--------------------------------------------------------------------------*/
/* Find free methods.
 *
 * We expect that each entity has an array with all implementations in its
 * link field.                                                              */
/*--------------------------------------------------------------------------*/

/**
 * Returns an array of all methods that could be called at a Sel node.
 * This array contains every entry only once.
 *
 * @param sel  the Sel node
 */
static ir_entity ** get_Sel_arr(ir_node * sel) {
	static ir_entity ** NULL_ARRAY = NULL;
	ir_entity * ent;
	ir_entity ** arr;

	assert(is_Sel(sel));
	ent = get_Sel_entity(sel);
	ent = get_inherited_methods_implementation(ent);

	assert(is_Method_type(get_entity_type(ent))); /* what else? */
	arr = get_entity_link(ent);
	if (arr) {
		return arr;
	} else {
		/* "NULL" zeigt an, dass keine Implementierung existiert. Dies
		 * kann f�r polymorphe (abstrakte) Methoden passieren. */
		if (!NULL_ARRAY) {
			NULL_ARRAY = NEW_ARR_F(ir_entity *, 0);
		}
		return NULL_ARRAY;
	}
}

/**
 * Returns the number of possible called methods at a Sel node.
 *
 * @param sel  the Sel node
 */
static int get_Sel_n_methods(ir_node * sel) {
	return ARR_LEN(get_Sel_arr(sel));
}

/**
 * Returns the ith possible called method entity at a Sel node.
 */
static ir_entity * get_Sel_method(ir_node * sel, int pos) {
	ir_entity ** arr = get_Sel_arr(sel);
	assert(pos >= 0 && pos < ARR_LEN(arr));
	return arr[pos];
}

/* forward */
static void free_mark(ir_node * node, eset * set);

static void free_mark_proj(ir_node * node, long n, eset * set) {
	assert(get_irn_mode(node) == mode_T);
	if (get_irn_link(node) == MARK) {
		/* already visited */
		return;
	}
	set_irn_link(node, MARK);
	switch (get_irn_opcode(node)) {
	case iro_Proj: {
		/* proj_proj: in einem "sinnvollen" Graphen kommt jetzt ein
		 * op_Tuple oder ein Knoten, der in "free_ana_walker" behandelt
		 * wird. */
		ir_node * pred = get_Proj_pred(node);
		if (get_irn_link(pred) != MARK && is_Tuple(pred)) {
			free_mark_proj(get_Tuple_pred(pred, get_Proj_proj(node)), n, set);
		} else {
			/* nothing: da in "free_ana_walker" behandelt. */
		}
		break;
	}

	case iro_Tuple:
		free_mark(get_Tuple_pred(node, n), set);
		break;

	case iro_Id:
		free_mark_proj(get_Id_pred(node), n, set);
		break;

	case iro_Start:
	case iro_Alloc:
	case iro_Load:
		/* nothing: Die Operationen werden in free_ana_walker() selbst
		 * behandelt. */
		break;

	default:
		assert(0 && "unexpected opcode or opcode not implemented");
		break;
	}
	// set_irn_link(node, NULL);
}

/**
 * Called for predecessors nodes of "interesting" ones.
 * Interesting ones include all nodes that can somehow make
 * a method visible.
 *
 * If a method (or a set of methods in case of polymorph calls) gets visible,
 * add it to the set of 'free' methods
 *
 * @param node  the current visited node
 * @param set   the set of all free methods
 */
static void free_mark(ir_node *node, eset * set) {
	int i;

	if (get_irn_link(node) == MARK)
		return; /* already visited */

	set_irn_link(node, MARK);

	switch (get_irn_opcode(node)) {
	case iro_Sel: {
		ir_entity *ent = get_Sel_entity(node);
		if (is_method_entity(ent)) {
			for (i = get_Sel_n_methods(node) - 1; i >= 0; --i) {
				eset_insert(set, get_Sel_method(node, i));
			}
		}
		break;
	}
	case iro_SymConst:
		if (get_SymConst_kind(node) == symconst_addr_ent) {
			ir_entity *ent = get_SymConst_entity(node);
			if (is_method_entity(ent)) {
				eset_insert(set, ent);
			}
		} else {
			assert(get_SymConst_kind(node) == symconst_addr_name);
			/* nothing: SymConst points to extern method */
		}
		break;

	case iro_Phi:
		for (i = get_Phi_n_preds(node) - 1; i >= 0; --i) {
			free_mark(get_Phi_pred(node, i), set);
		}
		break;
	case iro_Proj:
		free_mark_proj(get_Proj_pred(node), get_Proj_proj(node), set);
		break;
	default:
		/* nothing: */
		break;
	}
}

/**
 * post-walker. Find method addresses.
 */
static void free_ana_walker(ir_node *node, void *env) {
	eset *set = env;
	int i;

	if (get_irn_link(node) == MARK) {
		/* already visited */
		return;
	}
	switch (get_irn_opcode(node)) {
		/* special nodes */
	case iro_Sel:
	case iro_SymConst:
	case iro_Const:
	case iro_Phi:
	case iro_Id:
	case iro_Proj:
	case iro_Tuple:
		/* nothing */
		break;
	case iro_Call:
		/* we must handle Call nodes specially, because their call address input
		   do not expose a method address. */
		set_irn_link(node, MARK);
		for (i = get_Call_n_params(node) - 1; i >= 0; --i) {
			ir_node *pred = get_Call_param(node, i);
			if (mode_is_reference(get_irn_mode(pred))) {
				free_mark(pred, set);
			}
		}
		break;
	default:
		/* other nodes: Alle anderen Knoten nehmen wir als Verr�ter an, bis
		 * jemand das Gegenteil implementiert. */
		set_irn_link(node, MARK);
		for (i = get_irn_arity(node) - 1; i >= 0; --i) {
			ir_node *pred = get_irn_n(node, i);
			if (mode_is_reference(get_irn_mode(pred))) {
				free_mark(pred, set);
			}
		}
		break;
	}
}

/**
 * Add all method addresses in global new style initializers to the set.
 *
 * @note
 * We do NOT check the type here, just it it's an entity address.
 * The reason for this is code like:
 *
 * void *p = function;
 *
 * which is sometimes used to anchor functions.
 */
static void add_method_address_inititializer(ir_initializer_t *initializer,
                                             eset *set)
{
	ir_node *n;
	size_t  i;

	switch (initializer->kind) {
	case IR_INITIALIZER_CONST:
		n = initializer->consti.value;

		/* let's check if it's the address of a function */
		if (is_Global(n)) {
			ir_entity *ent = get_Global_entity(n);

			if (is_Method_type(get_entity_type(ent)))
				eset_insert(set, ent);
		}
		return;
	case IR_INITIALIZER_TARVAL:
	case IR_INITIALIZER_NULL:
		return;
	case IR_INITIALIZER_COMPOUND:
		for (i = 0; i < initializer->compound.n_initializers; ++i) {
			ir_initializer_t *sub_initializer
				= initializer->compound.initializers[i];
			add_method_address_inititializer(sub_initializer, set);
		}
		return;
	}
	panic("invalid initializer found");
}

/**
 * Add all method addresses in global initializers to the set.
 *
 * @note
 * We do NOT check the type here, just it it's an entity address.
 * The reason for this is code like:
 *
 * void *p = function;
 *
 * which is sometimes used to anchor functions.
 */
static void add_method_address(ir_entity *ent, eset *set)
{
	ir_node *n;
	ir_type *tp;
	int i;

	/* do not check uninitialized values */
	if (get_entity_variability(ent) == variability_uninitialized)
		return;

	if (ent->has_initializer) {
		add_method_address_inititializer(get_entity_initializer(ent), set);
	} else if (is_atomic_entity(ent)) {
		tp = get_entity_type(ent);

		/* ignore methods: these of course reference it's address */
		if (is_Method_type(tp))
			return;

		/* let's check if it's the address of a function */
		n = get_atomic_ent_value(ent);
		if (is_Global(n)) {
			ent = get_Global_entity(n);

			if (is_Method_type(get_entity_type(ent)))
				eset_insert(set, ent);
		}
	} else {
		for (i = get_compound_ent_n_values(ent) - 1; i >= 0; --i) {
			n = get_compound_ent_value(ent, i);

			/* let's check if it's the address of a function */
			if (is_Global(n)) {
				ir_entity *ent = get_Global_entity(n);

				if (is_Method_type(get_entity_type(ent)))
					eset_insert(set, ent);
			}
		}
	}
}

/**
 * returns a list of 'free' methods, i.e., the methods that can be called
 * from external or via function pointers.
 *
 * Die Datenstrukturen f�r sel-Methoden (sel_methods) mu� vor dem
 * Aufruf von "get_free_methods" aufgebaut sein. Die (internen)
 * SymConst(name)-Operationen m�ssen in passende SymConst(ent)-Operationen
 * umgewandelt worden sein, d.h. SymConst-Operationen verweisen immer
 * auf eine echt externe Methode.
 */
static ir_entity **get_free_methods(int *length)
{
	eset *free_set = eset_create();
	int i;
	ir_entity **arr;
	ir_entity *ent;
	ir_graph *irg;
	ir_type *tp;

	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		irg = get_irp_irg(i);
		ent = get_irg_entity(irg);
		if (get_entity_visibility(ent) != visibility_local) {
			/* insert non-local (external) methods. */
			eset_insert(free_set, ent);
		} else if (get_entity_stickyness(ent) == stickyness_sticky) {
			/* insert "sticky" methods. */
			eset_insert(free_set, ent);
		}

		ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
		/* Find all method entities that gets "visible" through this graphs,
		 * for instance because their address is stored. */
		irg_walk_graph(irg, firm_clear_link, free_ana_walker, free_set);
		ir_free_resources(irg, IR_RESOURCE_IRN_LINK);
	}

	/* insert all methods that are used in global variables initializers */
	tp = get_glob_type();
	for (i = get_class_n_members(tp) - 1; i >= 0; --i) {
		ent = get_class_member(tp, i);
		add_method_address(ent, free_set);
	}
	tp = get_tls_type();
	for (i = get_struct_n_members(tp) - 1; i >= 0; --i) {
		ent = get_struct_member(tp, i);
		add_method_address(ent, free_set);
	}

	/* the main program is even then "free", if it's not external visible. */
	irg = get_irp_main_irg();
	if (irg != NULL)
		eset_insert(free_set, get_irg_entity(irg));

	/* Finally, transform the set into an array. */
	*length = eset_count(free_set);
	arr = XMALLOCN(ir_entity*, *length);
	for (i = 0, ent = eset_first(free_set); ent; ent = eset_next(free_set)) {
		arr[i++] = ent;
	}
	eset_destroy(free_set);

	return arr;
}

/*--------------------------------------------------------------------------*/
/* Callee analysis.                                                         */
/*--------------------------------------------------------------------------*/

static void callee_ana_node(ir_node * node, eset * methods);

static void callee_ana_proj(ir_node *node, long n, eset *methods) {
	assert(get_irn_mode(node) == mode_T);
	if (get_irn_link(node) == MARK) {
		/* already visited */
		return;
	}
	set_irn_link(node, MARK);

	switch (get_irn_opcode(node)) {
	case iro_Proj: {
		/* proj_proj: in einem "sinnvollen" Graphen kommt jetzt ein
		 * op_Tuple oder ein Knoten, der eine "freie Methode"
		 * zur�ckgibt. */
		ir_node *pred = get_Proj_pred(node);
		if (get_irn_link(pred) != MARK) {
			if (is_Tuple(pred)) {
				callee_ana_proj(get_Tuple_pred(pred, get_Proj_proj(node)), n, methods);
			} else {
				eset_insert(methods, unknown_entity); /* free method -> unknown */
			}
		}
		break;
	}

	case iro_Tuple:
		callee_ana_node(get_Tuple_pred(node, n), methods);
		break;

	default:
		eset_insert(methods, unknown_entity); /* free method -> unknown */
		break;
	}
}

/**
 * Analyse a Call address.
 *
 * @param node     the node representing the call address
 * @param methods  after call contains the set of all possibly called entities
 */
static void callee_ana_node(ir_node *node, eset *methods) {
	int i;

	assert(mode_is_reference(get_irn_mode(node)) || is_Bad(node));
	/* Beware of recursion */
	if (get_irn_link(node) == MARK) {
		/* already visited */
		return;
	}
	set_irn_link(node, MARK);

	switch (get_irn_opcode(node)) {
	case iro_Const:
		/* A direct address call. We tread this as an external
		   call and ignore it completely. */
		eset_insert(methods, unknown_entity); /* free method -> unknown */
		break;
	case iro_SymConst:
		if (get_SymConst_kind(node) == symconst_addr_ent) {
			ir_entity *ent = get_SymConst_entity(node);
			assert(ent && is_method_entity(ent));
			eset_insert(methods, ent);
		} else {
			assert(get_SymConst_kind(node) == symconst_addr_name);
			/* external method (because fix_symconst()!) */
			eset_insert(methods, unknown_entity); /* free method -> unknown */
		}
		break;
	case iro_Sel:
		/* polymorphic method */
		for (i = get_Sel_n_methods(node) - 1; i >= 0; --i) {
			ir_entity *ent = get_Sel_method(node, i);
			if (ent != NULL) {
				eset_insert(methods, ent);
			} else {
				eset_insert(methods, unknown_entity);
			}
		}
		break;

	case iro_Bad:
		/* nothing */
		break;

	case iro_Phi:
		for (i = get_Phi_n_preds(node) - 1; i >= 0; --i) {
			callee_ana_node(get_Phi_pred(node, i), methods);
		}
		break;

	case iro_Mux:
		callee_ana_node(get_Mux_false(node), methods);
		callee_ana_node(get_Mux_true(node), methods);
		break;

	case iro_Id:
		callee_ana_node(get_Id_pred(node), methods);
		break;

	case iro_Proj:
		callee_ana_proj(get_Proj_pred(node), get_Proj_proj(node), methods);
		break;

	case iro_Add:
	case iro_Sub:
	case iro_Conv:
		/* extern */
		eset_insert(methods, unknown_entity); /* free method -> unknown */
		break;

	default:
		assert(0 && "invalid opcode or opcode not implemented");
		break;
	}
}

/**
 * Walker: Analyses every Call node and calculates an array of possible
 * callees for that call.
 */
static void callee_walker(ir_node *call, void *env) {
	(void) env;
	if (is_Call(call)) {
		eset *methods = eset_create();
		ir_entity *ent;
		ir_entity **arr;
		int i;

		callee_ana_node(get_Call_ptr(call), methods);
		arr = NEW_ARR_F(ir_entity *, eset_count(methods));
		for (i = 0, ent = eset_first(methods); ent; ent = eset_next(methods)) {
			arr[i] = ent;
			/* we want the unknown_entity on the zero position for easy tests later */
			if (ent == unknown_entity) {
				arr[i] = arr[0];
				arr[0] = unknown_entity;
			}
			++i;
		}
		set_Call_callee_arr(call, ARR_LEN(arr), arr);
		DEL_ARR_F(arr);
		eset_destroy(methods);
	}
}

/**
 * Walker: Removes all tuple.
 */
static void remove_Tuples(ir_node *proj, void *env) {
	ir_node *nn;
	(void) env;
	if (! is_Proj(proj)) return;

	nn = skip_Tuple(proj);
	if (nn != proj) exchange(proj, nn);
}


/**
 * Determine for every Call the set of possibly called methods and stores it
 * inside the Call (@see set_Call_callee()).
 * Uses the sel_methods set with much be already calculated.
 */
static void callee_ana(void) {
	int i;
	/* analyse all graphs */
	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		ir_graph *irg = get_irp_irg(i);
		irg_walk_graph(irg, callee_walker, remove_Tuples, NULL);
		set_irg_callee_info_state(irg, irg_callee_info_consistent);
	}
	set_irp_callee_info_state(irg_callee_info_consistent);
}

/*--------------------------------------------------------------------------*/
/* Cleanup after analyses.                                                  */
/*--------------------------------------------------------------------------*/

/** Frees intermediate data structures. */
static void sel_methods_dispose(void) {
	ir_entity * ent;
	assert(entities);
	for (ent = eset_first(entities); ent; ent = eset_next(entities)) {
		ir_entity ** arr = get_entity_link(ent);
		if (arr) {
			DEL_ARR_F(arr);
		}
		set_entity_link(ent, NULL);
	}
	eset_destroy(entities);
	entities = NULL;
}

/*--------------------------------------------------------------------------*/
/* Freeing the callee arrays.                                               */
/*--------------------------------------------------------------------------*/

static void destruct_walker(ir_node * node, void * env) {
	(void) env;
	if (is_Call(node)) {
		remove_Call_callee_arr(node);
	}
}

/*--------------------------------------------------------------------------*/
/* Main drivers.                                                            */
/*--------------------------------------------------------------------------*/

void cgana(int *length, ir_entity ***free_methods) {
	/* Optimize Sel/SymConst nodes and compute all methods that implement an entity. */
	sel_methods_init();
	*free_methods = get_free_methods(length);
	callee_ana();
	sel_methods_dispose();
}

void free_callee_info(ir_graph *irg) {
	irg_walk_graph(irg, destruct_walker, NULL, NULL);
	set_irg_callee_info_state(irg, irg_callee_info_none);
}

void free_irp_callee_info(void) {
	int i;
	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		free_callee_info(get_irp_irg(i));
	}
}

/* Optimize the address expressions passed to call nodes.
 *
 * This optimization performs the following transformations for
 * all ir graphs:
 * - All SymConst operations that refer to intern methods are replaced
 *   by Const operations referring to the corresponding entity.
 * - Sel nodes, that select entities that are not overwritten are
 *   replaced by Const nodes referring to the selected entity.
 * - Sel nodes, for which no method exists at all are replaced by Bad
 *   nodes.
 * - Sel nodes with a pointer input that is an Alloc node are replaced
 *   by Const nodes referring to the entity that implements the method in
 *   the type given by the Alloc node.
 */
void opt_call_addrs(void) {
	sel_methods_init();
	sel_methods_dispose();
}
