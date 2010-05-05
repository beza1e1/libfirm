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
 * @brief       This file implements the IR transformation from firm into
 *              ia32-Firm.
 * @author      Christian Wuerdig, Matthias Braun
 * @version     $Id$
 */
#include "config.h"

#include <limits.h>
#include <stdbool.h>

#include "irargs_t.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "iropt_t.h"
#include "irop_t.h"
#include "irprog_t.h"
#include "iredges_t.h"
#include "irgmod.h"
#include "irvrfy.h"
#include "ircons.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "debug.h"
#include "irdom.h"
#include "error.h"
#include "array_t.h"
#include "height.h"

#include "../benode.h"
#include "../besched.h"
#include "../beabi.h"
#include "../beutil.h"
#include "../beirg.h"
#include "../betranshlp.h"
#include "../be_t.h"

#include "bearch_ia32_t.h"
#include "ia32_common_transform.h"
#include "ia32_nodes_attr.h"
#include "ia32_transform.h"
#include "ia32_new_nodes.h"
#include "ia32_map_regs.h"
#include "ia32_dbg_stat.h"
#include "ia32_optimize.h"
#include "ia32_util.h"
#include "ia32_address_mode.h"
#include "ia32_architecture.h"

#include "gen_ia32_regalloc_if.h"

/* define this to construct SSE constants instead of load them */
#undef CONSTRUCT_SSE_CONST


#define SFP_SIGN   "0x80000000"
#define DFP_SIGN   "0x8000000000000000"
#define SFP_ABS    "0x7FFFFFFF"
#define DFP_ABS    "0x7FFFFFFFFFFFFFFF"
#define DFP_INTMAX "9223372036854775807"
#define ULL_BIAS   "18446744073709551616"

#define ENT_SFP_SIGN ".LC_ia32_sfp_sign"
#define ENT_DFP_SIGN ".LC_ia32_dfp_sign"
#define ENT_SFP_ABS  ".LC_ia32_sfp_abs"
#define ENT_DFP_ABS  ".LC_ia32_dfp_abs"
#define ENT_ULL_BIAS ".LC_ia32_ull_bias"

#define mode_vfp	(ia32_reg_classes[CLASS_ia32_vfp].mode)
#define mode_xmm    (ia32_reg_classes[CLASS_ia32_xmm].mode)

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static ir_node         *initial_fpcw = NULL;
int                     no_pic_adjust;

typedef ir_node *construct_binop_func(dbg_info *db, ir_node *block,
        ir_node *base, ir_node *index, ir_node *mem, ir_node *op1,
        ir_node *op2);

typedef ir_node *construct_binop_flags_func(dbg_info *db, ir_node *block,
        ir_node *base, ir_node *index, ir_node *mem, ir_node *op1, ir_node *op2,
        ir_node *flags);

typedef ir_node *construct_shift_func(dbg_info *db, ir_node *block,
        ir_node *op1, ir_node *op2);

typedef ir_node *construct_binop_dest_func(dbg_info *db, ir_node *block,
        ir_node *base, ir_node *index, ir_node *mem, ir_node *op);

typedef ir_node *construct_unop_dest_func(dbg_info *db, ir_node *block,
        ir_node *base, ir_node *index, ir_node *mem);

typedef ir_node *construct_binop_float_func(dbg_info *db, ir_node *block,
        ir_node *base, ir_node *index, ir_node *mem, ir_node *op1, ir_node *op2,
        ir_node *fpcw);

typedef ir_node *construct_unop_func(dbg_info *db, ir_node *block, ir_node *op);

static ir_node *create_immediate_or_transform(ir_node *node,
                                              char immediate_constraint_type);

static ir_node *create_I2I_Conv(ir_mode *src_mode, ir_mode *tgt_mode,
                                dbg_info *dbgi, ir_node *block,
                                ir_node *op, ir_node *orig_node);

/* its enough to have those once */
static ir_node *nomem, *noreg_GP;

/** a list to postprocess all calls */
static ir_node **call_list;
static ir_type **call_types;

/** Return non-zero is a node represents the 0 constant. */
static bool is_Const_0(ir_node *node)
{
	return is_Const(node) && is_Const_null(node);
}

/** Return non-zero is a node represents the 1 constant. */
static bool is_Const_1(ir_node *node)
{
	return is_Const(node) && is_Const_one(node);
}

/** Return non-zero is a node represents the -1 constant. */
static bool is_Const_Minus_1(ir_node *node)
{
	return is_Const(node) && is_Const_all_one(node);
}

/**
 * returns true if constant can be created with a simple float command
 */
static bool is_simple_x87_Const(ir_node *node)
{
	tarval *tv = get_Const_tarval(node);
	if (tarval_is_null(tv) || tarval_is_one(tv))
		return true;

	/* TODO: match all the other float constants */
	return false;
}

/**
 * returns true if constant can be created with a simple float command
 */
static bool is_simple_sse_Const(ir_node *node)
{
	tarval  *tv   = get_Const_tarval(node);
	ir_mode *mode = get_tarval_mode(tv);

	if (mode == mode_F)
		return true;

	if (tarval_is_null(tv)
#ifdef CONSTRUCT_SSE_CONST
	    || tarval_is_one(tv)
#endif
	   )
		return true;
#ifdef CONSTRUCT_SSE_CONST
	if (mode == mode_D) {
		unsigned val = get_tarval_sub_bits(tv, 0) |
			(get_tarval_sub_bits(tv, 1) << 8) |
			(get_tarval_sub_bits(tv, 2) << 16) |
			(get_tarval_sub_bits(tv, 3) << 24);
		if (val == 0)
			/* lower 32bit are zero, really a 32bit constant */
			return true;
	}
#endif /* CONSTRUCT_SSE_CONST */
	/* TODO: match all the other float constants */
	return false;
}

/**
 * return NoREG or pic_base in case of PIC.
 * This is necessary as base address for newly created symbols
 */
static ir_node *get_symconst_base(void)
{
	if (env_cg->birg->main_env->options->pic) {
		return arch_code_generator_get_pic_base(env_cg);
	}

	return noreg_GP;
}

/**
 * Transforms a Const.
 */
