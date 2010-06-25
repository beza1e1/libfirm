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
 * @brief   The main arm backend driver file.
 * @author  Matthias Braun, Oliver Richter, Tobias Gneist
 * @version $Id$
 */
#include "config.h"

#include "lc_opts.h"
#include "lc_opts_enum.h"

#include "pseudo_irg.h"
#include "irgwalk.h"
#include "irprog.h"
#include "irprintf.h"
#include "ircons.h"
#include "irgmod.h"
#include "irgopt.h"
#include "iroptimize.h"
#include "irdump.h"
#include "lowering.h"
#include "error.h"

#include "bitset.h"
#include "debug.h"
#include "array_t.h"
#include "irtools.h"

#include "../bearch.h"
#include "../benode.h"
#include "../belower.h"
#include "../besched.h"
#include "be.h"
#include "../beabi.h"
#include "../bemachine.h"
#include "../beilpsched.h"
#include "../bemodule.h"
#include "../beirg.h"
#include "../bespillslots.h"
#include "../begnuas.h"
#include "../belistsched.h"
#include "../beflags.h"

#include "bearch_arm_t.h"

#include "arm_new_nodes.h"
#include "gen_arm_regalloc_if.h"
#include "arm_transform.h"
#include "arm_optimize.h"
#include "arm_emitter.h"
#include "arm_map_regs.h"

static arch_irn_class_t arm_classify(const ir_node *irn)
{
	(void) irn;
	return 0;
}

static ir_entity *arm_get_frame_entity(const ir_node *irn)
{
	const arm_attr_t *attr = get_arm_attr_const(irn);

	if (is_arm_FrameAddr(irn)) {
		const arm_SymConst_attr_t *attr = get_irn_generic_attr_const(irn);
		return attr->entity;
	}
	if (attr->is_load_store) {
		const arm_load_store_attr_t *load_store_attr
			= get_arm_load_store_attr_const(irn);
		if (load_store_attr->is_frame_entity) {
			return load_store_attr->entity;
		}
	}
	return NULL;
}

static void arm_set_frame_entity(ir_node *irn, ir_entity *ent)
{
	(void) irn;
	(void) ent;
	panic("arm_set_frame_entity() called. This should not happen.");
}

/**
 * This function is called by the generic backend to correct offsets for
 * nodes accessing the stack.
 */
static void arm_set_stack_bias(ir_node *irn, int bias)
{
	if (is_arm_FrameAddr(irn)) {
		arm_SymConst_attr_t *attr = get_irn_generic_attr(irn);
		attr->fp_offset += bias;
	} else {
		arm_load_store_attr_t *attr = get_arm_load_store_attr(irn);
		assert(attr->base.is_load_store);
		attr->offset += bias;
	}
}

static int arm_get_sp_bias(const ir_node *irn)
{
	/* We don't have any nodes changing the stack pointer.
		TODO: we probably want to support post-/pre increment/decrement later */
	(void) irn;
	return 0;
}

/* fill register allocator interface */

static const arch_irn_ops_t arm_irn_ops = {
	get_arm_in_req,
	arm_classify,
	arm_get_frame_entity,
	arm_set_frame_entity,
	arm_set_stack_bias,
	arm_get_sp_bias,
	NULL,    /* get_inverse             */
	NULL,    /* get_op_estimated_cost   */
	NULL,    /* possible_memory_operand */
	NULL,    /* perform_memory_operand  */
};

/**
 * Transforms the standard Firm graph into
 * a ARM firm graph.
 */
static void arm_prepare_graph(void *self)
{
	arm_code_gen_t *cg = self;

	/* transform nodes into assembler instructions */
	arm_transform_graph(cg);

	/* do local optimizations (mainly CSE) */
	local_optimize_graph(cg->irg);

	if (cg->dump)
		dump_ir_graph(cg->irg, "transformed");

	/* do code placement, to optimize the position of constants */
	place_code(cg->irg);

	if (cg->dump)
		dump_ir_graph(cg->irg, "place");
}

/**
 * Called immediately before emit phase.
 */
static void arm_finish_irg(void *self)
{
	arm_code_gen_t *cg = self;

	/* do peephole optimizations and fix stack offsets */
	arm_peephole_optimization(cg);
}

static ir_node *arm_flags_remat(ir_node *node, ir_node *after)
{
	ir_node *block;
	ir_node *copy;

	if (is_Block(after)) {
		block = after;
	} else {
		block = get_nodes_block(after);
	}
	copy = exact_copy(node);
	set_nodes_block(copy, block);
	sched_add_after(after, copy);
	return copy;
}

static void arm_before_ra(void *self)
{
	arm_code_gen_t *cg = self;

	be_sched_fix_flags(cg->birg, &arm_reg_classes[CLASS_arm_flags],
	                   &arm_flags_remat);
}

