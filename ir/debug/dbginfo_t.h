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
 * @brief      Implements the Firm interface to debug information -- private header.
 * @author     Goetz Lindenmaier
 * @date       2001
 * @version    $Id$
 * @brief
 *  dbginfo: This is a empty implementation of the Firm interface to
 *  debugging support.  It only guarantees that the Firm library compiles
 *  and runs without any real debugging support.
 */
#ifndef FIRM_DEBUG_DBGINFO_T_H
#define FIRM_DEBUG_DBGINFO_T_H

#include <stdlib.h>
#include "dbginfo.h"

/**
 * The default merge_pair_func implementation, simply copies the debug info
 * from the old Firm node to the new one if the new one does not have debug info yet.
 *
 * @param nw    The new Firm node.
 * @param old   The old Firm node.
 * @param info  The action that cause old node to be replaced by new one.
 */
void default_dbg_info_merge_pair(ir_node *nw, ir_node *old, dbg_action info);

/**
 * The default merge_sets_func implementation.  If n_old_nodes is equal 1,
 * copies the debug info from the old node to all new ones (if they do not have
 * one), else does nothing.
 *
 * @param new_nodes     An array of new Firm nodes.
 * @param n_new_nodes   The length of the new_nodes array.
 * @param old_nodes     An array of old (replaced) Firm nodes.
 * @param n_old_nodes   The length of the old_nodes array.
 * @param info          The action that cause old node to be replaced by new one.
 */
void default_dbg_info_merge_sets(ir_node **new_nodes, int n_new_nodes,
                            ir_node **old_nodes, int n_old_nodes,
                            dbg_action info);

/**
 * The current merge_pair_func(), access only from inside firm.
 */
extern merge_pair_func *__dbg_info_merge_pair;

/**
 * The current merge_sets_func(), access only from inside firm.
 */
extern merge_sets_func *__dbg_info_merge_sets;

void ir_dbg_info_snprint(char *buf, size_t buf_size, const dbg_info *dbg);

#endif
