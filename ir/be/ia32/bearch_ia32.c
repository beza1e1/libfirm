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
 * @brief       This is the main ia32 firm backend driver.
 * @author      Christian Wuerdig
 * @version     $Id$
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "lc_opts.h"
#include "lc_opts_enum.h"

#include <math.h>

#include "pseudo_irg.h"
#include "irarch.h"
#include "irgwalk.h"
#include "irprog.h"
#include "irprintf.h"
#include "iredges_t.h"
#include "ircons.h"
#include "irgmod.h"
#include "irgopt.h"
#include "irbitset.h"
#include "irgopt.h"
#include "irdump_grgen.h"
#include "pdeq.h"
#include "pset.h"
#include "debug.h"
#include "error.h"
#include "xmalloc.h"
#include "irtools.h"
#include "iroptimize.h"
#include "instrument.h"

#include "../beabi.h"
#include "../beirg_t.h"
#include "../benode_t.h"
#include "../belower.h"
#include "../besched_t.h"
#include "be.h"
#include "../be_t.h"
#include "../beirgmod.h"
#include "../be_dbgout.h"
#include "../beblocksched.h"
#include "../bemachine.h"
#include "../beilpsched.h"
#include "../bespillslots.h"
#include "../bemodule.h"
#include "../begnuas.h"
#include "../bestate.h"
#include "../beflags.h"

#include "bearch_ia32_t.h"

#include "ia32_new_nodes.h"
#include "gen_ia32_regalloc_if.h"
#include "gen_ia32_machine.h"
#include "ia32_transform.h"
#include "ia32_emitter.h"
#include "ia32_map_regs.h"
#include "ia32_optimize.h"
#include "ia32_x87.h"
#include "ia32_dbg_stat.h"
#include "ia32_finish.h"
#include "ia32_util.h"
#include "ia32_fpu.h"
#include "ia32_architecture.h"

#ifdef FIRM_GRGEN_BE
#include "ia32_pbqp_transform.h"
#endif

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

/* TODO: ugly */
static set *cur_reg_set = NULL;

ir_mode         *mode_fpcw       = NULL;
ia32_code_gen_t *ia32_current_cg = NULL;

/**
 * The environment for the intrinsic mapping.
 */
static ia32_intrinsic_env_t intrinsic_env = {
	NULL,    /* the isa */
	NULL,    /* the irg, these entities belong to */
	NULL,    /* entity for first div operand (move into FPU) */
	NULL,    /* entity for second div operand (move into FPU) */
	NULL,    /* entity for converts ll -> d */
	NULL,    /* entity for converts d -> ll */
	NULL,    /* entity for __divdi3 library call */
	NULL,    /* entity for __moddi3 library call */
	NULL,    /* entity for __udivdi3 library call */
	NULL,    /* entity for __umoddi3 library call */
	NULL,    /* bias value for conversion from float to unsigned 64 */
};


typedef ir_node *(*create_const_node_func) (dbg_info *dbg, ir_graph *irg, ir_node *block);

static INLINE ir_node *create_const(ia32_code_gen_t *cg, ir_node **place,
                                    create_const_node_func func,
                                    const arch_register_t* reg)
{
	ir_node *block, *res;

	if(*place != NULL)
		return *place;

	block = get_irg_start_block(cg->irg);
	res = func(NULL, cg->irg, block);
	arch_set_irn_register(cg->arch_env, res, reg);
	*place = res;

	add_irn_dep(get_irg_end(cg->irg), res);
	/* add_irn_dep(get_irg_start(cg->irg), res); */

	return res;
}

/* Creates the unique per irg GP NoReg node. */
ir_node *ia32_new_NoReg_gp(ia32_code_gen_t *cg) {
	return create_const(cg, &cg->noreg_gp, new_rd_ia32_NoReg_GP,
	                    &ia32_gp_regs[REG_GP_NOREG]);
}

ir_node *ia32_new_NoReg_vfp(ia32_code_gen_t *cg) {
	return create_const(cg, &cg->noreg_vfp, new_rd_ia32_NoReg_VFP,
	                    &ia32_vfp_regs[REG_VFP_NOREG]);
}

ir_node *ia32_new_NoReg_xmm(ia32_code_gen_t *cg) {
	return create_const(cg, &cg->noreg_xmm, new_rd_ia32_NoReg_XMM,
	                    &ia32_xmm_regs[REG_XMM_NOREG]);
}

ir_node *ia32_new_Unknown_gp(ia32_code_gen_t *cg) {
	return create_const(cg, &cg->unknown_gp, new_rd_ia32_Unknown_GP,
	                    &ia32_gp_regs[REG_GP_UKNWN]);
}

ir_node *ia32_new_Unknown_vfp(ia32_code_gen_t *cg) {
	return create_const(cg, &cg->unknown_vfp, new_rd_ia32_Unknown_VFP,
	                    &ia32_vfp_regs[REG_VFP_UKNWN]);
}

ir_node *ia32_new_Unknown_xmm(ia32_code_gen_t *cg) {
	return create_const(cg, &cg->unknown_xmm, new_rd_ia32_Unknown_XMM,
	                    &ia32_xmm_regs[REG_XMM_UKNWN]);
}

ir_node *ia32_new_Fpu_truncate(ia32_code_gen_t *cg) {
	return create_const(cg, &cg->fpu_trunc_mode, new_rd_ia32_ChangeCW,
                        &ia32_fp_cw_regs[REG_FPCW]);
}


/**
 * Returns the admissible noreg register node for input register pos of node irn.
 */
ir_node *ia32_get_admissible_noreg(ia32_code_gen_t *cg, ir_node *irn, int pos) {
	const arch_register_req_t *req;

	req = arch_get_register_req(cg->arch_env, irn, pos);
	assert(req != NULL && "Missing register requirements");
	if (req->cls == &ia32_reg_classes[CLASS_ia32_gp])
		return ia32_new_NoReg_gp(cg);

	if (ia32_cg_config.use_sse2) {
		return ia32_new_NoReg_xmm(cg);
	} else {
		return ia32_new_NoReg_vfp(cg);
	}
}

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
 * Return register requirements for an ia32 node.
 * If the node returns a tuple (mode_T) then the proj's
 * will be asked for this information.
 */
static const arch_register_req_t *ia32_get_irn_reg_req(const void *self,
                                                       const ir_node *node,
													   int pos)
{
	ir_mode *mode = get_irn_mode(node);
	long    node_pos;

	(void)self;
	if (mode == mode_X || is_Block(node)) {
		return arch_no_register_req;
	}

	if (mode == mode_T && pos < 0) {
		return arch_no_register_req;
	}

	node_pos = pos == -1 ? 0 : pos;
	if (is_Proj(node)) {
		if (mode == mode_M || pos >= 0) {
			return arch_no_register_req;
		}

		node_pos = (pos == -1) ? get_Proj_proj(node) : pos;
		node     = skip_Proj_const(node);
	}

	if (is_ia32_irn(node)) {
		const arch_register_req_t *req;
		if (pos >= 0)
			req = get_ia32_in_req(node, pos);
		else
			req = get_ia32_out_req(node, node_pos);

		assert(req != NULL);

		return req;
	}

	/* unknowns should be transformed already */
	assert(!is_Unknown(node));
	return arch_no_register_req;
}

static void ia32_set_irn_reg(const void *self, ir_node *irn,
                             const arch_register_t *reg)
{
	int    pos = 0;
	(void) self;

	if (get_irn_mode(irn) == mode_X) {
		return;
	}

	if (is_Proj(irn)) {
		pos = get_Proj_proj(irn);
		irn = skip_Proj(irn);
	}

	if (is_ia32_irn(irn)) {
		const arch_register_t **slots;

		slots      = get_ia32_slots(irn);
		slots[pos] = reg;
	} else {
		ia32_set_firm_reg(irn, reg, cur_reg_set);
	}
}

static const arch_register_t *ia32_get_irn_reg(const void *self,
                                               const ir_node *irn)
{
	int pos = 0;
	const arch_register_t *reg = NULL;
	(void) self;

	if (is_Proj(irn)) {

		if (get_irn_mode(irn) == mode_X) {
			return NULL;
		}

		pos = get_Proj_proj(irn);
		irn = skip_Proj_const(irn);
	}

	if (is_ia32_irn(irn)) {
		const arch_register_t **slots;
		slots = get_ia32_slots(irn);
		assert(pos < get_ia32_n_res(irn));
		reg   = slots[pos];
	} else {
		reg = ia32_get_firm_reg(irn, cur_reg_set);
	}

	return reg;
}

static arch_irn_class_t ia32_classify(const void *self, const ir_node *irn) {
	arch_irn_class_t classification = arch_irn_class_normal;
	(void) self;

	irn = skip_Proj_const(irn);

	if (is_cfop(irn))
		classification |= arch_irn_class_branch;

	if (! is_ia32_irn(irn))
		return classification & ~arch_irn_class_normal;

	if (is_ia32_Ld(irn))
		classification |= arch_irn_class_load;

	if (is_ia32_St(irn))
		classification |= arch_irn_class_store;

	if (is_ia32_need_stackent(irn))
		classification |= arch_irn_class_reload;

	return classification;
}

