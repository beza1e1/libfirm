/* Copyright (C) 1998 - 2000 by Universitaet Karlsruhe
* All rights reserved.
*
* Authors: Christian Schaefer, Goetz Lindenmaier
*
* testprogram.
*/

# include <stdio.h>
# include <string.h>

# include "irvrfy.h"
# include "irdump.h"
# include "firm.h"

/*
 *   a dead block / unreachable code.
 */

/**
*  This file constructs a control flow of following shape:
*
*
*       firstCondBlock
*          /     \
*         /       \
*       |/_       _\|
*     Block1    scnCondBlock
*        |       |        |
* 	   |       |        |
* 	   |      \ /      \ /
*        |     Block2   Block3
*         \      |       /
*      \     |      /
*      _\|  \ /   |/_
*            nextBlock
*
*
*   This is a program as, e.g.,
*
*   if () then
*     { Jmp label1; } // happens anyways
*   else
*     { Jmp label1; } // happens anyways
* label1:
*   return();
*   Jmp label1;
*
**/

int main(int argc, char **argv)
{
  type     *prim_t_int;
  ir_graph *irg;          /* this variable contains the irgraph */
  type *owner;      /* the class in which this method is defined */
  type *proc_main; /* type information for the method main */
  entity *ent;            /* represents this method as entity of owner */
  ir_node *c1, *c2, *cond, *f, *t, *endBlock, *Block1, *jmp,
          *scndCondBlock, *Block2, *Block3, *x;

  /* init library */
  init_firm (NULL);

  set_optimize(1);

  /*** Make basic type information for primitive type int. ***/
  prim_t_int = new_type_primitive(id_from_str ("int", 3), mode_Is);

  /* FIRM was designed for oo languages where all methods belong to a class.
   * For imperative languages like C we view a file as a large class containing
   * all functions as methods in this file.
   * This class now is automatically generated.
   */
#define METHODNAME "THREE_CFPRED_EXAMPLE_main"
#define NRARGS 1
#define NRES 1
  printf("\nCreating an IR graph: ...\n");

  owner = get_glob_type();
  proc_main = new_type_method(id_from_str(METHODNAME, strlen(METHODNAME)),
			      NRARGS, NRES);
  set_method_param_type(proc_main, 0, prim_t_int);
  set_method_res_type(proc_main, 0, prim_t_int);

  ent = new_entity (owner,
                    id_from_str (METHODNAME, strlen(METHODNAME)),
                    proc_main);

#define NUM_OF_LOCAL_VARS 2

  irg = new_ir_graph (ent, NUM_OF_LOCAL_VARS);

  /* to make a condition  */
  c1 = new_Const (mode_Is, new_tarval_from_long (1, mode_Is));
  c2 = new_Proj (get_irg_args(irg), mode_Is, 0);
  set_value(1, c2);

  cond = new_Cond(new_Proj(new_Cmp(c1, c2), mode_b, Eq));
  set_value(0, new_Const (mode_Is, new_tarval_from_long (6, mode_Is)));
  f = new_Proj(cond, mode_X, 0);
  t = new_Proj(cond, mode_X, 1);
  mature_block(get_irg_current_block(irg));

  /* end block to add jmps */
  endBlock = new_immBlock();

  /* Block 1 */
  Block1 = new_immBlock();
  add_in_edge(Block1, t);
  mature_block(Block1);
  set_value(0, new_Const (mode_Is, new_tarval_from_long (5, mode_Is)));
  jmp = new_Jmp();
  add_in_edge(endBlock, jmp);

  /* scndCondBlock */
  scndCondBlock = new_immBlock();
  add_in_edge(scndCondBlock, f);
  mature_block(scndCondBlock);
  c1 = new_Const (mode_Is, new_tarval_from_long (3, mode_Is));
  cond = new_Cond(new_Proj(new_Cmp(c1, get_value(1, mode_Is)), mode_b, Eq));
  f = new_Proj(cond, mode_X, 0);
  t = new_Proj(cond, mode_X, 1);
  mature_block(get_irg_current_block(irg));

  /* Block 2 */
  Block2 = new_immBlock();
  add_in_edge(Block2, f);
  mature_block(Block2);
  jmp = new_Jmp();
  add_in_edge(endBlock, jmp);

  /* Block 3 */
  Block3 = new_immBlock();
  add_in_edge(Block3, t);
  mature_block(Block3);
  jmp = new_Jmp();
  add_in_edge(endBlock, jmp);

  /* finish the end Block */
  switch_block(endBlock);
  {
    ir_node *in[1];
    in[0] = get_value(0, mode_Is);
    x = new_Return (get_store(), 1, in);
  }
  mature_block (get_irg_current_block(irg));

  /* finish the Block with the end node */
  add_in_edge (get_irg_end_block(irg), x);
  mature_block (get_irg_end_block(irg));

  /* verify the graph */
  irg_vrfy(irg);
  finalize_cons (irg);

  printf("Optimizing ...\n");
  dead_node_elimination(irg);

  printf("Dumping the graph and a control flow graph.\n");
  dump_ir_block_graph (irg);
  dump_cfg (irg);
  printf("Use xvcg to view these graphs:\n");
  printf("/ben/goetz/bin/xvcg GRAPHNAME\n\n");

  return (0);
}
