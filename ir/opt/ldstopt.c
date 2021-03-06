/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   Load/Store optimizations.
 * @author  Michael Beck
 */
#include <string.h>

#include "iroptimize.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "iropt_t.h"
#include "ircons_t.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "irtools.h"
#include "tv_t.h"
#include "dbginfo_t.h"
#include "iropt_dbg.h"
#include "irflag_t.h"
#include "array_t.h"
#include "irhooks.h"
#include "iredges.h"
#include "irpass.h"
#include "irmemory.h"
#include "irnodehashmap.h"
#include "irgopt.h"
#include "set.h"
#include "be.h"
#include "debug.h"

/** The debug handle. */
DEBUG_ONLY(static firm_dbg_module_t *dbg;)

#define MAX_PROJ MAX(MAX((long)pn_Load_max, (long)pn_Store_max), (long)pn_Call_max)

enum changes_t {
	DF_CHANGED = 1,       /**< data flow changed */
	CF_CHANGED = 2,       /**< control flow changed */
};

/**
 * walker environment
 */
typedef struct walk_env_t {
	struct obstack obst;          /**< list of all stores */
	unsigned changes;             /**< a bitmask of graph changes */
} walk_env_t;

/** A Load/Store info. */
typedef struct ldst_info_t {
	ir_node  *projs[MAX_PROJ+1];  /**< list of Proj's of this node */
	ir_node  *exc_block;          /**< the exception block if available */
	int      exc_idx;             /**< predecessor index in the exception block */
	unsigned visited;             /**< visited counter for breaking loops */
} ldst_info_t;

/**
 * flags for control flow.
 */
enum block_flags_t {
	BLOCK_HAS_COND = 1,      /**< Block has conditional control flow */
	BLOCK_HAS_EXC  = 2       /**< Block has exceptional control flow */
};

/**
 * a Block info.
 */
typedef struct block_info_t {
	unsigned flags;               /**< flags for the block */
} block_info_t;

/** the master visited flag for loop detection. */
static unsigned master_visited = 0;

#define INC_MASTER()       ++master_visited
#define MARK_NODE(info)    (info)->visited = master_visited
#define NODE_VISITED(info) (info)->visited >= master_visited

/**
 * get the Load/Store info of a node
 */
static ldst_info_t *get_ldst_info(ir_node *node, struct obstack *obst)
{
	ldst_info_t *info = (ldst_info_t*)get_irn_link(node);

	if (! info) {
		info = OALLOCZ(obst, ldst_info_t);
		set_irn_link(node, info);
	}
	return info;
}

/**
 * get the Block info of a node
 */
static block_info_t *get_block_info(ir_node *node, struct obstack *obst)
{
	block_info_t *info = (block_info_t*)get_irn_link(node);

	if (! info) {
		info = OALLOCZ(obst, block_info_t);
		set_irn_link(node, info);
	}
	return info;
}

/**
 * update the projection info for a Load/Store
 */
static unsigned update_projs(ldst_info_t *info, ir_node *proj)
{
	long nr = get_Proj_proj(proj);

	assert(0 <= nr && nr <= MAX_PROJ && "Wrong proj from LoadStore");

	if (info->projs[nr]) {
		/* there is already one, do CSE */
		exchange(proj, info->projs[nr]);
		return DF_CHANGED;
	}
	else {
		info->projs[nr] = proj;
		return 0;
	}
}

/**
 * update the exception block info for a Load/Store node.
 *
 * @param info   the load/store info struct
 * @param block  the exception handler block for this load/store
 * @param pos    the control flow input of the block
 */
static unsigned update_exc(ldst_info_t *info, ir_node *block, int pos)
{
	assert(info->exc_block == NULL && "more than one exception block found");

	info->exc_block = block;
	info->exc_idx   = pos;
	return 0;
}

/**
 * walker, collects all Load/Store/Proj nodes
 *
 * walks from Start -> End
 */
static void collect_nodes(ir_node *node, void *env)
{
	walk_env_t  *wenv   = (walk_env_t *)env;
	unsigned     opcode = get_irn_opcode(node);
	ir_node     *pred, *blk, *pred_blk;
	ldst_info_t *ldst_info;

	if (opcode == iro_Proj) {
		pred   = get_Proj_pred(node);
		opcode = get_irn_opcode(pred);

		if (opcode == iro_Load || opcode == iro_Store || opcode == iro_Call) {
			ldst_info = get_ldst_info(pred, &wenv->obst);

			wenv->changes |= update_projs(ldst_info, node);

			/*
			 * Place the Proj's to the same block as the
			 * predecessor Load. This is always ok and prevents
			 * "non-SSA" form after optimizations if the Proj
			 * is in a wrong block.
			 */
			blk      = get_nodes_block(node);
			pred_blk = get_nodes_block(pred);
			if (blk != pred_blk) {
				wenv->changes |= DF_CHANGED;
				set_nodes_block(node, pred_blk);
			}
		}
	} else if (opcode == iro_Block) {
		int i;

		for (i = get_Block_n_cfgpreds(node) - 1; i >= 0; --i) {
			ir_node      *pred_block, *proj;
			block_info_t *bl_info;
			int          is_exc = 0;

			pred = proj = get_Block_cfgpred(node, i);

			if (is_Proj(proj)) {
				pred   = get_Proj_pred(proj);
				is_exc = is_x_except_Proj(proj);
			}

			/* ignore Bad predecessors, they will be removed later */
			if (is_Bad(pred))
				continue;

			pred_block = get_nodes_block(pred);
			bl_info    = get_block_info(pred_block, &wenv->obst);

			if (is_fragile_op(pred) && is_exc)
				bl_info->flags |= BLOCK_HAS_EXC;
			else if (is_irn_forking(pred))
				bl_info->flags |= BLOCK_HAS_COND;

			opcode = get_irn_opcode(pred);
			if (is_exc && (opcode == iro_Load || opcode == iro_Store || opcode == iro_Call)) {
				ldst_info = get_ldst_info(pred, &wenv->obst);

				wenv->changes |= update_exc(ldst_info, node, i);
			}
		}
	}
}

/**
 * Returns an entity if the address ptr points to a constant one.
 *
 * @param ptr  the address
 *
 * @return an entity or NULL
 */
static ir_entity *find_constant_entity(ir_node *ptr)
{
	for (;;) {
		if (is_SymConst(ptr) && get_SymConst_kind(ptr) == symconst_addr_ent) {
			return get_SymConst_entity(ptr);
		} else if (is_Sel(ptr)) {
			ir_entity *ent = get_Sel_entity(ptr);
			ir_type   *tp  = get_entity_owner(ent);

			/* Do not fiddle with polymorphism. */
			if (is_Class_type(tp) &&
				((get_entity_n_overwrites(ent)    != 0) ||
				(get_entity_n_overwrittenby(ent) != 0)   ) )
				return NULL;

			if (is_Array_type(tp)) {
				/* check bounds */
				int i, n;

				for (i = 0, n = get_Sel_n_indexs(ptr); i < n; ++i) {
					ir_node   *bound;
					ir_tarval *tlower, *tupper;
					ir_node   *index = get_Sel_index(ptr, i);
					ir_tarval *tv    = computed_value(index);

					/* check if the index is constant */
					if (tv == tarval_bad)
						return NULL;

					bound  = get_array_lower_bound(tp, i);
					tlower = computed_value(bound);
					bound  = get_array_upper_bound(tp, i);
					tupper = computed_value(bound);

					if (tlower == tarval_bad || tupper == tarval_bad)
						return NULL;

					if (tarval_cmp(tv, tlower) == ir_relation_less)
						return NULL;
					if (tarval_cmp(tupper, tv) == ir_relation_less)
						return NULL;

					/* ok, bounds check finished */
				}
			}

			if (get_entity_linkage(ent) & IR_LINKAGE_CONSTANT)
				return ent;

			/* try next */
			ptr = get_Sel_ptr(ptr);
		} else if (is_Add(ptr)) {
			ir_node *l = get_Add_left(ptr);
			ir_node *r = get_Add_right(ptr);

			if (get_irn_mode(l) == get_irn_mode(ptr) && is_Const(r))
				ptr = l;
			else if (get_irn_mode(r) == get_irn_mode(ptr) && is_Const(l))
				ptr = r;
			else
				return NULL;

			/* for now, we support only one addition, reassoc should fold all others */
			if (! is_SymConst(ptr) && !is_Sel(ptr))
				return NULL;
		} else if (is_Sub(ptr)) {
			ir_node *l = get_Sub_left(ptr);
			ir_node *r = get_Sub_right(ptr);

			if (get_irn_mode(l) == get_irn_mode(ptr) && is_Const(r))
				ptr = l;
			else
				return NULL;
			/* for now, we support only one substraction, reassoc should fold all others */
			if (! is_SymConst(ptr) && !is_Sel(ptr))
				return NULL;
		} else
			return NULL;
	}
}

