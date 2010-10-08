/*
 * Copyright (C) 1995-2010 University of Karlsruhe.  All right reserved.
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
 * @brief   iropt --- optimizations intertwined with IR construction.
 * @author  Christian Schaefer, Goetz Lindenmaier, Michael Beck
 * @version $Id$
 */
#include "config.h"

#include <string.h>
#include <stdbool.h>

#include "irnode_t.h"
#include "irgraph_t.h"
#include "iredges_t.h"
#include "irmode_t.h"
#include "iropt_t.h"
#include "ircons_t.h"
#include "irgmod.h"
#include "irverify.h"
#include "iroptimize.h"
#include "tv_t.h"
#include "dbginfo_t.h"
#include "iropt_dbg.h"
#include "irflag_t.h"
#include "irhooks.h"
#include "irarch.h"
#include "hashptr.h"
#include "opt_polymorphy.h"
#include "irtools.h"
#include "irhooks.h"
#include "array_t.h"
#include "vrp.h"
#include "firm_types.h"
#include "bitfiddle.h"
#include "be.h"

/* Make types visible to allow most efficient access */
#include "entity_t.h"

/**
 * Returns the tarval of a Const node or tarval_bad for all other nodes.
 */
static ir_tarval *default_value_of(const ir_node *n)
{
	if (is_Const(n))
		return get_Const_tarval(n); /* might return tarval_bad */
	else
		return tarval_bad;
}

value_of_func value_of_ptr = default_value_of;

/* * Set a new value_of function. */
void set_value_of_func(value_of_func func)
{
	if (func != NULL)
		value_of_ptr = func;
	else
		value_of_ptr = default_value_of;
}

/**
 * Return the value of a Constant.
 */
static ir_tarval *computed_value_Const(const ir_node *n)
{
	return get_Const_tarval(n);
}  /* computed_value_Const */

/**
 * Return the value of a 'sizeof', 'alignof' or 'offsetof' SymConst.
 */
static ir_tarval *computed_value_SymConst(const ir_node *n)
{
	ir_type   *type;
	ir_entity *ent;

	switch (get_SymConst_kind(n)) {
	case symconst_type_size:
		type = get_SymConst_type(n);
		if (get_type_state(type) == layout_fixed)
			return new_tarval_from_long(get_type_size_bytes(type), get_irn_mode(n));
		break;
	case symconst_type_align:
		type = get_SymConst_type(n);
		if (get_type_state(type) == layout_fixed)
			return new_tarval_from_long(get_type_alignment_bytes(type), get_irn_mode(n));
		break;
	case symconst_ofs_ent:
		ent  = get_SymConst_entity(n);
		type = get_entity_owner(ent);
		if (get_type_state(type) == layout_fixed)
			return new_tarval_from_long(get_entity_offset(ent), get_irn_mode(n));
		break;
	default:
		break;
	}
	return tarval_bad;
}  /* computed_value_SymConst */

/**
 * Return the value of an Add.
 */
static ir_tarval *computed_value_Add(const ir_node *n)
{
	ir_node *a = get_Add_left(n);
	ir_node *b = get_Add_right(n);

	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad))
		return tarval_add(ta, tb);

	return tarval_bad;
}  /* computed_value_Add */

/**
 * Return the value of a Sub.
 * Special case: a - a
 */
static ir_tarval *computed_value_Sub(const ir_node *n)
{
	ir_mode   *mode = get_irn_mode(n);
	ir_node   *a    = get_Sub_left(n);
	ir_node   *b    = get_Sub_right(n);
	ir_tarval *ta;
	ir_tarval *tb;

	/* NaN - NaN != 0 */
	if (! mode_is_float(mode)) {
		/* a - a = 0 */
		if (a == b)
			return get_mode_null(mode);
	}

	ta = value_of(a);
	tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad))
		return tarval_sub(ta, tb, mode);

	return tarval_bad;
}  /* computed_value_Sub */

/**
 * Return the value of a Carry.
 * Special : a op 0, 0 op b
 */
static ir_tarval *computed_value_Carry(const ir_node *n)
{
	ir_node   *a  = get_binop_left(n);
	ir_node   *b  = get_binop_right(n);
	ir_mode   *m  = get_irn_mode(n);
	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad)) {
		tarval_add(ta, tb);
		return tarval_carry() ? get_mode_one(m) : get_mode_null(m);
	} else {
		if (tarval_is_null(ta) || tarval_is_null(tb))
			return get_mode_null(m);
	}
	return tarval_bad;
}  /* computed_value_Carry */

/**
 * Return the value of a Borrow.
 * Special : a op 0
 */
static ir_tarval *computed_value_Borrow(const ir_node *n)
{
	ir_node   *a  = get_binop_left(n);
	ir_node   *b  = get_binop_right(n);
	ir_mode   *m  = get_irn_mode(n);
	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad)) {
		return tarval_cmp(ta, tb) == pn_Cmp_Lt ? get_mode_one(m) : get_mode_null(m);
	} else if (tarval_is_null(ta)) {
		return get_mode_null(m);
	}
	return tarval_bad;
}  /* computed_value_Borrow */

/**
 * Return the value of an unary Minus.
 */
static ir_tarval *computed_value_Minus(const ir_node *n)
{
	ir_node   *a  = get_Minus_op(n);
	ir_tarval *ta = value_of(a);

	if (ta != tarval_bad)
		return tarval_neg(ta);

	return tarval_bad;
}  /* computed_value_Minus */

/**
 * Return the value of a Mul.
 */
static ir_tarval *computed_value_Mul(const ir_node *n)
{
	ir_node   *a  = get_Mul_left(n);
	ir_node   *b  = get_Mul_right(n);
	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);
	ir_mode   *mode;

	mode = get_irn_mode(n);
	if (mode != get_irn_mode(a)) {
		/* n * n = 2n bit multiplication */
		ta = tarval_convert_to(ta, mode);
		tb = tarval_convert_to(tb, mode);
	}

	if (ta != tarval_bad && tb != tarval_bad) {
		return tarval_mul(ta, tb);
	} else {
		/* a * 0 != 0 if a == NaN or a == Inf */
		if (!mode_is_float(mode)) {
			/* a*0 = 0 or 0*b = 0 */
			if (ta == get_mode_null(mode))
				return ta;
			if (tb == get_mode_null(mode))
				return tb;
		}
	}
	return tarval_bad;
}  /* computed_value_Mul */

/**
 * Return the value of an And.
 * Special case: a & 0, 0 & b
 */
static ir_tarval *computed_value_And(const ir_node *n)
{
	ir_node   *a  = get_And_left(n);
	ir_node   *b  = get_And_right(n);
	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad)) {
		return tarval_and (ta, tb);
	} else {
		if (tarval_is_null(ta)) return ta;
		if (tarval_is_null(tb)) return tb;
	}
	return tarval_bad;
}  /* computed_value_And */

/**
 * Return the value of an Or.
 * Special case: a | 1...1, 1...1 | b
 */
static ir_tarval *computed_value_Or(const ir_node *n)
{
	ir_node   *a  = get_Or_left(n);
	ir_node   *b  = get_Or_right(n);
	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad)) {
		return tarval_or (ta, tb);
	} else {
		if (tarval_is_all_one(ta)) return ta;
		if (tarval_is_all_one(tb)) return tb;
	}
	return tarval_bad;
}  /* computed_value_Or */

/**
 * Return the value of an Eor.
 */
static ir_tarval *computed_value_Eor(const ir_node *n)
{
	ir_node *a = get_Eor_left(n);
	ir_node *b = get_Eor_right(n);

	ir_tarval *ta, *tb;

	if (a == b)
		return get_mode_null(get_irn_mode(n));

	ta = value_of(a);
	tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad)) {
		return tarval_eor(ta, tb);
	}
	return tarval_bad;
}  /* computed_value_Eor */

/**
 * Return the value of a Not.
 */
static ir_tarval *computed_value_Not(const ir_node *n)
{
	ir_node   *a  = get_Not_op(n);
	ir_tarval *ta = value_of(a);

	if (ta != tarval_bad)
		return tarval_not(ta);

	return tarval_bad;
}  /* computed_value_Not */

/**
 * Return the value of a Shl.
 */
static ir_tarval *computed_value_Shl(const ir_node *n)
{
	ir_node *a = get_Shl_left(n);
	ir_node *b = get_Shl_right(n);

	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad)) {
		return tarval_shl(ta, tb);
	}
	return tarval_bad;
}  /* computed_value_Shl */

/**
 * Return the value of a Shr.
 */
static ir_tarval *computed_value_Shr(const ir_node *n)
{
	ir_node *a = get_Shr_left(n);
	ir_node *b = get_Shr_right(n);

	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad)) {
		return tarval_shr(ta, tb);
	}
	return tarval_bad;
}  /* computed_value_Shr */

/**
 * Return the value of a Shrs.
 */
static ir_tarval *computed_value_Shrs(const ir_node *n)
{
	ir_node *a = get_Shrs_left(n);
	ir_node *b = get_Shrs_right(n);

	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad)) {
		return tarval_shrs(ta, tb);
	}
	return tarval_bad;
}  /* computed_value_Shrs */

/**
 * Return the value of a Rotl.
 */
static ir_tarval *computed_value_Rotl(const ir_node *n)
{
	ir_node *a = get_Rotl_left(n);
	ir_node *b = get_Rotl_right(n);

	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	if ((ta != tarval_bad) && (tb != tarval_bad)) {
		return tarval_rotl(ta, tb);
	}
	return tarval_bad;
}  /* computed_value_Rotl */

/**
 * Return the value of a Conv.
 */
static ir_tarval *computed_value_Conv(const ir_node *n)
{
	ir_node *a = get_Conv_op(n);
	ir_tarval *ta = value_of(a);

	if (ta != tarval_bad)
		return tarval_convert_to(ta, get_irn_mode(n));

	return tarval_bad;
}  /* computed_value_Conv */

/**
 * Calculate the value of a Mux: can be evaluated, if the
 * sel and the right input are known.
 */
static ir_tarval *computed_value_Mux(const ir_node *n)
{
	ir_node *sel = get_Mux_sel(n);
	ir_tarval *ts = value_of(sel);

	if (ts == get_tarval_b_true()) {
		ir_node *v = get_Mux_true(n);
		return value_of(v);
	}
	else if (ts == get_tarval_b_false()) {
		ir_node *v = get_Mux_false(n);
		return value_of(v);
	}
	return tarval_bad;
}  /* computed_value_Mux */

/**
 * Calculate the value of a Confirm: can be evaluated,
 * if it has the form Confirm(x, '=', Const).
 */
static ir_tarval *computed_value_Confirm(const ir_node *n)
{
	if (get_Confirm_cmp(n) == pn_Cmp_Eq) {
		ir_tarval *tv = value_of(get_Confirm_bound(n));
		if (tv != tarval_bad)
			return tv;
	}
	return value_of(get_Confirm_value(n));
}  /* computed_value_Confirm */

/**
 * Return the value of a Proj(Cmp).
 *
 * This performs a first step of unreachable code elimination.
 * Proj can not be computed, but folding a Cmp above the Proj here is
 * not as wasteful as folding a Cmp into a Tuple of 16 Consts of which
 * only 1 is used.
 * There are several case where we can evaluate a Cmp node, see later.
 */
static ir_tarval *computed_value_Proj_Cmp(const ir_node *n)
{
	ir_node *cmp   = get_Proj_pred(n);
	ir_node *left  = get_Cmp_left(cmp);
	ir_node *right = get_Cmp_right(cmp);
	long pn_cmp    = get_Proj_proj(n);
	ir_mode *mode  = get_irn_mode(left);
	ir_tarval *tv_l, *tv_r;

	/*
	 * BEWARE: a == a is NOT always True for floating Point values, as
	 * NaN != NaN is defined, so we must check this here.
	 */
	if (left == right && (!mode_is_float(mode) || pn_cmp == pn_Cmp_Lt ||  pn_cmp == pn_Cmp_Gt)) {
		/* This is a trick with the bits used for encoding the Cmp
		   Proj numbers, the following statement is not the same:
		return new_tarval_from_long(pn_cmp == pn_Cmp_Eq, mode_b) */
		return new_tarval_from_long(pn_cmp & pn_Cmp_Eq, mode_b);
	}
	tv_l = value_of(left);
	tv_r = value_of(right);

	if ((tv_l != tarval_bad) && (tv_r != tarval_bad)) {
		/*
		 * The predecessors of Cmp are target values.  We can evaluate
		 * the Cmp.
		 */
		pn_Cmp flags = tarval_cmp(tv_l, tv_r);
		if (flags != pn_Cmp_False) {
			return new_tarval_from_long (pn_cmp & flags, mode_b);
		}
	} else if (mode_is_int(mode)) {
		/* for integer values, we can check against MIN/MAX */
		pn_Cmp cmp_result;

		if (tv_l == get_mode_min(mode)) {
			/* MIN <=/> x.  This results in true/false. */
			if (pn_cmp == pn_Cmp_Le)
				return tarval_b_true;
			else if (pn_cmp == pn_Cmp_Gt)
				return tarval_b_false;
		} else if (tv_r == get_mode_min(mode)) {
			/* x >=/< MIN.  This results in true/false. */
			if (pn_cmp == pn_Cmp_Ge)
				return tarval_b_true;
			else if (pn_cmp == pn_Cmp_Lt)
				return tarval_b_false;
		} else if (tv_l == get_mode_max(mode)) {
			/* MAX >=/< x.  This results in true/false. */
			if (pn_cmp == pn_Cmp_Ge)
				return tarval_b_true;
			else if (pn_cmp == pn_Cmp_Lt)
				return tarval_b_false;
		} else if (tv_r == get_mode_max(mode)) {
			/* x <=/> MAX.  This results in true/false. */
			if (pn_cmp == pn_Cmp_Le)
				return tarval_b_true;
			else if (pn_cmp == pn_Cmp_Gt)
				return tarval_b_false;
		}

		cmp_result = vrp_cmp(left, right);
		if (cmp_result != pn_Cmp_False) {
			if (cmp_result == pn_Cmp_Lg) {
				if (pn_cmp == pn_Cmp_Eq) {
					return tarval_b_false;
				} else if (pn_cmp == pn_Cmp_Lg) {
					return tarval_b_true;
				}
			} else {
				return new_tarval_from_long(cmp_result & pn_cmp, mode_b);
			}
		}
	} else if (mode_is_reference(mode)) {
		/* pointer compare */
		ir_node *s_l = skip_Proj(left);
		ir_node *s_r = skip_Proj(right);

		if ((is_Alloc(s_l) && tarval_is_null(tv_r)) ||
			(tarval_is_null(tv_l) && is_Alloc(s_r))) {
			/*
			 * The predecessors are Allocs and (void*)(0) constants. In Firm Allocs never
			 * return NULL, they raise an exception. Therefore we can predict
			 * the Cmp result.
			 */
			return new_tarval_from_long(pn_cmp & pn_Cmp_Lg, mode_b);
		}
	}
	return computed_value_Cmp_Confirm(cmp, left, right, pn_cmp);
}  /* computed_value_Proj_Cmp */

/**
 * Return the value of a floating point Quot.
 */
static ir_tarval *do_computed_value_Quot(const ir_node *a, const ir_node *b)
{
	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	/* cannot optimize 0 / b = 0 because of NaN */
	if (ta != tarval_bad && tb != tarval_bad)
		return tarval_quo(ta, tb);
	return tarval_bad;
}  /* do_computed_value_Quot */

/**
 * Calculate the value of an integer Div of two nodes.
 * Special case: 0 / b
 */
static ir_tarval *do_computed_value_Div(const ir_node *a, const ir_node *b)
{
	ir_tarval     *ta = value_of(a);
	ir_tarval     *tb;
	const ir_node *dummy;

	/* Compute c1 / c2 or 0 / a, a != 0 */
	if (tarval_is_null(ta) && value_not_zero(b, &dummy))
		return ta;  /* 0 / b == 0 */
	tb = value_of(b);
	if (ta != tarval_bad && tb != tarval_bad)
		return tarval_div(ta, tb);
	return tarval_bad;
}  /* do_computed_value_Div */

/**
 * Calculate the value of an integer Mod of two nodes.
 * Special case: a % 1
 */
static ir_tarval *do_computed_value_Mod(const ir_node *a, const ir_node *b)
{
	ir_tarval *ta = value_of(a);
	ir_tarval *tb = value_of(b);

	/* Compute a % 1 or c1 % c2 */
	if (tarval_is_one(tb))
		return get_mode_null(get_irn_mode(a));
	if (ta != tarval_bad && tb != tarval_bad)
		return tarval_mod(ta, tb);
	return tarval_bad;
}  /* do_computed_value_Mod */

/**
 * Return the value of a Proj(DivMod).
 */
static ir_tarval *computed_value_Proj_DivMod(const ir_node *n)
{
	long proj_nr = get_Proj_proj(n);

	/* compute either the Div or the Mod part */
	if (proj_nr == pn_DivMod_res_div) {
		const ir_node *a = get_Proj_pred(n);
		return do_computed_value_Div(get_DivMod_left(a), get_DivMod_right(a));
	} else if (proj_nr == pn_DivMod_res_mod) {
		const ir_node *a = get_Proj_pred(n);
		return do_computed_value_Mod(get_DivMod_left(a), get_DivMod_right(a));
	}
	return tarval_bad;
}  /* computed_value_Proj_DivMod */

/**
 * Return the value of a Proj(Div).
 */
static ir_tarval *computed_value_Proj_Div(const ir_node *n)
{
	long proj_nr = get_Proj_proj(n);

	if (proj_nr == pn_Div_res) {
		const ir_node *a = get_Proj_pred(n);
		return do_computed_value_Div(get_Div_left(a), get_Div_right(a));
	}
	return tarval_bad;
}  /* computed_value_Proj_Div */

/**
 * Return the value of a Proj(Mod).
 */
static ir_tarval *computed_value_Proj_Mod(const ir_node *n)
{
	long proj_nr = get_Proj_proj(n);

	if (proj_nr == pn_Mod_res) {
		const ir_node *a = get_Proj_pred(n);
		return do_computed_value_Mod(get_Mod_left(a), get_Mod_right(a));
	}
	return tarval_bad;
}  /* computed_value_Proj_Mod */

/**
 * Return the value of a Proj(Quot).
 */
static ir_tarval *computed_value_Proj_Quot(const ir_node *n)
{
	long proj_nr = get_Proj_proj(n);

	if (proj_nr == pn_Quot_res) {
		const ir_node *a = get_Proj_pred(n);
		return do_computed_value_Quot(get_Quot_left(a), get_Quot_right(a));
	}
	return tarval_bad;
}  /* computed_value_Proj_Quot */

/**
 * Return the value of a Proj.
 */
static ir_tarval *computed_value_Proj(const ir_node *proj)
{
	ir_node *n = get_Proj_pred(proj);

	if (n->op->ops.computed_value_Proj != NULL)
		return n->op->ops.computed_value_Proj(proj);
	return tarval_bad;
}  /* computed_value_Proj */

/**
 * If the parameter n can be computed, return its value, else tarval_bad.
 * Performs constant folding.
 *
 * @param n  The node this should be evaluated
 */
ir_tarval *computed_value(const ir_node *n)
{
	vrp_attr *vrp = vrp_get_info(n);
	if (vrp && vrp->valid && tarval_cmp(vrp->bits_set, vrp->bits_not_set) == pn_Cmp_Eq) {
		return vrp->bits_set;
	}
	if (n->op->ops.computed_value)
		return n->op->ops.computed_value(n);
	return tarval_bad;
}  /* computed_value */

/**
 * Set the default computed_value evaluator in an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
static ir_op_ops *firm_set_default_computed_value(ir_opcode code, ir_op_ops *ops)
{
#define CASE(a)                                        \
	case iro_##a:                                      \
		ops->computed_value      = computed_value_##a; \
		break
#define CASE_PROJ(a)                                        \
	case iro_##a:                                           \
		ops->computed_value_Proj = computed_value_Proj_##a; \
		break

	switch (code) {
	CASE(Const);
	CASE(SymConst);
	CASE(Add);
	CASE(Sub);
	CASE(Carry);
	CASE(Borrow);
	CASE(Minus);
	CASE(Mul);
	CASE(And);
	CASE(Or);
	CASE(Eor);
	CASE(Not);
	CASE(Shl);
	CASE(Shr);
	CASE(Shrs);
	CASE(Rotl);
	CASE(Conv);
	CASE(Mux);
	CASE(Confirm);
	CASE_PROJ(Cmp);
	CASE_PROJ(DivMod);
	CASE_PROJ(Div);
	CASE_PROJ(Mod);
	CASE_PROJ(Quot);
	CASE(Proj);
	default:
		/* leave NULL */
		break;
	}

	return ops;
#undef CASE_PROJ
#undef CASE
}  /* firm_set_default_computed_value */

/**
 * Returns a equivalent block for another block.
 * If the block has only one predecessor, this is
 * the equivalent one. If the only predecessor of a block is
 * the block itself, this is a dead block.
 *
 * If both predecessors of a block are the branches of a binary
 * Cond, the equivalent block is Cond's block.
 *
 * If all predecessors of a block are bad or lies in a dead
 * block, the current block is dead as well.
 *
 * Note, that blocks are NEVER turned into Bad's, instead
 * the dead_block flag is set. So, never test for is_Bad(block),
 * always use is_dead_Block(block).
 */
static ir_node *equivalent_node_Block(ir_node *n)
{
	ir_node *oldn = n;
	int     n_preds;
	ir_graph *irg;

	/* don't optimize dead or labeled blocks */
	if (is_Block_dead(n) || has_Block_entity(n))
		return n;

	n_preds = get_Block_n_cfgpreds(n);

	/* The Block constructor does not call optimize, but mature_immBlock()
	   calls the optimization. */
	assert(get_Block_matured(n));

	irg = get_irn_irg(n);

	/* Straightening: a single entry Block following a single exit Block
	   can be merged, if it is not the Start block. */
	/* !!! Beware, all Phi-nodes of n must have been optimized away.
	   This should be true, as the block is matured before optimize is called.
	   But what about Phi-cycles with the Phi0/Id that could not be resolved?
	   Remaining Phi nodes are just Ids. */
	if (n_preds == 1) {
		ir_node *pred = skip_Proj(get_Block_cfgpred(n, 0));

		if (is_Jmp(pred)) {
			ir_node *predblock = get_nodes_block(pred);
			if (predblock == oldn) {
				/* Jmp jumps into the block it is in -- deal self cycle. */
				n = set_Block_dead(n);
				DBG_OPT_DEAD_BLOCK(oldn, n);
			} else {
				n = predblock;
				DBG_OPT_STG(oldn, n);
			}
		} else if (is_Cond(pred)) {
			ir_node *predblock = get_nodes_block(pred);
			if (predblock == oldn) {
				/* Jmp jumps into the block it is in -- deal self cycle. */
				n = set_Block_dead(n);
				DBG_OPT_DEAD_BLOCK(oldn, n);
			}
		}
	} else if (n_preds == 2) {
		/* Test whether Cond jumps twice to this block
		 * The more general case which more than 2 predecessors is handles
		 * in optimize_cf(), we handle only this special case for speed here.
		 */
		ir_node *a = get_Block_cfgpred(n, 0);
		ir_node *b = get_Block_cfgpred(n, 1);

		if (is_Proj(a) && is_Proj(b)) {
			ir_node *cond = get_Proj_pred(a);

		    if (cond == get_Proj_pred(b) && is_Cond(cond) &&
		        get_irn_mode(get_Cond_selector(cond)) == mode_b) {
				/* Also a single entry Block following a single exit Block.  Phis have
				   twice the same operand and will be optimized away. */
				n = get_nodes_block(cond);
				DBG_OPT_IFSIM1(oldn, a, b, n);
			}
		}
	} else if (get_opt_unreachable_code() &&
	           (n != get_irg_start_block(irg)) &&
	           (n != get_irg_end_block(irg))) {
		int i;

		/* If all inputs are dead, this block is dead too, except if it is
		   the start or end block.  This is one step of unreachable code
		   elimination */
		for (i = get_Block_n_cfgpreds(n) - 1; i >= 0; --i) {
			ir_node *pred = get_Block_cfgpred(n, i);
			ir_node *pred_blk;

			if (is_Bad(pred)) continue;
			pred_blk = get_nodes_block(skip_Proj(pred));

			if (is_Block_dead(pred_blk)) continue;

			if (pred_blk != n) {
				/* really found a living input */
				break;
			}
		}
		if (i < 0) {
			n = set_Block_dead(n);
			DBG_OPT_DEAD_BLOCK(oldn, n);
		}
	}

	return n;
}  /* equivalent_node_Block */

/**
 * Returns a equivalent node for a Jmp, a Bad :-)
 * Of course this only happens if the Block of the Jmp is dead.
 */
static ir_node *equivalent_node_Jmp(ir_node *n)
{
	ir_node *oldn = n;

	/* unreachable code elimination */
	if (is_Block_dead(get_nodes_block(n))) {
		ir_graph *irg = get_irn_irg(n);
		n = get_irg_bad(irg);
		DBG_OPT_DEAD_BLOCK(oldn, n);
	}
	return n;
}  /* equivalent_node_Jmp */

/** Raise is handled in the same way as Jmp. */
#define equivalent_node_Raise   equivalent_node_Jmp


/* We do not evaluate Cond here as we replace it by a new node, a Jmp.
   See transform_node_Proj_Cond(). */

/**
 * Optimize operations that are commutative and have neutral 0,
 * so a op 0 = 0 op a = a.
 */
static ir_node *equivalent_node_neutral_zero(ir_node *n)
{
	ir_node *oldn = n;

	ir_node *a = get_binop_left(n);
	ir_node *b = get_binop_right(n);

	ir_tarval *tv;
	ir_node *on;

	/* After running compute_node there is only one constant predecessor.
	   Find this predecessors value and remember the other node: */
	if ((tv = value_of(a)) != tarval_bad) {
		on = b;
	} else if ((tv = value_of(b)) != tarval_bad) {
		on = a;
	} else
		return n;

	/* If this predecessors constant value is zero, the operation is
	 * unnecessary. Remove it.
	 *
	 * Beware: If n is a Add, the mode of on and n might be different
	 * which happens in this rare construction: NULL + 3.
	 * Then, a Conv would be needed which we cannot include here.
	 */
	if (tarval_is_null(tv) && get_irn_mode(on) == get_irn_mode(n)) {
		n = on;

		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_0);
	}

	return n;
}  /* equivalent_node_neutral_zero */

/**
 * Eor is commutative and has neutral 0.
 */
static ir_node *equivalent_node_Eor(ir_node *n)
{
	ir_node *oldn = n;
	ir_node *a;
	ir_node *b;

	n = equivalent_node_neutral_zero(n);
	if (n != oldn) return n;

	a = get_Eor_left(n);
	b = get_Eor_right(n);

	if (is_Eor(a)) {
		ir_node *aa = get_Eor_left(a);
		ir_node *ab = get_Eor_right(a);

		if (aa == b) {
			/* (a ^ b) ^ a -> b */
			n = ab;
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_EOR_A_B_A);
			return n;
		} else if (ab == b) {
			/* (a ^ b) ^ b -> a */
			n = aa;
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_EOR_A_B_A);
			return n;
		}
	}
	if (is_Eor(b)) {
		ir_node *ba = get_Eor_left(b);
		ir_node *bb = get_Eor_right(b);

		if (ba == a) {
			/* a ^ (a ^ b) -> b */
			n = bb;
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_EOR_A_B_A);
			return n;
		} else if (bb == a) {
			/* a ^ (b ^ a) -> b */
			n = ba;
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_EOR_A_B_A);
			return n;
		}
	}
	return n;
}

/*
 * Optimize a - 0 and (a - x) + x (for modes with wrap-around).
 *
 * The second one looks strange, but this construct
 * is used heavily in the LCC sources :-).
 *
 * Beware: The Mode of an Add may be different than the mode of its
 * predecessors, so we could not return a predecessors in all cases.
 */
