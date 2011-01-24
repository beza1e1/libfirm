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
 * @file
 * @brief   Lowering of Calls with compound parameters and return types.
 * @author  Michael Beck
 * @version $Id$
 */

#include "config.h"

#include "lowering.h"
#include "irprog_t.h"
#include "irnode_t.h"
#include "type_t.h"
#include "irmode_t.h"
#include "ircons.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "irmemory.h"
#include "irtools.h"
#include "iroptimize.h"
#include "array_t.h"
#include "pmap.h"
#include "error.h"

/** A type map for def_find_pointer_type. */
static pmap *type_map;

/**
 * Default implementation for finding a pointer type for a given element type.
 * Simple create a new one.
 */
static ir_type *def_find_pointer_type(ir_type *e_type, ir_mode *mode,
                                      int alignment)
{
	/* Mode and alignment are always identical in all calls to def_find_pointer_type(), so
	   we simply can use a map from the element type to the pointer type. */
	ir_type *res = (ir_type*)pmap_get(type_map, e_type);
	if (res == NULL || get_type_mode(res) != mode) {
		res = new_type_pointer(e_type);
		set_type_mode(res, mode);
		set_type_alignment_bytes(res, alignment);
		pmap_insert(type_map, e_type, res);
	}
	return res;
}

/**
 * Creates a new lowered type for a method type with compound
 * arguments. The new type is associated to the old one and returned.
 *
 * @param lp   parameter struct
 * @param mtp  the method type to lower
 *
 * The current implementation expects that a lowered type already
 * includes the necessary changes ...
 */
static ir_type *create_modified_mtd_type(const lower_params_t *lp, ir_type *mtp)
{
	ir_type *lowered, *ptr_tp, *value_type;
	ir_type **params, **results, *res_tp;
	int     *param_map;
	ir_mode *modes[MAX_REGISTER_RET_VAL];
	int n_ress, n_params, nn_ress, nn_params, i, first_variadic;
	add_hidden hidden_params;
	int        changed = 0;
	ir_variadicity var;

	if (is_lowered_type(mtp)) {
		/* the type is already lowered. Not handled yet. */
		assert(0 && "lowered types NYI");
	}

	lowered = get_associated_type(mtp);
	if (lowered != NULL)
		return lowered;

	n_ress   = get_method_n_ress(mtp);
	NEW_ARR_A(ir_type *, results, n_ress);

	n_params = get_method_n_params(mtp);
	NEW_ARR_A(ir_type *, params, n_params + n_ress);

	NEW_ARR_A(int, param_map, n_params + n_ress);

	first_variadic = get_method_first_variadic_param_index(mtp);

	hidden_params = lp->hidden_params;
	if (hidden_params == ADD_HIDDEN_SMART &&
		get_method_variadicity(mtp) == variadicity_variadic)
		hidden_params = ADD_HIDDEN_ALWAYS_IN_FRONT;

	if (hidden_params == ADD_HIDDEN_ALWAYS_IN_FRONT) {
		/* add hidden in front */
		for (nn_ress = nn_params = i = 0; i < n_ress; ++i) {
			res_tp = get_method_res_type(mtp, i);

			if (is_compound_type(res_tp)) {
				int n_regs = 0;

				if (lp->flags & LF_SMALL_CMP_IN_REGS)
					n_regs = lp->ret_compound_in_regs(res_tp, modes);

				if (n_regs > 0) {
					/* this compound will be returned solely in registers */
					panic("Returning compounds in registers not yet implemented");
				}
				else {
					/* this compound will be allocated on callers stack and its
					   address will be transmitted as a hidden parameter. */
					ptr_tp = lp->find_pointer_type(res_tp, get_modeP_data(), lp->def_ptr_alignment);
					params[nn_params]    = ptr_tp;
					param_map[nn_params] = -1 - i;
					++nn_params;
					changed++;
					if (lp->flags & LF_RETURN_HIDDEN)
						results[nn_ress++] = ptr_tp;
				}
			} else {
				/* scalar result */
				results[nn_ress++] = res_tp;
			}
		}

		/* move the index of the first variadic parameter */
		first_variadic += nn_params;

		for (i = 0; i < n_params; ++i, ++nn_params) {
			params[nn_params]    = get_method_param_type(mtp, i);
			param_map[nn_params] = i;
		}
	} else {
		/* add hidden parameters last */
		assert(get_method_variadicity(mtp) == variadicity_non_variadic &&
			"Cannot add hidden parameters at end of variadic function");

		for (nn_params = 0; nn_params < n_params; ++nn_params) {
			params[nn_params]    = get_method_param_type(mtp, nn_params);
			param_map[nn_params] = nn_params;
		}

		for (nn_ress = i = 0; i < n_ress; ++i) {
			res_tp = get_method_res_type(mtp, i);

			if (is_compound_type(res_tp)) {
				params[nn_params] = lp->find_pointer_type(res_tp, get_modeP_data(), lp->def_ptr_alignment);
				param_map[nn_params] = -1 - i;
				++nn_params;
			} else {
				results[nn_ress++] = res_tp;
			}
		}
	}

	/* create the new type */
	lowered = new_d_type_method(nn_params, nn_ress, get_type_dbg_info(mtp));

	/* fill it */
	for (i = 0; i < nn_params; ++i)
		set_method_param_type(lowered, i, params[i]);
	for (i = 0; i < nn_ress; ++i)
		set_method_res_type(lowered, i, results[i]);

	var = get_method_variadicity(mtp);
	set_method_variadicity(lowered, var);
	if (var == variadicity_variadic)
		set_method_first_variadic_param_index(lowered, first_variadic);

	/* associate the lowered type with the original one for easier access */
	if (changed) {
		set_method_calling_convention(lowered, get_method_calling_convention(mtp) | cc_compound_ret);
	}

	set_lowered_type(mtp, lowered);

	value_type = get_method_value_param_type(mtp);
	if (value_type != NULL) {
		/* set new param positions */
		for (i = 0; i < nn_params; ++i) {
			ir_entity *ent = get_method_value_param_ent(lowered, i);
			int       pos  = param_map[i];
			ident     *id;

			set_entity_link(ent, INT_TO_PTR(pos));
			if (pos < 0) {
				/* formally return value, ignore for now */
				continue;
			}

			id = get_method_param_ident(mtp, pos);
			if (id != NULL) {
				set_method_param_ident(lowered, i, id);
				set_entity_ident(ent, id);
			}
		}

		set_lowered_type(value_type, get_method_value_param_type(lowered));
	}

	return lowered;
}

