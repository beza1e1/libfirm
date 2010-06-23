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
 * @brief       Backend ABI implementation.
 * @author      Sebastian Hack, Michael Beck
 * @version     $Id$
 */
#include "config.h"

#include "obst.h"

#include "irgopt.h"

#include "irgraph_t.h"
#include "irnode_t.h"
#include "ircons_t.h"
#include "iredges_t.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "irprintf_t.h"
#include "irgopt.h"
#include "irbitset.h"
#include "iropt_t.h"
#include "height.h"
#include "pdeq.h"
#include "irtools.h"
#include "raw_bitset.h"
#include "error.h"
#include "pset_new.h"

#include "be.h"
#include "beabi.h"
#include "bearch.h"
#include "benode.h"
#include "belive_t.h"
#include "besched.h"
#include "beirg.h"
#include "bessaconstr.h"
#include "bemodule.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg;)

typedef struct _be_abi_call_arg_t {
	unsigned is_res   : 1;  /**< 1: the call argument is a return value. 0: it's a call parameter. */
	unsigned in_reg   : 1;  /**< 1: this argument is transmitted in registers. */
	unsigned on_stack : 1;	/**< 1: this argument is transmitted on the stack. */
	unsigned callee   : 1;  /**< 1: someone called us. 0: We call another function */

	int                    pos;
	const arch_register_t *reg;
	ir_entity             *stack_ent;
	ir_mode               *load_mode;
	unsigned               alignment;    /**< stack alignment */
	unsigned               space_before; /**< allocate space before */
	unsigned               space_after;  /**< allocate space after */
} be_abi_call_arg_t;

struct _be_abi_call_t {
	be_abi_call_flags_t          flags;  /**< Flags describing the ABI behavior on calls */
	int                          pop;    /**< number of bytes the stack frame is shrinked by the callee on return. */
	const be_abi_callbacks_t    *cb;
	ir_type                     *between_type;
	set                         *params;
	const arch_register_class_t *cls_addr; /**< register class of the call address */
};

/**
 * The ABI information for the current birg.
 */
struct _be_abi_irg_t {
	be_irg_t             *birg;         /**< The back end IRG. */
	ir_graph             *irg;
	const arch_env_t     *arch_env;
	survive_dce_t        *dce_survivor;

	be_abi_call_t        *call;         /**< The ABI call information. */
	ir_type              *method_type;  /**< The type of the method of the IRG. */

	ir_node              *init_sp;      /**< The node representing the stack pointer
	                                         at the start of the function. */

	ir_node              *start;        /**< The be_Start params node. */
	pmap                 *regs;         /**< A map of all callee-save and ignore regs to
	                                         their Projs to the RegParams node. */

	int                  start_block_bias; /**< The stack bias at the end of the start block. */

	void                 *cb;           /**< ABI Callback self pointer. */

	pmap                 *keep_map;     /**< mapping blocks to keep nodes. */
	pset                 *ignore_regs;  /**< Additional registers which shall be ignored. */

	ir_node              **calls;       /**< flexible array containing all be_Call nodes */

	arch_register_req_t  *sp_req;

	be_stack_layout_t    frame;         /**< The stack frame model. */
};

static heights_t *ir_heights;

/** Flag: if set, try to omit the frame pointer in all routines. */
static int be_omit_fp = 1;

/** Flag: if set, try to omit the frame pointer in leaf routines only. */
static int be_omit_leaf_fp = 1;

/*
     _    ____ ___    ____      _ _ _                _
    / \  | __ )_ _|  / ___|__ _| | | |__   __ _  ___| | _____
   / _ \ |  _ \| |  | |   / _` | | | '_ \ / _` |/ __| |/ / __|
  / ___ \| |_) | |  | |__| (_| | | | |_) | (_| | (__|   <\__ \
 /_/   \_\____/___|  \____\__,_|_|_|_.__/ \__,_|\___|_|\_\___/

  These callbacks are used by the backend to set the parameters
  for a specific call type.
*/

/**
 * Set compare function: compares two ABI call object arguments.
 */
static int cmp_call_arg(const void *a, const void *b, size_t n)
{
	const be_abi_call_arg_t *p = a, *q = b;
	(void) n;
	return !(p->is_res == q->is_res && p->pos == q->pos && p->callee == q->callee);
}

/**
 * Get  an ABI call object argument.
 *
 * @param call      the abi call
 * @param is_res    true for call results, false for call arguments
 * @param pos       position of the argument
 * @param callee	context type - if we are callee or caller
 */
static be_abi_call_arg_t *get_call_arg(be_abi_call_t *call, int is_res, int pos, int callee)
{
	be_abi_call_arg_t arg;
	unsigned hash;

	memset(&arg, 0, sizeof(arg));
	arg.is_res = is_res;
	arg.pos    = pos;
	arg.callee = callee;

	hash = is_res * 128 + pos;

	return set_find(call->params, &arg, sizeof(arg), hash);
}

/**
 * Set an ABI call object argument.
 */
static void remember_call_arg(be_abi_call_arg_t *arg, be_abi_call_t *call, be_abi_context_t context)
{
	unsigned hash = arg->is_res * 128 + arg->pos;
	if (context & ABI_CONTEXT_CALLEE) {
		arg->callee = 1;
		set_insert(call->params, arg, sizeof(*arg), hash);
	}
	if (context & ABI_CONTEXT_CALLER) {
		arg->callee = 0;
		set_insert(call->params, arg, sizeof(*arg), hash);
	}
}

/* Set the flags for a call. */
void be_abi_call_set_flags(be_abi_call_t *call, be_abi_call_flags_t flags, const be_abi_callbacks_t *cb)
{
	call->flags = flags;
	call->cb    = cb;
}

/* Sets the number of bytes the stackframe is shrinked by the callee on return */
void be_abi_call_set_pop(be_abi_call_t *call, int pop)
{
	assert(pop >= 0);
	call->pop = pop;
}

/* Set register class for call address */
void be_abi_call_set_call_address_reg_class(be_abi_call_t *call, const arch_register_class_t *cls)
{
	call->cls_addr = cls;
}


void be_abi_call_param_stack(be_abi_call_t *call, int arg_pos,
                             ir_mode *load_mode, unsigned alignment,
                             unsigned space_before, unsigned space_after,
                             be_abi_context_t context)
{
	be_abi_call_arg_t arg;
	memset(&arg, 0, sizeof(arg));
	assert(alignment > 0 && "Alignment must be greater than 0");
	arg.on_stack     = 1;
	arg.load_mode    = load_mode;
	arg.alignment    = alignment;
	arg.space_before = space_before;
	arg.space_after  = space_after;
	arg.is_res       = 0;
	arg.pos          = arg_pos;

	remember_call_arg(&arg, call, context);
}

void be_abi_call_param_reg(be_abi_call_t *call, int arg_pos, const arch_register_t *reg, be_abi_context_t context)
{
	be_abi_call_arg_t arg;
	memset(&arg, 0, sizeof(arg));

	arg.in_reg = 1;
	arg.reg    = reg;
	arg.is_res = 0;
	arg.pos    = arg_pos;

	remember_call_arg(&arg, call, context);
}

void be_abi_call_res_reg(be_abi_call_t *call, int arg_pos, const arch_register_t *reg, be_abi_context_t context)
{
	be_abi_call_arg_t arg;
	memset(&arg, 0, sizeof(arg));

	arg.in_reg = 1;
	arg.reg    = reg;
	arg.is_res = 1;
	arg.pos    = arg_pos;

	remember_call_arg(&arg, call, context);
}

/* Get the flags of a ABI call object. */
be_abi_call_flags_t be_abi_call_get_flags(const be_abi_call_t *call)
{
	return call->flags;
}

/**
 * Constructor for a new ABI call object.
 *
 * @param cls_addr  register class of the call address
 *
 * @return the new ABI call object
 */
static be_abi_call_t *be_abi_call_new(const arch_register_class_t *cls_addr)
{
	be_abi_call_t *call = XMALLOCZ(be_abi_call_t);

	call->flags.val  = 0;
	call->params     = new_set(cmp_call_arg, 16);
	call->cb         = NULL;
	call->cls_addr   = cls_addr;

	call->flags.bits.try_omit_fp = be_omit_fp | be_omit_leaf_fp;

	return call;
}

/**
 * Destructor for an ABI call object.
 */
static void be_abi_call_free(be_abi_call_t *call)
{
	del_set(call->params);
	free(call);
}

/*
  _____                           _   _                 _ _ _
 |  ___| __ __ _ _ __ ___   ___  | | | | __ _ _ __   __| | (_)_ __   __ _
 | |_ | '__/ _` | '_ ` _ \ / _ \ | |_| |/ _` | '_ \ / _` | | | '_ \ / _` |
 |  _|| | | (_| | | | | | |  __/ |  _  | (_| | | | | (_| | | | | | | (_| |
 |_|  |_|  \__,_|_| |_| |_|\___| |_| |_|\__,_|_| |_|\__,_|_|_|_| |_|\__, |
                                                                    |___/

  Handling of the stack frame. It is composed of three types:
  1) The type of the arguments which are pushed on the stack.
  2) The "between type" which consists of stuff the call of the
     function pushes on the stack (like the return address and
	 the old base pointer for ia32).
  3) The Firm frame type which consists of all local variables
     and the spills.
*/

static int get_stack_entity_offset(be_stack_layout_t *frame, ir_entity *ent,
                                   int bias)
{
	ir_type *t = get_entity_owner(ent);
	int ofs    = get_entity_offset(ent);

	int index;

	/* Find the type the entity is contained in. */
	for (index = 0; index < N_FRAME_TYPES; ++index) {
		if (frame->order[index] == t)
			break;
		/* Add the size of all the types below the one of the entity to the entity's offset */
		ofs += get_type_size_bytes(frame->order[index]);
	}

	/* correct the offset by the initial position of the frame pointer */
	ofs -= frame->initial_offset;

	/* correct the offset with the current bias. */
	ofs += bias;

	return ofs;
}

/**
 * Retrieve the entity with given offset from a frame type.
 */
static ir_entity *search_ent_with_offset(ir_type *t, int offset)
{
	int i, n;

	for (i = 0, n = get_compound_n_members(t); i < n; ++i) {
		ir_entity *ent = get_compound_member(t, i);
		if (get_entity_offset(ent) == offset)
			return ent;
	}

	return NULL;
}

static int stack_frame_compute_initial_offset(be_stack_layout_t *frame)
{
	ir_type  *base = frame->stack_dir < 0 ? frame->between_type : frame->frame_type;
	ir_entity *ent = search_ent_with_offset(base, 0);

	if (ent == NULL) {
		frame->initial_offset
			= frame->stack_dir < 0 ? get_type_size_bytes(frame->frame_type) : get_type_size_bytes(frame->between_type);
	} else {
		frame->initial_offset = get_stack_entity_offset(frame, ent, 0);
	}

	return frame->initial_offset;
}

/**
 * Initializes the frame layout from parts
 *
 * @param frame     the stack layout that will be initialized
 * @param args      the stack argument layout type
 * @param between   the between layout type
 * @param locals    the method frame type
 * @param stack_dir the stack direction: < 0 decreasing, > 0 increasing addresses
 * @param param_map an array mapping method argument positions to the stack argument type
 *
 * @return the initialized stack layout
 */
