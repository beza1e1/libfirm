/*
 * Project:     libFIRM
 * File name:   testprograms/const_eval_example.c
 * Purpose:     Test constant evaluation.
 * Author:      Christian Schaefer, Goetz Lindenmaier
 * Modified by:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 1999-2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

#include <stdio.h>
#include <string.h>

#include <libfirm/firm.h>

/**
*  This file constructs the ir for the following pseudo-program:
*
*  main() {
*    int c, d;
*
*    c = 5 + 7;
*    d = 7 + 5;
*
*    return (c, d);
*  }
**/

int
main(void)
{
  ir_type     *prim_t_int;
  ir_graph *irg;
  ir_type *owner;
  ir_type *method;    /* the ir_type of this method */
  ir_entity *ent;
  ir_node *a, *b, *c, *d, *x;

  printf("\nCreating an IR graph: CONST_EVAL_EXAMPLE...\n");

  init_firm(NULL);

  /*** Make basic ir_type information for primitive ir_type int. ***/
  prim_t_int = new_type_primitive(new_id_from_chars("int", 3), mode_Is);

  /* Try both optimizations: */
  set_opt_constant_folding(1);
  set_opt_cse(1);

  owner = new_type_class(new_id_from_chars("CONST_EVAL_EXAMPLE", 18));
  method = new_type_method(new_id_from_chars("main", 4), 0, 2);
  set_method_res_type(method, 0, prim_t_int);
  set_method_res_type(method, 1, prim_t_int);
  ent = new_entity(owner, new_id_from_chars("main", 4), method);
  get_entity_ld_name(ent);

  irg = new_ir_graph(ent, 4);

  a = new_Const(mode_Is, new_tarval_from_long(7, mode_Is));
  b = new_Const(mode_Is, new_tarval_from_long(5, mode_Is));

  x = new_Jmp();
  mature_immBlock(get_irg_current_block(irg));

  /*  To test const eval on DivMod
  c = new_DivMod(get_store(), a, b);
  set_store(new_Proj(c, mode_M, pn_DivMod_M));
  d = new_Proj(c, mode_Is, pn_DivMod_res_mod);
  c = new_Proj(c, mode_Is, pn_DivMod_res_div);
  */

  c = new_Add(new_Const(mode_Is, new_tarval_from_long(5, mode_Is)),
	       new_Const(mode_Is, new_tarval_from_long(7, mode_Is)),
	       mode_Is);
  d = new_Add(new_Const(mode_Is, new_tarval_from_long(7, mode_Is)),
	       new_Const(mode_Is, new_tarval_from_long(5, mode_Is)),
	       mode_Is);

  {
     ir_node *in[2];
     in[0] = c;
     in[1] = d;

     x = new_Return(get_store(), 2, in);
  }

  add_immBlock_pred(get_irg_end_block(irg), x);
  mature_immBlock(get_irg_end_block(irg));

  irg_finalize_cons(irg);

  printf("Optimizing ...\n");
  dead_node_elimination(irg);

  /* verify the graph */
  irg_vrfy(irg);

  printf("Done building the graph.  Dumping it.\n");
  dump_ir_block_graph(irg, 0);

  printf("Use ycomp to view this graph:\n");
  printf("ycomp GRAPHNAME\n\n");

  return 0;
}
