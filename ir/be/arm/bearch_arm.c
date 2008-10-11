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
 * @author  Oliver Richter, Tobias Gneist
 * @version $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

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
#include "lowering.h"
#include "error.h"

#include "bitset.h"
#include "debug.h"
#include "array_t.h"
#include "irtools.h"

#include "../bearch_t.h"                /* the general register allocator interface */
#include "../benode_t.h"
#include "../belower.h"
#include "../besched_t.h"
#include "be.h"
#include "../beabi.h"
#include "../bemachine.h"
#include "../beilpsched.h"
#include "../bemodule.h"
#include "../beirg_t.h"
#include "../bespillslots.h"
#include "../begnuas.h"

#include "bearch_arm_t.h"

#include "arm_new_nodes.h"           /* arm nodes interface */
#include "gen_arm_regalloc_if.h"     /* the generated interface (register type and class defenitions) */
#include "arm_transform.h"
#include "arm_optimize.h"
#include "arm_emitter.h"
#include "arm_map_regs.h"

#define DEBUG_MODULE "firm.be.arm.isa"

/* TODO: ugly, but we need it to get access to the registers assigned to Phi nodes */
static set *cur_reg_set = NULL;

/**************************************************
 *                         _ _              _  __
 *                        | | |            (_)/ _|
 *  _ __ ___  __ _    __ _| | | ___   ___   _| |_
 * | '__/ _ \/ _` |  / _` | | |/ _ \ / __| | |  _|
 * | | |  __/ (_| | | (_| | | | (_) | (__  | | |
 * |_|  \___|\__, |  \__,_|_|_|\___/ \___| |_|_|
 *            __/ |
 *           |___/
 **************************************************/

/**
 * Return register requirements for a arm node.
 * If the node returns a tuple (mode_T) then the proj's
 * will be asked for this information.
 */
static const arch_register_req_t *arm_get_irn_reg_req(const ir_node *node,
                                                      int pos)
{
	long               node_pos = pos == -1 ? 0 : pos;
	ir_mode           *mode     = get_irn_mode(node);

	if (is_Block(node) || mode == mode_X) {
		return arch_no_register_req;
	}

	if (mode == mode_T && pos < 0) {
		return arch_no_register_req;
	}

	if (is_Proj(node)) {
		if(mode == mode_M)
			return arch_no_register_req;

		if(pos >= 0) {
			return arch_no_register_req;
		}

		node_pos = (pos == -1) ? get_Proj_proj(node) : pos;
		node     = skip_Proj_const(node);
	}

	/* get requirements for our own nodes */
	if (is_arm_irn(node)) {
		const arch_register_req_t *req;
		if (pos >= 0) {
			req = get_arm_in_req(node, pos);
		} else {
			req = get_arm_out_req(node, node_pos);
		}

		return req;
	}

	/* unknown should be transformed by now */
	assert(!is_Unknown(node));
	return arch_no_register_req;
}

static void arm_set_irn_reg(ir_node *irn, const arch_register_t *reg)
{
	int pos = 0;

	if (get_irn_mode(irn) == mode_X) {
		return;
	}

	if (is_Proj(irn)) {
		pos = get_Proj_proj(irn);
		irn = skip_Proj(irn);
	}

	if (is_arm_irn(irn)) {
		const arch_register_t **slots;

		slots      = get_arm_slots(irn);
		slots[pos] = reg;
	}
	else {
		/* here we set the registers for the Phi nodes */
		arm_set_firm_reg(irn, reg, cur_reg_set);
	}
}

static const arch_register_t *arm_get_irn_reg(const ir_node *irn)
{
	int pos = 0;
	const arch_register_t *reg = NULL;

	if (is_Proj(irn)) {

		if (get_irn_mode(irn) == mode_X) {
			return NULL;
		}

		pos = get_Proj_proj(irn);
		irn = skip_Proj_const(irn);
	}

	if (is_arm_irn(irn)) {
		const arch_register_t **slots;
		slots = get_arm_slots(irn);
		reg   = slots[pos];
	}
	else {
		reg = arm_get_firm_reg(irn, cur_reg_set);
	}

	return reg;
}