/**
 * A call list entry.
 */
typedef struct cl_entry cl_entry;
struct cl_entry {
	cl_entry *next;   /**< Pointer to the next entry. */
	ir_node  *call;   /**< Pointer to the Call node. */
	ir_node  *copyb;  /**< List of all CopyB nodes. */
};

/**
 * Walker environment for fix_args_and_collect_calls().
 */
typedef struct wlk_env_t {
	int                  arg_shift;        /**< The Argument index shift for parameters. */
	int                  first_hidden;     /**< The index of the first hidden argument. */
	struct obstack       obst;             /**< An obstack to allocate the data on. */
	cl_entry             *cl_list;         /**< The call list. */
	pmap                 *dummy_map;       /**< A map for finding the dummy arguments. */
	unsigned             dnr;              /**< The dummy index number. */
	const lower_params_t *params;          /**< Lowering parameters. */
	ir_type              *lowered_mtp;     /**< The lowered method type of the current irg if any. */
	ir_type              *value_params;    /**< The value params type if any. */
	unsigned             only_local_mem:1; /**< Set if only local memory access was found. */
	unsigned             changed:1;        /**< Set if the current graph was changed. */
} wlk_env;

/**
 * Return the call list entry of a call node.
 * If no entry exists yet, allocate one and enter the node into
 * the call list of the environment.
 *
 * @param call   A Call node.
 * @param env    The environment.
 */
static cl_entry *get_Call_entry(ir_node *call, wlk_env *env)
{
	cl_entry *res = (cl_entry*)get_irn_link(call);
	if (res == NULL) {
		cl_entry *res = OALLOC(&env->obst, cl_entry);
		res->next  = env->cl_list;
		res->call  = call;
		res->copyb = NULL;
		set_irn_link(call, res);
		env->cl_list = res;
	}
	return res;
}