static ir_node *gen_Const(ir_node *node)
{
	ir_node  *old_block = get_nodes_block(node);
	ir_node  *block     = be_transform_node(old_block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_mode  *mode      = get_irn_mode(node);

	assert(is_Const(node));

	if (mode_is_float(mode)) {
		ir_node   *res   = NULL;
		ir_node   *load;
		ir_node   *base;
		ir_entity *floatent;

		if (ia32_cg_config.use_sse2) {
			tarval *tv = get_Const_tarval(node);
			if (tarval_is_null(tv)) {
				load = new_bd_ia32_xZero(dbgi, block);
				set_ia32_ls_mode(load, mode);
				res  = load;
#ifdef CONSTRUCT_SSE_CONST
			} else if (tarval_is_one(tv)) {
				int     cnst  = mode == mode_F ? 26 : 55;
				ir_node *imm1 = ia32_create_Immediate(NULL, 0, cnst);
				ir_node *imm2 = ia32_create_Immediate(NULL, 0, 2);
				ir_node *pslld, *psrld;

				load = new_bd_ia32_xAllOnes(dbgi, block);
				set_ia32_ls_mode(load, mode);
				pslld = new_bd_ia32_xPslld(dbgi, block, load, imm1);
				set_ia32_ls_mode(pslld, mode);
				psrld = new_bd_ia32_xPsrld(dbgi, block, pslld, imm2);
				set_ia32_ls_mode(psrld, mode);
				res = psrld;
#endif /* CONSTRUCT_SSE_CONST */
			} else if (mode == mode_F) {
				/* we can place any 32bit constant by using a movd gp, sse */
				unsigned val = get_tarval_sub_bits(tv, 0) |
				               (get_tarval_sub_bits(tv, 1) << 8) |
				               (get_tarval_sub_bits(tv, 2) << 16) |
				               (get_tarval_sub_bits(tv, 3) << 24);
				ir_node *cnst = new_bd_ia32_Const(dbgi, block, NULL, 0, 0, val);
				load = new_bd_ia32_xMovd(dbgi, block, cnst);
				set_ia32_ls_mode(load, mode);
				res = load;
			} else {
#ifdef CONSTRUCT_SSE_CONST
				if (mode == mode_D) {
					unsigned val = get_tarval_sub_bits(tv, 0) |
						(get_tarval_sub_bits(tv, 1) << 8) |
						(get_tarval_sub_bits(tv, 2) << 16) |
						(get_tarval_sub_bits(tv, 3) << 24);
					if (val == 0) {
						ir_node *imm32 = ia32_create_Immediate(NULL, 0, 32);
						ir_node *cnst, *psllq;

						/* fine, lower 32bit are zero, produce 32bit value */
						val = get_tarval_sub_bits(tv, 4) |
							(get_tarval_sub_bits(tv, 5) << 8) |
							(get_tarval_sub_bits(tv, 6) << 16) |
							(get_tarval_sub_bits(tv, 7) << 24);
						cnst = new_bd_ia32_Const(dbgi, block, NULL, 0, 0, val);
						load = new_bd_ia32_xMovd(dbgi, block, cnst);
						set_ia32_ls_mode(load, mode);
						psllq = new_bd_ia32_xPsllq(dbgi, block, load, imm32);
						set_ia32_ls_mode(psllq, mode);
						res = psllq;
						goto end;
					}
				}
#endif /* CONSTRUCT_SSE_CONST */
				floatent = create_float_const_entity(node);

				base     = get_symconst_base();
				load     = new_bd_ia32_xLoad(dbgi, block, base, noreg_GP, nomem,
				                             mode);
				set_ia32_op_type(load, ia32_AddrModeS);
				set_ia32_am_sc(load, floatent);
				arch_irn_add_flags(load, arch_irn_flags_rematerializable);
				res = new_r_Proj(load, mode_xmm, pn_ia32_xLoad_res);
			}
		} else {
			if (is_Const_null(node)) {
				load = new_bd_ia32_vfldz(dbgi, block);
				res  = load;
				set_ia32_ls_mode(load, mode);
			} else if (is_Const_one(node)) {
				load = new_bd_ia32_vfld1(dbgi, block);
				res  = load;
				set_ia32_ls_mode(load, mode);
			} else {
				ir_mode *ls_mode;
				ir_node *base;

				floatent = create_float_const_entity(node);
				/* create_float_const_ent is smart and sometimes creates
				   smaller entities */
				ls_mode  = get_type_mode(get_entity_type(floatent));
				base     = get_symconst_base();
				load     = new_bd_ia32_vfld(dbgi, block, base, noreg_GP, nomem,
				                            ls_mode);
				set_ia32_op_type(load, ia32_AddrModeS);
				set_ia32_am_sc(load, floatent);
				arch_irn_add_flags(load, arch_irn_flags_rematerializable);
				res = new_r_Proj(load, mode_vfp, pn_ia32_vfld_res);
			}
		}
#ifdef CONSTRUCT_SSE_CONST
end:
#endif /* CONSTRUCT_SSE_CONST */
		SET_IA32_ORIG_NODE(load, node);

		be_dep_on_frame(load);
		return res;
	} else { /* non-float mode */
		ir_node *cnst;
		tarval  *tv = get_Const_tarval(node);
		long     val;

		tv = tarval_convert_to(tv, mode_Iu);

		if (tv == get_tarval_bad() || tv == get_tarval_undefined() ||
		    tv == NULL) {
			panic("couldn't convert constant tarval (%+F)", node);
		}
		val = get_tarval_long(tv);

		cnst = new_bd_ia32_Const(dbgi, block, NULL, 0, 0, val);
		SET_IA32_ORIG_NODE(cnst, node);

		be_dep_on_frame(cnst);
		return cnst;
	}
}

/**
 * Transforms a SymConst.
 */
static ir_node *gen_SymConst(ir_node *node)
{
	ir_node  *old_block = get_nodes_block(node);
	ir_node  *block = be_transform_node(old_block);
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_mode  *mode  = get_irn_mode(node);
	ir_node  *cnst;

	if (mode_is_float(mode)) {
		if (ia32_cg_config.use_sse2)
			cnst = new_bd_ia32_xLoad(dbgi, block, noreg_GP, noreg_GP, nomem, mode_E);
		else
			cnst = new_bd_ia32_vfld(dbgi, block, noreg_GP, noreg_GP, nomem, mode_E);
		set_ia32_am_sc(cnst, get_SymConst_entity(node));
		set_ia32_use_frame(cnst);
	} else {
		ir_entity *entity;

		if (get_SymConst_kind(node) != symconst_addr_ent) {
			panic("backend only support symconst_addr_ent (at %+F)", node);
		}
		entity = get_SymConst_entity(node);
		cnst = new_bd_ia32_Const(dbgi, block, entity, 0, 0, 0);
	}

	SET_IA32_ORIG_NODE(cnst, node);

	be_dep_on_frame(cnst);
	return cnst;
}

/**
 * Create a float type for the given mode and cache it.
 *
 * @param mode   the mode for the float type (might be integer mode for SSE2 types)
 * @param align  alignment
 */
static ir_type *ia32_create_float_type(ir_mode *mode, unsigned align)
{
	ir_type *tp;

	assert(align <= 16);

	if (mode == mode_Iu) {
		static ir_type *int_Iu[16] = {NULL, };

		if (int_Iu[align] == NULL) {
			int_Iu[align] = tp = new_type_primitive(mode);
			/* set the specified alignment */
			set_type_alignment_bytes(tp, align);
		}
		return int_Iu[align];
	} else if (mode == mode_Lu) {
		static ir_type *int_Lu[16] = {NULL, };

		if (int_Lu[align] == NULL) {
			int_Lu[align] = tp = new_type_primitive(mode);
			/* set the specified alignment */
			set_type_alignment_bytes(tp, align);
		}
		return int_Lu[align];
	} else if (mode == mode_F) {
		static ir_type *float_F[16] = {NULL, };

		if (float_F[align] == NULL) {
			float_F[align] = tp = new_type_primitive(mode);
			/* set the specified alignment */
			set_type_alignment_bytes(tp, align);
		}
		return float_F[align];
	} else if (mode == mode_D) {
		static ir_type *float_D[16] = {NULL, };

		if (float_D[align] == NULL) {
			float_D[align] = tp = new_type_primitive(mode);
			/* set the specified alignment */
			set_type_alignment_bytes(tp, align);
		}
		return float_D[align];
	} else {
		static ir_type *float_E[16] = {NULL, };

		if (float_E[align] == NULL) {
			float_E[align] = tp = new_type_primitive(mode);
			/* set the specified alignment */
			set_type_alignment_bytes(tp, align);
		}
		return float_E[align];
	}
}

/**
 * Create a float[2] array type for the given atomic type.
 *
 * @param tp  the atomic type
 */
static ir_type *ia32_create_float_array(ir_type *tp)
{
	ir_mode  *mode = get_type_mode(tp);
	unsigned align = get_type_alignment_bytes(tp);
	ir_type  *arr;

	assert(align <= 16);

	if (mode == mode_F) {
		static ir_type *float_F[16] = {NULL, };

		if (float_F[align] != NULL)
			return float_F[align];
		arr = float_F[align] = new_type_array(1, tp);
	} else if (mode == mode_D) {
		static ir_type *float_D[16] = {NULL, };

		if (float_D[align] != NULL)
			return float_D[align];
		arr = float_D[align] = new_type_array(1, tp);
	} else {
		static ir_type *float_E[16] = {NULL, };

		if (float_E[align] != NULL)
			return float_E[align];
		arr = float_E[align] = new_type_array(1, tp);
	}
	set_type_alignment_bytes(arr, align);
	set_type_size_bytes(arr, 2 * get_type_size_bytes(tp));
	set_type_state(arr, layout_fixed);
	return arr;
}

/* Generates an entity for a known FP const (used for FP Neg + Abs) */
ir_entity *ia32_gen_fp_known_const(ia32_known_const_t kct)
{
	static const struct {
		const char *ent_name;
		const char *cnst_str;
		char mode;
		unsigned char align;
	} names [ia32_known_const_max] = {
		{ ENT_SFP_SIGN, SFP_SIGN,   0, 16 }, /* ia32_SSIGN */
		{ ENT_DFP_SIGN, DFP_SIGN,   1, 16 }, /* ia32_DSIGN */
		{ ENT_SFP_ABS,  SFP_ABS,    0, 16 }, /* ia32_SABS */
		{ ENT_DFP_ABS,  DFP_ABS,    1, 16 }, /* ia32_DABS */
		{ ENT_ULL_BIAS, ULL_BIAS,   2, 4 }   /* ia32_ULLBIAS */
	};
	static ir_entity *ent_cache[ia32_known_const_max];

	const char    *ent_name, *cnst_str;
	ir_type       *tp;
	ir_entity     *ent;
	tarval        *tv;
	ir_mode       *mode;

	ent_name = names[kct].ent_name;
	if (! ent_cache[kct]) {
		cnst_str = names[kct].cnst_str;

		switch (names[kct].mode) {
		case 0:  mode = mode_Iu; break;
		case 1:  mode = mode_Lu; break;
		default: mode = mode_F;  break;
		}
		tv  = new_tarval_from_str(cnst_str, strlen(cnst_str), mode);
		tp  = ia32_create_float_type(mode, names[kct].align);

		if (kct == ia32_ULLBIAS)
			tp = ia32_create_float_array(tp);
		ent = new_entity(get_glob_type(), new_id_from_str(ent_name), tp);

		set_entity_ld_ident(ent, get_entity_ident(ent));
		add_entity_linkage(ent, IR_LINKAGE_CONSTANT);
		set_entity_visibility(ent, ir_visibility_local);

		if (kct == ia32_ULLBIAS) {
			ir_initializer_t *initializer = create_initializer_compound(2);

			set_initializer_compound_value(initializer, 0,
				create_initializer_tarval(get_mode_null(mode)));
			set_initializer_compound_value(initializer, 1,
				create_initializer_tarval(tv));

			set_entity_initializer(ent, initializer);
		} else {
			set_entity_initializer(ent, create_initializer_tarval(tv));
		}

		/* cache the entry */
		ent_cache[kct] = ent;
	}

	return ent_cache[kct];
}

/**
 * return true if the node is a Proj(Load) and could be used in source address
 * mode for another node. Will return only true if the @p other node is not
 * dependent on the memory of the Load (for binary operations use the other
 * input here, for unary operations use NULL).
 */
static int ia32_use_source_address_mode(ir_node *block, ir_node *node,
                                        ir_node *other, ir_node *other2, match_flags_t flags)
{
	ir_node *load;
	long     pn;

	/* float constants are always available */
	if (is_Const(node)) {
		ir_mode *mode = get_irn_mode(node);
		if (mode_is_float(mode)) {
			if (ia32_cg_config.use_sse2) {
				if (is_simple_sse_Const(node))
					return 0;
			} else {
				if (is_simple_x87_Const(node))
					return 0;
			}
			if (get_irn_n_edges(node) > 1)
				return 0;
			return 1;
		}
	}

	if (!is_Proj(node))
		return 0;
	load = get_Proj_pred(node);
	pn   = get_Proj_proj(node);
	if (!is_Load(load) || pn != pn_Load_res)
		return 0;
	if (get_nodes_block(load) != block)
		return 0;
	/* we only use address mode if we're the only user of the load */
	if (get_irn_n_edges(node) != (flags & match_two_users ? 2 : 1))
		return 0;
	/* in some edge cases with address mode we might reach the load normally
	 * and through some AM sequence, if it is already materialized then we
	 * can't create an AM node from it */
	if (be_is_transformed(node))
		return 0;

	/* don't do AM if other node inputs depend on the load (via mem-proj) */
	if (other != NULL && prevents_AM(block, load, other))
		return 0;

	if (other2 != NULL && prevents_AM(block, load, other2))
		return 0;

	return 1;
}

typedef struct ia32_address_mode_t ia32_address_mode_t;
struct ia32_address_mode_t {
	ia32_address_t  addr;
	ir_mode        *ls_mode;
	ir_node        *mem_proj;
	ir_node        *am_node;
	ia32_op_type_t  op_type;
	ir_node        *new_op1;
	ir_node        *new_op2;
	op_pin_state    pinned;
	unsigned        commutative  : 1;
	unsigned        ins_permuted : 1;
};

static void build_address_ptr(ia32_address_t *addr, ir_node *ptr, ir_node *mem)
{
	/* construct load address */
	memset(addr, 0, sizeof(addr[0]));
	ia32_create_address_mode(addr, ptr, 0);

	addr->base  = addr->base  ? be_transform_node(addr->base)  : noreg_GP;
	addr->index = addr->index ? be_transform_node(addr->index) : noreg_GP;
	addr->mem   = be_transform_node(mem);
}

static void build_address(ia32_address_mode_t *am, ir_node *node,
                          ia32_create_am_flags_t flags)
{
	ia32_address_t *addr = &am->addr;
	ir_node        *load;
	ir_node        *ptr;
	ir_node        *mem;
	ir_node        *new_mem;

	/* floating point immediates */
	if (is_Const(node)) {
		ir_entity *entity  = create_float_const_entity(node);
		addr->base         = get_symconst_base();
		addr->index        = noreg_GP;
		addr->mem          = nomem;
		addr->symconst_ent = entity;
		addr->use_frame    = 1;
		am->ls_mode        = get_type_mode(get_entity_type(entity));
		am->pinned         = op_pin_state_floats;
		return;
	}

	load         = get_Proj_pred(node);
	ptr          = get_Load_ptr(load);
	mem          = get_Load_mem(load);
	new_mem      = be_transform_node(mem);
	am->pinned   = get_irn_pinned(load);
	am->ls_mode  = get_Load_mode(load);
	am->mem_proj = be_get_Proj_for_pn(load, pn_Load_M);
	am->am_node  = node;

	/* construct load address */
	ia32_create_address_mode(addr, ptr, flags);

	addr->base  = addr->base  ? be_transform_node(addr->base)  : noreg_GP;
	addr->index = addr->index ? be_transform_node(addr->index) : noreg_GP;
	addr->mem   = new_mem;
}

static void set_address(ir_node *node, const ia32_address_t *addr)
{
	set_ia32_am_scale(node, addr->scale);
	set_ia32_am_sc(node, addr->symconst_ent);
	set_ia32_am_offs_int(node, addr->offset);
	if (addr->symconst_sign)
		set_ia32_am_sc_sign(node);
	if (addr->use_frame)
		set_ia32_use_frame(node);
	set_ia32_frame_ent(node, addr->frame_entity);
}

/**
 * Apply attributes of a given address mode to a node.
 */
static void set_am_attributes(ir_node *node, const ia32_address_mode_t *am)
{
	set_address(node, &am->addr);

	set_ia32_op_type(node, am->op_type);
	set_ia32_ls_mode(node, am->ls_mode);
	if (am->pinned == op_pin_state_pinned) {
		/* beware: some nodes are already pinned and did not allow to change the state */
		if (get_irn_pinned(node) != op_pin_state_pinned)
			set_irn_pinned(node, op_pin_state_pinned);
	}
	if (am->commutative)
		set_ia32_commutative(node);
}

/**
 * Check, if a given node is a Down-Conv, ie. a integer Conv
 * from a mode with a mode with more bits to a mode with lesser bits.
 * Moreover, we return only true if the node has not more than 1 user.
 *
 * @param node   the node
 * @return non-zero if node is a Down-Conv
 */
static int is_downconv(const ir_node *node)
{
	ir_mode *src_mode;
	ir_mode *dest_mode;

	if (!is_Conv(node))
		return 0;

	/* we only want to skip the conv when we're the only user
	 * (because this test is used in the context of address-mode selection
	 *  and we don't want to use address mode for multiple users) */
	if (get_irn_n_edges(node) > 1)
		return 0;

	src_mode  = get_irn_mode(get_Conv_op(node));
	dest_mode = get_irn_mode(node);
	return
		ia32_mode_needs_gp_reg(src_mode)  &&
		ia32_mode_needs_gp_reg(dest_mode) &&
		get_mode_size_bits(dest_mode) <= get_mode_size_bits(src_mode);
}

/** Skip all Down-Conv's on a given node and return the resulting node. */
ir_node *ia32_skip_downconv(ir_node *node)
{
	while (is_downconv(node))
		node = get_Conv_op(node);

	return node;
}

static bool is_sameconv(ir_node *node)
{
	ir_mode *src_mode;
	ir_mode *dest_mode;

	if (!is_Conv(node))
		return 0;

	/* we only want to skip the conv when we're the only user
	 * (because this test is used in the context of address-mode selection
	 *  and we don't want to use address mode for multiple users) */
	if (get_irn_n_edges(node) > 1)
		return 0;

	src_mode  = get_irn_mode(get_Conv_op(node));
	dest_mode = get_irn_mode(node);
	return
		ia32_mode_needs_gp_reg(src_mode)  &&
		ia32_mode_needs_gp_reg(dest_mode) &&
		get_mode_size_bits(dest_mode) == get_mode_size_bits(src_mode);
}

/** Skip all signedness convs */
static ir_node *ia32_skip_sameconv(ir_node *node)
{
	while (is_sameconv(node))
		node = get_Conv_op(node);

	return node;
}

static ir_node *create_upconv(ir_node *node, ir_node *orig_node)
{
	ir_mode  *mode = get_irn_mode(node);
	ir_node  *block;
	ir_mode  *tgt_mode;
	dbg_info *dbgi;

	if (mode_is_signed(mode)) {
		tgt_mode = mode_Is;
	} else {
		tgt_mode = mode_Iu;
	}
	block = get_nodes_block(node);
	dbgi  = get_irn_dbg_info(node);

	return create_I2I_Conv(mode, tgt_mode, dbgi, block, node, orig_node);
}

/**
 * matches operands of a node into ia32 addressing/operand modes. This covers
 * usage of source address mode, immediates, operations with non 32-bit modes,
 * ...
 * The resulting data is filled into the @p am struct. block is the block
 * of the node whose arguments are matched. op1, op2 are the first and second
 * input that are matched (op1 may be NULL). other_op is another unrelated
 * input that is not matched! but which is needed sometimes to check if AM
 * for op1/op2 is legal.
 * @p flags describes the supported modes of the operation in detail.
 */
static void match_arguments(ia32_address_mode_t *am, ir_node *block,
                            ir_node *op1, ir_node *op2, ir_node *other_op,
                            match_flags_t flags)
{
	ia32_address_t *addr      = &am->addr;
	ir_mode        *mode      = get_irn_mode(op2);
	int             mode_bits = get_mode_size_bits(mode);
	ir_node        *new_op1, *new_op2;
	int             use_am;
	unsigned        commutative;
	int             use_am_and_immediates;
	int             use_immediate;

	memset(am, 0, sizeof(am[0]));

	commutative           = (flags & match_commutative) != 0;
	use_am_and_immediates = (flags & match_am_and_immediates) != 0;
	use_am                = (flags & match_am) != 0;
	use_immediate         = (flags & match_immediate) != 0;
	assert(!use_am_and_immediates || use_immediate);

	assert(op2 != NULL);
	assert(!commutative || op1 != NULL);
	assert(use_am || !(flags & match_8bit_am));
	assert(use_am || !(flags & match_16bit_am));

	if ((mode_bits ==  8 && !(flags & match_8bit_am)) ||
	    (mode_bits == 16 && !(flags & match_16bit_am))) {
		use_am = 0;
	}

	/* we can simply skip downconvs for mode neutral nodes: the upper bits
	 * can be random for these operations */
	if (flags & match_mode_neutral) {
		op2 = ia32_skip_downconv(op2);
		if (op1 != NULL) {
			op1 = ia32_skip_downconv(op1);
		}
	} else {
		op2 = ia32_skip_sameconv(op2);
		if (op1 != NULL) {
			op1 = ia32_skip_sameconv(op1);
		}
	}

	/* match immediates. firm nodes are normalized: constants are always on the
	 * op2 input */
	new_op2 = NULL;
	if (!(flags & match_try_am) && use_immediate) {
		new_op2 = try_create_Immediate(op2, 0);
	}

	if (new_op2 == NULL &&
	    use_am && ia32_use_source_address_mode(block, op2, op1, other_op, flags)) {
		build_address(am, op2, 0);
		new_op1     = (op1 == NULL ? NULL : be_transform_node(op1));
		if (mode_is_float(mode)) {
			new_op2 = ia32_new_NoReg_vfp(env_cg);
		} else {
			new_op2 = noreg_GP;
		}
		am->op_type = ia32_AddrModeS;
	} else if (commutative && (new_op2 == NULL || use_am_and_immediates) &&
		       use_am &&
		       ia32_use_source_address_mode(block, op1, op2, other_op, flags)) {
		ir_node *noreg;
		build_address(am, op1, 0);

		if (mode_is_float(mode)) {
			noreg = ia32_new_NoReg_vfp(env_cg);
		} else {
			noreg = noreg_GP;
		}

		if (new_op2 != NULL) {
			new_op1 = noreg;
		} else {
			new_op1 = be_transform_node(op2);
			new_op2 = noreg;
			am->ins_permuted = 1;
		}
		am->op_type = ia32_AddrModeS;
	} else {
		ir_mode *mode;
		am->op_type = ia32_Normal;

		if (flags & match_try_am) {
			am->new_op1 = NULL;
			am->new_op2 = NULL;
			return;
		}

		mode = get_irn_mode(op2);
		if (flags & match_upconv_32 && get_mode_size_bits(mode) != 32) {
			new_op1 = (op1 == NULL ? NULL : create_upconv(op1, NULL));
			if (new_op2 == NULL)
				new_op2 = create_upconv(op2, NULL);
			am->ls_mode = mode_Iu;
		} else {
			new_op1 = (op1 == NULL ? NULL : be_transform_node(op1));
			if (new_op2 == NULL)
				new_op2 = be_transform_node(op2);
			am->ls_mode = (flags & match_mode_neutral) ? mode_Iu : mode;
		}
	}
	if (addr->base == NULL)
		addr->base = noreg_GP;
	if (addr->index == NULL)
		addr->index = noreg_GP;
	if (addr->mem == NULL)
		addr->mem = nomem;

	am->new_op1     = new_op1;
	am->new_op2     = new_op2;
	am->commutative = commutative;
}

/**
 * "Fixes" a node that uses address mode by turning it into mode_T
 * and returning a pn_ia32_res Proj.
 *
 * @param node  the node
 * @param am    its address mode
 *
 * @return a Proj(pn_ia32_res) if a memory address mode is used,
 *         node else
 */
static ir_node *fix_mem_proj(ir_node *node, ia32_address_mode_t *am)
{
	ir_mode  *mode;
	ir_node  *load;

	if (am->mem_proj == NULL)
		return node;

	/* we have to create a mode_T so the old MemProj can attach to us */
	mode = get_irn_mode(node);
	load = get_Proj_pred(am->mem_proj);

	be_set_transformed_node(load, node);

	if (mode != mode_T) {
		set_irn_mode(node, mode_T);
		return new_rd_Proj(NULL, node, mode, pn_ia32_res);
	} else {
		return node;
	}
}

/**
 * Construct a standard binary operation, set AM and immediate if required.
 *
 * @param node  The original node for which the binop is created
 * @param op1   The first operand
 * @param op2   The second operand
 * @param func  The node constructor function
 * @return The constructed ia32 node.
 */
static ir_node *gen_binop(ir_node *node, ir_node *op1, ir_node *op2,
                          construct_binop_func *func, match_flags_t flags)
{
	dbg_info            *dbgi;
	ir_node             *block, *new_block, *new_node;
	ia32_address_mode_t  am;
	ia32_address_t      *addr = &am.addr;

	block = get_nodes_block(node);
	match_arguments(&am, block, op1, op2, NULL, flags);

	dbgi      = get_irn_dbg_info(node);
	new_block = be_transform_node(block);
	new_node  = func(dbgi, new_block, addr->base, addr->index, addr->mem,
			am.new_op1, am.new_op2);
	set_am_attributes(new_node, &am);
	/* we can't use source address mode anymore when using immediates */
	if (!(flags & match_am_and_immediates) &&
	    (is_ia32_Immediate(am.new_op1) || is_ia32_Immediate(am.new_op2)))
		set_ia32_am_support(new_node, ia32_am_none);
	SET_IA32_ORIG_NODE(new_node, node);

	new_node = fix_mem_proj(new_node, &am);

	return new_node;
}

/**
 * Generic names for the inputs of an ia32 binary op.
 */
enum {
	n_ia32_l_binop_left,  /**< ia32 left input */
	n_ia32_l_binop_right, /**< ia32 right input */
	n_ia32_l_binop_eflags /**< ia32 eflags input */
};
COMPILETIME_ASSERT(n_ia32_l_binop_left   == n_ia32_l_Adc_left,       n_Adc_left)
COMPILETIME_ASSERT(n_ia32_l_binop_right  == n_ia32_l_Adc_right,      n_Adc_right)
COMPILETIME_ASSERT(n_ia32_l_binop_eflags == n_ia32_l_Adc_eflags,     n_Adc_eflags)
COMPILETIME_ASSERT(n_ia32_l_binop_left   == n_ia32_l_Sbb_minuend,    n_Sbb_minuend)
COMPILETIME_ASSERT(n_ia32_l_binop_right  == n_ia32_l_Sbb_subtrahend, n_Sbb_subtrahend)
COMPILETIME_ASSERT(n_ia32_l_binop_eflags == n_ia32_l_Sbb_eflags,     n_Sbb_eflags)

/**
 * Construct a binary operation which also consumes the eflags.
 *
 * @param node  The node to transform
 * @param func  The node constructor function
 * @param flags The match flags
 * @return      The constructor ia32 node
 */
static ir_node *gen_binop_flags(ir_node *node, construct_binop_flags_func *func,
                                match_flags_t flags)
{
	ir_node             *src_block  = get_nodes_block(node);
	ir_node             *op1        = get_irn_n(node, n_ia32_l_binop_left);
	ir_node             *op2        = get_irn_n(node, n_ia32_l_binop_right);
	ir_node             *eflags     = get_irn_n(node, n_ia32_l_binop_eflags);
	dbg_info            *dbgi;
	ir_node             *block, *new_node, *new_eflags;
	ia32_address_mode_t  am;
	ia32_address_t      *addr       = &am.addr;

	match_arguments(&am, src_block, op1, op2, eflags, flags);

	dbgi       = get_irn_dbg_info(node);
	block      = be_transform_node(src_block);
	new_eflags = be_transform_node(eflags);
	new_node   = func(dbgi, block, addr->base, addr->index, addr->mem,
	                  am.new_op1, am.new_op2, new_eflags);
	set_am_attributes(new_node, &am);
	/* we can't use source address mode anymore when using immediates */
	if (!(flags & match_am_and_immediates) &&
	    (is_ia32_Immediate(am.new_op1) || is_ia32_Immediate(am.new_op2)))
		set_ia32_am_support(new_node, ia32_am_none);
	SET_IA32_ORIG_NODE(new_node, node);

	new_node = fix_mem_proj(new_node, &am);

	return new_node;
}

static ir_node *get_fpcw(void)
{
	ir_node *fpcw;
	if (initial_fpcw != NULL)
		return initial_fpcw;

	fpcw         = be_abi_get_ignore_irn(env_cg->birg->abi,
	                                     &ia32_fp_cw_regs[REG_FPCW]);
	initial_fpcw = be_transform_node(fpcw);

	return initial_fpcw;
}

/**
 * Construct a standard binary operation, set AM and immediate if required.
 *
 * @param op1   The first operand
 * @param op2   The second operand
 * @param func  The node constructor function
 * @return The constructed ia32 node.
 */
static ir_node *gen_binop_x87_float(ir_node *node, ir_node *op1, ir_node *op2,
                                    construct_binop_float_func *func)
{
	ir_mode             *mode = get_irn_mode(node);
	dbg_info            *dbgi;
	ir_node             *block, *new_block, *new_node;
	ia32_address_mode_t  am;
	ia32_address_t      *addr = &am.addr;
	ia32_x87_attr_t     *attr;
	/* All operations are considered commutative, because there are reverse
	 * variants */
	match_flags_t        flags = match_commutative;

	/* happens for div nodes... */
	if (mode == mode_T)
		mode = get_divop_resmod(node);

	/* cannot use address mode with long double on x87 */
	if (get_mode_size_bits(mode) <= 64)
		flags |= match_am;

	block = get_nodes_block(node);
	match_arguments(&am, block, op1, op2, NULL, flags);

	dbgi      = get_irn_dbg_info(node);
	new_block = be_transform_node(block);
	new_node  = func(dbgi, new_block, addr->base, addr->index, addr->mem,
	                 am.new_op1, am.new_op2, get_fpcw());
	set_am_attributes(new_node, &am);

	attr = get_ia32_x87_attr(new_node);
	attr->attr.data.ins_permuted = am.ins_permuted;

	SET_IA32_ORIG_NODE(new_node, node);

	new_node = fix_mem_proj(new_node, &am);

	return new_node;
}

/**
 * Construct a shift/rotate binary operation, sets AM and immediate if required.
 *
 * @param op1   The first operand
 * @param op2   The second operand
 * @param func  The node constructor function
 * @return The constructed ia32 node.
 */
static ir_node *gen_shift_binop(ir_node *node, ir_node *op1, ir_node *op2,
                                construct_shift_func *func,
                                match_flags_t flags)
{
	dbg_info *dbgi;
	ir_node  *block, *new_block, *new_op1, *new_op2, *new_node;

	assert(! mode_is_float(get_irn_mode(node)));
	assert(flags & match_immediate);
	assert((flags & ~(match_mode_neutral | match_immediate)) == 0);

	if (flags & match_mode_neutral) {
		op1     = ia32_skip_downconv(op1);
		new_op1 = be_transform_node(op1);
	} else if (get_mode_size_bits(get_irn_mode(node)) != 32) {
		new_op1 = create_upconv(op1, node);
	} else {
		new_op1 = be_transform_node(op1);
	}

	/* the shift amount can be any mode that is bigger than 5 bits, since all
	 * other bits are ignored anyway */
	while (is_Conv(op2) && get_irn_n_edges(op2) == 1) {
		ir_node *const op = get_Conv_op(op2);
		if (mode_is_float(get_irn_mode(op)))
			break;
		op2 = op;
		assert(get_mode_size_bits(get_irn_mode(op2)) >= 5);
	}
	new_op2 = create_immediate_or_transform(op2, 0);

	dbgi      = get_irn_dbg_info(node);
	block     = get_nodes_block(node);
	new_block = be_transform_node(block);
	new_node  = func(dbgi, new_block, new_op1, new_op2);
	SET_IA32_ORIG_NODE(new_node, node);

	/* lowered shift instruction may have a dependency operand, handle it here */
	if (get_irn_arity(node) == 3) {
		/* we have a dependency */
		ir_node *new_dep = be_transform_node(get_irn_n(node, 2));
		add_irn_dep(new_node, new_dep);
	}

	return new_node;
}


/**
 * Construct a standard unary operation, set AM and immediate if required.
 *
 * @param op    The operand
 * @param func  The node constructor function
 * @return The constructed ia32 node.
 */
static ir_node *gen_unop(ir_node *node, ir_node *op, construct_unop_func *func,
                         match_flags_t flags)
{
	dbg_info *dbgi;
	ir_node  *block, *new_block, *new_op, *new_node;

	assert(flags == 0 || flags == match_mode_neutral);
	if (flags & match_mode_neutral) {
		op = ia32_skip_downconv(op);
	}

	new_op    = be_transform_node(op);
	dbgi      = get_irn_dbg_info(node);
	block     = get_nodes_block(node);
	new_block = be_transform_node(block);
	new_node  = func(dbgi, new_block, new_op);

	SET_IA32_ORIG_NODE(new_node, node);

	return new_node;
}

static ir_node *create_lea_from_address(dbg_info *dbgi,	ir_node *block,
                                        ia32_address_t *addr)
{
	ir_node *base, *index, *res;

	base = addr->base;
	if (base == NULL) {
		base = noreg_GP;
	} else {
		base = be_transform_node(base);
	}

	index = addr->index;
	if (index == NULL) {
		index = noreg_GP;
	} else {
		index = be_transform_node(index);
	}

	res = new_bd_ia32_Lea(dbgi, block, base, index);
	set_address(res, addr);

	return res;
}

/**
 * Returns non-zero if a given address mode has a symbolic or
 * numerical offset != 0.
 */
static int am_has_immediates(const ia32_address_t *addr)
{
	return addr->offset != 0 || addr->symconst_ent != NULL
		|| addr->frame_entity || addr->use_frame;
}

/**
 * Creates an ia32 Add.
 *
 * @return the created ia32 Add node
 */
static ir_node *gen_Add(ir_node *node)
{
	ir_mode  *mode = get_irn_mode(node);
	ir_node  *op1  = get_Add_left(node);
	ir_node  *op2  = get_Add_right(node);
	dbg_info *dbgi;
	ir_node  *block, *new_block, *new_node, *add_immediate_op;
	ia32_address_t       addr;
	ia32_address_mode_t  am;

	if (mode_is_float(mode)) {
		if (ia32_cg_config.use_sse2)
			return gen_binop(node, op1, op2, new_bd_ia32_xAdd,
			                 match_commutative | match_am);
		else
			return gen_binop_x87_float(node, op1, op2, new_bd_ia32_vfadd);
	}

	ia32_mark_non_am(node);

	op2 = ia32_skip_downconv(op2);
	op1 = ia32_skip_downconv(op1);

	/**
	 * Rules for an Add:
	 *   0. Immediate Trees (example Add(Symconst, Const) -> Const)
	 *   1. Add with immediate -> Lea
	 *   2. Add with possible source address mode -> Add
	 *   3. Otherwise -> Lea
	 */
	memset(&addr, 0, sizeof(addr));
	ia32_create_address_mode(&addr, node, ia32_create_am_force);
	add_immediate_op = NULL;

	dbgi      = get_irn_dbg_info(node);
	block     = get_nodes_block(node);
	new_block = be_transform_node(block);

	/* a constant? */
	if (addr.base == NULL && addr.index == NULL) {
		new_node = new_bd_ia32_Const(dbgi, new_block, addr.symconst_ent,
		                             addr.symconst_sign, 0, addr.offset);
		be_dep_on_frame(new_node);
		SET_IA32_ORIG_NODE(new_node, node);
		return new_node;
	}
	/* add with immediate? */
	if (addr.index == NULL) {
		add_immediate_op = addr.base;
	} else if (addr.base == NULL && addr.scale == 0) {
		add_immediate_op = addr.index;
	}

	if (add_immediate_op != NULL) {
		if (!am_has_immediates(&addr)) {
#ifdef DEBUG_libfirm
			ir_fprintf(stderr, "Optimisation warning Add x,0 (%+F) found\n",
					   node);
#endif
			return be_transform_node(add_immediate_op);
		}

		new_node = create_lea_from_address(dbgi, new_block, &addr);
		SET_IA32_ORIG_NODE(new_node, node);
		return new_node;
	}

	/* test if we can use source address mode */
	match_arguments(&am, block, op1, op2, NULL, match_commutative
			| match_mode_neutral | match_am | match_immediate | match_try_am);

	/* construct an Add with source address mode */
	if (am.op_type == ia32_AddrModeS) {
		ia32_address_t *am_addr = &am.addr;
		new_node = new_bd_ia32_Add(dbgi, new_block, am_addr->base,
		                         am_addr->index, am_addr->mem, am.new_op1,
		                         am.new_op2);
		set_am_attributes(new_node, &am);
		SET_IA32_ORIG_NODE(new_node, node);

		new_node = fix_mem_proj(new_node, &am);

		return new_node;
	}

	/* otherwise construct a lea */
	new_node = create_lea_from_address(dbgi, new_block, &addr);
	SET_IA32_ORIG_NODE(new_node, node);
	return new_node;
}

/**
 * Creates an ia32 Mul.
 *
 * @return the created ia32 Mul node
 */
static ir_node *gen_Mul(ir_node *node)
{
	ir_node *op1  = get_Mul_left(node);
	ir_node *op2  = get_Mul_right(node);
	ir_mode *mode = get_irn_mode(node);

	if (mode_is_float(mode)) {
		if (ia32_cg_config.use_sse2)
			return gen_binop(node, op1, op2, new_bd_ia32_xMul,
			                 match_commutative | match_am);
		else
			return gen_binop_x87_float(node, op1, op2, new_bd_ia32_vfmul);
	}
	return gen_binop(node, op1, op2, new_bd_ia32_IMul,
	                 match_commutative | match_am | match_mode_neutral |
	                 match_immediate | match_am_and_immediates);
}

/**
 * Creates an ia32 Mulh.
 * Note: Mul produces a 64Bit result and Mulh returns the upper 32 bit of
 * this result while Mul returns the lower 32 bit.
 *
 * @return the created ia32 Mulh node
 */
static ir_node *gen_Mulh(ir_node *node)
{
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *op1       = get_Mulh_left(node);
	ir_node  *op2       = get_Mulh_right(node);
	ir_mode  *mode      = get_irn_mode(node);
	ir_node  *new_node;
	ir_node  *proj_res_high;

	if (get_mode_size_bits(mode) != 32) {
		panic("Mulh without 32bit size not supported in ia32 backend (%+F)", node);
	}

	if (mode_is_signed(mode)) {
		new_node = gen_binop(node, op1, op2, new_bd_ia32_IMul1OP, match_commutative | match_am);
		proj_res_high = new_rd_Proj(dbgi, new_node, mode_Iu, pn_ia32_IMul1OP_res_high);
	} else {
		new_node = gen_binop(node, op1, op2, new_bd_ia32_Mul, match_commutative | match_am);
		proj_res_high = new_rd_Proj(dbgi, new_node, mode_Iu, pn_ia32_Mul_res_high);
	}
	return proj_res_high;
}

/**
 * Creates an ia32 And.
 *
 * @return The created ia32 And node
 */
static ir_node *gen_And(ir_node *node)
{
	ir_node *op1 = get_And_left(node);
	ir_node *op2 = get_And_right(node);
	assert(! mode_is_float(get_irn_mode(node)));

	/* is it a zero extension? */
	if (is_Const(op2)) {
		tarval   *tv    = get_Const_tarval(op2);
		long      v     = get_tarval_long(tv);

		if (v == 0xFF || v == 0xFFFF) {
			dbg_info *dbgi   = get_irn_dbg_info(node);
			ir_node  *block  = get_nodes_block(node);
			ir_mode  *src_mode;
			ir_node  *res;

			if (v == 0xFF) {
				src_mode = mode_Bu;
			} else {
				assert(v == 0xFFFF);
				src_mode = mode_Hu;
			}
			res = create_I2I_Conv(src_mode, mode_Iu, dbgi, block, op1, node);

			return res;
		}
	}
	return gen_binop(node, op1, op2, new_bd_ia32_And,
			match_commutative | match_mode_neutral | match_am | match_immediate);
}



/**
 * Creates an ia32 Or.
 *
 * @return The created ia32 Or node
 */
static ir_node *gen_Or(ir_node *node)
{
	ir_node *op1 = get_Or_left(node);
	ir_node *op2 = get_Or_right(node);

	assert (! mode_is_float(get_irn_mode(node)));
	return gen_binop(node, op1, op2, new_bd_ia32_Or, match_commutative
			| match_mode_neutral | match_am | match_immediate);
}



/**
 * Creates an ia32 Eor.
 *
 * @return The created ia32 Eor node
 */
static ir_node *gen_Eor(ir_node *node)
{
	ir_node *op1 = get_Eor_left(node);
	ir_node *op2 = get_Eor_right(node);

	assert(! mode_is_float(get_irn_mode(node)));
	return gen_binop(node, op1, op2, new_bd_ia32_Xor, match_commutative
			| match_mode_neutral | match_am | match_immediate);
}


/**
 * Creates an ia32 Sub.
 *
 * @return The created ia32 Sub node
 */
static ir_node *gen_Sub(ir_node *node)
{
	ir_node  *op1  = get_Sub_left(node);
	ir_node  *op2  = get_Sub_right(node);
	ir_mode  *mode = get_irn_mode(node);

	if (mode_is_float(mode)) {
		if (ia32_cg_config.use_sse2)
			return gen_binop(node, op1, op2, new_bd_ia32_xSub, match_am);
		else
			return gen_binop_x87_float(node, op1, op2, new_bd_ia32_vfsub);
	}

	if (is_Const(op2)) {
		ir_fprintf(stderr, "Optimisation warning: found sub with const (%+F)\n",
		           node);
	}

	return gen_binop(node, op1, op2, new_bd_ia32_Sub, match_mode_neutral
			| match_am | match_immediate);
}

static ir_node *transform_AM_mem(ir_node *const block,
                                 ir_node  *const src_val,
                                 ir_node  *const src_mem,
                                 ir_node  *const am_mem)
{
	if (is_NoMem(am_mem)) {
		return be_transform_node(src_mem);
	} else if (is_Proj(src_val) &&
	           is_Proj(src_mem) &&
	           get_Proj_pred(src_val) == get_Proj_pred(src_mem)) {
		/* avoid memory loop */
		return am_mem;
	} else if (is_Proj(src_val) && is_Sync(src_mem)) {
		ir_node  *const ptr_pred = get_Proj_pred(src_val);
		int       const arity    = get_Sync_n_preds(src_mem);
		int             n        = 0;
		ir_node **      ins;
		int             i;

		NEW_ARR_A(ir_node*, ins, arity + 1);

		/* NOTE: This sometimes produces dead-code because the old sync in
		 * src_mem might not be used anymore, we should detect this case
		 * and kill the sync... */
		for (i = arity - 1; i >= 0; --i) {
			ir_node *const pred = get_Sync_pred(src_mem, i);

			/* avoid memory loop */
			if (is_Proj(pred) && get_Proj_pred(pred) == ptr_pred)
				continue;

			ins[n++] = be_transform_node(pred);
		}

		ins[n++] = am_mem;

		return new_r_Sync(block, n, ins);
	} else {
		ir_node *ins[2];

		ins[0] = be_transform_node(src_mem);
		ins[1] = am_mem;
		return new_r_Sync(block, 2, ins);
	}
}

/**
 * Create a 32bit to 64bit signed extension.
 *
 * @param dbgi   debug info
 * @param block  the block where node nodes should be placed
 * @param val    the value to extend
 * @param orig   the original node
 */
static ir_node *create_sex_32_64(dbg_info *dbgi, ir_node *block,
                                 ir_node *val, const ir_node *orig)
{
	ir_node *res;

	(void)orig;
	if (ia32_cg_config.use_short_sex_eax) {
		ir_node *pval = new_bd_ia32_ProduceVal(dbgi, block);
		be_dep_on_frame(pval);
		res = new_bd_ia32_Cltd(dbgi, block, val, pval);
	} else {
		ir_node *imm31 = ia32_create_Immediate(NULL, 0, 31);
		res = new_bd_ia32_Sar(dbgi, block, val, imm31);
	}
	SET_IA32_ORIG_NODE(res, orig);
	return res;
}

/**
 * Generates an ia32 DivMod with additional infrastructure for the
 * register allocator if needed.
 */
static ir_node *create_Div(ir_node *node)
{
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	ir_node  *mem;
	ir_node  *new_mem;
	ir_node  *op1;
	ir_node  *op2;
	ir_node  *new_node;
	ir_mode  *mode;
	ir_node  *sign_extension;
	ia32_address_mode_t  am;
	ia32_address_t      *addr = &am.addr;

	/* the upper bits have random contents for smaller modes */
	switch (get_irn_opcode(node)) {
	case iro_Div:
		op1     = get_Div_left(node);
		op2     = get_Div_right(node);
		mem     = get_Div_mem(node);
		mode    = get_Div_resmode(node);
		break;
	case iro_Mod:
		op1     = get_Mod_left(node);
		op2     = get_Mod_right(node);
		mem     = get_Mod_mem(node);
		mode    = get_Mod_resmode(node);
		break;
	case iro_DivMod:
		op1     = get_DivMod_left(node);
		op2     = get_DivMod_right(node);
		mem     = get_DivMod_mem(node);
		mode    = get_DivMod_resmode(node);
		break;
	default:
		panic("invalid divmod node %+F", node);
	}

	match_arguments(&am, block, op1, op2, NULL, match_am | match_upconv_32);

	/* Beware: We don't need a Sync, if the memory predecessor of the Div node
	   is the memory of the consumed address. We can have only the second op as address
	   in Div nodes, so check only op2. */
	new_mem = transform_AM_mem(block, op2, mem, addr->mem);

	if (mode_is_signed(mode)) {
		sign_extension = create_sex_32_64(dbgi, new_block, am.new_op1, node);
		new_node       = new_bd_ia32_IDiv(dbgi, new_block, addr->base,
				addr->index, new_mem, am.new_op2, am.new_op1, sign_extension);
	} else {
		sign_extension = new_bd_ia32_Const(dbgi, new_block, NULL, 0, 0, 0);
		be_dep_on_frame(sign_extension);

		new_node = new_bd_ia32_Div(dbgi, new_block, addr->base,
		                           addr->index, new_mem, am.new_op2,
		                           am.new_op1, sign_extension);
	}

	set_irn_pinned(new_node, get_irn_pinned(node));

	set_am_attributes(new_node, &am);
	SET_IA32_ORIG_NODE(new_node, node);

	new_node = fix_mem_proj(new_node, &am);

	return new_node;
}

/**
 * Generates an ia32 Mod.
 */
static ir_node *gen_Mod(ir_node *node)
{
	return create_Div(node);
}

/**
 * Generates an ia32 Div.
 */
static ir_node *gen_Div(ir_node *node)
{
	return create_Div(node);
}

/**
 * Generates an ia32 DivMod.
 */
static ir_node *gen_DivMod(ir_node *node)
{
	return create_Div(node);
}



/**
 * Creates an ia32 floating Div.
 *
 * @return The created ia32 xDiv node
 */
static ir_node *gen_Quot(ir_node *node)
{
	ir_node *op1 = get_Quot_left(node);
	ir_node *op2 = get_Quot_right(node);

	if (ia32_cg_config.use_sse2) {
		return gen_binop(node, op1, op2, new_bd_ia32_xDiv, match_am);
	} else {
		return gen_binop_x87_float(node, op1, op2, new_bd_ia32_vfdiv);
	}
}


/**
 * Creates an ia32 Shl.
 *
 * @return The created ia32 Shl node
 */
static ir_node *gen_Shl(ir_node *node)
{
	ir_node *left  = get_Shl_left(node);
	ir_node *right = get_Shl_right(node);

	return gen_shift_binop(node, left, right, new_bd_ia32_Shl,
	                       match_mode_neutral | match_immediate);
}

/**
 * Creates an ia32 Shr.
 *
 * @return The created ia32 Shr node
 */
static ir_node *gen_Shr(ir_node *node)
{
	ir_node *left  = get_Shr_left(node);
	ir_node *right = get_Shr_right(node);

	return gen_shift_binop(node, left, right, new_bd_ia32_Shr, match_immediate);
}



/**
 * Creates an ia32 Sar.
 *
 * @return The created ia32 Shrs node
 */
static ir_node *gen_Shrs(ir_node *node)
{
	ir_node *left  = get_Shrs_left(node);
	ir_node *right = get_Shrs_right(node);

	if (is_Const(right)) {
		tarval *tv = get_Const_tarval(right);
		long val = get_tarval_long(tv);
		if (val == 31) {
			/* this is a sign extension */
			dbg_info *dbgi   = get_irn_dbg_info(node);
			ir_node  *block  = be_transform_node(get_nodes_block(node));
			ir_node  *new_op = be_transform_node(left);

			return create_sex_32_64(dbgi, block, new_op, node);
		}
	}

	/* 8 or 16 bit sign extension? */
	if (is_Const(right) && is_Shl(left)) {
		ir_node *shl_left  = get_Shl_left(left);
		ir_node *shl_right = get_Shl_right(left);
		if (is_Const(shl_right)) {
			tarval *tv1 = get_Const_tarval(right);
			tarval *tv2 = get_Const_tarval(shl_right);
			if (tv1 == tv2 && tarval_is_long(tv1)) {
				long val = get_tarval_long(tv1);
				if (val == 16 || val == 24) {
					dbg_info *dbgi   = get_irn_dbg_info(node);
					ir_node  *block  = get_nodes_block(node);
					ir_mode  *src_mode;
					ir_node  *res;

					if (val == 24) {
						src_mode = mode_Bs;
					} else {
						assert(val == 16);
						src_mode = mode_Hs;
					}
					res = create_I2I_Conv(src_mode, mode_Is, dbgi, block,
					                      shl_left, node);

					return res;
				}
			}
		}
	}

	return gen_shift_binop(node, left, right, new_bd_ia32_Sar, match_immediate);
}



/**
 * Creates an ia32 Rol.
 *
 * @param op1   The first operator
 * @param op2   The second operator
 * @return The created ia32 RotL node
 */
static ir_node *gen_Rol(ir_node *node, ir_node *op1, ir_node *op2)
{
	return gen_shift_binop(node, op1, op2, new_bd_ia32_Rol, match_immediate);
}



/**
 * Creates an ia32 Ror.
 * NOTE: There is no RotR with immediate because this would always be a RotL
 *       "imm-mode_size_bits" which can be pre-calculated.
 *
 * @param op1   The first operator
 * @param op2   The second operator
 * @return The created ia32 RotR node
 */
static ir_node *gen_Ror(ir_node *node, ir_node *op1, ir_node *op2)
{
	return gen_shift_binop(node, op1, op2, new_bd_ia32_Ror, match_immediate);
}



/**
 * Creates an ia32 RotR or RotL (depending on the found pattern).
 *
 * @return The created ia32 RotL or RotR node
 */
static ir_node *gen_Rotl(ir_node *node)
{
	ir_node *rotate = NULL;
	ir_node *op1    = get_Rotl_left(node);
	ir_node *op2    = get_Rotl_right(node);

	/* Firm has only RotL, so we are looking for a right (op2)
		 operand "-e+mode_size_bits" (it's an already modified "mode_size_bits-e",
		 that means we can create a RotR instead of an Add and a RotL */

	if (is_Add(op2)) {
		ir_node *add = op2;
		ir_node *left = get_Add_left(add);
		ir_node *right = get_Add_right(add);
		if (is_Const(right)) {
			tarval  *tv   = get_Const_tarval(right);
			ir_mode *mode = get_irn_mode(node);
			long     bits = get_mode_size_bits(mode);

			if (is_Minus(left) &&
			    tarval_is_long(tv)       &&
			    get_tarval_long(tv) == bits &&
			    bits                == 32)
			{
				DB((dbg, LEVEL_1, "RotL into RotR ... "));
				rotate = gen_Ror(node, op1, get_Minus_op(left));
			}
		}
	}

	if (rotate == NULL) {
		rotate = gen_Rol(node, op1, op2);
	}

	return rotate;
}



/**
 * Transforms a Minus node.
 *
 * @return The created ia32 Minus node
 */
static ir_node *gen_Minus(ir_node *node)
{
	ir_node   *op    = get_Minus_op(node);
	ir_node   *block = be_transform_node(get_nodes_block(node));
	dbg_info  *dbgi  = get_irn_dbg_info(node);
	ir_mode   *mode  = get_irn_mode(node);
	ir_entity *ent;
	ir_node   *new_node;
	int        size;

	if (mode_is_float(mode)) {
		ir_node *new_op = be_transform_node(op);
		if (ia32_cg_config.use_sse2) {
			/* TODO: non-optimal... if we have many xXors, then we should
			 * rather create a load for the const and use that instead of
			 * several AM nodes... */
			ir_node *noreg_xmm = ia32_new_NoReg_xmm(env_cg);

			new_node = new_bd_ia32_xXor(dbgi, block, get_symconst_base(),
			                            noreg_GP, nomem, new_op, noreg_xmm);

			size = get_mode_size_bits(mode);
			ent  = ia32_gen_fp_known_const(size == 32 ? ia32_SSIGN : ia32_DSIGN);

			set_ia32_am_sc(new_node, ent);
			set_ia32_op_type(new_node, ia32_AddrModeS);
			set_ia32_ls_mode(new_node, mode);
		} else {
			new_node = new_bd_ia32_vfchs(dbgi, block, new_op);
		}
	} else {
		new_node = gen_unop(node, op, new_bd_ia32_Neg, match_mode_neutral);
	}

	SET_IA32_ORIG_NODE(new_node, node);

	return new_node;
}

/**
 * Transforms a Not node.
 *
 * @return The created ia32 Not node
 */
static ir_node *gen_Not(ir_node *node)
{
	ir_node *op = get_Not_op(node);

	assert(get_irn_mode(node) != mode_b); /* should be lowered already */
	assert (! mode_is_float(get_irn_mode(node)));

	return gen_unop(node, op, new_bd_ia32_Not, match_mode_neutral);
}



/**
 * Transforms an Abs node.
 *
 * @return The created ia32 Abs node
 */
static ir_node *gen_Abs(ir_node *node)
{
	ir_node   *block     = get_nodes_block(node);
	ir_node   *new_block = be_transform_node(block);
	ir_node   *op        = get_Abs_op(node);
	dbg_info  *dbgi      = get_irn_dbg_info(node);
	ir_mode   *mode      = get_irn_mode(node);
	ir_node   *new_op;
	ir_node   *new_node;
	int        size;
	ir_entity *ent;

	if (mode_is_float(mode)) {
		new_op = be_transform_node(op);

		if (ia32_cg_config.use_sse2) {
			ir_node *noreg_fp = ia32_new_NoReg_xmm(env_cg);
			new_node = new_bd_ia32_xAnd(dbgi, new_block, get_symconst_base(),
			                            noreg_GP, nomem, new_op, noreg_fp);

			size = get_mode_size_bits(mode);
			ent  = ia32_gen_fp_known_const(size == 32 ? ia32_SABS : ia32_DABS);

			set_ia32_am_sc(new_node, ent);

			SET_IA32_ORIG_NODE(new_node, node);

			set_ia32_op_type(new_node, ia32_AddrModeS);
			set_ia32_ls_mode(new_node, mode);
		} else {
			new_node = new_bd_ia32_vfabs(dbgi, new_block, new_op);
			SET_IA32_ORIG_NODE(new_node, node);
		}
	} else {
		ir_node *xor, *sign_extension;

		if (get_mode_size_bits(mode) == 32) {
			new_op = be_transform_node(op);
		} else {
			new_op = create_I2I_Conv(mode, mode_Is, dbgi, block, op, node);
		}

		sign_extension = create_sex_32_64(dbgi, new_block, new_op, node);

		xor = new_bd_ia32_Xor(dbgi, new_block, noreg_GP, noreg_GP,
		                      nomem, new_op, sign_extension);
		SET_IA32_ORIG_NODE(xor, node);

		new_node = new_bd_ia32_Sub(dbgi, new_block, noreg_GP, noreg_GP,
		                           nomem, xor, sign_extension);
		SET_IA32_ORIG_NODE(new_node, node);
	}

	return new_node;
}

/**
 * Create a bt instruction for x & (1 << n) and place it into the block of cmp.
 */
static ir_node *gen_bt(ir_node *cmp, ir_node *x, ir_node *n)
{
	dbg_info *dbgi      = get_irn_dbg_info(cmp);
	ir_node  *block     = get_nodes_block(cmp);
	ir_node  *new_block = be_transform_node(block);
	ir_node  *op1       = be_transform_node(x);
	ir_node  *op2       = be_transform_node(n);

	return new_bd_ia32_Bt(dbgi, new_block, op1, op2);
}

/**
 * Transform a node returning a "flag" result.
 *
 * @param node     the node to transform
 * @param pnc_out  the compare mode to use
 */
static ir_node *get_flags_node(ir_node *node, pn_Cmp *pnc_out)
{
	ir_node  *flags;
	ir_node  *new_op;
	ir_node  *new_block;
	dbg_info *dbgi;

	/* we have a Cmp as input */
	if (is_Proj(node)) {
		ir_node *pred = get_Proj_pred(node);
		if (is_Cmp(pred)) {
			pn_Cmp pnc = get_Proj_proj(node);
			if (ia32_cg_config.use_bt && (pnc == pn_Cmp_Lg || pnc == pn_Cmp_Eq)) {
				ir_node *l = get_Cmp_left(pred);
				ir_node *r = get_Cmp_right(pred);
				if (is_And(l)) {
					ir_node *la = get_And_left(l);
					ir_node *ra = get_And_right(l);
					if (is_Shl(la)) {
						ir_node *c = get_Shl_left(la);
						if (is_Const_1(c) && (is_Const_0(r) || r == la)) {
							/* (1 << n) & ra) */
							ir_node *n = get_Shl_right(la);
							flags    = gen_bt(pred, ra, n);
							/* we must generate a Jc/Jnc jump */
							pnc = pnc == pn_Cmp_Lg ? pn_Cmp_Lt : pn_Cmp_Ge;
							if (r == la)
								pnc ^= pn_Cmp_Leg;
							*pnc_out = ia32_pn_Cmp_unsigned | pnc;
							return flags;
						}
					}
					if (is_Shl(ra)) {
						ir_node *c = get_Shl_left(ra);
						if (is_Const_1(c) && (is_Const_0(r) || r == ra)) {
							/* la & (1 << n)) */
							ir_node *n = get_Shl_right(ra);
							flags    = gen_bt(pred, la, n);
							/* we must generate a Jc/Jnc jump */
							pnc = pnc == pn_Cmp_Lg ? pn_Cmp_Lt : pn_Cmp_Ge;
							if (r == ra)
								pnc ^= pn_Cmp_Leg;
							*pnc_out = ia32_pn_Cmp_unsigned | pnc;
							return flags;
						}
					}
				}
			}
			/* add ia32 compare flags */
			{
				ir_node *l    = get_Cmp_left(pred);
				ir_mode *mode = get_irn_mode(l);
				if (mode_is_float(mode))
					pnc |= ia32_pn_Cmp_float;
				else if (! mode_is_signed(mode))
					pnc |= ia32_pn_Cmp_unsigned;
			}
			*pnc_out = pnc;
			flags = be_transform_node(pred);
			return flags;
		}
	}

	/* a mode_b value, we have to compare it against 0 */
	dbgi      = get_irn_dbg_info(node);
	new_block = be_transform_node(get_nodes_block(node));
	new_op    = be_transform_node(node);
	flags     = new_bd_ia32_Test(dbgi, new_block, noreg_GP, noreg_GP, nomem, new_op,
	                             new_op, /*is_permuted=*/0, /*cmp_unsigned=*/0);
	*pnc_out  = pn_Cmp_Lg;
	return flags;
}

/**
 * Transforms a Load.
 *
 * @return the created ia32 Load node
 */
static ir_node *gen_Load(ir_node *node)
{
	ir_node  *old_block = get_nodes_block(node);
	ir_node  *block   = be_transform_node(old_block);
	ir_node  *ptr     = get_Load_ptr(node);
	ir_node  *mem     = get_Load_mem(node);
	ir_node  *new_mem = be_transform_node(mem);
	ir_node  *base;
	ir_node  *index;
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_mode  *mode    = get_Load_mode(node);
	ir_node  *new_node;
	ia32_address_t addr;

	/* construct load address */
	memset(&addr, 0, sizeof(addr));
	ia32_create_address_mode(&addr, ptr, 0);
	base  = addr.base;
	index = addr.index;

	if (base == NULL) {
		base = noreg_GP;
	} else {
		base = be_transform_node(base);
	}

	if (index == NULL) {
		index = noreg_GP;
	} else {
		index = be_transform_node(index);
	}

	if (mode_is_float(mode)) {
		if (ia32_cg_config.use_sse2) {
			new_node = new_bd_ia32_xLoad(dbgi, block, base, index, new_mem,
			                             mode);
		} else {
			new_node = new_bd_ia32_vfld(dbgi, block, base, index, new_mem,
			                            mode);
		}
	} else {
		assert(mode != mode_b);

		/* create a conv node with address mode for smaller modes */
		if (get_mode_size_bits(mode) < 32) {
			new_node = new_bd_ia32_Conv_I2I(dbgi, block, base, index,
			                                new_mem, noreg_GP, mode);
		} else {
			new_node = new_bd_ia32_Load(dbgi, block, base, index, new_mem);
		}
	}

	set_irn_pinned(new_node, get_irn_pinned(node));
	set_ia32_op_type(new_node, ia32_AddrModeS);
	set_ia32_ls_mode(new_node, mode);
	set_address(new_node, &addr);

	if (get_irn_pinned(node) == op_pin_state_floats) {
		assert(pn_ia32_xLoad_res == pn_ia32_vfld_res
				&& pn_ia32_vfld_res == pn_ia32_Load_res
				&& pn_ia32_Load_res == pn_ia32_res);
		arch_irn_add_flags(new_node, arch_irn_flags_rematerializable);
	}

	SET_IA32_ORIG_NODE(new_node, node);

	be_dep_on_frame(new_node);
	return new_node;
}

static int use_dest_am(ir_node *block, ir_node *node, ir_node *mem,
                       ir_node *ptr, ir_node *other)
{
	ir_node *load;

	if (!is_Proj(node))
		return 0;

	/* we only use address mode if we're the only user of the load */
	if (get_irn_n_edges(node) > 1)
		return 0;

	load = get_Proj_pred(node);
	if (!is_Load(load))
		return 0;
	if (get_nodes_block(load) != block)
		return 0;

	/* store should have the same pointer as the load */
	if (get_Load_ptr(load) != ptr)
		return 0;

	/* don't do AM if other node inputs depend on the load (via mem-proj) */
	if (other != NULL                   &&
	    get_nodes_block(other) == block &&
	    heights_reachable_in_block(heights, other, load)) {
		return 0;
	}

	if (prevents_AM(block, load, mem))
		return 0;
	/* Store should be attached to the load via mem */
	assert(heights_reachable_in_block(heights, mem, load));

	return 1;
}

static ir_node *dest_am_binop(ir_node *node, ir_node *op1, ir_node *op2,
                              ir_node *mem, ir_node *ptr, ir_mode *mode,
                              construct_binop_dest_func *func,
                              construct_binop_dest_func *func8bit,
							  match_flags_t flags)
{
	ir_node  *src_block = get_nodes_block(node);
	ir_node  *block;
	dbg_info *dbgi;
	ir_node  *new_mem;
	ir_node  *new_node;
	ir_node  *new_op;
	ir_node  *mem_proj;
	int       commutative;
	ia32_address_mode_t  am;
	ia32_address_t      *addr = &am.addr;
	memset(&am, 0, sizeof(am));

	assert(flags & match_immediate); /* there is no destam node without... */
	commutative = (flags & match_commutative) != 0;

	if (use_dest_am(src_block, op1, mem, ptr, op2)) {
		build_address(&am, op1, ia32_create_am_double_use);
		new_op = create_immediate_or_transform(op2, 0);
	} else if (commutative && use_dest_am(src_block, op2, mem, ptr, op1)) {
		build_address(&am, op2, ia32_create_am_double_use);
		new_op = create_immediate_or_transform(op1, 0);
	} else {
		return NULL;
	}

	if (addr->base == NULL)
		addr->base = noreg_GP;
	if (addr->index == NULL)
		addr->index = noreg_GP;
	if (addr->mem == NULL)
		addr->mem = nomem;

	dbgi    = get_irn_dbg_info(node);
	block   = be_transform_node(src_block);
	new_mem = transform_AM_mem(block, am.am_node, mem, addr->mem);

	if (get_mode_size_bits(mode) == 8) {
		new_node = func8bit(dbgi, block, addr->base, addr->index, new_mem, new_op);
	} else {
		new_node = func(dbgi, block, addr->base, addr->index, new_mem, new_op);
	}
	set_address(new_node, addr);
	set_ia32_op_type(new_node, ia32_AddrModeD);
	set_ia32_ls_mode(new_node, mode);
	SET_IA32_ORIG_NODE(new_node, node);

	be_set_transformed_node(get_Proj_pred(am.mem_proj), new_node);
	mem_proj = be_transform_node(am.mem_proj);
	be_set_transformed_node(mem_proj ? mem_proj : am.mem_proj, new_node);

	return new_node;
}

static ir_node *dest_am_unop(ir_node *node, ir_node *op, ir_node *mem,
                             ir_node *ptr, ir_mode *mode,
                             construct_unop_dest_func *func)
{
	ir_node  *src_block = get_nodes_block(node);
	ir_node  *block;
	dbg_info *dbgi;
	ir_node  *new_mem;
	ir_node  *new_node;
	ir_node  *mem_proj;
	ia32_address_mode_t  am;
	ia32_address_t *addr = &am.addr;

	if (!use_dest_am(src_block, op, mem, ptr, NULL))
		return NULL;

	memset(&am, 0, sizeof(am));
	build_address(&am, op, ia32_create_am_double_use);

	dbgi     = get_irn_dbg_info(node);
	block    = be_transform_node(src_block);
	new_mem  = transform_AM_mem(block, am.am_node, mem, addr->mem);
	new_node = func(dbgi, block, addr->base, addr->index, new_mem);
	set_address(new_node, addr);
	set_ia32_op_type(new_node, ia32_AddrModeD);
	set_ia32_ls_mode(new_node, mode);
	SET_IA32_ORIG_NODE(new_node, node);

	be_set_transformed_node(get_Proj_pred(am.mem_proj), new_node);
	mem_proj = be_transform_node(am.mem_proj);
	be_set_transformed_node(mem_proj ? mem_proj : am.mem_proj, new_node);

	return new_node;
}

static pn_Cmp ia32_get_negated_pnc(pn_Cmp pnc)
{
	ir_mode *mode = pnc & ia32_pn_Cmp_float ? mode_F : mode_Iu;
	return get_negated_pnc(pnc, mode);
}

static ir_node *try_create_SetMem(ir_node *node, ir_node *ptr, ir_node *mem)
{
	ir_mode        *mode      = get_irn_mode(node);
	ir_node        *mux_true  = get_Mux_true(node);
	ir_node        *mux_false = get_Mux_false(node);
	ir_node        *cond;
	dbg_info       *dbgi;
	ir_node        *block;
	ir_node        *new_block;
	ir_node        *flags;
	ir_node        *new_node;
	bool            negated;
	pn_Cmp          pnc;
	ia32_address_t  addr;

	if (get_mode_size_bits(mode) != 8)
		return NULL;

	if (is_Const_1(mux_true) && is_Const_0(mux_false)) {
		negated = false;
	} else if (is_Const_0(mux_true) && is_Const_1(mux_false)) {
		negated = true;
	} else {
		return NULL;
	}

	cond  = get_Mux_sel(node);
	flags = get_flags_node(cond, &pnc);
	/* we can't handle the float special cases with SetM */
	if (pnc & ia32_pn_Cmp_float)
		return NULL;
	if (negated)
		pnc = ia32_get_negated_pnc(pnc);

	build_address_ptr(&addr, ptr, mem);

	dbgi      = get_irn_dbg_info(node);
	block     = get_nodes_block(node);
	new_block = be_transform_node(block);
	new_node  = new_bd_ia32_SetccMem(dbgi, new_block, addr.base,
	                                 addr.index, addr.mem, flags, pnc);
	set_address(new_node, &addr);
	set_ia32_op_type(new_node, ia32_AddrModeD);
	set_ia32_ls_mode(new_node, mode);
	SET_IA32_ORIG_NODE(new_node, node);

	return new_node;
}

static ir_node *try_create_dest_am(ir_node *node)
{
	ir_node  *val  = get_Store_value(node);
	ir_node  *mem  = get_Store_mem(node);
	ir_node  *ptr  = get_Store_ptr(node);
	ir_mode  *mode = get_irn_mode(val);
	unsigned  bits = get_mode_size_bits(mode);
	ir_node  *op1;
	ir_node  *op2;
	ir_node  *new_node;

	/* handle only GP modes for now... */
	if (!ia32_mode_needs_gp_reg(mode))
		return NULL;

	for (;;) {
		/* store must be the only user of the val node */
		if (get_irn_n_edges(val) > 1)
			return NULL;
		/* skip pointless convs */
		if (is_Conv(val)) {
			ir_node *conv_op   = get_Conv_op(val);
			ir_mode *pred_mode = get_irn_mode(conv_op);
			if (!ia32_mode_needs_gp_reg(pred_mode))
				break;
			if (pred_mode == mode_b || bits <= get_mode_size_bits(pred_mode)) {
				val = conv_op;
				continue;
			}
		}
		break;
	}

	/* value must be in the same block */
	if (get_nodes_block(node) != get_nodes_block(val))
		return NULL;

	switch (get_irn_opcode(val)) {
	case iro_Add:
		op1      = get_Add_left(val);
		op2      = get_Add_right(val);
		if (ia32_cg_config.use_incdec) {
			if (is_Const_1(op2)) {
				new_node = dest_am_unop(val, op1, mem, ptr, mode, new_bd_ia32_IncMem);
				break;
			} else if (is_Const_Minus_1(op2)) {
				new_node = dest_am_unop(val, op1, mem, ptr, mode, new_bd_ia32_DecMem);
				break;
			}
		}
		new_node = dest_am_binop(val, op1, op2, mem, ptr, mode,
		                         new_bd_ia32_AddMem, new_bd_ia32_AddMem8Bit,
		                         match_commutative | match_immediate);
		break;
	case iro_Sub:
		op1      = get_Sub_left(val);
		op2      = get_Sub_right(val);
		if (is_Const(op2)) {
			ir_fprintf(stderr, "Optimisation warning: not-normalized sub ,C found\n");
		}
		new_node = dest_am_binop(val, op1, op2, mem, ptr, mode,
		                         new_bd_ia32_SubMem, new_bd_ia32_SubMem8Bit,
		                         match_immediate);
		break;
	case iro_And:
		op1      = get_And_left(val);
		op2      = get_And_right(val);
		new_node = dest_am_binop(val, op1, op2, mem, ptr, mode,
		                         new_bd_ia32_AndMem, new_bd_ia32_AndMem8Bit,
		                         match_commutative | match_immediate);
		break;
	case iro_Or:
		op1      = get_Or_left(val);
		op2      = get_Or_right(val);
		new_node = dest_am_binop(val, op1, op2, mem, ptr, mode,
		                         new_bd_ia32_OrMem, new_bd_ia32_OrMem8Bit,
		                         match_commutative | match_immediate);
		break;
	case iro_Eor:
		op1      = get_Eor_left(val);
		op2      = get_Eor_right(val);
		new_node = dest_am_binop(val, op1, op2, mem, ptr, mode,
		                         new_bd_ia32_XorMem, new_bd_ia32_XorMem8Bit,
		                         match_commutative | match_immediate);
		break;
	case iro_Shl:
		op1      = get_Shl_left(val);
		op2      = get_Shl_right(val);
		new_node = dest_am_binop(val, op1, op2, mem, ptr, mode,
		                         new_bd_ia32_ShlMem, new_bd_ia32_ShlMem,
		                         match_immediate);
		break;
	case iro_Shr:
		op1      = get_Shr_left(val);
		op2      = get_Shr_right(val);
		new_node = dest_am_binop(val, op1, op2, mem, ptr, mode,
		                         new_bd_ia32_ShrMem, new_bd_ia32_ShrMem,
		                         match_immediate);
		break;
	case iro_Shrs:
		op1      = get_Shrs_left(val);
		op2      = get_Shrs_right(val);
		new_node = dest_am_binop(val, op1, op2, mem, ptr, mode,
		                         new_bd_ia32_SarMem, new_bd_ia32_SarMem,
		                         match_immediate);
		break;
	case iro_Rotl:
		op1      = get_Rotl_left(val);
		op2      = get_Rotl_right(val);
		new_node = dest_am_binop(val, op1, op2, mem, ptr, mode,
		                         new_bd_ia32_RolMem, new_bd_ia32_RolMem,
		                         match_immediate);
		break;
	/* TODO: match ROR patterns... */
	case iro_Mux:
		new_node = try_create_SetMem(val, ptr, mem);
		break;

	case iro_Minus:
		op1      = get_Minus_op(val);
		new_node = dest_am_unop(val, op1, mem, ptr, mode, new_bd_ia32_NegMem);
		break;
	case iro_Not:
		/* should be lowered already */
		assert(mode != mode_b);
		op1      = get_Not_op(val);
		new_node = dest_am_unop(val, op1, mem, ptr, mode, new_bd_ia32_NotMem);
		break;
	default:
		return NULL;
	}

	if (new_node != NULL) {
		if (get_irn_pinned(new_node) != op_pin_state_pinned &&
				get_irn_pinned(node) == op_pin_state_pinned) {
			set_irn_pinned(new_node, op_pin_state_pinned);
		}
	}

	return new_node;
}

static bool possible_int_mode_for_fp(ir_mode *mode)
{
	unsigned size;

	if (!mode_is_signed(mode))
		return false;
	size = get_mode_size_bits(mode);
	if (size != 16 && size != 32)
		return false;
	return true;
}

static int is_float_to_int_conv(const ir_node *node)
{
	ir_mode  *mode = get_irn_mode(node);
	ir_node  *conv_op;
	ir_mode  *conv_mode;

	if (!possible_int_mode_for_fp(mode))
		return 0;

	if (!is_Conv(node))
		return 0;
	conv_op   = get_Conv_op(node);
	conv_mode = get_irn_mode(conv_op);

	if (!mode_is_float(conv_mode))
		return 0;

	return 1;
}

/**
 * Transform a Store(floatConst) into a sequence of
 * integer stores.
 *
 * @return the created ia32 Store node
 */
static ir_node *gen_float_const_Store(ir_node *node, ir_node *cns)
{
	ir_mode        *mode      = get_irn_mode(cns);
	unsigned        size      = get_mode_size_bytes(mode);
	tarval         *tv        = get_Const_tarval(cns);
	ir_node        *block     = get_nodes_block(node);
	ir_node        *new_block = be_transform_node(block);
	ir_node        *ptr       = get_Store_ptr(node);
	ir_node        *mem       = get_Store_mem(node);
	dbg_info       *dbgi      = get_irn_dbg_info(node);
	int             ofs       = 0;
	size_t          i         = 0;
	ir_node        *ins[4];
	ia32_address_t  addr;

	assert(size % 4 ==  0);
	assert(size     <= 16);

	build_address_ptr(&addr, ptr, mem);

	do {
		unsigned val =
			 get_tarval_sub_bits(tv, ofs)            |
			(get_tarval_sub_bits(tv, ofs + 1) <<  8) |
			(get_tarval_sub_bits(tv, ofs + 2) << 16) |
			(get_tarval_sub_bits(tv, ofs + 3) << 24);
		ir_node *imm = ia32_create_Immediate(NULL, 0, val);

		ir_node *new_node = new_bd_ia32_Store(dbgi, new_block, addr.base,
			addr.index, addr.mem, imm);

		set_irn_pinned(new_node, get_irn_pinned(node));
		set_ia32_op_type(new_node, ia32_AddrModeD);
		set_ia32_ls_mode(new_node, mode_Iu);
		set_address(new_node, &addr);
		SET_IA32_ORIG_NODE(new_node, node);

		assert(i < 4);
		ins[i++] = new_node;

		size        -= 4;
		ofs         += 4;
		addr.offset += 4;
	} while (size != 0);

	if (i > 1) {
		return new_rd_Sync(dbgi, new_block, i, ins);
	} else {
		return ins[0];
	}
}

/**
 * Generate a vfist or vfisttp instruction.
 */
static ir_node *gen_vfist(dbg_info *dbgi, ir_node *block, ir_node *base, ir_node *index,
                          ir_node *mem,  ir_node *val, ir_node **fist)
{
	ir_node *new_node;

	if (ia32_cg_config.use_fisttp) {
		/* Note: fisttp ALWAYS pop the tos. We have to ensure here that the value is copied
		if other users exists */
		ir_node *vfisttp = new_bd_ia32_vfisttp(dbgi, block, base, index, mem, val);
		ir_node *value   = new_r_Proj(vfisttp, mode_E, pn_ia32_vfisttp_res);
		be_new_Keep(block, 1, &value);

		new_node = new_r_Proj(vfisttp, mode_M, pn_ia32_vfisttp_M);
		*fist    = vfisttp;
	} else {
		ir_node *trunc_mode = ia32_new_Fpu_truncate(env_cg);

		/* do a fist */
		new_node = new_bd_ia32_vfist(dbgi, block, base, index, mem, val, trunc_mode);
		*fist    = new_node;
	}
	return new_node;
}
/**
 * Transforms a general (no special case) Store.
 *
 * @return the created ia32 Store node
 */
static ir_node *gen_general_Store(ir_node *node)
{
	ir_node  *val       = get_Store_value(node);
	ir_mode  *mode      = get_irn_mode(val);
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	ir_node  *ptr       = get_Store_ptr(node);
	ir_node  *mem       = get_Store_mem(node);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *new_val, *new_node, *store;
	ia32_address_t addr;

	/* check for destination address mode */
	new_node = try_create_dest_am(node);
	if (new_node != NULL)
		return new_node;

	/* construct store address */
	memset(&addr, 0, sizeof(addr));
	ia32_create_address_mode(&addr, ptr, 0);

	if (addr.base == NULL) {
		addr.base = noreg_GP;
	} else {
		addr.base = be_transform_node(addr.base);
	}

	if (addr.index == NULL) {
		addr.index = noreg_GP;
	} else {
		addr.index = be_transform_node(addr.index);
	}
	addr.mem = be_transform_node(mem);

	if (mode_is_float(mode)) {
		/* Convs (and strict-Convs) before stores are unnecessary if the mode
		   is the same. */
		while (is_Conv(val) && mode == get_irn_mode(val)) {
			ir_node *op = get_Conv_op(val);
			if (!mode_is_float(get_irn_mode(op)))
				break;
			val = op;
		}
		new_val = be_transform_node(val);
		if (ia32_cg_config.use_sse2) {
			new_node = new_bd_ia32_xStore(dbgi, new_block, addr.base,
			                              addr.index, addr.mem, new_val);
		} else {
			new_node = new_bd_ia32_vfst(dbgi, new_block, addr.base,
			                            addr.index, addr.mem, new_val, mode);
		}
		store = new_node;
	} else if (!ia32_cg_config.use_sse2 && is_float_to_int_conv(val)) {
		val = get_Conv_op(val);

		/* TODO: is this optimisation still necessary at all (middleend)? */
		/* We can skip ALL float->float up-Convs (and strict-up-Convs) before stores. */
		while (is_Conv(val)) {
			ir_node *op = get_Conv_op(val);
			if (!mode_is_float(get_irn_mode(op)))
				break;
			if (get_mode_size_bits(get_irn_mode(op)) > get_mode_size_bits(get_irn_mode(val)))
				break;
			val = op;
		}
		new_val  = be_transform_node(val);
		new_node = gen_vfist(dbgi, new_block, addr.base, addr.index, addr.mem, new_val, &store);
	} else {
		new_val = create_immediate_or_transform(val, 0);
		assert(mode != mode_b);

		if (get_mode_size_bits(mode) == 8) {
			new_node = new_bd_ia32_Store8Bit(dbgi, new_block, addr.base,
			                                 addr.index, addr.mem, new_val);
		} else {
			new_node = new_bd_ia32_Store(dbgi, new_block, addr.base,
			                             addr.index, addr.mem, new_val);
		}
		store = new_node;
	}

	set_irn_pinned(store, get_irn_pinned(node));
	set_ia32_op_type(store, ia32_AddrModeD);
	set_ia32_ls_mode(store, mode);

	set_address(store, &addr);
	SET_IA32_ORIG_NODE(store, node);

	return new_node;
}

/**
 * Transforms a Store.
 *
 * @return the created ia32 Store node
 */
static ir_node *gen_Store(ir_node *node)
{
	ir_node  *val  = get_Store_value(node);
	ir_mode  *mode = get_irn_mode(val);

	if (mode_is_float(mode) && is_Const(val)) {
		/* We can transform every floating const store
		   into a sequence of integer stores.
		   If the constant is already in a register,
		   it would be better to use it, but we don't
		   have this information here. */
		return gen_float_const_Store(node, val);
	}
	return gen_general_Store(node);
}

/**
 * Transforms a Switch.
 *
 * @return the created ia32 SwitchJmp node
 */
static ir_node *create_Switch(ir_node *node)
{
	dbg_info *dbgi       = get_irn_dbg_info(node);
	ir_node  *block      = be_transform_node(get_nodes_block(node));
	ir_node  *sel        = get_Cond_selector(node);
	ir_node  *new_sel    = be_transform_node(sel);
	long      switch_min = LONG_MAX;
	long      switch_max = LONG_MIN;
	long      default_pn = get_Cond_default_proj(node);
	ir_node  *new_node;
	const ir_edge_t *edge;

	assert(get_mode_size_bits(get_irn_mode(sel)) == 32);

	/* determine the smallest switch case value */
	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		long     pn   = get_Proj_proj(proj);
		if (pn == default_pn)
			continue;

		if (pn < switch_min)
			switch_min = pn;
		if (pn > switch_max)
			switch_max = pn;
	}

	if ((unsigned long) (switch_max - switch_min) > 128000) {
		panic("Size of switch %+F bigger than 128000", node);
	}

	if (switch_min != 0) {
		/* if smallest switch case is not 0 we need an additional sub */
		new_sel = new_bd_ia32_Lea(dbgi, block, new_sel, noreg_GP);
		add_ia32_am_offs_int(new_sel, -switch_min);
		set_ia32_op_type(new_sel, ia32_AddrModeS);

		SET_IA32_ORIG_NODE(new_sel, node);
	}

	new_node = new_bd_ia32_SwitchJmp(dbgi, block, new_sel, default_pn);
	SET_IA32_ORIG_NODE(new_node, node);

	return new_node;
}