static ir_node *equivalent_node_Add(ir_node *n)
{
	ir_node *oldn = n;
	ir_node *left, *right;
	ir_mode *mode = get_irn_mode(n);

	n = equivalent_node_neutral_zero(n);
	if (n != oldn)
		return n;

	/* for FP these optimizations are only allowed if fp_strict_algebraic is disabled */
	if (mode_is_float(mode)) {
		ir_graph *irg = get_irn_irg(n);
		if (get_irg_fp_model(irg) & fp_strict_algebraic)
			return n;
	}

	left  = get_Add_left(n);
	right = get_Add_right(n);

	if (is_Sub(left)) {
		if (get_Sub_right(left) == right) {
			/* (a - x) + x */

			n = get_Sub_left(left);
			if (mode == get_irn_mode(n)) {
				DBG_OPT_ALGSIM1(oldn, left, right, n, FS_OPT_ADD_SUB);
				return n;
			}
		}
	}
	if (is_Sub(right)) {
		if (get_Sub_right(right) == left) {
			/* x + (a - x) */

			n = get_Sub_left(right);
			if (mode == get_irn_mode(n)) {
				DBG_OPT_ALGSIM1(oldn, left, right, n, FS_OPT_ADD_SUB);
				return n;
			}
		}
	}
	return n;
}  /* equivalent_node_Add */

/**
 * optimize operations that are not commutative but have neutral 0 on left,
 * so a op 0 = a.
 */
static ir_node *equivalent_node_left_zero(ir_node *n)
{
	ir_node *oldn = n;

	ir_node   *a  = get_binop_left(n);
	ir_node   *b  = get_binop_right(n);
	ir_tarval *tb = value_of(b);

	if (tarval_is_null(tb)) {
		n = a;

		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_0);
	}
	return n;
}  /* equivalent_node_left_zero */

#define equivalent_node_Shl   equivalent_node_left_zero
#define equivalent_node_Shr   equivalent_node_left_zero
#define equivalent_node_Shrs  equivalent_node_left_zero
#define equivalent_node_Rotl  equivalent_node_left_zero

/**
 * Optimize a - 0 and (a + x) - x (for modes with wrap-around).
 *
 * The second one looks strange, but this construct
 * is used heavily in the LCC sources :-).
 *
 * Beware: The Mode of a Sub may be different than the mode of its
 * predecessors, so we could not return a predecessors in all cases.
 */
static ir_node *equivalent_node_Sub(ir_node *n)
{
	ir_node   *oldn = n;
	ir_node   *b;
	ir_mode   *mode = get_irn_mode(n);
	ir_tarval *tb;

	/* for FP these optimizations are only allowed if fp_strict_algebraic is disabled */
	if (mode_is_float(mode)) {
		ir_graph *irg = get_irn_irg(n);
		if (get_irg_fp_model(irg) & fp_strict_algebraic)
			return n;
	}

	b  = get_Sub_right(n);
	tb = value_of(b);

	/* Beware: modes might be different */
	if (tarval_is_null(tb)) {
		ir_node *a = get_Sub_left(n);
		if (mode == get_irn_mode(a)) {
			n = a;

			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_0);
		}
	}
	return n;
}  /* equivalent_node_Sub */


/**
 * Optimize an "self-inverse unary op", ie op(op(n)) = n.
 *
 * @todo
 *   -(-a) == a, but might overflow two times.
 *   We handle it anyway here but the better way would be a
 *   flag. This would be needed for Pascal for instance.
 */
static ir_node *equivalent_node_idempotent_unop(ir_node *n)
{
	ir_node *oldn = n;
	ir_node *pred = get_unop_op(n);

	/* optimize symmetric unop */
	if (get_irn_op(pred) == get_irn_op(n)) {
		n = get_unop_op(pred);
		DBG_OPT_ALGSIM2(oldn, pred, n, FS_OPT_IDEM_UNARY);
	}
	return n;
}  /* equivalent_node_idempotent_unop */

/** Optimize Not(Not(x)) == x. */
#define equivalent_node_Not    equivalent_node_idempotent_unop

/** -(-x) == x       ??? Is this possible or can --x raise an
                       out of bounds exception if min =! max? */
#define equivalent_node_Minus  equivalent_node_idempotent_unop

/**
 * Optimize a * 1 = 1 * a = a.
 */
static ir_node *equivalent_node_Mul(ir_node *n)
{
	ir_node *oldn = n;
	ir_node *a = get_Mul_left(n);

	/* we can handle here only the n * n = n bit cases */
	if (get_irn_mode(n) == get_irn_mode(a)) {
		ir_node   *b = get_Mul_right(n);
		ir_tarval *tv;

		/*
		 * Mul is commutative and has again an other neutral element.
		 * Constants are place right, so check this case first.
		 */
		tv = value_of(b);
		if (tarval_is_one(tv)) {
			n = a;
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_1);
		} else {
			tv = value_of(a);
			if (tarval_is_one(tv)) {
				n = b;
				DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_1);
			}
		}
	}
	return n;
}  /* equivalent_node_Mul */

/**
 * Use algebraic simplification a | a = a | 0 = 0 | a = a.
 */
static ir_node *equivalent_node_Or(ir_node *n)
{
	ir_node *oldn = n;

	ir_node   *a = get_Or_left(n);
	ir_node   *b = get_Or_right(n);
	ir_tarval *tv;

	if (a == b) {
		n = a;    /* Or has it's own neutral element */
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_OR);
		return n;
	}
	/* constants are cormalized to right, check this site first */
	tv = value_of(b);
	if (tarval_is_null(tv)) {
		n = a;
		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_OR);
		return n;
	}
	tv = value_of(a);
	if (tarval_is_null(tv)) {
		n = b;
		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_OR);
		return n;
	}

	return n;
}  /* equivalent_node_Or */

/**
 * Optimize a & 0b1...1 = 0b1...1 & a = a & a = (a|X) & a = a.
 */
static ir_node *equivalent_node_And(ir_node *n)
{
	ir_node *oldn = n;

	ir_node   *a = get_And_left(n);
	ir_node   *b = get_And_right(n);
	ir_tarval *tv;

	if (a == b) {
		n = a;    /* And has it's own neutral element */
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_AND);
		return n;
	}
	/* constants are normalized to right, check this site first */
	tv = value_of(b);
	if (tarval_is_all_one(tv)) {
		n = a;
		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_AND);
		return n;
	}
	if (tv != get_tarval_bad()) {
		ir_mode *mode = get_irn_mode(n);
		if (!mode_is_signed(mode) && is_Conv(a)) {
			ir_node *convop     = get_Conv_op(a);
			ir_mode *convopmode = get_irn_mode(convop);
			if (!mode_is_signed(convopmode)) {
				if (tarval_is_all_one(tarval_convert_to(tv, convopmode))) {
					/* Conv(X) & all_one(mode(X)) = Conv(X) */
					n = a;
					DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_AND);
					return n;
				}
			}
		}
	}
	tv = value_of(a);
	if (tarval_is_all_one(tv)) {
		n = b;
		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_AND);
		return n;
	}
	if (is_Or(a)) {
		if (b == get_Or_left(a) || b == get_Or_right(a)) {
			/* (a|X) & a */
			n = b;
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_AND);
			return n;
		}
	}
	if (is_Or(b)) {
		if (a == get_Or_left(b) || a == get_Or_right(b)) {
			/* a & (a|X) */
			n = a;
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_AND);
			return n;
		}
	}
	return n;
}  /* equivalent_node_And */

/**
 * Try to remove useless Conv's:
 */
static ir_node *equivalent_node_Conv(ir_node *n)
{
	ir_node *oldn = n;
	ir_node *a = get_Conv_op(n);

	ir_mode *n_mode = get_irn_mode(n);
	ir_mode *a_mode = get_irn_mode(a);

restart:
	if (n_mode == a_mode) { /* No Conv necessary */
		if (get_Conv_strict(n)) {
			ir_node *p = a;

			/* neither Minus nor Confirm change the precision,
			   so we can "look-through" */
			for (;;) {
				if (is_Minus(p)) {
					p = get_Minus_op(p);
				} else if (is_Confirm(p)) {
					p = get_Confirm_value(p);
				} else {
					/* stop here */
					break;
				}
			}
			if (is_Conv(p) && get_Conv_strict(p)) {
				/* we known already, that a_mode == n_mode, and neither
				   Minus change the mode, so the second Conv
				   can be kicked */
				assert(get_irn_mode(p) == n_mode);
				n = a;
				DBG_OPT_ALGSIM0(oldn, n, FS_OPT_CONV);
				return n;
			}
			if (is_Proj(p)) {
				ir_node *pred = get_Proj_pred(p);
				if (is_Load(pred)) {
					/* Loads always return with the exact precision of n_mode */
					assert(get_Load_mode(pred) == n_mode);
					n = a;
					DBG_OPT_ALGSIM0(oldn, n, FS_OPT_CONV);
					return n;
				}
				if (is_Proj(pred) && get_Proj_proj(pred) == pn_Start_T_args) {
					pred = get_Proj_pred(pred);
					if (is_Start(pred)) {
						/* Arguments always return with the exact precision,
						   as strictConv's are place before Call -- if the
						   caller was compiled with the same setting.
						   Otherwise, the semantics is probably still right. */
						assert(get_irn_mode(p) == n_mode);
						n = a;
						DBG_OPT_ALGSIM0(oldn, n, FS_OPT_CONV);
						return n;
					}
				}
			}
			if (is_Conv(a)) {
				/* special case: the immediate predecessor is also a Conv */
				if (! get_Conv_strict(a)) {
					/* first one is not strict, kick it */
					a = get_Conv_op(a);
					a_mode = get_irn_mode(a);
					set_Conv_op(n, a);
					goto restart;
				}
				/* else both are strict conv, second is superfluous */
				n = a;
				DBG_OPT_ALGSIM0(oldn, n, FS_OPT_CONV);
				return n;
			}
		} else {
			n = a;
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_CONV);
			return n;
		}
	} else if (is_Conv(a)) { /* Conv(Conv(b)) */
		ir_node *b      = get_Conv_op(a);
		ir_mode *b_mode = get_irn_mode(b);

		if (get_Conv_strict(n) && get_Conv_strict(a)) {
			/* both are strict conv */
			if (smaller_mode(a_mode, n_mode)) {
				/* both are strict, but the first is smaller, so
				   the second cannot remove more precision, remove the
				   strict bit */
				set_Conv_strict(n, 0);
			}
		}
		if (n_mode == b_mode) {
			if (! get_Conv_strict(n) && ! get_Conv_strict(a)) {
				if (n_mode == mode_b) {
					n = b; /* Convb(Conv*(xxxb(...))) == xxxb(...) */
					DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_CONV);
					return n;
				} else if (get_mode_arithmetic(n_mode) == get_mode_arithmetic(a_mode)) {
					if (values_in_mode(b_mode, a_mode)) {
						n = b;        /* ConvS(ConvL(xxxS(...))) == xxxS(...) */
						DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_CONV);
						return n;
					}
				}
			}
			if (mode_is_int(n_mode) && get_mode_arithmetic(a_mode) == irma_ieee754) {
				/* ConvI(ConvF(I)) -> I, iff float mantissa >= int mode */
				unsigned int_mantissa   = get_mode_size_bits(n_mode) - (mode_is_signed(n_mode) ? 1 : 0);
				unsigned float_mantissa = tarval_ieee754_get_mantissa_size(a_mode);

				if (float_mantissa >= int_mantissa) {
					n = b;
					DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_CONV);
					return n;
				}
			}
			if (is_Conv(b)) {
				if (smaller_mode(b_mode, a_mode)) {
					if (get_Conv_strict(n))
						set_Conv_strict(b, 1);
					n = b; /* ConvA(ConvB(ConvA(...))) == ConvA(...) */
					DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_CONV);
					return n;
				}
			}
		}
	}
	return n;
}  /* equivalent_node_Conv */

/**
 * - fold Phi-nodes, iff they have only one predecessor except
 *   themselves.
 */
static ir_node *equivalent_node_Phi(ir_node *n)
{
	int i, n_preds;

	ir_node *oldn = n;
	ir_node *block;
	ir_node *first_val = NULL; /* to shutup gcc */

	if (!get_opt_optimize() &&
			get_irg_phase_state(get_irn_irg(n)) != phase_building)
		return n;

	n_preds = get_Phi_n_preds(n);

	block = get_nodes_block(n);
	/* Control dead */
	if (is_Block_dead(block)) {
		ir_graph *irg = get_irn_irg(n);
		return get_irg_bad(irg);
	}

	if (n_preds == 0) return n;           /* Phi of dead Region without predecessors. */

	/* Find first non-self-referencing input */
	for (i = 0; i < n_preds; ++i) {
		first_val = get_Phi_pred(n, i);
		if (   (first_val != n)                            /* not self pointer */
#if 0
		    /* BEWARE: when the if is changed to 1, Phi's will ignore it's Bad
		     * predecessors. Then, Phi nodes in dead code might be removed, causing
		     * nodes pointing to themself (Add's for instance).
		     * This is really bad and causes endless recursions in several
		     * code pathes, so we do NOT optimize such a code.
		     * This is not that bad as it sounds, optimize_cf() removes bad control flow
		     * (and bad Phi predecessors), so live code is optimized later.
		     */
			&& (! is_Bad(get_Block_cfgpred(block, i)))
#endif
		   ) {        /* value not dead */
			break;          /* then found first value. */
		}
	}

	if (i >= n_preds) {
		ir_graph *irg = get_irn_irg(n);
		/* A totally Bad or self-referencing Phi (we didn't break the above loop) */
		return get_irg_bad(irg);
	}

	/* search for rest of inputs, determine if any of these
	are non-self-referencing */
	while (++i < n_preds) {
		ir_node *scnd_val = get_Phi_pred(n, i);
		if (   (scnd_val != n)
		    && (scnd_val != first_val)
#if 0
		    /* see above */
		    && (! is_Bad(get_Block_cfgpred(block, i)))
#endif
			) {
			break;
		}
	}

	if (i >= n_preds && !is_Dummy(first_val)) {
		/* Fold, if no multiple distinct non-self-referencing inputs */
		n = first_val;
		DBG_OPT_PHI(oldn, n);
	}
	return n;
}  /* equivalent_node_Phi */

/**
 * Several optimizations:
 * - fold Sync-nodes, iff they have only one predecessor except
 *   themselves.
 */
static ir_node *equivalent_node_Sync(ir_node *n)
{
	int arity = get_Sync_n_preds(n);
	int i;

	for (i = 0; i < arity;) {
		ir_node *pred = get_Sync_pred(n, i);
		int      j;

		/* Remove Bad predecessors */
		if (is_Bad(pred)) {
			del_Sync_n(n, i);
			--arity;
			continue;
		}

		/* Remove duplicate predecessors */
		for (j = 0;; ++j) {
			if (j >= i) {
				++i;
				break;
			}
			if (get_Sync_pred(n, j) == pred) {
				del_Sync_n(n, i);
				--arity;
				break;
			}
		}
	}

	if (arity == 0) {
		ir_graph *irg = get_irn_irg(n);
		return get_irg_bad(irg);
	}
	if (arity == 1) return get_Sync_pred(n, 0);
	return n;
}  /* equivalent_node_Sync */

/**
 * Optimize Proj(Tuple).
 */
static ir_node *equivalent_node_Proj_Tuple(ir_node *proj)
{
	ir_node *oldn  = proj;
	ir_node *tuple = get_Proj_pred(proj);

	/* Remove the Tuple/Proj combination. */
	proj = get_Tuple_pred(tuple, get_Proj_proj(proj));
	DBG_OPT_TUPLE(oldn, tuple, proj);

	return proj;
}  /* equivalent_node_Proj_Tuple */

/**
 * Optimize a / 1 = a.
 */
static ir_node *equivalent_node_Proj_Div(ir_node *proj)
{
	ir_node   *oldn = proj;
	ir_node   *div  = get_Proj_pred(proj);
	ir_node   *b    = get_Div_right(div);
	ir_tarval *tb   = value_of(b);

	/* Div is not commutative. */
	if (tarval_is_one(tb)) { /* div(x, 1) == x */
		switch (get_Proj_proj(proj)) {
		case pn_Div_M:
			proj = get_Div_mem(div);
			DBG_OPT_ALGSIM0(oldn, proj, FS_OPT_NEUTRAL_1);
			return proj;

		case pn_Div_res:
			proj = get_Div_left(div);
			DBG_OPT_ALGSIM0(oldn, proj, FS_OPT_NEUTRAL_1);
			return proj;

		default:
			/* we cannot replace the exception Proj's here, this is done in
			   transform_node_Proj_Div() */
			return proj;
		}
	}
	return proj;
}  /* equivalent_node_Proj_Div */

/**
 * Optimize a / 1.0 = a.
 */
static ir_node *equivalent_node_Proj_Quot(ir_node *proj)
{
	ir_node   *oldn = proj;
	ir_node   *quot = get_Proj_pred(proj);
	ir_node   *b    = get_Quot_right(quot);
	ir_tarval *tb   = value_of(b);

	/* Div is not commutative. */
	if (tarval_is_one(tb)) { /* Quot(x, 1) == x */
		switch (get_Proj_proj(proj)) {
		case pn_Quot_M:
			proj = get_Quot_mem(quot);
			DBG_OPT_ALGSIM0(oldn, proj, FS_OPT_NEUTRAL_1);
			return proj;

		case pn_Quot_res:
			proj = get_Quot_left(quot);
			DBG_OPT_ALGSIM0(oldn, proj, FS_OPT_NEUTRAL_1);
			return proj;

		default:
			/* we cannot replace the exception Proj's here, this is done in
			   transform_node_Proj_Quot() */
			return proj;
		}
	}
	return proj;
}  /* equivalent_node_Proj_Quot */

/**
 * Optimize a / 1 = a.
 */
static ir_node *equivalent_node_Proj_DivMod(ir_node *proj)
{
	ir_node   *oldn   = proj;
	ir_node   *divmod = get_Proj_pred(proj);
	ir_node   *b      = get_DivMod_right(divmod);
	ir_tarval *tb     = value_of(b);

	/* Div is not commutative. */
	if (tarval_is_one(tb)) { /* div(x, 1) == x */
		switch (get_Proj_proj(proj)) {
		case pn_DivMod_M:
			proj = get_DivMod_mem(divmod);
			DBG_OPT_ALGSIM0(oldn, proj, FS_OPT_NEUTRAL_1);
			return proj;

		case pn_DivMod_res_div:
			proj = get_DivMod_left(divmod);
			DBG_OPT_ALGSIM0(oldn, proj, FS_OPT_NEUTRAL_1);
			return proj;

		default:
			/* we cannot replace the exception Proj's here, this is done in
			   transform_node_Proj_DivMod().
			   Note further that the pn_DivMod_res_div case is handled in
			   computed_value_Proj(). */
			return proj;
		}
	}
	return proj;
}  /* equivalent_node_Proj_DivMod */

/**
 * Optimize CopyB(mem, x, x) into a Nop.
 */
static ir_node *equivalent_node_Proj_CopyB(ir_node *proj)
{
	ir_node *oldn  = proj;
	ir_node *copyb = get_Proj_pred(proj);
	ir_node *a     = get_CopyB_dst(copyb);
	ir_node *b     = get_CopyB_src(copyb);

	if (a == b) {
		/* Turn CopyB into a tuple (mem, jmp, bad, bad) */
		switch (get_Proj_proj(proj)) {
		case pn_CopyB_M:
			proj = get_CopyB_mem(copyb);
			DBG_OPT_ALGSIM0(oldn, proj, FS_OPT_NOP);
			break;

		case pn_CopyB_X_except: {
			ir_graph *irg = get_irn_irg(proj);
			DBG_OPT_EXC_REM(proj);
			proj = get_irg_bad(irg);
			break;
		}
		}
	}
	return proj;
}  /* equivalent_node_Proj_CopyB */

/**
 * Optimize Bounds(idx, idx, upper) into idx.
 */
static ir_node *equivalent_node_Proj_Bound(ir_node *proj)
{
	ir_node *oldn  = proj;
	ir_node *bound = get_Proj_pred(proj);
	ir_node *idx   = get_Bound_index(bound);
	ir_node *pred  = skip_Proj(idx);
	int ret_tuple  = 0;

	if (idx == get_Bound_lower(bound))
		ret_tuple = 1;
	else if (is_Bound(pred)) {
		/*
		 * idx was Bounds checked previously, it is still valid if
		 * lower <= pred_lower && pred_upper <= upper.
		 */
		ir_node *lower = get_Bound_lower(bound);
		ir_node *upper = get_Bound_upper(bound);
		if (get_Bound_lower(pred) == lower &&
			get_Bound_upper(pred) == upper) {
			/*
			 * One could expect that we simply return the previous
			 * Bound here. However, this would be wrong, as we could
			 * add an exception Proj to a new location then.
			 * So, we must turn in into a tuple.
			 */
			ret_tuple = 1;
		}
	}
	if (ret_tuple) {
		/* Turn Bound into a tuple (mem, jmp, bad, idx) */
		switch (get_Proj_proj(proj)) {
		case pn_Bound_M:
			DBG_OPT_EXC_REM(proj);
			proj = get_Bound_mem(bound);
			break;
		case pn_Bound_X_except: {
			ir_graph *irg = get_irn_irg(proj);
			DBG_OPT_EXC_REM(proj);
			proj = get_irg_bad(irg);
			break;
		}
		case pn_Bound_res:
			proj = idx;
			DBG_OPT_ALGSIM0(oldn, proj, FS_OPT_NOP);
			break;
		default:
			/* cannot optimize pn_Bound_X_regular, handled in transform ... */
			break;
		}
	}
	return proj;
}  /* equivalent_node_Proj_Bound */

/**
 * Optimize an Exception Proj(Load) with a non-null address.
 */
static ir_node *equivalent_node_Proj_Load(ir_node *proj)
{
	if (get_opt_ldst_only_null_ptr_exceptions()) {
		if (get_irn_mode(proj) == mode_X) {
			ir_node *load = get_Proj_pred(proj);

			/* get the Load address */
			const ir_node *addr = get_Load_ptr(load);
			const ir_node *confirm;

			if (value_not_null(addr, &confirm)) {
				if (get_Proj_proj(proj) == pn_Load_X_except) {
					ir_graph *irg = get_irn_irg(proj);
					DBG_OPT_EXC_REM(proj);
					return get_irg_bad(irg);
				}
			}
		}
	}
	return proj;
}  /* equivalent_node_Proj_Load */

/**
 * Optimize an Exception Proj(Store) with a non-null address.
 */
static ir_node *equivalent_node_Proj_Store(ir_node *proj)
{
	if (get_opt_ldst_only_null_ptr_exceptions()) {
		if (get_irn_mode(proj) == mode_X) {
			ir_node *store = get_Proj_pred(proj);

			/* get the load/store address */
			const ir_node *addr = get_Store_ptr(store);
			const ir_node *confirm;

			if (value_not_null(addr, &confirm)) {
				if (get_Proj_proj(proj) == pn_Store_X_except) {
					ir_graph *irg = get_irn_irg(proj);
					DBG_OPT_EXC_REM(proj);
					return get_irg_bad(irg);
				}
			}
		}
	}
	return proj;
}  /* equivalent_node_Proj_Store */

/**
 * Does all optimizations on nodes that must be done on it's Proj's
 * because of creating new nodes.
 */
static ir_node *equivalent_node_Proj(ir_node *proj)
{
	ir_node *n = get_Proj_pred(proj);

	if (get_irn_mode(proj) == mode_X) {
		if (is_Block_dead(get_nodes_block(n))) {
			/* Remove dead control flow -- early gigo(). */
			ir_graph *irg = get_irn_irg(proj);
			return get_irg_bad(irg);
		}
	}
	if (n->op->ops.equivalent_node_Proj)
		return n->op->ops.equivalent_node_Proj(proj);
	return proj;
}  /* equivalent_node_Proj */

/**
 * Remove Id's.
 */
static ir_node *equivalent_node_Id(ir_node *n)
{
	ir_node *oldn = n;

	do {
		n = get_Id_pred(n);
	} while (is_Id(n));

	DBG_OPT_ID(oldn, n);
	return n;
}  /* equivalent_node_Id */

/**
 * Optimize a Mux.
 */
static ir_node *equivalent_node_Mux(ir_node *n)
{
	ir_node   *oldn = n, *sel = get_Mux_sel(n);
	ir_node   *n_t, *n_f;
	ir_tarval *ts = value_of(sel);

	/* Mux(true, f, t) == t */
	if (ts == tarval_b_true) {
		n = get_Mux_true(n);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_C);
		return n;
	}
	/* Mux(false, f, t) == f */
	if (ts == tarval_b_false) {
		n = get_Mux_false(n);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_C);
		return n;
	}
	n_t = get_Mux_true(n);
	n_f = get_Mux_false(n);

	/* Mux(v, x, T) == x */
	if (is_Unknown(n_f)) {
		n = n_t;
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_EQ);
		return n;
	}
	/* Mux(v, T, x) == x */
	if (is_Unknown(n_t)) {
		n = n_f;
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_EQ);
		return n;
	}

	/* Mux(v, x, x) == x */
	if (n_t == n_f) {
		n = n_t;
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_EQ);
		return n;
	}
	if (is_Proj(sel) && !mode_honor_signed_zeros(get_irn_mode(n))) {
		ir_node *cmp = get_Proj_pred(sel);
		long proj_nr = get_Proj_proj(sel);
		ir_node *f   = get_Mux_false(n);
		ir_node *t   = get_Mux_true(n);

		/*
		 * Note further that these optimization work even for floating point
		 * with NaN's because -NaN == NaN.
		 * However, if +0 and -0 is handled differently, we cannot use the first one.
		 */
		if (is_Cmp(cmp)) {
			ir_node *const cmp_l = get_Cmp_left(cmp);
			ir_node *const cmp_r = get_Cmp_right(cmp);

			switch (proj_nr) {
				case pn_Cmp_Eq:
					if ((cmp_l == t && cmp_r == f) || /* Mux(t == f, t, f) -> f */
							(cmp_l == f && cmp_r == t)) { /* Mux(f == t, t, f) -> f */
						n = f;
						DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_TRANSFORM);
						return n;
					}
					break;

				case pn_Cmp_Lg:
				case pn_Cmp_Ne:
					if ((cmp_l == t && cmp_r == f) || /* Mux(t != f, t, f) -> t */
							(cmp_l == f && cmp_r == t)) { /* Mux(f != t, t, f) -> t */
						n = t;
						DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_TRANSFORM);
						return n;
					}
					break;
			}

			/*
			 * Note: normalization puts the constant on the right side,
			 * so we check only one case.
			 */
			if (cmp_l == t && tarval_is_null(value_of(cmp_r))) {
				/* Mux(t CMP 0, X, t) */
				if (is_Minus(f) && get_Minus_op(f) == t) {
					/* Mux(t CMP 0, -t, t) */
					if (proj_nr == pn_Cmp_Eq) {
						/* Mux(t == 0, -t, t)  ==>  -t */
						n = f;
						DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_TRANSFORM);
					} else if (proj_nr == pn_Cmp_Lg || proj_nr == pn_Cmp_Ne) {
						/* Mux(t != 0, -t, t)  ==> t */
						n = t;
						DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_TRANSFORM);
					}
				}
			}
		}
	}
	return n;
}  /* equivalent_node_Mux */

/**
 * Remove Confirm nodes if setting is on.
 * Replace Confirms(x, '=', Constlike) by Constlike.
 */
static ir_node *equivalent_node_Confirm(ir_node *n)
{
	ir_node *pred = get_Confirm_value(n);
	pn_Cmp  pnc   = get_Confirm_cmp(n);

	while (is_Confirm(pred) && pnc == get_Confirm_cmp(pred)) {
		/*
		 * rare case: two identical Confirms one after another,
		 * replace the second one with the first.
		 */
		n    = pred;
		pred = get_Confirm_value(n);
		pnc  = get_Confirm_cmp(n);
	}
	return n;
}

/**
 * equivalent_node() returns a node equivalent to input n. It skips all nodes that
 * perform no actual computation, as, e.g., the Id nodes.  It does not create
 * new nodes.  It is therefore safe to free n if the node returned is not n.
 * If a node returns a Tuple we can not just skip it.  If the size of the
 * in array fits, we transform n into a tuple (e.g., Div).
 */
ir_node *equivalent_node(ir_node *n)
{
	if (n->op->ops.equivalent_node)
		return n->op->ops.equivalent_node(n);
	return n;
}  /* equivalent_node */