static be_stack_layout_t *stack_frame_init(be_stack_layout_t *frame, ir_type *args,
                                           ir_type *between, ir_type *locals, int stack_dir,
                                           ir_entity *param_map[])
{
	frame->arg_type       = args;
	frame->between_type   = between;
	frame->frame_type     = locals;
	frame->initial_offset = 0;
	frame->initial_bias   = 0;
	frame->stack_dir      = stack_dir;
	frame->order[1]       = between;
	frame->param_map      = param_map;

	if (stack_dir > 0) {
		frame->order[0] = args;
		frame->order[2] = locals;
	}
	else {
		/* typical decreasing stack: locals have the
		 * lowest addresses, arguments the highest */
		frame->order[0] = locals;
		frame->order[2] = args;
	}
	return frame;
}

/*
   ____      _ _
  / ___|__ _| | |___
 | |   / _` | | / __|
 | |__| (_| | | \__ \
  \____\__,_|_|_|___/

  Adjustment of the calls inside a graph.

*/

/**
 * Transform a call node into a be_Call node.
 *
 * @param env The ABI environment for the current irg.
 * @param irn The call node.
 * @param curr_sp The stack pointer node to use.
 * @return The stack pointer after the call.
 */
static ir_node *adjust_call(be_abi_irg_t *env, ir_node *irn, ir_node *curr_sp)
{
	ir_graph *irg              = env->birg->irg;
	const arch_env_t *arch_env = env->birg->main_env->arch_env;
	ir_type *call_tp           = get_Call_type(irn);
	ir_node *call_ptr          = get_Call_ptr(irn);
	int n_params               = get_method_n_params(call_tp);
	ir_node *curr_mem          = get_Call_mem(irn);
	ir_node *bl                = get_nodes_block(irn);
	int stack_size             = 0;
	int stack_dir              = arch_env->stack_dir;
	const arch_register_t *sp  = arch_env->sp;
	be_abi_call_t *call        = be_abi_call_new(sp->reg_class);
	ir_mode *mach_mode         = sp->reg_class->mode;
	struct obstack *obst       = be_get_birg_obst(irg);
	int no_alloc               = call->flags.bits.frame_is_setup_on_call;
	int n_res                  = get_method_n_ress(call_tp);
	int do_seq                 = call->flags.bits.store_args_sequential && !no_alloc;

	ir_node *res_proj  = NULL;
	int n_reg_params   = 0;
	int n_stack_params = 0;
	int n_ins;

	pset_new_t              destroyed_regs, states;
	pset_new_iterator_t     iter;
	ir_node                *low_call;
	ir_node               **in;
	ir_node               **res_projs;
	int                     n_reg_results = 0;
	const arch_register_t  *reg;
	const ir_edge_t        *edge;
	int                    *reg_param_idxs;
	int                    *stack_param_idx;
	int                     i, n, destroy_all_regs;
	dbg_info               *dbgi;

	pset_new_init(&destroyed_regs);
	pset_new_init(&states);

	/* Let the isa fill out the abi description for that call node. */
	arch_env_get_call_abi(arch_env, call_tp, call);

	/* Insert code to put the stack arguments on the stack. */
	assert(get_Call_n_params(irn) == n_params);
	assert(obstack_object_size(obst) == 0);
	stack_param_idx = ALLOCAN(int, n_params);
	for (i = 0; i < n_params; ++i) {
		be_abi_call_arg_t *arg = get_call_arg(call, 0, i, 0);
		assert(arg);
		if (arg->on_stack) {
			int arg_size = get_type_size_bytes(get_method_param_type(call_tp, i));

			stack_size += round_up2(arg->space_before, arg->alignment);
			stack_size += round_up2(arg_size, arg->alignment);
			stack_size += round_up2(arg->space_after, arg->alignment);

			stack_param_idx[n_stack_params++] = i;
		}
	}

	/* Collect all arguments which are passed in registers. */
	reg_param_idxs = ALLOCAN(int, n_params);
	for (i = 0; i < n_params; ++i) {
		be_abi_call_arg_t *arg = get_call_arg(call, 0, i, 0);
		if (arg && arg->in_reg) {
			reg_param_idxs[n_reg_params++] = i;
		}
	}

	/*
	 * If the stack is decreasing and we do not want to store sequentially,
	 * or someone else allocated the call frame
	 * we allocate as much space on the stack all parameters need, by
	 * moving the stack pointer along the stack's direction.
	 *
	 * Note: we also have to do this for stack_size == 0, because we may have
	 * to adjust stack alignment for the call.
	 */
	if (stack_dir < 0 && !do_seq && !no_alloc) {
		curr_sp = be_new_IncSP(sp, bl, curr_sp, stack_size, 1);
	}

	dbgi = get_irn_dbg_info(irn);
	/* If there are some parameters which shall be passed on the stack. */
	if (n_stack_params > 0) {
		int       curr_ofs = 0;
		ir_node **in       = ALLOCAN(ir_node*, n_stack_params+1);
		unsigned  n_in     = 0;

		/*
		 * Reverse list of stack parameters if call arguments are from left to right.
		 * We must them reverse again if they are pushed (not stored) and the stack
		 * direction is downwards.
		 */
		if (call->flags.bits.left_to_right ^ (do_seq && stack_dir < 0)) {
			for (i = 0; i < n_stack_params >> 1; ++i) {
				int other  = n_stack_params - i - 1;
				int tmp    = stack_param_idx[i];
				stack_param_idx[i]     = stack_param_idx[other];
				stack_param_idx[other] = tmp;
			}
		}

		curr_mem = get_Call_mem(irn);
		if (! do_seq) {
			in[n_in++] = curr_mem;
		}

		for (i = 0; i < n_stack_params; ++i) {
			int p                  = stack_param_idx[i];
			be_abi_call_arg_t *arg = get_call_arg(call, 0, p, 0);
			ir_node *param         = get_Call_param(irn, p);
			ir_node *addr          = curr_sp;
			ir_node *mem           = NULL;
			ir_type *param_type    = get_method_param_type(call_tp, p);
			int param_size         = get_type_size_bytes(param_type) + arg->space_after;

			/*
			 * If we wanted to build the arguments sequentially,
			 * the stack pointer for the next must be incremented,
			 * and the memory value propagated.
			 */
			if (do_seq) {
				curr_ofs = 0;
				addr = curr_sp = be_new_IncSP(sp, bl, curr_sp,
				                              param_size + arg->space_before, 0);
				add_irn_dep(curr_sp, curr_mem);
			} else {
				curr_ofs += arg->space_before;
				curr_ofs =  round_up2(curr_ofs, arg->alignment);

				/* Make the expression to compute the argument's offset. */
				if (curr_ofs > 0) {
					ir_mode *constmode = mach_mode;
					if (mode_is_reference(mach_mode)) {
						constmode = mode_Is;
					}
					addr = new_r_Const_long(irg, constmode, curr_ofs);
					addr = new_r_Add(bl, curr_sp, addr, mach_mode);
				}
			}

			/* Insert a store for primitive arguments. */
			if (is_atomic_type(param_type)) {
				ir_node *store;
				ir_node *mem_input = do_seq ? curr_mem : new_NoMem();
				store = new_rd_Store(dbgi, bl, mem_input, addr, param, 0);
				mem   = new_r_Proj(store, mode_M, pn_Store_M);
			} else {
				/* Make a mem copy for compound arguments. */
				ir_node *copy;

				assert(mode_is_reference(get_irn_mode(param)));
				copy = new_rd_CopyB(dbgi, bl, curr_mem, addr, param, param_type);
				mem = new_r_Proj(copy, mode_M, pn_CopyB_M_regular);
			}

			curr_ofs += param_size;

			if (do_seq)
				curr_mem = mem;
			else
				in[n_in++] = mem;
		}

		/* We need the sync only, if we didn't build the stores sequentially. */
		if (! do_seq) {
			if (n_stack_params >= 1) {
				curr_mem = new_r_Sync(bl, n_in, in);
			} else {
				curr_mem = get_Call_mem(irn);
			}
		}
	}

	/* check for the return_twice property */
	destroy_all_regs = 0;
	if (is_SymConst_addr_ent(call_ptr)) {
		ir_entity *ent = get_SymConst_entity(call_ptr);

		if (get_entity_additional_properties(ent) & mtp_property_returns_twice)
			destroy_all_regs = 1;
	} else {
		ir_type *call_tp = get_Call_type(irn);

		if (get_method_additional_properties(call_tp) & mtp_property_returns_twice)
			destroy_all_regs = 1;
	}

	/* Put caller save into the destroyed set and state registers in the states set */
	for (i = 0, n = arch_env_get_n_reg_class(arch_env); i < n; ++i) {
		unsigned j;
		const arch_register_class_t *cls = arch_env_get_reg_class(arch_env, i);
		for (j = 0; j < cls->n_regs; ++j) {
			const arch_register_t *reg = arch_register_for_index(cls, j);

			if (destroy_all_regs || arch_register_type_is(reg, caller_save)) {
				if (! arch_register_type_is(reg, ignore))
					pset_new_insert(&destroyed_regs, (void *) reg);
			}
			if (arch_register_type_is(reg, state)) {
				pset_new_insert(&destroyed_regs, (void*) reg);
				pset_new_insert(&states, (void*) reg);
			}
		}
	}

	if (destroy_all_regs) {
		/* even if destroyed all is specified, neither SP nor FP are destroyed (else bad things will happen) */
		pset_new_remove(&destroyed_regs, arch_env->sp);
		pset_new_remove(&destroyed_regs, arch_env->bp);
	}

	/* search the largest result proj number */
	res_projs = ALLOCANZ(ir_node*, n_res);

	foreach_out_edge(irn, edge) {
		const ir_edge_t *res_edge;
		ir_node         *irn = get_edge_src_irn(edge);

		if (!is_Proj(irn) || get_Proj_proj(irn) != pn_Call_T_result)
			continue;

		foreach_out_edge(irn, res_edge) {
			int proj;
			ir_node *res = get_edge_src_irn(res_edge);

			assert(is_Proj(res));

			proj = get_Proj_proj(res);
			assert(proj < n_res);
			assert(res_projs[proj] == NULL);
			res_projs[proj] = res;
		}
		res_proj = irn;
		break;
	}

	/** TODO: this is not correct for cases where return values are passed
	 * on the stack, but no known ABI does this currently...
	 */
	n_reg_results = n_res;

	assert(obstack_object_size(obst) == 0);
	n_ins = 0;
	in    = ALLOCAN(ir_node*, n_reg_params + pset_new_size(&states));

	/* make the back end call node and set its register requirements. */
	for (i = 0; i < n_reg_params; ++i) {
		in[n_ins++] = get_Call_param(irn, reg_param_idxs[i]);
	}

	/* add state registers ins */
	foreach_pset_new(&states, reg, iter) {
		const arch_register_class_t *cls = arch_register_get_class(reg);
#if 0
		ir_node *regnode = be_abi_reg_map_get(env->regs, reg);
		ir_fprintf(stderr, "Adding %+F\n", regnode);
#endif
		ir_node *regnode = new_r_Unknown(irg, arch_register_class_mode(cls));
		in[n_ins++]      = regnode;
	}
	assert(n_ins == (int) (n_reg_params + pset_new_size(&states)));

	/* ins collected, build the call */
	if (env->call->flags.bits.call_has_imm && is_SymConst(call_ptr)) {
		/* direct call */
		low_call = be_new_Call(dbgi, irg, bl, curr_mem, curr_sp, curr_sp,
		                       n_reg_results + pn_be_Call_first_res + pset_new_size(&destroyed_regs),
		                       n_ins, in, get_Call_type(irn));
		be_Call_set_entity(low_call, get_SymConst_entity(call_ptr));
	} else {
		/* indirect call */
		low_call = be_new_Call(dbgi, irg, bl, curr_mem, curr_sp, call_ptr,
		                       n_reg_results + pn_be_Call_first_res + pset_new_size(&destroyed_regs),
		                       n_ins, in, get_Call_type(irn));
	}
	be_Call_set_pop(low_call, call->pop);

	/* put the call into the list of all calls for later processing */
	ARR_APP1(ir_node *, env->calls, low_call);

	/* create new stack pointer */
	curr_sp = new_r_Proj(low_call, get_irn_mode(curr_sp), pn_be_Call_sp);
	be_set_constr_single_reg_out(low_call, pn_be_Call_sp, sp,
			arch_register_req_type_ignore | arch_register_req_type_produces_sp);
	arch_set_irn_register(curr_sp, sp);

	/* now handle results */
	for (i = 0; i < n_res; ++i) {
		int pn;
		ir_node           *proj = res_projs[i];
		be_abi_call_arg_t *arg  = get_call_arg(call, 1, i, 0);

		/* returns values on stack not supported yet */
		assert(arg->in_reg);

		/*
			shift the proj number to the right, since we will drop the
			unspeakable Proj_T from the Call. Therefore, all real argument
			Proj numbers must be increased by pn_be_Call_first_res
		*/
		pn = i + pn_be_Call_first_res;

		if (proj == NULL) {
			ir_type *res_type = get_method_res_type(call_tp, i);
			ir_mode *mode     = get_type_mode(res_type);
			proj              = new_r_Proj(low_call, mode, pn);
			res_projs[i]      = proj;
		} else {
			set_Proj_pred(proj, low_call);
			set_Proj_proj(proj, pn);
		}

		if (arg->in_reg) {
			pset_new_remove(&destroyed_regs, arg->reg);
		}
	}

	/*
		Set the register class of the call address to
		the backend provided class (default: stack pointer class)
	*/
	be_node_set_reg_class_in(low_call, be_pos_Call_ptr, call->cls_addr);

	DBG((dbg, LEVEL_3, "\tcreated backend call %+F\n", low_call));

	/* Set the register classes and constraints of the Call parameters. */
	for (i = 0; i < n_reg_params; ++i) {
		int index = reg_param_idxs[i];
		be_abi_call_arg_t *arg = get_call_arg(call, 0, index, 0);
		assert(arg->reg != NULL);

		be_set_constr_single_reg_in(low_call, be_pos_Call_first_arg + i,
		                            arg->reg, 0);
	}

	/* Set the register constraints of the results. */
	for (i = 0; i < n_res; ++i) {
		ir_node                 *proj = res_projs[i];
		const be_abi_call_arg_t *arg  = get_call_arg(call, 1, i, 0);
		int                      pn   = get_Proj_proj(proj);

		assert(arg->in_reg);
		be_set_constr_single_reg_out(low_call, pn, arg->reg, 0);
		arch_set_irn_register(proj, arg->reg);
	}
	exchange(irn, low_call);

	/* kill the ProjT node */
	if (res_proj != NULL) {
		kill_node(res_proj);
	}

	/* Make additional projs for the caller save registers
	   and the Keep node which keeps them alive. */
	{
		const arch_register_t *reg;
		ir_node               **in, *keep;
		int                   i;
		int                   n = 0;
		int                   curr_res_proj = pn_be_Call_first_res + n_reg_results;
		pset_new_iterator_t   iter;
		int                   n_ins;

		n_ins = (int)pset_new_size(&destroyed_regs) + n_reg_results + 1;
		in    = ALLOCAN(ir_node *, n_ins);

		/* also keep the stack pointer */
		set_irn_link(curr_sp, (void*) sp);
		in[n++] = curr_sp;

		foreach_pset_new(&destroyed_regs, reg, iter) {
			ir_node *proj = new_r_Proj(low_call, reg->reg_class->mode, curr_res_proj);

			/* memorize the register in the link field. we need afterwards to set the register class of the keep correctly. */
			be_set_constr_single_reg_out(low_call, curr_res_proj, reg, 0);
			arch_set_irn_register(proj, reg);

			set_irn_link(proj, (void*) reg);
			in[n++] = proj;
			++curr_res_proj;
		}

		for (i = 0; i < n_reg_results; ++i) {
			ir_node *proj = res_projs[i];
			const arch_register_t *reg = arch_get_irn_register(proj);
			set_irn_link(proj, (void*) reg);
			in[n++] = proj;
		}
		assert(n <= n_ins);

		/* create the Keep for the caller save registers */
		keep = be_new_Keep(bl, n, in);
		for (i = 0; i < n; ++i) {
			const arch_register_t *reg = get_irn_link(in[i]);
			be_node_set_reg_class_in(keep, i, reg->reg_class);
		}
	}

	/* Clean up the stack. */
	assert(stack_size >= call->pop);
	stack_size -= call->pop;

	if (stack_size > 0) {
		ir_node *mem_proj = NULL;

		foreach_out_edge(low_call, edge) {
			ir_node *irn = get_edge_src_irn(edge);
			if (is_Proj(irn) && get_Proj_proj(irn) == pn_Call_M) {
				mem_proj = irn;
				break;
			}
		}

		if (! mem_proj) {
			mem_proj = new_r_Proj(low_call, mode_M, pn_be_Call_M_regular);
			keep_alive(mem_proj);
		}
	}
	/* Clean up the stack frame or revert alignment fixes if we allocated it */
	if (! no_alloc) {
		curr_sp = be_new_IncSP(sp, bl, curr_sp, -stack_size, 0);
	}

	be_abi_call_free(call);

	pset_new_destroy(&states);
	pset_new_destroy(&destroyed_regs);

	return curr_sp;
}