/**
 * Return the Selection index of a Sel node from dimension n
 */
static long get_Sel_array_index_long(ir_node *n, int dim)
{
	ir_node *index = get_Sel_index(n, dim);
	return get_tarval_long(get_Const_tarval(index));
}

typedef struct path_entry {
	ir_entity         *ent;
	struct path_entry *next;
	size_t            index;
} path_entry;

static ir_node *rec_find_compound_ent_value(ir_node *ptr, path_entry *next)
{
	path_entry       entry, *p;
	ir_entity        *ent, *field;
	ir_initializer_t *initializer;
	ir_tarval        *tv;
	ir_type          *tp;
	size_t           n;

	entry.next = next;
	if (is_SymConst(ptr)) {
		/* found the root */
		ent         = get_SymConst_entity(ptr);
		initializer = get_entity_initializer(ent);
		for (p = next; p != NULL;) {
			if (initializer->kind != IR_INITIALIZER_COMPOUND)
				return NULL;
			n  = get_initializer_compound_n_entries(initializer);
			tp = get_entity_type(ent);

			if (is_Array_type(tp)) {
				ent = get_array_element_entity(tp);
				if (ent != p->ent) {
					/* a missing [0] */
					if (0 >= n)
						return NULL;
					initializer = get_initializer_compound_value(initializer, 0);
					continue;
				}
			}
			if (p->index >= n)
				return NULL;
			initializer = get_initializer_compound_value(initializer, p->index);

			ent = p->ent;
			p   = p->next;
		}
		tp = get_entity_type(ent);
		while (is_Array_type(tp)) {
			ent = get_array_element_entity(tp);
			tp = get_entity_type(ent);
			/* a missing [0] */
			n  = get_initializer_compound_n_entries(initializer);
			if (0 >= n)
				return NULL;
			initializer = get_initializer_compound_value(initializer, 0);
		}

		switch (initializer->kind) {
		case IR_INITIALIZER_CONST:
			return get_initializer_const_value(initializer);
		case IR_INITIALIZER_TARVAL:
		case IR_INITIALIZER_NULL:
		default:
			return NULL;
		}
	} else if (is_Sel(ptr)) {
		entry.ent = field = get_Sel_entity(ptr);
		tp = get_entity_owner(field);
		if (is_Array_type(tp)) {
			assert(get_Sel_n_indexs(ptr) == 1 && "multi dim arrays not implemented");
			entry.index = get_Sel_array_index_long(ptr, 0) - get_array_lower_bound_int(tp, 0);
		} else {
			size_t i, n_members = get_compound_n_members(tp);
			for (i = 0; i < n_members; ++i) {
				if (get_compound_member(tp, i) == field)
					break;
			}
			if (i >= n_members) {
				/* not found: should NOT happen */
				return NULL;
			}
			entry.index = i;
		}
		return rec_find_compound_ent_value(get_Sel_ptr(ptr), &entry);
	}  else if (is_Add(ptr)) {
		ir_mode  *mode;
		unsigned pos;

		{
			ir_node *l = get_Add_left(ptr);
			ir_node *r = get_Add_right(ptr);
			if (is_Const(r)) {
				ptr = l;
				tv  = get_Const_tarval(r);
			} else {
				ptr = r;
				tv  = get_Const_tarval(l);
			}
		}
ptr_arith:
		mode = get_tarval_mode(tv);

		/* ptr must be a Sel or a SymConst, this was checked in find_constant_entity() */
		if (is_Sel(ptr)) {
			field = get_Sel_entity(ptr);
		} else {
			field = get_SymConst_entity(ptr);
		}

		/* count needed entries */
		pos = 0;
		for (ent = field;;) {
			tp = get_entity_type(ent);
			if (! is_Array_type(tp))
				break;
			ent = get_array_element_entity(tp);
			++pos;
		}
		/* should be at least ONE entry */
		if (pos == 0)
			return NULL;

		/* allocate the right number of entries */
		NEW_ARR_A(path_entry, p, pos);

		/* fill them up */
		pos = 0;
		for (ent = field;;) {
			unsigned   size;
			ir_tarval *sz, *tv_index, *tlower, *tupper;
			long       index;
			ir_node   *bound;

			tp = get_entity_type(ent);
			if (! is_Array_type(tp))
				break;
			ent = get_array_element_entity(tp);
			p[pos].ent  = ent;
			p[pos].next = &p[pos + 1];

			size = get_type_size_bytes(get_entity_type(ent));
			sz   = new_tarval_from_long(size, mode);

			tv_index = tarval_div(tv, sz);
			tv       = tarval_mod(tv, sz);

			if (tv_index == tarval_bad || tv == tarval_bad)
				return NULL;

			assert(get_array_n_dimensions(tp) == 1 && "multiarrays not implemented");
			bound  = get_array_lower_bound(tp, 0);
			tlower = computed_value(bound);
			bound  = get_array_upper_bound(tp, 0);
			tupper = computed_value(bound);

			if (tlower == tarval_bad || tupper == tarval_bad)
				return NULL;

			if (tarval_cmp(tv_index, tlower) == ir_relation_less)
				return NULL;
			if (tarval_cmp(tupper, tv_index) == ir_relation_less)
				return NULL;

			/* ok, bounds check finished */
			index = get_tarval_long(tv_index);
			p[pos].index = index;
			++pos;
		}
		if (! tarval_is_null(tv)) {
			/* hmm, wrong access */
			return NULL;
		}
		p[pos - 1].next = next;
		return rec_find_compound_ent_value(ptr, p);
	} else if (is_Sub(ptr)) {
		ir_node *l = get_Sub_left(ptr);
		ir_node *r = get_Sub_right(ptr);

		ptr = l;
		tv  = get_Const_tarval(r);
		tv  = tarval_neg(tv);
		goto ptr_arith;
	}
	return NULL;
}

static ir_node *find_compound_ent_value(ir_node *ptr)
{
	return rec_find_compound_ent_value(ptr, NULL);
}

/* forward */
static void reduce_adr_usage(ir_node *ptr);

/**
 * Update a Load that may have lost its users.
 */
static void handle_load_update(ir_node *load)
{
	ldst_info_t *info = (ldst_info_t*)get_irn_link(load);

	/* do NOT touch volatile loads for now */
	if (get_Load_volatility(load) == volatility_is_volatile)
		return;

	if (! info->projs[pn_Load_res] && ! info->projs[pn_Load_X_except]) {
		ir_node *ptr = get_Load_ptr(load);
		ir_node *mem = get_Load_mem(load);

		/* a Load whose value is neither used nor exception checked, remove it */
		exchange(info->projs[pn_Load_M], mem);
		if (info->projs[pn_Load_X_regular])
			exchange(info->projs[pn_Load_X_regular], new_r_Jmp(get_nodes_block(load)));
		kill_node(load);
		reduce_adr_usage(ptr);
	}
}

/**
 * A use of an address node has vanished. Check if this was a Proj
 * node and update the counters.
 */
static void reduce_adr_usage(ir_node *ptr)
{
	ir_node *pred;
	if (!is_Proj(ptr))
		return;
	if (get_irn_n_edges(ptr) > 0)
		return;

	/* this Proj is dead now */
	pred = get_Proj_pred(ptr);
	if (is_Load(pred)) {
		ldst_info_t *info = (ldst_info_t*)get_irn_link(pred);
		info->projs[get_Proj_proj(ptr)] = NULL;

		/* this node lost its result proj, handle that */
		handle_load_update(pred);
	}
}

/**
 * Check, if an already existing value of mode old_mode can be converted
 * into the needed one new_mode without loss.
 */
static int can_use_stored_value(ir_mode *old_mode, ir_mode *new_mode)
{
	unsigned old_size;
	unsigned new_size;
	if (old_mode == new_mode)
		return true;

	old_size = get_mode_size_bits(old_mode);
	new_size = get_mode_size_bits(new_mode);

	/* if both modes are two-complement ones, we can always convert the
	   Stored value into the needed one. (on big endian machines we currently
	   only support this for modes of same size) */
	if (old_size >= new_size &&
		  get_mode_arithmetic(old_mode) == irma_twos_complement &&
		  get_mode_arithmetic(new_mode) == irma_twos_complement &&
		  (!be_get_backend_param()->byte_order_big_endian
	        || old_size == new_size)) {
		return true;
	}
	return false;
}