static arch_irn_flags_t ia32_get_flags(const void *self, const ir_node *irn) {
	arch_irn_flags_t flags = arch_irn_flags_none;
	(void) self;

	if (is_Unknown(irn))
		return arch_irn_flags_ignore;

	if(is_Proj(irn) && mode_is_datab(get_irn_mode(irn))) {
		ir_node *pred = get_Proj_pred(irn);

		if(is_ia32_irn(pred)) {
			flags = get_ia32_out_flags(pred, get_Proj_proj(irn));
		}

		irn = pred;
	}

	if (is_ia32_irn(irn)) {
		flags |= get_ia32_flags(irn);
	}

	return flags;
}

/**
 * The IA32 ABI callback object.
 */
typedef struct {
	be_abi_call_flags_bits_t flags;  /**< The call flags. */
	const arch_isa_t *isa;           /**< The ISA handle. */
	const arch_env_t *aenv;          /**< The architecture environment. */
	ir_graph *irg;                   /**< The associated graph. */
} ia32_abi_env_t;

static ir_entity *ia32_get_frame_entity(const void *self, const ir_node *irn) {
	(void) self;
	return is_ia32_irn(irn) ? get_ia32_frame_ent(irn) : NULL;
}

static void ia32_set_frame_entity(const void *self, ir_node *irn, ir_entity *ent) {
	(void) self;
	set_ia32_frame_ent(irn, ent);
}

static void ia32_set_frame_offset(const void *self, ir_node *irn, int bias)
{
	const ia32_irn_ops_t *ops = self;

	if (get_ia32_frame_ent(irn) == NULL)
		return;

	if (is_ia32_Pop(irn) || is_ia32_PopMem(irn)) {
		int omit_fp = be_abi_omit_fp(ops->cg->birg->abi);
		if (omit_fp) {
			/* Pop nodes modify the stack pointer before calculating the
			 * destination address, so fix this here
			 */
			bias -= 4;
		}
	}
	add_ia32_am_offs_int(irn, bias);
}

static int ia32_get_sp_bias(const void *self, const ir_node *node)
{
	(void) self;

	if (is_ia32_Push(node))
		return 4;

	if (is_ia32_Pop(node) || is_ia32_PopMem(node))
		return -4;

	return 0;
}

/**
 * Put all registers which are saved by the prologue/epilogue in a set.
 *
 * @param self  The callback object.
 * @param s     The result set.
 */
static void ia32_abi_dont_save_regs(void *self, pset *s)
{
	ia32_abi_env_t *env = self;
	if(env->flags.try_omit_fp)
		pset_insert_ptr(s, env->isa->bp);
}

/**
 * Generate the routine prologue.
 *
 * @param self    The callback object.
 * @param mem     A pointer to the mem node. Update this if you define new memory.
 * @param reg_map A map mapping all callee_save/ignore/parameter registers to their defining nodes.
 *
 * @return        The register which shall be used as a stack frame base.
 *
 * All nodes which define registers in @p reg_map must keep @p reg_map current.
 */
static const arch_register_t *ia32_abi_prologue(void *self, ir_node **mem, pmap *reg_map)
{
	ia32_abi_env_t *env = self;
	const ia32_isa_t *isa     = (ia32_isa_t *)env->isa;
	ia32_code_gen_t *cg = isa->cg;

	if (! env->flags.try_omit_fp) {
		ir_node *bl      = get_irg_start_block(env->irg);
		ir_node *curr_sp = be_abi_reg_map_get(reg_map, env->isa->sp);
		ir_node *curr_bp = be_abi_reg_map_get(reg_map, env->isa->bp);
		ir_node *noreg = ia32_new_NoReg_gp(cg);
		ir_node *push;

		/* ALL nodes representing bp must be set to ignore. */
		be_node_set_flags(get_Proj_pred(curr_bp), BE_OUT_POS(get_Proj_proj(curr_bp)), arch_irn_flags_ignore);

		/* push ebp */
		push    = new_rd_ia32_Push(NULL, env->irg, bl, noreg, noreg, *mem, curr_bp, curr_sp);
		curr_sp = new_r_Proj(env->irg, bl, push, get_irn_mode(curr_sp), pn_ia32_Push_stack);
		*mem    = new_r_Proj(env->irg, bl, push, mode_M, pn_ia32_Push_M);

		/* the push must have SP out register */
		arch_set_irn_register(env->aenv, curr_sp, env->isa->sp);
		set_ia32_flags(push, arch_irn_flags_ignore);

		/* move esp to ebp */
		curr_bp  = be_new_Copy(env->isa->bp->reg_class, env->irg, bl, curr_sp);
		be_set_constr_single_reg(curr_bp, BE_OUT_POS(0), env->isa->bp);
		arch_set_irn_register(env->aenv, curr_bp, env->isa->bp);
		be_node_set_flags(curr_bp, BE_OUT_POS(0), arch_irn_flags_ignore);

		/* beware: the copy must be done before any other sp use */
		curr_sp = be_new_CopyKeep_single(env->isa->sp->reg_class, env->irg, bl, curr_sp, curr_bp, get_irn_mode(curr_sp));
		be_set_constr_single_reg(curr_sp, BE_OUT_POS(0), env->isa->sp);
		arch_set_irn_register(env->aenv, curr_sp, env->isa->sp);
		be_node_set_flags(curr_sp, BE_OUT_POS(0), arch_irn_flags_ignore);

		be_abi_reg_map_set(reg_map, env->isa->sp, curr_sp);
		be_abi_reg_map_set(reg_map, env->isa->bp, curr_bp);

		return env->isa->bp;
	}

	return env->isa->sp;
}

/**
 * Generate the routine epilogue.
 * @param self    The callback object.
 * @param bl      The block for the epilog
 * @param mem     A pointer to the mem node. Update this if you define new memory.
 * @param reg_map A map mapping all callee_save/ignore/parameter registers to their defining nodes.
 * @return        The register which shall be used as a stack frame base.
 *
 * All nodes which define registers in @p reg_map must keep @p reg_map current.
 */
static void ia32_abi_epilogue(void *self, ir_node *bl, ir_node **mem, pmap *reg_map)
{
	ia32_abi_env_t *env     = self;
	ir_node        *curr_sp = be_abi_reg_map_get(reg_map, env->isa->sp);
	ir_node        *curr_bp = be_abi_reg_map_get(reg_map, env->isa->bp);

	if (env->flags.try_omit_fp) {
		/* simply remove the stack frame here */
		curr_sp = be_new_IncSP(env->isa->sp, env->irg, bl, curr_sp, BE_STACK_FRAME_SIZE_SHRINK, 0);
		add_irn_dep(curr_sp, *mem);
	} else {
		ir_mode         *mode_bp = env->isa->bp->reg_class->mode;
		ir_graph        *irg     = current_ir_graph;

		if (ia32_cg_config.use_leave) {
			ir_node *leave;

			/* leave */
			leave   = new_rd_ia32_Leave(NULL, irg, bl, curr_sp, curr_bp);
			set_ia32_flags(leave, arch_irn_flags_ignore);
			curr_bp = new_r_Proj(irg, bl, leave, mode_bp, pn_ia32_Leave_frame);
			curr_sp = new_r_Proj(irg, bl, leave, get_irn_mode(curr_sp), pn_ia32_Leave_stack);
		} else {
			ir_node *pop;

			/* the old SP is not needed anymore (kill the proj) */
			assert(is_Proj(curr_sp));
			be_kill_node(curr_sp);

			/* copy ebp to esp */
			curr_sp = be_new_Copy(&ia32_reg_classes[CLASS_ia32_gp], irg, bl, curr_bp);
			arch_set_irn_register(env->aenv, curr_sp, env->isa->sp);
			be_node_set_flags(curr_sp, BE_OUT_POS(0), arch_irn_flags_ignore);

			/* pop ebp */
			pop     = new_rd_ia32_Pop(NULL, env->irg, bl, *mem, curr_sp);
			set_ia32_flags(pop, arch_irn_flags_ignore);
			curr_bp = new_r_Proj(irg, bl, pop, mode_bp, pn_ia32_Pop_res);
			curr_sp = new_r_Proj(irg, bl, pop, get_irn_mode(curr_sp), pn_ia32_Pop_stack);

			*mem = new_r_Proj(irg, bl, pop, mode_M, pn_ia32_Pop_M);
		}
		arch_set_irn_register(env->aenv, curr_sp, env->isa->sp);
		arch_set_irn_register(env->aenv, curr_bp, env->isa->bp);
	}

	be_abi_reg_map_set(reg_map, env->isa->sp, curr_sp);
	be_abi_reg_map_set(reg_map, env->isa->bp, curr_bp);
}

/**
 * Initialize the callback object.
 * @param call The call object.
 * @param aenv The architecture environment.
 * @param irg  The graph with the method.
 * @return     Some pointer. This pointer is passed to all other callback functions as self object.
 */
static void *ia32_abi_init(const be_abi_call_t *call, const arch_env_t *aenv, ir_graph *irg)
{
	ia32_abi_env_t *env    = xmalloc(sizeof(env[0]));
	be_abi_call_flags_t fl = be_abi_call_get_flags(call);
	env->flags = fl.bits;
	env->irg   = irg;
	env->aenv  = aenv;
	env->isa   = aenv->isa;
	return env;
}

/**
 * Destroy the callback object.
 * @param self The callback object.
 */
static void ia32_abi_done(void *self) {
	free(self);
}