/**
 * Adjust the size of a node representing a stack alloc or free for the minimum stack alignment.
 *
 * @param alignment  the minimum stack alignment
 * @param size       the node containing the non-aligned size
 * @param block      the block where new nodes are allocated on
 * @param dbg        debug info for new nodes
 *
 * @return a node representing the aligned size
 */
static ir_node *adjust_alloc_size(unsigned stack_alignment, ir_node *size,
                                  ir_node *block, dbg_info *dbg)
{
	if (stack_alignment > 1) {
		ir_mode  *mode;
		tarval   *tv;
		ir_node  *mask;
		ir_graph *irg;

		assert(is_po2(stack_alignment));

		mode = get_irn_mode(size);
		tv   = new_tarval_from_long(stack_alignment-1, mode);
		irg  = get_Block_irg(block);
		mask = new_r_Const(irg, tv);
		size = new_rd_Add(dbg, block, size, mask, mode);

		tv   = new_tarval_from_long(-(long)stack_alignment, mode);
		mask = new_r_Const(irg, tv);
		size = new_rd_And(dbg, block, size, mask, mode);
	}
	return size;
}
/**
 * Adjust an alloca.
 * The alloca is transformed into a back end alloca node and connected to the stack nodes.
 */
static ir_node *adjust_alloc(be_abi_irg_t *env, ir_node *alloc, ir_node *curr_sp)
{
	ir_node *block;
	ir_graph *irg;
	ir_node *alloc_mem;
	ir_node *alloc_res;
	ir_type *type;
	dbg_info *dbg;

	const ir_edge_t *edge;
	ir_node *new_alloc;
	ir_node *count;
	ir_node *size;
	ir_node *ins[2];
	unsigned stack_alignment;

	assert(get_Alloc_where(alloc) == stack_alloc);

	block = get_nodes_block(alloc);
	irg   = get_Block_irg(block);
	alloc_mem = NULL;
	alloc_res = NULL;
	type = get_Alloc_type(alloc);

	foreach_out_edge(alloc, edge) {
		ir_node *irn = get_edge_src_irn(edge);

		assert(is_Proj(irn));
		switch (get_Proj_proj(irn)) {
		case pn_Alloc_M:
			alloc_mem = irn;
			break;
		case pn_Alloc_res:
			alloc_res = irn;
			break;
		default:
			break;
		}
	}

	/* Beware: currently Alloc nodes without a result might happen,
	   only escape analysis kills them and this phase runs only for object
	   oriented source. We kill the Alloc here. */
	if (alloc_res == NULL && alloc_mem) {
		exchange(alloc_mem, get_Alloc_mem(alloc));
		return curr_sp;
	}

	dbg   = get_irn_dbg_info(alloc);
	count = get_Alloc_count(alloc);

	/* we might need to multiply the count with the element size */
	if (type != firm_unknown_type && get_type_size_bytes(type) != 1) {
		ir_mode *mode = get_irn_mode(count);
		tarval *tv    = new_tarval_from_long(get_type_size_bytes(type),
		                                     mode);
		ir_node *cnst = new_rd_Const(dbg, irg, tv);
		size          = new_rd_Mul(dbg, block, count, cnst, mode);
	} else {
		size = count;
	}

	/* The stack pointer will be modified in an unknown manner.
	   We cannot omit it. */
	env->call->flags.bits.try_omit_fp = 0;

	stack_alignment = 1 << env->arch_env->stack_alignment;
	size            = adjust_alloc_size(stack_alignment, size, block, dbg);
	new_alloc       = be_new_AddSP(env->arch_env->sp, block, curr_sp, size);
	set_irn_dbg_info(new_alloc, dbg);

	if (alloc_mem != NULL) {
		ir_node *addsp_mem;
		ir_node *sync;

		addsp_mem = new_r_Proj(new_alloc, mode_M, pn_be_AddSP_M);

		/* We need to sync the output mem of the AddSP with the input mem
		   edge into the alloc node. */
		ins[0] = get_Alloc_mem(alloc);
		ins[1] = addsp_mem;
		sync = new_r_Sync(block, 2, ins);

		exchange(alloc_mem, sync);
	}

	exchange(alloc, new_alloc);

	/* fix projnum of alloca res */
	set_Proj_proj(alloc_res, pn_be_AddSP_res);

	curr_sp = new_r_Proj(new_alloc,  get_irn_mode(curr_sp), pn_be_AddSP_sp);

	return curr_sp;
}

/**
 * Adjust a Free.
 * The Free is transformed into a back end free node and connected to the stack nodes.
 */
static ir_node *adjust_free(be_abi_irg_t *env, ir_node *free, ir_node *curr_sp)
{
	ir_node *block;
	ir_graph *irg;
	ir_node *subsp, *mem, *res, *size, *sync;
	ir_type *type;
	ir_node *in[2];
	ir_mode *sp_mode;
	unsigned stack_alignment;
	dbg_info *dbg;

	assert(get_Free_where(free) == stack_alloc);

	block = get_nodes_block(free);
	irg = get_irn_irg(block);
	type = get_Free_type(free);
	sp_mode = env->arch_env->sp->reg_class->mode;
	dbg = get_irn_dbg_info(free);

	/* we might need to multiply the size with the element size */
	if (type != firm_unknown_type && get_type_size_bytes(type) != 1) {
		tarval *tv = new_tarval_from_long(get_type_size_bytes(type), mode_Iu);
		ir_node *cnst = new_rd_Const(dbg, irg, tv);
		ir_node *mul = new_rd_Mul(dbg, block, get_Free_size(free),
		                          cnst, mode_Iu);
		size = mul;
	} else {
		size = get_Free_size(free);
	}

	stack_alignment = 1 << env->arch_env->stack_alignment;
	size            = adjust_alloc_size(stack_alignment, size, block, dbg);

	/* The stack pointer will be modified in an unknown manner.
	   We cannot omit it. */
	env->call->flags.bits.try_omit_fp = 0;
	subsp = be_new_SubSP(env->arch_env->sp, block, curr_sp, size);
	set_irn_dbg_info(subsp, dbg);

	mem = new_r_Proj(subsp, mode_M, pn_be_SubSP_M);
	res = new_r_Proj(subsp, sp_mode, pn_be_SubSP_sp);

	/* we need to sync the memory */
	in[0] = get_Free_mem(free);
	in[1] = mem;
	sync = new_r_Sync(block, 2, in);

	/* and make the AddSP dependent on the former memory */
	add_irn_dep(subsp, get_Free_mem(free));

	/* kill the free */
	exchange(free, sync);
	curr_sp = res;

	return curr_sp;
}