/**
 * Check whether a Call is at least pure, i.e. does only read memory.
 */
static unsigned is_Call_pure(ir_node *call)
{
	ir_type *call_tp = get_Call_type(call);
	unsigned prop = get_method_additional_properties(call_tp);

	/* check first the call type */
	if ((prop & (mtp_property_const|mtp_property_pure)) == 0) {
		/* try the called entity */
		ir_node *ptr = get_Call_ptr(call);

		if (is_SymConst_addr_ent(ptr)) {
			ir_entity *ent = get_SymConst_entity(ptr);

			prop = get_entity_additional_properties(ent);
		}
	}
	return (prop & (mtp_property_const|mtp_property_pure)) != 0;
}

static ir_node *get_base_and_offset(ir_node *ptr, long *pOffset)
{
	ir_mode *mode  = get_irn_mode(ptr);
	long    offset = 0;

	/* TODO: long might not be enough, we should probably use some tarval thingy... */
	for (;;) {
		if (is_Add(ptr)) {
			ir_node *l = get_Add_left(ptr);
			ir_node *r = get_Add_right(ptr);

			if (get_irn_mode(l) != mode || !is_Const(r))
				break;

			offset += get_tarval_long(get_Const_tarval(r));
			ptr     = l;
		} else if (is_Sub(ptr)) {
			ir_node *l = get_Sub_left(ptr);
			ir_node *r = get_Sub_right(ptr);

			if (get_irn_mode(l) != mode || !is_Const(r))
				break;

			offset -= get_tarval_long(get_Const_tarval(r));
			ptr     = l;
		} else if (is_Sel(ptr)) {
			ir_entity *ent = get_Sel_entity(ptr);
			ir_type   *tp  = get_entity_owner(ent);

			if (is_Array_type(tp)) {
				int     size;
				ir_node *index;

				/* only one dimensional arrays yet */
				if (get_Sel_n_indexs(ptr) != 1)
					break;
				index = get_Sel_index(ptr, 0);
				if (! is_Const(index))
					break;

				tp = get_entity_type(ent);
				if (get_type_state(tp) != layout_fixed)
					break;

				size    = get_type_size_bytes(tp);
				offset += size * get_tarval_long(get_Const_tarval(index));
			} else {
				if (get_type_state(tp) != layout_fixed)
					break;
				offset += get_entity_offset(ent);
			}
			ptr = get_Sel_ptr(ptr);
		} else
			break;
	}

	*pOffset = offset;
	return ptr;
}

static int try_load_after_store(ir_node *load,
		ir_node *load_base_ptr, long load_offset, ir_node *store)
{
	ldst_info_t *info;
	ir_node *store_ptr      = get_Store_ptr(store);
	long     store_offset;
	ir_node *store_base_ptr = get_base_and_offset(store_ptr, &store_offset);
	ir_node *store_value;
	ir_mode *store_mode;
	ir_node *load_ptr;
	ir_mode *load_mode;
	long     load_mode_len;
	long     store_mode_len;
	long     delta;
	int      res;

	if (load_base_ptr != store_base_ptr)
		return 0;

	load_mode      = get_Load_mode(load);
	load_mode_len  = get_mode_size_bytes(load_mode);
	store_mode     = get_irn_mode(get_Store_value(store));
	store_mode_len = get_mode_size_bytes(store_mode);
	delta          = load_offset - store_offset;
	store_value    = get_Store_value(store);

	if (delta < 0 || delta+load_mode_len > store_mode_len)
		return 0;

	if (store_mode != load_mode &&
	    get_mode_arithmetic(store_mode) == irma_twos_complement &&
	    get_mode_arithmetic(load_mode)  == irma_twos_complement) {

		/* produce a shift to adjust offset delta */
		unsigned const shift = be_get_backend_param()->byte_order_big_endian
			? store_mode_len - load_mode_len - delta
			: delta;
		if (shift != 0) {
			ir_graph *const irg  = get_irn_irg(load);
			ir_node  *const cnst = new_r_Const_long(irg, mode_Iu, shift * 8);
			store_value = new_r_Shr(get_nodes_block(load),
									store_value, cnst, store_mode);
		}

		store_value = new_r_Conv(get_nodes_block(load), store_value, load_mode);
	} else {
		/* we would need some kind of bitcast node here */
		return 0;
	}

	DBG_OPT_RAW(load, store_value);

	info = (ldst_info_t*)get_irn_link(load);
	if (info->projs[pn_Load_M])
		exchange(info->projs[pn_Load_M], get_Load_mem(load));

	res = 0;
	/* no exception */
	if (info->projs[pn_Load_X_except]) {
		ir_graph *irg = get_irn_irg(load);
		exchange( info->projs[pn_Load_X_except], new_r_Bad(irg, mode_X));
		res |= CF_CHANGED;
	}
	if (info->projs[pn_Load_X_regular]) {
		exchange( info->projs[pn_Load_X_regular], new_r_Jmp(get_nodes_block(load)));
		res |= CF_CHANGED;
	}

	if (info->projs[pn_Load_res])
		exchange(info->projs[pn_Load_res], store_value);

	load_ptr = get_Load_ptr(load);
	kill_node(load);
	reduce_adr_usage(load_ptr);
	return res | DF_CHANGED;
}

/**
 * Follow the memory chain as long as there are only Loads,
 * alias free Stores, and constant Calls and try to replace the
 * current Load by a previous ones.
 * Note that in unreachable loops it might happen that we reach
 * load again, as well as we can fall into a cycle.
 * We break such cycles using a special visited flag.
 *
 * INC_MASTER() must be called before dive into
 */
static unsigned follow_Mem_chain(ir_node *load, ir_node *curr)
{
	unsigned    res = 0;
	ldst_info_t *info = (ldst_info_t*)get_irn_link(load);
	ir_node     *pred;
	ir_node     *ptr       = get_Load_ptr(load);
	ir_node     *mem       = get_Load_mem(load);
	ir_mode     *load_mode = get_Load_mode(load);

	for (pred = curr; load != pred; ) {
		ldst_info_t *pred_info = (ldst_info_t*)get_irn_link(pred);

		/*
		 * a Load immediately after a Store -- a read after write.
		 * We may remove the Load, if both Load & Store does not have an
		 * exception handler OR they are in the same Block. In the latter
		 * case the Load cannot throw an exception when the previous Store was
		 * quiet.
		 *
		 * Why we need to check for Store Exception? If the Store cannot
		 * be executed (ROM) the exception handler might simply jump into
		 * the load Block :-(
		 * We could make it a little bit better if we would know that the
		 * exception handler of the Store jumps directly to the end...
		 */
		if (is_Store(pred) && ((pred_info->projs[pn_Store_X_except] == NULL
				&& info->projs[pn_Load_X_except] == NULL)
				|| get_nodes_block(load) == get_nodes_block(pred)))
		{
			long    load_offset;
			ir_node *base_ptr = get_base_and_offset(ptr, &load_offset);
			int     changes   = try_load_after_store(load, base_ptr, load_offset, pred);

			if (changes != 0)
				return res | changes;
		} else if (is_Load(pred) && get_Load_ptr(pred) == ptr &&
		           can_use_stored_value(get_Load_mode(pred), load_mode)) {
			/*
			 * a Load after a Load -- a read after read.
			 * We may remove the second Load, if it does not have an exception
			 * handler OR they are in the same Block. In the later case
			 * the Load cannot throw an exception when the previous Load was
			 * quiet.
			 *
			 * Here, there is no need to check if the previous Load has an
			 * exception hander because they would have exact the same
			 * exception...
			 *
			 * TODO: implement load-after-load with different mode for big
			 *       endian
			 */
			if (info->projs[pn_Load_X_except] == NULL
					|| get_nodes_block(load) == get_nodes_block(pred)) {
				ir_node *value;

				DBG_OPT_RAR(load, pred);

				/* the result is used */
				if (info->projs[pn_Load_res]) {
					if (pred_info->projs[pn_Load_res] == NULL) {
						/* create a new Proj again */
						pred_info->projs[pn_Load_res] = new_r_Proj(pred, get_Load_mode(pred), pn_Load_res);
					}
					value = pred_info->projs[pn_Load_res];

					/* add an convert if needed */
					if (get_Load_mode(pred) != load_mode) {
						value = new_r_Conv(get_nodes_block(load), value, load_mode);
					}

					exchange(info->projs[pn_Load_res], value);
				}

				if (info->projs[pn_Load_M])
					exchange(info->projs[pn_Load_M], mem);

				/* no exception */
				if (info->projs[pn_Load_X_except]) {
					ir_graph *irg = get_irn_irg(load);
					exchange(info->projs[pn_Load_X_except], new_r_Bad(irg, mode_X));
					res |= CF_CHANGED;
				}
				if (info->projs[pn_Load_X_regular]) {
					exchange( info->projs[pn_Load_X_regular], new_r_Jmp(get_nodes_block(load)));
					res |= CF_CHANGED;
				}

				kill_node(load);
				reduce_adr_usage(ptr);
				return res |= DF_CHANGED;
			}
		}

		if (is_Store(pred)) {
			/* check if we can pass through this store */
			ir_alias_relation rel = get_alias_relation(
				get_Store_ptr(pred),
				get_irn_mode(get_Store_value(pred)),
				ptr, load_mode);
			/* if the might be an alias, we cannot pass this Store */
			if (rel != ir_no_alias)
				break;
			pred = skip_Proj(get_Store_mem(pred));
		} else if (is_Load(pred)) {
			pred = skip_Proj(get_Load_mem(pred));
		} else if (is_Call(pred)) {
			if (is_Call_pure(pred)) {
				/* The called graph is at least pure, so there are no Store's
				   in it. We can handle it like a Load and skip it. */
				pred = skip_Proj(get_Call_mem(pred));
			} else {
				/* there might be Store's in the graph, stop here */
				break;
			}
		} else {
			/* follow only Load chains */
			break;
		}

		/* check for cycles */
		if (NODE_VISITED(pred_info))
			break;
		MARK_NODE(pred_info);
	}

	if (is_Sync(pred)) {
		int i;

		/* handle all Sync predecessors */
		for (i = get_Sync_n_preds(pred) - 1; i >= 0; --i) {
			res |= follow_Mem_chain(load, skip_Proj(get_Sync_pred(pred, i)));
			if (res)
				return res;
		}
	}

	return res;
}