static void transform_Reload(ir_node *node)
{
	ir_graph  *irg    = get_irn_irg(node);
	ir_node   *block  = get_nodes_block(node);
	dbg_info  *dbgi   = get_irn_dbg_info(node);
	ir_node   *ptr    = get_irg_frame(irg);
	ir_node   *mem    = get_irn_n(node, be_pos_Reload_mem);
	ir_mode   *mode   = get_irn_mode(node);
	ir_entity *entity = be_get_frame_entity(node);
	const arch_register_t *reg;
	ir_node   *proj;
	ir_node   *load;

	ir_node  *sched_point = sched_prev(node);

	load = new_bd_arm_Ldr(dbgi, block, ptr, mem, mode, entity, false, 0, true);
	sched_add_after(sched_point, load);
	sched_remove(node);

	proj = new_rd_Proj(dbgi, load, mode, pn_arm_Ldr_res);

	reg = arch_get_irn_register(node);
	arch_set_irn_register(proj, reg);

	exchange(node, proj);
}

static void transform_Spill(ir_node *node)
{
	ir_graph  *irg    = get_irn_irg(node);
	ir_node   *block  = get_nodes_block(node);
	dbg_info  *dbgi   = get_irn_dbg_info(node);
	ir_node   *ptr    = get_irg_frame(irg);
	ir_node   *mem    = new_NoMem();
	ir_node   *val    = get_irn_n(node, be_pos_Spill_val);
	ir_mode   *mode   = get_irn_mode(val);
	ir_entity *entity = be_get_frame_entity(node);
	ir_node   *sched_point;
	ir_node   *store;

	sched_point = sched_prev(node);
	store = new_bd_arm_Str(dbgi, block, ptr, val, mem, mode, entity, false, 0,
	                       true);

	sched_remove(node);
	sched_add_after(sched_point, store);

	exchange(node, store);
}

static void arm_after_ra_walker(ir_node *block, void *data)
{
	ir_node *node, *prev;
	(void) data;

	for (node = sched_last(block); !sched_is_begin(node); node = prev) {
		prev = sched_prev(node);

		if (be_is_Reload(node)) {
			transform_Reload(node);
		} else if (be_is_Spill(node)) {
			transform_Spill(node);
		}
	}
}

static void arm_after_ra(void *self)
{
	arm_code_gen_t *cg = self;
	be_coalesce_spillslots(cg->birg);

	irg_block_walk_graph(cg->irg, NULL, arm_after_ra_walker, NULL);
}

/**
 * Emits the code, closes the output file and frees
 * the code generator interface.
 */
static void arm_emit_and_done(void *self)
{
	arm_code_gen_t *cg = self;
	ir_graph       *irg = cg->irg;

	arm_gen_routine(cg, irg);

	/* de-allocate code generator */
	del_set(cg->reg_set);
	free(self);
}

/**
 * Move a double floating point value into an integer register.
 * Place the move operation into block bl.
 *
 * Handle some special cases here:
 * 1.) A constant: simply split into two
 * 2.) A load: simply split into two
 */
static ir_node *convert_dbl_to_int(ir_node *bl, ir_node *arg, ir_node *mem,
                                   ir_node **resH, ir_node **resL)
{
	if (is_Const(arg)) {
		tarval *tv = get_Const_tarval(arg);
		unsigned v;

		/* get the upper 32 bits */
		v =            get_tarval_sub_bits(tv, 7);
		v = (v << 8) | get_tarval_sub_bits(tv, 6);
		v = (v << 8) | get_tarval_sub_bits(tv, 5);
		v = (v << 8) | get_tarval_sub_bits(tv, 4);
		*resH = new_Const_long(mode_Is, v);

		/* get the lower 32 bits */
		v =            get_tarval_sub_bits(tv, 3);
		v = (v << 8) | get_tarval_sub_bits(tv, 2);
		v = (v << 8) | get_tarval_sub_bits(tv, 1);
		v = (v << 8) | get_tarval_sub_bits(tv, 0);
		*resL = new_Const_long(mode_Is, v);
	} else if (is_Load(skip_Proj(arg))) {
		/* FIXME: handling of low/high depends on LE/BE here */
		panic("Unimplemented convert_dbl_to_int() case");
	}
	else {
		ir_node *conv;

		conv = new_bd_arm_fpaDbl2GP(NULL, bl, arg, mem);
		/* move high/low */
		*resL = new_r_Proj(conv, mode_Is, pn_arm_fpaDbl2GP_low);
		*resH = new_r_Proj(conv, mode_Is, pn_arm_fpaDbl2GP_high);
		mem   = new_r_Proj(conv, mode_M,  pn_arm_fpaDbl2GP_M);
	}
	return mem;
}

/**
 * Move a single floating point value into an integer register.
 * Place the move operation into block bl.
 *
 * Handle some special cases here:
 * 1.) A constant: simply move
 * 2.) A load: simply load
 */
static ir_node *convert_sng_to_int(ir_node *bl, ir_node *arg)
{
	(void) bl;

	if (is_Const(arg)) {
		tarval *tv = get_Const_tarval(arg);
		unsigned v;

		/* get the lower 32 bits */
		v =            get_tarval_sub_bits(tv, 3);
		v = (v << 8) | get_tarval_sub_bits(tv, 2);
		v = (v << 8) | get_tarval_sub_bits(tv, 1);
		v = (v << 8) | get_tarval_sub_bits(tv, 0);
		return new_Const_long(mode_Is, v);
	}
	panic("Unimplemented convert_sng_to_int() case");
}