/**
 * Transform a Cond node.
 */
static ir_node *gen_Cond(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node	 *sel       = get_Cond_selector(node);
	ir_mode  *sel_mode  = get_irn_mode(sel);
	ir_node  *flags     = NULL;
	ir_node  *new_node;
	pn_Cmp    pnc;

	if (sel_mode != mode_b) {
		return create_Switch(node);
	}

	/* we get flags from a Cmp */
	flags = get_flags_node(sel, &pnc);

	new_node = new_bd_ia32_Jcc(dbgi, new_block, flags, pnc);
	SET_IA32_ORIG_NODE(new_node, node);

	return new_node;
}

/**
 * Transform a be_Copy.
 */
static ir_node *gen_be_Copy(ir_node *node)
{
	ir_node *new_node = be_duplicate_node(node);
	ir_mode *mode     = get_irn_mode(new_node);

	if (ia32_mode_needs_gp_reg(mode)) {
		set_irn_mode(new_node, mode_Iu);
	}

	return new_node;
}

static ir_node *create_Fucom(ir_node *node)
{
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	ir_node  *left      = get_Cmp_left(node);
	ir_node  *new_left  = be_transform_node(left);
	ir_node  *right     = get_Cmp_right(node);
	ir_node  *new_right;
	ir_node  *new_node;

	if (ia32_cg_config.use_fucomi) {
		new_right = be_transform_node(right);
		new_node  = new_bd_ia32_vFucomi(dbgi, new_block, new_left,
		                                new_right, 0);
		set_ia32_commutative(new_node);
		SET_IA32_ORIG_NODE(new_node, node);
	} else {
		if (ia32_cg_config.use_ftst && is_Const_0(right)) {
			new_node = new_bd_ia32_vFtstFnstsw(dbgi, new_block, new_left, 0);
		} else {
			new_right = be_transform_node(right);
			new_node  = new_bd_ia32_vFucomFnstsw(dbgi, new_block, new_left, new_right, 0);
		}

		set_ia32_commutative(new_node);

		SET_IA32_ORIG_NODE(new_node, node);

		new_node = new_bd_ia32_Sahf(dbgi, new_block, new_node);
		SET_IA32_ORIG_NODE(new_node, node);
	}

	return new_node;
}