/**
 * Finds the base address of an address by skipping Sel's and address
 * calculation.
 *
 * @param adr   the address
 * @param pEnt  points to the base entity if any
 */
static ir_node *find_base_adr(ir_node *ptr, ir_entity **pEnt)
{
	ir_entity *ent = NULL;
	assert(mode_is_reference(get_irn_mode(ptr)));

	for (;;) {
		if (is_Sel(ptr)) {
			ent = get_Sel_entity(ptr);
			ptr = get_Sel_ptr(ptr);
		}
		else if (is_Add(ptr)) {
			ir_node *left = get_Add_left(ptr);
			if (mode_is_reference(get_irn_mode(left)))
				ptr = left;
			else
				ptr = get_Add_right(ptr);
			ent = NULL;
		} else if (is_Sub(ptr)) {
			ptr = get_Sub_left(ptr);
			ent = NULL;
		} else {
			*pEnt = ent;
			return ptr;
		}
	}
}

/**
 * Check if a given pointer represents non-local memory.
 */
static void check_ptr(ir_node *ptr, wlk_env *env)
{
	ir_storage_class_class_t sc;
	ir_entity                *ent;

	/* still alias free */
	ptr = find_base_adr(ptr, &ent);
	sc  = get_base_sc(classify_pointer(ptr, ent));
	if (sc != ir_sc_localvar && sc != ir_sc_malloced) {
		/* non-local memory access */
		env->only_local_mem = 0;
	}
}

/**
 * Post walker: shift all parameter indexes
 * and collect Calls with compound returns in the call list.
 * If a non-alias free memory access is found, reset the alias free
 * flag.
 */
static void fix_args_and_collect_calls(ir_node *n, void *ctx)
{
	wlk_env *env = (wlk_env*)ctx;
	int      i;
	ir_type *ctp;
	ir_node *ptr;

	switch (get_irn_opcode(n)) {
	case iro_Sel:
		if (env->lowered_mtp != NULL && env->value_params != NULL) {
			ir_entity *ent = get_Sel_entity(n);

			if (get_entity_owner(ent) == env->value_params) {
				int pos = get_struct_member_index(env->value_params, ent) + env->arg_shift;
				ir_entity *new_ent;

				new_ent = get_method_value_param_ent(env->lowered_mtp, pos);
				set_entity_ident(new_ent, get_entity_ident(ent));
				set_Sel_entity(n, new_ent);
			}
		}
		break;
	case iro_Load:
	case iro_Store:
		if (env->only_local_mem) {
			ptr = get_irn_n(n, 1);
			check_ptr(ptr, env);
		}
		break;
	case iro_Proj:
		if (env->arg_shift > 0) {
			ir_node *pred = get_Proj_pred(n);

			/* Fix the argument numbers */
			if (pred == get_irg_args(current_ir_graph)) {
				long pnr = get_Proj_proj(n);
				set_Proj_proj(n, pnr + env->arg_shift);
				env->changed = 1;
			}
		}
		break;
	case iro_Call:
		if (! is_self_recursive_Call(n)) {
			/* any non self recursive call might access global memory */
			env->only_local_mem = 0;
		}

		ctp = get_Call_type(n);
		if (env->params->flags & LF_COMPOUND_RETURN) {
			/* check for compound returns */
			for (i = get_method_n_ress(ctp) -1; i >= 0; --i) {
				if (is_compound_type(get_method_res_type(ctp, i))) {
					/*
					 * This is a call with a compound return. As the result
					 * might be ignored, we must put it in the list.
					 */
					(void)get_Call_entry(n, env);
					break;
				}
			}
		}
		break;
	case iro_CopyB:
		if (env->only_local_mem) {
			check_ptr(get_CopyB_src(n), env);
			if (env->only_local_mem)
				check_ptr(get_CopyB_dst(n), env);
		}
		if (env->params->flags & LF_COMPOUND_RETURN) {
			/* check for compound returns */
			ir_node *src = get_CopyB_src(n);
			/* older scheme using value_res_ent */
			if (is_Sel(src)) {
				ir_node *proj = get_Sel_ptr(src);
				if (is_Proj(proj) && get_Proj_proj(proj) == pn_Call_P_value_res_base) {
					ir_node *call = get_Proj_pred(proj);
					if (is_Call(call)) {
						/* found a CopyB from compound Call result */
						cl_entry *e = get_Call_entry(call, env);
						set_irn_link(n, e->copyb);
						e->copyb = n;
					}
				}
			} else {
				/* new scheme: compound results are determined by the call type only */
				if (is_Proj(src)) {
					ir_node *proj = get_Proj_pred(src);
					if (is_Proj(proj) && get_Proj_proj(proj) == pn_Call_T_result) {
						ir_node *call = get_Proj_pred(proj);
						if (is_Call(call)) {
							ctp = get_Call_type(call);
							if (is_compound_type(get_method_res_type(ctp, get_Proj_proj(src)))) {
								/* found a CopyB from compound Call result */
								cl_entry *e = get_Call_entry(call, env);
								set_irn_link(n, e->copyb);
								e->copyb = n;
							}
						}
					}
				}
			}
		}
		break;
	default:
		/* do nothing */
		break;
	}
}