/**
 * Convert the arguments of a call to support the
 * ARM calling convention of general purpose AND floating
 * point arguments.
 */
static void handle_calls(ir_node *call, void *env)
{
	arm_code_gen_t *cg = env;
	int i, j, n, size, idx, flag, n_param, n_res, first_variadic;
	ir_type *mtp, *new_mtd, *new_tp[5];
	ir_node *new_in[5], **in;
	ir_node *bl;

	if (! is_Call(call))
		return;

	/* check, if we need conversions */
	n = get_Call_n_params(call);
	mtp = get_Call_type(call);
	assert(get_method_n_params(mtp) == n);

	/* it's always enough to handle the first 4 parameters */
	if (n > 4)
		n = 4;
	flag = size = idx = 0;
	bl = get_nodes_block(call);
	for (i = 0; i < n; ++i) {
		ir_type *param_tp = get_method_param_type(mtp, i);

		if (is_compound_type(param_tp)) {
			/* an aggregate parameter: bad case */
			assert(0);
		}
		else {
			/* a primitive parameter */
			ir_mode *mode = get_type_mode(param_tp);

			if (mode_is_float(mode)) {
				if (get_mode_size_bits(mode) > 32) {
					ir_node *mem = get_Call_mem(call);

					/* Beware: ARM wants the high part first */
					size += 2 * 4;
					new_tp[idx]   = cg->int_tp;
					new_tp[idx+1] = cg->int_tp;
					mem = convert_dbl_to_int(bl, get_Call_param(call, i), mem, &new_in[idx], &new_in[idx+1]);
					idx += 2;
					set_Call_mem(call, mem);
				}
				else {
					size += 4;
					new_tp[idx] = cg->int_tp;
					new_in[idx] = convert_sng_to_int(bl, get_Call_param(call, i));
					++idx;
				}
				flag = 1;
			}
			else {
				size += 4;
				new_tp[idx] = param_tp;
				new_in[idx] = get_Call_param(call, i);
				++idx;
			}
		}

		if (size >= 16)
			break;
	}

	/* if flag is NOT set, no need to translate the method type */
	if (! flag)
		return;

	/* construct a new method type */
	n       = i;
	n_param = get_method_n_params(mtp) - n + idx;
	n_res   = get_method_n_ress(mtp);
	new_mtd = new_d_type_method(n_param, n_res, get_type_dbg_info(mtp));

	for (i = 0; i < idx; ++i)
		set_method_param_type(new_mtd, i, new_tp[i]);
	for (i = n, j = idx; i < get_method_n_params(mtp); ++i)
		set_method_param_type(new_mtd, j++, get_method_param_type(mtp, i));
	for (i = 0; i < n_res; ++i)
		set_method_res_type(new_mtd, i, get_method_res_type(mtp, i));

	set_method_calling_convention(new_mtd, get_method_calling_convention(mtp));
	first_variadic = get_method_first_variadic_param_index(mtp);
	if (first_variadic >= 0)
		set_method_first_variadic_param_index(new_mtd, first_variadic);

	if (is_lowered_type(mtp)) {
		mtp = get_associated_type(mtp);
	}
	set_lowered_type(mtp, new_mtd);

	set_Call_type(call, new_mtd);

	/* calculate new in array of the Call */
	NEW_ARR_A(ir_node *, in, n_param + 2);
	for (i = 0; i < idx; ++i)
		in[2 + i] = new_in[i];
	for (i = n, j = idx; i < get_method_n_params(mtp); ++i)
		in[2 + j++] = get_Call_param(call, i);

	in[0] = get_Call_mem(call);
	in[1] = get_Call_ptr(call);

	/* finally, change the call inputs */
	set_irn_in(call, n_param + 2, in);
}

/**
 * Handle graph transformations before the abi converter does its work.
 */
static void arm_before_abi(void *self)
{
	arm_code_gen_t *cg = self;

	irg_walk_graph(cg->irg, NULL, handle_calls, cg);
}

/* forward */
static void *arm_cg_init(be_irg_t *birg);

static const arch_code_generator_if_t arm_code_gen_if = {
	arm_cg_init,
	NULL,               /* get_pic_base */
	arm_before_abi,     /* before abi introduce */
	arm_prepare_graph,
	NULL,               /* spill */
	arm_before_ra,      /* before register allocation hook */
	arm_after_ra,
	arm_finish_irg,
	arm_emit_and_done,
};

/**
 * Initializes the code generator.
 */
static void *arm_cg_init(be_irg_t *birg)
{
	static ir_type *int_tp = NULL;
	arm_isa_t      *isa = (arm_isa_t *)birg->main_env->arch_env;
	arm_code_gen_t *cg;

	if (! int_tp) {
		/* create an integer type with machine size */
		int_tp = new_type_primitive(mode_Is);
	}

	cg = XMALLOC(arm_code_gen_t);
	cg->impl         = &arm_code_gen_if;
	cg->irg          = birg->irg;
	cg->reg_set      = new_set(arm_cmp_irn_reg_assoc, 1024);
	cg->isa          = isa;
	cg->birg         = birg;
	cg->int_tp       = int_tp;
	cg->have_fp_insn = 0;
	cg->dump         = (birg->main_env->options->dump_flags & DUMP_BE) ? 1 : 0;

	FIRM_DBG_REGISTER(cg->mod, "firm.be.arm.cg");

	/* enter the current code generator */
	isa->cg = cg;

	return (arch_code_generator_t *)cg;
}