/**
 * Produces the type which sits between the stack args and the locals on the stack.
 * it will contain the return address and space to store the old base pointer.
 * @return The Firm type modeling the ABI between type.
 */
static ir_type *ia32_abi_get_between_type(void *self)
{
#define IDENT(s) new_id_from_chars(s, sizeof(s)-1)
	static ir_type *omit_fp_between_type = NULL;
	static ir_type *between_type         = NULL;

	ia32_abi_env_t *env = self;

	if (! between_type) {
		ir_entity *old_bp_ent;
		ir_entity *ret_addr_ent;
		ir_entity *omit_fp_ret_addr_ent;

		ir_type *old_bp_type   = new_type_primitive(IDENT("bp"), mode_Iu);
		ir_type *ret_addr_type = new_type_primitive(IDENT("return_addr"), mode_Iu);

		between_type           = new_type_struct(IDENT("ia32_between_type"));
		old_bp_ent             = new_entity(between_type, IDENT("old_bp"), old_bp_type);
		ret_addr_ent           = new_entity(between_type, IDENT("ret_addr"), ret_addr_type);

		set_entity_offset(old_bp_ent, 0);
		set_entity_offset(ret_addr_ent, get_type_size_bytes(old_bp_type));
		set_type_size_bytes(between_type, get_type_size_bytes(old_bp_type) + get_type_size_bytes(ret_addr_type));
		set_type_state(between_type, layout_fixed);

		omit_fp_between_type = new_type_struct(IDENT("ia32_between_type_omit_fp"));
		omit_fp_ret_addr_ent = new_entity(omit_fp_between_type, IDENT("ret_addr"), ret_addr_type);

		set_entity_offset(omit_fp_ret_addr_ent, 0);
		set_type_size_bytes(omit_fp_between_type, get_type_size_bytes(ret_addr_type));
		set_type_state(omit_fp_between_type, layout_fixed);
	}

	return env->flags.try_omit_fp ? omit_fp_between_type : between_type;
#undef IDENT
}

/**
 * Get the estimated cycle count for @p irn.
 *
 * @param self The this pointer.
 * @param irn  The node.
 *
 * @return     The estimated cycle count for this operation
 */
static int ia32_get_op_estimated_cost(const void *self, const ir_node *irn)
{
	int            cost;
	ia32_op_type_t op_tp;
	(void) self;

	if (is_Proj(irn))
		return 0;
	if (!is_ia32_irn(irn))
		return 0;

	assert(is_ia32_irn(irn));

	cost  = get_ia32_latency(irn);
	op_tp = get_ia32_op_type(irn);

	if (is_ia32_CopyB(irn)) {
		cost = 250;
	}
	else if (is_ia32_CopyB_i(irn)) {
		int size = get_ia32_copyb_size(irn);
		cost     = 20 + (int)ceil((4/3) * size);
	}
	/* in case of address mode operations add additional cycles */
	else if (op_tp == ia32_AddrModeD || op_tp == ia32_AddrModeS) {
		/*
			In case of stack access and access to fixed addresses add 5 cycles
			(we assume they are in cache), other memory operations cost 20
			cycles.
		*/
		if(is_ia32_use_frame(irn) ||
				(is_ia32_NoReg_GP(get_irn_n(irn, 0)) &&
		         is_ia32_NoReg_GP(get_irn_n(irn, 1)))) {
			cost += 5;
		} else {
			cost += 20;
		}
	}

	return cost;
}

/**
 * Returns the inverse operation if @p irn, recalculating the argument at position @p i.
 *
 * @param irn       The original operation
 * @param i         Index of the argument we want the inverse operation to yield
 * @param inverse   struct to be filled with the resulting inverse op
 * @param obstack   The obstack to use for allocation of the returned nodes array
 * @return          The inverse operation or NULL if operation invertible
 */
static arch_inverse_t *ia32_get_inverse(const void *self, const ir_node *irn, int i, arch_inverse_t *inverse, struct obstack *obst) {
	ir_graph *irg;
	ir_mode  *mode;
	ir_mode  *irn_mode;
	ir_node  *block, *noreg, *nomem;
	dbg_info *dbg;
	(void) self;

	/* we cannot invert non-ia32 irns */
	if (! is_ia32_irn(irn))
		return NULL;

	/* operand must always be a real operand (not base, index or mem) */
	if (i != n_ia32_binary_left && i != n_ia32_binary_right)
		return NULL;

	/* we don't invert address mode operations */
	if (get_ia32_op_type(irn) != ia32_Normal)
		return NULL;

	/* TODO: adjust for new immediates... */
	ir_fprintf(stderr, "TODO: fix get_inverse for new immediates (%+F)\n",
	           irn);
	return NULL;

	irg      = get_irn_irg(irn);
	block    = get_nodes_block(irn);
	mode     = get_irn_mode(irn);
	irn_mode = get_irn_mode(irn);
	noreg    = get_irn_n(irn, 0);
	nomem    = new_r_NoMem(irg);
	dbg      = get_irn_dbg_info(irn);

	/* initialize structure */
	inverse->nodes = obstack_alloc(obst, 2 * sizeof(inverse->nodes[0]));
	inverse->costs = 0;
	inverse->n     = 1;

	switch (get_ia32_irn_opcode(irn)) {
		case iro_ia32_Add:
#if 0
			if (get_ia32_immop_type(irn) == ia32_ImmConst) {
				/* we have an add with a const here */
				/* invers == add with negated const */
				inverse->nodes[0] = new_rd_ia32_Add(dbg, irg, block, noreg, noreg, nomem, get_irn_n(irn, i), noreg);
				inverse->costs   += 1;
				copy_ia32_Immop_attr(inverse->nodes[0], (ir_node *)irn);
				set_ia32_Immop_tarval(inverse->nodes[0], tarval_neg(get_ia32_Immop_tarval(irn)));
				set_ia32_commutative(inverse->nodes[0]);
			}
			else if (get_ia32_immop_type(irn) == ia32_ImmSymConst) {
				/* we have an add with a symconst here */
				/* invers == sub with const */
				inverse->nodes[0] = new_rd_ia32_Sub(dbg, irg, block, noreg, noreg, nomem, get_irn_n(irn, i), noreg);
				inverse->costs   += 2;
				copy_ia32_Immop_attr(inverse->nodes[0], (ir_node *)irn);
			}
			else {
				/* normal add: inverse == sub */
				inverse->nodes[0] = new_rd_ia32_Sub(dbg, irg, block, noreg, noreg, nomem, (ir_node*) irn, get_irn_n(irn, i ^ 1));
				inverse->costs   += 2;
			}
#endif
			break;
		case iro_ia32_Sub:
#if 0
			if (get_ia32_immop_type(irn) != ia32_ImmNone) {
				/* we have a sub with a const/symconst here */
				/* invers == add with this const */
				inverse->nodes[0] = new_rd_ia32_Add(dbg, irg, block, noreg, noreg, nomem, get_irn_n(irn, i), noreg);
				inverse->costs   += (get_ia32_immop_type(irn) == ia32_ImmSymConst) ? 5 : 1;
				copy_ia32_Immop_attr(inverse->nodes[0], (ir_node *)irn);
			}
			else {
				/* normal sub */
				if (i == n_ia32_binary_left) {
					inverse->nodes[0] = new_rd_ia32_Add(dbg, irg, block, noreg, noreg, nomem, (ir_node*) irn, get_irn_n(irn, 3));
				}
				else {
					inverse->nodes[0] = new_rd_ia32_Sub(dbg, irg, block, noreg, noreg, nomem, get_irn_n(irn, n_ia32_binary_left), (ir_node*) irn);
				}
				inverse->costs += 1;
			}
#endif
			break;
		case iro_ia32_Xor:
#if 0
			if (get_ia32_immop_type(irn) != ia32_ImmNone) {
				/* xor with const: inverse = xor */
				inverse->nodes[0] = new_rd_ia32_Xor(dbg, irg, block, noreg, noreg, nomem, get_irn_n(irn, i), noreg);
				inverse->costs   += (get_ia32_immop_type(irn) == ia32_ImmSymConst) ? 5 : 1;
				copy_ia32_Immop_attr(inverse->nodes[0], (ir_node *)irn);
			}
			else {
				/* normal xor */
				inverse->nodes[0] = new_rd_ia32_Xor(dbg, irg, block, noreg, noreg, nomem, (ir_node *) irn, get_irn_n(irn, i));
				inverse->costs   += 1;
			}
#endif
			break;
		case iro_ia32_Not: {
			inverse->nodes[0] = new_rd_ia32_Not(dbg, irg, block, (ir_node*) irn);
			inverse->costs   += 1;
			break;
		}
		case iro_ia32_Neg: {
			inverse->nodes[0] = new_rd_ia32_Neg(dbg, irg, block, (ir_node*) irn);
			inverse->costs   += 1;
			break;
		}
		default:
			/* inverse operation not supported */
			return NULL;
	}

	return inverse;
}

static ir_mode *get_spill_mode_mode(const ir_mode *mode)
{
	if(mode_is_float(mode))
		return mode_D;

	return mode_Iu;
}

/**
 * Get the mode that should be used for spilling value node
 */
static ir_mode *get_spill_mode(const ir_node *node)
{
	ir_mode *mode = get_irn_mode(node);
	return get_spill_mode_mode(mode);
}