/**
 * Returns non-zero if a node is a compound address
 * of a frame-type entity.
 *
 * @param ft   the frame type
 * @param adr  the node
 */
static int is_compound_address(ir_type *ft, ir_node *adr)
{
	ir_entity *ent;

	if (! is_Sel(adr))
		return 0;
	if (get_Sel_n_indexs(adr) != 0)
		return 0;
	ent = get_Sel_entity(adr);
	return get_entity_owner(ent) == ft;
}

/** A pair for the copy-return-optimization. */
typedef struct cr_pair {
	ir_entity *ent; /**< the entity than can be removed from the frame */
	ir_node *arg;   /**< the argument that replaces the entities address */
} cr_pair;

/**
 * Post walker: fixes all entities addresses for the copy-return
 * optimization.
 *
 * Note: We expect the length of the cr_pair array (ie number of compound
 * return values) to be 1 (C, C++) in almost all cases, so ignore the
 * linear search complexity here.
 */
static void do_copy_return_opt(ir_node *n, void *ctx)
{
	if (is_Sel(n)) {
		ir_entity *ent = get_Sel_entity(n);
		cr_pair   *arr = (cr_pair*)ctx;
		size_t    i, l;

		for (i = 0, l = ARR_LEN(arr); i < l; ++i) {
			if (ent == arr[i].ent) {
				exchange(n, arr[i].arg);
				break;
			}
		}
	}
}

/**
 * Return a Sel node that selects a dummy argument of type tp.
 * Dummy arguments are only needed once and we use a map
 * to store them.
 * We could even assign all dummy arguments the same offset
 * in the frame type ...
 *
 * @param irg    the graph
 * @param block  the block where a newly create Sel should be placed
 * @param tp     the type of the dummy entity that should be create
 * @param env    the environment
 */
static ir_node *get_dummy_sel(ir_graph *irg, ir_node *block, ir_type *tp, wlk_env *env)
{
	ir_entity *ent;
	pmap_entry *e;

	/* use a map the check if we already create such an entity */
	e = pmap_find(env->dummy_map, tp);
	if (e)
		ent = (ir_entity*)e->value;
	else {
		ir_type *ft = get_irg_frame_type(irg);
		char buf[16];

		snprintf(buf, sizeof(buf), "dummy.%u", env->dnr++);
		ent = new_entity(ft, new_id_from_str(buf), tp);
		pmap_insert(env->dummy_map, tp, ent);

		if (get_type_state(ft) == layout_fixed) {
			/* Fix the layout again */
			assert(0 && "Fixed layout not implemented");
		}
	}
	return new_r_simpleSel(
		block,
		get_irg_no_mem(irg),
		get_irg_frame(irg),
		ent);
}

/**
 * Add the hidden parameter from the CopyB node to the Call node.
 *
 * @param irg    the graph
 * @param n_com  number of compound results (will be number of hidden parameters)
 * @param ins    in array to store the hidden parameters into
 * @param entry  the call list
 * @param env    the environment
 */