static arch_irn_class_t arm_classify(const ir_node *irn)
{
	irn = skip_Proj_const(irn);

	if (is_cfop(irn)) {
		return arch_irn_class_branch;
	}
	else if (is_arm_irn(irn)) {
		return arch_irn_class_normal;
	}

	return 0;
}

static arch_irn_flags_t arm_get_flags(const ir_node *irn)
{
	arch_irn_flags_t flags = arch_irn_flags_none;

	if(is_Unknown(irn)) {
		return arch_irn_flags_ignore;
	}

	if (is_Proj(irn) && mode_is_datab(get_irn_mode(irn))) {
		ir_node *pred = get_Proj_pred(irn);
		if (is_arm_irn(pred)) {
			flags = get_arm_out_flags(pred, get_Proj_proj(irn));
		}
		irn = pred;
	}

	if (is_arm_irn(irn)) {
		flags |= get_arm_flags(irn);
	}

	return flags;
}

static ir_entity *arm_get_frame_entity(const ir_node *irn) {
	/* we do NOT transform be_Spill or be_Reload nodes, so we never
	   have frame access using ARM nodes. */
	(void) irn;
	return NULL;
}

static void arm_set_frame_entity(ir_node *irn, ir_entity *ent) {
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
	(void) irn;
	(void) bias;
	/* TODO: correct offset if irn accesses the stack */
}

static int arm_get_sp_bias(const ir_node *irn)
{
	(void) irn;
	return 0;
}

/* fill register allocator interface */

static const arch_irn_ops_t arm_irn_ops = {
	arm_get_irn_reg_req,
	arm_set_irn_reg,
	arm_get_irn_reg,
	arm_classify,
	arm_get_flags,
	arm_get_frame_entity,
	arm_set_frame_entity,
	arm_set_stack_bias,
	arm_get_sp_bias,
	NULL,    /* get_inverse             */
	NULL,    /* get_op_estimated_cost   */
	NULL,    /* possible_memory_operand */
	NULL,    /* perform_memory_operand  */
};

/**************************************************
 *                _                         _  __
 *               | |                       (_)/ _|
 *   ___ ___   __| | ___  __ _  ___ _ __    _| |_
 *  / __/ _ \ / _` |/ _ \/ _` |/ _ \ '_ \  | |  _|
 * | (_| (_) | (_| |  __/ (_| |  __/ | | | | | |
 *  \___\___/ \__,_|\___|\__, |\___|_| |_| |_|_|
 *                        __/ |
 *                       |___/
 **************************************************/

/**
 * Transforms the standard Firm graph into
 * a ARM firm graph.
 */
