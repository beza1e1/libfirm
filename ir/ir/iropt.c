/*
 * Project:     libFIRM
 * File name:   ir/ir/iropt.c
 * Purpose:     iropt --- optimizations intertwined with IR construction.
 * Author:      Christian Schaefer
 * Modified by: Goetz Lindenmaier, Michael Beck
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1998-2005 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "irnode_t.h"
#include "irgraph_t.h"
#include "iredges_t.h"
#include "irmode_t.h"
#include "iropt_t.h"
#include "ircons_t.h"
#include "irgmod.h"
#include "irvrfy.h"
#include "tv_t.h"
#include "dbginfo_t.h"
#include "iropt_dbg.h"
#include "irflag_t.h"
#include "irhooks.h"
#include "irarch.h"
#include "hashptr.h"
#include "archop.h"
#include "opt_polymorphy.h"
#include "opt_confirms.h"

/* Make types visible to allow most efficient access */
# include "entity_t.h"

/**
 * return the value of a Constant
 */
static tarval *computed_value_Const(ir_node *n)
{
  return get_Const_tarval(n);
}

/**
 * return the value of a 'sizeof' SymConst
 */
static tarval *computed_value_SymConst(ir_node *n)
{
  if ((get_SymConst_kind(n) == symconst_size) &&
      (get_type_state(get_SymConst_type(n))) == layout_fixed)
    return new_tarval_from_long(get_type_size_bytes(get_SymConst_type(n)), get_irn_mode(n));
  return tarval_bad;
}

/**
 * return the value of an Add
 */
static tarval *computed_value_Add(ir_node *n)
{
  ir_node *a = get_Add_left(n);
  ir_node *b = get_Add_right(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad) && (get_irn_mode(a) == get_irn_mode(b)))
    return tarval_add(ta, tb);

  return tarval_bad;
}

/**
 * return the value of a Sub
 * Special case: a - a
 */
static tarval *computed_value_Sub(ir_node *n)
{
  ir_node *a = get_Sub_left(n);
  ir_node *b = get_Sub_right(n);
  tarval *ta;
  tarval *tb;

  /* a - a */
  if (a == b && !is_Bad(a))
    return get_mode_null(get_irn_mode(n));

  ta = value_of(a);
  tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad) && (get_irn_mode(a) == get_irn_mode(b)))
    return tarval_sub(ta, tb);

  return tarval_bad;
}

/**
 * return the value of a Carry
 * Special : a op 0, 0 op b
 */
static tarval *computed_value_Carry(ir_node *n)
{
  ir_node *a = get_binop_left(n);
  ir_node *b = get_binop_right(n);
  ir_mode *m = get_irn_mode(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad)) {
    tarval_add(ta, tb);
    return tarval_carry() ? get_mode_one(m) : get_mode_null(m);
  } else {
    if (   (classify_tarval(ta) == TV_CLASSIFY_NULL)
        || (classify_tarval(tb) == TV_CLASSIFY_NULL))
      return get_mode_null(m);
  }
  return tarval_bad;
}

/**
 * return the value of a Borrow
 * Special : a op 0
 */
static tarval *computed_value_Borrow(ir_node *n)
{
  ir_node *a = get_binop_left(n);
  ir_node *b = get_binop_right(n);
  ir_mode *m = get_irn_mode(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad)) {
    return tarval_cmp(ta, tb) == pn_Cmp_Lt ? get_mode_one(m) : get_mode_null(m);
  } else if (classify_tarval(ta) == TV_CLASSIFY_NULL) {
      return get_mode_null(m);
  }
  return tarval_bad;
}

/**
 * return the value of an unary Minus
 */
static tarval *computed_value_Minus(ir_node *n)
{
  ir_node *a = get_Minus_op(n);
  tarval *ta = value_of(a);

  if ((ta != tarval_bad) && mode_is_signed(get_irn_mode(a)))
    return tarval_neg(ta);

  return tarval_bad;
}

/**
 * return the value of a Mul
 */
static tarval *computed_value_Mul(ir_node *n)
{
  ir_node *a = get_Mul_left(n);
  ir_node *b = get_Mul_right(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad) && (get_irn_mode(a) == get_irn_mode(b))) {
    return tarval_mul(ta, tb);
  } else {
    /* a*0 = 0 or 0*b = 0:
       calls computed_value recursive and returns the 0 with proper
       mode. */
    if ((ta != tarval_bad) && (ta == get_mode_null(get_tarval_mode(ta))))
      return ta;
    if ((tb != tarval_bad) && (tb == get_mode_null(get_tarval_mode(tb))))
      return tb;
  }
  return tarval_bad;
}

/**
 * return the value of a floating point Quot
 */
static tarval *computed_value_Quot(ir_node *n)
{
  ir_node *a = get_Quot_left(n);
  ir_node *b = get_Quot_right(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  /* This was missing in original implementation. Why? */
  if ((ta != tarval_bad) && (tb != tarval_bad) && (get_irn_mode(a) == get_irn_mode(b))) {
    if (tb != get_mode_null(get_tarval_mode(tb)))   /* div by zero: return tarval_bad */
      return tarval_quo(ta, tb);
  }
  return tarval_bad;
}

/**
 * calculate the value of an integer Div of two nodes
 * Special case: 0 / b
 */
static tarval *do_computed_value_Div(ir_node *a, ir_node *b)
{
  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  /* Compute c1 / c2 or 0 / a, a != 0 */
  if (ta != tarval_bad) {
    if ((tb != tarval_bad) && (tb != get_mode_null(get_irn_mode(b))))   /* div by zero: return tarval_bad */
      return tarval_div(ta, tb);
    else if (ta == get_mode_null(get_tarval_mode(ta)))  /* 0 / b == 0 */
      return ta;
  }
  return tarval_bad;
}

/**
 * return the value of an integer Div
 */
static tarval *computed_value_Div(ir_node *n)
{
  return do_computed_value_Div(get_Div_left(n), get_Div_right(n));
}

/**
 * calculate the value of an integer Mod of two nodes
 * Special case: a % 1
 */
static tarval *do_computed_value_Mod(ir_node *a, ir_node *b)
{
  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  /* Compute c1 % c2 or a % 1 */
  if (tb != tarval_bad) {
    if ((ta != tarval_bad) && (tb != get_mode_null(get_tarval_mode(tb))))   /* div by zero: return tarval_bad */
      return tarval_mod(ta, tb);
    else if (tb == get_mode_one(get_tarval_mode(tb)))    /* x mod 1 == 0 */
      return get_mode_null(get_irn_mode(a));
  }

  return tarval_bad;
}

/**
 * return the value of an integer Mod
 */
static tarval *computed_value_Mod(ir_node *n)
{
  return do_computed_value_Mod(get_Mod_left(n), get_Mod_right(n));
}

/**
 * return the value of an Abs
 */
static tarval *computed_value_Abs(ir_node *n)
{
  ir_node *a = get_Abs_op(n);
  tarval *ta = value_of(a);

  if (ta != tarval_bad)
    return tarval_abs(ta);

  return tarval_bad;
}

/**
 * return the value of an And
 * Special case: a & 0, 0 & b
 */
static tarval *computed_value_And(ir_node *n)
{
  ir_node *a = get_And_left(n);
  ir_node *b = get_And_right(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad)) {
    return tarval_and (ta, tb);
  } else {
    tarval *v;

    if (   (classify_tarval ((v = ta)) == TV_CLASSIFY_NULL)
        || (classify_tarval ((v = tb)) == TV_CLASSIFY_NULL)) {
      return v;
    }
  }
  return tarval_bad;
}

/**
 * return the value of an Or
 * Special case: a | 1...1, 1...1 | b
 */
static tarval *computed_value_Or(ir_node *n)
{
  ir_node *a = get_Or_left(n);
  ir_node *b = get_Or_right(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad)) {
    return tarval_or (ta, tb);
  } else {
    tarval *v;
    if (   (classify_tarval ((v = ta)) == TV_CLASSIFY_ALL_ONE)
        || (classify_tarval ((v = tb)) == TV_CLASSIFY_ALL_ONE)) {
      return v;
    }
  }
  return tarval_bad;
}

/**
 * return the value of an Eor
 */
static tarval *computed_value_Eor(ir_node *n)
{
  ir_node *a = get_Eor_left(n);
  ir_node *b = get_Eor_right(n);

  tarval *ta, *tb;

  if (a == b)
    return get_mode_null(get_irn_mode(n));

  ta = value_of(a);
  tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad)) {
    return tarval_eor (ta, tb);
  }
  return tarval_bad;
}

/**
 * return the value of a Not
 */
static tarval *computed_value_Not(ir_node *n)
{
  ir_node *a = get_Not_op(n);
  tarval *ta = value_of(a);

  if (ta != tarval_bad)
    return tarval_not(ta);

  return tarval_bad;
}

/**
 * return the value of a Shl
 */
static tarval *computed_value_Shl(ir_node *n)
{
  ir_node *a = get_Shl_left(n);
  ir_node *b = get_Shl_right(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad)) {
    return tarval_shl (ta, tb);
  }
  return tarval_bad;
}

/**
 * return the value of a Shr
 */
static tarval *computed_value_Shr(ir_node *n)
{
  ir_node *a = get_Shr_left(n);
  ir_node *b = get_Shr_right(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad)) {
    return tarval_shr (ta, tb);
  }
  return tarval_bad;
}

/**
 * return the value of a Shrs
 */
static tarval *computed_value_Shrs(ir_node *n)
{
  ir_node *a = get_Shrs_left(n);
  ir_node *b = get_Shrs_right(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad)) {
    return tarval_shrs (ta, tb);
  }
  return tarval_bad;
}

/**
 * return the value of a Rot
 */
static tarval *computed_value_Rot(ir_node *n)
{
  ir_node *a = get_Rot_left(n);
  ir_node *b = get_Rot_right(n);

  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if ((ta != tarval_bad) && (tb != tarval_bad)) {
    return tarval_rot (ta, tb);
  }
  return tarval_bad;
}

/**
 * return the value of a Conv
 */
static tarval *computed_value_Conv(ir_node *n)
{
  ir_node *a = get_Conv_op(n);
  tarval *ta = value_of(a);

  if (ta != tarval_bad)
    return tarval_convert_to(ta, get_irn_mode(n));

  return tarval_bad;
}

/**
 * return the value of a Proj(Cmp)
 *
 * This performs a first step of unreachable code elimination.
 * Proj can not be computed, but folding a Cmp above the Proj here is
 * not as wasteful as folding a Cmp into a Tuple of 16 Consts of which
 * only 1 is used.
 * There are several case where we can evaluate a Cmp node, see later.
 */
static tarval *computed_value_Proj_Cmp(ir_node *n)
{
  ir_node *a   = get_Proj_pred(n);
  ir_node *aa  = get_Cmp_left(a);
  ir_node *ab  = get_Cmp_right(a);
  long proj_nr = get_Proj_proj(n);

  /*
   * BEWARE: a == a is NOT always True for floating Point values, as
   * NaN != NaN is defined, so we must check this here.
   */
  if (aa == ab && (
      !mode_is_float(get_irn_mode(aa)) || proj_nr == pn_Cmp_Lt ||  proj_nr == pn_Cmp_Gt)
      ) { /* 1.: */

    /* This is a trick with the bits used for encoding the Cmp
       Proj numbers, the following statement is not the same:
    return new_tarval_from_long (proj_nr == pn_Cmp_Eq, mode_b) */
    return new_tarval_from_long (proj_nr & pn_Cmp_Eq, mode_b);
  }
  else {
    tarval *taa = value_of(aa);
    tarval *tab = value_of(ab);
    ir_mode *mode = get_irn_mode(aa);

    /*
     * The predecessors of Cmp are target values.  We can evaluate
     * the Cmp.
     */
    if ((taa != tarval_bad) && (tab != tarval_bad)) {
      /* strange checks... */
      pn_Cmp flags = tarval_cmp(taa, tab);
      if (flags != pn_Cmp_False) {
        return new_tarval_from_long (proj_nr & flags, mode_b);
      }
    }
    /* for integer values, we can check against MIN/MAX */
    else if (mode_is_int(mode)) {
      /* MIN <=/> x.  This results in true/false. */
      if (taa == get_mode_min(mode)) {
        /* a compare with the MIN value */
        if (proj_nr == pn_Cmp_Le)
          return get_tarval_b_true();
        else if (proj_nr == pn_Cmp_Gt)
          return get_tarval_b_false();
      }
      /* x >=/< MIN.  This results in true/false. */
      else
      if (tab == get_mode_min(mode)) {
        /* a compare with the MIN value */
        if (proj_nr == pn_Cmp_Ge)
          return get_tarval_b_true();
        else if (proj_nr == pn_Cmp_Lt)
          return get_tarval_b_false();
      }
      /* MAX >=/< x.  This results in true/false. */
      else if (taa == get_mode_max(mode)) {
        if (proj_nr == pn_Cmp_Ge)
          return get_tarval_b_true();
        else if (proj_nr == pn_Cmp_Lt)
          return get_tarval_b_false();
      }
      /* x <=/> MAX.  This results in true/false. */
      else if (tab == get_mode_max(mode)) {
        if (proj_nr == pn_Cmp_Le)
          return get_tarval_b_true();
        else if (proj_nr == pn_Cmp_Gt)
          return get_tarval_b_false();
      }
    }
    /*
     * The predecessors are Allocs or (void*)(0) constants.  Allocs never
     * return NULL, they raise an exception.   Therefore we can predict
     * the Cmp result.
     */
    else {
      ir_node *aaa = skip_Id(skip_Proj(aa));
      ir_node *aba = skip_Id(skip_Proj(ab));

      if (   (   (/* aa is ProjP and aaa is Alloc */
                     (get_irn_op(aa) == op_Proj)
                  && (mode_is_reference(get_irn_mode(aa)))
                  && (get_irn_op(aaa) == op_Alloc))
              && (   (/* ab is NULL */
                         (get_irn_op(ab) == op_Const)
                      && (mode_is_reference(get_irn_mode(ab)))
                      && (get_Const_tarval(ab) == get_mode_null(get_irn_mode(ab))))
                  || (/* ab is other Alloc */
                         (get_irn_op(ab) == op_Proj)
                      && (mode_is_reference(get_irn_mode(ab)))
                      && (get_irn_op(aba) == op_Alloc)
                      && (aaa != aba))))
          || (/* aa is NULL and aba is Alloc */
                 (get_irn_op(aa) == op_Const)
              && (mode_is_reference(get_irn_mode(aa)))
              && (get_Const_tarval(aa) == get_mode_null(get_irn_mode(aa)))
              && (get_irn_op(ab) == op_Proj)
              && (mode_is_reference(get_irn_mode(ab)))
              && (get_irn_op(aba) == op_Alloc)))
        /* 3.: */
        return new_tarval_from_long(proj_nr & pn_Cmp_Ne, mode_b);
    }
  }

  return computed_value_Cmp_Confirm(a, aa, ab, proj_nr);
}