/**
 * Check if a node is somehow data dependent on another one.
 * both nodes must be in the same basic block.
 * @param n1 The first node.
 * @param n2 The second node.
 * @return 1, if n1 is data dependent (transitively) on n2, 0 if not.
 */
static int dependent_on(ir_node *n1, ir_node *n2)
{
	assert(get_nodes_block(n1) == get_nodes_block(n2));

	return heights_reachable_in_block(ir_heights, n1, n2);
}

static int cmp_call_dependency(const void *c1, const void *c2)
{
	ir_node *n1 = *(ir_node **) c1;
	ir_node *n2 = *(ir_node **) c2;

	/*
		Classical qsort() comparison function behavior:
		0  if both elements are equal
		1  if second is "smaller" that first
		-1 if first is "smaller" that second
	*/
	if (dependent_on(n1, n2))
		return -1;

	if (dependent_on(n2, n1))
		return 1;

	/* The nodes have no depth order, but we need a total order because qsort()
	 * is not stable. */
	return get_irn_idx(n1) - get_irn_idx(n2);
}

/**
 * Walker: links all Call/Alloc/Free nodes to the Block they are contained.
 * Clears the irg_is_leaf flag if a Call is detected.
 */
static void link_ops_in_block_walker(ir_node *irn, void *data)
{
	be_abi_irg_t *env  = data;
	ir_opcode     code = get_irn_opcode(irn);

	if (code == iro_Call ||
	   (code == iro_Alloc && get_Alloc_where(irn) == stack_alloc) ||
	   (code == iro_Free && get_Free_where(irn) == stack_alloc)) {
		ir_node *bl       = get_nodes_block(irn);
		void *save        = get_irn_link(bl);

		if (code == iro_Call)
			env->call->flags.bits.irg_is_leaf = 0;

		set_irn_link(irn, save);
		set_irn_link(bl, irn);
	}

	if (code == iro_Builtin && get_Builtin_kind(irn) == ir_bk_return_address) {
		ir_node       *param = get_Builtin_param(irn, 0);
		tarval        *tv    = get_Const_tarval(param);
		unsigned long  value = get_tarval_long(tv);
		/* use ebp, so the climbframe algo works... */
		if (value > 0) {
			env->call->flags.bits.try_omit_fp = 0;
		}
	}
}

/**
 * Block-walker:
 * Process all Call/Alloc/Free nodes inside a basic block.
 * Note that the link field of the block must contain a linked list of all
 * Call nodes inside the Block. We first order this list according to data dependency
 * and that connect the calls together.
 */
static void process_ops_in_block(ir_node *bl, void *data)
{
	be_abi_irg_t   *env     = data;
	ir_node        *curr_sp = env->init_sp;
	ir_node        *irn;
	ir_node       **nodes;
	int             n;
	int             n_nodes;

	n_nodes = 0;
	for (irn = get_irn_link(bl); irn != NULL; irn = get_irn_link(irn)) {
		++n_nodes;
	}

	nodes = ALLOCAN(ir_node*, n_nodes);
	for (irn = get_irn_link(bl), n = 0; irn; irn = get_irn_link(irn), ++n) {
		nodes[n] = irn;
	}

	/* If there were call nodes in the block. */
	if (n > 0) {
		ir_node *keep;
		int i;

		/* order the call nodes according to data dependency */
		qsort(nodes, n_nodes, sizeof(nodes[0]), cmp_call_dependency);

		for (i = n_nodes - 1; i >= 0; --i) {
			ir_node *irn = nodes[i];

			DBG((dbg, LEVEL_3, "\tprocessing call %+F\n", irn));
			switch (get_irn_opcode(irn)) {
			case iro_Call:
				if (! be_omit_fp) {
					/* The stack pointer will be modified due to a call. */
					env->call->flags.bits.try_omit_fp = 0;
				}
				curr_sp = adjust_call(env, irn, curr_sp);
				break;
			case iro_Alloc:
				if (get_Alloc_where(irn) == stack_alloc)
					curr_sp = adjust_alloc(env, irn, curr_sp);
				break;
			case iro_Free:
				if (get_Free_where(irn) == stack_alloc)
					curr_sp = adjust_free(env, irn, curr_sp);
				break;
			default:
				panic("invalid call");
			}
		}

		/* Keep the last stack state in the block by tying it to Keep node,
		 * the proj from calls is already kept */
		if (curr_sp != env->init_sp &&
		    !(is_Proj(curr_sp) && be_is_Call(get_Proj_pred(curr_sp)))) {
			nodes[0] = curr_sp;
			keep     = be_new_Keep(bl, 1, nodes);
			pmap_insert(env->keep_map, bl, keep);
		}
	}

	set_irn_link(bl, curr_sp);
}

/**
 * Adjust all call nodes in the graph to the ABI conventions.
 */
static void process_calls(be_abi_irg_t *env)
{
	ir_graph *irg = env->birg->irg;

	env->call->flags.bits.irg_is_leaf = 1;
	irg_walk_graph(irg, firm_clear_link, link_ops_in_block_walker, env);

	ir_heights = heights_new(env->birg->irg);
	irg_block_walk_graph(irg, NULL, process_ops_in_block, env);
	heights_free(ir_heights);
}

/**
 * Computes the stack argument layout type.
 * Changes a possibly allocated value param type by moving
 * entities to the stack layout type.
 *
 * @param env           the ABI environment
 * @param call          the current call ABI
 * @param method_type   the method type
 * @param val_param_tp  the value parameter type, will be destroyed
 * @param param_map     an array mapping method arguments to the stack layout type
 *
 * @return the stack argument layout type
 */
static ir_type *compute_arg_type(be_abi_irg_t *env, be_abi_call_t *call,
								 ir_type *method_type, ir_type *val_param_tp,
								 ir_entity ***param_map)
{
	int dir  = env->call->flags.bits.left_to_right ? 1 : -1;
	int inc  = env->birg->main_env->arch_env->stack_dir * dir;
	int n    = get_method_n_params(method_type);
	int curr = inc > 0 ? 0 : n - 1;
	struct obstack *obst = be_get_birg_obst(env->irg);
	int ofs  = 0;

	char buf[128];
	ir_type *res;
	int i;
	ident *id = get_entity_ident(get_irg_entity(env->birg->irg));
	ir_entity **map;

	*param_map = map = OALLOCN(obst, ir_entity*, n);
	res = new_type_struct(id_mangle_u(id, new_id_from_chars("arg_type", 8)));
	for (i = 0; i < n; ++i, curr += inc) {
		ir_type *param_type    = get_method_param_type(method_type, curr);
		be_abi_call_arg_t *arg = get_call_arg(call, 0, curr, 1);

		map[i] = NULL;
		if (arg->on_stack) {
			if (val_param_tp != NULL) {
				/* the entity was already created, create a copy in the param type */
				ir_entity *val_ent = get_method_value_param_ent(method_type, i);
				arg->stack_ent = copy_entity_own(val_ent, res);
				set_entity_link(val_ent, arg->stack_ent);
				set_entity_link(arg->stack_ent, NULL);
			} else {
				/* create a new entity */
				snprintf(buf, sizeof(buf), "param_%d", i);
				arg->stack_ent = new_entity(res, new_id_from_str(buf), param_type);
			}
			ofs += arg->space_before;
			ofs = round_up2(ofs, arg->alignment);
			set_entity_offset(arg->stack_ent, ofs);
			ofs += arg->space_after;
			ofs += get_type_size_bytes(param_type);
			map[i] = arg->stack_ent;
		}
	}
	set_type_size_bytes(res, ofs);
	set_type_state(res, layout_fixed);
	return res;
}

typedef struct {
	const arch_register_t *reg;
	ir_node *irn;
} reg_node_map_t;

static int cmp_regs(const void *a, const void *b)
{
	const reg_node_map_t *p = a;
	const reg_node_map_t *q = b;

	if (p->reg->reg_class == q->reg->reg_class)
		return p->reg->index - q->reg->index;
	else
		return p->reg->reg_class - q->reg->reg_class;
}

static void reg_map_to_arr(reg_node_map_t *res, pmap *reg_map)
{
	pmap_entry *ent;
	int n = pmap_count(reg_map);
	int i = 0;

	foreach_pmap(reg_map, ent) {
		res[i].reg = ent->key;
		res[i].irn = ent->value;
		i++;
	}

	qsort(res, n, sizeof(res[0]), cmp_regs);
}

/**
 * Creates a barrier.
 */
static ir_node *create_barrier(ir_node *bl, ir_node **mem, pmap *regs,
                               int in_req)
{
	int             n_regs = pmap_count(regs);
	int             n;
	ir_node        *irn;
	ir_node       **in;
	reg_node_map_t *rm;

	in = ALLOCAN(ir_node*, n_regs+1);
	rm = ALLOCAN(reg_node_map_t, n_regs);
	reg_map_to_arr(rm, regs);
	for (n = 0; n < n_regs; ++n) {
		in[n] = rm[n].irn;
	}

	if (mem) {
		in[n++] = *mem;
	}

	irn = be_new_Barrier(bl, n, in);

	for (n = 0; n < n_regs; ++n) {
		ir_node               *pred     = rm[n].irn;
		const arch_register_t *reg      = rm[n].reg;
		arch_register_type_t   add_type = 0;
		ir_node               *proj;
		const backend_info_t  *info;

		/* stupid workaround for now... as not all nodes report register
		 * requirements. */
		info = be_get_info(skip_Proj(pred));
		if (info != NULL && info->out_infos != NULL) {
			const arch_register_req_t *ireq = arch_get_register_req_out(pred);
			if (ireq->type & arch_register_req_type_ignore)
				add_type |= arch_register_req_type_ignore;
			if (ireq->type & arch_register_req_type_produces_sp)
				add_type |= arch_register_req_type_produces_sp;
		}

		proj = new_r_Proj(irn, get_irn_mode(pred), n);
		be_node_set_reg_class_in(irn, n, reg->reg_class);
		if (in_req)
			be_set_constr_single_reg_in(irn, n, reg, 0);
		be_set_constr_single_reg_out(irn, n, reg, add_type);
		arch_set_irn_register(proj, reg);

		pmap_insert(regs, (void *) reg, proj);
	}

	if (mem) {
		*mem = new_r_Proj(irn, mode_M, n);
	}

	return irn;
}

/**
 * Creates a be_Return for a Return node.
 *
 * @param @env    the abi environment
 * @param irn     the Return node or NULL if there was none
 * @param bl      the block where the be_Retun should be placed
 * @param mem     the current memory
 * @param n_res   number of return results
 */
