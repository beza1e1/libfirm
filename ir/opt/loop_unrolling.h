/*
 * Project:     libFIRM
 * File name:   ir/opt/loop_unrolling.h
 * Purpose:     Loop unrolling.
 * Author:      Beyhan Veliev
 * Modified by:
 * Created:     16.11.2004
 * CVS-ID:      $Id$
 * Copyright:   (c) 2004 Universit�t Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

/**
 * @file loop_unrolling.h
 *
 * Loop unrolling.
 *
 * @author Beyhan Veliev
 */
#ifndef _LOOP_UNROLLING_H_
#define _LOOP_UNROLLING_H_

#include "irgraph.h"
/* Make a copy for a ir node.*/
void copy_irn(ir_node *irn, void *env);
/**
 * Do Loop unrolling in the given graph.
 */
void optimize_loop_unrolling(ir_graph *irg);
#endif  /* _LOOP_UNROLLING_H_ */