ir_node *can_replace_load_by_const(const ir_node *load, ir_node *c)
{
	ir_mode  *c_mode = get_irn_mode(c);
	ir_mode  *l_mode = get_Load_mode(load);
	ir_node  *block  = get_nodes_block(load);
	dbg_info *dbgi   = get_irn_dbg_info(load);
	ir_node  *res    = copy_const_value(dbgi, c, block);

	if (c_mode != l_mode) {
		/* check, if the mode matches OR can be easily converted info */
		if (is_reinterpret_cast(c_mode, l_mode)) {
			/* copy the value from the const code irg and cast it */
			res = new_rd_Conv(dbgi, block, res, l_mode);
		} else {
			return NULL;
		}
	}
	return res;
}

/**
 * optimize a Load
 *
 * @param load  the Load node
 */
static unsigned optimize_load(ir_node *load)
{
	ldst_info_t *info = (ldst_info_t*)get_irn_link(load);
	ir_node     *mem, *ptr, *value;
	ir_entity   *ent;
	long        dummy;
	unsigned    res = 0;

	/* do NOT touch volatile loads for now */
	if (get_Load_volatility(load) == volatility_is_volatile)
		return 0;

	/* the address of the load to be optimized */
	ptr = get_Load_ptr(load);

	/* The mem of the Load. Must still be returned after optimization. */
	mem = get_Load_mem(load);

	if (info->projs[pn_Load_res] == NULL
			&& info->projs[pn_Load_X_except] == NULL) {
		/* the value is never used and we don't care about exceptions, remove */
		exchange(info->projs[pn_Load_M], mem);

		if (info->projs[pn_Load_X_regular]) {
			/* should not happen, but if it does, remove it */
			exchange(info->projs[pn_Load_X_regular], new_r_Jmp(get_nodes_block(load)));
			res |= CF_CHANGED;
		}
		kill_node(load);
		reduce_adr_usage(ptr);
		return res | DF_CHANGED;
	}

	value = NULL;
	/* check if we can determine the entity that will be loaded */
	ent = find_constant_entity(ptr);
	if (ent != NULL
			&& get_entity_visibility(ent) != ir_visibility_external) {
		/* a static allocation that is not external: there should be NO
		 * exception when loading even if we cannot replace the load itself.
		 */

		/* no exception, clear the info field as it might be checked later again */
		if (info->projs[pn_Load_X_except]) {
			ir_graph *irg = get_irn_irg(load);
			exchange(info->projs[pn_Load_X_except], new_r_Bad(irg, mode_X));
			info->projs[pn_Load_X_except] = NULL;
			res |= CF_CHANGED;
		}
		if (info->projs[pn_Load_X_regular]) {
			exchange(info->projs[pn_Load_X_regular], new_r_Jmp(get_nodes_block(load)));
			info->projs[pn_Load_X_regular] = NULL;
			res |= CF_CHANGED;
		}

		if (get_entity_linkage(ent) & IR_LINKAGE_CONSTANT) {
			if (has_entity_initializer(ent)) {
				/* new style initializer */
				value = find_compound_ent_value(ptr);
			}
			if (value != NULL) {
				ir_graph *irg = get_irn_irg(load);
				value = can_replace_load_by_const(load, value);
				if (value != NULL && is_Sel(ptr)) {
					/* frontend has inserted masking operations after bitfield accesses,
					 * so we might have to shift the const. */
					unsigned char bit_offset = get_entity_offset_bits_remainder(get_Sel_entity(ptr));
					if (bit_offset != 0) {
						if (is_Const(value)) {
							ir_tarval *tv_old = get_Const_tarval(value);
							ir_tarval *tv_offset = new_tarval_from_long(bit_offset, mode_Bu);
							ir_tarval *tv_new = tarval_shl(tv_old, tv_offset);
							value = new_r_Const(irg, tv_new);
						} else {
							value = NULL;
						}
					}
				}
			}
		}
	}
	if (value != NULL) {
		/* we completely replace the load by this value */
		if (info->projs[pn_Load_X_except]) {
			ir_graph *irg = get_irn_irg(load);
			exchange(info->projs[pn_Load_X_except], new_r_Bad(irg, mode_X));
			info->projs[pn_Load_X_except] = NULL;
			res |= CF_CHANGED;
		}
		if (info->projs[pn_Load_X_regular]) {
			exchange(info->projs[pn_Load_X_regular], new_r_Jmp(get_nodes_block(load)));
			info->projs[pn_Load_X_regular] = NULL;
			res |= CF_CHANGED;
		}
		if (info->projs[pn_Load_M]) {
			exchange(info->projs[pn_Load_M], mem);
			res |= DF_CHANGED;
		}
		if (info->projs[pn_Load_res]) {
			exchange(info->projs[pn_Load_res], value);
			res |= DF_CHANGED;
		}
		kill_node(load);
		reduce_adr_usage(ptr);
		return res;
	}

	/* Check, if the address of this load is used more than once.
	 * If not, more load cannot be removed in any case. */
	if (get_irn_n_edges(ptr) <= 1 && get_irn_n_edges(get_base_and_offset(ptr, &dummy)) <= 1)
		return res;

	/*
	 * follow the memory chain as long as there are only Loads
	 * and try to replace current Load or Store by a previous one.
	 * Note that in unreachable loops it might happen that we reach
	 * load again, as well as we can fall into a cycle.
	 * We break such cycles using a special visited flag.
	 */
	INC_MASTER();
	res = follow_Mem_chain(load, skip_Proj(mem));
	return res;
}

/**
 * Check whether a value of mode new_mode would completely overwrite a value
 * of mode old_mode in memory.
 */
static int is_completely_overwritten(ir_mode *old_mode, ir_mode *new_mode)
{
	return get_mode_size_bits(new_mode) >= get_mode_size_bits(old_mode);
}

/**
 * Check whether small is a part of large (starting at same address).
 */
static int is_partially_same(ir_node *small, ir_node *large)
{
	ir_mode *sm = get_irn_mode(small);
	ir_mode *lm = get_irn_mode(large);

	/* FIXME: Check endianness */
	return is_Conv(small) && get_Conv_op(small) == large
	    && get_mode_size_bytes(sm) < get_mode_size_bytes(lm)
	    && get_mode_arithmetic(sm) == irma_twos_complement
	    && get_mode_arithmetic(lm) == irma_twos_complement;
}