static ir_node *create_Ucomi(ir_node *node)
{
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *src_block = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(src_block);
	ir_node  *left      = get_Cmp_left(node);
	ir_node  *right     = get_Cmp_right(node);
	ir_node  *new_node;
	ia32_address_mode_t  am;
	ia32_address_t      *addr = &am.addr;

	match_arguments(&am, src_block, left, right, NULL,
	                match_commutative | match_am);

	new_node = new_bd_ia32_Ucomi(dbgi, new_block, addr->base, addr->index,
	                             addr->mem, am.new_op1, am.new_op2,
	                             am.ins_permuted);
	set_am_attributes(new_node, &am);

	SET_IA32_ORIG_NODE(new_node, node);

	new_node = fix_mem_proj(new_node, &am);

	return new_node;
}

/**
 * helper function: checks whether all Cmp projs are Lg or Eq which is needed
 * to fold an and into a test node
 */
static bool can_fold_test_and(ir_node *node)
{
	const ir_edge_t *edge;

	/** we can only have eq and lg projs */
	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		pn_Cmp   pnc  = get_Proj_proj(proj);
		if (pnc != pn_Cmp_Eq && pnc != pn_Cmp_Lg)
			return false;
	}

	return true;
}

/**
 * returns true if it is assured, that the upper bits of a node are "clean"
 * which means for a 16 or 8 bit value, that the upper bits in the register
 * are 0 for unsigned and a copy of the last significant bit for signed
 * numbers.
 */
static bool upper_bits_clean(ir_node *transformed_node, ir_mode *mode)
{
	assert(ia32_mode_needs_gp_reg(mode));
	if (get_mode_size_bits(mode) >= 32)
		return true;

	if (is_Proj(transformed_node))
		return upper_bits_clean(get_Proj_pred(transformed_node), mode);

	switch (get_ia32_irn_opcode(transformed_node)) {
		case iro_ia32_Conv_I2I:
		case iro_ia32_Conv_I2I8Bit: {
			ir_mode *smaller_mode = get_ia32_ls_mode(transformed_node);
			if (mode_is_signed(smaller_mode) != mode_is_signed(mode))
				return false;
			if (get_mode_size_bits(smaller_mode) > get_mode_size_bits(mode))
				return false;

			return true;
		}

		case iro_ia32_Shr:
			if (mode_is_signed(mode)) {
				return false; /* TODO handle signed modes */
			} else {
				ir_node *right = get_irn_n(transformed_node, n_ia32_Shr_count);
				if (is_ia32_Immediate(right) || is_ia32_Const(right)) {
					const ia32_immediate_attr_t *attr
						= get_ia32_immediate_attr_const(right);
					if (attr->symconst == 0 &&
							(unsigned)attr->offset >= 32 - get_mode_size_bits(mode)) {
						return true;
					}
				}
				return upper_bits_clean(get_irn_n(transformed_node, n_ia32_Shr_val), mode);
			}

		case iro_ia32_Sar:
			/* TODO too conservative if shift amount is constant */
			return upper_bits_clean(get_irn_n(transformed_node, n_ia32_Sar_val), mode);

		case iro_ia32_And:
			if (!mode_is_signed(mode)) {
				return
					upper_bits_clean(get_irn_n(transformed_node, n_ia32_And_right), mode) ||
					upper_bits_clean(get_irn_n(transformed_node, n_ia32_And_left),  mode);
			}
			/* TODO if one is known to be zero extended, then || is sufficient */
			/* FALLTHROUGH */
		case iro_ia32_Or:
		case iro_ia32_Xor:
			return
				upper_bits_clean(get_irn_n(transformed_node, n_ia32_binary_right), mode) &&
				upper_bits_clean(get_irn_n(transformed_node, n_ia32_binary_left),  mode);

		case iro_ia32_Const:
		case iro_ia32_Immediate: {
			const ia32_immediate_attr_t *attr =
				get_ia32_immediate_attr_const(transformed_node);
			if (mode_is_signed(mode)) {
				long shifted = attr->offset >> (get_mode_size_bits(mode) - 1);
				return shifted == 0 || shifted == -1;
			} else {
				unsigned long shifted = (unsigned long)attr->offset;
				shifted >>= get_mode_size_bits(mode);
				return shifted == 0;
			}
		}

		default:
			return false;
	}
}

/**
 * Generate code for a Cmp.
 */
static ir_node *gen_Cmp(ir_node *node)
{
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	ir_node  *left      = get_Cmp_left(node);
	ir_node  *right     = get_Cmp_right(node);
	ir_mode  *cmp_mode  = get_irn_mode(left);
	ir_node  *new_node;
	ia32_address_mode_t  am;
	ia32_address_t      *addr = &am.addr;
	int                  cmp_unsigned;

	if (mode_is_float(cmp_mode)) {
		if (ia32_cg_config.use_sse2) {
			return create_Ucomi(node);
		} else {
			return create_Fucom(node);
		}
	}

	assert(ia32_mode_needs_gp_reg(cmp_mode));

	/* Prefer the Test instruction, when encountering (x & y) ==/!= 0 */
	cmp_unsigned = !mode_is_signed(cmp_mode);
	if (is_Const_0(right)          &&
	    is_And(left)               &&
	    get_irn_n_edges(left) == 1 &&
	    can_fold_test_and(node)) {
		/* Test(and_left, and_right) */
		ir_node *and_left  = get_And_left(left);
		ir_node *and_right = get_And_right(left);

		/* matze: code here used mode instead of cmd_mode, I think it is always
		 * the same as cmp_mode, but I leave this here to see if this is really
		 * true...
		 */
		assert(get_irn_mode(and_left) == cmp_mode);

		match_arguments(&am, block, and_left, and_right, NULL,
										match_commutative |
										match_am | match_8bit_am | match_16bit_am |
										match_am_and_immediates | match_immediate);

		/* use 32bit compare mode if possible since the opcode is smaller */
		if (upper_bits_clean(am.new_op1, cmp_mode) &&
		    upper_bits_clean(am.new_op2, cmp_mode)) {
			cmp_mode = mode_is_signed(cmp_mode) ? mode_Is : mode_Iu;
		}

		if (get_mode_size_bits(cmp_mode) == 8) {
			new_node = new_bd_ia32_Test8Bit(dbgi, new_block, addr->base,
					addr->index, addr->mem, am.new_op1, am.new_op2, am.ins_permuted,
					cmp_unsigned);
		} else {
			new_node = new_bd_ia32_Test(dbgi, new_block, addr->base, addr->index,
					addr->mem, am.new_op1, am.new_op2, am.ins_permuted, cmp_unsigned);
		}
	} else {
		/* Cmp(left, right) */
		match_arguments(&am, block, left, right, NULL,
		                match_commutative | match_am | match_8bit_am |
		                match_16bit_am | match_am_and_immediates |
		                match_immediate);
		/* use 32bit compare mode if possible since the opcode is smaller */
		if (upper_bits_clean(am.new_op1, cmp_mode) &&
		    upper_bits_clean(am.new_op2, cmp_mode)) {
			cmp_mode = mode_is_signed(cmp_mode) ? mode_Is : mode_Iu;
		}

		if (get_mode_size_bits(cmp_mode) == 8) {
			new_node = new_bd_ia32_Cmp8Bit(dbgi, new_block, addr->base,
			                               addr->index, addr->mem, am.new_op1,
			                               am.new_op2, am.ins_permuted,
			                               cmp_unsigned);
		} else {
			new_node = new_bd_ia32_Cmp(dbgi, new_block, addr->base, addr->index,
					addr->mem, am.new_op1, am.new_op2, am.ins_permuted, cmp_unsigned);
		}
	}
	set_am_attributes(new_node, &am);
	set_ia32_ls_mode(new_node, cmp_mode);

	SET_IA32_ORIG_NODE(new_node, node);

	new_node = fix_mem_proj(new_node, &am);

	return new_node;
}

static ir_node *create_CMov(ir_node *node, ir_node *flags, ir_node *new_flags,
                            pn_Cmp pnc)
{
	dbg_info            *dbgi          = get_irn_dbg_info(node);
	ir_node             *block         = get_nodes_block(node);
	ir_node             *new_block     = be_transform_node(block);
	ir_node             *val_true      = get_Mux_true(node);
	ir_node             *val_false     = get_Mux_false(node);
	ir_node             *new_node;
	ia32_address_mode_t  am;
	ia32_address_t      *addr;

	assert(ia32_cg_config.use_cmov);
	assert(ia32_mode_needs_gp_reg(get_irn_mode(val_true)));

	addr = &am.addr;

	match_arguments(&am, block, val_false, val_true, flags,
			match_commutative | match_am | match_16bit_am | match_mode_neutral);

	if (am.ins_permuted)
		pnc = ia32_get_negated_pnc(pnc);

	new_node = new_bd_ia32_CMovcc(dbgi, new_block, addr->base, addr->index,
	                              addr->mem, am.new_op1, am.new_op2, new_flags,
	                              pnc);
	set_am_attributes(new_node, &am);

	SET_IA32_ORIG_NODE(new_node, node);

	new_node = fix_mem_proj(new_node, &am);

	return new_node;
}

/**
 * Creates a ia32 Setcc instruction.
 */
static ir_node *create_set_32bit(dbg_info *dbgi, ir_node *new_block,
                                 ir_node *flags, pn_Cmp pnc,
                                 ir_node *orig_node)
{
	ir_mode *mode  = get_irn_mode(orig_node);
	ir_node *new_node;

	new_node = new_bd_ia32_Setcc(dbgi, new_block, flags, pnc);
	SET_IA32_ORIG_NODE(new_node, orig_node);

	/* we might need to conv the result up */
	if (get_mode_size_bits(mode) > 8) {
		new_node = new_bd_ia32_Conv_I2I8Bit(dbgi, new_block, noreg_GP, noreg_GP,
		                                    nomem, new_node, mode_Bu);
		SET_IA32_ORIG_NODE(new_node, orig_node);
	}

	return new_node;
}

/**
 * Create instruction for an unsigned Difference or Zero.
 */
static ir_node *create_doz(ir_node *psi, ir_node *a, ir_node *b)
{
	ir_mode *mode  = get_irn_mode(psi);
	ir_node *new_node;
	ir_node *sub;
	ir_node *sbb;
	ir_node *not;
	ir_node *eflags;
	ir_node *block;

	dbg_info *dbgi;

	new_node = gen_binop(psi, a, b, new_bd_ia32_Sub,
		match_mode_neutral | match_am | match_immediate | match_two_users);

	block = get_nodes_block(new_node);

	if (is_Proj(new_node)) {
		sub = get_Proj_pred(new_node);
		assert(is_ia32_Sub(sub));
	} else {
		sub = new_node;
		set_irn_mode(sub, mode_T);
		new_node = new_rd_Proj(NULL, sub, mode, pn_ia32_res);
	}
	eflags = new_rd_Proj(NULL, sub, mode_Iu, pn_ia32_Sub_flags);

	dbgi   = get_irn_dbg_info(psi);
	sbb    = new_bd_ia32_Sbb0(dbgi, block, eflags);
	not    = new_bd_ia32_Not(dbgi, block, sbb);

	new_node = new_bd_ia32_And(dbgi, block, noreg_GP, noreg_GP, nomem, new_node, not);
	set_ia32_commutative(new_node);
	return new_node;
}

