/*
 * Project:     libFIRM
 * File name:   testprograms/irr_loop_example.c
 * Purpose:     Test Phi construction with irregular control flow.
 * Author:      Christian Schaefer, Goetz Lindenmaier
 * Modified by:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1999-2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

# include <stdio.h>
# include <string.h>

# include "irvrfy.h"
# include "irdump.h"
# include "firm.h"

/**
 *  This file constructs a control flow with an unreachable
 *  loop _and_ an unreachable endless loop.  This looks like:
 *
 *    LoopBlock2			  LoopBlock2'
 *     |    /|\				   |    /|\
 *     |     |               		   |     |
 *    \|/    |     			  \|/    |
 *    LoopBlock1    StartBlock 	          LoopBlock1'
 *        \              /
 *         \            /
 * 	   _\|        |/_
 *          ReturnBlock
 *              |
 *              |
 *             \|/
 *           nextBlock
 *
 *
 **/

int main(int argc, char **argv)
{
  ir_graph *irg;        /* this variable contains the irgraph */
  type     *prim_t_int;
  type     *owner;      /* the class in which this method is defined */
  type     *proc_main;  /* typeinformation for the method main */
  entity   *ent;        /* represents this method as entity of owner */
  ir_node  *returnBlock, *loopBlock1, *loopBlock2, *x, *c1, *c2, *t, *f;


  /* init library */
  init_firm (NULL);
  //set_opt_normalize(0);
  set_opt_constant_folding (0);  /* so that the stupid tests are not optimized. */
  set_opt_cse(1);
  set_opt_dead_node_elimination(1);

  /* FIRM was designed for oo languages where all methods belong to a class.
   * For imperative languages like C we view a file as a large class containing
   * all functions as methods in this file.
   * Therefore we define a class "empty" according to the file name
   * with a method main as an entity.
   */
#define CLASSNAME "DEAD_LOOP"
#define METHODNAME "main"
#define NRARGS 1
#define NRES 0
  printf("\nCreating an IR graph: %s...\n", CLASSNAME);

  prim_t_int = new_type_primitive(id_from_str ("int", 3), mode_Is);

  owner = new_type_class (new_id_from_str (CLASSNAME));
  proc_main = new_type_method(new_id_from_str(METHODNAME), NRARGS, NRES);
  set_method_param_type(proc_main, 0, prim_t_int);
  ent = new_entity (owner, new_id_from_str (METHODNAME), proc_main);
  get_entity_ld_name(ent); /* To enforce name mangling for vcg graph name */

#define NUM_OF_LOCAL_VARS 0

  irg = new_ir_graph (ent, NUM_OF_LOCAL_VARS);

  returnBlock = get_irg_current_block(irg);

  /* Make some real stupid stuff: a data loop (without Phi). */
  {
   ir_node *a, *b, *c, *in[2];
   add_in_edge(get_cur_block(), new_Bad());
   a = new_Const (mode_Is, new_tarval_from_long (1, mode_Is));
   b = new_Const (mode_Is, new_tarval_from_long (2, mode_Is));
   c = new_Add(a, b, mode_Is);
   b = new_Sub(c, b, mode_Is);
   in[0] = b;
   in[1] = new_Bad();
   a = new_Phi(2, in, mode_Is);
   set_Add_left(c, a);
   //add_End_keepalive(get_irg_end(irg), a);
   set_nodes_block(c, new_Bad());
   set_nodes_block(a, new_Bad());
  }

  /* Make the unreachable loop */
  loopBlock1 = new_immBlock();
  loopBlock2 = new_immBlock();
  x = new_Jmp();
  add_in_edge(loopBlock1, x);
  mature_block(loopBlock1);

  switch_block(loopBlock1);
  c1 = new_Const (mode_Is, new_tarval_from_long (1, mode_Is));
  c2 = new_Proj(get_irg_args(irg), mode_Is, 0);
  x =  new_Cond(new_Proj(new_Cmp(c1, c2), mode_b, Eq));
  f = new_Proj(x, mode_X, 0);
  t = new_Proj(x, mode_X, 1);
  add_in_edge(loopBlock2, t);
  add_in_edge(returnBlock, f);
  mature_block(loopBlock2);

  /* Make the unreachable, endless loop */
  loopBlock1 = new_immBlock();
  loopBlock2 = new_immBlock();
  x = new_Jmp();
  add_in_edge(loopBlock1, x);
  mature_block(loopBlock1);

  switch_block(loopBlock1);
  x = new_Jmp();
  add_in_edge(loopBlock2, x);
  add_End_keepalive(get_irg_end(irg), loopBlock1);
  mature_block(loopBlock2);

  /* Make the return block */
  switch_block(returnBlock);
  x = new_Return (get_store(), 0, NULL);
  mature_block (get_irg_current_block(irg));

  add_in_edge (get_irg_end_block(irg), x);
  mature_block (get_irg_end_block(irg));

  finalize_cons (irg);

  //printf("Optimizing ...\n");
  //dead_node_elimination(irg);

  /* verify the graph */
  irg_vrfy(irg);

  printf("Dumping the graph and a control flow graph.\n");
  turn_off_edge_labels();
  dump_keepalive_edges(1);
  dump_consts_local(0);
  dump_ir_graph (irg);
  dump_ir_block_graph (irg);
  dump_cfg (irg);

  printf("Running analyses.\n");
  compute_outs(irg);
  compute_doms(irg);
  construct_backedges(irg);

  printf("Dumping the graph with analyses information.\n");
  dump_file_suffix = "-ana";

  dump_out_edges();
  dump_dominator_information();
  dump_loop_information();
  dump_backedge_information(1);

  dump_ir_graph (irg);
  dump_ir_block_graph (irg);
  dump_cfg (irg);
  dump_loop_tree(irg, dump_file_suffix);

  printf("Optimizing.\n");
  optimize_cf(current_ir_graph);
  local_optimize_graph(current_ir_graph);

  printf("Dumping the optimized graph.\n");
  dump_file_suffix = "-opt";
  dump_ir_graph (irg);
  dump_ir_block_graph (irg);
  dump_cfg (irg);
  dump_loop_tree(irg, dump_file_suffix);

  printf("Use xvcg to view these graphs:\n");
  printf("/ben/goetz/bin/xvcg GRAPHNAME\n\n");

  return (0);
}
