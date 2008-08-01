/*
 * Project:     libFIRM
 * File name:   testprograms/indentify_types.c
 * Purpose:     Shows use of ir_type identification
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

int main(void)
{
  ident *i1, *i2;
  ir_type  *t1, *t2, *t3;
  firm_parameter_t params;
  type_identify_if_t params2;

  printf("\nCreating ir_type information for IDENTIFY_TYPES ...\n");

  /** init library */
  memset (&params, 0, sizeof(params));
  params.size = sizeof(params);
  params2.cmp = compare_names;
  params2.hash = NULL;
  params.ti_if = &params2;
  init_firm(&params);


  i1 = new_id_from_str("type1");
  i2 = new_id_from_str("type2");

  t1 = new_type_class(i1);
  t1 = mature_type(t1);

  t1 = mature_type(t1);

  t2 = new_type_class(i1);
  t2 = mature_type(t2);

  t3 = new_type_class(i2);
  t3 = mature_type(t3);

  /*
  printf(" t1: "); DDMT(t1);
  printf(" t2: "); DDMT(t2);
  printf(" t3: "); DDMT(t3);
  */

  printf("Done building the graph.  Dumping it.\n");
  dump_all_types(0);

  printf("Use ycomp to view this graph:\n");
  printf("ycomp GRAPHNAME\n\n");

  return (0);
}