/**
 * Maps all intrinsic calls that the backend support
 * and map all instructions the backend did not support
 * to runtime calls.
 */
static void arm_handle_intrinsics(void)
{
	ir_type *tp, *int_tp, *uint_tp;
	i_record records[8];
	int n_records = 0;

	runtime_rt rt_iDiv, rt_uDiv, rt_iMod, rt_uMod;

#define ID(x) new_id_from_chars(x, sizeof(x)-1)

	int_tp  = new_type_primitive(mode_Is);
	uint_tp = new_type_primitive(mode_Iu);

	/* ARM has neither a signed div instruction ... */
	{
		i_instr_record *map_Div = &records[n_records++].i_instr;

		tp = new_type_method(2, 1);
		set_method_param_type(tp, 0, int_tp);
		set_method_param_type(tp, 1, int_tp);
		set_method_res_type(tp, 0, int_tp);

		rt_iDiv.ent             = new_entity(get_glob_type(), ID("__divsi3"), tp);
		set_entity_ld_ident(rt_iDiv.ent, ID("__divsi3"));
		rt_iDiv.mode            = mode_T;
		rt_iDiv.res_mode        = mode_Is;
		rt_iDiv.mem_proj_nr     = pn_Div_M;
		rt_iDiv.regular_proj_nr = pn_Div_X_regular;
		rt_iDiv.exc_proj_nr     = pn_Div_X_except;
		rt_iDiv.exc_mem_proj_nr = pn_Div_M;
		rt_iDiv.res_proj_nr     = pn_Div_res;

		add_entity_linkage(rt_iDiv.ent, IR_LINKAGE_CONSTANT);
		set_entity_visibility(rt_iDiv.ent, ir_visibility_external);

		map_Div->kind     = INTRINSIC_INSTR;
		map_Div->op       = op_Div;
		map_Div->i_mapper = (i_mapper_func)i_mapper_RuntimeCall;
		map_Div->ctx      = &rt_iDiv;
	}
	/* ... nor an unsigned div instruction ... */
	{
		i_instr_record *map_Div = &records[n_records++].i_instr;

		tp = new_type_method(2, 1);
		set_method_param_type(tp, 0, uint_tp);
		set_method_param_type(tp, 1, uint_tp);
		set_method_res_type(tp, 0, uint_tp);

		rt_uDiv.ent             = new_entity(get_glob_type(), ID("__udivsi3"), tp);
		set_entity_ld_ident(rt_uDiv.ent, ID("__udivsi3"));
		rt_uDiv.mode            = mode_T;
		rt_uDiv.res_mode        = mode_Iu;
		rt_uDiv.mem_proj_nr     = pn_Div_M;
		rt_uDiv.regular_proj_nr = pn_Div_X_regular;
		rt_uDiv.exc_proj_nr     = pn_Div_X_except;
		rt_uDiv.exc_mem_proj_nr = pn_Div_M;
		rt_uDiv.res_proj_nr     = pn_Div_res;

		set_entity_visibility(rt_uDiv.ent, ir_visibility_external);

		map_Div->kind     = INTRINSIC_INSTR;
		map_Div->op       = op_Div;
		map_Div->i_mapper = (i_mapper_func)i_mapper_RuntimeCall;
		map_Div->ctx      = &rt_uDiv;
	}
	/* ... nor a signed mod instruction ... */
	{
		i_instr_record *map_Mod = &records[n_records++].i_instr;

		tp = new_type_method(2, 1);
		set_method_param_type(tp, 0, int_tp);
		set_method_param_type(tp, 1, int_tp);
		set_method_res_type(tp, 0, int_tp);

		rt_iMod.ent             = new_entity(get_glob_type(), ID("__modsi3"), tp);
		set_entity_ld_ident(rt_iMod.ent, ID("__modsi3"));
		rt_iMod.mode            = mode_T;
		rt_iMod.res_mode        = mode_Is;
		rt_iMod.mem_proj_nr     = pn_Mod_M;
		rt_iMod.regular_proj_nr = pn_Mod_X_regular;
		rt_iMod.exc_proj_nr     = pn_Mod_X_except;
		rt_iMod.exc_mem_proj_nr = pn_Mod_M;
		rt_iMod.res_proj_nr     = pn_Mod_res;

		set_entity_visibility(rt_iMod.ent, ir_visibility_external);

		map_Mod->kind     = INTRINSIC_INSTR;
		map_Mod->op       = op_Mod;
		map_Mod->i_mapper = (i_mapper_func)i_mapper_RuntimeCall;
		map_Mod->ctx      = &rt_iMod;
	}
	/* ... nor an unsigned mod. */
	{
		i_instr_record *map_Mod = &records[n_records++].i_instr;

		tp = new_type_method(2, 1);
		set_method_param_type(tp, 0, uint_tp);
		set_method_param_type(tp, 1, uint_tp);
		set_method_res_type(tp, 0, uint_tp);

		rt_uMod.ent             = new_entity(get_glob_type(), ID("__umodsi3"), tp);
		set_entity_ld_ident(rt_uMod.ent, ID("__umodsi3"));
		rt_uMod.mode            = mode_T;
		rt_uMod.res_mode        = mode_Iu;
		rt_uMod.mem_proj_nr     = pn_Mod_M;
		rt_uMod.regular_proj_nr = pn_Mod_X_regular;
		rt_uMod.exc_proj_nr     = pn_Mod_X_except;
		rt_uMod.exc_mem_proj_nr = pn_Mod_M;
		rt_uMod.res_proj_nr     = pn_Mod_res;

		set_entity_visibility(rt_uMod.ent, ir_visibility_external);

		map_Mod->kind     = INTRINSIC_INSTR;
		map_Mod->op       = op_Mod;
		map_Mod->i_mapper = (i_mapper_func)i_mapper_RuntimeCall;
		map_Mod->ctx      = &rt_uMod;
	}

	if (n_records > 0)
		lower_intrinsics(records, n_records, /*part_block_used=*/0);
}

