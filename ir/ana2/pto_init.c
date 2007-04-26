/* -*- c -*- */

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

/**
 * @file
 * @brief    Initialisation Functions
 * @author   Florian
 * @date     Sat Nov 13 19:35:27 CET 2004
 * @version  $Id$
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/*
 pto_init: Initialisation Functions
*/

# include <assert.h>
#ifdef HAVE_STRING_H
# include <string.h>
#endif
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif

# include "obst.h"
# include "pto.h"
# include "pto_init.h"
# include "pto_debug.h"
# include "pto_comp.h"
# include "pto_name.h"
# include "pto_util.h"

# include "typewalk.h"
# include "irgwalk.h"
# include "tv.h"
# include "xmalloc.h"

# include "gnu_ext.h"

/* Local Defines: */
# define obstack_chunk_alloc xmalloc
# define obstack_chunk_free  free

/* Local Data Types: */
typedef struct init_env_str
{
  int n_ctxs;
} init_env_t;

typedef struct reset_env_str
{
  int ctx_idx;
} reset_env_t;

/* Local Variables: */
extern struct obstack *qset_obst; /* from pto_name */

static struct obstack *pto_obst = NULL; /* all pto_t's go onto this one */

/* Local Prototypes: */

/* ===================================================
   Local Implementation:
   =================================================== */
/** Allocate a new pto */
static pto_t *new_pto (ir_node *node)
{
  pto_t *pto = obstack_alloc (pto_obst, sizeof (pto_t));
  pto->values = qset_new (N_INITIAL_OJBS, qset_obst);

  return (pto);
}

/** Allocate a new alloc_pto */
static alloc_pto_t *new_alloc_pto (ir_node *alloc, int n_ctxs)
{
  int i;
  alloc_pto_t *alloc_pto = obstack_alloc (pto_obst, sizeof (alloc_pto_t));
  ir_type *tp;

  assert (op_Alloc == get_irn_op(alloc));

  tp = get_Alloc_type (alloc);

  alloc_pto->ptos = (pto_t**) obstack_alloc (pto_obst, n_ctxs * sizeof (pto_t*));

  for (i = 0; i < n_ctxs; i ++) {
    desc_t *desc = new_name (tp, alloc, i);
    alloc_pto->ptos [i] = new_pto (alloc);
    qset_insert (alloc_pto->ptos [i]->values, desc);
  }

  return (alloc_pto);
}

/** Allocate a new pto for a symconst */
static pto_t* new_symconst_pto (ir_node *symconst)
{
  pto_t *pto;
  ir_entity *ent;
  desc_t *desc = NULL;

  assert (op_SymConst == get_irn_op(symconst));

  pto = new_pto (symconst);
  ent = get_SymConst_entity (symconst);

  /*
  const char *ent_name = (char*) get_entity_name (ent);
  const char *own_name = (char*) get_type_name (get_entity_owner (ent));
  HERE3 ("start", own_name, ent_name);
  */
  /* Ok, so if the symconst has a pointer-to-mumble, it's some address
     calculation, but if it's the mumble itself, it's just the same,
     except it's presumably a constant of mumble. In any case, we need to
     branch on this.  "How's that for object fucking oriented? --jwz" */
  if (is_Pointer_type (get_entity_type (ent))) {
    desc = new_ent_name (ent);
  } else if (is_Class_type (get_entity_type (ent))) {
    desc = new_name (get_entity_type (ent), symconst, -1);
  } else {
    fprintf (stderr, "%s: not handled: %s[%li] (\"%s\")\n",
             __FUNCTION__,
             get_op_name (get_irn_op (symconst)),
             get_irn_node_nr (symconst),
             get_entity_name (ent));
    assert (0 && "something not handled");
  }

  qset_insert (pto->values, desc);

  /* HERE3 ("end", own_name, ent_name); */

  return (pto);
}

/* Helper to pto_init --- clear the link fields of class types */
static void clear_type_link (type_or_ent *thing, void *_unused)
{
  if (is_type (thing)) {
    ir_type *tp = (ir_type*) thing;

    if (is_Class_type (tp)) {
      DBGPRINT (1, (stdout, "%s (\"%s\")\n",
                    __FUNCTION__,
                    get_type_name (tp)));

      set_type_link (tp, NULL);
    }
  } else if (is_entity (thing)) {
    ir_entity *ent = (ir_entity*) thing;

    DBGPRINT (1, (stdout, "%s (\"%s\")\n",
                  __FUNCTION__,
                  get_entity_name (ent)));

    set_entity_link (ent, NULL);
  }
}

/** Helper to pto_init_graph --- clear the links of the given node */
static void clear_node_link (ir_node *node, void *_unused)
{
  set_irn_link (node, NULL);
}

/** Helper to pto_init_graph --- clear the links of all nodes */
static void clear_graph_links (ir_graph *graph)
{
  irg_walk_graph (graph, clear_node_link, NULL, NULL);
}