/**
 * Create an const array of two float consts.
 *
 * @param c0        the first constant
 * @param c1        the second constant
 * @param new_mode  IN/OUT for the mode of the constants, if NULL
 *                  smallest possible mode will be used
 */
static ir_entity *ia32_create_const_array(ir_node *c0, ir_node *c1, ir_mode **new_mode)
{
	ir_entity        *ent;
	ir_mode          *mode = *new_mode;
	ir_type          *tp;
	ir_initializer_t *initializer;
	tarval           *tv0 = get_Const_tarval(c0);
	tarval           *tv1 = get_Const_tarval(c1);

	if (mode == NULL) {
		/* detect the best mode for the constants */
		mode = get_tarval_mode(tv0);

		if (mode != mode_F) {
			if (tarval_ieee754_can_conv_lossless(tv0, mode_F) &&
			    tarval_ieee754_can_conv_lossless(tv1, mode_F)) {
				mode = mode_F;
				tv0 = tarval_convert_to(tv0, mode);
				tv1 = tarval_convert_to(tv1, mode);
			} else if (mode != mode_D) {
				if (tarval_ieee754_can_conv_lossless(tv0, mode_D) &&
				    tarval_ieee754_can_conv_lossless(tv1, mode_D)) {
					mode = mode_D;
					tv0 = tarval_convert_to(tv0, mode);
					tv1 = tarval_convert_to(tv1, mode);
				}
			}
		}

	}

	tp = ia32_create_float_type(mode, 4);
	tp = ia32_create_float_array(tp);

	ent = new_entity(get_glob_type(), ia32_unique_id(".LC%u"), tp);

	set_entity_ld_ident(ent, get_entity_ident(ent));
	set_entity_visibility(ent, ir_visibility_local);
	add_entity_linkage(ent, IR_LINKAGE_CONSTANT);

	initializer = create_initializer_compound(2);

	set_initializer_compound_value(initializer, 0, create_initializer_tarval(tv0));
	set_initializer_compound_value(initializer, 1, create_initializer_tarval(tv1));

	set_entity_initializer(ent, initializer);

	*new_mode = mode;
	return ent;
}

/**
 * Possible transformations for creating a Setcc.
 */
enum setcc_transform_insn {
	SETCC_TR_ADD,
	SETCC_TR_ADDxx,
	SETCC_TR_LEA,
	SETCC_TR_LEAxx,
	SETCC_TR_SHL,
	SETCC_TR_NEG,
	SETCC_TR_NOT,
	SETCC_TR_AND,
	SETCC_TR_SET,
	SETCC_TR_SBB,
};

typedef struct setcc_transform {
	unsigned num_steps;
	unsigned permutate_cmp_ins;
	pn_Cmp   pnc;
	struct {
		enum setcc_transform_insn  transform;
		long val;
		int  scale;
	} steps[4];
} setcc_transform_t;

/**
 * Setcc can only handle 0 and 1 result.
 * Find a transformation that creates 0 and 1 from
 * tv_t and tv_f.
 */
static void find_const_transform(pn_Cmp pnc, tarval *t, tarval *f,
                                 setcc_transform_t *res)
{
	unsigned step = 0;

	res->num_steps = 0;
	res->permutate_cmp_ins = 0;

	if (tarval_is_null(t)) {
		tarval *tmp = t;
		t = f;
		f = tmp;
		pnc = ia32_get_negated_pnc(pnc);
	} else if (tarval_cmp(t, f) == pn_Cmp_Lt) {
		// now, t is the bigger one
		tarval *tmp = t;
		t = f;
		f = tmp;
		pnc = ia32_get_negated_pnc(pnc);
	}
	res->pnc = pnc;

	if (! tarval_is_null(f)) {
		tarval *t_sub = tarval_sub(t, f, NULL);

		t = t_sub;
		res->steps[step].transform = SETCC_TR_ADD;

		if (t == tarval_bad)
			panic("constant subtract failed");
		if (! tarval_is_long(f))
			panic("tarval is not long");

		res->steps[step].val = get_tarval_long(f);
		++step;
		f = tarval_sub(f, f, NULL);
		assert(tarval_is_null(f));
	}

	if (tarval_is_one(t)) {
		res->steps[step].transform = SETCC_TR_SET;
		res->num_steps = ++step;
		return;
	}

	if (tarval_is_minus_one(t)) {
		res->steps[step].transform = SETCC_TR_NEG;
		++step;
		res->steps[step].transform = SETCC_TR_SET;
		res->num_steps = ++step;
		return;
	}
	if (tarval_is_long(t)) {
		long v = get_tarval_long(t);

		res->steps[step].val = 0;
		switch (v) {
		case 9:
			if (step > 0 && res->steps[step - 1].transform == SETCC_TR_ADD)
				--step;
			res->steps[step].transform = SETCC_TR_LEAxx;
			res->steps[step].scale     = 3; /* (a << 3) + a */
			break;
		case 8:
			if (step > 0 && res->steps[step - 1].transform == SETCC_TR_ADD)
				--step;
			res->steps[step].transform = res->steps[step].val == 0 ? SETCC_TR_SHL : SETCC_TR_LEA;
			res->steps[step].scale     = 3; /* (a << 3) */
			break;
		case 5:
			if (step > 0 && res->steps[step - 1].transform == SETCC_TR_ADD)
				--step;
			res->steps[step].transform = SETCC_TR_LEAxx;
			res->steps[step].scale     = 2; /* (a << 2) + a */
			break;
		case 4:
			if (step > 0 && res->steps[step - 1].transform == SETCC_TR_ADD)
				--step;
			res->steps[step].transform = res->steps[step].val == 0 ? SETCC_TR_SHL : SETCC_TR_LEA;
			res->steps[step].scale     = 2; /* (a << 2) */
			break;
		case 3:
			if (step > 0 && res->steps[step - 1].transform == SETCC_TR_ADD)
				--step;
			res->steps[step].transform = SETCC_TR_LEAxx;
			res->steps[step].scale     = 1; /* (a << 1) + a */
			break;
		case 2:
			if (step > 0 && res->steps[step - 1].transform == SETCC_TR_ADD)
				--step;
			res->steps[step].transform = res->steps[step].val == 0 ? SETCC_TR_SHL : SETCC_TR_LEA;
			res->steps[step].scale     = 1; /* (a << 1) */
			break;
		case 1:
			res->num_steps = step;
			return;
		default:
			if (! tarval_is_single_bit(t)) {
				res->steps[step].transform = SETCC_TR_AND;
				res->steps[step].val       = v;
				++step;
				res->steps[step].transform = SETCC_TR_NEG;
			} else {
				int v = get_tarval_lowest_bit(t);
				assert(v >= 0);

				res->steps[step].transform = SETCC_TR_SHL;
				res->steps[step].scale     = v;
			}
		}
		++step;
		res->steps[step].transform = SETCC_TR_SET;
		res->num_steps = ++step;
		return;
	}
	panic("tarval is not long");
}

/**
 * Transforms a Mux node into some code sequence.
 *
 * @return The transformed node.
 */
static ir_node *gen_Mux(ir_node *node)
{
	dbg_info *dbgi        = get_irn_dbg_info(node);
	ir_node  *block       = get_nodes_block(node);
	ir_node  *new_block   = be_transform_node(block);
	ir_node  *mux_true    = get_Mux_true(node);
	ir_node  *mux_false   = get_Mux_false(node);
	ir_node  *cond        = get_Mux_sel(node);
	ir_mode  *mode        = get_irn_mode(node);
	ir_node  *flags;
	ir_node  *new_node;
	pn_Cmp   pnc;

	assert(get_irn_mode(cond) == mode_b);

	/* Note: a Mux node uses a Load two times IFF it's used in the compare AND in the result */
	if (mode_is_float(mode)) {
		ir_node  *cmp         = get_Proj_pred(cond);
		ir_node  *cmp_left    = get_Cmp_left(cmp);
		ir_node  *cmp_right   = get_Cmp_right(cmp);
		pn_Cmp   pnc          = get_Proj_proj(cond);

		if (ia32_cg_config.use_sse2) {
			if (pnc == pn_Cmp_Lt || pnc == pn_Cmp_Le) {
				if (cmp_left == mux_true && cmp_right == mux_false) {
					/* Mux(a <= b, a, b) => MIN */
					return gen_binop(node, cmp_left, cmp_right, new_bd_ia32_xMin,
			                 match_commutative | match_am | match_two_users);
				} else if (cmp_left == mux_false && cmp_right == mux_true) {
					/* Mux(a <= b, b, a) => MAX */
					return gen_binop(node, cmp_left, cmp_right, new_bd_ia32_xMax,
			                 match_commutative | match_am | match_two_users);
				}
			} else if (pnc == pn_Cmp_Gt || pnc == pn_Cmp_Ge) {
				if (cmp_left == mux_true && cmp_right == mux_false) {
					/* Mux(a >= b, a, b) => MAX */
					return gen_binop(node, cmp_left, cmp_right, new_bd_ia32_xMax,
			                 match_commutative | match_am | match_two_users);
				} else if (cmp_left == mux_false && cmp_right == mux_true) {
					/* Mux(a >= b, b, a) => MIN */
					return gen_binop(node, cmp_left, cmp_right, new_bd_ia32_xMin,
			                 match_commutative | match_am | match_two_users);
				}
			}
		}

		if (is_Const(mux_true) && is_Const(mux_false)) {
			ia32_address_mode_t am;
			ir_node             *load;
			ir_mode             *new_mode;
			unsigned            scale;

			flags    = get_flags_node(cond, &pnc);
			new_node = create_set_32bit(dbgi, new_block, flags, pnc, node);

			if (ia32_cg_config.use_sse2) {
				/* cannot load from different mode on SSE */
				new_mode = mode;
			} else {
				/* x87 can load any mode */
				new_mode = NULL;
			}

			am.addr.symconst_ent = ia32_create_const_array(mux_false, mux_true, &new_mode);

			switch (get_mode_size_bytes(new_mode)) {
			case 4:
				scale = 2;
				break;
			case 8:
				scale = 3;
				break;
			case 10:
				/* use 2 * 5 */
				scale = 1;
				new_node = new_bd_ia32_Lea(dbgi, new_block, new_node, new_node);
				set_ia32_am_scale(new_node, 2);
				break;
			case 12:
				/* use 4 * 3 */
				scale = 2;
				new_node = new_bd_ia32_Lea(dbgi, new_block, new_node, new_node);
				set_ia32_am_scale(new_node, 1);
				break;
			case 16:
				/* arg, shift 16 NOT supported */
				scale = 3;
				new_node = new_bd_ia32_Add(dbgi, new_block, noreg_GP, noreg_GP, nomem, new_node, new_node);
				break;
			default:
				panic("Unsupported constant size");
			}

			am.ls_mode            = new_mode;
			am.addr.base          = get_symconst_base();
			am.addr.index         = new_node;
			am.addr.mem           = nomem;
			am.addr.offset        = 0;
			am.addr.scale         = scale;
			am.addr.use_frame     = 0;
			am.addr.frame_entity  = NULL;
			am.addr.symconst_sign = 0;
			am.mem_proj           = am.addr.mem;
			am.op_type            = ia32_AddrModeS;
			am.new_op1            = NULL;
			am.new_op2            = NULL;
			am.pinned             = op_pin_state_floats;
			am.commutative        = 1;
			am.ins_permuted       = 0;

			if (ia32_cg_config.use_sse2)
				load = new_bd_ia32_xLoad(dbgi, block, am.addr.base, am.addr.index, am.addr.mem, new_mode);
			else
				load = new_bd_ia32_vfld(dbgi, block, am.addr.base, am.addr.index, am.addr.mem, new_mode);
			set_am_attributes(load, &am);

			return new_rd_Proj(NULL, load, mode_vfp, pn_ia32_res);
		}
		panic("cannot transform floating point Mux");

	} else {
		assert(ia32_mode_needs_gp_reg(mode));

		if (is_Proj(cond)) {
			ir_node *cmp = get_Proj_pred(cond);
			if (is_Cmp(cmp)) {
				ir_node  *cmp_left    = get_Cmp_left(cmp);
				ir_node  *cmp_right   = get_Cmp_right(cmp);
				pn_Cmp   pnc          = get_Proj_proj(cond);

				/* check for unsigned Doz first */
				if ((pnc & pn_Cmp_Gt) && !mode_is_signed(mode) &&
					is_Const_0(mux_false) && is_Sub(mux_true) &&
					get_Sub_left(mux_true) == cmp_left && get_Sub_right(mux_true) == cmp_right) {
					/* Mux(a >=u b, a - b, 0) unsigned Doz */
					return create_doz(node, cmp_left, cmp_right);
				} else if ((pnc & pn_Cmp_Lt) && !mode_is_signed(mode) &&
					is_Const_0(mux_true) && is_Sub(mux_false) &&
					get_Sub_left(mux_false) == cmp_left && get_Sub_right(mux_false) == cmp_right) {
					/* Mux(a <=u b, 0, a - b) unsigned Doz */
					return create_doz(node, cmp_left, cmp_right);
				}
			}
		}

		flags = get_flags_node(cond, &pnc);

		if (is_Const(mux_true) && is_Const(mux_false)) {
			/* both are const, good */
			tarval *tv_true = get_Const_tarval(mux_true);
			tarval *tv_false = get_Const_tarval(mux_false);
			setcc_transform_t res;
			int step;

			find_const_transform(pnc, tv_true, tv_false, &res);
			new_node = node;
			if (res.permutate_cmp_ins) {
				ia32_attr_t *attr = get_ia32_attr(flags);
				attr->data.ins_permuted ^= 1;
			}
			for (step = (int)res.num_steps - 1; step >= 0; --step) {
				ir_node *imm;

				switch (res.steps[step].transform) {
				case SETCC_TR_ADD:
					imm = ia32_immediate_from_long(res.steps[step].val);
					new_node = new_bd_ia32_Add(dbgi, new_block, noreg_GP, noreg_GP, nomem, new_node, imm);
					break;
				case SETCC_TR_ADDxx:
					new_node = new_bd_ia32_Lea(dbgi, new_block, new_node, new_node);
					break;
				case SETCC_TR_LEA:
					new_node = new_bd_ia32_Lea(dbgi, new_block, noreg_GP, new_node);
					set_ia32_am_scale(new_node, res.steps[step].scale);
					set_ia32_am_offs_int(new_node, res.steps[step].val);
					break;
				case SETCC_TR_LEAxx:
					new_node = new_bd_ia32_Lea(dbgi, new_block, new_node, new_node);
					set_ia32_am_scale(new_node, res.steps[step].scale);
					set_ia32_am_offs_int(new_node, res.steps[step].val);
					break;
				case SETCC_TR_SHL:
					imm = ia32_immediate_from_long(res.steps[step].scale);
					new_node = new_bd_ia32_Shl(dbgi, new_block, new_node, imm);
					break;
				case SETCC_TR_NEG:
					new_node = new_bd_ia32_Neg(dbgi, new_block, new_node);
					break;
				case SETCC_TR_NOT:
					new_node = new_bd_ia32_Not(dbgi, new_block, new_node);
					break;
				case SETCC_TR_AND:
					imm = ia32_immediate_from_long(res.steps[step].val);
					new_node = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, new_node, imm);
					break;
				case SETCC_TR_SET:
					new_node = create_set_32bit(dbgi, new_block, flags, res.pnc, new_node);
					break;
				case SETCC_TR_SBB:
					new_node = new_bd_ia32_Sbb0(dbgi, new_block, flags);
					break;
				default:
					panic("unknown setcc transform");
				}
			}
		} else {
			new_node = create_CMov(node, cond, flags, pnc);
		}
		return new_node;
	}
}


/**
 * Create a conversion from x87 state register to general purpose.
 */
static ir_node *gen_x87_fp_to_gp(ir_node *node)
{
	ir_node         *block      = be_transform_node(get_nodes_block(node));
	ir_node         *op         = get_Conv_op(node);
	ir_node         *new_op     = be_transform_node(op);
	ir_graph        *irg        = current_ir_graph;
	dbg_info        *dbgi       = get_irn_dbg_info(node);
	ir_mode         *mode       = get_irn_mode(node);
	ir_node         *fist, *load, *mem;

	mem = gen_vfist(dbgi, block, get_irg_frame(irg), noreg_GP, nomem, new_op, &fist);
	set_irn_pinned(fist, op_pin_state_floats);
	set_ia32_use_frame(fist);
	set_ia32_op_type(fist, ia32_AddrModeD);

	assert(get_mode_size_bits(mode) <= 32);
	/* exception we can only store signed 32 bit integers, so for unsigned
	   we store a 64bit (signed) integer and load the lower bits */
	if (get_mode_size_bits(mode) == 32 && !mode_is_signed(mode)) {
		set_ia32_ls_mode(fist, mode_Ls);
	} else {
		set_ia32_ls_mode(fist, mode_Is);
	}
	SET_IA32_ORIG_NODE(fist, node);

	/* do a Load */
	load = new_bd_ia32_Load(dbgi, block, get_irg_frame(irg), noreg_GP, mem);

	set_irn_pinned(load, op_pin_state_floats);
	set_ia32_use_frame(load);
	set_ia32_op_type(load, ia32_AddrModeS);
	set_ia32_ls_mode(load, mode_Is);
	if (get_ia32_ls_mode(fist) == mode_Ls) {
		ia32_attr_t *attr = get_ia32_attr(load);
		attr->data.need_64bit_stackent = 1;
	} else {
		ia32_attr_t *attr = get_ia32_attr(load);
		attr->data.need_32bit_stackent = 1;
	}
	SET_IA32_ORIG_NODE(load, node);

	return new_r_Proj(load, mode_Iu, pn_ia32_Load_res);
}

/**
 * Creates a x87 strict Conv by placing a Store and a Load
 */
static ir_node *gen_x87_strict_conv(ir_mode *tgt_mode, ir_node *node)
{
	ir_node  *block    = get_nodes_block(node);
	ir_graph *irg      = get_Block_irg(block);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *frame    = get_irg_frame(irg);
	ir_node  *store, *load;
	ir_node  *new_node;

	store = new_bd_ia32_vfst(dbgi, block, frame, noreg_GP, nomem, node, tgt_mode);
	set_ia32_use_frame(store);
	set_ia32_op_type(store, ia32_AddrModeD);
	SET_IA32_ORIG_NODE(store, node);

	load = new_bd_ia32_vfld(dbgi, block, frame, noreg_GP, store, tgt_mode);
	set_ia32_use_frame(load);
	set_ia32_op_type(load, ia32_AddrModeS);
	SET_IA32_ORIG_NODE(load, node);

	new_node = new_r_Proj(load, mode_E, pn_ia32_vfld_res);
	return new_node;
}

static ir_node *create_Conv_I2I(dbg_info *dbgi, ir_node *block, ir_node *base,
		ir_node *index, ir_node *mem, ir_node *val, ir_mode *mode)
{
	ir_node *(*func)(dbg_info*, ir_node*, ir_node*, ir_node*, ir_node*, ir_node*, ir_mode*);

	func = get_mode_size_bits(mode) == 8 ?
		new_bd_ia32_Conv_I2I8Bit : new_bd_ia32_Conv_I2I;
	return func(dbgi, block, base, index, mem, val, mode);
}

/**
 * Create a conversion from general purpose to x87 register
 */
