/*
 * Project:     libFIRM
 * File name:   testprograms/oo_program_example.c
 * Purpose:     A complex example.
 * Author:      Goetz Lindenmaier
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
*
*  class PRIMA {
*    a: int;
*
*    int c(d: int) {
*      return (d + self.a);
*    }
*
*    void set_a(e:int) {
*      self.a = e;
*    }
*
*  }
*
*  int main() {
*    o: PRIMA;
*    o = new PRIMA;
*    o.set_a(2);
*    return o.c(5);
*  };
*
**/

int
main(void)
{
  ir_type     *prim_t_int;
  ir_type     *owner, *class_prima;
  ir_type     *proc_main, *proc_set_a, *proc_c;
  ir_type     *class_p_ptr;
  ir_entity   *proc_main_e, *proc_set_a_e, *proc_c_e, *a_e;

  ir_graph     *main_irg, *set_a_irg, *c_irg;
  ir_node      *c2, *c5, *obj_o, *obj_size, *proc_ptr, *call, *res, *x;
  ir_node      *self, *par1, *a_ptr;
  ir_node      *a_val;
  symconst_symbol sym;
  ir_entity **free_methods;
  int arr_len;

  int o_pos, self_pos, e_pos;

  int i;

  init_firm(NULL);

  set_opt_constant_folding(1);
  set_opt_cse(1);

  /*** Make basic ir_type information for primitive ir_type int. ***/
  prim_t_int = new_type_primitive(new_id_from_chars("int", 3), mode_Is);

  /*** Make ir_type information for the class (PRIMA). ***/
  /* The ir_type of the class */
  class_prima = new_type_class(new_id_from_chars("PRIMA", 5));
  /* We need ir_type information for pointers to the class: */
  class_p_ptr = new_type_pointer(new_id_from_chars("class_prima_ptr", 15),
				  class_prima, mode_P);
  /* An ir_entity for the field (a).  The ir_entity constructor automatically adds
     the ir_entity as member of the owner. */
  a_e = new_entity(class_prima, new_id_from_chars("a", 1), prim_t_int);
  /* An ir_entity for the method set_a.  But first we need ir_type information
     for the method. */
  proc_set_a = new_type_method(new_id_from_chars("set_a", 5), 2, 0);
  set_method_param_type(proc_set_a, 0, class_p_ptr);
  set_method_param_type(proc_set_a, 1, prim_t_int);
  proc_set_a_e = new_entity(class_prima, new_id_from_chars("set_a", 5), proc_set_a);
  /* An ir_entity for the method c. Implicit argument "self" must be modeled
     explicit! */
  proc_c   = new_type_method(new_id_from_chars("c", 1 ), 2, 1);
  set_method_param_type(proc_c, 0, class_p_ptr);
  set_method_param_type(proc_c, 1, prim_t_int);
  set_method_res_type(proc_c, 0, prim_t_int);
  proc_c_e = new_entity(class_prima, new_id_from_chars("c", 1), proc_c);

  /*** Now build procedure main. ***/
  /** Type information for main. **/
  printf("\nCreating an IR graph: OO_PROGRAM_EXAMPLE...\n");
  /* Main is not modeled as part of an explicit class here. Therefore the
     owner is the global ir_type. */
  owner = get_glob_type();
  /* Main has zero parameters and one result. */
  proc_main = new_type_method(new_id_from_chars("OO_PROGRAM_EXAMPLE_main", 23), 0, 1);
  /* The result ir_type is int. */
  set_method_res_type(proc_main, 0, prim_t_int);

  /* The ir_entity for main. */
  proc_main_e = new_entity(owner, new_id_from_chars("OO_PROGRAM_EXAMPLE_main", 23), proc_main);

  /** Build code for procedure main. **/
  /* We need one local variable (for "o"). */
  main_irg = new_ir_graph(proc_main_e, 1);
  o_pos = 0;

  /* Remark that this irg is the main routine of the program. */
  set_irp_main_irg(main_irg);

  /* Make the constants.  They are independent of a block. */
  c2 = new_Const(mode_Is, new_tarval_from_long(2, mode_Is));
  c5 = new_Const(mode_Is, new_tarval_from_long(5, mode_Is));

  /* There is only one block in main, it contains the allocation and the calls. */
  /* Allocate the defined object and generate the ir_type information. */
  sym.type_p = class_prima;
  obj_size = new_SymConst(mode_Iu, sym, symconst_type_size);
  obj_o    = new_Alloc(get_store(), obj_size, class_prima, heap_alloc);
  set_store(new_Proj(obj_o, mode_M, pn_Alloc_M));  /* make the changed memory visible */
  obj_o    = new_Proj(obj_o, mode_P, pn_Alloc_res);  /* remember the pointer to the object */
  set_value(o_pos, obj_o);

  /* Get the pointer to the procedure from the object.  */
  proc_ptr = new_simpleSel(get_store(),             /* The memory containing the object. */
			   get_value(o_pos, mode_P),/* The pointer to the object. */
			   proc_set_a_e );            /* The feature to select. */

  /* Call procedure set_a, first built array with parameters. */
  {
    ir_node *in[2];
    in[0] = get_value(o_pos, mode_P);
    in[1] = c2;
    call = new_Call(get_store(), proc_ptr, 2, in, proc_set_a);
  }
  /* Make the change to memory visible.  There are no results.  */
  set_store(new_Proj(call, mode_M, pn_Call_M));

  /* Get the pointer to the nest procedure from the object. */
  proc_ptr = new_simpleSel(get_store(), get_value(o_pos, mode_P), proc_c_e);

  /* call procedure c, first built array with parameters */
  {
    ir_node *in[2];
    in[0] = get_value(o_pos, mode_P);
    in[1] = c5;
    call = new_Call(get_store(), proc_ptr, 2, in, proc_c);
  }
  /* make the change to memory visible */
  set_store(new_Proj(call, mode_M, pn_Call_M));
  /* Get the result of the procedure: select the result tuple from the call,
     then the proper result from the tuple. */
  res = new_Proj(new_Proj(call, mode_T, pn_Call_T_result), mode_Is, 0);

  /* return the results of procedure main */
  {
     ir_node *in[1];
     in[0] = res;
     x = new_Return(get_store(), 1, in);
  }
  mature_immBlock(get_irg_current_block(main_irg));

  /* complete the end_block */
  add_immBlock_pred(get_irg_end_block(main_irg), x);
  mature_immBlock(get_irg_end_block(main_irg));

  irg_vrfy(main_irg);
  irg_finalize_cons(main_irg);

  /****************************************************************************/

  printf("Creating IR graph for set_a: \n");

  /* Local variables: self, e */
  set_a_irg = new_ir_graph(proc_set_a_e, 2);
  self_pos = 0; e_pos = 1;

  /* get the procedure parameter */
  self = new_Proj(get_irg_args(set_a_irg), mode_P, 0);
  set_value(self_pos, self);
  par1 = new_Proj(get_irg_args(set_a_irg), mode_Is, 1);
  set_value(e_pos, par1);
  /* Create and select the ir_entity to set */
  a_ptr = new_simpleSel(get_store(), self, a_e);
  /* perform the assignment */
  set_store(new_Proj(new_Store(get_store(), a_ptr, par1), mode_M, pn_Store_M));

  /* return nothing */
  x = new_Return(get_store(), 0, NULL);
  mature_immBlock(get_irg_current_block(set_a_irg));

  /* complete the end_block */
  add_immBlock_pred(get_irg_end_block(set_a_irg), x);
  mature_immBlock(get_irg_end_block(set_a_irg));

  /* verify the graph */
  irg_vrfy(set_a_irg);
  irg_finalize_cons(set_a_irg);

  /****************************************************************************/

  printf("Creating IR graph for c: \n");

  /* Local variables self, d */
  c_irg = new_ir_graph(proc_c_e, 2);

  /* get the procedure parameter */
  self = new_Proj(get_irg_args(c_irg), mode_P, 0);
  par1 = new_Proj(get_irg_args(c_irg), mode_Is, 1);

  /* Select the ir_entity and load the value */
  a_ptr = new_simpleSel(get_store(), self, a_e);
  a_val = new_Load(get_store(), a_ptr, mode_Is);
  set_store(new_Proj(a_val, mode_M, pn_Load_M));
  a_val = new_Proj(a_val, mode_Is, pn_Load_res);

  /* return the result */
  {
    ir_node *in[1];
    in[0] = new_Add(par1, a_val, mode_Is);

    x = new_Return(get_store(), 1, in);
  }
  mature_immBlock(get_irg_current_block(c_irg));

  /* complete the end_block */
  add_immBlock_pred(get_irg_end_block(c_irg), x);
  mature_immBlock(get_irg_end_block(c_irg));

  /* verify the graph */
  irg_vrfy(c_irg);
  irg_finalize_cons(c_irg);

  /****************************************************************************/

  printf("Optimizing ...\n");
  for (i = 0; i < get_irp_n_irgs(); i++) {
    local_optimize_graph(get_irp_irg(i));
    dead_node_elimination(get_irp_irg(i));
  }

  printf("Dumping graphs of all procedures and a ir_type graph.\n");
  /* Touch ld names to distinguish names from oo_inline names. */
  get_entity_ld_ident(proc_set_a_e);
  get_entity_ld_ident(proc_c_e);

  dump_consts_local(1);
  turn_off_edge_labels();

  dump_all_ir_graphs(dump_ir_graph, "");
  dump_all_ir_graphs(dump_ir_block_graph, "");
  dump_all_ir_graphs(dump_ir_graph_w_types, "");
  dump_all_ir_graphs(dump_ir_block_graph_w_types, "");
  dump_all_ir_graphs(dump_type_graph, "");
  dump_all_ir_graphs(dump_graph_as_text, "");
  dump_all_types("");
  dump_class_hierarchy(1, "");

  cgana(&arr_len, &free_methods);
#ifdef INTERPROCEDURAL_VIEW
  cg_construct(arr_len, free_methods);

  set_interprocedural_view(1);
#endif
  dump_ir_graph(main_irg, "");
  dump_ir_block_graph(main_irg, "");
  dump_ir_graph_w_types(main_irg, "");
  dump_ir_block_graph_w_types(main_irg, "");
#ifdef INTERPROCEDURAL_VIEW
  dump_all_cg_block_graph("");
#endif

  printf("Use ycomp to view these graphs:\n");
  printf("ycomp GRAPHNAME\n\n");
  return 0;
}
