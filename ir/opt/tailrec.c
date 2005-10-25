/*
 * Project:     libFIRM
 * File name:   ir/opt/tailrec.c
 * Purpose:     tail-recursion optimization
 * Author:      Michael Beck
 * Modified by:
 * Created:     08.06.2004
 * CVS-ID:      $Id$
 * Copyright:   (c) 2002-2004 Universit�t Karlsruhe
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

#include <assert.h>
#include "tailrec.h"
#include "array.h"
#include "irprog_t.h"
#include "irgwalk.h"
#include "irgmod.h"
#include "irop.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "ircons.h"
#include "irflag.h"
#include "trouts.h"
#include "return.h"
#include "scalar_replace.h"
#include "irouts.h"
#include "irhooks.h"

/**
 * the environment for collecting data
 */
typedef struct _collect_t {
  ir_node *proj_X;      /**< initial exec proj */
  ir_node *block;       /**< old first block */
  int     blk_idx;      /**< cfgpred index of the initial exec in block */
  ir_node *proj_m;      /**< linked list of memory from start proj's */
  ir_node *proj_data;   /**< linked list of all parameter access proj's */
} collect_t;

/**
 * walker for collecting data, fills a collect_t environment
 */
static void collect_data(ir_node *node, void *env)
{
  collect_t *data = env;
  ir_node *pred;
  ir_op *op;

  switch (get_irn_opcode(node)) {
  case iro_Proj:
    pred = get_Proj_pred(node);

    op = get_irn_op(pred);
    if (op == op_Proj) {
      ir_node *start = get_Proj_pred(pred);

      if (get_irn_op(start) == op_Start) {
        if (get_Proj_proj(pred) == pn_Start_T_args) {
	        /* found Proj(ProjT(Start)) */
	        set_irn_link(node, data->proj_data);
	        data->proj_data = node;
        }
      }
    }
    else if (op == op_Start) {
      switch (get_Proj_proj(node)) {
        case pn_Start_M:
          /* found ProjM(Start) */
	      set_irn_link(node, data->proj_m);
	      data->proj_m = node;
	      break;
	    case pn_Start_X_initial_exec:
	      /* found ProjX(Start) */
	      data->proj_X = node;
	      break;
	    default:
	      break;
      }
    }
    break;
  case iro_Block: {
    int i, n_pred = get_Block_n_cfgpreds(node);

    /*
     * the first block has the initial exec as cfg predecessor
     */
    if (node != current_ir_graph->start_block) {
      for (i = 0; i < n_pred; ++i) {
        if (get_Block_cfgpred(node, i) == data->proj_X) {
	        data->block   = node;
	        data->blk_idx = i;
	        break;
        }
      }
    }
    break;
  }
  default:
    break;
  }
}

/**
 * do the graph reconstruction for tail-recursion elimination
 *
 * @param irg           the graph that will reconstructed
 * @param rets          linked list of all rets
 * @param n_tail_calls  number of tail-recursion calls
 */