static ir_node *create_be_return(be_abi_irg_t *env, ir_node *irn, ir_node *bl,
		ir_node *mem, int n_res)
{
	be_abi_call_t    *call     = env->call;
	const arch_env_t *arch_env = env->birg->main_env->arch_env;
	dbg_info *dbgi;
	pmap *reg_map  = pmap_create();
	ir_node *keep  = pmap_get(env->keep_map, bl);
	int in_max;
	ir_node *ret;
	int i, n;
	unsigned pop;
	ir_node **in;
	ir_node *stack;
	const arch_register_t **regs;
	pmap_entry *ent;

	/*
		get the valid stack node in this block.
		If we had a call in that block there is a Keep constructed by process_calls()
		which points to the last stack modification in that block. we'll use
		it then. Else we use the stack from the start block and let
		the ssa construction fix the usage.
	*/
	stack = be_abi_reg_map_get(env->regs, arch_env->sp);
	if (keep) {
		stack = get_irn_n(keep, 0);
		kill_node(keep);
		remove_End_keepalive(get_irg_end(env->birg->irg), keep);
	}

	/* Insert results for Return into the register map. */
	for (i = 0; i < n_res; ++i) {
		ir_node *res           = get_Return_res(irn, i);
		be_abi_call_arg_t *arg = get_call_arg(call, 1, i, 1);
		assert(arg->in_reg && "return value must be passed in register");
		pmap_insert(reg_map, (void *) arg->reg, res);
	}

	/* Add uses of the callee save registers. */
	foreach_pmap(env->regs, ent) {
		const arch_register_t *reg = ent->key;
		if (arch_register_type_is(reg, callee_save) || arch_register_type_is(reg, ignore))
			pmap_insert(reg_map, ent->key, ent->value);
	}

	be_abi_reg_map_set(reg_map, arch_env->sp, stack);

	/* Make the Epilogue node and call the arch's epilogue maker. */
	create_barrier(bl, &mem, reg_map, 1);
	call->cb->epilogue(env->cb, bl, &mem, reg_map);

	/*
		Maximum size of the in array for Return nodes is
		return args + callee save/ignore registers + memory + stack pointer
	*/
	in_max = pmap_count(reg_map) + n_res + 2;

	in   = ALLOCAN(ir_node*,               in_max);
	regs = ALLOCAN(arch_register_t const*, in_max);

	in[0]   = mem;
	in[1]   = be_abi_reg_map_get(reg_map, arch_env->sp);
	regs[0] = NULL;
	regs[1] = arch_env->sp;
	n       = 2;

	/* clear SP entry, since it has already been grown. */
	pmap_insert(reg_map, (void *) arch_env->sp, NULL);
	for (i = 0; i < n_res; ++i) {
		be_abi_call_arg_t *arg = get_call_arg(call, 1, i, 1);

		in[n]     = be_abi_reg_map_get(reg_map, arg->reg);
		regs[n++] = arg->reg;

		/* Clear the map entry to mark the register as processed. */
		be_abi_reg_map_set(reg_map, arg->reg, NULL);
	}

	/* grow the rest of the stuff. */
	foreach_pmap(reg_map, ent) {
		if (ent->value) {
			in[n]     = ent->value;
			regs[n++] = ent->key;
		}
	}

	/* The in array for the new back end return is now ready. */
	if (irn != NULL) {
		dbgi = get_irn_dbg_info(irn);
	} else {
		dbgi = NULL;
	}
	/* we have to pop the shadow parameter in in case of struct returns */
	pop = call->pop;
	ret = be_new_Return(dbgi, env->birg->irg, bl, n_res, pop, n, in);

	/* Set the register classes of the return's parameter accordingly. */
	for (i = 0; i < n; ++i) {
		if (regs[i] == NULL)
			continue;

		be_node_set_reg_class_in(ret, i, regs[i]->reg_class);
	}

	/* Free the space of the Epilog's in array and the register <-> proj map. */
	pmap_destroy(reg_map);

	return ret;
}

typedef struct ent_pos_pair ent_pos_pair;
struct ent_pos_pair {
	ir_entity    *ent;   /**< a value param entity */
	int          pos;    /**< its parameter number */
	ent_pos_pair *next;  /**< for linking */
};

typedef struct lower_frame_sels_env_t {
	ent_pos_pair *value_param_list;          /**< the list of all value param entities */
	ir_node      *frame;                     /**< the current frame */
	const arch_register_class_t *sp_class;   /**< register class of the stack pointer */
	const arch_register_class_t *link_class; /**< register class of the link pointer */
	ir_type      *value_tp;                  /**< the value type if any */
	ir_type      *frame_tp;                  /**< the frame type */
	int          static_link_pos;            /**< argument number of the hidden static link */
} lower_frame_sels_env_t;

/**
 * Return an entity from the backend for an value param entity.
 *
 * @param ent  an value param type entity
 * @param ctx  context
 */
static ir_entity *get_argument_entity(ir_entity *ent, lower_frame_sels_env_t *ctx)
{
	ir_entity *argument_ent = get_entity_link(ent);

	if (argument_ent == NULL) {
		/* we have NO argument entity yet: This is bad, as we will
		* need one for backing store.
		* Create one here.
		*/
		ir_type *frame_tp = ctx->frame_tp;
		unsigned offset   = get_type_size_bytes(frame_tp);
		ir_type  *tp      = get_entity_type(ent);
		unsigned align    = get_type_alignment_bytes(tp);

		offset += align - 1;
		offset &= ~(align - 1);

		argument_ent = copy_entity_own(ent, frame_tp);

		/* must be automatic to set a fixed layout */
		set_entity_offset(argument_ent, offset);
		offset += get_type_size_bytes(tp);

		set_type_size_bytes(frame_tp, offset);
		set_entity_link(ent, argument_ent);
	}
	return argument_ent;
}
/**
 * Walker: Replaces Sels of frame type and
 * value param type entities by FrameAddress.
 * Links all used entities.
 */
static void lower_frame_sels_walker(ir_node *irn, void *data)
{
	lower_frame_sels_env_t *ctx = data;

	if (is_Sel(irn)) {
		ir_node *ptr = get_Sel_ptr(irn);

		if (ptr == ctx->frame) {
			ir_entity    *ent = get_Sel_entity(irn);
			ir_node      *bl  = get_nodes_block(irn);
			ir_node      *nw;
			int          pos = 0;
			int          is_value_param = 0;

			if (get_entity_owner(ent) == ctx->value_tp) {
				is_value_param = 1;

				/* replace by its copy from the argument type */
				pos = get_struct_member_index(ctx->value_tp, ent);
				ent = get_argument_entity(ent, ctx);
			}

			nw = be_new_FrameAddr(ctx->sp_class, bl, ctx->frame, ent);
			exchange(irn, nw);

			/* check, if it's a param Sel and if have not seen this entity before */
			if (is_value_param && get_entity_link(ent) == NULL) {
				ent_pos_pair pair;

				pair.ent  = ent;
				pair.pos  = pos;
				pair.next = NULL;
				ARR_APP1(ent_pos_pair, ctx->value_param_list, pair);
				/* just a mark */
				set_entity_link(ent, ctx->value_param_list);
			}
		}
	}
}

/**
 * Check if a value parameter is transmitted as a register.
 * This might happen if the address of an parameter is taken which is
 * transmitted in registers.
 *
 * Note that on some architectures this case must be handled specially
 * because the place of the backing store is determined by their ABI.
 *
 * In the default case we move the entity to the frame type and create
 * a backing store into the first block.
 */
static void fix_address_of_parameter_access(be_abi_irg_t *env, ent_pos_pair *value_param_list)
{
	be_abi_call_t *call = env->call;
	ir_graph      *irg  = env->birg->irg;
	ent_pos_pair  *entry, *new_list;
	ir_type       *frame_tp;
	int           i, n = ARR_LEN(value_param_list);

	new_list = NULL;
	for (i = 0; i < n; ++i) {
		int               pos  = value_param_list[i].pos;
		be_abi_call_arg_t *arg = get_call_arg(call, 0, pos, 1);

		if (arg->in_reg) {
			DBG((dbg, LEVEL_2, "\targ #%d need backing store\n", pos));
			value_param_list[i].next = new_list;
			new_list = &value_param_list[i];
		}
	}
	if (new_list != NULL) {
		/* ok, change the graph */
		ir_node *start_bl = get_irg_start_block(irg);
		ir_node *first_bl = get_first_block_succ(start_bl);
		ir_node *frame, *imem, *nmem, *store, *mem, *args;
		optimization_state_t state;
		unsigned offset;

		assert(first_bl && first_bl != start_bl);
		/* we had already removed critical edges, so the following
		   assertion should be always true. */
		assert(get_Block_n_cfgpreds(first_bl) == 1);

		/* now create backing stores */
		frame = get_irg_frame(irg);
		imem = get_irg_initial_mem(irg);

		save_optimization_state(&state);
		set_optimize(0);
		nmem = new_r_Proj(get_irg_start(irg), mode_M, pn_Start_M);
		restore_optimization_state(&state);

		/* reroute all edges to the new memory source */
		edges_reroute(imem, nmem, irg);

		store   = NULL;
		mem     = imem;
		args    = get_irg_args(irg);
		for (entry = new_list; entry != NULL; entry = entry->next) {
			int     i     = entry->pos;
			ir_type *tp   = get_entity_type(entry->ent);
			ir_mode *mode = get_type_mode(tp);
			ir_node *addr;

			/* address for the backing store */
			addr = be_new_FrameAddr(env->arch_env->sp->reg_class, first_bl, frame, entry->ent);

			if (store)
				mem = new_r_Proj(store, mode_M, pn_Store_M);

			/* the backing store itself */
			store = new_r_Store(first_bl, mem, addr,
			                    new_r_Proj(args, mode, i), 0);
		}
		/* the new memory Proj gets the last Proj from store */
		set_Proj_pred(nmem, store);
		set_Proj_proj(nmem, pn_Store_M);

		/* move all entities to the frame type */
		frame_tp = get_irg_frame_type(irg);
		offset   = get_type_size_bytes(frame_tp);

		/* we will add new entities: set the layout to undefined */
		assert(get_type_state(frame_tp) == layout_fixed);
		set_type_state(frame_tp, layout_undefined);
		for (entry = new_list; entry != NULL; entry = entry->next) {
			ir_entity *ent = entry->ent;

			/* If the entity is still on the argument type, move it to the
			 * frame type.
			 * This happens if the value_param type was build due to compound
			 * params. */
			if (get_entity_owner(ent) != frame_tp) {
				ir_type  *tp   = get_entity_type(ent);
				unsigned align = get_type_alignment_bytes(tp);

				offset += align - 1;
				offset &= ~(align - 1);
				set_entity_owner(ent, frame_tp);
				/* must be automatic to set a fixed layout */
				set_entity_offset(ent, offset);
				offset += get_type_size_bytes(tp);
			}
		}
		set_type_size_bytes(frame_tp, offset);
		/* fix the layout again */
		set_type_state(frame_tp, layout_fixed);
	}
}

/**
 * The start block has no jump, instead it has an initial exec Proj.
 * The backend wants to handle all blocks the same way, so we replace
 * the out cfg edge with a real jump.
 */