/*****************************************************************
 *  ____             _                  _   _____  _____
 * |  _ \           | |                | | |_   _|/ ____|  /\
 * | |_) | __ _  ___| | _____ _ __   __| |   | | | (___   /  \
 * |  _ < / _` |/ __| |/ / _ \ '_ \ / _` |   | |  \___ \ / /\ \
 * | |_) | (_| | (__|   <  __/ | | | (_| |  _| |_ ____) / ____ \
 * |____/ \__,_|\___|_|\_\___|_| |_|\__,_| |_____|_____/_/    \_\
 *
 *****************************************************************/

static arm_isa_t arm_isa_template = {
	{
		&arm_isa_if,           /* isa interface */
		&arm_gp_regs[REG_SP],  /* stack pointer */
		&arm_gp_regs[REG_R11], /* base pointer */
		&arm_reg_classes[CLASS_arm_gp],  /* static link pointer class */
		-1,                    /* stack direction */
		2,                     /* power of two stack alignment for calls, 2^2 == 4 */
		NULL,                  /* main environment */
		7,                     /* spill costs */
		5,                     /* reload costs */
	},
	0,                     /* use generic register names instead of SP, LR, PC */
	ARM_FPU_ARCH_FPE,      /* FPU architecture */
	NULL,                  /* current code generator */
};

/**
 * Initializes the backend ISA and opens the output file.
 */
static arch_env_t *arm_init(FILE *file_handle)
{
	static int inited = 0;
	arm_isa_t *isa;

	if (inited)
		return NULL;

	isa = XMALLOC(arm_isa_t);
	memcpy(isa, &arm_isa_template, sizeof(*isa));

	arm_register_init();

	isa->cg  = NULL;
	be_emit_init(file_handle);

	arm_create_opcodes(&arm_irn_ops);
	arm_handle_intrinsics();

	be_gas_emit_types = false;

	/* needed for the debug support */
	be_gas_emit_switch_section(GAS_SECTION_TEXT);
	be_emit_irprintf("%stext0:\n", be_gas_get_private_prefix());
	be_emit_write_line();

	inited = 1;
	return &isa->arch_env;
}



/**
 * Closes the output file and frees the ISA structure.
 */
static void arm_done(void *self)
{
	arm_isa_t *isa = self;

	be_gas_emit_decls(isa->arch_env.main_env);

	be_emit_exit();
	free(self);
}


/**
 * Report the number of register classes.
 * If we don't have fp instructions, report only GP
 * here to speed up register allocation (and makes dumps
 * smaller and more readable).
 */
static unsigned arm_get_n_reg_class(void)
{
	return N_CLASSES;
}

/**
 * Return the register class with requested index.
 */
static const arch_register_class_t *arm_get_reg_class(unsigned i)
{
	assert(i < N_CLASSES);
	return &arm_reg_classes[i];
}

/**
 * Get the register class which shall be used to store a value of a given mode.
 * @param self The this pointer.
 * @param mode The mode in question.
 * @return A register class which can hold values of the given mode.
 */
static const arch_register_class_t *arm_get_reg_class_for_mode(const ir_mode *mode)
{
	if (mode_is_float(mode))
		return &arm_reg_classes[CLASS_arm_fpa];
	else
		return &arm_reg_classes[CLASS_arm_gp];
}

/**
 * Produces the type which sits between the stack args and the locals on the stack.
 * it will contain the return address and space to store the old base pointer.
 * @return The Firm type modeling the ABI between type.
 */
static ir_type *arm_get_between_type(void *self)
{
	static ir_type *between_type = NULL;
	(void) self;

	if (between_type == NULL) {
		between_type = new_type_class(new_id_from_str("arm_between_type"));
		set_type_size_bytes(between_type, 0);
	}

	return between_type;
}


