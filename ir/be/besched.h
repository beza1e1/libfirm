/*
 * Copyright (C) 1995-2010 University of Karlsruhe.  All right reserved.
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
 * @brief       data structures for scheduling nodes in basic blocks.
 *              (This file does not contain the scheduling algorithms)
 * @author      Sebastian Hack, Matthias Braun
 * @version     $Id$
 */
#ifndef FIRM_BE_BESCHED_H
#define FIRM_BE_BESCHED_H

#include <stdio.h>
#include <stdbool.h>

#include "irgraph.h"
#include "irnode.h"
#include "beirg.h"
#include "beinfo.h"
#include "beutil.h"

static sched_info_t *get_irn_sched_info(const ir_node *node)
{
	return &be_get_info(skip_Proj_const(node))->sched_info;
}

/**
 * Check, if the node is scheduled.
 * @param irn The node.
 * @return 1, if the node is scheduled, 0 if not.
 */
static inline bool sched_is_scheduled(const ir_node *irn)
{
	return get_irn_sched_info(irn)->next != NULL;
}

/**
 * Returns the time step of a node. Each node in a block has a timestep
 * unique to that block. A node schedule before another node has a lower
 * timestep than this node.
 * @param irn The node.
 * @return The time step in the schedule.
 */
static inline sched_timestep_t sched_get_time_step(const ir_node *irn)
{
	assert(sched_is_scheduled(irn));
	return get_irn_sched_info(irn)->time_step;
}

static inline bool sched_is_end(const ir_node *node)
{
	return is_Block(node);
}

static inline bool sched_is_begin(const ir_node *node)
{
	return is_Block(node);
}

/**
 * Check, if an ir_node has a scheduling successor.
 * @param irn The ir node.
 * @return 1, if the node has a scheduling successor, 0 if not.
 */
static inline bool sched_has_next(const ir_node *irn)
{
	const sched_info_t *info  = get_irn_sched_info(irn);
	const ir_node      *block = is_Block(irn) ? irn : get_nodes_block(irn);
	return info->next != block;
}

/**
 * Check, if an ir_node has a scheduling predecessor.
 * @param irn The ir node.
 * @return 1, if the node has a scheduling predecessor, 0 if not.
 */
static inline bool sched_has_prev(const ir_node *irn)
{
	const sched_info_t *info  = get_irn_sched_info(irn);
	const ir_node      *block = is_Block(irn) ? irn : get_nodes_block(irn);
	return info->prev != block;
}

/**
 * Get the scheduling successor of a node.
 * @param irn The node.
 * @return The next ir node in the schedule or the block, if the node has no next node.
 */
static inline ir_node *sched_next(const ir_node *irn)
{
	const sched_info_t *info = get_irn_sched_info(irn);
	return info->next;
}

/**
 * Get the scheduling predecessor of a node.
 * @param irn The node.
 * @return The next ir node in the schedule or the block, if the node has no predecessor.
 * predecessor.
 */
static inline ir_node *sched_prev(const ir_node *irn)
{
	const sched_info_t *info = get_irn_sched_info(irn);
	return info->prev;
}

/**
 * Get the first node in a block schedule.
 * @param block The block of which to get the schedule.
 * @return The first node in the schedule or the block itself
 *         if there is no node in the schedule.
 */
static inline ir_node *sched_first(const ir_node *block)
{
	assert(is_Block(block) && "Need a block here");
	return sched_next(block);
}

/**
 * Get the last node in a schedule.
 * @param  block The block to get the schedule for.
 * @return The last ir node in a schedule, or the block itself
 *         if there is no node in the schedule.
 */
static inline ir_node *sched_last(const ir_node *block)
{
	assert(is_Block(block) && "Need a block here");
	return sched_prev(block);
}

/**
 * Add a node to a block schedule.
 * @param block The block to whose schedule the node shall be added to.
 * @param irn The node to add.
 * @return The given node.
 */
void sched_add_before(ir_node *before, ir_node *irn);


/**
 * Add a node to a block schedule.
 * @param block The block to whose schedule the node shall be added to.
 * @param irn The node to add.
 * @return The given node.
 */
void sched_add_after(ir_node *after, ir_node *irn);

static inline void sched_init_block(ir_node *block)
{
	sched_info_t *info = get_irn_sched_info(block);
	assert(info->next == NULL && info->time_step == 0);
	info->next = block;
	info->prev = block;
}

static inline void sched_reset(ir_node *node)
{
	sched_info_t *info = get_irn_sched_info(node);
	info->next = NULL;
	info->prev = NULL;
}

/**
 * Remove a node from the scheduled.
 * @param irn The node.
 */
void sched_remove(ir_node *irn);

/**
 * Checks, if one node is scheduled before another.
 * @param n1   A node.
 * @param n2   Another node.
 * @return     true, if n1 is in front of n2 in the schedule, false else.
 * @note       Both nodes must be in the same block.
 */
static inline bool sched_comes_after(const ir_node *n1, const ir_node *n2)
{
	assert(sched_is_scheduled(n1));
	assert(sched_is_scheduled(n2));
	assert((is_Block(n1) ? n1 : get_nodes_block(n1)) == (is_Block(n2) ? n2 : get_nodes_block(n2)));
	return sched_get_time_step(n1) < sched_get_time_step(n2);
}

#define sched_foreach_from(from, irn) \
  for(irn = from; !sched_is_end(irn); irn = sched_next(irn))

#define sched_foreach_reverse_from(from, irn) \
  for(irn = from; !sched_is_begin(irn); irn = sched_prev(irn))

/**
 * A shorthand macro for iterating over a schedule.
 * @param block The block.
 * @param irn A ir node pointer used as an iterator.
 */
#define sched_foreach(block,irn) \
	sched_foreach_from(sched_first(block), irn)

/**
 * A shorthand macro for reversely iterating over a schedule.
 * @param block The block.
 * @param irn A ir node pointer used as an iterator.
 */
#define sched_foreach_reverse(block,irn) \
  sched_foreach_reverse_from(sched_last(block), irn)

/**
 * A shorthand macro for iterating over all Phi nodes of a schedule.
 * @param block The block.
 * @param phi A ir node pointer used as an iterator.
 */
#define sched_foreach_Phi(block,phi) \
	for (phi = sched_first(block); is_Phi(phi); phi = sched_next(phi))

/**
 * Type for a function scheduling a graph
 */
typedef void (*schedule_func) (ir_graph *irg);

/**
 * Register new scheduling algorithm
 */
void be_register_scheduler(const char *name, schedule_func func);

/**
 * schedule a graph with the currenty selected scheduler.
 */
void be_schedule_graph(ir_graph *irg);

#endif