/**
 * return the value of a Proj, handle Proj(Cmp), Proj(Div), Proj(Mod), Proj(DivMod)
 */
static tarval *computed_value_Proj(ir_node *n)
{
  ir_node *a = get_Proj_pred(n);
  long proj_nr;

  switch (get_irn_opcode(a)) {
  case iro_Cmp:
    return computed_value_Proj_Cmp(n);

  case iro_DivMod:
    /* compute either the Div or the Mod part */
    proj_nr = get_Proj_proj(n);
    if (proj_nr == pn_DivMod_res_div)
      return do_computed_value_Div(get_DivMod_left(a), get_DivMod_right(a));
    else if (proj_nr == pn_DivMod_res_mod)
      return do_computed_value_Mod(get_DivMod_left(a), get_DivMod_right(a));
    break;

  case iro_Div:
    if (get_Proj_proj(n) == pn_Div_res)
      return computed_value(a);
    break;

  case iro_Mod:
    if (get_Proj_proj(n) == pn_Mod_res)
      return computed_value(a);
    break;

  default:
    return tarval_bad;
  }
  return tarval_bad;
}

/**
 * calculate the value of a Mux: can be evaluated, if the
 * sel and the right input are known
 */
static tarval *computed_value_Mux(ir_node *n)
{
  ir_node *sel = get_Mux_sel(n);
  tarval *ts = value_of(sel);

  if (ts == get_tarval_b_true()) {
    ir_node *v = get_Mux_true(n);
    return value_of(v);
  }
  else if (ts == get_tarval_b_false()) {
    ir_node *v = get_Mux_false(n);
    return value_of(v);
  }
  return tarval_bad;
}

/**
 * Calculate the value of a Psi: can be evaluated, if a condition is true
 * and all previous conditions are false. If all conditions are false
 * we evaluate to the default one.
 */
static tarval *computed_value_Psi(ir_node *n)
{
  return tarval_bad;
}

/**
 * calculate the value of a Confirm: can be evaluated,
 * if it has the form Confirm(x, '=', Const).
 */
static tarval *computed_value_Confirm(ir_node *n)
{
  return get_Confirm_cmp(n) == pn_Cmp_Eq ?
    value_of(get_Confirm_bound(n)) : tarval_bad;
}

/**
 * If the parameter n can be computed, return its value, else tarval_bad.
 * Performs constant folding.
 *
 * @param n  The node this should be evaluated
 */
tarval *computed_value(ir_node *n)
{
  if (n->op->ops.computed_value)
    return n->op->ops.computed_value(n);
  return tarval_bad;
}

/**
 * set the default computed_value evaluator in an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
static ir_op_ops *firm_set_default_computed_value(opcode code, ir_op_ops *ops)
{
#define CASE(a)                               \
  case iro_##a:                               \
    ops->computed_value  = computed_value_##a; \
    break

  switch (code) {
  CASE(Const);
  CASE(SymConst);
  CASE(Add);
  CASE(Sub);
  CASE(Minus);
  CASE(Mul);
  CASE(Quot);
  CASE(Div);
  CASE(Mod);
  CASE(Abs);
  CASE(And);
  CASE(Or);
  CASE(Eor);
  CASE(Not);
  CASE(Shl);
  CASE(Shr);
  CASE(Shrs);
  CASE(Rot);
  CASE(Carry);
  CASE(Borrow);
  CASE(Conv);
  CASE(Proj);
  CASE(Mux);
  CASE(Psi);
  CASE(Confirm);
  default:
    /* leave NULL */;
  }

  return ops;
#undef CASE
}

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
  int n_preds   = get_Block_n_cfgpreds(n);

  /* The Block constructor does not call optimize, but mature_immBlock
     calls the optimization. */
  assert(get_Block_matured(n));

  /* Straightening: a single entry Block following a single exit Block
     can be merged, if it is not the Start block. */
  /* !!! Beware, all Phi-nodes of n must have been optimized away.
     This should be true, as the block is matured before optimize is called.
     But what about Phi-cycles with the Phi0/Id that could not be resolved?
     Remaining Phi nodes are just Ids. */
   if ((n_preds == 1) && (get_irn_op(get_Block_cfgpred(n, 0)) == op_Jmp)) {
     ir_node *predblock = get_nodes_block(get_Block_cfgpred(n, 0));
     if (predblock == oldn) {
       /* Jmp jumps into the block it is in -- deal self cycle. */
       n = set_Block_dead(n);
       DBG_OPT_DEAD_BLOCK(oldn, n);
     } else if (get_opt_control_flow_straightening()) {
       n = predblock;
       DBG_OPT_STG(oldn, n);
     }
   }
   else if ((n_preds == 1) &&
            (get_irn_op(skip_Proj(get_Block_cfgpred(n, 0))) == op_Cond)) {
     ir_node *predblock = get_Block_cfgpred_block(n, 0);
     if (predblock == oldn) {
       /* Jmp jumps into the block it is in -- deal self cycle. */
       n = set_Block_dead(n);
       DBG_OPT_DEAD_BLOCK(oldn, n);
     }
   }
   else if ((n_preds == 2) &&
            (get_opt_control_flow_weak_simplification())) {
    /* Test whether Cond jumps twice to this block
     * The more general case which more than 2 predecessors is handles
     * in optimize_cf(), we handle only this special case for speed here.
     */
    ir_node *a = get_Block_cfgpred(n, 0);
    ir_node *b = get_Block_cfgpred(n, 1);

    if ((get_irn_op(a) == op_Proj) &&
        (get_irn_op(b) == op_Proj) &&
        (get_Proj_pred(a) == get_Proj_pred(b)) &&
        (get_irn_op(get_Proj_pred(a)) == op_Cond) &&
        (get_irn_mode(get_Cond_selector(get_Proj_pred(a))) == mode_b)) {
      /* Also a single entry Block following a single exit Block.  Phis have
         twice the same operand and will be optimized away. */
      n = get_nodes_block(get_Proj_pred(a));
      DBG_OPT_IFSIM1(oldn, a, b, n);
    }
  }
  else if (get_opt_unreachable_code() &&
           (n != get_irg_start_block(current_ir_graph)) &&
           (n != get_irg_end_block(current_ir_graph))    ) {
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
}

/**
 * Returns a equivalent node for a Jmp, a Bad :-)
 * Of course this only happens if the Block of the Jmp is Bad.
 */
static ir_node *equivalent_node_Jmp(ir_node *n)
{
  /* unreachable code elimination */
  if (is_Block_dead(get_nodes_block(n)))
    n = new_Bad();

  return n;
}

/* Same for op_Raise */
#define equivalent_node_Raise   equivalent_node_Jmp


/* We do not evaluate Cond here as we replace it by a new node, a Jmp.
   See transform_node_Proj_Cond(). */

/**
 * optimize operations that are commutative and have neutral 0,
 * so a op 0 = 0 op a = a.
 */
static ir_node *equivalent_node_neutral_zero(ir_node *n)
{
  ir_node *oldn = n;

  ir_node *a = get_binop_left(n);
  ir_node *b = get_binop_right(n);

  tarval *tv;
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
  if (classify_tarval (tv) == TV_CLASSIFY_NULL) {
    if (get_irn_mode(on) == get_irn_mode(n)) {
      n = on;

      DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_0);
    }
  }

  return n;
}

#define equivalent_node_Eor  equivalent_node_neutral_zero

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

  n = equivalent_node_neutral_zero(n);
  if (n != oldn)
    return n;

  left  = get_Add_left(n);
  right = get_Add_right(n);

  if (get_irn_op(left) == op_Sub) {
    if (get_Sub_right(left) == right) {
      /* (a - x) + x */

      n = get_Sub_left(left);
      if (get_irn_mode(oldn) == get_irn_mode(n)) {
        DBG_OPT_ALGSIM1(oldn, left, right, n, FS_OPT_ADD_SUB);
        return n;
      }
    }
  }
  if (get_irn_op(right) == op_Sub) {
    if (get_Sub_right(right) == left) {
      /* x + (a - x) */

      n = get_Sub_left(right);
      if (get_irn_mode(oldn) == get_irn_mode(n)) {
        DBG_OPT_ALGSIM1(oldn, left, right, n, FS_OPT_ADD_SUB);
        return n;
      }
    }
  }
  return n;
}

/**
 * optimize operations that are not commutative but have neutral 0 on left,
 * so a op 0 = a.
 */
static ir_node *equivalent_node_left_zero(ir_node *n)
{
  ir_node *oldn = n;

  ir_node *a = get_binop_left(n);
  ir_node *b = get_binop_right(n);

  if (classify_tarval(value_of(b)) == TV_CLASSIFY_NULL) {
    n = a;

    DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_0);
  }

  return n;
}

#define equivalent_node_Shl   equivalent_node_left_zero
#define equivalent_node_Shr   equivalent_node_left_zero
#define equivalent_node_Shrs  equivalent_node_left_zero
#define equivalent_node_Rot   equivalent_node_left_zero

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
  ir_node *oldn = n;

  ir_node *a = get_Sub_left(n);
  ir_node *b = get_Sub_right(n);

  /* Beware: modes might be different */
  if (classify_tarval(value_of(b)) == TV_CLASSIFY_NULL) {
    if (get_irn_mode(n) == get_irn_mode(a)) {
      n = a;

      DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_0);
    }
  }
  else if (get_irn_op(a) == op_Add) {
    ir_mode *mode = get_irn_mode(n);

    if (mode_wrap_around(mode)) {
      ir_node *left  = get_Add_left(a);
      ir_node *right = get_Add_right(a);

      if (left == b) {
        if (get_irn_mode(n) == get_irn_mode(right)) {
          n = right;
          DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_ADD_SUB);
        }
      }
      else if (right == b) {
        if (get_irn_mode(n) == get_irn_mode(left)) {
          n = left;
          DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_ADD_SUB);
        }
      }
    }
  }

  return n;
}


/**
 * Optimize an "idempotent unary op", ie op(op(n)) = n.
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
    DBG_OPT_ALGSIM2(oldn, pred, n);
  }
  return n;
}

/* Not(Not(x)) == x */
#define equivalent_node_Not    equivalent_node_idempotent_unop

/* --x == x */  /* ??? Is this possible or can --x raise an
                       out of bounds exception if min =! max? */
#define equivalent_node_Minus  equivalent_node_idempotent_unop

/**
 * Optimize a * 1 = 1 * a = a.
 */
static ir_node *equivalent_node_Mul(ir_node *n)
{
  ir_node *oldn = n;

  ir_node *a = get_Mul_left(n);
  ir_node *b = get_Mul_right(n);

  /* Mul is commutative and has again an other neutral element. */
  if (classify_tarval(value_of(a)) == TV_CLASSIFY_ONE) {
    n = b;
    DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_1);
  } else if (classify_tarval(value_of(b)) == TV_CLASSIFY_ONE) {
    n = a;
    DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_NEUTRAL_1);
  }
  return n;
}

/**
 * Optimize a / 1 = a.
 */
static ir_node *equivalent_node_Div(ir_node *n)
{
  ir_node *a = get_Div_left(n);
  ir_node *b = get_Div_right(n);

  /* Div is not commutative. */
  if (classify_tarval(value_of(b)) == TV_CLASSIFY_ONE) { /* div(x, 1) == x */
    /* Turn Div into a tuple (mem, bad, a) */
    ir_node *mem = get_Div_mem(n);
    turn_into_tuple(n, pn_Div_max);
    set_Tuple_pred(n, pn_Div_M,        mem);
    set_Tuple_pred(n, pn_Div_X_except, new_Bad());        /* no exception */
    set_Tuple_pred(n, pn_Div_res,      a);
  }
  return n;
}

/**
 * Optimize a / 1 = a.
 */