/**
 * follow the memory chain as long as there are only Loads and alias free Stores.
 *
 * INC_MASTER() must be called before dive into
 */
static unsigned follow_Mem_chain_for_Store(ir_node *store, ir_node *curr, bool had_split)
{
	unsigned res = 0;
	ldst_info_t *info = (ldst_info_t*)get_irn_link(store);
	ir_node *pred;
	ir_node *ptr = get_Store_ptr(store);
	ir_node *mem = get_Store_mem(store);
	ir_node *value = get_Store_value(store);
	ir_mode *mode  = get_irn_mode(value);
	ir_node *block = get_nodes_block(store);

	for (pred = curr; pred != store;) {
		ldst_info_t *pred_info = (ldst_info_t*)get_irn_link(pred);

		/*
		 * BEWARE: one might think that checking the modes is useless, because
		 * if the pointers are identical, they refer to the same object.
		 * This is only true in strong typed languages, not is C were the following
		 * is possible *(ir_type1 *)p = a; *(ir_type2 *)p = b ...
		 * However, if the size of the mode that is written is bigger or equal the
		 * size of the old one, the old value is completely overwritten and can be
		 * killed ...
		 */
		if (is_Store(pred) && !had_split && get_Store_ptr(pred) == ptr &&
            get_nodes_block(pred) == block) {
			/*
			 * a Store after a Store in the same Block -- a write after write.
			 */

			/*
			 * We may remove the first Store, if the old value is completely
			 * overwritten or the old value is a part of the new value,
			 * and if it does not have an exception handler.
			 *
			 * TODO: What, if both have the same exception handler ???
			 */
			if (get_Store_volatility(pred) != volatility_is_volatile
			        && !pred_info->projs[pn_Store_X_except]) {
				ir_node *predvalue = get_Store_value(pred);
				ir_mode *predmode  = get_irn_mode(predvalue);

				if (is_completely_overwritten(predmode, mode)
				        || is_partially_same(predvalue, value)) {
					DBG_OPT_WAW(pred, store);
					DB((dbg, LEVEL_1, "  killing store %+F (override by %+F)\n", pred, store));
					exchange(pred_info->projs[pn_Store_M], get_Store_mem(pred));
					kill_node(pred);
					reduce_adr_usage(ptr);
					return DF_CHANGED;
				}
			}

			/*
			 * We may remove the Store, if the old value already contains
			 * the new value, and if it does not have an exception handler.
			 *
			 * TODO: What, if both have the same exception handler ???
			 */
			if (get_Store_volatility(store) != volatility_is_volatile
			        && !info->projs[pn_Store_X_except]) {
				ir_node *predvalue = get_Store_value(pred);

				if (is_partially_same(value, predvalue)) {
					DBG_OPT_WAW(pred, store);
					DB((dbg, LEVEL_1, "  killing store %+F (override by %+F)\n", pred, store));
					exchange(info->projs[pn_Store_M], mem);
					kill_node(store);
					reduce_adr_usage(ptr);
					return DF_CHANGED;
				}
			}
		} else if (is_Load(pred) && get_Load_ptr(pred) == ptr &&
		           value == pred_info->projs[pn_Load_res]) {
			/*
			 * a Store of a value just loaded from the same address
			 * -- a write after read.
			 * We may remove the Store, if it does not have an exception
			 * handler.
			 */
			if (! info->projs[pn_Store_X_except]) {
				DBG_OPT_WAR(store, pred);
				DB((dbg, LEVEL_1, "  killing store %+F (read %+F from same address)\n", store, pred));
				exchange(info->projs[pn_Store_M], mem);
				kill_node(store);
				reduce_adr_usage(ptr);
				return DF_CHANGED;
			}
		}

		if (is_Store(pred)) {
			/* check if we can pass through this store */
			ir_alias_relation rel = get_alias_relation(
				get_Store_ptr(pred),
				get_irn_mode(get_Store_value(pred)),
				ptr, mode);
			/* if the might be an alias, we cannot pass this Store */
			if (rel != ir_no_alias)
				break;
			pred = skip_Proj(get_Store_mem(pred));
		} else if (is_Load(pred)) {
			ir_alias_relation rel = get_alias_relation(
				get_Load_ptr(pred), get_Load_mode(pred),
				ptr, mode);
			if (rel != ir_no_alias)
				break;

			pred = skip_Proj(get_Load_mem(pred));
		} else {
			/* follow only Load chains */
			break;
		}

		/* check for cycles */
		if (NODE_VISITED(pred_info))
			break;
		MARK_NODE(pred_info);
	}

	if (is_Sync(pred)) {
		int i;

		/* handle all Sync predecessors */
		for (i = get_Sync_n_preds(pred) - 1; i >= 0; --i) {
			res |= follow_Mem_chain_for_Store(store, skip_Proj(get_Sync_pred(pred, i)), true);
			if (res)
				break;
		}
	}
	return res;
}

/** find entity used as base for an address calculation */
static ir_entity *find_entity(ir_node *ptr)
{
	switch (get_irn_opcode(ptr)) {
	case iro_SymConst:
		return get_SymConst_entity(ptr);
	case iro_Sel: {
		ir_node *pred = get_Sel_ptr(ptr);
		if (get_irg_frame(get_irn_irg(ptr)) == pred)
			return get_Sel_entity(ptr);

		return find_entity(pred);
	}
	case iro_Sub:
	case iro_Add: {
		ir_node *left = get_binop_left(ptr);
		ir_node *right;
		if (mode_is_reference(get_irn_mode(left)))
			return find_entity(left);
		right = get_binop_right(ptr);
		if (mode_is_reference(get_irn_mode(right)))
			return find_entity(right);
		return NULL;
	}
	default:
		return NULL;
	}
}

/**
 * optimize a Store
 *
 * @param store  the Store node
 */
static unsigned optimize_store(ir_node *store)
{
	ir_node   *ptr;
	ir_node   *mem;
	ir_entity *entity;

	if (get_Store_volatility(store) == volatility_is_volatile)
		return 0;

	ptr    = get_Store_ptr(store);
	entity = find_entity(ptr);

	/* a store to an entity which is never read is unnecessary */
	if (entity != NULL && !(get_entity_usage(entity) & ir_usage_read)) {
		ldst_info_t *info = (ldst_info_t*)get_irn_link(store);
		if (info->projs[pn_Store_X_except] == NULL) {
			DB((dbg, LEVEL_1, "  Killing useless %+F to never read entity %+F\n", store, entity));
			exchange(info->projs[pn_Store_M], get_Store_mem(store));
			kill_node(store);
			reduce_adr_usage(ptr);
			return DF_CHANGED;
		}
	}

	/* Check, if the address of this Store is used more than once.
	 * If not, this Store cannot be removed in any case. */
	if (get_irn_n_edges(ptr) <= 1)
		return 0;

	mem = get_Store_mem(store);

	/* follow the memory chain as long as there are only Loads */
	INC_MASTER();

	return follow_Mem_chain_for_Store(store, skip_Proj(mem), false);
}

/* check if a node has more than one real user. Keepalive edges do not count as
 * real users */
static bool has_multiple_users(const ir_node *node)
{
	unsigned real_users = 0;
	foreach_out_edge(node, edge) {
		ir_node *user = get_edge_src_irn(edge);
		if (is_End(user))
			continue;
		++real_users;
		if (real_users > 1)
			return true;
	}
	return false;
}

/**
 * walker, optimizes Phi after Stores to identical places:
 * Does the following optimization:
 * @verbatim
 *
 *   val1   val2   val3          val1  val2  val3
 *    |      |      |               \    |    /
 *  Store  Store  Store              \   |   /
 *      \    |    /                   PhiData
 *       \   |   /                       |
 *        \  |  /                      Store
 *          PhiM
 *
 * @endverbatim
 * This reduces the number of stores and allows for predicated execution.
 * Moves Stores back to the end of a function which may be bad.
 *
 * This is only possible if the predecessor blocks have only one successor.
 */