/**
 * Checks whether an addressmode reload for a node with mode mode is compatible
 * with a spillslot of mode spill_mode
 */
static int ia32_is_spillmode_compatible(const ir_mode *mode, const ir_mode *spillmode)
{
	if(mode_is_float(mode)) {
		return mode == spillmode;
	} else {
		return 1;
	}
}

/**
 * Check if irn can load its operand at position i from memory (source addressmode).
 * @param self   Pointer to irn ops itself
 * @param irn    The irn to be checked
 * @param i      The operands position
 * @return Non-Zero if operand can be loaded
 */
static int ia32_possible_memory_operand(const void *self, const ir_node *irn, unsigned int i) {
	ir_node *op = get_irn_n(irn, i);
	const ir_mode *mode = get_irn_mode(op);
	const ir_mode *spillmode = get_spill_mode(op);
	(void) self;

	if (
		(i != n_ia32_binary_left && i != n_ia32_binary_right) || /* a "real" operand position must be requested */
		! is_ia32_irn(irn)                                    ||  /* must be an ia32 irn */
		get_ia32_am_arity(irn) != ia32_am_binary              ||  /* must be a binary operation TODO is this necessary? */
		get_ia32_op_type(irn) != ia32_Normal                  ||  /* must not already be a addressmode irn */
		! (get_ia32_am_support(irn) & ia32_am_Source)         ||  /* must be capable of source addressmode */
		! ia32_is_spillmode_compatible(mode, spillmode)       ||
		is_ia32_use_frame(irn))                                  /* must not already use frame */
		return 0;

	if (i == n_ia32_binary_left) {
		const arch_register_req_t *req;
		if(!is_ia32_commutative(irn))
			return 0;
		/* we can't swap left/right for limited registers
		 * (As this (currently) breaks constraint handling copies)
		 */
		req = get_ia32_in_req(irn, n_ia32_binary_left);
		if (req->type & arch_register_req_type_limited) {
			return 0;
		}
	}

	return 1;
}

static void ia32_perform_memory_operand(const void *self, ir_node *irn,
                                        ir_node *spill, unsigned int i)
{
	const ia32_irn_ops_t *ops = self;

	assert(ia32_possible_memory_operand(self, irn, i) && "Cannot perform memory operand change");

	if (i == n_ia32_binary_left) {
		ia32_swap_left_right(irn);
	}

	set_ia32_op_type(irn, ia32_AddrModeS);
	set_ia32_ls_mode(irn, get_irn_mode(get_irn_n(irn, i)));
	set_ia32_use_frame(irn);
	set_ia32_need_stackent(irn);

	set_irn_n(irn, n_ia32_base, get_irg_frame(get_irn_irg(irn)));
	set_irn_n(irn, n_ia32_binary_right, ia32_get_admissible_noreg(ops->cg, irn, n_ia32_binary_right));
	set_irn_n(irn, n_ia32_mem, spill);

	/* immediates are only allowed on the right side */
	if (i == n_ia32_binary_left && is_ia32_Immediate(get_irn_n(irn, n_ia32_binary_left))) {
		ia32_swap_left_right(irn);
	}
}

static const be_abi_callbacks_t ia32_abi_callbacks = {
	ia32_abi_init,
	ia32_abi_done,
	ia32_abi_get_between_type,
	ia32_abi_dont_save_regs,
	ia32_abi_prologue,
	ia32_abi_epilogue
};

/* fill register allocator interface */

static const arch_irn_ops_if_t ia32_irn_ops_if = {
	ia32_get_irn_reg_req,
	ia32_set_irn_reg,
	ia32_get_irn_reg,
	ia32_classify,
	ia32_get_flags,
	ia32_get_frame_entity,
	ia32_set_frame_entity,
	ia32_set_frame_offset,
	ia32_get_sp_bias,
	ia32_get_inverse,
	ia32_get_op_estimated_cost,
	ia32_possible_memory_operand,
	ia32_perform_memory_operand,
};