typedef struct {
	be_abi_call_flags_bits_t flags;
	const arch_env_t *arch_env;
	ir_graph *irg;
} arm_abi_env_t;

static void *arm_abi_init(const be_abi_call_t *call, const arch_env_t *arch_env, ir_graph *irg)
{
	arm_abi_env_t       *env = XMALLOC(arm_abi_env_t);
	be_abi_call_flags_t  fl  = be_abi_call_get_flags(call);
	env->flags    = fl.bits;
	env->irg      = irg;
	env->arch_env = arch_env;
	return env;
}

/**
 * Generate the routine prologue.
 *
 * @param self       The callback object.
 * @param mem        A pointer to the mem node. Update this if you define new memory.
 * @param reg_map    A map mapping all callee_save/ignore/parameter registers to their defining nodes.
 * @param stack_bias Points to the current stack bias, can be modified if needed.
 *
 * @return        The register which shall be used as a stack frame base.
 *
 * All nodes which define registers in @p reg_map must keep @p reg_map current.
 */
static const arch_register_t *arm_abi_prologue(void *self, ir_node **mem, pmap *reg_map, int *stack_bias)
{
	arm_abi_env_t         *env = self;
	ir_node               *store;
	ir_graph              *irg;
	ir_node               *block;
	arch_register_class_t *gp;

	ir_node               *fp, *ip, *lr, *pc;
	ir_node               *sp = be_abi_reg_map_get(reg_map, env->arch_env->sp);

	(void) stack_bias;

	if (env->flags.try_omit_fp)
		return env->arch_env->sp;

	fp = be_abi_reg_map_get(reg_map, env->arch_env->bp);
	ip = be_abi_reg_map_get(reg_map, &arm_gp_regs[REG_R12]);
	lr = be_abi_reg_map_get(reg_map, &arm_gp_regs[REG_LR]);
	pc = be_abi_reg_map_get(reg_map, &arm_gp_regs[REG_PC]);

	gp    = &arm_reg_classes[CLASS_arm_gp];
	irg   = env->irg;
	block = get_irg_start_block(irg);

	/* mark bp register as ignore */
	be_set_constr_single_reg_out(get_Proj_pred(fp),
	                             get_Proj_proj(fp), env->arch_env->bp,
	                             arch_register_req_type_ignore);

	/* copy SP to IP (so we can spill it */
	ip = be_new_Copy(gp, block, sp);
	be_set_constr_single_reg_out(ip, 0, &arm_gp_regs[REG_R12], 0);

	/* spill stuff */
	store = new_bd_arm_StoreStackM4Inc(NULL, block, sp, fp, ip, lr, pc, *mem);

	sp = new_r_Proj(store, env->arch_env->sp->reg_class->mode, pn_arm_StoreStackM4Inc_ptr);
	arch_set_irn_register(sp, env->arch_env->sp);
	*mem = new_r_Proj(store, mode_M, pn_arm_StoreStackM4Inc_M);

	/* frame pointer is ip-4 (because ip is our old sp value) */
	fp = new_bd_arm_Sub_imm(NULL, block, ip, 4, 0);
	arch_set_irn_register(fp, env->arch_env->bp);

	/* beware: we change the fp but the StoreStackM4Inc above wants the old
	 * fp value. We are not allowed to spill or anything in the prolog, so we
	 * have to enforce some order here. (scheduler/regalloc are too stupid
	 * to extract this order from register requirements) */
	add_irn_dep(fp, store);

	fp = be_new_Copy(gp, block, fp); // XXX Gammelfix: only be_ have custom register requirements
	be_set_constr_single_reg_out(fp, 0, env->arch_env->bp,
	                             arch_register_req_type_ignore);
	arch_set_irn_register(fp, env->arch_env->bp);

	be_abi_reg_map_set(reg_map, env->arch_env->bp, fp);
	be_abi_reg_map_set(reg_map, &arm_gp_regs[REG_R12], ip);
	be_abi_reg_map_set(reg_map, env->arch_env->sp, sp);
	be_abi_reg_map_set(reg_map, &arm_gp_regs[REG_LR], lr);
	be_abi_reg_map_set(reg_map, &arm_gp_regs[REG_PC], pc);

	return env->arch_env->bp;
}

/**
 * Builds the ARM epilogue
 */