static unsigned optimize_phi(ir_node *phi, walk_env_t *wenv)
{
	int i, n;
	ir_node *store, *ptr, *block, *phi_block, *phiM, *phiD, *exc, *projM;
#ifdef DO_CACHEOPT
	ir_node *old_store;
#endif
	ir_mode *mode;
	ir_node **inM, **inD, **projMs;
	int *idx;
	dbg_info *db = NULL;
	ldst_info_t *info;
	block_info_t *bl_info;
	unsigned res = 0;

	/* Must be a memory Phi */
	if (get_irn_mode(phi) != mode_M)
		return 0;

	n = get_Phi_n_preds(phi);
	if (n <= 0)
		return 0;

	/* must be only one user */
	projM = get_Phi_pred(phi, 0);
	if (has_multiple_users(projM))
		return 0;

	store = skip_Proj(projM);
#ifdef DO_CACHEOPT
	old_store = store;
#endif
	if (!is_Store(store))
		return 0;

	block = get_nodes_block(store);

	/* check if the block is post dominated by Phi-block
	   and has no exception exit */
	bl_info = (block_info_t*)get_irn_link(block);
	if (bl_info->flags & BLOCK_HAS_EXC)
		return 0;

	phi_block = get_nodes_block(phi);
	if (! block_strictly_postdominates(phi_block, block))
		return 0;

	/* this is the address of the store */
	ptr  = get_Store_ptr(store);
	mode = get_irn_mode(get_Store_value(store));
	info = (ldst_info_t*)get_irn_link(store);
	exc  = info->exc_block;

	for (i = 1; i < n; ++i) {
		ir_node *pred = get_Phi_pred(phi, i);

		if (has_multiple_users(pred))
			return 0;

		pred = skip_Proj(pred);
		if (!is_Store(pred))
			return 0;

		if (ptr != get_Store_ptr(pred) || mode != get_irn_mode(get_Store_value(pred)))
			return 0;

		info = (ldst_info_t*)get_irn_link(pred);

		/* check, if all stores have the same exception flow */
		if (exc != info->exc_block)
			return 0;

		block = get_nodes_block(pred);

		/* check if the block is post dominated by Phi-block
		   and has no exception exit. Note that block must be different from
		   Phi-block, else we would move a Store from end End of a block to its
		   Start... */
		bl_info = (block_info_t*)get_irn_link(block);
		if (bl_info->flags & BLOCK_HAS_EXC)
			return 0;
		if (block == phi_block || ! block_postdominates(phi_block, block))
			return 0;
	}

	/*
	 * ok, when we are here, we found all predecessors of a Phi that
	 * are Stores to the same address and size. That means whatever
	 * we do before we enter the block of the Phi, we do a Store.
	 * So, we can move the Store to the current block:
	 *
	 *   val1    val2    val3          val1  val2  val3
	 *    |       |       |               \    |    /
	 * | Str | | Str | | Str |             \   |   /
	 *      \     |     /                   PhiData
	 *       \    |    /                       |
	 *        \   |   /                       Str
	 *           PhiM
	 *
	 * Is only allowed if the predecessor blocks have only one successor.
	 */

	NEW_ARR_A(ir_node *, projMs, n);
	NEW_ARR_A(ir_node *, inM, n);
	NEW_ARR_A(ir_node *, inD, n);
	NEW_ARR_A(int, idx, n);

	/* Prepare: Collect all Store nodes.  We must do this
	   first because we otherwise may loose a store when exchanging its
	   memory Proj.
	 */
	for (i = n - 1; i >= 0; --i) {
		projMs[i] = get_Phi_pred(phi, i);

		ir_node *const store = get_Proj_pred(projMs[i]);
		info  = (ldst_info_t*)get_irn_link(store);

		inM[i] = get_Store_mem(store);
		inD[i] = get_Store_value(store);
		idx[i] = info->exc_idx;
	}
	block = get_nodes_block(phi);

	/* second step: create a new memory Phi */
	phiM = new_rd_Phi(get_irn_dbg_info(phi), block, n, inM, mode_M);

	/* third step: create a new data Phi */
	phiD = new_rd_Phi(get_irn_dbg_info(phi), block, n, inD, mode);

	/* rewire memory and kill the node */
	for (i = n - 1; i >= 0; --i) {
		ir_node *proj  = projMs[i];

		if (is_Proj(proj)) {
			ir_node *store = get_Proj_pred(proj);
			exchange(proj, inM[i]);
			kill_node(store);
		}
	}

	/* fourth step: create the Store */
	store = new_rd_Store(db, block, phiM, ptr, phiD, cons_none);
#ifdef DO_CACHEOPT
	co_set_irn_name(store, co_get_irn_ident(old_store));
#endif

	projM = new_rd_Proj(NULL, store, mode_M, pn_Store_M);

	info = get_ldst_info(store, &wenv->obst);
	info->projs[pn_Store_M] = projM;

	/* fifths step: repair exception flow */
	if (exc) {
		ir_node *projX = new_rd_Proj(NULL, store, mode_X, pn_Store_X_except);

		info->projs[pn_Store_X_except] = projX;
		info->exc_block                = exc;
		info->exc_idx                  = idx[0];

		for (i = 0; i < n; ++i) {
			set_Block_cfgpred(exc, idx[i], projX);
		}

		if (n > 1) {
			/* the exception block should be optimized as some inputs are identical now */
		}

		res |= CF_CHANGED;
	}

	/* sixth step: replace old Phi */
	exchange(phi, projM);

	return res | DF_CHANGED;
}

static int optimize_conv_load(ir_node *conv)
{
	ir_node *op = get_Conv_op(conv);
	if (!is_Proj(op))
		return 0;
	if (has_multiple_users(op))
		return 0;
	/* shrink mode of load if possible. */
	ir_node *load = get_Proj_pred(op);
	if (!is_Load(load))
		return 0;

	/* only do it if we are the only user (otherwise the risk is too
	 * great that we end up with 2 loads instead of one). */
	ir_mode *mode      = get_irn_mode(conv);
	ir_mode *load_mode = get_Load_mode(load);
	int      bits_diff
		= get_mode_size_bits(load_mode) - get_mode_size_bits(mode);
	if (mode_is_float(load_mode) || mode_is_float(mode) || bits_diff < 0)
	    return 0;

	if (be_get_backend_param()->byte_order_big_endian) {
		if (bits_diff % 8 != 0)
			return 0;
		ir_graph *irg   = get_irn_irg(conv);
		ir_node  *ptr   = get_Load_ptr(load);
		ir_mode  *mode  = get_irn_mode(ptr);
		ir_node  *delta = new_r_Const_long(irg, mode, bits_diff/8);
		ir_node  *block = get_nodes_block(load);
		ir_node  *add   = new_r_Add(block, ptr, delta, mode);
		set_Load_ptr(load, add);
	}
	set_Load_mode(load, mode);
	set_irn_mode(op, mode);
	exchange(conv, op);
	return DF_CHANGED;
}

/**
 * walker, do the optimizations
 */
static void do_load_store_optimize(ir_node *n, void *env)
{
	walk_env_t *wenv = (walk_env_t*)env;

	switch (get_irn_opcode(n)) {

	case iro_Load:
		wenv->changes |= optimize_load(n);
		break;

	case iro_Store:
		wenv->changes |= optimize_store(n);
		break;

	case iro_Phi:
		wenv->changes |= optimize_phi(n, wenv);
		break;

	case iro_Conv:
		wenv->changes |= optimize_conv_load(n);
		break;

	default:
		break;
	}
}

/** A scc. */
typedef struct scc {
	ir_node *head;      /**< the head of the list */
} scc;

/** A node entry. */
typedef struct node_entry {
	unsigned DFSnum;    /**< the DFS number of this node */
	unsigned low;       /**< the low number of this node */
	int      in_stack;  /**< flag, set if the node is on the stack */
	ir_node  *next;     /**< link to the next node the the same scc */
	scc      *pscc;     /**< the scc of this node */
	unsigned POnum;     /**< the post order number for blocks */
} node_entry;

/** A loop entry. */
typedef struct loop_env {
	ir_nodehashmap_t map;
	struct obstack   obst;
	ir_node          **stack;      /**< the node stack */
	size_t           tos;          /**< tos index */
	unsigned         nextDFSnum;   /**< the current DFS number */
	unsigned         POnum;        /**< current post order number */

	unsigned         changes;      /**< a bitmask of graph changes */
} loop_env;

/**
* Gets the node_entry of a node
*/
static node_entry *get_irn_ne(ir_node *irn, loop_env *env)
{
	node_entry *e = ir_nodehashmap_get(node_entry, &env->map, irn);

	if (e == NULL) {
		e = OALLOC(&env->obst, node_entry);
		memset(e, 0, sizeof(*e));
		ir_nodehashmap_insert(&env->map, irn, e);
	}
	return e;
}

/**
 * Push a node onto the stack.
 *
 * @param env   the loop environment
 * @param n     the node to push
 */