static ir_node *gen_x87_gp_to_fp(ir_node *node, ir_mode *src_mode)
{
	ir_node  *src_block = get_nodes_block(node);
	ir_node  *block     = be_transform_node(src_block);
	ir_graph *irg       = get_Block_irg(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *op        = get_Conv_op(node);
	ir_node  *new_op    = NULL;
	ir_mode  *mode;
	ir_mode  *store_mode;
	ir_node  *fild;
	ir_node  *store;
	ir_node  *new_node;

	/* fild can use source AM if the operand is a signed 16bit or 32bit integer */
	if (possible_int_mode_for_fp(src_mode)) {
		ia32_address_mode_t am;

		match_arguments(&am, src_block, NULL, op, NULL, match_am | match_try_am | match_16bit_am);
		if (am.op_type == ia32_AddrModeS) {
			ia32_address_t *addr = &am.addr;

			fild     = new_bd_ia32_vfild(dbgi, block, addr->base, addr->index, addr->mem);
			new_node = new_r_Proj(fild, mode_vfp, pn_ia32_vfild_res);

			set_am_attributes(fild, &am);
			SET_IA32_ORIG_NODE(fild, node);

			fix_mem_proj(fild, &am);

			return new_node;
		}
	}
	if (new_op == NULL) {
		new_op = be_transform_node(op);
	}

	mode = get_irn_mode(op);

	/* first convert to 32 bit signed if necessary */
	if (get_mode_size_bits(src_mode) < 32) {
		if (!upper_bits_clean(new_op, src_mode)) {
			new_op = create_Conv_I2I(dbgi, block, noreg_GP, noreg_GP, nomem, new_op, src_mode);
			SET_IA32_ORIG_NODE(new_op, node);
		}
		mode = mode_Is;
	}

	assert(get_mode_size_bits(mode) == 32);

	/* do a store */
	store = new_bd_ia32_Store(dbgi, block, get_irg_frame(irg), noreg_GP, nomem, new_op);

	set_ia32_use_frame(store);
	set_ia32_op_type(store, ia32_AddrModeD);
	set_ia32_ls_mode(store, mode_Iu);

	/* exception for 32bit unsigned, do a 64bit spill+load */
	if (!mode_is_signed(mode)) {
		ir_node *in[2];
		/* store a zero */
		ir_node *zero_const = ia32_create_Immediate(NULL, 0, 0);

		ir_node *zero_store = new_bd_ia32_Store(dbgi, block, get_irg_frame(irg),
		                                        noreg_GP, nomem, zero_const);

		set_ia32_use_frame(zero_store);
		set_ia32_op_type(zero_store, ia32_AddrModeD);
		add_ia32_am_offs_int(zero_store, 4);
		set_ia32_ls_mode(zero_store, mode_Iu);

		in[0] = zero_store;
		in[1] = store;

		store      = new_rd_Sync(dbgi, block, 2, in);
		store_mode = mode_Ls;
	} else {
		store_mode = mode_Is;
	}

	/* do a fild */
	fild = new_bd_ia32_vfild(dbgi, block, get_irg_frame(irg), noreg_GP, store);

	set_ia32_use_frame(fild);
	set_ia32_op_type(fild, ia32_AddrModeS);
	set_ia32_ls_mode(fild, store_mode);

	new_node = new_r_Proj(fild, mode_vfp, pn_ia32_vfild_res);

	return new_node;
}

/**
 * Create a conversion from one integer mode into another one
 */
static ir_node *create_I2I_Conv(ir_mode *src_mode, ir_mode *tgt_mode,
                                dbg_info *dbgi, ir_node *block, ir_node *op,
                                ir_node *node)
{
	ir_node             *new_block = be_transform_node(block);
	ir_node             *new_node;
	ir_mode             *smaller_mode;
	ia32_address_mode_t  am;
	ia32_address_t      *addr = &am.addr;

	(void) node;
	if (get_mode_size_bits(src_mode) < get_mode_size_bits(tgt_mode)) {
		smaller_mode = src_mode;
	} else {
		smaller_mode = tgt_mode;
	}

#ifdef DEBUG_libfirm
	if (is_Const(op)) {
		ir_fprintf(stderr, "Optimisation warning: conv after constant %+F\n",
		           op);
	}
#endif

	match_arguments(&am, block, NULL, op, NULL,
	                match_am | match_8bit_am | match_16bit_am);

	if (upper_bits_clean(am.new_op2, smaller_mode)) {
		/* unnecessary conv. in theory it shouldn't have been AM */
		assert(is_ia32_NoReg_GP(addr->base));
		assert(is_ia32_NoReg_GP(addr->index));
		assert(is_NoMem(addr->mem));
		assert(am.addr.offset == 0);
		assert(am.addr.symconst_ent == NULL);
		return am.new_op2;
	}

	new_node = create_Conv_I2I(dbgi, new_block, addr->base, addr->index,
			addr->mem, am.new_op2, smaller_mode);
	set_am_attributes(new_node, &am);
	/* match_arguments assume that out-mode = in-mode, this isn't true here
	 * so fix it */
	set_ia32_ls_mode(new_node, smaller_mode);
	SET_IA32_ORIG_NODE(new_node, node);
	new_node = fix_mem_proj(new_node, &am);
	return new_node;
}

/**
 * Transforms a Conv node.
 *
 * @return The created ia32 Conv node
 */
static ir_node *gen_Conv(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	ir_node  *op        = get_Conv_op(node);
	ir_node  *new_op    = NULL;
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_mode  *src_mode  = get_irn_mode(op);
	ir_mode  *tgt_mode  = get_irn_mode(node);
	int       src_bits  = get_mode_size_bits(src_mode);
	int       tgt_bits  = get_mode_size_bits(tgt_mode);
	ir_node  *res       = NULL;

	assert(!mode_is_int(src_mode) || src_bits <= 32);
	assert(!mode_is_int(tgt_mode) || tgt_bits <= 32);

	/* modeB -> X should already be lowered by the lower_mode_b pass */
	if (src_mode == mode_b) {
		panic("ConvB not lowered %+F", node);
	}

	if (src_mode == tgt_mode) {
		if (get_Conv_strict(node)) {
			if (ia32_cg_config.use_sse2) {
				/* when we are in SSE mode, we can kill all strict no-op conversion */
				return be_transform_node(op);
			}
		} else {
			/* this should be optimized already, but who knows... */
			DEBUG_ONLY(ir_fprintf(stderr, "Debug warning: conv %+F is pointless\n", node));
			DB((dbg, LEVEL_1, "killed Conv(mode, mode) ..."));
			return be_transform_node(op);
		}
	}

	if (mode_is_float(src_mode)) {
		new_op = be_transform_node(op);
		/* we convert from float ... */
		if (mode_is_float(tgt_mode)) {
			/* ... to float */
			if (ia32_cg_config.use_sse2) {
				DB((dbg, LEVEL_1, "create Conv(float, float) ..."));
				res = new_bd_ia32_Conv_FP2FP(dbgi, new_block, noreg_GP, noreg_GP,
				                             nomem, new_op);
				set_ia32_ls_mode(res, tgt_mode);
			} else {
				if (get_Conv_strict(node)) {
					/* if fp_no_float_fold is not set then we assume that we
					 * don't have any float operations in a non
					 * mode_float_arithmetic mode and can skip strict upconvs */
					if (src_bits < tgt_bits
							&& !(get_irg_fp_model(current_ir_graph) & fp_no_float_fold)) {
						DB((dbg, LEVEL_1, "killed Conv(float, float) ..."));
						return new_op;
					} else {
						res = gen_x87_strict_conv(tgt_mode, new_op);
						SET_IA32_ORIG_NODE(get_Proj_pred(res), node);
						return res;
					}
				}
				DB((dbg, LEVEL_1, "killed Conv(float, float) ..."));
				return new_op;
			}
		} else {
			/* ... to int */
			DB((dbg, LEVEL_1, "create Conv(float, int) ..."));
			if (ia32_cg_config.use_sse2) {
				res = new_bd_ia32_Conv_FP2I(dbgi, new_block, noreg_GP, noreg_GP,
				                            nomem, new_op);
				set_ia32_ls_mode(res, src_mode);
			} else {
				return gen_x87_fp_to_gp(node);
			}
		}
	} else {
		/* we convert from int ... */
		if (mode_is_float(tgt_mode)) {
			/* ... to float */
			DB((dbg, LEVEL_1, "create Conv(int, float) ..."));
			if (ia32_cg_config.use_sse2) {
				new_op = be_transform_node(op);
				res = new_bd_ia32_Conv_I2FP(dbgi, new_block, noreg_GP, noreg_GP,
				                            nomem, new_op);
				set_ia32_ls_mode(res, tgt_mode);
			} else {
				unsigned int_mantissa   = get_mode_size_bits(src_mode) - (mode_is_signed(src_mode) ? 1 : 0);
				unsigned float_mantissa = tarval_ieee754_get_mantissa_size(tgt_mode);
				res = gen_x87_gp_to_fp(node, src_mode);

				/* we need a strict-Conv, if the int mode has more bits than the
				 * float mantissa */
				if (float_mantissa < int_mantissa) {
					res = gen_x87_strict_conv(tgt_mode, res);
					SET_IA32_ORIG_NODE(get_Proj_pred(res), node);
				}
				return res;
			}
		} else if (tgt_mode == mode_b) {
			/* mode_b lowering already took care that we only have 0/1 values */
			DB((dbg, LEVEL_1, "omitting unnecessary Conv(%+F, %+F) ...",
			    src_mode, tgt_mode));
			return be_transform_node(op);
		} else {
			/* to int */
			if (src_bits == tgt_bits) {
				DB((dbg, LEVEL_1, "omitting unnecessary Conv(%+F, %+F) ...",
				    src_mode, tgt_mode));
				return be_transform_node(op);
			}

			res = create_I2I_Conv(src_mode, tgt_mode, dbgi, block, op, node);
			return res;
		}
	}

	return res;
}

static ir_node *create_immediate_or_transform(ir_node *node,
                                              char immediate_constraint_type)
{
	ir_node *new_node = try_create_Immediate(node, immediate_constraint_type);
	if (new_node == NULL) {
		new_node = be_transform_node(node);
	}
	return new_node;
}

/**
 * Transforms a FrameAddr into an ia32 Add.
 */
static ir_node *gen_be_FrameAddr(ir_node *node)
{
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *op     = be_get_FrameAddr_frame(node);
	ir_node  *new_op = be_transform_node(op);
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *new_node;

	new_node = new_bd_ia32_Lea(dbgi, block, new_op, noreg_GP);
	set_ia32_frame_ent(new_node, arch_get_frame_entity(node));
	set_ia32_use_frame(new_node);

	SET_IA32_ORIG_NODE(new_node, node);

	return new_node;
}

/**
 * In case SSE is used we need to copy the result from XMM0 to FPU TOS before return.
 */
static ir_node *gen_be_Return(ir_node *node)
{
	ir_graph  *irg     = current_ir_graph;
	ir_node   *ret_val = get_irn_n(node, be_pos_Return_val);
	ir_node   *ret_mem = get_irn_n(node, be_pos_Return_mem);
	ir_entity *ent     = get_irg_entity(irg);
	ir_type   *tp      = get_entity_type(ent);
	dbg_info  *dbgi;
	ir_node   *block;
	ir_type   *res_type;
	ir_mode   *mode;
	ir_node   *frame, *sse_store, *fld, *mproj, *barrier;
	ir_node   *new_barrier, *new_ret_val, *new_ret_mem;
	ir_node   **in;
	int       pn_ret_val, pn_ret_mem, arity, i;

	assert(ret_val != NULL);
	if (be_Return_get_n_rets(node) < 1 || ! ia32_cg_config.use_sse2) {
		return be_duplicate_node(node);
	}

	res_type = get_method_res_type(tp, 0);

	if (! is_Primitive_type(res_type)) {
		return be_duplicate_node(node);
	}

	mode = get_type_mode(res_type);
	if (! mode_is_float(mode)) {
		return be_duplicate_node(node);
	}

	assert(get_method_n_ress(tp) == 1);

	pn_ret_val = get_Proj_proj(ret_val);
	pn_ret_mem = get_Proj_proj(ret_mem);

	/* get the Barrier */
	barrier = get_Proj_pred(ret_val);

	/* get result input of the Barrier */
	ret_val     = get_irn_n(barrier, pn_ret_val);
	new_ret_val = be_transform_node(ret_val);

	/* get memory input of the Barrier */
	ret_mem     = get_irn_n(barrier, pn_ret_mem);
	new_ret_mem = be_transform_node(ret_mem);

	frame = get_irg_frame(irg);

	dbgi  = get_irn_dbg_info(barrier);
	block = be_transform_node(get_nodes_block(barrier));

	/* store xmm0 onto stack */
	sse_store = new_bd_ia32_xStoreSimple(dbgi, block, frame, noreg_GP,
	                                     new_ret_mem, new_ret_val);
	set_ia32_ls_mode(sse_store, mode);
	set_ia32_op_type(sse_store, ia32_AddrModeD);
	set_ia32_use_frame(sse_store);

	/* load into x87 register */
	fld = new_bd_ia32_vfld(dbgi, block, frame, noreg_GP, sse_store, mode);
	set_ia32_op_type(fld, ia32_AddrModeS);
	set_ia32_use_frame(fld);

	mproj = new_r_Proj(fld, mode_M, pn_ia32_vfld_M);
	fld   = new_r_Proj(fld, mode_vfp, pn_ia32_vfld_res);

	/* create a new barrier */
	arity = get_irn_arity(barrier);
	in    = ALLOCAN(ir_node*, arity);
	for (i = 0; i < arity; ++i) {
		ir_node *new_in;

		if (i == pn_ret_val) {
			new_in = fld;
		} else if (i == pn_ret_mem) {
			new_in = mproj;
		} else {
			ir_node *in = get_irn_n(barrier, i);
			new_in = be_transform_node(in);
		}
		in[i] = new_in;
	}

	new_barrier = new_ir_node(dbgi, irg, block,
	                          get_irn_op(barrier), get_irn_mode(barrier),
	                          arity, in);
	copy_node_attr(irg, barrier, new_barrier);
	be_duplicate_deps(barrier, new_barrier);
	be_set_transformed_node(barrier, new_barrier);

	/* transform normally */
	return be_duplicate_node(node);
}

/**
 * Transform a be_AddSP into an ia32_SubSP.
 */
static ir_node *gen_be_AddSP(ir_node *node)
{
	ir_node  *sz = get_irn_n(node, be_pos_AddSP_size);
	ir_node  *sp = get_irn_n(node, be_pos_AddSP_old_sp);

	return gen_binop(node, sp, sz, new_bd_ia32_SubSP,
	                 match_am | match_immediate);
}

/**
 * Transform a be_SubSP into an ia32_AddSP
 */
static ir_node *gen_be_SubSP(ir_node *node)
{
	ir_node  *sz = get_irn_n(node, be_pos_SubSP_size);
	ir_node  *sp = get_irn_n(node, be_pos_SubSP_old_sp);

	return gen_binop(node, sp, sz, new_bd_ia32_AddSP,
	                 match_am | match_immediate);
}

/**
 * Change some phi modes
 */
static ir_node *gen_Phi(ir_node *node)
{
	const arch_register_req_t *req;
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_graph *irg   = current_ir_graph;
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_mode  *mode  = get_irn_mode(node);
	ir_node  *phi;

	if (ia32_mode_needs_gp_reg(mode)) {
		/* we shouldn't have any 64bit stuff around anymore */
		assert(get_mode_size_bits(mode) <= 32);
		/* all integer operations are on 32bit registers now */
		mode = mode_Iu;
		req  = ia32_reg_classes[CLASS_ia32_gp].class_req;
	} else if (mode_is_float(mode)) {
		if (ia32_cg_config.use_sse2) {
			mode = mode_xmm;
			req  = ia32_reg_classes[CLASS_ia32_xmm].class_req;
		} else {
			mode = mode_vfp;
			req  = ia32_reg_classes[CLASS_ia32_vfp].class_req;
		}
	} else {
		req = arch_no_register_req;
	}

	/* phi nodes allow loops, so we use the old arguments for now
	 * and fix this later */
	phi = new_ir_node(dbgi, irg, block, op_Phi, mode, get_irn_arity(node),
	                  get_irn_in(node) + 1);
	copy_node_attr(irg, node, phi);
	be_duplicate_deps(node, phi);

	arch_set_out_register_req(phi, 0, req);

	be_enqueue_preds(node);

	return phi;
}

static ir_node *gen_Jmp(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *new_node;

	new_node = new_bd_ia32_Jmp(dbgi, new_block);
	SET_IA32_ORIG_NODE(new_node, node);

	return new_node;
}

/**
 * Transform IJmp
 */
static ir_node *gen_IJmp(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *op        = get_IJmp_target(node);
	ir_node  *new_node;
	ia32_address_mode_t  am;
	ia32_address_t      *addr = &am.addr;

	assert(get_irn_mode(op) == mode_P);

	match_arguments(&am, block, NULL, op, NULL, match_am | match_immediate);

	new_node = new_bd_ia32_IJmp(dbgi, new_block, addr->base, addr->index,
			addr->mem, am.new_op2);
	set_am_attributes(new_node, &am);
	SET_IA32_ORIG_NODE(new_node, node);

	new_node = fix_mem_proj(new_node, &am);

	return new_node;
}

/**
 * Transform a Bound node.
 */
static ir_node *gen_Bound(ir_node *node)
{
	ir_node  *new_node;
	ir_node  *lower = get_Bound_lower(node);
	dbg_info *dbgi  = get_irn_dbg_info(node);

	if (is_Const_0(lower)) {
		/* typical case for Java */
		ir_node  *sub, *res, *flags, *block;

		res = gen_binop(node, get_Bound_index(node), get_Bound_upper(node),
			new_bd_ia32_Sub, match_mode_neutral	| match_am | match_immediate);

		block = get_nodes_block(res);
		if (! is_Proj(res)) {
			sub = res;
			set_irn_mode(sub, mode_T);
			res = new_rd_Proj(NULL, sub, mode_Iu, pn_ia32_res);
		} else {
			sub = get_Proj_pred(res);
		}
		flags = new_rd_Proj(NULL, sub, mode_Iu, pn_ia32_Sub_flags);
		new_node = new_bd_ia32_Jcc(dbgi, block, flags, pn_Cmp_Lt | ia32_pn_Cmp_unsigned);
		SET_IA32_ORIG_NODE(new_node, node);
	} else {
		panic("generic Bound not supported in ia32 Backend");
	}
	return new_node;
}


static ir_node *gen_ia32_l_ShlDep(ir_node *node)
{
	ir_node *left  = get_irn_n(node, n_ia32_l_ShlDep_val);
	ir_node *right = get_irn_n(node, n_ia32_l_ShlDep_count);

	return gen_shift_binop(node, left, right, new_bd_ia32_Shl,
	                       match_immediate | match_mode_neutral);
}

static ir_node *gen_ia32_l_ShrDep(ir_node *node)
{
	ir_node *left  = get_irn_n(node, n_ia32_l_ShrDep_val);
	ir_node *right = get_irn_n(node, n_ia32_l_ShrDep_count);
	return gen_shift_binop(node, left, right, new_bd_ia32_Shr,
	                       match_immediate);
}

static ir_node *gen_ia32_l_SarDep(ir_node *node)
{
	ir_node *left  = get_irn_n(node, n_ia32_l_SarDep_val);
	ir_node *right = get_irn_n(node, n_ia32_l_SarDep_count);
	return gen_shift_binop(node, left, right, new_bd_ia32_Sar,
	                       match_immediate);
}

static ir_node *gen_ia32_l_Add(ir_node *node)
{
	ir_node *left    = get_irn_n(node, n_ia32_l_Add_left);
	ir_node *right   = get_irn_n(node, n_ia32_l_Add_right);
	ir_node *lowered = gen_binop(node, left, right, new_bd_ia32_Add,
			match_commutative | match_am | match_immediate |
			match_mode_neutral);

	if (is_Proj(lowered)) {
		lowered	= get_Proj_pred(lowered);
	} else {
		assert(is_ia32_Add(lowered));
		set_irn_mode(lowered, mode_T);
	}

	return lowered;
}

static ir_node *gen_ia32_l_Adc(ir_node *node)
{
	return gen_binop_flags(node, new_bd_ia32_Adc,
			match_commutative | match_am | match_immediate |
			match_mode_neutral);
}

/**
 * Transforms a l_MulS into a "real" MulS node.
 *
 * @return the created ia32 Mul node
 */
static ir_node *gen_ia32_l_Mul(ir_node *node)
{
	ir_node *left  = get_binop_left(node);
	ir_node *right = get_binop_right(node);

	return gen_binop(node, left, right, new_bd_ia32_Mul,
	                 match_commutative | match_am | match_mode_neutral);
}

/**
 * Transforms a l_IMulS into a "real" IMul1OPS node.
 *
 * @return the created ia32 IMul1OP node
 */
static ir_node *gen_ia32_l_IMul(ir_node *node)
{
	ir_node  *left  = get_binop_left(node);
	ir_node  *right = get_binop_right(node);

	return gen_binop(node, left, right, new_bd_ia32_IMul1OP,
	                 match_commutative | match_am | match_mode_neutral);
}

static ir_node *gen_ia32_l_Sub(ir_node *node)
{
	ir_node *left    = get_irn_n(node, n_ia32_l_Sub_minuend);
	ir_node *right   = get_irn_n(node, n_ia32_l_Sub_subtrahend);
	ir_node *lowered = gen_binop(node, left, right, new_bd_ia32_Sub,
			match_am | match_immediate | match_mode_neutral);

	if (is_Proj(lowered)) {
		lowered	= get_Proj_pred(lowered);
	} else {
		assert(is_ia32_Sub(lowered));
		set_irn_mode(lowered, mode_T);
	}

	return lowered;
}

static ir_node *gen_ia32_l_Sbb(ir_node *node)
{
	return gen_binop_flags(node, new_bd_ia32_Sbb,
			match_am | match_immediate | match_mode_neutral);
}

/**
 * Transforms a l_ShlD/l_ShrD into a ShlD/ShrD. Those nodes have 3 data inputs:
 * op1 - target to be shifted
 * op2 - contains bits to be shifted into target
 * op3 - shift count
 * Only op3 can be an immediate.
 */
static ir_node *gen_lowered_64bit_shifts(ir_node *node, ir_node *high,
                                         ir_node *low, ir_node *count)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *new_high  = be_transform_node(high);
	ir_node  *new_low   = be_transform_node(low);
	ir_node  *new_count;
	ir_node  *new_node;

	/* the shift amount can be any mode that is bigger than 5 bits, since all
	 * other bits are ignored anyway */
	while (is_Conv(count)              &&
	       get_irn_n_edges(count) == 1 &&
	       mode_is_int(get_irn_mode(count))) {
		assert(get_mode_size_bits(get_irn_mode(count)) >= 5);
		count = get_Conv_op(count);
	}
	new_count = create_immediate_or_transform(count, 0);

	if (is_ia32_l_ShlD(node)) {
		new_node = new_bd_ia32_ShlD(dbgi, new_block, new_high, new_low,
		                            new_count);
	} else {
		new_node = new_bd_ia32_ShrD(dbgi, new_block, new_high, new_low,
		                            new_count);
	}
	SET_IA32_ORIG_NODE(new_node, node);

	return new_node;
}

static ir_node *gen_ia32_l_ShlD(ir_node *node)
{
	ir_node *high  = get_irn_n(node, n_ia32_l_ShlD_val_high);
	ir_node *low   = get_irn_n(node, n_ia32_l_ShlD_val_low);
	ir_node *count = get_irn_n(node, n_ia32_l_ShlD_count);
	return gen_lowered_64bit_shifts(node, high, low, count);
}

static ir_node *gen_ia32_l_ShrD(ir_node *node)
{
	ir_node *high  = get_irn_n(node, n_ia32_l_ShrD_val_high);
	ir_node *low   = get_irn_n(node, n_ia32_l_ShrD_val_low);
	ir_node *count = get_irn_n(node, n_ia32_l_ShrD_count);
	return gen_lowered_64bit_shifts(node, high, low, count);
}

static ir_node *gen_ia32_l_LLtoFloat(ir_node *node)
{
	ir_node  *src_block    = get_nodes_block(node);
	ir_node  *block        = be_transform_node(src_block);
	ir_graph *irg          = current_ir_graph;
	dbg_info *dbgi         = get_irn_dbg_info(node);
	ir_node  *frame        = get_irg_frame(irg);
	ir_node  *val_low      = get_irn_n(node, n_ia32_l_LLtoFloat_val_low);
	ir_node  *val_high     = get_irn_n(node, n_ia32_l_LLtoFloat_val_high);
	ir_node  *new_val_low  = be_transform_node(val_low);
	ir_node  *new_val_high = be_transform_node(val_high);
	ir_node  *in[2];
	ir_node  *sync, *fild, *res;
	ir_node  *store_low, *store_high;

	if (ia32_cg_config.use_sse2) {
		panic("ia32_l_LLtoFloat not implemented for SSE2");
	}

	/* do a store */
	store_low = new_bd_ia32_Store(dbgi, block, frame, noreg_GP, nomem,
	                              new_val_low);
	store_high = new_bd_ia32_Store(dbgi, block, frame, noreg_GP, nomem,
	                               new_val_high);
	SET_IA32_ORIG_NODE(store_low,  node);
	SET_IA32_ORIG_NODE(store_high, node);

	set_ia32_use_frame(store_low);
	set_ia32_use_frame(store_high);
	set_ia32_op_type(store_low, ia32_AddrModeD);
	set_ia32_op_type(store_high, ia32_AddrModeD);
	set_ia32_ls_mode(store_low, mode_Iu);
	set_ia32_ls_mode(store_high, mode_Is);
	add_ia32_am_offs_int(store_high, 4);

	in[0] = store_low;
	in[1] = store_high;
	sync  = new_rd_Sync(dbgi, block, 2, in);

	/* do a fild */
	fild = new_bd_ia32_vfild(dbgi, block, frame, noreg_GP, sync);

	set_ia32_use_frame(fild);
	set_ia32_op_type(fild, ia32_AddrModeS);
	set_ia32_ls_mode(fild, mode_Ls);

	SET_IA32_ORIG_NODE(fild, node);

	res = new_r_Proj(fild, mode_vfp, pn_ia32_vfild_res);

	if (! mode_is_signed(get_irn_mode(val_high))) {
		ia32_address_mode_t  am;

		ir_node *count = ia32_create_Immediate(NULL, 0, 31);
		ir_node *fadd;

		am.addr.base          = get_symconst_base();
		am.addr.index         = new_bd_ia32_Shr(dbgi, block, new_val_high, count);
		am.addr.mem           = nomem;
		am.addr.offset        = 0;
		am.addr.scale         = 2;
		am.addr.symconst_ent  = ia32_gen_fp_known_const(ia32_ULLBIAS);
		am.addr.use_frame     = 0;
		am.addr.frame_entity  = NULL;
		am.addr.symconst_sign = 0;
		am.ls_mode            = mode_F;
		am.mem_proj           = nomem;
		am.op_type            = ia32_AddrModeS;
		am.new_op1            = res;
		am.new_op2            = ia32_new_NoReg_vfp(env_cg);
		am.pinned             = op_pin_state_floats;
		am.commutative        = 1;
		am.ins_permuted       = 0;

		fadd  = new_bd_ia32_vfadd(dbgi, block, am.addr.base, am.addr.index, am.addr.mem,
			am.new_op1, am.new_op2, get_fpcw());
		set_am_attributes(fadd, &am);

		set_irn_mode(fadd, mode_T);
		res = new_rd_Proj(NULL, fadd, mode_vfp, pn_ia32_res);
	}
	return res;
}

static ir_node *gen_ia32_l_FloattoLL(ir_node *node)
{
	ir_node  *src_block  = get_nodes_block(node);
	ir_node  *block      = be_transform_node(src_block);
	ir_graph *irg        = get_Block_irg(block);
	dbg_info *dbgi       = get_irn_dbg_info(node);
	ir_node  *frame      = get_irg_frame(irg);
	ir_node  *val        = get_irn_n(node, n_ia32_l_FloattoLL_val);
	ir_node  *new_val    = be_transform_node(val);
	ir_node  *fist, *mem;

	mem = gen_vfist(dbgi, block, frame, noreg_GP, nomem, new_val, &fist);
	SET_IA32_ORIG_NODE(fist, node);
	set_ia32_use_frame(fist);
	set_ia32_op_type(fist, ia32_AddrModeD);
	set_ia32_ls_mode(fist, mode_Ls);

	return mem;
}

/**
 * the BAD transformer.
 */
static ir_node *bad_transform(ir_node *node)
{
	panic("No transform function for %+F available.", node);
}

static ir_node *gen_Proj_l_FloattoLL(ir_node *node)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_graph *irg      = get_Block_irg(block);
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	ir_node  *frame    = get_irg_frame(irg);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long      pn       = get_Proj_proj(node);
	ir_node  *load;
	ir_node  *proj;
	ia32_attr_t *attr;

	load = new_bd_ia32_Load(dbgi, block, frame, noreg_GP, new_pred);
	SET_IA32_ORIG_NODE(load, node);
	set_ia32_use_frame(load);
	set_ia32_op_type(load, ia32_AddrModeS);
	set_ia32_ls_mode(load, mode_Iu);
	/* we need a 64bit stackslot (fist stores 64bit) even though we only load
	 * 32 bit from it with this particular load */
	attr = get_ia32_attr(load);
	attr->data.need_64bit_stackent = 1;

	if (pn == pn_ia32_l_FloattoLL_res_high) {
		add_ia32_am_offs_int(load, 4);
	} else {
		assert(pn == pn_ia32_l_FloattoLL_res_low);
	}

	proj = new_r_Proj(load, mode_Iu, pn_ia32_Load_res);

	return proj;
}

/**
 * Transform the Projs of an AddSP.
 */
static ir_node *gen_Proj_be_AddSP(ir_node *node)
{
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	if (proj == pn_be_AddSP_sp) {
		ir_node *res = new_rd_Proj(dbgi, new_pred, mode_Iu,
		                           pn_ia32_SubSP_stack);
		arch_set_irn_register(res, &ia32_gp_regs[REG_ESP]);
		return res;
	} else if (proj == pn_be_AddSP_res) {
		return new_rd_Proj(dbgi, new_pred, mode_Iu,
		                   pn_ia32_SubSP_addr);
	} else if (proj == pn_be_AddSP_M) {
		return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_SubSP_M);
	}

	panic("No idea how to transform proj->AddSP");
}

/**
 * Transform the Projs of a SubSP.
 */
static ir_node *gen_Proj_be_SubSP(ir_node *node)
{
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	if (proj == pn_be_SubSP_sp) {
		ir_node *res = new_rd_Proj(dbgi, new_pred, mode_Iu,
		                           pn_ia32_AddSP_stack);
		arch_set_irn_register(res, &ia32_gp_regs[REG_ESP]);
		return res;
	} else if (proj == pn_be_SubSP_M) {
		return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_AddSP_M);
	}

	panic("No idea how to transform proj->SubSP");
}

/**
 * Transform and renumber the Projs from a Load.
 */