/**
 * Sets the default equivalent node operation for an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
static ir_op_ops *firm_set_default_equivalent_node(ir_opcode code, ir_op_ops *ops)
{
#define CASE(a)                                      \
	case iro_##a:                                    \
		ops->equivalent_node  = equivalent_node_##a; \
		break
#define CASE_PROJ(a)                                          \
	case iro_##a:                                             \
		ops->equivalent_node_Proj = equivalent_node_Proj_##a; \
		break

	switch (code) {
	CASE(Block);
	CASE(Jmp);
	CASE(Raise);
	CASE(Eor);
	CASE(Add);
	CASE(Shl);
	CASE(Shr);
	CASE(Shrs);
	CASE(Rotl);
	CASE(Sub);
	CASE(Not);
	CASE(Minus);
	CASE(Mul);
	CASE(Or);
	CASE(And);
	CASE(Conv);
	CASE(Phi);
	CASE(Sync);
	CASE_PROJ(Tuple);
	CASE_PROJ(Div);
	CASE_PROJ(Quot);
	CASE_PROJ(DivMod);
	CASE_PROJ(CopyB);
	CASE_PROJ(Bound);
	CASE_PROJ(Load);
	CASE_PROJ(Store);
	CASE(Proj);
	CASE(Id);
	CASE(Mux);
	CASE(Confirm);
	default:
		/* leave NULL */
		break;
	}

	return ops;
#undef CASE
#undef CASE_PROJ
}  /* firm_set_default_equivalent_node */

/**
 * Returns non-zero if a node is a Phi node
 * with all predecessors constant.
 */
static int is_const_Phi(ir_node *n)
{
	int i;

	if (! is_Phi(n) || get_irn_arity(n) == 0)
		return 0;
	for (i = get_irn_arity(n) - 1; i >= 0; --i) {
		if (! is_Const(get_irn_n(n, i)))
			return 0;
	}
	return 1;
}  /* is_const_Phi */

typedef ir_tarval *(*tarval_sub_type)(ir_tarval *a, ir_tarval *b, ir_mode *mode);
typedef ir_tarval *(*tarval_binop_type)(ir_tarval *a, ir_tarval *b);

/**
 * in reality eval_func should be tarval (*eval_func)() but incomplete
 * declarations are bad style and generate noisy warnings
 */
typedef void (*eval_func)(void);

/**
 * Wrapper for the tarval binop evaluation, tarval_sub has one more parameter.
 */
static ir_tarval *do_eval(eval_func eval, ir_tarval *a, ir_tarval *b, ir_mode *mode)
{
	if (eval == (eval_func) tarval_sub) {
		tarval_sub_type func = (tarval_sub_type)eval;

		return func(a, b, mode);
	} else {
		tarval_binop_type func = (tarval_binop_type)eval;

		return func(a, b);
	}
}

/**
 * Apply an evaluator on a binop with a constant operators (and one Phi).
 *
 * @param phi    the Phi node
 * @param other  the other operand
 * @param eval   an evaluator function
 * @param mode   the mode of the result, may be different from the mode of the Phi!
 * @param left   if non-zero, other is the left operand, else the right
 *
 * @return a new Phi node if the conversion was successful, NULL else
 */
static ir_node *apply_binop_on_phi(ir_node *phi, ir_tarval *other, eval_func eval, ir_mode *mode, int left)
{
	ir_tarval *tv;
	void      **res;
	ir_node   *pred;
	ir_graph  *irg;
	int       i, n = get_irn_arity(phi);

	NEW_ARR_A(void *, res, n);
	if (left) {
		for (i = 0; i < n; ++i) {
			pred = get_irn_n(phi, i);
			tv   = get_Const_tarval(pred);
			tv   = do_eval(eval, other, tv, mode);

			if (tv == tarval_bad) {
				/* folding failed, bad */
				return NULL;
			}
			res[i] = tv;
		}
	} else {
		for (i = 0; i < n; ++i) {
			pred = get_irn_n(phi, i);
			tv   = get_Const_tarval(pred);
			tv   = do_eval(eval, tv, other, mode);

			if (tv == tarval_bad) {
				/* folding failed, bad */
				return 0;
			}
			res[i] = tv;
		}
	}
	irg = get_irn_irg(phi);
	for (i = 0; i < n; ++i) {
		pred = get_irn_n(phi, i);
		res[i] = new_r_Const(irg, res[i]);
	}
	return new_r_Phi(get_nodes_block(phi), n, (ir_node **)res, mode);
}  /* apply_binop_on_phi */

/**
 * Apply an evaluator on a binop with two constant Phi.
 *
 * @param a      the left Phi node
 * @param b      the right Phi node
 * @param eval   an evaluator function
 * @param mode   the mode of the result, may be different from the mode of the Phi!
 *
 * @return a new Phi node if the conversion was successful, NULL else
 */
static ir_node *apply_binop_on_2_phis(ir_node *a, ir_node *b, eval_func eval, ir_mode *mode)
{
	ir_tarval *tv_l, *tv_r, *tv;
	void     **res;
	ir_node  *pred;
	ir_graph *irg;
	int      i, n;

	if (get_nodes_block(a) != get_nodes_block(b))
		return NULL;

	n = get_irn_arity(a);
	NEW_ARR_A(void *, res, n);

	for (i = 0; i < n; ++i) {
		pred = get_irn_n(a, i);
		tv_l = get_Const_tarval(pred);
		pred = get_irn_n(b, i);
		tv_r = get_Const_tarval(pred);
		tv   = do_eval(eval, tv_l, tv_r, mode);

		if (tv == tarval_bad) {
			/* folding failed, bad */
			return NULL;
		}
		res[i] = tv;
	}
	irg = get_irn_irg(a);
	for (i = 0; i < n; ++i) {
		pred = get_irn_n(a, i);
		res[i] = new_r_Const(irg, res[i]);
	}
	return new_r_Phi(get_nodes_block(a), n, (ir_node **)res, mode);
}  /* apply_binop_on_2_phis */

/**
 * Apply an evaluator on a unop with a constant operator (a Phi).
 *
 * @param phi    the Phi node
 * @param eval   an evaluator function
 *
 * @return a new Phi node if the conversion was successful, NULL else
 */
static ir_node *apply_unop_on_phi(ir_node *phi, ir_tarval *(*eval)(ir_tarval *))
{
	ir_tarval *tv;
	void     **res;
	ir_node  *pred;
	ir_mode  *mode;
	ir_graph *irg;
	int      i, n = get_irn_arity(phi);

	NEW_ARR_A(void *, res, n);
	for (i = 0; i < n; ++i) {
		pred = get_irn_n(phi, i);
		tv   = get_Const_tarval(pred);
		tv   = eval(tv);

		if (tv == tarval_bad) {
			/* folding failed, bad */
			return 0;
		}
		res[i] = tv;
	}
	mode = get_irn_mode(phi);
	irg  = get_irn_irg(phi);
	for (i = 0; i < n; ++i) {
		pred = get_irn_n(phi, i);
		res[i] = new_r_Const(irg, res[i]);
	}
	return new_r_Phi(get_nodes_block(phi), n, (ir_node **)res, mode);
}  /* apply_unop_on_phi */

/**
 * Apply a conversion on a constant operator (a Phi).
 *
 * @param phi    the Phi node
 *
 * @return a new Phi node if the conversion was successful, NULL else
 */
static ir_node *apply_conv_on_phi(ir_node *phi, ir_mode *mode)
{
	ir_tarval *tv;
	void     **res;
	ir_node  *pred;
	ir_graph *irg;
	int      i, n = get_irn_arity(phi);

	NEW_ARR_A(void *, res, n);
	for (i = 0; i < n; ++i) {
		pred = get_irn_n(phi, i);
		tv   = get_Const_tarval(pred);
		tv   = tarval_convert_to(tv, mode);

		if (tv == tarval_bad) {
			/* folding failed, bad */
			return 0;
		}
		res[i] = tv;
	}
	irg = get_irn_irg(phi);
	for (i = 0; i < n; ++i) {
		pred = get_irn_n(phi, i);
		res[i] = new_r_Const(irg, res[i]);
	}
	return new_r_Phi(get_nodes_block(phi), n, (ir_node **)res, mode);
}  /* apply_conv_on_phi */

/**
 * Transform AddP(P, ConvIs(Iu)), AddP(P, ConvIu(Is)) and
 * SubP(P, ConvIs(Iu)), SubP(P, ConvIu(Is)).
 * If possible, remove the Conv's.
 */
static ir_node *transform_node_AddSub(ir_node *n)
{
	ir_mode *mode = get_irn_mode(n);

	if (mode_is_reference(mode)) {
		ir_node *left     = get_binop_left(n);
		ir_node *right    = get_binop_right(n);
		unsigned ref_bits = get_mode_size_bits(mode);

		if (is_Conv(left)) {
			ir_mode *lmode = get_irn_mode(left);
			unsigned bits = get_mode_size_bits(lmode);

			if (ref_bits == bits &&
			    mode_is_int(lmode) &&
			    get_mode_arithmetic(lmode) == irma_twos_complement) {
				ir_node *pre      = get_Conv_op(left);
				ir_mode *pre_mode = get_irn_mode(pre);

				if (mode_is_int(pre_mode) &&
				    get_mode_size_bits(pre_mode) == bits &&
				    get_mode_arithmetic(pre_mode) == irma_twos_complement) {
					/* ok, this conv just changes to sign, moreover the calculation
					 * is done with same number of bits as our address mode, so
					 * we can ignore the conv as address calculation can be viewed
					 * as either signed or unsigned
					 */
					set_binop_left(n, pre);
				}
			}
		}

		if (is_Conv(right)) {
			ir_mode *rmode = get_irn_mode(right);
			unsigned bits = get_mode_size_bits(rmode);

			if (ref_bits == bits &&
			    mode_is_int(rmode) &&
			    get_mode_arithmetic(rmode) == irma_twos_complement) {
				ir_node *pre      = get_Conv_op(right);
				ir_mode *pre_mode = get_irn_mode(pre);

				if (mode_is_int(pre_mode) &&
				    get_mode_size_bits(pre_mode) == bits &&
				    get_mode_arithmetic(pre_mode) == irma_twos_complement) {
					/* ok, this conv just changes to sign, moreover the calculation
					 * is done with same number of bits as our address mode, so
					 * we can ignore the conv as address calculation can be viewed
					 * as either signed or unsigned
					 */
					set_binop_right(n, pre);
				}
			}
		}

		/* let address arithmetic use unsigned modes */
		if (is_Const(right)) {
			ir_mode *rmode = get_irn_mode(right);

			if (mode_is_signed(rmode) && get_mode_arithmetic(rmode) == irma_twos_complement) {
				/* convert a AddP(P, *s) into AddP(P, *u) */
				ir_mode *nm = get_reference_mode_unsigned_eq(mode);

				ir_node *pre = new_r_Conv(get_nodes_block(n), right, nm);
				set_binop_right(n, pre);
			}
		}
	}

	return n;
}  /* transform_node_AddSub */

#define HANDLE_BINOP_PHI(eval, a, b, c, mode)                     \
  do {                                                            \
  c = NULL;                                                       \
  if (is_Const(b) && is_const_Phi(a)) {                           \
    /* check for Op(Phi, Const) */                                \
    c = apply_binop_on_phi(a, get_Const_tarval(b), eval, mode, 0);\
  }                                                               \
  else if (is_Const(a) && is_const_Phi(b)) {                      \
    /* check for Op(Const, Phi) */                                \
    c = apply_binop_on_phi(b, get_Const_tarval(a), eval, mode, 1);\
  }                                                               \
  else if (is_const_Phi(a) && is_const_Phi(b)) {                  \
    /* check for Op(Phi, Phi) */                                  \
    c = apply_binop_on_2_phis(a, b, eval, mode);                  \
  }                                                               \
  if (c) {                                                        \
    DBG_OPT_ALGSIM0(oldn, c, FS_OPT_CONST_PHI);                   \
    return c;                                                     \
  }                                                               \
  } while(0)

#define HANDLE_UNOP_PHI(eval, a, c)               \
  do {                                            \
  c = NULL;                                       \
  if (is_const_Phi(a)) {                          \
    /* check for Op(Phi) */                       \
    c = apply_unop_on_phi(a, eval);               \
    if (c) {                                      \
      DBG_OPT_ALGSIM0(oldn, c, FS_OPT_CONST_PHI); \
      return c;                                   \
    }                                             \
  }                                               \
  } while(0)

/**
 * Do the AddSub optimization, then Transform
 *   Constant folding on Phi
 *   Add(a,a)          -> Mul(a, 2)
 *   Add(Mul(a, x), a) -> Mul(a, x+1)
 * if the mode is integer or float.
 * Transform Add(a,-b) into Sub(a,b).
 * Reassociation might fold this further.
 */
static ir_node *transform_node_Add(ir_node *n)
{
	ir_mode *mode;
	ir_node *a, *b, *c, *oldn = n;
	vrp_attr *a_vrp, *b_vrp;

	n = transform_node_AddSub(n);

	a = get_Add_left(n);
	b = get_Add_right(n);

	mode = get_irn_mode(n);

	if (mode_is_reference(mode)) {
		ir_mode *lmode = get_irn_mode(a);

		if (is_Const(b) && is_Const_null(b) && mode_is_int(lmode)) {
			/* an Add(a, NULL) is a hidden Conv */
			dbg_info *dbg = get_irn_dbg_info(n);
			return new_rd_Conv(dbg, get_nodes_block(n), a, mode);
		}
	}

	HANDLE_BINOP_PHI((eval_func) tarval_add, a, b, c, mode);

	/* for FP these optimizations are only allowed if fp_strict_algebraic is disabled */
	if (mode_is_float(mode)) {
		ir_graph *irg = get_irn_irg(n);
		if (get_irg_fp_model(irg) & fp_strict_algebraic)
			return n;
	}

	if (mode_is_num(mode)) {
		ir_graph *irg = get_irn_irg(n);
		/* the following code leads to endless recursion when Mul are replaced by a simple instruction chain */
		if (!is_irg_state(irg, IR_GRAPH_STATE_ARCH_DEP)
				&& a == b && mode_is_int(mode)) {
			ir_node *block = get_nodes_block(n);

			n = new_rd_Mul(
				get_irn_dbg_info(n),
				block,
				a,
				new_r_Const_long(irg, mode, 2),
				mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_A_A);
			return n;
		}
		if (is_Minus(a)) {
			n = new_rd_Sub(
					get_irn_dbg_info(n),
					get_nodes_block(n),
					b,
					get_Minus_op(a),
					mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_A_MINUS_B);
			return n;
		}
		if (is_Minus(b)) {
			n = new_rd_Sub(
					get_irn_dbg_info(n),
					get_nodes_block(n),
					a,
					get_Minus_op(b),
					mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_A_MINUS_B);
			return n;
		}
		if (get_mode_arithmetic(mode) == irma_twos_complement) {
			/* Here we rely on constants be on the RIGHT side */
			if (is_Not(a)) {
				ir_node *op = get_Not_op(a);

				if (is_Const(b) && is_Const_one(b)) {
					/* ~x + 1 = -x */
					ir_node *blk = get_nodes_block(n);
					n = new_rd_Minus(get_irn_dbg_info(n), blk, op, mode);
					DBG_OPT_ALGSIM0(oldn, n, FS_OPT_NOT_PLUS_1);
					return n;
				}
				if (op == b) {
					/* ~x + x = -1 */
					n = new_r_Const(irg, get_mode_minus_one(mode));
					DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_X_NOT_X);
					return n;
				}
			}
			if (is_Not(b)) {
				ir_node *op = get_Not_op(b);

				if (op == a) {
					/* x + ~x = -1 */
					n = new_r_Const(irg, get_mode_minus_one(mode));
					DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_X_NOT_X);
					return n;
				}
			}
		}
	}

	a_vrp = vrp_get_info(a);
	b_vrp = vrp_get_info(b);

	if (a_vrp && b_vrp) {
		ir_tarval *c = tarval_and(a_vrp->bits_not_set, b_vrp->bits_not_set);

		if (tarval_is_null(c)) {
			dbg_info *dbgi  = get_irn_dbg_info(n);
			return new_rd_Or(dbgi, get_nodes_block(n), a, b, mode);
		}
	}
	return n;
}  /* transform_node_Add */

/**
 * returns -cnst or NULL if impossible
 */
static ir_node *const_negate(ir_node *cnst)
{
	ir_tarval *tv    = tarval_neg(get_Const_tarval(cnst));
	dbg_info  *dbgi  = get_irn_dbg_info(cnst);
	ir_graph  *irg   = get_irn_irg(cnst);
	if (tv == tarval_bad) return NULL;
	return new_rd_Const(dbgi, irg, tv);
}

/**
 * Do the AddSub optimization, then Transform
 *   Constant folding on Phi
 *   Sub(0,a)          -> Minus(a)
 *   Sub(Mul(a, x), a) -> Mul(a, x-1)
 *   Sub(Sub(x, y), b) -> Sub(x, Add(y,b))
 *   Sub(Add(a, x), x) -> a
 *   Sub(x, Add(x, a)) -> -a
 *   Sub(x, Const)     -> Add(x, -Const)
 */
static ir_node *transform_node_Sub(ir_node *n)
{
	ir_mode *mode;
	ir_node *oldn = n;
	ir_node *a, *b, *c;

	n = transform_node_AddSub(n);

	a = get_Sub_left(n);
	b = get_Sub_right(n);

	mode = get_irn_mode(n);

	if (mode_is_int(mode)) {
		ir_mode *lmode = get_irn_mode(a);

		if (is_Const(b) && is_Const_null(b) && mode_is_reference(lmode)) {
			/* a Sub(a, NULL) is a hidden Conv */
			dbg_info *dbg = get_irn_dbg_info(n);
			n = new_rd_Conv(dbg, get_nodes_block(n), a, mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_TO_CONV);
			return n;
		}

		if (mode == lmode                                     &&
		    get_mode_arithmetic(mode) == irma_twos_complement &&
		    is_Const(a)                                       &&
		    get_Const_tarval(a) == get_mode_minus_one(mode)) {
			/* -1 - x -> ~x */
			dbg_info *dbg = get_irn_dbg_info(n);
			n = new_rd_Not(dbg, get_nodes_block(n), b, mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_TO_NOT);
			return n;
		}
	}

restart:
	HANDLE_BINOP_PHI((eval_func) tarval_sub, a, b, c, mode);

	/* for FP these optimizations are only allowed if fp_strict_algebraic is disabled */
	if (mode_is_float(mode)) {
		ir_graph *irg = get_irn_irg(n);
		if (get_irg_fp_model(irg) & fp_strict_algebraic)
			return n;
	}

	if (is_Const(b) && !mode_is_reference(get_irn_mode(b))) {
		/* a - C -> a + (-C) */
		ir_node *cnst = const_negate(b);
		if (cnst != NULL) {
			ir_node  *block = get_nodes_block(n);
			dbg_info *dbgi  = get_irn_dbg_info(n);

			n = new_rd_Add(dbgi, block, a, cnst, mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_TO_ADD);
			return n;
		}
	}

	if (is_Minus(a)) { /* (-a) - b -> -(a + b) */
		dbg_info *dbg   = get_irn_dbg_info(n);
		ir_node  *block = get_nodes_block(n);
		ir_node  *left  = get_Minus_op(a);
		ir_node  *add   = new_rd_Add(dbg, block, left, b, mode);

		n = new_rd_Minus(dbg, block, add, mode);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_TO_ADD);
		return n;
	} else if (is_Minus(b)) { /* a - (-b) -> a + b */
		dbg_info *dbg   = get_irn_dbg_info(n);
		ir_node  *block = get_nodes_block(n);
		ir_node  *right = get_Minus_op(b);

		n = new_rd_Add(dbg, block, a, right, mode);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_MINUS);
		return n;
	} else if (is_Sub(b)) {
		/* a - (b - c) -> a + (c - b)
		 *             -> (a - b) + c iff (b - c) is a pointer */
		dbg_info *s_dbg   = get_irn_dbg_info(b);
		ir_node  *s_block = get_nodes_block(b);
		ir_node  *s_left  = get_Sub_left(b);
		ir_node  *s_right = get_Sub_right(b);
		ir_mode  *s_mode  = get_irn_mode(b);
		if (mode_is_reference(s_mode)) {
			ir_node  *sub     = new_rd_Sub(s_dbg, s_block, a, s_left, mode);
			dbg_info *a_dbg   = get_irn_dbg_info(n);
			ir_node  *a_block = get_nodes_block(n);

			if (s_mode != mode)
				s_right = new_r_Conv(a_block, s_right, mode);
			n = new_rd_Add(a_dbg, a_block, sub, s_right, mode);
		} else {
			ir_node  *sub     = new_rd_Sub(s_dbg, s_block, s_right, s_left, s_mode);
			dbg_info *a_dbg   = get_irn_dbg_info(n);
			ir_node  *a_block = get_nodes_block(n);

			n = new_rd_Add(a_dbg, a_block, a, sub, mode);
		}
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_TO_ADD);
		return n;
	} else if (is_Mul(b)) { /* a - (b * C) -> a + (b * -C) */
		ir_node *m_right = get_Mul_right(b);
		if (is_Const(m_right)) {
			ir_node *cnst2 = const_negate(m_right);
			if (cnst2 != NULL) {
				dbg_info *m_dbg   = get_irn_dbg_info(b);
				ir_node  *m_block = get_nodes_block(b);
				ir_node  *m_left  = get_Mul_left(b);
				ir_mode  *m_mode  = get_irn_mode(b);
				ir_node  *mul     = new_rd_Mul(m_dbg, m_block, m_left, cnst2, m_mode);
				dbg_info *a_dbg   = get_irn_dbg_info(n);
				ir_node  *a_block = get_nodes_block(n);

				n = new_rd_Add(a_dbg, a_block, a, mul, mode);
				DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_TO_ADD);
				return n;
			}
		}
	}

	/* Beware of Sub(P, P) which cannot be optimized into a simple Minus ... */
	if (mode_is_num(mode) && mode == get_irn_mode(a) && is_Const(a) && is_Const_null(a)) {
		n = new_rd_Minus(
				get_irn_dbg_info(n),
				get_nodes_block(n),
				b,
				mode);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_0_A);
		return n;
	}
	if (is_Add(a)) {
		if (mode_wrap_around(mode)) {
			ir_node *left  = get_Add_left(a);
			ir_node *right = get_Add_right(a);

			/* FIXME: Does the Conv's work only for two complement or generally? */
			if (left == b) {
				if (mode != get_irn_mode(right)) {
					/* This Sub is an effective Cast */
					right = new_r_Conv(get_nodes_block(n), right, mode);
				}
				n = right;
				DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_ADD_SUB);
				return n;
			} else if (right == b) {
				if (mode != get_irn_mode(left)) {
					/* This Sub is an effective Cast */
					left = new_r_Conv(get_nodes_block(n), left, mode);
				}
				n = left;
				DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_ADD_SUB);
				return n;
			}
		}
	}
	if (is_Add(b)) {
		if (mode_wrap_around(mode)) {
			ir_node *left  = get_Add_left(b);
			ir_node *right = get_Add_right(b);

			/* FIXME: Does the Conv's work only for two complement or generally? */
			if (left == a) {
				ir_mode *r_mode = get_irn_mode(right);

				n = new_r_Minus(get_nodes_block(n), right, r_mode);
				if (mode != r_mode) {
					/* This Sub is an effective Cast */
					n = new_r_Conv(get_nodes_block(n), n, mode);
				}
				DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_ADD_SUB);
				return n;
			} else if (right == a) {
				ir_mode *l_mode = get_irn_mode(left);

				n = new_r_Minus(get_nodes_block(n), left, l_mode);
				if (mode != l_mode) {
					/* This Sub is an effective Cast */
					n = new_r_Conv(get_nodes_block(n), n, mode);
				}
				DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_ADD_SUB);
				return n;
			}
		}
	}
	if (mode_is_int(mode) && is_Conv(a) && is_Conv(b)) {
		ir_mode *mode = get_irn_mode(a);

		if (mode == get_irn_mode(b)) {
			ir_mode *ma, *mb;
			ir_node *op_a = get_Conv_op(a);
			ir_node *op_b = get_Conv_op(b);

			/* check if it's allowed to skip the conv */
			ma = get_irn_mode(op_a);
			mb = get_irn_mode(op_b);

			if (mode_is_reference(ma) && mode_is_reference(mb)) {
				/* SubInt(ConvInt(aP), ConvInt(bP)) -> SubInt(aP,bP) */
				a = op_a; b = op_b;
				set_Sub_left(n, a);
				set_Sub_right(n, b);

				goto restart;
			}
		}
	}
	/* do NOT execute this code if reassociation is enabled, it does the inverse! */
	if (!is_reassoc_running() && is_Mul(a)) {
		ir_node *ma = get_Mul_left(a);
		ir_node *mb = get_Mul_right(a);

		if (ma == b) {
			ir_node  *blk = get_nodes_block(n);
			ir_graph *irg = get_irn_irg(n);
			n = new_rd_Mul(
					get_irn_dbg_info(n),
					blk,
					ma,
					new_rd_Sub(
						get_irn_dbg_info(n),
						blk,
						mb,
						new_r_Const(irg, get_mode_one(mode)),
						mode),
					mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_MUL_A_X_A);
			return n;
		} else if (mb == b) {
			ir_node  *blk = get_nodes_block(n);
			ir_graph *irg = get_irn_irg(n);
			n = new_rd_Mul(
					get_irn_dbg_info(n),
					blk,
					mb,
					new_rd_Sub(
						get_irn_dbg_info(n),
						blk,
						ma,
						new_r_Const(irg, get_mode_one(mode)),
						mode),
					mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_MUL_A_X_A);
			return n;
		}
	}
	if (is_Sub(a)) { /* (x - y) - b -> x - (y + b) */
		ir_node *x        = get_Sub_left(a);
		ir_node *y        = get_Sub_right(a);
		ir_node *blk      = get_nodes_block(n);
		ir_mode *m_b      = get_irn_mode(b);
		ir_mode *m_y      = get_irn_mode(y);
		ir_mode *add_mode;
		ir_node *add;

		/* Determine the right mode for the Add. */
		if (m_b == m_y)
			add_mode = m_b;
		else if (mode_is_reference(m_b))
			add_mode = m_b;
		else if (mode_is_reference(m_y))
			add_mode = m_y;
		else {
			/*
			 * Both modes are different but none is reference,
			 * happens for instance in SubP(SubP(P, Iu), Is).
			 * We have two possibilities here: Cast or ignore.
			 * Currently we ignore this case.
			 */
			return n;
		}

		add = new_r_Add(blk, y, b, add_mode);

		n = new_rd_Sub(get_irn_dbg_info(n), blk, x, add, mode);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_SUB_X_Y_Z);
		return n;
	}

	if (get_mode_arithmetic(mode) == irma_twos_complement) {
		if (is_Const(a) && is_Not(b)) {
			/* c - ~X = X + (c+1) */
			ir_tarval *tv = get_Const_tarval(a);

			tv = tarval_add(tv, get_mode_one(mode));
			if (tv != tarval_bad) {
				ir_node  *blk = get_nodes_block(n);
				ir_graph *irg = get_irn_irg(n);
				ir_node *c = new_r_Const(irg, tv);
				n = new_rd_Add(get_irn_dbg_info(n), blk, get_Not_op(b), c, mode);
				DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_C_NOT_X);
				return n;
			}
		}
	}
	return n;
}  /* transform_node_Sub */

/**
 * Several transformation done on n*n=2n bits mul.
 * These transformations must be done here because new nodes may be produced.
 */
static ir_node *transform_node_Mul2n(ir_node *n, ir_mode *mode)
{
	ir_node   *oldn  = n;
	ir_node   *a     = get_Mul_left(n);
	ir_node   *b     = get_Mul_right(n);
	ir_tarval *ta    = value_of(a);
	ir_tarval *tb    = value_of(b);
	ir_mode   *smode = get_irn_mode(a);

	if (ta == get_mode_one(smode)) {
		/* (L)1 * (L)b = (L)b */
		ir_node *blk = get_nodes_block(n);
		n = new_rd_Conv(get_irn_dbg_info(n), blk, b, mode);
		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_1);
		return n;
	}
	else if (ta == get_mode_minus_one(smode)) {
		/* (L)-1 * (L)b = (L)b */
		ir_node *blk = get_nodes_block(n);
		n = new_rd_Minus(get_irn_dbg_info(n), blk, b, smode);
		n = new_rd_Conv(get_irn_dbg_info(n), blk, n, mode);
		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_MINUS_1);
		return n;
	}
	if (tb == get_mode_one(smode)) {
		/* (L)a * (L)1 = (L)a */
		ir_node *blk = get_irn_n(a, -1);
		n = new_rd_Conv(get_irn_dbg_info(n), blk, a, mode);
		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_1);
		return n;
	}
	else if (tb == get_mode_minus_one(smode)) {
		/* (L)a * (L)-1 = (L)-a */
		ir_node *blk = get_nodes_block(n);
		n = new_rd_Minus(get_irn_dbg_info(n), blk, a, smode);
		n = new_rd_Conv(get_irn_dbg_info(n), blk, n, mode);
		DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_MINUS_1);
		return n;
	}
	return n;
}