/** Reset ALL the pto values for a new pass */
static void reset_node_pto (ir_node *node, void *env)
{
  reset_env_t *reset_env = (reset_env_t*) env;
  int ctx_idx = reset_env->ctx_idx;
  ir_opcode op = get_irn_opcode (node);

  /* HERE ("start"); */

  switch (op) {
  case (iro_Load):
  case (iro_Call):
  case (iro_Block):             /* END BLOCK only */
  case (iro_Phi): {
    /* allocate 'empty' pto values */
    pto_t *pto = new_pto (node);
    set_node_pto (node, pto);
  } break;

  case (iro_Alloc): {
    /* set alloc to 'right' current pto */
    alloc_pto_t *alloc_pto = (alloc_pto_t*) get_irn_link (node);
    alloc_pto->curr_pto = alloc_pto->ptos [ctx_idx];

    DBGPRINT (1, (stdout, "%s: setting pto of \"%s[%li]\" for ctx %i\n",
                  __FUNCTION__,
                  OPNAME (node),
                  OPNUM (node),
                  ctx_idx));

    assert (alloc_pto->curr_pto);
  } break;
  case (iro_Const):
  case (iro_SymConst): {
      /* nothing, leave as-is */
    } break;

  default: {
    /* basically, nothing */
    DBGPRINT (2, (stdout, "%s: resetting pto of \"%s[%li]\"\n",
                  __FUNCTION__,
                  OPNAME (node),
                  OPNUM (node)));
    set_node_pto (node, NULL);
  } break;
  }

  /* HERE ("end"); */
}

/** Initialise primary name sources */
static void init_pto (ir_node *node, void *env)
{
  init_env_t *init_env = (init_env_t*) env;
  int n_ctxs = init_env->n_ctxs;

  ir_opcode op = get_irn_opcode (node);

  switch (op) {
  case (iro_SymConst): {
    if (mode_is_reference (get_irn_mode (node))) {
      ir_entity *ent = get_SymConst_entity (node);
      ir_type *tp = get_entity_type (ent);
      if (is_Class_type (tp) || is_Pointer_type (tp)) {
        pto_t *symconst_pto = new_symconst_pto (node);
        set_node_pto (node, symconst_pto);

        /* debugging only */
        DBGPRINT (1, (stdout, "%s: new name \"%s\" for \"%s[%li]\"\n",
                      __FUNCTION__,
                      get_entity_name (ent),
                      OPNAME (node),
                      OPNUM (node)));
      }
    }
  } break;

  case (iro_Alloc): {
    alloc_pto_t *alloc_pto = new_alloc_pto (node, n_ctxs);
    ir_type *tp;

    set_alloc_pto (node, alloc_pto);

    tp = get_Alloc_type (node); /* debugging only */
    DBGPRINT (1, (stdout, "%s: %i names \"%s\" for \"%s[%li]\"\n",
                  __FUNCTION__,
                  n_ctxs,
                  get_type_name (tp),
                  OPNAME (node),
                  OPNUM (node)));
  } break;

  case (iro_Const): {
    tarval *tv = get_Const_tarval (node);

    /* only need 'NULL' pointer constants */
    if (mode_P == get_tarval_mode (tv)) {
      if (get_tarval_null (mode_P) == tv) {
        pto_t *pto = new_pto (node);
        set_node_pto (node, pto);
      }
    }
  } break;
  case (iro_Load):
  case (iro_Call):
  case (iro_Phi):
    /* nothing --- handled by reset_node_pto on each pass */
    break;
  default: {
    /* nothing */
  } break;
  }
}


/** Initialise the given graph for a new pass run */
static void pto_init_graph_allocs (ir_graph *graph)
{
  graph_info_t *ginfo = ecg_get_info (graph);
  init_env_t *init_env;

  init_env = xmalloc (sizeof (init_env_t));
  init_env->n_ctxs = ginfo->n_ctxs;

  /* HERE ("start"); */

  irg_walk_graph (graph, init_pto, NULL, init_env);

  /* HERE ("end"); */
  memset (init_env, 0x00, sizeof (init_env_t));
  free (init_env);
}

/* ===================================================
   Exported Implementation:
   =================================================== */
/* "Fake" the arguments to the main method */
void fake_main_args (ir_graph *graph)
{
  /* HERE ("start"); */

  ir_entity *ent = get_irg_entity (graph);
  ir_type *mtp = get_entity_type (ent);
  ir_node **args = find_irg_args (graph);
  ir_type *ctp = get_method_param_type (mtp, 1); /* ctp == char[]*[]* */
  desc_t *arg_desc;
  pto_t *arg_pto;

  /* 'main' has signature 'void(int, char[]*[]*)' */
  assert (NULL == args [2]);

  assert (is_Pointer_type (ctp));

  ctp = get_pointer_points_to_type (ctp); /* ctp == char[]*[] */

  assert (is_Array_type (ctp));

  arg_desc = new_name (ctp, args [1], -1);
  arg_pto = new_pto (args [1]);
  /* todo: simulate 'store' to arg1[] ?!? */
  qset_insert (arg_pto->values, arg_desc);

  set_node_pto (args [1], arg_pto);

  DBGPRINT (1, (stdout, "%s:%i (%s[%li])\n",
                __FUNCTION__, __LINE__,
                OPNAME (args [1]), OPNUM (args [1])));

# ifdef TEST_MAIN_TYPE
  ctp = get_array_element_type (ctp); /* ctp == char[]* */

  assert (is_Pointer_type (ctp));

  ctp = get_pointer_points_to_type (ctp); /* ctp == char[] */

  assert (is_Array_type (ctp));

  ctp = get_array_element_type (ctp); /* ctp == char */

  assert (is_primitive_type (ctp));
# endif /* defined  TEST_MAIN_TYPE */

  /* HERE ("end"); */
}