static void arm_abi_epilogue(void *self, ir_node *bl, ir_node **mem, pmap *reg_map)
{
	arm_abi_env_t *env = self;
	ir_node *curr_sp = be_abi_reg_map_get(reg_map, env->arch_env->sp);
	ir_node *curr_bp = be_abi_reg_map_get(reg_map, env->arch_env->bp);
	ir_node *curr_pc = be_abi_reg_map_get(reg_map, &arm_gp_regs[REG_PC]);
	ir_node	*curr_lr = be_abi_reg_map_get(reg_map, &arm_gp_regs[REG_LR]);

	// TODO: Activate Omit fp in epilogue
	if (env->flags.try_omit_fp) {
		ir_node *incsp = be_new_IncSP(env->arch_env->sp, bl, curr_sp, BE_STACK_FRAME_SIZE_SHRINK, 0);
		curr_sp = incsp;
	} else {
		ir_node *load_node;

		load_node = new_bd_arm_LoadStackM3Epilogue(NULL, bl, curr_bp, *mem);

		curr_bp = new_r_Proj(load_node, env->arch_env->bp->reg_class->mode, pn_arm_LoadStackM3Epilogue_res0);
		curr_sp = new_r_Proj(load_node, env->arch_env->sp->reg_class->mode, pn_arm_LoadStackM3Epilogue_res1);
		curr_pc = new_r_Proj(load_node, mode_Iu, pn_arm_LoadStackM3Epilogue_res2);
		*mem    = new_r_Proj(load_node, mode_M, pn_arm_LoadStackM3Epilogue_M);
		arch_set_irn_register(curr_bp, env->arch_env->bp);
		arch_set_irn_register(curr_sp, env->arch_env->sp);
		arch_set_irn_register(curr_pc, &arm_gp_regs[REG_PC]);
	}
	be_abi_reg_map_set(reg_map, env->arch_env->sp, curr_sp);
	be_abi_reg_map_set(reg_map, env->arch_env->bp, curr_bp);
	be_abi_reg_map_set(reg_map, &arm_gp_regs[REG_LR], curr_lr);
	be_abi_reg_map_set(reg_map, &arm_gp_regs[REG_PC], curr_pc);
}

static const be_abi_callbacks_t arm_abi_callbacks = {
	arm_abi_init,
	free,
	arm_get_between_type,
	arm_abi_prologue,
	arm_abi_epilogue,
};


/**
 * Get the ABI restrictions for procedure calls.
 * @param self        The this pointer.
 * @param method_type The type of the method (procedure) in question.
 * @param abi         The abi object to be modified
 */
static void arm_get_call_abi(const void *self, ir_type *method_type, be_abi_call_t *abi)
{
	ir_type  *tp;
	ir_mode  *mode;
	int       i;
	int       n = get_method_n_params(method_type);
	be_abi_call_flags_t call_flags = be_abi_call_get_flags(abi);
	(void) self;

	/* set abi flags for calls */
	call_flags.bits.left_to_right         = 0;
	call_flags.bits.store_args_sequential = 0;
	/* call_flags.bits.try_omit_fp     don't change this we can handle both */
	call_flags.bits.fp_free               = 0;
	call_flags.bits.call_has_imm          = 1;  /* IA32 calls can have immediate address */

	/* set stack parameter passing style */
	be_abi_call_set_flags(abi, call_flags, &arm_abi_callbacks);

	for (i = 0; i < n; i++) {
		/* reg = get reg for param i;          */
		/* be_abi_call_param_reg(abi, i, reg); */
		if (i < 4) {
			be_abi_call_param_reg(abi, i, arm_get_RegParam_reg(i), ABI_CONTEXT_BOTH);
		} else {
			tp   = get_method_param_type(method_type, i);
			mode = get_type_mode(tp);
			be_abi_call_param_stack(abi, i, mode, 4, 0, 0, ABI_CONTEXT_BOTH);
		}
	}

	/* set return registers */
	n = get_method_n_ress(method_type);

	assert(n <= 2 && "more than two results not supported");

	/* In case of 64bit returns, we will have two 32bit values */
	if (n == 2) {
		tp   = get_method_res_type(method_type, 0);
		mode = get_type_mode(tp);

		assert(!mode_is_float(mode) && "two FP results not supported");

		tp   = get_method_res_type(method_type, 1);
		mode = get_type_mode(tp);

		assert(!mode_is_float(mode) && "mixed INT, FP results not supported");

		be_abi_call_res_reg(abi, 0, &arm_gp_regs[REG_R0], ABI_CONTEXT_BOTH);
		be_abi_call_res_reg(abi, 1, &arm_gp_regs[REG_R1], ABI_CONTEXT_BOTH);
	} else if (n == 1) {
		const arch_register_t *reg;

		tp   = get_method_res_type(method_type, 0);
		assert(is_atomic_type(tp));
		mode = get_type_mode(tp);

		reg = mode_is_float(mode) ? &arm_fpa_regs[REG_F0] : &arm_gp_regs[REG_R0];
		be_abi_call_res_reg(abi, 0, reg, ABI_CONTEXT_BOTH);
	}
}

static int arm_to_appear_in_schedule(void *block_env, const ir_node *irn)
{
	(void) block_env;
	if (!is_arm_irn(irn))
		return -1;

	return 1;
}

/**
 * Initializes the code generator interface.
 */
static const arch_code_generator_if_t *arm_get_code_generator_if(void *self)
{
	(void) self;
	return &arm_code_gen_if;
}

list_sched_selector_t arm_sched_selector;

/**
 * Returns the reg_pressure scheduler with to_appear_in_schedule() over\loaded
 */
static const list_sched_selector_t *arm_get_list_sched_selector(const void *self, list_sched_selector_t *selector)
{
	(void) self;
	memcpy(&arm_sched_selector, selector, sizeof(arm_sched_selector));
	/* arm_sched_selector.exectime              = arm_sched_exectime; */
	arm_sched_selector.to_appear_in_schedule = arm_to_appear_in_schedule;
	return &arm_sched_selector;

}