/**
 * Transform Mul(a,-1) into -a.
 * Do constant evaluation of Phi nodes.
 * Do architecture dependent optimizations on Mul nodes
 */
static ir_node *transform_node_Mul(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_mode *mode = get_irn_mode(n);
	ir_node *a = get_Mul_left(n);
	ir_node *b = get_Mul_right(n);

	if (is_Bad(a) || is_Bad(b))
		return n;

	if (mode != get_irn_mode(a))
		return transform_node_Mul2n(n, mode);

	HANDLE_BINOP_PHI((eval_func) tarval_mul, a, b, c, mode);

	if (mode_is_signed(mode)) {
		ir_node *r = NULL;

		if (value_of(a) == get_mode_minus_one(mode))
			r = b;
		else if (value_of(b) == get_mode_minus_one(mode))
			r = a;
		if (r) {
			n = new_rd_Minus(get_irn_dbg_info(n), get_nodes_block(n), r, mode);
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_MINUS_1);
			return n;
		}
	}
	if (is_Minus(a)) {
		if (is_Const(b)) { /* (-a) * const -> a * -const */
			ir_node *cnst = const_negate(b);
			if (cnst != NULL) {
				dbg_info *dbgi  = get_irn_dbg_info(n);
				ir_node  *block = get_nodes_block(n);
				n = new_rd_Mul(dbgi, block, get_Minus_op(a), cnst, mode);
				DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_MINUS_1);
				return n;
			}
		} else if (is_Minus(b)) { /* (-a) * (-b) -> a * b */
			dbg_info *dbgi  = get_irn_dbg_info(n);
			ir_node  *block = get_nodes_block(n);
			n = new_rd_Mul(dbgi, block, get_Minus_op(a), get_Minus_op(b), mode);
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_MINUS_MINUS);
			return n;
		} else if (is_Sub(b)) { /* (-a) * (b - c) -> a * (c - b) */
			ir_node  *sub_l = get_Sub_left(b);
			ir_node  *sub_r = get_Sub_right(b);
			dbg_info *dbgi  = get_irn_dbg_info(n);
			ir_node  *block = get_nodes_block(n);
			ir_node  *new_b = new_rd_Sub(dbgi, block, sub_r, sub_l, mode);
			n = new_rd_Mul(dbgi, block, get_Minus_op(a), new_b, mode);
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_MINUS);
			return n;
		}
	} else if (is_Minus(b)) {
		if (is_Sub(a)) { /* (a - b) * (-c) -> (b - a) * c */
			ir_node  *sub_l = get_Sub_left(a);
			ir_node  *sub_r = get_Sub_right(a);
			dbg_info *dbgi  = get_irn_dbg_info(n);
			ir_node  *block = get_nodes_block(n);
			ir_node  *new_a = new_rd_Sub(dbgi, block, sub_r, sub_l, mode);
			n = new_rd_Mul(dbgi, block, new_a, get_Minus_op(b), mode);
			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_MINUS);
			return n;
		}
	} else if (is_Shl(a)) {
		ir_node *const shl_l = get_Shl_left(a);
		if (is_Const(shl_l) && is_Const_one(shl_l)) {
			/* (1 << x) * b -> b << x */
			dbg_info *const dbgi  = get_irn_dbg_info(n);
			ir_node  *const block = get_nodes_block(n);
			ir_node  *const shl_r = get_Shl_right(a);
			n = new_rd_Shl(dbgi, block, b, shl_r, mode);
			// TODO add me DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_SHIFT);
			return n;
		}
	} else if (is_Shl(b)) {
		ir_node *const shl_l = get_Shl_left(b);
		if (is_Const(shl_l) && is_Const_one(shl_l)) {
			/* a * (1 << x) -> a << x */
			dbg_info *const dbgi  = get_irn_dbg_info(n);
			ir_node  *const block = get_nodes_block(n);
			ir_node  *const shl_r = get_Shl_right(b);
			n = new_rd_Shl(dbgi, block, a, shl_r, mode);
			// TODO add me DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_SHIFT);
			return n;
		}
	}
	if (get_mode_arithmetic(mode) == irma_ieee754) {
		if (is_Const(a)) {
			ir_tarval *tv = get_Const_tarval(a);
			if (tarval_ieee754_get_exponent(tv) == 1 && tarval_ieee754_zero_mantissa(tv)
					&& !tarval_is_negative(tv)) {
				/* 2.0 * b = b + b */
				n = new_rd_Add(get_irn_dbg_info(n), get_nodes_block(n), b, b, mode);
				DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_ADD_A_A);
				return n;
			}
		}
		else if (is_Const(b)) {
			ir_tarval *tv = get_Const_tarval(b);
			if (tarval_ieee754_get_exponent(tv) == 1 && tarval_ieee754_zero_mantissa(tv)
					&& !tarval_is_negative(tv)) {
				/* a * 2.0 = a + a */
				n = new_rd_Add(get_irn_dbg_info(n), get_nodes_block(n), a, a, mode);
				DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_ADD_A_A);
				return n;
			}
		}
	}
	return arch_dep_replace_mul_with_shifts(n);
}  /* transform_node_Mul */

/**
 * Transform a Div Node.
 */
static ir_node *transform_node_Div(ir_node *n)
{
	ir_mode *mode = get_Div_resmode(n);
	ir_node *a = get_Div_left(n);
	ir_node *b = get_Div_right(n);
	ir_node *value;
	const ir_node *dummy;

	if (is_Const(b) && is_const_Phi(a)) {
		/* check for Div(Phi, Const) */
		value = apply_binop_on_phi(a, get_Const_tarval(b), (eval_func) tarval_div, mode, 0);
		if (value) {
			DBG_OPT_ALGSIM0(n, value, FS_OPT_CONST_PHI);
			goto make_tuple;
		}
	}
	else if (is_Const(a) && is_const_Phi(b)) {
		/* check for Div(Const, Phi) */
		value = apply_binop_on_phi(b, get_Const_tarval(a), (eval_func) tarval_div, mode, 1);
		if (value) {
			DBG_OPT_ALGSIM0(n, value, FS_OPT_CONST_PHI);
			goto make_tuple;
		}
	}
	else if (is_const_Phi(a) && is_const_Phi(b)) {
		/* check for Div(Phi, Phi) */
		value = apply_binop_on_2_phis(a, b, (eval_func) tarval_div, mode);
		if (value) {
			DBG_OPT_ALGSIM0(n, value, FS_OPT_CONST_PHI);
			goto make_tuple;
		}
	}

	value = n;

	if (a == b && value_not_zero(a, &dummy)) {
		ir_graph *irg = get_irn_irg(n);
		/* BEWARE: we can optimize a/a to 1 only if this cannot cause a exception */
		value = new_r_Const(irg, get_mode_one(mode));
		DBG_OPT_CSTEVAL(n, value);
		goto make_tuple;
	} else {
		if (mode_is_signed(mode) && is_Const(b)) {
			ir_tarval *tv = get_Const_tarval(b);

			if (tv == get_mode_minus_one(mode)) {
				/* a / -1 */
				value = new_rd_Minus(get_irn_dbg_info(n), get_nodes_block(n), a, mode);
				DBG_OPT_CSTEVAL(n, value);
				goto make_tuple;
			}
		}
		/* Try architecture dependent optimization */
		value = arch_dep_replace_div_by_const(n);
	}

	if (value != n) {
		ir_node *mem, *blk;
		ir_graph *irg;

make_tuple:
		/* Turn Div into a tuple (mem, jmp, bad, value) */
		mem = get_Div_mem(n);
		blk = get_nodes_block(n);
		irg = get_irn_irg(blk);

		/* skip a potential Pin */
		mem = skip_Pin(mem);
		turn_into_tuple(n, pn_Div_max);
		set_Tuple_pred(n, pn_Div_M,         mem);
		set_Tuple_pred(n, pn_Div_X_regular, new_r_Jmp(blk));
		set_Tuple_pred(n, pn_Div_X_except,  new_r_Bad(irg));
		set_Tuple_pred(n, pn_Div_res,       value);
	}
	return n;
}  /* transform_node_Div */

/**
 * Transform a Mod node.
 */
static ir_node *transform_node_Mod(ir_node *n)
{
	ir_mode   *mode = get_Mod_resmode(n);
	ir_node   *a    = get_Mod_left(n);
	ir_node   *b    = get_Mod_right(n);
	ir_graph  *irg;
	ir_node   *value;
	ir_tarval *tv;

	if (is_Const(b) && is_const_Phi(a)) {
		/* check for Div(Phi, Const) */
		value = apply_binop_on_phi(a, get_Const_tarval(b), (eval_func) tarval_mod, mode, 0);
		if (value) {
			DBG_OPT_ALGSIM0(n, value, FS_OPT_CONST_PHI);
			goto make_tuple;
		}
	}
	else if (is_Const(a) && is_const_Phi(b)) {
		/* check for Div(Const, Phi) */
		value = apply_binop_on_phi(b, get_Const_tarval(a), (eval_func) tarval_mod, mode, 1);
		if (value) {
			DBG_OPT_ALGSIM0(n, value, FS_OPT_CONST_PHI);
			goto make_tuple;
		}
	}
	else if (is_const_Phi(a) && is_const_Phi(b)) {
		/* check for Div(Phi, Phi) */
		value = apply_binop_on_2_phis(a, b, (eval_func) tarval_mod, mode);
		if (value) {
			DBG_OPT_ALGSIM0(n, value, FS_OPT_CONST_PHI);
			goto make_tuple;
		}
	}

	value = n;
	tv = value_of(n);
	irg = get_irn_irg(n);
	if (tv != tarval_bad) {
		value = new_r_Const(irg, tv);

		DBG_OPT_CSTEVAL(n, value);
		goto make_tuple;
	} else {
		ir_node       *a = get_Mod_left(n);
		ir_node       *b = get_Mod_right(n);
		const ir_node *dummy;

		if (a == b && value_not_zero(a, &dummy)) {
			/* BEWARE: we can optimize a%a to 0 only if this cannot cause a exception */
			value = new_r_Const(irg, get_mode_null(mode));
			DBG_OPT_CSTEVAL(n, value);
			goto make_tuple;
		} else {
			if (mode_is_signed(mode) && is_Const(b)) {
				ir_tarval *tv = get_Const_tarval(b);

				if (tv == get_mode_minus_one(mode)) {
					/* a % -1 = 0 */
					value = new_r_Const(irg, get_mode_null(mode));
					DBG_OPT_CSTEVAL(n, value);
					goto make_tuple;
				}
			}
			/* Try architecture dependent optimization */
			value = arch_dep_replace_mod_by_const(n);
		}
	}

	if (value != n) {
		ir_node *mem, *blk;
		ir_graph *irg;

make_tuple:
		/* Turn Mod into a tuple (mem, jmp, bad, value) */
		mem = get_Mod_mem(n);
		blk = get_nodes_block(n);
		irg = get_irn_irg(blk);

		/* skip a potential Pin */
		mem = skip_Pin(mem);
		turn_into_tuple(n, pn_Mod_max);
		set_Tuple_pred(n, pn_Mod_M,         mem);
		set_Tuple_pred(n, pn_Mod_X_regular, new_r_Jmp(blk));
		set_Tuple_pred(n, pn_Mod_X_except,  new_r_Bad(irg));
		set_Tuple_pred(n, pn_Mod_res,       value);
	}
	return n;
}  /* transform_node_Mod */

/**
 * Transform a DivMod node.
 */
static ir_node *transform_node_DivMod(ir_node *n)
{
	const ir_node *dummy;
	ir_node       *a = get_DivMod_left(n);
	ir_node       *b = get_DivMod_right(n);
	ir_mode       *mode = get_DivMod_resmode(n);
	ir_node       *va, *vb;
	ir_graph      *irg = get_irn_irg(n);
	ir_tarval     *ta, *tb;
	int           evaluated = 0;

	if (is_Const(b) && is_const_Phi(a)) {
		/* check for Div(Phi, Const) */
		va = apply_binop_on_phi(a, get_Const_tarval(b), (eval_func) tarval_div, mode, 0);
		vb = apply_binop_on_phi(a, get_Const_tarval(b), (eval_func) tarval_mod, mode, 0);
		if (va && vb) {
			DBG_OPT_ALGSIM0(n, va, FS_OPT_CONST_PHI);
			DBG_OPT_ALGSIM0(n, vb, FS_OPT_CONST_PHI);
			goto make_tuple;
		}
	}
	else if (is_Const(a) && is_const_Phi(b)) {
		/* check for Div(Const, Phi) */
		va = apply_binop_on_phi(b, get_Const_tarval(a), (eval_func) tarval_div, mode, 1);
		vb = apply_binop_on_phi(b, get_Const_tarval(a), (eval_func) tarval_mod, mode, 1);
		if (va && vb) {
			DBG_OPT_ALGSIM0(n, va, FS_OPT_CONST_PHI);
			DBG_OPT_ALGSIM0(n, vb, FS_OPT_CONST_PHI);
			goto make_tuple;
		}
	}
	else if (is_const_Phi(a) && is_const_Phi(b)) {
		/* check for Div(Phi, Phi) */
		va = apply_binop_on_2_phis(a, b, (eval_func) tarval_div, mode);
		vb = apply_binop_on_2_phis(a, b, (eval_func) tarval_mod, mode);
		if (va && vb) {
			DBG_OPT_ALGSIM0(n, va, FS_OPT_CONST_PHI);
			DBG_OPT_ALGSIM0(n, vb, FS_OPT_CONST_PHI);
			goto make_tuple;
		}
	}

	ta = value_of(a);
	tb = value_of(b);
	if (tb != tarval_bad) {
		if (tb == get_mode_one(get_tarval_mode(tb))) {
			va = a;
			vb = new_r_Const(irg, get_mode_null(mode));
			DBG_OPT_CSTEVAL(n, vb);
			goto make_tuple;
		} else if (ta != tarval_bad) {
			ir_tarval *resa, *resb;
			resa = tarval_div(ta, tb);
			if (resa == tarval_bad) return n; /* Causes exception!!! Model by replacing through
			                                     Jmp for X result!? */
			resb = tarval_mod(ta, tb);
			if (resb == tarval_bad) return n; /* Causes exception! */
			va = new_r_Const(irg, resa);
			vb = new_r_Const(irg, resb);
			DBG_OPT_CSTEVAL(n, va);
			DBG_OPT_CSTEVAL(n, vb);
			goto make_tuple;
		} else if (mode_is_signed(mode) && tb == get_mode_minus_one(mode)) {
			va = new_rd_Minus(get_irn_dbg_info(n), get_nodes_block(n), a, mode);
			vb = new_r_Const(irg, get_mode_null(mode));
			DBG_OPT_CSTEVAL(n, va);
			DBG_OPT_CSTEVAL(n, vb);
			goto make_tuple;
		} else { /* Try architecture dependent optimization */
			va = a;
			vb = b;
			arch_dep_replace_divmod_by_const(&va, &vb, n);
			evaluated = va != NULL;
		}
	} else if (a == b) {
		if (value_not_zero(a, &dummy)) {
			/* a/a && a != 0 */
			va = new_r_Const(irg, get_mode_one(mode));
			vb = new_r_Const(irg, get_mode_null(mode));
			DBG_OPT_CSTEVAL(n, va);
			DBG_OPT_CSTEVAL(n, vb);
			goto make_tuple;
		} else {
			/* BEWARE: it is NOT possible to optimize a/a to 1, as this may cause a exception */
			return n;
		}
	} else if (ta == get_mode_null(mode) && value_not_zero(b, &dummy)) {
		/* 0 / non-Const = 0 */
		vb = va = a;
		goto make_tuple;
	}

	if (evaluated) { /* replace by tuple */
		ir_node *mem, *blk;

make_tuple:
		mem = get_DivMod_mem(n);
		/* skip a potential Pin */
		mem = skip_Pin(mem);

		blk = get_nodes_block(n);
		turn_into_tuple(n, pn_DivMod_max);
		set_Tuple_pred(n, pn_DivMod_M,         mem);
		set_Tuple_pred(n, pn_DivMod_X_regular, new_r_Jmp(blk));
		set_Tuple_pred(n, pn_DivMod_X_except,  new_r_Bad(irg)); /*no exception*/
		set_Tuple_pred(n, pn_DivMod_res_div,   va);
		set_Tuple_pred(n, pn_DivMod_res_mod,   vb);
	}

	return n;
}  /* transform_node_DivMod */

/**
 * Optimize x / c to x * (1/c)
 */
static ir_node *transform_node_Quot(ir_node *n)
{
	ir_mode *mode = get_Quot_resmode(n);
	ir_node *oldn = n;

	if (get_mode_arithmetic(mode) == irma_ieee754) {
		ir_node   *b  = get_Quot_right(n);
		ir_tarval *tv = value_of(b);

		if (tv != tarval_bad) {
			int rem = tarval_fp_ops_enabled();

			/*
			 * Floating point constant folding might be disabled here to
			 * prevent rounding.
			 * However, as we check for exact result, doing it is safe.
			 * Switch it on.
			 */
			tarval_enable_fp_ops(1);
			tv = tarval_quo(get_mode_one(mode), tv);
			tarval_enable_fp_ops(rem);

			/* Do the transformation if the result is either exact or we are not
			   using strict rules. */
			if (tv != tarval_bad &&
			    (tarval_ieee754_get_exact() || (get_irg_fp_model(get_irn_irg(n)) & fp_strict_algebraic) == 0)) {
				ir_node *blk = get_nodes_block(n);
				ir_graph *irg = get_irn_irg(blk);
				ir_node *c = new_r_Const(irg, tv);
				ir_node *a = get_Quot_left(n);
				ir_node *m = new_rd_Mul(get_irn_dbg_info(n), blk, a, c, mode);
				ir_node *mem = get_Quot_mem(n);

				/* skip a potential Pin */
				mem = skip_Pin(mem);
				turn_into_tuple(n, pn_Quot_max);
				set_Tuple_pred(n, pn_Quot_M, mem);
				set_Tuple_pred(n, pn_Quot_X_regular, new_r_Jmp(blk));
				set_Tuple_pred(n, pn_Quot_X_except,  new_r_Bad(irg));
				set_Tuple_pred(n, pn_Quot_res, m);
				DBG_OPT_ALGSIM1(oldn, a, b, m, FS_OPT_FP_INV_MUL);
			}
		}
	}
	return n;
}  /* transform_node_Quot */

/**
 * Optimize -a CMP -b into b CMP a.
 * This works only for for modes where unary Minus
 * cannot Overflow.
 * Note that two-complement integers can Overflow
 * so it will NOT work.
 *
 * For == and != can be handled in Proj(Cmp)
 */
static ir_node *transform_node_Cmp(ir_node *n)
{
	ir_node *oldn = n;
	ir_node *left  = get_Cmp_left(n);
	ir_node *right = get_Cmp_right(n);

	if (is_Minus(left) && is_Minus(right) &&
		!mode_overflow_on_unary_Minus(get_irn_mode(left))) {
		ir_node *const new_left  = get_Minus_op(right);
		ir_node *const new_right = get_Minus_op(left);
		n = new_rd_Cmp(get_irn_dbg_info(n), get_nodes_block(n), new_left, new_right);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_CMP_OP_OP);
	}
	return n;
}  /* transform_node_Cmp */


/**
 * Transform a Cond node.
 *
 * Replace the Cond by a Jmp if it branches on a constant
 * condition.
 */
static ir_node *transform_node_Cond(ir_node *n)
{

	ir_node   *a   = get_Cond_selector(n);
	ir_tarval *ta  = value_of(a);
	ir_graph  *irg = get_irn_irg(n);
	ir_node   *jmp;

	/* we need block info which is not available in floating irgs */
	if (get_irg_pinned(irg) == op_pin_state_floats)
		return n;

	if ((ta != tarval_bad) &&
	    (get_irn_mode(a) == mode_b) &&
	    (get_opt_unreachable_code())) {
		/* It's a boolean Cond, branching on a boolean constant.
		   Replace it by a tuple (Bad, Jmp) or (Jmp, Bad) */
		ir_node *blk = get_nodes_block(n);
		jmp = new_r_Jmp(blk);
		turn_into_tuple(n, pn_Cond_max);
		if (ta == tarval_b_true) {
			set_Tuple_pred(n, pn_Cond_false, new_r_Bad(irg));
			set_Tuple_pred(n, pn_Cond_true, jmp);
		} else {
			set_Tuple_pred(n, pn_Cond_false, jmp);
			set_Tuple_pred(n, pn_Cond_true, new_r_Bad(irg));
		}
		/* We might generate an endless loop, so keep it alive. */
		add_End_keepalive(get_irg_end(irg), blk);
	}
	return n;
}  /* transform_node_Cond */

/**
 * Prototype of a recursive transform function
 * for bitwise distributive transformations.
 */
typedef ir_node* (*recursive_transform)(ir_node *n);

/**
 * makes use of distributive laws for and, or, eor
 *     and(a OP c, b OP c) -> and(a, b) OP c
 * note, might return a different op than n
 */
static ir_node *transform_bitwise_distributive(ir_node *n,
                                               recursive_transform trans_func)
{
	ir_node *oldn    = n;
	ir_node *a       = get_binop_left(n);
	ir_node *b       = get_binop_right(n);
	ir_op   *op      = get_irn_op(a);
	ir_op   *op_root = get_irn_op(n);

	if (op != get_irn_op(b))
		return n;

	/* and(conv(a), conv(b)) -> conv(and(a,b)) */
	if (op == op_Conv) {
		ir_node *a_op   = get_Conv_op(a);
		ir_node *b_op   = get_Conv_op(b);
		ir_mode *a_mode = get_irn_mode(a_op);
		ir_mode *b_mode = get_irn_mode(b_op);
		if (a_mode == b_mode && (mode_is_int(a_mode) || a_mode == mode_b)) {
			ir_node *blk = get_nodes_block(n);

			n = exact_copy(n);
			set_binop_left(n, a_op);
			set_binop_right(n, b_op);
			set_irn_mode(n, a_mode);
			n = trans_func(n);
			n = new_r_Conv(blk, n, get_irn_mode(oldn));

			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_CONV);
			return n;
		}
	}

	if (op == op_Eor) {
		/* nothing to gain here */
		return n;
	}

	if (op == op_Shrs || op == op_Shr || op == op_Shl
			|| op == op_And || op == op_Or || op == op_Eor) {
		ir_node *a_left  = get_binop_left(a);
		ir_node *a_right = get_binop_right(a);
		ir_node *b_left  = get_binop_left(b);
		ir_node *b_right = get_binop_right(b);
		ir_node *c       = NULL;
		ir_node *op1     = NULL;
		ir_node *op2     = NULL;

		if (is_op_commutative(op)) {
			if (a_left == b_left) {
				c   = a_left;
				op1 = a_right;
				op2 = b_right;
			} else if (a_left == b_right) {
				c   = a_left;
				op1 = a_right;
				op2 = b_left;
			} else if (a_right == b_left) {
				c   = a_right;
				op1 = a_left;
				op2 = b_right;
			}
		}
		if (a_right == b_right) {
			c   = a_right;
			op1 = a_left;
			op2 = b_left;
		}

		if (c != NULL) {
			/* (a sop c) & (b sop c) => (a & b) sop c */
			ir_node *blk = get_nodes_block(n);

			ir_node *new_n = exact_copy(n);
			set_binop_left(new_n, op1);
			set_binop_right(new_n, op2);
			new_n = trans_func(new_n);

			if (op_root == op_Eor && op == op_Or) {
				dbg_info  *dbgi = get_irn_dbg_info(n);
				ir_mode   *mode = get_irn_mode(c);

				c = new_rd_Not(dbgi, blk, c, mode);
				n = new_rd_And(dbgi, blk, new_n, c, mode);
			} else {
				n = exact_copy(a);
				set_nodes_block(n, blk);
				set_binop_left(n, new_n);
				set_binop_right(n, c);
				add_identities(n);
			}

			DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_SHIFT_AND);
			return n;
		}
	}

	return n;
}

/**
 * Transform an And.
 */
static ir_node *transform_node_And(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_node *a = get_And_left(n);
	ir_node *b = get_And_right(n);
	ir_mode *mode;
	vrp_attr *a_vrp, *b_vrp;

	mode = get_irn_mode(n);
	HANDLE_BINOP_PHI((eval_func) tarval_and, a, b, c, mode);

	/* we can evaluate 2 Projs of the same Cmp */
	if (mode == mode_b && is_Proj(a) && is_Proj(b)) {
		ir_node *pred_a = get_Proj_pred(a);
		ir_node *pred_b = get_Proj_pred(b);
		if (pred_a == pred_b) {
			dbg_info *dbgi  = get_irn_dbg_info(n);
			pn_Cmp pn_a     = get_Proj_proj(a);
			pn_Cmp pn_b     = get_Proj_proj(b);
			/* yes, we can simply calculate with pncs */
			pn_Cmp new_pnc  = pn_a & pn_b;

			return new_rd_Proj(dbgi, pred_a, mode_b, new_pnc);
		}
	}
	if (is_Or(a)) {
		if (is_Not(b)) {
			ir_node *op = get_Not_op(b);
			if (is_And(op)) {
				ir_node *ba = get_And_left(op);
				ir_node *bb = get_And_right(op);

				/* it's enough to test the following cases due to normalization! */
				if (get_Or_left(a) == ba && get_Or_right(a) == bb) {
					/* (a|b) & ~(a&b) = a^b */
					ir_node *block = get_nodes_block(n);

					n = new_rd_Eor(get_irn_dbg_info(n), block, ba, bb, mode);
					DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_TO_EOR);
					return n;
				}
			}
		}
	}
	if (is_Or(b)) {
		if (is_Not(a)) {
			ir_node *op = get_Not_op(a);
			if (is_And(op)) {
				ir_node *aa = get_And_left(op);
				ir_node *ab = get_And_right(op);

				/* it's enough to test the following cases due to normalization! */
				if (get_Or_left(b) == aa && get_Or_right(b) == ab) {
					/* (a|b) & ~(a&b) = a^b */
					ir_node *block = get_nodes_block(n);

					n = new_rd_Eor(get_irn_dbg_info(n), block, aa, ab, mode);
					DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_TO_EOR);
					return n;
				}
			}
		}
	}
	if (is_Eor(a)) {
		ir_node *al = get_Eor_left(a);
		ir_node *ar = get_Eor_right(a);

		if (al == b) {
			/* (b ^ a) & b -> ~a & b */
			dbg_info *dbg  = get_irn_dbg_info(n);
			ir_node *block = get_nodes_block(n);

			ar = new_rd_Not(dbg, block, ar, mode);
			n  = new_rd_And(dbg, block, ar, b, mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_TO_NOT);
			return n;
		}
		if (ar == b) {
			/* (a ^ b) & b -> ~a & b */
			dbg_info *dbg  = get_irn_dbg_info(n);
			ir_node *block = get_nodes_block(n);

			al = new_rd_Not(dbg, block, al, mode);
			n  = new_rd_And(dbg, block, al, b, mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_TO_NOT);
			return n;
		}
	}
	if (is_Eor(b)) {
		ir_node *bl = get_Eor_left(b);
		ir_node *br = get_Eor_right(b);

		if (bl == a) {
			/* a & (a ^ b) -> a & ~b */
			dbg_info *dbg  = get_irn_dbg_info(n);
			ir_node *block = get_nodes_block(n);

			br = new_rd_Not(dbg, block, br, mode);
			n  = new_rd_And(dbg, block, br, a, mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_TO_NOT);
			return n;
		}
		if (br == a) {
			/* a & (b ^ a) -> a & ~b */
			dbg_info *dbg  = get_irn_dbg_info(n);
			ir_node *block = get_nodes_block(n);

			bl = new_rd_Not(dbg, block, bl, mode);
			n  = new_rd_And(dbg, block, bl, a, mode);
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_TO_NOT);
			return n;
		}
	}
	if (is_Not(a) && is_Not(b)) {
		/* ~a & ~b = ~(a|b) */
		ir_node *block = get_nodes_block(n);
		ir_mode *mode = get_irn_mode(n);

		a = get_Not_op(a);
		b = get_Not_op(b);
		n = new_rd_Or(get_irn_dbg_info(n), block, a, b, mode);
		n = new_rd_Not(get_irn_dbg_info(n), block, n, mode);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_DEMORGAN);
		return n;
	}

	b_vrp = vrp_get_info(b);
	if (is_Const(a) && b_vrp && (tarval_cmp(tarval_or(get_Const_tarval(a),
						b_vrp->bits_not_set), get_Const_tarval(a)) == pn_Cmp_Eq)) {

		return b;

	}

	a_vrp = vrp_get_info(a);
	if (is_Const(b) && a_vrp && (tarval_cmp(tarval_or(get_Const_tarval(b),
						a_vrp->bits_not_set), get_Const_tarval(b)) == pn_Cmp_Eq)) {
		return a;
	}

	n = transform_bitwise_distributive(n, transform_node_And);

	return n;
}  /* transform_node_And */

