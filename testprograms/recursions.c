/*
 * Project:     libFIRM
 * File name:   testprograms/recursion.c
 * Purpose:     Empty methods that recur.
 * Author:      Goetz Lindenmaier
 * Modified by:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 2004 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

#include <stdio.h>
#include <string.h>

#include <libfirm/firm.h>

/**
*

*
**/

static ir_graph *make_method(char *name, int n_locs) {
  ir_type *proc_t   = new_type_method(new_id_from_str(name), 0, 0);
  /*set_method_param_type(proc_set_a, 0, class_p_ptr);*/
  /*set_method_param_type(proc_set_a, 1, prim_t_int);*/
  ir_entity *proc_e = new_entity(get_glob_type(), new_id_from_str(name), proc_t);
  return new_ir_graph(proc_e, n_locs);
}


static ir_node *make_Call(ir_graph *c, int n_args, ir_node **args) {
  ir_entity *ent = get_irg_entity(c);
  ir_type *mtp = get_entity_type(ent);
  ir_node *addr;
  ir_node *call;
  symconst_symbol sym;
  sym.entity_p = ent;
  addr = new_SymConst(mode_P, sym, symconst_addr_ent);
  call = new_Call(get_store(), addr, n_args, args, mtp);
  set_store(new_Proj(call, mode_M, pn_Call_M_regular));
  if (get_method_n_ress(mtp) == 1) {
    ir_type *restp = get_method_res_type(mtp, 0);
    return new_Proj(new_Proj(call, mode_T, pn_Call_T_result), get_type_mode(restp), 0);
  }
  return NULL;
}

static void close_method(int n_ins, ir_node **ins) {
  ir_node *x =  new_Return(get_store(), n_ins, ins);
  mature_immBlock(get_cur_block());
  add_immBlock_pred(get_cur_end_block(), x);
  mature_immBlock(get_cur_end_block());
  irg_finalize_cons(current_ir_graph);
}


int
main(void)
{
  ir_graph *mainp;
  ir_graph *hs;
  ir_graph *ha;
  ir_graph *insert;
  ir_graph *remove;
  ir_graph *unheap;
  ir_graph *downh;
  ir_graph *exc;
  ir_graph *a, *b, *c, *d;
  ir_graph *self, *self1, *self2, *self3, *self4;
  ir_entity **free_methods;
  int arr_len;

  init_firm(NULL);

  set_opt_constant_folding(0);
  set_opt_cse(0);

  set_irp_prog_name(new_id_from_str("recursion"));

  /** The callgraph of the heapsort excample */
  mainp  = make_method("main", 0);
  hs     = make_method("hs", 0);
  ha     = make_method("ha", 0);
  insert = make_method("insert", 0);
  remove = make_method("remove", 0);
  unheap = make_method("unheap", 0);
  downh  = make_method("downh", 0);
  exc    = make_method("exc", 0);

  set_irp_main_irg(mainp);

  current_ir_graph = mainp;
  make_Call(hs, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = hs;
  make_Call(ha, 0, NULL);
  make_Call(remove, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = ha;
  make_Call(insert, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = insert;
  make_Call(unheap, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = remove;
  make_Call(unheap, 0, NULL);
  make_Call(downh, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = unheap;
  make_Call(exc, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = downh;
  make_Call(downh, 0, NULL);
  make_Call(exc, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = exc;
  close_method(0, NULL);


  /* A callgraph with a nested recursion. */
  a  = make_method("a", 0);
  b  = make_method("b", 0);
  c  = make_method("c", 0);
  d  = make_method("d", 0);

  current_ir_graph = a;
  make_Call(b, 0, NULL);
  make_Call(c, 0, NULL);
  make_Call(b, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = b;
  close_method(0, NULL);

  current_ir_graph = c;
  make_Call(b, 0, NULL);
  make_Call(d, 0, NULL);
  make_Call(a, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = d;
  make_Call(a, 0, NULL);
  make_Call(d, 0, NULL);
  close_method(0, NULL);

  /* A callgraph with a self recursion */
  self  = make_method("self", 0);

  current_ir_graph = self;
  make_Call(self, 0, NULL);
  close_method(0, NULL);

  /* A callgraph with a self recursion over several steps*/
  self1 = make_method("self1", 0);
  self2 = make_method("self2", 0);
  self3 = make_method("self3", 0);
  self4 = make_method("self4", 0);

  current_ir_graph = self1;
  make_Call(self2, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = self2;
  make_Call(self3, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = self3;
  make_Call(self4, 0, NULL);
  close_method(0, NULL);

  current_ir_graph = self4;
  make_Call(self1, 0, NULL);
  close_method(0, NULL);

  printf("Dumping Callgraph.\n");

  cgana(&arr_len, &free_methods);
  compute_callgraph();
  find_callgraph_recursions();
  /*dump_callgraph("");*/
  /* Order of edges depends on set.c, which is not deterministic. */
#ifdef INTERPROCEDURAL_VIEW
  cg_construct(arr_len, free_methods);
#endif

  printf("Use ycomp to view these graphs:\n");
  printf("ycomp GRAPHNAME\n\n");
  return 0;
}
