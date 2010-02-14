/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
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
 * @brief    Loop datastructure and access functions -- private stuff.
 * @author   Goetz Lindenmaier
 * @date     7.2002
 * @version  $Id$
 */
#ifndef FIRM_ANA_IRLOOP_T_H
#define FIRM_ANA_IRLOOP_T_H

#include "firm_common.h"
#include "irgraph_t.h"
#include "irnode_t.h"
#include "irloop.h"

/**
 * Possible loop flags, can be or'ed.
 */
typedef enum loop_flags {
	loop_is_count_loop = 0x00000001,  /**< if set it's a counting loop */
	loop_downto_loop   = 0x00000002,  /**< if set, it's a downto loop, else an upto loop */
	loop_is_endless    = 0x00000004,  /**< if set, this is an endless loop */
	loop_is_dead       = 0x00000008,  /**< if set, it's a dead loop ie will never be entered */
	loop_wrap_around   = 0x00000010,  /**< this loop is NOT endless, because of wrap around */
	loop_end_false     = 0x00000020,  /**< this loop end can't be computed "from compute_loop_info.c" */
	do_loop            = 0x00000040,  /**< this is a do loop */
	once               = 0x00000080,  /**< this is a do loop, with a false condition. It iterate exactly once. */
	loop_outer_loop    = 0x00000100   /**< id set, this loop has child loops (is a no leaf). */
} loop_flags_t;

/**
 * The loops data structure.
 *
 * The loops data structure represents circles in the intermediate
 * representation.  It does not represent loops in the terms of a
 * source program.
 * Each ir_graph can contain one outermost loop data structure.
 * loop is the entry point to the nested loops.
 * The loop data structure contains a field indicating the depth of
 * the loop within the nesting.  Further it contains a list of the
 * loops with nesting depth -1.  Finally it contains a list of all
 * nodes in the loop.
 *
 * @todo We could add a field pointing from a node to the containing loop,
 * this would cost a lot of memory, though.
 */
struct ir_loop {
	firm_kind kind;                   /**< A type tag, set to k_ir_loop. */
	int depth;                        /**< Nesting depth */
	int n_sons;                       /**< Number of ir_nodes in array "children" */
	int n_nodes;                      /**< Number of loop_nodes in array "children" */
	unsigned flags;                   /**< a set of loop_flags_t */
	struct ir_loop *outer_loop;       /**< The outer loop */
	loop_element   *children;         /**< Mixed flexible array: Contains sons and loop_nodes */
	tarval  *loop_iter_start;         /**< counting loop: the start value */
	tarval  *loop_iter_end;           /**< counting loop: the last value reached */
	tarval  *loop_iter_increment;     /**< counting loop: the increment */
	ir_node *loop_iter_variable;      /**< The iteration variable of counting loop.*/

	void *link;                       /**< link field. */
#ifdef DEBUG_libfirm
	long loop_nr;                     /**< A unique node number for each loop node to make output
	                                       readable. */
#endif
};

/**
 * Allocates a new loop as son of father on the given obstack.
 * If father is equal NULL, a new root loop is created.
 */
ir_loop *alloc_loop(ir_loop *father, struct obstack *obst);

/** Add a son loop to a father loop. */
void add_loop_son(ir_loop *loop, ir_loop *son);

/** Add a node to a loop. */
void add_loop_node(ir_loop *loop, ir_node *n);

/** Add an IR graph to a loop. */
void add_loop_irg(ir_loop *loop, ir_graph *irg);

/** Sets the loop a node belonging to. */
void set_irn_loop(ir_node *n, ir_loop *loop);

/**
 * Mature all loops by removing the flexible arrays of a loop tree
 * and putting them on the given obstack.
 */
void mature_loops(ir_loop *loop, struct obstack *obst);

/* -------- inline functions -------- */

static inline int _is_ir_loop(const void *thing)
{
	return get_kind(thing) == k_ir_loop;
}

static inline void _set_irg_loop(ir_graph *irg, ir_loop *loop)
{
	assert(irg);
	irg->loop = loop;
}

static inline ir_loop *_get_irg_loop(const ir_graph *irg)
{
	assert(irg);
	return irg->loop;
}

static inline ir_loop *_get_loop_outer_loop(const ir_loop *loop)
{
	assert(_is_ir_loop(loop));
	return loop->outer_loop;
}

static inline int _get_loop_depth(const ir_loop *loop)
{
	assert(_is_ir_loop(loop));
	return loop->depth;
}

static inline int _get_loop_n_sons(const ir_loop *loop)
{
	assert(_is_ir_loop(loop));
	return loop->n_sons;
}

/* Uses temporary information to get the loop */
static inline ir_loop *_get_irn_loop(const ir_node *n)
{
	return n->loop;
}

#define is_ir_loop(thing)         _is_ir_loop(thing)
#define set_irg_loop(irg, loop)   _set_irg_loop(irg, loop)
#define get_irg_loop(irg)         _get_irg_loop(irg)
#define get_loop_outer_loop(loop) _get_loop_outer_loop(loop)
#define get_loop_depth(loop)      _get_loop_depth(loop)
#define get_loop_n_sons(loop)     _get_loop_n_sons(loop)
#define get_irn_loop(n)           _get_irn_loop(n)

#endif