/* the order of the values is important! */
typedef enum const_class {
	const_const = 0,
	const_like  = 1,
	const_other = 2
} const_class;

static const_class classify_const(const ir_node* n)
{
	if (is_Const(n))         return const_const;
	if (is_irn_constlike(n)) return const_like;
	return const_other;
}

/**
 * Determines whether r is more constlike or has a larger index (in that order)
 * than l.
 */
static bool operands_are_normalized(const ir_node *l, const ir_node *r)
{
	const const_class l_order = classify_const(l);
	const const_class r_order = classify_const(r);
	return
		l_order > r_order ||
		(l_order == r_order && get_irn_idx(l) <= get_irn_idx(r));
}

/**
 * Transform an Eor.
 */
static ir_node *transform_node_Eor(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_node *a = get_Eor_left(n);
	ir_node *b = get_Eor_right(n);
	ir_mode *mode = get_irn_mode(n);

	HANDLE_BINOP_PHI((eval_func) tarval_eor, a, b, c, mode);

	/* we can evaluate 2 Projs of the same Cmp */
	if (mode == mode_b && is_Proj(a) && is_Proj(b)) {
		ir_node *pred_a = get_Proj_pred(a);
		ir_node *pred_b = get_Proj_pred(b);
		if (pred_a == pred_b) {
			dbg_info *dbgi  = get_irn_dbg_info(n);
			pn_Cmp pn_a     = get_Proj_proj(a);
			pn_Cmp pn_b     = get_Proj_proj(b);
			/* yes, we can simply calculate with pncs */
			pn_Cmp new_pnc  = pn_a ^ pn_b;

			return new_rd_Proj(dbgi, pred_a, mode_b, new_pnc);
		}
	}

	/* normalize not nodes... ~a ^ b <=> a ^ ~b */
	if (is_Not(a) && operands_are_normalized(get_Not_op(a), b)) {
		dbg_info *dbg      = get_irn_dbg_info(n);
		ir_node  *block    = get_nodes_block(n);
		ir_node  *new_not  = new_rd_Not(dbg, block, b, mode);
		ir_node  *new_left = get_Not_op(a);
		n = new_rd_Eor(dbg, block, new_left, new_not, mode);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_TO_NOT);
		return n;
	} else if (is_Not(b) && !operands_are_normalized(a, get_Not_op(b))) {
		dbg_info *dbg       = get_irn_dbg_info(n);
		ir_node  *block     = get_nodes_block(n);
		ir_node  *new_not   = new_rd_Not(dbg, block, a, mode);
		ir_node  *new_right = get_Not_op(b);
		n = new_rd_Eor(dbg, block, new_not, new_right, mode);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_TO_NOT);
		return n;
	}

	/* x ^ 1...1 -> ~1 */
	if (is_Const(b) && is_Const_all_one(b)) {
		n = new_r_Not(get_nodes_block(n), a, mode);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_TO_NOT);
		return n;
	}

	n = transform_bitwise_distributive(n, transform_node_Eor);
	return n;
}  /* transform_node_Eor */

/**
 * Transform a Not.
 */
static ir_node *transform_node_Not(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_node *a    = get_Not_op(n);
	ir_mode *mode = get_irn_mode(n);

	HANDLE_UNOP_PHI(tarval_not,a,c);

	/* check for a boolean Not */
	if (mode == mode_b && is_Proj(a)) {
		ir_node *a_pred = get_Proj_pred(a);
		if (is_Cmp(a_pred)) {
			/* We negate a Cmp. The Cmp has the negated result anyways! */
			n = new_r_Proj(get_Proj_pred(a),
			               mode_b, get_negated_pnc(get_Proj_proj(a), mode_b));
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_NOT_CMP);
			return n;
		}
	}

	/* normalize ~(a ^ b) => a ^ ~b */
	if (is_Eor(a)) {
		dbg_info *dbg       = get_irn_dbg_info(n);
		ir_node  *block     = get_nodes_block(n);
		ir_node  *eor_right = get_Eor_right(a);
		ir_node  *eor_left  = get_Eor_left(a);
		eor_right = new_rd_Not(dbg, block, eor_right, mode);
		n = new_rd_Eor(dbg, block, eor_left, eor_right, mode);
		return n;
	}

	if (get_mode_arithmetic(mode) == irma_twos_complement) {
		if (is_Minus(a)) { /* ~-x -> x + -1 */
			dbg_info *dbg   = get_irn_dbg_info(n);
			ir_graph *irg   = get_irn_irg(n);
			ir_node  *block = get_nodes_block(n);
			ir_node  *add_l = get_Minus_op(a);
			ir_node  *add_r = new_rd_Const(dbg, irg, get_mode_minus_one(mode));
			n = new_rd_Add(dbg, block, add_l, add_r, mode);
		} else if (is_Add(a)) {
			ir_node *add_r = get_Add_right(a);
			if (is_Const(add_r) && is_Const_all_one(add_r)) {
				/* ~(x + -1) = -x */
				ir_node *op = get_Add_left(a);
				ir_node *blk = get_nodes_block(n);
				n = new_rd_Minus(get_irn_dbg_info(n), blk, op, get_irn_mode(n));
				DBG_OPT_ALGSIM0(oldn, n, FS_OPT_NOT_MINUS_1);
			}
		}
	}
	return n;
}  /* transform_node_Not */

/**
 * Transform a Minus.
 * Optimize:
 *   -(~x) = x + 1
 *   -(a-b) = b - a
 *   -(a >>u (size-1)) = a >>s (size-1)
 *   -(a >>s (size-1)) = a >>u (size-1)
 *   -(a * const) -> a * -const
 */
static ir_node *transform_node_Minus(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_node *a = get_Minus_op(n);
	ir_mode *mode;

	HANDLE_UNOP_PHI(tarval_neg,a,c);

	mode = get_irn_mode(a);
	if (get_mode_arithmetic(mode) == irma_twos_complement) {
		/* the following rules are only to twos-complement */
		if (is_Not(a)) {
			/* -(~x) = x + 1 */
			ir_node   *op  = get_Not_op(a);
			ir_tarval *tv  = get_mode_one(mode);
			ir_node   *blk = get_nodes_block(n);
			ir_graph  *irg = get_irn_irg(blk);
			ir_node   *c   = new_r_Const(irg, tv);
			n = new_rd_Add(get_irn_dbg_info(n), blk, op, c, mode);
			DBG_OPT_ALGSIM2(oldn, a, n, FS_OPT_MINUS_NOT);
			return n;
		}
		if (is_Shr(a)) {
			ir_node *c = get_Shr_right(a);

			if (is_Const(c)) {
				ir_tarval *tv = get_Const_tarval(c);

				if (tarval_is_long(tv) && get_tarval_long(tv) == (int) get_mode_size_bits(mode) - 1) {
					/* -(a >>u (size-1)) = a >>s (size-1) */
					ir_node *v = get_Shr_left(a);

					n = new_rd_Shrs(get_irn_dbg_info(n), get_nodes_block(n), v, c, mode);
					DBG_OPT_ALGSIM2(oldn, a, n, FS_OPT_PREDICATE);
					return n;
				}
			}
		}
		if (is_Shrs(a)) {
			ir_node *c = get_Shrs_right(a);

			if (is_Const(c)) {
				ir_tarval *tv = get_Const_tarval(c);

				if (tarval_is_long(tv) && get_tarval_long(tv) == (int) get_mode_size_bits(mode) - 1) {
					/* -(a >>s (size-1)) = a >>u (size-1) */
					ir_node *v = get_Shrs_left(a);

					n = new_rd_Shr(get_irn_dbg_info(n), get_nodes_block(n), v, c, mode);
					DBG_OPT_ALGSIM2(oldn, a, n, FS_OPT_PREDICATE);
					return n;
				}
			}
		}
	}
	if (is_Sub(a)) {
		/* - (a-b) = b - a */
		ir_node *la  = get_Sub_left(a);
		ir_node *ra  = get_Sub_right(a);
		ir_node *blk = get_nodes_block(n);

		n = new_rd_Sub(get_irn_dbg_info(n), blk, ra, la, mode);
		DBG_OPT_ALGSIM2(oldn, a, n, FS_OPT_MINUS_SUB);
		return n;
	}

	if (is_Mul(a)) { /* -(a * const) -> a * -const */
		ir_node   *mul_l = get_Mul_left(a);
		ir_node   *mul_r = get_Mul_right(a);
		ir_tarval *tv    = value_of(mul_r);
		if (tv != tarval_bad) {
			tv = tarval_neg(tv);
			if (tv != tarval_bad) {
				ir_graph *irg   = get_irn_irg(n);
				ir_node  *cnst  = new_r_Const(irg, tv);
				dbg_info *dbg   = get_irn_dbg_info(a);
				ir_node  *block = get_nodes_block(a);
				n = new_rd_Mul(dbg, block, mul_l, cnst, mode);
				DBG_OPT_ALGSIM2(oldn, a, n, FS_OPT_MINUS_MUL_C);
				return n;
			}
		}
	}

	return n;
}  /* transform_node_Minus */

/**
 * Transform a Proj(Load) with a non-null address.
 */
static ir_node *transform_node_Proj_Load(ir_node *proj)
{
	if (get_opt_ldst_only_null_ptr_exceptions()) {
		if (get_irn_mode(proj) == mode_X) {
			ir_node *load = get_Proj_pred(proj);

			/* get the Load address */
			const ir_node *addr = get_Load_ptr(load);
			const ir_node *confirm;

			if (value_not_null(addr, &confirm)) {
				if (confirm == NULL) {
					/* this node may float if it did not depend on a Confirm */
					set_irn_pinned(load, op_pin_state_floats);
				}
				if (get_Proj_proj(proj) == pn_Load_X_except) {
					ir_graph *irg = get_irn_irg(proj);
					DBG_OPT_EXC_REM(proj);
					return get_irg_bad(irg);
				} else {
					ir_node *blk = get_nodes_block(load);
					return new_r_Jmp(blk);
				}
			}
		}
	}
	return proj;
}  /* transform_node_Proj_Load */

/**
 * Transform a Proj(Store) with a non-null address.
 */
static ir_node *transform_node_Proj_Store(ir_node *proj)
{
	if (get_opt_ldst_only_null_ptr_exceptions()) {
		if (get_irn_mode(proj) == mode_X) {
			ir_node *store = get_Proj_pred(proj);

			/* get the load/store address */
			const ir_node *addr = get_Store_ptr(store);
			const ir_node *confirm;

			if (value_not_null(addr, &confirm)) {
				if (confirm == NULL) {
					/* this node may float if it did not depend on a Confirm */
					set_irn_pinned(store, op_pin_state_floats);
				}
				if (get_Proj_proj(proj) == pn_Store_X_except) {
					ir_graph *irg = get_irn_irg(proj);
					DBG_OPT_EXC_REM(proj);
					return get_irg_bad(irg);
				} else {
					ir_node *blk = get_nodes_block(store);
					return new_r_Jmp(blk);
				}
			}
		}
	}
	return proj;
}  /* transform_node_Proj_Store */

/**
 * Transform a Proj(Div) with a non-zero value.
 * Removes the exceptions and routes the memory to the NoMem node.
 */
static ir_node *transform_node_Proj_Div(ir_node *proj)
{
	ir_node *div = get_Proj_pred(proj);
	ir_node *b   = get_Div_right(div);
	ir_node *res, *new_mem;
	const ir_node *confirm;
	long proj_nr;

	if (value_not_zero(b, &confirm)) {
		/* div(x, y) && y != 0 */
		if (confirm == NULL) {
			/* we are sure we have a Const != 0 */
			new_mem = get_Div_mem(div);
			new_mem = skip_Pin(new_mem);
			set_Div_mem(div, new_mem);
			set_irn_pinned(div, op_pin_state_floats);
		}

		proj_nr = get_Proj_proj(proj);
		switch (proj_nr) {
		case pn_Div_X_regular:
			return new_r_Jmp(get_nodes_block(div));

		case pn_Div_X_except: {
			ir_graph *irg = get_irn_irg(proj);
			/* we found an exception handler, remove it */
			DBG_OPT_EXC_REM(proj);
			return new_r_Bad(irg);
		}

		case pn_Div_M: {
			ir_graph *irg = get_irn_irg(proj);
			res = get_Div_mem(div);
			new_mem = get_irg_no_mem(irg);

			if (confirm) {
				/* This node can only float up to the Confirm block */
				new_mem = new_r_Pin(get_nodes_block(confirm), new_mem);
			}
			set_irn_pinned(div, op_pin_state_floats);
			/* this is a Div without exception, we can remove the memory edge */
			set_Div_mem(div, new_mem);
			return res;
		}
		}
	}
	return proj;
}  /* transform_node_Proj_Div */

/**
 * Transform a Proj(Mod) with a non-zero value.
 * Removes the exceptions and routes the memory to the NoMem node.
 */
static ir_node *transform_node_Proj_Mod(ir_node *proj)
{
	ir_node *mod = get_Proj_pred(proj);
	ir_node *b   = get_Mod_right(mod);
	ir_node *res, *new_mem;
	const ir_node *confirm;
	long proj_nr;

	if (value_not_zero(b, &confirm)) {
		/* mod(x, y) && y != 0 */
		proj_nr = get_Proj_proj(proj);

		if (confirm == NULL) {
			/* we are sure we have a Const != 0 */
			new_mem = get_Mod_mem(mod);
			new_mem = skip_Pin(new_mem);
			set_Mod_mem(mod, new_mem);
			set_irn_pinned(mod, op_pin_state_floats);
		}

		switch (proj_nr) {

		case pn_Mod_X_regular:
			return new_r_Jmp(get_irn_n(mod, -1));

		case pn_Mod_X_except: {
			ir_graph *irg = get_irn_irg(proj);
			/* we found an exception handler, remove it */
			DBG_OPT_EXC_REM(proj);
			return new_r_Bad(irg);
		}

		case pn_Mod_M: {
			ir_graph *irg = get_irn_irg(proj);
			res = get_Mod_mem(mod);
			new_mem = get_irg_no_mem(irg);

			if (confirm) {
				/* This node can only float up to the Confirm block */
				new_mem = new_r_Pin(get_nodes_block(confirm), new_mem);
			}
			/* this is a Mod without exception, we can remove the memory edge */
			set_Mod_mem(mod, new_mem);
			return res;
		}
		case pn_Mod_res:
			if (get_Mod_left(mod) == b) {
				/* a % a = 0 if a != 0 */
				ir_graph *irg  = get_irn_irg(proj);
				ir_mode  *mode = get_irn_mode(proj);
				ir_node  *res  = new_r_Const(irg, get_mode_null(mode));

				DBG_OPT_CSTEVAL(mod, res);
				return res;
			}
		}
	}
	return proj;
}  /* transform_node_Proj_Mod */

/**
 * Transform a Proj(DivMod) with a non-zero value.
 * Removes the exceptions and routes the memory to the NoMem node.
 */
static ir_node *transform_node_Proj_DivMod(ir_node *proj)
{
	ir_node *divmod = get_Proj_pred(proj);
	ir_node *b      = get_DivMod_right(divmod);
	ir_node *res, *new_mem;
	const ir_node *confirm;
	long proj_nr;

	if (value_not_zero(b, &confirm)) {
		/* DivMod(x, y) && y != 0 */
		proj_nr = get_Proj_proj(proj);

		if (confirm == NULL) {
			/* we are sure we have a Const != 0 */
			new_mem = get_DivMod_mem(divmod);
			new_mem = skip_Pin(new_mem);
			set_DivMod_mem(divmod, new_mem);
			set_irn_pinned(divmod, op_pin_state_floats);
		}

		switch (proj_nr) {

		case pn_DivMod_X_regular:
			return new_r_Jmp(get_nodes_block(divmod));

		case pn_DivMod_X_except: {
			/* we found an exception handler, remove it */
			ir_graph *irg = get_irn_irg(proj);
			DBG_OPT_EXC_REM(proj);
			return new_r_Bad(irg);
		}

		case pn_DivMod_M: {
			ir_graph *irg = get_irn_irg(proj);
			res = get_DivMod_mem(divmod);
			new_mem = get_irg_no_mem(irg);

			if (confirm) {
				/* This node can only float up to the Confirm block */
				new_mem = new_r_Pin(get_nodes_block(confirm), new_mem);
			}
			/* this is a DivMod without exception, we can remove the memory edge */
			set_DivMod_mem(divmod, new_mem);
			return res;
		}

		case pn_DivMod_res_mod:
			if (get_DivMod_left(divmod) == b) {
				/* a % a = 0 if a != 0 */
				ir_graph *irg  = get_irn_irg(proj);
				ir_mode  *mode = get_irn_mode(proj);
				ir_node  *res  = new_r_Const(irg, get_mode_null(mode));

				DBG_OPT_CSTEVAL(divmod, res);
				return res;
			}
		}
	}
	return proj;
}  /* transform_node_Proj_DivMod */

/**
 * Optimizes jump tables (CondIs or CondIu) by removing all impossible cases.
 */
static ir_node *transform_node_Proj_Cond(ir_node *proj)
{
	if (get_opt_unreachable_code()) {
		ir_node *n = get_Proj_pred(proj);
		ir_node *b = get_Cond_selector(n);

		if (mode_is_int(get_irn_mode(b))) {
			ir_tarval *tb = value_of(b);

			if (tb != tarval_bad) {
				/* we have a constant switch */
				long num = get_Proj_proj(proj);

				if (num != get_Cond_default_proj(n)) { /* we cannot optimize default Proj's yet */
					if (get_tarval_long(tb) == num) {
						/* Do NOT create a jump here, or we will have 2 control flow ops
						 * in a block. This case is optimized away in optimize_cf(). */
						return proj;
					} else {
						ir_graph *irg = get_irn_irg(proj);
						/* this case will NEVER be taken, kill it */
						return get_irg_bad(irg);
					}
				}
			} else {
				long num = get_Proj_proj(proj);
				vrp_attr *b_vrp = vrp_get_info(b);
				if (num != get_Cond_default_proj(n) && b_vrp) {
					/* Try handling with vrp data. We only remove dead parts. */
					ir_tarval *tp = new_tarval_from_long(num, get_irn_mode(b));

					if (b_vrp->range_type == VRP_RANGE) {
						pn_Cmp cmp_result = tarval_cmp(b_vrp->range_bottom, tp);
						pn_Cmp cmp_result2 = tarval_cmp(b_vrp->range_top, tp);

						if ((cmp_result & pn_Cmp_Gt) == cmp_result && (cmp_result2
									& pn_Cmp_Lt) == cmp_result2) {
							ir_graph *irg = get_irn_irg(proj);
							return get_irg_bad(irg);
						}
					} else if (b_vrp->range_type == VRP_ANTIRANGE) {
						pn_Cmp cmp_result = tarval_cmp(b_vrp->range_bottom, tp);
						pn_Cmp cmp_result2 = tarval_cmp(b_vrp->range_top, tp);

						if ((cmp_result & pn_Cmp_Le) == cmp_result && (cmp_result2
									& pn_Cmp_Ge) == cmp_result2) {
							ir_graph *irg = get_irn_irg(proj);
							return get_irg_bad(irg);
						}
					}

					if (!(tarval_cmp(
									tarval_and( b_vrp->bits_set, tp),
									b_vrp->bits_set
									) == pn_Cmp_Eq)) {
						ir_graph *irg = get_irn_irg(proj);
						return get_irg_bad(irg);
					}

					if (!(tarval_cmp(
									tarval_and(
										tarval_not(tp),
										tarval_not(b_vrp->bits_not_set)),
									tarval_not(b_vrp->bits_not_set))
									 == pn_Cmp_Eq)) {
						ir_graph *irg = get_irn_irg(proj);
						return get_irg_bad(irg);
					}


				}
			}
		}
	}
	return proj;
}  /* transform_node_Proj_Cond */

/**
 * Create a 0 constant of given mode.
 */
static ir_node *create_zero_const(ir_graph *irg, ir_mode *mode)
{
	ir_tarval *tv   = get_mode_null(mode);
	ir_node   *cnst = new_r_Const(irg, tv);

	return cnst;
}

/**
 * Normalizes and optimizes Cmp nodes.
 */
static ir_node *transform_node_Proj_Cmp(ir_node *proj)
{
	ir_node     *n       = get_Proj_pred(proj);
	ir_node     *left    = get_Cmp_left(n);
	ir_node     *right   = get_Cmp_right(n);
	ir_tarval   *tv      = NULL;
	int          changed = 0;
	ir_mode     *mode    = NULL;
	long         proj_nr = get_Proj_proj(proj);

	/* we can evaluate some cases directly */
	switch (proj_nr) {
	case pn_Cmp_False: {
		ir_graph *irg = get_irn_irg(proj);
		return new_r_Const(irg, get_tarval_b_false());
	}
	case pn_Cmp_True: {
		ir_graph *irg = get_irn_irg(proj);
		return new_r_Const(irg, get_tarval_b_true());
	}
	case pn_Cmp_Leg:
		if (!mode_is_float(get_irn_mode(left))) {
			ir_graph *irg = get_irn_irg(proj);
			return new_r_Const(irg, get_tarval_b_true());
		}
		break;
	default:
		break;
	}

	/* remove Casts of both sides */
	left  = skip_Cast(left);
	right = skip_Cast(right);

	/* Remove unnecessary conversions */
	/* TODO handle constants */
	if (is_Conv(left) && is_Conv(right)) {
		ir_mode *mode        = get_irn_mode(left);
		ir_node *op_left     = get_Conv_op(left);
		ir_node *op_right    = get_Conv_op(right);
		ir_mode *mode_left   = get_irn_mode(op_left);
		ir_mode *mode_right  = get_irn_mode(op_right);

		if (smaller_mode(mode_left, mode) && smaller_mode(mode_right, mode)
				&& mode_left != mode_b && mode_right != mode_b) {
			ir_node  *block = get_nodes_block(n);

			if (mode_left == mode_right) {
				left  = op_left;
				right = op_right;
				changed |= 1;
				DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_CONV_CONV);
			} else if (smaller_mode(mode_left, mode_right)) {
				left  = new_r_Conv(block, op_left, mode_right);
				right = op_right;
				changed |= 1;
				DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_CONV);
			} else if (smaller_mode(mode_right, mode_left)) {
				left  = op_left;
				right = new_r_Conv(block, op_right, mode_left);
				changed |= 1;
				DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_CONV);
			}
		}
	}

	/* remove operation on both sides if possible */
	if (proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg) {
		/*
		 * The following operations are NOT safe for floating point operations, for instance
		 * 1.0 + inf == 2.0 + inf, =/=> x == y
		 */
		if (mode_is_int(get_irn_mode(left))) {
			unsigned lop = get_irn_opcode(left);

			if (lop == get_irn_opcode(right)) {
				ir_node *ll, *lr, *rl, *rr;

				/* same operation on both sides, try to remove */
				switch (lop) {
				case iro_Not:
				case iro_Minus:
					/* ~a CMP ~b => a CMP b, -a CMP -b ==> a CMP b */
					left  = get_unop_op(left);
					right = get_unop_op(right);
					changed |= 1;
					DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
					break;
				case iro_Add:
					ll = get_Add_left(left);
					lr = get_Add_right(left);
					rl = get_Add_left(right);
					rr = get_Add_right(right);

					if (ll == rl) {
						/* X + a CMP X + b ==> a CMP b */
						left  = lr;
						right = rr;
						changed |= 1;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
					} else if (ll == rr) {
						/* X + a CMP b + X ==> a CMP b */
						left  = lr;
						right = rl;
						changed |= 1;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
					} else if (lr == rl) {
						/* a + X CMP X + b ==> a CMP b */
						left  = ll;
						right = rr;
						changed |= 1;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
					} else if (lr == rr) {
						/* a + X CMP b + X ==> a CMP b */
						left  = ll;
						right = rl;
						changed |= 1;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
					}
					break;
				case iro_Sub:
					ll = get_Sub_left(left);
					lr = get_Sub_right(left);
					rl = get_Sub_left(right);
					rr = get_Sub_right(right);

					if (ll == rl) {
						/* X - a CMP X - b ==> a CMP b */
						left  = lr;
						right = rr;
						changed |= 1;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
					} else if (lr == rr) {
						/* a - X CMP b - X ==> a CMP b */
						left  = ll;
						right = rl;
						changed |= 1;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
					}
					break;
				case iro_Rotl:
					if (get_Rotl_right(left) == get_Rotl_right(right)) {
						/* a ROTL X CMP b ROTL X ==> a CMP b */
						left  = get_Rotl_left(left);
						right = get_Rotl_left(right);
						changed |= 1;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
					}
					break;
				default:
					break;
				}
			}

			/* X+A == A, A+X == A, A-X == A -> X == 0 */
			if (is_Add(left) || is_Sub(left)) {
				ir_node *ll = get_binop_left(left);
				ir_node *lr = get_binop_right(left);

				if (lr == right && is_Add(left)) {
					ir_node *tmp = ll;
					ll = lr;
					lr = tmp;
				}
				if (ll == right) {
					ir_graph *irg = get_irn_irg(n);
					left     = lr;
					right    = create_zero_const(irg, get_irn_mode(left));
					changed |= 1;
					DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
				}
			}
			if (is_Add(right) || is_Sub(right)) {
				ir_node *rl = get_binop_left(right);
				ir_node *rr = get_binop_right(right);

				if (rr == left && is_Add(right)) {
					ir_node *tmp = rl;
					rl = rr;
					rr = tmp;
				}
				if (rl == left) {
					ir_graph *irg = get_irn_irg(n);
					left     = rr;
					right    = create_zero_const(irg, get_irn_mode(left));
					changed |= 1;
					DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_OP);
				}
			}
			if (is_And(left) && is_Const(right)) {
				ir_node *ll = get_binop_left(left);
				ir_node *lr = get_binop_right(left);
				if (is_Shr(ll) && is_Const(lr)) {
					/* Cmp((x >>u c1) & c2, c3) = Cmp(x & (c2 << c1), c3 << c1) */
					ir_node *block = get_nodes_block(n);
					ir_mode *mode = get_irn_mode(left);

					ir_node *llr = get_Shr_right(ll);
					if (is_Const(llr)) {
						dbg_info *dbg = get_irn_dbg_info(left);
						ir_graph *irg = get_irn_irg(left);

						ir_tarval *c1    = get_Const_tarval(llr);
						ir_tarval *c2    = get_Const_tarval(lr);
						ir_tarval *c3    = get_Const_tarval(right);
						ir_tarval *mask  = tarval_shl(c2, c1);
						ir_tarval *value = tarval_shl(c3, c1);

						left  = new_rd_And(dbg, block, get_Shr_left(ll), new_r_Const(irg, mask), mode);
						right = new_r_Const(irg, value);
						changed |= 1;
					}
				}
			}
			/* Cmp(Eor(x, y), 0) <=> Cmp(x, y) at least for the ==0,!=0
			 * cases */
			if (is_Const(right) && is_Const_null(right) && is_Eor(left)) {
				right = get_Eor_right(left);
				left  = get_Eor_left(left);
				changed |= 1;
			}
		}  /* mode_is_int(...) */
	}  /* proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg */

	/* replace mode_b compares with ands/ors */
	if (get_irn_mode(left) == mode_b) {
		ir_node  *block = get_nodes_block(n);
		ir_node  *bres;

		switch (proj_nr) {
			case pn_Cmp_Le: bres = new_r_Or( block, new_r_Not(block, left, mode_b), right, mode_b); break;
			case pn_Cmp_Lt: bres = new_r_And(block, new_r_Not(block, left, mode_b), right, mode_b); break;
			case pn_Cmp_Ge: bres = new_r_Or( block, left, new_r_Not(block, right, mode_b), mode_b); break;
			case pn_Cmp_Gt: bres = new_r_And(block, left, new_r_Not(block, right, mode_b), mode_b); break;
			case pn_Cmp_Lg: bres = new_r_Eor(block, left, right, mode_b); break;
			case pn_Cmp_Eq: bres = new_r_Not(block, new_r_Eor(block, left, right, mode_b), mode_b); break;
			default: bres = NULL;
		}
		if (bres) {
			DBG_OPT_ALGSIM0(n, bres, FS_OPT_CMP_TO_BOOL);
			return bres;
		}
	}

	/*
	 * First step: normalize the compare op
	 * by placing the constant on the right side
	 * or moving the lower address node to the left.
	 */
	if (!operands_are_normalized(left, right)) {
		ir_node *t = left;

		left  = right;
		right = t;

		proj_nr = get_inversed_pnc(proj_nr);
		changed |= 1;
	}

	/*
	 * Second step: Try to reduce the magnitude
	 * of a constant. This may help to generate better code
	 * later and may help to normalize more compares.
	 * Of course this is only possible for integer values.
	 */
	tv = value_of(right);
	if (tv != tarval_bad) {
		mode = get_irn_mode(right);

		/* TODO extend to arbitrary constants */
		if (is_Conv(left) && tarval_is_null(tv)) {
			ir_node *op      = get_Conv_op(left);
			ir_mode *op_mode = get_irn_mode(op);

			/*
			 * UpConv(x) REL 0  ==> x REL 0
			 * Don't do this for float values as it's unclear whether it is a
			 * win. (on the other side it makes detection/creation of fabs hard)
			 */
			if (get_mode_size_bits(mode) > get_mode_size_bits(op_mode) &&
			    ((proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg) ||
				 mode_is_signed(mode) || !mode_is_signed(op_mode)) &&
				!mode_is_float(mode)) {
				tv   = get_mode_null(op_mode);
				left = op;
				mode = op_mode;
				changed |= 2;
				DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_CONV);
			}
		}

		if (tv != tarval_bad) {
			/* the following optimization is possible on modes without Overflow
			 * on Unary Minus or on == and !=:
			 * -a CMP c  ==>  a swap(CMP) -c
			 *
			 * Beware: for two-complement Overflow may occur, so only == and != can
			 * be optimized, see this:
			 * -MININT < 0 =/=> MININT > 0 !!!
			 */
			if (is_Minus(left) &&
				(!mode_overflow_on_unary_Minus(mode) ||
				(mode_is_int(mode) && (proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg)))) {
				tv = tarval_neg(tv);

				if (tv != tarval_bad) {
					left = get_Minus_op(left);
					proj_nr = get_inversed_pnc(proj_nr);
					changed |= 2;
					DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_C);
				}
			} else if (is_Not(left) && (proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg)) {
				/* Not(a) ==/!= c  ==>  a ==/!= Not(c) */
				tv = tarval_not(tv);

				if (tv != tarval_bad) {
					left = get_Not_op(left);
					changed |= 2;
					DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_C);
				}
			}

			/* for integer modes, we have more */
			if (mode_is_int(mode)) {
				/* Ne includes Unordered which is not possible on integers.
				 * However, frontends often use this wrong, so fix it here */
				if (proj_nr & pn_Cmp_Uo) {
					proj_nr &= ~pn_Cmp_Uo;
					set_Proj_proj(proj, proj_nr);
				}

				/* c > 0 : a < c  ==>  a <= (c-1)    a >= c  ==>  a > (c-1) */
				if ((proj_nr == pn_Cmp_Lt || proj_nr == pn_Cmp_Ge) &&
					tarval_cmp(tv, get_mode_null(mode)) == pn_Cmp_Gt) {
					tv = tarval_sub(tv, get_mode_one(mode), NULL);

					if (tv != tarval_bad) {
						proj_nr ^= pn_Cmp_Eq;
						changed |= 2;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_CNST_MAGN);
					}
				}
				/* c < 0 : a > c  ==>  a >= (c+1)    a <= c  ==>  a < (c+1) */
				else if ((proj_nr == pn_Cmp_Gt || proj_nr == pn_Cmp_Le) &&
					tarval_cmp(tv, get_mode_null(mode)) == pn_Cmp_Lt) {
					tv = tarval_add(tv, get_mode_one(mode));

					if (tv != tarval_bad) {
						proj_nr ^= pn_Cmp_Eq;
						changed |= 2;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_CNST_MAGN);
					}
				}

				/* the following reassociations work only for == and != */
				if (proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg) {

#if 0 /* Might be not that good in general */
					/* a-b == 0  ==>  a == b,  a-b != 0  ==>  a != b */
					if (tarval_is_null(tv) && is_Sub(left)) {
						right = get_Sub_right(left);
						left  = get_Sub_left(left);

						tv = value_of(right);
						changed = 1;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_C);
					}
