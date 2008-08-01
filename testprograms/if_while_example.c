/*
 * Project:     libFIRM
 * File name:   testprograms/if_while_example.c
 * Purpose:     Shows more complex control flow.
 * Author:      Christian Schaefer, Goetz Lindenmaier
 * Modified by:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1999-2003 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */
#include <stdio.h>
#include <string.h>

#include <libfirm/firm.h>

/**
*  This file constructs the ir for the following pseudo-program:
*
*  main() {
*    int a = 0;         //  pos 0
*    int b = 1;         //  pos 1
*    int h;             //  pos 2
*
*    if (0 == 0)
*      { a = 2; }
*
*    while (0 == 0) loop {
*      h = a;
*      a = b;
*      b = h;
*    }
*
*    return a-b;
*  }
**/

int
main(void)
{
  ir_graph *irg;
  ir_type *owner;
  ir_type *proc_main;
  ir_type *prim_t_int;
  ir_entity *ent;
  ir_node *b, *x, *r, *t, *f;

  printf("\nCreating an IR graph: IF_WHILE_EXAMPLE...\n");

  init_firm (NULL);
  turn_off_edge_labels();

  set_optimize(1);
  set_opt_constant_folding(0);  /* so that the stupid tests are not optimized. */
                                /* if optimized no path to End remains!! */
  set_opt_cse(1);

  /*** Make basic ir_type information for primitive ir_type int. ***/
  prim_t_int = new_type_primitive(new_id_from_chars ("int", 3), mode_Iu);

#define METHODNAME "main"
#define NRARGS 0
#define NRES 1

  proc_main = new_type_method(new_id_from_chars(METHODNAME, strlen(METHODNAME)),
                              NRARGS, NRES);
  set_method_res_type(proc_main, 0, prim_t_int);
  owner = new_type_class (new_id_from_chars ("IF_WHILE_EXAMPLE", 16));
  ent = new_entity (owner, new_id_from_chars ("main", 4), proc_main);
  get_entity_ld_name(ent);

  /* Generates start and end blocks and nodes and a first, initial block */
  irg = new_ir_graph (ent, 4);

  /* Generate two constants */
  set_value(0, new_Const(mode_Iu, new_tarval_from_long(0, mode_Iu)));
  set_value(1, new_Const(mode_Iu, new_tarval_from_long(1, mode_Iu)));
  mature_immBlock(get_irg_current_block(irg));

  /* Generate a conditional branch */
  x = new_Jmp();

  /* generate the fall through block and add all cfg edges */
  r = new_immBlock();
  add_immBlock_pred(r, x);
  mature_immBlock(r);
  x = new_Jmp();

  /* generate a block for the loop header and the conditional branch */
  r = new_immBlock();
  add_immBlock_pred(r, x);
  x = new_Cond(new_Proj(new_Cmp(new_Const(mode_Iu, new_tarval_from_long(0, mode_Iu)),
                                new_Const(mode_Iu, new_tarval_from_long(0, mode_Iu))),
                        mode_b, pn_Cmp_Eq));
  f = new_Proj(x, mode_X, pn_Cond_false);
  t = new_Proj(x, mode_X, pn_Cond_true);

  /* generate the block for the loop body */
  b = new_immBlock();
  add_immBlock_pred(b,t);
  x = new_Jmp();
  add_immBlock_pred(r, x);
  mature_immBlock(r);

  /* the code in the loop body,
     as we are dealing with local variables only the dataflow edges
     are manipulated */
  set_value(2, get_value(0, mode_Iu));
  set_value(0, get_value(1, mode_Iu));
  set_value(1, get_value(2, mode_Iu));
  mature_immBlock(b);

  /* generate the return block */
  r = new_immBlock();
  add_immBlock_pred(r, f);
  mature_immBlock(r);

  {
     ir_node *in[1];
     in[0] = new_Sub(get_value(0, mode_Iu), get_value(1, mode_Iu), mode_Iu);

     x = new_Return(get_store(), 1, in);
  }

  /* finalize the end block generated in new_ir_graph() */
  add_immBlock_pred(get_irg_end_block(irg), x);
  mature_immBlock(get_irg_end_block(irg));

  irg_finalize_cons(irg);

  printf("Optimizing ...\n");

  local_optimize_graph(irg);
  dead_node_elimination(irg);

  compute_irg_outs(irg);

  /* verify the graph */
  irg_vrfy(irg);

  /* output the vcg file */
  printf("Done building the graph.  Dumping it with out-edges.\n");
  dump_out_edges(1);
  dump_ir_graph(irg, 0);
  printf("Use ycomp to view this graph:\n");
  printf("ycomp GRAPHNAME\n\n");

  return 0;
}
