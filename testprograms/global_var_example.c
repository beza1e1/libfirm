/*
 * Project:     libFIRM
 * File name:   testprograms/global_var_example.c
 * Purpose:     Illustrates representation of global variable.
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

/*
 * das leere FIRM Programm
 */

/**
*  This program shows how to build ir for global variables.
*  It constructs the ir for the following pseudo-program:
*
*  int i;
*
*  main() {
*    i = 2;
*    return;
*  }
**/

int main(void)
{
  char *dump_file_suffix = "";
  ir_graph *irg;        /* this variable contains the irgraph */
  ir_type     *owner;      /* the class in which this method is defined */
  ir_type     *proc_main;  /* ir_type information for the method main */
  ir_type     *prim_t_int; /* describes int ir_type defined by the language */
  ir_entity   *main_ent;   /* represents this method as ir_entity of owner */
  ir_entity   *i_ent;      /* the ir_entity representing the global variable i */
  union symconst_symbol symbol;
  ir_node  *x, *i_ptr, *store;

  printf("\nCreating an IR graph: GLOBAL_VAR ...\n");

  /* init library */
  init_firm (NULL);

  /* make basic ir_type information for primitive ir_type int.
     In Sather primitive types are represented by a class.
     This is the modeling appropriate for other languages.
     Mode_i says that all integers shall be implemented as a
     32 bit integer value.  */
  prim_t_int = new_type_primitive(new_id_from_chars ("int", 3), mode_Is);

  /* FIRM was designed for oo languages where all methods belong to a class.
   * For imperative languages like C we view a file or compilation unit as
   * a large class containing all functions as methods in this file.
   * This class is automatically generated and can be obtained by get_glob_type().
   */
#define METHODNAME "GLOBAL_VAR_main"
#define NRARGS 0
#define NRES 0

  /* Main is an ir_entity of this global class. */
  owner = get_glob_type();
  proc_main = new_type_method(new_id_from_chars(METHODNAME, strlen(METHODNAME)),
                              NRARGS, NRES);
  main_ent = new_entity (owner,
			 new_id_from_chars (METHODNAME, strlen(METHODNAME)),
			 proc_main);

  /* Generates the basic graph for the method represented by ir_entity main_ent, that
   * is, generates start and end blocks and nodes and a first, initial block.
   * The constructor needs to know how many local variables the method has.
   */
#define NUM_OF_LOCAL_VARS 0

  /* Generate the entities for the global variables. */
  i_ent = new_entity (get_glob_type(),
		      new_id_from_chars ("i", strlen("i")),
		      prim_t_int);

  irg = new_ir_graph (main_ent, NUM_OF_LOCAL_VARS);

  /* The constructor new_ir_graph() generated a region to place nodes in.
   * This region is accessible via the attribut current_block of irg and
   * it is not matured.
   * Generate the assignment to i and the return node into this region.
   * The Return node is needed to return at least the store. */
  symbol.entity_p = i_ent;
  i_ptr = new_SymConst(mode_P, symbol, symconst_addr_ent);

  store = new_Store (get_store(), i_ptr,
		     new_Const(mode_Is, new_tarval_from_long (2, mode_Is)));
  set_store(new_Proj(store, mode_M, pn_Store_M));

  x = new_Return (get_store(), 0, NULL);

  /* Now generate all instructions for this block and all its predecessor blocks
   * so we can mature it. */
  mature_immBlock (get_irg_current_block(irg));

  /* This adds the in edge of the end block which originates at the return statement.
   * The return node passes controlflow to the end block.  */
  add_immBlock_pred (get_irg_end_block(irg), x);
  /* Now we can mature the end block as all it's predecessors are known. */
  mature_immBlock (get_irg_end_block(irg));

  irg_finalize_cons (irg);

  printf("Optimizing ...\n");
  dead_node_elimination(irg);

  /* verify the graph */
  irg_vrfy(irg);

  printf("Done building the graph.  Dumping it.\n");
  dump_ir_block_graph (irg, dump_file_suffix);
  dump_ir_graph_w_types (irg, dump_file_suffix);
  printf("Use ycomp to view this graph:\n");
  printf("ycomp GRAPHNAME\n\n");

  return (0);
}