static void add_hidden_param(ir_graph *irg, int n_com, ir_node **ins, cl_entry *entry, wlk_env *env)
{
	ir_node *p, *n, *src, *mem, *blk;
	ir_entity *ent;
	ir_type *owner;
	int idx, n_args;

	n_args = 0;
	for (p = entry->copyb; p; p = n) {
		n   = (ir_node*)get_irn_link(p);
		src = get_CopyB_src(p);

		/* old scheme using value_res_ent */
		if (is_Sel(src)) {
			ent = get_Sel_entity(src);
			owner = get_entity_owner(ent);

			/* find the hidden parameter index */
			for (idx = 0; idx < get_struct_n_members(owner); ++idx)
				if (get_struct_member(owner, idx) == ent)
					break;
			assert(idx < get_struct_n_members(owner));
		} else {
			/* new scheme: compound returns are determined by the call type and are Proj's */
			idx = get_Proj_proj(src);
		}

		ins[idx] = get_CopyB_dst(p);
		mem      = get_CopyB_mem(p);
		blk      = get_nodes_block(p);

		/* get rid of the CopyB */
		turn_into_tuple(p, pn_CopyB_max);
		set_Tuple_pred(p, pn_CopyB_M,         mem);
		set_Tuple_pred(p, pn_CopyB_X_regular, new_r_Jmp(blk));
		set_Tuple_pred(p, pn_CopyB_X_except,  get_irg_bad(irg));
		++n_args;
	}

	/* now create dummy entities for function with ignored return value */
	if (n_args < n_com) {
		ir_type *ctp = get_Call_type(entry->call);
		int i, j;

		if (is_lowered_type(ctp))
			ctp = get_associated_type(ctp);

		for (j = i = 0; i < get_method_n_ress(ctp); ++i) {
			ir_type *rtp = get_method_res_type(ctp, i);
			if (is_compound_type(rtp)) {
				if (ins[j] == NULL)
					ins[j] = get_dummy_sel(irg, get_nodes_block(entry->call), rtp, env);
				++j;
			}
		}
	}
}

/**
 * Fix all calls on a call list by adding hidden parameters.
 *
 * @param irg  the graph
 * @param env  the environment
 */
static void fix_call_list(ir_graph *irg, wlk_env *env)
{
	const lower_params_t *lp = env->params;
	cl_entry *p;
	ir_node *call, **new_in;
	ir_type *ctp, *lowered_mtp;
	add_hidden hidden_params;
	int i, n_params, n_com, pos;

	new_in = NEW_ARR_F(ir_node *, 0);
	for (p = env->cl_list; p; p = p->next) {
		call = p->call;
		ctp = get_Call_type(call);
		lowered_mtp = create_modified_mtd_type(lp, ctp);
		set_Call_type(call, lowered_mtp);

		hidden_params = lp->hidden_params;
		if (hidden_params == ADD_HIDDEN_SMART &&
			get_method_variadicity(ctp) == variadicity_variadic)
			hidden_params = ADD_HIDDEN_ALWAYS_IN_FRONT;

		n_params = get_Call_n_params(call);

		n_com = 0;
		for (i = get_method_n_ress(ctp) - 1; i >= 0; --i) {
			if (is_compound_type(get_method_res_type(ctp, i)))
				++n_com;
		}
		pos = 2;
		ARR_RESIZE(ir_node *, new_in, n_params + n_com + pos);
		memset(new_in, 0, sizeof(*new_in) * (n_params + n_com + pos));
		if (hidden_params == ADD_HIDDEN_ALWAYS_IN_FRONT) {
			add_hidden_param(irg, n_com, &new_in[pos], p, env);
			pos += n_com;
		}
		/* copy all other parameters */
		for (i = 0; i < n_params; ++i)
			new_in[pos++] = get_Call_param(call, i);
		if (hidden_params == ADD_HIDDEN_ALWAYS_LAST) {
			add_hidden_param(irg, n_com, &new_in[pos], p, env);
			pos += n_com;
		}
		new_in[0] = get_Call_mem(call);
		new_in[1] = get_Call_ptr(call);

		set_irn_in(call, n_params + n_com + 2, new_in);
	}
}

/**
 * Transform a graph. If it has compound parameter returns,
 * remove them and use the hidden parameter instead.
 * If it calls methods with compound parameter returns, add hidden
 * parameters.
 *
 * @param lp   parameter struct
 * @param irg  the graph to transform
 */