static void do_opt_tail_rec(ir_graph *irg, ir_node *rets, int n_tail_calls)
{
  ir_node *end_block = get_irg_end_block(irg);
  ir_node *block, *jmp, *call, *calls;
  ir_node **in;
  ir_node **phis;
  ir_node ***call_params;
  ir_node *p;
  int i, j, n_params;
  collect_t data;
  int rem         = get_optimize();
  entity *ent     = get_irg_entity(irg);
  type *method_tp = get_entity_type(ent);

  assert(n_tail_calls);

  /* we add new nodes, so the outs are inconsistant */
  set_irg_outs_inconsistent(irg);

  /* we add new blocks and change the control flow */
  set_irg_dom_inconsistent(irg);

  /* we add a new loop */
  set_irg_loopinfo_inconsistent(irg);

  /* calls are removed */
  set_trouts_inconsistent();

  /* we must build some new nodes WITHOUT CSE */
  set_optimize(0);

  /* collect needed data */
  data.proj_X    = NULL;
  data.block     = NULL;
  data.blk_idx   = -1;
  data.proj_m    = NULL;
  data.proj_data = NULL;
  irg_walk_graph(irg, NULL, collect_data, &data);

  /* check number of arguments */
  call = get_irn_link(end_block);
  n_params = get_Call_n_params(call);

  assert(data.proj_X && "Could not find initial exec from Start");
  assert(data.block  && "Could not find first block");
  assert(data.proj_m && "Could not find ProjM(Start)");
  assert((data.proj_data || n_params == 0) && "Could not find Proj(ProjT(Start)) of non-void function");

  /* allocate in's for phi and block construction */
  NEW_ARR_A(ir_node *, in, n_tail_calls + 1);

  in[0] = data.proj_X;

  /* turn Return's into Jmp's */
  for (i = 1, p = rets; p; p = get_irn_link(p)) {
    ir_node *jmp;
    ir_node *block = get_nodes_block(p);

    jmp = new_r_Jmp(irg, block);

    exchange(p, new_r_Bad(irg));
    in[i++] = jmp;

    /* we might generate an endless loop, so add
     * the block to the keep-alive list */
    add_End_keepalive(get_irg_end(irg), block);
  }

  /* create a new block at start */
  block = new_r_Block(irg, n_tail_calls + 1, in);
  jmp   = new_r_Jmp(irg, block);

  /* the old first block is now the second one */
  set_Block_cfgpred(data.block, data.blk_idx, jmp);

  /* allocate phi's, position 0 contains the memory phi */
  NEW_ARR_A(ir_node *, phis, n_params + 1);

  /* build the memory phi */
  i = 0;
  in[i] = new_r_Proj(irg, get_irg_start_block(irg), get_irg_start(irg), mode_M, pn_Start_M);
  ++i;

  for (calls = call; calls; calls = get_irn_link(calls)) {
    in[i] = get_Call_mem(calls);
    ++i;
  }
  assert(i == n_tail_calls + 1);

  phis[0] = new_r_Phi(irg, block, n_tail_calls + 1, in, mode_M);

  /* build the data phi's */
  if (n_params > 0) {
    ir_node *calls;

    NEW_ARR_A(ir_node **, call_params, n_tail_calls);

    /* collect all parameters */
    for (i = 0, calls = call; calls; calls = get_irn_link(calls)) {
      call_params[i] = get_Call_param_arr(calls);
      ++i;
    }

    /* build new projs and Phi's */
    for (i = 0; i < n_params; ++i) {
      ir_mode *mode = get_type_mode(get_method_param_type(method_tp, i));

      in[0] = new_r_Proj(irg, block, irg->args, mode, i);
      for (j = 0; j < n_tail_calls; ++j)
        in[j + 1] = call_params[j][i];

      phis[i + 1] = new_r_Phi(irg, block, n_tail_calls + 1, in, mode);
    }
  }

  /*
   * ok, we are here, so we have build and collected all needed Phi's
   * now exchange all Projs into links to Phi
   */
  for (p = data.proj_m; p; p = get_irn_link(p)) {
    exchange(p, phis[0]);
  }
  for (p = data.proj_data; p; p = get_irn_link(p)) {
    long proj = get_Proj_proj(p);

    assert(0 <= proj && proj < n_params);
    exchange(p, phis[proj + 1]);
  }

  /* tail recursion was done, all info is invalid */
  set_irg_dom_inconsistent(irg);
  set_irg_outs_inconsistent(irg);
  set_irg_loopinfo_state(current_ir_graph, loopinfo_cf_inconsistent);
  set_trouts_inconsistent();
  set_irg_callee_info_state(irg, irg_callee_info_inconsistent);

  set_optimize(rem);
}

/**
 * Check the lifetime of locals in the given graph.
 * Tail recursion can only be done, if we can prove that
 * the lifetime of locals end with the recursive call.
 * We do this by checking that no address of a local variable is
 * stored or transmitted as an argument to a call.
 *
 * @return non-zero if it's ok to do tail recursion
 */