static ir_node *equivalent_node_DivMod(ir_node *n)
{
  ir_node *a = get_DivMod_left(n);
  ir_node *b = get_DivMod_right(n);

  /* Div is not commutative. */
  if (classify_tarval(value_of(b)) == TV_CLASSIFY_ONE) { /* div(x, 1) == x */
    /* Turn DivMod into a tuple (mem, bad, a, 0) */
    ir_node *mem = get_Div_mem(n);
    ir_mode *mode = get_irn_mode(b);

    turn_into_tuple(n, pn_DivMod_max);
    set_Tuple_pred(n, pn_DivMod_M,        mem);
    set_Tuple_pred(n, pn_DivMod_X_except, new_Bad());        /* no exception */
    set_Tuple_pred(n, pn_DivMod_res_div,  a);
    set_Tuple_pred(n, pn_DivMod_res_mod,  new_Const(mode, get_mode_null(mode)));
  }
  return n;
}

/**
 * Use algebraic simplification a | a = a | 0 = 0 | a = a.
 */
static ir_node *equivalent_node_Or(ir_node *n)
{
  ir_node *oldn = n;

  ir_node *a = get_Or_left(n);
  ir_node *b = get_Or_right(n);

  if (a == b) {
    n = a;    /* Or has it's own neutral element */
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_OR);
  } else if (classify_tarval(value_of(a)) == TV_CLASSIFY_NULL) {
    n = b;
    DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_OR);
  } else if (classify_tarval(value_of(b)) == TV_CLASSIFY_NULL) {
    n = a;
    DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_OR);
  }

  return n;
}

/**
 * Optimize a & 0b1...1 = 0b1...1 & a =  a & a = a.
 */
static ir_node *equivalent_node_And(ir_node *n)
{
  ir_node *oldn = n;

  ir_node *a = get_And_left(n);
  ir_node *b = get_And_right(n);

  if (a == b) {
    n = a;    /* And has it's own neutral element */
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_AND);
  } else if (classify_tarval(value_of(a)) == TV_CLASSIFY_ALL_ONE) {
    n = b;
    DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_AND);
  } else if (classify_tarval(value_of(b)) == TV_CLASSIFY_ALL_ONE) {
    n = a;
    DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_AND);
  }
  return n;
}

/**
 * Try to remove useless Conv's:
 */
static ir_node *equivalent_node_Conv(ir_node *n)
{
  ir_node *oldn = n;
  ir_node *a = get_Conv_op(n);
  ir_node *b;

  ir_mode *n_mode = get_irn_mode(n);
  ir_mode *a_mode = get_irn_mode(a);

  if (n_mode == a_mode) { /* No Conv necessary */
    n = a;
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_CONV);
  } else if (get_irn_op(a) == op_Conv) { /* Conv(Conv(b)) */
    ir_mode *b_mode;

    b = get_Conv_op(a);
    n_mode = get_irn_mode(n);
    b_mode = get_irn_mode(b);

    if (n_mode == b_mode) {
      if (n_mode == mode_b) {
        n = b; /* Convb(Conv*(xxxb(...))) == xxxb(...) */
        DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_CONV);
      }
      else if (mode_is_int(n_mode) || mode_is_character(n_mode)) {
        if (smaller_mode(b_mode, a_mode)){
          n = b;        /* ConvS(ConvL(xxxS(...))) == xxxS(...) */
          DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_CONV);
        }
      }
    }
  }
  return n;
}

/**
 * A Cast may be removed if the type of the previous node
 * is already the type of the Cast.
 */
static ir_node *equivalent_node_Cast(ir_node *n) {
  ir_node *oldn = n;
  ir_node *pred = get_Cast_op(n);

  if (get_irn_type(pred) == get_Cast_type(n)) {
    n = pred;
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_CAST);
  }
  return n;
}

/**
  Several optimizations:
   - no Phi in start block.
   - remove Id operators that are inputs to Phi
   - fold Phi-nodes, iff they have only one predecessor except
           themselves.
 */
static ir_node *equivalent_node_Phi(ir_node *n)
{
  int i, n_preds;

  ir_node *oldn = n;
  ir_node *block = NULL;     /* to shutup gcc */
  ir_node *first_val = NULL; /* to shutup gcc */

  if (!get_opt_normalize()) return n;

  n_preds = get_Phi_n_preds(n);

  block = get_nodes_block(n);
  /* @@@ fliegt 'raus, sollte aber doch immer wahr sein!!!
     assert(get_irn_arity(block) == n_preds && "phi in wrong block!"); */
  if ((is_Block_dead(block)) ||                  /* Control dead */
      (block == get_irg_start_block(current_ir_graph)))  /* There should be no Phi nodes */
    return new_Bad();                                    /* in the Start Block. */

  if (n_preds == 0) return n;           /* Phi of dead Region without predecessors. */

  /* If the Block has a Bad pred, we also have one. */
  for (i = 0;  i < n_preds;  ++i)
    if (is_Bad(get_Block_cfgpred(block, i)))
      set_Phi_pred(n, i, new_Bad());

  /* Find first non-self-referencing input */
  for (i = 0; i < n_preds; ++i) {
    first_val = get_Phi_pred(n, i);
    if (   (first_val != n)                            /* not self pointer */
#if 1
        && (! is_Bad(first_val))
#endif
           ) {        /* value not dead */
      break;          /* then found first value. */
    }
  }

  if (i >= n_preds) {
    /* A totally Bad or self-referencing Phi (we didn't break the above loop) */
    return new_Bad();
  }

  /* search for rest of inputs, determine if any of these
     are non-self-referencing */
  while (++i < n_preds) {
    ir_node *scnd_val = get_Phi_pred(n, i);
    if (   (scnd_val != n)
        && (scnd_val != first_val)
#if 1
        && (! is_Bad(scnd_val))
#endif
           ) {
      break;
    }
  }

  if (i >= n_preds) {
    /* Fold, if no multiple distinct non-self-referencing inputs */
    n = first_val;
    DBG_OPT_PHI(oldn, n);
  }
  return n;
}

/**
  Several optimizations:
   - no Sync in start block.
   - fold Sync-nodes, iff they have only one predecessor except
           themselves.
  @fixme: are there loop's in Sync's
 */
static ir_node *equivalent_node_Sync(ir_node *n)
{
  int i, n_preds;

  ir_node *oldn = n;
  ir_node *first_val = NULL; /* to shutup gcc */

  if (!get_opt_normalize()) return n;

  n_preds = get_Sync_n_preds(n);

  /* Find first non-self-referencing input */
  for (i = 0; i < n_preds; ++i) {
    first_val = get_Sync_pred(n, i);
    if ((first_val != n)  /* not self pointer */ &&
        (! is_Bad(first_val))
       ) {            /* value not dead */
      break;          /* then found first value. */
    }
  }

  if (i >= n_preds)
    /* A totally Bad or self-referencing Sync (we didn't break the above loop) */
    return new_Bad();

  /* search the rest of inputs, determine if any of these
     are non-self-referencing */
  while (++i < n_preds) {
    ir_node *scnd_val = get_Sync_pred(n, i);
    if ((scnd_val != n) &&
        (scnd_val != first_val) &&
        (! is_Bad(scnd_val))
       )
      break;
  }

  if (i >= n_preds) {
    /* Fold, if no multiple distinct non-self-referencing inputs */
    n = first_val;
    DBG_OPT_SYNC(oldn, n);
  }
  return n;
}

/**
 * optimize Proj(Tuple) and gigo() for ProjX in Bad block,
 * ProjX(Load) and ProjX(Store)
 */
static ir_node *equivalent_node_Proj(ir_node *n)
{
  ir_node *oldn = n;

  ir_node *a = get_Proj_pred(n);

  if ( get_irn_op(a) == op_Tuple) {
    /* Remove the Tuple/Proj combination. */
    if ( get_Proj_proj(n) <= get_Tuple_n_preds(a) ) {
      n = get_Tuple_pred(a, get_Proj_proj(n));
      DBG_OPT_TUPLE(oldn, a, n);
    } else {
      assert(0); /* This should not happen! */
      n = new_Bad();
    }
  }
  else if (get_irn_mode(n) == mode_X) {
    if (is_Block_dead(get_nodes_block(skip_Proj(n)))) {
      /* Remove dead control flow -- early gigo(). */
      n = new_Bad();
    }
    else if (get_opt_ldst_only_null_ptr_exceptions()) {
      ir_op *op = get_irn_op(a);

      if (op == op_Load || op == op_Store) {
        /* get the load/store address */
        ir_node *addr = get_irn_n(a, 1);
        if (value_not_null(addr)) {
          /* this node may float if it did not depend on a Confirm */
          set_irn_pinned(a, op_pin_state_floats);
          DBG_OPT_EXC_REM(n);
          return new_Bad();
        }
      }
    }
  }

  return n;
}

/**
 * Remove Id's.
 */
static ir_node *equivalent_node_Id(ir_node *n)
{
  ir_node *oldn = n;

  do {
    n = get_Id_pred(n);
  } while (get_irn_op(n) == op_Id);

  DBG_OPT_ID(oldn, n);
  return n;
}

/**
 * optimize a Mux
 */
static ir_node *equivalent_node_Mux(ir_node *n)
{
  ir_node *oldn = n, *sel = get_Mux_sel(n);
  tarval *ts = value_of(sel);

  /* Mux(true, f, t) == t */
  if (ts == tarval_b_true) {
    n = get_Mux_true(n);
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_C);
  }
  /* Mux(false, f, t) == f */
  else if (ts == tarval_b_false) {
    n = get_Mux_false(n);
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_C);
  }
  /* Mux(v, x, x) == x */
  else if (get_Mux_false(n) == get_Mux_true(n)) {
    n = get_Mux_true(n);
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_EQ);
  }
  else if (get_irn_op(sel) == op_Proj && !mode_honor_signed_zeros(get_irn_mode(n))) {
    ir_node *cmp = get_Proj_pred(sel);
    long proj_nr = get_Proj_proj(sel);
    ir_node *b   = get_Mux_false(n);
    ir_node *a   = get_Mux_true(n);

    /*
     * Note: normalization puts the constant on the right site,
     * so we check only one case.
     *
     * Note further that these optimization work even for floating point
     * with NaN's because -NaN == NaN.
     * However, if +0 and -0 is handled differently, we cannot use the first one.
     */
    if (get_irn_op(cmp) == op_Cmp && get_Cmp_left(cmp) == a) {
      if (classify_Const(get_Cmp_right(cmp)) == CNST_NULL) {
        /* Mux(a CMP 0, X, a) */
        if (get_irn_op(b) == op_Minus && get_Minus_op(b) == a) {
          /* Mux(a CMP 0, -a, a) */
          if (proj_nr == pn_Cmp_Eq) {
            /* Mux(a == 0, -a, a)  ==>  -a */
            n = b;
            DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_TRANSFORM);
          }
          else if (proj_nr == pn_Cmp_Lg || proj_nr == pn_Cmp_Ne) {
            /* Mux(a != 0, -a, a)  ==> a */
            n = a;
            DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_TRANSFORM);
          }
        }
        else if (classify_Const(b) == CNST_NULL) {
          /* Mux(a CMP 0, 0, a) */
          if (proj_nr == pn_Cmp_Lg || proj_nr == pn_Cmp_Ne) {
            /* Mux(a != 0, 0, a) ==> a */
            n = a;
            DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_TRANSFORM);
          }
          else if (proj_nr == pn_Cmp_Eq) {
            /* Mux(a == 0, 0, a) ==> 0 */
            n = b;
            DBG_OPT_ALGSIM0(oldn, n, FS_OPT_MUX_TRANSFORM);
          }
        }
      }
    }
  }
  return n;
}

/**
 * Returns a equivalent node of  a Psi: if a condition is true
 * and all previous conditions are false we know its value.
 * If all conditions are false its value is the default one.
 */
static ir_node *equivalent_node_Psi(ir_node *n) {
  return n;
}

/**
 * Optimize -a CMP -b into b CMP a.
 * This works only for for modes where unary Minus
 * cannot Overflow.
 * Note that two-complement integers can Overflow
 * so it will NOT work.
 */
static ir_node *equivalent_node_Cmp(ir_node *n)
{
  ir_node *left  = get_Cmp_left(n);
  ir_node *right = get_Cmp_right(n);

  if (get_irn_op(left) == op_Minus && get_irn_op(right) == op_Minus &&
      !mode_overflow_on_unary_Minus(get_irn_mode(left))) {
    left  = get_Minus_op(left);
    right = get_Minus_op(right);
    set_Cmp_left(n, right);
    set_Cmp_right(n, left);
  }
  return n;
}

/**
 * Remove Confirm nodes if setting is on.
 * Replace Confirms(x, '=', Constlike) by Constlike.
 */
static ir_node *equivalent_node_Confirm(ir_node *n)
{
  ir_node *pred = get_Confirm_value(n);
  pn_Cmp  pnc   = get_Confirm_cmp(n);

  if (get_irn_op(pred) == op_Confirm && pnc == get_Confirm_cmp(pred)) {
    /*
     * rare case: two identical Confirms one after another,
     * replace the second one with the first.
     */
    n = pred;
  }
  if (pnc == pn_Cmp_Eq) {
    ir_node *bound = get_Confirm_bound(n);

    /*
     * Optimize a rare case:
     * Confirm(x, '=', Constlike) ==> Constlike
     */
    if (is_irn_constlike(bound)) {
      DBG_OPT_CONFIRM(n, bound);
      return bound;
    }
  }
  return get_opt_remove_confirm() ? get_Confirm_value(n) : n;
}

/**
 * Optimize CopyB(mem, x, x) into a Nop
 */