static void fix_start_block(ir_graph *irg)
{
	ir_node         *initial_X   = get_irg_initial_exec(irg);
	ir_node         *start_block = get_irg_start_block(irg);
	const ir_edge_t *edge;

	assert(is_Proj(initial_X));

	foreach_out_edge(initial_X, edge) {
		ir_node *block = get_edge_src_irn(edge);

		if (is_Anchor(block))
			continue;
		if (block != start_block) {
			ir_node *jmp = new_r_Jmp(start_block);
			set_Block_cfgpred(block, get_edge_src_pos(edge), jmp);
			set_irg_initial_exec(irg, jmp);
			return;
		}
	}
	panic("Initial exec has no follow block in %+F", irg);
}

/**
 * Update the entity of Sels to the outer value parameters.
 */
static void update_outer_frame_sels(ir_node *irn, void *env)
{
	lower_frame_sels_env_t *ctx = env;
	ir_node                *ptr;
	ir_entity              *ent;
	int                    pos = 0;

	if (! is_Sel(irn))
		return;
	ptr = get_Sel_ptr(irn);
	if (! is_arg_Proj(ptr))
		return;
	if (get_Proj_proj(ptr) != ctx->static_link_pos)
		return;
	ent   = get_Sel_entity(irn);

	if (get_entity_owner(ent) == ctx->value_tp) {
		/* replace by its copy from the argument type */
		pos = get_struct_member_index(ctx->value_tp, ent);
		ent = get_argument_entity(ent, ctx);
		set_Sel_entity(irn, ent);

		/* check, if we have not seen this entity before */
		if (get_entity_link(ent) == NULL) {
			ent_pos_pair pair;

			pair.ent  = ent;
			pair.pos  = pos;
			pair.next = NULL;
			ARR_APP1(ent_pos_pair, ctx->value_param_list, pair);
			/* just a mark */
			set_entity_link(ent, ctx->value_param_list);
		}
	}
}

/**
 * Fix access to outer local variables.
 */
static void fix_outer_variable_access(be_abi_irg_t *env,
                                      lower_frame_sels_env_t *ctx)
{
	int      i;
	ir_graph *irg;
	(void) env;

	for (i = get_class_n_members(ctx->frame_tp) - 1; i >= 0; --i) {
		ir_entity *ent = get_class_member(ctx->frame_tp, i);

		if (! is_method_entity(ent))
			continue;

		irg = get_entity_irg(ent);
		if (irg == NULL)
			continue;

		/*
		 * FIXME: find the number of the static link parameter
		 * for now we assume 0 here
		 */
		ctx->static_link_pos = 0;

		irg_walk_graph(irg, NULL, update_outer_frame_sels, ctx);
	}
}

/**
 * Modify the irg itself and the frame type.
 */
static void modify_irg(be_abi_irg_t *env)
{
	be_abi_call_t *call       = env->call;
	const arch_env_t *arch_env= env->birg->main_env->arch_env;
	const arch_register_t *sp = arch_env->sp;
	ir_graph *irg             = env->birg->irg;
	ir_node *end;
	ir_node *old_mem;
	ir_node *new_mem_proj;
	ir_node *mem;
	ir_type *method_type      = get_entity_type(get_irg_entity(irg));
	struct obstack *obst      = be_get_birg_obst(irg);

	int n_params;
	int i, n;
	unsigned j;
	unsigned frame_size;

	reg_node_map_t *rm;
	const arch_register_t *fp_reg;
	ir_node *frame_pointer;
	ir_node *start_bl;
	ir_node **args;
	ir_node *arg_tuple;
	const ir_edge_t *edge;
	ir_type *arg_type, *bet_type, *tp;
	lower_frame_sels_env_t ctx;
	ir_entity **param_map;

	DBG((dbg, LEVEL_1, "introducing abi on %+F\n", irg));

	/* Must fetch memory here, otherwise the start Barrier gets the wrong
	 * memory, which leads to loops in the DAG. */
	old_mem = get_irg_initial_mem(irg);

	irp_reserve_resources(irp, IR_RESOURCE_ENTITY_LINK);

	/* set the links of all frame entities to NULL, we use it
	   to detect if an entity is already linked in the value_param_list */
	tp = get_method_value_param_type(method_type);
	ctx.value_tp = tp;
	if (tp != NULL) {
		/* clear the links of the clone type, let the
		   original entities point to its clones */
		for (i = get_struct_n_members(tp) - 1; i >= 0; --i) {
			ir_entity *mem  = get_struct_member(tp, i);
			set_entity_link(mem, NULL);
		}
	}

	arg_type = compute_arg_type(env, call, method_type, tp, &param_map);

	/* Convert the Sel nodes in the irg to frame addr nodes: */
	ctx.value_param_list = NEW_ARR_F(ent_pos_pair, 0);
	ctx.frame            = get_irg_frame(irg);
	ctx.sp_class         = env->arch_env->sp->reg_class;
	ctx.link_class       = env->arch_env->link_class;
	ctx.frame_tp         = get_irg_frame_type(irg);

	/* layout the stackframe now */
	if (get_type_state(ctx.frame_tp) == layout_undefined) {
		default_layout_compound_type(ctx.frame_tp);
	}

	/* we will possible add new entities to the frame: set the layout to undefined */
	assert(get_type_state(ctx.frame_tp) == layout_fixed);
	set_type_state(ctx.frame_tp, layout_undefined);

	irg_walk_graph(irg, lower_frame_sels_walker, NULL, &ctx);

	/* fix the frame type layout again */
	set_type_state(ctx.frame_tp, layout_fixed);
	/* align stackframe to 4 byte */
	frame_size = get_type_size_bytes(ctx.frame_tp);
	if (frame_size % 4 != 0) {
		set_type_size_bytes(ctx.frame_tp, frame_size + 4 - (frame_size % 4));
	}

	env->regs  = pmap_create();

	n_params = get_method_n_params(method_type);
	args     = OALLOCNZ(obst, ir_node*, n_params);

	/*
	 * for inner function we must now fix access to outer frame entities.
	 */
	fix_outer_variable_access(env, &ctx);

	/* Check if a value parameter is transmitted as a register.
	 * This might happen if the address of an parameter is taken which is
	 * transmitted in registers.
	 *
	 * Note that on some architectures this case must be handled specially
	 * because the place of the backing store is determined by their ABI.
	 *
	 * In the default case we move the entity to the frame type and create
	 * a backing store into the first block.
	 */
	fix_address_of_parameter_access(env, ctx.value_param_list);

	DEL_ARR_F(ctx.value_param_list);
	irp_free_resources(irp, IR_RESOURCE_ENTITY_LINK);

	/* Fill the argument vector */
	arg_tuple = get_irg_args(irg);
	foreach_out_edge(arg_tuple, edge) {
		ir_node *irn = get_edge_src_irn(edge);
		if (! is_Anchor(irn)) {
			int nr       = get_Proj_proj(irn);
			args[nr]     = irn;
			DBG((dbg, LEVEL_2, "\treading arg: %d -> %+F\n", nr, irn));
		}
	}

	bet_type = call->cb->get_between_type(env->cb);
	stack_frame_init(&env->frame, arg_type, bet_type, get_irg_frame_type(irg), arch_env->stack_dir, param_map);

	/* Count the register params and add them to the number of Projs for the RegParams node */
	for (i = 0; i < n_params; ++i) {
		be_abi_call_arg_t *arg = get_call_arg(call, 0, i, 1);
		if (arg->in_reg && args[i]) {
			assert(arg->reg != sp && "cannot use stack pointer as parameter register");
			assert(i == get_Proj_proj(args[i]));

			/* For now, associate the register with the old Proj from Start representing that argument. */
			pmap_insert(env->regs, (void *) arg->reg, args[i]);
			DBG((dbg, LEVEL_2, "\targ #%d -> reg %s\n", i, arg->reg->name));
		}
	}

	/* Collect all callee-save registers */
	for (i = 0, n = arch_env_get_n_reg_class(arch_env); i < n; ++i) {
		const arch_register_class_t *cls = arch_env_get_reg_class(arch_env, i);
		for (j = 0; j < cls->n_regs; ++j) {
			const arch_register_t *reg = &cls->regs[j];
			if (arch_register_type_is(reg, callee_save) ||
					arch_register_type_is(reg, state)) {
				pmap_insert(env->regs, (void *) reg, NULL);
			}
		}
	}

	/* handle start block here (place a jump in the block) */
	fix_start_block(irg);

	pmap_insert(env->regs, (void *) sp, NULL);
	pmap_insert(env->regs, (void *) arch_env->bp, NULL);
	start_bl   = get_irg_start_block(irg);
	env->start = be_new_Start(NULL, start_bl, pmap_count(env->regs) + 1);

	/*
	 * make proj nodes for the callee save registers.
	 * memorize them, since Return nodes get those as inputs.
	 *
	 * Note, that if a register corresponds to an argument, the regs map contains
	 * the old Proj from start for that argument.
	 */

	rm = ALLOCAN(reg_node_map_t, pmap_count(env->regs));
	reg_map_to_arr(rm, env->regs);
	for (i = 0, n = pmap_count(env->regs); i < n; ++i) {
		arch_register_t          *reg      = (void *) rm[i].reg;
		ir_mode                  *mode     = reg->reg_class->mode;
		long                      nr       = i;
		arch_register_req_type_t  add_type = 0;
		ir_node                  *proj;

		if (reg == sp)
			add_type |= arch_register_req_type_produces_sp | arch_register_req_type_ignore;

		assert(nr >= 0);
		proj = new_r_Proj(env->start, mode, nr + 1);
		pmap_insert(env->regs, (void *) reg, proj);
		be_set_constr_single_reg_out(env->start, nr + 1, reg, add_type);
		arch_set_irn_register(proj, reg);

		DBG((dbg, LEVEL_2, "\tregister save proj #%d -> reg %s\n", nr, reg->name));
	}

	/* create a new initial memory proj */
	assert(is_Proj(old_mem));
	arch_set_out_register_req(env->start, 0, arch_no_register_req);
	new_mem_proj = new_r_Proj(env->start, mode_M, 0);
	mem = new_mem_proj;
	set_irg_initial_mem(irg, mem);

	/* Generate the Prologue */
	fp_reg = call->cb->prologue(env->cb, &mem, env->regs, &env->frame.initial_bias);

	/* do the stack allocation BEFORE the barrier, or spill code
	   might be added before it */
	env->init_sp = be_abi_reg_map_get(env->regs, sp);
	env->init_sp = be_new_IncSP(sp, start_bl, env->init_sp, BE_STACK_FRAME_SIZE_EXPAND, 0);
	be_abi_reg_map_set(env->regs, sp, env->init_sp);

	create_barrier(start_bl, &mem, env->regs, 0);

	env->init_sp = be_abi_reg_map_get(env->regs, sp);
	arch_set_irn_register(env->init_sp, sp);

	frame_pointer = be_abi_reg_map_get(env->regs, fp_reg);
	set_irg_frame(irg, frame_pointer);
	pset_insert_ptr(env->ignore_regs, fp_reg);

	/* rewire old mem users to new mem */
	exchange(old_mem, mem);

	/* keep the mem (for functions with an endless loop = no return) */
	keep_alive(mem);

	set_irg_initial_mem(irg, mem);

	/* Now, introduce stack param nodes for all parameters passed on the stack */
	for (i = 0; i < n_params; ++i) {
		ir_node *arg_proj = args[i];
		ir_node *repl     = NULL;

		if (arg_proj != NULL) {
			be_abi_call_arg_t *arg;
			ir_type *param_type;
			int     nr = get_Proj_proj(arg_proj);
			ir_mode *mode;

			nr         = MIN(nr, n_params);
			arg        = get_call_arg(call, 0, nr, 1);
			param_type = get_method_param_type(method_type, nr);

			if (arg->in_reg) {
				repl = pmap_get(env->regs, (void *) arg->reg);
			} else if (arg->on_stack) {
				ir_node *addr = be_new_FrameAddr(sp->reg_class, start_bl, frame_pointer, arg->stack_ent);

				/* For atomic parameters which are actually used, we create a Load node. */
				if (is_atomic_type(param_type) && get_irn_n_edges(args[i]) > 0) {
					ir_mode *mode      = get_type_mode(param_type);
					ir_mode *load_mode = arg->load_mode;

					ir_node *load = new_r_Load(start_bl, new_NoMem(), addr, load_mode, cons_floats);
					repl = new_r_Proj(load, load_mode, pn_Load_res);

					if (mode != load_mode) {
						repl = new_r_Conv(start_bl, repl, mode);
					}
				} else {
					/* The stack parameter is not primitive (it is a struct or array),
					 * we thus will create a node representing the parameter's address
					 * on the stack. */
					repl = addr;
				}
			}

			assert(repl != NULL);

			/* Beware: the mode of the register parameters is always the mode of the register class
			   which may be wrong. Add Conv's then. */
			mode = get_irn_mode(args[i]);
			if (mode != get_irn_mode(repl)) {
				repl = new_r_Conv(get_nodes_block(repl), repl, mode);
			}
			exchange(args[i], repl);
		}
	}

	/* the arg proj is not needed anymore now and should be only used by the anchor */
	assert(get_irn_n_edges(arg_tuple) == 1);
	kill_node(arg_tuple);
	set_irg_args(irg, new_r_Bad(irg));

	/* All Return nodes hang on the End node, so look for them there. */
	end = get_irg_end_block(irg);
	for (i = 0, n = get_Block_n_cfgpreds(end); i < n; ++i) {
		ir_node *irn = get_Block_cfgpred(end, i);

		if (is_Return(irn)) {
			ir_node *blk = get_nodes_block(irn);
			ir_node *mem = get_Return_mem(irn);
			ir_node *ret = create_be_return(env, irn, blk, mem, get_Return_n_ress(irn));
			exchange(irn, ret);
		}
	}

	/* if we have endless loops here, n might be <= 0. Do NOT create a be_Return then,
	   the code is dead and will never be executed. */
}