static ia32_irn_ops_t ia32_irn_ops = {
	&ia32_irn_ops_if,
	NULL
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

static ir_entity *mcount = NULL;

#define ID(s) new_id_from_chars(s, sizeof(s) - 1)

static void ia32_before_abi(void *self) {
	lower_mode_b_config_t lower_mode_b_config = {
		mode_Iu,  /* lowered mode */
		mode_Bu,  /* prefered mode for set */
		0,        /* don't lower direct compares */
	};
	ia32_code_gen_t *cg = self;

	ir_lower_mode_b(cg->irg, &lower_mode_b_config);
	if (cg->dump)
		be_dump(cg->irg, "-lower_modeb", dump_ir_block_graph_sched);
	if (cg->gprof) {
		if (mcount == NULL) {
			ir_type *tp = new_type_method(ID("FKT.mcount"), 0, 0);
			mcount = new_entity(get_glob_type(), ID("mcount"), tp);
			/* FIXME: enter the right ld_ident here */
			set_entity_ld_ident(mcount, get_entity_ident(mcount));
			set_entity_visibility(mcount, visibility_external_allocated);
		}
		instrument_initcall(cg->irg, mcount);
	}
}

/**
 * Transforms the standard firm graph into
 * an ia32 firm graph
 */
static void ia32_prepare_graph(void *self) {
	ia32_code_gen_t *cg = self;

	/* do local optimisations */
	optimize_graph_df(cg->irg);

	/* TODO: we often have dead code reachable through out-edges here. So for
	 * now we rebuild edges (as we need correct user count for code selection)
	 */
#if 1
	edges_deactivate(cg->irg);
	edges_activate(cg->irg);
#endif

	if (cg->dump)
		be_dump(cg->irg, "-pre_transform", dump_ir_block_graph_sched);

#ifdef FIRM_GRGEN_BE
	/* transform nodes into assembler instructions by PBQP magic */
	ia32_transform_graph_by_pbqp(cg);
#endif

	if (cg->dump)
		be_dump(cg->irg, "-after_pbqp_transform", dump_ir_block_graph_sched);

	/* transform remaining nodes into assembler instructions */
	ia32_transform_graph(cg);

	/* do local optimisations (mainly CSE) */
	optimize_graph_df(cg->irg);

	if (cg->dump)
		be_dump(cg->irg, "-transformed", dump_ir_block_graph_sched);

	/* optimize address mode */
	ia32_optimize_graph(cg);

	if (cg->dump)
		be_dump(cg->irg, "-am", dump_ir_block_graph_sched);

	/* do code placement, to optimize the position of constants */
	place_code(cg->irg);

	if (cg->dump)
		be_dump(cg->irg, "-place", dump_ir_block_graph_sched);
}

/**
 * Dummy functions for hooks we don't need but which must be filled.
 */
static void ia32_before_sched(void *self) {
	(void) self;
}

static void turn_back_am(ir_node *node)
{
	ir_graph *irg   = current_ir_graph;
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_node  *block = get_nodes_block(node);
	ir_node  *base  = get_irn_n(node, n_ia32_base);
	ir_node  *index = get_irn_n(node, n_ia32_index);
	ir_node  *mem   = get_irn_n(node, n_ia32_mem);
	ir_node  *noreg = ia32_new_NoReg_gp(ia32_current_cg);
	ir_node  *load;
	ir_node  *load_res;
	ir_node  *mem_proj;
	const ir_edge_t *edge;

	load     = new_rd_ia32_Load(dbgi, irg, block, base, index, mem);
	load_res = new_rd_Proj(dbgi, irg, block, load, mode_Iu, pn_ia32_Load_res);

	ia32_copy_am_attrs(load, node);
	set_irn_n(node, n_ia32_mem, new_NoMem());

	switch (get_ia32_am_arity(node)) {
		case ia32_am_unary:
			set_irn_n(node, n_ia32_unary_op, load_res);
			break;

		case ia32_am_binary:
			if (is_ia32_Immediate(get_irn_n(node, n_ia32_Cmp_right))) {
				assert(is_ia32_Cmp(node)  || is_ia32_Cmp8Bit(node) ||
				       is_ia32_Test(node) || is_ia32_Test8Bit(node));
				set_irn_n(node, n_ia32_binary_left, load_res);
			} else {
				set_irn_n(node, n_ia32_binary_right, load_res);
			}
			break;

		case ia32_am_ternary:
			set_irn_n(node, n_ia32_binary_right, load_res);
			break;

		default: break;
	}
	set_irn_n(node, n_ia32_base, noreg);
	set_irn_n(node, n_ia32_index, noreg);
	set_ia32_am_offs_int(node, 0);
	set_ia32_am_sc(node, NULL);
	set_ia32_am_scale(node, 0);
	clear_ia32_am_sc_sign(node);

	/* rewire mem-proj */
	if (get_irn_mode(node) == mode_T) {
		mem_proj = NULL;
		foreach_out_edge(node, edge) {
			ir_node *out = get_edge_src_irn(edge);
			if(get_Proj_proj(out) == pn_ia32_mem) {
				mem_proj = out;
				break;
			}
		}

		if(mem_proj != NULL) {
			set_Proj_pred(mem_proj, load);
			set_Proj_proj(mem_proj, pn_ia32_Load_M);
		}
	}

	set_ia32_op_type(node, ia32_Normal);
	if (sched_is_scheduled(node))
		sched_add_before(node, load);
}

static ir_node *flags_remat(ir_node *node, ir_node *after)
{
	/* we should turn back source address mode when rematerializing nodes */
	ia32_op_type_t type;
	ir_node        *block;
	ir_node        *copy;

	if (is_Block(after)) {
		block = after;
	} else {
		block = get_nodes_block(after);
	}

	type = get_ia32_op_type(node);
	switch (type) {
		case ia32_AddrModeS: turn_back_am(node); break;

		case ia32_AddrModeD:
			/* TODO implement this later... */
			panic("found DestAM with flag user %+F this should not happen", node);
			break;

		default: assert(type == ia32_Normal); break;
	}

	copy = exact_copy(node);
	set_nodes_block(copy, block);
	sched_add_after(after, copy);

	return copy;
}

/**
 * Called before the register allocator.
 * Calculate a block schedule here. We need it for the x87
 * simulator and the emitter.
 */
static void ia32_before_ra(void *self) {
	ia32_code_gen_t *cg = self;

	/* setup fpu rounding modes */
	ia32_setup_fpu_mode(cg);

	/* fixup flags */
	be_sched_fix_flags(cg->birg, &ia32_reg_classes[CLASS_ia32_flags],
	                   &flags_remat);

	ia32_add_missing_keeps(cg);
}


/**
 * Transforms a be_Reload into a ia32 Load.
 */
static void transform_to_Load(ia32_code_gen_t *cg, ir_node *node) {
	ir_graph *irg        = get_irn_irg(node);
	dbg_info *dbg        = get_irn_dbg_info(node);
	ir_node *block       = get_nodes_block(node);
	ir_entity *ent       = be_get_frame_entity(node);
	ir_mode *mode        = get_irn_mode(node);
	ir_mode *spillmode   = get_spill_mode(node);
	ir_node *noreg       = ia32_new_NoReg_gp(cg);
	ir_node *sched_point = NULL;
	ir_node *ptr         = get_irg_frame(irg);
	ir_node *mem         = get_irn_n(node, be_pos_Reload_mem);
	ir_node *new_op, *proj;
	const arch_register_t *reg;

	if (sched_is_scheduled(node)) {
		sched_point = sched_prev(node);
	}

	if (mode_is_float(spillmode)) {
		if (ia32_cg_config.use_sse2)
			new_op = new_rd_ia32_xLoad(dbg, irg, block, ptr, noreg, mem, spillmode);
		else
			new_op = new_rd_ia32_vfld(dbg, irg, block, ptr, noreg, mem, spillmode);
	}
	else if (get_mode_size_bits(spillmode) == 128) {
		/* Reload 128 bit SSE registers */
		new_op = new_rd_ia32_xxLoad(dbg, irg, block, ptr, noreg, mem);
	}
	else
		new_op = new_rd_ia32_Load(dbg, irg, block, ptr, noreg, mem);

	set_ia32_op_type(new_op, ia32_AddrModeS);
	set_ia32_ls_mode(new_op, spillmode);
	set_ia32_frame_ent(new_op, ent);
	set_ia32_use_frame(new_op);

	DBG_OPT_RELOAD2LD(node, new_op);

	proj = new_rd_Proj(dbg, irg, block, new_op, mode, pn_ia32_Load_res);

	if (sched_point) {
		sched_add_after(sched_point, new_op);
		sched_remove(node);
	}

	/* copy the register from the old node to the new Load */
	reg = arch_get_irn_register(cg->arch_env, node);
	arch_set_irn_register(cg->arch_env, new_op, reg);

	SET_IA32_ORIG_NODE(new_op, ia32_get_old_node_name(cg, node));

	exchange(node, proj);
}

/**
 * Transforms a be_Spill node into a ia32 Store.
 */
static void transform_to_Store(ia32_code_gen_t *cg, ir_node *node) {
	ir_graph *irg  = get_irn_irg(node);
	dbg_info *dbg  = get_irn_dbg_info(node);
	ir_node *block = get_nodes_block(node);
	ir_entity *ent = be_get_frame_entity(node);
	const ir_node *spillval = get_irn_n(node, be_pos_Spill_val);
	ir_mode *mode  = get_spill_mode(spillval);
	ir_node *noreg = ia32_new_NoReg_gp(cg);
	ir_node *nomem = new_rd_NoMem(irg);
	ir_node *ptr   = get_irg_frame(irg);
	ir_node *val   = get_irn_n(node, be_pos_Spill_val);
	ir_node *store;
	ir_node *sched_point = NULL;

	if (sched_is_scheduled(node)) {
		sched_point = sched_prev(node);
	}

	/* No need to spill unknown values... */
	if(is_ia32_Unknown_GP(val) ||
		is_ia32_Unknown_VFP(val) ||
		is_ia32_Unknown_XMM(val)) {
		store = nomem;
		if(sched_point)
			sched_remove(node);

		exchange(node, store);
		return;
	}

	if (mode_is_float(mode)) {
		if (ia32_cg_config.use_sse2)
			store = new_rd_ia32_xStore(dbg, irg, block, ptr, noreg, nomem, val);
		else
			store = new_rd_ia32_vfst(dbg, irg, block, ptr, noreg, nomem, val, mode);
	} else if (get_mode_size_bits(mode) == 128) {
		/* Spill 128 bit SSE registers */
		store = new_rd_ia32_xxStore(dbg, irg, block, ptr, noreg, nomem, val);
	} else if (get_mode_size_bits(mode) == 8) {
		store = new_rd_ia32_Store8Bit(dbg, irg, block, ptr, noreg, nomem, val);
	} else {
		store = new_rd_ia32_Store(dbg, irg, block, ptr, noreg, nomem, val);
	}

	set_ia32_op_type(store, ia32_AddrModeD);
	set_ia32_ls_mode(store, mode);
	set_ia32_frame_ent(store, ent);
	set_ia32_use_frame(store);
	SET_IA32_ORIG_NODE(store, ia32_get_old_node_name(cg, node));
	DBG_OPT_SPILL2ST(node, store);

	if (sched_point) {
		sched_add_after(sched_point, store);
		sched_remove(node);
	}

	exchange(node, store);
}

static ir_node *create_push(ia32_code_gen_t *cg, ir_node *node, ir_node *schedpoint, ir_node *sp, ir_node *mem, ir_entity *ent) {
	ir_graph *irg = get_irn_irg(node);
	dbg_info *dbg = get_irn_dbg_info(node);
	ir_node *block = get_nodes_block(node);
	ir_node *noreg = ia32_new_NoReg_gp(cg);
	ir_node *frame = get_irg_frame(irg);

	ir_node *push = new_rd_ia32_Push(dbg, irg, block, frame, noreg, mem, noreg, sp);

	set_ia32_frame_ent(push, ent);
	set_ia32_use_frame(push);
	set_ia32_op_type(push, ia32_AddrModeS);
	set_ia32_ls_mode(push, mode_Is);

	sched_add_before(schedpoint, push);
	return push;
}

static ir_node *create_pop(ia32_code_gen_t *cg, ir_node *node, ir_node *schedpoint, ir_node *sp, ir_entity *ent) {
	ir_graph *irg = get_irn_irg(node);
	dbg_info *dbg = get_irn_dbg_info(node);
	ir_node *block = get_nodes_block(node);
	ir_node *noreg = ia32_new_NoReg_gp(cg);
	ir_node *frame = get_irg_frame(irg);

	ir_node *pop = new_rd_ia32_PopMem(dbg, irg, block, frame, noreg, new_NoMem(), sp);

	set_ia32_frame_ent(pop, ent);
	set_ia32_use_frame(pop);
	set_ia32_op_type(pop, ia32_AddrModeD);
	set_ia32_ls_mode(pop, mode_Is);

	sched_add_before(schedpoint, pop);

	return pop;
}

static ir_node* create_spproj(ia32_code_gen_t *cg, ir_node *node, ir_node *pred, int pos) {
	ir_graph *irg = get_irn_irg(node);
	dbg_info *dbg = get_irn_dbg_info(node);
	ir_node *block = get_nodes_block(node);
	ir_mode *spmode = mode_Iu;
	const arch_register_t *spreg = &ia32_gp_regs[REG_ESP];
	ir_node *sp;

	sp = new_rd_Proj(dbg, irg, block, pred, spmode, pos);
	arch_set_irn_register(cg->arch_env, sp, spreg);

	return sp;
}

/**
 * Transform MemPerm, currently we do this the ugly way and produce
 * push/pop into/from memory cascades. This is possible without using
 * any registers.
 */
static void transform_MemPerm(ia32_code_gen_t *cg, ir_node *node) {
	ir_graph *irg = get_irn_irg(node);
	ir_node *block = get_nodes_block(node);
	ir_node *in[1];
	ir_node *keep;
	int i, arity;
	ir_node *sp = be_abi_get_ignore_irn(cg->birg->abi, &ia32_gp_regs[REG_ESP]);
	const ir_edge_t *edge;
	const ir_edge_t *next;
	ir_node **pops;

	arity = be_get_MemPerm_entity_arity(node);
	pops = alloca(arity * sizeof(pops[0]));

	/* create Pushs */
	for(i = 0; i < arity; ++i) {
		ir_entity *inent = be_get_MemPerm_in_entity(node, i);
		ir_entity *outent = be_get_MemPerm_out_entity(node, i);
		ir_type *enttype = get_entity_type(inent);
		unsigned entsize = get_type_size_bytes(enttype);
		unsigned entsize2 = get_type_size_bytes(get_entity_type(outent));
		ir_node *mem = get_irn_n(node, i + 1);
		ir_node *push;

		/* work around cases where entities have different sizes */
		if(entsize2 < entsize)
			entsize = entsize2;
		assert( (entsize == 4 || entsize == 8) && "spillslot on x86 should be 32 or 64 bit");

		push = create_push(cg, node, node, sp, mem, inent);
		sp = create_spproj(cg, node, push, pn_ia32_Push_stack);
		if(entsize == 8) {
			/* add another push after the first one */
			push = create_push(cg, node, node, sp, mem, inent);
			add_ia32_am_offs_int(push, 4);
			sp = create_spproj(cg, node, push, pn_ia32_Push_stack);
		}

		set_irn_n(node, i, new_Bad());
	}

	/* create pops */
	for(i = arity - 1; i >= 0; --i) {
		ir_entity *inent = be_get_MemPerm_in_entity(node, i);
		ir_entity *outent = be_get_MemPerm_out_entity(node, i);
		ir_type *enttype = get_entity_type(outent);
		unsigned entsize = get_type_size_bytes(enttype);
		unsigned entsize2 = get_type_size_bytes(get_entity_type(inent));
		ir_node *pop;

		/* work around cases where entities have different sizes */
		if(entsize2 < entsize)
			entsize = entsize2;
		assert( (entsize == 4 || entsize == 8) && "spillslot on x86 should be 32 or 64 bit");

		pop = create_pop(cg, node, node, sp, outent);
		sp = create_spproj(cg, node, pop, pn_ia32_Pop_stack);
		if(entsize == 8) {
			add_ia32_am_offs_int(pop, 4);

			/* add another pop after the first one */
			pop = create_pop(cg, node, node, sp, outent);
			sp = create_spproj(cg, node, pop, pn_ia32_Pop_stack);
		}

		pops[i] = pop;
	}

	in[0] = sp;
	keep  = be_new_Keep(&ia32_reg_classes[CLASS_ia32_gp], irg, block, 1, in);
	sched_add_before(node, keep);

	/* exchange memprojs */
	foreach_out_edge_safe(node, edge, next) {
		ir_node *proj = get_edge_src_irn(edge);
		int p = get_Proj_proj(proj);

		assert(p < arity);

		set_Proj_pred(proj, pops[p]);
		set_Proj_proj(proj, pn_ia32_Pop_M);
	}

	/* remove memperm */
	arity = get_irn_arity(node);
	for(i = 0; i < arity; ++i) {
		set_irn_n(node, i, new_Bad());
	}
	sched_remove(node);
}

/**
 * Block-Walker: Calls the transform functions Spill and Reload.
 */
static void ia32_after_ra_walker(ir_node *block, void *env) {
	ir_node *node, *prev;
	ia32_code_gen_t *cg = env;

	/* beware: the schedule is changed here */
	for (node = sched_last(block); !sched_is_begin(node); node = prev) {
		prev = sched_prev(node);

		if (be_is_Reload(node)) {
			transform_to_Load(cg, node);
		} else if (be_is_Spill(node)) {
			transform_to_Store(cg, node);
		} else if (be_is_MemPerm(node)) {
			transform_MemPerm(cg, node);
		}
	}
}

/**
 * Collects nodes that need frame entities assigned.
 */
static void ia32_collect_frame_entity_nodes(ir_node *node, void *data)
{
	be_fec_env_t *env = data;

	if (be_is_Reload(node) && be_get_frame_entity(node) == NULL) {
		const ir_mode *mode = get_spill_mode_mode(get_irn_mode(node));
		int align = get_mode_size_bytes(mode);
		be_node_needs_frame_entity(env, node, mode, align);
	} else if(is_ia32_irn(node) && get_ia32_frame_ent(node) == NULL
	          && is_ia32_use_frame(node)) {
		if (is_ia32_need_stackent(node) || is_ia32_Load(node)) {
			const ir_mode     *mode  = get_ia32_ls_mode(node);
			const ia32_attr_t *attr  = get_ia32_attr_const(node);
			int                align = get_mode_size_bytes(mode);

			if(attr->data.need_64bit_stackent) {
				mode = mode_Ls;
			}
			if(attr->data.need_32bit_stackent) {
				mode = mode_Is;
			}
			be_node_needs_frame_entity(env, node, mode, align);
		} else if (is_ia32_vfild(node) || is_ia32_xLoad(node)
		           || is_ia32_vfld(node)) {
			const ir_mode *mode  = get_ia32_ls_mode(node);
			int            align = 4;
			be_node_needs_frame_entity(env, node, mode, align);
		} else if(is_ia32_FldCW(node)) {
			/* although 2 byte would be enough 4 byte performs best */
			const ir_mode *mode  = mode_Iu;
			int            align = 4;
			be_node_needs_frame_entity(env, node, mode, align);
		} else {
#ifndef NDEBUG
			assert(is_ia32_St(node) ||
 				   is_ia32_xStoreSimple(node) ||
				   is_ia32_vfst(node) ||
				   is_ia32_vfist(node) ||
			       is_ia32_FnstCW(node));
#endif
		}
	}
}

/**
 * We transform Spill and Reload here. This needs to be done before
 * stack biasing otherwise we would miss the corrected offset for these nodes.
 */
static void ia32_after_ra(void *self) {
	ia32_code_gen_t *cg = self;
	ir_graph *irg = cg->irg;
	be_fec_env_t *fec_env = be_new_frame_entity_coalescer(cg->birg);

	/* create and coalesce frame entities */
	irg_walk_graph(irg, NULL, ia32_collect_frame_entity_nodes, fec_env);
	be_assign_entities(fec_env);
	be_free_frame_entity_coalescer(fec_env);

	irg_block_walk_graph(irg, NULL, ia32_after_ra_walker, cg);
}

/**
 * Last touchups for the graph before emit: x87 simulation to replace the
 * virtual with real x87 instructions, creating a block schedule and peephole
 * optimisations.
 */
static void ia32_finish(void *self) {
	ia32_code_gen_t *cg = self;
	ir_graph        *irg = cg->irg;

	ia32_finish_irg(irg, cg);

	/* we might have to rewrite x87 virtual registers */
	if (cg->do_x87_sim) {
		x87_simulate_graph(cg->arch_env, cg->birg);
	}

	/* do peephole optimisations */
	ia32_peephole_optimization(cg);

	/* create block schedule, this also removes empty blocks which might
	 * produce critical edges */
	cg->blk_sched = be_create_block_schedule(irg, cg->birg->exec_freq);
}

/**
 * Emits the code, closes the output file and frees
 * the code generator interface.
 */
static void ia32_codegen(void *self) {
	ia32_code_gen_t *cg = self;
	ir_graph        *irg = cg->irg;

	ia32_gen_routine(cg, irg);

	cur_reg_set = NULL;

	/* remove it from the isa */
	cg->isa->cg = NULL;

	assert(ia32_current_cg == cg);
	ia32_current_cg = NULL;

	/* de-allocate code generator */
	del_set(cg->reg_set);
	free(cg);
}

/**
 * Returns the node representing the PIC base.
 */
static ir_node *ia32_get_pic_base(void *self) {
	ir_node         *block;
	ia32_code_gen_t *cg      = self;
	ir_node         *get_eip = cg->get_eip;
	if (get_eip != NULL)
		return get_eip;

	block       = get_irg_start_block(cg->irg);
	get_eip     = new_rd_ia32_GetEIP(NULL, cg->irg, block);
	cg->get_eip = get_eip;

	add_irn_dep(get_eip, get_irg_frame(cg->irg));

	return get_eip;
}

static void *ia32_cg_init(be_irg_t *birg);

static const arch_code_generator_if_t ia32_code_gen_if = {
	ia32_cg_init,
	ia32_get_pic_base,   /* return node used as base in pic code addresses */
	ia32_before_abi,     /* before abi introduce hook */
	ia32_prepare_graph,
	NULL,                /* spill */
	ia32_before_sched,   /* before scheduling hook */
	ia32_before_ra,      /* before register allocation hook */
	ia32_after_ra,       /* after register allocation hook */
	ia32_finish,         /* called before codegen */
	ia32_codegen         /* emit && done */
};

/**
 * Initializes a IA32 code generator.
 */
static void *ia32_cg_init(be_irg_t *birg) {
	ia32_isa_t      *isa = (ia32_isa_t *)birg->main_env->arch_env.isa;
	ia32_code_gen_t *cg  = xcalloc(1, sizeof(*cg));

	cg->impl      = &ia32_code_gen_if;
	cg->irg       = birg->irg;
	cg->reg_set   = new_set(ia32_cmp_irn_reg_assoc, 1024);
	cg->arch_env  = &birg->main_env->arch_env;
	cg->isa       = isa;
	cg->birg      = birg;
	cg->blk_sched = NULL;
	cg->dump      = (birg->main_env->options->dump_flags & DUMP_BE) ? 1 : 0;
	cg->gprof     = (birg->main_env->options->gprof) ? 1 : 0;

	if (cg->gprof) {
		/* Linux gprof implementation needs base pointer */
		birg->main_env->options->omit_fp = 0;
	}

	/* enter it */
	isa->cg = cg;

#ifndef NDEBUG
	if (isa->name_obst) {
		obstack_free(isa->name_obst, NULL);
		obstack_init(isa->name_obst);
	}
#endif /* NDEBUG */

	cur_reg_set = cg->reg_set;

	ia32_irn_ops.cg = cg;

	assert(ia32_current_cg == NULL);
	ia32_current_cg = cg;

	return (arch_code_generator_t *)cg;
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

/**
 * Set output modes for GCC
 */
static const tarval_mode_info mo_integer = {
	TVO_HEX,
	"0x",
	NULL,
};

/*
 * set the tarval output mode of all integer modes to decimal
 */
static void set_tarval_output_modes(void)
{
	int i;

	for (i = get_irp_n_modes() - 1; i >= 0; --i) {
		ir_mode *mode = get_irp_mode(i);

		if (mode_is_int(mode))
			set_tarval_mode_output_option(mode, &mo_integer);
	}
}

const arch_isa_if_t ia32_isa_if;

/**
 * The template that generates a new ISA object.
 * Note that this template can be changed by command line
 * arguments.
 */
static ia32_isa_t ia32_isa_template = {
	{
		&ia32_isa_if,            /* isa interface implementation */
		&ia32_gp_regs[REG_ESP],  /* stack pointer register */
		&ia32_gp_regs[REG_EBP],  /* base pointer register */
		-1,                      /* stack direction */
		16,                      /* stack alignment */
		NULL,                    /* main environment */
		7,                       /* costs for a spill instruction */
		5,                       /* costs for a reload instruction */
	},
	NULL,                    /* 16bit register names */
	NULL,                    /* 8bit register names */
	NULL,                    /* 8bit register names high */
	NULL,                    /* types */
	NULL,                    /* tv_ents */
	NULL,                    /* current code generator */
	NULL,                    /* abstract machine */
#ifndef NDEBUG
	NULL,                    /* name obstack */
#endif
};

/**
 * Initializes the backend ISA.
 */
static void *ia32_init(FILE *file_handle) {
	static int inited = 0;
	ia32_isa_t *isa;

	if (inited)
		return NULL;
	inited = 1;

	set_tarval_output_modes();

	isa = xmalloc(sizeof(*isa));
	memcpy(isa, &ia32_isa_template, sizeof(*isa));

	if(mode_fpcw == NULL) {
		mode_fpcw = new_ir_mode("Fpcw", irms_int_number, 16, 0, irma_none, 0);
	}

	ia32_register_init();
	ia32_create_opcodes();

	be_emit_init(file_handle);
	isa->regs_16bit     = pmap_create();
	isa->regs_8bit      = pmap_create();
	isa->regs_8bit_high = pmap_create();
	isa->types          = pmap_create();
	isa->tv_ent         = pmap_create();
	isa->cpu            = ia32_init_machine_description();

	ia32_build_16bit_reg_map(isa->regs_16bit);
	ia32_build_8bit_reg_map(isa->regs_8bit);
	ia32_build_8bit_reg_map_high(isa->regs_8bit_high);

#ifndef NDEBUG
	isa->name_obst = xmalloc(sizeof(*isa->name_obst));
	obstack_init(isa->name_obst);
#endif /* NDEBUG */

	/* enter the ISA object into the intrinsic environment */
	intrinsic_env.isa = isa;
	ia32_handle_intrinsics();

	/* needed for the debug support */
	be_gas_emit_switch_section(GAS_SECTION_TEXT);
	be_emit_cstring(".Ltext0:\n");
	be_emit_write_line();

	/* we mark referenced global entities, so we can only emit those which
	 * are actually referenced. (Note: you mustn't use the type visited flag
	 * elsewhere in the backend)
	 */
	inc_master_type_visited();

	return isa;
}



/**
 * Closes the output file and frees the ISA structure.
 */
static void ia32_done(void *self) {
	ia32_isa_t *isa = self;

	/* emit now all global declarations */
	be_gas_emit_decls(isa->arch_isa.main_env, 1);

	pmap_destroy(isa->regs_16bit);
	pmap_destroy(isa->regs_8bit);
	pmap_destroy(isa->regs_8bit_high);
	pmap_destroy(isa->tv_ent);
	pmap_destroy(isa->types);

#ifndef NDEBUG
	obstack_free(isa->name_obst, NULL);
#endif /* NDEBUG */

	be_emit_exit();

	free(self);
}


/**
 * Return the number of register classes for this architecture.
 * We report always these:
 *  - the general purpose registers
 *  - the SSE floating point register set
 *  - the virtual floating point registers
 *  - the SSE vector register set
 */
static unsigned ia32_get_n_reg_class(const void *self) {
	(void) self;
	return N_CLASSES;
}

/**
 * Return the register class for index i.
 */
static const arch_register_class_t *ia32_get_reg_class(const void *self,
                                                       unsigned i)
{
	(void) self;
	assert(i < N_CLASSES);
	return &ia32_reg_classes[i];
}

/**
 * Get the register class which shall be used to store a value of a given mode.
 * @param self The this pointer.
 * @param mode The mode in question.
 * @return A register class which can hold values of the given mode.
 */
const arch_register_class_t *ia32_get_reg_class_for_mode(const void *self,
		const ir_mode *mode)
{
	(void) self;

	if (mode_is_float(mode)) {
		return ia32_cg_config.use_sse2 ? &ia32_reg_classes[CLASS_ia32_xmm] : &ia32_reg_classes[CLASS_ia32_vfp];
	}
	else
		return &ia32_reg_classes[CLASS_ia32_gp];
}

/**
 * Get the ABI restrictions for procedure calls.
 * @param self        The this pointer.
 * @param method_type The type of the method (procedure) in question.
 * @param abi         The abi object to be modified
 */
static void ia32_get_call_abi(const void *self, ir_type *method_type,
                              be_abi_call_t *abi)
{
	ir_type  *tp;
	ir_mode  *mode;
	unsigned  cc;
	int       n, i, regnum;
	be_abi_call_flags_t call_flags = be_abi_call_get_flags(abi);
	(void) self;

	/* set abi flags for calls */
	call_flags.bits.left_to_right         = 0;  /* always last arg first on stack */
	call_flags.bits.store_args_sequential = 0;
	/* call_flags.bits.try_omit_fp                 not changed: can handle both settings */
	call_flags.bits.fp_free               = 0;  /* the frame pointer is fixed in IA32 */
	call_flags.bits.call_has_imm          = 1;  /* IA32 calls can have immediate address */

	/* set parameter passing style */
	be_abi_call_set_flags(abi, call_flags, &ia32_abi_callbacks);

	if (get_method_variadicity(method_type) == variadicity_variadic) {
		/* pass all parameters of a variadic function on the stack */
		cc = cc_cdecl_set;
	} else {
		cc = get_method_calling_convention(method_type);
		if (get_method_additional_properties(method_type) & mtp_property_private
				&& (ia32_cg_config.optimize_cc)) {
			/* set the calling conventions to register parameter */
			cc = (cc & ~cc_bits) | cc_reg_param;
		}
	}

	/* we have to pop the shadow parameter ourself for compound calls */
	if( (get_method_calling_convention(method_type) & cc_compound_ret)
			&& !(cc & cc_reg_param)) {
		be_abi_call_set_pop(abi, get_mode_size_bytes(mode_P_data));
	}

	n = get_method_n_params(method_type);
	for (i = regnum = 0; i < n; i++) {
		ir_mode               *mode;
		const arch_register_t *reg = NULL;

		tp   = get_method_param_type(method_type, i);
		mode = get_type_mode(tp);
		if (mode != NULL) {
			reg  = ia32_get_RegParam_reg(cc, regnum, mode);
		}
		if (reg != NULL) {
			be_abi_call_param_reg(abi, i, reg);
			++regnum;
		} else {
			/* Micro optimisation: if the mode is shorter than 4 bytes, load 4 bytes.
			 * movl has a shorter opcode than mov[sz][bw]l */
			ir_mode *load_mode = mode;
			if (mode != NULL && get_mode_size_bytes(mode) < 4) load_mode = mode_Iu;
			be_abi_call_param_stack(abi, i, load_mode, 4, 0, 0);
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

		be_abi_call_res_reg(abi, 0, &ia32_gp_regs[REG_EAX]);
		be_abi_call_res_reg(abi, 1, &ia32_gp_regs[REG_EDX]);
	}
	else if (n == 1) {
		const arch_register_t *reg;

		tp   = get_method_res_type(method_type, 0);
		assert(is_atomic_type(tp));
		mode = get_type_mode(tp);

		reg = mode_is_float(mode) ? &ia32_vfp_regs[REG_VF0] : &ia32_gp_regs[REG_EAX];

		be_abi_call_res_reg(abi, 0, reg);
	}
}


static const void *ia32_get_irn_ops(const arch_irn_handler_t *self,
                                    const ir_node *irn)
{
	(void) self;
	(void) irn;
	return &ia32_irn_ops;
}

const arch_irn_handler_t ia32_irn_handler = {
	ia32_get_irn_ops
};

const arch_irn_handler_t *ia32_get_irn_handler(const void *self)
{
	(void) self;
	return &ia32_irn_handler;
}

int ia32_to_appear_in_schedule(void *block_env, const ir_node *irn)
{
	(void) block_env;

	if(!is_ia32_irn(irn)) {
		return -1;
	}

	if(is_ia32_NoReg_GP(irn) || is_ia32_NoReg_VFP(irn) || is_ia32_NoReg_XMM(irn)
		|| is_ia32_Unknown_GP(irn) || is_ia32_Unknown_XMM(irn)
		|| is_ia32_Unknown_VFP(irn) || is_ia32_ChangeCW(irn)
		|| is_ia32_Immediate(irn))
		return 0;

	return 1;
}

/**
 * Initializes the code generator interface.
 */
static const arch_code_generator_if_t *ia32_get_code_generator_if(void *self)
{
	(void) self;
	return &ia32_code_gen_if;
}

/**
 * Returns the estimated execution time of an ia32 irn.
 */
static sched_timestep_t ia32_sched_exectime(void *env, const ir_node *irn) {
	const arch_env_t *arch_env = env;
	return is_ia32_irn(irn) ? ia32_get_op_estimated_cost(arch_get_irn_ops(arch_env, irn), irn) : 1;
}

list_sched_selector_t ia32_sched_selector;

/**
 * Returns the reg_pressure scheduler with to_appear_in_schedule() overloaded
 */
static const list_sched_selector_t *ia32_get_list_sched_selector(
		const void *self, list_sched_selector_t *selector)
{
	(void) self;
	memcpy(&ia32_sched_selector, selector, sizeof(ia32_sched_selector));
	ia32_sched_selector.exectime              = ia32_sched_exectime;
	ia32_sched_selector.to_appear_in_schedule = ia32_to_appear_in_schedule;
	return &ia32_sched_selector;
}

static const ilp_sched_selector_t *ia32_get_ilp_sched_selector(const void *self)
{
	(void) self;
	return NULL;
}

/**
 * Returns the necessary byte alignment for storing a register of given class.
 */
static int ia32_get_reg_class_alignment(const void *self,
                                        const arch_register_class_t *cls)
{
	ir_mode *mode = arch_register_class_mode(cls);
	int bytes     = get_mode_size_bytes(mode);
	(void) self;

	if (mode_is_float(mode) && bytes > 8)
		return 16;
	return bytes;
}

static const be_execution_unit_t ***ia32_get_allowed_execution_units(
		const void *self, const ir_node *irn)
{
	static const be_execution_unit_t *_allowed_units_BRANCH[] = {
		&ia32_execution_units_BRANCH[IA32_EXECUNIT_TP_BRANCH_BRANCH1],
		&ia32_execution_units_BRANCH[IA32_EXECUNIT_TP_BRANCH_BRANCH2],
		NULL,
	};
	static const be_execution_unit_t *_allowed_units_GP[] = {
		&ia32_execution_units_GP[IA32_EXECUNIT_TP_GP_GP_EAX],
		&ia32_execution_units_GP[IA32_EXECUNIT_TP_GP_GP_EBX],
		&ia32_execution_units_GP[IA32_EXECUNIT_TP_GP_GP_ECX],
		&ia32_execution_units_GP[IA32_EXECUNIT_TP_GP_GP_EDX],
		&ia32_execution_units_GP[IA32_EXECUNIT_TP_GP_GP_ESI],
		&ia32_execution_units_GP[IA32_EXECUNIT_TP_GP_GP_EDI],
		&ia32_execution_units_GP[IA32_EXECUNIT_TP_GP_GP_EBP],
		NULL,
	};
	static const be_execution_unit_t *_allowed_units_DUMMY[] = {
		&be_machine_execution_units_DUMMY[0],
		NULL,
	};
	static const be_execution_unit_t **_units_callret[] = {
		_allowed_units_BRANCH,
		NULL
	};
	static const be_execution_unit_t **_units_other[] = {
		_allowed_units_GP,
		NULL
	};
	static const be_execution_unit_t **_units_dummy[] = {
		_allowed_units_DUMMY,
		NULL
	};
	const be_execution_unit_t ***ret;
	(void) self;

	if (is_ia32_irn(irn)) {
		ret = get_ia32_exec_units(irn);
	}
	else if (is_be_node(irn)) {
		if (be_is_Call(irn) || be_is_Return(irn)) {
			ret = _units_callret;
		}
		else if (be_is_Barrier(irn)) {
			ret = _units_dummy;
		}
		else {
			 ret = _units_other;
		}
	}
	else {
		ret = _units_dummy;
	}

	return ret;
}

/**
 * Return the abstract ia32 machine.
 */
static const be_machine_t *ia32_get_machine(const void *self) {
	const ia32_isa_t *isa = self;
	return isa->cpu;
}

/**
 * Return irp irgs in the desired order.
 */
static ir_graph **ia32_get_irg_list(const void *self, ir_graph ***irg_list)
{
	(void) self;
	(void) irg_list;
	return NULL;
}

/**
 * Allows or disallows the creation of Psi nodes for the given Phi nodes.
 * @return 1 if allowed, 0 otherwise
 */
static int ia32_is_psi_allowed(ir_node *sel, ir_node *phi_list, int i, int j)
{
	ir_node *phi;

	(void)sel;
	(void)i;
	(void)j;

	if(!ia32_cg_config.use_cmov) {
		/* TODO: we could still handle abs(x)... */
		return 0;
	}

	/* we can't handle psis with 64bit compares yet */
	if(is_Proj(sel)) {
		ir_node *pred = get_Proj_pred(sel);
		if(is_Cmp(pred)) {
			ir_node *left     = get_Cmp_left(pred);
			ir_mode *cmp_mode = get_irn_mode(left);
			if(!mode_is_float(cmp_mode) && get_mode_size_bits(cmp_mode) > 32)
				return 0;
		}
	}

	/* check the Phi nodes */
	for (phi = phi_list; phi; phi = get_irn_link(phi)) {
		ir_mode *mode = get_irn_mode(phi);

		if (mode_is_float(mode) || get_mode_size_bits(mode) > 32)
			return 0;
	}

	return 1;
}

/**
 * Returns the libFirm configuration parameter for this backend.
 */
static const backend_params *ia32_get_libfirm_params(void) {
	static const ir_settings_if_conv_t ifconv = {
		4,                    /* maxdepth, doesn't matter for Psi-conversion */
		ia32_is_psi_allowed   /* allows or disallows Psi creation for given selector */
	};
	static const ir_settings_arch_dep_t ad = {
		1,                   /* also use subs */
		4,                   /* maximum shifts */
		31,                  /* maximum shift amount */
		ia32_evaluate_insn,  /* evaluate the instruction sequence */

		1,  /* allow Mulhs */
		1,  /* allow Mulus */
		32  /* Mulh allowed up to 32 bit */
	};
	static backend_params p = {
		1,     /* need dword lowering */
		1,     /* support inline assembly */
		NULL,  /* no additional opcodes */
		NULL,  /* will be set later */
		ia32_create_intrinsic_fkt,
		&intrinsic_env,  /* context for ia32_create_intrinsic_fkt */
		NULL,  /* will be set below */
	};

	ia32_setup_cg_config();

	p.dep_param    = &ad;
	p.if_conv_info = &ifconv;
	return &p;
}

static const lc_opt_enum_int_items_t gas_items[] = {
	{ "elf",     GAS_FLAVOUR_ELF },
	{ "mingw",   GAS_FLAVOUR_MINGW  },
	{ "yasm",    GAS_FLAVOUR_YASM   },
	{ "macho",   GAS_FLAVOUR_MACH_O },
	{ NULL,      0 }
};

static lc_opt_enum_int_var_t gas_var = {
	(int*) &be_gas_flavour, gas_items
};

static const lc_opt_table_entry_t ia32_options[] = {
	LC_OPT_ENT_ENUM_INT("gasmode", "set the GAS compatibility mode", &gas_var),
	LC_OPT_ENT_INT("stackalign", "set stack alignment for calls",
	               &ia32_isa_template.arch_isa.stack_alignment),
	LC_OPT_LAST
};

const arch_isa_if_t ia32_isa_if = {
	ia32_init,
	ia32_done,
	ia32_get_n_reg_class,
	ia32_get_reg_class,
	ia32_get_reg_class_for_mode,
	ia32_get_call_abi,
	ia32_get_irn_handler,
	ia32_get_code_generator_if,
	ia32_get_list_sched_selector,
	ia32_get_ilp_sched_selector,
	ia32_get_reg_class_alignment,
	ia32_get_libfirm_params,
	ia32_get_allowed_execution_units,
	ia32_get_machine,
	ia32_get_irg_list,
};

void ia32_init_emitter(void);
void ia32_init_finish(void);
void ia32_init_optimize(void);
void ia32_init_transform(void);
void ia32_init_x87(void);

void be_init_arch_ia32(void)
{
	lc_opt_entry_t *be_grp   = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *ia32_grp = lc_opt_get_grp(be_grp, "ia32");

	lc_opt_add_table(ia32_grp, ia32_options);
	be_register_isa_if("ia32", &ia32_isa_if);

	FIRM_DBG_REGISTER(dbg, "firm.be.ia32.cg");

	ia32_init_emitter();
	ia32_init_finish();
	ia32_init_optimize();
	ia32_init_transform();
	ia32_init_x87();
	ia32_init_architecture();
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_arch_ia32);