static ir_node *equivalent_node_CopyB(ir_node *n)
{
  ir_node *a = get_CopyB_dst(n);
  ir_node *b = get_CopyB_src(n);

  if (a == b) {
    /* Turn CopyB into a tuple (mem, bad, bad) */
    ir_node *mem = get_CopyB_mem(n);
    turn_into_tuple(n, pn_CopyB_max);
    set_Tuple_pred(n, pn_CopyB_M,        mem);
    set_Tuple_pred(n, pn_CopyB_X_except, new_Bad());        /* no exception */
    set_Tuple_pred(n, pn_CopyB_M_except, new_Bad());
  }
  return n;
}

/**
 * Optimize Bounds(idx, idx, upper) into idx.
 */
static ir_node *equivalent_node_Bound(ir_node *n)
{
  ir_node *idx   = get_Bound_index(n);
  ir_node *lower = get_Bound_lower(n);
  int ret_tuple = 0;

  /* By definition lower < upper, so if idx == lower -->
     lower <= idx && idx < upper */
  if (idx == lower) {
    /* Turn Bound into a tuple (mem, bad, idx) */
    ret_tuple = 1;
  }
  else {
    ir_node *pred = skip_Proj(idx);

    if (get_irn_op(pred) == op_Bound) {
      /*
       * idx was Bounds_check previously, it is still valid if
       * lower <= pred_lower && pred_upper <= upper.
       */
      ir_node *upper = get_Bound_upper(n);
       if (get_Bound_lower(pred) == lower &&
           get_Bound_upper(pred) == upper) {
         /*
          * One could expect that we simple return the previous
          * Bound here. However, this would be wrong, as we could
          * add an exception Proj to a new location than.
          * So, we must turn in into a tuple
          */
         ret_tuple = 1;
       }
    }
  }
  if (ret_tuple) {
    /* Turn Bound into a tuple (mem, bad, idx) */
    ir_node *mem = get_Bound_mem(n);
    turn_into_tuple(n, pn_Bound_max);
    set_Tuple_pred(n, pn_Bound_M_regular, mem);
    set_Tuple_pred(n, pn_Bound_X_except,  new_Bad());       /* no exception */
    set_Tuple_pred(n, pn_Bound_res,       idx);
    set_Tuple_pred(n, pn_Bound_M_except,  mem);
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
ir_node *
equivalent_node(ir_node *n)
{
  if (n->op->ops.equivalent_node)
    return n->op->ops.equivalent_node(n);
  return n;
}

/**
 * sets the default equivalent node operation for an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
static ir_op_ops *firm_set_default_equivalent_node(opcode code, ir_op_ops *ops)
{
#define CASE(a)                                 \
  case iro_##a:                                 \
    ops->equivalent_node  = equivalent_node_##a; \
    break

  switch (code) {
  CASE(Block);
  CASE(Jmp);
  CASE(Raise);
  CASE(Or);
  CASE(Add);
  CASE(Eor);
  CASE(Sub);
  CASE(Shl);
  CASE(Shr);
  CASE(Shrs);
  CASE(Rot);
  CASE(Not);
  CASE(Minus);
  CASE(Mul);
  CASE(Div);
  CASE(DivMod);
  CASE(And);
  CASE(Conv);
  CASE(Cast);
  CASE(Phi);
  CASE(Sync);
  CASE(Proj);
  CASE(Id);
  CASE(Mux);
  CASE(Psi);
  CASE(Cmp);
  CASE(Confirm);
  CASE(CopyB);
  CASE(Bound);
  default:
    /* leave NULL */;
  }

  return ops;
#undef CASE
}

/**
 * Do node specific optimizations of nodes predecessors.
 */
static void
optimize_preds(ir_node *n) {
  ir_node *a = NULL, *b = NULL;

  /* get the operands we will work on for simple cases. */
  if (is_binop(n)) {
    a = get_binop_left(n);
    b = get_binop_right(n);
  } else if (is_unop(n)) {
    a = get_unop_op(n);
  }

  switch (get_irn_opcode(n)) {

  case iro_Cmp:
    /* We don't want Cast as input to Cmp. */
    if (get_irn_op(a) == op_Cast) {
      a = get_Cast_op(a);
      set_Cmp_left(n, a);
    }
    if (get_irn_op(b) == op_Cast) {
      b = get_Cast_op(b);
      set_Cmp_right(n, b);
    }
    break;

  default: break;
  } /* end switch */
}

/**
 * Transform AddP(P, ConvIs(Iu)), AddP(P, ConvIu(Is)) and
 * SubP(P, ConvIs(Iu)), SubP(P, ConvIu(Is)).
 * If possible, remove the Conv's.
 */
static ir_node *transform_node_AddSub(ir_node *n)
{
  ir_mode *mode = get_irn_mode(n);

  if (mode_is_reference(mode)) {
    ir_node *left  = get_binop_left(n);
    ir_node *right = get_binop_right(n);
    int ref_bits   = get_mode_size_bits(mode);

    if (get_irn_op(left) == op_Conv) {
      ir_mode *mode = get_irn_mode(left);
      int bits      = get_mode_size_bits(mode);

      if (ref_bits == bits &&
          mode_is_int(mode) &&
          get_mode_arithmetic(mode) == irma_twos_complement) {
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

    if (get_irn_op(right) == op_Conv) {
      ir_mode *mode = get_irn_mode(right);
      int bits      = get_mode_size_bits(mode);

      if (ref_bits == bits &&
          mode_is_int(mode) &&
          get_mode_arithmetic(mode) == irma_twos_complement) {
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
  }
  return n;
}

/**
 * Do the AddSub optimization, then Transform
 *   Add(a,a)          -> Mul(a, 2)
 *   Add(Mul(a, x), a) -> Mul(a, x+1)
 * if the mode is integer or float.
 * Transform Add(a,-b) into Sub(a,b).
 * Reassociation might fold this further.
 */
static ir_node *transform_node_Add(ir_node *n)
{
  ir_mode *mode;
  ir_node *oldn = n;

  n = transform_node_AddSub(n);

  mode = get_irn_mode(n);
  if (mode_is_num(mode)) {
    ir_node *a = get_Add_left(n);
    ir_node *b = get_Add_right(n);

    if (a == b) {
      ir_node *block = get_irn_n(n, -1);

      n = new_rd_Mul(
            get_irn_dbg_info(n),
            current_ir_graph,
            block,
            a,
            new_r_Const_long(current_ir_graph, block, mode, 2),
            mode);
      DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_A_A);
    }
    else if (get_irn_op(a) == op_Minus) {
      n = new_rd_Sub(
          get_irn_dbg_info(n),
          current_ir_graph,
          get_irn_n(n, -1),
          b,
          get_Minus_op(a),
          mode);
      DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_A_MINUS_B);
    }
    else if (get_irn_op(b) == op_Minus) {
      n = new_rd_Sub(
          get_irn_dbg_info(n),
          current_ir_graph,
          get_irn_n(n, -1),
          a,
          get_Minus_op(b),
          mode);
      DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_A_MINUS_B);
    }
    /* do NOT execute this code if reassociation is enabled, it does the inverse! */
    else if (!get_opt_reassociation() && get_irn_op(a) == op_Mul) {
      ir_node *ma = get_Mul_left(a);
      ir_node *mb = get_Mul_right(a);

      if (b == ma) {
        ir_node *blk = get_irn_n(n, -1);
        n = new_rd_Mul(
          get_irn_dbg_info(n), current_ir_graph, blk,
          ma,
          new_rd_Add(
            get_irn_dbg_info(n), current_ir_graph, blk,
            mb,
            new_r_Const_long(current_ir_graph, blk, mode, 1),
            mode),
          mode);
        DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_MUL_A_X_A);
      }
      else if (b == mb) {
        ir_node *blk = get_irn_n(n, -1);
        n = new_rd_Mul(
          get_irn_dbg_info(n), current_ir_graph, blk,
          mb,
          new_rd_Add(
            get_irn_dbg_info(n), current_ir_graph, blk,
            ma,
            new_r_Const_long(current_ir_graph, blk, mode, 1),
            mode),
          mode);
        DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_MUL_A_X_A);
      }
    }
    /* do NOT execute this code if reassociation is enabled, it does the inverse! */
    else if (!get_opt_reassociation() && get_irn_op(b) == op_Mul) {
      ir_node *ma = get_Mul_left(b);
      ir_node *mb = get_Mul_right(b);

      if (a == ma) {
        ir_node *blk = get_irn_n(n, -1);
        n = new_rd_Mul(
          get_irn_dbg_info(n), current_ir_graph, blk,
          ma,
          new_rd_Add(
            get_irn_dbg_info(n), current_ir_graph, blk,
            mb,
            new_r_Const_long(current_ir_graph, blk, mode, 1),
            mode),
          mode);
        DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_MUL_A_X_A);
      }
      else if (a == mb) {
        ir_node *blk = get_irn_n(n, -1);
        n = new_rd_Mul(
          get_irn_dbg_info(n), current_ir_graph, blk,
          mb,
          new_rd_Add(
            get_irn_dbg_info(n), current_ir_graph, blk,
            ma,
            new_r_Const_long(current_ir_graph, blk, mode, 1),
            mode),
          mode);
        DBG_OPT_ALGSIM0(oldn, n, FS_OPT_ADD_MUL_A_X_A);
      }
    }
  }
  return n;
}

/**
 * Do the AddSub optimization, then Transform
 *   Sub(0,a)          -> Minus(a)
 *   Sub(Mul(a, x), a) -> Mul(a, x-1)
 */
static ir_node *transform_node_Sub(ir_node *n)
{
  ir_mode *mode;
  ir_node *oldn = n;
  ir_node *a, *b;

  n = transform_node_AddSub(n);

  mode = get_irn_mode(n);
  a    = get_Sub_left(n);
  b    = get_Sub_right(n);
  if (mode_is_num(mode) && (classify_Const(a) == CNST_NULL)) {
    n = new_rd_Minus(
          get_irn_dbg_info(n),
          current_ir_graph,
          get_irn_n(n, -1),
          b,
          mode);
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_0_A);
  }
  /* do NOT execute this code if reassociation is enabled, it does the inverse! */
  else if (get_opt_reassociation() && get_irn_op(a) == op_Mul) {
    ir_node *ma = get_Mul_left(a);
    ir_node *mb = get_Mul_right(a);

    if (ma == b) {
      ir_node *blk = get_irn_n(n, -1);
      n = new_rd_Mul(
        get_irn_dbg_info(n),
        current_ir_graph, blk,
        ma,
        new_rd_Sub(
          get_irn_dbg_info(n),
          current_ir_graph, blk,
          mb,
          new_r_Const_long(current_ir_graph, blk, mode, 1),
          mode),
        mode);
      DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_MUL_A_X_A);
    }
    else if (mb == b) {
      ir_node *blk = get_irn_n(n, -1);
      n = new_rd_Mul(
        get_irn_dbg_info(n),
        current_ir_graph, blk,
        mb,
        new_rd_Sub(
          get_irn_dbg_info(n),
          current_ir_graph, blk,
          ma,
          new_r_Const_long(current_ir_graph, blk, mode, 1),
          mode),
        mode);
      DBG_OPT_ALGSIM0(oldn, n, FS_OPT_SUB_MUL_A_X_A);
    }
  }

  return n;
}

/**
 * Transform Mul(a,-1) into -a.
 * Do architecture dependent optimizations on Mul nodes
 */
static ir_node *transform_node_Mul(ir_node *n) {
  ir_node *oldn = n;
  ir_mode *mode = get_irn_mode(n);

  if (mode_is_signed(mode)) {
    ir_node *r = NULL;
    ir_node *a = get_Mul_left(n);
    ir_node *b = get_Mul_right(n);

    if (value_of(a) == get_mode_minus_one(mode))
      r = b;
    else if (value_of(b) == get_mode_minus_one(mode))
      r = a;
    if (r) {
      n = new_rd_Minus(get_irn_dbg_info(n), current_ir_graph, get_irn_n(n, -1), r, mode);
      DBG_OPT_ALGSIM1(oldn, a, b, n, FS_OPT_MUL_MINUS_1);
      return n;
    }
  }
  return arch_dep_replace_mul_with_shifts(n);
}

/**
 * transform a Div Node
 */
static ir_node *transform_node_Div(ir_node *n)
{
  tarval *tv = value_of(n);
  ir_node *value = n;

  /* BEWARE: it is NOT possible to optimize a/a to 1, as this may cause a exception */

  if (tv != tarval_bad) {
    value = new_Const(get_tarval_mode(tv), tv);

    DBG_OPT_CSTEVAL(n, value);
  }
  else /* Try architecture dependent optimization */
    value = arch_dep_replace_div_by_const(n);

  if (value != n) {
    /* Turn Div into a tuple (mem, bad, value) */
    ir_node *mem = get_Div_mem(n);

    turn_into_tuple(n, pn_Div_max);
    set_Tuple_pred(n, pn_Div_M, mem);
    set_Tuple_pred(n, pn_Div_X_except, new_Bad());
    set_Tuple_pred(n, pn_Div_res, value);
  }
  return n;
}

/**
 * transform a Mod node
 */
static ir_node *transform_node_Mod(ir_node *n)
{
  tarval *tv = value_of(n);
  ir_node *value = n;

  /* BEWARE: it is NOT possible to optimize a%a to 0, as this may cause a exception */

  if (tv != tarval_bad) {
    value = new_Const(get_tarval_mode(tv), tv);

    DBG_OPT_CSTEVAL(n, value);
  }
  else /* Try architecture dependent optimization */
    value = arch_dep_replace_mod_by_const(n);

  if (value != n) {
    /* Turn Mod into a tuple (mem, bad, value) */
    ir_node *mem = get_Mod_mem(n);

    turn_into_tuple(n, pn_Mod_max);
    set_Tuple_pred(n, pn_Mod_M, mem);
    set_Tuple_pred(n, pn_Mod_X_except, new_Bad());
    set_Tuple_pred(n, pn_Mod_res, value);
  }
  return n;
}

