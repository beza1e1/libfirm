/*
 * Copyright (C) 1995-2007 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/*
 * Project:     libFIRM
 * File name:   ir/st/st.h
 * Purpose:     Provide some auxilliary structures for firm graphs.
 * Author:      Florian Liekweg
 * Modified by:
 * Created:     26.2.2002
 * CVS-ID:      $Id$
 * Copyright:   (c) 2002-2003 Universit�t Karlsruhe
 */
/**
   @file st.h

   Provide some auxilliary structures for firm graphs.

   @author Florian Liekweg

   @note
     not quite complete
*/

# ifndef _ST_H_
# define _ST_H_

/* Includes:  */
#include "irgraph.h"
#include "irnode.h"

#include "bs.h"

#include <stdbool.h>


/* Data Types: */

/**  One dominator tree */
typedef struct
{
  int n_blocks;
  ir_graph *graph;	/**< PRE */
  ir_node **blocks;
  ir_node **idoms;	/**< idom [n] == immediate dominator of blocks [n] */
  bs_t *masks;
}
dt_t;

/** List entry.  */
typedef struct dtree_t
{
  dt_t *tree;
  ir_graph *graph;

  struct dtree_t *next;
}
dtree_t;

/** dominator environment for a node dom_env_t::a in graph dom_env_t::graph */
typedef struct dom_env_t
{
  dt_t     *dt;
  ir_graph *graph;
  ir_node  *a;
  int       index_a;
  bs_t      mask;
} dom_env_t;

/* Forwards for Globals:  */
extern dtree_t *trees;
extern dtree_t *last;

/* Prototypes: */
void     st_build_dominator_tree (ir_graph*);
bool     dominates            (ir_graph*, ir_node*, ir_node*);
ir_node *get_idom             (ir_graph*, ir_node*);

dom_env_t *get_dom_env (ir_graph*, ir_node*);
void delete_dom_env (dom_env_t*);
bool dominates_l (dom_env_t*, ir_node*);

# endif /* defined _ST_H_ */