static void push(loop_env *env, ir_node *n)
{
	node_entry *e;

	if (env->tos == ARR_LEN(env->stack)) {
		size_t nlen = ARR_LEN(env->stack) * 2;
		ARR_RESIZE(ir_node *, env->stack, nlen);
	}
	env->stack[env->tos++] = n;
	e = get_irn_ne(n, env);
	e->in_stack = 1;
}

/**
 * pop a node from the stack
 *
 * @param env   the loop environment
 *
 * @return  The topmost node
 */
static ir_node *pop(loop_env *env)
{
	ir_node *n = env->stack[--env->tos];
	node_entry *e = get_irn_ne(n, env);

	e->in_stack = 0;
	return n;
}

/**
 * Check if irn is a region constant.
 * The block or irn must strictly dominate the header block.
 *
 * @param irn           the node to check
 * @param header_block  the header block of the induction variable
 */
static int is_rc(ir_node *irn, ir_node *header_block)
{
	ir_node *block = get_nodes_block(irn);

	return (block != header_block) && block_dominates(block, header_block);
}

typedef struct phi_entry phi_entry;
struct phi_entry {
	ir_node   *phi;    /**< A phi with a region const memory. */
	int       pos;     /**< The position of the region const memory */
	ir_node   *load;   /**< the newly created load for this phi */
	phi_entry *next;
};

/**
 * An entry in the avail set.
 */
typedef struct avail_entry_t {
	ir_node *ptr;   /**< the address pointer */
	ir_mode *mode;  /**< the load mode */
	ir_node *load;  /**< the associated Load */
} avail_entry_t;

/**
 * Compare two avail entries.
 */
static int cmp_avail_entry(const void *elt, const void *key, size_t size)
{
	const avail_entry_t *a = (const avail_entry_t*)elt;
	const avail_entry_t *b = (const avail_entry_t*)key;
	(void) size;

	return a->ptr != b->ptr || a->mode != b->mode;
}

/**
 * Calculate the hash value of an avail entry.
 */
static unsigned hash_cache_entry(const avail_entry_t *entry)
{
	return get_irn_idx(entry->ptr) * 9 + hash_ptr(entry->mode);
}

/**
 * Move loops out of loops if possible.
 *
 * @param pscc   the loop described by an SCC
 * @param env    the loop environment
 */
static void move_loads_out_of_loops(scc *pscc, loop_env *env)
{
	ir_node   *phi, *load, *next, *other, *next_other;
	int       j;
	phi_entry *phi_list = NULL;
	set       *avail;

	/* collect all outer memories */
	for (phi = pscc->head; phi != NULL; phi = next) {
		node_entry *ne = get_irn_ne(phi, env);
		next = ne->next;

		/* check all memory Phi's */
		if (! is_Phi(phi))
			continue;

		assert(get_irn_mode(phi) == mode_M && "DFS return non-memory Phi");

		for (j = get_irn_arity(phi) - 1; j >= 0; --j) {
			ir_node    *pred = get_irn_n(phi, j);
			node_entry *pe   = get_irn_ne(pred, env);

			if (pe->pscc != ne->pscc) {
				/* not in the same SCC, is region const */
				phi_entry *pe = OALLOC(&env->obst, phi_entry);

				pe->phi  = phi;
				pe->pos  = j;
				pe->next = phi_list;
				phi_list = pe;
			}
		}
	}
	/* no Phis no fun */
	assert(phi_list != NULL && "DFS found a loop without Phi");

	/* for now, we cannot handle more than one input (only reducible cf) */
	if (phi_list->next != NULL)
		return;

	avail = new_set(cmp_avail_entry, 8);

	for (load = pscc->head; load; load = next) {
		ir_mode *load_mode;
		node_entry *ne = get_irn_ne(load, env);
		next = ne->next;

		if (is_Load(load)) {
			ldst_info_t *info = (ldst_info_t*)get_irn_link(load);
			ir_node     *ptr = get_Load_ptr(load);

			/* for now, we cannot handle Loads with exceptions */
			if (info->projs[pn_Load_res] == NULL || info->projs[pn_Load_X_regular] != NULL || info->projs[pn_Load_X_except] != NULL)
				continue;

			/* for now, we can only move Load(Global) */
			if (! is_SymConst_addr_ent(ptr))
				continue;
			load_mode = get_Load_mode(load);
			for (other = pscc->head; other != NULL; other = next_other) {
				node_entry *ne = get_irn_ne(other, env);
				next_other = ne->next;

				if (is_Store(other)) {
					ir_alias_relation rel = get_alias_relation(
						get_Store_ptr(other),
						get_irn_mode(get_Store_value(other)),
						ptr, load_mode);
					/* if the might be an alias, we cannot pass this Store */
					if (rel != ir_no_alias)
						break;
				}
				/* only Phis and pure Calls are allowed here, so ignore them */
			}
			if (other == NULL) {
				ldst_info_t *ninfo = NULL;
				phi_entry   *pe;
				dbg_info    *db;

				/* yep, no aliasing Store found, Load can be moved */
				DB((dbg, LEVEL_1, "  Found a Load that could be moved: %+F\n", load));

				db   = get_irn_dbg_info(load);
				for (pe = phi_list; pe != NULL; pe = pe->next) {
					int     pos   = pe->pos;
					ir_node *phi  = pe->phi;
					ir_node *blk  = get_nodes_block(phi);
					ir_node *pred = get_Block_cfgpred_block(blk, pos);
					ir_node *irn, *mem;
					avail_entry_t entry, *res;

					entry.ptr  = ptr;
					entry.mode = load_mode;
					res = set_find(avail_entry_t, avail, &entry, sizeof(entry), hash_cache_entry(&entry));
					if (res != NULL) {
						irn = res->load;
					} else {
						irn = new_rd_Load(db, pred, get_Phi_pred(phi, pos), ptr, load_mode, cons_none);
						entry.load = irn;
						(void)set_insert(avail_entry_t, avail, &entry, sizeof(entry), hash_cache_entry(&entry));
						DB((dbg, LEVEL_1, "  Created %+F in %+F\n", irn, pred));
					}
					pe->load = irn;
					ninfo = get_ldst_info(irn, &env->obst);

					ninfo->projs[pn_Load_M] = mem = new_r_Proj(irn, mode_M, pn_Load_M);
					if (res == NULL) {
						/* irn is from cache, so do not set phi pred again.
						 * There might be other Loads between phi and irn already.
						 */
						set_Phi_pred(phi, pos, mem);
					}

					ninfo->projs[pn_Load_res] = new_r_Proj(irn, load_mode, pn_Load_res);
				}

				/* now kill the old Load */
				exchange(info->projs[pn_Load_M], get_Load_mem(load));
				exchange(info->projs[pn_Load_res], ninfo->projs[pn_Load_res]);

				env->changes |= DF_CHANGED;
			}
		}
	}
	del_set(avail);
}

/**
 * Process a loop SCC.
 *
 * @param pscc  the SCC
 * @param env   the loop environment
 */