/**
 * transform a DivMod node
 */
static ir_node *transform_node_DivMod(ir_node *n)
{
  int evaluated = 0;

  ir_node *a = get_DivMod_left(n);
  ir_node *b = get_DivMod_right(n);
  ir_mode *mode = get_irn_mode(a);
  tarval *ta = value_of(a);
  tarval *tb = value_of(b);

  if (!(mode_is_int(mode) && mode_is_int(get_irn_mode(b))))
    return n;

  /* BEWARE: it is NOT possible to optimize a/a to 1, as this may cause a exception */

  if (tb != tarval_bad) {
    if (tb == get_mode_one(get_tarval_mode(tb))) {
      b = new_Const (mode, get_mode_null(mode));
      evaluated = 1;

      DBG_OPT_CSTEVAL(n, b);
    }
    else if (ta != tarval_bad) {
      tarval *resa, *resb;
      resa = tarval_div (ta, tb);
      if (resa == tarval_bad) return n; /* Causes exception!!! Model by replacing through
                                        Jmp for X result!? */
      resb = tarval_mod (ta, tb);
      if (resb == tarval_bad) return n; /* Causes exception! */
      a = new_Const (mode, resa);
      b = new_Const (mode, resb);
      evaluated = 1;

      DBG_OPT_CSTEVAL(n, a);
      DBG_OPT_CSTEVAL(n, b);
    }
    else { /* Try architecture dependent optimization */
      arch_dep_replace_divmod_by_const(&a, &b, n);
      evaluated = a != NULL;
    }
  } else if (ta == get_mode_null(mode)) {
    /* 0 / non-Const = 0 */
    b = a;
    evaluated = 1;
  }

  if (evaluated) { /* replace by tuple */
    ir_node *mem = get_DivMod_mem(n);
    turn_into_tuple(n, pn_DivMod_max);
    set_Tuple_pred(n, pn_DivMod_M,        mem);
    set_Tuple_pred(n, pn_DivMod_X_except, new_Bad());  /* no exception */
    set_Tuple_pred(n, pn_DivMod_res_div,  a);
    set_Tuple_pred(n, pn_DivMod_res_mod,  b);
  }

  return n;
}

/**
 * Optimize Abs(x) into  x if x is Confirmed >= 0
 * Optimize Abs(x) into -x if x is Confirmed <= 0
 */
static ir_node *transform_node_Abs(ir_node *n)
{
  ir_node        *oldn = n;
  ir_node        *a = get_Abs_op(n);
  value_classify sign = classify_value_sign(a);

  if (sign == VALUE_NEGATIVE) {
    ir_mode *mode = get_irn_mode(n);

    /*
     * We can replace the Abs by -x here.
     * We even could add a new Confirm here.
     *
     * Note that -x would create a new node, so we could
     * not run it in the equivalent_node() context.
     */
    n = new_rd_Minus(get_irn_dbg_info(n), current_ir_graph,
                     get_irn_n(n, -1), a, mode);

    DBG_OPT_CONFIRM(oldn, n);
  }
  else if (sign == VALUE_POSITIVE) {
    /* n is positive, Abs is not needed */
    n = a;

    DBG_OPT_CONFIRM(oldn, n);
  }

  return n;
}

/**
 * transform a Cond node
 */
static ir_node *transform_node_Cond(ir_node *n)
{
  /* Replace the Cond by a Jmp if it branches on a constant
     condition. */
  ir_node *jmp;
  ir_node *a = get_Cond_selector(n);
  tarval *ta = value_of(a);

  /* we need block info which is not available in floating irgs */
  if (get_irg_pinned(current_ir_graph) == op_pin_state_floats)
     return n;

  if ((ta != tarval_bad) &&
      (get_irn_mode(a) == mode_b) &&
      (get_opt_unreachable_code())) {
    /* It's a boolean Cond, branching on a boolean constant.
               Replace it by a tuple (Bad, Jmp) or (Jmp, Bad) */
    jmp = new_r_Jmp(current_ir_graph, get_nodes_block(n));
    turn_into_tuple(n, pn_Cond_max);
    if (ta == tarval_b_true) {
      set_Tuple_pred(n, pn_Cond_false, new_Bad());
      set_Tuple_pred(n, pn_Cond_true, jmp);
    } else {
      set_Tuple_pred(n, pn_Cond_false, jmp);
      set_Tuple_pred(n, pn_Cond_true, new_Bad());
    }
    /* We might generate an endless loop, so keep it alive. */
    add_End_keepalive(get_irg_end(current_ir_graph), get_nodes_block(n));
  }
  return n;
}

/**
 * Transform an Eor.
 */
static ir_node *transform_node_Eor(ir_node *n)
{
  ir_node *oldn = n;
  ir_node *a = get_Eor_left(n);
  ir_node *b = get_Eor_right(n);
  ir_mode *mode = get_irn_mode(n);

  if (a == b) {
    /* a ^ a = 0 */
    n = new_rd_Const(get_irn_dbg_info(n), current_ir_graph, get_irn_n(n, -1),
                     mode, get_mode_null(mode));
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_A_A);
  }
  else if ((mode == mode_b)
      && (get_irn_op(a) == op_Proj)
      && (get_irn_mode(a) == mode_b)
      && (classify_tarval (value_of(b)) == TV_CLASSIFY_ONE)
      && (get_irn_op(get_Proj_pred(a)) == op_Cmp)) {
    /* The Eor negates a Cmp. The Cmp has the negated result anyways! */
    n = new_r_Proj(current_ir_graph, get_irn_n(n, -1), get_Proj_pred(a),
                   mode_b, get_negated_pnc(get_Proj_proj(a), mode));

    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_TO_NOT_BOOL);
  }
  else if ((mode == mode_b)
        && (classify_tarval (value_of(b)) == TV_CLASSIFY_ONE)) {
    /* The Eor is a Not. Replace it by a Not. */
    /*   ????!!!Extend to bitfield 1111111. */
    n = new_r_Not(current_ir_graph, get_irn_n(n, -1), a, mode_b);

    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_EOR_TO_NOT);
  }

  return n;
}

/**
 * Transform a boolean Not.
 */
static ir_node *transform_node_Not(ir_node *n)
{
  ir_node *oldn = n;
  ir_node *a = get_Not_op(n);

  if (   (get_irn_mode(n) == mode_b)
      && (get_irn_op(a) == op_Proj)
      && (get_irn_mode(a) == mode_b)
      && (get_irn_op(get_Proj_pred(a)) == op_Cmp)) {
    /* We negate a Cmp. The Cmp has the negated result anyways! */
    n = new_r_Proj(current_ir_graph, get_irn_n(n, -1), get_Proj_pred(a),
                   mode_b, get_negated_pnc(get_Proj_proj(a), mode_b));
    DBG_OPT_ALGSIM0(oldn, n, FS_OPT_NOT_CMP);
  }

  return n;
}

/**
 * Transform a Cast_type(Const) into a new Const_type
 */
static ir_node *transform_node_Cast(ir_node *n) {
  ir_node *oldn = n;
  ir_node *pred = get_Cast_op(n);
  ir_type *tp = get_irn_type(n);

  if (get_irn_op(pred) == op_Const && get_Const_type(pred) != tp) {
    n = new_rd_Const_type(NULL, current_ir_graph, get_irn_n(pred, -1), get_irn_mode(pred),
              get_Const_tarval(pred), tp);
    DBG_OPT_CSTEVAL(oldn, n);
  } else if ((get_irn_op(pred) == op_SymConst) && (get_SymConst_value_type(pred) != tp)) {
    n = new_rd_SymConst_type(NULL, current_ir_graph, get_irn_n(pred, -1), get_SymConst_symbol(pred),
                 get_SymConst_kind(pred), tp);
    DBG_OPT_CSTEVAL(oldn, n);
  }

  return n;
}

/**
 * Transform a Proj(Div) with a non-zero value.
 * Removes the exceptions and routes the memory to the NoMem node.
 */
static ir_node *transform_node_Proj_Div(ir_node *proj)
{
  ir_node *n = get_Proj_pred(proj);
  ir_node *b = get_Div_right(n);
  long proj_nr;

  if (value_not_zero(b)) {
    /* div(x, y) && y != 0 */
    proj_nr = get_Proj_proj(proj);

    /* this node may float if it did not depend on a Confirm */
    set_irn_pinned(n, op_pin_state_floats);

    if (proj_nr == pn_Div_X_except) {
      /* we found an exception handler, remove it */
      DBG_OPT_EXC_REM(proj);
      return new_Bad();
    }
    else if (proj_nr == pn_Div_M) {
      ir_node *res = get_Div_mem(n);
      /* the memory Proj can only be removed if we divide by a
         real constant, but the node never produce a new memory */
      if (value_of(b) != tarval_bad) {
        /* this is a Div by a const, we can remove the memory edge */
        set_Div_mem(n, get_irg_no_mem(current_ir_graph));
      }
      return res;
    }
  }
  return proj;
}

/**
 * Transform a Proj(Mod) with a non-zero value.
 * Removes the exceptions and routes the memory to the NoMem node.
 */
static ir_node *transform_node_Proj_Mod(ir_node *proj)
{
  ir_node *n = get_Proj_pred(proj);
  ir_node *b = get_Mod_right(n);
  long proj_nr;

  if (value_not_zero(b)) {
    /* mod(x, y) && y != 0 */
    proj_nr = get_Proj_proj(proj);

    /* this node may float if it did not depend on a Confirm */
    set_irn_pinned(n, op_pin_state_floats);

    if (proj_nr == pn_Mod_X_except) {
      /* we found an exception handler, remove it */
      DBG_OPT_EXC_REM(proj);
      return new_Bad();
    } else if (proj_nr == pn_Mod_M) {
      ir_node *res = get_Mod_mem(n);
      /* the memory Proj can only be removed if we divide by a
         real constant, but the node never produce a new memory */
      if (value_of(b) != tarval_bad) {
        /* this is a Mod by a const, we can remove the memory edge */
        set_Mod_mem(n, get_irg_no_mem(current_ir_graph));
      }
      return res;
    }
    else if (proj_nr == pn_Mod_res && get_Mod_left(n) == b) {
      /* a % a = 0 if a != 0 */
      ir_mode *mode = get_irn_mode(proj);
      ir_node *res  = new_Const(mode, get_mode_null(mode));

      DBG_OPT_CSTEVAL(n, res);
      return res;
    }
  }
  return proj;
}

/**
 * Transform a Proj(DivMod) with a non-zero value.
 * Removes the exceptions and routes the memory to the NoMem node.
 */
static ir_node *transform_node_Proj_DivMod(ir_node *proj)
{
  ir_node *n = get_Proj_pred(proj);
  ir_node *b = get_DivMod_right(n);
  long proj_nr;

  if (value_not_zero(b)) {
    /* DivMod(x, y) && y != 0 */
    proj_nr = get_Proj_proj(proj);

    /* this node may float if it did not depend on a Confirm */
    set_irn_pinned(n, op_pin_state_floats);

    if (proj_nr == pn_DivMod_X_except) {
      /* we found an exception handler, remove it */
      DBG_OPT_EXC_REM(proj);
      return new_Bad();
    }
    else if (proj_nr == pn_DivMod_M) {
      ir_node *res = get_DivMod_mem(n);
      /* the memory Proj can only be removed if we divide by a
         real constant, but the node never produce a new memory */
      if (value_of(b) != tarval_bad) {
        /* this is a DivMod by a const, we can remove the memory edge */
        set_DivMod_mem(n, get_irg_no_mem(current_ir_graph));
      }
      return res;
    }
    else if (proj_nr == pn_DivMod_res_mod && get_DivMod_left(n) == b) {
      /* a % a = 0 if a != 0 */
      ir_mode *mode = get_irn_mode(proj);
      ir_node *res  = new_Const(mode, get_mode_null(mode));

      DBG_OPT_CSTEVAL(n, res);
      return res;
    }
  }
  return proj;
}

/**
 * Optimizes jump tables (CondIs or CondIu) by removing all impossible cases.
 */
static ir_node *transform_node_Proj_Cond(ir_node *proj)
{
  if (get_opt_unreachable_code()) {
    ir_node *n = get_Proj_pred(proj);
    ir_node *b = get_Cond_selector(n);

    if (mode_is_int(get_irn_mode(b))) {
      tarval *tb = value_of(b);

      if (tb != tarval_bad) {
        /* we have a constant switch */
        long num = get_Proj_proj(proj);

        if (num != get_Cond_defaultProj(n)) { /* we cannot optimize default Proj's yet */
          if (get_tarval_long(tb) == num) {
            /* Do NOT create a jump here, or we will have 2 control flow ops
             * in a block. This case is optimized away in optimize_cf(). */
            return proj;
          }
          else {
            /* this case will NEVER be taken, kill it */
            return new_Bad();
          }
        }
      }
    }
  }
  return proj;
}

/**
 * Normalizes and optimizes Cmp nodes.
 */