static void transform_irg(const lower_params_t *lp, ir_graph *irg)
{
	ir_graph   *rem = current_ir_graph;
	ir_entity  *ent = get_irg_entity(irg);
	ir_type    *mtp, *lowered_mtp, *tp, *ft;
	int        i, j, k, n_ress = 0, n_ret_com = 0;
	size_t     n_cr_opt;
	ir_node    **new_in, *ret, *endbl, *bl, *mem, *copy;
	cr_pair    *cr_opt;
	wlk_env    env;
	add_hidden hidden_params;

	current_ir_graph = irg;

	assert(ent && "Cannot transform graph without an entity");
	assert(get_irg_phase_state(irg) == phase_high && "call lowering must be done in phase high");

	mtp = get_entity_type(ent);

	if (lp->flags & LF_COMPOUND_RETURN) {
		/* calculate the number of compound returns */
		n_ress = get_method_n_ress(mtp);
		for (n_ret_com = i = 0; i < n_ress; ++i) {
			tp = get_method_res_type(mtp, i);

			if (is_compound_type(tp))
				++n_ret_com;
		}
	}

	if (n_ret_com) {
		/* much easier if we have only one return */
		normalize_one_return(irg);

		/* This graph has a compound argument. Create a new type */
		lowered_mtp = create_modified_mtd_type(lp, mtp);
		set_entity_type(ent, lowered_mtp);

		hidden_params = lp->hidden_params;
		if (hidden_params == ADD_HIDDEN_SMART &&
			get_method_variadicity(mtp) == variadicity_variadic)
			hidden_params = ADD_HIDDEN_ALWAYS_IN_FRONT;

		if (hidden_params == ADD_HIDDEN_ALWAYS_IN_FRONT) {
			/* hidden arguments are added first */
			env.arg_shift    = n_ret_com;
			env.first_hidden = 0;
		} else {
			/* hidden arguments are added last */
			env.arg_shift    = 0;
			env.first_hidden = get_method_n_params(mtp);
		}
	} else {
		/* we must only search for calls */
		env.arg_shift = 0;
		lowered_mtp   = NULL;
	}
	obstack_init(&env.obst);
	env.cl_list        = NULL;
	env.dummy_map      = pmap_create_ex(8);
	env.dnr            = 0;
	env.params         = lp;
	env.lowered_mtp    = lowered_mtp;
	env.value_params   = get_method_value_param_type(mtp);
	env.only_local_mem = 1;
	env.changed        = 0;

	/* scan the code, fix argument numbers and collect calls. */
	irg_walk_graph(irg, firm_clear_link, fix_args_and_collect_calls, &env);

	/* fix all calls */
	if (env.cl_list) {
		fix_call_list(irg, &env);
		env.changed = 1;
	}

	if (n_ret_com) {
		/*
		 * Now fix the Return node of the current graph.
		 */
		env.changed = 1;

		/* STEP 1: find the return. This is simple, we have normalized the graph. */
		endbl = get_irg_end_block(irg);
		ret = NULL;
		for (i = get_Block_n_cfgpreds(endbl) - 1; i >= 0; --i) {
			ir_node *pred = get_Block_cfgpred(endbl, i);

			if (is_Return(pred)) {
				ret = pred;
				break;
			}
		}
		/* there should always be a return */
		assert(ret);

		/*
		 * STEP 2: fix it. For all compound return values add a CopyB,
		 * all others are copied.
		 */
		NEW_ARR_A(ir_node *, new_in, n_ress + 1);

		bl  = get_nodes_block(ret);
		mem = get_Return_mem(ret);

		ft = get_irg_frame_type(irg);
		NEW_ARR_A(cr_pair, cr_opt, n_ret_com);
		n_cr_opt = 0;
		for (j = 1, i = k = 0; i < n_ress; ++i) {
			ir_node *pred = get_Return_res(ret, i);
			tp = get_method_res_type(mtp, i);

			if (is_compound_type(tp)) {
				ir_node *arg = get_irg_args(irg);
				arg = new_r_Proj(arg, mode_P_data, env.first_hidden + k);
				++k;

				if (is_Unknown(pred)) {
					/* The Return(Unknown) is the Firm construct for a missing return.
					   Do nothing. */
				} else {
					/**
					 * Sorrily detecting that copy-return is possible isn't that simple.
					 * We must check, that the hidden address is alias free during the whole
					 * function.
					 * A simple heuristic: all Loads/Stores inside
					 * the function access only local frame.
					 */
					if (env.only_local_mem && is_compound_address(ft, pred)) {
						/* we can do the copy-return optimization here */
						cr_opt[n_cr_opt].ent = get_Sel_entity(pred);
						cr_opt[n_cr_opt].arg = arg;
						++n_cr_opt;
					} else { /* copy-return optimization is impossible, do the copy. */
						copy = new_r_CopyB(
							bl,
							mem,
							arg,
							pred,
							tp
							);
						mem = new_r_Proj(copy, mode_M, pn_CopyB_M);
					}
				}
				if (lp->flags & LF_RETURN_HIDDEN) {
					new_in[j] = arg;
					++j;
				}
			} else { /* scalar return value */
				new_in[j] = pred;
				++j;
			}
		}
		/* replace the in of the Return */
		new_in[0] = mem;
		set_irn_in(ret, j, new_in);

		if (n_cr_opt > 0) {
			size_t i, n;

			irg_walk_graph(irg, NULL, do_copy_return_opt, cr_opt);

			for (i = 0, n = ARR_LEN(cr_opt); i < n; ++i) {
				free_entity(cr_opt[i].ent);
			}
		}
	} /* if (n_ret_com) */

	pmap_destroy(env.dummy_map);
	obstack_free(&env.obst, NULL);

	if (env.changed) {
		/* invalidate the analysis info */
		set_irg_outs_inconsistent(irg);
		set_irg_loopinfo_state(irg, loopinfo_inconsistent);
	}
	current_ir_graph = rem;
}