static const ilp_sched_selector_t *arm_get_ilp_sched_selector(const void *self)
{
	(void) self;
	return NULL;
}

/**
 * Returns the necessary byte alignment for storing a register of given class.
 */
static int arm_get_reg_class_alignment(const arch_register_class_t *cls)
{
	(void) cls;
	/* ARM is a 32 bit CPU, no need for other alignment */
	return 4;
}

static const be_execution_unit_t ***arm_get_allowed_execution_units(const ir_node *irn)
{
	(void) irn;
	/* TODO */
	panic("Unimplemented arm_get_allowed_execution_units()");
}

static const be_machine_t *arm_get_machine(const void *self)
{
	(void) self;
	/* TODO */
	panic("Unimplemented arm_get_machine()");
}

/**
 * Return irp irgs in the desired order.
 */
static ir_graph **arm_get_irg_list(const void *self, ir_graph ***irg_list)
{
	(void) self;
	(void) irg_list;
	return NULL;
}

/**
 * Allows or disallows the creation of Psi nodes for the given Phi nodes.
 * @return 1 if allowed, 0 otherwise
 */
static int arm_is_mux_allowed(ir_node *sel, ir_node *mux_false,
                              ir_node *mux_true)
{
	(void) sel;
	(void) mux_false;
	(void) mux_true;

	return 0;
}

static asm_constraint_flags_t arm_parse_asm_constraint(const char **c)
{
	/* asm not supported */
	(void) c;
	return ASM_CONSTRAINT_FLAG_INVALID;
}

static int arm_is_valid_clobber(const char *clobber)
{
	(void) clobber;
	return 0;
}

/**
 * Returns the libFirm configuration parameter for this backend.
 */
static const backend_params *arm_get_libfirm_params(void)
{
	static const ir_settings_if_conv_t ifconv = {
		4,                    /* maxdepth, doesn't matter for Psi-conversion */
		arm_is_mux_allowed   /* allows or disallows Mux creation for given selector */
	};
	static ir_settings_arch_dep_t ad = {
		1,    /* allow subs */
		1,	  /* Muls are fast enough on ARM but ... */
		31,   /* ... one shift would be possible better */
		NULL, /* no evaluator function */
		0,    /* SMUL is needed, only in Arch M */
		0,    /* UMUL is needed, only in Arch M */
		32,   /* SMUL & UMUL available for 32 bit */
	};
	static backend_params p = {
		1,     /* need dword lowering */
		0,     /* don't support inline assembler yet */
		NULL,  /* will be set later */
		NULL,  /* but yet no creator function */
		NULL,  /* context for create_intrinsic_fkt */
		NULL,  /* ifconv_info will be set below */
		NULL,  /* float arithmetic mode (TODO) */
		0,     /* no trampoline support: size 0 */
		0,     /* no trampoline support: align 0 */
		NULL,  /* no trampoline support: no trampoline builder */
		4      /* alignment of stack parameter */
	};

	p.dep_param    = &ad;
	p.if_conv_info = &ifconv;
	return &p;
}

/* fpu set architectures. */
static const lc_opt_enum_int_items_t arm_fpu_items[] = {
	{ "softfloat", ARM_FPU_ARCH_SOFTFLOAT },
	{ "fpe",       ARM_FPU_ARCH_FPE },
	{ "fpa",       ARM_FPU_ARCH_FPA },
	{ "vfp1xd",    ARM_FPU_ARCH_VFP_V1xD },
	{ "vfp1",      ARM_FPU_ARCH_VFP_V1 },
	{ "vfp2",      ARM_FPU_ARCH_VFP_V2 },
	{ NULL,        0 }
};

static lc_opt_enum_int_var_t arch_fpu_var = {
	&arm_isa_template.fpu_arch, arm_fpu_items
};

static const lc_opt_table_entry_t arm_options[] = {
	LC_OPT_ENT_ENUM_INT("fpunit",    "select the floating point unit", &arch_fpu_var),
	LC_OPT_ENT_BOOL("gen_reg_names", "use generic register names", &arm_isa_template.gen_reg_names),
	LC_OPT_LAST
};

const arch_isa_if_t arm_isa_if = {
	arm_init,
	arm_done,
	NULL,  /* handle_intrinsics */
	arm_get_n_reg_class,
	arm_get_reg_class,
	arm_get_reg_class_for_mode,
	arm_get_call_abi,
	arm_get_code_generator_if,
	arm_get_list_sched_selector,
	arm_get_ilp_sched_selector,
	arm_get_reg_class_alignment,
	arm_get_libfirm_params,
	arm_get_allowed_execution_units,
	arm_get_machine,
	arm_get_irg_list,
	NULL,               /* mark remat */
	arm_parse_asm_constraint,
	arm_is_valid_clobber
};

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_arch_arm);
void be_init_arch_arm(void)
{
	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *arm_grp = lc_opt_get_grp(be_grp, "arm");

	lc_opt_add_table(arm_grp, arm_options);

	be_register_isa_if("arm", &arm_isa_if);

	arm_init_transform();
	arm_init_emitter();
}