#endif

					if (tv != tarval_bad) {
						/* a-c1 == c2  ==>  a == c2+c1,  a-c1 != c2  ==>  a != c2+c1 */
						if (is_Sub(left)) {
							ir_node *c1 = get_Sub_right(left);
							ir_tarval *tv2 = value_of(c1);

							if (tv2 != tarval_bad) {
								tv2 = tarval_add(tv, value_of(c1));

								if (tv2 != tarval_bad) {
									left    = get_Sub_left(left);
									tv      = tv2;
									changed |= 2;
									DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_C);
								}
							}
						}
						/* a+c1 == c2  ==>  a == c2-c1,  a+c1 != c2  ==>  a != c2-c1 */
						else if (is_Add(left)) {
							ir_node *a_l = get_Add_left(left);
							ir_node *a_r = get_Add_right(left);
							ir_node *a;
							ir_tarval *tv2;

							if (is_Const(a_l)) {
								a = a_r;
								tv2 = value_of(a_l);
							} else {
								a = a_l;
								tv2 = value_of(a_r);
							}

							if (tv2 != tarval_bad) {
								tv2 = tarval_sub(tv, tv2, NULL);

								if (tv2 != tarval_bad) {
									left    = a;
									tv      = tv2;
									changed |= 2;
									DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_C);
								}
							}
						}
						/* -a == c ==> a == -c, -a != c ==> a != -c */
						else if (is_Minus(left)) {
							ir_tarval *tv2 = tarval_sub(get_mode_null(mode), tv, NULL);

							if (tv2 != tarval_bad) {
								left    = get_Minus_op(left);
								tv      = tv2;
								changed |= 2;
								DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_OP_C);
							}
						}
					}
				} /* == or != */
			} /* mode_is_int */

			if (proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg) {
				switch (get_irn_opcode(left)) {
					ir_node *c1;

				case iro_And:
					c1 = get_And_right(left);
					if (is_Const(c1)) {
						/*
						 * And(x, C1) == C2 ==> FALSE if C2 & C1 != C2
						 * And(x, C1) != C2 ==> TRUE if C2 & C1 != C2
						 */
						ir_tarval *mask = tarval_and(get_Const_tarval(c1), tv);
						if (mask != tv) {
							/* TODO: move to constant evaluation */
							ir_graph *irg = get_irn_irg(n);
							tv = proj_nr == pn_Cmp_Eq ? get_tarval_b_false() : get_tarval_b_true();
							c1 = new_r_Const(irg, tv);
							DBG_OPT_CSTEVAL(proj, c1);
							return c1;
						}

						if (tarval_is_single_bit(tv)) {
							/*
							 * optimization for AND:
							 * Optimize:
							 *   And(x, C) == C  ==>  And(x, C) != 0
							 *   And(x, C) != C  ==>  And(X, C) == 0
							 *
							 * if C is a single Bit constant.
							 */

							/* check for Constant's match. We have check hare the tarvals,
							   because our const might be changed */
							if (get_Const_tarval(c1) == tv) {
								/* fine: do the transformation */
								tv = get_mode_null(get_tarval_mode(tv));
								proj_nr ^= pn_Cmp_Leg;
								changed |= 2;
								DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_CNST_MAGN);
							}
						}
					}
					break;
				case iro_Or:
					c1 = get_Or_right(left);
					if (is_Const(c1) && tarval_is_null(tv)) {
						/*
						 * Or(x, C) == 0  && C != 0 ==> FALSE
						 * Or(x, C) != 0  && C != 0 ==> TRUE
						 */
						if (! tarval_is_null(get_Const_tarval(c1))) {
							/* TODO: move to constant evaluation */
							ir_graph *irg = get_irn_irg(n);
							tv = proj_nr == pn_Cmp_Eq ? get_tarval_b_false() : get_tarval_b_true();
							c1 = new_r_Const(irg, tv);
							DBG_OPT_CSTEVAL(proj, c1);
							return c1;
						}
					}
					break;
				case iro_Shl:
					/*
					 * optimize x << c1 == c into x & (-1 >>u c1) == c >> c1  if  c & (-1 << c1) == c
					 *                             FALSE                       else
					 * optimize x << c1 != c into x & (-1 >>u c1) != c >> c1  if  c & (-1 << c1) == c
					 *                             TRUE                        else
					 */
					c1 = get_Shl_right(left);
					if (is_Const(c1)) {
						ir_graph  *irg    = get_irn_irg(c1);
						ir_tarval *tv1    = get_Const_tarval(c1);
						ir_mode   *mode   = get_irn_mode(left);
						ir_tarval *minus1 = get_mode_all_one(mode);
						ir_tarval *amask  = tarval_shr(minus1, tv1);
						ir_tarval *cmask  = tarval_shl(minus1, tv1);
						ir_node   *sl, *blk;

						if (tarval_and(tv, cmask) != tv) {
							/* condition not met */
							tv = proj_nr == pn_Cmp_Eq ? get_tarval_b_false() : get_tarval_b_true();
							c1 = new_r_Const(irg, tv);
							DBG_OPT_CSTEVAL(proj, c1);
							return c1;
						}
						sl   = get_Shl_left(left);
						blk  = get_nodes_block(n);
						left = new_rd_And(get_irn_dbg_info(left), blk, sl, new_r_Const(irg, amask), mode);
						tv   = tarval_shr(tv, tv1);
						changed |= 2;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_SHF_TO_AND);
					}
					break;
				case iro_Shr:
					/*
					 * optimize x >>u c1 == c into x & (-1 << c1) == c << c1  if  c & (-1 >>u c1) == c
					 *                             FALSE                       else
					 * optimize x >>u c1 != c into x & (-1 << c1) != c << c1  if  c & (-1 >>u c1) == c
					 *                             TRUE                        else
					 */
					c1 = get_Shr_right(left);
					if (is_Const(c1)) {
						ir_graph  *irg    = get_irn_irg(c1);
						ir_tarval *tv1    = get_Const_tarval(c1);
						ir_mode   *mode   = get_irn_mode(left);
						ir_tarval *minus1 = get_mode_all_one(mode);
						ir_tarval *amask  = tarval_shl(minus1, tv1);
						ir_tarval *cmask  = tarval_shr(minus1, tv1);
						ir_node   *sl, *blk;

						if (tarval_and(tv, cmask) != tv) {
							/* condition not met */
							tv = proj_nr == pn_Cmp_Eq ? get_tarval_b_false() : get_tarval_b_true();
							c1 = new_r_Const(irg, tv);
							DBG_OPT_CSTEVAL(proj, c1);
							return c1;
						}
						sl   = get_Shr_left(left);
						blk  = get_nodes_block(n);
						left = new_rd_And(get_irn_dbg_info(left), blk, sl, new_r_Const(irg, amask), mode);
						tv   = tarval_shl(tv, tv1);
						changed |= 2;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_SHF_TO_AND);
					}
					break;
				case iro_Shrs:
					/*
					 * optimize x >>s c1 == c into x & (-1 << c1) == c << c1  if  (c >>s (BITS - c1)) \in {0,-1}
					 *                             FALSE                       else
					 * optimize x >>s c1 != c into x & (-1 << c1) != c << c1  if  (c >>s (BITS - c1)) \in {0,-1}
					 *                             TRUE                        else
					 */
					c1 = get_Shrs_right(left);
					if (is_Const(c1)) {
						ir_graph  *irg    = get_irn_irg(c1);
						ir_tarval *tv1    = get_Const_tarval(c1);
						ir_mode   *mode   = get_irn_mode(left);
						ir_tarval *minus1 = get_mode_all_one(mode);
						ir_tarval *amask  = tarval_shl(minus1, tv1);
						ir_tarval *cond   = new_tarval_from_long(get_mode_size_bits(mode), get_tarval_mode(tv1));
						ir_node *sl, *blk;

						cond = tarval_sub(cond, tv1, NULL);
						cond = tarval_shrs(tv, cond);

						if (!tarval_is_all_one(cond) && !tarval_is_null(cond)) {
							/* condition not met */
							tv = proj_nr == pn_Cmp_Eq ? get_tarval_b_false() : get_tarval_b_true();
							c1 = new_r_Const(irg, tv);
							DBG_OPT_CSTEVAL(proj, c1);
							return c1;
						}
						sl   = get_Shrs_left(left);
						blk  = get_nodes_block(n);
						left = new_rd_And(get_irn_dbg_info(left), blk, sl, new_r_Const(irg, amask), mode);
						tv   = tarval_shl(tv, tv1);
						changed |= 2;
						DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_SHF_TO_AND);
					}
					break;
				}  /* switch */
			}
		} /* tarval != bad */
	}

	if (changed & 2) {     /* need a new Const */
		ir_graph *irg = get_irn_irg(n);
		right = new_r_Const(irg, tv);
	}

	if ((proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg) && is_Const(right) && is_Const_null(right) && is_Proj(left)) {
		ir_node *op = get_Proj_pred(left);

		if ((is_Mod(op) && get_Proj_proj(left) == pn_Mod_res) ||
		    (is_DivMod(op) && get_Proj_proj(left) == pn_DivMod_res_mod)) {
			ir_node *c = get_binop_right(op);

			if (is_Const(c)) {
				ir_tarval *tv = get_Const_tarval(c);

				if (tarval_is_single_bit(tv)) {
					/* special case: (x % 2^n) CMP 0 ==> x & (2^n-1) CMP 0 */
					ir_node *v    = get_binop_left(op);
					ir_node *blk  = get_irn_n(op, -1);
					ir_graph *irg = get_irn_irg(op);
					ir_mode *mode = get_irn_mode(v);

					tv = tarval_sub(tv, get_mode_one(mode), NULL);
					left = new_rd_And(get_irn_dbg_info(op), blk, v, new_r_Const(irg, tv), mode);
					changed |= 1;
					DBG_OPT_ALGSIM0(n, n, FS_OPT_CMP_MOD_TO_AND);
				}
			}
		}
	}

	if (changed) {
		ir_node *block = get_nodes_block(n);

		/* create a new compare */
		n = new_rd_Cmp(get_irn_dbg_info(n), block, left, right);
		proj = new_rd_Proj(get_irn_dbg_info(proj), n, get_irn_mode(proj), proj_nr);
	}

	return proj;
}  /* transform_node_Proj_Cmp */

/**
 * Optimize CopyB(mem, x, x) into a Nop.
 */
static ir_node *transform_node_Proj_CopyB(ir_node *proj)
{
	ir_node *copyb = get_Proj_pred(proj);
	ir_node *a     = get_CopyB_dst(copyb);
	ir_node *b     = get_CopyB_src(copyb);

	if (a == b) {
		switch (get_Proj_proj(proj)) {
		case pn_CopyB_X_regular:
			/* Turn CopyB into a tuple (mem, jmp, bad, bad) */
			DBG_OPT_EXC_REM(proj);
			proj = new_r_Jmp(get_nodes_block(copyb));
			break;
		case pn_CopyB_X_except:
			DBG_OPT_EXC_REM(proj);
			proj = get_irg_bad(get_irn_irg(proj));
			break;
		default:
			break;
		}
	}
	return proj;
}  /* transform_node_Proj_CopyB */

/**
 * Optimize Bounds(idx, idx, upper) into idx.
 */
static ir_node *transform_node_Proj_Bound(ir_node *proj)
{
	ir_node *oldn  = proj;
	ir_node *bound = get_Proj_pred(proj);
	ir_node *idx   = get_Bound_index(bound);
	ir_node *pred  = skip_Proj(idx);
	int ret_tuple  = 0;

	if (idx == get_Bound_lower(bound))
		ret_tuple = 1;
	else if (is_Bound(pred)) {
		/*
		* idx was Bounds checked previously, it is still valid if
		* lower <= pred_lower && pred_upper <= upper.
		*/
		ir_node *lower = get_Bound_lower(bound);
		ir_node *upper = get_Bound_upper(bound);
		if (get_Bound_lower(pred) == lower &&
			get_Bound_upper(pred) == upper) {
			/*
			 * One could expect that we simply return the previous
			 * Bound here. However, this would be wrong, as we could
			 * add an exception Proj to a new location then.
			 * So, we must turn in into a tuple.
			 */
			ret_tuple = 1;
		}
	}
	if (ret_tuple) {
		/* Turn Bound into a tuple (mem, jmp, bad, idx) */
		switch (get_Proj_proj(proj)) {
		case pn_Bound_M:
			DBG_OPT_EXC_REM(proj);
			proj = get_Bound_mem(bound);
			break;
		case pn_Bound_X_except:
			DBG_OPT_EXC_REM(proj);
			proj = get_irg_bad(get_irn_irg(proj));
			break;
		case pn_Bound_res:
			proj = idx;
			DBG_OPT_ALGSIM0(oldn, proj, FS_OPT_NOP);
			break;
		case pn_Bound_X_regular:
			DBG_OPT_EXC_REM(proj);
			proj = new_r_Jmp(get_nodes_block(bound));
			break;
		default:
			break;
		}
	}
	return proj;
}  /* transform_node_Proj_Bound */

/**
 * Does all optimizations on nodes that must be done on it's Proj's
 * because of creating new nodes.
 */
static ir_node *transform_node_Proj(ir_node *proj)
{
	ir_node *n = get_Proj_pred(proj);

	if (n->op->ops.transform_node_Proj)
		return n->op->ops.transform_node_Proj(proj);
	return proj;
}  /* transform_node_Proj */

/**
 * Move Confirms down through Phi nodes.
 */
static ir_node *transform_node_Phi(ir_node *phi)
{
	int i, n;
	ir_mode *mode = get_irn_mode(phi);

	if (mode_is_reference(mode)) {
		n = get_irn_arity(phi);

		/* Beware of Phi0 */
		if (n > 0) {
			ir_node *pred = get_irn_n(phi, 0);
			ir_node *bound, *new_phi, *block, **in;
			pn_Cmp  pnc;

			if (! is_Confirm(pred))
				return phi;

			bound = get_Confirm_bound(pred);
			pnc   = get_Confirm_cmp(pred);

			NEW_ARR_A(ir_node *, in, n);
			in[0] = get_Confirm_value(pred);

			for (i = 1; i < n; ++i) {
				pred = get_irn_n(phi, i);

				if (! is_Confirm(pred) ||
					get_Confirm_bound(pred) != bound ||
					get_Confirm_cmp(pred) != pnc)
					return phi;
				in[i] = get_Confirm_value(pred);
			}
			/* move the Confirm nodes "behind" the Phi */
			block = get_irn_n(phi, -1);
			new_phi = new_r_Phi(block, n, in, get_irn_mode(phi));
			return new_r_Confirm(block, new_phi, bound, pnc);
		}
	}
	return phi;
}  /* transform_node_Phi */

/**
 * Returns the operands of a commutative bin-op, if one operand is
 * a const, it is returned as the second one.
 */
static void get_comm_Binop_Ops(ir_node *binop, ir_node **a, ir_node **c)
{
	ir_node *op_a = get_binop_left(binop);
	ir_node *op_b = get_binop_right(binop);

	assert(is_op_commutative(get_irn_op(binop)));

	if (is_Const(op_a)) {
		*a = op_b;
		*c = op_a;
	} else {
		*a = op_a;
		*c = op_b;
	}
}  /* get_comm_Binop_Ops */

/**
 * Optimize a Or(And(Or(And(v,c4),c3),c2),c1) pattern if possible.
 * Such pattern may arise in bitfield stores.
 *
 * value  c4                  value      c4 & c2
 *    AND     c3                    AND           c1 | c3
 *        OR     c2      ===>               OR
 *           AND    c1
 *               OR
 *
 *
 * value  c2                 value  c1
 *     AND   c1    ===>           OR     if (c1 | c2) == 0x111..11
 *        OR
 */
static ir_node *transform_node_Or_bf_store(ir_node *or)
{
	ir_node *and, *c1;
	ir_node *or_l, *c2;
	ir_node *and_l, *c3;
	ir_node *value, *c4;
	ir_node *new_and, *new_const, *block;
	ir_mode *mode = get_irn_mode(or);

	ir_tarval *tv1, *tv2, *tv3, *tv4, *tv;

	for (;;) {
		ir_graph *irg;
		get_comm_Binop_Ops(or, &and, &c1);
		if (!is_Const(c1) || !is_And(and))
			return or;

		get_comm_Binop_Ops(and, &or_l, &c2);
		if (!is_Const(c2))
			return or;

		tv1 = get_Const_tarval(c1);
		tv2 = get_Const_tarval(c2);

		tv = tarval_or(tv1, tv2);
		if (tarval_is_all_one(tv)) {
			/* the AND does NOT clear a bit with isn't set by the OR */
			set_Or_left(or, or_l);
			set_Or_right(or, c1);

			/* check for more */
			continue;
		}

		if (!is_Or(or_l))
			return or;

		get_comm_Binop_Ops(or_l, &and_l, &c3);
		if (!is_Const(c3) || !is_And(and_l))
			return or;

		get_comm_Binop_Ops(and_l, &value, &c4);
		if (!is_Const(c4))
			return or;

		/* ok, found the pattern, check for conditions */
		assert(mode == get_irn_mode(and));
		assert(mode == get_irn_mode(or_l));
		assert(mode == get_irn_mode(and_l));

		tv3 = get_Const_tarval(c3);
		tv4 = get_Const_tarval(c4);

		tv = tarval_or(tv4, tv2);
		if (!tarval_is_all_one(tv)) {
			/* have at least one 0 at the same bit position */
			return or;
		}

		if (tv3 != tarval_andnot(tv3, tv4)) {
			/* bit in the or_mask is outside the and_mask */
			return or;
		}

		if (tv1 != tarval_andnot(tv1, tv2)) {
			/* bit in the or_mask is outside the and_mask */
			return or;
		}

		/* ok, all conditions met */
		block = get_irn_n(or, -1);
		irg   = get_irn_irg(block);

		new_and = new_r_And(block, value, new_r_Const(irg, tarval_and(tv4, tv2)), mode);

		new_const = new_r_Const(irg, tarval_or(tv3, tv1));

		set_Or_left(or, new_and);
		set_Or_right(or, new_const);

		/* check for more */
	}
}  /* transform_node_Or_bf_store */

/**
 * Optimize an Or(shl(x, c), shr(x, bits - c)) into a Rotl
 */
static ir_node *transform_node_Or_Rotl(ir_node *or)
{
	ir_mode *mode = get_irn_mode(or);
	ir_node *shl, *shr, *block;
	ir_node *irn, *x, *c1, *c2, *n;
	ir_tarval *tv1, *tv2;

	/* some backends can't handle rotl */
	if (!be_get_backend_param()->support_rotl)
		return or;

	if (! mode_is_int(mode))
		return or;

	shl = get_binop_left(or);
	shr = get_binop_right(or);

	if (is_Shr(shl)) {
		if (!is_Shl(shr))
			return or;

		irn = shl;
		shl = shr;
		shr = irn;
	} else if (!is_Shl(shl)) {
		return or;
	} else if (!is_Shr(shr)) {
		return or;
	}
	x = get_Shl_left(shl);
	if (x != get_Shr_left(shr))
		return or;

	c1 = get_Shl_right(shl);
	c2 = get_Shr_right(shr);
	if (is_Const(c1) && is_Const(c2)) {
		tv1 = get_Const_tarval(c1);
		if (! tarval_is_long(tv1))
			return or;

		tv2 = get_Const_tarval(c2);
		if (! tarval_is_long(tv2))
			return or;

		if (get_tarval_long(tv1) + get_tarval_long(tv2)
				!= (int) get_mode_size_bits(mode))
			return or;

		/* yet, condition met */
		block = get_nodes_block(or);

		n = new_r_Rotl(block, x, c1, mode);

		DBG_OPT_ALGSIM1(or, shl, shr, n, FS_OPT_OR_SHFT_TO_ROTL);
		return n;
	}

	/* Note: the obvious rot formulation (a << x) | (a >> (32-x)) gets
	 * transformed to (a << x) | (a >> -x) by transform_node_shift_modulo() */
	if (!is_negated_value(c1, c2)) {
		return or;
	}

	/* yet, condition met */
	block = get_nodes_block(or);
	n = new_r_Rotl(block, x, c1, mode);
	DBG_OPT_ALGSIM0(or, n, FS_OPT_OR_SHFT_TO_ROTL);
	return n;
}  /* transform_node_Or_Rotl */

/**
 * Transform an Or.
 */
static ir_node *transform_node_Or(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_node *a = get_Or_left(n);
	ir_node *b = get_Or_right(n);
	ir_mode *mode;

	if (is_Not(a) && is_Not(b)) {
		/* ~a | ~b = ~(a&b) */
		ir_node *block = get_nodes_block(n);

		mode = get_irn_mode(n);
		a = get_Not_op(a);
		b = get_Not_op(b);
		n = new_rd_And(get_irn_dbg_info(n), block, a, b, mode);
		n = new_rd_Not(get_irn_dbg_info(n), block, n, mode);
		DBG_OPT_ALGSIM0(oldn, n, FS_OPT_DEMORGAN);
		return n;
	}

	/* we can evaluate 2 Projs of the same Cmp */
	if (get_irn_mode(n) == mode_b && is_Proj(a) && is_Proj(b)) {
		ir_node *pred_a = get_Proj_pred(a);
		ir_node *pred_b = get_Proj_pred(b);
		if (pred_a == pred_b) {
			dbg_info *dbgi  = get_irn_dbg_info(n);
			pn_Cmp pn_a     = get_Proj_proj(a);
			pn_Cmp pn_b     = get_Proj_proj(b);
			/* yes, we can simply calculate with pncs */
			pn_Cmp new_pnc  = pn_a | pn_b;

			return new_rd_Proj(dbgi, pred_a, mode_b, new_pnc);
		}
	}

	mode = get_irn_mode(n);
	HANDLE_BINOP_PHI((eval_func) tarval_or, a, b, c, mode);

	n = transform_node_Or_bf_store(n);
	n = transform_node_Or_Rotl(n);
	if (n != oldn)
		return n;

	n = transform_bitwise_distributive(n, transform_node_Or);

	return n;
}  /* transform_node_Or */


/* forward */
static ir_node *transform_node(ir_node *n);

/**
 * Optimize (a >> c1) >> c2), works for Shr, Shrs, Shl, Rotl.
 *
 * Should be moved to reassociation?
 */
static ir_node *transform_node_shift(ir_node *n)
{
	ir_node *left, *right;
	ir_mode *mode;
	ir_tarval *tv1, *tv2, *res;
	ir_node *in[2], *irn, *block;
	ir_graph *irg;

	left = get_binop_left(n);

	/* different operations */
	if (get_irn_op(left) != get_irn_op(n))
		return n;

	right = get_binop_right(n);
	tv1 = value_of(right);
	if (tv1 == tarval_bad)
		return n;

	tv2 = value_of(get_binop_right(left));
	if (tv2 == tarval_bad)
		return n;

	res  = tarval_add(tv1, tv2);
	mode = get_irn_mode(n);
	irg  = get_irn_irg(n);

	/* beware: a simple replacement works only, if res < modulo shift */
	if (!is_Rotl(n)) {
		int modulo_shf = get_mode_modulo_shift(mode);
		if (modulo_shf > 0) {
			ir_tarval *modulo = new_tarval_from_long(modulo_shf,
			                                         get_tarval_mode(res));

			assert(modulo_shf >= (int) get_mode_size_bits(mode));

			/* shifting too much */
			if (!(tarval_cmp(res, modulo) & pn_Cmp_Lt)) {
				if (is_Shrs(n)) {
					ir_node  *block = get_nodes_block(n);
					dbg_info *dbgi  = get_irn_dbg_info(n);
					ir_mode  *smode  = get_irn_mode(right);
					ir_node  *cnst  = new_r_Const_long(irg, smode, get_mode_size_bits(mode) - 1);
					return new_rd_Shrs(dbgi, block, get_binop_left(left), cnst, mode);
				}

				return new_r_Const(irg, get_mode_null(mode));
			}
		}
	} else {
		res = tarval_mod(res, new_tarval_from_long(get_mode_size_bits(mode), get_tarval_mode(res)));
	}

	/* ok, we can replace it */
	block = get_nodes_block(n);

	in[0] = get_binop_left(left);
	in[1] = new_r_Const(irg, res);

	irn = new_ir_node(NULL, get_Block_irg(block), block, get_irn_op(n), mode, 2, in);

	DBG_OPT_ALGSIM0(n, irn, FS_OPT_REASSOC_SHIFT);

	return transform_node(irn);
}  /* transform_node_shift */