/** Fix the state inputs of calls that still hang on unknowns */
static void fix_call_state_inputs(be_abi_irg_t *env)
{
	const arch_env_t *arch_env = env->arch_env;
	int i, n, n_states;
	arch_register_t **stateregs = NEW_ARR_F(arch_register_t*, 0);

	/* Collect caller save registers */
	n = arch_env_get_n_reg_class(arch_env);
	for (i = 0; i < n; ++i) {
		unsigned j;
		const arch_register_class_t *cls = arch_env_get_reg_class(arch_env, i);
		for (j = 0; j < cls->n_regs; ++j) {
			const arch_register_t *reg = arch_register_for_index(cls, j);
			if (arch_register_type_is(reg, state)) {
				ARR_APP1(arch_register_t*, stateregs, (arch_register_t *)reg);
			}
		}
	}

	n = ARR_LEN(env->calls);
	n_states = ARR_LEN(stateregs);
	for (i = 0; i < n; ++i) {
		int s, arity;
		ir_node *call = env->calls[i];

		arity = get_irn_arity(call);

		/* the state reg inputs are the last n inputs of the calls */
		for (s = 0; s < n_states; ++s) {
			int inp = arity - n_states + s;
			const arch_register_t *reg = stateregs[s];
			ir_node *regnode = be_abi_reg_map_get(env->regs, reg);

			set_irn_n(call, inp, regnode);
		}
	}

	DEL_ARR_F(stateregs);
}

/**
 * Create a trampoline entity for the given method.
 */
static ir_entity *create_trampoline(be_main_env_t *be, ir_entity *method)
{
	ir_type   *type   = get_entity_type(method);
	ident     *old_id = get_entity_ld_ident(method);
	ident     *id     = id_mangle3("", old_id, "$stub");
	ir_type   *parent = be->pic_trampolines_type;
	ir_entity *ent    = new_entity(parent, old_id, type);
	set_entity_ld_ident(ent, id);
	set_entity_visibility(ent, ir_visibility_private);

	return ent;
}

/**
 * Returns the trampoline entity for the given method.
 */
static ir_entity *get_trampoline(be_main_env_t *env, ir_entity *method)
{
	ir_entity *result = pmap_get(env->ent_trampoline_map, method);
	if (result == NULL) {
		result = create_trampoline(env, method);
		pmap_insert(env->ent_trampoline_map, method, result);
	}

	return result;
}

static ir_entity *create_pic_symbol(be_main_env_t *be, ir_entity *entity)
{
	ident     *old_id = get_entity_ld_ident(entity);
	ident     *id     = id_mangle3("", old_id, "$non_lazy_ptr");
	ir_type   *e_type = get_entity_type(entity);
	ir_type   *type   = new_type_pointer(e_type);
	ir_type   *parent = be->pic_symbols_type;
	ir_entity *ent    = new_entity(parent, old_id, type);
	set_entity_ld_ident(ent, id);
	set_entity_visibility(ent, ir_visibility_private);

	return ent;
}

static ir_entity *get_pic_symbol(be_main_env_t *env, ir_entity *entity)
{
	ir_entity *result = pmap_get(env->ent_pic_symbol_map, entity);
	if (result == NULL) {
		result = create_pic_symbol(env, entity);
		pmap_insert(env->ent_pic_symbol_map, entity, result);
	}

	return result;
}



/**
 * Returns non-zero if a given entity can be accessed using a relative address.
 */
static int can_address_relative(ir_entity *entity)
{
	return get_entity_visibility(entity) != ir_visibility_external
		&& !(get_entity_linkage(entity) & IR_LINKAGE_MERGE);
}

/** patches SymConsts to work in position independent code */
static void fix_pic_symconsts(ir_node *node, void *data)
{
	ir_graph     *irg;
	ir_node      *pic_base;
	ir_node      *add;
	ir_node      *block;
	ir_mode      *mode;
	ir_node      *load;
	ir_node      *load_res;
	be_abi_irg_t *env = data;
	int           arity, i;
	be_main_env_t *be = env->birg->main_env;

	arity = get_irn_arity(node);
	for (i = 0; i < arity; ++i) {
		dbg_info  *dbgi;
		ir_node   *pred = get_irn_n(node, i);
		ir_entity *entity;
		ir_entity *pic_symbol;
		ir_node   *pic_symconst;

		if (!is_SymConst(pred))
			continue;

		entity = get_SymConst_entity(pred);
		block  = get_nodes_block(pred);
		irg    = get_irn_irg(pred);

		/* calls can jump to relative addresses, so we can directly jump to
		   the (relatively) known call address or the trampoline */
		if (i == 1 && is_Call(node)) {
			ir_entity *trampoline;
			ir_node   *trampoline_const;

			if (can_address_relative(entity))
				continue;

			dbgi             = get_irn_dbg_info(pred);
			trampoline       = get_trampoline(be, entity);
			trampoline_const = new_rd_SymConst_addr_ent(dbgi, irg, mode_P_code,
			                                            trampoline, NULL);
			set_irn_n(node, i, trampoline_const);
			continue;
		}

		/* everything else is accessed relative to EIP */
		mode     = get_irn_mode(pred);
		pic_base = arch_code_generator_get_pic_base(env->birg->cg);

		/* all ok now for locally constructed stuff */
		if (can_address_relative(entity)) {
			ir_node *add = new_r_Add(block, pic_base, pred, mode);

			/* make sure the walker doesn't visit this add again */
			mark_irn_visited(add);
			set_irn_n(node, i, add);
			continue;
		}

		/* get entry from pic symbol segment */
		dbgi         = get_irn_dbg_info(pred);
		pic_symbol   = get_pic_symbol(be, entity);
		pic_symconst = new_rd_SymConst_addr_ent(dbgi, irg, mode_P_code,
 		                                        pic_symbol, NULL);
		add = new_r_Add(block, pic_base, pic_symconst, mode);
		mark_irn_visited(add);

		/* we need an extra indirection for global data outside our current
		   module. The loads are always safe and can therefore float
		   and need no memory input */
		load     = new_r_Load(block, new_NoMem(), add, mode, cons_floats);
		load_res = new_r_Proj(load, mode, pn_Load_res);

		set_irn_n(node, i, load_res);
	}
}

be_abi_irg_t *be_abi_introduce(be_irg_t *birg)
{
	be_abi_irg_t *env  = XMALLOC(be_abi_irg_t);
	ir_node *old_frame = get_irg_frame(birg->irg);
	ir_graph *irg      = birg->irg;
	struct obstack *obst = be_get_birg_obst(irg);

	pmap_entry *ent;
	ir_node *dummy;
	unsigned *limited_bitset;
	arch_register_req_t *sp_req;

	be_omit_fp      = birg->main_env->options->omit_fp;
	be_omit_leaf_fp = birg->main_env->options->omit_leaf_fp;

	obstack_init(obst);

	env->arch_env    = birg->main_env->arch_env;
	env->method_type = get_entity_type(get_irg_entity(irg));
	env->call        = be_abi_call_new(env->arch_env->sp->reg_class);
	arch_env_get_call_abi(env->arch_env, env->method_type, env->call);

	env->ignore_regs  = pset_new_ptr_default();
	env->keep_map     = pmap_create();
	env->dce_survivor = new_survive_dce();
	env->birg         = birg;
	env->irg          = irg;

	sp_req = OALLOCZ(obst, arch_register_req_t);
	env->sp_req = sp_req;

	sp_req->type = arch_register_req_type_limited
	             | arch_register_req_type_produces_sp;
	sp_req->cls  = arch_register_get_class(env->arch_env->sp);

	limited_bitset = rbitset_obstack_alloc(obst, sp_req->cls->n_regs);
	rbitset_set(limited_bitset, arch_register_get_index(env->arch_env->sp));
	sp_req->limited = limited_bitset;
	if (env->arch_env->sp->type & arch_register_type_ignore) {
		sp_req->type |= arch_register_req_type_ignore;
	}

	env->init_sp = dummy = new_r_Dummy(irg, env->arch_env->sp->reg_class->mode);

	env->calls = NEW_ARR_F(ir_node*, 0);

	if (birg->main_env->options->pic) {
		irg_walk_graph(irg, fix_pic_symconsts, NULL, env);
	}

	/* Lower all call nodes in the IRG. */
	process_calls(env);

	/*
		Beware: init backend abi call object after processing calls,
		otherwise some information might be not yet available.
	*/
	env->cb = env->call->cb->init(env->call, birg->main_env->arch_env, irg);

	/* Process the IRG */
	modify_irg(env);

	/* fix call inputs for state registers */
	fix_call_state_inputs(env);

	/* We don't need the keep map anymore. */
	pmap_destroy(env->keep_map);
	env->keep_map = NULL;

	/* calls array is not needed anymore */
	DEL_ARR_F(env->calls);
	env->calls = NULL;

	/* reroute the stack origin of the calls to the true stack origin. */
	exchange(dummy, env->init_sp);
	exchange(old_frame, get_irg_frame(irg));

	/* Make some important node pointers survive the dead node elimination. */
	survive_dce_register_irn(env->dce_survivor, &env->init_sp);
	foreach_pmap(env->regs, ent) {
		survive_dce_register_irn(env->dce_survivor, (ir_node **) &ent->value);
	}

	env->call->cb->done(env->cb);
	env->cb = NULL;
	return env;
}