static ir_node *transform_node_Proj_Cmp(ir_node *proj)
{
  if (get_opt_reassociation()) {
    ir_node *n     = get_Proj_pred(proj);
    ir_node *left  = get_Cmp_left(n);
    ir_node *right = get_Cmp_right(n);
    ir_node *c     = NULL;
    tarval *tv     = NULL;
    int changed    = 0;
    ir_mode *mode  = NULL;
    long proj_nr   = get_Proj_proj(proj);

    /*
     * First step: normalize the compare op
     * by placing the constant on the right site
     * or moving the lower address node to the left.
     * We ignore the case that both are constants
     * this case should be optimized away.
     */
    if (get_irn_op(right) == op_Const)
      c = right;
    else if (get_irn_op(left) == op_Const) {
      c     = left;
      left  = right;
      right = c;

      proj_nr = get_inversed_pnc(proj_nr);
      changed |= 1;
    }
    else if (left > right) {
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
    if (c) {
      mode = get_irn_mode(c);
      tv = get_Const_tarval(c);

      if (tv != tarval_bad) {
        /* the following optimization is possible on modes without Overflow
         * on Unary Minus or on == and !=:
         * -a CMP c  ==>  a swap(CMP) -c
         *
         * Beware: for two-complement Overflow may occur, so only == and != can
         * be optimized, see this:
         * -MININT < 0 =/=> MININT > 0 !!!
         */
        if (get_opt_constant_folding() && get_irn_op(left) == op_Minus &&
            (!mode_overflow_on_unary_Minus(mode) ||
             (mode_is_int(mode) && (proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg)))) {
          left = get_Minus_op(left);
          tv = tarval_sub(get_mode_null(mode), tv);

          proj_nr = get_inversed_pnc(proj_nr);
          changed |= 2;
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
            tv = tarval_sub(tv, get_mode_one(mode));

            proj_nr ^= pn_Cmp_Eq;
            changed |= 2;
          }
          /* c < 0 : a > c  ==>  a >= (c+1)    a <= c  ==>  a < (c+1) */
          else if ((proj_nr == pn_Cmp_Gt || proj_nr == pn_Cmp_Le) &&
              tarval_cmp(tv, get_mode_null(mode)) == pn_Cmp_Lt) {
            tv = tarval_add(tv, get_mode_one(mode));

            proj_nr ^= pn_Cmp_Eq;
            changed |= 2;
          }

          /* the following reassociations work only for == and != */
          if (proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg) {

            /* a-b == 0  ==>  a == b,  a-b != 0  ==>  a != b */
            if (classify_tarval(tv) == TV_CLASSIFY_NULL && get_irn_op(left) == op_Sub) {
              right = get_Sub_right(left);
              left  = get_Sub_left(left);

              tv = value_of(right);
              changed = 1;
            }

            if (tv != tarval_bad) {
              ir_op *op = get_irn_op(left);

              /* a-c1 == c2  ==>  a == c2+c1,  a-c1 != c2  ==>  a != c2+c1 */
              if (op == op_Sub) {
                ir_node *c1 = get_Sub_right(left);
                tarval *tv2 = value_of(c1);

                if (tv2 != tarval_bad) {
                  tv2 = tarval_add(tv, value_of(c1));

                  if (tv2 != tarval_bad) {
                    left    = get_Sub_left(left);
                    tv      = tv2;
                    changed |= 2;
                  }
                }
              }
              /* a+c1 == c2  ==>  a == c2-c1,  a+c1 != c2  ==>  a != c2-c1 */
              else if (op == op_Add) {
                ir_node *a_l = get_Add_left(left);
                ir_node *a_r = get_Add_right(left);
                ir_node *a;
                tarval *tv2;

                if (get_irn_op(a_l) == op_Const) {
                  a = a_r;
                  tv2 = value_of(a_l);
                }
                else {
                  a = a_l;
                  tv2 = value_of(a_r);
                }

                if (tv2 != tarval_bad) {
                  tv2 = tarval_sub(tv, tv2);

                  if (tv2 != tarval_bad) {
                    left    = a;
                    tv      = tv2;
                    changed |= 2;
                  }
                }
              }
              /* -a == c ==> a == -c, -a != c ==> a != -c */
              else if (op == op_Minus) {
                tarval *tv2 = tarval_sub(get_mode_null(mode), tv);

                if (tv2 != tarval_bad) {
                  left    = get_Minus_op(left);
                  tv      = tv2;
                  changed |= 2;
                }
              }
            }
          } /* == or != */
          /* the following reassociations work only for <= */
          else if (proj_nr == pn_Cmp_Le || proj_nr == pn_Cmp_Lt) {
            if (tv != tarval_bad) {
              ir_op *op = get_irn_op(left);

              /* c >= 0 : Abs(a) <= c  ==>  (unsigned)(a + c) <= 2*c */
              if (op == op_Abs) {
              }
            }
          }
        } /* mode_is_int */

        /*
         * optimization for AND:
         * Optimize:
         *   And(x, C) == C  ==>  And(x, C) != 0
         *   And(x, C) != C  ==>  And(X, C) == 0
         *
         * if C is a single Bit constant.
         */
        if ((proj_nr == pn_Cmp_Eq || proj_nr == pn_Cmp_Lg) &&
            (get_irn_op(left) == op_And)) {
          if (is_single_bit_tarval(tv)) {
            /* check for Constant's match. We have check hare the tarvals,
               because our const might be changed */
            ir_node *la = get_And_left(left);
            ir_node *ra = get_And_right(left);
            if ((is_Const(la) && get_Const_tarval(la) == tv) ||
                (is_Const(ra) && get_Const_tarval(ra) == tv)) {
              /* fine: do the transformation */
              tv = get_mode_null(get_tarval_mode(tv));
              proj_nr ^= pn_Cmp_Leg;
              changed |= 2;
            }
          }
        }
      } /* tarval != bad */
    }

    if (changed) {
      ir_node *block = get_irn_n(n, -1); /* Beware of get_nodes_Block() */

      if (changed & 2)      /* need a new Const */
        right = new_Const(mode, tv);

      /* create a new compare */
      n = new_rd_Cmp(get_irn_dbg_info(n), current_ir_graph, block,
            left, right);

      set_Proj_pred(proj, n);
      set_Proj_proj(proj, proj_nr);
    }
  }
  return proj;
}

/**
 * Does all optimizations on nodes that must be done on it's Proj's
 * because of creating new nodes.
 */
static ir_node *transform_node_Proj(ir_node *proj)
{
  ir_node *n = get_Proj_pred(proj);

  switch (get_irn_opcode(n)) {
  case iro_Div:
    return transform_node_Proj_Div(proj);

  case iro_Mod:
    return transform_node_Proj_Mod(proj);

  case iro_DivMod:
    return transform_node_Proj_DivMod(proj);

  case iro_Cond:
    return transform_node_Proj_Cond(proj);

  case iro_Cmp:
    return transform_node_Proj_Cmp(proj);

  case iro_Tuple:
    /* should not happen, but if it does will be optimized away */
    return equivalent_node_Proj(proj);

  default:
    /* do nothing */
    return proj;
  }
}

/**
 * returns the operands of a commutative bin-op, if one operand is
 * a const, it is returned as the second one.
 */
static void get_comm_Binop_Ops(ir_node *binop, ir_node **a, ir_node **c)
{
  ir_node *op_a = get_binop_left(binop);
  ir_node *op_b = get_binop_right(binop);

  assert(is_op_commutative(get_irn_op(binop)));

  if (get_irn_op(op_a) == op_Const) {
    *a = op_b;
    *c = op_a;
  }
  else {
    *a = op_a;
    *c = op_b;
  }
}

/**
 * Optimize a Or(And(Or(And(v,c4),c3),c2),c1) pattern if possible.
 * Such pattern may arise in bitfield stores.
 *
 * value  c4                  value      c4 & c2
 *    AND     c3                    AND           c1 | c3
 *        OR     c2      ===>               OR
 *           AND    c1
 *               OR
 */
static ir_node *transform_node_Or_bf_store(ir_node *or)
{
  ir_node *and, *c1;
  ir_node *or_l, *c2;
  ir_node *and_l, *c3;
  ir_node *value, *c4;
  ir_node *new_and, *new_const, *block;
  ir_mode *mode = get_irn_mode(or);

  tarval *tv1, *tv2, *tv3, *tv4, *tv, *n_tv4, *n_tv2;

  get_comm_Binop_Ops(or, &and, &c1);
  if ((get_irn_op(c1) != op_Const) || (get_irn_op(and) != op_And))
    return or;

  get_comm_Binop_Ops(and, &or_l, &c2);
  if ((get_irn_op(c2) != op_Const) || (get_irn_op(or_l) != op_Or))
    return or;

  get_comm_Binop_Ops(or_l, &and_l, &c3);
  if ((get_irn_op(c3) != op_Const) || (get_irn_op(and_l) != op_And))
    return or;

  get_comm_Binop_Ops(and_l, &value, &c4);
  if (get_irn_op(c4) != op_Const)
    return or;

  /* ok, found the pattern, check for conditions */
  assert(mode == get_irn_mode(and));
  assert(mode == get_irn_mode(or_l));
  assert(mode == get_irn_mode(and_l));

  tv1 = get_Const_tarval(c1);
  tv2 = get_Const_tarval(c2);
  tv3 = get_Const_tarval(c3);
  tv4 = get_Const_tarval(c4);

  tv = tarval_or(tv4, tv2);
  if (classify_tarval(tv) != TV_CLASSIFY_ALL_ONE) {
    /* have at least one 0 at the same bit position */
    return or;
  }

  n_tv4 = tarval_not(tv4);
  if (tv3 != tarval_and(tv3, n_tv4)) {
    /* bit in the or_mask is outside the and_mask */
    return or;
  }

  n_tv2 = tarval_not(tv2);
  if (tv1 != tarval_and(tv1, n_tv2)) {
    /* bit in the or_mask is outside the and_mask */
    return or;
  }

  /* ok, all conditions met */
  block = get_irn_n(or, -1);

  new_and = new_r_And(current_ir_graph, block,
      value, new_r_Const(current_ir_graph, block, mode, tarval_and(tv4, tv2)), mode);

  new_const = new_r_Const(current_ir_graph, block, mode, tarval_or(tv3, tv1));

  set_Or_left(or, new_and);
  set_Or_right(or, new_const);

  /* check for more */
  return transform_node_Or_bf_store(or);
}

/**
 * Optimize an Or(shl(x, c), shr(x, bits - c)) into a Rot
 */
static ir_node *transform_node_Or_Rot(ir_node *or)
{
  ir_mode *mode = get_irn_mode(or);
  ir_node *shl, *shr, *block;
  ir_node *irn, *x, *c1, *c2, *v, *sub, *n;
  tarval *tv1, *tv2;

  if (! mode_is_int(mode))
    return or;

  shl = get_binop_left(or);
  shr = get_binop_right(or);

  if (get_irn_op(shl) == op_Shr) {
    if (get_irn_op(shr) != op_Shl)
      return or;

    irn = shl;
    shl = shr;
    shr = irn;
  }
  else if (get_irn_op(shl) != op_Shl)
    return or;
  else if (get_irn_op(shr) != op_Shr)
    return or;

  x = get_Shl_left(shl);
  if (x != get_Shr_left(shr))
    return or;

  c1 = get_Shl_right(shl);
  c2 = get_Shr_right(shr);
  if (get_irn_op(c1) == op_Const && get_irn_op(c2) == op_Const) {
    tv1 = get_Const_tarval(c1);
    if (! tarval_is_long(tv1))
      return or;

    tv2 = get_Const_tarval(c2);
    if (! tarval_is_long(tv2))
      return or;

    if (get_tarval_long(tv1) + get_tarval_long(tv2)
        != get_mode_size_bits(mode))
      return or;

    /* yet, condition met */
    block = get_irn_n(or, -1);

    n = new_r_Rot(current_ir_graph, block, x, c1, mode);

    DBG_OPT_ALGSIM1(or, shl, shr, n, FS_OPT_OR_SHFT_TO_ROT);
    return n;
  }
  else if (get_irn_op(c1) == op_Sub) {
    v   = c2;
    sub = c1;

    if (get_Sub_right(sub) != v)
      return or;

    c1 = get_Sub_left(sub);
    if (get_irn_op(c1) != op_Const)
      return or;

    tv1 = get_Const_tarval(c1);
    if (! tarval_is_long(tv1))
      return or;

    if (get_tarval_long(tv1) != get_mode_size_bits(mode))
      return or;

    /* yet, condition met */
    block = get_nodes_block(or);

    /* a Rot right is not supported, so use a rot left */
    n =  new_r_Rot(current_ir_graph, block, x, sub, mode);

    DBG_OPT_ALGSIM0(or, n, FS_OPT_OR_SHFT_TO_ROT);
    return n;
  }
  else if (get_irn_op(c2) == op_Sub) {
    v   = c1;
    sub = c2;

    c1 = get_Sub_left(sub);
    if (get_irn_op(c1) != op_Const)
      return or;

    tv1 = get_Const_tarval(c1);
    if (! tarval_is_long(tv1))
      return or;

    if (get_tarval_long(tv1) != get_mode_size_bits(mode))
      return or;

    /* yet, condition met */
    block = get_irn_n(or, -1);

    /* a Rot Left */
    n = new_r_Rot(current_ir_graph, block, x, v, mode);

    DBG_OPT_ALGSIM0(or, n, FS_OPT_OR_SHFT_TO_ROT);
    return n;
  }

  return or;
}

/**
 * Optimize an Or
 */