/**
 * normalisation: (x & c1) >> c2   to   (x >> c2) & (c1 >> c2)
 *  (we can use:
 *    - and, or, xor          instead of &
 *    - Shl, Shr, Shrs, rotl  instead of >>
 *    (with a special case for Or/Xor + Shrs)
 */
static ir_node *transform_node_bitop_shift(ir_node *n)
{
	ir_node   *left;
	ir_node   *right = get_binop_right(n);
	ir_mode   *mode  = get_irn_mode(n);
	ir_node   *bitop_left;
	ir_node   *bitop_right;
	ir_op     *op_left;
	ir_node   *block;
	dbg_info  *dbgi;
	ir_graph  *irg;
	ir_node   *new_shift;
	ir_node   *new_bitop;
	ir_node   *new_const;
	ir_tarval *tv1;
	ir_tarval *tv2;
	ir_tarval *tv_shift;

	assert(is_Shrs(n) || is_Shr(n) || is_Shl(n) || is_Rotl(n));

	if (!is_Const(right))
		return n;

	left    = get_binop_left(n);
	op_left = get_irn_op(left);
	if (op_left != op_And && op_left != op_Or && op_left != op_Eor)
		return n;

	/* doing it with Shrs is not legal if the Or/Eor affects the topmost bit */
	if (is_Shrs(n) && (op_left == op_Or || op_left == op_Eor)) {
		/* TODO: test if sign bit is affectes */
		return n;
	}

	bitop_right = get_binop_right(left);
	if (!is_Const(bitop_right))
		return n;

	bitop_left = get_binop_left(left);

	block = get_nodes_block(n);
	dbgi  = get_irn_dbg_info(n);
	tv1   = get_Const_tarval(bitop_right);
	tv2   = get_Const_tarval(right);

	assert(get_tarval_mode(tv1) == mode);

	if (is_Shl(n)) {
		new_shift = new_rd_Shl(dbgi, block, bitop_left, right, mode);
		tv_shift  = tarval_shl(tv1, tv2);
	} else if (is_Shr(n)) {
		new_shift = new_rd_Shr(dbgi, block, bitop_left, right, mode);
		tv_shift  = tarval_shr(tv1, tv2);
	} else if (is_Shrs(n)) {
		new_shift = new_rd_Shrs(dbgi, block, bitop_left, right, mode);
		tv_shift  = tarval_shrs(tv1, tv2);
	} else {
		assert(is_Rotl(n));
		new_shift = new_rd_Rotl(dbgi, block, bitop_left, right, mode);
		tv_shift  = tarval_rotl(tv1, tv2);
	}

	assert(get_tarval_mode(tv_shift) == mode);
	irg       = get_irn_irg(n);
	new_const = new_r_Const(irg, tv_shift);

	if (op_left == op_And) {
		new_bitop = new_rd_And(dbgi, block, new_shift, new_const, mode);
	} else if (op_left == op_Or) {
		new_bitop = new_rd_Or(dbgi, block, new_shift, new_const, mode);
	} else {
		assert(op_left == op_Eor);
		new_bitop = new_rd_Eor(dbgi, block, new_shift, new_const, mode);
	}

	return new_bitop;
}

/**
 * normalisation:
 *    (x << c1) >> c2  <=>  x OP (c2-c1) & ((-1 << c1) >> c2)
 *    also:
 *    (x >> c1) << c2  <=>  x OP (c2-c1) & ((-1 >> c1) << c2)
 *      (also with x >>s c1  when c1>=c2)
 */
static ir_node *transform_node_shl_shr(ir_node *n)
{
	ir_node   *left;
	ir_node   *right = get_binop_right(n);
	ir_node   *x;
	ir_node   *block;
	ir_mode   *mode;
	dbg_info  *dbgi;
	ir_node   *new_const;
	ir_node   *new_shift;
	ir_node   *new_and;
	ir_tarval *tv_shl;
	ir_tarval *tv_shr;
	ir_tarval *tv_shift;
	ir_tarval *tv_mask;
	ir_graph  *irg;
	pn_Cmp     pnc;
	int        need_shrs = 0;

	assert(is_Shl(n) || is_Shr(n) || is_Shrs(n));

	if (!is_Const(right))
		return n;

	left = get_binop_left(n);
	mode = get_irn_mode(n);
	if (is_Shl(n) && (is_Shr(left) || is_Shrs(left))) {
		ir_node *shr_right = get_binop_right(left);

		if (!is_Const(shr_right))
			return n;

		x      = get_binop_left(left);
		tv_shr = get_Const_tarval(shr_right);
		tv_shl = get_Const_tarval(right);

		if (is_Shrs(left)) {
			/* shrs variant only allowed if c1 >= c2 */
			if (! (tarval_cmp(tv_shl, tv_shr) & pn_Cmp_Ge))
				return n;

			tv_mask = tarval_shrs(get_mode_all_one(mode), tv_shr);
			need_shrs = 1;
		} else {
			tv_mask = tarval_shr(get_mode_all_one(mode), tv_shr);
		}
		tv_mask = tarval_shl(tv_mask, tv_shl);
	} else if (is_Shr(n) && is_Shl(left)) {
		ir_node *shl_right = get_Shl_right(left);

		if (!is_Const(shl_right))
			return n;

		x      = get_Shl_left(left);
		tv_shr = get_Const_tarval(right);
		tv_shl = get_Const_tarval(shl_right);

		tv_mask = tarval_shl(get_mode_all_one(mode), tv_shl);
		tv_mask = tarval_shr(tv_mask, tv_shr);
	} else {
		return n;
	}

	if (get_tarval_mode(tv_shl) != get_tarval_mode(tv_shr)) {
		tv_shl = tarval_convert_to(tv_shl, get_tarval_mode(tv_shr));
	}

	assert(tv_mask != tarval_bad);
	assert(get_tarval_mode(tv_mask) == mode);

	block = get_nodes_block(n);
	irg   = get_irn_irg(block);
	dbgi  = get_irn_dbg_info(n);

	pnc = tarval_cmp(tv_shl, tv_shr);
	if (pnc == pn_Cmp_Lt || pnc == pn_Cmp_Eq) {
		tv_shift  = tarval_sub(tv_shr, tv_shl, NULL);
		new_const = new_r_Const(irg, tv_shift);
		if (need_shrs) {
			new_shift = new_rd_Shrs(dbgi, block, x, new_const, mode);
		} else {
			new_shift = new_rd_Shr(dbgi, block, x, new_const, mode);
		}
	} else {
		assert(pnc == pn_Cmp_Gt);
		tv_shift  = tarval_sub(tv_shl, tv_shr, NULL);
		new_const = new_r_Const(irg, tv_shift);
		new_shift = new_rd_Shl(dbgi, block, x, new_const, mode);
	}

	new_const = new_r_Const(irg, tv_mask);
	new_and   = new_rd_And(dbgi, block, new_shift, new_const, mode);

	return new_and;
}

static ir_tarval *get_modulo_tv_value(ir_tarval *tv, int modulo_val)
{
	ir_mode   *mode      = get_tarval_mode(tv);
	ir_tarval *modulo_tv = new_tarval_from_long(modulo_val, mode);
	return tarval_mod(tv, modulo_tv);
}

typedef ir_node*(*new_shift_func)(dbg_info *dbgi, ir_node *block,
                                  ir_node *left, ir_node *right, ir_mode *mode);

/**
 * Normalisation: if we have a shl/shr with modulo_shift behaviour
 * then we can use that to minimize the value of Add(x, const) or
 * Sub(Const, x). In particular this often avoids 1 instruction in some
 * backends for the Shift(x, Sub(Const, y)) case because it can be replaced
 * by Shift(x, Minus(y)) which doesnt't need an explicit Const constructed.
 */
static ir_node *transform_node_shift_modulo(ir_node *n,
                                            new_shift_func new_shift)
{
	ir_mode  *mode   = get_irn_mode(n);
	int       modulo = get_mode_modulo_shift(mode);
	ir_node  *newop  = NULL;
	ir_mode  *mode_right;
	ir_node  *block;
	ir_node  *right;
	ir_graph *irg;

	if (modulo == 0)
		return n;
	if (get_mode_arithmetic(mode) != irma_twos_complement)
		return n;
	if (!is_po2(modulo))
		return n;

	irg        = get_irn_irg(n);
	block      = get_nodes_block(n);
	right      = get_binop_right(n);
	mode_right = get_irn_mode(right);
	if (is_Const(right)) {
		ir_tarval *tv     = get_Const_tarval(right);
		ir_tarval *tv_mod = get_modulo_tv_value(tv, modulo);

		if (tv_mod == tv)
			return n;

		newop = new_r_Const(irg, tv_mod);
	} else if (is_Add(right)) {
		ir_node *add_right = get_Add_right(right);
		if (is_Const(add_right)) {
			ir_tarval *tv     = get_Const_tarval(add_right);
			ir_tarval *tv_mod = get_modulo_tv_value(tv, modulo);
			ir_node   *newconst;
			if (tv_mod == tv)
				return n;

			newconst = new_r_Const(irg, tv_mod);
			newop    = new_r_Add(block, get_Add_left(right), newconst,
			                     mode_right);
		}
	} else if (is_Sub(right)) {
		ir_node *sub_left = get_Sub_left(right);
		if (is_Const(sub_left)) {
			ir_tarval *tv     = get_Const_tarval(sub_left);
			ir_tarval *tv_mod = get_modulo_tv_value(tv, modulo);
			ir_node  *newconst;
			if (tv_mod == tv)
				return n;

			newconst = new_r_Const(irg, tv_mod);
			newop    = new_r_Sub(block, newconst, get_Sub_right(right),
			                     mode_right);
		}
	} else {
		return n;
	}

	if (newop != NULL) {
		dbg_info *dbgi = get_irn_dbg_info(n);
		ir_node  *left = get_binop_left(n);
		return new_shift(dbgi, block, left, newop, mode);
	}
	return n;
}

/**
 * Transform a Shr.
 */
static ir_node *transform_node_Shr(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_node *left  = get_Shr_left(n);
	ir_node *right = get_Shr_right(n);
	ir_mode *mode  = get_irn_mode(n);

	HANDLE_BINOP_PHI((eval_func) tarval_shr, left, right, c, mode);
	n = transform_node_shift(n);

	if (is_Shr(n))
		n = transform_node_shift_modulo(n, new_rd_Shr);
	if (is_Shr(n))
		n = transform_node_shl_shr(n);
	if (is_Shr(n))
		n = transform_node_bitop_shift(n);

	return n;
}  /* transform_node_Shr */

/**
 * Transform a Shrs.
 */
static ir_node *transform_node_Shrs(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_node *a    = get_Shrs_left(n);
	ir_node *b    = get_Shrs_right(n);
	ir_mode *mode = get_irn_mode(n);

	HANDLE_BINOP_PHI((eval_func) tarval_shrs, a, b, c, mode);
	n = transform_node_shift(n);

	if (is_Shrs(n))
		n = transform_node_shift_modulo(n, new_rd_Shrs);
	if (is_Shrs(n))
		n = transform_node_bitop_shift(n);

	return n;
}  /* transform_node_Shrs */

/**
 * Transform a Shl.
 */
static ir_node *transform_node_Shl(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_node *a    = get_Shl_left(n);
	ir_node *b    = get_Shl_right(n);
	ir_mode *mode = get_irn_mode(n);

	HANDLE_BINOP_PHI((eval_func) tarval_shl, a, b, c, mode);
	n = transform_node_shift(n);

	if (is_Shl(n))
		n = transform_node_shift_modulo(n, new_rd_Shl);
	if (is_Shl(n))
		n = transform_node_shl_shr(n);
	if (is_Shl(n))
		n = transform_node_bitop_shift(n);

	return n;
}  /* transform_node_Shl */

/**
 * Transform a Rotl.
 */
static ir_node *transform_node_Rotl(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_node *a    = get_Rotl_left(n);
	ir_node *b    = get_Rotl_right(n);
	ir_mode *mode = get_irn_mode(n);

	HANDLE_BINOP_PHI((eval_func) tarval_rotl, a, b, c, mode);
	n = transform_node_shift(n);

	if (is_Rotl(n))
		n = transform_node_bitop_shift(n);

	return n;
}  /* transform_node_Rotl */

/**
 * Transform a Conv.
 */
static ir_node *transform_node_Conv(ir_node *n)
{
	ir_node *c, *oldn = n;
	ir_mode *mode = get_irn_mode(n);
	ir_node *a    = get_Conv_op(n);

	if (mode != mode_b && is_const_Phi(a)) {
		/* Do NOT optimize mode_b Conv's, this leads to remaining
		 * Phib nodes later, because the conv_b_lower operation
		 * is instantly reverted, when it tries to insert a Convb.
		 */
		c = apply_conv_on_phi(a, mode);
		if (c) {
			DBG_OPT_ALGSIM0(oldn, c, FS_OPT_CONST_PHI);
			return c;
		}
	}

	if (is_Unknown(a)) { /* Conv_A(Unknown_B) -> Unknown_A */
		ir_graph *irg = get_irn_irg(n);
		return new_r_Unknown(irg, mode);
	}

	if (mode_is_reference(mode) &&
	        get_mode_size_bits(mode) == get_mode_size_bits(get_irn_mode(a)) &&
	        is_Add(a)) {
		ir_node *l = get_Add_left(a);
		ir_node *r = get_Add_right(a);
		dbg_info *dbgi = get_irn_dbg_info(a);
		ir_node *block = get_nodes_block(n);
		if (is_Conv(l)) {
			ir_node *lop = get_Conv_op(l);
			if (get_irn_mode(lop) == mode) {
				/* ConvP(AddI(ConvI(P), x)) -> AddP(P, x) */
				n = new_rd_Add(dbgi, block, lop, r, mode);
				return n;
			}
		}
		if (is_Conv(r)) {
			ir_node *rop = get_Conv_op(r);
			if (get_irn_mode(rop) == mode) {
				/* ConvP(AddI(x, ConvI(P))) -> AddP(x, P) */
				n = new_rd_Add(dbgi, block, l, rop, mode);
				return n;
			}
		}
	}

	return n;
}  /* transform_node_Conv */

/**
 * Remove dead blocks and nodes in dead blocks
 * in keep alive list.  We do not generate a new End node.
 */
static ir_node *transform_node_End(ir_node *n)
{
	int i, j, n_keepalives = get_End_n_keepalives(n);
	ir_node **in;

	NEW_ARR_A(ir_node *, in, n_keepalives);

	for (i = j = 0; i < n_keepalives; ++i) {
		ir_node *ka = get_End_keepalive(n, i);
		if (is_Block(ka)) {
			if (! is_Block_dead(ka)) {
				in[j++] = ka;
			}
			continue;
		} else if (is_irn_pinned_in_irg(ka) && is_Block_dead(get_nodes_block(ka))) {
			continue;
		} else if (is_Bad(ka)) {
			/* no need to keep Bad */
			continue;
		}
		in[j++] = ka;
	}
	if (j != n_keepalives)
		set_End_keepalives(n, j, in);
	return n;
}  /* transform_node_End */

bool is_negated_value(ir_node *a, ir_node *b)
{
	if (is_Minus(a) && get_Minus_op(a) == b)
		return true;
	if (is_Minus(b) && get_Minus_op(b) == a)
		return true;
	if (is_Sub(a) && is_Sub(b)) {
		ir_node *a_left  = get_Sub_left(a);
		ir_node *a_right = get_Sub_right(a);
		ir_node *b_left  = get_Sub_left(b);
		ir_node *b_right = get_Sub_right(b);

		if (a_left == b_right && a_right == b_left)
			return true;
	}

	return false;
}

/**
 * Optimize a Mux into some simpler cases.
 */
static ir_node *transform_node_Mux(ir_node *n)
{
	ir_node *oldn = n, *sel = get_Mux_sel(n);
	ir_mode *mode = get_irn_mode(n);
	ir_node  *t   = get_Mux_true(n);
	ir_node  *f   = get_Mux_false(n);
	ir_graph *irg = get_irn_irg(n);

	if (is_irg_state(irg, IR_GRAPH_STATE_KEEP_MUX))
		return n;

	if (is_Mux(t)) {
		ir_node*  block = get_nodes_block(n);
		ir_node*  c0    = sel;
		ir_node*  c1    = get_Mux_sel(t);
		ir_node*  t1    = get_Mux_true(t);
		ir_node*  f1    = get_Mux_false(t);
		if (f == f1) {
			/* Mux(cond0, Mux(cond1, x, y), y) -> typical if (cond0 && cond1) x else y */
			ir_node* and_    = new_r_And(block, c0, c1, mode_b);
			ir_node* new_mux = new_r_Mux(block, and_, f1, t1, mode);
			n   = new_mux;
			sel = and_;
			f   = f1;
			t   = t1;
			DBG_OPT_ALGSIM0(oldn, t, FS_OPT_MUX_COMBINE);
		} else if (f == t1) {
			/* Mux(cond0, Mux(cond1, x, y), x) */
			ir_node* not_c1  = new_r_Not(block, c1, mode_b);
			ir_node* and_    = new_r_And(block, c0, not_c1, mode_b);
			ir_node* new_mux = new_r_Mux(block, and_, t1, f1, mode);
			n   = new_mux;
			sel = and_;
			f   = t1;
			t   = f1;
			DBG_OPT_ALGSIM0(oldn, t, FS_OPT_MUX_COMBINE);
		}
	} else if (is_Mux(f)) {
		ir_node*  block = get_nodes_block(n);
		ir_node*  c0    = sel;
		ir_node*  c1    = get_Mux_sel(f);
		ir_node*  t1    = get_Mux_true(f);
		ir_node*  f1    = get_Mux_false(f);
		if (t == t1) {
			/* Mux(cond0, x, Mux(cond1, x, y)) -> typical if (cond0 || cond1) x else y */
			ir_node* or_     = new_r_Or(block, c0, c1, mode_b);
			ir_node* new_mux = new_r_Mux(block, or_, f1, t1, mode);
			n   = new_mux;
			sel = or_;
			f   = f1;
			t   = t1;
			DBG_OPT_ALGSIM0(oldn, f, FS_OPT_MUX_COMBINE);
		} else if (t == f1) {
			/* Mux(cond0, x, Mux(cond1, y, x)) */
			ir_node* not_c1  = new_r_Not(block, c1, mode_b);
			ir_node* or_     = new_r_Or(block, c0, not_c1, mode_b);
			ir_node* new_mux = new_r_Mux(block, or_, t1, f1, mode);
			n   = new_mux;
			sel = or_;
			f   = t1;
			t   = f1;
			DBG_OPT_ALGSIM0(oldn, f, FS_OPT_MUX_COMBINE);
		}
	}

	/* first normalization step: try to move a constant to the false side,
	 * 0 preferred on false side too */
	if (is_Proj(sel)) {
		ir_node *cmp = get_Proj_pred(sel);

		if (is_Cmp(cmp) && is_Const(t) &&
		    (!is_Const(f) || (is_Const_null(t) && !is_Const_null(f)))) {
			pn_Cmp pnc = get_Proj_proj(sel);
			ir_node *tmp = t;
			t = f;
			f = tmp;

			/* Mux(x, a, b) => Mux(not(x), b, a) */
			sel = new_r_Proj(cmp, mode_b,
				get_negated_pnc(pnc, get_irn_mode(get_Cmp_left(cmp))));
			n = new_rd_Mux(get_irn_dbg_info(n), get_nodes_block(n), sel, f, t, mode);
		}
	}

	/* note: after normalization, false can only happen on default */
	if (mode == mode_b) {
		dbg_info *dbg   = get_irn_dbg_info(n);
		ir_node  *block = get_nodes_block(n);

		if (is_Const(t)) {
			ir_tarval *tv_t = get_Const_tarval(t);
			if (tv_t == tarval_b_true) {
				if (is_Const(f)) {
					/* Muxb(sel, true, false) = sel */
					assert(get_Const_tarval(f) == tarval_b_false);
					DBG_OPT_ALGSIM0(oldn, sel, FS_OPT_MUX_BOOL);
					return sel;
				} else {
					/* Muxb(sel, true, x) = Or(sel, x) */
					n = new_rd_Or(dbg, block, sel, f, mode_b);
					DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_OR_BOOL);
					return n;
				}
			}
		} else if (is_Const(f)) {
			ir_tarval *tv_f = get_Const_tarval(f);
			if (tv_f == tarval_b_true) {
				/* Muxb(sel, x, true) = Or(Not(sel), x) */
				ir_node* not_sel = new_rd_Not(dbg, block, sel, mode_b);
				DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_ORNOT_BOOL);
				n = new_rd_Or(dbg, block, not_sel, t, mode_b);
				return n;
			} else {
				/* Muxb(sel, x, false) = And(sel, x) */
				assert(tv_f == tarval_b_false);
				n = new_rd_And(dbg, block, sel, t, mode_b);
				DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_AND_BOOL);
				return n;
			}
		}
	}

	/* more normalization: Mux(sel, 0, 1) is simply a conv from the mode_b
	 * value to integer. */
	if (is_Const(t) && is_Const(f) && mode_is_int(mode)) {
		ir_tarval *a = get_Const_tarval(t);
		ir_tarval *b = get_Const_tarval(f);

		if (tarval_is_one(a) && tarval_is_null(b)) {
			ir_node *block = get_nodes_block(n);
			ir_node *conv  = new_r_Conv(block, sel, mode);
			n = conv;
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_CONV);
			return n;
		} else if (tarval_is_null(a) && tarval_is_one(b)) {
			ir_node *block = get_nodes_block(n);
			ir_node *not_  = new_r_Not(block, sel, mode_b);
			ir_node *conv  = new_r_Conv(block, not_, mode);
			n = conv;
			DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_CONV);
			return n;
		}
	}

	if (is_Proj(sel)) {
		ir_node *cmp = get_Proj_pred(sel);
		long     pn  = get_Proj_proj(sel);

		/*
		 * Note: normalization puts the constant on the right side,
		 * so we check only one case.
		 */
		if (is_Cmp(cmp)) {
			ir_node *cmp_r = get_Cmp_right(cmp);
			if (is_Const(cmp_r) && is_Const_null(cmp_r)) {
				ir_node *block = get_nodes_block(n);
				ir_node *cmp_l = get_Cmp_left(cmp);

				if (mode_is_int(mode)) {
					/* integer only */
					if ((pn == pn_Cmp_Lg || pn == pn_Cmp_Eq) && is_And(cmp_l)) {
						/* Mux((a & b) != 0, c, 0) */
						ir_node *and_r = get_And_right(cmp_l);
						ir_node *and_l;

						if (and_r == t && f == cmp_r) {
							if (is_Const(t) && tarval_is_single_bit(get_Const_tarval(t))) {
								if (pn == pn_Cmp_Lg) {
									/* Mux((a & 2^C) != 0, 2^C, 0) */
									n = cmp_l;
									DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_BITOP);
								} else {
									/* Mux((a & 2^C) == 0, 2^C, 0) */
									n = new_rd_Eor(get_irn_dbg_info(n),
										block, cmp_l, t, mode);
									DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_BITOP);
								}
								return n;
							}
						}
						if (is_Shl(and_r)) {
							ir_node *shl_l = get_Shl_left(and_r);
							if (is_Const(shl_l) && is_Const_one(shl_l)) {
								if (and_r == t && f == cmp_r) {
									if (pn == pn_Cmp_Lg) {
										/* (a & (1 << n)) != 0, (1 << n), 0) */
										n = cmp_l;
										DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_BITOP);
									} else {
										/* (a & (1 << n)) == 0, (1 << n), 0) */
										n = new_rd_Eor(get_irn_dbg_info(n),
											block, cmp_l, t, mode);
										DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_BITOP);
									}
									return n;
								}
							}
						}
						and_l = get_And_left(cmp_l);
						if (is_Shl(and_l)) {
							ir_node *shl_l = get_Shl_left(and_l);
							if (is_Const(shl_l) && is_Const_one(shl_l)) {
								if (and_l == t && f == cmp_r) {
									if (pn == pn_Cmp_Lg) {
										/* ((1 << n) & a) != 0, (1 << n), 0) */
										n = cmp_l;
										DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_BITOP);
									} else {
										/* ((1 << n) & a) == 0, (1 << n), 0) */
										n = new_rd_Eor(get_irn_dbg_info(n),
											block, cmp_l, t, mode);
										DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_BITOP);
									}
									return n;
								}
							}
						}
					}
				}
			}
		}
	}

	return n;
}  /* transform_node_Mux */

/**
 * optimize Sync nodes that have other syncs as input we simply add the inputs
 * of the other sync to our own inputs
 */
static ir_node *transform_node_Sync(ir_node *n)
{
	int arity = get_Sync_n_preds(n);
	int i;

	for (i = 0; i < arity;) {
		ir_node *pred = get_Sync_pred(n, i);
		int      pred_arity;
		int      j;

		if (!is_Sync(pred)) {
			++i;
			continue;
		}

		del_Sync_n(n, i);
		--arity;

		pred_arity = get_Sync_n_preds(pred);
		for (j = 0; j < pred_arity; ++j) {
			ir_node *pred_pred = get_Sync_pred(pred, j);
			int      k;

			for (k = 0;; ++k) {
				if (k >= arity) {
					add_irn_n(n, pred_pred);
					++arity;
					break;
				}
				if (get_Sync_pred(n, k) == pred_pred) break;
			}
		}
	}

	/* rehash the sync node */
	add_identities(n);

	return n;
}  /* transform_node_Sync */

/**
 * optimize a trampoline Call into a direct Call
 */
static ir_node *transform_node_Call(ir_node *call)
{
	ir_node  *callee = get_Call_ptr(call);
	ir_node  *adr, *mem, *res, *bl, **in;
	ir_type  *ctp, *mtp, *tp;
	ir_graph *irg;
	type_dbg_info *tdb;
	dbg_info *db;
	int      i, n_res, n_param;
	ir_variadicity var;

	if (! is_Proj(callee))
		return call;
	callee = get_Proj_pred(callee);
	if (! is_Builtin(callee))
		return call;
	if (get_Builtin_kind(callee) != ir_bk_inner_trampoline)
		return call;

	mem = get_Call_mem(call);

	if (skip_Proj(mem) == callee) {
		/* memory is routed to the trampoline, skip */
		mem = get_Builtin_mem(callee);
	}

	/* build a new call type */
	mtp = get_Call_type(call);
	tdb = get_type_dbg_info(mtp);

	n_res   = get_method_n_ress(mtp);
	n_param = get_method_n_params(mtp);
	ctp     = new_d_type_method(n_param + 1, n_res, tdb);

	for (i = 0; i < n_res; ++i)
		set_method_res_type(ctp, i, get_method_res_type(mtp, i));

	NEW_ARR_A(ir_node *, in, n_param + 1);

	/* FIXME: we don't need a new pointer type in every step */
	irg = get_irn_irg(call);
	tp = get_irg_frame_type(irg);
	tp = new_type_pointer(tp);
	set_method_param_type(ctp, 0, tp);

	in[0] = get_Builtin_param(callee, 2);
	for (i = 0; i < n_param; ++i) {
		set_method_param_type(ctp, i + 1, get_method_param_type(mtp, i));
		in[i + 1] = get_Call_param(call, i);
	}
	var = get_method_variadicity(mtp);
	set_method_variadicity(ctp, var);
	if (var == variadicity_variadic) {
		set_method_first_variadic_param_index(ctp, get_method_first_variadic_param_index(mtp) + 1);
	}
	/* When we resolve a trampoline, the function must be called by a this-call */
	set_method_calling_convention(ctp, get_method_calling_convention(mtp) | cc_this_call);
	set_method_additional_properties(ctp, get_method_additional_properties(mtp));

	adr = get_Builtin_param(callee, 1);

	db  = get_irn_dbg_info(call);
	bl  = get_nodes_block(call);

	res = new_rd_Call(db, bl, mem, adr, n_param + 1, in, ctp);
	if (get_irn_pinned(call) == op_pin_state_floats)
		set_irn_pinned(res, op_pin_state_floats);
	return res;
}  /* transform_node_Call */