static int check_lifetime_of_locals(ir_graph *irg)
{
  ir_node *irg_frame = get_irg_frame(irg);
  int i;

  if (get_irg_outs_state(irg) != outs_consistent)
    compute_irg_outs(irg);

  for (i = get_irn_n_outs(irg_frame) - 1; i >= 0; --i) {
    ir_node *succ = get_irn_out(irg_frame, i);

    if (get_irn_op(succ) == op_Sel && is_address_taken(succ))
      return 0;
  }
  return 1;
}

/*
 * convert simple tail-calls into loops
 */
int opt_tail_rec_irg(ir_graph *irg)
{
  ir_node *end_block;
  int i, n_tail_calls = 0;
  ir_node *rets = NULL;
  type *mtd_type, *call_type;

  if (! get_opt_tail_recursion() || ! get_opt_optimize())
    return 0;

  if (! check_lifetime_of_locals(irg))
    return 0;

  /*
   * This tail recursion optimization works best
   * if the Returns are normalized.
   */
  normalize_n_returns(irg);

  end_block = get_irg_end_block(irg);
  set_irn_link(end_block, NULL);

  for (i = get_Block_n_cfgpreds(end_block) - 1; i >= 0; --i) {
    ir_node *ret = get_Block_cfgpred(end_block, i);
    ir_node *call, *call_ptr;
    entity *ent;
    int j;
    ir_node **ress;

    /* search all returns of a block */
    if (get_irn_op(ret) != op_Return)
      continue;

    /* check, if it's a Return self() */
    call = skip_Proj(get_Return_mem(ret));
    if (get_irn_op(call) != op_Call)
      continue;

    /* check if it's a recursive call */
    call_ptr = get_Call_ptr(call);

    if (get_irn_op(call_ptr) != op_SymConst)
      continue;

    if (get_SymConst_kind(call_ptr) != symconst_addr_ent)
      continue;

    ent = get_SymConst_entity(call_ptr);
    if (!ent || get_entity_irg(ent) != irg)
      continue;

    /* ok, mem is routed to a recursive call, check return args */
    ress = get_Return_res_arr(ret);
    for (j = get_Return_n_ress(ret) - 1; j >= 0; --j) {
      ir_node *irn = skip_Proj(skip_Proj(ress[j]));

      if (irn != call) {
	      /* not routed to a call */
	      break;
      }
    }
    if (j >= 0)
      continue;

    /*
     * Check, that the types match. At least in C
     * this might fail.
     */
    mtd_type  = get_entity_type(ent);
    call_type = get_Call_type(call);

    if (mtd_type != call_type) {
      /*
       * Hmm, the types did not match, bad.
       * This can happen in C when no prototype is given
       * or K&R style is used.
       */
#if 0
      printf("Warning: Tail recursion fails because of different method and call types:\n");
      dump_type(mtd_type);
      dump_type(call_type);
#endif
      return 0;
    }

    /* here, we have found a call */
    set_irn_link(call, get_irn_link(end_block));
    set_irn_link(end_block, call);
    ++n_tail_calls;

    /* link all returns, we will need this */
    set_irn_link(ret, rets);
    rets = ret;
  }

  /* now, end_block->link contains the list of all tail calls */
  if (! n_tail_calls)
    return 0;

  if (get_opt_tail_recursion_verbose() && get_firm_verbosity() > 1)
    printf("  Performing tail recursion for graph %s and %d Calls\n",
	   get_entity_ld_name(get_irg_entity(irg)), n_tail_calls);

  hook_tail_rec(irg, n_tail_calls);
  do_opt_tail_rec(irg, rets, n_tail_calls);

  return n_tail_calls;
}

/*
 * optimize tail recursion away
 */
void opt_tail_recursion(void)
{
  int i;
  int n_opt_applications = 0;
  ir_graph *irg;

  if (! get_opt_tail_recursion() || ! get_opt_optimize())
    return;

  for (i = get_irp_n_irgs() - 1; i >= 0; --i) {
    irg = get_irp_irg(i);

    if (opt_tail_rec_irg(irg))
      ++n_opt_applications;
  }

  if (get_opt_tail_recursion_verbose())
    printf("Performed tail recursion for %d of %d graphs\n", n_opt_applications, get_irp_n_irgs());
}