void be_abi_free(be_abi_irg_t *env)
{
	be_abi_call_free(env->call);
	free_survive_dce(env->dce_survivor);
	del_pset(env->ignore_regs);
	pmap_destroy(env->regs);
	free(env);
}

void be_abi_put_ignore_regs(be_abi_irg_t *abi, const arch_register_class_t *cls, bitset_t *bs)
{
	arch_register_t *reg;

	for (reg = pset_first(abi->ignore_regs); reg; reg = pset_next(abi->ignore_regs))
		if (reg->reg_class == cls)
			bitset_set(bs, reg->index);
}

void be_abi_set_non_ignore_regs(be_abi_irg_t *abi, const arch_register_class_t *cls, unsigned *raw_bitset)
{
	unsigned         i;
	arch_register_t *reg;

	for (i = 0; i < cls->n_regs; ++i) {
		if (arch_register_type_is(&cls->regs[i], ignore))
			continue;

		rbitset_set(raw_bitset, i);
	}

	for (reg = pset_first(abi->ignore_regs); reg != NULL;
	     reg = pset_next(abi->ignore_regs)) {
		if (reg->reg_class != cls)
			continue;

		rbitset_clear(raw_bitset, reg->index);
	}
}

/* Returns the stack layout from a abi environment. */
const be_stack_layout_t *be_abi_get_stack_layout(const be_abi_irg_t *abi)
{
	return &abi->frame;
}

/*

  _____ _        ____  _             _
 |  ___(_)_  __ / ___|| |_ __ _  ___| | __
 | |_  | \ \/ / \___ \| __/ _` |/ __| |/ /
 |  _| | |>  <   ___) | || (_| | (__|   <
 |_|   |_/_/\_\ |____/ \__\__,_|\___|_|\_\

*/

typedef ir_node **node_array;

typedef struct fix_stack_walker_env_t {
	node_array sp_nodes;
} fix_stack_walker_env_t;

/**
 * Walker. Collect all stack modifying nodes.
 */
static void collect_stack_nodes_walker(ir_node *node, void *data)
{
	ir_node                   *insn = node;
	fix_stack_walker_env_t    *env = data;
	const arch_register_req_t *req;

	if (is_Proj(node)) {
		insn = get_Proj_pred(node);
	}

	if (arch_irn_get_n_outs(insn) == 0)
		return;
	if (get_irn_mode(node) == mode_T)
		return;

	req = arch_get_register_req_out(node);
	if (! (req->type & arch_register_req_type_produces_sp))
		return;

	ARR_APP1(ir_node*, env->sp_nodes, node);
}

void be_abi_fix_stack_nodes(be_abi_irg_t *env)
{
	be_ssa_construction_env_t senv;
	int i, len;
	ir_node **phis;
	be_irg_t *birg = env->birg;
	be_lv_t *lv = be_get_birg_liveness(birg);
	fix_stack_walker_env_t walker_env;

	walker_env.sp_nodes = NEW_ARR_F(ir_node*, 0);

	irg_walk_graph(birg->irg, collect_stack_nodes_walker, NULL, &walker_env);

	/* nothing to be done if we didn't find any node, in fact we mustn't
	 * continue, as for endless loops incsp might have had no users and is bad
	 * now.
	 */
	len = ARR_LEN(walker_env.sp_nodes);
	if (len == 0) {
		DEL_ARR_F(walker_env.sp_nodes);
		return;
	}

	be_ssa_construction_init(&senv, birg);
	be_ssa_construction_add_copies(&senv, walker_env.sp_nodes,
                                   ARR_LEN(walker_env.sp_nodes));
	be_ssa_construction_fix_users_array(&senv, walker_env.sp_nodes,
	                                    ARR_LEN(walker_env.sp_nodes));

	if (lv != NULL) {
		len = ARR_LEN(walker_env.sp_nodes);
		for (i = 0; i < len; ++i) {
			be_liveness_update(lv, walker_env.sp_nodes[i]);
		}
		be_ssa_construction_update_liveness_phis(&senv, lv);
	}

	phis = be_ssa_construction_get_new_phis(&senv);

	/* set register requirements for stack phis */
	len = ARR_LEN(phis);
	for (i = 0; i < len; ++i) {
		ir_node *phi = phis[i];
		be_set_phi_reg_req(phi, env->sp_req);
		arch_set_irn_register(phi, env->arch_env->sp);
	}
	be_ssa_construction_destroy(&senv);

	DEL_ARR_F(walker_env.sp_nodes);
}

/**
 * Fix all stack accessing operations in the block bl.
 *
 * @param env        the abi environment
 * @param bl         the block to process
 * @param real_bias  the bias value
 *
 * @return the bias at the end of this block
 */
static int process_stack_bias(be_abi_irg_t *env, ir_node *bl, int real_bias)
{
	int               omit_fp  = env->call->flags.bits.try_omit_fp;
	ir_node          *irn;
	int               wanted_bias = real_bias;

	sched_foreach(bl, irn) {
		int ofs;

		/*
		   Check, if the node relates to an entity on the stack frame.
		   If so, set the true offset (including the bias) for that
		   node.
		 */
		ir_entity *ent = arch_get_frame_entity(irn);
		if (ent != NULL) {
			int bias   = omit_fp ? real_bias : 0;
			int offset = get_stack_entity_offset(&env->frame, ent, bias);
			arch_set_frame_offset(irn, offset);
			DBG((dbg, LEVEL_2, "%F has offset %d (including bias %d)\n",
			     ent, offset, bias));
		}

		/*
		 * If the node modifies the stack pointer by a constant offset,
		 * record that in the bias.
		 */
		ofs = arch_get_sp_bias(irn);

		if (be_is_IncSP(irn)) {
			/* fill in real stack frame size */
			if (ofs == BE_STACK_FRAME_SIZE_EXPAND) {
				ir_type *frame_type = get_irg_frame_type(env->birg->irg);
				ofs = (int) get_type_size_bytes(frame_type);
				be_set_IncSP_offset(irn, ofs);
			} else if (ofs == BE_STACK_FRAME_SIZE_SHRINK) {
				ir_type *frame_type = get_irg_frame_type(env->birg->irg);
				ofs = - (int)get_type_size_bytes(frame_type);
				be_set_IncSP_offset(irn, ofs);
			} else {
				if (be_get_IncSP_align(irn)) {
					/* patch IncSP to produce an aligned stack pointer */
					ir_type *between_type = env->frame.between_type;
					int      between_size = get_type_size_bytes(between_type);
					int      alignment    = 1 << env->arch_env->stack_alignment;
					int      delta        = (real_bias + ofs + between_size) & (alignment - 1);
					assert(ofs >= 0);
					if (delta > 0) {
						be_set_IncSP_offset(irn, ofs + alignment - delta);
						real_bias += alignment - delta;
					}
				} else {
					/* adjust so real_bias corresponds with wanted_bias */
					int delta = wanted_bias - real_bias;
					assert(delta <= 0);
					if (delta != 0) {
						be_set_IncSP_offset(irn, ofs + delta);
						real_bias += delta;
					}
				}
			}
		}

		real_bias   += ofs;
		wanted_bias += ofs;
	}

	assert(real_bias == wanted_bias);
	return real_bias;
}

/**
 * A helper struct for the bias walker.
 */
struct bias_walk {
	be_abi_irg_t *env;     /**< The ABI irg environment. */
	int           start_block_bias;  /**< The bias at the end of the start block. */
	int           between_size;
	ir_node      *start_block;  /**< The start block of the current graph. */
};

/**
 * Block-Walker: fix all stack offsets for all blocks
 * except the start block
 */
static void stack_bias_walker(ir_node *bl, void *data)
{
	struct bias_walk *bw = data;
	if (bl != bw->start_block) {
		process_stack_bias(bw->env, bl, bw->start_block_bias);
	}
}

/**
 * Walker: finally lower all Sels of outer frame or parameter
 * entities.
 */
static void lower_outer_frame_sels(ir_node *sel, void *ctx)
{
	be_abi_irg_t *env = ctx;
	ir_node      *ptr;
	ir_entity    *ent;
	ir_type      *owner;

	if (! is_Sel(sel))
		return;

	ent   = get_Sel_entity(sel);
	owner = get_entity_owner(ent);
	ptr   = get_Sel_ptr(sel);

	if (owner == env->frame.frame_type || owner == env->frame.arg_type) {
		/* found access to outer frame or arguments */
		int offset = get_stack_entity_offset(&env->frame, ent, 0);

		if (offset != 0) {
			ir_node  *bl   = get_nodes_block(sel);
			dbg_info *dbgi = get_irn_dbg_info(sel);
			ir_mode  *mode = get_irn_mode(sel);
			ir_mode  *mode_UInt = get_reference_mode_unsigned_eq(mode);
			ir_node  *cnst = new_r_Const_long(current_ir_graph, mode_UInt, offset);

			ptr = new_rd_Add(dbgi, bl, ptr, cnst, mode);
		}
		exchange(sel, ptr);
	}
}

void be_abi_fix_stack_bias(be_abi_irg_t *env)
{
	ir_graph          *irg = env->birg->irg;
	ir_type           *frame_tp;
	int               i;
	struct bias_walk  bw;

	stack_frame_compute_initial_offset(&env->frame);
	// stack_layout_dump(stdout, frame);

	/* Determine the stack bias at the end of the start block. */
	bw.start_block_bias = process_stack_bias(env, get_irg_start_block(irg), env->frame.initial_bias);
	bw.between_size     = get_type_size_bytes(env->frame.between_type);

	/* fix the bias is all other blocks */
	bw.env = env;
	bw.start_block = get_irg_start_block(irg);
	irg_block_walk_graph(irg, stack_bias_walker, NULL, &bw);

	/* fix now inner functions: these still have Sel node to outer
	   frame and parameter entities */
	frame_tp = get_irg_frame_type(irg);
	for (i = get_class_n_members(frame_tp) - 1; i >= 0; --i) {
		ir_entity *ent = get_class_member(frame_tp, i);
		ir_graph  *irg = get_entity_irg(ent);

		if (irg != NULL) {
			irg_walk_graph(irg, NULL, lower_outer_frame_sels, env);
		}
	}
}

ir_node *be_abi_get_callee_save_irn(be_abi_irg_t *abi, const arch_register_t *reg)
{
	assert(arch_register_type_is(reg, callee_save));
	assert(pmap_contains(abi->regs, (void *) reg));
	return pmap_get(abi->regs, (void *) reg);
}

ir_node *be_abi_get_ignore_irn(be_abi_irg_t *abi, const arch_register_t *reg)
{
	assert(arch_register_type_is(reg, ignore));
	assert(pmap_contains(abi->regs, (void *) reg));
	return pmap_get(abi->regs, (void *) reg);
}

/**
 * Returns non-zero if the ABI has omitted the frame pointer in
 * the current graph.
 */
int be_abi_omit_fp(const be_abi_irg_t *abi)
{
	return abi->call->flags.bits.try_omit_fp;
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_abi);
void be_init_abi(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.abi");
}