/**
 * Tries several [inplace] [optimizing] transformations and returns an
 * equivalent node.  The difference to equivalent_node() is that these
 * transformations _do_ generate new nodes, and thus the old node must
 * not be freed even if the equivalent node isn't the old one.
 */
static ir_node *transform_node(ir_node *n)
{
	ir_node *oldn;

	/*
	 * Transform_node is the only "optimizing transformation" that might
	 * return a node with a different opcode. We iterate HERE until fixpoint
	 * to get the final result.
	 */
	do {
		oldn = n;
		if (n->op->ops.transform_node != NULL)
			n = n->op->ops.transform_node(n);
	} while (oldn != n);

	return n;
}  /* transform_node */

/**
 * Sets the default transform node operation for an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
static ir_op_ops *firm_set_default_transform_node(ir_opcode code, ir_op_ops *ops)
{
#define CASE(a)                                         \
	case iro_##a:                                       \
		ops->transform_node      = transform_node_##a;  \
		break
#define CASE_PROJ(a)                                         \
	case iro_##a:                                            \
		ops->transform_node_Proj = transform_node_Proj_##a;  \
		break
#define CASE_PROJ_EX(a)                                      \
	case iro_##a:                                            \
		ops->transform_node      = transform_node_##a;       \
		ops->transform_node_Proj = transform_node_Proj_##a;  \
		break

	switch (code) {
	CASE(Add);
	CASE(Sub);
	CASE(Mul);
	CASE_PROJ_EX(Div);
	CASE_PROJ_EX(Mod);
	CASE_PROJ_EX(DivMod);
	CASE(Quot);
	CASE_PROJ_EX(Cmp);
	CASE_PROJ_EX(Cond);
	CASE(And);
	CASE(Eor);
	CASE(Not);
	CASE(Minus);
	CASE_PROJ(Load);
	CASE_PROJ(Store);
	CASE_PROJ(Bound);
	CASE_PROJ(CopyB);
	CASE(Proj);
	CASE(Phi);
	CASE(Or);
	CASE(Sel);
	CASE(Shr);
	CASE(Shrs);
	CASE(Shl);
	CASE(Rotl);
	CASE(Conv);
	CASE(End);
	CASE(Mux);
	CASE(Sync);
	CASE(Call);
	default:
	  /* leave NULL */;
	}

	return ops;
#undef CASE_PROJ_EX
#undef CASE_PROJ
#undef CASE
}  /* firm_set_default_transform_node */


/* **************** Common Subexpression Elimination **************** */

/** The size of the hash table used, should estimate the number of nodes
    in a graph. */
#define N_IR_NODES 512

/** Compares the attributes of two Const nodes. */
static int node_cmp_attr_Const(ir_node *a, ir_node *b)
{
	return get_Const_tarval(a) != get_Const_tarval(b);
}

/** Compares the attributes of two Proj nodes. */
static int node_cmp_attr_Proj(ir_node *a, ir_node *b)
{
	return a->attr.proj != b->attr.proj;
}  /* node_cmp_attr_Proj */

/** Compares the attributes of two Alloc nodes. */
static int node_cmp_attr_Alloc(ir_node *a, ir_node *b)
{
	const alloc_attr *pa = &a->attr.alloc;
	const alloc_attr *pb = &b->attr.alloc;
	return (pa->where != pb->where) || (pa->type != pb->type);
}  /* node_cmp_attr_Alloc */

/** Compares the attributes of two Free nodes. */
static int node_cmp_attr_Free(ir_node *a, ir_node *b)
{
	const free_attr *pa = &a->attr.free;
	const free_attr *pb = &b->attr.free;
	return (pa->where != pb->where) || (pa->type != pb->type);
}  /* node_cmp_attr_Free */

/** Compares the attributes of two SymConst nodes. */
static int node_cmp_attr_SymConst(ir_node *a, ir_node *b)
{
	const symconst_attr *pa = &a->attr.symc;
	const symconst_attr *pb = &b->attr.symc;
	return (pa->kind       != pb->kind)
	    || (pa->sym.type_p != pb->sym.type_p);
}

/** Compares the attributes of two Call nodes. */
static int node_cmp_attr_Call(ir_node *a, ir_node *b)
{
	const call_attr *pa = &a->attr.call;
	const call_attr *pb = &b->attr.call;
	return (pa->type != pb->type)
		|| (pa->tail_call != pb->tail_call);
}  /* node_cmp_attr_Call */

/** Compares the attributes of two Sel nodes. */
static int node_cmp_attr_Sel(ir_node *a, ir_node *b)
{
	const ir_entity *a_ent = get_Sel_entity(a);
	const ir_entity *b_ent = get_Sel_entity(b);
	return a_ent != b_ent;
}  /* node_cmp_attr_Sel */

/** Compares the attributes of two Phi nodes. */
static int node_cmp_attr_Phi(ir_node *a, ir_node *b)
{
	/* we can only enter this function if both nodes have the same number of inputs,
	   hence it is enough to check if one of them is a Phi0 */
	if (is_Phi0(a)) {
		/* check the Phi0 pos attribute */
		return a->attr.phi.u.pos != b->attr.phi.u.pos;
	}
	return 0;
}  /* node_cmp_attr_Phi */

/** Compares the attributes of two Conv nodes. */
static int node_cmp_attr_Conv(ir_node *a, ir_node *b)
{
	return get_Conv_strict(a) != get_Conv_strict(b);
}  /* node_cmp_attr_Conv */

/** Compares the attributes of two Cast nodes. */
static int node_cmp_attr_Cast(ir_node *a, ir_node *b)
{
	return get_Cast_type(a) != get_Cast_type(b);
}  /* node_cmp_attr_Cast */

/** Compares the attributes of two Load nodes. */
static int node_cmp_attr_Load(ir_node *a, ir_node *b)
{
	if (get_Load_volatility(a) == volatility_is_volatile ||
	    get_Load_volatility(b) == volatility_is_volatile)
		/* NEVER do CSE on volatile Loads */
		return 1;
	/* do not CSE Loads with different alignment. Be conservative. */
	if (get_Load_align(a) != get_Load_align(b))
		return 1;

	return get_Load_mode(a) != get_Load_mode(b);
}  /* node_cmp_attr_Load */

/** Compares the attributes of two Store nodes. */
static int node_cmp_attr_Store(ir_node *a, ir_node *b)
{
	/* do not CSE Stores with different alignment. Be conservative. */
	if (get_Store_align(a) != get_Store_align(b))
		return 1;

	/* NEVER do CSE on volatile Stores */
	return (get_Store_volatility(a) == volatility_is_volatile ||
	        get_Store_volatility(b) == volatility_is_volatile);
}  /* node_cmp_attr_Store */

/** Compares two exception attributes */
static int node_cmp_exception(ir_node *a, ir_node *b)
{
	const except_attr *ea = &a->attr.except;
	const except_attr *eb = &b->attr.except;

	return ea->pin_state != eb->pin_state;
}

#define node_cmp_attr_Bound  node_cmp_exception

/** Compares the attributes of two Div nodes. */
static int node_cmp_attr_Div(ir_node *a, ir_node *b)
{
	const divmod_attr *ma = &a->attr.divmod;
	const divmod_attr *mb = &b->attr.divmod;
	return ma->exc.pin_state != mb->exc.pin_state ||
		   ma->resmode       != mb->resmode ||
		   ma->no_remainder  != mb->no_remainder;
}  /* node_cmp_attr_Div */

/** Compares the attributes of two DivMod nodes. */
static int node_cmp_attr_DivMod(ir_node *a, ir_node *b)
{
	const divmod_attr *ma = &a->attr.divmod;
	const divmod_attr *mb = &b->attr.divmod;
	return ma->exc.pin_state != mb->exc.pin_state ||
		   ma->resmode       != mb->resmode;
}  /* node_cmp_attr_DivMod */

/** Compares the attributes of two Mod nodes. */
static int node_cmp_attr_Mod(ir_node *a, ir_node *b)
{
	return node_cmp_attr_DivMod(a, b);
}  /* node_cmp_attr_Mod */

/** Compares the attributes of two Quot nodes. */
static int node_cmp_attr_Quot(ir_node *a, ir_node *b)
{
	return node_cmp_attr_DivMod(a, b);
}  /* node_cmp_attr_Quot */

/** Compares the attributes of two Confirm nodes. */
static int node_cmp_attr_Confirm(ir_node *a, ir_node *b)
{
	/* no need to compare the bound, as this is a input */
	return (get_Confirm_cmp(a) != get_Confirm_cmp(b));
}  /* node_cmp_attr_Confirm */

/** Compares the attributes of two Builtin nodes. */
static int node_cmp_attr_Builtin(ir_node *a, ir_node *b)
{
	/* no need to compare the type, equal kind means equal type */
	return get_Builtin_kind(a) != get_Builtin_kind(b);
}  /* node_cmp_attr_Builtin */

/** Compares the attributes of two ASM nodes. */
static int node_cmp_attr_ASM(ir_node *a, ir_node *b)
{
	int i, n;
	const ir_asm_constraint *ca;
	const ir_asm_constraint *cb;
	ident **cla, **clb;

	if (get_ASM_text(a) != get_ASM_text(b))
		return 1;

	/* Should we really check the constraints here? Should be better, but is strange. */
	n = get_ASM_n_input_constraints(a);
	if (n != get_ASM_n_input_constraints(b))
		return 0;

	ca = get_ASM_input_constraints(a);
	cb = get_ASM_input_constraints(b);
	for (i = 0; i < n; ++i) {
		if (ca[i].pos != cb[i].pos || ca[i].constraint != cb[i].constraint)
			return 1;
	}

	n = get_ASM_n_output_constraints(a);
	if (n != get_ASM_n_output_constraints(b))
		return 0;

	ca = get_ASM_output_constraints(a);
	cb = get_ASM_output_constraints(b);
	for (i = 0; i < n; ++i) {
		if (ca[i].pos != cb[i].pos || ca[i].constraint != cb[i].constraint)
			return 1;
	}

	n = get_ASM_n_clobbers(a);
	if (n != get_ASM_n_clobbers(b))
		return 0;

	cla = get_ASM_clobbers(a);
	clb = get_ASM_clobbers(b);
	for (i = 0; i < n; ++i) {
		if (cla[i] != clb[i])
			return 1;
	}
	return 0;
}  /* node_cmp_attr_ASM */

/** Compares the inexistent attributes of two Dummy nodes. */
static int node_cmp_attr_Dummy(ir_node *a, ir_node *b)
{
	(void) a;
	(void) b;
	return 1;
}

/**
 * Set the default node attribute compare operation for an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
static ir_op_ops *firm_set_default_node_cmp_attr(ir_opcode code, ir_op_ops *ops)
{
#define CASE(a)                              \
	case iro_##a:                              \
		ops->node_cmp_attr  = node_cmp_attr_##a; \
		break

	switch (code) {
	CASE(Const);
	CASE(Proj);
	CASE(Alloc);
	CASE(Free);
	CASE(SymConst);
	CASE(Call);
	CASE(Sel);
	CASE(Phi);
	CASE(Conv);
	CASE(Cast);
	CASE(Load);
	CASE(Store);
	CASE(Confirm);
	CASE(ASM);
	CASE(Div);
	CASE(DivMod);
	CASE(Mod);
	CASE(Quot);
	CASE(Bound);
	CASE(Builtin);
	CASE(Dummy);
	/* FIXME CopyB */
	default:
		/* leave NULL */
		break;
	}

	return ops;
#undef CASE
}  /* firm_set_default_node_cmp_attr */

/*
 * Compare function for two nodes in the value table. Gets two
 * nodes as parameters.  Returns 0 if the nodes are a Common Sub Expression.
 */
int identities_cmp(const void *elt, const void *key)
{
	ir_node *a = (ir_node *)elt;
	ir_node *b = (ir_node *)key;
	int i, irn_arity_a;

	if (a == b) return 0;

	if ((get_irn_op(a) != get_irn_op(b)) ||
	    (get_irn_mode(a) != get_irn_mode(b))) return 1;

	/* compare if a's in and b's in are of equal length */
	irn_arity_a = get_irn_arity(a);
	if (irn_arity_a != get_irn_arity(b))
		return 1;

	/* blocks are never the same */
	if (is_Block(a))
		return 1;

	if (get_irn_pinned(a) == op_pin_state_pinned) {
		/* for pinned nodes, the block inputs must be equal */
		if (get_irn_n(a, -1) != get_irn_n(b, -1))
			return 1;
	} else if (! get_opt_global_cse()) {
		/* for block-local CSE both nodes must be in the same Block */
		if (get_nodes_block(a) != get_nodes_block(b))
			return 1;
	}

	/* compare a->in[0..ins] with b->in[0..ins] */
	for (i = 0; i < irn_arity_a; ++i) {
		ir_node *pred_a = get_irn_n(a, i);
		ir_node *pred_b = get_irn_n(b, i);
		if (pred_a != pred_b) {
			/* if both predecessors are CSE neutral they might be different */
			if (!is_irn_cse_neutral(pred_a) || !is_irn_cse_neutral(pred_b))
				return 1;
		}
	}

	/*
	 * here, we already now that the nodes are identical except their
	 * attributes
	 */
	if (a->op->ops.node_cmp_attr)
		return a->op->ops.node_cmp_attr(a, b);

	return 0;
}  /* identities_cmp */

/*
 * Calculate a hash value of a node.
 *
 * @param node  The IR-node
 */
unsigned ir_node_hash(const ir_node *node)
{
	return node->op->ops.hash(node);
}  /* ir_node_hash */


void new_identities(ir_graph *irg)
{
	if (irg->value_table != NULL)
		del_pset(irg->value_table);
	irg->value_table = new_pset(identities_cmp, N_IR_NODES);
}  /* new_identities */

void del_identities(ir_graph *irg)
{
	if (irg->value_table != NULL)
		del_pset(irg->value_table);
}  /* del_identities */

/* Normalize a node by putting constants (and operands with larger
 * node index) on the right (operator side). */
void ir_normalize_node(ir_node *n)
{
	if (is_op_commutative(get_irn_op(n))) {
		ir_node *l = get_binop_left(n);
		ir_node *r = get_binop_right(n);

		/* For commutative operators perform  a OP b == b OP a but keep
		 * constants on the RIGHT side. This helps greatly in some
		 * optimizations.  Moreover we use the idx number to make the form
		 * deterministic. */
		if (!operands_are_normalized(l, r)) {
			set_binop_left(n, r);
			set_binop_right(n, l);
			hook_normalize(n);
		}
	}
}  /* ir_normalize_node */

/*
 * Return the canonical node computing the same value as n.
 * Looks up the node in a hash table, enters it in the table
 * if it isn't there yet.
 *
 * @param n            the node to look up
 *
 * @return a node that computes the same value as n or n if no such
 *         node could be found
 */
ir_node *identify_remember(ir_node *n)
{
	ir_graph *irg         = get_irn_irg(n);
	pset     *value_table = irg->value_table;
	ir_node  *nn;

	if (value_table == NULL)
		return n;

	ir_normalize_node(n);
	/* lookup or insert in hash table with given hash key. */
	nn = pset_insert(value_table, n, ir_node_hash(n));

	if (nn != n) {
		/* n is reachable again */
		edges_node_revival(nn, get_irn_irg(nn));
	}

	return nn;
}  /* identify_remember */

/**
 * During construction we set the op_pin_state_pinned flag in the graph right
 * when the optimization is performed.  The flag turning on procedure global
 * cse could be changed between two allocations.  This way we are safe.
 *
 * @param n            The node to lookup
 */
static inline ir_node *identify_cons(ir_node *n)
{
	ir_node *old = n;

	n = identify_remember(n);
	if (n != old && get_nodes_block(old) != get_nodes_block(n)) {
		ir_graph *irg = get_irn_irg(n);
		set_irg_pinned(irg, op_pin_state_floats);
	}
	return n;
}  /* identify_cons */

/* Add a node to the identities value table. */
void add_identities(ir_node *node)
{
	if (!get_opt_cse())
		return;
	if (is_Block(node))
		return;

	identify_remember(node);
}

/* Visit each node in the value table of a graph. */
void visit_all_identities(ir_graph *irg, irg_walk_func visit, void *env)
{
	ir_node  *node;
	ir_graph *rem = current_ir_graph;

	current_ir_graph = irg;
	foreach_pset(irg->value_table, node) {
		visit(node, env);
	}
	current_ir_graph = rem;
}  /* visit_all_identities */

/**
 * Garbage in, garbage out. If a node has a dead input, i.e., the
 * Bad node is input to the node, return the Bad node.
 */
static ir_node *gigo(ir_node *node)
{
	int i, irn_arity;
	ir_op *op = get_irn_op(node);

	/* remove garbage blocks by looking at control flow that leaves the block
	   and replacing the control flow by Bad. */
	if (get_irn_mode(node) == mode_X) {
		ir_node  *block = get_nodes_block(skip_Proj(node));
		ir_graph *irg   = get_irn_irg(block);

		/* Don't optimize nodes in immature blocks. */
		if (!get_Block_matured(block))
			return node;
		/* Don't optimize End, may have Bads. */
		if (op == op_End) return node;

		if (is_Block(block)) {
			if (is_Block_dead(block)) {
				/* control flow from dead block is dead */
				return new_r_Bad(irg);
			}

			for (i = get_irn_arity(block) - 1; i >= 0; --i) {
				if (!is_Bad(get_irn_n(block, i)))
					break;
			}
			if (i < 0) {
				ir_graph *irg = get_irn_irg(block);
				/* the start block is never dead */
				if (block != get_irg_start_block(irg)
					&& block != get_irg_end_block(irg)) {
					/*
					 * Do NOT kill control flow without setting
					 * the block to dead of bad things can happen:
					 * We get a Block that is not reachable be irg_block_walk()
					 * but can be found by irg_walk()!
					 */
					set_Block_dead(block);
					return new_r_Bad(irg);
				}
			}
		}
	}

	/* Blocks, Phis and Tuples may have dead inputs, e.g., if one of the
	   blocks predecessors is dead. */
	if (op != op_Block && op != op_Phi && op != op_Tuple) {
		ir_graph *irg = get_irn_irg(node);
		irn_arity = get_irn_arity(node);

		/*
		 * Beware: we can only read the block of a non-floating node.
		 */
		if (is_irn_pinned_in_irg(node) &&
			is_Block_dead(get_nodes_block(skip_Proj(node))))
			return new_r_Bad(irg);

		for (i = 0; i < irn_arity; i++) {
			ir_node *pred = get_irn_n(node, i);

			if (is_Bad(pred))
				return new_r_Bad(irg);
#if 0
			/* Propagating Unknowns here seems to be a bad idea, because
			   sometimes we need a node as a input and did not want that
			   it kills it's user.
			   However, it might be useful to move this into a later phase
			   (if you think that optimizing such code is useful). */
			if (is_Unknown(pred) && mode_is_data(get_irn_mode(node)))
				return new_r_Unknown(irg, get_irn_mode(node));
#endif
		}
	}
#if 0
	/* With this code we violate the agreement that local_optimize
	   only leaves Bads in Block, Phi and Tuple nodes. */
	/* If Block has only Bads as predecessors it's garbage. */
	/* If Phi has only Bads as predecessors it's garbage. */
	if ((op == op_Block && get_Block_matured(node)) || op == op_Phi)  {
		irn_arity = get_irn_arity(node);
		for (i = 0; i < irn_arity; i++) {
			if (!is_Bad(get_irn_n(node, i))) break;
		}
		if (i == irn_arity) node = new_r_Bad(irg);
	}
#endif
	return node;
}  /* gigo */

/**
 * These optimizations deallocate nodes from the obstack.
 * It can only be called if it is guaranteed that no other nodes
 * reference this one, i.e., right after construction of a node.
 *
 * @param n   The node to optimize
 */
ir_node *optimize_node(ir_node *n)
{
	ir_node   *oldn = n;
	ir_graph  *irg  = get_irn_irg(n);
	ir_opcode  iro  = get_irn_opcode(n);
	ir_tarval *tv;

	/* Always optimize Phi nodes: part of the construction. */
	if ((!get_opt_optimize()) && (iro != iro_Phi)) return n;

	/* constant expression evaluation / constant folding */
	if (get_opt_constant_folding()) {
		/* neither constants nor Tuple values can be evaluated */
		if (iro != iro_Const && (get_irn_mode(n) != mode_T)) {
			/* try to evaluate */
			tv = computed_value(n);
			if (tv != tarval_bad) {
				ir_node *nw;
				int node_size;

				/*
				 * we MUST copy the node here temporary, because it's still needed
				 * for DBG_OPT_CSTEVAL
				 */
				node_size = offsetof(ir_node, attr) +  n->op->attr_size;
				oldn = alloca(node_size);

				memcpy(oldn, n, node_size);
				CLONE_ARR_A(ir_node *, oldn->in, n->in);

				/* ARG, copy the in array, we need it for statistics */
				memcpy(oldn->in, n->in, ARR_LEN(n->in) * sizeof(n->in[0]));

				/* note the inplace edges module */
				edges_node_deleted(n, irg);

				/* evaluation was successful -- replace the node. */
				irg_kill_node(irg, n);
				nw = new_r_Const(irg, tv);

				DBG_OPT_CSTEVAL(oldn, nw);
				return nw;
			}
		}
	}

	/* remove unnecessary nodes */
	if (get_opt_algebraic_simplification() ||
	    (iro == iro_Phi)  ||   /* always optimize these nodes. */
	    (iro == iro_Id)   ||
	    (iro == iro_Proj) ||
	    (iro == iro_Block)  )  /* Flags tested local. */
		n = equivalent_node(n);

	/* Common Subexpression Elimination.
	 *
	 * Checks whether n is already available.
	 * The block input is used to distinguish different subexpressions. Right
	 * now all nodes are op_pin_state_pinned to blocks, i.e., the CSE only finds common
	 * subexpressions within a block.
	 */
	if (get_opt_cse())
		n = identify_cons(n);

	if (n != oldn) {
		edges_node_deleted(oldn, irg);

		/* We found an existing, better node, so we can deallocate the old node. */
		irg_kill_node(irg, oldn);
		return n;
	}

	/* Some more constant expression evaluation that does not allow to
	   free the node. */
	iro = get_irn_opcode(n);
	if (get_opt_algebraic_simplification() ||
	    (iro == iro_Cond) ||
	    (iro == iro_Proj))     /* Flags tested local. */
		n = transform_node(n);

	/* Remove nodes with dead (Bad) input.
	   Run always for transformation induced Bads. */
	n = gigo(n);

	/* Now we have a legal, useful node. Enter it in hash table for CSE */
	if (get_opt_cse() && (get_irn_opcode(n) != iro_Block)) {
		ir_node *o = n;
		n = identify_remember(o);
		if (o != n)
			DBG_OPT_CSE(o, n);
	}

	return n;
}  /* optimize_node */


/**
 * These optimizations never deallocate nodes (in place).  This can cause dead
 * nodes lying on the obstack.  Remove these by a dead node elimination,
 * i.e., a copying garbage collection.
 */
ir_node *optimize_in_place_2(ir_node *n)
{
	ir_tarval *tv;
	ir_node   *oldn = n;
	ir_opcode  iro  = get_irn_opcode(n);

	if (!get_opt_optimize() && !is_Phi(n)) return n;

	if (iro == iro_Deleted)
		return n;

	/* constant expression evaluation / constant folding */
	if (get_opt_constant_folding()) {
		/* neither constants nor Tuple values can be evaluated */
		if (iro != iro_Const && get_irn_mode(n) != mode_T) {
			/* try to evaluate */
			tv = computed_value(n);
			if (tv != tarval_bad) {
				/* evaluation was successful -- replace the node. */
				ir_graph *irg = get_irn_irg(n);

				n = new_r_Const(irg, tv);

				DBG_OPT_CSTEVAL(oldn, n);
				return n;
			}
		}
	}

	/* remove unnecessary nodes */
	if (get_opt_constant_folding() ||
	    (iro == iro_Phi)  ||   /* always optimize these nodes. */
	    (iro == iro_Id)   ||   /* ... */
	    (iro == iro_Proj) ||   /* ... */
	    (iro == iro_Block)  )  /* Flags tested local. */
		n = equivalent_node(n);

	/** common subexpression elimination **/
	/* Checks whether n is already available. */
	/* The block input is used to distinguish different subexpressions.  Right
	   now all nodes are op_pin_state_pinned to blocks, i.e., the cse only finds common
	   subexpressions within a block. */
	if (get_opt_cse()) {
		ir_node *o = n;
		n = identify_remember(o);
		if (o != n)
			DBG_OPT_CSE(o, n);
	}

	/* Some more constant expression evaluation. */
	iro = get_irn_opcode(n);
	if (get_opt_constant_folding() ||
		(iro == iro_Cond) ||
		(iro == iro_Proj))     /* Flags tested local. */
		n = transform_node(n);

	/* Remove nodes with dead (Bad) input.
	   Run always for transformation induced Bads.  */
	n = gigo(n);

	/* Now we can verify the node, as it has no dead inputs any more. */
	irn_verify(n);

	/* Now we have a legal, useful node. Enter it in hash table for cse.
	   Blocks should be unique anyways.  (Except the successor of start:
	   is cse with the start block!) */
	if (get_opt_cse() && (get_irn_opcode(n) != iro_Block)) {
		ir_node *o = n;
		n = identify_remember(o);
		if (o != n)
			DBG_OPT_CSE(o, n);
	}

	return n;
}  /* optimize_in_place_2 */

/**
 * Wrapper for external use, set proper status bits after optimization.
 */
ir_node *optimize_in_place(ir_node *n)
{
	ir_graph *irg = get_irn_irg(n);
	/* Handle graph state */
	assert(get_irg_phase_state(irg) != phase_building);

	if (get_opt_global_cse())
		set_irg_pinned(irg, op_pin_state_floats);
	if (get_irg_outs_state(irg) == outs_consistent)
		set_irg_outs_inconsistent(irg);

	/* FIXME: Maybe we could also test whether optimizing the node can
	   change the control graph. */
	set_irg_doms_inconsistent(irg);
	return optimize_in_place_2(n);
}  /* optimize_in_place */

/**
 * Calculate a hash value of a Const node.
 */
static unsigned hash_Const(const ir_node *node)
{
	unsigned h;

	/* special value for const, as they only differ in their tarval. */
	h = HASH_PTR(node->attr.con.tarval);

	return h;
}  /* hash_Const */

/**
 * Calculate a hash value of a SymConst node.
 */
static unsigned hash_SymConst(const ir_node *node)
{
	unsigned h;

	/* all others are pointers */
	h = HASH_PTR(node->attr.symc.sym.type_p);

	return h;
}  /* hash_SymConst */

/**
 * Set the default hash operation in an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
static ir_op_ops *firm_set_default_hash(ir_opcode code, ir_op_ops *ops)
{
#define CASE(a)                                    \
	case iro_##a:                                  \
		ops->hash  = hash_##a; \
		break

	/* hash function already set */
	if (ops->hash != NULL)
		return ops;

	switch (code) {
	CASE(Const);
	CASE(SymConst);
	default:
		/* use input/mode default hash if no function was given */
		ops->hash = firm_default_hash;
	}

	return ops;
#undef CASE
}

/*
 * Sets the default operation for an ir_ops.
 */
ir_op_ops *firm_set_default_operations(ir_opcode code, ir_op_ops *ops)
{
	ops = firm_set_default_hash(code, ops);
	ops = firm_set_default_computed_value(code, ops);
	ops = firm_set_default_equivalent_node(code, ops);
	ops = firm_set_default_transform_node(code, ops);
	ops = firm_set_default_node_cmp_attr(code, ops);
	ops = firm_set_default_get_type_attr(code, ops);
	ops = firm_set_default_get_entity_attr(code, ops);

	return ops;
}  /* firm_set_default_operations */
