/* -*- c -*- */

/*
 * Copyrigth (C) 1995-2007 University of Karlsruhe.  All right reserved.
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

/**
 * @file
 * @brief    walk along memory edges
 * @author   Florian
 * @date     Mon 18 Oct 2004
 * @version  $Id$
 * @summary
 *   Walk over a firm graph along its memory edges.
 *
 *   Any number of graphs can be visited at the same time, but no graph
 *   can be traversed more than once at any time.
 */
# ifdef HAVE_CONFIG_H
#  include "config.h"
# endif

# include "irnode_t.h"
# include "irgwalk.h"           /* for irg_walk_func */
# include "irprog.h"            /* for get_irp_main_irg */
# include "xmalloc.h"
# include "gnu_ext.h"

# ifndef TRUE
#  define TRUE 1
#  define FALSE 0
# endif /* not defined TRUE */

/*
   Data
*/

/** environment for a single memory walker */
typedef struct walk_mem_env_str {
  ir_graph *graph;              /**< the graph we're visiting */
  unsigned long visited;        /**< 'visited' marker
                                 (unsigned long in case we walk more than 2^32 graphs) */
  irg_walk_func *pre;           /**< pre action */
  irg_walk_func *post;          /**< post action */
  void *env;                    /**< user-defined environment */

  struct walk_mem_env_str *prev; /**< link up walking instances */
  /* what else? */
} walk_mem_env_t;

/*
  Globals
*/

/* Link up walking instances */
static walk_mem_env_t *walk_envs = NULL;

/*
  Walk over the firm nodes of a graph via the memory edges (only)
  starting from a node that has a memory input.
*/
static void irg_walk_mem_node (ir_node *node,
                               walk_mem_env_t *walk_env)
{
  const ir_opcode op = get_irn_opcode (node);
  ir_node *in = NULL;

  if (get_irn_visited (node) >= walk_env->visited) {
    return;
  } else {
    set_irn_visited (node, walk_env->visited);
  }

  if (op_NoMem == get_irn_op (node)) {
    /* We don't want to see it it if it's not memory */
    return;
  }

  if (iro_Proj == op) {
    /* We don't want to see proj nodes at all --- skip over them */
    in = get_Proj_pred (node);

    irg_walk_mem_node (in, walk_env);

    return;
  }

  /* execute the 'pre' function */
  if (NULL != walk_env->pre) {
    walk_env->pre (node, walk_env->env);
  }

  switch (op) {
  case (iro_Start): {
  } break;
  case (iro_Load): {
    in = get_Load_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Store): {
    in = get_Store_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Alloc): {
    in = get_Alloc_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Free): {
    in = get_Free_mem (node);
    /* WTF? */
    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Raise): {
    in = get_Raise_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Sel): {
    in = get_Sel_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Call): {
    in = get_Call_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Return): {
    in = get_Return_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Phi): {
    int i;
    int n_ins = get_irn_arity (node);

    for (i = 0; i < n_ins; i ++) {
      in = get_irn_n (node, i);

      irg_walk_mem_node (in, walk_env);
    }
  } break;
  case (iro_Div): {
    in = get_Div_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Quot): {
    in = get_Quot_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Mod): {
    in = get_Mod_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_DivMod): {
    in = get_DivMod_mem (node);

    irg_walk_mem_node (in, walk_env);
  } break;
  case (iro_Block): {
    /* End Block ONLY */
    int i;
    int n_ins = get_irn_arity (node);

    for (i = 0; i < n_ins; i ++) {
      ir_node *ret = get_irn_n (node, i);

      irg_walk_mem_node (ret, walk_env);
    }
  } break;
  default: {
    fprintf (stderr, "%s: not handled: node[%li].op = %s\n",
             __FUNCTION__,
             get_irn_node_nr (node),
             get_op_name (get_irn_op (node)));

    assert (0 && "something not handled");
  }
  }

  /* execute the 'post' function */
  if (NULL != walk_env->post) {
    walk_env->post (node, walk_env->env);
  }
}

/*
   See whether the given graph is being visited right now.
   We can't be visiting a graph multiple times.
*/
int get_irg_is_mem_visited (ir_graph *graph)
{
  walk_mem_env_t *walk_env = walk_envs;

  while (NULL != walk_env) {
    if (graph == walk_env->graph) {
      return (TRUE);
    }

    walk_env = walk_env->prev;
  }

  return (FALSE);
}

/*
  Walk over the nodes of the given graph via the memory edges (only).
  Each graph can only be subject to this walk once at any given time.
*/
void irg_walk_mem (ir_graph *graph,
                   irg_walk_func *pre, irg_walk_func *post,
                   void *env)
{
  ir_node *end_block = get_irg_end_block (graph);
  walk_mem_env_t *walk_env = xmalloc (sizeof (walk_mem_env_t));

  assert (! get_irg_is_mem_visited (graph));

  walk_env->graph = graph;
  inc_irg_visited (walk_env->graph);
  walk_env->visited = get_irg_visited (graph);

  walk_env->prev = walk_envs;
  walk_envs = walk_env;

  walk_env->pre = pre;
  walk_env->post = post;
  walk_env->env  = env;

  /* 'graph' is not actually being visited right now, so make sure it is reported that way */
  assert (get_irg_is_mem_visited (graph));

  /*
    The ins of the end BLOCK are either 'return's (regular exits) or
    'ProjX'/'Raise's (exception exits).  We only walk over the
    'return' nodes, assuming that all memory-changing nodes are found
    from there on.
  */
  irg_walk_mem_node (end_block, walk_env);
  /*
    The end NODE sometimes has some more ins. not sure whether we need to walk them.
  */

  /* allow only properly nested calls right now */
  assert (walk_envs == walk_env);
  walk_envs = walk_envs->prev;

  free (walk_env);

  assert (! get_irg_is_mem_visited (graph));
}



/*
  $Log$
  Revision 1.12  2007/01/16 15:45:42  beck
  renamed type opcode to ir_opcode

  Revision 1.11  2005/01/26 12:20:20  beck
  gnu_ext.h included

  Revision 1.10  2005/01/14 13:34:48  liekweg
  Don't cast malloc

  Revision 1.9  2005/01/10 17:26:34  liekweg
  fixup printfs, don't put environments on the stack

  Revision 1.8  2004/12/22 14:43:14  beck
  made allocations C-like

  Revision 1.7  2004/12/21 14:25:35  beck
  removed C99 constructs
  make visit counter of same type as irn visit counter

  Revision 1.6  2004/12/02 16:17:51  beck
  fixed config.h include

  Revision 1.5  2004/11/19 10:35:20  liekweg
  also test for NoMem

  Revision 1.4  2004/11/18 16:35:11  liekweg
  Do not touch Proj nodes at all

  Revision 1.3  2004/11/04 14:57:12  liekweg
  fixed end block handling

  Revision 1.2  2004/10/22 14:41:12  liekweg
  execute 'pre' for a change.  Also, add CVS log


*/