/* Initialise the Init module */
void pto_init_init (void)
{
  pto_obst = (struct obstack*) xmalloc (sizeof (struct obstack));

  obstack_init (pto_obst);
}

/* Cleanup the Init module */
void pto_init_cleanup (void)
{
  obstack_free (pto_obst, NULL);
  memset (pto_obst, 0x00, sizeof (struct obstack));
  free (pto_obst);
  pto_obst = NULL;
}


/* Initialise the Names of the Types/Entities */
void pto_init_type_names (void)
{
  /* HERE ("start"); */
  type_walk (clear_type_link, NULL, NULL);
  /* HERE ("end"); */
}

/* Initialise the given graph for a new pass run */
void pto_init_graph (ir_graph *graph)
{
  ir_node **proj_args;
  graph_info_t *ginfo = ecg_get_info (graph);
  const int n_ctxs = ginfo->n_ctxs;

  /* only for debugging stuff: */
  ir_entity *ent = get_irg_entity (graph);
  const char *ent_name = (char*) get_entity_name (ent);
  const char *own_name = (char*) get_type_name (get_entity_owner (ent));

  DBGPRINT (2, (stdout, "%s: init \"%s.%s\" for %i ctxs\n",
                __FUNCTION__,
                own_name, ent_name, n_ctxs));

  /* HERE ("start"); */

  clear_graph_links     (graph);
  pto_init_graph_allocs (graph);

  /* HERE ("end"); */

  assert (NULL == get_irg_proj_args (graph));
  proj_args = find_irg_args (graph);
  set_irg_proj_args (graph, proj_args);
  assert (proj_args == get_irg_proj_args (graph));
}

/* Reset the given graph for a new pass run */
void pto_reset_graph_pto (ir_graph *graph, int ctx_idx)
{
  reset_env_t *reset_env;

  reset_env = (reset_env_t*) xmalloc (sizeof (reset_env_t));
  reset_env->ctx_idx = ctx_idx;

  /* HERE ("start"); */

  irg_walk_graph (graph, reset_node_pto, NULL, reset_env);

  /* HERE ("end"); */
  memset (reset_env, 0x00, sizeof (reset_env_t));
  free (reset_env);
}


/*
  $Log$
  Revision 1.23  2007/01/16 15:45:42  beck
  renamed type opcode to ir_opcode

  Revision 1.22  2006/12/13 19:46:47  beck
  rename type entity into ir_entity

  Revision 1.21  2006/06/08 10:49:07  beck
  renamed type to ir_type

  Revision 1.20  2005/12/05 12:19:54  beck
  added missing include <assert.h> (not anymore included in libFirm)

  Revision 1.19  2005/06/17 17:42:32  beck
  added doxygen docu
  fixed (void) function headers

  Revision 1.18  2005/02/16 13:27:52  beck
  added needed tv.h include

  Revision 1.17  2005/01/14 14:12:51  liekweg
  prepare gnu extension fix

  Revision 1.16  2005/01/14 13:36:50  liekweg
  don't put environments on the stack; handle consts

  Revision 1.15  2005/01/10 17:26:34  liekweg
  fixup printfs, don't put environments on the stack

  Revision 1.14  2005/01/05 14:25:54  beck
  renames all is_x*_type() functions to is_X*_type() to prevent name clash with EDG fronten

  Revision 1.13  2004/12/21 15:07:55  beck
  removed C99 contructs
  removed unnecessary allocation
  removed use of mode_P, use mode_is_reference() instead
  removed handling of Const with pointer tarvals, these constructs are removed

  Revision 1.12  2004/12/20 17:41:14  liekweg
  __unused -> _unused

  Revision 1.11  2004/12/20 17:34:35  liekweg
  fix recursion handling

  Revision 1.10  2004/12/15 13:31:00  liekweg
  store ctx idx in names

  Revision 1.9  2004/12/15 09:18:18  liekweg
  pto_name.c

  Revision 1.8  2004/12/02 16:17:51  beck
  fixed config.h include

  Revision 1.7  2004/11/30 14:47:54  liekweg
  fix initialisation; do correct iteration

  Revision 1.6  2004/11/26 16:00:41  liekweg
  recognize class consts vs. ptr-to-class consts

  Revision 1.5  2004/11/24 14:53:56  liekweg
  Bugfixes

  Revision 1.4  2004/11/20 21:21:56  liekweg
  Finalise initialisation

  Revision 1.3  2004/11/18 16:37:07  liekweg
  rewrite


*/