static ir_node *gen_Proj_Load(ir_node *node)
{
	ir_node  *new_pred;
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	/* loads might be part of source address mode matches, so we don't
	 * transform the ProjMs yet (with the exception of loads whose result is
	 * not used)
	 */
	if (is_Load(pred) && proj == pn_Load_M && get_irn_n_edges(pred) > 1) {
		ir_node *res;

		/* this is needed, because sometimes we have loops that are only
		   reachable through the ProjM */
		be_enqueue_preds(node);
		/* do it in 2 steps, to silence firm verifier */
		res = new_rd_Proj(dbgi, pred, mode_M, pn_Load_M);
		set_Proj_proj(res, pn_ia32_mem);
		return res;
	}

	/* renumber the proj */
	new_pred = be_transform_node(pred);
	if (is_ia32_Load(new_pred)) {
		switch (proj) {
		case pn_Load_res:
			return new_rd_Proj(dbgi, new_pred, mode_Iu, pn_ia32_Load_res);
		case pn_Load_M:
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_Load_M);
		case pn_Load_X_regular:
			return new_rd_Jmp(dbgi, block);
		case pn_Load_X_except:
			/* This Load might raise an exception. Mark it. */
			set_ia32_exc_label(new_pred, 1);
			return new_rd_Proj(dbgi, new_pred, mode_X, pn_ia32_Load_X_exc);
		default:
			break;
		}
	} else if (is_ia32_Conv_I2I(new_pred) ||
	           is_ia32_Conv_I2I8Bit(new_pred)) {
		set_irn_mode(new_pred, mode_T);
		if (proj == pn_Load_res) {
			return new_rd_Proj(dbgi, new_pred, mode_Iu, pn_ia32_res);
		} else if (proj == pn_Load_M) {
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_mem);
		}
	} else if (is_ia32_xLoad(new_pred)) {
		switch (proj) {
		case pn_Load_res:
			return new_rd_Proj(dbgi, new_pred, mode_xmm, pn_ia32_xLoad_res);
		case pn_Load_M:
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_xLoad_M);
		case pn_Load_X_regular:
			return new_rd_Jmp(dbgi, block);
		case pn_Load_X_except:
			/* This Load might raise an exception. Mark it. */
			set_ia32_exc_label(new_pred, 1);
			return new_rd_Proj(dbgi, new_pred, mode_X, pn_ia32_xLoad_X_exc);
		default:
			break;
		}
	} else if (is_ia32_vfld(new_pred)) {
		switch (proj) {
		case pn_Load_res:
			return new_rd_Proj(dbgi, new_pred, mode_vfp, pn_ia32_vfld_res);
		case pn_Load_M:
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_vfld_M);
		case pn_Load_X_regular:
			return new_rd_Jmp(dbgi, block);
		case pn_Load_X_except:
			/* This Load might raise an exception. Mark it. */
			set_ia32_exc_label(new_pred, 1);
			return new_rd_Proj(dbgi, new_pred, mode_X, pn_ia32_vfld_X_exc);
		default:
			break;
		}
	} else {
		/* can happen for ProJMs when source address mode happened for the
		   node */

		/* however it should not be the result proj, as that would mean the
		   load had multiple users and should not have been used for
		   SourceAM */
		if (proj != pn_Load_M) {
			panic("internal error: transformed node not a Load");
		}
		return new_rd_Proj(dbgi, new_pred, mode_M, 1);
	}

	panic("No idea how to transform proj");
}

/**
 * Transform and renumber the Projs from a DivMod like instruction.
 */
static ir_node *gen_Proj_DivMod(ir_node *node)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	assert(is_ia32_Div(new_pred) || is_ia32_IDiv(new_pred));

	switch (get_irn_opcode(pred)) {
	case iro_Div:
		switch (proj) {
		case pn_Div_M:
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_Div_M);
		case pn_Div_res:
			return new_rd_Proj(dbgi, new_pred, mode_Iu, pn_ia32_Div_div_res);
		case pn_Div_X_regular:
			return new_rd_Jmp(dbgi, block);
		case pn_Div_X_except:
			set_ia32_exc_label(new_pred, 1);
			return new_rd_Proj(dbgi, new_pred, mode_X, pn_ia32_Div_X_exc);
		default:
			break;
		}
		break;
	case iro_Mod:
		switch (proj) {
		case pn_Mod_M:
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_Div_M);
		case pn_Mod_res:
			return new_rd_Proj(dbgi, new_pred, mode_Iu, pn_ia32_Div_mod_res);
		case pn_Mod_X_except:
			set_ia32_exc_label(new_pred, 1);
			return new_rd_Proj(dbgi, new_pred, mode_X, pn_ia32_Div_X_exc);
		default:
			break;
		}
		break;
	case iro_DivMod:
		switch (proj) {
		case pn_DivMod_M:
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_Div_M);
		case pn_DivMod_res_div:
			return new_rd_Proj(dbgi, new_pred, mode_Iu, pn_ia32_Div_div_res);
		case pn_DivMod_res_mod:
			return new_rd_Proj(dbgi, new_pred, mode_Iu, pn_ia32_Div_mod_res);
		case pn_DivMod_X_regular:
			return new_rd_Jmp(dbgi, block);
		case pn_DivMod_X_except:
			set_ia32_exc_label(new_pred, 1);
			return new_rd_Proj(dbgi, new_pred, mode_X, pn_ia32_Div_X_exc);
		default:
			break;
		}
		break;
	default:
		break;
	}

	panic("No idea how to transform proj->DivMod");
}

/**
 * Transform and renumber the Projs from a CopyB.
 */
static ir_node *gen_Proj_CopyB(ir_node *node)
{
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	switch (proj) {
	case pn_CopyB_M_regular:
		if (is_ia32_CopyB_i(new_pred)) {
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_CopyB_i_M);
		} else if (is_ia32_CopyB(new_pred)) {
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_CopyB_M);
		}
		break;
	default:
		break;
	}

	panic("No idea how to transform proj->CopyB");
}

/**
 * Transform and renumber the Projs from a Quot.
 */
static ir_node *gen_Proj_Quot(ir_node *node)
{
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	switch (proj) {
	case pn_Quot_M:
		if (is_ia32_xDiv(new_pred)) {
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_xDiv_M);
		} else if (is_ia32_vfdiv(new_pred)) {
			return new_rd_Proj(dbgi, new_pred, mode_M, pn_ia32_vfdiv_M);
		}
		break;
	case pn_Quot_res:
		if (is_ia32_xDiv(new_pred)) {
			return new_rd_Proj(dbgi, new_pred, mode_xmm, pn_ia32_xDiv_res);
		} else if (is_ia32_vfdiv(new_pred)) {
			return new_rd_Proj(dbgi, new_pred, mode_vfp, pn_ia32_vfdiv_res);
		}
		break;
	case pn_Quot_X_regular:
	case pn_Quot_X_except:
	default:
		break;
	}

	panic("No idea how to transform proj->Quot");
}

static ir_node *gen_be_Call(ir_node *node)
{
	dbg_info       *const dbgi      = get_irn_dbg_info(node);
	ir_node        *const src_block = get_nodes_block(node);
	ir_node        *const block     = be_transform_node(src_block);
	ir_node        *const src_mem   = get_irn_n(node, be_pos_Call_mem);
	ir_node        *const src_sp    = get_irn_n(node, be_pos_Call_sp);
	ir_node        *const sp        = be_transform_node(src_sp);
	ir_node        *const src_ptr   = get_irn_n(node, be_pos_Call_ptr);
	ia32_address_mode_t   am;
	ia32_address_t *const addr      = &am.addr;
	ir_node        *      mem;
	ir_node        *      call;
	int                   i;
	ir_node        *      fpcw;
	ir_node        *      eax       = noreg_GP;
	ir_node        *      ecx       = noreg_GP;
	ir_node        *      edx       = noreg_GP;
	unsigned        const pop       = be_Call_get_pop(node);
	ir_type        *const call_tp   = be_Call_get_type(node);
	int                   old_no_pic_adjust;

	/* Run the x87 simulator if the call returns a float value */
	if (get_method_n_ress(call_tp) > 0) {
		ir_type *const res_type = get_method_res_type(call_tp, 0);
		ir_mode *const res_mode = get_type_mode(res_type);

		if (res_mode != NULL && mode_is_float(res_mode)) {
			env_cg->do_x87_sim = 1;
		}
	}

	/* We do not want be_Call direct calls */
	assert(be_Call_get_entity(node) == NULL);

	/* special case for PIC trampoline calls */
	old_no_pic_adjust = no_pic_adjust;
	no_pic_adjust     = env_cg->birg->main_env->options->pic;

	match_arguments(&am, src_block, NULL, src_ptr, src_mem,
	                match_am | match_immediate);

	no_pic_adjust = old_no_pic_adjust;

	i    = get_irn_arity(node) - 1;
	fpcw = be_transform_node(get_irn_n(node, i--));
	for (; i >= be_pos_Call_first_arg; --i) {
		arch_register_req_t const *const req = arch_get_register_req(node, i);
		ir_node *const reg_parm = be_transform_node(get_irn_n(node, i));

		assert(req->type == arch_register_req_type_limited);
		assert(req->cls == &ia32_reg_classes[CLASS_ia32_gp]);

		switch (*req->limited) {
			case 1 << REG_EAX: assert(eax == noreg_GP); eax = reg_parm; break;
			case 1 << REG_ECX: assert(ecx == noreg_GP); ecx = reg_parm; break;
			case 1 << REG_EDX: assert(edx == noreg_GP); edx = reg_parm; break;
			default: panic("Invalid GP register for register parameter");
		}
	}

	mem  = transform_AM_mem(block, src_ptr, src_mem, addr->mem);
	call = new_bd_ia32_Call(dbgi, block, addr->base, addr->index, mem,
	                        am.new_op2, sp, fpcw, eax, ecx, edx, pop, call_tp);
	set_am_attributes(call, &am);
	call = fix_mem_proj(call, &am);

	if (get_irn_pinned(node) == op_pin_state_pinned)
		set_irn_pinned(call, op_pin_state_pinned);

	SET_IA32_ORIG_NODE(call, node);

	if (ia32_cg_config.use_sse2) {
		/* remember this call for post-processing */
		ARR_APP1(ir_node *, call_list, call);
		ARR_APP1(ir_type *, call_types, be_Call_get_type(node));
	}

	return call;
}

/**
 * Transform Builtin trap
 */
static ir_node *gen_trap(ir_node *node)
{
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_node *block  = be_transform_node(get_nodes_block(node));
	ir_node *mem    = be_transform_node(get_Builtin_mem(node));

	return new_bd_ia32_UD2(dbgi, block, mem);
}

/**
 * Transform Builtin debugbreak
 */
static ir_node *gen_debugbreak(ir_node *node)
{
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_node *block  = be_transform_node(get_nodes_block(node));
	ir_node *mem    = be_transform_node(get_Builtin_mem(node));

	return new_bd_ia32_Breakpoint(dbgi, block, mem);
}

/**
 * Transform Builtin return_address
 */
static ir_node *gen_return_address(ir_node *node)
{
	ir_node *param      = get_Builtin_param(node, 0);
	ir_node *frame      = get_Builtin_param(node, 1);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	tarval  *tv         = get_Const_tarval(param);
	unsigned long value = get_tarval_long(tv);

	ir_node *block  = be_transform_node(get_nodes_block(node));
	ir_node *ptr    = be_transform_node(frame);
	ir_node *load;

	if (value > 0) {
		ir_node *cnt = new_bd_ia32_ProduceVal(dbgi, block);
		ir_node *res = new_bd_ia32_ProduceVal(dbgi, block);
		ptr = new_bd_ia32_ClimbFrame(dbgi, block, ptr, cnt, res, value);
	}

	/* load the return address from this frame */
	load = new_bd_ia32_Load(dbgi, block, ptr, noreg_GP, nomem);

	set_irn_pinned(load, get_irn_pinned(node));
	set_ia32_op_type(load, ia32_AddrModeS);
	set_ia32_ls_mode(load, mode_Iu);

	set_ia32_am_offs_int(load, 0);
	set_ia32_use_frame(load);
	set_ia32_frame_ent(load, ia32_get_return_address_entity());

	if (get_irn_pinned(node) == op_pin_state_floats) {
		assert(pn_ia32_xLoad_res == pn_ia32_vfld_res
				&& pn_ia32_vfld_res == pn_ia32_Load_res
				&& pn_ia32_Load_res == pn_ia32_res);
		arch_irn_add_flags(load, arch_irn_flags_rematerializable);
	}

	SET_IA32_ORIG_NODE(load, node);
	return new_r_Proj(load, mode_Iu, pn_ia32_Load_res);
}

/**
 * Transform Builtin frame_address
 */
static ir_node *gen_frame_address(ir_node *node)
{
	ir_node *param      = get_Builtin_param(node, 0);
	ir_node *frame      = get_Builtin_param(node, 1);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	tarval  *tv         = get_Const_tarval(param);
	unsigned long value = get_tarval_long(tv);

	ir_node *block  = be_transform_node(get_nodes_block(node));
	ir_node *ptr    = be_transform_node(frame);
	ir_node *load;
	ir_entity *ent;

	if (value > 0) {
		ir_node *cnt = new_bd_ia32_ProduceVal(dbgi, block);
		ir_node *res = new_bd_ia32_ProduceVal(dbgi, block);
		ptr = new_bd_ia32_ClimbFrame(dbgi, block, ptr, cnt, res, value);
	}

	/* load the frame address from this frame */
	load = new_bd_ia32_Load(dbgi, block, ptr, noreg_GP, nomem);

	set_irn_pinned(load, get_irn_pinned(node));
	set_ia32_op_type(load, ia32_AddrModeS);
	set_ia32_ls_mode(load, mode_Iu);

	ent = ia32_get_frame_address_entity();
	if (ent != NULL) {
		set_ia32_am_offs_int(load, 0);
		set_ia32_use_frame(load);
		set_ia32_frame_ent(load, ent);
	} else {
		/* will fail anyway, but gcc does this: */
		set_ia32_am_offs_int(load, 0);
	}

	if (get_irn_pinned(node) == op_pin_state_floats) {
		assert(pn_ia32_xLoad_res == pn_ia32_vfld_res
				&& pn_ia32_vfld_res == pn_ia32_Load_res
				&& pn_ia32_Load_res == pn_ia32_res);
		arch_irn_add_flags(load, arch_irn_flags_rematerializable);
	}

	SET_IA32_ORIG_NODE(load, node);
	return new_r_Proj(load, mode_Iu, pn_ia32_Load_res);
}

/**
 * Transform Builtin frame_address
 */
static ir_node *gen_prefetch(ir_node *node)
{
	dbg_info       *dbgi;
	ir_node        *ptr, *block, *mem, *base, *index;
	ir_node        *param,  *new_node;
	long           rw, locality;
	tarval         *tv;
	ia32_address_t addr;

	if (!ia32_cg_config.use_sse_prefetch && !ia32_cg_config.use_3dnow_prefetch) {
		/* no prefetch at all, route memory */
		return be_transform_node(get_Builtin_mem(node));
	}

	param = get_Builtin_param(node, 1);
	tv    = get_Const_tarval(param);
	rw    = get_tarval_long(tv);

	/* construct load address */
	memset(&addr, 0, sizeof(addr));
	ptr = get_Builtin_param(node, 0);
	ia32_create_address_mode(&addr, ptr, 0);
	base  = addr.base;
	index = addr.index;

	if (base == NULL) {
		base = noreg_GP;
	} else {
		base = be_transform_node(base);
	}

	if (index == NULL) {
		index = noreg_GP;
	} else {
		index = be_transform_node(index);
	}

	dbgi     = get_irn_dbg_info(node);
	block    = be_transform_node(get_nodes_block(node));
	mem      = be_transform_node(get_Builtin_mem(node));

	if (rw == 1 && ia32_cg_config.use_3dnow_prefetch) {
		/* we have 3DNow!, this was already checked above */
		new_node = new_bd_ia32_PrefetchW(dbgi, block, base, index, mem);
	} else if (ia32_cg_config.use_sse_prefetch) {
		/* note: rw == 1 is IGNORED in that case */
		param    = get_Builtin_param(node, 2);
		tv       = get_Const_tarval(param);
		locality = get_tarval_long(tv);

		/* SSE style prefetch */
		switch (locality) {
		case 0:
			new_node = new_bd_ia32_PrefetchNTA(dbgi, block, base, index, mem);
			break;
		case 1:
			new_node = new_bd_ia32_Prefetch2(dbgi, block, base, index, mem);
			break;
		case 2:
			new_node = new_bd_ia32_Prefetch1(dbgi, block, base, index, mem);
			break;
		default:
			new_node = new_bd_ia32_Prefetch0(dbgi, block, base, index, mem);
			break;
		}
	} else {
		assert(ia32_cg_config.use_3dnow_prefetch);
		/* 3DNow! style prefetch */
		new_node = new_bd_ia32_Prefetch(dbgi, block, base, index, mem);
	}

	set_irn_pinned(new_node, get_irn_pinned(node));
	set_ia32_op_type(new_node, ia32_AddrModeS);
	set_ia32_ls_mode(new_node, mode_Bu);
	set_address(new_node, &addr);

	SET_IA32_ORIG_NODE(new_node, node);

	be_dep_on_frame(new_node);
	return new_r_Proj(new_node, mode_M, pn_ia32_Prefetch_M);
}

/**
 * Transform bsf like node
 */
static ir_node *gen_unop_AM(ir_node *node, construct_binop_dest_func *func)
{
	ir_node *param     = get_Builtin_param(node, 0);
	dbg_info *dbgi     = get_irn_dbg_info(node);

	ir_node *block     = get_nodes_block(node);
	ir_node *new_block = be_transform_node(block);

	ia32_address_mode_t  am;
	ia32_address_t      *addr = &am.addr;
	ir_node             *cnt;

	match_arguments(&am, block, NULL, param, NULL, match_am);

	cnt = func(dbgi, new_block, addr->base, addr->index, addr->mem, am.new_op2);
	set_am_attributes(cnt, &am);
	set_ia32_ls_mode(cnt, get_irn_mode(param));

	SET_IA32_ORIG_NODE(cnt, node);
	return fix_mem_proj(cnt, &am);
}

/**
 * Transform builtin ffs.
 */
static ir_node *gen_ffs(ir_node *node)
{
	ir_node  *bsf   = gen_unop_AM(node, new_bd_ia32_Bsf);
	ir_node  *real  = skip_Proj(bsf);
	dbg_info *dbgi  = get_irn_dbg_info(real);
	ir_node  *block = get_nodes_block(real);
	ir_node  *flag, *set, *conv, *neg, *or;

	/* bsf x */
	if (get_irn_mode(real) != mode_T) {
		set_irn_mode(real, mode_T);
		bsf = new_r_Proj(real, mode_Iu, pn_ia32_res);
	}

	flag = new_r_Proj(real, mode_b, pn_ia32_flags);

	/* sete */
	set = new_bd_ia32_Setcc(dbgi, block, flag, pn_Cmp_Eq);
	SET_IA32_ORIG_NODE(set, node);

	/* conv to 32bit */
	conv = new_bd_ia32_Conv_I2I8Bit(dbgi, block, noreg_GP, noreg_GP, nomem, set, mode_Bu);
	SET_IA32_ORIG_NODE(conv, node);

	/* neg */
	neg = new_bd_ia32_Neg(dbgi, block, conv);

	/* or */
	or = new_bd_ia32_Or(dbgi, block, noreg_GP, noreg_GP, nomem, bsf, neg);
	set_ia32_commutative(or);

	/* add 1 */
	return new_bd_ia32_Add(dbgi, block, noreg_GP, noreg_GP, nomem, or, ia32_create_Immediate(NULL, 0, 1));
}

/**
 * Transform builtin clz.
 */
static ir_node *gen_clz(ir_node *node)
{
	ir_node  *bsr   = gen_unop_AM(node, new_bd_ia32_Bsr);
	ir_node  *real  = skip_Proj(bsr);
	dbg_info *dbgi  = get_irn_dbg_info(real);
	ir_node  *block = get_nodes_block(real);
	ir_node  *imm   = ia32_create_Immediate(NULL, 0, 31);

	return new_bd_ia32_Xor(dbgi, block, noreg_GP, noreg_GP, nomem, bsr, imm);
}

/**
 * Transform builtin ctz.
 */
static ir_node *gen_ctz(ir_node *node)
{
	return gen_unop_AM(node, new_bd_ia32_Bsf);
}

/**
 * Transform builtin parity.
 */
static ir_node *gen_parity(ir_node *node)
{
	ir_node *param      = get_Builtin_param(node, 0);
	dbg_info *dbgi      = get_irn_dbg_info(node);

	ir_node *block      = get_nodes_block(node);

	ir_node *new_block  = be_transform_node(block);
	ir_node *imm, *cmp, *new_node;

	ia32_address_mode_t am;
	ia32_address_t      *addr = &am.addr;


	/* cmp param, 0 */
	match_arguments(&am, block, NULL, param, NULL, match_am);
	imm = ia32_create_Immediate(NULL, 0, 0);
	cmp = new_bd_ia32_Cmp(dbgi, new_block, addr->base, addr->index,
	                      addr->mem, imm, am.new_op2, am.ins_permuted, 0);
	set_am_attributes(cmp, &am);
	set_ia32_ls_mode(cmp, mode_Iu);

	SET_IA32_ORIG_NODE(cmp, node);

	cmp = fix_mem_proj(cmp, &am);

	/* setp */
	new_node = new_bd_ia32_Setcc(dbgi, new_block, cmp, ia32_pn_Cmp_parity);
	SET_IA32_ORIG_NODE(new_node, node);

	/* conv to 32bit */
	new_node = new_bd_ia32_Conv_I2I8Bit(dbgi, new_block, noreg_GP, noreg_GP,
	                                    nomem, new_node, mode_Bu);
	SET_IA32_ORIG_NODE(new_node, node);
	return new_node;
}

/**
 * Transform builtin popcount
 */
static ir_node *gen_popcount(ir_node *node)
{
	ir_node *param     = get_Builtin_param(node, 0);
	dbg_info *dbgi     = get_irn_dbg_info(node);

	ir_node *block     = get_nodes_block(node);
	ir_node *new_block = be_transform_node(block);

	ir_node *new_param;
	ir_node *imm, *simm, *m1, *s1, *s2, *s3, *s4, *s5, *m2, *m3, *m4, *m5, *m6, *m7, *m8, *m9, *m10, *m11, *m12, *m13;

	/* check for SSE4.2 or SSE4a and use the popcnt instruction */
	if (ia32_cg_config.use_popcnt) {
		ia32_address_mode_t am;
		ia32_address_t      *addr = &am.addr;
		ir_node             *cnt;

		match_arguments(&am, block, NULL, param, NULL, match_am | match_16bit_am);

		cnt = new_bd_ia32_Popcnt(dbgi, new_block, addr->base, addr->index, addr->mem, am.new_op2);
		set_am_attributes(cnt, &am);
		set_ia32_ls_mode(cnt, get_irn_mode(param));

		SET_IA32_ORIG_NODE(cnt, node);
		return fix_mem_proj(cnt, &am);
	}

	new_param = be_transform_node(param);

	/* do the standard popcount algo */

	/* m1 = x & 0x55555555 */
	imm = ia32_create_Immediate(NULL, 0, 0x55555555);
	m1 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, new_param, imm);

	/* s1 = x >> 1 */
	simm = ia32_create_Immediate(NULL, 0, 1);
	s1 = new_bd_ia32_Shl(dbgi, new_block, new_param, simm);

	/* m2 = s1 & 0x55555555 */
	m2 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, s1, imm);

	/* m3 = m1 + m2 */
	m3 = new_bd_ia32_Lea(dbgi, new_block, m2, m1);

	/* m4 = m3 & 0x33333333 */
	imm = ia32_create_Immediate(NULL, 0, 0x33333333);
	m4 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, m3, imm);

	/* s2 = m3 >> 2 */
	simm = ia32_create_Immediate(NULL, 0, 2);
	s2 = new_bd_ia32_Shl(dbgi, new_block, m3, simm);

	/* m5 = s2 & 0x33333333 */
	m5 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, s2, imm);

	/* m6 = m4 + m5 */
	m6 = new_bd_ia32_Lea(dbgi, new_block, m4, m5);

	/* m7 = m6 & 0x0F0F0F0F */
	imm = ia32_create_Immediate(NULL, 0, 0x0F0F0F0F);
	m7 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, m6, imm);

	/* s3 = m6 >> 4 */
	simm = ia32_create_Immediate(NULL, 0, 4);
	s3 = new_bd_ia32_Shl(dbgi, new_block, m6, simm);

	/* m8 = s3 & 0x0F0F0F0F */
	m8 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, s3, imm);

	/* m9 = m7 + m8 */
	m9 = new_bd_ia32_Lea(dbgi, new_block, m7, m8);

	/* m10 = m9 & 0x00FF00FF */
	imm = ia32_create_Immediate(NULL, 0, 0x00FF00FF);
	m10 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, m9, imm);

	/* s4 = m9 >> 8 */
	simm = ia32_create_Immediate(NULL, 0, 8);
	s4 = new_bd_ia32_Shl(dbgi, new_block, m9, simm);

	/* m11 = s4 & 0x00FF00FF */
	m11 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, s4, imm);

	/* m12 = m10 + m11 */
	m12 = new_bd_ia32_Lea(dbgi, new_block, m10, m11);

	/* m13 = m12 & 0x0000FFFF */
	imm = ia32_create_Immediate(NULL, 0, 0x0000FFFF);
	m13 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, m12, imm);

	/* s5 = m12 >> 16 */
	simm = ia32_create_Immediate(NULL, 0, 16);
	s5 = new_bd_ia32_Shl(dbgi, new_block, m12, simm);

	/* res = m13 + s5 */
	return new_bd_ia32_Lea(dbgi, new_block, m13, s5);
}

/**
 * Transform builtin byte swap.
 */