static void arm_prepare_graph(void *self) {
	arm_code_gen_t *cg = self;

	/* transform nodes into assembler instructions */
	arm_transform_graph(cg);

	/* do local optimizations (mainly CSE) */
	local_optimize_graph(cg->irg);

	if (cg->dump)
		be_dump(cg->irg, "-transformed", dump_ir_block_graph_sched);

	/* do code placement, to optimize the position of constants */
	place_code(cg->irg);

	if (cg->dump)
		be_dump(cg->irg, "-place", dump_ir_block_graph_sched);
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


/**
 * These are some hooks which must be filled but are probably not needed.
 */
static void arm_before_sched(void *self)
{
	(void) self;
	/* Some stuff you need to do after scheduling but before register allocation */
}

static void arm_before_ra(void *self)
{
	(void) self;
	/* Some stuff you need to do immediately after register allocation */
}

/**
 * We transform Spill and Reload here. This needs to be done before
 * stack biasing otherwise we would miss the corrected offset for these nodes.
 */
static void arm_after_ra(void *self)
{
	arm_code_gen_t *cg = self;
	be_coalesce_spillslots(cg->birg);
}

/**
 * Emits the code, closes the output file and frees
 * the code generator interface.
 */
static void arm_emit_and_done(void *self) {
	arm_code_gen_t *cg = self;
	ir_graph       *irg = cg->irg;

	arm_gen_routine(cg, irg);

	cur_reg_set = NULL;

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
                                   ir_node **resH, ir_node **resL) {
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
		ir_graph *irg = current_ir_graph;
		ir_node *conv;

		conv = new_rd_arm_fpaDbl2GP(NULL, irg, bl, arg, mem);
		/* move high/low */
		*resL = new_r_Proj(irg, bl, conv, mode_Is, pn_arm_fpaDbl2GP_low);
		*resH = new_r_Proj(irg, bl, conv, mode_Is, pn_arm_fpaDbl2GP_high);
		mem   = new_r_Proj(irg, bl, conv, mode_M,  pn_arm_fpaDbl2GP_M);
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
	} else if (is_Load(skip_Proj(arg))) {
		ir_node *load;

		load = skip_Proj(arg);
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
	new_mtd = new_d_type_method(get_type_ident(mtp), n_param, n_res, get_type_dbg_info(mtp));

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
static void arm_before_abi(void *self) {
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
	arm_before_sched,   /* before scheduling hook */
	arm_before_ra,      /* before register allocation hook */
	arm_after_ra,
	arm_finish_irg,
	arm_emit_and_done,
};

/**
 * Initializes the code generator.
 */
static void *arm_cg_init(be_irg_t *birg) {
	static ir_type *int_tp = NULL;
	arm_isa_t      *isa = (arm_isa_t *)birg->main_env->arch_env;
	arm_code_gen_t *cg;

	if (! int_tp) {
		/* create an integer type with machine size */
		int_tp = new_type_primitive(new_id_from_chars("int", 3), mode_Is);
	}

	cg = XMALLOC(arm_code_gen_t);
	cg->impl         = &arm_code_gen_if;
	cg->irg          = birg->irg;
	cg->reg_set      = new_set(arm_cmp_irn_reg_assoc, 1024);
	cg->arch_env     = birg->main_env->arch_env;
	cg->isa          = isa;
	cg->birg         = birg;
	cg->int_tp       = int_tp;
	cg->have_fp_insn = 0;
	cg->unknown_gp   = NULL;
	cg->unknown_fpa  = NULL;
	cg->dump         = (birg->main_env->options->dump_flags & DUMP_BE) ? 1 : 0;

	FIRM_DBG_REGISTER(cg->mod, "firm.be.arm.cg");

	cur_reg_set = cg->reg_set;

	/* enter the current code generator */
	isa->cg = cg;

	return (arch_code_generator_t *)cg;
}


/**
 * Maps all intrinsic calls that the backend support
 * and map all instructions the backend did not support
 * to runtime calls.
 */
static void arm_handle_intrinsics(void) {
	ir_type *tp, *int_tp, *uint_tp;
	i_record records[8];
	int n_records = 0;

	runtime_rt rt_iDiv, rt_uDiv, rt_iMod, rt_uMod;

#define ID(x) new_id_from_chars(x, sizeof(x)-1)

	int_tp  = new_type_primitive(ID("int"), mode_Is);
	uint_tp = new_type_primitive(ID("uint"), mode_Iu);

	/* ARM has neither a signed div instruction ... */
	{
		i_instr_record *map_Div = &records[n_records++].i_instr;

		tp = new_type_method(ID("rt_iDiv"), 2, 1);
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

		set_entity_visibility(rt_iDiv.ent, visibility_external_allocated);

		map_Div->kind     = INTRINSIC_INSTR;
		map_Div->op       = op_Div;
		map_Div->i_mapper = (i_mapper_func)i_mapper_RuntimeCall;
		map_Div->ctx      = &rt_iDiv;
	}
	/* ... nor an unsigned div instruction ... */
	{
		i_instr_record *map_Div = &records[n_records++].i_instr;

		tp = new_type_method(ID("rt_uDiv"), 2, 1);
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

		set_entity_visibility(rt_uDiv.ent, visibility_external_allocated);

		map_Div->kind     = INTRINSIC_INSTR;
		map_Div->op       = op_Div;
		map_Div->i_mapper = (i_mapper_func)i_mapper_RuntimeCall;
		map_Div->ctx      = &rt_uDiv;
	}
	/* ... nor a signed mod instruction ... */
	{
		i_instr_record *map_Mod = &records[n_records++].i_instr;

		tp = new_type_method(ID("rt_iMod"), 2, 1);
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

		set_entity_visibility(rt_iMod.ent, visibility_external_allocated);

		map_Mod->kind     = INTRINSIC_INSTR;
		map_Mod->op       = op_Mod;
		map_Mod->i_mapper = (i_mapper_func)i_mapper_RuntimeCall;
		map_Mod->ctx      = &rt_iMod;
	}
	/* ... nor an unsigned mod. */
	{
		i_instr_record *map_Mod = &records[n_records++].i_instr;

		tp = new_type_method(ID("rt_uMod"), 2, 1);
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

		set_entity_visibility(rt_uMod.ent, visibility_external_allocated);

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
static arch_env_t *arm_init(FILE *file_handle) {
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

	/* needed for the debug support */
	be_gas_emit_switch_section(GAS_SECTION_TEXT);
	be_emit_cstring(".Ltext0:\n");
	be_emit_write_line();

	/* we mark referenced global entities, so we can only emit those which
	 * are actually referenced. (Note: you mustn't use the type visited flag
	 * elsewhere in the backend)
	 */
	inc_master_type_visited();

	inited = 1;
	return &isa->arch_env;
}



/**
 * Closes the output file and frees the ISA structure.
 */
static void arm_done(void *self) {
	arm_isa_t *isa = self;

	be_gas_emit_decls(isa->arch_env.main_env, 1);

	be_emit_exit();
	free(self);
}


/**
 * Report the number of register classes.
 * If we don't have fp instructions, report only GP
 * here to speed up register allocation (and makes dumps
 * smaller and more readable).
 */
static unsigned arm_get_n_reg_class(const void *self) {
	(void) self;
	return N_CLASSES;
}

/**
 * Return the register class with requested index.
 */
static const arch_register_class_t *arm_get_reg_class(const void *self,
                                                      unsigned i) {
	(void) self;
	assert(i < N_CLASSES);
	return &arm_reg_classes[i];
}

/**
 * Get the register class which shall be used to store a value of a given mode.
 * @param self The this pointer.
 * @param mode The mode in question.
 * @return A register class which can hold values of the given mode.
 */
const arch_register_class_t *arm_get_reg_class_for_mode(const void *self, const ir_mode *mode) {
	(void) self;
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
static ir_type *arm_get_between_type(void *self) {
	static ir_type *between_type = NULL;
	static ir_entity *old_bp_ent = NULL;
	(void) self;

	if (between_type == NULL) {
		ir_entity *ret_addr_ent;
		ir_type *ret_addr_type = new_type_primitive(new_id_from_str("return_addr"), mode_P);
		ir_type *old_bp_type   = new_type_primitive(new_id_from_str("bp"), mode_P);

		between_type           = new_type_class(new_id_from_str("arm_between_type"));
		old_bp_ent             = new_entity(between_type, new_id_from_str("old_bp"), old_bp_type);
		ret_addr_ent           = new_entity(between_type, new_id_from_str("old_bp"), ret_addr_type);

		set_entity_offset(old_bp_ent, 0);
		set_entity_offset(ret_addr_ent, get_type_size_bytes(old_bp_type));
		set_type_size_bytes(between_type, get_type_size_bytes(old_bp_type) + get_type_size_bytes(ret_addr_type));
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
 * Put all registers which are saved by the prologue/epilogue in a set.
 *
 * @param self  The callback object.
 * @param s     The result set.
 */
static void arm_abi_dont_save_regs(void *self, pset *s)
{
	arm_abi_env_t *env = self;
	if (env->flags.try_omit_fp)
		pset_insert_ptr(s, env->arch_env->bp);
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
static const arch_register_t *arm_abi_prologue(void *self, ir_node **mem, pmap *reg_map, int *stack_bias) {
	arm_abi_env_t         *env = self;
	ir_node               *keep, *store;
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

	ip = be_new_Copy(gp, irg, block, sp);
	arch_set_irn_register(ip, &arm_gp_regs[REG_R12]);
	be_set_constr_single_reg(ip, BE_OUT_POS(0), &arm_gp_regs[REG_R12] );

	store = new_rd_arm_StoreStackM4Inc(NULL, irg, block, sp, fp, ip, lr, pc, *mem);

	sp = new_r_Proj(irg, block, store, env->arch_env->sp->reg_class->mode, pn_arm_StoreStackM4Inc_ptr);
	arch_set_irn_register(sp, env->arch_env->sp);
	*mem = new_r_Proj(irg, block, store, mode_M, pn_arm_StoreStackM4Inc_M);

	keep = be_new_CopyKeep_single(gp, irg, block, ip, sp, get_irn_mode(ip));
	be_node_set_reg_class(keep, 1, gp);
	arch_set_irn_register(keep, &arm_gp_regs[REG_R12]);
	be_set_constr_single_reg(keep, BE_OUT_POS(0), &arm_gp_regs[REG_R12] );

	fp = new_rd_arm_Sub_i(NULL, irg, block, keep, get_irn_mode(fp), 4);
	arch_set_irn_register(fp, env->arch_env->bp);
	fp = be_new_Copy(gp, irg, block, fp); // XXX Gammelfix: only be_ nodes can have the ignore flag set
	arch_set_irn_register(fp, env->arch_env->bp);
	be_node_set_flags(fp, BE_OUT_POS(0), arch_irn_flags_ignore);

	be_abi_reg_map_set(reg_map, env->arch_env->bp, fp);
	be_abi_reg_map_set(reg_map, &arm_gp_regs[REG_R12], keep);
	be_abi_reg_map_set(reg_map, env->arch_env->sp, sp);
	be_abi_reg_map_set(reg_map, &arm_gp_regs[REG_LR], lr);
	be_abi_reg_map_set(reg_map, &arm_gp_regs[REG_PC], pc);

	return env->arch_env->bp;
}

/**
 * Builds the ARM epilogue
 */
static void arm_abi_epilogue(void *self, ir_node *bl, ir_node **mem, pmap *reg_map) {
	arm_abi_env_t *env = self;
	ir_node *curr_sp = be_abi_reg_map_get(reg_map, env->arch_env->sp);
	ir_node *curr_bp = be_abi_reg_map_get(reg_map, env->arch_env->bp);
	ir_node *curr_pc = be_abi_reg_map_get(reg_map, &arm_gp_regs[REG_PC]);
	ir_node	*curr_lr = be_abi_reg_map_get(reg_map, &arm_gp_regs[REG_LR]);

	// TODO: Activate Omit fp in epilogue
	if (env->flags.try_omit_fp) {
		curr_sp = be_new_IncSP(env->arch_env->sp, env->irg, bl, curr_sp, BE_STACK_FRAME_SIZE_SHRINK, 0);

		curr_lr = be_new_CopyKeep_single(&arm_reg_classes[CLASS_arm_gp], env->irg, bl, curr_lr, curr_sp, get_irn_mode(curr_lr));
		be_node_set_reg_class(curr_lr, 1, &arm_reg_classes[CLASS_arm_gp]);
		arch_set_irn_register(curr_lr, &arm_gp_regs[REG_LR]);
		be_set_constr_single_reg(curr_lr, BE_OUT_POS(0), &arm_gp_regs[REG_LR] );

		curr_pc = be_new_Copy(&arm_reg_classes[CLASS_arm_gp], env->irg, bl, curr_lr );
		arch_set_irn_register(curr_pc, &arm_gp_regs[REG_PC]);
		be_set_constr_single_reg(curr_pc, BE_OUT_POS(0), &arm_gp_regs[REG_PC] );
		be_node_set_flags(curr_pc, BE_OUT_POS(0), arch_irn_flags_ignore);
	} else {
		ir_node *sub12_node;
		ir_node *load_node;
		sub12_node = new_rd_arm_Sub_i(NULL, env->irg, bl, curr_bp, mode_Iu, 12);
		// FIXME
		//set_arm_req_out_all(sub12_node, sub12_req);
		arch_set_irn_register(sub12_node, env->arch_env->sp);
		load_node = new_rd_arm_LoadStackM3( NULL, env->irg, bl, sub12_node, *mem );
		// FIXME
		//set_arm_req_out(load_node, &arm_default_req_arm_gp_r11, 0);
		//set_arm_req_out(load_node, &arm_default_req_arm_gp_sp, 1);
		//set_arm_req_out(load_node, &arm_default_req_arm_gp_pc, 2);
		curr_bp = new_r_Proj(env->irg, bl, load_node, env->arch_env->bp->reg_class->mode, pn_arm_LoadStackM3_res0);
		curr_sp = new_r_Proj(env->irg, bl, load_node, env->arch_env->sp->reg_class->mode, pn_arm_LoadStackM3_res1);
		curr_pc = new_r_Proj(env->irg, bl, load_node, mode_Iu, pn_arm_LoadStackM3_res2);
		*mem    = new_r_Proj(env->irg, bl, load_node, mode_M, pn_arm_LoadStackM3_M);
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
	arm_abi_dont_save_regs,
	arm_abi_prologue,
	arm_abi_epilogue,
};


/**
 * Get the ABI restrictions for procedure calls.
 * @param self        The this pointer.
 * @param method_type The type of the method (procedure) in question.
 * @param abi         The abi object to be modified
 */
void arm_get_call_abi(const void *self, ir_type *method_type, be_abi_call_t *abi) {
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
			be_abi_call_param_reg(abi, i, arm_get_RegParam_reg(i));
		} else {
			tp   = get_method_param_type(method_type, i);
			mode = get_type_mode(tp);
			be_abi_call_param_stack(abi, i, mode, 4, 0, 0);
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

		be_abi_call_res_reg(abi, 0, &arm_gp_regs[REG_R0]);
		be_abi_call_res_reg(abi, 1, &arm_gp_regs[REG_R1]);
	} else if (n == 1) {
		const arch_register_t *reg;

		tp   = get_method_res_type(method_type, 0);
		assert(is_atomic_type(tp));
		mode = get_type_mode(tp);

		reg = mode_is_float(mode) ? &arm_fpa_regs[REG_F0] : &arm_gp_regs[REG_R0];
		be_abi_call_res_reg(abi, 0, reg);
	}
}

int arm_to_appear_in_schedule(void *block_env, const ir_node *irn) {
	(void) block_env;
	if(!is_arm_irn(irn))
		return -1;

	return 1;
}

/**
 * Initializes the code generator interface.
 */
static const arch_code_generator_if_t *arm_get_code_generator_if(void *self) {
	(void) self;
	return &arm_code_gen_if;
}

list_sched_selector_t arm_sched_selector;

/**
 * Returns the reg_pressure scheduler with to_appear_in_schedule() over\loaded
 */
static const list_sched_selector_t *arm_get_list_sched_selector(const void *self, list_sched_selector_t *selector) {
	(void) self;
	memcpy(&arm_sched_selector, selector, sizeof(arm_sched_selector));
	/* arm_sched_selector.exectime              = arm_sched_exectime; */
	arm_sched_selector.to_appear_in_schedule = arm_to_appear_in_schedule;
	return &arm_sched_selector;

}

static const ilp_sched_selector_t *arm_get_ilp_sched_selector(const void *self) {
	(void) self;
	return NULL;
}

/**
 * Returns the necessary byte alignment for storing a register of given class.
 */
static int arm_get_reg_class_alignment(const void *self, const arch_register_class_t *cls) {
	(void) self;
	(void) cls;
	/* ARM is a 32 bit CPU, no need for other alignment */
	return 4;
}

static const be_execution_unit_t ***arm_get_allowed_execution_units(const void *self, const ir_node *irn) {
	(void) self;
	(void) irn;
	/* TODO */
	panic("Unimplemented arm_get_allowed_execution_units()");
}

static const be_machine_t *arm_get_machine(const void *self) {
	(void) self;
	/* TODO */
	panic("Unimplemented arm_get_machine()");
}

/**
 * Return irp irgs in the desired order.
 */
static ir_graph **arm_get_irg_list(const void *self, ir_graph ***irg_list) {
	(void) self;
	(void) irg_list;
	return NULL;
}

/**
 * Allows or disallows the creation of Psi nodes for the given Phi nodes.
 * @return 1 if allowed, 0 otherwise
 */
static int arm_is_psi_allowed(ir_node *sel, ir_node *phi_list, int i, int j) {
	ir_node *cmp, *cmp_a, *phi;
	ir_mode *mode;


	/* currently Psi support is not implemented */
	return 0;

/* we don't want long long Psi */
#define IS_BAD_PSI_MODE(mode) (!mode_is_float(mode) && get_mode_size_bits(mode) > 32)

	if (get_irn_mode(sel) != mode_b)
		return 0;

	cmp   = get_Proj_pred(sel);
	cmp_a = get_Cmp_left(cmp);
	mode  = get_irn_mode(cmp_a);

	if (IS_BAD_PSI_MODE(mode))
		return 0;

	/* check the Phi nodes */
	for (phi = phi_list; phi; phi = get_irn_link(phi)) {
		ir_node *pred_i = get_irn_n(phi, i);
		ir_node *pred_j = get_irn_n(phi, j);
		ir_mode *mode_i = get_irn_mode(pred_i);
		ir_mode *mode_j = get_irn_mode(pred_j);

		if (IS_BAD_PSI_MODE(mode_i) || IS_BAD_PSI_MODE(mode_j))
			return 0;
	}

#undef IS_BAD_PSI_MODE

	return 1;
}

static asm_constraint_flags_t arm_parse_asm_constraint(const void *self, const char **c)
{
	/* asm not supported */
	(void) self;
	(void) c;
	return ASM_CONSTRAINT_FLAG_INVALID;
}

static int arm_is_valid_clobber(const void *self, const char *clobber)
{
	(void) self;
	(void) clobber;
	return 0;
}

/**
 * Returns the libFirm configuration parameter for this backend.
 */
static const backend_params *arm_get_libfirm_params(void) {
	static const ir_settings_if_conv_t ifconv = {
		4,                    /* maxdepth, doesn't matter for Psi-conversion */
		arm_is_psi_allowed   /* allows or disallows Psi creation for given selector */
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
		0,     /* no immediate floating point mode. */
		NULL,  /* no additional opcodes */
		NULL,  /* will be set later */
		NULL,  /* but yet no creator function */
		NULL,  /* context for create_intrinsic_fkt */
		NULL,  /* will be set below */
		NULL   /* no immediate fp mode */
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

void be_init_arch_arm(void)
{
	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *arm_grp = lc_opt_get_grp(be_grp, "arm");

	lc_opt_add_table(arm_grp, arm_options);

	be_register_isa_if("arm", &arm_isa_if);

	arm_init_transform();
	arm_init_emitter();
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_arch_arm);