static void process_loop(scc *pscc, loop_env *env)
{
	ir_node *irn, *next, *header = NULL;
	node_entry *b, *h = NULL;
	int j, only_phi, num_outside, process = 0;
	ir_node *out_rc;

	/* find the header block for this scc */
	for (irn = pscc->head; irn; irn = next) {
		node_entry *e = get_irn_ne(irn, env);
		ir_node *block = get_nodes_block(irn);

		next = e->next;
		b = get_irn_ne(block, env);

		if (header != NULL) {
			if (h->POnum < b->POnum) {
				header = block;
				h      = b;
			}
		} else {
			header = block;
			h      = b;
		}
	}

	/* check if this scc contains only Phi, Loads or Stores nodes */
	only_phi    = 1;
	num_outside = 0;
	out_rc      = NULL;
	for (irn = pscc->head; irn; irn = next) {
		node_entry *e = get_irn_ne(irn, env);

		next = e->next;
		switch (get_irn_opcode(irn)) {
		case iro_Call:
			if (is_Call_pure(irn)) {
				/* pure calls can be treated like loads */
				only_phi = 0;
				break;
			}
			/* non-pure calls must be handle like may-alias Stores */
			goto fail;
		case iro_CopyB:
			/* cannot handle CopyB yet */
			goto fail;
		case iro_Load:
			process = 1;
			if (get_Load_volatility(irn) == volatility_is_volatile) {
				/* cannot handle loops with volatile Loads */
				goto fail;
			}
			only_phi = 0;
			break;
		case iro_Store:
			if (get_Store_volatility(irn) == volatility_is_volatile) {
				/* cannot handle loops with volatile Stores */
				goto fail;
			}
			only_phi = 0;
			break;
		default:
			only_phi = 0;
			break;
		case iro_Phi:
			for (j = get_irn_arity(irn) - 1; j >= 0; --j) {
				ir_node *pred  = get_irn_n(irn, j);
				node_entry *pe = get_irn_ne(pred, env);

				if (pe->pscc != e->pscc) {
					/* not in the same SCC, must be a region const */
					if (! is_rc(pred, header)) {
						/* not a memory loop */
						goto fail;
					}
					if (out_rc == NULL) {
						/* first region constant */
						out_rc = pred;
						++num_outside;
					} else if (out_rc != pred) {
						/* another region constant */
						++num_outside;
					}
				}
			}
			break;
		}
	}
	if (! process)
		goto fail;

	/* found a memory loop */
	DB((dbg, LEVEL_2, "  Found a memory loop:\n  "));
	if (only_phi && num_outside == 1) {
		/* a phi cycle with only one real predecessor can be collapsed */
		DB((dbg, LEVEL_2, "  Found an USELESS Phi cycle:\n  "));

		for (irn = pscc->head; irn; irn = next) {
			node_entry *e = get_irn_ne(irn, env);
			next = e->next;
			exchange(irn, out_rc);
		}
		env->changes |= DF_CHANGED;
		return;
	}

#ifdef DEBUG_libfirm
	for (irn = pscc->head; irn; irn = next) {
		node_entry *e = get_irn_ne(irn, env);
		next = e->next;
		DB((dbg, LEVEL_2, " %+F,", irn));
	}
	DB((dbg, LEVEL_2, "\n"));
#endif
	move_loads_out_of_loops(pscc, env);

fail:
	;
}

/**
 * Process a SCC.
 *
 * @param pscc  the SCC
 * @param env   the loop environment
 */
static void process_scc(scc *pscc, loop_env *env)
{
	ir_node *head = pscc->head;
	node_entry *e = get_irn_ne(head, env);

#ifdef DEBUG_libfirm
	{
		ir_node *irn, *next;

		DB((dbg, LEVEL_4, " SCC at %p:\n ", pscc));
		for (irn = pscc->head; irn; irn = next) {
			node_entry *e = get_irn_ne(irn, env);

			next = e->next;

			DB((dbg, LEVEL_4, " %+F,", irn));
		}
		DB((dbg, LEVEL_4, "\n"));
	}
#endif

	if (e->next != NULL) {
		/* this SCC has more than one member */
		process_loop(pscc, env);
	}
}

/**
 * Do Tarjan's SCC algorithm and drive load/store optimization.
 *
 * @param irn  start at this node
 * @param env  the loop environment
 */
static void dfs(ir_node *irn, loop_env *env)
{
	int i, n;
	node_entry *node = get_irn_ne(irn, env);

	mark_irn_visited(irn);

	node->DFSnum = env->nextDFSnum++;
	node->low    = node->DFSnum;
	push(env, irn);

	/* handle preds */
	if (is_Phi(irn) || is_Sync(irn)) {
		n = get_irn_arity(irn);
		for (i = 0; i < n; ++i) {
			ir_node *pred = get_irn_n(irn, i);
			node_entry *o = get_irn_ne(pred, env);

			if (!irn_visited(pred)) {
				dfs(pred, env);
				node->low = MIN(node->low, o->low);
			}
			if (o->DFSnum < node->DFSnum && o->in_stack)
				node->low = MIN(o->DFSnum, node->low);
		}
	} else if (is_fragile_op(irn)) {
		ir_node *pred = get_memop_mem(irn);
		node_entry *o = get_irn_ne(pred, env);

		if (!irn_visited(pred)) {
			dfs(pred, env);
			node->low = MIN(node->low, o->low);
		}
		if (o->DFSnum < node->DFSnum && o->in_stack)
			node->low = MIN(o->DFSnum, node->low);
	} else if (is_Proj(irn)) {
		ir_node *pred = get_Proj_pred(irn);
		node_entry *o = get_irn_ne(pred, env);

		if (!irn_visited(pred)) {
			dfs(pred, env);
			node->low = MIN(node->low, o->low);
		}
		if (o->DFSnum < node->DFSnum && o->in_stack)
			node->low = MIN(o->DFSnum, node->low);
	}
	else {
		 /* IGNORE predecessors */
	}

	if (node->low == node->DFSnum) {
		scc *pscc = OALLOC(&env->obst, scc);
		ir_node *x;

		pscc->head = NULL;
		do {
			node_entry *e;

			x = pop(env);
			e = get_irn_ne(x, env);
			e->pscc    = pscc;
			e->next    = pscc->head;
			pscc->head = x;
		} while (x != irn);

		process_scc(pscc, env);
	}
}

/**
 * Do the DFS on the memory edges a graph.
 *
 * @param irg  the graph to process
 * @param env  the loop environment
 */
static void do_dfs(ir_graph *irg, loop_env *env)
{
	ir_node  *endblk, *end;
	int      i;

	inc_irg_visited(irg);

	/* visit all memory nodes */
	endblk = get_irg_end_block(irg);
	for (i = get_Block_n_cfgpreds(endblk) - 1; i >= 0; --i) {
		ir_node *pred = get_Block_cfgpred(endblk, i);

		pred = skip_Proj(pred);
		if (is_Return(pred)) {
			dfs(get_Return_mem(pred), env);
		} else if (is_Raise(pred)) {
			dfs(get_Raise_mem(pred), env);
		} else if (is_fragile_op(pred)) {
			dfs(get_memop_mem(pred), env);
		} else if (is_Bad(pred)) {
			/* ignore non-optimized block predecessor */
		} else {
			assert(0 && "Unknown EndBlock predecessor");
		}
	}

	/* visit the keep-alives */
	end = get_irg_end(irg);
	for (i = get_End_n_keepalives(end) - 1; i >= 0; --i) {
		ir_node *ka = get_End_keepalive(end, i);

		if (is_Phi(ka) && !irn_visited(ka))
			dfs(ka, env);
	}
}

/**
 * Optimize Loads/Stores in loops.
 *
 * @param irg  the graph
 */
static int optimize_loops(ir_graph *irg)
{
	loop_env env;

	env.stack         = NEW_ARR_F(ir_node *, 128);
	env.tos           = 0;
	env.nextDFSnum    = 0;
	env.POnum         = 0;
	env.changes       = 0;
	ir_nodehashmap_init(&env.map);
	obstack_init(&env.obst);

	/* calculate the SCC's and drive loop optimization. */
	do_dfs(irg, &env);

	DEL_ARR_F(env.stack);
	obstack_free(&env.obst, NULL);
	ir_nodehashmap_destroy(&env.map);

	return env.changes;
}

void optimize_load_store(ir_graph *irg)
{
	walk_env_t env;

	assure_irg_properties(irg,
		IR_GRAPH_PROPERTY_NO_UNREACHABLE_CODE
		| IR_GRAPH_PROPERTY_CONSISTENT_OUT_EDGES
		| IR_GRAPH_PROPERTY_NO_CRITICAL_EDGES
		| IR_GRAPH_PROPERTY_CONSISTENT_DOMINANCE
		| IR_GRAPH_PROPERTY_CONSISTENT_ENTITY_USAGE);

	FIRM_DBG_REGISTER(dbg, "firm.opt.ldstopt");

	assert(get_irg_pinned(irg) != op_pin_state_floats &&
		"LoadStore optimization needs pinned graph");

	if (get_opt_alias_analysis()) {
		assure_irp_globals_entity_usage_computed();
	}

	obstack_init(&env.obst);
	env.changes = 0;

	/* init the links, then collect Loads/Stores/Proj's in lists */
	master_visited = 0;
	irg_walk_graph(irg, firm_clear_link, collect_nodes, &env);

	/* now we have collected enough information, optimize */
	irg_walk_graph(irg, NULL, do_load_store_optimize, &env);

	env.changes |= optimize_loops(irg);

	obstack_free(&env.obst, NULL);

	confirm_irg_properties(irg,
		env.changes
		? env.changes & CF_CHANGED
			? IR_GRAPH_PROPERTIES_NONE
			: IR_GRAPH_PROPERTIES_CONTROL_FLOW
		: IR_GRAPH_PROPERTIES_ALL);
}

ir_graph_pass_t *optimize_load_store_pass(const char *name)
{
	return def_graph_pass(name ? name : "ldst", optimize_load_store);
}