static ir_node *gen_bswap(ir_node *node)
{
	ir_node *param     = be_transform_node(get_Builtin_param(node, 0));
	dbg_info *dbgi     = get_irn_dbg_info(node);

	ir_node *block     = get_nodes_block(node);
	ir_node *new_block = be_transform_node(block);
	ir_mode *mode      = get_irn_mode(param);
	unsigned size      = get_mode_size_bits(mode);
	ir_node  *m1, *m2, *m3, *m4, *s1, *s2, *s3, *s4;

	switch (size) {
	case 32:
		if (ia32_cg_config.use_i486) {
			/* swap available */
			return new_bd_ia32_Bswap(dbgi, new_block, param);
		}
		s1 = new_bd_ia32_Shl(dbgi, new_block, param, ia32_create_Immediate(NULL, 0, 24));
		s2 = new_bd_ia32_Shl(dbgi, new_block, param, ia32_create_Immediate(NULL, 0, 8));

		m1 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, s2, ia32_create_Immediate(NULL, 0, 0xFF00));
		m2 = new_bd_ia32_Lea(dbgi, new_block, s1, m1);

		s3 = new_bd_ia32_Shr(dbgi, new_block, param, ia32_create_Immediate(NULL, 0, 8));

		m3 = new_bd_ia32_And(dbgi, new_block, noreg_GP, noreg_GP, nomem, s3, ia32_create_Immediate(NULL, 0, 0xFF0000));
		m4 = new_bd_ia32_Lea(dbgi, new_block, m2, m3);

		s4 = new_bd_ia32_Shr(dbgi, new_block, param, ia32_create_Immediate(NULL, 0, 24));
		return new_bd_ia32_Lea(dbgi, new_block, m4, s4);

	case 16:
		/* swap16 always available */
		return new_bd_ia32_Bswap16(dbgi, new_block, param);

	default:
		panic("Invalid bswap size (%d)", size);
	}
}

/**
 * Transform builtin outport.
 */
static ir_node *gen_outport(ir_node *node)
{
	ir_node *port  = create_immediate_or_transform(get_Builtin_param(node, 0), 0);
	ir_node *oldv  = get_Builtin_param(node, 1);
	ir_mode *mode  = get_irn_mode(oldv);
	ir_node *value = be_transform_node(oldv);
	ir_node *block = be_transform_node(get_nodes_block(node));
	ir_node *mem   = be_transform_node(get_Builtin_mem(node));
	dbg_info *dbgi = get_irn_dbg_info(node);

	ir_node *res = new_bd_ia32_Outport(dbgi, block, port, value, mem);
	set_ia32_ls_mode(res, mode);
	return res;
}

/**
 * Transform builtin inport.
 */
static ir_node *gen_inport(ir_node *node)
{
	ir_type *tp    = get_Builtin_type(node);
	ir_type *rstp  = get_method_res_type(tp, 0);
	ir_mode *mode  = get_type_mode(rstp);
	ir_node *port  = create_immediate_or_transform(get_Builtin_param(node, 0), 0);
	ir_node *block = be_transform_node(get_nodes_block(node));
	ir_node *mem   = be_transform_node(get_Builtin_mem(node));
	dbg_info *dbgi = get_irn_dbg_info(node);

	ir_node *res = new_bd_ia32_Inport(dbgi, block, port, mem);
	set_ia32_ls_mode(res, mode);

	/* check for missing Result Proj */
	return res;
}

/**
 * Transform a builtin inner trampoline
 */
static ir_node *gen_inner_trampoline(ir_node *node)
{
	ir_node  *ptr       = get_Builtin_param(node, 0);
	ir_node  *callee    = get_Builtin_param(node, 1);
	ir_node  *env       = be_transform_node(get_Builtin_param(node, 2));
	ir_node  *mem       = get_Builtin_mem(node);
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	ir_node  *val;
	ir_node  *store;
	ir_node  *rel;
	ir_node  *trampoline;
	ir_node  *in[2];
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ia32_address_t addr;

	/* construct store address */
	memset(&addr, 0, sizeof(addr));
	ia32_create_address_mode(&addr, ptr, 0);

	if (addr.base == NULL) {
		addr.base = noreg_GP;
	} else {
		addr.base = be_transform_node(addr.base);
	}

	if (addr.index == NULL) {
		addr.index = noreg_GP;
	} else {
		addr.index = be_transform_node(addr.index);
	}
	addr.mem = be_transform_node(mem);

	/* mov  ecx, <env> */
	val   = ia32_create_Immediate(NULL, 0, 0xB9);
	store = new_bd_ia32_Store8Bit(dbgi, new_block, addr.base,
	                              addr.index, addr.mem, val);
	set_irn_pinned(store, get_irn_pinned(node));
	set_ia32_op_type(store, ia32_AddrModeD);
	set_ia32_ls_mode(store, mode_Bu);
	set_address(store, &addr);
	addr.mem = store;
	addr.offset += 1;

	store = new_bd_ia32_Store(dbgi, new_block, addr.base,
	                          addr.index, addr.mem, env);
	set_irn_pinned(store, get_irn_pinned(node));
	set_ia32_op_type(store, ia32_AddrModeD);
	set_ia32_ls_mode(store, mode_Iu);
	set_address(store, &addr);
	addr.mem = store;
	addr.offset += 4;

	/* jmp rel <callee> */
	val   = ia32_create_Immediate(NULL, 0, 0xE9);
	store = new_bd_ia32_Store8Bit(dbgi, new_block, addr.base,
	                             addr.index, addr.mem, val);
	set_irn_pinned(store, get_irn_pinned(node));
	set_ia32_op_type(store, ia32_AddrModeD);
	set_ia32_ls_mode(store, mode_Bu);
	set_address(store, &addr);
	addr.mem = store;
	addr.offset += 1;

	trampoline = be_transform_node(ptr);

	/* the callee is typically an immediate */
	if (is_SymConst(callee)) {
		rel = new_bd_ia32_Const(dbgi, new_block, get_SymConst_entity(callee), 0, 0, -10);
	} else {
		rel = new_bd_ia32_Lea(dbgi, new_block, be_transform_node(callee), ia32_create_Immediate(NULL, 0, -10));
	}
	rel = new_bd_ia32_Sub(dbgi, new_block, noreg_GP, noreg_GP, nomem, rel, trampoline);

	store = new_bd_ia32_Store(dbgi, new_block, addr.base,
	                          addr.index, addr.mem, rel);
	set_irn_pinned(store, get_irn_pinned(node));
	set_ia32_op_type(store, ia32_AddrModeD);
	set_ia32_ls_mode(store, mode_Iu);
	set_address(store, &addr);

	in[0] = store;
	in[1] = trampoline;

	return new_r_Tuple(new_block, 2, in);
}

/**
 * Transform Builtin node.
 */
static ir_node *gen_Builtin(ir_node *node)
{
	ir_builtin_kind kind = get_Builtin_kind(node);

	switch (kind) {
	case ir_bk_trap:
		return gen_trap(node);
	case ir_bk_debugbreak:
		return gen_debugbreak(node);
	case ir_bk_return_address:
		return gen_return_address(node);
	case ir_bk_frame_address:
		return gen_frame_address(node);
	case ir_bk_prefetch:
		return gen_prefetch(node);
	case ir_bk_ffs:
		return gen_ffs(node);
	case ir_bk_clz:
		return gen_clz(node);
	case ir_bk_ctz:
		return gen_ctz(node);
	case ir_bk_parity:
		return gen_parity(node);
	case ir_bk_popcount:
		return gen_popcount(node);
	case ir_bk_bswap:
		return gen_bswap(node);
	case ir_bk_outport:
		return gen_outport(node);
	case ir_bk_inport:
		return gen_inport(node);
	case ir_bk_inner_trampoline:
		return gen_inner_trampoline(node);
	}
	panic("Builtin %s not implemented in IA32", get_builtin_kind_name(kind));
}

/**
 * Transform Proj(Builtin) node.
 */
static ir_node *gen_Proj_Builtin(ir_node *proj)
{
	ir_node         *node     = get_Proj_pred(proj);
	ir_node         *new_node = be_transform_node(node);
	ir_builtin_kind kind      = get_Builtin_kind(node);

	switch (kind) {
	case ir_bk_return_address:
	case ir_bk_frame_address:
	case ir_bk_ffs:
	case ir_bk_clz:
	case ir_bk_ctz:
	case ir_bk_parity:
	case ir_bk_popcount:
	case ir_bk_bswap:
		assert(get_Proj_proj(proj) == pn_Builtin_1_result);
		return new_node;
	case ir_bk_trap:
	case ir_bk_debugbreak:
	case ir_bk_prefetch:
	case ir_bk_outport:
		assert(get_Proj_proj(proj) == pn_Builtin_M);
		return new_node;
	case ir_bk_inport:
		if (get_Proj_proj(proj) == pn_Builtin_1_result) {
			return new_r_Proj(new_node, get_irn_mode(proj), pn_ia32_Inport_res);
		} else {
			assert(get_Proj_proj(proj) == pn_Builtin_M);
			return new_r_Proj(new_node, mode_M, pn_ia32_Inport_M);
		}
	case ir_bk_inner_trampoline:
		if (get_Proj_proj(proj) == pn_Builtin_1_result) {
			return get_Tuple_pred(new_node, 1);
		} else {
			assert(get_Proj_proj(proj) == pn_Builtin_M);
			return get_Tuple_pred(new_node, 0);
		}
	}
	panic("Builtin %s not implemented in IA32", get_builtin_kind_name(kind));
}

static ir_node *gen_be_IncSP(ir_node *node)
{
	ir_node *res = be_duplicate_node(node);
	arch_irn_add_flags(res, arch_irn_flags_modify_flags);

	return res;
}

/**
 * Transform the Projs from a be_Call.
 */
static ir_node *gen_Proj_be_Call(ir_node *node)
{
	ir_node  *call        = get_Proj_pred(node);
	ir_node  *new_call    = be_transform_node(call);
	dbg_info *dbgi        = get_irn_dbg_info(node);
	long      proj        = get_Proj_proj(node);
	ir_mode  *mode        = get_irn_mode(node);
	ir_node  *res;

	if (proj == pn_be_Call_M_regular) {
		return new_rd_Proj(dbgi, new_call, mode_M, n_ia32_Call_mem);
	}
	/* transform call modes */
	if (mode_is_data(mode)) {
		const arch_register_class_t *cls = arch_get_irn_reg_class_out(node);
		mode = cls->mode;
	}

	/* Map from be_Call to ia32_Call proj number */
	if (proj == pn_be_Call_sp) {
		proj = pn_ia32_Call_stack;
	} else if (proj == pn_be_Call_M_regular) {
		proj = pn_ia32_Call_M;
	} else {
		arch_register_req_t const *const req    = arch_get_register_req_out(node);
		int                        const n_outs = arch_irn_get_n_outs(new_call);
		int                              i;

		assert(proj      >= pn_be_Call_first_res);
		assert(req->type & arch_register_req_type_limited);

		for (i = 0; i < n_outs; ++i) {
			arch_register_req_t const *const new_req
				= arch_get_out_register_req(new_call, i);

			if (!(new_req->type & arch_register_req_type_limited) ||
			    new_req->cls      != req->cls                     ||
			    *new_req->limited != *req->limited)
				continue;

			proj = i;
			break;
		}
		assert(i < n_outs);
	}

	res = new_rd_Proj(dbgi, new_call, mode, proj);

	/* TODO arch_set_irn_register() only operates on Projs, need variant with index */
	switch (proj) {
		case pn_ia32_Call_stack:
			arch_set_irn_register(res, &ia32_gp_regs[REG_ESP]);
			break;

		case pn_ia32_Call_fpcw:
			arch_set_irn_register(res, &ia32_fp_cw_regs[REG_FPCW]);
			break;
	}

	return res;
}

/**
 * Transform the Projs from a Cmp.
 */
static ir_node *gen_Proj_Cmp(ir_node *node)
{
	/* this probably means not all mode_b nodes were lowered... */
	panic("trying to directly transform Proj_Cmp %+F (mode_b not lowered?)",
	      node);
}

/**
 * Transform the Projs from a Bound.
 */
static ir_node *gen_Proj_Bound(ir_node *node)
{
	ir_node *new_node;
	ir_node *pred = get_Proj_pred(node);

	switch (get_Proj_proj(node)) {
	case pn_Bound_M:
		return be_transform_node(get_Bound_mem(pred));
	case pn_Bound_X_regular:
		new_node = be_transform_node(pred);
		return new_r_Proj(new_node, mode_X, pn_ia32_Jcc_true);
	case pn_Bound_X_except:
		new_node = be_transform_node(pred);
		return new_r_Proj(new_node, mode_X, pn_ia32_Jcc_false);
	case pn_Bound_res:
		return be_transform_node(get_Bound_index(pred));
	default:
		panic("unsupported Proj from Bound");
	}
}

static ir_node *gen_Proj_ASM(ir_node *node)
{
	ir_mode *mode     = get_irn_mode(node);
	ir_node *pred     = get_Proj_pred(node);
	ir_node *new_pred = be_transform_node(pred);
	long     pos      = get_Proj_proj(node);

	if (mode == mode_M) {
		pos = arch_irn_get_n_outs(new_pred)-1;
	} else if (mode_is_int(mode) || mode_is_reference(mode)) {
		mode = mode_Iu;
	} else if (mode_is_float(mode)) {
		mode = mode_E;
	} else {
		panic("unexpected proj mode at ASM");
	}

	return new_r_Proj(new_pred, mode, pos);
}

/**
 * Transform and potentially renumber Proj nodes.
 */
static ir_node *gen_Proj(ir_node *node)
{
	ir_node *pred = get_Proj_pred(node);
	long    proj;

	switch (get_irn_opcode(pred)) {
	case iro_Store:
		proj = get_Proj_proj(node);
		if (proj == pn_Store_M) {
			return be_transform_node(pred);
		} else {
			panic("No idea how to transform proj->Store");
		}
	case iro_Load:
		return gen_Proj_Load(node);
	case iro_ASM:
		return gen_Proj_ASM(node);
	case iro_Builtin:
		return gen_Proj_Builtin(node);
	case iro_Div:
	case iro_Mod:
	case iro_DivMod:
		return gen_Proj_DivMod(node);
	case iro_CopyB:
		return gen_Proj_CopyB(node);
	case iro_Quot:
		return gen_Proj_Quot(node);
	case beo_SubSP:
		return gen_Proj_be_SubSP(node);
	case beo_AddSP:
		return gen_Proj_be_AddSP(node);
	case beo_Call:
		return gen_Proj_be_Call(node);
	case iro_Cmp:
		return gen_Proj_Cmp(node);
	case iro_Bound:
		return gen_Proj_Bound(node);
	case iro_Start:
		proj = get_Proj_proj(node);
		switch (proj) {
			case pn_Start_X_initial_exec: {
				ir_node  *block     = get_nodes_block(pred);
				ir_node  *new_block = be_transform_node(block);
				dbg_info *dbgi      = get_irn_dbg_info(node);
				/* we exchange the ProjX with a jump */
				ir_node  *jump      = new_rd_Jmp(dbgi, new_block);

				return jump;
			}

			case pn_Start_P_tls:
				return gen_Proj_tls(node);
		}
		break;

	default:
		if (is_ia32_l_FloattoLL(pred)) {
			return gen_Proj_l_FloattoLL(node);
#ifdef FIRM_EXT_GRS
		} else if (!is_ia32_irn(pred)) { // Quick hack for SIMD optimization
#else
		} else {
#endif
			ir_mode *mode = get_irn_mode(node);
			if (ia32_mode_needs_gp_reg(mode)) {
				ir_node *new_pred = be_transform_node(pred);
				ir_node *new_proj = new_r_Proj(new_pred, mode_Iu,
				                               get_Proj_proj(node));
				new_proj->node_nr = node->node_nr;
				return new_proj;
			}
		}
	}
	return be_duplicate_node(node);
}

/**
 * Enters all transform functions into the generic pointer
 */
static void register_transformers(void)
{
	/* first clear the generic function pointer for all ops */
	clear_irp_opcodes_generic_func();

#define GEN(a)   { be_transform_func *func = gen_##a; op_##a->ops.generic = (op_func) func; }
#define BAD(a)   { op_##a->ops.generic = (op_func)bad_transform; }

	GEN(Add)
	GEN(Sub)
	GEN(Mul)
	GEN(Mulh)
	GEN(And)
	GEN(Or)
	GEN(Eor)

	GEN(Shl)
	GEN(Shr)
	GEN(Shrs)
	GEN(Rotl)

	GEN(Quot)

	GEN(Div)
	GEN(Mod)
	GEN(DivMod)

	GEN(Minus)
	GEN(Conv)
	GEN(Abs)
	GEN(Not)

	GEN(Load)
	GEN(Store)
	GEN(Cond)

	GEN(Cmp)
	GEN(ASM)
	GEN(CopyB)
	GEN(Mux)
	GEN(Proj)
	GEN(Phi)
	GEN(Jmp)
	GEN(IJmp)
	GEN(Bound)

	/* transform ops from intrinsic lowering */
	GEN(ia32_l_Add)
	GEN(ia32_l_Adc)
	GEN(ia32_l_Mul)
	GEN(ia32_l_IMul)
	GEN(ia32_l_ShlDep)
	GEN(ia32_l_ShrDep)
	GEN(ia32_l_SarDep)
	GEN(ia32_l_ShlD)
	GEN(ia32_l_ShrD)
	GEN(ia32_l_Sub)
	GEN(ia32_l_Sbb)
	GEN(ia32_l_LLtoFloat)
	GEN(ia32_l_FloattoLL)

	GEN(Const)
	GEN(SymConst)
	GEN(Unknown)

	/* we should never see these nodes */
	BAD(Raise)
	BAD(Sel)
	BAD(InstOf)
	BAD(Cast)
	BAD(Free)
	BAD(Tuple)
	BAD(Id)
	//BAD(Bad)
	BAD(Confirm)
	BAD(Filter)
	BAD(CallBegin)
	BAD(EndReg)
	BAD(EndExcept)

	/* handle builtins */
	GEN(Builtin)

	/* handle generic backend nodes */
	GEN(be_FrameAddr)
	GEN(be_Call)
	GEN(be_IncSP)
	GEN(be_Return)
	GEN(be_AddSP)
	GEN(be_SubSP)
	GEN(be_Copy)

#undef GEN
#undef BAD
}

/**
 * Pre-transform all unknown and noreg nodes.
 */
static void ia32_pretransform_node(void)
{
	ia32_code_gen_t *cg = env_cg;

	cg->noreg_gp    = be_pre_transform_node(cg->noreg_gp);
	cg->noreg_vfp   = be_pre_transform_node(cg->noreg_vfp);
	cg->noreg_xmm   = be_pre_transform_node(cg->noreg_xmm);

	nomem    = get_irg_no_mem(current_ir_graph);
	noreg_GP = ia32_new_NoReg_gp(cg);

	get_fpcw();
}

/**
 * Walker, checks if all ia32 nodes producing more than one result have their
 * Projs, otherwise creates new Projs and keeps them using a be_Keep node.
 */
static void add_missing_keep_walker(ir_node *node, void *data)
{
	int              n_outs, i;
	unsigned         found_projs = 0;
	const ir_edge_t *edge;
	ir_mode         *mode = get_irn_mode(node);
	ir_node         *last_keep;
	(void) data;
	if (mode != mode_T)
		return;
	if (!is_ia32_irn(node))
		return;

	n_outs = arch_irn_get_n_outs(node);
	if (n_outs <= 0)
		return;
	if (is_ia32_SwitchJmp(node))
		return;

	assert(n_outs < (int) sizeof(unsigned) * 8);
	foreach_out_edge(node, edge) {
		ir_node *proj = get_edge_src_irn(edge);
		int      pn;

		/* The node could be kept */
		if (is_End(proj))
			continue;

		if (get_irn_mode(proj) == mode_M)
			continue;

		pn = get_Proj_proj(proj);
		assert(pn < n_outs);
		found_projs |= 1 << pn;
	}


	/* are keeps missing? */
	last_keep = NULL;
	for (i = 0; i < n_outs; ++i) {
		ir_node                     *block;
		ir_node                     *in[1];
		const arch_register_req_t   *req;
		const arch_register_class_t *cls;

		if (found_projs & (1 << i)) {
			continue;
		}

		req = arch_get_out_register_req(node, i);
		cls = req->cls;
		if (cls == NULL) {
			continue;
		}
		if (cls == &ia32_reg_classes[CLASS_ia32_flags]) {
			continue;
		}

		block = get_nodes_block(node);
		in[0] = new_r_Proj(node, arch_register_class_mode(cls), i);
		if (last_keep != NULL) {
			be_Keep_add_node(last_keep, cls, in[0]);
		} else {
			last_keep = be_new_Keep(block, 1, in);
			if (sched_is_scheduled(node)) {
				sched_add_after(node, last_keep);
			}
		}
	}
}

/**
 * Adds missing keeps to nodes. Adds missing Proj nodes for unused outputs
 * and keeps them.
 */
void ia32_add_missing_keeps(ia32_code_gen_t *cg)
{
	ir_graph *irg = be_get_birg_irg(cg->birg);
	irg_walk_graph(irg, add_missing_keep_walker, NULL, NULL);
}

/**
 * Post-process all calls if we are in SSE mode.
 * The ABI requires that the results are in st0, copy them
 * to a xmm register.
 */
static void postprocess_fp_call_results(void)
{
	int i;

	for (i = ARR_LEN(call_list) - 1; i >= 0; --i) {
		ir_node *call = call_list[i];
		ir_type *mtp  = call_types[i];
		int     j;

		for (j = get_method_n_ress(mtp) - 1; j >= 0; --j) {
			ir_type *res_tp = get_method_res_type(mtp, j);
			ir_node *res, *new_res;
			const ir_edge_t *edge, *next;
			ir_mode *mode;

			if (! is_atomic_type(res_tp)) {
				/* no floating point return */
				continue;
			}
			mode = get_type_mode(res_tp);
			if (! mode_is_float(mode)) {
				/* no floating point return */
				continue;
			}

			res     = be_get_Proj_for_pn(call, pn_ia32_Call_vf0 + j);
			new_res = NULL;

			/* now patch the users */
			foreach_out_edge_safe(res, edge, next) {
				ir_node *succ = get_edge_src_irn(edge);

				/* ignore Keeps */
				if (be_is_Keep(succ))
					continue;

				if (is_ia32_xStore(succ)) {
					/* an xStore can be patched into an vfst */
					dbg_info *db    = get_irn_dbg_info(succ);
					ir_node  *block = get_nodes_block(succ);
					ir_node  *base  = get_irn_n(succ, n_ia32_xStore_base);
					ir_node  *index = get_irn_n(succ, n_ia32_xStore_index);
					ir_node  *mem   = get_irn_n(succ, n_ia32_xStore_mem);
					ir_node  *value = get_irn_n(succ, n_ia32_xStore_val);
					ir_mode  *mode  = get_ia32_ls_mode(succ);

					ir_node  *st = new_bd_ia32_vfst(db, block, base, index, mem, value, mode);
					set_ia32_am_offs_int(st, get_ia32_am_offs_int(succ));
					if (is_ia32_use_frame(succ))
						set_ia32_use_frame(st);
					set_ia32_frame_ent(st, get_ia32_frame_ent(succ));
					set_irn_pinned(st, get_irn_pinned(succ));
					set_ia32_op_type(st, ia32_AddrModeD);

					exchange(succ, st);
				} else {
					if (new_res == NULL) {
						dbg_info *db       = get_irn_dbg_info(call);
						ir_node  *block    = get_nodes_block(call);
						ir_node  *frame    = get_irg_frame(current_ir_graph);
						ir_node  *old_mem  = be_get_Proj_for_pn(call, pn_ia32_Call_M);
						ir_node  *call_mem = new_r_Proj(call, mode_M, pn_ia32_Call_M);
						ir_node  *vfst, *xld, *new_mem;

						/* store st(0) on stack */
						vfst = new_bd_ia32_vfst(db, block, frame, noreg_GP, call_mem, res, mode);
						set_ia32_op_type(vfst, ia32_AddrModeD);
						set_ia32_use_frame(vfst);

						/* load into SSE register */
						xld = new_bd_ia32_xLoad(db, block, frame, noreg_GP, vfst, mode);
						set_ia32_op_type(xld, ia32_AddrModeS);
						set_ia32_use_frame(xld);

						new_res = new_r_Proj(xld, mode, pn_ia32_xLoad_res);
						new_mem = new_r_Proj(xld, mode_M, pn_ia32_xLoad_M);

						if (old_mem != NULL) {
							edges_reroute(old_mem, new_mem, current_ir_graph);
							kill_node(old_mem);
						}
					}
					set_irn_n(succ, get_edge_src_pos(edge), new_res);
				}
			}
		}
	}
}

/* do the transformation */
void ia32_transform_graph(ia32_code_gen_t *cg)
{
	int cse_last;

	register_transformers();
	env_cg        = cg;
	initial_fpcw  = NULL;
	no_pic_adjust = 0;

	be_timer_push(T_HEIGHTS);
	heights      = heights_new(cg->irg);
	be_timer_pop(T_HEIGHTS);
	ia32_calculate_non_address_mode_nodes(cg->birg);

	/* the transform phase is not safe for CSE (yet) because several nodes get
	 * attributes set after their creation */
	cse_last = get_opt_cse();
	set_opt_cse(0);

	call_list  = NEW_ARR_F(ir_node *, 0);
	call_types = NEW_ARR_F(ir_type *, 0);
	be_transform_graph(cg->irg, ia32_pretransform_node);

	if (ia32_cg_config.use_sse2)
		postprocess_fp_call_results();
	DEL_ARR_F(call_types);
	DEL_ARR_F(call_list);

	set_opt_cse(cse_last);

	ia32_free_non_address_mode_nodes();
	heights_free(heights);
	heights = NULL;
}

void ia32_init_transform(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.ia32.transform");
}
