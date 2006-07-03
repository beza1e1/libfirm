/*
 * Project:     libFIRM
 * File name:   ir/opt/funccall.c
 * Purpose:     optimization of function calls
 * Author:      Michael Beck
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1998-2004 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "irvrfy.h"
#include "dbginfo_t.h"
#include "irflag_t.h"
#include "ircons.h"
#include "funccall.h"
#include "irhooks.h"

/**
 * The walker environment for rem_mem_from_const_fkt_calls
 */
typedef struct _env_t {
  int  n_calls_removed_SymConst;
  int  n_calls_removed_Sel;
  ir_node *list;                  /**< The list of all Calls that will be changed. */
  ir_node *proj_list;             /**< list of all potential Proj nones that must be fixed.*/
} env_t;

/**
 * remove memory from const function calls by rerouting
 * it's ProjM and connection the call with a NoMem node.
 *
 * Note: By "const function" we understand a function that did neither
 * read nor write memory. Hence its result depends solely on its
 * arguments.
 */
static void rem_mem_from_const_fkt_calls(ir_node *node, void *env)
{
  env_t *ctx = env;
  ir_node *call, *ptr;
  entity *ent;

  if (get_irn_op(node) == op_Call) {
    call = node;

    set_irn_link(call, NULL);
    ptr = get_Call_ptr(call);
    if (get_irn_op(ptr) == op_SymConst && get_SymConst_kind(ptr) == symconst_addr_ent) {
      ent = get_SymConst_entity(ptr);

      if ((get_entity_additional_properties(ent) & mtp_property_const) == 0)
        return;
      ++ctx->n_calls_removed_SymConst;
    }
    else if (is_Sel(ptr) &&
	     get_irg_callee_info_state(current_ir_graph) == irg_callee_info_consistent) {
      /* If all possible callees are real functions, we can remove the memory edge. */
      int i, n_callees = get_Call_n_callees(call);
      if (n_callees == 0)
        /* This is kind of strange:  dying code or a Call that will raise an exception
	         when executed as there is no implementation to call.  So better not
	         optimize. */
        return;
      for (i = 0; i < n_callees; ++i) {
        ent = get_Call_callee(call, i);
        if (ent == unknown_entity) {
          /* we don't know which entity is called here */
          return;
        }
        if ((get_entity_additional_properties(ent) & mtp_property_const) == 0)
          return;
      }
      ++ctx->n_calls_removed_Sel;
    }
    else
      return;

    /* ok, if we get here we found a call to a const function */
    set_irn_link(call, ctx->list);
    ctx->list = call;
  }
  else if (get_irn_op(node) == op_Proj) {
    /*
     * Collect all memory and exception Proj's from
     * calls.
     */
    call = get_Proj_pred(node);
    if (get_irn_op(call) != op_Call)
      return;

    switch (get_Proj_proj(node)) {
    case pn_Call_M_regular:
    case pn_Call_X_except:
    case pn_Call_M_except:
      set_irn_link(node, ctx->proj_list);
      ctx->proj_list = node;
      break;
    default:
      ;
    }
  }
}

/**
 * Fix the list of collected Calls.
 */
static void fix_call_list(ir_graph *irg, ir_node *call_list, ir_node *proj_list) {
  ir_node *call, *next, *mem, *proj;
  int exc_changed = 0;

  /* fix all calls by removing it's memory input */
  for (call = call_list; call; call = next) {
    next = get_irn_link(call);
    mem  = get_Call_mem(call);

    set_irn_link(call, mem);
    set_Call_mem(call, get_irg_no_mem(irg));

    /* finally, this call can float */
    set_irn_pinned(call, op_pin_state_floats);
    hook_func_call(irg, call);
  }

  /* finally fix all Proj's */
  for (proj = proj_list; proj; proj = next) {
    next = get_irn_link(proj);
    call = get_Proj_pred(proj);
    mem  = get_irn_link(call);
    if (! mem)
      continue;

    switch (get_Proj_proj(proj)) {
    case pn_Call_M_regular: {
      exchange(proj, mem);
    } break;
    case pn_Call_X_except:
    case pn_Call_M_except:
      exc_changed = 1;
      exchange(proj, get_irg_bad(irg));
      break;
    default:
      ;
    }
  }

  /* changes were done ... */
  set_irg_outs_inconsistent(irg);
  set_irg_loopinfo_state(current_ir_graph, loopinfo_cf_inconsistent);

  if (exc_changed) {
    /* ... including exception edges */
    set_irg_doms_inconsistent(irg);
  }
}

/*
 * optimize function calls by handling const functions
 */
void optimize_funccalls(int force_run)
{
  int i, j;
  int change;
  unsigned num_pure = 0;

  if (! get_opt_real_function_call())
    return;

  /* first step: detect, which functions are const, i.e. do NOT touch any memory */
  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    ir_graph *irg  = get_irp_irg(i);
    ir_node *end   = get_irg_end(irg);
    ir_node *endbl = get_nodes_block(end);

    change = 0;

    if (get_irg_additional_properties(irg) & mtp_property_const) {
      /* already marked as a const function */
      ++num_pure;
    }
    else {
      /* visit every Return */
      for (j = get_Block_n_cfgpreds(endbl) - 1; j >= 0; --j) {
        ir_node *node = get_Block_cfgpred(endbl, j);
        ir_op   *op   = get_irn_op(node);
        ir_node *mem;

        /* Bad nodes usually do NOT produce anything, so it's ok */
        if (op == op_Bad)
          continue;

        if (op == op_Return) {
          mem = get_Return_mem(node);

          /* Bad nodes usually do NOT produce anything, so it's ok */
          if (is_Bad(mem))
            continue;

          change = mem != get_irg_initial_mem(irg);
          if (change)
            break;
        }
        else {
          /* exception found */
          change = 1;
          break;
        }
      }

      if (! change) {
        /* check, if a keep-alive exists */
        for (j = get_End_n_keepalives(end) - 1; j >= 0; --j) {
          ir_node *mem = get_End_keepalive(end, j);

          if (mode_M != get_irn_mode(mem))
            continue;

          change = mem != get_irg_initial_mem(irg);
          if (change)
            break;
        }
      }

      if (! change) {
        /* no memory changes found, it's a const function */
        set_irg_additional_property(irg, mtp_property_const);
        ++num_pure;
      }
    }
  }

  if (force_run || num_pure > 0) {
    env_t ctx;

    ctx.n_calls_removed_SymConst = 0;
    ctx.n_calls_removed_Sel = 0;

    /* all calls of pure functions can be transformed into FuncCalls */
    for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
      ir_graph *irg  = get_irp_irg(i);

      /* no need to do this on const functions */
      if ((get_irg_additional_properties(irg) & mtp_property_const) == 0) {
        ctx.list      = NULL;
        ctx.proj_list = NULL;
        irg_walk_graph(irg, NULL, rem_mem_from_const_fkt_calls, &ctx);

        if (ctx.list)
          fix_call_list(irg, ctx.list, ctx.proj_list);
      }
    }

    if (get_firm_verbosity()) {
      printf("Detected %d graphs without side effects.\n", num_pure);
      printf("Optimizes %d(SymConst) + %d(Sel) calls to const functions.\n",
	     ctx.n_calls_removed_SymConst, ctx.n_calls_removed_Sel);
    }
  }
  else {
    if (get_firm_verbosity()) {
      printf("No graphs without side effects detected\n");
    }
  }
}