static ir_node *transform_node_Or(ir_node *or)
{
  or = transform_node_Or_bf_store(or);
  or = transform_node_Or_Rot(or);

  return or;
}

/* forward */
static ir_node *transform_node(ir_node *n);

/**
 * Optimize (a >> c1) >> c2), works for Shr, Shrs, Shl.
 *
 * Should be moved to reassociation?
 */
static ir_node *transform_node_shift(ir_node *n)
{
  ir_node *left, *right;
  tarval *tv1, *tv2, *res;
  ir_mode *mode;
  int modulo_shf, flag;

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

  res = tarval_add(tv1, tv2);

  /* beware: a simple replacement works only, if res < modulo shift */
  mode = get_irn_mode(n);

  flag = 0;

  modulo_shf = get_mode_modulo_shift(mode);
  if (modulo_shf > 0) {
    tarval *modulo = new_tarval_from_long(modulo_shf, get_tarval_mode(res));

    if (tarval_cmp(res, modulo) & pn_Cmp_Lt)
      flag = 1;
  }
  else
    flag = 1;

  if (flag) {
    /* ok, we can replace it */
    ir_node *in[2], *irn, *block = get_irn_n(n, -1);

    in[0] = get_binop_left(left);
    in[1] = new_r_Const(current_ir_graph, block, get_tarval_mode(res), res);

    irn = new_ir_node(NULL, current_ir_graph, block, get_irn_op(n), mode, 2, in);

    DBG_OPT_ALGSIM0(n, irn, FS_OPT_REASSOC_SHIFT);

    return transform_node(irn);
  }
  return n;
}

#define transform_node_Shr  transform_node_shift
#define transform_node_Shrs transform_node_shift
#define transform_node_Shl  transform_node_shift

/**
 * Remove dead blocks and nodes in dead blocks
 * in keep alive list.  We do not generate a new End node.
 */
static ir_node *transform_node_End(ir_node *n) {
  int i, n_keepalives = get_End_n_keepalives(n);

  for (i = 0; i < n_keepalives; ++i) {
    ir_node *ka = get_End_keepalive(n, i);
    if (is_Block(ka)) {
      if (is_Block_dead(ka)) {
        set_End_keepalive(n, i, new_Bad());
      }
    }
    else if (is_irn_pinned_in_irg(ka) && is_Block_dead(get_nodes_block(ka)))
      set_End_keepalive(n, i, new_Bad());
  }
  return n;
}

/**
 * Optimize a Mux into some simpler cases.
 */
static ir_node *transform_node_Mux(ir_node *n)
{
  ir_node *oldn = n, *sel = get_Mux_sel(n);
  ir_mode *mode = get_irn_mode(n);

  if (get_irn_op(sel) == op_Proj && !mode_honor_signed_zeros(mode)) {
    ir_node *cmp = get_Proj_pred(sel);
    long proj_nr = get_Proj_proj(sel);
    ir_node *f   =  get_Mux_false(n);
    ir_node *t   = get_Mux_true(n);

    if (get_irn_op(cmp) == op_Cmp && classify_Const(get_Cmp_right(cmp)) == CNST_NULL) {
      ir_node *block = get_irn_n(n, -1);

      /*
       * Note: normalization puts the constant on the right site,
       * so we check only one case.
       *
       * Note further that these optimization work even for floating point
       * with NaN's because -NaN == NaN.
       * However, if +0 and -0 is handled differently, we cannot use the first one.
       */
      if (get_irn_op(f) == op_Minus &&
          get_Minus_op(f)   == t &&
          get_Cmp_left(cmp) == t) {

        if (proj_nr == pn_Cmp_Ge || proj_nr == pn_Cmp_Gt) {
          /* Mux(a >=/> 0, -a, a)  ==>  Abs(a) */
          n = new_rd_Abs(get_irn_dbg_info(n),
                current_ir_graph,
                block,
                t, mode);
          DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_ABS);
          return n;
        }
        else if (proj_nr == pn_Cmp_Le || proj_nr == pn_Cmp_Lt) {
          /* Mux(a <=/< 0, -a, a)  ==>  Minus(Abs(a)) */
          n = new_rd_Abs(get_irn_dbg_info(n),
                current_ir_graph,
                block,
                t, mode);
          n = new_rd_Minus(get_irn_dbg_info(n),
                current_ir_graph,
                block,
                n, mode);

          DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_ABS);
          return n;
        }
      }
      else if (get_irn_op(t) == op_Minus &&
          get_Minus_op(t)   == f &&
          get_Cmp_left(cmp) == f) {

        if (proj_nr == pn_Cmp_Le || proj_nr == pn_Cmp_Lt) {
          /* Mux(a <=/< 0, a, -a)  ==>  Abs(a) */
          n = new_rd_Abs(get_irn_dbg_info(n),
                current_ir_graph,
                block,
                f, mode);
          DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_ABS);
          return n;
        }
        else if (proj_nr == pn_Cmp_Ge || proj_nr == pn_Cmp_Gt) {
          /* Mux(a >=/> 0, a, -a)  ==>  Minus(Abs(a)) */
          n = new_rd_Abs(get_irn_dbg_info(n),
                current_ir_graph,
                block,
                f, mode);
          n = new_rd_Minus(get_irn_dbg_info(n),
                current_ir_graph,
                block,
                n, mode);

          DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_ABS);
          return n;
        }
      }

      if (mode_is_int(mode) && mode_is_signed(mode) &&
          get_mode_arithmetic(mode) == irma_twos_complement) {
        ir_node *x = get_Cmp_left(cmp);

        /* the following optimization works only with signed integer two-complement mode */

        if (mode == get_irn_mode(x)) {
          /*
           * FIXME: this restriction is two rigid, as it would still
           * work if mode(x) = Hs and mode == Is, but at least it removes
           * all wrong cases.
           */
          if ((proj_nr == pn_Cmp_Lt || proj_nr == pn_Cmp_Le) &&
              classify_Const(t) == CNST_ALL_ONE &&
              classify_Const(f) == CNST_NULL) {
            /*
             * Mux(x:T </<= 0, 0, -1) -> Shrs(x, sizeof_bits(T) - 1)
             * Conditions:
             * T must be signed.
             */
            n = new_rd_Shrs(get_irn_dbg_info(n),
                  current_ir_graph, block, x,
                  new_r_Const_long(current_ir_graph, block, mode_Iu,
                    get_mode_size_bits(mode) - 1),
                  mode);
            DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_SHR);
            return n;
          }
          else if ((proj_nr == pn_Cmp_Gt || proj_nr == pn_Cmp_Ge) &&
                   classify_Const(t) == CNST_ONE &&
                   classify_Const(f) == CNST_NULL) {
            /*
             * Mux(x:T >/>= 0, 0, 1) -> Shr(-x, sizeof_bits(T) - 1)
             * Conditions:
             * T must be signed.
             */
            n = new_rd_Shr(get_irn_dbg_info(n),
                  current_ir_graph, block,
                  new_r_Minus(current_ir_graph, block, x, mode),
                  new_r_Const_long(current_ir_graph, block, mode_Iu,
                    get_mode_size_bits(mode) - 1),
                  mode);
            DBG_OPT_ALGSIM1(oldn, cmp, sel, n, FS_OPT_MUX_TO_SHR);
            return n;
          }
        }
      }
    }
  }
  return arch_transform_node_Mux(n);
}

/**
 * Optimize a Psi into some simpler cases.
 */
static ir_node *transform_node_Psi(ir_node *n) {
  if (is_Mux(n))
    return transform_node_Mux(n);

  return n;
}

/**
 * Tries several [inplace] [optimizing] transformations and returns an
 * equivalent node.  The difference to equivalent_node() is that these
 * transformations _do_ generate new nodes, and thus the old node must
 * not be freed even if the equivalent node isn't the old one.
 */
static ir_node *transform_node(ir_node *n)
{
  if (n->op->ops.transform_node)
    n = n->op->ops.transform_node(n);
  return n;
}

/**
 * sSets the default transform node operation for an ir_op_ops.
 *
 * @param code   the opcode for the default operation
 * @param ops    the operations initialized
 *
 * @return
 *    The operations.
 */
static ir_op_ops *firm_set_default_transform_node(opcode code, ir_op_ops *ops)
{
#define CASE(a)                                 \
  case iro_##a:                                 \
    ops->transform_node  = transform_node_##a;   \
    break

  switch (code) {
  CASE(Add);
  CASE(Sub);
  CASE(Mul);
  CASE(Div);
  CASE(Mod);
  CASE(DivMod);
  CASE(Abs);
  CASE(Cond);
  CASE(Eor);
  CASE(Not);
  CASE(Cast);
  CASE(Proj);
  CASE(Sel);
  CASE(Or);
  CASE(Shr);
  CASE(Shrs);
  CASE(Shl);
  CASE(End);
  CASE(Mux);
  CASE(Psi);
  default:
    /* leave NULL */;
  }

  return ops;
#undef CASE
}


/* **************** Common Subexpression Elimination **************** */

/** The size of the hash table used, should estimate the number of nodes
    in a graph. */
#define N_IR_NODES 512

/** Compares the attributes of two Const nodes. */
static int node_cmp_attr_Const(ir_node *a, ir_node *b)
{
  return (get_Const_tarval(a) != get_Const_tarval(b))
      || (get_Const_type(a) != get_Const_type(b));
}

/** Compares the attributes of two Proj nodes. */
static int node_cmp_attr_Proj(ir_node *a, ir_node *b)
{
  return get_irn_proj_attr (a) != get_irn_proj_attr (b);
}

/** Compares the attributes of two Filter nodes. */
static int node_cmp_attr_Filter(ir_node *a, ir_node *b)
{
  return get_Filter_proj(a) != get_Filter_proj(b);
}

/** Compares the attributes of two Alloc nodes. */
static int node_cmp_attr_Alloc(ir_node *a, ir_node *b)
{
  return (get_irn_alloc_attr(a).where != get_irn_alloc_attr(b).where)
      || (get_irn_alloc_attr(a).type != get_irn_alloc_attr(b).type);
}

/** Compares the attributes of two Free nodes. */
static int node_cmp_attr_Free(ir_node *a, ir_node *b)
{
  return (get_irn_free_attr(a).where != get_irn_free_attr(b).where)
      || (get_irn_free_attr(a).type != get_irn_free_attr(b).type);
}

/** Compares the attributes of two SymConst nodes. */
static int node_cmp_attr_SymConst(ir_node *a, ir_node *b)
{
  return (get_irn_symconst_attr(a).num != get_irn_symconst_attr(b).num)
      || (get_irn_symconst_attr(a).sym.type_p != get_irn_symconst_attr(b).sym.type_p)
      || (get_irn_symconst_attr(a).tp != get_irn_symconst_attr(b).tp);
}

/** Compares the attributes of two Call nodes. */
static int node_cmp_attr_Call(ir_node *a, ir_node *b)
{
  return (get_irn_call_attr(a) != get_irn_call_attr(b));
}

/** Compares the attributes of two Sel nodes. */
static int node_cmp_attr_Sel(ir_node *a, ir_node *b)
{
  return (get_irn_sel_attr(a).ent->kind  != get_irn_sel_attr(b).ent->kind)
      || (get_irn_sel_attr(a).ent->name    != get_irn_sel_attr(b).ent->name)
      || (get_irn_sel_attr(a).ent->owner   != get_irn_sel_attr(b).ent->owner)
      || (get_irn_sel_attr(a).ent->ld_name != get_irn_sel_attr(b).ent->ld_name)
      || (get_irn_sel_attr(a).ent->type    != get_irn_sel_attr(b).ent->type);
}

/** Compares the attributes of two Phi nodes. */
static int node_cmp_attr_Phi(ir_node *a, ir_node *b)
{
  return get_irn_phi_attr (a) != get_irn_phi_attr (b);
}

/** Compares the attributes of two Cast nodes. */
static int node_cmp_attr_Cast(ir_node *a, ir_node *b)
{
  return get_Cast_type(a) != get_Cast_type(b);
}

/** Compares the attributes of two Load nodes. */
static int node_cmp_attr_Load(ir_node *a, ir_node *b)
{
  if (get_Load_volatility(a) == volatility_is_volatile ||
      get_Load_volatility(b) == volatility_is_volatile)
    /* NEVER do CSE on volatile Loads */
    return 1;

  return get_Load_mode(a) != get_Load_mode(b);
}

/** Compares the attributes of two Store nodes. */
static int node_cmp_attr_Store(ir_node *a, ir_node *b)
{
  /* NEVER do CSE on volatile Stores */
  return (get_Store_volatility(a) == volatility_is_volatile ||
          get_Store_volatility(b) == volatility_is_volatile);
}