/**
 * Returns non-zero if the given type is a method
 * type that must be lowered.
 *
 * @param lp  lowering parameters
 * @param tp  The type.
 */
static int must_be_lowered(const lower_params_t *lp, ir_type *tp)
{
  int i, n_ress;
  ir_type *res_tp;

  if (is_Method_type(tp)) {
    if (lp->flags & LF_COMPOUND_RETURN) {
      /* check for compound returns */
      n_ress = get_method_n_ress(tp);
      for (i = 0; i < n_ress; ++i) {
        res_tp = get_method_res_type(tp, i);

        if (is_compound_type(res_tp))
          return 1;
      }
    }
  }
  return 0;
}

/**
 * type-walker: lower all method types of entities
 * and points-to types.
 */
static void lower_method_types(type_or_ent tore, void *env)
{
	const lower_params_t *lp = (const lower_params_t*)env;
	ir_type *tp;

	/* fix method entities */
	if (is_entity(tore.ent)) {
		ir_entity *ent = tore.ent;
		tp = get_entity_type(ent);

		if (must_be_lowered(lp, tp)) {
			tp = create_modified_mtd_type(lp, tp);
			set_entity_type(ent, tp);
		}
	} else {
		tp = tore.typ;

		/* fix pointer to methods */
		if (is_Pointer_type(tp)) {
			ir_type *etp = get_pointer_points_to_type(tp);
			if (must_be_lowered(lp, etp)) {
				etp = create_modified_mtd_type(lp, etp);
				set_pointer_points_to_type(tp, etp);
			}
		}
	}
}

/*
 * Lower calls with compound parameters and return types.
 * This function does the following transformations:
 *
 * - Adds a new (hidden) pointer parameter for
 *   any return compound type.
 *
 * - Use of the hidden parameters in the function code.
 *
 * - Change all calls to functions with compound return
 *   by providing space for the hidden parameter on the callers
 *   stack.
 *
 * - Replace a possible block copy after the function call.
 */
void lower_calls_with_compounds(const lower_params_t *params)
{
	int i;
	ir_graph *irg;
	lower_params_t param = *params;

	if (param.find_pointer_type == NULL) {
		param.find_pointer_type = def_find_pointer_type;
		type_map = pmap_create_ex(8);
	} else
		type_map = NULL;

	/* first step: Transform all graphs */
	for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
		irg = get_irp_irg(i);

		transform_irg(&param, irg);
	}

	/* second step: Lower all method types of visible entities */
	type_walk(NULL, lower_method_types, &param);

	if (type_map)
		pmap_destroy(type_map);
}