/** Compares the attributes of two Confirm nodes. */
static int node_cmp_attr_Confirm(ir_node *a, ir_node *b)
{
  return (get_Confirm_cmp(a) != get_Confirm_cmp(b));
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
static ir_op_ops *firm_set_default_node_cmp_attr(opcode code, ir_op_ops *ops)
{
#define CASE(a)                              \
  case iro_##a:                              \
    ops->node_cmp_attr  = node_cmp_attr_##a; \
    break

  switch (code) {
  CASE(Const);
  CASE(Proj);
  CASE(Filter);
  CASE(Alloc);
  CASE(Free);
  CASE(SymConst);
  CASE(Call);
  CASE(Sel);
  CASE(Phi);
  CASE(Cast);
  CASE(Load);
  CASE(Store);
  CASE(Confirm);
  default:
    /* leave NULL */;
  }

  return ops;
#undef CASE
}

/*
 * Compare function for two nodes in the hash table. Gets two
 * nodes as parameters.  Returns 0 if the nodes are a cse.
 */
int identities_cmp(const void *elt, const void *key)
{
  ir_node *a, *b;
  int i, irn_arity_a;

  a = (void *)elt;
  b = (void *)key;

  if (a == b) return 0;

  if ((get_irn_op(a) != get_irn_op(b)) ||
      (get_irn_mode(a) != get_irn_mode(b))) return 1;

  /* compare if a's in and b's in are of equal length */
  irn_arity_a = get_irn_intra_arity (a);
  if (irn_arity_a != get_irn_intra_arity(b))
    return 1;

  /* for block-local cse and op_pin_state_pinned nodes: */
  if (!get_opt_global_cse() || (get_irn_pinned(a) == op_pin_state_pinned)) {
    if (get_irn_intra_n(a, -1) != get_irn_intra_n(b, -1))
      return 1;
  }

  /* compare a->in[0..ins] with b->in[0..ins] */
  for (i = 0; i < irn_arity_a; i++)
    if (get_irn_intra_n(a, i) != get_irn_intra_n(b, i))
      return 1;

  /*
   * here, we already now that the nodes are identical except their
   * attributes
   */
  if (a->op->ops.node_cmp_attr)
    return a->op->ops.node_cmp_attr(a, b);

  return 0;
}

/*
 * Calculate a hash value of a node.
 */
unsigned
ir_node_hash (ir_node *node)
{
  unsigned h;
  int i, irn_arity;

  if (node->op == op_Const) {
    /* special value for const, as they only differ in their tarval. */
    h = HASH_PTR(node->attr.con.tv);
    h = 9*h + HASH_PTR(get_irn_mode(node));
  } else if (node->op == op_SymConst) {
    /* special value for const, as they only differ in their symbol. */
    h = HASH_PTR(node->attr.i.sym.type_p);
    h = 9*h + HASH_PTR(get_irn_mode(node));
  } else {

    /* hash table value = 9*(9*(9*(9*(9*arity+in[0])+in[1])+ ...)+mode)+code */
    h = irn_arity = get_irn_intra_arity(node);

    /* consider all in nodes... except the block if not a control flow. */
    for (i =  is_cfop(node) ? -1 : 0;  i < irn_arity;  i++) {
      h = 9*h + HASH_PTR(get_irn_intra_n(node, i));
    }

    /* ...mode,... */
    h = 9*h + HASH_PTR(get_irn_mode(node));
    /* ...and code */
    h = 9*h + HASH_PTR(get_irn_op(node));
  }

  return h;
}

pset *
new_identities(void) {
  return new_pset(identities_cmp, N_IR_NODES);
}

void
del_identities(pset *value_table) {
  del_pset(value_table);
}

/**
 * Return the canonical node computing the same value as n.
 * Looks up the node in a hash table.
 *
 * For Const nodes this is performed in the constructor, too.  Const
 * nodes are extremely time critical because of their frequent use in
 * constant string arrays.
 */
static INLINE ir_node *
identify (pset *value_table, ir_node *n)
{
  ir_node *o = NULL;

  if (!value_table) return n;

  if (get_opt_reassociation()) {
    if (is_op_commutative(get_irn_op(n))) {
      ir_node *l = get_binop_left(n);
      ir_node *r = get_binop_right(n);

      /* for commutative operators perform  a OP b == b OP a */
      if (l > r) {
        set_binop_left(n, r);
        set_binop_right(n, l);
      }
    }
  }

  o = pset_find (value_table, n, ir_node_hash (n));
  if (!o) return n;

  DBG_OPT_CSE(n, o);

  return o;
}

/**
 * During construction we set the op_pin_state_pinned flag in the graph right when the
 * optimization is performed.  The flag turning on procedure global cse could
 * be changed between two allocations.  This way we are safe.
 */
static INLINE ir_node *
identify_cons (pset *value_table, ir_node *n) {
  ir_node *old = n;

  n = identify(value_table, n);
  if (get_irn_n(old, -1) != get_irn_n(n, -1))
    set_irg_pinned(current_ir_graph, op_pin_state_floats);
  return n;
}

/*
 * Return the canonical node computing the same value as n.
 * Looks up the node in a hash table, enters it in the table
 * if it isn't there yet.
 */
ir_node *
identify_remember (pset *value_table, ir_node *n)
{
  ir_node *o = NULL;

  if (!value_table) return n;

  if (get_opt_reassociation()) {
    if (is_op_commutative(get_irn_op(n))) {
      ir_node *l = get_binop_left(n);
      ir_node *r = get_binop_right(n);

      /* for commutative operators perform  a OP b == b OP a */
      if (l > r) {
        set_binop_left(n, r);
        set_binop_right(n, l);
      }
    }
  }

  /* lookup or insert in hash table with given hash key. */
  o = pset_insert (value_table, n, ir_node_hash (n));

  if (o != n) {
    DBG_OPT_CSE(n, o);
  }

  return o;
}

void
add_identities (pset *value_table, ir_node *node) {
  if (get_opt_cse() && (get_irn_opcode(node) != iro_Block))
    identify_remember (value_table, node);
}

/**
 * garbage in, garbage out. If a node has a dead input, i.e., the
 * Bad node is input to the node, return the Bad node.
 */
static INLINE ir_node *
gigo (ir_node *node)
{
  int i, irn_arity;
  ir_op *op = get_irn_op(node);

  /* remove garbage blocks by looking at control flow that leaves the block
     and replacing the control flow by Bad. */
  if (get_irn_mode(node) == mode_X) {
    ir_node *block = get_nodes_block(skip_Proj(node));

    /* Don't optimize nodes in immature blocks. */
    if (!get_Block_matured(block)) return node;
     /* Don't optimize End, may have Bads. */
    if (op == op_End) return node;

    if (is_Block(block)) {
      irn_arity = get_irn_arity(block);
      for (i = 0; i < irn_arity; i++) {
        if (!is_Bad(get_irn_n(block, i)))
          break;
      }
      if (i == irn_arity) return new_Bad();
    }
  }

  /* Blocks, Phis and Tuples may have dead inputs, e.g., if one of the
     blocks predecessors is dead. */
  if ( op != op_Block && op != op_Phi && op != op_Tuple) {
    irn_arity = get_irn_arity(node);

    /*
     * Beware: we can only read the block of a non-floating node.
     */
    if (is_irn_pinned_in_irg(node) &&
        is_Block_dead(get_nodes_block(node)))
      return new_Bad();

    for (i = 0; i < irn_arity; i++) {
      ir_node *pred = get_irn_n(node, i);

      if (is_Bad(pred))
        return new_Bad();
#if 0
      /* Propagating Unknowns here seems to be a bad idea, because
         sometimes we need a node as a input and did not want that
         it kills it's user.
         However, i might be useful to move this into a later phase
         (it you thing optimizing such code is useful). */
      if (is_Unknown(pred) && mode_is_data(get_irn_mode(node)))
        return new_Unknown(get_irn_mode(node));
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
    if (i == irn_arity) node = new_Bad();
  }
#endif
  return node;
}


/**
 * These optimizations deallocate nodes from the obstack.
 * It can only be called if it is guaranteed that no other nodes
 * reference this one, i.e., right after construction of a node.
 *
 * current_ir_graph must be set to the graph of the node!
 */
ir_node *
optimize_node(ir_node *n)
{
  tarval *tv;
  ir_node *oldn = n;
  opcode iro = get_irn_opcode(n);

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
        ir_type *old_tp = get_irn_type(n);
        int i, arity = get_irn_arity(n);
        int node_size;

        /*
         * Try to recover the type of the new expression.
         */
        for (i = 0; i < arity && !old_tp; ++i)
          old_tp = get_irn_type(get_irn_n(n, i));

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
        edges_node_deleted(n, current_ir_graph);

        /* evaluation was successful -- replace the node. */
        obstack_free(current_ir_graph->obst, n);
        nw = new_Const(get_tarval_mode (tv), tv);

        if (old_tp && get_type_mode(old_tp) == get_tarval_mode (tv))
          set_Const_type(nw, old_tp);
        DBG_OPT_CSTEVAL(oldn, nw);
        return nw;
      }
    }
  }

  /* remove unnecessary nodes */
  if (get_opt_constant_folding() ||
    (iro == iro_Phi)  ||   /* always optimize these nodes. */
    (iro == iro_Id)   ||
    (iro == iro_Proj) ||
    (iro == iro_Block)  )  /* Flags tested local. */
    n = equivalent_node (n);

  optimize_preds(n);                  /* do node specific optimizations of nodes predecessors. */

  /* Common Subexpression Elimination.
   *
   * Checks whether n is already available.
   * The block input is used to distinguish different subexpressions. Right
   * now all nodes are op_pin_state_pinned to blocks, i.e., the CSE only finds common
   * subexpressions within a block.
   */
  if (get_opt_cse())
    n = identify_cons (current_ir_graph->value_table, n);

  if (n != oldn) {
    edges_node_deleted(oldn, current_ir_graph);

    /* We found an existing, better node, so we can deallocate the old node. */
    obstack_free (current_ir_graph->obst, oldn);

    return n;
  }

  /* Some more constant expression evaluation that does not allow to
     free the node. */
  iro = get_irn_opcode(n);
  if (get_opt_constant_folding() ||
    (iro == iro_Cond) ||
    (iro == iro_Proj) ||
    (iro == iro_Sel))     /* Flags tested local. */
    n = transform_node (n);

  /* Remove nodes with dead (Bad) input.
     Run always for transformation induced Bads. */
  n = gigo (n);

  /* Now we have a legal, useful node. Enter it in hash table for CSE */
  if (get_opt_cse() && (get_irn_opcode(n) != iro_Block)) {
    n = identify_remember (current_ir_graph->value_table, n);
  }

  return n;
}


/**
 * These optimizations never deallocate nodes (in place).  This can cause dead
 * nodes lying on the obstack.  Remove these by a dead node elimination,
 * i.e., a copying garbage collection.
 */
ir_node *
optimize_in_place_2 (ir_node *n)
{
  tarval *tv;
  ir_node *oldn = n;
  opcode iro = get_irn_opcode(n);

  if (!get_opt_optimize() && (get_irn_op(n) != op_Phi)) return n;

  /* constant expression evaluation / constant folding */
  if (get_opt_constant_folding()) {
    /* neither constants nor Tuple values can be evaluated */
    if (iro != iro_Const && get_irn_mode(n) != mode_T) {
      /* try to evaluate */
      tv = computed_value(n);
      if (tv != tarval_bad) {
        /* evaluation was successful -- replace the node. */
        ir_type *old_tp = get_irn_type(n);
        int i, arity = get_irn_arity(n);

        /*
         * Try to recover the type of the new expression.
         */
        for (i = 0; i < arity && !old_tp; ++i)
          old_tp = get_irn_type(get_irn_n(n, i));

        n = new_Const(get_tarval_mode(tv), tv);

        if (old_tp && get_type_mode(old_tp) == get_tarval_mode(tv))
          set_Const_type(n, old_tp);

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

  optimize_preds(n);                  /* do node specific optimizations of nodes predecessors. */

  /** common subexpression elimination **/
  /* Checks whether n is already available. */
  /* The block input is used to distinguish different subexpressions.  Right
     now all nodes are op_pin_state_pinned to blocks, i.e., the cse only finds common
     subexpressions within a block. */
  if (get_opt_cse()) {
    n = identify(current_ir_graph->value_table, n);
  }

  /* Some more constant expression evaluation. */
  iro = get_irn_opcode(n);
  if (get_opt_constant_folding() ||
      (iro == iro_Cond) ||
      (iro == iro_Proj) ||
      (iro == iro_Sel))     /* Flags tested local. */
    n = transform_node(n);

  /* Remove nodes with dead (Bad) input.
     Run always for transformation induced Bads.  */
  n = gigo(n);

  /* Now we can verify the node, as it has no dead inputs any more. */
  irn_vrfy(n);

  /* Now we have a legal, useful node. Enter it in hash table for cse.
     Blocks should be unique anyways.  (Except the successor of start:
     is cse with the start block!) */
  if (get_opt_cse() && (get_irn_opcode(n) != iro_Block))
    n = identify_remember(current_ir_graph->value_table, n);

  return n;
}

/**
 * Wrapper for external use, set proper status bits after optimization.
 */
ir_node *
optimize_in_place (ir_node *n)
{
  /* Handle graph state */
  assert(get_irg_phase_state(current_ir_graph) != phase_building);

  if (get_opt_global_cse())
    set_irg_pinned(current_ir_graph, op_pin_state_floats);
  if (get_irg_outs_state(current_ir_graph) == outs_consistent)
    set_irg_outs_inconsistent(current_ir_graph);

  /* FIXME: Maybe we could also test whether optimizing the node can
     change the control graph. */
  set_irg_doms_inconsistent(current_ir_graph);
  return optimize_in_place_2 (n);
}

/*
 * Sets the default operation for an ir_ops.
 */
ir_op_ops *firm_set_default_operations(opcode code, ir_op_ops *ops)
{
  ops = firm_set_default_computed_value(code, ops);
  ops = firm_set_default_equivalent_node(code, ops);
  ops = firm_set_default_transform_node(code, ops);
  ops = firm_set_default_node_cmp_attr(code, ops);
  ops = firm_set_default_get_type(code, ops);
  ops = firm_set_default_get_type_attr(code, ops);
  ops = firm_set_default_get_entity_attr(code, ops);

  return ops;
}
